#include "mock_orch_test.h"
#include "mock_table.h"
#include "mock_sai_api.h"
#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "dash/dashhaorch.h"
using namespace ::testing;

EXTERN_MOCK_FNS

namespace dashhaorch_ut 
{
    DEFINE_SAI_GENERIC_APIS_MOCK(dash_ha, ha_set, ha_scope);

    using namespace mock_orch_test;

    class DashHaOrchTest : public MockOrchTest
    {
    protected:

        void ApplySaiMock()
        {
            INIT_SAI_API_MOCK(dash_ha);
            MockSaiApis();
        }

        void PreTearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_ha);
        }

        void CreateHaSet()
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_HA_SET_TABLE_NAME, 1, 1),
                m_dashHaOrch, APP_DASH_HA_SET_TABLE_NAME));

            dash::ha_set::HaSet ha_set = dash::ha_set::HaSet();
            swss::IpAddress vip_v4("1.1.1.1");
            swss::IpAddress vip_v6("::1");
            swss::IpAddress npu_ip("2.2.2.2");
            swss::IpAddress local_ip("3.3.3.3");
            swss::IpAddress peer_ip("4.4.4.4");

            ha_set.set_version("1");
            ha_set.set_scope(dash::types::SCOPE_DPU);
            ha_set.mutable_vip_v4()->set_ipv4(vip_v4.getV4Addr());
            ha_set.mutable_vip_v6()->set_ipv6(reinterpret_cast<const char*>(vip_v6.getV6Addr()));
            ha_set.mutable_local_npu_ip()->set_ipv4(npu_ip.getV4Addr());
            ha_set.mutable_local_ip()->set_ipv4(local_ip.getV4Addr());
            ha_set.mutable_peer_ip()->set_ipv4(peer_ip.getV4Addr());
            ha_set.set_cp_data_channel_port(100);
            ha_set.set_dp_channel_dst_port(200);
            ha_set.set_dp_channel_src_port_min(0);
            ha_set.set_dp_channel_src_port_max(1000);
            ha_set.set_dp_channel_probe_interval_ms(1000);
            ha_set.set_dp_channel_probe_fail_threshold(3);
            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            "HA_SET_1",
                            SET_COMMAND,
                            {
                                { "pb", ha_set.SerializeAsString() }
                            }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaOrch)->doTask(*consumer.get());
        }

        void RemoveHaSet()
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_HA_SET_TABLE_NAME, 1, 1),
                m_dashHaOrch, APP_DASH_HA_SET_TABLE_NAME));

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            "HA_SET_1",
                            DEL_COMMAND,
                            { }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaOrch)->doTask(*consumer.get());
        }

        void CreateHaScope()
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_HA_SCOPE_TABLE_NAME, 1, 1),
                m_dashHaOrch, APP_DASH_HA_SCOPE_TABLE_NAME));

            dash::ha_scope::HaScope ha_scope;
            ha_scope.set_version("1");
            ha_scope.set_ha_role(dash::types::HA_SCOPE_ROLE_DEAD);

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            "HA_SET_1",
                            SET_COMMAND,
                            {
                                { "pb", ha_scope.SerializeAsString() }
                            }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaOrch)->doTask(*consumer.get());
        }

        void RemoveHaScope()
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_HA_SCOPE_TABLE_NAME, 1, 1),
                m_dashHaOrch, APP_DASH_HA_SCOPE_TABLE_NAME));

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            "HA_SET_1",
                            DEL_COMMAND,
                            { }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaOrch)->doTask(*consumer.get());
        }

        void SetHaScopeHaRole()
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_HA_SCOPE_TABLE_NAME, 1, 1),
                m_dashHaOrch, APP_DASH_HA_SCOPE_TABLE_NAME));
            
            dash::ha_scope::HaScope ha_scope;
            ha_scope.set_version("1");
            ha_scope.set_ha_role(dash::types::HA_SCOPE_ROLE_ACTIVE);

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            "HA_SET_1",
                            SET_COMMAND,
                            {
                                { "pb", ha_scope.SerializeAsString() }
                            }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaOrch)->doTask(*consumer.get());
        }

        void SetHaScopeActivateRoleRequest()
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_HA_SCOPE_TABLE_NAME, 1, 1),
                m_dashHaOrch, APP_DASH_HA_SCOPE_TABLE_NAME));

            dash::ha_scope::HaScope ha_scope;
            ha_scope.set_version("1");
            ha_scope.set_ha_role(dash::types::HA_SCOPE_ROLE_ACTIVE);
            ha_scope.set_activate_role_requested(true);

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            "HA_SET_1",
                            SET_COMMAND,
                            {
                                { "pb", ha_scope.SerializeAsString() }
                            }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaOrch)->doTask(*consumer.get());
        }

        void SetHaScopeFlowReconcileRequest()
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_HA_SCOPE_TABLE_NAME, 1, 1),
                m_dashHaOrch, APP_DASH_HA_SCOPE_TABLE_NAME));

            dash::ha_scope::HaScope ha_scope;
            ha_scope.set_version("1");
            ha_scope.set_ha_role(dash::types::HA_SCOPE_ROLE_ACTIVE);
            ha_scope.set_flow_reconcile_requested(true);

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            "HA_SET_1",
                            SET_COMMAND,
                            {
                                { "pb", ha_scope.SerializeAsString() }
                            }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaOrch)->doTask(*consumer.get());
        }
    };

    TEST_F(DashHaOrchTest, AddRemoveHaSet)
    {
        EXPECT_CALL(*mock_sai_dash_ha_api, create_ha_set)
        .Times(1)
        .WillOnce(Return(SAI_STATUS_SUCCESS));

        CreateHaSet();

        EXPECT_CALL(*mock_sai_dash_ha_api, remove_ha_set)
        .Times(1)
        .WillOnce(Return(SAI_STATUS_SUCCESS));

        RemoveHaSet();
    }

    TEST_F(DashHaOrchTest, HaSetAlreadyExists)
    {
        CreateHaSet();

        EXPECT_CALL(*mock_sai_dash_ha_api, create_ha_set)
        .Times(0);

        CreateHaSet();

        EXPECT_CALL(*mock_sai_dash_ha_api, remove_ha_set)
        .Times(1)
        .WillOnce(Return(SAI_STATUS_SUCCESS));

        RemoveHaSet();
    }

    TEST_F(DashHaOrchTest, AddRemoveHaScope)
    {
        CreateHaSet();

        EXPECT_CALL(*mock_sai_dash_ha_api, create_ha_scope)
        .Times(1)
        .WillOnce(Return(SAI_STATUS_SUCCESS));

        CreateHaScope();

        EXPECT_CALL(*mock_sai_dash_ha_api, remove_ha_scope)
        .Times(1)
        .WillOnce(Return(SAI_STATUS_SUCCESS));

        RemoveHaScope();
    }

    TEST_F(DashHaOrchTest, NoHaSetFound)
    {
        EXPECT_CALL(*mock_sai_dash_ha_api, create_ha_scope)
        .Times(0);

        CreateHaScope();

        EXPECT_CALL(*mock_sai_dash_ha_api, remove_ha_scope)
        .Times(0);

        RemoveHaScope();
    }

    TEST_F(DashHaOrchTest, SetHaScopeHaRole)
    {
        CreateHaSet();
        CreateHaScope();
        EXPECT_EQ(m_dashHaOrch->getHaScopeEntries().find("HA_SET_1")->second.metadata.ha_role(), dash::types::HA_SCOPE_ROLE_DEAD);

        SetHaScopeHaRole();

        EXPECT_EQ(m_dashHaOrch->getHaScopeEntries().find("HA_SET_1")->second.metadata.ha_role(), dash::types::HA_SCOPE_ROLE_ACTIVE);

        RemoveHaScope();
        RemoveHaSet();
    }

    TEST_F(DashHaOrchTest, SetHaScopeActivateRoleRequest)
    {
        CreateHaSet();
        CreateHaScope();
        SetHaScopeHaRole();

        EXPECT_CALL(*mock_sai_dash_ha_api, set_ha_scope_attribute)
        .Times(1)
        .WillOnce(Return(SAI_STATUS_SUCCESS));

        SetHaScopeActivateRoleRequest();

        RemoveHaScope();
        RemoveHaSet();
    }

    TEST_F(DashHaOrchTest, SetHaScopeFlowReconcileRequest)
    {
        CreateHaSet();
        CreateHaScope();
        SetHaScopeHaRole();

        EXPECT_CALL(*mock_sai_dash_ha_api, set_ha_scope_attribute)
        .Times(1)
        .WillOnce(Return(SAI_STATUS_SUCCESS));

        SetHaScopeFlowReconcileRequest();

        RemoveHaScope();
        RemoveHaSet();
    }
}
