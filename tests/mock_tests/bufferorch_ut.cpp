#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "mock_response_publisher.h"

extern string gMySwitchType;

extern std::unique_ptr<MockResponsePublisher> gMockResponsePublisher;

extern bool gOrchUnhealthyCached;

namespace bufferorch_test
{
    using namespace std;

    sai_port_api_t ut_sai_port_api;
    sai_port_api_t *pold_sai_port_api;
    sai_buffer_api_t ut_sai_buffer_api;
    sai_buffer_api_t *pold_sai_buffer_api;
    sai_queue_api_t ut_sai_queue_api;
    sai_queue_api_t *pold_sai_queue_api;

    shared_ptr<swss::DBConnector> m_app_db;
    shared_ptr<swss::DBConnector> m_app_state_db;
    shared_ptr<swss::DBConnector> m_config_db;
    shared_ptr<swss::DBConnector> m_state_db;
    shared_ptr<swss::DBConnector> m_chassis_app_db;
    shared_ptr<swss::DBConnector> m_counters_db;

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

    uint32_t _ut_stub_set_pg_count;
    sai_status_t _ut_stub_sai_set_ingress_priority_group_attribute(
        _In_ sai_object_id_t ingress_priority_group_id,
        _In_ const sai_attribute_t *attr)
    {
        _ut_stub_set_pg_count++;
        return pold_sai_buffer_api->set_ingress_priority_group_attribute(ingress_priority_group_id, attr);
    }

    sai_uint64_t _ut_stub_buffer_profile_size;
    sai_uint64_t _ut_stub_buffer_profile_xon;
    sai_uint64_t _ut_stub_buffer_profile_xoff;
    bool _ut_stub_buffer_profile_sanity_check = false;
    sai_attr_id_t _ut_stub_buffer_profile_not_implemented_attr = SAI_BUFFER_PROFILE_ATTR_END;
    sai_attr_id_t _ut_stub_buffer_profile_failing_attr = SAI_BUFFER_PROFILE_ATTR_END;
    sai_status_t _ut_stub_buffer_profile_failing_status = SAI_STATUS_FAILURE;
    sai_status_t _ut_stub_sai_set_buffer_profile_attribute(
        _In_ sai_object_id_t buffer_profile_id,
        _In_ const sai_attribute_t *attr)
    {
        if (attr[0].id == _ut_stub_buffer_profile_not_implemented_attr)
        {
            return SAI_STATUS_ATTR_NOT_IMPLEMENTED_0;
        }
        if (attr[0].id == _ut_stub_buffer_profile_failing_attr)
        {
            return _ut_stub_buffer_profile_failing_status;
        }
        if (_ut_stub_buffer_profile_sanity_check)
        {
            if (SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE == attr[0].id)
            {
                if (attr[0].value.u64 < _ut_stub_buffer_profile_xon + _ut_stub_buffer_profile_xoff)
                {
                    return SAI_STATUS_INVALID_PARAMETER;
                }
                else
                {
                    _ut_stub_buffer_profile_size = attr[0].value.u64;
                }
            }
            if (SAI_BUFFER_PROFILE_ATTR_XOFF_TH == attr[0].id)
            {
                if (_ut_stub_buffer_profile_size < _ut_stub_buffer_profile_xon + attr[0].value.u64)
                {
                    return SAI_STATUS_INVALID_PARAMETER;
                }
                else
                {
                    _ut_stub_buffer_profile_xoff = attr[0].value.u64;
                }
            }
            if (SAI_BUFFER_PROFILE_ATTR_XON_TH == attr[0].id)
            {
                if (_ut_stub_buffer_profile_size < _ut_stub_buffer_profile_xoff + attr[0].value.u64)
                {
                    return SAI_STATUS_INVALID_PARAMETER;
                }
                else
                {
                    _ut_stub_buffer_profile_xon = attr[0].value.u64;
                }
            }
        }
        return pold_sai_buffer_api->set_buffer_profile_attribute(buffer_profile_id, attr);
    }

    uint32_t _ut_stub_set_queue_count;
    sai_status_t _ut_stub_sai_set_queue_attribute(
        _In_ sai_object_id_t queue_id,
        _In_ const sai_attribute_t *attr)
    {
        _ut_stub_set_queue_count++;
        return pold_sai_queue_api->set_queue_attribute(queue_id, attr);
    }

    sai_status_t _ut_stub_sai_set_ports_attribute(
        uint32_t object_count,
        const sai_object_id_t *object_id,
        const sai_attribute_t *attr_list,
        sai_bulk_op_error_mode_t mode,
        sai_status_t *object_statuses)
    {
        for (size_t i = 0; i < object_count; i++)
        {
            object_statuses[i] = _ut_stub_sai_set_port_attribute(object_id[i], attr_list + i);
        }
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_set_ingress_priority_groups_attribute(
        uint32_t object_count,
        const sai_object_id_t *object_id,
        const sai_attribute_t *attr_list,
        sai_bulk_op_error_mode_t mode,
        sai_status_t *object_statuses)
    {
        for (size_t i = 0; i < object_count; i++)
        {
            object_statuses[i] = _ut_stub_sai_set_ingress_priority_group_attribute(object_id[i], attr_list + i);
        }
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_set_queues_attribute(
        uint32_t object_count,
        const sai_object_id_t *object_id,
        const sai_attribute_t *attr_list,
        sai_bulk_op_error_mode_t mode,
        sai_status_t *object_statuses)
    {
        for (size_t i = 0; i < object_count; i++)
        {
            object_statuses[i] = _ut_stub_sai_set_queue_attribute(object_id[i], attr_list + i);
        }
        return SAI_STATUS_SUCCESS;
    }

    void _hook_sai_apis()
    {
        ut_sai_port_api = *sai_port_api;
        pold_sai_port_api = sai_port_api;
        ut_sai_port_api.set_port_attribute = _ut_stub_sai_set_port_attribute;
        ut_sai_port_api.set_ports_attribute = _ut_stub_sai_set_ports_attribute;
        sai_port_api = &ut_sai_port_api;

        ut_sai_buffer_api = *sai_buffer_api;
        pold_sai_buffer_api = sai_buffer_api;
        ut_sai_buffer_api.set_ingress_priority_group_attribute = _ut_stub_sai_set_ingress_priority_group_attribute;
        ut_sai_buffer_api.set_ingress_priority_groups_attribute = _ut_stub_sai_set_ingress_priority_groups_attribute;
        ut_sai_buffer_api.set_buffer_profile_attribute = _ut_stub_sai_set_buffer_profile_attribute;
        sai_buffer_api = &ut_sai_buffer_api;

        ut_sai_queue_api = *sai_queue_api;
        pold_sai_queue_api = sai_queue_api;
        ut_sai_queue_api.set_queue_attribute = _ut_stub_sai_set_queue_attribute;
        ut_sai_queue_api.set_queues_attribute = _ut_stub_sai_set_queues_attribute;
        sai_queue_api = &ut_sai_queue_api;
    }

    void _unhook_sai_apis()
    {
        sai_port_api = pold_sai_port_api;
        sai_buffer_api = pold_sai_buffer_api;
        sai_queue_api = pold_sai_queue_api;
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
            m_app_state_db = make_shared<swss::DBConnector>("APPL_STATE_DB", 0);
            m_counters_db = make_shared<swss::DBConnector>("COUNTERS_DB", 0);
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
                CFG_FLEX_COUNTER_TABLE_NAME,
                CFG_DEVICE_METADATA_TABLE_NAME
            };
            auto* flexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(flexCounterOrch);

            const vector<string> stel_tables = {
                CFG_HIGH_FREQUENCY_TELEMETRY_PROFILE_TABLE_NAME,
                CFG_HIGH_FREQUENCY_TELEMETRY_GROUP_TABLE_NAME
            };
            gHFTOrch = new HFTelOrch(m_config_db.get(), m_state_db.get(), stel_tables);

            ASSERT_EQ(gPortsOrch, nullptr);
            gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());

            ASSERT_EQ(gVrfOrch, nullptr);
            gVrfOrch = new VRFOrch(m_app_db.get(), APP_VRF_TABLE_NAME, m_state_db.get(), STATE_VRF_OBJECT_TABLE_NAME);
            vector<table_name_with_pri_t> intf_tables = {
                { APP_INTF_TABLE_NAME,  IntfsOrch::intfsorch_pri},
                { APP_SAG_TABLE_NAME,   IntfsOrch::intfsorch_pri}
            };

            ASSERT_EQ(gIntfsOrch, nullptr);
            gIntfsOrch = new IntfsOrch(m_app_db.get(), intf_tables, gVrfOrch, m_chassis_app_db.get());

            const int fdborch_pri = 20;

            vector<table_name_with_pri_t> app_fdb_tables = {
                { APP_FDB_TABLE_NAME,        FdbOrch::fdborch_pri},
                { APP_VXLAN_FDB_TABLE_NAME,  FdbOrch::fdborch_pri},
                { APP_MCLAG_FDB_TABLE_NAME,  fdborch_pri}
            };

            TableConnector stateDbFdb(m_state_db.get(), STATE_FDB_TABLE_NAME);
            TableConnector stateMclagDbFdb(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME);
            ASSERT_EQ(gFdbOrch, nullptr);
            gFdbOrch = new FdbOrch(m_app_db.get(), app_fdb_tables, stateDbFdb, stateMclagDbFdb, gPortsOrch,
                                   m_config_db.get());

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
            _ut_stub_buffer_profile_not_implemented_attr = SAI_BUFFER_PROFILE_ATTR_END;
            _ut_stub_buffer_profile_failing_attr = SAI_BUFFER_PROFILE_ATTR_END;
            _ut_stub_buffer_profile_failing_status = SAI_STATUS_FAILURE;
            auto buffer_maps = BufferOrch::m_buffer_type_maps;
            for (auto &i : buffer_maps)
            {
                i.second->clear();
            }

            // Clean up FlexCounterOrch
            auto* flexCounterOrch = gDirectory.get<FlexCounterOrch*>();
            if (flexCounterOrch)
            {
                delete flexCounterOrch;
            }

            gDirectory.m_values.clear();

            delete gCrmOrch;
            gCrmOrch = nullptr;

            delete gBufferOrch;
            gBufferOrch = nullptr;

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

    // Runs doTask with the swss logger redirected to stderr so that the emitted log can be asserted
    // on, and restores syslog output afterwards.
    static std::string drainBufferOrchCapturingLog()
    {
        swss::Logger::swssOutputNotify("", "STDERR");
        testing::internal::CaptureStderr();
        static_cast<Orch *>(gBufferOrch)->doTask();
        std::string logs = testing::internal::GetCapturedStderr();
        swss::Logger::swssOutputNotify("", "SYSLOG");
        return logs;
    }

    static sai_object_id_t profileOid(const string &name)
    {
        auto &profiles = *BufferOrch::m_buffer_type_maps[APP_BUFFER_PROFILE_TABLE_NAME];
        auto it = profiles.find(name);
        return (it == profiles.end()) ? SAI_NULL_OBJECT_ID : it->second.m_saiObjectId;
    }

    // Queues only the given entry: the mock DB keeps the rows of earlier tests
    static void addToOrch(Orch *orch, const string &table, const string &key, const vector<FieldValueTuple> &fvs)
    {
        std::deque<KeyOpFieldsValuesTuple> entries = {{key, "SET", fvs}};
        dynamic_cast<Consumer *>(orch->getExecutor(table))->addToSync(entries);
    }

    TEST_F(BufferOrchTest, BufferOrchTestSharedHeadroomPool)
    {
        gMockResponsePublisher = std::make_unique<MockResponsePublisher>();

        Table bufferPoolTable = Table(m_app_db.get(), APP_BUFFER_POOL_TABLE_NAME);
        Table bufferPoolStateTable = Table(m_app_state_db.get(), APP_BUFFER_POOL_TABLE_NAME);

        bufferPoolTable.set("ingress_lossless_pool",
                            {
                                {"xoff", "10240"}
                            });
        gBufferOrch->addExistingData(&bufferPoolTable);
        EXPECT_CALL(*gMockResponsePublisher, publish(APP_BUFFER_POOL_TABLE_NAME, "ingress_lossless_pool", std::vector<FieldValueTuple>{{"xoff", "10240"}}, ReturnCode(SAI_STATUS_SUCCESS), true)).Times(1);
        static_cast<Orch *>(gBufferOrch)->doTask();

        gMockResponsePublisher.reset();
    }

    TEST_F(BufferOrchTest, BufferOrchTestLosslessBufferProfilePublish)
    {
        gMockResponsePublisher = std::make_unique<MockResponsePublisher>();

        std::deque<KeyOpFieldsValuesTuple> entries;
        Table bufferProfileTable = Table(m_app_db.get(), APP_BUFFER_PROFILE_TABLE_NAME);

        // Create a lossless buffer profile with xoff field
        bufferProfileTable.set("test_lossless_profile",
                               {
                                   {"pool", "ingress_lossless_pool"},
                                   {"dynamic_th", "0"},
                                   {"size", "39936"},
                                   {"xon", "19456"},
                                   {"xoff", "20480"}
                               });
        gBufferOrch->addExistingData(&bufferProfileTable);
        // Creating a new profile should publish the result
        EXPECT_CALL(*gMockResponsePublisher, publish(
            APP_BUFFER_PROFILE_TABLE_NAME,
            "test_lossless_profile",
            std::vector<FieldValueTuple>{
                {"pool", "ingress_lossless_pool"},
                {"dynamic_th", "0"},
                {"size", "39936"},
                {"xon", "19456"},
                {"xoff", "20480"}
            },
            ReturnCode(SAI_STATUS_SUCCESS),
            true)).Times(1);
        static_cast<Orch *>(gBufferOrch)->doTask();

        // Delete the lossless buffer profile
        entries.push_back({"test_lossless_profile", "DEL", {}});
        auto consumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PROFILE_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();
        // Deleting a lossless profile should publish the result with empty fvs
        EXPECT_CALL(*gMockResponsePublisher, publish(
            APP_BUFFER_PROFILE_TABLE_NAME,
            "test_lossless_profile",
            std::vector<FieldValueTuple>{},
            ReturnCode(SAI_STATUS_SUCCESS),
            true)).Times(1);
        static_cast<Orch *>(gBufferOrch)->doTask();

        gMockResponsePublisher.reset();
    }

    TEST_F(BufferOrchTest, BufferOrchTestBufferPgReferencingObjRemoveThenAdd)
    {
        _hook_sai_apis();
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
        auto sai_pg_attr_set_count = _ut_stub_set_pg_count;
        static_cast<Orch *>(gBufferOrch)->doTask();
        // Make sure the dependency recovers
        CheckDependency(APP_BUFFER_PG_TABLE_NAME, "Ethernet0:0", "profile", APP_BUFFER_PROFILE_TABLE_NAME, "ingress_lossy_profile");
        ASSERT_EQ(++sai_pg_attr_set_count, _ut_stub_set_pg_count);

        // All items have been drained
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        // Try applying the same profile, which should not call SAI API
        entries.push_back({"Ethernet0:0", "SET",
                           {
                               {"profile", "ingress_lossy_profile"}
                           }});
        bufferPgConsumer->addToSync(entries);
        entries.clear();
        sai_pg_attr_set_count = _ut_stub_set_pg_count;
        static_cast<Orch *>(gBufferOrch)->doTask();
        ASSERT_EQ(sai_pg_attr_set_count, _ut_stub_set_pg_count);
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        _unhook_sai_apis();
    }

    TEST_F(BufferOrchTest, BufferOrchTestReferencingObjRemoveThenAdd)
    {
        _hook_sai_apis();
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

        // Remove and recreate the referenced profile on another pool, which needs a real recreate
        entries.push_back({"ingress_lossy_profile", "DEL", {}});
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossless_pool"},
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
        ASSERT_EQ(ts[1], "BUFFER_PROFILE_TABLE:ingress_lossy_profile|SET|pool:ingress_lossless_pool|size:0|dynamic_th:0");
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
        ASSERT_EQ(ts[3], "BUFFER_PROFILE_TABLE:ingress_lossy_profile|SET|pool:ingress_lossless_pool|size:0|dynamic_th:0");
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

        // Apply a buffer item only if it is changed
        _ut_stub_expected_profile_list_type = SAI_PORT_ATTR_QOS_INGRESS_BUFFER_PROFILE_LIST;
        _ut_stub_expected_profile_count = 1;
        entries.push_back({"Ethernet0", "SET",
                           {
                               {"profile_list", "ingress_lossy_profile"}
                           }});
        consumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME));
        consumer->addToSync(entries);
        sai_port_profile_list_create_count = _ut_stub_port_profile_list_add_count;
        // Drain BUFFER_PORT_INGRESS_PROFILE_LIST_TABLE table
        static_cast<Orch *>(gBufferOrch)->doTask();
        ASSERT_EQ(++sai_port_profile_list_create_count, _ut_stub_port_profile_list_add_count);
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        // Try applying it for the second time, which should not call SAI API
        consumer->addToSync(entries);
        static_cast<Orch *>(gBufferOrch)->doTask();
        ASSERT_EQ(sai_port_profile_list_create_count, _ut_stub_port_profile_list_add_count);
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
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

        _unhook_sai_apis();
    }

    TEST_F(BufferOrchTest, BufferOrchTestReferencedProfileRemoveThenAddSamePool)
    {
        _hook_sai_apis();
        vector<string> ts;
        std::deque<KeyOpFieldsValuesTuple> entries;
        Table bufferPgTable = Table(m_app_db.get(), APP_BUFFER_PG_TABLE_NAME);
        bufferPgTable.set("Ethernet0:0", {{"profile", "ingress_lossy_profile"}});
        gBufferOrch->addExistingData(&bufferPgTable);
        static_cast<Orch *>(gBufferOrch)->doTask();
        CheckDependency(APP_BUFFER_PG_TABLE_NAME, "Ethernet0:0", "profile", APP_BUFFER_PROFILE_TABLE_NAME, "ingress_lossy_profile");

        auto &profiles = *BufferOrch::m_buffer_type_maps[APP_BUFFER_PROFILE_TABLE_NAME];
        auto profile_oid = profiles["ingress_lossy_profile"].m_saiObjectId;
        auto sai_profile_attr = [&](sai_attr_id_t id) {
            sai_attribute_t attr;
            attr.id = id;
            EXPECT_EQ(SAI_STATUS_SUCCESS, sai_buffer_api->get_buffer_profile_attribute(profiles["ingress_lossy_profile"].m_saiObjectId, 1, &attr));
            return attr.value;
        };
        auto consumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PROFILE_TABLE_NAME));

        // Same pool and fields while the PG still uses the profile: the DEL is dropped and the SET applies in place
        entries.push_back({"ingress_lossy_profile", "DEL", {}});
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossy_pool"},
                               {"size", "1024"},
                               {"dynamic_th", "0"}
                           }});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ASSERT_EQ(profile_oid, profiles["ingress_lossy_profile"].m_saiObjectId);
        ASSERT_FALSE(profiles["ingress_lossy_profile"].m_pendingRemove);
        ASSERT_EQ(1024, sai_profile_attr(SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE).u64);
        CheckDependency(APP_BUFFER_PG_TABLE_NAME, "Ethernet0:0", "profile", APP_BUFFER_PROFILE_TABLE_NAME, "ingress_lossy_profile");

        // A later change to the profile applies
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossy_pool"},
                               {"size", "2048"},
                               {"dynamic_th", "0"}
                           }});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ASSERT_EQ(2048, sai_profile_attr(SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE).u64);

        // The same when the SET arrives after the DEL has already been retried
        entries.push_back({"ingress_lossy_profile", "DEL", {}});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 1);
        ASSERT_EQ(ts[0], "BUFFER_PROFILE_TABLE:ingress_lossy_profile|DEL");
        ASSERT_TRUE(profiles["ingress_lossy_profile"].m_pendingRemove);
        ts.clear();
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossy_pool"},
                               {"size", "4096"},
                               {"dynamic_th", "0"}
                           }});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ASSERT_EQ(profile_oid, profiles["ingress_lossy_profile"].m_saiObjectId);
        ASSERT_FALSE(profiles["ingress_lossy_profile"].m_pendingRemove);
        ASSERT_EQ(4096, sai_profile_attr(SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE).u64);

        // An in-place update the SAI refuses part way keeps the DEL and SET queued, and is not retried in place
        _ut_stub_buffer_profile_not_implemented_attr = SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH;
        entries.push_back({"ingress_lossy_profile", "DEL", {}});
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossy_pool"},
                               {"size", "8192"},
                               {"dynamic_th", "1"}
                           }});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 2);
        ASSERT_EQ(ts[0], "BUFFER_PROFILE_TABLE:ingress_lossy_profile|DEL");
        ASSERT_EQ(ts[1], "BUFFER_PROFILE_TABLE:ingress_lossy_profile|SET|pool:ingress_lossy_pool|size:8192|dynamic_th:1");
        ts.clear();
        ASSERT_TRUE(profiles["ingress_lossy_profile"].m_pendingRemove);
        ASSERT_EQ(profile_oid, profiles["ingress_lossy_profile"].m_saiObjectId);
        ASSERT_EQ(8192, sai_profile_attr(SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE).u64);
        ASSERT_EQ(0, sai_profile_attr(SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH).s8);
        _ut_stub_buffer_profile_not_implemented_attr = SAI_BUFFER_PROFILE_ATTR_END;
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 2);
        ts.clear();
        ASSERT_EQ(0, sai_profile_attr(SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH).s8);

        entries.push_back({"Ethernet0:0", "DEL", {}});
        auto pg_consumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PG_TABLE_NAME));
        pg_consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ASSERT_NE(profile_oid, profiles["ingress_lossy_profile"].m_saiObjectId);
        ASSERT_EQ(8192, sai_profile_attr(SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE).u64);
        ASSERT_EQ(1, sai_profile_attr(SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH).s8);
        profile_oid = profiles["ingress_lossy_profile"].m_saiObjectId;

        // The recreated profile is updated in place again, even with the SET that was refused before the recreate
        bufferPgTable.set("Ethernet0:0", {{"profile", "ingress_lossy_profile"}});
        gBufferOrch->addExistingData(&bufferPgTable);
        static_cast<Orch *>(gBufferOrch)->doTask();
        CheckDependency(APP_BUFFER_PG_TABLE_NAME, "Ethernet0:0", "profile", APP_BUFFER_PROFILE_TABLE_NAME, "ingress_lossy_profile");
        entries.push_back({"ingress_lossy_profile", "DEL", {}});
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossy_pool"},
                               {"size", "8192"},
                               {"dynamic_th", "1"}
                           }});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ASSERT_EQ(profile_oid, profiles["ingress_lossy_profile"].m_saiObjectId);

        // Other fields (here a threshold mode change) need a recreate, so the DEL waits for the PG to move
        entries.push_back({"ingress_lossy_profile", "DEL", {}});
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossy_pool"},
                               {"size", "0"},
                               {"static_th", "4096"}
                           }});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 2);
        ASSERT_EQ(ts[0], "BUFFER_PROFILE_TABLE:ingress_lossy_profile|DEL");
        ASSERT_EQ(ts[1], "BUFFER_PROFILE_TABLE:ingress_lossy_profile|SET|pool:ingress_lossy_pool|size:0|static_th:4096");
        ts.clear();

        entries.push_back({"Ethernet0:0", "DEL", {}});
        pg_consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ASSERT_NE(profile_oid, profiles["ingress_lossy_profile"].m_saiObjectId);
        ASSERT_EQ(SAI_BUFFER_PROFILE_THRESHOLD_MODE_STATIC, sai_profile_attr(SAI_BUFFER_PROFILE_ATTR_THRESHOLD_MODE).s32);

        _unhook_sai_apis();
    }

    // Reads one attribute of a buffer profile back from the SAI
    static sai_attribute_value_t saiProfileAttr(const string &name, sai_attr_id_t id)
    {
        sai_attribute_t attr;
        attr.id = id;
        auto oid = (*BufferOrch::m_buffer_type_maps[APP_BUFFER_PROFILE_TABLE_NAME])[name].m_saiObjectId;
        EXPECT_EQ(SAI_STATUS_SUCCESS, sai_buffer_api->get_buffer_profile_attribute(oid, 1, &attr));
        return attr.value;
    }

    TEST_F(BufferOrchTest, BufferOrchTestReferencedProfileInPlaceSetFails)
    {
        _hook_sai_apis();
        vector<string> ts;
        std::deque<KeyOpFieldsValuesTuple> entries;
        // Queued directly rather than from the DB, which keeps the rows of earlier tests
        entries.push_back({"Ethernet0:0", "SET", {{"profile", "ingress_lossy_profile"}}});
        dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PG_TABLE_NAME))->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        auto &profiles = *BufferOrch::m_buffer_type_maps[APP_BUFFER_PROFILE_TABLE_NAME];
        auto profile_oid = profiles["ingress_lossy_profile"].m_saiObjectId;
        auto consumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PROFILE_TABLE_NAME));

        // A set that needs a retry: the attributes after the failed one are applied, and the DEL and SET stay queued
        _ut_stub_buffer_profile_failing_attr = SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE;
        _ut_stub_buffer_profile_failing_status = SAI_STATUS_INSUFFICIENT_RESOURCES;
        entries.push_back({"ingress_lossy_profile", "DEL", {}});
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossy_pool"},
                               {"size", "1024"},
                               {"dynamic_th", "1"}
                           }});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 2);
        ASSERT_EQ(ts[0], "BUFFER_PROFILE_TABLE:ingress_lossy_profile|DEL");
        ASSERT_EQ(ts[1], "BUFFER_PROFILE_TABLE:ingress_lossy_profile|SET|pool:ingress_lossy_pool|size:1024|dynamic_th:1");
        ts.clear();
        ASSERT_TRUE(profiles["ingress_lossy_profile"].m_pendingRemove);
        ASSERT_EQ(profile_oid, profiles["ingress_lossy_profile"].m_saiObjectId);
        ASSERT_EQ(0, saiProfileAttr("ingress_lossy_profile", SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE).u64);
        ASSERT_EQ(1, saiProfileAttr("ingress_lossy_profile", SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH).s8);

        // Once the SAI accepts it, the next pass applies the same SET in place
        _ut_stub_buffer_profile_failing_attr = SAI_BUFFER_PROFILE_ATTR_END;
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ASSERT_FALSE(profiles["ingress_lossy_profile"].m_pendingRemove);
        ASSERT_EQ(profile_oid, profiles["ingress_lossy_profile"].m_saiObjectId);
        ASSERT_EQ(1024, saiProfileAttr("ingress_lossy_profile", SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE).u64);

        // A set that fails outright: the same, and the same SET is not tried in place again once the SAI accepts it.
        // The refusal falls back to the delete and recreate, so it is not reported as a SAI failure.
        gOrchUnhealthyCached = false;
        _ut_stub_buffer_profile_failing_attr = SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE;
        _ut_stub_buffer_profile_failing_status = SAI_STATUS_FAILURE;
        entries.push_back({"ingress_lossy_profile", "DEL", {}});
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossy_pool"},
                               {"size", "2048"},
                               {"dynamic_th", "0"}
                           }});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 2);
        ts.clear();
        ASSERT_FALSE(gOrchUnhealthyCached);
        ASSERT_TRUE(profiles["ingress_lossy_profile"].m_pendingRemove);
        ASSERT_EQ(1024, saiProfileAttr("ingress_lossy_profile", SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE).u64);
        ASSERT_EQ(0, saiProfileAttr("ingress_lossy_profile", SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH).s8);
        _ut_stub_buffer_profile_failing_attr = SAI_BUFFER_PROFILE_ATTR_END;
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 2);
        ts.clear();
        ASSERT_EQ(1024, saiProfileAttr("ingress_lossy_profile", SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE).u64);

        // A different SET is tried in place
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossy_pool"},
                               {"size", "2048"},
                               {"dynamic_th", "1"}
                           }});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ASSERT_FALSE(profiles["ingress_lossy_profile"].m_pendingRemove);
        ASSERT_EQ(profile_oid, profiles["ingress_lossy_profile"].m_saiObjectId);
        ASSERT_EQ(2048, saiProfileAttr("ingress_lossy_profile", SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE).u64);
        ASSERT_EQ(1, saiProfileAttr("ingress_lossy_profile", SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH).s8);

        // After a failed set, the profile is recreated once the PG moves off
        _ut_stub_buffer_profile_failing_attr = SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE;
        entries.push_back({"ingress_lossy_profile", "DEL", {}});
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossy_pool"},
                               {"size", "4096"},
                               {"dynamic_th", "0"}
                           }});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 2);
        ts.clear();

        _ut_stub_buffer_profile_failing_attr = SAI_BUFFER_PROFILE_ATTR_END;
        entries.push_back({"Ethernet0:0", "DEL", {}});
        dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PG_TABLE_NAME))->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ASSERT_NE(profile_oid, profiles["ingress_lossy_profile"].m_saiObjectId);
        ASSERT_EQ(4096, saiProfileAttr("ingress_lossy_profile", SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE).u64);

        _unhook_sai_apis();
    }

    TEST_F(BufferOrchTest, BufferOrchTestProfileListReferencedProfileRemoveThenAdd)
    {
        _hook_sai_apis();
        vector<string> ts;
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "SET", {{"profile_list", "ingress_lossy_profile,ingress_lossless_profile"}}});
        dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME))->addToSync(entries);
        entries.clear();
        _ut_stub_expected_profile_count = 2;
        _ut_stub_expected_profile_list_type = SAI_PORT_ATTR_QOS_INGRESS_BUFFER_PROFILE_LIST;
        static_cast<Orch *>(gBufferOrch)->doTask();
        CheckDependency(APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, "Ethernet0", "profile_list",
                        APP_BUFFER_PROFILE_TABLE_NAME, "ingress_lossy_profile,ingress_lossless_profile");
        auto &profiles = *BufferOrch::m_buffer_type_maps[APP_BUFFER_PROFILE_TABLE_NAME];
        auto &pools = *BufferOrch::m_buffer_type_maps[APP_BUFFER_POOL_TABLE_NAME];
        auto profile_oid = profiles["ingress_lossy_profile"].m_saiObjectId;
        auto pool_oid = pools["ingress_lossy_pool"].m_saiObjectId;
        auto profile_consumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PROFILE_TABLE_NAME));
        auto pool_consumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_POOL_TABLE_NAME));

        // A profile in a port's profile list is updated in place, and the port's list is not set again
        auto list_set_count = _ut_stub_port_profile_list_add_count;
        entries.push_back({"ingress_lossy_profile", "DEL", {}});
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossy_pool"},
                               {"size", "1024"},
                               {"dynamic_th", "0"}
                           }});
        profile_consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ASSERT_EQ(profile_oid, profiles["ingress_lossy_profile"].m_saiObjectId);
        ASSERT_EQ(1024, saiProfileAttr("ingress_lossy_profile", SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE).u64);
        ASSERT_EQ(list_set_count, _ut_stub_port_profile_list_add_count);
        CheckDependency(APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, "Ethernet0", "profile_list",
                        APP_BUFFER_PROFILE_TABLE_NAME, "ingress_lossy_profile,ingress_lossless_profile");

        // While the profile's pool is pending remove, a DEL and SET of the profile are not applied in place,
        // and the pool is not resolved for the check
        entries.push_back({"ingress_lossy_pool", "DEL", {}});
        entries.push_back({"ingress_lossy_pool", "SET", {{"type", "ingress"}, {"size", "1024000"}, {"mode", "dynamic"}}});
        pool_consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        ASSERT_TRUE(pools["ingress_lossy_pool"].m_pendingRemove);
        entries.push_back({"ingress_lossy_profile", "DEL", {}});
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossy_pool"},
                               {"size", "2048"},
                               {"dynamic_th", "0"}
                           }});
        profile_consumer->addToSync(entries);
        entries.clear();
        auto logs = drainBufferOrchCapturingLog();
        EXPECT_NE(logs.find("Can't remove object ingress_lossy_profile"), std::string::npos);
        EXPECT_EQ(logs.find("contains a pending removed object ingress_lossy_pool"), std::string::npos);
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 4);
        ts.clear();
        ASSERT_TRUE(profiles["ingress_lossy_profile"].m_pendingRemove);
        ASSERT_EQ(1024, saiProfileAttr("ingress_lossy_profile", SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE).u64);

        // Once the port's list is removed, the profile and the pool are deleted and recreated
        entries.push_back({"Ethernet0", "DEL", {}});
        dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME))->addToSync(entries);
        entries.clear();
        _ut_stub_expected_profile_count = 0;
        for (int i = 0; i < 4; i++)
        {
            static_cast<Orch *>(gBufferOrch)->doTask();
        }
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ASSERT_NE(pool_oid, pools["ingress_lossy_pool"].m_saiObjectId);
        ASSERT_NE(profile_oid, profiles["ingress_lossy_profile"].m_saiObjectId);
        ASSERT_EQ(2048, saiProfileAttr("ingress_lossy_profile", SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE).u64);

        _unhook_sai_apis();
    }

    // Behaviour change: once a profile has been updated in place, its DEL is gone, so a later DEL and SET of its
    // pool waits until the profile itself is deleted, as for a profile that was never deleted
    TEST_F(BufferOrchTest, BufferOrchTestPoolRemoveThenAddAfterProfileUpdatedInPlace)
    {
        _hook_sai_apis();
        vector<string> ts;
        std::deque<KeyOpFieldsValuesTuple> entries;
        // Queued directly rather than from the DB, which keeps the rows of earlier tests
        entries.push_back({"Ethernet0:0", "SET", {{"profile", "ingress_lossy_profile"}}});
        dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PG_TABLE_NAME))->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        auto &profiles = *BufferOrch::m_buffer_type_maps[APP_BUFFER_PROFILE_TABLE_NAME];
        auto &pools = *BufferOrch::m_buffer_type_maps[APP_BUFFER_POOL_TABLE_NAME];
        auto profile_oid = profiles["ingress_lossy_profile"].m_saiObjectId;
        auto pool_oid = pools["ingress_lossy_pool"].m_saiObjectId;
        auto profile_consumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PROFILE_TABLE_NAME));

        entries.push_back({"ingress_lossy_profile", "DEL", {}});
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossy_pool"},
                               {"size", "1024"},
                               {"dynamic_th", "0"}
                           }});
        profile_consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        // The pool's DEL and SET arrive in a later batch, then the PG moves off
        entries.push_back({"ingress_lossy_pool", "DEL", {}});
        entries.push_back({"ingress_lossy_pool", "SET", {{"type", "ingress"}, {"size", "1024000"}, {"mode", "dynamic"}}});
        dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_POOL_TABLE_NAME))->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        entries.push_back({"Ethernet0:0", "DEL", {}});
        dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PG_TABLE_NAME))->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->doTask();

        // The profile stays and keeps the pool referenced, so the pool's DEL and SET stay queued
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 2);
        ASSERT_EQ(ts[0], "BUFFER_POOL_TABLE:ingress_lossy_pool|DEL");
        ASSERT_EQ(ts[1], "BUFFER_POOL_TABLE:ingress_lossy_pool|SET|type:ingress|size:1024000|mode:dynamic");
        ts.clear();
        ASSERT_TRUE(pools["ingress_lossy_pool"].m_pendingRemove);
        ASSERT_EQ(pool_oid, pools["ingress_lossy_pool"].m_saiObjectId);
        ASSERT_EQ(profile_oid, profiles["ingress_lossy_profile"].m_saiObjectId);
        ASSERT_FALSE(profiles["ingress_lossy_profile"].m_pendingRemove);
        CheckDependency(APP_BUFFER_PROFILE_TABLE_NAME, "ingress_lossy_profile", "pool",
                        APP_BUFFER_POOL_TABLE_NAME, "ingress_lossy_pool");

        // Deleting the profile lets the pool be recreated, and the profile is then created on it
        entries.push_back({"ingress_lossy_profile", "DEL", {}});
        entries.push_back({"ingress_lossy_profile", "SET",
                           {
                               {"pool", "ingress_lossy_pool"},
                               {"size", "1024"},
                               {"dynamic_th", "0"}
                           }});
        profile_consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ASSERT_NE(pool_oid, pools["ingress_lossy_pool"].m_saiObjectId);
        ASSERT_NE(profile_oid, profiles["ingress_lossy_profile"].m_saiObjectId);
        ASSERT_FALSE(pools["ingress_lossy_pool"].m_pendingRemove);

        _unhook_sai_apis();
    }

    TEST_F(BufferOrchTest, BufferOrchTestCreateAndRemoveEgressProfileList)
    {
        _hook_sai_apis();
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

        // Try applying it for the second time, which should not call SAI API
        entries.push_back({"Ethernet0", "SET",
                           {
                               {"profile_list", "egress_lossless_profile"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME));
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        ASSERT_EQ(sai_port_profile_list_create_count, _ut_stub_port_profile_list_add_count);
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        // Remove egress port profile list
        entries.push_back({"Ethernet0", "DEL", {}});
        consumer->addToSync(entries);
        entries.clear();
        // Drain BUFFER_PORT_EGRESS_PROFILE_LIST_TABLE table
        _ut_stub_expected_profile_count = 0;
        auto sai_port_profile_list_remove_count = _ut_stub_port_profile_list_del_count;
        static_cast<Orch *>(gBufferOrch)->doTask();
        ASSERT_EQ(++sai_port_profile_list_remove_count, _ut_stub_port_profile_list_del_count);

        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        // Queue table
        entries.push_back({"Ethernet0:0", "SET",
                           {
                               {"profile", "egress_lossless_profile"}
                           }});
        consumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_QUEUE_TABLE_NAME));
        consumer->addToSync(entries);
        auto sai_queue_set_count = _ut_stub_set_queue_count;
        static_cast<Orch *>(gBufferOrch)->doTask();
        ASSERT_EQ(++sai_queue_set_count, _ut_stub_set_queue_count);
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        consumer->addToSync(entries);
        static_cast<Orch *>(gBufferOrch)->doTask();
         ASSERT_EQ(sai_queue_set_count, _ut_stub_set_queue_count);
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        _unhook_sai_apis();
    }

    TEST_F(BufferOrchTest, BufferOrchTestSetBufferProfile)
    {
        _hook_sai_apis();
        vector<string> ts;
        std::deque<KeyOpFieldsValuesTuple> entries;
        Table bufferPoolTable = Table(m_app_db.get(), APP_BUFFER_POOL_TABLE_NAME);
        Table bufferProfileTable = Table(m_app_db.get(), APP_BUFFER_PROFILE_TABLE_NAME);

        bufferPoolTable.set("ingress_lossless_pool",
                            {
                                {"size", "1024000"},
                                {"mode", "dynamic"},
                                {"type", "ingress"}
                            });
        bufferProfileTable.set("test_lossless_profile",
                               {
                                   {"pool", "ingress_lossless_pool"},
                                   {"dynamic_th", "0"},
                                   {"size", "39936"},
                                   {"xon", "19456"},
                                   {"xoff", "20480"}
                               });

        gBufferOrch->addExistingData(&bufferPoolTable);
        gBufferOrch->addExistingData(&bufferProfileTable);

        static_cast<Orch *>(gBufferOrch)->doTask();

        _ut_stub_buffer_profile_size = 39936;
        _ut_stub_buffer_profile_xon = 19456;
        _ut_stub_buffer_profile_xoff = 20480;
        _ut_stub_buffer_profile_sanity_check = true;

        // Decrease xoff, size
        entries.push_back({"test_lossless_profile", "SET",
                           {
                               {"size", "29936"},
                               {"xon", "19456"},
                               {"xoff", "10480"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PROFILE_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        ASSERT_EQ(_ut_stub_buffer_profile_size, 29936);
        ASSERT_EQ(_ut_stub_buffer_profile_xoff, 10480);
        ASSERT_EQ(_ut_stub_buffer_profile_xon, 19456);
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        // Increase xoff, size
        entries.push_back({"test_lossless_profile", "SET",
                           {
                               {"xoff", "20480"},
                               {"size", "39936"},
                               {"xon", "19456"}
                           }});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gBufferOrch)->doTask();
        ASSERT_EQ(_ut_stub_buffer_profile_size, 39936);
        ASSERT_EQ(_ut_stub_buffer_profile_xoff, 20480);
        ASSERT_EQ(_ut_stub_buffer_profile_xon, 19456);
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        _ut_stub_buffer_profile_sanity_check = false;
        _unhook_sai_apis();
    }

    TEST_F(BufferOrchTest, BufferOrchTestCreateOnlyConfigDbBuffersDynamicUpdate)
    {
        // Get FlexCounterOrch from directory
        auto* flexCounterOrch = gDirectory.get<FlexCounterOrch*>();
        ASSERT_NE(flexCounterOrch, nullptr);

        // Test Phase 1: Initial State Verification
        // Set up initial configuration with create_only_config_db_buffers = false and verify it
        ASSERT_FALSE(flexCounterOrch->isCreateOnlyConfigDbBuffers());

        // Test Phase 2: Enable Flex Counter and Verify All Counters Added
        // Add configuration to enable flex counter for watermark
        Table flexCounterTable = Table(m_config_db.get(), CFG_FLEX_COUNTER_TABLE_NAME);
        flexCounterTable.set("PG_WATERMARK", {
            {"FLEX_COUNTER_STATUS", "enable"},
            {"POLL_INTERVAL", "1000"}
        });
        flexCounterTable.set("QUEUE_WATERMARK", {
            {"FLEX_COUNTER_STATUS", "enable"},
            {"POLL_INTERVAL", "1000"}
        });
        flexCounterOrch->addExistingData(&flexCounterTable);
        static_cast<Orch *>(flexCounterOrch)->doTask();

        // Verify all counters are added to counter database (because create_only_config_db_buffers = false)
        Table countersPgTable = Table(m_counters_db.get(), COUNTERS_PG_NAME_MAP);
        Table countersQueueTable = Table(m_counters_db.get(), COUNTERS_QUEUE_NAME_MAP);

        // Check that counter database has entries for all PGs and queues
        // These tables use empty string "" as key and store all entries as fields
        std::vector<FieldValueTuple> pgFields;
        std::vector<FieldValueTuple> queueFields;
        countersPgTable.get("", pgFields);
        countersQueueTable.get("", queueFields);
        ASSERT_GT(pgFields.size(), 0);
        ASSERT_GT(queueFields.size(), 0);

        // Test Phase 3: Add Individual Buffer Configurations (Should NOT be added to map)
        // Clear existing counter entries
        countersPgTable.del("");
        countersQueueTable.del("");

        // Add buffer PG and buffer queue configuration
        Table bufferPgTable = Table(m_app_db.get(), APP_BUFFER_PG_TABLE_NAME);
        Table bufferQueueTable = Table(m_app_db.get(), APP_BUFFER_QUEUE_TABLE_NAME);

        bufferPgTable.set("Ethernet0:0", {
            {"profile", "ingress_lossy_profile"}
        });
        bufferQueueTable.set("Ethernet0:0", {
            {"profile", "ingress_lossy_profile"}
        });

        gBufferOrch->addExistingData(&bufferPgTable);
        gBufferOrch->addExistingData(&bufferQueueTable);
        static_cast<Orch *>(gBufferOrch)->doTask();

        // Verify they are NOT added to the counter database (because create_only_config_db_buffers = false)
        // Individual buffer configurations should not create counter database entries again
        pgFields.clear();
        queueFields.clear();
        countersPgTable.get("", pgFields);
        countersQueueTable.get("", queueFields);
        ASSERT_EQ(pgFields.size(), 0);
        ASSERT_EQ(queueFields.size(), 0);

        // Test Phase 4: Dynamic Configuration Update to true
        // Update DEVICE_METADATA table
        Table deviceMetadataTable = Table(m_config_db.get(), CFG_DEVICE_METADATA_TABLE_NAME);
        deviceMetadataTable.set("localhost", {
            {"create_only_config_db_buffers", "true"}
        });
        flexCounterOrch->addExistingData(&deviceMetadataTable);
        static_cast<Orch *>(flexCounterOrch)->doTask();

        // Verify configuration change
        ASSERT_TRUE(flexCounterOrch->isCreateOnlyConfigDbBuffers());

        // Test Phase 5: Add Individual Buffer Configurations (Should be added to map)
        // Add buffer PG and buffer queue configuration for different objects
        bufferPgTable.set("Ethernet0:1", {
            {"profile", "ingress_lossy_profile"}
        });
        bufferQueueTable.set("Ethernet0:1", {
            {"profile", "ingress_lossy_profile"}
        });

        gBufferOrch->addExistingData(&bufferPgTable);
        gBufferOrch->addExistingData(&bufferQueueTable);
        static_cast<Orch *>(gBufferOrch)->doTask();

        // Verify they ARE added to the counter database (because create_only_config_db_buffers = true)
        // Individual buffer configurations should create counter database entries
        pgFields.clear();
        queueFields.clear();
        countersPgTable.get("", pgFields);
        countersQueueTable.get("", queueFields);
        ASSERT_EQ(pgFields.size(), 1);
        ASSERT_EQ(queueFields.size(), 1);

        // Verify the specific entries exist
        {
            std::string value;
            bool found = countersPgTable.hget("", "Ethernet0:1", value);
            ASSERT_TRUE(found);
            ASSERT_FALSE(value.empty());
        }
        {
            std::string value;
            bool found = countersQueueTable.hget("", "Ethernet0:1", value);
            ASSERT_TRUE(found);
            ASSERT_FALSE(value.empty());
        }
    }

    TEST_F(BufferOrchTest, BufferOrchTestLosslessProfileUpdatedInPlacePublish)
    {
        _hook_sai_apis();
        gMockResponsePublisher = std::make_unique<MockResponsePublisher>();
        std::vector<FieldValueTuple> fvs = {
            {"pool", "ingress_lossless_pool"},
            {"dynamic_th", "0"},
            {"size", "39936"},
            {"xon", "19456"},
            {"xoff", "20480"}
        };
        EXPECT_CALL(*gMockResponsePublisher, publish(APP_BUFFER_PROFILE_TABLE_NAME, "test_lossless_profile", fvs,
                                                     ReturnCode(SAI_STATUS_SUCCESS), true)).Times(1);
        addToOrch(gBufferOrch, APP_BUFFER_PROFILE_TABLE_NAME, "test_lossless_profile", fvs);
        addToOrch(gBufferOrch, APP_BUFFER_PG_TABLE_NAME, "Ethernet0:3", {{"profile", "test_lossless_profile"}});
        static_cast<Orch *>(gBufferOrch)->doTask();
        auto profile_oid = profileOid("test_lossless_profile");
        testing::Mock::VerifyAndClearExpectations(gMockResponsePublisher.get());

        // The in-place update publishes the SET's fields, and no removal is published
        std::vector<FieldValueTuple> new_fvs = {
            {"pool", "ingress_lossless_pool"},
            {"dynamic_th", "0"},
            {"size", "49152"},
            {"xon", "19456"},
            {"xoff", "29696"}
        };
        EXPECT_CALL(*gMockResponsePublisher, publish(APP_BUFFER_PROFILE_TABLE_NAME, "test_lossless_profile", new_fvs,
                                                     ReturnCode(SAI_STATUS_SUCCESS), true)).Times(1);
        EXPECT_CALL(*gMockResponsePublisher, publish(APP_BUFFER_PROFILE_TABLE_NAME, "test_lossless_profile",
                                                     std::vector<FieldValueTuple>{},
                                                     ReturnCode(SAI_STATUS_SUCCESS), true)).Times(0);
        std::deque<KeyOpFieldsValuesTuple> entries = {
            {"test_lossless_profile", "DEL", {}},
            {"test_lossless_profile", "SET", new_fvs}};
        dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PROFILE_TABLE_NAME))->addToSync(entries);
        static_cast<Orch *>(gBufferOrch)->doTask();

        vector<string> ts;
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ASSERT_EQ(profile_oid, profileOid("test_lossless_profile"));
        ASSERT_EQ(49152, saiProfileAttr("test_lossless_profile", SAI_BUFFER_PROFILE_ATTR_BUFFER_SIZE).u64);

        gMockResponsePublisher.reset();
        _unhook_sai_apis();
    }

    // The shape APPL_DB delivers: the profile DEL is popped and retried while the PG still uses the profile,
    // then the SET of the same profile and the coalesced PG SET (same profile) arrive in a later pop.
    // On master all three stay queued; with the change they clear and the profile keeps its OID.
    TEST_F(BufferOrchTest, BufferOrchTestReferencedProfileRemoveThenAddAcrossPops)
    {
        _hook_sai_apis();
        std::vector<FieldValueTuple> fvs = {
            {"pool", "ingress_lossless_pool"}, {"dynamic_th", "0"}, {"size", "39936"}, {"xon", "19456"}, {"xoff", "20480"}};
        addToOrch(gBufferOrch, APP_BUFFER_PROFILE_TABLE_NAME, "pops_lossless_profile", fvs);
        addToOrch(gBufferOrch, APP_BUFFER_PG_TABLE_NAME, "Ethernet0:3", {{"profile", "pops_lossless_profile"}});
        static_cast<Orch *>(gBufferOrch)->doTask();
        auto oid = profileOid("pops_lossless_profile");
        ASSERT_NE(oid, SAI_NULL_OBJECT_ID);
        std::deque<KeyOpFieldsValuesTuple> del = {{"pops_lossless_profile", "DEL", {}}};
        dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PROFILE_TABLE_NAME))->addToSync(del);
        static_cast<Orch *>(gBufferOrch)->doTask();
        addToOrch(gBufferOrch, APP_BUFFER_PROFILE_TABLE_NAME, "pops_lossless_profile", fvs);
        addToOrch(gBufferOrch, APP_BUFFER_PG_TABLE_NAME, "Ethernet0:3", {{"profile", "pops_lossless_profile"}});
        for (int i = 0; i < 3; i++)
        {
            static_cast<Orch *>(gBufferOrch)->doTask();
        }
        vector<string> ts;
        static_cast<Orch *>(gBufferOrch)->dumpPendingTasks(ts);
        EXPECT_TRUE(ts.empty());
        EXPECT_EQ(oid, profileOid("pops_lossless_profile"));
        _unhook_sai_apis();
    }

    // A value that does not parse makes the in-place SET throw; the profile stays as the queued DEL
    // expects, and the in-place state that keeps a refusal from being reported as a SAI failure is reset
    TEST_F(BufferOrchTest, BufferOrchTestReferencedProfileInPlaceSetThrows)
    {
        _hook_sai_apis();
        std::vector<FieldValueTuple> fvs = {
            {"pool", "ingress_lossless_pool"}, {"dynamic_th", "0"}, {"size", "39936"}, {"xon", "19456"}, {"xoff", "20480"}};
        addToOrch(gBufferOrch, APP_BUFFER_PROFILE_TABLE_NAME, "throw_lossless_profile", fvs);
        addToOrch(gBufferOrch, APP_BUFFER_PG_TABLE_NAME, "Ethernet0:3", {{"profile", "throw_lossless_profile"}});
        static_cast<Orch *>(gBufferOrch)->doTask();
        ASSERT_NE(profileOid("throw_lossless_profile"), SAI_NULL_OBJECT_ID);

        std::vector<FieldValueTuple> bad = fvs;
        for (auto &fv : bad)
        {
            if (fvField(fv) == "size")
            {
                fvValue(fv) = "abc";
            }
        }
        std::deque<KeyOpFieldsValuesTuple> entries = {{"throw_lossless_profile", "DEL", {}},
                                                      {"throw_lossless_profile", "SET", bad}};
        dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PROFILE_TABLE_NAME))->addToSync(entries);
        static_cast<Orch *>(gBufferOrch)->doTask();

        auto &profiles = *BufferOrch::m_buffer_type_maps[APP_BUFFER_PROFILE_TABLE_NAME];
        EXPECT_TRUE(profiles["throw_lossless_profile"].m_pendingRemove);
        EXPECT_FALSE(gBufferOrch->m_profileSetInPlace);
        _unhook_sai_apis();
    }

    // After the in-place SET throws once, the profile keys queued after it must still be processed:
    // the failed SET is not tried in place again, so the next pass does not throw.
    TEST_F(BufferOrchTest, BufferOrchTestInPlaceSetThrowDoesNotBlockLaterProfiles)
    {
        _hook_sai_apis();
        std::vector<FieldValueTuple> fvs = {
            {"pool", "ingress_lossless_pool"}, {"dynamic_th", "0"}, {"size", "39936"}, {"xon", "19456"}, {"xoff", "20480"}};
        addToOrch(gBufferOrch, APP_BUFFER_PROFILE_TABLE_NAME, "aa_lossless_profile", fvs);
        addToOrch(gBufferOrch, APP_BUFFER_PG_TABLE_NAME, "Ethernet0:3", {{"profile", "aa_lossless_profile"}});
        static_cast<Orch *>(gBufferOrch)->doTask();
        std::vector<FieldValueTuple> bad = fvs;
        for (auto &fv : bad)
        {
            if (fvField(fv) == "size")
            {
                fvValue(fv) = "abc";
            }
        }
        std::deque<KeyOpFieldsValuesTuple> entries = {{"aa_lossless_profile", "DEL", {}},
                                                      {"aa_lossless_profile", "SET", bad}};
        dynamic_cast<Consumer *>(gBufferOrch->getExecutor(APP_BUFFER_PROFILE_TABLE_NAME))->addToSync(entries);
        static_cast<Orch *>(gBufferOrch)->doTask();
        // Sorts after the throwing key in m_toSync
        addToOrch(gBufferOrch, APP_BUFFER_PROFILE_TABLE_NAME, "zz_profile",
                  {{"pool", "ingress_lossy_pool"}, {"size", "0"}, {"dynamic_th", "0"}});
        for (int i = 0; i < 3; i++)
        {
            static_cast<Orch *>(gBufferOrch)->doTask();
        }
        EXPECT_NE(profileOid("zz_profile"), SAI_NULL_OBJECT_ID);
        _unhook_sai_apis();
    }
}
