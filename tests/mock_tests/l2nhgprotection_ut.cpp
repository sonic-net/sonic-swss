#include <gtest/gtest.h>
#include <map>
#include <set>
#include <memory>
#include <functional>
#include <limits>
#include <stdexcept>
#define private public
#define protected public
#include "l2nhgorch.h"
#include "lib/fpm_protection_ids.h"
#include "bpprotorch.h"
#include "vxlanorch.h"
#include "routeorch.h"
#include "portsorch.h"
#include "stporch.h"
#include "mlagorch.h"
#include "fdborch.h"
#include "neighorch.h"
#undef protected
#undef private
#include "mock_orch_test.h"
#include "mock_sai_api.h"
#include "mock_table.h"

extern Directory<Orch*> gDirectory;
extern RouteOrch *gRouteOrch;
extern PortsOrch *gPortsOrch;
extern StpOrch *gStpOrch;
extern MlagOrch *gMlagOrch;
extern FdbOrch *gFdbOrch;
extern NeighOrch *gNeighOrch;
extern BpProtOrch *gBpProtOrch;

namespace
{
string fault;
sai_status_t faultStatus = SAI_STATUS_FAILURE;
map<string, unsigned> calls;
sai_object_id_t sequence;
map<sai_object_id_t, sai_object_id_t> nhTunnels, gmGroups, gmHops;
std::set<sai_object_id_t> failedGroupMembers;
vector<sai_object_id_t> memberRemovalAttempts;
std::function<void()> memberCreateHook;
std::function<void()> rmacSetHook;
std::set<sai_object_id_t> failedRmacTunnels;
vector<pair<sai_object_id_t, MacAddress>> rmacWrites;
std::set<sai_object_id_t> tunnels, groups;
map<sai_object_id_t, MacAddress> rmacs;
decltype(sai_switch_api->get_switch_attribute) originalGetSwitch = nullptr;
bool fail(const string &stage) { ++calls[stage]; return fault == stage; }
sai_status_t createTunnel(sai_object_id_t *oid, sai_object_id_t, uint32_t count, const sai_attribute_t *attrs)
{
    bool p2p = false;
    for (uint32_t i=0; i<count; ++i)
        if (attrs[i].id == SAI_TUNNEL_ATTR_PEER_MODE) p2p = attrs[i].value.s32 == SAI_TUNNEL_PEER_MODE_P2P;
    EXPECT_TRUE(p2p);
    if (fail("tunnel-create")) return faultStatus;
    *oid = ++sequence; tunnels.insert(*oid); return SAI_STATUS_SUCCESS;
}
sai_status_t removeTunnel(sai_object_id_t oid)
{
    if (fail("tunnel-remove"))
    {
        if (faultStatus == SAI_STATUS_ITEM_NOT_FOUND) { tunnels.erase(oid); rmacs.erase(oid); }
        return faultStatus;
    }
    for (auto nh : nhTunnels) if (nh.second == oid) return SAI_STATUS_OBJECT_IN_USE;
    tunnels.erase(oid); rmacs.erase(oid); return SAI_STATUS_SUCCESS;
}
sai_status_t setRmac(sai_object_id_t oid, const sai_attribute_t *attr)
{
    EXPECT_EQ(attr->id, SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC);
    rmacWrites.emplace_back(oid, MacAddress(attr->value.mac));
    if (rmacSetHook) { auto hook=std::move(rmacSetHook); hook(); }
    if (fail("rmac-set") || failedRmacTunnels.count(oid)) return faultStatus;
    rmacs[oid] = MacAddress(attr->value.mac); return SAI_STATUS_SUCCESS;
}
sai_status_t getSwitch(sai_object_id_t oid, uint32_t count, sai_attribute_t *attrs)
{
    if (count != 1 || attrs->id != SAI_SWITCH_ATTR_VXLAN_DEFAULT_ROUTER_MAC)
        return originalGetSwitch(oid, count, attrs);
    if (fail("default-get")) return SAI_STATUS_FAILURE;
    MacAddress mac("02:00:00:00:00:aa"); mac.getMac(attrs->value.mac); return SAI_STATUS_SUCCESS;
}
sai_status_t createHop(sai_object_id_t *oid, sai_object_id_t, uint32_t count, const sai_attribute_t *attrs)
{
    EXPECT_EQ(count, 3u);
    EXPECT_EQ(attrs[0].id, SAI_NEXT_HOP_ATTR_TYPE);
    EXPECT_EQ(attrs[0].value.s32, SAI_NEXT_HOP_TYPE_BRIDGE_PORT);
    EXPECT_EQ(attrs[1].id, SAI_NEXT_HOP_ATTR_IP);
    EXPECT_EQ(attrs[2].id, SAI_NEXT_HOP_ATTR_TUNNEL_ID);
    EXPECT_TRUE(tunnels.count(attrs[2].value.oid));
    if (fail("nh-create")) return faultStatus;
    *oid = ++sequence; nhTunnels[*oid] = attrs[2].value.oid; return SAI_STATUS_SUCCESS;
}
sai_status_t removeHop(sai_object_id_t oid)
{
    if (fail("nh-remove")) return SAI_STATUS_FAILURE;
    for (auto gm : gmHops) if (gm.second == oid) return SAI_STATUS_OBJECT_IN_USE;
    nhTunnels.erase(oid); return SAI_STATUS_SUCCESS;
}
sai_status_t createGroup(sai_object_id_t *oid, sai_object_id_t, uint32_t count, const sai_attribute_t *attrs)
{
    EXPECT_EQ(count, 1u); EXPECT_EQ(attrs->id, SAI_NEXT_HOP_GROUP_ATTR_TYPE);
    EXPECT_EQ(attrs->value.s32, SAI_NEXT_HOP_GROUP_TYPE_BRIDGE_PORT);
    if (fail("group-create")) return faultStatus;
    *oid = ++sequence; groups.insert(*oid); return SAI_STATUS_SUCCESS;
}
sai_status_t removeGroup(sai_object_id_t oid)
{
    if (fail("group-remove")) return faultStatus;
    for (auto gm : gmGroups) if (gm.second == oid) return SAI_STATUS_OBJECT_IN_USE;
    groups.erase(oid); return SAI_STATUS_SUCCESS;
}
sai_status_t createGroupMember(sai_object_id_t *oid, sai_object_id_t, uint32_t count, const sai_attribute_t *attrs)
{
    EXPECT_EQ(count, 2u); EXPECT_TRUE(groups.count(attrs[0].value.oid));
    EXPECT_TRUE(nhTunnels.count(attrs[1].value.oid));
    if (memberCreateHook) { auto hook=std::move(memberCreateHook); hook(); }
    if (fail("member-create")) return SAI_STATUS_FAILURE;
    *oid = ++sequence; gmGroups[*oid] = attrs[0].value.oid; gmHops[*oid] = attrs[1].value.oid;
    return SAI_STATUS_SUCCESS;
}
sai_status_t removeGroupMember(sai_object_id_t oid)
{
    memberRemovalAttempts.push_back(oid);
    if (fail("member-remove") || failedGroupMembers.count(oid)) return faultStatus;
    gmGroups.erase(oid); gmHops.erase(oid); return SAI_STATUS_SUCCESS;
}
}

class L2ProtectionTest : public mock_orch_test::MockOrchTest
{
protected:
    std::unique_ptr<L2NhgOrch> builder;
    std::unique_ptr<EvpnNvoOrch> nvo;
    VxlanTunnel *source = nullptr;
    sai_tunnel_api_t savedTunnel{};
    sai_next_hop_api_t savedHop{};
    sai_next_hop_group_api_t savedGroup{};
    sai_switch_api_t savedSwitch{};
    void PostSetUp() override
    {
        Table ids(m_app_db.get(), FPM_PROTECTION_ID_TABLE);
        vector<string> recorded; ids.getKeys(recorded);
        for (const auto &key : recorded) ids.del(key);
        fault.clear(); faultStatus=SAI_STATUS_FAILURE; calls.clear(); sequence = 0x100000;
        nhTunnels.clear(); gmGroups.clear(); gmHops.clear(); tunnels.clear(); groups.clear(); rmacs.clear();
        failedGroupMembers.clear(); memberRemovalAttempts.clear();
        memberCreateHook={};
        rmacSetHook={}; failedRmacTunnels.clear(); rmacWrites.clear();
        savedTunnel=*sai_tunnel_api; savedHop=*sai_next_hop_api;
        savedGroup=*sai_next_hop_group_api; savedSwitch=*sai_switch_api;
        originalGetSwitch=sai_switch_api->get_switch_attribute;
        sai_tunnel_api->create_tunnel=createTunnel; sai_tunnel_api->remove_tunnel=removeTunnel;
        sai_tunnel_api->set_tunnel_attribute=setRmac; sai_switch_api->get_switch_attribute=getSwitch;
        sai_next_hop_api->create_next_hop=createHop; sai_next_hop_api->remove_next_hop=removeHop;
        sai_next_hop_group_api->create_next_hop_group=createGroup;
        sai_next_hop_group_api->remove_next_hop_group=removeGroup;
        sai_next_hop_group_api->create_next_hop_group_member=createGroupMember;
        sai_next_hop_group_api->remove_next_hop_group_member=removeGroupMember;
        nvo.reset(new EvpnNvoOrch(m_app_db.get(), APP_VXLAN_EVPN_NVO_TABLE_NAME)); gDirectory.set(nvo.get());
        source = new VxlanTunnel("source", IpAddress("10.0.0.1"), IpAddress("0.0.0.0"), TNL_CREATION_SRC_CLI);
        source->active_=true; m_VxlanTunnelOrch->addTunnel("source", source); nvo->source_vtep_ptr=source;
        m_VxlanTunnelOrch->is_dip_tunnel_supported=true;
        builder.reset(new L2NhgOrch(m_app_db.get(), APP_L2_NEXTHOP_GROUP_TABLE_NAME));
        ASSERT_GT(gRouteOrch->getMaxNhgCount(), 0u);
    }
    void PreTearDown() override
    {
        fault.clear(); source->active_=true; source->del_tnl_hw_pending=false;
        failedGroupMembers.clear();
        memberCreateHook={};
        rmacSetHook={}; failedRmacTunnels.clear();
        nvo->source_vtep_ptr=source; m_VxlanTunnelOrch->is_dip_tunnel_supported=true;
        for (auto &g : builder->m_protectionGroups) g.second.deleted=true;
        for (auto &m : builder->m_protectionMembers) m.second.deleted=true;
        for (int i=0;i<4;++i) step();
        EXPECT_TRUE(nhTunnels.empty()); EXPECT_TRUE(gmGroups.empty()); EXPECT_TRUE(tunnels.empty());
        builder.reset(); nvo.reset();
        *sai_tunnel_api=savedTunnel; *sai_next_hop_api=savedHop;
        *sai_next_hop_group_api=savedGroup; *sai_switch_api=savedSwitch;
    }
    void member(const string &id, const string &ip="10.0.0.2", const string &mac="02:00:00:00:00:01", const string &type="l3")
    {
        builder->updateProtectionRow({id, SET_COMMAND, {{"remote_vtep",ip},{"router_mac",mac},{"type",type}}});
    }
    void group(const string &id, const string &members)
    {
        builder->m_fpmL2Ids.set(id, {{"NULL","NULL"}});
        builder->updateProtectionRow({id, SET_COMMAND, {{"nexthop_group",members}}});
    }
    void step() { builder->reconcileProtection(); }
    void install() { member("1073741825"); group("2147483649","1073741825"); step(); }
};

TEST_F(L2ProtectionTest, ProtectionOnlyCreatesPinsAndReleasesWithoutImet)
{
    install(); ASSERT_TRUE(builder->hasActiveL2Nhg("2147483649"));
    EXPECT_EQ(source->tnl_users_.at("10.0.0.2").protection_refcnt,1u);
    EXPECT_EQ(source->getRemoteEndPointIPRefCnt("10.0.0.2"),0);
    EXPECT_EQ(source->getRemoteEndPointIMRRefCnt("10.0.0.2"),0);
    EXPECT_EQ(source->getRemoteEndPointRefCnt("10.0.0.2"),1);
    VxlanTunnelRequest request; request.parse({"source",DEL_COMMAND,{}});
    EXPECT_FALSE(m_VxlanTunnelOrch->delOperation(request));
    builder->deleteProtectionRow("1073741825"); builder->deleteProtectionRow("2147483649"); step(); step();
    EXPECT_TRUE(nhTunnels.empty()); EXPECT_TRUE(tunnels.empty()); EXPECT_TRUE(source->tnl_users_.empty());
    EXPECT_EQ(builder->getL2NhgCount(),0u);
}

TEST_F(L2ProtectionTest, CompatibleContextsAndGroupsShareOneHopAndPin)
{
    install(); member("1073741826"); group("2147483650","1073741826"); step();
    EXPECT_EQ(nhTunnels.size(),1u); EXPECT_EQ(tunnels.size(),1u); EXPECT_EQ(gmHops.size(),2u);
    EXPECT_EQ(source->tnl_users_.at("10.0.0.2").protection_refcnt,1u);
    EXPECT_EQ(builder->m_protectionPeers.at("10.0.0.2").owners.size(),2u);
    auto count=calls["nh-create"]; step(); EXPECT_EQ(calls["nh-create"],count);
    builder->deleteProtectionRow("1073741825"); builder->deleteProtectionRow("2147483649"); step();
    EXPECT_EQ(nhTunnels.size(),1u); EXPECT_TRUE(builder->hasActiveL2Nhg("2147483650"));
}

TEST_F(L2ProtectionTest, ConflictingRmacIsPendingAndOriginalContextSurvives)
{
    install(); member("1073741826","10.0.0.2","02:00:00:00:00:02"); group("2147483650","1073741826"); step();
    EXPECT_EQ(nhTunnels.size(),1u); EXPECT_EQ(tunnels.size(),1u);
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649")); EXPECT_FALSE(builder->hasActiveL2Nhg("2147483650"));
    builder->deleteProtectionRow("1073741825"); step(); step();
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483650")); EXPECT_EQ(nhTunnels.size(),1u);
}

TEST_F(L2ProtectionTest, ArrivalOrderMissingRmacL2AndEmptyGroupsStayUnready)
{
    group("2147483649","1073741825"); group("2147483650",""); step();
    auto oid=builder->m_protectionGroups.at("2147483649").oid;
    EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649")); EXPECT_FALSE(builder->hasActiveL2Nhg("2147483650"));
    member("1073741825","10.0.0.2",""); step(); EXPECT_TRUE(tunnels.empty());
    member("1073741825","10.0.0.2","","l2"); step(); EXPECT_TRUE(tunnels.empty());
    member("1073741825"); step(); EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649"));
    EXPECT_EQ(builder->m_protectionGroups.at("2147483649").oid,oid);
    group("2147483649",""); step(); EXPECT_EQ(builder->m_protectionGroups.at("2147483649").oid,oid);
    EXPECT_EQ(builder->getNumL2NhgNextHops("2147483649"),0u);
    EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649"));
}

TEST_F(L2ProtectionTest, MembershipChurnAndMemberReplacementPreserveGroupOid)
{
    install(); auto oid=builder->m_protectionGroups.at("2147483649").oid;
    member("1073741826","10.0.0.3"); group("2147483649","1073741825,1073741826"); step();
    EXPECT_EQ(builder->getNumL2NhgNextHops("2147483649"),2u);
    group("2147483649","1073741826"); step(); EXPECT_EQ(builder->getNumL2NhgNextHops("2147483649"),1u);
    member("1073741826","10.0.0.4","02:00:00:00:00:02"); step(); step();
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649")); EXPECT_EQ(builder->m_protectionGroups.at("2147483649").oid,oid);
    EXPECT_EQ(builder->m_protectionMembers.at("1073741826").peer,"10.0.0.4");
    member("1073741826","10.0.0.4","","l2"); step(); step();
    EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649")); EXPECT_EQ(builder->getNumL2NhgNextHops("2147483649"),0u);
}

TEST_F(L2ProtectionTest, ExistingRouteUserKeepsSharedTunnelAfterProtectionWithdrawal)
{
    ASSERT_TRUE(source->createDynamicDIPTunnel("10.0.0.2",TUNNEL_USER_IP)); install();
    auto oid=*tunnels.begin(); EXPECT_EQ(tunnels.size(),1u);
    builder->deleteProtectionRow("1073741825"); step(); step();
    EXPECT_TRUE(tunnels.count(oid)); EXPECT_EQ(source->getRemoteEndPointRefCnt("10.0.0.2"),1);
    EXPECT_EQ(source->tnl_users_.at("10.0.0.2").protection_refcnt,0u);
    EXPECT_TRUE(source->deleteDynamicDIPTunnel("10.0.0.2",TUNNEL_USER_IP));
}

TEST_F(L2ProtectionTest, CreationFailuresRollBackWithoutPhantomUsers)
{
    for (const string stage : {"tunnel-create","rmac-set","nh-create","group-create","member-create"})
    {
        fault=stage; member("1073741825"); group("2147483649","1073741825"); step();
        EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649"));
        fault.clear(); step(); step(); ASSERT_TRUE(builder->hasActiveL2Nhg("2147483649"));
        EXPECT_EQ(source->tnl_users_.at("10.0.0.2").protection_refcnt,1u);
        builder->deleteProtectionRow("2147483649"); builder->deleteProtectionRow("1073741825"); step(); step();
        EXPECT_TRUE(nhTunnels.empty()); EXPECT_TRUE(tunnels.empty()); EXPECT_TRUE(source->tnl_users_.empty());
    }
}

TEST_F(L2ProtectionTest, RemovalFailuresRetainResourcesForRetry)
{
    for (const string stage : {"member-remove","nh-remove","default-get","rmac-set","tunnel-remove","group-remove"})
    {
        install(); fault=stage; builder->deleteProtectionRow("2147483649"); builder->deleteProtectionRow("1073741825"); step(); step();
        EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649"));
        EXPECT_FALSE(builder->m_protectionGroups.empty() && builder->m_protectionMembers.empty() && builder->m_protectionPeers.empty());
        fault.clear(); step(); step(); EXPECT_TRUE(nhTunnels.empty()); EXPECT_TRUE(tunnels.empty()); EXPECT_TRUE(groups.empty());
        EXPECT_TRUE(source->tnl_users_.empty());
    }
}

TEST_F(L2ProtectionTest, TunnelPrerequisitesAndHardwareGroupLimitDeferAndRetry)
{
    member("1073741825"); group("2147483649","1073741825");
    nvo->source_vtep_ptr=nullptr; step(); EXPECT_TRUE(tunnels.empty());
    nvo->source_vtep_ptr=source; source->active_=false; step(); EXPECT_TRUE(tunnels.empty());
    source->active_=true; source->del_tnl_hw_pending=true; step(); EXPECT_TRUE(tunnels.empty());
    source->del_tnl_hw_pending=false; m_VxlanTunnelOrch->is_dip_tunnel_supported=false; step(); EXPECT_TRUE(tunnels.empty());
    builder->deleteProtectionRow("2147483649"); step(); group("2147483649","1073741825");
    m_VxlanTunnelOrch->is_dip_tunnel_supported=true;
    auto limit=gRouteOrch->m_maxNextHopGroupCount; gRouteOrch->m_maxNextHopGroupCount=0;
    step(); EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649")); gRouteOrch->m_maxNextHopGroupCount=limit;
    step(); EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649"));
}

TEST_F(L2ProtectionTest, InvalidRowsPreserveAcceptedObjectsAndOpaqueUint32Ids)
{
    install();
    for (const string id : {"","0","01","-1","x","4294967296","99999999999999999999"}) member(id);
    member("1073741825","bad"); member("1073741825","0.0.0.0");
    member("1073741825","10.0.0.2","broken"); member("1073741825","10.0.0.2","00:00:00:00:00:00");
    member("1073741825","10.0.0.2","01:00:00:00:00:01"); member("1073741825","10.0.0.2","","unknown");
    for (const string list : {"0","1073741825,1073741825","x",",1073741825","1073741825,","1073741825,,1073741826"}) group("2147483649",list);
    string tooMany;
    for (unsigned i=1;i<=4097;++i) { if (i>1) tooMany+=","; tooMany+=std::to_string(i); }
    group("2147483649",tooMany);
    group("1073741825","1073741825"); member("2147483649");
    builder->updateProtectionRow({"2147483649",SET_COMMAND,{{"unexpected","invalid"}}});
    builder->updateProtectionRow({"2147483649",SET_COMMAND,{{"nexthop_group","1073741825"},{"router_mac","02:00:00:00:00:01"}}});
    step(); EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649")); EXPECT_EQ(builder->m_protectionMembers.size(),1u);
    member("4294967295","10.0.0.3"); group("2147483650","4294967295"); step();
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483650"));
}

TEST_F(L2ProtectionTest, ConsumerDispatchAndLegacyRowsStaySeparate)
{
    auto consumer=dynamic_cast<Consumer*>(builder->getExecutor(APP_L2_NEXTHOP_GROUP_TABLE_NAME)); ASSERT_NE(consumer,nullptr);
    consumer->m_toSync.emplace("1073741825",KeyOpFieldsValuesTuple{"1073741825",SET_COMMAND,{{"type","l3"},{"remote_vtep","10.0.0.2"},{"router_mac","02:00:00:00:00:01"}}});
    builder->m_fpmL2Ids.set("2147483649", {{"NULL","NULL"}});
    consumer->m_toSync.emplace("2147483649",KeyOpFieldsValuesTuple{"2147483649",SET_COMMAND,{{"nexthop_group","1073741825"}}});
    builder->doL2NhgTask(*consumer); step(); EXPECT_TRUE(consumer->m_toSync.empty());
    builder->m_fpmL2Ids.del("2147483649");
    consumer->m_toSync.emplace("2147483649",KeyOpFieldsValuesTuple{"2147483649",SET_COMMAND,{{"nexthop_group",""}}});
    builder->doL2NhgTask(*consumer);
    EXPECT_EQ(builder->m_protectionGroups.at("2147483649").desired.size(),1u);
    builder->m_fpmL2Ids.set("2147483649", {{"NULL","NULL"}});
    consumer->m_toSync.emplace("1073741825",KeyOpFieldsValuesTuple{"1073741825",SET_COMMAND,{{"remote_vtep","10.0.0.3"}}});
    builder->doL2NhgTask(*consumer); EXPECT_EQ(builder->m_protectionMembers.at("1073741825").ip,"10.0.0.2");
    builder->m_nhg_vtep["123"]={"10.0.0.2",0,"source"}; member("123"); EXPECT_EQ(builder->m_protectionMembers.count("123"),0u);
    consumer->m_toSync.emplace("1073741825",KeyOpFieldsValuesTuple{"1073741825",DEL_COMMAND,{}});
    consumer->m_toSync.emplace("2147483649",KeyOpFieldsValuesTuple{"2147483649",DEL_COMMAND,{}});
    builder->doL2NhgTask(*consumer); step(); step(); EXPECT_TRUE(nhTunnels.empty());
    consumer->m_toSync.emplace("unknown",KeyOpFieldsValuesTuple{"unknown","UNKNOWN",{}});
    builder->doL2NhgTask(*consumer); EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_FALSE(builder->hasActiveL2Nhg("missing")); EXPECT_EQ(builder->getNumL2NhgNextHops("missing"),0u);
}

TEST_F(L2ProtectionTest, SourceChangesAndAddressFamiliesReconcileWithoutStaleReferences)
{
    install(); auto oldOid=builder->m_protectionGroups.at("2147483649").oid;
    source->active_=false; step(); EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649"));
    source->active_=true; step(); EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649"));
    auto replacement=new VxlanTunnel("replacement",IpAddress("10.1.0.1"),IpAddress("0.0.0.0"),TNL_CREATION_SRC_CLI);
    replacement->active_=true; m_VxlanTunnelOrch->addTunnel("replacement",replacement); nvo->source_vtep_ptr=replacement;
    step(); step(); EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649"));
    EXPECT_TRUE(source->tnl_users_.empty()); EXPECT_EQ(replacement->getRemoteEndPointRefCnt("10.0.0.2"),1);
    EXPECT_EQ(builder->m_protectionGroups.at("2147483649").oid,oldOid);
    member("1073741826","2001:db8::2"); group("2147483650","1073741826"); step();
    EXPECT_FALSE(builder->hasActiveL2Nhg("2147483650")); // IPv4 source cannot serve an IPv6 peer.
    auto v6=new VxlanTunnel("v6",IpAddress("2001:db8::1"),IpAddress("::"),TNL_CREATION_SRC_CLI);
    v6->active_=true; m_VxlanTunnelOrch->addTunnel("v6",v6); nvo->source_vtep_ptr=v6;
    step(); step(); EXPECT_TRUE(builder->hasActiveL2Nhg("2147483650"));
    EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649"));
}

TEST_F(L2ProtectionTest, RegisteredTaskRetriesWithoutNewEventsAndWaitsForPorts)
{
    auto consumer=dynamic_cast<Consumer*>(builder->getExecutor(APP_L2_NEXTHOP_GROUP_TABLE_NAME));
    ASSERT_NE(consumer,nullptr);
    consumer->m_toSync.emplace("1073741825",KeyOpFieldsValuesTuple{"1073741825",SET_COMMAND,{{"type","l3"},{"remote_vtep","10.0.0.2"},{"router_mac","02:00:00:00:00:01"}}});
    builder->m_fpmL2Ids.set("2147483649", {{"NULL","NULL"}});
    consumer->m_toSync.emplace("2147483649",KeyOpFieldsValuesTuple{"2147483649",SET_COMMAND,{{"nexthop_group","1073741825"}}});
    const auto ready=gPortsOrch->m_initDone; auto pending=gPortsOrch->m_pendingPortSet;
    gPortsOrch->m_initDone=false; builder->doTask(); EXPECT_EQ(consumer->m_toSync.size(),2u); EXPECT_TRUE(tunnels.empty());
    gPortsOrch->m_initDone=true; gPortsOrch->m_pendingPortSet.clear(); fault="member-create";
    builder->doTask(); EXPECT_TRUE(consumer->m_toSync.empty()); EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649"));
    fault.clear(); builder->doTask(); EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649"));
    gPortsOrch->m_initDone=ready; gPortsOrch->m_pendingPortSet=std::move(pending);
}

TEST_F(L2ProtectionTest, HldGroupBeforeMemberUsesAcceptedIdWithoutExtraRowFields)
{
    auto consumer=dynamic_cast<Consumer*>(builder->getExecutor(APP_L2_NEXTHOP_GROUP_TABLE_NAME));
    ASSERT_NE(consumer,nullptr);
    builder->m_fpmL2Ids.set("2147483649", {{"NULL","NULL"}});
    consumer->m_toSync.emplace("2147483649",KeyOpFieldsValuesTuple{"2147483649",SET_COMMAND,{{"nexthop_group","1073741825"}}});
    builder->doL2NhgTask(*consumer); step();
    EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_EQ(builder->m_protectionGroups.count("2147483649"),1u);
    EXPECT_EQ(builder->m_nhg_nh.count("2147483649"),0u);
    EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649"));
    builder->m_fpmL2Ids.set("1073741825", {{"NULL","NULL"}});
    consumer->m_toSync.emplace("1073741825",KeyOpFieldsValuesTuple{"1073741825",SET_COMMAND,{{"type","l3"},{"remote_vtep","10.0.0.2"},{"router_mac","02:00:00:00:00:01"}}});
    builder->doL2NhgTask(*consumer); step();
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649"));
}

TEST_F(L2ProtectionTest, LegacyFactoryAndUntypedMemberKeepExistingBehavior)
{
    ASSERT_TRUE(source->createDynamicDIPTunnel("10.0.0.2",TUNNEL_USER_IP));
    const auto tid=*tunnels.begin(); auto gid=builder->createSaiNextHopGroup();
    ASSERT_NE(gid,SAI_NULL_OBJECT_ID);
    auto ids=builder->createSaiNextHop(gid,tid,"10.0.0.2"); ASSERT_NE(ids.first,SAI_NULL_OBJECT_ID);
    EXPECT_TRUE(builder->removeSaiNextHop({ids.first,ids.second})); EXPECT_TRUE(builder->removeSaiNextHopGroup(gid));
    fault="group-create"; faultStatus=SAI_STATUS_INSUFFICIENT_RESOURCES;
    EXPECT_EQ(builder->createSaiNextHopGroup(),SAI_NULL_OBJECT_ID); fault.clear();
    gid=builder->createSaiNextHopGroup(); fault="group-remove"; faultStatus=SAI_STATUS_OBJECT_IN_USE;
    EXPECT_FALSE(builder->removeSaiNextHopGroup(gid)); fault.clear(); EXPECT_TRUE(builder->removeSaiNextHopGroup(gid));
    fault="nh-create"; faultStatus=SAI_STATUS_INSUFFICIENT_RESOURCES;
    EXPECT_EQ(builder->createSaiBridgeNextHop(tid,"10.0.0.2"),SAI_NULL_OBJECT_ID);
    EXPECT_EQ(builder->createSaiNextHop(gid,tid,"10.0.0.2").first,SAI_NULL_OBJECT_ID); fault.clear(); faultStatus=SAI_STATUS_FAILURE;
    auto name=m_VxlanTunnelOrch->getTunnelPortName("10.0.0.2"); EXPECT_TRUE(gPortsOrch->addTunnel(name,tid,false));
    auto consumer=dynamic_cast<Consumer*>(builder->getExecutor(APP_L2_NEXTHOP_GROUP_TABLE_NAME));
    consumer->m_toSync.emplace("123",KeyOpFieldsValuesTuple{"123",SET_COMMAND,{{"remote_vtep","10.0.0.2"}}});
    builder->doL2NhgTask(*consumer); EXPECT_EQ(builder->m_nhg_vtep.count("123"),1u); EXPECT_EQ(builder->m_protectionMembers.count("123"),0u);
    consumer->m_toSync.emplace("123",KeyOpFieldsValuesTuple{"123",DEL_COMMAND,{}}); builder->doL2NhgTask(*consumer);
    EXPECT_EQ(builder->m_nhg_vtep.count("123"),0u);
    Port port; ASSERT_TRUE(gPortsOrch->getPort(name,port)); gPortsOrch->removeTunnel(port);
    EXPECT_TRUE(source->deleteDynamicDIPTunnel("10.0.0.2",TUNNEL_USER_IP));
}

TEST_F(L2ProtectionTest, ImetAndFdbUsersKeepTunnelAfterProtectionWithdrawal)
{
    ASSERT_TRUE(source->createDynamicDIPTunnel("10.0.0.2",TUNNEL_USER_IMR));
    auto tid=*tunnels.begin(); auto name=m_VxlanTunnelOrch->getTunnelPortName("10.0.0.2");
    ASSERT_TRUE(gPortsOrch->addTunnel(name,tid,false));
    Port port; ASSERT_TRUE(gPortsOrch->getPort(name,port)); port.m_fdb_count=1; gPortsOrch->setPort(name,port);
    ASSERT_TRUE(m_VxlanTunnelOrch->addTunnelUser("10.0.0.2",0,0,TUNNEL_USER_IMR));
    ASSERT_TRUE(source->deleteDynamicDIPTunnel("10.0.0.2",TUNNEL_USER_IMR));
    install(); EXPECT_EQ(source->getRemoteEndPointRefCnt("10.0.0.2"),2);
    builder->deleteProtectionRow("1073741825"); step(); step();
    EXPECT_TRUE(tunnels.count(tid)); EXPECT_EQ(source->getRemoteEndPointIMRRefCnt("10.0.0.2"),1);
    EXPECT_EQ(source->tnl_users_.at("10.0.0.2").protection_refcnt,0u);
    EXPECT_TRUE(source->deleteDynamicDIPTunnel("10.0.0.2",TUNNEL_USER_IMR)); source->cleanupDynamicDIPTunnel("10.0.0.2");
    EXPECT_TRUE(tunnels.count(tid)); EXPECT_EQ(source->getRemoteEndPointRefCnt("10.0.0.2"),0);
    // Simulate the existing FDB/port owner completing its own withdrawal.
    gPortsOrch->removeTunnel(port); EXPECT_TRUE(source->deleteDynamicDIPTunnel("10.0.0.2",TUNNEL_USER_IMR,false));
}

TEST_F(L2ProtectionTest, FailedCreateStatusesNeverPublishTunnelReferences)
{
    for (auto status : {SAI_STATUS_ITEM_ALREADY_EXISTS,SAI_STATUS_ITEM_NOT_FOUND,SAI_STATUS_OBJECT_IN_USE,SAI_STATUS_INSUFFICIENT_RESOURCES})
    {
        fault="tunnel-create"; faultStatus=static_cast<sai_status_t>(status);
        EXPECT_FALSE(source->createDynamicDIPTunnel("10.0.0.2",TUNNEL_USER_PROTECTION));
        EXPECT_TRUE(source->tnl_users_.empty()); EXPECT_TRUE(tunnels.empty());
    }
    fault.clear();
}

TEST_F(L2ProtectionTest, PartialGroupUsesReadyPeersAndRetriesMissingMembers)
{
    member("1073741825"); member("1073741826","10.0.0.3","");
    group("2147483649","1073741825,1073741826,1073741827"); step();
    auto oid=builder->m_protectionGroups.at("2147483649").oid;
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649")); EXPECT_EQ(builder->getNumL2NhgNextHops("2147483649"),1u);
    member("1073741826","10.0.0.3"); member("1073741827","10.0.0.4"); fault="member-create"; step();
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649")); EXPECT_EQ(builder->getNumL2NhgNextHops("2147483649"),1u);
    fault.clear(); step();
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649")); EXPECT_EQ(builder->getNumL2NhgNextHops("2147483649"),3u);
    EXPECT_EQ(builder->m_protectionGroups.at("2147483649").oid,oid);
}

TEST_F(L2ProtectionTest, SourceReplacementWaitsForOtherUsersBeforeReusingDipName)
{
    ASSERT_TRUE(source->createDynamicDIPTunnel("10.0.0.2",TUNNEL_USER_IP)); install();
    auto oldTunnel=*tunnels.begin(); auto replacement=new VxlanTunnel("replacement",IpAddress("10.1.0.1"),IpAddress("0.0.0.0"),TNL_CREATION_SRC_CLI);
    replacement->active_=true; m_VxlanTunnelOrch->addTunnel("replacement",replacement); nvo->source_vtep_ptr=replacement;
    step(); step();
    EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649")); EXPECT_TRUE(tunnels.count(oldTunnel));
    EXPECT_TRUE(replacement->tnl_users_.empty()); EXPECT_EQ(source->getRemoteEndPointIPRefCnt("10.0.0.2"),1);
    ASSERT_TRUE(source->deleteDynamicDIPTunnel("10.0.0.2",TUNNEL_USER_IP)); source->cleanupDynamicDIPTunnel("10.0.0.2");
    step(); EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649")); EXPECT_FALSE(tunnels.count(oldTunnel));
    EXPECT_EQ(replacement->getRemoteEndPointRefCnt("10.0.0.2"),1);
}

TEST_F(L2ProtectionTest, SamePeerContextsInOneGroupShareMembershipAndWithdrawIndependently)
{
    install(); member("1073741826"); group("2147483649","1073741825,1073741826"); step();
    EXPECT_EQ(gmHops.size(),1u); EXPECT_EQ(builder->getNumL2NhgNextHops("2147483649"),1u);
    EXPECT_EQ(builder->m_protectionGroups.at("2147483649").members.size(),2u);
    builder->deleteProtectionRow("1073741825"); step();
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649")); EXPECT_EQ(gmHops.size(),1u); EXPECT_EQ(nhTunnels.size(),1u);
    group("2147483649","1073741826"); step();
    EXPECT_EQ(gmHops.size(),1u);
    builder->deleteProtectionRow("1073741826"); fault="member-remove"; step();
    EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649")); EXPECT_EQ(gmHops.size(),1u);
    fault.clear(); step(); step(); EXPECT_TRUE(gmHops.empty()); EXPECT_TRUE(tunnels.empty());
    member("1073741825"); member("1073741826"); group("2147483649","1073741825,1073741826"); step();
    builder->deleteProtectionRow("2147483649"); step(); EXPECT_TRUE(gmHops.empty()); EXPECT_EQ(nhTunnels.size(),1u);
}

TEST_F(L2ProtectionTest, TunnelUserCreationAndPendingSourceDeletionPropagateFailures)
{
    fault="tunnel-create";
    EXPECT_FALSE(m_VxlanTunnelOrch->addTunnelUser("10.0.0.2",0,0,TUNNEL_USER_IMR));
    EXPECT_TRUE(source->tnl_users_.empty()); fault.clear();
    tunnel_refcnt_t refs{}; source->updateRemoteEndPointRefCnt(false,refs,TUNNEL_USER_PROTECTION);
    EXPECT_EQ(refs.protection_refcnt,0u);
    const auto oid=++sequence; tunnels.insert(oid); source->ids_.tunnel_id=oid;
    source->del_tnl_hw_pending=true; fault="tunnel-remove";
    source->deletePendingSIPTunnel(); EXPECT_TRUE(source->del_tnl_hw_pending);
    EXPECT_EQ(source->getTunnelId(),oid); EXPECT_TRUE(source->isActive());
    fault.clear(); source->deletePendingSIPTunnel(); EXPECT_FALSE(source->del_tnl_hw_pending);
    EXPECT_EQ(source->getTunnelId(),SAI_NULL_OBJECT_ID); EXPECT_TRUE(tunnels.empty());
    source->active_=true;
    // A mapper exception occurs after successful tunnel removal; retry resumes
    // mapper cleanup without attempting removal of a stale tunnel OID.
    tunnels.insert(oid); source->ids_.tunnel_id=oid; source->ids_.tunnel_encap_id[TUNNEL_MAP_T_VLAN]=oid+1;
    sai_tunnel_api->remove_tunnel_map=[](sai_object_id_t)->sai_status_t { throw std::runtime_error("mapper failure"); };
    uint8_t mapper=0; TUNNELMAP_SET_VLAN(mapper);
    EXPECT_FALSE(source->deleteTunnelHw(mapper,TUNNEL_MAP_USE_DEDICATED_ENCAP_DECAP,false));
    EXPECT_EQ(source->getTunnelId(),SAI_NULL_OBJECT_ID); EXPECT_TRUE(source->isActive());
    sai_tunnel_api->remove_tunnel_map=[](sai_object_id_t)->sai_status_t { return SAI_STATUS_SUCCESS; };
    EXPECT_TRUE(source->deleteTunnelHw(mapper,TUNNEL_MAP_USE_DEDICATED_ENCAP_DECAP,false));
    sai_tunnel_api->remove_tunnel_map=savedTunnel.remove_tunnel_map;
    source->ids_.tunnel_encap_id[TUNNEL_MAP_T_VLAN]=SAI_NULL_OBJECT_ID;
}

TEST_F(L2ProtectionTest, AlreadyRemovedTunnelCompletesCleanupAndAmbiguousRowsStayInert)
{
    install(); builder->deleteProtectionRow("1073741825"); step();
    fault="tunnel-remove"; faultStatus=SAI_STATUS_ITEM_NOT_FOUND; step();
    EXPECT_TRUE(tunnels.empty()); EXPECT_TRUE(source->tnl_users_.empty()); fault.clear(); faultStatus=SAI_STATUS_FAILURE;
    auto consumer=dynamic_cast<Consumer*>(builder->getExecutor(APP_L2_NEXTHOP_GROUP_TABLE_NAME));
    builder->m_fpmL2Ids.set("123", {{"NULL","NULL"}});
    consumer->m_toSync.emplace("123",KeyOpFieldsValuesTuple{"123",SET_COMMAND,{{"remote_vtep","10.0.0.2"}}});
    consumer->m_toSync.emplace("124",KeyOpFieldsValuesTuple{"124",SET_COMMAND,{{"type","l3"},{"nexthop_group",""}}});
    builder->m_fpmL2Ids.set("2147483649", {{"NULL","NULL"}});
    consumer->m_toSync.emplace("2147483649",KeyOpFieldsValuesTuple{"2147483649",SET_COMMAND,{{"nexthop_group",""},{"remote_vtep","10.0.0.2"}}});
    builder->doL2NhgTask(*consumer); EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_FALSE(builder->m_protectionGroups.count("123")); EXPECT_FALSE(builder->m_protectionGroups.count("124"));
    EXPECT_EQ(builder->m_protectionGroups.at("2147483649").desired.size(),1u);
    // A malformed private record must not authorize a legacy Group update to
    // an existing protection object, even when its numeric key is unchanged.
    builder->m_fpmL2Ids.set("2147483649", {{"NULL","invalid"}});
    consumer->m_toSync.emplace("2147483649",KeyOpFieldsValuesTuple{"2147483649",SET_COMMAND,{{"nexthop_group",""}}});
    builder->doL2NhgTask(*consumer); EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_EQ(builder->m_protectionGroups.at("2147483649").desired.size(),1u);
}

// Consumers can call lookup/reference APIs from callbacks. They retain
// a failed-clear handle and retry independently; notifications are edge based.
class ProtectionListener : public Observer
{
public:
    explicit ProtectionListener(L2NhgOrch &orch) : orch(orch) { orch.attach(this); }
    ~ProtectionListener() override { orch.detach(this); }
    vector<L2NhgUpdate> events;
    std::function<void(const L2NhgUpdate &)> action;
    void update(SubjectType type, void *context) override
    {
        ASSERT_EQ(type,SUBJECT_TYPE_L2_NHG_CHANGE);
        const auto &event=*static_cast<L2NhgUpdate*>(context);
        events.push_back(event);
        if (action) action(event);
    }
private:
    L2NhgOrch &orch;
};

class L2ProtectionRefTest : public L2ProtectionTest {};

TEST_F(L2ProtectionRefTest, LookupRequiresReadyOwnedGroupAndClearsMissOutput)
{
    sai_object_id_t oid=123;
    EXPECT_FALSE(builder->getNhgOid(0,oid)); EXPECT_EQ(oid,SAI_NULL_OBJECT_ID);
    EXPECT_FALSE(builder->refNhg(UINT32_MAX)); builder->unrefNhg(UINT32_MAX);
    group("2147483649","1073741825"); step();
    EXPECT_FALSE(builder->getNhgOid(2147483649u,oid));
    member("1073741825","10.0.0.2",""); step(); EXPECT_FALSE(builder->getNhgOid(2147483649u,oid));
    member("1073741825"); step(); ASSERT_TRUE(builder->getNhgOid(2147483649u,oid));
    EXPECT_EQ(oid,builder->m_protectionGroups.at("2147483649").oid);
    group(std::to_string(UINT32_MAX),"1073741825"); step();
    EXPECT_TRUE(builder->getNhgOid(UINT32_MAX,oid)); EXPECT_TRUE(builder->refNhg(UINT32_MAX)); builder->unrefNhg(UINT32_MAX);
    builder->m_nhg_nh["123"].oid=oid; builder->m_nhg_nh["123"].is_active=true;
    EXPECT_FALSE(builder->getNhgOid(123,oid)); EXPECT_EQ(oid,SAI_NULL_OBJECT_ID);
    builder->deleteProtectionRow("2147483649"); EXPECT_FALSE(builder->getNhgOid(2147483649u,oid));
    EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649"));
}

TEST_F(L2ProtectionRefTest, CountedPinsHoldGroupAndLastPathUntilEveryUserReleases)
{
    install(); const auto oid=builder->m_protectionGroups.at("2147483649").oid;
    ASSERT_TRUE(builder->refNhg(2147483649u)); ASSERT_TRUE(builder->refNhg(2147483649u));
    builder->deleteProtectionRow("2147483649"); builder->deleteProtectionRow("1073741825"); step(); step();
    EXPECT_TRUE(groups.count(oid)); EXPECT_EQ(gmHops.size(),1u); EXPECT_EQ(nhTunnels.size(),1u);
    EXPECT_EQ(source->getRemoteEndPointRefCnt("10.0.0.2"),1); EXPECT_FALSE(builder->refNhg(2147483649u));
    builder->unrefNhg(2147483649u); step(); EXPECT_TRUE(groups.count(oid));
    builder->unrefNhg(0u); builder->unrefNhg(2147483649u); step(); step();
    EXPECT_TRUE(groups.empty()); EXPECT_TRUE(tunnels.empty());
    builder->unrefNhg(2147483649u); EXPECT_EQ(builder->getL2NhgCount(),0u);
}

TEST_F(L2ProtectionRefTest, ReservedAndCommittedHandlesPinIndependently)
{
    install(); L2NhgReference first,second;
    ASSERT_TRUE(builder->reserveNhg(2147483649u,first)); ASSERT_TRUE(builder->reserveNhg(2147483649u,second));
    EXPECT_NE(first.token,second.token); EXPECT_EQ(first.oid,second.oid);
    auto &refs=builder->m_protectionGroups.at("2147483649").references;
    EXPECT_FALSE(refs.at(first.token)); EXPECT_FALSE(refs.at(second.token));
    EXPECT_TRUE(builder->commitNhgRef(first)); EXPECT_TRUE(builder->commitNhgRef(first)); EXPECT_TRUE(refs.at(first.token));
    builder->unrefNhg(2147483649u); EXPECT_EQ(refs.size(),2u);
    builder->deleteProtectionRow("2147483649"); step(); EXPECT_FALSE(builder->commitNhgRef(second));
    EXPECT_EQ(refs.size(),2u); builder->unrefNhg(first); builder->unrefNhg(first); step();
    EXPECT_EQ(refs.size(),1u); EXPECT_TRUE(groups.count(first.oid));
    builder->unrefNhg(second); step(); EXPECT_TRUE(groups.empty());
}

TEST_F(L2ProtectionRefTest, LiveHandleAndCounterOverflowCannotLeakOrWrapPins)
{
    L2NhgReference missing; EXPECT_FALSE(builder->reserveNhg(0,missing)); EXPECT_EQ(missing.token,0u);
    install(); L2NhgReference held; ASSERT_TRUE(builder->reserveNhg(2147483649u,held));
    const auto token=held.token; EXPECT_FALSE(builder->reserveNhg(2147483649u,held)); EXPECT_EQ(held.token,token);
    auto &group=builder->m_protectionGroups.at("2147483649");
    group.legacyReferences=UINT32_MAX; EXPECT_FALSE(builder->refNhg(2147483649u)); EXPECT_EQ(group.legacyReferences,UINT32_MAX);
    group.legacyReferences=0; builder->m_nextProtectionReference=std::numeric_limits<uint64_t>::max();
    L2NhgReference overflow; EXPECT_FALSE(builder->reserveNhg(2147483649u,overflow)); EXPECT_EQ(overflow.token,0u);
    EXPECT_EQ(group.references.size(),1u); builder->unrefNhg(held);
}

TEST_F(L2ProtectionRefTest, InvalidIdentityOrTokenCannotReleaseAnotherUsersPin)
{
    install(); L2NhgReference held; ASSERT_TRUE(builder->reserveNhg(2147483649u,held));
    EXPECT_FALSE(builder->commitNhgRef({})); builder->unrefNhg(L2NhgReference{});
    for (auto invalid : {L2NhgReference{2147483650u,held.oid,held.token},L2NhgReference{held.nhg_id,held.oid+1,held.token},L2NhgReference{held.nhg_id,held.oid,held.token+1}})
    {
        EXPECT_FALSE(builder->commitNhgRef(invalid)); builder->unrefNhg(invalid);
    }
    EXPECT_EQ(builder->m_protectionGroups.at("2147483649").references.size(),1u); builder->unrefNhg(held);
}

TEST_F(L2ProtectionRefTest, RecycledIdAndSaiOidRejectStaleReferenceTokens)
{
    install(); L2NhgReference old; ASSERT_TRUE(builder->reserveNhg(2147483649u,old)); ASSERT_TRUE(builder->commitNhgRef(old));
    builder->unrefNhg(old); builder->deleteProtectionRow("2147483649"); step(); ASSERT_TRUE(groups.empty());
    // Simulate a hardware OID allocator reusing the same group OID.
    sequence=old.oid-1; group("2147483649","1073741825"); step();
    L2NhgReference current; ASSERT_TRUE(builder->reserveNhg(2147483649u,current));
    EXPECT_EQ(current.oid,old.oid); EXPECT_NE(current.token,old.token);
    EXPECT_FALSE(builder->commitNhgRef(old)); builder->unrefNhg(old);
    EXPECT_EQ(builder->m_protectionGroups.at("2147483649").references.size(),1u);
    builder->unrefNhg(current);
}

TEST_F(L2ProtectionRefTest, ReadyWithdrawalAndPhysicalRemovalAreDistinctOrderedEdges)
{
    ProtectionListener listener(*builder);
    listener.action=[&](const L2NhgUpdate &event) {
        sai_object_id_t oid=123;
        if (event.change==L2NhgChange::READY)
        {
            EXPECT_TRUE(builder->getNhgOid(event.nhg_id,oid)); EXPECT_EQ(oid,event.oid); EXPECT_TRUE(groups.count(event.oid));
        }
        else
        {
            EXPECT_FALSE(builder->getNhgOid(event.nhg_id,oid)); EXPECT_EQ(oid,SAI_NULL_OBJECT_ID);
            if (event.change==L2NhgChange::WITHDRAWING)
            {
                EXPECT_TRUE(groups.count(event.oid)); EXPECT_EQ(gmHops.size(),1u); EXPECT_EQ(calls["member-remove"],0u);
                EXPECT_FALSE(builder->refNhg(event.nhg_id));
            }
            else EXPECT_FALSE(groups.count(event.oid));
        }
    };
    install(); step(); ASSERT_EQ(listener.events.size(),1u); const auto oid=listener.events.front().oid;
    fault="group-remove"; builder->deleteProtectionRow("2147483649"); step();
    ASSERT_EQ(listener.events.size(),2u); EXPECT_EQ(listener.events.back().change,L2NhgChange::WITHDRAWING);
    builder->deleteProtectionRow("2147483649"); step(); EXPECT_EQ(listener.events.size(),2u);
    EXPECT_TRUE(groups.count(oid)); fault.clear(); step();
    ASSERT_EQ(listener.events.size(),3u); EXPECT_EQ(listener.events.back().change,L2NhgChange::REMOVED);
    EXPECT_EQ(listener.events.back().oid,oid); step(); EXPECT_EQ(listener.events.size(),3u);
}

TEST_F(L2ProtectionRefTest, FailedClearRetainsPinsUntilBothObserversConfirmDetach)
{
    ProtectionListener first(*builder),second(*builder); install();
    L2NhgReference a,b; ASSERT_TRUE(builder->reserveNhg(2147483649u,a)); ASSERT_TRUE(builder->reserveNhg(2147483649u,b));
    ASSERT_TRUE(builder->commitNhgRef(a)); ASSERT_TRUE(builder->commitNhgRef(b));
    first.action=[&](const L2NhgUpdate &event) { if (event.change==L2NhgChange::WITHDRAWING) builder->unrefNhg(a); };
    bool clearFailed=true;
    second.action=[&](const L2NhgUpdate &event) { if (event.change==L2NhgChange::WITHDRAWING && !clearFailed) builder->unrefNhg(b); };
    builder->deleteProtectionRow("2147483649"); builder->deleteProtectionRow("1073741825"); step(); step();
    EXPECT_TRUE(groups.count(a.oid)); EXPECT_EQ(gmHops.size(),1u); EXPECT_EQ(nhTunnels.size(),1u);
    EXPECT_EQ(calls["member-remove"],0u); EXPECT_EQ(first.events.size(),2u); EXPECT_EQ(second.events.size(),2u);
    clearFailed=false; builder->unrefNhg(b); step(); step();
    EXPECT_TRUE(groups.empty()); EXPECT_TRUE(tunnels.empty());
    ASSERT_EQ(second.events.size(),3u); EXPECT_EQ(second.events.back().change,L2NhgChange::REMOVED);
}

TEST_F(L2ProtectionRefTest, CallbackReservationCommitAndSynchronousDetachAreSafe)
{
    ProtectionListener listener(*builder); L2NhgReference held;
    listener.action=[&](const L2NhgUpdate &event) {
        if (event.change==L2NhgChange::READY)
        {
            EXPECT_TRUE(builder->reserveNhg(event.nhg_id,held)); EXPECT_TRUE(builder->commitNhgRef(held));
        }
        if (event.change==L2NhgChange::WITHDRAWING)
        {
            EXPECT_TRUE(groups.count(event.oid)); EXPECT_EQ(gmHops.size(),1u);
            builder->unrefNhg(held);
        }
    };
    install(); builder->deleteProtectionRow("2147483649"); step();
    EXPECT_TRUE(groups.empty()); ASSERT_EQ(listener.events.size(),3u);
}

TEST_F(L2ProtectionRefTest, LastMemberReplacementWaitsForDetachThenReannouncesStableOid)
{
    ProtectionListener listener(*builder); install(); L2NhgReference held;
    ASSERT_TRUE(builder->reserveNhg(2147483649u,held)); ASSERT_TRUE(builder->commitNhgRef(held));
    auto oldHop=nhTunnels.begin()->first; member("1073741825","10.0.0.3"); step();
    EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649")); EXPECT_TRUE(nhTunnels.count(oldHop));
    EXPECT_EQ(source->getRemoteEndPointRefCnt("10.0.0.2"),1); EXPECT_EQ(tunnels.size(),1u);
    EXPECT_FALSE(builder->commitNhgRef(held)); EXPECT_EQ(listener.events.size(),2u);
    builder->unrefNhg(held); step(); step();
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649")); EXPECT_EQ(builder->m_protectionGroups.at("2147483649").oid,held.oid);
    EXPECT_FALSE(nhTunnels.count(oldHop)); EXPECT_EQ(tunnels.size(),1u);
    ASSERT_EQ(listener.events.size(),3u); EXPECT_EQ(listener.events.back().change,L2NhgChange::READY);
}

TEST_F(L2ProtectionRefTest, PartialChurnAndFailedAdditionKeepReadyPinnedGroup)
{
    ProtectionListener listener(*builder); member("1073741825"); member("1073741826","10.0.0.3");
    group("2147483649","1073741825,1073741826"); step(); L2NhgReference held;
    ASSERT_TRUE(builder->reserveNhg(2147483649u,held));
    builder->deleteProtectionRow("1073741825"); step(); EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649"));
    EXPECT_EQ(listener.events.size(),1u); EXPECT_EQ(builder->getNumL2NhgNextHops("2147483649"),1u);
    member("1073741827","10.0.0.4"); group("2147483649","1073741826,1073741827"); fault="member-create"; step();
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649")); EXPECT_EQ(listener.events.size(),1u);
    fault.clear(); step(); EXPECT_EQ(builder->getNumL2NhgNextHops("2147483649"),2u);
    EXPECT_EQ(builder->m_protectionGroups.at("2147483649").oid,held.oid); builder->unrefNhg(held);
}

TEST_F(L2ProtectionRefTest, FailedPartialRemovalKeepsReadyAndRetainsFailedMember)
{
    ProtectionListener listener(*builder); member("1073741825"); member("1073741826","10.0.0.3");
    group("2147483649","1073741825,1073741826"); step(); L2NhgReference held;
    ASSERT_TRUE(builder->reserveNhg(2147483649u,held)); group("2147483649","1073741826"); fault="member-remove"; step();
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649")); EXPECT_EQ(gmHops.size(),2u);
    EXPECT_EQ(listener.events.size(),1u); EXPECT_TRUE(builder->commitNhgRef(held));
    fault.clear(); step(); EXPECT_EQ(gmHops.size(),1u); EXPECT_EQ(listener.events.size(),1u);
    EXPECT_EQ(builder->m_protectionGroups.at("2147483649").oid,held.oid);
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649")); builder->unrefNhg(held);
}

TEST_F(L2ProtectionRefTest, FailedFirstAndThirdRemovalStillCleansLaterMembersOncePerPass)
{
    ProtectionListener listener(*builder);
    member("1073741825"); member("1073741826","10.0.0.3"); member("1073741827","10.0.0.4"); member("1073741828","10.0.0.5");
    group("2147483649","1073741825,1073741826,1073741827,1073741828"); step();
    auto &g = builder->m_protectionGroups.at("2147483649"); const auto oid = g.oid;
    const auto first = g.members.at("1073741825"), second = g.members.at("1073741826"), third = g.members.at("1073741827");
    L2NhgReference held; ASSERT_TRUE(builder->reserveNhg(g.id,held)); ASSERT_TRUE(builder->commitNhgRef(held));
    failedGroupMembers = {first,third}; memberRemovalAttempts.clear(); group("2147483649","1073741828"); step();
    EXPECT_FALSE(gmHops.count(second)); EXPECT_TRUE(gmHops.count(first)); EXPECT_TRUE(gmHops.count(third));
    EXPECT_EQ(g.members.size(),3u); EXPECT_TRUE(g.ready); EXPECT_EQ(g.oid,oid); EXPECT_EQ(listener.events.size(),1u);
    for (auto member : {first,second,third}) EXPECT_EQ(std::count(memberRemovalAttempts.begin(),memberRemovalAttempts.end(),member),1);
    memberRemovalAttempts.clear(); step(); EXPECT_EQ(memberRemovalAttempts.size(),2u);
    failedGroupMembers.clear(); step(); EXPECT_EQ(g.members.size(),1u); EXPECT_TRUE(g.ready); EXPECT_EQ(listener.events.size(),1u);
    EXPECT_TRUE(builder->commitNhgRef(held)); builder->unrefNhg(held);
}
TEST_F(L2ProtectionRefTest, FullDeletionRetainsOnlyFailedMemberRegardlessOfItsPosition)
{
    for (unsigned index = 0; index < 3; ++index)
    {
        member("1073741825"); member("1073741826","10.0.0.3"); member("1073741827","10.0.0.4");
        group("2147483649","1073741825,1073741826,1073741827"); step();
        const auto g = builder->m_protectionGroups.at("2147483649"); vector<sai_object_id_t> ids;
        for (const auto &m : g.members) ids.push_back(m.second);
        const auto keep = ids.at(index), hop = gmHops.at(keep), tunnel = nhTunnels.at(hop);
        const auto groupRemoves = calls["group-remove"]; failedGroupMembers.insert(keep); memberRemovalAttempts.clear();
        builder->deleteProtectionRow("2147483649");
        for (const string key : {"1073741825","1073741826","1073741827"}) builder->deleteProtectionRow(key);
        step(); step(); EXPECT_EQ(gmHops.size(),1u); EXPECT_EQ(nhTunnels.size(),1u); EXPECT_EQ(tunnels.size(),1u);
        EXPECT_TRUE(gmHops.count(keep)); EXPECT_TRUE(nhTunnels.count(hop)); EXPECT_TRUE(tunnels.count(tunnel));
        EXPECT_TRUE(groups.count(g.oid)); EXPECT_EQ(calls["group-remove"],groupRemoves);
        sai_object_id_t oid = 1; EXPECT_FALSE(builder->getNhgOid(g.id,oid)); EXPECT_EQ(oid,SAI_NULL_OBJECT_ID);
        failedGroupMembers.clear(); step(); step(); EXPECT_TRUE(groups.empty()); EXPECT_TRUE(gmHops.empty());
        EXPECT_TRUE(nhTunnels.empty()); EXPECT_TRUE(tunnels.empty());
    }
}
TEST_F(L2ProtectionRefTest, AllFailedRemovalsKeepTheGroupAndRetryEveryMemberWithoutSpinning)
{
    member("1073741825"); member("1073741826","10.0.0.3"); member("1073741827","10.0.0.4");
    group("2147483649","1073741825,1073741826,1073741827"); step();
    const auto oid = builder->m_protectionGroups.at("2147483649").oid;
    for (const auto &m : gmHops) failedGroupMembers.insert(m.first);
    builder->deleteProtectionRow("2147483649"); memberRemovalAttempts.clear(); step();
    EXPECT_EQ(memberRemovalAttempts.size(),3u); EXPECT_EQ(gmHops.size(),3u); EXPECT_TRUE(groups.count(oid));
    const auto ready=gPortsOrch->m_initDone; auto pending=gPortsOrch->m_pendingPortSet;
    gPortsOrch->m_initDone=true; gPortsOrch->m_pendingPortSet.clear();
    memberRemovalAttempts.clear(); builder->doTask(); EXPECT_EQ(memberRemovalAttempts.size(),3u);
    EXPECT_EQ(calls["group-remove"],0u); failedGroupMembers.clear(); builder->doTask(); EXPECT_FALSE(groups.count(oid));
    gPortsOrch->m_initDone=ready; gPortsOrch->m_pendingPortSet=std::move(pending);
}
TEST_F(L2ProtectionRefTest, FailedWithdrawalDoesNotStarveReadyAdditionsToTheHealthyGroup)
{
    ProtectionListener listener(*builder); member("1073741825"); member("1073741826","10.0.0.3");
    group("2147483649","1073741825,1073741826"); step(); auto &g = builder->m_protectionGroups.at("2147483649");
    const auto keep = g.members.at("1073741825"), oid = g.oid;
    failedGroupMembers.insert(keep); member("1073741827","10.0.0.4");
    group("2147483649","1073741826,1073741827"); memberRemovalAttempts.clear(); step();
    EXPECT_EQ(g.members.size(),3u); EXPECT_TRUE(g.members.count("1073741827")); EXPECT_TRUE(g.ready);
    EXPECT_EQ(g.oid,oid); EXPECT_EQ(listener.events.size(),1u); EXPECT_EQ(memberRemovalAttempts.size(),1u);
    failedGroupMembers.clear(); step(); EXPECT_EQ(g.members.size(),2u); EXPECT_TRUE(g.ready);
}
TEST_F(L2ProtectionRefTest, ObsoleteRetainedMemberCannotMakeAnOtherwiseUnavailableGroupReady)
{
    ProtectionListener listener(*builder); install(); auto &g = builder->m_protectionGroups.at("2147483649");
    const auto failed = g.members.at("1073741825"), oid = g.oid; failedGroupMembers.insert(failed);
    group("2147483649","1073741826"); step();
    EXPECT_TRUE(groups.count(oid)); EXPECT_TRUE(gmHops.count(failed)); EXPECT_FALSE(g.ready);
    sai_object_id_t resolved=1; EXPECT_FALSE(builder->getNhgOid(g.id,resolved)); EXPECT_EQ(resolved,SAI_NULL_OBJECT_ID);
    ASSERT_EQ(listener.events.size(),2u); EXPECT_EQ(listener.events.back().change,L2NhgChange::WITHDRAWING);
    member("1073741826","10.0.0.3"); step(); EXPECT_TRUE(g.ready); EXPECT_EQ(g.members.size(),2u);
    ASSERT_EQ(listener.events.size(),3u); EXPECT_EQ(listener.events.back().change,L2NhgChange::READY);
    failedGroupMembers.clear(); step(); EXPECT_EQ(g.members.size(),1u); EXPECT_EQ(g.oid,oid);
}
TEST_F(L2ProtectionRefTest, FailedAliasedPhysicalMemberIsAttemptedOnceWhileOtherMembersCleanUp)
{
    member("1073741825"); member("1073741826"); member("1073741827","10.0.0.3"); member("1073741828","10.0.0.4");
    group("2147483649","1073741825,1073741826,1073741827,1073741828"); step();
    auto &g = builder->m_protectionGroups.at("2147483649"); const auto alias = g.members.at("1073741825");
    ASSERT_EQ(alias,g.members.at("1073741826")); const auto later=g.members.at("1073741827");
    failedGroupMembers.insert(alias); group("2147483649","1073741828"); memberRemovalAttempts.clear(); step();
    EXPECT_FALSE(gmHops.count(later)); EXPECT_TRUE(gmHops.count(alias)); EXPECT_EQ(g.members.size(),3u);
    EXPECT_EQ(std::count(memberRemovalAttempts.begin(),memberRemovalAttempts.end(),alias),1);
    EXPECT_TRUE(g.ready); failedGroupMembers.clear(); step(); EXPECT_EQ(g.members.size(),1u);
}
TEST_F(L2ProtectionRefTest, CompatibleAliasRemovalStillProgressesBesideAnIndependentFailedRemoval)
{
    member("1073741825"); member("1073741826"); member("1073741827","10.0.0.3");
    group("2147483649","1073741825,1073741826,1073741827"); step();
    auto &g=builder->m_protectionGroups.at("2147483649"); const auto alias=g.members.at("1073741825"), failed=g.members.at("1073741827");
    failedGroupMembers.insert(failed); group("2147483649","1073741826"); memberRemovalAttempts.clear(); step();
    EXPECT_FALSE(g.members.count("1073741825")); EXPECT_EQ(g.members.at("1073741826"),alias);
    EXPECT_TRUE(gmHops.count(alias)); EXPECT_TRUE(gmHops.count(failed)); EXPECT_TRUE(g.ready);
    EXPECT_EQ(memberRemovalAttempts.size(),1u); EXPECT_EQ(memberRemovalAttempts.front(),failed);
}
TEST_F(L2ProtectionRefTest, SharedPeerOwnershipIsHeldOnlyByTheGroupWhoseRemovalFailed)
{
    member("1073741825"); member("1073741826","10.0.0.3");
    group("2147483649","1073741825,1073741826"); group("2147483650","1073741825,1073741826"); step();
    auto &a=builder->m_protectionGroups.at("2147483649"); auto &b=builder->m_protectionGroups.at("2147483650");
    const auto failed=a.members.at("1073741825"), removed=b.members.at("1073741825"), hop=gmHops.at(failed);
    failedGroupMembers.insert(failed); builder->deleteProtectionRow("1073741825");
    group("2147483649","1073741826"); group("2147483650","1073741826"); step();
    EXPECT_TRUE(gmHops.count(failed)); EXPECT_FALSE(gmHops.count(removed)); EXPECT_TRUE(nhTunnels.count(hop));
    EXPECT_TRUE(a.ready); EXPECT_TRUE(b.ready); EXPECT_EQ(b.members.size(),1u);
    EXPECT_EQ(source->getRemoteEndPointRefCnt("10.0.0.2"),1);
    failedGroupMembers.clear(); step(); step(); EXPECT_FALSE(nhTunnels.count(hop));
    EXPECT_EQ(source->tnl_users_.count("10.0.0.2"),0u); EXPECT_TRUE(a.ready); EXPECT_TRUE(b.ready);
}
TEST_F(L2ProtectionRefTest, RemovalErrorsLogIdentityRawStatusAndContinueAction)
{
    install(); const auto &g=builder->m_protectionGroups.at("2147483649"); const auto failed=g.members.at("1073741825");
    failedGroupMembers.insert(failed); faultStatus=SAI_STATUS_OBJECT_IN_USE;
    const auto priority=swss::Logger::getMinPrio(); swss::Logger::swssOutputNotify("orchagent","STDOUT"); swss::Logger::setMinPrio(swss::Logger::SWSS_ERROR);
    testing::internal::CaptureStdout(); builder->deleteProtectionRow("2147483649"); step();
    const auto text=testing::internal::GetCapturedStdout();
    swss::Logger::swssOutputNotify("orchagent",getenv("SWSS_SYSLOG_STDOUT") ? "STDOUT" : "SYSLOG"); swss::Logger::setMinPrio(priority);
    for (const string field : {"event=protection_member_remove_failed", "nhg_id=2147483649", "member_id=1073741825", "nhg_oid=0x", "member_oid=0x", "action=retain_and_continue"})
        EXPECT_NE(text.find(field),string::npos);
    EXPECT_NE(text.find("status="+to_string(SAI_STATUS_OBJECT_IN_USE)),string::npos);
    EXPECT_TRUE(groups.count(g.oid)); EXPECT_TRUE(gmHops.count(failed));
}
TEST_F(L2ProtectionRefTest, FailedRetirementCannotBeCanceledBySameIdReplay)
{
    install(); const auto old=builder->m_protectionGroups.at("2147483649").oid;
    const auto failed=builder->m_protectionGroups.at("2147483649").members.at("1073741825");
    failedGroupMembers.insert(failed); builder->deleteProtectionRow("2147483649"); step();
    auto consumer=dynamic_cast<Consumer*>(builder->getExecutor(APP_L2_NEXTHOP_GROUP_TABLE_NAME));
    consumer->addToSync({"2147483649",SET_COMMAND,{{"nexthop_group","1073741825"}}});
    builder->doL2NhgTask(*consumer); step(); EXPECT_EQ(consumer->m_toSync.size(),1u); EXPECT_TRUE(groups.count(old));
    failedGroupMembers.clear(); step(); EXPECT_FALSE(groups.count(old));
    builder->doL2NhgTask(*consumer); step(); EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_NE(builder->m_protectionGroups.at("2147483649").oid,old); EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649"));
}

TEST_F(L2ProtectionRefTest, SourceLossDuringFailedAdditionStillWithdrawsAndHoldsExistingPins)
{
    ProtectionListener listener(*builder); install(); L2NhgReference held;
    ASSERT_TRUE(builder->reserveNhg(2147483649u,held)); ASSERT_TRUE(builder->commitNhgRef(held));
    const auto hop=nhTunnels.begin()->first;
    member("1073741826","10.0.0.3"); group("2147483649","1073741825,1073741826");
    memberCreateHook=[&]{ source->active_=false; }; fault="member-create"; step();
    EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649")); EXPECT_TRUE(nhTunnels.count(hop));
    ASSERT_EQ(listener.events.size(),2u); EXPECT_EQ(listener.events.back().change,L2NhgChange::WITHDRAWING);
    fault.clear(); source->active_=true; step();
    EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649")); EXPECT_TRUE(nhTunnels.count(hop));
    builder->unrefNhg(held); step();
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649"));
    ASSERT_EQ(listener.events.size(),3u); EXPECT_EQ(listener.events.back().change,L2NhgChange::READY);
}

TEST_F(L2ProtectionRefTest, ObservedDeleteDefersReplayUntilOldGroupIsPhysicallyRemoved)
{
    ProtectionListener listener(*builder); install(); L2NhgReference held;
    ASSERT_TRUE(builder->reserveNhg(2147483649u,held)); builder->deleteProtectionRow("2147483649"); step();
    auto consumer=dynamic_cast<Consumer*>(builder->getExecutor(APP_L2_NEXTHOP_GROUP_TABLE_NAME));
    builder->m_fpmL2Ids.set("2147483649", {{"NULL","NULL"}});
    consumer->m_toSync.emplace("2147483649",KeyOpFieldsValuesTuple{"2147483649",SET_COMMAND,{{"nexthop_group","1073741825"}}});
    builder->doL2NhgTask(*consumer); step(); step(); L2NhgReference replay;
    EXPECT_EQ(consumer->m_toSync.size(),1u); EXPECT_FALSE(builder->reserveNhg(2147483649u,replay));
    EXPECT_TRUE(groups.count(held.oid)); EXPECT_EQ(listener.events.size(),2u);
    builder->unrefNhg(held); step(); EXPECT_FALSE(groups.count(held.oid));
    ASSERT_EQ(listener.events.size(),3u); EXPECT_EQ(listener.events.back().change,L2NhgChange::REMOVED);
    builder->doL2NhgTask(*consumer); step(); EXPECT_TRUE(consumer->m_toSync.empty());
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649")); EXPECT_NE(builder->m_protectionGroups.at("2147483649").oid,held.oid);
    ASSERT_EQ(listener.events.size(),4u); EXPECT_EQ(listener.events.back().change,L2NhgChange::READY);
}

TEST_F(L2ProtectionRefTest, NeverCreatedGroupAndDetachedObserversEmitNoEvents)
{
    ProtectionListener listener(*builder); fault="group-create"; install();
    EXPECT_TRUE(listener.events.empty()); builder->deleteProtectionRow("2147483649"); step();
    EXPECT_TRUE(listener.events.empty()); fault.clear(); builder->detach(&listener);
    group("2147483649","1073741825"); step(); EXPECT_TRUE(listener.events.empty());
}

TEST_F(L2ProtectionRefTest, SourceRecoveryWaitsForOldAttachmentAcknowledgement)
{
    ProtectionListener listener(*builder); install(); L2NhgReference held;
    ASSERT_TRUE(builder->reserveNhg(2147483649u,held)); source->active_=false; step();
    EXPECT_EQ(gmHops.size(),1u); EXPECT_EQ(tunnels.size(),1u); EXPECT_EQ(listener.events.size(),2u);
    source->active_=true; step(); EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649"));
    builder->unrefNhg(held); step(); EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649"));
    EXPECT_EQ(listener.events.size(),3u);
}

TEST_F(L2ProtectionRefTest, RegisteredTaskRetriesPhysicalDeleteAfterUnrefWithoutNewRows)
{
    ProtectionListener listener(*builder); install(); L2NhgReference held;
    ASSERT_TRUE(builder->reserveNhg(2147483649u,held));
    auto consumer=dynamic_cast<Consumer*>(builder->getExecutor(APP_L2_NEXTHOP_GROUP_TABLE_NAME));
    const auto initialized=gPortsOrch->m_initDone; auto pending=gPortsOrch->m_pendingPortSet;
    gPortsOrch->m_initDone=true; gPortsOrch->m_pendingPortSet.clear();
    consumer->m_toSync.emplace("2147483649",KeyOpFieldsValuesTuple{"2147483649",DEL_COMMAND,{}});
    consumer->m_toSync.emplace("1073741825",KeyOpFieldsValuesTuple{"1073741825",DEL_COMMAND,{}});
    builder->doTask(); EXPECT_TRUE(consumer->m_toSync.empty()); EXPECT_TRUE(groups.count(held.oid));
    builder->unrefNhg(held); fault="group-remove"; builder->doTask(); EXPECT_TRUE(groups.count(held.oid));
    EXPECT_EQ(listener.events.size(),2u); fault.clear(); builder->doTask(); builder->doTask();
    EXPECT_TRUE(groups.empty()); EXPECT_TRUE(tunnels.empty()); EXPECT_EQ(listener.events.size(),3u);
    gPortsOrch->m_initDone=initialized; gPortsOrch->m_pendingPortSet=std::move(pending);
}

TEST_F(L2ProtectionRefTest, ReplacementAcquiresNewBeforeOldAndKeepsBothThroughFailure)
{
    install(); group("2147483650","1073741825"); step();
    L2NhgReference old,replacement;
    ASSERT_TRUE(builder->reserveNhg(2147483649u,old)); ASSERT_TRUE(builder->commitNhgRef(old));
    EXPECT_FALSE(builder->reserveNhg(123u,replacement)); EXPECT_TRUE(groups.count(old.oid));
    ASSERT_TRUE(builder->reserveNhg(2147483650u,replacement));
    // Model an unconfirmed replacement while both group identities stay pinned.
    builder->deleteProtectionRow("2147483650"); EXPECT_FALSE(builder->commitNhgRef(replacement)); step();
    EXPECT_TRUE(groups.count(old.oid)); EXPECT_TRUE(groups.count(replacement.oid));
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649"));
    builder->unrefNhg(replacement); step(); EXPECT_FALSE(groups.count(replacement.oid));
    group("2147483650","1073741825"); step(); L2NhgReference confirmed;
    ASSERT_TRUE(builder->reserveNhg(2147483650u,confirmed)); ASSERT_TRUE(builder->commitNhgRef(confirmed));
    EXPECT_EQ(builder->m_protectionGroups.at("2147483649").references.size(),1u);
    builder->unrefNhg(old); builder->deleteProtectionRow("2147483649"); step();
    EXPECT_FALSE(groups.count(old.oid)); EXPECT_TRUE(groups.count(confirmed.oid)); EXPECT_EQ(gmHops.size(),1u);
    builder->unrefNhg(confirmed);
}

TEST_F(L2ProtectionRefTest, ReadyCallbackCanResolveAndPinAnotherUnchangedGroup)
{
    member("1073741825"); group("2147483650","1073741825"); step();
    ProtectionListener listener(*builder);
    listener.action=[&](const L2NhgUpdate &event) {
        if (event.change!=L2NhgChange::READY) return;
        sai_object_id_t oid; EXPECT_TRUE(builder->getNhgOid(2147483650u,oid));
        L2NhgReference held; EXPECT_TRUE(builder->reserveNhg(2147483650u,held));
        EXPECT_TRUE(builder->commitNhgRef(held)); builder->unrefNhg(held);
    };
    group("2147483649","1073741825"); step();
    ASSERT_EQ(listener.events.size(),1u); EXPECT_EQ(listener.events.front().nhg_id,2147483649u);
}

TEST_F(L2ProtectionRefTest, ObservedGroupAndMemberDeletesRebuildTheEntireReplayedPath)
{
    ProtectionListener listener(*builder); install(); L2NhgReference held;
    ASSERT_TRUE(builder->reserveNhg(2147483649u,held)); ASSERT_TRUE(builder->commitNhgRef(held));
    const auto oldHop=nhTunnels.begin()->first,oldTunnel=*tunnels.begin();
    builder->deleteProtectionRow("2147483649"); builder->deleteProtectionRow("1073741825"); step();
    auto consumer=dynamic_cast<Consumer*>(builder->getExecutor(APP_L2_NEXTHOP_GROUP_TABLE_NAME));
    consumer->m_toSync.emplace("1073741825",KeyOpFieldsValuesTuple{"1073741825",SET_COMMAND,{{"type","l3"},{"remote_vtep","10.0.0.2"},{"router_mac","02:00:00:00:00:01"}}});
    builder->m_fpmL2Ids.set("2147483649", {{"NULL","NULL"}});
    consumer->m_toSync.emplace("2147483649",KeyOpFieldsValuesTuple{"2147483649",SET_COMMAND,{{"nexthop_group","1073741825"}}});
    builder->doL2NhgTask(*consumer); EXPECT_EQ(consumer->m_toSync.size(),2u); step();
    EXPECT_TRUE(nhTunnels.count(oldHop)); EXPECT_TRUE(tunnels.count(oldTunnel));
    builder->unrefNhg(held); step(); EXPECT_FALSE(groups.count(held.oid)); EXPECT_FALSE(nhTunnels.count(oldHop));
    builder->doL2NhgTask(*consumer); EXPECT_TRUE(consumer->m_toSync.empty()); step();
    EXPECT_FALSE(tunnels.count(oldTunnel)); EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649"));
    EXPECT_EQ(source->tnl_users_.at("10.0.0.2").protection_refcnt,1u);
    ASSERT_EQ(listener.events.size(),4u); EXPECT_EQ(listener.events[2].change,L2NhgChange::REMOVED);
    EXPECT_EQ(listener.events[3].change,L2NhgChange::READY);
}

namespace
{
map<sai_object_id_t, map<sai_attr_id_t, sai_attribute_t>> bridgeValues;
vector<pair<sai_object_id_t,sai_attr_id_t>> bridgeWrites;
unsigned bridgeReads = 0;
string bridgeFault;
sai_status_t bridgeStatus=SAI_STATUS_FAILURE;
sai_attr_id_t bridgeMismatch=0;
// Emulate a successful command whose committed path readback is delayed.
// Unlike bridgeMismatch, this returns a valid old path, not an invalid enum.
int32_t bridgeDelayedState = -1;
std::function<void()> readHook, writeHook;
sai_status_t bridgeSet(sai_object_id_t oid, const sai_attribute_t *attr)
{
    bridgeWrites.emplace_back(oid,attr->id);
    EXPECT_NE(attr->id,SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_REVERTIVE);
    EXPECT_NE(attr->id,SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_WAIT_TO_RESTORE_TIME);
    if (attr->id==SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE)
    {
        EXPECT_NE(attr->value.s32,SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_AUTO);
    }
    string stage=attr->id==SAI_BRIDGE_PORT_ATTR_ADMIN_STATE ? (attr->value.booldata ? "enable" : "disable") :
        attr->id==SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID ?
        (attr->value.oid ? "group" : "clear") :
        attr->id==SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_MODE ? "mode" : "admin";
    if (bridgeFault==stage) return bridgeStatus;
    bridgeValues[oid][attr->id]=*attr;
    if (writeHook) { auto hook=std::move(writeHook); hook(); }
    return SAI_STATUS_SUCCESS;
}
sai_status_t bridgeGet(sai_object_id_t oid, uint32_t count, sai_attribute_t *attrs)
{
    ++bridgeReads;
    if (bridgeFault==(count==2 ? "clear-get" : "get")) return bridgeStatus;
    for (uint32_t i=0;i<count;++i)
    {
        auto id=attrs[i].id;
        if (id==SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_STATE)
            attrs[i].value.s32=!bridgeValues[oid][SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID].value.oid ?
                SAI_BRIDGE_PORT_PROTECTION_STATE_NOT_APPLICABLE :
                bridgeValues[oid][SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE].value.s32==
                SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY ? SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY : SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION;
        else attrs[i]=bridgeValues[oid][id];
        attrs[i].id=id;
        if (count == 4 && id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_STATE && bridgeDelayedState >= 0)
            attrs[i].value.s32 = bridgeDelayedState;
        if (id==bridgeMismatch) attrs[i].value.u32=42;
    }
    if (readHook) { auto hook=std::move(readHook); hook(); }
    return SAI_STATUS_SUCCESS;
}
}
class BpProtectionAttachmentTest : public L2ProtectionTest
{
protected:
    unique_ptr<BpProtOrch> protection;
    sai_bridge_api_t savedBridge;
    uint64_t taskNow = 1000;
    void PostSetUp() override
    {
        L2ProtectionTest::PostSetUp();
        bridgeValues.clear(); bridgeWrites.clear(); bridgeReads=0; bridgeFault.clear(); bridgeMismatch=0; bridgeDelayedState=-1; readHook={}; writeHook={}; bridgeStatus=SAI_STATUS_FAILURE;
        savedBridge=*sai_bridge_api;
        sai_bridge_api->set_bridge_port_attribute=bridgeSet; sai_bridge_api->get_bridge_port_attribute=bridgeGet;
        protection.reset(new BpProtOrch(m_config_db.get(),m_state_db.get(),m_app_db.get(),gPortsOrch,builder.get()));
        protection->m_now = [&]{ return taskNow; };
        po(); install();
    }
    void PreTearDown() override
    {
        bridgeFault.clear(); bridgeMismatch=0; bridgeDelayedState=-1; readHook={}; writeHook={};
        config(DEL_COMMAND); protection.reset(); *sai_bridge_api=savedBridge;
        L2ProtectionTest::PreTearDown();
    }
    void po(sai_port_oper_status_t status=SAI_PORT_OPER_STATUS_UP,sai_object_id_t oid=0xabc,Port::Type type=Port::LAG)
    {
        Port port("PortChannel0",type); port.m_bridge_port_id=oid; port.m_oper_status=status;
        gPortsOrch->setPort("PortChannel0",port);
    }
    void apply(const string &tableName,const string &op,const vector<FieldValueTuple>&fields={})
    {
        Table table(tableName==CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME ? m_config_db.get() : m_app_db.get(),tableName);
        table.del("PortChannel0"); if (op==SET_COMMAND) table.set("PortChannel0",fields);
        auto consumer=dynamic_cast<Consumer*>(protection->getConsumerBase(tableName));
        consumer->addToSync(KeyOpFieldsValuesTuple{"PortChannel0",op,fields});
        // A notification drives exactly one attempt, independently of periodic retry.
        protection->doTask(*consumer);
    }
    void config(const string &op=SET_COMMAND,const string &admin="AUTO")
    { apply(CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME,op,{{"admin_mode",admin},{"revertive","true"},{"wait_to_restore_time_ms","5000"}}); }
    void bind(const string &id="2147483649")
    { apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,SET_COMMAND,{{"nexthop_group",id}}); }
    map<string,string> state()
    {
        vector<FieldValueTuple> fields; Table(m_state_db.get(),STATE_BRIDGE_PORT_PROTECTION_TABLE_NAME).get("PortChannel0",fields);
        return {fields.begin(),fields.end()};
    }
    BpProtOrch::Attachment &runtime() { return protection->m_attachments.at("PortChannel0"); }
    string desired()
    {
        if (!runtime().inputsKnown) return "UNKNOWN";
        return runtime().selected == SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY ? "PRIMARY" : "PROTECTION";
    }
    string wtrStatus() { return runtime().wtrRunning ? "WAIT_TO_RESTORE" : "IDLE"; }
    size_t refs(const string &id="2147483649") { return builder->m_protectionGroups.at(id).references.size(); }
    void retry() { taskNow += 5000; protection->doTask(); }
    void attached(const string &admin="AUTO") { config(SET_COMMAND,admin); bind(); }
};

TEST_F(BpProtectionAttachmentTest, StaticAutoConfirmsThreeWritesAndStateThenClearsBeforeUnref)
{
    auto consumer=dynamic_cast<Consumer*>(protection->getConsumerBase(APP_BP_PROTECTION_BINDING_TABLE_NAME));
    EXPECT_EQ(consumer->getDbName(),"APPL_DB");
    attached(); EXPECT_EQ(bridgeWrites.size(),3u); EXPECT_EQ(refs(),1u);
    EXPECT_EQ(state().at("admin_mode"),"AUTO"); EXPECT_EQ(state().at("protection_state"),"PRIMARY");
    EXPECT_EQ(runtime().programmed,"PRIMARY"); EXPECT_EQ(state().at("reconciliation_state"),"STEADY");
    auto writes=bridgeWrites.size(); retry(); EXPECT_EQ(bridgeWrites.size(),writes);
    readHook=[&]{EXPECT_EQ(refs(),1u);}; apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND);
    EXPECT_EQ(refs(),0u); EXPECT_EQ(state().at("reconciliation_state"),"INCONSISTENT");
    EXPECT_EQ(state().at("admin_mode"),"AUTO"); EXPECT_EQ(state().count("protection_state"),0u);
}
TEST_F(BpProtectionAttachmentTest, ConfiguredProtectionAndPrimaryAreHonored)
{
    attached("PROTECTION"); EXPECT_EQ(state().at("protection_state"),"PROTECTION");
    po(SAI_PORT_OPER_STATUS_DOWN); config(SET_COMMAND,"PRIMARY"); EXPECT_EQ(state().at("protection_state"),"PRIMARY");
    config(); EXPECT_EQ(state().at("protection_state"),"PROTECTION"); EXPECT_EQ(refs(),1u);
}
TEST_F(BpProtectionAttachmentTest, InitialDownAttachesAndHonorsAllAdminPolicies)
{
    for (const string admin : {"AUTO", "PRIMARY", "PROTECTION"})
    {
        po(SAI_PORT_OPER_STATUS_DOWN); auto writes = bridgeWrites.size(); attached(admin);
        const string expected = admin == "PRIMARY" ? "PRIMARY" : "PROTECTION";
        EXPECT_EQ(bridgeWrites.size(), writes+3); EXPECT_EQ(refs(), 1u);
        EXPECT_EQ(state().at("admin_mode"), admin);
        EXPECT_EQ(state().at("protection_state"), expected);
        EXPECT_EQ(runtime().programmed, expected);
        EXPECT_EQ(state().at("reconciliation_state"), "STEADY");
        EXPECT_EQ(wtrStatus(), "IDLE");
        retry(); EXPECT_EQ(bridgeWrites.size(), writes+3);
        config(DEL_COMMAND); EXPECT_EQ(refs(), 0u);
    }
}
TEST_F(BpProtectionAttachmentTest, MissingAndWrongPortKindsDoNotProgram)
{
    po(SAI_PORT_OPER_STATUS_UP,0); attached(); EXPECT_TRUE(bridgeWrites.empty());
    EXPECT_EQ(runtime().lastReason,"bridge_port_pending");
    po(SAI_PORT_OPER_STATUS_UP,0xabc,Port::PHY); retry(); EXPECT_TRUE(bridgeWrites.empty());
    gPortsOrch->m_portList.erase("PortChannel0"); retry(); EXPECT_TRUE(bridgeWrites.empty());
    po(); retry(); EXPECT_EQ(refs(),1u);
}
TEST_F(BpProtectionAttachmentTest, BindingBeforeConfigAndMissingGroupAreRetained)
{
    bind("4294967295"); EXPECT_TRUE(state().empty()); config(); EXPECT_EQ(runtime().lastReason,"nhg_pending");
    group("4294967295","1073741825"); step(); retry(); EXPECT_EQ(refs("4294967295"),1u);
}
TEST_F(BpProtectionAttachmentTest, BindingValidationAndAuthoritativeRows)
{
    attached();
    for (const string id : {"0","","01","-1","0x80000001","4294967296","21a"})
    { bind(id); EXPECT_EQ(refs(),1u); EXPECT_EQ(state().at("reconciliation_state"),"STEADY"); }
    apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,SET_COMMAND,{{"unexpected","1"}});
    apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,SET_COMMAND,{{"nexthop_group","1"},{"extra","x"}});
    apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,"INVALID"); EXPECT_EQ(refs(),1u);
    auto consumer=dynamic_cast<Consumer*>(protection->getConsumerBase(APP_BP_PROTECTION_BINDING_TABLE_NAME));
    for (const string name : {"Ethernet0","PortChannel","PortChannelx","PortChannel12345"})
    {
        Table(m_app_db.get(),APP_BP_PROTECTION_BINDING_TABLE_NAME).set(name,{{"nexthop_group","2147483649"}});
        consumer->addToSync(KeyOpFieldsValuesTuple{name,SET_COMMAND,{}}); protection->doTask(*consumer);
    }
    Table(m_app_db.get(),APP_BP_PROTECTION_BINDING_TABLE_NAME).del("PortChannel0");
    consumer->addToSync(KeyOpFieldsValuesTuple{"PortChannel0",SET_COMMAND,{{"nexthop_group","2147483649"}}}); protection->doTask(*consumer);
    EXPECT_EQ(refs(),0u); EXPECT_EQ(runtime().lastReason,"binding_pending");
}
TEST_F(BpProtectionAttachmentTest, EachAttachWriteAndReadFailureRetainsReservationUntilRetry)
{
    config();
    for (const string stage : {"group","mode","admin","get"})
    {
        bridgeFault=stage; bind(); EXPECT_EQ(refs(),1u); EXPECT_EQ(state().at("reconciliation_state"),"INCONSISTENT");
        bridgeFault.clear(); retry(); EXPECT_EQ(state().at("reconciliation_state"),"STEADY");
        apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND); EXPECT_EQ(refs(),0u);
    }
}
TEST_F(BpProtectionAttachmentTest, EveryReadbackMismatchPreventsSteadyAndPreservesPin)
{
    config();
    for (const auto id : {SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID,SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_MODE,
        SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE,SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_STATE})
    {
        bridgeMismatch=id; bind(); EXPECT_EQ(refs(),1u); EXPECT_EQ(runtime().lastReason,"attach_mismatch");
        bridgeMismatch=0; retry(); EXPECT_EQ(state().at("reconciliation_state"),"STEADY");
        apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND);
    }
}
TEST_F(BpProtectionAttachmentTest, UnsupportedIsLatchedAndClearedBeforeExplicitRetry)
{
    for (sai_status_t status : std::vector<sai_status_t>{SAI_STATUS_NOT_IMPLEMENTED,SAI_STATUS_NOT_SUPPORTED,
        SAI_STATUS_ATTR_NOT_SUPPORTED_0,SAI_STATUS_ATTR_NOT_SUPPORTED_MAX,
        SAI_STATUS_ATTR_NOT_IMPLEMENTED_0,SAI_STATUS_ATTR_NOT_IMPLEMENTED_MAX,
        SAI_STATUS_UNKNOWN_ATTRIBUTE_0,SAI_STATUS_UNKNOWN_ATTRIBUTE_MAX})
    {
        bridgeStatus=status; bridgeFault="admin"; attached(); EXPECT_EQ(state().at("reconciliation_state"),"INCONSISTENT");
        EXPECT_EQ(refs(),1u); bridgeFault.clear(); retry(); EXPECT_EQ(refs(),0u);
        EXPECT_EQ(state().at("reconciliation_state"),"INCONSISTENT"); auto writes=bridgeWrites.size();
        retry(); EXPECT_EQ(bridgeWrites.size(),writes); config(); EXPECT_EQ(refs(),1u);
        apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND);
    }
}
TEST_F(BpProtectionAttachmentTest, ClearSetReadAndMismatchKeepPinAndConfigDeletionRetries)
{
    for (const string stage : {"clear","clear-get","bridgeMismatch"})
    {
        attached(); if(stage=="bridgeMismatch") bridgeMismatch=SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID; else bridgeFault=stage;
        config(DEL_COMMAND); EXPECT_EQ(refs(),1u); EXPECT_EQ(state().at("reconciliation_state"),"INCONSISTENT");
        bridgeFault.clear(); bridgeMismatch=0; retry(); EXPECT_EQ(refs(),0u); EXPECT_TRUE(state().empty());
    }
}
TEST_F(BpProtectionAttachmentTest, ReplacementKeepsBothPinsUntilNewGroupAndPathAreConfirmed)
{
    attached(); group("2147483650","1073741825"); step();
    auto writes=bridgeWrites.size(); bridgeFault="mode"; bind("2147483650");
    EXPECT_EQ(refs(),1u); EXPECT_EQ(refs("2147483650"),1u);
    EXPECT_EQ(bridgeValues[0xabc][SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID].value.oid,
        builder->m_protectionGroups.at("2147483650").oid);
    // No intervening NULL association is written for an ordinary replacement.
    EXPECT_EQ(bridgeWrites.size()-writes,2u);
    bridgeFault.clear(); readHook=[&]{EXPECT_EQ(refs(),1u); EXPECT_EQ(refs("2147483650"),1u);}; retry();
    EXPECT_EQ(refs(),0u); EXPECT_EQ(refs("2147483650"),1u); EXPECT_EQ(state().at("reconciliation_state"),"STEADY");
}
TEST_F(BpProtectionAttachmentTest, SupersededPartialReplacementRetainsAllWrittenPinsUntilConfirmedClear)
{
    attached(); group("2147483650","1073741825"); group("2147483651","1073741825"); group("2147483652","1073741825"); step();
    bridgeFault="mode"; bind("2147483650"); EXPECT_EQ(refs("2147483650"),1u);
    bridgeFault="clear"; bind("2147483651");
    EXPECT_EQ(refs(),1u); EXPECT_EQ(refs("2147483650"),1u); EXPECT_EQ(refs("2147483651"),1u);
    bind("2147483652"); EXPECT_EQ(refs("2147483651"),0u); EXPECT_EQ(refs("2147483652"),1u);
    EXPECT_EQ(refs("2147483650"),1u); apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND); EXPECT_EQ(refs(),1u);
    bridgeFault.clear(); retry(); EXPECT_EQ(refs(),0u);
    EXPECT_EQ(refs("2147483650"),0u); EXPECT_EQ(refs("2147483652"),0u);
}
TEST_F(BpProtectionAttachmentTest, GroupWithdrawalWaitsForConfirmedClear)
{
    attached(); bridgeFault="clear"; builder->deleteProtectionRow("2147483649"); step();
    retry(); step(); EXPECT_EQ(refs(),1u); EXPECT_TRUE(groups.size());
    bridgeFault.clear(); retry(); step(); EXPECT_EQ(groups.size(),0u);
    EXPECT_EQ(runtime().lastReason,"nhg_pending"); EXPECT_EQ(state().at("admin_mode"),"AUTO");
}
TEST_F(BpProtectionAttachmentTest, StalePortRetryClearsStoredOidBeforeUsingRecreatedPort)
{
    attached(); bridgeFault="clear"; po(SAI_PORT_OPER_STATUS_UP,0xdef); retry();
    EXPECT_EQ(bridgeWrites.back().first,0xabcu); EXPECT_EQ(refs(),1u);
    bridgeFault.clear(); retry(); EXPECT_EQ(refs(),0u);
    EXPECT_EQ(runtime().lastReason,"bridge_port_replaced"); retry();
    EXPECT_EQ(bridgeWrites.back().first,0xdefu); EXPECT_EQ(refs(),1u);
}
TEST_F(BpProtectionAttachmentTest, CommitWithdrawalDuringReadbackRetainsPinUntilClear)
{
    config(); readHook=[&]{builder->deleteProtectionRow("2147483649"); step();}; bind();
    EXPECT_EQ(runtime().lastReason,"bridge_port_dependency_changed"); EXPECT_EQ(refs(),1u);
    retry(); EXPECT_EQ(refs(),0u); step(); EXPECT_TRUE(groups.empty());
}
TEST_F(BpProtectionAttachmentTest, StaleConfigSetClearsAndRemovesState)
{
    attached(); Table(m_config_db.get(),CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME).del("PortChannel0");
    auto consumer=dynamic_cast<Consumer*>(protection->getConsumerBase(CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME));
    consumer->addToSync(KeyOpFieldsValuesTuple{"PortChannel0",SET_COMMAND,{}}); protection->doTask(*consumer);
    EXPECT_EQ(refs(),0u); EXPECT_TRUE(state().empty());
}

TEST_F(BpProtectionAttachmentTest, ReservationExhaustionIsPendingAndDoesNotWrite)
{
    builder->m_nextProtectionReference=std::numeric_limits<uint64_t>::max(); attached();
    EXPECT_EQ(runtime().lastReason,"nhg_reservation_pending"); EXPECT_TRUE(bridgeWrites.empty());
    builder->m_nextProtectionReference=0; retry(); group("2147483650","1073741825"); step();
    builder->m_nextProtectionReference=std::numeric_limits<uint64_t>::max(); bind("2147483650");
    EXPECT_EQ(runtime().lastReason,"nhg_reservation_pending"); EXPECT_EQ(refs(),1u); EXPECT_EQ(refs("2147483650"),0u);
    builder->m_nextProtectionReference=1; bridgeFault="mode"; bind("2147483650");
    EXPECT_EQ(refs("2147483650"),1u); group("2147483651","1073741825"); step();
    builder->m_nextProtectionReference=std::numeric_limits<uint64_t>::max(); bind("2147483651");
    EXPECT_EQ(runtime().lastReason,"nhg_reservation_pending"); EXPECT_EQ(refs(),1u); EXPECT_EQ(refs("2147483650"),1u);
    builder->m_nextProtectionReference=2;
}
TEST_F(BpProtectionAttachmentTest, ConfirmedRemovedOldPortReleasesWithoutWritingNewObject)
{
    attached(); po(SAI_PORT_OPER_STATUS_UP,0xdef); bridgeStatus=SAI_STATUS_ITEM_NOT_FOUND;
    bridgeFault="clear"; retry(); EXPECT_EQ(refs(),1u);
    // SET absence alone is insufficient when GET still reports an association.
    bridgeFault="clear-get"; retry(); EXPECT_EQ(refs(),0u);
    EXPECT_EQ(bridgeWrites.back().first,0xabcu); bridgeFault.clear(); retry(); EXPECT_EQ(refs(),1u);
}
TEST_F(BpProtectionAttachmentTest, InitialPartialAttachmentRetriesBackupIfPortDropsBeforeConfirmation)
{
    bridgeFault="admin"; attached(); EXPECT_EQ(refs(),1u);
    bridgeFault.clear(); po(SAI_PORT_OPER_STATUS_DOWN); retry();
    EXPECT_EQ(refs(),1u); EXPECT_EQ(state().at("protection_state"),"PROTECTION");
    EXPECT_EQ(state().at("reconciliation_state"),"STEADY");
    EXPECT_EQ(bridgeValues[0xabc][SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID].value.oid,
        builder->m_protectionGroups.at("2147483649").oid);
}
TEST_F(BpProtectionAttachmentTest, FirstFailedReadOmitsPathAndConfirmedRetryPublishesHldRow)
{
    bridgeFault = "get"; attached(); EXPECT_EQ(refs(), 1u);
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT"); EXPECT_FALSE(state().count("protection_state"));
    EXPECT_TRUE(state().at("last_resync_ts").empty()); bridgeFault.clear(); retry();
    EXPECT_EQ(state().size(), 11u); EXPECT_EQ(state().at("protection_state"), "PRIMARY");
    EXPECT_EQ(state().at("reconciliation_state"), "STEADY"); EXPECT_FALSE(state().at("last_resync_ts").empty());
    for (const string field : {"last_event", "programmed_sai_admin_mode", "pending_dependencies", "wtr_state", "retry_attempts"})
        EXPECT_FALSE(state().count(field));
}
TEST_F(BpProtectionAttachmentTest, FailedClearReadKeepsObservationUntilConfirmedTeardown)
{
    attached(); const auto observedAt = state().at("last_resync_ts"); bridgeFault = "clear-get";
    apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND);
    EXPECT_EQ(state().at("protection_state"), "PRIMARY"); EXPECT_EQ(state().at("last_resync_ts"), observedAt);
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT"); EXPECT_EQ(refs(), 1u);
    bridgeFault.clear(); retry(); EXPECT_FALSE(state().count("protection_state")); EXPECT_EQ(refs(), 0u);
    EXPECT_EQ(state().at("admin_mode"), "AUTO"); EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    bind(); runtime().lastResync = "before-successful-clear-read";
    bridgeMismatch = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID;
    apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND);
    EXPECT_EQ(runtime().lastReason, "clear_mismatch"); EXPECT_EQ(refs(), 1u);
    EXPECT_NE(state().at("last_resync_ts"), "before-successful-clear-read");
    EXPECT_EQ(state().at("protection_state"), "PRIMARY");
    bridgeMismatch = 0; retry(); EXPECT_FALSE(state().count("protection_state"));
}
TEST_F(BpProtectionAttachmentTest, InitialReadbackFromDepartedBridgeCannotCertifySteady)
{
    for (unsigned change = 0; change < 3; ++change)
    {
        apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND);
        Port old; ASSERT_TRUE(gPortsOrch->getPort("PortChannel0", old));
        config(); readHook = [&, old, change] {
            if (change == 0) gPortsOrch->m_portList.erase(old.m_alias);
            else
            {
                Port replacement = old;
                if (change == 1) ++replacement.m_bridge_port_id;
                else ++replacement.m_bridge_port_generation;
                gPortsOrch->setPort(old.m_alias, replacement);
            }
        };
        bind(); EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
        EXPECT_FALSE(state().count("protection_state")); EXPECT_TRUE(state().at("last_resync_ts").empty());
        EXPECT_EQ(refs(), 1u); gPortsOrch->setPort(old.m_alias, old); config(DEL_COMMAND);
    }
}
TEST_F(BpProtectionAttachmentTest, CachedObservationRejectsDepartedAndReusedBridgeIdentities)
{
    attached(); Port old; ASSERT_TRUE(gPortsOrch->getPort("PortChannel0", old));
    const auto observedAt = state().at("last_resync_ts");
    for (unsigned change = 0; change < 3; ++change)
    {
        if (change == 0) gPortsOrch->m_portList.erase(old.m_alias);
        else
        {
            Port replacement = old;
            if (change == 1) ++replacement.m_bridge_port_id;
            else ++replacement.m_bridge_port_generation;
            gPortsOrch->setPort(old.m_alias, replacement);
        }
        // A late observation cannot refresh the cache of a departed identity.
        protection->recordObservation("PortChannel0", runtime(), "PROTECTION");
        protection->publishRuntime("PortChannel0", "INCONSISTENT", "stale_observation");
        EXPECT_EQ(state().at("protection_state"), "PRIMARY");
        EXPECT_EQ(state().at("last_resync_ts"), observedAt);
        EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
        gPortsOrch->setPort(old.m_alias, old);
    }
}
TEST_F(BpProtectionAttachmentTest, ActiveAutoFollowsCachedAggregateDown)
{
    attached(); po(SAI_PORT_OPER_STATUS_DOWN); auto writes=bridgeWrites.size(); retry();
    EXPECT_EQ(bridgeWrites.size(),writes+1); EXPECT_EQ(state().at("protection_state"),"PROTECTION");
    config(SET_COMMAND,"PROTECTION"); EXPECT_EQ(state().at("protection_state"),"PROTECTION");
}

TEST_F(BpProtectionAttachmentTest, AutoOnProtectedUpPortStartsWtrWithoutImmediateReversion)
{
    attached("PROTECTION"); config(); EXPECT_EQ(state().at("admin_mode"),"AUTO");
    EXPECT_EQ(state().at("protection_state"),"PROTECTION"); EXPECT_EQ(refs(),1u);
}

TEST_F(BpProtectionAttachmentTest, TwoBridgePortsKeepWithdrawingGroupUntilBothConfirmClear)
{
    attached(); Port second("PortChannel1",Port::LAG); second.m_bridge_port_id=0x999;
    second.m_oper_status=SAI_PORT_OPER_STATUS_UP; gPortsOrch->setPort("PortChannel1",second);
    auto row=[&](const string &tableName,const string &op,const vector<FieldValueTuple> &fields)
    {
        Table table(tableName==CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME ? m_config_db.get() : m_app_db.get(),tableName);
        if(op==DEL_COMMAND) table.del("PortChannel1"); else table.set("PortChannel1",fields);
        auto consumer=dynamic_cast<Consumer*>(protection->getConsumerBase(tableName));
        consumer->addToSync(KeyOpFieldsValuesTuple{"PortChannel1",op,fields}); protection->doTask(*consumer);
    };
    row(CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME,SET_COMMAND,{{"admin_mode","PROTECTION"}});
    row(APP_BP_PROTECTION_BINDING_TABLE_NAME,SET_COMMAND,{{"nexthop_group","2147483649"}});
    EXPECT_EQ(refs(),2u); bridgeFault="clear"; builder->deleteProtectionRow("2147483649"); step();
    apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND); EXPECT_EQ(refs(),2u);
    bridgeFault.clear(); row(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND,{}); EXPECT_EQ(refs(),1u);
    step(); EXPECT_FALSE(groups.empty()); retry(); EXPECT_EQ(refs(),0u); step(); EXPECT_TRUE(groups.empty());
    row(CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME,DEL_COMMAND,{});
}

TEST_F(BpProtectionAttachmentTest, ClearedGroupWithDelayedPathStateKeepsPinUntilNotApplicable)
{
    attached(); bridgeMismatch=SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_STATE;
    apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND); EXPECT_EQ(refs(),1u);
    EXPECT_EQ(runtime().lastReason,"clear_mismatch"); bridgeMismatch=0;
    retry(); EXPECT_EQ(refs(),0u);
}

TEST_F(BpProtectionAttachmentTest, SupersedingPartialReplacementBackToOriginalConfirmsClearFirst)
{
    attached(); group("2147483650","1073741825"); step(); bridgeFault="mode"; bind("2147483650");
    bridgeFault="clear"; bind(); EXPECT_EQ(refs(),2u); EXPECT_EQ(refs("2147483650"),1u);
    bridgeFault.clear(); readHook=[&]{EXPECT_EQ(refs(),2u); EXPECT_EQ(refs("2147483650"),1u);};
    retry(); EXPECT_EQ(refs(),1u); EXPECT_EQ(refs("2147483650"),0u);
    EXPECT_EQ(state().at("reconciliation_state"),"STEADY");
}

namespace
{
sai_object_id_t bridgeAllocation = 0xabc;
unsigned bridgeRemoves = 0, bridgeCreates = 0, bridgeFlushes = 0;
vector<sai_object_id_t> bridgeFlushedVlans;
bool hostTagFailure = false;
vector<int32_t> hostTags;
sai_status_t observerHostTag(sai_object_id_t, const sai_attribute_t *attribute)
{
    hostTags.push_back(attribute->value.s32);
    return hostTagFailure ? SAI_STATUS_INSUFFICIENT_RESOURCES : SAI_STATUS_SUCCESS;
}
sai_status_t observerBridgeCreate(sai_object_id_t *oid, sai_object_id_t, uint32_t, const sai_attribute_t *)
{
    ++bridgeCreates;
    *oid = bridgeAllocation;
    bridgeValues[*oid].clear();
    return SAI_STATUS_SUCCESS;
}
sai_status_t observerBridgeRemove(sai_object_id_t oid)
{
    ++bridgeRemoves;
    EXPECT_EQ(bridgeValues[oid][SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID].value.oid, SAI_NULL_OBJECT_ID);
    if (bridgeFault == "remove") return SAI_STATUS_OBJECT_IN_USE;
    bridgeValues.erase(oid);
    return SAI_STATUS_SUCCESS;
}
sai_status_t observerFlush(sai_object_id_t, uint32_t count, const sai_attribute_t *attrs)
{
    ++bridgeFlushes;
    sai_object_id_t vlan = SAI_NULL_OBJECT_ID;
    for (uint32_t i = 0; i < count; ++i)
        if (attrs[i].id == SAI_FDB_FLUSH_ATTR_BV_ID) vlan = attrs[i].value.oid;
    bridgeFlushedVlans.push_back(vlan);
    return SAI_STATUS_SUCCESS;
}
struct PortListener : Observer
{
    std::function<void(SubjectType,void *)> action;
    void update(SubjectType type, void *context) override { if (action) action(type, context); }
};
struct ObserverVlanRemoveGuard
{
    decltype(sai_vlan_api->remove_vlan_member) saved = sai_vlan_api->remove_vlan_member;
    decltype(sai_vlan_api->create_vlan_member) savedCreate = sai_vlan_api->create_vlan_member;
    ~ObserverVlanRemoveGuard() {
        sai_vlan_api->remove_vlan_member = saved;
        sai_vlan_api->create_vlan_member = savedCreate;
    }
};
}

class BpProtectionObserverTest : public BpProtectionAttachmentTest
{
protected:
    unique_ptr<StpOrch> stp;
    StpOrch *previousStp = nullptr;
    unique_ptr<MlagOrch> mlag;
    MlagOrch *previousMlag = nullptr;
    sai_fdb_api_t savedFdb;
    sai_hostif_api_t savedHost;
    void PostSetUp() override
    {
        BpProtectionAttachmentTest::PostSetUp();
        bridgeCreates = bridgeRemoves = bridgeFlushes = 0; bridgeAllocation = 0xabc;
        bridgeFlushedVlans.clear();
        savedFdb = *sai_fdb_api; savedHost = *sai_hostif_api;
        hostTagFailure = false; hostTags.clear();
        sai_hostif_api->set_hostif_attribute = observerHostTag;
        sai_fdb_api->flush_fdb_entries = observerFlush;
        sai_bridge_api->create_bridge_port = observerBridgeCreate;
        sai_bridge_api->remove_bridge_port = observerBridgeRemove;
        previousStp = gStpOrch;
        vector<string> tables;
        stp.reset(new StpOrch(m_app_db.get(), m_state_db.get(), tables)); gStpOrch = stp.get();
        // The real port-down notification also invokes FdbOrch, as in orchdaemon.
        previousMlag = gMlagOrch;
        mlag.reset(new MlagOrch(m_config_db.get(), tables)); gMlagOrch = mlag.get();
    }
    void PreTearDown() override
    {
        // Policy withdrawal can now call FdbOrch. Keep its SAI hooks and MLAG
        // dependency alive until that callback finishes; later DEL is idempotent.
        config(DEL_COMMAND);
        gMlagOrch = previousMlag; mlag.reset();
        gStpOrch = previousStp; stp.reset(); *sai_fdb_api = savedFdb; *sai_hostif_api = savedHost;
        BpProtectionAttachmentTest::PreTearDown();
    }
    Port port()
    {
        Port value; EXPECT_TRUE(gPortsOrch->getPort("PortChannel0", value)); return value;
    }
    void addEvent(const Port &value)
    {
        PortUpdate event{value, true}; gPortsOrch->notify(SUBJECT_TYPE_BRIDGE_PORT_CHANGE, &event);
    }
    BridgePortRemovalUpdate preRemove(const Port &value, bool ready = true)
    {
        BridgePortRemovalUpdate event{value, ready};
        gPortsOrch->notify(SUBJECT_TYPE_BRIDGE_PORT_PRE_REMOVE, &event); return event;
    }
    void removedEvent(const Port &old)
    {
        Port value = old; value.m_bridge_port_id = 0; value.m_bridge_port_generation = 0;
        PortUpdate event{value, false, old.m_bridge_port_id, old.m_bridge_port_generation};
        gPortsOrch->notify(SUBJECT_TYPE_BRIDGE_PORT_CHANGE, &event);
    }
    void noGroup()
    {
        builder->deleteProtectionRow("2147483649"); step(); ASSERT_TRUE(groups.empty());
    }
    Consumer &vlanDelete(bool parent, bool member, bool otherMember = false)
    {
        Port vlan("Vlan1000", Port::VLAN); vlan.m_vlan_info.vlan_id = 1000;
        vlan.m_vlan_info.vlan_oid = 0x8800;
        if (member)
        {
            vlan.m_members.insert("PortChannel0");
            gPortsOrch->m_portVlanMember["PortChannel0"][1000] = {0x9000, SAI_VLAN_TAGGING_MODE_TAGGED};
        }
        if (parent) gPortsOrch->setPort(vlan.m_alias, vlan);
        gPortsOrch->m_bridge_port_ref_count["PortChannel0"] = unsigned(member) + unsigned(otherMember);
        if (otherMember)
            gPortsOrch->m_portVlanMember["PortChannel0"][2000] = {0x9001, SAI_VLAN_TAGGING_MODE_TAGGED};
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_VLAN_MEMBER_TABLE_NAME));
        EXPECT_NE(consumer, nullptr);
        consumer->addToSync(KeyOpFieldsValuesTuple{"Vlan1000:PortChannel0", DEL_COMMAND, {}});
        return *consumer;
    }
};

TEST_F(BpProtectionObserverTest, LastVlanDeleteRetainsTaskUntilProtectionAndBridgeRemovalFinish)
{
    attached(); auto &consumer = vlanDelete(true, true); ObserverVlanRemoveGuard restore;
    sai_vlan_api->remove_vlan_member = [](sai_object_id_t) -> sai_status_t {
        return ++calls["vlan-member-remove"] == 1 ? SAI_STATUS_OBJECT_IN_USE : SAI_STATUS_SUCCESS;
    };
    bridgeFault = "clear";
    gPortsOrch->doVlanMemberTask(consumer);
    auto value = port();
    EXPECT_EQ(consumer.m_toSync.size(), 1u); EXPECT_EQ(gPortsOrch->getBridgePortReferenceCount(value), 1u);
    gPortsOrch->doVlanMemberTask(consumer);
    EXPECT_EQ(consumer.m_toSync.size(), 1u); EXPECT_EQ(refs(), 1u);
    EXPECT_EQ(bridgeRemoves, 0u);
    // Explicit VLAN removal retains its normal VLAN-scoped FDB flush, while
    // the deferred bridge-wide flush must wait for protection acknowledgement.
    ASSERT_EQ(bridgeFlushedVlans.size(), 1u); EXPECT_EQ(bridgeFlushedVlans.back(), 0x8800u);
    EXPECT_EQ(gPortsOrch->m_portVlanMember.count("PortChannel0"), 0u);
    bridgeFault.clear(); retry(); gPortsOrch->doVlanMemberTask(consumer);
    EXPECT_TRUE(consumer.m_toSync.empty()); EXPECT_EQ(refs(), 0u);
    EXPECT_EQ(port().m_bridge_port_id, SAI_NULL_OBJECT_ID);
    EXPECT_EQ(bridgeRemoves, 1u); EXPECT_EQ(calls["vlan-member-remove"], 2u);
}

TEST_F(BpProtectionObserverTest, PendingVlanDeleteRetriesAfterItsMemberIsAlreadyGone)
{
    attached(); auto &consumer = vlanDelete(true, false); auto value = port(); bridgeFault = "clear";
    ASSERT_FALSE(gPortsOrch->removeBridgePort(value));
    gPortsOrch->doVlanMemberTask(consumer);
    EXPECT_EQ(consumer.m_toSync.size(), 1u); EXPECT_EQ(refs(), 1u);
    bridgeFault.clear(); retry(); gPortsOrch->doVlanMemberTask(consumer);
    EXPECT_TRUE(consumer.m_toSync.empty()); EXPECT_EQ(bridgeRemoves, 1u);
}

TEST_F(BpProtectionObserverTest, PendingVlanDeleteRetriesAfterTheVlanObjectIsGone)
{
    attached(); auto &consumer = vlanDelete(false, false); auto value = port(); bridgeFault = "clear";
    ASSERT_FALSE(gPortsOrch->removeBridgePort(value));
    gPortsOrch->doVlanMemberTask(consumer);
    EXPECT_EQ(consumer.m_toSync.size(), 1u); EXPECT_EQ(refs(), 1u);
    bridgeFault.clear(); retry(); gPortsOrch->doVlanMemberTask(consumer);
    EXPECT_TRUE(consumer.m_toSync.empty()); EXPECT_EQ(bridgeRemoves, 1u);
}

TEST_F(BpProtectionObserverTest, UnrelatedMissingVlanMemberDeleteLeavesTheBridgeActive)
{
    attached(); const auto writes = bridgeWrites.size();
    for (bool parent : {false, true})
    {
        auto &consumer = vlanDelete(parent, false);
        consumer.addToSync(KeyOpFieldsValuesTuple{"Vlan3000:PortChannelMissing", DEL_COMMAND, {}});
        gPortsOrch->doVlanMemberTask(consumer);
        EXPECT_TRUE(consumer.m_toSync.empty()); EXPECT_EQ(bridgeRemoves, 0u);
        EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(refs(), 1u);
    }
}

TEST_F(BpProtectionObserverTest, NonlastVlanDeleteKeepsTheReferencedBridgeActive)
{
    attached(); auto &consumer = vlanDelete(true, true, true); const auto writes = bridgeWrites.size();
    ObserverVlanRemoveGuard restore;
    sai_vlan_api->remove_vlan_member = [](sai_object_id_t) -> sai_status_t { return SAI_STATUS_SUCCESS; };
    gPortsOrch->doVlanMemberTask(consumer);
    EXPECT_TRUE(consumer.m_toSync.empty()); EXPECT_EQ(bridgeRemoves, 0u);
    auto value = port(); EXPECT_EQ(gPortsOrch->getBridgePortReferenceCount(value), 1u);
    EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(refs(), 1u);
}

TEST_F(BpProtectionObserverTest, NewVlanDemandCancelsQueuedBridgeRetirement)
{
    attached(); auto &consumer = vlanDelete(true, false); auto value = port(); bridgeFault = "clear";
    ASSERT_FALSE(gPortsOrch->removeBridgePort(value)); gPortsOrch->doVlanMemberTask(consumer);
    ASSERT_EQ(consumer.m_toSync.size(), 1u);
    ObserverVlanRemoveGuard restore;
    sai_vlan_api->create_vlan_member = [](sai_object_id_t *oid, sai_object_id_t, uint32_t,
                                          const sai_attribute_t *) -> sai_status_t {
        *oid = 0x9002; return SAI_STATUS_SUCCESS;
    };
    const string unrelated = "Vlan0999:PortMissing";
    consumer.addToSync(KeyOpFieldsValuesTuple{unrelated, SET_COMMAND, {{"tagging_mode", "tagged"}}});
    consumer.addToSync(KeyOpFieldsValuesTuple{"Vlan1000:PortChannel0", SET_COMMAND,
                                             {{"tagging_mode", "tagged"}}});
    // Keep the clear failure active until the queued SET cancels retirement.
    gPortsOrch->doVlanMemberTask(consumer);
    EXPECT_EQ(consumer.m_toSync.size(), 1u); EXPECT_EQ(consumer.m_toSync.count(unrelated), 1u);
    EXPECT_EQ(consumer.m_toSync.count("Vlan1000:PortChannel0"), 0u);
    EXPECT_EQ(bridgeRemoves, 0u);
    Port vlan; ASSERT_TRUE(gPortsOrch->getPort("Vlan1000", vlan));
    EXPECT_EQ(vlan.m_members.count("PortChannel0"), 1u);
    EXPECT_EQ(port().m_bridge_port_id, value.m_bridge_port_id);
    EXPECT_EQ(gPortsOrch->getBridgePortReferenceCount(value), 1u);
    bridgeFault.clear(); retry(); // Confirm the unfinished clear first.
    EXPECT_EQ(refs(), 0u);
    retry(); // Reattach only on the following reconciliation pass.
    gPortsOrch->doVlanMemberTask(consumer);
    EXPECT_EQ(gPortsOrch->m_portVlanMember.at("PortChannel0").at(1000).vlan_member_id, 0x9002u);
    EXPECT_EQ(bridgeRemoves, 0u); EXPECT_EQ(refs(), 1u);
    EXPECT_EQ(state().at("reconciliation_state"), "STEADY");
}


TEST_F(BpProtectionObserverTest, NewVlanDemandRetriesRestoreAndCreationWithoutDeletingItsReplacement)
{
    attached(); auto &consumer = vlanDelete(true, false); auto value = port();
    Port member("EthernetObserver", Port::PHY); member.m_hif_id = 999;
    gPortsOrch->setPort(member.m_alias, member); value.m_members.insert(member.m_alias);
    gPortsOrch->setPort(value.m_alias, value); bridgeFault = "clear";
    ASSERT_FALSE(gPortsOrch->removeBridgePort(value));
    ObserverVlanRemoveGuard restore;
    sai_vlan_api->create_vlan_member = [](sai_object_id_t *oid, sai_object_id_t, uint32_t,
                                          const sai_attribute_t *) -> sai_status_t {
        if (++calls["vlan-member-create"] == 1) return SAI_STATUS_TABLE_FULL;
        *oid = 0x9003; return SAI_STATUS_SUCCESS;
    };
    consumer.addToSync(KeyOpFieldsValuesTuple{"Vlan1000:PortChannel0", SET_COMMAND,
                                             {{"tagging_mode", "tagged"}}});
    hostTagFailure = true; gPortsOrch->doVlanMemberTask(consumer);
    EXPECT_EQ(consumer.m_toSync.size(), 2u); EXPECT_EQ(calls["vlan-member-create"], 0u);
    EXPECT_TRUE(gPortsOrch->m_bridgePortRemovalPending.count(value.m_alias));
    hostTagFailure = false; gPortsOrch->doVlanMemberTask(consumer);
    EXPECT_EQ(consumer.m_toSync.size(), 2u); EXPECT_EQ(calls["vlan-member-create"], 1u);
    EXPECT_EQ(gPortsOrch->m_portVlanMember.count(value.m_alias), 0u);
    gPortsOrch->doVlanMemberTask(consumer);
    EXPECT_TRUE(consumer.m_toSync.empty()); EXPECT_EQ(calls["vlan-member-create"], 2u);
    EXPECT_EQ(port().m_bridge_port_id, value.m_bridge_port_id); EXPECT_EQ(bridgeRemoves, 0u);
    EXPECT_EQ(gPortsOrch->m_portVlanMember.at(value.m_alias).at(1000).vlan_member_id, 0x9003u);
    bridgeFault.clear(); retry(); retry(); gPortsOrch->doVlanMemberTask(consumer);
    EXPECT_EQ(calls["vlan-member-create"], 2u); EXPECT_EQ(refs(), 1u);
    EXPECT_EQ(state().at("reconciliation_state"), "STEADY");
}

TEST_F(BpProtectionObserverTest, BindingBeforeGroupAttachesOnRealReadyNotification)
{
    noGroup(); attached(); EXPECT_EQ(runtime().lastReason, "nhg_pending");
    EXPECT_TRUE(bridgeWrites.empty()); group("2147483649", "1073741825"); step();
    EXPECT_EQ(bridgeWrites.size(), 3u); EXPECT_EQ(refs(), 1u);
    EXPECT_EQ(state().at("reconciliation_state"), "STEADY");
}

TEST_F(BpProtectionObserverTest, BindingBeforeBridgePortAttachesOnPortsOrchAdd)
{
    po(SAI_PORT_OPER_STATUS_UP, 0); attached();
    EXPECT_EQ(runtime().lastReason, "bridge_port_pending");
    Port value = port(); ASSERT_TRUE(gPortsOrch->addBridgePort(value));
    EXPECT_GT(value.m_bridge_port_generation, 0u); EXPECT_EQ(refs(), 1u);
    EXPECT_EQ(bridgeWrites.size(), 3u); EXPECT_EQ(state().at("reconciliation_state"), "STEADY");
}

TEST_F(BpProtectionObserverTest, BothDependenciesMissingGroupArrivesLast)
{
    noGroup(); po(SAI_PORT_OPER_STATUS_UP, 0); attached();
    EXPECT_EQ(protection->pendingDependencies("PortChannel0"), "NHG,BRIDGE_PORT");
    Port value = port(); ASSERT_TRUE(gPortsOrch->addBridgePort(value)); EXPECT_TRUE(bridgeWrites.empty());
    group("2147483649", "1073741825"); step(); EXPECT_EQ(bridgeWrites.size(), 3u);
    addEvent(value); step(); EXPECT_EQ(bridgeWrites.size(), 3u);
}

TEST_F(BpProtectionObserverTest, BothDependenciesMissingPortArrivesLast)
{
    noGroup(); po(SAI_PORT_OPER_STATUS_UP, 0); bind(); config();
    group("2147483649", "1073741825"); step(); EXPECT_TRUE(bridgeWrites.empty());
    Port value = port(); ASSERT_TRUE(gPortsOrch->addBridgePort(value)); EXPECT_EQ(bridgeWrites.size(), 3u);
    step(); addEvent(value); EXPECT_EQ(bridgeWrites.size(), 3u);
}

TEST_F(BpProtectionObserverTest, ConfigArrivingLastAndUnconfiguredNotificationsDoNotAllocateWork)
{
    bind(); auto size = protection->m_attachments.size(); addEvent(port());
    L2NhgUpdate event{2147483649u, builder->m_protectionGroups.at("2147483649").oid, L2NhgChange::READY};
    builder->notify(SUBJECT_TYPE_L2_NHG_CHANGE, &event);
    EXPECT_EQ(protection->m_attachments.size(), size); EXPECT_TRUE(bridgeWrites.empty());
    config(); EXPECT_EQ(refs(), 1u); EXPECT_EQ(bridgeWrites.size(), 3u);
}

TEST_F(BpProtectionObserverTest, ActualAggregateUpIsVisibleBeforeNotificationAndStartsRestoreWindow)
{
    po(SAI_PORT_OPER_STATUS_DOWN); attached(); Port value = port();
    EXPECT_EQ(state().at("protection_state"), "PROTECTION"); EXPECT_EQ(wtrStatus(), "IDLE");
    PortListener listener;
    listener.action = [&](SubjectType type, void *) {
        if (type == SUBJECT_TYPE_PORT_OPER_STATE_CHANGE)
        {
            EXPECT_EQ(port().m_oper_status, SAI_PORT_OPER_STATUS_UP);
        }
    };
    gPortsOrch->attach(&listener); gPortsOrch->updatePortOperStatus(value, SAI_PORT_OPER_STATUS_UP);
    gPortsOrch->detach(&listener); EXPECT_EQ(bridgeWrites.size(), 3u); EXPECT_EQ(refs(), 1u);
    EXPECT_EQ(wtrStatus(), "WAIT_TO_RESTORE");
}

TEST_F(BpProtectionObserverTest, ActiveAggregateDownFailsOverButIndividualMemberEventsDoNot)
{
    attached(); auto writes = bridgeWrites.size(); Port value = port();
    gPortsOrch->updatePortOperStatus(value, SAI_PORT_OPER_STATUS_DOWN);
    EXPECT_EQ(bridgeWrites.size(), writes+1); EXPECT_EQ(state().at("protection_state"), "PROTECTION");
    writes = bridgeWrites.size();
    Port member("Ethernet0", Port::PHY); PortOperStateUpdate event{member, SAI_PORT_OPER_STATUS_DOWN};
    protection->update(SUBJECT_TYPE_PORT_OPER_STATE_CHANGE, &event);
    LagMemberUpdate lag{value, member, false}; protection->update(SUBJECT_TYPE_LAG_MEMBER_CHANGE, &lag);
    EXPECT_EQ(bridgeWrites.size(), writes); config(SET_COMMAND, "PROTECTION");
    EXPECT_EQ(state().at("protection_state"), "PROTECTION");
}

TEST_F(BpProtectionObserverTest, StableMembershipChangesAndDuplicateReadyDoNotReattach)
{
    attached(); const auto oid = builder->m_protectionGroups.at("2147483649").oid;
    auto writes = bridgeWrites.size(); member("1073741826", "10.0.0.3");
    group("2147483649", "1073741825,1073741826"); step();
    group("2147483649", "1073741826"); step();
    L2NhgUpdate event{2147483649u, oid, L2NhgChange::READY}; builder->notify(SUBJECT_TYPE_L2_NHG_CHANGE, &event);
    EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(refs(), 1u);
}

TEST_F(BpProtectionObserverTest, WithdrawalClearsAndUnrefsBeforeGroupRemovalInSameTask)
{
    attached(); const auto oid = builder->m_protectionGroups.at("2147483649").oid;
    readHook = [&] { EXPECT_EQ(refs(), 1u); EXPECT_TRUE(groups.count(oid)); };
    builder->deleteProtectionRow("2147483649"); step();
    EXPECT_FALSE(groups.count(oid)); EXPECT_EQ(runtime().lastReason, "nhg_pending");
    EXPECT_EQ(state().at("admin_mode"), "AUTO");
}

TEST_F(BpProtectionObserverTest, FailedWithdrawalCallbackPinsUntilPeriodicAcknowledgement)
{
    attached(); bridgeFault = "clear-get"; builder->deleteProtectionRow("2147483649"); step();
    EXPECT_EQ(refs(), 1u); EXPECT_FALSE(groups.empty()); EXPECT_EQ(runtime().lastReason, "sai_retry");
    bridgeFault.clear(); retry(); step(); EXPECT_TRUE(groups.empty());
}

TEST_F(BpProtectionObserverTest, WithdrawingOldGroupDuringReplacementDoesNotLeakPins)
{
    attached(); group("2147483650", "1073741825"); step();
    bridgeFault = "mode"; bind("2147483650"); EXPECT_EQ(refs(), 1u); EXPECT_EQ(refs("2147483650"), 1u);
    bridgeFault.clear(); builder->deleteProtectionRow("2147483649"); step();
    EXPECT_EQ(refs(), 1u); retry(); step();
    EXPECT_FALSE(builder->m_protectionGroups.count("2147483649"));
    EXPECT_EQ(refs("2147483650"), 1u); EXPECT_EQ(state().at("reconciliation_state"), "STEADY");
}

TEST_F(BpProtectionObserverTest, WithdrawingPartiallyWrittenReplacementClearsAllWrittenGroups)
{
    attached(); group("2147483650", "1073741825"); step(); bridgeFault = "mode"; bind("2147483650");
    bridgeFault.clear(); builder->deleteProtectionRow("2147483650"); step();
    EXPECT_EQ(refs(), 0u); EXPECT_FALSE(builder->m_protectionGroups.count("2147483650"));
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
}

TEST_F(BpProtectionObserverTest, PreRemoveFailureDefersAdminDownFlushAndSaiRemoval)
{
    attached(); Port value = port(); bridgeFault = "clear";
    EXPECT_FALSE(gPortsOrch->removeBridgePort(value)); EXPECT_EQ(refs(), 1u);
    EXPECT_EQ(bridgeRemoves, 0u); EXPECT_EQ(bridgeFlushes, 0u);
    for (const auto &write : bridgeWrites) EXPECT_NE(write.second, SAI_BRIDGE_PORT_ATTR_ADMIN_STATE);
    bridgeFault.clear(); readHook = [&] { EXPECT_EQ(refs(), 1u); EXPECT_EQ(bridgeRemoves, 0u); };
    ASSERT_TRUE(gPortsOrch->removeBridgePort(value)); EXPECT_EQ(refs(), 0u);
    EXPECT_EQ(bridgeRemoves, 1u); EXPECT_EQ(bridgeFlushes, 1u);
    EXPECT_EQ(port().m_bridge_port_id, SAI_NULL_OBJECT_ID);
    EXPECT_EQ(runtime().lastReason, "bridge_port_pending");
}

TEST_F(BpProtectionObserverTest, PeriodicClearWhileRemovalPendingNeverReattachesOldPort)
{
    attached(); Port value = port(); bridgeFault = "clear";
    EXPECT_FALSE(gPortsOrch->removeBridgePort(value)); bridgeFault.clear(); retry();
    EXPECT_EQ(refs(), 0u); EXPECT_EQ(runtime().lastReason, "bridge_port_removing");
    auto writes = bridgeWrites.size(); retry(); EXPECT_EQ(bridgeWrites.size(), writes);
    ASSERT_TRUE(gPortsOrch->removeBridgePort(value)); EXPECT_EQ(refs(), 0u);
}

TEST_F(BpProtectionObserverTest, SaiRemoveFailureCanRetryWithoutReattachingOrCancelOnNewAddDemand)
{
    attached(); Port value = port(); bridgeFault = "remove";
    EXPECT_FALSE(gPortsOrch->removeBridgePort(value)); EXPECT_EQ(refs(), 0u);
    auto writes = bridgeWrites.size(); retry(); EXPECT_EQ(bridgeWrites.size(), writes);
    bridgeFault.clear(); ASSERT_TRUE(gPortsOrch->addBridgePort(value)); EXPECT_EQ(refs(), 1u);
    EXPECT_EQ(bridgeCreates, 0u); EXPECT_EQ(state().at("reconciliation_state"), "STEADY");
    ASSERT_TRUE(gPortsOrch->removeBridgePort(value)); EXPECT_EQ(refs(), 0u);
}

TEST_F(BpProtectionObserverTest, ReusedBridgeOidHasNewGenerationAndRejectsDelayedEvents)
{
    po(SAI_PORT_OPER_STATUS_UP, 0); config(); bind(); Port value = port();
    ASSERT_TRUE(gPortsOrch->addBridgePort(value)); const auto old = value;
    ASSERT_TRUE(gPortsOrch->removeBridgePort(value)); ASSERT_TRUE(gPortsOrch->addBridgePort(value));
    EXPECT_EQ(old.m_bridge_port_id, value.m_bridge_port_id);
    EXPECT_GT(value.m_bridge_port_generation, old.m_bridge_port_generation);
    const auto writes = bridgeWrites.size(); addEvent(old); removedEvent(old);
    EXPECT_TRUE(preRemove(old).ready);
    PortOperStateUpdate oper{old, SAI_PORT_OPER_STATUS_DOWN}; protection->update(SUBJECT_TYPE_PORT_OPER_STATE_CHANGE, &oper);
    EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(refs(), 1u);
    Port stale = old; EXPECT_FALSE(gPortsOrch->removeBridgePort(stale));
    EXPECT_FALSE(gPortsOrch->addBridgePort(stale)); EXPECT_EQ(bridgeRemoves, 1u);
}

TEST_F(BpProtectionObserverTest, UnexpectedReusedOidNeverClearsNewIncarnationWithoutOldRemovalAck)
{
    attached(); Port old = port(); Port current = old; ++current.m_bridge_port_generation;
    gPortsOrch->setPort(current.m_alias, current); addEvent(current);
    EXPECT_EQ(refs(), 1u); EXPECT_EQ(runtime().lastReason, "bridge_port_incarnation_conflict");
    const auto writes = bridgeWrites.size(); EXPECT_FALSE(preRemove(current).ready);
    EXPECT_EQ(bridgeWrites.size(), writes); removedEvent(old);
    EXPECT_EQ(refs(), 1u); EXPECT_EQ(state().at("reconciliation_state"), "STEADY");
}

TEST_F(BpProtectionObserverTest, ConfirmedOldRemovalReleasesWithoutWritingRecreatedDifferentOid)
{
    attached(); Port old = port(); po(SAI_PORT_OPER_STATUS_UP, 0xdef);
    const auto writes = bridgeWrites.size(); removedEvent(old);
    ASSERT_EQ(bridgeWrites.size(), writes + 3); EXPECT_EQ(bridgeWrites[writes].first, 0xdefu);
    EXPECT_EQ(refs(), 1u); EXPECT_EQ(state().at("reconciliation_state"), "STEADY");
}

TEST_F(BpProtectionObserverTest, PreRemoveDrainsOlderDifferentOidBeforeAdoptingCurrentRemovalIdentity)
{
    attached(); Port old = port(); po(SAI_PORT_OPER_STATUS_UP, 0xdef); Port current = port();
    bridgeFault = "clear"; EXPECT_FALSE(preRemove(current).ready); EXPECT_EQ(refs(), 1u);
    bridgeFault.clear(); EXPECT_TRUE(preRemove(current).ready); EXPECT_EQ(refs(), 0u);
    EXPECT_EQ(runtime().port, 0xdefu);
    po(SAI_PORT_OPER_STATUS_UP, 0); removedEvent(current);
    EXPECT_EQ(runtime().lastReason, "bridge_port_pending");
}

TEST_F(BpProtectionObserverTest, OtherObserverVetoIsPreservedAndUnconfiguredPortsHaveNoSideEffects)
{
    attached(); EXPECT_FALSE(preRemove(port(), false).ready); EXPECT_EQ(refs(), 0u);
    Port value("PortChannel99", Port::LAG); value.m_bridge_port_id = 999;
    gPortsOrch->setPort(value.m_alias, value); EXPECT_TRUE(preRemove(value).ready);
    value.m_type = Port::PHY; EXPECT_TRUE(preRemove(value).ready);
    PortUpdate event{value, true}; protection->update(SUBJECT_TYPE_PORT_CHANGE, &event);
    protection->update(SUBJECT_TYPE_BRIDGE_PORT_CHANGE, &event);
}

TEST_F(BpProtectionObserverTest, StaleMissingAndUnrelatedEventsAreIgnored)
{
    attached(); const auto writes = bridgeWrites.size(); protection->update(SUBJECT_TYPE_L2_NHG_CHANGE, nullptr);
    L2NhgUpdate wrong{2147483649u, 999, L2NhgChange::READY}; builder->notify(SUBJECT_TYPE_L2_NHG_CHANGE, &wrong);
    wrong.change = L2NhgChange::REMOVED; builder->notify(SUBJECT_TYPE_L2_NHG_CHANGE, &wrong);
    wrong.nhg_id = 999; wrong.change = L2NhgChange::READY; builder->notify(SUBJECT_TYPE_L2_NHG_CHANGE, &wrong);
    Port missing("PortChannel99", Port::LAG); missing.m_bridge_port_id = 777; addEvent(missing);
    Port stale = port(); stale.m_bridge_port_id = 999; addEvent(stale);
    PortOperStateUpdate oper{port(), SAI_PORT_OPER_STATUS_DOWN}; protection->update(SUBJECT_TYPE_PORT_OPER_STATE_CHANGE, &oper);
    gPortsOrch->m_portList.erase("PortChannel0"); addEvent(stale); preRemove(stale);
    protection->update(SUBJECT_TYPE_PORT_OPER_STATE_CHANGE, &oper); po();
    EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(refs(), 1u);
}

TEST_F(BpProtectionObserverTest, UnknownLagAndPortChangeDeletionDoNotAllocateOrDropBinding)
{
    config(); bind(); const auto accepted = protection->m_bindings.at("PortChannel0");
    Port unknown("PortChannel99", Port::LAG); PortUpdate addition{unknown, true};
    protection->update(SUBJECT_TYPE_PORT_CHANGE, &addition);
    Port old = port(); gPortsOrch->m_portList.erase(old.m_alias);
    PortUpdate removal{old, false}; protection->update(SUBJECT_TYPE_PORT_CHANGE, &removal);
    EXPECT_EQ(refs(), 0u); EXPECT_EQ(protection->m_bindings.at("PortChannel0"), accepted);
    po(); PortUpdate creation{port(), true}; protection->update(SUBJECT_TYPE_PORT_CHANGE, &creation);
    EXPECT_EQ(refs(), 1u);
}

TEST_F(BpProtectionObserverTest, ReentrantPreRemoveVetoesUntilInflightAttachmentReturns)
{
    config(); readHook = [&] { EXPECT_FALSE(preRemove(port()).ready); };
    bind(); EXPECT_EQ(refs(), 1u); EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    retry(); EXPECT_EQ(refs(), 0u);
    EXPECT_EQ(runtime().lastReason, "bridge_port_removing");
}

TEST_F(BpProtectionObserverTest, ConfigDeletionDuringDeferredRemovalDrainsAndBoundsAttachmentWork)
{
    attached(); bridgeFault = "clear"; EXPECT_FALSE(preRemove(port()).ready);
    config(DEL_COMMAND); EXPECT_EQ(protection->m_attachments.size(), 1u);
    bridgeFault.clear(); retry(); EXPECT_TRUE(protection->m_attachments.empty()); EXPECT_TRUE(state().empty());
    auto value = port(); EXPECT_TRUE(preRemove(value).ready); addEvent(value);
    EXPECT_TRUE(protection->m_attachments.empty()); EXPECT_EQ(protection->m_bindings.size(), 1u);
}

TEST_F(BpProtectionObserverTest, ObserverDetachIsIdempotentAndSafeBeforeSubjectDestruction)
{
    EXPECT_TRUE(protection->m_observersAttached);
    protection->detachObservers(); protection->detachObservers();
    EXPECT_FALSE(protection->m_observersAttached);
    attached(); const auto writes = bridgeWrites.size(); addEvent(port());
    EXPECT_TRUE(preRemove(port()).ready); EXPECT_EQ(bridgeWrites.size(), writes);
    protection->update(SUBJECT_TYPE_L2_NHG_CHANGE, nullptr);
    config(DEL_COMMAND); protection->detachObservers();
}

TEST_F(BpProtectionObserverTest, GenerationExhaustionCannotWrapOrCreateAnUnidentifiablePort)
{
    po(SAI_PORT_OPER_STATUS_UP, 0); auto value = port();
    gPortsOrch->m_nextBridgePortGeneration = std::numeric_limits<uint64_t>::max();
    EXPECT_FALSE(gPortsOrch->addBridgePort(value)); EXPECT_EQ(bridgeCreates, 0u);
    EXPECT_EQ(value.m_bridge_port_id, SAI_NULL_OBJECT_ID);
    EXPECT_TRUE(gPortsOrch->removeBridgePort(value));
}

namespace
{
decltype(sai_next_hop_group_api_t::create_next_hop_group) nativeGroupCreate = nullptr;
decltype(sai_next_hop_group_api_t::remove_next_hop_group) nativeGroupRemove = nullptr;
sai_status_t observerNativeGroupCreate(sai_object_id_t *oid, sai_object_id_t sid,
    uint32_t count, const sai_attribute_t *attributes)
{
    auto status = nativeGroupCreate(oid, sid, count, attributes);
    if (status == SAI_STATUS_SUCCESS) groups.insert(*oid);
    return status;
}
sai_status_t observerNativeGroupRemove(sai_object_id_t oid)
{
    auto status = nativeGroupRemove(oid);
    if (status == SAI_STATUS_SUCCESS) groups.erase(oid);
    return status;
}
}

// Production observers/PortsOrch, real native bridge/NHG metadata and libVS.
// Redis and peer/member resources retain the established mock-fixture backend;
// these cases validate bridge association/path and lifecycle, not forwarding.
class BpProtectionNativeObserverTest : public BpProtectionObserverTest
{
protected:
    sai_object_id_t nativeLag = SAI_NULL_OBJECT_ID;
    void PostSetUp() override
    {
        BpProtectionObserverTest::PostSetUp(); noGroup();
        nativeGroupCreate = savedGroup.create_next_hop_group;
        nativeGroupRemove = savedGroup.remove_next_hop_group;
        sai_next_hop_group_api->create_next_hop_group = observerNativeGroupCreate;
        sai_next_hop_group_api->remove_next_hop_group = observerNativeGroupRemove;
        *sai_bridge_api = savedBridge;
        ASSERT_EQ(sai_lag_api->create_lag(&nativeLag, gSwitchId, 0, nullptr), SAI_STATUS_SUCCESS);
        po(SAI_PORT_OPER_STATUS_UP, 0); Port value = port(); value.m_lag_id = nativeLag;
        gPortsOrch->setPort(value.m_alias, value);
    }
    void PreTearDown() override
    {
        config(DEL_COMMAND); Port value = port();
        if (value.m_bridge_port_id)
        {
            EXPECT_TRUE(gPortsOrch->removeBridgePort(value));
        }
        EXPECT_EQ(sai_lag_api->remove_lag(nativeLag), SAI_STATUS_SUCCESS);
        BpProtectionObserverTest::PreTearDown();
    }
    void nativeAdd()
    {
        Port value = port(); ASSERT_TRUE(gPortsOrch->addBridgePort(value));
    }
    void nativeReady()
    { group("2147483649", "1073741825"); step(); }
    void nativeAttached()
    {
        ASSERT_EQ(state().at("reconciliation_state"), "STEADY");
        sai_attribute_t attrs[2]{};
        attrs[0].id = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID;
        attrs[1].id = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_STATE;
        ASSERT_EQ(sai_bridge_api->get_bridge_port_attribute(port().m_bridge_port_id, 2, attrs), SAI_STATUS_SUCCESS);
        EXPECT_EQ(attrs[0].value.oid, builder->m_protectionGroups.at("2147483649").oid);
        EXPECT_EQ(attrs[1].value.s32, SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY);
        EXPECT_EQ(refs(), 1u);
    }
};

TEST_F(BpProtectionNativeObserverTest, NativeBindingBeforeNhg)
{
    nativeAdd(); attached(); EXPECT_EQ(runtime().lastReason, "nhg_pending");
    nativeReady(); nativeAttached();
}

TEST_F(BpProtectionNativeObserverTest, NativeBindingBeforePort)
{
    nativeReady(); attached(); EXPECT_EQ(runtime().lastReason, "bridge_port_pending");
    nativeAdd(); nativeAttached();
}

TEST_F(BpProtectionNativeObserverTest, NativeBothMissingGroupArrivesLastThenWithdraws)
{
    attached(); EXPECT_EQ(protection->pendingDependencies("PortChannel0"), "NHG,BRIDGE_PORT");
    nativeAdd(); nativeReady(); nativeAttached();
    const auto oid = builder->m_protectionGroups.at("2147483649").oid;
    builder->deleteProtectionRow("2147483649"); step(); EXPECT_FALSE(groups.count(oid));
    sai_attribute_t observed{}; observed.id = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID;
    ASSERT_EQ(sai_bridge_api->get_bridge_port_attribute(port().m_bridge_port_id, 1, &observed), SAI_STATUS_SUCCESS);
    EXPECT_EQ(observed.value.oid, SAI_NULL_OBJECT_ID);
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
}

TEST_F(BpProtectionNativeObserverTest, NativeBothMissingPortArrivesLastThenIsRemovedAndRecreated)
{
    attached(); nativeReady(); nativeAdd(); nativeAttached();
    Port value = port(); const auto generation = value.m_bridge_port_generation;
    ASSERT_TRUE(gPortsOrch->removeBridgePort(value)); EXPECT_EQ(refs(), 0u);
    EXPECT_EQ(runtime().lastReason, "bridge_port_pending");
    nativeAdd(); nativeAttached(); EXPECT_GT(port().m_bridge_port_generation, generation);
}

TEST_F(BpProtectionObserverTest, PendingObjectChangesDoNotAddDiagnosticsToHldRow)
{
    noGroup(); po(SAI_PORT_OPER_STATUS_UP, 0); attached();
    EXPECT_EQ(protection->pendingDependencies("PortChannel0"), "NHG,BRIDGE_PORT");
    Table table(m_state_db.get(), STATE_BRIDGE_PORT_PROTECTION_TABLE_NAME);
    const auto unchanged = state();
    addEvent(port()); retry();
    EXPECT_EQ(state(), unchanged);
    group("2147483649", "1073741825"); step();
    EXPECT_EQ(protection->pendingDependencies("PortChannel0"), "BRIDGE_PORT");
    EXPECT_EQ(state(), unchanged);
    po(SAI_PORT_OPER_STATUS_DOWN); addEvent(port());
    EXPECT_EQ(protection->pendingDependencies("PortChannel0"), "");
    EXPECT_EQ(state().at("reconciliation_state"), "STEADY");
    EXPECT_EQ(state().at("protection_state"), "PROTECTION");
    apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND);
    EXPECT_EQ(protection->pendingDependencies("PortChannel0"), "BINDING");
}

TEST_F(BpProtectionObserverTest, ForeignAliasReusingOidCannotBeClearedByOldAttachment)
{
    attached(); Port old = port(); Port foreign("PortChannel99", Port::LAG);
    foreign.m_bridge_port_id = old.m_bridge_port_id; foreign.m_bridge_port_generation = 100;
    gPortsOrch->setPort(foreign.m_alias, foreign);
    gPortsOrch->saiOidToAlias[foreign.m_bridge_port_id] = foreign.m_alias;
    po(SAI_PORT_OPER_STATUS_UP, 0xdef);
    const auto writes = bridgeWrites.size(); retry();
    EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(refs(), 1u);
    EXPECT_EQ(runtime().lastReason, "bridge_port_incarnation_conflict");
    gPortsOrch->saiOidToAlias.erase(foreign.m_bridge_port_id);
}

TEST_F(BpProtectionObserverTest, MissingAliasRejectsOldGeneratedRequestsAndLegacyUnmanagedPortStillRemoves)
{
    Port stale("PortChannel99", Port::LAG); stale.m_bridge_port_id = 999;
    stale.m_bridge_port_generation = 99;
    EXPECT_FALSE(gPortsOrch->addBridgePort(stale)); EXPECT_FALSE(gPortsOrch->removeBridgePort(stale));
    EXPECT_EQ(bridgeRemoves, 0u); stale.m_bridge_port_generation = 0;
    EXPECT_TRUE(gPortsOrch->addBridgePort(stale)); EXPECT_TRUE(gPortsOrch->removeBridgePort(stale));
    EXPECT_EQ(bridgeRemoves, 1u);
}

TEST_F(BpProtectionObserverTest, RuntimeConstructorRejectsMissingSubjectsBeforeRegistering)
{
    for (const auto &subjects : {std::make_pair(static_cast<PortsOrch *>(nullptr), builder.get()),
        std::make_pair(gPortsOrch, static_cast<L2NhgOrch *>(nullptr)),
        std::make_pair(static_cast<PortsOrch *>(nullptr), static_cast<L2NhgOrch *>(nullptr))})
    {
        EXPECT_THROW(BpProtOrch(m_config_db.get(), m_state_db.get(), m_app_db.get(),
            subjects.first, subjects.second), std::invalid_argument);
    }
}

TEST_F(BpProtectionObserverTest, RemovalCancellationRestoresHostTagAndAdminBeforeNotifyingProtection)
{
    attached(); Port value = port(); Port member("EthernetObserver", Port::PHY);
    member.m_hif_id = 999; gPortsOrch->setPort(member.m_alias, member);
    value.m_members.insert(member.m_alias); gPortsOrch->setPort(value.m_alias, value);
    bridgeFault = "remove"; EXPECT_FALSE(gPortsOrch->removeBridgePort(value));
    EXPECT_EQ(hostTags.back(), SAI_HOSTIF_VLAN_TAG_STRIP); EXPECT_EQ(refs(), 0u);
    hostTagFailure = true; EXPECT_FALSE(gPortsOrch->addBridgePort(value));
    EXPECT_EQ(refs(), 0u); EXPECT_TRUE(gPortsOrch->m_bridgePortRemovalPending.count(value.m_alias));
    hostTagFailure = false; bridgeFault = "enable";
    EXPECT_FALSE(gPortsOrch->addBridgePort(value)); EXPECT_EQ(refs(), 0u);
    EXPECT_TRUE(gPortsOrch->m_bridgePortRemovalPending.count(value.m_alias));
    bridgeFault.clear(); ASSERT_TRUE(gPortsOrch->addBridgePort(value));
    EXPECT_EQ(hostTags.back(), SAI_HOSTIF_VLAN_TAG_KEEP); EXPECT_EQ(refs(), 1u);
    EXPECT_TRUE(bridgeValues[value.m_bridge_port_id][SAI_BRIDGE_PORT_ATTR_ADMIN_STATE].value.booldata);
}

TEST_F(BpProtectionObserverTest, PostRemovePublishesCachedAbsenceAndPreservesObserverUpdates)
{
    attached(); Port value = port(); PortListener listener;
    listener.action = [&](SubjectType type, void *context) {
        if (type == SUBJECT_TYPE_BRIDGE_PORT_CHANGE && !static_cast<PortUpdate *>(context)->add)
        {
            Port cached = port(); EXPECT_EQ(cached.m_bridge_port_id, SAI_NULL_OBJECT_ID);
            cached.m_oper_status = SAI_PORT_OPER_STATUS_DOWN;
            gPortsOrch->setPort(cached.m_alias, cached);
        }
    };
    gPortsOrch->attach(&listener); EXPECT_TRUE(gPortsOrch->removeBridgePort(value));
    gPortsOrch->detach(&listener); EXPECT_EQ(port().m_oper_status, SAI_PORT_OPER_STATUS_DOWN);
    EXPECT_EQ(refs(), 0u);
}

class BpProtectionFsmTest : public BpProtectionObserverTest
{
protected:
    uint64_t now = 1000;
    void PostSetUp() override
    {
        BpProtectionObserverTest::PostSetUp(); protection->m_now = [&]{ return now; };
    }
    void policy(const string &admin = "AUTO", bool revertive = true, uint32_t wtr = 5000)
    {
        apply(CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME, SET_COMMAND,
            {{"admin_mode", admin}, {"revertive", revertive ? "true" : "false"},
             {"wait_to_restore_time_ms", to_string(wtr)}});
    }
    void start(const string &admin = "AUTO", bool revertive = true, uint32_t wtr = 5000)
    { policy(admin, revertive, wtr); bind(); }
    void link(bool up)
    {
        Port value = port();
        gPortsOrch->updatePortOperStatus(value, up ? SAI_PORT_OPER_STATUS_UP : SAI_PORT_OPER_STATUS_DOWN);
    }
    void tick(uint64_t ms)
    { now += ms; protection->doTask(); protection->doTask(*protection->m_fsmTimer); }
    void path(const string &expected)
    {
        EXPECT_EQ(state().at("protection_state"), expected);
        EXPECT_EQ(runtime().programmed, expected);
        EXPECT_EQ(state().at("reconciliation_state"), "STEADY"); EXPECT_EQ(refs(), 1u);
    }
};

TEST_F(BpProtectionFsmTest, FailedReadRetriesGetOnlyAndHonorsBothTaskDeadlines)
{
    start("AUTO", false); bridgeFault = "get"; link(false);
    const auto writes = bridgeWrites.size(); const auto reads = bridgeReads;
    tick(99); EXPECT_EQ(bridgeReads, reads); EXPECT_EQ(bridgeWrites.size(), writes);
    tick(1); EXPECT_EQ(bridgeReads, reads+1); EXPECT_EQ(bridgeWrites.size(), writes);
    EXPECT_EQ(runtime().retryAt, now+200); EXPECT_TRUE(runtime().verificationPending);
    bridgeFault.clear(); tick(199); EXPECT_EQ(bridgeReads, reads+1);
    tick(1); path("PROTECTION"); EXPECT_EQ(bridgeReads, reads+2); EXPECT_EQ(bridgeWrites.size(), writes);
    EXPECT_FALSE(runtime().pathPending); EXPECT_EQ(runtime().retryAttempts, 0u);
}
TEST_F(BpProtectionFsmTest, DelayedPathPollsAtBackoffWithoutRepeatingEitherAdminWrite)
{
    start("AUTO", false);
    for (bool backup : {true, false})
    {
        bridgeDelayedState = backup ? SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY : SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION;
        if (backup) link(false); else policy("PRIMARY", false);
        const auto writes = bridgeWrites.size(); auto reads = bridgeReads;
        for (uint64_t delay : {100u, 200u, 400u})
        {
            tick(delay-1); EXPECT_EQ(bridgeReads, reads); tick(1); ++reads;
            EXPECT_EQ(bridgeReads, reads); EXPECT_EQ(bridgeWrites.size(), writes);
            EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
        }
        bridgeDelayedState = -1; tick(800); path(backup ? "PROTECTION" : "PRIMARY");
        EXPECT_EQ(bridgeWrites.size(), writes);
    }
}
TEST_F(BpProtectionFsmTest, ReadFirstCorrectsOnlyTheMismatchedWritableAttribute)
{
    for (const auto attr : {SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID,
        SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_MODE, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE})
    {
        start("AUTO", false); bridgeFault = "get"; link(false); bridgeFault.clear();
        auto &value = bridgeValues[port().m_bridge_port_id][attr];
        if (attr == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID) value.value.oid = 0;
        else if (attr == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_MODE) value.value.s32 = SAI_BRIDGE_PORT_PROTECTION_MODE_HARDWARE;
        else value.value.s32 = SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY;
        const auto writes = bridgeWrites.size(); const auto reads = bridgeReads;
        tick(100); path("PROTECTION"); EXPECT_EQ(bridgeReads, reads+2);
        ASSERT_EQ(bridgeWrites.size(), writes+1); EXPECT_EQ(bridgeWrites.back().second, attr);
        EXPECT_EQ(refs(), 1u); config(DEL_COMMAND); po();
    }
}
TEST_F(BpProtectionFsmTest, FailedAttachmentCorrectionRequeuesProgrammingAndRetainsPins)
{
    start("AUTO", false); bridgeFault = "get"; link(false); bridgeFault.clear();
    auto &values = bridgeValues[port().m_bridge_port_id];
    values[SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID].value.oid = 0;
    values[SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_MODE].value.s32 = SAI_BRIDGE_PORT_PROTECTION_MODE_HARDWARE;
    values[SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE].value.s32 = SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY;
    bridgeFault = "mode"; const auto before = bridgeWrites.size(); tick(100);
    EXPECT_EQ(bridgeWrites.size(), before+2); EXPECT_EQ(refs(), 1u);
    EXPECT_FALSE(runtime().verificationPending); EXPECT_TRUE(runtime().repairAttributes);
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    bridgeFault.clear(); const auto reads = bridgeReads; tick(199);
    EXPECT_EQ(bridgeReads, reads); tick(1); path("PROTECTION"); EXPECT_EQ(bridgeWrites.size(), before+5);
    EXPECT_FALSE(runtime().repairAttributes);
}
TEST_F(BpProtectionFsmTest, AcceptedSamePolicyPollsEarlyWithoutWritingAndDuplicateOperDoesNot)
{
    start("AUTO", false); bridgeFault = "get"; link(false);
    const auto writes = bridgeWrites.size(); auto reads = bridgeReads;
    tick(50); link(false); EXPECT_EQ(bridgeReads, reads); EXPECT_EQ(runtime().retryAt, now+50);
    policy("AUTO", false); EXPECT_EQ(bridgeReads, ++reads); EXPECT_EQ(bridgeWrites.size(), writes);
    EXPECT_EQ(runtime().retryAttempts, 1u); bridgeFault.clear(); tick(100); path("PROTECTION");
    EXPECT_EQ(bridgeWrites.size(), writes);
}
TEST_F(BpProtectionFsmTest, ChangedIntentPreemptsPendingVerificationAndDoesNotReplayOldPath)
{
    start("AUTO", false); bridgeFault = "get"; link(false); bridgeFault.clear();
    const auto writes = bridgeWrites.size(); policy("PRIMARY", false); path("PRIMARY");
    EXPECT_EQ(bridgeWrites.size(), writes+1); EXPECT_FALSE(runtime().pathPending);
    const auto reads = bridgeReads; tick(10000); EXPECT_EQ(bridgeReads, reads);
    EXPECT_EQ(bridgeWrites.size(), writes+1);
}
TEST_F(BpProtectionFsmTest, DeletedConfigOrBindingCancelsQueuedVerification)
{
    for (const string table : {CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME, APP_BP_PROTECTION_BINDING_TABLE_NAME})
    {
        start("AUTO", false); bridgeFault = "get"; link(false);
        apply(table, DEL_COMMAND); EXPECT_EQ(refs(), 0u);
        const auto writes = bridgeWrites.size(); const auto reads = bridgeReads;
        bridgeFault.clear(); tick(10000); EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(bridgeReads, reads);
        config(DEL_COMMAND); po();
    }
}
TEST_F(BpProtectionFsmTest, InitialAndReplacementVerificationReadFirstWithoutReleasingPinsEarly)
{
    bridgeFault = "get"; start("AUTO", false); auto writes = bridgeWrites.size(); auto reads = bridgeReads;
    EXPECT_EQ(writes, 3u); tick(99); EXPECT_EQ(bridgeReads, reads); tick(1);
    EXPECT_EQ(bridgeReads, reads+1); EXPECT_EQ(bridgeWrites.size(), writes);
    bridgeFault.clear(); tick(200); path("PRIMARY"); EXPECT_EQ(bridgeWrites.size(), writes);
    group("2147483650", "1073741825"); step(); bridgeFault = "get"; bind("2147483650");
    writes = bridgeWrites.size(); EXPECT_EQ(refs(), 1u); EXPECT_EQ(refs("2147483650"), 1u);
    tick(100); EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(refs(), 1u);
    bridgeFault.clear(); tick(200); EXPECT_EQ(bridgeWrites.size(), writes);
    EXPECT_EQ(refs(), 0u); EXPECT_EQ(refs("2147483650"), 1u);
    EXPECT_EQ(state().at("reconciliation_state"), "STEADY");
}
TEST_F(BpProtectionFsmTest, InitialWriteFailureBackoffYieldsAndEventuallyRecoversWithoutEvents)
{
    bridgeFault = "group"; start("AUTO", false); auto writes = bridgeWrites.size(); auto reads = bridgeReads;
    for (uint64_t delay : {100u, 200u, 400u, 800u, 1600u, 3200u, 5000u, 5000u})
    {
        tick(delay-1); EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(bridgeReads, reads);
        tick(1); EXPECT_EQ(bridgeWrites.size(), ++writes); EXPECT_EQ(bridgeReads, ++reads);
        EXPECT_EQ(refs(), 1u); EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    }
    bridgeFault.clear(); tick(5000); path("PRIMARY"); EXPECT_EQ(bridgeWrites.size(), writes+3);
}
TEST_F(BpProtectionFsmTest, FailedInterfaceYieldsToIndependentInterfaceTasks)
{
    start("AUTO", false); bridgeFault = "get"; link(false); bridgeFault.clear();
    auto reads = bridgeReads; auto writes = bridgeWrites.size();
    Port other("PortChannel1", Port::LAG); other.m_bridge_port_id = 0xdef; other.m_oper_status = SAI_PORT_OPER_STATUS_UP;
    gPortsOrch->setPort(other.m_alias, other);
    auto row = [&](const string &table, const string &op, const vector<FieldValueTuple> &fields) {
        Table db(table == CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME ? m_config_db.get() : m_app_db.get(), table);
        if (op == DEL_COMMAND) db.del(other.m_alias); else db.set(other.m_alias, fields);
        auto consumer = dynamic_cast<Consumer *>(protection->getConsumerBase(table));
        consumer->addToSync(KeyOpFieldsValuesTuple{other.m_alias, op, fields}); protection->doTask(*consumer);
    };
    row(CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME, SET_COMMAND, {{"admin_mode", "PRIMARY"}});
    row(APP_BP_PROTECTION_BINDING_TABLE_NAME, SET_COMMAND, {{"nexthop_group", "2147483649"}});
    EXPECT_EQ(bridgeWrites.size(), writes+3); EXPECT_EQ(bridgeReads, reads+1); EXPECT_EQ(refs(), 2u);
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    EXPECT_TRUE(protection->m_attachments.at(other.m_alias).active);
    tick(100); EXPECT_EQ(state().at("reconciliation_state"), "STEADY");
    row(CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME, DEL_COMMAND, {});
    row(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND, {});
}
TEST_F(BpProtectionFsmTest, ReadFirstRechecksPolicyBindingAndBridgeBeforeCorrecting)
{
    for (unsigned change = 0; change < 8; ++change)
    {
        start("AUTO", false); bridgeFault = "get"; link(false); bridgeFault.clear();
        const auto old = port(); const auto writes = bridgeWrites.size();
        runtime().lastResync = "2000-01-01T00:00:00Z";
        bridgeValues[old.m_bridge_port_id][SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE].value.s32 = SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY;
        readHook = [&, change] {
            if (change == 0) apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND);
            else if (change == 1) config(DEL_COMMAND);
            else if (change == 2) gPortsOrch->m_portList.erase(old.m_alias);
            else if (change == 3) { auto replacement = old; ++replacement.m_bridge_port_generation; gPortsOrch->setPort(old.m_alias, replacement); }
            else if (change == 4) policy("PRIMARY", false);
            else if (change == 5) policy("AUTO", true);
            else if (change == 6) policy("AUTO", false, 1234);
            else link(true);
        };
        now += 100; protection->doTask(); EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(refs(), 1u);
        EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
        if (change == 2 || change == 3) { EXPECT_EQ(state().at("last_resync_ts"), "2000-01-01T00:00:00Z"); }
        else { EXPECT_NE(state().at("last_resync_ts"), "2000-01-01T00:00:00Z"); }
        gPortsOrch->setPort(old.m_alias, old); config(DEL_COMMAND); bind(); po();
    }
}

TEST_F(BpProtectionFsmTest, CorrectiveWriteRechecksIdentityBeforeAnyFurtherAttributeOrRead)
{
    for (bool deleted : {true, false})
    {
        start("AUTO", false); bridgeFault = "get"; link(false); bridgeFault.clear();
        const auto old = port(); auto &values = bridgeValues[old.m_bridge_port_id];
        values[SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID].value.oid = 0;
        values[SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_MODE].value.s32 = SAI_BRIDGE_PORT_PROTECTION_MODE_HARDWARE;
        values[SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE].value.s32 = SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY;
        const auto writes = bridgeWrites.size(); const auto reads = bridgeReads;
        writeHook = [&] {
            if (deleted) config(DEL_COMMAND);
            else { auto replacement = old; ++replacement.m_bridge_port_generation; gPortsOrch->setPort(old.m_alias, replacement); }
        };
        now += 100; protection->doTask();
        EXPECT_EQ(bridgeWrites.size(), writes+1); EXPECT_EQ(bridgeReads, reads+1);
        EXPECT_EQ(refs(), 1u); EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
        gPortsOrch->setPort(old.m_alias, old); config(DEL_COMMAND); po();
    }
}

TEST_F(BpProtectionFsmTest, TerminalSetStopsBeforeGetAndOrdinaryEventsCannotRearm)
{
    for (sai_status_t status : std::vector<sai_status_t>{SAI_STATUS_NOT_SUPPORTED, SAI_STATUS_NOT_IMPLEMENTED,
        SAI_STATUS_ATTR_NOT_SUPPORTED_0, SAI_STATUS_ATTR_NOT_SUPPORTED_MAX,
        SAI_STATUS_ATTR_NOT_IMPLEMENTED_0, SAI_STATUS_ATTR_NOT_IMPLEMENTED_MAX,
        SAI_STATUS_UNKNOWN_ATTRIBUTE_0, SAI_STATUS_UNKNOWN_ATTRIBUTE_MAX})
    {
        start("AUTO", false); const auto observedAt = state().at("last_resync_ts");
        bridgeStatus = status; bridgeFault = "admin"; const auto reads = bridgeReads;
        link(false); EXPECT_EQ(bridgeReads, reads); EXPECT_TRUE(runtime().blocked);
        EXPECT_EQ(runtime().terminalOperation, "set"); EXPECT_EQ(runtime().terminalStatus, status);
        EXPECT_NE(runtime().terminalAttributes.find("PROTECTION_ADMIN_MODE"), string::npos);
        EXPECT_EQ(state().at("protection_state"), "PRIMARY"); EXPECT_EQ(state().at("last_resync_ts"), observedAt);
        const auto writes = bridgeWrites.size(); bridgeFault.clear(); tick(100000);
        link(true); link(false); bind(); addEvent(port());
        const auto oid = builder->m_protectionGroups.at("2147483649").oid;
        L2NhgUpdate ready{2147483649u, oid, L2NhgChange::READY}; builder->notify(SUBJECT_TYPE_L2_NHG_CHANGE, &ready);
        apply(CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME, SET_COMMAND, {{"admin_mode", "invalid"}});
        EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(bridgeReads, reads); EXPECT_EQ(refs(), 1u);
        EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
        policy("AUTO", false); path("PROTECTION"); EXPECT_FALSE(runtime().blocked);
        config(DEL_COMMAND); apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND); po();
    }
}
TEST_F(BpProtectionFsmTest, TerminalGetStopsPollingUntilExplicitConfigReapplication)
{
    for (sai_status_t status : std::vector<sai_status_t>{SAI_STATUS_NOT_SUPPORTED, SAI_STATUS_NOT_IMPLEMENTED,
        SAI_STATUS_ATTR_NOT_SUPPORTED_0, SAI_STATUS_ATTR_NOT_SUPPORTED_MAX,
        SAI_STATUS_ATTR_NOT_IMPLEMENTED_0, SAI_STATUS_ATTR_NOT_IMPLEMENTED_MAX,
        SAI_STATUS_UNKNOWN_ATTRIBUTE_0, SAI_STATUS_UNKNOWN_ATTRIBUTE_MAX})
    {
        start("AUTO", false); const auto observedAt = state().at("last_resync_ts");
        bridgeStatus = status; bridgeFault = "get"; link(false);
        EXPECT_EQ(runtime().terminalOperation, "get"); EXPECT_EQ(runtime().terminalStatus, status);
        EXPECT_NE(runtime().terminalAttributes.find("PROTECTION_STATE"), string::npos);
        EXPECT_EQ(state().at("protection_state"), "PRIMARY"); EXPECT_EQ(state().at("last_resync_ts"), observedAt);
        const auto writes = bridgeWrites.size(); const auto reads = bridgeReads;
        bridgeFault.clear(); link(true); bind(); tick(100000);
        EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(bridgeReads, reads); EXPECT_TRUE(runtime().blocked);
        policy("AUTO", false); path("PROTECTION");
        EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(bridgeReads, reads+1);
        config(DEL_COMMAND); apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND); po();
    }
}
TEST_F(BpProtectionFsmTest, TerminalPrimaryRestoreCancelsWtrAndPortFlapsDoNotRestartIt)
{
    start("AUTO", true, 50); link(false); link(true); EXPECT_TRUE(runtime().wtrRunning);
    bridgeStatus = SAI_STATUS_NOT_IMPLEMENTED; bridgeFault = "admin"; tick(50);
    EXPECT_TRUE(runtime().blocked); EXPECT_FALSE(runtime().wtrRunning); EXPECT_EQ(runtime().wtrDeadline, 0u);
    EXPECT_EQ(state().at("protection_state"), "PROTECTION"); const auto writes = bridgeWrites.size(); const auto reads = bridgeReads;
    bridgeFault.clear(); link(false); link(true); tick(100000);
    EXPECT_FALSE(runtime().wtrRunning); EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(bridgeReads, reads);
    policy("PRIMARY", false); path("PRIMARY"); EXPECT_FALSE(runtime().blocked);
}
TEST_F(BpProtectionFsmTest, InitialTerminalFailureDrainsOnceAndRequiresConfigToAttachAgain)
{
    for (const string stage : {"group", "mode", "admin", "get"})
    {
        bridgeStatus = SAI_STATUS_NOT_IMPLEMENTED; bridgeFault = stage; start("AUTO", false);
        EXPECT_TRUE(runtime().blocked); EXPECT_EQ(refs(), 1u); tick(1); EXPECT_EQ(refs(), 0u);
        const auto writes = bridgeWrites.size(); const auto reads = bridgeReads;
        bridgeFault.clear(); bind(); link(false); addEvent(port()); tick(100000);
        EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(bridgeReads, reads);
        EXPECT_EQ(state().at("admin_mode"), "AUTO"); EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
        policy("AUTO", false); path("PROTECTION");
        config(DEL_COMMAND); apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND); po();
    }
}
TEST_F(BpProtectionFsmTest, TerminalCleanupStopsAutomaticCallsAndPreservesReferencesUntilReapplied)
{
    for (const string stage : {"clear", "clear-get"})
    {
        start("AUTO", false); bridgeStatus = SAI_STATUS_NOT_SUPPORTED; bridgeFault = stage;
        apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND);
        EXPECT_TRUE(runtime().blocked); EXPECT_TRUE(runtime().cleanupBlocked); EXPECT_TRUE(runtime().cleanupPending);
        EXPECT_EQ(refs(), 1u); EXPECT_EQ(state().at("protection_state"), "PRIMARY");
        const auto writes = bridgeWrites.size(); const auto reads = bridgeReads;
        bridgeFault.clear(); bind(); link(false); addEvent(port()); tick(100000);
        EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(bridgeReads, reads); EXPECT_EQ(refs(), 1u);
        policy("AUTO", false); EXPECT_EQ(refs(), 0u); EXPECT_FALSE(runtime().cleanupBlocked);
        tick(1); path("PROTECTION");
        config(DEL_COMMAND); apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND); po();
    }
}
TEST_F(BpProtectionFsmTest, ExplicitConfigDeletionCanRetryTerminalCleanupWithoutRearmingProgramming)
{
    start("AUTO", false); bridgeStatus = SAI_STATUS_NOT_IMPLEMENTED; bridgeFault = "clear-get";
    apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND); EXPECT_EQ(refs(), 1u);
    bridgeFault.clear(); tick(100000); EXPECT_EQ(refs(), 1u);
    config(DEL_COMMAND); EXPECT_EQ(refs(), 0u); EXPECT_TRUE(state().empty());
    const auto writes = bridgeWrites.size(); const auto reads = bridgeReads;
    bind(); tick(100000); EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(bridgeReads, reads);
}
TEST_F(BpProtectionFsmTest, RemovalProofReleasesPinsButDoesNotClearTerminalFeatureFailure)
{
    start("AUTO", false); const auto old = port(); bridgeStatus = SAI_STATUS_NOT_SUPPORTED; bridgeFault = "clear-get";
    EXPECT_FALSE(preRemove(old).ready); EXPECT_TRUE(runtime().cleanupBlocked); EXPECT_EQ(refs(), 1u);
    const auto writes = bridgeWrites.size(); const auto reads = bridgeReads;
    bridgeFault.clear(); addEvent(old); tick(100000);
    EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(bridgeReads, reads); EXPECT_EQ(refs(), 1u);
    removedEvent(old); EXPECT_EQ(refs(), 0u); EXPECT_TRUE(runtime().blocked);
    po(SAI_PORT_OPER_STATUS_UP, 0xdef); addEvent(port()); bind(); tick(100000);
    EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(bridgeReads, reads); EXPECT_FALSE(state().count("protection_state"));
    policy("AUTO", false); path("PRIMARY"); EXPECT_FALSE(runtime().blocked);
}
TEST_F(BpProtectionFsmTest, WithdrawingHeldGroupDrainsTerminalPathEvenWithAnotherReadyBinding)
{
    start("AUTO", false); bridgeStatus = SAI_STATUS_NOT_SUPPORTED; bridgeFault = "admin"; link(false); bridgeFault.clear();
    group("2147483650", "1073741825"); step(); bind("2147483650"); EXPECT_EQ(refs(), 1u); EXPECT_EQ(refs("2147483650"), 0u);
    builder->deleteProtectionRow("2147483649"); step();
    EXPECT_FALSE(builder->m_protectionGroups.count("2147483649")); EXPECT_EQ(refs("2147483650"), 0u);
    EXPECT_TRUE(runtime().blocked); EXPECT_FALSE(state().count("protection_state"));
    policy("AUTO", false); EXPECT_EQ(refs("2147483650"), 1u);
    EXPECT_EQ(state().at("protection_state"), "PROTECTION"); EXPECT_EQ(state().at("reconciliation_state"), "STEADY");
}
TEST_F(BpProtectionFsmTest, TerminalReplacementAndCleanupRetainBothPinsUntilExplicitRecovery)
{
    start("AUTO", false); group("2147483650", "1073741825"); group("2147483651", "1073741825"); step();
    bridgeStatus = SAI_STATUS_NOT_IMPLEMENTED; bridgeFault = "mode"; bind("2147483650");
    EXPECT_TRUE(runtime().blocked); EXPECT_EQ(refs(), 1u); EXPECT_EQ(refs("2147483650"), 1u);
    bridgeFault = "clear-get"; tick(1); EXPECT_TRUE(runtime().cleanupBlocked);
    EXPECT_EQ(refs(), 1u); EXPECT_EQ(refs("2147483650"), 1u);
    const auto writes = bridgeWrites.size(); const auto reads = bridgeReads;
    bridgeFault.clear(); bind("2147483651"); link(false); tick(100000);
    EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(bridgeReads, reads);
    EXPECT_EQ(refs(), 1u); EXPECT_EQ(refs("2147483650"), 1u); EXPECT_EQ(refs("2147483651"), 0u);
    policy("AUTO", false); EXPECT_EQ(refs(), 0u); EXPECT_EQ(refs("2147483650"), 0u);
    tick(1); EXPECT_EQ(refs("2147483651"), 1u); EXPECT_EQ(state().at("protection_state"), "PROTECTION");
    EXPECT_EQ(state().at("reconciliation_state"), "STEADY");
}

TEST_F(BpProtectionFsmTest, TerminalErrorLogIdentifiesInterfaceOperationAttributesAndStatus)
{
    start("AUTO", false); BpProtLog::setOutput("bpProtOrch", "STDOUT"); BpProtLog::setPriority("bpProtOrch", "ERROR");
    bridgeStatus = SAI_STATUS_NOT_IMPLEMENTED; bridgeFault = "admin";
    testing::internal::CaptureStdout(); link(false); const auto text = testing::internal::GetCapturedStdout();
    BpProtLog::setOutput("bpProtOrch", "SYSLOG"); BpProtLog::setPriority("bpProtOrch", "NOTICE");
    for (const string expected : {"event=sai_terminal_failure", "ifname=PortChannel0", "operation=set", "stage=programming",
        "SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE", "status_class=NOT_IMPLEMENTED", "reapply_required=true"})
        EXPECT_NE(text.find(expected), string::npos);
    EXPECT_NE(text.find("status="+to_string(SAI_STATUS_NOT_IMPLEMENTED)), string::npos);
    bridgeFault.clear(); policy("AUTO", false); path("PROTECTION");
}
TEST_F(BpProtectionFsmTest, TerminalFailureOnOneInterfaceDoesNotStopIndependentTasks)
{
    start("AUTO", false); bridgeStatus = SAI_STATUS_NOT_SUPPORTED; bridgeFault = "admin"; link(false); bridgeFault.clear();
    const auto writes = bridgeWrites.size(); const auto reads = bridgeReads;
    Port other("PortChannel1", Port::LAG); other.m_bridge_port_id = 0xdef; other.m_oper_status = SAI_PORT_OPER_STATUS_UP;
    gPortsOrch->setPort(other.m_alias, other);
    auto row = [&](const string &table, const string &op, const vector<FieldValueTuple> &fields) {
        Table db(table == CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME ? m_config_db.get() : m_app_db.get(), table);
        if (op == DEL_COMMAND) db.del(other.m_alias); else db.set(other.m_alias, fields);
        auto consumer = dynamic_cast<Consumer *>(protection->getConsumerBase(table));
        consumer->addToSync(KeyOpFieldsValuesTuple{other.m_alias, op, fields}); protection->doTask(*consumer);
    };
    row(CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME, SET_COMMAND, {{"admin_mode", "PRIMARY"}});
    row(APP_BP_PROTECTION_BINDING_TABLE_NAME, SET_COMMAND, {{"nexthop_group", "2147483649"}});
    EXPECT_EQ(bridgeWrites.size(), writes+3); EXPECT_EQ(bridgeReads, reads+1);
    EXPECT_TRUE(runtime().blocked); EXPECT_TRUE(protection->m_attachments.at(other.m_alias).active); EXPECT_EQ(refs(), 2u);
    row(CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME, DEL_COMMAND, {}); row(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND, {});
}

TEST_F(BpProtectionFsmTest, AggregateDownSwitchesOnlyAdminAndConfirmsHardware)
{
    start(); auto writes = bridgeWrites.size(); link(false); path("PROTECTION");
    ASSERT_EQ(bridgeWrites.size(), writes+1); EXPECT_EQ(bridgeWrites.back().second, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE);
    EXPECT_EQ(state().at("admin_mode"), "AUTO"); EXPECT_EQ(desired(), "PROTECTION");
    EXPECT_EQ(wtrStatus(), "IDLE");
}
TEST_F(BpProtectionFsmTest, WtrStartsOnUpDespiteRejectedWriteFailedReadOrDelayedBackup)
{
    for (const string fault : {"admin", "get", "delayed"})
    {
        start("AUTO", true, 50);
        bridgeFault = fault == "delayed" ? "" : fault;
        bridgeDelayedState = fault == "delayed" ? SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY : -1;
        link(false); EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
        EXPECT_EQ(desired(), "PROTECTION"); EXPECT_EQ(state().at("protection_state"), "PRIMARY");
        now += 20; link(true); const auto due = now+50;
        EXPECT_EQ(wtrStatus(), "WAIT_TO_RESTORE"); EXPECT_EQ(runtime().wtrDeadline, due);
        tick(49); EXPECT_EQ(desired(), "PROTECTION"); EXPECT_EQ(runtime().wtrDeadline, due);
        tick(1); EXPECT_EQ(desired(), "PRIMARY"); EXPECT_EQ(wtrStatus(), "IDLE");
        EXPECT_TRUE(runtime().wtrCompleted); EXPECT_EQ(refs(), 1u);
        if (fault != "delayed") { EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT"); }
        bridgeFault.clear(); bridgeDelayedState = -1; tick(100); path("PRIMARY");
        config(DEL_COMMAND); apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND); po();
    }
}
TEST_F(BpProtectionFsmTest, PrimaryArmsOneUpWindowAndExpiryNeverRepeatsConfirmedProgramming)
{
    start("AUTO", true, 50); path("PRIMARY");
    ASSERT_EQ(wtrStatus(), "WAIT_TO_RESTORE"); const auto writes = bridgeWrites.size(); const auto reads = bridgeReads;
    tick(50); EXPECT_EQ(wtrStatus(), "IDLE"); EXPECT_TRUE(runtime().wtrCompleted);
    tick(10000); link(true); bind(); policy("AUTO", true, 50);
    EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(bridgeReads, reads);
    EXPECT_EQ(wtrStatus(), "IDLE"); EXPECT_EQ(runtime().wtrDeadline, 0u);
    link(false); link(true); EXPECT_EQ(runtime().wtrDeadline, now+50);
    tick(50); path("PRIMARY"); EXPECT_EQ(wtrStatus(), "IDLE");
}
TEST_F(BpProtectionFsmTest, WtrEligibilityUsesOnlyUpAutoAndRevertiveWithoutAPathObservation)
{
    for (const string admin : {"AUTO", "PRIMARY", "PROTECTION"})
    for (bool revertive : {false, true})
    for (bool up : {false, true})
    {
        po(up ? SAI_PORT_OPER_STATUS_UP : SAI_PORT_OPER_STATUS_DOWN);
        const auto writes = bridgeWrites.size(); const auto reads = bridgeReads;
        policy(admin, revertive, 50);
        EXPECT_FALSE(runtime().active); EXPECT_FALSE(state().count("protection_state"));
        const bool eligible = admin == "AUTO" && revertive && up;
        EXPECT_EQ(runtime().wtrRunning, eligible); EXPECT_EQ(runtime().wtrEligible, eligible);
        EXPECT_EQ(runtime().wtrDeadline, eligible ? now+50 : 0u);
        tick(50); EXPECT_FALSE(runtime().wtrRunning); EXPECT_EQ(runtime().wtrCompleted, eligible);
        EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(bridgeReads, reads);
        config(DEL_COMMAND);
    }
}
TEST_F(BpProtectionFsmTest, InitialFailedReadAndLateBindingDoNotMoveThePolicyDeadline)
{
    policy("AUTO", true, 100); ASSERT_EQ(runtime().wtrDeadline, now+100);
    const auto due = runtime().wtrDeadline; tick(40); EXPECT_EQ(runtime().wtrDeadline, due);
    bridgeFault = "get"; bind(); EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    EXPECT_FALSE(runtime().active); EXPECT_EQ(runtime().observed, "UNKNOWN"); EXPECT_EQ(runtime().wtrDeadline, due);
    tick(60); EXPECT_EQ(wtrStatus(), "IDLE"); EXPECT_TRUE(runtime().wtrCompleted); EXPECT_EQ(refs(), 1u);
    EXPECT_FALSE(state().count("protection_state"));
    bridgeFault.clear(); tick(100); path("PRIMARY"); EXPECT_TRUE(runtime().wtrCompleted);
    EXPECT_EQ(runtime().wtrDeadline, 0u); tick(10000); EXPECT_EQ(wtrStatus(), "IDLE");
}
TEST_F(BpProtectionFsmTest, MissingBridgePortAndGroupCannotGateOrRestartTheUpWindow)
{
    noGroup(); po(SAI_PORT_OPER_STATUS_UP, 0); policy("AUTO", true, 50); bind();
    ASSERT_EQ(wtrStatus(), "WAIT_TO_RESTORE"); const auto due = now+50;
    const auto writes = bridgeWrites.size(); const auto reads = bridgeReads;
    tick(49); EXPECT_EQ(runtime().wtrDeadline, due); tick(1);
    EXPECT_TRUE(runtime().wtrCompleted); EXPECT_EQ(wtrStatus(), "IDLE");
    EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(bridgeReads, reads);
    group("2147483649", "1073741825"); step(); EXPECT_TRUE(runtime().wtrCompleted);
    po(); addEvent(port()); path("PRIMARY"); EXPECT_EQ(wtrStatus(), "IDLE");
    EXPECT_EQ(runtime().wtrDeadline, 0u);
}
TEST_F(BpProtectionFsmTest, WtrTimerPreemptsBackupVerificationBeforeItsRetryDeadline)
{
    start("AUTO", true, 50); bridgeFault = "get"; link(false); link(true);
    ASSERT_EQ(runtime().retryAt, now+100); ASSERT_EQ(runtime().wtrDeadline, now+50);
    const auto writes = bridgeWrites.size(); const auto reads = bridgeReads;
    now += 49; protection->doTask(*protection->m_fsmTimer);
    EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(bridgeReads, reads);
    ++now; protection->doTask(*protection->m_fsmTimer);
    EXPECT_EQ(desired(), "PRIMARY"); EXPECT_EQ(bridgeWrites.size(), writes+1);
    EXPECT_EQ(bridgeReads, reads+1); EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    EXPECT_EQ(wtrStatus(), "IDLE"); EXPECT_TRUE(runtime().wtrCompleted);
    bridgeFault.clear(); tick(100); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, ExpiredWindowStaysCompletedThroughPrimaryWriteReadAndPathFailures)
{
    for (const string fault : {"admin", "get", "delayed"})
    {
        start("AUTO", true, 50); link(false); link(true);
        bridgeFault = fault == "delayed" ? "" : fault;
        bridgeDelayedState = fault == "delayed" ? SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION : -1;
        tick(50); EXPECT_EQ(desired(), "PRIMARY"); EXPECT_EQ(wtrStatus(), "IDLE");
        EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
        for (uint64_t delay : {100u, 200u, 400u})
        {
            tick(delay); EXPECT_EQ(desired(), "PRIMARY"); EXPECT_TRUE(runtime().wtrCompleted);
            EXPECT_EQ(runtime().wtrDeadline, 0u); EXPECT_EQ(wtrStatus(), "IDLE");
        }
        policy("AUTO", true, 50); EXPECT_EQ(wtrStatus(), "IDLE");
        bridgeFault.clear(); bridgeDelayedState = -1; tick(100); path("PRIMARY");
        config(DEL_COMMAND); apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND); po();
    }
}
TEST_F(BpProtectionFsmTest, DurationEditsRestartFromTheEditEvenWhileReadbackRemainsFailed)
{
    start("AUTO", true, 100); bridgeFault = "get"; link(false); link(true);
    tick(40); policy("AUTO", true, 200); const auto due = now+200;
    EXPECT_EQ(runtime().wtrDeadline, due); tick(30); policy("AUTO", true, 200); link(true); bind();
    EXPECT_EQ(runtime().wtrDeadline, due); tick(169); EXPECT_EQ(desired(), "PROTECTION");
    tick(1); EXPECT_EQ(desired(), "PRIMARY"); EXPECT_EQ(wtrStatus(), "IDLE");
    bridgeFault.clear(); tick(100); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, FailedReplacementRestartsOnceAndRetriesCannotExtendTheWindow)
{
    start("AUTO", true, 100); link(false); link(true); tick(40);
    group("2147483650", "1073741825"); step(); bridgeFault = "mode"; bind("2147483650");
    ASSERT_EQ(runtime().wtrDeadline, now+100); const auto due = runtime().wtrDeadline;
    EXPECT_EQ(refs(), 1u); EXPECT_EQ(refs("2147483650"), 1u);
    tick(30); EXPECT_EQ(runtime().wtrDeadline, due); tick(70);
    EXPECT_EQ(desired(), "PRIMARY"); EXPECT_EQ(wtrStatus(), "IDLE"); EXPECT_TRUE(runtime().wtrCompleted);
    EXPECT_EQ(refs(), 1u); EXPECT_EQ(refs("2147483650"), 1u);
    bridgeFault.clear(); tick(100);
    EXPECT_EQ(state().at("protection_state"), "PRIMARY"); EXPECT_EQ(state().at("reconciliation_state"), "STEADY");
    EXPECT_EQ(refs(), 0u); EXPECT_EQ(refs("2147483650"), 1u); EXPECT_EQ(wtrStatus(), "IDLE");
}
TEST_F(BpProtectionFsmTest, DownCancelsUnconfirmedWindowAndNextUpStartsTheFullDuration)
{
    start("AUTO", true, 50); bridgeFault = "get"; link(false); link(true); tick(49);
    link(false); EXPECT_EQ(wtrStatus(), "IDLE"); EXPECT_FALSE(runtime().wtrCompleted);
    tick(100); link(true); EXPECT_EQ(runtime().wtrDeadline, now+50);
    tick(49); EXPECT_EQ(desired(), "PROTECTION"); tick(1); EXPECT_EQ(desired(), "PRIMARY");
    bridgeFault.clear(); tick(100); path("PRIMARY");
}

TEST_F(BpProtectionFsmTest, InitialDownAutoRecoveryHonorsRevertiveAndWtrBoundary)
{
    for (bool revertive : {false, true})
    for (uint32_t wtr : {0u, 5000u})
    {
        link(false); start("AUTO", revertive, wtr); path("PROTECTION");
        EXPECT_EQ(desired(), "PROTECTION");
        EXPECT_EQ(wtrStatus(), "IDLE");
        auto writes = bridgeWrites.size(); tick(10000);
        EXPECT_EQ(bridgeWrites.size(), writes); path("PROTECTION");
        link(true);
        if (!revertive)
        {
            tick(10000); path("PROTECTION");
            EXPECT_EQ(wtrStatus(), "IDLE");
        }
        else if (wtr == 0) path("PRIMARY");
        else
        {
            EXPECT_EQ(wtrStatus(), "WAIT_TO_RESTORE");
            EXPECT_EQ(runtime().wtrDeadline, now+wtr);
            tick(wtr-1); path("PROTECTION"); tick(1); path("PRIMARY");
        }
        config(DEL_COMMAND); EXPECT_EQ(refs(), 0u);
    }
}
TEST_F(BpProtectionFsmTest, DownReplayHonorsAllAdminModesWithoutWaitingForUp)
{
    for (const string admin : {"AUTO", "PRIMARY", "PROTECTION"})
    for (bool revertive : {false, true})
    {
        link(false); start(admin, revertive);
        const string expected = admin == "PRIMARY" ? "PRIMARY" : "PROTECTION";
        path(expected); apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND);
        EXPECT_EQ(refs(), 0u); EXPECT_EQ(state().at("admin_mode"), admin);
        EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
        EXPECT_EQ(runtime().lastReason, "binding_pending");
        bind(); path(expected); EXPECT_EQ(wtrStatus(), "IDLE");
        auto writes = bridgeWrites.size(); bind(); link(false); tick(10000);
        EXPECT_EQ(bridgeWrites.size(), writes); path(expected);
        config(DEL_COMMAND); EXPECT_EQ(refs(), 0u);
    }
}
TEST_F(BpProtectionFsmTest, DownProvisioningWaitsOnlyForMissingObjects)
{
    for (const string last : {"config", "binding", "group", "member", "bridge"})
    {
        config(DEL_COMMAND); apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND);
        builder->deleteProtectionRow("2147483649"); builder->deleteProtectionRow("1073741825");
        step(); step(); ASSERT_TRUE(groups.empty());
        po(SAI_PORT_OPER_STATUS_DOWN, last == "bridge" ? 0 : 0xabc);
        auto writes = bridgeWrites.size();
        if (last != "config") policy();
        if (last != "binding") bind();
        if (last != "member") member("1073741825");
        if (last != "group") group("2147483649", "1073741825");
        step(); EXPECT_EQ(bridgeWrites.size(), writes);
        if (last == "config") EXPECT_TRUE(state().empty());
        else
        {
            EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
            EXPECT_EQ(state().count("protection_state"), 0u);
        }
        if (last == "config") policy();
        else if (last == "binding") bind();
        else if (last == "member") member("1073741825");
        else if (last == "group") group("2147483649", "1073741825");
        else { po(SAI_PORT_OPER_STATUS_DOWN); addEvent(port()); }
        step(); path("PROTECTION"); EXPECT_EQ(port().m_oper_status, SAI_PORT_OPER_STATUS_DOWN);
        EXPECT_EQ(wtrStatus(), "IDLE");
    }
}
TEST_F(BpProtectionFsmTest, InitialDownAttachFailuresRetryWithoutPortUp)
{
    for (const string failure : {"group", "mode", "admin", "get"})
    {
        link(false); bridgeFault = failure; start();
        EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
        EXPECT_EQ(refs(), 1u);
        if (failure == "group" || failure == "get") { EXPECT_EQ(state().count("protection_state"), 0u); }
        EXPECT_EQ(desired(), "PROTECTION");
        bridgeFault.clear(); tick(100); path("PROTECTION");
        EXPECT_EQ(port().m_oper_status, SAI_PORT_OPER_STATUS_DOWN);
        EXPECT_EQ(wtrStatus(), "IDLE"); config(DEL_COMMAND);
    }
}
TEST_F(BpProtectionFsmTest, NonRevertiveUpKeepsBackupUntilManualPrimary)
{
    start("AUTO", false); link(false); link(true); tick(100000); path("PROTECTION");
    policy("PRIMARY", false); path("PRIMARY"); policy("AUTO", false); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, ContinuousUpWindowRevertsAtExactDeadline)
{
    start(); link(false); link(true); EXPECT_EQ(wtrStatus(), "WAIT_TO_RESTORE");
    tick(4999); path("PROTECTION"); tick(1); path("PRIMARY"); EXPECT_EQ(wtrStatus(), "IDLE");
}
TEST_F(BpProtectionFsmTest, FlapsCancelAndRestartFullWindow)
{
    start(); link(false); link(true); tick(4000); link(false); EXPECT_EQ(wtrStatus(), "IDLE");
    tick(2000); link(true); tick(4999); path("PROTECTION"); tick(1); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, DuplicateUpSetAndBindingDoNotRestartOrRewrite)
{
    start(); link(false); link(true); tick(1000); auto writes = bridgeWrites.size();
    auto due = runtime().wtrDeadline; link(true); policy(); bind();
    EXPECT_EQ(runtime().wtrDeadline, due); EXPECT_EQ(bridgeWrites.size(), writes);
    tick(4000); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, SelectingAutoWhileAlreadyUpStartsRestoreWindow)
{
    start("PROTECTION"); tick(4000); policy(); tick(4999); path("PROTECTION"); tick(1); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, PinnedPrimaryTracksDownBeforeAutoSelection)
{
    start("PRIMARY"); auto writes = bridgeWrites.size(); link(false); path("PRIMARY");
    EXPECT_EQ(bridgeWrites.size(), writes); policy(); path("PROTECTION");
}
TEST_F(BpProtectionFsmTest, PinnedProtectionNeverRevertsOnLinkEvents)
{
    start("PROTECTION"); auto writes = bridgeWrites.size(); link(false); link(true); tick(100000);
    path("PROTECTION"); EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(wtrStatus(), "IDLE");
}
TEST_F(BpProtectionFsmTest, DisablingRevertiveCancelsAndEnablingStartsFresh)
{
    start(); link(false); link(true); tick(4000); policy("AUTO", false); tick(100000);
    path("PROTECTION"); EXPECT_EQ(wtrStatus(), "IDLE");
    policy(); tick(4999); path("PROTECTION"); tick(1); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, DurationEditsRestartFullNewWindow)
{
    start(); link(false); link(true); tick(4000); policy("AUTO", true, 10000);
    tick(9999); path("PROTECTION"); policy("AUTO", true, 2000);
    tick(1999); path("PROTECTION"); tick(1); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, ZeroWtrRestoresImmediatelyAndNeverUsesSaiAuto)
{
    start("AUTO", true, 0); link(false); path("PROTECTION"); link(true); path("PRIMARY");
    for (const auto &write : bridgeWrites) EXPECT_NE(write.second, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_REVERTIVE);
    EXPECT_EQ(bridgeValues[port().m_bridge_port_id][SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE].value.s32,
        SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY);
}
TEST_F(BpProtectionFsmTest, MaximumUint32DurationUsesWideDeadline)
{
    now = uint64_t(std::numeric_limits<uint32_t>::max()) + 123;
    start("AUTO", true, std::numeric_limits<uint32_t>::max()); link(false); link(true);
    EXPECT_EQ(runtime().wtrDeadline, now + uint64_t(std::numeric_limits<uint32_t>::max()));
    tick(uint64_t(std::numeric_limits<uint32_t>::max())-1); path("PROTECTION"); tick(1); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, PinnedModeDuringCountdownCancelsTimer)
{
    start(); link(false); link(true); tick(1000); policy("PROTECTION"); tick(10000); path("PROTECTION");
    EXPECT_EQ(wtrStatus(), "IDLE"); policy("PRIMARY"); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, DownAtExpiryWinsWhenAuthoritativeCacheIsAlreadyDown)
{
    start(); link(false); link(true); now += 5000; po(SAI_PORT_OPER_STATUS_DOWN);
    protection->doTask(*protection->m_fsmTimer); path("PROTECTION"); EXPECT_EQ(wtrStatus(), "IDLE");
}
TEST_F(BpProtectionFsmTest, WriteFailureReportsObservedPrimaryAndRetainsPin)
{
    start(); bridgeFault = "admin"; link(false);
    EXPECT_EQ(state().at("protection_state"), "PRIMARY"); EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    EXPECT_EQ(runtime().lastReason, "path_write_failed"); EXPECT_EQ(refs(), 1u);
    bridgeFault.clear(); now += 99; protection->doTask(*protection->m_fsmTimer);
    EXPECT_EQ(state().at("protection_state"), "PRIMARY"); now += 1;
    protection->getExecutor("BP_PROTECTION_FSM")->execute(); path("PROTECTION");
}
TEST_F(BpProtectionFsmTest, ReadFailureRetainsConfirmedPathAndReportsInconsistent)
{
    start(); bridgeFault = "get"; link(false);
    EXPECT_EQ(state().at("protection_state"), "PRIMARY"); EXPECT_EQ(runtime().programmed, "UNKNOWN");
    EXPECT_EQ(runtime().observed, "UNKNOWN"); EXPECT_EQ(runtime().lastConfirmedPath, "PRIMARY");
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    EXPECT_EQ(runtime().lastReason, "path_read_failed");
    link(true); EXPECT_EQ(wtrStatus(), "WAIT_TO_RESTORE"); // UP stability is independent of backup readback.
    const auto due = runtime().wtrDeadline;
    bridgeFault.clear(); tick(100); path("PROTECTION"); EXPECT_EQ(runtime().wtrDeadline, due);
    tick(4899); path("PROTECTION"); tick(1); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, HldMismatchReportsActualPathAndReadFailureKeepsLastObservation)
{
    start(); runtime().lastResync = "2000-01-01T00:00:00Z";
    bridgeDelayedState = SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY; link(false);
    EXPECT_EQ(state().at("protection_state"), "PRIMARY"); EXPECT_EQ(state().at("admin_mode"), "AUTO");
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    EXPECT_NE(state().at("last_resync_ts"), "2000-01-01T00:00:00Z");
    const auto observedAt = state().at("last_resync_ts"); bridgeFault = "get"; tick(100);
    EXPECT_EQ(state().at("protection_state"), "PRIMARY"); EXPECT_EQ(state().at("last_resync_ts"), observedAt);
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    bridgeFault.clear(); bridgeDelayedState = -1; tick(200); path("PROTECTION");
    EXPECT_EQ(state().size(), 11u); EXPECT_EQ(state().at("convergence_mode"), "n/a");
    EXPECT_EQ(state().at("last_arbitration_source"), "n/a");
}
TEST_F(BpProtectionFsmTest, EveryReadbackMismatchStaysInconsistent)
{
    for (const auto attr : {SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID,
        SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_MODE, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE,
        SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_STATE})
    {
        start(); bridgeMismatch = attr; link(false); EXPECT_EQ(runtime().lastReason, "path_mismatch");
        EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT"); EXPECT_EQ(refs(), 1u);
        bridgeMismatch = 0; tick(100); path("PROTECTION"); config(DEL_COMMAND); po();
    }
}
TEST_F(BpProtectionFsmTest, BackoffCapsIntervalAndContinuesRecoveryWithoutAnExternalEvent)
{
    start(); bridgeFault = "admin"; link(false); EXPECT_EQ(to_string(runtime().retryAttempts), "1");
    for (const uint64_t delay : {100u, 200u, 400u, 800u, 1600u, 3200u, 5000u})
    {
        auto writes = bridgeWrites.size(); tick(delay-1); EXPECT_EQ(bridgeWrites.size(), writes); tick(1);
        EXPECT_EQ(bridgeWrites.size(), writes+1);
    }
    EXPECT_EQ(to_string(runtime().retryAttempts), "8"); EXPECT_EQ(string(runtime().retryAttempts >= 7 ? "true" : "false"), "true");
    // A long-lived failure must not wrap the telemetry counter or stop retries.
    runtime().retryAttempts = std::numeric_limits<unsigned>::max();
    tick(5000); EXPECT_EQ(to_string(runtime().retryAttempts), to_string(std::numeric_limits<unsigned>::max()));
    auto writes = bridgeWrites.size(); bridgeFault.clear(); tick(4999); EXPECT_EQ(bridgeWrites.size(), writes);
    tick(1); path("PROTECTION"); EXPECT_EQ(to_string(runtime().retryAttempts), "0");
    EXPECT_EQ(string(runtime().retryAttempts >= 7 ? "true" : "false"), "false");
}
TEST_F(BpProtectionFsmTest, LinkAndPolicyChangesPreemptRetryDelay)
{
    start(); bridgeFault = "admin"; link(false); bridgeFault.clear(); policy("PRIMARY"); path("PRIMARY");
    link(true); policy(); bridgeFault = "admin"; link(false); bridgeFault.clear();
    link(true); path("PROTECTION");
}
TEST_F(BpProtectionFsmTest, UnsupportedPathStatusLatchesWithoutDroppingActiveGroup)
{
    for (const sai_status_t status : std::vector<sai_status_t>{SAI_STATUS_NOT_SUPPORTED, SAI_STATUS_NOT_IMPLEMENTED,
        SAI_STATUS_ATTR_NOT_SUPPORTED_0, SAI_STATUS_ATTR_NOT_SUPPORTED_MAX,
        SAI_STATUS_ATTR_NOT_IMPLEMENTED_0, SAI_STATUS_ATTR_NOT_IMPLEMENTED_MAX,
        SAI_STATUS_UNKNOWN_ATTRIBUTE_0, SAI_STATUS_UNKNOWN_ATTRIBUTE_MAX})
    {
        start(); bridgeStatus = status; bridgeFault = "admin"; link(false);
        EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT"); EXPECT_EQ(refs(), 1u);
        auto writes = bridgeWrites.size(); tick(100000); EXPECT_EQ(bridgeWrites.size(), writes);
        bridgeFault.clear(); policy(); path("PROTECTION"); config(DEL_COMMAND); po();
    }
}
TEST_F(BpProtectionFsmTest, UnsupportedReadStatusAlsoLatches)
{
    start(); bridgeStatus = SAI_STATUS_NOT_SUPPORTED; bridgeFault = "get"; link(false);
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT"); bridgeFault.clear(); policy(); path("PROTECTION");
}
TEST_F(BpProtectionFsmTest, ConfigDeletionCancelsCountdownAndAllLaterTicks)
{
    start(); link(false); link(true); config(DEL_COMMAND); auto writes = bridgeWrites.size();
    tick(10000); EXPECT_TRUE(state().empty()); EXPECT_EQ(refs(), 0u); EXPECT_EQ(bridgeWrites.size(), writes);
}
TEST_F(BpProtectionFsmTest, BindingWithdrawalCancelsCountdownAndReplayDownRestoresBackup)
{
    start(); link(false); link(true); apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND);
    EXPECT_EQ(wtrStatus(), "IDLE"); link(false); bind(); tick(10000);
    path("PROTECTION"); EXPECT_EQ(wtrStatus(), "IDLE");
    link(true); tick(4999); path("PROTECTION"); tick(1); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, GroupWithdrawalInvalidatesCountdownEvenWhileClearFails)
{
    start(); link(false); link(true); bridgeFault = "clear";
    builder->deleteProtectionRow("2147483649"); step(); EXPECT_EQ(wtrStatus(), "IDLE");
    auto writes = bridgeWrites.size(); tick(10000); EXPECT_EQ(bridgeWrites.size(), writes+1);
    EXPECT_EQ(bridgeWrites.back().second, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID); EXPECT_EQ(refs(), 1u);
    bridgeFault.clear(); tick(1); step(); EXPECT_TRUE(groups.empty());
}
TEST_F(BpProtectionFsmTest, BridgeRemovalCancelsCountdownAndRecreateStartsFresh)
{
    start(); link(false); link(true); Port value = port(); ASSERT_TRUE(gPortsOrch->removeBridgePort(value));
    auto writes = bridgeWrites.size(); tick(10000); EXPECT_EQ(bridgeWrites.size(), writes);
    ASSERT_TRUE(gPortsOrch->addBridgePort(value)); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, ReplacementInvalidatesOldWindowWhilePreservingSelectedBackup)
{
    start(); link(false); link(true); tick(4000); group("2147483650", "1073741825"); step(); bind("2147483650");
    EXPECT_EQ(refs(), 0u); EXPECT_EQ(refs("2147483650"), 1u); tick(4999);
    EXPECT_EQ(state().at("protection_state"), "PROTECTION"); tick(1); EXPECT_EQ(state().at("protection_state"), "PRIMARY");
}
TEST_F(BpProtectionFsmTest, ReentrantRemovalDuringPathReadCannotPublishSteady)
{
    start(); readHook = [&]{ auto removal = preRemove(port()); EXPECT_FALSE(removal.ready); };
    link(false); EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT"); EXPECT_EQ(refs(), 1u);
    tick(10000); EXPECT_EQ(refs(), 0u); EXPECT_EQ(wtrStatus(), "IDLE");
}
TEST_F(BpProtectionFsmTest, ReentrantGroupWithdrawalDuringPathReadRetainsUntilClear)
{
    start(); readHook = [&]{ builder->deleteProtectionRow("2147483649"); step(); };
    link(false); EXPECT_EQ(runtime().lastReason, "path_dependency_changed"); EXPECT_EQ(refs(), 1u);
    tick(100); step(); EXPECT_TRUE(groups.empty());
}
TEST_F(BpProtectionFsmTest, InvalidPolicyDoesNotRestartWindow)
{
    start(); link(false); link(true); tick(4000); auto due = runtime().wtrDeadline;
    apply(CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME, SET_COMMAND, {{"wait_to_restore_time_ms", "-1"}});
    EXPECT_EQ(runtime().wtrDeadline, due); tick(1000); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, UnknownTimerAndDetachedTimerHaveNoEffects)
{
    start(); link(false); link(true); SelectableTimer other(timespec{0, 100000000}); now += 10000;
    EXPECT_EQ(protection->m_fsmTimer->getPri(), -1);
    EXPECT_EQ(protection->getExecutor("BP_PROTECTION_FSM")->getPri(), -1);
    protection->doTask(other); path("PROTECTION");
    protection->getExecutor("BP_PROTECTION_FSM")->execute(); path("PRIMARY");
    protection->detachObservers(); auto writes = bridgeWrites.size();
    protection->doTask(*protection->m_fsmTimer); EXPECT_EQ(bridgeWrites.size(), writes);
}
TEST_F(BpProtectionFsmTest, DeadlineSaturatesInsteadOfWrapping)
{
    now = std::numeric_limits<uint64_t>::max()-100;
    start(); link(false); link(true); EXPECT_EQ(runtime().wtrDeadline, std::numeric_limits<uint64_t>::max());
    tick(99); path("PROTECTION"); tick(1); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, MemberNotificationsAndUnknownPortsDoNotChangeAggregateDecision)
{
    start(); link(false); link(true); auto due = runtime().wtrDeadline;
    Port memberPort("Ethernet0", Port::PHY); PortOperStateUpdate event{memberPort, SAI_PORT_OPER_STATUS_DOWN};
    protection->update(SUBJECT_TYPE_PORT_OPER_STATE_CHANGE, &event);
    Port missing("PortChannel99", Port::LAG); PortOperStateUpdate missingEvent{missing, SAI_PORT_OPER_STATUS_DOWN};
    protection->update(SUBJECT_TYPE_PORT_OPER_STATE_CHANGE, &missingEvent);
    EXPECT_EQ(runtime().wtrDeadline, due); tick(5000); path("PRIMARY");
}

class BpProtectionNativeFsmTest : public BpProtectionNativeObserverTest
{
protected:
    uint64_t now = 1000;
    void PostSetUp() override
    { BpProtectionNativeObserverTest::PostSetUp(); protection->m_now = [&]{ return now; }; }
    void policy(const string &admin = "AUTO", bool revertive = true, uint32_t wtr = 5000)
    {
        apply(CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME, SET_COMMAND,
            {{"admin_mode", admin}, {"revertive", revertive ? "true" : "false"},
             {"wait_to_restore_time_ms", to_string(wtr)}});
    }
    void start(const string &admin = "AUTO", bool revertive = true, uint32_t wtr = 5000)
    { nativeReady(); nativeAdd(); policy(admin, revertive, wtr); bind(); }
    void link(bool up)
    { Port value = port(); gPortsOrch->updatePortOperStatus(value, up ? SAI_PORT_OPER_STATUS_UP : SAI_PORT_OPER_STATUS_DOWN); }
    void tick(uint64_t ms)
    { now += ms; protection->doTask(); protection->doTask(*protection->m_fsmTimer); }
    void path(int32_t admin, int32_t observed)
    {
        sai_attribute_t attrs[2]{};
        attrs[0].id = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE;
        attrs[1].id = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_STATE;
        ASSERT_EQ(sai_bridge_api->get_bridge_port_attribute(port().m_bridge_port_id, 2, attrs), SAI_STATUS_SUCCESS);
        EXPECT_EQ(attrs[0].value.s32, admin); EXPECT_EQ(attrs[1].value.s32, observed);
        EXPECT_EQ(state().at("reconciliation_state"), "STEADY"); EXPECT_EQ(refs(), 1u);
    }
};
TEST_F(BpProtectionNativeFsmTest, NativeNonRevertiveFailoverAndManualReturn)
{
    start("AUTO", false); link(false);
    path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION, SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION);
    link(true); tick(100000);
    path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION, SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION);
    policy("PRIMARY", false); policy("AUTO", false); path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY, SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY);
}
TEST_F(BpProtectionNativeFsmTest, NativeWtrFlapCancellationAndExpiry)
{
    start(); link(false);
    for (unsigned flap = 0; flap < 3; ++flap)
    {
        link(true); tick(4999);
        path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION, SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION);
        link(false); EXPECT_EQ(wtrStatus(), "IDLE");
    }
    link(true); tick(4999);
    path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION, SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION);
    tick(1); path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY, SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY);
}
TEST_F(BpProtectionNativeFsmTest, NativePinnedPolicyAndSelectingAutoWithZeroWtr)
{
    for (bool revertive : {false, true})
    {
        start("PRIMARY", revertive); link(false);
        path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY, SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY);
        policy("AUTO", revertive); path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION, SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION);
        policy("PROTECTION", revertive); link(true); tick(100000);
        path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION, SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION);
        policy("AUTO", true, 0); path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY, SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY);
    }
}
TEST_F(BpProtectionNativeFsmTest, NativeInitialDownAttachesAllAdminPolicies)
{
    nativeReady(); nativeAdd(); link(false);
    for (const string admin : {"AUTO", "PRIMARY", "PROTECTION"})
    {
        policy(admin); bind();
        const bool primary = admin == "PRIMARY";
        path(primary ? SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY : SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION,
             primary ? SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY : SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION);
        EXPECT_EQ(state().at("admin_mode"), admin);
        EXPECT_EQ(desired(), primary ? "PRIMARY" : "PROTECTION");
        EXPECT_EQ(wtrStatus(), "IDLE"); tick(10000);
        config(DEL_COMMAND); EXPECT_EQ(refs(), 0u);
    }
}
TEST_F(BpProtectionNativeFsmTest, NativeColdObjectReplayWhileDownRestoresPolicyAndRecovery)
{
    nativeAdd(); link(false);
    for (const string admin : {"AUTO", "PRIMARY", "PROTECTION"})
    for (bool revertive : {false, true})
    {
        link(false); member("1073741825"); nativeReady(); policy(admin, revertive); bind();
        const bool primary = admin == "PRIMARY";
        const auto requested = primary ? SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY : SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION;
        const auto observed = primary ? SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY : SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION;
        path(requested, observed);
        apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND);
        EXPECT_EQ(refs(), 0u); builder->deleteProtectionRow("2147483649");
        builder->deleteProtectionRow("1073741825"); step(); step(); ASSERT_TRUE(groups.empty());
        EXPECT_EQ(state().at("admin_mode"), admin); EXPECT_EQ(wtrStatus(), "IDLE");
        // Replayed Group/Binding precede the Member while the primary is DOWN.
        nativeReady(); bind(); EXPECT_EQ(runtime().lastReason, "nhg_pending");
        member("1073741825"); step(); path(requested, observed);
        EXPECT_EQ(port().m_oper_status, SAI_PORT_OPER_STATUS_DOWN);
        EXPECT_EQ(wtrStatus(), "IDLE");
        sai_attribute_t association{}; association.id = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID;
        ASSERT_EQ(sai_bridge_api->get_bridge_port_attribute(port().m_bridge_port_id, 1, &association), SAI_STATUS_SUCCESS);
        EXPECT_EQ(association.value.oid, builder->m_protectionGroups.at("2147483649").oid);
        link(true);
        if (admin == "AUTO" && revertive)
        {
            EXPECT_EQ(wtrStatus(), "WAIT_TO_RESTORE"); tick(4999);
            path(requested, observed); tick(1);
            path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY, SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY);
        }
        else { tick(10000); path(requested, observed); }
        config(DEL_COMMAND); EXPECT_EQ(refs(), 0u);
    }
}

TEST_F(BpProtectionFsmTest, PortIdentityChangedDuringReadCannotCertifyOldPath)
{
    for (unsigned change = 0; change < 3; ++change)
    {
        start(); Port old = port();
        readHook = [&, old, change] {
            if (change == 0) gPortsOrch->m_portList.erase(old.m_alias);
            else
            {
                Port recreated = old;
                if (change == 1) ++recreated.m_bridge_port_id;
                else ++recreated.m_bridge_port_generation;
                gPortsOrch->setPort(recreated.m_alias, recreated);
            }
        };
        link(false); EXPECT_EQ(runtime().lastReason, "path_dependency_changed");
        EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT"); EXPECT_EQ(refs(), 1u);
        // Restore this fault fixture's cache before cleanup; real incarnation
        // conflict/drain and removal acknowledgements have separate tests.
        gPortsOrch->setPort(old.m_alias, old); config(DEL_COMMAND); po();
    }
}

TEST_F(BpProtectionFsmTest, ManualPrimaryDuringWtrCancelsCountdownWithoutLateWrites)
{
    start(); link(false); link(true); tick(1000);
    ASSERT_EQ(wtrStatus(), "WAIT_TO_RESTORE");
    policy("PRIMARY"); path("PRIMARY"); EXPECT_EQ(wtrStatus(), "IDLE");
    EXPECT_EQ(runtime().wtrDeadline, 0u);
    auto writes = bridgeWrites.size(); tick(10000); EXPECT_EQ(bridgeWrites.size(), writes); path("PRIMARY");
    link(false); path("PRIMARY"); EXPECT_EQ(bridgeWrites.size(), writes);
}
TEST_F(BpProtectionFsmTest, NonRevertiveProtectionToAutoWhileUpKeepsBackupUntilManualReturn)
{
    start("PROTECTION", false); path("PROTECTION"); auto writes = bridgeWrites.size();
    policy("AUTO", false); path("PROTECTION"); EXPECT_EQ(wtrStatus(), "IDLE");
    tick(100000); EXPECT_EQ(bridgeWrites.size(), writes); path("PROTECTION");
    policy("PRIMARY", false); path("PRIMARY"); policy("AUTO", false); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, SuccessfulBackupWriteWithDelayedOldPrimaryStateUsesBackoffThenConverges)
{
    start("AUTO", false); bridgeDelayedState = SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY; link(false);
    // ADMIN_MODE has changed successfully; the committed path has not yet
    // acknowledged it. Reporting either STEADY or PROTECTION would be wrong.
    EXPECT_EQ(desired(), "PROTECTION");
    EXPECT_EQ(runtime().programmed, "PROTECTION");
    EXPECT_EQ(state().at("protection_state"), "PRIMARY");
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT"); EXPECT_EQ(refs(), 1u);
    for (uint64_t delay : {100u, 200u, 400u, 800u, 1600u, 3200u, 5000u})
    {
        auto writes = bridgeWrites.size(); tick(delay-1); EXPECT_EQ(bridgeWrites.size(), writes);
        tick(1); EXPECT_EQ(bridgeWrites.size(), writes);
        EXPECT_EQ(state().at("protection_state"), "PRIMARY");
        EXPECT_EQ(runtime().programmed, "PROTECTION");
        EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT"); EXPECT_EQ(refs(), 1u);
    }
    bridgeDelayedState = -1; tick(5000); path("PROTECTION");
    EXPECT_EQ(to_string(runtime().retryAttempts), "0");
}
TEST_F(BpProtectionFsmTest, FailedPrimaryRestoreAtExpiryRetriesWithoutRestartingWtr)
{
    start(); link(false); link(true); bridgeFault = "admin"; tick(5000);
    EXPECT_EQ(desired(), "PRIMARY");
    EXPECT_EQ(runtime().programmed, "PROTECTION");
    EXPECT_EQ(state().at("protection_state"), "PROTECTION");
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    EXPECT_EQ(wtrStatus(), "IDLE"); EXPECT_EQ(runtime().lastReason, "path_write_failed");
    bridgeFault.clear(); auto writes = bridgeWrites.size(); link(true); tick(99);
    EXPECT_EQ(bridgeWrites.size(), writes); EXPECT_EQ(wtrStatus(), "IDLE");
    tick(1); path("PRIMARY"); EXPECT_EQ(to_string(runtime().retryAttempts), "0");
}
TEST_F(BpProtectionFsmTest, DownDuringFailedPrimaryRestorePreemptsRetryAndNextUpStartsFreshWtr)
{
    start(); link(false); link(true); bridgeFault = "admin"; tick(5000);
    ASSERT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    now += 50; bridgeFault.clear(); link(false); path("PROTECTION");
    EXPECT_EQ(desired(), "PROTECTION"); EXPECT_EQ(to_string(runtime().retryAttempts), "0");
    auto writes = bridgeWrites.size(); tick(10000); EXPECT_EQ(bridgeWrites.size(), writes); path("PROTECTION");
    link(true); EXPECT_EQ(runtime().wtrDeadline, now+5000);
    tick(4999); path("PROTECTION"); tick(1); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, SuccessfulPrimaryWriteWithDelayedOldProtectionStateDoesNotRestartWtr)
{
    start(); link(false); link(true); bridgeDelayedState = SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION;
    tick(5000); EXPECT_EQ(runtime().programmed, "PRIMARY");
    EXPECT_EQ(desired(), "PRIMARY"); EXPECT_EQ(state().at("protection_state"), "PROTECTION");
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT"); EXPECT_EQ(wtrStatus(), "IDLE");
    tick(100); EXPECT_EQ(to_string(runtime().retryAttempts), "2");
    EXPECT_EQ(state().at("protection_state"), "PROTECTION"); EXPECT_EQ(wtrStatus(), "IDLE");
    bridgeDelayedState = -1; tick(200); path("PRIMARY");
}

TEST_F(BpProtectionNativeFsmTest, NativeManualPrimaryDuringWtrCancelsCountdown)
{
    start(); link(false); link(true); tick(1000); ASSERT_EQ(wtrStatus(), "WAIT_TO_RESTORE");
    policy("PRIMARY"); EXPECT_EQ(wtrStatus(), "IDLE");
    path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY, SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY);
    tick(10000); link(false);
    path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY, SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY);
}
TEST_F(BpProtectionNativeFsmTest, NativeProtectionToAutoUpHandoffHonorsBothRevertivePolicies)
{
    start("PROTECTION", false); policy("AUTO", false); tick(100000);
    EXPECT_EQ(wtrStatus(), "IDLE");
    path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION, SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION);
    policy("PRIMARY", false); policy("AUTO", false);
    path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY, SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY);
    policy("PROTECTION"); policy("AUTO"); EXPECT_EQ(wtrStatus(), "WAIT_TO_RESTORE");
    tick(4999);
    path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION, SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION);
    tick(1); path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY, SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY);
}

namespace
{
decltype(sai_bridge_api->set_bridge_port_attribute) nativeFsmSet = nullptr;
decltype(sai_bridge_api->get_bridge_port_attribute) nativeFsmGet = nullptr;
bool nativeRejectAdmin = false;
sai_status_t nativeFailureStatus = SAI_STATUS_FAILURE;
bool nativeRejectRead = false;
unsigned nativeWriteCalls = 0, nativeReadCalls = 0;
int32_t nativeReportedState = -1;
sai_status_t nativeFaultSet(sai_object_id_t oid, const sai_attribute_t *attr)
{
    ++nativeWriteCalls;
    if (nativeRejectAdmin && attr->id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE)
        return nativeFailureStatus;
    return nativeFsmSet(oid, attr);
}
sai_status_t nativeFaultGet(sai_object_id_t oid, uint32_t count, sai_attribute_t *attrs)
{
    ++nativeReadCalls;
    if (nativeRejectRead) return nativeFailureStatus;
    auto status = nativeFsmGet(oid, count, attrs);
    if (status == SAI_STATUS_SUCCESS && nativeReportedState >= 0)
        for (uint32_t i = 0; i < count; ++i)
            if (attrs[i].id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_STATE)
                attrs[i].value.s32 = nativeReportedState;
    return status;
}
}
class BpProtectionNativeFsmFaultTest : public BpProtectionNativeFsmTest
{
protected:
    void PostSetUp() override
    {
        BpProtectionNativeFsmTest::PostSetUp(); nativeRejectAdmin = false; nativeRejectRead = false; nativeFailureStatus = SAI_STATUS_FAILURE; nativeReportedState = -1;
        nativeFsmSet = sai_bridge_api->set_bridge_port_attribute;
        nativeFsmGet = sai_bridge_api->get_bridge_port_attribute;
        sai_bridge_api->set_bridge_port_attribute = nativeFaultSet;
        sai_bridge_api->get_bridge_port_attribute = nativeFaultGet;
    }
    void PreTearDown() override
    {
        nativeRejectAdmin = false; nativeRejectRead = false; nativeFailureStatus = SAI_STATUS_FAILURE; nativeReportedState = -1;
        sai_bridge_api->set_bridge_port_attribute = nativeFsmSet;
        sai_bridge_api->get_bridge_port_attribute = nativeFsmGet;
        BpProtectionNativeFsmTest::PreTearDown();
    }
};
TEST_F(BpProtectionNativeFsmFaultTest, NativeRejectedWriteAndDelayedValidOldPathRecoverWithReadback)
{
    start("AUTO", false); nativeRejectAdmin = true; link(false);
    EXPECT_EQ(state().at("protection_state"), "PRIMARY");
    EXPECT_EQ(runtime().programmed, "PRIMARY");
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    nativeRejectAdmin = false; tick(100);
    path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION, SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION);
    policy("PRIMARY", false); link(true); policy("AUTO", false);
    nativeReportedState = SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY; link(false);
    EXPECT_EQ(runtime().programmed, "PROTECTION");
    EXPECT_EQ(state().at("protection_state"), "PRIMARY"); EXPECT_EQ(runtime().lastReason, "path_mismatch");
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    // The real libVS command has committed. Only our readback fault wrapper
    // reports delay; this test does not claim to reproduce vendor timing.
    sai_attribute_t actual{}; actual.id = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_STATE;
    ASSERT_EQ(nativeFsmGet(port().m_bridge_port_id, 1, &actual), SAI_STATUS_SUCCESS);
    EXPECT_EQ(actual.value.s32, SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION);
    tick(99); EXPECT_EQ(to_string(runtime().retryAttempts), "1"); tick(1);
    EXPECT_EQ(to_string(runtime().retryAttempts), "2"); EXPECT_EQ(state().at("protection_state"), "PRIMARY");
    nativeReportedState = -1; tick(200);
    path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION, SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION);
}

TEST_F(BpProtectionNativeFsmFaultTest, NativeFailedReadRecoversByGetWithoutRepeatingSuccessfulWrite)
{
    start("AUTO", false); nativeRejectRead = true; link(false);
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    const auto writes = nativeWriteCalls; auto reads = nativeReadCalls;
    tick(99); EXPECT_EQ(nativeReadCalls, reads); tick(1); EXPECT_EQ(nativeReadCalls, ++reads);
    EXPECT_EQ(nativeWriteCalls, writes); nativeRejectRead = false; tick(200);
    EXPECT_EQ(nativeReadCalls, reads+1); EXPECT_EQ(nativeWriteCalls, writes);
    path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION, SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION);
}
TEST_F(BpProtectionNativeFsmFaultTest, NativeReadFirstRepairsAdminDriftAndVerifiesRealMetadata)
{
    start("AUTO", false); nativeRejectRead = true; link(false); nativeRejectRead = false;
    sai_attribute_t drift{}; drift.id = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE;
    drift.value.s32 = SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY;
    ASSERT_EQ(nativeFsmSet(port().m_bridge_port_id, &drift), SAI_STATUS_SUCCESS);
    const auto writes = nativeWriteCalls, reads = nativeReadCalls; tick(100);
    EXPECT_EQ(nativeWriteCalls, writes+1); EXPECT_EQ(nativeReadCalls, reads+2);
    path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION, SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION);
}

TEST_F(BpProtectionNativeFsmFaultTest, NativeTerminalSetAndGetRequireExplicitReapplication)
{
    for (bool reading : {false, true})
    {
        link(true); start("AUTO", false); const auto beforeReads = nativeReadCalls;
        nativeFailureStatus = reading ? SAI_STATUS_NOT_SUPPORTED : SAI_STATUS_NOT_IMPLEMENTED;
        nativeRejectRead = reading; nativeRejectAdmin = !reading; link(false);
        EXPECT_TRUE(runtime().blocked); EXPECT_EQ(nativeReadCalls, beforeReads+(reading ? 1u : 0u));
        const auto writes = nativeWriteCalls; const auto reads = nativeReadCalls;
        nativeRejectRead = nativeRejectAdmin = false; link(true); link(false); bind(); tick(100000);
        EXPECT_EQ(nativeWriteCalls, writes); EXPECT_EQ(nativeReadCalls, reads);
        EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
        policy("AUTO", false); EXPECT_FALSE(runtime().blocked);
        path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION, SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION);
        config(DEL_COMMAND); apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND);
    }
}

TEST_F(BpProtectionNativeFsmFaultTest, NativeFailedBackupReadCannotDelayWtrStartOrExpiry)
{
    start("AUTO", true, 50); nativeRejectRead = true; link(false); link(true);
    ASSERT_EQ(wtrStatus(), "WAIT_TO_RESTORE"); EXPECT_EQ(runtime().wtrDeadline, now+50);
    EXPECT_EQ(state().at("protection_state"), "PRIMARY"); EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    const auto writes = nativeWriteCalls; tick(50);
    EXPECT_EQ(nativeWriteCalls, writes+1); EXPECT_EQ(desired(), "PRIMARY");
    EXPECT_TRUE(runtime().wtrCompleted); EXPECT_EQ(wtrStatus(), "IDLE"); EXPECT_EQ(refs(), 1u);
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    nativeRejectRead = false; const auto completedWrites = nativeWriteCalls; tick(100);
    EXPECT_EQ(nativeWriteCalls, completedWrites);
    path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY, SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY);
    tick(10000); EXPECT_EQ(wtrStatus(), "IDLE");
}
TEST_F(BpProtectionNativeFsmFaultTest, NativeConfirmedPrimaryRunsOneWindowWithoutRedundantSaiCalls)
{
    start("AUTO", true, 50); path(SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY, SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY);
    EXPECT_EQ(wtrStatus(), "WAIT_TO_RESTORE"); const auto writes = nativeWriteCalls; const auto reads = nativeReadCalls;
    tick(50); tick(10000); link(true); bind(); policy("AUTO", true, 50);
    EXPECT_EQ(nativeWriteCalls, writes); EXPECT_EQ(nativeReadCalls, reads);
    EXPECT_EQ(wtrStatus(), "IDLE"); EXPECT_TRUE(runtime().wtrCompleted);
}

TEST_F(BpProtectionFsmTest, PartialMemberRemovalFailureNeverReprogramsOrDetachesTheBridge)
{
    for (bool up : {true,false})
    {
        po(up ? SAI_PORT_OPER_STATUS_UP : SAI_PORT_OPER_STATUS_DOWN); start("AUTO",false);
        member("1073741826","10.0.0.3"); member("1073741827","10.0.0.4");
        group("2147483649","1073741825,1073741826,1073741827"); step();
        auto &g=builder->m_protectionGroups.at("2147483649"); const auto oid=g.oid, failed=g.members.at("1073741825"), removed=g.members.at("1073741826");
        const auto writes=bridgeWrites.size(); const auto reads=bridgeReads; failedGroupMembers.insert(failed);
        group("2147483649","1073741827"); step(); protection->doTask();
        EXPECT_TRUE(g.ready); EXPECT_EQ(g.oid,oid); EXPECT_TRUE(gmHops.count(failed)); EXPECT_FALSE(gmHops.count(removed));
        EXPECT_EQ(refs(),1u); EXPECT_EQ(bridgeWrites.size(),writes); EXPECT_EQ(bridgeReads,reads);
        path(up ? "PRIMARY" : "PROTECTION"); failedGroupMembers.clear(); step();
        config(DEL_COMMAND); apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND);
    }
}
TEST_F(BpProtectionFsmTest, FullGroupCleanupWaitsForBridgeClearThenContinuesPastFailedMember)
{
    start("AUTO",false); member("1073741826","10.0.0.3"); member("1073741827","10.0.0.4");
    group("2147483649","1073741825,1073741826,1073741827"); step();
    const auto g=builder->m_protectionGroups.at("2147483649"); const auto failed=g.members.at("1073741825");
    failedGroupMembers.insert(failed); bridgeFault="clear"; memberRemovalAttempts.clear();
    builder->deleteProtectionRow("2147483649"); step(); EXPECT_EQ(refs(),1u); EXPECT_TRUE(memberRemovalAttempts.empty());
    EXPECT_EQ(gmHops.size(),3u); bridgeFault.clear(); protection->doTask(); step();
    EXPECT_EQ(refs(),0u); EXPECT_EQ(gmHops.size(),1u); EXPECT_TRUE(gmHops.count(failed)); EXPECT_TRUE(groups.count(g.oid));
    EXPECT_EQ(state().at("reconciliation_state"),"INCONSISTENT");
    failedGroupMembers.clear(); step(); EXPECT_FALSE(groups.count(g.oid));
}
TEST_F(L2ProtectionRefTest, RmacOnlyUpdatePreservesProtectionOnlyObjectsAndNotifications)
{
    ProtectionListener listener(*builder); install();
    const auto hops=nhTunnels, members=gmHops, memberships=gmGroups;
    const auto tunnel=*tunnels.begin(); const auto oid=builder->m_protectionGroups.at("2147483649").oid;
    const auto writes=calls["rmac-set"]; const auto events=listener.events.size();
    member("1073741825","10.0.0.2","02:00:00:00:00:22"); step();
    EXPECT_EQ(nhTunnels,hops); EXPECT_EQ(gmHops,members); EXPECT_EQ(gmGroups,memberships);
    EXPECT_EQ(tunnels,std::set<sai_object_id_t>{tunnel}); EXPECT_EQ(calls["rmac-set"],writes+1);
    EXPECT_EQ(rmacs.at(tunnel),MacAddress("02:00:00:00:00:22")); EXPECT_EQ(listener.events.size(),events);
    EXPECT_EQ(builder->m_protectionGroups.at("2147483649").oid,oid); EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649"));
    EXPECT_EQ(source->tnl_users_.at("10.0.0.2").protection_refcnt,1u);
    EXPECT_EQ(calls["default-get"],0u); EXPECT_EQ(calls["nh-remove"],0u); EXPECT_EQ(calls["member-remove"],0u);
    member("1073741825","10.0.0.2","02:00:00:00:00:22"); step(); step();
    EXPECT_EQ(calls["rmac-set"],writes+1); EXPECT_EQ(listener.events.size(),events);
}
TEST_F(L2ProtectionRefTest, SharedOwnersMigrateTogetherWithOneSetAndStableAliasesAcrossGroups)
{
    install(); member("1073741826"); group("2147483649","1073741825,1073741826"); group("2147483650","1073741826"); step();
    ProtectionListener listener(*builder); const auto hops=nhTunnels, members=gmHops; const auto tunnel=*tunnels.begin();
    const auto writes=calls["rmac-set"]; const MacAddress mac("02:00:00:00:00:22");
    member("1073741825","10.0.0.2",mac.to_string()); member("1073741826","10.0.0.2",mac.to_string()); step();
    EXPECT_EQ(calls["rmac-set"],writes+1); EXPECT_EQ(nhTunnels,hops); EXPECT_EQ(gmHops,members); EXPECT_TRUE(listener.events.empty());
    const auto &peer=builder->m_protectionPeers.at("10.0.0.2"); EXPECT_EQ(peer.mac,mac); EXPECT_EQ(peer.owners.size(),2u);
    const auto *object=m_VxlanTunnelOrch->getVxlanTunnel(peer.tunnel);
    EXPECT_EQ(object->protection_rmac_users_.at(1073741825u),mac); EXPECT_EQ(object->protection_rmac_users_.at(1073741826u),mac);
    EXPECT_EQ(tunnels,std::set<sai_object_id_t>{tunnel}); EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649"));
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483650")); EXPECT_EQ(calls["default-get"],0u);
}
TEST_F(L2ProtectionRefTest, FailedRmacSetKeepsInstalledPathAndOwnersAndRetriesOncePerTask)
{
    install(); member("1073741826"); group("2147483649","1073741825,1073741826"); step();
    ProtectionListener listener(*builder); const auto hops=nhTunnels, members=gmHops; const auto tunnel=*tunnels.begin();
    const MacAddress old("02:00:00:00:00:01"), target("02:00:00:00:00:22");
    const auto writes=calls["rmac-set"];
    member("1073741825","10.0.0.2",target.to_string()); member("1073741826","10.0.0.2",target.to_string()); fault="rmac-set";
    const auto initialized=gPortsOrch->m_initDone; auto pending=std::move(gPortsOrch->m_pendingPortSet);
    gPortsOrch->m_pendingPortSet.clear(); gPortsOrch->m_initDone=true;
    for (unsigned i=1;i<=3;++i) {
        builder->doTask(); EXPECT_EQ(calls["rmac-set"],writes+i); EXPECT_EQ(nhTunnels,hops); EXPECT_EQ(gmHops,members);
        EXPECT_EQ(rmacs.at(tunnel),old); EXPECT_EQ(builder->m_protectionPeers.at("10.0.0.2").mac,old);
        auto *object=m_VxlanTunnelOrch->getVxlanTunnel(builder->m_protectionPeers.at("10.0.0.2").tunnel);
        EXPECT_EQ(object->protection_rmac_users_.at(1073741825u),old); EXPECT_EQ(object->protection_rmac_users_.at(1073741826u),old);
        EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649")); EXPECT_TRUE(listener.events.empty());
    }
    fault.clear(); builder->doTask(); EXPECT_EQ(rmacs.at(tunnel),target); EXPECT_EQ(nhTunnels,hops); EXPECT_EQ(gmHops,members);
    gPortsOrch->m_initDone=initialized; gPortsOrch->m_pendingPortSet=std::move(pending);
}
TEST_F(L2ProtectionRefTest, PendingRmacUsesLatestIntentAndReturningToInstalledMacCancelsWrite)
{
    install(); const auto tunnel=*tunnels.begin(); const auto hops=nhTunnels;
    fault="rmac-set"; member("1073741825","10.0.0.2","02:00:00:00:00:22"); step();
    member("1073741825","10.0.0.2","02:00:00:00:00:33"); fault.clear(); step();
    EXPECT_EQ(rmacs.at(tunnel),MacAddress("02:00:00:00:00:33")); EXPECT_EQ(nhTunnels,hops);
    fault="rmac-set"; member("1073741825","10.0.0.2","02:00:00:00:00:44"); step();
    const auto writes=calls["rmac-set"]; member("1073741825","10.0.0.2","02:00:00:00:00:33"); step();
    EXPECT_EQ(calls["rmac-set"],writes); EXPECT_EQ(rmacs.at(tunnel),MacAddress("02:00:00:00:00:33"));
}
TEST_F(L2ProtectionRefTest, ConflictingExistingOwnerDefersWithoutRetuningAndLaterAgreementRecovers)
{
    install(); member("1073741826"); group("2147483649","1073741825,1073741826"); step();
    const auto tunnel=*tunnels.begin(); const auto hops=nhTunnels, members=gmHops; const auto writes=calls["rmac-set"];
    member("1073741825","10.0.0.2","02:00:00:00:00:22"); step();
    EXPECT_EQ(calls["rmac-set"],writes); EXPECT_EQ(rmacs.at(tunnel),MacAddress("02:00:00:00:00:01"));
    EXPECT_TRUE(builder->m_protectionMembers.at("1073741825").peer.empty()); EXPECT_EQ(gmHops,members);
    EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649")); EXPECT_EQ(nhTunnels,hops);
    member("1073741826","10.0.0.2","02:00:00:00:00:22"); step();
    EXPECT_EQ(calls["rmac-set"],writes+1); EXPECT_EQ(rmacs.at(tunnel),MacAddress("02:00:00:00:00:22"));
    EXPECT_EQ(builder->m_protectionPeers.at("10.0.0.2").owners.size(),2u); EXPECT_EQ(gmHops,members); EXPECT_EQ(nhTunnels,hops);
}
TEST_F(L2ProtectionRefTest, FailedRmacPeerDoesNotBlockOtherPeersOrInventANewOwner)
{
    install(); member("1073741826","10.0.0.3"); group("2147483649","1073741825,1073741826"); step();
    auto &first=builder->m_protectionPeers.at("10.0.0.2"); const auto failed=nhTunnels.at(first.nh);
    const auto good=nhTunnels.at(builder->m_protectionPeers.at("10.0.0.3").nh); const auto hops=nhTunnels;
    failedRmacTunnels.insert(failed);
    member("1073741825","10.0.0.2","02:00:00:00:00:22"); member("1073741826","10.0.0.3","02:00:00:00:00:33");
    member("1073741827","10.0.0.2","02:00:00:00:00:22"); step();
    EXPECT_EQ(rmacs.at(failed),MacAddress("02:00:00:00:00:01")); EXPECT_EQ(rmacs.at(good),MacAddress("02:00:00:00:00:33"));
    EXPECT_EQ(first.owners.size(),1u); EXPECT_TRUE(builder->m_protectionMembers.at("1073741827").peer.empty());
    EXPECT_EQ(nhTunnels,hops); failedRmacTunnels.clear(); step(); EXPECT_EQ(first.owners.size(),2u); EXPECT_EQ(nhTunnels,hops);
}
TEST_F(L2ProtectionRefTest, RmacSetCallbacksCannotCertifySupersededIntentOrCancelRetirement)
{
    install(); const auto tunnel=*tunnels.begin(); const auto hops=nhTunnels;
    member("1073741825","10.0.0.2","02:00:00:00:00:22");
    rmacSetHook=[&]{ member("1073741825","10.0.0.2","02:00:00:00:00:33"); }; step();
    EXPECT_EQ(builder->m_protectionPeers.at("10.0.0.2").mac,MacAddress("02:00:00:00:00:22"));
    EXPECT_EQ(builder->m_protectionMembers.at("1073741825").mac,MacAddress("02:00:00:00:00:33")); EXPECT_EQ(nhTunnels,hops);
    step(); EXPECT_EQ(rmacs.at(tunnel),MacAddress("02:00:00:00:00:33"));
    member("1073741825","10.0.0.2","02:00:00:00:00:44");
    rmacSetHook=[&]{ builder->deleteProtectionRow("1073741825"); }; step(); step();
    EXPECT_TRUE(nhTunnels.empty()); EXPECT_TRUE(tunnels.empty());
}
TEST_F(L2ProtectionRefTest, PendingRmacWithdrawalAndIdentityChangesKeepLifecycleSafety)
{
    for (const string change : {"delete","missing","l2","vtep","source"}) {
        install(); L2NhgReference held; ASSERT_TRUE(builder->reserveNhg(2147483649u,held)); ASSERT_TRUE(builder->commitNhgRef(held));
        const auto hop=nhTunnels.begin()->first, tunnel=*tunnels.begin();
        member("1073741825","10.0.0.2","02:00:00:00:00:22"); fault="rmac-set"; step(); const auto writes=calls["rmac-set"];
        if (change=="delete") builder->deleteProtectionRow("1073741825");
        if (change=="missing") member("1073741825","10.0.0.2","");
        if (change=="l2") member("1073741825","10.0.0.2","","l2");
        if (change=="vtep") member("1073741825","10.0.0.3","02:00:00:00:00:22");
        if (change=="source") source->active_=false;
        step(); EXPECT_EQ(calls["rmac-set"],writes); EXPECT_TRUE(nhTunnels.count(hop)); EXPECT_TRUE(tunnels.count(tunnel));
        EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649"));
        fault.clear(); source->active_=true; builder->unrefNhg(held);
        builder->deleteProtectionRow("1073741825"); builder->deleteProtectionRow("2147483649"); step(); step();
        EXPECT_TRUE(nhTunnels.empty()); EXPECT_TRUE(tunnels.empty());
    }
}
TEST_F(L2ProtectionRefTest, RmacUpdateOnTunnelSharedWithRouteUserKeepsEveryReference)
{
    ASSERT_TRUE(source->createDynamicDIPTunnel("10.0.0.2",TUNNEL_USER_IP)); install();
    const auto tunnel=*tunnels.begin(); const auto count=source->getRemoteEndPointRefCnt("10.0.0.2"); const auto hops=nhTunnels;
    member("1073741825","10.0.0.2","02:00:00:00:00:22"); step();
    EXPECT_EQ(source->getRemoteEndPointRefCnt("10.0.0.2"),count); EXPECT_EQ(nhTunnels,hops);
    builder->deleteProtectionRow("1073741825"); step(); step(); EXPECT_TRUE(tunnels.count(tunnel));
    EXPECT_EQ(rmacs.at(tunnel),MacAddress("02:00:00:00:00:aa"));
    EXPECT_TRUE(source->deleteDynamicDIPTunnel("10.0.0.2",TUNNEL_USER_IP));
}
TEST_F(L2ProtectionRefTest, RmacTargetRejectsStaleOrMismatchedOwnerRecords)
{
    install(); MacAddress target; auto &peer=builder->m_protectionPeers.at("10.0.0.2");
    const auto owners=peer.owners; peer.owners.clear(); EXPECT_FALSE(builder->getProtectionRmacTarget("10.0.0.2",target));
    peer.owners={9}; EXPECT_FALSE(builder->getProtectionRmacTarget("10.0.0.2",target)); peer.owners=owners;
    auto &owner=builder->m_protectionMembers.at("1073741825"); const auto actual=owner.peer;
    owner.peer="10.0.0.3"; EXPECT_FALSE(builder->getProtectionRmacTarget("10.0.0.2",target)); owner.peer=actual;
    EXPECT_TRUE(builder->getProtectionRmacTarget("10.0.0.2",target));
}
TEST_F(L2ProtectionRefTest, RmacUpdateFailuresLogActualTunnelRequestedMacAndRawStatus)
{
    install(); const auto tunnel=*tunnels.begin();
    auto &logger=swss::Logger::getInstance(); const auto priority=logger.getMinPrio();
    logger.setMinPrio(swss::Logger::SWSS_ERROR); logger.swssOutputNotify("orchagent","STDOUT");
    testing::internal::CaptureStdout(); fault="rmac-set"; faultStatus=SAI_STATUS_NOT_SUPPORTED;
    member("1073741825","10.0.0.2","02:00:00:00:00:22"); step();
    const auto log=testing::internal::GetCapturedStdout(); logger.setMinPrio(priority); logger.swssOutputNotify("orchagent","SYSLOG");
    EXPECT_NE(log.find("event=protection_rmac_update_failed"),string::npos);
    EXPECT_NE(log.find("requested_rmac=02:00:00:00:00:22"),string::npos);
    EXPECT_NE(log.find("owners=1"),string::npos); EXPECT_NE(log.find("status="+to_string(SAI_STATUS_NOT_SUPPORTED)),string::npos);
    EXPECT_NE(log.find("action=retain_and_retry"),string::npos); EXPECT_TRUE(tunnels.count(tunnel));
}
TEST_F(BpProtectionFsmTest, RmacOnlyChangeAndFailedSetNeverDetachOrReprogramTheBridge)
{
    for (const bool up : {true,false}) {
        start("AUTO",false); link(up); const auto writes=bridgeWrites.size(); const auto reads=bridgeReads;
        const auto oid=runtime().reference.oid; const auto bp=runtime().port; const auto members=gmHops;
        const auto tunnel=*tunnels.begin(); const auto installed=rmacs.at(tunnel);
        fault="rmac-set"; member("1073741825","10.0.0.2","02:00:00:00:00:22"); step(); protection->doTask();
        EXPECT_EQ(bridgeWrites.size(),writes); EXPECT_EQ(bridgeReads,reads); EXPECT_EQ(runtime().reference.oid,oid);
        EXPECT_EQ(runtime().port,bp); EXPECT_EQ(gmHops,members); EXPECT_EQ(refs(),1u); EXPECT_EQ(rmacs.at(tunnel),installed);
        fault.clear(); step(); protection->doTask(); EXPECT_EQ(bridgeWrites.size(),writes); EXPECT_EQ(bridgeReads,reads);
        EXPECT_EQ(gmHops,members); EXPECT_EQ(rmacs.at(tunnel),MacAddress("02:00:00:00:00:22"));
        member("1073741825"); step();
    }
}
TEST_F(BpProtectionFsmTest, SharedRmacMigrationDoesNotRestartPolicyWtr)
{
    start("AUTO",true,1000); link(false); link(true); const auto deadline=runtime().wtrDeadline;
    member("1073741826"); group("2147483649","1073741825,1073741826"); step();
    const auto writes=bridgeWrites.size(); const auto members=gmHops;
    member("1073741825","10.0.0.2","02:00:00:00:00:22"); member("1073741826","10.0.0.2","02:00:00:00:00:22");
    now+=400; step(); protection->doTask(); EXPECT_EQ(runtime().wtrDeadline,deadline); EXPECT_EQ(bridgeWrites.size(),writes); EXPECT_EQ(gmHops,members);
    tick(600); path("PRIMARY");
}
TEST_F(BpProtectionFsmTest, PendingRmacUpdateCannotBypassBindingWithdrawalOrFailedBridgeClear)
{
    start("AUTO",false); const auto members=gmHops; const auto tunnel=*tunnels.begin();
    fault="rmac-set"; member("1073741825","10.0.0.2","02:00:00:00:00:22"); step();
    bridgeFault="clear"; apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND,{});
    builder->deleteProtectionRow("1073741825"); builder->deleteProtectionRow("2147483649"); const auto writes=calls["rmac-set"];
    step(); EXPECT_EQ(gmHops,members); EXPECT_TRUE(tunnels.count(tunnel)); EXPECT_EQ(calls["rmac-set"],writes);
    fault.clear(); bridgeFault.clear(); tick(100); step(); step();
    EXPECT_FALSE(runtime().active); EXPECT_TRUE(builder->m_protectionGroups.empty());
    EXPECT_TRUE(gmHops.empty()); EXPECT_TRUE(tunnels.empty());
}
TEST_F(L2ProtectionRefTest, RmacOnlyRetentionRequiresCurrentSourcePeerAndTunnelIdentity)
{
    install(); const auto ip=string("10.0.0.2");
    auto &owner=builder->m_protectionMembers.at("1073741825"); const auto savedPeer=builder->m_protectionPeers.at(ip);
    member("1073741825",ip,"02:00:00:00:00:22"); const auto writes=calls["rmac-set"];
    nvo->source_vtep_ptr=nullptr; EXPECT_FALSE(builder->protectionMemberReady("1073741825")); builder->reconcileProtectionRmac();
    nvo->source_vtep_ptr=source;
    const string key=typeid(EvpnNvoOrch*).name(); const auto registered=gDirectory.m_values.at(key);
    gDirectory.m_values.erase(key); EXPECT_FALSE(builder->protectionMemberReady("1073741825")); builder->reconcileProtectionRmac();
    gDirectory.m_values[key]=registered;
    source->del_tnl_hw_pending=true; EXPECT_FALSE(builder->protectionMemberReady("1073741825")); builder->reconcileProtectionRmac(); source->del_tnl_hw_pending=false;
    builder->m_protectionPeers.at(ip).nh=SAI_NULL_OBJECT_ID; EXPECT_FALSE(builder->protectionMemberReady("1073741825")); builder->reconcileProtectionRmac();
    builder->m_protectionPeers.at(ip)=savedPeer;
    builder->m_protectionPeers.at(ip).source="different-source"; EXPECT_FALSE(builder->protectionMemberReady("1073741825")); builder->reconcileProtectionRmac();
    builder->m_protectionPeers.at(ip)=savedPeer;
    builder->m_protectionPeers.at(ip).tunnel="missing-tunnel"; builder->reconcileProtectionRmac();
    builder->m_protectionPeers.at(ip)=savedPeer;
    builder->m_protectionPeers.erase(ip); EXPECT_FALSE(builder->protectionMemberReady("1073741825")); builder->m_protectionPeers[ip]=savedPeer;
    member("1073741826");
    auto &other=builder->m_protectionMembers.at("1073741826"); other.peer=ip;
    const auto desired=owner.mac; builder->m_protectionPeers.at(ip).owners={other.owner};
    EXPECT_FALSE(builder->protectionMemberReady("1073741825"));
    other.peer.clear(); builder->m_protectionPeers.at(ip)=savedPeer;
    EXPECT_EQ(calls["rmac-set"],writes); EXPECT_EQ(owner.mac,desired);
}
TEST_F(L2ProtectionRefTest, SourceLossDuringSuccessfulRmacSetStillWithdrawsAndHoldsPins)
{
    ProtectionListener listener(*builder); install(); L2NhgReference held;
    ASSERT_TRUE(builder->reserveNhg(2147483649u,held)); ASSERT_TRUE(builder->commitNhgRef(held));
    const auto members=gmHops, hops=nhTunnels;
    member("1073741825","10.0.0.2","02:00:00:00:00:22"); rmacSetHook=[&]{source->active_=false;}; step();
    EXPECT_EQ(builder->m_protectionPeers.at("10.0.0.2").mac,MacAddress("02:00:00:00:00:22"));
    EXPECT_FALSE(builder->hasActiveL2Nhg("2147483649")); EXPECT_EQ(gmHops,members); EXPECT_EQ(nhTunnels,hops);
    ASSERT_EQ(listener.events.size(),2u); EXPECT_EQ(listener.events.back().change,L2NhgChange::WITHDRAWING);
    source->active_=true; builder->unrefNhg(held); step(); EXPECT_TRUE(builder->hasActiveL2Nhg("2147483649"));
}
namespace {
struct ProtectionFdbListener : Observer {
    vector<FdbFlushUpdate> events;
    void update(SubjectType type, void *context) override {
        if (type==SUBJECT_TYPE_FDB_FLUSH_CHANGE) events.push_back(*static_cast<FdbFlushUpdate*>(context));
    }
};
vector<map<sai_attr_id_t,sai_object_id_t>> protectionFdbCalls;
sai_status_t protectionFdbStatus=SAI_STATUS_SUCCESS;
sai_status_t recordProtectionFdbFlush(sai_object_id_t, uint32_t count, const sai_attribute_t *attrs) {
    map<sai_attr_id_t,sai_object_id_t> request;
    for (uint32_t i=0;i<count;++i)
        request[attrs[i].id]=attrs[i].id==SAI_FDB_FLUSH_ATTR_ENTRY_TYPE ? static_cast<sai_object_id_t>(attrs[i].value.s32) : attrs[i].value.oid;
    protectionFdbCalls.push_back(request); return protectionFdbStatus;
}
}
class BpProtectionFdbTest : public BpProtectionFsmTest
{
protected:
    BpProtOrch *previousProtection=nullptr;
    ProtectionFdbListener listener;
    vector<NeighborEntry> neighbors;
    void PostSetUp() override {
        BpProtectionFsmTest::PostSetUp();
        Table(m_config_db.get(),CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME).del("PortChannel0");
        Table(m_app_db.get(),APP_BP_PROTECTION_BINDING_TABLE_NAME).del("PortChannel0");
        previousProtection=gBpProtOrch; gBpProtOrch=protection.get();
        protectionFdbCalls.clear(); protectionFdbStatus=SAI_STATUS_SUCCESS;
        sai_fdb_api->flush_fdb_entries=recordProtectionFdbFlush; gFdbOrch->attach(&listener);
        for (const uint16_t id : {uint16_t(1000),uint16_t(2000)}) {
            Port vlan("Vlan"+to_string(id),Port::VLAN); vlan.m_vlan_info.vlan_id=id; vlan.m_vlan_info.vlan_oid=0x710000+id;
            gPortsOrch->setPort(vlan.m_alias,vlan); gPortsOrch->saiOidToAlias[vlan.m_vlan_info.vlan_oid]=vlan.m_alias;
            gPortsOrch->m_portVlanMember["PortChannel0"][id]={0,SAI_VLAN_TAGGING_MODE_UNTAGGED};
            FdbEntry entry{MacAddress(id==1000 ? "00:11:22:33:44:01" : "00:11:22:33:44:02"),vlan.m_vlan_info.vlan_oid,"PortChannel0"};
            FdbData data; data.bridge_port_id=port().m_bridge_port_id; data.type="dynamic"; data.origin=FDB_ORIGIN_LEARN;
            gFdbOrch->m_entries[entry]=data;
            NeighborEntry neighbor(IpAddress(id==1000 ? "192.0.2.1" : "192.0.2.2"),vlan.m_alias);
            NeighborData n; n.mac=entry.mac; n.hw_configured=true; gNeighOrch->m_syncdNeighbors[neighbor]=n; neighbors.push_back(neighbor);
        }
        testing_db::resetOperationCounters();
    }
    void PreTearDown() override {
        gFdbOrch->detach(&listener);
        apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND); gBpProtOrch=previousProtection;
        for (const auto &n:neighbors) gNeighOrch->m_syncdNeighbors.erase(n);
        for (auto it=gFdbOrch->m_entries.begin();it!=gFdbOrch->m_entries.end();)
            if (it->first.port_name=="PortChannel0" || it->first.port_name=="PortChannel1") it=gFdbOrch->m_entries.erase(it); else ++it;
        BpProtectionFsmTest::PreTearDown();
    }
    BpProtectionStatus status() { return protection->isProtectedBridgePort("PortChannel0"); }
    void down() { link(false); }
    void expectPreserved() {
        EXPECT_TRUE(protectionFdbCalls.empty()); EXPECT_TRUE(listener.events.empty());
        EXPECT_EQ(testing_db::getProducerSetCount(APP_NEIGH_RESOLVE_TABLE_NAME),0u);
        for (const auto &entry:gFdbOrch->m_entries) if (entry.first.port_name=="PortChannel0") { EXPECT_FALSE(entry.second.is_flush_pending); }
        for (const auto &n:neighbors) EXPECT_TRUE(gNeighOrch->m_syncdNeighbors.at(n).hw_configured);
    }
    void expectNormal(unsigned calls=1) {
        EXPECT_EQ(protectionFdbCalls.size(),calls); EXPECT_EQ(listener.events.size(),2u*calls);
        EXPECT_EQ(testing_db::getProducerSetCount(APP_NEIGH_RESOLVE_TABLE_NAME),2u*calls);
        for (const auto &request:protectionFdbCalls) {
            EXPECT_EQ(request.at(SAI_FDB_FLUSH_ATTR_BRIDGE_PORT_ID),port().m_bridge_port_id);
            EXPECT_EQ(request.at(SAI_FDB_FLUSH_ATTR_ENTRY_TYPE),static_cast<sai_object_id_t>(SAI_FDB_FLUSH_ENTRY_TYPE_DYNAMIC));
        }
    }
};
TEST_F(BpProtectionFdbTest, AttachedProtectionSuppressesSaiFdbAndRealNeighborResolveFanout)
{
    start("AUTO",false); ASSERT_EQ(status(),BpProtectionStatus::PROTECTED); down(); path("PROTECTION"); expectPreserved();
    for (int i=0;i<3;++i) gFdbOrch->updatePortOperState({port(),SAI_PORT_OPER_STATUS_DOWN});
    expectPreserved();
}
TEST_F(BpProtectionFdbTest, PolicyBeforeBindingAndAllPinnedModesSuppressDown)
{
    for (const string admin:{"AUTO","PRIMARY","PROTECTION"}) {
        config(SET_COMMAND,admin); ASSERT_EQ(status(),BpProtectionStatus::PENDING); down(); expectPreserved();
        po(); config(DEL_COMMAND);
    }
}
TEST_F(BpProtectionFdbTest, BindingBeforePolicyAndMissingGroupRemainPending)
{
    bind(); ASSERT_EQ(status(),BpProtectionStatus::PENDING); down(); expectPreserved();
    config(); builder->deleteProtectionRow("2147483649"); step(); ASSERT_EQ(status(),BpProtectionStatus::PENDING); expectPreserved();
}
TEST_F(BpProtectionFdbTest, TerminalSaiFailureAndUnavailableBackupCannotLoseFdb)
{
    bridgeFault="group"; bridgeStatus=SAI_STATUS_NOT_SUPPORTED; start();
    ASSERT_EQ(status(),BpProtectionStatus::PENDING); down(); expectPreserved();
    bridgeFault.clear(); config(); builder->deleteProtectionRow("1073741825"); step();
    EXPECT_EQ(status(),BpProtectionStatus::PENDING); expectPreserved();
}
TEST_F(BpProtectionFdbTest, BothWithdrawalOrdersFlushOnlyOnConfirmedNoneAndOnlyOnce)
{
    for (const bool configFirst:{true,false}) {
        if (configFirst) { start("AUTO",false); down(); }
        else { po(); start("AUTO",false); down(); }
        expectPreserved();
        if (configFirst) config(DEL_COMMAND); else apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND);
        EXPECT_EQ(status(),BpProtectionStatus::PENDING); expectPreserved();
        if (configFirst) apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND); else config(DEL_COMMAND);
        EXPECT_EQ(status(),BpProtectionStatus::NONE); expectNormal();
        config(DEL_COMMAND); apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND); protection->doTask(); expectNormal();
        protectionFdbCalls.clear(); listener.events.clear(); testing_db::resetOperationCounters();
        for (auto &entry:gFdbOrch->m_entries) entry.second.is_flush_pending=false;
    }
}
TEST_F(BpProtectionFdbTest, WithdrawalWhileUpDoesNotFlushAndFutureOrdinaryDownDoes)
{
    start("AUTO",false); config(DEL_COMMAND); apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND);
    EXPECT_EQ(status(),BpProtectionStatus::NONE); expectPreserved(); down(); expectNormal();
}
TEST_F(BpProtectionFdbTest, FpmBindingFlushAndReplayWithPolicyKeepFdbAndNoReplaySignalIsNeeded)
{
    start("AUTO",false); down(); apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND);
    builder->deleteProtectionRow("2147483649"); builder->deleteProtectionRow("1073741825"); step(); step();
    EXPECT_EQ(status(),BpProtectionStatus::PENDING); expectPreserved();
    member("1073741825"); group("2147483649","1073741825"); step(); bind(); path("PROTECTION");
    EXPECT_EQ(status(),BpProtectionStatus::PROTECTED); expectPreserved();
}
TEST_F(BpProtectionFdbTest, RedisPolicyBeforeConsumerAndCoalescedDeleteKeepIntentUntilDelivery)
{
    Table configTable(m_config_db.get(),CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME);
    configTable.set("PortChannel0",{{"NULL","NULL"}}); ASSERT_EQ(status(),BpProtectionStatus::PENDING);
    down(); expectPreserved(); configTable.del("PortChannel0");
    EXPECT_EQ(status(),BpProtectionStatus::PENDING); gFdbOrch->updatePortOperState({port(),SAI_PORT_OPER_STATUS_DOWN}); expectPreserved();
    config(DEL_COMMAND); EXPECT_EQ(status(),BpProtectionStatus::NONE); expectNormal(); config(DEL_COMMAND); expectNormal();
}
TEST_F(BpProtectionFdbTest, RedisBindingBeforeConsumerRemainsConservativeUntilConfirmedDelete)
{
    Table binding(m_app_db.get(),APP_BP_PROTECTION_BINDING_TABLE_NAME);
    binding.set("PortChannel0",{{"nexthop_group","2147483649"}}); down(); expectPreserved();
    binding.del("PortChannel0"); EXPECT_EQ(status(),BpProtectionStatus::PENDING); expectPreserved();
    apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND); EXPECT_EQ(status(),BpProtectionStatus::NONE); expectNormal();
}
TEST_F(BpProtectionFdbTest, StaleConfigSetConfirmsMissingPolicyAndTriggersDeferredFlush)
{
    config(); down(); expectPreserved(); Table(m_config_db.get(),CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME).del("PortChannel0");
    auto consumer=dynamic_cast<Consumer*>(protection->getConsumerBase(CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME));
    consumer->addToSync(KeyOpFieldsValuesTuple{"PortChannel0",SET_COMMAND,{{"admin_mode","AUTO"}}}); protection->doTask(*consumer);
    EXPECT_EQ(status(),BpProtectionStatus::NONE); expectNormal();
}
TEST_F(BpProtectionFdbTest, StaleBindingSetConfirmsMissingBindingAndTriggersDeferredFlush)
{
    bind(); down(); expectPreserved(); Table(m_app_db.get(),APP_BP_PROTECTION_BINDING_TABLE_NAME).del("PortChannel0");
    auto consumer=dynamic_cast<Consumer*>(protection->getConsumerBase(APP_BP_PROTECTION_BINDING_TABLE_NAME));
    consumer->addToSync(KeyOpFieldsValuesTuple{"PortChannel0",SET_COMMAND,{{"nexthop_group","2147483649"}}}); protection->doTask(*consumer);
    EXPECT_EQ(status(),BpProtectionStatus::NONE); expectNormal();
}
TEST_F(BpProtectionFdbTest, RowRecreatedBeforeFinalDeleteProcessingSuppressesPrematureFlush)
{
    start("AUTO",false); down(); apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND); expectPreserved();
    auto consumer=dynamic_cast<Consumer*>(protection->getConsumerBase(CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME));
    Table(m_config_db.get(),CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME).set("PortChannel0",{{"NULL","NULL"}});
    consumer->addToSync(KeyOpFieldsValuesTuple{"PortChannel0",DEL_COMMAND,{}}); protection->doTask(*consumer);
    EXPECT_EQ(status(),BpProtectionStatus::PENDING); expectPreserved();
    config(DEL_COMMAND); expectNormal();
}
TEST_F(BpProtectionFdbTest, GroupReplacementAndFailedReadRetainIntent)
{
    start("AUTO",false); down(); group("2147483650","1073741825"); step(); bridgeFault="get"; bind("2147483650");
    EXPECT_EQ(status(),BpProtectionStatus::PENDING); expectPreserved(); bridgeFault.clear(); tick(5000);
    EXPECT_EQ(status(),BpProtectionStatus::PROTECTED); expectPreserved();
}
TEST_F(BpProtectionFdbTest, ConfirmedIntentWithdrawalFlushesEvenIfHardwareCleanupNeedsRetry)
{
    start("AUTO",false); down(); bridgeFault="clear"; config(DEL_COMMAND); expectPreserved();
    apply(APP_BP_PROTECTION_BINDING_TABLE_NAME,DEL_COMMAND); EXPECT_EQ(status(),BpProtectionStatus::NONE); expectNormal();
    EXPECT_EQ(refs(),1u); bridgeFault.clear(); tick(5000); expectNormal(); EXPECT_EQ(refs(),0u);
}
TEST_F(BpProtectionFdbTest, DeferredFlushRechecksCurrentStatusMlagAndFreshIntent)
{
    po(SAI_PORT_OPER_STATUS_UP); gFdbOrch->flushProtectedPortOnWithdraw("PortChannel0"); expectPreserved();
    gFdbOrch->flushProtectedPortOnWithdraw("PortChannel9999"); expectPreserved();
    po(SAI_PORT_OPER_STATUS_DOWN); config(); gFdbOrch->flushProtectedPortOnWithdraw("PortChannel0"); expectPreserved();
    mlag->m_mlagIntfs.insert("PortChannel0"); config(DEL_COMMAND); EXPECT_EQ(status(),BpProtectionStatus::NONE); expectPreserved();
    mlag->m_mlagIntfs.erase("PortChannel0"); gFdbOrch->flushProtectedPortOnWithdraw("PortChannel0"); expectNormal();
}
TEST_F(BpProtectionFdbTest, UnprotectedInterfaceAndDisabledFeatureKeepLegacyBehavior)
{
    EXPECT_EQ(status(),BpProtectionStatus::NONE); down(); expectNormal();
    gBpProtOrch=nullptr; gFdbOrch->updatePortOperState({port(),SAI_PORT_OPER_STATUS_DOWN}); expectNormal(2);
    gBpProtOrch=protection.get();
}
TEST_F(BpProtectionFdbTest, NoBridgePortAndMissingVlanKeepNormalObserverBehavior)
{
    po(SAI_PORT_OPER_STATUS_DOWN,SAI_NULL_OBJECT_ID);
    gPortsOrch->m_portVlanMember["PortChannel0"][9999]={0,SAI_VLAN_TAGGING_MODE_UNTAGGED};
    gFdbOrch->updatePortOperState({port(),SAI_PORT_OPER_STATUS_DOWN});
    EXPECT_TRUE(protectionFdbCalls.empty()); EXPECT_EQ(listener.events.size(),2u);
    EXPECT_EQ(testing_db::getProducerSetCount(APP_NEIGH_RESOLVE_TABLE_NAME),2u);
}
TEST_F(BpProtectionFdbTest, DirectVlanTeardownAndExplicitFlushAreNotSuppressed)
{
    start("AUTO",false); down(); expectPreserved(); Port vlan; ASSERT_TRUE(gPortsOrch->getPort("Vlan1000",vlan));
    VlanMemberUpdate event{vlan,port(),false}; gFdbOrch->update(SUBJECT_TYPE_VLAN_MEMBER_CHANGE,&event);
    EXPECT_EQ(protectionFdbCalls.size(),1u); EXPECT_EQ(listener.events.size(),1u);
    EXPECT_EQ(testing_db::getProducerSetCount(APP_NEIGH_RESOLVE_TABLE_NAME),1u);
    gFdbOrch->flushFDBEntries(port().m_bridge_port_id,SAI_NULL_OBJECT_ID); EXPECT_EQ(protectionFdbCalls.size(),2u);
}
TEST_F(BpProtectionFdbTest, FailedNormalFlushRetainsExistingSaiAndNotificationPolicy)
{
    protectionFdbStatus=SAI_STATUS_FAILURE; down(); expectNormal();
    for (const auto &entry:gFdbOrch->m_entries) if (entry.first.port_name=="PortChannel0") { EXPECT_FALSE(entry.second.is_flush_pending); }
}
TEST_F(BpProtectionFdbTest, PredicateRejectsStaleAttachmentIdentityAndUnreadyGroup)
{
    start("AUTO",false); auto &a=runtime(); const auto reference=a.reference; const auto p=port();
    ASSERT_EQ(status(),BpProtectionStatus::PROTECTED);
    a.removing=true; EXPECT_EQ(status(),BpProtectionStatus::PENDING); a.removing=false;
    a.reference.token=0; EXPECT_EQ(status(),BpProtectionStatus::PENDING); a.reference=reference;
    a.reference.nhg_id=123; EXPECT_EQ(status(),BpProtectionStatus::PENDING); a.reference=reference;
    a.reference.oid=123; EXPECT_EQ(status(),BpProtectionStatus::PENDING); a.reference=reference;
    a.port=123; EXPECT_EQ(status(),BpProtectionStatus::PENDING); a.port=p.m_bridge_port_id;
    a.generation=p.m_bridge_port_generation+1; EXPECT_EQ(status(),BpProtectionStatus::PENDING); a.generation=p.m_bridge_port_generation;
    gPortsOrch->m_portList.erase("PortChannel0"); EXPECT_EQ(status(),BpProtectionStatus::PENDING); gPortsOrch->setPort("PortChannel0",p);
    builder->m_protectionGroups.at("2147483649").ready=false; EXPECT_EQ(status(),BpProtectionStatus::PENDING);
    builder->m_protectionGroups.at("2147483649").ready=true; EXPECT_EQ(status(),BpProtectionStatus::PROTECTED);
}
TEST_F(BpProtectionFdbTest, MissingFdbConsumerCannotRepeatWithdrawalOrTouchOtherInterfaces)
{
    config(); down(); auto fdb=gFdbOrch; gFdbOrch=nullptr; config(DEL_COMMAND); gFdbOrch=fdb;
    EXPECT_EQ(status(),BpProtectionStatus::NONE); expectPreserved(); config(DEL_COMMAND); expectPreserved();
    EXPECT_EQ(protection->isProtectedBridgePort("PortChannel1"),BpProtectionStatus::NONE);
    Port other("PortChannel1",Port::LAG); other.m_bridge_port_id=0x999; other.m_oper_status=SAI_PORT_OPER_STATUS_DOWN;
    gFdbOrch->updatePortOperState({other,SAI_PORT_OPER_STATUS_DOWN}); EXPECT_EQ(protectionFdbCalls.size(),1u); EXPECT_TRUE(listener.events.empty());
}


TEST_F(BpProtectionFdbTest, DeferredFlushUsesFreshPopulatedLagSnapshotAndCurrentBridgeOid)
{
    config(); down(); expectPreserved();
    auto current=port(); current.m_bridge_port_id=0xdef; ++current.m_bridge_port_generation;
    current.m_members={"Ethernet0","Ethernet4"}; current.m_child_ports={"PortChannel0.1000"};
    current.m_media_type="platform-reported-media";
    gPortsOrch->setPort(current.m_alias,current);
    for (auto &entry:gFdbOrch->m_entries) if (entry.first.port_name==current.m_alias) entry.second.bridge_port_id=current.m_bridge_port_id;
    config(DEL_COMMAND); expectNormal();
    EXPECT_EQ(port().m_members,current.m_members); EXPECT_EQ(port().m_child_ports,current.m_child_ports);
    EXPECT_EQ(port().m_media_type,current.m_media_type);
}

TEST_F(BpProtectionFdbTest, RevertiveFlapsAndFailedRestorePreserveFdbAndNeighbors)
{
    start("AUTO", true, 50); path("PRIMARY");
    down(); path("PROTECTION"); expectPreserved();
    link(true); tick(49); path("PROTECTION");
    down(); EXPECT_EQ(wtrStatus(), "IDLE"); tick(100); path("PROTECTION");
    link(true); bridgeFault = "admin"; tick(50);
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    EXPECT_EQ(state().at("protection_state"), "PROTECTION");
    EXPECT_EQ(desired(), "PRIMARY"); EXPECT_EQ(refs(), 1u); expectPreserved();
    bridgeFault.clear(); tick(5000); path("PRIMARY"); expectPreserved();
    EXPECT_EQ(wtrStatus(), "IDLE");
}

TEST_F(BpProtectionFdbTest, NonRevertiveManualAndAutoTransitionsKeepPrimaryEntries)
{
    start("AUTO", false); const auto oid = runtime().reference.oid;
    for (unsigned flap = 0; flap != 3; ++flap)
    {
        down(); path("PROTECTION"); link(true); tick(5000);
        path("PROTECTION"); expectPreserved();
        EXPECT_EQ(runtime().reference.oid, oid); EXPECT_EQ(wtrStatus(), "IDLE");
    }
    policy("PRIMARY", false); path("PRIMARY");
    policy("AUTO", false); path("PRIMARY");
    down(); path("PROTECTION"); expectPreserved();
    EXPECT_EQ(runtime().reference.oid, oid);
}

TEST_F(BpProtectionFdbTest, FinalWithdrawalDuringWtrCancelsTimerAndRestoresNormalDownFlush)
{
    start("AUTO", true, 50); down(); link(true);
    ASSERT_EQ(wtrStatus(), "WAIT_TO_RESTORE"); expectPreserved();
    config(DEL_COMMAND); EXPECT_EQ(status(), BpProtectionStatus::PENDING);
    apply(APP_BP_PROTECTION_BINDING_TABLE_NAME, DEL_COMMAND);
    EXPECT_EQ(status(), BpProtectionStatus::NONE); EXPECT_TRUE(state().empty());
    expectPreserved(); const auto writes = bridgeWrites.size();
    tick(5000); EXPECT_EQ(bridgeWrites.size(), writes); expectPreserved();
    down(); expectNormal(); tick(5000); expectNormal();
    EXPECT_EQ(bridgeWrites.size(), writes);
}

TEST_F(BpProtectionFdbTest, FailedGroupRetirementAndOrderedRebuildPreservePrimaryEntries)
{
    start("AUTO", false); down(); path("PROTECTION");
    const auto oid = runtime().reference.oid; bridgeFault = "clear";
    builder->deleteProtectionRow("2147483649");
    builder->deleteProtectionRow("1073741825"); step();
    EXPECT_EQ(refs(), 1u); EXPECT_EQ(status(), BpProtectionStatus::PENDING);
    EXPECT_EQ(state().at("reconciliation_state"), "INCONSISTENT");
    expectPreserved(); step(); EXPECT_EQ(refs(), 1u);
    bridgeFault.clear(); tick(5000); step(); step();
    ASSERT_FALSE(builder->m_protectionGroups.count("2147483649"));
    expectPreserved(); member("1073741825"); group("2147483649", "1073741825");
    step(); protection->doTask(); path("PROTECTION");
    EXPECT_NE(runtime().reference.oid, oid);
    EXPECT_EQ(status(), BpProtectionStatus::PROTECTED); expectPreserved();
}
