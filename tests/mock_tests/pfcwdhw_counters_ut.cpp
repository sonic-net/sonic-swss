#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_orch_test.h"
#include "mock_table.h"

#define private public
#include "pfcwdhworch.h"
#undef private

#include <gtest/gtest.h>

namespace pfcwdhworch_test
{
    using namespace std;
    using namespace mock_orch_test;

	    class PfcWdHwCountersTest : public MockOrchTest
	    {
	    protected:
	        shared_ptr<swss::DBConnector> m_counters_db;
	        shared_ptr<swss::Table> m_counters_table;
	        PfcWdHwOrch *m_pfcwd_hw_orch;
	        
	        void SetUp() override
	        {
	            MockOrchTest::SetUp();

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
	        
	        // Helper: Set hardware counter values in COUNTERS_DB for a queue
        void setQueueHwCounters(sai_object_id_t queueId, uint64_t txPkt, uint64_t txDropPkt)
        {
            string queueIdStr = sai_serialize_object_id(queueId);
            vector<FieldValueTuple> fvs = {
                {"SAI_QUEUE_STAT_PACKETS", to_string(txPkt)},
                {"SAI_QUEUE_STAT_DROPPED_PACKETS", to_string(txDropPkt)}
            };
            m_counters_table->set(queueIdStr, fvs);
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
        PfcWdHwOrch::PfcWdQueueStats getQueueStats(sai_object_id_t queueId)
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
                             uint64_t expectedRxPkt,
                             uint64_t expectedRxDropPkt,
                             bool expectedOperational)
        {
            auto stats = getQueueStats(queueId);
            EXPECT_EQ(stats.detectCount, expectedDetect) << "detectCount mismatch";
            EXPECT_EQ(stats.restoreCount, expectedRestore) << "restoreCount mismatch";
            EXPECT_EQ(stats.txPkt, expectedTxPkt) << "txPkt mismatch";
            EXPECT_EQ(stats.txDropPkt, expectedTxDropPkt) << "txDropPkt mismatch";
            EXPECT_EQ(stats.rxPkt, expectedRxPkt) << "rxPkt mismatch";
            EXPECT_EQ(stats.rxDropPkt, expectedRxDropPkt) << "rxDropPkt mismatch";
            EXPECT_EQ(stats.operational, expectedOperational) << "operational status mismatch";
        }
        
        void resetQueueStats(sai_object_id_t queueId)
        {
            string queueIdStr = sai_serialize_object_id(queueId);

            // Delete the stats entry from COUNTERS_DB
            m_counters_table->del(queueIdStr);

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
                           uint64_t expectedTxDropPkt,
                           uint64_t expectedRxPkt,
                           uint64_t expectedRxDropPkt)
        {
            ASSERT_NE(m_pfcwd_hw_orch->m_queueBaselineStats.find(queueId),
                     m_pfcwd_hw_orch->m_queueBaselineStats.end()) 
                << "Baseline not found for queue";
            
            auto& baseline = m_pfcwd_hw_orch->m_queueBaselineStats[queueId];
            EXPECT_EQ(baseline.txPkt, expectedTxPkt) << "Baseline txPkt mismatch";
            EXPECT_EQ(baseline.txDropPkt, expectedTxDropPkt) << "Baseline txDropPkt mismatch";
            EXPECT_EQ(baseline.rxPkt, expectedRxPkt) << "Baseline rxPkt mismatch";
            EXPECT_EQ(baseline.rxDropPkt, expectedRxDropPkt) << "Baseline rxDropPkt mismatch";
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

        // Setup: Populate HW counters in COUNTERS_DB
        setQueueHwCounters(queueId, 1000, 50);
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 2000, 100);

        // Setup: Add queue to port mapping
        setupQueuePortMapping(queueId, port, queueIndex);

        // Test: Call initQueueCounters (simulates storm detection)
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId, queueIndex);

        // Verify: detectCount = 1, restoreCount = 0
        // Verify: operational = false (storm state)
        // Verify: *_LAST counters = 0 (reset on detection)
        // Verify: cumulative counters = 0 (new storm)
        verifyQueueStats(queueId,
                        1,      // detectCount
                        0,      // restoreCount
                        0,      // txPkt
                        0,      // txDropPkt
                        0,      // rxPkt
                        0,      // rxDropPkt
                        false); // operational

        // Verify: Baseline stored with current HW values
        verifyBaseline(queueId, 1000, 50, 2000, 100);
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
        PfcWdHwOrch::PfcWdQueueStats prevStats;
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
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 8000, 300);

        // Test: Call initQueueCounters for second storm
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId, queueIndex);

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
        verifyBaseline(queueId, 5000, 200, 8000, 300);
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
        PfcWdHwOrch::PfcWdQueueStats prevStats;
        memset(&prevStats, 0, sizeof(prevStats));
        prevStats.detectCount = 1;
        prevStats.restoreCount = 0;  // Storm still ongoing
        prevStats.operational = false;
        m_pfcwd_hw_orch->updateQueueStats(queueIdStr, prevStats);

        // Setup: HW counters
        setQueueHwCounters(queueId, 3000, 100);
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 5000, 200);

        // Test: Call initQueueCounters (warm reboot recovery)
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId, queueIndex);

        // Verify: detectCount NOT incremented (still 1)
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.detectCount, 1) << "detectCount should not increment on warm reboot recovery";
        EXPECT_EQ(stats.restoreCount, 0);
        EXPECT_EQ(stats.operational, false);

        // Verify: Baseline re-established for continued monitoring
        verifyBaseline(queueId, 3000, 100, 5000, 200);
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
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 2000, 100);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId, queueIndex);

        // Setup: Update HW counters (traffic during storm)
        setQueueHwCounters(queueId, 1500, 75);
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 2500, 150);

        // Test: Call updateQueueCounters with periodic=true
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, queueIndex, true);

        // Verify: Deltas accumulated
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.txPkt, 500) << "txPkt should be delta: 1500 - 1000";
        EXPECT_EQ(stats.txDropPkt, 25) << "txDropPkt should be delta: 75 - 50";
        EXPECT_EQ(stats.rxPkt, 500) << "rxPkt should be delta: 2500 - 2000";
        EXPECT_EQ(stats.rxDropPkt, 50) << "rxDropPkt should be delta: 150 - 100";

        // Verify: *_LAST counters also updated
        EXPECT_EQ(stats.txPktLast, 500);
        EXPECT_EQ(stats.txDropPktLast, 25);
        EXPECT_EQ(stats.rxPktLast, 500);
        EXPECT_EQ(stats.rxDropPktLast, 50);

        // Verify: Still in storm state
        EXPECT_EQ(stats.operational, false);
        EXPECT_EQ(stats.detectCount, 1);
        EXPECT_EQ(stats.restoreCount, 0) << "restoreCount should not change on periodic update";

        // Verify: Baseline updated to current HW values
        verifyBaseline(queueId, 1500, 75, 2500, 150);
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
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 2000, 100);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId, queueIndex);

        // First periodic update
        setQueueHwCounters(queueId, 1500, 75);
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 2500, 150);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, queueIndex, true);

        // Second periodic update
        setQueueHwCounters(queueId, 2200, 110);
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 3300, 220);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, queueIndex, true);

        // Third periodic update
        setQueueHwCounters(queueId, 3000, 150);
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 4500, 300);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, queueIndex, true);

        // Verify: Cumulative counters = sum of all deltas
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.txPkt, 2000) << "Should be total: (1500-1000) + (2200-1500) + (3000-2200)";
        EXPECT_EQ(stats.txDropPkt, 100) << "Should be total: (75-50) + (110-75) + (150-110)";
        EXPECT_EQ(stats.rxPkt, 2500);
        EXPECT_EQ(stats.rxDropPkt, 200);

        // Verify: *_LAST counters also accumulate
        EXPECT_EQ(stats.txPktLast, 2000);
        EXPECT_EQ(stats.txDropPktLast, 100);

        // Verify: Final baseline is latest HW value
        verifyBaseline(queueId, 3000, 150, 4500, 300);
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
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 2000, 100);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId, queueIndex);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, queueIndex, true);

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
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 2000, 100);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId, queueIndex);

        // Periodic update during storm
        setQueueHwCounters(queueId, 2000, 100);
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 4000, 200);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, queueIndex, true);

        // Test: Restoration (periodic=false)
        setQueueHwCounters(queueId, 3000, 150);
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 6000, 300);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, queueIndex, false);

        // Verify: restoreCount incremented
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.restoreCount, 1);
        EXPECT_EQ(stats.detectCount, 1);

        // Verify: operational = true (restored)
        EXPECT_EQ(stats.operational, true);

        // Verify: Deltas still calculated and accumulated
        EXPECT_EQ(stats.txPkt, 2000) << "Total: (2000-1000) + (3000-2000)";
        EXPECT_EQ(stats.txDropPkt, 100) << "Total: (100-50) + (150-100)";
        EXPECT_EQ(stats.rxPkt, 4000);
        EXPECT_EQ(stats.rxDropPkt, 200);
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
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 2000, 100);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId, queueIndex);

        // Multiple periodic updates
        for (int i = 1; i <= 5; i++)
        {
            setQueueHwCounters(queueId, 1000 + (i * 500), 50 + (i * 25));
            setPgHwCounters(port.m_priority_group_ids[queueIndex], 2000 + (i * 1000), 100 + (i * 50));
            m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, queueIndex, true);
        }

        // Final restoration
        setQueueHwCounters(queueId, 5000, 250);
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 10000, 500);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, queueIndex, false);

        // Verify restoration
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.restoreCount, 1);
        EXPECT_EQ(stats.operational, true);
        EXPECT_EQ(stats.txPkt, 4000) << "Total accumulated: 5000 - 1000";
        EXPECT_EQ(stats.txDropPkt, 200) << "Total accumulated: 250 - 50";
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
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 10000, 500);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId, queueIndex);

        // Test: HW counter reset/wrapped (txPkt < baseline)
        setQueueHwCounters(queueId, 100, 260);
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 10500, 550);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, queueIndex, true);

        // Verify: txPkt NOT updated (underflow protection)
        auto stats = getQueueStats(queueId);
        EXPECT_EQ(stats.txPkt, 0) << "txPkt should not update when HW < baseline";

        // Verify: Other counters that didn't wrap still update
        EXPECT_EQ(stats.txDropPkt, 10) << "txDropPkt should still update: 260 - 250";
        EXPECT_EQ(stats.rxPkt, 500);
        EXPECT_EQ(stats.rxDropPkt, 50);
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
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 10000, 500);
        m_pfcwd_hw_orch->initQueueCounters(queueIdStr, queueId, queueIndex);

        // Test: All counters reset to 0
        setQueueHwCounters(queueId, 0, 0);
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 0, 0);
        m_pfcwd_hw_orch->updateQueueCounters(queueIdStr, queueId, queueIndex, true);

        // Verify: No counters updated (all skipped due to underflow)
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
        // Test successful read of HW counters for both queue and PG
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];

        setupQueuePortMapping(queueId, port, queueIndex);

        // Setup: Populate HW counters in COUNTERS_DB
        setQueueHwCounters(queueId, 1000, 50);
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 2000, 100);

        // Test: Read HW counters
        PfcWdHwOrch::PfcWdHwStats hwStats;
        bool result = m_pfcwd_hw_orch->readHwCounters(queueId, queueIndex, hwStats);

        // Verify: Success
        EXPECT_TRUE(result);
        EXPECT_EQ(hwStats.txPkt, 1000);
        EXPECT_EQ(hwStats.txDropPkt, 50);
        EXPECT_EQ(hwStats.rxPkt, 2000);
        EXPECT_EQ(hwStats.rxDropPkt, 100);
    }

    TEST_F(PfcWdHwCountersTest, ReadHwCounters_MissingQueueCounters)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        string queueIdStr = sai_serialize_object_id(queueId);

        setupQueuePortMapping(queueId, port, queueIndex);

        // Ensure queue entry is deleted from COUNTERS_DB
        m_counters_table->del(queueIdStr);

        // Test: Read HW counters
        PfcWdHwOrch::PfcWdHwStats hwStats;
        bool result = m_pfcwd_hw_orch->readHwCounters(queueId, queueIndex, hwStats);

        // Verify: When queue entry is missing, implementation returns early with ALL counters zero
        EXPECT_TRUE(result);
        EXPECT_EQ(hwStats.txPkt, 0);
        EXPECT_EQ(hwStats.txDropPkt, 0);
        EXPECT_EQ(hwStats.rxPkt, 0);
        EXPECT_EQ(hwStats.rxDropPkt, 0);
    }

    TEST_F(PfcWdHwCountersTest, ReadHwCounters_MissingPgCounters)
    {
        // Test when PG counters are missing from COUNTERS_DB
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];
        sai_object_id_t pgId = port.m_priority_group_ids[queueIndex];
        string pgIdStr = sai_serialize_object_id(pgId);

        setupQueuePortMapping(queueId, port, queueIndex);

        // Ensure PG entry is deleted from COUNTERS_DB (might exist from previous test)
        m_counters_table->del(pgIdStr);

        // Setup: Only populate queue counters (PG counters missing)
        setQueueHwCounters(queueId, 1000, 50);

        // Test: Read HW counters
        PfcWdHwOrch::PfcWdHwStats hwStats;
        bool result = m_pfcwd_hw_orch->readHwCounters(queueId, queueIndex, hwStats);

        // Verify: Success with zero RX counters (implementation returns true when PG entry missing)
        EXPECT_TRUE(result);
        EXPECT_EQ(hwStats.txPkt, 1000);
        EXPECT_EQ(hwStats.txDropPkt, 50);
        EXPECT_EQ(hwStats.rxPkt, 0);
        EXPECT_EQ(hwStats.rxDropPkt, 0);
    }

    TEST_F(PfcWdHwCountersTest, ReadHwCounters_MissingQueuePortMapping)
    {
        // Test when queue is not in m_queueToPortMap
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        uint8_t queueIndex = 3;
        sai_object_id_t queueId = port.m_queue_ids[queueIndex];

        // NOTE: Not calling setupQueuePortMapping - mapping missing

        // Setup: Populate HW counters
        setQueueHwCounters(queueId, 1000, 50);
        setPgHwCounters(port.m_priority_group_ids[queueIndex], 2000, 100);

        // Test: Read HW counters
        PfcWdHwOrch::PfcWdHwStats hwStats;
        bool result = m_pfcwd_hw_orch->readHwCounters(queueId, queueIndex, hwStats);

        // Verify: Failure expected (cannot find port info)
        EXPECT_FALSE(result);
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
        PfcWdHwOrch::PfcWdQueueStats expectedStats;
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

}

