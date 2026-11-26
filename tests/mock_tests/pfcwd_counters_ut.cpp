#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_orch_test.h"
#include "mock_table.h"

#include "pfcactionhandler.h"
#include "pfcwdorch.h"

#include <gtest/gtest.h>

namespace pfcwd_sw_counters_test
{
    using namespace std;
    using namespace mock_orch_test;

    // Namespace scope: an in-class static const is odr-used by make_shared. The
    // OIDs sit outside the vs-SAI allocator range, off any real queue's entry.
    constexpr sai_object_id_t kPort     = 0x1000000000001;
    constexpr sai_object_id_t kQueue    = 0xff00000000000001ULL;
    constexpr uint8_t         kQueueIdx = 3;

    // Drives the shared counter code the way PfcWdActionHandler does, with the
    // raw hardware sample supplied by the test instead of read from SAI.
    class FakeCounterHandler
    {
    public:
        FakeCounterHandler(shared_ptr<swss::Table> countersTable, string queueIdStr)
            : m_countersTable(countersTable), m_queueIdStr(queueIdStr)
        {
        }

        void initCounters()
        {
            if (!m_readable)
            {
                return;
            }

            PfcWdBaseOrch::beginStormCounters(m_countersTable, m_queueIdStr, m_sample, m_baseline);
            m_baselineValid = true;
        }

        void commitCounters(bool periodic = false)
        {
            if (!m_readable)
            {
                return;
            }

            PfcWdBaseOrch::accumulateStormCounters(m_countersTable, m_queueIdStr, m_sample,
                                                   m_baseline, periodic, m_baselineValid);
        }

        void setSample(uint64_t txPkt, uint64_t txDropPkt, uint64_t rxPkt, uint64_t rxDropPkt)
        {
            m_sample.txPkt = txPkt;
            m_sample.txDropPkt = txDropPkt;
            m_sample.rxPkt = rxPkt;
            m_sample.rxDropPkt = rxDropPkt;
        }

        PfcWdHwStats m_sample = {0, 0, 0, 0};
        bool m_readable = true;

    private:
        shared_ptr<swss::Table> m_countersTable;
        string m_queueIdStr;
        PfcWdHwStats m_baseline = {0, 0, 0, 0};
        bool m_baselineValid = false;
    };

    class PfcWdSwCountersTest : public MockOrchTest
    {
    protected:
        shared_ptr<swss::DBConnector> m_counters_db;
        shared_ptr<swss::Table> m_counters_table;

        void SetUp() override
        {
            MockOrchTest::SetUp();
            m_counters_db = make_shared<swss::DBConnector>("COUNTERS_DB", 0);
            m_counters_table = make_shared<swss::Table>(m_counters_db.get(), COUNTERS_TABLE);

            // Table::set() merges fields into a process-global map, so clear the
            // queue key to keep a prior test out of this one.
            m_counters_table->del(queueKey());
        }

        void TearDown() override
        {
            // Leave no residue in the process-global mock table for later suites.
            m_counters_table->del(queueKey());
            MockOrchTest::TearDown();
        }

        string queueKey() const
        {
            return sai_serialize_object_id(kQueue);
        }

        shared_ptr<FakeCounterHandler> makeHandler()
        {
            return make_shared<FakeCounterHandler>(m_counters_table, queueKey());
        }

        PfcWdQueueStats readBack()
        {
            return PfcWdBaseOrch::getQueueStats(m_counters_table, queueKey());
        }

        void seed(const PfcWdQueueStats& stats)
        {
            PfcWdBaseOrch::updateQueueStats(m_counters_table, queueKey(), stats);
        }

        PfcWdQueueStats zeroStats()
        {
            PfcWdQueueStats s;
            memset(&s, 0, sizeof(s));
            s.operational = true;
            return s;
        }
    };

    // A first storm bumps detectCount and marks the queue stormed.
    TEST_F(PfcWdSwCountersTest, InitCounters_NewStorm)
    {
        auto handler = makeHandler();
        handler->setSample(100, 10, 200, 20);

        handler->initCounters();

        auto stats = readBack();
        EXPECT_EQ(stats.detectCount, 1u);
        EXPECT_EQ(stats.restoreCount, 0u);
        EXPECT_FALSE(stats.operational);
    }

    // A storm already in progress (detectCount > restoreCount, e.g. resumed
    // across warm reboot) must not bump detectCount again.
    TEST_F(PfcWdSwCountersTest, InitCounters_WarmRebootOngoingStorm)
    {
        auto seeded = zeroStats();
        seeded.detectCount = 5;
        seeded.restoreCount = 4;
        seeded.txPktLast = 77;
        seed(seeded);

        auto handler = makeHandler();
        handler->setSample(100, 10, 200, 20);
        handler->initCounters();

        auto stats = readBack();
        EXPECT_EQ(stats.detectCount, 5u);
        EXPECT_EQ(stats.restoreCount, 4u);
        EXPECT_EQ(stats.txPktLast, 77u);   // preserved, not cleared
        EXPECT_FALSE(stats.operational);
    }

    // A new storm clears the per-storm _LAST counters.
    TEST_F(PfcWdSwCountersTest, InitCounters_NewStormClearsLastCounters)
    {
        auto seeded = zeroStats();
        seeded.detectCount = 2;
        seeded.restoreCount = 2;
        seeded.txPktLast = 77;
        seeded.rxPktLast = 88;
        seed(seeded);

        auto handler = makeHandler();
        handler->setSample(100, 10, 200, 20);
        handler->initCounters();

        auto stats = readBack();
        EXPECT_EQ(stats.detectCount, 3u);
        EXPECT_EQ(stats.txPktLast, 0u);
        EXPECT_EQ(stats.rxPktLast, 0u);
    }

    // initCounters has the same getHwCounters() guard as commitCounters: an
    // unreadable sample must leave COUNTERS_DB untouched (no detectCount bump).
    TEST_F(PfcWdSwCountersTest, InitCounters_UnreadableSampleIsNoOp)
    {
        auto handler = makeHandler();
        handler->m_readable = false;

        handler->initCounters();

        auto stats = readBack();
        EXPECT_EQ(stats.detectCount, 0u);
        EXPECT_TRUE(stats.operational);
    }

    // A periodic commit accumulates the delta since the storm began, into both
    // the running totals and the per-storm _LAST counters, and leaves the queue
    // stormed.
    TEST_F(PfcWdSwCountersTest, CommitCounters_PeriodicAccumulatesDelta)
    {
        auto handler = makeHandler();
        handler->setSample(100, 10, 200, 20);
        handler->initCounters();

        handler->setSample(150, 15, 260, 26);
        handler->commitCounters(true);

        auto stats = readBack();
        EXPECT_EQ(stats.txPkt, 50u);
        EXPECT_EQ(stats.txDropPkt, 5u);
        EXPECT_EQ(stats.rxPkt, 60u);
        EXPECT_EQ(stats.rxDropPkt, 6u);
        EXPECT_EQ(stats.txPktLast, 50u);
        EXPECT_EQ(stats.txDropPktLast, 5u);
        EXPECT_EQ(stats.rxPktLast, 60u);
        EXPECT_EQ(stats.rxDropPktLast, 6u);
        EXPECT_EQ(stats.restoreCount, 0u);
        EXPECT_FALSE(stats.operational);
    }

    // Successive periodic commits accumulate rather than overwrite.
    TEST_F(PfcWdSwCountersTest, CommitCounters_PeriodicMultipleUpdates)
    {
        auto handler = makeHandler();
        handler->setSample(100, 10, 200, 20);
        handler->initCounters();

        handler->setSample(150, 15, 260, 26);
        handler->commitCounters(true);
        handler->setSample(170, 16, 280, 28);
        handler->commitCounters(true);

        auto stats = readBack();
        EXPECT_EQ(stats.txPkt, 70u);
        EXPECT_EQ(stats.txDropPkt, 6u);
        EXPECT_EQ(stats.rxPkt, 80u);
        EXPECT_EQ(stats.rxDropPkt, 8u);
    }

    // No traffic between polls leaves the counters untouched.
    TEST_F(PfcWdSwCountersTest, CommitCounters_PeriodicNoTraffic)
    {
        auto handler = makeHandler();
        handler->setSample(100, 10, 200, 20);
        handler->initCounters();

        handler->commitCounters(true);

        auto stats = readBack();
        EXPECT_EQ(stats.txPkt, 0u);
        EXPECT_EQ(stats.rxPkt, 0u);
        EXPECT_FALSE(stats.operational);
    }

    // Recovery bumps restoreCount and returns the queue to operational; the
    // packet delta lands in both the running totals and the _LAST counters,
    // same as a periodic commit.
    TEST_F(PfcWdSwCountersTest, CommitCounters_Restoration)
    {
        auto handler = makeHandler();
        handler->setSample(100, 10, 200, 20);
        handler->initCounters();

        handler->setSample(150, 15, 260, 26);
        handler->commitCounters(false);

        auto stats = readBack();
        EXPECT_EQ(stats.detectCount, 1u);
        EXPECT_EQ(stats.restoreCount, 1u);
        EXPECT_EQ(stats.txPkt, 50u);
        EXPECT_EQ(stats.txPktLast, 50u);
        EXPECT_EQ(stats.txDropPktLast, 5u);
        EXPECT_EQ(stats.rxPktLast, 60u);
        EXPECT_EQ(stats.rxDropPktLast, 6u);
        EXPECT_TRUE(stats.operational);
    }

    // An unreadable hardware sample must leave COUNTERS_DB alone.
    TEST_F(PfcWdSwCountersTest, CommitCounters_UnreadableSampleIsNoOp)
    {
        auto handler = makeHandler();
        handler->setSample(100, 10, 200, 20);
        handler->initCounters();
        auto before = readBack();

        handler->m_readable = false;
        handler->commitCounters(true);

        auto after = readBack();
        EXPECT_EQ(after.txPkt, before.txPkt);
        EXPECT_EQ(after.detectCount, before.detectCount);
    }

    // initWdCounters writes only detect/restore/status, deliberately leaving the
    // packet counters in place across a reconfiguration.
    TEST_F(PfcWdSwCountersTest, InitWdCounters_PreservesPacketCounters)
    {
        auto seeded = zeroStats();
        seeded.detectCount = 4;
        seeded.restoreCount = 4;
        seeded.txPkt = 1234;
        seeded.rxPkt = 5678;
        seeded.operational = false;
        seed(seeded);

        PfcWdActionHandler::initWdCounters(m_counters_table, queueKey());

        auto stats = readBack();
        EXPECT_EQ(stats.detectCount, 4u);
        EXPECT_EQ(stats.restoreCount, 4u);
        EXPECT_EQ(stats.txPkt, 1234u);     // preserved
        EXPECT_EQ(stats.rxPkt, 5678u);     // preserved
        EXPECT_TRUE(stats.operational);
    }

    // A backward-moving hardware sample is a counter reset, not a wrap: what the
    // counter has reached since the reset belongs to the storm, and the baseline
    // resyncs so the next delta is correct.
    TEST_F(PfcWdSwCountersTest, CommitCounters_CounterResetCountsFromZero)
    {
        auto handler = makeHandler();
        handler->setSample(100, 10, 200, 20);
        handler->initCounters();

        handler->setSample(40, 10, 200, 20);   // txPkt went backward
        handler->commitCounters(true);

        auto stats = readBack();
        EXPECT_EQ(stats.txPkt, 40u);           // counted from the reset, not wrapped
        EXPECT_EQ(stats.txPktLast, 40u);

        handler->setSample(60, 10, 200, 20);   // resumes from the new baseline
        handler->commitCounters(true);

        auto after = readBack();
        EXPECT_EQ(after.txPkt, 60u);
    }

    // A reset is per field; the others still count their own delta.
    TEST_F(PfcWdSwCountersTest, CommitCounters_CounterResetIsPerField)
    {
        auto handler = makeHandler();
        handler->setSample(100, 10, 200, 20);
        handler->initCounters();

        handler->setSample(40, 15, 260, 26);   // only txPkt went backward
        handler->commitCounters(true);

        auto stats = readBack();
        EXPECT_EQ(stats.txPkt, 40u);
        EXPECT_EQ(stats.txDropPkt, 5u);
        EXPECT_EQ(stats.rxPkt, 60u);
        EXPECT_EQ(stats.rxDropPkt, 6u);
    }

    // All four fields reset at once. Each of the eight the software path touches
    // - four running totals and their paired _LAST counters - counts from the
    // reset rather than wrapping.
    TEST_F(PfcWdSwCountersTest, CommitCounters_AllCountersCountFromReset)
    {
        auto handler = makeHandler();
        handler->setSample(100, 10, 200, 20);
        handler->initCounters();

        handler->setSample(40, 4, 80, 8);   // all four went backward
        handler->commitCounters(true);

        auto stats = readBack();
        EXPECT_EQ(stats.txPkt, 40u);
        EXPECT_EQ(stats.txDropPkt, 4u);
        EXPECT_EQ(stats.rxPkt, 80u);
        EXPECT_EQ(stats.rxDropPkt, 8u);
        EXPECT_EQ(stats.txPktLast, 40u);
        EXPECT_EQ(stats.txDropPktLast, 4u);
        EXPECT_EQ(stats.rxPktLast, 80u);
        EXPECT_EQ(stats.rxDropPktLast, 8u);
    }
}
