#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#define private public
#define protected public
#include "neighorch.h"
#include "muxorch.h"
#include "fdborch.h"
#undef protected
#undef private
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_orch_test.h"
#include "nexthopkey.h"
#include "ipaddress.h"
#include "gtest/gtest.h"
#include <string>

EXTERN_MOCK_FNS

namespace mux_subnet_slicing_test
{
    DEFINE_SAI_API_MOCK(neighbor);
    DEFINE_SAI_API_MOCK_SPECIFY_ENTRY_WITH_SET(route, route);
    DEFINE_SAI_GENERIC_API_MOCK(acl, acl_entry);
    DEFINE_SAI_GENERIC_API_OBJECT_BULK_MOCK(next_hop, next_hop);
    using ::testing::_;
    using namespace std;
    using namespace mock_orch_test;
    using ::testing::Return;
    using ::testing::DoAll;
    using ::testing::SetArrayArgument;

    static const string TEST_INTERFACE = "Ethernet4";
    static const string OTHER_INTERFACE = "Ethernet0";
    static const string SLICE_PREFIX = "fc02:1000::/64";
    static const string SERVER_IP6 = "a::a";
    static const string IN_SLICE_IP = "fc02:1000::5";
    static const string IN_SLICE_IP2 = "fc02:1000::6";
    static const string OUT_OF_SLICE_IP = "b::1";

    sai_bulk_create_neighbor_entry_fn old_create_neighbor_entries;
    sai_bulk_remove_neighbor_entry_fn old_remove_neighbor_entries;
    sai_bulk_create_route_entry_fn old_create_route_entries;
    sai_bulk_remove_route_entry_fn old_remove_route_entries;
    sai_bulk_set_route_entry_attribute_fn old_set_route_entries_attribute;
    sai_bulk_object_create_fn old_object_create;
    sai_bulk_object_remove_fn old_object_remove;

    // Fixture identical in spirit to MuxRollbackTest, but the mux cable is
    // configured with `server_ipv6_subnet` so the slice code paths are live.
    class MuxSubnetSlicingTest : public MockOrchTest
    {
    protected:
        void ApplyInitialConfigs()
        {
            Table peer_switch_table = Table(m_config_db.get(), CFG_PEER_SWITCH_TABLE_NAME);
            Table decap_tunnel_table = Table(m_app_db.get(), APP_TUNNEL_DECAP_TABLE_NAME);
            Table decap_term_table = Table(m_app_db.get(), APP_TUNNEL_DECAP_TERM_TABLE_NAME);
            Table mux_cable_table = Table(m_config_db.get(), CFG_MUX_CABLE_TABLE_NAME);
            Table port_table = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
            Table vlan_table = Table(m_app_db.get(), APP_VLAN_TABLE_NAME);
            Table vlan_member_table = Table(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);
            Table neigh_table = Table(m_app_db.get(), APP_NEIGH_TABLE_NAME);
            Table intf_table = Table(m_app_db.get(), APP_INTF_TABLE_NAME);

            auto ports = ut_helper::getInitialSaiPorts();
            port_table.set(TEST_INTERFACE, ports[TEST_INTERFACE]);
            port_table.set(OTHER_INTERFACE, ports[OTHER_INTERFACE]);
            port_table.set("PortConfigDone", { { "count", to_string(2) } });
            port_table.set("PortInitDone", { {} });

            neigh_table.set(
                VLAN_1000 + neigh_table.getTableNameSeparator() + SERVER_IP1, { { "neigh", "62:f9:65:10:2f:04" },
                                                                               { "family", "IPv4" } });

            vlan_table.set(VLAN_1000, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "00:aa:bb:cc:dd:ee" } });
            vlan_member_table.set(
                VLAN_1000 + vlan_member_table.getTableNameSeparator() + TEST_INTERFACE,
                { { "tagging_mode", "untagged" } });
            vlan_member_table.set(
                VLAN_1000 + vlan_member_table.getTableNameSeparator() + OTHER_INTERFACE,
                { { "tagging_mode", "untagged" } });

            intf_table.set(VLAN_1000, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
                                        { "mac_addr", "00:00:00:00:00:00" } });
            intf_table.set(
                VLAN_1000 + neigh_table.getTableNameSeparator() + "192.168.0.1/21", {
                                                                                        { "scope", "global" },
                                                                                        { "family", "IPv4" },
                                                                                    });
            intf_table.set(
                VLAN_1000 + neigh_table.getTableNameSeparator() + "a::1/64", {
                                                                                { "scope", "global" },
                                                                                { "family", "IPv6" },
                                                                            });

            decap_term_table.set(
                MUX_TUNNEL + neigh_table.getTableNameSeparator() + "2.2.2.2", { { "src_ip", "1.1.1.1" },
                                                                                { "term_type", "P2P" } });

            decap_tunnel_table.set(MUX_TUNNEL, { { "dscp_mode", "uniform" },
                                                 { "src_ip", "1.1.1.1" },
                                                 { "ecn_mode", "copy_from_outer" },
                                                 { "encap_ecn_mode", "standard" },
                                                 { "ttl_mode", "pipe" },
                                                 { "tunnel_type", "IPINIP" } });

            peer_switch_table.set(PEER_SWITCH_HOSTNAME, { { "address_ipv4", PEER_IPV4_ADDRESS } });

            mux_cable_table.set(TEST_INTERFACE, { { "server_ipv4", SERVER_IP1 + "/32" },
                                                  { "server_ipv6", SERVER_IP6 + "/128" },
                                                  { "server_ipv6_subnet", SLICE_PREFIX },
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

            gNeighOrch->addExistingData(&neigh_table);
            static_cast<Orch *>(gNeighOrch)->doTask();

            m_MuxCable = m_MuxOrch->getMuxCable(TEST_INTERFACE);

            EXPECT_EQ(STANDBY_STATE, m_MuxCable->getState());
        }

        void PostSetUp() override
        {
            INIT_SAI_API_MOCK(neighbor);
            INIT_SAI_API_MOCK(route);
            INIT_SAI_API_MOCK(acl);
            INIT_SAI_API_MOCK(next_hop);
            MockSaiApis();
            old_create_neighbor_entries = gNeighOrch->gNeighBulker.create_entries;
            old_remove_neighbor_entries = gNeighOrch->gNeighBulker.remove_entries;
            old_object_create = gNeighOrch->gNextHopBulker.create_entries;
            old_object_remove = gNeighOrch->gNextHopBulker.remove_entries;
            old_create_route_entries = m_MuxCable->nbr_handler_->gRouteBulker.create_entries;
            old_remove_route_entries = m_MuxCable->nbr_handler_->gRouteBulker.remove_entries;
            old_set_route_entries_attribute = m_MuxCable->nbr_handler_->gRouteBulker.set_entries_attribute;
            gNeighOrch->gNeighBulker.create_entries = mock_create_neighbor_entries;
            gNeighOrch->gNeighBulker.remove_entries = mock_remove_neighbor_entries;
            gNeighOrch->gNextHopBulker.create_entries = mock_create_next_hops;
            gNeighOrch->gNextHopBulker.remove_entries = mock_remove_next_hops;
            m_MuxCable->nbr_handler_->gRouteBulker.create_entries = mock_create_route_entries;
            m_MuxCable->nbr_handler_->gRouteBulker.remove_entries = mock_remove_route_entries;
            m_MuxCable->nbr_handler_->gRouteBulker.set_entries_attribute = mock_set_route_entries_attribute;
        }

        void PreTearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(next_hop);
            DEINIT_SAI_API_MOCK(acl);
            DEINIT_SAI_API_MOCK(route);
            DEINIT_SAI_API_MOCK(neighbor);
            gNeighOrch->gNeighBulker.create_entries = old_create_neighbor_entries;
            gNeighOrch->gNeighBulker.remove_entries = old_remove_neighbor_entries;
            gNeighOrch->gNextHopBulker.create_entries = old_object_create;
            gNeighOrch->gNextHopBulker.remove_entries = old_object_remove;
            m_MuxCable->nbr_handler_->gRouteBulker.create_entries = old_create_route_entries;
            m_MuxCable->nbr_handler_->gRouteBulker.remove_entries = old_remove_route_entries;
            m_MuxCable->nbr_handler_->gRouteBulker.set_entries_attribute = old_set_route_entries_attribute;
        }

        // Inject an FDB entry so getMuxPort(mac, Vlan1000) resolves to `port`.
        void InjectFdbEntry(const MacAddress& mac, const string& port)
        {
            Port vlan_port, eth_port;
            ASSERT_TRUE(gPortsOrch->getVlanByVlanId(1000, vlan_port));
            ASSERT_TRUE(gPortsOrch->getPort(port, eth_port));
            FdbEntry fdb_entry;
            fdb_entry.mac = mac;
            fdb_entry.bv_id = vlan_port.m_vlan_info.vlan_oid;
            fdb_entry.port_name = port;
            FdbData fdb_data{};
            fdb_data.bridge_port_id = eth_port.m_bridge_port_id;
            fdb_data.type = "dynamic";
            fdb_data.origin = FDB_ORIGIN_LEARN;
            gFdbOrch->m_entries[fdb_entry] = fdb_data;
        }

        void RemoveFdbEntry(const MacAddress& mac)
        {
            for (auto it = gFdbOrch->m_entries.begin(); it != gFdbOrch->m_entries.end(); )
            {
                if (it->first.mac == mac)
                {
                    it = gFdbOrch->m_entries.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    };

    // Sanity: the fixture established a slice and isIpInSlice classifies
    // correctly for IPv4, in-slice IPv6, and out-of-slice IPv6.
    TEST_F(MuxSubnetSlicingTest, SliceClassification)
    {
        ASSERT_TRUE(m_MuxOrch->isSliceConfigured());
        ASSERT_TRUE(m_MuxCable->hasSlicePrefix());

        EXPECT_TRUE(m_MuxCable->isIpInSlice(IpAddress(IN_SLICE_IP)));
        EXPECT_FALSE(m_MuxCable->isIpInSlice(IpAddress(OUT_OF_SLICE_IP)));
        // IPv4 must always be out of slice even if numerically in range.
        EXPECT_FALSE(m_MuxCable->isIpInSlice(IpAddress(SERVER_IP1)));

        EXPECT_EQ(m_MuxCable, m_MuxOrch->findMuxCableBySlice(IpAddress(IN_SLICE_IP)));
        EXPECT_EQ(nullptr, m_MuxOrch->findMuxCableBySlice(IpAddress(OUT_OF_SLICE_IP)));
    }

    // Truth table for isSuppressedNeighbor: empty port, non-slice port, the
    // server_ipv6 anchor, and a positive case must all behave correctly.
    TEST_F(MuxSubnetSlicingTest, IsSuppressedNeighborTruthTable)
    {
        MuxCable* out = nullptr;

        // Empty port → never suppressed.
        EXPECT_FALSE(m_MuxOrch->isSuppressedNeighbor(IpAddress(IN_SLICE_IP), "", &out));
        EXPECT_EQ(nullptr, out);

        // Port other than the slice cable's own port → not suppressed.
        EXPECT_FALSE(m_MuxOrch->isSuppressedNeighbor(IpAddress(IN_SLICE_IP), OTHER_INTERFACE, &out));
        EXPECT_EQ(nullptr, out);

        // IP not in slice → not suppressed even on the cable port.
        EXPECT_FALSE(m_MuxOrch->isSuppressedNeighbor(IpAddress(OUT_OF_SLICE_IP), TEST_INTERFACE, &out));
        EXPECT_EQ(nullptr, out);

        // server_ipv6 anchor on its own port → never suppressed.
        EXPECT_FALSE(m_MuxOrch->isSuppressedNeighbor(IpAddress(SERVER_IP6), TEST_INTERFACE, &out));
        EXPECT_EQ(nullptr, out);

        // In-slice IPv6 on the slice cable's port → suppressed, returns cable.
        EXPECT_TRUE(m_MuxOrch->isSuppressedNeighbor(IpAddress(IN_SLICE_IP), TEST_INTERFACE, &out));
        EXPECT_EQ(m_MuxCable, out);
    }

    // updateNeighbor: in-slice IP with FDB on slice port → suppressed.
    // Delete clears the suppressed map.
    TEST_F(MuxSubnetSlicingTest, UpdateNeighborSuppressesInSliceOnSlicePort)
    {
        const MacAddress mac("aa:bb:cc:dd:ee:01");
        InjectFdbEntry(mac, TEST_INTERFACE);

        NeighborEntry entry(IpAddress(IN_SLICE_IP), VLAN_1000);
        gNeighOrch->m_syncdNeighbors[entry] = { mac, false, 0, false };

        NeighborUpdate up;
        up.entry = entry;
        up.mac = mac;
        up.add = true;
        m_MuxOrch->updateNeighbor(up);

        EXPECT_NE(m_MuxOrch->suppressed_neighbors_.end(),
                  m_MuxOrch->suppressed_neighbors_.find(entry));

        // Re-issuing the same update must only refresh the MAC, not double-add.
        size_t before = m_MuxOrch->suppressed_neighbors_.size();
        m_MuxOrch->updateNeighbor(up);
        EXPECT_EQ(before, m_MuxOrch->suppressed_neighbors_.size());

        // Delete must remove the suppressed entry.
        NeighborUpdate del;
        del.entry = entry;
        del.mac = mac;
        del.add = false;
        m_MuxOrch->updateNeighbor(del);
        EXPECT_EQ(m_MuxOrch->suppressed_neighbors_.end(),
                  m_MuxOrch->suppressed_neighbors_.find(entry));

        gNeighOrch->m_syncdNeighbors.erase(entry);
        RemoveFdbEntry(mac);
    }

    // updateNeighbor: a previously-suppressed neighbor whose FDB now points
    // at a non-slice port must be un-suppressed.
    TEST_F(MuxSubnetSlicingTest, UpdateNeighborUnsuppressesOnNonSlicePort)
    {
        const MacAddress mac("aa:bb:cc:dd:ee:02");
        NeighborEntry entry(IpAddress(IN_SLICE_IP2), VLAN_1000);
        gNeighOrch->m_syncdNeighbors[entry] = { mac, false, 0, false };

        // Seed the suppressed map as if it had been suppressed earlier.
        m_MuxOrch->suppressed_neighbors_[entry] = mac;

        // FDB now points at a port other than the slice cable's own port.
        InjectFdbEntry(mac, OTHER_INTERFACE);

        NeighborUpdate up;
        up.entry = entry;
        up.mac = mac;
        up.add = true;
        m_MuxOrch->updateNeighbor(up);

        EXPECT_EQ(m_MuxOrch->suppressed_neighbors_.end(),
                  m_MuxOrch->suppressed_neighbors_.find(entry));

        gNeighOrch->m_syncdNeighbors.erase(entry);
        RemoveFdbEntry(mac);
    }

    // updateFdb (a): MAC of a suppressed neighbor moves OFF the slice port
    // → that neighbor must be un-suppressed.
    TEST_F(MuxSubnetSlicingTest, UpdateFdbUnsuppressesOnMoveOffSlicePort)
    {
        const MacAddress mac("aa:bb:cc:dd:ee:03");
        NeighborEntry entry(IpAddress(IN_SLICE_IP), VLAN_1000);
        gNeighOrch->m_syncdNeighbors[entry] = { mac, false, 0, false };
        m_MuxOrch->suppressed_neighbors_[entry] = mac;

        FdbUpdate move;
        move.entry.mac = mac;
        move.entry.port_name = OTHER_INTERFACE;
        move.add = true;
        move.type = "dynamic";
        m_MuxOrch->updateFdb(move);

        EXPECT_EQ(m_MuxOrch->suppressed_neighbors_.end(),
                  m_MuxOrch->suppressed_neighbors_.find(entry));

        gNeighOrch->m_syncdNeighbors.erase(entry);
    }

    // updateFdb (b): MAC lands on the slice cable's port, and an in-slice
    // neighbor in mux_nexthop_tb_ with that MAC must be suppressed.
    TEST_F(MuxSubnetSlicingTest, UpdateFdbSuppressesOnMoveOntoSlicePort)
    {
        const MacAddress mac("aa:bb:cc:dd:ee:04");
        NeighborEntry entry(IpAddress(IN_SLICE_IP2), VLAN_1000);
        NextHopKey nh_key(IpAddress(IN_SLICE_IP2), VLAN_1000);

        gNeighOrch->m_syncdNeighbors[entry] = { mac, false, 0, false };
        // Pretend it had been programmed as a normal mux neighbor on OTHER.
        m_MuxOrch->mux_nexthop_tb_[nh_key] = OTHER_INTERFACE;
        // updateFdb's reconcile loop walks mux_nexthop_tb_ and calls
        // NeighOrch::getNeighborEntry(NextHopKey, ...), which only resolves
        // if the NH is present in m_syncdNextHops.
        gNeighOrch->m_syncdNextHops[nh_key] = { (sai_object_id_t)0x9001, 0, 0 };

        FdbUpdate move;
        move.entry.mac = mac;
        move.entry.port_name = TEST_INTERFACE;
        move.add = true;
        move.type = "dynamic";
        m_MuxOrch->updateFdb(move);

        // Must be erased from mux_nexthop_tb_ and added to suppressed map.
        EXPECT_EQ(m_MuxOrch->mux_nexthop_tb_.end(),
                  m_MuxOrch->mux_nexthop_tb_.find(nh_key));
        EXPECT_NE(m_MuxOrch->suppressed_neighbors_.end(),
                  m_MuxOrch->suppressed_neighbors_.find(entry));

        gNeighOrch->m_syncdNeighbors.erase(entry);
        gNeighOrch->m_syncdNextHops.erase(nh_key);
        m_MuxOrch->suppressed_neighbors_.erase(entry);
    }

    // updateFdb (b'): MAC lands on the slice cable's port but the matching
    // neighbor is OUT of slice → must NOT be suppressed.
    TEST_F(MuxSubnetSlicingTest, UpdateFdbLeavesOutOfSliceNeighborAlone)
    {
        const MacAddress mac("aa:bb:cc:dd:ee:05");
        NeighborEntry entry(IpAddress(OUT_OF_SLICE_IP), VLAN_1000);
        NextHopKey nh_key(IpAddress(OUT_OF_SLICE_IP), VLAN_1000);

        gNeighOrch->m_syncdNeighbors[entry] = { mac, false, 0, false };
        m_MuxOrch->mux_nexthop_tb_[nh_key] = TEST_INTERFACE;
        gNeighOrch->m_syncdNextHops[nh_key] = { (sai_object_id_t)0x9002, 0, 0 };

        FdbUpdate move;
        move.entry.mac = mac;
        move.entry.port_name = TEST_INTERFACE;
        move.add = true;
        move.type = "dynamic";
        m_MuxOrch->updateFdb(move);

        EXPECT_EQ(m_MuxOrch->suppressed_neighbors_.end(),
                  m_MuxOrch->suppressed_neighbors_.find(entry));

        gNeighOrch->m_syncdNeighbors.erase(entry);
        gNeighOrch->m_syncdNextHops.erase(nh_key);
        m_MuxOrch->mux_nexthop_tb_.erase(nh_key);
    }
}
