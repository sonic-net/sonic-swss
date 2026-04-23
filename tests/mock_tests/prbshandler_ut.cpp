#include "prbshandler.h"
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"

extern sai_port_api_t *sai_port_api;

namespace prbshandler_test
{
    using namespace std;
    using namespace swss;

    /* ----- SAI mock state ------------------------------------------------- */

    sai_port_api_t ut_sai_port_api;
    sai_port_api_t *pold_sai_port_api;

    sai_status_t mock_set_result = SAI_STATUS_SUCCESS;
    sai_status_t mock_get_result = SAI_STATUS_SUCCESS;

    uint32_t set_port_attr_count = 0;
    sai_port_attr_t last_set_attr_id;
    int32_t last_set_s32_value;
    uint32_t last_set_u32_value;

    uint32_t mock_polynomial_value = static_cast<uint32_t>(SAI_PORT_PRBS_PATTERN_PRBS31);

    vector<int32_t> mock_supported_patterns;
    sai_status_t mock_get_supported_result = SAI_STATUS_SUCCESS;

    /* ----- SAI mock callbacks --------------------------------------------- */

    sai_status_t _mock_set_port_attribute(
        _In_ sai_object_id_t port_id,
        _In_ const sai_attribute_t *attr)
    {
        set_port_attr_count++;
        last_set_attr_id = static_cast<sai_port_attr_t>(attr->id);
        last_set_s32_value = attr->value.s32;
        last_set_u32_value = attr->value.u32;
        return mock_set_result;
    }

    sai_status_t _mock_get_port_attribute(
        _In_ sai_object_id_t port_id,
        _In_ uint32_t attr_count,
        _Inout_ sai_attribute_t *attr_list)
    {
        if (mock_get_result != SAI_STATUS_SUCCESS)
            return mock_get_result;

        for (uint32_t i = 0; i < attr_count; i++)
        {
            switch (attr_list[i].id)
            {
                case SAI_PORT_ATTR_PRBS_PATTERN:
                    attr_list[i].value.s32 = static_cast<int32_t>(mock_polynomial_value);
                    break;

                case SAI_PORT_ATTR_SUPPORTED_PRBS_PATTERN:
                {
                    if (mock_get_supported_result != SAI_STATUS_SUCCESS)
                        return mock_get_supported_result;

                    uint32_t count = static_cast<uint32_t>(mock_supported_patterns.size());
                    if (count > attr_list[i].value.s32list.count)
                    {
                        attr_list[i].value.s32list.count = count;
                        return SAI_STATUS_BUFFER_OVERFLOW;
                    }
                    attr_list[i].value.s32list.count = count;
                    for (uint32_t j = 0; j < count; j++)
                    {
                        attr_list[i].value.s32list.list[j] = mock_supported_patterns[j];
                    }
                    break;
                }

                case SAI_PORT_ATTR_PRBS_RX_STATE:
                    attr_list[i].value.rx_state.rx_status = SAI_PORT_PRBS_RX_STATUS_OK;
                    attr_list[i].value.rx_state.error_count = 42;
                    break;

                case SAI_PORT_ATTR_PRBS_PER_LANE_RX_STATE_LIST:
                {
                    uint32_t count = attr_list[i].value.prbs_rx_state_list.count;
                    if (count > 4) count = 4;
                    attr_list[i].value.prbs_rx_state_list.count = count;
                    if (attr_list[i].value.prbs_rx_state_list.list)
                    {
                        for (uint32_t j = 0; j < count; j++)
                        {
                            attr_list[i].value.prbs_rx_state_list.list[j].lane = j;
                            if (j == 0)
                                attr_list[i].value.prbs_rx_state_list.list[j].rx_state.rx_status = SAI_PORT_PRBS_RX_STATUS_OK;
                            else if (j == 1)
                                attr_list[i].value.prbs_rx_state_list.list[j].rx_state.rx_status = SAI_PORT_PRBS_RX_STATUS_LOCK_WITH_ERRORS;
                            else if (j == 2)
                                attr_list[i].value.prbs_rx_state_list.list[j].rx_state.rx_status = SAI_PORT_PRBS_RX_STATUS_NOT_LOCKED;
                            else
                                attr_list[i].value.prbs_rx_state_list.list[j].rx_state.rx_status = SAI_PORT_PRBS_RX_STATUS_LOST_LOCK;

                            attr_list[i].value.prbs_rx_state_list.list[j].rx_state.error_count = j * 10;
                        }
                    }
                    break;
                }

                case SAI_PORT_ATTR_PRBS_PER_LANE_BER_LIST:
                {
                    uint32_t count = attr_list[i].value.prbs_ber_list.count;
                    if (count > 4) count = 4;
                    attr_list[i].value.prbs_ber_list.count = count;
                    if (attr_list[i].value.prbs_ber_list.list)
                    {
                        for (uint32_t j = 0; j < count; j++)
                        {
                            attr_list[i].value.prbs_ber_list.list[j].lane = j;
                            attr_list[i].value.prbs_ber_list.list[j].ber.mantissa = 183 + j;
                            attr_list[i].value.prbs_ber_list.list[j].ber.exponent = 11;
                        }
                    }
                    break;
                }

                default:
                    return pold_sai_port_api->get_port_attribute(port_id, attr_count, attr_list);
            }
        }
        return SAI_STATUS_SUCCESS;
    }

    /* ----- Test fixture --------------------------------------------------- */

    struct PrbsHandlerTest : public ::testing::Test
    {
        void SetUp() override
        {
            ::testing_db::reset();

            m_state_db = make_shared<DBConnector>("STATE_DB", 0);
            m_stateTable = make_unique<Table>(m_state_db.get(), "PORT_PRBS_TEST");
            m_laneResultTable = make_unique<Table>(m_state_db.get(), "PORT_PRBS_LANE_RESULT");
            m_resultsTable = make_unique<Table>(m_state_db.get(), "PORT_PRBS_RESULTS");

            m_portStateTable = make_unique<Table>(m_state_db.get(), "PORT_TABLE");
            m_handler = make_unique<PrbsHandler>(*m_stateTable, *m_laneResultTable, *m_resultsTable);

            m_port.m_alias = "Ethernet0";
            m_port.m_port_id = 0x100000001;

            pold_sai_port_api = sai_port_api;
            ut_sai_port_api = *sai_port_api;
            ut_sai_port_api.set_port_attribute = _mock_set_port_attribute;
            ut_sai_port_api.get_port_attribute = _mock_get_port_attribute;
            sai_port_api = &ut_sai_port_api;

            mock_set_result = SAI_STATUS_SUCCESS;
            mock_get_result = SAI_STATUS_SUCCESS;
            mock_get_supported_result = SAI_STATUS_SUCCESS;
            mock_polynomial_value = static_cast<uint32_t>(SAI_PORT_PRBS_PATTERN_PRBS31);
            mock_supported_patterns.clear();
            set_port_attr_count = 0;
        }

        void TearDown() override
        {
            sai_port_api = pold_sai_port_api;
            ::testing_db::reset();
        }

        static void SetUpTestCase()
        {
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
        }

        static void TearDownTestCase()
        {
            sai_switch_api->remove_switch(gSwitchId);
            gSwitchId = 0;
            ut_helper::uninitSaiApi();
        }

        string getField(Table &table, const string &key, const string &field)
        {
            vector<FieldValueTuple> fvs;
            if (!table.get(key, fvs))
                return "";
            for (const auto &fv : fvs)
            {
                if (fvField(fv) == field)
                    return fvValue(fv);
            }
            return "";
        }

        bool keyExists(Table &table, const string &key)
        {
            vector<FieldValueTuple> fvs;
            return table.get(key, fvs);
        }

        shared_ptr<DBConnector> m_state_db;
        unique_ptr<Table> m_stateTable;
        unique_ptr<Table> m_laneResultTable;
        unique_ptr<Table> m_resultsTable;
        unique_ptr<Table> m_portStateTable;
        unique_ptr<PrbsHandler> m_handler;
        Port m_port;
    };

    /* ===================================================================== */
    /*  handlePrbsPatternConfig — patterns in both #ifdef and #else maps     */
    /* ===================================================================== */

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS31_SetsCorrectAttr)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS31"));
        EXPECT_EQ(set_port_attr_count, 1u);

        EXPECT_EQ(last_set_attr_id, SAI_PORT_ATTR_PRBS_PATTERN);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS31));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS7)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS7"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS7));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS9)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS9"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS9));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS11)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS11"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS11));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS15)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS15"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS15));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS23)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS23"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS23));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_none_MapsToAuto)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "none"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_AUTO));
    }

    /* ===================================================================== */
    /*  handlePrbsPatternConfig — extended patterns (SAI_PORT_ATTR_PRBS_PATTERN only) */
    /* ===================================================================== */

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS10)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS10"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS10));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS13)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS13"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS13));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS16)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS16"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS16));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS20)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS20"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS20));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS32)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS32"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS32));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS49)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS49"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS49));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS58)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS58"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS58));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS7Q)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS7Q"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS7Q));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS9Q)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS9Q"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS9Q));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS13Q)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS13Q"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS13Q));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS15Q)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS15Q"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS15Q));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS23Q)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS23Q"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS23Q));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_PRBS31Q)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS31Q"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_PRBS31Q));
    }

    TEST_F(PrbsHandlerTest, PatternConfig_SSPRQ)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "SSPRQ"));
        EXPECT_EQ(set_port_attr_count, 1u);
        EXPECT_EQ(last_set_s32_value, static_cast<int32_t>(SAI_PORT_PRBS_PATTERN_SSPRQ));
    }

    /* ===================================================================== */
    /*  handlePrbsPatternConfig — error cases                                */
    /* ===================================================================== */

    TEST_F(PrbsHandlerTest, PatternConfig_InvalidPattern_ReturnsFalse)
    {
        EXPECT_FALSE(m_handler->handlePrbsPatternConfig(m_port, "PRBS99"));
        EXPECT_EQ(set_port_attr_count, 0u);
    }

    TEST_F(PrbsHandlerTest, PatternConfig_EmptyString_SkipsAndReturnsTrue)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, ""));
        EXPECT_EQ(set_port_attr_count, 0u);
    }

    TEST_F(PrbsHandlerTest, PatternConfig_SaiFailure_ReturnsFalse)
    {
        mock_set_result = SAI_STATUS_FAILURE;
        EXPECT_FALSE(m_handler->handlePrbsPatternConfig(m_port, "PRBS31"));
        EXPECT_EQ(set_port_attr_count, 1u);
    }

    /* ===================================================================== */
    /*  handlePrbsConfig — enable                                            */
    /* ===================================================================== */

    TEST_F(PrbsHandlerTest, Enable_Rx_WritesStateDB)
    {
        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "rx"));

        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "status"), "running");
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "mode"), "rx");
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "pattern"), "PRBS31");
        EXPECT_FALSE(getField(*m_stateTable, "Ethernet0", "start_time").empty());
    }

    TEST_F(PrbsHandlerTest, Enable_Tx_WritesStateDB)
    {
        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "tx"));

        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "status"), "running");
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "mode"), "tx");
    }

    TEST_F(PrbsHandlerTest, Enable_Both_WritesStateDB)
    {
        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "both"));

        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "status"), "running");
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "mode"), "both");
    }

    TEST_F(PrbsHandlerTest, Enable_PatternQueryReflectsMockValue)
    {
        mock_polynomial_value = static_cast<uint32_t>(SAI_PORT_PRBS_PATTERN_PRBS9);
        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "rx"));
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "pattern"), "PRBS9");
    }

    TEST_F(PrbsHandlerTest, Enable_UnknownPolynomialValue_StoresUnknown)
    {
        mock_polynomial_value = 999;
        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "rx"));
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "pattern"), "unknown");
    }

    TEST_F(PrbsHandlerTest, Enable_SaiFailure_SetsFailedStatus)
    {
        mock_set_result = SAI_STATUS_FAILURE;
        EXPECT_FALSE(m_handler->handlePrbsConfig(m_port, "both"));
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "status"), "failed");
    }

    TEST_F(PrbsHandlerTest, Enable_InvalidMode_ReturnsFalse)
    {
        EXPECT_FALSE(m_handler->handlePrbsConfig(m_port, "invalid"));
        EXPECT_EQ(set_port_attr_count, 0u);
    }

    TEST_F(PrbsHandlerTest, Enable_ClearsOldResults)
    {
        m_stateTable->set("Ethernet0", {{"status", "stopped"}});
        m_resultsTable->set("Ethernet0", {{"error_count", "100"}});
        m_laneResultTable->set("Ethernet0|0", {{"rx_status", "OK"}});
        m_laneResultTable->set("Ethernet0|1", {{"rx_status", "NOT_LOCKED"}});

        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "rx"));

        EXPECT_EQ(getField(*m_resultsTable, "Ethernet0", "error_count"), "");
        EXPECT_EQ(getField(*m_laneResultTable, "Ethernet0|0", "rx_status"), "");
        EXPECT_EQ(getField(*m_laneResultTable, "Ethernet0|1", "rx_status"), "");
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "status"), "running");
    }

    TEST_F(PrbsHandlerTest, Enable_PatternGetFailure_StillSucceeds)
    {
        mock_get_result = SAI_STATUS_FAILURE;
        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "rx"));
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "status"), "running");
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "pattern"), "unknown");
    }

    /* ===================================================================== */
    /*  handlePrbsConfig — disable                                           */
    /* ===================================================================== */

    TEST_F(PrbsHandlerTest, Disable_Success_SetsStoppedAndStopTime)
    {
        m_stateTable->set("Ethernet0", {{"status", "running"}, {"mode", "rx"}});

        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "disabled"));

        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "status"), "stopped");
        EXPECT_FALSE(getField(*m_stateTable, "Ethernet0", "stop_time").empty());
    }

    TEST_F(PrbsHandlerTest, Disable_SaiFailure_SetsErroredStatus)
    {
        m_stateTable->set("Ethernet0", {{"status", "running"}, {"mode", "rx"}});
        mock_set_result = SAI_STATUS_FAILURE;

        EXPECT_FALSE(m_handler->handlePrbsConfig(m_port, "disabled"));
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "status"), "errored");
    }

    TEST_F(PrbsHandlerTest, Disable_TxOnly_SkipsRxCapture)
    {
        m_stateTable->set("Ethernet0", {{"status", "running"}, {"mode", "tx"}});

        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "disabled"));
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "status"), "stopped");
        EXPECT_EQ(getField(*m_resultsTable, "Ethernet0", "rx_status"), "");
    }

    TEST_F(PrbsHandlerTest, Disable_CapturesPerPortResults)
    {
        m_stateTable->set("Ethernet0", {{"status", "running"}, {"mode", "rx"}});

        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "disabled"));

        EXPECT_EQ(getField(*m_resultsTable, "Ethernet0", "rx_status"), "OK");
        EXPECT_EQ(getField(*m_resultsTable, "Ethernet0", "error_count"), "42");
    }

    TEST_F(PrbsHandlerTest, Disable_CapturesPerLaneResults_4Lanes)
    {
        m_stateTable->set("Ethernet0", {{"status", "running"}, {"mode", "both"}});

        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "disabled"));

        EXPECT_EQ(getField(*m_laneResultTable, "Ethernet0|0", "rx_status"), "OK");
        EXPECT_EQ(getField(*m_laneResultTable, "Ethernet0|0", "error_count"), "0");
        EXPECT_EQ(getField(*m_laneResultTable, "Ethernet0|0", "ber_mantissa"), "183");
        EXPECT_EQ(getField(*m_laneResultTable, "Ethernet0|0", "ber_exponent"), "11");

        EXPECT_EQ(getField(*m_laneResultTable, "Ethernet0|1", "rx_status"), "LOCK_WITH_ERRORS");
        EXPECT_EQ(getField(*m_laneResultTable, "Ethernet0|1", "error_count"), "10");
        EXPECT_EQ(getField(*m_laneResultTable, "Ethernet0|1", "ber_mantissa"), "184");

        EXPECT_EQ(getField(*m_laneResultTable, "Ethernet0|2", "rx_status"), "NOT_LOCKED");
        EXPECT_EQ(getField(*m_laneResultTable, "Ethernet0|2", "error_count"), "20");

        EXPECT_EQ(getField(*m_laneResultTable, "Ethernet0|3", "rx_status"), "LOST_LOCK");
        EXPECT_EQ(getField(*m_laneResultTable, "Ethernet0|3", "error_count"), "30");

        EXPECT_EQ(getField(*m_resultsTable, "Ethernet0", "total_lanes"), "4");
    }

    TEST_F(PrbsHandlerTest, Disable_GetFailure_StillSetsStoppedStatus)
    {
        m_stateTable->set("Ethernet0", {{"status", "running"}, {"mode", "rx"}});
        mock_get_result = SAI_STATUS_FAILURE;

        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "disabled"));
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "status"), "stopped");
    }

    TEST_F(PrbsHandlerTest, Disable_NoTestData_StillSucceeds)
    {
        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "disabled"));
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "status"), "stopped");
    }

    /* ===================================================================== */
    /*  clearPrbsResults                                                     */
    /* ===================================================================== */

    TEST_F(PrbsHandlerTest, Clear_RemovesAllTablesForPort)
    {
        m_stateTable->set("Ethernet0", {{"status", "stopped"}});
        m_resultsTable->set("Ethernet0", {{"error_count", "100"}});
        m_laneResultTable->set("Ethernet0|0", {{"rx_status", "OK"}});
        m_laneResultTable->set("Ethernet0|1", {{"rx_status", "NOT_LOCKED"}});
        m_laneResultTable->set("Ethernet0|2", {{"rx_status", "LOST_LOCK"}});

        m_handler->clearPrbsResults("Ethernet0");

        EXPECT_FALSE(keyExists(*m_stateTable, "Ethernet0"));
        EXPECT_FALSE(keyExists(*m_resultsTable, "Ethernet0"));
        EXPECT_FALSE(keyExists(*m_laneResultTable, "Ethernet0|0"));
        EXPECT_FALSE(keyExists(*m_laneResultTable, "Ethernet0|1"));
        EXPECT_FALSE(keyExists(*m_laneResultTable, "Ethernet0|2"));
    }

    TEST_F(PrbsHandlerTest, Clear_DoesNotAffectOtherPorts)
    {
        m_stateTable->set("Ethernet0", {{"status", "stopped"}});
        m_stateTable->set("Ethernet4", {{"status", "stopped"}});
        m_resultsTable->set("Ethernet4", {{"error_count", "50"}});

        m_handler->clearPrbsResults("Ethernet0");

        EXPECT_FALSE(keyExists(*m_stateTable, "Ethernet0"));
        EXPECT_TRUE(keyExists(*m_stateTable, "Ethernet4"));
        EXPECT_TRUE(keyExists(*m_resultsTable, "Ethernet4"));
    }

    TEST_F(PrbsHandlerTest, Clear_NoopWhenNoData)
    {
        m_handler->clearPrbsResults("Ethernet0");
        EXPECT_FALSE(keyExists(*m_stateTable, "Ethernet0"));
    }

    /* ===================================================================== */
    /*  removeAllPrbsEntries                                                 */
    /* ===================================================================== */

    TEST_F(PrbsHandlerTest, RemoveAll_ClearsMultiplePorts)
    {
        m_stateTable->set("Ethernet0", {{"status", "stopped"}});
        m_stateTable->set("Ethernet4", {{"status", "running"}});
        m_stateTable->set("Ethernet8", {{"status", "stopped"}});
        m_resultsTable->set("Ethernet0", {{"error_count", "100"}});
        m_resultsTable->set("Ethernet8", {{"error_count", "0"}});
        m_laneResultTable->set("Ethernet0|0", {{"rx_status", "OK"}});
        m_laneResultTable->set("Ethernet4|0", {{"rx_status", "NOT_LOCKED"}});
        m_laneResultTable->set("Ethernet8|0", {{"rx_status", "OK"}});

        m_handler->removeAllPrbsEntries();

        EXPECT_FALSE(keyExists(*m_stateTable, "Ethernet0"));
        EXPECT_FALSE(keyExists(*m_stateTable, "Ethernet4"));
        EXPECT_FALSE(keyExists(*m_stateTable, "Ethernet8"));
        EXPECT_FALSE(keyExists(*m_resultsTable, "Ethernet0"));
        EXPECT_FALSE(keyExists(*m_resultsTable, "Ethernet8"));
        EXPECT_FALSE(keyExists(*m_laneResultTable, "Ethernet0|0"));
        EXPECT_FALSE(keyExists(*m_laneResultTable, "Ethernet4|0"));
        EXPECT_FALSE(keyExists(*m_laneResultTable, "Ethernet8|0"));
    }

    TEST_F(PrbsHandlerTest, RemoveAll_NoopWhenEmpty)
    {
        m_handler->removeAllPrbsEntries();

        vector<string> keys;
        m_stateTable->getKeys(keys);
        EXPECT_TRUE(keys.empty());
    }

    /* ===================================================================== */
    /*  Full enable -> disable lifecycle                                     */
    /* ===================================================================== */

    TEST_F(PrbsHandlerTest, Lifecycle_EnableThenDisable_CapturesResults)
    {
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS31"));
        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "rx"));

        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "status"), "running");
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "mode"), "rx");
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "pattern"), "PRBS31");

        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "disabled"));

        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "status"), "stopped");
        EXPECT_FALSE(getField(*m_stateTable, "Ethernet0", "stop_time").empty());
        EXPECT_EQ(getField(*m_resultsTable, "Ethernet0", "rx_status"), "OK");
        EXPECT_EQ(getField(*m_resultsTable, "Ethernet0", "error_count"), "42");
    }

    TEST_F(PrbsHandlerTest, Lifecycle_EnableClearEnable_StartsClean)
    {
        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "rx"));
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "status"), "running");

        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "disabled"));
        EXPECT_EQ(getField(*m_resultsTable, "Ethernet0", "rx_status"), "OK");

        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "rx"));
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "status"), "running");
        EXPECT_EQ(getField(*m_resultsTable, "Ethernet0", "error_count"), "");
    }

    TEST_F(PrbsHandlerTest, Lifecycle_PatternThenEnable_PRBS7)
    {
        mock_polynomial_value = static_cast<uint32_t>(SAI_PORT_PRBS_PATTERN_PRBS7);
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS7"));
        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "both"));
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "pattern"), "PRBS7");
    }

    TEST_F(PrbsHandlerTest, Lifecycle_PatternThenEnable_PRBS23)
    {
        mock_polynomial_value = static_cast<uint32_t>(SAI_PORT_PRBS_PATTERN_PRBS23);
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS23"));
        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "rx"));
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "pattern"), "PRBS23");
    }

    TEST_F(PrbsHandlerTest, Lifecycle_PatternThenEnable_PRBS31Q)
    {
        mock_polynomial_value = static_cast<uint32_t>(SAI_PORT_PRBS_PATTERN_PRBS31Q);
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "PRBS31Q"));
        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "both"));
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "pattern"), "PRBS31Q");
    }

    TEST_F(PrbsHandlerTest, Lifecycle_PatternThenEnable_SSPRQ)
    {
        mock_polynomial_value = static_cast<uint32_t>(SAI_PORT_PRBS_PATTERN_SSPRQ);
        EXPECT_TRUE(m_handler->handlePrbsPatternConfig(m_port, "SSPRQ"));
        EXPECT_TRUE(m_handler->handlePrbsConfig(m_port, "rx"));
        EXPECT_EQ(getField(*m_stateTable, "Ethernet0", "pattern"), "SSPRQ");
    }

    /* ===================================================================== */
    /*  getPortSupportedPrbsPatterns                                         */
    /* ===================================================================== */

    TEST_F(PrbsHandlerTest, SupportedPatterns_Success_PopulatesList)
    {
        mock_supported_patterns = {
            SAI_PORT_PRBS_PATTERN_PRBS7,
            SAI_PORT_PRBS_PATTERN_PRBS31,
            SAI_PORT_PRBS_PATTERN_PRBS9
        };

        PortSupportedPrbsPatterns result;
        m_handler->getPortSupportedPrbsPatterns("Ethernet0", m_port.m_port_id, result);

        ASSERT_EQ(result.size(), 3u);
        EXPECT_EQ(result[0], static_cast<sai_uint32_t>(SAI_PORT_PRBS_PATTERN_PRBS7));
        EXPECT_EQ(result[1], static_cast<sai_uint32_t>(SAI_PORT_PRBS_PATTERN_PRBS31));
        EXPECT_EQ(result[2], static_cast<sai_uint32_t>(SAI_PORT_PRBS_PATTERN_PRBS9));
    }

    TEST_F(PrbsHandlerTest, SupportedPatterns_EmptyList_ReturnsEmpty)
    {
        mock_supported_patterns.clear();

        PortSupportedPrbsPatterns result;
        m_handler->getPortSupportedPrbsPatterns("Ethernet0", m_port.m_port_id, result);

        EXPECT_TRUE(result.empty());
    }

    TEST_F(PrbsHandlerTest, SupportedPatterns_NotSupported_ReturnsEmpty)
    {
        mock_get_supported_result = SAI_STATUS_NOT_SUPPORTED;

        PortSupportedPrbsPatterns result;
        m_handler->getPortSupportedPrbsPatterns("Ethernet0", m_port.m_port_id, result);

        EXPECT_TRUE(result.empty());
    }

    TEST_F(PrbsHandlerTest, SupportedPatterns_NotImplemented_ReturnsEmpty)
    {
        mock_get_supported_result = SAI_STATUS_NOT_IMPLEMENTED;

        PortSupportedPrbsPatterns result;
        m_handler->getPortSupportedPrbsPatterns("Ethernet0", m_port.m_port_id, result);

        EXPECT_TRUE(result.empty());
    }

    TEST_F(PrbsHandlerTest, SupportedPatterns_GenericFailure_ReturnsEmpty)
    {
        mock_get_supported_result = SAI_STATUS_FAILURE;

        PortSupportedPrbsPatterns result;
        m_handler->getPortSupportedPrbsPatterns("Ethernet0", m_port.m_port_id, result);

        EXPECT_TRUE(result.empty());
    }

    TEST_F(PrbsHandlerTest, SupportedPatterns_SinglePattern)
    {
        mock_supported_patterns = { SAI_PORT_PRBS_PATTERN_PRBS31 };

        PortSupportedPrbsPatterns result;
        m_handler->getPortSupportedPrbsPatterns("Ethernet0", m_port.m_port_id, result);

        ASSERT_EQ(result.size(), 1u);
        EXPECT_EQ(result[0], static_cast<sai_uint32_t>(SAI_PORT_PRBS_PATTERN_PRBS31));
    }

    /* ===================================================================== */
    /*  initPortSupportedPrbsPatterns                                        */
    /* ===================================================================== */

    TEST_F(PrbsHandlerTest, InitSupported_WritesToPortStateTable)
    {
        mock_supported_patterns = {
            SAI_PORT_PRBS_PATTERN_PRBS7,
            SAI_PORT_PRBS_PATTERN_PRBS31
        };

        m_handler->initPortSupportedPrbsPatterns("Ethernet0", m_port.m_port_id, *m_portStateTable);

        string patterns_str = getField(*m_portStateTable, "Ethernet0", "supported_prbs_patterns");
        EXPECT_FALSE(patterns_str.empty());
        EXPECT_NE(patterns_str.find("PRBS7"), string::npos);
        EXPECT_NE(patterns_str.find("PRBS31"), string::npos);
    }

    TEST_F(PrbsHandlerTest, InitSupported_EmptyPatterns_NoStateEntry)
    {
        mock_supported_patterns.clear();

        m_handler->initPortSupportedPrbsPatterns("Ethernet0", m_port.m_port_id, *m_portStateTable);

        string patterns_str = getField(*m_portStateTable, "Ethernet0", "supported_prbs_patterns");
        EXPECT_TRUE(patterns_str.empty());
    }

    TEST_F(PrbsHandlerTest, InitSupported_CachesResult_SecondCallSkipsSai)
    {
        mock_supported_patterns = { SAI_PORT_PRBS_PATTERN_PRBS31 };

        m_handler->initPortSupportedPrbsPatterns("Ethernet0", m_port.m_port_id, *m_portStateTable);

        mock_supported_patterns = { SAI_PORT_PRBS_PATTERN_PRBS7 };
        m_handler->initPortSupportedPrbsPatterns("Ethernet0", m_port.m_port_id, *m_portStateTable);

        string patterns_str = getField(*m_portStateTable, "Ethernet0", "supported_prbs_patterns");
        EXPECT_NE(patterns_str.find("PRBS31"), string::npos);
        EXPECT_EQ(patterns_str.find("PRBS7"), string::npos);
    }

    TEST_F(PrbsHandlerTest, InitSupported_SaiFailure_NoStateEntry)
    {
        mock_get_supported_result = SAI_STATUS_NOT_SUPPORTED;

        m_handler->initPortSupportedPrbsPatterns("Ethernet0", m_port.m_port_id, *m_portStateTable);

        string patterns_str = getField(*m_portStateTable, "Ethernet0", "supported_prbs_patterns");
        EXPECT_TRUE(patterns_str.empty());
    }

    TEST_F(PrbsHandlerTest, InitSupported_UnknownEnumValue_StoresNumeric)
    {
        mock_supported_patterns = { SAI_PORT_PRBS_PATTERN_PRBS7, 999 };

        m_handler->initPortSupportedPrbsPatterns("Ethernet0", m_port.m_port_id, *m_portStateTable);

        string patterns_str = getField(*m_portStateTable, "Ethernet0", "supported_prbs_patterns");
        EXPECT_NE(patterns_str.find("PRBS7"), string::npos);
        EXPECT_NE(patterns_str.find("999"), string::npos);
    }

    /* ===================================================================== */
    /*  isPrbsPatternSupported                                               */
    /* ===================================================================== */

    TEST_F(PrbsHandlerTest, IsSupported_PatternInList_ReturnsTrue)
    {
        mock_supported_patterns = {
            SAI_PORT_PRBS_PATTERN_PRBS7,
            SAI_PORT_PRBS_PATTERN_PRBS31
        };

        EXPECT_TRUE(m_handler->isPrbsPatternSupported("Ethernet0", m_port.m_port_id, "PRBS31", *m_portStateTable));
    }

    TEST_F(PrbsHandlerTest, IsSupported_PatternNotInList_ReturnsFalse)
    {
        mock_supported_patterns = {
            SAI_PORT_PRBS_PATTERN_PRBS7,
            SAI_PORT_PRBS_PATTERN_PRBS31
        };

        EXPECT_FALSE(m_handler->isPrbsPatternSupported("Ethernet0", m_port.m_port_id, "PRBS9", *m_portStateTable));
    }

    TEST_F(PrbsHandlerTest, IsSupported_InvalidPatternString_ReturnsFalse)
    {
        mock_supported_patterns = { SAI_PORT_PRBS_PATTERN_PRBS31 };

        EXPECT_FALSE(m_handler->isPrbsPatternSupported("Ethernet0", m_port.m_port_id, "INVALID", *m_portStateTable));
    }

    TEST_F(PrbsHandlerTest, IsSupported_EmptyCapability_ReturnsFalse)
    {
        mock_supported_patterns.clear();

        EXPECT_FALSE(m_handler->isPrbsPatternSupported("Ethernet0", m_port.m_port_id, "PRBS31", *m_portStateTable));
    }

    TEST_F(PrbsHandlerTest, IsSupported_SaiFailure_ReturnsFalse)
    {
        mock_get_supported_result = SAI_STATUS_NOT_SUPPORTED;

        EXPECT_FALSE(m_handler->isPrbsPatternSupported("Ethernet0", m_port.m_port_id, "PRBS31", *m_portStateTable));
    }

    TEST_F(PrbsHandlerTest, IsSupported_UsesCachedData)
    {
        mock_supported_patterns = { SAI_PORT_PRBS_PATTERN_PRBS31 };
        EXPECT_TRUE(m_handler->isPrbsPatternSupported("Ethernet0", m_port.m_port_id, "PRBS31", *m_portStateTable));

        mock_supported_patterns = { SAI_PORT_PRBS_PATTERN_PRBS7 };
        EXPECT_TRUE(m_handler->isPrbsPatternSupported("Ethernet0", m_port.m_port_id, "PRBS31", *m_portStateTable));
        EXPECT_FALSE(m_handler->isPrbsPatternSupported("Ethernet0", m_port.m_port_id, "PRBS7", *m_portStateTable));
    }

    /* ===================================================================== */
    /*  clearSupportedPrbsPatterns                                           */
    /* ===================================================================== */

    TEST_F(PrbsHandlerTest, ClearSupported_AllowsRequery)
    {
        mock_supported_patterns = { SAI_PORT_PRBS_PATTERN_PRBS31 };
        m_handler->initPortSupportedPrbsPatterns("Ethernet0", m_port.m_port_id, *m_portStateTable);
        EXPECT_TRUE(m_handler->isPrbsPatternSupported("Ethernet0", m_port.m_port_id, "PRBS31", *m_portStateTable));

        m_handler->clearSupportedPrbsPatterns(m_port.m_port_id);

        mock_supported_patterns = { SAI_PORT_PRBS_PATTERN_PRBS7 };
        EXPECT_TRUE(m_handler->isPrbsPatternSupported("Ethernet0", m_port.m_port_id, "PRBS7", *m_portStateTable));
        EXPECT_FALSE(m_handler->isPrbsPatternSupported("Ethernet0", m_port.m_port_id, "PRBS31", *m_portStateTable));
    }

    TEST_F(PrbsHandlerTest, ClearSupported_NoopWhenNoCachedData)
    {
        m_handler->clearSupportedPrbsPatterns(m_port.m_port_id);

        mock_supported_patterns = { SAI_PORT_PRBS_PATTERN_PRBS31 };
        EXPECT_TRUE(m_handler->isPrbsPatternSupported("Ethernet0", m_port.m_port_id, "PRBS31", *m_portStateTable));
    }

} // namespace prbshandler_test
