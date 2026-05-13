#include "ut_helper.h"
#include "saihelper.h"

#include <sstream>

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
