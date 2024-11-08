#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private

#include "gtest/gtest.h"
#include "mock_table.h"
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "txmonorch.h"

namespace txmonorch_test
{
    using namespace std;

    // copy from portorch_ut.cpp, logic to mock some sai APIs.

    // SAI default ports
    std::map<std::string, std::vector<swss::FieldValueTuple>> defaultPortList;

    sai_port_api_t ut_sai_port_api;
    sai_port_api_t *old_sai_port_api_ptr;

    // return counters num with _sai_set_tx_err_counters.
    uint32_t _sai_set_tx_err_counters;
    sai_status_t _ut_stub_sai_get_port_stats(
        _In_ sai_object_id_t port_id,
        _In_ uint32_t number_of_counters,
        _In_ const sai_stat_id_t *counter_ids,
        _Out_ uint64_t *counters)
    {
        (void)port_id;
        (void)number_of_counters;
        (void)counter_ids;
        *counters = _sai_set_tx_err_counters;
        return SAI_STATUS_SUCCESS;
    }

    void _hook_sai_port_api()
    {
        ut_sai_port_api = *sai_port_api;
        old_sai_port_api_ptr = sai_port_api;
        ut_sai_port_api.get_port_stats = _ut_stub_sai_get_port_stats;

        sai_port_api = &ut_sai_port_api;
    }

    void _unhook_sai_port_api()
    {
        sai_port_api = old_sai_port_api_ptr;
    }

    // Define a test fixture class
    struct TxMonOrchTest : public ::testing::Test
    {
        shared_ptr<swss::DBConnector> m_app_db;
        shared_ptr<swss::DBConnector> m_config_db;
        shared_ptr<swss::DBConnector> m_state_db;
        shared_ptr<swss::DBConnector> m_counters_db;
        shared_ptr<swss::DBConnector> m_chassis_app_db;
        shared_ptr<swss::DBConnector> m_asic_db;

        TxMonOrchTest()
        {
            // again, logics come from portorch_ut.cpp

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

    // We don't have API for checking period and threshold independtly
    // Test with TX_ERR_STATE, TX_ERR_APPL and TX_ERR_CFG.
    TEST_F(TxMonOrchTest, pollOnePortErrorStatisticsErr9SmallerThanThres10Test) {
        // Given TX ERR DBs state, appl and cfg and last stat.
        _hook_sai_port_api();
        TableConnector stateDbTxErr(m_state_db.get(), STATE_TX_ERR_TABLE_NAME);
        TableConnector applDbTxErr(m_app_db.get(), APP_TX_ERR_TABLE_NAME);
        TableConnector confDbTxErr(m_config_db.get(), CFG_PORT_TX_ERR_TABLE_NAME);
        TxMonOrch txMonOrch(applDbTxErr, confDbTxErr, stateDbTxErr);
        vector<FieldValueTuple> period {{TXMONORCH_FIELD_CFG_PERIOD, "1"}};
        vector<FieldValueTuple> threshold {{TXMONORCH_FIELD_CFG_THRESHOLD, "10"}};
        // port ID 2, error count 0, threshold 10.
        sai_object_id_t portId = 2;
        uint64_t oriTxErrCnt = 0;
        uint64_t thres = 10;
        TxErrorStatistics stat = {TXMONORCH_PORT_STATE_UNKNOWN, portId, oriTxErrCnt, thres};

        // When set tx err counter to 9, smaller than thres 10.
        _sai_set_tx_err_counters = 9;
        txMonOrch.pollOnePortErrorStatistics("2", stat);

        // Then expect PORT state OK.
        cout << "txErrState: " << stat.txErrState << endl;
        cout << "txErrPortId: " << stat.txErrPortId << endl;
        cout << "txErrStat: " << stat.txErrStat << endl;
        cout << "txErrThreshold: " << stat.txErrThreshold << endl;
        EXPECT_TRUE(stat.txErrState == TXMONORCH_PORT_STATE_OK);
        EXPECT_TRUE(stat.txErrPortId == portId);
        EXPECT_TRUE(stat.txErrStat == _sai_set_tx_err_counters);
        EXPECT_TRUE(stat.txErrThreshold == thres);
    }

    TEST_F(TxMonOrchTest, pollOnePortErrorStatisticsErr100ExceedThres10Test) {
        // Given TX ERR DBs state, appl and cfg and last stat.
        _hook_sai_port_api();
        TableConnector stateDbTxErr(m_state_db.get(), STATE_TX_ERR_TABLE_NAME);
        TableConnector applDbTxErr(m_app_db.get(), APP_TX_ERR_TABLE_NAME);
        TableConnector confDbTxErr(m_config_db.get(), CFG_PORT_TX_ERR_TABLE_NAME);
        TxMonOrch txMonOrch(applDbTxErr, confDbTxErr, stateDbTxErr);
        vector<FieldValueTuple> period {{TXMONORCH_FIELD_CFG_PERIOD, "1"}};
        vector<FieldValueTuple> threshold {{TXMONORCH_FIELD_CFG_THRESHOLD, "10"}};
        // port ID 2, error count 0, threshold 10.
        sai_object_id_t portId = 2;
        uint64_t oriTxErrCnt = 0;
        uint64_t thres = 10;
        TxErrorStatistics stat = {TXMONORCH_PORT_STATE_UNKNOWN, portId, oriTxErrCnt, thres};

        // When set tx err counter to 100, bigger than thres 10.
        _sai_set_tx_err_counters = 100;
        txMonOrch.pollOnePortErrorStatistics("2", stat);

        // Then expect PORT state ERROR.
        cout << "txErrState: " << stat.txErrState << endl;
        cout << "txErrPortId: " << stat.txErrPortId << endl;
        cout << "txErrStat: " << stat.txErrStat << endl;
        cout << "txErrThreshold: " << stat.txErrThreshold << endl;
        EXPECT_TRUE(stat.txErrState == TXMONORCH_PORT_STATE_ERROR);
        EXPECT_TRUE(stat.txErrPortId == portId);
        EXPECT_TRUE(stat.txErrStat == _sai_set_tx_err_counters);
        EXPECT_TRUE(stat.txErrThreshold == thres);
    }
}
