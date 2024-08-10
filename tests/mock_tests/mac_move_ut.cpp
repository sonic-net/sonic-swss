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
#include "gtest/gtest.h"
#include <string>
#include <unistd.h>

EXTERN_MOCK_FNS

namespace mac_move_test
{
    DEFINE_SAI_API_MOCK(neighbor);
    DEFINE_SAI_API_MOCK(route);
    DEFINE_SAI_GENERIC_API_MOCK(acl, acl_entry);
    DEFINE_SAI_GENERIC_API_MOCK(next_hop, next_hop);
    using namespace std;
    using namespace mock_orch_test;
    using ::testing::Return;
    using ::testing::Throw;

    // static const string ACTIVE_INTERFACE = "Ethernet4";
    // static const string STANDBY_INTERFACE = "Ethernet8";
    static const string NEIGH_IP = "192.168.0.100";

    class MacMoveTest : public MockOrchTest
    {
    protected:
        MuxCable* m_ActiveMuxCable;
        MuxCable* m_StandbyMuxCable;

        void SetMuxStateFromAppDb(std::string interface, std::string state)
        {
            Table mux_cable_table = Table(m_app_db.get(), APP_MUX_CABLE_TABLE_NAME);
            mux_cable_table.set(interface, { { STATE, state } });
            m_MuxCableOrch->addExistingData(&mux_cable_table);
            static_cast<Orch *>(m_MuxCableOrch)->doTask();
        }

        void SetAndAssertMuxState(MuxCable* cable, std::string state)
        {
            cable->setState(state);
            EXPECT_EQ(state, cable->getState());
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

            auto ports = ut_helper::getInitialSaiPorts();
            port_table.set(ACTIVE_INTERFACE, ports[ACTIVE_INTERFACE]);
            port_table.set(STANDBY_INTERFACE, ports[STANDBY_INTERFACE]);
            port_table.set("PortConfigDone", { { "count", to_string(1) } });
            port_table.set("PortInitDone", { {} });

            neigh_table.set(
                VLAN_1000 + neigh_table.getTableNameSeparator() + SERVER_IP1, { { "neigh", MAC1 },
                                                                               { "family", "IPv4" } });

            vlan_table.set(VLAN_1000, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "00:aa:bb:cc:dd:ee" } });
            vlan_member_table.set(
                VLAN_1000 + vlan_member_table.getTableNameSeparator() + ACTIVE_INTERFACE,
                { { "tagging_mode", "untagged" } });

            vlan_member_table.set(
                VLAN_1000 + vlan_member_table.getTableNameSeparator() + STANDBY_INTERFACE,
                { { "tagging_mode", "untagged" } });

            intf_table.set(VLAN_1000, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
                                        { "mac_addr", "00:00:00:00:00:00" } });
            intf_table.set(
                VLAN_1000 + neigh_table.getTableNameSeparator() + "192.168.0.1/21", {
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

            mux_cable_table.set(STANDBY_INTERFACE, { { "server_ipv4", SERVER_IP2 + "/32" },
                                                  { "server_ipv6", "a::a/128" },
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

            gNeighOrch->addExistingData(&neigh_table);
            static_cast<Orch *>(gNeighOrch)->doTask();

            m_ActiveMuxCable = m_MuxOrch->getMuxCable(ACTIVE_INTERFACE);
            m_StandbyMuxCable = m_MuxOrch->getMuxCable(STANDBY_INTERFACE);

            // We always expect the mux to be initialized to standby
            SetAndAssertMuxState(m_ActiveMuxCable, ACTIVE_STATE);
            SetAndAssertMuxState(m_StandbyMuxCable, STANDBY_STATE);
        }

        void PostSetUp() override
        {
            INIT_SAI_API_MOCK(neighbor);
            INIT_SAI_API_MOCK(route);
            INIT_SAI_API_MOCK(acl);
            INIT_SAI_API_MOCK(next_hop);
            MockSaiApis();
        }

        void PreTearDown() override
        {
            RestoreSaiApis();
        }
    };

    

    TEST_F(MacMoveTest, TestTestTest)
    {
        FdbEntry activeEntry = {MAC1, 0, ACTIVE_INTERFACE};
        FdbEntry standbyEntry = {MAC1, 0, STANDBY_INTERFACE};
        Port activePort, standbyPort;
        gPortsOrch->getPort(ACTIVE_INTERFACE, activePort);
        gPortsOrch->getPort(STANDBY_INTERFACE, standbyPort);
        FdbUpdate activeUpdate = {activeEntry, activePort, "", true, SAI_FDB_ENTRY_TYPE_DYNAMIC};
        FdbUpdate standbyUpdate = {standbyEntry, standbyPort, "", true, SAI_FDB_ENTRY_TYPE_DYNAMIC};
        sleep(5);
        for (int i = 0; i < 1000000; i++)
        {
            m_MuxOrch->update(SUBJECT_TYPE_FDB_CHANGE, static_cast<void *> (&activeUpdate));
            m_MuxOrch->update(SUBJECT_TYPE_FDB_CHANGE, static_cast<void *> (&standbyUpdate));
        }
        sleep(60);
    }
}
