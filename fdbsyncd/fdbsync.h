#ifndef __FDBSYNC__
#define __FDBSYNC__

#include <string>
#include <arpa/inet.h>
#include "dbconnector.h"
#include "producerstatetable.h"
#include "subscriberstatetable.h"
#include "netmsg.h"
#include "warmRestartAssist.h"

/*
 * Default timer interval for fdbsyncd reconcillation 
 */
#define DEFAULT_FDBSYNC_WARMSTART_TIMER 120

/*
 * This is the MAX time in seconds, fdbsyncd will wait after warm-reboot
 * for the interface entries to be recreated in kernel before attempting to 
 * write the FDB data to kernel
 */
#define INTF_RESTORE_MAX_WAIT_TIME 180

namespace swss {

enum FDB_OP_TYPE {
    FDB_OPER_ADD =1,
    FDB_OPER_DEL = 2,
};

enum FDB_TYPE {
    FDB_TYPE_STATIC = 1,
    FDB_TYPE_DYNAMIC = 2,
};

enum NEXT_HOP_VALUE_TYPE {
    UNKNOWN = 0,
    VTEP = 1,
    NEXTHOPGROUP = 2,
    IFNAME = 3,
};

struct m_fdb_info
{
    std::string  mac;
    std::string  vid;           /*Store as Vlan<ID> */
    std::string  port_name;
    short type;                 /*dynamic or static*/
    short op_type;              /*add or del*/
};

class FdbSync : public NetMsg
{
public:
    enum { MAX_ADDR_SIZE = 64 };

    FdbSync(RedisPipeline *pipelineAppDB, DBConnector *stateDb, DBConnector *config_db);
    ~FdbSync();

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

    bool isIntfRestoreDone();

    AppRestartAssist *getRestartAssist()
    {
        return m_AppRestartAssist;
    }

    SubscriberStateTable *getFdbStateTable()
    {
        return &m_fdbStateTable;
    }

    SubscriberStateTable *getMclagRemoteFdbStateTable()
    {
        return &m_mclagRemoteFdbStateTable;
    }

    SubscriberStateTable *getCfgEvpnNvoTable()
    {
        return &m_cfgEvpnNvoTable;
    }

    void processStateFdb();

    void processStateMclagRemoteFdb();

    void processCfgEvpnNvo();

    bool m_reconcileDone = false;

    bool m_isEvpnNvoExist = false;

private:
    ProducerStateTable m_fdbTable;
    ProducerStateTable m_imetTable;
    SubscriberStateTable m_fdbStateTable;
    SubscriberStateTable m_mclagRemoteFdbStateTable;
    AppRestartAssist  *m_AppRestartAssist;
    SubscriberStateTable m_cfgEvpnNvoTable;

    struct m_local_fdb_info
    {
        std::string port_name;
        short type;/*dynamic or static*/
    };
    std::unordered_map<std::string, m_local_fdb_info> m_fdb_mac;

    std::unordered_map<std::string, m_local_fdb_info> m_mclag_remote_fdb_mac;

    void macDelVxlanEntry(std::string auxkey, struct m_fdb_info *info);

    void macUpdateCache(struct m_fdb_info *info);

    bool macCheckSrcDB(struct m_fdb_info *info);

    void updateLocalMac(struct m_fdb_info *info);

    void updateAllLocalMac();

    void macRefreshStateDB(int vlan, std::string kmac);

    void updateMclagRemoteMac(struct m_fdb_info *info);

    void updateMclagRemoteMacPort(int ifindex, int vlan, std::string mac);

    void macUpdateMclagRemoteCache(struct m_fdb_info *info);

    bool checkImetExist(std::string key, uint32_t vni);

    bool checkDelImet(std::string key, uint32_t vni);

    struct m_mac_info
    {
        NEXT_HOP_VALUE_TYPE nhtype;
        std::string type;
        unsigned int vni;
        std::string  ifname;
        union {
            std::string remote_vtep;
            std::string nexthop_group;
            std::string ifname;
        } v;
    };
    std::unordered_map<std::string, m_mac_info> m_mac;

    struct m_imet_info
    {
        unsigned int vni;
    };
    std::unordered_map<std::string, m_imet_info> m_imet_route;

    struct intf
    {
        std::string ifname;
        unsigned int vni;
    };
    std::unordered_map<int, intf> m_intf_info;

    void addLocalMac(std::string key, std::string op);
    void macAddVxlan(std::string key, struct nl_addr *vtep, std::string type, uint32_t vni, std::string intf_name, std::string nexthop_group, NEXT_HOP_VALUE_TYPE dest_type);
    void macDelVxlan(std::string auxkey);
    void macDelVxlanDB(std::string key);
    void imetAddRoute(struct in_addr vtep, std::string ifname, uint32_t vni);
    void imetDelRoute(struct in_addr vtep, std::string ifname, uint32_t vni);
    void onMsgNbr(int nlmsg_type, struct nl_object *obj);
    void onMsgLink(int nlmsg_type, struct nl_object *obj);
};

}

#endif

