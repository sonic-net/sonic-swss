#ifndef __STPMGR__
#define __STPMGR__

#include <set>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <bitset>
#include <vector>
#include <string>
#include <unordered_set>
#include <chrono>
#include <map>

#include "dbconnector.h"
#include "netmsg.h"
#include "orch.h"
#include "producerstatetable.h"
#include <stddef.h>
#include <algorithm>

// We remove PACKED definitions, only keep ALIGNED
#if defined(__GNUC__)
#define ALIGNED(x) __attribute__((aligned(x)))
#else
#define ALIGNED(x)
#endif

// Socket paths default to /var/run but can be overridden at compile time
// (e.g. component tests point them at a writable temp dir).
#ifndef STPMGRD_SOCK_NAME
#define STPMGRD_SOCK_NAME "/var/run/stpmgrd.sock"
#endif

#define TAGGED_MODE    1
#define UNTAGGED_MODE  0
#define INVALID_MODE  -1

#define MAX_VLANS 4096

// Maximum number of instances supported
#define L2_INSTANCE_MAX             MAX_VLANS
#define STP_DEFAULT_MAX_INSTANCES   255
#define STP_DFLT_FORWARD_DELAY      15
#define STP_DFLT_HELLO_TIME         2
#define STP_DFLT_MAX_AGE            20
#define STP_DFLT_BRIDGE_PRIORITY    32768

// STATE_DB rows for a VLAN or MST instance spanning tree is configured for
// but the hardware has no instance left for.
#define STATE_STP_VLAN_TABLE_NAME     "STP_VLAN_TABLE"
#define STATE_STP_MST_INST_TABLE_NAME "STP_MST_INST_TABLE"
#define STP_INSTANCE_STATE_FIELD      "instance_state"
#define STP_INSTANCE_UNAVAILABLE      "unavailable"
#define INVALID_INSTANCE            -1

#define GET_FIRST_FREE_INST_ID(_idx) \
    while (_idx < (int)l2InstPool.size() && l2InstPool.test(_idx)) ++_idx; \
    l2InstPool.set(_idx)

#define FREE_INST_ID(_idx) l2InstPool.reset(_idx)
#define FREE_ALL_INST_ID() l2InstPool.reset()
#define IS_INST_ID_AVAILABLE() (l2InstPool.count() < max_stp_instances)

#ifndef STPD_SOCK_NAME
#define STPD_SOCK_NAME "/var/run/stpipc.sock"
#endif

// Retry budget for a send that finds stpd not listening: 20 x 100 ms, spent
// once per backoff window; while stpd stays away every further send is one
// attempt.
#define STPD_SEND_RETRIES   20
#define STPD_SEND_RETRY_US  100000
#define STPD_SEND_BACKOFF_S 5
#define STPD_READY_POLL_MS  100
#define STPD_READY_WAIT_MS  30000

/* STP_MST_INST VLAN mapping. The YANG model defines leaf-list "vlan", which
 * ConfigDBConnector stores as "vlan@"; swss::Table strips the "@" on read, so
 * the consumer sees "vlan". _LEGACY is the spelling older images wrote, still
 * read so an existing mapping is not dropped. */
#define STP_MST_INST_VLAN_FIELD        "vlan"
#define STP_MST_INST_VLAN_FIELD_LEGACY "vlan_list"

// Enumerations must match stp_ipc.h
typedef enum L2_PROTO_MODE {
    L2_NONE,
    L2_PVSTP,
    L2_MSTP
}L2_PROTO_MODE;


typedef enum LinkType {
    AUTO =              0,      // Auto
    POINT_TO_POINT =    1,      // Point-to-point
    SHARED =            2       // Shared
} LinkType;

typedef enum STP_MSG_TYPE {
    STP_INVALID_MSG,
    STP_INIT_READY,
    STP_BRIDGE_CONFIG,
    STP_VLAN_CONFIG,
    STP_VLAN_PORT_CONFIG,
    STP_PORT_CONFIG,
    STP_VLAN_MEM_CONFIG,
    STP_STPCTL_MSG,
    STP_MST_GLOBAL_CONFIG,
    STP_MST_INST_CONFIG,
    STP_MST_VLAN_PORT_LIST_CONFIG,
    STP_MST_INST_PORT_CONFIG,
    STP_MAX_MSG
} STP_MSG_TYPE;

typedef enum STP_CTL_TYPE {
    STP_CTL_HELP,
    STP_CTL_DUMP_ALL,
    STP_CTL_DUMP_GLOBAL,
    STP_CTL_DUMP_VLAN_ALL,
    STP_CTL_DUMP_VLAN,
    STP_CTL_DUMP_INTF,
    STP_CTL_SET_LOG_LVL,
    STP_CTL_DUMP_NL_DB,
    STP_CTL_DUMP_NL_DB_INTF,
    STP_CTL_DUMP_LIBEV_STATS,
    STP_CTL_SET_DBG,
    STP_CTL_CLEAR_ALL,
    STP_CTL_CLEAR_VLAN,
    STP_CTL_CLEAR_INTF,
    STP_CTL_CLEAR_VLAN_INTF,
    STP_CTL_MAX
} STP_CTL_TYPE;

// Remove PACKED, add ALIGNED(4)
typedef struct STP_IPC_MSG {
    int          msg_type;
    unsigned int msg_len;
    L2_PROTO_MODE proto_mode;
    char         data[0];
} ALIGNED(4) STP_IPC_MSG;

#define STP_SET_COMMAND 1
#define STP_DEL_COMMAND 0

// Add padding for alignment if needed (compare to stp_ipc.h)
typedef struct STP_INIT_READY_MSG {
    uint8_t  opcode; // enable/disable
    uint16_t max_stp_instances;
    // Example: potential extra padding if alignment warnings arise
    // uint8_t  padding[1];
} ALIGNED(4) STP_INIT_READY_MSG;

// Add padding for alignment if needed
typedef struct STP_BRIDGE_CONFIG_MSG {
    uint8_t opcode;       // enable/disable
    uint8_t stp_mode;
    int     rootguard_timeout;
    uint8_t base_mac_addr[6];
    // Potential padding for alignment:
    // uint8_t padding[2];
} ALIGNED(4) STP_BRIDGE_CONFIG_MSG;

// Must match the version in stp_ipc.h exactly
typedef struct PORT_ATTR {
    char   intf_name[IFNAMSIZ]; // 16 bytes typically
    int8_t mode;
    uint8_t enabled;
    // Add padding to align to 4 bytes
    uint16_t padding;
} ALIGNED(4) PORT_ATTR;

// Must match the version in stp_ipc.h exactly
typedef struct STP_VLAN_CONFIG_MSG {
    uint8_t   opcode;      // enable/disable
    uint8_t   newInstance;
    int       vlan_id;
    int       inst_id;
    int       forward_delay;
    int       hello_time;
    int       max_age;
    int       priority;
    int       count;
    PORT_ATTR port_list[0];
} ALIGNED(4) STP_VLAN_CONFIG_MSG;

typedef struct STP_VLAN_PORT_CONFIG_MSG {
    uint8_t opcode;  // enable/disable
    int     vlan_id;
    char    intf_name[IFNAMSIZ];
    int     inst_id;
    int     path_cost;
    int     priority;
} ALIGNED(4) STP_VLAN_PORT_CONFIG_MSG;

typedef struct VLAN_ATTR {
    int   inst_id;
    int   vlan_id;
    int8_t mode;
    // Add padding to align to 4 bytes
    uint8_t padding[3];
} ALIGNED(4) VLAN_ATTR;

typedef struct VLAN_LIST{
    uint16_t    vlan_id;
}VLAN_LIST;

typedef struct STP_PORT_CONFIG_MSG {
    uint8_t     opcode;             // enable/disable
    char        intf_name[IFNAMSIZ];
    uint8_t     enabled;
    uint8_t     root_guard;
    uint8_t     loop_guard;
    uint8_t     bpdu_guard;
    uint8_t     bpdu_guard_do_disable;
    uint8_t     portfast;           // PVST only
    uint8_t     uplink_fast;        // PVST only
    uint8_t     edge_port;          // MSTP only
    LinkType    link_type;          // MSTP only
    int         path_cost;
    int         priority;
    int         count;
    VLAN_ATTR   vlan_list[0];
} STP_PORT_CONFIG_MSG;

typedef struct STP_VLAN_MEM_CONFIG_MSG {
    uint8_t opcode;   // enable/disable
    int     vlan_id;
    int     inst_id;
    char    intf_name[IFNAMSIZ];
    uint8_t enabled;
    int8_t  mode;
    // Add 1 byte padding
    uint8_t padding;
    int     path_cost;
    int     priority;
} ALIGNED(4) STP_VLAN_MEM_CONFIG_MSG;

typedef struct STP_MST_GLOBAL_CONFIG_MSG {
    uint8_t     opcode; // enable/disable
    uint32_t    revision_number;
    char        name[32];
    uint8_t     forward_delay;
    uint8_t     hello_time;
    uint8_t     max_age;
    uint8_t     max_hops;
}__attribute__ ((packed))STP_MST_GLOBAL_CONFIG_MSG;

typedef struct STP_MST_INST_CONFIG_MSG {
    uint8_t         opcode;     // enable/disable
    uint16_t        mst_id;     // MST instance ID
    int             priority;   // Bridge priority
    uint16_t        vlan_count; // Number of VLANs in this instance
    VLAN_LIST       vlan_list[0]; // Flexible array for VLAN IDs
}__attribute__((packed)) STP_MST_INST_CONFIG_MSG;

typedef struct STP_MST_INST_PORT_CONFIG_MSG {
    uint8_t     opcode;         // enable/disable
    char        intf_name[IFNAMSIZ];  // Interface name
    uint16_t    mst_id;         // MST instance ID
    int         path_cost;      // Path cost
    int         priority;       // Port priority
} __attribute__((packed)) STP_MST_INST_PORT_CONFIG_MSG;

/* Layout must match sonic-stp include/stp_ipc.h */
typedef struct PORT_LIST {
    char        intf_name[IFNAMSIZ];
    int8_t      tagging_mode;
} PORT_LIST;

typedef struct STP_MST_VLAN_PORT_MAP {
    uint16_t    vlan_id;
    uint16_t    port_count;
    int8_t      stp_mode;
    uint8_t     add;
    PORT_LIST   port_list[0];
} STP_MST_VLAN_PORT_MAP;

/* A per-instance port attribute the row did not carry. stpd falls back to
 * the port-wide value for this exact value. */
#define STP_MST_PORT_ATTR_UNSET (-1)

namespace swss {

class StpMgr : public Orch
{
public:
    StpMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb,
           const std::vector<TableConnector> &tables);

    using Orch::doTask;
    void ipcInitStpd();
    int  sendMsgStpd(STP_MSG_TYPE msgType, uint32_t msgLen, void *data);
    MacAddress macAddress;
    bool isPortInitDone(DBConnector *app_db);
    bool waitStpdReady(int timeout_ms = STPD_READY_WAIT_MS);
    uint16_t getStpMaxInstances(void);    

private:
    Table m_cfgStpGlobalTable;
    Table m_cfgStpVlanTable;
    Table m_cfgStpVlanPortTable;
    Table m_cfgStpPortTable;
    Table m_cfgLagMemberTable;
    Table m_cfgVlanMemberTable;
    Table m_stateVlanTable;
    Table m_stateVlanMemberTable;
    Table m_stateLagTable;
    Table m_stateStpTable;
    Table m_cfgMstGlobalTable;
    Table m_cfgMstInstTable;
    Table m_cfgMstInstPortTable;
    Table m_stateStpVlanTable;
    Table m_stateStpMstInstTable;

    std::bitset<L2_INSTANCE_MAX> l2InstPool;
    int stpd_fd;
    enum L2_PROTO_MODE l2ProtoEnabled;
    int m_vlanInstMap[MAX_VLANS];
    bool portCfgDone;
    uint16_t max_stp_instances;
    std::map<std::string, int> m_lagMap;

    // MST instance rows handed to stpd, so the count against the hardware
    // limit is known; the CIST takes one whether or not its row exists.
    std::set<int> m_mstInstances;
    // Spanning tree is configured for these but no instance was free; each
    // gets the next instance that is freed, lowest first.
    std::set<int> m_vlansWaitingForInstance;
    std::set<int> m_mstInstancesWaiting;

    // STP|GLOBAL priority and timers the PVST VLANs inherit, as last seen;
    // -1 until the first row.
    int m_globalPriority;
    int m_globalForwardDelay;
    int m_globalHelloTime;
    int m_globalMaxAge;
    // STP|GLOBAL fields already reported as unparseable, with the bad value.
    std::map<std::string, std::string> m_badGlobalFields;
    // Whether the last send found no listener, and when.
    bool m_sendFailed;
    std::chrono::steady_clock::time_point m_lastSendFailure;

    bool stpGlobalTask;
    bool stpVlanTask;
    bool stpVlanPortTask;
    bool stpPortTask;
    bool stpMstInstTask;

    void doTask(Consumer &consumer);
    void disableProtocol();
    void replayModeConfig();
    void doStpGlobalTask(Consumer &consumer);
    void doStpVlanTask(Consumer &consumer);
    void doStpVlanPortTask(Consumer &consumer);
    void doStpPortTask(Consumer &consumer);
    void doVlanMemUpdateTask(Consumer &consumer);
    void doLagMemUpdateTask(Consumer &consumer);
    void doStpMstGlobalTask(Consumer &consumer);
    void doStpMstInstTask(Consumer &consumer);
    void doStpMstInstPortTask(Consumer &consumer);
    void sendMstVlanPortList(int vlan_id, const std::vector<PORT_ATTR> &memPorts, bool add);
    void sendVlanMemConfig(int vlan_id, const std::string &intfName, const std::string &vlanMemKey, bool add);
    void reenrolLagVlans(const std::string &po_name);
    void enrolMstVlanMembers();
    void readStpVlanFields(const std::vector<swss::FieldValueTuple> &fvs, bool &enabled,
                           int &forwardDelay, int &helloTime, int &maxAge, int &priority);
    void refreshPvstVlans();
    int  parseGlobalInt(const std::string &field, const std::string &value, int dflt);
    bool updateGlobalPvstFields(const std::vector<swss::FieldValueTuple> &fvs);

    bool isVlanStateOk(const std::string &alias);
    bool isLagStateOk(const std::string &alias);
    bool isStpPortEmpty();
    bool isStpEnabled(const std::string &intf_name);
    int getAllVlanMem(const std::string &vlanKey, std::vector<PORT_ATTR>& port_list);
    int getAllPortVlan(const std::string &intfKey, std::vector<VLAN_ATTR>& vlan_list);
    int8_t getVlanMemMode(const std::string &key);
    int allocL2Instance(uint32_t vlan_id);
    void deallocL2Instance(uint32_t vlan_id);
    bool mstInstanceLimitReached();
    void holdForInstance(Table &stateTable, const std::string &key, std::set<int> &waiting, int id);
    void releaseHold(Table &stateTable, const std::string &key, std::set<int> &waiting, int id);
    void retryWaitingVlan();
    void retryWaitingMstInstance();
    void clearInstanceHolds();
    bool isLagEmpty(const std::string &key);
    void processStpPortAttr(const std::string op, std::vector<FieldValueTuple>&tupEntry, const std::string intfName);
    void processStpVlanPortAttr(const std::string op, uint32_t vlan_id, const std::string intfName,
                    std::vector<FieldValueTuple>&tupEntry);
    void processStpMstInstPortAttr(const std::string op, uint16_t mst_id, const std::string intfName,
                                       std::vector<FieldValueTuple>&tupEntry);
    bool parseVlanList(const std::string &vlanStr, std::vector<uint16_t> &vlanList);
    void updateVlanInstanceMap(int instance, const std::vector<uint16_t>&newVlanList, bool operation);
    bool isInstanceMapped(uint16_t instance);
    std::vector<std::string> getVlanAliasesForInstance(uint16_t instance);
    
    
};

}
#endif
