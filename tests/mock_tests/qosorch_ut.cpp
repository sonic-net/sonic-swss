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


namespace qosorch_test
{
    using namespace std;

    shared_ptr<swss::DBConnector> m_app_db;
    shared_ptr<swss::DBConnector> m_config_db;
    shared_ptr<swss::DBConnector> m_state_db;
    shared_ptr<swss::DBConnector> m_chassis_app_db;

    int sai_remove_qos_map_count;
    int sai_remove_wred_profile_count;
    int sai_remove_scheduler_count;
    sai_object_id_t switch_dscp_to_tc_map_id;

    sai_remove_scheduler_fn old_remove_scheduler;
    sai_scheduler_api_t ut_sai_scheduler_api, *pold_sai_scheduler_api;
    sai_remove_wred_fn old_remove_wred;
    sai_wred_api_t ut_sai_wred_api, *pold_sai_wred_api;
    sai_remove_qos_map_fn old_remove_qos_map;
    sai_qos_map_api_t ut_sai_qos_map_api, *pold_sai_qos_map_api;
    sai_set_switch_attribute_fn old_set_switch_attribute_fn;
    sai_switch_api_t ut_sai_switch_api, *pold_sai_switch_api;

    sai_status_t _ut_stub_sai_set_switch_attribute(sai_object_id_t switch_id, const sai_attribute_t *attr)
    {
        auto rc = old_set_switch_attribute_fn(switch_id, attr);
        if (rc == SAI_STATUS_SUCCESS && attr->id == SAI_SWITCH_ATTR_QOS_DSCP_TO_TC_MAP)
            switch_dscp_to_tc_map_id = attr->value.oid;
        return rc;
    }

    sai_status_t _ut_stub_sai_remove_qos_map(sai_object_id_t qos_map_id)
    {
        auto rc = old_remove_qos_map(qos_map_id);
        if (rc == SAI_STATUS_SUCCESS)
            sai_remove_qos_map_count++;
        return rc;
    }

    sai_status_t _ut_stub_sai_remove_wred(sai_object_id_t wred_id)
    {
        auto rc = old_remove_wred(wred_id);
        if (rc == SAI_STATUS_SUCCESS)
            sai_remove_wred_profile_count++;
        return rc;
    }

    sai_status_t _ut_stub_sai_remove_scheduler(sai_object_id_t scheduler_id)
    {
        auto rc = old_remove_scheduler(scheduler_id);
        if (rc == SAI_STATUS_SUCCESS)
            sai_remove_scheduler_count++;
        return rc;
    }

    struct QosOrchTest : public ::testing::Test
    {
        QosOrchTest()
        {
        }

        void CheckDependency(const string &referencingTableName, const string &referencingObjectName, const string &field, const string &dependentTableName, const string &dependentObjectName="")
        {
            auto &qosTypeMaps = QosOrch::getTypeMap();
            auto &referencingTable = (*qosTypeMaps[referencingTableName]);
            auto &dependentTable = (*qosTypeMaps[dependentTableName]);

            if (dependentObjectName.empty())
            {
                ASSERT_TRUE(referencingTable[referencingObjectName].m_objsReferencingByMe[field].empty());
                ASSERT_EQ(dependentTable[dependentObjectName].m_objsDependingOnMe.count(referencingObjectName), 0);
            }
            else
            {
                ASSERT_EQ(referencingTable[referencingObjectName].m_objsReferencingByMe[field], dependentTableName + ":" + dependentObjectName);
                ASSERT_EQ(dependentTable[dependentObjectName].m_objsDependingOnMe.count(referencingObjectName), 1);
            }
        }

        void RemoveItem(const string &table, const string &key)
        {
            std::deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({key, "DEL", {}});
            auto consumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(table));
            consumer->addToSync(entries);
        }

        template<typename sai_api_t, typename sai_remove_func> void ReplaceSaiRemoveApi(sai_api_t* &sai_api,
                                                                                        sai_api_t &ut_sai_api,
                                                                                        sai_api_t* &pold_sai_api,
                                                                                        sai_remove_func ut_remove,
                                                                                        sai_remove_func &sai_remove,
                                                                                        sai_remove_func &old_remove,
                                                                                        sai_remove_func &put_remove)
        {
            old_remove = sai_remove;
            pold_sai_api = sai_api;
            ut_sai_api = *pold_sai_api;
            sai_api = &ut_sai_api;
            put_remove = ut_remove;
        }

        void SetUp() override
        {
            ASSERT_EQ(sai_route_api, nullptr);
            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            ut_helper::initSaiApi(profile);

            // Hack SAI APIs
            ReplaceSaiRemoveApi<sai_qos_map_api_t, sai_remove_qos_map_fn>(sai_qos_map_api, ut_sai_qos_map_api, pold_sai_qos_map_api,
                                                                          _ut_stub_sai_remove_qos_map, sai_qos_map_api->remove_qos_map,
                                                                          old_remove_qos_map, ut_sai_qos_map_api.remove_qos_map);
            ReplaceSaiRemoveApi<sai_scheduler_api_t, sai_remove_scheduler_fn>(sai_scheduler_api, ut_sai_scheduler_api, pold_sai_scheduler_api,
                                                                              _ut_stub_sai_remove_scheduler, sai_scheduler_api->remove_scheduler,
                                                                              old_remove_scheduler, ut_sai_scheduler_api.remove_scheduler);
            ReplaceSaiRemoveApi<sai_wred_api_t, sai_remove_wred_fn>(sai_wred_api, ut_sai_wred_api, pold_sai_wred_api,
                                                                    _ut_stub_sai_remove_wred, sai_wred_api->remove_wred,
                                                                    old_remove_wred, ut_sai_wred_api.remove_wred);
            pold_sai_switch_api = sai_switch_api;
            ut_sai_switch_api = *pold_sai_switch_api;
            old_set_switch_attribute_fn = pold_sai_switch_api->set_switch_attribute;
            sai_switch_api = &ut_sai_switch_api;
            ut_sai_switch_api.set_switch_attribute = _ut_stub_sai_set_switch_attribute;

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

            Table tcToQueueMapTable = Table(m_config_db.get(), CFG_TC_TO_QUEUE_MAP_TABLE_NAME);
            Table scheduleTable = Table(m_config_db.get(), CFG_SCHEDULER_TABLE_NAME);
            Table dscpToTcMapTable = Table(m_config_db.get(), CFG_DSCP_TO_TC_MAP_TABLE_NAME);
            Table dot1pToTcMapTable = Table(m_config_db.get(), CFG_DOT1P_TO_TC_MAP_TABLE_NAME);
            Table queueTable = Table(m_config_db.get(), CFG_QUEUE_TABLE_NAME);
            Table portQosMapTable = Table(m_config_db.get(), CFG_PORT_QOS_MAP_TABLE_NAME);
            Table wredProfileTable = Table(m_config_db.get(), CFG_WRED_PROFILE_TABLE_NAME);
            Table tcToPgMapTable = Table(m_config_db.get(), CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME);
            Table pfcPriorityToPgMapTable = Table(m_config_db.get(), CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME);
            Table pfcPriorityToQueueMapTable = Table(m_config_db.get(), CFG_PFC_PRIORITY_TO_QUEUE_MAP_TABLE_NAME);
            Table dscpToFcMapTable = Table(m_config_db.get(), CFG_DSCP_TO_FC_MAP_TABLE_NAME);
            Table expToFcMapTable = Table(m_config_db.get(), CFG_EXP_TO_FC_MAP_TABLE_NAME);

            scheduleTable.set("scheduler.1",
                              {
                                  {"type", "DWRR"},
                                  {"weight", "15"}
                              });

            scheduleTable.set("scheduler.0",
                              {
                                  {"type", "DWRR"},
                                  {"weight", "14"}
                              });

            wredProfileTable.set("AZURE_LOSSLESS",
                                 {
                                     {"ecn", "ecn_all"},
                                     {"green_drop_probability", "5"},
                                     {"green_max_threshold", "2097152"},
                                     {"green_min_threshold", "1048576"},
                                     {"wred_green_enable", "true"},
                                     {"yellow_drop_probability", "5"},
                                     {"yellow_max_threshold", "2097152"},
                                     {"yellow_min_threshold", "1048576"},
                                     {"wred_yellow_enable", "true"},
                                     {"red_drop_probability", "5"},
                                     {"red_max_threshold", "2097152"},
                                     {"red_min_threshold", "1048576"},
                                     {"wred_red_enable", "true"}
                                 });

            tcToQueueMapTable.set("AZURE",
                                  {
                                      {"0", "0"},
                                      {"1", "1"}
                                  });

            dscpToTcMapTable.set("AZURE",
                                 {
                                     {"0", "0"},
                                     {"1", "1"}
                                 });

            tcToPgMapTable.set("AZURE",
                               {
                                   {"0", "0"},
                                   {"1", "1"}
                               });

            dot1pToTcMapTable.set("AZURE",
                              {
                                  {"0", "0"},
                                  {"1", "1"}
                              });

            pfcPriorityToPgMapTable.set("AZURE",
                                    {
                                        {"0", "0"},
                                        {"1", "1"}
                                    });

            pfcPriorityToQueueMapTable.set("AZURE",
                                       {
                                           {"0", "0"},
                                           {"1", "1"}
                                       });

            dot1pToTcMapTable.set("AZURE",
                                  {
                                      {"0", "0"},
                                      {"1", "1"}
                                  });

            gQosOrch->addExistingData(&tcToQueueMapTable);
            gQosOrch->addExistingData(&dscpToTcMapTable);
            gQosOrch->addExistingData(&tcToPgMapTable);
            gQosOrch->addExistingData(&pfcPriorityToPgMapTable);
            gQosOrch->addExistingData(&pfcPriorityToQueueMapTable);
            gQosOrch->addExistingData(&scheduleTable);
            gQosOrch->addExistingData(&wredProfileTable);

            static_cast<Orch *>(gQosOrch)->doTask();
        }

        void TearDown() override
        {
            auto qos_maps = QosOrch::getTypeMap();
            for (auto &i : qos_maps)
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

            sai_qos_map_api = pold_sai_qos_map_api;
            sai_scheduler_api = pold_sai_scheduler_api;
            sai_wred_api = pold_sai_wred_api;
            sai_switch_api = pold_sai_switch_api;
            ut_helper::uninitSaiApi();
        }
    };

    TEST_F(QosOrchTest, QosOrchTestPortQosMapRemoveOneField)
    {
        Table portQosMapTable = Table(m_config_db.get(), CFG_PORT_QOS_MAP_TABLE_NAME);

        portQosMapTable.set("Ethernet0",
                            {
                                {"dscp_to_tc_map", "AZURE"},
                                {"pfc_to_pg_map", "AZURE"},
                                {"pfc_to_queue_map", "AZURE"},
                                {"tc_to_pg_map", "AZURE"},
                                {"tc_to_queue_map", "AZURE"},
                                {"pfc_enable", "3,4"}
                            });
        gQosOrch->addExistingData(&portQosMapTable);
        static_cast<Orch *>(gQosOrch)->doTask();

        // Check whether the dependencies have been recorded
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "dscp_to_tc_map", CFG_DSCP_TO_TC_MAP_TABLE_NAME, "AZURE");
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "pfc_to_pg_map", CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME, "AZURE");
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "pfc_to_queue_map", CFG_PFC_PRIORITY_TO_QUEUE_MAP_TABLE_NAME, "AZURE");
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "tc_to_pg_map", CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME, "AZURE");
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "tc_to_queue_map", CFG_TC_TO_QUEUE_MAP_TABLE_NAME, "AZURE");

        // Try removing AZURE from DSCP_TO_TC_MAP while it is still referenced
        RemoveItem(CFG_DSCP_TO_TC_MAP_TABLE_NAME, "AZURE");
        auto current_sai_remove_qos_map_count = sai_remove_qos_map_count;
        static_cast<Orch *>(gQosOrch)->doTask();
        ASSERT_EQ(current_sai_remove_qos_map_count, sai_remove_qos_map_count);
        // Dependency is not cleared
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "dscp_to_tc_map", CFG_DSCP_TO_TC_MAP_TABLE_NAME, "AZURE");

        // Remove dscp_to_tc_map from Ethernet0 via resetting the entry with field dscp_to_tc_map removed
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Ethernet0", "SET",
                           {
                               {"pfc_to_pg_map", "AZURE"},
                               {"pfc_to_queue_map", "AZURE"},
                               {"tc_to_pg_map", "AZURE"},
                               {"tc_to_queue_map", "AZURE"},
                               {"pfc_enable", "3,4"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_PORT_QOS_MAP_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();
        // Drain PORT_QOS_MAP table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Drain DSCP_TO_TC_MAP table
        static_cast<Orch *>(gQosOrch)->doTask();
        ASSERT_EQ(current_sai_remove_qos_map_count + 1, sai_remove_qos_map_count);
        ASSERT_EQ((*QosOrch::getTypeMap()[CFG_DSCP_TO_TC_MAP_TABLE_NAME]).count("AZURE"), 0);
        // Dependency of dscp_to_tc_map should be cleared
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "dscp_to_tc_map", CFG_DSCP_TO_TC_MAP_TABLE_NAME);
        // Dependencies of other items are not touched
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "pfc_to_pg_map", CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME, "AZURE");
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "pfc_to_queue_map", CFG_PFC_PRIORITY_TO_QUEUE_MAP_TABLE_NAME, "AZURE");
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "tc_to_pg_map", CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME, "AZURE");
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "tc_to_queue_map", CFG_TC_TO_QUEUE_MAP_TABLE_NAME, "AZURE");
    }

    TEST_F(QosOrchTest, QosOrchTestQueueRemoveWredProfile)
    {
        std::deque<KeyOpFieldsValuesTuple> entries;
        Table queueTable = Table(m_config_db.get(), CFG_QUEUE_TABLE_NAME);

        queueTable.set("Ethernet0|3",
                       {
                           {"scheduler", "scheduler.1"},
                           {"wred_profile", "AZURE_LOSSLESS"}
                       });
        gQosOrch->addExistingData(&queueTable);
        static_cast<Orch *>(gQosOrch)->doTask();

        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "scheduler", CFG_SCHEDULER_TABLE_NAME, "scheduler.1");
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "wred_profile", CFG_WRED_PROFILE_TABLE_NAME, "AZURE_LOSSLESS");

        // Try removing scheduler from WRED_PROFILE table while it is still referenced
        RemoveItem(CFG_WRED_PROFILE_TABLE_NAME, "AZURE_LOSSLESS");
        auto current_sai_remove_wred_profile_count = sai_remove_wred_profile_count;
        static_cast<Orch *>(gQosOrch)->doTask();
        ASSERT_EQ(current_sai_remove_wred_profile_count, sai_remove_wred_profile_count);
        // Make sure the dependency is untouched
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "wred_profile", CFG_WRED_PROFILE_TABLE_NAME, "AZURE_LOSSLESS");

        // Remove wred_profile from Ethernet0 queue 3
        entries.push_back({"Ethernet0|3", "SET",
                           {
                               {"scheduler", "scheduler.1"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_QUEUE_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();
        // Drain QUEUE table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Drain WRED_PROFILE table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Make sure the dependency is cleared
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "wred_profile", CFG_WRED_PROFILE_TABLE_NAME);
        // And the sai remove API has been called
        ASSERT_EQ(current_sai_remove_wred_profile_count + 1, sai_remove_wred_profile_count);
        ASSERT_EQ((*QosOrch::getTypeMap()[CFG_WRED_PROFILE_TABLE_NAME]).count("AZURE_LOSSLESS"), 0);
        // Other field should be untouched
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "scheduler", CFG_SCHEDULER_TABLE_NAME, "scheduler.1");
    }

    TEST_F(QosOrchTest, QosOrchTestQueueRemoveScheduler)
    {
        std::deque<KeyOpFieldsValuesTuple> entries;
        Table queueTable = Table(m_config_db.get(), CFG_QUEUE_TABLE_NAME);

        queueTable.set("Ethernet0|3",
                       {
                           {"scheduler", "scheduler.1"},
                           {"wred_profile", "AZURE_LOSSLESS"}
                       });
        gQosOrch->addExistingData(&queueTable);
        static_cast<Orch *>(gQosOrch)->doTask();

        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "scheduler", CFG_SCHEDULER_TABLE_NAME, "scheduler.1");
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "wred_profile", CFG_WRED_PROFILE_TABLE_NAME, "AZURE_LOSSLESS");

        // Try removing scheduler from QUEUE table while it is still referenced
        RemoveItem(CFG_SCHEDULER_TABLE_NAME, "scheduler.1");
        auto current_sai_remove_scheduler_count = sai_remove_scheduler_count;
        static_cast<Orch *>(gQosOrch)->doTask();
        ASSERT_EQ(current_sai_remove_scheduler_count, sai_remove_scheduler_count);
        // Make sure the dependency is untouched
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "scheduler", CFG_SCHEDULER_TABLE_NAME, "scheduler.1");

        // Remove scheduler from Ethernet0 queue 3
        entries.push_back({"Ethernet0|3", "SET",
                           {
                               {"wred_profile", "AZURE_LOSSLESS"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_QUEUE_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();
        // Drain QUEUE table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Drain SCHEDULER table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Make sure the dependency is cleared
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "scheduler", CFG_SCHEDULER_TABLE_NAME);
        // And the sai remove API has been called
        ASSERT_EQ(current_sai_remove_scheduler_count + 1, sai_remove_scheduler_count);
        ASSERT_EQ((*QosOrch::getTypeMap()[CFG_SCHEDULER_TABLE_NAME]).count("scheduler.1"), 0);
        // Other field should be untouched
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "wred_profile", CFG_WRED_PROFILE_TABLE_NAME, "AZURE_LOSSLESS");
    }

    TEST_F(QosOrchTest, QosOrchTestQueueReplaceFieldAndRemoveObject)
    {
        std::deque<KeyOpFieldsValuesTuple> entries;
        Table queueTable = Table(m_config_db.get(), CFG_QUEUE_TABLE_NAME);
        auto queueConsumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_QUEUE_TABLE_NAME));
        auto wredProfileConsumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_WRED_PROFILE_TABLE_NAME));
        auto schedulerConsumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_SCHEDULER_TABLE_NAME));

        queueTable.set("Ethernet0|3",
                       {
                           {"scheduler", "scheduler.1"},
                           {"wred_profile", "AZURE_LOSSLESS"}
                       });
        gQosOrch->addExistingData(&queueTable);
        static_cast<Orch *>(gQosOrch)->doTask();

        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "scheduler", CFG_SCHEDULER_TABLE_NAME, "scheduler.1");
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "wred_profile", CFG_WRED_PROFILE_TABLE_NAME, "AZURE_LOSSLESS");

        // Try replacing scheduler in QUEUE table: scheduler.1 => scheduler.0
        entries.push_back({"Ethernet0|3", "SET",
                           {
                               {"scheduler", "scheduler.0"},
                               {"wred_profile", "AZURE_LOSSLESS"}
                           }});
        queueConsumer->addToSync(entries);
        entries.clear();
        // Drain QUEUE table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Make sure the dependency is updated
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "scheduler", CFG_SCHEDULER_TABLE_NAME, "scheduler.0");
        // And the other field is not touched
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "wred_profile", CFG_WRED_PROFILE_TABLE_NAME, "AZURE_LOSSLESS");

        RemoveItem(CFG_SCHEDULER_TABLE_NAME, "scheduler.1");
        auto current_sai_remove_scheduler_count = sai_remove_scheduler_count;
        static_cast<Orch *>(gQosOrch)->doTask();
        ASSERT_EQ(++current_sai_remove_scheduler_count, sai_remove_scheduler_count);
        ASSERT_EQ((*QosOrch::getTypeMap()[CFG_SCHEDULER_TABLE_NAME]).count("scheduler.1"), 0);

        entries.push_back({"AZURE_LOSSLESS_1", "SET",
                           {
                               {"ecn", "ecn_all"},
                               {"green_drop_probability", "5"},
                               {"green_max_threshold", "2097152"},
                               {"green_min_threshold", "1048576"},
                               {"wred_green_enable", "true"},
                               {"yellow_drop_probability", "5"},
                               {"yellow_max_threshold", "2097152"},
                               {"yellow_min_threshold", "1048576"},
                               {"wred_yellow_enable", "true"},
                               {"red_drop_probability", "5"},
                               {"red_max_threshold", "2097152"},
                               {"red_min_threshold", "1048576"},
                               {"wred_red_enable", "true"}
                           }});
        wredProfileConsumer->addToSync(entries);
        entries.clear();
        // Drain WRED_PROFILE table
        static_cast<Orch *>(gQosOrch)->doTask();

        // Replace wred_profile from Ethernet0 queue 3
        entries.push_back({"Ethernet0|3", "SET",
                           {
                               {"scheduler", "scheduler.0"},
                               {"wred_profile", "AZURE_LOSSLESS_1"}
                           }});
        queueConsumer->addToSync(entries);
        entries.clear();
        // Drain QUEUE table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Make sure the dependency is updated
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "wred_profile", CFG_WRED_PROFILE_TABLE_NAME, "AZURE_LOSSLESS_1");
        // And the other field is not touched
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "scheduler", CFG_SCHEDULER_TABLE_NAME, "scheduler.0");

        RemoveItem(CFG_WRED_PROFILE_TABLE_NAME, "AZURE_LOSSLESS");
        // Drain WRED_PROFILE table
        auto current_sai_remove_wred_profile_count = sai_remove_wred_profile_count;
        static_cast<Orch *>(gQosOrch)->doTask();
        ASSERT_EQ(++current_sai_remove_wred_profile_count, sai_remove_wred_profile_count);
        ASSERT_EQ((*QosOrch::getTypeMap()[CFG_WRED_PROFILE_TABLE_NAME]).count("AZURE_LOSSLESS"), 0);

        // Remove object
        entries.push_back({"Ethernet0|3", "DEL", {}});
        queueConsumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gQosOrch)->doTask();

        // Make sure the dependency is updated
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "wred_profile", CFG_WRED_PROFILE_TABLE_NAME);
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|3", "scheduler", CFG_SCHEDULER_TABLE_NAME);

        // Remove scheduler object
        entries.push_back({"scheduler.0", "DEL", {}});
        schedulerConsumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gQosOrch)->doTask();
        ASSERT_EQ(++current_sai_remove_scheduler_count, sai_remove_scheduler_count);
        ASSERT_EQ((*QosOrch::getTypeMap()[CFG_SCHEDULER_TABLE_NAME]).count("scheduler.0"), 0);

        // Remove wred profile object
        entries.push_back({"AZURE_LOSSLESS_1", "DEL", {}});
        wredProfileConsumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gQosOrch)->doTask();
        ASSERT_EQ(++current_sai_remove_wred_profile_count, sai_remove_wred_profile_count);
        ASSERT_EQ((*QosOrch::getTypeMap()[CFG_WRED_PROFILE_TABLE_NAME]).count("AZURE_LOSSLESS_1"), 0);
    }

    TEST_F(QosOrchTest, QosOrchTestPortQosMapReplaceOneFieldAndRemoveObject)
    {
        std::deque<KeyOpFieldsValuesTuple> entries;
        Table portQosMapTable = Table(m_config_db.get(), CFG_PORT_QOS_MAP_TABLE_NAME);

        portQosMapTable.set("Ethernet0",
                            {
                                {"dscp_to_tc_map", "AZURE"},
                                {"pfc_to_pg_map", "AZURE"},
                                {"pfc_to_queue_map", "AZURE"},
                                {"tc_to_pg_map", "AZURE"},
                                {"tc_to_queue_map", "AZURE"},
                                {"pfc_enable", "3,4"}
                            });

        static_cast<Orch *>(gQosOrch)->doTask();

        entries.push_back({"AZURE_1", "SET",
                           {
                               {"1", "0"},
                               {"0", "1"}
                           }});

        auto consumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_DSCP_TO_TC_MAP_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();
        // Drain DSCP_TO_TC_MAP table
        static_cast<Orch *>(gQosOrch)->doTask();

        entries.push_back({"Ethernet0", "SET",
                           {
                               {"dscp_to_tc_map", "AZURE_1"},
                               {"pfc_to_pg_map", "AZURE"},
                               {"pfc_to_queue_map", "AZURE"},
                               {"tc_to_pg_map", "AZURE"},
                               {"tc_to_queue_map", "AZURE"},
                               {"pfc_enable", "3,4"}
                           }});
        consumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_PORT_QOS_MAP_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();
        // Drain PORT_QOS_MAP table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Dependency is updated
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "dscp_to_tc_map", CFG_DSCP_TO_TC_MAP_TABLE_NAME, "AZURE_1");

        // Try removing AZURE from DSCP_TO_TC_MAP
        RemoveItem(CFG_DSCP_TO_TC_MAP_TABLE_NAME, "AZURE");
        auto current_sai_remove_qos_map_count = sai_remove_qos_map_count;
        static_cast<Orch *>(gQosOrch)->doTask();
        ASSERT_EQ(++current_sai_remove_qos_map_count, sai_remove_qos_map_count);
        ASSERT_EQ((*QosOrch::getTypeMap()[CFG_DSCP_TO_TC_MAP_TABLE_NAME]).count("AZURE"), 0);

        // Make sure other dependencies are not touched
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "pfc_to_pg_map", CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME, "AZURE");
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "pfc_to_queue_map", CFG_PFC_PRIORITY_TO_QUEUE_MAP_TABLE_NAME, "AZURE");
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "tc_to_pg_map", CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME, "AZURE");
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "tc_to_queue_map", CFG_TC_TO_QUEUE_MAP_TABLE_NAME, "AZURE");

        // Remove port from PORT_QOS_MAP table
        entries.push_back({"Ethernet0", "DEL", {}});
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gQosOrch)->doTask();
        ASSERT_EQ((*QosOrch::getTypeMap()[CFG_PORT_QOS_MAP_TABLE_NAME]).count("Ethernet0"), 0);

        // Make sure the maps can be removed now. Checking anyone should suffice since all the maps are handled in the same way.
        entries.push_back({"AZURE", "DEL", {}});
        consumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gQosOrch)->doTask();
        ASSERT_EQ(++current_sai_remove_qos_map_count, sai_remove_qos_map_count);
        ASSERT_EQ((*QosOrch::getTypeMap()[CFG_PFC_PRIORITY_TO_PRIORITY_GROUP_MAP_TABLE_NAME]).count("AZURE"), 0);

        entries.push_back({"AZURE_1", "DEL", {}});
        consumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_DSCP_TO_TC_MAP_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gQosOrch)->doTask();
        ASSERT_EQ(++current_sai_remove_qos_map_count, sai_remove_qos_map_count);
        ASSERT_EQ((*QosOrch::getTypeMap()[CFG_DSCP_TO_TC_MAP_TABLE_NAME]).count("AZURE_1"), 0);
        // Global dscp to tc map should be cleared
        ASSERT_EQ((*QosOrch::getTypeMap()[CFG_DSCP_TO_TC_MAP_TABLE_NAME])["AZURE_1"].m_saiObjectId, SAI_NULL_OBJECT_ID);
    }

    TEST_F(QosOrchTest, QosOrchTestPortQosMapReferencingObjRemoveThenAdd)
    {
        vector<string> ts;
        std::deque<KeyOpFieldsValuesTuple> entries;
        Table portQosMapTable = Table(m_config_db.get(), CFG_PORT_QOS_MAP_TABLE_NAME);

        portQosMapTable.set("Ethernet0",
                            {
                                {"dscp_to_tc_map", "AZURE"}
                            });
        gQosOrch->addExistingData(&portQosMapTable);
        static_cast<Orch *>(gQosOrch)->doTask();
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "dscp_to_tc_map", CFG_DSCP_TO_TC_MAP_TABLE_NAME, "AZURE");

        // Remove referenced obj
        entries.push_back({"AZURE", "DEL", {}});
        auto dscpToTcMapConsumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_DSCP_TO_TC_MAP_TABLE_NAME));
        dscpToTcMapConsumer->addToSync(entries);
        entries.clear();
        // Drain DSCP_TO_TC_MAP table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Make sure the dependency remains
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "dscp_to_tc_map", CFG_DSCP_TO_TC_MAP_TABLE_NAME, "AZURE");
        // Make sure the notification isn't drained
        static_cast<Orch *>(gQosOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 1);
        ASSERT_EQ(ts[0], "DSCP_TO_TC_MAP|AZURE|DEL");
        ts.clear();

        // Remove and readd referencing obj
        entries.push_back({"Ethernet0", "DEL", {}});
        entries.push_back({"Ethernet0", "SET",
                           {
                               {"dscp_to_tc_map", "AZURE"}
                           }});
        auto portQosMapConsumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_PORT_QOS_MAP_TABLE_NAME));
        portQosMapConsumer->addToSync(entries);
        entries.clear();
        // Drain the PORT_QOS_MAP table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Drain the DSCP_TO_TC_MAP table which contains items need to retry
        static_cast<Orch *>(gQosOrch)->doTask();
        // The dependency should be removed
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "dscp_to_tc_map", CFG_DSCP_TO_TC_MAP_TABLE_NAME);
        static_cast<Orch *>(gQosOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 1);
        ASSERT_EQ(ts[0], "PORT_QOS_MAP|Ethernet0|SET|dscp_to_tc_map:AZURE");
        ts.clear();

        // Re-create referenced obj
        entries.push_back({"AZURE", "SET",
                           {
                               {"1", "0"}
                           }});
        dscpToTcMapConsumer->addToSync(entries);
        entries.clear();
        // Drain DSCP_TO_TC_MAP table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Make sure the dependency recovers
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "dscp_to_tc_map", CFG_DSCP_TO_TC_MAP_TABLE_NAME, "AZURE");

        // All items have been drained
        static_cast<Orch *>(gQosOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        // Remove and recreate the referenced obj
        entries.push_back({"AZURE", "DEL", {}});
        entries.push_back({"AZURE", "SET",
                           {
                               {"1", "0"}
                           }});
        dscpToTcMapConsumer->addToSync(entries);
        entries.clear();
        // Drain DSCP_TO_TC_MAP table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Make sure the dependency remains
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "dscp_to_tc_map", CFG_DSCP_TO_TC_MAP_TABLE_NAME, "AZURE");
        // Make sure the notification isn't drained
        static_cast<Orch *>(gQosOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 2);
        ASSERT_EQ(ts[0], "DSCP_TO_TC_MAP|AZURE|DEL");
        ASSERT_EQ(ts[1], "DSCP_TO_TC_MAP|AZURE|SET|1:0");
        ts.clear();
    }

    TEST_F(QosOrchTest, QosOrchTestQueueReferencingObjRemoveThenAdd)
    {
        vector<string> ts;
        std::deque<KeyOpFieldsValuesTuple> entries;
        Table queueTable = Table(m_config_db.get(), CFG_QUEUE_TABLE_NAME);

        queueTable.set("Ethernet0|0",
                       {
                           {"scheduler", "scheduler.0"}
                       });
        gQosOrch->addExistingData(&queueTable);
        static_cast<Orch *>(gQosOrch)->doTask();
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|0", "scheduler", CFG_SCHEDULER_TABLE_NAME, "scheduler.0");

        // Remove referenced obj
        entries.push_back({"scheduler.0", "DEL", {}});
        auto schedulerConsumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_SCHEDULER_TABLE_NAME));
        schedulerConsumer->addToSync(entries);
        entries.clear();
        // Drain SCHEDULER table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Make sure the dependency remains
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|0", "scheduler", CFG_SCHEDULER_TABLE_NAME, "scheduler.0");
        static_cast<Orch *>(gQosOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 1);
        ASSERT_EQ(ts[0], "SCHEDULER|scheduler.0|DEL");
        ts.clear();

        // Remove and readd referencing obj
        entries.push_back({"Ethernet0|0", "DEL", {}});
        entries.push_back({"Ethernet0|0", "SET",
                           {
                               {"scheduler", "scheduler.0"}
                           }});
        auto queueConsumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_QUEUE_TABLE_NAME));
        queueConsumer->addToSync(entries);
        entries.clear();
        // Drain QUEUE table
        static_cast<Orch *>(gQosOrch)->doTask();
        // The dependency should be removed
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|0", "scheduler", CFG_SCHEDULER_TABLE_NAME);
        static_cast<Orch *>(gQosOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 1);
        ASSERT_EQ(ts[0], "QUEUE|Ethernet0|0|SET|scheduler:scheduler.0");
        ts.clear();

        // Re-create referenced obj
        entries.push_back({"scheduler.0", "SET",
                           {
                               {"type", "DWRR"},
                               {"weight", "14"}
                           }});
        schedulerConsumer->addToSync(entries);
        entries.clear();
        // Drain SCHEDULER table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Drain QUEUE table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Make sure the dependency recovers
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|0", "scheduler", CFG_SCHEDULER_TABLE_NAME, "scheduler.0");

        // All items have been drained
        static_cast<Orch *>(gQosOrch)->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        // Remove and then re-add the referenced obj
        entries.push_back({"scheduler.0", "DEL", {}});
        entries.push_back({"scheduler.0", "SET",
                           {
                               {"type", "DWRR"},
                               {"weight", "14"}
                           }});
        schedulerConsumer->addToSync(entries);
        entries.clear();
        // Drain SCHEDULER table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Make sure the dependency remains
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|0", "scheduler", CFG_SCHEDULER_TABLE_NAME, "scheduler.0");
        static_cast<Orch *>(gQosOrch)->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 2);
        ASSERT_EQ(ts[0], "SCHEDULER|scheduler.0|DEL");
        ASSERT_EQ(ts[1], "SCHEDULER|scheduler.0|SET|type:DWRR|weight:14");
        ts.clear();
    }

    TEST_F(QosOrchTest, QosOrchTestGlobalDscpToTcMap)
    {
        // Create a new dscp to tc map
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"AZURE", "SET",
                           {
                               {"1", "0"},
                               {"0", "1"}
                           }});

        auto consumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_DSCP_TO_TC_MAP_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();

        entries.push_back({"global", "SET",
                            {
                                {"dscp_to_tc_map", "AZURE"}
                            }});
        consumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_PORT_QOS_MAP_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();

        // Drain DSCP_TO_TC_MAP and PORT_QOS_MAP table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Check DSCP_TO_TC_MAP|AZURE is applied to switch
        ASSERT_EQ((*QosOrch::getTypeMap()[CFG_DSCP_TO_TC_MAP_TABLE_NAME])["AZURE"].m_saiObjectId, switch_dscp_to_tc_map_id);

        // Remove global DSCP_TO_TC_MAP
        entries.push_back({"global", "DEL", {}});
        consumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_PORT_QOS_MAP_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();
        // Drain PORT_QOS_TABLE table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Check switch_level dscp_to_tc_map is set to NULL
        ASSERT_EQ(SAI_NULL_OBJECT_ID, switch_dscp_to_tc_map_id);

        entries.push_back({"AZURE", "DEL", {}});
        consumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_DSCP_TO_TC_MAP_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();

        auto current_sai_remove_qos_map_count = sai_remove_qos_map_count;
        // Drain DSCP_TO_TC_MAP table
        static_cast<Orch *>(gQosOrch)->doTask();
        // Check DSCP_TO_TC_MAP|AZURE is removed, and the switch_level dscp_to_tc_map is set to NULL
        ASSERT_EQ(current_sai_remove_qos_map_count + 1, sai_remove_qos_map_count);
        ASSERT_EQ((*QosOrch::getTypeMap()[CFG_DSCP_TO_TC_MAP_TABLE_NAME]).count("AZURE"), 0);
        
    }

    TEST_F(QosOrchTest, QosOrchTestRetryFirstItem)
    {
        // There was a bug in QosOrch that the 2nd notifications and after can not be handled, eg the 1st one needs to be retried
        // This is to verify the bug has been fixed
        vector<string> ts;
        std::deque<KeyOpFieldsValuesTuple> entries;

        // Try adding dscp_to_tc_map AZURE.1 and AZURE to PORT_QOS_MAP table
        // The object AZURE.1 does not exist so the first item can not be handled and remain in m_toSync.
        entries.push_back({"Ethernet0", "SET",
                           {
                               {"dscp_to_tc_map", "AZURE.1"}
                           }});
        entries.push_back({"Ethernet4", "SET",
                           {
                               {"dscp_to_tc_map", "AZURE"}
                           }});
        auto portQosMapConsumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_PORT_QOS_MAP_TABLE_NAME));
        portQosMapConsumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gQosOrch)->doTask();
        // The 2nd notification should be handled. Make sure by checking reference
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet4", "dscp_to_tc_map", CFG_DSCP_TO_TC_MAP_TABLE_NAME, "AZURE");
        // Make sure there is one item left
        portQosMapConsumer->dumpPendingTasks(ts);
        ASSERT_EQ(ts[0], "PORT_QOS_MAP|Ethernet0|SET|dscp_to_tc_map:AZURE.1");
        ASSERT_EQ(ts.size(), 1);
        ts.clear();

        // Try adding scheduler.0 and scheduler.2 to QUEUE table
        entries.push_back({"Ethernet0|0", "SET",
                           {
                               {"scheduler", "scheduler.2"}
                           }});
        entries.push_back({"Ethernet0|1", "SET",
                           {
                               {"scheduler", "scheduler.0"}
                           }});
        auto queueConsumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_QUEUE_TABLE_NAME));
        queueConsumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gQosOrch)->doTask();
        // The 2nd notification should be handled. Make sure by checking reference
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|1", "scheduler", CFG_SCHEDULER_TABLE_NAME, "scheduler.0");
        // Make sure there is one item left
        queueConsumer->dumpPendingTasks(ts);
        ASSERT_EQ(ts[0], "QUEUE|Ethernet0|0|SET|scheduler:scheduler.2");
        ASSERT_EQ(ts.size(), 1);
        ts.clear();

        // Try removing AZURE and adding AZURE.1 to DSCP_TO_TC_MAP table
        entries.push_back({"AZURE", "DEL", {{}}});
        entries.push_back({"AZURE.1", "SET",
                           {
                               {"1", "1"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_DSCP_TO_TC_MAP_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gQosOrch)->doTask();
        // The 2nd notification should be handled. Make sure by checking reference
        CheckDependency(CFG_PORT_QOS_MAP_TABLE_NAME, "Ethernet0", "dscp_to_tc_map", CFG_DSCP_TO_TC_MAP_TABLE_NAME, "AZURE.1");
        // The pending item in PORT_QOS_MAP table should also be handled since the dependency is met
        portQosMapConsumer->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        consumer->dumpPendingTasks(ts);
        ASSERT_EQ(ts[0], "DSCP_TO_TC_MAP|AZURE|DEL|:");
        ASSERT_EQ(ts.size(), 1);
        ts.clear();

        entries.push_back({"scheduler.0", "DEL", {{}}});
        entries.push_back({"scheduler.2", "SET",
                           {
                               {"type", "DWRR"},
                               {"weight", "15"}
                           }});
        consumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_SCHEDULER_TABLE_NAME));
        consumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gQosOrch)->doTask();
        // We need a second call to "doTask" because scheduler table is handled after queue table
        static_cast<Orch *>(gQosOrch)->doTask();
        // The 2nd notification should be handled. Make sure by checking reference
        CheckDependency(CFG_QUEUE_TABLE_NAME, "Ethernet0|0", "scheduler", CFG_SCHEDULER_TABLE_NAME, "scheduler.2");
        // The pending item in QUEUE table should also be handled since the dependency is met
        queueConsumer->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        consumer->dumpPendingTasks(ts);
        ASSERT_EQ(ts[0], "SCHEDULER|scheduler.0|DEL|:");
        ASSERT_EQ(ts.size(), 1);
        ts.clear();
    }
}
