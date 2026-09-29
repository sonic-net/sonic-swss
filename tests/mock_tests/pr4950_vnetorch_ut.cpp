// Release-local PR4950 fixture: no unrelated master VNet tests or production code.
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_orch_test.h"
#include "mock_table.h"
#include "sai_serialize.h"
#include "saihelper.h"
#include "swssnet.h"
#include "json.h"
#include <algorithm>
#include <hiredis/hiredis.h>
EXTERN_MOCK_FNS

extern redisReply *mockReply;

namespace vnetorch_test
{
using namespace std;
using namespace swss;
using namespace mock_orch_test;
class VNetOrchTest : public MockOrchTest
{
protected:
    static VNetOrchTest *active;
    sai_route_api_t routes{};
    sai_tunnel_api_t tunnelApi{};
    sai_next_hop_group_api_t groups{};
    unique_ptr<VNetRouteOrch> vnetRoutes;
    unique_ptr<VNetCfgRouteOrch> cfgRoutes;
    vector<sai_status_t> statuses;
    vector<sai_object_id_t> members;
    size_t routeRemoves = 0;
    bool split = false;
    struct Object { sai_object_id_t oid; int32_t type; };
    vector<Object> tunnels, maps;
    set<sai_object_id_t> removedTunnels, removedMaps;
    static int32_t type(uint32_t n, const sai_attribute_t *a, sai_attr_id_t id)
    {
        for (uint32_t i = 0; i < n; ++i) if (a[i].id == id) return a[i].value.s32;
        return -1;
    }
    static sai_status_t createRoutes(uint32_t n, const sai_route_entry_t *e, const uint32_t *c,
                                    const sai_attribute_t **a, sai_bulk_op_error_mode_t mode, sai_status_t *s)
    {
        sai_status_t result = SAI_STATUS_SUCCESS;
        if (active->split)
        {
            // Avoid libsaivs bulk prevalidation suppressing successful siblings.
            for (uint32_t i = 0; i < n; ++i)
            {
                s[i] = active->routes.create_route_entry(e + i, c[i], a[i]);
                if (s[i] != SAI_STATUS_SUCCESS) result = SAI_STATUS_FAILURE;
            }
        }
        else result = active->routes.create_route_entries(n, e, c, a, mode, s);
        active->statuses.assign(s, s + n);
        return result;
    }
    static sai_status_t removeRoutes(uint32_t n, const sai_route_entry_t *e,
                                    sai_bulk_op_error_mode_t mode, sai_status_t *s)
    {
        active->routeRemoves += n;
        return active->routes.remove_route_entries(n, e, mode, s);
    }
    static sai_status_t createMembers(sai_object_id_t sw, uint32_t n, const uint32_t *c,
                                     const sai_attribute_t **a, sai_bulk_op_error_mode_t mode,
                                     sai_object_id_t *ids, sai_status_t *s)
    {
        const auto result = active->groups.create_next_hop_group_members(sw, n, c, a, mode, ids, s);
        for (uint32_t i = 0; i < n; ++i)
            if (s[i] == SAI_STATUS_SUCCESS) active->members.push_back(ids[i]);
        return result;
    }
    static sai_status_t createTunnel(sai_object_id_t *id, sai_object_id_t sw, uint32_t n, const sai_attribute_t *a)
    {
        const auto result = active->tunnelApi.create_tunnel(id, sw, n, a);
        if (result == SAI_STATUS_SUCCESS) active->tunnels.push_back({*id, type(n, a, SAI_TUNNEL_ATTR_TYPE)});
        return result;
    }
    static sai_status_t createMap(sai_object_id_t *id, sai_object_id_t sw, uint32_t n, const sai_attribute_t *a)
    {
        const auto result = active->tunnelApi.create_tunnel_map_entry(id, sw, n, a);
        if (result == SAI_STATUS_SUCCESS)
            active->maps.push_back({*id, type(n, a, SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE)});
        return result;
    }
    static sai_status_t removeTunnel(sai_object_id_t id)
    {
        active->removedTunnels.insert(id);
        return active->tunnelApi.remove_tunnel(id);
    }
    static sai_status_t removeMap(sai_object_id_t id)
    {
        active->removedMaps.insert(id);
        return active->tunnelApi.remove_tunnel_map_entry(id);
    }
    void SetUp() override
    {
        testing_db::reset();
        active = this;
        MockOrchTest::SetUp();
    }
    void ApplySaiMock() override
    {
        // Bulkers bind these pointers during construction, before PostSetUp.
        routes = *sai_route_api;
        tunnelApi = *sai_tunnel_api;
        groups = *sai_next_hop_group_api;
        sai_route_api->create_route_entries = createRoutes;
        sai_route_api->remove_route_entries = removeRoutes;
        sai_next_hop_group_api->create_next_hop_group_members = createMembers;
        sai_tunnel_api->create_tunnel = createTunnel;
        sai_tunnel_api->create_tunnel_map_entry = createMap;
        sai_tunnel_api->remove_tunnel = removeTunnel;
        sai_tunnel_api->remove_tunnel_map_entry = removeMap;
    }
    void ApplyInitialConfigs() override
    {
        Table ports(m_app_db.get(), APP_PORT_TABLE_NAME);
        const auto initial = ut_helper::getInitialSaiPorts();
        for (const auto &p : initial) ports.set(p.first, p.second);
        ports.set("PortConfigDone", {{"count", to_string(initial.size())}});
        ports.set("PortInitDone", {{}});
        gPortsOrch->addExistingData(&ports);
        static_cast<Orch *>(gPortsOrch)->doTask();
    }
    void PostSetUp() override
    {
        TableConnector bfd(m_state_db.get(), STATE_BFD_SESSION_TABLE_NAME);
        gBfdOrch = new BfdOrch(m_app_db.get(), APP_BFD_SESSION_TABLE_NAME, bfd);
        vector<string> decap = {APP_TUNNEL_DECAP_TABLE_NAME, APP_TUNNEL_DECAP_TERM_TABLE_NAME};
        gTunneldecapOrch = new TunnelDecapOrch(m_app_db.get(), m_state_db.get(), m_config_db.get(), decap);
        vector<string> app = {APP_VNET_RT_TABLE_NAME, APP_VNET_RT_TUNNEL_TABLE_NAME};
        vnetRoutes = make_unique<VNetRouteOrch>(m_app_db.get(), app, m_vnetOrch);
        gDirectory.set(vnetRoutes.get());
        vector<string> cfg = {CFG_VNET_RT_TABLE_NAME, CFG_VNET_RT_TUNNEL_TABLE_NAME};
        cfgRoutes = make_unique<VNetCfgRouteOrch>(m_config_db.get(), m_app_db.get(), cfg);
    }
    void PreTearDown() override
    {
        vnetRoutes.reset();
        cfgRoutes.reset();
        delete gBfdOrch;
        gBfdOrch = nullptr;
        delete gTunneldecapOrch;
        gTunneldecapOrch = nullptr;
        *sai_route_api = routes;
        *sai_tunnel_api = tunnelApi;
        *sai_next_hop_group_api = groups;
        active = nullptr;
    }
    void setRow(Orch *o, DBConnector *db, const string &table, const string &key, const vector<FieldValueTuple> &f)
    {
        Table t(db, table);
        t.set(key, f);
        o->addExistingData(&t);
        o->doTask();
    }
    void delRow(Orch *o, const string &table, const string &key)
    {
        auto *c = dynamic_cast<Consumer *>(o->getExecutor(table));
        ASSERT_NE(c, nullptr);
        c->addToSync({{key, DEL_COMMAND, {}}});
        o->doTask(*c);
        Table t(m_app_db.get(), table);
        t.del(key);
    }
    void createOwner()
    {
        setRow(m_VxlanTunnelOrch, m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME, "tunnel", {{"src_ip", "10.10.10.10"}});
        setRow(m_vnetOrch, m_app_db.get(), APP_VNET_TABLE_NAME, "vnet",
               {{"vxlan_tunnel", "tunnel"}, {"vni", "2000"}, {"peer_list", ""}, {"scope", "default"}});
    }
    void ownerRoute(const string &prefix, const string &endpoint = "10.10.10.9")
    {
        setRow(cfgRoutes.get(), m_config_db.get(), CFG_VNET_RT_TUNNEL_TABLE_NAME, "vnet|" + prefix, {{"endpoint", endpoint}});
        Table t(m_app_db.get(), APP_VNET_RT_TUNNEL_TABLE_NAME);
        vnetRoutes->addExistingData(&t);
        static_cast<Orch *>(vnetRoutes.get())->doTask();
    }
    void delOwnerRoute(const string &p) { delRow(vnetRoutes.get(), APP_VNET_RT_TUNNEL_TABLE_NAME, "vnet:" + p); }
    void delRoute(const string &p) { delRow(gRouteOrch, APP_ROUTE_TABLE_NAME, p); }
    void deleteOwner()
    {
        delRow(m_vnetOrch, APP_VNET_TABLE_NAME, "vnet");
        delRow(m_VxlanTunnelOrch, APP_VXLAN_TUNNEL_TABLE_NAME, "tunnel");
    }
    void underlay()
    {
        Table t(m_app_db.get(), APP_INTF_TABLE_NAME);
        t.set("Ethernet0", {{"NULL", "NULL"}});
        t.set("Ethernet0:10.10.10.1/24", {{"scope", "global"}, {"family", "IPv4"}});
        gIntfsOrch->addExistingData(&t);
        static_cast<Orch *>(gIntfsOrch)->doTask();
        Port p;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", p));
        // updatePortOperStatus() alone updates this copy, not the PortsOrch
        // cache. Use the real notification path before learning neighbors, or
        // they inherit NHFLAGS_IFDOWN and routes never reach the SAI bulker.
        sai_port_oper_status_notification_t notification{};
        notification.port_id = p.m_port_id;
        notification.port_state = SAI_PORT_OPER_STATUS_UP;
        const auto data = sai_serialize_port_oper_status_ntf(1, &notification);
        string message = JSon::buildJson({{"port_state_change", data}});
        redisReply payload{}, reply{};
        payload.type = REDIS_REPLY_STRING;
        payload.str = &message[0];
        payload.len = message.size();
        redisReply *elements[] = {nullptr, nullptr, &payload};
        reply.type = REDIS_REPLY_ARRAY;
        reply.elements = 3;
        reply.element = elements;
        auto *exec = dynamic_cast<Notifier *>(gPortsOrch->getExecutor("PORT_STATUS_NOTIFICATIONS"));
        ASSERT_NE(exec, nullptr);
        auto *consumer = exec->getNotificationConsumer();
        ASSERT_NE(consumer, nullptr);
        ASSERT_EQ(mockReply, nullptr);
        mockReply = &reply;
        // mock_hiredis deep-copies the reply; no stack-backed data escapes.
        consumer->readData();
        mockReply = nullptr;
        static_cast<Orch *>(gPortsOrch)->doTask(*consumer);
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", p));
        EXPECT_EQ(p.m_oper_status, SAI_PORT_OPER_STATUS_UP);
        setRow(gNeighOrch, m_app_db.get(), APP_NEIGH_TABLE_NAME, "Ethernet0:10.10.10.3", {{"neigh", "00:00:00:00:00:03"}, {"family", "IPv4"}});
        setRow(gNeighOrch, m_app_db.get(), APP_NEIGH_TABLE_NAME, "Ethernet0:10.10.10.4", {{"neigh", "00:00:00:00:00:04"}, {"family", "IPv4"}});
        for (const auto &ip : {"10.10.10.3", "10.10.10.4"})
        {
            const NextHopKey nh(string(ip) + "@Ethernet0");
            ASSERT_TRUE(gNeighOrch->hasNextHop(nh));
            EXPECT_FALSE(gNeighOrch->isNextHopFlagSet(nh, NHFLAGS_IFDOWN)) << nh.to_string();
        }
    }
    sai_status_t readRoute(const string &p, sai_object_id_t &nh)
    {
        sai_route_entry_t e{};
        e.switch_id = gSwitchId;
        e.vr_id = gVirtualRouterId;
        copy(e.destination, IpPrefix(p));
        sai_attribute_t a{};
        a.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        const auto result = routes.get_route_entry_attribute(&e, 1, &a);
        nh = a.value.oid;
        return result;
    }
    void checkRoute(const string &p, sai_object_id_t expected)
    {
        sai_object_id_t nh;
        ASSERT_EQ(readRoute(p, nh), SAI_STATUS_SUCCESS);
        EXPECT_EQ(nh, expected);
    }
    void ownership(bool ecmp, bool withdraw)
    {
        const string p = "100.100.1.0/24";
        createOwner();
        ownerRoute(p, "10.10.10.3");
        sai_object_id_t owner;
        ASSERT_EQ(readRoute(p, owner), SAI_STATUS_SUCCESS);
        underlay();
        const NextHopKey n1("10.10.10.3@Ethernet0"), n2("10.10.10.4@Ethernet0");
        const NextHopGroupKey group(ecmp ? "10.10.10.3@Ethernet0,10.10.10.4@Ethernet0" : "10.10.10.3@Ethernet0");
        const auto refs1 = gNeighOrch->getNextHopRefCount(n1), refs2 = gNeighOrch->getNextHopRefCount(n2);
        const auto count = gRouteOrch->getNhgCount();
        auto used = []() { return Portal::CrmOrchInternal::getResourceMap(gCrmOrch).at(CrmResourceType::CRM_IPV4_ROUTE).countersMap.at("STATS").usedCounter; };
        const auto routeCount = used();
        auto *c = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        ASSERT_NE(c, nullptr);
        setSaiFailureStatus(false, "");
        statuses.clear();
        setRow(gRouteOrch, m_app_db.get(), APP_ROUTE_TABLE_NAME, p,
               {{"nexthop", ecmp ? "10.10.10.3,10.10.10.4" : "10.10.10.3"}, {"ifname", ecmp ? "Ethernet0,Ethernet0" : "Ethernet0"}});
        EXPECT_NE(find(statuses.begin(), statuses.end(), SAI_STATUS_ITEM_ALREADY_EXISTS), statuses.end());
        for (int retry = 0; retry < 3; ++retry)
        {
            SCOPED_TRACE(retry);
            EXPECT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix(p)));
            EXPECT_EQ(c->m_toSync.count(p), 1U);
            EXPECT_EQ(used(), routeCount);
            EXPECT_EQ(gNeighOrch->getNextHopRefCount(n1), refs1);
            EXPECT_EQ(gNeighOrch->getNextHopRefCount(n2), refs2);
            if (ecmp) { EXPECT_FALSE(gRouteOrch->hasNextHopGroup(group)); }
            EXPECT_EQ(gRouteOrch->getNhgCount(), count);
            checkRoute(p, owner);
            static_cast<Orch *>(gRouteOrch)->doTask(*c);
        }
        if (withdraw)
        {
            const auto removes = routeRemoves;
            delRoute(p);
            EXPECT_EQ(c->m_toSync.count(p), 0U);
            EXPECT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix(p)));
            EXPECT_EQ(routeRemoves, removes);
            EXPECT_EQ(used(), routeCount);
            checkRoute(p, owner);
            delOwnerRoute(p);
            static_cast<Orch *>(gRouteOrch)->doTask(*c);
            EXPECT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix(p)));
            sai_object_id_t nh;
            EXPECT_EQ(readRoute(p, nh), SAI_STATUS_ITEM_NOT_FOUND);
        }
        else
        {
            delOwnerRoute(p);
            EXPECT_EQ(used(), routeCount - 1);
            static_cast<Orch *>(gRouteOrch)->doTask(*c);
            EXPECT_EQ(c->m_toSync.count(p), 0U);
            ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix(p)));
            EXPECT_EQ(used(), routeCount);
            if (ecmp)
            {
                ASSERT_TRUE(gRouteOrch->hasNextHopGroup(group));
                checkRoute(p, gRouteOrch->getNextHopGroupId(group));
                EXPECT_EQ(gRouteOrch->getNextHopGroupRefCount(group), 1);
            }
            else checkRoute(p, gNeighOrch->getNextHopId(n1));
            delRoute(p);
        }
        EXPECT_EQ(used(), routeCount - 1);
        EXPECT_EQ(gNeighOrch->getNextHopRefCount(n1), refs1);
        EXPECT_EQ(gNeighOrch->getNextHopRefCount(n2), refs2);
        if (ecmp) { EXPECT_FALSE(gRouteOrch->hasNextHopGroup(group)); }
        EXPECT_EQ(gRouteOrch->getNhgCount(), count);
        string error;
        EXPECT_FALSE(getSaiFailureStatus(error)) << error;
        deleteOwner();
    }
    void resources(const string &kind, bool ecmp, bool sibling, bool mixed = false)
    {
        createOwner();
        ownerRoute("100.100.1.0/24");
        ownerRoute("100.100.2.0/24");
        sai_object_id_t owner;
        ASSERT_EQ(readRoute("100.100.1.0/24", owner), SAI_STATUS_SUCCESS);
        vector<FieldValueTuple> fields;
        NextHopGroupKey group;
        unique_ptr<EvpnNvoOrch> nvo;
        if (kind == "mpls")
        {
            underlay();
            fields = {{"nexthop", ecmp ? "10.10.10.3,10.10.10.4" : "10.10.10.3"}, {"ifname", ecmp ? "Ethernet0,Ethernet0" : "Ethernet0"}, {"mpls_nh", ecmp ? "push100,push200" : "push100"}};
            group = NextHopGroupKey(ecmp ? "push100+10.10.10.3@Ethernet0,push200+10.10.10.4@Ethernet0" : "push100+10.10.10.3@Ethernet0");
        }
        else if (kind == "overlay")
        {
            nvo = make_unique<EvpnNvoOrch>(m_app_db.get(), APP_VXLAN_EVPN_NVO_TABLE_NAME);
            gDirectory.set(nvo.get());
            setRow(nvo.get(), m_app_db.get(), APP_VXLAN_EVPN_NVO_TABLE_NAME, "nvo", {{"source_vtep", "tunnel"}});
            ASSERT_NE(nvo->getEVPNVtep(), nullptr);
            auto *c = dynamic_cast<Consumer *>(gVrfOrch->getExecutor(APP_VRF_TABLE_NAME));
            c->addToSync({{"Vrf_resources", SET_COMMAND, {{"vni", "5000"}}}});
            static_cast<Orch *>(gVrfOrch)->doTask(*c);
            ASSERT_TRUE(gVrfOrch->isL3VniVlan(5000));
            fields = {{"nexthop", ecmp ? "10.10.10.3,10.10.10.4" : "10.10.10.3"}, {"ifname", ecmp ? "5000,5000" : "5000"}, {"vni_label", ecmp ? "5000,5000" : "5000"}, {"router_mac", ecmp ? "00:00:00:00:00:03,00:00:00:00:00:04" : "00:00:00:00:00:03"}};
            group = NextHopGroupKey(ecmp ? "10.10.10.3@vni5000@5000@00:00:00:00:00:03,10.10.10.4@vni5000@5000@00:00:00:00:00:04" : "10.10.10.3@vni5000@5000@00:00:00:00:00:03", true);
        }
        else
        {
            fields = {{"nexthop", ecmp ? "2001:db8::3,2001:db8::4" : "2001:db8::3"}, {"ifname", ecmp ? "Ethernet0,Ethernet0" : "Ethernet0"}, {"seg_src", ecmp ? "2001:db8::1,2001:db8::1" : "2001:db8::1"}, {"vpn_sid", ecmp ? "2001:db8:1::3,2001:db8:1::4" : "2001:db8:1::3"}};
            group = NextHopGroupKey(ecmp ? "2001:db8::3@@2001:db8::1@2001:db8:1::3@,2001:db8::4@@2001:db8::1@2001:db8:1::4@" : "2001:db8::3@@2001:db8::1@2001:db8:1::3@", false, true);
        }
        auto *c = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
        ASSERT_NE(c, nullptr);
        const auto count = gRouteOrch->getNhgCount();
        auto prepare = [&]() {
            if (nvo && ecmp)
                for (const auto &nh : group.getNextHops())
                    if (!gNeighOrch->hasNextHop(nh))
                    {
                        ASSERT_TRUE(gRouteOrch->createRemoteVtep(gVirtualRouterId, nh));
                        ASSERT_NE(gNeighOrch->addTunnelNextHop(nh), SAI_NULL_OBJECT_ID);
                    }
        };
        auto check = [&](bool inUse) {
            EXPECT_EQ(gRouteOrch->getNhgCount(), count + (inUse && ecmp ? 1 : 0));
            if (ecmp) { EXPECT_EQ(gRouteOrch->hasNextHopGroup(group), inUse); }
            for (const auto &nh : group.getNextHops())
            {
                EXPECT_EQ(gNeighOrch->hasNextHop(nh), inUse) << nh.to_string();
                if (inUse && gNeighOrch->hasNextHop(nh))
                {
                    EXPECT_EQ(gNeighOrch->getNextHopRefCount(nh), 1);
                    sai_attribute_t a{};
                    a.id = SAI_NEXT_HOP_ATTR_TYPE;
                    EXPECT_EQ(sai_next_hop_api->get_next_hop_attribute(gNeighOrch->getNextHopId(nh), 1, &a), SAI_STATUS_SUCCESS);
                }
                if (nvo) { EXPECT_EQ(nvo->getEVPNVtep()->getRemoteEndPointIPRefCnt(nh.ip_address.to_string()), inUse ? 1 : -1); }
            }
            if (kind == "srv6")
            {
                size_t live = 0;
                for (const auto &m : maps)
                    if (m.type == SAI_TUNNEL_MAP_TYPE_PREFIX_AGG_ID_TO_SRV6_VPN_SID && !removedMaps.count(m.oid)) ++live;
                EXPECT_EQ(live, inUse ? group.getSize() : 0U);
                for (const auto &t : tunnels)
                    if (t.type == SAI_TUNNEL_TYPE_SRV6 && !inUse) { EXPECT_EQ(removedTunnels.count(t.oid), 1U); }
            }
        };
        setSaiFailureStatus(false, "");
        auto first = fields;
        if (mixed) for (auto &f : first) fvValue(f) = fvValue(f).substr(0, fvValue(f).find(','));
        c->addToSync({{"100.100.1.0/24", SET_COMMAND, first}, {"100.100.2.0/24", SET_COMMAND, fields}});
        if (sibling) c->addToSync({{"100.100.3.0/24", SET_COMMAND, fields}});
        split = true;
        for (int retry = 0; retry < 3; ++retry)
        {
            SCOPED_TRACE(retry);
            prepare();
            static_cast<Orch *>(gRouteOrch)->doTask(*c);
            ASSERT_GE(statuses.size(), 2U);
            EXPECT_EQ(statuses[0], SAI_STATUS_ITEM_ALREADY_EXISTS);
            EXPECT_EQ(statuses[1], SAI_STATUS_ITEM_ALREADY_EXISTS);
            EXPECT_EQ(c->m_toSync.size(), 2U);
            EXPECT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("100.100.1.0/24")));
            EXPECT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("100.100.2.0/24")));
            checkRoute("100.100.1.0/24", owner);
            checkRoute("100.100.2.0/24", owner);
            check(sibling);
            if (sibling)
            {
                ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("100.100.3.0/24")));
                checkRoute("100.100.3.0/24", ecmp ? gRouteOrch->getNextHopGroupId(group) : gNeighOrch->getNextHopId(*group.getNextHops().begin()));
            }
        }
        prepare();
        delRoute("100.100.1.0/24");
        checkRoute("100.100.1.0/24", owner);
        check(sibling);
        if (sibling)
        {
            delRoute("100.100.2.0/24");
            checkRoute("100.100.2.0/24", owner);
            check(true);
            delRoute("100.100.3.0/24");
        }
        else
        {
            delOwnerRoute("100.100.2.0/24");
            prepare();
            static_cast<Orch *>(gRouteOrch)->doTask(*c);
            ASSERT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("100.100.2.0/24")));
            check(true);
            delRoute("100.100.2.0/24");
        }
        EXPECT_TRUE(c->m_toSync.empty());
        check(false);
        string error;
        EXPECT_FALSE(getSaiFailureStatus(error)) << error;
        delOwnerRoute("100.100.1.0/24");
        if (sibling) delOwnerRoute("100.100.2.0/24");
        delRow(m_vnetOrch, APP_VNET_TABLE_NAME, "vnet");
        if (nvo) delRow(gVrfOrch, APP_VRF_TABLE_NAME, "Vrf_resources");
        delRow(m_VxlanTunnelOrch, APP_VXLAN_TUNNEL_TABLE_NAME, "tunnel");
    }
};
VNetOrchTest *VNetOrchTest::active = nullptr;
TEST_F(VNetOrchTest, VnetDuplicateRouteHandledGracefully) { ownership(false, true); }
TEST_F(VNetOrchTest, VnetDuplicateRouteWithdrawalPreservesVnet) { ownership(false, true); }
TEST_F(VNetOrchTest, VnetDuplicateEcmpRouteWithdrawalPreservesVnet) { ownership(true, true); }
TEST_F(VNetOrchTest, VnetDuplicateRouteRetriesAfterVnetRemoval) { ownership(false, false); }
TEST_F(VNetOrchTest, VnetDuplicateEcmpRouteRetriesAfterVnetRemoval) { ownership(true, false); }
TEST_F(VNetOrchTest, VnetDuplicateRoutePreservesSharedEcmpGroup)
{
    createOwner();
    ownerRoute("100.100.1.0/24", "10.10.10.3");
    sai_object_id_t owner;
    ASSERT_EQ(readRoute("100.100.1.0/24", owner), SAI_STATUS_SUCCESS);
    underlay();
    auto *c = dynamic_cast<Consumer *>(gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
    ASSERT_NE(c, nullptr);
    const vector<FieldValueTuple> f = {{"nexthop", "10.10.10.3,10.10.10.4"}, {"ifname", "Ethernet0,Ethernet0"}};
    c->addToSync({{"100.100.1.0/24", SET_COMMAND, f}, {"100.100.2.0/24", SET_COMMAND, f}});
    split = true;
    static_cast<Orch *>(gRouteOrch)->doTask(*c);
    split = false;
    ASSERT_EQ(statuses.size(), 2U);
    EXPECT_EQ(statuses[0], SAI_STATUS_ITEM_ALREADY_EXISTS);
    EXPECT_EQ(statuses[1], SAI_STATUS_SUCCESS);
    const NextHopGroupKey group("10.10.10.3@Ethernet0,10.10.10.4@Ethernet0");
    EXPECT_FALSE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("100.100.1.0/24")));
    EXPECT_TRUE(gRouteOrch->isRouteExists(gVirtualRouterId, IpPrefix("100.100.2.0/24")));
    EXPECT_EQ(c->m_toSync.count("100.100.1.0/24"), 1U);
    EXPECT_EQ(c->m_toSync.count("100.100.2.0/24"), 0U);
    ASSERT_TRUE(gRouteOrch->hasNextHopGroup(group));
    EXPECT_EQ(gRouteOrch->getNextHopGroupRefCount(group), 1);
    const auto id = gRouteOrch->getNextHopGroupId(group);
    checkRoute("100.100.2.0/24", id);
    set<sai_object_id_t> live;
    for (const auto member : members)
    {
        sai_attribute_t a[2]{};
        a[0].id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
        a[1].id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
        ASSERT_EQ(groups.get_next_hop_group_member_attribute(member, 2, a), SAI_STATUS_SUCCESS);
        if (a[0].value.oid == id) live.insert(a[1].value.oid);
    }
    EXPECT_EQ(live.size(), 2U);
    for (const auto &nh : group.getNextHops()) { EXPECT_EQ(live.count(gNeighOrch->getNextHopId(nh)), 1U); }
    delRoute("100.100.1.0/24");
    checkRoute("100.100.1.0/24", owner);
    delRoute("100.100.2.0/24");
    EXPECT_FALSE(gRouteOrch->hasNextHopGroup(group));
    delOwnerRoute("100.100.1.0/24");
    deleteOwner();
}
TEST_F(VNetOrchTest, VnetDuplicateMplsResources) { resources("mpls", false, false); }
TEST_F(VNetOrchTest, VnetDuplicateMplsSharedResources) { resources("mpls", false, true); }
TEST_F(VNetOrchTest, VnetDuplicateMplsEcmpResources) { resources("mpls", true, false); }
TEST_F(VNetOrchTest, VnetDuplicateMplsEcmpSharedResources) { resources("mpls", true, true); }
TEST_F(VNetOrchTest, VnetDuplicateOverlayResources) { resources("overlay", false, false); }
TEST_F(VNetOrchTest, VnetDuplicateOverlaySharedResources) { resources("overlay", false, true); }
TEST_F(VNetOrchTest, VnetDuplicateOverlayEcmpResources) { resources("overlay", true, false); }
TEST_F(VNetOrchTest, VnetDuplicateOverlayEcmpSharedResources) { resources("overlay", true, true); }
TEST_F(VNetOrchTest, VnetDuplicateSrv6Resources) { resources("srv6", false, false); }
TEST_F(VNetOrchTest, VnetDuplicateSrv6SharedResources) { resources("srv6", false, true); }
TEST_F(VNetOrchTest, VnetDuplicateSrv6EcmpResources) { resources("srv6", true, false); }
TEST_F(VNetOrchTest, VnetDuplicateSrv6EcmpSharedResources) { resources("srv6", true, true); }
TEST_F(VNetOrchTest, VnetDuplicateMplsMixedSingleEcmpResources) { resources("mpls", true, false, true); }
TEST_F(VNetOrchTest, VnetDuplicateOverlayMixedSingleEcmpResources) { resources("overlay", true, false, true); }
TEST_F(VNetOrchTest, VnetDuplicateSrv6MixedSingleEcmpResources) { resources("srv6", true, false, true); }
}
