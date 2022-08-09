#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"

extern string gMySwitchType;


namespace bufferorch_test
{
    using namespace std;

    sai_port_api_t ut_sai_port_api;
    sai_port_api_t *pold_sai_port_api;

    shared_ptr<swss::DBConnector> m_app_db;
    shared_ptr<swss::DBConnector> m_config_db;
    shared_ptr<swss::DBConnector> m_state_db;
    shared_ptr<swss::DBConnector> m_chassis_app_db;

    uint32_t _ut_stub_expected_profile_count;
    uint32_t _ut_stub_port_profile_list_add_count;
    uint32_t _ut_stub_port_profile_list_del_count;
    sai_port_attr_t _ut_stub_expected_profile_list_type;
    sai_status_t _ut_stub_sai_set_port_attribute(
        _In_ sai_object_id_t port_id,
        _In_ const sai_attribute_t *attr)
    {
        if (attr[0].id == _ut_stub_expected_profile_list_type)
        {
            if (_ut_stub_expected_profile_count == attr[0].value.objlist.count)
            {
                if (_ut_stub_expected_profile_count != 0)
                {
                    _ut_stub_port_profile_list_add_count++;
                }
                else
                {
                    _ut_stub_port_profile_list_del_count++;
                }
            }
        }
        return pold_sai_port_api->set_port_attribute(port_id, attr);
    }

    void _hook_sai_port_api()
    {
        ut_sai_port_api = *sai_port_api;
        pold_sai_port_api = sai_port_api;
        ut_sai_port_api.set_port_attribute = _ut_stub_sai_set_port_attribute;
        sai_port_api = &ut_sai_port_api;
    }

    void _unhook_sai_port_api()
    {
        sai_port_api = pold_sai_port_api;
    }

    struct BufferOrchTest : public ::testing::Test
    {
        BufferOrchTest()
        {
        }

        void CheckDependency(const string &referencingTableName, const string &referencingObjectName, const string &field, const string &dependentTableName, const string &dependentObjectNames="")
        {
            auto &bufferTypeMaps = BufferOrch::m_buffer_type_maps;
            auto &referencingTable = (*bufferTypeMaps[referencingTableName]);
            auto &dependentTable = (*bufferTypeMaps[dependentTableName]);

            if (dependentObjectNames.empty())
            {
                ASSERT_TRUE(referencingTable[referencingObjectName].m_objsReferencingByMe[field].empty());
            }
            else
            {
                auto objects = tokenize(dependentObjectNames, ',');
                string reference;
                for (auto &object : objects)
                {
                    reference += dependentTableName + ":" + object + ",";
                    ASSERT_EQ(dependentTable[object].m_objsDependingOnMe.count(referencingObjectName), 1);
                }
                //reference.pop();
                ASSERT_EQ(referencingTable[referencingObjectName].m_objsReferencingByMe[field] + ",", reference);
            }
        }

        void RemoveItem(const string &table, const string &key)
        {
            std::deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({key, "DEL", {}});
            auto consumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(table));
            consumer->addToSync(entries);
        }

        void SetUp() override
        {
            ASSERT_EQ(sai_route_api, nullptr);
            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            ut_helper::initSaiApi(profile);

            // Init switch and create dependencies
            m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
            m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
            if(gMySwitchType == "voq")
                m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);

            sai_attribute_t attr;

            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            auto status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            // Get switch source MAC address
            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);

            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gMacAddress = attr.value.mac;

            // Get the default virtual router ID
            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);

            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            gVirtualRouterId = attr.value.oid;

            ASSERT_EQ(gCrmOrch, nullptr);
            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);

            TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
            TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);
            TableConnector app_switch_table(m_app_db.get(),  APP_SWITCH_TABLE_NAME);

            vector<TableConnector> switch_tables = {
                conf_asic_sensors,
                app_switch_table
            };

            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

            // Create dependencies ...

            const int portsorch_base_pri = 40;

            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
                { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
            };

            vector<string> flex_counter_tables = {
                CFG_FLEX_COUNTER_TABLE_NAME
            };
            auto* flexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(flexCounterOrch);

            ASSERT_EQ(gPortsOrch, nullptr);
            gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());

            ASSERT_EQ(gVrfOrch, nullptr);
            gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME, m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);

            ASSERT_EQ(gIntfsOrch, nullptr);
            gIntfsOrch = new IntfsOrch(m_app_db.get(), APP_INTF_TABLE_NAME, gVrfOrch, m_chassis_app_db.get());

            const int fdborch_pri = 20;

            vector<table_name_with_pri_t> app_fdb_tables = {
                { APP_FDB_TABLE_NAME,        FdbOrch::fdborch_pri},
                { APP_VXLAN_FDB_TABLE_NAME,  FdbOrch::fdborch_pri},
                { APP_MCLAG_FDB_TABLE_NAME,  fdborch_pri}
            };

            TableConnector stateDbFdb(m_state_db.get(), STATE_FDB_TABLE_NAME);
            TableConnector stateMclagDbFdb(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME);
            ASSERT_EQ(gFdbOrch, nullptr);
            gFdbOrch = new FdbOrch(m_app_db.get(), app_fdb_tables, stateDbFdb, stateMclagDbFdb, gPortsOrch);

            ASSERT_EQ(gNeighOrch, nullptr);
            gNeighOrch = new NeighOrch(m_app_db.get(), APP_NEIGH_TABLE_NAME, gIntfsOrch, gFdbOrch, gPortsOrch, m_chassis_app_db.get());

            vector<string> qos_tables = {
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
            gQosOrch = new QosOrch(m_config_db.get(), qos_tables);

            // Recreate buffer orch to read populated data
            vector<string> buffer_tables = { APP_BUFFER_POOL_TABLE_NAME,
                                             APP_BUFFER_PROFILE_TABLE_NAME,
                                             APP_BUFFER_QUEUE_TABLE_NAME,
                                             APP_BUFFER_PG_TABLE_NAME,
                                             APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                                             APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME };

            gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);

            Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

            // Get SAI default ports to populate DB
            auto ports = ut_helper::getInitialSaiPorts();

            // Populate pot table with SAI ports
            for (const auto &it : ports)
            {
                portTable.set(it.first, it.second);
            }

            // Set PortConfigDone
            portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch *>(gPortsOrch)->doTask();

            portTable.set("PortInitDone", { { "lanes", "0" } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch *>(gPortsOrch)->doTask();

            Table bufferPoolTable = Table(m_app_db.get(), APP_BUFFER_POOL_TABLE_NAME);
            Table bufferProfileTable = Table(m_app_db.get(), APP_BUFFER_PROFILE_TABLE_NAME);

            bufferPoolTable.set("ingress_lossless_pool",
                                {
                                    {"size", "1024000"},
                                    {"mode", "dynamic"},
                                    {"type", "ingress"}
                                });
            bufferPoolTable.set("ingress_lossy_pool",
                                {
                                    {"size", "1024000"},
                                    {"mode", "dynamic"},
                                    {"type", "ingress"}
                                });
            bufferProfileTable.set("ingress_lossless_profile",
                                   {
                                       {"pool", "ingress_lossless_pool"},
                                       {"size", "0"},
                                       {"dynamic_th", "0"}
                                   });
            bufferProfileTable.set("ingress_lossy_profile",
                                   {
                                       {"pool", "ingress_lossy_pool"},
                                       {"size", "0"},
                                       {"dynamic_th", "0"}
                                   });

            gBufferOrch->addExistingData(&bufferPoolTable);
            gBufferOrch->addExistingData(&bufferProfileTable);

            static_cast<Orch *>(gBufferOrch)->doTask();
        }

        void TearDown() override
        {
            auto buffer_maps = BufferOrch::m_buffer_type_maps;
            for (auto &i : buffer_maps)
            {
                i.second->clear();
            }

            gDirectory.m_values.clear();

            delete gCrmOrch;
            gCrmOrch = nullptr;

            delete gSwitchOrch;
            gSwitchOrch = nullptr;

            delete gVrfOrch;
            gVrfOrch = nullptr;

            delete gIntfsOrch;
            gIntfsOrch = nullptr;

            delete gNeighOrch;
            gNeighOrch = nullptr;

            delete gFdbOrch;
            gFdbOrch = nullptr;

            delete gPortsOrch;
            gPortsOrch = nullptr;

            delete gQosOrch;
            gQosOrch = nullptr;

            ut_helper::uninitSaiApi();
        }
    };

    TEST_F(BufferOrchTest, BufferOrchTestBufferPgReferencingObjRemoveThenAdd)
    {
        vector<string> ts;
        std::deque<KeyOpFieldsValuesTuple> entries;
        Table bufferPgTable = Table(m_app_db.get(), APP_BUFFER_PG_TABLE_NAME);

        bufferPgTable.set("Ethernet0:0",
                          {
                              {"profile", "ingress_lossy_profile"}
                          });
        gBufferOrch->addExistingData(&bufferPgTable);
        static_cast<Orch *>(gBufferOrch)->doTask();
        CheckDependency(APP_BUFFER_PG_TABLE_NAME, "Ethernet0:0", "profile", APP_BUFFER_PROFILE_TABLE_NAME, "ingress_lossy_profile");

        // Remove referenced obj
        entries.push_back({"ingress_lossy_profile", "DEL", {}});
        auto bufferProfileConsumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PROFILE_TABLE_NAME));
        bufferProfileConsumer->addToSync(entries);
        entries.clear();
        // Drain BUFFER_PROFILE_TABLE
        static_cast<Orch *>(gBufferOrch)->doTask();
        // Make sure the dependency remains
        CheckDependency(APP_BUFFER_PG_TABLE_NAME, "Ethernet0:0", "profile", APP_BUFFER_PROFILE_TABLE_NAME, "ingress_lossy_profile");
        // Make sure the notification isn't drained
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 1);
        ASSERT_EQ(ts[0], "BUFFER_PROFILE_TABLE:ingress_lossy_profile|DEL");
        ts.clear();

        // Remove and readd referencing obj
        entries.push_back({"Ethernet0:0", "DEL", {}});
        entries.push_back({"Ethernet0:0", "SET",
                           {
                               {"profile", "ingress_lossy_profile"}
                           }});
        auto bufferPgConsumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PG_TABLE_NAME));
        bufferPgConsumer->addToSync(entries);
        entries.clear();
        // Drain the BUFFER_PG_TABLE
        static_cast<Orch *>(gBufferOrch)->doTask();
        // Drain the BUFFER_PROFILE_TABLE which contains items need to retry
        static_cast<Orch *>(gBufferOrch)->doTask();
        // The dependency should be removed
        CheckDependency(APP_BUFFER_PG_TABLE_NAME, "Ethernet0:0", "profile", APP_BUFFER_PROFILE_TABLE_NAME);
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 1);
        ASSERT_EQ(ts[0], "BUFFER_PG_TABLE:Ethernet0:0|SET|profile:ingress_lossy_profile");
        ts.clear();

        // Re-create referenced obj
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossy_pool"},
                               {"size", "0"},
                               {"dynamic_th", "0"}
                           }});
        bufferProfileConsumer->addToSync(entries);
        entries.clear();
        // Drain BUFFER_PROFILE_TABLE table
        static_cast<Orch *>(gBufferOrch)->doTask();
        // Make sure the dependency recovers
        CheckDependency(APP_BUFFER_PG_TABLE_NAME, "Ethernet0:0", "profile", APP_BUFFER_PROFILE_TABLE_NAME, "ingress_lossy_profile");

        // All items have been drained
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
    }

    TEST_F(BufferOrchTest, BufferOrchTestReferencingObjRemoveThenAdd)
    {
        _hook_sai_port_api();
        vector<string> ts;
        std::deque<KeyOpFieldsValuesTuple> entries;
        Table bufferProfileListTable = Table(m_app_db.get(), APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME);
        bufferProfileListTable.set("Ethernet0",
                                   {
                                       {"profile_list", "ingress_lossy_profile,ingress_lossless_profile"}
                                   });
        gBufferOrch->addExistingData(&bufferProfileListTable);
        auto sai_port_profile_list_create_count = _ut_stub_port_profile_list_add_count;
        _ut_stub_expected_profile_count = 2;
        _ut_stub_expected_profile_list_type = SAI_PORT_ATTR_QOS_INGRESS_BUFFER_PROFILE_LIST;
        static_cast<Orch *>(gBufferOrch)->doTask();
        ASSERT_EQ(++sai_port_profile_list_create_count, _ut_stub_port_profile_list_add_count);
        CheckDependency(APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, "Ethernet0", "profile_list",
                        APP_BUFFER_PROFILE_TABLE_NAME, "ingress_lossy_profile,ingress_lossless_profile");

        // Remove and recreate the referenced profile
        entries.push_back({"ingress_lossy_profile", "DEL", {}});
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossy_pool"},
                               {"size", "0"},
                               {"dynamic_th", "0"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PROFILE_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();
        // Drain BUFFER_PROFILE_TABLE table
        static_cast<Orch *>(gBufferOrch)->doTask();
        // Make sure the dependency recovers
        CheckDependency(APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, "Ethernet0", "profile_list",
                        APP_BUFFER_PROFILE_TABLE_NAME, "ingress_lossy_profile,ingress_lossless_profile");
        // Make sure the notification isn't drained
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 2);
        ASSERT_EQ(ts[0], "BUFFER_PROFILE_TABLE:ingress_lossy_profile|DEL");
        ASSERT_EQ(ts[1], "BUFFER_PROFILE_TABLE:ingress_lossy_profile|SET|pool:ingress_lossy_pool|size:0|dynamic_th:0");
        ts.clear();

        // Remove and recreate the referenced pool
        entries.push_back({"ingress_lossy_pool", "DEL", {}});
        entries.push_back({"ingress_lossy_pool", "SET",
                           {
                               {"type", "ingress"},
                               {"size", "1024000"},
                               {"mode", "dynamic"}
                           }});
        consumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_POOL_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();
        // Drain BUFFER_POOL_TABLE table
        static_cast<Orch *>(gBufferOrch)->doTask();
        // Make sure the dependency recovers
        CheckDependency(APP_BUFFER_PROFILE_TABLE_NAME, "ingress_lossy_profile", "pool",
                        APP_BUFFER_POOL_TABLE_NAME, "ingress_lossy_pool");
        // Make sure the notification isn't drained
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 4);
        ASSERT_EQ(ts[0], "BUFFER_POOL_TABLE:ingress_lossy_pool|DEL");
        ASSERT_EQ(ts[1], "BUFFER_POOL_TABLE:ingress_lossy_pool|SET|type:ingress|size:1024000|mode:dynamic");
        ASSERT_EQ(ts[2], "BUFFER_PROFILE_TABLE:ingress_lossy_profile|DEL");
        ASSERT_EQ(ts[3], "BUFFER_PROFILE_TABLE:ingress_lossy_profile|SET|pool:ingress_lossy_pool|size:0|dynamic_th:0");
        ts.clear();

        // Remove ingress port profile list
        entries.push_back({"Ethernet0", "DEL", {}});
        consumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME));
        consumer->addToSync(entries);
        entries.clear();
        // Drain BUFFER_PORT_INGRESS_PROFILE_LIST_TABLE table
        _ut_stub_expected_profile_count = 0;
        auto sai_port_profile_list_remove_count = _ut_stub_port_profile_list_del_count;
        static_cast<Orch *>(gBufferOrch)->doTask();
        ASSERT_EQ(++sai_port_profile_list_remove_count, _ut_stub_port_profile_list_del_count);
        // Drain BUFFER_PROFILE_TABLE del operation
        static_cast<Orch *>(gBufferOrch)->doTask();
        // Drain BUFFER_POOL_TABLE del operation
        static_cast<Orch *>(gBufferOrch)->doTask();
        // Drain the rest create operations
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        // As an side-effect, all pending notifications should be drained
        ASSERT_TRUE(ts.empty());

        // To satisfy the coverage requirement
        bufferProfileListTable.set("Ethernet0",
                                   {
                                       {"profile_list", "ingress_no_exist_profile"}
                                   });
        gBufferOrch->addExistingData(&bufferProfileListTable);
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts[0], "BUFFER_PORT_INGRESS_PROFILE_LIST_TABLE:Ethernet0|SET|profile_list:ingress_no_exist_profile");
        ts.clear();

        _unhook_sai_port_api();
    }

    TEST_F(BufferOrchTest, BufferOrchTestCreateAndRemoveEgressProfileList)
    {
        _hook_sai_port_api();
        vector<string> ts;
        std::deque<KeyOpFieldsValuesTuple> entries;
        Table bufferPoolTable = Table(m_app_db.get(), APP_BUFFER_POOL_TABLE_NAME);
        Table bufferProfileTable = Table(m_app_db.get(), APP_BUFFER_PROFILE_TABLE_NAME);
        Table bufferProfileListTable = Table(m_app_db.get(), APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME);

        // To satisfy the coverage requirement
        bufferProfileListTable.set("Ethernet0",
                                   {
                                       {"profile_list", "egress_lossless_profile"}
                                   });

        gBufferOrch->addExistingData(&bufferProfileListTable);
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts[0], "BUFFER_PORT_EGRESS_PROFILE_LIST_TABLE:Ethernet0|SET|profile_list:egress_lossless_profile");
        ts.clear();

        bufferPoolTable.set("egress_lossless_pool",
                            {
                                {"size", "1024000"},
                                {"mode", "dynamic"},
                                {"type", "egress"}
                            });
        bufferProfileTable.set("egress_lossless_profile",
                               {
                                   {"pool", "egress_lossless_pool"},
                                   {"size", "0"},
                                   {"dynamic_th", "0"}
                               });

        gBufferOrch->addExistingData(&bufferPoolTable);
        gBufferOrch->addExistingData(&bufferProfileTable);

        auto sai_port_profile_list_create_count = _ut_stub_port_profile_list_add_count;
        _ut_stub_expected_profile_count = 1;
        _ut_stub_expected_profile_list_type = SAI_PORT_ATTR_QOS_EGRESS_BUFFER_PROFILE_LIST;
        static_cast<Orch *>(gBufferOrch)->doTask();
        ASSERT_EQ(++sai_port_profile_list_create_count, _ut_stub_port_profile_list_add_count);
        CheckDependency(APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME, "Ethernet0", "profile_list",
                        APP_BUFFER_PROFILE_TABLE_NAME, "egress_lossless_profile");

        // Remove egress port profile list
        entries.push_back({"Ethernet0", "DEL", {}});
        auto consumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME));
        consumer->addToSync(entries);
        entries.clear();
        // Drain BUFFER_PORT_EGRESS_PROFILE_LIST_TABLE table
        _ut_stub_expected_profile_count = 0;
        auto sai_port_profile_list_remove_count = _ut_stub_port_profile_list_del_count;
        static_cast<Orch *>(gBufferOrch)->doTask();
        ASSERT_EQ(++sai_port_profile_list_remove_count, _ut_stub_port_profile_list_del_count);

        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        _unhook_sai_port_api();
    }
}
