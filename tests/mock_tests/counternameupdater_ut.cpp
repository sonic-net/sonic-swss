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
            m_counters_db = make_shared<DBConnector>("COUNTERS_DB", 0);
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

    // Test that setCounterNameMap works for Priority Groups without HFT support
    TEST_F(CounterNameMapUpdaterTest, SetPriorityGroupMapWithoutHFT)
    {
        // Ensure gHFTOrch is nullptr to simulate platform without HFT support
        HFTelOrch *saved_gHFTOrch = gHFTOrch;
        gHFTOrch = nullptr;

        // Create CounterNameMapUpdater for Priority Groups
        CounterNameMapUpdater pg_updater("COUNTERS_DB", "COUNTERS_PG_NAME_MAP");

        // Create test data - vector of PG counter name maps
        // Use sai_serialize_object_id to create properly formatted OID strings
        vector<FieldValueTuple> pg_maps = {
            {"Ethernet0:0", sai_serialize_object_id(0x1a00000000001)},
            {"Ethernet0:1", sai_serialize_object_id(0x1a00000000002)},
            {"Ethernet4:0", sai_serialize_object_id(0x1a00000000003)},
        };

        // Call setCounterNameMap with vector - this should work even without HFT
        pg_updater.setCounterNameMap(pg_maps);

        // Verify that the counter names were written to COUNTERS_DB
        string value;
        bool result;

        result = m_counters_pg_name_map_table->hget("", "Ethernet0:0", value);
        ASSERT_TRUE(result);
        ASSERT_EQ(value, "oid:0x1a00000000001");

        result = m_counters_pg_name_map_table->hget("", "Ethernet0:1", value);
        ASSERT_TRUE(result);
        ASSERT_EQ(value, "oid:0x1a00000000002");

        result = m_counters_pg_name_map_table->hget("", "Ethernet4:0", value);
        ASSERT_TRUE(result);
        ASSERT_EQ(value, "oid:0x1a00000000003");

        // Restore gHFTOrch
        gHFTOrch = saved_gHFTOrch;
    }

    // Test that setCounterNameMap handles empty OID values
    TEST_F(CounterNameMapUpdaterTest, SetCounterNameMapWithEmptyOID)
    {
        // Ensure gHFTOrch is nullptr
        HFTelOrch *saved_gHFTOrch = gHFTOrch;
        gHFTOrch = nullptr;

        CounterNameMapUpdater queue_updater("COUNTERS_DB", "COUNTERS_QUEUE_NAME_MAP");

        // Create test data with empty OID value
        // Use sai_serialize_object_id to create properly formatted OID strings
        vector<FieldValueTuple> queue_maps = {
            {"Ethernet0:0", sai_serialize_object_id(0x1500000000001)},
            {"Ethernet0:1", ""},  // Empty OID
            {"Ethernet0:2", sai_serialize_object_id(0x1500000000003)},
        };

        // Call setCounterNameMap - should handle empty OID gracefully
        queue_updater.setCounterNameMap(queue_maps);

        // Verify that entries were written
        string value;
        bool result;

        result = m_counters_queue_name_map_table->hget("", "Ethernet0:0", value);
        ASSERT_TRUE(result);
        ASSERT_EQ(value, "oid:0x1500000000001");

        // Empty OID should be written as "oid:0x0"
        result = m_counters_queue_name_map_table->hget("", "Ethernet0:1", value);
        ASSERT_TRUE(result);
        ASSERT_EQ(value, "oid:0x0");

        result = m_counters_queue_name_map_table->hget("", "Ethernet0:2", value);
        ASSERT_TRUE(result);
        ASSERT_EQ(value, "oid:0x1500000000003");

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
}

