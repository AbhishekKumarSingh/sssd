/*
   SSSD

   PAC Responder, header file

   Copyright (C) Sumit Bose <sbose@redhat.com> 2011

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

#ifndef __PACSRV_H__
#define __PACSRV_H__
#include <stdbool.h>
#include <util/data_blob.h>
#include <ndr.h>
#include <gen_ndr/krb5pac.h>
#include <gen_ndr/ndr_krb5pac.h>

#include <stdint.h>
#include <sys/un.h>
#include "config.h"
#include "talloc.h"
#include "tevent.h"
#include "ldb.h"
#include "dbus/dbus.h"
#include "sbus/sssd_dbus.h"
#include "responder/common/responder_packet.h"
#include "responder/common/responder.h"
#include "responder/common/responder_sbus.h"
#include "lib/idmap/sss_idmap.h"
#include "util/sss_nss.h"
#include "db/sysdb.h"

#define PAC_PACKET_MAX_RECV_SIZE 1024

struct getent_ctx;
struct dom_sid;

struct pac_ctx {
    struct resp_ctx *rctx;
    struct sss_idmap_ctx *idmap_ctx;
    struct dom_sid *my_dom_sid;
    struct local_mapping_ranges *range_map;
};

struct range {
    uint32_t min;
    uint32_t max;
};

struct local_mapping_ranges {
    struct range local_ids;
    struct range primary_rids;
    struct range secondary_rids;
};

struct grp_info {
    gid_t gid;
    char *orig_dn;
    struct ldb_dn *dn;
};

struct pac_dom_grps {
    struct sss_domain_info *grp_dom;
    size_t gid_count;
    gid_t *gids;
};

struct sss_cmd_table *get_pac_cmds(void);

errno_t local_sid_to_id(struct local_mapping_ranges *map, struct dom_sid *sid,
                        uint32_t *id);

errno_t add_idmap_domain(struct sss_idmap_ctx *idmap_ctx,
                         struct sysdb_ctx *sysdb,
                         const char *domain_name,
                         const char *dom_sid_str);

errno_t domsid_rid_to_uid(struct pac_ctx *pac_ctx,
                          struct sysdb_ctx *sysdb,
                          const char *domain_name,
                          struct dom_sid2 *domsid, uint32_t rid,
                          uid_t *uid);

errno_t get_parent_domain_data(struct pac_ctx *pac_ctx,
                               struct sss_domain_info *dom,
                               struct dom_sid **_sid,
                               struct local_mapping_ranges **_range_map);

errno_t get_gids_from_pac(TALLOC_CTX *mem_ctx,
                          struct pac_ctx *pac_ctx,
                          struct local_mapping_ranges *range_map,
                          struct dom_sid *domain_sid,
                          struct PAC_LOGON_INFO *logon_info,
                          size_t *_gid_count, struct pac_dom_grps **_gids);

errno_t get_data_from_pac(TALLOC_CTX *mem_ctx,
                          uint8_t *pac_blob, size_t pac_len,
                          struct PAC_LOGON_INFO **_logon_info);

errno_t get_pwd_from_pac(TALLOC_CTX *mem_ctx,
                         struct pac_ctx *pac_ctx,
                         struct sss_domain_info *dom,
                         struct PAC_LOGON_INFO *logon_info,
                         struct passwd **_pwd,
                         struct sysdb_attrs **_attrs);

errno_t diff_gid_lists(TALLOC_CTX *mem_ctx,
                       size_t cur_grp_num,
                       struct grp_info *cur_gid_list,
                       size_t new_gid_num,
                       struct pac_dom_grps *new_gid_list,
                       size_t *_add_gid_num,
                       struct pac_dom_grps **_add_gid_list,
                       size_t *_del_gid_num,
                       struct grp_info ***_del_gid_list);

struct sss_domain_info *find_domain_by_id(struct sss_domain_info *domains,
                                          const char *id_str);

bool new_and_cached_user_differs(struct passwd *pwd, struct ldb_message *msg);
#endif /* __PACSRV_H__ */
