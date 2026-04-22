/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Supplemental netlink neighbour definitions for constants not yet
 * available in older system headers. Include system <linux/neighbour.h>
 * first, then this file to get missing definitions.
 */
#ifndef __FDBSYNCD_NEIGHBOUR_COMPAT_H
#define __FDBSYNCD_NEIGHBOUR_COMPAT_H

#include <linux/neighbour.h>

/* NDA_NH_ID attribute added in Linux kernel 5.3 
 * Define only if not already present in system header
 */
#ifndef NDA_NH_ID
#define NDA_NH_ID 13
#endif

/* NDA_PROTOCOL attribute added in Linux kernel 5.2
 * Define only if not already present in system header
 */
#ifndef NDA_PROTOCOL
#define NDA_PROTOCOL 12
#endif

/* NDA_FLAGS_EXT attribute added in Linux kernel 5.1
 * Define only if not already present in system header
 */
#ifndef NDA_FLAGS_EXT
#define NDA_FLAGS_EXT 15
#endif

/* Extended flags under NDA_FLAGS_EXT, added in Linux kernel 5.11
 * Define only if not already present in system header
 */
#ifndef NTF_EXT_MH_PEER_SYNC
#define NTF_EXT_MH_PEER_SYNC (1 << 2)
#endif

#ifndef NTF_EXT_REMOTE_ONLY
#define NTF_EXT_REMOTE_ONLY (1 << 3)
#endif

#endif /* __FDBSYNCD_NEIGHBOUR_COMPAT_H */
