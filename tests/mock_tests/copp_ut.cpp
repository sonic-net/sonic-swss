#include "gtest/gtest.h"
#include <string>
#include "schema.h"
#include "warm_restart.h"
#include "ut_helper.h"
#include "coppmgr.h"
#include "coppmgr.cpp"
#include <fstream>
#include <streambuf>
using namespace std;
using namespace swss;

void create_init_file()
{
    system("sudo mkdir /etc/sonic/");
    system("sudo chmod 777 /etc/sonic/");

    ofstream file(COPP_INIT_FILE);

    ifstream t("copp_cfg.json");
    string data((std::istreambuf_iterator<char>(t)),
                 std::istreambuf_iterator<char>());

    file << data;

    t.close();
    file.close();
}

void cleanup()
{
    system("sudo rm -rf /etc/sonic/");
}

TEST(CoppMgrTest, CoppTest)
{
    create_init_file();

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

    CoppMgr coppmgr(&cfgDb, &appDb, &stateDb, cfg_copp_tables);

    string overide_val;
    coppTable.hget("queue1_group1", "cbs",overide_val);
    EXPECT_EQ( overide_val, "6000");


    cleanup();
}



