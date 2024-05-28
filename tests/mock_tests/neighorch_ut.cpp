#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
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
    static const NeighborEntry VLAN1000_NEIGH = NeighborEntry(TEST_IP, VLAN_1000); 
    static const NeighborEntry VLAN2000_NEIGH = NeighborEntry(TEST_IP, VLAN_2000);

    class NeighOrchTest: public MockOrchTest
    {
    protected:
        void SetAndAssertMuxState(std::string interface, std::string state)
        {
            MuxCable* muxCable = m_MuxOrch->getMuxCable(interface);
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
            Table peer_switch_table = Table(m_config_db.get(), CFG_PEER_SWITCH_TABLE_NAME);
            Table decap_tunnel_table = Table(m_app_db.get(), APP_TUNNEL_DECAP_TABLE_NAME);
            Table decap_term_table = Table(m_app_db.get(), APP_TUNNEL_DECAP_TERM_TABLE_NAME);
            Table mux_cable_table = Table(m_config_db.get(), CFG_MUX_CABLE_TABLE_NAME);
            Table port_table = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
            Table vlan_table = Table(m_app_db.get(), APP_VLAN_TABLE_NAME);
            Table vlan_member_table = Table(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);
            Table neigh_table = Table(m_app_db.get(), APP_NEIGH_TABLE_NAME);
            Table intf_table = Table(m_app_db.get(), APP_INTF_TABLE_NAME);
            Table fdb_table = Table(m_app_db.get(), APP_FDB_TABLE_NAME);

            auto ports = ut_helper::getInitialSaiPorts();
            port_table.set(ACTIVE_INTERFACE, ports[ACTIVE_INTERFACE]);
            port_table.set(STANDBY_INTERFACE, ports[STANDBY_INTERFACE]);
            port_table.set("PortConfigDone", { { "count", to_string(1) } });
            port_table.set("PortInitDone", { {} });

            vlan_table.set(VLAN_1000, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "00:aa:bb:cc:dd:ee" } });
            vlan_table.set(VLAN_2000, { { "admin_status", "up"},
                                        { "mtu", "9100" },
                                        { "mac", "aa:11:bb:22:cc:33" } });
            vlan_member_table.set(
                VLAN_1000 + vlan_member_table.getTableNameSeparator() + ACTIVE_INTERFACE,
                { { "tagging_mode", "untagged" } });

            vlan_member_table.set(
                VLAN_2000 + vlan_member_table.getTableNameSeparator() + STANDBY_INTERFACE,
                { { "tagging_mode", "untagged" } });

            intf_table.set(VLAN_1000, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
                                        { "mac_addr", "00:00:00:00:00:00" } });

            intf_table.set(VLAN_2000, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
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

            mux_cable_table.set(ACTIVE_INTERFACE, { { "server_ipv4", SERVER_IP1 + "/32" },
                                                  { "server_ipv6", "a::a/128" },
                                                  { "state", "auto" } });

            mux_cable_table.set(STANDBY_INTERFACE, { { "server_ipv4", SERVER_IP2+ "/32" },
                                                  { "server_ipv6", "a::b/128" },
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

            fdb_table.set(
                VLAN_1000 + fdb_table.getTableNameSeparator() + MAC1,
                { { "type", "dynamic" },
                  { "port", ACTIVE_INTERFACE } });

            fdb_table.set(
                VLAN_2000 + fdb_table.getTableNameSeparator() + MAC2,
                { { "type", "dynamic" },
                  { "port", STANDBY_INTERFACE} });

            fdb_table.set(
                VLAN_1000 + fdb_table.getTableNameSeparator() + MAC3,
                { { "type", "dynamic" },
                  { "port", ACTIVE_INTERFACE} });

            gFdbOrch->addExistingData(&fdb_table);
            static_cast<Orch *>(gFdbOrch)->doTask();

            SetAndAssertMuxState(ACTIVE_INTERFACE, ACTIVE_STATE);
            SetAndAssertMuxState(STANDBY_INTERFACE, STANDBY_STATE);
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

    TEST_F(NeighOrchTest, MultiVlanIpLearning)
    {
        
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, TEST_IP, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 1);

        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry);
        LearnNeighbor(VLAN_2000, TEST_IP, MAC2);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN1000_NEIGH), 0);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(VLAN2000_NEIGH), 1);

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
}
