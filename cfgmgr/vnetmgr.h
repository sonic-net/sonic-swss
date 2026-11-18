#ifndef __VNETMGRMGR__
#define __VNETMGRMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"
#include <map>
#include <set>
#include <vector>
#include <memory>
#include <string>
#include <utility>

#define VNET_VXLAN_TX_DEV "VxlanOutbSvd"
#define VNET_NFT_TABLE "sonic_vnet_vxtx"
#define VNET_DEFAULT_VXLAN_PORT 4789

namespace swss {

class VNetMgr : public Orch
{
public:
    VNetMgr(DBConnector *cfgDb, DBConnector *appDb, const std::vector<std::string> &tables,
            DBConnector *stateDb = nullptr);
    ~VNetMgr();
    using Orch::doTask;

    void reconcileKernelState();
    bool hasPendingTasks();

    typedef struct VnetInfo
    {
        std::string m_vxlanTunnel;
        std::string m_vni;
    } VnetInfo;

    typedef struct VxlanRouteTunnelInfo
    {
        std::string m_routeName;
        std::string m_macAddress;
        std::string m_endpoint;
        std::string m_vni;
        std::string m_vnet;
        std::string m_prefix;
        bool m_installOnKernel;
    } VxlanRouteTunnelInfo;

    typedef struct VxlanKernelRouteInfo
    {
        std::string m_routeName;
        std::string m_dstMac;
        std::string m_dstIp;
        std::string m_srcIp;
        std::string m_vni;
        std::string m_vnetVni;
        std::string m_vnet;
        std::string m_prefix;
    } VxlanKernelRouteInfo;

    typedef struct SmacRewriteRule
    {
        std::string m_vni;
        std::string m_srcPrefix;
        std::string m_mac;
        unsigned m_pref;
        uint32_t m_handle;
        bool m_installed;
    } SmacRewriteRule;

    typedef struct VxlanSwitchTableConfig
    {
        std::string m_routerMac;
        unsigned m_basePort = VNET_DEFAULT_VXLAN_PORT;
        unsigned m_srcPortMin = 0;
        unsigned m_srcPortMax = 0;
    } VxlanSwitchTableConfig;

private:
    void doTask(Consumer &consumer);
    bool doVnetCreateTask(const KeyOpFieldsValuesTuple & t);
    bool doVnetDeleteTask(const KeyOpFieldsValuesTuple & t);
    bool doVnetRouteTask(const KeyOpFieldsValuesTuple & t, const std::string & op);
    bool doVnetRouteTunnelCreateTask(const KeyOpFieldsValuesTuple & t);
    bool doVnetRouteTunnelDeleteTask(const KeyOpFieldsValuesTuple & t);
    bool doVxlanStateTask(const KeyOpFieldsValuesTuple & t);
    bool doAclRuleTask(const KeyOpFieldsValuesTuple & t);

    bool createKernelRoute(const VxlanRouteTunnelInfo & vxlanRouteInfo);
    bool deleteKernelRoute(const std::string & vnet, const std::string & prefix);

    bool getSwitchTableVxlanConfig();
    bool readVtepSrcIp(const std::string & tunnel, std::string & srcIp);

    bool ensureTxDevice(const std::string & vtep);
    void teardownTxDevice();
    bool installNftTable();

    bool neighRef(const std::string & mac);
    void neighUnref(const std::string & mac);

    bool installDmacBypassIfNeeded(const std::string & vnetVni, bool force = false);
    void removeDmacBypass(const std::string & vnetVni);

    bool isVniKernelRouted(const std::string & vni);
    void installSmacRewrites(const std::string & vni);
    void removeSmacRewrites(const std::string & vni);
    bool installSmacRewrite(SmacRewriteRule & rule);
    void removeSmacRewrite(SmacRewriteRule & rule);

    Table m_appSwitchTable;
    Table m_cfgVxlanTunnelTable;
    ProducerStateTable m_appVnetRouteTunnelTable, m_appVnetRouteTable;

    DBConnector *m_app_db;
    std::map<std::string, VnetInfo> m_vnetCache;
    std::map<std::string, std::vector<FieldValueTuple>> m_appVnetRouteTunnelPublished;
    // vnet -> prefix -> kernel route; a vnet entry exists only while it has routes
    std::map<std::string, std::map<std::string, VxlanKernelRouteInfo>> m_kernelRouteTunnelCache;

    std::map<std::string, int> m_neighRefs;
    std::set<std::string> m_dmacBypassInstalledVnis;
    // vni -> ACL rule key -> rule, plus the reverse index needed for ACL rule deletes
    std::map<std::string, std::map<std::string, SmacRewriteRule>> m_smacRewriteRules;
    std::map<std::string, std::string> m_smacRuleVni;
    uint32_t m_nextSmacHandle = 1;

    VxlanSwitchTableConfig m_switchVxlanCfg;

    bool m_txReady = false;
    std::string m_txVtep;
};

} // namespace swss

#endif

