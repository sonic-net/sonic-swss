#include <unordered_set>

#include "intfsorch.h"

IntfsOrch::IntfsOrch(DBConnector* db, string tableName, VRFOrch* vrf_orch,
                     DBConnector* chassisAppDb)
    : Orch(db, tableName), m_vrfOrch(vrf_orch) {}

sai_object_id_t IntfsOrch::getRouterIntfsId(const string& alias) {
  return SAI_NULL_OBJECT_ID;
}

bool IntfsOrch::isPrefixSubnet(const IpPrefix& ip_prefix, const string& alias) {
  return true;
}

string IntfsOrch::getRouterIntfsAlias(const IpAddress& ip,
                                      const string& vrf_name) {
  return string();
}

bool IntfsOrch::isInbandIntfInMgmtVrf(const string& alias) { return false; }

void IntfsOrch::increaseRouterIntfsRefCount(const string& alias) {}

void IntfsOrch::decreaseRouterIntfsRefCount(const string& alias) {}

bool IntfsOrch::setRouterIntfsMpls(const Port& port) { return true; }

bool IntfsOrch::setRouterIntfsMtu(const Port& port) { return true; }

bool IntfsOrch::setRouterIntfsMac(const Port& port) { return true; }

bool IntfsOrch::setRouterIntfsNatZoneId(Port& port) { return true; }

bool IntfsOrch::setRouterIntfsAdminStatus(const Port& port) { return true; }

bool IntfsOrch::setIntfVlanFloodType(
    const Port& port, sai_vlan_flood_control_type_t vlan_flood_type) {
  return true;
}

bool IntfsOrch::setIntfProxyArp(const string& alias, const string& proxy_arp) {
  return true;
}

set<IpPrefix> IntfsOrch::getSubnetRoutes() {
  set<IpPrefix> ip_prefix;
  return ip_prefix;
}

bool IntfsOrch::setIntf(const string& alias, sai_object_id_t vrf_id,
                        const IpPrefix* ip_prefix, const bool adminUp,
                        const uint32_t mtu, string loopbackAction) {
  return true;
}

bool IntfsOrch::removeIntf(const string& alias, sai_object_id_t vrf_id,
                           const IpPrefix* ip_prefix) {
  return true;
}

void IntfsOrch::doTask(Consumer& consumer) {}

bool IntfsOrch::addRouterIntfs(sai_object_id_t vrf_id, Port& port,
                               string loopbackAction) {
  return true;
}

bool IntfsOrch::removeRouterIntfs(Port& port) { return true; }

void IntfsOrch::addIp2MeRoute(sai_object_id_t vrf_id,
                              const IpPrefix& ip_prefix) {}

void IntfsOrch::removeIp2MeRoute(sai_object_id_t vrf_id,
                                 const IpPrefix& ip_prefix) {}

void IntfsOrch::addDirectedBroadcast(const Port& port,
                                     const IpPrefix& ip_prefix) {}

void IntfsOrch::removeDirectedBroadcast(const Port& port,
                                        const IpPrefix& ip_prefix) {}

void IntfsOrch::addRifToFlexCounter(const string& id, const string& name,
                                    const string& type) {}

void IntfsOrch::removeRifFromFlexCounter(const string& id, const string& name) {
}

string IntfsOrch::getRifFlexCounterTableKey(string key) { return string(); }

void IntfsOrch::generateInterfaceMap() { m_updateMapsTimer->start(); }

bool IntfsOrch::updateSyncdIntfPfx(const string& alias,
                                   const IpPrefix& ip_prefix, bool add) {
  return false;
}

void IntfsOrch::doTask(SelectableTimer& timer) {}

bool IntfsOrch::isRemoteSystemPortIntf(string alias) { return false; }

void IntfsOrch::voqSyncAddIntf(string& alias) {}

void IntfsOrch::voqSyncDelIntf(string& alias) {}
