/**
 * @file gearbox_ut.cpp
 * @brief Unit tests for gearbox functionality in GearboxUtils and PortsOrch
 *
 * Tests gearbox configuration parsing, map loading from _GEARBOX_TABLE,
 * and PortsOrch gearbox integration.
 */

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "gearboxutils.h"
#include "portsorch.h"
#include "switchorch.h"
#include "sai_serialize.h"
#include "bufferorch.h"
#include "flexcounterorch.h"
#include "intfsorch.h"
#include "fdborch.h"
#include "neighorch.h"
#include "vrforch.h"
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>
#include <cstdlib>

extern SwitchOrch *gSwitchOrch;
extern PortsOrch *gPortsOrch;
extern BufferOrch *gBufferOrch;
extern IntfsOrch *gIntfsOrch;
extern FdbOrch *gFdbOrch;
extern NeighOrch *gNeighOrch;
extern VRFOrch *gVrfOrch;

using namespace std;

namespace gearbox_test
{

/**
 * Test fixture for GearboxUtils - standalone tests without SAI/PortsOrch
 */
struct GearboxUtilsTest : public ::testing::Test
{
    GearboxUtilsTest() {}

    void SetUp() override
    {
        ::testing_db::reset();
        m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
    }

    void TearDown() override
    {
        ::testing_db::reset();
    }

    shared_ptr<swss::DBConnector> m_app_db;
};

/**
 * Test fixture for PortsOrch gearbox integration - requires SAI init
 */
struct GearboxPortsOrchTest : public ::testing::Test
{
    GearboxPortsOrchTest() {}

    void SetUp() override
    {
        ::testing_db::reset();

        m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
        m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
        m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
        m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);

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
        vector<table_name_with_pri_t> port_tables = {
            { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
            { APP_SEND_TO_INGRESS_PORT_TABLE_NAME, portsorch_base_pri + 5 },
            { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
            { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
            { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
            { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
        };

        ASSERT_EQ(gPortsOrch, nullptr);
        gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), port_tables, m_chassis_app_db.get());
    }

    void TearDown() override
    {
        ::testing_db::reset();

        delete gPortsOrch;
        gPortsOrch = nullptr;
        delete gSwitchOrch;
        gSwitchOrch = nullptr;
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
        auto status = sai_switch_api->remove_switch(gSwitchId);
        ASSERT_EQ(status, SAI_STATUS_SUCCESS);
        gSwitchId = 0;
        ut_helper::uninitSaiApi();
    }

    shared_ptr<swss::DBConnector> m_app_db;
    shared_ptr<swss::DBConnector> m_config_db;
    shared_ptr<swss::DBConnector> m_state_db;
    shared_ptr<swss::DBConnector> m_chassis_app_db;
};

// ============== GearboxUtils Tests ==============

TEST_F(GearboxUtilsTest, IsGearboxConfigDone_WhenNotSet_ReturnsFalse)
{
    swss::Table gearboxTable(m_app_db.get(), "_GEARBOX_TABLE");
    swss::GearboxUtils gearbox;

    bool result = gearbox.isGearboxConfigDone(gearboxTable);
    EXPECT_FALSE(result);

    result = gearbox.isGearboxConfigDone(&gearboxTable);
    EXPECT_FALSE(result);
}

TEST_F(GearboxUtilsTest, IsGearboxConfigDone_WhenSet_ReturnsTrue)
{
    swss::Table gearboxTable(m_app_db.get(), "_GEARBOX_TABLE");
    gearboxTable.set("GearboxConfigDone", {});
    swss::GearboxUtils gearbox;

    bool result = gearbox.isGearboxConfigDone(gearboxTable);
    EXPECT_TRUE(result);

    result = gearbox.isGearboxConfigDone(&gearboxTable);
    EXPECT_TRUE(result);
}

TEST_F(GearboxUtilsTest, LoadPhyMap_EmptyTable_ReturnsEmptyMap)
{
    swss::Table gearboxTable(m_app_db.get(), "_GEARBOX_TABLE");
    swss::GearboxUtils gearbox;

    auto phyMap = gearbox.loadPhyMap(&gearboxTable);
    EXPECT_TRUE(phyMap.empty());
}

TEST_F(GearboxUtilsTest, LoadPhyMap_SinglePhy_ParsesCorrectly)
{
    swss::Table gearboxTable(m_app_db.get(), "_GEARBOX_TABLE");
    gearboxTable.set("phy:1", {
        {"phy_id", "1"},
        {"phy_oid", "oid:0x1000000000001"},
        {"name", "phy1"},
        {"lib_name", "libphy.so"},
        {"firmware_path", "/firmware.bin"},
        {"config_file", "config.json"},
        {"sai_init_config_file", "sai_init.json"},
        {"phy_access", "mdio"},
        {"address", "0"},
        {"bus_id", "1"},
        {"context_id", "0"},
        {"macsec_supported", "true"}
    });

    swss::GearboxUtils gearbox;
    auto phyMap = gearbox.loadPhyMap(&gearboxTable);

    ASSERT_EQ(phyMap.size(), 1u);
    EXPECT_EQ(phyMap[1].phy_id, 1);
    EXPECT_EQ(phyMap[1].phy_oid, "oid:0x1000000000001");
    EXPECT_EQ(phyMap[1].name, "phy1");
    EXPECT_EQ(phyMap[1].lib_name, "libphy.so");
    EXPECT_TRUE(phyMap[1].macsec_supported);
}

TEST_F(GearboxUtilsTest, LoadPhyMap_MacsecNotSupported_ParsesCorrectly)
{
    swss::Table gearboxTable(m_app_db.get(), "_GEARBOX_TABLE");
    gearboxTable.set("phy:1", {
        {"phy_id", "1"},
        {"phy_oid", "oid:0x1"},
        {"macsec_supported", "false"}
    });

    swss::GearboxUtils gearbox;
    auto phyMap = gearbox.loadPhyMap(&gearboxTable);

    ASSERT_EQ(phyMap.size(), 1u);
    EXPECT_FALSE(phyMap[1].macsec_supported);
}

TEST_F(GearboxUtilsTest, LoadInterfaceMap_EmptyTable_ReturnsEmptyMap)
{
    swss::Table gearboxTable(m_app_db.get(), "_GEARBOX_TABLE");
    swss::GearboxUtils gearbox;

    auto intfMap = gearbox.loadInterfaceMap(&gearboxTable);
    EXPECT_TRUE(intfMap.empty());
}

TEST_F(GearboxUtilsTest, LoadInterfaceMap_SingleInterface_ParsesCorrectly)
{
    swss::Table gearboxTable(m_app_db.get(), "_GEARBOX_TABLE");
    gearboxTable.set("interface:0", {
        {"index", "0"},
        {"phy_id", "1"},
        {"line_lanes", "0, 1, 2, 3"},
        {"system_lanes", "100, 101, 102, 103"}
    });

    swss::GearboxUtils gearbox;
    auto intfMap = gearbox.loadInterfaceMap(&gearboxTable);

    ASSERT_EQ(intfMap.size(), 1u);
    EXPECT_EQ(intfMap[0].index, 0);
    EXPECT_EQ(intfMap[0].phy_id, 1);
    EXPECT_EQ(intfMap[0].line_lanes.size(), 4u);
    EXPECT_EQ(intfMap[0].system_lanes.size(), 4u);
}

TEST_F(GearboxUtilsTest, LoadLaneMap_EmptyTable_ReturnsEmptyMap)
{
    swss::Table gearboxTable(m_app_db.get(), "_GEARBOX_TABLE");
    swss::GearboxUtils gearbox;

    auto laneMap = gearbox.loadLaneMap(&gearboxTable);
    EXPECT_TRUE(laneMap.empty());
}

TEST_F(GearboxUtilsTest, LoadLaneMap_SingleLane_ParsesCorrectly)
{
    swss::Table gearboxTable(m_app_db.get(), "_GEARBOX_TABLE");
    gearboxTable.set("phy:1:lanes:200", {
        {"index", "200"},
        {"system_side", "true"},
        {"tx_polarity", "0"},
        {"rx_polarity", "0"},
        {"line_tx_lanemap", "0"},
        {"line_rx_lanemap", "0"},
        {"line_to_system_lanemap", "0"},
        {"mdio_addr", "0x1"}
    });

    swss::GearboxUtils gearbox;
    auto laneMap = gearbox.loadLaneMap(&gearboxTable);

    ASSERT_EQ(laneMap.size(), 1u);
    EXPECT_EQ(laneMap[200].index, 200);
    EXPECT_TRUE(laneMap[200].system_side);
    EXPECT_EQ(laneMap[200].mdio_addr, "0x1");
}

TEST_F(GearboxUtilsTest, LoadPortMap_EmptyTable_ReturnsEmptyMap)
{
    swss::Table gearboxTable(m_app_db.get(), "_GEARBOX_TABLE");
    swss::GearboxUtils gearbox;

    auto portMap = gearbox.loadPortMap(&gearboxTable);
    EXPECT_TRUE(portMap.empty());
}

TEST_F(GearboxUtilsTest, LoadPortMap_SinglePort_ParsesCorrectly)
{
    swss::Table gearboxTable(m_app_db.get(), "_GEARBOX_TABLE");
    gearboxTable.set("phy:1:ports:0", {
        {"index", "0"},
        {"mdio_addr", "0x1"},
        {"system_speed", "100000"},
        {"system_fec", "rs"},
        {"system_auto_neg", "false"},
        {"system_loopback", "none"},
        {"system_training", "false"},
        {"line_speed", "100000"},
        {"line_fec", "rs"},
        {"line_auto_neg", "true"},
        {"line_media_type", "fiber"},
        {"line_intf_type", "cr4"},
        {"line_loopback", "none"},
        {"line_training", "false"},
        {"line_adver_speed", "100000, 50000"},
        {"line_adver_fec", "1, 2"},
        {"line_adver_auto_neg", "true"},
        {"line_adver_asym_pause", "false"},
        {"line_adver_media_type", "fiber"}
    });

    swss::GearboxUtils gearbox;
    auto portMap = gearbox.loadPortMap(&gearboxTable);

    ASSERT_EQ(portMap.size(), 1u);
    EXPECT_EQ(portMap[0].index, 0);
    EXPECT_EQ(portMap[0].system_speed, 100000);
    EXPECT_EQ(portMap[0].system_fec, "rs");
    EXPECT_FALSE(portMap[0].system_auto_neg);
    EXPECT_EQ(portMap[0].line_speed, 100000);
    EXPECT_TRUE(portMap[0].line_auto_neg);
    EXPECT_EQ(portMap[0].line_media_type, "fiber");
    EXPECT_EQ(portMap[0].line_intf_type, "cr4");
    EXPECT_EQ(portMap[0].line_adver_speed.size(), 2u);
    EXPECT_EQ(portMap[0].line_adver_fec.size(), 2u);
}

TEST_F(GearboxUtilsTest, LoadMaps_CombinedGearboxConfig_ParsesAllCorrectly)
{
    swss::Table gearboxTable(m_app_db.get(), "_GEARBOX_TABLE");

    gearboxTable.set("phy:1", {
        {"phy_id", "1"},
        {"phy_oid", "oid:0x1"},
        {"name", "phy1"}
    });
    gearboxTable.set("phy:1:ports:0", {
        {"index", "0"},
        {"system_speed", "100000"},
        {"system_fec", "rs"},
        {"line_speed", "100000"},
        {"line_fec", "rs"}
    });
    gearboxTable.set("phy:1:lanes:100", {
        {"index", "100"},
        {"system_side", "true"}
    });
    gearboxTable.set("interface:0", {
        {"index", "0"},
        {"phy_id", "1"},
        {"line_lanes", "0, 1"},
        {"system_lanes", "100, 101"}
    });

    swss::GearboxUtils gearbox;

    auto phyMap = gearbox.loadPhyMap(&gearboxTable);
    EXPECT_EQ(phyMap.size(), 1u);

    auto intfMap = gearbox.loadInterfaceMap(&gearboxTable);
    EXPECT_EQ(intfMap.size(), 1u);

    auto laneMap = gearbox.loadLaneMap(&gearboxTable);
    EXPECT_EQ(laneMap.size(), 1u);

    auto portMap = gearbox.loadPortMap(&gearboxTable);
    EXPECT_EQ(portMap.size(), 1u);
}

// ============== PortsOrch Gearbox Integration Tests ==============

TEST_F(GearboxPortsOrchTest, IsGearboxEnabled_WithoutConfigFile_ReturnsFalse)
{
    ASSERT_NE(gPortsOrch, nullptr);
    EXPECT_FALSE(gPortsOrch->isGearboxEnabled());
}

TEST_F(GearboxPortsOrchTest, GetDestPortId_WhenGearboxDisabled_ReturnsFalse)
{
    ASSERT_NE(gPortsOrch, nullptr);
    sai_object_id_t src_port_id = 0x1000;
    sai_object_id_t des_port_id = 0;

    bool result = gPortsOrch->getDestPortId(src_port_id, PortsOrch::PHY_PORT_TYPE, des_port_id);
    EXPECT_FALSE(result);
    // When gearbox disabled, des_port_id is set to src_port_id (no translation)
    EXPECT_EQ(des_port_id, src_port_id);
}

/**
 * Test fixture for gearbox-enabled tests - uses GEARBOX_UT_TEST_ENABLE env var
 * and populates _GEARBOX_TABLE before PortsOrch creation
 */
struct GearboxEnabledPortsOrchTest : public GearboxPortsOrchTest
{
    void SetUp() override
    {
        setenv("GEARBOX_UT_TEST_ENABLE", "1", 1);
        ::testing_db::reset();

        m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
        m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
        m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
        m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);

        setupGearboxTable();

        TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
        TableConnector app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);
        TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);
        vector<TableConnector> switch_tables = { conf_asic_sensors, app_switch_table };

        ASSERT_EQ(gSwitchOrch, nullptr);
        gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

        const int portsorch_base_pri = 40;
        vector<table_name_with_pri_t> port_tables = {
            { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
            { APP_SEND_TO_INGRESS_PORT_TABLE_NAME, portsorch_base_pri + 5 },
            { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
            { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
            { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
            { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
        };

        ASSERT_EQ(gPortsOrch, nullptr);
        gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), port_tables, m_chassis_app_db.get());
    }

    void TearDown() override
    {
        ::testing_db::reset();
        delete gPortsOrch;
        gPortsOrch = nullptr;
        delete gSwitchOrch;
        gSwitchOrch = nullptr;
        unsetenv("GEARBOX_UT_TEST_ENABLE");
    }

    void setupGearboxTable()
    {
        swss::Table gearboxTable(m_app_db.get(), "_GEARBOX_TABLE");
        gearboxTable.set("GearboxConfigDone", {});
        string phy_oid = "oid:0x1000000000001";
        if (gSwitchId != 0)
        {
            phy_oid = sai_serialize_object_id(gSwitchId);
        }
        gearboxTable.set("phy:1", {{"phy_id", "1"}, {"phy_oid", phy_oid}, {"name", "phy1"}});
        gearboxTable.set("interface:0", {
            {"index", "0"}, {"phy_id", "1"},
            {"line_lanes", "0, 1, 2, 3"}, {"system_lanes", "100, 101, 102, 103"}
        });
        gearboxTable.set("phy:1:ports:0", {
            {"index", "0"}, {"system_speed", "100000"}, {"system_fec", "rs"},
            {"system_auto_neg", "false"}, {"system_loopback", "none"}, {"system_training", "false"},
            {"line_speed", "100000"}, {"line_fec", "rs"}, {"line_auto_neg", "true"},
            {"line_media_type", "fiber"}, {"line_intf_type", "cr4"}, {"line_loopback", "none"},
            {"line_training", "false"}, {"line_adver_speed", "100000"}, {"line_adver_fec", "1"},
            {"line_adver_auto_neg", "true"}, {"line_adver_asym_pause", "false"},
            {"line_adver_media_type", "fiber"}
        });
    }
};

TEST_F(GearboxEnabledPortsOrchTest, IsGearboxEnabled_WithEnvVarAndConfig_ReturnsTrue)
{
    ASSERT_NE(gPortsOrch, nullptr);
    EXPECT_TRUE(gPortsOrch->isGearboxEnabled());
}

TEST_F(GearboxEnabledPortsOrchTest, InitGearbox_WithEnvVar_PopulatesMaps)
{
    ASSERT_NE(gPortsOrch, nullptr);
    EXPECT_TRUE(gPortsOrch->isGearboxEnabled());
    EXPECT_GT(gPortsOrch->m_gearboxPhyMap.size(), 0u);
    EXPECT_GT(gPortsOrch->m_gearboxInterfaceMap.size(), 0u);
    EXPECT_GT(gPortsOrch->m_gearboxPortMap.size(), 0u);
}

TEST_F(GearboxEnabledPortsOrchTest, GetDestPortId_WhenGearboxEnabledButNoPortMapping_ReturnsFalse)
{
    ASSERT_NE(gPortsOrch, nullptr);
    sai_object_id_t src_port_id = 0x1000;
    sai_object_id_t des_port_id = 0;

    bool result = gPortsOrch->getDestPortId(src_port_id, PortsOrch::PHY_PORT_TYPE, des_port_id);
    EXPECT_FALSE(result);
    EXPECT_EQ(des_port_id, src_port_id);
}

/**
 * SAI port API mocks for gearbox create_port and create_port_connector
 */
static sai_port_api_t *s_gearbox_orig_port_api = nullptr;
static sai_port_api_t s_gearbox_mock_port_api;
static sai_object_id_t s_gearbox_next_port_oid = 0x2000;

static sai_status_t gearbox_mock_create_port(
    _Out_ sai_object_id_t *port_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t *attr_list)
{
    (void)switch_id;
    (void)attr_count;
    (void)attr_list;
    *port_id = s_gearbox_next_port_oid++;
    return SAI_STATUS_SUCCESS;
}

static sai_status_t gearbox_mock_create_port_connector(
    _Out_ sai_object_id_t *port_connector_id,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t *attr_list)
{
    (void)switch_id;
    (void)attr_count;
    (void)attr_list;
    *port_connector_id = 0x3000;
    return SAI_STATUS_SUCCESS;
}

static sai_status_t gearbox_mock_set_port_attribute(
    _In_ sai_object_id_t port_id,
    _In_ const sai_attribute_t *attr)
{
    if (port_id >= 0x2000 && port_id < 0x3000)
    {
        return SAI_STATUS_SUCCESS;
    }
    return s_gearbox_orig_port_api->set_port_attribute(port_id, attr);
}

static sai_status_t gearbox_mock_get_port_attribute(
    _In_ sai_object_id_t port_id,
    _In_ uint32_t attr_count,
    _Inout_ sai_attribute_t *attr_list)
{
    if (port_id >= 0x2000 && port_id < 0x3000)
    {
        for (uint32_t i = 0; i < attr_count; i++)
        {
            if (attr_list[i].id == SAI_PORT_ATTR_OPER_STATUS)
            {
                attr_list[i].value.u32 = SAI_PORT_OPER_STATUS_UP;
            }
        }
        return SAI_STATUS_SUCCESS;
    }
    return s_gearbox_orig_port_api->get_port_attribute(port_id, attr_count, attr_list);
}

static void gearbox_hook_sai_port_api()
{
    s_gearbox_orig_port_api = sai_port_api;
    s_gearbox_mock_port_api = *sai_port_api;
    s_gearbox_mock_port_api.create_port = gearbox_mock_create_port;
    s_gearbox_mock_port_api.create_port_connector = gearbox_mock_create_port_connector;
    s_gearbox_mock_port_api.set_port_attribute = gearbox_mock_set_port_attribute;
    s_gearbox_mock_port_api.get_port_attribute = gearbox_mock_get_port_attribute;
    sai_port_api = &s_gearbox_mock_port_api;
}

static void gearbox_unhook_sai_port_api()
{
    sai_port_api = s_gearbox_orig_port_api;
}

/**
 * Full coverage test - initGearboxPort, setGearboxPortsAttr, updateGearboxPortOperStatus
 */
struct GearboxFullCoverageTest : public GearboxEnabledPortsOrchTest
{
    FlexCounterOrch *m_flexCounterOrch = nullptr;

    void SetUp() override
    {
        setenv("GEARBOX_UT_TEST_ENABLE", "1", 1);
        ::testing_db::reset();

        m_app_db = make_shared<swss::DBConnector>("APPL_DB", 0);
        m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
        m_state_db = make_shared<swss::DBConnector>("STATE_DB", 0);
        m_chassis_app_db = make_shared<swss::DBConnector>("CHASSIS_APP_DB", 0);

        setupGearboxTable();
        gearbox_hook_sai_port_api();

        TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
        TableConnector app_switch_table(m_app_db.get(), APP_SWITCH_TABLE_NAME);
        TableConnector conf_asic_sensors(m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);
        vector<TableConnector> switch_tables = { conf_asic_sensors, app_switch_table };

        ASSERT_EQ(gSwitchOrch, nullptr);
        gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

        vector<string> flex_counter_tables = { CFG_FLEX_COUNTER_TABLE_NAME };
        m_flexCounterOrch = new FlexCounterOrch(m_config_db.get(), flex_counter_tables);
        gDirectory.set(m_flexCounterOrch);

        vector<string> buffer_tables = {
            APP_BUFFER_POOL_TABLE_NAME, APP_BUFFER_PROFILE_TABLE_NAME,
            APP_BUFFER_QUEUE_TABLE_NAME, APP_BUFFER_PG_TABLE_NAME,
            APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME, APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME
        };
        ASSERT_EQ(gBufferOrch, nullptr);
        gBufferOrch = new BufferOrch(m_app_db.get(), m_config_db.get(), m_state_db.get(), buffer_tables);

        const int portsorch_base_pri = 40;
        vector<table_name_with_pri_t> port_tables = {
            { APP_PORT_TABLE_NAME, portsorch_base_pri + 5 },
            { APP_SEND_TO_INGRESS_PORT_TABLE_NAME, portsorch_base_pri + 5 },
            { APP_VLAN_TABLE_NAME, portsorch_base_pri + 2 },
            { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
            { APP_LAG_TABLE_NAME, portsorch_base_pri + 4 },
            { APP_LAG_MEMBER_TABLE_NAME, portsorch_base_pri }
        };

        ASSERT_EQ(gPortsOrch, nullptr);
        gPortsOrch = new PortsOrch(m_app_db.get(), m_state_db.get(), port_tables, m_chassis_app_db.get());

        Table portTable(m_app_db.get(), APP_PORT_TABLE_NAME);
        auto ports = ut_helper::getInitialSaiPorts();
        for (const auto &it : ports)
        {
            portTable.set(it.first, it.second);
        }
        portTable.set("PortConfigDone", {{"count", to_string(ports.size())}});
        gPortsOrch->addExistingData(&portTable);
        static_cast<Orch *>(gPortsOrch)->doTask();

        portTable.set("PortInitDone", {{"lanes", "0"}});
        gPortsOrch->addExistingData(&portTable);
        static_cast<Orch *>(gPortsOrch)->doTask();
    }

    void TearDown() override
    {
        gearbox_unhook_sai_port_api();
        gDirectory.m_values.clear();
        delete gBufferOrch;
        gBufferOrch = nullptr;
        delete m_flexCounterOrch;
        m_flexCounterOrch = nullptr;
        delete gPortsOrch;
        gPortsOrch = nullptr;
        delete gSwitchOrch;
        gSwitchOrch = nullptr;
        ::testing_db::reset();
        unsetenv("GEARBOX_UT_TEST_ENABLE");
    }
};

TEST_F(GearboxFullCoverageTest, InitGearboxPort_CreatesGearboxPortMapping)
{
    ASSERT_NE(gPortsOrch, nullptr);
    ASSERT_TRUE(gPortsOrch->isGearboxEnabled());

    Port port;
    ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

    EXPECT_NE(port.m_system_side_id, (sai_object_id_t)0);
    EXPECT_NE(port.m_line_side_id, (sai_object_id_t)0);
    EXPECT_GT(gPortsOrch->m_gearboxPortListLaneMap.count(port.m_port_id), 0u);
}

TEST_F(GearboxFullCoverageTest, SetGearboxPortsAttr_ViaSetPortAdminStatus)
{
    ASSERT_NE(gPortsOrch, nullptr);
    Port port;
    ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
    ASSERT_NE(port.m_system_side_id, (sai_object_id_t)0);

    bool success = gPortsOrch->setPortAdminStatus(port, false);
    EXPECT_TRUE(success);

    success = gPortsOrch->setPortAdminStatus(port, true);
    EXPECT_TRUE(success);
}

TEST_F(GearboxFullCoverageTest, SetGearboxPortsAttr_ViaSetPortMtu)
{
    ASSERT_NE(gPortsOrch, nullptr);
    Port port;
    ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
    ASSERT_NE(port.m_system_side_id, (sai_object_id_t)0);

    bool success = gPortsOrch->setPortMtu(port, 1500);
    EXPECT_TRUE(success);
}

TEST_F(GearboxFullCoverageTest, UpdateGearboxPortOperStatus)
{
    ASSERT_NE(gPortsOrch, nullptr);
    Port port;
    ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
    ASSERT_NE(port.m_system_side_id, (sai_object_id_t)0);
    ASSERT_NE(port.m_line_side_id, (sai_object_id_t)0);

    gPortsOrch->updateGearboxPortOperStatus(port);

    swss::Table portTable(m_app_db.get(), APP_PORT_TABLE_NAME);
    vector<swss::FieldValueTuple> tuples;
    ASSERT_TRUE(portTable.get(port.m_alias, tuples));
    bool has_system_oper = false;
    for (const auto &t : tuples)
    {
        if (fvField(t) == "system_oper_status")
        {
            has_system_oper = true;
            EXPECT_EQ(fvValue(t), "up");
            break;
        }
    }
    EXPECT_TRUE(has_system_oper);
}

TEST_F(GearboxFullCoverageTest, GetDestPortId_WhenGearboxPortMappingExists_ReturnsTrue)
{
    ASSERT_NE(gPortsOrch, nullptr);
    Port port;
    ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

    sai_object_id_t des_port_id = 0;
    bool result = gPortsOrch->getDestPortId(port.m_port_id, PortsOrch::PHY_PORT_TYPE, des_port_id);
    EXPECT_TRUE(result);
    EXPECT_EQ(des_port_id, port.m_system_side_id);

    des_port_id = 0;
    result = gPortsOrch->getDestPortId(port.m_port_id, PortsOrch::LINE_PORT_TYPE, des_port_id);
    EXPECT_TRUE(result);
    EXPECT_EQ(des_port_id, port.m_line_side_id);
}

TEST_F(GearboxFullCoverageTest, SetGearboxPortsAttr_ViaSetPortFec)
{
    ASSERT_NE(gPortsOrch, nullptr);
    Port port;
    ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));
    ASSERT_NE(port.m_system_side_id, (sai_object_id_t)0);

    bool success = gPortsOrch->setPortFec(port, SAI_PORT_FEC_MODE_RS, false);
    EXPECT_TRUE(success);
}

} // namespace gearbox_test
