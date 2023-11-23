#ifndef __VRRPSYNC__
#define __VRRPSYNC__

#include <set>
#include <map>

#include "dbconnector.h"
#include "subscriberstatetable.h"
#include "producerstatetable.h"
#include "netmsg.h"
#include "vrrpintf.h"

namespace swss
{

#define VRRP_DEFAULT_PRIORITY 100
/* configured in config db */
struct VrrpConf
{
    /* <track intf, track weight> */
    std::map<std::string, int> track;
    int priority{VRRP_DEFAULT_PRIORITY};
    bool backup_forward;
};

struct VrrpInfo
{
    VrrpIntf netlink_vrrp_ipv4;
    VrrpIntf netlink_vrrp_ipv6;
    VrrpConf conf;
    /* effective priority */
    int effect_priority{VRRP_DEFAULT_PRIORITY};
};

class VrrpSync : public NetMsg
{
public:
    VrrpSync(RedisPipeline *pipelineAppDB, DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb);

    virtual void onMsg(int nlmsg_type, struct nl_object *obj) override;

    SubscriberStateTable *getCfgVrrpTable()
    {
        return &m_cfgVrrpTable;
    }

    void processCfgVrrp();

private:
    ProducerStateTable m_vrrpTable;
    SubscriberStateTable m_cfgVrrpTable;
    Table m_appVrrpTable;

    /* <vrid, vrrp info> */
    std::map<int, VrrpInfo> m_vrrpInfoList;
    /* <intf, link state> */
    std::map<std::string, bool> m_linkStateList;

    /* Calculate the current priority based on the comparison of the input vrrp with the locally cached linkState */
    int calculateCurrentPriority(const VrrpConf &vrrp);

    bool updateVrrpEffectPriority(const std::string &parent_name, const int vrid, const int priority);
    bool updateVrrpEffectPriorityWithIpTpye(const std::string &parent_name, const int vrid, const bool is_ipv4, const int priority);
    // bool updateVrrpState(const std::string &parent_name, const int vrid, const bool state);
    bool updateVrrpStateWithIpTpye(const std::string &parent_name, const int vrid, const bool is_ipv4, const bool state);
    bool setFrrVrrpPriority(const std::string &vrrp_name, const int vrid, const int priority);
};

}

#endif /* __VRRPSYNC__ */