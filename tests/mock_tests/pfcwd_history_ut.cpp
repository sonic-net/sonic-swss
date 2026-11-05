#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_orch_test.h"
#include "mock_table.h"

#define private public
#define protected public
#include "pfcwdorch.h"
#include "pfcwdhworch.h"
#include "pfcwdsworch.h"
#include "warm_restart.h"
#undef protected
#undef private
#include "pfcactionhandler.h"

#include <gtest/gtest.h>
#include <regex>

namespace pfcwd_history_test
{
    using namespace std;
    using namespace mock_orch_test;

    // Outside the vs-SAI allocator range and distinct from the other PFC
    // watchdog counter suites, since the mock table is process-global.
    constexpr sai_object_id_t kPort = 0x1000000000002;
    constexpr sai_object_id_t kQueue = 0xff00000000000002ULL;
    constexpr uint8_t kIdx = 3;
    const string kAlias = "Ethernet0";
    const string kKey = "Ethernet0|3";
    const regex kEpochUs("^[0-9]{16}$");

    // Fixed hardware samples, so a test can see which traffic a restore recorded.
    // Queue (TX) and PG (RX) stats come from SAI, so both gets are hooked.
    PfcWdHwStats _ut_sample = {0, 0, 0, 0};

    sai_queue_api_t ut_sai_queue_api;
    sai_queue_api_t *pold_sai_queue_api;
    sai_buffer_api_t ut_sai_buffer_api;
    sai_buffer_api_t *pold_sai_buffer_api;

    sai_status_t _ut_stub_get_queue_stats(sai_object_id_t, uint32_t number_of_counters,
                                          const sai_stat_id_t *counter_ids, uint64_t *counters)
    {
        for (uint32_t i = 0; i < number_of_counters; i++)
        {
            counters[i] = counter_ids[i] == SAI_QUEUE_STAT_PACKETS ? _ut_sample.txPkt :
                          counter_ids[i] == SAI_QUEUE_STAT_DROPPED_PACKETS ? _ut_sample.txDropPkt : 0;
        }
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_get_pg_stats(sai_object_id_t, uint32_t number_of_counters,
                                       const sai_stat_id_t *counter_ids, uint64_t *counters)
    {
        for (uint32_t i = 0; i < number_of_counters; i++)
        {
            counters[i] = counter_ids[i] == SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS ? _ut_sample.rxPkt :
                          counter_ids[i] == SAI_INGRESS_PRIORITY_GROUP_STAT_DROPPED_PACKETS ? _ut_sample.rxDropPkt : 0;
        }
        return SAI_STATUS_SUCCESS;
    }

    void _hook_sai_stats()
    {
        ut_sai_queue_api = *sai_queue_api;
        pold_sai_queue_api = sai_queue_api;
        ut_sai_queue_api.get_queue_stats = _ut_stub_get_queue_stats;
        sai_queue_api = &ut_sai_queue_api;

        ut_sai_buffer_api = *sai_buffer_api;
        pold_sai_buffer_api = sai_buffer_api;
        ut_sai_buffer_api.get_ingress_priority_group_stats = _ut_stub_get_pg_stats;
        sai_buffer_api = &ut_sai_buffer_api;
    }

    void _unhook_sai_stats()
    {
        sai_queue_api = pold_sai_queue_api;
        sai_buffer_api = pold_sai_buffer_api;
        _ut_sample = {0, 0, 0, 0};
    }

    using SwOrch = PfcWdSwOrch<PfcWdDlrHandler, PfcWdActionHandler>;

    class PfcWdHistoryTest : public MockOrchTest
    {
    protected:
        shared_ptr<swss::DBConnector> m_counters_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::Table> m_history;
        shared_ptr<swss::Table> m_state;
        shared_ptr<swss::Table> m_counters_table;
        PfcWdHwOrch *m_orch = nullptr;

        void SetUp() override
        {
            MockOrchTest::SetUp();
            m_counters_db = make_shared<swss::DBConnector>("COUNTERS_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            m_history = make_shared<swss::Table>(m_state_db.get(), PFC_WD_STORM_HISTORY_TABLE);
            m_state = make_shared<swss::Table>(m_state_db.get(), PFC_WD_QUEUE_STATE_TABLE);
            m_counters_table = make_shared<swss::Table>(m_counters_db.get(), COUNTERS_TABLE);
            clear();

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

            m_orch = newOrch();
        }

        void TearDown() override
        {
            delete m_orch;
            WarmStart::getInstance().m_enabled = false;
            clear();
            MockOrchTest::TearDown();
        }

        PfcWdHwOrch *newOrch()
        {
            vector<string> tables = { CFG_PFC_WD_TABLE_NAME };
            static const vector<sai_port_stat_t> portStatIds = {};
            return new PfcWdHwOrch(m_config_db.get(), tables, portStatIds);
        }

        SwOrch *newSwOrch()
        {
            vector<string> tables = { CFG_PFC_WD_TABLE_NAME };
            static const vector<sai_port_stat_t> portStatIds = {};
            static const vector<sai_queue_stat_t> queueStatIds = {};
            static const vector<sai_queue_attr_t> queueAttrIds = {};
            return new SwOrch(m_config_db.get(), tables, portStatIds, queueStatIds, queueAttrIds, 100);
        }

        void clear()
        {
            m_counters_table->del(queueKey());
            for (auto table : {m_history, m_state})
            {
                vector<string> keys;
                table->getKeys(keys);
                for (const auto& key : keys)
                {
                    table->del(key);
                }
            }
        }

        static string queueKey()
        {
            return sai_serialize_object_id(kQueue);
        }

        static PfcWdQueueStats zeroStats()
        {
            PfcWdQueueStats s;
            memset(&s, 0, sizeof(s));
            s.operational = true;
            return s;
        }

        static bool has(const shared_ptr<swss::Table>& table, const string& field, const string& key = kKey)
        {
            string value;
            return table->hget(key, field, value);
        }

        static string get(const shared_ptr<swss::Table>& table, const string& field, const string& key = kKey)
        {
            string value;
            if (!table->hget(key, field, value))
            {
                return "<absent>";
            }
            return value;
        }

        bool has(const string& field) { return has(m_history, field); }
        string get(const string& field) { return get(m_history, field); }
        string state(const string& field) { return get(m_state, field); }

        void seed(const string& field, const string& value)
        {
            m_history->set(kKey, {{field, value}});
        }

        static string sf(uint64_t n, const char* suffix)
        {
            return "storm_" + to_string(n) + "_" + suffix;
        }

        static string nowPlusUs(int64_t deltaUs)
        {
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            return to_string(static_cast<int64_t>(tv.tv_sec) * 1000000LL + tv.tv_usec + deltaUs);
        }

        void detect() { m_orch->report_pfc_storm(kQueue, kPort, kIdx, kAlias); }
        void restore() { m_orch->report_pfc_restored(kQueue, kPort, kIdx, kAlias); }
    };

    TEST_F(PfcWdHistoryTest, Detect_WritesStateAndFirstStorm)
    {
        detect();

        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_STATUS), PFC_WD_QUEUE_STATE_STATUS_DETECTED);
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_DETECTION_COUNT), "1");
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_RESTORATION_COUNT), "0");
        EXPECT_FALSE(has(m_state, PFC_WD_QUEUE_STATE_LAST_RESTORATION_TIME));

        EXPECT_EQ(get(PFC_WD_STORM_HISTORY_LAST_STORM), "1");
        EXPECT_TRUE(regex_match(get(sf(1, PFC_WD_STORM_HISTORY_DETECTED)), kEpochUs));
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_LAST_DETECTION_TIME), get(sf(1, PFC_WD_STORM_HISTORY_DETECTED)));
        EXPECT_FALSE(has(sf(1, PFC_WD_STORM_HISTORY_RESTORED)));
        EXPECT_FALSE(has(PFC_WD_QUEUE_STATE_STATUS));
    }

    TEST_F(PfcWdHistoryTest, Detect_NumbersFromHistory)
    {
        seed(PFC_WD_STORM_HISTORY_LAST_STORM, "3");
        seed(sf(3, PFC_WD_STORM_HISTORY_DETECTED), "1735689600000000");
        seed(sf(3, PFC_WD_STORM_HISTORY_RESTORED), "1735689601000000");
        detect();

        EXPECT_EQ(get(PFC_WD_STORM_HISTORY_LAST_STORM), "4");
        EXPECT_TRUE(regex_match(get(sf(4, PFC_WD_STORM_HISTORY_DETECTED)), kEpochUs));
        EXPECT_EQ(get(sf(3, PFC_WD_STORM_HISTORY_DETECTED)), "1735689600000000");
    }

    TEST_F(PfcWdHistoryTest, Detect_UnparsableLastStormStartsAtOne)
    {
        seed(PFC_WD_STORM_HISTORY_LAST_STORM, "junk");
        detect();

        EXPECT_EQ(get(PFC_WD_STORM_HISTORY_LAST_STORM), "1");
        EXPECT_TRUE(has(sf(1, PFC_WD_STORM_HISTORY_DETECTED)));
    }

    // A warm restart replays an open storm's detection; it is the same storm.
    TEST_F(PfcWdHistoryTest, Detect_OpenStormIsResumed)
    {
        detect();
        const string detected = get(sf(1, PFC_WD_STORM_HISTORY_DETECTED));
        detect();

        EXPECT_EQ(get(PFC_WD_STORM_HISTORY_LAST_STORM), "1");
        EXPECT_EQ(get(sf(1, PFC_WD_STORM_HISTORY_DETECTED)), detected);
        EXPECT_FALSE(has(sf(2, PFC_WD_STORM_HISTORY_DETECTED)));
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_DETECTION_COUNT), "1");
    }

    TEST_F(PfcWdHistoryTest, Detect_PrunesNumberMinusDepth)
    {
        seed(PFC_WD_STORM_HISTORY_LAST_STORM, to_string(PFC_WD_STORM_HISTORY_DEPTH));
        for (const char* suffix : {PFC_WD_STORM_HISTORY_DETECTED, PFC_WD_STORM_HISTORY_RESTORED,
                                   PFC_WD_STORM_HISTORY_DURATION_MS, PFC_WD_STORM_HISTORY_TX_PACKETS,
                                   PFC_WD_STORM_HISTORY_TX_DROPPED, PFC_WD_STORM_HISTORY_RX_PACKETS,
                                   PFC_WD_STORM_HISTORY_RX_DROPPED, PFC_WD_STORM_HISTORY_INTERRUPTED})
        {
            seed(sf(1, suffix), "1");
            seed(sf(2, suffix), "2");
        }
        seed(sf(PFC_WD_STORM_HISTORY_DEPTH, PFC_WD_STORM_HISTORY_DETECTED), "1735689600000000");
        seed(sf(PFC_WD_STORM_HISTORY_DEPTH, PFC_WD_STORM_HISTORY_RESTORED), "1735689601000000");

        detect();

        for (const char* suffix : {PFC_WD_STORM_HISTORY_DETECTED, PFC_WD_STORM_HISTORY_RESTORED,
                                   PFC_WD_STORM_HISTORY_DURATION_MS, PFC_WD_STORM_HISTORY_TX_PACKETS,
                                   PFC_WD_STORM_HISTORY_TX_DROPPED, PFC_WD_STORM_HISTORY_RX_PACKETS,
                                   PFC_WD_STORM_HISTORY_RX_DROPPED, PFC_WD_STORM_HISTORY_INTERRUPTED})
        {
            EXPECT_FALSE(has(sf(1, suffix))) << suffix;
            EXPECT_EQ(get(sf(2, suffix)), "2") << suffix;
        }
        EXPECT_TRUE(has(sf(PFC_WD_STORM_HISTORY_DEPTH + 1, PFC_WD_STORM_HISTORY_DETECTED)));
    }

    TEST_F(PfcWdHistoryTest, Detect_NoPruneAtDepth)
    {
        seed(PFC_WD_STORM_HISTORY_LAST_STORM, to_string(PFC_WD_STORM_HISTORY_DEPTH - 1));
        seed(sf(1, PFC_WD_STORM_HISTORY_DETECTED), "1735689600000000");
        detect();

        EXPECT_TRUE(has(sf(1, PFC_WD_STORM_HISTORY_DETECTED)));
        EXPECT_TRUE(has(sf(PFC_WD_STORM_HISTORY_DEPTH, PFC_WD_STORM_HISTORY_DETECTED)));
    }

    TEST_F(PfcWdHistoryTest, Restore_FillsStormAndStateFromCounters)
    {
        seed(PFC_WD_STORM_HISTORY_LAST_STORM, "3");
        seed(sf(3, PFC_WD_STORM_HISTORY_DETECTED), nowPlusUs(-500000));
        auto stats = zeroStats();
        stats.txPktLast = 1200;
        stats.txDropPktLast = 1100;
        stats.rxPktLast = 980;
        stats.rxDropPktLast = 970;
        PfcWdBaseOrch::updateQueueStats(m_counters_table, queueKey(), stats);
        restore();

        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_STATUS), PFC_WD_QUEUE_STATE_STATUS_RESTORED);
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_RESTORATION_COUNT), "1");
        EXPECT_TRUE(regex_match(get(sf(3, PFC_WD_STORM_HISTORY_RESTORED)), kEpochUs));
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_LAST_RESTORATION_TIME), get(sf(3, PFC_WD_STORM_HISTORY_RESTORED)));
        auto duration = stoull(get(sf(3, PFC_WD_STORM_HISTORY_DURATION_MS)));
        EXPECT_GE(duration, 500u);
        EXPECT_LE(duration, 800u);
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_STORM_DURATION_MS), get(sf(3, PFC_WD_STORM_HISTORY_DURATION_MS)));
        EXPECT_EQ(get(sf(3, PFC_WD_STORM_HISTORY_TX_PACKETS)), "1200");
        EXPECT_EQ(get(sf(3, PFC_WD_STORM_HISTORY_TX_DROPPED)), "1100");
        EXPECT_EQ(get(sf(3, PFC_WD_STORM_HISTORY_RX_PACKETS)), "980");
        EXPECT_EQ(get(sf(3, PFC_WD_STORM_HISTORY_RX_DROPPED)), "970");
    }

    TEST_F(PfcWdHistoryTest, Restore_NoOpenStormUpdatesStateOnly)
    {
        restore();

        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_STATUS), PFC_WD_QUEUE_STATE_STATUS_RESTORED);
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_RESTORATION_COUNT), "1");
        EXPECT_TRUE(regex_match(state(PFC_WD_QUEUE_STATE_LAST_RESTORATION_TIME), kEpochUs));
        EXPECT_FALSE(has(m_state, PFC_WD_QUEUE_STATE_STORM_DURATION_MS));
        vector<string> keys;
        m_history->getKeys(keys);
        EXPECT_TRUE(keys.empty());
    }

    TEST_F(PfcWdHistoryTest, Restore_AlreadyClosedStormIsKept)
    {
        seed(PFC_WD_STORM_HISTORY_LAST_STORM, "3");
        seed(sf(3, PFC_WD_STORM_HISTORY_DETECTED), "1735689600000000");
        seed(sf(3, PFC_WD_STORM_HISTORY_RESTORED), "1735689601000000");
        seed(sf(3, PFC_WD_STORM_HISTORY_DURATION_MS), "1000");
        restore();

        EXPECT_EQ(get(sf(3, PFC_WD_STORM_HISTORY_RESTORED)), "1735689601000000");
        EXPECT_EQ(get(sf(3, PFC_WD_STORM_HISTORY_DURATION_MS)), "1000");
        EXPECT_FALSE(has(sf(3, PFC_WD_STORM_HISTORY_TX_PACKETS)));
    }

    TEST_F(PfcWdHistoryTest, Restore_InterruptedStormIsNotClosed)
    {
        seed(PFC_WD_STORM_HISTORY_LAST_STORM, "3");
        seed(sf(3, PFC_WD_STORM_HISTORY_DETECTED), "1735689600000000");
        seed(sf(3, PFC_WD_STORM_HISTORY_INTERRUPTED), "1735689900000000");
        restore();

        EXPECT_FALSE(has(sf(3, PFC_WD_STORM_HISTORY_RESTORED)));
        EXPECT_FALSE(has(sf(3, PFC_WD_STORM_HISTORY_DURATION_MS)));
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_STATUS), PFC_WD_QUEUE_STATE_STATUS_RESTORED);
    }

    TEST_F(PfcWdHistoryTest, Restore_ClockStepBackwards)
    {
        seed(PFC_WD_STORM_HISTORY_LAST_STORM, "3");
        seed(sf(3, PFC_WD_STORM_HISTORY_DETECTED), nowPlusUs(60000000));
        restore();

        EXPECT_EQ(get(sf(3, PFC_WD_STORM_HISTORY_DURATION_MS)), "0");
        EXPECT_TRUE(has(sf(3, PFC_WD_STORM_HISTORY_RESTORED)));
    }

    TEST_F(PfcWdHistoryTest, Restore_CorruptDetectTimestamp)
    {
        seed(PFC_WD_STORM_HISTORY_LAST_STORM, "3");
        seed(sf(3, PFC_WD_STORM_HISTORY_DETECTED), "garbage");
        restore();

        EXPECT_EQ(get(sf(3, PFC_WD_STORM_HISTORY_DURATION_MS)), "0");
        EXPECT_TRUE(has(sf(3, PFC_WD_STORM_HISTORY_RESTORED)));
    }

    TEST_F(PfcWdHistoryTest, Cycle_CountsAndPrune)
    {
        for (uint64_t i = 0; i < PFC_WD_STORM_HISTORY_DEPTH + 1; i++)
        {
            detect();
            restore();
        }

        EXPECT_FALSE(has(sf(1, PFC_WD_STORM_HISTORY_DETECTED)));
        for (uint64_t i = 2; i <= PFC_WD_STORM_HISTORY_DEPTH + 1; i++)
        {
            EXPECT_TRUE(has(sf(i, PFC_WD_STORM_HISTORY_DETECTED))) << "storm " << i;
            EXPECT_TRUE(has(sf(i, PFC_WD_STORM_HISTORY_RESTORED))) << "storm " << i;
        }
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_DETECTION_COUNT), to_string(PFC_WD_STORM_HISTORY_DEPTH + 1));
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_RESTORATION_COUNT), to_string(PFC_WD_STORM_HISTORY_DEPTH + 1));

        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(m_history->get(kKey, fvs));
        EXPECT_EQ(fvs.size(), 1u + 7u * PFC_WD_STORM_HISTORY_DEPTH);
    }

    // Counter updates alone write no storm records.
    TEST_F(PfcWdHistoryTest, CounterFunctions_WriteNoRecords)
    {
        PfcWdBaseOrch::updateQueueStats(m_counters_table, queueKey(), zeroStats());
        PfcWdHwStats sample = {100, 10, 200, 20};
        PfcWdHwStats baseline = {0, 0, 0, 0};
        PfcWdBaseOrch::beginStormCounters(m_counters_table, queueKey(), sample, baseline);
        PfcWdBaseOrch::accumulateStormCounters(m_counters_table, queueKey(), sample, baseline, false);

        vector<string> h, s;
        m_history->getKeys(h);
        m_state->getKeys(s);
        EXPECT_TRUE(h.empty());
        EXPECT_TRUE(s.empty());
    }

    TEST_F(PfcWdHistoryTest, Interrupt_ClosesOpenStormOnly)
    {
        detect();
        m_orch->interruptStorm(kAlias, kIdx);
        EXPECT_TRUE(regex_match(get(sf(1, PFC_WD_STORM_HISTORY_INTERRUPTED)), kEpochUs));

        const string interrupted = get(sf(1, PFC_WD_STORM_HISTORY_INTERRUPTED));
        m_orch->interruptStorm(kAlias, kIdx);
        EXPECT_EQ(get(sf(1, PFC_WD_STORM_HISTORY_INTERRUPTED)), interrupted);

        // The next detection opens a new storm.
        detect();
        EXPECT_EQ(get(PFC_WD_STORM_HISTORY_LAST_STORM), "2");
    }

    TEST_F(PfcWdHistoryTest, Interrupt_ClosedStormUntouched)
    {
        detect();
        restore();
        m_orch->interruptStorm(kAlias, kIdx);

        EXPECT_FALSE(has(sf(1, PFC_WD_STORM_HISTORY_INTERRUPTED)));
    }

    TEST_F(PfcWdHistoryTest, Clear_RemovesOnlyThatPort)
    {
        for (const string key : {"Ethernet0|3", "Ethernet0|4", "Ethernet04|3", "Ethernet4|3"})
        {
            m_history->set(key, {{PFC_WD_STORM_HISTORY_LAST_STORM, "1"}});
            m_state->set(key, {{PFC_WD_QUEUE_STATE_STATUS, PFC_WD_QUEUE_STATE_STATUS_RESTORED}});
        }

        m_orch->clearStormLog("Ethernet0");

        for (auto table : {m_history, m_state})
        {
            vector<string> keys;
            table->getKeys(keys);
            sort(keys.begin(), keys.end());
            EXPECT_EQ(keys, (vector<string>{"Ethernet04|3", "Ethernet4|3"}));
        }
    }

    // A cold start re-initialises the ASIC, ending whatever storm handling was in
    // place: an open storm is marked interrupted, and the current state is reset
    // along with the COUNTERS_DB counters swss.sh flushes.
    TEST_F(PfcWdHistoryTest, ColdStart_MarksOpenStormInterrupted)
    {
        seed(PFC_WD_STORM_HISTORY_LAST_STORM, "2");
        seed(sf(1, PFC_WD_STORM_HISTORY_DETECTED), "1735689600000000");
        seed(sf(1, PFC_WD_STORM_HISTORY_RESTORED), "1735689601000000");
        seed(sf(2, PFC_WD_STORM_HISTORY_DETECTED), "1735689660000000");
        m_history->set("Ethernet4|3", {{PFC_WD_STORM_HISTORY_LAST_STORM, "1"},
                                       {sf(1, PFC_WD_STORM_HISTORY_DETECTED), "1735689600000000"},
                                       {sf(1, PFC_WD_STORM_HISTORY_RESTORED), "1735689601000000"}});

        m_orch->closeStormsOnColdStart();

        EXPECT_TRUE(regex_match(get(sf(2, PFC_WD_STORM_HISTORY_INTERRUPTED)), kEpochUs));
        EXPECT_FALSE(has(sf(2, PFC_WD_STORM_HISTORY_RESTORED)));
        EXPECT_FALSE(has(sf(1, PFC_WD_STORM_HISTORY_INTERRUPTED)));
        EXPECT_FALSE(has(m_history, sf(1, PFC_WD_STORM_HISTORY_INTERRUPTED), "Ethernet4|3"));
    }

    TEST_F(PfcWdHistoryTest, ColdStart_KeepsEarlierInterruptTime)
    {
        seed(PFC_WD_STORM_HISTORY_LAST_STORM, "2");
        seed(sf(2, PFC_WD_STORM_HISTORY_DETECTED), "1735689660000000");
        seed(sf(2, PFC_WD_STORM_HISTORY_INTERRUPTED), "1735689720000000");

        m_orch->closeStormsOnColdStart();

        EXPECT_EQ(get(sf(2, PFC_WD_STORM_HISTORY_INTERRUPTED)), "1735689720000000");
    }

    TEST_F(PfcWdHistoryTest, ColdStart_ResetsStateKeepsHistory)
    {
        seed(PFC_WD_STORM_HISTORY_LAST_STORM, "1");
        for (const string key : {"Ethernet0|3", "Ethernet4|3"})
        {
            m_state->set(key, {{PFC_WD_QUEUE_STATE_STATUS, PFC_WD_QUEUE_STATE_STATUS_DETECTED}});
        }

        m_orch->closeStormsOnColdStart();

        vector<string> keys;
        m_state->getKeys(keys);
        EXPECT_TRUE(keys.empty());
        EXPECT_EQ(get(PFC_WD_STORM_HISTORY_LAST_STORM), "1");
    }

    TEST_F(PfcWdHistoryTest, ColdStart_NextStormGetsNextNumber)
    {
        seed(PFC_WD_STORM_HISTORY_LAST_STORM, "2");
        seed(sf(2, PFC_WD_STORM_HISTORY_DETECTED), "1735689660000000");
        m_orch->closeStormsOnColdStart();

        detect();
        restore();

        EXPECT_TRUE(has(sf(2, PFC_WD_STORM_HISTORY_INTERRUPTED)));
        EXPECT_FALSE(has(sf(2, PFC_WD_STORM_HISTORY_RESTORED)));
        EXPECT_TRUE(has(sf(3, PFC_WD_STORM_HISTORY_DETECTED)));
        EXPECT_TRUE(has(sf(3, PFC_WD_STORM_HISTORY_RESTORED)));
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_DETECTION_COUNT), "1");
    }

    TEST_F(PfcWdHistoryTest, ColdStartConstruction_ClosesStorms)
    {
        detect();
        delete m_orch;
        WarmStart::getInstance().m_enabled = false;
        m_orch = newOrch();

        EXPECT_TRUE(has(sf(1, PFC_WD_STORM_HISTORY_INTERRUPTED)));
        EXPECT_FALSE(has(m_state, PFC_WD_QUEUE_STATE_STATUS));
    }

    TEST_F(PfcWdHistoryTest, WarmStartConstruction_KeepsStorms)
    {
        detect();
        delete m_orch;
        WarmStart::getInstance().m_enabled = true;
        m_orch = newOrch();

        EXPECT_FALSE(has(sf(1, PFC_WD_STORM_HISTORY_INTERRUPTED)));
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_STATUS), PFC_WD_QUEUE_STATE_STATUS_DETECTED);
    }

    TEST_F(PfcWdHistoryTest, DeleteEntry_ClearsPortStorms)
    {
        for (const string key : {"Ethernet0|3", "Ethernet4|3"})
        {
            m_history->set(key, {{PFC_WD_STORM_HISTORY_LAST_STORM, "1"}});
            m_state->set(key, {{PFC_WD_QUEUE_STATE_STATUS, PFC_WD_QUEUE_STATE_STATUS_RESTORED}});
        }

        EXPECT_EQ(m_orch->deleteEntry("Ethernet0"), task_process_status::task_success);

        EXPECT_FALSE(has(m_history, PFC_WD_STORM_HISTORY_LAST_STORM, "Ethernet0|3"));
        EXPECT_FALSE(has(m_state, PFC_WD_QUEUE_STATE_STATUS, "Ethernet0|3"));
        EXPECT_TRUE(has(m_history, PFC_WD_STORM_HISTORY_LAST_STORM, "Ethernet4|3"));
        EXPECT_TRUE(has(m_state, PFC_WD_QUEUE_STATE_STATUS, "Ethernet4|3"));
    }

    // The software watchdog, driven through its storm/restore handler.
    class PfcWdSwHistoryTest : public PfcWdHistoryTest
    {
    protected:
        SwOrch *m_sw = nullptr;

        void SetUp() override
        {
            PfcWdHistoryTest::SetUp();
            m_sw = newSwOrch();
            m_sw->m_entryMap.emplace(kQueue, SwOrch::PfcWdQueueEntry(
                PfcWdAction::PFC_WD_ACTION_ALERT, kPort, kIdx, kAlias));
        }

        void TearDown() override
        {
            delete m_sw;
            PfcWdHistoryTest::TearDown();
        }

        SwOrch::PfcWdQueueEntry& entry() { return m_sw->m_entryMap.at(kQueue); }
    };

    TEST_F(PfcWdSwHistoryTest, StormAndRestore)
    {
        ASSERT_TRUE(m_sw->startWdActionOnQueue("storm", kQueue));
        EXPECT_EQ(get(PFC_WD_STORM_HISTORY_LAST_STORM), "1");
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_STATUS), PFC_WD_QUEUE_STATE_STATUS_DETECTED);

        ASSERT_TRUE(m_sw->startWdActionOnQueue("restore", kQueue));
        EXPECT_TRUE(has(sf(1, PFC_WD_STORM_HISTORY_RESTORED)));
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_STATUS), PFC_WD_QUEUE_STATE_STATUS_RESTORED);
    }

    // The restore record carries the storm's committed traffic.
    TEST_F(PfcWdSwHistoryTest, RestoreRecordsCommittedTraffic)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort(kAlias, port));
        const sai_object_id_t queue = port.m_queue_ids[kIdx];
        const string queueStr = sai_serialize_object_id(queue);
        m_sw->m_entryMap.erase(kQueue);
        m_sw->m_entryMap.emplace(queue, SwOrch::PfcWdQueueEntry(
            PfcWdAction::PFC_WD_ACTION_ALERT, port.m_port_id, kIdx, kAlias));

        _hook_sai_stats();
        PfcWdBaseOrch::updateQueueStats(m_counters_table, queueStr, zeroStats());
        m_sw->report_pfc_storm(queue, port.m_port_id, kIdx, kAlias);
        _ut_sample = {100, 10, 200, 20};
        auto handler = make_shared<PfcWdActionHandler>(port.m_port_id, queue, kIdx, m_counters_table);
        handler->initCounters();
        _ut_sample = {1300, 1210, 1180, 1000};
        m_sw->m_entryMap.at(queue).handler = handler;

        ASSERT_TRUE(m_sw->startWdActionOnQueue("restore", queue));
        _unhook_sai_stats();

        EXPECT_EQ(get(sf(1, PFC_WD_STORM_HISTORY_TX_PACKETS)), "1200");
        EXPECT_EQ(get(sf(1, PFC_WD_STORM_HISTORY_TX_DROPPED)), "1200");
        EXPECT_EQ(get(sf(1, PFC_WD_STORM_HISTORY_RX_PACKETS)), "980");
        EXPECT_EQ(get(sf(1, PFC_WD_STORM_HISTORY_RX_DROPPED)), "980");
        m_counters_table->del(queueStr);
    }

    // After a warm restart the open storm is replayed with no handler in place.
    TEST_F(PfcWdSwHistoryTest, WarmReplayResumesStorm)
    {
        ASSERT_TRUE(m_sw->startWdActionOnQueue("storm", kQueue));
        entry().handler = nullptr;
        ASSERT_TRUE(m_sw->startWdActionOnQueue("storm", kQueue));

        EXPECT_EQ(get(PFC_WD_STORM_HISTORY_LAST_STORM), "1");
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_DETECTION_COUNT), "1");
    }

    TEST_F(PfcWdSwHistoryTest, BigRedSwitchInterruptsAndRecordsNoStorm)
    {
        ASSERT_TRUE(m_sw->startWdActionOnQueue("storm", kQueue));

        m_sw->enableBigRedSwitchMode();
        EXPECT_TRUE(has(sf(1, PFC_WD_STORM_HISTORY_INTERRUPTED)));
        EXPECT_FALSE(has(sf(1, PFC_WD_STORM_HISTORY_RESTORED)));

        m_sw->disableBigRedSwitchMode();
        EXPECT_EQ(get(PFC_WD_STORM_HISTORY_LAST_STORM), "1");
        EXPECT_FALSE(has(sf(1, PFC_WD_STORM_HISTORY_RESTORED)));
        EXPECT_EQ(state(PFC_WD_QUEUE_STATE_STATUS), PFC_WD_QUEUE_STATE_STATUS_DETECTED);
        vector<string> keys;
        m_history->getKeys(keys);
        EXPECT_EQ(keys, vector<string>{kKey});
    }

    TEST_F(PfcWdSwHistoryTest, UnregisterInterruptsOpenStorm)
    {
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort(kAlias, port));
        const sai_object_id_t queue = port.m_queue_ids[kIdx];
        m_sw->m_entryMap.erase(kQueue);
        m_sw->m_entryMap.emplace(queue, SwOrch::PfcWdQueueEntry(
            PfcWdAction::PFC_WD_ACTION_ALERT, port.m_port_id, kIdx, kAlias));
        ASSERT_TRUE(m_sw->startWdActionOnQueue("storm", queue));

        m_sw->unregisterFromWdDb(port);

        EXPECT_TRUE(has(sf(1, PFC_WD_STORM_HISTORY_INTERRUPTED)));
        EXPECT_FALSE(has(sf(1, PFC_WD_STORM_HISTORY_RESTORED)));
        m_counters_table->del(sai_serialize_object_id(queue));
    }
}
