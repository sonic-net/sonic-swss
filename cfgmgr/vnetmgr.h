
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
    using Orch::doTask;

    typedef struct VnetInfo
    {
        std::string m_vxlanTunnel;
        std::string m_sourceIp;
        std::string m_vnet;
        std::string m_vni;
        std::string m_macAddress;
    } VnetInfo;

    typedef struct TunCache
    {
        std::vector<FieldValueTuple> fvt;
        std::string m_sourceIp;
    } TunCache;

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
        std::string m_vni;
        std::string m_vnet;
        std::string m_prefix;
        std::string m_vxlanDevName;
        std::string m_vxlanSrcUdpPort;
    } VxlanKernelRouteInfo;

private:
    void doTask(Consumer &consumer);
    std::vector<std::string> parseNetDev(const std::string& stdout);
    void getAllVxlanNetDevices();
    bool doVnetCreateTask(const KeyOpFieldsValuesTuple & t);
    bool doVnetDeleteTask(const KeyOpFieldsValuesTuple & t);
    bool doVxlanTunnelCreateTask(const KeyOpFieldsValuesTuple & t);
    bool doVxlanTunnelDeleteTask(const KeyOpFieldsValuesTuple & t);
    bool doVnetRouteTask(const KeyOpFieldsValuesTuple & t, const std::string & op);
    bool doVnetRouteTunnelCreateTask(const KeyOpFieldsValuesTuple & t);
    bool doVnetRouteTunnelDeleteTask(const KeyOpFieldsValuesTuple & t);

    bool createKernelRoute(const VxlanRouteTunnelInfo & vxlanRouteInfo);
    bool deleteKernelRoute(const VxlanRouteTunnelInfo & vxlanRouteInfo);
    std::string getVxlanSourcePort();


    Table m_appSwitchTable;
    ProducerStateTable m_appVnetRouteTunnelTable, m_appVnetRouteTable;

    DBConnector *m_app_db;

    std::map<std::string, std::string> m_vxlanNetDevices;
    std::map<std::string, TunCache > m_vxlanTunnelCache;
    std::map<std::string, VnetInfo> m_vnetCache;
    std::map<std::string, VxlanRouteTunnelInfo> m_vnetRouteTunnelCache;
    std::map<std::string, VxlanKernelRouteInfo> m_kernelRouteTunnelCache;
};

} // namespace swss

#endif