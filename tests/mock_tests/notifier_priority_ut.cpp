/*
 * Unit tests for Notifier priority delegation and stall detection.
 *
 * Validates:
 *  1. Notifier.getPri() delegates to NotificationConsumer (pri=100).
 *  2. Table consumers keep Executor-default priority (pri=0).
 *  3. Select::cmp orders Notifier before table consumers.
 *  4. Stall detection via execute(): after STALL_THRESHOLD no-progress
 *     execute() calls, hasCachedData() returns false.
 *  5. Recovery: counter resets when consumption drains the queue.
 */

/* Pre-include standard library headers that conflict with
 * the #define private/protected public hack. */
#include <string>
#include <fstream>
#include <iostream>
#include <sstream>
#include <memory>
#include <set>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>

#define protected public
#define private public
#include "orch.h"
#include "select.h"
#include "notifier.h"
#undef private
#undef protected

#include "dbconnector.h"
#include "notificationconsumer.h"
#include "consumerstatetable.h"
#include "mock_table.h"

#include <gtest/gtest.h>

extern redisReply *mockReply;

namespace notifier_priority_test
{
    using namespace std;

    /* Minimal Orch that does NOT call pop() — simulates allPortsReady() guard. */
    class DeferringOrch : public Orch
    {
    public:
        DeferringOrch(swss::DBConnector *db, const string &tableName)
            : Orch(db, tableName)
        {
        }

        void doTask(Consumer &consumer) override
        {
            consumer.m_toSync.clear();
        }

        void doTask(swss::NotificationConsumer &consumer) override
        {
            /* no-op: simulates deferral */
        }
    };

    /* Orch that always pops one notification (normal consumption). */
    class ConsumingOrch : public Orch
    {
    public:
        ConsumingOrch(swss::DBConnector *db, const string &tableName)
            : Orch(db, tableName)
        {
        }

        void doTask(Consumer &consumer) override
        {
            consumer.m_toSync.clear();
        }

        void doTask(swss::NotificationConsumer &consumer) override
        {
            string op, data;
            vector<swss::FieldValueTuple> values;
            consumer.pop(op, data, values);
        }
    };

    struct NotifierPriorityTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;

        NotifierPriorityTest()
        {
            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
        }

        void SetUp() override
        {
            ::testing_db::reset();
        }

        void TearDown() override
        {
            ::testing_db::reset();
        }

        /* Inject one notification into the consumer's internal queue. */
        void enqueueNotification(swss::NotificationConsumer *consumer)
        {
            std::vector<swss::FieldValueTuple> values;
            values.emplace_back("test_op", "test_data");
            std::string msg = swss::JSon::buildJson(values);

            mockReply = (redisReply *)calloc(1, sizeof(redisReply));
            mockReply->type = REDIS_REPLY_ARRAY;
            mockReply->elements = 3;
            mockReply->element = (redisReply **)calloc(3, sizeof(redisReply *));
            mockReply->element[0] = (redisReply *)calloc(1, sizeof(redisReply));
            mockReply->element[1] = (redisReply *)calloc(1, sizeof(redisReply));
            mockReply->element[2] = (redisReply *)calloc(1, sizeof(redisReply));
            mockReply->element[2]->type = REDIS_REPLY_STRING;
            mockReply->element[2]->str = (char *)calloc(1, msg.length() + 1);
            memcpy(mockReply->element[2]->str, msg.c_str(), msg.length());

            consumer->readData();
            mockReply = nullptr;
        }
    };

    /* Notifier wrapping NotificationConsumer reports pri=100, not Executor default 0. */
    TEST_F(NotifierPriorityTest, NotifierReportsNotificationConsumerPriority)
    {
        DeferringOrch orch(m_app_db.get(), "DUMMY_TABLE");

        auto *notifConsumer = new swss::NotificationConsumer(m_app_db.get(), "TEST_CHANNEL");
        EXPECT_EQ(notifConsumer->getPri(), 100);

        Notifier notifier(notifConsumer, &orch, "TEST_NOTIFICATIONS");
        EXPECT_EQ(notifier.getPri(), 100);
    }

    /* Consumer (table consumer) wrapping ConsumerStateTable reports pri=0. */
    TEST_F(NotifierPriorityTest, TableConsumerReportsDefaultPriority)
    {
        DeferringOrch orch(m_app_db.get(), "DUMMY_TABLE");

        auto *cst = new swss::ConsumerStateTable(m_app_db.get(), "PORT_TABLE", 1, 45);
        EXPECT_EQ(cst->getPri(), 45);

        Consumer consumer(cst, &orch, "PORT_TABLE");
        EXPECT_EQ(consumer.getPri(), 0);
    }

    /* Select::cmp places Notifier (pri=100) before Consumer (pri=0). */
    TEST_F(NotifierPriorityTest, SelectComparatorOrdersNotifierBeforeConsumer)
    {
        DeferringOrch orch(m_app_db.get(), "DUMMY_TABLE");

        auto *notifConsumer = new swss::NotificationConsumer(m_app_db.get(), "TEST_CHANNEL");
        Notifier notifier(notifConsumer, &orch, "TEST_NOTIFICATIONS");

        auto *cst = new swss::ConsumerStateTable(m_app_db.get(), "TEST_TABLE");
        Consumer consumer(cst, &orch, "TEST_TABLE");

        std::set<swss::Selectable *, swss::Select::cmp> readySet;
        readySet.insert(&notifier);
        readySet.insert(&consumer);

        EXPECT_EQ(*readySet.begin(), static_cast<swss::Selectable *>(&notifier));
    }

    /* Priority ordering is stable regardless of insertion order. */
    TEST_F(NotifierPriorityTest, SelectComparatorPriorityOverridesInsertionOrder)
    {
        DeferringOrch orch(m_app_db.get(), "DUMMY_TABLE");

        auto *cst = new swss::ConsumerStateTable(m_app_db.get(), "TEST_TABLE");
        Consumer consumer(cst, &orch, "TEST_TABLE");

        auto *notifConsumer = new swss::NotificationConsumer(m_app_db.get(), "TEST_CHANNEL");
        Notifier notifier(notifConsumer, &orch, "TEST_NOTIFICATIONS");

        std::set<swss::Selectable *, swss::Select::cmp> readySet;
        readySet.insert(&consumer);
        readySet.insert(&notifier);

        auto it = readySet.begin();
        EXPECT_EQ(*it, static_cast<swss::Selectable *>(&notifier));
        ++it;
        EXPECT_EQ(*it, static_cast<swss::Selectable *>(&consumer));
    }

    /* Drive stall detection through execute(): when the Orch defers (no pop),
     * m_noProgressCount increments and hasCachedData() suppresses after threshold. */
    TEST_F(NotifierPriorityTest, ExecuteDrivenStallDetection)
    {
        DeferringOrch orch(m_app_db.get(), "DUMMY_TABLE");

        auto *notifConsumer = new swss::NotificationConsumer(m_app_db.get(), "TEST_STALL");
        Notifier notifier(notifConsumer, &orch, "TEST_STALL");

        enqueueNotification(notifConsumer);
        ASSERT_TRUE(notifConsumer->hasCachedData());

        /* First execute: Orch defers → counter=1, still below threshold */
        notifier.execute();
        EXPECT_EQ(notifier.m_noProgressCount, 1);
        EXPECT_TRUE(notifier.hasCachedData());

        /* Second execute: counter=2 → at threshold, hasCachedData() suppressed */
        enqueueNotification(notifConsumer);
        notifier.execute();
        EXPECT_EQ(notifier.m_noProgressCount, 2);
        EXPECT_FALSE(notifier.hasCachedData());

        /* getPri() stays constant throughout — stall yields via hasCachedData only */
        EXPECT_EQ(notifier.getPri(), 100);
    }

    /* Drive consumption through execute(): when the Orch pops and drains the queue,
     * m_noProgressCount resets to 0. */
    TEST_F(NotifierPriorityTest, ExecuteDrivenConsumptionResetsCounter)
    {
        ConsumingOrch orch(m_app_db.get(), "DUMMY_TABLE");

        auto *notifConsumer = new swss::NotificationConsumer(m_app_db.get(), "TEST_CONSUME");
        Notifier notifier(notifConsumer, &orch, "TEST_CONSUME");

        /* Artificially stall first */
        notifier.m_noProgressCount = Notifier::STALL_THRESHOLD;
        EXPECT_FALSE(notifier.hasCachedData());

        /* Enqueue one notification and execute — ConsumingOrch pops it */
        enqueueNotification(notifConsumer);
        notifier.execute();

        /* Queue drained → counter reset → hasCachedData delegates normally */
        EXPECT_EQ(notifier.m_noProgressCount, 0);
        EXPECT_EQ(notifier.getPri(), 100);
    }

    /* A stalled Notifier still sorts before Consumer in Select::cmp.
     * Stall works by suppressing re-insertion (hasCachedData=false), not priority. */
    TEST_F(NotifierPriorityTest, StalledNotifierKeepsConstantPriority)
    {
        DeferringOrch orch(m_app_db.get(), "DUMMY_TABLE");

        auto *notifConsumer = new swss::NotificationConsumer(m_app_db.get(), "TEST_CHANNEL");
        Notifier notifier(notifConsumer, &orch, "TEST_NOTIFICATIONS");

        auto *cst = new swss::ConsumerStateTable(m_app_db.get(), "TEST_TABLE");
        Consumer consumer(cst, &orch, "TEST_TABLE");

        notifier.m_noProgressCount = Notifier::STALL_THRESHOLD;

        EXPECT_EQ(notifier.getPri(), 100);
        EXPECT_EQ(consumer.getPri(), 0);

        std::set<swss::Selectable *, swss::Select::cmp> readySet;
        readySet.insert(&consumer);
        readySet.insert(&notifier);

        EXPECT_EQ(readySet.size(), 2u);
        EXPECT_EQ(*readySet.begin(), static_cast<swss::Selectable *>(&notifier));
    }
}
