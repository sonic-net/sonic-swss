#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#define private public
#define protected public
#include "muxorch.h"
#undef protected
#undef private
#include "mock_orchagent_main.h"
#include "mock_orch_test.h"
#include "ipaddress.h"
#include "ipprefix.h"
#include "gtest/gtest.h"
#include <string>

namespace mux_subnet_slicing_test
{
    using namespace std;
    using namespace mock_orch_test;
    using namespace swss;

    // -----------------------------------------------------------------
    // Test helpers backed by MuxOrch internals (mux_nexthop_tb_ for NH
    // state, nh_route_refs_ for per-NH route refs).
    // -----------------------------------------------------------------
    static size_t getSliceSuppressedCount(MuxOrch* orch, const std::string& port)
    {
        size_t n = 0;
        for (const auto& kv : orch->mux_nexthop_tb_)
        {
            if (kv.second.port_name == port &&
                kv.second.state == MuxNhState::SuppressedResolved)
            {
                ++n;
            }
        }
        return n;
    }

    static bool isSliceSuppressed(MuxOrch* orch, const IpAddress& ip)
    {
        for (const auto& kv : orch->mux_nexthop_tb_)
        {
            if (kv.first.ip_address == ip &&
                kv.second.state == MuxNhState::SuppressedResolved)
            {
                return true;
            }
        }
        return false;
    }

    static bool isReferencedByRouteNh(MuxOrch* orch, const IpAddress& ip)
    {
        for (const auto& kv : orch->nh_route_refs_)
        {
            if (kv.first.ip_address == ip && !kv.second.empty())
            {
                return true;
            }
        }
        return false;
    }

    // Per-cable refcount: total live route refs across every NH whose
    // owning port matches `port`.
    static size_t getRouteNhRefCountForPort(MuxOrch* orch, const std::string& port)
    {
        size_t n = 0;
        for (const auto& kv : orch->nh_route_refs_)
        {
            if (kv.first.alias == port)
            {
                n += kv.second.size();
            }
        }
        return n;
    }

    // Drive a route NH add/remove the way RouteOrch does: per-NH
    // onRouteAdd/onRouteRemove plus a drainPendingReSuppress() at the
    // end of the remove tick (mirrors RouteOrch::doTask).
    static void applyRouteNhChange(MuxOrch* orch,
                                   const NextHopGroupKey& nhg,
                                   sai_object_id_t vrf,
                                   const IpPrefix& prefix,
                                   bool add)
    {
        RouteKey rk{vrf, prefix};
        for (const auto& nh : nhg.getNextHops())
        {
            if (add)
            {
                orch->onRouteAdd(nh, rk);
            }
            else
            {
                orch->onRouteRemove(nh, rk);
            }
        }
        if (!add)
        {
            orch->drainPendingReSuppress();
        }
    }

    static const string SLICE_INTERFACE = "Ethernet4";
    static const string NON_SLICE_INTERFACE = "Ethernet8";

    static const string SLICE_SERVER_IP4   = "192.168.0.2";
    static const string SLICE_SERVER_IP6   = "fc00::100";
    static const string SLICE_PREFIX       = "fc00::/64";
    static const string IN_SLICE_NEIGHBOR  = "fc00::200";
    static const string IN_SLICE_NEIGHBOR2 = "fc00::deef";
    static const string OUT_OF_SLICE_IP    = "fc01::1";

    static const string NON_SLICE_SERVER_IP4 = "192.168.0.3";
    static const string NON_SLICE_SERVER_IP6 = "fd00::100";

    class MuxSubnetSlicingTest : public MockOrchTest
    {
    protected:
        void ApplyInitialConfigs() override
        {
            Table peer_switch_table = Table(m_config_db.get(), CFG_PEER_SWITCH_TABLE_NAME);
            Table decap_tunnel_table = Table(m_app_db.get(), APP_TUNNEL_DECAP_TABLE_NAME);
            Table decap_term_table = Table(m_app_db.get(), APP_TUNNEL_DECAP_TERM_TABLE_NAME);
            Table mux_cable_table = Table(m_config_db.get(), CFG_MUX_CABLE_TABLE_NAME);
            Table port_table = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
            Table vlan_table = Table(m_app_db.get(), APP_VLAN_TABLE_NAME);
            Table vlan_member_table = Table(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);
            Table intf_table = Table(m_app_db.get(), APP_INTF_TABLE_NAME);

            auto ports = ut_helper::getInitialSaiPorts();
            port_table.set(SLICE_INTERFACE, ports[SLICE_INTERFACE]);
            port_table.set(NON_SLICE_INTERFACE, ports[NON_SLICE_INTERFACE]);
            port_table.set("PortConfigDone", { { "count", to_string(2) } });
            port_table.set("PortInitDone", { {} });

            vlan_table.set(VLAN_1000, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "00:aa:bb:cc:dd:ee" } });
            vlan_member_table.set(
                VLAN_1000 + vlan_member_table.getTableNameSeparator() + SLICE_INTERFACE,
                { { "tagging_mode", "untagged" } });
            vlan_member_table.set(
                VLAN_1000 + vlan_member_table.getTableNameSeparator() + NON_SLICE_INTERFACE,
                { { "tagging_mode", "untagged" } });

            intf_table.set(VLAN_1000, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
                                        { "mac_addr", "00:00:00:00:00:00" } });
            intf_table.set(
                VLAN_1000 + intf_table.getTableNameSeparator() + "192.168.0.1/21",
                { { "scope", "global" }, { "family", "IPv4" } });
            intf_table.set(
                VLAN_1000 + intf_table.getTableNameSeparator() + "fc00::1/64",
                { { "scope", "global" }, { "family", "IPv6" } });

            decap_term_table.set(
                "MuxTunnel0" + decap_term_table.getTableNameSeparator() + "2.2.2.2",
                { { "src_ip", "1.1.1.1" }, { "term_type", "P2P" } });

            decap_tunnel_table.set("MuxTunnel0",
                { { "dscp_mode", "uniform" },
                  { "src_ip", "1.1.1.1" },
                  { "ecn_mode", "copy_from_outer" },
                  { "encap_ecn_mode", "standard" },
                  { "ttl_mode", "pipe" },
                  { "tunnel_type", "IPINIP" } });

            peer_switch_table.set(PEER_SWITCH_HOSTNAME, { { "address_ipv4", PEER_IPV4_ADDRESS } });

            mux_cable_table.set(SLICE_INTERFACE,
                { { "server_ipv4", SLICE_SERVER_IP4 + "/32" },
                  { "server_ipv6", SLICE_SERVER_IP6 + "/128" },
                  { "server_ipv6_subnet", SLICE_PREFIX },
                  { "state", "auto" } });

            mux_cable_table.set(NON_SLICE_INTERFACE,
                { { "server_ipv4", NON_SLICE_SERVER_IP4 + "/32" },
                  { "server_ipv6", NON_SLICE_SERVER_IP6 + "/128" },
                  { "state", "auto" } });

            gPortsOrch->addExistingData(&port_table);
            gPortsOrch->addExistingData(&vlan_table);
            gPortsOrch->addExistingData(&vlan_member_table);
            static_cast<Orch *>(gPortsOrch)->doTask();

            gIntfsOrch->addExistingData(&intf_table);
            static_cast<Orch *>(gIntfsOrch)->doTask();

            m_TunnelDecapOrch->addExistingData(&decap_tunnel_table);
            m_TunnelDecapOrch->addExistingData(&decap_term_table);
            static_cast<Orch *>(m_TunnelDecapOrch)->doTask();

            m_MuxOrch->addExistingData(&peer_switch_table);
            static_cast<Orch *>(m_MuxOrch)->doTask();

            m_MuxOrch->addExistingData(&mux_cable_table);
            static_cast<Orch *>(m_MuxOrch)->doTask();
        }

        MuxCable* getSliceCable()  { return m_MuxOrch->getMuxCable(SLICE_INTERFACE); }
        MuxCable* getPlainCable()  { return m_MuxOrch->getMuxCable(NON_SLICE_INTERFACE); }
    };

    // --- Parsing tests ---------------------------------------------------

    TEST_F(MuxSubnetSlicingTest, ParsesValidSlicePrefix)
    {
        MuxCable* cable = getSliceCable();
        ASSERT_NE(cable, nullptr);
        EXPECT_TRUE(cable->hasSlicePrefix());
        EXPECT_EQ(cable->getSlicePrefix(), IpPrefix(SLICE_PREFIX));
    }

    TEST_F(MuxSubnetSlicingTest, NoSlicePrefixWhenFieldAbsent)
    {
        MuxCable* cable = getPlainCable();
        ASSERT_NE(cable, nullptr);
        EXPECT_FALSE(cable->hasSlicePrefix());
    }

    TEST_F(MuxSubnetSlicingTest, RejectsIpv4SlicePrefix)
    {
        // Use a fresh cable name so prior tests' redis residue can't leak in.
        const std::string port = "Ethernet12";
        Table mux_cable_table = Table(m_config_db.get(), CFG_MUX_CABLE_TABLE_NAME);
        mux_cable_table.del(port);
        mux_cable_table.set(port,
            { { "server_ipv4", NON_SLICE_SERVER_IP4 + "/32" },
              { "server_ipv6", NON_SLICE_SERVER_IP6 + "/128" },
              { "server_ipv6_subnet", "10.0.0.0/24" },
              { "state", "auto" } });
        m_MuxOrch->addExistingData(&mux_cable_table);
        static_cast<Orch *>(m_MuxOrch)->doTask();

        EXPECT_EQ(m_MuxOrch->mux_cable_tb_.count(port), 0u);
    }

    TEST_F(MuxSubnetSlicingTest, RejectsZeroBaseSlicePrefix)
    {
        const std::string port = "Ethernet12";
        Table mux_cable_table = Table(m_config_db.get(), CFG_MUX_CABLE_TABLE_NAME);
        mux_cable_table.del(port);
        mux_cable_table.set(port,
            { { "server_ipv4", NON_SLICE_SERVER_IP4 + "/32" },
              { "server_ipv6", NON_SLICE_SERVER_IP6 + "/128" },
              { "server_ipv6_subnet", "::/64" },
              { "state", "auto" } });
        m_MuxOrch->addExistingData(&mux_cable_table);
        static_cast<Orch *>(m_MuxOrch)->doTask();

        EXPECT_EQ(m_MuxOrch->mux_cable_tb_.count(port), 0u);
    }

    // --- findMuxCableBySlice --------------------------------------------

    TEST_F(MuxSubnetSlicingTest, FindMuxCableBySliceMatchesEnclosing)
    {
        MuxCable* cable = m_MuxOrch->findMuxCableBySlice(IpAddress(IN_SLICE_NEIGHBOR));
        ASSERT_NE(cable, nullptr);
        EXPECT_EQ(cable, getSliceCable());
    }

    TEST_F(MuxSubnetSlicingTest, FindMuxCableBySliceMissesOutsideAddress)
    {
        EXPECT_EQ(m_MuxOrch->findMuxCableBySlice(IpAddress(OUT_OF_SLICE_IP)), nullptr);
    }

    TEST_F(MuxSubnetSlicingTest, FindMuxCableBySliceIgnoresIpv4)
    {
        EXPECT_EQ(m_MuxOrch->findMuxCableBySlice(IpAddress(SLICE_SERVER_IP4)), nullptr);
    }

    // --- isInSliceSuppressed --------------------------------------------

    TEST_F(MuxSubnetSlicingTest, SuppressesInSliceNeighbor)
    {
        EXPECT_TRUE(m_MuxOrch->isInSliceSuppressed(IpAddress(IN_SLICE_NEIGHBOR)));
        EXPECT_TRUE(m_MuxOrch->isInSliceSuppressed(IpAddress(IN_SLICE_NEIGHBOR2)));
    }

    TEST_F(MuxSubnetSlicingTest, DoesNotSuppressSliceAnchor)
    {
        EXPECT_FALSE(m_MuxOrch->isInSliceSuppressed(IpAddress(SLICE_SERVER_IP6)));
    }

    TEST_F(MuxSubnetSlicingTest, DoesNotSuppressOutOfSliceAddress)
    {
        EXPECT_FALSE(m_MuxOrch->isInSliceSuppressed(IpAddress(OUT_OF_SLICE_IP)));
    }

    TEST_F(MuxSubnetSlicingTest, DoesNotSuppressIpv4)
    {
        EXPECT_FALSE(m_MuxOrch->isInSliceSuppressed(IpAddress(SLICE_SERVER_IP4)));
    }

    TEST_F(MuxSubnetSlicingTest, DoesNotSuppressSkipNeighbor)
    {
        IpAddress soc(IN_SLICE_NEIGHBOR2);
        m_MuxOrch->addSkipNeighbors({ soc }, SLICE_INTERFACE);
        EXPECT_FALSE(m_MuxOrch->isInSliceSuppressed(soc));
        EXPECT_TRUE(m_MuxOrch->isInSliceSuppressed(IpAddress(IN_SLICE_NEIGHBOR)));
        m_MuxOrch->removeSkipNeighbors({ soc });
        EXPECT_TRUE(m_MuxOrch->isInSliceSuppressed(soc));
    }

    TEST_F(MuxSubnetSlicingTest, DoesNotSuppressWhenNoSliceConfigured)
    {
        EXPECT_FALSE(m_MuxOrch->isInSliceSuppressed(IpAddress(NON_SLICE_SERVER_IP6)));
    }

    // --- Partial-SET mutation guard -------------------------------------

    TEST_F(MuxSubnetSlicingTest, PartialUpdateWithoutSlicePrefixIsAllowed)
    {
        Table mux_cable_table = Table(m_config_db.get(), CFG_MUX_CABLE_TABLE_NAME);
        // Re-apply WITHOUT server_ipv6_subnet — should not be flagged as a mismatch.
        mux_cable_table.set(SLICE_INTERFACE,
            { { "server_ipv4", SLICE_SERVER_IP4 + "/32" },
              { "server_ipv6", SLICE_SERVER_IP6 + "/128" },
              { "state", "auto" } });
        m_MuxOrch->addExistingData(&mux_cable_table);
        static_cast<Orch *>(m_MuxOrch)->doTask();

        MuxCable* cable = getSliceCable();
        ASSERT_NE(cable, nullptr);
        // Slice prefix must still be in effect.
        EXPECT_TRUE(cable->hasSlicePrefix());
        EXPECT_EQ(cable->getSlicePrefix(), IpPrefix(SLICE_PREFIX));
    }

    // --- Stage 2a: updateNeighbor slice gate ----------------------------

    TEST_F(MuxSubnetSlicingTest, UpdateNeighborTracksInSliceNeighbor)
    {
        EXPECT_EQ(getSliceSuppressedCount(m_MuxOrch, SLICE_INTERFACE), 0u);

        NeighborEntry entry(IpAddress(IN_SLICE_NEIGHBOR), SLICE_INTERFACE);
        NeighborUpdate update = { entry, MacAddress("00:aa:bb:cc:dd:01"), true };
        m_MuxOrch->updateNeighbor(update);

        EXPECT_TRUE(isSliceSuppressed(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR)));
        EXPECT_EQ(getSliceSuppressedCount(m_MuxOrch, SLICE_INTERFACE), 1u);
        // In-slice neighbor MUST NOT be programmed as a per-host mux nexthop.
        EXPECT_FALSE(m_MuxOrch->containsNextHop(NextHopKey(IpAddress(IN_SLICE_NEIGHBOR), SLICE_INTERFACE)));
    }

    TEST_F(MuxSubnetSlicingTest, UpdateNeighborUntracksOnRemove)
    {
        NeighborEntry entry(IpAddress(IN_SLICE_NEIGHBOR), SLICE_INTERFACE);
        m_MuxOrch->updateNeighbor({ entry, MacAddress("00:aa:bb:cc:dd:02"), true });
        ASSERT_EQ(getSliceSuppressedCount(m_MuxOrch, SLICE_INTERFACE), 1u);

        m_MuxOrch->updateNeighbor({ entry, MacAddress("00:aa:bb:cc:dd:02"), false });
        EXPECT_FALSE(isSliceSuppressed(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR)));
        EXPECT_EQ(getSliceSuppressedCount(m_MuxOrch, SLICE_INTERFACE), 0u);
    }

    TEST_F(MuxSubnetSlicingTest, UpdateNeighborSkipsSliceAnchor)
    {
        NeighborEntry entry(IpAddress(SLICE_SERVER_IP6), SLICE_INTERFACE);
        m_MuxOrch->updateNeighbor({ entry, MacAddress("00:aa:bb:cc:dd:03"), true });

        EXPECT_FALSE(isSliceSuppressed(m_MuxOrch, IpAddress(SLICE_SERVER_IP6)));
        EXPECT_EQ(getSliceSuppressedCount(m_MuxOrch, SLICE_INTERFACE), 0u);
    }

    TEST_F(MuxSubnetSlicingTest, UpdateNeighborSkipsSkipNeighbor)
    {
        IpAddress soc(IN_SLICE_NEIGHBOR2);
        m_MuxOrch->addSkipNeighbors({ soc }, SLICE_INTERFACE);

        NeighborEntry entry(soc, SLICE_INTERFACE);
        m_MuxOrch->updateNeighbor({ entry, MacAddress("00:aa:bb:cc:dd:04"), true });

        EXPECT_FALSE(isSliceSuppressed(m_MuxOrch, soc));
        EXPECT_EQ(getSliceSuppressedCount(m_MuxOrch, SLICE_INTERFACE), 0u);

        m_MuxOrch->removeSkipNeighbors({ soc });
    }

    TEST_F(MuxSubnetSlicingTest, UpdateNeighborIgnoresIpv4ForSliceGate)
    {
        NeighborEntry entry(IpAddress(SLICE_SERVER_IP4), SLICE_INTERFACE);
        m_MuxOrch->updateNeighbor({ entry, MacAddress("00:aa:bb:cc:dd:05"), true });

        EXPECT_EQ(getSliceSuppressedCount(m_MuxOrch, SLICE_INTERFACE), 0u);
    }

    TEST_F(MuxSubnetSlicingTest, UpdateNeighborVlanAliasWithoutFdbStillSuppresses)
    {
        // Regression guard for the port-affinity fix: when the neighbor's
        // alias is a VLAN but no FDB entry exists yet for the MAC, getMuxPort
        // returns an empty port name. The slice gate must NOT fall through —
        // it should still suppress the in-slice IP, preserving the
        // race-tolerant default for not-yet-learned FDB entries.
        NeighborEntry entry(IpAddress(IN_SLICE_NEIGHBOR), VLAN_1000);
        m_MuxOrch->updateNeighbor({ entry, MacAddress("00:aa:bb:cc:dd:fa"), true });

        EXPECT_TRUE(isSliceSuppressed(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR)));
        EXPECT_EQ(getSliceSuppressedCount(m_MuxOrch, SLICE_INTERFACE), 1u);
    }

    TEST_F(MuxSubnetSlicingTest, UpdateNeighborOutOfSliceIpUnaffected)
    {
        NeighborEntry entry(IpAddress(OUT_OF_SLICE_IP), SLICE_INTERFACE);
        m_MuxOrch->updateNeighbor({ entry, MacAddress("00:aa:bb:cc:dd:06"), true });

        EXPECT_FALSE(isSliceSuppressed(m_MuxOrch, IpAddress(OUT_OF_SLICE_IP)));
        EXPECT_EQ(getSliceSuppressedCount(m_MuxOrch, SLICE_INTERFACE), 0u);
    }

    // --- Stage 2a: convertNeighborToMux slice gate ----------------------

    // Slice suppression is routed through MuxOrch::updateNeighbor only;
    // convertNeighborToMux is not a state-mutating path for slice tracking.

    TEST_F(MuxSubnetSlicingTest, ConvertNeighborToMuxDoesNotShortCircuitAnchor)
    {
        NeighborEntry entry(IpAddress(SLICE_SERVER_IP6), SLICE_INTERFACE);
        // Anchor must not be added to the suppressed set even if the call
        // exits later for unrelated reasons.
        (void)m_MuxOrch->convertNeighborToMux(entry, SLICE_INTERFACE, "unit test");

        EXPECT_FALSE(isSliceSuppressed(m_MuxOrch, IpAddress(SLICE_SERVER_IP6)));
        EXPECT_EQ(getSliceSuppressedCount(m_MuxOrch, SLICE_INTERFACE), 0u);
    }

    // ---------------------------------------------------------------------
    // Route-NH skip (un-suppress in-slice IPs used as route NHs)
    // ---------------------------------------------------------------------
    //
    // These tests drive MuxOrch::onRouteNexthopsChange directly with
    // NextHopGroupKey values, mirroring how RouteOrch invokes it on
    // addRoute/removeRoute. NeighOrch enable/disable calls inside the hook
    // are safe no-ops when the IP isn't in m_syncdNeighbors, so the fixture
    // doesn't need to seed neighbors for these checks.

    TEST_F(MuxSubnetSlicingTest, RouteNhAddUnsuppressesInSliceIp)
    {
        // Baseline: in-slice IP is within the slice scope (gate predicate).
        EXPECT_TRUE(m_MuxOrch->isInSliceSuppressed(IpAddress(IN_SLICE_NEIGHBOR)));

        NextHopGroupKey nhg(IN_SLICE_NEIGHBOR + std::string("@") + SLICE_INTERFACE);
        applyRouteNhChange(m_MuxOrch, nhg, gVirtualRouterId, IpPrefix("2001:db8::/64"), true);

        EXPECT_TRUE(isReferencedByRouteNh(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR)));
        // Route arrived before learn → PlaceholderUnresolved, not Suppressed.
        EXPECT_FALSE(isSliceSuppressed(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR)));
        EXPECT_EQ(getRouteNhRefCountForPort(m_MuxOrch, SLICE_INTERFACE), 1u);
    }

    TEST_F(MuxSubnetSlicingTest, RouteNhRemoveResuppressesWhenLastReferenceGone)
    {
        NextHopGroupKey nhg(IN_SLICE_NEIGHBOR + std::string("@") + SLICE_INTERFACE);
        applyRouteNhChange(m_MuxOrch, nhg, gVirtualRouterId, IpPrefix("2001:db8::/64"), true);
        applyRouteNhChange(m_MuxOrch, nhg, gVirtualRouterId, IpPrefix("2001:db8::/64"), false);

        EXPECT_FALSE(isReferencedByRouteNh(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR)));
        EXPECT_TRUE(m_MuxOrch->isInSliceSuppressed(IpAddress(IN_SLICE_NEIGHBOR)));
        EXPECT_EQ(getRouteNhRefCountForPort(m_MuxOrch, SLICE_INTERFACE), 0u);
    }

    TEST_F(MuxSubnetSlicingTest, RouteNhRefcountedAcrossMultipleRoutes)
    {
        // Learn the neighbor first so it's actually in SuppressedResolved
        // state — we only enter that state via the learn path.
        NeighborEntry entry(IpAddress(IN_SLICE_NEIGHBOR), SLICE_INTERFACE);
        m_MuxOrch->updateNeighbor({ entry, MacAddress("00:aa:bb:cc:dd:ff"), true });
        ASSERT_TRUE(isSliceSuppressed(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR)));

        NextHopGroupKey nhg1(IN_SLICE_NEIGHBOR + std::string("@") + SLICE_INTERFACE);
        NextHopGroupKey nhg2(IN_SLICE_NEIGHBOR + std::string("@") + SLICE_INTERFACE);
        applyRouteNhChange(m_MuxOrch, nhg1, gVirtualRouterId, IpPrefix("2001:db8:1::/64"), true);
        applyRouteNhChange(m_MuxOrch, nhg2, gVirtualRouterId, IpPrefix("2001:db8:2::/64"), true);

        EXPECT_FALSE(isSliceSuppressed(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR)));

        // Removing one of the two routes keeps the IP un-suppressed.
        applyRouteNhChange(m_MuxOrch, nhg1, gVirtualRouterId, IpPrefix("2001:db8:1::/64"), false);
        EXPECT_FALSE(isSliceSuppressed(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR)));
        EXPECT_TRUE(isReferencedByRouteNh(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR)));

        // Last route gone: re-suppress.
        applyRouteNhChange(m_MuxOrch, nhg2, gVirtualRouterId, IpPrefix("2001:db8:2::/64"), false);
        EXPECT_TRUE(isSliceSuppressed(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR)));
    }

    TEST_F(MuxSubnetSlicingTest, RouteNhEcmpGroupHandlesAllInSliceMembers)
    {
        // Learn both NHs into SuppressedResolved first.
        NeighborEntry e1(IpAddress(IN_SLICE_NEIGHBOR),  SLICE_INTERFACE);
        NeighborEntry e2(IpAddress(IN_SLICE_NEIGHBOR2), SLICE_INTERFACE);
        m_MuxOrch->updateNeighbor({ e1, MacAddress("00:aa:bb:cc:dd:01"), true });
        m_MuxOrch->updateNeighbor({ e2, MacAddress("00:aa:bb:cc:dd:02"), true });
        ASSERT_TRUE(isSliceSuppressed(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR)));
        ASSERT_TRUE(isSliceSuppressed(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR2)));

        // ECMP group with two in-slice NHs — both should be un-suppressed.
        NextHopGroupKey nhg(IN_SLICE_NEIGHBOR + std::string("@") + SLICE_INTERFACE +
                            std::string(",") +
                            IN_SLICE_NEIGHBOR2 + std::string("@") + SLICE_INTERFACE);
        applyRouteNhChange(m_MuxOrch, nhg, gVirtualRouterId, IpPrefix("2001:db8::/64"), true);

        EXPECT_FALSE(isSliceSuppressed(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR)));
        EXPECT_FALSE(isSliceSuppressed(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR2)));
        EXPECT_EQ(getRouteNhRefCountForPort(m_MuxOrch, SLICE_INTERFACE), 2u);

        applyRouteNhChange(m_MuxOrch, nhg, gVirtualRouterId, IpPrefix("2001:db8::/64"), false);
        EXPECT_TRUE(isSliceSuppressed(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR)));
        EXPECT_TRUE(isSliceSuppressed(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR2)));
    }

    TEST_F(MuxSubnetSlicingTest, RouteNhIgnoresOutOfSliceIp)
    {
        NextHopGroupKey nhg(OUT_OF_SLICE_IP + std::string("@") + SLICE_INTERFACE);
        applyRouteNhChange(m_MuxOrch, nhg, gVirtualRouterId, IpPrefix("2001:db8::/64"), true);

        EXPECT_EQ(getRouteNhRefCountForPort(m_MuxOrch, SLICE_INTERFACE), 0u);
        EXPECT_FALSE(m_MuxOrch->isInSliceSuppressed(IpAddress(OUT_OF_SLICE_IP)));
    }

    TEST_F(MuxSubnetSlicingTest, RouteNhIgnoresIpv4)
    {
        // IPv4 NHs cannot be in an IPv6 slice — must be a no-op.
        NextHopGroupKey nhg(SLICE_SERVER_IP4 + std::string("@") + SLICE_INTERFACE);
        applyRouteNhChange(m_MuxOrch, nhg, gVirtualRouterId, IpPrefix("2001:db8::/64"), true);
        EXPECT_EQ(getRouteNhRefCountForPort(m_MuxOrch, SLICE_INTERFACE), 0u);
    }

    TEST_F(MuxSubnetSlicingTest, RouteNhIgnoresSliceAnchor)
    {
        // server_ipv6 itself is the slice anchor: it's never slice-suppressed,
        // so a route pointing to it must not bump the refcount.
        NextHopGroupKey nhg(SLICE_SERVER_IP6 + std::string("@") + SLICE_INTERFACE);
        applyRouteNhChange(m_MuxOrch, nhg, gVirtualRouterId, IpPrefix("2001:db8::/64"), true);
        EXPECT_EQ(getRouteNhRefCountForPort(m_MuxOrch, SLICE_INTERFACE), 0u);
        EXPECT_FALSE(m_MuxOrch->isInSliceSuppressed(IpAddress(SLICE_SERVER_IP6)));
    }

    TEST_F(MuxSubnetSlicingTest, RouteNhIgnoresSkipNeighbor)
    {
        // soc_ip / explicitly skipped neighbors are out of slice-suppression
        // scope already; a route NH onto one should not be refcounted.
        IpAddress soc("fc00::301");
        m_MuxOrch->addSkipNeighbors({ soc }, SLICE_INTERFACE);
        NextHopGroupKey nhg(soc.to_string() + std::string("@") + SLICE_INTERFACE);
        applyRouteNhChange(m_MuxOrch, nhg, gVirtualRouterId, IpPrefix("2001:db8::/64"), true);
        EXPECT_EQ(getRouteNhRefCountForPort(m_MuxOrch, SLICE_INTERFACE), 0u);
        m_MuxOrch->removeSkipNeighbors({ soc });
    }

    TEST_F(MuxSubnetSlicingTest, RouteNhIgnoresNonDefaultVrf)
    {
        // Slice is scoped to the default VRF only; events from other VRFs
        // must be ignored entirely.
        NextHopGroupKey nhg(IN_SLICE_NEIGHBOR + std::string("@") + SLICE_INTERFACE);
        const sai_object_id_t bogus_vrf = 0xDEADBEEF;
        applyRouteNhChange(m_MuxOrch, nhg, bogus_vrf, IpPrefix("2001:db8::/64"), true);
        EXPECT_EQ(getRouteNhRefCountForPort(m_MuxOrch, SLICE_INTERFACE), 0u);
        EXPECT_TRUE(m_MuxOrch->isInSliceSuppressed(IpAddress(IN_SLICE_NEIGHBOR)));
    }

    TEST_F(MuxSubnetSlicingTest, RouteNhRemovesSliceSuppressedTrackingOnUnsuppress)
    {
        // If an in-slice IP is currently tracked as slice-suppressed (e.g.
        // because a neighbor was learned and gated), a later route NH ref
        // should remove it from the suppressed set.
        NeighborEntry entry(IpAddress(IN_SLICE_NEIGHBOR), SLICE_INTERFACE);
        m_MuxOrch->updateNeighbor({ entry, MacAddress("00:aa:bb:cc:dd:ff"), true });
        ASSERT_EQ(getSliceSuppressedCount(m_MuxOrch, SLICE_INTERFACE), 1u);

        NextHopGroupKey nhg(IN_SLICE_NEIGHBOR + std::string("@") + SLICE_INTERFACE);
        applyRouteNhChange(m_MuxOrch, nhg, gVirtualRouterId, IpPrefix("2001:db8::/64"), true);
        EXPECT_EQ(getSliceSuppressedCount(m_MuxOrch, SLICE_INTERFACE), 0u);

        // Drop the route: with no live neighbor in NeighOrch the deferred
        // drain has nothing to gate, but the IP must once again be considered
        // slice-suppressed from RouteOrch's perspective.
        applyRouteNhChange(m_MuxOrch, nhg, gVirtualRouterId, IpPrefix("2001:db8::/64"), false);
        static_cast<Orch *>(m_MuxOrch)->doTask();
        EXPECT_FALSE(isReferencedByRouteNh(m_MuxOrch, IpAddress(IN_SLICE_NEIGHBOR)));
        EXPECT_TRUE(m_MuxOrch->isInSliceSuppressed(IpAddress(IN_SLICE_NEIGHBOR)));
    }
}
