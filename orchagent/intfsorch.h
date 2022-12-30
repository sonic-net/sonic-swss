#ifndef SWSS_INTFSORCH_H
#define SWSS_INTFSORCH_H

#include "orch.h"
#include "portsorch.h"
#include "vrforch.h"
#include "timer.h"

#include "ipaddresses.h"
#include "ipprefix.h"
#include "macaddress.h"

#include <map>
#include <set>

extern sai_object_id_t gVirtualRouterId;
extern MacAddress gMacAddress;

#define RIF_STAT_COUNTER_FLEX_COUNTER_GROUP "RIF_STAT_COUNTER"
#define RIF_RATE_COUNTER_FLEX_COUNTER_GROUP "RIF_RATE_COUNTER"

struct IntfsEntry
{
    std::set<IpPrefix>  ip_addresses;
    int                 ref_count;
    sai_object_id_t     vrf_id;
    bool                proxy_arp;
};

typedef map<string, IntfsEntry> IntfsTable;

class IntfsOrch : public Orch
{
public:
    IntfsOrch(DBConnector *db, string tableName, VRFOrch *vrf_orch, DBConnector *chassisAppDb);

    sai_object_id_t getRouterIntfsId(const string&);
    bool isPrefixSubnet(const IpPrefix&, const string&);
    bool isInbandIntfInMgmtVrf(const string& alias);
    string getRouterIntfsAlias(const IpAddress &ip, const string &vrf_name = "");
    string getRifRateFlexCounterTableKey(string key);
    void increaseRouterIntfsRefCount(const string&);
    void decreaseRouterIntfsRefCount(const string&);

    bool setRouterIntfsMtu(const Port &port);
    bool setRouterIntfsMac(const Port &port);
    bool setRouterIntfsNatZoneId(Port &port);
    bool setRouterIntfsAdminStatus(const Port &port);
    bool setRouterIntfsMpls(const Port &port);

    std::set<IpPrefix> getSubnetRoutes();

    void generateInterfaceMap();
    void addRifToFlexCounter(const string&, const string&, const string&);
    void removeRifFromFlexCounter(const string&, const string&);

    bool setIntfLoopbackAction(const Port &port, string actionStr);
    bool getSaiLoopbackAction(const string &actionStr, sai_packet_action_t &action);
    bool setIntf(const string& alias, sai_object_id_t vrf_id = gVirtualRouterId, const IpPrefix *ip_prefix = nullptr, const bool adminUp = true, const uint32_t mtu = 0, string loopbackAction = "");
    bool removeIntf(const string& alias, sai_object_id_t vrf_id = gVirtualRouterId, const IpPrefix *ip_prefix = nullptr);

    void addIp2MeRoute(sai_object_id_t vrf_id, const IpPrefix &ip_prefix);
    void removeIp2MeRoute(sai_object_id_t vrf_id, const IpPrefix &ip_prefix);

    const IntfsTable& getSyncdIntfses(void)
    {
        return m_syncdIntfses;
    }

    bool updateSyncdIntfPfx(const string &alias, const IpPrefix &ip_prefix, bool add = true);

    bool isRemoteSystemPortIntf(string alias);
    bool isLocalSystemPortIntf(string alias);

private:

    SelectableTimer* m_updateMapsTimer = nullptr;
    std::vector<Port> m_rifsToAdd;

    VRFOrch *m_vrfOrch;
    IntfsTable m_syncdIntfses;
    map<string, string> m_vnetInfses;
    void doTask(Consumer &consumer);
    void doTask(SelectableTimer &timer);

    shared_ptr<DBConnector> m_counter_db;
    shared_ptr<DBConnector> m_flex_db;
    shared_ptr<DBConnector> m_asic_db;
    unique_ptr<Table> m_rifNameTable;
    unique_ptr<Table> m_rifTypeTable;
    unique_ptr<Table> m_vidToRidTable;
    unique_ptr<ProducerTable> m_flexCounterTable;
    unique_ptr<ProducerTable> m_flexCounterGroupTable;

    std::string getRifFlexCounterTableKey(std::string s);

    bool addRouterIntfs(sai_object_id_t vrf_id, Port &port, string loopbackAction);
    bool removeRouterIntfs(Port &port);

    void addDirectedBroadcast(const Port &port, const IpPrefix &ip_prefix);
    void removeDirectedBroadcast(const Port &port, const IpPrefix &ip_prefix);

    bool setIntfVlanFloodType(const Port &port, sai_vlan_flood_control_type_t vlan_flood_type);
    bool setIntfProxyArp(const string &alias, const string &proxy_arp);

    unique_ptr<Table> m_tableVoqSystemInterfaceTable;
    void voqSyncAddIntf(string &alias);
    void voqSyncDelIntf(string &alias);

};

#endif /* SWSS_INTFSORCH_H */
