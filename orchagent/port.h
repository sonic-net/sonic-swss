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

namespace swss {

struct VlanMemberEntry
{
    sai_object_id_t            vlan_member_id;
    sai_vlan_tagging_mode_t    vlan_mode;
};

typedef std::map<sai_vlan_id_t, VlanMemberEntry> vlan_members_t;

struct VlanInfo
{
    sai_object_id_t     vlan_oid = 0;
    sai_vlan_id_t       vlan_id = 0;
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
        SUBPORT,
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
    bool                m_autoneg = false;
    bool                m_admin_state_up = false;
    bool                m_init = false;
    bool                m_l3_vni = false;
    sai_object_id_t     m_port_id = 0;
    sai_port_fec_mode_t m_fec_mode = SAI_PORT_FEC_MODE_NONE;
    VlanInfo            m_vlan_info;
    sai_object_id_t     m_bridge_port_id = 0;   // TODO: port could have multiple bridge port IDs
    sai_vlan_id_t       m_port_vlan_id = DEFAULT_PORT_VLAN_ID;  // Port VLAN ID
    sai_object_id_t     m_rif_id = 0;
    sai_object_id_t     m_vr_id = 0;
    sai_object_id_t     m_hif_id = 0;
    sai_object_id_t     m_lag_id = 0;
    sai_object_id_t     m_lag_member_id = 0;
    sai_object_id_t     m_ingress_acl_table_group_id = 0;
    sai_object_id_t     m_egress_acl_table_group_id = 0;
    vlan_members_t      m_vlan_members;
    sai_object_id_t     m_parent_port_id = 0;
    uint32_t            m_dependency_bitmap = 0;
    sai_port_oper_status_t m_oper_status = SAI_PORT_OPER_STATUS_UNKNOWN;
    std::set<std::string> m_members;
    std::set<std::string> m_child_ports;
    std::vector<sai_object_id_t> m_queue_ids;
    std::vector<sai_object_id_t> m_priority_group_ids;
    sai_port_priority_flow_control_mode_t m_pfc_asym = SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_COMBINED;
    uint8_t m_pfc_bitmask = 0;
    uint32_t m_nat_zone_id = 0;

    /*
     * Following two bit vectors are used to lock
     * the PG/queue from being changed in BufferOrch.
     * The use case scenario is when PfcWdZeroBufferHandler
     * sets zero buffer profile it should protect PG/queue
     * from being overwritten in BufferOrch.
     */
    std::vector<bool> m_queue_lock;
    std::vector<bool> m_priority_group_lock;

    std::unordered_set<sai_object_id_t> m_ingress_acl_tables_uset;
    std::unordered_set<sai_object_id_t> m_egress_acl_tables_uset;
};

}

#endif /* SWSS_PORT_H */
