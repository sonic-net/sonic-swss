#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"

#include <sstream>

extern PortsOrch *gPortsOrch;

namespace consumer_test
{
    using namespace std;

    class TestOrch : public Orch
    {
    public:
        TestOrch(swss::DBConnector *db, string tableName)
            :Orch(db, tableName),
            m_notification_count(0)
        {
        }

        void doTask(Consumer& consumer)
        {
            std::cout << "TestOrch::doTask " << consumer.m_toSync.size() << std::endl;
            m_notification_count += consumer.m_toSync.size();
            consumer.m_toSync.clear();
        }

        long m_notification_count;
    };

    enum class ThrowType
    {
        None,
        InvalidArgument,
        LogicError,
        RuntimeError,
        UnknownException
    };

    class ThrowingOrch : public Orch
    {
    public:
        ThrowingOrch(swss::DBConnector *db, string tableName)
            :Orch(db, tableName),
            m_throwType(ThrowType::None),
            m_doTaskCallCount(0)
        {
        }

        void doTask(Consumer& consumer)
        {
            m_doTaskCallCount++;
            switch (m_throwType)
            {
                case ThrowType::InvalidArgument:
                    throw std::invalid_argument("test invalid argument");
                case ThrowType::LogicError:
                    throw std::logic_error("test logic error");
                case ThrowType::RuntimeError:
                    throw std::runtime_error("test runtime error");
                case ThrowType::UnknownException:
                    throw 42;
                case ThrowType::None:
                default:
                    consumer.m_toSync.clear();
                    break;
            }
        }

        ThrowType m_throwType;
        int m_doTaskCallCount;
    };

    struct ConsumerTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;

        string key = "key";
        string f1 = "field1";
        string v1a = "value1_a";
        string v1b = "value1_b";
        string f2 = "field2";
        string v2a = "value2_a";
        string v2b = "value2_b";
        string f3 = "field3";
        string v3a = "value3_a";
        KeyOpFieldsValuesTuple exp_kofv;

        unique_ptr<Consumer> consumer;
        deque <KeyOpFieldsValuesTuple> kofv_q;

        ConsumerTest()
        {
            // FIXME: move out from constructor
            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_config_db.get(), "CFG_TEST_TABLE", 1, 1), gPortsOrch, "CFG_TEST_TABLE"));
        }

        virtual void SetUp() override
        {
            ::testing_db::reset();
        }

        virtual void TearDown() override
        {
            ::testing_db::reset();
        }

        void validate_syncmap(SyncMap &sync, uint16_t exp_sz, std::string exp_key, KeyOpFieldsValuesTuple exp_kofv)
        {
            // verify the content in syncMap
            ASSERT_EQ(sync.size(), exp_sz);
            auto it = sync.begin();
            while (it != sync.end())
            {
                KeyOpFieldsValuesTuple t = it->second;

                string itkey = kfvKey(t);
                if (itkey == exp_key) {
                    ASSERT_EQ(t, exp_kofv);
                    it = sync.erase(it);
                    break;
                } else {
                    it++;
                }
            }
            ASSERT_EQ(sync.size(), exp_sz-1);
        }
    };

    TEST_F(ConsumerTest, ConsumerAddToSync_Set)
    {

        // Test case, one set_command
        auto entry = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        kofv_q.push_back(entry);
        consumer->addToSync(kofv_q);
        exp_kofv = entry;
        validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);
    }

    TEST_F(ConsumerTest, ConsumerAddToSync_Del)
    {
        // Test case, one with del_command
        auto entry = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        kofv_q.push_back(entry);
        consumer->addToSync(kofv_q);

        exp_kofv = entry;
        validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);

    }

    TEST_F(ConsumerTest, ConsumerAddToSync_Set_Del)
    {
        // Test case, add SET then DEL
        auto entrya = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        auto entryb = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        kofv_q.push_back(entrya);
        kofv_q.push_back(entryb);
        consumer->addToSync(kofv_q);

        // expect only DEL
        exp_kofv = entryb;
        validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);
    }

    TEST_F(ConsumerTest, ConsumerAddToSync_Del_Set)
    {
        auto entrya = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        auto entryb = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        // Test case, add DEL then SET, re-try 100 times, order should be kept
        for (auto x = 0; x < 100; x++)
        {
            kofv_q.push_back(entrya);
            kofv_q.push_back(entryb);
            consumer->addToSync(kofv_q);

            // expect DEL then SET
            exp_kofv = entrya;
            validate_syncmap(consumer->m_toSync, 2, key, exp_kofv);

            exp_kofv = entryb;
            validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);
        }
    }

    TEST_F(ConsumerTest, ConsumerAddToSync_Set_Del_Set_Multi)
    {
        // Test5, add SET, DEL then SET, re-try 100 times , order should be kept
        auto entrya = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        auto entryb = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        auto entryc = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        for (auto x = 0; x < 100; x++)
        {
            kofv_q.push_back(entrya);
            kofv_q.push_back(entryb);
            kofv_q.push_back(entryc);
            consumer->addToSync(kofv_q);

            // expect DEL then SET
            exp_kofv = entryb;
            validate_syncmap(consumer->m_toSync, 2, key, exp_kofv);

            exp_kofv = entryc;
            validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);
        }
    }

    TEST_F(ConsumerTest, ConsumerAddToSync_Set_Del_Set_Multi_In_Q)
    {
        // Test5, add SET, DEL then SET, repeat 100 times in queue, final result and order should be kept
        auto entrya = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        auto entryb = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        auto entryc = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        for (auto x = 0; x < 100; x++)
        {
            kofv_q.push_back(entrya);
            kofv_q.push_back(entryb);
            kofv_q.push_back(entryc);
        }
        consumer->addToSync(kofv_q);

        // expect DEL then SET
        exp_kofv = entryb;
        validate_syncmap(consumer->m_toSync, 2, key, exp_kofv);

        exp_kofv = entryc;
        validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);
    }

    TEST_F(ConsumerTest, ConsumerAddToSync_Del_Set_Setnew)
    {
        // Test case, DEL, SET, then SET with different value
        auto entrya = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        auto entryb = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        auto entryc = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1b },
                    { f2, v2b } } });

        kofv_q.push_back(entrya);
        kofv_q.push_back(entryb);
        kofv_q.push_back(entryc);
        consumer->addToSync(kofv_q);

        // expect DEL then SET with new values
        exp_kofv = entrya;
        validate_syncmap(consumer->m_toSync, 2, key, exp_kofv);

        exp_kofv = entryc;
        validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);
    }

    TEST_F(ConsumerTest, ConsumerAddToSync_Del_Set_Setnew1)
    {
        // Test case, DEL, SET, then SET with new values and new fields
        auto entrya = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        auto entryb = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        auto entryc = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1b },
                    { f3, v3a } } });

        kofv_q.push_back(entrya);
        kofv_q.push_back(entryb);
        kofv_q.push_back(entryc);
        consumer->addToSync(kofv_q);

        // expect DEL then SET with new values and new fields
        exp_kofv = entrya;
        validate_syncmap(consumer->m_toSync, 2, key, exp_kofv);

        exp_kofv = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f2, v2a },
                    { f1, v1b },
                    { f3, v3a } } });

        validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);
    }

    TEST_F(ConsumerTest, ConsumerAddToSync_Ind_Set_Del)
    {
        // Test case,  Add individuals by addToSync, SET then DEL
        auto entrya = KeyOpFieldsValuesTuple(
            { key,
                SET_COMMAND,
                { { f1, v1a },
                    { f2, v2a } } });

        auto entryb = KeyOpFieldsValuesTuple(
            { key,
                DEL_COMMAND,
                { { } } });

        consumer->addToSync(entrya);
        consumer->addToSync(entryb);

        // expect only DEL
        exp_kofv = entryb;
        validate_syncmap(consumer->m_toSync, 1, key, exp_kofv);

    }

    TEST_F(ConsumerTest, ConsumerPops_notification_count)
    {
        int consumer_pops_batch_size = 10;
        TestOrch test_orch(m_config_db.get(), "CFG_TEST_TABLE");
        Consumer test_consumer(
                new swss::ConsumerStateTable(m_config_db.get(), "CFG_TEST_TABLE", consumer_pops_batch_size, 1), &test_orch, "CFG_TEST_TABLE");
        swss::ProducerStateTable producer_table(m_config_db.get(), "CFG_TEST_TABLE");

        m_config_db->flushdb();
        for (int notification_count = 0; notification_count< consumer_pops_batch_size*2; notification_count++)
        {
            std::vector<FieldValueTuple> fields;
            FieldValueTuple t("test_field", "test_value");
            fields.push_back(t);
            producer_table.set(std::to_string(notification_count), fields);
            
            cout << "ConsumerPops_notification_count:: add key: " << notification_count << endl;
        }

        // consumer should pops consumer_pops_batch_size notifications 
        test_consumer.execute();
        ASSERT_EQ(test_orch.m_notification_count, consumer_pops_batch_size);

        test_consumer.execute();
        ASSERT_EQ(test_orch.m_notification_count, consumer_pops_batch_size*2);
    }

    /*
     * Exception handling tests for Consumer::drain() and Orch::doTask()
     *
     * These tests verify that exceptions thrown inside doTask(Consumer&)
     * are caught gracefully and do not crash the process.
     */

    struct ExceptionHandlingTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        unique_ptr<ThrowingOrch> m_orch;

        virtual void SetUp() override
        {
            ::testing_db::reset();
            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_orch = make_unique<ThrowingOrch>(m_app_db.get(), "APP_TEST_TABLE");
        }

        virtual void TearDown() override
        {
            m_orch.reset();
            ::testing_db::reset();
        }

        void populateConsumer(Consumer &consumer, int count = 1)
        {
            deque<KeyOpFieldsValuesTuple> entries;
            for (int i = 0; i < count; i++)
            {
                entries.push_back({"key" + to_string(i), SET_COMMAND, {{"field", "value"}}});
            }
            consumer.addToSync(entries);
        }
    };

    TEST_F(ExceptionHandlingTest, DrainCatchesInvalidArgument)
    {
        auto *executor = m_orch->getExecutor("APP_TEST_TABLE");
        auto *consumer = dynamic_cast<Consumer *>(executor);
        ASSERT_NE(consumer, nullptr);

        populateConsumer(*consumer);
        ASSERT_FALSE(consumer->m_toSync.empty());

        m_orch->m_throwType = ThrowType::InvalidArgument;

        // drain() should catch the exception and not crash
        ASSERT_NO_THROW(consumer->drain());
        ASSERT_EQ(m_orch->m_doTaskCallCount, 1);

        // m_toSync is not cleared because the exception prevented it
        ASSERT_FALSE(consumer->m_toSync.empty());
    }

    TEST_F(ExceptionHandlingTest, DrainCatchesLogicError)
    {
        auto *consumer = dynamic_cast<Consumer *>(m_orch->getExecutor("APP_TEST_TABLE"));
        ASSERT_NE(consumer, nullptr);

        populateConsumer(*consumer);
        m_orch->m_throwType = ThrowType::LogicError;

        ASSERT_NO_THROW(consumer->drain());
        ASSERT_EQ(m_orch->m_doTaskCallCount, 1);
        ASSERT_FALSE(consumer->m_toSync.empty());
    }

    TEST_F(ExceptionHandlingTest, DrainCatchesRuntimeError)
    {
        auto *consumer = dynamic_cast<Consumer *>(m_orch->getExecutor("APP_TEST_TABLE"));
        ASSERT_NE(consumer, nullptr);

        populateConsumer(*consumer);
        m_orch->m_throwType = ThrowType::RuntimeError;

        ASSERT_NO_THROW(consumer->drain());
        ASSERT_EQ(m_orch->m_doTaskCallCount, 1);
        ASSERT_FALSE(consumer->m_toSync.empty());
    }

    TEST_F(ExceptionHandlingTest, DrainCatchesUnknownException)
    {
        auto *consumer = dynamic_cast<Consumer *>(m_orch->getExecutor("APP_TEST_TABLE"));
        ASSERT_NE(consumer, nullptr);

        populateConsumer(*consumer);
        m_orch->m_throwType = ThrowType::UnknownException;

        ASSERT_NO_THROW(consumer->drain());
        ASSERT_EQ(m_orch->m_doTaskCallCount, 1);
        ASSERT_FALSE(consumer->m_toSync.empty());
    }

    TEST_F(ExceptionHandlingTest, DrainNoExceptionClearsSync)
    {
        auto *consumer = dynamic_cast<Consumer *>(m_orch->getExecutor("APP_TEST_TABLE"));
        ASSERT_NE(consumer, nullptr);

        populateConsumer(*consumer, 3);
        m_orch->m_throwType = ThrowType::None;

        ASSERT_NO_THROW(consumer->drain());
        ASSERT_EQ(m_orch->m_doTaskCallCount, 1);

        // Normal path: doTask clears m_toSync
        ASSERT_TRUE(consumer->m_toSync.empty());
    }

    TEST_F(ExceptionHandlingTest, OrchDoTaskCatchesExceptionPerConsumer)
    {
        auto *consumer = dynamic_cast<Consumer *>(m_orch->getExecutor("APP_TEST_TABLE"));
        ASSERT_NE(consumer, nullptr);

        populateConsumer(*consumer, 2);
        m_orch->m_throwType = ThrowType::RuntimeError;

        // Orch::doTask() (no-arg) iterates consumers and should not crash
        ASSERT_NO_THROW(static_cast<Orch *>(m_orch.get())->doTask());
        ASSERT_EQ(m_orch->m_doTaskCallCount, 1);
    }

    TEST_F(ExceptionHandlingTest, DrainRecoveryAfterException)
    {
        auto *consumer = dynamic_cast<Consumer *>(m_orch->getExecutor("APP_TEST_TABLE"));
        ASSERT_NE(consumer, nullptr);

        populateConsumer(*consumer);

        // First call throws
        m_orch->m_throwType = ThrowType::RuntimeError;
        ASSERT_NO_THROW(consumer->drain());
        ASSERT_EQ(m_orch->m_doTaskCallCount, 1);
        ASSERT_FALSE(consumer->m_toSync.empty());

        // Second call succeeds — orch recovers and processes tasks
        m_orch->m_throwType = ThrowType::None;
        ASSERT_NO_THROW(consumer->drain());
        ASSERT_EQ(m_orch->m_doTaskCallCount, 2);
        ASSERT_TRUE(consumer->m_toSync.empty());
    }
}
