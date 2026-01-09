#include "intfsorch.h"

const int intfsorch_pri = 35;

IntfsOrch::IntfsOrch(DBConnector *db, string tableName, VRFOrch *vrf_orch, DBConnector *chassisAppDb) :
        Orch(db, tableName, intfsorch_pri), m_vrfOrch(vrf_orch)
{
}

sai_object_id_t IntfsOrch::getRouterIntfsId(const string &alias)
{
    return 0;
}

bool IntfsOrch::isPrefixSubnet(const IpPrefix &ip_prefix, const string &alias)
{
    return true;
}

string IntfsOrch::getRouterIntfsAlias(const IpAddress &ip, const string &vrf_name)
{
    return "";
}

bool IntfsOrch::isInbandIntfInMgmtVrf(const string& alias)
{
    return true;
}

void IntfsOrch::increaseRouterIntfsRefCount(const string &alias)
{
}

void IntfsOrch::decreaseRouterIntfsRefCount(const string &alias)
{
}

bool IntfsOrch::setRouterIntfsMpls(const Port &port)
{
    return true;
}

bool IntfsOrch::setRouterIntfsMtu(const Port &port)
{
    return true;
}

bool IntfsOrch::setRouterIntfsMac(const Port &port)
{
    return true;
}

bool IntfsOrch::setRouterIntfsNatZoneId(Port &port)
{
    return true;
}

bool IntfsOrch::setRouterIntfsAdminStatus(const Port &port)
{
    return true;
}

bool IntfsOrch::setIntfVlanFloodType(const Port &port, sai_vlan_flood_control_type_t vlan_flood_type)
{
    return true;
}

bool IntfsOrch::setIntfProxyArp(const string &alias, const string &proxy_arp)
{
    return true;
}

bool IntfsOrch::setIntfLoopbackAction(const Port &port, string actionStr)
{
    return true;
}

set<IpPrefix> IntfsOrch:: getSubnetRoutes()
{
    set<IpPrefix> subnet_routes;
    return subnet_routes;
}

bool IntfsOrch::setIntf(const string& alias, sai_object_id_t vrf_id, const IpPrefix *ip_prefix,
                        const bool adminUp, const uint32_t mtu, string loopbackAction)

{
    return true;
}

bool IntfsOrch::removeIntf(const string& alias, sai_object_id_t vrf_id, const IpPrefix *ip_prefix)
{
    return true;
}

void IntfsOrch::doTask(Consumer &consumer)
{
}

bool IntfsOrch::getSaiLoopbackAction(const string &actionStr, sai_packet_action_t &action)
{
    return true;
}

bool IntfsOrch::addRouterIntfs(sai_object_id_t vrf_id, Port &port, string loopbackActionStr)
{
    return true;
}

bool IntfsOrch::removeRouterIntfs(Port &port)
{
    return true;
}

void IntfsOrch::addIp2MeRoute(sai_object_id_t vrf_id, const IpPrefix &ip_prefix)
{
}

void IntfsOrch::removeIp2MeRoute(sai_object_id_t vrf_id, const IpPrefix &ip_prefix)
{
}

void IntfsOrch::addDirectedBroadcast(const Port &port, const IpPrefix &ip_prefix)
{
}

void IntfsOrch::removeDirectedBroadcast(const Port &port, const IpPrefix &ip_prefix)
{
}

void IntfsOrch::addRifToFlexCounter(const string &id, const string &name, const string &type)
{
}

void IntfsOrch::removeRifFromFlexCounter(const string &id, const string &name)
{
}

string IntfsOrch::getRifFlexCounterTableKey(string key)
{
    return "";
}

void IntfsOrch::generateInterfaceMap()
{
}

bool IntfsOrch::updateSyncdIntfPfx(const string &alias, const IpPrefix &ip_prefix, bool add)
{
    return true;
}

void IntfsOrch::doTask(SelectableTimer &timer)
{
}

bool IntfsOrch::isRemoteSystemPortIntf(string alias)
{
    return true;
}

bool IntfsOrch::isLocalSystemPortIntf(string alias)
{
    return true;
}

void IntfsOrch::voqSyncAddIntf(string &alias)
{
}

void IntfsOrch::voqSyncDelIntf(string &alias)
{
}

void IntfsOrch::voqSyncIntfState(string &alias, bool isUp)
{
}