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

namespace mux_rollback_test
{
    DEFINE_SAI_API_MOCK(neighbor);
    DEFINE_SAI_API_MOCK_SPECIFY_ENTRY_WITH_SET(route, route);
    DEFINE_SAI_GENERIC_API_MOCK(acl, acl_entry);
    DEFINE_SAI_GENERIC_API_OBJECT_BULK_MOCK(next_hop, next_hop);
    using ::testing::_;
    using namespace std;
    using namespace mock_orch_test;
    using ::testing::Return;
    using ::testing::Throw;
    using ::testing::DoAll;
    using ::testing::SetArrayArgument;

    static const string TEST_INTERFACE = "Ethernet4";

    sai_bulk_create_neighbor_entry_fn old_create_neighbor_entries;
    sai_bulk_remove_neighbor_entry_fn old_remove_neighbor_entries;
    sai_bulk_create_route_entry_fn old_create_route_entries;
    sai_bulk_remove_route_entry_fn old_remove_route_entries;
    sai_bulk_set_route_entry_attribute_fn old_set_route_entries_attribute;
    sai_bulk_object_create_fn old_object_create;
    sai_bulk_object_remove_fn old_object_remove;

    class MuxRollbackTest : public MockOrchTest
    {
    protected:
        std::string m_neighbor_mode = "host-route";

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

        bool IsPrefixBasedMuxNeighbor()
        {
            NextHopKey nhKey = NextHopKey(IpAddress(SERVER_IP1), VLAN_1000);
            return gNeighOrch->isPrefixNeighborNh(nhKey);
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

            auto ports = ut_helper::getInitialSaiPorts();
            port_table.set(TEST_INTERFACE, ports[TEST_INTERFACE]);
            port_table.set("PortConfigDone", { { "count", to_string(1) } });
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

            intf_table.set(VLAN_1000, { { "grat_arp", "enabled" },
                                        { "proxy_arp", "enabled" },
                                        { "mac_addr", "00:00:00:00:00:00" } });
            intf_table.set(
                VLAN_1000 + neigh_table.getTableNameSeparator() + "192.168.0.1/21", {
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

            mux_cable_table.set(TEST_INTERFACE, { { "server_ipv4", SERVER_IP1 + "/32" },
                                                  { "server_ipv6", "a::a/128" },
                                                  { "neighbor_mode", m_neighbor_mode },
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

            // We always expect the mux to be initialized to standby
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
    };

    class MuxRollbackPrefixRouteTest : public MuxRollbackTest
    {
    public:
        MuxRollbackPrefixRouteTest()
        {
            m_neighbor_mode = "prefix-route";
        }
    };

    TEST_F(MuxRollbackTest, StandbyToActiveNeighborAlreadyExists)
    {
        if (!IsPrefixBasedMuxNeighbor())
        {
            std::vector<sai_status_t> exp_status{SAI_STATUS_ITEM_ALREADY_EXISTS};
            EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entries)
                .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_ITEM_ALREADY_EXISTS)));
        }
        SetAndAssertMuxState(ACTIVE_STATE);
    }

    TEST_F(MuxRollbackTest, ActiveToStandbyNeighborNotFound)
    {
        SetAndAssertMuxState(ACTIVE_STATE);
        std::vector<sai_status_t> exp_status{SAI_STATUS_ITEM_NOT_FOUND};
        if (!IsPrefixBasedMuxNeighbor())
        {
            EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entries)
                .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_ITEM_NOT_FOUND)));
        }
        SetAndAssertMuxState(STANDBY_STATE);
    }

    TEST_F(MuxRollbackTest, StandbyToActiveRouteNotFound)
    {
        std::vector<sai_status_t> exp_status{SAI_STATUS_ITEM_NOT_FOUND};
        if (!IsPrefixBasedMuxNeighbor())
        {
            EXPECT_CALL(*mock_sai_route_api, remove_route_entries)
                .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_ITEM_NOT_FOUND)));
        }
        SetAndAssertMuxState(ACTIVE_STATE);
    }

    TEST_F(MuxRollbackPrefixRouteTest, StandbyToActiveSetRouteAttrNotFoundRollbackToStandby)
    {
        std::vector<sai_status_t> exp_status_not_found{SAI_STATUS_ITEM_NOT_FOUND};
        std::vector<sai_status_t> exp_status_success{SAI_STATUS_SUCCESS};
        EXPECT_CALL(*mock_sai_route_api, set_route_entries_attribute)
            .WillOnce(DoAll(SetArrayArgument<4>(exp_status_not_found.begin(), exp_status_not_found.end()),
                            Return(SAI_STATUS_ITEM_NOT_FOUND)))
            .WillOnce(DoAll(SetArrayArgument<4>(exp_status_success.begin(), exp_status_success.end()),
                            Return(SAI_STATUS_SUCCESS)));
        SetMuxStateFromAppDb(ACTIVE_STATE);
        EXPECT_EQ(STANDBY_STATE, m_MuxCable->getState());
    }

    TEST_F(MuxRollbackTest, ActiveToStandbyRouteAlreadyExists)
    {
        SetAndAssertMuxState(ACTIVE_STATE);
        std::vector<sai_status_t> exp_status{SAI_STATUS_ITEM_ALREADY_EXISTS};

        if (!IsPrefixBasedMuxNeighbor())
        {
            EXPECT_CALL(*mock_sai_route_api, create_route_entries)
                .WillOnce(DoAll(SetArrayArgument<5>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_ITEM_ALREADY_EXISTS)));
        }
        SetAndAssertMuxState(STANDBY_STATE);
    }

    TEST_F(MuxRollbackTest, StandbyToActiveAclNotFound)
    {
        EXPECT_CALL(*mock_sai_acl_api, remove_acl_entry)
            .WillOnce(Return(SAI_STATUS_ITEM_NOT_FOUND));
        SetAndAssertMuxState(ACTIVE_STATE);
    }

    TEST_F(MuxRollbackTest, ActiveToStandbyAclAlreadyExists)
    {
        SetAndAssertMuxState(ACTIVE_STATE);
        EXPECT_CALL(*mock_sai_acl_api, create_acl_entry)
            .WillOnce(Return(SAI_STATUS_ITEM_ALREADY_EXISTS));
        SetAndAssertMuxState(STANDBY_STATE);
    }

    TEST_F(MuxRollbackTest, StandbyToActiveNextHopAlreadyExists)
    {
        if (!IsPrefixBasedMuxNeighbor())
        {
            std::vector<sai_status_t> exp_status{SAI_STATUS_ITEM_ALREADY_EXISTS};
            EXPECT_CALL(*mock_sai_next_hop_api, create_next_hops)
                .WillOnce(DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_ITEM_ALREADY_EXISTS)));
        }
        SetAndAssertMuxState(ACTIVE_STATE);
    }

    TEST_F(MuxRollbackTest, ActiveToStandbyNextHopNotFound)
    {
        SetAndAssertMuxState(ACTIVE_STATE);
        if (!IsPrefixBasedMuxNeighbor())
        {
            std::vector<sai_status_t> exp_status{SAI_STATUS_ITEM_NOT_FOUND};
            EXPECT_CALL(*mock_sai_next_hop_api, remove_next_hops)
                .WillOnce(DoAll(SetArrayArgument<3>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_ITEM_NOT_FOUND)));
        }
        SetAndAssertMuxState(STANDBY_STATE);
    }

    TEST_F(MuxRollbackTest, StandbyToActiveRuntimeErrorRollbackToStandby)
    {
        if (!IsPrefixBasedMuxNeighbor())
        {
            EXPECT_CALL(*mock_sai_route_api, remove_route_entries)
                .WillOnce(Throw(runtime_error("Mock runtime error")));
        }
        SetMuxStateFromAppDb(ACTIVE_STATE);
        if (IsPrefixBasedMuxNeighbor())
        {
            // With prefix-based neighbors, state transition should succeed
            EXPECT_EQ(ACTIVE_STATE, m_MuxCable->getState());
        }
        else
        {
            // Without prefix-based neighbors, expect rollback to standby
            EXPECT_EQ(STANDBY_STATE, m_MuxCable->getState());
        }
    }

    TEST_F(MuxRollbackTest, ActiveToStandbyRuntimeErrorRollbackToActive)
    {
        SetAndAssertMuxState(ACTIVE_STATE);
        if (!IsPrefixBasedMuxNeighbor())
        {
            EXPECT_CALL(*mock_sai_route_api, create_route_entries)
                .WillOnce(Throw(runtime_error("Mock runtime error")));
        }
        SetMuxStateFromAppDb(STANDBY_STATE);
        if (IsPrefixBasedMuxNeighbor())
        {
            // With prefix-based neighbors, state transition should succeed
            EXPECT_EQ(STANDBY_STATE, m_MuxCable->getState());
        }
        else
        {
            // Without prefix-based neighbors, expect rollback to active
            EXPECT_EQ(ACTIVE_STATE, m_MuxCable->getState());
        }
    }

    TEST_F(MuxRollbackTest, StandbyToActiveLogicErrorRollbackToStandby)
    {
        if (!IsPrefixBasedMuxNeighbor())
        {
            EXPECT_CALL(*mock_sai_neighbor_api, create_neighbor_entries)
                .WillOnce(Throw(logic_error("Mock logic error")));
        }
        SetMuxStateFromAppDb(ACTIVE_STATE);
        if (IsPrefixBasedMuxNeighbor())
        {
            // With prefix-based neighbors, state transition should succeed
            EXPECT_EQ(ACTIVE_STATE, m_MuxCable->getState());
        }
        else
        {
            // Without prefix-based neighbors, expect rollback to standby
            EXPECT_EQ(STANDBY_STATE, m_MuxCable->getState());
        }
    }

    TEST_F(MuxRollbackTest, ActiveToStandbyLogicErrorRollbackToActive)
    {
        SetAndAssertMuxState(ACTIVE_STATE);
        if (!IsPrefixBasedMuxNeighbor())
        {
            EXPECT_CALL(*mock_sai_neighbor_api, remove_neighbor_entries)
                .WillOnce(Throw(logic_error("Mock logic error")));
        }
        SetMuxStateFromAppDb(STANDBY_STATE);
        if (IsPrefixBasedMuxNeighbor())
        {
            // With prefix-based neighbors, state transition should succeed
            EXPECT_EQ(STANDBY_STATE, m_MuxCable->getState());
        }
        else
        {
            // Without prefix-based neighbors, expect rollback to active
            EXPECT_EQ(ACTIVE_STATE, m_MuxCable->getState());
        }
    }

    TEST_F(MuxRollbackTest, StandbyToActiveExceptionRollbackToStandby)
    {
        if (!IsPrefixBasedMuxNeighbor())
        {
            EXPECT_CALL(*mock_sai_next_hop_api, create_next_hops)
                .WillOnce(Throw(exception()));
        }
        SetMuxStateFromAppDb(ACTIVE_STATE);
        if (IsPrefixBasedMuxNeighbor())
        {
            // With prefix-based neighbors, state transition should succeed
            EXPECT_EQ(ACTIVE_STATE, m_MuxCable->getState());
        }
        else
        {
            // Without prefix-based neighbors, expect rollback to standby
            EXPECT_EQ(STANDBY_STATE, m_MuxCable->getState());
        }
    }

    TEST_F(MuxRollbackTest, ActiveToStandbyExceptionRollbackToActive)
    {
        SetAndAssertMuxState(ACTIVE_STATE);
        if (!IsPrefixBasedMuxNeighbor())
        {
            EXPECT_CALL(*mock_sai_next_hop_api, remove_next_hops)
                .WillOnce(Throw(exception()));
        }
        SetMuxStateFromAppDb(STANDBY_STATE);
        if (IsPrefixBasedMuxNeighbor())
        {
            // With prefix-based neighbors, state transition should succeed
            EXPECT_EQ(STANDBY_STATE, m_MuxCable->getState());
        }
        else
        {
            // Without prefix-based neighbors, expect rollback to active
            EXPECT_EQ(ACTIVE_STATE, m_MuxCable->getState());
        }
    }

    TEST_F(MuxRollbackTest, StandbyToActiveNextHopTableFullRollbackToActive)
    {
        std::vector<sai_status_t> exp_status{SAI_STATUS_TABLE_FULL};
        if (!IsPrefixBasedMuxNeighbor())
        {
            EXPECT_CALL(*mock_sai_next_hop_api, create_next_hops)
                .WillOnce(DoAll(SetArrayArgument<6>(exp_status.begin(), exp_status.end()), Return(SAI_STATUS_TABLE_FULL)));
        }
        SetMuxStateFromAppDb(ACTIVE_STATE);
        if (IsPrefixBasedMuxNeighbor())
        {
            // With prefix-based neighbors, state transition should succeed
            EXPECT_EQ(ACTIVE_STATE, m_MuxCable->getState());
        }
        else
        {
            // Without prefix-based neighbors, expect rollback to standby
            EXPECT_EQ(STANDBY_STATE, m_MuxCable->getState());
        }
    }
}
