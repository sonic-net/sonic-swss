#include "gtest/gtest.h"
#define protected public
#define private public
#include "teamsync.h"
#undef protected
#undef private 
#include "mock_table.h"

namespace teamsyncd_ut
{
    struct TeamSyncdTest : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_config_db;
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::DBConnector> m_state_db;

        std::shared_ptr<swss::Table> m_stateWarmRestartTable;

        void SetUp() override
        {
            testing_db::reset();
            m_config_db = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_state_db = std::make_shared<swss::DBConnector>("STATE_DB", 0);
        }
    };

    TEST_F(TeamSyncdTest, testAddingLagOnWarmBootSetsStateDbFlag)
    {
        swss::TeamSync sync(m_config_db.get(), m_state_db.get(), nullptr);
        swss::Table stateLagTable(m_state_db.get(), STATE_LAG_TABLE_NAME);

        sync.m_warmstart = true;

        const bool admin_state = true;
        const bool oper_state = true;
        const int if_index = 1;
        const unsigned int mtu = 1500;
        const char* lag_name = "PortChannel1";
        sync.addLag(lag_name, if_index, admin_state, oper_state, mtu);

        std::string okValue;
        const bool found = stateLagTable.hget(lag_name, "state", okValue);
        ASSERT_TRUE(found);
        ASSERT_EQ(okValue, "ok");
    }
}
