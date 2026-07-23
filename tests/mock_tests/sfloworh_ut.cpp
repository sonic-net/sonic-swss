#include <algorithm>
#include <string>
#include <vector>
#include <map>
#include <set>

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mirrororch_sample_port_sai_wrap.h"

using namespace swss;

namespace sflow_test
{
    sai_status_t failSaiCreate(sai_object_id_t *, sai_object_id_t, uint32_t,
                               const sai_attribute_t *)
    {
        return SAI_STATUS_INSUFFICIENT_RESOURCES;
    }

    sai_status_t failSaiSetSwitchAttribute(sai_object_id_t, const sai_attribute_t *)
    {
        return SAI_STATUS_INSUFFICIENT_RESOURCES;
    }

    template <typename SaiApi>
    class ScopedSaiApiOverride final
    {
    public:
        explicit ScopedSaiApiOverride(SaiApi *&target) : target(target), original(target), replacement(*target)
        {
            this->target = &replacement;
        }

        ~ScopedSaiApiOverride()
        {
            target = original;
        }

        ScopedSaiApiOverride(const ScopedSaiApiOverride &) = delete;
        ScopedSaiApiOverride &operator=(const ScopedSaiApiOverride &) = delete;

        SaiApi &api()
        {
            return replacement;
        }

    private:
        SaiApi *&target;
        SaiApi *original;
        SaiApi replacement;
    };

    class MockSflowOrch final
    {
    public:
        MockSflowOrch()
        {
            this->appDb = std::make_shared<DBConnector>("APPL_DB", 0);
            std::vector<string> sflow_tables = {
                APP_SFLOW_TABLE_NAME,
                APP_SFLOW_SESSION_TABLE_NAME,
                APP_SFLOW_SAMPLE_RATE_TABLE_NAME
            };
            sflowOrch = std::make_shared<SflowOrch>(this->appDb.get(),  sflow_tables);
        }
        ~MockSflowOrch() = default;

        void doSflowTableTask(const std::deque<KeyOpFieldsValuesTuple> &entries)
        {
            // ConsumerStateTable is used for APP DB
            auto consumer = std::unique_ptr<Consumer>(new Consumer(
                new ConsumerStateTable(this->appDb.get(), APP_SFLOW_TABLE_NAME, 1, 1),
                this->sflowOrch.get(), APP_SFLOW_TABLE_NAME
            ));

            consumer->addToSync(entries);
            static_cast<Orch*>(this->sflowOrch.get())->doTask(*consumer);
        }

        void doSflowSessionTableTask(const std::deque<KeyOpFieldsValuesTuple> &entries)
        {
            // ConsumerStateTable is used for APP DB
            auto consumer = std::unique_ptr<Consumer>(new Consumer(
                new ConsumerStateTable(this->appDb.get(), APP_SFLOW_SESSION_TABLE_NAME, 1, 1),
                this->sflowOrch.get(), APP_SFLOW_SESSION_TABLE_NAME
            ));

            consumer->addToSync(entries);
            static_cast<Orch*>(this->sflowOrch.get())->doTask(*consumer);
        }

        void doSflowSampleTableTask(const std::deque<KeyOpFieldsValuesTuple> &entries)
        {
            // ConsumerStateTable is used for APP DB
            auto consumer = std::unique_ptr<Consumer>(new Consumer(
                new ConsumerStateTable(this->appDb.get(), APP_SFLOW_SAMPLE_RATE_TABLE_NAME, 1, 1),
                this->sflowOrch.get(), APP_SFLOW_SAMPLE_RATE_TABLE_NAME
            ));

            consumer->addToSync(entries);
            static_cast<Orch*>(this->sflowOrch.get())->doTask(*consumer);
        }

        SflowOrch& get()
        {
            return *sflowOrch;
        }

    private:
        std::shared_ptr<SflowOrch> sflowOrch;
        std::shared_ptr<DBConnector> appDb;
    };

    void setDropMonitorLimit(MockSflowOrch &mock_orch, const string &limit)
    {
        auto table = deque<KeyOpFieldsValuesTuple>(
            {
                {
                    "global",
                    SET_COMMAND,
                    {
                        {"admin_state", "up"},
                        {"drop_monitor_limit", limit}
                    }
                }
            });
        mock_orch.doSflowTableTask(table);
    }

    class SflowOrchTest : public ::testing::Test
    {
    public:
        SflowOrchTest()
        {
            this->initDb();
        }
        virtual ~SflowOrchTest() = default;

        void SetUp() override
        {
            this->initSaiApi();
            this->initSwitch();
            this->initOrch();
            this->initPorts();
        }

        void TearDown() override
        {
            this->deinitOrch();
            this->deinitSwitch();
            this->deinitSaiApi();
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

            TableConnector switchCapTableStateDb(this->stateDb.get(), "SWITCH_CAPABILITY");
            TableConnector asicSensorsTableCfgDb(this->configDb.get(), CFG_ASIC_SENSORS_TABLE_NAME);
            TableConnector switchTableAppDb(this->appDb.get(), APP_SWITCH_TABLE_NAME);

            std::vector<TableConnector> switchTableList = {
                asicSensorsTableCfgDb,
                switchTableAppDb
            };

            gSwitchOrch = new SwitchOrch(this->appDb.get(), switchTableList, switchCapTableStateDb);
            gDirectory.set(gSwitchOrch);
            resourcesList.push_back(gSwitchOrch);

            //
            // PortsOrch
            //

            const int portsorchBasePri = 40;

            std::vector<table_name_with_pri_t> portTableList = {
                { APP_PORT_TABLE_NAME,        portsorchBasePri + 5 },
                { APP_VLAN_TABLE_NAME,        portsorchBasePri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorchBasePri     },
                { APP_LAG_TABLE_NAME,         portsorchBasePri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME,  portsorchBasePri     }
            };

            gPortsOrch = new PortsOrch(this->appDb.get(), this->stateDb.get(), portTableList, this->chassisAppDb.get());
            gDirectory.set(gPortsOrch);
            resourcesList.push_back(gPortsOrch);

            //
            // QosOrch
            //

            std::vector<std::string> qosTableList = {
                CFG_TC_TO_QUEUE_MAP_TABLE_NAME,
                CFG_SCHEDULER_TABLE_NAME,
                CFG_DSCP_TO_TC_MAP_TABLE_NAME,
                CFG_MPLS_TC_TO_TC_MAP_TABLE_NAME,
                CFG_DOT1P_TO_TC_MAP_TABLE_NAME,
                CFG_QUEUE_TABLE_NAME,
                CFG_PORT_QOS_MAP_TABLE_NAME,
                CFG_WRED_PROFILE_TABLE_NAME,
                CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
                CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME,
                CFG_PFC_PRIORITY_TO_QUEUE_MAP_TABLE_NAME,
                CFG_DSCP_TO_FC_MAP_TABLE_NAME,
                CFG_EXP_TO_FC_MAP_TABLE_NAME
            };
            gQosOrch = new QosOrch(this->configDb.get(), qosTableList);
            gDirectory.set(gQosOrch);
            resourcesList.push_back(gQosOrch);

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
        }

        void deinitOrch()
        {
            std::reverse(this->resourcesList.begin(), this->resourcesList.end());
            for (auto &it : this->resourcesList)
            {
                delete it;
            }

            gSwitchOrch = nullptr;
            gPortsOrch = nullptr;
            gQosOrch = nullptr;
            gBufferOrch = nullptr;

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
            this->chassisAppDb = std::make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);
        }

        std::shared_ptr<DBConnector> appDb;
        std::shared_ptr<DBConnector> configDb;
        std::shared_ptr<DBConnector> stateDb;
        std::shared_ptr<DBConnector> chassisAppDb;

        std::vector<Orch*> resourcesList;
    };

    /* Test enabling/disabling SFLOW */
    TEST_F(SflowOrchTest, SflowEnableDisable)
    {
        MockSflowOrch mock_orch;
        {
            auto table1 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        SET_COMMAND,
                        {
                            {"admin_state", "down"}
                        }
                    }
                });
            mock_orch.doSflowTableTask(table1);

            ASSERT_FALSE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
        }
        {
            auto table2 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        SET_COMMAND,
                        {
                            {"admin_state", "up"}
                        }
                    }
                });
            mock_orch.doSflowTableTask(table2);

            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
        }
    }

    /* Test create/delete SFLOW */
    TEST_F(SflowOrchTest, SflowCreateDelete)
    {
        MockSflowOrch mock_orch;
        {
            auto table3 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        SET_COMMAND,
                        {
                            {"admin_state", "up"},
                        }
                    }
                });
            mock_orch.doSflowTableTask(table3);
            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
        }
        {
            auto table4 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        DEL_COMMAND,
                        {
                            {"admin_state", "up"},
                        }
                    }
                });
            mock_orch.doSflowTableTask(table4);
            ASSERT_FALSE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
        }
    }

    /* Test enabling/disabling SFLOW drop monitor */
    TEST_F(SflowOrchTest, SflowDropMonitorEnableDisable)
    {
        // SFLOW drop monitor only enable when SFLOW is enabled
        MockSflowOrch mock_orch;
        {
            auto table1 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        SET_COMMAND,
                        {
                            {"admin_state", "down"},
                            {"drop_monitor_limit", "100"}
                        }
                    }
                });
            mock_orch.doSflowTableTask(table1);

            ASSERT_FALSE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
            ASSERT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
        }
        {
            auto table2 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        SET_COMMAND,
                        {
                            {"admin_state", "up"},
                            {"drop_monitor_limit", "100"}
                        }
                    }
                });
            mock_orch.doSflowTableTask(table2);

            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
        }
        // Disable SFLOW drop monitor by setting rate limit to 0
        {
            auto table3 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        SET_COMMAND,
                        {
                            {"admin_state", "up"},
                            {"drop_monitor_limit", "0"}
                        }
                    }
                });
            mock_orch.doSflowTableTask(table3);

            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
            ASSERT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
        }
    }

    /* Test changing SFLOW drop monitor limit rate */
    TEST_F(SflowOrchTest, SflowDropMonitorChangeLimitRate)
    {
        MockSflowOrch mock_orch;
        {
            auto table1 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        SET_COMMAND,
                        {
                            {"admin_state", "up"},
                            {"drop_monitor_limit", "100"}
                        }
                    }
                });
            mock_orch.doSflowTableTask(table1);

            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
            ASSERT_EQ(Portal::SflowOrchInternal::getSflowDropMonitorLimitRate(mock_orch.get()), 100);
        }
        {
            auto table2 = deque<KeyOpFieldsValuesTuple>(
                {
                    {
                        "global",
                        SET_COMMAND,
                        {
                            {"admin_state", "up"},
                            {"drop_monitor_limit", "200"}
                        }
                    }
                });
            mock_orch.doSflowTableTask(table2);

            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowStatusEnable(mock_orch.get()));
            ASSERT_TRUE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
            ASSERT_EQ(Portal::SflowOrchInternal::getSflowDropMonitorLimitRate(mock_orch.get()), 200);
        }
    }

    /* Test: getDropMonitorCpuQueue fallback when config file not found */
    TEST_F(SflowOrchTest, SflowDropMonitorCpuQueueFileNotFound)
    {
        MockSflowOrch mock_orch;
        uint32_t queue = Portal::SflowOrchInternal::getSflowDropMonitorCpuQueue(
            mock_orch.get(), "./nonexistent_sflow_mod.json");
        ASSERT_EQ(queue, 47);
    }

    /* Test: getDropMonitorCpuQueue reads valid config file */
    TEST_F(SflowOrchTest, SflowDropMonitorCpuQueueFromFile)
    {
        MockSflowOrch mock_orch;
        uint32_t queue = Portal::SflowOrchInternal::getSflowDropMonitorCpuQueue(
            mock_orch.get(), "./sflow_mod_valid.json");
        ASSERT_EQ(queue, 99);
    }

    /* Test: getDropMonitorCpuQueue fallback when config value is invalid type */
    TEST_F(SflowOrchTest, SflowDropMonitorCpuQueueInvalidValue)
    {
        MockSflowOrch mock_orch;
        uint32_t queue = Portal::SflowOrchInternal::getSflowDropMonitorCpuQueue(
            mock_orch.get(), "./sflow_mod_invalid.json");
        ASSERT_EQ(queue, 47);
    }

    TEST_F(SflowOrchTest, SflowDropMonitorTamReportCreateFailure)
    {
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        tam_api_override.api().create_tam_report = failSaiCreate;
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
    }

    TEST_F(SflowOrchTest, SflowDropMonitorTamCreateFailure)
    {
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        tam_api_override.api().create_tam = failSaiCreate;
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
    }

    TEST_F(SflowOrchTest, SflowDropMonitorTrapGroupCreateFailure)
    {
        ScopedSaiApiOverride<sai_hostif_api_t> hostif_api_override(sai_hostif_api);
        hostif_api_override.api().create_hostif_trap_group = failSaiCreate;
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
    }

    TEST_F(SflowOrchTest, SflowDropMonitorTamEventActionCreateFailure)
    {
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        tam_api_override.api().create_tam_event_action = failSaiCreate;
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
    }

    TEST_F(SflowOrchTest, SflowDropMonitorTamTransportCreateFailure)
    {
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        tam_api_override.api().create_tam_transport = failSaiCreate;
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
    }

    TEST_F(SflowOrchTest, SflowDropMonitorPolicerCreateFailure)
    {
        ScopedSaiApiOverride<sai_policer_api_t> policer_api_override(sai_policer_api);
        policer_api_override.api().create_policer = failSaiCreate;
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
    }

    TEST_F(SflowOrchTest, SflowDropMonitorUserDefinedTrapCreateFailure)
    {
        ScopedSaiApiOverride<sai_hostif_api_t> hostif_api_override(sai_hostif_api);
        hostif_api_override.api().create_hostif_user_defined_trap = failSaiCreate;
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
    }

    TEST_F(SflowOrchTest, SflowDropMonitorTamCollectorCreateFailure)
    {
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        tam_api_override.api().create_tam_collector = failSaiCreate;
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
    }

    TEST_F(SflowOrchTest, SflowDropMonitorTamEventCreateFailure)
    {
        ScopedSaiApiOverride<sai_tam_api_t> tam_api_override(sai_tam_api);
        tam_api_override.api().create_tam_event = failSaiCreate;
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
    }

    TEST_F(SflowOrchTest, SflowDropMonitorReconfigureSwitchUnbindFailure)
    {
        ScopedSaiApiOverride<sai_switch_api_t> switch_api_override(sai_switch_api);
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        ASSERT_TRUE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
        switch_api_override.api().set_switch_attribute = failSaiSetSwitchAttribute;
        setDropMonitorLimit(mock_orch, "200");
        EXPECT_TRUE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
    }

    TEST_F(SflowOrchTest, SflowDropMonitorSwitchBindFailure)
    {
        ScopedSaiApiOverride<sai_switch_api_t> switch_api_override(sai_switch_api);
        switch_api_override.api().set_switch_attribute = failSaiSetSwitchAttribute;
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        EXPECT_FALSE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
    }

    TEST_F(SflowOrchTest, SflowDropMonitorSwitchUnbindFailure)
    {
        ScopedSaiApiOverride<sai_switch_api_t> switch_api_override(sai_switch_api);
        MockSflowOrch mock_orch;
        setDropMonitorLimit(mock_orch, "100");
        ASSERT_TRUE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
        switch_api_override.api().set_switch_attribute = failSaiSetSwitchAttribute;
        setDropMonitorLimit(mock_orch, "0");
        EXPECT_TRUE(Portal::SflowOrchInternal::getSflowDropMonitorStatusEnable(mock_orch.get()));
    }

    TEST_F(SflowOrchTest, SflowAddPortRejectsConflictingEgressBinding)
    {
        MockSflowOrch mock_orch;
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_NE(port.m_port_id, SAI_NULL_OBJECT_ID);

        sai_object_id_t foreign_oid;
        sai_attribute_t sp_attr;
        sp_attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr.value.u32 = 8888;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&foreign_oid, gSwitchId, 1, &sp_attr),
                  SAI_STATUS_SUCCESS);

        sai_attribute_t attr;
        attr.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = foreign_oid;
        ASSERT_EQ(sai_port_api->set_port_attribute(port.m_port_id, &attr), SAI_STATUS_SUCCESS);

        sai_object_id_t sflow_sample;
        sai_attribute_t sp_attr2;
        sp_attr2.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr2.value.u32 = 5678;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&sflow_sample, gSwitchId, 1, &sp_attr2),
                  SAI_STATUS_SUCCESS);

        ASSERT_FALSE(Portal::SflowOrchInternal::sflowAddPort(
            mock_orch.get(), sflow_sample, port.m_port_id, "tx"));

        sai_attribute_t after;
        after.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        ASSERT_EQ(sai_port_api->get_port_attribute(port.m_port_id, 1, &after), SAI_STATUS_SUCCESS);
        ASSERT_EQ(after.value.oid, foreign_oid);

        attr.value.oid = SAI_NULL_OBJECT_ID;
        sai_port_api->set_port_attribute(port.m_port_id, &attr);
        sai_samplepacket_api->remove_samplepacket(foreign_oid);
        sai_samplepacket_api->remove_samplepacket(sflow_sample);
    }

    TEST_F(SflowOrchTest, SflowAddPortAllowsSflowOwnedOidBinding)
    {
        MockSflowOrch mock_orch;
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_NE(port.m_port_id, SAI_NULL_OBJECT_ID);

        // Create two real samplepacket OIDs (SAI VS validates OIDs) and treat
        // both as sflow-owned via seedSamplePacketOid.
        sai_object_id_t prior_sflow_oid;
        sai_attribute_t sp_attr;
        sp_attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr.value.u32 = 10000;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&prior_sflow_oid, gSwitchId, 1, &sp_attr),
                  SAI_STATUS_SUCCESS);

        sai_object_id_t new_sflow_oid;
        sai_attribute_t sp_attr2;
        sp_attr2.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr2.value.u32 = 20000;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&new_sflow_oid, gSwitchId, 1, &sp_attr2),
                  SAI_STATUS_SUCCESS);

        Portal::SflowOrchInternal::seedSamplePacketOid(mock_orch.get(), 10000, prior_sflow_oid);

        // Pre-bind port egress to that sflow-owned OID
        sai_attribute_t attr;
        attr.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = prior_sflow_oid;
        ASSERT_EQ(sai_port_api->set_port_attribute(port.m_port_id, &attr), SAI_STATUS_SUCCESS);

        // Bind a different sflow sample_id on tx; pre-check sees prior_sflow_oid is
        // sflow-owned and lets the bind through.
        ASSERT_TRUE(Portal::SflowOrchInternal::sflowAddPort(
            mock_orch.get(), new_sflow_oid, port.m_port_id, "tx"));

        // Egress attr is now the new sflow OID
        sai_attribute_t after;
        after.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        ASSERT_EQ(sai_port_api->get_port_attribute(port.m_port_id, 1, &after), SAI_STATUS_SUCCESS);
        ASSERT_EQ(after.value.oid, new_sflow_oid);

        // Cleanup
        attr.value.oid = SAI_NULL_OBJECT_ID;
        sai_port_api->set_port_attribute(port.m_port_id, &attr);
        sai_samplepacket_api->remove_samplepacket(prior_sflow_oid);
        sai_samplepacket_api->remove_samplepacket(new_sflow_oid);
    }

    TEST_F(SflowOrchTest, SflowUpdateDirectionRejectsConflictingBinding)
    {
        MockSflowOrch mock_orch;
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_NE(port.m_port_id, SAI_NULL_OBJECT_ID);

        // Seed port info so sflowUpdateSampleDirection can find the sample_id.
        // Initial direction is "rx"; we are transitioning to "tx".
        sai_object_id_t my_sflow_oid;
        sai_attribute_t sp_attr;
        sp_attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr.value.u32 = 4242;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&my_sflow_oid, gSwitchId, 1, &sp_attr),
                  SAI_STATUS_SUCCESS);
        Portal::SflowOrchInternal::seedPortInfo(
            mock_orch.get(), port.m_port_id, my_sflow_oid, "rx");

        // Pre-bind port egress to a foreign (non-sflow) OID
        sai_object_id_t foreign_oid;
        sai_attribute_t sp_attr2;
        sp_attr2.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr2.value.u32 = 7777;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&foreign_oid, gSwitchId, 1, &sp_attr2),
                  SAI_STATUS_SUCCESS);

        sai_attribute_t attr;
        attr.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = foreign_oid;
        ASSERT_EQ(sai_port_api->set_port_attribute(port.m_port_id, &attr), SAI_STATUS_SUCCESS);

        // rx -> tx wants to assign egress; pre-check must detect the conflict
        // and bail out before any SAI mutation happens.
        ASSERT_FALSE(Portal::SflowOrchInternal::sflowUpdateSampleDirection(
            mock_orch.get(), port.m_port_id, "rx", "tx"));

        // Egress binding must remain the foreign OID (no SAI mutation happened)
        sai_attribute_t after;
        after.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        ASSERT_EQ(sai_port_api->get_port_attribute(port.m_port_id, 1, &after), SAI_STATUS_SUCCESS);
        ASSERT_EQ(after.value.oid, foreign_oid);

        // Cleanup
        attr.value.oid = SAI_NULL_OBJECT_ID;
        sai_port_api->set_port_attribute(port.m_port_id, &attr);
        sai_samplepacket_api->remove_samplepacket(my_sflow_oid);
        sai_samplepacket_api->remove_samplepacket(foreign_oid);
    }

    TEST_F(SflowOrchTest, SflowAddPortRejectsConflictingIngressBinding)
    {
        mirror_sample_port_wrap_ut::PortSampleSaiGuard saiPortSampleGuard;

        MockSflowOrch mock_orch;
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_NE(port.m_port_id, SAI_NULL_OBJECT_ID);

        // Pre-bind port ingress to a foreign (non-sflow) OID via the wrap.
        sai_object_id_t foreign_oid;
        sai_attribute_t sp_attr;
        sp_attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr.value.u32 = 8888;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&foreign_oid, gSwitchId, 1, &sp_attr),
                  SAI_STATUS_SUCCESS);

        sai_attribute_t attr;
        attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = foreign_oid;
        ASSERT_EQ(sai_port_api->set_port_attribute(port.m_port_id, &attr), SAI_STATUS_SUCCESS);

        sai_object_id_t sflow_sample;
        sai_attribute_t sp_attr2;
        sp_attr2.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr2.value.u32 = 5678;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&sflow_sample, gSwitchId, 1, &sp_attr2),
                  SAI_STATUS_SUCCESS);

        // rx bind must be rejected because ingress is held by a foreign OID.
        ASSERT_FALSE(Portal::SflowOrchInternal::sflowAddPort(
            mock_orch.get(), sflow_sample, port.m_port_id, "rx"));

        // Ingress binding must remain the foreign OID (pre-check bails before mutation).
        sai_attribute_t after;
        after.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
        ASSERT_EQ(sai_port_api->get_port_attribute(port.m_port_id, 1, &after), SAI_STATUS_SUCCESS);
        ASSERT_EQ(after.value.oid, foreign_oid);

        attr.value.oid = SAI_NULL_OBJECT_ID;
        sai_port_api->set_port_attribute(port.m_port_id, &attr);
        sai_samplepacket_api->remove_samplepacket(foreign_oid);
        sai_samplepacket_api->remove_samplepacket(sflow_sample);
    }

    TEST_F(SflowOrchTest, SflowUpdateDirectionRejectsConflictingIngressBinding)
    {
        mirror_sample_port_wrap_ut::PortSampleSaiGuard saiPortSampleGuard;

        MockSflowOrch mock_orch;
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_NE(port.m_port_id, SAI_NULL_OBJECT_ID);

        // Seed port info with initial direction "tx"; transition tx -> rx assigns
        // ingress, so the ingress conflict pre-check runs.
        sai_object_id_t my_sflow_oid;
        sai_attribute_t sp_attr;
        sp_attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr.value.u32 = 4242;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&my_sflow_oid, gSwitchId, 1, &sp_attr),
                  SAI_STATUS_SUCCESS);
        Portal::SflowOrchInternal::seedPortInfo(
            mock_orch.get(), port.m_port_id, my_sflow_oid, "tx");

        // Pre-bind port ingress to a foreign (non-sflow) OID via the wrap.
        sai_object_id_t foreign_oid;
        sai_attribute_t sp_attr2;
        sp_attr2.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
        sp_attr2.value.u32 = 7777;
        ASSERT_EQ(sai_samplepacket_api->create_samplepacket(&foreign_oid, gSwitchId, 1, &sp_attr2),
                  SAI_STATUS_SUCCESS);

        sai_attribute_t attr;
        attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = foreign_oid;
        ASSERT_EQ(sai_port_api->set_port_attribute(port.m_port_id, &attr), SAI_STATUS_SUCCESS);

        // tx -> rx wants to assign ingress; pre-check must detect the conflict
        // and bail out before any SAI mutation happens.
        ASSERT_FALSE(Portal::SflowOrchInternal::sflowUpdateSampleDirection(
            mock_orch.get(), port.m_port_id, "tx", "rx"));

        // Ingress binding must remain the foreign OID (no SAI mutation happened).
        sai_attribute_t after;
        after.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
        ASSERT_EQ(sai_port_api->get_port_attribute(port.m_port_id, 1, &after), SAI_STATUS_SUCCESS);
        ASSERT_EQ(after.value.oid, foreign_oid);

        attr.value.oid = SAI_NULL_OBJECT_ID;
        sai_port_api->set_port_attribute(port.m_port_id, &attr);
        sai_samplepacket_api->remove_samplepacket(my_sflow_oid);
        sai_samplepacket_api->remove_samplepacket(foreign_oid);
    }

}
