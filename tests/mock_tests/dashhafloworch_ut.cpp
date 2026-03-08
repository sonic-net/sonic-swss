#include "mock_orch_test.h"
#include "mock_table.h"
#include "mock_sai_api.h"
#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "dash/dashhafloworch.h"
using namespace ::testing;
using ::testing::DoAll;
using ::testing::SetArgPointee;

extern redisReply *mockReply;
extern sai_redis_communication_mode_t gRedisCommunicationMode;

EXTERN_MOCK_FNS

namespace dashhafloworch_ut 
{
    DEFINE_SAI_GENERIC_APIS_MOCK(dash_flow, flow_entry_bulk_get_session, flow_entry_bulk_get_session_filter);

    using namespace mock_orch_test;

    class DashHaFlowOrchTestable : public DashHaFlowOrch
    {
    public:
        DashHaFlowOrchTestable(swss::DBConnector *db, const std::vector<std::string> &tableNames, swss::DBConnector *app_state_db, swss::ZmqServer *zmqServer)
            : DashHaFlowOrch(db, tableNames, app_state_db, zmqServer) {}
        void doTask(swss::NotificationConsumer &consumer) { DashHaFlowOrch::doTask(consumer); }
        void doTask(swss::SelectableTimer &timer) { DashHaFlowOrch::doTask(timer); }
        void handleSessionFinished(sai_object_id_t session_id) { DashHaFlowOrch::handleSessionFinished(session_id); }
        void handleTimerExpired(swss::SelectableTimer *timer) { DashHaFlowOrch::handleTimerExpired(timer); }
        swss::SelectableTimer* getSyncTimer() { return m_sync_timer; }
        swss::SelectableTimer* getDumpTimer() { return m_dump_timer; }
    };

    class DashHaFlowOrchTest : public MockOrchTest
    {
    protected:
        DashHaFlowOrchTestable *m_dashHaFlowOrch;
        shared_ptr<swss::DBConnector> m_dpu_state_db;

        void PostSetUp() override
        {
            m_dpu_state_db = make_shared<swss::DBConnector>("DPU_STATE_DB", 0);
            vector<string> dash_ha_flow_tables = {
                APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME,
                APP_DASH_FLOW_DUMP_FILTER_TABLE_NAME
            };
            m_dashHaFlowOrch = new DashHaFlowOrchTestable(m_dpu_app_db.get(), dash_ha_flow_tables, m_dpu_app_state_db.get(), nullptr);
            gDirectory.set(m_dashHaFlowOrch);
            ut_orch_list.push_back((Orch **)&m_dashHaFlowOrch);
        }

        void ApplySaiMock()
        {
            INIT_SAI_API_MOCK(dash_flow);
            MockSaiApis();
        }

        void PreTearDown() override
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_flow);
        }

        void CreateFlowSyncSession(const string &key, const string &ha_set_id, const string &target_server_ip, const string &target_server_port, const string &timeout = "120")
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME, 1, 1),
                m_dashHaFlowOrch, APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME));

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            key,
                            SET_COMMAND,
                            {
                                {"type", DashHaFlowOrch::SESSION_TYPE_BULK_SYNC},
                                {"ha_set_id", ha_set_id},
                                {"target_server_ip", target_server_ip},
                                {"target_server_port", target_server_port},
                                {"timeout", timeout}
                            }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaFlowOrch)->doTask(*consumer.get());
        }

        void RemoveFlowSyncSession(const string &key)
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME, 1, 1),
                m_dashHaFlowOrch, APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME));

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            key,
                            DEL_COMMAND,
                            { }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaFlowOrch)->doTask(*consumer.get());
        }

        void CreateFlowDumpFilter(const string &key, const string &filter_key, const string &filter_op, const string &filter_value)
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_FLOW_DUMP_FILTER_TABLE_NAME, 1, 1),
                m_dashHaFlowOrch, APP_DASH_FLOW_DUMP_FILTER_TABLE_NAME));

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            key,
                            SET_COMMAND,
                            {
                                {"key", filter_key},
                                {"op", filter_op},
                                {"value", filter_value}
                            }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaFlowOrch)->doTask(*consumer.get());
        }

        void RemoveFlowDumpFilter(const string &key)
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_FLOW_DUMP_FILTER_TABLE_NAME, 1, 1),
                m_dashHaFlowOrch, APP_DASH_FLOW_DUMP_FILTER_TABLE_NAME));

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            key,
                            DEL_COMMAND,
                            { }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaFlowOrch)->doTask(*consumer.get());
        }

        void CreateFlowDumpSession(const string &key, const string &flow_state = "true", const string &max_flows = "1000", const string &timeout = "300", const vector<string> &filters = {})
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME, 1, 1),
                m_dashHaFlowOrch, APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME));

            vector<FieldValueTuple> fvs = {
                {"type", DashHaFlowOrch::SESSION_TYPE_FLOW_DUMP},
                {"flow_state", flow_state},
                {"max_flows", max_flows},
                {"timeout", timeout}
            };
            
            for (size_t i = 0; i < filters.size() && i < 5; i++)
            {
                string filter_attr = "filter_" + to_string(i + 1);
                fvs.push_back({filter_attr, filters[i]});
            }

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            key,
                            SET_COMMAND,
                            fvs
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaFlowOrch)->doTask(*consumer.get());
        }

        void RemoveFlowDumpSession(const string &key)
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME, 1, 1),
                m_dashHaFlowOrch, APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME));

            consumer->addToSync(
                deque<KeyOpFieldsValuesTuple>(
                    {
                        {
                            key,
                            DEL_COMMAND,
                            { }
                        }
                    }
                )
            );
            static_cast<Orch *>(m_dashHaFlowOrch)->doTask(*consumer.get());
        }
    };

    TEST_F(DashHaFlowOrchTest, CreateRemoveFlowSyncSession)
    {
        sai_object_id_t session_id = 0x1000000000000001;
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(session_id), Return(SAI_STATUS_SUCCESS)));

        CreateFlowSyncSession("SYNC_SESSION_1", "", "192.168.1.1", "8080");

        // Verify STATE_DB update after creation
        swss::Table state_table(m_dpu_state_db.get(), STATE_DASH_FLOW_SYNC_SESSION_STATE_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(state_table.get("SYNC_SESSION_1", fvs));
        bool found_state = false;
        for (const auto &fv : fvs)
        {
            if (fvField(fv) == "state" && fvValue(fv) == "created")
            {
                found_state = true;
                break;
            }
        }
        ASSERT_TRUE(found_state) << "STATE_DB should have state=created after session creation";
    }

    TEST_F(DashHaFlowOrchTest, CreateFlowSyncSessionMissingFields)
    {
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session)
            .Times(0);

        // Missing target_server_ip
        auto consumer = unique_ptr<Consumer>(new Consumer(
            new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME, 1, 1),
            m_dashHaFlowOrch, APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME));

        consumer->addToSync(
            deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "SYNC_SESSION_1",
                        SET_COMMAND,
                            {
                                {"type", DashHaFlowOrch::SESSION_TYPE_BULK_SYNC},
                                {"ha_set_id", ""},
                                {"target_server_port", "8080"}
                            }
                    }
                }
            )
        );
        static_cast<Orch *>(m_dashHaFlowOrch)->doTask(*consumer.get());

        // Verify STATE_DB update shows failed state
        swss::Table state_table(m_dpu_state_db.get(), STATE_DASH_FLOW_SYNC_SESSION_STATE_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(state_table.get("SYNC_SESSION_1", fvs));
        bool found_failed = false;
        for (const auto &fv : fvs)
        {
            if (fvField(fv) == "state" && fvValue(fv) == "failed")
            {
                found_failed = true;
                break;
            }
        }
        ASSERT_TRUE(found_failed) << "STATE_DB should have state=failed when session creation fails";
    }

    TEST_F(DashHaFlowOrchTest, CreateFlowDumpSessionWithFilters)
    {
        sai_object_id_t filter_id_1 = 0x2000000000000001;
        sai_object_id_t filter_id_2 = 0x2000000000000002;
        sai_object_id_t session_id = 0x1000000000000001;
        
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session_filter)
            .Times(2)
            .WillOnce(DoAll(SetArgPointee<0>(filter_id_1), Return(SAI_STATUS_SUCCESS)))
            .WillOnce(DoAll(SetArgPointee<0>(filter_id_2), Return(SAI_STATUS_SUCCESS)));

        CreateFlowDumpFilter("FILTER_1", "eni_addr", "equal_to", "00:11:22:33:44:55");
        CreateFlowDumpFilter("FILTER_2", "ip_protocol", "equal_to", "6");

        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(session_id), Return(SAI_STATUS_SUCCESS)));

        CreateFlowDumpSession("DUMP_SESSION_1", "true", "1000", "300", {"FILTER_1", "FILTER_2"});
    }

    TEST_F(DashHaFlowOrchTest, CreateFlowDumpSessionWaitingForFilters)
    {
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session)
            .Times(0);

        // Create session before filters are available - should return task_need_retry
        auto consumer = unique_ptr<Consumer>(new Consumer(
            new swss::ConsumerStateTable(m_dpu_app_db.get(), APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME, 1, 1),
            m_dashHaFlowOrch, APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME));

        vector<FieldValueTuple> fvs = {
            {"type", DashHaFlowOrch::SESSION_TYPE_FLOW_DUMP},
            {"flow_state", "true"},
            {"max_flows", "1000"},
            {"timeout", "300"},
            {"filter_1", "FILTER_1"}
        };

        consumer->addToSync(
            deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "DUMP_SESSION_1",
                        SET_COMMAND,
                        fvs
                    }
                }
            )
        );
        static_cast<Orch *>(m_dashHaFlowOrch)->doTask(*consumer.get());

        // Verify session is in pending state
        swss::Table state_table(m_dpu_state_db.get(), STATE_DASH_FLOW_SYNC_SESSION_STATE_TABLE_NAME);
        vector<FieldValueTuple> state_fvs;
        ASSERT_TRUE(state_table.get("DUMP_SESSION_1", state_fvs));
        bool found_pending = false;
        for (const auto &fv : state_fvs)
        {
            if (fvField(fv) == "state" && fvValue(fv) == "pending")
            {
                found_pending = true;
                break;
            }
        }
        ASSERT_TRUE(found_pending) << "Session should be in pending state when filters are missing";

        // Now add the filter
        sai_object_id_t filter_id = 0x2000000000000001;
        sai_object_id_t session_id = 0x1000000000000001;
        
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session_filter)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(filter_id), Return(SAI_STATUS_SUCCESS)));

        CreateFlowDumpFilter("FILTER_1", "eni_addr", "equal_to", "00:11:22:33:44:55");

        // Retry the session creation - now filters are available, session should be created
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(session_id), Return(SAI_STATUS_SUCCESS)));

        // Retry by calling doTask again on the same consumer (simulating orchagent retry mechanism)
        static_cast<Orch *>(m_dashHaFlowOrch)->doTask(*consumer.get());

        // Verify session was created successfully
        ASSERT_TRUE(state_table.get("DUMP_SESSION_1", state_fvs));
        bool found_created = false;
        for (const auto &fv : state_fvs)
        {
            if (fvField(fv) == "state" && fvValue(fv) == "created")
            {
                found_created = true;
                break;
            }
        }
        ASSERT_TRUE(found_created) << "Session should be created after filters are available and retry";
    }

    TEST_F(DashHaFlowOrchTest, DuplicateSessionCreation)
    {
        sai_object_id_t session_id = 0x1000000000000001;
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(session_id), Return(SAI_STATUS_SUCCESS)));

        CreateFlowSyncSession("SYNC_SESSION_1", "", "192.168.1.1", "8080");

        // Try to create another sync session - should fail because first session is still active
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session)
            .Times(0);

        CreateFlowSyncSession("SYNC_SESSION_2", "", "192.168.1.2", "8081");

        // Verify second session failed (should have failed state in STATE_DB)
        swss::Table state_table(m_dpu_state_db.get(), STATE_DASH_FLOW_SYNC_SESSION_STATE_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        if (state_table.get("SYNC_SESSION_2", fvs))
        {
            bool found_failed = false;
            for (const auto &fv : fvs)
            {
                if (fvField(fv) == "state" && fvValue(fv) == "failed")
                {
                    found_failed = true;
                    break;
                }
            }
            ASSERT_TRUE(found_failed) << "Second session should have failed state";
        }
    }

    TEST_F(DashHaFlowOrchTest, CreateFlowSyncSessionSAIFailure)
    {
        // Test that when SAI create fails, state is updated to failed
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session)
            .Times(1)
            .WillOnce(Return(SAI_STATUS_FAILURE));

        CreateFlowSyncSession("SYNC_SESSION_1", "", "192.168.1.1", "8080");

        // Verify STATE_DB update shows failed state
        swss::Table state_table(m_dpu_state_db.get(), STATE_DASH_FLOW_SYNC_SESSION_STATE_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(state_table.get("SYNC_SESSION_1", fvs));
        bool found_failed = false;
        for (const auto &fv : fvs)
        {
            if (fvField(fv) == "state" && fvValue(fv) == "failed")
            {
                found_failed = true;
                break;
            }
        }
        ASSERT_TRUE(found_failed) << "STATE_DB should have state=failed when SAI create fails";
    }

    TEST_F(DashHaFlowOrchTest, CreateFlowDumpSessionSAIFailure)
    {
        sai_object_id_t filter_id = 0x2000000000000001;
        
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session_filter)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(filter_id), Return(SAI_STATUS_SUCCESS)));

        CreateFlowDumpFilter("FILTER_1", "eni_addr", "equal_to", "00:11:22:33:44:55");

        // SAI session create fails
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session)
            .Times(1)
            .WillOnce(Return(SAI_STATUS_FAILURE));

        CreateFlowDumpSession("DUMP_SESSION_1", "true", "1000", "300", {"FILTER_1"});

        // Verify STATE_DB update shows failed state
        swss::Table state_table(m_dpu_state_db.get(), STATE_DASH_FLOW_SYNC_SESSION_STATE_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(state_table.get("DUMP_SESSION_1", fvs));
        bool found_failed = false;
        for (const auto &fv : fvs)
        {
            if (fvField(fv) == "state" && fvValue(fv) == "failed")
            {
                found_failed = true;
                break;
            }
        }
        ASSERT_TRUE(found_failed) << "STATE_DB should have state=failed when SAI create fails";
    }

    TEST_F(DashHaFlowOrchTest, CreateFlowDumpFilterInvalidKey)
    {
        // Test that invalid filter key results in failure
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session_filter)
            .Times(0);

        CreateFlowDumpFilter("FILTER_1", "invalid_key", "equal_to", "value");
    }

    TEST_F(DashHaFlowOrchTest, CreateFlowDumpFilterInvalidOp)
    {
        // Test that invalid filter op results in failure
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session_filter)
            .Times(0);

        CreateFlowDumpFilter("FILTER_1", "eni_addr", "invalid_op", "00:11:22:33:44:55");
    }

    TEST_F(DashHaFlowOrchTest, RemoveFlowDumpFilter)
    {
        sai_object_id_t filter_id = 0x2000000000000001;
        
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session_filter)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(filter_id), Return(SAI_STATUS_SUCCESS)));

        CreateFlowDumpFilter("FILTER_1", "eni_addr", "equal_to", "00:11:22:33:44:55");

        EXPECT_CALL(*mock_sai_dash_flow_api, remove_flow_entry_bulk_get_session_filter)
            .Times(1)
            .WillOnce(Return(SAI_STATUS_SUCCESS));

        RemoveFlowDumpFilter("FILTER_1");
    }

    TEST_F(DashHaFlowOrchTest, HandleFinishedBulkSyncSession)
    {
        sai_object_id_t session_id = 0x1000000000000001;
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(session_id), Return(SAI_STATUS_SUCCESS)));

        CreateFlowSyncSession("SYNC_SESSION_1", "", "192.168.1.1", "8080");

        // Verify session was created
        swss::Table state_table(m_dpu_state_db.get(), STATE_DASH_FLOW_SYNC_SESSION_STATE_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(state_table.get("SYNC_SESSION_1", fvs));

        // Verify SAI object deletion is called when handleFinished is called
        EXPECT_CALL(*mock_sai_dash_flow_api, remove_flow_entry_bulk_get_session)
            .Times(1)
            .WillOnce(Return(SAI_STATUS_SUCCESS));

        // Call handleFinished via handleSessionFinished
        m_dashHaFlowOrch->handleSessionFinished(session_id);

        // Verify state DB shows completed state
        ASSERT_TRUE(state_table.get("SYNC_SESSION_1", fvs));
        bool found_completed = false;
        for (const auto &fv : fvs)
        {
            if (fvField(fv) == "state" && fvValue(fv) == "completed")
            {
                found_completed = true;
                break;
            }
        }
        ASSERT_TRUE(found_completed) << "STATE_DB should have state=completed after handleFinished";

        // Verify that after handleFinished, a new session can be created
        sai_object_id_t new_session_id = 0x1000000000000002;
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(new_session_id), Return(SAI_STATUS_SUCCESS)));

        CreateFlowSyncSession("SYNC_SESSION_2", "", "192.168.1.2", "8081");

        // Verify new session was created successfully
        ASSERT_TRUE(state_table.get("SYNC_SESSION_2", fvs));
        bool found_created = false;
        for (const auto &fv : fvs)
        {
            if (fvField(fv) == "state" && fvValue(fv) == "created")
            {
                found_created = true;
                break;
            }
        }
        ASSERT_TRUE(found_created) << "New session should be created after previous session finished";
    }

    TEST_F(DashHaFlowOrchTest, HandleTimeoutBulkSyncSession)
    {
        sai_object_id_t session_id = 0x1000000000000001;
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(session_id), Return(SAI_STATUS_SUCCESS)));

        CreateFlowSyncSession("SYNC_SESSION_1", "", "192.168.1.1", "8080");

        // Verify session was created
        swss::Table state_table(m_dpu_state_db.get(), STATE_DASH_FLOW_SYNC_SESSION_STATE_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(state_table.get("SYNC_SESSION_1", fvs));

        // Verify SAI object deletion is called when handleTimeout is called
        EXPECT_CALL(*mock_sai_dash_flow_api, remove_flow_entry_bulk_get_session)
            .Times(1)
            .WillOnce(Return(SAI_STATUS_SUCCESS));

        // Call handleTimeout via handleTimerExpired
        m_dashHaFlowOrch->handleTimerExpired(m_dashHaFlowOrch->getSyncTimer());

        // Verify state DB shows failed state
        ASSERT_TRUE(state_table.get("SYNC_SESSION_1", fvs));
        bool found_failed = false;
        for (const auto &fv : fvs)
        {
            if (fvField(fv) == "state" && fvValue(fv) == "failed")
            {
                found_failed = true;
                break;
            }
        }
        ASSERT_TRUE(found_failed) << "STATE_DB should have state=failed after handleTimeout";
    }

    TEST_F(DashHaFlowOrchTest, HandleFinishedFlowDumpSession)
    {
        sai_object_id_t filter_id = 0x2000000000000001;
        sai_object_id_t session_id = 0x1000000000000001;
        
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session_filter)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(filter_id), Return(SAI_STATUS_SUCCESS)));

        CreateFlowDumpFilter("FILTER_1", "eni_addr", "equal_to", "00:11:22:33:44:55");

        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(session_id), Return(SAI_STATUS_SUCCESS)));

        CreateFlowDumpSession("DUMP_SESSION_1", "true", "1000", "300", {"FILTER_1"});

        // Verify session was created
        swss::Table state_table(m_dpu_state_db.get(), STATE_DASH_FLOW_SYNC_SESSION_STATE_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(state_table.get("DUMP_SESSION_1", fvs));

        // Verify SAI object deletion is called when handleFinished is called
        // Note: Filters are user-managed and not deleted when session finishes
        EXPECT_CALL(*mock_sai_dash_flow_api, remove_flow_entry_bulk_get_session)
            .Times(1)
            .WillOnce(Return(SAI_STATUS_SUCCESS));

        // Call handleFinished via handleSessionFinished
        m_dashHaFlowOrch->handleSessionFinished(session_id);

        // Verify state DB shows completed state with output_file
        ASSERT_TRUE(state_table.get("DUMP_SESSION_1", fvs));
        bool found_completed = false;
        bool found_output_file = false;
        for (const auto &fv : fvs)
        {
            if (fvField(fv) == "state" && fvValue(fv) == "completed")
            {
                found_completed = true;
            }
            if (fvField(fv) == "output_file" && !fvValue(fv).empty())
            {
                found_output_file = true;
            }
        }
        ASSERT_TRUE(found_completed) << "STATE_DB should have state=completed after handleFinished";
        ASSERT_TRUE(found_output_file) << "STATE_DB should have output_file field after handleFinished";
    }

    TEST_F(DashHaFlowOrchTest, HandleTimeoutFlowDumpSession)
    {
        sai_object_id_t filter_id = 0x2000000000000001;
        sai_object_id_t session_id = 0x1000000000000001;
        
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session_filter)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(filter_id), Return(SAI_STATUS_SUCCESS)));

        CreateFlowDumpFilter("FILTER_1", "eni_addr", "equal_to", "00:11:22:33:44:55");

        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(session_id), Return(SAI_STATUS_SUCCESS)));

        CreateFlowDumpSession("DUMP_SESSION_1", "true", "1000", "300", {"FILTER_1"});

        // Verify session was created
        swss::Table state_table(m_dpu_state_db.get(), STATE_DASH_FLOW_SYNC_SESSION_STATE_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(state_table.get("DUMP_SESSION_1", fvs));

        // Verify SAI object deletion is called when handleTimeout is called
        // Note: Filters are user-managed and not deleted when session times out
        EXPECT_CALL(*mock_sai_dash_flow_api, remove_flow_entry_bulk_get_session)
            .Times(1)
            .WillOnce(Return(SAI_STATUS_SUCCESS));

        // Call handleTimeout via handleTimerExpired
        m_dashHaFlowOrch->handleTimerExpired(m_dashHaFlowOrch->getDumpTimer());

        // Verify state DB shows failed state
        ASSERT_TRUE(state_table.get("DUMP_SESSION_1", fvs));
        bool found_failed = false;
        for (const auto &fv : fvs)
        {
            if (fvField(fv) == "state" && fvValue(fv) == "failed")
            {
                found_failed = true;
                break;
            }
        }
        ASSERT_TRUE(found_failed) << "STATE_DB should have state=failed after handleTimeout";
    }

    TEST_F(DashHaFlowOrchTest, CreateBothFlowDumpAndBulkSyncSessions)
    {
        // Create flow dump session with filters
        sai_object_id_t filter_id = 0x2000000000000001;
        sai_object_id_t dump_session_id = 0x1000000000000001;
        
        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session_filter)
            .Times(1)
            .WillOnce(DoAll(SetArgPointee<0>(filter_id), Return(SAI_STATUS_SUCCESS)));

        CreateFlowDumpFilter("FILTER_1", "eni_addr", "equal_to", "00:11:22:33:44:55");

        EXPECT_CALL(*mock_sai_dash_flow_api, create_flow_entry_bulk_get_session)
            .Times(2)
            .WillOnce(DoAll(SetArgPointee<0>(dump_session_id), Return(SAI_STATUS_SUCCESS)))
            .WillOnce(DoAll(SetArgPointee<0>(0x1000000000000002), Return(SAI_STATUS_SUCCESS)));

        CreateFlowDumpSession("DUMP_SESSION_1", "true", "1000", "300", {"FILTER_1"});

        // Create bulk sync session
        CreateFlowSyncSession("SYNC_SESSION_1", "", "192.168.1.1", "8080");

        // Verify both sessions are created successfully in STATE_DB
        swss::Table state_table(m_dpu_state_db.get(), STATE_DASH_FLOW_SYNC_SESSION_STATE_TABLE_NAME);
        vector<FieldValueTuple> fvs;

        // Verify flow dump session
        ASSERT_TRUE(state_table.get("DUMP_SESSION_1", fvs)) << "Flow dump session should exist in STATE_DB";
        bool found_dump_created = false;
        bool found_dump_type = false;
        for (const auto &fv : fvs)
        {
            if (fvField(fv) == "state" && fvValue(fv) == "created")
            {
                found_dump_created = true;
            }
            if (fvField(fv) == "type" && fvValue(fv) == DashHaFlowOrch::SESSION_TYPE_FLOW_DUMP)
            {
                found_dump_type = true;
            }
        }
        ASSERT_TRUE(found_dump_created) << "Flow dump session should have state=created";
        ASSERT_TRUE(found_dump_type) << "Flow dump session should have type=flow_dump";

        // Verify bulk sync session
        ASSERT_TRUE(state_table.get("SYNC_SESSION_1", fvs)) << "Bulk sync session should exist in STATE_DB";
        bool found_sync_created = false;
        bool found_sync_type = false;
        for (const auto &fv : fvs)
        {
            if (fvField(fv) == "state" && fvValue(fv) == "created")
            {
                found_sync_created = true;
            }
            if (fvField(fv) == "type" && fvValue(fv) == DashHaFlowOrch::SESSION_TYPE_BULK_SYNC)
            {
                found_sync_type = true;
            }
        }
        ASSERT_TRUE(found_sync_created) << "Bulk sync session should have state=created";
        ASSERT_TRUE(found_sync_type) << "Bulk sync session should have type=bulk_sync";
    }
}

