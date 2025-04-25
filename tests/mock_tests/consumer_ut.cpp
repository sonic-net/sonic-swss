#include "tests/mock_tests/ut_helper.h"
#include "tests/mock_tests/mock_orchagent_main.h"
#include "tests/mock_tests/mock_table.h"

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
}
