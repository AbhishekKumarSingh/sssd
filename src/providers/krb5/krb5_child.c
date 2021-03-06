/*
    SSSD

    Kerberos 5 Backend Module -- tgt_req and changepw child

    Authors:
        Sumit Bose <sbose@redhat.com>

    Copyright (C) 2009-2010 Red Hat

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

#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <popt.h>

#include <security/pam_modules.h>

#include "util/util.h"
#include "util/sss_krb5.h"
#include "util/user_info_msg.h"
#include "util/child_common.h"
#include "providers/dp_backend.h"
#include "providers/krb5/krb5_auth.h"
#include "providers/krb5/krb5_utils.h"
#include "sss_cli.h"

#define SSSD_KRB5_CHANGEPW_PRINCIPAL "kadmin/changepw"

struct krb5_req {
    krb5_context ctx;
    krb5_principal princ;
    char* name;
    krb5_creds *creds;
    krb5_get_init_creds_opt *options;

    struct pam_data *pd;

    char *realm;
    char *ccname;
    char *keytab;
    bool validate;
    bool upn_from_different_realm;
    bool use_enterprise_princ;
    char *fast_ccname;

    const char *upn;
    uid_t uid;
    gid_t gid;
};

static krb5_context krb5_error_ctx;
#define KRB5_CHILD_DEBUG(level, error) KRB5_DEBUG(level, krb5_error_ctx, error)

static krb5_error_code get_changepw_options(krb5_context ctx,
                                            krb5_get_init_creds_opt **_options)
{
    krb5_get_init_creds_opt *options;
    krb5_error_code kerr;

    kerr = sss_krb5_get_init_creds_opt_alloc(ctx, &options);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        return kerr;
    }

    sss_krb5_get_init_creds_opt_set_canonicalize(options, 0);
    krb5_get_init_creds_opt_set_forwardable(options, 0);
    krb5_get_init_creds_opt_set_proxiable(options, 0);
    krb5_get_init_creds_opt_set_renew_life(options, 0);
    krb5_get_init_creds_opt_set_tkt_life(options, 5*60);

    *_options = options;

    return 0;
}

static errno_t sss_send_pac(krb5_authdata **pac_authdata)
{
    struct sss_cli_req_data sss_data;
    int ret;
    int errnop;

    sss_data.len = pac_authdata[0]->length;
    sss_data.data = pac_authdata[0]->contents;

    ret = sss_pac_make_request(SSS_PAC_ADD_PAC_USER, &sss_data,
                               NULL, NULL, &errnop);
    if (ret != NSS_STATUS_SUCCESS || errnop != 0) {
        DEBUG(SSSDBG_OP_FAILURE, ("sss_pac_make_request failed [%d][%d].\n",
                                  ret, errnop));
        return EIO;
    }

    return EOK;
}

static void sss_krb5_expire_callback_func(krb5_context context, void *data,
                                          krb5_timestamp password_expiration,
                                          krb5_timestamp account_expiration,
                                          krb5_boolean is_last_req)
{
    int ret;
    uint32_t *blob;
    long exp_time;
    struct krb5_req *kr = talloc_get_type(data, struct krb5_req);

    if (password_expiration == 0) {
        return;
    }

    exp_time = password_expiration - time(NULL);
    if (exp_time < 0 || exp_time > UINT32_MAX) {
        DEBUG(1, ("Time to expire out of range.\n"));
        return;
    }
    DEBUG(SSSDBG_TRACE_INTERNAL, ("exp_time: [%d]\n", exp_time));

    blob = talloc_array(kr->pd, uint32_t, 2);
    if (blob == NULL) {
        DEBUG(1, ("talloc_size failed.\n"));
        return;
    }

    blob[0] = SSS_PAM_USER_INFO_EXPIRE_WARN;
    blob[1] = (uint32_t) exp_time;

    ret = pam_add_response(kr->pd, SSS_PAM_USER_INFO, 2 * sizeof(uint32_t),
                           (uint8_t *) blob);
    if (ret != EOK) {
        DEBUG(1, ("pam_add_response failed.\n"));
    }

    return;
}

#ifdef HAVE_KRB5_GET_INIT_CREDS_OPT_SET_RESPONDER
/*
 * TODO: These features generally would requires a significant refactoring
 * of SSSD and MIT krb5 doesn't support them anyway. They are listed here
 * simply as a reminder of things that might become future feature potential.
 *
 *   1. tokeninfo selection
 *   2. challenge
 *   3. discreet token/pin prompting
 *   4. interactive otp format correction
 *   5. nextOTP
 *
 */
typedef int (*checker)(int c);

static inline checker pick_checker(int format)
{
    switch (format) {
    case KRB5_RESPONDER_OTP_FORMAT_DECIMAL:
        return isdigit;
    case KRB5_RESPONDER_OTP_FORMAT_HEXADECIMAL:
        return isxdigit;
    case KRB5_RESPONDER_OTP_FORMAT_ALPHANUMERIC:
        return isalnum;
    }

    return NULL;
}

static int token_pin_destructor(char *mem)
{
    safezero(mem, strlen(mem));
    return 0;
}

static krb5_error_code tokeninfo_matches(TALLOC_CTX *mem_ctx,
                                         const krb5_responder_otp_tokeninfo *ti,
                                         const char *pwd, size_t len,
                                         char **out_token, char **out_pin)
{
    char *token = NULL, *pin = NULL;
    checker check = NULL;
    int i;


    if (ti->flags & KRB5_RESPONDER_OTP_FLAGS_NEXTOTP) {
        return ENOTSUP;
    }

    if (ti->challenge != NULL) {
        return ENOTSUP;
    }

    /* This is a non-sensical value. */
    if (ti->length == 0) {
        return EPROTO;
    }

    if (ti->flags & KRB5_RESPONDER_OTP_FLAGS_COLLECT_TOKEN) {
        /* ASSUMPTION: authtok has one of the following formats:
         *   1. TokenValue
         *   2. PIN+TokenValue
         */
        token = talloc_strndup(mem_ctx, pwd, len);
        if (token == NULL) {
            return ENOMEM;
        }
        talloc_set_destructor(token, token_pin_destructor);

        if (ti->flags & KRB5_RESPONDER_OTP_FLAGS_COLLECT_PIN) {
            /* If the server desires a separate pin, we will split it.
             * ASSUMPTION: Format of authtok is PIN+TokenValue. */
            if (ti->flags & KRB5_RESPONDER_OTP_FLAGS_SEPARATE_PIN) {
                if (ti->length < 1) {
                    talloc_free(token);
                    return ENOTSUP;
                }

                if (ti->length >= len) {
                    talloc_free(token);
                    return EMSGSIZE;
                }

                /* Copy the PIN from the front of the value. */
                pin = talloc_strndup(NULL, pwd, len - ti->length);
                if (pin == NULL) {
                    talloc_free(token);
                    return ENOMEM;
                }
                talloc_set_destructor(pin, token_pin_destructor);

                /* Remove the PIN from the front of the token value. */
                memmove(token, token + len - ti->length, ti->length + 1);

                check = pick_checker(ti->format);
            } else {
                if (ti->length > 0 && ti->length > len) {
                    talloc_free(token);
                    return EMSGSIZE;
                }
            }
        } else {
            if (ti->length > 0 && ti->length != len) {
                talloc_free(token);
                return EMSGSIZE;
            }

            check = pick_checker(ti->format);
        }
    } else {
        pin = talloc_strndup(mem_ctx, pwd, len);
        if (pin == NULL) {
            return ENOMEM;
        }
        talloc_set_destructor(pin, token_pin_destructor);
    }

    /* If check is set, we need to verify the contents of the token. */
    for (i = 0; check != NULL && token[i] != '\0'; i++) {
        if (!check(token[i])) {
            talloc_free(token);
            talloc_free(pin);
            return EBADMSG;
        }
    }

    *out_token = token;
    *out_pin = pin;
    return 0;
}

static krb5_error_code answer_otp(krb5_context ctx,
                                  struct krb5_req *kr,
                                  krb5_responder_context rctx)
{
    krb5_responder_otp_challenge *chl;
    char *token = NULL, *pin = NULL;
    const char *pwd = NULL;
    krb5_error_code ret;
    size_t i, len;

    ret = krb5_responder_otp_get_challenge(ctx, rctx, &chl);
    if (ret != EOK || chl == NULL) {
        /* Either an error, or nothing to do. */
        return ret;
    }

    if (chl->tokeninfo == NULL || chl->tokeninfo[0] == NULL) {
        /* No tokeninfos? Absurd! */
        ret = EINVAL;
        goto done;
    }

    /* Validate our assumptions about the contents of authtok. */
    ret = sss_authtok_get_password(kr->pd->authtok, &pwd, &len);
    if (ret != EOK)
        goto done;

    /* Find the first supported tokeninfo which matches our authtoken. */
    for (i = 0; chl->tokeninfo[i] != NULL; i++) {
        ret = tokeninfo_matches(kr, chl->tokeninfo[i], pwd, len, &token, &pin);
        if (ret == EOK) {
            break;
        }

        switch (ret) {
        case EBADMSG:
        case EMSGSIZE:
        case ENOTSUP:
        case EPROTO:
            break;
        default:
            goto done;
        }
    }
    if (chl->tokeninfo[i] == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("No tokeninfos found which match our credentials.\n"));
        ret = EOK;
        goto done;
    }

    if (chl->tokeninfo[i]->flags & KRB5_RESPONDER_OTP_FLAGS_COLLECT_TOKEN) {
        /* Don't let SSSD cache the OTP authtok since it is single-use. */
        ret = pam_add_response(kr->pd, SSS_OTP, 0, NULL);
        if (ret != EOK) {
            DEBUG(1, ("pam_add_response failed.\n"));
            goto done;
        }
    }

    /* Respond with the appropriate answer. */
    ret = krb5_responder_otp_set_answer(ctx, rctx, i, token, pin);
done:
    talloc_free(token);
    talloc_free(pin);
    krb5_responder_otp_challenge_free(ctx, rctx, chl);
    return ret;
}

static krb5_error_code sss_krb5_responder(krb5_context ctx,
                                          void *data,
                                          krb5_responder_context rctx)
{
    struct krb5_req *kr = talloc_get_type(data, struct krb5_req);

    if (kr == NULL) {
        return EINVAL;
    }

    return answer_otp(ctx, kr, rctx);
}
#endif

static krb5_error_code sss_krb5_prompter(krb5_context context, void *data,
                                         const char *name, const char *banner,
                                         int num_prompts, krb5_prompt prompts[])
{
    int ret;
    struct krb5_req *kr = talloc_get_type(data, struct krb5_req);

    if (num_prompts != 0) {
        DEBUG(1, ("Cannot handle password prompts.\n"));
        return KRB5_LIBOS_CANTREADPWD;
    }

    if (banner == NULL || *banner == '\0') {
        DEBUG(5, ("Prompter called with empty banner, nothing to do.\n"));
        return EOK;
    }

    DEBUG(SSSDBG_FUNC_DATA, ("Prompter called with [%s].\n", banner));

    ret = pam_add_response(kr->pd, SSS_PAM_TEXT_MSG, strlen(banner)+1,
                           (const uint8_t *) banner);
    if (ret != EOK) {
        DEBUG(1, ("pam_add_response failed.\n"));
    }

    return EOK;
}


static krb5_error_code create_empty_cred(krb5_context ctx, krb5_principal princ,
                                         krb5_creds **_cred)
{
    krb5_error_code kerr;
    krb5_creds *cred = NULL;
    krb5_data *krb5_realm;

    cred = calloc(sizeof(krb5_creds), 1);
    if (cred == NULL) {
        DEBUG(1, ("calloc failed.\n"));
        return ENOMEM;
    }

    kerr = krb5_copy_principal(ctx, princ, &cred->client);
    if (kerr != 0) {
        DEBUG(1, ("krb5_copy_principal failed.\n"));
        goto done;
    }

    krb5_realm = krb5_princ_realm(ctx, princ);

    kerr = krb5_build_principal_ext(ctx, &cred->server,
                                    krb5_realm->length, krb5_realm->data,
                                    KRB5_TGS_NAME_SIZE, KRB5_TGS_NAME,
                                    krb5_realm->length, krb5_realm->data, 0);
    if (kerr != 0) {
        DEBUG(1, ("krb5_build_principal_ext failed.\n"));
        goto done;
    }

    DEBUG(SSSDBG_TRACE_INTERNAL, ("Created empty krb5_creds.\n"));

done:
    if (kerr != 0) {
        if (cred != NULL && cred->client != NULL) {
            krb5_free_principal(ctx, cred->client);
        }

        free(cred);
    } else {
        *_cred = cred;
    }

    return kerr;
}

static krb5_error_code
store_creds_in_ccache(krb5_context ctx, krb5_principal princ,
                      krb5_ccache cc, krb5_creds *creds)
{
    krb5_error_code kerr;
    krb5_creds *l_cred;

    kerr = krb5_cc_initialize(ctx, cc, princ);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_OP_FAILURE, kerr);
        goto done;
    }

    if (creds == NULL) {
        kerr = create_empty_cred(ctx, princ, &l_cred);
        if (kerr != 0) {
            KRB5_CHILD_DEBUG(SSSDBG_OP_FAILURE, kerr);
            goto done;
        }
    } else {
        l_cred = creds;
    }

    kerr = krb5_cc_store_cred(ctx, cc, l_cred);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_OP_FAILURE, kerr);
        goto done;
    }

#ifdef HAVE_KRB5_DIRCACHE
    kerr = krb5_cc_switch(ctx, cc);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_OP_FAILURE, kerr);
        goto done;
    }
#endif /* HAVE_KRB5_DIRCACHE */

    kerr = krb5_cc_close(ctx, cc);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_OP_FAILURE, kerr);
        goto done;
    }

done:
    return kerr;
}

static krb5_error_code create_ccache_file(krb5_context ctx,
                                          krb5_principal princ,
                                          char *ccname, krb5_creds *creds)
{
    krb5_error_code kerr;
    krb5_ccache tmp_cc = NULL;
    char *cc_file_name;
    int fd = -1;
    size_t ccname_len;
    char *dummy;
    char *tmp_ccname;
    TALLOC_CTX *tmp_ctx = NULL;
    mode_t old_umask;

    DEBUG(SSSDBG_FUNC_DATA, ("Creating ccache at [%s]\n", ccname));

    if (strncmp(ccname, "FILE:", 5) == 0) {
        cc_file_name = ccname + 5;
    } else {
        cc_file_name = ccname;
    }

    if (cc_file_name[0] != '/') {
        DEBUG(1, ("Ccache filename is not an absolute path.\n"));
        return EINVAL;
    }

    tmp_ctx = talloc_new(tmp_ctx);
    if (tmp_ctx == NULL) {
        DEBUG(1, ("talloc_new failed.\n"));
        return ENOMEM;
    }

    dummy = strrchr(cc_file_name, '/');
    tmp_ccname = talloc_strndup(tmp_ctx, cc_file_name,
                                (size_t) (dummy-cc_file_name));
    if (tmp_ccname == NULL) {
        DEBUG(1, ("talloc_strdup failed.\n"));
        kerr = ENOMEM;
        goto done;
    }
    tmp_ccname = talloc_asprintf_append(tmp_ccname, "/.krb5cc_dummy_XXXXXX");
    if (tmp_ccname == NULL) {
        kerr = ENOMEM;
        goto done;
    }

    old_umask = umask(077);
    fd = mkstemp(tmp_ccname);
    umask(old_umask);
    if (fd == -1) {
        kerr = errno;
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("mkstemp failed [%d][%s].\n", kerr, strerror(kerr)));
        goto done;
    }

    kerr = krb5_cc_resolve(ctx, tmp_ccname, &tmp_cc);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        goto done;
    }

    kerr = store_creds_in_ccache(ctx, princ, tmp_cc, creds);
    if (fd != -1) {
        close(fd);
        fd = -1;
    }
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        goto done;
    }


    ccname_len = strlen(cc_file_name);
    if (ccname_len >= 6 && strcmp(cc_file_name + (ccname_len - 6), "XXXXXX") == 0) {
        fd = mkstemp(cc_file_name);
        if (fd == -1) {
            kerr = errno;
            DEBUG(SSSDBG_CRIT_FAILURE,
                  ("mkstemp failed [%d][%s].\n", kerr, strerror(kerr)));
            goto done;
        }
    }

    kerr = rename(tmp_ccname, cc_file_name);
    if (kerr == -1) {
        kerr = errno;
        DEBUG(1, ("rename failed [%d][%s].\n", kerr, strerror(kerr)));
    }

    DEBUG(SSSDBG_TRACE_LIBS, ("Created ccache file: [%s]\n", ccname));

done:
    if (kerr != 0 && tmp_cc != NULL) {
        krb5_cc_destroy(ctx, tmp_cc);
    }

    if (fd != -1) {
        close(fd);
    }

    talloc_free(tmp_ctx);
    return kerr;
}

#ifdef HAVE_KRB5_DIRCACHE

static errno_t
create_ccdir(const char *dirname, uid_t uid, gid_t gid)
{
    mode_t old_umask;
    struct stat statbuf;
    errno_t ret;

    old_umask = umask(0000);
    ret = mkdir(dirname, 0700);
    umask(old_umask);
    if (ret == -1) {
        /* Failing the mkdir is only OK if the directory already
         * exists AND it is owned by the same user and group and
         * has the correct permissions.
         */
        ret = errno;
        if (ret == EEXIST) {
            errno = 0;
            ret = stat(dirname, &statbuf);
            if (ret == -1) {
                ret = errno;
                DEBUG(SSSDBG_CRIT_FAILURE,
                        ("stat failed [%d]: %s\n", ret, strerror(ret)));
                return EIO;
            }

            if (statbuf.st_uid != uid || statbuf.st_gid != gid) {
                DEBUG(SSSDBG_CRIT_FAILURE,
                      ("The directory %s is owned by %d/%d, expected %d/%d\n",
                       dirname, statbuf.st_uid, statbuf.st_gid, uid, gid));
                return EACCES;
            }

            if ((statbuf.st_mode & ~S_IFMT) != 0700) {
                DEBUG(SSSDBG_CRIT_FAILURE,
                      ("The directory %s has wrong permissions %o, expected 0700\n",
                       dirname, (statbuf.st_mode & ~S_IFMT)));
                return EACCES;
            }
        } else {
            DEBUG(SSSDBG_CRIT_FAILURE, ("mkdir [%s] failed [%d]: %s\n",
                  dirname, ret, strerror(ret)));
            return ret;
        }
    }

    return EOK;
}

static krb5_error_code
create_ccache_in_dir(uid_t uid, gid_t gid,
                     krb5_context ctx,
                     krb5_principal princ,
                     char *ccname, krb5_creds *creds)
{
    krb5_error_code kerr;
    krb5_ccache tmp_cc = NULL;
    const char *dirname;

    DEBUG(SSSDBG_FUNC_DATA, ("Creating ccache at [%s]\n", ccname));

    dirname = sss_krb5_residual_check_type(ccname, SSS_KRB5_TYPE_DIR);
    if (dirname == NULL) {
        return EIO;
    }

    kerr = become_user(uid, gid);
    if (kerr != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("become_user failed.\n"));
        goto done;
    }

    if (dirname[0] == ':') {
        /* Cache name in the form of DIR::filepath represents a single
         * ccache in a collection that we are trying to reuse.
         * See src/lib/krb5/ccache/cc_dir.c in the MIT Kerberos tree.
         */
        kerr = krb5_cc_resolve(ctx, ccname, &tmp_cc);
        if (kerr != 0) {
            KRB5_CHILD_DEBUG(SSSDBG_OP_FAILURE, kerr);
            goto done;
        }
    } else if (dirname[0] == '/') {
        /* An absolute path denotes that krb5_child should create a new
         * ccache. We can afford to just call mkdir(dirname) because we
         * only want the last component to be created.
         */

        kerr = create_ccdir(dirname, uid, gid);
        if (kerr) {
            DEBUG(SSSDBG_OP_FAILURE,
                  ("Cannot create directory %s\n", dirname));
            goto done;
        }

        kerr = krb5_cc_set_default_name(ctx, ccname);
        if (kerr != 0) {
            KRB5_CHILD_DEBUG(SSSDBG_OP_FAILURE, kerr);
            goto done;
        }

        kerr = krb5_cc_new_unique(ctx, "DIR", NULL, &tmp_cc);
        if (kerr != 0) {
            KRB5_CHILD_DEBUG(SSSDBG_OP_FAILURE, kerr);
            goto done;
        }
    } else {
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("Wrong residual format for DIR in ccache %s\n", ccname));
        return EIO;
    }

    kerr = store_creds_in_ccache(ctx, princ, tmp_cc, creds);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_OP_FAILURE, kerr);
        goto done;
    }

done:
    if (kerr != 0 && tmp_cc != NULL) {
        krb5_cc_destroy(ctx, tmp_cc);
    }
    return kerr;
}

#endif /* HAVE_KRB5_DIRCACHE */

static krb5_error_code
create_ccache(uid_t uid, gid_t gid, krb5_context ctx,
              krb5_principal princ, char *ccname, krb5_creds *creds)
{
    enum sss_krb5_cc_type cctype;

    cctype = sss_krb5_get_type(ccname);
    switch (cctype) {
        case SSS_KRB5_TYPE_FILE:
            return create_ccache_file(ctx, princ, ccname, creds);
#ifdef HAVE_KRB5_DIRCACHE
        case SSS_KRB5_TYPE_DIR:
            return create_ccache_in_dir(uid, gid, ctx, princ, ccname, creds);
#endif /* HAVE_KRB5_DIRCACHE */
        default:
            DEBUG(SSSDBG_CRIT_FAILURE, ("Unknown cache type\n"));
            return EINVAL;
    }

    return EINVAL;  /* Should never get here */
}

static errno_t pack_response_packet(TALLOC_CTX *mem_ctx, errno_t error,
                                    struct response_data *resp_list,
                                    uint8_t **_buf, size_t *_len)
{
    uint8_t *buf;
    size_t size = 0;
    size_t p = 0;
    struct response_data *pdr;

    /* A buffer with the following structure must be created:
     * int32_t status of the request (required)
     * message (zero or more)
     *
     * A message consists of:
     * int32_t type of the message
     * int32_t length of the following data
     * uint8_t[len] data
     */

    size = sizeof(int32_t);

    for (pdr = resp_list; pdr != NULL; pdr = pdr->next) {
        size += 2*sizeof(int32_t) + pdr->len;
    }

    buf = talloc_array(mem_ctx, uint8_t, size);
    if (!buf) {
        DEBUG(1, ("Insufficient memory to create message.\n"));
        return ENOMEM;
    }

    SAFEALIGN_SET_INT32(&buf[p], error, &p);

    for (pdr = resp_list; pdr != NULL; pdr = pdr->next) {
        SAFEALIGN_SET_INT32(&buf[p], pdr->type, &p);
        SAFEALIGN_SET_INT32(&buf[p], pdr->len, &p);
        safealign_memcpy(&buf[p], pdr->data, pdr->len, &p);
    }

    DEBUG(SSSDBG_TRACE_INTERNAL, ("response packet size: [%d]\n", p));

    *_buf = buf;
    *_len = p;
    return EOK;
}

static errno_t k5c_attach_ccname_msg(struct krb5_req *kr)
{
    char *msg = NULL;
    int ret;

    if (kr->ccname == NULL) {
        DEBUG(1, ("Error obtaining ccname.\n"));
        return ERR_INTERNAL;
    }

    msg = talloc_asprintf(kr, "%s=%s",CCACHE_ENV_NAME, kr->ccname);
    if (msg == NULL) {
        DEBUG(1, ("talloc_asprintf failed.\n"));
        return ENOMEM;
    }

    ret = pam_add_response(kr->pd, SSS_PAM_ENV_ITEM,
                           strlen(msg) + 1, (uint8_t *)msg);
    talloc_zfree(msg);

    return ret;
}

static errno_t k5c_send_data(struct krb5_req *kr, int fd, errno_t error)
{
    size_t written;
    uint8_t *buf;
    size_t len;
    int ret;

    ret = pack_response_packet(kr, error, kr->pd->resp_list, &buf, &len);
    if (ret != EOK) {
        DEBUG(1, ("pack_response_packet failed.\n"));
        return ret;
    }

    errno = 0;
    written = sss_atomic_write_s(fd, buf, len);
    if (written == -1) {
        ret = errno;
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("write failed [%d][%s].\n", ret, strerror(ret)));
        return ret;
    }

    if (written != len) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("Write error, wrote [%d] bytes, expected [%d]\n",
               written, len));
        return EOK;
    }

    DEBUG(SSSDBG_TRACE_ALL, ("Response sent.\n"));

    return EOK;
}

static errno_t add_ticket_times_and_upn_to_response(struct krb5_req *kr)
{
    int ret;
    int64_t t[4];
    krb5_error_code kerr;
    char *upn = NULL;
    unsigned int upn_len = 0;

    t[0] = (int64_t) kr->creds->times.authtime;
    t[1] = (int64_t) kr->creds->times.starttime;
    t[2] = (int64_t) kr->creds->times.endtime;
    t[3] = (int64_t) kr->creds->times.renew_till;

    ret = pam_add_response(kr->pd, SSS_KRB5_INFO_TGT_LIFETIME,
                           4*sizeof(int64_t), (uint8_t *) t);
    if (ret != EOK) {
        DEBUG(1, ("pack_response_packet failed.\n"));
        goto done;
    }

    kerr = krb5_unparse_name_ext(kr->ctx, kr->creds->client, &upn, &upn_len);
    if (kerr != 0) {
        DEBUG(SSSDBG_OP_FAILURE, ("krb5_unparse_name failed.\n"));
        goto done;
    }

    ret = pam_add_response(kr->pd, SSS_KRB5_INFO_UPN, upn_len,
                           (uint8_t *) upn);
    krb5_free_unparsed_name(kr->ctx, upn);
    if (ret != EOK) {
        DEBUG(1, ("pack_response_packet failed.\n"));
        goto done;
    }

done:
    return ret;
}

static krb5_error_code validate_tgt(struct krb5_req *kr)
{
    krb5_error_code kerr;
    krb5_error_code kt_err;
    char *principal = NULL;
    krb5_keytab keytab;
    krb5_kt_cursor cursor;
    krb5_keytab_entry entry;
    krb5_verify_init_creds_opt opt;
    krb5_principal validation_princ = NULL;
    bool realm_entry_found = false;
    krb5_ccache validation_ccache = NULL;
    krb5_authdata **pac_authdata = NULL;

    memset(&keytab, 0, sizeof(keytab));
    kerr = krb5_kt_resolve(kr->ctx, kr->keytab, &keytab);
    if (kerr != 0) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("error resolving keytab [%s], " \
                                    "not verifying TGT.\n", kr->keytab));
        return kerr;
    }

    memset(&cursor, 0, sizeof(cursor));
    kerr = krb5_kt_start_seq_get(kr->ctx, keytab, &cursor);
    if (kerr != 0) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("error reading keytab [%s], " \
                                    "not verifying TGT.\n", kr->keytab));
        return kerr;
    }

    /* We look for the first entry from our realm or take the last one */
    memset(&entry, 0, sizeof(entry));
    while ((kt_err = krb5_kt_next_entry(kr->ctx, keytab, &entry, &cursor)) == 0) {
        if (validation_princ != NULL) {
            krb5_free_principal(kr->ctx, validation_princ);
            validation_princ = NULL;
        }
        kerr = krb5_copy_principal(kr->ctx, entry.principal,
                                   &validation_princ);
        if (kerr != 0) {
            DEBUG(SSSDBG_CRIT_FAILURE, ("krb5_copy_principal failed.\n"));
            goto done;
        }

        kerr = sss_krb5_free_keytab_entry_contents(kr->ctx, &entry);
        if (kerr != 0) {
            DEBUG(SSSDBG_MINOR_FAILURE, ("Failed to free keytab entry.\n"));
        }
        memset(&entry, 0, sizeof(entry));

        if (krb5_realm_compare(kr->ctx, validation_princ, kr->princ)) {
            DEBUG(SSSDBG_TRACE_INTERNAL,
                  ("Found keytab entry with the realm of the credential.\n"));
            realm_entry_found = true;
            break;
        }
    }

    if (!realm_entry_found) {
        DEBUG(SSSDBG_TRACE_INTERNAL,
                ("Keytab entry with the realm of the credential not found "
                 "in keytab. Using the last entry.\n"));
    }

    /* Close the keytab here.  Even though we're using cursors, the file
     * handle is stored in the krb5_keytab structure, and it gets
     * overwritten when the verify_init_creds() call below creates its own
     * cursor, creating a leak. */
    kerr = krb5_kt_end_seq_get(kr->ctx, keytab, &cursor);
    if (kerr != 0) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("krb5_kt_end_seq_get failed, " \
                                    "not verifying TGT.\n"));
        goto done;
    }

    /* check if we got any errors from krb5_kt_next_entry */
    if (kt_err != 0 && kt_err != KRB5_KT_END) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("error reading keytab [%s], " \
                                    "not verifying TGT.\n", kr->keytab));
        goto done;
    }

    /* Get the principal to which the key belongs, for logging purposes. */
    principal = NULL;
    kerr = krb5_unparse_name(kr->ctx, validation_princ, &principal);
    if (kerr != 0) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("internal error parsing principal name, "
                                    "not verifying TGT.\n"));
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        goto done;
    }


    krb5_verify_init_creds_opt_init(&opt);
    kerr = krb5_verify_init_creds(kr->ctx, kr->creds, validation_princ, keytab,
                                  &validation_ccache, &opt);

    if (kerr == 0) {
        DEBUG(SSSDBG_TRACE_FUNC, ("TGT verified using key for [%s].\n",
                                  principal));
    } else {
        DEBUG(SSSDBG_CRIT_FAILURE ,("TGT failed verification using key " \
                                    "for [%s].\n", principal));
        goto done;
    }

    /* Try to find and send the PAC to the PAC responder for principals which
     * do not belong to our realm. Failures are not critical. */
    if (kr->upn_from_different_realm) {
        kerr = sss_extract_pac(kr->ctx, validation_ccache, validation_princ,
                               kr->creds->client, keytab, &pac_authdata);
        if (kerr != 0) {
            DEBUG(SSSDBG_OP_FAILURE, ("sss_extract_and_send_pac failed, group " \
                                      "membership for user with principal [%s] " \
                                      "might not be correct.\n", kr->name));
            kerr = 0;
            goto done;
        }

        kerr = sss_send_pac(pac_authdata);
        krb5_free_authdata(kr->ctx, pac_authdata);
        if (kerr != 0) {
            DEBUG(SSSDBG_OP_FAILURE, ("sss_send_pac failed, group " \
                                      "membership for user with principal [%s] " \
                                      "might not be correct.\n", kr->name));
            kerr = 0;
        }
    }

done:
    if (validation_ccache != NULL) {
        krb5_cc_destroy(kr->ctx, validation_ccache);
    }

    if (krb5_kt_close(kr->ctx, keytab) != 0) {
        DEBUG(SSSDBG_MINOR_FAILURE, ("krb5_kt_close failed"));
    }
    if (validation_princ != NULL) {
        krb5_free_principal(kr->ctx, validation_princ);
    }
    if (principal != NULL) {
        sss_krb5_free_unparsed_name(kr->ctx, principal);
    }

    return kerr;

}

static void krb5_set_canonicalize(krb5_get_init_creds_opt *opts)
{
    int canonicalize = 0;
    char *tmp_str;

    tmp_str = getenv(SSSD_KRB5_CANONICALIZE);
    if (tmp_str != NULL && strcasecmp(tmp_str, "true") == 0) {
        canonicalize = 1;
    }
    DEBUG(SSSDBG_CONF_SETTINGS, ("%s is set to [%s]\n",
          SSSD_KRB5_CANONICALIZE, tmp_str ? tmp_str : "not set"));
    sss_krb5_get_init_creds_opt_set_canonicalize(opts, canonicalize);
}

static krb5_error_code get_and_save_tgt_with_keytab(krb5_context ctx,
                                                    krb5_principal princ,
                                                    krb5_keytab keytab,
                                                    char *ccname)
{
    krb5_error_code kerr = 0;
    krb5_creds creds;
    krb5_get_init_creds_opt options;

    memset(&creds, 0, sizeof(creds));
    memset(&options, 0, sizeof(options));

    krb5_get_init_creds_opt_set_address_list(&options, NULL);
    krb5_get_init_creds_opt_set_forwardable(&options, 0);
    krb5_get_init_creds_opt_set_proxiable(&options, 0);
    krb5_set_canonicalize(&options);

    kerr = krb5_get_init_creds_keytab(ctx, &creds, princ, keytab, 0, NULL,
                                      &options);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        return kerr;
    }

    /* Use the updated principal in the creds in case canonicalized */
    kerr = create_ccache_file(ctx, creds.client, ccname, &creds);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        goto done;
    }
    kerr = 0;

done:
    krb5_free_cred_contents(ctx, &creds);

    return kerr;

}

static krb5_error_code get_and_save_tgt(struct krb5_req *kr,
                                        const char *password)
{
    const char *realm_name;
    int realm_length;
    krb5_error_code kerr;


    kerr = sss_krb5_get_init_creds_opt_set_expire_callback(kr->ctx, kr->options,
                                                  sss_krb5_expire_callback_func,
                                                  kr);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        DEBUG(1, ("Failed to set expire callback, continue without.\n"));
    }

    sss_krb5_princ_realm(kr->ctx, kr->princ, &realm_name, &realm_length);

    DEBUG(SSSDBG_TRACE_FUNC,
          ("Attempting kinit for realm [%s]\n",realm_name));
    kerr = krb5_get_init_creds_password(kr->ctx, kr->creds, kr->princ,
                                        discard_const(password),
                                        sss_krb5_prompter, kr, 0,
                                        NULL, kr->options);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        return kerr;
    }

    if (kr->validate) {
        kerr = validate_tgt(kr);
        if (kerr != 0) {
            KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
            return kerr;
        }

    } else {
        DEBUG(SSSDBG_CONF_SETTINGS, ("TGT validation is disabled.\n"));
    }

    if (kr->validate || kr->fast_ccname != NULL) {
        /* We drop root privileges which were needed to read the keytab file
         * for the validation of the credentials or for FAST here to run the
         * ccache I/O operations with user privileges. */
        kerr = become_user(kr->uid, kr->gid);
        if (kerr != 0) {
            DEBUG(1, ("become_user failed.\n"));
            return kerr;
        }
    }

    /* Use the updated principal in the creds in case canonicalized */
    kerr = create_ccache(kr->uid, kr->gid, kr->ctx,
                         kr->creds ? kr->creds->client : kr->princ,
                         kr->ccname, kr->creds);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        goto done;
    }

    kerr = add_ticket_times_and_upn_to_response(kr);
    if (kerr != 0) {
        DEBUG(1, ("add_ticket_times_and_upn_to_response failed.\n"));
    }

    kerr = 0;

done:
    krb5_free_cred_contents(kr->ctx, kr->creds);

    return kerr;

}

static errno_t map_krb5_error(krb5_error_code kerr)
{
    KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);

    switch (kerr) {
    case 0:
        return ERR_OK;

    case KRB5_LIBOS_CANTREADPWD:
        return ERR_NO_CREDS;

    case KRB5_KDC_UNREACH:
        return ERR_NETWORK_IO;

    case KRB5KDC_ERR_KEY_EXP:
        return ERR_CREDS_EXPIRED;

    case KRB5KRB_AP_ERR_BAD_INTEGRITY:
    case KRB5_PREAUTH_FAILED:
    case KRB5KDC_ERR_PREAUTH_FAILED:
        return ERR_AUTH_FAILED;

    default:
        return ERR_INTERNAL;
    }
}

static errno_t changepw_child(struct krb5_req *kr, bool prelim)
{
    int ret;
    krb5_error_code kerr = 0;
    const char *password = NULL;
    const char *newpassword = NULL;
    int result_code = -1;
    krb5_data result_code_string;
    krb5_data result_string;
    char *user_error_message = NULL;
    size_t user_resp_len;
    uint8_t *user_resp;
    krb5_prompter_fct prompter = NULL;
    const char *realm_name;
    int realm_length;
    krb5_get_init_creds_opt *chagepw_options;

    DEBUG(SSSDBG_TRACE_LIBS, ("Password change operation\n"));

    ret = sss_authtok_get_password(kr->pd->authtok, &password, NULL);
    if (ret != EOK) {
        DEBUG(1, ("Failed to fetch current password [%d] %s.\n",
                  ret, strerror(ret)));
        return ERR_NO_CREDS;
    }

    if (!prelim) {
        /* We do not need a password expiration warning here. */
        prompter = sss_krb5_prompter;
    }

    kerr = get_changepw_options(kr->ctx, &chagepw_options);
    if (kerr != 0) {
        DEBUG(SSSDBG_OP_FAILURE, ("get_changepw_options failed.\n"));
        return kerr;
    }

    sss_krb5_princ_realm(kr->ctx, kr->princ, &realm_name, &realm_length);

    DEBUG(SSSDBG_TRACE_FUNC,
          ("Attempting kinit for realm [%s]\n",realm_name));
    kerr = krb5_get_init_creds_password(kr->ctx, kr->creds, kr->princ,
                                        discard_const(password),
                                        prompter, kr, 0,
                                        SSSD_KRB5_CHANGEPW_PRINCIPAL,
                                        chagepw_options);
    sss_krb5_get_init_creds_opt_free(kr->ctx, chagepw_options);
    if (kerr != 0) {
        return kerr;
    }

    sss_authtok_set_empty(kr->pd->authtok);

    if (prelim) {
        DEBUG(SSSDBG_TRACE_LIBS,
              ("Initial authentication for change password operation "
               "successful.\n"));
        krb5_free_cred_contents(kr->ctx, kr->creds);
        return EOK;
    }

    ret = sss_authtok_get_password(kr->pd->newauthtok, &newpassword, NULL);
    if (ret != EOK) {
        DEBUG(1, ("Failed to fetch new password [%d] %s.\n",
                  ret, strerror(ret)));
        return ERR_NO_CREDS;
    }

    memset(&result_code_string, 0, sizeof(krb5_data));
    memset(&result_string, 0, sizeof(krb5_data));
    kerr = krb5_change_password(kr->ctx, kr->creds,
                                discard_const(newpassword), &result_code,
                                &result_code_string, &result_string);

    if (kerr == KRB5_KDC_UNREACH) {
        return ERR_NETWORK_IO;
    }

    if (kerr != 0 || result_code != 0) {
        if (kerr != 0) {
            KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        }

        if (result_code_string.length > 0) {
            DEBUG(1, ("krb5_change_password failed [%d][%.*s].\n", result_code,
                      result_code_string.length, result_code_string.data));
            user_error_message = talloc_strndup(kr->pd, result_code_string.data,
                                                result_code_string.length);
            if (user_error_message == NULL) {
                DEBUG(1, ("talloc_strndup failed.\n"));
            }
        }

        if (result_string.length > 0) {
            DEBUG(1, ("krb5_change_password failed [%d][%.*s].\n", result_code,
                      result_string.length, result_string.data));
            talloc_free(user_error_message);
            user_error_message = talloc_strndup(kr->pd, result_string.data,
                                                result_string.length);
            if (user_error_message == NULL) {
                DEBUG(1, ("talloc_strndup failed.\n"));
            }
        }

        if (user_error_message != NULL) {
            ret = pack_user_info_chpass_error(kr->pd, user_error_message,
                                              &user_resp_len, &user_resp);
            if (ret != EOK) {
                DEBUG(1, ("pack_user_info_chpass_error failed.\n"));
            } else {
                ret = pam_add_response(kr->pd, SSS_PAM_USER_INFO, user_resp_len,
                                       user_resp);
                if (ret != EOK) {
                    DEBUG(1, ("pack_response_packet failed.\n"));
                }
            }
        }

        return ERR_CHPASS_FAILED;
    }

    krb5_free_cred_contents(kr->ctx, kr->creds);

    kerr = get_and_save_tgt(kr, newpassword);

    sss_authtok_set_empty(kr->pd->newauthtok);

    if (kerr == 0) {
        kerr = k5c_attach_ccname_msg(kr);
    }
    return map_krb5_error(kerr);
}

static errno_t tgt_req_child(struct krb5_req *kr)
{
    krb5_get_init_creds_opt *chagepw_options;
    const char *password = NULL;
    krb5_error_code kerr;
    int ret;

    DEBUG(SSSDBG_TRACE_LIBS, ("Attempting to get a TGT\n"));

    ret = sss_authtok_get_password(kr->pd->authtok, &password, NULL);
    switch (ret) {
        if (ret == EACCES) {
            DEBUG(SSSDBG_OP_FAILURE, ("Invalid authtok type\n"));
            return ERR_INVALID_CRED_TYPE;
        }
        DEBUG(SSSDBG_OP_FAILURE, ("No credentials available\n"));
        return ERR_NO_CREDS;
    }

    kerr = get_and_save_tgt(kr, password);

    if (kerr != KRB5KDC_ERR_KEY_EXP) {
        if (kerr == 0) {
            kerr = k5c_attach_ccname_msg(kr);
        }
        ret = map_krb5_error(kerr);
        goto done;
    }

    /* If the password is expired the KDC will always return
       KRB5KDC_ERR_KEY_EXP regardless if the supplied password is correct or
       not. In general the password can still be used to get a changepw ticket.
       So we validate the password by trying to get a changepw ticket. */
    DEBUG(SSSDBG_TRACE_LIBS, ("Password was expired\n"));
    kerr = sss_krb5_get_init_creds_opt_set_expire_callback(kr->ctx,
                                                           kr->options,
                                                           NULL, NULL);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        DEBUG(1, ("Failed to unset expire callback, continue ...\n"));
    }

    kerr = get_changepw_options(kr->ctx, &chagepw_options);
    if (kerr != 0) {
        DEBUG(SSSDBG_OP_FAILURE, ("get_changepw_options failed.\n"));
        return kerr;
    }

    kerr = krb5_get_init_creds_password(kr->ctx, kr->creds, kr->princ,
                                        discard_const(password),
                                        sss_krb5_prompter, kr, 0,
                                        SSSD_KRB5_CHANGEPW_PRINCIPAL,
                                        chagepw_options);

    sss_krb5_get_init_creds_opt_free(kr->ctx, chagepw_options);

    krb5_free_cred_contents(kr->ctx, kr->creds);
    if (kerr == 0) {
        ret = ERR_CREDS_EXPIRED;
    } else {
        ret = map_krb5_error(kerr);
    }

done:
    sss_authtok_set_empty(kr->pd->authtok);
    return ret;
}

static errno_t kuserok_child(struct krb5_req *kr)
{
    krb5_boolean access_allowed;
    krb5_error_code kerr;

    DEBUG(SSSDBG_TRACE_LIBS, ("Verifying if principal can log in as user\n"));

    /* krb5_kuserok tries to verify that kr->pd->user is a locally known
     * account, so we have to unset _SSS_LOOPS to make getpwnam() work. */
    if (unsetenv("_SSS_LOOPS") != 0) {
        DEBUG(1, ("Failed to unset _SSS_LOOPS, "
                  "krb5_kuserok will most certainly fail.\n"));
    }

    kerr = krb5_set_default_realm(kr->ctx, kr->realm);
    if (kerr != 0) {
        DEBUG(1, ("krb5_set_default_realm failed, "
                  "krb5_kuserok may fail.\n"));
    }

    access_allowed = krb5_kuserok(kr->ctx, kr->princ, kr->pd->user);
    DEBUG(SSSDBG_TRACE_LIBS,
          ("Access was %s\n", access_allowed ? "allowed" : "denied"));

    if (access_allowed) {
        return EOK;
    }

    return ERR_AUTH_DENIED;
}

static errno_t renew_tgt_child(struct krb5_req *kr)
{
    const char *ccname;
    krb5_ccache ccache = NULL;
    krb5_error_code kerr;
    int ret;

    DEBUG(SSSDBG_TRACE_LIBS, ("Renewing a ticket\n"));

    ret = sss_authtok_get_ccfile(kr->pd->authtok, &ccname, NULL);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE,
              ("Unsupported authtok type for TGT renewal [%d].\n",
               sss_authtok_get_type(kr->pd->authtok)));
        return ERR_INVALID_CRED_TYPE;
    }

    kerr = krb5_cc_resolve(kr->ctx, ccname, &ccache);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        goto done;
    }

    kerr = krb5_get_renewed_creds(kr->ctx, kr->creds, kr->princ, ccache, NULL);
    if (kerr != 0) {
        goto done;
    }

    if (kr->validate) {
        kerr = validate_tgt(kr);
        if (kerr != 0) {
            KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
            goto done;
        }

    } else {
        DEBUG(SSSDBG_CONF_SETTINGS, ("TGT validation is disabled.\n"));
    }

    if (kr->validate || kr->fast_ccname != NULL) {
        /* We drop root privileges which were needed to read the keytab file
         * for the validation of the credentials or for FAST here to run the
         * ccache I/O operations with user privileges. */
        kerr = become_user(kr->uid, kr->gid);
        if (kerr != 0) {
            DEBUG(1, ("become_user failed.\n"));
            goto done;
        }
    }

    kerr = krb5_cc_initialize(kr->ctx, ccache, kr->princ);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        goto done;
    }

    kerr = krb5_cc_store_cred(kr->ctx, ccache, kr->creds);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        goto done;
    }

    kerr = add_ticket_times_and_upn_to_response(kr);
    if (kerr != 0) {
        DEBUG(1, ("add_ticket_times_and_upn_to_response failed.\n"));
    }

    kerr = k5c_attach_ccname_msg(kr);

done:
    krb5_free_cred_contents(kr->ctx, kr->creds);

    if (ccache != NULL) {
        krb5_cc_close(kr->ctx, ccache);
    }

    return map_krb5_error(kerr);
}

static errno_t create_empty_ccache(struct krb5_req *kr)
{
    krb5_error_code kerr;

    DEBUG(SSSDBG_TRACE_LIBS, ("Creating empty ccache\n"));

    kerr = create_ccache(kr->uid, kr->gid, kr->ctx,
                         kr->princ, kr->ccname, NULL);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
    } else {
        kerr = k5c_attach_ccname_msg(kr);
    }

    return map_krb5_error(kerr);
}

static errno_t unpack_authtok(TALLOC_CTX *mem_ctx, struct sss_auth_token *tok,
                              uint8_t *buf, size_t size, size_t *p)
{
    uint32_t auth_token_type;
    uint32_t auth_token_length;
    errno_t ret = EOK;

    SAFEALIGN_COPY_UINT32_CHECK(&auth_token_type, buf + *p, size, p);
    SAFEALIGN_COPY_UINT32_CHECK(&auth_token_length, buf + *p, size, p);
    if ((*p + auth_token_length) > size) {
        return EINVAL;
    }
    switch (auth_token_type) {
    case SSS_AUTHTOK_TYPE_EMPTY:
        sss_authtok_set_empty(tok);
        break;
    case SSS_AUTHTOK_TYPE_PASSWORD:
        ret = sss_authtok_set_password(tok, (char *)(buf + *p), 0);
        break;
    case SSS_AUTHTOK_TYPE_CCFILE:
        ret = sss_authtok_set_ccfile(tok, (char *)(buf + *p), 0);
        break;
    default:
        return EINVAL;
    }

    if (ret == EOK) {
        *p += auth_token_length;
    }
    return ret;
}

static errno_t unpack_buffer(uint8_t *buf, size_t size,
                             struct krb5_req *kr, uint32_t *offline)
{
    size_t p = 0;
    uint32_t len;
    uint32_t validate;
    uint32_t different_realm;
    uint32_t use_enterprise_princ;
    struct pam_data *pd;
    errno_t ret;

    DEBUG(SSSDBG_TRACE_LIBS, ("total buffer size: [%d]\n", size));

    if (!offline || !kr) return EINVAL;

    pd = create_pam_data(kr);
    if (pd == NULL) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("talloc_zero failed.\n"));
        return ENOMEM;
    }
    kr->pd = pd;

    SAFEALIGN_COPY_UINT32_CHECK(&pd->cmd, buf + p, size, &p);
    SAFEALIGN_COPY_UINT32_CHECK(&kr->uid, buf + p, size, &p);
    SAFEALIGN_COPY_UINT32_CHECK(&kr->gid, buf + p, size, &p);
    SAFEALIGN_COPY_UINT32_CHECK(&validate, buf + p, size, &p);
    kr->validate = (validate == 0) ? false : true;
    SAFEALIGN_COPY_UINT32_CHECK(offline, buf + p, size, &p);
    SAFEALIGN_COPY_UINT32_CHECK(&different_realm, buf + p, size, &p);
    kr->upn_from_different_realm = (different_realm == 0) ? false : true;
    SAFEALIGN_COPY_UINT32_CHECK(&use_enterprise_princ, buf + p, size, &p);
    kr->use_enterprise_princ = (use_enterprise_princ == 0) ? false : true;
    SAFEALIGN_COPY_UINT32_CHECK(&len, buf + p, size, &p);
    if ((p + len ) > size) return EINVAL;
    kr->upn = talloc_strndup(pd, (char *)(buf + p), len);
    if (kr->upn == NULL) return ENOMEM;
    p += len;

    DEBUG(SSSDBG_CONF_SETTINGS,
          ("cmd [%d] uid [%llu] gid [%llu] validate [%s] "
           "enterprise principal [%s] offline [%s] UPN [%s]\n",
           pd->cmd, (unsigned long long) kr->uid,
           (unsigned long long) kr->gid, kr->validate ? "true" : "false",
           kr->use_enterprise_princ ? "true" : "false",
           *offline ? "true" : "false", kr->upn ? kr->upn : "none"));

    if (pd->cmd == SSS_PAM_AUTHENTICATE ||
        pd->cmd == SSS_CMD_RENEW ||
        pd->cmd == SSS_PAM_CHAUTHTOK_PRELIM || pd->cmd == SSS_PAM_CHAUTHTOK) {
        SAFEALIGN_COPY_UINT32_CHECK(&len, buf + p, size, &p);
        if ((p + len ) > size) return EINVAL;
        kr->ccname = talloc_strndup(pd, (char *)(buf + p), len);
        if (kr->ccname == NULL) return ENOMEM;
        p += len;

        SAFEALIGN_COPY_UINT32_CHECK(&len, buf + p, size, &p);
        if ((p + len ) > size) return EINVAL;
        kr->keytab = talloc_strndup(pd, (char *)(buf + p), len);
        if (kr->keytab == NULL) return ENOMEM;
        p += len;

        ret = unpack_authtok(pd, pd->authtok, buf, size, &p);
        if (ret) {
            return ret;
        }

        DEBUG(SSSDBG_CONF_SETTINGS, ("ccname: [%s] keytab: [%s]\n",
              kr->ccname, kr->keytab));
    } else {
        kr->ccname = NULL;
        kr->keytab = NULL;
        sss_authtok_set_empty(pd->authtok);
    }

    if (pd->cmd == SSS_PAM_CHAUTHTOK) {
        ret = unpack_authtok(pd, pd->newauthtok, buf, size, &p);
        if (ret) {
            return ret;
        }
    } else {
        sss_authtok_set_empty(pd->newauthtok);
    }

    if (pd->cmd == SSS_PAM_ACCT_MGMT) {
        SAFEALIGN_COPY_UINT32_CHECK(&len, buf + p, size, &p);
        if ((p + len ) > size) return EINVAL;
        pd->user = talloc_strndup(pd, (char *)(buf + p), len);
        if (pd->user == NULL) return ENOMEM;
        p += len;
        DEBUG(SSSDBG_CONF_SETTINGS, ("user: [%s]\n", pd->user));
    } else {
        pd->user = NULL;
    }

    return EOK;
}

static int krb5_cleanup(struct krb5_req *kr)
{
    if (kr == NULL) return EOK;

    if (kr->options != NULL) {
        sss_krb5_get_init_creds_opt_free(kr->ctx, kr->options);
    }

    if (kr->creds != NULL) {
        krb5_free_cred_contents(kr->ctx, kr->creds);
        krb5_free_creds(kr->ctx, kr->creds);
    }
    if (kr->name != NULL)
        sss_krb5_free_unparsed_name(kr->ctx, kr->name);
    if (kr->princ != NULL)
        krb5_free_principal(kr->ctx, kr->princ);
    if (kr->ctx != NULL)
        krb5_free_context(kr->ctx);

    memset(kr, 0, sizeof(struct krb5_req));

    return EOK;
}

static krb5_error_code get_tgt_times(krb5_context ctx, const char *ccname,
                                     krb5_principal server_principal,
                                     krb5_principal client_principal,
                                     sss_krb5_ticket_times *tgtt)
{
    krb5_error_code krberr;
    krb5_ccache ccache = NULL;
    krb5_creds mcred;
    krb5_creds cred;

    krberr = krb5_cc_resolve(ctx, ccname, &ccache);
    if (krberr != 0) {
        DEBUG(1, ("krb5_cc_resolve failed.\n"));
        goto done;
    }

    memset(&mcred, 0, sizeof(mcred));
    memset(&cred, 0, sizeof(mcred));

    mcred.server = server_principal;
    mcred.client = client_principal;

    krberr = krb5_cc_retrieve_cred(ctx, ccache, 0, &mcred, &cred);
    if (krberr != 0) {
        DEBUG(1, ("krb5_cc_retrieve_cred failed.\n"));
        krberr = 0;
        goto done;
    }

    tgtt->authtime = cred.times.authtime;
    tgtt->starttime = cred.times.starttime;
    tgtt->endtime = cred.times.endtime;
    tgtt->renew_till = cred.times.renew_till;

    krb5_free_cred_contents(ctx, &cred);

    krberr = 0;

done:
    if (ccache != NULL) {
        krb5_cc_close(ctx, ccache);
    }

    return krberr;
}

static krb5_error_code check_fast_ccache(TALLOC_CTX *mem_ctx,
                                         krb5_context ctx,
                                         const char *primary,
                                         const char *realm,
                                         const char *keytab_name,
                                         char **fast_ccname)
{
    TALLOC_CTX *tmp_ctx = NULL;
    krb5_error_code kerr;
    char *ccname;
    char *server_name;
    sss_krb5_ticket_times tgtt;
    krb5_keytab keytab = NULL;
    krb5_principal client_princ = NULL;
    krb5_principal server_princ = NULL;

    tmp_ctx = talloc_new(NULL);
    if (tmp_ctx == NULL) {
        DEBUG(1, ("talloc_new failed.\n"));
        kerr = ENOMEM;
        goto done;
    }

    ccname = talloc_asprintf(tmp_ctx, "FILE:%s/fast_ccache_%s", DB_PATH, realm);
    if (ccname == NULL) {
        DEBUG(1, ("talloc_asprintf failed.\n"));
        kerr = ENOMEM;
        goto done;
    }

    if (keytab_name != NULL) {
        kerr = krb5_kt_resolve(ctx, keytab_name, &keytab);
    } else {
        kerr = krb5_kt_default(ctx, &keytab);
    }
    if (kerr) {
        DEBUG(SSSDBG_FATAL_FAILURE,
              ("Failed to read keytab file [%s]: %s\n",
               KEYTAB_CLEAN_NAME,
               sss_krb5_get_error_message(ctx, kerr)));
        goto done;
    }

    kerr = find_principal_in_keytab(ctx, keytab, primary, realm, &client_princ);
    if (kerr != 0) {
        DEBUG(SSSDBG_MINOR_FAILURE,
              ("find_principal_in_keytab failed for principal %s@%s.\n",
               primary, realm));
        goto done;
    }

    server_name = talloc_asprintf(tmp_ctx, "krbtgt/%s@%s", realm, realm);
    if (server_name == NULL) {
        DEBUG(1, ("talloc_asprintf failed.\n"));
        kerr = ENOMEM;
        goto done;
    }

    kerr = krb5_parse_name(ctx, server_name, &server_princ);
    if (kerr != 0) {
        DEBUG(1, ("krb5_parse_name failed.\n"));
        goto done;
    }

    memset(&tgtt, 0, sizeof(tgtt));
    kerr = get_tgt_times(ctx, ccname, server_princ, client_princ, &tgtt);
    if (kerr == 0) {
        if (tgtt.endtime > time(NULL)) {
            DEBUG(5, ("FAST TGT is still valid.\n"));
            goto done;
        }
    }

    kerr = get_and_save_tgt_with_keytab(ctx, client_princ, keytab, ccname);
    if (kerr != 0) {
        DEBUG(1, ("get_and_save_tgt_with_keytab failed.\n"));
        goto done;
    }

    kerr = 0;

done:
    if (client_princ != NULL) {
        krb5_free_principal(ctx, client_princ);
    }
    if (server_princ != NULL) {
        krb5_free_principal(ctx, server_princ);
    }

    if (kerr == 0) {
        *fast_ccname = talloc_steal(mem_ctx, ccname);
    }
    talloc_free(tmp_ctx);

    if (keytab != NULL) {
        krb5_kt_close(ctx, keytab);
    }

    return kerr;
}

static errno_t k5c_recv_data(struct krb5_req *kr, int fd, uint32_t *offline)
{
    uint8_t buf[IN_BUF_SIZE];
    ssize_t len;
    errno_t ret;

    errno = 0;
    len = sss_atomic_read_s(fd, buf, IN_BUF_SIZE);
    if (len == -1) {
        ret = errno;
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("read failed [%d][%s].\n", ret, strerror(ret)));
        return ret;
    }

    ret = unpack_buffer(buf, len, kr, offline);
    if (ret != EOK) {
        DEBUG(1, ("unpack_buffer failed.\n"));
    }

    return ret;
}

static int k5c_setup_fast(struct krb5_req *kr, char *lifetime_str, bool demand)
{
    krb5_principal fast_princ_struct;
    krb5_data *realm_data;
    char *fast_principal_realm;
    char *fast_principal;
    krb5_error_code kerr;
    char *tmp_str;

    DEBUG(SSSDBG_CONF_SETTINGS, ("%s is set to [%s]\n",
                                 SSSD_KRB5_LIFETIME, lifetime_str));

    tmp_str = getenv(SSSD_KRB5_FAST_PRINCIPAL);
    if (tmp_str) {
        DEBUG(SSSDBG_CONF_SETTINGS, ("%s is set to [%s]\n",
                                     SSSD_KRB5_FAST_PRINCIPAL, tmp_str));
        kerr = krb5_parse_name(kr->ctx, tmp_str, &fast_princ_struct);
        if (kerr) {
            DEBUG(1, ("krb5_parse_name failed.\n"));
            return kerr;
        }
        kerr = sss_krb5_unparse_name_flags(kr->ctx, fast_princ_struct,
                                       KRB5_PRINCIPAL_UNPARSE_NO_REALM,
                                       &tmp_str);
        if (kerr) {
            DEBUG(1, ("sss_krb5_unparse_name_flags failed.\n"));
            return kerr;
        }
        fast_principal = talloc_strdup(kr, tmp_str);
        if (!fast_principal) {
            DEBUG(1, ("talloc_strdup failed.\n"));
            return KRB5KRB_ERR_GENERIC;
        }
        free(tmp_str);
        realm_data = krb5_princ_realm(kr->ctx, fast_princ_struct);
        fast_principal_realm = talloc_asprintf(kr, "%.*s", realm_data->length, realm_data->data);
        if (!fast_principal_realm) {
            DEBUG(1, ("talloc_asprintf failed.\n"));
            return ENOMEM;
        }
    } else {
        fast_principal_realm = kr->realm;
        fast_principal = NULL;
    }

    kerr = check_fast_ccache(kr, kr->ctx, fast_principal, fast_principal_realm,
                             kr->keytab, &kr->fast_ccname);
    if (kerr != 0) {
        DEBUG(1, ("check_fast_ccache failed.\n"));
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        return kerr;
    }

    kerr = sss_krb5_get_init_creds_opt_set_fast_ccache_name(kr->ctx,
                                                            kr->options,
                                                            kr->fast_ccname);
    if (kerr != 0) {
        DEBUG(1, ("sss_krb5_get_init_creds_opt_set_fast_ccache_name "
                  "failed.\n"));
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        return kerr;
    }

    if (demand) {
        kerr = sss_krb5_get_init_creds_opt_set_fast_flags(kr->ctx,
                                                kr->options,
                                                SSS_KRB5_FAST_REQUIRED);
        if (kerr != 0) {
            DEBUG(1, ("sss_krb5_get_init_creds_opt_set_fast_flags "
                      "failed.\n"));
            KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
            return kerr;
        }
    }

    return EOK;
}

static int k5c_setup(struct krb5_req *kr, uint32_t offline)
{
    krb5_error_code kerr;
    char *lifetime_str;
    char *use_fast_str;
    krb5_deltat lifetime;
    int parse_flags;

    kr->realm = getenv(SSSD_KRB5_REALM);
    if (kr->realm == NULL) {
        DEBUG(SSSDBG_MINOR_FAILURE,
              ("Cannot read [%s] from environment.\n", SSSD_KRB5_REALM));
    }

    kerr = krb5_init_context(&kr->ctx);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        return kerr;
    }

    /* Set the global error context */
    krb5_error_ctx = kr->ctx;

    if (debug_level & SSSDBG_TRACE_ALL) {
        kerr = sss_child_set_krb5_tracing(kr->ctx);
        if (kerr) {
            KRB5_CHILD_DEBUG(SSSDBG_MINOR_FAILURE, kerr);
            return EIO;
        }
    }

    parse_flags = kr->use_enterprise_princ ? KRB5_PRINCIPAL_PARSE_ENTERPRISE : 0;
    kerr = sss_krb5_parse_name_flags(kr->ctx, kr->upn, parse_flags, &kr->princ);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        return kerr;
    }

    kerr = krb5_unparse_name(kr->ctx, kr->princ, &kr->name);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        return kerr;
    }

    kr->creds = calloc(1, sizeof(krb5_creds));
    if (kr->creds == NULL) {
        DEBUG(1, ("talloc_zero failed.\n"));
        return ENOMEM;
    }

    kerr = sss_krb5_get_init_creds_opt_alloc(kr->ctx, &kr->options);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        return kerr;
    }

#ifdef HAVE_KRB5_GET_INIT_CREDS_OPT_SET_RESPONDER
    kerr = krb5_get_init_creds_opt_set_responder(kr->ctx, kr->options,
                                                 sss_krb5_responder, kr);
    if (kerr != 0) {
        KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
        return kerr;
    }
#endif

#ifdef HAVE_KRB5_GET_INIT_CREDS_OPT_SET_CHANGE_PASSWORD_PROMPT
    /* A prompter is used to catch messages about when a password will
     * expired. The library shall not use the prompter to ask for a new password
     * but shall return KRB5KDC_ERR_KEY_EXP. */
    krb5_get_init_creds_opt_set_change_password_prompt(kr->options, 0);
#endif

    lifetime_str = getenv(SSSD_KRB5_RENEWABLE_LIFETIME);
    if (lifetime_str == NULL) {
        DEBUG(SSSDBG_CONF_SETTINGS, ("Cannot read [%s] from environment.\n",
              SSSD_KRB5_RENEWABLE_LIFETIME));
    } else {
        kerr = krb5_string_to_deltat(lifetime_str, &lifetime);
        if (kerr != 0) {
            DEBUG(1, ("krb5_string_to_deltat failed for [%s].\n",
                      lifetime_str));
            KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
            return kerr;
        }
        DEBUG(SSSDBG_CONF_SETTINGS, ("%s is set to [%s]\n",
              SSSD_KRB5_RENEWABLE_LIFETIME, lifetime_str));
        krb5_get_init_creds_opt_set_renew_life(kr->options, lifetime);
    }

    lifetime_str = getenv(SSSD_KRB5_LIFETIME);
    if (lifetime_str == NULL) {
        DEBUG(SSSDBG_CONF_SETTINGS, ("Cannot read [%s] from environment.\n",
              SSSD_KRB5_LIFETIME));
    } else {
        kerr = krb5_string_to_deltat(lifetime_str, &lifetime);
        if (kerr != 0) {
            DEBUG(1, ("krb5_string_to_deltat failed for [%s].\n",
                      lifetime_str));
            KRB5_CHILD_DEBUG(SSSDBG_CRIT_FAILURE, kerr);
            return kerr;
        }
        DEBUG(SSSDBG_CONF_SETTINGS,
              ("%s is set to [%s]\n", SSSD_KRB5_LIFETIME, lifetime_str));
        krb5_get_init_creds_opt_set_tkt_life(kr->options, lifetime);
    }

    if (!offline) {
        krb5_set_canonicalize(kr->options);

        use_fast_str = getenv(SSSD_KRB5_USE_FAST);
        if (use_fast_str == NULL || strcasecmp(use_fast_str, "never") == 0) {
            DEBUG(SSSDBG_CONF_SETTINGS, ("Not using FAST.\n"));
        } else if (strcasecmp(use_fast_str, "try") == 0) {
            kerr = k5c_setup_fast(kr, lifetime_str, false);
        } else if (strcasecmp(use_fast_str, "demand") == 0) {
            kerr = k5c_setup_fast(kr, lifetime_str, true);
        } else {
            DEBUG(SSSDBG_CRIT_FAILURE,
                  ("Unsupported value [%s] for krb5_use_fast.\n",
                   use_fast_str));
            return EINVAL;
        }
    }

/* TODO: set options, e.g.
 *  krb5_get_init_creds_opt_set_forwardable
 *  krb5_get_init_creds_opt_set_proxiable
 *  krb5_get_init_creds_opt_set_etype_list
 *  krb5_get_init_creds_opt_set_address_list
 *  krb5_get_init_creds_opt_set_preauth_list
 *  krb5_get_init_creds_opt_set_salt
 *  krb5_get_init_creds_opt_set_change_password_prompt
 *  krb5_get_init_creds_opt_set_pa
 */

    return kerr;
}

int main(int argc, const char *argv[])
{
    struct krb5_req *kr = NULL;
    uint32_t offline;
    int opt;
    poptContext pc;
    int debug_fd = -1;
    errno_t ret;

    struct poptOption long_options[] = {
        POPT_AUTOHELP
        {"debug-level", 'd', POPT_ARG_INT, &debug_level, 0,
         _("Debug level"), NULL},
        {"debug-timestamps", 0, POPT_ARG_INT, &debug_timestamps, 0,
         _("Add debug timestamps"), NULL},
        {"debug-microseconds", 0, POPT_ARG_INT, &debug_microseconds, 0,
         _("Show timestamps with microseconds"), NULL},
        {"debug-fd", 0, POPT_ARG_INT, &debug_fd, 0,
         _("An open file descriptor for the debug logs"), NULL},
        POPT_TABLEEND
    };

    /* Set debug level to invalid value so we can decide if -d 0 was used. */
    debug_level = SSSDBG_INVALID;

    pc = poptGetContext(argv[0], argc, argv, long_options, 0);
    while((opt = poptGetNextOpt(pc)) != -1) {
        switch(opt) {
        default:
        fprintf(stderr, "\nInvalid option %s: %s\n\n",
                  poptBadOption(pc, 0), poptStrerror(opt));
            poptPrintUsage(pc, stderr, 0);
            _exit(-1);
        }
    }

    poptFreeContext(pc);

    DEBUG_INIT(debug_level);

    kr = talloc_zero(NULL, struct krb5_req);
    if (kr == NULL) {
        DEBUG(1, ("talloc failed.\n"));
        exit(-1);
    }

    debug_prg_name = talloc_asprintf(kr, "[sssd[krb5_child[%d]]]", getpid());
    if (!debug_prg_name) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("talloc_asprintf failed.\n"));
        ret = ENOMEM;
        goto done;
    }

    if (debug_fd != -1) {
        ret = set_debug_file_from_fd(debug_fd);
        if (ret != EOK) {
            DEBUG(SSSDBG_CRIT_FAILURE, ("set_debug_file_from_fd failed.\n"));
        }
    }

    DEBUG(SSSDBG_TRACE_FUNC, ("krb5_child started.\n"));

    ret = k5c_recv_data(kr, STDIN_FILENO, &offline);
    if (ret != EOK) {
        goto done;
    }

    close(STDIN_FILENO);

    ret = k5c_setup(kr, offline);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("krb5_child_setup failed.\n"));
        goto done;
    }

    switch(kr->pd->cmd) {
    case SSS_PAM_AUTHENTICATE:
        /* If we are offline, we need to create an empty ccache file */
        if (offline) {
            DEBUG(SSSDBG_TRACE_FUNC, ("Will perform offline auth\n"));
            ret = create_empty_ccache(kr);
        } else {
            DEBUG(SSSDBG_TRACE_FUNC, ("Will perform online auth\n"));
            ret = tgt_req_child(kr);
        }
        break;
    case SSS_PAM_CHAUTHTOK:
        DEBUG(SSSDBG_TRACE_FUNC, ("Will perform password change\n"));
        ret = changepw_child(kr, false);
        break;
    case SSS_PAM_CHAUTHTOK_PRELIM:
        DEBUG(SSSDBG_TRACE_FUNC, ("Will perform password change checks\n"));
        ret = changepw_child(kr, true);
        break;
    case SSS_PAM_ACCT_MGMT:
        DEBUG(SSSDBG_TRACE_FUNC, ("Will perform account management\n"));
        ret = kuserok_child(kr);
        break;
    case SSS_CMD_RENEW:
        if (offline) {
            DEBUG(SSSDBG_CRIT_FAILURE, ("Cannot renew TGT while offline\n"));
            ret = KRB5_KDC_UNREACH;
            goto done;
        }
        DEBUG(SSSDBG_TRACE_FUNC, ("Will perform ticket renewal\n"));
        ret = renew_tgt_child(kr);
        break;
    default:
        DEBUG(1, ("PAM command [%d] not supported.\n", kr->pd->cmd));
        ret = EINVAL;
        goto done;
    }

    ret = k5c_send_data(kr, STDOUT_FILENO, ret);
    if (ret != EOK) {
        DEBUG(SSSDBG_CRIT_FAILURE, ("Failed to send reply\n"));
    }

done:
    krb5_cleanup(kr);
    talloc_free(kr);
    if (ret == EOK) {
        DEBUG(SSSDBG_TRACE_FUNC, ("krb5_child completed successfully\n"));
        exit(0);
    } else {
        DEBUG(SSSDBG_CRIT_FAILURE, ("krb5_child failed!\n"));
        exit(-1);
    }
}
