#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#define private public
#include "routeorch.h"
#undef private
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_orch_test.h"

EXTERN_MOCK_FNS

namespace neighorch_test
{
    DEFINE_SAI_API_MOCK(neighbor);
    using namespace std;
    using namespace mock_orch_test;
    using ::testing::Return;
    using ::testing::Throw;

    static const string TEST_IP = "10.10.10.10";
    static const string VRF_3000 = "Vrf3000";
    static const NeighborEntry VLAN1000_NEIGH = NeighborEntry(TEST_IP, VLAN_1000);
    static const NeighborEntry VLAN2000_NEIGH = NeighborEntry(TEST_IP, VLAN_2000);
    static const NeighborEntry VLAN3000_NEIGH = NeighborEntry(TEST_IP, VLAN_3000);
    static const NeighborEntry VLAN4000_NEIGH = NeighborEntry(TEST_IP, VLAN_4000);

    class NeighOrchTest : public MockOrchTest
    {
    protected:
        void SetAndAssertMuxState(std::string interface, std::string state)
        {
            MuxCable *muxCable = m_MuxOrch->getMuxCable(interface);
            muxCable->setState(state);
            EXPECT_EQ(state, muxCable->getState());
        }

        void LearnNeighbor(std::string vlan, std::string ip, std::string mac)
        {
            Table neigh_table = Table(m_app_db.get(), APP_NEIGH_TABLE_NAME);
            string key = vlan + neigh_table.getTableNameSeparator() + ip;
            neigh_table.set(key, { { "neigh", mac }, { "family", "IPv4" } });
            gNeighOrch->addExistingData(&neigh_table);
            static_cast<Orch *>(gNeighOrch)->doTask();
            neigh_table.del(key);
        }

        void ApplyInitialConfigs()
        {
            Table port_table = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
            Table vlan_table = Table(m_app_db.get(), APP_VLAN_TABLE_NAME);
            Table vlan_member_table = Table(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);
            Table neigh_table = Table(m_app_db.get(), APP_NEIGH_TABLE_NAME);
            Table intf_table = Table(m_app_db.get(), APP_INTF_TABLE_NAME);
            Table fdb_table = Table(m_app_db.get(), APP_FDB_TABLE_NAME);
            Table vrf_table = Table(m_app_db.get(), APP_VRF_TABLE_NAME);

            auto ports = ut_helper::getInitialSaiPorts();
            port_table.set(ETHERNET0, ports[ETHERNET0]);
            port_table.set(ETHERNET4, ports[ETHERNET4]);
            port_table.set(ETHERNET8, ports[ETHERNET8]);
            port_table.set("PortConfigDone", { { "count", to_string(1) } });
            port_table.set("PortInitDone", { {} });

            vrf_table.set(VRF_3000, { {"NULL", "NULL"} });

            vlan_table.set(VLAN_1000, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "00:aa:bb:cc:dd:ee" } });
            vlan_table.set(VLAN_2000, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "aa:11:bb:22:cc:33" } });
            vlan_table.set(VLAN_3000, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "99:ff:88:ee:77:dd" } });
            vlan_table.set(VLAN_4000, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "99:ff:88:ee:77:dd" } });
            vlan_member_table.set(
                VLAN_1000 + vlan_member_table.getTableNameSeparator() + ETHERNET0,
                { { "tagging_mode", "untagged" } });

            vlan_member_table.set(
                VLAN_2000 + vlan_member_table.getTableNameSeparator() + ETHERNET4,
                { { "tagging_mode", "untagged" } });

            vlan_member_table.set(
                VLAN_3000 + vlan_member_table.getTableNameSeparator() + ETHERNET8,
                { { "tagging_mode", "untagged" } });

            vlan_member_table.set(
                VLAN_4000 + vlan_member_table.getTableNameSeparator() + ETHERNET12,
                { { "tagging_mode", "untagged" } });

            intf_table.set(VLAN_1000, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
                                        { "mac_addr", "00:00:00:00:00:00" } });

            intf_table.set(VLAN_2000, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
                                        { "mac_addr", "00:00:00:00:00:00" } });

            intf_table.set(VLAN_3000, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
                                        { "vrf_name", VRF_3000 },
                                        { "mac_addr", "00:00:00:00:00:00" } });

            intf_table.set(VLAN_4000, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
                                        { "vrf_name", VRF_3000 },
                                        { "mac_addr", "00:00:00:00:00:00" } });

            intf_table.set(
                VLAN_1000 + neigh_table.getTableNameSeparator() + "192.168.0.1/24", {
                                                                                        { "scope", "global" },
                                                                                        { "family", "IPv4" },
                                                                                    });

            intf_table.set(
                VLAN_2000 + neigh_table.getTableNameSeparator() + "192.168.2.1/24", {
                                                                                        { "scope", "global" },
                                                                                        { "family", "IPv4" },
                                                                                    });
            intf_table.set(
                VLAN_3000 + neigh_table.getTableNameSeparator() + "192.168.3.1/24", {
                                                                                        { "scope", "global" },
                                                                                        { "family", "IPv4" },
                                                                                    });

            intf_table.set(
                VLAN_4000 + neigh_table.getTableNameSeparator() + "192.168.3.1/24", {
                                                                                        { "scope", "global" },
                                                                                        { "family", "IPv4" },
                                                                                    });

            gPortsOrch->addExistingData(&port_table);
            gPortsOrch->addExistingData(&vlan_table);
            gPortsOrch->addExistingData(&vlan_member_table);
            static_cast<Orch *>(gPortsOrch)->doTask();

            gVrfOrch->addExistingData(&vrf_table);
            static_cast<Orch *>(gVrfOrch)->doTask();

            gIntfsOrch->addExistingData(&intf_table);
            static_cast<Orch *>(gIntfsOrch)->doTask();

            fdb_table.set(
                VLAN_1000 + fdb_table.getTableNameSeparator() + MAC1,
                { { "type", "dynamic" },
                  { "port", ETHERNET0 } });

            fdb_table.set(
                VLAN_2000 + fdb_table.getTableNameSeparator() + MAC2,
                { { "type", "dynamic" },
                  { "port", ETHERNET4 } });

            fdb_table.set(
                VLAN_1000 + fdb_table.getTableNameSeparator() + MAC3,
                { { "type", "dynamic" },
                  { "port", ETHERNET0 } });

            fdb_table.set(
                VLAN_3000 + fdb_table.getTableNameSeparator() + MAC4,
                { { "type", "dynamic" },
                  { "port", ETHERNET8 } });

            fdb_table.set(
                VLAN_4000 + fdb_table.getTableNameSeparator() + MAC5,
                { { "type", "dynamic" },
                  { "port", ETHERNET12 } });

            gFdbOrch->addExistingData(&fdb_table);
            static_cast<Orch *>(gFdbOrch)->doTask();
        }

        void PostSetUp() override
        {
            INIT_SAI_API_MOCK(neighbor);
            MockSaiApis();
        }

        void PreTearDown() override
        {
            RestoreSaiApis();
        }
    };

    TEST_F(NeighOrchTest, MultiVlanDuplicateNeighbor)
    {
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry);
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_2000, TEST_IP, MAC2);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 0);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN2000_NEIGH), 1);

        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry);
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC3);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN2000_NEIGH), 0);
    }

    TEST_F(NeighOrchTest, MultiVlanUnableToRemoveNeighbor)
    {
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);
        NextHopKey nexthop = { TEST_IP, VLAN_1000 };
        gNeighOrch->m_syncdNextHops[nexthop].ref_count = 1;

        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry).Times(0);
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry).Times(0);
        LearnNeighbor(VLAN_2000, TEST_IP, MAC2);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN2000_NEIGH), 0);
    }

    TEST_F(NeighOrchTest, MultiVlanDifferentVrfDuplicateNeighbor)
    {
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry).Times(0);
        LearnNeighbor(VLAN_3000, TEST_IP, MAC4);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN3000_NEIGH), 1);
    }

    TEST_F(NeighOrchTest, MultiVlanSameVrfDuplicateNeighbor)
    {
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_3000, TEST_IP, MAC4);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN3000_NEIGH), 1);

        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry);
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_4000, TEST_IP, MAC5);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN3000_NEIGH), 0);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN4000_NEIGH), 1);
    }

    TEST_F(NeighOrchTest, MultiVlanDuplicateNeighborMissingExistingVlanPort)
    {
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);

        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry).Times(0);
        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry).Times(0);
        gPortsOrch->m_portList.erase(VLAN_1000);
        LearnNeighbor(VLAN_2000, TEST_IP, MAC2);
    }

    TEST_F(NeighOrchTest, MultiVlanDuplicateNeighborMissingNewVlanPort)
    {
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);

        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry).Times(0);
        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry).Times(0);
        gPortsOrch->m_portList.erase(VLAN_2000);
        LearnNeighbor(VLAN_2000, TEST_IP, MAC2);
    }

    TEST_F(NeighOrchTest, ProcessFDBAdd_EnableNeighbor)
    {
        // Setup: Learn a neighbor first
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        // Disable the neighbor to simulate it being disabled
        EXPECT_TRUE(gNeighOrch->disableNeighbor(VLAN1000_NEIGH));
        EXPECT_FALSE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));

        // Create FDB entry to trigger processFDBAdd
        Port vlan_port;
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_1000, vlan_port));

        FdbEntry fdb_entry;
        fdb_entry.mac = MacAddress(MAC1);
        fdb_entry.bv_id = vlan_port.m_vlan_info.vlan_oid;
        fdb_entry.port_name = ETHERNET0;

        // Test processFDBAdd - should re-enable the neighbor
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        gNeighOrch->processFDBAdd(fdb_entry);

        // Verify neighbor is enabled
        EXPECT_TRUE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));
    }

    TEST_F(NeighOrchTest, ProcessFDBAdd_InvalidVlanId)
    {
        // Setup: Learn a neighbor first
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        // Create FDB entry with invalid VLAN ID
        FdbEntry fdb_entry;
        fdb_entry.mac = MacAddress(MAC1);
        fdb_entry.bv_id = 0x999999; // Invalid VLAN ID
        fdb_entry.port_name = ETHERNET0;

        // Test processFDBAdd with invalid VLAN - should not crash or affect neighbors
        gNeighOrch->processFDBAdd(fdb_entry);

        // Verify neighbor state unchanged
        EXPECT_TRUE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));
    }

    TEST_F(NeighOrchTest, ProcessFDBDelete_DisableNeighbor)
    {
        // Setup: Learn a neighbor first
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);
        EXPECT_TRUE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));

        // Create FDB entry to trigger processFDBDelete
        Port vlan_port;
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_1000, vlan_port));

        FdbEntry fdb_entry;
        fdb_entry.mac = MacAddress(MAC1);
        fdb_entry.bv_id = vlan_port.m_vlan_info.vlan_oid;
        fdb_entry.port_name = ETHERNET0;

        // Test processFDBDelete - should disable the neighbor
        gNeighOrch->processFDBDelete(fdb_entry);

        // Verify neighbor is disabled but still in cache
        EXPECT_FALSE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);
    }

    TEST_F(NeighOrchTest, ProcessFDBDelete_NoMatchingNeighbor)
    {
        // Setup: Learn a neighbor with MAC1
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        // Create FDB entry with different MAC
        Port vlan_port;
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_1000, vlan_port));

        FdbEntry fdb_entry;
        fdb_entry.mac = MacAddress(MAC2); // Different MAC
        fdb_entry.bv_id = vlan_port.m_vlan_info.vlan_oid;
        fdb_entry.port_name = ETHERNET0;

        // Test processFDBDelete with non-matching MAC - should not affect neighbor
        gNeighOrch->processFDBDelete(fdb_entry);

        // Verify neighbor state unchanged
        EXPECT_TRUE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));
    }

    TEST_F(NeighOrchTest, ProcessFDBResolve_TriggerArpResolution)
    {
        // Setup: Learn a neighbor first
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        // Create FDB entry to trigger processFDBResolve
        Port vlan_port;
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_1000, vlan_port));

        FdbEntry fdb_entry;
        fdb_entry.mac = MacAddress(MAC1);
        fdb_entry.bv_id = vlan_port.m_vlan_info.vlan_oid;
        fdb_entry.port_name = ETHERNET0;

        // Test processFDBResolve - should trigger ARP resolution
        gNeighOrch->processFDBResolve(fdb_entry);

        // Verify neighbor entry is still present (ARP resolve doesn't remove it)
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);
    }

    TEST_F(NeighOrchTest, ProcessFDBResolve_InvalidVlanId)
    {
        // Setup: Learn a neighbor first
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        // Create FDB entry with invalid VLAN ID
        FdbEntry fdb_entry;
        fdb_entry.mac = MacAddress(MAC1);
        fdb_entry.bv_id = 0x888888; // Invalid VLAN ID
        fdb_entry.port_name = ETHERNET0;

        // Test processFDBResolve with invalid VLAN - should not crash
        gNeighOrch->processFDBResolve(fdb_entry);

        // Verify neighbor state unchanged
        EXPECT_TRUE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));
    }

    TEST_F(NeighOrchTest, ProcessFDBFunctions_MultipleNeighborsOnSameVlan)
    {
        const string TEST_IP2 = "10.10.10.11";
        const NeighborEntry VLAN1000_NEIGH2 = NeighborEntry(TEST_IP2, VLAN_1000);

        // Setup: Learn two neighbors on the same VLAN
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry).Times(2);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        LearnNeighbor(VLAN_1000, TEST_IP2, MAC3);

        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH2), 1);

        // Create FDB entry for first neighbor's MAC
        Port vlan_port;
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_1000, vlan_port));

        FdbEntry fdb_entry;
        fdb_entry.mac = MacAddress(MAC1);
        fdb_entry.bv_id = vlan_port.m_vlan_info.vlan_oid;
        fdb_entry.port_name = ETHERNET0;

        // Test processFDBDelete - should only affect the matching neighbor
        gNeighOrch->processFDBDelete(fdb_entry);

        // Verify only first neighbor is disabled, second remains enabled
        EXPECT_FALSE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));
        EXPECT_TRUE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH2));
    }

    TEST_F(NeighOrchTest, ProcessFDBFunctions_DifferentVlansSameMac)
    {
        // Setup: Learn neighbors on different VLANs with different MACs
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_2000, TEST_IP, MAC2); // Different MAC, different VLAN
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN2000_NEIGH), 1);

        // Create FDB entry for VLAN_1000
        Port vlan_port;
        ASSERT_TRUE(gPortsOrch->getPort(VLAN_1000, vlan_port));

        FdbEntry fdb_entry;
        fdb_entry.mac = MacAddress(MAC1);
        fdb_entry.bv_id = vlan_port.m_vlan_info.vlan_oid;
        fdb_entry.port_name = ETHERNET0;

        // Test processFDBDelete - should only affect VLAN_1000 neighbor
        gNeighOrch->processFDBDelete(fdb_entry);

        // Verify only VLAN_1000 neighbor is disabled, VLAN_2000 remains enabled
        EXPECT_FALSE(gNeighOrch->isHwConfigured(VLAN1000_NEIGH));
        EXPECT_TRUE(gNeighOrch->isHwConfigured(VLAN2000_NEIGH));
    }
}
