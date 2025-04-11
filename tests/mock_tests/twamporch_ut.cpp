#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "notifier.h"

extern string gMySwitchType;

extern sai_object_id_t      gSwitchId;

extern redisReply *mockReply;


namespace twamporch_test
{
    using namespace std;

    int create_twamp_session_count;
    int set_twamp_session_count;
    int remove_twamp_session_count;

    sai_twamp_api_t ut_sai_twamp_api;
    sai_twamp_api_t *pold_sai_twamp_api;
    sai_switch_api_t ut_sai_switch_api;
    sai_switch_api_t *pold_sai_switch_api;

    sai_create_twamp_session_fn              old_create_twamp_session;
    sai_remove_twamp_session_fn              old_remove_twamp_session;
    sai_set_twamp_session_attribute_fn       old_set_twamp_session_attribute;

    sai_status_t _ut_stub_sai_create_twamp_session(
        _Out_ sai_object_id_t *twamp_session_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        *twamp_session_id = (sai_object_id_t)(0x1);
        create_twamp_session_count++;
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_remove_twamp_session(
        _In_ sai_object_id_t twamp_session_id)
    {
        remove_twamp_session_count++;
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_set_twamp_session_attribute(
            _In_ sai_object_id_t twamp_session_id,
            _In_ const sai_attribute_t *attr)
    {
        set_twamp_session_count++;
        if (attr->id == SAI_TWAMP_SESSION_ATTR_SESSION_ENABLE_TRANSMIT)
        {
            return SAI_STATUS_SUCCESS;
        }
        return old_set_twamp_session_attribute(twamp_session_id, attr);
    }

    sai_status_t _ut_stub_sai_get_switch_attribute(
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _Inout_ sai_attribute_t *attr_list)
    {
        if (attr_count == 1)
        {
            if (attr_list[0].id == SAI_SWITCH_ATTR_MAX_TWAMP_SESSION)
            {
                attr_list[0].value.u32 = 128;
                return SAI_STATUS_SUCCESS;
            }
        }
        return pold_sai_switch_api->get_switch_attribute(switch_id, attr_count, attr_list);
    }

    sai_status_t _ut_stub_sai_set_switch_attribute(
        _In_ sai_object_id_t switch_id,
        _In_ const sai_attribute_t *attr)
    {
        if (attr[0].id == SAI_SWITCH_ATTR_TWAMP_SESSION_EVENT_NOTIFY)
        {
            return SAI_STATUS_SUCCESS;
        }
        return pold_sai_switch_api->set_switch_attribute(switch_id, attr);
    }

    void _hook_sai_twamp_api()
    {
        ut_sai_twamp_api = *sai_twamp_api;
        pold_sai_twamp_api = sai_twamp_api;
        ut_sai_twamp_api.create_twamp_session = _ut_stub_sai_create_twamp_session;
        ut_sai_twamp_api.remove_twamp_session = _ut_stub_sai_remove_twamp_session;
        ut_sai_twamp_api.set_twamp_session_attribute = _ut_stub_sai_set_twamp_session_attribute;
        sai_twamp_api = &ut_sai_twamp_api;
    }

    void _unhook_sai_twamp_api()
    {
        sai_twamp_api = pold_sai_twamp_api;
    }

    void _hook_sai_switch_api()
    {
        ut_sai_switch_api = *sai_switch_api;
        pold_sai_switch_api = sai_switch_api;
        ut_sai_switch_api.get_switch_attribute = _ut_stub_sai_get_switch_attribute;
        ut_sai_switch_api.set_switch_attribute = _ut_stub_sai_set_switch_attribute;
        sai_switch_api = &ut_sai_switch_api;
    }

    void _unhook_sai_switch_api()
    {
        sai_switch_api = pold_sai_switch_api;
    }

    class MockTwampOrch final
    {
    public:
        MockTwampOrch()
        {
            this->confDb = std::make_shared<DBConnector>("CONFIG_DB", 0);
            TableConnector confDbTwampTable(this->confDb.get(), CFG_TWAMP_SESSION_TABLE_NAME);
            TableConnector stateDbTwampTable(this->confDb.get(), STATE_TWAMP_SESSION_TABLE_NAME);
            this->twampOrch = std::make_shared<TwampOrch>(confDbTwampTable, stateDbTwampTable, gSwitchOrch, gPortsOrch, gVrfOrch);
        }
        ~MockTwampOrch() = default;

        void doTwampTableTask(const std::deque<KeyOpFieldsValuesTuple> &entries)
        {
            auto consumer = dynamic_cast<Consumer *>((this->twampOrch.get())->getExecutor(CFG_TWAMP_SESSION_TABLE_NAME));
            consumer->addToSync(entries);
            static_cast<Orch *>(this->twampOrch.get())->doTask(*consumer);
        }

        void doTwampNotificationTask()
        {
            auto exec = static_cast<Notifier *>((this->twampOrch.get())->getExecutor("TWAMP_NOTIFICATIONS"));
            auto consumer = exec->getNotificationConsumer();
            consumer->readData();
            consumer->saveToSync();
           this->twampOrch->doTask(*consumer);
        }

        TwampOrch& get()
        {
            return *twampOrch;
        }

    private:
        std::shared_ptr<DBConnector> confDb;
        std::shared_ptr<TwampOrch> twampOrch;
    };

    class TwampOrchTest : public ::testing::Test
    {
    public:
        TwampOrchTest()
        {
            this->initDb();
        }
        virtual ~TwampOrchTest() = default;

        void SetUp() override
        {
            this->initSaiApi();
            this->initSwitch();
            this->initOrch();
            this->initPorts();
            _hook_sai_twamp_api();
            _hook_sai_switch_api();
        }

        void TearDown() override
        {
            this->deinitOrch();
            this->deinitSwitch();
            this->deinitSaiApi();
            _unhook_sai_twamp_api();
            _unhook_sai_switch_api();
        }

    private:
        void initSaiApi()
        {
            std::map<std::string, std::string> profileMap = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00"        }
            };
            auto status = ut_helper::initSaiApi(profileMap);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
        }

        void deinitSaiApi()
        {
            auto status = ut_helper::uninitSaiApi();
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
        }

        void initSwitch()
        {
            sai_status_t status;
            sai_attribute_t attr;

            // Create switch
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            // Get switch source MAC address
            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;

            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gMacAddress = attr.value.mac;

            // Get switch default virtual router ID
            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;

            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gVirtualRouterId = attr.value.oid;
        }

        void deinitSwitch()
        {
            // Remove switch
            auto status = sai_switch_api->remove_switch(gSwitchId);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gSwitchId = SAI_NULL_OBJECT_ID;
            gVirtualRouterId = SAI_NULL_OBJECT_ID;
        }

        void initOrch()
        {
            //
            // SwitchOrch
            //
            TableConnector state_switch_table(this->stateDb.get(), "SWITCH_CAPABILITY");
            TableConnector app_switch_table(this->appDb.get(), APP_SWITCH_TABLE_NAME);
            TableConnector conf_asic_sensors(this->configDb.get(), CFG_ASIC_SENSORS_TABLE_NAME);

            std::vector<TableConnector> switchTableList = {
                conf_asic_sensors,
                app_switch_table
            };

            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(this->appDb.get(), switchTableList, state_switch_table);
            gDirectory.set(gSwitchOrch);
            resourcesList.push_back(gSwitchOrch);

            //
            // PortsOrch
            //
            const int portsorch_base_pri = 40;

            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME,        portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME,        portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri     },
                { APP_LAG_TABLE_NAME,         portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME,  portsorch_base_pri     }
            };

            ASSERT_EQ(gPortsOrch, nullptr);
            gPortsOrch = new PortsOrch(this->appDb.get(), this->stateDb.get(), ports_tables, this->chassisAppDb.get());
            gDirectory.set(gPortsOrch);
            resourcesList.push_back(gPortsOrch);

            //
            // VrfOrch
            //
            ASSERT_EQ(gVrfOrch, nullptr);
            gVrfOrch = new VRFOrch(this->appDb.get(), APP_VRF_TABLE_NAME, this->stateDb.get(), STATE_VRF_OBJECT_TABLE_NAME);
            resourcesList.push_back(gVrfOrch);


            //
            // BufferOrch
            //
            std::vector<std::string> bufferTableList = {
                APP_BUFFER_POOL_TABLE_NAME,
                APP_BUFFER_PROFILE_TABLE_NAME,
                APP_BUFFER_QUEUE_TABLE_NAME,
                APP_BUFFER_PG_TABLE_NAME,
                APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME
            };
            gBufferOrch = new BufferOrch(this->appDb.get(), this->configDb.get(), this->stateDb.get(), bufferTableList);
            gDirectory.set(gBufferOrch);
            resourcesList.push_back(gBufferOrch);

            //
            // FlexCounterOrch
            //
            std::vector<std::string> flexCounterTableList = {
                CFG_FLEX_COUNTER_TABLE_NAME
            };

            auto flexCounterOrch = new FlexCounterOrch(this->configDb.get(), flexCounterTableList);
            gDirectory.set(flexCounterOrch);
            resourcesList.push_back(flexCounterOrch);

            //
            // CrmOrch
            //
            ASSERT_EQ(gCrmOrch, nullptr);
            gCrmOrch = new CrmOrch(this->configDb.get(), CFG_CRM_TABLE_NAME);
            gDirectory.set(gCrmOrch);
            resourcesList.push_back(gCrmOrch);
        }

        void deinitOrch()
        {
            std::reverse(resourcesList.begin(), resourcesList.end());
            for (auto &it : resourcesList)
            {
                delete it;
            }

            gSwitchOrch = nullptr;
            gPortsOrch = nullptr;
            gVrfOrch = nullptr;
            gBufferOrch = nullptr;
            gCrmOrch = nullptr;

            Portal::DirectoryInternal::clear(gDirectory);
            EXPECT_TRUE(Portal::DirectoryInternal::empty(gDirectory));
        }

        void initPorts()
        {
            auto portTable = Table(this->appDb.get(), APP_PORT_TABLE_NAME);

            // Get SAI default ports to populate DB
            auto ports = ut_helper::getInitialSaiPorts();

            // Populate port table with SAI ports
            for (const auto &cit : ports)
            {
                portTable.set(cit.first, cit.second);
            }

            // Set PortConfigDone
            portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch*>(gPortsOrch)->doTask();

            // Set PortInitDone
            portTable.set("PortInitDone", { { "lanes", "0" } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch*>(gPortsOrch)->doTask();
        }

        void initDb()
        {
            this->appDb = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            this->configDb = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            this->stateDb = std::make_shared<swss::DBConnector>("STATE_DB", 0);
            this->countersDb = make_shared<swss::DBConnector>("COUNTERS_DB", 0);
            this->chassisAppDb = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);
            this->asicDb = make_shared<swss::DBConnector>("ASIC_DB", 0);
        }

        shared_ptr<swss::DBConnector> appDb;
        shared_ptr<swss::DBConnector> configDb;
        shared_ptr<swss::DBConnector> stateDb;
        shared_ptr<swss::DBConnector> countersDb;
        shared_ptr<swss::DBConnector> chassisAppDb;
        shared_ptr<swss::DBConnector> asicDb;

        std::vector<Orch*> resourcesList;
    };

    TEST_F(TwampOrchTest, TwampOrchTestCreateDeleteSenderPacketCountSingle)
    {
        string twampSessionName = "TEST_SENDER1";

        MockTwampOrch twampOrch;

        auto current_create_count = create_twamp_session_count;
        auto current_remove_count = remove_twamp_session_count;
        auto current_set_count = set_twamp_session_count;

        // Create TWAMP Light session
        {
            std::deque<KeyOpFieldsValuesTuple> tableKofvt;
            tableKofvt.push_back(
                {
                    twampSessionName,
                    SET_COMMAND,
                    {
                        {"mode",                "LIGHT"   },
                        {"role",                "SENDER"  },
                        {"src_ip",              "1.1.1.1" },
                        {"src_udp_port",        "862"     },
                        {"dst_ip",              "2.2.2.2" },
                        {"dst_udp_port",        "863"     },
                        {"packet_count",        "1000"    },
                        {"tx_interval",         "10"      },
                        {"timeout",             "10"      },
                        {"statistics_interval", "20000"   },
                        {"vrf_name",            "default" },
                        {"dscp",                "0"       },
                        {"ttl",                 "10"      },
                        {"timestamp_format",    "ntp"     },
                        {"padding_size",        "100"     },
                        {"hw_lookup",           "true"    }
                    }
                }
            );

            twampOrch.doTwampTableTask(tableKofvt);

            string session_status;
            ASSERT_TRUE(twampOrch.get().getSessionStatus(twampSessionName, session_status));
            ASSERT_EQ(session_status, "inactive");
            ASSERT_EQ(current_create_count + 1, create_twamp_session_count);
        }

        // Start TWAMP Light session
        {
            std::deque<KeyOpFieldsValuesTuple> tableKofvt;
            tableKofvt.push_back(
                {
                    twampSessionName,
                    SET_COMMAND,
                    {
                        {"admin_state", "enabled"}
                    }
                }
            );

            twampOrch.doTwampTableTask(tableKofvt);

            string session_status;
            ASSERT_TRUE(twampOrch.get().getSessionStatus(twampSessionName, session_status));
            ASSERT_EQ(session_status, "active");
            ASSERT_EQ(current_set_count + 1, set_twamp_session_count);
        }

        // Process Notification
        {
            // mock a redis reply for notification
            mockReply = (redisReply *)calloc(sizeof(redisReply), 1);
            mockReply->type = REDIS_REPLY_ARRAY;
            mockReply->elements = 3; // REDIS_PUBLISH_MESSAGE_ELEMNTS
            mockReply->element = (redisReply **)calloc(sizeof(redisReply *), mockReply->elements);
            mockReply->element[2] = (redisReply *)calloc(sizeof(redisReply), 1);
            mockReply->element[2]->type = REDIS_REPLY_STRING;
            sai_twamp_session_event_notification_data_t twamp_session_data;
            sai_twamp_session_stat_t counters_ids[SAI_TWAMP_SESSION_STAT_DURATION_TS];
            uint64_t counters[SAI_TWAMP_SESSION_STAT_DURATION_TS];
            twamp_session_data.session_state = SAI_TWAMP_SESSION_STATE_INACTIVE;
            twamp_session_data.twamp_session_id = (sai_object_id_t)0x1;
            twamp_session_data.session_stats.index = 1;
            twamp_session_data.session_stats.number_of_counters = 11;

            counters_ids[0] = SAI_TWAMP_SESSION_STAT_RX_PACKETS;
            counters_ids[1] = SAI_TWAMP_SESSION_STAT_RX_BYTE;
            counters_ids[2] = SAI_TWAMP_SESSION_STAT_TX_PACKETS;
            counters_ids[3] = SAI_TWAMP_SESSION_STAT_TX_BYTE;
            counters_ids[4] = SAI_TWAMP_SESSION_STAT_DROP_PACKETS;
            counters_ids[5] = SAI_TWAMP_SESSION_STAT_MAX_LATENCY;
            counters_ids[6] = SAI_TWAMP_SESSION_STAT_MIN_LATENCY;
            counters_ids[7] = SAI_TWAMP_SESSION_STAT_AVG_LATENCY;
            counters_ids[8] = SAI_TWAMP_SESSION_STAT_MAX_JITTER;
            counters_ids[9] = SAI_TWAMP_SESSION_STAT_MIN_JITTER;
            counters_ids[10] = SAI_TWAMP_SESSION_STAT_AVG_JITTER;
            counters[0] = 1000;
            counters[1] = 100000;
            counters[2] = 1000;
            counters[3] = 100000;
            counters[4] = 0;
            counters[5] = 1987;
            counters[6] = 1983;
            counters[7] = 1984;
            counters[8] = 2097;
            counters[9] = 1896;
            counters[10] = 1985;
            twamp_session_data.session_stats.counters_ids = counters_ids;
            twamp_session_data.session_stats.counters = counters;

            std::string data = sai_serialize_twamp_session_event_ntf(1, &twamp_session_data);

            std::vector<FieldValueTuple> notifyValues;
            FieldValueTuple opdata("twamp_session_event", data);
            notifyValues.push_back(opdata);
            std::string msg = swss::JSon::buildJson(notifyValues);
            mockReply->element[2]->str = (char*)calloc(1, msg.length() + 1);
            memcpy(mockReply->element[2]->str, msg.c_str(), msg.length());

            // trigger the notification
            twampOrch.doTwampNotificationTask();
            mockReply = nullptr;

            TwampStatsTable twampStatistics = Portal::TwampOrchInternal::getTwampSessionStatistics(twampOrch.get());
            ASSERT_TRUE(twampStatistics.find(twampSessionName) != twampStatistics.end());
            ASSERT_EQ(twampStatistics[twampSessionName].rx_packets, 1000);
            ASSERT_EQ(twampStatistics[twampSessionName].rx_bytes, 100000);
            ASSERT_EQ(twampStatistics[twampSessionName].tx_packets, 1000);
            ASSERT_EQ(twampStatistics[twampSessionName].tx_bytes, 100000);
            ASSERT_EQ(twampStatistics[twampSessionName].drop_packets, 0);
            ASSERT_EQ(twampStatistics[twampSessionName].max_latency, 1987);
            ASSERT_EQ(twampStatistics[twampSessionName].min_latency, 1983);
            ASSERT_EQ(twampStatistics[twampSessionName].avg_latency, 1984);
            ASSERT_EQ(twampStatistics[twampSessionName].max_jitter, 2097);
            ASSERT_EQ(twampStatistics[twampSessionName].min_jitter, 1896);
            ASSERT_EQ(twampStatistics[twampSessionName].avg_jitter, 1985);
        }

        // Delete TWAMP Light session
        {
            std::deque<KeyOpFieldsValuesTuple> tableKofvt;
            tableKofvt.push_back(
                {
                    twampSessionName,
                    DEL_COMMAND,
                    { {} }
                }
            );

            twampOrch.doTwampTableTask(tableKofvt);

            string session_status;
            ASSERT_FALSE(twampOrch.get().getSessionStatus(twampSessionName, session_status));
            ASSERT_EQ(current_remove_count + 1, remove_twamp_session_count);
        }

        // Make sure both create and set has been called
        ASSERT_EQ(current_create_count + 1, create_twamp_session_count);
        ASSERT_EQ(current_remove_count + 1, remove_twamp_session_count);
        ASSERT_EQ(current_set_count + 1, set_twamp_session_count);
    }

    TEST_F(TwampOrchTest, TwampOrchTestCreateDeleteSenderPacketCountMulti)
    {
        string twampSessionName = "TEST_SENDER1";

        MockTwampOrch twampOrch;

        auto current_create_count = create_twamp_session_count;
        auto current_remove_count = remove_twamp_session_count;
        auto current_set_count = set_twamp_session_count;

        // Create TWAMP Light session
        {
            std::deque<KeyOpFieldsValuesTuple> tableKofvt;
            tableKofvt.push_back(
                {
                    twampSessionName,
                    SET_COMMAND,
                    {
                        {"mode",                "LIGHT"   },
                        {"role",                "SENDER"  },
                        {"src_ip",              "1.1.1.1" },
                        {"src_udp_port",        "1862"    },
                        {"dst_ip",              "2.2.2.2" },
                        {"dst_udp_port",        "1863"    },
                        {"packet_count",        "1000"    },
                        {"tx_interval",         "10"      },
                        {"timeout",             "10"      },
                        {"statistics_interval", "11000"   }
                    }
                }
            );

            twampOrch.doTwampTableTask(tableKofvt);

            string session_status;
            ASSERT_TRUE(twampOrch.get().getSessionStatus(twampSessionName, session_status));
            ASSERT_EQ(session_status, "inactive");
            ASSERT_EQ(current_create_count + 1, create_twamp_session_count);
        }

        // Start TWAMP Light session
        {
            std::deque<KeyOpFieldsValuesTuple> tableKofvt;
            tableKofvt.push_back(
                {
                    twampSessionName,
                    SET_COMMAND,
                    {
                        {"admin_state", "enabled"}
                    }
                }
            );

            twampOrch.doTwampTableTask(tableKofvt);

            string session_status;
            ASSERT_TRUE(twampOrch.get().getSessionStatus(twampSessionName, session_status));
            ASSERT_EQ(session_status, "active");
            ASSERT_EQ(current_set_count + 1, set_twamp_session_count);
        }

        // Process Notification
        {
            sai_twamp_session_event_notification_data_t twamp_session_data;
            sai_twamp_session_stat_t counters_ids[SAI_TWAMP_SESSION_STAT_DURATION_TS];
            uint64_t counters[SAI_TWAMP_SESSION_STAT_DURATION_TS];
            uint64_t latency_total = 0;
            uint64_t jitter_total = 0;
            twamp_session_data.twamp_session_id = (sai_object_id_t)0x1;
            twamp_session_data.session_stats.number_of_counters = 11;
            counters_ids[0] = SAI_TWAMP_SESSION_STAT_RX_PACKETS;
            counters_ids[1] = SAI_TWAMP_SESSION_STAT_RX_BYTE;
            counters_ids[2] = SAI_TWAMP_SESSION_STAT_TX_PACKETS;
            counters_ids[3] = SAI_TWAMP_SESSION_STAT_TX_BYTE;
            counters_ids[4] = SAI_TWAMP_SESSION_STAT_DROP_PACKETS;
            counters_ids[5] = SAI_TWAMP_SESSION_STAT_MAX_LATENCY;
            counters_ids[6] = SAI_TWAMP_SESSION_STAT_MIN_LATENCY;
            counters_ids[7] = SAI_TWAMP_SESSION_STAT_AVG_LATENCY;
            counters_ids[8] = SAI_TWAMP_SESSION_STAT_MAX_JITTER;
            counters_ids[9] = SAI_TWAMP_SESSION_STAT_MIN_JITTER;
            counters_ids[10] = SAI_TWAMP_SESSION_STAT_AVG_JITTER;
            twamp_session_data.session_stats.counters_ids = counters_ids;
            twamp_session_data.session_stats.counters = counters;
            for (uint8_t i = 1; i <= 10; i++)
            {
                // mock a redis reply for notification
                mockReply = (redisReply *)calloc(sizeof(redisReply), 1);
                mockReply->type = REDIS_REPLY_ARRAY;
                mockReply->elements = 3; // REDIS_PUBLISH_MESSAGE_ELEMNTS
                mockReply->element = (redisReply **)calloc(sizeof(redisReply *), mockReply->elements);
                mockReply->element[2] = (redisReply *)calloc(sizeof(redisReply), 1);
                mockReply->element[2]->type = REDIS_REPLY_STRING;

                twamp_session_data.session_state = (i<10) ? SAI_TWAMP_SESSION_STATE_ACTIVE : SAI_TWAMP_SESSION_STATE_INACTIVE;
                twamp_session_data.session_stats.index = i;
                counters[0] = 100;
                counters[1] = 10000;
                counters[2] = 100;
                counters[3] = 10000;
                counters[4] = 0;
                counters[5] = 1000+i;
                counters[6] = 1000+i;
                counters[7] = 1000+i;
                counters[8] = 1100+i;
                counters[9] = 1100+i;
                counters[10] = 1100+i;
                latency_total += counters[7];
                jitter_total += counters[10];

                std::string data = sai_serialize_twamp_session_event_ntf(1, &twamp_session_data);

                std::vector<FieldValueTuple> notifyValues;
                FieldValueTuple opdata("twamp_session_event", data);
                notifyValues.push_back(opdata);
                std::string msg = swss::JSon::buildJson(notifyValues);
                mockReply->element[2]->str = (char*)calloc(1, msg.length() + 1);
                memcpy(mockReply->element[2]->str, msg.c_str(), msg.length());

                // trigger the notification
                twampOrch.doTwampNotificationTask();
                mockReply = nullptr;

                string session_status;
                ASSERT_TRUE(twampOrch.get().getSessionStatus(twampSessionName, session_status));
                if (i<10)
                {
                    ASSERT_EQ(session_status, "active");
                }
                else
                {
                    ASSERT_EQ(session_status, "inactive");
                }

                TwampStatsTable twampStatistics = Portal::TwampOrchInternal::getTwampSessionStatistics(twampOrch.get());
                ASSERT_TRUE(twampStatistics.find(twampSessionName) != twampStatistics.end());
                ASSERT_EQ(twampStatistics[twampSessionName].rx_packets, 100*i);
                ASSERT_EQ(twampStatistics[twampSessionName].rx_bytes, 10000*i);
                ASSERT_EQ(twampStatistics[twampSessionName].tx_packets, 100*i);
                ASSERT_EQ(twampStatistics[twampSessionName].tx_bytes, 10000*i);
                ASSERT_EQ(twampStatistics[twampSessionName].drop_packets, 0);
                ASSERT_EQ(twampStatistics[twampSessionName].max_latency, 1000+i);
                ASSERT_EQ(twampStatistics[twampSessionName].min_latency, 1000+1);
                ASSERT_EQ(twampStatistics[twampSessionName].avg_latency, latency_total/i);
                ASSERT_EQ(twampStatistics[twampSessionName].max_jitter, 1100+i);
                ASSERT_EQ(twampStatistics[twampSessionName].min_jitter, 1100+1);
                ASSERT_EQ(twampStatistics[twampSessionName].avg_jitter, jitter_total/i);
            }
        }

        // Delete TWAMP Light session
        {
            std::deque<KeyOpFieldsValuesTuple> tableKofvt;
            tableKofvt.push_back(
                {
                    twampSessionName,
                    DEL_COMMAND,
                    { {} }
                }
            );

            twampOrch.doTwampTableTask(tableKofvt);

            string session_status;
            ASSERT_FALSE(twampOrch.get().getSessionStatus(twampSessionName, session_status));
            ASSERT_EQ(current_remove_count + 1, remove_twamp_session_count);
        }

        // Make sure both create and set has been called
        ASSERT_EQ(current_create_count + 1, create_twamp_session_count);
        ASSERT_EQ(current_remove_count + 1, remove_twamp_session_count);
        ASSERT_EQ(current_set_count + 1, set_twamp_session_count);
    }

    TEST_F(TwampOrchTest, TwampOrchTestCreateDeleteSenderContinuousSingle)
    {
        string twampSessionName = "TEST_SENDER1";

        MockTwampOrch twampOrch;

        auto current_create_count = create_twamp_session_count;
        auto current_remove_count = remove_twamp_session_count;
        auto current_set_count = set_twamp_session_count;

        // Create TWAMP Light session
        {
            std::deque<KeyOpFieldsValuesTuple> tableKofvt;
            tableKofvt.push_back(
                {
                    twampSessionName,
                    SET_COMMAND,
                    {
                        {"mode",                "LIGHT"   },
                        {"role",                "SENDER"  },
                        {"src_ip",              "1.1.1.1" },
                        {"src_udp_port",        "862"     },
                        {"dst_ip",              "2.2.2.2" },
                        {"dst_udp_port",        "863"     },
                        {"monitor_time",        "60"      },
                        {"tx_interval",         "100"     },
                        {"timeout",             "10"      },
                        {"statistics_interval", "60000"   },
                        {"vrf_name",            "default" },
                        {"dscp",                "0"       },
                        {"ttl",                 "10"      },
                        {"timestamp_format",    "ntp"     },
                        {"padding_size",        "100"     },
                        {"hw_lookup",           "true"    }
                    }
                }
            );

            twampOrch.doTwampTableTask(tableKofvt);

            string session_status;
            ASSERT_TRUE(twampOrch.get().getSessionStatus(twampSessionName, session_status));
            ASSERT_EQ(session_status, "inactive");
            ASSERT_EQ(current_create_count + 1, create_twamp_session_count);
        }

        // Start TWAMP Light session
        {
            std::deque<KeyOpFieldsValuesTuple> tableKofvt;
            tableKofvt.push_back(
                {
                    twampSessionName,
                    SET_COMMAND,
                    {
                        {"admin_state", "enabled"}
                    }
                }
            );

            twampOrch.doTwampTableTask(tableKofvt);

            string session_status;
            ASSERT_TRUE(twampOrch.get().getSessionStatus(twampSessionName, session_status));
            ASSERT_EQ(session_status, "active");
            ASSERT_EQ(current_set_count + 1, set_twamp_session_count);
        }

        // Delete TWAMP Light session
        {
            std::deque<KeyOpFieldsValuesTuple> tableKofvt;
            tableKofvt.push_back(
                {
                    twampSessionName,
                    DEL_COMMAND,
                    { {} }
                }
            );

            twampOrch.doTwampTableTask(tableKofvt);

            string session_status;
            ASSERT_FALSE(twampOrch.get().getSessionStatus(twampSessionName, session_status));
            ASSERT_EQ(current_remove_count + 1, remove_twamp_session_count);
        }

        // Make sure both create and set has been called
        ASSERT_EQ(current_create_count + 1, create_twamp_session_count);
        ASSERT_EQ(current_remove_count + 1, remove_twamp_session_count);
        ASSERT_EQ(current_set_count + 1, set_twamp_session_count);
    }

    TEST_F(TwampOrchTest, TwampOrchTestCreateDeleteSenderContinuousMulti)
    {
        string twampSessionName = "TEST_SENDER1";

        MockTwampOrch twampOrch;

        auto current_create_count = create_twamp_session_count;
        auto current_remove_count = remove_twamp_session_count;
        auto current_set_count = set_twamp_session_count;

        // Create TWAMP Light session
        {
            std::deque<KeyOpFieldsValuesTuple> tableKofvt;
            tableKofvt.push_back(
                {
                    twampSessionName,
                    SET_COMMAND,
                    {
                        {"mode",                "LIGHT"   },
                        {"role",                "SENDER"  },
                        {"src_ip",              "1.1.1.1" },
                        {"src_udp_port",        "1862"    },
                        {"dst_ip",              "2.2.2.2" },
                        {"dst_udp_port",        "1863"    },
                        {"monitor_time",        "0"       },
                        {"tx_interval",         "100"     },
                        {"timeout",             "10"      },
                        {"statistics_interval", "20000"   },
                    }
                }
            );

            twampOrch.doTwampTableTask(tableKofvt);

            string session_status;
            ASSERT_TRUE(twampOrch.get().getSessionStatus(twampSessionName, session_status));
            ASSERT_EQ(session_status, "inactive");
            ASSERT_EQ(current_create_count + 1, create_twamp_session_count);
        }

        // Start TWAMP Light session
        {
            std::deque<KeyOpFieldsValuesTuple> tableKofvt;
            tableKofvt.push_back(
                {
                    twampSessionName,
                    SET_COMMAND,
                    {
                        {"admin_state", "enabled"}
                    }
                }
            );

            twampOrch.doTwampTableTask(tableKofvt);

            string session_status;
            ASSERT_TRUE(twampOrch.get().getSessionStatus(twampSessionName, session_status));
            ASSERT_EQ(session_status, "active");
            ASSERT_EQ(current_set_count + 1, set_twamp_session_count);
        }

        // Stop TWAMP Light session
        {
            std::deque<KeyOpFieldsValuesTuple> tableKofvt;
            tableKofvt.push_back(
                {
                    twampSessionName,
                    SET_COMMAND,
                    {
                        {"admin_state", "disabled"}
                    }
                }
            );

            twampOrch.doTwampTableTask(tableKofvt);

            string session_status;
            ASSERT_TRUE(twampOrch.get().getSessionStatus(twampSessionName, session_status));
            ASSERT_EQ(session_status, "inactive");
            ASSERT_EQ(current_set_count + 2, set_twamp_session_count);
        }

        // Delete TWAMP Light session
        {
            std::deque<KeyOpFieldsValuesTuple> tableKofvt;
            tableKofvt.push_back(
                {
                    twampSessionName,
                    DEL_COMMAND,
                    { {} }
                }
            );

            twampOrch.doTwampTableTask(tableKofvt);

            string session_status;
            ASSERT_FALSE(twampOrch.get().getSessionStatus(twampSessionName, session_status));
            ASSERT_EQ(current_remove_count + 1, remove_twamp_session_count);
        }

        // Make sure both create and set has been called
        ASSERT_EQ(current_create_count + 1, create_twamp_session_count);
        ASSERT_EQ(current_remove_count + 1, remove_twamp_session_count);
        ASSERT_EQ(current_set_count + 2, set_twamp_session_count);
    }

    TEST_F(TwampOrchTest, TwampOrchTestCreateDeleteReflector)
    {
        string twampSessionName = "TEST_SENDER1";

        MockTwampOrch twampOrch;

        auto current_create_count = create_twamp_session_count;
        auto current_remove_count = remove_twamp_session_count;
        auto current_set_count = set_twamp_session_count;

        // Create TWAMP Light session
        {
            std::deque<KeyOpFieldsValuesTuple> tableKofvt;
            tableKofvt.push_back(
                {
                    twampSessionName,
                    SET_COMMAND,
                    {
                        {"mode", "LIGHT"},
                        {"role", "REFLECTOR"},
                        {"src_ip", "1.1.1.1"},
                        {"src_udp_port", "862"},
                        {"dst_ip", "2.2.2.2"},
                        {"dst_udp_port", "863"}
                    }
                }
            );

            twampOrch.doTwampTableTask(tableKofvt);

            string session_status;
            ASSERT_TRUE(twampOrch.get().getSessionStatus(twampSessionName, session_status));
            ASSERT_EQ(session_status, "active");
            ASSERT_EQ(current_create_count + 1, create_twamp_session_count);
        }

        // Delete TWAMP Light session
        {
            std::deque<KeyOpFieldsValuesTuple> tableKofvt;
            tableKofvt.push_back(
                {
                    twampSessionName,
                    DEL_COMMAND,
                    { {} }
                }
            );

            twampOrch.doTwampTableTask(tableKofvt);

            string session_status;
            ASSERT_FALSE(twampOrch.get().getSessionStatus(twampSessionName, session_status));
            ASSERT_EQ(current_remove_count + 1, remove_twamp_session_count);
        }

        // Make sure both create and set has been called
        ASSERT_EQ(current_create_count + 1, create_twamp_session_count);
        ASSERT_EQ(current_remove_count + 1, remove_twamp_session_count);
        ASSERT_EQ(current_set_count, set_twamp_session_count);
    }
}