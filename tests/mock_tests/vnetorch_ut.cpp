#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_sai_tunnel.h"
#include "mock_orch_test.h"
#include "common/mock_test_helpers.h"
#include "mock_table.h"
#include "macaddress.h"
#include "sai_serialize.h"
#include "saihelper.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <set>

EXTERN_MOCK_FNS

// Injected redis pub-sub reply used to drive gBfdOrch's BFD state-change
// notification consumer, standing in for the ASIC_DB NOTIFICATIONS channel the
// VS test writes with a NotificationProducer (vnet_lib.update_bfd_session_state).
extern redisReply *mockReply;

namespace vnetorch_test
{
    // Free the hand-built pattern-message reply tree assigned to the global
    // mockReply. redisGetReply() hands the notification consumer a *deep copy*
    // (mock_hiredis.cpp), so the mock never takes ownership of this original --
    // clearing the pointer without freeing leaks the tree (array + element structs
    // + their strings) on every injected notification. free(nullptr) is a no-op,
    // so the unset array slots / top-level str are handled safely.
    static void freeMockReply()
    {
        if (mockReply == nullptr)
        {
            return;
        }
        if (mockReply->element != nullptr)
        {
            for (size_t i = 0; i < mockReply->elements; i++)
            {
                if (mockReply->element[i] != nullptr)
                {
                    free(mockReply->element[i]->str);
                    free(mockReply->element[i]);
                }
            }
            free(mockReply->element);
        }
        free(mockReply->str);
        free(mockReply);
        mockReply = nullptr;
    }

    // VNetOrch programs a SAI virtual router per (non-default-scope) VNET via
    // create/set/remove with no bulk ops, so the WITH_SET generic mock variant
    // fits (same shape as sai_policer_api).
    DEFINE_SAI_GENERIC_API_MOCK_WITH_SET(virtual_router, virtual_router);
    // VNetRouteOrch programs tunnel routes with non-bulk single-object SAI
    // calls, so the plain generic mocks fit (next hop, next hop group + members,
    // and the route entry). All default to call-through to real libsaivs so the
    // route -> next hop -> tunnel object references resolve to valid OIDs.
    DEFINE_SAI_GENERIC_API_MOCK(next_hop, next_hop);
    DEFINE_SAI_GENERIC_APIS_MOCK(next_hop_group, next_hop_group, next_hop_group_member);
    DEFINE_SAI_API_MOCK_MATCH_ENTRY(route);
    // gBfdOrch programs a SAI BFD session per endpoint monitor for a monitored
    // VNET route (create/remove, no bulk ops). Default call-through to real
    // libsaivs so the session gets a valid OID, which is also the id a
    // bfd_session_state_change notification must carry.
    DEFINE_SAI_GENERIC_API_MOCK(bfd, bfd_session);

    using namespace ::testing;
    using namespace std;
    using namespace swss;
    using namespace mock_orch_test;

    // The mock harness has no populated ASIC_DB (no syncd), so -- as with the
    // other ported suites -- we verify what VNetOrch programs by capturing the
    // attributes it passes to the mocked SAI virtual-router API rather than
    // reading ASIC_DB. In the VS test this is check_vnet_entry() asserting a new
    // SAI_OBJECT_TYPE_VIRTUAL_ROUTER exists for the VNET.
    struct VirtualRouterSaiMock
    {
        int create_count = 0;
        sai_object_id_t created_oid = SAI_NULL_OBJECT_ID;
        vector<sai_attribute_t> create_attrs;
        sai_object_id_t removed_oid = SAI_NULL_OBJECT_ID;

        sai_status_t handleCreate(sai_object_id_t *vr_id, sai_object_id_t switch_id,
                                  uint32_t attr_count, const sai_attribute_t *attr_list)
        {
            create_attrs.assign(attr_list, attr_list + attr_count);
            // Call through to real libsaivs so a valid VR OID is returned. A
            // fabricated OID makes downstream real SAI calls on this VR --
            // e.g. RouteOrch::addLinkLocalRouteToMe()'s create_route_entry() --
            // fail, which throws and aborts the VNET bind before any tunnel
            // objects are programmed.
            sai_status_t status = old_sai_virtual_router_api->create_virtual_router(
                vr_id, switch_id, attr_count, attr_list);
            if (status == SAI_STATUS_SUCCESS)
            {
                created_oid = *vr_id;
                create_count++;
            }
            return status;
        }

        sai_status_t handleRemove(sai_object_id_t vr_id)
        {
            removed_oid = vr_id;
            return SAI_STATUS_SUCCESS;
        }
    };

    // Raw (list,count) attribute lookup -- the capture handlers see the C SAI
    // signature, not a vector, so mock_test_helpers::findAttr doesn't apply.
    static const sai_attribute_t *findRawAttr(const sai_attribute_t *list,
                                              uint32_t count, sai_attr_id_t id)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            if (list[i].id == id)
            {
                return &list[i];
            }
        }
        return nullptr;
    }

    static bool ipAddrEquals(const sai_ip_address_t &a, const string &ip)
    {
        swss::IpAddress expected(ip);
        if (expected.isV4())
        {
            return a.addr_family == SAI_IP_ADDR_FAMILY_IPV4 &&
                   a.addr.ip4 == expected.getV4Addr();
        }
        return a.addr_family == SAI_IP_ADDR_FAMILY_IPV6 &&
               memcmp(a.addr.ip6, expected.getV6Addr(), sizeof(a.addr.ip6)) == 0;
    }

    static bool saiMacEquals(const sai_mac_t &m, const string &mac)
    {
        swss::MacAddress expected(mac);
        return memcmp(m, expected.getMac(), sizeof(sai_mac_t)) == 0;
    }

    static bool prefixAddrEquals(const sai_ip_prefix_t &p, const string &ip)
    {
        swss::IpAddress expected(ip);
        if (expected.isV4())
        {
            return p.addr_family == SAI_IP_ADDR_FAMILY_IPV4 &&
                   p.addr.ip4 == expected.getV4Addr();
        }
        return p.addr_family == SAI_IP_ADDR_FAMILY_IPV6 &&
               memcmp(p.addr.ip6, expected.getV6Addr(), sizeof(p.addr.ip6)) == 0;
    }

    // Captured VXLAN tunnel SAI objects with the attributes VNetOrch /
    // VxlanTunnelOrch program -- the mock-test equivalent of the ASIC_DB reads
    // in vnet_lib.check_vxlan_tunnel() / check_vxlan_tunnel_entry().
    struct TunnelCaptures
    {
        struct Map { sai_object_id_t oid; int32_t type; };
        struct Tunnel
        {
            sai_object_id_t oid;
            int32_t type = -1;
            sai_object_id_t underlay = SAI_NULL_OBJECT_ID;
            sai_ip_address_t src{};
            vector<sai_object_id_t> decap_mappers;
            vector<sai_object_id_t> encap_mappers;
            // IPINIP subnet-decap tunnels carry decap DSCP/ECN/TTL modes; -1
            // means the attribute was not present at create time.
            int32_t dscp_mode = -1;
            int32_t ecn_mode = -1;
            int32_t ttl_mode = -1;
        };
        struct Term
        {
            sai_object_id_t oid;
            int32_t type = -1;
            int32_t tunnel_type = -1;
            sai_object_id_t vr = SAI_NULL_OBJECT_ID;
            sai_object_id_t action_tunnel = SAI_NULL_OBJECT_ID;
            sai_ip_address_t dst{};
            // MP2MP subnet-decap terms additionally set the dst mask and the
            // src network + mask; P2P/P2MP terms leave these default.
            sai_ip_address_t dst_mask{};
            sai_ip_address_t src{};
            sai_ip_address_t src_mask{};
        };
        struct MapEntry
        {
            sai_object_id_t oid;
            int32_t map_type = -1;
            sai_object_id_t tunnel_map = SAI_NULL_OBJECT_ID;
            sai_object_id_t vr_key = SAI_NULL_OBJECT_ID;
            sai_object_id_t vr_value = SAI_NULL_OBJECT_ID;
            uint32_t vni_key = 0;
            uint32_t vni_value = 0;
            // VNI_TO_VLAN_ID decap map entries (VxlanTunnelMapOrch) carry a VLAN
            // in the value slot; other map types leave these zero.
            uint16_t vlan_key = 0;
            uint16_t vlan_value = 0;
        };
        vector<Map> maps;
        vector<Tunnel> tunnels;
        vector<Term> terms;
        vector<MapEntry> mapEntries;
        vector<sai_object_id_t> removedTunnels;
        vector<sai_object_id_t> removedMaps;
        vector<sai_object_id_t> removedTerms;
        vector<sai_object_id_t> removedMapEntries;
    };

    // Captured SAI next-hop / next-hop-group / route objects VNetRouteOrch
    // programs for a VNET tunnel route -- the mock-test equivalent of the
    // ASIC_DB reads in vnet_lib.check_vnet_routes() / check_vnet_ecmp_routes().
    struct RouteCaptures
    {
        struct NextHop
        {
            sai_object_id_t oid = SAI_NULL_OBJECT_ID;
            int32_t type = -1;
            sai_ip_address_t ip{};
            sai_object_id_t tunnel_id = SAI_NULL_OBJECT_ID;
            sai_object_id_t rif = SAI_NULL_OBJECT_ID;
            bool has_vni = false;
            uint32_t vni = 0;
            bool has_mac = false;
            sai_mac_t mac{};
        };
        struct Group { sai_object_id_t oid = SAI_NULL_OBJECT_ID; int32_t type = -1; };
        struct Member
        {
            sai_object_id_t oid = SAI_NULL_OBJECT_ID;
            sai_object_id_t nhg = SAI_NULL_OBJECT_ID;
            sai_object_id_t nh = SAI_NULL_OBJECT_ID;
            bool has_seq = false;
            uint32_t seq = 0;
        };
        struct Route
        {
            sai_object_id_t vr = SAI_NULL_OBJECT_ID;
            sai_ip_prefix_t dest{};
            sai_object_id_t next_hop_id = SAI_NULL_OBJECT_ID;
            int32_t packet_action = -1;
        };
        vector<NextHop> nexthops;
        vector<Group> groups;
        vector<Member> members;
        vector<Route> routes;
        vector<sai_object_id_t> removedNexthops;
        vector<sai_object_id_t> removedGroups;
        vector<sai_object_id_t> removedMembers;
        vector<sai_ip_prefix_t> removedRoutes;
        // Per-entry statuses libsaivs returned for the most recent bulk route
        // create -- used by the duplicate-route regression test to prove the
        // conflicting create actually hit an ITEM_ALREADY_EXISTS/NOT_EXECUTED.
        vector<sai_status_t> lastBulkCreateStatuses;
    };

    // Captured SAI BFD sessions gBfdOrch programs for a monitored VNET route's
    // endpoint monitors -- the mock equivalent of vnet_lib.get_bfd_session_id(),
    // which finds a session by its SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS (the
    // monitor address) with MULTIHOP=true. The OID is retained so a
    // bfd_session_state_change notification can target the right session.
    struct BfdCaptures
    {
        struct Session { sai_object_id_t oid = SAI_NULL_OBJECT_ID; sai_ip_address_t dst{}; };
        vector<Session> sessions;
        vector<sai_object_id_t> removed;
    };

    // set_route_entry_attribute is not routed through the mock_sai_api framework
    // (its macro only mocks create/remove), yet VNetRouteOrch::update_route
    // repoints a route in place via this SET when an ECMP route's endpoint set
    // changes. We swap the single function pointer to a trampoline that records
    // the new NEXT_HOP_ID onto the matching captured route (so a captured
    // route's next_hop_id reflects its current value, mirroring the ASIC_DB read
    // in check_vnet_ecmp_routes), then calls through to real libsaivs. gtest
    // runs tests serially, so a file-scope active pointer is safe.
    static RouteCaptures *g_activeRouteCaptures = nullptr;
    static sai_status_t (*g_savedSetRouteAttr)(const sai_route_entry_t *,
                                               const sai_attribute_t *) = nullptr;

    static bool saiIpPrefixEquals(const sai_ip_prefix_t &a, const sai_ip_prefix_t &b)
    {
        if (a.addr_family != b.addr_family) return false;
        if (a.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
        {
            return a.addr.ip4 == b.addr.ip4 && a.mask.ip4 == b.mask.ip4;
        }
        return memcmp(a.addr.ip6, b.addr.ip6, sizeof(a.addr.ip6)) == 0 &&
               memcmp(a.mask.ip6, b.mask.ip6, sizeof(a.mask.ip6)) == 0;
    }

    static sai_status_t captureSetRouteEntryAttr(const sai_route_entry_t *e,
                                                 const sai_attribute_t *attr)
    {
        if (g_activeRouteCaptures && e && attr &&
            attr->id == SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID)
        {
            for (auto &r : g_activeRouteCaptures->routes)
            {
                if (r.vr == e->vr_id && saiIpPrefixEquals(r.dest, e->destination))
                {
                    r.next_hop_id = attr->value.oid;
                }
            }
        }
        return g_savedSetRouteAttr(e, attr);
    }

    // gRouteOrch programs VNET *local* routes through its route bulker, which
    // copies the SAI create/remove_route_entries function pointers at
    // construction time (bulker.h) -- i.e. before PostSetUp() can apply the SAI
    // mock. So, like captureSetRouteEntryAttr above, these bulk entries are
    // captured with a plain function-pointer trampoline installed early in
    // ApplySaiMock() (before gRouteOrch is constructed) rather than via gmock.
    static sai_status_t (*g_savedCreateRouteEntries)(
        uint32_t, const sai_route_entry_t *, const uint32_t *,
        const sai_attribute_t **, sai_bulk_op_error_mode_t, sai_status_t *) = nullptr;
    static sai_status_t (*g_savedRemoveRouteEntries)(
        uint32_t, const sai_route_entry_t *, sai_bulk_op_error_mode_t,
        sai_status_t *) = nullptr;

    static sai_status_t captureCreateRouteEntries(
        uint32_t count, const sai_route_entry_t *entries, const uint32_t *attr_count,
        const sai_attribute_t **attr_list, sai_bulk_op_error_mode_t mode,
        sai_status_t *statuses)
    {
        sai_status_t st = g_savedCreateRouteEntries(count, entries, attr_count,
                                                    attr_list, mode, statuses);
        if (g_activeRouteCaptures)
        {
            g_activeRouteCaptures->lastBulkCreateStatuses.assign(statuses, statuses + count);
            for (uint32_t i = 0; i < count; ++i)
            {
                if (statuses[i] != SAI_STATUS_SUCCESS) continue;
                RouteCaptures::Route r;
                r.vr = entries[i].vr_id;
                r.dest = entries[i].destination;
                if (auto a = findRawAttr(attr_list[i], attr_count[i],
                                         SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID))
                    r.next_hop_id = a->value.oid;
                g_activeRouteCaptures->routes.push_back(r);
            }
        }
        return st;
    }

    static sai_status_t captureRemoveRouteEntries(
        uint32_t count, const sai_route_entry_t *entries,
        sai_bulk_op_error_mode_t mode, sai_status_t *statuses)
    {
        sai_status_t st = g_savedRemoveRouteEntries(count, entries, mode, statuses);
        if (g_activeRouteCaptures)
        {
            for (uint32_t i = 0; i < count; ++i)
                if (statuses[i] == SAI_STATUS_SUCCESS)
                    g_activeRouteCaptures->removedRoutes.push_back(entries[i].destination);
        }
        return st;
    }

    // The route bulker also binds set_route_entries_attribute at construction, so
    // an in-place local-route repoint (NEXT_HOP_ID change when an ECMP endpoint
    // set changes) flows through this bulk API rather than the single
    // set_route_entry_attribute captureSetRouteEntryAttr intercepts. Trampoline it
    // early too, updating the matching captured route's next_hop_id.
    static sai_status_t (*g_savedSetRouteEntries)(
        uint32_t, const sai_route_entry_t *, const sai_attribute_t *,
        sai_bulk_op_error_mode_t, sai_status_t *) = nullptr;

    static sai_status_t captureSetRouteEntries(
        uint32_t count, const sai_route_entry_t *entries,
        const sai_attribute_t *attr_list, sai_bulk_op_error_mode_t mode,
        sai_status_t *statuses)
    {
        sai_status_t st = g_savedSetRouteEntries(count, entries, attr_list, mode, statuses);
        if (g_activeRouteCaptures)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                if (attr_list[i].id != SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID) continue;
                for (auto &r : g_activeRouteCaptures->routes)
                    if (r.vr == entries[i].vr_id &&
                        saiIpPrefixEquals(r.dest, entries[i].destination))
                        r.next_hop_id = attr_list[i].value.oid;
            }
        }
        return st;
    }

    // gRouteOrch programs an ECMP local route's group members through its
    // next-hop-group-member *object* bulker (gNextHopGroupMemberBulker), which --
    // like the route bulker -- binds create/remove_next_hop_group_members at
    // construction. Tunnel-route members go through VNetRouteOrch's single-object
    // creates (captured by installRouteMock), but local ECMP members need the
    // same early-trampoline treatment, installed in ApplySaiMock().
    static sai_status_t (*g_savedCreateNhgMembers)(
        sai_object_id_t, uint32_t, const uint32_t *, const sai_attribute_t **,
        sai_bulk_op_error_mode_t, sai_object_id_t *, sai_status_t *) = nullptr;
    static sai_status_t (*g_savedRemoveNhgMembers)(
        uint32_t, const sai_object_id_t *, sai_bulk_op_error_mode_t,
        sai_status_t *) = nullptr;

    static sai_status_t captureCreateNhgMembers(
        sai_object_id_t switch_id, uint32_t count, const uint32_t *attr_count,
        const sai_attribute_t **attr_list, sai_bulk_op_error_mode_t mode,
        sai_object_id_t *object_id, sai_status_t *statuses)
    {
        sai_status_t st = g_savedCreateNhgMembers(switch_id, count, attr_count,
                                                  attr_list, mode, object_id, statuses);
        if (g_activeRouteCaptures)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                if (statuses[i] != SAI_STATUS_SUCCESS) continue;
                RouteCaptures::Member m;
                m.oid = object_id[i];
                if (auto a = findRawAttr(attr_list[i], attr_count[i],
                                         SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID))
                    m.nhg = a->value.oid;
                if (auto a = findRawAttr(attr_list[i], attr_count[i],
                                         SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID))
                    m.nh = a->value.oid;
                if (auto a = findRawAttr(attr_list[i], attr_count[i],
                                         SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID))
                {
                    m.has_seq = true;
                    m.seq = a->value.u32;
                }
                g_activeRouteCaptures->members.push_back(m);
            }
        }
        return st;
    }

    static sai_status_t captureRemoveNhgMembers(
        uint32_t count, const sai_object_id_t *object_id,
        sai_bulk_op_error_mode_t mode, sai_status_t *statuses)
    {
        sai_status_t st = g_savedRemoveNhgMembers(count, object_id, mode, statuses);
        if (g_activeRouteCaptures)
        {
            for (uint32_t i = 0; i < count; ++i)
                if (statuses[i] == SAI_STATUS_SUCCESS)
                    g_activeRouteCaptures->removedMembers.push_back(object_id[i]);
        }
        return st;
    }

    class VNetOrchTest : public MockOrchTest
    {
    protected:
        unique_ptr<VirtualRouterSaiMock> m_vrMock;

        // sai_tunnel_api is not routed through the mock_sai_api framework (which
        // swaps whole api structs); VxlanTunnelOrch uses the free create/remove
        // helpers, so we swap the individual function pointers to the ready-made
        // mock_sai_tunnel shims and default every call to pass through to real
        // libsaivs (so tunnel objects get valid OIDs and the map/term chain
        // succeeds) while capturing the attributes VNetOrch programs.
        NiceMock<MockSaiTunnel> m_tunnelMock;
        decltype(sai_tunnel_api->create_tunnel) m_savedCreateTunnel{};
        decltype(sai_tunnel_api->create_tunnel_map) m_savedCreateTunnelMap{};
        decltype(sai_tunnel_api->create_tunnel_map_entry) m_savedCreateTunnelMapEntry{};
        decltype(sai_tunnel_api->create_tunnel_term_table_entry) m_savedCreateTunnelTerm{};
        decltype(sai_tunnel_api->remove_tunnel) m_savedRemoveTunnel{};
        decltype(sai_tunnel_api->remove_tunnel_map) m_savedRemoveTunnelMap{};
        decltype(sai_tunnel_api->remove_tunnel_map_entry) m_savedRemoveTunnelMapEntry{};
        decltype(sai_tunnel_api->remove_tunnel_term_table_entry) m_savedRemoveTunnelTerm{};
        TunnelCaptures m_tun;

        unique_ptr<VNetRouteOrch> m_vnetRouteOrch;
        // Mirrors CONFIG_DB VNET_ROUTE / VNET_ROUTE_TUNNEL into the APP_DB tables
        // VNetRouteOrch consumes, exactly as orchdaemon's VNetCfgRouteOrch does
        // (orchdaemon.cpp:307). The route SET helpers drive routes through this
        // real CONFIG->APP path, matching the VS test's create_vnet_routes /
        // create_vnet_local_routes (which write CONFIG_DB VNET_ROUTE[_TUNNEL]).
        unique_ptr<VNetCfgRouteOrch> m_vnetCfgRouteOrch;
        RouteCaptures m_rt;

        // Consumes STATE_DB VNET_MONITOR_TABLE (the table update_monitor_session_
        // state() writes) and forwards each up/down to VNetRouteOrch, exactly as
        // orchdaemon's gMonitorOrch does. Custom-monitored (non-BFD) priority
        // routes are driven entirely through this path.
        unique_ptr<MonitorOrch> m_monitorOrch;

        // Consumes STATE_DB BFD_SESSION_TABLE (which gBfdOrch mirrors each BFD
        // session state into) and forwards every up/down to VNetRouteOrch's
        // updateCustomBfdState(), exactly as orchdaemon's gBfdMonitorOrch does.
        // custom_bfd-monitored routes react ONLY through this path: the SAI
        // notification observer (VNetRouteOrch::updateVnetTunnel) explicitly
        // skips custom_bfd sessions (vnetorch.cpp), so a bfd_session_state_change
        // alone never activates them -- BfdMonitorOrch must consume the STATE_DB
        // row gBfdOrch writes.
        unique_ptr<BfdMonitorOrch> m_bfdMonitorOrch;

        // Consumes APP_DB VXLAN_TUNNEL_MAP_TABLE (the table vxlanmgrd produces
        // from CONFIG_DB VXLAN_TUNNEL_MAP), standing in for orchdaemon's
        // gVxlanTunnelMapOrch. Programs the explicit VLAN<->VNI decap tunnel-map
        // entries test_vnet_vxlan_multi_map adds on top of a VNET-bound tunnel.
        unique_ptr<VxlanTunnelMapOrch> m_vxlanTunnelMapOrch;

        // BfdOrch::doTask() consults gDirectory for a BgpGlobalStateOrch and,
        // when it is absent, defaults to *software* BFD -- programming STATE_DB
        // instead of creating a SAI hardware session. orchdaemon always
        // registers one, so the fixture does too (deleted in PreTearDown before
        // gDirectory is cleared); with it present getSoftwareBfd() reports the
        // switch's real (hardware) BFD-offload capability, matching the DVS.
        BgpGlobalStateOrch *m_bgpGlobalStateOrch = nullptr;

        // Captured BFD sessions + the APP_BFD_SESSION_TABLE keys already
        // delivered to gBfdOrch, so syncBfd() can diff and deliver only new
        // SETs / disappeared DELs.
        BfdCaptures m_bfd;
        set<string> m_deliveredBfdKeys;

        // APP_TUNNEL_DECAP_TERM_TABLE keys already delivered to gTunneldecapOrch,
        // so syncTunnelDecapTerm() can diff VNetRouteOrch's subnet-decap-term
        // producer writes the same way syncBfd() diffs BFD sessions.
        set<string> m_deliveredDecapTermKeys;

        void SetUp() override
        {
            // gDB (the mock DB backing swss::Table) is process-global and is not
            // flushed by MockOrchTest::TearDown(), so CONFIG/APP rows a test
            // writes (VNET, VXLAN_TUNNEL, VNET_ROUTE_TUNNEL...) would linger and
            // be re-consumed by the next test's addExistingData()+doTask(),
            // inflating the captured SAI object counts. Reset before each test.
            testing_db::reset();
            MockOrchTest::SetUp();
        }

        // Runs inside PrepareSai(), before any Orch (incl. gRouteOrch) is built.
        // gRouteOrch's route bulker copies the SAI create/remove_route_entries
        // function pointers at construction (bulker.h), so to capture the VNET
        // *local* routes it programs in bulk we must install our trampolines on
        // the real sai_route_api here -- PostSetUp() (where the gmock swap runs)
        // is too late, as the bulker already holds the pre-mock pointers.
        void ApplySaiMock() override
        {
            g_savedCreateRouteEntries = sai_route_api->create_route_entries;
            g_savedRemoveRouteEntries = sai_route_api->remove_route_entries;
            g_savedSetRouteEntries = sai_route_api->set_route_entries_attribute;
            sai_route_api->create_route_entries = captureCreateRouteEntries;
            sai_route_api->remove_route_entries = captureRemoveRouteEntries;
            sai_route_api->set_route_entries_attribute = captureSetRouteEntries;

            g_savedCreateNhgMembers = sai_next_hop_group_api->create_next_hop_group_members;
            g_savedRemoveNhgMembers = sai_next_hop_group_api->remove_next_hop_group_members;
            sai_next_hop_group_api->create_next_hop_group_members = captureCreateNhgMembers;
            sai_next_hop_group_api->remove_next_hop_group_members = captureRemoveNhgMembers;
        }

        void ApplyInitialConfigs() override
        {
            // VNetOrch/VxlanTunnelOrch depend on ports being ready, so bring the
            // default SAI ports up first (same idiom as the other suites).
            Table port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
            auto ports = ut_helper::getInitialSaiPorts();
            for (const auto &it : ports)
            {
                port_table.set(it.first, it.second);
            }
            port_table.set("PortConfigDone", {{"count", to_string(ports.size())}});
            port_table.set("PortInitDone", {{}});
            gPortsOrch->addExistingData(&port_table);
            static_cast<Orch *>(gPortsOrch)->doTask();
        }

        void PostSetUp() override
        {
            INIT_SAI_API_MOCK(virtual_router);
            INIT_SAI_API_MOCK(next_hop);
            INIT_SAI_API_MOCK(next_hop_group);
            INIT_SAI_API_MOCK(route);
            INIT_SAI_API_MOCK(bfd);
            MockSaiApis();
            m_vrMock = make_unique<VirtualRouterSaiMock>();
            // Record every virtual-router create (the VNET's, plus any the route
            // path drives) so created_oid always reflects the latest VR. Route
            // tests capture the VNET's VR right after setVnet(), before any other.
            ON_CALL(*mock_sai_virtual_router_api, create_virtual_router)
                .WillByDefault(Invoke(m_vrMock.get(), &VirtualRouterSaiMock::handleCreate));

            installTunnelMock();
            installRouteMock();
            installBfdMock();

            // BfdOrch::doTask() falls back to software BFD unless a
            // BgpGlobalStateOrch is registered in gDirectory (see the member
            // comment). Register one before gBfdOrch consumes anything so
            // monitored routes drive real SAI BFD sessions.
            m_bgpGlobalStateOrch = new BgpGlobalStateOrch(
                m_config_db.get(), CFG_BGP_DEVICE_GLOBAL_TABLE_NAME);
            gDirectory.set(m_bgpGlobalStateOrch);

            // VNetRouteOrch's ctor calls gBfdOrch->attach(this) with no null
            // guard, so gBfdOrch must exist first. VNetRouteOrch has no dtor of
            // its own (never detaches), so gBfdOrch must outlive it -- see
            // PreTearDown for the matching teardown order.
            TableConnector stateDbBfdSessionTable(m_state_db.get(), STATE_BFD_SESSION_TABLE_NAME);
            gBfdOrch = new BfdOrch(m_app_db.get(), APP_BFD_SESSION_TABLE_NAME, stateDbBfdSessionTable);

            // BgpGlobalStateOrch::doTask() resolves BfdOrch via gDirectory to
            // drive handleTsaStateChange() on a TSA state change, so register
            // gBfdOrch the same way orchdaemon does (orchdaemon.cpp:269).
            gDirectory.set(gBfdOrch);

            // VNetRouteOrch::postRouteState() dereferences gTunneldecapOrch
            // (getSubnetDecapConfig()) on every tunnel route, so it must exist
            // before any route task runs. Subnet decap is disabled by default,
            // so no decap term is programmed -- it just needs to be non-null.
            vector<string> tunnel_tables = {
                APP_TUNNEL_DECAP_TABLE_NAME, APP_TUNNEL_DECAP_TERM_TABLE_NAME};
            gTunneldecapOrch = new TunnelDecapOrch(
                m_app_db.get(), m_state_db.get(), m_config_db.get(), tunnel_tables);

            // Drive VNetRouteOrch directly off the APP_DB VNET_ROUTE(_TUNNEL)
            // tables, standing in for VNetCfgRouteOrch's CONFIG->APP translation
            // (the same tables orchdaemon wires it to).
            vector<string> vnet_route_tables = {
                APP_VNET_RT_TABLE_NAME, APP_VNET_RT_TUNNEL_TABLE_NAME};
            m_vnetRouteOrch = make_unique<VNetRouteOrch>(
                m_app_db.get(), vnet_route_tables, m_vnetOrch);

            // MonitorOrch::addOperation resolves VNetRouteOrch via gDirectory
            // (as does VNetOrch on an overlay_dmac change), so register it the
            // same way orchdaemon does. gDirectory is cleared by the base
            // teardown after PreTearDown, so no stale pointer survives the test.
            gDirectory.set(m_vnetRouteOrch.get());

            // The real CONFIG->APP translation for VNET routes
            // (orchdaemon.cpp:307). The route SET helpers write CONFIG_DB
            // VNET_ROUTE[_TUNNEL] and drive this orch, which mirrors each row to
            // the APP_DB VNET_ROUTE[_TUNNEL]_TABLE that VNetRouteOrch consumes --
            // so the mock exercises doVnetRouteTask/doVnetTunnelRouteTask exactly
            // as the VS test's create_vnet_routes/create_vnet_local_routes did.
            vector<string> cfg_vnet_route_tables = {
                CFG_VNET_RT_TABLE_NAME, CFG_VNET_RT_TUNNEL_TABLE_NAME};
            m_vnetCfgRouteOrch = make_unique<VNetCfgRouteOrch>(
                m_config_db.get(), m_app_db.get(), cfg_vnet_route_tables);

            m_monitorOrch = make_unique<MonitorOrch>(
                m_state_db.get(), STATE_VNET_MONITOR_TABLE_NAME);

            // custom_bfd routes are driven off STATE_DB BFD_SESSION_TABLE (see
            // the member comment). It resolves VNetRouteOrch via gDirectory
            // (already set above), so no registration of its own is needed.
            m_bfdMonitorOrch = make_unique<BfdMonitorOrch>(
                m_state_db.get(), STATE_BFD_SESSION_TABLE_NAME);

            // gVxlanTunnelMapOrch equivalent, driven off APP_DB
            // VXLAN_TUNNEL_MAP_TABLE. Registered in gDirectory the same way
            // orchdaemon does; the VNET-bind tunnel path never resolves it (the
            // other 39 tests run without one), so this only affects explicit
            // tunnel-map programming.
            m_vxlanTunnelMapOrch = make_unique<VxlanTunnelMapOrch>(
                m_app_db.get(), APP_VXLAN_TUNNEL_MAP_TABLE_NAME);
            gDirectory.set(m_vxlanTunnelMapOrch.get());

            // FdbOrch observes port oper-status changes and, on a DOWN, calls
            // gMlagOrch->isMlagInterface(); orchdaemon always builds an MlagOrch,
            // so the fixture does too -- otherwise a port-down test segfaults on
            // the null gMlagOrch (fdborch.cpp:1722).
            vector<string> mlag_tables = {CFG_MCLAG_TABLE_NAME, CFG_MCLAG_INTF_TABLE_NAME};
            gMlagOrch = new MlagOrch(m_config_db.get(), mlag_tables);
        }

        void PreTearDown() override
        {
            // Tear the route orch down before gBfdOrch: it attached to gBfdOrch
            // in its ctor and relies on gBfdOrch staying alive until it is gone.
            m_vxlanTunnelMapOrch.reset();
            delete gMlagOrch;
            gMlagOrch = nullptr;
            m_bfdMonitorOrch.reset();
            m_monitorOrch.reset();
            m_vnetRouteOrch.reset();
            m_vnetCfgRouteOrch.reset();
            delete gBfdOrch;
            gBfdOrch = nullptr;
            delete gTunneldecapOrch;
            gTunneldecapOrch = nullptr;
            delete m_bgpGlobalStateOrch;
            m_bgpGlobalStateOrch = nullptr;

            restoreTunnelMock();
            // Restore the route SET pointer before the mock framework swaps the
            // route api struct back (symmetric with installRouteMock()).
            if (g_savedSetRouteAttr)
            {
                sai_route_api->set_route_entry_attribute = g_savedSetRouteAttr;
                g_savedSetRouteAttr = nullptr;
            }
            g_activeRouteCaptures = nullptr;
            m_vrMock.reset();
            RestoreSaiApis();
            // RestoreSaiApis() repointed sai_route_api back at the real struct
            // the bulker trampolines were installed on in ApplySaiMock(); undo
            // them so the next test re-saves the genuine libsaivs pointers (the
            // struct is a process-global libsaivs singleton, reused every test).
            if (g_savedCreateRouteEntries)
            {
                sai_route_api->create_route_entries = g_savedCreateRouteEntries;
                sai_route_api->remove_route_entries = g_savedRemoveRouteEntries;
                sai_route_api->set_route_entries_attribute = g_savedSetRouteEntries;
                g_savedCreateRouteEntries = nullptr;
                g_savedRemoveRouteEntries = nullptr;
                g_savedSetRouteEntries = nullptr;
            }
            if (g_savedCreateNhgMembers)
            {
                sai_next_hop_group_api->create_next_hop_group_members = g_savedCreateNhgMembers;
                sai_next_hop_group_api->remove_next_hop_group_members = g_savedRemoveNhgMembers;
                g_savedCreateNhgMembers = nullptr;
                g_savedRemoveNhgMembers = nullptr;
            }
            DEINIT_SAI_API_MOCK(virtual_router);
            DEINIT_SAI_API_MOCK(next_hop);
            DEINIT_SAI_API_MOCK(next_hop_group);
            DEINIT_SAI_API_MOCK(route);
            DEINIT_SAI_API_MOCK(bfd);
        }

        void installTunnelMock()
        {
            mock_sai_tunnel = &m_tunnelMock;

            m_savedCreateTunnel = sai_tunnel_api->create_tunnel;
            m_savedCreateTunnelMap = sai_tunnel_api->create_tunnel_map;
            m_savedCreateTunnelMapEntry = sai_tunnel_api->create_tunnel_map_entry;
            m_savedCreateTunnelTerm = sai_tunnel_api->create_tunnel_term_table_entry;
            m_savedRemoveTunnel = sai_tunnel_api->remove_tunnel;
            m_savedRemoveTunnelMap = sai_tunnel_api->remove_tunnel_map;
            m_savedRemoveTunnelMapEntry = sai_tunnel_api->remove_tunnel_map_entry;
            m_savedRemoveTunnelTerm = sai_tunnel_api->remove_tunnel_term_table_entry;

            sai_tunnel_api->create_tunnel = mock_create_tunnel;
            sai_tunnel_api->create_tunnel_map = mock_create_tunnel_map;
            sai_tunnel_api->create_tunnel_map_entry = mock_create_tunnel_map_entry;
            sai_tunnel_api->create_tunnel_term_table_entry = mock_create_tunnel_term_table_entry;
            sai_tunnel_api->remove_tunnel = mock_remove_tunnel;
            sai_tunnel_api->remove_tunnel_map = mock_remove_tunnel_map;
            sai_tunnel_api->remove_tunnel_map_entry = mock_remove_tunnel_map_entry;
            sai_tunnel_api->remove_tunnel_term_table_entry = mock_remove_tunnel_term_table_entry;

            ON_CALL(m_tunnelMock, create_tunnel(_, _, _, _))
                .WillByDefault(Invoke([this](sai_object_id_t *id, sai_object_id_t sw,
                                             uint32_t n, const sai_attribute_t *l) {
                    sai_status_t st = m_savedCreateTunnel(id, sw, n, l);
                    if (st == SAI_STATUS_SUCCESS)
                    {
                        TunnelCaptures::Tunnel t;
                        t.oid = *id;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_ATTR_TYPE)) t.type = a->value.s32;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE)) t.underlay = a->value.oid;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_ATTR_ENCAP_SRC_IP)) t.src = a->value.ipaddr;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_ATTR_DECAP_MAPPERS))
                            t.decap_mappers.assign(a->value.objlist.list, a->value.objlist.list + a->value.objlist.count);
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_ATTR_ENCAP_MAPPERS))
                            t.encap_mappers.assign(a->value.objlist.list, a->value.objlist.list + a->value.objlist.count);
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_ATTR_DECAP_DSCP_MODE)) t.dscp_mode = a->value.s32;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_ATTR_DECAP_ECN_MODE)) t.ecn_mode = a->value.s32;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_ATTR_DECAP_TTL_MODE)) t.ttl_mode = a->value.s32;
                        m_tun.tunnels.push_back(t);
                    }
                    return st;
                }));
            ON_CALL(m_tunnelMock, create_tunnel_map(_, _, _, _))
                .WillByDefault(Invoke([this](sai_object_id_t *id, sai_object_id_t sw,
                                             uint32_t n, const sai_attribute_t *l) {
                    sai_status_t st = m_savedCreateTunnelMap(id, sw, n, l);
                    if (st == SAI_STATUS_SUCCESS)
                    {
                        TunnelCaptures::Map m;
                        m.oid = *id;
                        auto a = findRawAttr(l, n, SAI_TUNNEL_MAP_ATTR_TYPE);
                        m.type = a ? a->value.s32 : -1;
                        m_tun.maps.push_back(m);
                    }
                    return st;
                }));
            ON_CALL(m_tunnelMock, create_tunnel_map_entry(_, _, _, _))
                .WillByDefault(Invoke([this](sai_object_id_t *id, sai_object_id_t sw,
                                             uint32_t n, const sai_attribute_t *l) {
                    sai_status_t st = m_savedCreateTunnelMapEntry(id, sw, n, l);
                    if (st == SAI_STATUS_SUCCESS)
                    {
                        TunnelCaptures::MapEntry e;
                        e.oid = *id;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE)) e.map_type = a->value.s32;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP)) e.tunnel_map = a->value.oid;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_KEY)) e.vr_key = a->value.oid;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_VALUE)) e.vr_value = a->value.oid;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY)) e.vni_key = a->value.u32;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE)) e.vni_value = a->value.u32;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_KEY)) e.vlan_key = a->value.u16;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE)) e.vlan_value = a->value.u16;
                        m_tun.mapEntries.push_back(e);
                    }
                    return st;
                }));
            ON_CALL(m_tunnelMock, create_tunnel_term_table_entry(_, _, _, _))
                .WillByDefault(Invoke([this](sai_object_id_t *id, sai_object_id_t sw,
                                             uint32_t n, const sai_attribute_t *l) {
                    sai_status_t st = m_savedCreateTunnelTerm(id, sw, n, l);
                    if (st == SAI_STATUS_SUCCESS)
                    {
                        TunnelCaptures::Term t;
                        t.oid = *id;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE)) t.type = a->value.s32;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE)) t.tunnel_type = a->value.s32;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID)) t.vr = a->value.oid;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID)) t.action_tunnel = a->value.oid;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP)) t.dst = a->value.ipaddr;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP_MASK)) t.dst_mask = a->value.ipaddr;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP)) t.src = a->value.ipaddr;
                        if (auto a = findRawAttr(l, n, SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP_MASK)) t.src_mask = a->value.ipaddr;
                        m_tun.terms.push_back(t);
                    }
                    return st;
                }));
            ON_CALL(m_tunnelMock, remove_tunnel(_))
                .WillByDefault(Invoke([this](sai_object_id_t id) { m_tun.removedTunnels.push_back(id); return m_savedRemoveTunnel(id); }));
            ON_CALL(m_tunnelMock, remove_tunnel_map(_))
                .WillByDefault(Invoke([this](sai_object_id_t id) { m_tun.removedMaps.push_back(id); return m_savedRemoveTunnelMap(id); }));
            ON_CALL(m_tunnelMock, remove_tunnel_map_entry(_))
                .WillByDefault(Invoke([this](sai_object_id_t id) { m_tun.removedMapEntries.push_back(id); return m_savedRemoveTunnelMapEntry(id); }));
            ON_CALL(m_tunnelMock, remove_tunnel_term_table_entry(_))
                .WillByDefault(Invoke([this](sai_object_id_t id) { m_tun.removedTerms.push_back(id); return m_savedRemoveTunnelTerm(id); }));
        }

        void restoreTunnelMock()
        {
            if (sai_tunnel_api)
            {
                sai_tunnel_api->create_tunnel = m_savedCreateTunnel;
                sai_tunnel_api->create_tunnel_map = m_savedCreateTunnelMap;
                sai_tunnel_api->create_tunnel_map_entry = m_savedCreateTunnelMapEntry;
                sai_tunnel_api->create_tunnel_term_table_entry = m_savedCreateTunnelTerm;
                sai_tunnel_api->remove_tunnel = m_savedRemoveTunnel;
                sai_tunnel_api->remove_tunnel_map = m_savedRemoveTunnelMap;
                sai_tunnel_api->remove_tunnel_map_entry = m_savedRemoveTunnelMapEntry;
                sai_tunnel_api->remove_tunnel_term_table_entry = m_savedRemoveTunnelTerm;
            }
            mock_sai_tunnel = nullptr;
        }

        // Capture the SAI next-hop / next-hop-group / route objects
        // VNetRouteOrch programs, calling through to real libsaivs so downstream
        // references (route -> next hop -> tunnel) resolve to valid OIDs.
        // VNetRouteOrch programs tunnel routes via the non-bulk single-object
        // create calls captured here; VNET *local* routes go through gRouteOrch's
        // route bulker, whose bulk *_route_entries APIs are captured separately by
        // the ApplySaiMock() trampolines (the bulker binds them before this runs).
        void installRouteMock()
        {
            ON_CALL(*mock_sai_next_hop_api, create_next_hop(_, _, _, _))
                .WillByDefault(Invoke([this](sai_object_id_t *id, sai_object_id_t sw,
                                             uint32_t n, const sai_attribute_t *l) {
                    sai_status_t st = old_sai_next_hop_api->create_next_hop(id, sw, n, l);
                    if (st == SAI_STATUS_SUCCESS)
                    {
                        RouteCaptures::NextHop nh;
                        nh.oid = *id;
                        if (auto a = findRawAttr(l, n, SAI_NEXT_HOP_ATTR_TYPE)) nh.type = a->value.s32;
                        if (auto a = findRawAttr(l, n, SAI_NEXT_HOP_ATTR_IP)) nh.ip = a->value.ipaddr;
                        if (auto a = findRawAttr(l, n, SAI_NEXT_HOP_ATTR_TUNNEL_ID)) nh.tunnel_id = a->value.oid;
                        if (auto a = findRawAttr(l, n, SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID)) nh.rif = a->value.oid;
                        if (auto a = findRawAttr(l, n, SAI_NEXT_HOP_ATTR_TUNNEL_VNI)) { nh.has_vni = true; nh.vni = a->value.u32; }
                        if (auto a = findRawAttr(l, n, SAI_NEXT_HOP_ATTR_TUNNEL_MAC))
                        {
                            nh.has_mac = true;
                            memcpy(nh.mac, a->value.mac, sizeof(sai_mac_t));
                        }
                        m_rt.nexthops.push_back(nh);
                    }
                    return st;
                }));
            ON_CALL(*mock_sai_next_hop_api, remove_next_hop(_))
                .WillByDefault(Invoke([this](sai_object_id_t id) {
                    m_rt.removedNexthops.push_back(id);
                    return old_sai_next_hop_api->remove_next_hop(id);
                }));

            ON_CALL(*mock_sai_next_hop_group_api, create_next_hop_group(_, _, _, _))
                .WillByDefault(Invoke([this](sai_object_id_t *id, sai_object_id_t sw,
                                             uint32_t n, const sai_attribute_t *l) {
                    sai_status_t st = old_sai_next_hop_group_api->create_next_hop_group(id, sw, n, l);
                    if (st == SAI_STATUS_SUCCESS)
                    {
                        RouteCaptures::Group g;
                        g.oid = *id;
                        if (auto a = findRawAttr(l, n, SAI_NEXT_HOP_GROUP_ATTR_TYPE)) g.type = a->value.s32;
                        m_rt.groups.push_back(g);
                    }
                    return st;
                }));
            ON_CALL(*mock_sai_next_hop_group_api, remove_next_hop_group(_))
                .WillByDefault(Invoke([this](sai_object_id_t id) {
                    m_rt.removedGroups.push_back(id);
                    return old_sai_next_hop_group_api->remove_next_hop_group(id);
                }));
            ON_CALL(*mock_sai_next_hop_group_api, create_next_hop_group_member(_, _, _, _))
                .WillByDefault(Invoke([this](sai_object_id_t *id, sai_object_id_t sw,
                                             uint32_t n, const sai_attribute_t *l) {
                    sai_status_t st = old_sai_next_hop_group_api->create_next_hop_group_member(id, sw, n, l);
                    if (st == SAI_STATUS_SUCCESS)
                    {
                        RouteCaptures::Member m;
                        m.oid = *id;
                        if (auto a = findRawAttr(l, n, SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID)) m.nhg = a->value.oid;
                        if (auto a = findRawAttr(l, n, SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID)) m.nh = a->value.oid;
                        if (auto a = findRawAttr(l, n, SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID)) { m.has_seq = true; m.seq = a->value.u32; }
                        m_rt.members.push_back(m);
                    }
                    return st;
                }));
            ON_CALL(*mock_sai_next_hop_group_api, remove_next_hop_group_member(_))
                .WillByDefault(Invoke([this](sai_object_id_t id) {
                    m_rt.removedMembers.push_back(id);
                    return old_sai_next_hop_group_api->remove_next_hop_group_member(id);
                }));

            ON_CALL(*mock_sai_route_api, create_route_entry(_, _, _))
                .WillByDefault(Invoke([this](const sai_route_entry_t *e, uint32_t n,
                                             const sai_attribute_t *l) {
                    sai_status_t st = old_sai_route_api->create_route_entry(e, n, l);
                    if (st == SAI_STATUS_SUCCESS)
                    {
                        RouteCaptures::Route r;
                        r.vr = e->vr_id;
                        r.dest = e->destination;
                        if (auto a = findRawAttr(l, n, SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID)) r.next_hop_id = a->value.oid;
                        if (auto a = findRawAttr(l, n, SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION)) r.packet_action = a->value.s32;
                        m_rt.routes.push_back(r);
                    }
                    return st;
                }));
            ON_CALL(*mock_sai_route_api, remove_route_entry(_))
                .WillByDefault(Invoke([this](const sai_route_entry_t *e) {
                    m_rt.removedRoutes.push_back(e->destination);
                    return old_sai_route_api->remove_route_entry(e);
                }));

            // Capture in-place route repoints too (see captureSetRouteEntryAttr).
            g_activeRouteCaptures = &m_rt;
            g_savedSetRouteAttr = sai_route_api->set_route_entry_attribute;
            sai_route_api->set_route_entry_attribute = captureSetRouteEntryAttr;
        }

        // Record the BFD sessions gBfdOrch creates/removes, keyed by the monitor
        // (DST_IP) address the notification path later references, calling
        // through to real libsaivs so each session gets a valid OID.
        void installBfdMock()
        {
            ON_CALL(*mock_sai_bfd_api, create_bfd_session(_, _, _, _))
                .WillByDefault(Invoke([this](sai_object_id_t *id, sai_object_id_t sw,
                                             uint32_t n, const sai_attribute_t *l) {
                    sai_status_t st = old_sai_bfd_api->create_bfd_session(id, sw, n, l);
                    if (st == SAI_STATUS_SUCCESS)
                    {
                        BfdCaptures::Session s;
                        s.oid = *id;
                        if (auto a = findRawAttr(l, n, SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS)) s.dst = a->value.ipaddr;
                        m_bfd.sessions.push_back(s);
                    }
                    return st;
                }));
            ON_CALL(*mock_sai_bfd_api, remove_bfd_session(_))
                .WillByDefault(Invoke([this](sai_object_id_t id) {
                    m_bfd.removed.push_back(id);
                    return old_sai_bfd_api->remove_bfd_session(id);
                }));
        }

        // The VS test writes VXLAN_TUNNEL / VNET to CONFIG_DB and relies on
        // vxlanmgr/vrfmgr to mirror them into APP_DB (vrfmgr passes the VNET
        // fields through unchanged). The mock harness has no cfgmgr daemons, so
        // we write the APP_DB entries the daemons would have produced.
        void setVxlanTunnel(const string &name, const string &src_ip)
        {
            Table tbl(m_app_db.get(), APP_VXLAN_TUNNEL_TABLE_NAME);
            tbl.set(name, {{"src_ip", src_ip}});
            m_VxlanTunnelOrch->addExistingData(&tbl);
            static_cast<Orch *>(m_VxlanTunnelOrch)->doTask();
        }

        // Bring up a VLAN (e.g. "Vlan1000") so VxlanTunnelMapOrch's
        // gPortsOrch->getVlanByVlanId() succeeds. vlanmgrd normally mirrors
        // CONFIG_DB VLAN into APP_DB VLAN_TABLE; the mock writes it directly and
        // drives gPortsOrch, the same idiom evpnmhorch_ut uses.
        void createVlan(const string &vlan)
        {
            Table tbl(m_app_db.get(), APP_VLAN_TABLE_NAME);
            tbl.set(vlan, {{"admin_status", "up"}, {"mtu", "9100"},
                           {"mac", "00:aa:bb:cc:dd:ee"}});
            gPortsOrch->addExistingData(&tbl);
            static_cast<Orch *>(gPortsOrch)->doTask();
        }

        // Add an explicit VLAN<->VNI decap tunnel-map entry, the mock equivalent
        // of vnet_lib.create_vxlan_tunnel_map. vxlanmgrd turns CONFIG_DB
        // VXLAN_TUNNEL_MAP|<tunnel>|<name> into APP_DB VXLAN_TUNNEL_MAP_TABLE
        // keyed <tunnel>:<name> (the ':' VxlanTunnelMapRequest parses), so we
        // write that and drive m_vxlanTunnelMapOrch.
        void createVxlanTunnelMap(const string &tunnel, const string &mapName,
                                  const string &vlan, const string &vni)
        {
            Table tbl(m_app_db.get(), APP_VXLAN_TUNNEL_MAP_TABLE_NAME);
            tbl.set(tunnel + ":" + mapName, {{"vni", vni}, {"vlan", vlan}});
            m_vxlanTunnelMapOrch->addExistingData(&tbl);
            static_cast<Orch *>(m_vxlanTunnelMapOrch.get())->doTask();
        }

        void delVxlanTunnelMap(const string &tunnel, const string &mapName)
        {
            auto consumer = dynamic_cast<Consumer *>(
                m_vxlanTunnelMapOrch->getExecutor(APP_VXLAN_TUNNEL_MAP_TABLE_NAME));
            consumer->addToSync({{tunnel + ":" + mapName, DEL_COMMAND, {}}});
            static_cast<Orch *>(m_vxlanTunnelMapOrch.get())->doTask(*consumer);
        }

        // Mirror the VS test's ordered_ecmp fixture: writing SWITCH_TABLE:switch
        // ordered_ecmp=true drives gSwitchOrch to query the SAI ORDERED_ECMP
        // capability and, when present, program VNET ECMP groups as
        // SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_ORDERED_ECMP with per-member sequence
        // IDs (vnetorch.cpp:830/869).
        void enableOrderedEcmp()
        {
            Table tbl(m_app_db.get(), APP_SWITCH_TABLE_NAME);
            tbl.set("switch", {{"ordered_ecmp", "true"}});
            gSwitchOrch->addExistingData(&tbl);
            static_cast<Orch *>(gSwitchOrch)->doTask();
        }

        void setVnet(const string &name, const string &tunnel, const string &vni,
                     const string &peer_list, bool advertise_prefix = false,
                     const string &overlay_dmac = "", const string &scope = "")
        {
            vector<FieldValueTuple> fvs = {{"vxlan_tunnel", tunnel}, {"vni", vni},
                                           {"peer_list", peer_list}};
            if (advertise_prefix) fvs.push_back({"advertise_prefix", "true"});
            if (!overlay_dmac.empty()) fvs.push_back({"overlay_dmac", overlay_dmac});
            if (!scope.empty()) fvs.push_back({"scope", scope});
            Table tbl(m_app_db.get(), APP_VNET_TABLE_NAME);
            tbl.set(name, fvs);
            m_vnetOrch->addExistingData(&tbl);
            static_cast<Orch *>(m_vnetOrch)->doTask();
        }

        // Deletes must flow through the consumer as a DEL_COMMAND: replaying an
        // addExistingData() after removing the APP_DB key would silently no-op
        // the delete handler and lose delete/refcount coverage.
        void delVnet(const string &name)
        {
            auto consumer = dynamic_cast<Consumer *>(m_vnetOrch->getExecutor(APP_VNET_TABLE_NAME));
            consumer->addToSync({{name, DEL_COMMAND, {}}});
            static_cast<Orch *>(m_vnetOrch)->doTask(*consumer);
        }

        void delVxlanTunnel(const string &name)
        {
            auto consumer = dynamic_cast<Consumer *>(m_VxlanTunnelOrch->getExecutor(APP_VXLAN_TUNNEL_TABLE_NAME));
            consumer->addToSync({{name, DEL_COMMAND, {}}});
            static_cast<Orch *>(m_VxlanTunnelOrch)->doTask(*consumer);
        }

        // Program a VNET route the way the VS tests did: write CONFIG_DB
        // VNET_ROUTE_TUNNEL / VNET_ROUTE (key "vnet|prefix") and drive
        // VNetCfgRouteOrch, which mirrors it (doVnetTunnelRouteTask /
        // doVnetRouteTask) to the APP_DB table VNetRouteOrch consumes; then drive
        // VNetRouteOrch off that APP_DB row. This exercises the real CONFIG->APP
        // translation instead of writing APP_DB directly.
        void programVnetRouteViaCfg(const string &cfgTable, const string &appTable,
                                    const string &vnet, const string &prefix,
                                    const vector<FieldValueTuple> &fvs)
        {
            Table cfg(m_config_db.get(), cfgTable);
            cfg.set(vnet + "|" + prefix, fvs);
            m_vnetCfgRouteOrch->addExistingData(&cfg);
            static_cast<Orch *>(m_vnetCfgRouteOrch.get())->doTask();
            Table app(m_app_db.get(), appTable);
            m_vnetRouteOrch->addExistingData(&app);
            static_cast<Orch *>(m_vnetRouteOrch.get())->doTask();
        }

        // The VS test writes CONFIG_DB VNET_ROUTE_TUNNEL and relies on
        // VNetCfgRouteOrch to mirror it to APP_DB VNET_ROUTE_TUNNEL_TABLE; the
        // mock drives that same path via programVnetRouteViaCfg. endpoint is a
        // comma-separated list -> a single endpoint programs one tunnel next
        // hop; multiple endpoints program an ECMP next hop group.
        void setVnetRoute(const string &vnet, const string &prefix,
                          const string &endpoints, const string &mac = "",
                          const string &vni = "", const string &metric = "")
        {
            vector<FieldValueTuple> fvs = {{"endpoint", endpoints}};
            if (!mac.empty()) fvs.push_back({"mac_address", mac});
            if (!vni.empty()) fvs.push_back({"vni", vni});
            if (!metric.empty()) fvs.push_back({"metric", metric});
            programVnetRouteViaCfg(CFG_VNET_RT_TUNNEL_TABLE_NAME,
                                   APP_VNET_RT_TUNNEL_TABLE_NAME, vnet, prefix, fvs);
        }

        void delVnetRoute(const string &vnet, const string &prefix)
        {
            auto consumer = dynamic_cast<Consumer *>(
                m_vnetRouteOrch->getExecutor(APP_VNET_RT_TUNNEL_TABLE_NAME));
            consumer->addToSync({{vnet + ":" + prefix, DEL_COMMAND, {}}});
            static_cast<Orch *>(m_vnetRouteOrch.get())->doTask(*consumer);
            // Keep the mock DB in sync: the Consumer push above drives the delete
            // handler, but the row must also leave the backing table so a later
            // addExistingData() (e.g. from setVnetRoute*) does not re-enqueue this
            // deleted route as a SET and resurrect it.
            Table tbl(m_app_db.get(), APP_VNET_RT_TUNNEL_TABLE_NAME);
            tbl.del(vnet + ":" + prefix);
        }

        // A BFD-monitored VNET route: the VS test writes VNET_ROUTE_TUNNEL with
        // endpoint + endpoint_monitor (ep_monitor). VNetRouteOrch pairs each
        // endpoint with the monitor at the same list position, and (default BFD
        // monitoring) writes an APP_BFD_SESSION_TABLE row per monitor via its
        // producer. We then flush those rows into gBfdOrch (syncBfd) so the SAI
        // BFD sessions are actually created, mirroring what libsaivs/syncd would
        // have done in the VS test.
        void setVnetRouteMonitored(const string &vnet, const string &prefix,
                                   const string &endpoints, const string &monitors,
                                   const string &profile = "")
        {
            vector<FieldValueTuple> fvs = {{"endpoint", endpoints},
                                           {"endpoint_monitor", monitors}};
            if (!profile.empty()) fvs.push_back({"profile", profile});
            programVnetRouteViaCfg(CFG_VNET_RT_TUNNEL_TABLE_NAME,
                                   APP_VNET_RT_TUNNEL_TABLE_NAME, vnet, prefix, fvs);
            syncBfd();
            syncTunnelDecapTerm();
        }

        // Delete a monitored route, then flush the BFD session removals its
        // teardown produced (removeBfdSession -> producer.del) into gBfdOrch.
        void delVnetRouteMonitored(const string &vnet, const string &prefix)
        {
            delVnetRoute(vnet, prefix);
            syncBfd();
            syncTunnelDecapTerm();
        }

        // A priority (primary/secondary) custom-monitored VNET route. The VS
        // test writes VNET_ROUTE_TUNNEL with endpoint (all endpoints),
        // endpoint_monitor, primary (the subset preferred while any of them is
        // up), monitoring ("custom" or "custom_bfd") and optionally adv_prefix.
        // Endpoints not in primary form the secondary group. For "custom"
        // monitoring VNetRouteOrch writes an APP_DB VNET_MONITOR_TABLE row per
        // endpoint and waits for STATE_DB monitor updates (see
        // updateMonitorSessionState); for "custom_bfd" it also creates SAI BFD
        // sessions, so we flush those into gBfdOrch.
        void setVnetRoutePriority(const string &vnet, const string &prefix,
                                  const string &endpoints, const string &monitors,
                                  const string &primary,
                                  const string &monitoring = "custom",
                                  const string &adv_prefix = "",
                                  const string &profile = "",
                                  bool check_directly_connected = false,
                                  int rx_monitor_timer = -1,
                                  int tx_monitor_timer = -1,
                                  const string &pinned_state = "")
        {
            vector<FieldValueTuple> fvs = {{"endpoint", endpoints},
                                           {"endpoint_monitor", monitors},
                                           {"primary", primary},
                                           {"monitoring", monitoring}};
            if (!adv_prefix.empty()) fvs.push_back({"adv_prefix", adv_prefix});
            if (!profile.empty()) fvs.push_back({"profile", profile});
            if (check_directly_connected) fvs.push_back({"check_directly_connected", "true"});
            if (rx_monitor_timer >= 0) fvs.push_back({"rx_monitor_timer", to_string(rx_monitor_timer)});
            if (tx_monitor_timer >= 0) fvs.push_back({"tx_monitor_timer", to_string(tx_monitor_timer)});
            if (!pinned_state.empty()) fvs.push_back({"pinned_state", pinned_state});
            programVnetRouteViaCfg(CFG_VNET_RT_TUNNEL_TABLE_NAME,
                                   APP_VNET_RT_TUNNEL_TABLE_NAME, vnet, prefix, fvs);
            if (monitoring == "custom_bfd") syncBfd();
        }

        // Drive a custom-monitor endpoint up/down, the mock equivalent of
        // vnet_lib.update_monitor_session_state(): write STATE_DB
        // VNET_MONITOR_TABLE key "monitor|prefix" and deliver it to MonitorOrch,
        // which forwards the state to VNetRouteOrch. Pushed through the consumer
        // as a single SET so only the changed monitor is (re)processed, rather
        // than replaying every row via addExistingData().
        void updateMonitorSessionState(const string &prefix, const string &monitor,
                                       const string &state)
        {
            Table tbl(m_state_db.get(), STATE_VNET_MONITOR_TABLE_NAME);
            tbl.set(monitor + "|" + prefix, {{"state", state}});
            auto consumer = dynamic_cast<Consumer *>(
                m_monitorOrch->getExecutor(STATE_VNET_MONITOR_TABLE_NAME));
            consumer->addToSync({{monitor + "|" + prefix, SET_COMMAND, {{"state", state}}}});
            static_cast<Orch *>(m_monitorOrch.get())->doTask(*consumer);
        }

        // Deliver the APP_BFD_SESSION_TABLE changes VNetRouteOrch's producer made
        // to gBfdOrch. VNetRouteOrch writes new sessions and .del()s removed ones
        // straight into the mock DB, so we diff against what we delivered before:
        // new keys -> SET (gBfdOrch creates the SAI session), disappeared keys ->
        // DEL (gBfdOrch removes it). A plain table removal + addExistingData()
        // would silently drop the DELs, so removals must flow as DEL_COMMAND.
        void syncBfd()
        {
            Table tbl(m_app_db.get(), APP_BFD_SESSION_TABLE_NAME);
            vector<string> keyList;
            tbl.getKeys(keyList);
            set<string> current(keyList.begin(), keyList.end());

            deque<KeyOpFieldsValuesTuple> entries;
            for (const auto &k : current)
            {
                if (m_deliveredBfdKeys.count(k)) continue;
                vector<FieldValueTuple> fvs;
                tbl.get(k, fvs);
                entries.push_back({k, SET_COMMAND, fvs});
            }
            for (const auto &k : m_deliveredBfdKeys)
            {
                if (!current.count(k)) entries.push_back({k, DEL_COMMAND, {}});
            }
            m_deliveredBfdKeys = current;

            if (entries.empty()) return;
            auto consumer = dynamic_cast<Consumer *>(
                gBfdOrch->getExecutor(APP_BFD_SESSION_TABLE_NAME));
            consumer->addToSync(entries);
            static_cast<Orch *>(gBfdOrch)->doTask(*consumer);
        }

        // Deliver the APP_TUNNEL_DECAP_TERM_TABLE changes VNetRouteOrch's
        // producer made to gTunneldecapOrch, exactly as syncBfd() does for BFD
        // sessions. On a route going active VNetRouteOrch::createSubnetDecapTerm
        // writes a TUNNEL_DECAP_TERM_TABLE row (<tunnel>:<prefix>) and on the
        // route going inactive removeSubnetDecapTerm .del()s it; we diff against
        // what we delivered so new keys flow as SET (gTunneldecapOrch creates the
        // MP2MP SAI term) and disappeared keys as DEL (it removes it). When
        // subnet decap is disabled createSubnetDecapTerm is a no-op, so the term
        // table stays empty and this is a harmless no-op for non-decap tests.
        void syncTunnelDecapTerm()
        {
            Table tbl(m_app_db.get(), APP_TUNNEL_DECAP_TERM_TABLE_NAME);
            vector<string> keyList;
            tbl.getKeys(keyList);
            set<string> current(keyList.begin(), keyList.end());

            deque<KeyOpFieldsValuesTuple> entries;
            for (const auto &k : current)
            {
                if (m_deliveredDecapTermKeys.count(k)) continue;
                vector<FieldValueTuple> fvs;
                tbl.get(k, fvs);
                entries.push_back({k, SET_COMMAND, fvs});
            }
            for (const auto &k : m_deliveredDecapTermKeys)
            {
                if (!current.count(k)) entries.push_back({k, DEL_COMMAND, {}});
            }
            m_deliveredDecapTermKeys = current;

            if (entries.empty()) return;
            auto consumer = dynamic_cast<Consumer *>(
                gTunneldecapOrch->getExecutor(APP_TUNNEL_DECAP_TERM_TABLE_NAME));
            consumer->addToSync(entries);
            static_cast<Orch *>(gTunneldecapOrch)->doTask(*consumer);
        }

        // Apply the SUBNET_DECAP feature config, the mock equivalent of the
        // setup_subnet_decap fixture (CONFIG_DB SUBNET_DECAP|AZURE). TunnelDecapOrch
        // stores src_ip / src_ip_v6 in its SubnetDecapConfig, which
        // VNetRouteOrch::createSubnetDecapTerm later uses as the term's source
        // subnet. Driven through gTunneldecapOrch's CFG_SUBNET_DECAP consumer.
        void setSubnetDecapConfig(const string &status, const string &src_ip,
                                  const string &src_ip_v6)
        {
            Table tbl(m_config_db.get(), CFG_SUBNET_DECAP_TABLE_NAME);
            vector<FieldValueTuple> fvs = {{"status", status},
                                           {"src_ip", src_ip},
                                           {"src_ip_v6", src_ip_v6}};
            tbl.set("AZURE", fvs);
            gTunneldecapOrch->addExistingData(&tbl);
            static_cast<Orch *>(gTunneldecapOrch)->doTask();
        }

        // Create the IPINIP subnet-decap tunnel, the mock equivalent of
        // vnet_lib.create_subnet_decap_tunnel (APP_DB TUNNEL_DECAP_TABLE).
        // Orchagent creates a SAI IPINIP tunnel with the given decap DSCP/ECN/TTL
        // modes; the mock captures it via the tunnel API trampolines.
        void createSubnetDecapTunnel(const string &name, const string &dscp_mode,
                                     const string &ecn_mode, const string &ttl_mode)
        {
            Table tbl(m_app_db.get(), APP_TUNNEL_DECAP_TABLE_NAME);
            vector<FieldValueTuple> fvs = {{"tunnel_type", "IPINIP"},
                                           {"dscp_mode", dscp_mode},
                                           {"ecn_mode", ecn_mode},
                                           {"ttl_mode", ttl_mode}};
            tbl.set(name, fvs);
            gTunneldecapOrch->addExistingData(&tbl);
            static_cast<Orch *>(gTunneldecapOrch)->doTask();
        }

        // Delete the IPINIP subnet-decap tunnel (DEL via consumer push so the
        // remove handler actually runs). Mock equivalent of
        // vnet_lib.delete_subnet_decap_tunnel.
        void delSubnetDecapTunnel(const string &name)
        {
            Table tbl(m_app_db.get(), APP_TUNNEL_DECAP_TABLE_NAME);
            tbl.del(name);
            auto consumer = dynamic_cast<Consumer *>(
                gTunneldecapOrch->getExecutor(APP_TUNNEL_DECAP_TABLE_NAME));
            consumer->addToSync({{name, DEL_COMMAND, {}}});
            static_cast<Orch *>(gTunneldecapOrch)->doTask(*consumer);
        }

        // Find the IPINIP subnet-decap tunnel among the captured tunnels (a VXLAN
        // tunnel coexists) and assert its decap DSCP/ECN/TTL modes -- the mock
        // equivalent of vnet_lib.check_ipinip_tunnel. Returns the tunnel oid so
        // the decap-term check can assert ACTION_TUNNEL_ID points at it. Note the
        // VS check inspects the ENCAP_* mode attrs (a libsaivs quirk), whereas
        // orchagent actually programs the DECAP_* attrs (tunneldecaporch.cpp), so
        // the mock verifies those -- same intent, faithful to what is programmed.
        sai_object_id_t checkIpinipTunnel(int32_t dscp_mode, int32_t ecn_mode,
                                          int32_t ttl_mode)
        {
            const TunnelCaptures::Tunnel *found = nullptr;
            for (const auto &t : m_tun.tunnels)
            {
                if (t.type == SAI_TUNNEL_TYPE_IPINIP) { found = &t; break; }
            }
            EXPECT_NE(found, nullptr) << "no IPINIP tunnel captured";
            if (!found) return SAI_NULL_OBJECT_ID;
            EXPECT_EQ(found->dscp_mode, dscp_mode);
            EXPECT_EQ(found->ecn_mode, ecn_mode);
            EXPECT_EQ(found->ttl_mode, ttl_mode);
            return found->oid;
        }

        // Locate the currently-active MP2MP IPINIP subnet-decap term for a route
        // prefix + source subnet ("active" = created and not since removed),
        // mirroring the (tunnel, src, dst) keying of
        // vnet_lib.check_ipinip_tunnel_decap_term.
        const TunnelCaptures::Term *findIpinipDecapTerm(const string &dstPrefix,
                                                        const string &srcSubnet)
        {
            swss::IpPrefix dp(dstPrefix), sp(srcSubnet);
            for (const auto &t : m_tun.terms)
            {
                if (t.type != SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_MP2MP) continue;
                if (t.tunnel_type != SAI_TUNNEL_TYPE_IPINIP) continue;
                if (std::find(m_tun.removedTerms.begin(), m_tun.removedTerms.end(),
                              t.oid) != m_tun.removedTerms.end())
                    continue;
                if (ipAddrEquals(t.dst, dp.getIp().to_string()) &&
                    ipAddrEquals(t.src, sp.getIp().to_string()))
                    return &t;
            }
            return nullptr;
        }

        // Assert an active MP2MP IPINIP decap term exists with the right VR,
        // action tunnel, and src/dst network+mask -- the mock equivalent of
        // vnet_lib.check_ipinip_tunnel_decap_term.
        void checkIpinipDecapTerm(const string &dstPrefix, const string &srcSubnet,
                                  sai_object_id_t tunnelOid)
        {
            swss::IpPrefix dp(dstPrefix), sp(srcSubnet);
            const auto *term = findIpinipDecapTerm(dstPrefix, srcSubnet);
            ASSERT_NE(term, nullptr)
                << "no MP2MP IPINIP decap term for dst " << dstPrefix
                << " src " << srcSubnet;
            EXPECT_EQ(term->vr, gVirtualRouterId);
            EXPECT_EQ(term->action_tunnel, tunnelOid);
            EXPECT_TRUE(ipAddrEquals(term->dst, dp.getIp().to_string()));
            EXPECT_TRUE(ipAddrEquals(term->dst_mask, dp.getMask().to_string()));
            EXPECT_TRUE(ipAddrEquals(term->src, sp.getIp().to_string()));
            EXPECT_TRUE(ipAddrEquals(term->src_mask, sp.getMask().to_string()));
        }

        // Assert no active MP2MP IPINIP decap term exists for the prefix (the
        // term has not been created yet) -- the mock equivalent of the VS
        // `with pytest.raises(AssertionError): check_ipinip_tunnel_decap_term`.
        void checkNoIpinipDecapTerm(const string &dstPrefix, const string &srcSubnet)
        {
            EXPECT_EQ(findIpinipDecapTerm(dstPrefix, srcSubnet), nullptr)
                << "unexpected MP2MP IPINIP decap term for dst " << dstPrefix;
        }

        // Assert the previously-created MP2MP IPINIP decap term for the prefix
        // has been removed -- the mock equivalent of
        // vnet_lib.check_del_ipinip_tunnel_decap_term (asserts a matching term
        // was created and then removed, and none is currently active).
        void checkIpinipDecapTermRemoved(const string &dstPrefix,
                                         const string &srcSubnet)
        {
            swss::IpPrefix dp(dstPrefix), sp(srcSubnet);
            EXPECT_EQ(findIpinipDecapTerm(dstPrefix, srcSubnet), nullptr)
                << "MP2MP IPINIP decap term for " << dstPrefix << " should be gone";
            bool removed = false;
            for (const auto &t : m_tun.terms)
            {
                if (t.type != SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_MP2MP) continue;
                if (!ipAddrEquals(t.dst, dp.getIp().to_string())) continue;
                if (!ipAddrEquals(t.src, sp.getIp().to_string())) continue;
                if (std::find(m_tun.removedTerms.begin(), m_tun.removedTerms.end(),
                              t.oid) != m_tun.removedTerms.end())
                    removed = true;
            }
            EXPECT_TRUE(removed) << "decap term for " << dstPrefix
                                 << " was never created/removed";
        }

        // Program a directly-connected (local) endpoint. Creates an L3 router
        // interface on a port with an IP address, mirroring the VS
        // create_l3_intf + add_ip_address sequence. intfmgrd/portmgrd normally
        // translate CONFIG_DB to these APP_DB rows; the ports are already
        // admin-up from ApplyInitialConfigs. gIntfsOrch is the shared global the
        // base fixture rebuilds per test.
        void createL3Interface(const string &port, const string &ipPrefix)
        {
            Table intfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
            intfTable.set(port, {{"NULL", "NULL"}});
            intfTable.set(port + ":" + ipPrefix, {{"scope", "global"}, {"family", "IPv4"}});
            gIntfsOrch->addExistingData(&intfTable);
            static_cast<Orch *>(gIntfsOrch)->doTask();
        }

        // Resolve a neighbor on a router interface so VNetRouteOrch's
        // isLocalEndpoint() finds it (gNeighOrch->getNeighborEntry) and the
        // route reuses the IP next hop gNeighOrch creates for it, instead of a
        // tunnel-encap next hop. Mock equivalent of the VS add_neighbor (which
        // neighsyncd normally produces from a kernel netlink event).
        void addNeighbor(const string &port, const string &ip, const string &mac)
        {
            Table neighTable(m_app_db.get(), APP_NEIGH_TABLE_NAME);
            neighTable.set(port + ":" + ip, {{"neigh", mac}, {"family", "IPv4"}});
            gNeighOrch->addExistingData(&neighTable);
            static_cast<Orch *>(gNeighOrch)->doTask();
        }

        // Substitute a regular (FRR/fpmsyncd) route by writing APP_DB ROUTE_TABLE
        // and driving gRouteOrch. fpmsyncd normally produces this from a zebra FPM
        // message; that netlink->APP_DB translation is covered by test_route +
        // tests_fpmsyncd (plan Sec 8.4), so the mock writes the entry directly.
        // RouteOrch needs at least nexthop + ifname (an empty ifname is skipped).
        void setRoute(const string &prefix, const string &nexthop, const string &ifname)
        {
            Table tbl(m_app_db.get(), APP_ROUTE_TABLE_NAME);
            tbl.set(prefix, {{"nexthop", nexthop}, {"ifname", ifname}});
            gRouteOrch->addExistingData(&tbl);
            static_cast<Orch *>(gRouteOrch)->doTask();
        }

        // Remove a regular (RouteOrch) route via a DEL_COMMAND Consumer push --
        // the fpmsyncd route-withdraw equivalent. As with delVnetRoute, also drop
        // the backing row so a later addExistingData() cannot resurrect it.
        void delRoute(const string &prefix)
        {
            auto consumer = dynamic_cast<Consumer *>(
                gRouteOrch->getExecutor(APP_ROUTE_TABLE_NAME));
            consumer->addToSync({{prefix, DEL_COMMAND, {}}});
            static_cast<Orch *>(gRouteOrch)->doTask(*consumer);
            Table tbl(m_app_db.get(), APP_ROUTE_TABLE_NAME);
            tbl.del(prefix);
        }

        // Assert the (still-present) next hop for endpoint ip carries the given
        // SAI next hop type. A directly-connected/local endpoint resolves to a
        // SAI_NEXT_HOP_TYPE_IP next hop (created by gNeighOrch), whereas a remote
        // overlay endpoint resolves to a SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP next hop.
        // This is the distinguishing behavior of a check_directly_connected route
        // and is invisible to STATE_DB active_endpoints alone.
        void checkEndpointType(const string &ip, int32_t type) const
        {
            for (auto it = m_rt.nexthops.rbegin(); it != m_rt.nexthops.rend(); ++it)
            {
                if (ipAddrEquals(it->ip, ip) &&
                    find(m_rt.removedNexthops.begin(), m_rt.removedNexthops.end(),
                         it->oid) == m_rt.removedNexthops.end())
                {
                    EXPECT_EQ(it->type, type)
                        << "endpoint " << ip << " has unexpected next hop type";
                    return;
                }
            }
            ADD_FAILURE() << "no active next hop for endpoint " << ip;
        }

        void checkEndpointIsLocal(const string &ip) const
        {
            checkEndpointType(ip, SAI_NEXT_HOP_TYPE_IP);
        }

        void checkEndpointIsRemote(const string &ip) const
        {
            checkEndpointType(ip, SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP);
        }

        // The captured OID for the (still-existing) BFD session monitoring addr,
        // or SAI_NULL_OBJECT_ID -- vnet_lib.get_bfd_session_id() equivalent.
        sai_object_id_t bfdSessionOid(const string &addr) const
        {
            for (auto it = m_bfd.sessions.rbegin(); it != m_bfd.sessions.rend(); ++it)
            {
                if (ipAddrEquals(it->dst, addr) &&
                    find(m_bfd.removed.begin(), m_bfd.removed.end(), it->oid) == m_bfd.removed.end())
                {
                    return it->oid;
                }
            }
            return SAI_NULL_OBJECT_ID;
        }

        bool bfdSessionExists(const string &addr) const
        {
            return bfdSessionOid(addr) != SAI_NULL_OBJECT_ID;
        }

        // Inject a bfd_session_state_change notification for the session that
        // monitors addr, standing in for vnet_lib.update_bfd_session_state()
        // (which sends the same op on the ASIC_DB NOTIFICATIONS channel). gBfdOrch
        // deserializes it, looks the OID up, and notifies VNetRouteOrch, which
        // adds/removes the endpoint's group member synchronously.
        void updateBfdSessionState(const string &addr, sai_bfd_session_state_t state)
        {
            sai_object_id_t oid = bfdSessionOid(addr);
            ASSERT_NE(oid, SAI_NULL_OBJECT_ID) << "no BFD session for monitor " << addr;

            sai_bfd_session_state_notification_t ntf{};
            ntf.bfd_session_id = oid;
            ntf.session_state = state;
            string data = sai_serialize_bfd_session_state_ntf(1, &ntf);

            vector<FieldValueTuple> notifyValues;
            notifyValues.push_back(FieldValueTuple("bfd_session_state_change", data));
            string msg = swss::JSon::buildJson(notifyValues);

            mockReply = (redisReply *)calloc(1, sizeof(redisReply));
            mockReply->type = REDIS_REPLY_ARRAY;
            mockReply->elements = 3; // pattern-message reply: pattern, channel, payload
            mockReply->element = (redisReply **)calloc(mockReply->elements, sizeof(redisReply *));
            mockReply->element[2] = (redisReply *)calloc(1, sizeof(redisReply));
            mockReply->element[2]->type = REDIS_REPLY_STRING;
            mockReply->element[2]->str = (char *)calloc(1, msg.length() + 1);
            memcpy(mockReply->element[2]->str, msg.c_str(), msg.length());

            auto exec = static_cast<Notifier *>(gBfdOrch->getExecutor("BFD_STATE_NOTIFICATIONS"));
            auto consumer = exec->getNotificationConsumer();
            consumer->readData();
            static_cast<Orch *>(gBfdOrch)->doTask(*consumer);
            freeMockReply();

            // gBfdOrch mirrored the new state into STATE_DB BFD_SESSION_TABLE. On
            // a real switch a keyspace notification wakes gBfdMonitorOrch, which
            // drives the custom_bfd monitor state; drive it here so custom_bfd
            // routes react. For plain-BFD routes there is no monitor_info_ entry,
            // so updateCustomBfdState() returns early -- a harmless no-op.
            Table bfdStateTbl(m_state_db.get(), STATE_BFD_SESSION_TABLE_NAME);
            m_bfdMonitorOrch->addExistingData(&bfdStateTbl);
            static_cast<Orch *>(m_bfdMonitorOrch.get())->doTask();

            // A BFD state change activates/deactivates the route, which (when
            // subnet decap is enabled) creates/removes its MP2MP decap term;
            // flush those term writes to gTunneldecapOrch. No-op otherwise.
            syncTunnelDecapTerm();
        }

        // Enable/disable Traffic-Shift-Away, the mock equivalent of vnet_lib
        // set_tsa()/clear_tsa() (which write CONFIG_DB BGP_DEVICE_GLOBAL|STATE
        // tsa_enabled). BgpGlobalStateOrch consumes it and calls
        // BfdOrch::handleTsaStateChange(): on enable it synchronously notifies
        // every shutdown_bfd_during_tsa session down (withdrawing the monitored
        // routes through them) and removes the SAI sessions; on disable it
        // recreates them (initial state down), so routes stay withdrawn until
        // their monitors report up again.
        void setTsa(bool enabled)
        {
            const string val = enabled ? "true" : "false";
            Table tbl(m_config_db.get(), CFG_BGP_DEVICE_GLOBAL_TABLE_NAME);
            tbl.set("STATE", {{"tsa_enabled", val}});
            auto consumer = dynamic_cast<Consumer *>(
                m_bgpGlobalStateOrch->getExecutor(CFG_BGP_DEVICE_GLOBAL_TABLE_NAME));
            consumer->addToSync({{"STATE", SET_COMMAND, {{"tsa_enabled", val}}}});
            static_cast<Orch *>(m_bgpGlobalStateOrch)->doTask(*consumer);
        }

        // Members currently bound to nhg: created for this group and not since
        // removed. VNetRouteOrch adds/removes members in place as BFD state
        // changes, so this is the group's live member count.
        size_t activeMembers(sai_object_id_t nhg) const
        {
            size_t n = 0;
            for (const auto &m : m_rt.members)
            {
                if (m.nhg != nhg) continue;
                if (find(m_rt.removedMembers.begin(), m_rt.removedMembers.end(), m.oid) ==
                    m_rt.removedMembers.end())
                {
                    n++;
                }
            }
            return n;
        }

        // Assert the active member of nhg for tunnel endpoint ip exists, and
        // (ordered ECMP) carries the expected SAI_NEXT_HOP_GROUP_MEMBER_ATTR_
        // SEQUENCE_ID, mirroring vnet_lib.check_next_hop_group_member(). With
        // unordered ECMP, members carry no sequence id.
        void checkGroupMember(sai_object_id_t nhg, const string &ip, bool ordered,
                              uint32_t expected_seq) const
        {
            for (const auto &m : m_rt.members)
            {
                if (m.nhg != nhg) continue;
                if (find(m_rt.removedMembers.begin(), m_rt.removedMembers.end(), m.oid) !=
                    m_rt.removedMembers.end())
                    continue;
                for (const auto &nh : m_rt.nexthops)
                {
                    if (nh.oid == m.nh && ipAddrEquals(nh.ip, ip))
                    {
                        if (ordered)
                        {
                            EXPECT_TRUE(m.has_seq) << "member " << ip << " missing sequence id";
                            EXPECT_EQ(m.seq, expected_seq) << "member " << ip << " wrong sequence id";
                        }
                        else
                        {
                            EXPECT_FALSE(m.has_seq)
                                << "member " << ip << " unexpectedly has a sequence id";
                        }
                        return;
                    }
                }
            }
            ADD_FAILURE() << "no active group member for endpoint " << ip;
        }

        // Scenario body for the mock equivalent of test_vnet_orch_11 (mixed
        // single-endpoint + ECMP BFD-monitored routes sharing sessions). A
        // fixture method so it can drive the helpers and read the captures;
        // called by the ordered and unordered TEST_Fs below.
        void runMixedMonitoredRoutes(bool ordered)
        {
            if (ordered) enableOrderedEcmp();

            setVxlanTunnel("tunnel_11", "11.11.11.11");
            setVnet("Vnet11", "tunnel_11", "100011", "");

            auto activeNexthops = [&]() {
                size_t c = 0;
                for (const auto &nh : m_rt.nexthops)
                    if (find(m_rt.removedNexthops.begin(), m_rt.removedNexthops.end(),
                             nh.oid) == m_rt.removedNexthops.end())
                        c++;
                return c;
            };

            // route1: single endpoint 11.0.0.1 / monitor 11.1.0.1. Default BFD
            // state is down -> not programmed, not advertised.
            setVnetRouteMonitored("Vnet11", "100.100.1.1/32", "11.0.0.1", "11.1.0.1");
            EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
            checkStateDbRoute("Vnet11", "100.100.1.1/32", "");
            checkRouteNotAdvertised("100.100.1.1/32");

            // Monitor up: route1 programmed as a single tunnel next hop (no
            // group).
            updateBfdSessionState("11.1.0.1", SAI_BFD_SESSION_STATE_UP);
            ASSERT_NE(findRoute("100.100.1.1"), nullptr);
            EXPECT_TRUE(m_rt.groups.empty());
            EXPECT_EQ(activeNexthops(), 1U);
            checkStateDbRoute("Vnet11", "100.100.1.1/32", "11.0.0.1");

            // route2: ECMP over 11.0.0.2/11.0.0.1 with monitors 11.1.0.2/
            // 11.1.0.1. Only 11.1.0.1 is up so far, so route2 is a one-member
            // group over 11.0.0.1 (sequence id 1 = 11.0.0.1's position in the
            // sorted set).
            setVnetRouteMonitored("Vnet11", "100.100.2.1/32", "11.0.0.2,11.0.0.1",
                                  "11.1.0.2,11.1.0.1");
            const RouteCaptures::Route *r2 = findRoute("100.100.2.1");
            ASSERT_NE(r2, nullptr);
            const sai_object_id_t nhg2 = r2->next_hop_id;
            EXPECT_EQ(activeMembers(nhg2), 1U);
            checkGroupMember(nhg2, "11.0.0.1", ordered, 1);
            checkStateDbRoute("Vnet11", "100.100.2.1/32", "11.0.0.1");

            // route3: single endpoint 11.0.0.2 / monitor 11.1.0.2 (shares
            // monitor 11.1.0.2 with route2). Still down -> not programmed.
            setVnetRouteMonitored("Vnet11", "100.100.3.1/32", "11.0.0.2", "11.1.0.2");
            EXPECT_EQ(findRoute("100.100.3.1"), nullptr);
            checkStateDbRoute("Vnet11", "100.100.3.1/32", "");

            // Monitor 11.1.0.2 up: route3 programmed (single next hop), and
            // route2 grows to both endpoints (sequence ids 1 and 2).
            updateBfdSessionState("11.1.0.2", SAI_BFD_SESSION_STATE_UP);
            ASSERT_NE(findRoute("100.100.3.1"), nullptr);
            checkStateDbRoute("Vnet11", "100.100.3.1/32", "11.0.0.2");
            EXPECT_EQ(activeMembers(nhg2), 2U);
            checkGroupMember(nhg2, "11.0.0.1", ordered, 1);
            checkGroupMember(nhg2, "11.0.0.2", ordered, 2);
            checkStateDbRoute("Vnet11", "100.100.2.1/32", "11.0.0.1,11.0.0.2");

            // Monitor 11.1.0.1 down: route2 keeps only 11.0.0.2 (sequence id 2
            // -- the sorted position is preserved even though 11.0.0.1 is
            // gone), and route1 (whose only endpoint used 11.1.0.1) is
            // deprogrammed.
            updateBfdSessionState("11.1.0.1", SAI_BFD_SESSION_STATE_DOWN);
            EXPECT_EQ(activeMembers(nhg2), 1U);
            checkGroupMember(nhg2, "11.0.0.2", ordered, 2);
            checkStateDbRoute("Vnet11", "100.100.2.1/32", "11.0.0.2");
            checkStateDbRoute("Vnet11", "100.100.1.1/32", "");

            // Repoint route1 at a new endpoint 11.0.0.2 / monitor 11.1.0.2
            // (already up): route1 comes back over a single next hop, route3 is
            // unaffected.
            setVnetRouteMonitored("Vnet11", "100.100.1.1/32", "11.0.0.2", "11.1.0.2");
            ASSERT_NE(findRoute("100.100.1.1"), nullptr);
            checkStateDbRoute("Vnet11", "100.100.1.1/32", "11.0.0.2");
            checkStateDbRoute("Vnet11", "100.100.3.1/32", "11.0.0.2");

            // Delete route2: its group is removed, and monitor 11.1.0.1 -- now
            // referenced by no route (route1 moved off it) -- has its BFD
            // session removed, while 11.1.0.2 (route1/route3) survives.
            delVnetRouteMonitored("Vnet11", "100.100.2.1/32");
            EXPECT_NE(find(m_rt.removedGroups.begin(), m_rt.removedGroups.end(), nhg2),
                      m_rt.removedGroups.end());
            checkStateDbRouteRemoved("Vnet11", "100.100.2.1/32");
            EXPECT_FALSE(bfdSessionExists("11.1.0.1"));
            EXPECT_TRUE(bfdSessionExists("11.1.0.2"));

            // Delete route1: 11.1.0.2 still referenced by route3, so it
            // survives.
            delVnetRouteMonitored("Vnet11", "100.100.1.1/32");
            checkStateDbRouteRemoved("Vnet11", "100.100.1.1/32");
            EXPECT_TRUE(bfdSessionExists("11.1.0.2"));

            // Delete route3: the last reference to 11.1.0.2 is gone.
            delVnetRouteMonitored("Vnet11", "100.100.3.1/32");
            checkStateDbRouteRemoved("Vnet11", "100.100.3.1/32");
            EXPECT_FALSE(bfdSessionExists("11.1.0.2"));
        }


        // VNET's IPv6 link-local (fe80::/10) route that RouteOrch programs during
        // bind -- its address never matches a VNET route's endpoint prefix.
        //
        // Routes are captured append-only: a create pushes a Route, a remove
        // records the dest in removedRoutes (the Route entry is left in place),
        // and an in-place repoint updates next_hop_id on every entry matching the
        // dest. A priority route can be removed and re-added within one test, so
        // return the *currently active* route for the prefix -- the last created
        // entry, and only when it has been created more times than removed.
        const RouteCaptures::Route *findRoute(const string &ip) const
        {
            size_t creates = 0, removes = 0;
            const RouteCaptures::Route *last = nullptr;
            for (const auto &r : m_rt.routes)
            {
                if (prefixAddrEquals(r.dest, ip))
                {
                    creates++;
                    last = &r;
                }
            }
            for (const auto &d : m_rt.removedRoutes)
            {
                if (prefixAddrEquals(d, ip)) removes++;
            }
            return creates > removes ? last : nullptr;
        }

        // STATE_DB VNET_ROUTE_TUNNEL_TABLE assertions -- the mock equivalent of
        // vnet_lib.check_state_db_routes(). VNetRouteOrch writes this table via a
        // plain swss::Table on STATE_DB, so it is directly readable here (unlike
        // ASIC_DB). activeEndpoints is the comma-joined list of active endpoints
        // in ascending order (NextHopKey/std::map order), which matches the
        // ascending test data.
        void checkStateDbRoute(const string &vnet, const string &prefix,
                               const string &activeEndpoints)
        {
            Table tbl(m_state_db.get(), STATE_VNET_RT_TUNNEL_TABLE_NAME);
            vector<FieldValueTuple> fvs;
            ASSERT_TRUE(tbl.get(vnet + "|" + prefix, fvs))
                << "missing STATE_DB route " << vnet << "|" << prefix;
            string active, state;
            for (const auto &fv : fvs)
            {
                if (fvField(fv) == "active_endpoints") active = fvValue(fv);
                else if (fvField(fv) == "state") state = fvValue(fv);
            }
            EXPECT_EQ(active, activeEndpoints);
            EXPECT_EQ(state, activeEndpoints.empty() ? "inactive" : "active");
        }

        void checkStateDbRouteRemoved(const string &vnet, const string &prefix)
        {
            Table tbl(m_state_db.get(), STATE_VNET_RT_TUNNEL_TABLE_NAME);
            vector<FieldValueTuple> fvs;
            EXPECT_FALSE(tbl.get(vnet + "|" + prefix, fvs))
                << "STATE_DB route " << vnet << "|" << prefix << " not removed";
        }

        // The default VNET does not advertise prefixes, so the prefix must be
        // absent from STATE_DB ADVERTISE_NETWORK_TABLE --
        // vnet_lib.check_remove_routes_advertisement().
        void checkRouteNotAdvertised(const string &prefix)
        {
            Table tbl(m_state_db.get(), STATE_ADVERTISE_NETWORK_TABLE_NAME);
            vector<string> keys;
            tbl.getKeys(keys);
            EXPECT_EQ(find(keys.begin(), keys.end(), prefix), keys.end())
                << "prefix " << prefix << " unexpectedly advertised";
        }

        // Assert a prefix is present in STATE_DB ADVERTISE_NETWORK_TABLE (and,
        // when given, carries the expected profile) --
        // vnet_lib.check_routes_advertisement().
        void checkRouteAdvertised(const string &prefix, const string &profile = "")
        {
            Table tbl(m_state_db.get(), STATE_ADVERTISE_NETWORK_TABLE_NAME);
            vector<string> keys;
            tbl.getKeys(keys);
            ASSERT_NE(find(keys.begin(), keys.end(), prefix), keys.end())
                << "prefix " << prefix << " not advertised";
            if (!profile.empty())
            {
                vector<FieldValueTuple> fvs;
                ASSERT_TRUE(tbl.get(prefix, fvs));
                string value;
                for (const auto &fv : fvs)
                    if (fvField(fv) == "profile") value = fvValue(fv);
                EXPECT_EQ(value, profile);
            }
        }

        // Assert the SAI programming for a priority/custom-monitored route --
        // vnet_lib.check_priority_vnet_ecmp_routes(). With a single active
        // endpoint the route points directly at that endpoint's tunnel next hop
        // (no group); with several it points at an ECMP group whose active
        // members are exactly those endpoints. The authoritative active-endpoint
        // set is checked separately via checkStateDbRoute().
        void checkPriorityRoute(const string &prefix, const vector<string> &activeEndpoints,
                                bool ordered = false)
        {
            const RouteCaptures::Route *r = findRoute(prefix);
            ASSERT_NE(r, nullptr) << "no route for prefix " << prefix;
            if (activeEndpoints.size() == 1)
            {
                bool found = false;
                for (const auto &nh : m_rt.nexthops)
                {
                    if (nh.oid == r->next_hop_id && ipAddrEquals(nh.ip, activeEndpoints[0]))
                    {
                        found = true;
                        break;
                    }
                }
                EXPECT_TRUE(found) << "route " << prefix
                                   << " not pointing at tunnel next hop for "
                                   << activeEndpoints[0];
            }
            else
            {
                sai_object_id_t nhg = r->next_hop_id;
                int32_t type = -1;
                for (const auto &g : m_rt.groups)
                    if (g.oid == nhg) type = g.type;
                EXPECT_EQ(type, SAI_NEXT_HOP_GROUP_TYPE_ECMP)
                    << "priority route " << prefix << " group is not unordered ECMP";
                EXPECT_EQ(activeMembers(nhg), activeEndpoints.size());
                for (size_t i = 0; i < activeEndpoints.size(); i++)
                    checkGroupMember(nhg, activeEndpoints[i], ordered, (uint32_t)(i + 1));
            }
        }

        // APP_DB VNET_MONITOR_TABLE assertions -- the monitor sessions
        // VNetRouteOrch writes for custom-monitored endpoints. Key is
        // "monitor:prefix". Mock equivalent of
        // vnet_lib.check_custom_monitor_app_db().
        void checkCustomMonitorAppDb(const string &prefix, const string &monitor,
                                     const string &packet_type, const string &overlay_dmac)
        {
            Table tbl(m_app_db.get(), APP_VNET_MONITOR_TABLE_NAME);
            vector<FieldValueTuple> fvs;
            ASSERT_TRUE(tbl.get(monitor + ":" + prefix, fvs))
                << "missing APP_DB monitor session " << monitor << ":" << prefix;
            string pt, dmac;
            for (const auto &fv : fvs)
            {
                if (fvField(fv) == "packet_type") pt = fvValue(fv);
                else if (fvField(fv) == "overlay_dmac") dmac = fvValue(fv);
            }
            EXPECT_EQ(pt, packet_type);
            EXPECT_EQ(dmac, overlay_dmac);
        }

        // Assert a custom-monitor session's APP_DB row is gone --
        // vnet_lib.check_custom_monitor_deleted().
        void checkCustomMonitorDeleted(const string &prefix, const string &monitor)
        {
            Table tbl(m_app_db.get(), APP_VNET_MONITOR_TABLE_NAME);
            vector<FieldValueTuple> fvs;
            EXPECT_FALSE(tbl.get(monitor + ":" + prefix, fvs))
                << "APP_DB monitor session " << monitor << ":" << prefix << " not removed";
        }

        // The subset of the given endpoint IPs that currently have an active
        // (non-removed) next hop. A local endpoint's IP next hop persists once
        // gNeighOrch creates it; a remote endpoint's tunnel-encap next hop is
        // removed once the priority route stops using it. This is the mock view
        // of the ASIC_DB next hops (filtered by SAI_NEXT_HOP_ATTR_IP -- both
        // IP and tunnel-encap next hops carry it) that test_vnet_orch_28 counts.
        set<string> activeEndpointNexthops(const vector<string> &candidates) const
        {
            set<string> result;
            for (const auto &ip : candidates)
                for (const auto &nh : m_rt.nexthops)
                    if (ipAddrEquals(nh.ip, ip) &&
                        find(m_rt.removedNexthops.begin(), m_rt.removedNexthops.end(),
                             nh.oid) == m_rt.removedNexthops.end())
                        result.insert(ip);
            return result;
        }

        // Assert the tunnel-termination ACL rule VNetRouteOrch programs for a
        // local endpoint. VNetTunnelTermAcl writes the redirect rule into APP_DB
        // ACL_RULE_TABLE (consumed by AclOrch, whose APP_DB->SAI translation is
        // covered by aclorch_ut) with priority 9998, a DST_IP match on the route
        // VIP, TUNNEL_TERM=true and a redirect to the local endpoint IP; the
        // ACL_TABLE_TYPE it defines carries the REDIRECT + COUNTER actions. Mock
        // equivalent of dvs_acl.verify_redirect_acl_rule +
        // verify_acl_table_action_list, at the VNet->ACL (APP_DB) boundary
        // (the VS test asserts the resulting SAI ACL entry in ASIC_DB).
        void checkTunnelTermAclRule(const string &vnet, const string &prefix,
                                    const string &redirectIp)
        {
            string ruleName = string(VNET_TUNNEL_TERM_ACL_TABLE) + ":" + vnet + "_" +
                              prefix + "_" + VNET_TUNNEL_TERM_ACL_RULE_NAME_SUFFIX;
            Table ruleTbl(m_app_db.get(), APP_ACL_RULE_TABLE_NAME);
            vector<FieldValueTuple> fvs;
            ASSERT_TRUE(ruleTbl.get(ruleName, fvs))
                << "missing tunnel-term ACL rule " << ruleName;
            map<string, string> f;
            for (const auto &fv : fvs) f[fvField(fv)] = fvValue(fv);
            EXPECT_EQ(f[RULE_PRIORITY], to_string(VNET_TUNNEL_TERM_ACL_BASE_PRIORITY));
            EXPECT_EQ(f[MATCH_DST_IP], prefix);
            EXPECT_EQ(f[MATCH_TUNNEL_TERM], "true");
            EXPECT_EQ(f[ACTION_REDIRECT_ACTION], redirectIp);

            Table typeTbl(m_app_db.get(), APP_ACL_TABLE_TYPE_TABLE_NAME);
            vector<FieldValueTuple> typeFvs;
            ASSERT_TRUE(typeTbl.get(VNET_TUNNEL_TERM_ACL_TABLE_TYPE, typeFvs))
                << "missing tunnel-term ACL table type";
            string actions;
            for (const auto &fv : typeFvs)
                if (fvField(fv) == ACL_TABLE_TYPE_ACTIONS) actions = fvValue(fv);
            EXPECT_NE(actions.find(ACTION_REDIRECT_ACTION), string::npos)
                << "ACL table type actions missing REDIRECT_ACTION: " << actions;
            EXPECT_NE(actions.find(ACTION_COUNTER), string::npos)
                << "ACL table type actions missing COUNTER: " << actions;
        }

        // Assert no tunnel-term ACL rule exists yet -- dvs_acl.verify_no_acl_rules()
        // equivalent, scoped to the VNET local-endpoint redirect table.
        void checkNoTunnelTermAclRules()
        {
            Table ruleTbl(m_app_db.get(), APP_ACL_RULE_TABLE_NAME);
            vector<string> keys;
            ruleTbl.getKeys(keys);
            for (const auto &k : keys)
                EXPECT_NE(k.find(string(VNET_TUNNEL_TERM_ACL_TABLE) + ":"), 0U)
                    << "unexpected tunnel-term ACL rule " << k
                    << " before route creation";
        }

        // Bring a port oper-up by injecting a port_state_change SAI notification
        // (the same channel syncd uses). Without this a fixture port stays
        // oper-down, so NeighOrch stamps NHFLAGS_IFDOWN on next hops learned on it
        // and RouteOrch refuses to program routes through them. Mock equivalent of
        // the VS ports coming up once their carrier is set.
        void setPortOperStatus(const string &port, sai_port_oper_status_t status)
        {
            Port p;
            ASSERT_TRUE(gPortsOrch->getPort(port, p));

            sai_port_oper_status_notification_t ntf{};
            ntf.port_id = p.m_port_id;
            ntf.port_state = status;
            string data = sai_serialize_port_oper_status_ntf(1, &ntf);

            vector<FieldValueTuple> notifyValues;
            notifyValues.push_back(FieldValueTuple("port_state_change", data));
            string msg = swss::JSon::buildJson(notifyValues);

            mockReply = (redisReply *)calloc(1, sizeof(redisReply));
            mockReply->type = REDIS_REPLY_ARRAY;
            mockReply->elements = 3; // pattern-message reply: pattern, channel, payload
            mockReply->element = (redisReply **)calloc(mockReply->elements, sizeof(redisReply *));
            mockReply->element[2] = (redisReply *)calloc(1, sizeof(redisReply));
            mockReply->element[2]->type = REDIS_REPLY_STRING;
            mockReply->element[2]->str = (char *)calloc(1, msg.length() + 1);
            memcpy(mockReply->element[2]->str, msg.c_str(), msg.length());

            auto exec = static_cast<Notifier *>(gPortsOrch->getExecutor("PORT_STATUS_NOTIFICATIONS"));
            auto consumer = exec->getNotificationConsumer();
            consumer->readData();
            static_cast<Orch *>(gPortsOrch)->doTask(*consumer);
            freeMockReply();
        }

        // Bind a router interface to a VNET, standing in for the VS
        // create_phy_interface(): it writes CONFIG_DB INTERFACE with a vnet_name
        // (mirrored to APP_DB INTF_TABLE by intfmgr) plus the interface IP.
        // gIntfsOrch reads the vnet_name, resolves the VNET via gDirectory, and
        // asks VNetOrch::setIntf() to create the RIF in the VNET's ingress VR
        // (vnetorch.cpp) -- exactly the path a VNET-bound local route needs so
        // its next hops resolve inside the VNET VR rather than the default VR.
        void createVnetL3Interface(const string &port, const string &vnet,
                                   const string &ipPrefix)
        {
            Table intfTable(m_app_db.get(), APP_INTF_TABLE_NAME);
            intfTable.set(port, {{"vnet_name", vnet}});
            intfTable.set(port + ":" + ipPrefix, {{"scope", "global"}, {"family", "IPv4"}});
            gIntfsOrch->addExistingData(&intfTable);
            static_cast<Orch *>(gIntfsOrch)->doTask();

            // Bring the port up so neighbors learned on it produce programmable
            // (non-IFDOWN) next hops.
            setPortOperStatus(port, SAI_PORT_OPER_STATUS_UP);
        }

        // The VS test writes CONFIG_DB VNET_ROUTE (ifname + nexthop) and relies on
        // VNetCfgRouteOrch to mirror it to APP_DB VNET_ROUTE_TABLE; the mock drives
        // that same CONFIG->APP path via programVnetRouteViaCfg. A non-empty
        // nexthop list makes this a VNET local route: VNetRouteOrch::handleRoutes()
        // -> doRouteTask() builds an ip@ifname next-hop-group string and programs
        // it through gRouteOrch, resolving each nexthop to the neighbor's
        // SAI_NEXT_HOP_TYPE_IP next hop.
        void setVnetLocalRoute(const string &vnet, const string &prefix,
                               const string &ifnames, const string &nexthops)
        {
            vector<FieldValueTuple> fvs = {{"ifname", ifnames}, {"nexthop", nexthops}};
            programVnetRouteViaCfg(CFG_VNET_RT_TABLE_NAME,
                                   APP_VNET_RT_TABLE_NAME, vnet, prefix, fvs);
        }

        void delVnetLocalRoute(const string &vnet, const string &prefix)
        {
            auto consumer = dynamic_cast<Consumer *>(
                m_vnetRouteOrch->getExecutor(APP_VNET_RT_TABLE_NAME));
            consumer->addToSync({{vnet + ":" + prefix, DEL_COMMAND, {}}});
            static_cast<Orch *>(m_vnetRouteOrch.get())->doTask(*consumer);
            Table tbl(m_app_db.get(), APP_VNET_RT_TABLE_NAME);
            tbl.del(vnet + ":" + prefix);
        }

        // Assert a VNET local route's SAI programming --
        // vnet_lib.check_vnet_local_routes() + check_vnet_local_route_nexthops().
        // A single nexthop points the route directly at that neighbor's IP next
        // hop; multiple nexthops point it at an (unordered) ECMP group whose
        // members are exactly those neighbor IP next hops.
        void checkVnetLocalRoute(const string &prefix, const vector<string> &nexthops)
        {
            const RouteCaptures::Route *r = findRoute(prefix.substr(0, prefix.find('/')));
            ASSERT_NE(r, nullptr) << "no local route for prefix " << prefix;
            if (nexthops.size() == 1)
            {
                bool found = false;
                for (const auto &nh : m_rt.nexthops)
                    if (nh.oid == r->next_hop_id && ipAddrEquals(nh.ip, nexthops[0]))
                    {
                        found = true;
                        break;
                    }
                EXPECT_TRUE(found) << "local route " << prefix
                                   << " not pointing at IP next hop for " << nexthops[0];
            }
            else
            {
                sai_object_id_t nhg = r->next_hop_id;
                int32_t type = -1;
                for (const auto &g : m_rt.groups)
                    if (g.oid == nhg) type = g.type;
                EXPECT_EQ(type, SAI_NEXT_HOP_GROUP_TYPE_ECMP)
                    << "local ECMP route " << prefix << " group is not unordered ECMP";
                EXPECT_EQ(activeMembers(nhg), nexthops.size());
                for (size_t i = 0; i < nexthops.size(); i++)
                    checkGroupMember(nhg, nexthops[i], false, (uint32_t)(i + 1));
            }
        }

        // Assert a VNET local route is gone -- vnet_lib.check_del_vnet_local_routes().
        void checkVnetLocalRouteRemoved(const string &prefix)
        {
            EXPECT_EQ(findRoute(prefix.substr(0, prefix.find('/'))), nullptr)
                << "local route " << prefix << " not removed";
        }

        // The SAI router-interface OID IntfsOrch created for a port (a real
        // libsaivs RIF -- only the VR/NH/NHG/route/bfd/tunnel APIs are mocked).
        sai_object_id_t rifOf(const string &port)
        {
            Port p;
            if (!gPortsOrch->getPort(port, p)) return SAI_NULL_OBJECT_ID;
            return p.m_rif_id;
        }

        // Collect the router interfaces the prefix's active next hop(s) resolve
        // to -- a lone IP next hop's RIF, or every *active* NHG member's next-hop
        // RIF -- and assert they equal the expected set. In the unnumbered-ECMP
        // case the members share one next-hop IP, so the RIF (not the IP) is what
        // distinguishes them, mirroring test37's _nhgm_rifs()/_nh_rif() checks.
        void checkVnetLocalRouteRifs(const string &prefix,
                                     const set<sai_object_id_t> &expected)
        {
            const RouteCaptures::Route *r = findRoute(prefix.substr(0, prefix.find('/')));
            ASSERT_NE(r, nullptr) << "no local route for prefix " << prefix;
            auto rifOfNh = [&](sai_object_id_t nhoid) -> sai_object_id_t {
                for (auto it = m_rt.nexthops.rbegin(); it != m_rt.nexthops.rend(); ++it)
                    if (it->oid == nhoid) return it->rif;
                return SAI_NULL_OBJECT_ID;
            };
            bool isGroup = false;
            for (const auto &g : m_rt.groups)
                if (g.oid == r->next_hop_id) { isGroup = true; break; }
            set<sai_object_id_t> got;
            if (!isGroup)
            {
                got.insert(rifOfNh(r->next_hop_id));
            }
            else
            {
                for (const auto &m : m_rt.members)
                    if (m.nhg == r->next_hop_id &&
                        find(m_rt.removedMembers.begin(), m_rt.removedMembers.end(),
                             m.oid) == m_rt.removedMembers.end())
                        got.insert(rifOfNh(m.nh));
            }
            EXPECT_EQ(got, expected)
                << "prefix " << prefix << " next-hop RIF set mismatch";
        }
    };

    // Minimal end-to-end check that the fixture drives VNetOrch: creating a VNET
    // that references a VXLAN tunnel programs a SAI virtual router for the VNET
    // (the mock-test equivalent of check_vnet_entry() asserting a new
    // SAI_OBJECT_TYPE_VIRTUAL_ROUTER). The VR is created with an empty attribute
    // list, so we assert on the create call itself, not on the attributes.
    TEST_F(VNetOrchTest, VnetCreateProgramsVirtualRouter)
    {
        EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router)
            .Times(1)
            .WillOnce(Invoke(m_vrMock.get(), &VirtualRouterSaiMock::handleCreate));

        setVxlanTunnel("tunnel_v4", "10.1.0.32");
        setVnet("Vnet1", "tunnel_v4", "10001", "");

        EXPECT_EQ(m_vrMock->create_count, 1);
        EXPECT_NE(m_vrMock->created_oid, SAI_NULL_OBJECT_ID);
    }

    // Binding a VNET to a VXLAN tunnel drives VxlanTunnelOrch::createVxlanTunnelMap
    // -> createTunnelHw, programming four tunnel maps, one tunnel, and one
    // tunnel-term entry with the attributes vnet_lib.check_vxlan_tunnel() asserts.
    TEST_F(VNetOrchTest, VnetBindProgramsVxlanTunnel)
    {
        EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router)
            .Times(1)
            .WillOnce(Invoke(m_vrMock.get(), &VirtualRouterSaiMock::handleCreate));

        setVxlanTunnel("tunnel_v4", "10.1.0.32");
        setVnet("Vnet1", "tunnel_v4", "10001", "");

        // Four tunnel maps, in the order VxlanTunnelOrch programs them:
        // VNI->VLAN, VLAN->VNI, VNI->VR, VR->VNI (the tunnel_map_id[0..3]
        // ordering check_vxlan_tunnel relies on).
        ASSERT_EQ(m_tun.maps.size(), 4U);
        EXPECT_EQ(m_tun.maps[0].type, SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID);
        EXPECT_EQ(m_tun.maps[1].type, SAI_TUNNEL_MAP_TYPE_VLAN_ID_TO_VNI);
        EXPECT_EQ(m_tun.maps[2].type, SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID);
        EXPECT_EQ(m_tun.maps[3].type, SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI);

        ASSERT_EQ(m_tun.tunnels.size(), 1U);
        const auto &t = m_tun.tunnels[0];
        EXPECT_EQ(t.type, SAI_TUNNEL_TYPE_VXLAN);
        EXPECT_NE(t.underlay, SAI_NULL_OBJECT_ID);
        EXPECT_TRUE(ipAddrEquals(t.src, "10.1.0.32"));
        // DECAP mappers = {VNI->VLAN, VNI->VR}; ENCAP mappers = {VLAN->VNI, VR->VNI}.
        ASSERT_EQ(t.decap_mappers.size(), 2U);
        EXPECT_EQ(t.decap_mappers[0], m_tun.maps[0].oid);
        EXPECT_EQ(t.decap_mappers[1], m_tun.maps[2].oid);
        ASSERT_EQ(t.encap_mappers.size(), 2U);
        EXPECT_EQ(t.encap_mappers[0], m_tun.maps[1].oid);
        EXPECT_EQ(t.encap_mappers[1], m_tun.maps[3].oid);

        ASSERT_EQ(m_tun.terms.size(), 1U);
        const auto &term = m_tun.terms[0];
        EXPECT_EQ(term.type, SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP);
        EXPECT_EQ(term.tunnel_type, SAI_TUNNEL_TYPE_VXLAN);
        EXPECT_EQ(term.vr, gVirtualRouterId);
        EXPECT_EQ(term.action_tunnel, t.oid);
        EXPECT_TRUE(ipAddrEquals(term.dst, "10.1.0.32"));
    }

    // The same bind programs the two VR<->VNI tunnel-map entries carrying the
    // VNET's VNI -- the mock equivalent of vnet_lib.check_vxlan_tunnel_entry().
    TEST_F(VNetOrchTest, VnetBindProgramsTunnelMapEntries)
    {
        EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router)
            .Times(1)
            .WillOnce(Invoke(m_vrMock.get(), &VirtualRouterSaiMock::handleCreate));

        setVxlanTunnel("tunnel_v4", "10.1.0.32");
        setVnet("Vnet1", "tunnel_v4", "10001", "");

        const sai_object_id_t vnetVr = m_vrMock->created_oid;
        ASSERT_EQ(m_tun.maps.size(), 4U);
        ASSERT_EQ(m_tun.mapEntries.size(), 2U);

        // Entry 0: VR->VNI, keyed by the VNET VR, valued with the VNI.
        const auto &e0 = m_tun.mapEntries[0];
        EXPECT_EQ(e0.map_type, SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI);
        EXPECT_EQ(e0.tunnel_map, m_tun.maps[3].oid);
        EXPECT_EQ(e0.vr_key, vnetVr);
        EXPECT_EQ(e0.vni_value, 10001U);

        // Entry 1: VNI->VR, keyed by the VNI, valued with the VNET VR.
        const auto &e1 = m_tun.mapEntries[1];
        EXPECT_EQ(e1.map_type, SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID);
        EXPECT_EQ(e1.tunnel_map, m_tun.maps[2].oid);
        EXPECT_EQ(e1.vni_key, 10001U);
        EXPECT_EQ(e1.vr_value, vnetVr);
    }

    // Deleting the VNET tears down its per-VNET SAI objects -- the two tunnel-map
    // entries and the VNET virtual router. Deleting the VXLAN tunnel then removes
    // the shared tunnel, four maps, and term entry (the mock equivalent of
    // vnet_lib.check_del_vxlan_tunnel(); the VS test's check_del_vnet_entry is a
    // no-op, so the VR/map-entry removal here is stronger than the VS coverage).
    TEST_F(VNetOrchTest, VnetAndTunnelDeleteRemoveObjects)
    {
        EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router)
            .Times(1)
            .WillOnce(Invoke(m_vrMock.get(), &VirtualRouterSaiMock::handleCreate));
        EXPECT_CALL(*mock_sai_virtual_router_api, remove_virtual_router)
            .Times(1)
            .WillOnce(Invoke(m_vrMock.get(), &VirtualRouterSaiMock::handleRemove));

        setVxlanTunnel("tunnel_v4", "10.1.0.32");
        setVnet("Vnet1", "tunnel_v4", "10001", "");
        const sai_object_id_t vnetVr = m_vrMock->created_oid;
        ASSERT_EQ(m_tun.mapEntries.size(), 2U);

        delVnet("Vnet1");
        EXPECT_EQ(m_vrMock->removed_oid, vnetVr);
        EXPECT_EQ(m_tun.removedMapEntries.size(), 2U);

        delVxlanTunnel("tunnel_v4");
        EXPECT_EQ(m_tun.removedTunnels.size(), 1U);
        EXPECT_EQ(m_tun.removedMaps.size(), 4U);
        EXPECT_EQ(m_tun.removedTerms.size(), 1U);
    }

    // Mock equivalent of test_vnet_vxlan_multi_map: on a tunnel that a VNET has
    // already bound (and thus activated), an explicit VXLAN_TUNNEL_MAP entry adds
    // a further VLAN<->VNI decap tunnel-map entry on top of the VNET's VR<->VNI
    // maps -- hence "multi map". VxlanTunnelMapOrch programs a VNI_TO_VLAN_ID map
    // entry against the tunnel's dedicated VNI->VLAN decap map. The VS test only
    // checks the VNET/tunnel entries and that adding the map does not disturb
    // them; the mock additionally asserts the explicit entry's vni/vlan, then
    // that VNET + tunnel delete still tear everything down.
    TEST_F(VNetOrchTest, VnetVxlanMultiMap)
    {
        EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router)
            .Times(1)
            .WillOnce(Invoke(m_vrMock.get(), &VirtualRouterSaiMock::handleCreate));
        EXPECT_CALL(*mock_sai_virtual_router_api, remove_virtual_router)
            .Times(1)
            .WillOnce(Invoke(m_vrMock.get(), &VirtualRouterSaiMock::handleRemove));

        setVxlanTunnel("tunnel_v4", "10.1.0.32");
        setVnet("Vnet1", "tunnel_v4", "10001", "");

        // The bind programs 4 maps + the 2 VR<->VNI entries and 1 term, exactly
        // as VnetBindProgramsVxlanTunnel / ...TunnelMapEntries assert.
        const sai_object_id_t vnetVr = m_vrMock->created_oid;
        ASSERT_EQ(m_tun.maps.size(), 4U);
        ASSERT_EQ(m_tun.mapEntries.size(), 2U);
        EXPECT_EQ(m_tun.maps[0].type, SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID);

        // Add the explicit VLAN<->VNI map on the already-active tunnel. Vlan1000
        // must exist for VxlanTunnelMapOrch's getVlanByVlanId() to resolve it.
        createVlan("Vlan1000");
        createVxlanTunnelMap("tunnel_v4", "map_1", "Vlan1000", "1000");

        // A third tunnel-map entry appears: VNI 1000 -> VLAN 1000, against the
        // tunnel's VNI->VLAN decap map (maps[0]). The VNET's VR<->VNI entries are
        // untouched.
        ASSERT_EQ(m_tun.mapEntries.size(), 3U);
        const auto &e = m_tun.mapEntries[2];
        EXPECT_EQ(e.map_type, SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID);
        EXPECT_EQ(e.tunnel_map, m_tun.maps[0].oid);
        EXPECT_EQ(e.vni_key, 1000U);
        EXPECT_EQ(e.vlan_value, 1000U);

        // Deleting the VNET removes its VR + the 2 VR<->VNI entries (the explicit
        // VLAN<->VNI entry is owned by the tunnel map, not the VNET).
        delVnet("Vnet1");
        EXPECT_EQ(m_vrMock->removed_oid, vnetVr);
        EXPECT_EQ(m_tun.removedMapEntries.size(), 2U);

        // The tunnel still can't be torn down while the explicit map references
        // it (the VS test stops here, asserting nothing further). Removing the
        // explicit VLAN<->VNI map drops that 3rd entry and releases the tunnel...
        delVxlanTunnelMap("tunnel_v4", "map_1");
        EXPECT_EQ(m_tun.removedMapEntries.size(), 3U);

        // ...so the tunnel and its 4 maps + P2MP term now tear down cleanly.
        delVxlanTunnel("tunnel_v4");
        EXPECT_EQ(m_tun.removedTunnels.size(), 1U);
        EXPECT_EQ(m_tun.removedMaps.size(), 4U);
        EXPECT_EQ(m_tun.removedTerms.size(), 1U);
    }

    // A single-endpoint VNET tunnel route programs one SAI tunnel encap next hop
    // (TUNNEL_ENCAP over the VNET's tunnel to the endpoint) and one route entry
    // in the VNET's virtual router pointing straight at it -- the mock
    // equivalent of vnet_lib.check_vnet_routes() with a single endpoint.
    TEST_F(VNetOrchTest, VnetRouteProgramsTunnelNextHop)
    {
        setVxlanTunnel("tunnel_v4", "10.10.10.10");
        setVnet("Vnet_2000", "tunnel_v4", "2000", "");
        const sai_object_id_t vnetVr = m_vrMock->created_oid;

        setVnetRoute("Vnet_2000", "100.100.1.1/32", "10.10.10.1");

        // One tunnel encap next hop to the endpoint, over the VNET's tunnel.
        ASSERT_EQ(m_rt.nexthops.size(), 1U);
        const auto &nh = m_rt.nexthops[0];
        EXPECT_EQ(nh.type, SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP);
        EXPECT_TRUE(ipAddrEquals(nh.ip, "10.10.10.1"));
        ASSERT_EQ(m_tun.tunnels.size(), 1U);
        EXPECT_EQ(nh.tunnel_id, m_tun.tunnels[0].oid);
        EXPECT_FALSE(nh.has_vni);
        EXPECT_FALSE(nh.has_mac);

        // Single endpoint -> no next hop group; the route points at the next hop.
        EXPECT_TRUE(m_rt.groups.empty());

        const RouteCaptures::Route *r = findRoute("100.100.1.1");
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(r->vr, vnetVr);
        EXPECT_EQ(r->next_hop_id, nh.oid);
    }

    // A route whose endpoint carries an inner MAC programs the tunnel next hop
    // with SAI_NEXT_HOP_ATTR_TUNNEL_MAC -- vnet_lib.check_vnet_routes(..., mac).
    TEST_F(VNetOrchTest, VnetRouteWithMacProgramsTunnelMac)
    {
        setVxlanTunnel("tunnel_v4", "10.10.10.10");
        setVnet("Vnet_2000", "tunnel_v4", "2000", "");

        setVnetRoute("Vnet_2000", "100.100.2.1/32", "10.10.10.2", "00:12:34:56:78:9A");

        ASSERT_EQ(m_rt.nexthops.size(), 1U);
        const auto &nh = m_rt.nexthops[0];
        EXPECT_EQ(nh.type, SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP);
        EXPECT_TRUE(ipAddrEquals(nh.ip, "10.10.10.2"));
        ASSERT_TRUE(nh.has_mac);
        EXPECT_TRUE(saiMacEquals(nh.mac, "00:12:34:56:78:9A"));
    }

    // A multi-endpoint route programs one tunnel encap next hop per endpoint, a
    // next hop group binding them, and a route entry pointing at the group --
    // the mock equivalent of vnet_lib.check_vnet_ecmp_routes().
    TEST_F(VNetOrchTest, VnetEcmpRouteProgramsNextHopGroup)
    {
        setVxlanTunnel("tunnel_v4", "7.7.7.7");
        setVnet("Vnet1", "tunnel_v4", "10007", "");
        const sai_object_id_t vnetVr = m_vrMock->created_oid;

        setVnetRoute("Vnet1", "100.100.1.1/32", "7.0.0.1,7.0.0.2,7.0.0.3");

        // One tunnel encap next hop per endpoint, all over the VNET's tunnel.
        ASSERT_EQ(m_rt.nexthops.size(), 3U);
        ASSERT_EQ(m_tun.tunnels.size(), 1U);
        for (const auto &nh : m_rt.nexthops)
        {
            EXPECT_EQ(nh.type, SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP);
            EXPECT_EQ(nh.tunnel_id, m_tun.tunnels[0].oid);
        }
        for (const char *ep : {"7.0.0.1", "7.0.0.2", "7.0.0.3"})
        {
            bool found = false;
            for (const auto &nh : m_rt.nexthops)
            {
                if (ipAddrEquals(nh.ip, ep)) found = true;
            }
            EXPECT_TRUE(found) << "missing tunnel next hop for endpoint " << ep;
        }

        // One next hop group. Orchagent programs SAI_NEXT_HOP_GROUP_TYPE_ECMP,
        // which is the same enum value libsairedis serializes to the
        // ..._DYNAMIC_UNORDERED_ECMP string the VS test matches.
        ASSERT_EQ(m_rt.groups.size(), 1U);
        const sai_object_id_t nhg = m_rt.groups[0].oid;
        EXPECT_EQ(m_rt.groups[0].type, SAI_NEXT_HOP_GROUP_TYPE_ECMP);

        // One member per endpoint, all bound to the group, each referencing a
        // captured tunnel next hop.
        ASSERT_EQ(m_rt.members.size(), 3U);
        set<sai_object_id_t> nhOids;
        for (const auto &nh : m_rt.nexthops) nhOids.insert(nh.oid);
        for (const auto &m : m_rt.members)
        {
            EXPECT_EQ(m.nhg, nhg);
            EXPECT_EQ(nhOids.count(m.nh), 1U);
        }

        // The route points at the group, in the VNET's virtual router.
        const RouteCaptures::Route *r = findRoute("100.100.1.1");
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(r->vr, vnetVr);
        EXPECT_EQ(r->next_hop_id, nhg);
    }

    // Deleting a single-endpoint route removes its route entry and the tunnel
    // next hop (vnet_lib.check_del_vnet_routes()). The delete flows through the
    // consumer as a DEL_COMMAND so the delete handler actually runs.
    TEST_F(VNetOrchTest, VnetRouteDeleteRemovesObjects)
    {
        setVxlanTunnel("tunnel_v4", "10.10.10.10");
        setVnet("Vnet_2000", "tunnel_v4", "2000", "");
        setVnetRoute("Vnet_2000", "100.100.1.1/32", "10.10.10.1");
        ASSERT_EQ(m_rt.nexthops.size(), 1U);
        const sai_object_id_t nhOid = m_rt.nexthops[0].oid;

        delVnetRoute("Vnet_2000", "100.100.1.1/32");

        bool routeRemoved = false;
        for (const auto &d : m_rt.removedRoutes)
        {
            if (prefixAddrEquals(d, "100.100.1.1")) routeRemoved = true;
        }
        EXPECT_TRUE(routeRemoved);
        EXPECT_NE(find(m_rt.removedNexthops.begin(), m_rt.removedNexthops.end(), nhOid),
                  m_rt.removedNexthops.end());
    }

    // Re-applying an identical multi-endpoint IPv6 VNET route is idempotent: the
    // second SET reuses the existing next hop group rather than creating a new
    // one, and STATE_DB still reports the same active endpoints -- the mock
    // equivalent of test_vnet_orch_13 (check_vnet_ecmp_routes with route_ids
    // reused + "only one group is present"). The default VNET does not advertise
    // the prefix.
    TEST_F(VNetOrchTest, VnetEcmpRouteReaddIsIdempotent)
    {
        setVxlanTunnel("tunnel_13", "fd:8::32");
        setVnet("Vnet13", "tunnel_13", "10008", "");

        setVnetRoute("Vnet13", "fd:8:10::32/128", "fd:8:1::1,fd:8:1::2,fd:8:1::3");

        // One ECMP group with three members on the first apply.
        ASSERT_EQ(m_rt.groups.size(), 1U);
        const sai_object_id_t nhg = m_rt.groups[0].oid;
        EXPECT_EQ(m_rt.members.size(), 3U);
        const RouteCaptures::Route *r = findRoute("fd:8:10::32");
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(r->next_hop_id, nhg);
        checkStateDbRoute("Vnet13", "fd:8:10::32/128", "fd:8:1::1,fd:8:1::2,fd:8:1::3");
        checkRouteNotAdvertised("fd:8:10::32/128");

        // Re-apply the identical route: no new group or members, same group OID,
        // nothing removed.
        setVnetRoute("Vnet13", "fd:8:10::32/128", "fd:8:1::1,fd:8:1::2,fd:8:1::3");
        EXPECT_EQ(m_rt.groups.size(), 1U);
        EXPECT_EQ(m_rt.members.size(), 3U);
        EXPECT_TRUE(m_rt.removedGroups.empty());
        r = findRoute("fd:8:10::32");
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(r->next_hop_id, nhg);
        checkStateDbRoute("Vnet13", "fd:8:10::32/128", "fd:8:1::1,fd:8:1::2,fd:8:1::3");
        checkRouteNotAdvertised("fd:8:10::32/128");

        // Deleting removes the route, its group and its STATE_DB entry.
        delVnetRoute("Vnet13", "fd:8:10::32/128");
        EXPECT_NE(find(m_rt.removedGroups.begin(), m_rt.removedGroups.end(), nhg),
                  m_rt.removedGroups.end());
        checkStateDbRouteRemoved("Vnet13", "fd:8:10::32/128");
        checkRouteNotAdvertised("fd:8:10::32/128");
    }

    // Changing a multi-endpoint route's endpoint set replaces the next hop
    // group: a new group is created for the new endpoints, the old group is
    // removed, and the route repoints -- the mock equivalent of
    // test_vnet_orch_14 (re-add idempotent, then endpoint update swaps the
    // group). Deleting twice is idempotent (the second delete is a no-op).
    TEST_F(VNetOrchTest, VnetEcmpRouteEndpointUpdateReplacesGroup)
    {
        setVxlanTunnel("tunnel_14", "fd:8::32");
        setVnet("Vnet14", "tunnel_14", "10008", "");

        setVnetRoute("Vnet14", "fd:8:10::32/128", "fd:8:1::1,fd:8:1::2,fd:8:1::3");
        ASSERT_EQ(m_rt.groups.size(), 1U);
        const sai_object_id_t oldNhg = m_rt.groups[0].oid;
        checkStateDbRoute("Vnet14", "fd:8:10::32/128", "fd:8:1::1,fd:8:1::2,fd:8:1::3");

        // Re-apply identical endpoints: still one group (idempotent).
        setVnetRoute("Vnet14", "fd:8:10::32/128", "fd:8:1::1,fd:8:1::2,fd:8:1::3");
        EXPECT_EQ(m_rt.groups.size(), 1U);
        EXPECT_TRUE(m_rt.removedGroups.empty());

        // Update the endpoint set (add a fourth endpoint): a new four-member
        // group is created and the old group removed; the route repoints.
        setVnetRoute("Vnet14", "fd:8:10::32/128",
                     "fd:8:1::1,fd:8:1::2,fd:8:1::3,fd:8:1::4");
        ASSERT_EQ(m_rt.groups.size(), 2U);
        const sai_object_id_t newNhg = m_rt.groups[1].oid;
        EXPECT_NE(newNhg, oldNhg);
        EXPECT_NE(find(m_rt.removedGroups.begin(), m_rt.removedGroups.end(), oldNhg),
                  m_rt.removedGroups.end());
        int newMembers = 0;
        for (const auto &m : m_rt.members) if (m.nhg == newNhg) newMembers++;
        EXPECT_EQ(newMembers, 4);
        const RouteCaptures::Route *r = findRoute("fd:8:10::32");
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(r->next_hop_id, newNhg);
        checkStateDbRoute("Vnet14", "fd:8:10::32/128",
                          "fd:8:1::1,fd:8:1::2,fd:8:1::3,fd:8:1::4");
        checkRouteNotAdvertised("fd:8:10::32/128");

        // Delete, then delete again: the second delete removes nothing more.
        delVnetRoute("Vnet14", "fd:8:10::32/128");
        checkStateDbRouteRemoved("Vnet14", "fd:8:10::32/128");
        EXPECT_NE(find(m_rt.removedGroups.begin(), m_rt.removedGroups.end(), newNhg),
                  m_rt.removedGroups.end());
        const size_t removedGroupsAfterFirst = m_rt.removedGroups.size();

        delVnetRoute("Vnet14", "fd:8:10::32/128");
        EXPECT_EQ(m_rt.removedGroups.size(), removedGroupsAfterFirst);
        checkStateDbRouteRemoved("Vnet14", "fd:8:10::32/128");
    }

    // Re-applying an identical single-endpoint IPv6 VNET route is idempotent:
    // the second SET reuses the existing tunnel next hop and creates no group --
    // the mock equivalent of test_vnet_orch_15 ("only one" next hop present).
    TEST_F(VNetOrchTest, VnetSingleRouteReaddIsIdempotent)
    {
        setVxlanTunnel("tunnel_15", "fd:8::32");
        setVnet("Vnet15", "tunnel_15", "10008", "");

        setVnetRoute("Vnet15", "fd:8:10::32/128", "fd:8:1::1");
        ASSERT_EQ(m_rt.nexthops.size(), 1U);
        const sai_object_id_t nhOid = m_rt.nexthops[0].oid;
        EXPECT_TRUE(m_rt.groups.empty());
        const RouteCaptures::Route *r = findRoute("fd:8:10::32");
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(r->next_hop_id, nhOid);
        checkStateDbRoute("Vnet15", "fd:8:10::32/128", "fd:8:1::1");
        checkRouteNotAdvertised("fd:8:10::32/128");

        // Re-apply identical: no new next hop, still no group.
        setVnetRoute("Vnet15", "fd:8:10::32/128", "fd:8:1::1");
        EXPECT_EQ(m_rt.nexthops.size(), 1U);
        EXPECT_TRUE(m_rt.groups.empty());
        r = findRoute("fd:8:10::32");
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(r->next_hop_id, nhOid);
        checkStateDbRoute("Vnet15", "fd:8:10::32/128", "fd:8:1::1");

        // Deleting removes the route, the tunnel next hop and its STATE_DB entry.
        delVnetRoute("Vnet15", "fd:8:10::32/128");
        bool routeRemoved = false;
        for (const auto &d : m_rt.removedRoutes)
            if (prefixAddrEquals(d, "fd:8:10::32")) routeRemoved = true;
        EXPECT_TRUE(routeRemoved);
        EXPECT_NE(find(m_rt.removedNexthops.begin(), m_rt.removedNexthops.end(), nhOid),
                  m_rt.removedNexthops.end());
        checkStateDbRouteRemoved("Vnet15", "fd:8:10::32/128");
    }

    // BFD-monitored ECMP VNET route lifecycle -- the mock equivalent of
    // test_vnet_orch_9 (IPv4). With default BFD monitoring an endpoint only
    // joins the route's group while its monitor's BFD session is UP:
    //  * created with every session DOWN -> route not programmed, STATE_DB
    //    inactive, but a BFD session exists per monitor;
    //  * sessions UP -> route programmed over the ECMP group with one member
    //    per UP endpoint, STATE_DB active with those endpoints;
    //  * a session DOWN drops just that member; back UP re-adds it;
    //  * all DOWN -> route removed, STATE_DB inactive, sessions still present;
    //  * deleting the route removes the group and every BFD session.
    TEST_F(VNetOrchTest, VnetMonitoredEcmpRouteFollowsBfdState)
    {
        setVxlanTunnel("tunnel_9", "9.9.9.9");
        setVnet("Vnet9", "tunnel_9", "10009", "");

        setVnetRouteMonitored("Vnet9", "100.100.1.1/32",
                              "9.0.0.1,9.0.0.2,9.0.0.3", "9.1.0.1,9.1.0.2,9.1.0.3");

        // Default BFD state is down: the route is not programmed and STATE_DB
        // reports it inactive, but a BFD session exists for each monitor.
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRoute("Vnet9", "100.100.1.1/32", "");
        checkRouteNotAdvertised("100.100.1.1/32");
        for (const char *mon : {"9.1.0.1", "9.1.0.2", "9.1.0.3"})
            EXPECT_TRUE(bfdSessionExists(mon)) << "missing BFD session " << mon;

        // All sessions up: the route is programmed over the ECMP group with a
        // member per endpoint; STATE_DB lists all three active.
        updateBfdSessionState("9.1.0.1", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("9.1.0.2", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("9.1.0.3", SAI_BFD_SESSION_STATE_UP);
        const RouteCaptures::Route *r = findRoute("100.100.1.1");
        ASSERT_NE(r, nullptr);
        const sai_object_id_t nhg = r->next_hop_id;
        EXPECT_NE(nhg, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(activeMembers(nhg), 3U);
        checkStateDbRoute("Vnet9", "100.100.1.1/32", "9.0.0.1,9.0.0.2,9.0.0.3");
        checkRouteNotAdvertised("100.100.1.1/32");

        // One endpoint's session goes down: only that member is dropped.
        updateBfdSessionState("9.1.0.2", SAI_BFD_SESSION_STATE_DOWN);
        EXPECT_EQ(activeMembers(nhg), 2U);
        checkStateDbRoute("Vnet9", "100.100.1.1/32", "9.0.0.1,9.0.0.3");

        // Back up: the member is re-added, all three active again.
        updateBfdSessionState("9.1.0.2", SAI_BFD_SESSION_STATE_UP);
        EXPECT_EQ(activeMembers(nhg), 3U);
        checkStateDbRoute("Vnet9", "100.100.1.1/32", "9.0.0.1,9.0.0.2,9.0.0.3");

        // All endpoints down: the route is removed and STATE_DB goes inactive,
        // but the BFD sessions remain (only route deletion removes them).
        updateBfdSessionState("9.1.0.1", SAI_BFD_SESSION_STATE_DOWN);
        updateBfdSessionState("9.1.0.2", SAI_BFD_SESSION_STATE_DOWN);
        updateBfdSessionState("9.1.0.3", SAI_BFD_SESSION_STATE_DOWN);
        EXPECT_EQ(activeMembers(nhg), 0U);
        bool routeRemoved = false;
        for (const auto &d : m_rt.removedRoutes)
            if (prefixAddrEquals(d, "100.100.1.1")) routeRemoved = true;
        EXPECT_TRUE(routeRemoved);
        checkStateDbRoute("Vnet9", "100.100.1.1/32", "");
        for (const char *mon : {"9.1.0.1", "9.1.0.2", "9.1.0.3"})
            EXPECT_TRUE(bfdSessionExists(mon));

        // Deleting the route removes the group, its STATE_DB entry, and the BFD
        // sessions for all its monitors.
        delVnetRouteMonitored("Vnet9", "100.100.1.1/32");
        EXPECT_NE(find(m_rt.removedGroups.begin(), m_rt.removedGroups.end(), nhg),
                  m_rt.removedGroups.end());
        checkStateDbRouteRemoved("Vnet9", "100.100.1.1/32");
        for (const char *mon : {"9.1.0.1", "9.1.0.2", "9.1.0.3"})
            EXPECT_FALSE(bfdSessionExists(mon)) << "BFD session " << mon << " not removed";
    }

    // BFD-monitored IPv6 ECMP routes with two overlapping routes, a
    // make-before-break group change, and shared/ref-counted BFD sessions --
    // the mock equivalent of test_vnet_orch_10. Endpoint fd:10:1::N is paired
    // (by list position) with monitor fd:10:2::N, so bringing monitor N up/down
    // adds/drops endpoint N.
    TEST_F(VNetOrchTest, VnetMonitoredEcmpRoutesIpv6Overlap)
    {
        setVxlanTunnel("tunnel_10", "fd:10::32");
        setVnet("Vnet10", "tunnel_10", "10010", "");

        // Route 1 over endpoints 1/2/3, all monitors down: not programmed.
        setVnetRouteMonitored("Vnet10", "fd:10:10::1/128",
                              "fd:10:1::1,fd:10:1::2,fd:10:1::3",
                              "fd:10:2::1,fd:10:2::2,fd:10:2::3");
        EXPECT_EQ(findRoute("fd:10:10::1"), nullptr);
        checkStateDbRoute("Vnet10", "fd:10:10::1/128", "");
        for (const char *mon : {"fd:10:2::1", "fd:10:2::2", "fd:10:2::3"})
            EXPECT_TRUE(bfdSessionExists(mon)) << "missing BFD session " << mon;

        // All three up: route 1 programmed over the group with three members.
        updateBfdSessionState("fd:10:2::1", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("fd:10:2::2", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("fd:10:2::3", SAI_BFD_SESSION_STATE_UP);
        const RouteCaptures::Route *r1 = findRoute("fd:10:10::1");
        ASSERT_NE(r1, nullptr);
        const sai_object_id_t nhg1 = r1->next_hop_id;
        EXPECT_EQ(activeMembers(nhg1), 3U);
        checkStateDbRoute("Vnet10", "fd:10:10::1/128",
                          "fd:10:1::1,fd:10:1::2,fd:10:1::3");

        // Endpoint 2's monitor goes down: dropped from route 1.
        updateBfdSessionState("fd:10:2::2", SAI_BFD_SESSION_STATE_DOWN);
        EXPECT_EQ(activeMembers(nhg1), 2U);
        checkStateDbRoute("Vnet10", "fd:10:10::1/128", "fd:10:1::1,fd:10:1::3");

        // Route 2 over endpoints 1/2/5 -- overlaps route 1 on 1/2 (whose BFD
        // sessions are shared) and adds a new session for monitor 5. Only
        // endpoint 1 is up, so route 2 starts with a single member.
        setVnetRouteMonitored("Vnet10", "fd:10:20::1/128",
                              "fd:10:1::1,fd:10:1::2,fd:10:1::5",
                              "fd:10:2::1,fd:10:2::2,fd:10:2::5");
        const RouteCaptures::Route *r2 = findRoute("fd:10:20::1");
        ASSERT_NE(r2, nullptr);
        const sai_object_id_t nhg2 = r2->next_hop_id;
        EXPECT_EQ(activeMembers(nhg2), 1U);
        checkStateDbRoute("Vnet10", "fd:10:20::1/128", "fd:10:1::1");
        EXPECT_TRUE(bfdSessionExists("fd:10:2::5"));

        // Endpoint 5 up: route 2 now has two members.
        updateBfdSessionState("fd:10:2::5", SAI_BFD_SESSION_STATE_UP);
        EXPECT_EQ(activeMembers(nhg2), 2U);
        checkStateDbRoute("Vnet10", "fd:10:20::1/128", "fd:10:1::1,fd:10:1::5");

        // Endpoint 3 down, endpoint 2 back up: route 1 -> {1,2} (same group).
        updateBfdSessionState("fd:10:2::3", SAI_BFD_SESSION_STATE_DOWN);
        updateBfdSessionState("fd:10:2::2", SAI_BFD_SESSION_STATE_UP);
        EXPECT_EQ(activeMembers(nhg1), 2U);
        checkStateDbRoute("Vnet10", "fd:10:10::1/128", "fd:10:1::1,fd:10:1::2");

        // Make-before-break: change route 1's endpoint set to add endpoint 4.
        // A new group is created (with the currently-up subset {1,2}); the route
        // repoints to it and the old group is removed. Bringing endpoint 4 up
        // then adds its member.
        setVnetRouteMonitored("Vnet10", "fd:10:10::1/128",
                              "fd:10:1::1,fd:10:1::2,fd:10:1::3,fd:10:1::4",
                              "fd:10:2::1,fd:10:2::2,fd:10:2::3,fd:10:2::4");
        updateBfdSessionState("fd:10:2::4", SAI_BFD_SESSION_STATE_UP);
        const RouteCaptures::Route *r1b = findRoute("fd:10:10::1");
        ASSERT_NE(r1b, nullptr);
        const sai_object_id_t nhg1b = r1b->next_hop_id;
        EXPECT_NE(nhg1b, nhg1);
        EXPECT_NE(find(m_rt.removedGroups.begin(), m_rt.removedGroups.end(), nhg1),
                  m_rt.removedGroups.end());
        EXPECT_EQ(activeMembers(nhg1b), 3U);
        checkStateDbRoute("Vnet10", "fd:10:10::1/128",
                          "fd:10:1::1,fd:10:1::2,fd:10:1::4");

        // Endpoint 3 back up: all four members active on the new group.
        updateBfdSessionState("fd:10:2::3", SAI_BFD_SESSION_STATE_UP);
        EXPECT_EQ(activeMembers(nhg1b), 4U);
        checkStateDbRoute("Vnet10", "fd:10:10::1/128",
                          "fd:10:1::1,fd:10:1::2,fd:10:1::3,fd:10:1::4");

        // All of route 1's endpoints down: route 1 removed. Route 2 keeps only
        // endpoint 5 (its monitors 1/2 are now down).
        updateBfdSessionState("fd:10:2::1", SAI_BFD_SESSION_STATE_DOWN);
        updateBfdSessionState("fd:10:2::2", SAI_BFD_SESSION_STATE_DOWN);
        updateBfdSessionState("fd:10:2::3", SAI_BFD_SESSION_STATE_DOWN);
        updateBfdSessionState("fd:10:2::4", SAI_BFD_SESSION_STATE_DOWN);
        bool route1Removed = false;
        for (const auto &d : m_rt.removedRoutes)
            if (prefixAddrEquals(d, "fd:10:10::1")) route1Removed = true;
        EXPECT_TRUE(route1Removed);
        checkStateDbRoute("Vnet10", "fd:10:10::1/128", "");
        EXPECT_EQ(activeMembers(nhg2), 1U);
        checkStateDbRoute("Vnet10", "fd:10:20::1/128", "fd:10:1::5");

        // Delete route 2: its group is removed and only its unique BFD session
        // (monitor 5) is torn down -- monitors 1-4 are still held by route 1.
        delVnetRouteMonitored("Vnet10", "fd:10:20::1/128");
        EXPECT_NE(find(m_rt.removedGroups.begin(), m_rt.removedGroups.end(), nhg2),
                  m_rt.removedGroups.end());
        checkStateDbRouteRemoved("Vnet10", "fd:10:20::1/128");
        EXPECT_FALSE(bfdSessionExists("fd:10:2::5"));
        for (const char *mon : {"fd:10:2::1", "fd:10:2::2", "fd:10:2::3", "fd:10:2::4"})
            EXPECT_TRUE(bfdSessionExists(mon)) << "BFD session " << mon << " removed too early";

        // Delete route 1: the new group and all its remaining BFD sessions go.
        delVnetRouteMonitored("Vnet10", "fd:10:10::1/128");
        EXPECT_NE(find(m_rt.removedGroups.begin(), m_rt.removedGroups.end(), nhg1b),
                  m_rt.removedGroups.end());
        checkStateDbRouteRemoved("Vnet10", "fd:10:10::1/128");
        for (const char *mon : {"fd:10:2::1", "fd:10:2::2", "fd:10:2::3", "fd:10:2::4"})
            EXPECT_FALSE(bfdSessionExists(mon)) << "BFD session " << mon << " not removed";
    }

    // Re-applying an identical single-endpoint BFD-monitored route is
    // idempotent -- the mock equivalent of test_vnet_orch_16. A monitored
    // single-endpoint route programs one tunnel next hop while its BFD session
    // is up; re-applying it (up, or while down then back up) neither leaks nor
    // duplicates the next hop, and deleting it removes the next hop.
    TEST_F(VNetOrchTest, VnetMonitoredSingleRouteReaddIsIdempotent)
    {
        setVxlanTunnel("tunnel_16", "fd:8::33");
        setVnet("Vnet16", "tunnel_16", "10008", "");

        auto activeNexthops = [&]() {
            size_t c = 0;
            for (const auto &nh : m_rt.nexthops)
                if (find(m_rt.removedNexthops.begin(), m_rt.removedNexthops.end(),
                         nh.oid) == m_rt.removedNexthops.end())
                    c++;
            return c;
        };

        setVnetRouteMonitored("Vnet16", "fd:8:11::32/128", "fd:8:2::1", "fd:8:2::1");
        updateBfdSessionState("fd:8:2::1", SAI_BFD_SESSION_STATE_UP);
        const RouteCaptures::Route *r = findRoute("fd:8:11::32");
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(activeNexthops(), 1U);
        EXPECT_TRUE(m_rt.groups.empty());
        checkStateDbRoute("Vnet16", "fd:8:11::32/128", "fd:8:2::1");
        checkRouteNotAdvertised("fd:8:11::32/128");

        // Re-apply the identical route while up: still exactly one next hop.
        setVnetRouteMonitored("Vnet16", "fd:8:11::32/128", "fd:8:2::1", "fd:8:2::1");
        EXPECT_EQ(activeNexthops(), 1U);
        EXPECT_TRUE(m_rt.groups.empty());
        checkStateDbRoute("Vnet16", "fd:8:11::32/128", "fd:8:2::1");

        // Session down: route deprogrammed. Re-apply while down, then bring it
        // back up: the route is programmed again and STATE_DB reports it active.
        updateBfdSessionState("fd:8:2::1", SAI_BFD_SESSION_STATE_DOWN);
        checkStateDbRoute("Vnet16", "fd:8:11::32/128", "");
        setVnetRouteMonitored("Vnet16", "fd:8:11::32/128", "fd:8:2::1", "fd:8:2::1");
        updateBfdSessionState("fd:8:2::1", SAI_BFD_SESSION_STATE_UP);
        checkStateDbRoute("Vnet16", "fd:8:11::32/128", "fd:8:2::1");
        checkRouteNotAdvertised("fd:8:11::32/128");

        // Deleting the route removes the tunnel next hop and its STATE_DB entry,
        // and the BFD session for its monitor.
        delVnetRouteMonitored("Vnet16", "fd:8:11::32/128");
        EXPECT_EQ(activeNexthops(), 0U);
        checkStateDbRouteRemoved("Vnet16", "fd:8:11::32/128");
        EXPECT_FALSE(bfdSessionExists("fd:8:2::1"));
    }

    // Re-applying an identical multi-endpoint BFD-monitored route is idempotent
    // -- the mock equivalent of test_vnet_orch_17. Re-applying while all
    // monitors are down keeps the route unprogrammed; re-applying while up
    // reuses the same next hop group (no new group, none removed); deleting
    // removes the group and all its BFD sessions.
    TEST_F(VNetOrchTest, VnetMonitoredEcmpRouteReaddIsIdempotent)
    {
        setVxlanTunnel("tunnel_17", "9.9.9.9");
        setVnet("Vnet17", "tunnel_17", "10017", "");

        setVnetRouteMonitored("Vnet17", "100.100.1.1/32",
                              "9.0.0.1,9.0.0.2,9.0.0.3", "9.1.0.1,9.1.0.2,9.1.0.3");
        // All monitors down: not programmed. Re-apply -- still not programmed.
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRoute("Vnet17", "100.100.1.1/32", "");
        setVnetRouteMonitored("Vnet17", "100.100.1.1/32",
                              "9.0.0.1,9.0.0.2,9.0.0.3", "9.1.0.1,9.1.0.2,9.1.0.3");
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRoute("Vnet17", "100.100.1.1/32", "");

        // All up: one group with three members.
        updateBfdSessionState("9.1.0.1", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("9.1.0.2", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("9.1.0.3", SAI_BFD_SESSION_STATE_UP);
        const RouteCaptures::Route *r = findRoute("100.100.1.1");
        ASSERT_NE(r, nullptr);
        const sai_object_id_t nhg = r->next_hop_id;
        EXPECT_EQ(activeMembers(nhg), 3U);
        const size_t groupsAfterUp = m_rt.groups.size();
        checkStateDbRoute("Vnet17", "100.100.1.1/32", "9.0.0.1,9.0.0.2,9.0.0.3");

        // Re-apply the identical active route: same group, no new group, none
        // removed.
        setVnetRouteMonitored("Vnet17", "100.100.1.1/32",
                              "9.0.0.1,9.0.0.2,9.0.0.3", "9.1.0.1,9.1.0.2,9.1.0.3");
        r = findRoute("100.100.1.1");
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(r->next_hop_id, nhg);
        EXPECT_EQ(m_rt.groups.size(), groupsAfterUp);
        EXPECT_TRUE(m_rt.removedGroups.empty());
        EXPECT_EQ(activeMembers(nhg), 3U);
        checkStateDbRoute("Vnet17", "100.100.1.1/32", "9.0.0.1,9.0.0.2,9.0.0.3");

        // Deleting removes the group, its STATE_DB entry and all BFD sessions.
        delVnetRouteMonitored("Vnet17", "100.100.1.1/32");
        EXPECT_NE(find(m_rt.removedGroups.begin(), m_rt.removedGroups.end(), nhg),
                  m_rt.removedGroups.end());
        checkStateDbRouteRemoved("Vnet17", "100.100.1.1/32");
        for (const char *mon : {"9.1.0.1", "9.1.0.2", "9.1.0.3"})
            EXPECT_FALSE(bfdSessionExists(mon)) << "BFD session " << mon << " not removed";
    }

    // BFD-monitored ECMP routes on an advertise_prefix VNET -- the mock
    // equivalent of test_vnet_orch_12. On a VNET created with
    // advertise_prefix=true, an active monitored route is published to STATE_DB
    // ADVERTISE_NETWORK_TABLE (carrying its route profile); the advertisement
    // follows the route's active/inactive state and its profile follows a
    // make-before-break endpoint/profile change.
    TEST_F(VNetOrchTest, VnetMonitoredEcmpRouteAdvertisesPrefix)
    {
        setVxlanTunnel("tunnel_12", "12.12.12.12");
        setVnet("Vnet12", "tunnel_12", "10012", "", /*advertise_prefix=*/true);

        // route1 over 1/2/3 with a profile, all monitors down: not programmed,
        // not advertised.
        setVnetRouteMonitored("Vnet12", "100.100.1.1/32",
                              "12.0.0.1,12.0.0.2,12.0.0.3",
                              "12.1.0.1,12.1.0.2,12.1.0.3", "test_profile");
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRoute("Vnet12", "100.100.1.1/32", "");
        checkRouteNotAdvertised("100.100.1.1/32");

        // All up: programmed over a 3-member group and advertised with its
        // profile.
        updateBfdSessionState("12.1.0.1", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("12.1.0.2", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("12.1.0.3", SAI_BFD_SESSION_STATE_UP);
        const RouteCaptures::Route *r1 = findRoute("100.100.1.1");
        ASSERT_NE(r1, nullptr);
        const sai_object_id_t nhg1 = r1->next_hop_id;
        EXPECT_EQ(activeMembers(nhg1), 3U);
        checkStateDbRoute("Vnet12", "100.100.1.1/32", "12.0.0.1,12.0.0.2,12.0.0.3");
        checkRouteAdvertised("100.100.1.1/32", "test_profile");

        // Endpoint 2 down: still active/advertised over the remaining members.
        updateBfdSessionState("12.1.0.2", SAI_BFD_SESSION_STATE_DOWN);
        EXPECT_EQ(activeMembers(nhg1), 2U);
        checkStateDbRoute("Vnet12", "100.100.1.1/32", "12.0.0.1,12.0.0.3");
        checkRouteAdvertised("100.100.1.1/32", "test_profile");

        // Overlapping route2 over 1/2/5 (no profile), only endpoint 1 up.
        setVnetRouteMonitored("Vnet12", "100.100.2.1/32",
                              "12.0.0.1,12.0.0.2,12.0.0.5",
                              "12.1.0.1,12.1.0.2,12.1.0.5");
        const RouteCaptures::Route *r2 = findRoute("100.100.2.1");
        ASSERT_NE(r2, nullptr);
        const sai_object_id_t nhg2 = r2->next_hop_id;
        EXPECT_EQ(activeMembers(nhg2), 1U);
        checkStateDbRoute("Vnet12", "100.100.2.1/32", "12.0.0.1");
        checkRouteAdvertised("100.100.1.1/32");

        // Endpoint 5 up: route2 grows and is advertised (no profile).
        updateBfdSessionState("12.1.0.5", SAI_BFD_SESSION_STATE_UP);
        EXPECT_EQ(activeMembers(nhg2), 2U);
        checkStateDbRoute("Vnet12", "100.100.2.1/32", "12.0.0.1,12.0.0.5");
        checkRouteAdvertised("100.100.2.1/32");

        // Endpoint 3 down: route1 down to endpoint 1 only, still advertised.
        updateBfdSessionState("12.1.0.3", SAI_BFD_SESSION_STATE_DOWN);
        EXPECT_EQ(activeMembers(nhg1), 1U);
        checkStateDbRoute("Vnet12", "100.100.1.1/32", "12.0.0.1");
        checkRouteAdvertised("100.100.1.1/32", "test_profile");

        // Make-before-break with a new profile: route1 -> 1/2/3/4, profile
        // test_profile2; bring endpoint 4 up. New group, old group removed, and
        // the advertisement now carries the new profile.
        setVnetRouteMonitored("Vnet12", "100.100.1.1/32",
                              "12.0.0.1,12.0.0.2,12.0.0.3,12.0.0.4",
                              "12.1.0.1,12.1.0.2,12.1.0.3,12.1.0.4", "test_profile2");
        updateBfdSessionState("12.1.0.4", SAI_BFD_SESSION_STATE_UP);
        const RouteCaptures::Route *r1b = findRoute("100.100.1.1");
        ASSERT_NE(r1b, nullptr);
        const sai_object_id_t nhg1b = r1b->next_hop_id;
        EXPECT_NE(nhg1b, nhg1);
        EXPECT_NE(find(m_rt.removedGroups.begin(), m_rt.removedGroups.end(), nhg1),
                  m_rt.removedGroups.end());
        EXPECT_EQ(activeMembers(nhg1b), 2U);
        checkStateDbRoute("Vnet12", "100.100.1.1/32", "12.0.0.1,12.0.0.4");
        checkRouteAdvertised("100.100.1.1/32", "test_profile2");

        // Endpoint 2 back up: route1 has three members, still test_profile2.
        updateBfdSessionState("12.1.0.2", SAI_BFD_SESSION_STATE_UP);
        EXPECT_EQ(activeMembers(nhg1b), 3U);
        checkStateDbRoute("Vnet12", "100.100.1.1/32", "12.0.0.1,12.0.0.2,12.0.0.4");
        checkRouteAdvertised("100.100.1.1/32", "test_profile2");

        // All of route1's monitors down: route1 removed and its advertisement
        // withdrawn; route2 keeps only endpoint 5 and stays advertised.
        updateBfdSessionState("12.1.0.1", SAI_BFD_SESSION_STATE_DOWN);
        updateBfdSessionState("12.1.0.2", SAI_BFD_SESSION_STATE_DOWN);
        updateBfdSessionState("12.1.0.3", SAI_BFD_SESSION_STATE_DOWN);
        updateBfdSessionState("12.1.0.4", SAI_BFD_SESSION_STATE_DOWN);
        checkStateDbRoute("Vnet12", "100.100.1.1/32", "");
        checkRouteNotAdvertised("100.100.1.1/32");
        EXPECT_EQ(activeMembers(nhg2), 1U);
        checkStateDbRoute("Vnet12", "100.100.2.1/32", "12.0.0.5");
        checkRouteAdvertised("100.100.2.1/32");

        // Delete route2: group + its unique session (5) gone; route1's monitors
        // remain.
        delVnetRouteMonitored("Vnet12", "100.100.2.1/32");
        EXPECT_NE(find(m_rt.removedGroups.begin(), m_rt.removedGroups.end(), nhg2),
                  m_rt.removedGroups.end());
        checkStateDbRouteRemoved("Vnet12", "100.100.2.1/32");
        checkRouteNotAdvertised("100.100.2.1/32");
        EXPECT_FALSE(bfdSessionExists("12.1.0.5"));
        for (const char *mon : {"12.1.0.1", "12.1.0.2", "12.1.0.3", "12.1.0.4"})
            EXPECT_TRUE(bfdSessionExists(mon)) << "BFD session " << mon << " removed too early";

        // Delete route1: new group and all remaining sessions gone.
        delVnetRouteMonitored("Vnet12", "100.100.1.1/32");
        EXPECT_NE(find(m_rt.removedGroups.begin(), m_rt.removedGroups.end(), nhg1b),
                  m_rt.removedGroups.end());
        checkStateDbRouteRemoved("Vnet12", "100.100.1.1/32");
        for (const char *mon : {"12.1.0.1", "12.1.0.2", "12.1.0.3", "12.1.0.4"})
            EXPECT_FALSE(bfdSessionExists(mon)) << "BFD session " << mon << " not removed";
    }

    // Mixed single-endpoint and ECMP BFD-monitored routes that share BFD
    // sessions -- the mock equivalent of test_vnet_orch_11. A single-endpoint
    // monitored route programs one tunnel next hop (no group); an overlapping
    // ECMP monitored route programs a group whose members follow BFD state; and
    // a monitor shared between routes is a single ref-counted BFD session. Run
    // for both unordered and ordered ECMP (ordered adds per-member sequence
    // IDs), mirroring the VS test's ordered_ecmp parametrization.
    TEST_F(VNetOrchTest, VnetMixedMonitoredRoutesUnorderedEcmp)
    {
        runMixedMonitoredRoutes(/*ordered=*/false);
    }

    TEST_F(VNetOrchTest, VnetMixedMonitoredRoutesOrderedEcmp)
    {
        runMixedMonitoredRoutes(/*ordered=*/true);
    }

    // Priority (primary/secondary) custom-monitored VNET route failover -- the
    // mock equivalent of test_vnet_orch_18. Four endpoints with the first two as
    // primary; a per-endpoint custom monitor drives up/down through STATE_DB
    // VNET_MONITOR_TABLE (MonitorOrch -> VNetRouteOrch), with no SAI BFD
    // sessions. While any primary endpoint is up the route uses the active
    // primary subset; once all primaries are down it falls back to the active
    // secondary subset; once every endpoint is down the route is withdrawn.
    // The route carries an adv_prefix summary that is advertised to STATE_DB
    // ADVERTISE_NETWORK_TABLE whenever the route is active.
    TEST_F(VNetOrchTest, VnetPriorityRouteCustomMonitorFailover)
    {
        setVxlanTunnel("tunnel_18", "9.9.9.9");
        setVnet("Vnet18", "tunnel_18", "10018", "", /*advertise_prefix=*/true,
                /*overlay_dmac=*/"22:33:33:44:44:66");

        // 4 endpoints, primary = first two, custom monitoring, adv_prefix summary.
        setVnetRoutePriority("Vnet18", "100.100.1.1/32",
                             "9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4",
                             "9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4",
                             /*primary=*/"9.1.0.1,9.1.0.2",
                             /*monitoring=*/"custom",
                             /*adv_prefix=*/"100.100.1.0/24");

        // A monitor session is written to APP_DB for every endpoint, all down;
        // the route is not programmed and its adv_prefix is not advertised.
        for (const char *m : {"9.1.0.1", "9.1.0.2", "9.1.0.3", "9.1.0.4"})
            checkCustomMonitorAppDb("100.100.1.1/32", m, "vxlan", "22:33:33:44:44:66");
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRoute("Vnet18", "100.100.1.1/32", "");
        checkRouteNotAdvertised("100.100.1.0/24");

        // All monitors up: only the primary subset {1,2} is used.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.1", "up");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.2", "up");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.3", "up");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.4", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1", "9.1.0.2"});
        checkStateDbRoute("Vnet18", "100.100.1.1/32", "9.1.0.1,9.1.0.2");
        checkRouteAdvertised("100.100.1.0/24");

        // First primary down: route stays on the remaining primary {1}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.2", "down");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("Vnet18", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.0/24");

        // Both primaries down: fall back to the secondary subset {3,4}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.1", "down");
        checkPriorityRoute("100.100.1.1", {"9.1.0.3", "9.1.0.4"});
        checkStateDbRoute("Vnet18", "100.100.1.1/32", "9.1.0.3,9.1.0.4");
        checkRouteAdvertised("100.100.1.0/24");

        // First secondary down: route stays on the remaining secondary {4}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.3", "down");
        checkPriorityRoute("100.100.1.1", {"9.1.0.4"});
        checkStateDbRoute("Vnet18", "100.100.1.1/32", "9.1.0.4");
        checkRouteAdvertised("100.100.1.0/24");

        // Last secondary down: every endpoint is down, route withdrawn.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.4", "down");
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRouteRemoved("Vnet18", "100.100.1.1/32");
        checkRouteNotAdvertised("100.100.1.0/24");

        // Secondary endpoints back up: route re-added on secondary {3,4}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.3", "up");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.4", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.3", "9.1.0.4"});
        checkStateDbRoute("Vnet18", "100.100.1.1/32", "9.1.0.3,9.1.0.4");
        checkRouteAdvertised("100.100.1.0/24");

        // First primary back up: route switches back to the primary {1}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.1", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("Vnet18", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.0/24");

        // Second primary up: primary subset {1,2}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.2", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1", "9.1.0.2"});
        checkStateDbRoute("Vnet18", "100.100.1.1/32", "9.1.0.1,9.1.0.2");
        checkRouteAdvertised("100.100.1.0/24");

        // Secondary endpoints going down does not affect a primary-active route.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.3", "down");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.4", "down");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1", "9.1.0.2"});
        checkStateDbRoute("Vnet18", "100.100.1.1/32", "9.1.0.1,9.1.0.2");
        checkRouteAdvertised("100.100.1.0/24");

        // Secondary endpoints coming back up does not affect it either.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.3", "up");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.4", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1", "9.1.0.2"});
        checkStateDbRoute("Vnet18", "100.100.1.1/32", "9.1.0.1,9.1.0.2");
        checkRouteAdvertised("100.100.1.0/24");

        // Delete the route: withdrawn, STATE_DB cleared, and every custom
        // monitor session removed from APP_DB.
        delVnetRoute("Vnet18", "100.100.1.1/32");
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRouteRemoved("Vnet18", "100.100.1.1/32");
        checkRouteNotAdvertised("100.100.1.0/24");
        for (const char *m : {"9.1.0.1", "9.1.0.2", "9.1.0.3", "9.1.0.4"})
            checkCustomMonitorDeleted("100.100.1.1/32", m);
    }

    // Two overlapping priority routes with different primary subsets over the
    // same four endpoints -- the mock equivalent of test_vnet_orch_19. route1's
    // primary is {1,2} (advertised with a profile), route2's is {3,4}
    // (advertised without one) under a separate adv_prefix. Each route's monitors
    // are independent (keyed monitor|prefix), so the same endpoint IP can be up
    // for one route and down for the other. The two routes converge on the same
    // active set as endpoints flap, exercising primary-preference, secondary
    // fallback and per-route advertisement.
    TEST_F(VNetOrchTest, VnetTwoPriorityRoutesOverlappingGroups)
    {
        setVxlanTunnel("tunnel_19", "9.9.9.19");
        setVnet("Vnet19", "tunnel_19", "10019", "", /*advertise_prefix=*/true,
                "22:33:33:44:44:66");

        setVnetRoutePriority("Vnet19", "100.100.1.1/32",
                             "9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4",
                             "9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4",
                             /*primary=*/"9.1.0.1,9.1.0.2", "custom",
                             /*adv_prefix=*/"100.100.1.0/24", /*profile=*/"Test_profile");
        setVnetRoutePriority("Vnet19", "200.100.1.1/32",
                             "9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4",
                             "9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4",
                             /*primary=*/"9.1.0.3,9.1.0.4", "custom",
                             /*adv_prefix=*/"200.100.1.0/24");

        // All monitors down: neither route programmed, neither prefix advertised.
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRoute("Vnet19", "100.100.1.1/32", "");
        checkRouteNotAdvertised("100.100.1.0/24");
        EXPECT_EQ(findRoute("200.100.1.1"), nullptr);
        checkStateDbRoute("Vnet19", "200.100.1.1/32", "");
        checkRouteNotAdvertised("200.100.1.0/24");

        // route1 endpoint 1 up: route1 on its primary {1}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.1", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("Vnet19", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.0/24", "Test_profile");

        // route2 endpoint 1 up: 1 is secondary for route2, its primary {3,4} is
        // still down, so route2 falls back to {1}.
        updateMonitorSessionState("200.100.1.1/32", "9.1.0.1", "up");
        checkPriorityRoute("200.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("Vnet19", "200.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("200.100.1.0/24");

        // route1 endpoint 2 up: primary {1,2}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.2", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1", "9.1.0.2"});
        checkStateDbRoute("Vnet19", "100.100.1.1/32", "9.1.0.1,9.1.0.2");
        checkRouteAdvertised("100.100.1.0/24", "Test_profile");

        // route2 endpoint 2 up: primary {3,4} still down, secondary {1,2}.
        updateMonitorSessionState("200.100.1.1/32", "9.1.0.2", "up");
        checkPriorityRoute("200.100.1.1", {"9.1.0.1", "9.1.0.2"});
        checkStateDbRoute("Vnet19", "200.100.1.1/32", "9.1.0.1,9.1.0.2");
        checkRouteAdvertised("200.100.1.0/24");

        // endpoint 3 up for both: route1 unaffected (primary {1,2}); route2's
        // primary {3,4} now has 3 up, so it switches to {3}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.3", "up");
        updateMonitorSessionState("200.100.1.1/32", "9.1.0.3", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1", "9.1.0.2"});
        checkStateDbRoute("Vnet19", "100.100.1.1/32", "9.1.0.1,9.1.0.2");
        checkRouteAdvertised("100.100.1.0/24", "Test_profile");
        checkPriorityRoute("200.100.1.1", {"9.1.0.3"});
        checkStateDbRoute("Vnet19", "200.100.1.1/32", "9.1.0.3");
        checkRouteAdvertised("200.100.1.0/24");

        // endpoint 4 up for both: route1 still {1,2}; route2 primary {3,4}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.4", "up");
        updateMonitorSessionState("200.100.1.1/32", "9.1.0.4", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1", "9.1.0.2"});
        checkStateDbRoute("Vnet19", "100.100.1.1/32", "9.1.0.1,9.1.0.2");
        checkPriorityRoute("200.100.1.1", {"9.1.0.3", "9.1.0.4"});
        checkStateDbRoute("Vnet19", "200.100.1.1/32", "9.1.0.3,9.1.0.4");

        // endpoint 1 down for both: route1 primary drops to {2}; route2 keeps
        // its primary {3,4}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.1", "down");
        updateMonitorSessionState("200.100.1.1/32", "9.1.0.1", "down");
        checkPriorityRoute("100.100.1.1", {"9.1.0.2"});
        checkStateDbRoute("Vnet19", "100.100.1.1/32", "9.1.0.2");
        checkPriorityRoute("200.100.1.1", {"9.1.0.3", "9.1.0.4"});
        checkStateDbRoute("Vnet19", "200.100.1.1/32", "9.1.0.3,9.1.0.4");

        // endpoint 2 down for both: route1 primary all down -> secondary {3,4};
        // route2 unaffected.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.2", "down");
        updateMonitorSessionState("200.100.1.1/32", "9.1.0.2", "down");
        checkPriorityRoute("100.100.1.1", {"9.1.0.3", "9.1.0.4"});
        checkStateDbRoute("Vnet19", "100.100.1.1/32", "9.1.0.3,9.1.0.4");
        checkPriorityRoute("200.100.1.1", {"9.1.0.3", "9.1.0.4"});
        checkStateDbRoute("Vnet19", "200.100.1.1/32", "9.1.0.3,9.1.0.4");

        // endpoint 3 down for both: route1 secondary drops to {4}; route2 primary
        // drops to {4}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.3", "down");
        updateMonitorSessionState("200.100.1.1/32", "9.1.0.3", "down");
        checkPriorityRoute("100.100.1.1", {"9.1.0.4"});
        checkStateDbRoute("Vnet19", "100.100.1.1/32", "9.1.0.4");
        checkPriorityRoute("200.100.1.1", {"9.1.0.4"});
        checkStateDbRoute("Vnet19", "200.100.1.1/32", "9.1.0.4");

        // endpoint 4 down for both: every endpoint down, both routes withdrawn.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.4", "down");
        updateMonitorSessionState("200.100.1.1/32", "9.1.0.4", "down");
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRouteRemoved("Vnet19", "100.100.1.1/32");
        checkRouteNotAdvertised("100.100.1.0/24");
        EXPECT_EQ(findRoute("200.100.1.1"), nullptr);
        checkStateDbRouteRemoved("Vnet19", "200.100.1.1/32");
        checkRouteNotAdvertised("200.100.1.0/24");

        // Delete both routes; every monitor session is removed.
        delVnetRoute("Vnet19", "100.100.1.1/32");
        delVnetRoute("Vnet19", "200.100.1.1/32");
        for (const char *p : {"100.100.1.1/32", "200.100.1.1/32"})
            for (const char *m : {"9.1.0.1", "9.1.0.2", "9.1.0.3", "9.1.0.4"})
                checkCustomMonitorDeleted(p, m);
    }

    // Single-endpoint priority route primary/secondary switchover -- the mock
    // equivalent of test_vnet_orch_20. Two endpoints, primary {1}, secondary
    // {2}; each transition programs the route to point directly at exactly one
    // endpoint's tunnel next hop (never a group), preferring the primary while
    // it is up and falling back to the secondary otherwise.
    TEST_F(VNetOrchTest, VnetSingleEndpointPriorityRouteSwitchover)
    {
        setVxlanTunnel("tunnel_20", "9.9.9.9");
        setVnet("Vnet20", "tunnel_20", "10020", "", /*advertise_prefix=*/true,
                "22:33:33:44:44:66");

        setVnetRoutePriority("Vnet20", "100.100.1.1/32", "9.1.0.1,9.1.0.2",
                             "9.1.0.1,9.1.0.2", /*primary=*/"9.1.0.1", "custom",
                             /*adv_prefix=*/"100.100.1.0/24", /*profile=*/"Test_profile");

        // Both up: primary {1} preferred.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.1", "up");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.2", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("Vnet20", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.0/24", "Test_profile");

        // Secondary down: primary still up, route unchanged on {1}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.2", "down");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("Vnet20", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.0/24", "Test_profile");

        // Primary down, secondary up: fall back to {2}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.1", "down");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.2", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.2"});
        checkStateDbRoute("Vnet20", "100.100.1.1/32", "9.1.0.2");
        checkRouteAdvertised("100.100.1.0/24", "Test_profile");

        // Primary back up: switch back to {1}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.1", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("Vnet20", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.0/24", "Test_profile");

        // Both down: route withdrawn.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.1", "down");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.2", "down");
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRouteRemoved("Vnet20", "100.100.1.1/32");

        // Delete the route; monitor sessions removed.
        delVnetRoute("Vnet20", "100.100.1.1/32");
        checkStateDbRouteRemoved("Vnet20", "100.100.1.1/32");
        checkRouteNotAdvertised("100.100.1.0/24");
        checkCustomMonitorDeleted("100.100.1.1/32", "9.1.0.1");
        checkCustomMonitorDeleted("100.100.1.1/32", "9.1.0.2");
    }

    // Multiple IPv6 priority routes sharing one adv_prefix -- the mock
    // equivalent of test_vnet_orch_21. Three routes all advertise the same
    // summary prefix fd:10:10::/64; the advertisement is reference-counted, so it
    // stays up while any contributing route is active and is withdrawn only once
    // the last one is deleted. Also exercises adding a route after another with
    // the shared prefix is already advertised.
    TEST_F(VNetOrchTest, VnetPriorityRoutesSharedAdvPrefixRefcount)
    {
        setVxlanTunnel("tunnel_21", "fd:10::32");
        setVnet("Vnet21", "tunnel_21", "10021", "", /*advertise_prefix=*/true,
                "22:33:33:44:44:66");

        // Route 1 over fd:10:1::1-4, primary {1,2}; bring its primary up.
        setVnetRoutePriority("Vnet21", "fd:10:10::1/128",
                             "fd:10:1::1,fd:10:1::2,fd:10:1::3,fd:10:1::4",
                             "fd:10:2::1,fd:10:2::2,fd:10:2::3,fd:10:2::4",
                             /*primary=*/"fd:10:1::3,fd:10:1::4", "custom",
                             /*adv_prefix=*/"fd:10:10::/64", /*profile=*/"test_prf");
        // Only the two secondary monitors up -> route on the secondary {1,2}.
        updateMonitorSessionState("fd:10:10::1/128", "fd:10:2::1", "up");
        updateMonitorSessionState("fd:10:10::1/128", "fd:10:2::2", "up");
        checkPriorityRoute("fd:10:10::1", {"fd:10:1::1", "fd:10:1::2"});
        checkStateDbRoute("Vnet21", "fd:10:10::1/128", "fd:10:1::1,fd:10:1::2");
        checkRouteAdvertised("fd:10:10::/64", "test_prf");

        // Route 2 over a different endpoint block, primary {1,2}; all up.
        setVnetRoutePriority("Vnet21", "fd:10:10::21/128",
                             "fd:11:1::1,fd:11:1::2,fd:11:1::3,fd:11:1::4",
                             "fd:11:2::1,fd:11:2::2,fd:11:2::3,fd:11:2::4",
                             /*primary=*/"fd:11:1::1,fd:11:1::2", "custom",
                             /*adv_prefix=*/"fd:10:10::/64", /*profile=*/"test_prf");
        for (const char *m : {"fd:11:2::1", "fd:11:2::2", "fd:11:2::3", "fd:11:2::4"})
            updateMonitorSessionState("fd:10:10::21/128", m, "up");
        checkPriorityRoute("fd:10:10::21", {"fd:11:1::1", "fd:11:1::2"});
        checkStateDbRoute("Vnet21", "fd:10:10::21/128", "fd:11:1::1,fd:11:1::2");
        checkRouteAdvertised("fd:10:10::/64", "test_prf");

        // Delete route 1: the shared advertisement stays (route 2 still active).
        delVnetRoute("Vnet21", "fd:10:10::1/128");
        checkStateDbRouteRemoved("Vnet21", "fd:10:10::1/128");
        checkRouteAdvertised("fd:10:10::/64");

        // Route 3 with the same shared prefix; primary {1,2} up.
        setVnetRoutePriority("Vnet21", "fd:10:10::31/128",
                             "fd:11:1::1,fd:11:1::2,fd:11:1::3,fd:11:1::4",
                             "fd:11:2::1,fd:11:2::2,fd:11:2::3,fd:11:2::4",
                             /*primary=*/"fd:11:1::1,fd:11:1::2", "custom",
                             /*adv_prefix=*/"fd:10:10::/64", /*profile=*/"test_prf");
        updateMonitorSessionState("fd:10:10::31/128", "fd:11:2::1", "up");
        updateMonitorSessionState("fd:10:10::31/128", "fd:11:2::2", "up");
        checkPriorityRoute("fd:10:10::31", {"fd:11:1::1", "fd:11:1::2"});
        checkStateDbRoute("Vnet21", "fd:10:10::31/128", "fd:11:1::1,fd:11:1::2");
        checkRouteAdvertised("fd:10:10::/64", "test_prf");

        // Delete route 2: advertisement still up (route 3 remains).
        delVnetRoute("Vnet21", "fd:10:10::21/128");
        checkStateDbRouteRemoved("Vnet21", "fd:10:10::21/128");
        checkRouteAdvertised("fd:10:10::/64");

        // Delete route 3: last contributor gone, advertisement withdrawn.
        delVnetRoute("Vnet21", "fd:10:10::31/128");
        checkStateDbRouteRemoved("Vnet21", "fd:10:10::31/128");
        checkRouteNotAdvertised("fd:10:10::/64");
    }

    // Priority route re-add + primary/secondary swap by update -- the mock
    // equivalent of test_vnet_orch_22. Re-applying an identical priority route is
    // a no-op; updating only the primary set to endpoints that are currently down
    // leaves the route on its still-active endpoints (the roles swap, the active
    // set does not); a full endpoint-set change moves the route to the new
    // endpoints; and a route with no secondary endpoints is monitored without any
    // SAI BFD session (custom monitoring never creates one).
    TEST_F(VNetOrchTest, VnetPriorityRouteReaddAndPrimarySwap)
    {
        setVxlanTunnel("tunnel_22", "9.9.9.3");
        setVnet("Vnet22", "tunnel_22", "10022", "", /*advertise_prefix=*/true,
                "22:33:33:44:44:66");

        // Single-primary route; bring the primary up.
        setVnetRoutePriority("Vnet22", "100.100.1.11/32", "19.0.0.1,19.0.0.2,19.0.0.3",
                             "19.1.0.1,19.1.0.2,19.1.0.3", /*primary=*/"19.0.0.1",
                             "custom", /*adv_prefix=*/"100.100.1.0/24", "test_prf");
        updateMonitorSessionState("100.100.1.11/32", "19.1.0.1", "up");
        checkPriorityRoute("100.100.1.11", {"19.0.0.1"});
        checkStateDbRoute("Vnet22", "100.100.1.11/32", "19.0.0.1");
        checkRouteAdvertised("100.100.1.0/24", "test_prf");

        // Re-apply the identical route: still on {19.0.0.1}, still advertised.
        setVnetRoutePriority("Vnet22", "100.100.1.11/32", "19.0.0.1,19.0.0.2,19.0.0.3",
                             "19.1.0.1,19.1.0.2,19.1.0.3", /*primary=*/"19.0.0.1",
                             "custom", /*adv_prefix=*/"100.100.1.0/24", "test_prf");
        checkPriorityRoute("100.100.1.11", {"19.0.0.1"});
        checkStateDbRoute("Vnet22", "100.100.1.11/32", "19.0.0.1");
        checkRouteAdvertised("100.100.1.0/24", "test_prf");

        // Delete: advertisement withdrawn.
        delVnetRoute("Vnet22", "100.100.1.11/32");
        checkStateDbRouteRemoved("Vnet22", "100.100.1.11/32");
        checkRouteNotAdvertised("100.100.1.0/24");

        // ECMP route, primary {1,2} up.
        setVnetRoutePriority("Vnet22", "100.100.1.57/32",
                             "5.0.0.1,5.0.0.2,5.0.0.3,5.0.0.4",
                             "5.1.0.1,5.1.0.2,5.1.0.3,5.1.0.4",
                             /*primary=*/"5.0.0.1,5.0.0.2", "custom",
                             /*adv_prefix=*/"100.100.1.0/24", "test_prf");
        updateMonitorSessionState("100.100.1.57/32", "5.1.0.1", "up");
        updateMonitorSessionState("100.100.1.57/32", "5.1.0.2", "up");
        checkPriorityRoute("100.100.1.57", {"5.0.0.1", "5.0.0.2"});
        checkStateDbRoute("Vnet22", "100.100.1.57/32", "5.0.0.1,5.0.0.2");

        // Swap primary to {3,4} (currently down): the route stays on {1,2} (now
        // acting as the active secondary).
        setVnetRoutePriority("Vnet22", "100.100.1.57/32",
                             "5.0.0.1,5.0.0.2,5.0.0.3,5.0.0.4",
                             "5.1.0.1,5.1.0.2,5.1.0.3,5.1.0.4",
                             /*primary=*/"5.0.0.3,5.0.0.4", "custom",
                             /*adv_prefix=*/"100.100.1.0/24", "test_prf");
        checkPriorityRoute("100.100.1.57", {"5.0.0.1", "5.0.0.2"});
        checkStateDbRoute("Vnet22", "100.100.1.57/32", "5.0.0.1,5.0.0.2");
        checkRouteAdvertised("100.100.1.0/24", "test_prf");
        delVnetRoute("Vnet22", "100.100.1.57/32");
        checkStateDbRouteRemoved("Vnet22", "100.100.1.57/32");
        checkRouteNotAdvertised("100.100.1.0/24");

        // Route whose endpoint set is fully replaced by a new block.
        setVnetRoutePriority("Vnet22", "100.100.1.67/32",
                             "5.0.0.1,5.0.0.2,5.0.0.3,5.0.0.4",
                             "5.1.0.1,5.1.0.2,5.1.0.3,5.1.0.4",
                             /*primary=*/"5.0.0.1,5.0.0.2", "custom",
                             /*adv_prefix=*/"100.100.1.0/24", "test_prf");
        updateMonitorSessionState("100.100.1.67/32", "5.1.0.1", "up");
        updateMonitorSessionState("100.100.1.67/32", "5.1.0.2", "up");
        checkPriorityRoute("100.100.1.67", {"5.0.0.1", "5.0.0.2"});
        checkStateDbRoute("Vnet22", "100.100.1.67/32", "5.0.0.1,5.0.0.2");

        // Swap primary to {3,4} (down): stays on the active {1,2}.
        setVnetRoutePriority("Vnet22", "100.100.1.67/32",
                             "5.0.0.1,5.0.0.2,5.0.0.3,5.0.0.4",
                             "5.1.0.1,5.1.0.2,5.1.0.3,5.1.0.4",
                             /*primary=*/"5.0.0.3,5.0.0.4", "custom",
                             /*adv_prefix=*/"100.100.1.0/24", "test_prf");
        checkPriorityRoute("100.100.1.67", {"5.0.0.1", "5.0.0.2"});
        checkStateDbRoute("Vnet22", "100.100.1.67/32", "5.0.0.1,5.0.0.2");
        checkRouteAdvertised("100.100.1.0/24", "test_prf");

        // Replace endpoints with 5.0.0.5-8, primary {5,6} up.
        setVnetRoutePriority("Vnet22", "100.100.1.67/32",
                             "5.0.0.5,5.0.0.6,5.0.0.7,5.0.0.8",
                             "5.1.0.5,5.1.0.6,5.1.0.7,5.1.0.8",
                             /*primary=*/"5.0.0.5,5.0.0.6", "custom",
                             /*adv_prefix=*/"100.100.1.0/24", "test_prf");
        updateMonitorSessionState("100.100.1.67/32", "5.1.0.5", "up");
        updateMonitorSessionState("100.100.1.67/32", "5.1.0.6", "up");
        checkPriorityRoute("100.100.1.67", {"5.0.0.5", "5.0.0.6"});
        checkStateDbRoute("Vnet22", "100.100.1.67/32", "5.0.0.5,5.0.0.6");
        checkRouteAdvertised("100.100.1.0/24", "test_prf");

        // Bring up 7,8 then swap primary to {7,8}: route follows to {7,8}.
        updateMonitorSessionState("100.100.1.67/32", "5.1.0.7", "up");
        updateMonitorSessionState("100.100.1.67/32", "5.1.0.8", "up");
        setVnetRoutePriority("Vnet22", "100.100.1.67/32",
                             "5.0.0.5,5.0.0.6,5.0.0.7,5.0.0.8",
                             "5.1.0.5,5.1.0.6,5.1.0.7,5.1.0.8",
                             /*primary=*/"5.0.0.7,5.0.0.8", "custom",
                             /*adv_prefix=*/"100.100.1.0/24", "test_prf");
        checkPriorityRoute("100.100.1.67", {"5.0.0.7", "5.0.0.8"});
        checkStateDbRoute("Vnet22", "100.100.1.67/32", "5.0.0.7,5.0.0.8");
        checkRouteAdvertised("100.100.1.0/24", "test_prf");
        delVnetRoute("Vnet22", "100.100.1.67/32");
        checkStateDbRouteRemoved("Vnet22", "100.100.1.67/32");
        checkRouteNotAdvertised("100.100.1.0/24");

        // Priority route with no secondary endpoints (primary == all endpoints);
        // custom monitoring must not create any SAI BFD session.
        setVnetRoutePriority("Vnet22", "100.100.1.71/32", "19.0.0.1,19.0.0.2",
                             "19.0.0.1,19.0.0.2", /*primary=*/"19.0.0.1,19.0.0.2",
                             "custom", /*adv_prefix=*/"100.100.1.0/24", "test_prf");
        updateMonitorSessionState("100.100.1.71/32", "19.0.0.1", "up");
        updateMonitorSessionState("100.100.1.71/32", "19.0.0.2", "up");
        EXPECT_FALSE(bfdSessionExists("19.0.0.1"));
        EXPECT_FALSE(bfdSessionExists("19.0.0.2"));
        checkStateDbRoute("Vnet22", "100.100.1.71/32", "19.0.0.1,19.0.0.2");
        checkRouteAdvertised("100.100.1.0/24", "test_prf");

        // One endpoint down: route shrinks to the survivor; both down: withdrawn.
        updateMonitorSessionState("100.100.1.71/32", "19.0.0.1", "down");
        checkStateDbRoute("Vnet22", "100.100.1.71/32", "19.0.0.2");
        checkRouteAdvertised("100.100.1.0/24", "test_prf");
        updateMonitorSessionState("100.100.1.71/32", "19.0.0.2", "down");
        checkStateDbRouteRemoved("Vnet22", "100.100.1.71/32");
        delVnetRoute("Vnet22", "100.100.1.71/32");
        checkStateDbRouteRemoved("Vnet22", "100.100.1.71/32");
    }

    // Changing a VNET's overlay_dmac on the fly -- the mock equivalent of
    // test_vnet_orch_23. A VNET can be created, deleted and re-created
    // repeatedly and its overlay_dmac updated; each custom-monitor APP_DB row a
    // route creates carries the VNET's current overlay_dmac, and updating the
    // dmac while routes exist rewrites every one of their monitor rows. An empty
    // dmac update is ignored (the previous value is retained).
    TEST_F(VNetOrchTest, VnetCustomMonitorOverlayDmacUpdate)
    {
        setVxlanTunnel("tunnel_22", "9.9.9.3");

        // Create/delete/re-create the VNET a few times and update the dmac before
        // any routes exist; the net current dmac is ...:77.
        setVnet("Vnet22", "tunnel_22", "10022", "", /*advertise_prefix=*/true,
                "22:33:33:44:44:66");
        delVnet("Vnet22");
        setVnet("Vnet22", "tunnel_22", "10022", "", true, "22:33:33:44:44:66");
        setVnet("Vnet22", "tunnel_22", "10022", "", true, "22:33:33:44:44:77");
        delVnet("Vnet22");
        setVnet("Vnet22", "tunnel_22", "10022", "", true, "22:33:33:44:44:66");
        setVnet("Vnet22", "tunnel_22", "10022", "", true, "22:33:33:44:44:77");

        // Add a route; its monitor sessions carry the current dmac ...:77.
        setVnetRoutePriority("Vnet22", "100.100.1.11/32", "19.0.0.1,19.0.0.2,19.0.0.3",
                             "19.1.0.1,19.1.0.2,19.1.0.3", /*primary=*/"19.0.0.1",
                             "custom", /*adv_prefix=*/"100.100.1.0/24", "test_prf");
        for (const char *m : {"19.1.0.1", "19.1.0.2", "19.1.0.3"})
            checkCustomMonitorAppDb("100.100.1.11/32", m, "vxlan", "22:33:33:44:44:77");

        // Update the dmac to ...:88 while the route exists: every monitor row is
        // rewritten with the new dmac.
        setVnet("Vnet22", "tunnel_22", "10022", "", true, "22:33:33:44:44:88");
        for (const char *m : {"19.1.0.1", "19.1.0.2", "19.1.0.3"})
            checkCustomMonitorAppDb("100.100.1.11/32", m, "vxlan", "22:33:33:44:44:88");

        // Bringing an endpoint up does not change the monitor rows' dmac.
        updateMonitorSessionState("100.100.1.11/32", "19.1.0.1", "up");
        for (const char *m : {"19.1.0.1", "19.1.0.2", "19.1.0.3"})
            checkCustomMonitorAppDb("100.100.1.11/32", m, "vxlan", "22:33:33:44:44:88");

        // An empty dmac update is a no-op: rows retain ...:88.
        setVnet("Vnet22", "tunnel_22", "10022", "", true, "");
        for (const char *m : {"19.1.0.1", "19.1.0.2", "19.1.0.3"})
            checkCustomMonitorAppDb("100.100.1.11/32", m, "vxlan", "22:33:33:44:44:88");

        // Remove the route: its monitor sessions are deleted.
        delVnetRoute("Vnet22", "100.100.1.11/32");
        checkStateDbRouteRemoved("Vnet22", "100.100.1.11/32");
        for (const char *m : {"19.1.0.1", "19.1.0.2", "19.1.0.3"})
            checkCustomMonitorDeleted("100.100.1.11/32", m);

        // Bring the endpoint down, update the dmac to ...:66 and re-add the route;
        // the fresh monitor rows carry ...:66 and the active route is advertised.
        updateMonitorSessionState("100.100.1.11/32", "19.1.0.1", "down");
        setVnet("Vnet22", "tunnel_22", "10022", "", true, "22:33:33:44:44:66");
        setVnetRoutePriority("Vnet22", "100.100.1.11/32", "19.0.0.1,19.0.0.2,19.0.0.3",
                             "19.1.0.1,19.1.0.2,19.1.0.3", /*primary=*/"19.0.0.1",
                             "custom", /*adv_prefix=*/"100.100.1.0/24", "test_prf");
        updateMonitorSessionState("100.100.1.11/32", "19.1.0.1", "up");
        checkRouteAdvertised("100.100.1.0/24", "test_prf");
        for (const char *m : {"19.1.0.1", "19.1.0.2", "19.1.0.3"})
            checkCustomMonitorAppDb("100.100.1.11/32", m, "vxlan", "22:33:33:44:44:66");

        delVnetRoute("Vnet22", "100.100.1.11/32");
        checkStateDbRouteRemoved("Vnet22", "100.100.1.11/32");
        for (const char *m : {"19.1.0.1", "19.1.0.2", "19.1.0.3"})
            checkCustomMonitorDeleted("100.100.1.11/32", m);
    }

    // VNET tunnel routes carrying a metric field -- the mock equivalent of
    // test_vnet_orch_33. metric is a passthrough field (VNetCfgRouteOrch mirrors
    // it verbatim into APP_DB and orchagent's VNetRouteOrch does not act on it),
    // so this asserts that a full range of metric values does not disturb route
    // programming: each single-endpoint route still programs a tunnel next hop
    // and an active STATE_DB entry, and the metric round-trips into APP_DB.
    TEST_F(VNetOrchTest, VnetTunnelRouteMetricValues)
    {
        setVxlanTunnel("tunnel_33", "10.10.10.10");
        setVnet("Vnet33", "tunnel_33", "10033", "");

        for (int i = 0; i <= 20; i++)
        {
            const string prefix = "0.0.0." + to_string(i) + "/32";
            const string endpoint = "10.10.10." + to_string(i);
            const string metric = to_string(i);

            setVnetRoute("Vnet33", prefix, endpoint, "", "", metric);

            // The route programs a tunnel encap next hop to the endpoint.
            const RouteCaptures::Route *r = findRoute("0.0.0." + to_string(i));
            ASSERT_NE(r, nullptr) << "route " << prefix << " not programmed";
            const RouteCaptures::NextHop *nh = nullptr;
            for (const auto &cand : m_rt.nexthops)
                if (cand.oid == r->next_hop_id) nh = &cand;
            ASSERT_NE(nh, nullptr);
            EXPECT_EQ(nh->type, SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP);
            EXPECT_TRUE(ipAddrEquals(nh->ip, endpoint));
            checkStateDbRoute("Vnet33", prefix, endpoint);

            // The metric field survives into APP_DB (what VNetRouteOrch consumes).
            Table rtTbl(m_app_db.get(), APP_VNET_RT_TUNNEL_TABLE_NAME);
            vector<FieldValueTuple> fvs;
            ASSERT_TRUE(rtTbl.get("Vnet33:" + prefix, fvs));
            string readMetric;
            for (const auto &fv : fvs)
                if (fvField(fv) == "metric") readMetric = fvValue(fv);
            EXPECT_EQ(readMetric, metric);

            delVnetRoute("Vnet33", prefix);
            EXPECT_EQ(findRoute("0.0.0." + to_string(i)), nullptr);
            checkStateDbRouteRemoved("Vnet33", prefix);
        }
    }

    // Mock equivalent of test_vnet_orch_29: a priority (custom-monitored) route
    // whose secondary endpoints are directly-connected/local. The primary subset
    // {9.1.0.1,9.1.0.2} is remote (tunnel-encap next hops); the secondary subset
    // {9.1.0.3,9.1.0.4} is local, resolved through neighbor IP next hops, enabled
    // by check_directly_connected. The failover ladder matches test 18, but when
    // the route falls back to the secondary it must point at local IP next hops
    // rather than tunnel next hops -- a distinction STATE_DB active_endpoints
    // alone cannot show, so we additionally assert each active endpoint's next
    // hop type (checkEndpointIsLocal/Remote). Like the VS test, the secondary
    // states are backbone-checked via STATE_DB (its ASIC helper only matches
    // tunnel next hops), while the primary states use the full priority-route
    // group check.
    TEST_F(VNetOrchTest, VnetPriorityRouteLocalSecondaryFailover)
    {
        setVxlanTunnel("tunnel_29", "9.9.9.9");
        setVnet("Vnet29", "tunnel_29", "10029", "", /*advertise_prefix=*/true,
                /*overlay_dmac=*/"22:33:33:44:44:66");

        // Two directly-connected local endpoints: each is its own /32 interface
        // with a neighbor resolved at the same address (as the VS test wires it).
        createL3Interface("Ethernet8", "9.1.0.3/32");
        createL3Interface("Ethernet12", "9.1.0.4/32");
        addNeighbor("Ethernet8", "9.1.0.3", "00:01:02:03:04:05");
        addNeighbor("Ethernet12", "9.1.0.4", "00:01:02:03:04:06");

        // gNeighOrch created an IP (directly-connected) next hop for each local
        // endpoint the moment the neighbor resolved.
        checkEndpointIsLocal("9.1.0.3");
        checkEndpointIsLocal("9.1.0.4");

        // primary {1,2} remote, secondary {3,4} local, custom monitoring.
        setVnetRoutePriority("Vnet29", "100.100.1.1/32",
                             "9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4",
                             "9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4",
                             /*primary=*/"9.1.0.1,9.1.0.2",
                             /*monitoring=*/"custom",
                             /*adv_prefix=*/"100.100.1.0/24",
                             /*profile=*/"",
                             /*check_directly_connected=*/true);

        // All monitors down: route not programmed, adv_prefix not advertised.
        for (const char *m : {"9.1.0.1", "9.1.0.2", "9.1.0.3", "9.1.0.4"})
            checkCustomMonitorAppDb("100.100.1.1/32", m, "vxlan", "22:33:33:44:44:66");
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRoute("Vnet29", "100.100.1.1/32", "");
        checkRouteNotAdvertised("100.100.1.0/24");

        // All monitors up: only the remote primary subset {1,2} is used.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.1", "up");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.2", "up");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.3", "up");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.4", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1", "9.1.0.2"});
        checkEndpointIsRemote("9.1.0.1");
        checkEndpointIsRemote("9.1.0.2");
        checkStateDbRoute("Vnet29", "100.100.1.1/32", "9.1.0.1,9.1.0.2");
        checkRouteAdvertised("100.100.1.0/24");

        // First primary down: route stays on the remaining primary {1}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.2", "down");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("Vnet29", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.0/24");

        // Both primaries down: fall back to the local secondary subset {3,4}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.1", "down");
        checkStateDbRoute("Vnet29", "100.100.1.1/32", "9.1.0.3,9.1.0.4");
        checkEndpointIsLocal("9.1.0.3");
        checkEndpointIsLocal("9.1.0.4");
        checkRouteAdvertised("100.100.1.0/24");

        // First secondary down: route stays on the remaining secondary {4}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.3", "down");
        checkStateDbRoute("Vnet29", "100.100.1.1/32", "9.1.0.4");
        checkEndpointIsLocal("9.1.0.4");
        checkRouteAdvertised("100.100.1.0/24");

        // Last secondary down: every endpoint down, route withdrawn, adv removed.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.4", "down");
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRouteRemoved("Vnet29", "100.100.1.1/32");
        checkRouteNotAdvertised("100.100.1.0/24");

        // Secondary endpoints back up: route re-added on the local secondary {3,4}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.3", "up");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.4", "up");
        checkStateDbRoute("Vnet29", "100.100.1.1/32", "9.1.0.3,9.1.0.4");
        checkEndpointIsLocal("9.1.0.3");
        checkEndpointIsLocal("9.1.0.4");
        checkRouteAdvertised("100.100.1.0/24");

        // First primary back up: route switches back to the remote primary {1}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.1", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkEndpointIsRemote("9.1.0.1");
        checkStateDbRoute("Vnet29", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.0/24");

        // Second primary up: primary subset {1,2}.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.2", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1", "9.1.0.2"});
        checkStateDbRoute("Vnet29", "100.100.1.1/32", "9.1.0.1,9.1.0.2");
        checkRouteAdvertised("100.100.1.0/24");

        // Secondary going down does not affect a primary-active route.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.3", "down");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.4", "down");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1", "9.1.0.2"});
        checkStateDbRoute("Vnet29", "100.100.1.1/32", "9.1.0.1,9.1.0.2");
        checkRouteAdvertised("100.100.1.0/24");

        // Secondary coming back up does not affect it either.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.3", "up");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.4", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1", "9.1.0.2"});
        checkStateDbRoute("Vnet29", "100.100.1.1/32", "9.1.0.1,9.1.0.2");
        checkRouteAdvertised("100.100.1.0/24");

        // Delete the route: withdrawn, STATE_DB cleared, monitors removed.
        delVnetRoute("Vnet29", "100.100.1.1/32");
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRouteRemoved("Vnet29", "100.100.1.1/32");
        checkRouteNotAdvertised("100.100.1.0/24");
        for (const char *m : {"9.1.0.1", "9.1.0.2", "9.1.0.3", "9.1.0.4"})
            checkCustomMonitorDeleted("100.100.1.1/32", m);
    }

    // Mock equivalent of test_vnet_orch_28: a custom-monitored priority route
    // with a directly-connected/local primary endpoint, exercising the
    // tunnel-termination ACL VNetRouteOrch programs for local endpoints.
    // Endpoints {9.1.0.1 (local), 9.1.0.2 (remote)}, primary 9.1.0.1, monitored
    // by {9.1.0.3->9.1.0.1, 9.1.0.4->9.1.0.2}. Unique coverage vs the other
    // priority tests: (a) no tunnel-term ACL rule exists before the route;
    // (b) creating the route programs a redirect ACL rule (DST_IP=vip,
    // TUNNEL_TERM, redirect to the local endpoint, priority 9998) whose table
    // type carries the REDIRECT + COUNTER actions -- even before any monitor is
    // up, since the ACL follows the local endpoint, not route health; (c) the
    // local endpoint's next hop persists across a primary->secondary->primary
    // flap while the remote endpoint's tunnel next hop is created/removed on
    // demand (active next-hop set 1->2->1). The ACL rule's APP_DB->SAI
    // translation is covered by aclorch_ut; here we assert the VNet->ACL
    // (APP_DB) boundary (the VS test asserts the resulting SAI ACL entry).
    TEST_F(VNetOrchTest, VnetPriorityRouteLocalPrimaryTunnelTermAcl)
    {
        setVxlanTunnel("tunnel_28", "9.9.9.9");
        setVnet("Vnet28", "tunnel_28", "10028", "", /*advertise_prefix=*/true,
                /*overlay_dmac=*/"22:33:33:44:44:66");

        // No tunnel-term ACL rule before any route is created.
        checkNoTunnelTermAclRules();

        // Directly-connected local primary endpoint 9.1.0.1 on Ethernet8.
        createL3Interface("Ethernet8", "9.1.0.1/32");
        addNeighbor("Ethernet8", "9.1.0.1", "00:01:02:03:04:05");
        checkEndpointIsLocal("9.1.0.1");

        // Route: local primary {9.1.0.1}, remote secondary {9.1.0.2}, custom
        // monitored by {9.1.0.3->9.1.0.1, 9.1.0.4->9.1.0.2}.
        setVnetRoutePriority("Vnet28", "100.100.1.1/32", "9.1.0.1,9.1.0.2",
                             "9.1.0.3,9.1.0.4", /*primary=*/"9.1.0.1",
                             /*monitoring=*/"custom",
                             /*adv_prefix=*/"100.100.1.0/24",
                             /*profile=*/"Test_profile",
                             /*check_directly_connected=*/true);

        // The route programs the tunnel-term redirect ACL for the local primary
        // even before any monitor is up: DST_IP=vip, redirect to 9.1.0.1,
        // priority 9998; table type actions = REDIRECT + COUNTER.
        checkTunnelTermAclRule("Vnet28", "100.100.1.1/32", "9.1.0.1");

        // Monitors default down: route not programmed, prefix not advertised.
        for (const char *m : {"9.1.0.3", "9.1.0.4"})
            checkCustomMonitorAppDb("100.100.1.1/32", m, "vxlan", "22:33:33:44:44:66");
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRoute("Vnet28", "100.100.1.1/32", "");
        checkRouteNotAdvertised("100.100.1.0/24");

        // Both monitors up: only the local primary 9.1.0.1 is used; the remote
        // secondary's tunnel next hop is not created (active next-hop set {1}).
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.3", "up");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.4", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkEndpointIsLocal("9.1.0.1");
        EXPECT_EQ(activeEndpointNexthops({"9.1.0.1", "9.1.0.2"}),
                  (set<string>{"9.1.0.1"}));
        checkStateDbRoute("Vnet28", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.0/24", "Test_profile");

        // Secondary monitor down does not affect the primary-active route.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.4", "down");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("Vnet28", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.0/24", "Test_profile");

        // Primary down, secondary up: fail over to the remote secondary 9.1.0.2.
        // Its tunnel next hop is now created; the local next hop persists
        // (active next-hop set {1,2}).
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.3", "down");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.4", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.2"});
        checkEndpointIsRemote("9.1.0.2");
        EXPECT_EQ(activeEndpointNexthops({"9.1.0.1", "9.1.0.2"}),
                  (set<string>{"9.1.0.1", "9.1.0.2"}));
        checkStateDbRoute("Vnet28", "100.100.1.1/32", "9.1.0.2");
        checkRouteAdvertised("100.100.1.0/24", "Test_profile");

        // Primary back up: switch back to the local primary; the remote tunnel
        // next hop is removed while the local one persists (active set {1}).
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.3", "up");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkEndpointIsLocal("9.1.0.1");
        EXPECT_EQ(activeEndpointNexthops({"9.1.0.1", "9.1.0.2"}),
                  (set<string>{"9.1.0.1"}));
        checkStateDbRoute("Vnet28", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.0/24", "Test_profile");

        // All monitors down: route withdrawn, advertisement removed.
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.3", "down");
        updateMonitorSessionState("100.100.1.1/32", "9.1.0.4", "down");
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRouteRemoved("Vnet28", "100.100.1.1/32");
        checkRouteNotAdvertised("100.100.1.0/24");

        // Delete the route: withdrawn, STATE_DB cleared, monitors removed.
        delVnetRoute("Vnet28", "100.100.1.1/32");
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRouteRemoved("Vnet28", "100.100.1.1/32");
        checkRouteNotAdvertised("100.100.1.0/24");
        for (const char *m : {"9.1.0.3", "9.1.0.4"})
            checkCustomMonitorDeleted("100.100.1.1/32", m);
    }

    // test_vnet_local_route_single: a VNET local route with a single directly-
    // connected nexthop. The route is programmed inside the VNET's VR and points
    // at the neighbor's IP next hop; deleting it removes the route.
    TEST_F(VNetOrchTest, VnetLocalRouteSingle)
    {
        setVxlanTunnel("tunnel_30", "30.30.30.30");
        setVnet("Vnet5000", "tunnel_30", "5000", "");

        // VNET-bound RIF + directly-connected neighbor.
        createVnetL3Interface("Ethernet20", "Vnet5000", "10.10.0.8/31");
        addNeighbor("Ethernet20", "10.10.0.9", "00:01:02:03:04:05");

        // Local route via the neighbor: route points directly at its IP next hop.
        setVnetLocalRoute("Vnet5000", "10.10.0.0/24", "Ethernet20", "10.10.0.9");
        checkVnetLocalRoute("10.10.0.0/24", {"10.10.0.9"});

        // Delete removes the route.
        delVnetLocalRoute("Vnet5000", "10.10.0.0/24");
        checkVnetLocalRouteRemoved("10.10.0.0/24");
    }

    // test_vnet_local_route_ecmp: a VNET local route with multiple nexthops
    // programs an unordered ECMP group; updating to a single nexthop collapses to
    // a direct next hop, and back to ECMP re-expands the group.
    TEST_F(VNetOrchTest, VnetLocalRouteEcmp)
    {
        setVxlanTunnel("tunnel_31", "31.31.31.31");
        setVnet("Vnet5001", "tunnel_31", "5001", "");

        // Two VNET-bound RIFs, each with a directly-connected neighbor.
        createVnetL3Interface("Ethernet20", "Vnet5001", "10.10.0.8/31");
        createVnetL3Interface("Ethernet16", "Vnet5001", "10.10.0.10/31");
        addNeighbor("Ethernet20", "10.10.0.9", "00:01:02:03:04:05");
        addNeighbor("Ethernet16", "10.10.0.11", "00:01:02:03:04:06");

        // ECMP local route over both nexthops.
        setVnetLocalRoute("Vnet5001", "10.10.0.0/24", "Ethernet20,Ethernet16",
                          "10.10.0.9,10.10.0.11");
        checkVnetLocalRoute("10.10.0.0/24", {"10.10.0.9", "10.10.0.11"});

        // Update to a single nexthop: route points directly at that IP next hop.
        setVnetLocalRoute("Vnet5001", "10.10.0.0/24", "Ethernet20", "10.10.0.9");
        checkVnetLocalRoute("10.10.0.0/24", {"10.10.0.9"});

        // Update back to ECMP.
        setVnetLocalRoute("Vnet5001", "10.10.0.0/24", "Ethernet20,Ethernet16",
                          "10.10.0.9,10.10.0.11");
        checkVnetLocalRoute("10.10.0.0/24", {"10.10.0.9", "10.10.0.11"});

        // Delete removes the route.
        delVnetLocalRoute("Vnet5001", "10.10.0.0/24");
        checkVnetLocalRouteRemoved("10.10.0.0/24");
    }

    // Migrates test_vnet_local_route_ecmp_unnumbered: a VNET local ECMP route
    // whose members share ONE (link-local) next-hop IP reachable via two
    // interfaces -- the "unnumbered" case learned from FRR/BGP. master's
    // VNetRouteOrch parses `nexthop` as an ordered ip/ifname *list* (duplicate
    // IPs preserved) rather than a deduped IpAddresses set, so 169.254.0.1 listed
    // twice with different ifnames yields a 2-member NHG on distinct RIFs instead
    // of collapsing to one. Because the IP is identical, membership is asserted by
    // RIF, exactly as the VS test does via _nhgm_rifs()/_nh_rif().
    TEST_F(VNetOrchTest, VnetLocalRouteUnnumberedEcmp)
    {
        setVxlanTunnel("tunnel_5099", "99.99.99.99");
        setVnet("Vnet5099", "tunnel_5099", "5099", "");

        // Two VNET-bound RIFs; the same next-hop IP is reachable via both.
        createVnetL3Interface("Ethernet20", "Vnet5099", "10.77.0.4/30");
        createVnetL3Interface("Ethernet16", "Vnet5099", "10.77.0.8/30");
        const sai_object_id_t rif_a = rifOf("Ethernet20");
        const sai_object_id_t rif_b = rifOf("Ethernet16");
        ASSERT_NE(rif_a, SAI_NULL_OBJECT_ID);
        ASSERT_NE(rif_b, SAI_NULL_OBJECT_ID);
        ASSERT_NE(rif_a, rif_b);
        addNeighbor("Ethernet20", "169.254.0.1", "00:01:02:03:04:05");
        addNeighbor("Ethernet16", "169.254.0.1", "00:01:02:03:04:06");

        // Phase A: duplicate IP + 2 ifnames -> 2-member NHG on distinct RIFs.
        setVnetLocalRoute("Vnet5099", "10.99.0.0/24", "Ethernet20,Ethernet16",
                          "169.254.0.1,169.254.0.1");
        checkVnetLocalRouteRifs("10.99.0.0/24", {rif_a, rif_b});

        // Phase B: shrink to 1 ifname -> a single IP next hop on rif_a (no group).
        setVnetLocalRoute("Vnet5099", "10.99.0.0/24", "Ethernet20", "169.254.0.1");
        checkVnetLocalRouteRifs("10.99.0.0/24", {rif_a});

        // Phase C: grow back to 2 -> a new 2-member NHG on both RIFs.
        setVnetLocalRoute("Vnet5099", "10.99.0.0/24", "Ethernet20,Ethernet16",
                          "169.254.0.1,169.254.0.1");
        checkVnetLocalRouteRifs("10.99.0.0/24", {rif_a, rif_b});

        // Phase D: Ethernet16 goes oper-down -> its next hop is IFDOWN-flagged and
        // pruned (NeighOrch::ifChangeInformNextHop ->
        // RouteOrch::invalidnexthopinNextHopGroup); the group survives on rif_a.
        setPortOperStatus("Ethernet16", SAI_PORT_OPER_STATUS_DOWN);
        checkVnetLocalRouteRifs("10.99.0.0/24", {rif_a});

        // Phase E: Ethernet16 back up + neighbor re-learned -> the member reforms.
        setPortOperStatus("Ethernet16", SAI_PORT_OPER_STATUS_UP);
        addNeighbor("Ethernet16", "169.254.0.1", "00:01:02:03:04:06");
        checkVnetLocalRouteRifs("10.99.0.0/24", {rif_a, rif_b});

        // Cleanup.
        delVnetLocalRoute("Vnet5099", "10.99.0.0/24");
        checkVnetLocalRouteRemoved("10.99.0.0/24");
        delVnet("Vnet5099");
        delVxlanTunnel("tunnel_5099");
    }

    // Mock equivalent of test_vnet_orch_local_endpoint_alias_resolution: a
    // custom-monitored, check_directly_connected route whose endpoints become
    // directly-connected local endpoints. The routes are added BEFORE the
    // neighbors exist, so the cached NextHopKey must resolve the local interface
    // when the monitor later goes up -- the endpoint then resolves to the
    // neighbor's SAI_NEXT_HOP_TYPE_IP next hop (gNeighOrch), not a tunnel-encap
    // next hop. A single active local endpoint points the route directly at that
    // IP next hop; two active local endpoints form an (unordered) ECMP group of
    // IP next hops.
    TEST_F(VNetOrchTest, VnetLocalEndpointAliasResolution)
    {
        const string endpoint = "20.20.20.5";
        const string backup_endpoint = "20.20.20.6";
        const string ecmp_endpoint = "20.20.20.7";
        const string route_prefix = "103.100.1.1/32";
        const string ecmp_route_prefix = "103.100.1.2/32";

        // Local router interface Ethernet0 owns the 20.20.20.0/24 subnet, so the
        // endpoints below are directly connected once their neighbors resolve.
        createL3Interface("Ethernet0", "20.20.20.1/24");

        setVxlanTunnel("tunnel_local_ep", "9.9.9.9");
        setVnet("Vnet_local_ep", "tunnel_local_ep", "1001", "");

        // Add the routes before the neighbors exist: the endpoint's NextHopKey is
        // cached unresolved and must be resolved against the local interface when
        // the monitor later goes up.
        setVnetRoutePriority("Vnet_local_ep", route_prefix,
                             endpoint + "," + backup_endpoint,
                             endpoint + "," + backup_endpoint,
                             /*primary=*/endpoint, /*monitoring=*/"custom",
                             /*adv_prefix=*/"", /*profile=*/"",
                             /*check_directly_connected=*/true);
        setVnetRoutePriority("Vnet_local_ep", ecmp_route_prefix,
                             endpoint + "," + ecmp_endpoint,
                             endpoint + "," + ecmp_endpoint,
                             /*primary=*/endpoint + "," + ecmp_endpoint,
                             /*monitoring=*/"custom", /*adv_prefix=*/"",
                             /*profile=*/"", /*check_directly_connected=*/true);

        // Resolve the neighbors -> gNeighOrch creates an IP (directly-connected)
        // next hop for each local endpoint.
        addNeighbor("Ethernet0", endpoint, "00:01:02:03:04:05");
        addNeighbor("Ethernet0", ecmp_endpoint, "00:01:02:03:04:07");
        checkEndpointIsLocal(endpoint);
        checkEndpointIsLocal(ecmp_endpoint);

        // Bring the monitored endpoints up.
        updateMonitorSessionState(route_prefix, endpoint, "up");
        updateMonitorSessionState(ecmp_route_prefix, endpoint, "up");
        updateMonitorSessionState(ecmp_route_prefix, ecmp_endpoint, "up");

        // The single-endpoint route points directly at the local endpoint's IP
        // next hop; the ECMP route forms a group of both local IP next hops.
        checkVnetLocalRoute(route_prefix, {endpoint});
        checkVnetLocalRoute(ecmp_route_prefix, {endpoint, ecmp_endpoint});

        // Deleting the routes withdraws them and tears down the monitor sessions.
        delVnetRoute("Vnet_local_ep", ecmp_route_prefix);
        delVnetRoute("Vnet_local_ep", route_prefix);
        checkVnetLocalRouteRemoved(route_prefix);
        checkVnetLocalRouteRemoved(ecmp_route_prefix);

        delVnet("Vnet_local_ep");
        delVxlanTunnel("tunnel_local_ep");
    }

    // Mock equivalent of test_vnet_orch_26: a BFD-monitored ECMP VNET route on
    // a subnet-decap-enabled switch. The MP2MP IPINIP decap term is created when
    // the route first becomes active (first monitor up) and removed when it goes
    // inactive (all monitors down); the ECMP route + prefix advertisement follow
    // the usual BFD-state machine.
    TEST_F(VNetOrchTest, VnetSubnetDecapTermFollowsBfdState)
    {
        // Enable subnet decap and create the IPINIP subnet-decap tunnel.
        setSubnetDecapConfig("enable", "10.10.10.0/24", "20c1:ba8::/64");
        createSubnetDecapTunnel("IPINIP_SUBNET", "uniform", "standard", "pipe");
        const sai_object_id_t ipinip = checkIpinipTunnel(
            SAI_TUNNEL_DSCP_MODE_UNIFORM_MODEL,
            SAI_TUNNEL_DECAP_ECN_MODE_STANDARD,
            SAI_TUNNEL_TTL_MODE_PIPE_MODEL);
        ASSERT_NE(ipinip, SAI_NULL_OBJECT_ID);

        setVxlanTunnel("tunnel_26", "26.26.26.26");
        setVnet("Vnet26", "tunnel_26", "10026", "", /*advertise_prefix=*/true);

        setVnetRouteMonitored("Vnet26", "100.100.1.1/32",
                              "26.0.0.1,26.0.0.2,26.0.0.3",
                              "26.1.0.1,26.1.0.2,26.1.0.3", "test_profile");

        // Default BFD state is down: no decap term, the route is not programmed
        // and is neither in STATE_DB nor advertised.
        checkNoIpinipDecapTerm("100.100.1.1/32", "10.10.10.0/24");
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRoute("Vnet26", "100.100.1.1/32", "");
        checkRouteNotAdvertised("100.100.1.1/32");

        // First monitor up makes the route active -> the decap term is created.
        updateBfdSessionState("26.1.0.1", SAI_BFD_SESSION_STATE_UP);
        checkIpinipDecapTerm("100.100.1.1/32", "10.10.10.0/24", ipinip);

        // All monitors up: the ECMP route is programmed over a 3-member group,
        // STATE_DB lists all three, and the prefix is advertised with its
        // profile. The decap term stays.
        updateBfdSessionState("26.1.0.2", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("26.1.0.3", SAI_BFD_SESSION_STATE_UP);
        const RouteCaptures::Route *r = findRoute("100.100.1.1");
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(activeMembers(r->next_hop_id), 3U);
        checkStateDbRoute("Vnet26", "100.100.1.1/32", "26.0.0.1,26.0.0.2,26.0.0.3");
        checkRouteAdvertised("100.100.1.1/32", "test_profile");
        checkIpinipDecapTerm("100.100.1.1/32", "10.10.10.0/24", ipinip);

        // All monitors down: the route is withdrawn, STATE_DB inactive, prefix
        // unadvertised, and the decap term is removed.
        updateBfdSessionState("26.1.0.1", SAI_BFD_SESSION_STATE_DOWN);
        updateBfdSessionState("26.1.0.2", SAI_BFD_SESSION_STATE_DOWN);
        updateBfdSessionState("26.1.0.3", SAI_BFD_SESSION_STATE_DOWN);
        checkIpinipDecapTermRemoved("100.100.1.1/32", "10.10.10.0/24");
        checkStateDbRoute("Vnet26", "100.100.1.1/32", "");
        checkRouteNotAdvertised("100.100.1.1/32");

        // Delete the route: its STATE_DB entry and BFD sessions are removed.
        delVnetRouteMonitored("Vnet26", "100.100.1.1/32");
        checkStateDbRouteRemoved("Vnet26", "100.100.1.1/32");
        for (const char *mon : {"26.1.0.1", "26.1.0.2", "26.1.0.3"})
            EXPECT_FALSE(bfdSessionExists(mon)) << "BFD session " << mon << " not removed";

        delVnet("Vnet26");
        delVxlanTunnel("tunnel_26");

        // Removing the subnet-decap tunnel tears down the IPINIP SAI tunnel.
        delSubnetDecapTunnel("IPINIP_SUBNET");
        EXPECT_NE(find(m_tun.removedTunnels.begin(), m_tun.removedTunnels.end(), ipinip),
                  m_tun.removedTunnels.end())
            << "IPINIP tunnel not removed";
    }

    // Mock equivalent of test_vnet_orch_27: the IPv6 counterpart of test 26 --
    // a BFD-monitored IPv6 ECMP VNET route whose MP2MP IPINIP decap term (source
    // subnet from src_ip_v6) follows the route's active/inactive state.
    TEST_F(VNetOrchTest, VnetSubnetDecapTermFollowsBfdStateIpv6)
    {
        setSubnetDecapConfig("enable", "10.10.10.0/24", "20c1:ba8::/64");
        createSubnetDecapTunnel("IPINIP_SUBNET_V6", "uniform", "standard", "pipe");
        const sai_object_id_t ipinip = checkIpinipTunnel(
            SAI_TUNNEL_DSCP_MODE_UNIFORM_MODEL,
            SAI_TUNNEL_DECAP_ECN_MODE_STANDARD,
            SAI_TUNNEL_TTL_MODE_PIPE_MODEL);
        ASSERT_NE(ipinip, SAI_NULL_OBJECT_ID);

        // The VS test reuses the name 'Vnet26' for this IPv6 case.
        setVxlanTunnel("tunnel_27", "fd:10::32");
        setVnet("Vnet26", "tunnel_27", "10010", "", /*advertise_prefix=*/true);

        setVnetRouteMonitored("Vnet26", "fd:10:10::1/128",
                              "fd:10:1::1,fd:10:1::2,fd:10:1::3",
                              "fd:10:2::1,fd:10:2::2,fd:10:2::3", "test_profile");

        // Default BFD state is down: no decap term, route not programmed.
        checkNoIpinipDecapTerm("fd:10:10::1/128", "20c1:ba8::/64");
        EXPECT_EQ(findRoute("fd:10:10::1"), nullptr);
        checkStateDbRoute("Vnet26", "fd:10:10::1/128", "");
        checkRouteNotAdvertised("fd:10:10::1/128");

        // First monitor up -> the IPv6 decap term is created (src from src_ip_v6).
        updateBfdSessionState("fd:10:2::2", SAI_BFD_SESSION_STATE_UP);
        checkIpinipDecapTerm("fd:10:10::1/128", "20c1:ba8::/64", ipinip);

        // All up: ECMP route programmed, advertised with profile, term stays.
        updateBfdSessionState("fd:10:2::3", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("fd:10:2::1", SAI_BFD_SESSION_STATE_UP);
        const RouteCaptures::Route *r = findRoute("fd:10:10::1");
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(activeMembers(r->next_hop_id), 3U);
        checkStateDbRoute("Vnet26", "fd:10:10::1/128",
                          "fd:10:1::1,fd:10:1::2,fd:10:1::3");
        checkRouteAdvertised("fd:10:10::1/128", "test_profile");
        checkIpinipDecapTerm("fd:10:10::1/128", "20c1:ba8::/64", ipinip);

        // All down: route withdrawn, term removed.
        updateBfdSessionState("fd:10:2::1", SAI_BFD_SESSION_STATE_DOWN);
        updateBfdSessionState("fd:10:2::2", SAI_BFD_SESSION_STATE_DOWN);
        updateBfdSessionState("fd:10:2::3", SAI_BFD_SESSION_STATE_DOWN);
        checkIpinipDecapTermRemoved("fd:10:10::1/128", "20c1:ba8::/64");
        checkStateDbRoute("Vnet26", "fd:10:10::1/128", "");
        checkRouteNotAdvertised("fd:10:10::1/128");

        delVnetRouteMonitored("Vnet26", "fd:10:10::1/128");
        checkStateDbRouteRemoved("Vnet26", "fd:10:10::1/128");
        for (const char *mon : {"fd:10:2::1", "fd:10:2::2", "fd:10:2::3"})
            EXPECT_FALSE(bfdSessionExists(mon)) << "BFD session " << mon << " not removed";

        delVnet("Vnet26");
        delVxlanTunnel("tunnel_27");

        delSubnetDecapTunnel("IPINIP_SUBNET_V6");
        EXPECT_NE(find(m_tun.removedTunnels.begin(), m_tun.removedTunnels.end(), ipinip),
                  m_tun.removedTunnels.end())
            << "IPINIP tunnel not removed";
    }

    // Mock equivalent of test_vnet_orch_32: a custom_bfd priority route with two
    // primary (remote) and two secondary (directly-connected local) endpoints.
    // Exercises the full failover cascade: primaries preferred whenever any is
    // up, fall back to the secondary group only when both primaries are down,
    // withdrawn entirely when all four are down, then restored primary-first as
    // monitors return -- and secondary monitor churn never disturbs an
    // active-primary route. The adv_prefix is a /24 summary that is advertised
    // whenever the route is active.
    TEST_F(VNetOrchTest, VnetCustomBfdPriorityRouteTwoPrimaryTwoSecondary)
    {
        setVxlanTunnel("tunnel_32", "9.9.9.9");
        setVnet("vnet32", "tunnel_32", "10029", "", /*advertise_prefix=*/true,
                /*overlay_dmac=*/"22:33:33:44:44:66");

        // 9.1.0.3 / 9.1.0.4 are directly-connected local secondary endpoints.
        createL3Interface("Ethernet8", "9.1.0.3/32");
        createL3Interface("Ethernet12", "9.1.0.4/32");
        addNeighbor("Ethernet8", "9.1.0.3", "00:01:02:03:04:05");
        addNeighbor("Ethernet12", "9.1.0.4", "00:01:02:03:04:06");

        // Primaries 9.1.0.1,9.1.0.2 (remote); secondaries 9.1.0.3,9.1.0.4
        // (local). adv_prefix is the /24 summary of the /32 route.
        setVnetRoutePriority("vnet32", "100.100.1.1/32",
                             "9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4",
                             "9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4",
                             /*primary=*/"9.1.0.1,9.1.0.2", "custom_bfd",
                             /*adv_prefix=*/"100.100.1.0/24", /*profile=*/"",
                             /*check_directly_connected=*/true, 100, 100);

        // Default monitor state is down -> route not programmed, STATE_DB
        // inactive, /24 summary not advertised.
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRoute("vnet32", "100.100.1.1/32", "");
        checkRouteNotAdvertised("100.100.1.0/24");

        // All four up -> only the two primaries are in use (ECMP of 2).
        updateBfdSessionState("9.1.0.1", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("9.1.0.2", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("9.1.0.3", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("9.1.0.4", SAI_BFD_SESSION_STATE_UP);
        checkPriorityRoute("100.100.1.1", {"9.1.0.1", "9.1.0.2"});
        checkStateDbRoute("vnet32", "100.100.1.1/32", "9.1.0.1,9.1.0.2");
        checkRouteAdvertised("100.100.1.0/24");

        // Drop one primary -> route stays on the primary group with just 9.1.0.1.
        updateBfdSessionState("9.1.0.2", SAI_BFD_SESSION_STATE_DOWN);
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("vnet32", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.0/24");

        // Both primaries down -> fail over to the secondary (local) group.
        updateBfdSessionState("9.1.0.1", SAI_BFD_SESSION_STATE_DOWN);
        checkStateDbRoute("vnet32", "100.100.1.1/32", "9.1.0.3,9.1.0.4");
        checkRouteAdvertised("100.100.1.0/24");

        // Drop one secondary -> route stays on the secondary group with 9.1.0.4.
        updateBfdSessionState("9.1.0.3", SAI_BFD_SESSION_STATE_DOWN);
        checkStateDbRoute("vnet32", "100.100.1.1/32", "9.1.0.4");
        checkRouteAdvertised("100.100.1.0/24");

        // Last endpoint down -> route withdrawn entirely, summary unadvertised.
        updateBfdSessionState("9.1.0.4", SAI_BFD_SESSION_STATE_DOWN);
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRouteRemoved("vnet32", "100.100.1.1/32");
        checkRouteNotAdvertised("100.100.1.0/24");

        // Secondaries return -> route restored over the secondary group.
        updateBfdSessionState("9.1.0.3", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("9.1.0.4", SAI_BFD_SESSION_STATE_UP);
        checkStateDbRoute("vnet32", "100.100.1.1/32", "9.1.0.3,9.1.0.4");
        checkRouteAdvertised("100.100.1.0/24");

        // A primary returns -> route switches back to the primary group.
        updateBfdSessionState("9.1.0.1", SAI_BFD_SESSION_STATE_UP);
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("vnet32", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.0/24");

        // Second primary returns -> both primaries in the group again.
        updateBfdSessionState("9.1.0.2", SAI_BFD_SESSION_STATE_UP);
        checkPriorityRoute("100.100.1.1", {"9.1.0.1", "9.1.0.2"});
        checkStateDbRoute("vnet32", "100.100.1.1/32", "9.1.0.1,9.1.0.2");
        checkRouteAdvertised("100.100.1.0/24");

        // Secondary churn must not disturb an active-primary route.
        updateBfdSessionState("9.1.0.3", SAI_BFD_SESSION_STATE_DOWN);
        updateBfdSessionState("9.1.0.4", SAI_BFD_SESSION_STATE_DOWN);
        checkPriorityRoute("100.100.1.1", {"9.1.0.1", "9.1.0.2"});
        checkStateDbRoute("vnet32", "100.100.1.1/32", "9.1.0.1,9.1.0.2");
        checkRouteAdvertised("100.100.1.0/24");

        updateBfdSessionState("9.1.0.3", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("9.1.0.4", SAI_BFD_SESSION_STATE_UP);
        checkPriorityRoute("100.100.1.1", {"9.1.0.1", "9.1.0.2"});
        checkStateDbRoute("vnet32", "100.100.1.1/32", "9.1.0.1,9.1.0.2");
        checkRouteAdvertised("100.100.1.0/24");

        // Delete: route withdrawn, summary unadvertised, all sessions removed.
        delVnetRouteMonitored("vnet32", "100.100.1.1/32");
        checkStateDbRouteRemoved("vnet32", "100.100.1.1/32");
        checkRouteNotAdvertised("100.100.1.0/24");
        EXPECT_FALSE(bfdSessionExists("9.1.0.1"));
        EXPECT_FALSE(bfdSessionExists("9.1.0.2"));
        EXPECT_FALSE(bfdSessionExists("9.1.0.3"));
        EXPECT_FALSE(bfdSessionExists("9.1.0.4"));

        delVnet("vnet32");
        delVxlanTunnel("tunnel_32");
    }

    // Mock equivalent of test_vnet_orch_34: a custom_bfd priority route whose
    // primary is a directly-connected local endpoint. The route is re-paired on
    // the fly (endpoint set + primary changed): the BFD session for the dropped
    // remote endpoint is torn down, the route falls back to the still-up
    // secondary while the new primary's monitor is down, and switches to the new
    // primary once its BFD comes up. Advertisement follows the active state.
    TEST_F(VNetOrchTest, VnetCustomBfdPriorityRouteRepairing)
    {
        setVxlanTunnel("tunnel_34", "9.9.9.9");
        setVnet("vnet34", "tunnel_34", "10029", "", /*advertise_prefix=*/true,
                /*overlay_dmac=*/"22:33:33:44:44:66");

        // 9.1.0.1 is a directly-connected local endpoint on Ethernet4 (/32).
        createL3Interface("Ethernet4", "9.1.0.1/32");
        addNeighbor("Ethernet4", "9.1.0.1", "00:01:02:03:04:05");

        // Priority route: primary 9.1.0.1 (local), secondary 9.1.0.2 (remote),
        // custom_bfd monitoring with per-endpoint monitor timers.
        setVnetRoutePriority("vnet34", "100.100.1.1/32", "9.1.0.1,9.1.0.2",
                             "9.1.0.1,9.1.0.2", /*primary=*/"9.1.0.1",
                             /*monitoring=*/"custom_bfd",
                             /*adv_prefix=*/"100.100.1.1/32", /*profile=*/"",
                             /*check_directly_connected=*/true,
                             /*rx_monitor_timer=*/100, /*tx_monitor_timer=*/100);

        // Default monitor state is down -> route not programmed (ASIC), present
        // in STATE_DB but inactive, not advertised.
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRoute("vnet34", "100.100.1.1/32", "");
        checkRouteNotAdvertised("100.100.1.1/32");

        // Both monitors up -> only the primary 9.1.0.1 is in use.
        updateBfdSessionState("9.1.0.1", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("9.1.0.2", SAI_BFD_SESSION_STATE_UP);
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("vnet34", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.1/32");

        // Re-pair: swap the secondary 9.1.0.2 -> 9.1.0.3 and make it primary.
        setVnetRoutePriority("vnet34", "100.100.1.1/32", "9.1.0.1,9.1.0.3",
                             "9.1.0.1,9.1.0.3", /*primary=*/"9.1.0.3",
                             "custom_bfd", "100.100.1.1/32", "", true, 100, 100);

        // The dropped remote endpoint's BFD session is gone; the new primary
        // 9.1.0.3 is still down, so the route falls back to the up secondary.
        EXPECT_FALSE(bfdSessionExists("9.1.0.2"));
        checkStateDbRoute("vnet34", "100.100.1.1/32", "9.1.0.1");

        // New primary up -> route switches to 9.1.0.3.
        updateBfdSessionState("9.1.0.3", SAI_BFD_SESSION_STATE_UP);
        checkPriorityRoute("100.100.1.1", {"9.1.0.3"});
        checkStateDbRoute("vnet34", "100.100.1.1/32", "9.1.0.3");
        checkRouteAdvertised("100.100.1.1/32");

        // Delete: route withdrawn, advertisement removed, all sessions gone.
        delVnetRouteMonitored("vnet34", "100.100.1.1/32");
        checkStateDbRouteRemoved("vnet34", "100.100.1.1/32");
        checkRouteNotAdvertised("100.100.1.1/32");
        EXPECT_FALSE(bfdSessionExists("9.1.0.1"));
        EXPECT_FALSE(bfdSessionExists("9.1.0.3"));

        delVnet("vnet34");
        delVxlanTunnel("tunnel_34");
    }

    // Mock equivalent of test_vnet_orch_35 (DPU repair under the same NPU): the
    // endpoint set is unchanged, but the *monitor* IP paired with the remote
    // endpoint is re-pointed several times (10.1.0.1 -> 10.1.0.2 -> 10.1.0.3 ->
    // 10.1.0.1). Each re-pair tears down the old monitor's BFD session and
    // creates one for the new monitor; the active endpoint follows the primary.
    TEST_F(VNetOrchTest, VnetCustomBfdMonitorRepairSameNpu)
    {
        setVxlanTunnel("tunnel_35", "9.9.9.9");
        setVnet("vnet35", "tunnel_35", "10029", "", /*advertise_prefix=*/true,
                /*overlay_dmac=*/"22:33:33:44:44:66");

        createL3Interface("Ethernet4", "9.1.0.1/32");
        addNeighbor("Ethernet4", "9.1.0.1", "00:01:02:03:04:05");

        // endpoints 9.1.0.1 (local) + 9.1.0.2 (remote); the remote endpoint is
        // monitored through a separate DPU monitor IP 10.1.0.1.
        setVnetRoutePriority("vnet35", "100.100.1.1/32", "9.1.0.1,9.1.0.2",
                             "9.1.0.1,10.1.0.1", /*primary=*/"9.1.0.1",
                             "custom_bfd", "100.100.1.1/32", "", true, 100, 100);
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRoute("vnet35", "100.100.1.1/32", "");
        checkRouteNotAdvertised("100.100.1.1/32");

        updateBfdSessionState("9.1.0.1", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("10.1.0.1", SAI_BFD_SESSION_STATE_UP);
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("vnet35", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.1/32");

        // Re-point the remote monitor 10.1.0.1 -> 10.1.0.2 (endpoints unchanged).
        setVnetRoutePriority("vnet35", "100.100.1.1/32", "9.1.0.1,9.1.0.2",
                             "9.1.0.1,10.1.0.2", /*primary=*/"9.1.0.1",
                             "custom_bfd", "100.100.1.1/32", "", true, 100, 100);
        EXPECT_FALSE(bfdSessionExists("10.1.0.1"));
        checkStateDbRoute("vnet35", "100.100.1.1/32", "9.1.0.1");
        updateBfdSessionState("10.1.0.2", SAI_BFD_SESSION_STATE_UP);
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("vnet35", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.1/32");

        // Re-point monitor 10.1.0.2 -> 10.1.0.3 and make the remote endpoint
        // primary: while 10.1.0.3 is down the route stays on the local 9.1.0.1.
        setVnetRoutePriority("vnet35", "100.100.1.1/32", "9.1.0.1,9.1.0.2",
                             "9.1.0.1,10.1.0.3", /*primary=*/"9.1.0.2",
                             "custom_bfd", "100.100.1.1/32", "", true, 100, 100);
        EXPECT_FALSE(bfdSessionExists("10.1.0.2"));
        checkStateDbRoute("vnet35", "100.100.1.1/32", "9.1.0.1");
        updateBfdSessionState("10.1.0.3", SAI_BFD_SESSION_STATE_UP);
        checkPriorityRoute("100.100.1.1", {"9.1.0.2"});
        checkStateDbRoute("vnet35", "100.100.1.1/32", "9.1.0.2");
        checkRouteAdvertised("100.100.1.1/32");

        // Re-pair back to the original monitor + primary.
        setVnetRoutePriority("vnet35", "100.100.1.1/32", "9.1.0.1,9.1.0.2",
                             "9.1.0.1,10.1.0.1", /*primary=*/"9.1.0.1",
                             "custom_bfd", "100.100.1.1/32", "", true, 100, 100);
        EXPECT_FALSE(bfdSessionExists("10.1.0.3"));
        checkStateDbRoute("vnet35", "100.100.1.1/32", "9.1.0.1");
        updateBfdSessionState("10.1.0.1", SAI_BFD_SESSION_STATE_UP);
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("vnet35", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.1/32");

        delVnetRouteMonitored("vnet35", "100.100.1.1/32");
        checkStateDbRouteRemoved("vnet35", "100.100.1.1/32");
        checkRouteNotAdvertised("100.100.1.1/32");
        EXPECT_FALSE(bfdSessionExists("9.1.0.1"));
        EXPECT_FALSE(bfdSessionExists("10.1.0.1"));

        delVnet("vnet35");
        delVxlanTunnel("tunnel_35");
    }

    // Mock equivalent of test_vnet_orch_36 (pinned monitor state): a custom_bfd
    // priority route whose primary monitor's *pinned* state overrides the actual
    // BFD state. Pinning the primary down forces failover to the backup even
    // though its BFD is up; pinning it up keeps the route on the primary even
    // when the primary's BFD goes down.
    TEST_F(VNetOrchTest, VnetCustomBfdPinnedMonitorState)
    {
        setVxlanTunnel("tunnel_36", "9.9.9.9");
        setVnet("vnet36", "tunnel_36", "10029", "", /*advertise_prefix=*/true,
                /*overlay_dmac=*/"22:33:33:44:44:66");

        createL3Interface("Ethernet4", "9.1.0.1/32");
        addNeighbor("Ethernet4", "9.1.0.1", "00:01:02:03:04:05");

        // Primary 9.1.0.1 (local DPU), backup 10.1.0.1 (remote NPU) monitored
        // through DPU 9.1.0.2; no pins initially.
        setVnetRoutePriority("vnet36", "100.100.1.1/32", "9.1.0.1,10.1.0.1",
                             "9.1.0.1,9.1.0.2", /*primary=*/"9.1.0.1",
                             "custom_bfd", "100.100.1.1/32", "", true, 100, 100,
                             /*pinned_state=*/"none,none");
        EXPECT_EQ(findRoute("100.100.1.1"), nullptr);
        checkStateDbRoute("vnet36", "100.100.1.1/32", "");
        checkRouteNotAdvertised("100.100.1.1/32");

        updateBfdSessionState("9.1.0.1", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("9.1.0.2", SAI_BFD_SESSION_STATE_UP);
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("vnet36", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.1/32");

        // Pin the primary monitor down -> route fails over to the backup even
        // though the primary's BFD is still up.
        setVnetRoutePriority("vnet36", "100.100.1.1/32", "9.1.0.1,10.1.0.1",
                             "9.1.0.1,9.1.0.2", /*primary=*/"9.1.0.1",
                             "custom_bfd", "", "", false, -1, -1,
                             /*pinned_state=*/"down,none");
        checkPriorityRoute("100.100.1.1", {"10.1.0.1"});
        checkStateDbRoute("vnet36", "100.100.1.1/32", "10.1.0.1");
        checkRouteAdvertised("100.100.1.1/32");

        // Un-pin -> back to the primary.
        setVnetRoutePriority("vnet36", "100.100.1.1/32", "9.1.0.1,10.1.0.1",
                             "9.1.0.1,9.1.0.2", /*primary=*/"9.1.0.1",
                             "custom_bfd", "", "", false, -1, -1,
                             /*pinned_state=*/"none,none");
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("vnet36", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.1/32");

        // Pin the primary up, then drop its BFD: the route stays on the primary.
        setVnetRoutePriority("vnet36", "100.100.1.1/32", "9.1.0.1,10.1.0.1",
                             "9.1.0.1,9.1.0.2", /*primary=*/"9.1.0.1",
                             "custom_bfd", "", "", false, -1, -1,
                             /*pinned_state=*/"up,none");
        updateBfdSessionState("9.1.0.1", SAI_BFD_SESSION_STATE_DOWN);
        checkPriorityRoute("100.100.1.1", {"9.1.0.1"});
        checkStateDbRoute("vnet36", "100.100.1.1/32", "9.1.0.1");
        checkRouteAdvertised("100.100.1.1/32");

        delVnetRouteMonitored("vnet36", "100.100.1.1/32");
        checkStateDbRouteRemoved("vnet36", "100.100.1.1/32");
        checkRouteNotAdvertised("100.100.1.1/32");
        EXPECT_FALSE(bfdSessionExists("9.1.0.1"));
        EXPECT_FALSE(bfdSessionExists("9.1.0.2"));

        delVnet("vnet36");
        delVxlanTunnel("tunnel_36");
    }

    // BFD-monitored ECMP VNET route through a Traffic-Shift-Away cycle -- the
    // mock equivalent of test_vnet_orch_25. TSA (BGP_DEVICE_GLOBAL|STATE
    // tsa_enabled) drives BfdOrch::handleTsaStateChange, which tears down every
    // BFD session (withdrawing the routes monitored through them) and later
    // recreates them:
    //  * all monitors up -> route programmed over the ECMP group;
    //  * set TSA -> all BFD sessions removed, route withdrawn, STATE_DB
    //    inactive;
    //  * clear TSA -> sessions recreated (initially down) so the route stays
    //    withdrawn; bringing the monitors up again restores the route over the
    //    same (persisted) group;
    //  * deleting the route removes the group and its BFD sessions.
    TEST_F(VNetOrchTest, VnetMonitoredEcmpRouteTsaWithdrawsAndRestores)
    {
        setVxlanTunnel("tunnel_25", "9.9.9.9");
        setVnet("Vnet25", "tunnel_25", "10025", "");

        setVnetRouteMonitored("Vnet25", "125.100.1.1/32",
                              "9.0.0.1,9.0.0.2,9.0.0.3", "9.1.0.1,9.1.0.2,9.1.0.3");

        // Default BFD state is down: the route is not programmed, STATE_DB is
        // inactive, and the default VNET does not advertise the prefix.
        EXPECT_EQ(findRoute("125.100.1.1"), nullptr);
        checkStateDbRoute("Vnet25", "125.100.1.1/32", "");
        checkRouteNotAdvertised("125.100.1.1/32");

        // All monitors up: the route is programmed over the ECMP group with a
        // member per endpoint.
        updateBfdSessionState("9.1.0.1", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("9.1.0.2", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("9.1.0.3", SAI_BFD_SESSION_STATE_UP);
        const RouteCaptures::Route *r = findRoute("125.100.1.1");
        ASSERT_NE(r, nullptr);
        const sai_object_id_t nhg = r->next_hop_id;
        EXPECT_NE(nhg, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(activeMembers(nhg), 3U);
        checkStateDbRoute("Vnet25", "125.100.1.1/32", "9.0.0.1,9.0.0.2,9.0.0.3");
        checkRouteNotAdvertised("125.100.1.1/32");

        // Enable TSA: every BFD session is removed and the route is withdrawn
        // (STATE_DB inactive), but the ECMP group persists (as on all-down).
        setTsa(true);
        for (const char *mon : {"9.1.0.1", "9.1.0.2", "9.1.0.3"})
            EXPECT_FALSE(bfdSessionExists(mon)) << "BFD session " << mon << " not removed by TSA";
        bool routeRemoved = false;
        for (const auto &d : m_rt.removedRoutes)
            if (prefixAddrEquals(d, "125.100.1.1")) routeRemoved = true;
        EXPECT_TRUE(routeRemoved);
        EXPECT_EQ(activeMembers(nhg), 0U);
        checkStateDbRoute("Vnet25", "125.100.1.1/32", "");
        checkRouteNotAdvertised("125.100.1.1/32");

        // Clear TSA: the BFD sessions are recreated but start down, so the route
        // stays withdrawn until the monitors report up.
        setTsa(false);
        for (const char *mon : {"9.1.0.1", "9.1.0.2", "9.1.0.3"})
            EXPECT_TRUE(bfdSessionExists(mon)) << "BFD session " << mon << " not recreated after TSA clear";
        EXPECT_EQ(activeMembers(nhg), 0U);
        checkStateDbRoute("Vnet25", "125.100.1.1/32", "");

        // Monitors up again: the route is restored over the same persisted group.
        updateBfdSessionState("9.1.0.1", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("9.1.0.2", SAI_BFD_SESSION_STATE_UP);
        updateBfdSessionState("9.1.0.3", SAI_BFD_SESSION_STATE_UP);
        const RouteCaptures::Route *r2 = findRoute("125.100.1.1");
        ASSERT_NE(r2, nullptr);
        EXPECT_EQ(r2->next_hop_id, nhg);
        EXPECT_EQ(activeMembers(nhg), 3U);
        checkStateDbRoute("Vnet25", "125.100.1.1/32", "9.0.0.1,9.0.0.2,9.0.0.3");
        checkRouteNotAdvertised("125.100.1.1/32");

        // Deleting the route removes the group, its STATE_DB entry, and the BFD
        // sessions for all its monitors.
        delVnetRouteMonitored("Vnet25", "125.100.1.1/32");
        EXPECT_NE(find(m_rt.removedGroups.begin(), m_rt.removedGroups.end(), nhg),
                  m_rt.removedGroups.end());
        checkStateDbRouteRemoved("Vnet25", "125.100.1.1/32");
        for (const char *mon : {"9.1.0.1", "9.1.0.2", "9.1.0.3"})
            EXPECT_FALSE(bfdSessionExists(mon)) << "BFD session " << mon << " not removed";
    }

    // A default-scope VNET (scope=default) reuses the global default virtual
    // router instead of creating a per-VNET one -- the mock equivalent of
    // test_vnet_orch_5 / vnet_lib.check_default_vnet_entry (which asserts no new
    // SAI_OBJECT_TYPE_VIRTUAL_ROUTER is created). The VXLAN tunnel is still
    // programmed and its VR<->VNI tunnel-map entries are keyed by the default
    // VR (gVirtualRouterId), and deleting the VNET removes neither the default
    // VR nor recreates one.
    TEST_F(VNetOrchTest, VnetDefaultScopeReusesDefaultVirtualRouter)
    {
        // A default-scope VNET must not create or remove a virtual router.
        EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router).Times(0);
        EXPECT_CALL(*mock_sai_virtual_router_api, remove_virtual_router).Times(0);

        setVxlanTunnel("tunnel_5", "8.8.8.8");
        setVnet("Vnet_5", "tunnel_5", "4789", "", false, "", "default");

        // The tunnel is still fully programmed (one tunnel, four maps, one term).
        ASSERT_EQ(m_tun.tunnels.size(), 1U);
        ASSERT_EQ(m_tun.maps.size(), 4U);
        ASSERT_EQ(m_tun.terms.size(), 1U);

        // The two VR<->VNI map entries carry the VNI and are keyed by the default
        // VR (the mock equivalent of check_vxlan_tunnel_entry against the default
        // VNET's default-VR mapper).
        ASSERT_EQ(m_tun.mapEntries.size(), 2U);
        const auto &e0 = m_tun.mapEntries[0];
        EXPECT_EQ(e0.map_type, SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI);
        EXPECT_EQ(e0.vr_key, gVirtualRouterId);
        EXPECT_EQ(e0.vni_value, 4789U);
        const auto &e1 = m_tun.mapEntries[1];
        EXPECT_EQ(e1.map_type, SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID);
        EXPECT_EQ(e1.vni_key, 4789U);
        EXPECT_EQ(e1.vr_value, gVirtualRouterId);

        // Deleting the VNET removes its two map entries but not the default VR
        // (the Times(0) on remove_virtual_router above), and deleting the tunnel
        // tears down the shared tunnel/maps/term.
        delVnet("Vnet_5");
        EXPECT_EQ(m_tun.removedMapEntries.size(), 2U);

        delVxlanTunnel("tunnel_5");
        EXPECT_EQ(m_tun.removedTunnels.size(), 1U);
        EXPECT_EQ(m_tun.removedMaps.size(), 4U);
        EXPECT_EQ(m_tun.removedTerms.size(), 1U);
    }

    // Migration of test_vnet_orch_4's peering scenario. In the VS suite that test
    // is @pytest.mark.skip ("Failing. Under investigation"), so this restores real
    // coverage of the VNET peering path rather than duplicating a live test. Two
    // VNETs list each other in peer_list; a tunnel route created in one VNET is
    // replicated into the peer VNET's virtual router as well as its own --
    // VNetRouteOrch::doRouteTask<VNetVrfObject> builds vr_set = getVRids(own) plus
    // each peer's VRidIngress and programs the route once per VR
    // (vnetorch.cpp:1746-1784). This is the check_vnet_routes(dvs, 'Vnet3004', ...)
    // assertion the VS test made on the *peer* VNET.
    TEST_F(VNetOrchTest, VnetPeeringReplicatesRouteIntoPeerVr)
    {
        setVxlanTunnel("tunnel_peer", "10.10.10.10");

        // Vnet_peerA and Vnet_peerB peer with each other; each gets its own VR.
        setVnet("Vnet_peerA", "tunnel_peer", "3003", "Vnet_peerB");
        const sai_object_id_t vrA = m_vrMock->created_oid;
        setVnet("Vnet_peerB", "tunnel_peer", "3004", "Vnet_peerA");
        const sai_object_id_t vrB = m_vrMock->created_oid;
        ASSERT_NE(vrA, SAI_NULL_OBJECT_ID);
        ASSERT_NE(vrB, SAI_NULL_OBJECT_ID);
        ASSERT_NE(vrA, vrB);

        // A tunnel route created in Vnet_peerA must be programmed in BOTH VRs.
        setVnetRoute("Vnet_peerA", "5.5.5.10/32", "fd:2::35");

        set<sai_object_id_t> vrs;
        for (const auto &r : m_rt.routes)
            if (prefixAddrEquals(r.dest, "5.5.5.10")) vrs.insert(r.vr);
        EXPECT_TRUE(vrs.count(vrA)) << "route missing from its own VNET's VR";
        EXPECT_TRUE(vrs.count(vrB)) << "route not replicated into the peer VNET's VR";
        EXPECT_EQ(vrs.size(), 2U);
        // Both VRs point the prefix at the same remote tunnel-encap next hop.
        checkEndpointType("fd:2::35", SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP);

        // Deleting the route withdraws it from both VRs (doRouteTask DEL iterates
        // the same vr_set); tearing down mirrors the VS order.
        delVnetRoute("Vnet_peerA", "5.5.5.10/32");
        EXPECT_EQ(findRoute("5.5.5.10"), nullptr);
        delVnet("Vnet_peerA");
        delVnet("Vnet_peerB");
        delVxlanTunnel("tunnel_peer");
    }

    // Mock equivalent of test_vnet_orch_24 -- the duplicate-route regression from
    // commit 6e25014c ("Handle duplicate routes in a graceful manner"). A
    // default-scope VNET programs a tunnel route for 100.100.1.0/24 directly in
    // the default virtual router (gVirtualRouterId, via the single
    // create_route_entry). A regular route for the SAME prefix -- normally
    // learned via FRR/BGP and delivered by fpmsyncd, here substituted as a direct
    // APP_DB ROUTE_TABLE write (that netlink->APP_DB translation is covered by
    // test_route + tests_fpmsyncd, plan Sec 8.4) -- then drives gRouteOrch, which
    // is unaware of the VNET route and tries to program the same prefix in the
    // same VR. libsaivs returns SAI_STATUS_ITEM_ALREADY_EXISTS (or NOT_EXECUTED
    // for the other entries of a multi-route bulk) for that create, and
    // handleSaiCreateStatus must treat it as success instead of calling
    // handleSaiFailure (which records an unhealthy status + logs "Encountered
    // failure in create operation"). The VS test asserts this via check_syslog
    // for the ABSENCE of that failure message; the mock equivalent is
    // getSaiFailureStatus() staying false. The remove side is symmetric
    // (handleSaiRemoveStatus with ITEM_NOT_FOUND/NOT_EXECUTED). The captured
    // duplicate status proves the conflicting create was really exercised, and
    // the surviving route entry is the check_route_entries equivalent.
    TEST_F(VNetOrchTest, VnetDuplicateRouteHandledGracefully)
    {
        // scope=default -> the VNET reuses gVirtualRouterId (no VR of its own).
        EXPECT_CALL(*mock_sai_virtual_router_api, create_virtual_router).Times(0);

        setVxlanTunnel("tunnel_24", "10.10.10.10");
        setVnet("Vnet_2000", "tunnel_24", "2000", "", false, "", "default");

        // The VNET tunnel route is programmed directly in the default VR as a
        // tunnel route to the (remote) endpoint 10.10.10.3.
        setVnetRoute("Vnet_2000", "100.100.1.0/24", "10.10.10.3");
        const RouteCaptures::Route *vnetRoute = findRoute("100.100.1.0");
        ASSERT_NE(vnetRoute, nullptr);
        EXPECT_EQ(vnetRoute->vr, gVirtualRouterId);

        // Bring up Ethernet0 in the default VRF and resolve 10.10.10.3 as a local
        // neighbor so the substituted regular route's next hop resolves.
        createL3Interface("Ethernet0", "10.10.10.1/24");
        setPortOperStatus("Ethernet0", SAI_PORT_OPER_STATUS_UP);
        addNeighbor("Ethernet0", "10.10.10.3", "00:00:00:00:00:03");

        // Clear any stale unhealthy flag so this window observes only our ops.
        setSaiFailureStatus(false, "");

        // Substitute the FRR route (vtysh "ip route 100.100.1.0/24 10.10.10.3"):
        // RouteOrch tries to program the same prefix in the same (default) VR ->
        // libsaivs duplicate.
        m_rt.lastBulkCreateStatuses.clear();
        setRoute("100.100.1.0/24", "10.10.10.3", "Ethernet0");

        // The conflicting create really hit a duplicate (regression path
        // exercised): libsaivs returns ITEM_ALREADY_EXISTS for the same
        // (VR, prefix).
        bool sawDup = false;
        for (auto s : m_rt.lastBulkCreateStatuses)
            if (s == SAI_STATUS_ITEM_ALREADY_EXISTS || s == SAI_STATUS_NOT_EXECUTED)
                sawDup = true;
        EXPECT_TRUE(sawDup) << "expected the duplicate route create to return "
                               "ITEM_ALREADY_EXISTS/NOT_EXECUTED from libsaivs";

        // The mock equivalent of the VS check_syslog: handleSaiCreateStatus
        // swallowed the duplicate, so orchagent recorded no SAI failure. Without
        // the graceful handling this flag would be set and the failure logged.
        std::string saiErr;
        EXPECT_FALSE(getSaiFailureStatus(saiErr))
            << "unexpected SAI failure on duplicate route create: " << saiErr;

        // Route still present (check_route_entries equivalent).
        EXPECT_NE(findRoute("100.100.1.0"), nullptr);

        // Tear down mirroring the VS order (remove FRR route, then the VNET
        // route) -- exercising the graceful *remove* path: the second remove of
        // the now-shared prefix returns ITEM_NOT_FOUND/NOT_EXECUTED, which
        // handleSaiRemoveStatus must also swallow.
        delRoute("100.100.1.0/24");
        delVnetRoute("Vnet_2000", "100.100.1.0/24");
        EXPECT_FALSE(getSaiFailureStatus(saiErr))
            << "unexpected SAI failure on duplicate route remove: " << saiErr;

        delVnet("Vnet_2000");
        delVxlanTunnel("tunnel_24");
    }

    // A (non-default-scope) VNET VR gets an IPv6 link-local (fe80::/10) trap
    // route installed at VR creation that forwards matched control packets to
    // the CPU -- the mock equivalent of test_vnet_ip2me_link_local. RouteOrch's
    // constructor installs these only for the default VR, so
    // VNetVrfObject::createObj adds one per VNET VR (vnetorch.cpp:110) and its
    // destructor removes it (vnetorch.cpp:361). The route is programmed with the
    // real create_route_entry (captured in m_rt.routes), unlike VNET tunnel
    // routes, so its vr/action/next-hop are directly assertable.
    TEST_F(VNetOrchTest, VnetInstallsIpv6LinkLocalTrapRoute)
    {
        setVxlanTunnel("tunnel_ip2me", "10.10.10.10");
        setVnet("Vnet_ip2me", "tunnel_ip2me", "20001", "");

        const sai_object_id_t vnetVr = m_vrMock->created_oid;
        ASSERT_NE(vnetVr, SAI_NULL_OBJECT_ID);
        ASSERT_NE(vnetVr, gVirtualRouterId);

        // Exactly one fe80::/10 route was programmed (the VS test asserts
        // ll_prefixes == ["fe80::/10"]). The default-VR link-local routes
        // RouteOrch installs during SetUp() predate the capture, so only the
        // VNET VR's route is seen here.
        const RouteCaptures::Route *ll = nullptr;
        size_t llCount = 0;
        for (const auto &r : m_rt.routes)
        {
            if (prefixAddrEquals(r.dest, "fe80::"))
            {
                llCount++;
                ll = &r;
            }
        }
        ASSERT_EQ(llCount, 1U);
        // /10 mask: first 10 bits set -> 0xff 0xc0.
        EXPECT_EQ(ll->dest.mask.ip6[0], 0xff);
        EXPECT_EQ(ll->dest.mask.ip6[1], 0xc0);

        // Programmed in the VNET's VR, not the default VR.
        EXPECT_EQ(ll->vr, vnetVr);

        // FORWARD-to-CPU: packet action FORWARD + next hop set to the CPU port.
        EXPECT_EQ(ll->packet_action, SAI_PACKET_ACTION_FORWARD);
        EXPECT_NE(ll->next_hop_id, SAI_NULL_OBJECT_ID);

        // Deleting the VNET removes the link-local trap route.
        delVnet("Vnet_ip2me");
        bool removed = false;
        for (const auto &d : m_rt.removedRoutes)
            if (prefixAddrEquals(d, "fe80::")) removed = true;
        EXPECT_TRUE(removed);

        delVxlanTunnel("tunnel_ip2me");
    }
}
