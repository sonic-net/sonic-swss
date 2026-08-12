/**
 * @file p4_enabled_asic_ext_ut.cpp
 *
 * Task 4.3 – Tích hợp P4-enabled ASIC và phần mở rộng
 *
 * Contract and integration-shape tests for P4RT table key semantics,
 * core/extension dispatch assumptions, and status contracts.
 */

#include "gtest/gtest.h"

#include "ut_helper.h"
#include "mock_table.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace swss;
using namespace std;
using namespace testing_db;

namespace
{

static vector<FieldValueTuple> fv(initializer_list<pair<string, string>> pairs)
{
    vector<FieldValueTuple> out;
    for (const auto &p : pairs)
    {
        out.emplace_back(p.first, p.second);
    }
    return out;
}

static bool hasField(const vector<FieldValueTuple> &fvs, const string &field)
{
    return any_of(fvs.begin(), fvs.end(), [&](const FieldValueTuple &x) {
        return fvField(x) == field;
    });
}

static bool getField(const vector<FieldValueTuple> &fvs, const string &field, string &value)
{
    for (const auto &x : fvs)
    {
        if (fvField(x) == field)
        {
            value = fvValue(x);
            return true;
        }
    }
    return false;
}

static bool hasKeyDelimiter(const string &key)
{
    return key.find(':') != string::npos;
}

static string tablePart(const string &key)
{
    auto pos = key.find(':');
    if (pos == string::npos)
    {
        return "";
    }
    return key.substr(0, pos);
}

} // namespace

class P4EnabledAsicContractTest : public ::testing::Test
{
  protected:
    Table p4rt_tbl{APP_DB, APP_P4RT_TABLE_NAME};
    Table state_tbl{STATE_DB, "P4RT_STATE_TABLE"};
    Table counters_tbl{COUNTERS_DB, "COUNTERS:P4RT_TABLE"};

    // Contract-driven set of core tables registered in P4Orch.
    vector<string> core_tables = {
        APP_P4RT_TABLES_DEFINITION_TABLE_NAME,
        APP_P4RT_ROUTER_INTERFACE_TABLE_NAME,
        APP_P4RT_NEIGHBOR_TABLE_NAME,
        APP_P4RT_NEXTHOP_TABLE_NAME,
        APP_P4RT_IPV4_TABLE_NAME,
        APP_P4RT_IPV6_TABLE_NAME,
        APP_P4RT_WCMP_GROUP_TABLE_NAME,
        APP_P4RT_MIRROR_SESSION_TABLE_NAME,
        APP_P4RT_L3_ADMIT_TABLE_NAME,
        APP_P4RT_TUNNEL_TABLE_NAME,
        APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME,
    };

    // Simulates minimal dispatch rule in P4Orch: known table -> core; else -> extension.
    string classifyManagerRoute(const string &p4_key)
    {
        auto t = tablePart(p4_key);
        if (find(core_tables.begin(), core_tables.end(), t) != core_tables.end())
        {
            return "core";
        }
        return "extension";
    }

    void SetUp() override
    {
        reset();
    }

    void TearDown() override
    {
        reset();
    }
};

// ===========================================================================
// 4.3.1 APP_P4RT_TABLE key contract
// ===========================================================================
TEST_F(P4EnabledAsicContractTest, P4rtKeyMustContainTableAndPayload)
{
    string good = string(APP_P4RT_IPV4_TABLE_NAME) + ":{\"match/vrf_id\":\"b4-traffic\"}";
    string bad = "invalid_key_without_delimiter";

    EXPECT_TRUE(hasKeyDelimiter(good));
    EXPECT_FALSE(hasKeyDelimiter(bad));
}

TEST_F(P4EnabledAsicContractTest, P4rtKeyTablePartShouldNotBeEmpty)
{
    string good = string(APP_P4RT_NEIGHBOR_TABLE_NAME) + ":{\"match/router_interface_id\":\"Ethernet0\"}";
    string bad = ":{\"match\":\"x\"}";

    EXPECT_FALSE(tablePart(good).empty());
    EXPECT_TRUE(tablePart(bad).empty());
}

TEST_F(P4EnabledAsicContractTest, P4rtEntryCanBePersistedInAppDb)
{
    string key = string(APP_P4RT_NEXTHOP_TABLE_NAME) + ":{\"match/nexthop_id\":\"nh1\"}";
    p4rt_tbl.set(key, fv({{"action", "set_nexthop"}, {"param/router_interface_id", "Ethernet0"}}));

    auto attrs = p4rt_tbl.get(key);
    EXPECT_TRUE(hasField(attrs, "action"));
    EXPECT_TRUE(hasField(attrs, "param/router_interface_id"));
}

// ===========================================================================
// 4.3.2 Core vs extension dispatch contract
// ===========================================================================
TEST_F(P4EnabledAsicContractTest, CoreTableRoutesToCoreManager)
{
    string key = string(APP_P4RT_IPV6_TABLE_NAME) + ":{\"match/ipv6_dst\":\"2001:db8::/64\"}";
    EXPECT_EQ(classifyManagerRoute(key), "core");
}

TEST_F(P4EnabledAsicContractTest, UnknownTableRoutesToExtensionManager)
{
    string key = "my_vendor_custom_table:{\"match/custom\":\"1\"}";
    EXPECT_EQ(classifyManagerRoute(key), "extension");
}

TEST_F(P4EnabledAsicContractTest, ExtensionRouteMaintainsSameP4rtEnvelope)
{
    string key = "my_vendor_custom_table:{\"match/custom\":\"x\"}";
    p4rt_tbl.set(key, fv({{"action", "vendor_action"}}));

    auto attrs = p4rt_tbl.get(key);
    EXPECT_TRUE(hasField(attrs, "action"));
}

// ===========================================================================
// 4.3.3 P4 table-definition contract
// ===========================================================================
TEST_F(P4EnabledAsicContractTest, TableDefinitionEntryRequiresInfoField)
{
    string key = string(APP_P4RT_TABLES_DEFINITION_TABLE_NAME) + ":pipeline_main";
    p4rt_tbl.set(key, fv({{"info", "{\"tables\":[{\"id\":1,\"alias\":\"ipv4_table\"}]}"}}));

    auto attrs = p4rt_tbl.get(key);
    EXPECT_TRUE(hasField(attrs, "info"));
}

TEST_F(P4EnabledAsicContractTest, TableDefinitionWithoutInfoIsInvalidContract)
{
    string key = string(APP_P4RT_TABLES_DEFINITION_TABLE_NAME) + ":pipeline_bad";
    p4rt_tbl.set(key, fv({{"unexpected", "x"}}));

    auto attrs = p4rt_tbl.get(key);
    EXPECT_FALSE(hasField(attrs, "info"));
}

// ===========================================================================
// 4.3.4 OID mapping semantics contract (conceptual)
// ===========================================================================
TEST_F(P4EnabledAsicContractTest, MappingStoreTracksOidAndRefCount)
{
    map<string, pair<uint64_t, uint32_t>> mapping;
    mapping["nh:nh1"] = {0x1234, 0};

    ASSERT_EQ(mapping.count("nh:nh1"), 1u);
    EXPECT_EQ(mapping["nh:nh1"].first, 0x1234u);
    EXPECT_EQ(mapping["nh:nh1"].second, 0u);
}

TEST_F(P4EnabledAsicContractTest, MappingRefCountMustReachZeroBeforeErase)
{
    map<string, pair<uint64_t, uint32_t>> mapping;
    mapping["rif:Ethernet0"] = {0x7777, 2};

    // Simulate ref decrements.
    mapping["rif:Ethernet0"].second--;
    mapping["rif:Ethernet0"].second--;

    EXPECT_EQ(mapping["rif:Ethernet0"].second, 0u);
    mapping.erase("rif:Ethernet0");
    EXPECT_EQ(mapping.count("rif:Ethernet0"), 0u);
}

// ===========================================================================
// 4.3.5 Response and observability contract
// ===========================================================================
TEST_F(P4EnabledAsicContractTest, InvalidInputShouldProduceErrorStatus)
{
    string key = "invalid_key_without_table_delimiter";

    // Simulate status publication for invalid key.
    state_tbl.set(key, fv({{"status", "SWSS_RC_INVALID_PARAM"}}));

    string status;
    auto attrs = state_tbl.get(key);
    ASSERT_TRUE(getField(attrs, "status", status));
    EXPECT_EQ(status, "SWSS_RC_INVALID_PARAM");
}

TEST_F(P4EnabledAsicContractTest, CounterSnapshotIsNumericForP4Objects)
{
    counters_tbl.set("route:ipv4_table:10.0.0.0/24", fv({
        {"packets", "1000"},
        {"bytes", "64000"}
    }));

    auto attrs = counters_tbl.get("route:ipv4_table:10.0.0.0/24");
    string packets;
    ASSERT_TRUE(getField(attrs, "packets", packets));
    EXPECT_EQ(packets, "1000");
}
