#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#define private public
#include "neighorch.h"
#include "muxorch.h"
#undef private
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_orch_test.h"
#include "gtest/gtest.h"
#include <string>

EXTERN_MOCK_FNS

namespace mux_orch_test
{
    DEFINE_SAI_API_MOCK(neighbor);
    DEFINE_SAI_API_MOCK(route);
    DEFINE_SAI_GENERIC_API_MOCK(acl, acl_entry);
    DEFINE_SAI_GENERIC_API_MOCK(next_hop, next_hop);
    using ::testing::_;
    using namespace std;
    using namespace mock_orch_test;

    static const string TEST_INTERFACE = "Ethernet4";

    class MuxOrchTest : public MockOrchTest
    {
    protected:
        void ApplyInitialConfigs()
        {
            Table port_table = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
            Table vlan_table = Table(m_app_db.get(), APP_VLAN_TABLE_NAME);
            Table vlan_member_table = Table(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);
            Table intf_table = Table(m_app_db.get(), APP_INTF_TABLE_NAME);
            Table decap_tunnel_table = Table(m_app_db.get(), APP_TUNNEL_DECAP_TABLE_NAME);
            Table decap_term_table = Table(m_app_db.get(), APP_TUNNEL_DECAP_TERM_TABLE_NAME);

            auto ports = ut_helper::getInitialSaiPorts();
            port_table.set(TEST_INTERFACE, ports[TEST_INTERFACE]);
            port_table.set("PortConfigDone", { { "count", to_string(1) } });
            port_table.set("PortInitDone", { {} });

            vlan_table.set(VLAN_1000, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "00:aa:bb:cc:dd:ee" } });
            vlan_member_table.set(
                VLAN_1000 + vlan_member_table.getTableNameSeparator() + TEST_INTERFACE,
                { { "tagging_mode", "untagged" } });

            intf_table.set(VLAN_1000, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
                                        { "mac_addr", "00:00:00:00:00:00" } });
            intf_table.set(
                VLAN_1000 + intf_table.getTableNameSeparator() + "192.168.0.1/21", {
                                                                                       { "scope", "global" },
                                                                                       { "family", "IPv4" },
                                                                                   });

            decap_term_table.set(
                MUX_TUNNEL + decap_term_table.getTableNameSeparator() + "2.2.2.2", { { "src_ip", "1.1.1.1" },
                                                                                     { "term_type", "P2P" } });

            decap_tunnel_table.set(MUX_TUNNEL, { { "dscp_mode", "uniform" },
                                                 { "src_ip", "1.1.1.1" },
                                                 { "ecn_mode", "copy_from_outer" },
                                                 { "encap_ecn_mode", "standard" },
                                                 { "ttl_mode", "pipe" },
                                                 { "tunnel_type", "IPINIP" } });

            gPortsOrch->addExistingData(&port_table);
            gPortsOrch->addExistingData(&vlan_table);
            gPortsOrch->addExistingData(&vlan_member_table);
            static_cast<Orch *>(gPortsOrch)->doTask();

            gIntfsOrch->addExistingData(&intf_table);
            static_cast<Orch *>(gIntfsOrch)->doTask();

            m_TunnelDecapOrch->addExistingData(&decap_tunnel_table);
            m_TunnelDecapOrch->addExistingData(&decap_term_table);
            static_cast<Orch *>(m_TunnelDecapOrch)->doTask();
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

    TEST_F(MuxOrchTest, TryInitMuxCable)
    {
        Table peer_switch_table = Table(m_config_db.get(), CFG_PEER_SWITCH_TABLE_NAME);
        Table mux_cable_table = Table(m_config_db.get(), CFG_MUX_CABLE_TABLE_NAME);
        Table neigh_table = Table(m_app_db.get(), APP_NEIGH_TABLE_NAME);

        neigh_table.set(
            VLAN_1000 + neigh_table.getTableNameSeparator() + SERVER_IP1,
            { { "neigh", "62:f9:65:10:2f:04" },
              { "family", "IPv4" } });
        peer_switch_table.set(PEER_SWITCH_HOSTNAME, { { "address_ipv4", PEER_IPV4_ADDRESS } });

        mux_cable_table.set(TEST_INTERFACE, { { "server_ipv4", SERVER_IP1 + "/32" },
                                              { "server_ipv6", "a::a/128" },
                                              { "state", "auto" } });

        m_MuxOrch->addExistingData(&peer_switch_table);
        m_MuxOrch->addExistingData(&mux_cable_table);
        m_MuxOrch->tryInitMuxCable();

        EXPECT_CALL(*mock_sai_route_api, create_route_entry(_, _, _));
        gNeighOrch->addExistingData(&neigh_table);
        static_cast<Orch *>(gNeighOrch)->doTask();

        m_MuxCable = m_MuxOrch->getMuxCable(TEST_INTERFACE);
        EXPECT_EQ(STANDBY_STATE, m_MuxCable->getState());
    }
}
