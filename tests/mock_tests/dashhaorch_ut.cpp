#include "mock_orch_test.h"
#include "mock_table.h"
#include "mock_sai_api.h"
#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "dash/dashhaorch.h"
#include "pbutils.h"
using namespace ::testing;

extern redisReply *mockReply;
extern sai_redis_communication_mode_t gRedisCommunicationMode;

EXTERN_MOCK_FNS

namespace dashhaorch_ut 
{
    DEFINE_SAI_GENERIC_APIS_MOCK(dash_ha, ha_set, ha_scope);

    using namespace mock_orch_test;

    class DashHaOrchTestable : public DashHaOrch
    {
    public:
        void doTask(swss::NotificationConsumer &consumer) { DashHaOrch::doTask(consumer); }
    };

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

        dash::ha_set::HaSet HaSetPbObject()
        {
            dash::ha_set::HaSet ha_set = dash::ha_set::HaSet();
            swss::IpAddress vip_v4("1.1.1.1");
            swss::IpAddress vip_v6("::1");
            swss::IpAddress npu_ip("2.2.2.2");
            swss::IpAddress local_ip("3.3.3.3");
            // swss::IpAddress peer_ip("4.4.4.4");
            swss::IpAddress peer_ip("::2");

            ha_set.set_version("1");
            ha_set.set_scope(dash::types::HA_SCOPE_DPU);
            ha_set.mutable_vip_v4()->set_ipv4(vip_v4.getV4Addr());
            ha_set.mutable_vip_v6()->set_ipv6(reinterpret_cast<const char*>(vip_v6.getV6Addr()));
            ha_set.mutable_local_npu_ip()->set_ipv4(npu_ip.getV4Addr());
            ha_set.mutable_local_ip()->set_ipv4(local_ip.getV4Addr());
            // ha_set.mutable_peer_ip()->set_ipv4(peer_ip.getV4Addr());
            ha_set.mutable_peer_ip()->set_ipv6(reinterpret_cast<const char*>(peer_ip.getV6Addr()));
            ha_set.set_cp_data_channel_port(100);
            ha_set.set_dp_channel_dst_port(200);
            ha_set.set_dp_channel_src_port_min(0);
            ha_set.set_dp_channel_src_port_max(1000);
            ha_set.set_dp_channel_probe_interval_ms(1000);
            ha_set.set_dp_channel_probe_fail_threshold(3);

            return ha_set;
        }

        void CreateHaSet()
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_HA_SET_TABLE_NAME, 1, 1),
                m_dashHaOrch, APP_DASH_HA_SET_TABLE_NAME));

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            "HA_SET_1",
                            SET_COMMAND,
                            {
                                {"version", "1"},
                                {"vip_v4", "10.0.0.1"},
                                {"vip_v6", "fc00::1"},
                                {"owner", "dpu"},
                                {"scope", "dpu"},
                                {"local_npu_ip", "192.168.1.10"},
                                {"local_ip", "192.168.2.1"},
                                {"peer_ip", "192.168.2.2"},
                                {"cp_data_channel_port", "4789"},
                                {"dp_channel_dst_port", "4790"},
                                {"dp_channel_src_port_min", "5000"},
                                {"dp_channel_src_port_max", "6000"},
                                {"dp_channel_probe_interval_ms", "1000"},
                                {"dp_channel_probe_fail_threshold", "3"}
                            }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaOrch)->doTask(*consumer.get());
        }

        void CreateEniScopeHaSet()
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_HA_SET_TABLE_NAME, 1, 1),
                m_dashHaOrch, APP_DASH_HA_SET_TABLE_NAME));

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            "HA_SET_1",
                            SET_COMMAND,
                            {
                                {"version", "1"},
                                {"vip_v4", "10.0.0.1"},
                                {"vip_v6", "fc00::1"},
                                {"owner", "switch"},
                                {"scope", "eni"},
                                {"local_npu_ip", "192.168.1.10"},
                                {"local_ip", "192.168.2.1"},
                                {"peer_ip", "192.168.2.2"},
                                {"cp_data_channel_port", "4789"},
                                {"dp_channel_dst_port", "4790"},
                                {"dp_channel_src_port_min", "5000"},
                                {"dp_channel_src_port_max", "6000"},
                                {"dp_channel_probe_interval_ms", "1000"},
                                {"dp_channel_probe_fail_threshold", "3"}
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

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            "HA_SET_1",
                            SET_COMMAND,
                            {
                                {"version", "1"},
                                {"ha_role", "dead"}
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

        void SetHaScopeHaRole(std::string role="active")
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_HA_SCOPE_TABLE_NAME, 1, 1),
                m_dashHaOrch, APP_DASH_HA_SCOPE_TABLE_NAME));

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            "HA_SET_1",
                            SET_COMMAND,
                            {
                                {"version", "1"},
                                {"ha_role", role}
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

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            "HA_SET_1",
                            SET_COMMAND,
                            {
                                {"version", "1"},
                                {"ha_role", "active"},
                                {"activate_role_requested", "true"}
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

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            "HA_SET_1",
                            SET_COMMAND,
                            {
                                {"version", "1"},
                                {"ha_role", "active"},
                                {"flow_reconcile_requested", "true"}
                            }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaOrch)->doTask(*consumer.get());
        }

        void RandomTable()
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), "RANDOM_TABLE", 1, 1),
                m_dashHaOrch, "RANDOM_TABLE"));

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            "random_key",
                            SET_COMMAND,
                            {
                                { "pb", "random" }
                            }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaOrch)->doTask(*consumer.get());
        }

        void InvalidHaScopePbString()
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_HA_SCOPE_TABLE_NAME, 1, 1),
                m_dashHaOrch, APP_DASH_HA_SCOPE_TABLE_NAME));

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            "HA_SET_1",
                            SET_COMMAND,
                            {
                                { "pb", "invalid" }
                            }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaOrch)->doTask(*consumer.get());
        }

        void InvalidHaSetPbString()
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_HA_SET_TABLE_NAME, 1, 1),
                m_dashHaOrch, APP_DASH_HA_SET_TABLE_NAME));

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            "HA_SET_1",
                            SET_COMMAND,
                            {
                                { "pb", "invalid" }
                            }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaOrch)->doTask(*consumer.get());
        }

        void HaSetScopeUnspecified()
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_HA_SET_TABLE_NAME, 1, 1),
                m_dashHaOrch, APP_DASH_HA_SET_TABLE_NAME));

            dash::ha_set::HaSet ha_set = dash::ha_set::HaSet();
            ha_set.set_version("1");
            ha_set.set_scope(dash::types::HA_SCOPE_UNSPECIFIED);

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

        void HaSetEvent(sai_ha_set_event_t event_type)
        {
            mockReply = (redisReply *)calloc(sizeof(redisReply), 1);
            mockReply->type = REDIS_REPLY_ARRAY;
            mockReply->elements = 3; // REDIS_PUBLISH_MESSAGE_ELEMNTS
            mockReply->element = (redisReply **)calloc(sizeof(redisReply *), mockReply->elements);
            mockReply->element[2] = (redisReply *)calloc(sizeof(redisReply), 1);
            mockReply->element[2]->type = REDIS_REPLY_STRING;

            sai_ha_set_event_data_t event;
            memset(&event, 0, sizeof(event));
            event.ha_set_id = m_dashHaOrch->getHaScopeEntries().begin()->second.ha_scope_id;
            event.event_type = event_type;

            std::string data = sai_serialize_ha_set_event_ntf(1, &event);

            std::vector<FieldValueTuple> notifyValues;
            FieldValueTuple opdata(SAI_SWITCH_NOTIFICATION_NAME_HA_SET_EVENT, data);
            notifyValues.push_back(opdata);
            std::string msg = swss::JSon::buildJson(notifyValues);

            mockReply->element[2]->str = (char*)calloc(1, msg.length() + 1);
            memcpy(mockReply->element[2]->str, msg.c_str(), msg.length());

            auto exec = static_cast<Notifier *>(m_dashHaOrch->getExecutor("HA_SET_NOTIFICATIONS"));
            auto consumer = exec->getNotificationConsumer();
            consumer->readData();
            static_cast<DashHaOrchTestable*>(m_dashHaOrch)->doTask(*consumer);
            mockReply = nullptr;

            sai_redis_communication_mode_t old_mode = gRedisCommunicationMode;
            gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC;
            on_ha_set_event(1, &event);
            gRedisCommunicationMode = old_mode;
        }

        void HaScopeEvent(sai_ha_scope_event_t event_type,
                        sai_dash_ha_role_t ha_role,
                        sai_dash_ha_state_t ha_state)
        {
            mockReply = (redisReply *)calloc(sizeof(redisReply), 1);
            mockReply->type = REDIS_REPLY_ARRAY;
            mockReply->elements = 3; // REDIS_PUBLISH_MESSAGE_ELEMNTS
            mockReply->element = (redisReply **)calloc(sizeof(redisReply *), mockReply->elements);
            mockReply->element[2] = (redisReply *)calloc(sizeof(redisReply), 1);
            mockReply->element[2]->type = REDIS_REPLY_STRING;

            sai_ha_scope_event_data_t event;
            memset(&event, 0, sizeof(event));
            event.ha_scope_id = m_dashHaOrch->getHaScopeEntries().begin()->second.ha_scope_id;
            event.event_type = event_type;
            event.ha_role = ha_role;
            event.ha_state = ha_state;

            std::string data = sai_serialize_ha_scope_event_ntf(1, &event);

            std::vector<FieldValueTuple> notifyValues;
            FieldValueTuple opdata(SAI_SWITCH_NOTIFICATION_NAME_HA_SCOPE_EVENT, data);
            notifyValues.push_back(opdata);
            std::string msg = swss::JSon::buildJson(notifyValues);

            mockReply->element[2]->str = (char*)calloc(1, msg.length() + 1);
            memcpy(mockReply->element[2]->str, msg.c_str(), msg.length());

            mockReply->element[2]->str = (char*)calloc(1, msg.length() + 1);
            memcpy(mockReply->element[2]->str, msg.c_str(), msg.length());

            auto exec = static_cast<Notifier *>(m_dashHaOrch->getExecutor("HA_SCOPE_NOTIFICATIONS"));
            auto consumer = exec->getNotificationConsumer();
            consumer->readData();
            static_cast<DashHaOrchTestable*>(m_dashHaOrch)->doTask(*consumer);
            mockReply = nullptr;

            sai_redis_communication_mode_t old_mode = gRedisCommunicationMode;
            gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC;
            on_ha_scope_event(1, &event);
            gRedisCommunicationMode = old_mode;
        }
    };

    TEST_F(DashHaOrchTest, AddRemoveHaSet)
    {
        EXPECT_CALL(*mock_sai_dash_ha_api, create_ha_set)
        .Times(1)
        .WillOnce(Return(SAI_STATUS_SUCCESS));

        CreateHaSet();

        HaSetEvent(SAI_HA_SET_EVENT_DP_CHANNEL_UP);

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

        // HA Scope already exists
        EXPECT_CALL(*mock_sai_dash_ha_api, create_ha_scope)
        .Times(0);
        CreateHaScope();

        EXPECT_CALL(*mock_sai_dash_ha_api, remove_ha_scope)
        .Times(1)
        .WillOnce(Return(SAI_STATUS_SUCCESS));

        RemoveHaScope();
    }

    TEST_F(DashHaOrchTest, AddRemoveEniHaScope)
    {
        CreateEniScopeHaSet();

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

        EXPECT_CALL(*mock_sai_dash_ha_api, remove_ha_set)
        .Times(0);

        RemoveHaSet();
    }

    TEST_F(DashHaOrchTest, SetHaScopeHaRole)
    {
        CreateHaSet();
        CreateHaScope();

        EXPECT_EQ(to_sai(m_dashHaOrch->getHaScopeEntries().find("HA_SET_1")->second.metadata.ha_role()), SAI_DASH_HA_ROLE_DEAD);

        SetHaScopeHaRole();
        HaScopeEvent(SAI_HA_SCOPE_EVENT_STATE_CHANGED,
                    SAI_DASH_HA_ROLE_ACTIVE, SAI_DASH_HA_STATE_ACTIVE);
        EXPECT_EQ(to_sai(m_dashHaOrch->getHaScopeEntries().find("HA_SET_1")->second.metadata.ha_role()), SAI_DASH_HA_ROLE_ACTIVE);

        SetHaScopeHaRole("");
        HaScopeEvent(SAI_HA_SCOPE_EVENT_STATE_CHANGED,
                    SAI_DASH_HA_ROLE_DEAD, SAI_DASH_HA_STATE_DEAD);
        EXPECT_EQ(to_sai(m_dashHaOrch->getHaScopeEntries().find("HA_SET_1")->second.metadata.ha_role()), SAI_DASH_HA_ROLE_DEAD);

        SetHaScopeHaRole("dead");
        HaScopeEvent(SAI_HA_SCOPE_EVENT_STATE_CHANGED,
                    SAI_DASH_HA_ROLE_DEAD, SAI_DASH_HA_STATE_DEAD);
        EXPECT_EQ(to_sai(m_dashHaOrch->getHaScopeEntries().find("HA_SET_1")->second.metadata.ha_role()), SAI_DASH_HA_ROLE_DEAD);

        SetHaScopeHaRole("standby");
        HaScopeEvent(SAI_HA_SCOPE_EVENT_STATE_CHANGED,
                    SAI_DASH_HA_ROLE_STANDBY, SAI_DASH_HA_STATE_STANDBY);
        EXPECT_EQ(to_sai(m_dashHaOrch->getHaScopeEntries().find("HA_SET_1")->second.metadata.ha_role()), SAI_DASH_HA_ROLE_STANDBY);

        SetHaScopeHaRole("standalone");
        HaScopeEvent(SAI_HA_SCOPE_EVENT_STATE_CHANGED,
                    SAI_DASH_HA_ROLE_STANDALONE, SAI_DASH_HA_STATE_STANDALONE);
        EXPECT_EQ(to_sai(m_dashHaOrch->getHaScopeEntries().find("HA_SET_1")->second.metadata.ha_role()), SAI_DASH_HA_ROLE_STANDALONE);

        SetHaScopeHaRole("switching_to_active");
        HaScopeEvent(SAI_HA_SCOPE_EVENT_STATE_CHANGED,
                    SAI_DASH_HA_ROLE_SWITCHING_TO_ACTIVE, SAI_DASH_HA_STATE_PENDING_ACTIVE_ACTIVATION);
        EXPECT_EQ(to_sai(m_dashHaOrch->getHaScopeEntries().find("HA_SET_1")->second.metadata.ha_role()), SAI_DASH_HA_ROLE_SWITCHING_TO_ACTIVE);

        RemoveHaScope();
        RemoveHaSet();
    }

    TEST_F(DashHaOrchTest, LastRoleStartTime)
    {
        CreateHaSet();
        CreateHaScope();

        std::time_t last_role_start_time = m_dashHaOrch->getHaScopeEntries().begin()->second.last_role_start_time;

        sleep(1); // Ensure time difference

        HaScopeEvent(SAI_HA_SCOPE_EVENT_STATE_CHANGED,
                    SAI_DASH_HA_ROLE_ACTIVE, SAI_DASH_HA_STATE_ACTIVE);

        EXPECT_TRUE(m_dashHaOrch->getHaScopeEntries().begin()->second.last_role_start_time > last_role_start_time);

        last_role_start_time = m_dashHaOrch->getHaScopeEntries().begin()->second.last_role_start_time;

        sleep(1); 

        HaScopeEvent(SAI_HA_SCOPE_EVENT_STATE_CHANGED,
                    SAI_DASH_HA_ROLE_ACTIVE, SAI_DASH_HA_STATE_ACTIVE);

        EXPECT_TRUE(m_dashHaOrch->getHaScopeEntries().begin()->second.last_role_start_time == last_role_start_time);

        RemoveHaScope();
        RemoveHaSet();
    }

    TEST_F(DashHaOrchTest, HaScopeActivateRoleRequest)
    {
        CreateHaSet();
        CreateHaScope();

        HaScopeEvent(SAI_HA_SCOPE_EVENT_STATE_CHANGED,
                    SAI_DASH_HA_ROLE_SWITCHING_TO_ACTIVE, SAI_DASH_HA_STATE_PENDING_ACTIVE_ACTIVATION);

        EXPECT_EQ(to_sai(m_dashHaOrch->getHaScopeEntries().find("HA_SET_1")->second.metadata.ha_role()), SAI_DASH_HA_ROLE_SWITCHING_TO_ACTIVE);

        EXPECT_CALL(*mock_sai_dash_ha_api, set_ha_scope_attribute)
        .Times(2)       // Set ha_role and activate_role_requested
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));

        SetHaScopeActivateRoleRequest();

        HaScopeEvent(SAI_HA_SCOPE_EVENT_STATE_CHANGED,
                    SAI_DASH_HA_ROLE_ACTIVE, SAI_DASH_HA_STATE_ACTIVE);

        EXPECT_EQ(to_sai(m_dashHaOrch->getHaScopeEntries().find("HA_SET_1")->second.metadata.ha_role()), SAI_DASH_HA_ROLE_ACTIVE);

        RemoveHaScope();
        RemoveHaSet();
    }

    TEST_F(DashHaOrchTest, HaScopeFlowReconcileRequest)
    {
        CreateHaSet();
        CreateHaScope();

        HaScopeEvent(SAI_HA_SCOPE_EVENT_FLOW_RECONCILE_NEEDED,
            SAI_DASH_HA_ROLE_ACTIVE, SAI_DASH_HA_STATE_ACTIVE);

        EXPECT_CALL(*mock_sai_dash_ha_api, set_ha_scope_attribute)
        .Times(1)
        .WillOnce(Return(SAI_STATUS_SUCCESS));

        SetHaScopeFlowReconcileRequest();

        RemoveHaScope();
        RemoveHaSet();
    }

    TEST_F(DashHaOrchTest, HaScopeSplitBrain)
    {
        CreateHaSet();
        CreateHaScope();

        HaScopeEvent(SAI_HA_SCOPE_EVENT_SPLIT_BRAIN_DETECTED,
            SAI_DASH_HA_ROLE_ACTIVE, SAI_DASH_HA_STATE_ACTIVE);

        RemoveHaScope();
        RemoveHaSet();
    }

    TEST_F(DashHaOrchTest, InvalidInput)
    {
        RandomTable();
        InvalidHaScopePbString();
        InvalidHaSetPbString();

        EXPECT_EQ(m_dashHaOrch->getHaScopeEntries().find("HA_SET_1"), m_dashHaOrch->getHaScopeEntries().end());
        EXPECT_EQ(m_dashHaOrch->getHaSetEntries().find("HA_SET_1"), m_dashHaOrch->getHaSetEntries().end());

        HaSetScopeUnspecified();
        CreateHaScope();
    }
}
