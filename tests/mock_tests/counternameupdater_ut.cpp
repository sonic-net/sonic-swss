#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include <gtest/gtest.h>

#define private public
#include "high_frequency_telemetry/counternameupdater.h"
#undef private

extern HFTelOrch *gHFTOrch;

namespace counternameupdater_test
{
    using namespace std;
    using namespace swss;

    struct CounterNameMapUpdaterTest : public ::testing::Test
    {
        shared_ptr<DBConnector> m_counters_db;
        shared_ptr<Table> m_counters_queue_name_map_table;
        shared_ptr<Table> m_counters_pg_name_map_table;

        CounterNameMapUpdaterTest()
        {
        }

        void SetUp() override
        {
            // Initialize database connectors
            // Use the string constructor to get the correct dbId from database_config.json
            m_counters_db = make_shared<DBConnector>("COUNTERS_DB", 0, true);
            m_counters_queue_name_map_table = make_shared<Table>(m_counters_db.get(), "COUNTERS_QUEUE_NAME_MAP");
            m_counters_pg_name_map_table = make_shared<Table>(m_counters_db.get(), "COUNTERS_PG_NAME_MAP");

            // Clear tables
            m_counters_queue_name_map_table->del("");
            m_counters_pg_name_map_table->del("");
        }

        void TearDown() override
        {
            // Clean up
            m_counters_queue_name_map_table->del("");
            m_counters_pg_name_map_table->del("");
        }
    };

    // Test that setCounterNameMap works without HFT support (gHFTOrch == nullptr)
    TEST_F(CounterNameMapUpdaterTest, SetCounterNameMapWithoutHFT)
    {
        // Ensure gHFTOrch is nullptr to simulate platform without HFT support
        HFTelOrch *saved_gHFTOrch = gHFTOrch;
        gHFTOrch = nullptr;

        cout << "Testing QUEUE counter maps without HFT support (gHFTOrch=" << (void*)gHFTOrch << ")" << endl;

        // Create CounterNameMapUpdater for QUEUE
        CounterNameMapUpdater queue_updater("COUNTERS_DB", "COUNTERS_QUEUE_NAME_MAP");

        // Set counter maps one by one using numeric OIDs
        cout << "Calling setCounterNameMap with 3 entries..." << endl;
        queue_updater.setCounterNameMap("Ethernet0:0", 0x1500000000001ULL);
        queue_updater.setCounterNameMap("Ethernet0:1", 0x1500000000002ULL);
        queue_updater.setCounterNameMap("Ethernet0:2", 0x1500000000003ULL);

        cout << "Verifying entries were written to COUNTERS_DB..." << endl;

        // Verify that the counter names were written to COUNTERS_DB
        string value;
        bool result;

        result = m_counters_queue_name_map_table->hget("", "Ethernet0:0", value);
        cout << "  Ethernet0:0 -> " << (result ? value : "NOT FOUND") << endl;
        ASSERT_TRUE(result);
        ASSERT_EQ(value, "oid:0x1500000000001");

        result = m_counters_queue_name_map_table->hget("", "Ethernet0:1", value);
        cout << "  Ethernet0:1 -> " << (result ? value : "NOT FOUND") << endl;
        ASSERT_TRUE(result);
        ASSERT_EQ(value, "oid:0x1500000000002");

        result = m_counters_queue_name_map_table->hget("", "Ethernet0:2", value);
        cout << "  Ethernet0:2 -> " << (result ? value : "NOT FOUND") << endl;
        ASSERT_TRUE(result);
        ASSERT_EQ(value, "oid:0x1500000000003");

        cout << "All QUEUE counter map entries verified successfully!" << endl;

        // Restore gHFTOrch
        gHFTOrch = saved_gHFTOrch;
    }

    // Test single counter name map set
    TEST_F(CounterNameMapUpdaterTest, SetSingleCounterNameMap)
    {
        // Ensure gHFTOrch is nullptr
        HFTelOrch *saved_gHFTOrch = gHFTOrch;
        gHFTOrch = nullptr;

        CounterNameMapUpdater queue_updater("COUNTERS_DB", "COUNTERS_QUEUE_NAME_MAP");

        // Set single counter name map
        sai_object_id_t oid = 0x1500000000001;
        queue_updater.setCounterNameMap("Ethernet0:0", oid);

        // Verify
        string value;
        bool result = m_counters_queue_name_map_table->hget("", "Ethernet0:0", value);
        ASSERT_TRUE(result);
        ASSERT_EQ(value, "oid:0x1500000000001");

        // Restore gHFTOrch
        gHFTOrch = saved_gHFTOrch;
    }

    // Test delCounterNameMap removes the entry from DB
    TEST_F(CounterNameMapUpdaterTest, DelCounterNameMap)
    {
        HFTelOrch *saved_gHFTOrch = gHFTOrch;
        gHFTOrch = nullptr;

        CounterNameMapUpdater queue_updater("COUNTERS_DB", "COUNTERS_QUEUE_NAME_MAP");

        // Set then delete
        queue_updater.setCounterNameMap("Ethernet0:5", 0x1500000000005ULL);

        string value;
        ASSERT_TRUE(m_counters_queue_name_map_table->hget("", "Ethernet0:5", value));

        queue_updater.delCounterNameMap("Ethernet0:5");

        ASSERT_FALSE(m_counters_queue_name_map_table->hget("", "Ethernet0:5", value));

        gHFTOrch = saved_gHFTOrch;
    }

    // Test batch setCounterNameMap with FieldValueTuple vector
    TEST_F(CounterNameMapUpdaterTest, SetCounterNameMapBatch)
    {
        HFTelOrch *saved_gHFTOrch = gHFTOrch;
        gHFTOrch = nullptr;

        CounterNameMapUpdater pg_updater("COUNTERS_DB", "COUNTERS_PG_NAME_MAP");

        vector<FieldValueTuple> batch = {
            {"Ethernet0|3", "oid:0x1a00000000001"},
            {"Ethernet0|4", "oid:0x1a00000000002"},
        };
        pg_updater.setCounterNameMap(batch);

        string value;
        ASSERT_TRUE(m_counters_pg_name_map_table->hget("", "Ethernet0|3", value));
        ASSERT_EQ(value, "oid:0x1a00000000001");
        ASSERT_TRUE(m_counters_pg_name_map_table->hget("", "Ethernet0|4", value));
        ASSERT_EQ(value, "oid:0x1a00000000002");

        gHFTOrch = saved_gHFTOrch;
    }

    // Test unify_counter_name replaces ':' with '|'
    TEST_F(CounterNameMapUpdaterTest, UnifyCounterName)
    {
        CounterNameMapUpdater updater("COUNTERS_DB", "COUNTERS_QUEUE_NAME_MAP");

        // ':' separator should be replaced with '|'
        ASSERT_EQ(updater.unify_counter_name("Ethernet0:3"), "Ethernet0|3");

        // No ':' should return unchanged
        ASSERT_EQ(updater.unify_counter_name("Ethernet0"), "Ethernet0");

        // Only last ':' should be replaced
        ASSERT_EQ(updater.unify_counter_name("a:b:c"), "a:b|c");
    }

    // Test Message struct uses owned strings (no dangling pointers)
    TEST_F(CounterNameMapUpdaterTest, MessageStructOwnsStrings)
    {
        CounterNameMapUpdater::Message msg;
        msg.m_table_name = "COUNTERS_PORT_NAME_MAP";
        msg.m_operation = CounterNameMapUpdater::SET;
        msg.m_counter_name = "Ethernet0";
        msg.m_oid = 0x1000000000001ULL;

        // Verify message fields are accessible (owned, not dangling)
        ASSERT_EQ(msg.m_table_name, "COUNTERS_PORT_NAME_MAP");
        ASSERT_EQ(msg.m_counter_name, "Ethernet0");
        ASSERT_EQ(msg.m_oid, 0x1000000000001ULL);
        ASSERT_EQ(msg.m_operation, CounterNameMapUpdater::SET);

        // DEL message
        CounterNameMapUpdater::Message del_msg;
        del_msg.m_table_name = "COUNTERS_QUEUE_NAME_MAP";
        del_msg.m_operation = CounterNameMapUpdater::DEL;
        del_msg.m_counter_name = "Ethernet0|0";

        ASSERT_EQ(del_msg.m_operation, CounterNameMapUpdater::DEL);
        ASSERT_EQ(del_msg.m_oid, SAI_NULL_OBJECT_ID);
    }
}

