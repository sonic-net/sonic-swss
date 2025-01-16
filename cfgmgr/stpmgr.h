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

#include "dbconnector.h"
#include "netmsg.h"
#include "orch.h"
#include "producerstatetable.h"
#include <stddef.h>
#include <algorithm>

#define STPMGRD_SOCK_NAME "/var/run/stpmgrd.sock"

#define TAGGED_MODE      1
#define UNTAGGED_MODE    0
#define INVALID_MODE    -1

#define MAX_VLANS   4096

// Maximum number of instances supported
#define L2_INSTANCE_MAX             MAX_VLANS
#define STP_DEFAULT_MAX_INSTANCES   255
#define INVALID_INSTANCE            -1


#define GET_FIRST_FREE_INST_ID(_idx) \
    while (_idx < (int)l2InstPool.size() && l2InstPool.test(_idx)) ++_idx; \
    l2InstPool.set(_idx)

#define FREE_INST_ID(_idx) l2InstPool.reset(_idx)

#define FREE_ALL_INST_ID() l2InstPool.reset()

#define IS_INST_ID_AVAILABLE() (l2InstPool.count() < max_stp_instances) ? true : false

#define STPD_SOCK_NAME "/var/run/stpipc.sock"

typedef enum L2_PROTO_MODE{
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
    STP_MAX_MSG,
    STP_MST_GLOBAL_CONFIG,
    STP_MST_INST_CONFIG,
    STP_MST_INST_PORT_CONFIG
}STP_MSG_TYPE;

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
}STP_CTL_TYPE;

typedef struct STP_IPC_MSG {
    int             msg_type;
    unsigned int    msg_len;
    char            data[0];
}STP_IPC_MSG;

#define STP_SET_COMMAND      1
#define STP_DEL_COMMAND      0

typedef struct STP_INIT_READY_MSG {
    uint8_t     opcode; // enable/disable
    uint16_t    max_stp_instances;
}__attribute__ ((packed))STP_INIT_READY_MSG;

typedef struct STP_BRIDGE_CONFIG_MSG {
    uint8_t     opcode; // enable/disable
    uint8_t     stp_mode;
    int         rootguard_timeout;
    uint8_t     base_mac_addr[6];
}__attribute__ ((packed))STP_BRIDGE_CONFIG_MSG;

typedef struct PORT_ATTR {
    char       intf_name[IFNAMSIZ];
    int8_t     mode;
    uint8_t    enabled;
}PORT_ATTR;

typedef struct STP_VLAN_CONFIG_MSG {
    uint8_t     opcode; // enable/disable
    uint8_t     newInstance;
    int         vlan_id;
    int         inst_id;
    int         forward_delay;
    int         hello_time;
    int         max_age;
    int         priority;
    int         count;
    PORT_ATTR   port_list[0];
}__attribute__ ((packed))STP_VLAN_CONFIG_MSG;

typedef struct STP_VLAN_PORT_CONFIG_MSG {
    uint8_t     opcode; // enable/disable
    int         vlan_id;
    char        intf_name[IFNAMSIZ];
    int         inst_id;
    int         path_cost;
    int         priority;
}__attribute__ ((packed))STP_VLAN_PORT_CONFIG_MSG;

typedef struct VLAN_ATTR {
    int         inst_id;
    int         vlan_id;
    int8_t      mode;
}VLAN_ATTR;

typedef struct VLAN_LIST{
    uint16_t    vlan_id;
}VLAN_LIST;

typedef struct STP_PORT_CONFIG_MSG {
    uint8_t     opcode;             // enable/disable
    char        intf_name[IFNAMSIZ];
    uint8_t     enabled;
    uint8_t     root_guard;
    uint8_t     bpdu_guard;
    uint8_t     bpdu_guard_do_disable;
    int         path_cost;
    int         priority;
    int         count;
    VLAN_ATTR   vlan_list[0];
    // Union for protocol-specific fields (PVST vs MSTP)
    union {
        struct {
            uint8_t portfast;    // PVST only
            uint8_t uplink_fast; // PVST only
        } pvst_fields;
        struct {
            uint8_t edge_port;   // MSTP only
            LinkType link_type;  // MSTP only
        } mstp_fields;
    };
} __attribute__ ((packed)) STP_PORT_CONFIG_MSG;


typedef struct STP_VLAN_MEM_CONFIG_MSG {
    uint8_t     opcode; // enable/disable
    int         vlan_id;
    int         inst_id;
    char        intf_name[IFNAMSIZ];
    uint8_t     enabled;
    int8_t      mode;
    int         path_cost;
    int         priority;
}__attribute__ ((packed))STP_VLAN_MEM_CONFIG_MSG;

typedef struct STP_MST_GLOBAL_CONFIG_MSG {
    uint8_t     opcode; // enable/disable
    uint32_t    revision_number;
    char        name[32];
    uint8_t     forward_delay;
    uint8_t     hello_time;
    uint8_t     max_age;
    uint8_t     max_hop;
}__attribute__ ((packed))STP_MST_GLOBAL_CONFIG_MSG;

typedef struct VLAN_MST_ATTR {
    uint16_t    vlan_id;           // VLAN ID
    uint8_t     port_count;        // Number of ports in this VLAN
    PORT_ATTR   ports[0];         // Flexible array for Port attributes
}__attribute__ ((packed))VLAN_MST_ATTR;

typedef struct STP_MST_INST_CONFIG_MSG{
    uint8_t         opcode; // enable/disable
    uint16_t        mst_id;
    int             priority;
    uint16_t        vlan_count;
    VLAN_MST_ATTR   vlan_list[0];
}__attribute__ ((packed))STP_MST_INST_CONFIG_MSG;

typedef struct STP_MST_INST_PORT_CONFIG_MSG {
    uint8_t     opcode;         // enable/disable
    char        intf_name[IFNAMSIZ];  // Interface name
    uint16_t    mst_id;         // MST instance ID
    int         path_cost;      // Path cost
    int         priority;       // Port priority
} __attribute__((packed)) STP_MST_INST_PORT_CONFIG_MSG;

namespace swss {

class StpMgr : public Orch
{
public:
    StpMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb,
            const std::vector<TableConnector> &tables);

    using Orch::doTask;
	void ipcInitStpd();
    int sendMsgStpd(STP_MSG_TYPE msgType, uint32_t msgLen, void *data);
    MacAddress macAddress;
    bool isPortInitDone(DBConnector *app_db);
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
    Table m_cfgStpMstGlobalTable;
    Table m_cfgStpMstInstTable;
    Table m_cfgStpMstInstPortTable;

    std::bitset<L2_INSTANCE_MAX> l2InstPool;
	int stpd_fd;
    enum L2_PROTO_MODE l2ProtoEnabled;
    int m_vlanInstMap[MAX_VLANS];
    bool portCfgDone;
    uint16_t max_stp_instances;
    std::map<std::string, int> m_lagMap;

    bool stpGlobalTask;
    bool stpVlanTask;
    bool stpVlanPortTask;
    bool stpPortTask;
    bool stpMstGlobalTask;
    bool stpMstInstTask;
    bool stpMstInstPortTask;

    void doTask(Consumer &consumer);
    void doStpGlobalTask(Consumer &consumer);
    void doStpVlanTask(Consumer &consumer);
    void doStpVlanPortTask(Consumer &consumer);
    void doStpPortTask(Consumer &consumer);
    void doVlanMemUpdateTask(Consumer &consumer);
    void doLagMemUpdateTask(Consumer &consumer);
    void doStpMstGlobalTask(Consumer &consumer);
    void doStpMstInstTask(Consumer &consumer);
    void doStpMstInstPortTask(Consumer &consumer);

    bool isVlanStateOk(const std::string &alias);
    bool isLagStateOk(const std::string &alias);
    bool isStpPortEmpty();
    bool isStpEnabled(const std::string &intf_name);
    int getAllVlanMem(const std::string &vlanKey, std::vector<PORT_ATTR>&port_list);
    int getAllPortVlan(const std::string &intfKey, std::vector<VLAN_ATTR>&vlan_list);
    int8_t getVlanMemMode(const std::string &key);
    int allocL2Instance(uint32_t vlan_id);
    void deallocL2Instance(uint32_t vlan_id);
    bool isLagEmpty(const std::string &key);
    void processStpPortAttr(const std::string op, std::vector<FieldValueTuple>&tupEntry, const std::string intfName);
    void processStpVlanPortAttr(const std::string op, uint32_t vlan_id, const std::string intfName,
                    std::vector<FieldValueTuple>&tupEntry);
    void processStpMstInstPortAttr(const std::string op, uint16_t mst_id, const std::string intfName,
                                       std::vector<FieldValueTuple>&tupEntry);
    std::vector<int> parseVlanList(const std::string &vlanStr);
    void updateVlanInstanceMap(int instance, const std::vector<int>&newVlanList, bool operation);
};

}
#endif