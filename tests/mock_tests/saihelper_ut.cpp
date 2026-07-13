#include "ut_helper.h"
#include "saihelper.h"
#include "mock_table.h"

#include <memory>
#include <sstream>

extern std::unique_ptr<swss::DBConnector> gHealthStateDb;
extern std::unique_ptr<swss::Table> gOrchHealthTable;
extern bool gOrchUnhealthyCached;
extern std::string gLastSaiError;

namespace saihelper_test
{
    TEST(ResolveCommunicationModeFromContextConfig, NonZmqInputUnchanged)
    {
        std::istringstream iss(R"({"CONTEXTS":[{"guid":0,"zmq_enable":false}]})");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC));
    }

    TEST(ResolveCommunicationModeFromContextConfig, RedisSyncInputUnchanged)
    {
        std::istringstream iss(R"({"CONTEXTS":[{"guid":0,"zmq_enable":false}]})");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC));
    }

    TEST(ResolveCommunicationModeFromContextConfig, ZmqDisabledForGuidZeroDemotes)
    {
        std::istringstream iss(R"({"CONTEXTS":[{"guid":0,"zmq_enable":false}]})");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC));
    }

    TEST(ResolveCommunicationModeFromContextConfig, ZmqEnabledForGuidZeroUnchanged)
    {
        std::istringstream iss(R"({"CONTEXTS":[{"guid":0,"zmq_enable":true}]})");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC));
    }

    TEST(ResolveCommunicationModeFromContextConfig, MissingZmqEnableUnchanged)
    {
        std::istringstream iss(R"({"CONTEXTS":[{"guid":0}]})");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC));
    }

    TEST(ResolveCommunicationModeFromContextConfig, NonZeroGuidSkipped)
    {
        // Only guid=0 (the default context) is consulted; non-zero guids are
        // ignored even if they disable zmq.
        std::istringstream iss(R"({"CONTEXTS":[{"guid":1,"zmq_enable":false}]})");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC));
    }

    TEST(ResolveCommunicationModeFromContextConfig, FindsGuidZeroAmongMultiple)
    {
        std::istringstream iss(R"({"CONTEXTS":[{"guid":1,"zmq_enable":true},{"guid":0,"zmq_enable":false}]})");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_REDIS_SYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC));
    }

    TEST(ResolveCommunicationModeFromContextConfig, MalformedJsonUnchanged)
    {
        std::istringstream iss("not valid json");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC));
    }

    TEST(ResolveCommunicationModeFromContextConfig, MissingContextsKeyUnchanged)
    {
        std::istringstream iss(R"({"OTHER_KEY":[]})");
        EXPECT_EQ(SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC,
                  resolveCommunicationModeFromContextConfig(
                      iss, SAI_REDIS_COMMUNICATION_MODE_ZMQ_SYNC));
    }
}

namespace sai_failure_status_test
{
    struct SaiFailureStatusTest : public ::testing::Test
    {
        void SetUp() override
        {
            testing_db::reset();
            gLastSaiError.clear();
            initSaiFailureTable();
        }

        void TearDown() override
        {
            testing_db::reset();
            gLastSaiError.clear();
        }
    };

    TEST_F(SaiFailureStatusTest, InitiallyHealthy)
    {
        setSaiFailureStatus(false);
        std::string error;
        EXPECT_FALSE(getSaiFailureStatus(error));
    }

    TEST_F(SaiFailureStatusTest, SetUnhealthyReturnsTrueWithError)
    {
        std::string errorMsg = "Encountered failure in set operation, SAI API: SAI_API_SWITCH, status: -1";
        setSaiFailureStatus(true, errorMsg);

        std::string error;
        EXPECT_TRUE(getSaiFailureStatus(error));
        EXPECT_EQ(error, errorMsg);
    }

    TEST_F(SaiFailureStatusTest, ResetToHealthyAfterFailure)
    {
        setSaiFailureStatus(true, "some SAI error");

        std::string error;
        EXPECT_TRUE(getSaiFailureStatus(error));
        EXPECT_EQ(error, "some SAI error");

        // Reset
        setSaiFailureStatus(false);
        EXPECT_FALSE(getSaiFailureStatus(error));
    }

    TEST_F(SaiFailureStatusTest, ExternalResetDetected)
    {
        // Simulate a SAI failure
        setSaiFailureStatus(true, "SAI failure");

        std::string error;
        EXPECT_TRUE(getSaiFailureStatus(error));

        // Simulate an external operator clearing the flag in STATE_DB
        // by writing "false" directly via the mock DB
        swss::DBConnector db("STATE_DB", 0);
        swss::Table tbl(&db, "PROCESS_HEALTH");
        std::vector<swss::FieldValueTuple> fvs;
        fvs.emplace_back("unhealthy", "false");
        fvs.emplace_back("error", "");
        tbl.set("orchagent", fvs);

        // getSaiFailureStatus should detect the external reset
        EXPECT_FALSE(getSaiFailureStatus(error));

        // Subsequent calls should also return false (cache updated)
        EXPECT_FALSE(getSaiFailureStatus(error));
    }

    TEST_F(SaiFailureStatusTest, MultipleFailuresOverwrite)
    {
        setSaiFailureStatus(true, "first error");
        setSaiFailureStatus(true, "second error");

        std::string error;
        EXPECT_TRUE(getSaiFailureStatus(error));
        EXPECT_EQ(error, "second error");
    }

    TEST_F(SaiFailureStatusTest, KeyMissingKeepsUnhealthy)
    {
        // Mark unhealthy so the cache is set
        setSaiFailureStatus(true, "SAI failure");

        // Delete the key from mock DB to simulate missing key
        testing_db::reset();

        // getSaiFailureStatus should still report unhealthy with cached error
        std::string error;
        EXPECT_TRUE(getSaiFailureStatus(error));
        EXPECT_EQ(error, "SAI failure");
    }

    TEST_F(SaiFailureStatusTest, NullTableReturnsUnhealthy)
    {
        // Null out the table directly
        gOrchHealthTable.reset();
        gHealthStateDb.reset();

        // Set cache to unhealthy (table is null, so DB write is skipped)
        setSaiFailureStatus(true, "SAI failure");

        // getSaiFailureStatus should hit the null table guard with cached error
        std::string error;
        EXPECT_TRUE(getSaiFailureStatus(error));
        EXPECT_EQ(error, "SAI failure");

        // Re-init for TearDown safety
        initSaiFailureTable();
    }
}
