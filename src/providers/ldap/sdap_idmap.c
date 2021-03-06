/*
    SSSD

    Authors:
        Stephen Gallagher <sgallagh@redhat.com>

    Copyright (C) 2012 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "util/util.h"
#include "util/dlinklist.h"
#include "util/murmurhash3.h"
#include "providers/ldap/sdap_idmap.h"

static void *
sdap_idmap_talloc(size_t size, void *pvt)
{
    return talloc_size(pvt, size);
}

static void
sdap_idmap_talloc_free(void *ptr, void *pvt)
{
    talloc_free(ptr);
}

errno_t
sdap_idmap_init(TALLOC_CTX *mem_ctx,
                struct sdap_id_ctx *id_ctx,
                struct sdap_idmap_ctx **_idmap_ctx)
{
    errno_t ret;
    TALLOC_CTX *tmp_ctx;
    enum idmap_error_code err;
    size_t i;
    struct ldb_result *res;
    const char *dom_name;
    const char *sid_str;
    id_t slice_num;
    id_t idmap_lower;
    id_t idmap_upper;
    id_t rangesize;
    bool autorid_mode;
    struct sdap_idmap_ctx *idmap_ctx = NULL;
    struct sysdb_ctx *sysdb = id_ctx->be->domain->sysdb;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) return ENOMEM;

    idmap_ctx = talloc_zero(tmp_ctx, struct sdap_idmap_ctx);
    if (!idmap_ctx) {
        ret = ENOMEM;
        goto done;
    }
    idmap_ctx->id_ctx = id_ctx;

    idmap_lower = dp_opt_get_int(idmap_ctx->id_ctx->opts->basic,
                                 SDAP_IDMAP_LOWER);
    idmap_upper = dp_opt_get_int(idmap_ctx->id_ctx->opts->basic,
                                 SDAP_IDMAP_UPPER);
    rangesize = dp_opt_get_int(idmap_ctx->id_ctx->opts->basic,
                               SDAP_IDMAP_RANGESIZE);
    autorid_mode = dp_opt_get_bool(idmap_ctx->id_ctx->opts->basic,
                                   SDAP_IDMAP_AUTORID_COMPAT);

    /* Validate that the values make sense */
    if (rangesize <= 0
            || idmap_upper <= idmap_lower
            || (idmap_upper-idmap_lower) < rangesize)
    {
        DEBUG(SSSDBG_FATAL_FAILURE,
              ("Invalid settings for range selection: [%d][%d][%d]\n",
               idmap_lower, idmap_upper, rangesize));
        ret = EINVAL;
        goto done;
    }

    if (((idmap_upper - idmap_lower) % rangesize) != 0) {
        DEBUG(SSSDBG_CONF_SETTINGS,
              ("Range size does not divide evenly. Uppermost range will "
               "not be used\n"));
    }

    /* Initialize the map */
    err = sss_idmap_init(sdap_idmap_talloc, idmap_ctx,
                         sdap_idmap_talloc_free,
                         &idmap_ctx->map);
    if (err != IDMAP_SUCCESS) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("Could not initialize the ID map: [%s]\n",
               idmap_error_string(err)));
        if (err == IDMAP_OUT_OF_MEMORY) {
            ret = ENOMEM;
        } else {
            ret = EINVAL;
        }
        goto done;
    }

    err = sss_idmap_ctx_set_autorid(idmap_ctx->map, autorid_mode);
    err |= sss_idmap_ctx_set_lower(idmap_ctx->map, idmap_lower);
    err |= sss_idmap_ctx_set_upper(idmap_ctx->map, idmap_upper);
    err |= sss_idmap_ctx_set_rangesize(idmap_ctx->map, rangesize);
    if (err != IDMAP_SUCCESS) {
        /* This should never happen */
        DEBUG(SSSDBG_CRIT_FAILURE, ("sss_idmap_ctx corrupted\n"));
        return EIO;
    }

    /* Read in any existing mappings from the cache */
    ret = sysdb_idmap_get_mappings(tmp_ctx, sysdb, id_ctx->be->domain, &res);
    if (ret != EOK && ret != ENOENT) {
        DEBUG(SSSDBG_FATAL_FAILURE,
              ("Could not read ID mappings from the cache: [%s]\n",
               strerror(ret)));
        goto done;
    }

    if (ret == EOK && res->count > 0) {
        DEBUG(SSSDBG_CONF_SETTINGS,
              ("Initializing [%d] domains for ID-mapping\n", res->count));

        for (i = 0; i < res->count; i++) {
            dom_name = ldb_msg_find_attr_as_string(res->msgs[i],
                                                   SYSDB_NAME,
                                                   NULL);
            if (!dom_name) {
                /* This should never happen */
                ret = EINVAL;
                goto done;
            }

            sid_str = ldb_msg_find_attr_as_string(res->msgs[i],
                                                  SYSDB_IDMAP_SID_ATTR,
                                                  NULL);
            if (!sid_str) {
                /* This should never happen */
                ret = EINVAL;
                goto done;
            }

            slice_num = ldb_msg_find_attr_as_int(res->msgs[i],
                                                 SYSDB_IDMAP_SLICE_ATTR,
                                                 -1);
            if (slice_num == -1) {
                /* This should never happen */
                ret = EINVAL;
                goto done;
            }

            ret = sdap_idmap_add_domain(idmap_ctx, dom_name,
                                        sid_str, slice_num);
            if (ret != EOK) {
                DEBUG(SSSDBG_CRIT_FAILURE,
                      ("Could not add domain [%s][%s][%u] to ID map: [%s]\n",
                       dom_name, sid_str, slice_num, strerror(ret)));
                goto done;
            }
        }
    } else {
        /* This is the first time we're setting up id-mapping
         * Store the default domain as slice 0
         */
        dom_name = dp_opt_get_string(idmap_ctx->id_ctx->opts->basic, SDAP_IDMAP_DEFAULT_DOMAIN);
        if (!dom_name) {
            /* If it's not explicitly specified, use the SSSD domain name */
            dom_name = idmap_ctx->id_ctx->be->domain->name;
            ret = dp_opt_set_string(idmap_ctx->id_ctx->opts->basic,
                                    SDAP_IDMAP_DEFAULT_DOMAIN,
                                    dom_name);
            if (ret != EOK) goto done;
        }

        sid_str = dp_opt_get_string(idmap_ctx->id_ctx->opts->basic, SDAP_IDMAP_DEFAULT_DOMAIN_SID);
        if (sid_str) {
            /* Set the default domain as slice 0 */
            ret = sdap_idmap_add_domain(idmap_ctx, dom_name,
                                        sid_str, 0);
            if (ret != EOK) {
                DEBUG(SSSDBG_CRIT_FAILURE,
                      ("Could not add domain [%s][%s][%u] to ID map: [%s]\n",
                       dom_name, sid_str, 0, strerror(ret)));
                goto done;
            }
        } else {
            if (dp_opt_get_bool(idmap_ctx->id_ctx->opts->basic, SDAP_IDMAP_AUTORID_COMPAT)) {
                /* In autorid compatibility mode, we MUST have a slice 0 */
                DEBUG(SSSDBG_CRIT_FAILURE,
                      ("WARNING: Autorid compatibility mode selected, "
                       "but %s is not set. UID/GID values may differ "
                       "between clients.\n",
                       idmap_ctx->id_ctx->opts->basic[SDAP_IDMAP_DEFAULT_DOMAIN_SID].opt_name));
            }
            /* Otherwise, we'll just fall back to hash values as they are seen */
        }
    }

    *_idmap_ctx = talloc_steal(mem_ctx, idmap_ctx);
    ret = EOK;

done:
    talloc_free(tmp_ctx);
    return ret;
}

errno_t
sdap_idmap_add_domain(struct sdap_idmap_ctx *idmap_ctx,
                      const char *dom_name,
                      const char *dom_sid,
                      id_t slice)
{
    errno_t ret;
    struct sss_idmap_range range;
    enum idmap_error_code err;
    id_t idmap_upper;

    ret = sss_idmap_ctx_get_upper(idmap_ctx->map, &idmap_upper);
    if (ret != IDMAP_SUCCESS) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("Failed to get upper bound of available ID range.\n"));
        ret = EIO;
        goto done;
    }

    ret = sss_idmap_calculate_range(idmap_ctx->map, dom_sid, &slice, &range);
    if (ret != IDMAP_SUCCESS) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("Failed to calculate range for domain [%s]: [%d]\n", dom_name,
               ret));
        ret = EIO;
        goto done;
    }
    DEBUG(SSSDBG_TRACE_LIBS,
          ("Adding domain [%s] as slice [%llu]\n", dom_sid, slice));

    if (range.max > idmap_upper) {
        /* This should never happen */
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("BUG: Range maximum exceeds the global maximum: %d > %d\n",
               range.max, idmap_upper));
        ret = EINVAL;
        goto done;
    }

    /* Add this domain to the map */
    err = sss_idmap_add_domain(idmap_ctx->map, dom_name, dom_sid, &range);
    if (err != IDMAP_SUCCESS) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("Could not add domain [%s] to the map: [%d]\n",
               dom_name, err));
        ret = EIO;
        goto done;
    }

    /* Add this domain to the SYSDB cache so it will survive reboot */
    ret = sysdb_idmap_store_mapping(idmap_ctx->id_ctx->be->domain->sysdb,
                                    idmap_ctx->id_ctx->be->domain,
                                    dom_name, dom_sid,
                                    slice);
done:
    return ret;
}

errno_t
sdap_idmap_get_dom_sid_from_object(TALLOC_CTX *mem_ctx,
                                   const char *object_sid,
                                   char **dom_sid_str)
{
    const char *p;
    long long a;
    size_t c;
    char *endptr;

    if (object_sid == NULL
            || strncmp(object_sid, DOM_SID_PREFIX, DOM_SID_PREFIX_LEN) != 0) {
        return EINVAL;
    }

    p = object_sid + DOM_SID_PREFIX_LEN;
    c = 0;

    do {
        errno = 0;
        a = strtoull(p, &endptr, 10);
        if (errno != 0 || a > UINT32_MAX) {
            return EINVAL;
        }

        if (*endptr == '-') {
            p = endptr + 1;
        } else {
            return EINVAL;
        }
        c++;
    } while(c < 3);

    /* If we made it here, we are now one character past
     * the last hyphen in the object-sid.
     * Copy the dom-sid substring.
     */
    *dom_sid_str = talloc_strndup(mem_ctx, object_sid,
                                  (endptr-object_sid));
    if (!*dom_sid_str) return ENOMEM;

    return EOK;
}

errno_t
sdap_idmap_sid_to_unix(struct sdap_idmap_ctx *idmap_ctx,
                       const char *sid_str,
                       id_t *id)
{
    errno_t ret;
    enum idmap_error_code err;
    char *dom_sid_str = NULL;

    /* Convert the SID into a UNIX ID */
    err = sss_idmap_sid_to_unix(idmap_ctx->map,
                                sid_str,
                                (uint32_t *)id);
    switch (err) {
    case IDMAP_SUCCESS:
        break;
    case IDMAP_NO_DOMAIN:
        /* This is the first time we've seen this domain
         * Create a new domain for it. We'll use the dom-sid
         * as the domain name for now, since we don't have
         * any way to get the real name.
         */
        ret = sdap_idmap_get_dom_sid_from_object(NULL, sid_str,
                                                 &dom_sid_str);
        if (ret != EOK) {
            DEBUG(SSSDBG_MINOR_FAILURE,
                  ("Could not parse domain SID from [%s]\n", sid_str));
            goto done;
        }

        ret = sdap_idmap_add_domain(idmap_ctx,
                                    dom_sid_str, dom_sid_str,
                                    -1);
        if (ret != EOK) {
            DEBUG(SSSDBG_MINOR_FAILURE,
                  ("Could not add new domain for sid [%s]\n", sid_str));
            goto done;
        }

        /* Now try converting to a UNIX ID again */
        err = sss_idmap_sid_to_unix(idmap_ctx->map,
                                    sid_str,
                                    (uint32_t *)id);
        if (err != IDMAP_SUCCESS) {
            DEBUG(SSSDBG_MINOR_FAILURE,
                  ("Could not convert objectSID [%s] to a UNIX ID\n",
                   sid_str));
            ret = EIO;
            goto done;
        }
        break;
    case IDMAP_BUILTIN_SID:
        DEBUG(SSSDBG_TRACE_FUNC,
              ("Object SID [%s] is a built-in one.\n", sid_str));
        /* ENOTSUP indicates built-in SID */
        ret = ENOTSUP;
        goto done;
        break;
    default:
        DEBUG(SSSDBG_MINOR_FAILURE,
              ("Could not convert objectSID [%s] to a UNIX ID\n",
               sid_str));
        ret = EIO;
        goto done;
    }

    ret = EOK;

done:
    talloc_free(dom_sid_str);
    return ret;
}
