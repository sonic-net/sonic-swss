#ifndef SWSS_PORT_H
#define SWSS_PORT_H

extern "C" {
#include <sai.h>
}

#include <set>
#include <string>
#include <vector>
#include <map>
#include <bitset>
#include <chrono>
#include <unordered_set>
#include <iomanip>
#include <sstream>
#include <macaddress.h>
#include <sairedis.h>

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

typedef std::map<sai_uint16_t, sai_object_id_t> stp_port_ids_t;
class PortOperErrorEvent
{
public:
    PortOperErrorEvent() = default;
    PortOperErrorEvent(const sai_port_error_status_t error, std::string key) : m_errorFlag(error), m_dbKeyError(key){}
    ~PortOperErrorEvent() = default;

    inline void incrementErrorCount(void) { m_errorCount++; }
    
    inline size_t getErrorCount(void) const { return m_errorCount; }
    
    void recordEventTime(void) {
        auto now = std::chrono::system_clock::now();
        m_eventTime = std::chrono::system_clock::to_time_t(now);
    }
    
    std::string getEventTime(void) {
        std::ostringstream oss;
        oss << std::put_time(std::gmtime(&m_eventTime), "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

    inline std::string getDbKey(void) const { return m_dbKeyError; }
    
    // Returns true if port oper error flag in sai_port_error_status_t is set
    bool isErrorSet(sai_port_error_status_t errstatus) const { return (m_errorFlag & errstatus);}

    static const std::unordered_map<sai_port_error_status_t, std::string> db_key_errors;

private:
    sai_port_error_status_t m_errorFlag = SAI_PORT_ERROR_STATUS_CLEAR;
    size_t m_errorCount = 0;
    std::string m_dbKeyError; // DB key for this port error
    std::time_t m_eventTime = 0;
};

class Port
{
public:
    typedef sai_bridge_port_fdb_learning_mode_t port_learn_mode_t;

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
    };

    enum Role
    {
        Ext, // external
        Int, // internal
        Inb, // inband
        Rec, // recirculation
        Dpc  // DPU Connect Port on SmartSwitch
    };

public:
    static constexpr std::size_t max_lanes = 8; // Max HW lanes
    static constexpr std::size_t max_fec_modes = 3; // Max FEC modes (sync with SAI)

public:
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
    Type                m_type = UNKNOWN;
    uint16_t            m_index = 0;    // PHY_PORT: index
    uint32_t            m_mtu = DEFAULT_MTU;
    uint32_t            m_speed = 0;    // Mbps
    port_learn_mode_t   m_learn_mode = SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW;
    bool                m_autoneg = false;
    bool                m_link_training = false;
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
    sai_port_error_status_t m_oper_error_status = SAI_PORT_ERROR_STATUS_CLEAR; //Bitmap of last port oper error status
    std::set<std::string> m_members;
    std::set<std::string> m_child_ports;
    std::vector<sai_object_id_t> m_queue_ids;
    std::vector<sai_object_id_t> m_priority_group_ids;
    sai_port_priority_flow_control_mode_t m_pfc_asym = SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_COMBINED;
    uint8_t   m_pfc_bitmask = 0;        // PFC enable bit mask
    uint8_t   m_pfcwd_sw_bitmask = 0;   // PFC software watchdog enable
    uint8_t   m_host_tx_queue = 0;
    bool      m_host_tx_queue_configured = false;
    uint16_t  m_tpid = DEFAULT_TPID;
    uint32_t  m_nat_zone_id = 0;
    uint32_t  m_vnid = VNID_NONE;
    uint32_t  m_fdb_count = 0;
    uint64_t  m_flap_count = 0;
    uint32_t  m_up_member_count = 0;
    uint32_t  m_maximum_headroom = 0;
    std::set<uint32_t> m_adv_speeds;
    sai_port_interface_type_t m_interface_type = SAI_PORT_INTERFACE_TYPE_NONE;
    std::set<sai_port_interface_type_t> m_adv_interface_types;
    bool      m_mpls = false;
    /*
     * Following bit vector is used to lock
     * the queue from being changed in BufferOrch.
     * The use case scenario is when PfcWdZeroBufferHandler
     * sets zero buffer profile it should protect queue
     * from being overwritten in BufferOrch.
     */
    std::vector<bool> m_queue_lock;

    std::unordered_set<sai_object_id_t> m_ingress_acl_tables_uset;
    std::unordered_set<sai_object_id_t> m_egress_acl_tables_uset;

    sai_object_id_t  m_system_port_oid = 0;
    SystemPortInfo   m_system_port_info;
    SystemLagInfo    m_system_lag_info;

    sai_object_id_t  m_switch_id = 0;
    sai_object_id_t  m_system_side_id = 0;
    sai_object_id_t  m_line_side_id = 0;

    stp_port_ids_t m_stp_port_ids; //STP Port object ids for each STP instance
    sai_int16_t m_stp_id = -1; //STP instance for the VLAN
    /* Port oper error status to event map*/
    std::unordered_map<sai_port_error_status_t, PortOperErrorEvent> m_portOperErrorToEvent;

    /* pre-emphasis */
    std::map<sai_port_serdes_attr_t, std::vector<uint32_t>> m_preemphasis;

    /* Force initial parameter configuration flags */
    bool m_an_cfg = false;        // Auto-negotiation (AN)
    bool m_adv_speed_cfg = false; // Advertised speed
    bool m_intf_cfg = false;      // Interface type
    bool m_adv_intf_cfg = false;  // Advertised interface type
    bool m_fec_cfg = false;       // Forward Error Correction (FEC)
    bool m_override_fec = false;  // Enable Override FEC
    bool m_pfc_asym_cfg = false;  // Asymmetric Priority Flow Control (PFC)
    bool m_lm_cfg = false;        // Forwarding Database (FDB) Learning Mode (LM)
    bool m_lt_cfg = false;        // Link Training (LT)

    int m_cap_an = -1; /* Capability - AutoNeg, -1 means not set */
    int m_cap_lt = -1; /* Capability - LinkTraining, -1 means not set */

    /* Path Tracing */
    uint16_t m_pt_intf_id = 0;
    sai_port_path_tracing_timestamp_type_t m_pt_timestamp_template = SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_16_23;

    /* link event damping */
    sai_redis_link_event_damping_algorithm_t m_link_event_damping_algorithm = SAI_REDIS_LINK_EVENT_DAMPING_ALGORITHM_DISABLED;
    uint32_t m_max_suppress_time = 0;
    uint32_t m_decay_half_life = 0;
    uint32_t m_suppress_threshold = 0;
    uint32_t m_reuse_threshold = 0;
    uint32_t m_flap_penalty = 0;
};

}

#endif /* SWSS_PORT_H */