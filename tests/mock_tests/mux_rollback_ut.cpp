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

EXTERN_MOCK_FNS
DEFINE_SAI_API_MOCK(neighbor);
DEFINE_SAI_API_MOCK(route);
DEFINE_SAI_GENERIC_API_MOCK(acl, acl_entry);
DEFINE_SAI_GENERIC_API_MOCK(next_hop, next_hop);

namespace mux_rollback_test
{
    using namespace std;
    using ::testing::Return;
    using ::testing::Throw;

    static const string PEER_SWITCH_HOSTNAME = "peer_hostname";
    static const string PEER_IPV4_ADDRESS = "1.1.1.1";
    static const string TEST_INTERFACE = "Ethernet4";
    static const string ACTIVE = "active";
    static const string STANDBY = "standby";
    static const string STATE = "state";
    static const string VLAN_NAME = "Vlan1000";
    static const string SERVER_IP = "192.168.0.2";

    class MuxRollbackTest : public mock_orch_test::MockOrchTest
    {
    protected:
        void SetMuxStateFromAppDb(std::string state)
        {
            Table mux_cable_table = Table(m_app_db.get(), APP_MUX_CABLE_TABLE_NAME);
            mux_cable_table.set(TEST_INTERFACE, { { STATE, state } });
            m_MuxCableOrch->addExistingData(&mux_cable_table);
            static_cast<Orch *>(m_MuxCableOrch)->doTask();
        }

        void SetAndAssertMuxState(std::string state)
        {
            m_MuxCable->setState(state);
            EXPECT_EQ(state, m_MuxCable->getState());
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
            port_table.set(TEST_INTERFACE, ports[TEST_INTERFACE]);
            port_table.set("PortConfigDone", { { "count", to_string(1) } });
            port_table.set("PortInitDone", { {} });

            neigh_table.set(
                VLAN_NAME + neigh_table.getTableNameSeparator() + SERVER_IP, { { "neigh", "62:f9:65:10:2f:04" },
                                                                               { "family", "IPv4" } });

            vlan_table.set(VLAN_NAME, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "00:aa:bb:cc:dd:ee" } });
            vlan_member_table.set(
                VLAN_NAME + vlan_member_table.getTableNameSeparator() + TEST_INTERFACE,
                { { "tagging_mode", "untagged" } });

            intf_table.set(VLAN_NAME, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
                                        { "mac_addr", "00:00:00:00:00:00" } });
            intf_table.set(
                VLAN_NAME + neigh_table.getTableNameSeparator() + "192.168.0.1/21", {
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

            mux_cable_table.set(TEST_INTERFACE, { { "server_ipv4", SERVER_IP + "/32" },
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

            m_MuxCable = m_MuxOrch->getMuxCable(TEST_INTERFACE);

            // We always expect the mux to be initialized to standby
            EXPECT_EQ(STANDBY, m_MuxCable->getState());
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

    TEST_F(MuxRollbackTest, StandbyToActiveNeighborAlreadyExists)
    {
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry)
            .WillOnce(Return(SAI_STATUS_ITEM_ALREADY_EXISTS));
        SetAndAssertMuxState(ACTIVE);
    }

    TEST_F(MuxRollbackTest, ActiveToStandbyNeighborNotFound)
    {
        SetAndAssertMuxState(ACTIVE);
        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry)
            .WillOnce(Return(SAI_STATUS_ITEM_NOT_FOUND));
        SetAndAssertMuxState(STANDBY);
    }

    TEST_F(MuxRollbackTest, StandbyToActiveRouteNotFound)
    {
        EXPECT_CALL(*mock_sai_route_api, remove_route_entry)
            .WillOnce(Return(SAI_STATUS_ITEM_NOT_FOUND));
        SetAndAssertMuxState(ACTIVE);
    }

    TEST_F(MuxRollbackTest, ActiveToStandbyRouteAlreadyExists)
    {
        SetAndAssertMuxState(ACTIVE);
        EXPECT_CALL(*mock_sai_route_api, create_route_entry)
            .WillOnce(Return(SAI_STATUS_ITEM_ALREADY_EXISTS));
        SetAndAssertMuxState(STANDBY);
    }

    TEST_F(MuxRollbackTest, StandbyToActiveAclNotFound)
    {
        EXPECT_CALL(*mock_sai_acl_api, remove_acl_entry)
            .WillOnce(Return(SAI_STATUS_ITEM_NOT_FOUND));
        SetAndAssertMuxState(ACTIVE);
    }

    TEST_F(MuxRollbackTest, ActiveToStandbyAclAlreadyExists)
    {
        SetAndAssertMuxState(ACTIVE);
        EXPECT_CALL(*mock_sai_acl_api, create_acl_entry)
            .WillOnce(Return(SAI_STATUS_ITEM_ALREADY_EXISTS));
        SetAndAssertMuxState(STANDBY);
    }

    TEST_F(MuxRollbackTest, StandbyToActiveNextHopAlreadyExists)
    {
        EXPECT_CALL(*mock_sai_next_hop_api, create_next_hop)
            .WillOnce(Return(SAI_STATUS_ITEM_ALREADY_EXISTS));
        SetAndAssertMuxState(ACTIVE);
    }

    TEST_F(MuxRollbackTest, ActiveToStandbyNextHopNotFound)
    {
        SetAndAssertMuxState(ACTIVE);
        EXPECT_CALL(*mock_sai_next_hop_api, remove_next_hop)
            .WillOnce(Return(SAI_STATUS_ITEM_NOT_FOUND));
        SetAndAssertMuxState(STANDBY);
    }

    TEST_F(MuxRollbackTest, StandbyToActiveRuntimeErrorRollbackToStandby)
    {
        EXPECT_CALL(*mock_sai_route_api, remove_route_entry)
            .WillOnce(Throw(runtime_error("Mock runtime error")));
        SetMuxStateFromAppDb(ACTIVE);
        EXPECT_EQ(STANDBY, m_MuxCable->getState());
    }

    TEST_F(MuxRollbackTest, ActiveToStandbyRuntimeErrorRollbackToActive)
    {
        SetAndAssertMuxState(ACTIVE);
        EXPECT_CALL(*mock_sai_route_api, create_route_entry)
            .WillOnce(Throw(runtime_error("Mock runtime error")));
        SetMuxStateFromAppDb(STANDBY);
        EXPECT_EQ(ACTIVE, m_MuxCable->getState());
    }

    TEST_F(MuxRollbackTest, StandbyToActiveLogicErrorRollbackToStandby)
    {
        EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entry)
            .WillOnce(Throw(logic_error("Mock logic error")));
        SetMuxStateFromAppDb(ACTIVE);
        EXPECT_EQ(STANDBY, m_MuxCable->getState());
    }

    TEST_F(MuxRollbackTest, ActiveToStandbyLogicErrorRollbackToActive)
    {
        SetAndAssertMuxState(ACTIVE);
        EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entry)
            .WillOnce(Throw(logic_error("Mock logic error")));
        SetMuxStateFromAppDb(STANDBY);
        EXPECT_EQ(ACTIVE, m_MuxCable->getState());
    }

    TEST_F(MuxRollbackTest, StandbyToActiveExceptionRollbackToStandby)
    {
        EXPECT_CALL(*mock_sai_next_hop_api, create_next_hop)
            .WillOnce(Throw(exception()));
        SetMuxStateFromAppDb(ACTIVE);
        EXPECT_EQ(STANDBY, m_MuxCable->getState());
    }

    TEST_F(MuxRollbackTest, ActiveToStandbyExceptionRollbackToActive)
    {
        SetAndAssertMuxState(ACTIVE);
        EXPECT_CALL(*mock_sai_next_hop_api, remove_next_hop)
            .WillOnce(Throw(exception()));
        SetMuxStateFromAppDb(STANDBY);
        EXPECT_EQ(ACTIVE, m_MuxCable->getState());
    }
}
