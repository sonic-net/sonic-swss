/*
 * Public definitions pertaining to the Forwarding Plane Manager component.
 *
 * Permission is granted to use, copy, modify and/or distribute this
 * software under either one of the licenses below.
 *
 * Note that if you use other files from the Quagga tree directly or
 * indirectly, then the licenses in those files still apply.
 *
 * Please retain both licenses below when modifying this code in the
 * Quagga tree.
 *
 * Copyright (C) 2012 by Open Source Routing.
 * Copyright (C) 2012 by Internet Systems Consortium, Inc. ("ISC")
 */

/*
 * License Option 1: GPL
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

/*
 * License Option 2: ISC License
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef SONIC_FPM_PROTECTION_H
#define SONIC_FPM_PROTECTION_H

#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>
#include <linux/rtnetlink.h>

/* FPM-specific extension values, for messages and attributes */

typedef enum {
  /*
  * Ideally, this should be set to (RTM_MAX + 1) but as
  * FRR and SWSS build with slightly different kernel headers
  * RTM_MAX differs between them...
  */
  RTM_FPM_FIRST = 140,
  /* Ask FPM server to send encap info */
  RTM_FPM_GETENCAPS = RTM_FPM_FIRST,
  /* Notification about new or updated encap */
  RTM_FPM_ADDENCAP,
  /* Notification about deleted encap */
  RTM_FPM_DELENCAP,
  /* Notify FPM about a new Split Horizon List for a given port */
  RTM_FPM_ADD_EVPN_SHL,
  /* Notify FPM about a Split Horizon List is being deleted from a given port */
  RTM_FPM_DEL_EVPN_SHL,
  /* Notify FPM about an add/update of DF role information for a given port */
  RTM_FPM_ADD_EVPN_DF,
  /* Notify FPM about a delete of DF role information */
  RTM_FPM_DEL_EVPN_DF,
  /* Notify FPM about a backup NHG for a port */
  RTM_FPM_ADD_EVPN_ES_BACKUP_NHG,
  /* Notify FPM about a delete of a backup NHG */
  RTM_FPM_DEL_EVPN_ES_BACKUP_NHG,
  RTM_FPM_ADD_L2_NEXTHOP = 149,
  RTM_FPM_DEL_L2_NEXTHOP = 150,
  RTM_FPM_ADD_L2_NEXTHOP_GROUP = 151,
  RTM_FPM_DEL_L2_NEXTHOP_GROUP = 152,
  /* End of the producer's protection replay; no payload or activation gate. */
  RTM_FPM_EVPN_MH_REPLAY_DONE = 153,
  RTM_FPM_LAST = RTM_FPM_EVPN_MH_REPLAY_DONE,
  RTM_FPM_MAX = RTM_FPM_LAST
} rtm_fpm_msg_types_et;

struct evpn_shl_msg {
  int esm_ifindex;
  uint16_t esm_vid;
};

#define FPM_EVPN_SHL_RTA(r) ((struct rtattr *)(void *)(((char *)(r)) + NLMSG_ALIGN(sizeof(struct evpn_shl_msg))))

struct evpn_df_msg {
  int edm_ifindex;
  uint16_t edm_vid;
  int edm_non_df;
};


/* Shared SONiC/FRR contract. Coordinate these assignments before deployment.
 * Kernel L2 objects retain their existing tags. Protection uses the tags in
 * the SONiC HLD examples; producers supply the complete, already-tagged ID.
 * See https://github.com/sonic-net/SONiC/pull/2556 (identifier ownership).
 */
#ifndef NHG_ID_TYPE_POS
#define NHG_ID_TYPE_POS 28
#endif
#define NHG_ID_TYPE_MASK (UINT32_C(0xf) << NHG_ID_TYPE_POS)
#define NHG_ID_VALUE_MASK UINT32_C(0x0fffffff)
enum fpm_nhg_type {
  /* Namespaced aliases avoid redeclaring FRR's existing enum nhg_type. */
  FPM_NHG_TYPE_KERNEL_L2_NH = 1, /* NHG_TYPE_L2_NH; kernel netlink only */
  FPM_NHG_TYPE_KERNEL_L2 = 2,    /* NHG_TYPE_L2; kernel netlink only */
  NHG_TYPE_L3_BPP_NH = 4,   /* FPM protection Member only */
  NHG_TYPE_L3_BPP = 8,      /* FPM protection Group and Binding only */
};
#define NHG_ID_TYPE(id) (((uint32_t)(id) & NHG_ID_TYPE_MASK) >> NHG_ID_TYPE_POS)
#define L2_NHG_MAX_MEMBERS 4096u
#define L2_NHG_ESI_LEN 10u
struct __attribute__((packed)) l2_nhg_member_msg {
  uint32_t l2nm_nh_id;
  uint8_t l2nm_type;
  uint8_t l2nm_family;
  union { struct in_addr ip4; struct in6_addr ip6; } l2nm_remote_vtep;
  uint8_t l2nm_router_mac[6];
};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpacked"
struct __attribute__((packed)) l2_nhg_group_msg {
  uint32_t l2ng_nhg_id;
  uint32_t l2ng_count;
  /* Opaque ES identity; it does not replace the Group ID as the DB key. */
  uint8_t l2ng_esi[L2_NHG_ESI_LEN];
  struct l2_nhg_member_msg l2ng_members[];
};
#pragma GCC diagnostic pop
#ifdef __cplusplus
static_assert(NHG_ID_TYPE_POS == 28 && NHG_ID_TYPE_MASK == UINT32_C(0xf0000000), "FPM ID type layout");
static_assert(sizeof(l2_nhg_member_msg) == 28 &&
              offsetof(l2_nhg_member_msg, l2nm_type) == 4 &&
              offsetof(l2_nhg_member_msg, l2nm_family) == 5 &&
              offsetof(l2_nhg_member_msg, l2nm_remote_vtep) == 6 &&
              offsetof(l2_nhg_member_msg, l2nm_router_mac) == 22, "FPM member layout");
static_assert(sizeof(l2_nhg_group_msg) == 18 &&
              offsetof(l2_nhg_group_msg, l2ng_count) == 4 &&
              offsetof(l2_nhg_group_msg, l2ng_esi) == 8 &&
              offsetof(l2_nhg_group_msg, l2ng_members) == 18, "FPM group layout");
#else
_Static_assert(NHG_ID_TYPE_POS == 28 && NHG_ID_TYPE_MASK == UINT32_C(0xf0000000), "FPM ID type layout");
_Static_assert(sizeof(struct l2_nhg_member_msg) == 28 &&
               offsetof(struct l2_nhg_member_msg, l2nm_type) == 4 &&
               offsetof(struct l2_nhg_member_msg, l2nm_family) == 5 &&
               offsetof(struct l2_nhg_member_msg, l2nm_remote_vtep) == 6 &&
               offsetof(struct l2_nhg_member_msg, l2nm_router_mac) == 22, "FPM member layout");
_Static_assert(sizeof(struct l2_nhg_group_msg) == 18 &&
               offsetof(struct l2_nhg_group_msg, l2ng_count) == 4 &&
               offsetof(struct l2_nhg_group_msg, l2ng_esi) == 8 &&
               offsetof(struct l2_nhg_group_msg, l2ng_members) == 18, "FPM group layout");
#endif

struct evpn_backup_nhg_msg {
  int ebnm_ifindex;
  uint32_t ebnm_backup_nhg_id;
};

/* Signed interface index, unsigned NHG ID; no packing/endian change. */
#ifdef __cplusplus
static_assert(sizeof(int) == 4 && sizeof(evpn_backup_nhg_msg) == 8,
              "FPM binding ABI requires two native 32-bit integers");
static_assert(RTM_FPM_ADD_EVPN_ES_BACKUP_NHG == 147 && RTM_FPM_DEL_EVPN_ES_BACKUP_NHG == 148,
              "FPM binding message types must remain fixed");
#else
_Static_assert(sizeof(int) == 4 && sizeof(struct evpn_backup_nhg_msg) == 8,
               "FPM binding ABI requires two native 32-bit integers");
_Static_assert(RTM_FPM_ADD_EVPN_ES_BACKUP_NHG == 147 && RTM_FPM_DEL_EVPN_ES_BACKUP_NHG == 148,
               "FPM binding message types must remain fixed");
#endif

/* Attribute IDs for SHL Updates */
#define FPM_SHL_IPV4_ADDR   1
#define FPM_SHL_IPV6_ADDR   2
#define FPM_SHL_LAST    FPM_SHL_IPV6_ADDR

#define FPM_SHL_MAX     FPM_SHL_LAST

/* End of FPM extensions */

#endif /* SONIC_FPM_PROTECTION_H */
