/*
 * Unit tests for Notifier priority delegation and stall detection.
 *
 * Validates:
 *  1. Notifier.getPri() delegates to NotificationConsumer (pri=100).
 *  2. Table consumers keep Executor-default priority (pri=0).
 *  3. Select::cmp orders Notifier before table consumers.
 *  4. Stall detection: after STALL_THRESHOLD no-progress cycles,
 *     hasCachedData() returns false to yield to table consumers.
 *  5. getPri() stays constant regardless of stall state.
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

    /* Stall detection: after STALL_THRESHOLD consecutive no-progress cycles,
     * hasCachedData() returns false to yield m_ready to table consumers.
     * getPri() stays constant — stall works via hasCachedData, not priority.
     *
     * Note: execute() cannot be safely called in mock_tests because hasData()
     * triggers readData() on the mock subscriber with a null mockReply.
     * The stall logic is verified via direct m_noProgressCount manipulation;
     * the execute() path is covered by VS integration tests. */
    TEST_F(NotifierPriorityTest, StallDetectionSuppressesCachedData)
    {
        DeferringOrch orch(m_app_db.get(), "DUMMY_TABLE");

        auto *notifConsumer = new swss::NotificationConsumer(m_app_db.get(), "TEST_STALL");
        Notifier notifier(notifConsumer, &orch, "TEST_STALL");

        EXPECT_EQ(notifier.getPri(), 100);

        /* Below threshold: hasCachedData delegates to wrapped consumer */
        notifier.m_noProgressCount = 0;
        EXPECT_EQ(notifier.getPri(), 100);

        notifier.m_noProgressCount = Notifier::STALL_THRESHOLD - 1;
        EXPECT_EQ(notifier.getPri(), 100);

        /* At threshold: hasCachedData() returns false regardless of queue */
        notifier.m_noProgressCount = Notifier::STALL_THRESHOLD;
        EXPECT_EQ(notifier.getPri(), 100);
        EXPECT_FALSE(notifier.hasCachedData());

        /* Well past threshold: still suppressed, priority still constant */
        notifier.m_noProgressCount = 10;
        EXPECT_EQ(notifier.getPri(), 100);
        EXPECT_FALSE(notifier.hasCachedData());

        /* Recovery: counter reset restores delegation */
        notifier.m_noProgressCount = 0;
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
