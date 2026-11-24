#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_orch_test.h"
#include "mock_table.h"

#define private public
#include "pfcwdhworch.h"
#undef private

#include <gtest/gtest.h>

namespace pfcwdhw_stat_history_test
{
    using namespace std;
    using namespace mock_orch_test;

    // Reaches the protected members the tests inspect
    struct BaseAccess : PfcWdBaseOrch
    {
        using PfcWdBaseOrch::filterPfcCounters;

        static FlexCounterTaggedCachedManager<sai_object_type_t> &manager(PfcWdBaseOrch &orch)
        {
            return *(orch.*(&BaseAccess::m_pfcwdFlexCounterManager));
        }
    };

    struct ManagerAccess : FlexCounterManager
    {
        static bool installed(FlexCounterManager &mgr, sai_object_id_t oid)
        {
            return (mgr.*(&ManagerAccess::installed_counters)).count(oid) != 0;
        }
    };

    // The PFC_WD counter fields syncd has been given, by object
    map<string, set<string>> _ut_pfcwd_counter_fields;

    sai_switch_api_t ut_sai_switch_api;
    sai_switch_api_t *pold_sai_switch_api;

    sai_status_t _ut_stub_sai_set_switch_attribute(sai_object_id_t switch_id, const sai_attribute_t *attr)
    {
        if (attr[0].id != SAI_REDIS_SWITCH_ATTR_FLEX_COUNTER)
        {
            return pold_sai_switch_api->set_switch_attribute(switch_id, attr);
        }

        auto *param = reinterpret_cast<sai_redis_flex_counter_parameter_t*>(attr[0].value.ptr);
        string keys((const char*)param->counter_key.list, param->counter_key.count);
        if (keys.rfind(PFC_WD_FLEX_COUNTER_GROUP ":", 0) != 0)
        {
            return SAI_STATUS_SUCCESS;
        }

        // Only the first key carries the group name
        for (const auto &key : tokenize(keys, ','))
        {
            string oid = key.substr(key.find("oid:"));
            if (param->counter_ids.list == nullptr)
            {
                _ut_pfcwd_counter_fields.erase(oid);
            }
            else
            {
                _ut_pfcwd_counter_fields[oid].insert(
                    string((const char*)param->counter_field_name.list, param->counter_field_name.count));
            }
        }
        return SAI_STATUS_SUCCESS;
    }

    void _hook_sai_switch_api()
    {
        ut_sai_switch_api = *sai_switch_api;
        pold_sai_switch_api = sai_switch_api;
        ut_sai_switch_api.set_switch_attribute = _ut_stub_sai_set_switch_attribute;
        sai_switch_api = &ut_sai_switch_api;
    }

    void _unhook_sai_switch_api()
    {
        sai_switch_api = pold_sai_switch_api;
        _ut_pfcwd_counter_fields.clear();
    }

    class PfcWdHwStatHistoryTest : public MockOrchTest
    {
    protected:
        shared_ptr<swss::DBConnector> m_counters_db;
        shared_ptr<swss::Table> m_counters_table;
        PfcWdHwOrch *m_pfcwd_hw_orch;
        Port m_port;

        void SetUp() override
        {
            MockOrchTest::SetUp();
            _hook_sai_switch_api();
            bringUpPorts();

            m_counters_db = make_shared<swss::DBConnector>("COUNTERS_DB", 0);
            m_counters_table = make_shared<swss::Table>(m_counters_db.get(), COUNTERS_TABLE);
            // COUNTERS_DB is shared by every test in the binary
            swss::Table(m_counters_db.get(), PFC_WD_STAT_HISTORY_TABLE).del(PFC_WD_STAT_HISTORY_PORTS);

            m_pfcwd_hw_orch = newOrch();

            ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", m_port));
            // Only a queue with PFC enabled is watched, so set both masks
            ASSERT_TRUE(gPortsOrch->setPortPfc(m_port.m_port_id, (1 << 3) | (1 << 4)));
            ASSERT_TRUE(gPortsOrch->setPortPfcWatchdogStatus(m_port.m_port_id, (1 << 3) | (1 << 4)));
            ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", m_port));

            // COUNTERS_DB is shared by every test in the binary
            for (auto queueId : m_port.m_queue_ids)
            {
                string key = sai_serialize_object_id(queueId);
                m_counters_table->hdel(key, "PFC_STAT_HISTORY");
                m_counters_table->hdel(key, "SAI_QUEUE_ATTR_PAUSE_STATUS_last");
            }
        }

        void TearDown() override
        {
            delete m_pfcwd_hw_orch;
            _unhook_sai_switch_api();
            MockOrchTest::TearDown();
        }

        PfcWdHwOrch *newOrch()
        {
            static vector<string> pfc_wd_tables = { CFG_PFC_WD_TABLE_NAME };
            static const vector<sai_port_stat_t> portStatIds = {
                SAI_PORT_STAT_PFC_3_RX_PKTS,
                SAI_PORT_STAT_PFC_4_RX_PKTS,
                SAI_PORT_STAT_PFC_5_RX_PKTS,
                SAI_PORT_STAT_PFC_3_ON2OFF_RX_PKTS,
                SAI_PORT_STAT_PFC_4_ON2OFF_RX_PKTS,
                SAI_PORT_STAT_PFC_5_ON2OFF_RX_PKTS,
            };
            static const vector<sai_queue_stat_t> queueStatIds = { SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES };
            static const vector<sai_queue_attr_t> queueAttrIds = { SAI_QUEUE_ATTR_PAUSE_STATUS };

            auto *orch = new PfcWdHwOrch(m_config_db.get(), pfc_wd_tables,
                                         portStatIds, queueStatIds, queueAttrIds);
            // The mock build has no plugin file to load; the plugin is tested on its own
            orch->m_statHistoryPluginLoaded = true;
            return orch;
        }

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

        string queueKey(uint8_t tc)
        {
            return sai_serialize_object_id(m_port.m_queue_ids[tc]);
        }

        string portKey()
        {
            return sai_serialize_object_id(m_port.m_port_id);
        }

        bool hasField(const string &key, const string &field, string *value = nullptr)
        {
            string v;
            bool found = m_counters_table->hget(key, field, v);
            if (found && value)
            {
                *value = v;
            }
            return found;
        }

        bool portListed()
        {
            swss::Table ports(m_counters_db.get(), PFC_WD_STAT_HISTORY_TABLE);
            string v;
            return ports.hget(PFC_WD_STAT_HISTORY_PORTS, portKey(), v);
        }

        bool portCountersPolled()
        {
            return ManagerAccess::installed(BaseAccess::manager(*m_pfcwd_hw_orch), m_port.m_port_id);
        }

        bool queuePolled(uint8_t tc)
        {
            return ManagerAccess::installed(BaseAccess::manager(*m_pfcwd_hw_orch), m_port.m_queue_ids[tc]);
        }

        // The counter fields syncd holds for the queue once pending lists are flushed
        set<string> queueCounterFields(uint8_t tc)
        {
            BaseAccess::manager(*m_pfcwd_hw_orch).flush();
            auto it = _ut_pfcwd_counter_fields.find(queueKey(tc));
            return it == _ut_pfcwd_counter_fields.end() ? set<string>() : it->second;
        }
    };

    TEST_F(PfcWdHwStatHistoryTest, FilterPfcCountersKeepsLosslessPriorities)
    {
        unordered_set<string> counters = {
            "SAI_PORT_STAT_PFC_3_RX_PKTS",
            "SAI_PORT_STAT_PFC_4_ON2OFF_RX_PKTS",
            "SAI_PORT_STAT_PFC_5_RX_PKTS",
            "SAI_PORT_STAT_IF_IN_OCTETS",
        };

        auto filtered = BaseAccess::filterPfcCounters(counters, { 3, 4 });

        EXPECT_EQ(filtered, (unordered_set<string>{
            "SAI_PORT_STAT_PFC_3_RX_PKTS",
            "SAI_PORT_STAT_PFC_4_ON2OFF_RX_PKTS",
            "SAI_PORT_STAT_IF_IN_OCTETS",
        }));
    }

    TEST_F(PfcWdHwStatHistoryTest, EnableFlagsLosslessQueuesAndPollsPortPfcCounters)
    {
        ASSERT_FALSE(portCountersPolled());
        ASSERT_FALSE(portListed());

        m_pfcwd_hw_orch->enableStatHistory(m_port);

        string value;
        ASSERT_TRUE(hasField(queueKey(3), "PFC_STAT_HISTORY", &value));
        EXPECT_EQ(value, "enable");
        ASSERT_TRUE(hasField(queueKey(4), "PFC_STAT_HISTORY", &value));
        EXPECT_EQ(value, "enable");
        EXPECT_FALSE(hasField(queueKey(0), "PFC_STAT_HISTORY"));
        EXPECT_FALSE(hasField(queueKey(5), "PFC_STAT_HISTORY"));

        EXPECT_TRUE(portCountersPolled());
        EXPECT_TRUE(portListed());

        // Pause status is polled only for the lossless queues
        EXPECT_TRUE(queuePolled(3));
        EXPECT_TRUE(queuePolled(4));
        EXPECT_FALSE(queuePolled(0));
        EXPECT_FALSE(queuePolled(5));
    }

    // A warm restart starts the watchdog without a stop, so samples from the
    // previous run must not become the first poll's baseline.
    TEST_F(PfcWdHwStatHistoryTest, EnableDropsSamplesOfAPreviousRun)
    {
        m_counters_table->hset(queueKey(4), "SAI_QUEUE_ATTR_PAUSE_STATUS_last", "true");
        m_counters_table->hset(portKey(), "SAI_PORT_STAT_PFC_4_RX_PKTS_last", "90");
        m_counters_table->hset(portKey(), "EST_PORT_STAT_PFC_4_RECENT_PAUSE_TIME_US", "3000");

        m_pfcwd_hw_orch->enableStatHistory(m_port);

        EXPECT_FALSE(hasField(queueKey(4), "SAI_QUEUE_ATTR_PAUSE_STATUS_last"));
        EXPECT_FALSE(hasField(portKey(), "SAI_PORT_STAT_PFC_4_RX_PKTS_last"));
        EXPECT_TRUE(hasField(portKey(), "EST_PORT_STAT_PFC_4_RECENT_PAUSE_TIME_US"));
    }

    TEST_F(PfcWdHwStatHistoryTest, HistoryQueuesArePolledForStatsSoThePluginRuns)
    {
        m_pfcwd_hw_orch->enableStatHistory(m_port);

        // syncd hands a queue plugin only the queues polled for stats
        const set<string> expected = { "QUEUE_COUNTER_ID_LIST", "QUEUE_ATTR_ID_LIST" };
        EXPECT_EQ(queueCounterFields(3), expected);
        EXPECT_EQ(queueCounterFields(4), expected);
        EXPECT_TRUE(queueCounterFields(0).empty());
    }

    TEST_F(PfcWdHwStatHistoryTest, WatchdogWithoutHistoryDoesNotPollQueues)
    {
        // Storm counters are read from SAI, so the watchdog alone polls nothing
        m_pfcwd_hw_orch->initializeQueueStats(m_port, { 3, 4 });

        EXPECT_FALSE(queuePolled(3));
        EXPECT_FALSE(queuePolled(4));
        EXPECT_TRUE(queueCounterFields(3).empty());
        EXPECT_TRUE(queueCounterFields(4).empty());
        EXPECT_FALSE(portCountersPolled());
    }

    TEST_F(PfcWdHwStatHistoryTest, ClearStateKeepsRecordedHistory)
    {
        m_counters_table->set(queueKey(4), {
            { "PFC_STAT_HISTORY", "enable" },
            { "SAI_QUEUE_ATTR_PAUSE_STATUS", "true" },
            { "SAI_QUEUE_ATTR_PAUSE_STATUS_last", "true" },
        });
        m_counters_table->set(portKey(), {
            { "SAI_PORT_STAT_PFC_4_RX_PKTS", "100" },
            { "SAI_PORT_STAT_PFC_4_RX_PKTS_last", "90" },
            { "SAI_PORT_STAT_PFC_3_RX_PKTS_last", "7" },
            { "EST_PORT_STAT_PFC_4_RX_PAUSE_DURATION_US", "5000" },
            { "EST_PORT_STAT_PFC_4_RECENT_PAUSE_TIME_US", "3000" },
            { "EST_PORT_STAT_PFC_4_RECENT_PAUSE_TIMESTAMP", "1791967131719412" },
        });

        m_pfcwd_hw_orch->clearStatHistoryState(m_port.m_port_id, m_port.m_queue_ids[4], 4);

        EXPECT_FALSE(hasField(queueKey(4), "PFC_STAT_HISTORY"));
        EXPECT_FALSE(hasField(queueKey(4), "SAI_QUEUE_ATTR_PAUSE_STATUS_last"));
        EXPECT_TRUE(hasField(queueKey(4), "SAI_QUEUE_ATTR_PAUSE_STATUS"));

        EXPECT_FALSE(hasField(portKey(), "SAI_PORT_STAT_PFC_4_RX_PKTS_last"));
        EXPECT_TRUE(hasField(portKey(), "SAI_PORT_STAT_PFC_3_RX_PKTS_last"));
        EXPECT_TRUE(hasField(portKey(), "SAI_PORT_STAT_PFC_4_RX_PKTS"));
        EXPECT_TRUE(hasField(portKey(), "EST_PORT_STAT_PFC_4_RX_PAUSE_DURATION_US"));
        EXPECT_TRUE(hasField(portKey(), "EST_PORT_STAT_PFC_4_RECENT_PAUSE_TIME_US"));
        EXPECT_TRUE(hasField(portKey(), "EST_PORT_STAT_PFC_4_RECENT_PAUSE_TIMESTAMP"));
    }

    TEST_F(PfcWdHwStatHistoryTest, DisableStopsHistory)
    {
        m_pfcwd_hw_orch->enableStatHistory(m_port);
        m_counters_table->hset(queueKey(4), "SAI_QUEUE_ATTR_PAUSE_STATUS_last", "true");
        m_counters_table->hset(portKey(), "SAI_PORT_STAT_PFC_4_RX_PKTS_last", "90");
        m_counters_table->hset(portKey(), "EST_PORT_STAT_PFC_4_RECENT_PAUSE_TIME_US", "3000");
        ASSERT_TRUE(portCountersPolled());
        ASSERT_TRUE(portListed());

        m_pfcwd_hw_orch->disableHwWatchdog(m_port);

        EXPECT_FALSE(portCountersPolled());
        EXPECT_FALSE(portListed());
        EXPECT_FALSE(queuePolled(3));
        EXPECT_FALSE(queuePolled(4));
        EXPECT_TRUE(queueCounterFields(3).empty());
        EXPECT_TRUE(queueCounterFields(4).empty());
        EXPECT_FALSE(hasField(queueKey(3), "PFC_STAT_HISTORY"));
        EXPECT_FALSE(hasField(queueKey(4), "PFC_STAT_HISTORY"));
        EXPECT_FALSE(hasField(queueKey(4), "SAI_QUEUE_ATTR_PAUSE_STATUS_last"));
        EXPECT_FALSE(hasField(portKey(), "SAI_PORT_STAT_PFC_4_RX_PKTS_last"));
        EXPECT_TRUE(hasField(portKey(), "EST_PORT_STAT_PFC_4_RECENT_PAUSE_TIME_US"));
    }

    TEST_F(PfcWdHwStatHistoryTest, ConstructionDropsStaleListing)
    {
        // Left by a previous orchagent across a warm restart, for a port whose
        // watchdog config is gone
        swss::Table ports(m_counters_db.get(), PFC_WD_STAT_HISTORY_TABLE);
        ports.hset(PFC_WD_STAT_HISTORY_PORTS, portKey(), "Ethernet0");
        ASSERT_TRUE(portListed());

        delete m_pfcwd_hw_orch;
        m_pfcwd_hw_orch = newOrch();

        EXPECT_FALSE(portListed());
    }

    // createEntry() with the SAI calls the hardware watchdog makes stubbed out
    sai_port_api_t config_sai_port_api;
    sai_port_api_t *config_old_sai_port_api;
    sai_queue_api_t config_sai_queue_api;
    sai_queue_api_t *config_old_sai_queue_api;
    sai_switch_api_t config_sai_switch_api;
    sai_switch_api_t *config_old_sai_switch_api;

    sai_status_t _config_set_port_attribute(sai_object_id_t port_id, const sai_attribute_t *attr)
    {
        if (attr->id == SAI_PORT_ATTR_PFC_TC_DLD_INTERVAL || attr->id == SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL)
        {
            return SAI_STATUS_SUCCESS;
        }
        return config_old_sai_port_api->set_port_attribute(port_id, attr);
    }

    sai_status_t _config_get_port_attribute(sai_object_id_t port_id, uint32_t attr_count, sai_attribute_t *attr_list)
    {
        if (attr_count == 1 &&
            (attr_list[0].id == SAI_PORT_ATTR_PFC_TC_DLD_INTERVAL || attr_list[0].id == SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL))
        {
            for (uint32_t i = 0; i < attr_list[0].value.maplist.count; i++)
            {
                attr_list[0].value.maplist.list[i].value = 400;
            }
            return SAI_STATUS_SUCCESS;
        }
        return config_old_sai_port_api->get_port_attribute(port_id, attr_count, attr_list);
    }

    bool config_dldr_disable_fails = false;

    sai_status_t _config_set_queue_attribute(sai_object_id_t queue_id, const sai_attribute_t *attr)
    {
        if (attr->id == SAI_QUEUE_ATTR_ENABLE_PFC_DLDR)
        {
            if (config_dldr_disable_fails && !attr->value.booldata)
            {
                return SAI_STATUS_FAILURE;
            }
            return SAI_STATUS_SUCCESS;
        }
        return config_old_sai_queue_api->set_queue_attribute(queue_id, attr);
    }

    sai_status_t _config_set_switch_attribute(sai_object_id_t switch_id, const sai_attribute_t *attr)
    {
        if (attr->id == SAI_SWITCH_ATTR_PFC_DLR_PACKET_ACTION)
        {
            return SAI_STATUS_SUCCESS;
        }
        return config_old_sai_switch_api->set_switch_attribute(switch_id, attr);
    }

    class PfcWdHwStatHistoryConfigTest : public PfcWdHwStatHistoryTest
    {
    protected:
        void SetUp() override
        {
            PfcWdHwStatHistoryTest::SetUp();

            config_sai_port_api = *sai_port_api;
            config_old_sai_port_api = sai_port_api;
            config_sai_port_api.set_port_attribute = _config_set_port_attribute;
            config_sai_port_api.get_port_attribute = _config_get_port_attribute;
            sai_port_api = &config_sai_port_api;

            config_sai_queue_api = *sai_queue_api;
            config_old_sai_queue_api = sai_queue_api;
            config_sai_queue_api.set_queue_attribute = _config_set_queue_attribute;
            sai_queue_api = &config_sai_queue_api;

            config_sai_switch_api = *sai_switch_api;
            config_old_sai_switch_api = sai_switch_api;
            config_sai_switch_api.set_switch_attribute = _config_set_switch_attribute;
            sai_switch_api = &config_sai_switch_api;
        }

        void TearDown() override
        {
            config_dldr_disable_fails = false;
            sai_port_api = config_old_sai_port_api;
            sai_queue_api = config_old_sai_queue_api;
            sai_switch_api = config_old_sai_switch_api;
            PfcWdHwStatHistoryTest::TearDown();
        }

        task_process_status configure(const string &history)
        {
            return m_pfcwd_hw_orch->createEntry("Ethernet0", {
                { PFC_WD_ACTION, "drop" },
                { PFC_WD_DETECTION_TIME, "400" },
                { PFC_WD_RESTORATION_TIME, "400" },
                { PFC_STAT_HISTORY, history },
            });
        }
    };

    // The configured setting reaches the plugin: before, the hardware watchdog
    // accepted pfc_stat_history and only logged that it was ignored.
    TEST_F(PfcWdHwStatHistoryConfigTest, ConfiguredHistoryIsRecorded)
    {
        ASSERT_EQ(configure("enable"), task_process_status::task_success);

        string value;
        ASSERT_TRUE(hasField(queueKey(4), "PFC_STAT_HISTORY", &value));
        EXPECT_EQ(value, "enable");
        EXPECT_TRUE(portListed());
        EXPECT_TRUE(portCountersPolled());
        EXPECT_TRUE(queuePolled(4));
    }

    TEST_F(PfcWdHwStatHistoryConfigTest, HistoryOffRegistersNothing)
    {
        ASSERT_EQ(configure("disable"), task_process_status::task_success);

        EXPECT_FALSE(hasField(queueKey(4), "PFC_STAT_HISTORY"));
        EXPECT_FALSE(portListed());
        EXPECT_FALSE(portCountersPolled());
        EXPECT_FALSE(queuePolled(3));
        EXPECT_FALSE(queuePolled(4));
    }

    // Reconfiguration stops the watchdog first, so turning history off ends it
    TEST_F(PfcWdHwStatHistoryConfigTest, ReconfiguringWithoutHistoryEndsIt)
    {
        ASSERT_EQ(configure("enable"), task_process_status::task_success);
        ASSERT_TRUE(portListed());

        ASSERT_EQ(configure("disable"), task_process_status::task_success);

        EXPECT_FALSE(hasField(queueKey(4), "PFC_STAT_HISTORY"));
        EXPECT_FALSE(portListed());
        EXPECT_FALSE(portCountersPolled());
        EXPECT_FALSE(queuePolled(4));
    }

    // A queue dropped from the lossless set is stopped by the second pass of the
    // teardown, and its history ends there too
    TEST_F(PfcWdHwStatHistoryConfigTest, StopEndsHistoryOnAQueueDroppedFromTheLosslessSet)
    {
        ASSERT_EQ(configure("enable"), task_process_status::task_success);
        ASSERT_TRUE(hasField(queueKey(3), "PFC_STAT_HISTORY"));

        ASSERT_TRUE(gPortsOrch->setPortPfcWatchdogStatus(m_port.m_port_id, 1 << 4));
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", m_port));
        ASSERT_TRUE(m_pfcwd_hw_orch->disableHwWatchdog(m_port));

        EXPECT_FALSE(hasField(queueKey(3), "PFC_STAT_HISTORY"));
        EXPECT_FALSE(queuePolled(3));
        EXPECT_FALSE(hasField(queueKey(4), "PFC_STAT_HISTORY"));
        EXPECT_FALSE(portListed());
    }

    // History ends with the stop even when a queue's DLDR stays on and the
    // watchdog keeps the port for a retry
    TEST_F(PfcWdHwStatHistoryConfigTest, StopEndsHistoryWhenDldrDisableFails)
    {
        ASSERT_EQ(configure("enable"), task_process_status::task_success);

        config_dldr_disable_fails = true;
        for (bool bestEffort : { false, true })
        {
            // A queue still armed is reported as a failed teardown either way
            EXPECT_FALSE(m_pfcwd_hw_orch->disableHwWatchdog(m_port, bestEffort));

            EXPECT_FALSE(hasField(queueKey(3), "PFC_STAT_HISTORY"));
            EXPECT_FALSE(hasField(queueKey(4), "PFC_STAT_HISTORY"));
            EXPECT_FALSE(queuePolled(3));
            EXPECT_FALSE(queuePolled(4));
            EXPECT_FALSE(portListed());
            EXPECT_FALSE(portCountersPolled());
        }
    }

    TEST_F(PfcWdHwStatHistoryTest, NoPluginNoHistory)
    {
        m_pfcwd_hw_orch->m_statHistoryPluginLoaded = false;
        m_pfcwd_hw_orch->enableStatHistory(m_port);

        EXPECT_FALSE(hasField(queueKey(4), "PFC_STAT_HISTORY"));
        EXPECT_FALSE(portListed());
        EXPECT_FALSE(portCountersPolled());
        EXPECT_FALSE(queuePolled(4));
    }
}
