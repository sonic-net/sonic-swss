#pragma once

#include <cstdint>

/*
 * IFLA_GROUP values used by swss daemons to tag netdevs they own, enabling
 * bulk cleanup via `ip link delete group <N> type <kind>`.
 *
 * swss reserves [SWSS_BASE, SWSS_BASE + 0xFF]. Allocate a new low byte
 * here — do not reuse an assigned value.
 */

namespace swss {

constexpr uint32_t NETLINK_GROUP_SWSS_BASE = 0x534F4E00u;

constexpr uint32_t NETLINK_GROUP_VXLAN_MGR = NETLINK_GROUP_SWSS_BASE | 0x01u; // vxlanmgr
constexpr uint32_t NETLINK_GROUP_VRF_MGR   = NETLINK_GROUP_SWSS_BASE | 0x02u; // vrfmgr

} // namespace swss
