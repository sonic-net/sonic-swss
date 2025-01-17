#include "portsorch.h"
#include "intfsorch.h"
#include "bufferorch.h"
#include "neighorch.h"
#include "gearboxutils.h"
#include "vxlanorch.h"
#include "directory.h"
#include "subintf.h"
#include "notifications.h"

#include <inttypes.h>
#include <cassert>
#include <fstream>
#include <sstream>
#include <set>
#include <algorithm>
#include <tuple>
#include <sstream>
#include <unordered_set>

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
#include "switchorch.h"
#include "stringutility.h"
#include "subscriberstatetable.h"

#include "saitam.h"

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
extern sai_tam_api_t *sai_tam_api;
extern sai_l2mc_group_api_t *sai_l2mc_group_api;
extern sai_buffer_api_t *sai_buffer_api;
extern IntfsOrch *gIntfsOrch;
extern NeighOrch *gNeighOrch;
extern CrmOrch *gCrmOrch;
extern BufferOrch *gBufferOrch;
extern FdbOrch *gFdbOrch;
extern SwitchOrch *gSwitchOrch;
extern Directory<Orch*> gDirectory;
extern sai_system_port_api_t *sai_system_port_api;
extern string gMySwitchType;
extern int32_t gVoqMySwitchId;
extern string gMyHostName;
extern string gMyAsicName;
extern event_handle_t g_events_handle;

// defines ------------------------------------------------------------------------------------------------------------

#define DEFAULT_SYSTEM_PORT_MTU 9100
#define VLAN_PREFIX         "Vlan"
#define DEFAULT_VLAN_ID     1
#define MAX_VALID_VLAN_ID   4094
#define DEFAULT_HOSTIF_TX_QUEUE 7

#define PORT_SPEED_LIST_DEFAULT_SIZE                     16
#define PORT_STATE_POLLING_SEC                            5
#define PORT_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS     1000
#define PORT_BUFFER_DROP_STAT_POLLING_INTERVAL_MS     60000
#define QUEUE_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS   10000

// types --------------------------------------------------------------------------------------------------------------

struct PortAttrValue
{
    std::vector<std::uint32_t> lanes;
};

typedef PortAttrValue PortAttrValue_t;
typedef std::map<sai_port_serdes_attr_t, std::vector<std::uint32_t>> PortSerdesAttrMap_t;

// constants ----------------------------------------------------------------------------------------------------------

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

static map<sai_port_link_training_failure_status_t, string> link_training_failure_map =
{
    { SAI_PORT_LINK_TRAINING_FAILURE_STATUS_NO_ERROR, "none" },
    { SAI_PORT_LINK_TRAINING_FAILURE_STATUS_FRAME_LOCK_ERROR, "frame_lock"},
    { SAI_PORT_LINK_TRAINING_FAILURE_STATUS_SNR_LOWER_THRESHOLD, "snr_low"},
    { SAI_PORT_LINK_TRAINING_FAILURE_STATUS_TIME_OUT, "timeout"}
};

static map<sai_port_link_training_rx_status_t, string> link_training_rx_status_map =
{
    { SAI_PORT_LINK_TRAINING_RX_STATUS_NOT_TRAINED, "not_trained" },
    { SAI_PORT_LINK_TRAINING_RX_STATUS_TRAINED, "trained"}
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

// Timestamp Template map used for Path Tracing
static map<string, sai_port_path_tracing_timestamp_type_t> pt_timestamp_template_map =
{
 { "template1", SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_8_15 },
 { "template2", SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_12_19 },
 { "template3", SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_16_23 },
 { "template4", SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_20_27 }
};

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
    SAI_PORT_STAT_IF_IN_FEC_SYMBOL_ERRORS,
    SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S0,
    SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S1,
    SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S2,
    SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S3,
    SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S4,
    SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S5,
    SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S6,
    SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S7,
    SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S8,
    SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S9,
    SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S10,
    SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S11,
    SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S12,
    SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S13,
    SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S14,
    SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S15,
    SAI_PORT_STAT_IF_IN_FEC_CORRECTED_BITS
};

const vector<sai_port_stat_t> gbport_stat_ids =
{
    SAI_PORT_STAT_IF_IN_OCTETS,
    SAI_PORT_STAT_IF_IN_UCAST_PKTS,
    SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS,
    SAI_PORT_STAT_IF_OUT_OCTETS,
    SAI_PORT_STAT_IF_OUT_UCAST_PKTS,
    SAI_PORT_STAT_IF_OUT_NON_UCAST_PKTS,
    SAI_PORT_STAT_IF_IN_DISCARDS,
    SAI_PORT_STAT_IF_OUT_DISCARDS,
    SAI_PORT_STAT_IF_IN_ERRORS,
    SAI_PORT_STAT_IF_OUT_ERRORS,
    SAI_PORT_STAT_ETHER_RX_OVERSIZE_PKTS,
    SAI_PORT_STAT_ETHER_TX_OVERSIZE_PKTS,
    SAI_PORT_STAT_ETHER_STATS_UNDERSIZE_PKTS,
    SAI_PORT_STAT_ETHER_STATS_JABBERS,
    SAI_PORT_STAT_ETHER_STATS_FRAGMENTS,
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
static const vector<sai_queue_stat_t> voq_stat_ids =
{
    SAI_QUEUE_STAT_CREDIT_WD_DELETED_PACKETS
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

// functions ----------------------------------------------------------------------------------------------------------

static bool isValidPortTypeForLagMember(const Port& port)
{
    return (port.m_type == Port::Type::PHY || port.m_type == Port::Type::SYSTEM);
}

static void getPortSerdesAttr(PortSerdesAttrMap_t &map, const PortConfig &port)
{
    if (port.serdes.preemphasis.is_set)
    {
        map[SAI_PORT_SERDES_ATTR_PREEMPHASIS] = port.serdes.preemphasis.value;
    }

    if (port.serdes.idriver.is_set)
    {
        map[SAI_PORT_SERDES_ATTR_IDRIVER] = port.serdes.idriver.value;
    }

    if (port.serdes.ipredriver.is_set)
    {
        map[SAI_PORT_SERDES_ATTR_IPREDRIVER] = port.serdes.ipredriver.value;
    }

    if (port.serdes.pre1.is_set)
    {
        map[SAI_PORT_SERDES_ATTR_TX_FIR_PRE1] = port.serdes.pre1.value;
    }

    if (port.serdes.pre2.is_set)
    {
        map[SAI_PORT_SERDES_ATTR_TX_FIR_PRE2] = port.serdes.pre2.value;
    }

    if (port.serdes.pre3.is_set)
    {
        map[SAI_PORT_SERDES_ATTR_TX_FIR_PRE3] = port.serdes.pre3.value;
    }

    if (port.serdes.main.is_set)
    {
        map[SAI_PORT_SERDES_ATTR_TX_FIR_MAIN] = port.serdes.main.value;
    }

    if (port.serdes.post1.is_set)
    {
        map[SAI_PORT_SERDES_ATTR_TX_FIR_POST1] = port.serdes.post1.value;
    }

    if (port.serdes.post2.is_set)
    {
        map[SAI_PORT_SERDES_ATTR_TX_FIR_POST2] = port.serdes.post2.value;
    }

    if (port.serdes.post3.is_set)
    {
        map[SAI_PORT_SERDES_ATTR_TX_FIR_POST3] = port.serdes.post3.value;
    }

    if (port.serdes.attn.is_set)
    {
        map[SAI_PORT_SERDES_ATTR_TX_FIR_ATTN] = port.serdes.attn.value;
    }

    if (port.serdes.ob_m2lp.is_set)
    {

        map[SAI_PORT_SERDES_ATTR_TX_PAM4_RATIO] = port.serdes.ob_m2lp.value;
    }

    if (port.serdes.ob_alev_out.is_set)
    {
        map[SAI_PORT_SERDES_ATTR_TX_OUT_COMMON_MODE] = port.serdes.ob_alev_out.value;
    }

    if (port.serdes.obplev.is_set)
    {
        map[SAI_PORT_SERDES_ATTR_TX_PMOS_COMMON_MODE] = port.serdes.obplev.value;
    }

    if (port.serdes.obnlev.is_set)
    {
        map[SAI_PORT_SERDES_ATTR_TX_NMOS_COMMON_MODE] = port.serdes.obnlev.value;
    }

    if (port.serdes.regn_bfm1p.is_set)
    {
        map[SAI_PORT_SERDES_ATTR_TX_PMOS_VLTG_REG] = port.serdes.regn_bfm1p.value;
    }

    if (port.serdes.regn_bfm1n.is_set)
    {
        map[SAI_PORT_SERDES_ATTR_TX_NMOS_VLTG_REG] = port.serdes.regn_bfm1n.value;
    }


}

static bool isPathTracingSupported()
{
    /*
     * Path Tracing is supported when four conditions are met:
     *
     *   1. The switch supports SAI_OBJECT_TYPE_TAM
     *   2. SAI_OBJECT_TYPE_PORT supports SAI_PORT_ATTR_PATH_TRACING_INTF attribute
     *   3. SAI_OBJECT_TYPE_PORT supports SAI_PORT_ATTR_PATH_TRACING_TIMESTAMP_TYPE attribute
     *   4. SAI_OBJECT_TYPE_PORT supports SAI_PORT_ATTR_TAM_OBJECT attribute
     */

    /* First, query switch capabilities */
    sai_attribute_t attr;
    std::vector<sai_int32_t> switchCapabilities(SAI_OBJECT_TYPE_MAX);
    attr.id = SAI_SWITCH_ATTR_SUPPORTED_OBJECT_TYPE_LIST;
    attr.value.s32list.count = static_cast<uint32_t>(switchCapabilities.size());
    attr.value.s32list.list = switchCapabilities.data();

    bool is_tam_supported = false;
    auto status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status == SAI_STATUS_SUCCESS)
    {
        for (std::uint32_t i = 0; i < attr.value.s32list.count; i++)
        {
            switch(static_cast<sai_object_type_t>(attr.value.s32list.list[i]))
            {
                case SAI_OBJECT_TYPE_TAM:
                    is_tam_supported = true;
                    break;
                default:
                    /* Received an attribute in which we are not interested, ignoring it */
                    break;
            }
        }
    }
    else if (SAI_STATUS_IS_ATTR_NOT_SUPPORTED(status) || SAI_STATUS_IS_ATTR_NOT_IMPLEMENTED(status)
             || status ==  SAI_STATUS_NOT_SUPPORTED || status == SAI_STATUS_NOT_IMPLEMENTED)
    {
        SWSS_LOG_INFO("Querying OBJECT_TYPE_LIST is not supported on this platform");
        return false;
    }
    else 
    {
        SWSS_LOG_ERROR(
            "Failed to get a list of supported switch capabilities. Error=%d", status
        );
        return false;
    }

    /* Then verify if the four conditions are met */
    if (!is_tam_supported ||
            !gSwitchOrch->querySwitchCapability(SAI_OBJECT_TYPE_PORT, SAI_PORT_ATTR_PATH_TRACING_INTF) ||
            !gSwitchOrch->querySwitchCapability(SAI_OBJECT_TYPE_PORT, SAI_PORT_ATTR_PATH_TRACING_TIMESTAMP_TYPE) ||
            !gSwitchOrch->querySwitchCapability(SAI_OBJECT_TYPE_PORT, SAI_PORT_ATTR_TAM_OBJECT))
    {
        return false;
    }

    return true;
}

bool PortsOrch::checkPathTracingCapability()
{
    vector<FieldValueTuple> fvVector;
    if (isPathTracingSupported())
    {
        SWSS_LOG_INFO("Path Tracing is supported");
        /* Set PATH_TRACING_CAPABLE = true in STATE DB */
        fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_PATH_TRACING_CAPABLE, "true");
        m_isPathTracingSupported = true;
    }
    else
    {
        SWSS_LOG_INFO("Path Tracing is not supported");
        /* Set PATH_TRACING_CAPABLE = false in STATE DB */
        fvVector.emplace_back(SWITCH_CAPABILITY_TABLE_PATH_TRACING_CAPABLE, "false");
        m_isPathTracingSupported = false;
    }
    gSwitchOrch->set_switch_capability(fvVector);

    return m_isPathTracingSupported;
}

// Port OA ------------------------------------------------------------------------------------------------------------

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
        gb_port_stat_manager(true,
                PORT_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ,
                PORT_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, false),
        port_buffer_drop_stat_manager(PORT_BUFFER_DROP_STAT_FLEX_COUNTER_GROUP, StatsMode::READ, PORT_BUFFER_DROP_STAT_POLLING_INTERVAL_MS, false),
        queue_stat_manager(QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ, QUEUE_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, false),
        m_port_state_poller(new SelectableTimer(timespec { .tv_sec = PORT_STATE_POLLING_SEC, .tv_nsec = 0 }))
{
    SWSS_LOG_ENTER();

    /* Initialize counter table */
    m_counter_db = shared_ptr<DBConnector>(new DBConnector("COUNTERS_DB", 0));
    m_counterTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_PORT_NAME_MAP));
    m_counterSysPortTable = unique_ptr<Table>(
                    new Table(m_counter_db.get(), COUNTERS_SYSTEM_PORT_NAME_MAP));
    m_counterLagTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_LAG_NAME_MAP));
    FieldValueTuple tuple("", "");
    vector<FieldValueTuple> defaultLagFv;
    defaultLagFv.push_back(tuple);
    m_counterLagTable->set("", defaultLagFv);

    /* Initialize port and vlan table */
    m_portTable = unique_ptr<Table>(new Table(db, APP_PORT_TABLE_NAME));
    m_sendToIngressPortTable = unique_ptr<Table>(new Table(db, APP_SEND_TO_INGRESS_PORT_TABLE_NAME));

    /* Initialize gearbox */
    m_gearboxTable = unique_ptr<Table>(new Table(db, "_GEARBOX_TABLE"));

    /* Initialize queue tables */
    m_queueTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_QUEUE_NAME_MAP));
    m_voqTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_VOQ_NAME_MAP));
    m_queuePortTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_QUEUE_PORT_MAP));
    m_queueIndexTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_QUEUE_INDEX_MAP));
    m_queueTypeTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_QUEUE_TYPE_MAP));

    /* Initialize ingress priority group tables */
    m_pgTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_PG_NAME_MAP));
    m_pgPortTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_PG_PORT_MAP));
    m_pgIndexTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_PG_INDEX_MAP));

    m_state_db = shared_ptr<DBConnector>(new DBConnector("STATE_DB", 0));
    m_stateBufferMaximumValueTable = unique_ptr<Table>(new Table(m_state_db.get(), STATE_BUFFER_MAXIMUM_VALUE_TABLE));

    initGearbox();

    string queueWmSha, pgWmSha, portRateSha;
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
        portRateSha = swss::loadRedisScript(m_counter_db.get(), portRateLuaScript);
    }
    catch (const runtime_error &e)
    {
        SWSS_LOG_ERROR("Port flex counter groups were not set successfully: %s", e.what());
    }

    setFlexCounterGroupParameter(QUEUE_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP,
                                 QUEUE_WATERMARK_FLEX_STAT_COUNTER_POLL_MSECS,
                                 STATS_MODE_READ_AND_CLEAR,
                                 QUEUE_PLUGIN_FIELD,
                                 queueWmSha);

    setFlexCounterGroupParameter(PG_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP,
                                 PG_WATERMARK_FLEX_STAT_COUNTER_POLL_MSECS,
                                 STATS_MODE_READ_AND_CLEAR,
                                 PG_PLUGIN_FIELD,
                                 pgWmSha);

    setFlexCounterGroupParameter(PORT_STAT_COUNTER_FLEX_COUNTER_GROUP,
                                 PORT_RATE_FLEX_COUNTER_POLLING_INTERVAL_MS,
                                 STATS_MODE_READ,
                                 PORT_PLUGIN_FIELD,
                                 portRateSha);

    setFlexCounterGroupParameter(PG_DROP_STAT_COUNTER_FLEX_COUNTER_GROUP,
                                 PG_DROP_FLEX_STAT_COUNTER_POLL_MSECS,
                                 STATS_MODE_READ);

    /* Get CPU port */
    this->initializeCpuPort();

    /* Get ports */
    this->initializePorts();

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

    // Query whether SAI supports Host Tx Signal and Host Tx Notification

    sai_attr_capability_t capability;


    if (sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_PORT,
                                            SAI_PORT_ATTR_HOST_TX_SIGNAL_ENABLE,
                                            &capability) == SAI_STATUS_SUCCESS)
    {
        if (capability.create_implemented == true)
        {
            SWSS_LOG_DEBUG("SAI_PORT_ATTR_HOST_TX_SIGNAL_ENABLE is true");
            saiHwTxSignalSupported = true;
        }
    }

    if (sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_SWITCH,
                                            SAI_SWITCH_ATTR_PORT_HOST_TX_READY_NOTIFY,
                                            &capability) == SAI_STATUS_SUCCESS)
    {
        if (capability.create_implemented == true)
        {
            SWSS_LOG_DEBUG("SAI_SWITCH_ATTR_PORT_HOST_TX_READY_NOTIFY is true");
            saiTxReadyNotifySupported = true;
        }
    }

    if (saiHwTxSignalSupported && saiTxReadyNotifySupported)
    {
        SWSS_LOG_DEBUG("m_cmisModuleAsicSyncSupported is true");
        m_cmisModuleAsicSyncSupported = true;

        // set HOST_TX_READY callback function attribute to SAI, only if the feature is enabled
        sai_attribute_t attr;
        attr.id = SAI_SWITCH_ATTR_PORT_HOST_TX_READY_NOTIFY;
        attr.value.ptr = (void *)on_port_host_tx_ready;

        if (sai_switch_api->set_switch_attribute(gSwitchId, &attr) != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("PortsOrch failed to set SAI_SWITCH_ATTR_PORT_HOST_TX_READY_NOTIFY attribute");
        }

        Orch::addExecutor(new Consumer(new SubscriberStateTable(stateDb, STATE_TRANSCEIVER_INFO_TABLE_NAME, TableConsumable::DEFAULT_POP_BATCH_SIZE, 0), this, STATE_TRANSCEIVER_INFO_TABLE_NAME));
    }

    if (gMySwitchType != "dpu")
    {
        sai_attr_capability_t attr_cap;
        if (sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_PORT,
                                           SAI_PORT_ATTR_AUTO_NEG_FEC_MODE_OVERRIDE,
                                           &attr_cap) != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_NOTICE("Unable to query autoneg fec mode override");
        }
        else if (attr_cap.set_implemented && attr_cap.create_implemented)
        {
            fec_override_sup = true;
        }

        sai_attr_capability_t oper_fec_cap;
        if (sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_PORT,
                                           SAI_PORT_ATTR_OPER_PORT_FEC_MODE, &oper_fec_cap)
                                           != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_NOTICE("Unable to query capability support for oper fec mode");
        }
        else if (oper_fec_cap.get_implemented)
        {
            oper_fec_sup = true;
        }

        /* Get default 1Q bridge and default VLAN */
        sai_status_t status;
        sai_attribute_t attr;
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
    }

    /* Get System ports */
    getSystemPorts();

    if (gMySwitchType != "dpu")
    {
        removeDefaultVlanMembers();
        removeDefaultBridgePorts();
    }

    /* Add port oper status notification support */
    m_notificationsDb = make_shared<DBConnector>("ASIC_DB", 0);
    m_portStatusNotificationConsumer = new swss::NotificationConsumer(m_notificationsDb.get(), "NOTIFICATIONS");
    auto portStatusNotificatier = new Notifier(m_portStatusNotificationConsumer, this, "PORT_STATUS_NOTIFICATIONS");
    Orch::addExecutor(portStatusNotificatier);

    if (m_cmisModuleAsicSyncSupported)
    {
        m_portHostTxReadyNotificationConsumer = new swss::NotificationConsumer(m_notificationsDb.get(), "NOTIFICATIONS");
        auto portHostTxReadyNotificatier = new Notifier(m_portHostTxReadyNotificationConsumer, this, "PORT_HOST_TX_NOTIFICATIONS");
        Orch::addExecutor(portHostTxReadyNotificatier);
    }

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

    /* Query Path Tracing capability */
    checkPathTracingCapability();

    auto executor = new ExecutableTimer(m_port_state_poller, this, "PORT_STATE_POLLER");
    Orch::addExecutor(executor);
}

void PortsOrch::initializeCpuPort()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_SWITCH_ATTR_CPU_PORT;

    auto status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get CPU port, rv:%d", status);
        auto handle_status = handleSaiGetStatus(SAI_API_SWITCH, status);
        if (handle_status != task_process_status::task_success)
        {
            SWSS_LOG_THROW("PortsOrch initialization failure");
        }
    }

    this->m_cpuPort = Port("CPU", Port::CPU);
    this->m_cpuPort.m_port_id = attr.value.oid;
    this->m_portList[m_cpuPort.m_alias] = m_cpuPort;
    this->m_port_ref_count[m_cpuPort.m_alias] = 0;

    SWSS_LOG_NOTICE("Get CPU port pid:%" PRIx64, this->m_cpuPort.m_port_id);
}

void PortsOrch::initializePorts()
{
    SWSS_LOG_ENTER();

    sai_status_t status;
    sai_attribute_t attr;

    // Get port number
    attr.id = SAI_SWITCH_ATTR_PORT_NUMBER;

    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get port number, rv:%d", status);
        auto handle_status = handleSaiGetStatus(SAI_API_SWITCH, status);
        if (handle_status != task_process_status::task_success)
        {
            SWSS_LOG_THROW("PortsOrch initialization failure");
        }
    }

    this->m_portCount = attr.value.u32;

    SWSS_LOG_NOTICE("Get %d ports", this->m_portCount);

    // Get port list
    std::vector<sai_object_id_t> portList(this->m_portCount, SAI_NULL_OBJECT_ID);

    attr.id = SAI_SWITCH_ATTR_PORT_LIST;
    attr.value.objlist.count = static_cast<sai_uint32_t>(portList.size());
    attr.value.objlist.list = portList.data();

    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get port list, rv:%d", status);
        auto handle_status = handleSaiGetStatus(SAI_API_SWITCH, status);
        if (handle_status != task_process_status::task_success)
        {
            SWSS_LOG_THROW("PortsOrch initialization failure");
        }
    }

    // Get port hardware lane info
    for (const auto &portId : portList)
    {
        std::vector<sai_uint32_t> laneList(Port::max_lanes, 0);

        attr.id = SAI_PORT_ATTR_HW_LANE_LIST;
        attr.value.u32list.count = static_cast<sai_uint32_t>(laneList.size());
        attr.value.u32list.list = laneList.data();

        status = sai_port_api->get_port_attribute(portId, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to get hardware lane list pid:%" PRIx64, portId);
            auto handle_status = handleSaiGetStatus(SAI_API_PORT, status);
            if (handle_status != task_process_status::task_success)
            {
                SWSS_LOG_THROW("PortsOrch initialization failure");
            }
        }

        std::set<std::uint32_t> laneSet;
        for (sai_uint32_t i = 0; i < attr.value.u32list.count; i++)
        {
            laneSet.insert(attr.value.u32list.list[i]);
        }

        this->m_portListLaneMap[laneSet] = portId;

        SWSS_LOG_NOTICE(
            "Get port with lanes pid:%" PRIx64 " lanes:%s",
            portId, swss::join(" ", laneSet.cbegin(), laneSet.cend()).c_str()
        );
    }
}

auto PortsOrch::getPortConfigState() const -> port_config_state_t
{
    return this->m_portConfigState;
}

void PortsOrch::setPortConfigState(port_config_state_t value)
{
    this->m_portConfigState = value;
}

bool PortsOrch::addPortBulk(const std::vector<PortConfig> &portList)
{
    // The method is used to create ports in a bulk mode.
    // The action takes place when:
    // 1. Ports are being initialized at system start
    // 2. Ports are being added/removed by a user at runtime

    SWSS_LOG_ENTER();

    if (portList.empty())
    {
        return true;
    }

    std::vector<PortAttrValue_t> attrValueList;
    std::vector<std::vector<sai_attribute_t>> attrDataList;
    std::vector<std::uint32_t> attrCountList;
    std::vector<const sai_attribute_t*> attrPtrList;

    auto portCount = static_cast<std::uint32_t>(portList.size());
    std::vector<sai_object_id_t> oidList(portCount, SAI_NULL_OBJECT_ID);
    std::vector<sai_status_t> statusList(portCount, SAI_STATUS_SUCCESS);

    for (const auto &cit : portList)
    {
        sai_attribute_t attr;
        std::vector<sai_attribute_t> attrList;

        if (cit.lanes.is_set)
        {
            PortAttrValue_t attrValue;
            auto &outList = attrValue.lanes;
            auto &inList = cit.lanes.value;
            outList.insert(outList.begin(), inList.begin(), inList.end());
            attrValueList.push_back(attrValue);

            attr.id = SAI_PORT_ATTR_HW_LANE_LIST;
            attr.value.u32list.count = static_cast<std::uint32_t>(attrValueList.back().lanes.size());
            attr.value.u32list.list = attrValueList.back().lanes.data();
            attrList.push_back(attr);
        }

        if (cit.speed.is_set)
        {
            attr.id = SAI_PORT_ATTR_SPEED;
            attr.value.u32 = cit.speed.value;
            attrList.push_back(attr);
        }

        if (cit.autoneg.is_set)
        {
            attr.id = SAI_PORT_ATTR_AUTO_NEG_MODE;
            attr.value.booldata = cit.autoneg.value;
            attrList.push_back(attr);
        }

        if (cit.fec.is_set)
        {
            attr.id = SAI_PORT_ATTR_FEC_MODE;
            attr.value.s32 = cit.fec.value;
            attrList.push_back(attr);
        }

        if (m_cmisModuleAsicSyncSupported)
        {
            attr.id = SAI_PORT_ATTR_HOST_TX_SIGNAL_ENABLE;
            attr.value.booldata = false;
            attrList.push_back(attr);
        }

        if (cit.pt_intf_id.is_set)
        {
            if (!m_isPathTracingSupported)
            {
                SWSS_LOG_WARN(
                    "Failed to set Path Tracing Interface ID: Path Tracing is not supported by the switch"
                );
                continue;
            }

            /*
             * First, let's check the Path Tracing Interface ID configured for the port.
             *
             * Path Tracing Interface ID > 0 -> Path Tracing ENABLED on the port
             * Path Tracing Interface ID == 0 -> Path Tracing DISABLED on the port
             */
            if (cit.pt_intf_id.value != 0)
            {
                /* Path Tracing ENABLED case */

                /*
                 * The port does not have a TAM object assigned to it.
                 *
                 * Let's create a new TAM object (if we don't already have one)
                 * and assign it to the port.
                 */
                if (m_ptTam == SAI_NULL_OBJECT_ID)
                {
                    if (!createPtTam())
                    {
                        SWSS_LOG_ERROR(
                            "Failed to create TAM object for Path Tracing"
                        );
                    }
                }

                if (m_ptTam != SAI_NULL_OBJECT_ID)
                {
                    vector<sai_object_id_t> tam_objects_list;
                    tam_objects_list.push_back(m_ptTam);
                    attr.id = SAI_PORT_ATTR_TAM_OBJECT;
                    attr.value.objlist.count = (uint32_t)tam_objects_list.size();
                    attr.value.objlist.list = tam_objects_list.data();

                    m_ptTamRefCount++;
                    m_portPtTam[cit.key] = m_ptTam;
                }
            }

            attr.id = SAI_PORT_ATTR_PATH_TRACING_INTF;
            attr.value.u16 = cit.pt_intf_id.value;
            attrList.push_back(attr);
        }

        if (cit.pt_timestamp_template.is_set)
        {
            if (!m_isPathTracingSupported)
            {
                SWSS_LOG_WARN(
                    "Failed to set Path Tracing Timestamp Template: Path Tracing is not supported by the switch"
                );
                continue;
            }

            attr.id = SAI_PORT_ATTR_PATH_TRACING_TIMESTAMP_TYPE;
            attr.value.u16 = cit.pt_timestamp_template.value;
            attrList.push_back(attr);
        }

        attrDataList.push_back(attrList);
        attrCountList.push_back(static_cast<std::uint32_t>(attrDataList.back().size()));
        attrPtrList.push_back(attrDataList.back().data());
    }

    auto status = sai_port_api->create_ports(
        gSwitchId, portCount, attrCountList.data(), attrPtrList.data(),
        SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR,
        oidList.data(), statusList.data()
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create ports with bulk operation, rv:%d", status);

        auto handle_status = handleSaiCreateStatus(SAI_API_PORT, status);
        if (handle_status != task_process_status::task_success)
        {
            SWSS_LOG_THROW("PortsOrch bulk create failure");
        }

        return false;
    }

    for (std::uint32_t i = 0; i < portCount; i++)
    {
        if (statusList.at(i) != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR(
                "Failed to create port %s with bulk operation, rv:%d",
                portList.at(i).key.c_str(), statusList.at(i)
            );

            auto handle_status = handleSaiCreateStatus(SAI_API_PORT, statusList.at(i));
            if (handle_status != task_process_status::task_success)
            {
                SWSS_LOG_THROW("PortsOrch bulk create failure");
            }

            return false;
        }

        m_portListLaneMap[portList.at(i).lanes.value] = oidList.at(i);
        m_portCount++;
    }

    // newly created ports might be put in the default vlan so remove all ports from
    // the default vlan.
    if (gMySwitchType == "voq") {
        removeDefaultVlanMembers();
        removeDefaultBridgePorts();
    }

    SWSS_LOG_NOTICE("Created ports: %s", swss::join(',', oidList.begin(), oidList.end()).c_str());

    return true;
}

bool PortsOrch::removePortBulk(const std::vector<sai_object_id_t> &portList)
{
    SWSS_LOG_ENTER();

    if (portList.empty())
    {
        return true;
    }

    for (const auto &cit : portList)
    {
        Port p;

        // Make sure to bring down admin state
        if (getPort(cit, p))
        {
            setPortAdminStatus(p, false);
        }
        // else : port is in default state or not yet created

        // Remove port serdes (if exists) before removing port since this reference is dependency
        removePortSerdesAttribute(cit);

        /*
         * Decrease TAM object ref count before removing the port, if the port
         * has a TAM object assigned
         */
        if (m_portPtTam.find(p.m_alias) != m_portPtTam.end())
        {
            m_ptTamRefCount--;
            if (m_ptTamRefCount == 0)
            {
                if (!removePtTam(m_ptTam))
                {
                    throw runtime_error("Remove port TAM object for Path Tracing failed");
                }
            }
        }
    }

    auto portCount = static_cast<std::uint32_t>(portList.size());
    std::vector<sai_status_t> statusList(portCount, SAI_STATUS_SUCCESS);

    auto status = sai_port_api->remove_ports(
        portCount, portList.data(),
        SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR,
        statusList.data()
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove ports with bulk operation, rv:%d", status);

        auto handle_status = handleSaiRemoveStatus(SAI_API_PORT, status);
        if (handle_status != task_process_status::task_success)
        {
            SWSS_LOG_THROW("PortsOrch bulk remove failure");
        }

        return false;
    }

    for (std::uint32_t i = 0; i < portCount; i++)
    {
        if (statusList.at(i) != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR(
                "Failed to remove port %" PRIx64 " with bulk operation, rv:%d",
                portList.at(i), statusList.at(i)
            );

            auto handle_status = handleSaiRemoveStatus(SAI_API_PORT, statusList.at(i));
            if (handle_status != task_process_status::task_success)
            {
                SWSS_LOG_THROW("PortsOrch bulk remove failure");
            }

            return false;
        }

        m_portSupportedSpeeds.erase(portList.at(i));
        m_portCount--;
    }

    SWSS_LOG_NOTICE("Removed ports: %s", swss::join(',', portList.begin(), portList.end()).c_str());

    return true;
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

bool PortsOrch::isGearboxEnabled()
{
    return m_gearboxEnabled;
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

unordered_set<string>& PortsOrch::getAllVlans()
{
    return m_vlanPorts;
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
        if (!getPort(itr->second, port))
        {
            SWSS_LOG_THROW("Inconsistent saiOidToAlias map and m_portList map: oid=%" PRIx64, id);
        }
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

/*
 * Create host_tx_ready field in PORT_TABLE of STATE-DB
 * and set the field to false by default for the
 * front<Ethernet> port.
 */
void PortsOrch::initHostTxReadyState(Port &port)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> tuples;
    bool exist = m_portStateTable.get(port.m_alias, tuples);
    string hostTxReady;

    if (exist)
    {
        for (auto i : tuples)
        {
            if (fvField(i) == "host_tx_ready")
            {
                hostTxReady = fvValue(i);
            }
        }
    }

    if (hostTxReady.empty())
    {
        setHostTxReady(port, "false");
        SWSS_LOG_NOTICE("initialize host_tx_ready as false for port %s",
                        port.m_alias.c_str());
    }
}

bool PortsOrch::setPortAdminStatus(Port &port, bool state)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_ADMIN_STATE;
    attr.value.booldata = state;

    // if sync between cmis module configuration and asic is supported,
    // do not change host_tx_ready value in STATE DB when admin status is changed.

    /* Update the host_tx_ready to false before setting admin_state, when admin state is false */
    if (!state && !m_cmisModuleAsicSyncSupported)
    {
        setHostTxReady(port, "false");
        SWSS_LOG_NOTICE("Set admin status DOWN host_tx_ready to false for port %s",
                port.m_alias.c_str());
    }

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set admin status %s for port %s."
                       " Setting host_tx_ready as false",
                       state ? "UP" : "DOWN", port.m_alias.c_str());

        if (!m_cmisModuleAsicSyncSupported)
        {
            setHostTxReady(port, "false");
        }
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    bool gbstatus = setGearboxPortsAttr(port, SAI_PORT_ATTR_ADMIN_STATE, &state);
    if (gbstatus != true && !m_cmisModuleAsicSyncSupported)
    {
        setHostTxReady(port, "false");
        SWSS_LOG_NOTICE("Set host_tx_ready to false as gbstatus is false "
                        "for port %s", port.m_alias.c_str());
    }

    /* Update the state table for host_tx_ready*/
    if (state && (gbstatus == true) && (status == SAI_STATUS_SUCCESS) && !m_cmisModuleAsicSyncSupported)
    {
        setHostTxReady(port, "true");
        SWSS_LOG_NOTICE("Set admin status UP host_tx_ready to true for port %s",
                port.m_alias.c_str());
    }

    return true;
}

void PortsOrch::setHostTxReady(Port port, const std::string &status)
{
    SWSS_LOG_NOTICE("Setting host_tx_ready status = %s, alias = %s, port_id = 0x%" PRIx64, status.c_str(), port.m_alias.c_str(), port.m_port_id);
    m_portStateTable.hset(port.m_alias, "host_tx_ready", status);
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

bool PortsOrch::getPortHostTxReady(const Port& port, bool &hostTxReadyVal)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_HOST_TX_READY_STATUS;

    sai_status_t status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        hostTxReadyVal = false;
        return false;
    }

    hostTxReadyVal = attr.value.s32;

    return true;
}

bool PortsOrch::getPortMtu(const Port& port, sai_uint32_t &mtu)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_MTU;

    sai_status_t status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        return false;
    }

    mtu = attr.value.u32 - (uint32_t)(sizeof(struct ether_header) + FCS_LEN + VLAN_TAG_LEN);

    /* Reduce the default MTU got from ASIC by MAX_MACSEC_SECTAG_SIZE */
    if (mtu > MAX_MACSEC_SECTAG_SIZE)
    {
        mtu -= MAX_MACSEC_SECTAG_SIZE;
    }

    return true;
}

bool PortsOrch::setPortMtu(const Port& port, sai_uint32_t mtu)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_MTU;
    /* mtu + 14 + 4 + 4 = 22 bytes */
    mtu += (uint32_t)(sizeof(struct ether_header) + FCS_LEN + VLAN_TAG_LEN);
    attr.value.u32 = mtu;

    if (isMACsecPort(port.m_port_id))
    {
        attr.value.u32 += MAX_MACSEC_SECTAG_SIZE;
    }

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set MTU %u to port pid:%" PRIx64 ", rv:%d",
                attr.value.u32, port.m_port_id, status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    if (m_gearboxEnabled)
    {
        setGearboxPortsAttr(port, SAI_PORT_ATTR_MTU, &mtu);
    }
    SWSS_LOG_INFO("Set MTU %u to port pid:%" PRIx64, attr.value.u32, port.m_port_id);
    return true;
}


bool PortsOrch::setPortTpid(Port &port, sai_uint16_t tpid)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_TPID;
    attr.value.u16 = tpid;

    auto status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set TPID 0x%x to port %s, rv:%d",
                attr.value.u16, port.m_alias.c_str(), status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("Set TPID 0x%x to port %s", attr.value.u16, port.m_alias.c_str());

    return true;
}

bool PortsOrch::setPortFecOverride(sai_object_id_t port_obj, bool override_fec)
{
    sai_attribute_t attr;
    sai_status_t status;

    attr.id = SAI_PORT_ATTR_AUTO_NEG_FEC_MODE_OVERRIDE;
    attr.value.booldata = override_fec;

    status = sai_port_api->set_port_attribute(port_obj, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set fec override %d to port pid:%" PRIx64, attr.value.booldata, port_obj);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    SWSS_LOG_INFO("Set fec override %d to port pid:%" PRIx64, attr.value.booldata, port_obj);
    return true;
}

bool PortsOrch::setPortFec(Port &port, sai_port_fec_mode_t fec_mode, bool override_fec)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_FEC_MODE;
    attr.value.s32 = fec_mode;

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set FEC mode %d to port %s", fec_mode, port.m_alias.c_str());
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    if (fec_override_sup && !setPortFecOverride(port.m_port_id, override_fec))
    {
        return false;
    }
    setGearboxPortsAttr(port, SAI_PORT_ATTR_FEC_MODE, &fec_mode, override_fec);

    SWSS_LOG_NOTICE("Set port %s FEC mode %d", port.m_alias.c_str(), fec_mode);

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

bool PortsOrch::setPortPfcAsym(Port &port, sai_port_priority_flow_control_mode_t pfc_asym)
{
    SWSS_LOG_ENTER();

    uint8_t pfc = 0;
    if (!getPortPfc(port.m_port_id, &pfc))
    {
        return false;
    }

    port.m_pfc_asym = pfc_asym;
    m_portList[port.m_alias] = port;

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL_MODE;
    attr.value.s32 = pfc_asym;

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set PFC mode %d to port id 0x%" PRIx64 " (rc:%d)", pfc_asym, port.m_port_id, status);
        if (status == SAI_STATUS_NOT_SUPPORTED)
        {
            return true;
        }
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

    if (pfc_asym == SAI_PORT_PRIORITY_FLOW_CONTROL_MODE_SEPARATE)
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

    SWSS_LOG_INFO("Set asymmetric PFC %d to port id 0x%" PRIx64, pfc_asym, port.m_port_id);

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
    member_attr.value.u32 = 100;
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

    for (const auto &p: portv)
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


void PortsOrch::initPortCapAutoNeg(Port &port)
{
    sai_status_t status;
    sai_attribute_t attr;

    attr.id = SAI_PORT_ATTR_SUPPORTED_AUTO_NEG_MODE;
    status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (status == SAI_STATUS_SUCCESS)
    {
        port.m_cap_an = attr.value.booldata ? 1 : 0;
    }
    else
    {
        // To avoid breakage on the existing platforms, AN should be 1 by default
        port.m_cap_an = 1;
        SWSS_LOG_WARN("Unable to get %s AN support capability",
                      port.m_alias.c_str());
    }
}

void PortsOrch::initPortCapLinkTraining(Port &port)
{
    // TODO:
    // Add SAI_PORT_ATTR_SUPPORTED_LINK_TRAINING_MODE query when it is
    // available in the saiport.h of SAI.
    port.m_cap_lt = 1;
    SWSS_LOG_WARN("Unable to get %s LT support capability", port.m_alias.c_str());
}

bool PortsOrch::isFecModeSupported(const Port &port, sai_port_fec_mode_t fec_mode)
{
    initPortSupportedFecModes(port.m_alias, port.m_port_id);

    const auto &obj = m_portSupportedFecModes.at(port.m_port_id);

    if (!obj.supported)
    {
        return true;
    }

    if (obj.data.empty())
    {
        return false;
    }

    return std::find(obj.data.cbegin(), obj.data.cend(), fec_mode) != obj.data.cend();
}

sai_status_t PortsOrch::getPortSupportedFecModes(PortSupportedFecModes &supported_fecmodes, sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    std::vector<sai_int32_t> fecModes(Port::max_fec_modes);
    attr.id = SAI_PORT_ATTR_SUPPORTED_FEC_MODE;
    attr.value.s32list.count = static_cast<uint32_t>(fecModes.size());
    attr.value.s32list.list = fecModes.data();

    auto status = sai_port_api->get_port_attribute(port_id, 1, &attr);
    if (status == SAI_STATUS_SUCCESS)
    {
        for (std::uint32_t i = 0; i < attr.value.s32list.count; i++)
        {
            supported_fecmodes.insert(static_cast<sai_port_fec_mode_t>(attr.value.s32list.list[i]));
        }
    }
    else
    {
        if (SAI_STATUS_IS_ATTR_NOT_SUPPORTED(status) ||
            SAI_STATUS_IS_ATTR_NOT_IMPLEMENTED(status) ||
            (status == SAI_STATUS_NOT_SUPPORTED) ||
            (status == SAI_STATUS_NOT_IMPLEMENTED))
        {
            // unable to validate FEC mode if attribute is not supported on platform
            SWSS_LOG_NOTICE(
                "Unable to validate FEC mode for port id=%" PRIx64 " due to unsupported by platform", port_id
            );
        }
        else
        {
            SWSS_LOG_ERROR(
                "Failed to get a list of supported FEC modes for port id=%" PRIx64 ". Error=%d", port_id, status
            );
        }
    }

    return status;
}

void PortsOrch::initPortSupportedFecModes(const std::string& alias, sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    // If port supported speeds map already contains the information, save the SAI call
    if (m_portSupportedFecModes.count(port_id) > 0)
    {
        return;
    }

    auto &obj = m_portSupportedFecModes[port_id];
    auto &supported_fec_modes = obj.data;

    auto status = getPortSupportedFecModes(supported_fec_modes, port_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        // Do not expose "supported_fecs" in case fetching FEC modes is not supported by the vendor
        SWSS_LOG_INFO("No supported_fecs exposed to STATE_DB for port %s since fetching supported FEC modes is not supported by the vendor",
                      alias.c_str());
        return;
    }

    obj.supported = true;

    std::vector<std::string> fecModeList;
    if (supported_fec_modes.empty())
    {
        fecModeList.push_back("N/A");
    }
    else
    {
        for (const auto &cit : supported_fec_modes)
        {
            std::string fecMode;
            if (!m_portHlpr.fecToStr(fecMode, cit))
            {
                SWSS_LOG_ERROR(
                    "Failed to convert FEC mode for port %s: unknown value %d",
                    alias.c_str(), static_cast<std::int32_t>(cit)
                );
                continue;
            }

            fecModeList.push_back(fecMode);
        }
        if (!fecModeList.empty() && fec_override_sup)
        {
            fecModeList.push_back(PORT_FEC_AUTO);
        }
    }

    std::vector<FieldValueTuple> v;
    std::string supported_fec_modes_str = swss::join(',', fecModeList.begin(), fecModeList.end());
    v.emplace_back(std::make_pair("supported_fecs", supported_fec_modes_str));

    m_portStateTable.set(alias, v);
}

/*
 * If Gearbox is enabled and this is a Gearbox port then set the attributes accordingly.
 */
bool PortsOrch::setGearboxPortsAttr(const Port &port, sai_port_attr_t id, void *value, bool override_fec)
{
    bool status = false;

    status = setGearboxPortAttr(port, PHY_PORT_TYPE, id, value, override_fec);

    if (status == true)
    {
        status = setGearboxPortAttr(port, LINE_PORT_TYPE, id, value, override_fec);
    }

    return status;
}

/*
 * If Gearbox is enabled and this is a Gearbox port then set the specific lane attribute.
 * Note: the appl_db is also updated (Gearbox config_db tables are TBA).
 */
bool PortsOrch::setGearboxPortAttr(const Port &port, dest_port_type_t port_type, sai_port_attr_t id, void *value, bool override_fec)
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
                case SAI_PORT_ATTR_MTU:
                    attr.id = id;
                    attr.value.u32 = *static_cast<sai_uint32_t*>(value);
                    if (LINE_PORT_TYPE == port_type && isMACsecPort(dest_port_id))
                    {
                        attr.value.u32 += MAX_MACSEC_SECTAG_SIZE;
                    }
                    SWSS_LOG_NOTICE("BOX: Set %s MTU %d", port.m_alias.c_str(), attr.value.u32);
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
                else if (id == SAI_PORT_ATTR_FEC_MODE && fec_override_sup && !setPortFecOverride(dest_port_id, override_fec))
                {
                    return false;
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

bool PortsOrch::getPortAdvSpeeds(const Port& port, bool remote, std::vector<sai_uint32_t>& speed_list)
{
    sai_object_id_t port_id = port.m_port_id;
    sai_object_id_t line_port_id;
    sai_attribute_t attr;
    sai_status_t status;
    std::vector<sai_uint32_t> speeds(PORT_SPEED_LIST_DEFAULT_SIZE);

    attr.id = remote ? SAI_PORT_ATTR_REMOTE_ADVERTISED_SPEED : SAI_PORT_ATTR_ADVERTISED_SPEED;
    attr.value.u32list.count = static_cast<uint32_t>(speeds.size());
    attr.value.u32list.list = speeds.data();

    if (getDestPortId(port_id, LINE_PORT_TYPE, line_port_id))
    {
        status = sai_port_api->get_port_attribute(line_port_id, 1, &attr);
    }
    else
    {
        status = sai_port_api->get_port_attribute(port_id, 1, &attr);
    }
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Unable to get advertised speed for %s", port.m_alias.c_str());
        return false;
    }
    speeds.resize(attr.value.u32list.count);
    speed_list.swap(speeds);
    return true;
}

bool PortsOrch::getPortAdvSpeeds(const Port& port, bool remote, string& adv_speeds)
{
    std::vector<sai_uint32_t> speed_list;
    bool rc = getPortAdvSpeeds(port, remote, speed_list);

    adv_speeds = rc ? swss::join(',', speed_list.begin(), speed_list.end()) : "";
    return rc;
}

task_process_status PortsOrch::setPortAdvSpeeds(Port &port, std::set<sai_uint32_t> &speed_list)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    std::vector<std::uint32_t> speedList(speed_list.begin(), speed_list.end());
    attr.id = SAI_PORT_ATTR_ADVERTISED_SPEED;
    attr.value.u32list.list  = speedList.data();
    attr.value.u32list.count = static_cast<std::uint32_t>(speedList.size());

    auto status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        return handleSaiSetStatus(SAI_API_PORT, status);
    }

    return task_success;
}

task_process_status PortsOrch::setPortInterfaceType(Port &port, sai_port_interface_type_t interface_type)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_INTERFACE_TYPE;
    attr.value.s32 = interface_type;

    auto status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        return handleSaiSetStatus(SAI_API_PORT, status);
    }

    return task_success;
}

task_process_status PortsOrch::setPortAdvInterfaceTypes(Port &port, std::set<sai_port_interface_type_t> &interface_types)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    std::vector<std::int32_t> interfaceTypeList(interface_types.begin(), interface_types.end());
    attr.id = SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE;
    attr.value.s32list.list  = interfaceTypeList.data();
    attr.value.s32list.count = static_cast<std::uint32_t>(interfaceTypeList.size());

    auto status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        return handleSaiSetStatus(SAI_API_PORT, status);
    }

    return task_success;
}

bool PortsOrch::getQueueTypeAndIndex(sai_object_id_t queue_id, string &type, uint8_t &index)
{
    SWSS_LOG_ENTER();

    auto const &queueInfoRef = m_queueInfo.find(queue_id);

    sai_attribute_t attr[2];
    if (queueInfoRef == m_queueInfo.end())
    {
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

        SWSS_LOG_INFO("Caching information (index %d type %d) for queue %" PRIx64, attr[1].value.u8, attr[0].value.s32, queue_id);

        m_queueInfo[queue_id].type = static_cast<sai_queue_type_t>(attr[0].value.s32);
        m_queueInfo[queue_id].index = attr[1].value.u8;
    }
    else
    {
        attr[0].value.s32 = m_queueInfo[queue_id].type;
        attr[1].value.u8 = m_queueInfo[queue_id].index;

        SWSS_LOG_INFO("Fetched cached information (index %d type %d) for queue %" PRIx64, attr[1].value.u8, attr[0].value.s32, queue_id);
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
    case SAI_QUEUE_TYPE_UNICAST_VOQ:
        type = "SAI_QUEUE_TYPE_UNICAST_VOQ";
        break;
    default:
        SWSS_LOG_ERROR("Got unsupported queue type %d for %" PRIx64 " queue", attr[0].value.s32, queue_id);
        throw runtime_error("Got unsupported queue type");
    }

    index = attr[1].value.u8;

    return true;
}

bool PortsOrch::isAutoNegEnabled(sai_object_id_t id)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_AUTO_NEG_MODE;

    sai_status_t status = sai_port_api->get_port_attribute(id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get port AutoNeg status for port pid:%" PRIx64, id);
        return false;
    }

    return attr.value.booldata;
}

task_process_status PortsOrch::setPortAutoNeg(Port &port, bool autoneg)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_AUTO_NEG_MODE;
    attr.value.booldata = autoneg;

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set AutoNeg %u to port %s", attr.value.booldata, port.m_alias.c_str());
        return handleSaiSetStatus(SAI_API_PORT, status);
    }
    SWSS_LOG_INFO("Set AutoNeg %u to port %s", attr.value.booldata, port.m_alias.c_str());
    return task_success;
}

task_process_status PortsOrch::setPortLinkTraining(const Port &port, bool state)
{
    SWSS_LOG_ENTER();

    if (port.m_type != Port::PHY)
    {
        return task_failed;
    }

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_LINK_TRAINING_ENABLE;
    attr.value.booldata = state;

    string op = state ? "on" : "off";
    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set LT %s to port %s", op.c_str(), port.m_alias.c_str());
        return handleSaiSetStatus(SAI_API_PORT, status);
    }

    SWSS_LOG_INFO("Set LT %s to port %s", op.c_str(), port.m_alias.c_str());

    return task_success;
}

ReturnCode PortsOrch::setPortLinkEventDampingAlgorithm(Port &port,
                                                       sai_redis_link_event_damping_algorithm_t &link_event_damping_algorithm)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    attr.id = SAI_REDIS_PORT_ATTR_LINK_EVENT_DAMPING_ALGORITHM;
    attr.value.s32 = link_event_damping_algorithm;

    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_port_api->set_port_attribute(port.m_port_id, &attr),
        "Failed to set link event damping algorithm (" << link_event_damping_algorithm << ") for port "
                                                       << port.m_alias);

    SWSS_LOG_INFO("Set link event damping algorithm %u for port %s", link_event_damping_algorithm, port.m_alias.c_str());
    return ReturnCode();
}

ReturnCode PortsOrch::setPortLinkEventDampingAiedConfig(Port &port,
                                                        sai_redis_link_event_damping_algo_aied_config_t &config) {

    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    attr.id = SAI_REDIS_PORT_ATTR_LINK_EVENT_DAMPING_ALGO_AIED_CONFIG;
    attr.value.ptr = (void *) &config;

    std::stringstream msg;
    msg << "link event damping algorithm aied config for port " << port.m_alias << " - ";
    msg << "max_suppress_time: " << config.max_suppress_time << ", ";
    msg << "decay_half_life: " << config.decay_half_life << ", ";
    msg << "suppress_threshold: " << config.suppress_threshold << ", ";
    msg << "reuse_threshold: " << config.reuse_threshold << ", ";
    msg << "flap_penalty: " << config.flap_penalty;

    std::string msg_str = msg.str();

    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_port_api->set_port_attribute(port.m_port_id, &attr), "Failed to set " + msg_str);

    SWSS_LOG_INFO("Set %s", msg_str.c_str());

    return ReturnCode();
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

    event_params_t params = {{"ifname",port.m_alias},{"status",isUp ? "up" : "down"}};
    event_publish(g_events_handle, "if-state", &params);
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

    bool set_hostif_tx_queue = false;
    if (gSwitchOrch->querySwitchCapability(SAI_OBJECT_TYPE_HOSTIF, SAI_HOSTIF_ATTR_QUEUE))
    {
        set_hostif_tx_queue = true;
    }
    else
    {
        SWSS_LOG_WARN("Hostif queue attribute not supported");
    }

    if (set_hostif_tx_queue)
    {
        attr.id = SAI_HOSTIF_ATTR_QUEUE;
        attr.value.u32 = DEFAULT_HOSTIF_TX_QUEUE;
        attrs.push_back(attr);
    }

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

void PortsOrch::updateDbPortFlapCount(Port& port, sai_port_oper_status_t pstatus)
{
    SWSS_LOG_ENTER();

    ++port.m_flap_count;
    vector<FieldValueTuple> tuples;
    FieldValueTuple tuple("flap_count", std::to_string(port.m_flap_count));
    tuples.push_back(tuple);

    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    if (pstatus == SAI_PORT_OPER_STATUS_DOWN)
    {
        char buffer[32];
        // Format: Www Mmm dd hh:mm:ss yyyy
        std::strftime(buffer, sizeof(buffer), "%a %b %d %H:%M:%S %Y", std::gmtime(&now_c));
        FieldValueTuple tuple("last_down_time", buffer);
        tuples.push_back(tuple);
    }
    else if (pstatus == SAI_PORT_OPER_STATUS_UP)
    {
        char buffer[32];
        // Format: Www Mmm dd hh:mm:ss yyyy
        std::strftime(buffer, sizeof(buffer), "%a %b %d %H:%M:%S %Y", std::gmtime(&now_c));
        FieldValueTuple tuple("last_up_time", buffer);
        tuples.push_back(tuple);
    }
    m_portTable->set(port.m_alias, tuples);
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
        /* Bring port down before removing port */
        if (!setPortAdminStatus(port, false))
        {
            SWSS_LOG_ERROR("Failed to set admin status to DOWN to remove port %" PRIx64, port_id);
        }
    }
    /* else : port is in default state or not yet created */

    /* Remove port counters */
    port_stat_manager.clearCounterIdList(port.m_port_id);
    port_buffer_drop_stat_manager.clearCounterIdList(port.m_port_id);

    /*
     * Remove port serdes (if exists) before removing port since this
     * reference is dependency.
     */

    removePortSerdesAttribute(port_id);

    for (auto queue_id : port.m_queue_ids)
    {
        SWSS_LOG_INFO("Removing cached information for queue %" PRIx64, queue_id);
        m_queueInfo.erase(queue_id);
    }

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

bool PortsOrch::initPort(const PortConfig &port)
{
    SWSS_LOG_ENTER();

    const auto &alias = port.key;
    const auto &role = port.role.value;
    const auto &index = port.index.value;
    const auto &lane_set = port.lanes.value;

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
                    port_stat_manager.setCounterIdList(p.m_port_id,
                            CounterType::PORT, port_counter_stats);
                    auto gbport_counter_stats = generateCounterStats(PORT_STAT_COUNTER_FLEX_COUNTER_GROUP, true);
                    if (p.m_system_side_id)
                        gb_port_stat_manager.setCounterIdList(p.m_system_side_id,
                                CounterType::PORT, gbport_counter_stats, p.m_switch_id);
                    if (p.m_line_side_id)
                        gb_port_stat_manager.setCounterIdList(p.m_line_side_id,
                                CounterType::PORT, gbport_counter_stats, p.m_switch_id);
                }
                if (flex_counters_orch->getPortBufferDropCountersState())
                {
                    auto port_buffer_drop_stats = generateCounterStats(PORT_BUFFER_DROP_STAT_FLEX_COUNTER_GROUP);
                    port_buffer_drop_stat_manager.setCounterIdList(p.m_port_id, CounterType::PORT, port_buffer_drop_stats);
                }

                PortUpdate update = { p, true };
                notify(SUBJECT_TYPE_PORT_CHANGE, static_cast<void *>(&update));

                m_portList[alias].m_init = true;

                if (role == Port::Role::Rec || role == Port::Role::Inb)
                {
                    m_recircPortRole[alias] = role;
                }

                // We have to test the size of m_queue_ids here since it isn't initialized on some platforms (like DPU)
                if (p.m_host_tx_queue_configured && p.m_queue_ids.size() > p.m_host_tx_queue)
                {
                    createPortBufferQueueCounters(p, to_string(p.m_host_tx_queue), false);
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

    if (p.m_host_tx_queue_configured && p.m_queue_ids.size() > p.m_host_tx_queue)
    {
        removePortBufferQueueCounters(p, to_string(p.m_host_tx_queue), false);
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

    /* remove port name map from counter table */
    m_counterTable->hdel("", alias);

    /* Remove the associated port serdes attribute */
    removePortSerdesAttribute(p.m_port_id);

    /* Remove the entry from buffer maximum parameter table*/
    m_stateBufferMaximumValueTable->del(alias);

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
    if (saiHwTxSignalSupported && saiTxReadyNotifySupported)
    {
        addExistingData(STATE_TRANSCEIVER_INFO_TABLE_NAME);
    }
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
        if (it->second.key == alias)
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

void PortsOrch::doSendToIngressPortTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto &t = it->second;

        string alias = kfvKey(t);
        string op = kfvOp(t);
        ReturnCode rc;
        std::vector<FieldValueTuple> app_state_db_attrs;

        if (op == SET_COMMAND)
        {
            if (m_isSendToIngressPortConfigured)
            {
                rc = ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
                    << "Update operation on SendToIngress port with alias="
                    << alias << " is not suported";
                SWSS_LOG_ERROR("%s", rc.message().c_str());
                m_publisher.publish(consumer.getTableName(), kfvKey(t),
                                kfvFieldsValues(t), rc);
                it = consumer.m_toSync.erase(it);
                continue;
            }
            rc = addSendToIngressHostIf(alias);
            if (!rc.ok())
            {
                SWSS_LOG_ERROR("%s", rc.message().c_str());
            }
            else
            {
                m_isSendToIngressPortConfigured = true;
            }
        }
        else if (op == DEL_COMMAND)
        {
            // For SendToIngress port, delete the host interface and unbind from the CPU port
            rc = removeSendToIngressHostIf();
            if (!rc.ok())
            {
                SWSS_LOG_ERROR("Failed to remove SendToIngress port rc=%s",
                    rc.message().c_str());
            }
            else
            {
                m_isSendToIngressPortConfigured = false;
            }
        }
        else
        {
            rc = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) <<
                            "Unknown operation type " << op;
            SWSS_LOG_ERROR("%s", rc.message().c_str());
        }
        m_publisher.publish(consumer.getTableName(), kfvKey(t),
                            kfvFieldsValues(t), rc);
        it = consumer.m_toSync.erase(it);
    }
}

void PortsOrch::doPortTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto &taskMap = consumer.m_toSync;
    auto it = taskMap.begin();

    while (it != taskMap.end())
    {
        auto keyOpFieldsValues = it->second;
        auto key = kfvKey(keyOpFieldsValues);
        auto op = kfvOp(keyOpFieldsValues);

        SWSS_LOG_INFO("KEY: %s, OP: %s", key.c_str(), op.c_str());

        if (key.empty())
        {
            SWSS_LOG_ERROR("Failed to parse port key: empty string");
            it = taskMap.erase(it);
            continue;
        }

        /* Got notification from portsyncd application:
         *
         * When portsorch receives 'PortConfigDone' message, it indicates port configuration
         * procedure is done. Port configuration assumes all data has been read from config db
         * and pushed to application db.
         *
         * Before port configuration procedure, none of other tasks are executed.
         */
        if (key == "PortConfigDone")
        {
            it = taskMap.erase(it);

            /* portsyncd restarting case:
             * When portsyncd restarts, duplicate notifications may be received.
             */
            if (getPortConfigState() != PORT_CONFIG_MISSING)
            {
                // Already received, ignore this task
                continue;
            }

            setPortConfigState(PORT_CONFIG_RECEIVED);

            SWSS_LOG_INFO("Got PortConfigDone notification from portsyncd");

            it = taskMap.begin();
            continue;
        }

        /* Got notification from portsyncd application:
         *
         * When portsorch receives 'PortInitDone' message, it indicates port initialization
         * procedure is done. Port initialization assumes all netdevs have been created.
         *
         * Before port initialization procedure, none of other tasks are executed.
         */
        if (key == "PortInitDone")
        {
            /* portsyncd restarting case:
             * When portsyncd restarts, duplicate notifications may be received.
             */
            if (!m_initDone)
            {
                addSystemPorts();
                m_initDone = true;
                SWSS_LOG_INFO("Got PortInitDone notification from portsyncd");
            }

            it = taskMap.erase(it);
            continue;
        }

        PortConfig pCfg(key, op);

        if (op == SET_COMMAND)
        {
            auto parsePortFvs = [&](auto& fvMap) -> bool
            {
                for (const auto &cit : kfvFieldsValues(keyOpFieldsValues))
                {
                    auto fieldName = fvField(cit);
                    auto fieldValue = fvValue(cit);

                    SWSS_LOG_INFO("FIELD: %s, VALUE: %s", fieldName.c_str(), fieldValue.c_str());

                    fvMap[fieldName] = fieldValue;
                }

                pCfg.fieldValueMap = fvMap;

                if (!m_portHlpr.parsePortConfig(pCfg))
                {
                    return false;
                }

                return true;
            };

            if (m_portList.find(key) == m_portList.end())
            {
                // Aggregate configuration while the port is not created.
                auto &fvMap = m_portConfigMap[key];

                if (!parsePortFvs(fvMap))
                {
                    it = taskMap.erase(it);
                    continue;
                }

                if (!m_portHlpr.validatePortConfig(pCfg))
                {
                    it = taskMap.erase(it);
                    continue;
                }

                /* Collect information about all received ports */
                m_lanesAliasSpeedMap[pCfg.lanes.value] = pCfg;
            }
            else
            {
                // Port is already created, gather updated field-values.
                std::unordered_map<std::string, std::string> fvMap;

                if (!parsePortFvs(fvMap))
                {
                    it = taskMap.erase(it);
                    continue;
                }
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
            if (getPortConfigState() != PORT_CONFIG_MISSING)
            {
                std::vector<PortConfig> portsToAddList;
                std::vector<sai_object_id_t> portsToRemoveList;

                // Port remove comparison logic
                for (auto it = m_portListLaneMap.begin(); it != m_portListLaneMap.end();)
                {
                    if (m_lanesAliasSpeedMap.find(it->first) == m_lanesAliasSpeedMap.end())
                    {
                        portsToRemoveList.push_back(it->second);
                        it = m_portListLaneMap.erase(it);
                        continue;
                    }

                    it++;
                }

                // Bulk port remove
                if (!portsToRemoveList.empty())
                {
                    if (!removePortBulk(portsToRemoveList))
                    {
                        SWSS_LOG_THROW("PortsOrch initialization failure");
                    }
                }

                // Port add comparison logic
                for (auto it = m_lanesAliasSpeedMap.begin(); it != m_lanesAliasSpeedMap.end();)
                {
                    if (m_portListLaneMap.find(it->first) == m_portListLaneMap.end())
                    {
                        portsToAddList.push_back(it->second);
                        it++;
                        continue;
                    }

                    if (!initPort(it->second))
                    {
                        // Failure has been recorded in initPort
                        it++;
                        continue;
                    }

                    initPortSupportedSpeeds(it->second.key, m_portListLaneMap[it->first]);
                    initPortSupportedFecModes(it->second.key, m_portListLaneMap[it->first]);

                    it++;
                }

                // Bulk port add
                if (!portsToAddList.empty())
                {
                    if (!addPortBulk(portsToAddList))
                    {
                        SWSS_LOG_THROW("PortsOrch initialization failure");
                    }

                    for (const auto &cit : portsToAddList)
                    {
                        if (!initPort(cit))
                        {
                            // Failure has been recorded in initPort
                            continue;
                        }

                        initPortSupportedSpeeds(cit.key, m_portListLaneMap[cit.lanes.value]);
                        initPortSupportedFecModes(cit.key, m_portListLaneMap[cit.lanes.value]);
                    }
                }

                setPortConfigState(PORT_CONFIG_DONE);
            }

            if (getPortConfigState() != PORT_CONFIG_DONE)
            {
                // Not yet receive PortConfigDone. Save it for future retry
                it++;
                continue;
            }

            if (!gBufferOrch->isPortReady(pCfg.key))
            {
                // buffer configuration hasn't been applied yet. save it for future retry
                m_pendingPortSet.emplace(pCfg.key);
                it++;
                continue;
            }
            else
            {
                m_pendingPortSet.erase(pCfg.key);
            }

            Port p;
            if (!getPort(pCfg.key, p))
            {
                SWSS_LOG_ERROR("Failed to get port id by alias: %s", pCfg.key.c_str());
            }
            else
            {
                PortSerdesAttrMap_t serdes_attr;
                getPortSerdesAttr(serdes_attr, pCfg);

                // Saved configured admin status
                bool admin_status = p.m_admin_state_up;

                if (pCfg.autoneg.is_set)
                {
                    if (!p.m_an_cfg || p.m_autoneg != pCfg.autoneg.value)
                    {
                        if (p.m_cap_an < 0)
                        {
                            initPortCapAutoNeg(p);
                            m_portList[p.m_alias] = p;
                        }
                        if (p.m_cap_an < 1)
                        {
                            SWSS_LOG_ERROR("%s: autoneg is not supported (cap=%d)", p.m_alias.c_str(), p.m_cap_an);
                            // autoneg is not supported, don't retry
                            it = taskMap.erase(it);
                            continue;
                        }
                        if (p.m_admin_state_up)
                        {
                            /* Bring port down before applying speed */
                            if (!setPortAdminStatus(p, false))
                            {
                                SWSS_LOG_ERROR(
                                    "Failed to set port %s admin status DOWN to set port autoneg mode",
                                    p.m_alias.c_str()
                                );
                                it++;
                                continue;
                            }

                            p.m_admin_state_up = false;
                            m_portList[p.m_alias] = p;
                        }

                        auto status = setPortAutoNeg(p, pCfg.autoneg.value);
                        if (status != task_success)
                        {
                            SWSS_LOG_ERROR(
                                "Failed to set port %s AN from %d to %d",
                                p.m_alias.c_str(), p.m_autoneg, pCfg.autoneg.value
                            );
                            if (status == task_need_retry)
                            {
                                it++;
                            }
                            else
                            {
                                it = taskMap.erase(it);
                            }
                            continue;
                        }

                        p.m_autoneg = pCfg.autoneg.value;
                        p.m_an_cfg = true;
                        m_portList[p.m_alias] = p;
                        m_portStateTable.hdel(p.m_alias, "rmt_adv_speeds");
                        updatePortStatePoll(p, PORT_STATE_POLL_AN, pCfg.autoneg.value);

                        SWSS_LOG_NOTICE(
                            "Set port %s autoneg to %s",
                            p.m_alias.c_str(), m_portHlpr.getAutonegStr(pCfg).c_str()
                        );
                    }
                }

                if (pCfg.link_training.is_set)
                {
                    if (!p.m_lt_cfg || ((p.m_link_training != pCfg.link_training.value) && (p.m_type == Port::PHY)))
                    {
                        if (p.m_cap_lt < 0)
                        {
                            initPortCapLinkTraining(p);
                            m_portList[p.m_alias] = p;
                        }
                        if (p.m_cap_lt < 1)
                        {
                            SWSS_LOG_WARN("%s: LT is not supported(cap=%d)", p.m_alias.c_str(), p.m_cap_lt);
                            // Don't retry
                            it = taskMap.erase(it);
                            continue;
                        }

                        auto status = setPortLinkTraining(p, pCfg.link_training.value);
                        if (status != task_success)
                        {
                            SWSS_LOG_ERROR(
                                "Failed to set port %s LT from %d to %d",
                                p.m_alias.c_str(), p.m_link_training, pCfg.link_training.value
                            );
                            if (status == task_need_retry)
                            {
                                it++;
                            }
                            else
                            {
                                it = taskMap.erase(it);
                            }
                            continue;
                        }

                        m_portStateTable.hset(p.m_alias, "link_training_status", m_portHlpr.getLinkTrainingStr(pCfg));
                        p.m_link_training = pCfg.link_training.value;
                        p.m_lt_cfg = true;
                        m_portList[p.m_alias] = p;
                        updatePortStatePoll(p, PORT_STATE_POLL_LT, pCfg.link_training.value);

                        // Restore pre-emphasis when LT is transitioned from ON to OFF
                        if (!p.m_link_training && serdes_attr.empty())
                        {
                            serdes_attr = p.m_preemphasis;
                        }

                        SWSS_LOG_NOTICE(
                            "Set port %s link training to %s",
                            p.m_alias.c_str(), m_portHlpr.getLinkTrainingStr(pCfg).c_str()
                        );
                    }
                }

                if (pCfg.link_event_damping_algorithm.is_set)
                {
                    if (p.m_link_event_damping_algorithm != pCfg.link_event_damping_algorithm.value)
                    {
                        auto status = setPortLinkEventDampingAlgorithm(p, pCfg.link_event_damping_algorithm.value);
                        if (!status.ok())
                        {
                            SWSS_LOG_ERROR(
                                "Failed to set port %s link event damping algorithm to %s",
                                p.m_alias.c_str(), m_portHlpr.getDampingAlgorithm(pCfg).c_str()
                            );
                            it = taskMap.erase(it);
                            continue;
                        }

                         p.m_link_event_damping_algorithm = pCfg.link_event_damping_algorithm.value;
                         m_portList[p.m_alias] = p;

                         SWSS_LOG_NOTICE(
                             "Set port %s link event damping algorithm to %s",
                             p.m_alias.c_str(), m_portHlpr.getDampingAlgorithm(pCfg).c_str()
                         );
                    }
                }

                sai_redis_link_event_damping_algo_aied_config_t aied_config = {
                    p.m_max_suppress_time,
                    p.m_suppress_threshold,
                    p.m_reuse_threshold,
                    p.m_decay_half_life,
                    p.m_flap_penalty,
                };

                if (pCfg.link_event_damping_config.max_suppress_time.is_set)
                {
                    aied_config.max_suppress_time = pCfg.link_event_damping_config.max_suppress_time.value;
                }
                if (pCfg.link_event_damping_config.decay_half_life.is_set)
                {
                    aied_config.decay_half_life = pCfg.link_event_damping_config.decay_half_life.value;
                }
                if (pCfg.link_event_damping_config.suppress_threshold.is_set)
                {
                    aied_config.suppress_threshold = pCfg.link_event_damping_config.suppress_threshold.value;
                }
                if (pCfg.link_event_damping_config.reuse_threshold.is_set)
                {
                    aied_config.reuse_threshold = pCfg.link_event_damping_config.reuse_threshold.value;
                }
                if (pCfg.link_event_damping_config.flap_penalty.is_set)
                {
                    aied_config.flap_penalty = pCfg.link_event_damping_config.flap_penalty.value;
                }

                bool config_changed = !(aied_config.max_suppress_time == p.m_max_suppress_time &&
                                        aied_config.decay_half_life == p.m_decay_half_life &&
                                        aied_config.suppress_threshold == p.m_suppress_threshold &&
                                        aied_config.reuse_threshold == p.m_reuse_threshold &&
                                        aied_config.flap_penalty == p.m_flap_penalty);

                if (config_changed)
                {
                    auto status = setPortLinkEventDampingAiedConfig(p, aied_config);
                    if (!status.ok())
                    {
                        SWSS_LOG_ERROR("Failed to set port %s link event damping config", p.m_alias.c_str());
                        it = taskMap.erase(it);
                        continue;
                    }

                    p.m_max_suppress_time = aied_config.max_suppress_time;
                    p.m_decay_half_life = aied_config.decay_half_life;
                    p.m_suppress_threshold = aied_config.suppress_threshold;
                    p.m_reuse_threshold = aied_config.reuse_threshold;
                    p.m_flap_penalty = aied_config.flap_penalty;
                    m_portList[p.m_alias] = p;

                    SWSS_LOG_NOTICE("Set port %s link event damping config successfully", p.m_alias.c_str());
                }

                if (pCfg.speed.is_set)
                {
                    if (p.m_speed != pCfg.speed.value)
                    {
                        if (!isSpeedSupported(p.m_alias, p.m_port_id, pCfg.speed.value))
                        {
                            SWSS_LOG_ERROR(
                                "Unsupported port %s speed %u",
                                p.m_alias.c_str(), pCfg.speed.value
                            );
                            // Speed not supported, dont retry
                            it = taskMap.erase(it);
                            continue;
                        }

                        // for backward compatible, if autoneg is off, toggle admin status
                        if (p.m_admin_state_up && !p.m_autoneg)
                        {
                            /* Bring port down before applying speed */
                            if (!setPortAdminStatus(p, false))
                            {
                                SWSS_LOG_ERROR(
                                    "Failed to set port %s admin status DOWN to set speed",
                                    p.m_alias.c_str()
                                );
                                it++;
                                continue;
                            }

                            p.m_admin_state_up = false;
                            m_portList[p.m_alias] = p;
                        }

                        auto status = setPortSpeed(p, pCfg.speed.value);
                        if (status != task_success)
                        {
                            SWSS_LOG_ERROR(
                                "Failed to set port %s speed from %u to %u",
                                p.m_alias.c_str(), p.m_speed, pCfg.speed.value
                            );
                            if (status == task_need_retry)
                            {
                                it++;
                            }
                            else
                            {
                                it = taskMap.erase(it);
                            }
                            continue;
                        }

                        p.m_speed = pCfg.speed.value;
                        m_portList[p.m_alias] = p;

                        SWSS_LOG_NOTICE(
                            "Set port %s speed to %u",
                            p.m_alias.c_str(), pCfg.speed.value
                        );
                    }
                    else
                    {
                        /* Always update Gearbox speed on Gearbox ports */
                        setGearboxPortsAttr(p, SAI_PORT_ATTR_SPEED, &pCfg.speed.value);
                    }
                }

                if (pCfg.adv_speeds.is_set)
                {
                    if (!p.m_adv_speed_cfg || p.m_adv_speeds != pCfg.adv_speeds.value)
                    {
                        if (p.m_admin_state_up && p.m_autoneg)
                        {
                            /* Bring port down before applying speed */
                            if (!setPortAdminStatus(p, false))
                            {
                                SWSS_LOG_ERROR(
                                    "Failed to set port %s admin status DOWN to set interface type",
                                    p.m_alias.c_str()
                                );
                                it++;
                                continue;
                            }

                            p.m_admin_state_up = false;
                            m_portList[p.m_alias] = p;
                        }

                        auto adv_speeds = swss::join(',', pCfg.adv_speeds.value.begin(), pCfg.adv_speeds.value.end());
                        auto ori_adv_speeds = swss::join(',', p.m_adv_speeds.begin(), p.m_adv_speeds.end());
                        auto status = setPortAdvSpeeds(p, pCfg.adv_speeds.value);
                        if (status != task_success)
                        {
                            SWSS_LOG_ERROR(
                                "Failed to set port %s advertised speed from %s to %s",
                                p.m_alias.c_str(), ori_adv_speeds.c_str(), adv_speeds.c_str()
                            );
                            if (status == task_need_retry)
                            {
                                it++;
                            }
                            else
                            {
                                it = taskMap.erase(it);
                            }
                            continue;
                        }

                        p.m_adv_speeds = pCfg.adv_speeds.value;
                        p.m_adv_speed_cfg = true;
                        m_portList[p.m_alias] = p;

                        SWSS_LOG_NOTICE(
                            "Set port %s advertised speed from %s to %s",
                            p.m_alias.c_str(), ori_adv_speeds.c_str(), adv_speeds.c_str()
                        );
                    }
                }

                if (pCfg.interface_type.is_set)
                {
                    if (!p.m_intf_cfg || p.m_interface_type != pCfg.interface_type.value)
                    {
                        if (p.m_admin_state_up && !p.m_autoneg)
                        {
                            /* Bring port down before applying speed */
                            if (!setPortAdminStatus(p, false))
                            {
                                SWSS_LOG_ERROR(
                                    "Failed to set port %s admin status DOWN to set interface type",
                                    p.m_alias.c_str()
                                );
                                it++;
                                continue;
                            }

                            p.m_admin_state_up = false;
                            m_portList[p.m_alias] = p;
                        }

                        auto status = setPortInterfaceType(p, pCfg.interface_type.value);
                        if (status != task_success)
                        {
                            SWSS_LOG_ERROR(
                                "Failed to set port %s interface type to %s",
                                p.m_alias.c_str(), m_portHlpr.getPortInterfaceTypeStr(pCfg).c_str()
                            );
                            if (status == task_need_retry)
                            {
                                it++;
                            }
                            else
                            {
                                it = taskMap.erase(it);
                            }
                            continue;
                        }

                        p.m_interface_type = pCfg.interface_type.value;
                        p.m_intf_cfg = true;
                        m_portList[p.m_alias] = p;

                        SWSS_LOG_NOTICE(
                            "Set port %s interface type to %s",
                            p.m_alias.c_str(), m_portHlpr.getPortInterfaceTypeStr(pCfg).c_str()
                        );
                    }
                }

                if (pCfg.adv_interface_types.is_set)
                {
                    if (!p.m_adv_intf_cfg || p.m_adv_interface_types != pCfg.adv_interface_types.value)
                    {
                        if (p.m_admin_state_up && p.m_autoneg)
                        {
                            /* Bring port down before applying speed */
                            if (!setPortAdminStatus(p, false))
                            {
                                SWSS_LOG_ERROR(
                                    "Failed to set port %s admin status DOWN to set interface type",
                                    p.m_alias.c_str()
                                );
                                it++;
                                continue;
                            }

                            p.m_admin_state_up = false;
                            m_portList[p.m_alias] = p;
                        }

                        auto status = setPortAdvInterfaceTypes(p, pCfg.adv_interface_types.value);
                        if (status != task_success)
                        {
                            SWSS_LOG_ERROR(
                                "Failed to set port %s advertised interface types to %s",
                                p.m_alias.c_str(), m_portHlpr.getAdvInterfaceTypesStr(pCfg).c_str()
                            );
                            if (status == task_need_retry)
                            {
                                it++;
                            }
                            else
                            {
                                it = taskMap.erase(it);
                            }
                            continue;
                        }

                        p.m_adv_interface_types = pCfg.adv_interface_types.value;
                        p.m_adv_intf_cfg = true;
                        m_portList[p.m_alias] = p;

                        SWSS_LOG_NOTICE(
                            "Set port %s advertised interface type to %s",
                            p.m_alias.c_str(), m_portHlpr.getAdvInterfaceTypesStr(pCfg).c_str()
                        );
                    }
                }

                if (pCfg.mtu.is_set)
                {
                    if (p.m_mtu != pCfg.mtu.value)
                    {
                        if (!setPortMtu(p, pCfg.mtu.value))
                        {
                            SWSS_LOG_ERROR(
                                "Failed to set port %s MTU to %u",
                                p.m_alias.c_str(), pCfg.mtu.value
                            );
                            it++;
                            continue;
                        }

                        p.m_mtu = pCfg.mtu.value;
                        m_portList[p.m_alias] = p;

                        if (p.m_rif_id)
                        {
                            gIntfsOrch->setRouterIntfsMtu(p);
                        }

                        // Sub interfaces inherit parent physical port mtu
                        updateChildPortsMtu(p, pCfg.mtu.value);

                        SWSS_LOG_NOTICE(
                            "Set port %s MTU to %u",
                            p.m_alias.c_str(), pCfg.mtu.value
                        );
                    }
                }

                if (pCfg.tpid.is_set)
                {
                    if (p.m_tpid != pCfg.tpid.value)
                    {
                        if (!setPortTpid(p, pCfg.tpid.value))
                        {
                            SWSS_LOG_ERROR(
                                "Failed to set port %s TPID to 0x%x",
                                p.m_alias.c_str(), pCfg.tpid.value
                            );
                            it++;
                            continue;
                        }

                        p.m_tpid = pCfg.tpid.value;
                        m_portList[p.m_alias] = p;

                        SWSS_LOG_NOTICE(
                            "Set port %s TPID to 0x%x",
                            p.m_alias.c_str(), pCfg.tpid.value
                        );
                    }
                }

                if (pCfg.fec.is_set)
                {
                    /* reset fec mode upon mode change */
                    if (!p.m_fec_cfg || p.m_fec_mode != pCfg.fec.value || p.m_override_fec != pCfg.fec.override_fec)
                    {
                        if (!pCfg.fec.override_fec && !fec_override_sup)
                        {
                            SWSS_LOG_ERROR("Auto FEC mode is not supported");
                            it = taskMap.erase(it);
                            continue;
                        }
                        if (!isFecModeSupported(p, pCfg.fec.value))
                        {
                            SWSS_LOG_ERROR(
                                "Unsupported port %s FEC mode %s",
                                p.m_alias.c_str(), m_portHlpr.getFecStr(pCfg).c_str()
                            );
                            // FEC mode is not supported, don't retry
                            it = taskMap.erase(it);
                            continue;
                        }

                        if (!pCfg.fec.override_fec && !p.m_autoneg)
                        {
                            SWSS_LOG_NOTICE("Autoneg must be enabled for port fec mode auto to work");
                        }

                        if (p.m_admin_state_up)
                        {
                            /* Bring port down before applying fec mode*/
                            if (!setPortAdminStatus(p, false))
                            {
                                SWSS_LOG_ERROR(
                                    "Failed to set port %s admin status DOWN to set fec mode",
                                    p.m_alias.c_str()
                                );
                                it++;
                                continue;
                            }

                            p.m_admin_state_up = false;
                            m_portList[p.m_alias] = p;
                        }

                        if (!setPortFec(p, pCfg.fec.value, pCfg.fec.override_fec))
                        {
                            SWSS_LOG_ERROR(
                                "Failed to set port %s FEC mode %s",
                                p.m_alias.c_str(), m_portHlpr.getFecStr(pCfg).c_str()
                            );
                            it++;
                            continue;
                        }

                        p.m_fec_mode = pCfg.fec.value;
                        p.m_override_fec = pCfg.fec.override_fec;
                        p.m_fec_cfg = true;
                        m_portList[p.m_alias] = p;

                        SWSS_LOG_NOTICE(
                            "Set port %s FEC mode to %s",
                            p.m_alias.c_str(), m_portHlpr.getFecStr(pCfg).c_str()
                        );
                    }
                }

                if (pCfg.learn_mode.is_set)
                {
                    if (!p.m_lm_cfg || p.m_learn_mode != pCfg.learn_mode.value)
                    {
                        if(!setBridgePortLearnMode(p, pCfg.learn_mode.value))
                        {
                            SWSS_LOG_ERROR(
                                "Failed to set port %s learn mode to %s",
                                p.m_alias.c_str(), m_portHlpr.getLearnModeStr(pCfg).c_str()
                            );
                            it++;
                            continue;
                        }

                        p.m_learn_mode = pCfg.learn_mode.value;
                        p.m_lm_cfg = true;
                        m_portList[p.m_alias] = p;

                        SWSS_LOG_NOTICE(
                            "Set port %s learn mode to %s",
                            p.m_alias.c_str(), m_portHlpr.getLearnModeStr(pCfg).c_str()
                        );
                    }
                }

                if (pCfg.pfc_asym.is_set)
                {
                    if (!p.m_pfc_asym_cfg || p.m_pfc_asym != pCfg.pfc_asym.value)
                    {
                        if (m_portCap.isPortPfcAsymSupported())
                        {
                            if (!setPortPfcAsym(p, pCfg.pfc_asym.value))
                            {
                                SWSS_LOG_ERROR(
                                    "Failed to set port %s asymmetric PFC to %s",
                                    p.m_alias.c_str(), m_portHlpr.getPfcAsymStr(pCfg).c_str()
                                );
                                it++;
                                continue;
                            }

                            p.m_pfc_asym = pCfg.pfc_asym.value;
                            p.m_pfc_asym_cfg = true;
                            m_portList[p.m_alias] = p;

                            SWSS_LOG_NOTICE(
                                "Set port %s asymmetric PFC to %s",
                                p.m_alias.c_str(), m_portHlpr.getPfcAsymStr(pCfg).c_str()
                            );
                        }
                        else
                        {
                            SWSS_LOG_WARN(
                                "Port %s asymmetric PFC configuration is not supported: skipping ...",
                                p.m_alias.c_str()
                            );
                        }
                    }
                }

                if (!serdes_attr.empty())
                {
                    if (p.m_link_training)
                    {
                        SWSS_LOG_NOTICE("Save port %s preemphasis for LT", p.m_alias.c_str());
                        p.m_preemphasis = serdes_attr;
                        m_portList[p.m_alias] = p;
                    }
                    else
                    {
                        if (p.m_admin_state_up)
                        {
                                /* Bring port down before applying serdes attribute*/
                                if (!setPortAdminStatus(p, false))
                                {
                                    SWSS_LOG_ERROR("Failed to set port %s admin status DOWN to set serdes attr", p.m_alias.c_str());
                                    it++;
                                    continue;
                                }

                                p.m_admin_state_up = false;
                                m_portList[p.m_alias] = p;
                        }

                        if (setPortSerdesAttribute(p.m_port_id, gSwitchId, serdes_attr))
                        {
                            SWSS_LOG_NOTICE("Set port %s SI settings is successful", p.m_alias.c_str());
                            p.m_preemphasis = serdes_attr;
                            m_portList[p.m_alias] = p;
                        }
                        else
                        {
                            SWSS_LOG_ERROR("Failed to set port %s SI settings", p.m_alias.c_str());
                            it++;
                            continue;
                        }
                    }
                }

                /* create host_tx_ready field in state-db */
                initHostTxReadyState(p);

                // Restore admin status if the port was brought down
                if (admin_status != p.m_admin_state_up)
                {
                    pCfg.admin_status.is_set = true;
                    pCfg.admin_status.value = admin_status;
                }

                /* Last step set port admin status */
                if (pCfg.admin_status.is_set)
                {
                    if (p.m_admin_state_up != pCfg.admin_status.value)
                    {
                        if (!setPortAdminStatus(p, pCfg.admin_status.value))
                        {
                            SWSS_LOG_ERROR(
                                "Failed to set port %s admin status to %s",
                                p.m_alias.c_str(), m_portHlpr.getAdminStatusStr(pCfg).c_str()
                            );
                            it++;
                            continue;
                        }

                        p.m_admin_state_up = pCfg.admin_status.value;
                        m_portList[p.m_alias] = p;

                        SWSS_LOG_NOTICE(
                            "Set port %s admin status to %s",
                            p.m_alias.c_str(), m_portHlpr.getAdminStatusStr(pCfg).c_str()
                        );
                    }
                }

                if (pCfg.pt_intf_id.is_set)
                {
                    if (!m_isPathTracingSupported)
                    {
                        SWSS_LOG_WARN(
                            "Failed to set Path Tracing Interface ID: Path Tracing is not supported by the switch"
                        );
                        it = taskMap.erase(it);
                        continue;
                    }

                    if (p.m_pt_intf_id != pCfg.pt_intf_id.value)
                    {
                        /*
                         * First, let's check the Path Tracing Interface ID configured for the port.
                         *
                         * Path Tracing Interface ID > 0 -> Path Tracing ENABLED on the port
                         * Path Tracing Interface ID == 0 -> Path Tracing DISABLED on the port
                         */
                        if (pCfg.pt_intf_id.value != 0)
                        {
                            /* Path Tracing ENABLED case */

                            /* Create and set port TAM object */
                            if (!createAndSetPortPtTam(p))
                            {
                                SWSS_LOG_ERROR(
                                    "Failed to create and set port %s TAM object for Path Tracing",
                                    p.m_alias.c_str()
                                );
                                it++;
                                continue;
                            }
                        }
                        else
                        {
                            /* Path Tracing DISABLED case */

                            /* Unset port TAM object */
                            if (!unsetPortPtTam(p))
                            {
                                SWSS_LOG_ERROR(
                                    "Failed to unset port %s TAM object for Path Tracing",
                                    p.m_alias.c_str()
                                );
                                it++;
                                continue;
                            }
                        }

                        /* Set Path Tracing Interface ID */
                        if (!setPortPtIntfId(p, pCfg.pt_intf_id.value))
                        {
                            SWSS_LOG_ERROR(
                                "Failed to set port %s Intf ID to %u",
                                p.m_alias.c_str(), pCfg.pt_intf_id.value
                            );
                            it++;
                            continue;
                        }

                        p.m_pt_intf_id = pCfg.pt_intf_id.value;
                        m_portList[p.m_alias] = p;

                        SWSS_LOG_NOTICE(
                            "Set port %s Intf ID to %u",
                            p.m_alias.c_str(), pCfg.pt_intf_id.value
                        );
                    }
                }

                if (pCfg.pt_timestamp_template.is_set)
                {
                    if (!m_isPathTracingSupported)
                    {
                        SWSS_LOG_WARN(
                            "Failed to set Path Tracing Timestamp Template: Path Tracing is not supported by the switch"
                        );
                        it = taskMap.erase(it);
                        continue;
                    }

                    if (p.m_pt_timestamp_template != pCfg.pt_timestamp_template.value)
                    {
                        if (!setPortPtTimestampTemplate(p, pCfg.pt_timestamp_template.value))
                        {
                            SWSS_LOG_ERROR(
                                "Failed to set port %s Timestamp Template to %s",
                                p.m_alias.c_str(), m_portHlpr.getPtTimestampTemplateStr(pCfg).c_str()
                            );
                            it++;
                            continue;
                        }

                        p.m_pt_timestamp_template = pCfg.pt_timestamp_template.value;
                        m_portList[p.m_alias] = p;

                        SWSS_LOG_NOTICE(
                            "Set port %s Timestamp Template to %s",
                            p.m_alias.c_str(), m_portHlpr.getPtTimestampTemplateStr(pCfg).c_str()
                        );
                    }
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            Port p;
            if (!getPort(pCfg.key, p))
            {
                SWSS_LOG_ERROR("Failed to remove port: alias %s doesn't exist", pCfg.key.c_str());
                m_portConfigMap.erase(pCfg.key);
                it = taskMap.erase(it);
                continue;
            }

            const auto &alias = pCfg.key;

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

            /*
             * Unset port Path Tracing TAM object and decrease TAM object refcount before
             * removing the port (if the port has a TAM object associated)
             */
            if (!unsetPortPtTam(p))
            {
                SWSS_LOG_ERROR(
                    "Failed to unset port %s TAM object for Path Tracing",
                    p.m_alias.c_str()
                );
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
            m_portConfigMap.erase(alias);
            m_portList.erase(alias);
            saiOidToAlias.erase(port_id);

            SWSS_LOG_NOTICE("Removed port %s", alias.c_str());
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

void PortsOrch::doTransceiverPresenceCheck(Consumer &consumer)
{
    /*
    the idea is to listen to transceiver info table, and also maintain an internal list of plugged modules.

    */
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while(it != consumer.m_toSync.end())
    {
        auto t = it->second;
        string alias = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            SWSS_LOG_DEBUG("TRANSCEIVER_INFO table has changed - SET command for port %s", alias.c_str());

            if (m_pluggedModulesPort.find(alias) == m_pluggedModulesPort.end())
            {
                m_pluggedModulesPort[alias] = m_portList[alias];

                SWSS_LOG_DEBUG("Setting host_tx_signal allow for port %s", alias.c_str());
                setSaiHostTxSignal(m_pluggedModulesPort[alias], true);
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_DEBUG("TRANSCEIVER_INFO table has changed - DEL command for port %s", alias.c_str());

            Port p;
            if (m_pluggedModulesPort.find(alias) != m_pluggedModulesPort.end())
            {
                p = m_pluggedModulesPort[alias];
                m_pluggedModulesPort.erase(alias);
                SWSS_LOG_DEBUG("Setting host_tx_signal NOT allow for port %s", alias.c_str());
                setSaiHostTxSignal(p, false);
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

bool PortsOrch::setSaiHostTxSignal(const Port &port, bool enable)
{
    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_HOST_TX_SIGNAL_ENABLE;
    attr.value.booldata = enable;

    if (saiOidToAlias.find(port.m_port_id) != saiOidToAlias.end())
    {
        sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Could not set SAI_PORT_ATTR_HOST_TX_SIGNAL_ENABLE to port 0x%" PRIx64, port.m_port_id);
            return false;
        }
        return true;
    }

    SWSS_LOG_NOTICE("Could not set SAI_PORT_ATTR_HOST_TX_SIGNAL_ENABLE - OID does not exist 0x%" PRIx64, port.m_port_id);
    return true;
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
            string learn_mode_str;
            sai_bridge_port_fdb_learning_mode_t learn_mode = SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW;
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
                    learn_mode_str = fvValue(i);

                    const auto &cit = learn_mode_map.find(learn_mode_str);
                    if (cit == learn_mode_map.cend())
                    {
                        SWSS_LOG_ERROR("Invalid MAC learn mode: %s", learn_mode_str.c_str());
                        it++;
                        continue;
                    }

                    learn_mode = cit->second;
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

                if (!learn_mode_str.empty() && (l.m_learn_mode != learn_mode))
                {
                    if (l.m_bridge_port_id != SAI_NULL_OBJECT_ID)
                    {
                        if(setBridgePortLearnMode(l, learn_mode))
                        {
                            l.m_learn_mode = learn_mode;
                            m_portList[alias] = l;
                            SWSS_LOG_NOTICE("Set port %s learn mode to %s", alias.c_str(), learn_mode_str.c_str());
                        }
                        else
                        {
                            SWSS_LOG_ERROR("Failed to set port %s learn mode to %s", alias.c_str(), learn_mode_str.c_str());
                            it++;
                            continue;
                        }
                    }
                    else
                    {
                        l.m_learn_mode = learn_mode;
                        m_portList[alias] = l;

                        SWSS_LOG_NOTICE("Saved to set port %s learn mode %s", alias.c_str(), learn_mode_str.c_str());
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

                if (!addLagMember(lag, port, status))
                {
                    it++;
                    continue;
                }
            }

            if ((gMySwitchType == "voq") && (port.m_type != Port::SYSTEM))
            {
               //Sync to SYSTEM_LAG_MEMBER_TABLE of CHASSIS_APP_DB
               voqSyncAddLagMember(lag, port, status);
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
        APP_VLAN_MEMBER_TABLE_NAME
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

    if (table_name == STATE_TRANSCEIVER_INFO_TABLE_NAME)
    {
        doTransceiverPresenceCheck(consumer);
    }
    else if (table_name == APP_PORT_TABLE_NAME)
    {
        doPortTask(consumer);
    }
    else if (table_name == APP_SEND_TO_INGRESS_PORT_TABLE_NAME)
    {
        doSendToIngressPortTask(consumer);
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

void PortsOrch::initializeVoqs(Port &port)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_SYSTEM_PORT_ATTR_QOS_NUMBER_OF_VOQS;
    sai_status_t status = sai_system_port_api->get_system_port_attribute(
		    port.m_system_port_oid, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get number of voqs for port %s rv:%d", port.m_alias.c_str(), status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
        if (handle_status != task_process_status::task_success)
        {
            throw runtime_error("PortsOrch initialization failure.");
        }
    }
    SWSS_LOG_INFO("Get %d voq for port %s", attr.value.u32, port.m_alias.c_str());

    m_port_voq_ids[port.m_alias] = std::vector<sai_object_id_t>( attr.value.u32 );

    if (attr.value.u32 == 0)
    {
        return;
    }

    attr.id = SAI_SYSTEM_PORT_ATTR_QOS_VOQ_LIST;
    attr.value.objlist.count = (uint32_t) m_port_voq_ids[port.m_alias].size();
    attr.value.objlist.list = m_port_voq_ids[port.m_alias].data();

    status = sai_system_port_api->get_system_port_attribute(
			port.m_system_port_oid, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get voq list for port %s rv:%d", port.m_alias.c_str(), status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
        if (handle_status != task_process_status::task_success)
        {
            throw runtime_error("PortsOrch initialization failure.");
        }
    }

    SWSS_LOG_INFO("Get voqs for port %s", port.m_alias.c_str());
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

void PortsOrch::initializeSchedulerGroups(Port &port)
{
    std::vector<sai_object_id_t> scheduler_group_ids;
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_QOS_NUMBER_OF_SCHEDULER_GROUPS;
    sai_status_t status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get number of scheduler groups for port:%s", port.m_alias.c_str());
        task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
        if (handle_status != task_process_status::task_success)
        {
            throw runtime_error("PortsOrch initialization failure.");
        }
    }
    SWSS_LOG_INFO("Got %d number of scheduler groups for port %s", attr.value.u32, port.m_alias.c_str());

    scheduler_group_ids.resize(attr.value.u32);

    if (attr.value.u32 == 0)
    {
        return;
    }

    attr.id = SAI_PORT_ATTR_QOS_SCHEDULER_GROUP_LIST;
    attr.value.objlist.count = (uint32_t)scheduler_group_ids.size();
    attr.value.objlist.list = scheduler_group_ids.data();

    status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get scheduler group list for port %s rv:%d", port.m_alias.c_str(), status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
        if (handle_status != task_process_status::task_success)
        {
            throw runtime_error("PortsOrch initialization failure.");
        }
    }

    SWSS_LOG_INFO("Got scheduler groups for port %s", port.m_alias.c_str());
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

    if (gMySwitchType != "dpu")
    {
        initializePriorityGroups(port);
        initializeQueues(port);
        initializeSchedulerGroups(port);
        initializePortBufferMaximumParameters(port);
    }

    /* Create host interface */
    if (!addHostIntfs(port, port.m_alias, port.m_hif_id))
    {
        SWSS_LOG_ERROR("Failed to create host interface for port %s", port.m_alias.c_str());
        return false;
    }

    /* Check warm start states */
    vector<FieldValueTuple> tuples;
    bool exist = m_portTable->get(port.m_alias, tuples);
    string operStatus, flapCount = "0";
    if (exist)
    {
        for (auto i : tuples)
        {
            if (fvField(i) == "oper_status")
            {
                operStatus = fvValue(i);
            }

            if (fvField(i) == "flap_count")
            {
                flapCount = fvValue(i);
            }
        }
    }
    SWSS_LOG_INFO("Port %s with oper %s flap_count=%s", port.m_alias.c_str(), operStatus.c_str(), flapCount.c_str());

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

    // initalize port flap count
    if (!flapCount.empty())
    {
        try
        {
            port.m_flap_count = stoull(flapCount);
            m_portTable->hset(port.m_alias, "flap_count", flapCount);
        }
        catch (const std::exception &e)
        {
            SWSS_LOG_ERROR("Failed to get port (%s) flap_count: %s", port.m_alias.c_str(), e.what());
        }
    }

    /* initialize port admin status */
    if (!getPortAdminStatus(port.m_port_id, port.m_admin_state_up))
    {
        SWSS_LOG_ERROR("Failed to get initial port admin status %s", port.m_alias.c_str());
        return false;
    }

    /* initialize port admin speed */
    if (!isAutoNegEnabled(port.m_port_id) && !getPortSpeed(port.m_port_id, port.m_speed))
    {
        SWSS_LOG_ERROR("Failed to get initial port admin speed %d", port.m_speed);
        return false;
    }

    /* initialize port mtu */
    if (!getPortMtu(port, port.m_mtu))
    {
        SWSS_LOG_ERROR("Failed to get initial port mtu %d", port.m_mtu);
    }

    /* initialize port host_tx_ready value (only for supporting systems) */
    if (m_cmisModuleAsicSyncSupported)
    {
        bool hostTxReadyVal;
        if (!getPortHostTxReady(port, hostTxReadyVal))
        {
            SWSS_LOG_ERROR("Failed to get host_tx_ready value from SAI to Port %" PRIx64 , port.m_port_id);
        }
        /* set value to state DB */

        string hostTxReadyStr = hostTxReadyVal ? "true" : "false";

        SWSS_LOG_DEBUG("Received host_tx_ready current status: port_id: 0x%" PRIx64 " status: %s", port.m_port_id, hostTxReadyStr.c_str());
        setHostTxReady(port, hostTxReadyStr);
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

    bool set_hostif_tx_queue = false;
    if (gSwitchOrch->querySwitchCapability(SAI_OBJECT_TYPE_HOSTIF, SAI_HOSTIF_ATTR_QUEUE))
    {
        set_hostif_tx_queue = true;
    }
    else
    {
        SWSS_LOG_WARN("Hostif queue attribute not supported");
    }

    if (set_hostif_tx_queue)
    {
        attr.id = SAI_HOSTIF_ATTR_QUEUE;
        attr.value.u32 = DEFAULT_HOSTIF_TX_QUEUE;
        attrs.push_back(attr);

        port.m_host_tx_queue = DEFAULT_HOSTIF_TX_QUEUE;
        port.m_host_tx_queue_configured = true;
    }

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

ReturnCode PortsOrch::addSendToIngressHostIf(const std::string &send_to_ingress_name)
{
    SWSS_LOG_ENTER();

    // For SendToIngress port, add the host interface and bind to the CPU port
    vector<sai_attribute_t> ingress_attribs;
    sai_attribute_t attr;

    attr.id = SAI_HOSTIF_ATTR_TYPE;
    attr.value.s32 = SAI_HOSTIF_TYPE_NETDEV;
    ingress_attribs.push_back(attr);

    attr.id = SAI_HOSTIF_ATTR_NAME;
    auto size = sizeof(attr.value.chardata);
    strncpy(attr.value.chardata, send_to_ingress_name.c_str(),
            size - 1);
    attr.value.chardata[size - 1] = '\0';
    ingress_attribs.push_back(attr);

    // If this isn't passed as true, the false setting makes
    // the device unready for later attempts to set UP/RUNNING
    attr.id = SAI_HOSTIF_ATTR_OPER_STATUS;
    attr.value.booldata = true;
    ingress_attribs.push_back(attr);

    // Get CPU port object id to signal send to ingress
    attr.id = SAI_HOSTIF_ATTR_OBJ_ID;
    attr.value.oid = m_cpuPort.m_port_id;
    ingress_attribs.push_back(attr);

    LOG_AND_RETURN_IF_ERROR(sai_hostif_api->create_hostif(&m_cpuPort.m_hif_id,
                                                          gSwitchId,
                                                          (uint32_t)ingress_attribs.size(),
                                                          ingress_attribs.data()));

    return ReturnCode();
}

ReturnCode PortsOrch::removeSendToIngressHostIf()
{
    SWSS_LOG_ENTER();

    if (SAI_NULL_OBJECT_ID == m_cpuPort.m_hif_id)
    {
        ReturnCode rc = ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
            << "Can't delete invalid SendToIngress hostif with SAI_NULL_OBJECT_ID oid";
        SWSS_LOG_ERROR("%s", rc.message().c_str());
        return rc;
    }

    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_hostif_api->remove_hostif(m_cpuPort.m_hif_id),
        "Failed to delete SendToIngress hostif:0x"
            << std::hex << m_cpuPort.m_hif_id);

    return ReturnCode();
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

    if (port.m_rif_id != 0)
    {
        SWSS_LOG_NOTICE("Cannot create bridge port, interface %s is a router port", port.m_alias.c_str());
        return false;
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
    attr.value.s32 = port.m_learn_mode;
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

bool PortsOrch::setBridgePortLearnMode(Port &port, sai_bridge_port_fdb_learning_mode_t learn_mode)
{
    SWSS_LOG_ENTER();

    if (port.m_bridge_port_id == SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    /* Set bridge port learning mode */
    sai_attribute_t attr;
    attr.id = SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE;
    attr.value.s32 = learn_mode;

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

    SWSS_LOG_NOTICE("Set bridge port %s learning mode %d", port.m_alias.c_str(), learn_mode);

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
    m_vlanPorts.emplace(vlan_alias);

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
    m_vlanPorts.erase(vlan.m_alias);

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
            task_process_status handle_status = handleSaiSetStatus(SAI_API_VLAN, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
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
            task_process_status handle_status = handleSaiSetStatus(SAI_API_VLAN, status);
            if (handle_status != task_success)
            {
                m_portList[vlan.m_alias] = vlan;
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
        vlan.m_vlan_info.bc_flood_type = SAI_VLAN_FLOOD_CONTROL_TYPE_COMBINED;
    }

    if (vlan.m_vlan_info.l2mc_group_id == SAI_NULL_OBJECT_ID)
    {
        status = sai_l2mc_group_api->create_l2mc_group(&l2mc_group_id, gSwitchId, 0, NULL);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create l2mc flood group");
            task_process_status handle_status = handleSaiCreateStatus(SAI_API_L2MC_GROUP, status);
            if (handle_status != task_success)
            {
                m_portList[vlan.m_alias] = vlan;
                return parseHandleSaiStatusFailure(handle_status);
            }
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
                task_process_status handle_status = handleSaiSetStatus(SAI_API_VLAN, status);
                if (handle_status != task_success)
                {
                    m_portList[vlan.m_alias] = vlan;
                    return parseHandleSaiStatusFailure(handle_status);
                }
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
                task_process_status handle_status = handleSaiSetStatus(SAI_API_VLAN, status);
                if (handle_status != task_success)
                {
                    m_portList[vlan.m_alias] = vlan;
                    return parseHandleSaiStatusFailure(handle_status);
                }
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
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_L2MC_GROUP, status);
        if (handle_status != task_success)
        {
            m_portList[vlan.m_alias] = vlan;
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    vlan.m_vlan_info.l2mc_members[end_point_ip] = l2mc_group_member;
    m_portList[vlan.m_alias] = vlan;
    increaseBridgePortRefCount(port);

    VlanMemberUpdate update = { vlan, port, true };
    notify(SUBJECT_TYPE_VLAN_MEMBER_CHANGE, static_cast<void *>(&update));
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
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_L2MC_GROUP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
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
                task_process_status handle_status = handleSaiSetStatus(SAI_API_VLAN, status);
                if (handle_status != task_success)
                {
                    m_portList[vlan.m_alias] = vlan;
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
            attr.id = SAI_VLAN_ATTR_UNKNOWN_UNICAST_FLOOD_CONTROL_TYPE;
            attr.value.s32 = SAI_VLAN_FLOOD_CONTROL_TYPE_ALL;
            status = sai_vlan_api->set_vlan_attribute(vlan.m_vlan_info.vlan_oid, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set flood control type all"
                               " to vlan %hu for unknown unicast flooding",
                               vlan.m_vlan_info.vlan_id);
                task_process_status handle_status = handleSaiSetStatus(SAI_API_VLAN, status);
                if (handle_status != task_success)
                {
                    m_portList[vlan.m_alias] = vlan;
                    return parseHandleSaiStatusFailure(handle_status);
                }
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
                task_process_status handle_status = handleSaiSetStatus(SAI_API_VLAN, status);
                if (handle_status != task_success)
                {
                    m_portList[vlan.m_alias] = vlan;
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
            attr.id = SAI_VLAN_ATTR_BROADCAST_FLOOD_CONTROL_TYPE;
            attr.value.s32 = SAI_VLAN_FLOOD_CONTROL_TYPE_ALL;
            status = sai_vlan_api->set_vlan_attribute(vlan.m_vlan_info.vlan_oid, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set flood control type all"
                               " to vlan %hu for broadcast flooding",
                               vlan.m_vlan_info.vlan_id);
                task_process_status handle_status = handleSaiSetStatus(SAI_API_VLAN, status);
                if (handle_status != task_success)
                {
                    m_portList[vlan.m_alias] = vlan;
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
            vlan.m_vlan_info.bc_flood_type = SAI_VLAN_FLOOD_CONTROL_TYPE_ALL;
        }
        status = sai_l2mc_group_api->remove_l2mc_group(vlan.m_vlan_info.l2mc_group_id);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove l2mc group %" PRIx64, l2mc_group_id);
            task_process_status handle_status = handleSaiRemoveStatus(SAI_API_L2MC_GROUP, status);
            if (handle_status != task_success)
            {
                m_portList[vlan.m_alias] = vlan;
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
        vlan.m_vlan_info.l2mc_group_id = SAI_NULL_OBJECT_ID;
    }
    m_portList[vlan.m_alias] = vlan;
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

bool PortsOrch::addLagMember(Port &lag, Port &port, string member_status)
{
    SWSS_LOG_ENTER();
    bool enableForwarding = (member_status == "enabled");

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
        voqSyncAddLagMember(lag, port, member_status);
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
        tunnel.m_learn_mode = SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW;
    }
    else
    {
        tunnel.m_learn_mode = SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DISABLE;
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

void PortsOrch::generateQueueMap(map<string, FlexCounterQueueStates> queuesStateVector)
{
    if (m_isQueueMapGenerated)
    {
        return;
    }

    bool isCreateAllQueues = false;

    if (queuesStateVector.count(createAllAvailableBuffersStr))
    {
        isCreateAllQueues = true;
        queuesStateVector.clear();
    }

    for (const auto& it: m_portList)
    {
        if (it.second.m_type == Port::PHY)
        {
            if (!queuesStateVector.count(it.second.m_alias))
            {
                auto maxQueueNumber = getNumberOfPortSupportedQueueCounters(it.second.m_alias);
                FlexCounterQueueStates flexCounterQueueState(maxQueueNumber);
                if (isCreateAllQueues && maxQueueNumber)
                {
                    flexCounterQueueState.enableQueueCounters(0, maxQueueNumber - 1);
                }
                else if (it.second.m_host_tx_queue_configured && it.second.m_host_tx_queue <= maxQueueNumber)
                {
                    flexCounterQueueState.enableQueueCounters(it.second.m_host_tx_queue, it.second.m_host_tx_queue);
                }
                queuesStateVector.insert(make_pair(it.second.m_alias, flexCounterQueueState));
            }
            generateQueueMapPerPort(it.second, queuesStateVector.at(it.second.m_alias), false);
            if (gMySwitchType == "voq")
            {
                generateQueueMapPerPort(it.second, queuesStateVector.at(it.second.m_alias), true);
            }
        }

        if (it.second.m_type == Port::SYSTEM)
        {
            if (!queuesStateVector.count(it.second.m_alias))
            {
                auto maxQueueNumber = getNumberOfPortSupportedQueueCounters(it.second.m_alias);
                FlexCounterQueueStates flexCounterQueueState(maxQueueNumber);
                queuesStateVector.insert(make_pair(it.second.m_alias, flexCounterQueueState));
            }
            generateQueueMapPerPort(it.second, queuesStateVector.at(it.second.m_alias), true);
        }
    }

    m_isQueueMapGenerated = true;
}

void PortsOrch::generateQueueMapPerPort(const Port& port, FlexCounterQueueStates& queuesState, bool voq)
{
    /* Create the Queue map in the Counter DB */
    vector<FieldValueTuple> queueVector;
    vector<FieldValueTuple> queuePortVector;
    vector<FieldValueTuple> queueIndexVector;
    vector<FieldValueTuple> queueTypeVector;
    std::vector<sai_object_id_t> queue_ids;

    if (voq)
    {
        queue_ids = m_port_voq_ids[port.m_alias];
    }
    else
    {
        queue_ids = port.m_queue_ids;
    }

    for (size_t queueIndex = 0; queueIndex < queue_ids.size(); ++queueIndex)
    {
        std::ostringstream name;

        if (voq)
        {
            name << port.m_system_port_info.alias << ":" << queueIndex;
        }
        else
        {
            name << port.m_alias << ":" << queueIndex;
        }

        const auto id = sai_serialize_object_id(queue_ids[queueIndex]);

        string queueType;
        uint8_t queueRealIndex = 0;
        if (getQueueTypeAndIndex(queue_ids[queueIndex], queueType, queueRealIndex))
        {
	    /* voq counters are always enabled. There is no mechanism to disable voq
	     * counters in a voq system. */
            if ((gMySwitchType != "voq")  && !queuesState.isQueueCounterEnabled(queueRealIndex))
            {
                continue;
            }
            queueTypeVector.emplace_back(id, queueType);
            queueIndexVector.emplace_back(id, to_string(queueRealIndex));
        }

        queueVector.emplace_back(name.str(), id);
        if (voq)
        {
            // Install a flex counter for this voq to track stats. Voq counters do
            // not have buffer queue config. So it does not get enabled through the
            // flexcounter orch logic. Always enabled voq counters.
            addQueueFlexCountersPerPortPerQueueIndex(port, queueIndex, true);
            queuePortVector.emplace_back(id, sai_serialize_object_id(port.m_system_port_oid));
        }
        else
        {
            // In voq systems, always install a flex counter for this egress queue
            // to track stats. In voq systems, the buffer profiles are defined on
            // sysports. So the phy ports do not have buffer queue config. Hence
            // queuesStateVector built by getQueueConfigurations in flexcounterorch
            // never has phy ports in voq systems. So always enabled egress queue
            // counter on voq systems.
            if (gMySwitchType == "voq")
            {
               addQueueFlexCountersPerPortPerQueueIndex(port, queueIndex, false);
            }
            queuePortVector.emplace_back(id, sai_serialize_object_id(port.m_port_id));
        }
    }

    if (voq)
    {
        m_voqTable->set("", queueVector);
    }
    else
    {
        m_queueTable->set("", queueVector);
        CounterCheckOrch::getInstance().addPort(port);
    }
    m_queuePortTable->set("", queuePortVector);
    m_queueIndexTable->set("", queueIndexVector);
    m_queueTypeTable->set("", queueTypeVector);

}

void PortsOrch::addQueueFlexCounters(map<string, FlexCounterQueueStates> queuesStateVector)
{
    if (m_isQueueFlexCountersAdded)
    {
        return;
    }

    bool isCreateAllQueues = false;

    if (queuesStateVector.count(createAllAvailableBuffersStr))
    {
        isCreateAllQueues = true;
        queuesStateVector.clear();
    }

    for (const auto& it: m_portList)
    {
        if (it.second.m_type == Port::PHY)
        {
            if (!queuesStateVector.count(it.second.m_alias))
            {
                auto maxQueueNumber = getNumberOfPortSupportedQueueCounters(it.second.m_alias);
                FlexCounterQueueStates flexCounterQueueState(maxQueueNumber);
                if (isCreateAllQueues && maxQueueNumber)
                {
                    flexCounterQueueState.enableQueueCounters(0, maxQueueNumber - 1);
                }
                else if (it.second.m_host_tx_queue_configured && it.second.m_host_tx_queue <= maxQueueNumber)
                {
                    flexCounterQueueState.enableQueueCounters(it.second.m_host_tx_queue, it.second.m_host_tx_queue);
                }
                queuesStateVector.insert(make_pair(it.second.m_alias, flexCounterQueueState));
            }
            addQueueFlexCountersPerPort(it.second, queuesStateVector.at(it.second.m_alias));
        }
    }

    m_isQueueFlexCountersAdded = true;
}


void PortsOrch::addQueueFlexCountersPerPort(const Port& port, FlexCounterQueueStates& queuesState)
{
    for (size_t queueIndex = 0; queueIndex < port.m_queue_ids.size(); ++queueIndex)
    {
        string queueType;
        uint8_t queueRealIndex = 0;
        if (getQueueTypeAndIndex(port.m_queue_ids[queueIndex], queueType, queueRealIndex))
        {
            if (!queuesState.isQueueCounterEnabled(queueRealIndex))
            {
                continue;
            }
            // Install a flex counter for this queue to track stats
            addQueueFlexCountersPerPortPerQueueIndex(port, queueIndex, false);
        }
    }
}

void PortsOrch::addQueueFlexCountersPerPortPerQueueIndex(const Port& port, size_t queueIndex, bool voq)
{
    std::unordered_set<string> counter_stats;
    std::vector<sai_object_id_t> queue_ids;

    for (const auto& it: queue_stat_ids)
    {
        counter_stats.emplace(sai_serialize_queue_stat(it));
    }
    if (voq)
    {
        for (const auto& voq_it: voq_stat_ids)
        {
            counter_stats.emplace(sai_serialize_queue_stat(voq_it));
        }
        queue_ids = m_port_voq_ids[port.m_alias];
    }
    else
    {
        queue_ids = port.m_queue_ids;
    }

    queue_stat_manager.setCounterIdList(queue_ids[queueIndex], CounterType::QUEUE, counter_stats);
}


void PortsOrch::addQueueWatermarkFlexCounters(map<string, FlexCounterQueueStates> queuesStateVector)
{
    if (m_isQueueWatermarkFlexCountersAdded)
    {
        return;
    }

    bool isCreateAllQueues = false;

    if (queuesStateVector.count(createAllAvailableBuffersStr))
    {
        isCreateAllQueues = true;
        queuesStateVector.clear();
    }

    for (const auto& it: m_portList)
    {
        if (it.second.m_type == Port::PHY)
        {
            if (!queuesStateVector.count(it.second.m_alias))
            {
                auto maxQueueNumber = getNumberOfPortSupportedQueueCounters(it.second.m_alias);
                FlexCounterQueueStates flexCounterQueueState(maxQueueNumber);
                if (isCreateAllQueues && maxQueueNumber)
                {
                    flexCounterQueueState.enableQueueCounters(0, maxQueueNumber - 1);
                }
                else if (it.second.m_host_tx_queue_configured && it.second.m_host_tx_queue <= maxQueueNumber)
                {
                    flexCounterQueueState.enableQueueCounters(it.second.m_host_tx_queue, it.second.m_host_tx_queue);
                }
                queuesStateVector.insert(make_pair(it.second.m_alias, flexCounterQueueState));
            }
            addQueueWatermarkFlexCountersPerPort(it.second, queuesStateVector.at(it.second.m_alias));
        }
    }

    m_isQueueWatermarkFlexCountersAdded = true;
}

void PortsOrch::addQueueWatermarkFlexCountersPerPort(const Port& port, FlexCounterQueueStates& queuesState)
{
    /* Add stat counters to flex_counter */

    for (size_t queueIndex = 0; queueIndex < port.m_queue_ids.size(); ++queueIndex)
    {
        string queueType;
        uint8_t queueRealIndex = 0;
        if (getQueueTypeAndIndex(port.m_queue_ids[queueIndex], queueType, queueRealIndex))
        {
            if (!queuesState.isQueueCounterEnabled(queueRealIndex))
            {
                continue;
            }
            addQueueWatermarkFlexCountersPerPortPerQueueIndex(port, queueIndex);
        }
    }
}

void PortsOrch::addQueueWatermarkFlexCountersPerPortPerQueueIndex(const Port& port, size_t queueIndex)
{
    const auto id = sai_serialize_object_id(port.m_queue_ids[queueIndex]);

    /* add watermark queue counters */
    string key = getQueueWatermarkFlexCounterTableKey(id);

    string delimiter("");
    std::ostringstream counters_stream;
    for (const auto& it: queueWatermarkStatIds)
    {
        counters_stream << delimiter << sai_serialize_queue_stat(it);
        delimiter = comma;
    }
    auto &&counters_str = counters_stream.str();

    startFlexCounterPolling(gSwitchId, key, counters_str, QUEUE_COUNTER_ID_LIST);
}

void PortsOrch::createPortBufferQueueCounters(const Port &port, string queues, bool skip_host_tx_queue)
{
    SWSS_LOG_ENTER();

    /* Create the Queue map in the Counter DB */
    vector<FieldValueTuple> queueVector;
    vector<FieldValueTuple> queuePortVector;
    vector<FieldValueTuple> queueIndexVector;
    vector<FieldValueTuple> queueTypeVector;

    auto toks = tokenize(queues, '-');
    auto startIndex = to_uint<uint32_t>(toks[0]);
    auto endIndex = startIndex;
    if (toks.size() > 1)
    {
        endIndex = to_uint<uint32_t>(toks[1]);
    }

    for (auto queueIndex = startIndex; queueIndex <= endIndex; queueIndex++)
    {
        if (queueIndex == (uint32_t)port.m_host_tx_queue && skip_host_tx_queue)
        {
            continue;
        }

        std::ostringstream name;
        name << port.m_alias << ":" << queueIndex;

        const auto id = sai_serialize_object_id(port.m_queue_ids[queueIndex]);

        string queueType;
        uint8_t queueRealIndex = 0;
        if (getQueueTypeAndIndex(port.m_queue_ids[queueIndex], queueType, queueRealIndex))
        {
            queueTypeVector.emplace_back(id, queueType);
            queueIndexVector.emplace_back(id, to_string(queueRealIndex));
        }

        queueVector.emplace_back(name.str(), id);
        queuePortVector.emplace_back(id, sai_serialize_object_id(port.m_port_id));

        auto flexCounterOrch = gDirectory.get<FlexCounterOrch*>();
        if (flexCounterOrch->getQueueCountersState())
        {
            // Install a flex counter for this queue to track stats
            addQueueFlexCountersPerPortPerQueueIndex(port, queueIndex, false);
        }
        if (flexCounterOrch->getQueueWatermarkCountersState())
        {
            /* add watermark queue counters */
            addQueueWatermarkFlexCountersPerPortPerQueueIndex(port, queueIndex);
        }
    }

    m_queueTable->set("", queueVector);
    m_queuePortTable->set("", queuePortVector);
    m_queueIndexTable->set("", queueIndexVector);
    m_queueTypeTable->set("", queueTypeVector);

    CounterCheckOrch::getInstance().addPort(port);
}

void PortsOrch::removePortBufferQueueCounters(const Port &port, string queues, bool skip_host_tx_queue)
{
    SWSS_LOG_ENTER();

    /* Remove the Queues maps in the Counter DB */
    /* Remove stat counters from flex_counter DB */
    auto toks = tokenize(queues, '-');
    auto startIndex = to_uint<uint32_t>(toks[0]);
    auto endIndex = startIndex;
    if (toks.size() > 1)
    {
        endIndex = to_uint<uint32_t>(toks[1]);
    }

    for (auto queueIndex = startIndex; queueIndex <= endIndex; queueIndex++)
    {
        if (queueIndex == (uint32_t)port.m_host_tx_queue && skip_host_tx_queue)
        {
            continue;
        }

        std::ostringstream name;
        name << port.m_alias << ":" << queueIndex;
        const auto id = sai_serialize_object_id(port.m_queue_ids[queueIndex]);

        // Remove the queue counter from counters DB maps
        m_queueTable->hdel("", name.str());
        m_queuePortTable->hdel("", id);

        string queueType;
        uint8_t queueRealIndex = 0;
        if (getQueueTypeAndIndex(port.m_queue_ids[queueIndex], queueType, queueRealIndex))
        {
            m_queueTypeTable->hdel("", id);
            m_queueIndexTable->hdel("", id);
        }

        auto flexCounterOrch = gDirectory.get<FlexCounterOrch*>();
        if (flexCounterOrch->getQueueCountersState())
        {
            // Remove the flex counter for this queue
            queue_stat_manager.clearCounterIdList(port.m_queue_ids[queueIndex]);
        }

        if (flexCounterOrch->getQueueWatermarkCountersState())
        {
            // Remove watermark queue counters
            string key = getQueueWatermarkFlexCounterTableKey(id);
            stopFlexCounterPolling(gSwitchId, key);
        }
    }

    CounterCheckOrch::getInstance().removePort(port);
}

void PortsOrch::generatePriorityGroupMap(map<string, FlexCounterPgStates> pgsStateVector)
{
    if (m_isPriorityGroupMapGenerated)
    {
        return;
    }

    bool isCreateAllPgs = false;

    if (pgsStateVector.count(createAllAvailableBuffersStr))
    {
        isCreateAllPgs = true;
        pgsStateVector.clear();
    }

    for (const auto& it: m_portList)
    {
        if (it.second.m_type == Port::PHY)
        {
            if (!pgsStateVector.count(it.second.m_alias))
            {
                auto maxPgNumber = getNumberOfPortSupportedPgCounters(it.second.m_alias);
                FlexCounterPgStates flexCounterPgState(maxPgNumber);
                if (isCreateAllPgs && maxPgNumber)
                {
                    flexCounterPgState.enablePgCounters(0, maxPgNumber - 1);
                }
                pgsStateVector.insert(make_pair(it.second.m_alias, flexCounterPgState));
            }
            generatePriorityGroupMapPerPort(it.second, pgsStateVector.at(it.second.m_alias));
        }
    }

    m_isPriorityGroupMapGenerated = true;
}

void PortsOrch::generatePriorityGroupMapPerPort(const Port& port, FlexCounterPgStates& pgsState)
{
    /* Create the PG map in the Counter DB */
    vector<FieldValueTuple> pgVector;
    vector<FieldValueTuple> pgPortVector;
    vector<FieldValueTuple> pgIndexVector;

    for (size_t pgIndex = 0; pgIndex < port.m_priority_group_ids.size(); ++pgIndex)
    {
        if (!pgsState.isPgCounterEnabled(static_cast<uint32_t>(pgIndex)))
        {
            continue;
        }
        std::ostringstream name;
        name << port.m_alias << ":" << pgIndex;

        const auto id = sai_serialize_object_id(port.m_priority_group_ids[pgIndex]);

        pgVector.emplace_back(name.str(), id);
        pgPortVector.emplace_back(id, sai_serialize_object_id(port.m_port_id));
        pgIndexVector.emplace_back(id, to_string(pgIndex));

    }

    m_pgTable->set("", pgVector);
    m_pgPortTable->set("", pgPortVector);
    m_pgIndexTable->set("", pgIndexVector);

    CounterCheckOrch::getInstance().addPort(port);
}

void PortsOrch::createPortBufferPgCounters(const Port& port, string pgs)
{
    SWSS_LOG_ENTER();

    /* Create the PG map in the Counter DB */
    /* Add stat counters to flex_counter */
    vector<FieldValueTuple> pgVector;
    vector<FieldValueTuple> pgPortVector;
    vector<FieldValueTuple> pgIndexVector;

    auto toks = tokenize(pgs, '-');
    auto startIndex = to_uint<uint32_t>(toks[0]);
    auto endIndex = startIndex;
    if (toks.size() > 1)
    {
        endIndex = to_uint<uint32_t>(toks[1]);
    }

    for (auto pgIndex = startIndex; pgIndex <= endIndex; pgIndex++)
    {
        std::ostringstream name;
        name << port.m_alias << ":" << pgIndex;

        const auto id = sai_serialize_object_id(port.m_priority_group_ids[pgIndex]);

        pgVector.emplace_back(name.str(), id);
        pgPortVector.emplace_back(id, sai_serialize_object_id(port.m_port_id));
        pgIndexVector.emplace_back(id, to_string(pgIndex));

        auto flexCounterOrch = gDirectory.get<FlexCounterOrch*>();
        if (flexCounterOrch->getPgCountersState())
        {
            /* Add dropped packets counters to flex_counter */
            addPriorityGroupFlexCountersPerPortPerPgIndex(port, pgIndex);
        }
        if (flexCounterOrch->getPgWatermarkCountersState())
        {
            /* Add watermark counters to flex_counter */
            addPriorityGroupWatermarkFlexCountersPerPortPerPgIndex(port, pgIndex);
        }
    }

    m_pgTable->set("", pgVector);
    m_pgPortTable->set("", pgPortVector);
    m_pgIndexTable->set("", pgIndexVector);

    CounterCheckOrch::getInstance().addPort(port);
}

void PortsOrch::addPriorityGroupFlexCounters(map<string, FlexCounterPgStates> pgsStateVector)
{
    if (m_isPriorityGroupFlexCountersAdded)
    {
        return;
    }

    bool isCreateAllPgs = false;

    if (pgsStateVector.count(createAllAvailableBuffersStr))
    {
        isCreateAllPgs = true;
        pgsStateVector.clear();
    }

    for (const auto& it: m_portList)
    {
        if (it.second.m_type == Port::PHY)
        {
            if (!pgsStateVector.count(it.second.m_alias))
            {
                auto maxPgNumber = getNumberOfPortSupportedPgCounters(it.second.m_alias);
                FlexCounterPgStates flexCounterPgState(maxPgNumber);
                if (isCreateAllPgs && maxPgNumber)
                {
                    flexCounterPgState.enablePgCounters(0, maxPgNumber - 1);
                }
                pgsStateVector.insert(make_pair(it.second.m_alias, flexCounterPgState));
            }
            addPriorityGroupFlexCountersPerPort(it.second, pgsStateVector.at(it.second.m_alias));
        }
    }

    m_isPriorityGroupFlexCountersAdded = true;
}

void PortsOrch::addPriorityGroupFlexCountersPerPort(const Port& port, FlexCounterPgStates& pgsState)
{
    for (size_t pgIndex = 0; pgIndex < port.m_priority_group_ids.size(); ++pgIndex)
    {
        if (!pgsState.isPgCounterEnabled(static_cast<uint32_t>(pgIndex)))
        {
            continue;
        }
        addPriorityGroupFlexCountersPerPortPerPgIndex(port, pgIndex);
    }
}

void PortsOrch::addPriorityGroupFlexCountersPerPortPerPgIndex(const Port& port, size_t pgIndex)
{
    const auto id = sai_serialize_object_id(port.m_priority_group_ids[pgIndex]);

    string delimiter = "";
    std::ostringstream ingress_pg_drop_packets_counters_stream;
    string key = getPriorityGroupDropPacketsFlexCounterTableKey(id);
    /* Add dropped packets counters to flex_counter */
    for (const auto& it: ingressPriorityGroupDropStatIds)
    {
        ingress_pg_drop_packets_counters_stream << delimiter << sai_serialize_ingress_priority_group_stat(it);
        if (delimiter.empty())
        {
            delimiter = comma;
        }
    }
    auto &&counters_str = ingress_pg_drop_packets_counters_stream.str();
    startFlexCounterPolling(gSwitchId, key, counters_str, PG_COUNTER_ID_LIST);
}

void PortsOrch::addPriorityGroupWatermarkFlexCounters(map<string, FlexCounterPgStates> pgsStateVector)
{
    if (m_isPriorityGroupWatermarkFlexCountersAdded)
    {
        return;
    }

    bool isCreateAllPgs = false;

    if (pgsStateVector.count(createAllAvailableBuffersStr))
    {
        isCreateAllPgs = true;
        pgsStateVector.clear();
    }

    for (const auto& it: m_portList)
    {
        if (it.second.m_type == Port::PHY)
        {
            if (!pgsStateVector.count(it.second.m_alias))
            {
                auto maxPgNumber = getNumberOfPortSupportedPgCounters(it.second.m_alias);
                FlexCounterPgStates flexCounterPgState(maxPgNumber);
                if (isCreateAllPgs && maxPgNumber)
                {
                    flexCounterPgState.enablePgCounters(0, maxPgNumber - 1);
                }
                pgsStateVector.insert(make_pair(it.second.m_alias, flexCounterPgState));
            }
            addPriorityGroupWatermarkFlexCountersPerPort(it.second, pgsStateVector.at(it.second.m_alias));
        }
    }

    m_isPriorityGroupWatermarkFlexCountersAdded = true;
}

void PortsOrch::addPriorityGroupWatermarkFlexCountersPerPort(const Port& port, FlexCounterPgStates& pgsState)
{
    /* Add stat counters to flex_counter */

    for (size_t pgIndex = 0; pgIndex < port.m_priority_group_ids.size(); ++pgIndex)
    {
        if (!pgsState.isPgCounterEnabled(static_cast<uint32_t>(pgIndex)))
        {
            continue;
        }
        addPriorityGroupWatermarkFlexCountersPerPortPerPgIndex(port, pgIndex);
    }
}

void PortsOrch::addPriorityGroupWatermarkFlexCountersPerPortPerPgIndex(const Port& port, size_t pgIndex)
{
    const auto id = sai_serialize_object_id(port.m_priority_group_ids[pgIndex]);

    string key = getPriorityGroupWatermarkFlexCounterTableKey(id);

    std::string delimiter = "";
    std::ostringstream counters_stream;
    /* Add watermark counters to flex_counter */
    for (const auto& it: ingressPriorityGroupWatermarkStatIds)
    {
        counters_stream << delimiter << sai_serialize_ingress_priority_group_stat(it);
        delimiter = comma;
    }

    auto &&counters_str = counters_stream.str();

    startFlexCounterPolling(gSwitchId, key, counters_str, PG_COUNTER_ID_LIST);
}

void PortsOrch::removePortBufferPgCounters(const Port& port, string pgs)
{
    SWSS_LOG_ENTER();

    /* Remove the Pgs maps in the Counter DB */
    /* Remove stat counters from flex_counter DB */
    auto toks = tokenize(pgs, '-');
    auto startIndex = to_uint<uint32_t>(toks[0]);
    auto endIndex = startIndex;
    if (toks.size() > 1)
    {
        endIndex = to_uint<uint32_t>(toks[1]);
    }

    for (auto pgIndex = startIndex; pgIndex <= endIndex; pgIndex++)
    {
        std::ostringstream name;
        name << port.m_alias << ":" << pgIndex;
        const auto id = sai_serialize_object_id(port.m_priority_group_ids[pgIndex]);

        // Remove the pg counter from counters DB maps
        m_pgTable->hdel("", name.str());
        m_pgPortTable->hdel("", id);
        m_pgIndexTable->hdel("", id);

        auto flexCounterOrch = gDirectory.get<FlexCounterOrch*>();
        if (flexCounterOrch->getPgCountersState())
        {
            // Remove dropped packets counters from flex_counter
            string key = getPriorityGroupDropPacketsFlexCounterTableKey(id);
            stopFlexCounterPolling(gSwitchId, key);
        }

        if (flexCounterOrch->getPgWatermarkCountersState())
        {
            // Remove watermark counters from flex_counter
            string key = getPriorityGroupWatermarkFlexCounterTableKey(id);
            stopFlexCounterPolling(gSwitchId, key);
        }
    }

    CounterCheckOrch::getInstance().removePort(port);
}

void PortsOrch::generatePortCounterMap()
{
    if (m_isPortCounterMapGenerated)
    {
        return;
    }

    auto port_counter_stats = generateCounterStats(PORT_STAT_COUNTER_FLEX_COUNTER_GROUP);
    auto gbport_counter_stats = generateCounterStats(PORT_STAT_COUNTER_FLEX_COUNTER_GROUP, true);
    for (const auto& it: m_portList)
    {
        // Set counter stats only for PHY ports to ensure syncd will not try to query the counter statistics from the HW for non-PHY ports.
        if (it.second.m_type != Port::Type::PHY)
        {
            continue;
        }
        port_stat_manager.setCounterIdList(it.second.m_port_id,
                CounterType::PORT, port_counter_stats);
        if (it.second.m_system_side_id)
            gb_port_stat_manager.setCounterIdList(it.second.m_system_side_id,
                    CounterType::PORT, gbport_counter_stats, it.second.m_switch_id);
        if (it.second.m_line_side_id)
            gb_port_stat_manager.setCounterIdList(it.second.m_line_side_id,
                    CounterType::PORT, gbport_counter_stats, it.second.m_switch_id);
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

uint32_t PortsOrch::getNumberOfPortSupportedPgCounters(string port)
{
    return static_cast<uint32_t>(m_portList[port].m_priority_group_ids.size());
}

uint32_t PortsOrch::getNumberOfPortSupportedQueueCounters(string port)
{
    return static_cast<uint32_t>(m_portList[port].m_queue_ids.size());
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

    if (&consumer != m_portStatusNotificationConsumer && &consumer != m_portHostTxReadyNotificationConsumer)
    {
        return;
    }

    if (&consumer == m_portStatusNotificationConsumer && op == "port_state_change")
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
                SWSS_LOG_NOTICE("Got port state change for port id 0x%" PRIx64 " which does not exist, possibly outdated event", id);
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
                sai_port_fec_mode_t fec_mode;
                string fec_str;
                if (oper_fec_sup && getPortOperFec(port, fec_mode))
                {
                    if (!m_portHlpr.fecToStr(fec_str, fec_mode))
                    {
                        SWSS_LOG_ERROR("Error unknown fec mode %d while querying port %s fec mode",
                                       static_cast<std::int32_t>(fec_mode), port.m_alias.c_str());
                        fec_str = "N/A";
                    }
                    updateDbPortOperFec(port,fec_str);
                }
                else
                {
                    updateDbPortOperFec(port, "N/A");
                }
            }

            /* update m_portList */
            m_portList[port.m_alias] = port;
        }

        sai_deserialize_free_port_oper_status_ntf(count, portoperstatus);
    }
    else if (&consumer == m_portHostTxReadyNotificationConsumer && op == "port_host_tx_ready")
    {
        sai_object_id_t port_id;
        sai_object_id_t switch_id;
        sai_port_host_tx_ready_status_t host_tx_ready_status;

        sai_deserialize_port_host_tx_ready_ntf(data, switch_id, port_id, host_tx_ready_status);
        SWSS_LOG_DEBUG("Recieved host_tx_ready notification for port 0x%" PRIx64, port_id);

        Port p;
        if (!getPort(port_id, p))
        {
            SWSS_LOG_ERROR("Failed to get port object for port id 0x%" PRIx64, port_id);
            return;
        }
        setHostTxReady(p, host_tx_ready_status == SAI_PORT_HOST_TX_READY_STATUS_READY ? "true" : "false");
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
        updateDbPortFlapCount(port, status);
        updateGearboxPortOperStatus(port);

        /* Refresh the port states and reschedule the poller tasks */
        if (port.m_autoneg > 0)
        {
            refreshPortStateAutoNeg(port);
            updatePortStatePoll(port, PORT_STATE_POLL_AN, !(status == SAI_PORT_OPER_STATUS_UP));
        }
        if (port.m_link_training > 0)
        {
            refreshPortStateLinkTraining(port);
            updatePortStatePoll(port, PORT_STATE_POLL_LT, !(status == SAI_PORT_OPER_STATUS_UP));
        }
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
    SWSS_LOG_INFO("Updating the nexthop for port %s and operational status %s", port.m_alias.c_str(), isUp ? "up" : "down");

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

    if(gMySwitchType == "voq")
    {
        if (gIntfsOrch->isLocalSystemPortIntf(port.m_alias))
        {
            gIntfsOrch->voqSyncIntfState(port.m_alias, isUp);
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

void PortsOrch::updateDbPortOperFec(Port &port, string fec_str)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> tuples;
    tuples.emplace_back(std::make_pair("fec", fec_str));
    m_portStateTable.set(port.m_alias, tuples);

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
            sai_port_fec_mode_t fec_mode;
            string fec_str = "N/A";
            if (oper_fec_sup && getPortOperFec(port, fec_mode))
            {
                if (!m_portHlpr.fecToStr(fec_str, fec_mode))
                {
                    SWSS_LOG_ERROR("Error unknown fec mode %d while querying port %s fec mode",
                                   static_cast<std::int32_t>(fec_mode), port.m_alias.c_str());
                    fec_str = "N/A";
                }
            }
            updateDbPortOperFec(port,fec_str);
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

bool PortsOrch::getPortOperFec(const Port& port, sai_port_fec_mode_t &fec_mode) const
{
    SWSS_LOG_ENTER();

    if (port.m_type != Port::PHY)
    {
        return false;
    }

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_OPER_PORT_FEC_MODE;

    sai_status_t ret = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (ret != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_NOTICE("Failed to get oper fec for %s", port.m_alias.c_str());
        return false;
    }

    fec_mode = static_cast<sai_port_fec_mode_t>(attr.value.s32);
    return true;
}
bool PortsOrch::getPortLinkTrainingRxStatus(const Port &port, sai_port_link_training_rx_status_t &rx_status)
{
    SWSS_LOG_ENTER();

    if (port.m_type != Port::PHY)
    {
        return false;
    }

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_LINK_TRAINING_RX_STATUS;
    sai_status_t ret = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (ret != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get LT rx status for %s", port.m_alias.c_str());
        return false;
    }

    rx_status = static_cast<sai_port_link_training_rx_status_t>(attr.value.u32);
    return true;
}

bool PortsOrch::getPortLinkTrainingFailure(const Port &port, sai_port_link_training_failure_status_t &failure)
{
    SWSS_LOG_ENTER();

    if (port.m_type != Port::PHY)
    {
        return false;
    }

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_LINK_TRAINING_FAILURE_STATUS;
    sai_status_t ret = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (ret != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get LT failure status for %s", port.m_alias.c_str());
        return false;
    }

    failure = static_cast<sai_port_link_training_failure_status_t>(attr.value.u32);
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

bool PortsOrch::setPortSerdesAttribute(sai_object_id_t port_id, sai_object_id_t switch_id,
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
    status = sai_port_api->create_port_serdes(&port_serdes_id, switch_id,
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
                                 std::vector<uint32_t> &lane_values,
                                 int base)
{
    SWSS_LOG_ENTER();

    uint32_t lane_val;
    std::string lane_str;
    std::istringstream iss(val_str);

    while (std::getline(iss, lane_str, ','))
    {
        lane_val = (uint32_t)std::stoul(lane_str, NULL, base);
        lane_values.push_back(lane_val);
    }
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

        m_gb_counter_db = shared_ptr<DBConnector>(new DBConnector("GB_COUNTERS_DB", 0));
        m_gbcounterTable = unique_ptr<Table>(new Table(m_gb_counter_db.get(), COUNTERS_PORT_NAME_MAP));
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
    sai_port_fec_mode_t sai_fec;

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
            if (!m_portHlpr.fecToSaiFecMode(m_gearboxPortMap[port.m_index].system_fec, sai_fec))
            {
                SWSS_LOG_ERROR("Invalid system FEC mode %s", m_gearboxPortMap[port.m_index].system_fec.c_str());
                return false;
            }
            attr.value.s32 = sai_fec;
            attrs.push_back(attr);

            if (fec_override_sup)
            {
                attr.id = SAI_PORT_ATTR_AUTO_NEG_FEC_MODE_OVERRIDE;

                attr.value.booldata = m_portHlpr.fecIsOverrideRequired(m_gearboxPortMap[port.m_index].system_fec);
                attrs.push_back(attr);
            }

            attr.id = SAI_PORT_ATTR_INTERNAL_LOOPBACK_MODE;
            attr.value.u32 = loopback_mode_map[m_gearboxPortMap[port.m_index].system_loopback];
            attrs.push_back(attr);

            attr.id = SAI_PORT_ATTR_LINK_TRAINING_ENABLE;
            attr.value.booldata = m_gearboxPortMap[port.m_index].system_training;
            attrs.push_back(attr);

            if (m_cmisModuleAsicSyncSupported)
            {
                attr.id = SAI_PORT_ATTR_HOST_TX_SIGNAL_ENABLE;
                attr.value.booldata = false;
                attrs.push_back(attr);
            }

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
            port.m_system_side_id = systemPort;

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
            if (!m_portHlpr.fecToSaiFecMode(m_gearboxPortMap[port.m_index].line_fec, sai_fec))
            {
                SWSS_LOG_ERROR("Invalid line FEC mode %s", m_gearboxPortMap[port.m_index].line_fec.c_str());
                return false;
            }
            attr.value.s32 = sai_fec;
            attrs.push_back(attr);

            // FEC override will take effect only when autoneg is enabled
            if (fec_override_sup)
            {
                attr.id = SAI_PORT_ATTR_AUTO_NEG_FEC_MODE_OVERRIDE;
                attr.value.booldata = m_portHlpr.fecIsOverrideRequired(m_gearboxPortMap[port.m_index].line_fec);
                attrs.push_back(attr);
            }

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

            if (m_cmisModuleAsicSyncSupported)
            {
                attr.id = SAI_PORT_ATTR_HOST_TX_SIGNAL_ENABLE;
                attr.value.booldata = false;
                attrs.push_back(attr);
            }

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
            saiOidToAlias[systemPort] = port.m_alias;
            saiOidToAlias[linePort] = port.m_alias;

            /* Add gearbox system/line port name map to counter table */
            FieldValueTuple tuple(port.m_alias + "_system", sai_serialize_object_id(systemPort));
            vector<FieldValueTuple> fields;
            fields.push_back(tuple);
            m_gbcounterTable->set("", fields);

            fields[0] = FieldValueTuple(port.m_alias + "_line", sai_serialize_object_id(linePort));
            m_gbcounterTable->set("", fields);

            /* Set serdes tx taps on system and line side */
            map<sai_port_serdes_attr_t, vector<uint32_t>> serdes_attr;
            typedef pair<sai_port_serdes_attr_t, vector<uint32_t>> serdes_attr_pair;
            vector<uint32_t> attr_val;
            for (auto pair: tx_fir_strings_system_side) {
                if (m_gearboxInterfaceMap[port.m_index].tx_firs.find(pair.first) != m_gearboxInterfaceMap[port.m_index].tx_firs.end() ) {
                    attr_val.clear();
                    getPortSerdesVal(m_gearboxInterfaceMap[port.m_index].tx_firs[pair.first], attr_val, 10);
                    serdes_attr.insert(serdes_attr_pair(pair.second, attr_val));
                }
            }
            if (serdes_attr.size() != 0)
            {
                if (setPortSerdesAttribute(systemPort, phyOid, serdes_attr))
                {
                    SWSS_LOG_NOTICE("Set port %s system side preemphasis is success", port.m_alias.c_str());
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to set port %s system side pre-emphasis", port.m_alias.c_str());
                    return false;
                }
            }
            serdes_attr.clear();
            for (auto pair: tx_fir_strings_line_side) {
                if (m_gearboxInterfaceMap[port.m_index].tx_firs.find(pair.first) != m_gearboxInterfaceMap[port.m_index].tx_firs.end() ) {
                    attr_val.clear();
                    getPortSerdesVal(m_gearboxInterfaceMap[port.m_index].tx_firs[pair.first], attr_val, 10);
                    serdes_attr.insert(serdes_attr_pair(pair.second, attr_val));
                }
            }
            if (serdes_attr.size() != 0)
            {
                if (setPortSerdesAttribute(linePort, phyOid, serdes_attr))
                {
                    SWSS_LOG_NOTICE("Set port %s line side preemphasis is success", port.m_alias.c_str());
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to set port %s line side pre-emphasis", port.m_alias.c_str());
                    return false;
                }
            }
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

bool PortsOrch::getRecircPort(Port &port, Port::Role role)
{
    for (auto it = m_recircPortRole.begin(); it != m_recircPortRole.end(); it++)
    {
        if (it->second == role)
        {
            return getPort(it->first, port);
        }
    }

    SWSS_LOG_ERROR(
        "Failed to find recirc port %s with role %d",
        port.m_alias.c_str(), static_cast<std::int32_t>(role)
    );

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

            initializeVoqs( port );
            setPort(port.m_alias, port);
            /* Add system port name map to counter table */
            FieldValueTuple tuple(port.m_system_port_info.alias,
                                  sai_serialize_object_id(system_port_oid));
            vector<FieldValueTuple> fields;
            fields.push_back(tuple);
            m_counterSysPortTable->set("", fields);
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

void PortsOrch::voqSyncAddLagMember(Port &lag, Port &port, string status)
{
    // Sync only local lag's member add to CHASSIS_APP_DB
    if (lag.m_system_lag_info.switch_id != gVoqMySwitchId)
    {
        return;
    }

    vector<FieldValueTuple> attrs;
    FieldValueTuple statusFv ("status", status);
    attrs.push_back(statusFv);

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

std::unordered_set<std::string> PortsOrch::generateCounterStats(const string& type, bool gearbox)
{
    std::unordered_set<std::string> counter_stats;
    if (type == PORT_STAT_COUNTER_FLEX_COUNTER_GROUP)
    {
        auto& stat_ids = gearbox ? gbport_stat_ids : port_stat_ids;
        for (const auto& it: stat_ids)
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

void PortsOrch::updateGearboxPortOperStatus(const Port& port)
{
    if (!isGearboxEnabled())
        return;

    SWSS_LOG_NOTICE("BOX: port %s, system_side_id:0x%" PRIx64 "line_side_id:0x%" PRIx64,
            port.m_alias.c_str(), port.m_system_side_id, port.m_line_side_id);

    if (!port.m_system_side_id || !port.m_line_side_id)
        return;

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_OPER_STATUS;
    sai_status_t ret = sai_port_api->get_port_attribute(port.m_system_side_id, 1, &attr);
    if (ret != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("BOX: Failed to get system_oper_status for %s", port.m_alias.c_str());
    }
    else
    {
        sai_port_oper_status_t oper = static_cast<sai_port_oper_status_t>(attr.value.u32);
        vector<FieldValueTuple> tuples;
        FieldValueTuple tuple("system_oper_status", oper_status_strings.at(oper));
        tuples.push_back(tuple);
        m_portTable->set(port.m_alias, tuples);
    }

    attr.id = SAI_PORT_ATTR_OPER_STATUS;
    ret = sai_port_api->get_port_attribute(port.m_line_side_id, 1, &attr);
    if (ret != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("BOX: Failed to get line_oper_status for %s", port.m_alias.c_str());
    }
    else
    {
        sai_port_oper_status_t oper = static_cast<sai_port_oper_status_t>(attr.value.u32);
        vector<FieldValueTuple> tuples;
        FieldValueTuple tuple("line_oper_status", oper_status_strings.at(oper));
        tuples.push_back(tuple);
        m_portTable->set(port.m_alias, tuples);
    }
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

void PortsOrch::setMACsecEnabledState(sai_object_id_t port_id, bool enabled)
{
    SWSS_LOG_ENTER();

    Port p;
    if (!getPort(port_id, p))
    {
        SWSS_LOG_ERROR("Failed to get port object for port id 0x%" PRIx64, port_id);
        return;
    }

    if (enabled)
    {
        m_macsecEnabledPorts.insert(port_id);
    }
    else
    {
        m_macsecEnabledPorts.erase(port_id);
    }

    if (p.m_mtu)
    {
        setPortMtu(p, p.m_mtu);
    }
}

bool PortsOrch::isMACsecPort(sai_object_id_t port_id) const
{
    SWSS_LOG_ENTER();

    return m_macsecEnabledPorts.find(port_id) != m_macsecEnabledPorts.end();
}

vector<sai_object_id_t> PortsOrch::getPortVoQIds(Port& port)
{
    SWSS_LOG_ENTER();

    return m_port_voq_ids[port.m_alias];
}

/* Refresh the per-port Auto-Negotiation operational states */
void PortsOrch::refreshPortStateAutoNeg(const Port &port)
{
    SWSS_LOG_ENTER();

    if (port.m_type != Port::Type::PHY)
    {
        return;
    }

    string adv_speeds = "N/A";

    if (port.m_admin_state_up)
    {
        if (!getPortAdvSpeeds(port, true, adv_speeds))
        {
            adv_speeds = "N/A";
            updatePortStatePoll(port, PORT_STATE_POLL_AN, false);
        }
    }

    m_portStateTable.hset(port.m_alias, "rmt_adv_speeds", adv_speeds);
}

/* Refresh the per-port Link-Training operational states */
void PortsOrch::refreshPortStateLinkTraining(const Port &port)
{
    SWSS_LOG_ENTER();

    if (port.m_type != Port::Type::PHY)
    {
        return;
    }

    string status = "off";

    if (port.m_admin_state_up && port.m_link_training > 0 && port.m_cap_lt > 0)
    {
        sai_port_link_training_rx_status_t rx_status;
        sai_port_link_training_failure_status_t failure;

        if (!getPortLinkTrainingRxStatus(port, rx_status))
        {
            status = "on"; // LT is enabled, while the rx status is unavailable
        }
        else if (rx_status == SAI_PORT_LINK_TRAINING_RX_STATUS_TRAINED)
        {
            status = link_training_rx_status_map.at(rx_status);
        }
        else
        {
            if (getPortLinkTrainingFailure(port, failure) &&
                failure != SAI_PORT_LINK_TRAINING_FAILURE_STATUS_NO_ERROR)
            {
                status = link_training_failure_map.at(failure);
            }
            else
            {
                status = link_training_rx_status_map.at(rx_status);
            }
        }
    }

    m_portStateTable.hset(port.m_alias, "link_training_status", status);
}

/* Activate/De-activate a specific port state poller task */
void PortsOrch::updatePortStatePoll(const Port &port, port_state_poll_t type, bool active)
{
    if (type == PORT_STATE_POLL_NONE)
    {
        return;
    }
    if (active)
    {
        m_port_state_poll[port.m_alias] |= type;
        m_port_state_poller->start();
    }
    else
    {
        m_port_state_poll[port.m_alias] &= ~type;
    }
}

bool PortsOrch::createAndSetPortPtTam(const Port &p)
{
    /*
     * First, let's check if a TAM object is already assigned to the port.
     */

    /* If the port has already a TAM object, nothing to do */
    if (m_portPtTam.find(p.m_alias) != m_portPtTam.end())
    {
        SWSS_LOG_DEBUG(
            "Port %s has already a TAM object", p.m_alias.c_str()
        );
        return true;
    }

    /*
     * The port does not have a TAM object assigned to it.
     *
     * Let's create a new TAM object (if we don't already have one)
     * and assign it to the port.
     */
    if (m_ptTam == SAI_NULL_OBJECT_ID)
    {
        if (!createPtTam())
        {
            SWSS_LOG_ERROR(
                "Failed to create TAM object for Path Tracing"
            );
            return false;
        }
    }

    if (!setPortPtTam(p, m_ptTam))
    {
        SWSS_LOG_ERROR(
            "Failed to set port %s TAM object for Path Tracing",
            p.m_alias.c_str()
        );
        return false;
    }

    m_ptTamRefCount++;
    m_portPtTam[p.m_alias] = m_ptTam;

    return true;
}

bool PortsOrch::unsetPortPtTam(const Port &p)
{
    /*
     * Let's unassign the TAM object from the port and decrease ref counter
     */
    if (m_portPtTam.find(p.m_alias) != m_portPtTam.end())
    {
        if (!setPortPtTam(p, SAI_NULL_OBJECT_ID))
        {
            SWSS_LOG_ERROR(
                "Failed to unset port %s TAM object for Path Tracing",
                p.m_alias.c_str()
            );
            return false;
        }
        m_ptTamRefCount--;
        m_portPtTam.erase(p.m_alias);

        /*
         * If the TAM object is no longer used, we can safely remove it.
         */
        if (m_ptTamRefCount == 0)
        {
            if (!removePtTam(m_ptTam))
            {
                SWSS_LOG_ERROR(
                    "Failed to remove TAM object for Path Tracing"
                );
                return false;
            }
        }
    }

    return true;
}

bool PortsOrch::setPortPtIntfId(const Port& port, sai_uint16_t intf_id)
{
    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_PATH_TRACING_INTF;
    attr.value.u16 = intf_id;

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);

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

bool PortsOrch::setPortPtTimestampTemplate(const Port& port, sai_port_path_tracing_timestamp_type_t ts_type)
{
    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_PATH_TRACING_TIMESTAMP_TYPE;
    attr.value.s32 = ts_type;

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);

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

bool PortsOrch::setPortPtTam(const Port& port, sai_object_id_t tam_id)
{
    sai_attribute_t attr;

    attr.id = SAI_PORT_ATTR_TAM_OBJECT;

    if (tam_id != SAI_NULL_OBJECT_ID)
    {
        attr.value.objlist.count = 1;
        attr.value.objlist.list = &tam_id;
    }
    else
    {
        attr.value.objlist.count = 0;
    }

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);

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

bool PortsOrch::createPtTam()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;
    sai_status_t status;

    /* First, create a TAM report */
    if (m_ptTamReport == SAI_NULL_OBJECT_ID)
    {
        sai_object_id_t tam_report_id;

        attr.id = SAI_TAM_REPORT_ATTR_TYPE;
        attr.value.s32 = SAI_TAM_REPORT_TYPE_VENDOR_EXTN;
        attrs.push_back(attr);

        status = sai_tam_api->create_tam_report(&tam_report_id, gSwitchId, static_cast<uint32_t>(attrs.size()), attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create TAM Report object for Path Tracing, rv:%d", status);
            task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }

        m_ptTamReport = tam_report_id;
        SWSS_LOG_NOTICE("Created TAM Report object %" PRIx64 " for Path Tracing", tam_report_id);
    }

    /* Second, create a TAM INT object */
    if (m_ptTamInt == SAI_NULL_OBJECT_ID)
    {
        sai_object_id_t tam_int_id;

        attrs.clear();

        attr.id = SAI_TAM_INT_ATTR_TYPE;
        attr.value.s32 = SAI_TAM_INT_TYPE_PATH_TRACING;
        attrs.push_back(attr);

        attr.id = SAI_TAM_INT_ATTR_DEVICE_ID;
        attr.value.u32 = 0;
        attrs.push_back(attr);

        attr.id = SAI_TAM_INT_ATTR_INT_PRESENCE_TYPE;
        attr.value.u32 = SAI_TAM_INT_PRESENCE_TYPE_UNDEFINED;
        attrs.push_back(attr);

        attr.id = SAI_TAM_INT_ATTR_INLINE;
        attr.value.u32 = false;
        attrs.push_back(attr);

        attr.id = SAI_TAM_INT_ATTR_REPORT_ID;
        attr.value.oid = m_ptTamReport;
        attrs.push_back(attr);

        status = sai_tam_api->create_tam_int(&tam_int_id, gSwitchId, static_cast<uint32_t>(attrs.size()), attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create TAM INT object for Path Tracing, rv:%d", status);
            task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }

        m_ptTamInt = tam_int_id;
        SWSS_LOG_NOTICE("Created TAM INT object %" PRIx64 " for Path Tracing", tam_int_id);
    }

    /* Finally, create a TAM object */
    if (m_ptTam == SAI_NULL_OBJECT_ID)
    {
        sai_object_id_t tam_id;

        attrs.clear();

        attr.id = SAI_TAM_ATTR_INT_OBJECTS_LIST;
        attr.value.objlist.count = 1;
        attr.value.objlist.list = &m_ptTamInt;
        attrs.push_back(attr);

        status = sai_tam_api->create_tam(&tam_id, gSwitchId, static_cast<uint32_t>(attrs.size()), attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create TAM object for Path Tracing, rv:%d", status);
            task_process_status handle_status = handleSaiCreateStatus(SAI_API_TAM, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }

        m_ptTam = tam_id;
        SWSS_LOG_NOTICE("Created TAM object %" PRIx64 " for Path Tracing", tam_id);
    }

    return true;
}

bool PortsOrch::removePtTam(sai_object_id_t tam_id)
{
    SWSS_LOG_ENTER();

    sai_status_t status;

    if (m_ptTam != SAI_NULL_OBJECT_ID)
    {
        status = sai_tam_api->remove_tam(m_ptTam);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove TAM object for Path Tracing, rv:%d", status);
            return false;
        }

        SWSS_LOG_NOTICE("Removed TAM %" PRIx64, m_ptTam);
        m_ptTam = SAI_NULL_OBJECT_ID;
    }

    if (m_ptTamInt != SAI_NULL_OBJECT_ID)
    {
        status = sai_tam_api->remove_tam_int(m_ptTamInt);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove TAM INT object for Path Tracing, rv:%d", status);
            return false;
        }

        SWSS_LOG_NOTICE("Removed TAM INT %" PRIx64, m_ptTamInt);
        m_ptTamInt = SAI_NULL_OBJECT_ID;
    }

    if (m_ptTamReport != SAI_NULL_OBJECT_ID)
    {
        status = sai_tam_api->remove_tam_report(m_ptTamReport);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove TAM Report for Path Tracing, rv:%d", status);
            return false;
        }

        SWSS_LOG_NOTICE("Removed TAM Report %" PRIx64, m_ptTamReport);
        m_ptTamReport = SAI_NULL_OBJECT_ID;
    }

    return true;
}

void PortsOrch::doTask(swss::SelectableTimer &timer)
{
    Port port;

    for (auto it = m_port_state_poll.begin(); it != m_port_state_poll.end(); )
    {
        if ((it->second == PORT_STATE_POLL_NONE) || !getPort(it->first, port))
        {
            it = m_port_state_poll.erase(it);
            continue;
        }
        if (!port.m_admin_state_up)
        {
            ++it;
            continue;
        }
        if (it->second & PORT_STATE_POLL_AN)
        {
            refreshPortStateAutoNeg(port);
        }
        if (it->second & PORT_STATE_POLL_LT)
        {
            refreshPortStateLinkTraining(port);
        }
        ++it;
    }
    if (m_port_state_poll.size() == 0)
    {
        m_port_state_poller->stop();
    }
}

