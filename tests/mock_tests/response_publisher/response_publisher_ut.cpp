#include "response_publisher.h"

#include <gtest/gtest.h>

using namespace swss;

TEST(ResponsePublisher, TestPublish)
{
    DBConnector conn{"APPL_STATE_DB", 0};
    Table stateTable{&conn, "SOME_TABLE"};
    std::string value;
    ResponsePublisher publisher{"APPL_STATE_DB"};

    publisher.publish("SOME_TABLE", "SOME_KEY", {{"field", "value"}}, ReturnCode(SAI_STATUS_SUCCESS));
    ASSERT_TRUE(stateTable.hget("SOME_KEY", "field", value));
    ASSERT_EQ(value, "value");
}

TEST(ResponsePublisher, TestPublishBuffered)
{
    DBConnector conn{"APPL_STATE_DB", 0};
    Table stateTable{&conn, "SOME_TABLE"};
    std::string value;
    ResponsePublisher publisher{"APPL_STATE_DB"};

    publisher.setBuffered(true);

    publisher.publish("SOME_TABLE", "SOME_KEY", {{"field", "value"}}, ReturnCode(SAI_STATUS_SUCCESS));
    publisher.flush();
    ASSERT_TRUE(stateTable.hget("SOME_KEY", "field", value));
    ASSERT_EQ(value, "value");
}
