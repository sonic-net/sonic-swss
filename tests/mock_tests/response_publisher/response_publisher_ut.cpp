#include "response_publisher.h"

#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <thread>

#include "return_code.h"

using namespace swss;

namespace
{
/* ResponsePublisher::flush() returns after enqueueing work; the state thread
 * applies DB writes asynchronously. Poll until hget succeeds or timeout. */
bool pollHget(Table &t, const std::string &key, const std::string &field, std::string *out, int timeout_ms = 3000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::string v;
        if (t.hget(key, field, v))
        {
            if (out)
                *out = std::move(v);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}
} // namespace

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

TEST(ResponsePublisher, TestPublishEnableDbWrite)
{
    DBConnector conn{"APPL_STATE_DB", 0};
    Table stateTable{&conn, "SOME_TABLE"};
    std::string value;
    ResponsePublisher publisher{"APPL_STATE_DB"};

    publisher.publish("SOME_TABLE", "SOME_KEY", {{"field", "value"}}, ReturnCode(SAI_STATUS_SUCCESS));
    publisher.flush();
    ASSERT_TRUE(stateTable.hget("SOME_KEY", "field", value));
    ASSERT_EQ(value, "value");

    publisher.setEnableDbWriteAndNotify(false);

    publisher.publish("SOME_TABLE", "SOME_KEY", {{"field", "new-value"}}, ReturnCode(SAI_STATUS_SUCCESS));
    publisher.flush();
    ASSERT_TRUE(stateTable.hget("SOME_KEY", "field", value));
    ASSERT_EQ(value, "value");

    publisher.setEnableDbWriteAndNotify(true);

    publisher.publish("SOME_TABLE", "SOME_KEY", {{"field", "new-value"}}, ReturnCode(SAI_STATUS_SUCCESS));
    publisher.flush();
    ASSERT_TRUE(stateTable.hget("SOME_KEY", "field", value));
    ASSERT_EQ(value, "new-value");
}

/* RouteOrch-style async path: third ctor arg starts m_update_thread; RouteOrch
 * uses publishAsync + publishAsyncBatch + flush after gRouteBulker.flush(). */
TEST(ResponsePublisher, PublishAsyncNoThreadFallsBackToSyncPublish)
{
    DBConnector conn{"APPL_STATE_DB", 0};
    Table stateTable{&conn, "ROUTE_TABLE"};
    std::string value;

    ResponsePublisher publisher{"APPL_STATE_DB", /*buffered=*/false, /*db_write_thread=*/false};
    publisher.m_directDbWrite = true;

    publisher.publishAsync("ROUTE_TABLE", "10.0.0.0/24", {{"protocol", "bgp"}, {"state", "ok"}},
                           ReturnCode(SAI_STATUS_SUCCESS));
    ASSERT_TRUE(pollHget(stateTable, "10.0.0.0/24", "state", &value));
    ASSERT_EQ(value, "ok");
}

TEST(ResponsePublisher, PublishAsyncBatchWithWorkerWritesAllKeys)
{
    DBConnector conn{"APPL_STATE_DB", 0};
    Table stateTable{&conn, "ROUTE_TABLE"};

    ResponsePublisher publisher{"APPL_STATE_DB", /*buffered=*/false, /*db_write_thread=*/true};
    publisher.m_directDbWrite = true;
    publisher.setAsyncFullPublish(true);

    publisher.publishAsync("ROUTE_TABLE", "10.1.0.0/24", {{"protocol", "bgp"}, {"state", "ok"}},
                           ReturnCode(SAI_STATUS_SUCCESS));
    publisher.publishAsync("ROUTE_TABLE", "10.2.0.0/24", {{"protocol", "bgp"}, {"state", "ok"}},
                           ReturnCode(SAI_STATUS_SUCCESS));
    publisher.publishAsync("ROUTE_TABLE", "10.3.0.0/24", {{"protocol", "bgp"}, {"state", "ok"}},
                           ReturnCode(SAI_STATUS_SUCCESS));

    publisher.publishAsyncBatch();
    publisher.flush();

    std::string v;
    ASSERT_TRUE(pollHget(stateTable, "10.1.0.0/24", "state", &v));
    ASSERT_EQ(v, "ok");
    ASSERT_TRUE(pollHget(stateTable, "10.2.0.0/24", "state", &v));
    ASSERT_EQ(v, "ok");
    ASSERT_TRUE(pollHget(stateTable, "10.3.0.0/24", "state", &v));
    ASSERT_EQ(v, "ok");
}

TEST(ResponsePublisher, PublishAsyncWithoutBatchThenFlushDoesNotWriteState)
{
    DBConnector conn{"APPL_STATE_DB", 0};
    Table stateTable{&conn, "ROUTE_TABLE"};

    {
        ResponsePublisher publisher{"APPL_STATE_DB", false, true};
        publisher.m_directDbWrite = true;
        publisher.setAsyncFullPublish(true);

        publisher.publishAsync("ROUTE_TABLE", "10.9.9.0/24", {{"protocol", "bgp"}, {"state", "ok"}},
                               ReturnCode(SAI_STATUS_SUCCESS));
        /* Missing publishAsyncBatch(): pending stays in m_async_publish_pending; flush only drains pipes. */
        publisher.flush();

        std::string dummy;
        ASSERT_FALSE(pollHget(stateTable, "10.9.9.0/24", "state", &dummy, 200));
    }

    /* After destructor, key must still be absent (batch was never enqueued). */
    std::string value;
    ASSERT_FALSE(stateTable.hget("10.9.9.0/24", "state", value));
}

TEST(ResponsePublisher, PublishAsyncBatchEmptyIsNoOpThenFlush)
{
    DBConnector conn{"APPL_STATE_DB", 0};
    ResponsePublisher publisher{"APPL_STATE_DB", false, true};
    publisher.setAsyncFullPublish(true);

    publisher.publishAsyncBatch();
    publisher.flush();
    /* No ASSERT on DB: should complete without deadlock/hang. */
    SUCCEED();
}

TEST(ResponsePublisher, PublishAsyncErrorStatusSkipsApplStateWrite)
{
    DBConnector conn{"APPL_STATE_DB", 0};
    Table stateTable{&conn, "ROUTE_TABLE"};

    ResponsePublisher publisher{"APPL_STATE_DB", false, true};
    publisher.m_directDbWrite = true;
    publisher.setAsyncFullPublish(true);

    publisher.publishAsync("ROUTE_TABLE", "10.8.0.0/24", {{"protocol", "bgp"}, {"state", "bad"}},
                           ReturnCode(SAI_STATUS_INVALID_PARAMETER));
    publisher.publishAsyncBatch();
    publisher.flush();

    std::string dummy;
    ASSERT_FALSE(pollHget(stateTable, "10.8.0.0/24", "state", &dummy, 200));
}

TEST(ResponsePublisher, PublishAsyncTwoSequentialBatches)
{
    DBConnector conn{"APPL_STATE_DB", 0};
    Table stateTable{&conn, "ROUTE_TABLE"};

    ResponsePublisher publisher{"APPL_STATE_DB", false, true};
    publisher.m_directDbWrite = true;
    publisher.setAsyncFullPublish(true);

    publisher.publishAsync("ROUTE_TABLE", "10.10.1.0/24", {{"state", "a"}}, ReturnCode(SAI_STATUS_SUCCESS));
    publisher.publishAsyncBatch();
    publisher.flush();
    std::string v;
    ASSERT_TRUE(pollHget(stateTable, "10.10.1.0/24", "state", &v));
    ASSERT_EQ(v, "a");

    publisher.publishAsync("ROUTE_TABLE", "10.10.2.0/24", {{"state", "b"}}, ReturnCode(SAI_STATUS_SUCCESS));
    publisher.publishAsyncBatch();
    publisher.flush();
    ASSERT_TRUE(pollHget(stateTable, "10.10.2.0/24", "state", &v));
    ASSERT_EQ(v, "b");
}

