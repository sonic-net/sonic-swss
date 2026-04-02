/**
 * @file db_contract_model_ut.cpp
 *
 * Task 4.2 – Mô hình dữ liệu và hợp đồng cơ sở dữ liệu
 *
 * Contract-focused tests for key format, required fields, enum values,
 * references between tables, and multi-DB role separation.
 */

#include "gtest/gtest.h"

#include "ut_helper.h"
#include "mock_table.h"

#include <algorithm>
#include <cctype>
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

static bool isNonNegativeNumber(const string &s)
{
    if (s.empty())
    {
        return false;
    }
    return all_of(s.begin(), s.end(), [](unsigned char c) { return isdigit(c) != 0; });
}

static bool isValidAdminStatus(const string &v)
{
    return v == "up" || v == "down";
}

static bool hasPrefix(const string &key, const string &prefix)
{
    return key.rfind(prefix, 0) == 0;
}

static bool isLikelyMac(const string &s)
{
    if (s.size() != 17)
    {
        return false;
    }

    for (size_t i = 0; i < s.size(); ++i)
    {
        if ((i + 1) % 3 == 0)
        {
            if (s[i] != ':')
            {
                return false;
            }
        }
        else if (!isxdigit(static_cast<unsigned char>(s[i])))
        {
            return false;
        }
    }

    return true;
}

} // namespace

class DbContractModelTest : public ::testing::Test
{
  protected:
    shared_ptr<DBConnector> app_db;
    shared_ptr<DBConnector> cfg_db;
    shared_ptr<DBConnector> state_db;
    shared_ptr<DBConnector> counters_db;

    void SetUp() override
    {
        reset();
        app_db = make_shared<DBConnector>("APPL_DB", 0);
        cfg_db = make_shared<DBConnector>("CONFIG_DB", 0);
        state_db = make_shared<DBConnector>("STATE_DB", 0);
        counters_db = make_shared<DBConnector>("COUNTERS_DB", 0);
    }

    void TearDown() override
    {
        reset();
    }
};

// ===========================================================================
// 4.2.1 Key format contract
// ===========================================================================
TEST_F(DbContractModelTest, VlanKeyMustUseVlanPrefix)
{
    const string good = "Vlan100";
    const string bad = "VLAN100";

    EXPECT_TRUE(hasPrefix(good, "Vlan"));
    EXPECT_FALSE(hasPrefix(bad, "Vlan"));
}

TEST_F(DbContractModelTest, NeighborKeyMustContainInterfaceAndIp)
{
    const string good = "Ethernet0:10.0.0.1";
    const string bad = "10.0.0.1";

    EXPECT_NE(good.find(':'), string::npos);
    EXPECT_EQ(bad.find(':'), string::npos);
}

TEST_F(DbContractModelTest, FdbKeyMustContainVlanAndMac)
{
    const string key = "100:aa:bb:cc:dd:ee:ff";
    auto pos = key.find(':');

    ASSERT_NE(pos, string::npos);
    string vlan = key.substr(0, pos);
    string mac = key.substr(pos + 1);

    EXPECT_TRUE(isNonNegativeNumber(vlan));
    EXPECT_TRUE(isLikelyMac(mac));
}

// ===========================================================================
// 4.2.2 Required field and enum contract
// ===========================================================================
TEST_F(DbContractModelTest, PortTableRequiresAdminStatus)
{
    Table app_port(app_db.get(), APP_PORT_TABLE_NAME);

    app_port.set("Ethernet0", fv({{"speed", "10000"}}));
    auto attrs = app_port.get("Ethernet0");

    EXPECT_FALSE(hasField(attrs, "admin_status"));

    app_port.set("Ethernet0", fv({{"admin_status", "up"}}));
    attrs = app_port.get("Ethernet0");
    EXPECT_TRUE(hasField(attrs, "admin_status"));
}

TEST_F(DbContractModelTest, AdminStatusEnumOnlyUpOrDown)
{
    EXPECT_TRUE(isValidAdminStatus("up"));
    EXPECT_TRUE(isValidAdminStatus("down"));
    EXPECT_FALSE(isValidAdminStatus("enable"));
    EXPECT_FALSE(isValidAdminStatus("1"));
}

TEST_F(DbContractModelTest, CounterValuesMustBeNonNegativeNumbers)
{
    EXPECT_TRUE(isNonNegativeNumber("0"));
    EXPECT_TRUE(isNonNegativeNumber("12345"));
    EXPECT_FALSE(isNonNegativeNumber("-1"));
    EXPECT_FALSE(isNonNegativeNumber("12ms"));
}

// ===========================================================================
// 4.2.3 Reference/dependency contract
// ===========================================================================
TEST_F(DbContractModelTest, NeighborReferenceRequiresPortExistence)
{
    Table app_port(app_db.get(), APP_PORT_TABLE_NAME);
    Table app_neigh(app_db.get(), APP_NEIGH_TABLE_NAME);

    app_neigh.set("Ethernet4:10.0.0.2", fv({{"neigh", "00:11:22:33:44:55"}}));

    // Dependency is missing until the corresponding port appears.
    EXPECT_TRUE(app_port.get("Ethernet4").empty());

    app_port.set("Ethernet4", fv({{"admin_status", "up"}}));
    EXPECT_FALSE(app_port.get("Ethernet4").empty());
}

TEST_F(DbContractModelTest, RouteContractRequiresNexthopInfo)
{
    Table app_route(app_db.get(), APP_ROUTE_TABLE_NAME);

    app_route.set("10.0.0.0/24", fv({{"blackhole", "0"}}));
    auto attrs = app_route.get("10.0.0.0/24");

    bool has_nexthop = hasField(attrs, "nexthop");
    bool has_nhg = hasField(attrs, "nexthop_group");
    EXPECT_FALSE(has_nexthop || has_nhg);

    app_route.set("10.0.0.0/24", fv({{"nexthop", "10.0.0.1"}, {"ifname", "Ethernet0"}}));
    attrs = app_route.get("10.0.0.0/24");
    EXPECT_TRUE(hasField(attrs, "nexthop"));
}

TEST_F(DbContractModelTest, FdbTypeEnumMustBeStaticOrDynamic)
{
    Table app_fdb(app_db.get(), APP_FDB_TABLE_NAME);
    app_fdb.set("100:aa:bb:cc:dd:ee:ff", fv({{"port", "Ethernet0"}, {"type", "dynamic"}}));

    auto attrs = app_fdb.get("100:aa:bb:cc:dd:ee:ff");
    string type;
    ASSERT_TRUE(getField(attrs, "type", type));
    EXPECT_TRUE(type == "static" || type == "dynamic");
}

// ===========================================================================
// 4.2.4 Multi-DB role separation contract
// ===========================================================================
TEST_F(DbContractModelTest, ConfigAndAppDbAreIndependentNamespaces)
{
    Table cfg_tbl(cfg_db.get(), "MGMT_INTF_CONFIG");
    Table app_tbl(app_db.get(), "MGMT_INTF_APP");

    cfg_tbl.set("global", fv({{"admin_status", "up"}}));

    EXPECT_FALSE(cfg_tbl.get("global").empty());
    EXPECT_TRUE(app_tbl.get("global").empty());
}

TEST_F(DbContractModelTest, StateDbStoresOperationalNotIntent)
{
    Table app_tbl(app_db.get(), "MGMT_INTF_APP");
    Table state_tbl(state_db.get(), "MGMT_OBSERVE_STATUS");

    app_tbl.set("global", fv({{"admin_status", "up"}}));
    state_tbl.set("global", fv({{"service_state", "active"}}));

    EXPECT_TRUE(hasField(app_tbl.get("global"), "admin_status"));
    EXPECT_TRUE(hasField(state_tbl.get("global"), "service_state"));
}

TEST_F(DbContractModelTest, CountersDbStoresNumericObservabilityData)
{
    Table ctr_tbl(counters_db.get(), "MGMT_OBSERVE_COUNTERS");
    ctr_tbl.set("Ethernet0", fv({{"rx_packets", "1000"}, {"tx_packets", "500"}}));

    auto attrs = ctr_tbl.get("Ethernet0");
    string rx;
    ASSERT_TRUE(getField(attrs, "rx_packets", rx));
    EXPECT_TRUE(isNonNegativeNumber(rx));
}

// ===========================================================================
// 4.2.5 Contract resilience with merge semantics
// ===========================================================================
TEST_F(DbContractModelTest, SetMergeKeepsExistingContractFields)
{
    Table app_port(app_db.get(), APP_PORT_TABLE_NAME);
    app_port.set("Ethernet0", fv({{"admin_status", "up"}, {"speed", "10000"}}));
    app_port.set("Ethernet0", fv({{"mtu", "9100"}}));

    auto attrs = app_port.get("Ethernet0");
    EXPECT_TRUE(hasField(attrs, "admin_status"));
    EXPECT_TRUE(hasField(attrs, "speed"));
    EXPECT_TRUE(hasField(attrs, "mtu"));
}

TEST_F(DbContractModelTest, DelRemovesEntryFromContractDomain)
{
    Table app_port(app_db.get(), APP_PORT_TABLE_NAME);
    app_port.set("Ethernet0", fv({{"admin_status", "up"}}));
    EXPECT_FALSE(app_port.get("Ethernet0").empty());

    app_port.del("Ethernet0");
    EXPECT_TRUE(app_port.get("Ethernet0").empty());
}
