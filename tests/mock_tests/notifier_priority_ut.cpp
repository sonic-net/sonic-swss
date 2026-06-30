/**
 * Unit test: Verify that Notifier.getPri() delegates to the wrapped
 * NotificationConsumer (pri=100), so the Select event loop dispatches
 * notifications before table consumers (pri=0).
 *
 * This validates the fix for the "priority dead code" bug where the Executor
 * base class did not override getPri(), causing all wrapped Selectables to
 * report pri=0 regardless of their actual priority.
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
#undef private
#undef protected

#include "notifier.h"
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
            // no-op for test
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
        // This is INTENTIONAL: table priorities should not affect cross-orch scheduling
        EXPECT_EQ(consumer.getPri(), 0);
    }

    /**
     * Verify that Select's comparator (which orders its m_ready set) places
     * a Notifier before a Consumer. This is the actual mechanism that gives
     * notifications dispatch priority in the event loop.
     *
     * Select::cmp returns true if 'a' should come before 'b' (higher priority first).
     */
    TEST_F(NotifierPriorityTest, SelectComparatorOrdersNotifierBeforeConsumer)
    {
        DummyOrch orch(m_app_db.get(), "DUMMY_TABLE");

        auto *notifConsumer = new swss::NotificationConsumer(m_app_db.get(), "TEST_CHANNEL");
        Notifier notifier(notifConsumer, &orch, "TEST_NOTIFICATIONS");

        auto *cst = new swss::ConsumerStateTable(m_app_db.get(), "TEST_TABLE");
        Consumer consumer(cst, &orch, "TEST_TABLE");

        // Use the same comparator that Select uses internally for its m_ready set.
        // This set determines dispatch order: begin() is returned first.
        std::set<swss::Selectable *, swss::Select::cmp> readySet;
        readySet.insert(&notifier);
        readySet.insert(&consumer);

        // The first element in readySet (highest priority) must be the Notifier
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

} // namespace notifier_priority_test