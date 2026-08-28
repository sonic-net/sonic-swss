#ifndef __VNETMGRMGR__
#define __VNETMGRMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"
#include <map>
#include <vector>
#include <memory>
#include <string>
#include <utility>

namespace swss {

class VNetMgr : public Orch
{
public:
    VNetMgr(DBConnector *cfgDb, DBConnector *appDb, const std::vector<std::string> &tables);
    ~VNetMgr();
    using Orch::doTask;

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
        std::string m_srcMac;
        std::string m_vni;         // encap VNI (route VNI, may differ from vnet VNI)
        std::string m_vnetVni;     // vnet's own VNI (identifies the shared Vxlan<vnetVni> netdev)
        std::string m_vnet;
        std::string m_prefix;
    } VxlanKernelRouteInfo;

private:
    void doTask(Consumer &consumer);
    bool doVnetCreateTask(const KeyOpFieldsValuesTuple & t);
    bool doVnetDeleteTask(const KeyOpFieldsValuesTuple & t);
    bool doVnetRouteTask(const KeyOpFieldsValuesTuple & t, const std::string & op);
    bool doVnetRouteTunnelCreateTask(const KeyOpFieldsValuesTuple & t);
    bool doVnetRouteTunnelDeleteTask(const KeyOpFieldsValuesTuple & t);

    bool probeVxlanBridgePair(const VxlanKernelRouteInfo & info);
    bool createKernelRoute(const VxlanRouteTunnelInfo & vxlanRouteInfo);
    bool deleteKernelRoute(const VxlanRouteTunnelInfo & vxlanRouteInfo);

    Table m_appSwitchTable;
    ProducerStateTable m_appVnetRouteTunnelTable, m_appVnetRouteTable;

    DBConnector *m_app_db;
    std::map<std::string, VnetInfo> m_vnetCache;
    std::map<std::string, VxlanRouteTunnelInfo> m_vnetRouteTunnelCache;
    std::map<std::string, VxlanKernelRouteInfo> m_kernelRouteTunnelCache;
};

} // namespace swss

#endif
