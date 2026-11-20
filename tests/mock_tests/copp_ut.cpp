#include "gtest/gtest.h"
#include <string>
#include "schema.h"
#include "warm_restart.h"
#include "ut_helper.h"
#include "mock_table.h"
#include "coppmgr.h"
#include <fstream>
#include <streambuf>

using namespace std;
using namespace swss;

TEST(CoppMgrTest, CoppTest)
{
    const vector<string> cfg_copp_tables = {
                CFG_COPP_TRAP_TABLE_NAME,
                CFG_COPP_GROUP_TABLE_NAME,
                CFG_FEATURE_TABLE_NAME,
            };

    WarmStart::initialize("coppmgrd", "swss");
    WarmStart::checkWarmStart("coppmgrd", "swss");

    DBConnector cfgDb("CONFIG_DB", 0);
    DBConnector appDb("APPL_DB", 0);
    DBConnector stateDb("STATE_DB", 0);
    
    /* The test will set an entry with queue1_group1|cbs value which differs from the init value
     * found in the copp_cfg.json file. Then coppmgr constructor will be called and it will detect
     * that there is already an entry for queue1_group1|cbs with different value and it should be
     * overwritten with the init value.
     * hget will verify that this indeed happened.
     */
    Table coppTable = Table(&appDb, APP_COPP_TABLE_NAME);
    coppTable.set("queue1_group1",
                {
                    {"cbs", "6100"},
                    {"cir", "6000"},
                    {"meter_type", "packets"},
                    {"mode", "sr_tcm"},
                    {"queue", "1"},
                    {"red_action", "drop"},
                    {"trap_action", "trap"},
                    {"trap_priority", "1"},
                    {"trap_ids", "ip2me"}
                });

    CoppMgr coppmgr(&cfgDb, &appDb, &stateDb, cfg_copp_tables, "./copp_cfg.json");

    string overide_val;
    coppTable.hget("queue1_group1", "cbs",overide_val);
    EXPECT_EQ( overide_val, "6000");
}


class CoppMgrExcludePortsTest : public ::testing::Test
{
protected:
    DBConnector cfgDb{"CONFIG_DB", 0};
    DBConnector appDb{"APPL_DB", 0};
    DBConnector stateDb{"STATE_DB", 0};
    Table cfgPort{&cfgDb, CFG_PORT_TABLE_NAME};
    Table cfgLagMember{&cfgDb, CFG_LAG_MEMBER_TABLE_NAME};
    Table cfgExclude{&cfgDb, CFG_COPP_TRAP_EXCLUDE_PORTS_TABLE_NAME};
    Table cfgGroup{&cfgDb, CFG_COPP_GROUP_TABLE_NAME};
    Table appCopp{&appDb, APP_COPP_TABLE_NAME};

    void SetUp() override
    {
        testing_db::reset();
        for (auto port : { "Ethernet0", "Ethernet4", "Ethernet8" })
        {
            cfgPort.set(port, { { "lanes", "0" } });
        }
        cfgLagMember.set("PortChannel1|Ethernet8", { { "NULL", "NULL" } });
    }

    unique_ptr<CoppMgr> create()
    {
        const vector<string> tables = {
            CFG_COPP_TRAP_TABLE_NAME,
            CFG_COPP_GROUP_TABLE_NAME,
            CFG_FEATURE_TABLE_NAME,
            CFG_COPP_TRAP_EXCLUDE_PORTS_TABLE_NAME,
            CFG_PORT_TABLE_NAME,
            CFG_LAG_MEMBER_TABLE_NAME,
        };
        return unique_ptr<CoppMgr>(new CoppMgr(&cfgDb, &appDb, &stateDb, tables, "./copp_cfg.json"));
    }

    void notify(CoppMgr &mgr, Table &table)
    {
        mgr.addExistingData(&table);
        mgr.doTask();
    }

    string excludePorts(const string &group)
    {
        string value = "<unset>";
        appCopp.hget(group, "exclude_ports", value);
        return value;
    }
};

TEST_F(CoppMgrExcludePortsTest, Validation)
{
    // Duplicates are dropped, unknown ports and PortChannel members are skipped, the list is sorted
    cfgExclude.set("GLOBAL", { { "ports@", "Ethernet4,Ethernet0,Ethernet4,Ethernet99,Ethernet8" } });
    auto mgr = create();

    EXPECT_EQ(excludePorts("queue4_group1"), "Ethernet0,Ethernet4");
    EXPECT_EQ(excludePorts("default"), "Ethernet0,Ethernet4");
    EXPECT_EQ(excludePorts("queue2_group1"), "<unset>");
}

TEST_F(CoppMgrExcludePortsTest, NonTrapAction)
{
    cfgExclude.set("GLOBAL", { { "ports@", "Ethernet0" } });
    auto mgr = create();

    cfgGroup.set("queue4_group1", { { "trap_action", "drop" } });
    notify(*mgr, cfgGroup);
    EXPECT_EQ(excludePorts("queue4_group1"), "");

    cfgGroup.set("queue4_group1", { { "trap_action", "copy" } });
    notify(*mgr, cfgGroup);
    EXPECT_EQ(excludePorts("queue4_group1"), "Ethernet0");
}

TEST_F(CoppMgrExcludePortsTest, Dynamic)
{
    auto mgr = create();
    EXPECT_EQ(excludePorts("queue4_group1"), "<unset>");

    cfgExclude.set("GLOBAL", { { "ports@", "Ethernet0" } });
    notify(*mgr, cfgExclude);
    EXPECT_EQ(excludePorts("queue4_group1"), "Ethernet0");

    cfgLagMember.set("PortChannel1|Ethernet0", { { "NULL", "NULL" } });
    notify(*mgr, cfgLagMember);
    EXPECT_EQ(excludePorts("queue4_group1"), "");

    cfgLagMember.del("PortChannel1|Ethernet0");
    notify(*mgr, cfgLagMember);
    EXPECT_EQ(excludePorts("queue4_group1"), "Ethernet0");

    cfgExclude.del("GLOBAL");
    notify(*mgr, cfgPort);
    EXPECT_EQ(excludePorts("queue4_group1"), "");
}

TEST_F(CoppMgrExcludePortsTest, RestartNoChurn)
{
    cfgExclude.set("GLOBAL", { { "ports@", "Ethernet4,Ethernet0" } });
    auto mgr = create();

    auto sets = testing_db::getProducerSetCount(APP_COPP_TABLE_NAME);
    auto dels = testing_db::getProducerDelCount(APP_COPP_TABLE_NAME);
    mgr = create();
    EXPECT_EQ(testing_db::getProducerSetCount(APP_COPP_TABLE_NAME), sets);
    EXPECT_EQ(testing_db::getProducerDelCount(APP_COPP_TABLE_NAME), dels);
    EXPECT_EQ(excludePorts("queue4_group1"), "Ethernet0,Ethernet4");
}
