#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_orch_test.h"
#include "mock_table.h"

#define private public
#define protected public
#include "pfcwdhworch.h"
#undef protected
#undef private

#include <gtest/gtest.h>

namespace pfcwdhworch_test
{
    using namespace std;
    using namespace mock_orch_test;

    // What the hooked SAI queue stat get returns; a queue with no entry fails
    map<sai_object_id_t, pair<uint64_t, uint64_t>> _ut_sai_queue_stats;
    uint32_t _ut_sai_queue_stat_reads = 0;

    sai_queue_api_t ut_sai_queue_api;
    sai_queue_api_t *pold_sai_queue_api;

    sai_status_t _ut_stub_sai_get_queue_stats(
        sai_object_id_t queue_id,
        uint32_t number_of_counters,
        const sai_stat_id_t *counter_ids,
        uint64_t *counters)
    {
        _ut_sai_queue_stat_reads++;
        auto it = _ut_sai_queue_stats.find(queue_id);
        if (it == _ut_sai_queue_stats.end())
        {
            return SAI_STATUS_FAILURE;
        }

        for (uint32_t i = 0; i < number_of_counters; i++)
        {
            counters[i] = counter_ids[i] == SAI_QUEUE_STAT_PACKETS ? it->second.first :
                          counter_ids[i] == SAI_QUEUE_STAT_DROPPED_PACKETS ? it->second.second : 0;
        }
        return SAI_STATUS_SUCCESS;
    }

    void _hook_sai_queue_stats()
    {
        ut_sai_queue_api = *sai_queue_api;
        pold_sai_queue_api = sai_queue_api;
        ut_sai_queue_api.get_queue_stats = _ut_stub_sai_get_queue_stats;
        sai_queue_api = &ut_sai_queue_api;
    }

    void _unhook_sai_queue_stats()
    {
        sai_queue_api = pold_sai_queue_api;
        _ut_sai_queue_stats.clear();
        _ut_sai_queue_stat_reads = 0;
    }

        class PfcWdHwCountersTest : public MockOrchTest
        {
        protected:
            shared_ptr<swss::DBConnector> m_counters_db;
            shared_ptr<swss::Table> m_counters_table;
            PfcWdHwOrch *m_pfcwd_hw_orch;

            void SetUp() override
            {
                MockOrchTest::SetUp();
                _hook_sai_queue_stats();

                // Bring up ports so gPortsOrch can resolve Ethernet0 and its queues/PGs.
                bringUpPorts();

                // Initialize COUNTERS_DB and table
                m_counters_db = make_shared<swss::DBConnector>("COUNTERS_DB", 0);
                m_counters_table = make_shared<swss::Table>(m_counters_db.get(), COUNTERS_TABLE);

                vector<string> pfc_wd_tables = { CFG_PFC_WD_TABLE_NAME };

                static const vector<sai_port_stat_t> portStatIds = {};

                static const vector<sai_queue_stat_t> queueStatIds = {
                    SAI_QUEUE_STAT_PACKETS,
                    SAI_QUEUE_STAT_DROPPED_PACKETS
                };

                static const vector<sai_queue_attr_t> queueAttrIds = {};

                m_pfcwd_hw_orch = new PfcWdHwOrch(
                    m_config_db.get(), 
                    pfc_wd_tables, 
                    portStatIds, 
                    queueStatIds, 
                    queueAttrIds);
            }

            void TearDown() override
            {
                delete m_pfcwd_hw_orch;
                _unhook_sai_queue_stats();
                MockOrchTest::TearDown();
            }

            // Populate APP_PORT_TABLE and run PortsOrch so ports are ready.
            void bringUpPorts()
            {
                swss::Table portTable(m_app_db.get(), APP_PORT_TABLE_NAME);
                auto ports = ut_helper::getInitialSaiPorts();

                for (const auto &it : ports)
                {
                    portTable.set(it.first, it.second);
                }

                portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
                portTable.set("PortInitDone",   { { "lanes", "0" } });

                gPortsOrch->addExistingData(&portTable);
                static_cast<Orch *>(gPortsOrch)->doTask();
                static_cast<Orch *>(gPortsOrch)->doTask();
                ASSERT_TRUE(gPortsOrch->allPortsReady());
            }

            // Helper: Set the hardware counter values SAI returns for a queue
            void setQueueHwCounters(sai_object_id_t queueId, uint64_t txPkt, uint64_t txDropPkt)
            {
                _ut_sai_queue_stats[queueId] = { txPkt, txDropPkt };
            }

            // Helper: Set hardware counter values in COUNTERS_DB for a priority group
            void setPgHwCounters(sai_object_id_t pgId, uint64_t rxPkt, uint64_t rxDropPkt)
            {
                string pgIdStr = sai_serialize_object_id(pgId);
                vector<FieldValueTuple> fvs = {
                    {"SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS", to_string(rxPkt)},
                    {"SAI_INGRESS_PRIORITY_GROUP_STAT_DROPPED_PACKETS", to_string(rxDropPkt)}
                };
                m_counters_table->set(pgIdStr, fvs);
            }

            // Helper: Get queue statistics from COUNTERS_DB via the orch method
            PfcWdQueueStats getQueueStats(sai_object_id_t queueId)
            {
                string queueIdStr = sai_serialize_object_id(queueId);
                return m_pfcwd_hw_orch->getQueueStats(queueIdStr);
            }

            // Helper: Verify counter values match expectations
            void verifyQueueStats(sai_object_id_t queueId,
                                 uint64_t expectedDetect, 
                                 uint64_t expectedRestore,
                                 uint64_t expectedTxPkt,
                                 uint64_t expectedTxDropPkt,
                                 bool expectedOperational)
            {
                auto stats = getQueueStats(queueId);
                EXPECT_EQ(stats.detectCount, expectedDetect) << "detectCount mismatch";
                EXPECT_EQ(stats.restoreCount, expectedRestore) << "restoreCount mismatch";
                EXPECT_EQ(stats.txPkt, expectedTxPkt) << "txPkt mismatch";
                EXPECT_EQ(stats.txDropPkt, expectedTxDropPkt) << "txDropPkt mismatch";
                EXPECT_EQ(stats.operational, expectedOperational) << "operational status mismatch";
            }

            void resetQueueStats(sai_object_id_t queueId)
            {
                string queueIdStr = sai_serialize_object_id(queueId);

                // Delete the stats entry from COUNTERS_DB
                m_counters_table->del(queueIdStr);
                _ut_sai_queue_stats.erase(queueId);

                // Clear baseline so next initQueueCounters starts fresh
                m_pfcwd_hw_orch->m_queueBaselineStats.erase(queueId);
            }

            void setupQueuePortMapping(sai_object_id_t queueId, const Port& port, uint8_t queueIndex)
            {
                PfcWdHwOrch::PortQueueInfo info;
                info.port_id = port.m_port_id;
                info.port_alias = port.m_alias;
                info.queue_index = queueIndex;
                m_pfcwd_hw_orch->m_queueToPortMap[queueId] = info;
            }

            // Helper: Verify baseline counters are stored correctly
            void verifyBaseline(sai_object_id_t queueId, 
                               uint64_t expectedTxPkt,
                               uint64_t expectedTxDropPkt)
            {
                ASSERT_NE(m_pfcwd_hw_orch->m_queueBaselineStats.find(queueId),
                         m_pfcwd_hw_orch->m_queueBaselineStats.end()) 
                    << "Baseline not found for queue";

                auto& baseline = m_pfcwd_hw_orch->m_queueBaselineStats[queueId];
                EXPECT_EQ(baseline.txPkt, expectedTxPkt) << "Baseline txPkt mismatch";
                EXPECT_EQ(baseline.txDropPkt, expectedTxDropPkt) << "Baseline txDropPkt mismatch";
            }
        };

    // ========================================================================
    // Test Cases: initQueueCounters()
    // ========================================================================

    TEST_F(PfcWdHwCountersTest, InitQueueCounters_NewStorm)
    {
        // Get port from existing PortsOrch setup
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        // Other suites leave storm counters on this queue
        resetQueueStats(queueId);

        // Setup: Populate HW counters in COUNTERS_DB
        setQueueHwCounters(queueId, 1000, 50);

        // Setup: Add queue to port mapping
        setupQueuePortMapping(queueId, port, queueIndex);

        // Test: Call initQueueCounters (simulates storm detection)
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // Verify: detectCount = 1, restoreCount = 0
        // Verify: operational = false (storm state)
        // Verify: *_LAST counters = 0 (reset on detection)
        // Verify: cumulative counters = 0 (new storm)
        verifyQueueStats(queueId,
                        1,      // detectCount
                        0,      // restoreCount
                        0,      // txPkt
                        0,      // txDropPkt
                        false); // operational

        // Verify: Baseline stored with current HW values
        verifyBaseline(queueId, 1000, 50);
    }

    TEST_F(PfcWdHwCountersTest, InitQueueCounters_SubsequentStorm)
    {
        // Setup: Simulate previous storm that was restored
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        setupQueuePortMapping(queueId, port, queueIndex);

        // Pre-populate COUNTERS_DB with previous storm history
        PfcWdQueueStats prevStats;
        memset(&prevStats, 0, sizeof(prevStats));
        prevStats.detectCount = 1;
        prevStats.restoreCount = 1;
        prevStats.txPkt = 500;
        prevStats.txDropPkt = 25;
        prevStats.rxPkt = 1000;
        prevStats.rxDropPkt = 50;
        prevStats.operational = true;
        m_pfcwd_hw_orch->updateQueueStats(queueIdStr, prevStats);

        // Setup: New HW counter values for second storm
        setQueueHwCounters(queueId, 5000, 200);

        // Test: Call initQueueCounters for second storm
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.detectCount, 2);
        EXPECT_EQ(stats.restoreCount, 1);
        EXPECT_EQ(stats.operational, false);

        // Verify: *_LAST counters reset to 0 for new storm
        EXPECT_EQ(stats.txPktLast, 0);
        EXPECT_EQ(stats.txDropPktLast, 0);
        EXPECT_EQ(stats.rxPktLast, 0);
        EXPECT_EQ(stats.rxDropPktLast, 0);

        // Verify: Cumulative counters from previous storm preserved
        EXPECT_EQ(stats.txPkt, 500);
        EXPECT_EQ(stats.txDropPkt, 25);

        // Verify: New baseline set
        verifyBaseline(queueId, 5000, 200);
    }

    TEST_F(PfcWdHwCountersTest, InitQueueCounters_WarmRebootOngoingStorm)
    {
        // Test scenario: Storm was ongoing during warm reboot
        // detectCount > restoreCount, so we don't increment detectCount again
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        setupQueuePortMapping(queueId, port, queueIndex);

        // Pre-populate: Storm detected before warm reboot
        PfcWdQueueStats prevStats;
        memset(&prevStats, 0, sizeof(prevStats));
        prevStats.detectCount = 1;
        prevStats.restoreCount = 0;  // Storm still ongoing
        prevStats.operational = false;
        m_pfcwd_hw_orch->updateQueueStats(queueIdStr, prevStats);

        // Setup: HW counters
        setQueueHwCounters(queueId, 3000, 100);

        // Test: Call initQueueCounters (warm reboot recovery)
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // Verify: detectCount NOT incremented (still 1)
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.detectCount, 1) << "detectCount should not increment on warm reboot recovery";
        EXPECT_EQ(stats.restoreCount, 0);
        EXPECT_EQ(stats.operational, false);

        // Verify: Baseline re-established for continued monitoring
        verifyBaseline(queueId, 3000, 100);
    }

    // ========================================================================
    // Test Cases: updateQueueCounters() - Periodic Updates
    // ========================================================================

    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_PeriodicFirstUpdate)
    {
        // Test first periodic update during an ongoing storm
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        // Setup: Initialize counters (storm detected)
        setQueueHwCounters(queueId, 1000, 50);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // Setup: Update HW counters (traffic during storm)
        setQueueHwCounters(queueId, 1500, 75);

        // Test: Call updateQueueCounters with periodic=true
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        // Verify: Deltas accumulated
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.txPkt, 500) << "txPkt should be delta: 1500 - 1000";
        EXPECT_EQ(stats.txDropPkt, 25) << "txDropPkt should be delta: 75 - 50";

        // Verify: *_LAST counters also updated
        EXPECT_EQ(stats.txPktLast, 500);
        EXPECT_EQ(stats.txDropPktLast, 25);

        // Verify: Still in storm state
        EXPECT_EQ(stats.operational, false);
        EXPECT_EQ(stats.detectCount, 1);
        EXPECT_EQ(stats.restoreCount, 0) << "restoreCount should not change on periodic update";

        // Verify: Baseline updated to current HW values
        verifyBaseline(queueId, 1500, 75);
    }

    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_PeriodicMultipleUpdates)
    {
        // Test multiple periodic updates accumulate correctly
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        // Setup: Initialize counters
        setQueueHwCounters(queueId, 1000, 50);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // First periodic update
        setQueueHwCounters(queueId, 1500, 75);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        // Second periodic update
        setQueueHwCounters(queueId, 2200, 110);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        // Third periodic update
        setQueueHwCounters(queueId, 3000, 150);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        // Verify: Cumulative counters = sum of all deltas
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.txPkt, 2000) << "Should be total: (1500-1000) + (2200-1500) + (3000-2200)";
        EXPECT_EQ(stats.txDropPkt, 100) << "Should be total: (75-50) + (110-75) + (150-110)";

        // Verify: *_LAST counters also accumulate
        EXPECT_EQ(stats.txPktLast, 2000);
        EXPECT_EQ(stats.txDropPktLast, 100);

        // Verify: Final baseline is latest HW value
        verifyBaseline(queueId, 3000, 150);
    }

    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_PeriodicNoTraffic)
    {
        // Test periodic update when no new traffic (HW counters unchanged)
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        // Setup: Initialize counters
        setQueueHwCounters(queueId, 1000, 50);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        // Verify: All delta counters remain 0
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.txPkt, 0);
        EXPECT_EQ(stats.txDropPkt, 0);
        EXPECT_EQ(stats.rxPkt, 0);
        EXPECT_EQ(stats.rxDropPkt, 0);
        EXPECT_EQ(stats.operational, false);
    }

    // ========================================================================
    // Test Cases: updateQueueCounters() - Restoration
    // ========================================================================

    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_Restoration)
    {
        // Test storm restoration (periodic=false)
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        // Setup: Storm detected and some traffic during storm
        setQueueHwCounters(queueId, 1000, 50);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // Periodic update during storm
        setQueueHwCounters(queueId, 2000, 100);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        // Test: Restoration (periodic=false)
        setQueueHwCounters(queueId, 3000, 150);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, false);

        // Verify: restoreCount incremented
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.restoreCount, 1);
        EXPECT_EQ(stats.detectCount, 1);

        // Verify: operational = true (restored)
        EXPECT_EQ(stats.operational, true);

        // Verify: Deltas still calculated and accumulated
        EXPECT_EQ(stats.txPkt, 2000) << "Total: (2000-1000) + (3000-2000)";
        EXPECT_EQ(stats.txDropPkt, 100) << "Total: (100-50) + (150-100)";
    }

    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_RestorationAfterMultiplePeriodic)
    {
        // Test restoration after multiple periodic updates
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        // Init
        setQueueHwCounters(queueId, 1000, 50);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // Multiple periodic updates
        for (int i = 1; i <= 5; i++)
        {
            setQueueHwCounters(queueId, 1000 + (i * 500), 50 + (i * 25));
            m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);
        }

        // Final restoration
        setQueueHwCounters(queueId, 5000, 250);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, false);

        // Verify restoration
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.restoreCount, 1);
        EXPECT_EQ(stats.operational, true);
        EXPECT_EQ(stats.txPkt, 4000) << "Total accumulated: 5000 - 1000";
        EXPECT_EQ(stats.txDropPkt, 200) << "Total accumulated: 250 - 50";
    }

    // ========================================================================
    // Test Cases: updateQueueCounters() - No Baseline (Pre-Storm Poll)
    // ========================================================================

    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_NoBaselineSeedsWithoutDeltas)
    {
        // Test a queue polled before its first storm: no baseline exists yet.
        // updateQueueCounters must still apply restoreCount/operational and seed
        // the baseline, but must not apply any deltas.
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        // Deliberately do NOT call initQueueCounters(): no baseline must exist.
        // resetQueueStats() already erased any leftover baseline, but make the
        // precondition explicit since it is the whole point of this test.
        m_pfcwd_hw_orch->m_queueBaselineStats.erase(queueId);

        // Setup: Non-zero HW counters
        setQueueHwCounters(queueId, 1000, 50);

        // Test: Call updateQueueCounters with no baseline present (periodic=false)
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, false);

        // Verify: restoreCount incremented and operational restored, but no
        // deltas applied since there was no baseline to diff against.
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.restoreCount, 1);
        EXPECT_EQ(stats.detectCount, 0);
        EXPECT_EQ(stats.operational, true);
        EXPECT_EQ(stats.txPkt, 0);
        EXPECT_EQ(stats.txDropPkt, 0);
        EXPECT_EQ(stats.rxPkt, 0);
        EXPECT_EQ(stats.rxDropPkt, 0);
        EXPECT_EQ(stats.txPktLast, 0);
        EXPECT_EQ(stats.txDropPktLast, 0);
        EXPECT_EQ(stats.rxPktLast, 0);
        EXPECT_EQ(stats.rxDropPktLast, 0);

        // Verify: the baseline holds the current HW sample
        verifyBaseline(queueId, 1000, 50);
    }

    // ========================================================================
    // Test Cases: Counter Underflow Protection
    // ========================================================================

    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_CounterResetTxPkt)
    {
        // Test underflow protection when txPkt counter resets/wraps
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        // Setup: Initialize with high counter values
        setQueueHwCounters(queueId, 5000, 250);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // Test: HW counter reset/wrapped (txPkt < baseline)
        setQueueHwCounters(queueId, 100, 260);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        // Verify: txPkt counts from the reset rather than wrapping
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.txPkt, 100) << "txPkt should count from the reset";

        // Verify: Other counters that didn't wrap still update
        EXPECT_EQ(stats.txDropPkt, 10) << "txDropPkt should still update: 260 - 250";
    }

    TEST_F(PfcWdHwCountersTest, UpdateQueueCounters_AllCountersReset)
    {
        // Test when all HW counters reset (complete reset scenario)
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);

        // Setup: Initialize with high values
        setQueueHwCounters(queueId, 5000, 250);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // Test: All counters reset to 0
        setQueueHwCounters(queueId, 0, 0);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        // Verify: a reset to zero leaves nothing to count
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.txPkt, 0);
        EXPECT_EQ(stats.txDropPkt, 0);
        EXPECT_EQ(stats.rxPkt, 0);
        EXPECT_EQ(stats.rxDropPkt, 0);
    }

    // ========================================================================
    // Test Cases: readHwCounters()
    // ========================================================================

    TEST_F(PfcWdHwCountersTest, ReadHwCounters_Success)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];

        setQueueHwCounters(queueId, 1000, 50);

        PfcWdHwStats hwStats;
        EXPECT_TRUE(m_pfcwd_hw_orch->readHwCounters(queueId, hwStats));

        EXPECT_EQ(hwStats.txPkt, 1000);
        EXPECT_EQ(hwStats.txDropPkt, 50);
    }

    // The hardware watchdog acts only on the egress queue, so RX is never read
    // and a missing priority group entry does not affect the sample.
    TEST_F(PfcWdHwCountersTest, ReadHwCounters_LeavesRxZero)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];

        setQueueHwCounters(queueId, 1000, 50);
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 2000, 100);

        PfcWdHwStats hwStats;
        EXPECT_TRUE(m_pfcwd_hw_orch->readHwCounters(queueId, hwStats));

        EXPECT_EQ(hwStats.rxPkt, 0);
        EXPECT_EQ(hwStats.rxDropPkt, 0);
    }

    TEST_F(PfcWdHwCountersTest, ReadHwCounters_SaiReadFailureIsUnreadable)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];

        _ut_sai_queue_stats.erase(queueId);

        PfcWdHwStats hwStats;
        EXPECT_FALSE(m_pfcwd_hw_orch->readHwCounters(queueId, hwStats));

        EXPECT_EQ(hwStats.txPkt, 0);
        EXPECT_EQ(hwStats.txDropPkt, 0);
    }

    // The queue is read from SAI, so a stale copy another counter group left in
    // COUNTERS_DB is not a source.
    TEST_F(PfcWdHwCountersTest, ReadHwCounters_IgnoresCountersDbCopy)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];

        setQueueHwCounters(queueId, 1000, 50);
        m_counters_table->set(sai_serialize_object_id(queueId), {
            { "SAI_QUEUE_STAT_PACKETS", "7" },
            { "SAI_QUEUE_STAT_DROPPED_PACKETS", "3" } });

        PfcWdHwStats hwStats;
        ASSERT_TRUE(m_pfcwd_hw_orch->readHwCounters(queueId, hwStats));

        EXPECT_EQ(hwStats.txPkt, 1000);
        EXPECT_EQ(hwStats.txDropPkt, 50);
    }

    // Only the queues in storm are read; nothing else touches SAI.
    TEST_F(PfcWdHwCountersTest, PeriodicTimer_ReadsOnlyStormedQueues)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        sai_object_id_t stormed = port.m_queue_ids[3];
        sai_object_id_t idle = port.m_queue_ids[4];

        resetQueueStats(stormed);
        resetQueueStats(idle);
        setupQueuePortMapping(stormed, port, 3);
        setupQueuePortMapping(idle, port, 4);
        setQueueHwCounters(stormed, 1000, 50);
        setQueueHwCounters(idle, 2000, 0);
        m_pfcwd_hw_orch->m_stormedQueues.insert(stormed);

        _ut_sai_queue_stat_reads = 0;
        swss::SelectableTimer timer(timespec { .tv_sec = 1, .tv_nsec = 0 });
        m_pfcwd_hw_orch->doTask(timer);

        EXPECT_EQ(_ut_sai_queue_stat_reads, 1u);

        m_pfcwd_hw_orch->m_stormedQueues.erase(stormed);
    }

    // ========================================================================
    // Test Cases: getQueueStats() and updateQueueStats()
    // ========================================================================

    TEST_F(PfcWdHwCountersTest, GetQueueStats_NonExistent)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        sai_object_id_t queueId = port.m_queue_ids[7];

        resetQueueStats(queueId);
        auto stats = getQueueStats(queueId);

        // Verify: All fields are zero, but operational defaults to true (implementation behavior)
        EXPECT_EQ(stats.detectCount, 0);
        EXPECT_EQ(stats.restoreCount, 0);
        EXPECT_EQ(stats.txPkt, 0);
        EXPECT_EQ(stats.txDropPkt, 0);
        EXPECT_EQ(stats.rxPkt, 0);
        EXPECT_EQ(stats.rxDropPkt, 0);
        EXPECT_EQ(stats.txPktLast, 0);
        EXPECT_EQ(stats.txDropPktLast, 0);
        EXPECT_EQ(stats.rxPktLast, 0);
        EXPECT_EQ(stats.rxDropPktLast, 0);
        EXPECT_EQ(stats.operational, true);  // getQueueStats() defaults to true when entry missing
    }

    TEST_F(PfcWdHwCountersTest, UpdateAndGetQueueStats)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        sai_object_id_t queueId = port.m_queue_ids[3];
        string queueIdStr = sai_serialize_object_id(queueId);

        // Create and populate stats
        PfcWdQueueStats expectedStats;
        memset(&expectedStats, 0, sizeof(expectedStats));
        expectedStats.detectCount = 5;
        expectedStats.restoreCount = 3;
        expectedStats.txPkt = 12345;
        expectedStats.txDropPkt = 678;
        expectedStats.rxPkt = 98765;
        expectedStats.rxDropPkt = 432;
        expectedStats.txPktLast = 1000;
        expectedStats.txDropPktLast = 50;
        expectedStats.rxPktLast = 2000;
        expectedStats.rxDropPktLast = 100;
        expectedStats.operational = true;

        // Test: Update stats
        m_pfcwd_hw_orch->updateQueueStats(queueIdStr, expectedStats);

        // Test: Read stats back
        auto actualStats = getQueueStats(queueId);

        // Verify: All fields match
        EXPECT_EQ(actualStats.detectCount, expectedStats.detectCount);
        EXPECT_EQ(actualStats.restoreCount, expectedStats.restoreCount);
        EXPECT_EQ(actualStats.txPkt, expectedStats.txPkt);
        EXPECT_EQ(actualStats.txDropPkt, expectedStats.txDropPkt);
        EXPECT_EQ(actualStats.rxPkt, expectedStats.rxPkt);
        EXPECT_EQ(actualStats.rxDropPkt, expectedStats.rxDropPkt);
        EXPECT_EQ(actualStats.txPktLast, expectedStats.txPktLast);
        EXPECT_EQ(actualStats.txDropPktLast, expectedStats.txDropPktLast);
        EXPECT_EQ(actualStats.rxPktLast, expectedStats.rxPktLast);
        EXPECT_EQ(actualStats.rxDropPktLast, expectedStats.rxDropPktLast);
        EXPECT_EQ(actualStats.operational, expectedStats.operational);
    }

    // Reconfiguring the watchdog must not erase accumulated packet counters. It
    // is a stop-then-start, and zeroing here would look like a counter wrap to a
    // delta-based collector. sonic-clear pfcwd counters is how they are cleared.
    TEST_F(PfcWdHwCountersTest, InitializeQueueStats_PreservesPacketCounters)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        const uint8_t tc = 3;
        sai_object_id_t queueId = port.m_queue_ids[tc];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);

        PfcWdQueueStats seeded;
        memset(&seeded, 0, sizeof(seeded));
        seeded.detectCount = 2;
        seeded.restoreCount = 2;
        seeded.txPkt = 1234;
        seeded.txDropPkt = 111;
        seeded.rxPkt = 5678;
        seeded.rxDropPkt = 222;
        seeded.txPktLast = 333;
        seeded.txDropPktLast = 444;
        seeded.rxPktLast = 555;
        seeded.rxDropPktLast = 666;
        seeded.operational = false;
        m_pfcwd_hw_orch->updateQueueStats(queueIdStr, seeded);

        set<uint8_t> losslessTc = { tc };
        m_pfcwd_hw_orch->initializeQueueStats(port, losslessTc);

        auto stats = m_pfcwd_hw_orch->getQueueStats(queueIdStr);
        EXPECT_EQ(stats.detectCount, 2u);
        EXPECT_EQ(stats.restoreCount, 2u);
        EXPECT_EQ(stats.txPkt, 1234u);         // preserved
        EXPECT_EQ(stats.txDropPkt, 111u);      // preserved
        EXPECT_EQ(stats.rxPkt, 5678u);         // preserved
        EXPECT_EQ(stats.rxDropPkt, 222u);      // preserved
        EXPECT_EQ(stats.txPktLast, 333u);      // preserved
        EXPECT_EQ(stats.txDropPktLast, 444u);  // preserved
        EXPECT_EQ(stats.rxPktLast, 555u);      // preserved
        EXPECT_EQ(stats.rxDropPktLast, 666u);  // preserved
        EXPECT_TRUE(stats.operational);        // reset to operational
    }

    // A storm that was open when the watchdog went down has lost its RECOVERED,
    // so initialization closes it. Left open, the next storm's detect would be
    // swallowed by the resume guard in beginStormCounters.
    TEST_F(PfcWdHwCountersTest, InitializeQueueStats_ClosesStormLeftOpenAcrossRestart)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        const uint8_t tc = 3;
        sai_object_id_t queueId = port.m_queue_ids[tc];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, tc);

        PfcWdQueueStats persisted;
        memset(&persisted, 0, sizeof(persisted));
        persisted.detectCount = 3;
        persisted.restoreCount = 2;   // last storm never restored
        persisted.txPkt = 1234;
        persisted.operational = false;
        m_pfcwd_hw_orch->updateQueueStats(queueIdStr, persisted);

        m_pfcwd_hw_orch->initializeQueueStats(port, { tc });

        auto stats = m_pfcwd_hw_orch->getQueueStats(queueIdStr);
        EXPECT_EQ(stats.detectCount, 3u);
        EXPECT_EQ(stats.restoreCount, 3u);     // closed
        EXPECT_EQ(stats.txPkt, 1234u);         // accumulation preserved
        EXPECT_TRUE(stats.operational);

        // The next storm is a distinct one again: it bumps the detect count
        // and starts a fresh _LAST window.
        setQueueHwCounters(queueId, 5000, 250);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        stats = m_pfcwd_hw_orch->getQueueStats(queueIdStr);
        EXPECT_EQ(stats.detectCount, 4u);
        EXPECT_EQ(stats.restoreCount, 3u);
        EXPECT_FALSE(stats.operational);
    }

    // ========================================================================
    // Test Cases: FlexCounter registration and teardown
    // ========================================================================

    // The armed queues have to reach the PFC_WD group. Covered by the global
    // QUEUE group alone, a sample would be older than most storms. Without a
    // manager the registration is skipped silently, so assert it is there.
    TEST_F(PfcWdHwCountersTest, InitializeQueueStats_RegistersQueuesWithFlexCounter)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];

        ASSERT_NE(m_pfcwd_hw_orch->m_pfcwdFlexCounterManager, nullptr);
        auto& installed = m_pfcwd_hw_orch->m_pfcwdFlexCounterManager->installed_counters;
        ASSERT_EQ(installed.count(queueId), 0u);

        m_pfcwd_hw_orch->initializeQueueStats(port, { queueIndex });
        EXPECT_EQ(installed.count(queueId), 1u);

        m_pfcwd_hw_orch->clearQueueCounterState(queueId);
        EXPECT_EQ(installed.count(queueId), 0u);
    }

    // A queue the watchdog no longer watches must stop reporting a status, while
    // the counters it accumulated stay for show pfcwd stats.
    TEST_F(PfcWdHwCountersTest, ClearQueueCounterState_RemovesStatusKeepsCounters)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);
        setQueueHwCounters(queueId, 1000, 50);

        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, false);

        string status;
        ASSERT_TRUE(m_counters_table->hget(queueIdStr, PFC_WD_QUEUE_STATUS, status));

        m_pfcwd_hw_orch->clearQueueCounterState(queueId);

        EXPECT_FALSE(m_counters_table->hget(queueIdStr, PFC_WD_QUEUE_STATUS, status));

        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.detectCount, 1u);
        EXPECT_EQ(stats.restoreCount, 1u);
    }

    // A queue torn down while stormed is restored by the teardown. Otherwise the
    // storm stays open and the next one is counted as a continuation of it.
    TEST_F(PfcWdHwCountersTest, ClearQueueCounterState_ClosesStormInProgress)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);
        setQueueHwCounters(queueId, 1000, 50);

        m_pfcwd_hw_orch->m_stormedQueues.insert(queueId);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);
        ASSERT_EQ(getQueueStats(queueId).detectCount, 1u);
        ASSERT_EQ(getQueueStats(queueId).restoreCount, 0u);

        m_pfcwd_hw_orch->clearQueueCounterState(queueId);
        EXPECT_EQ(getQueueStats(queueId).restoreCount, 1u);

        // The next storm is its own, not a continuation of the one torn down.
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);
        EXPECT_EQ(getQueueStats(queueId).detectCount, 2u);
    }

    // The storm writes the watchdog's own fields into the queue's COUNTERS_DB
    // hash, so the entry existing does not mean the hardware counters have been
    // published. A storm detected before they are must not be charged the
    // queue's lifetime traffic on the next pass.
    TEST_F(PfcWdHwCountersTest, InitQueueCounters_StormBeforeCountersPublish)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);
        sai_object_id_t pgId = port.m_priority_group_ids[queueIndex];

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);
        m_counters_table->del(queueIdStr);
        m_counters_table->del(sai_serialize_object_id(pgId));

        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);

        // The storm is recorded, but nothing readable became a baseline.
        EXPECT_EQ(getQueueStats(queueId).detectCount, 1u);
        EXPECT_FALSE(getQueueStats(queueId).operational);
        EXPECT_EQ(m_pfcwd_hw_orch->m_queueBaselineStats.count(queueId), 0u);

        // The counters become readable. They seed the baseline instead of
        // being charged to the storm.
        setQueueHwCounters(queueId, 5000, 250);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        auto seeded = getQueueStats(queueId);
        EXPECT_EQ(seeded.txPkt, 0u);
        EXPECT_EQ(seeded.txDropPkt, 0u);
        EXPECT_EQ(seeded.rxPkt, 0u);
        EXPECT_EQ(seeded.rxDropPkt, 0u);

        // From there the storm counts normally.
        setQueueHwCounters(queueId, 5100, 255);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, true);

        auto counted = getQueueStats(queueId);
        EXPECT_EQ(counted.txPkt, 100u);
        EXPECT_EQ(counted.txDropPkt, 5u);
    }

    // The periodic pass has to reach every queue in storm, not only those that
    // already have a baseline: a storm that began before the counters published
    // has none, and would otherwise never get one and close with zero traffic.
    TEST_F(PfcWdHwCountersTest, PeriodicPass_ReachesAStormWithNoBaseline)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);
        sai_object_id_t pgId = port.m_priority_group_ids[queueIndex];

        resetQueueStats(queueId);
        setupQueuePortMapping(queueId, port, queueIndex);
        m_counters_table->del(queueIdStr);
        m_counters_table->del(sai_serialize_object_id(pgId));

        m_pfcwd_hw_orch->m_stormedQueues.insert(queueId);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId);
        ASSERT_EQ(m_pfcwd_hw_orch->m_queueBaselineStats.count(queueId), 0u);

        // The counters become readable. The timer must still visit this queue.
        setQueueHwCounters(queueId, 5000, 250);

        swss::SelectableTimer countersTimer(timespec { .tv_sec = 1, .tv_nsec = 0 });
        m_pfcwd_hw_orch->doTask(countersTimer);

        EXPECT_EQ(m_pfcwd_hw_orch->m_queueBaselineStats.count(queueId), 1u);
        EXPECT_EQ(getQueueStats(queueId).txPkt, 0u);

        // From there the storm accumulates normally.
        setQueueHwCounters(queueId, 5100, 255);
        m_pfcwd_hw_orch->doTask(countersTimer);

        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.txPkt, 100u);
        EXPECT_EQ(stats.txDropPkt, 5u);
    }

    // Optional restoration time

    // Timer the stubbed SAI reports when none was written
    const uint32_t PLATFORM_DLR_MS = 100;

    sai_port_api_t restoration_sai_port_api;
    sai_port_api_t *restoration_old_sai_port_api;
    sai_queue_api_t restoration_sai_queue_api;
    sai_queue_api_t *restoration_old_sai_queue_api;
    sai_switch_api_t restoration_sai_switch_api;
    sai_switch_api_t *restoration_old_sai_switch_api;

    bool dlr_written;
    uint32_t programmed_dlr_ms;

    sai_status_t _restoration_set_port_attribute(sai_object_id_t port_id, const sai_attribute_t *attr)
    {
        if (attr->id == SAI_PORT_ATTR_PFC_TC_DLD_INTERVAL || attr->id == SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL)
        {
            // Teardown clears the intervals with an empty list; only count programming
            if (attr->id == SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL && attr->value.maplist.count > 0)
            {
                dlr_written = true;
                programmed_dlr_ms = attr->value.maplist.list[0].value;
            }
            return SAI_STATUS_SUCCESS;
        }
        return restoration_old_sai_port_api->set_port_attribute(port_id, attr);
    }

    sai_status_t _restoration_get_port_attribute(sai_object_id_t port_id, uint32_t attr_count, sai_attribute_t *attr_list)
    {
        if (attr_count == 1 &&
            (attr_list[0].id == SAI_PORT_ATTR_PFC_TC_DLD_INTERVAL || attr_list[0].id == SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL))
        {
            uint32_t value = attr_list[0].id == SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL ? programmed_dlr_ms : 400;
            for (uint32_t i = 0; i < attr_list[0].value.maplist.count; i++)
            {
                attr_list[0].value.maplist.list[i].value = value;
            }
            return SAI_STATUS_SUCCESS;
        }
        return restoration_old_sai_port_api->get_port_attribute(port_id, attr_count, attr_list);
    }

    sai_status_t _restoration_set_queue_attribute(sai_object_id_t queue_id, const sai_attribute_t *attr)
    {
        if (attr->id == SAI_QUEUE_ATTR_ENABLE_PFC_DLDR)
        {
            return SAI_STATUS_SUCCESS;
        }
        return restoration_old_sai_queue_api->set_queue_attribute(queue_id, attr);
    }

    sai_status_t _restoration_set_switch_attribute(sai_object_id_t switch_id, const sai_attribute_t *attr)
    {
        if (attr->id == SAI_SWITCH_ATTR_PFC_DLR_PACKET_ACTION)
        {
            return SAI_STATUS_SUCCESS;
        }
        return restoration_old_sai_switch_api->set_switch_attribute(switch_id, attr);
    }

    class PfcWdHwRestorationTest : public PfcWdHwCountersTest
    {
    protected:
        shared_ptr<swss::DBConnector> m_state_db;

        void SetUp() override
        {
            PfcWdHwCountersTest::SetUp();

            // PFC watchdog on TC3 and TC4 of Ethernet0
            Port port;
            ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
            ASSERT_TRUE(gPortsOrch->setPortPfcWatchdogStatus(port.m_port_id, (1 << 3) | (1 << 4)));

            restoration_sai_port_api = *sai_port_api;
            restoration_old_sai_port_api = sai_port_api;
            restoration_sai_port_api.set_port_attribute = _restoration_set_port_attribute;
            restoration_sai_port_api.get_port_attribute = _restoration_get_port_attribute;
            sai_port_api = &restoration_sai_port_api;

            restoration_sai_queue_api = *sai_queue_api;
            restoration_old_sai_queue_api = sai_queue_api;
            restoration_sai_queue_api.set_queue_attribute = _restoration_set_queue_attribute;
            sai_queue_api = &restoration_sai_queue_api;

            restoration_sai_switch_api = *sai_switch_api;
            restoration_old_sai_switch_api = sai_switch_api;
            restoration_sai_switch_api.set_switch_attribute = _restoration_set_switch_attribute;
            sai_switch_api = &restoration_sai_switch_api;

            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            dlr_written = false;
            programmed_dlr_ms = PLATFORM_DLR_MS;
        }

        void TearDown() override
        {
            sai_port_api = restoration_old_sai_port_api;
            sai_queue_api = restoration_old_sai_queue_api;
            sai_switch_api = restoration_old_sai_switch_api;
            PfcWdHwCountersTest::TearDown();
        }

        bool hwState(const string &field, string *value)
        {
            swss::Table hwState(m_state_db.get(), STATE_PFC_WD_HW_STATE_TABLE_NAME);
            return hwState.hget("Ethernet0", field, *value);
        }
    };

    TEST_F(PfcWdHwRestorationTest, StartsWithoutRestorationTime)
    {
        auto status = m_pfcwd_hw_orch->createEntry("Ethernet0", {
            { PFC_WD_ACTION, "drop" },
            { PFC_WD_DETECTION_TIME, "400" },
        });

        ASSERT_EQ(status, task_process_status::task_success);
        EXPECT_FALSE(dlr_written);

        string value;
        ASSERT_TRUE(hwState("status", &value));
        EXPECT_EQ(value, "configured");
        ASSERT_TRUE(hwState("hw_restoration_time", &value));
        EXPECT_EQ(value, to_string(PLATFORM_DLR_MS));
        EXPECT_FALSE(hwState("configured_restoration_time", &value));
    }

    TEST_F(PfcWdHwRestorationTest, ProgramsConfiguredRestorationTime)
    {
        auto status = m_pfcwd_hw_orch->createEntry("Ethernet0", {
            { PFC_WD_ACTION, "drop" },
            { PFC_WD_DETECTION_TIME, "400" },
            { PFC_WD_RESTORATION_TIME, "600" },
        });

        ASSERT_EQ(status, task_process_status::task_success);
        EXPECT_TRUE(dlr_written);
        EXPECT_EQ(programmed_dlr_ms, 600u);

        string value;
        ASSERT_TRUE(hwState("configured_restoration_time", &value));
        EXPECT_EQ(value, "600");
        ASSERT_TRUE(hwState("hw_restoration_time", &value));
        EXPECT_EQ(value, "600");
    }

    TEST_F(PfcWdHwRestorationTest, RangeCheckSkipsMissingRestorationTime)
    {
        m_pfcwd_hw_orch->m_timerRangesValid = true;
        m_pfcwd_hw_orch->m_detectionTimeMin = 100;
        m_pfcwd_hw_orch->m_detectionTimeMax = 1500;
        m_pfcwd_hw_orch->m_restorationTimeMin = 100;
        m_pfcwd_hw_orch->m_restorationTimeMax = 1000;

        EXPECT_EQ(m_pfcwd_hw_orch->createEntry("Ethernet0", {
            { PFC_WD_ACTION, "drop" },
            { PFC_WD_DETECTION_TIME, "400" },
            { PFC_WD_RESTORATION_TIME, "1200" },
        }), task_process_status::task_invalid_entry);

        EXPECT_EQ(m_pfcwd_hw_orch->createEntry("Ethernet0", {
            { PFC_WD_ACTION, "drop" },
            { PFC_WD_DETECTION_TIME, "400" },
        }), task_process_status::task_success);
        EXPECT_FALSE(dlr_written);
    }
}
