#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private

#include "json.h"
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "notifier.h"
#include "mock_sai_bridge.h"
#define private public
#include "pfcactionhandler.h"
#include "switchorch.h"
#include <sys/mman.h>
#undef private
#define private public
#include "warm_restart.h"
#undef private

#include <sstream>

extern redisReply *mockReply;
using ::testing::_;
using ::testing::StrictMock;

namespace portsorch_test
{
    using namespace std;

    // SAI default ports
    std::map<std::string, std::vector<swss::FieldValueTuple>> defaultPortList;

    sai_port_api_t ut_sai_port_api;
    sai_port_api_t *pold_sai_port_api;
    sai_switch_api_t ut_sai_switch_api;
    sai_switch_api_t *pold_sai_switch_api;

    bool not_support_fetching_fec;
    uint32_t _sai_set_port_fec_count;
    int32_t _sai_port_fec_mode;
    vector<sai_port_fec_mode_t> mock_port_fec_modes = {SAI_PORT_FEC_MODE_RS, SAI_PORT_FEC_MODE_FC};

    sai_status_t _ut_stub_sai_get_port_attribute(
        _In_ sai_object_id_t port_id,
        _In_ uint32_t attr_count,
        _Inout_ sai_attribute_t *attr_list)
    {
        sai_status_t status;
        if (attr_count == 1 && attr_list[0].id == SAI_PORT_ATTR_SUPPORTED_FEC_MODE)
        {
            if (not_support_fetching_fec)
            {
                status = SAI_STATUS_NOT_IMPLEMENTED;
            }
            else
            {
                uint32_t i;
                for (i = 0; i < attr_list[0].value.s32list.count && i < mock_port_fec_modes.size(); i++)
                {
                    attr_list[0].value.s32list.list[i] = mock_port_fec_modes[i];
                }
                attr_list[0].value.s32list.count = i;
                status = SAI_STATUS_SUCCESS;
            }
        }
        else if (attr_count == 1 && attr_list[0].id == SAI_PORT_ATTR_OPER_PORT_FEC_MODE)
        {
            attr_list[0].value.s32 = _sai_port_fec_mode;
            status = SAI_STATUS_SUCCESS;
        }
        else if (attr_count== 1 && attr_list[0].id == SAI_PORT_ATTR_OPER_STATUS)
        {
            attr_list[0].value.u32 = (uint32_t)SAI_PORT_OPER_STATUS_UP;
            status = SAI_STATUS_SUCCESS;
        }
        else
        {
            status = pold_sai_port_api->get_port_attribute(port_id, attr_count, attr_list);
        }
        return status;
    }

    uint32_t _sai_set_pfc_mode_count;
    uint32_t _sai_set_admin_state_up_count;
    uint32_t _sai_set_admin_state_down_count;
    bool set_pt_interface_id_fail = false;
    bool set_pt_timestamp_template_fail = false;
    bool set_port_tam_fail = false;
    uint32_t set_pt_interface_id_count = false;
    uint32_t set_pt_timestamp_template_count = false;
    uint32_t set_port_tam_count = false;
    uint32_t set_pt_interface_id_failures;
    uint32_t set_pt_timestamp_template_failures;
    uint32_t set_port_tam_failures;
    bool set_link_event_damping_success = true;
    uint32_t _sai_set_link_event_damping_algorithm_count;
    uint32_t _sai_set_link_event_damping_config_count;
    int32_t _sai_link_event_damping_algorithm = 0;
    sai_redis_link_event_damping_algo_aied_config_t _sai_link_event_damping_config = {0, 0, 0, 0, 0};

    sai_status_t _ut_stub_sai_set_port_attribute(
        _In_ sai_object_id_t port_id,
        _In_ const sai_attribute_t *attr)
    {
        if (attr[0].id == SAI_PORT_ATTR_FEC_MODE)
        {
            _sai_set_port_fec_count++;
            _sai_port_fec_mode = attr[0].value.s32;
        }
        else if (attr[0].id == SAI_PORT_ATTR_AUTO_NEG_MODE)
        {
            /* Simulating failure case */
            return SAI_STATUS_FAILURE;
        }
	else if (attr[0].id == SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_COMBINED)
	{
	    _sai_set_pfc_mode_count++;
        }
	else if (attr[0].id == SAI_PORT_ATTR_ADMIN_STATE)
	{
            if (attr[0].value.booldata) {
	        _sai_set_admin_state_up_count++;
            } else {
	        _sai_set_admin_state_down_count++;
            }
        }
        else if (attr[0].id == SAI_PORT_ATTR_PATH_TRACING_INTF)
        {
            set_pt_interface_id_count++;
            /* Simulating failure case */
            if (set_pt_interface_id_fail)
            {
                set_pt_interface_id_failures++;
                return SAI_STATUS_INVALID_ATTR_VALUE_0;
            }
        }
        else if (attr[0].id == SAI_PORT_ATTR_PATH_TRACING_TIMESTAMP_TYPE)
        {
            set_pt_timestamp_template_count++;
            /* Simulating failure case */
            if (set_pt_timestamp_template_fail)
            {
                set_pt_timestamp_template_failures++;
                return SAI_STATUS_INVALID_ATTR_VALUE_0;
            }
        }
        else if (attr[0].id == SAI_PORT_ATTR_TAM_OBJECT)
        {
            set_port_tam_count++;
            /* Simulating failure case */
            if (set_port_tam_fail)
            {
                set_port_tam_failures++;
                return SAI_STATUS_INVALID_ATTR_VALUE_0;
            }
        }
        else if (attr[0].id == SAI_REDIS_PORT_ATTR_LINK_EVENT_DAMPING_ALGORITHM)
        {
            _sai_set_link_event_damping_algorithm_count++;

            if (set_link_event_damping_success) {
                _sai_link_event_damping_algorithm = attr[0].value.s32;
                return SAI_STATUS_SUCCESS;
            }
            return SAI_STATUS_FAILURE;
        }
        else if (attr[0].id == SAI_REDIS_PORT_ATTR_LINK_EVENT_DAMPING_ALGO_AIED_CONFIG)
        {
            _sai_set_link_event_damping_config_count++;

            if (set_link_event_damping_success) {
                _sai_link_event_damping_config = *(reinterpret_cast<sai_redis_link_event_damping_algo_aied_config_t*>(attr[0].value.ptr));
                return SAI_STATUS_SUCCESS;
            }
            return SAI_STATUS_FAILURE;
        }
        return pold_sai_port_api->set_port_attribute(port_id, attr);
    }

    vector<sai_object_type_t> supported_sai_objects = {
        SAI_OBJECT_TYPE_PORT,
        SAI_OBJECT_TYPE_LAG,
        SAI_OBJECT_TYPE_TAM,
        SAI_OBJECT_TYPE_TAM_INT,
        SAI_OBJECT_TYPE_TAM_COLLECTOR,
        SAI_OBJECT_TYPE_TAM_REPORT,
        SAI_OBJECT_TYPE_TAM_TRANSPORT,
        SAI_OBJECT_TYPE_TAM_TELEMETRY,
        SAI_OBJECT_TYPE_TAM_EVENT_THRESHOLD
    };

    sai_status_t _ut_stub_sai_get_switch_attribute(
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _Inout_ sai_attribute_t *attr_list)
    {
        sai_status_t status;
        if (attr_count == 1 && attr_list[0].id == SAI_SWITCH_ATTR_SUPPORTED_OBJECT_TYPE_LIST)
        {
            uint32_t i;
            for (i = 0; i < attr_list[0].value.s32list.count && i < supported_sai_objects.size(); i++)
            {
                attr_list[0].value.s32list.list[i] = supported_sai_objects[i];
            }
            attr_list[0].value.s32list.count = i;
            status = SAI_STATUS_SUCCESS;
        }
        else
        {
            status = pold_sai_switch_api->get_switch_attribute(switch_id, attr_count, attr_list);
        }
        return status;
    }

    uint32_t *_sai_syncd_notifications_count;
    int32_t *_sai_syncd_notification_event;
    uint32_t _sai_switch_dlr_packet_action_count;
    uint32_t _sai_switch_dlr_packet_action;
    sai_status_t _ut_stub_sai_set_switch_attribute(
        _In_ sai_object_id_t switch_id,
        _In_ const sai_attribute_t *attr)
    {
        if (attr[0].id == SAI_REDIS_SWITCH_ATTR_NOTIFY_SYNCD)
        {
            *_sai_syncd_notifications_count =+ 1;
            *_sai_syncd_notification_event = attr[0].value.s32;
        }
	else if (attr[0].id == SAI_SWITCH_ATTR_PFC_DLR_PACKET_ACTION)
        {
	    _sai_switch_dlr_packet_action_count++;
	    _sai_switch_dlr_packet_action = attr[0].value.s32;
	}
        return pold_sai_switch_api->set_switch_attribute(switch_id, attr);
    }

    void _hook_sai_port_api()
    {
        ut_sai_port_api = *sai_port_api;
        pold_sai_port_api = sai_port_api;
        ut_sai_port_api.get_port_attribute = _ut_stub_sai_get_port_attribute;
        ut_sai_port_api.set_port_attribute = _ut_stub_sai_set_port_attribute;
        sai_port_api = &ut_sai_port_api;
    }

    void _unhook_sai_port_api()
    {
        sai_port_api = pold_sai_port_api;
    }

    void _hook_sai_switch_api()
    {
        ut_sai_switch_api = *sai_switch_api;
        pold_sai_switch_api = sai_switch_api;
        ut_sai_switch_api.set_switch_attribute = _ut_stub_sai_set_switch_attribute;
        ut_sai_switch_api.get_switch_attribute = _ut_stub_sai_get_switch_attribute;
        sai_switch_api = &ut_sai_switch_api;
    }

    void _unhook_sai_switch_api()
    {
        sai_switch_api = pold_sai_switch_api;
    }

    sai_queue_api_t ut_sai_queue_api;
    sai_queue_api_t *pold_sai_queue_api;
    int _sai_set_queue_attr_count = 0;

    sai_status_t _ut_stub_sai_set_queue_attribute(sai_object_id_t queue_id, const sai_attribute_t *attr)
    {
        if(attr->id == SAI_QUEUE_ATTR_PFC_DLR_INIT)
        {
            if(attr->value.booldata == true)
            {
                _sai_set_queue_attr_count++;
            }
            else
            {
                _sai_set_queue_attr_count--;
            }
        }
        return SAI_STATUS_SUCCESS;
    }

    uint32_t _sai_get_queue_attr_count;
    bool _sai_mock_queue_attr = false;
    sai_status_t _ut_stub_sai_get_queue_attribute(
        _In_ sai_object_id_t queue_id,
        _In_ uint32_t attr_count,
        _Inout_ sai_attribute_t *attr_list)
    {
        if (_sai_mock_queue_attr)
        {
            _sai_get_queue_attr_count++;
            for (auto i = 0u; i < attr_count; i++)
            {
                if (attr_list[i].id == SAI_QUEUE_ATTR_TYPE)
                {
                    attr_list[i].value.s32 = static_cast<sai_queue_type_t>(SAI_QUEUE_TYPE_UNICAST);
                }
                else if (attr_list[i].id == SAI_QUEUE_ATTR_INDEX)
                {
                    attr_list[i].value.u8 = 0;
                }
                else
                {
                    pold_sai_queue_api->get_queue_attribute(queue_id, 1, &attr_list[i]);
                }
            }
        }

        return SAI_STATUS_SUCCESS;
    }

    void _hook_sai_queue_api()
    {
        _sai_mock_queue_attr = true;
        ut_sai_queue_api = *sai_queue_api;
        pold_sai_queue_api = sai_queue_api;
        ut_sai_queue_api.set_queue_attribute = _ut_stub_sai_set_queue_attribute;
        ut_sai_queue_api.get_queue_attribute = _ut_stub_sai_get_queue_attribute;
        sai_queue_api = &ut_sai_queue_api;
    }

    void _unhook_sai_queue_api()
    {
        sai_queue_api = pold_sai_queue_api;
        _sai_mock_queue_attr = false;
    }

    sai_bridge_api_t ut_sai_bridge_api;
    sai_bridge_api_t *org_sai_bridge_api;

    void _hook_sai_bridge_api()
    {
        ut_sai_bridge_api = *sai_bridge_api;
        org_sai_bridge_api = sai_bridge_api;
        sai_bridge_api = &ut_sai_bridge_api;
    }

    void _unhook_sai_bridge_api()
    {
        sai_bridge_api = org_sai_bridge_api;
    }

    void cleanupPorts(PortsOrch *obj)
    {
        // Get CPU port
        Port p;
        obj->getCpuPort(p);

        // Get port list
        auto portList = obj->getAllPorts();
        portList.erase(p.m_alias);

        // Generate port config
        std::deque<KeyOpFieldsValuesTuple> kfvList;

        for (const auto &cit : portList)
        {
            kfvList.push_back({ cit.first, DEL_COMMAND, { } });
        }

        // Refill consumer
        auto consumer = dynamic_cast<Consumer*>(obj->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(kfvList);

        // Apply configuration
        static_cast<Orch*>(obj)->doTask();

        // Dump pending tasks
        std::vector<std::string> taskList;
        obj->dumpPendingTasks(taskList);
        ASSERT_TRUE(taskList.empty());
    }

    struct PortsOrchTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::DBConnector> m_counters_db;
        shared_ptr<swss::DBConnector> m_chassis_app_db;
        shared_ptr<swss::DBConnector> m_asic_db;

        PortsOrchTest()
        {
            // FIXME: move out from constructor
            m_app_db = make_shared<swss::DBConnector>(
                "APPL_DB", 0);
            m_counters_db = make_shared<swss::DBConnector>(
                "COUNTERS_DB", 0);
            m_config_db = make_shared<swss::DBConnector>(
                "CONFIG_DB", 0);
            m_state_db = make_shared<swss::DBConnector>(
                "STATE_DB", 0);
            m_chassis_app_db = make_shared<swss::DBConnector>(
                "CHASSIS_APP_DB", 0);
            m_asic_db = make_shared<swss::DBConnector>(
                "ASIC_DB", 0);
        }

        virtual void SetUp() override
        {
            ::testing_db::reset();

            // Create dependencies ...
            TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
            TableConnector app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);
            TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);

            vector<TableConnector> switch_tables = {
                conf_asic_sensors,
                app_switch_table
            };

            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

            const int portsorch_base_pri = 40;

            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
                { APP_SEND_TO_INGRESS_PORT_TABLE_NAME, portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
                { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
            };

            ASSERT_EQ(gPortsOrch, nullptr);

            gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), ports_tables, m_chassis_app_db.get());

            vector<string> flex_counter_tables = {
                CFG_FLEX_COUNTER_TABLE_NAME
            };
            auto* flexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
            gDirectory.set(flexCounterOrch);

            vector<string> buffer_tables = { APP_BUFFER_POOL_TABLE_NAME,
                                             APP_BUFFER_PROFILE_TABLE_NAME,
                                             APP_BUFFER_QUEUE_TABLE_NAME,
                                             APP_BUFFER_PG_TABLE_NAME,
                                             APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                                             APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME };

            ASSERT_EQ(gBufferOrch, nullptr);
            gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);

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
                CFG_EXP_TO_FC_MAP_TABLE_NAME,
                CFG_TC_TO_DSCP_MAP_TABLE_NAME
            };
            gQosOrch = new QosOrch(m_config_db.get(), qos_tables);

            vector<string> pfc_wd_tables = {
                CFG_PFC_WD_TABLE_NAME
            };

            static const vector<sai_port_stat_t> portStatIds =
            {
                SAI_PORT_STAT_PFC_0_RX_PKTS,
                SAI_PORT_STAT_PFC_1_RX_PKTS,
                SAI_PORT_STAT_PFC_2_RX_PKTS,
                SAI_PORT_STAT_PFC_3_RX_PKTS,
                SAI_PORT_STAT_PFC_4_RX_PKTS,
                SAI_PORT_STAT_PFC_5_RX_PKTS,
                SAI_PORT_STAT_PFC_6_RX_PKTS,
                SAI_PORT_STAT_PFC_7_RX_PKTS,
                SAI_PORT_STAT_PFC_0_ON2OFF_RX_PKTS,
                SAI_PORT_STAT_PFC_1_ON2OFF_RX_PKTS,
                SAI_PORT_STAT_PFC_2_ON2OFF_RX_PKTS,
                SAI_PORT_STAT_PFC_3_ON2OFF_RX_PKTS,
                SAI_PORT_STAT_PFC_4_ON2OFF_RX_PKTS,
                SAI_PORT_STAT_PFC_5_ON2OFF_RX_PKTS,
                SAI_PORT_STAT_PFC_6_ON2OFF_RX_PKTS,
                SAI_PORT_STAT_PFC_7_ON2OFF_RX_PKTS,
            };

            static const vector<sai_queue_stat_t> queueStatIds =
            {
                SAI_QUEUE_STAT_PACKETS,
                SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES,
            };

            static const vector<sai_queue_attr_t> queueAttrIds =
            {
                SAI_QUEUE_ATTR_PAUSE_STATUS,
            };
            ASSERT_EQ((gPfcwdOrch<PfcWdDlrHandler, PfcWdDlrHandler>), nullptr);
            gPfcwdOrch<PfcWdDlrHandler, PfcWdDlrHandler> = new PfcWdSwOrch<PfcWdDlrHandler, PfcWdDlrHandler>(m_config_db.get(), pfc_wd_tables, portStatIds, queueStatIds, queueAttrIds, 100);

        }

        virtual void TearDown() override
        {
            ::testing_db::reset();

            auto buffer_maps = BufferOrch::m_buffer_type_maps;
            for (auto &i : buffer_maps)
            {
                i.second->clear();
            }

            delete gNeighOrch;
            gNeighOrch = nullptr;
            delete gFdbOrch;
            gFdbOrch = nullptr;
            delete gIntfsOrch;
            gIntfsOrch = nullptr;
            delete gPortsOrch;
            gPortsOrch = nullptr;
            delete gBufferOrch;
            gBufferOrch = nullptr;
            delete gPfcwdOrch<PfcWdDlrHandler, PfcWdDlrHandler>;
            gPfcwdOrch<PfcWdDlrHandler, PfcWdDlrHandler> = nullptr;
            delete gQosOrch;
            gQosOrch = nullptr;
            delete gSwitchOrch;
            gSwitchOrch = nullptr;

            // clear orchs saved in directory
            gDirectory.m_values.clear();
        }

        static void SetUpTestCase()
        {
            // Init switch and create dependencies

            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };

            auto status = ut_helper::initSaiApi(profile);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            sai_attribute_t attr;

            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;

            status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
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

            // Get SAI default ports
            defaultPortList = ut_helper::getInitialSaiPorts();
            ASSERT_TRUE(!defaultPortList.empty());
        }

        static void TearDownTestCase()
        {
            auto status = sai_switch_api->remove_switch(gSwitchId);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gSwitchId = 0;

            ut_helper::uninitSaiApi();
        }

    };

    /*
    * Test port flap count
    */
    TEST_F(PortsOrchTest, PortFlapCount)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);
        // Apply configuration : create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        // Get first port, expect the oper status is not UP
        Port port;
        gPortsOrch->getPort("Ethernet0", port);
        ASSERT_TRUE(port.m_oper_status != SAI_PORT_OPER_STATUS_UP);
        ASSERT_TRUE(port.m_flap_count == 0);

        auto exec = static_cast<Notifier *>(gPortsOrch->getExecutor("PORT_STATUS_NOTIFICATIONS"));
        auto consumer = exec->getNotificationConsumer();

        // mock a redis reply for notification, it notifies that Ehernet0 is going to up
        for (uint32_t count=0; count < 5; count++) {
            sai_port_oper_status_t oper_status = (count % 2 == 0) ? SAI_PORT_OPER_STATUS_UP : SAI_PORT_OPER_STATUS_DOWN;
            mockReply = (redisReply *)calloc(sizeof(redisReply), 1);
            mockReply->type = REDIS_REPLY_ARRAY;
            mockReply->elements = 3; // REDIS_PUBLISH_MESSAGE_ELEMNTS
            mockReply->element = (redisReply **)calloc(sizeof(redisReply *), mockReply->elements);
            mockReply->element[2] = (redisReply *)calloc(sizeof(redisReply), 1);
            mockReply->element[2]->type = REDIS_REPLY_STRING;
            sai_port_oper_status_notification_t port_oper_status;
            port_oper_status.port_state = oper_status;
            port_oper_status.port_id = port.m_port_id;
            std::string data = sai_serialize_port_oper_status_ntf(1, &port_oper_status);
            std::vector<FieldValueTuple> notifyValues;
            FieldValueTuple opdata("port_state_change", data);
            notifyValues.push_back(opdata);
            std::string msg = swss::JSon::buildJson(notifyValues);
            mockReply->element[2]->str = (char*)calloc(1, msg.length() + 1);
            memcpy(mockReply->element[2]->str, msg.c_str(), msg.length());

            // trigger the notification
            consumer->readData();
            gPortsOrch->doTask(*consumer);
            mockReply = nullptr;

            gPortsOrch->getPort("Ethernet0", port);
            ASSERT_TRUE(port.m_oper_status == oper_status);
            ASSERT_TRUE(port.m_flap_count == count+1);
        }

        cleanupPorts(gPortsOrch);
    }

    TEST_F(PortsOrchTest, PortBulkCreateRemove)
    {
        auto portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

        // Get SAI default ports
        auto &ports = defaultPortList;
        ASSERT_TRUE(!ports.empty());

        // Generate port config
        for (std::uint32_t idx1 = 0, idx2 = 1; idx1 < ports.size() * 4; idx1 += 4, idx2++)
        {
            std::stringstream key;
            key << FRONT_PANEL_PORT_PREFIX << idx1;

            std::stringstream alias;
            alias << "etp" << idx2;

            std::stringstream index;
            index << idx2;

            std::stringstream lanes;
            lanes << idx1 << "," << idx1 + 1 << "," << idx1 + 2 << "," << idx1 + 3;

            std::vector<FieldValueTuple> fvList = {
                { "alias",               alias.str() },
                { "index",               index.str() },
                { "lanes",               lanes.str() },
                { "speed",               "100000"    },
                { "autoneg",             "off"       },
                { "adv_speeds",          "all"       },
                { "interface_type",      "none"      },
                { "adv_interface_types", "all"       },
                { "fec",                 "rs"        },
                { "mtu",                 "9100"      },
                { "tpid",                "0x8100"    },
                { "pfc_asym",            "off"       },
                { "admin_status",        "up"        },
                { "description",         "FP port"   }
            };

            portTable.set(key.str(), fvList);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", std::to_string(ports.size()) } });

        // Refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();

        // Port count: 32 Data + 1 CPU
        ASSERT_EQ(gPortsOrch->getAllPorts().size(), ports.size() + 1);

        // Dump pending tasks
        std::vector<std::string> taskList;
        gPortsOrch->dumpPendingTasks(taskList);
        ASSERT_TRUE(taskList.empty());

        // Cleanup ports
        cleanupPorts(gPortsOrch);

        // Check buffer maximum parameter table entries are removed
        auto bufferMaxParameterTable = Table(m_state_db.get(), STATE_BUFFER_MAXIMUM_VALUE_TABLE);
        std::vector<std::string> keys;
        bufferMaxParameterTable.getKeys(keys);
        ASSERT_TRUE(keys.empty());
    }

    TEST_F(PortsOrchTest, PortBasicConfig)
    {
        auto portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

        // Get SAI default ports
        auto &ports = defaultPortList;
        ASSERT_TRUE(!ports.empty());

        // Generate port config
        for (const auto &cit : ports)
        {
            portTable.set(cit.first, cit.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", std::to_string(ports.size()) } });

        // Refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();

        // Port count: 32 Data + 1 CPU
        ASSERT_EQ(gPortsOrch->getAllPorts().size(), ports.size() + 1);

        // Generate port config
        std::deque<KeyOpFieldsValuesTuple> kfvList = {{
            "Ethernet0",
            SET_COMMAND, {
                { "speed",               "100000"            },
                { "autoneg",             "on"                },
                { "adv_speeds",          "1000,10000,100000" },
                { "interface_type",      "CR"                },
                { "adv_interface_types", "CR,CR2,CR4,CR8"    },
                { "fec",                 "fc"                },
                { "mtu",                 "9100"              },
                { "tpid",                "0x9100"            },
                { "pfc_asym",            "on"                },
                { "link_training",       "on"                },
                { "admin_status",        "up"                }
            }
        }};

        // Refill consumer
        auto consumer = dynamic_cast<Consumer*>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(kfvList);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();

        // Get port
        Port p;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", p));

        // Verify speed
        ASSERT_EQ(p.m_speed, 100000);

        // Verify auto-negotiation
        ASSERT_TRUE(p.m_autoneg);

        // Verify advertised speed
        std::set<std::uint32_t> adv_speeds = { 1000, 10000, 100000 };
        ASSERT_EQ(p.m_adv_speeds, adv_speeds);

        // Verify interface type
        ASSERT_EQ(p.m_interface_type, SAI_PORT_INTERFACE_TYPE_CR);

        // Verify advertised interface type
        std::set<sai_port_interface_type_t> adv_interface_types = {
            SAI_PORT_INTERFACE_TYPE_CR,
            SAI_PORT_INTERFACE_TYPE_CR2,
            SAI_PORT_INTERFACE_TYPE_CR4,
            SAI_PORT_INTERFACE_TYPE_CR8
        };
        ASSERT_EQ(p.m_adv_interface_types, adv_interface_types);

        // Verify FEC
        ASSERT_EQ(p.m_fec_mode, SAI_PORT_FEC_MODE_FC);

        // Verify MTU
        ASSERT_EQ(p.m_mtu, 9100);

        // Verify TPID
        ASSERT_EQ(p.m_tpid, 0x9100);

        // Verify asymmetric PFC
        ASSERT_EQ(p.m_pfc_asym, SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_SEPARATE);

        // Verify link training
        ASSERT_TRUE(p.m_link_training);

        // Verify admin status
        ASSERT_TRUE(p.m_admin_state_up);

        // Dump pending tasks
        std::vector<std::string> taskList;
        gPortsOrch->dumpPendingTasks(taskList);
        ASSERT_TRUE(taskList.empty());

        // Cleanup ports
        cleanupPorts(gPortsOrch);
    }

    TEST_F(PortsOrchTest, PortAdvancedConfig)
    {
        auto portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

        // Get SAI default ports
        auto &ports = defaultPortList;
        ASSERT_TRUE(!ports.empty());

        // Generate port config
        for (const auto &cit : ports)
        {
            portTable.set(cit.first, cit.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", std::to_string(ports.size()) } });

        // Refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();

        // Port count: 32 Data + 1 CPU
        ASSERT_EQ(gPortsOrch->getAllPorts().size(), ports.size() + 1);

        // Generate port serdes config
        std::deque<KeyOpFieldsValuesTuple> kfvList = {{
            "Ethernet0",
            SET_COMMAND, {
                { "preemphasis", "0xcad0,0xc6e0,0xc6e0,0xd2b0" },
                { "idriver",     "0x5,0x3,0x4,0x1"             },
                { "ipredriver",  "0x1,0x4,0x3,0x5"             },
                { "pre1",        "0xfff0,0xfff2,0xfff1,0xfff3" },
                { "pre2",        "0xfff0,0xfff2,0xfff1,0xfff3" },
                { "pre3",        "0xfff0,0xfff2,0xfff1,0xfff3" },
                { "main",        "0x90,0x92,0x91,0x93"         },
                { "post1",       "0x10,0x12,0x11,0x13"         },
                { "post2",       "0x10,0x12,0x11,0x13"         },
                { "post3",       "0x10,0x12,0x11,0x13"         },
                { "attn",        "0x80,0x82,0x81,0x83"         },
                { "ob_m2lp",     "0x4,0x6,0x5,0x7"             },
                { "ob_alev_out", "0xf,0x11,0x10,0x12"          },
                { "obplev",      "0x69,0x6b,0x6a,0x6c"         },
                { "obnlev",      "0x5f,0x61,0x60,0x62"         },
                { "regn_bfm1p",  "0x1e,0x20,0x1f,0x21"         },
                { "regn_bfm1n",  "0xaa,0xac,0xab,0xad"         }
            }
        }};

        // Refill consumer
        auto consumer = dynamic_cast<Consumer*>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(kfvList);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();

        // Get port
        Port p;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", p));

        // Verify preemphasis
        std::vector<std::uint32_t> preemphasis = { 0xcad0, 0xc6e0, 0xc6e0, 0xd2b0 };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_PREEMPHASIS), preemphasis);

        // Verify idriver
        std::vector<std::uint32_t> idriver = { 0x5, 0x3, 0x4, 0x1 };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_IDRIVER), idriver);

        // Verify ipredriver
        std::vector<std::uint32_t> ipredriver = { 0x1, 0x4, 0x3, 0x5 };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_IPREDRIVER), ipredriver);

        // Verify pre1
        std::vector<std::uint32_t> pre1 = { 0xfff0, 0xfff2, 0xfff1, 0xfff3 };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_TX_FIR_PRE1), pre1);

        // Verify pre2
        std::vector<std::uint32_t> pre2 = { 0xfff0, 0xfff2, 0xfff1, 0xfff3 };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_TX_FIR_PRE2), pre2);

        // Verify pre3
        std::vector<std::uint32_t> pre3 = { 0xfff0, 0xfff2, 0xfff1, 0xfff3 };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_TX_FIR_PRE3), pre3);

        // Verify main
        std::vector<std::uint32_t> main = { 0x90, 0x92, 0x91, 0x93 };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_TX_FIR_MAIN), main);

        // Verify post1
        std::vector<std::uint32_t> post1 = { 0x10, 0x12, 0x11, 0x13 };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_TX_FIR_POST1), post1);

        // Verify post2
        std::vector<std::uint32_t> post2 = { 0x10, 0x12, 0x11, 0x13 };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_TX_FIR_POST2), post2);

        // Verify post3
        std::vector<std::uint32_t> post3 = { 0x10, 0x12, 0x11, 0x13 };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_TX_FIR_POST3), post3);

        // Verify attn
        std::vector<std::uint32_t> attn = { 0x80, 0x82, 0x81, 0x83 };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_TX_FIR_ATTN), attn);

        // Verify ob_m2lp
        std::vector<std::uint32_t> ob_m2lp = { 0x4, 0x6, 0x5, 0x7 };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_TX_PAM4_RATIO), ob_m2lp);

        // Verify ob_alev_out
        std::vector<std::uint32_t> ob_alev_out = { 0xf, 0x11, 0x10, 0x12 };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_TX_OUT_COMMON_MODE), ob_alev_out);

        // Verify obplev
        std::vector<std::uint32_t> obplev = { 0x69, 0x6b, 0x6a, 0x6c };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_TX_PMOS_COMMON_MODE), obplev);

        // Verify obnlev
        std::vector<std::uint32_t> obnlev = { 0x5f, 0x61, 0x60, 0x62 };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_TX_NMOS_COMMON_MODE), obnlev);

        // Verify regn_bfm1p
        std::vector<std::uint32_t> regn_bfm1p = { 0x1e, 0x20, 0x1f, 0x21 };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_TX_PMOS_VLTG_REG), regn_bfm1p);

        // Verify regn_bfm1n
        std::vector<std::uint32_t> regn_bfm1n = { 0xaa, 0xac, 0xab, 0xad };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_TX_NMOS_VLTG_REG), regn_bfm1n);

        // Dump pending tasks
        std::vector<std::string> taskList;
        gPortsOrch->dumpPendingTasks(taskList);
        ASSERT_TRUE(taskList.empty());

        // Cleanup ports
        cleanupPorts(gPortsOrch);
    }

    /**
     * Test that verifies admin-disable then admin-enable during setPortSerdesAttribute()
     */
    TEST_F(PortsOrchTest, PortSerdesConfig)
    {
        auto portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

        // Get SAI default ports
        auto &ports = defaultPortList;
        ASSERT_TRUE(!ports.empty());

        // Generate port config
        for (const auto &cit : ports)
        {
            portTable.set(cit.first, cit.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", std::to_string(ports.size()) } });

        // Refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();

        // Generate basic port config
        std::deque<KeyOpFieldsValuesTuple> kfvBasic = {{
            "Ethernet0",
            SET_COMMAND, {
                { "speed",               "100000"            },
                { "fec",                 "rs"                },
                { "mtu",                 "9100"              },
                { "admin_status",        "up"                }
            }
        }};

        // Refill consumer
        auto consumer = dynamic_cast<Consumer*>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(kfvBasic);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();

        // Get port and verify admin status
        Port p;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", p));
        ASSERT_TRUE(p.m_admin_state_up);

        // Generate port serdes config
        std::deque<KeyOpFieldsValuesTuple> kfvSerdes = {{
            "Ethernet0",
            SET_COMMAND, {
                { "idriver"     , "0x6,0x6,0x6,0x6" }
            }
        }};

        // Refill consumer
        consumer->addToSync(kfvSerdes);

        _hook_sai_port_api();
        uint32_t current_sai_api_call_count = _sai_set_admin_state_down_count;

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();

        _unhook_sai_port_api();

        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", p));
        ASSERT_TRUE(p.m_admin_state_up);

        // Verify idriver
        std::vector<std::uint32_t> idriver = { 0x6, 0x6, 0x6, 0x6 };
        ASSERT_EQ(p.m_preemphasis.at(SAI_PORT_SERDES_ATTR_IDRIVER), idriver);

        // Verify admin-disable then admin-enable
        ASSERT_EQ(_sai_set_admin_state_down_count, ++current_sai_api_call_count);
        ASSERT_EQ(_sai_set_admin_state_up_count, current_sai_api_call_count);

        // Configure non-serdes attribute that does not trigger admin state change
        std::deque<KeyOpFieldsValuesTuple> kfvMtu = {{
            "Ethernet0",
            SET_COMMAND, {
                { "mtu", "1234" },
            }
        }};

        // Refill consumer
        consumer->addToSync(kfvMtu);

        _hook_sai_port_api();
        current_sai_api_call_count = _sai_set_admin_state_down_count;

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();

        _unhook_sai_port_api();

        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", p));
        ASSERT_TRUE(p.m_admin_state_up);

        // Verify mtu is set
        ASSERT_EQ(p.m_mtu, 1234);

        // Verify no admin-disable then admin-enable
        ASSERT_EQ(_sai_set_admin_state_down_count, current_sai_api_call_count);
        ASSERT_EQ(_sai_set_admin_state_up_count, current_sai_api_call_count);

        // Dump pending tasks
        std::vector<std::string> taskList;
        gPortsOrch->dumpPendingTasks(taskList);
        ASSERT_TRUE(taskList.empty());

        // Cleanup ports
        cleanupPorts(gPortsOrch);
    }

    /**
     * Test that verifies PortsOrch::getPort() on a port that has been deleted
     */
    TEST_F(PortsOrchTest, GetPortTest)
    {
        _hook_sai_queue_api();
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        std::deque<KeyOpFieldsValuesTuple> entries;

        // Get SAI default ports to populate DB
        auto &ports = defaultPortList;
        ASSERT_TRUE(!ports.empty());

        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
        ASSERT_NE(port.m_port_id, SAI_NULL_OBJECT_ID);

        // Get queue info
        string type;
        uint8_t index;
        auto queue_id = port.m_queue_ids[0];
        auto ut_sai_get_queue_attr_count = _sai_get_queue_attr_count;
        gPortsOrch->getQueueTypeAndIndex(queue_id, type, index);
        ASSERT_EQ(type, "SAI_QUEUE_TYPE_UNICAST");
        ASSERT_EQ(index, 0);
        type = "";
        index = 255;
        gPortsOrch->getQueueTypeAndIndex(queue_id, type, index);
        ASSERT_EQ(type, "SAI_QUEUE_TYPE_UNICAST");
        ASSERT_EQ(index, 0);
        ASSERT_EQ(++ut_sai_get_queue_attr_count, _sai_get_queue_attr_count);

        // Delete port
        entries.push_back({"Ethernet0", "DEL", {}});
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gPortsOrch)->doTask();
        entries.clear();

        ASSERT_FALSE(gPortsOrch->getPort(port.m_port_id, port));
        ASSERT_EQ(gPortsOrch->m_queueInfo.find(queue_id), gPortsOrch->m_queueInfo.end());
        _unhook_sai_queue_api();
    }

    TEST_F(PortsOrchTest, PortPTConfigDefaultTimestampTemplate)
    {
        auto portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Port p;
        std::deque<KeyOpFieldsValuesTuple> kfvList;
        auto consumer = dynamic_cast<Consumer*>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));

        // Get SAI default ports to populate DB
        auto &ports = defaultPortList;
        ASSERT_TRUE(!ports.empty());

        // Generate port config
        for (const auto &cit : ports)
        {
            portTable.set(cit.first, cit.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", std::to_string(ports.size()) } });

        // Refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();

        // Port count: 32 Data + 1 CPU
        ASSERT_EQ(gPortsOrch->getAllPorts().size(), ports.size() + 1);

        // Get port
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet8", p));

        // Verify PT Interface ID
        ASSERT_EQ(p.m_pt_intf_id, 0);

        // Verify PT Timestamp Template
        ASSERT_EQ(p.m_pt_timestamp_template, SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_16_23);

        // Enable Path Tracing on Ethernet8 with Interface ID 128 and default Timestamp Template
        kfvList = {{
            "Ethernet8",
            SET_COMMAND, {
                { "pt_interface_id", "128" }
            }
        }};

        // Refill consumer
        consumer->addToSync(kfvList);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();
        kfvList.clear();

        // Get port
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet8", p));

        // Verify PT Interface ID
        ASSERT_EQ(p.m_pt_intf_id, 128);

        // Verify PT Timestamp Template
        ASSERT_EQ(p.m_pt_timestamp_template, SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_16_23);

        // Disable Path Tracing on Ethernet8
        kfvList = {{
            "Ethernet8",
            SET_COMMAND, {
                { "pt_interface_id", "None" },
                { "pt_timestamp_template", "None" }
            }
        }};

        // Refill consumer
        consumer->addToSync(kfvList);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();
        kfvList.clear();

        // Get port
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet8", p));

        // Verify PT Interface ID
        ASSERT_EQ(p.m_pt_intf_id, 0);

        // Verify PT Timestamp Template
        ASSERT_EQ(p.m_pt_timestamp_template, SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_16_23);

        // Dump pending tasks
        std::vector<std::string> taskList;
        gPortsOrch->dumpPendingTasks(taskList);
        ASSERT_TRUE(taskList.empty());
    }

    TEST_F(PortsOrchTest, PortPTConfigNonDefaultTimestampTemplate)
    {
        auto portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Port p;
        std::deque<KeyOpFieldsValuesTuple> kfvList;
        auto consumer = dynamic_cast<Consumer*>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));

        // Get SAI default ports
        auto &ports = defaultPortList;
        ASSERT_TRUE(!ports.empty());

        // Generate port config
        for (const auto &cit : ports)
        {
            portTable.set(cit.first, cit.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", std::to_string(ports.size()) } });

        // Refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();

        // Port count: 32 Data + 1 CPU
        ASSERT_EQ(gPortsOrch->getAllPorts().size(), ports.size() + 1);

        // Get port
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet9", p));

        // Verify PT Interface ID
        ASSERT_EQ(p.m_pt_intf_id, 0);

        // Verify PT Timestamp Template
        ASSERT_EQ(p.m_pt_timestamp_template, SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_16_23);

        // Enable Path Tracing on Ethernet9 with Interface ID 129 and Timestamp Template template2
        kfvList = {{
            "Ethernet9",
            SET_COMMAND, {
                { "pt_interface_id",       "129"       },
                { "pt_timestamp_template", "template2" }
            }
        }};

        // Refill consumer
        consumer->addToSync(kfvList);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();
        kfvList.clear();

        // Get port
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet9", p));

        // Verify PT Interface ID
        ASSERT_EQ(p.m_pt_intf_id, 129);

        // Verify PT Timestamp Template
        ASSERT_EQ(p.m_pt_timestamp_template, SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_12_19);

        // Disable Path Tracing on Ethernet9
        kfvList = {{
            "Ethernet9",
            SET_COMMAND, {
                { "pt_interface_id", "None" },
                { "pt_timestamp_template", "None" }
            }
        }};

        // Refill consumer
        consumer->addToSync(kfvList);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();
        kfvList.clear();

        // Get port
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet9", p));

        // Verify PT Interface ID
        ASSERT_EQ(p.m_pt_intf_id, 0);

        // Verify PT Timestamp Template
        ASSERT_EQ(p.m_pt_timestamp_template, SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_16_23);

        // Dump pending tasks
        std::vector<std::string> taskList;
        gPortsOrch->dumpPendingTasks(taskList);
        ASSERT_TRUE(taskList.empty());
    }

    TEST_F(PortsOrchTest, PortPTConfigInvalidInterfaceID)
    {
        auto portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Port p;
        std::deque<KeyOpFieldsValuesTuple> kfvList;
        auto consumer = dynamic_cast<Consumer*>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));

        // Get SAI default ports
        auto &ports = defaultPortList;
        ASSERT_TRUE(!ports.empty());

        // Generate port config
        for (const auto &cit : ports)
        {
            portTable.set(cit.first, cit.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", std::to_string(ports.size()) } });

        // Refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();

        // Port count: 32 Data + 1 CPU
        ASSERT_EQ(gPortsOrch->getAllPorts().size(), ports.size() + 1);

        // Get port
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet9", p));

        // Verify PT Interface ID
        ASSERT_EQ(p.m_pt_intf_id, 0);

        // Verify PT Timestamp Template
        ASSERT_EQ(p.m_pt_timestamp_template, SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_16_23);

        // Enable Path Tracing on Ethernet9 with Interface ID 4096 (INVALID) and Timestamp Template template2
        kfvList = {{
            "Ethernet9",
            SET_COMMAND, {
                { "pt_interface_id",       "4096"      },
                { "pt_timestamp_template", "template2" }
            }
        }};

        // Refill consumer
        consumer->addToSync(kfvList);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();
        kfvList.clear();

        // Get port
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet9", p));

        // Verify PT Interface ID
        // We provided an invalid Path Tracing Interface ID, therefore we expect PortsOrch rejects the port
        // configuration and Path Tracing remains disabled (i.e., Tracing Interface ID should be 0)
        ASSERT_EQ(p.m_pt_intf_id, 0);

        // Dump pending tasks
        std::vector<std::string> taskList;
        gPortsOrch->dumpPendingTasks(taskList);
        ASSERT_TRUE(taskList.empty());
    }

    TEST_F(PortsOrchTest, PortPTConfigInvalidInterfaceTimestampTemplate)
    {
        auto portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Port p;
        std::deque<KeyOpFieldsValuesTuple> kfvList;
        auto consumer = dynamic_cast<Consumer*>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));

        // Get SAI default ports
        auto &ports = defaultPortList;
        ASSERT_TRUE(!ports.empty());

        // Generate port config
        for (const auto &cit : ports)
        {
            portTable.set(cit.first, cit.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", std::to_string(ports.size()) } });

        // Refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();

        // Port count: 32 Data + 1 CPU
        ASSERT_EQ(gPortsOrch->getAllPorts().size(), ports.size() + 1);

        // Get port
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet9", p));

        // Verify PT Interface ID
        ASSERT_EQ(p.m_pt_intf_id, 0);

        // Verify PT Timestamp Template
        ASSERT_EQ(p.m_pt_timestamp_template, SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_16_23);

        // Enable Path Tracing on Ethernet9 with Interface ID 129 and Timestamp Template template5 (INVALID)
        kfvList = {{
            "Ethernet9",
            SET_COMMAND, {
                { "pt_interface_id",       "129"      },
                { "pt_timestamp_template", "template5" }
            }
        }};

        // Refill consumer
        consumer->addToSync(kfvList);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();
        kfvList.clear();

        // Get port
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet9", p));

        // Verify PT Interface ID
        // We provided an invalid Timestamp Template, therefore we expect PortsOrch rejects the port
        // configuration and Path Tracing remains disabled (i.e., Tracing Interface ID should be 0)
        ASSERT_EQ(p.m_pt_intf_id, 0);

        // Dump pending tasks
        std::vector<std::string> taskList;
        gPortsOrch->dumpPendingTasks(taskList);
        ASSERT_TRUE(taskList.empty());
    }

    TEST_F(PortsOrchTest, PortPTSAIFailureHandling)
    {
        _hook_sai_port_api();
        _hook_sai_switch_api();

        auto portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Port p;
        std::deque<KeyOpFieldsValuesTuple> kfvList;
        auto consumer = dynamic_cast<Consumer*>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));

        // Get SAI default ports
        auto &ports = defaultPortList;
        ASSERT_TRUE(!ports.empty());

        // Generate port config
        for (const auto &cit : ports)
        {
            portTable.set(cit.first, cit.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", std::to_string(ports.size()) } });

        // Refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();

        // Port count: 32 Data + 1 CPU
        ASSERT_EQ(gPortsOrch->getAllPorts().size(), ports.size() + 1);

        // Get port
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet9", p));

        // Verify PT Interface ID
        ASSERT_EQ(p.m_pt_intf_id, 0);

        // Verify PT Timestamp Template
        ASSERT_EQ(p.m_pt_timestamp_template, SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_16_23);

        // Simulate failure when PortsOrch attempts to set the Path Tracing Interface ID
        set_pt_interface_id_fail = true;

        // Enable Path Tracing on Ethernet9 with Interface ID 129 and Timestamp Template template2
        kfvList = {{
            "Ethernet9",
            SET_COMMAND, {
                { "pt_interface_id",       "129"       },
                { "pt_timestamp_template", "template2" }
            }
        }};

        // Refill consumer
        consumer->addToSync(kfvList);

        static_cast<Orch*>(gPortsOrch)->doTask();

        ASSERT_EQ(set_pt_interface_id_fail, 1);

        set_pt_interface_id_fail = false;


        // Simulate failure when PortsOrch attempts to set the Path Tracing Timestamp Template
        set_pt_timestamp_template_fail = true;

        // Enable Path Tracing on Ethernet10 with Interface ID 129 and Timestamp Template template2
        kfvList = {{
            "Ethernet10",
            SET_COMMAND, {
                { "pt_interface_id",       "129"       },
                { "pt_timestamp_template", "template2" }
            }
        }};

        // Refill consumer
        consumer->addToSync(kfvList);

        static_cast<Orch*>(gPortsOrch)->doTask();

        ASSERT_EQ(set_pt_timestamp_template_failures, 1);

        set_pt_timestamp_template_fail = false;


        // Simulate failure when PortsOrch attempts to set the port TAM object
        set_port_tam_fail = true;

        // Enable Path Tracing on Ethernet11 with Interface ID 129 and Timestamp Template template2
        kfvList = {{
            "Ethernet11",
            SET_COMMAND, {
                { "pt_interface_id",       "129"       },
                { "pt_timestamp_template", "template2" }
            }
        }};

        // Refill consumer
        consumer->addToSync(kfvList);

        static_cast<Orch*>(gPortsOrch)->doTask();

        ASSERT_EQ(set_port_tam_fail, 1);

        set_port_tam_fail = false;

        _unhook_sai_switch_api();
        _unhook_sai_port_api();
    }

    TEST_F(PortsOrchTest, PortPTCapabilityUnsupported)
    {
        _hook_sai_port_api();
        _hook_sai_switch_api();

        auto portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Port p;
        std::deque<KeyOpFieldsValuesTuple> kfvList;
        auto consumer = dynamic_cast<Consumer*>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));

        // Get SAI default ports
        auto &ports = defaultPortList;
        ASSERT_TRUE(!ports.empty());

        // Generate port config
        for (const auto &cit : ports)
        {
            portTable.set(cit.first, cit.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", std::to_string(ports.size()) } });

        // Refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration
        static_cast<Orch*>(gPortsOrch)->doTask();

        // Port count: 32 Data + 1 CPU
        ASSERT_EQ(gPortsOrch->getAllPorts().size(), ports.size() + 1);

        // Scenario 1: Path Tracing supported
        ASSERT_TRUE(gPortsOrch->checkPathTracingCapability());

        // Scenario 2: Path Tracing is not supported
        supported_sai_objects.erase(std::remove(supported_sai_objects.begin(), supported_sai_objects.end(), SAI_OBJECT_TYPE_TAM), supported_sai_objects.end());
        ASSERT_FALSE(gPortsOrch->checkPathTracingCapability());

        kfvList = {{
            "Ethernet10",
            SET_COMMAND, {
                { "pt_interface_id", "129"}
            }
        }};
        consumer->addToSync(kfvList);
        static_cast<Orch*>(gPortsOrch)->doTask();
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet10", p));
        // Expect Path Tracing Interface ID is ignored because Path Tracing is not supported on the switch
        ASSERT_EQ(p.m_pt_intf_id, 0);

        kfvList = {{
            "Ethernet10",
            SET_COMMAND, {
                { "pt_timestamp_template", "template2"}
            }
        }};
        consumer->addToSync(kfvList);
        static_cast<Orch*>(gPortsOrch)->doTask();
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet10", p));
        // Expect Path Tracing template is ignored because Path Tracing is not supported on the switch
        ASSERT_NE(p.m_pt_timestamp_template, SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_12_19);

        _unhook_sai_switch_api();
        _unhook_sai_port_api();
    }

    /**
     * Test case: PortsOrch::addBridgePort() does not add router port to .1Q bridge
     */
    TEST_F(PortsOrchTest, addBridgePortOnRouterPort)
    {
        _hook_sai_bridge_api();

        StrictMock<MockSaiBridge> mock_sai_bridge_;
        mock_sai_bridge = &mock_sai_bridge_;
        sai_bridge_api->create_bridge_port = mock_create_bridge_port;

        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);
        // Apply configuration : create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        // Get first port and set its rif id to simulate it is router port
        Port port;
        gPortsOrch->getPort("Ethernet0", port);
        port.m_rif_id = 1;

        ASSERT_FALSE(gPortsOrch->addBridgePort(port));
        EXPECT_CALL(mock_sai_bridge_, create_bridge_port(_, _, _, _)).Times(0);

        _unhook_sai_bridge_api();
    }

    TEST_F(PortsOrchTest, SupportedLinkEventDampingAlgorithmSuccess)
    {
        _hook_sai_port_api();
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        std::deque<KeyOpFieldsValuesTuple> entries;

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        uint32_t current_sai_api_call_count = _sai_set_link_event_damping_algorithm_count;

        entries.push_back({"Ethernet0", "SET",
                           {
                               {"link_event_damping_algorithm", "aied"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gPortsOrch)->doTask();
        entries.clear();

        // verify SAI call was made and set algorithm successfully
        ASSERT_EQ(_sai_set_link_event_damping_algorithm_count, ++current_sai_api_call_count);
        ASSERT_EQ(_sai_link_event_damping_algorithm, SAI_REDIS_LINK_EVENT_DAMPING_ALGORITHM_AIED);

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        _unhook_sai_port_api();
    }

    TEST_F(PortsOrchTest, SupportedLinkEventDampingAlgorithmFailure)
    {
        _hook_sai_port_api();
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        std::deque<KeyOpFieldsValuesTuple> entries;

        set_link_event_damping_success = false;
        _sai_link_event_damping_algorithm = SAI_REDIS_LINK_EVENT_DAMPING_ALGORITHM_DISABLED;

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        uint32_t current_sai_api_call_count = _sai_set_link_event_damping_algorithm_count;


        entries.push_back({"Ethernet0", "SET",
                           {
                               {"link_event_damping_algorithm", "aied"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gPortsOrch)->doTask();
        entries.clear();

        // Verify that SAI call was made, algorithm not set
        ASSERT_EQ(_sai_set_link_event_damping_algorithm_count, ++current_sai_api_call_count);
        ASSERT_EQ(_sai_link_event_damping_algorithm, SAI_REDIS_LINK_EVENT_DAMPING_ALGORITHM_DISABLED);

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        _unhook_sai_port_api();
    }

    TEST_F(PortsOrchTest, NotSupportedLinkEventDampingAlgorithm)
    {
        _hook_sai_port_api();
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        std::deque<KeyOpFieldsValuesTuple> entries;

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        uint32_t current_sai_api_call_count = _sai_set_link_event_damping_algorithm_count;

        entries.push_back({"Ethernet0", "SET",
                           {
                               {"link_event_damping_algorithm", "test_algo"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gPortsOrch)->doTask();
        entries.clear();

        // Verify that no SAI call was made
        ASSERT_EQ(_sai_set_link_event_damping_algorithm_count, current_sai_api_call_count);
        ASSERT_EQ(_sai_link_event_damping_algorithm, SAI_REDIS_LINK_EVENT_DAMPING_ALGORITHM_DISABLED);

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        _unhook_sai_port_api();
    }

    TEST_F(PortsOrchTest, SetLinkEventDampingFullConfigSuccess) {
        _hook_sai_port_api();
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        std::deque<KeyOpFieldsValuesTuple> entries;

        set_link_event_damping_success = true;
        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        uint32_t current_sai_api_call_count = _sai_set_link_event_damping_config_count;

        entries.push_back({"Ethernet0", "SET",
                           {
                               {"max_suppress_time", "64000"},
                               {"decay_half_life", "45000"},
                               {"suppress_threshold", "1650"},
                               {"reuse_threshold", "1500"},
                               {"flap_penalty", "1000"},
                           }});
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gPortsOrch)->doTask();
        entries.clear();

        ASSERT_EQ(_sai_set_link_event_damping_config_count, ++current_sai_api_call_count);
        ASSERT_EQ(_sai_link_event_damping_config.max_suppress_time, 64000);
        ASSERT_EQ(_sai_link_event_damping_config.decay_half_life, 45000);
        ASSERT_EQ(_sai_link_event_damping_config.suppress_threshold, 1650);
        ASSERT_EQ(_sai_link_event_damping_config.reuse_threshold, 1500);
        ASSERT_EQ(_sai_link_event_damping_config.flap_penalty, 1000);

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        _unhook_sai_port_api();
    }

    TEST_F(PortsOrchTest, SetLinkEventDampingPartialConfigSuccess) {
        _hook_sai_port_api();
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        std::deque<KeyOpFieldsValuesTuple> entries;

        _sai_link_event_damping_config = {0, 0, 0, 0, 0};

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        uint32_t current_sai_api_call_count = _sai_set_link_event_damping_config_count;

        entries.push_back({"Ethernet0", "SET",
                           {
                               {"decay_half_life", "30000"},
                               {"reuse_threshold", "1200"},
                           }});
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gPortsOrch)->doTask();
        entries.clear();

        ASSERT_EQ(_sai_set_link_event_damping_config_count, ++current_sai_api_call_count);
        ASSERT_EQ(_sai_link_event_damping_config.max_suppress_time, 0);
        ASSERT_EQ(_sai_link_event_damping_config.decay_half_life, 30000);
        ASSERT_EQ(_sai_link_event_damping_config.suppress_threshold, 0);
        ASSERT_EQ(_sai_link_event_damping_config.reuse_threshold, 1200);
        ASSERT_EQ(_sai_link_event_damping_config.flap_penalty, 0);

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        _unhook_sai_port_api();
    }

    TEST_F(PortsOrchTest, SetLinkEventDampingConfigFailure) {
        _hook_sai_port_api();
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        std::deque<KeyOpFieldsValuesTuple> entries;

        set_link_event_damping_success = false;
        _sai_link_event_damping_config = {0, 0, 0, 0, 0};

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        uint32_t current_sai_api_call_count = _sai_set_link_event_damping_config_count;

        entries.push_back({"Ethernet0", "SET",
                           {
                               {"max_suppress_time", "64000"},
                               {"decay_half_life", "45000"},
                               {"suppress_threshold", "1650"},
                               {"reuse_threshold", "1500"},
                               {"flap_penalty", "1000"},
                           }});
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gPortsOrch)->doTask();
        entries.clear();

        // Verify that config is not set
        ASSERT_EQ(_sai_set_link_event_damping_config_count, ++current_sai_api_call_count);
        ASSERT_EQ(_sai_link_event_damping_config.max_suppress_time, 0);
        ASSERT_EQ(_sai_link_event_damping_config.decay_half_life, 0);
        ASSERT_EQ(_sai_link_event_damping_config.suppress_threshold, 0);
        ASSERT_EQ(_sai_link_event_damping_config.reuse_threshold, 0);
        ASSERT_EQ(_sai_link_event_damping_config.flap_penalty, 0);

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        _unhook_sai_port_api();
    }

    TEST_F(PortsOrchTest, PortSupportedFecModes)
    {
        _hook_sai_port_api();
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        std::deque<KeyOpFieldsValuesTuple> entries;

        not_support_fetching_fec = false;
        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        uint32_t current_sai_api_call_count = _sai_set_port_fec_count;

        entries.push_back({"Ethernet0", "SET",
                           {
                               {"fec", "rs"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gPortsOrch)->doTask();
        entries.clear();

        ASSERT_EQ(_sai_set_port_fec_count, ++current_sai_api_call_count);
        ASSERT_EQ(_sai_port_fec_mode, SAI_PORT_FEC_MODE_RS);

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        entries.push_back({"Ethernet0", "SET",
                           {
                               {"fec", "none"}
                           }});
        consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gPortsOrch)->doTask();

        ASSERT_EQ(_sai_set_port_fec_count, current_sai_api_call_count);
        ASSERT_EQ(_sai_port_fec_mode, SAI_PORT_FEC_MODE_RS);

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_EQ(ts.size(), 0);

        _unhook_sai_port_api();
    }

    /*
     * Test case: SAI_PORT_ATTR_SUPPORTED_FEC_MODE is not supported by vendor
     **/
    TEST_F(PortsOrchTest, PortNotSupportedFecModes)
    {
        _hook_sai_port_api();
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        std::deque<KeyOpFieldsValuesTuple> entries;

        not_support_fetching_fec = true;
        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        uint32_t current_sai_api_call_count = _sai_set_port_fec_count;

        entries.push_back({"Ethernet0", "SET",
                           {
                               {"fec", "rs"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gPortsOrch)->doTask();
        entries.clear();

        ASSERT_EQ(_sai_set_port_fec_count, ++current_sai_api_call_count);
        ASSERT_EQ(_sai_port_fec_mode, SAI_PORT_FEC_MODE_RS);

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        _unhook_sai_port_api();
    }

    /*
     * Test case: Fetching SAI_PORT_ATTR_SUPPORTED_FEC_MODE is supported but no FEC mode is supported on the port
     **/
    TEST_F(PortsOrchTest, PortSupportNoFecModes)
    {
        _hook_sai_port_api();
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        std::deque<KeyOpFieldsValuesTuple> entries;

        not_support_fetching_fec = false;
        auto old_mock_port_fec_modes = mock_port_fec_modes;
        mock_port_fec_modes.clear();
        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        uint32_t current_sai_api_call_count = _sai_set_port_fec_count;

        entries.push_back({"Ethernet0", "SET",
                           {
                               {"fec", "rs"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gPortsOrch)->doTask();
        entries.clear();

        ASSERT_EQ(_sai_set_port_fec_count, current_sai_api_call_count);

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        mock_port_fec_modes = old_mock_port_fec_modes;
        _unhook_sai_port_api();
    }

    /*
     * Test case: Fetching SAI_PORT_ATTR_OPER_PORT_FEC_MODE
     **/
    TEST_F(PortsOrchTest, PortVerifyOperFec)
    {
        _hook_sai_port_api();
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table statePortTable = Table(m_state_db.get(), STATE_PORT_TABLE_NAME);
        std::deque<KeyOpFieldsValuesTuple> entries;

        not_support_fetching_fec = false;
        auto old_mock_port_fec_modes = mock_port_fec_modes;
        mock_port_fec_modes.clear();
        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        uint32_t current_sai_api_call_count = _sai_set_port_fec_count;
        gPortsOrch->oper_fec_sup = true;

        entries.push_back({"Ethernet0", "SET",
                           {
                               {"fec", "rs"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gPortsOrch)->doTask();
        entries.clear();

        ASSERT_EQ(_sai_set_port_fec_count, current_sai_api_call_count);

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        Port port;
        gPortsOrch->getPort("Ethernet0", port);

        sai_port_fec_mode_t fec_mode;
        gPortsOrch->getPortOperFec(port, fec_mode);

        ASSERT_EQ(fec_mode, SAI_PORT_FEC_MODE_RS);

        gPortsOrch->refreshPortStatus();
        std::vector<FieldValueTuple> values;
        statePortTable.get("Ethernet0", values);
        bool fec_found = false;
        for (auto &valueTuple : values)
        {
            if (fvField(valueTuple) == "fec")
            {
                fec_found = true;
                ASSERT_TRUE(fvValue(valueTuple) == "rs");
            }
        }
        ASSERT_TRUE(fec_found == true);

        /*Mock an invalid fec mode with high value*/
        _sai_port_fec_mode = 100;
        gPortsOrch->refreshPortStatus();
        statePortTable.get("Ethernet0", values);
        fec_found = false;
        for (auto &valueTuple : values)
        {
            if (fvField(valueTuple) == "fec")
            {
                fec_found = true;
                ASSERT_TRUE(fvValue(valueTuple) == "N/A");
            }
        }
        mock_port_fec_modes = old_mock_port_fec_modes;
        _unhook_sai_port_api();
    }
    TEST_F(PortsOrchTest, PortTestSAIFailureHandling)
    {
        _hook_sai_port_api();
        _hook_sai_switch_api();
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        std::deque<KeyOpFieldsValuesTuple> entries;

        not_support_fetching_fec = false;
        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        _sai_syncd_notifications_count = (uint32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        _sai_syncd_notification_event = (int32_t*)mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        *_sai_syncd_notifications_count = 0;

        entries.push_back({"Ethernet0", "SET",
                           {
                               {"autoneg", "on"}
                           }});
        auto consumer = dynamic_cast<Consumer *>(gPortsOrch->getExecutor(APP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        ASSERT_DEATH({static_cast<Orch *>(gPortsOrch)->doTask();}, "");

        ASSERT_EQ(*_sai_syncd_notifications_count, 1);
        ASSERT_EQ(*_sai_syncd_notification_event, SAI_REDIS_NOTIFY_SYNCD_INVOKE_DUMP);
        _unhook_sai_port_api();
        _unhook_sai_switch_api();
    }

    TEST_F(PortsOrchTest, PortReadinessColdBoot)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table sendToIngressPortTable = Table(m_app_db.get(), APP_SEND_TO_INGRESS_PORT_TABLE_NAME);
        Table pgTable = Table(m_app_db.get(), APP_BUFFER_PG_TABLE_NAME);
        Table pgTableCfg = Table(m_config_db.get(), CFG_BUFFER_PG_TABLE_NAME);
        Table profileTable = Table(m_app_db.get(), APP_BUFFER_PROFILE_TABLE_NAME);
        Table poolTable = Table(m_app_db.get(), APP_BUFFER_POOL_TABLE_NAME);

        // Get SAI default ports to populate DB

        auto ports = ut_helper::getInitialSaiPorts();

        // Create test buffer pool
        poolTable.set(
            "test_pool",
            {
                { "type", "ingress" },
                { "mode", "dynamic" },
                { "size", "4200000" },
            });

        // Create test buffer profile
        profileTable.set("test_profile", { { "pool", "test_pool" },
                                           { "xon", "14832" },
                                           { "xoff", "14832" },
                                           { "size", "35000" },
                                           { "dynamic_th", "0" } });

        // Apply profile on PGs 3-4 all ports
        for (const auto &it : ports)
        {
            std::ostringstream ossAppl, ossCfg;
            ossAppl << it.first << ":3-4";
            pgTable.set(ossAppl.str(), { { "profile", "test_profile" } });
            ossCfg << it.first << "|3-4";
            pgTableCfg.set(ossCfg.str(), { { "profile", "test_profile" } });
        }

        // Recreate buffer orch to read populated data
        vector<string> buffer_tables = { APP_BUFFER_POOL_TABLE_NAME,
                                         APP_BUFFER_PROFILE_TABLE_NAME,
                                         APP_BUFFER_QUEUE_TABLE_NAME,
                                         APP_BUFFER_PG_TABLE_NAME,
                                         APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                                         APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME };

        gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);

        // Populate pot table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        // Populate send to ingresss port table
        sendToIngressPortTable.set("SEND_TO_INGRESS", {{"NULL", "NULL"}});

        // refill consumer
        gPortsOrch->addExistingData(&portTable);
        gBufferOrch->addExistingData(&pgTable);
        gBufferOrch->addExistingData(&poolTable);
        gBufferOrch->addExistingData(&profileTable);

        // Apply configuration :
        //  create ports

        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gPortsOrch)->doTask();

        // Ports are not ready yet

        ASSERT_FALSE(gPortsOrch->allPortsReady());

        // Ports host interfaces are created

        portTable.set("PortInitDone", { { "lanes", "0" } });
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration
        //  configure buffers
        //          ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        // Since init done is set now, apply buffers
        static_cast<Orch *>(gBufferOrch)->doTask();

        // Ports are not ready yet, mtu, speed left
        ASSERT_FALSE(gPortsOrch->allPortsReady());

        static_cast<Orch *>(gPortsOrch)->doTask();
        ASSERT_TRUE(gPortsOrch->allPortsReady());

        // No more tasks

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        ts.clear();

        gBufferOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
    }

    TEST_F(PortsOrchTest, PortReadinessWarmBoot)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table pgTable = Table(m_app_db.get(), APP_BUFFER_PG_TABLE_NAME);
        Table profileTable = Table(m_app_db.get(), APP_BUFFER_PROFILE_TABLE_NAME);
        Table poolTable = Table(m_app_db.get(), APP_BUFFER_POOL_TABLE_NAME);
        Table transceieverInfoTable = Table(m_state_db.get(), STATE_TRANSCEIVER_INFO_TABLE_NAME);

        // Get SAI default ports to populate DB

        auto ports = ut_helper::getInitialSaiPorts();

        // Create test buffer pool
        poolTable.set(
            "test_pool",
            {
                { "type", "ingress" },
                { "mode", "dynamic" },
                { "size", "4200000" },
            });

        // Create test buffer profile
        profileTable.set("test_profile", { { "pool", "test_pool" },
                                           { "xon", "14832" },
                                           { "xoff", "14832" },
                                           { "size", "35000" },
                                           { "dynamic_th", "0" } });

        // Apply profile on PGs 3-4 all ports
        for (const auto &it : ports)
        {
            std::ostringstream oss;
            oss << it.first << ":3-4";
            pgTable.set(oss.str(), { { "profile", "test_profile" } });
        }

        // Populate pot table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
            transceieverInfoTable.set(it.first, {});
        }

        // Set PortConfigDone, PortInitDone

        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        // warm start, initialize ports ready list

        WarmStart::getInstance().m_enabled = true;
        gBufferOrch->initBufferReadyLists(m_app_db.get(), m_config_db.get());
        WarmStart::getInstance().m_enabled = false;

        // warm start, bake fill refill consumer

        gBufferOrch->bake();
        gPortsOrch->bake();

        // Create ports, BufferOrch skips processing
        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gPortsOrch)->doTask();

        // Ports should not be ready here, buffers not applied,
        // BufferOrch depends on ports to be created

        ASSERT_FALSE(gPortsOrch->allPortsReady());

        // Drain remaining

        static_cast<Orch *>(gBufferOrch)->doTask();
        static_cast<Orch *>(gPortsOrch)->doTask();

        // Now ports should be ready

        ASSERT_TRUE(gPortsOrch->allPortsReady());

        // No more tasks

        vector<string> ts;

        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        ts.clear();

        gBufferOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        // Verify port configuration
        vector<sai_object_id_t> port_list;
        port_list.resize(ports.size());
        sai_attribute_t attr;
        sai_status_t status;
        attr.id = SAI_SWITCH_ATTR_PORT_LIST;
        attr.value.objlist.count = static_cast<uint32_t>(port_list.size());
        attr.value.objlist.list = port_list.data();
        status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
        ASSERT_EQ(status, SAI_STATUS_SUCCESS);

        for (uint32_t i = 0; i < port_list.size(); i++)
        {
            attr.id = SAI_PORT_ATTR_HOST_TX_SIGNAL_ENABLE;
            status = sai_port_api->get_port_attribute(port_list[i], 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            ASSERT_TRUE(attr.value.booldata);
        }
    }

    TEST_F(PortsOrchTest, PfcDlrHandlerCallingDlrInitAttribute)
    {
        _hook_sai_port_api();
        _hook_sai_queue_api();
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table pgTable = Table(m_app_db.get(), APP_BUFFER_PG_TABLE_NAME);
        Table profileTable = Table(m_app_db.get(), APP_BUFFER_PROFILE_TABLE_NAME);
        Table poolTable = Table(m_app_db.get(), APP_BUFFER_POOL_TABLE_NAME);
        Table queueTable = Table(m_app_db.get(), APP_BUFFER_QUEUE_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports

        static_cast<Orch *>(gPortsOrch)->doTask();

        // Apply configuration
        //          ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        ASSERT_TRUE(gPortsOrch->allPortsReady());

        // No more tasks
        vector<string> ts;
        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ts.clear();

        // Simulate storm drop handler started on Ethernet0 TC 3
        Port port;
        gPortsOrch->getPort("Ethernet0", port);
	auto current_pfc_mode_count = _sai_set_pfc_mode_count;
        auto countersTable = make_shared<Table>(m_counters_db.get(), COUNTERS_TABLE);
        auto dropHandler = make_unique<PfcWdDlrHandler>(port.m_port_id, port.m_queue_ids[3], 3, countersTable);
	ASSERT_EQ(current_pfc_mode_count, _sai_set_pfc_mode_count);
        ASSERT_TRUE(_sai_set_queue_attr_count == 1);

        dropHandler.reset();
	ASSERT_EQ(current_pfc_mode_count, _sai_set_pfc_mode_count);
        ASSERT_FALSE(_sai_set_queue_attr_count == 1);

        _unhook_sai_queue_api();
	_unhook_sai_port_api();
    }

    TEST_F(PortsOrchTest, PfcDlrPacketAction)
    {
	_hook_sai_switch_api();
	std::deque<KeyOpFieldsValuesTuple> entries;
	sai_packet_action_t dlr_packet_action;
	gSwitchOrch->m_PfcDlrInitEnable = true;
        gPfcwdOrch<PfcWdDlrHandler, PfcWdDlrHandler>->m_platform = BRCM_PLATFORM_SUBSTRING;
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
	Table cfgPfcwdTable = Table(m_config_db.get(), CFG_PFC_WD_TABLE_NAME);
	Table cfgPortQosMapTable = Table(m_config_db.get(), CFG_PORT_QOS_MAP_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports

        static_cast<Orch *>(gPortsOrch)->doTask();

        // Apply configuration
        //          ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        ASSERT_TRUE(gPortsOrch->allPortsReady());

        // No more tasks
        vector<string> ts;
        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ts.clear();

        entries.clear();
	entries.push_back({"Ethernet0", "SET",
			    {
			      {"pfc_enable", "3,4"},
			      {"pfcwd_sw_enable", "3,4"}
			  }});
	entries.push_back({"Ethernet8", "SET",
			    {
			      {"pfc_enable", "3,4"},
			      {"pfcwd_sw_enable", "3,4"}
			  }});
        auto portQosMapConsumer = dynamic_cast<Consumer *>(gQosOrch->getExecutor(CFG_PORT_QOS_MAP_TABLE_NAME));
        portQosMapConsumer->addToSync(entries);
        entries.clear();
	static_cast<Orch *>(gQosOrch)->doTask();

        // create pfcwd entry for first port with drop action
	dlr_packet_action = SAI_PACKET_ACTION_DROP;
	entries.push_back({"GLOBAL", "SET",
			  {
			    {"POLL_INTERVAL", "200"},
			  }});
	entries.push_back({"Ethernet0", "SET",
			  {
			    {"action", "drop"},
			    {"detection_time", "200"},
			    {"restoration_time", "200"}
			  }});

        auto PfcwdConsumer = dynamic_cast<Consumer *>(gPfcwdOrch<PfcWdDlrHandler, PfcWdDlrHandler>->getExecutor(CFG_PFC_WD_TABLE_NAME));
	PfcwdConsumer->addToSync(entries);
        entries.clear();

        auto current_switch_dlr_packet_action_count = _sai_switch_dlr_packet_action_count;
        static_cast<Orch *>(gPfcwdOrch<PfcWdDlrHandler, PfcWdDlrHandler>)->doTask();
	ASSERT_EQ(++current_switch_dlr_packet_action_count, _sai_switch_dlr_packet_action_count);
        ASSERT_EQ(_sai_switch_dlr_packet_action, dlr_packet_action);
        ASSERT_EQ((gPfcwdOrch<PfcWdDlrHandler, PfcWdDlrHandler>->m_pfcwd_ports.size()), 1);

	// create pfcwd entry for second port with drop action
	entries.push_back({"Ethernet8", "SET",
			  {
			    {"action", "drop"},
			    {"detection_time", "200"},
			    {"restoration_time", "200"}
			  }});
	PfcwdConsumer->addToSync(entries);
        entries.clear();
        current_switch_dlr_packet_action_count = _sai_switch_dlr_packet_action_count;
        static_cast<Orch *>(gPfcwdOrch<PfcWdDlrHandler, PfcWdDlrHandler>)->doTask();
	// verify no change in count
	ASSERT_EQ(current_switch_dlr_packet_action_count, _sai_switch_dlr_packet_action_count);

        // remove both the entries
        entries.push_back({"Ethernet0", "DEL",
                           {{}}
                          });
	PfcwdConsumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gPfcwdOrch<PfcWdDlrHandler, PfcWdDlrHandler>)->doTask();
        ASSERT_EQ((gPfcwdOrch<PfcWdDlrHandler, PfcWdDlrHandler>->m_pfcwd_ports.size()), 1);

        entries.push_back({"Ethernet8", "DEL",
                           {{}}
                          });
	PfcwdConsumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gPfcwdOrch<PfcWdDlrHandler, PfcWdDlrHandler>)->doTask();

        // create pfcwd entry for first port with forward action
	dlr_packet_action = SAI_PACKET_ACTION_FORWARD;
	entries.push_back({"Ethernet0", "SET",
			  {
			    {"action", "forward"},
			    {"detection_time", "200"},
			    {"restoration_time", "200"}
			  }});

	PfcwdConsumer->addToSync(entries);
        entries.clear();

        current_switch_dlr_packet_action_count = _sai_switch_dlr_packet_action_count;
        static_cast<Orch *>(gPfcwdOrch<PfcWdDlrHandler, PfcWdDlrHandler>)->doTask();
	ASSERT_EQ(++current_switch_dlr_packet_action_count, _sai_switch_dlr_packet_action_count);
        ASSERT_EQ(_sai_switch_dlr_packet_action, dlr_packet_action);
        ASSERT_EQ((gPfcwdOrch<PfcWdDlrHandler, PfcWdDlrHandler>->m_pfcwd_ports.size()), 1);

        // remove the entry
        entries.push_back({"Ethernet0", "DEL",
                           {{}}
                          });
	PfcwdConsumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gPfcwdOrch<PfcWdDlrHandler, PfcWdDlrHandler>)->doTask();
        ASSERT_EQ((gPfcwdOrch<PfcWdDlrHandler, PfcWdDlrHandler>->m_pfcwd_ports.size()), 0);

	_unhook_sai_switch_api();
    }

    TEST_F(PortsOrchTest, PfcZeroBufferHandler)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table pgTable = Table(m_app_db.get(), APP_BUFFER_PG_TABLE_NAME);
        Table profileTable = Table(m_app_db.get(), APP_BUFFER_PROFILE_TABLE_NAME);
        Table poolTable = Table(m_app_db.get(), APP_BUFFER_POOL_TABLE_NAME);
        Table queueTable = Table(m_app_db.get(), APP_BUFFER_QUEUE_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);

        // Apply configuration :
        //  create ports

        static_cast<Orch *>(gPortsOrch)->doTask();

        // Apply configuration
        //          ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        ASSERT_TRUE(gPortsOrch->allPortsReady());

        // No more tasks
        vector<string> ts;
        gPortsOrch->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());
        ts.clear();

        // Simulate storm drop handler started on Ethernet0 TC 3
        Port port;
        gPortsOrch->getPort("Ethernet0", port);

        auto countersTable = make_shared<Table>(m_counters_db.get(), COUNTERS_TABLE);
        auto dropHandler = make_unique<PfcWdZeroBufferHandler>(port.m_port_id, port.m_queue_ids[3], 3, countersTable);

        // Create test buffer pool
        poolTable.set(
            "egress_pool",
            {
                { "type", "egress" },
                { "mode", "dynamic" },
                { "size", "4200000" },
            });
        poolTable.set(
            "ingress_pool",
            {
                { "type", "ingress" },
                { "mode", "dynamic" },
                { "size", "4200000" },
            });

        // Create test buffer profile
        profileTable.set("ingress_profile", { { "pool", "ingress_pool" },
                                              { "xon", "14832" },
                                              { "xoff", "14832" },
                                              { "size", "35000" },
                                              { "dynamic_th", "0" } });
        profileTable.set("egress_profile", { { "pool", "egress_pool" },
                                             { "size", "0" },
                                             { "dynamic_th", "0" } });

        // Apply profile on Queue and PGs 3-4 all ports
        for (const auto &it : ports)
        {
            std::ostringstream oss;
            oss << it.first << ":3-4";
            pgTable.set(oss.str(), { { "profile", "ingress_profile" } });
            queueTable.set(oss.str(), { {"profile", "egress_profile" } });
        }
        gBufferOrch->addExistingData(&pgTable);
        gBufferOrch->addExistingData(&poolTable);
        gBufferOrch->addExistingData(&profileTable);
        gBufferOrch->addExistingData(&queueTable);

        // process pool, profile and Q's
        static_cast<Orch *>(gBufferOrch)->doTask();

        auto queueConsumer = static_cast<Consumer*>(gBufferOrch->getExecutor(APP_BUFFER_QUEUE_TABLE_NAME));
        queueConsumer->dumpPendingTasks(ts);
        ASSERT_FALSE(ts.empty()); // Queue is skipped
        ts.clear();

        auto pgConsumer = static_cast<Consumer*>(gBufferOrch->getExecutor(APP_BUFFER_PG_TABLE_NAME));
        pgConsumer->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty()); // PG Notification is not skipped
        ts.clear();

        // release zero buffer drop handler
        dropHandler.reset();

        // process queue
        static_cast<Orch *>(gBufferOrch)->doTask();

        queueConsumer->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty()); // queue should be processed now
        ts.clear();
    }

    /* This test checks that a LAG member validation happens on orchagent level
     * and no SAI call is executed in case a port requested to be a LAG member
     * is already a LAG member.
     */
    TEST_F(PortsOrchTest, LagMemberDoesNotCallSAIApiWhenPortIsAlreadyALagMember)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table lagTable = Table(m_app_db.get(), APP_LAG_TABLE_NAME);
        Table lagMemberTable = Table(m_app_db.get(), APP_LAG_MEMBER_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        /*
         * Next we will prepare some configuration data to be consumed by PortsOrch
         * 32 Ports, 2 LAGs, 1 port is LAG member.
         */

        // Populate pot table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { } });

        lagTable.set("PortChannel999",
            {
                {"admin_status", "up"},
                {"mtu", "9100"}
            }
        );
        lagTable.set("PortChannel0001",
            {
                {"admin_status", "up"},
                {"mtu", "9100"}
            }
        );
        lagMemberTable.set(
            std::string("PortChannel999") + lagMemberTable.getTableNameSeparator() + ports.begin()->first,
            { {"status", "enabled"} });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);
        gPortsOrch->addExistingData(&lagTable);
        gPortsOrch->addExistingData(&lagMemberTable);

        static_cast<Orch *>(gPortsOrch)->doTask();

        // check LAG, VLAN tasks were processed
        // port table may require one more doTask iteration
        for (auto tableName: {APP_LAG_TABLE_NAME, APP_LAG_MEMBER_TABLE_NAME})
        {
            vector<string> ts;
            auto exec = gPortsOrch->getExecutor(tableName);
            auto consumer = static_cast<Consumer*>(exec);
            ts.clear();
            consumer->dumpPendingTasks(ts);
            ASSERT_TRUE(ts.empty());
        }

        // Set first port as a LAG member while this port is still a member of different LAG.
        lagMemberTable.set(
            std::string("PortChannel0001") + lagMemberTable.getTableNameSeparator() + ports.begin()->first,
            { {"status", "enabled"} });

        // save original api since we will spy
        auto orig_lag_api = sai_lag_api;
        sai_lag_api = new sai_lag_api_t();
        memcpy(sai_lag_api, orig_lag_api, sizeof(*sai_lag_api));

        bool lagMemberCreateCalled = false;

        auto lagSpy = SpyOn<SAI_API_LAG, SAI_OBJECT_TYPE_LAG_MEMBER>(&sai_lag_api->create_lag_member);
        lagSpy->callFake([&](sai_object_id_t *oid, sai_object_id_t swoid, uint32_t count, const sai_attribute_t * attrs) -> sai_status_t
            {
                lagMemberCreateCalled = true;
                return orig_lag_api->create_lag_member(oid, swoid, count, attrs);
            }
        );

        gPortsOrch->addExistingData(&lagMemberTable);

        static_cast<Orch *>(gPortsOrch)->doTask();
        sai_lag_api = orig_lag_api;

        // verify there is a pending task to do.
        vector<string> ts;
        auto exec = gPortsOrch->getExecutor(APP_LAG_MEMBER_TABLE_NAME);
        auto consumer = static_cast<Consumer*>(exec);
        ts.clear();
        consumer->dumpPendingTasks(ts);
        ASSERT_FALSE(ts.empty());

        // verify there was no SAI call executed.
        ASSERT_FALSE(lagMemberCreateCalled);
    }

    /*
    * The scope of this test is a negative test which verify that:
    * if port operational status is up but operational speed is 0, the port speed should not be
    * updated to DB.
    */
    TEST_F(PortsOrchTest, PortOperStatusIsUpAndOperSpeedIsZero)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        // Populate port table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone, PortInitDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { "lanes", "0" } });

        // refill consumer
        gPortsOrch->addExistingData(&portTable);
        // Apply configuration : create ports
        static_cast<Orch *>(gPortsOrch)->doTask();

        // Get first port, expect the oper status is not UP
        Port port;
        gPortsOrch->getPort("Ethernet0", port);
        ASSERT_TRUE(port.m_oper_status != SAI_PORT_OPER_STATUS_UP);

        // save original api since we will spy
        auto orig_port_api = sai_port_api;
        sai_port_api = new sai_port_api_t();
        memcpy(sai_port_api, orig_port_api, sizeof(*sai_port_api));

        // mock SAI API sai_port_api->get_port_attribute
        auto portSpy = SpyOn<SAI_API_PORT, SAI_OBJECT_TYPE_PORT>(&sai_port_api->get_port_attribute);
        portSpy->callFake([&](sai_object_id_t oid, uint32_t count, sai_attribute_t * attrs) -> sai_status_t {
                if (attrs[0].id == SAI_PORT_ATTR_OPER_STATUS)
                {
                    attrs[0].value.u32 = (uint32_t)SAI_PORT_OPER_STATUS_UP;
                }
                else if (attrs[0].id == SAI_PORT_ATTR_OPER_SPEED)
                {
                    // Return 0 for port operational speed
                    attrs[0].value.u32 = 0;
                }

                return (sai_status_t)SAI_STATUS_SUCCESS;
            }
        );

        auto exec = static_cast<Notifier *>(gPortsOrch->getExecutor("PORT_STATUS_NOTIFICATIONS"));
        auto consumer = exec->getNotificationConsumer();

        // mock a redis reply for notification, it notifies that Ehernet0 is going to up
        mockReply = (redisReply *)calloc(sizeof(redisReply), 1);
        mockReply->type = REDIS_REPLY_ARRAY;
        mockReply->elements = 3; // REDIS_PUBLISH_MESSAGE_ELEMNTS
        mockReply->element = (redisReply **)calloc(sizeof(redisReply *), mockReply->elements);
        mockReply->element[2] = (redisReply *)calloc(sizeof(redisReply), 1);
        mockReply->element[2]->type = REDIS_REPLY_STRING;
        sai_port_oper_status_notification_t port_oper_status;
        port_oper_status.port_id = port.m_port_id;
        port_oper_status.port_state = SAI_PORT_OPER_STATUS_UP;
        std::string data = sai_serialize_port_oper_status_ntf(1, &port_oper_status);
        std::vector<FieldValueTuple> notifyValues;
        FieldValueTuple opdata("port_state_change", data);
        notifyValues.push_back(opdata);
        std::string msg = swss::JSon::buildJson(notifyValues);
        mockReply->element[2]->str = (char*)calloc(1, msg.length() + 1);
        memcpy(mockReply->element[2]->str, msg.c_str(), msg.length());

        // trigger the notification
        consumer->readData();
        gPortsOrch->doTask(*consumer);
        mockReply = nullptr;

        gPortsOrch->getPort("Ethernet0", port);
        ASSERT_TRUE(port.m_oper_status == SAI_PORT_OPER_STATUS_UP);
        ASSERT_TRUE(port.m_flap_count == 1);

        std::vector<FieldValueTuple> values;
        portTable.get("Ethernet0", values);
        for (auto &valueTuple : values)
        {
            if (fvField(valueTuple) == "speed")
            {
                ASSERT_TRUE(fvValue(valueTuple) != "0");
            }
        }

        gPortsOrch->refreshPortStatus();
        for (const auto &it : ports)
        {
            gPortsOrch->getPort(it.first, port);
            ASSERT_TRUE(port.m_oper_status == SAI_PORT_OPER_STATUS_UP);

            std::vector<FieldValueTuple> values;
            portTable.get(it.first, values);
            for (auto &valueTuple : values)
            {
                if (fvField(valueTuple) == "speed")
                {
                    ASSERT_TRUE(fvValue(valueTuple) != "0");
                }
            }
        }

        sai_port_api = orig_port_api;
    }

    /*
    * The scope of this test is to verify that LAG member is
    * added to a LAG before any other object on LAG is created, like RIF, bridge port in warm mode.
    * For objects like RIF which are created by a different Orch we know that they will wait until
    * allPortsReady(), so we can guaranty they won't be created if PortsOrch can process ports, lags,
    * vlans in single doTask().
    * If objects are created in PortsOrch, like bridge port, we will spy on SAI API to verify they are
    * not called before create_lag_member.
    * This is done like this because of limitation on Mellanox platform that does not allow to create objects
    * on LAG before at least one LAG members is added in warm reboot. Later this will be fixed.
    *
    */
    TEST_F(PortsOrchTest, LagMemberIsCreatedBeforeOtherObjectsAreCreatedOnLag)
    {
        Table portTable = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
        Table lagTable = Table(m_app_db.get(), APP_LAG_TABLE_NAME);
        Table lagMemberTable = Table(m_app_db.get(), APP_LAG_MEMBER_TABLE_NAME);
        Table vlanTable = Table(m_app_db.get(), APP_VLAN_TABLE_NAME);
        Table vlanMemberTable = Table(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);

        // Get SAI default ports to populate DB
        auto ports = ut_helper::getInitialSaiPorts();

        /*
         * Next we will prepare some configuration data to be consumed by PortsOrch
         * 32 Ports, 1 LAG, 1 port is LAG member and LAG is in Vlan.
         */

        // Populate pot table with SAI ports
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }

        // Set PortConfigDone
        portTable.set("PortConfigDone", { { "count", to_string(ports.size()) } });
        portTable.set("PortInitDone", { { } });

        lagTable.set("PortChannel0001",
            {
                {"admin_status", "up"},
                {"mtu", "9100"}
            }
        );
        lagMemberTable.set(
            std::string("PortChannel0001") + lagMemberTable.getTableNameSeparator() + ports.begin()->first,
            { {"status", "enabled"} });
        vlanTable.set("Vlan5",
            {
                {"admin_status", "up"},
                {"mtu", "9100"}
            }
        );
        vlanMemberTable.set(
            std::string("Vlan5") + vlanMemberTable.getTableNameSeparator() + std::string("PortChannel0001"),
            { {"tagging_mode", "untagged"} }
        );

        // refill consumer
        gPortsOrch->addExistingData(&portTable);
        gPortsOrch->addExistingData(&lagTable);
        gPortsOrch->addExistingData(&lagMemberTable);
        gPortsOrch->addExistingData(&vlanTable);
        gPortsOrch->addExistingData(&vlanMemberTable);

        // save original api since we will spy
        auto orig_lag_api = sai_lag_api;
        sai_lag_api = new sai_lag_api_t();
        memcpy(sai_lag_api, orig_lag_api, sizeof(*sai_lag_api));

        auto orig_bridge_api = sai_bridge_api;
        sai_bridge_api = new sai_bridge_api_t();
        memcpy(sai_bridge_api, orig_bridge_api, sizeof(*sai_bridge_api));

        bool bridgePortCalled = false;
        bool bridgePortCalledBeforeLagMember = false;

        auto lagSpy = SpyOn<SAI_API_LAG, SAI_OBJECT_TYPE_LAG_MEMBER>(&sai_lag_api->create_lag_member);
        lagSpy->callFake([&](sai_object_id_t *oid, sai_object_id_t swoid, uint32_t count, const sai_attribute_t * attrs) -> sai_status_t {
                if (bridgePortCalled) {
                    bridgePortCalledBeforeLagMember = true;
                }
                return orig_lag_api->create_lag_member(oid, swoid, count, attrs);
            }
        );

        auto bridgeSpy = SpyOn<SAI_API_BRIDGE, SAI_OBJECT_TYPE_BRIDGE_PORT>(&sai_bridge_api->create_bridge_port);
        bridgeSpy->callFake([&](sai_object_id_t *oid, sai_object_id_t swoid, uint32_t count, const sai_attribute_t * attrs) -> sai_status_t {
                bridgePortCalled = true;
                return orig_bridge_api->create_bridge_port(oid, swoid, count, attrs);
            }
        );

        static_cast<Orch *>(gPortsOrch)->doTask();

        vector<string> ts;

        // check LAG, VLAN tasks were processed
        // port table may require one more doTask iteration
        for (auto tableName: {
                APP_LAG_TABLE_NAME,
                APP_LAG_MEMBER_TABLE_NAME,
                APP_VLAN_TABLE_NAME,
                APP_VLAN_MEMBER_TABLE_NAME})
        {
            auto exec = gPortsOrch->getExecutor(tableName);
            auto consumer = static_cast<Consumer*>(exec);
            ts.clear();
            consumer->dumpPendingTasks(ts);
            ASSERT_TRUE(ts.empty());
        }

        ASSERT_FALSE(bridgePortCalledBeforeLagMember); // bridge port created on lag before lag member was created
    }

}
