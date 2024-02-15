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


extern std::set<void (*)()> apply_mock_fns;
extern std::set<void (*)()> remove_mock_fns;

namespace neighorch_test
{
    DEFINE_SAI_API_MOCK(neighbor);
    using namespace std;
    using ::testing::Return;
    using ::testing::Throw;

    static const string PEER_SWITCH_HOSTNAME = "peer_hostname";
    static const string PEER_IPV4_ADDRESS = "1.1.1.1";
    static const string ACTIVE_INTERFACE = "Ethernet4";
    static const string STANDBY_INTERFACE = "Ethernet8";
    static const string ACTIVE = "active";
    static const string STANDBY = "standby";
    static const string STATE = "state";
    static const string VLAN_1000 = "Vlan1000";
    static const string VLAN_2000 = "Vlan2000";
    static const string SERVER_IP1 = "192.168.0.2";
    static const string SERVER_IP2 = "192.168.0.3";
    static const string MAC1 = "62-f9-65-10-2f-01";
    static const string MAC2 = "62-f9-65-10-2f-02";
    static const string MAC3 = "62-f9-65-10-2f-03";

    class NeighOrchTest: public mock_orch_test::MockOrchTest
    {
    protected:
        void SetMuxStateFromAppDb(std::string interface, std::string state)
        {
            Table mux_cable_table = Table(m_app_db.get(), APP_MUX_CABLE_TABLE_NAME);
            mux_cable_table.set(interface, { { STATE, state } });
            m_MuxCableOrch->addExistingData(&mux_cable_table);
            static_cast<Orch *>(m_MuxCableOrch)->doTask();
        }

        void SetAndAssertMuxState(std::string interface, std::string state)
        {
            MuxCable* muxCable = m_MuxOrch->getMuxCable(interface);
            muxCable->setState(state);
            EXPECT_EQ(state, muxCable->getState());
        }

        void LearnNeighbor(std::string vlan, std::string ip, std::string mac)
        {
            Table neigh_table = Table(m_app_db.get(), APP_NEIGH_TABLE_NAME);
            neigh_table.set(
                vlan + neigh_table.getTableNameSeparator() + ip, { { "neigh", mac },
                                                                   { "family", "IPv4" } });
            gNeighOrch->addExistingData(&neigh_table);
            static_cast<Orch *>(gNeighOrch)->doTask();
        }

        void ApplyInitialConfigs()
        {
            Table peer_switch_table = Table(m_config_db.get(), CFG_PEER_SWITCH_TABLE_NAME);
            Table tunnel_table = Table(m_app_db.get(), APP_TUNNEL_DECAP_TABLE_NAME);
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
            tunnel_table.set(MUX_TUNNEL, { { "dscp_mode", "uniform" },
                                           { "dst_ip", "2.2.2.2" },
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

            m_TunnelDecapOrch->addExistingData(&tunnel_table);
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

            SetAndAssertMuxState(ACTIVE_INTERFACE, ACTIVE);
            SetAndAssertMuxState(STANDBY_INTERFACE, STANDBY);
        }

        void SetUp() override
        {
            mock_orch_test::MockOrchTest::SetUp();
            INIT_SAI_API_MOCK(neighbor);
            MockSaiApis();
        }

        void TearDown() override
        {
            mock_orch_test::MockOrchTest::TearDown();  
            RestoreSaiApis();
        }
    };

    TEST_F(NeighOrchTest, MultiVlanIpLearning)
    {
        std::string neigh_ip = "10.10.10.10";
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, neigh_ip, MAC1);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(NeighborEntry(IpAddress(neigh_ip), VLAN_1000)), 1);

        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry);
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_2000, neigh_ip, MAC2);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(NeighborEntry(IpAddress(neigh_ip), VLAN_1000)), 0);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(NeighborEntry(IpAddress(neigh_ip), VLAN_2000)), 1);

        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry);
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry);
        LearnNeighbor(VLAN_1000, neigh_ip, MAC3);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(NeighborEntry(IpAddress(neigh_ip), VLAN_2000)), 0);
        ASSERT_EQ(gNeighOrch->m_syncdNeighbors.count(NeighborEntry(IpAddress(neigh_ip), VLAN_1000)), 1);
    }
}
