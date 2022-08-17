#include <algorithm>
#include <string>
#include <vector>
#include <map>
#include <set>

#include "ut_helper.h"
#include "mock_orchagent_main.h"

using namespace swss;

namespace sflow_test
{
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
}


