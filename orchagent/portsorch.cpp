#include "portsorch.h"
#include "intfsorch.h"
#include "bufferorch.h"
#include "neighorch.h"
#include "gearboxutils.h"
#include "vxlanorch.h"
#include "directory.h"
#include "subintf.h"

#include <inttypes.h>
#include <cassert>
#include <fstream>
#include <sstream>
#include <set>
#include <algorithm>
#include <tuple>
#include <sstream>
#include <unordered_set>
#include <boost/algorithm/string.hpp>

#include <netinet/if_ether.h>
#include "net/if.h"

#include "logger.h"
#include "schema.h"
#include "redisapi.h"
#include "converter.h"
#include "sai_serialize.h"
#include "crmorch.h"
#include "countercheckorch.h"
#include "notifier.h"
#include "fdborch.h"
#include "stringutility.h"
#include "subscriberstatetable.h"

extern sai_switch_api_t *sai_switch_api;
extern sai_bridge_api_t *sai_bridge_api;
extern sai_port_api_t *sai_port_api;
extern sai_vlan_api_t *sai_vlan_api;
extern sai_lag_api_t *sai_lag_api;
extern sai_hostif_api_t* sai_hostif_api;
extern sai_acl_api_t* sai_acl_api;
extern sai_queue_api_t *sai_queue_api;
extern sai_object_id_t gSwitchId;
extern sai_fdb_api_t *sai_fdb_api;
extern sai_l2mc_group_api_t *sai_l2mc_group_api;
extern IntfsOrch *gIntfsOrch;
extern NeighOrch *gNeighOrch;
extern CrmOrch *gCrmOrch;
extern BufferOrch *gBufferOrch;
extern FdbOrch *gFdbOrch;
extern Directory<Orch*> gDirectory;
extern sai_system_port_api_t *sai_system_port_api;
extern string gMySwitchType;
extern int32_t gVoqMySwitchId;
extern string gMyHostName;
extern string gMyAsicName;

#define DEFAULT_SYSTEM_PORT_MTU 9100
#define VLAN_PREFIX         "Vlan"
#define DEFAULT_VLAN_ID     1
#define MAX_VALID_VLAN_ID   4094

#define PORT_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS     1000
#define PORT_BUFFER_DROP_STAT_POLLING_INTERVAL_MS     60000
#define QUEUE_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS   10000
#define QUEUE_WATERMARK_FLEX_STAT_COUNTER_POLL_MSECS "60000"
#define PG_WATERMARK_FLEX_STAT_COUNTER_POLL_MSECS    "60000"
#define PG_DROP_FLEX_STAT_COUNTER_POLL_MSECS         "10000"
#define PORT_RATE_FLEX_COUNTER_POLLING_INTERVAL_MS   "1000"


static map<string, sai_port_fec_mode_t> fec_mode_map =
{
    { "none",  SAI_PORT_FEC_MODE_NONE },
    { "rs", SAI_PORT_FEC_MODE_RS },
    { "fc", SAI_PORT_FEC_MODE_FC }
};

static map<string, sai_port_priority_flow_control_mode_t> pfc_asym_map =
{
    { "on", SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_SEPARATE },
    { "off", SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_COMBINED }
};

static map<string, sai_bridge_port_fdb_learning_mode_t> learn_mode_map =
{
    { "drop",  SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DROP },
    { "disable", SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DISABLE },
    { "hardware", SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW },
    { "cpu_trap", SAI_BRIDGE_PORT_FDB_LEARNING_MODE_CPU_TRAP},
    { "cpu_log", SAI_BRIDGE_PORT_FDB_LEARNING_MODE_CPU_LOG},
    { "notification", SAI_BRIDGE_PORT_FDB_LEARNING_MODE_FDB_NOTIFICATION}
};

static map<string, sai_port_media_type_t> media_type_map =
{
    { "fiber", SAI_PORT_MEDIA_TYPE_FIBER },
    { "copper", SAI_PORT_MEDIA_TYPE_COPPER }
};

static map<string, sai_port_internal_loopback_mode_t> loopback_mode_map =
{
    { "none",  SAI_PORT_INTERNAL_LOOPBACK_MODE_NONE },
    { "phy", SAI_PORT_INTERNAL_LOOPBACK_MODE_PHY },
    { "mac", SAI_PORT_INTERNAL_LOOPBACK_MODE_MAC }
};

static map<string, int> autoneg_mode_map =
{
    { "on", 1 },
    { "off", 0 }
};

// Interface type map used for gearbox
static map<string, sai_port_interface_type_t> interface_type_map =
{
 { "none", SAI_PORT_INTERFACE_TYPE_NONE },
 { "cr", SAI_PORT_INTERFACE_TYPE_CR },
 { "cr4", SAI_PORT_INTERFACE_TYPE_CR4 },
 { "cr8", SAI_PORT_INTERFACE_TYPE_CR8 },
 { "sr", SAI_PORT_INTERFACE_TYPE_SR },
 { "sr4", SAI_PORT_INTERFACE_TYPE_SR4 },
 { "sr8", SAI_PORT_INTERFACE_TYPE_SR8 },
 { "lr", SAI_PORT_INTERFACE_TYPE_LR },
 { "lr4", SAI_PORT_INTERFACE_TYPE_LR4 },
 { "lr8", SAI_PORT_INTERFACE_TYPE_LR8 },
 { "kr", SAI_PORT_INTERFACE_TYPE_KR },
 { "kr4", SAI_PORT_INTERFACE_TYPE_KR4 },
 { "kr8", SAI_PORT_INTERFACE_TYPE_KR8 }
};

// Interface type map used for auto negotiation
static map<string, sai_port_interface_type_t> interface_type_map_for_an =
{
    { "none", SAI_PORT_INTERFACE_TYPE_NONE },
    { "cr", SAI_PORT_INTERFACE_TYPE_CR },
    { "cr2", SAI_PORT_INTERFACE_TYPE_CR2 },
    { "cr4", SAI_PORT_INTERFACE_TYPE_CR4 },
    { "cr8", SAI_PORT_INTERFACE_TYPE_CR8 },
    { "sr", SAI_PORT_INTERFACE_TYPE_SR },
    { "sr2", SAI_PORT_INTERFACE_TYPE_SR2 },
    { "sr4", SAI_PORT_INTERFACE_TYPE_SR4 },
    { "sr8", SAI_PORT_INTERFACE_TYPE_SR8 },
    { "lr", SAI_PORT_INTERFACE_TYPE_LR },
    { "lr4", SAI_PORT_INTERFACE_TYPE_LR4 },
    { "lr8", SAI_PORT_INTERFACE_TYPE_LR8 },
    { "kr", SAI_PORT_INTERFACE_TYPE_KR },
    { "kr4", SAI_PORT_INTERFACE_TYPE_KR4 },
    { "kr8", SAI_PORT_INTERFACE_TYPE_KR8 },
    { "caui", SAI_PORT_INTERFACE_TYPE_CAUI },
    { "gmii", SAI_PORT_INTERFACE_TYPE_GMII },
    { "sfi", SAI_PORT_INTERFACE_TYPE_SFI },
    { "xlaui", SAI_PORT_INTERFACE_TYPE_XLAUI },
    { "kr2", SAI_PORT_INTERFACE_TYPE_KR2 },
    { "caui4", SAI_PORT_INTERFACE_TYPE_CAUI4 },
    { "xaui", SAI_PORT_INTERFACE_TYPE_XAUI },
    { "xfi", SAI_PORT_INTERFACE_TYPE_XFI },
    { "xgmii", SAI_PORT_INTERFACE_TYPE_XGMII }
};

static const std::string& getValidInterfaceTypes()
{
    static std::string validInterfaceTypes;
    if (validInterfaceTypes.empty())
    {
        std::ostringstream oss;
        for (auto &iter : interface_type_map_for_an)
        {
            oss << iter.first << " ";
        }
        validInterfaceTypes = oss.str();
        boost::to_upper(validInterfaceTypes);
    }

    return validInterfaceTypes;
}

const vector<sai_port_stat_t> port_stat_ids =
{
    SAI_PORT_STAT_IF_IN_OCTETS,
    SAI_PORT_STAT_IF_IN_UCAST_PKTS,
    SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS,
    SAI_PORT_STAT_IF_IN_DISCARDS,
    SAI_PORT_STAT_IF_IN_ERRORS,
    SAI_PORT_STAT_IF_IN_UNKNOWN_PROTOS,
    SAI_PORT_STAT_IF_OUT_OCTETS,
    SAI_PORT_STAT_IF_OUT_UCAST_PKTS,
    SAI_PORT_STAT_IF_OUT_NON_UCAST_PKTS,
    SAI_PORT_STAT_IF_OUT_DISCARDS,
    SAI_PORT_STAT_IF_OUT_ERRORS,
    SAI_PORT_STAT_IF_OUT_QLEN,
    SAI_PORT_STAT_IF_IN_MULTICAST_PKTS,
    SAI_PORT_STAT_IF_IN_BROADCAST_PKTS,
    SAI_PORT_STAT_IF_OUT_MULTICAST_PKTS,
    SAI_PORT_STAT_IF_OUT_BROADCAST_PKTS,
    SAI_PORT_STAT_ETHER_RX_OVERSIZE_PKTS,
    SAI_PORT_STAT_ETHER_TX_OVERSIZE_PKTS,
    SAI_PORT_STAT_ETHER_IN_PKTS_64_OCTETS,
    SAI_PORT_STAT_ETHER_IN_PKTS_65_TO_127_OCTETS,
    SAI_PORT_STAT_ETHER_IN_PKTS_128_TO_255_OCTETS,
    SAI_PORT_STAT_ETHER_IN_PKTS_256_TO_511_OCTETS,
    SAI_PORT_STAT_ETHER_IN_PKTS_512_TO_1023_OCTETS,
    SAI_PORT_STAT_ETHER_IN_PKTS_1024_TO_1518_OCTETS,
    SAI_PORT_STAT_ETHER_IN_PKTS_1519_TO_2047_OCTETS,
    SAI_PORT_STAT_ETHER_IN_PKTS_2048_TO_4095_OCTETS,
    SAI_PORT_STAT_ETHER_IN_PKTS_4096_TO_9216_OCTETS,
    SAI_PORT_STAT_ETHER_IN_PKTS_9217_TO_16383_OCTETS,
    SAI_PORT_STAT_ETHER_OUT_PKTS_64_OCTETS,
    SAI_PORT_STAT_ETHER_OUT_PKTS_65_TO_127_OCTETS,
    SAI_PORT_STAT_ETHER_OUT_PKTS_128_TO_255_OCTETS,
    SAI_PORT_STAT_ETHER_OUT_PKTS_256_TO_511_OCTETS,
    SAI_PORT_STAT_ETHER_OUT_PKTS_512_TO_1023_OCTETS,
    SAI_PORT_STAT_ETHER_OUT_PKTS_1024_TO_1518_OCTETS,
    SAI_PORT_STAT_ETHER_OUT_PKTS_1519_TO_2047_OCTETS,
    SAI_PORT_STAT_ETHER_OUT_PKTS_2048_TO_4095_OCTETS,
    SAI_PORT_STAT_ETHER_OUT_PKTS_4096_TO_9216_OCTETS,
    SAI_PORT_STAT_ETHER_OUT_PKTS_9217_TO_16383_OCTETS,
    SAI_PORT_STAT_PFC_0_TX_PKTS,
    SAI_PORT_STAT_PFC_1_TX_PKTS,
    SAI_PORT_STAT_PFC_2_TX_PKTS,
    SAI_PORT_STAT_PFC_3_TX_PKTS,
    SAI_PORT_STAT_PFC_4_TX_PKTS,
    SAI_PORT_STAT_PFC_5_TX_PKTS,
    SAI_PORT_STAT_PFC_6_TX_PKTS,
    SAI_PORT_STAT_PFC_7_TX_PKTS,
    SAI_PORT_STAT_PFC_0_RX_PKTS,
    SAI_PORT_STAT_PFC_1_RX_PKTS,
    SAI_PORT_STAT_PFC_2_RX_PKTS,
    SAI_PORT_STAT_PFC_3_RX_PKTS,
    SAI_PORT_STAT_PFC_4_RX_PKTS,
    SAI_PORT_STAT_PFC_5_RX_PKTS,
    SAI_PORT_STAT_PFC_6_RX_PKTS,
    SAI_PORT_STAT_PFC_7_RX_PKTS,
    SAI_PORT_STAT_PAUSE_RX_PKTS,
    SAI_PORT_STAT_PAUSE_TX_PKTS,
    SAI_PORT_STAT_ETHER_STATS_TX_NO_ERRORS,
    SAI_PORT_STAT_IP_IN_UCAST_PKTS,
    SAI_PORT_STAT_ETHER_STATS_JABBERS,
    SAI_PORT_STAT_ETHER_STATS_FRAGMENTS,
    SAI_PORT_STAT_ETHER_STATS_UNDERSIZE_PKTS,
    SAI_PORT_STAT_IP_IN_RECEIVES,
    SAI_PORT_STAT_IF_IN_FEC_CORRECTABLE_FRAMES,
    SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES,
    SAI_PORT_STAT_IF_IN_FEC_SYMBOL_ERRORS
};

const vector<sai_port_stat_t> port_buffer_drop_stat_ids =
{
    SAI_PORT_STAT_IN_DROPPED_PKTS,
    SAI_PORT_STAT_OUT_DROPPED_PKTS
};

static const vector<sai_queue_stat_t> queue_stat_ids =
{
    SAI_QUEUE_STAT_PACKETS,
    SAI_QUEUE_STAT_BYTES,
    SAI_QUEUE_STAT_DROPPED_PACKETS,
    SAI_QUEUE_STAT_DROPPED_BYTES,
};

static const vector<sai_queue_stat_t> queueWatermarkStatIds =
{
    SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES,
};

static const vector<sai_ingress_priority_group_stat_t> ingressPriorityGroupWatermarkStatIds =
{
    SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_BYTES,
    SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_BYTES,
};

static const vector<sai_ingress_priority_group_stat_t> ingressPriorityGroupDropStatIds =
{
    SAI_INGRESS_PRIORITY_GROUP_STAT_DROPPED_PACKETS
};

static char* hostif_vlan_tag[] = {
    [SAI_HOSTIF_VLAN_TAG_STRIP]     = "SAI_HOSTIF_VLAN_TAG_STRIP",
    [SAI_HOSTIF_VLAN_TAG_KEEP]      = "SAI_HOSTIF_VLAN_TAG_KEEP",
    [SAI_HOSTIF_VLAN_TAG_ORIGINAL]  = "SAI_HOSTIF_VLAN_TAG_ORIGINAL"
};

static bool isValidPortTypeForLagMember(const Port& port)
{
    return (port.m_type == Port::Type::PHY || port.m_type == Port::Type::SYSTEM);
}

/*
 * Initialize PortsOrch
 * 0) If Gearbox is enabled, then initialize the external PHYs as defined in
 *    the GEARBOX_TABLE.
 * 1) By default, a switch has one CPU port, one 802.1Q bridge, and one default
 *    VLAN. All ports are in .1Q bridge as bridge ports, and all bridge ports
 *    are in default VLAN as VLAN members.
 * 2) Query switch CPU port.
 * 3) Query ports associated with lane mappings
 * 4) Query switch .1Q bridge and all its bridge ports.
 * 5) Query switch default VLAN and all its VLAN members.
 * 6) Remove each VLAN member from default VLAN and each bridge port from .1Q
 *    bridge. By design, SONiC switch starts with all bridge ports removed from
 *    default VLAN and all ports removed from .1Q bridge.
 */
PortsOrch::PortsOrch(DBConnector *db, DBConnector *stateDb, vector<table_name_with_pri_t> &tableNames, DBConnector *chassisAppDb) :
        Orch(db, tableNames),
        m_portStateTable(stateDb, STATE_PORT_TABLE_NAME),
        port_stat_manager(PORT_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ, PORT_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, false),
        port_buffer_drop_stat_manager(PORT_BUFFER_DROP_STAT_FLEX_COUNTER_GROUP, StatsMode::READ, PORT_BUFFER_DROP_STAT_POLLING_INTERVAL_MS, false),
        queue_stat_manager(QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ, QUEUE_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, false)
{
    SWSS_LOG_ENTER();

    /* Initialize counter table */
    m_counter_db = shared_ptr<DBConnector>(new DBConnector("COUNTERS_DB", 0));
    m_counterTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_PORT_NAME_MAP));
    m_counterLagTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_LAG_NAME_MAP));
    FieldValueTuple tuple("", "");
    vector<FieldValueTuple> defaultLagFv;
    defaultLagFv.push_back(tuple);
    m_counterLagTable->set("", defaultLagFv);

    /* Initialize port and vlan table */
    m_portTable = unique_ptr<Table>(new Table(db, APP_PORT_TABLE_NAME));

    /* Initialize gearbox */
    m_gearboxTable = unique_ptr<Table>(new Table(db, "_GEARBOX_TABLE"));

    /* Initialize queue tables */
    m_queueTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_QUEUE_NAME_MAP));
    m_queuePortTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_QUEUE_PORT_MAP));
    m_queueIndexTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_QUEUE_INDEX_MAP));
    m_queueTypeTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_QUEUE_TYPE_MAP));

    /* Initialize ingress priority group tables */
    m_pgTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_PG_NAME_MAP));
    m_pgPortTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_PG_PORT_MAP));
    m_pgIndexTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_PG_INDEX_MAP));

    m_flex_db = shared_ptr<DBConnector>(new DBConnector("FLEX_COUNTER_DB", 0));
    m_flexCounterTable = unique_ptr<ProducerTable>(new ProducerTable(m_flex_db.get(), FLEX_COUNTER_TABLE));
    m_flexCounterGroupTable = unique_ptr<ProducerTable>(new ProducerTable(m_flex_db.get(), FLEX_COUNTER_GROUP_TABLE));

    m_state_db = shared_ptr<DBConnector>(new DBConnector("STATE_DB", 0));
    m_stateBufferMaximumValueTable = unique_ptr<Table>(new Table(m_state_db.get(), STATE_BUFFER_MAXIMUM_VALUE_TABLE));

    initGearbox();

    string queueWmSha, pgWmSha;
    string queueWmPluginName = "watermark_queue.lua";
    string pgWmPluginName = "watermark_pg.lua";
    string portRatePluginName = "port_rates.lua";

    try
    {
        string queueLuaScript = swss::loadLuaScript(queueWmPluginName);
        queueWmSha = swss::loadRedisScript(m_counter_db.get(), queueLuaScript);

        string pgLuaScript = swss::loadLuaScript(pgWmPluginName);
        pgWmSha = swss::loadRedisScript(m_counter_db.get(), pgLuaScript);

        string portRateLuaScript = swss::loadLuaScript(portRatePluginName);
        string portRateSha = swss::loadRedisScript(m_counter_db.get(), portRateLuaScript);

        vector<FieldValueTuple> fieldValues;
        fieldValues.emplace_back(QUEUE_PLUGIN_FIELD, queueWmSha);
        fieldValues.emplace_back(POLL_INTERVAL_FIELD, QUEUE_WATERMARK_FLEX_STAT_COUNTER_POLL_MSECS);
        fieldValues.emplace_back(STATS_MODE_FIELD, STATS_MODE_READ_AND_CLEAR);
        m_flexCounterGroupTable->set(QUEUE_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP, fieldValues);

        fieldValues.clear();
        fieldValues.emplace_back(PG_PLUGIN_FIELD, pgWmSha);
        fieldValues.emplace_back(POLL_INTERVAL_FIELD, PG_WATERMARK_FLEX_STAT_COUNTER_POLL_MSECS);
        fieldValues.emplace_back(STATS_MODE_FIELD, STATS_MODE_READ_AND_CLEAR);
        m_flexCounterGroupTable->set(PG_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP, fieldValues);

        fieldValues.clear();
        fieldValues.emplace_back(PORT_PLUGIN_FIELD, portRateSha);
        fieldValues.emplace_back(POLL_INTERVAL_FIELD, PORT_RATE_FLEX_COUNTER_POLLING_INTERVAL_MS);
        fieldValues.emplace_back(STATS_MODE_FIELD, STATS_MODE_READ);
        m_flexCounterGroupTable->set(PORT_STAT_COUNTER_FLEX_COUNTER_GROUP, fieldValues);

        fieldValues.clear();
        fieldValues.emplace_back(POLL_INTERVAL_FIELD, PG_DROP_FLEX_STAT_COUNTER_POLL_MSECS);
        fieldValues.emplace_back(STATS_MODE_FIELD, STATS_MODE_READ);
        m_flexCounterGroupTable->set(PG_DROP_STAT_COUNTER_FLEX_COUNTER_GROUP, fieldValues);
    }
    catch (const runtime_error &e)
    {
        SWSS_LOG_ERROR("Port flex counter groups were not set successfully: %s", e.what());
    }

    uint32_t i, j;
    sai_status_t status;
    sai_attribute_t attr;

    /* Get CPU port */
    attr.id = SAI_SWITCH_ATTR_CPU_PORT;

    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get CPU port, rv:%d", status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_SWITCH, status);
        if (handle_status != task_process_status::task_success)
        {
            throw runtime_error("PortsOrch initialization failure");
        }
    }

    m_cpuPort = Port("CPU", Port::CPU);
    m_cpuPort.m_port_id = attr.value.oid;
    m_portList[m_cpuPort.m_alias] = m_cpuPort;
    m_port_ref_count[m_cpuPort.m_alias] = 0;

    /* Get port number */
    attr.id = SAI_SWITCH_ATTR_PORT_NUMBER;

    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get port number, rv:%d", status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_SWITCH, status);
        if (handle_status != task_process_status::task_success)
        {
            throw runtime_error("PortsOrch initialization failure");
        }
    }

    m_portCount = attr.value.u32;
    SWSS_LOG_NOTICE("Get %d ports", m_portCount);

    /* Get port list */
    vector<sai_object_id_t> port_list;
    port_list.resize(m_portCount);

    attr.id = SAI_SWITCH_ATTR_PORT_LIST;
    attr.value.objlist.count = (uint32_t)port_list.size();
    attr.value.objlist.list = port_list.data();

    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get port list, rv:%d", status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_SWITCH, status);
        if (handle_status != task_process_status::task_success)
        {
            throw runtime_error("PortsOrch initialization failure");
        }
    }

    /* Get port hardware lane info */
    for (i = 0; i < m_portCount; i++)
    {
        sai_uint32_t lanes[8] = { 0,0,0,0,0,0,0,0 };
        attr.id = SAI_PORT_ATTR_HW_LANE_LIST;
        attr.value.u32list.count = 8;
        attr.value.u32list.list = lanes;

        status = sai_port_api->get_port_attribute(port_list[i], 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to get hardware lane list pid:%" PRIx64, port_list[i]);
            task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
            if (handle_status != task_process_status::task_success)
            {
                throw runtime_error("PortsOrch initialization failure");
            }
        }

        set<int> tmp_lane_set;
        for (j = 0; j < attr.value.u32list.count; j++)
        {
            tmp_lane_set.insert(attr.value.u32list.list[j]);
        }

        string tmp_lane_str = "";
        for (auto s : tmp_lane_set)
        {
            tmp_lane_str += to_string(s) + " ";
        }
        tmp_lane_str = tmp_lane_str.substr(0, tmp_lane_str.size()-1);

        SWSS_LOG_NOTICE("Get port with lanes pid:%" PRIx64 " lanes:%s", port_list[i], tmp_lane_str.c_str());
        m_portListLaneMap[tmp_lane_set] = port_list[i];
    }

    /* Get the flood control types and check if combined mode is supported */
    vector<int32_t> supported_flood_control_types(max_flood_control_types, 0);
    sai_s32_list_t values;
    values.count = max_flood_control_types;
    values.list = supported_flood_control_types.data();

    if (sai_query_attribute_enum_values_capability(gSwitchId, SAI_OBJECT_TYPE_VLAN,
                                                   SAI_VLAN_ATTR_UNKNOWN_UNICAST_FLOOD_CONTROL_TYPE,
                                                   &values) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_NOTICE("This device does not support unknown unicast flood control types");
    }
    else
    {
        for (uint32_t idx = 0; idx < values.count; idx++)
        {
            uuc_sup_flood_control_type.insert(static_cast<sai_vlan_flood_control_type_t>(values.list[idx]));
        }
    }


    supported_flood_control_types.assign(max_flood_control_types, 0);
    values.count = max_flood_control_types;
    values.list = supported_flood_control_types.data();

    if (sai_query_attribute_enum_values_capability(gSwitchId, SAI_OBJECT_TYPE_VLAN,
                                                   SAI_VLAN_ATTR_BROADCAST_FLOOD_CONTROL_TYPE,
                                                   &values) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_NOTICE("This device does not support broadcast flood control types");
    }
    else
    {
        for (uint32_t idx = 0; idx < values.count; idx++)
        {
            bc_sup_flood_control_type.insert(static_cast<sai_vlan_flood_control_type_t>(values.list[idx]));
        }
    }

    /* Get default 1Q bridge and default VLAN */
    vector<sai_attribute_t> attrs;
    attr.id = SAI_SWITCH_ATTR_DEFAULT_1Q_BRIDGE_ID;
    attrs.push_back(attr);
    attr.id = SAI_SWITCH_ATTR_DEFAULT_VLAN_ID;
    attrs.push_back(attr);

    status = sai_switch_api->get_switch_attribute(gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get default 1Q bridge and/or default VLAN, rv:%d", status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_SWITCH, status);
        if (handle_status != task_process_status::task_success)
        {
            throw runtime_error("PortsOrch initialization failure");
        }
    }

    m_default1QBridge = attrs[0].value.oid;
    m_defaultVlan = attrs[1].value.oid;

    /* Get System ports */
    getSystemPorts();

    removeDefaultVlanMembers();
    removeDefaultBridgePorts();

    /* Add port oper status notification support */
    DBConnector *notificationsDb = new DBConnector("ASIC_DB", 0);
    m_portStatusNotificationConsumer = new swss::NotificationConsumer(notificationsDb, "NOTIFICATIONS");
    auto portStatusNotificatier = new Notifier(m_portStatusNotificationConsumer, this, "PORT_STATUS_NOTIFICATIONS");
    Orch::addExecutor(portStatusNotificatier);

    if (gMySwitchType == "voq")
    {
        string tableName;
        //Add subscriber to process system LAG (System PortChannel) table
        tableName = CHASSIS_APP_LAG_TABLE_NAME;
        Orch::addExecutor(new Consumer(new SubscriberStateTable(chassisAppDb, tableName, TableConsumable::DEFAULT_POP_BATCH_SIZE, 0), this, tableName));
        m_tableVoqSystemLagTable = unique_ptr<Table>(new Table(chassisAppDb, CHASSIS_APP_LAG_TABLE_NAME));

        //Add subscriber to process system LAG member (System PortChannelMember) table
        tableName = CHASSIS_APP_LAG_MEMBER_TABLE_NAME;
        Orch::addExecutor(new Consumer(new SubscriberStateTable(chassisAppDb, tableName, TableConsumable::DEFAULT_POP_BATCH_SIZE, 0), this, tableName));
        m_tableVoqSystemLagMemberTable = unique_ptr<Table>(new Table(chassisAppDb, CHASSIS_APP_LAG_MEMBER_TABLE_NAME));

        m_lagIdAllocator = unique_ptr<LagIdAllocator> (new LagIdAllocator(chassisAppDb));
    }
}

void PortsOrch::removeDefaultVlanMembers()
{
    /* Get VLAN members in default VLAN */
    vector<sai_object_id_t> vlan_member_list(m_portCount + m_systemPortCount);

    sai_attribute_t attr;
    attr.id = SAI_VLAN_ATTR_MEMBER_LIST;
    attr.value.objlist.count = (uint32_t)vlan_member_list.size();
    attr.value.objlist.list = vlan_member_list.data();

    sai_status_t status = sai_vlan_api->get_vlan_attribute(m_defaultVlan, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get VLAN member list in default VLAN, rv:%d", status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_VLAN, status);
        if (handle_status != task_process_status::task_success)
        {
            throw runtime_error("PortsOrch initialization failure");
        }
    }

    /* Remove VLAN members in default VLAN */
    for (uint32_t i = 0; i < attr.value.objlist.count; i++)
    {
        status = sai_vlan_api->remove_vlan_member(vlan_member_list[i]);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove VLAN member, rv:%d", status);
            throw runtime_error("PortsOrch initialization failure");
        }
    }

    SWSS_LOG_NOTICE("Remove %d VLAN members from default VLAN", attr.value.objlist.count);
}

void PortsOrch::removeDefaultBridgePorts()
{
    /* Get bridge ports in default 1Q bridge
     * By default, there will be (m_portCount + m_systemPortCount) number of SAI_BRIDGE_PORT_TYPE_PORT
     * ports and one SAI_BRIDGE_PORT_TYPE_1Q_ROUTER port. The former type of
     * ports will be removed. */
    vector<sai_object_id_t> bridge_port_list(m_portCount + m_systemPortCount + 1);

    sai_attribute_t attr;
    attr.id = SAI_BRIDGE_ATTR_PORT_LIST;
    attr.value.objlist.count = (uint32_t)bridge_port_list.size();
    attr.value.objlist.list = bridge_port_list.data();

    sai_status_t status = sai_bridge_api->get_bridge_attribute(m_default1QBridge, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get bridge port list in default 1Q bridge, rv:%d", status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_BRIDGE, status);
        if (handle_status != task_process_status::task_success)
        {
            throw runtime_error("PortsOrch initialization failure");
        }
    }

    auto bridge_port_count = attr.value.objlist.count;

    /* Remove SAI_BRIDGE_PORT_TYPE_PORT bridge ports in default 1Q bridge */
    for (uint32_t i = 0; i < bridge_port_count; i++)
    {
        attr.id = SAI_BRIDGE_PORT_ATTR_TYPE;
        attr.value.s32 = SAI_NULL_OBJECT_ID;

        status = sai_bridge_api->get_bridge_port_attribute(bridge_port_list[i], 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to get bridge port type, rv:%d", status);
            task_process_status handle_status = handleSaiGetStatus(SAI_API_BRIDGE, status);
            if (handle_status != task_process_status::task_success)
            {
                throw runtime_error("PortsOrch initialization failure");
            }
        }
        if (attr.value.s32 == SAI_BRIDGE_PORT_TYPE_PORT)
        {
            status = sai_bridge_api->remove_bridge_port(bridge_port_list[i]);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove bridge port, rv:%d", status);
                throw runtime_error("PortsOrch initialization failure");
            }
        }
    }

    SWSS_LOG_NOTICE("Remove bridge ports from default 1Q bridge");
}

bool PortsOrch::allPortsReady()
{
    return m_initDone && m_pendingPortSet.empty();
}

/* Upon receiving PortInitDone, all the configured ports have been created in both hardware and kernel*/
bool PortsOrch::isInitDone()
{
    return m_initDone;
}

// Upon m_portConfigState transiting to PORT_CONFIG_DONE state, all physical ports have been "created" in hardware.
// Because of the asynchronous nature of sairedis calls, "create" in the strict sense means that the SAI create_port()
// function is called and the create port event has been pushed to the sairedis pipeline. Because sairedis pipeline
// preserves the order of the events received, any event that depends on the physical port being created first, e.g.,
// buffer profile apply, will be popped in the FIFO fashion, processed in the right order after the physical port is
// physically created in the ASIC, and thus can be issued safely when this function call returns true.
bool PortsOrch::isConfigDone()
{
    return m_portConfigState == PORT_CONFIG_DONE;
}

/* Use this method to retrieve the desired port if the destination port is a Gearbox port.
 * For example, if Gearbox is enabled on a specific physical interface,
 * the destination port may be the PHY or LINE side of the external PHY.
 * The original port id is returned if it's not a Gearbox configured port.
 */
bool PortsOrch::getDestPortId(sai_object_id_t src_port_id, dest_port_type_t port_type, sai_object_id_t &des_port_id)
{
    bool status = false;
    des_port_id = src_port_id;

    if (m_gearboxEnabled)
    {
        if (m_gearboxPortListLaneMap.find(src_port_id) != m_gearboxPortListLaneMap.end())
        {
            if (PHY_PORT_TYPE == port_type)
            {
                des_port_id = get<0>(m_gearboxPortListLaneMap[src_port_id]);
                SWSS_LOG_DEBUG("BOX: port id:%" PRIx64 " has a phy-side port id:%" PRIx64, src_port_id, des_port_id);
                status = true;
            }
            else if (LINE_PORT_TYPE == port_type)
            {
                des_port_id = get<1>(m_gearboxPortListLaneMap[src_port_id]);
                SWSS_LOG_DEBUG("BOX: port id:%" PRIx64 " has a line-side port id:%" PRIx64, src_port_id, des_port_id);
                status = true;
            }
        }
    }

    return status;
}

bool PortsOrch::isPortAdminUp(const string &alias)
{
    auto it = m_portList.find(alias);
    if (it == m_portList.end())
    {
        SWSS_LOG_ERROR("Failed to get Port object by port alias: %s", alias.c_str());
        return false;
    }

    return it->second.m_admin_state_up;
}

map<string, Port>& PortsOrch::getAllPorts()
{
    return m_portList;
}

bool PortsOrch::getPort(string alias, Port &p)
{
    SWSS_LOG_ENTER();

    if (m_portList.find(alias) == m_portList.end())
    {
        return false;
    }
    else
    {
        p = m_portList[alias];
        return true;
    }
}

bool PortsOrch::getPort(sai_object_id_t id, Port &port)
{
    SWSS_LOG_ENTER();

    auto itr = saiOidToAlias.find(id);
    if (itr == saiOidToAlias.end())
    {
        return false;
    }
    else
    {
        getPort(itr->second, port);
        return true;
    }

    return false;
}

void PortsOrch::increasePortRefCount(const string &alias)
{
    assert (m_port_ref_count.find(alias) != m_port_ref_count.end());
    m_port_ref_count[alias]++;
}

void PortsOrch::decreasePortRefCount(const string &alias)
{
    assert (m_port_ref_count.find(alias) != m_port_ref_count.end());
    m_port_ref_count[alias]--;
}

void PortsOrch::increaseBridgePortRefCount(Port &port)
{
    assert (m_bridge_port_ref_count.find(port.m_alias) != m_bridge_port_ref_count.end());
    m_bridge_port_ref_count[port.m_alias]++;
}

void PortsOrch::decreaseBridgePortRefCount(Port &port)
{
    assert (m_bridge_port_ref_count.find(port.m_alias) != m_bridge_port_ref_count.end());
    m_bridge_port_ref_count[port.m_alias]--;
}

bool PortsOrch::getBridgePortReferenceCount(Port &port)
{
    assert (m_bridge_port_ref_count.find(port.m_alias) != m_bridge_port_ref_count.end());
    return m_bridge_port_ref_count[port.m_alias];
}

bool PortsOrch::getPortByBridgePortId(sai_object_id_t bridge_port_id, Port &port)
{
    SWSS_LOG_ENTER();

    auto itr = saiOidToAlias.find(bridge_port_id);
    if (itr == saiOidToAlias.end())
    {
        return false;
    }
    else
    {
        getPort(itr->second, port);
        return true;
    }

    return false;
}

bool PortsOrch::addSubPort(Port &port, const string &alias, const string &vlan, const bool &adminUp, const uint32_t &mtu)
{
    SWSS_LOG_ENTER();

    size_t found = alias.find(VLAN_SUB_INTERFACE_SEPARATOR);
    if (found == string::npos)
    {
        SWSS_LOG_ERROR("%s is not a sub interface", alias.c_str());
        return false;
    }
    subIntf subIf(alias);
    string parentAlias = subIf.parentIntf();
    sai_vlan_id_t vlan_id;
    try
    {
        vlan_id = static_cast<sai_vlan_id_t>(stoul(vlan));
    }
    catch (const std::invalid_argument &e)
    {
        SWSS_LOG_ERROR("Invalid argument %s to %s()", vlan.c_str(), e.what());
        return false;
    }
    catch (const std::out_of_range &e)
    {
        SWSS_LOG_ERROR("Out of range argument %s to %s()", vlan.c_str(), e.what());
        return false;
    }
    if (vlan_id > MAX_VALID_VLAN_ID)
    {
        SWSS_LOG_ERROR("Sub interface %s Port object creation failed: invalid VLAN id %u", alias.c_str(), vlan_id);
        return false;
    }

    auto it = m_portList.find(parentAlias);
    if (it == m_portList.end())
    {
        SWSS_LOG_NOTICE("Sub interface %s Port object creation: parent port %s is not ready", alias.c_str(), parentAlias.c_str());
        return false;
    }
    Port &parentPort = it->second;

    Port p(alias, Port::SUBPORT);

    p.m_admin_state_up = adminUp;

    if (mtu)
    {
        p.m_mtu = mtu;
    }
    else
    {
        SWSS_LOG_NOTICE("Sub interface %s inherits mtu size %u from parent port %s", alias.c_str(), parentPort.m_mtu, parentAlias.c_str());
        p.m_mtu = parentPort.m_mtu;
    }

    switch (parentPort.m_type)
    {
        case Port::PHY:
            p.m_parent_port_id = parentPort.m_port_id;
            break;
        case Port::LAG:
            p.m_parent_port_id = parentPort.m_lag_id;
            break;
        default:
            SWSS_LOG_ERROR("Sub interface %s Port object creation failed: \
                    parent port %s of invalid type (must be physical port or LAG)", alias.c_str(), parentAlias.c_str());
            return false;
    }
    p.m_vlan_info.vlan_id = vlan_id;

    // Change hostif vlan tag for the parent port only when a first subport is created
    if (parentPort.m_child_ports.empty())
    {
        if (!setHostIntfsStripTag(parentPort, SAI_HOSTIF_VLAN_TAG_KEEP))
        {
            SWSS_LOG_ERROR("Failed to set %s for hostif of port %s",
                    hostif_vlan_tag[SAI_HOSTIF_VLAN_TAG_KEEP], parentPort.m_alias.c_str());
            return false;
        }
    }

    parentPort.m_child_ports.insert(alias);
    increasePortRefCount(parentPort.m_alias);

    m_portList[alias] = p;
    m_port_ref_count[alias] = 0;
    port = p;
    return true;
}

bool PortsOrch::removeSubPort(const string &alias)
{
    SWSS_LOG_ENTER();

    auto it = m_portList.find(alias);
    if (it == m_portList.end())
    {
        SWSS_LOG_WARN("Sub interface %s Port object not found", alias.c_str());
        return false;
    }
    Port &port = it->second;

    if (port.m_type != Port::SUBPORT)
    {
        SWSS_LOG_ERROR("Sub interface %s not of type sub port", alias.c_str());
        return false;
    }

    if (m_port_ref_count[alias] > 0)
    {
        SWSS_LOG_ERROR("Unable to remove sub interface %s: ref count %u", alias.c_str(), m_port_ref_count[alias]);
        return false;
    }

    Port parentPort;
    if (!getPort(port.m_parent_port_id, parentPort))
    {
        SWSS_LOG_WARN("Sub interface %s: parent Port object not found", alias.c_str());
    }

    if (!parentPort.m_child_ports.erase(alias))
    {
        SWSS_LOG_WARN("Sub interface %s not associated to parent port %s", alias.c_str(), parentPort.m_alias.c_str());
    }
    else
    {
        decreasePortRefCount(parentPort.m_alias);
    }
    m_portList[parentPort.m_alias] = parentPort;

    m_portList.erase(it);

    // Restore hostif vlan tag for the parent port when the last subport is removed
    if (parentPort.m_child_ports.empty())
    {
        if (parentPort.m_bridge_port_id == SAI_NULL_OBJECT_ID)
        {
            if (!setHostIntfsStripTag(parentPort, SAI_HOSTIF_VLAN_TAG_STRIP))
            {
                SWSS_LOG_ERROR("Failed to set %s for hostif of port %s",
                        hostif_vlan_tag[SAI_HOSTIF_VLAN_TAG_STRIP], parentPort.m_alias.c_str());
                return false;
            }
        }
    }

    return true;
}

void PortsOrch::updateChildPortsMtu(const Port &p, const uint32_t mtu)
{
    if (p.m_type != Port::PHY && p.m_type != Port::LAG)
    {
        return;
    }

    for (const auto &child_port : p.m_child_ports)
    {
        Port subp;
        if (!getPort(child_port, subp))
        {
            SWSS_LOG_WARN("Sub interface %s Port object not found", child_port.c_str());
            continue;
        }

        subp.m_mtu = mtu;
        m_portList[child_port] = subp;
        SWSS_LOG_NOTICE("Sub interface %s inherits mtu change %u from parent port %s", child_port.c_str(), mtu, p.m_alias.c_str());

        if (subp.m_rif_id)
        {
            gIntfsOrch->setRouterIntfsMtu(subp);
        }
    }
}

void PortsOrch::setPort(string alias, Port p)
{
    m_portList[alias] = p;
}

void PortsOrch::getCpuPort(Port &port)
{
    port = m_cpuPort;
}

bool PortsOrch::setPortAdminStatus(Port &port, bool state)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_ADMIN_STATE;
    attr.value.booldata = state;

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set admin status %s to port pid:%" PRIx64,
                       state ? "UP" : "DOWN", port.m_port_id);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_INFO("Set admin status %s to port pid:%" PRIx64,
                    state ? "UP" : "DOWN", port.m_port_id);

    setGearboxPortsAttr(port, SAI_PORT_ATTR_ADMIN_STATE, &state);

    return true;
}

bool PortsOrch::getPortAdminStatus(sai_object_id_t id, bool &up)
{
    SWSS_LOG_ENTER();

    getDestPortId(id, LINE_PORT_TYPE, id);

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_ADMIN_STATE;

    sai_status_t status = sai_port_api->get_port_attribute(id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get admin status for port pid:%" PRIx64, id);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
        if (handle_status != task_process_status::task_success)
        {
            return false;
        }
    }

    up = attr.value.booldata;

    return true;
}

bool PortsOrch::setPortMtu(sai_object_id_t id, sai_uint32_t mtu)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_MTU;
    /* mtu + 14 + 4 + 4 = 22 bytes */
    attr.value.u32 = (uint32_t)(mtu + sizeof(struct ether_header) + FCS_LEN + VLAN_TAG_LEN);

    sai_status_t status = sai_port_api->set_port_attribute(id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set MTU %u to port pid:%" PRIx64 ", rv:%d",
                attr.value.u32, id, status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    SWSS_LOG_INFO("Set MTU %u to port pid:%" PRIx64, attr.value.u32, id);
    return true;
}


bool PortsOrch::setPortTpid(sai_object_id_t id, sai_uint16_t tpid)
{
    SWSS_LOG_ENTER();
    sai_status_t status = SAI_STATUS_SUCCESS;
    sai_attribute_t attr;

    attr.id = SAI_PORT_ATTR_TPID;

    attr.value.u16 = (uint16_t)tpid;

    status = sai_port_api->set_port_attribute(id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set TPID 0x%x to port pid:%" PRIx64 ", rv:%d",
                attr.value.u16, id, status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    else
    {
        SWSS_LOG_NOTICE("Set TPID 0x%x to port pid:%" PRIx64, attr.value.u16, id);
    }
    return true;
}


bool PortsOrch::setPortFec(Port &port, sai_port_fec_mode_t mode)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_FEC_MODE;
    attr.value.s32 = mode;

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set fec mode %d to port pid:%" PRIx64, mode, port.m_port_id);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_INFO("Set fec mode %d to port pid:%" PRIx64, mode, port.m_port_id);

    setGearboxPortsAttr(port, SAI_PORT_ATTR_FEC_MODE, &mode);

    return true;
}

bool PortsOrch::getPortPfc(sai_object_id_t portId, uint8_t *pfc_bitmask)
{
    SWSS_LOG_ENTER();

    Port p;

    if (!getPort(portId, p))
    {
        SWSS_LOG_ERROR("Failed to get port object for port id 0x%" PRIx64, portId);
        return false;
    }

    *pfc_bitmask = p.m_pfc_bitmask;

    return true;
}

bool PortsOrch::setPortPfc(sai_object_id_t portId, uint8_t pfc_bitmask)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    Port p;

    if (!getPort(portId, p))
    {
        SWSS_LOG_ERROR("Failed to get port object for port id 0x%" PRIx64, portId);
        return false;
    }

    if (p.m_pfc_asym == SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_COMBINED)
    {
        attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL;
    }
    else if (p.m_pfc_asym == SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_SEPARATE)
    {
        attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_TX;
    }
    else
    {
        SWSS_LOG_ERROR("Incorrect asymmetric PFC mode: %u", p.m_pfc_asym);
        return false;
    }

    attr.value.u8 = pfc_bitmask;

    sai_status_t status = sai_port_api->set_port_attribute(portId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set PFC 0x%x to port id 0x%" PRIx64 " (rc:%d)", attr.value.u8, portId, status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    if (p.m_pfc_bitmask != pfc_bitmask)
    {
        p.m_pfc_bitmask = pfc_bitmask;
        m_portList[p.m_alias] = p;
    }

    return true;
}

bool PortsOrch::setPortPfcWatchdogStatus(sai_object_id_t portId, uint8_t pfcwd_bitmask)
{
    SWSS_LOG_ENTER();

    Port p;

    if (!getPort(portId, p))
    {
        SWSS_LOG_ERROR("Failed to get port object for port id 0x%" PRIx64, portId);
        return false;
    }
    
    p.m_pfcwd_sw_bitmask = pfcwd_bitmask;
   
    m_portList[p.m_alias] = p;

    SWSS_LOG_INFO("Set PFC watchdog port id=0x%" PRIx64 ", bitmast=0x%x", portId, pfcwd_bitmask);
    return true;
}

bool PortsOrch::getPortPfcWatchdogStatus(sai_object_id_t portId, uint8_t *pfcwd_bitmask)
{
    SWSS_LOG_ENTER();

    Port p;

    if (!pfcwd_bitmask || !getPort(portId, p))
    {
        SWSS_LOG_ERROR("Failed to get port object for port id 0x%" PRIx64, portId);
        return false;
    }
    
    *pfcwd_bitmask = p.m_pfcwd_sw_bitmask;
    
    return true;
}

bool PortsOrch::setPortPfcAsym(Port &port, string pfc_asym)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    uint8_t pfc = 0;

    if (!getPortPfc(port.m_port_id, &pfc))
    {
        return false;
    }

    auto found = pfc_asym_map.find(pfc_asym);
    if (found == pfc_asym_map.end())
    {
        SWSS_LOG_ERROR("Incorrect asymmetric PFC mode: %s", pfc_asym.c_str());
        return false;
    }

    auto new_pfc_asym = found->second;
    if (port.m_pfc_asym == new_pfc_asym)
    {
        SWSS_LOG_NOTICE("Already set asymmetric PFC mode: %s", pfc_asym.c_str());
        return true;
    }

    port.m_pfc_asym = new_pfc_asym;
    m_portList[port.m_alias] = port;

    attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_MODE;
    attr.value.s32 = (int32_t) port.m_pfc_asym;

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set PFC mode %d to port id 0x%" PRIx64 " (rc:%d)", port.m_pfc_asym, port.m_port_id, status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    if (!setPortPfc(port.m_port_id, pfc))
    {
        return false;
    }

    if (port.m_pfc_asym == SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_SEPARATE)
    {
        attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_RX;
        attr.value.u8 = static_cast<uint8_t>(0xff);

        sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set RX PFC 0x%x to port id 0x%" PRIx64 " (rc:%d)", attr.value.u8, port.m_port_id, status);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
            if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
        }
    }

    SWSS_LOG_INFO("Set asymmetric PFC %s to port id 0x%" PRIx64, pfc_asym.c_str(), port.m_port_id);

    return true;
}

/*
 * Name: bindUnbindAclTableGroup
 *
 * Description:
 *     To bind a port to ACL table we need to do two things.
 *     1. Create ACL table member, which maps
 *        ACL table group OID --> ACL table OID
 *     2. Set ACL table group OID as value port attribute.
 *
 *      This function performs the second step of binding.
 *
 *      Also, while unbinding we use this function to
 *      set port attribute value to SAI_NULL_OBJECT_ID
 *
 *      Port attribute name is derived from port type
 *
 * Return: true on success, false on failure
 */
bool PortsOrch::bindUnbindAclTableGroup(Port &port,
                                        bool ingress,
                                        bool bind)
{

    sai_attribute_t    attr;
    sai_status_t       status = SAI_STATUS_SUCCESS;
    string             bind_str = bind ? "bind" : "unbind";

    attr.value.oid = bind ? (ingress ? port.m_ingress_acl_table_group_id :
                                       port.m_egress_acl_table_group_id):
                            SAI_NULL_OBJECT_ID;
    switch (port.m_type)
    {
        case Port::PHY:
        {
            attr.id = ingress ?
                    SAI_PORT_ATTR_INGRESS_ACL : SAI_PORT_ATTR_EGRESS_ACL;
            status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
            if (SAI_STATUS_SUCCESS != status)
            {
                SWSS_LOG_ERROR("Failed to %s %s to ACL table group %" PRIx64 ", rv:%d",
                            bind_str.c_str(), port.m_alias.c_str(), attr.value.oid, status);
                task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
            break;
        }
        case Port::LAG:
        {
            attr.id = ingress ?
                    SAI_LAG_ATTR_INGRESS_ACL : SAI_LAG_ATTR_EGRESS_ACL;
            status = sai_lag_api->set_lag_attribute(port.m_lag_id, &attr);
            if (SAI_STATUS_SUCCESS != status)
            {
                SWSS_LOG_ERROR("Failed to %s %s to ACL table group %" PRIx64 ", rv:%d",
                            bind_str.c_str(), port.m_alias.c_str(), attr.value.oid, status);
                task_process_status handle_status = handleSaiSetStatus(SAI_API_LAG, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
            break;
        }
        case Port::VLAN:
        {
            attr.id = ingress ?
                    SAI_VLAN_ATTR_INGRESS_ACL : SAI_VLAN_ATTR_EGRESS_ACL;
            status =
                sai_vlan_api->set_vlan_attribute(port.m_vlan_info.vlan_oid,
                                                 &attr);
            if (SAI_STATUS_SUCCESS != status)
            {
                SWSS_LOG_ERROR("Failed to %s %s to ACL table group %" PRIx64 ", rv:%d",
                            bind_str.c_str(), port.m_alias.c_str(), attr.value.oid, status);
                task_process_status handle_status = handleSaiSetStatus(SAI_API_VLAN, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
            break;
        }
        default:
        {
            SWSS_LOG_ERROR("Failed to %s %s port with type %d",
                           bind_str.c_str(), port.m_alias.c_str(), port.m_type);
            return false;
        }
    }

    return true;
}

bool PortsOrch::unbindRemoveAclTableGroup(sai_object_id_t  port_oid,
                                          sai_object_id_t  acl_table_oid,
                                          acl_stage_type_t acl_stage)
{
    SWSS_LOG_ENTER();

    sai_status_t       status;
    bool               ingress = (acl_stage == ACL_STAGE_INGRESS);
    Port               port;

    if (!getPort(port_oid, port))
    {
        SWSS_LOG_ERROR("Failed to get port by port OID %" PRIx64, port_oid);
        return false;
    }


    sai_object_id_t &group_oid_ref =
            ingress? port.m_ingress_acl_table_group_id :
                     port.m_egress_acl_table_group_id;
    unordered_set<sai_object_id_t> &acl_list_ref =
            ingress ? port.m_ingress_acl_tables_uset :
                      port.m_egress_acl_tables_uset;

    if (SAI_NULL_OBJECT_ID == group_oid_ref)
    {
        assert(acl_list_ref.find(acl_table_oid) == acl_list_ref.end());
        return true;
    }
    assert(acl_list_ref.find(acl_table_oid) != acl_list_ref.end());
    acl_list_ref.erase(acl_table_oid);
    if (!acl_list_ref.empty())
    {
        // This port is in more than one acl table's port list
        // So, we need to preserve group OID
        SWSS_LOG_NOTICE("Preserving port OID %" PRIx64" ACL table grop ID", port_oid);
        setPort(port.m_alias, port);
        return true;
    }

    SWSS_LOG_NOTICE("Removing port OID %" PRIx64" ACL table group ID", port_oid);

    // Unbind ACL group
    if (!bindUnbindAclTableGroup(port, ingress, false))
    {
        SWSS_LOG_ERROR("Failed to remove ACL group ID from port");
        return false;
    }

    // Remove ACL group
    status = sai_acl_api->remove_acl_table_group(group_oid_ref);
    if (SAI_STATUS_SUCCESS != status)
    {
        SWSS_LOG_ERROR("Failed to remove ACL table group, rv:%d", status);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_ACL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    sai_acl_bind_point_type_t bind_type;
    if (!getSaiAclBindPointType(port.m_type, bind_type))
    {
        SWSS_LOG_ERROR("Unknown SAI ACL bind point type");
        return false;
    }
    gCrmOrch->decCrmAclUsedCounter(CrmResourceType::CRM_ACL_GROUP,
                                   ingress ? SAI_ACL_STAGE_INGRESS : SAI_ACL_STAGE_EGRESS,
                                   bind_type, group_oid_ref);

    group_oid_ref = SAI_NULL_OBJECT_ID;
    setPort(port.m_alias, port);
    return true;
}

bool PortsOrch::createBindAclTableGroup(sai_object_id_t  port_oid,
                                        sai_object_id_t  acl_table_oid,
                                        sai_object_id_t  &group_oid,
                                        acl_stage_type_t acl_stage)
{
    SWSS_LOG_ENTER();

    if (ACL_STAGE_UNKNOWN == acl_stage)
    {
        SWSS_LOG_ERROR("unknown ACL stage for table group creation");
        return false;
    }
    assert(ACL_STAGE_INGRESS == acl_stage || ACL_STAGE_EGRESS == acl_stage);

    sai_status_t    status;
    Port            port;
    bool            ingress = (ACL_STAGE_INGRESS == acl_stage) ?
                              true : false;
    if (!getPort(port_oid, port))
    {
        SWSS_LOG_ERROR("Failed to get port by port ID %" PRIx64, port_oid);
        return false;
    }

    unordered_set<sai_object_id_t> &acl_list_ref =
            ingress ? port.m_ingress_acl_tables_uset :
                      port.m_egress_acl_tables_uset;
    sai_object_id_t &group_oid_ref =
            ingress ? port.m_ingress_acl_table_group_id :
                      port.m_egress_acl_table_group_id;

    if (acl_list_ref.empty())
    {
        // Port ACL table group does not exist, create one
        assert(group_oid_ref == SAI_NULL_OBJECT_ID);
        sai_acl_bind_point_type_t bind_type;
        if (!getSaiAclBindPointType(port.m_type, bind_type))
        {
            SWSS_LOG_ERROR("Failed to bind ACL table to port %s with unknown type %d",
                        port.m_alias.c_str(), port.m_type);
            return false;
        }
        sai_object_id_t bp_list[] = { bind_type };

        vector<sai_attribute_t> group_attrs;
        sai_attribute_t group_attr;

        group_attr.id = SAI_ACL_TABLE_GROUP_ATTR_ACL_STAGE;
        group_attr.value.s32 = ingress ? SAI_ACL_STAGE_INGRESS :
                                         SAI_ACL_STAGE_EGRESS;
        group_attrs.push_back(group_attr);

        group_attr.id = SAI_ACL_TABLE_GROUP_ATTR_ACL_BIND_POINT_TYPE_LIST;
        group_attr.value.objlist.count = 1;
        group_attr.value.objlist.list = bp_list;
        group_attrs.push_back(group_attr);

        group_attr.id = SAI_ACL_TABLE_GROUP_ATTR_TYPE;
        group_attr.value.s32 = SAI_ACL_TABLE_GROUP_TYPE_PARALLEL;
        group_attrs.push_back(group_attr);

        status = sai_acl_api->create_acl_table_group(&group_oid_ref, gSwitchId,
                        (uint32_t)group_attrs.size(), group_attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create ACL table group, rv:%d", status);
            task_process_status handle_status = handleSaiCreateStatus(SAI_API_ACL, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
        assert(group_oid_ref != SAI_NULL_OBJECT_ID);

        gCrmOrch->incCrmAclUsedCounter(CrmResourceType::CRM_ACL_GROUP,
                        ingress ? SAI_ACL_STAGE_INGRESS :
                                  SAI_ACL_STAGE_EGRESS, bind_type);

        // Bind ACL table group
        if (!bindUnbindAclTableGroup(port, ingress, true))
        {
            return false;
        }

        SWSS_LOG_NOTICE("Create %s ACL table group and bind port %s to it",
                        ingress ? "ingress" : "egress", port.m_alias.c_str());
    }

    assert(group_oid_ref != SAI_NULL_OBJECT_ID);
    group_oid = group_oid_ref;
    acl_list_ref.insert(acl_table_oid);
    setPort(port.m_alias, port);

    return true;
}

bool PortsOrch::unbindAclTable(sai_object_id_t  port_oid,
                               sai_object_id_t  acl_table_oid,
                               sai_object_id_t  acl_group_member_oid,
                               acl_stage_type_t acl_stage)
{

    /*
     * Do the following in-order
     * 1. Delete ACL table group member
     * 2. Unbind ACL table group
     * 3. Delete ACL table group
     */
    sai_status_t status =
            sai_acl_api->remove_acl_table_group_member(acl_group_member_oid);
    if (status != SAI_STATUS_SUCCESS) {
        SWSS_LOG_ERROR("Failed to remove ACL group member: %" PRIu64 " ",
                       acl_group_member_oid);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_ACL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }


    Port port;
    if (getPort(port_oid, port))
    {
        decreasePortRefCount(port.m_alias);
    }

    if (!unbindRemoveAclTableGroup(port_oid, acl_table_oid, acl_stage)) {
        return false;
    }

    return true;
}

bool PortsOrch::bindAclTable(sai_object_id_t  port_oid,
                             sai_object_id_t  table_oid,
                             sai_object_id_t  &group_member_oid,
                             acl_stage_type_t acl_stage)
{
    SWSS_LOG_ENTER();
    /*
     * Do the following in-order
     * 1. Create ACL table group
     * 2. Bind ACL table group (set ACL table group ID on port)
     * 3. Create ACL table group member
     */

    if (table_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Invalid ACL table %" PRIx64, table_oid);
        return false;
    }

    sai_object_id_t    group_oid;
    sai_status_t       status;

    // Create an ACL table group and bind to port
    if (!createBindAclTableGroup(port_oid, table_oid, group_oid, acl_stage))
    {
        SWSS_LOG_ERROR("Fail to create or bind to port %" PRIx64 " ACL table group", port_oid);
        return false;
    }

    // Create an ACL group member with table_oid and group_oid
    vector<sai_attribute_t> member_attrs;

    sai_attribute_t member_attr;
    member_attr.id = SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID;
    member_attr.value.oid = group_oid;
    member_attrs.push_back(member_attr);

    member_attr.id = SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID;
    member_attr.value.oid = table_oid;
    member_attrs.push_back(member_attr);

    member_attr.id = SAI_ACL_TABLE_GROUP_MEMBER_ATTR_PRIORITY;
    member_attr.value.u32 = 100; // TODO: double check!
    member_attrs.push_back(member_attr);

    status = sai_acl_api->create_acl_table_group_member(&group_member_oid, gSwitchId, (uint32_t)member_attrs.size(), member_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create member in ACL table group %" PRIx64 " for ACL table %" PRIx64 ", rv:%d",
                group_oid, table_oid, status);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_ACL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    Port port;
    if (getPort(port_oid, port))
    {
        increasePortRefCount(port.m_alias);
    }

    return true;
}

bool PortsOrch::setPortPvid(Port &port, sai_uint32_t pvid)
{
    SWSS_LOG_ENTER();

    if(port.m_type == Port::TUNNEL)
    {
        SWSS_LOG_ERROR("pvid setting for tunnel %s is not allowed", port.m_alias.c_str());
        return true;
    }

    if(port.m_type == Port::SYSTEM)
    {
        SWSS_LOG_INFO("pvid setting for system port %s is not applicable", port.m_alias.c_str());
        return true;
    }

    if (port.m_rif_id)
    {
        SWSS_LOG_ERROR("pvid setting for router interface %s is not allowed", port.m_alias.c_str());
        return false;
    }

    vector<Port> portv;
    if (port.m_type == Port::PHY)
    {
        sai_attribute_t attr;
        attr.id = SAI_PORT_ATTR_PORT_VLAN_ID;
        attr.value.u32 = pvid;

        sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set pvid %u to port: %s", attr.value.u32, port.m_alias.c_str());
            task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
        SWSS_LOG_NOTICE("Set pvid %u to port: %s", attr.value.u32, port.m_alias.c_str());
    }
    else if (port.m_type == Port::LAG)
    {
        sai_attribute_t attr;
        attr.id = SAI_LAG_ATTR_PORT_VLAN_ID;
        attr.value.u32 = pvid;

        sai_status_t status = sai_lag_api->set_lag_attribute(port.m_lag_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set pvid %u to lag: %s", attr.value.u32, port.m_alias.c_str());
            task_process_status handle_status = handleSaiSetStatus(SAI_API_LAG, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
        SWSS_LOG_NOTICE("Set pvid %u to lag: %s", attr.value.u32, port.m_alias.c_str());
    }
    else
    {
        SWSS_LOG_ERROR("PortsOrch::setPortPvid port type %d not supported", port.m_type);
        return false;
    }

    port.m_port_vlan_id = (sai_vlan_id_t)pvid;
    return true;
}

bool PortsOrch::getPortPvid(Port &port, sai_uint32_t &pvid)
{
    /* Just return false if the router interface exists */
    if (port.m_rif_id)
    {
        SWSS_LOG_DEBUG("Router interface exists on %s, don't set pvid",
                      port.m_alias.c_str());
        return false;
    }

    pvid = port.m_port_vlan_id;
    return true;
}

bool PortsOrch::setHostIntfsStripTag(Port &port, sai_hostif_vlan_tag_t strip)
{
    SWSS_LOG_ENTER();
    vector<Port> portv;

    if(port.m_type == Port::TUNNEL)
    {
        return true;
    }

    /*
     * Before SAI_HOSTIF_VLAN_TAG_ORIGINAL is supported by libsai from all asic vendors,
     * the VLAN tag on hostif is explicitly controlled with SAI_HOSTIF_VLAN_TAG_STRIP &
     * SAI_HOSTIF_VLAN_TAG_KEEP attributes.
     */
    if (port.m_type == Port::PHY)
    {
        portv.push_back(port);
    }
    else if (port.m_type == Port::LAG)
    {
        getLagMember(port, portv);
    }
    else
    {
        SWSS_LOG_ERROR("port type %d not supported", port.m_type);
        return false;
    }

    for (const auto p: portv)
    {
        sai_attribute_t attr;
        attr.id = SAI_HOSTIF_ATTR_VLAN_TAG;
        attr.value.s32 = strip;

        sai_status_t status = sai_hostif_api->set_hostif_attribute(p.m_hif_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set %s to host interface %s",
                        hostif_vlan_tag[strip], p.m_alias.c_str());
            task_process_status handle_status = handleSaiSetStatus(SAI_API_HOSTIF, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
        SWSS_LOG_NOTICE("Set %s to host interface: %s",
                        hostif_vlan_tag[strip], p.m_alias.c_str());
    }

    return true;
}

bool PortsOrch::isSpeedSupported(const std::string& alias, sai_object_id_t port_id, sai_uint32_t speed)
{
    // This method will return false iff we get a list of supported speeds and the requested speed
    // is not supported
    // Otherwise the method will return true (even if we received errors)
    initPortSupportedSpeeds(alias, port_id);

    const auto &supp_speeds = m_portSupportedSpeeds[port_id];
    if (supp_speeds.empty())
    {
        // we don't have the list for this port, so return true to change speed anyway
        return true;
    }

    return std::find(supp_speeds.begin(), supp_speeds.end(), speed) != supp_speeds.end();
}

void PortsOrch::getPortSupportedSpeeds(const std::string& alias, sai_object_id_t port_id, PortSupportedSpeeds &supported_speeds)
{
    sai_attribute_t attr;
    sai_status_t status;
    const auto size_guess = 25; // Guess the size which could be enough

    PortSupportedSpeeds speeds(size_guess);

    // two attempts to get our value, first with the guess, other with the returned value
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        attr.id = SAI_PORT_ATTR_SUPPORTED_SPEED;
        attr.value.u32list.count = static_cast<uint32_t>(speeds.size());
        attr.value.u32list.list = speeds.data();

        status = sai_port_api->get_port_attribute(port_id, 1, &attr);
        if (status != SAI_STATUS_BUFFER_OVERFLOW)
        {
            break;
        }

        // if our guess was wrong, retry with the correct value
        speeds.resize(attr.value.u32list.count);
    }

    if (status == SAI_STATUS_SUCCESS)
    {
        speeds.resize(attr.value.u32list.count);
        supported_speeds.swap(speeds);
    }
    else
    {
        if (status == SAI_STATUS_BUFFER_OVERFLOW)
        {
            // something went wrong in SAI implementation
            SWSS_LOG_ERROR("Failed to get supported speed list for port %s id=%" PRIx64 ". Not enough container size",
                           alias.c_str(), port_id);
        }
        else if (SAI_STATUS_IS_ATTR_NOT_SUPPORTED(status) ||
                 SAI_STATUS_IS_ATTR_NOT_IMPLEMENTED(status) ||
                 status == SAI_STATUS_NOT_IMPLEMENTED)
        {
            // unable to validate speed if attribute is not supported on platform
            // assuming input value is correct
            SWSS_LOG_WARN("Unable to validate speed for port %s id=%" PRIx64 ". Not supported by platform",
                          alias.c_str(), port_id);
        }
        else
        {
            SWSS_LOG_ERROR("Failed to get a list of supported speeds for port %s id=%" PRIx64 ". Error=%d",
                           alias.c_str(), port_id, status);
        }

        supported_speeds.clear(); // return empty
    }
}

void PortsOrch::initPortSupportedSpeeds(const std::string& alias, sai_object_id_t port_id)
{
    // If port supported speeds map already contains the information, save the SAI call
    if (m_portSupportedSpeeds.count(port_id))
    {
        return;
    }
    PortSupportedSpeeds supported_speeds;
    getPortSupportedSpeeds(alias, port_id, supported_speeds);
    m_portSupportedSpeeds[port_id] = supported_speeds;
    vector<FieldValueTuple> v;
    std::string supported_speeds_str = swss::join(',', supported_speeds.begin(), supported_speeds.end());
    v.emplace_back(std::make_pair("supported_speeds", supported_speeds_str));
    m_portStateTable.set(alias, v);
}

/*
 * If Gearbox is enabled and this is a Gearbox port then set the attributes accordingly.
 */
bool PortsOrch::setGearboxPortsAttr(Port &port, sai_port_attr_t id, void *value)
{
    bool status;

    status = setGearboxPortAttr(port, PHY_PORT_TYPE, id, value);

    if (status == true)
    {
        status = setGearboxPortAttr(port, LINE_PORT_TYPE, id, value);
    }

    return status;
}

/*
 * If Gearbox is enabled and this is a Gearbox port then set the specific lane attribute.
 * Note: the appl_db is also updated (Gearbox config_db tables are TBA).
 */
bool PortsOrch::setGearboxPortAttr(Port &port, dest_port_type_t port_type, sai_port_attr_t id, void *value)
{
    sai_status_t status = SAI_STATUS_SUCCESS;
    sai_object_id_t dest_port_id;
    sai_attribute_t attr;
    string speed_attr;
    sai_uint32_t speed = 0;

    SWSS_LOG_ENTER();

    if (m_gearboxEnabled)
    {
        if (getDestPortId(port.m_port_id, port_type, dest_port_id) == true)
        {
            switch (id)
            {
                case SAI_PORT_ATTR_FEC_MODE:
                    attr.id = id;
                    attr.value.s32 = *static_cast<sai_int32_t*>(value);
                    SWSS_LOG_NOTICE("BOX: Set %s FEC_MODE %d", port.m_alias.c_str(), attr.value.s32);
                    break;
                case SAI_PORT_ATTR_ADMIN_STATE:
                    attr.id = id;
                    attr.value.booldata = *static_cast<bool*>(value);
                    SWSS_LOG_NOTICE("BOX: Set %s ADMIN_STATE %d", port.m_alias.c_str(), attr.value.booldata);
                    break;
                case SAI_PORT_ATTR_SPEED:
                    switch (port_type)
                    {
                        case PHY_PORT_TYPE:
                            speed_attr = "system_speed";
                            break;
                        case LINE_PORT_TYPE:
                            speed_attr = "line_speed";
                            break;
                        default:
                            return false;
                    }

                    speed = *static_cast<sai_int32_t*>(value);
                    if (isSpeedSupported(port.m_alias, dest_port_id, speed))
                    {
                        // Gearbox may not implement speed check, so
                        // invalidate speed if it doesn't make sense.
                        if (to_string(speed).size() < 5)
                        {
                            speed = 0;
                        }

                        attr.id = SAI_PORT_ATTR_SPEED;
                        attr.value.u32 = speed;
                    }
                    SWSS_LOG_NOTICE("BOX: Set %s lane %s %d", port.m_alias.c_str(), speed_attr.c_str(), speed);
                    break;
                default:
                    return false;
            }

            status = sai_port_api->set_port_attribute(dest_port_id, &attr);
            if (status == SAI_STATUS_SUCCESS)
            {
                if (id == SAI_PORT_ATTR_SPEED)
                {
                    string key = "phy:"+to_string(m_gearboxInterfaceMap[port.m_index].phy_id)+":ports:"+to_string(port.m_index);
                    m_gearboxTable->hset(key, speed_attr, to_string(speed));
                    SWSS_LOG_NOTICE("BOX: Updated APPL_DB key:%s %s %d", key.c_str(), speed_attr.c_str(), speed);
                }
            }
            else
            {
                SWSS_LOG_ERROR("BOX: Failed to set %s port attribute %d", port.m_alias.c_str(), id);
                task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
        }
    }

    return true;
}

task_process_status PortsOrch::setPortSpeed(Port &port, sai_uint32_t speed)
{
    sai_attribute_t attr;
    sai_status_t status;

    SWSS_LOG_ENTER();

    attr.id = SAI_PORT_ATTR_SPEED;
    attr.value.u32 = speed;

    status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        return handleSaiSetStatus(SAI_API_PORT, status);
    }

    setGearboxPortsAttr(port, SAI_PORT_ATTR_SPEED, &speed);
    return task_success;
}

bool PortsOrch::getPortSpeed(sai_object_id_t id, sai_uint32_t &speed)
{
    SWSS_LOG_ENTER();

    getDestPortId(id, LINE_PORT_TYPE, id);

    sai_attribute_t attr;
    sai_status_t status;

    attr.id = SAI_PORT_ATTR_SPEED;
    attr.value.u32 = 0;

    status = sai_port_api->get_port_attribute(id, 1, &attr);

    if (status == SAI_STATUS_SUCCESS)
    {
        speed = attr.value.u32;
    }
    else
    {
        task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
        if (handle_status != task_process_status::task_success)
        {
            return false;
        }
    }

    return true;
}

task_process_status PortsOrch::setPortAdvSpeeds(sai_object_id_t port_id, std::vector<sai_uint32_t>& speed_list)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    sai_status_t status;

    attr.id = SAI_PORT_ATTR_ADVERTISED_SPEED;
    attr.value.u32list.list  = speed_list.data();
    attr.value.u32list.count = static_cast<uint32_t>(speed_list.size());

    status = sai_port_api->set_port_attribute(port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        return handleSaiSetStatus(SAI_API_PORT, status);
    }

    return task_success;
}

task_process_status PortsOrch::setPortInterfaceType(sai_object_id_t port_id, sai_port_interface_type_t interface_type)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    sai_status_t status;

    attr.id = SAI_PORT_ATTR_INTERFACE_TYPE;
    attr.value.u32 = static_cast<uint32_t>(interface_type);

    status = sai_port_api->set_port_attribute(port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        return handleSaiSetStatus(SAI_API_PORT, status);
    }

    return task_success;
}

task_process_status PortsOrch::setPortAdvInterfaceTypes(sai_object_id_t port_id, std::vector<uint32_t> &interface_types)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    sai_status_t status;

    attr.id = SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE;
    attr.value.u32list.list  = interface_types.data();
    attr.value.u32list.count = static_cast<uint32_t>(interface_types.size());

    status = sai_port_api->set_port_attribute(port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        return handleSaiSetStatus(SAI_API_PORT, status);
    }

    return task_success;
}

bool PortsOrch::getQueueTypeAndIndex(sai_object_id_t queue_id, string &type, uint8_t &index)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr[2];
    attr[0].id = SAI_QUEUE_ATTR_TYPE;
    attr[1].id = SAI_QUEUE_ATTR_INDEX;

    sai_status_t status = sai_queue_api->get_queue_attribute(queue_id, 2, attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get queue type and index for queue %" PRIu64 " rv:%d", queue_id, status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_QUEUE, status);
        if (handle_status != task_process_status::task_success)
        {
            return false;
        }
    }

    switch (attr[0].value.s32)
    {
    case SAI_QUEUE_TYPE_ALL:
        type = "SAI_QUEUE_TYPE_ALL";
        break;
    case SAI_QUEUE_TYPE_UNICAST:
        type = "SAI_QUEUE_TYPE_UNICAST";
        break;
    case SAI_QUEUE_TYPE_MULTICAST:
        type = "SAI_QUEUE_TYPE_MULTICAST";
        break;
    default:
        SWSS_LOG_ERROR("Got unsupported queue type %d for %" PRIu64 " queue", attr[0].value.s32, queue_id);
        throw runtime_error("Got unsupported queue type");
    }

    index = attr[1].value.u8;

    return true;
}

task_process_status PortsOrch::setPortAutoNeg(sai_object_id_t id, int an)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_AUTO_NEG_MODE;
    attr.value.booldata = (an == 1 ? true : false);

    sai_status_t status = sai_port_api->set_port_attribute(id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set AutoNeg %u to port pid:%" PRIx64, attr.value.booldata, id);
        return handleSaiSetStatus(SAI_API_PORT, status);
    }
    SWSS_LOG_INFO("Set AutoNeg %u to port pid:%" PRIx64, attr.value.booldata, id);
    return task_success;
}

bool PortsOrch::setHostIntfsOperStatus(const Port& port, bool isUp) const
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_HOSTIF_ATTR_OPER_STATUS;
    attr.value.booldata = isUp;

    sai_status_t status = sai_hostif_api->set_hostif_attribute(port.m_hif_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to set operation status %s to host interface %s",
                isUp ? "UP" : "DOWN", port.m_alias.c_str());
        return false;
    }

    SWSS_LOG_NOTICE("Set operation status %s to host interface %s",
            isUp ? "UP" : "DOWN", port.m_alias.c_str());

    return true;
}

bool PortsOrch::createVlanHostIntf(Port& vl, string hostif_name)
{
    SWSS_LOG_ENTER();

    if (vl.m_vlan_info.host_intf_id != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Host interface already assigned to VLAN %d", vl.m_vlan_info.vlan_id);
        return false;
    }

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    attr.id = SAI_HOSTIF_ATTR_TYPE;
    attr.value.s32 = SAI_HOSTIF_TYPE_NETDEV;
    attrs.push_back(attr);

    attr.id = SAI_HOSTIF_ATTR_OBJ_ID;
    attr.value.oid = vl.m_vlan_info.vlan_oid;
    attrs.push_back(attr);

    attr.id = SAI_HOSTIF_ATTR_NAME;
    if (hostif_name.length() >= SAI_HOSTIF_NAME_SIZE)
    {
        SWSS_LOG_WARN("Host interface name %s is too long and will be truncated to %d bytes", hostif_name.c_str(), SAI_HOSTIF_NAME_SIZE - 1);
    }
    strncpy(attr.value.chardata, hostif_name.c_str(), SAI_HOSTIF_NAME_SIZE);
    attr.value.chardata[SAI_HOSTIF_NAME_SIZE - 1] = '\0';
    attrs.push_back(attr);

    sai_status_t status = sai_hostif_api->create_hostif(&vl.m_vlan_info.host_intf_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create host interface %s for VLAN %d", hostif_name.c_str(), vl.m_vlan_info.vlan_id);
        return false;
    }

    m_portList[vl.m_alias] = vl;

    return true;
}

bool PortsOrch::removeVlanHostIntf(Port vl)
{
    sai_status_t status = sai_hostif_api->remove_hostif(vl.m_vlan_info.host_intf_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove VLAN %d host interface", vl.m_vlan_info.vlan_id);
        return false;
    }

    return true;
}

void PortsOrch::updateDbPortOperStatus(const Port& port, sai_port_oper_status_t status) const
{
    SWSS_LOG_ENTER();

    if(port.m_type == Port::TUNNEL)
    {
        VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
        tunnel_orch->updateDbTunnelOperStatus(port.m_alias, status);
        return;
    }

    vector<FieldValueTuple> tuples;
    FieldValueTuple tuple("oper_status", oper_status_strings.at(status));
    tuples.push_back(tuple);
    m_portTable->set(port.m_alias, tuples);
}

bool PortsOrch::addPort(const set<int> &lane_set, uint32_t speed, int an, string fec_mode)
{
    SWSS_LOG_ENTER();

    vector<uint32_t> lanes(lane_set.begin(), lane_set.end());

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_PORT_ATTR_SPEED;
    attr.value.u32 = speed;
    attrs.push_back(attr);

    attr.id = SAI_PORT_ATTR_HW_LANE_LIST;
    attr.value.u32list.list = lanes.data();
    attr.value.u32list.count = static_cast<uint32_t>(lanes.size());
    attrs.push_back(attr);

    if (an == true)
    {
        attr.id = SAI_PORT_ATTR_AUTO_NEG_MODE;
        attr.value.booldata = true;
        attrs.push_back(attr);
    }

    if (!fec_mode.empty())
    {
        attr.id = SAI_PORT_ATTR_FEC_MODE;
        attr.value.u32 = fec_mode_map[fec_mode];
        attrs.push_back(attr);
    }

    sai_object_id_t port_id;
    sai_status_t status = sai_port_api->create_port(&port_id, gSwitchId, static_cast<uint32_t>(attrs.size()), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create port with the speed %u, rv:%d", speed, status);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    m_portListLaneMap[lane_set] = port_id;
    m_portCount++;

    SWSS_LOG_NOTICE("Create port %" PRIx64 " with the speed %u", port_id, speed);

    return true;
}

sai_status_t PortsOrch::removePort(sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    Port port;

    /*
     * Make sure to bring down admin state.
     * SET would have replaced with DEL
     */
    if (getPort(port_id, port))
    {
        setPortAdminStatus(port, false);
    }
    /* else : port is in default state or not yet created */

    /*
     * Remove port serdes (if exists) before removing port since this
     * reference is dependency.
     */

    removePortSerdesAttribute(port_id);

    sai_status_t status = sai_port_api->remove_port(port_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        return status;
    }

    m_portCount--;
    m_portSupportedSpeeds.erase(port_id);
    SWSS_LOG_NOTICE("Remove port %" PRIx64, port_id);

    return status;
}

string PortsOrch::getQueueWatermarkFlexCounterTableKey(string key)
{
    return string(QUEUE_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP) + ":" + key;
}

string PortsOrch::getPriorityGroupWatermarkFlexCounterTableKey(string key)
{
    return string(PG_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP) + ":" + key;
}

string PortsOrch::getPriorityGroupDropPacketsFlexCounterTableKey(string key)
{
    return string(PG_DROP_STAT_COUNTER_FLEX_COUNTER_GROUP) + ":" + key;
}

bool PortsOrch::initPort(const string &alias, const string &role, const int index, const set<int> &lane_set)
{
    SWSS_LOG_ENTER();

    /* Determine if the lane combination exists in switch */
    if (m_portListLaneMap.find(lane_set) != m_portListLaneMap.end())
    {
        sai_object_id_t id = m_portListLaneMap[lane_set];

        /* Determine if the port has already been initialized before */
        if (m_portList.find(alias) != m_portList.end() && m_portList[alias].m_port_id == id)
        {
            SWSS_LOG_DEBUG("Port has already been initialized before alias:%s", alias.c_str());
        }
        else
        {
            Port p(alias, Port::PHY);

            p.m_index = index;
            p.m_port_id = id;

            /* Initialize the port and create corresponding host interface */
            if (initializePort(p))
            {
                /* Create associated Gearbox lane mapping */
                initGearboxPort(p);

                /* Add port to port list */
                m_portList[alias] = p;
                saiOidToAlias[id] = alias;
                m_port_ref_count[alias] = 0;
                m_portOidToIndex[id] = index;

                /* Add port name map to counter table */
                FieldValueTuple tuple(p.m_alias, sai_serialize_object_id(p.m_port_id));
                vector<FieldValueTuple> fields;
                fields.push_back(tuple);
                m_counterTable->set("", fields);

                // Install a flex counter for this port to track stats
                auto flex_counters_orch = gDirectory.get<FlexCounterOrch*>();
                /* Delay installing the counters if they are yet enabled
                If they are enabled, install the counters immediately */
                if (flex_counters_orch->getPortCountersState())
                {
                    auto port_counter_stats = generateCounterStats(PORT_STAT_COUNTER_FLEX_COUNTER_GROUP);
                    port_stat_manager.setCounterIdList(p.m_port_id, CounterType::PORT, port_counter_stats);
                }
                if (flex_counters_orch->getPortBufferDropCountersState())
                {
                    auto port_buffer_drop_stats = generateCounterStats(PORT_BUFFER_DROP_STAT_FLEX_COUNTER_GROUP);
                    port_buffer_drop_stat_manager.setCounterIdList(p.m_port_id, CounterType::PORT, port_buffer_drop_stats);
                }

                /* when a port is added and priority group map counter is enabled --> we need to add pg counter for it */
                if (m_isPriorityGroupMapGenerated)
                {
                    generatePriorityGroupMapPerPort(p);
                }

                /* when a port is added and queue map counter is enabled --> we need to add queue map counter for it */
                if (m_isQueueMapGenerated)
                {
                    generateQueueMapPerPort(p);
                }

                PortUpdate update = { p, true };
                notify(SUBJECT_TYPE_PORT_CHANGE, static_cast<void *>(&update));

                m_portList[alias].m_init = true;

                if (role == "Rec" || role == "Inb")
                {
                    m_recircPortRole[alias] = role;
                }

                SWSS_LOG_NOTICE("Initialized port %s", alias.c_str());
            }
            else
            {
                SWSS_LOG_ERROR("Failed to initialize port %s", alias.c_str());
                return false;
            }
        }
    }
    else
    {
        SWSS_LOG_ERROR("Failed to locate port lane combination alias:%s", alias.c_str());
        return false;
    }

    return true;
}

void PortsOrch::deInitPort(string alias, sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    Port p;

    if (!getPort(port_id, p))
    {
        SWSS_LOG_ERROR("Failed to get port object for port id 0x%" PRIx64, port_id);
        return;
    }

    /* remove port from flex_counter_table for updating counters  */
    auto flex_counters_orch = gDirectory.get<FlexCounterOrch*>();
    if ((flex_counters_orch->getPortCountersState()))
    {
        port_stat_manager.clearCounterIdList(p.m_port_id);
    }

    if (flex_counters_orch->getPortBufferDropCountersState())
    {
        port_buffer_drop_stat_manager.clearCounterIdList(p.m_port_id);
    }

    /* remove pg port counters */
    if (m_isPriorityGroupMapGenerated)
    {
        removePriorityGroupMapPerPort(p);
    }

    /* remove queue port counters */
    if (m_isQueueMapGenerated)
    {
        removeQueueMapPerPort(p);
    }

    /* remove port name map from counter table */
    m_counterTable->hdel("", alias);

    /* Remove the associated port serdes attribute */
    removePortSerdesAttribute(p.m_port_id);

    m_portList[alias].m_init = false;
    SWSS_LOG_NOTICE("De-Initialized port %s", alias.c_str());
}

bool PortsOrch::bake()
{
    SWSS_LOG_ENTER();

    // Check the APP_DB port table for warm reboot
    vector<FieldValueTuple> tuples;
    string value;
    bool foundPortConfigDone = m_portTable->hget("PortConfigDone", "count", value);
    uintmax_t portCount;
    char* endPtr = NULL;
    SWSS_LOG_NOTICE("foundPortConfigDone = %d", foundPortConfigDone);

    bool foundPortInitDone = m_portTable->get("PortInitDone", tuples);
    SWSS_LOG_NOTICE("foundPortInitDone = %d", foundPortInitDone);

    vector<string> keys;
    m_portTable->getKeys(keys);
    SWSS_LOG_NOTICE("m_portTable->getKeys %zd", keys.size());

    if (!foundPortConfigDone || !foundPortInitDone)
    {
        SWSS_LOG_NOTICE("No port table, fallback to cold start");
        cleanPortTable(keys);
        return false;
    }

    portCount = strtoumax(value.c_str(), &endPtr, 0);
    SWSS_LOG_NOTICE("portCount = %" PRIuMAX ", m_portCount = %u", portCount, m_portCount);
    if (portCount != keys.size() - 2)
    {
        // Invalid port table
        SWSS_LOG_ERROR("Invalid port table: portCount, expecting %" PRIuMAX ", got %zu",
                portCount, keys.size() - 2);

        cleanPortTable(keys);
        return false;
    }

    for (const auto& alias: keys)
    {
        if (alias == "PortConfigDone" || alias == "PortInitDone")
        {
            continue;
        }

        m_pendingPortSet.emplace(alias);
    }

    addExistingData(m_portTable.get());
    addExistingData(APP_LAG_TABLE_NAME);
    addExistingData(APP_LAG_MEMBER_TABLE_NAME);
    addExistingData(APP_VLAN_TABLE_NAME);
    addExistingData(APP_VLAN_MEMBER_TABLE_NAME);

    return true;
}

// Clean up port table
void PortsOrch::cleanPortTable(const vector<string>& keys)
{
    for (auto& key : keys)
    {
        m_portTable->del(key);
    }
}

void PortsOrch::removePortFromLanesMap(string alias)
{

    for (auto it = m_lanesAliasSpeedMap.begin(); it != m_lanesAliasSpeedMap.end(); it++)
    {
        if (get<0>(it->second) == alias)
        {
            SWSS_LOG_NOTICE("Removing port %s from lanes map", alias.c_str());
            it = m_lanesAliasSpeedMap.erase(it);
            break;
        }
    }
}

void PortsOrch::removePortFromPortListMap(sai_object_id_t port_id)
{

    for (auto it = m_portListLaneMap.begin(); it != m_portListLaneMap.end(); it++)
    {
        if (it->second == port_id)
        {
            SWSS_LOG_NOTICE("Removing port-id %" PRIx64 " from port list map", port_id);
            it = m_portListLaneMap.erase(it);
            break;
        }
    }
}


void PortsOrch::doPortTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;

        string alias = kfvKey(t);
        string op = kfvOp(t);

        if (alias == "PortConfigDone")
        {
            if (m_portConfigState != PORT_CONFIG_MISSING)
            {
                // Already received, ignore this task
                it = consumer.m_toSync.erase(it);
                continue;
            }

            m_portConfigState = PORT_CONFIG_RECEIVED;

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "count")
                {
                    m_portCount = to_uint<uint32_t>(fvValue(i));
                }
            }
        }

        /* Get notification from application */
        /* portsyncd application:
         * When portsorch receives 'PortInitDone' message, it indicates port initialization
         * procedure is done. Before port initialization procedure, none of other tasks
         * are executed.
         */
        if (alias == "PortInitDone")
        {
            /* portsyncd restarting case:
             * When portsyncd restarts, duplicate notifications may be received.
             */
            if (!m_initDone)
            {
                addSystemPorts();
                m_initDone = true;
                SWSS_LOG_INFO("Get PortInitDone notification from portsyncd.");
            }

            it = consumer.m_toSync.erase(it);
            return;

        }

        if (op == SET_COMMAND)
        {
            set<int> lane_set;
            vector<uint32_t> attr_val;
            map<sai_port_serdes_attr_t, vector<uint32_t>> serdes_attr;
            typedef pair<sai_port_serdes_attr_t, vector<uint32_t>> serdes_attr_pair;
            string admin_status;
            string fec_mode;
            string pfc_asym;
            uint32_t mtu = 0;
            uint32_t speed = 0;
            string learn_mode;
            string an_str;
            int an = -1;
            int index = -1;
            string role;
            string adv_speeds_str;
            string interface_type_str;
            string adv_interface_types_str;
            vector<uint32_t> adv_speeds;
            sai_port_interface_type_t interface_type;
            vector<uint32_t> adv_interface_types;
            string tpid_string;
            uint16_t tpid = 0;

            for (auto i : kfvFieldsValues(t))
            {
                attr_val.clear();
                /* Set interface index */
                if (fvField(i) == "index")
                {
                    index = (int)stoul(fvValue(i));
                }
                /* Get lane information of a physical port and initialize the port */
                else if (fvField(i) == "lanes")
                {
                    string lane_str;
                    istringstream iss(fvValue(i));

                    while (getline(iss, lane_str, ','))
                    {
                        int lane = stoi(lane_str);
                        lane_set.insert(lane);
                    }
                }
                /* Set port admin status */
                else if (fvField(i) == "admin_status")
                {
                    admin_status = fvValue(i);
                }
                /* Set port MTU */
                else if (fvField(i) == "mtu")
                {
                    mtu = (uint32_t)stoul(fvValue(i));
                }
                /* Set port TPID */
                if (fvField(i) == "tpid")
                {
                    tpid_string = fvValue(i);
                    // Need to get rid of the leading 0x
                    tpid_string.erase(0,2);
                    tpid = (uint16_t)stoi(tpid_string, 0, 16);
                    SWSS_LOG_DEBUG("Handling TPID to 0x%x, string value:%s", tpid, tpid_string.c_str());
                }
                /* Set port speed */
                else if (fvField(i) == "speed")
                {
                    speed = (uint32_t)stoul(fvValue(i));
                }
                /* Set port fec */
                else if (fvField(i) == "fec")
                {
                    fec_mode = fvValue(i);
                }
                /* Get port fdb learn mode*/
                else if (fvField(i) == "learn_mode")
                {
                    learn_mode = fvValue(i);
                }
                /* Set port asymmetric PFC */
                else if (fvField(i) == "pfc_asym")
                {
                    pfc_asym = fvValue(i);
                }
                /* Set autoneg and ignore the port speed setting */
                else if (fvField(i) == "autoneg")
                {
                    an_str = fvValue(i);
                }
                /* Set advertised speeds */
                else if (fvField(i) == "adv_speeds")
                {
                    adv_speeds_str = fvValue(i);
                }
                /* Set interface type */
                else if (fvField(i) == "interface_type")
                {
                    interface_type_str = fvValue(i);
                }
                /* Set advertised interface type */
                else if (fvField(i) == "adv_interface_types")
                {
                    adv_interface_types_str = fvValue(i);
                }
                /* Set port serdes Pre-emphasis */
                else if (fvField(i) == "preemphasis")
                {
                    getPortSerdesVal(fvValue(i), attr_val);
                    serdes_attr.insert(serdes_attr_pair(SAI_PORT_SERDES_ATTR_PREEMPHASIS, attr_val));
                }
                /* Set port serdes idriver */
                else if (fvField(i) == "idriver")
                {
                    getPortSerdesVal(fvValue(i), attr_val);
                    serdes_attr.insert(serdes_attr_pair(SAI_PORT_SERDES_ATTR_IDRIVER, attr_val));
                }
                /* Set port serdes ipredriver */
                else if (fvField(i) == "ipredriver")
                {
                    getPortSerdesVal(fvValue(i), attr_val);
                    serdes_attr.insert(serdes_attr_pair(SAI_PORT_SERDES_ATTR_IPREDRIVER, attr_val));
                }
                /* Set port serdes pre1 */
                else if (fvField(i) == "pre1")
                {
                    getPortSerdesVal(fvValue(i), attr_val);
                    serdes_attr.insert(serdes_attr_pair(SAI_PORT_SERDES_ATTR_TX_FIR_PRE1, attr_val));
                }
                /* Set port serdes pre2 */
                else if (fvField(i) == "pre2")
                {
                    getPortSerdesVal(fvValue(i), attr_val);
                    serdes_attr.insert(serdes_attr_pair(SAI_PORT_SERDES_ATTR_TX_FIR_PRE2, attr_val));
                }
                /* Set port serdes pre3 */
                else if (fvField(i) == "pre3")
                {
                    getPortSerdesVal(fvValue(i), attr_val);
                    serdes_attr.insert(serdes_attr_pair(SAI_PORT_SERDES_ATTR_TX_FIR_PRE3, attr_val));
                }
                /* Set port serdes main */
                else if (fvField(i) == "main")
                {
                    getPortSerdesVal(fvValue(i), attr_val);
                    serdes_attr.insert(serdes_attr_pair(SAI_PORT_SERDES_ATTR_TX_FIR_MAIN, attr_val));
                }
                /* Set port serdes post1 */
                else if (fvField(i) == "post1")
                {
                    getPortSerdesVal(fvValue(i), attr_val);
                    serdes_attr.insert(serdes_attr_pair(SAI_PORT_SERDES_ATTR_TX_FIR_POST1, attr_val));
                }
                /* Set port serdes post2 */
                else if (fvField(i) == "post2")
                {
                    getPortSerdesVal(fvValue(i), attr_val);
                    serdes_attr.insert(serdes_attr_pair(SAI_PORT_SERDES_ATTR_TX_FIR_POST2, attr_val));
                }
                /* Set port serdes post3 */
                else if (fvField(i) == "post3")
                {
                    getPortSerdesVal(fvValue(i), attr_val);
                    serdes_attr.insert(serdes_attr_pair(SAI_PORT_SERDES_ATTR_TX_FIR_POST3, attr_val));
                }
                /* Set port serdes attn */
                else if (fvField(i) == "attn")
                {
                    getPortSerdesVal(fvValue(i), attr_val);
                    serdes_attr.insert(serdes_attr_pair(SAI_PORT_SERDES_ATTR_TX_FIR_ATTN, attr_val));
                }

                /* Get port role */
                if (fvField(i) == "role")
                {
                    role = fvValue(i);
                }
            }

            /* Collect information about all received ports */
            if (lane_set.size())
            {
                m_lanesAliasSpeedMap[lane_set] = make_tuple(alias, speed, an, fec_mode, index, role);
            }

            // TODO:
            // Fix the issue below
            // After PortConfigDone, while waiting for "PortInitDone" and the first gBufferOrch->isPortReady(alias),
            // the complete m_lanesAliasSpeedMap may be populated again, so initPort() will be called more than once
            // for the same port.

            /* Once all ports received, go through the each port and perform appropriate actions:
             * 1. Remove ports which don't exist anymore
             * 2. Create new ports
             * 3. Initialize all ports
             */
            if (m_portConfigState == PORT_CONFIG_RECEIVED || m_portConfigState == PORT_CONFIG_DONE)
            {
                for (auto it = m_portListLaneMap.begin(); it != m_portListLaneMap.end();)
                {
                    if (m_lanesAliasSpeedMap.find(it->first) == m_lanesAliasSpeedMap.end())
                    {
                        if (SAI_STATUS_SUCCESS != removePort(it->second))
                        {
                            throw runtime_error("PortsOrch initialization failure.");
                        }
                        it = m_portListLaneMap.erase(it);
                    }
                    else
                    {
                        it++;
                    }
                }

                for (auto it = m_lanesAliasSpeedMap.begin(); it != m_lanesAliasSpeedMap.end();)
                {
                    if (m_portListLaneMap.find(it->first) == m_portListLaneMap.end())
                    {
                        if (!addPort(it->first, get<1>(it->second), get<2>(it->second), get<3>(it->second)))
                        {
                            throw runtime_error("PortsOrch initialization failure.");
                        }
                    }

                    if (!initPort(get<0>(it->second), get<5>(it->second), get<4>(it->second), it->first))
                    {
                        // Failure has been recorded in initPort
                        it++;
                        continue;
                    }

                    initPortSupportedSpeeds(get<0>(it->second), m_portListLaneMap[it->first]);
                    it++;
                }

                m_portConfigState = PORT_CONFIG_DONE;
            }

            if (m_portConfigState != PORT_CONFIG_DONE)
            {
                // Not yet receive PortConfigDone. Save it for future retry
                it++;
                continue;
            }

            if (alias == "PortConfigDone")
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (!gBufferOrch->isPortReady(alias))
            {
                // buffer configuration hasn't been applied yet. save it for future retry
                m_pendingPortSet.emplace(alias);
                it++;
                continue;
            }
            else
            {
                m_pendingPortSet.erase(alias);
            }

            Port p;
            if (!getPort(alias, p))
            {
                SWSS_LOG_ERROR("Failed to get port id by alias:%s", alias.c_str());
            }
            else
            {
                if (!an_str.empty())
                {
                    if (autoneg_mode_map.find(an_str) == autoneg_mode_map.end())
                    {
                        SWSS_LOG_ERROR("Failed to parse autoneg value: %s", an_str.c_str());
                        // Invalid auto negotiation mode configured, don't retry
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }

                    an = autoneg_mode_map[an_str];
                    if (an != p.m_autoneg)
                    {
                        if (p.m_admin_state_up)
                        {
                            /* Bring port down before applying speed */
                            if (!setPortAdminStatus(p, false))
                            {
                                SWSS_LOG_ERROR("Failed to set port %s admin status DOWN to set port autoneg mode", alias.c_str());
                                it++;
                                continue;
                            }

                            p.m_admin_state_up = false;
                            m_portList[alias] = p;
                        }

                        auto status = setPortAutoNeg(p.m_port_id, an);
                        if (status != task_success)
                        {
                            SWSS_LOG_ERROR("Failed to set port %s AN from %d to %d", alias.c_str(), p.m_autoneg, an);
                            if (status == task_need_retry)
                            {
                                it++;
                            }
                            else
                            {
                                it = consumer.m_toSync.erase(it);
                            }
                            continue;
                        }
                        SWSS_LOG_NOTICE("Set port %s AutoNeg from %d to %d", alias.c_str(), p.m_autoneg, an);
                        p.m_autoneg = an;
                        m_portList[alias] = p;
                    }
                }

                if (speed != 0)
                {
                    if (speed != p.m_speed)
                    {
                        if (!isSpeedSupported(alias, p.m_port_id, speed))
                        {
                            SWSS_LOG_ERROR("Unsupported port speed %u", speed);
                            // Speed not supported, dont retry
                            it = consumer.m_toSync.erase(it);
                            continue;
                        }

                        // for backward compatible, if p.m_autoneg != 1, toggle admin status
                        if (p.m_admin_state_up && p.m_autoneg != 1)
                        {
                            /* Bring port down before applying speed */
                            if (!setPortAdminStatus(p, false))
                            {
                                SWSS_LOG_ERROR("Failed to set port %s admin status DOWN to set speed", alias.c_str());
                                it++;
                                continue;
                            }

                            p.m_admin_state_up = false;
                            m_portList[alias] = p;
                        }

                        auto status = setPortSpeed(p, speed);
                        if (status != task_success)
                        {
                            SWSS_LOG_ERROR("Failed to set port %s speed from %u to %u", alias.c_str(), p.m_speed, speed);
                            if (status == task_need_retry)
                            {
                                it++;
                            }
                            else
                            {
                                it = consumer.m_toSync.erase(it);
                            }
                            continue;
                        }

                        SWSS_LOG_NOTICE("Set port %s speed from %u to %u", alias.c_str(), p.m_speed, speed);
                        p.m_speed = speed;
                        m_portList[alias] = p;
                    }
                    else
                    {
                        /* Always update Gearbox speed on Gearbox ports */
                        setGearboxPortsAttr(p, SAI_PORT_ATTR_SPEED, &speed);
                    }
                }

                if (!adv_speeds_str.empty())
                {
                    boost::to_lower(adv_speeds_str);
                    if (!getPortAdvSpeedsVal(adv_speeds_str, adv_speeds))
                    {
                        // Invalid advertised speeds configured, dont retry
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }

                    if (adv_speeds != p.m_adv_speeds)
                    {
                        if (p.m_admin_state_up && p.m_autoneg == 1)
                        {
                            /* Bring port down before applying speed */
                            if (!setPortAdminStatus(p, false))
                            {
                                SWSS_LOG_ERROR("Failed to set port %s admin status DOWN to set interface type", alias.c_str());
                                it++;
                                continue;
                            }

                            p.m_admin_state_up = false;
                            m_portList[alias] = p;
                        }

                        auto ori_adv_speeds = swss::join(',', p.m_adv_speeds.begin(), p.m_adv_speeds.end());
                        auto status = setPortAdvSpeeds(p.m_port_id, adv_speeds);
                        if (status != task_success)
                        {

                            SWSS_LOG_ERROR("Failed to set port %s advertised speed from %s to %s", alias.c_str(),
                                                                                                   ori_adv_speeds.c_str(),
                                                                                                   adv_speeds_str.c_str());
                            if (status == task_need_retry)
                            {
                                it++;
                            }
                            else
                            {
                                it = consumer.m_toSync.erase(it);
                            }
                            continue;
                        }
                        SWSS_LOG_NOTICE("Set port %s advertised speed from %s to %s", alias.c_str(),
                                                                                      ori_adv_speeds.c_str(),
                                                                                      adv_speeds_str.c_str());
                        p.m_adv_speeds.swap(adv_speeds);
                        m_portList[alias] = p;
                    }
                }

                if (!interface_type_str.empty())
                {
                    boost::to_lower(interface_type_str);
                    if (!getPortInterfaceTypeVal(interface_type_str, interface_type))
                    {
                        // Invalid interface type configured, dont retry
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }

                    if (interface_type != p.m_interface_type)
                    {
                        if (p.m_admin_state_up && p.m_autoneg == 0)
                        {
                            /* Bring port down before applying speed */
                            if (!setPortAdminStatus(p, false))
                            {
                                SWSS_LOG_ERROR("Failed to set port %s admin status DOWN to set interface type", alias.c_str());
                                it++;
                                continue;
                            }

                            p.m_admin_state_up = false;
                            m_portList[alias] = p;
                        }

                        auto status = setPortInterfaceType(p.m_port_id, interface_type);
                        if (status != task_success)
                        {
                            SWSS_LOG_ERROR("Failed to set port %s interface type to %s", alias.c_str(), interface_type_str.c_str());
                            if (status == task_need_retry)
                            {
                                it++;
                            }
                            else
                            {
                                it = consumer.m_toSync.erase(it);
                            }
                            continue;
                        }

                        SWSS_LOG_NOTICE("Set port %s interface type to %s", alias.c_str(), interface_type_str.c_str());
                        p.m_interface_type = interface_type;
                        m_portList[alias] = p;
                    }
                }

                if (!adv_interface_types_str.empty())
                {
                    boost::to_lower(adv_interface_types_str);
                    if (!getPortAdvInterfaceTypesVal(adv_interface_types_str, adv_interface_types))
                    {
                        // Invalid advertised interface types configured, dont retry
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }

                    if (adv_interface_types != p.m_adv_interface_types && p.m_autoneg == 1)
                    {
                        if (p.m_admin_state_up)
                        {
                            /* Bring port down before applying speed */
                            if (!setPortAdminStatus(p, false))
                            {
                                SWSS_LOG_ERROR("Failed to set port %s admin status DOWN to set interface type", alias.c_str());
                                it++;
                                continue;
                            }

                            p.m_admin_state_up = false;
                            m_portList[alias] = p;
                        }

                        auto status = setPortAdvInterfaceTypes(p.m_port_id, adv_interface_types);
                        if (status != task_success)
                        {
                            SWSS_LOG_ERROR("Failed to set port %s advertised interface type to %s", alias.c_str(), adv_interface_types_str.c_str());
                            if (status == task_need_retry)
                            {
                                it++;
                            }
                            else
                            {
                                it = consumer.m_toSync.erase(it);
                            }
                            continue;
                        }

                        SWSS_LOG_NOTICE("Set port %s advertised interface type to %s", alias.c_str(), adv_interface_types_str.c_str());
                        p.m_adv_interface_types.swap(adv_interface_types);
                        m_portList[alias] = p;
                    }
                }

                if (mtu != 0 && mtu != p.m_mtu)
                {
                    if (setPortMtu(p.m_port_id, mtu))
                    {
                        p.m_mtu = mtu;
                        m_portList[alias] = p;
                        SWSS_LOG_NOTICE("Set port %s MTU to %u", alias.c_str(), mtu);
                        if (p.m_rif_id)
                        {
                            gIntfsOrch->setRouterIntfsMtu(p);
                        }
                        // Sub interfaces inherit parent physical port mtu
                        updateChildPortsMtu(p, mtu);
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to set port %s MTU to %u", alias.c_str(), mtu);
                        it++;
                        continue;
                    }
                }

                if (tpid != 0 && tpid != p.m_tpid)
                {
                    SWSS_LOG_DEBUG("Set port %s TPID to 0x%x", alias.c_str(), tpid);
                    if (setPortTpid(p.m_port_id, tpid))
                    {
                        p.m_tpid = tpid;
                        m_portList[alias] = p;
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to set port %s TPID to 0x%x", alias.c_str(), tpid);
                        it++;
                        continue;
                    }
                }

                if (!fec_mode.empty())
                {
                    if (fec_mode_map.find(fec_mode) != fec_mode_map.end())
                    {
                        /* reset fec mode upon mode change */
                        if (!p.m_fec_cfg || p.m_fec_mode != fec_mode_map[fec_mode])
                        {
                            if (p.m_admin_state_up)
                            {
                                /* Bring port down before applying fec mode*/
                                if (!setPortAdminStatus(p, false))
                                {
                                    SWSS_LOG_ERROR("Failed to set port %s admin status DOWN to set fec mode", alias.c_str());
                                    it++;
                                    continue;
                                }

                                p.m_admin_state_up = false;
                                p.m_fec_mode = fec_mode_map[fec_mode];
                                p.m_fec_cfg = true;

                                if (setPortFec(p, p.m_fec_mode))
                                {
                                    m_portList[alias] = p;
                                    SWSS_LOG_NOTICE("Set port %s fec to %s", alias.c_str(), fec_mode.c_str());
                                }
                                else
                                {
                                    SWSS_LOG_ERROR("Failed to set port %s fec to %s", alias.c_str(), fec_mode.c_str());
                                    it++;
                                    continue;
                                }
                            }
                            else
                            {
                                /* Port is already down, setting fec mode*/
                                p.m_fec_mode = fec_mode_map[fec_mode];
                                p.m_fec_cfg = true;
                                if (setPortFec(p, p.m_fec_mode))
                                {
                                    m_portList[alias] = p;
                                    SWSS_LOG_NOTICE("Set port %s fec to %s", alias.c_str(), fec_mode.c_str());
                                }
                                else
                                {
                                    SWSS_LOG_ERROR("Failed to set port %s fec to %s", alias.c_str(), fec_mode.c_str());
                                    it++;
                                    continue;
                                }
                            }
                        }
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Unknown fec mode %s", fec_mode.c_str());
                    }
                }

                if (!learn_mode.empty() && (p.m_learn_mode != learn_mode))
                {
                    if (p.m_bridge_port_id != SAI_NULL_OBJECT_ID)
                    {
                        if(setBridgePortLearnMode(p, learn_mode))
                        {
                            p.m_learn_mode = learn_mode;
                            m_portList[alias] = p;
                            SWSS_LOG_NOTICE("Set port %s learn mode to %s", alias.c_str(), learn_mode.c_str());
                        }
                        else
                        {
                            SWSS_LOG_ERROR("Failed to set port %s learn mode to %s", alias.c_str(), learn_mode.c_str());
                            it++;
                            continue;
                        }
                    }
                    else
                    {
                        p.m_learn_mode = learn_mode;
                        m_portList[alias] = p;

                        SWSS_LOG_NOTICE("Saved to set port %s learn mode %s", alias.c_str(), learn_mode.c_str());
                    }
                }

                if (pfc_asym != "")
                {
                    if (setPortPfcAsym(p, pfc_asym))
                    {
                        SWSS_LOG_NOTICE("Set port %s asymmetric PFC to %s", alias.c_str(), pfc_asym.c_str());
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to set port %s asymmetric PFC to %s", alias.c_str(), pfc_asym.c_str());
                        it++;
                        continue;
                    }
                }

                if (serdes_attr.size() != 0)
                {
                    if (setPortSerdesAttribute(p.m_port_id, serdes_attr))
                    {
                        SWSS_LOG_NOTICE("Set port %s  preemphasis is success", alias.c_str());
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to set port %s pre-emphasis", alias.c_str());
                        it++;
                        continue;
                    }

                }

                /* Last step set port admin status */
                if (!admin_status.empty() && (p.m_admin_state_up != (admin_status == "up")))
                {
                    if (setPortAdminStatus(p, admin_status == "up"))
                    {
                        p.m_admin_state_up = (admin_status == "up");
                        m_portList[alias] = p;
                        SWSS_LOG_NOTICE("Set port %s admin status to %s", alias.c_str(), admin_status.c_str());
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to set port %s admin status to %s", alias.c_str(), admin_status.c_str());
                        it++;
                        continue;
                    }
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_port_ref_count[alias] > 0)
            {
                SWSS_LOG_WARN("Unable to remove port %s: ref count %u", alias.c_str(), m_port_ref_count[alias]);
                it++;
                continue;
            }

            SWSS_LOG_NOTICE("Deleting Port %s", alias.c_str());
            auto port_id = m_portList[alias].m_port_id;
            auto hif_id = m_portList[alias].m_hif_id;
            auto bridge_port_oid = m_portList[alias].m_bridge_port_id;

            if (bridge_port_oid != SAI_NULL_OBJECT_ID)
            {
                // Bridge port OID is set on a port as long as
                // port is part of at-least one VLAN.
                // Ideally this should be tracked by SAI redis.
                // Until then, let this snippet be here.
                SWSS_LOG_WARN("Cannot remove port as bridge port OID is present %" PRIx64 , bridge_port_oid);
                it++;
                continue;
            }

            if (m_portList[alias].m_init)
            {
                deInitPort(alias, port_id);
                SWSS_LOG_NOTICE("Removing hostif %" PRIx64 " for Port %s", hif_id, alias.c_str());
                sai_status_t status = sai_hostif_api->remove_hostif(hif_id);
                if (status != SAI_STATUS_SUCCESS)
                {
                    throw runtime_error("Remove hostif for the port failed");
                }

                Port p;
                if (getPort(port_id, p))
                {
                    PortUpdate update = {p, false};
                    notify(SUBJECT_TYPE_PORT_CHANGE, static_cast<void *>(&update));
                }
            }

            sai_status_t status = removePort(port_id);
            if (SAI_STATUS_SUCCESS != status)
            {
                if (SAI_STATUS_OBJECT_IN_USE != status)
                {
                    throw runtime_error("Delete port failed");
                }
                SWSS_LOG_WARN("Failed to remove port %" PRIx64 ", as the object is in use", port_id);
                it++;
                continue;
            }
            removePortFromLanesMap(alias);
            removePortFromPortListMap(port_id);

            /* Delete port from port list */
            m_portList.erase(alias);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

void PortsOrch::doVlanTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;

        string key = kfvKey(t);

        /* Ensure the key starts with "Vlan" otherwise ignore */
        if (strncmp(key.c_str(), VLAN_PREFIX, 4))
        {
            SWSS_LOG_ERROR("Invalid key format. No 'Vlan' prefix: %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        int vlan_id;
        vlan_id = stoi(key.substr(4)); // FIXME: might raise exception

        string vlan_alias;
        vlan_alias = VLAN_PREFIX + to_string(vlan_id);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            // Retrieve attributes
            uint32_t mtu = 0;
            MacAddress mac;
            string hostif_name = "";
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "mtu")
                {
                    mtu = (uint32_t)stoul(fvValue(i));
                }
                if (fvField(i) == "mac")
                {
                    mac = MacAddress(fvValue(i));
                }
                if (fvField(i) == "host_ifname")
                {
                    hostif_name = fvValue(i);
                }
            }

            /*
             * Only creation is supported for now.
             * We may add support for VLAN mac learning enable/disable,
             * VLAN flooding control setting and etc. in the future.
             */
            if (m_portList.find(vlan_alias) == m_portList.end())
            {
                if (!addVlan(vlan_alias))
                {
                    it++;
                    continue;
                }
            }

            // Process attributes
            Port vl;
            if (!getPort(vlan_alias, vl))
            {
                SWSS_LOG_ERROR("Failed to get VLAN %s", vlan_alias.c_str());
            }
            else
            {
                if (mtu != 0)
                {
                    vl.m_mtu = mtu;
                    m_portList[vlan_alias] = vl;
                    if (vl.m_rif_id)
                    {
                        gIntfsOrch->setRouterIntfsMtu(vl);
                    }
                }
                if (mac)
                {
                    vl.m_mac = mac;
                    m_portList[vlan_alias] = vl;
                    if (vl.m_rif_id)
                    {
                        gIntfsOrch->setRouterIntfsMac(vl);
                    }
                }
                if (!hostif_name.empty())
                {
                    if (!createVlanHostIntf(vl, hostif_name))
                    {
                        // No need to fail in case of error as this is for monitoring VLAN.
                        // Error message is printed by "createVlanHostIntf" so just handle failure gracefully.
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }
                }
            }

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            Port vlan;
            getPort(vlan_alias, vlan);

            if (removeVlan(vlan))
                it = consumer.m_toSync.erase(it);
            else
                it++;
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void PortsOrch::doVlanMemberTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;

        string key = kfvKey(t);

        /* Ensure the key starts with "Vlan" otherwise ignore */
        if (strncmp(key.c_str(), VLAN_PREFIX, 4))
        {
            SWSS_LOG_ERROR("Invalid key format. No 'Vlan' prefix: %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        key = key.substr(4);
        size_t found = key.find(':');
        int vlan_id;
        string vlan_alias, port_alias;
        if (found != string::npos)
        {
            vlan_id = stoi(key.substr(0, found)); // FIXME: might raise exception
            port_alias = key.substr(found+1);
        }
        else
        {
            SWSS_LOG_ERROR("Invalid key format. No member port is presented: %s",
                           kfvKey(t).c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        vlan_alias = VLAN_PREFIX + to_string(vlan_id);
        string op = kfvOp(t);

        assert(m_portList.find(vlan_alias) != m_portList.end());
        Port vlan, port;

        /* When VLAN member is to be created before VLAN is created */
        if (!getPort(vlan_alias, vlan))
        {
            SWSS_LOG_INFO("Failed to locate VLAN %s", vlan_alias.c_str());
            it++;
            continue;
        }

        if (!getPort(port_alias, port))
        {
            SWSS_LOG_DEBUG("%s is not not yet created, delaying", port_alias.c_str());
            it++;
            continue;
        }

        if (op == SET_COMMAND)
        {
            string tagging_mode = "untagged";

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "tagging_mode")
                    tagging_mode = fvValue(i);
            }

            if (tagging_mode != "untagged" &&
                tagging_mode != "tagged"   &&
                tagging_mode != "priority_tagged")
            {
                SWSS_LOG_ERROR("Wrong tagging_mode '%s' for key: %s", tagging_mode.c_str(), kfvKey(t).c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* Duplicate entry */
            if (vlan.m_members.find(port_alias) != vlan.m_members.end())
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (addBridgePort(port) && addVlanMember(vlan, port, tagging_mode))
                it = consumer.m_toSync.erase(it);
            else
                it++;
        }
        else if (op == DEL_COMMAND)
        {
            if (vlan.m_members.find(port_alias) != vlan.m_members.end())
            {
                if (removeVlanMember(vlan, port))
                {
                    if (m_portVlanMember[port.m_alias].empty())
                    {
                        removeBridgePort(port);
                    }
                    it = consumer.m_toSync.erase(it);
                }
                else
                {
                    it++;
                }
            }
            else
                /* Cannot locate the VLAN */
                it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void PortsOrch::doLagTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;

        string alias = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            // Retrieve attributes
            uint32_t mtu = 0;
            string learn_mode;
            string operation_status;
            uint32_t lag_id = 0;
            int32_t switch_id = -1;
            string tpid_string;
            uint16_t tpid = 0;

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "mtu")
                {
                    mtu = (uint32_t)stoul(fvValue(i));
                }
                else if (fvField(i) == "learn_mode")
                {
                    learn_mode = fvValue(i);
                }
                else if (fvField(i) == "oper_status")
                {
                    operation_status = fvValue(i);
                    if (!string_oper_status.count(operation_status))
                    {
                        SWSS_LOG_ERROR("Invalid operation status value:%s", operation_status.c_str());
                        it++;
                        continue;
                    }
                }
                else if (fvField(i) == "lag_id")
                {
                    lag_id = (uint32_t)stoul(fvValue(i));
                }
                else if (fvField(i) == "switch_id")
                {
                    switch_id = stoi(fvValue(i));
                }
                else if (fvField(i) == "tpid")
                {
                    tpid_string = fvValue(i);
                    // Need to get rid of the leading 0x
                    tpid_string.erase(0,2);
                    tpid = (uint16_t)stoi(tpid_string, 0, 16);
                    SWSS_LOG_DEBUG("reading TPID string:%s to uint16: 0x%x", tpid_string.c_str(), tpid);
                 }
            }

            if (table_name == CHASSIS_APP_LAG_TABLE_NAME)
            {
                if (switch_id == gVoqMySwitchId)
                {
                    //Already created, syncd local lag from CHASSIS_APP_DB. Skip
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
            }
            else
            {
                // For local portchannel

                lag_id = 0;
                switch_id = -1;
            }

            if (m_portList.find(alias) == m_portList.end())
            {
                if (!addLag(alias, lag_id, switch_id))
                {
                    it++;
                    continue;
                }
            }

            // Process attributes
            Port l;
            if (!getPort(alias, l))
            {
                SWSS_LOG_ERROR("Failed to get LAG %s", alias.c_str());
            }
            else
            {
                if (!operation_status.empty())
                {
                    updatePortOperStatus(l, string_oper_status.at(operation_status));

                    m_portList[alias] = l;
                }

                if (mtu != 0)
                {
                    l.m_mtu = mtu;
                    m_portList[alias] = l;
                    if (l.m_rif_id)
                    {
                        gIntfsOrch->setRouterIntfsMtu(l);
                    }
                    // Sub interfaces inherit parent LAG mtu
                    updateChildPortsMtu(l, mtu);
                }

                if (tpid != 0)
                {
                    if (tpid != l.m_tpid)
                    {
                        if(!setLagTpid(l.m_lag_id, tpid))
                        {
                            SWSS_LOG_ERROR("Failed to set LAG %s TPID 0x%x", alias.c_str(), tpid);
                        }
                        else
                        {
                            SWSS_LOG_DEBUG("Set LAG %s TPID to 0x%x", alias.c_str(), tpid);
                            l.m_tpid = tpid;
                            m_portList[alias] = l;
                        }
                    }
                }

                if (!learn_mode.empty() && (l.m_learn_mode != learn_mode))
                {
                    if (l.m_bridge_port_id != SAI_NULL_OBJECT_ID)
                    {
                        if(setBridgePortLearnMode(l, learn_mode))
                        {
                            l.m_learn_mode = learn_mode;
                            m_portList[alias] = l;
                            SWSS_LOG_NOTICE("Set port %s learn mode to %s", alias.c_str(), learn_mode.c_str());
                        }
                        else
                        {
                            SWSS_LOG_ERROR("Failed to set port %s learn mode to %s", alias.c_str(), learn_mode.c_str());
                            it++;
                            continue;
                        }
                    }
                    else
                    {
                        l.m_learn_mode = learn_mode;
                        m_portList[alias] = l;

                        SWSS_LOG_NOTICE("Saved to set port %s learn mode %s", alias.c_str(), learn_mode.c_str());
                    }
                }
            }

            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            Port lag;
            /* Cannot locate LAG */
            if (!getPort(alias, lag))
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (removeLag(lag))
                it = consumer.m_toSync.erase(it);
            else
                it++;
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void PortsOrch::doLagMemberTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;

        /* Retrieve LAG alias and LAG member alias from key */
        string key = kfvKey(t);
        size_t found = key.find(':');
        /* Return if the format of key is wrong */
        if (found == string::npos)
        {
            SWSS_LOG_ERROR("Failed to parse %s", key.c_str());
            return;
        }
        string lag_alias = key.substr(0, found);
        string port_alias = key.substr(found+1);

        string op = kfvOp(t);

        Port lag, port;
        if (!getPort(lag_alias, lag))
        {
            SWSS_LOG_INFO("Failed to locate LAG %s", lag_alias.c_str());
            it++;
            continue;
        }

        if (!getPort(port_alias, port))
        {
            SWSS_LOG_ERROR("Failed to locate port %s", port_alias.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        /* Fail if a port type is not a valid type for being a LAG member port.
         * Erase invalid entry, no need to retry in this case. */
        if (!isValidPortTypeForLagMember(port))
        {
            SWSS_LOG_ERROR("LAG member port has to be of type PHY or SYSTEM");
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (table_name == CHASSIS_APP_LAG_MEMBER_TABLE_NAME)
        {
            int32_t lag_switch_id = lag.m_system_lag_info.switch_id;
            if (lag_switch_id == gVoqMySwitchId)
            {
                //Synced local member addition to local lag. Skip
                it = consumer.m_toSync.erase(it);
                continue;
            }

            //Sanity check: The switch id-s of lag and member must match
            int32_t port_switch_id = port.m_system_port_info.switch_id;
            if (port_switch_id != lag_switch_id)
            {
                SWSS_LOG_ERROR("System lag switch id mismatch. Lag %s switch id: %d, Member %s switch id: %d",
                        lag_alias.c_str(), lag_switch_id, port_alias.c_str(), port_switch_id);
                it = consumer.m_toSync.erase(it);
                continue;
            }
        }

        /* Update a LAG member */
        if (op == SET_COMMAND)
        {
            string status;
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "status")
                    status = fvValue(i);
            }

            if (lag.m_members.find(port_alias) == lag.m_members.end())
            {
                if (port.m_lag_member_id != SAI_NULL_OBJECT_ID)
                {
                    SWSS_LOG_INFO("Port %s is already a LAG member", port.m_alias.c_str());
                    it++;
                    continue;
                }

                if (!port.m_ingress_acl_tables_uset.empty() || !port.m_egress_acl_tables_uset.empty())
                {
                    SWSS_LOG_ERROR(
                        "Failed to add member %s to LAG %s: ingress/egress ACL configuration is present",
                        port.m_alias.c_str(),
                        lag.m_alias.c_str()
                    );
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                if (!addLagMember(lag, port, (status == "enabled")))
                {
                    it++;
                    continue;
                }
            }

            /* Sync an enabled member */
            if (status == "enabled")
            {
                /* enable collection first, distribution-only mode
                 * is not supported on Mellanox platform
                 */
                if (setCollectionOnLagMember(port, true) &&
                    setDistributionOnLagMember(port, true))
                {
                    it = consumer.m_toSync.erase(it);
                }
                else
                {
                    it++;
                    continue;
                }
            }
            /* Sync an disabled member */
            else /* status == "disabled" */
            {
                /* disable distribution first, distribution-only mode
                 * is not supported on Mellanox platform
                 */
                if (setDistributionOnLagMember(port, false) &&
                    setCollectionOnLagMember(port, false))
                {
                    it = consumer.m_toSync.erase(it);
                }
                else
                {
                    it++;
                    continue;
                }
            }
        }
        /* Remove a LAG member */
        else if (op == DEL_COMMAND)
        {
            /* Assert the LAG member exists */
            assert(lag.m_members.find(port_alias) != lag.m_members.end());

            if (!port.m_lag_id || !port.m_lag_member_id)
            {
                SWSS_LOG_WARN("Member %s not found in LAG %s lid:%" PRIx64 " lmid:%" PRIx64 ",",
                        port.m_alias.c_str(), lag.m_alias.c_str(), lag.m_lag_id, port.m_lag_member_id);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (removeLagMember(lag, port))
            {
                it = consumer.m_toSync.erase(it);
            }
            else
            {
                it++;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void PortsOrch::doTask()
{
    auto tableOrder = {
        APP_PORT_TABLE_NAME,
        APP_LAG_TABLE_NAME,
        APP_LAG_MEMBER_TABLE_NAME,
        APP_VLAN_TABLE_NAME,
        APP_VLAN_MEMBER_TABLE_NAME,
    };

    for (auto tableName: tableOrder)
    {
        auto consumer = getExecutor(tableName);
        consumer->drain();
    }

    // drain remaining tables
    for (auto& it: m_consumerMap)
    {
        auto tableName = it.first;
        auto consumer = it.second.get();
        if (find(tableOrder.begin(), tableOrder.end(), tableName) == tableOrder.end())
        {
            consumer->drain();
        }
    }
}

void PortsOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    if (table_name == APP_PORT_TABLE_NAME)
    {
        doPortTask(consumer);
    }
    else
    {
        /* Wait for all ports to be initialized */
        if (!allPortsReady())
        {
            return;
        }

        if (table_name == APP_VLAN_TABLE_NAME)
        {
            doVlanTask(consumer);
        }
        else if (table_name == APP_VLAN_MEMBER_TABLE_NAME)
        {
            doVlanMemberTask(consumer);
        }
        else if (table_name == APP_LAG_TABLE_NAME || table_name == CHASSIS_APP_LAG_TABLE_NAME)
        {
            doLagTask(consumer);
        }
        else if (table_name == APP_LAG_MEMBER_TABLE_NAME || table_name == CHASSIS_APP_LAG_MEMBER_TABLE_NAME)
        {
            doLagMemberTask(consumer);
        }
    }
}

void PortsOrch::initializeQueues(Port &port)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_QOS_NUMBER_OF_QUEUES;
    sai_status_t status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get number of queues for port %s rv:%d", port.m_alias.c_str(), status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
        if (handle_status != task_process_status::task_success)
        {
            throw runtime_error("PortsOrch initialization failure.");
        }
    }
    SWSS_LOG_INFO("Get %d queues for port %s", attr.value.u32, port.m_alias.c_str());

    port.m_queue_ids.resize(attr.value.u32);
    port.m_queue_lock.resize(attr.value.u32);

    if (attr.value.u32 == 0)
    {
        return;
    }

    attr.id = SAI_PORT_ATTR_QOS_QUEUE_LIST;
    attr.value.objlist.count = (uint32_t)port.m_queue_ids.size();
    attr.value.objlist.list = port.m_queue_ids.data();

    status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get queue list for port %s rv:%d", port.m_alias.c_str(), status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
        if (handle_status != task_process_status::task_success)
        {
            throw runtime_error("PortsOrch initialization failure.");
        }
    }

    SWSS_LOG_INFO("Get queues for port %s", port.m_alias.c_str());
}

void PortsOrch::initializePriorityGroups(Port &port)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_NUMBER_OF_INGRESS_PRIORITY_GROUPS;
    sai_status_t status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get number of priority groups for port %s rv:%d", port.m_alias.c_str(), status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
        if (handle_status != task_process_status::task_success)
        {
            throw runtime_error("PortsOrch initialization failure.");
        }
    }
    SWSS_LOG_INFO("Get %d priority groups for port %s", attr.value.u32, port.m_alias.c_str());

    port.m_priority_group_ids.resize(attr.value.u32);
    port.m_priority_group_lock.resize(attr.value.u32);
    port.m_priority_group_pending_profile.resize(attr.value.u32);

    if (attr.value.u32 == 0)
    {
        return;
    }

    attr.id = SAI_PORT_ATTR_INGRESS_PRIORITY_GROUP_LIST;
    attr.value.objlist.count = (uint32_t)port.m_priority_group_ids.size();
    attr.value.objlist.list = port.m_priority_group_ids.data();

    status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Fail to get priority group list for port %s rv:%d", port.m_alias.c_str(), status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
        if (handle_status != task_process_status::task_success)
        {
            throw runtime_error("PortsOrch initialization failure.");
        }
    }
    SWSS_LOG_INFO("Get priority groups for port %s", port.m_alias.c_str());
}

void PortsOrch::initializePortBufferMaximumParameters(Port &port)
{
    sai_attribute_t attr;
    vector<FieldValueTuple> fvVector;

    attr.id = SAI_PORT_ATTR_QOS_MAXIMUM_HEADROOM_SIZE;

    sai_status_t status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_NOTICE("Unable to get the maximum headroom for port %s rv:%d, ignored", port.m_alias.c_str(), status);
    }
    else
    {
        port.m_maximum_headroom = attr.value.u32;
        fvVector.emplace_back("max_headroom_size", to_string(port.m_maximum_headroom));
    }

    fvVector.emplace_back("max_priority_groups", to_string(port.m_priority_group_ids.size()));
    fvVector.emplace_back("max_queues", to_string(port.m_queue_ids.size()));

    m_stateBufferMaximumValueTable->set(port.m_alias, fvVector);
}

bool PortsOrch::initializePort(Port &port)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("Initializing port alias:%s pid:%" PRIx64, port.m_alias.c_str(), port.m_port_id);

    initializePriorityGroups(port);
    initializeQueues(port);
    initializePortBufferMaximumParameters(port);

    /* Create host interface */
    if (!addHostIntfs(port, port.m_alias, port.m_hif_id))
    {
        SWSS_LOG_ERROR("Failed to create host interface for port %s", port.m_alias.c_str());
        return false;
    }

    /* Check warm start states */
    vector<FieldValueTuple> tuples;
    bool exist = m_portTable->get(port.m_alias, tuples);
    string operStatus;
    if (exist)
    {
        for (auto i : tuples)
        {
            if (fvField(i) == "oper_status")
            {
                operStatus = fvValue(i);
            }
        }
    }
    SWSS_LOG_DEBUG("initializePort %s with oper %s", port.m_alias.c_str(), operStatus.c_str());

    /**
     * Create database port oper status as DOWN if attr missing
     * This status will be updated upon receiving port_oper_status_notification.
     */
    if (operStatus == "up")
    {
        port.m_oper_status = SAI_PORT_OPER_STATUS_UP;
    }
    else if (operStatus.empty())
    {
        port.m_oper_status = SAI_PORT_OPER_STATUS_DOWN;
        /* Fill oper_status in db with default value "down" */
        m_portTable->hset(port.m_alias, "oper_status", "down");
    }
    else
    {
        port.m_oper_status = SAI_PORT_OPER_STATUS_DOWN;
    }

    /* initialize port admin status */
    if (!getPortAdminStatus(port.m_port_id, port.m_admin_state_up))
    {
        SWSS_LOG_ERROR("Failed to get initial port admin status %s", port.m_alias.c_str());
        return false;
    }

    /* initialize port admin speed */
    if (!getPortSpeed(port.m_port_id, port.m_speed))
    {
        SWSS_LOG_ERROR("Failed to get initial port admin speed %d", port.m_speed);
        return false;
    }

    /*
     * always initialize Port SAI_HOSTIF_ATTR_OPER_STATUS based on oper_status value in appDB.
     */
    bool isUp = port.m_oper_status == SAI_PORT_OPER_STATUS_UP;
    if (!setHostIntfsOperStatus(port, isUp))
    {
        SWSS_LOG_WARN("Failed to set operation status %s to host interface %s",
                      operStatus.c_str(), port.m_alias.c_str());
        return false;
    }

    return true;
}

bool PortsOrch::addHostIntfs(Port &port, string alias, sai_object_id_t &host_intfs_id)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_HOSTIF_ATTR_TYPE;
    attr.value.s32 = SAI_HOSTIF_TYPE_NETDEV;
    attrs.push_back(attr);

    attr.id = SAI_HOSTIF_ATTR_OBJ_ID;
    attr.value.oid = port.m_port_id;
    attrs.push_back(attr);

    attr.id = SAI_HOSTIF_ATTR_NAME;
    strncpy((char *)&attr.value.chardata, alias.c_str(), SAI_HOSTIF_NAME_SIZE);
    if (alias.length() >= SAI_HOSTIF_NAME_SIZE)
    {
        SWSS_LOG_WARN("Host interface name %s is too long and will be truncated to %d bytes", alias.c_str(), SAI_HOSTIF_NAME_SIZE - 1);
    }
    attr.value.chardata[SAI_HOSTIF_NAME_SIZE - 1] = '\0';
    attrs.push_back(attr);

    sai_status_t status = sai_hostif_api->create_hostif(&host_intfs_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create host interface for port %s", alias.c_str());
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_HOSTIF, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("Create host interface for port %s", alias.c_str());

    return true;
}

bool PortsOrch::setBridgePortLearningFDB(Port &port, sai_bridge_port_fdb_learning_mode_t mode)
{
    // TODO: how to support 1D bridge?
    if (port.m_type != Port::PHY) return false;

    auto bridge_port_id = port.m_bridge_port_id;
    if (bridge_port_id == SAI_NULL_OBJECT_ID) return false;

    sai_attribute_t bport_attr;
    bport_attr.id = SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE;
    bport_attr.value.s32 = mode;
    auto status = sai_bridge_api->set_bridge_port_attribute(bridge_port_id, &bport_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set bridge port %" PRIx64 " learning_mode attribute: %d", bridge_port_id, status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_BRIDGE, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    SWSS_LOG_NOTICE("Disable FDB learning on bridge port %s(%" PRIx64 ")", port.m_alias.c_str(), bridge_port_id);
    return true;
}

bool PortsOrch::addBridgePort(Port &port)
{
    SWSS_LOG_ENTER();

    if (port.m_bridge_port_id != SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    if (port.m_type == Port::PHY)
    {
        attr.id = SAI_BRIDGE_PORT_ATTR_TYPE;
        attr.value.s32 = SAI_BRIDGE_PORT_TYPE_PORT;
        attrs.push_back(attr);

        attr.id = SAI_BRIDGE_PORT_ATTR_PORT_ID;
        attr.value.oid = port.m_port_id;
        attrs.push_back(attr);
    }
    else if  (port.m_type == Port::LAG)
    {
        attr.id = SAI_BRIDGE_PORT_ATTR_TYPE;
        attr.value.s32 = SAI_BRIDGE_PORT_TYPE_PORT;
        attrs.push_back(attr);

        attr.id = SAI_BRIDGE_PORT_ATTR_PORT_ID;
        attr.value.oid = port.m_lag_id;
        attrs.push_back(attr);
    }
    else if  (port.m_type == Port::TUNNEL)
    {
        attr.id = SAI_BRIDGE_PORT_ATTR_TYPE;
        attr.value.s32 = SAI_BRIDGE_PORT_TYPE_TUNNEL;
        attrs.push_back(attr);

        attr.id = SAI_BRIDGE_PORT_ATTR_TUNNEL_ID;
        attr.value.oid = port.m_tunnel_id;
        attrs.push_back(attr);

        attr.id = SAI_BRIDGE_PORT_ATTR_BRIDGE_ID;
        attr.value.oid = m_default1QBridge;
        attrs.push_back(attr);
    }
    else
    {
        SWSS_LOG_ERROR("Failed to add bridge port %s to default 1Q bridge, invalid port type %d",
            port.m_alias.c_str(), port.m_type);
        return false;
    }

    /* Create a bridge port with admin status set to UP */
    attr.id = SAI_BRIDGE_PORT_ATTR_ADMIN_STATE;
    attr.value.booldata = true;
    attrs.push_back(attr);

    /* And with hardware FDB learning mode set to HW (explicit default value) */
    attr.id = SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE;
    auto found = learn_mode_map.find(port.m_learn_mode);
    if (found == learn_mode_map.end())
    {
        attr.value.s32 = SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW;
    }
    else
    {
        attr.value.s32 = found->second;
    }
    attrs.push_back(attr);

    sai_status_t status = sai_bridge_api->create_bridge_port(&port.m_bridge_port_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to add bridge port %s to default 1Q bridge, rv:%d",
            port.m_alias.c_str(), status);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_BRIDGE, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    if (!setHostIntfsStripTag(port, SAI_HOSTIF_VLAN_TAG_KEEP))
    {
        SWSS_LOG_ERROR("Failed to set %s for hostif of port %s",
                hostif_vlan_tag[SAI_HOSTIF_VLAN_TAG_KEEP], port.m_alias.c_str());
        return false;
    }
    m_portList[port.m_alias] = port;
    saiOidToAlias[port.m_bridge_port_id] = port.m_alias;
    SWSS_LOG_NOTICE("Add bridge port %s to default 1Q bridge", port.m_alias.c_str());

    PortUpdate update = { port, true };
    notify(SUBJECT_TYPE_BRIDGE_PORT_CHANGE, static_cast<void *>(&update));

    return true;
}

bool PortsOrch::removeBridgePort(Port &port)
{
    SWSS_LOG_ENTER();

    if (port.m_bridge_port_id == SAI_NULL_OBJECT_ID)
    {
        return true;
    }
    /* Set bridge port admin status to DOWN */
    sai_attribute_t attr;
    attr.id = SAI_BRIDGE_PORT_ATTR_ADMIN_STATE;
    attr.value.booldata = false;

    sai_status_t status = sai_bridge_api->set_bridge_port_attribute(port.m_bridge_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set bridge port %s admin status to DOWN, rv:%d",
            port.m_alias.c_str(), status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_BRIDGE, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    if (!setHostIntfsStripTag(port, SAI_HOSTIF_VLAN_TAG_STRIP))
    {
        SWSS_LOG_ERROR("Failed to set %s for hostif of port %s",
                hostif_vlan_tag[SAI_HOSTIF_VLAN_TAG_STRIP], port.m_alias.c_str());
        return false;
    }

    //Flush the FDB entires corresponding to the port
    gFdbOrch->flushFDBEntries(port.m_bridge_port_id, SAI_NULL_OBJECT_ID);
    SWSS_LOG_INFO("Flush FDB entries for port %s", port.m_alias.c_str());

    /* Remove bridge port */
    status = sai_bridge_api->remove_bridge_port(port.m_bridge_port_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove bridge port %s from default 1Q bridge, rv:%d",
            port.m_alias.c_str(), status);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_BRIDGE, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    saiOidToAlias.erase(port.m_bridge_port_id);
    port.m_bridge_port_id = SAI_NULL_OBJECT_ID;

    /* Remove bridge port */
    PortUpdate update = { port, false };
    notify(SUBJECT_TYPE_BRIDGE_PORT_CHANGE, static_cast<void *>(&update));

    SWSS_LOG_NOTICE("Remove bridge port %s from default 1Q bridge", port.m_alias.c_str());

    m_portList[port.m_alias] = port;
    return true;
}

bool PortsOrch::setBridgePortLearnMode(Port &port, string learn_mode)
{
    SWSS_LOG_ENTER();

    if (port.m_bridge_port_id == SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    auto found = learn_mode_map.find(learn_mode);
    if (found == learn_mode_map.end())
    {
        SWSS_LOG_ERROR("Incorrect MAC learn mode: %s", learn_mode.c_str());
        return false;
    }

    /* Set bridge port learning mode */
    sai_attribute_t attr;
    attr.id = SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE;
    attr.value.s32 = found->second;

    sai_status_t status = sai_bridge_api->set_bridge_port_attribute(port.m_bridge_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set bridge port %s learning mode, rv:%d",
            port.m_alias.c_str(), status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_BRIDGE, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("Set bridge port %s learning mode %s", port.m_alias.c_str(), learn_mode.c_str());

    return true;
}

bool PortsOrch::addVlan(string vlan_alias)
{
    SWSS_LOG_ENTER();

    sai_object_id_t vlan_oid;

    sai_vlan_id_t vlan_id = (uint16_t)stoi(vlan_alias.substr(4));
    sai_attribute_t attr;
    attr.id = SAI_VLAN_ATTR_VLAN_ID;
    attr.value.u16 = vlan_id;

    sai_status_t status = sai_vlan_api->create_vlan(&vlan_oid, gSwitchId, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create VLAN %s vid:%hu", vlan_alias.c_str(), vlan_id);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_VLAN, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("Create an empty VLAN %s vid:%hu vlan_oid:%" PRIx64, vlan_alias.c_str(), vlan_id, vlan_oid);

    Port vlan(vlan_alias, Port::VLAN);
    vlan.m_vlan_info.vlan_oid = vlan_oid;
    vlan.m_vlan_info.vlan_id = vlan_id;
    vlan.m_vlan_info.uuc_flood_type = SAI_VLAN_FLOOD_CONTROL_TYPE_ALL;
    vlan.m_vlan_info.bc_flood_type = SAI_VLAN_FLOOD_CONTROL_TYPE_ALL;
    vlan.m_members = set<string>();
    m_portList[vlan_alias] = vlan;
    m_port_ref_count[vlan_alias] = 0;
    saiOidToAlias[vlan_oid] =  vlan_alias;

    return true;
}

bool PortsOrch::removeVlan(Port vlan)
{
    SWSS_LOG_ENTER();

    /* If there are still fdb entries associated with the VLAN,
       return false for retry */
    if (vlan.m_fdb_count > 0)
    {
        SWSS_LOG_NOTICE("VLAN %s still has %d FDB entries", vlan.m_alias.c_str(), vlan.m_fdb_count);
        return false;
    }

    if (m_port_ref_count[vlan.m_alias] > 0)
    {
        SWSS_LOG_ERROR("Failed to remove ref count %d VLAN %s",
                       m_port_ref_count[vlan.m_alias],
                       vlan.m_alias.c_str());
        return false;
    }

    /* Vlan removing is not allowed when the VLAN still has members */
    if (vlan.m_members.size() > 0)
    {
        SWSS_LOG_ERROR("Failed to remove non-empty VLAN %s", vlan.m_alias.c_str());
        return false;
    }

    // Fail VLAN removal if there is a vnid associated
    if (vlan.m_vnid != VNID_NONE)
    {
       SWSS_LOG_ERROR("VLAN-VNI mapping not yet removed. VLAN %s VNI %d",
                      vlan.m_alias.c_str(), vlan.m_vnid);
       return false;
    }


    if (vlan.m_vlan_info.host_intf_id && !removeVlanHostIntf(vlan))
    {
        SWSS_LOG_ERROR("Failed to remove VLAN %d host interface", vlan.m_vlan_info.vlan_id);
        return false;
    }

    sai_status_t status = sai_vlan_api->remove_vlan(vlan.m_vlan_info.vlan_oid);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove VLAN %s vid:%hu",
                vlan.m_alias.c_str(), vlan.m_vlan_info.vlan_id);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_VLAN, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    removeAclTableGroup(vlan);

    SWSS_LOG_NOTICE("Remove VLAN %s vid:%hu", vlan.m_alias.c_str(),
            vlan.m_vlan_info.vlan_id);

    saiOidToAlias.erase(vlan.m_vlan_info.vlan_oid);
    m_portList.erase(vlan.m_alias);
    m_port_ref_count.erase(vlan.m_alias);

    return true;
}

bool PortsOrch::getVlanByVlanId(sai_vlan_id_t vlan_id, Port &vlan)
{
    SWSS_LOG_ENTER();

    for (auto &it: m_portList)
    {
        if (it.second.m_type == Port::VLAN && it.second.m_vlan_info.vlan_id == vlan_id)
        {
            vlan = it.second;
            return true;
        }
    }

    return false;
}

bool PortsOrch::addVlanMember(Port &vlan, Port &port, string &tagging_mode, string end_point_ip)
{
    SWSS_LOG_ENTER();

    if (!end_point_ip.empty())
    {
        if ((uuc_sup_flood_control_type.find(SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED)
             == uuc_sup_flood_control_type.end()) ||
            (bc_sup_flood_control_type.find(SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED)
             == bc_sup_flood_control_type.end()))
        {
            SWSS_LOG_ERROR("Flood group with end point ip is not supported");
            return false;
        }
        return addVlanFloodGroups(vlan, port, end_point_ip);
    }

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_VLAN_MEMBER_ATTR_VLAN_ID;
    attr.value.oid = vlan.m_vlan_info.vlan_oid;
    attrs.push_back(attr);

    attr.id = SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID;
    attr.value.oid = port.m_bridge_port_id;
    attrs.push_back(attr);


    sai_vlan_tagging_mode_t sai_tagging_mode = SAI_VLAN_TAGGING_MODE_TAGGED;
    attr.id = SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE;
    if (tagging_mode == "untagged")
        sai_tagging_mode = SAI_VLAN_TAGGING_MODE_UNTAGGED;
    else if (tagging_mode == "tagged")
        sai_tagging_mode = SAI_VLAN_TAGGING_MODE_TAGGED;
    else if (tagging_mode == "priority_tagged")
        sai_tagging_mode = SAI_VLAN_TAGGING_MODE_PRIORITY_TAGGED;
    else assert(false);
    attr.value.s32 = sai_tagging_mode;
    attrs.push_back(attr);

    sai_object_id_t vlan_member_id;
    sai_status_t status = sai_vlan_api->create_vlan_member(&vlan_member_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to add member %s to VLAN %s vid:%hu pid:%" PRIx64,
                port.m_alias.c_str(), vlan.m_alias.c_str(), vlan.m_vlan_info.vlan_id, port.m_port_id);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_VLAN, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    SWSS_LOG_NOTICE("Add member %s to VLAN %s vid:%hu pid%" PRIx64,
            port.m_alias.c_str(), vlan.m_alias.c_str(), vlan.m_vlan_info.vlan_id, port.m_port_id);

    /* Use untagged VLAN as pvid of the member port */
    if (sai_tagging_mode == SAI_VLAN_TAGGING_MODE_UNTAGGED)
    {
        if(!setPortPvid(port, vlan.m_vlan_info.vlan_id))
        {
            return false;
        }
    }

    /* a physical port may join multiple vlans */
    VlanMemberEntry vme = {vlan_member_id, sai_tagging_mode};
    m_portVlanMember[port.m_alias][vlan.m_vlan_info.vlan_id] = vme;
    m_portList[port.m_alias] = port;
    vlan.m_members.insert(port.m_alias);
    m_portList[vlan.m_alias] = vlan;

    VlanMemberUpdate update = { vlan, port, true };
    notify(SUBJECT_TYPE_VLAN_MEMBER_CHANGE, static_cast<void *>(&update));

    return true;
}

bool PortsOrch::getPortVlanMembers(Port &port, vlan_members_t &vlan_members)
{
    vlan_members = m_portVlanMember[port.m_alias];
    return true;
}

bool PortsOrch::addVlanFloodGroups(Port &vlan, Port &port, string end_point_ip)
{
    SWSS_LOG_ENTER();

    sai_object_id_t l2mc_group_id = SAI_NULL_OBJECT_ID;
    sai_status_t    status;
    sai_attribute_t attr;

    if (vlan.m_vlan_info.uuc_flood_type != SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED)
    {
        attr.id = SAI_VLAN_ATTR_UNKNOWN_UNICAST_FLOOD_CONTROL_TYPE;
        attr.value.s32 = SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED;

        status = sai_vlan_api->set_vlan_attribute(vlan.m_vlan_info.vlan_oid, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set l2mc flood type combined "
                           " to vlan %hu for unknown unicast flooding", vlan.m_vlan_info.vlan_id);
            return false;
        }
        vlan.m_vlan_info.uuc_flood_type = SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED;
    }

    if (vlan.m_vlan_info.bc_flood_type != SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED)
    {
        attr.id = SAI_VLAN_ATTR_BROADCAST_FLOOD_CONTROL_TYPE;
        attr.value.s32 = SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED;

        status = sai_vlan_api->set_vlan_attribute(vlan.m_vlan_info.vlan_oid, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set l2mc flood type combined "
                           " to vlan %hu for broadcast flooding", vlan.m_vlan_info.vlan_id);
            return false;
        }
        vlan.m_vlan_info.bc_flood_type = SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED;
    }

    if (vlan.m_vlan_info.l2mc_group_id == SAI_NULL_OBJECT_ID)
    {
        status = sai_l2mc_group_api->create_l2mc_group(&l2mc_group_id, gSwitchId, 0, NULL);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create l2mc flood group");
            return false;
        }

        if (vlan.m_vlan_info.uuc_flood_type == SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED)
        {
            attr.id = SAI_VLAN_ATTR_UNKNOWN_UNICAST_FLOOD_GROUP;
            attr.value.oid = l2mc_group_id;

            status = sai_vlan_api->set_vlan_attribute(vlan.m_vlan_info.vlan_oid, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set l2mc group %" PRIx64
                               " to vlan %hu for unknown unicast flooding",
                               l2mc_group_id, vlan.m_vlan_info.vlan_id);
                return false;
            }
        }
        if (vlan.m_vlan_info.bc_flood_type == SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED)
        {
            attr.id = SAI_VLAN_ATTR_BROADCAST_FLOOD_GROUP;
            attr.value.oid = l2mc_group_id;

            status = sai_vlan_api->set_vlan_attribute(vlan.m_vlan_info.vlan_oid, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set l2mc group %" PRIx64
                               " to vlan %hu for broadcast flooding",
                               l2mc_group_id, vlan.m_vlan_info.vlan_id);
                return false;
            }
        }
        vlan.m_vlan_info.l2mc_group_id = l2mc_group_id;
        m_portList[vlan.m_alias] = vlan;
    }

    vector<sai_attribute_t> attrs;
    attr.id = SAI_L2MC_GROUP_MEMBER_ATTR_L2MC_GROUP_ID;
    attr.value.oid = vlan.m_vlan_info.l2mc_group_id;
    attrs.push_back(attr);

    attr.id = SAI_L2MC_GROUP_MEMBER_ATTR_L2MC_OUTPUT_ID;
    attr.value.oid = port.m_bridge_port_id;
    attrs.push_back(attr);

    attr.id = SAI_L2MC_GROUP_MEMBER_ATTR_L2MC_ENDPOINT_IP;
    IpAddress remote = IpAddress(end_point_ip);
    sai_ip_address_t ipaddr;
    if (remote.isV4())
    {
        ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        ipaddr.addr.ip4 = remote.getV4Addr();
    }
    else
    {
        ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
        memcpy(ipaddr.addr.ip6, remote.getV6Addr(), sizeof(ipaddr.addr.ip6));
    }
    attr.value.ipaddr = ipaddr;
    attrs.push_back(attr);

    sai_object_id_t l2mc_group_member = SAI_NULL_OBJECT_ID;
    status = sai_l2mc_group_api->create_l2mc_group_member(&l2mc_group_member, gSwitchId,
                                                          static_cast<uint32_t>(attrs.size()),
                                                          attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create l2mc group member for adding tunnel %s to vlan %hu",
                       end_point_ip.c_str(), vlan.m_vlan_info.vlan_id);
        return false;
    }
    vlan.m_vlan_info.l2mc_members[end_point_ip] = l2mc_group_member;
    m_portList[vlan.m_alias] = vlan;
    increaseBridgePortRefCount(port);
    return true;
}


bool PortsOrch::removeVlanEndPointIp(Port &vlan, Port &port, string end_point_ip)
{
    SWSS_LOG_ENTER();

    sai_status_t status;

    if(vlan.m_vlan_info.l2mc_members.find(end_point_ip) == vlan.m_vlan_info.l2mc_members.end())
    {
        SWSS_LOG_NOTICE("End point ip %s is not part of vlan %hu",
                        end_point_ip.c_str(), vlan.m_vlan_info.vlan_id);
        return true;
    }

    status = sai_l2mc_group_api->remove_l2mc_group_member(vlan.m_vlan_info.l2mc_members[end_point_ip]);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove end point ip %s from vlan %hu",
                       end_point_ip.c_str(), vlan.m_vlan_info.vlan_id);
        return false;
    }
    decreaseBridgePortRefCount(port);
    vlan.m_vlan_info.l2mc_members.erase(end_point_ip);
    sai_object_id_t l2mc_group_id = SAI_NULL_OBJECT_ID;
    sai_attribute_t attr;

    if (vlan.m_vlan_info.l2mc_members.empty())
    {
        if (vlan.m_vlan_info.uuc_flood_type == SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED)
        {
            attr.id = SAI_VLAN_ATTR_UNKNOWN_UNICAST_FLOOD_GROUP;
            attr.value.oid = SAI_NULL_OBJECT_ID;

            status = sai_vlan_api->set_vlan_attribute(vlan.m_vlan_info.vlan_oid, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set null l2mc group "
                               " to vlan %hu for unknown unicast flooding",
                               vlan.m_vlan_info.vlan_id);
                return false;
            }
            attr.id = SAI_VLAN_ATTR_UNKNOWN_UNICAST_FLOOD_CONTROL_TYPE;
            attr.value.s32 = SAI_VLAN_FLOOD_CONTROL_TYPE_ALL;
            status = sai_vlan_api->set_vlan_attribute(vlan.m_vlan_info.vlan_oid, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set flood control type all"
                               " to vlan %hu for unknown unicast flooding",
                               vlan.m_vlan_info.vlan_id);
                return false;
            }
            vlan.m_vlan_info.uuc_flood_type = SAI_VLAN_FLOOD_CONTROL_TYPE_ALL;
        }
        if (vlan.m_vlan_info.bc_flood_type == SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED)
        {
            attr.id = SAI_VLAN_ATTR_BROADCAST_FLOOD_GROUP;
            attr.value.oid = SAI_NULL_OBJECT_ID;

            status = sai_vlan_api->set_vlan_attribute(vlan.m_vlan_info.vlan_oid, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set null l2mc group "
                               " to vlan %hu for broadcast flooding",
                               vlan.m_vlan_info.vlan_id);
                return false;
            }
            attr.id = SAI_VLAN_ATTR_BROADCAST_FLOOD_CONTROL_TYPE;
            attr.value.s32 = SAI_VLAN_FLOOD_CONTROL_TYPE_ALL;
            status = sai_vlan_api->set_vlan_attribute(vlan.m_vlan_info.vlan_oid, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set flood control type all"
                               " to vlan %hu for broadcast flooding",
                               vlan.m_vlan_info.vlan_id);
                return false;
            }
            vlan.m_vlan_info.bc_flood_type = SAI_VLAN_FLOOD_CONTROL_TYPE_ALL;
        }
        status = sai_l2mc_group_api->remove_l2mc_group(vlan.m_vlan_info.l2mc_group_id);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove l2mc group %" PRIx64, l2mc_group_id);
            return false;
        }
        vlan.m_vlan_info.l2mc_group_id = SAI_NULL_OBJECT_ID;
    }
    return true;
}

bool PortsOrch::removeVlanMember(Port &vlan, Port &port, string end_point_ip)
{
    SWSS_LOG_ENTER();

    if (!end_point_ip.empty())
    {
        return removeVlanEndPointIp(vlan, port, end_point_ip);
    }
    sai_object_id_t vlan_member_id;
    sai_vlan_tagging_mode_t sai_tagging_mode;
    auto vlan_member = m_portVlanMember[port.m_alias].find(vlan.m_vlan_info.vlan_id);

    /* Assert the port belongs to this VLAN */
    assert (vlan_member != m_portVlanMember[port.m_alias].end());
    sai_tagging_mode = vlan_member->second.vlan_mode;
    vlan_member_id = vlan_member->second.vlan_member_id;

    sai_status_t status = sai_vlan_api->remove_vlan_member(vlan_member_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove member %s from VLAN %s vid:%hx vmid:%" PRIx64,
                port.m_alias.c_str(), vlan.m_alias.c_str(), vlan.m_vlan_info.vlan_id, vlan_member_id);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_VLAN, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    m_portVlanMember[port.m_alias].erase(vlan_member);
    if (m_portVlanMember[port.m_alias].empty())
    {
        m_portVlanMember.erase(port.m_alias);
    }
    SWSS_LOG_NOTICE("Remove member %s from VLAN %s lid:%hx vmid:%" PRIx64,
            port.m_alias.c_str(), vlan.m_alias.c_str(), vlan.m_vlan_info.vlan_id, vlan_member_id);

    /* Restore to default pvid if this port joined this VLAN in untagged mode previously */
    if (sai_tagging_mode == SAI_VLAN_TAGGING_MODE_UNTAGGED)
    {
        if (!setPortPvid(port, DEFAULT_PORT_VLAN_ID))
        {
            return false;
        }
    }

    m_portList[port.m_alias] = port;
    vlan.m_members.erase(port.m_alias);
    m_portList[vlan.m_alias] = vlan;

    VlanMemberUpdate update = { vlan, port, false };
    notify(SUBJECT_TYPE_VLAN_MEMBER_CHANGE, static_cast<void *>(&update));

    return true;
}

bool PortsOrch::isVlanMember(Port &vlan, Port &port, string end_point_ip)
{
    if (!end_point_ip.empty())
    {
        if (vlan.m_vlan_info.l2mc_members.find(end_point_ip) != vlan.m_vlan_info.l2mc_members.end())
        {
            return true;
        }
        return false;
    }
    if (vlan.m_members.find(port.m_alias) == vlan.m_members.end())
       return false;

    return true;
}

bool PortsOrch::addLag(string lag_alias, uint32_t spa_id, int32_t switch_id)
{
    SWSS_LOG_ENTER();

    auto lagport = m_portList.find(lag_alias);
    if (lagport != m_portList.end())
    {
        /* The deletion of bridgeport attached to the lag may still be
         * pending due to fdb entries still present on the lag. Wait
         * until the cleanup is done.
         */
        if (m_portList[lag_alias].m_bridge_port_id != SAI_NULL_OBJECT_ID)
        {
            return false;
        }
        return true;
    }

    vector<sai_attribute_t> lag_attrs;
    string system_lag_alias = lag_alias;

    if (gMySwitchType == "voq")
    {
        if (switch_id < 0)
        {
            // Local PortChannel. Allocate unique lag id from central CHASSIS_APP_DB
            // Use the chassis wide unique system lag name.

            // Get the local switch id and derive the system lag name.

            switch_id = gVoqMySwitchId;
            system_lag_alias = gMyHostName + "|" + gMyAsicName + "|" + lag_alias;

            // Allocate unique lag id
            spa_id = m_lagIdAllocator->lagIdAdd(system_lag_alias, 0);

            if ((int32_t)spa_id <= 0)
            {
                SWSS_LOG_ERROR("Failed to allocate unique LAG id for local lag %s rv:%d", lag_alias.c_str(), spa_id);
                return false;
            }
        }

        sai_attribute_t attr;
        attr.id = SAI_LAG_ATTR_SYSTEM_PORT_AGGREGATE_ID;
        attr.value.u32 = spa_id;
        lag_attrs.push_back(attr);
    }

    sai_object_id_t lag_id;
    sai_status_t status = sai_lag_api->create_lag(&lag_id, gSwitchId, static_cast<uint32_t>(lag_attrs.size()), lag_attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create LAG %s lid:%" PRIx64, lag_alias.c_str(), lag_id);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_LAG, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("Create an empty LAG %s lid:%" PRIx64, lag_alias.c_str(), lag_id);

    Port lag(lag_alias, Port::LAG);
    lag.m_lag_id = lag_id;
    lag.m_members = set<string>();
    m_portList[lag_alias] = lag;
    m_port_ref_count[lag_alias] = 0;
    saiOidToAlias[lag_id] = lag_alias;

    PortUpdate update = { lag, true };
    notify(SUBJECT_TYPE_PORT_CHANGE, static_cast<void *>(&update));

    FieldValueTuple tuple(lag_alias, sai_serialize_object_id(lag_id));
    vector<FieldValueTuple> fields;
    fields.push_back(tuple);
    m_counterLagTable->set("", fields);

    if (gMySwitchType == "voq")
    {
        // If this is voq switch, record system lag info

        lag.m_system_lag_info.alias = system_lag_alias;
        lag.m_system_lag_info.switch_id = switch_id;
        lag.m_system_lag_info.spa_id = spa_id;

        // This will update port list with local port channel name for local port channels
        // and with system lag name for the system lags received from chassis app db

        m_portList[lag_alias] = lag;

        // Sync to SYSTEM_LAG_TABLE of CHASSIS_APP_DB

        voqSyncAddLag(lag);
    }

    return true;
}

bool PortsOrch::removeLag(Port lag)
{
    SWSS_LOG_ENTER();

    if (m_port_ref_count[lag.m_alias] > 0)
    {
        SWSS_LOG_ERROR("Failed to remove ref count %d LAG %s",
                        m_port_ref_count[lag.m_alias],
                        lag.m_alias.c_str());
        return false;
    }

    /* Retry when the LAG still has members */
    if (lag.m_members.size() > 0)
    {
        SWSS_LOG_ERROR("Failed to remove non-empty LAG %s", lag.m_alias.c_str());
        return false;
    }
    if (m_portVlanMember[lag.m_alias].size() > 0)
    {
        SWSS_LOG_ERROR("Failed to remove LAG %s, it is still in VLAN", lag.m_alias.c_str());
        return false;
    }

    if (lag.m_bridge_port_id != SAI_NULL_OBJECT_ID)
    {
        return false;
    }

    sai_status_t status = sai_lag_api->remove_lag(lag.m_lag_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove LAG %s lid:%" PRIx64, lag.m_alias.c_str(), lag.m_lag_id);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_LAG, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("Remove LAG %s lid:%" PRIx64, lag.m_alias.c_str(), lag.m_lag_id);

    saiOidToAlias.erase(lag.m_lag_id);
    m_portList.erase(lag.m_alias);
    m_port_ref_count.erase(lag.m_alias);

    PortUpdate update = { lag, false };
    notify(SUBJECT_TYPE_PORT_CHANGE, static_cast<void *>(&update));

    m_counterLagTable->hdel("", lag.m_alias);

    if (gMySwitchType == "voq")
    {
        // Free the lag id, if this is local LAG

        if (lag.m_system_lag_info.switch_id == gVoqMySwitchId)
        {
            int32_t rv;
            int32_t spa_id = lag.m_system_lag_info.spa_id;

            rv = m_lagIdAllocator->lagIdDel(lag.m_system_lag_info.alias);

            if (rv != spa_id)
            {
                SWSS_LOG_ERROR("Failed to delete LAG id %d of local lag %s rv:%d", spa_id, lag.m_alias.c_str(), rv);
                return false;
            }

            // Sync to SYSTEM_LAG_TABLE of CHASSIS_APP_DB

            voqSyncDelLag(lag);
        }
    }

    return true;
}

void PortsOrch::getLagMember(Port &lag, vector<Port> &portv)
{
    Port member;

    for (auto &name: lag.m_members)
    {
        if (!getPort(name, member))
        {
            SWSS_LOG_ERROR("Failed to get port for %s alias", name.c_str());
            return;
        }
        portv.push_back(member);
    }
}

bool PortsOrch::addLagMember(Port &lag, Port &port, bool enableForwarding)
{
    SWSS_LOG_ENTER();

    sai_uint32_t pvid;
    if (getPortPvid(lag, pvid))
    {
        setPortPvid (port, pvid);
    }

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_LAG_MEMBER_ATTR_LAG_ID;
    attr.value.oid = lag.m_lag_id;
    attrs.push_back(attr);

    attr.id = SAI_LAG_MEMBER_ATTR_PORT_ID;
    attr.value.oid = port.m_port_id;
    attrs.push_back(attr);

    if (!enableForwarding && port.m_type != Port::SYSTEM)
    {
        attr.id = SAI_LAG_MEMBER_ATTR_EGRESS_DISABLE;
        attr.value.booldata = true;
        attrs.push_back(attr);

        attr.id = SAI_LAG_MEMBER_ATTR_INGRESS_DISABLE;
        attr.value.booldata = true;
        attrs.push_back(attr);
    }

    sai_object_id_t lag_member_id;
    sai_status_t status = sai_lag_api->create_lag_member(&lag_member_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to add member %s to LAG %s lid:%" PRIx64 " pid:%" PRIx64,
                port.m_alias.c_str(), lag.m_alias.c_str(), lag.m_lag_id, port.m_port_id);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_LAG, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("Add member %s to LAG %s lid:%" PRIx64 " pid:%" PRIx64,
            port.m_alias.c_str(), lag.m_alias.c_str(), lag.m_lag_id, port.m_port_id);

    port.m_lag_id = lag.m_lag_id;
    port.m_lag_member_id = lag_member_id;
    m_portList[port.m_alias] = port;
    lag.m_members.insert(port.m_alias);

    m_portList[lag.m_alias] = lag;

    if (lag.m_bridge_port_id > 0)
    {
        if (!setHostIntfsStripTag(port, SAI_HOSTIF_VLAN_TAG_KEEP))
        {
            SWSS_LOG_ERROR("Failed to set %s for hostif of port %s which is in LAG %s",
                    hostif_vlan_tag[SAI_HOSTIF_VLAN_TAG_KEEP], port.m_alias.c_str(), lag.m_alias.c_str());
            return false;
        }
    }

    increasePortRefCount(port.m_alias);

    LagMemberUpdate update = { lag, port, true };
    notify(SUBJECT_TYPE_LAG_MEMBER_CHANGE, static_cast<void *>(&update));

    if (gMySwitchType == "voq")
    {
        //Sync to SYSTEM_LAG_MEMBER_TABLE of CHASSIS_APP_DB
        voqSyncAddLagMember(lag, port);
    }

    return true;
}

bool PortsOrch::removeLagMember(Port &lag, Port &port)
{
    sai_status_t status = sai_lag_api->remove_lag_member(port.m_lag_member_id);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove member %s from LAG %s lid:%" PRIx64 " lmid:%" PRIx64,
                port.m_alias.c_str(), lag.m_alias.c_str(), lag.m_lag_id, port.m_lag_member_id);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_LAG, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("Remove member %s from LAG %s lid:%" PRIx64 " lmid:%" PRIx64,
            port.m_alias.c_str(), lag.m_alias.c_str(), lag.m_lag_id, port.m_lag_member_id);

    port.m_lag_id = 0;
    port.m_lag_member_id = 0;
    m_portList[port.m_alias] = port;
    lag.m_members.erase(port.m_alias);
    m_portList[lag.m_alias] = lag;

    if (lag.m_bridge_port_id > 0)
    {
        if (!setHostIntfsStripTag(port, SAI_HOSTIF_VLAN_TAG_STRIP))
        {
            SWSS_LOG_ERROR("Failed to set %s for hostif of port %s which is leaving LAG %s",
                    hostif_vlan_tag[SAI_HOSTIF_VLAN_TAG_STRIP], port.m_alias.c_str(), lag.m_alias.c_str());
            return false;
        }
    }

    decreasePortRefCount(port.m_alias);

    LagMemberUpdate update = { lag, port, false };
    notify(SUBJECT_TYPE_LAG_MEMBER_CHANGE, static_cast<void *>(&update));

    if (gMySwitchType == "voq")
    {
        //Sync to SYSTEM_LAG_MEMBER_TABLE of CHASSIS_APP_DB
        voqSyncDelLagMember(lag, port);
    }

    return true;
}

bool PortsOrch::setLagTpid(sai_object_id_t id, sai_uint16_t tpid)
{
    SWSS_LOG_ENTER();
    sai_status_t status = SAI_STATUS_SUCCESS;
    sai_attribute_t attr;

    attr.id = SAI_LAG_ATTR_TPID;

    attr.value.u16 = (uint16_t)tpid;

    status = sai_lag_api->set_lag_attribute(id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set TPID 0x%x to LAG pid:%" PRIx64 ", rv:%d",
                attr.value.u16, id, status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_LAG, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    else
    {
        SWSS_LOG_NOTICE("Set TPID 0x%x to LAG pid:%" PRIx64 , attr.value.u16, id);
    }
    return true;
}


bool PortsOrch::setCollectionOnLagMember(Port &lagMember, bool enableCollection)
{
    /* Port must be LAG member */
    assert(lagMember.m_lag_member_id);

    // Collection is not applicable for system port lag members (i.e, members of remote LAGs)
    if (lagMember.m_type == Port::SYSTEM)
    {
        return true;
    }

    sai_status_t status = SAI_STATUS_FAILURE;
    sai_attribute_t attr {};

    attr.id = SAI_LAG_MEMBER_ATTR_INGRESS_DISABLE;
    attr.value.booldata = !enableCollection;

    status = sai_lag_api->set_lag_member_attribute(lagMember.m_lag_member_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to %s collection on LAG member %s",
            enableCollection ? "enable" : "disable",
            lagMember.m_alias.c_str());
        task_process_status handle_status = handleSaiSetStatus(SAI_API_LAG, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("%s collection on LAG member %s",
        enableCollection ? "Enable" : "Disable",
        lagMember.m_alias.c_str());

    return true;
}

bool PortsOrch::setDistributionOnLagMember(Port &lagMember, bool enableDistribution)
{
    /* Port must be LAG member */
    assert(lagMember.m_lag_member_id);

    // Distribution is not applicable for system port lag members (i.e, members of remote LAGs)
    if (lagMember.m_type == Port::SYSTEM)
    {
        return true;
    }

    sai_status_t status = SAI_STATUS_FAILURE;
    sai_attribute_t attr {};

    attr.id = SAI_LAG_MEMBER_ATTR_EGRESS_DISABLE;
    attr.value.booldata = !enableDistribution;

    status = sai_lag_api->set_lag_member_attribute(lagMember.m_lag_member_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to %s distribution on LAG member %s",
            enableDistribution ? "enable" : "disable",
            lagMember.m_alias.c_str());
        task_process_status handle_status = handleSaiSetStatus(SAI_API_LAG, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("%s distribution on LAG member %s",
        enableDistribution ? "Enable" : "Disable",
        lagMember.m_alias.c_str());

    return true;
}

bool PortsOrch::addTunnel(string tunnel_alias, sai_object_id_t tunnel_id, bool hwlearning)
{
    SWSS_LOG_ENTER();

    Port tunnel(tunnel_alias, Port::TUNNEL);
    tunnel.m_tunnel_id = tunnel_id;
    if (hwlearning)
    {
        tunnel.m_learn_mode = "hardware";
    }
    else
    {
        tunnel.m_learn_mode = "disable";
    }
    m_portList[tunnel_alias] = tunnel;

    SWSS_LOG_INFO("addTunnel:: %" PRIx64, tunnel_id);

    return true;
}

bool PortsOrch::removeTunnel(Port tunnel)
{
    SWSS_LOG_ENTER();

    m_portList.erase(tunnel.m_alias);

    return true;
}

void PortsOrch::generateQueueMap()
{
    if (m_isQueueMapGenerated)
    {
        return;
    }

    for (const auto& it: m_portList)
    {
        if (it.second.m_type == Port::PHY)
        {
            generateQueueMapPerPort(it.second);
        }
    }

    m_isQueueMapGenerated = true;
}

void PortsOrch::removeQueueMapPerPort(const Port& port)
{
    /* Remove the Queue map in the Counter DB */

    for (size_t queueIndex = 0; queueIndex < port.m_queue_ids.size(); ++queueIndex)
    {
        std::ostringstream name;
        name << port.m_alias << ":" << queueIndex;
        std::unordered_set<string> counter_stats;

        const auto id = sai_serialize_object_id(port.m_queue_ids[queueIndex]);

        m_queueTable->hdel("",name.str());
        m_queuePortTable->hdel("",id);

        string queueType;
        uint8_t queueRealIndex = 0;
        if (getQueueTypeAndIndex(port.m_queue_ids[queueIndex], queueType, queueRealIndex))
        {
            m_queueTypeTable->hdel("",id);
            m_queueIndexTable->hdel("",id);
        }

        for (const auto& it: queue_stat_ids)
        {
            counter_stats.emplace(sai_serialize_queue_stat(it));
        }
        queue_stat_manager.clearCounterIdList(port.m_queue_ids[queueIndex]);

        /* remove watermark queue counters */
        string key = getQueueWatermarkFlexCounterTableKey(id);

        m_flexCounterTable->del(key);
    }

    CounterCheckOrch::getInstance().removePort(port);
}

void PortsOrch::generateQueueMapPerPort(const Port& port)
{
    /* Create the Queue map in the Counter DB */
    /* Add stat counters to flex_counter */
    vector<FieldValueTuple> queueVector;
    vector<FieldValueTuple> queuePortVector;
    vector<FieldValueTuple> queueIndexVector;
    vector<FieldValueTuple> queueTypeVector;

    for (size_t queueIndex = 0; queueIndex < port.m_queue_ids.size(); ++queueIndex)
    {
        std::ostringstream name;
        name << port.m_alias << ":" << queueIndex;

        const auto id = sai_serialize_object_id(port.m_queue_ids[queueIndex]);

        queueVector.emplace_back(name.str(), id);
        queuePortVector.emplace_back(id, sai_serialize_object_id(port.m_port_id));

        string queueType;
        uint8_t queueRealIndex = 0;
        if (getQueueTypeAndIndex(port.m_queue_ids[queueIndex], queueType, queueRealIndex))
        {
            queueTypeVector.emplace_back(id, queueType);
            queueIndexVector.emplace_back(id, to_string(queueRealIndex));
        }

        // Install a flex counter for this queue to track stats
        std::unordered_set<string> counter_stats;
        for (const auto& it: queue_stat_ids)
        {
            counter_stats.emplace(sai_serialize_queue_stat(it));
        }
        queue_stat_manager.setCounterIdList(port.m_queue_ids[queueIndex], CounterType::QUEUE, counter_stats);

        /* add watermark queue counters */
        string key = getQueueWatermarkFlexCounterTableKey(id);

        string delimiter("");
        std::ostringstream counters_stream;
        for (const auto& it: queueWatermarkStatIds)
        {
            counters_stream << delimiter << sai_serialize_queue_stat(it);
            delimiter = comma;
        }

        vector<FieldValueTuple> fieldValues;
        fieldValues.emplace_back(QUEUE_COUNTER_ID_LIST, counters_stream.str());

        m_flexCounterTable->set(key, fieldValues);
    }

    m_queueTable->set("", queueVector);
    m_queuePortTable->set("", queuePortVector);
    m_queueIndexTable->set("", queueIndexVector);
    m_queueTypeTable->set("", queueTypeVector);

    CounterCheckOrch::getInstance().addPort(port);
}

void PortsOrch::generatePriorityGroupMap()
{
    if (m_isPriorityGroupMapGenerated)
    {
        return;
    }

    for (const auto& it: m_portList)
    {
        if (it.second.m_type == Port::PHY)
        {
            generatePriorityGroupMapPerPort(it.second);
        }
    }

    m_isPriorityGroupMapGenerated = true;
}

void PortsOrch::removePriorityGroupMapPerPort(const Port& port)
{
    /* Remove the PG map in the Counter DB */

    for (size_t pgIndex = 0; pgIndex < port.m_priority_group_ids.size(); ++pgIndex)
    {
        std::ostringstream name;
        name << port.m_alias << ":" << pgIndex;

        const auto id = sai_serialize_object_id(port.m_priority_group_ids[pgIndex]);
        string key = getPriorityGroupWatermarkFlexCounterTableKey(id);

        m_pgTable->hdel("",name.str());
        m_pgPortTable->hdel("",id);
        m_pgIndexTable->hdel("",id);

        m_flexCounterTable->del(key);

        key = getPriorityGroupDropPacketsFlexCounterTableKey(id);
        /* remove dropped packets counters to flex_counter */
        m_flexCounterTable->del(key);
    }

    CounterCheckOrch::getInstance().removePort(port);
}

void PortsOrch::generatePriorityGroupMapPerPort(const Port& port)
{
    /* Create the PG map in the Counter DB */
    /* Add stat counters to flex_counter */
    vector<FieldValueTuple> pgVector;
    vector<FieldValueTuple> pgPortVector;
    vector<FieldValueTuple> pgIndexVector;

    for (size_t pgIndex = 0; pgIndex < port.m_priority_group_ids.size(); ++pgIndex)
    {
        std::ostringstream name;
        name << port.m_alias << ":" << pgIndex;

        const auto id = sai_serialize_object_id(port.m_priority_group_ids[pgIndex]);

        pgVector.emplace_back(name.str(), id);
        pgPortVector.emplace_back(id, sai_serialize_object_id(port.m_port_id));
        pgIndexVector.emplace_back(id, to_string(pgIndex));

        string key = getPriorityGroupWatermarkFlexCounterTableKey(id);

        std::string delimiter = "";
        std::ostringstream counters_stream;
        /* Add watermark counters to flex_counter */
        for (const auto& it: ingressPriorityGroupWatermarkStatIds)
        {
            counters_stream << delimiter << sai_serialize_ingress_priority_group_stat(it);
            delimiter = comma;
        }

        vector<FieldValueTuple> fieldValues;
        fieldValues.emplace_back(PG_COUNTER_ID_LIST, counters_stream.str());
        m_flexCounterTable->set(key, fieldValues);

        delimiter = "";
        std::ostringstream ingress_pg_drop_packets_counters_stream;
        key = getPriorityGroupDropPacketsFlexCounterTableKey(id);
        /* Add dropped packets counters to flex_counter */
        for (const auto& it: ingressPriorityGroupDropStatIds)
        {
            ingress_pg_drop_packets_counters_stream << delimiter << sai_serialize_ingress_priority_group_stat(it);
            if (delimiter.empty())
            {
                delimiter = comma;
            }
        }
        fieldValues.clear();
        fieldValues.emplace_back(PG_COUNTER_ID_LIST, ingress_pg_drop_packets_counters_stream.str());
        m_flexCounterTable->set(key, fieldValues);
    }

    m_pgTable->set("", pgVector);
    m_pgPortTable->set("", pgPortVector);
    m_pgIndexTable->set("", pgIndexVector);

    CounterCheckOrch::getInstance().addPort(port);
}

void PortsOrch::generatePortCounterMap()
{
    if (m_isPortCounterMapGenerated)
    {
        return;
    }

    auto port_counter_stats = generateCounterStats(PORT_STAT_COUNTER_FLEX_COUNTER_GROUP);
    for (const auto& it: m_portList)
    {
        // Set counter stats only for PHY ports to ensure syncd will not try to query the counter statistics from the HW for non-PHY ports.
        if (it.second.m_type != Port::Type::PHY)
        {
            continue;
        }
        port_stat_manager.setCounterIdList(it.second.m_port_id, CounterType::PORT, port_counter_stats);
    }

    m_isPortCounterMapGenerated = true;
}

void PortsOrch::generatePortBufferDropCounterMap()
{
    if (m_isPortBufferDropCounterMapGenerated)
    {
        return;
    }

    auto port_buffer_drop_stats = generateCounterStats(PORT_BUFFER_DROP_STAT_FLEX_COUNTER_GROUP);
    for (const auto& it: m_portList)
    {
        // Set counter stats only for PHY ports to ensure syncd will not try to query the counter statistics from the HW for non-PHY ports.
        if (it.second.m_type != Port::Type::PHY)
        {
            continue;
        }
        port_buffer_drop_stat_manager.setCounterIdList(it.second.m_port_id, CounterType::PORT, port_buffer_drop_stats);
    }

    m_isPortBufferDropCounterMapGenerated = true;
}

void PortsOrch::doTask(NotificationConsumer &consumer)
{
    SWSS_LOG_ENTER();

    /* Wait for all ports to be initialized */
    if (!allPortsReady())
    {
        return;
    }

    std::string op;
    std::string data;
    std::vector<swss::FieldValueTuple> values;

    consumer.pop(op, data, values);

    if (&consumer != m_portStatusNotificationConsumer)
    {
        return;
    }

    if (op == "port_state_change")
    {
        uint32_t count;
        sai_port_oper_status_notification_t *portoperstatus = nullptr;

        sai_deserialize_port_oper_status_ntf(data, count, &portoperstatus);

        for (uint32_t i = 0; i < count; i++)
        {
            sai_object_id_t id = portoperstatus[i].port_id;
            sai_port_oper_status_t status = portoperstatus[i].port_state;

            SWSS_LOG_NOTICE("Get port state change notification id:%" PRIx64 " status:%d", id, status);

            Port port;

            if (!getPort(id, port))
            {
                SWSS_LOG_ERROR("Failed to get port object for port id 0x%" PRIx64, id);
                continue;
            }

            updatePortOperStatus(port, status);
            if (status == SAI_PORT_OPER_STATUS_UP)
            {
                sai_uint32_t speed;
                if (getPortOperSpeed(port, speed))
                {
                    SWSS_LOG_NOTICE("%s oper speed is %d", port.m_alias.c_str(), speed);
                    updateDbPortOperSpeed(port, speed);
                }
                else
                {
                    updateDbPortOperSpeed(port, 0);
                }
            }

            /* update m_portList */
            m_portList[port.m_alias] = port;
        }

        sai_deserialize_free_port_oper_status_ntf(count, portoperstatus);
    }
}

void PortsOrch::updatePortOperStatus(Port &port, sai_port_oper_status_t status)
{
    SWSS_LOG_NOTICE("Port %s oper state set from %s to %s",
            port.m_alias.c_str(), oper_status_strings.at(port.m_oper_status).c_str(),
            oper_status_strings.at(status).c_str());
    if (status == port.m_oper_status)
    {
        return;
    }

    if (port.m_type == Port::PHY)
    {
        updateDbPortOperStatus(port, status);
    }
    port.m_oper_status = status;

    if(port.m_type == Port::TUNNEL)
    {
        return;
    }

    bool isUp = status == SAI_PORT_OPER_STATUS_UP;
    if (port.m_type == Port::PHY)
    {
        if (!setHostIntfsOperStatus(port, isUp))
        {
            SWSS_LOG_ERROR("Failed to set host interface %s operational status %s", port.m_alias.c_str(),
                    isUp ? "up" : "down");
        }
    }
    if (!gNeighOrch->ifChangeInformNextHop(port.m_alias, isUp))
    {
        SWSS_LOG_WARN("Inform nexthop operation failed for interface %s", port.m_alias.c_str());
    }
    for (const auto &child_port : port.m_child_ports)
    {
        if (!gNeighOrch->ifChangeInformNextHop(child_port, isUp))
        {
            SWSS_LOG_WARN("Inform nexthop operation failed for sub interface %s", child_port.c_str());
        }
    }

    PortOperStateUpdate update = {port, status};
    notify(SUBJECT_TYPE_PORT_OPER_STATE_CHANGE, static_cast<void *>(&update));
}

void PortsOrch::updateDbPortOperSpeed(Port &port, sai_uint32_t speed)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> tuples;
    string speedStr = speed != 0 ? to_string(speed) : "N/A";
    tuples.emplace_back(std::make_pair("speed", speedStr));
    m_portStateTable.set(port.m_alias, tuples);

    // We don't set port.m_speed = speed here, because CONFIG_DB still hold the old
    // value. If we set it here, next time configure any attributes related port will
    // cause a port flapping.
}

/*
 * sync up orchagent with libsai/ASIC for port state.
 *
 * Currently NotificationProducer is used by syncd to inform port state change,
 * which means orchagent will miss the signal if it happens between orchagent shutdown and startup.
 * Syncd doesn't know whether the signal has been lost or not.
 * Also the source of notification event is from libsai/SDK.
 *
 * Latest oper status for each port is retrieved via SAI_PORT_ATTR_OPER_STATUS sai API,
 * the hostif and db are updated accordingly.
 */
void PortsOrch::refreshPortStatus()
{
    SWSS_LOG_ENTER();

    for (auto &it: m_portList)
    {
        auto &port = it.second;
        if (port.m_type != Port::PHY)
        {
            continue;
        }

        sai_port_oper_status_t status;
        if (!getPortOperStatus(port, status))
        {
            throw runtime_error("PortsOrch get port oper status failure");
        }

        SWSS_LOG_INFO("%s oper status is %s", port.m_alias.c_str(), oper_status_strings.at(status).c_str());
        updatePortOperStatus(port, status);

        if (status == SAI_PORT_OPER_STATUS_UP)
        {
            sai_uint32_t speed;
            if (getPortOperSpeed(port, speed))
            {
                SWSS_LOG_INFO("%s oper speed is %d", port.m_alias.c_str(), speed);
                updateDbPortOperSpeed(port, speed);
            }
            else
            {
                updateDbPortOperSpeed(port, 0);
            }
        }
    }
}

bool PortsOrch::getPortOperStatus(const Port& port, sai_port_oper_status_t& status) const
{
    SWSS_LOG_ENTER();

    if (port.m_type != Port::PHY)
    {
        return false;
    }

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_OPER_STATUS;

    sai_status_t ret = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (ret != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get oper_status for %s", port.m_alias.c_str());
        return false;
    }

    status = static_cast<sai_port_oper_status_t>(attr.value.u32);

    return true;
}

bool PortsOrch::getPortOperSpeed(const Port& port, sai_uint32_t& speed) const
{
    SWSS_LOG_ENTER();

    if (port.m_type != Port::PHY)
    {
        return false;
    }

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_OPER_SPEED;

    sai_status_t ret = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (ret != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get oper speed for %s", port.m_alias.c_str());
        return false;
    }

    speed = static_cast<sai_uint32_t>(attr.value.u32);

    if (speed == 0)
    {
        // Port operational status is up, but operational speed is 0. It could be a valid case because
        // port state can change during two SAI calls:
        //    1. getPortOperStatus returns UP
        //    2. port goes down due to any reason
        //    3. getPortOperSpeed gets speed value 0
        // And it could also be a bug. So, we log a warning here.
        SWSS_LOG_WARN("Port %s operational speed is 0", port.m_alias.c_str());
        return false;
    }

    return true;
}

bool PortsOrch::getSaiAclBindPointType(Port::Type           type,
                                       sai_acl_bind_point_type_t &sai_acl_bind_type)
{
    switch(type)
    {
        case Port::PHY:
            sai_acl_bind_type = SAI_ACL_BIND_POINT_TYPE_PORT;
            break;
        case Port::LAG:
            sai_acl_bind_type = SAI_ACL_BIND_POINT_TYPE_LAG;
            break;
        case Port::VLAN:
            sai_acl_bind_type = SAI_ACL_BIND_POINT_TYPE_VLAN;
            break;
        default:
            // Dealing with port, lag and vlan for now.
            return false;
    }
    return true;
}

bool PortsOrch::removeAclTableGroup(const Port &p)
{
    sai_acl_bind_point_type_t bind_type;
    if (!getSaiAclBindPointType(p.m_type, bind_type))
    {
        SWSS_LOG_ERROR("Unknown SAI ACL bind point type");
        return false;
    }

    sai_status_t ret;
    if (p.m_ingress_acl_table_group_id != 0)
    {
        ret = sai_acl_api->remove_acl_table_group(p.m_ingress_acl_table_group_id);
        if (ret != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove ingress acl table group for %s", p.m_alias.c_str());
            task_process_status handle_status = handleSaiRemoveStatus(SAI_API_ACL, ret);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
        gCrmOrch->decCrmAclUsedCounter(CrmResourceType::CRM_ACL_GROUP, SAI_ACL_STAGE_INGRESS, bind_type, p.m_ingress_acl_table_group_id);
    }

    if (p.m_egress_acl_table_group_id != 0)
    {
        ret = sai_acl_api->remove_acl_table_group(p.m_egress_acl_table_group_id);
        if (ret != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove egress acl table group for %s", p.m_alias.c_str());
            task_process_status handle_status = handleSaiRemoveStatus(SAI_API_ACL, ret);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
        gCrmOrch->decCrmAclUsedCounter(CrmResourceType::CRM_ACL_GROUP, SAI_ACL_STAGE_EGRESS, bind_type, p.m_egress_acl_table_group_id);
    }
    return true;
}

bool PortsOrch::setPortSerdesAttribute(sai_object_id_t port_id,
                                       map<sai_port_serdes_attr_t, vector<uint32_t>> &serdes_attr)
{
    SWSS_LOG_ENTER();

    vector<sai_attribute_t> attr_list;
    sai_attribute_t port_attr;
    sai_attribute_t port_serdes_attr;
    sai_status_t status;
    sai_object_id_t port_serdes_id = SAI_NULL_OBJECT_ID;

    port_attr.id = SAI_PORT_ATTR_PORT_SERDES_ID;
    status = sai_port_api->get_port_attribute(port_id, 1, &port_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get port attr serdes id %d to port pid:0x%" PRIx64,
                       port_attr.id, port_id);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
        if (handle_status != task_process_status::task_success)
        {
            return false;
        }
    }

    if (port_attr.value.oid != SAI_NULL_OBJECT_ID)
    {
        status = sai_port_api->remove_port_serdes(port_attr.value.oid);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove existing port serdes attr 0x%" PRIx64 " port 0x%" PRIx64,
                           port_attr.value.oid, port_id);
            task_process_status handle_status = handleSaiRemoveStatus(SAI_API_PORT, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }


    port_serdes_attr.id = SAI_PORT_SERDES_ATTR_PORT_ID;
    port_serdes_attr.value.oid = port_id;
    attr_list.emplace_back(port_serdes_attr);
    SWSS_LOG_INFO("Creating serdes for port 0x%" PRIx64, port_id);

    for (auto it = serdes_attr.begin(); it != serdes_attr.end(); it++)
    {
        port_serdes_attr.id = it->first;
        port_serdes_attr.value.u32list.count = (uint32_t)it->second.size();
        port_serdes_attr.value.u32list.list = it->second.data();
        attr_list.emplace_back(port_serdes_attr);
    }
    status = sai_port_api->create_port_serdes(&port_serdes_id, gSwitchId,
                                              static_cast<uint32_t>(serdes_attr.size()+1),
                                              attr_list.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create port serdes for port 0x%" PRIx64,
                       port_id);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    SWSS_LOG_NOTICE("Created port serdes object 0x%" PRIx64 " for port 0x%" PRIx64, port_serdes_id, port_id);
    return true;
}

void PortsOrch::removePortSerdesAttribute(sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    sai_attribute_t port_attr;
    sai_status_t status;
    sai_object_id_t port_serdes_id = SAI_NULL_OBJECT_ID;

    port_attr.id = SAI_PORT_ATTR_PORT_SERDES_ID;
    status = sai_port_api->get_port_attribute(port_id, 1, &port_attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_DEBUG("Failed to get port attr serdes id %d to port pid:0x%" PRIx64,
                       port_attr.id, port_id);
        return;
    }

    if (port_attr.value.oid != SAI_NULL_OBJECT_ID)
    {
        status = sai_port_api->remove_port_serdes(port_attr.value.oid);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove existing port serdes attr 0x%" PRIx64 " port 0x%" PRIx64,
                           port_attr.value.oid, port_id);
            handleSaiRemoveStatus(SAI_API_PORT, status);
            return;
        }
    }
    SWSS_LOG_NOTICE("Removed port serdes object 0x%" PRIx64 " for port 0x%" PRIx64, port_serdes_id, port_id);
}

void PortsOrch::getPortSerdesVal(const std::string& val_str,
                                 std::vector<uint32_t> &lane_values)
{
    SWSS_LOG_ENTER();

    uint32_t lane_val;
    std::string lane_str;
    std::istringstream iss(val_str);

    while (std::getline(iss, lane_str, ','))
    {
        lane_val = (uint32_t)std::stoul(lane_str, NULL, 16);
        lane_values.push_back(lane_val);
    }
}

bool PortsOrch::getPortAdvSpeedsVal(const std::string &val_str,
                                    std::vector<uint32_t> &speed_values)
{
    SWSS_LOG_ENTER();

    if (val_str == "all")
    {
        return true;
    }

    uint32_t speed_val;
    std::string speed_str;
    std::istringstream iss(val_str);

    try
    {
        while (std::getline(iss, speed_str, ','))
        {
            speed_val = (uint32_t)std::stoul(speed_str);
            speed_values.push_back(speed_val);
        }
    }
    catch (const std::invalid_argument &e)
    {
        SWSS_LOG_ERROR("Failed to parse adv_speeds value: %s", val_str.c_str());
        return false;
    }
    std::sort(speed_values.begin(), speed_values.end());
    return true;
}

bool PortsOrch::getPortInterfaceTypeVal(const std::string &s,
                                        sai_port_interface_type_t &interface_type)
{
    SWSS_LOG_ENTER();

    auto iter = interface_type_map_for_an.find(s);
    if (iter != interface_type_map_for_an.end())
    {
        interface_type = interface_type_map_for_an[s];
        return true;
    }
    else
    {
        const std::string &validInterfaceTypes = getValidInterfaceTypes();
        SWSS_LOG_ERROR("Failed to parse interface_type value %s, valid interface type includes: %s",
                       s.c_str(), validInterfaceTypes.c_str());
        return false;
    }
}

bool PortsOrch::getPortAdvInterfaceTypesVal(const std::string &val_str,
                                            std::vector<uint32_t> &type_values)
{
    SWSS_LOG_ENTER();
    if (val_str == "all")
    {
        return true;
    }

    sai_port_interface_type_t interface_type ;
    std::string type_str;
    std::istringstream iss(val_str);
    bool valid;

    while (std::getline(iss, type_str, ','))
    {
        valid = getPortInterfaceTypeVal(type_str, interface_type);
        if (!valid) {
            const std::string &validInterfaceTypes = getValidInterfaceTypes();
            SWSS_LOG_ERROR("Failed to parse adv_interface_types value %s, valid interface type includes: %s",
                           val_str.c_str(), validInterfaceTypes.c_str());
            return false;
        }
        type_values.push_back(static_cast<uint32_t>(interface_type));
    }
    std::sort(type_values.begin(), type_values.end());
    return true;
}

/* Bring up/down Vlan interface associated with L3 VNI*/
bool PortsOrch::updateL3VniStatus(uint16_t vlan_id, bool isUp)
{
    Port vlan;
    string vlan_alias;

    vlan_alias = VLAN_PREFIX + to_string(vlan_id);
    SWSS_LOG_INFO("update L3Vni Status for Vlan %d with isUp %d vlan %s",
            vlan_id, isUp, vlan_alias.c_str());

    if (!getPort(vlan_alias, vlan))
    {
        SWSS_LOG_INFO("Failed to locate VLAN %d", vlan_id);
        return false;
    }

    SWSS_LOG_INFO("member count %d, l3vni %d", vlan.m_up_member_count, vlan.m_l3_vni);
    if (isUp) {
        auto old_count = vlan.m_up_member_count;
        vlan.m_up_member_count++;
        if (old_count == 0)
        {
            /* updateVlanOperStatus(vlan, true); */ /* TBD */
            vlan.m_oper_status = SAI_PORT_OPER_STATUS_UP;
        }
        vlan.m_l3_vni = true;
    } else {
        vlan.m_up_member_count--;
        if (vlan.m_up_member_count == 0)
        {
            /* updateVlanOperStatus(vlan, false); */ /* TBD */
            vlan.m_oper_status = SAI_PORT_OPER_STATUS_DOWN;
        }
        vlan.m_l3_vni = false;
    }

    m_portList[vlan_alias] = vlan;

    SWSS_LOG_INFO("Updated L3Vni status of VLAN %d member count %d", vlan_id, vlan.m_up_member_count);

    return true;
}

/*
 * If Gearbox is enabled (wait for GearboxConfigDone),
 * then initialize global storage maps
 */
void PortsOrch::initGearbox()
{
    GearboxUtils gearbox;
    Table* tmpGearboxTable = m_gearboxTable.get();
    m_gearboxEnabled = gearbox.isGearboxEnabled(tmpGearboxTable);

    SWSS_LOG_ENTER();

    if (m_gearboxEnabled)
    {
        m_gearboxPhyMap = gearbox.loadPhyMap(tmpGearboxTable);
        m_gearboxInterfaceMap = gearbox.loadInterfaceMap(tmpGearboxTable);
        m_gearboxLaneMap = gearbox.loadLaneMap(tmpGearboxTable);
        m_gearboxPortMap = gearbox.loadPortMap(tmpGearboxTable);

        SWSS_LOG_NOTICE("BOX: m_gearboxPhyMap size       = %d.", (int) m_gearboxPhyMap.size());
        SWSS_LOG_NOTICE("BOX: m_gearboxInterfaceMap size = %d.", (int) m_gearboxInterfaceMap.size());
        SWSS_LOG_NOTICE("BOX: m_gearboxLaneMap size      = %d.", (int) m_gearboxLaneMap.size());
        SWSS_LOG_NOTICE("BOX: m_gearboxPortMap size      = %d.", (int) m_gearboxPortMap.size());
    }
}

/*
 * Create both the system-side and line-side gearbox ports for the associated
 * PHY and connect the ports.
 *
 */
bool PortsOrch::initGearboxPort(Port &port)
{
    vector<sai_attribute_t> attrs;
    vector<uint32_t> lanes;
    vector<uint32_t> vals;
    sai_attribute_t attr;
    sai_object_id_t systemPort;
    sai_object_id_t linePort;
    sai_object_id_t connector;
    sai_object_id_t phyOid;
    sai_status_t status;
    string phyOidStr;
    int phy_id;

    SWSS_LOG_ENTER();

    if (m_gearboxEnabled)
    {
        if (m_gearboxInterfaceMap.find(port.m_index) != m_gearboxInterfaceMap.end())
        {
            SWSS_LOG_NOTICE("BOX: port_id:0x%" PRIx64 " index:%d alias:%s", port.m_port_id, port.m_index, port.m_alias.c_str());

            phy_id = m_gearboxInterfaceMap[port.m_index].phy_id;
            phyOidStr = m_gearboxPhyMap[phy_id].phy_oid;

            if (phyOidStr.size() == 0)
            {
                SWSS_LOG_ERROR("BOX: Gearbox PHY phy_id:%d has an invalid phy_oid", phy_id);
                return false;
            }

            sai_deserialize_object_id(phyOidStr, phyOid);

            SWSS_LOG_NOTICE("BOX: Gearbox port %s assigned phyOid 0x%" PRIx64, port.m_alias.c_str(), phyOid);
            port.m_switch_id = phyOid;

            /* Create SYSTEM-SIDE port */
            attrs.clear();

            attr.id = SAI_PORT_ATTR_ADMIN_STATE;
            attr.value.booldata = port.m_admin_state_up;
            attrs.push_back(attr);

            attr.id = SAI_PORT_ATTR_HW_LANE_LIST;
            lanes.assign(m_gearboxInterfaceMap[port.m_index].system_lanes.begin(), m_gearboxInterfaceMap[port.m_index].system_lanes.end());
            attr.value.u32list.list = lanes.data();
            attr.value.u32list.count = static_cast<uint32_t>(lanes.size());
            attrs.push_back(attr);

            for (uint32_t i = 0; i < attr.value.u32list.count; i++)
            {
                SWSS_LOG_DEBUG("BOX: list[%d] = %d", i, attr.value.u32list.list[i]);
            }

            attr.id = SAI_PORT_ATTR_SPEED;
            attr.value.u32 = (uint32_t) m_gearboxPortMap[port.m_index].system_speed * (uint32_t) lanes.size();
            if (isSpeedSupported(port.m_alias, port.m_port_id, attr.value.u32))
            {
                attrs.push_back(attr);
            }

            attr.id = SAI_PORT_ATTR_AUTO_NEG_MODE;
            attr.value.booldata = m_gearboxPortMap[port.m_index].system_auto_neg;
            attrs.push_back(attr);

            attr.id = SAI_PORT_ATTR_FEC_MODE;
            attr.value.s32 = fec_mode_map[m_gearboxPortMap[port.m_index].system_fec];
            attrs.push_back(attr);

            attr.id = SAI_PORT_ATTR_INTERNAL_LOOPBACK_MODE;
            attr.value.u32 = loopback_mode_map[m_gearboxPortMap[port.m_index].system_loopback];
            attrs.push_back(attr);

            attr.id = SAI_PORT_ATTR_LINK_TRAINING_ENABLE;
            attr.value.booldata = m_gearboxPortMap[port.m_index].system_training;
            attrs.push_back(attr);

            status = sai_port_api->create_port(&systemPort, phyOid, static_cast<uint32_t>(attrs.size()), attrs.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("BOX: Failed to create Gearbox system-side port for alias:%s port_id:0x%" PRIx64 " index:%d status:%d",
                        port.m_alias.c_str(), port.m_port_id, port.m_index, status);
                task_process_status handle_status = handleSaiCreateStatus(SAI_API_PORT, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
            SWSS_LOG_NOTICE("BOX: Created Gearbox system-side port 0x%" PRIx64 " for alias:%s index:%d",
                    systemPort, port.m_alias.c_str(), port.m_index);

            /* Create LINE-SIDE port */
            attrs.clear();

            attr.id = SAI_PORT_ATTR_ADMIN_STATE;
            attr.value.booldata = port.m_admin_state_up;
            attrs.push_back(attr);

            attr.id = SAI_PORT_ATTR_HW_LANE_LIST;
            lanes.assign(m_gearboxInterfaceMap[port.m_index].line_lanes.begin(), m_gearboxInterfaceMap[port.m_index].line_lanes.end());
            attr.value.u32list.list = lanes.data();
            attr.value.u32list.count = static_cast<uint32_t>(lanes.size());
            attrs.push_back(attr);

            for (uint32_t i = 0; i < attr.value.u32list.count; i++)
            {
                SWSS_LOG_DEBUG("BOX: list[%d] = %d", i, attr.value.u32list.list[i]);
            }

            attr.id = SAI_PORT_ATTR_SPEED;
            attr.value.u32 = (uint32_t) m_gearboxPortMap[port.m_index].line_speed * (uint32_t) lanes.size();
            if (isSpeedSupported(port.m_alias, port.m_port_id, attr.value.u32))
            {
                attrs.push_back(attr);
            }

            attr.id = SAI_PORT_ATTR_AUTO_NEG_MODE;
            attr.value.booldata = m_gearboxPortMap[port.m_index].line_auto_neg;
            attrs.push_back(attr);

            attr.id = SAI_PORT_ATTR_FEC_MODE;
            attr.value.s32 = fec_mode_map[m_gearboxPortMap[port.m_index].line_fec];
            attrs.push_back(attr);

            attr.id = SAI_PORT_ATTR_MEDIA_TYPE;
            attr.value.u32 = media_type_map[m_gearboxPortMap[port.m_index].line_media_type];
            attrs.push_back(attr);

            attr.id = SAI_PORT_ATTR_INTERNAL_LOOPBACK_MODE;
            attr.value.u32 = loopback_mode_map[m_gearboxPortMap[port.m_index].line_loopback];
            attrs.push_back(attr);

            attr.id = SAI_PORT_ATTR_LINK_TRAINING_ENABLE;
            attr.value.booldata = m_gearboxPortMap[port.m_index].line_training;
            attrs.push_back(attr);

            attr.id = SAI_PORT_ATTR_INTERFACE_TYPE;
            attr.value.u32 = interface_type_map[m_gearboxPortMap[port.m_index].line_intf_type];
            attrs.push_back(attr);

            attr.id = SAI_PORT_ATTR_ADVERTISED_SPEED;
            vals.assign(m_gearboxPortMap[port.m_index].line_adver_speed.begin(), m_gearboxPortMap[port.m_index].line_adver_speed.end());
            attr.value.u32list.list = vals.data();
            attr.value.u32list.count = static_cast<uint32_t>(vals.size());
            attrs.push_back(attr);

            attr.id = SAI_PORT_ATTR_ADVERTISED_FEC_MODE;
            vals.assign(m_gearboxPortMap[port.m_index].line_adver_fec.begin(), m_gearboxPortMap[port.m_index].line_adver_fec.end());
            attr.value.u32list.list = vals.data();
            attr.value.u32list.count = static_cast<uint32_t>(vals.size());
            attrs.push_back(attr);

            attr.id = SAI_PORT_ATTR_ADVERTISED_AUTO_NEG_MODE;
            attr.value.booldata = m_gearboxPortMap[port.m_index].line_adver_auto_neg;
            attrs.push_back(attr);

            attr.id = SAI_PORT_ATTR_ADVERTISED_ASYMMETRIC_PAUSE_MODE;
            attr.value.booldata = m_gearboxPortMap[port.m_index].line_adver_asym_pause;
            attrs.push_back(attr);

            attr.id = SAI_PORT_ATTR_ADVERTISED_MEDIA_TYPE;
            attr.value.u32 = media_type_map[m_gearboxPortMap[port.m_index].line_adver_media_type];
            attrs.push_back(attr);

            status = sai_port_api->create_port(&linePort, phyOid, static_cast<uint32_t>(attrs.size()), attrs.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("BOX: Failed to create Gearbox line-side port for alias:%s port_id:0x%" PRIx64 " index:%d status:%d",
                   port.m_alias.c_str(), port.m_port_id, port.m_index, status);
                task_process_status handle_status = handleSaiCreateStatus(SAI_API_PORT, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
            SWSS_LOG_NOTICE("BOX: Created Gearbox line-side port 0x%" PRIx64 " for alias:%s index:%d",
                linePort, port.m_alias.c_str(), port.m_index);

            /* Connect SYSTEM-SIDE to LINE-SIDE */
            attrs.clear();

            attr.id = SAI_PORT_CONNECTOR_ATTR_SYSTEM_SIDE_PORT_ID;
            attr.value.oid = systemPort;
            attrs.push_back(attr);
            attr.id = SAI_PORT_CONNECTOR_ATTR_LINE_SIDE_PORT_ID;
            attr.value.oid = linePort;
            attrs.push_back(attr);

            status = sai_port_api->create_port_connector(&connector, phyOid, static_cast<uint32_t>(attrs.size()), attrs.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("BOX: Failed to connect Gearbox system-side:0x%" PRIx64 " to line-side:0x%" PRIx64 "; status:%d", systemPort, linePort, status);
                task_process_status handle_status = handleSaiCreateStatus(SAI_API_PORT, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }

            SWSS_LOG_NOTICE("BOX: Connected Gearbox ports; system-side:0x%" PRIx64 " to line-side:0x%" PRIx64, systemPort, linePort);
            m_gearboxPortListLaneMap[port.m_port_id] = make_tuple(systemPort, linePort);
            port.m_line_side_id = linePort;
        }
    }

    return true;
}

const gearbox_phy_t* PortsOrch::getGearboxPhy(const Port &port)
{
    auto gearbox_interface = m_gearboxInterfaceMap.find(port.m_index);
    if (gearbox_interface == m_gearboxInterfaceMap.end())
    {
        return nullptr;
    }

    auto phy = m_gearboxPhyMap.find(gearbox_interface->second.phy_id);
    if (phy == m_gearboxPhyMap.end())
    {
        SWSS_LOG_ERROR("Gearbox Phy %d dones't exist", gearbox_interface->second.phy_id);
        return nullptr;
    }

    return &phy->second;
}

bool PortsOrch::getPortIPG(sai_object_id_t port_id, uint32_t &ipg)
{
    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_IPG;

    sai_status_t status = sai_port_api->get_port_attribute(port_id, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    ipg = attr.value.u32;

    return true;
}

bool PortsOrch::setPortIPG(sai_object_id_t port_id, uint32_t ipg)
{
    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_IPG;
    attr.value.u32 = ipg;

    sai_status_t status = sai_port_api->set_port_attribute(port_id, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

bool PortsOrch::getSystemPorts()
{
    sai_status_t status;
    sai_attribute_t attr;
    uint32_t i;

    m_systemPortCount = 0;

    attr.id = SAI_SWITCH_ATTR_NUMBER_OF_SYSTEM_PORTS;

    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_INFO("Failed to get number of system ports, rv:%d", status);
        return false;
    }

    m_systemPortCount = attr.value.u32;
    SWSS_LOG_NOTICE("Got %d system ports", m_systemPortCount);

    if(m_systemPortCount)
    {
        /* Make <switch_id, core, core port> tuple and system port oid map */

        vector<sai_object_id_t> system_port_list;
        system_port_list.resize(m_systemPortCount);

        attr.id = SAI_SWITCH_ATTR_SYSTEM_PORT_LIST;
        attr.value.objlist.count = (uint32_t)system_port_list.size();
        attr.value.objlist.list = system_port_list.data();

        status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to get system port list, rv:%d", status);
            task_process_status handle_status = handleSaiGetStatus(SAI_API_SWITCH, status);
            if (handle_status != task_process_status::task_success)
            {
                return false;
            }
        }

        uint32_t spcnt = attr.value.objlist.count;
        for(i = 0; i < spcnt; i++)
        {
            attr.id = SAI_SYSTEM_PORT_ATTR_CONFIG_INFO;

            status = sai_system_port_api->get_system_port_attribute(system_port_list[i], 1, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to get system port config info spid:%" PRIx64, system_port_list[i]);
                task_process_status handle_status = handleSaiGetStatus(SAI_API_SYSTEM_PORT, status);
                if (handle_status != task_process_status::task_success)
                {
                    return false;
                }
            }

            SWSS_LOG_NOTICE("SystemPort(0x%" PRIx64 ") - port_id:%u, switch_id:%u, core:%u, core_port:%u, speed:%u, voqs:%u",
                            system_port_list[i],
                            attr.value.sysportconfig.port_id,
                            attr.value.sysportconfig.attached_switch_id,
                            attr.value.sysportconfig.attached_core_index,
                            attr.value.sysportconfig.attached_core_port_index,
                            attr.value.sysportconfig.speed,
                            attr.value.sysportconfig.num_voq);

            tuple<int, int, int> sp_key(attr.value.sysportconfig.attached_switch_id,
                    attr.value.sysportconfig.attached_core_index,
                    attr.value.sysportconfig.attached_core_port_index);

            m_systemPortOidMap[sp_key] = system_port_list[i];
        }
    }

    return true;
}

bool PortsOrch::getRecircPort(Port &port, string role)
{
    for (auto it = m_recircPortRole.begin(); it != m_recircPortRole.end(); it++)
    {
        if (it->second == role)
        {
            return getPort(it->first, port);
        }
    }
    SWSS_LOG_ERROR("Failed to find recirc port with role %s", role.c_str());
    return false;
}

bool PortsOrch::addSystemPorts()
{
    vector<string> keys;
    vector<FieldValueTuple> spFv;

    DBConnector appDb(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    Table appSystemPortTable(&appDb, APP_SYSTEM_PORT_TABLE_NAME);

    //Retrieve system port configurations from APP DB
    appSystemPortTable.getKeys(keys);
    for ( auto &alias : keys )
    {
        appSystemPortTable.get(alias, spFv);

        int32_t system_port_id = -1;
        int32_t switch_id = -1;
        int32_t core_index = -1;
        int32_t core_port_index = -1;

        for ( auto &fv : spFv )
        {
            if(fv.first == "switch_id")
            {
                switch_id = stoi(fv.second);
                continue;
            }
            if(fv.first == "core_index")
            {
                core_index = stoi(fv.second);
                continue;
            }
            if(fv.first == "core_port_index")
            {
                core_port_index = stoi(fv.second);
                continue;
            }
            if(fv.first == "system_port_id")
            {
                system_port_id = stoi(fv.second);
                continue;
            }
        }

        if(system_port_id < 0 || switch_id < 0 || core_index < 0 || core_port_index < 0)
        {
            SWSS_LOG_ERROR("Invalid or Missing field values for %s! system_port id:%d, switch_id:%d, core_index:%d, core_port_index:%d",
                    alias.c_str(), system_port_id, switch_id, core_index, core_port_index);
            continue;
        }

        tuple<int, int, int> sp_key(switch_id, core_index, core_port_index);

        if(m_systemPortOidMap.find(sp_key) != m_systemPortOidMap.end())
        {

            sai_attribute_t attr;
            vector<sai_attribute_t> attrs;
            sai_object_id_t system_port_oid;
            sai_status_t status;

            //Retrive system port config info and enable
            system_port_oid = m_systemPortOidMap[sp_key];

            attr.id = SAI_SYSTEM_PORT_ATTR_TYPE;
            attrs.push_back(attr);

            attr.id = SAI_SYSTEM_PORT_ATTR_CONFIG_INFO;
            attrs.push_back(attr);

            status = sai_system_port_api->get_system_port_attribute(system_port_oid, static_cast<uint32_t>(attrs.size()), attrs.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to get system port config info spid:%" PRIx64, system_port_oid);
                task_process_status handle_status = handleSaiGetStatus(SAI_API_SYSTEM_PORT, status);
                if (handle_status != task_process_status::task_success)
                {
                    continue;
                }
            }

            //Create or update system port and add to the port list.
            Port port(alias, Port::SYSTEM);
            port.m_port_id = system_port_oid;
            port.m_admin_state_up = true;
            port.m_oper_status = SAI_PORT_OPER_STATUS_UP;
            port.m_speed = attrs[1].value.sysportconfig.speed;
            port.m_mtu = DEFAULT_SYSTEM_PORT_MTU;
            if (attrs[0].value.s32 == SAI_SYSTEM_PORT_TYPE_LOCAL)
            {
                //Get the local port oid
                attr.id = SAI_SYSTEM_PORT_ATTR_PORT;

                status = sai_system_port_api->get_system_port_attribute(system_port_oid, 1, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to get local port oid of local system port spid:%" PRIx64, system_port_oid);
                    task_process_status handle_status = handleSaiGetStatus(SAI_API_SYSTEM_PORT, status);
                    if (handle_status != task_process_status::task_success)
                    {
                        continue;
                    }
                }

                //System port for local port. Update the system port info in the existing physical port
                if(!getPort(attr.value.oid, port))
                {
                    //This is system port for non-front panel local port (CPU or OLP or RCY (Inband)). Not an error
                    SWSS_LOG_NOTICE("Add port for non-front panel local system port 0x%" PRIx64 "; core: %d, core port: %d",
                            system_port_oid, core_index, core_port_index);
                }
                port.m_system_port_info.local_port_oid = attr.value.oid;
            }

            port.m_system_port_oid = system_port_oid;

            port.m_system_port_info.alias = alias;
            port.m_system_port_info.type = (sai_system_port_type_t) attrs[0].value.s32;
            port.m_system_port_info.port_id = attrs[1].value.sysportconfig.port_id;
            port.m_system_port_info.switch_id = attrs[1].value.sysportconfig.attached_switch_id;
            port.m_system_port_info.core_index = attrs[1].value.sysportconfig.attached_core_index;
            port.m_system_port_info.core_port_index = attrs[1].value.sysportconfig.attached_core_port_index;
            port.m_system_port_info.speed = attrs[1].value.sysportconfig.speed;
            port.m_system_port_info.num_voq = attrs[1].value.sysportconfig.num_voq;

            setPort(port.m_alias, port);
            if(m_port_ref_count.find(port.m_alias) == m_port_ref_count.end())
            {
                m_port_ref_count[port.m_alias] = 0;
            }

            SWSS_LOG_NOTICE("Added system port %" PRIx64 " for %s", system_port_oid, alias.c_str());
        }
        else
        {
            //System port does not exist in the switch
            //This can not happen since all the system ports are supposed to be created during switch creation itself

            SWSS_LOG_ERROR("System port %s does not exist in switch. Port not added!", alias.c_str());
            continue;
        }
    }

    return true;
}

bool PortsOrch::getInbandPort(Port &port)
{
    if (m_portList.find(m_inbandPortName) == m_portList.end())
    {
        return false;
    }
    else
    {
        port = m_portList[m_inbandPortName];
        return true;
    }
}

bool PortsOrch::isInbandPort(const string &alias)
{
    return (m_inbandPortName == alias);
}

bool PortsOrch::setVoqInbandIntf(string &alias, string &type)
{
    if(m_inbandPortName == alias)
    {
        //Inband interface already exists with this name
        SWSS_LOG_NOTICE("Interface %s is already configured as inband!", alias.c_str());
        return true;
    }

    //Make sure port and host if exists for the configured inband interface
    Port port;
    if (!getPort(alias, port))
    {
        SWSS_LOG_ERROR("Port/Vlan configured for inband intf %s is not ready!", alias.c_str());
        return false;
    }

    if(type == "port" && !port.m_hif_id)
    {
        SWSS_LOG_ERROR("Host interface is not available for port %s", alias.c_str());
        return false;
    }

    //Store the name of the local inband port
    m_inbandPortName = alias;

    return true;
}

void PortsOrch::voqSyncAddLag (Port &lag)
{
    int32_t switch_id = lag.m_system_lag_info.switch_id;

    // Sync only local lag add to CHASSIS_APP_DB

    if (switch_id != gVoqMySwitchId)
    {
        return;
    }

    uint32_t spa_id = lag.m_system_lag_info.spa_id;

    vector<FieldValueTuple> attrs;

    FieldValueTuple li ("lag_id", to_string(spa_id));
    attrs.push_back(li);

    FieldValueTuple si ("switch_id", to_string(switch_id));
    attrs.push_back(si);

    string key = lag.m_system_lag_info.alias;

    m_tableVoqSystemLagTable->set(key, attrs);
}

void PortsOrch::voqSyncDelLag(Port &lag)
{
    // Sync only local lag del to CHASSIS_APP_DB
    if (lag.m_system_lag_info.switch_id != gVoqMySwitchId)
    {
        return;
    }

    string key = lag.m_system_lag_info.alias;

    m_tableVoqSystemLagTable->del(key);
}

void PortsOrch::voqSyncAddLagMember(Port &lag, Port &port)
{
    // Sync only local lag's member add to CHASSIS_APP_DB
    if (lag.m_system_lag_info.switch_id != gVoqMySwitchId)
    {
        return;
    }

    vector<FieldValueTuple> attrs;
    FieldValueTuple nullFv ("NULL", "NULL");
    attrs.push_back(nullFv);

    string key = lag.m_system_lag_info.alias + ":" + port.m_system_port_info.alias;
    m_tableVoqSystemLagMemberTable->set(key, attrs);
}

void PortsOrch::voqSyncDelLagMember(Port &lag, Port &port)
{
    // Sync only local lag's member del to CHASSIS_APP_DB
    if (lag.m_system_lag_info.switch_id != gVoqMySwitchId)
    {
        return;
    }

    string key = lag.m_system_lag_info.alias + ":" + port.m_system_port_info.alias;
    m_tableVoqSystemLagMemberTable->del(key);
}

std::unordered_set<std::string> PortsOrch::generateCounterStats(const string& type)
{
    std::unordered_set<std::string> counter_stats;
    if (type == PORT_STAT_COUNTER_FLEX_COUNTER_GROUP)
    {
        for (const auto& it: port_stat_ids)
        {
            counter_stats.emplace(sai_serialize_port_stat(it));
        }
    }
    else if (type == PORT_BUFFER_DROP_STAT_FLEX_COUNTER_GROUP)
    {
        for (const auto& it: port_buffer_drop_stat_ids)
        {
            counter_stats.emplace(sai_serialize_port_stat(it));
        }
    }
    return counter_stats;
}

bool PortsOrch::decrFdbCount(const std::string& alias, int count)
{
    auto itr = m_portList.find(alias);
    if (itr == m_portList.end())
    {
        return false;
    }
    else
    {
        itr->second.m_fdb_count -= count;
    }
    return true;
}
