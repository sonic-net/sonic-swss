#include "orchdaemon.h"
#include "dbconnector.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "mock_sai_switch.h"

extern sai_switch_api_t* sai_switch_api;
sai_switch_api_t test_sai_switch;

namespace orchdaemon_test
{

    using ::testing::_;
    using ::testing::Return;
    using ::testing::StrictMock;

    DBConnector appl_db("APPL_DB", 0);
    DBConnector state_db("STATE_DB", 0);
    DBConnector config_db("CONFIG_DB", 0);
    DBConnector counters_db("COUNTERS_DB", 0);

    class OrchDaemonTest : public ::testing::Test
    {
        public:
            StrictMock<MockSaiSwitch> mock_sai_switch_;

            OrchDaemon* orchd;

            OrchDaemonTest()
            {
                mock_sai_switch = &mock_sai_switch_;
                sai_switch_api = &test_sai_switch;
                sai_switch_api->get_switch_attribute = &mock_get_switch_attribute;
                sai_switch_api->set_switch_attribute = &mock_set_switch_attribute;

                orchd = new OrchDaemon(&appl_db, &config_db, &state_db, &counters_db);

            };

            ~OrchDaemonTest()
            {

            };
    };

    TEST_F(OrchDaemonTest, logRotate)
    {
        EXPECT_CALL(mock_sai_switch_, set_switch_attribute( _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));

        orchd->logRotate();
    }
}
