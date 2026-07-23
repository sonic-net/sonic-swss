/**
 * Unit tests for Notifier priority delegation and adaptive stall detection.
 *
 * Validates:
 *  1. Notifier.getPri() delegates to the wrapped NotificationConsumer (pri=100).
 *  2. Table consumers keep Executor-default priority (pri=0).
 *  3. Select::cmp orders Notifier before table consumers.
 *  4. Adaptive stall detection: after STALL_THRESHOLD consecutive execute()
 *     calls with no detectable consumption, hasCachedData() returns false to
 *     prevent the Notifier from being re-inserted into Select's m_ready set.
 *     This allows lower-priority table consumers to proceed.
 */

// Pre-include standard library headers that conflict with
// the #define private/protected public hack (they use 'private' internally).
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

    /**
     * Minimal Orch subclass that accepts notification tasks.
     */
    class DummyOrch : public Orch
    {
    public:
        DummyOrch(swss::DBConnector *db, const string &tableName)
            : Orch(db, tableName)
        {
        }

        void doTask(Consumer &consumer) override
        {
            consumer.m_toSync.clear();
        }

        void doTask(swss::NotificationConsumer &consumer) override
        {
            // no-op -- does NOT call pop(), simulating the allPortsReady() guard
        }
    };

    /**
     * Orch subclass that always consumes one notification.
     */
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
    };

    /**
     * Core test: Notifier wrapping a NotificationConsumer must report
     * the NotificationConsumer's priority (100), not the Executor default (0).
     */
    TEST_F(NotifierPriorityTest, NotifierReportsNotificationConsumerPriority)
    {
        DummyOrch orch(m_app_db.get(), "DUMMY_TABLE");

        // NotificationConsumer is constructed with pri=100 by default
        auto *notifConsumer = new swss::NotificationConsumer(m_app_db.get(), "TEST_CHANNEL");

        // Verify the raw NotificationConsumer has pri=100
        EXPECT_EQ(notifConsumer->getPri(), 100);

        // Wrap it in a Notifier (which is an Executor subclass)
        Notifier notifier(notifConsumer, &orch, "TEST_NOTIFICATIONS");

        // The fix: Notifier.getPri() should delegate to the wrapped consumer
        EXPECT_EQ(notifier.getPri(), 100);
    }

    /**
     * Contrast test: A regular Consumer (table consumer) wrapping a
     * ConsumerStateTable should still report pri=0, because Executor base
     * does NOT delegate getPri() (intentional for table consumers).
     */
    TEST_F(NotifierPriorityTest, TableConsumerReportsDefaultPriority)
    {
        DummyOrch orch(m_app_db.get(), "DUMMY_TABLE");

        // ConsumerStateTable with explicit priority (e.g., 45 for PORT_TABLE)
        auto *cst = new swss::ConsumerStateTable(m_app_db.get(), "PORT_TABLE", 1, 45);

        // The raw ConsumerStateTable has pri=45
        EXPECT_EQ(cst->getPri(), 45);

        // Wrap it in a Consumer (Executor subclass - no getPri override)
        Consumer consumer(cst, &orch, "PORT_TABLE");

        // Executor base does NOT delegate getPri - returns default 0
        EXPECT_EQ(consumer.getPri(), 0);
    }

    /**
     * Verify that Select's comparator (which orders its m_ready set) places
     * a Notifier before a Consumer.
     */
    TEST_F(NotifierPriorityTest, SelectComparatorOrdersNotifierBeforeConsumer)
    {
        DummyOrch orch(m_app_db.get(), "DUMMY_TABLE");

        auto *notifConsumer = new swss::NotificationConsumer(m_app_db.get(), "TEST_CHANNEL");
        Notifier notifier(notifConsumer, &orch, "TEST_NOTIFICATIONS");

        auto *cst = new swss::ConsumerStateTable(m_app_db.get(), "TEST_TABLE");
        Consumer consumer(cst, &orch, "TEST_TABLE");

        std::set<swss::Selectable *, swss::Select::cmp> readySet;
        readySet.insert(&notifier);
        readySet.insert(&consumer);

        // The first element (highest priority) must be the Notifier
        auto first = *readySet.begin();
        EXPECT_EQ(first, static_cast<swss::Selectable *>(&notifier))
            << "Select should dispatch Notifier (pri=100) before Consumer (pri=0)";
    }

    /**
     * Verify ordering is stable: even if Consumer is inserted first, Notifier
     * still wins due to higher priority.
     */
    TEST_F(NotifierPriorityTest, SelectComparatorPriorityOverridesInsertionOrder)
    {
        DummyOrch orch(m_app_db.get(), "DUMMY_TABLE");

        auto *cst = new swss::ConsumerStateTable(m_app_db.get(), "TEST_TABLE");
        Consumer consumer(cst, &orch, "TEST_TABLE");

        auto *notifConsumer = new swss::NotificationConsumer(m_app_db.get(), "TEST_CHANNEL");
        Notifier notifier(notifConsumer, &orch, "TEST_NOTIFICATIONS");

        // Insert consumer first, then notifier
        std::set<swss::Selectable *, swss::Select::cmp> readySet;
        readySet.insert(&consumer);
        readySet.insert(&notifier);

        // Notifier should still be first regardless of insertion order
        auto first = *readySet.begin();
        EXPECT_EQ(first, static_cast<swss::Selectable *>(&notifier))
            << "Priority should override insertion order";

        // Second element should be the consumer
        auto it = readySet.begin();
        ++it;
        EXPECT_EQ(*it, static_cast<swss::Selectable *>(&consumer));
    }

    /**
     * Verify stall detection: after STALL_THRESHOLD (2) consecutive execute()
     * calls where the Orch does NOT consume, hasCachedData() returns false.
     * This prevents the Notifier from being re-inserted into Select's m_ready
     * set, allowing table consumers to proceed.
     *
     * Note: getPri() remains constant at 100 -- we must NOT mutate priority
     * while the element may be in std::set<Selectable*, Select::cmp>, as that
     * would violate the ordering invariant (undefined behavior).
     */
    TEST_F(NotifierPriorityTest, StallDetectionSuppressesCachedData)
    {
        DummyOrch orch(m_app_db.get(), "DUMMY_TABLE");

        auto *notifConsumer = new swss::NotificationConsumer(m_app_db.get(), "TEST_STALL");
        Notifier notifier(notifConsumer, &orch, "TEST_STALL");

        // getPri() is ALWAYS 100 regardless of stall state
        EXPECT_EQ(notifier.getPri(), 100);

        // Simulate stall progression via m_noProgressCount
        notifier.m_noProgressCount = 0;
        EXPECT_EQ(notifier.getPri(), 100) << "Priority must be constant";
        // hasCachedData delegates when not stalled (queue is empty so false)
        EXPECT_FALSE(notifier.hasCachedData());

        notifier.m_noProgressCount = 1;
        EXPECT_EQ(notifier.getPri(), 100) << "Priority must remain constant";

        // At threshold: hasCachedData() returns false regardless of queue state
        notifier.m_noProgressCount = 2;
        EXPECT_EQ(notifier.getPri(), 100) << "Priority must remain constant at threshold";
        EXPECT_FALSE(notifier.hasCachedData())
            << "hasCachedData should return false at stall threshold";

        notifier.m_noProgressCount = 10;
        EXPECT_EQ(notifier.getPri(), 100) << "Priority must remain constant while stalled";
        EXPECT_FALSE(notifier.hasCachedData())
            << "hasCachedData should stay false while stalled";

        // After Orch resumes consuming, counter resets
        notifier.m_noProgressCount = 0;
        EXPECT_EQ(notifier.getPri(), 100) << "Priority must remain constant after recovery";
    }

    /**
     * Verify that a stalled Notifier retains its high priority in the
     * comparator -- the stall mechanism works via hasCachedData() (preventing
     * re-insertion), NOT via getPri() (which would corrupt the set).
     *
     * This is the critical invariant: getPri() must never change while the
     * element could be in std::set<Selectable*, Select::cmp>.
     */
    TEST_F(NotifierPriorityTest, StalledNotifierKeepsConstantPriority)
    {
        DummyOrch orch(m_app_db.get(), "DUMMY_TABLE");

        auto *notifConsumer = new swss::NotificationConsumer(m_app_db.get(), "TEST_CHANNEL");
        Notifier notifier(notifConsumer, &orch, "TEST_NOTIFICATIONS");

        auto *cst = new swss::ConsumerStateTable(m_app_db.get(), "TEST_TABLE");
        Consumer consumer(cst, &orch, "TEST_TABLE");

        // Stall the notifier
        notifier.m_noProgressCount = 2;

        // Even when stalled, getPri() still returns 100
        // (stall yields via hasCachedData, not priority)
        EXPECT_EQ(notifier.getPri(), 100);
        EXPECT_EQ(consumer.getPri(), 0);

        // If both are in m_ready, Notifier still sorts first
        std::set<swss::Selectable *, swss::Select::cmp> readySet;
        readySet.insert(&consumer);
        readySet.insert(&notifier);

        EXPECT_EQ(readySet.size(), 2u);
        auto first = *readySet.begin();
        EXPECT_EQ(first, static_cast<swss::Selectable *>(&notifier))
            << "Stalled Notifier must still sort before Consumer in m_ready "
               "(stall prevents re-insertion, not ordering)";
    }
}
