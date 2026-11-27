#include "gtest/gtest.h"
#include <string>
#include "schema.h"
#include "warm_restart.h"
#include "ut_helper.h"
#include "mock_table.h"
#define private public
#include "coppmgr.h"
#undef private
#include <cstdlib>
#include <fstream>
#include <streambuf>
#include <unistd.h>

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

namespace
{

const char *kPtpCoppCfg = R"json({
    "COPP_GROUP": {
        "queue4_group3": {
            "trap_action": "trap",
            "trap_priority": "4",
            "queue": "4"
        }
    },
    "COPP_TRAP": {
        "lacp": {
            "trap_ids": "lacp",
            "trap_group": "queue4_group3",
            "always_enabled": "true"
        },
        "ptp": {
            "trap_ids": "ptp",
            "trap_group": "queue4_group3"
        }
    }
})json";

class PtpPlatformEnv
{
public:
    PtpPlatformEnv()
    {
        char tmpl[] = "/tmp/swss-ptp-env-XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0)
        {
            close(fd);
        }
        m_path = tmpl;
        setenv("SWSS_PTP_PLATFORM_ENV", m_path.c_str(), 1);
    }

    ~PtpPlatformEnv()
    {
        unsetenv("SWSS_PTP_PLATFORM_ENV");
        unlink(m_path.c_str());
    }

    void write(const std::string &contents) const
    {
        std::ofstream out(m_path, std::ios::trunc);
        out << contents;
    }

private:
    std::string m_path;
};

std::string writeCoppCfg()
{
    char tmpl[] = "/tmp/swss-copp-cfg-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0)
    {
        close(fd);
    }
    std::ofstream out(tmpl, std::ios::trunc);
    out << kPtpCoppCfg;
    return tmpl;
}

void setFeatureState(swss::DBConnector *cfgDb, const std::string &feature, const std::string &state)
{
    swss::Table featureTable(cfgDb, CFG_FEATURE_TABLE_NAME);
    featureTable.set(feature, { { "state", state } });
}

std::string trapIds(swss::DBConnector *appDb)
{
    swss::Table coppTable(appDb, APP_COPP_TABLE_NAME);
    std::string ids;
    coppTable.hget("queue4_group3", "trap_ids", ids);
    return ids;
}

} // namespace

TEST(CoppMgrTest, PtpTrapStaysOffWithoutPlatformFlag)
{
    ::testing_db::reset();
    unsetenv("SWSS_PTP_PLATFORM_ENV");

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
    setFeatureState(&cfgDb, "ptp", "enabled");

    // A missing platform file must not install the trap, even though the
    // feature state is enabled so the container can be created.
    unlink("/tmp/swss-ptp-env-missing");
    setenv("SWSS_PTP_PLATFORM_ENV", "/tmp/swss-ptp-env-missing", 1);
    std::string cfg = writeCoppCfg();
    CoppMgr coppmgr(&cfgDb, &appDb, &stateDb, cfg_copp_tables, cfg);

    EXPECT_FALSE(coppmgr.isFeatureEnabled("ptp"));
    EXPECT_EQ(trapIds(&appDb), "lacp");

    PtpPlatformEnv env;
    env.write("ptp_enabled=0\n");
    EXPECT_FALSE(coppmgr.isFeatureEnabled("ptp"));

    env.write("# comment\n\nPTP_ENABLED = 1\n");
    EXPECT_TRUE(coppmgr.isFeatureEnabled("ptp"));

    env.write("ptp_enabled=\n");
    EXPECT_FALSE(coppmgr.isFeatureEnabled("ptp"));

    env.write("other=1\n");
    EXPECT_FALSE(coppmgr.isFeatureEnabled("ptp"));

    env.write("ptp_enabled=0\nptp_enabled=1\n");
    EXPECT_FALSE(coppmgr.isFeatureEnabled("ptp"));

    unlink(cfg.c_str());
    ::testing_db::reset();
}

TEST(CoppMgrTest, PtpTrapInstalledWhenPlatformAndFeatureEnabled)
{
    ::testing_db::reset();

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

    PtpPlatformEnv env;
    env.write("ptp_enabled=1\n");
    setFeatureState(&cfgDb, "ptp", "enabled");

    std::string cfg = writeCoppCfg();
    CoppMgr coppmgr(&cfgDb, &appDb, &stateDb, cfg_copp_tables, cfg);

    EXPECT_TRUE(coppmgr.isFeatureEnabled("ptp"));
    EXPECT_EQ(trapIds(&appDb), "lacp,ptp");

    unlink(cfg.c_str());
    ::testing_db::reset();
}

TEST(CoppMgrTest, PtpTrapStaysOffWhenFeatureDisabled)
{
    ::testing_db::reset();

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

    PtpPlatformEnv env;
    env.write("ptp_enabled=1\n");
    setFeatureState(&cfgDb, "ptp", "disabled");

    std::string cfg = writeCoppCfg();
    {
        CoppMgr disabled(&cfgDb, &appDb, &stateDb, cfg_copp_tables, cfg);
        EXPECT_FALSE(disabled.isFeatureEnabled("ptp"));
        EXPECT_EQ(trapIds(&appDb), "lacp");
    }

    ::testing_db::reset();
    setFeatureState(&cfgDb, "ptp", "always_enabled");
    {
        CoppMgr always(&cfgDb, &appDb, &stateDb, cfg_copp_tables, cfg);
        EXPECT_TRUE(always.isFeatureEnabled("ptp"));
        EXPECT_EQ(trapIds(&appDb), "lacp,ptp");
    }

    unlink(cfg.c_str());
    ::testing_db::reset();
}
