/**
 * @file db_state_handling_ut.cpp
 *
 * Task 2.1 – Thử nghiệm đánh giá chức năng xử lý trạng thái cơ sở dữ liệu
 *
 * Unit tests for the mock database state handling infrastructure used by
 * orchagent unit tests.  The tests cover:
 *   - Table::set / get / del / hset / hget / hdel / getKeys
 *   - ProducerStateTable::set / del (single and batch forms)
 *   - Value-field merge semantics on repeated set() calls
 *   - Multi-database isolation (each DB id is an independent namespace)
 *   - testing_db::reset() clears all state across all databases
 */

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"

#include <memory>
#include <string>
#include <vector>
#include <algorithm>

using namespace swss;
using namespace std;

// ---------------------------------------------------------------------------
// Helper: build a FieldValueTuple vector from an initializer list
// ---------------------------------------------------------------------------
static vector<FieldValueTuple> fvList(
    initializer_list<pair<string, string>> pairs)
{
    vector<FieldValueTuple> result;
    for (auto &p : pairs)
        result.emplace_back(p.first, p.second);
    return result;
}

// ---------------------------------------------------------------------------
// Helper: look up a field inside a FieldValueTuple vector
// ---------------------------------------------------------------------------
static bool findField(const vector<FieldValueTuple> &fvs,
                      const string &field, string &value)
{
    for (const auto &fv : fvs)
    {
        if (fvField(fv) == field)
        {
            value = fvValue(fv);
            return true;
        }
    }
    return false;
}

// ===========================================================================
// Test fixture – resets the mock DB before every test
// ===========================================================================
class DbStateHandlingTest : public ::testing::Test
{
protected:
    shared_ptr<DBConnector> m_app_db;
    shared_ptr<DBConnector> m_config_db;
    shared_ptr<DBConnector> m_state_db;
    shared_ptr<DBConnector> m_asic_db;

    void SetUp() override
    {
        ::testing_db::reset();
        m_app_db    = make_shared<DBConnector>("APPL_DB",   0);
        m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
        m_state_db  = make_shared<DBConnector>("STATE_DB",  0);
        m_asic_db   = make_shared<DBConnector>("ASIC_DB",   0);
    }

    void TearDown() override
    {
        ::testing_db::reset();
    }
};

// ===========================================================================
// 2.1.1  Table::set and Table::get – cơ bản
// ===========================================================================
TEST_F(DbStateHandlingTest, TableSetGet_BasicFields)
{
    Table tbl(m_app_db.get(), "TEST_TABLE");

    auto fvs = fvList({{"field1", "value1"}, {"field2", "value2"}});
    tbl.set("key1", fvs);

    vector<FieldValueTuple> result;
    ASSERT_TRUE(tbl.get("key1", result));
    ASSERT_EQ(result.size(), 2u);

    string v;
    ASSERT_TRUE(findField(result, "field1", v));
    EXPECT_EQ(v, "value1");

    ASSERT_TRUE(findField(result, "field2", v));
    EXPECT_EQ(v, "value2");
}

TEST_F(DbStateHandlingTest, TableGet_NonExistentKey_ReturnsFalse)
{
    Table tbl(m_app_db.get(), "TEST_TABLE");

    vector<FieldValueTuple> result;
    EXPECT_FALSE(tbl.get("does_not_exist", result));
    EXPECT_TRUE(result.empty());
}

// ===========================================================================
// 2.1.2  Table::set merge semantics – trường hợp cập nhật trường hiện có
// ===========================================================================
TEST_F(DbStateHandlingTest, TableSet_MergeUpdatesExistingField)
{
    Table tbl(m_app_db.get(), "TEST_TABLE");

    tbl.set("key1", fvList({{"f1", "v1a"}, {"f2", "v2a"}}));
    // Update f1, leave f2 unchanged
    tbl.set("key1", fvList({{"f1", "v1b"}}));

    vector<FieldValueTuple> result;
    ASSERT_TRUE(tbl.get("key1", result));

    string v;
    ASSERT_TRUE(findField(result, "f1", v));
    EXPECT_EQ(v, "v1b");        // updated

    ASSERT_TRUE(findField(result, "f2", v));
    EXPECT_EQ(v, "v2a");        // preserved
}

TEST_F(DbStateHandlingTest, TableSet_MergeAddsNewField)
{
    Table tbl(m_app_db.get(), "TEST_TABLE");

    tbl.set("key1", fvList({{"f1", "v1"}}));
    tbl.set("key1", fvList({{"f2", "v2"}}));   // new field

    vector<FieldValueTuple> result;
    ASSERT_TRUE(tbl.get("key1", result));
    ASSERT_EQ(result.size(), 2u);

    string v;
    ASSERT_TRUE(findField(result, "f1", v));
    EXPECT_EQ(v, "v1");

    ASSERT_TRUE(findField(result, "f2", v));
    EXPECT_EQ(v, "v2");
}

// ===========================================================================
// 2.1.3  Table::del – xóa khóa
// ===========================================================================
TEST_F(DbStateHandlingTest, TableDel_RemovesKey)
{
    Table tbl(m_app_db.get(), "TEST_TABLE");

    tbl.set("key1", fvList({{"f1", "v1"}}));
    EXPECT_TRUE(tbl.get("key1", *(new vector<FieldValueTuple>)));

    tbl.del("key1");

    vector<FieldValueTuple> result;
    EXPECT_FALSE(tbl.get("key1", result));
}

TEST_F(DbStateHandlingTest, TableDel_NonExistentKey_NoError)
{
    Table tbl(m_app_db.get(), "TEST_TABLE");
    // Must not throw or crash
    EXPECT_NO_THROW(tbl.del("ghost_key"));
}

// ===========================================================================
// 2.1.4  Table::hset / hget – thao tác trường đơn
// ===========================================================================
TEST_F(DbStateHandlingTest, TableHsetHget_BasicField)
{
    Table tbl(m_config_db.get(), "CFG_TABLE");

    tbl.hset("entry1", "status", "active");

    string value;
    ASSERT_TRUE(tbl.hget("entry1", "status", value));
    EXPECT_EQ(value, "active");
}

TEST_F(DbStateHandlingTest, TableHget_UnknownField_ReturnsFalse)
{
    Table tbl(m_config_db.get(), "CFG_TABLE");

    tbl.hset("entry1", "status", "active");

    string value;
    EXPECT_FALSE(tbl.hget("entry1", "nonexistent_field", value));
}

TEST_F(DbStateHandlingTest, TableHget_UnknownKey_ReturnsFalse)
{
    Table tbl(m_config_db.get(), "CFG_TABLE");

    string value;
    EXPECT_FALSE(tbl.hget("no_such_key", "status", value));
}

// ===========================================================================
// 2.1.5  Table::hdel – xóa trường đơn
// ===========================================================================
TEST_F(DbStateHandlingTest, TableHdel_RemovesField)
{
    Table tbl(m_state_db.get(), "STATE_TABLE");

    tbl.set("key1", fvList({{"f1", "v1"}, {"f2", "v2"}}));
    tbl.hdel("key1", "f1");

    vector<FieldValueTuple> result;
    ASSERT_TRUE(tbl.get("key1", result));
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(fvField(result[0]), "f2");
}

TEST_F(DbStateHandlingTest, TableHdel_LastField_RemovesKey)
{
    Table tbl(m_state_db.get(), "STATE_TABLE");

    tbl.set("key1", fvList({{"only_field", "only_value"}}));
    tbl.hdel("key1", "only_field");

    vector<FieldValueTuple> result;
    EXPECT_FALSE(tbl.get("key1", result));
}

// ===========================================================================
// 2.1.6  Table::getKeys – lấy danh sách khóa
// ===========================================================================
TEST_F(DbStateHandlingTest, TableGetKeys_ReturnsAllKeys)
{
    Table tbl(m_app_db.get(), "APP_TABLE");

    tbl.set("key_a", fvList({{"f", "v"}}));
    tbl.set("key_b", fvList({{"f", "v"}}));
    tbl.set("key_c", fvList({{"f", "v"}}));

    vector<string> keys;
    tbl.getKeys(keys);

    ASSERT_EQ(keys.size(), 3u);
    EXPECT_NE(find(keys.begin(), keys.end(), "key_a"), keys.end());
    EXPECT_NE(find(keys.begin(), keys.end(), "key_b"), keys.end());
    EXPECT_NE(find(keys.begin(), keys.end(), "key_c"), keys.end());
}

TEST_F(DbStateHandlingTest, TableGetKeys_EmptyTable_ReturnsNoKeys)
{
    Table tbl(m_app_db.get(), "EMPTY_TABLE");

    vector<string> keys;
    tbl.getKeys(keys);
    EXPECT_TRUE(keys.empty());
}

TEST_F(DbStateHandlingTest, TableGetKeys_AfterDel_KeyAbsent)
{
    Table tbl(m_app_db.get(), "APP_TABLE");

    tbl.set("k1", fvList({{"f", "v"}}));
    tbl.set("k2", fvList({{"f", "v"}}));
    tbl.del("k1");

    vector<string> keys;
    tbl.getKeys(keys);

    ASSERT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys[0], "k2");
}

// ===========================================================================
// 2.1.7  ProducerStateTable::set / del – đơn lẻ
// ===========================================================================
TEST_F(DbStateHandlingTest, ProducerStateTable_SetAndGet)
{
    ProducerStateTable producer(m_app_db.get(), "PROD_TABLE");
    Table reader(m_app_db.get(), "PROD_TABLE");

    producer.set("route1", fvList({{"nexthop", "10.0.0.1"}, {"ifname", "Ethernet0"}}));

    vector<FieldValueTuple> result;
    ASSERT_TRUE(reader.get("route1", result));

    string v;
    ASSERT_TRUE(findField(result, "nexthop", v));
    EXPECT_EQ(v, "10.0.0.1");

    ASSERT_TRUE(findField(result, "ifname", v));
    EXPECT_EQ(v, "Ethernet0");
}

TEST_F(DbStateHandlingTest, ProducerStateTable_Del_RemovesEntry)
{
    ProducerStateTable producer(m_app_db.get(), "PROD_TABLE");
    Table reader(m_app_db.get(), "PROD_TABLE");

    producer.set("route1", fvList({{"nexthop", "10.0.0.1"}}));
    producer.del("route1");

    vector<FieldValueTuple> result;
    EXPECT_FALSE(reader.get("route1", result));
}

// ===========================================================================
// 2.1.8  ProducerStateTable::set(vector) – thao tác theo lô (batch)
// ===========================================================================
TEST_F(DbStateHandlingTest, ProducerStateTable_BatchSet)
{
    ProducerStateTable producer(m_app_db.get(), "ROUTE_TABLE");
    Table reader(m_app_db.get(), "ROUTE_TABLE");

    vector<KeyOpFieldsValuesTuple> batch = {
        {"10.0.0.0/8",  SET_COMMAND, fvList({{"nexthop", "192.168.1.1"}})},
        {"172.16.0.0/12", SET_COMMAND, fvList({{"nexthop", "192.168.1.2"}})},
        {"192.168.0.0/16", SET_COMMAND, fvList({{"nexthop", "192.168.1.3"}})},
    };
    producer.set(batch);

    vector<string> keys;
    reader.getKeys(keys);
    ASSERT_EQ(keys.size(), 3u);

    vector<FieldValueTuple> result;
    ASSERT_TRUE(reader.get("10.0.0.0/8", result));
    string v;
    ASSERT_TRUE(findField(result, "nexthop", v));
    EXPECT_EQ(v, "192.168.1.1");
}

TEST_F(DbStateHandlingTest, ProducerStateTable_BatchSetAndDel)
{
    ProducerStateTable producer(m_app_db.get(), "ROUTE_TABLE");
    Table reader(m_app_db.get(), "ROUTE_TABLE");

    // Add two routes then remove one in a single batch
    vector<KeyOpFieldsValuesTuple> batch = {
        {"10.0.0.0/8",  SET_COMMAND, fvList({{"nexthop", "192.168.1.1"}})},
        {"172.16.0.0/12", SET_COMMAND, fvList({{"nexthop", "192.168.1.2"}})},
        {"10.0.0.0/8",  DEL_COMMAND, {}},
    };
    producer.set(batch);

    vector<FieldValueTuple> result;
    EXPECT_FALSE(reader.get("10.0.0.0/8", result));   // deleted

    ASSERT_TRUE(reader.get("172.16.0.0/12", result));  // still present
    string v;
    ASSERT_TRUE(findField(result, "nexthop", v));
    EXPECT_EQ(v, "192.168.1.2");
}

// ===========================================================================
// 2.1.9  Cách ly đa cơ sở dữ liệu – mỗi DB là không gian độc lập
// ===========================================================================
TEST_F(DbStateHandlingTest, MultiDB_IsolationBetweenDatabases)
{
    Table app_tbl(m_app_db.get(),    "TEST_TABLE");
    Table cfg_tbl(m_config_db.get(), "TEST_TABLE");
    Table state_tbl(m_state_db.get(), "TEST_TABLE");
    Table asic_tbl(m_asic_db.get(),   "TEST_TABLE");

    // Write the same key to different DBs with distinct values
    app_tbl.set("key1",   fvList({{"src", "APP_DB"}}));
    cfg_tbl.set("key1",   fvList({{"src", "CONFIG_DB"}}));
    state_tbl.set("key1", fvList({{"src", "STATE_DB"}}));
    asic_tbl.set("key1",  fvList({{"src", "ASIC_DB"}}));

    auto checkDb = [](Table &tbl, const string &expected) {
        vector<FieldValueTuple> result;
        ASSERT_TRUE(tbl.get("key1", result));
        string v;
        ASSERT_TRUE(findField(result, "src", v));
        EXPECT_EQ(v, expected);
    };

    checkDb(app_tbl,   "APP_DB");
    checkDb(cfg_tbl,   "CONFIG_DB");
    checkDb(state_tbl, "STATE_DB");
    checkDb(asic_tbl,  "ASIC_DB");
}

TEST_F(DbStateHandlingTest, MultiDB_DelInOneDatabaseDoesNotAffectOthers)
{
    Table app_tbl(m_app_db.get(),    "TEST_TABLE");
    Table state_tbl(m_state_db.get(), "TEST_TABLE");

    app_tbl.set("shared_key",   fvList({{"f", "v"}}));
    state_tbl.set("shared_key", fvList({{"f", "v"}}));

    app_tbl.del("shared_key");   // delete only from APP_DB

    vector<FieldValueTuple> result;
    EXPECT_FALSE(app_tbl.get("shared_key", result));
    EXPECT_TRUE(state_tbl.get("shared_key", result));  // STATE_DB unaffected
}

TEST_F(DbStateHandlingTest, MultiDB_DifferentTableNamesScopeData)
{
    // Same DB, different table names → separate namespaces
    Table tbl_a(m_app_db.get(), "TABLE_A");
    Table tbl_b(m_app_db.get(), "TABLE_B");

    tbl_a.set("key1", fvList({{"owner", "A"}}));

    vector<FieldValueTuple> result;
    EXPECT_TRUE(tbl_a.get("key1", result));
    EXPECT_FALSE(tbl_b.get("key1", result));   // TABLE_B has no key1
}

// ===========================================================================
// 2.1.10  testing_db::reset() – xóa toàn bộ trạng thái
// ===========================================================================
TEST_F(DbStateHandlingTest, Reset_ClearsAllDatabases)
{
    Table app_tbl(m_app_db.get(),    "T");
    Table state_tbl(m_state_db.get(), "T");

    app_tbl.set("k",   fvList({{"f", "v"}}));
    state_tbl.set("k", fvList({{"f", "v"}}));

    ::testing_db::reset();   // explicit reset mid-test

    vector<FieldValueTuple> result;
    EXPECT_FALSE(app_tbl.get("k", result));
    EXPECT_FALSE(state_tbl.get("k", result));
}

TEST_F(DbStateHandlingTest, Reset_SubsequentWritesWorkNormally)
{
    Table tbl(m_app_db.get(), "T");

    tbl.set("k", fvList({{"f", "before"}}));
    ::testing_db::reset();
    tbl.set("k", fvList({{"f", "after"}}));

    vector<FieldValueTuple> result;
    ASSERT_TRUE(tbl.get("k", result));
    string v;
    ASSERT_TRUE(findField(result, "f", v));
    EXPECT_EQ(v, "after");
}

// ===========================================================================
// 2.1.11  Luồng trạng thái DB đầu cuối – CONFIG_DB → APP_DB → STATE_DB
// ===========================================================================

/**
 * Simulates the typical SONiC state flow:
 *   1. Operator writes intent to CONFIG_DB.
 *   2. cfgmgr/orchagent translates it into APP_DB.
 *   3. Orchagent reflects operational state in STATE_DB.
 */
TEST_F(DbStateHandlingTest, StateFlow_ConfigToAppToState)
{
    Table cfg_tbl(m_config_db.get(), "INTERFACE");
    Table app_tbl(m_app_db.get(),    "INTF_TABLE");
    Table state_tbl(m_state_db.get(), "INTERFACE_TABLE_STATE");

    // Step 1: operator configures interface
    cfg_tbl.set("Ethernet0", fvList({{"admin_status", "up"}, {"mtu", "9100"}}));

    // Step 2: cfgmgr translates into APP_DB
    vector<FieldValueTuple> cfg_result;
    ASSERT_TRUE(cfg_tbl.get("Ethernet0", cfg_result));
    app_tbl.set("Ethernet0", cfg_result);   // propagate fields

    // Step 3: orchagent sets operational state in STATE_DB
    string admin_val;
    ASSERT_TRUE(app_tbl.hget("Ethernet0", "admin_status", admin_val));
    state_tbl.hset("Ethernet0", "oper_status", admin_val == "up" ? "up" : "down");
    state_tbl.hset("Ethernet0", "vlan", "");

    // Verify STATE_DB reflects expected operational state
    string oper_state;
    ASSERT_TRUE(state_tbl.hget("Ethernet0", "oper_status", oper_state));
    EXPECT_EQ(oper_state, "up");
}

/**
 * Tests that deleting an entry from APP_DB and clearing STATE_DB
 * leaves both databases clean.
 */
TEST_F(DbStateHandlingTest, StateFlow_DeleteCleansAllDatabases)
{
    Table app_tbl(m_app_db.get(),     "ROUTE_TABLE");
    Table state_tbl(m_state_db.get(), "ROUTE_STATE");

    app_tbl.set("10.0.0.0/8",   fvList({{"nexthop", "192.168.0.1"}}));
    state_tbl.set("10.0.0.0/8", fvList({{"status",  "active"}}));

    // Removal propagated through the pipeline
    app_tbl.del("10.0.0.0/8");
    state_tbl.del("10.0.0.0/8");

    vector<FieldValueTuple> result;
    EXPECT_FALSE(app_tbl.get("10.0.0.0/8", result));
    EXPECT_FALSE(state_tbl.get("10.0.0.0/8", result));
}

// ===========================================================================
// 2.1.12  Nhiều khóa trong cùng bảng
// ===========================================================================
TEST_F(DbStateHandlingTest, MultipleKeys_IndependentStorage)
{
    Table tbl(m_app_db.get(), "NEIGH_TABLE");

    tbl.set("192.168.0.1", fvList({{"mac", "aa:bb:cc:dd:ee:01"}, {"dev", "Ethernet0"}}));
    tbl.set("192.168.0.2", fvList({{"mac", "aa:bb:cc:dd:ee:02"}, {"dev", "Ethernet4"}}));
    tbl.set("192.168.0.3", fvList({{"mac", "aa:bb:cc:dd:ee:03"}, {"dev", "Ethernet8"}}));

    // Delete middle entry; others must be unaffected
    tbl.del("192.168.0.2");

    vector<string> keys;
    tbl.getKeys(keys);
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(find(keys.begin(), keys.end(), "192.168.0.2"), keys.end());

    vector<FieldValueTuple> result;
    string v;

    ASSERT_TRUE(tbl.get("192.168.0.1", result));
    ASSERT_TRUE(findField(result, "mac", v));
    EXPECT_EQ(v, "aa:bb:cc:dd:ee:01");

    ASSERT_TRUE(tbl.get("192.168.0.3", result));
    ASSERT_TRUE(findField(result, "mac", v));
    EXPECT_EQ(v, "aa:bb:cc:dd:ee:03");
}
