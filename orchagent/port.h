#ifndef SWSS_PORT_H
#define SWSS_PORT_H

extern "C" {
#include "sai.h"
}

#include <set>
#include <string>
#include <vector>
#include <map>
#include <bitset>
#include <unordered_set>

#define DEFAULT_PORT_VLAN_ID    1
/*
 * Default MTU is derived from SAI_PORT_ATTR_MTU (1514)
 * Orchagent adds extra 22 bytes for Ethernet header and FCS,
 * hence setting to 1492 (1514 - 22)
 */
#define DEFAULT_MTU             1492

/*
 * Default TPID is 8100
 * User can configure other values such as 9100, 9200, or 88A8
 */
#define DEFAULT_TPID             0x8100

#define VNID_NONE               0xFFFFFFFF

namespace swss {

struct VlanMemberEntry
{
    sai_object_id_t            vlan_member_id;
    sai_vlan_tagging_mode_t    vlan_mode;
};

typedef std::map<sai_vlan_id_t, VlanMemberEntry> vlan_members_t;

typedef std::map<std::string, sai_object_id_t> endpoint_ip_l2mc_group_member_map_t;

struct VlanInfo
{
    sai_object_id_t     vlan_oid = 0;
    sai_vlan_id_t       vlan_id = 0;
    sai_object_id_t     host_intf_id = SAI_NULL_OBJECT_ID;
    sai_vlan_flood_control_type_t uuc_flood_type = SAI_VLAN_FLOOD_CONTROL_TYPE_ALL;
    sai_vlan_flood_control_type_t bc_flood_type = SAI_VLAN_FLOOD_CONTROL_TYPE_ALL;
    sai_object_id_t    l2mc_group_id = SAI_NULL_OBJECT_ID;
    endpoint_ip_l2mc_group_member_map_t l2mc_members;
};

struct SystemPortInfo
{
    std::string alias = "";
    sai_system_port_type_t type = SAI_SYSTEM_PORT_TYPE_LOCAL;
    sai_object_id_t local_port_oid = 0;
    uint32_t port_id = 0;
    uint32_t switch_id = 0;
    uint32_t core_index = 0;
    uint32_t core_port_index = 0;
    uint32_t speed = 400000;
    uint32_t num_voq = 8;
};

struct SystemLagInfo
{
    std::string alias = "";
    int32_t switch_id = -1;
    int32_t spa_id = 0;
};

class Port
{
public:
    enum Type {
        CPU,
        PHY,
        MGMT,
        LOOPBACK,
        VLAN,
        LAG,
        TUNNEL,
        SUBPORT,
        SYSTEM,
        UNKNOWN
    } ;

    Port() {};
    Port(std::string alias, Type type) :
            m_alias(alias), m_type(type) {};

    inline bool operator<(const Port &o) const
    {
        return m_alias < o.m_alias;
    }

    inline bool operator==(const Port &o) const
    {
        return m_alias == o.m_alias;
    }

    inline bool operator!=(const Port &o) const
    {
        return !(*this == o);
    }

    std::string         m_alias;
    Type                m_type;
    int                 m_index = 0;    // PHY_PORT: index
    uint32_t            m_mtu = DEFAULT_MTU;
    uint32_t            m_speed = 0;    // Mbps
    std::string         m_learn_mode = "hardware";
    int                 m_autoneg = -1;  // -1 means not set, 0 = disabled, 1 = enabled
    bool                m_admin_state_up = false;
    bool                m_init = false;
    bool                m_l3_vni = false;
    sai_object_id_t     m_port_id = 0;
    sai_port_fec_mode_t m_fec_mode = SAI_PORT_FEC_MODE_NONE;
    VlanInfo            m_vlan_info;
    MacAddress          m_mac;
    sai_object_id_t     m_bridge_port_id = 0;   // TODO: port could have multiple bridge port IDs
    sai_object_id_t     m_bridge_port_admin_state = 0;   // TODO: port could have multiple bridge port IDs
    sai_vlan_id_t       m_port_vlan_id = DEFAULT_PORT_VLAN_ID;  // Port VLAN ID
    sai_object_id_t     m_rif_id = 0;
    sai_object_id_t     m_vr_id = 0;
    sai_object_id_t     m_hif_id = 0;
    sai_object_id_t     m_lag_id = 0;
    sai_object_id_t     m_lag_member_id = 0;
    sai_object_id_t     m_tunnel_id = 0;
    sai_object_id_t     m_ingress_acl_table_group_id = 0;
    sai_object_id_t     m_egress_acl_table_group_id = 0;
    sai_object_id_t     m_parent_port_id = 0;
    uint32_t            m_dependency_bitmap = 0;
    sai_port_oper_status_t m_oper_status = SAI_PORT_OPER_STATUS_UNKNOWN;
    std::set<std::string> m_members;
    std::set<std::string> m_child_ports;
    std::vector<sai_object_id_t> m_queue_ids;
    std::vector<sai_object_id_t> m_priority_group_ids;
    sai_port_priority_flow_control_mode_t m_pfc_asym = SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_COMBINED;
    uint8_t   m_pfc_bitmask = 0;        // PFC enable bit mask
    uint8_t   m_pfcwd_sw_bitmask = 0;   // PFC software watchdog enable
    uint16_t  m_tpid = DEFAULT_TPID;
    uint32_t  m_nat_zone_id = 0;
    uint32_t  m_vnid = VNID_NONE;
    uint32_t  m_fdb_count = 0;
    uint32_t  m_up_member_count = 0;
    uint32_t  m_maximum_headroom = 0;
    std::vector<uint32_t> m_adv_speeds;
    sai_port_interface_type_t m_interface_type;
    std::vector<uint32_t> m_adv_interface_types;
    bool      m_mpls = false;

    /*
     * Following two bit vectors are used to lock
     * the PG/queue from being changed in BufferOrch.
     * The use case scenario is when PfcWdZeroBufferHandler
     * sets zero buffer profile it should protect PG/queue
     * from being overwritten in BufferOrch.
     */
    std::vector<bool> m_queue_lock;
    std::vector<bool> m_priority_group_lock;
    std::vector<sai_object_id_t> m_priority_group_pending_profile;

    std::unordered_set<sai_object_id_t> m_ingress_acl_tables_uset;
    std::unordered_set<sai_object_id_t> m_egress_acl_tables_uset;

    sai_object_id_t  m_system_port_oid = 0;
    SystemPortInfo   m_system_port_info;
    SystemLagInfo    m_system_lag_info;

    sai_object_id_t  m_switch_id = 0;
    sai_object_id_t  m_line_side_id = 0;

    bool m_fec_cfg = false;
    bool m_an_cfg = false;
};

}

#endif /* SWSS_PORT_H */
