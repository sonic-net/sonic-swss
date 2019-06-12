#include "portsorch.h"
#include "intfsorch.h"
#include "bufferorch.h"
#include "neighorch.h"

#include <cassert>
#include <fstream>
#include <sstream>
#include <set>
#include <algorithm>
#include <tuple>
#include <sstream>
#include <string>
#include <ctime>

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

extern sai_switch_api_t *sai_switch_api;
extern sai_bridge_api_t *sai_bridge_api;
extern sai_port_api_t *sai_port_api;
extern sai_vlan_api_t *sai_vlan_api;
extern sai_lag_api_t *sai_lag_api;
extern sai_hostif_api_t* sai_hostif_api;
extern sai_acl_api_t* sai_acl_api;
extern sai_queue_api_t *sai_queue_api;
extern sai_object_id_t gSwitchId;
extern IntfsOrch *gIntfsOrch;
extern NeighOrch *gNeighOrch;
extern CrmOrch *gCrmOrch;
extern BufferOrch *gBufferOrch;

#define VLAN_PREFIX         "Vlan"
#define DEFAULT_VLAN_ID     1
#define PORT_FLEX_STAT_COUNTER_POLL_MSECS "1000"
#define QUEUE_FLEX_STAT_COUNTER_POLL_MSECS "10000"
#define QUEUE_WATERMARK_FLEX_STAT_COUNTER_POLL_MSECS "10000"
#define PG_WATERMARK_FLEX_STAT_COUNTER_POLL_MSECS "10000"


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

const vector<sai_port_stat_t> portStatIds =
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
    SAI_PORT_STAT_ETHER_IN_PKTS_128_TO_255_OCTETS,
};

static const vector<sai_queue_stat_t> queueStatIds =
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

static char* hostif_vlan_tag[] = {
    [SAI_HOSTIF_VLAN_TAG_STRIP]     = "SAI_HOSTIF_VLAN_TAG_STRIP",
    [SAI_HOSTIF_VLAN_TAG_KEEP]      = "SAI_HOSTIF_VLAN_TAG_KEEP",
    [SAI_HOSTIF_VLAN_TAG_ORIGINAL]  = "SAI_HOSTIF_VLAN_TAG_ORIGINAL"
};
/*
 * Initialize PortsOrch
 * 0) By default, a switch has one CPU port, one 802.1Q bridge, and one default
 *    VLAN. All ports are in .1Q bridge as bridge ports, and all bridge ports
 *    are in default VLAN as VLAN members.
 * 1) Query switch CPU port.
 * 2) Query ports associated with lane mappings
 * 3) Query switch .1Q bridge and all its bridge ports.
 * 4) Query switch default VLAN and all its VLAN members.
 * 5) Remove each VLAN member from default VLAN and each bridge port from .1Q
 *    bridge. By design, SONiC switch starts with all bridge ports removed from
 *    default VLAN and all ports removed from .1Q bridge.
 */
PortsOrch::PortsOrch(DBConnector *db, vector<table_name_with_pri_t> &tableNames) :
        Orch(db, tableNames)
{
    SWSS_LOG_ENTER();

    /* Initialize counter table */
    m_counter_db = shared_ptr<DBConnector>(new DBConnector(COUNTERS_DB, DBConnector::DEFAULT_UNIXSOCKET, 0));
    m_counterTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_PORT_NAME_MAP));

    /* Initialize port table */
    m_portTable = unique_ptr<Table>(new Table(db, APP_PORT_TABLE_NAME));

    /* Initialize queue tables */
    m_queueTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_QUEUE_NAME_MAP));
    m_queuePortTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_QUEUE_PORT_MAP));
    m_queueIndexTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_QUEUE_INDEX_MAP));
    m_queueTypeTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_QUEUE_TYPE_MAP));

    /* Initialize ingress priority group tables */
    m_pgTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_PG_NAME_MAP));
    m_pgPortTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_PG_PORT_MAP));
    m_pgIndexTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_PG_INDEX_MAP));

    m_flex_db = shared_ptr<DBConnector>(new DBConnector(FLEX_COUNTER_DB, DBConnector::DEFAULT_UNIXSOCKET, 0));
    m_flexCounterTable = unique_ptr<ProducerTable>(new ProducerTable(m_flex_db.get(), FLEX_COUNTER_TABLE));
    m_flexCounterGroupTable = unique_ptr<ProducerTable>(new ProducerTable(m_flex_db.get(), FLEX_COUNTER_GROUP_TABLE));

    vector<FieldValueTuple> fields;
    fields.emplace_back(POLL_INTERVAL_FIELD, PORT_FLEX_STAT_COUNTER_POLL_MSECS);
    fields.emplace_back(STATS_MODE_FIELD, STATS_MODE_READ);
    m_flexCounterGroupTable->set(PORT_STAT_COUNTER_FLEX_COUNTER_GROUP, fields);

    fields.emplace_back(POLL_INTERVAL_FIELD, QUEUE_FLEX_STAT_COUNTER_POLL_MSECS);
    fields.emplace_back(STATS_MODE_FIELD, STATS_MODE_READ);
    m_flexCounterGroupTable->set(QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP, fields);

    string queueWmSha, pgWmSha;
    string queueWmPluginName = "watermark_queue.lua";
    string pgWmPluginName = "watermark_pg.lua";

    try
    {
        string queueLuaScript = swss::loadLuaScript(queueWmPluginName);
        queueWmSha = swss::loadRedisScript(m_counter_db.get(), queueLuaScript);

        string pgLuaScript = swss::loadLuaScript(pgWmPluginName);
        pgWmSha = swss::loadRedisScript(m_counter_db.get(), pgLuaScript);

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
    }
    catch (...)
    {
        SWSS_LOG_WARN("Watermark flex counter groups were not set successfully");
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
        throw runtime_error("PortsOrch initialization failure");
    }

    m_cpuPort = Port("CPU", Port::CPU);
    m_cpuPort.m_port_id = attr.value.oid;
    m_portList[m_cpuPort.m_alias] = m_cpuPort;

    /* Get port number */
    attr.id = SAI_SWITCH_ATTR_PORT_NUMBER;

    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get port number, rv:%d", status);
        throw runtime_error("PortsOrch initialization failure");
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
        throw runtime_error("PortsOrch initialization failure");
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
            SWSS_LOG_ERROR("Failed to get hardware lane list pid:%lx", port_list[i]);
            throw runtime_error("PortsOrch initialization failure");
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

        SWSS_LOG_NOTICE("Get port with lanes pid:%lx lanes:%s", port_list[i], tmp_lane_str.c_str());
        m_portListLaneMap[tmp_lane_set] = port_list[i];
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
        throw runtime_error("PortsOrch initialization failure");
    }

    m_default1QBridge = attrs[0].value.oid;
    m_defaultVlan = attrs[1].value.oid;

    removeDefaultVlanMembers();
    removeDefaultBridgePorts();

    /* Add port oper status notification support */
    DBConnector *notificationsDb = new DBConnector(ASIC_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    m_portStatusNotificationConsumer = new swss::NotificationConsumer(notificationsDb, "NOTIFICATIONS");
    auto portStatusNotificatier = new Notifier(m_portStatusNotificationConsumer, this, "PORT_STATUS_NOTIFICATIONS");
    Orch::addExecutor(portStatusNotificatier);
}

void PortsOrch::removeDefaultVlanMembers()
{
    /* Get VLAN members in default VLAN */
    vector<sai_object_id_t> vlan_member_list(m_portCount);

    sai_attribute_t attr;
    attr.id = SAI_VLAN_ATTR_MEMBER_LIST;
    attr.value.objlist.count = (uint32_t)vlan_member_list.size();
    attr.value.objlist.list = vlan_member_list.data();

    sai_status_t status = sai_vlan_api->get_vlan_attribute(m_defaultVlan, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get VLAN member list in default VLAN, rv:%d", status);
        throw runtime_error("PortsOrch initialization failure");
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
     * By default, there will be m_portCount number of SAI_BRIDGE_PORT_TYPE_PORT
     * ports and one SAI_BRIDGE_PORT_TYPE_1Q_ROUTER port. The former type of
     * ports will be removed. */
    vector<sai_object_id_t> bridge_port_list(m_portCount + 1);

    sai_attribute_t attr;
    attr.id = SAI_BRIDGE_ATTR_PORT_LIST;
    attr.value.objlist.count = (uint32_t)bridge_port_list.size();
    attr.value.objlist.list = bridge_port_list.data();

    sai_status_t status = sai_bridge_api->get_bridge_attribute(m_default1QBridge, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get bridge port list in default 1Q bridge, rv:%d", status);
        throw runtime_error("PortsOrch initialization failure");
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
            throw runtime_error("PortsOrch initialization failure");
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

bool PortsOrch::isPortReady()
{
    return m_initDone && m_pendingPortSet.empty();
}

/* Upon receiving PortInitDone, all the configured ports have been created*/
bool PortsOrch::isInitDone()
{
    return m_initDone;
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

    for (const auto& portIter: m_portList)
    {
        switch (portIter.second.m_type)
        {
        case Port::PHY:
            if(portIter.second.m_port_id == id)
            {
                port = portIter.second;
                return true;
            }
            break;
        case Port::LAG:
            if(portIter.second.m_lag_id == id)
            {
                port = portIter.second;
                return true;
            }
            break;
        case Port::VLAN:
            if (portIter.second.m_vlan_info.vlan_oid == id)
            {
                port = portIter.second;
                return true;
            }
            break;
        default:
            continue;
        }
    }

    return false;
}

bool PortsOrch::getPortByBridgePortId(sai_object_id_t bridge_port_id, Port &port)
{
    SWSS_LOG_ENTER();

    for (auto &it: m_portList)
    {
        if (it.second.m_bridge_port_id == bridge_port_id)
        {
            port = it.second;
            return true;
        }
    }

    return false;
}

// TODO: move this into AclOrch
bool PortsOrch::getAclBindPortId(string alias, sai_object_id_t &port_id)
{
    SWSS_LOG_ENTER();

    Port port;
    if (getPort(alias, port))
    {
        switch (port.m_type)
        {
        case Port::PHY:
            if (port.m_lag_member_id != SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_WARN("Invalid configuration. Bind table to LAG member %s is not allowed", alias.c_str());
                return false;
            }
            else
            {
                port_id = port.m_port_id;
            }
            break;
        case Port::LAG:
            port_id = port.m_lag_id;
            break;
        case Port::VLAN:
            port_id = port.m_vlan_info.vlan_oid;
            break;
        default:
            SWSS_LOG_ERROR("Failed to process port. Incorrect port %s type %d", alias.c_str(), port.m_type);
            return false;
        }

        return true;
    }
    else
    {
        return false;
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

bool PortsOrch::setPortAdminStatus(sai_object_id_t id, bool up)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_ADMIN_STATE;
    attr.value.booldata = up;

    sai_status_t status = sai_port_api->set_port_attribute(id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set admin status %s to port pid:%lx",
                       up ? "UP" : "DOWN", id);
        return false;
    }
    SWSS_LOG_INFO("Set admin status %s to port pid:%lx",
                    up ? "UP" : "DOWN", id);
    return true;
}

bool PortsOrch::getPortAdminStatus(sai_object_id_t id, bool &up)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_ADMIN_STATE;

    sai_status_t status = sai_port_api->get_port_attribute(id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get admin status for port pid:%lx", id);
        return false;
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
        SWSS_LOG_ERROR("Failed to set MTU %u to port pid:%lx, rv:%d",
                attr.value.u32, id, status);
        return false;
    }
    SWSS_LOG_INFO("Set MTU %u to port pid:%lx", attr.value.u32, id);
    return true;
}

bool PortsOrch::setPortFec(sai_object_id_t id, sai_port_fec_mode_t mode)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_FEC_MODE;
    attr.value.s32 = mode;

    sai_status_t status = sai_port_api->set_port_attribute(id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set fec mode %d to port pid:%lx",
                       mode, id);
        return false;
    }
    SWSS_LOG_INFO("Set fec mode %d to port pid:%lx",
                       mode, id);
    return true;
}

bool PortsOrch::getPortPfc(sai_object_id_t portId, uint8_t *pfc_bitmask)
{
    SWSS_LOG_ENTER();

    Port p;

    if (!getPort(portId, p))
    {
        SWSS_LOG_ERROR("Failed to get port object for port id 0x%lx", portId);
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
        SWSS_LOG_ERROR("Failed to get port object for port id 0x%lx", portId);
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
        SWSS_LOG_ERROR("Failed to set PFC 0x%x to port id 0x%lx (rc:%d)", attr.value.u8, portId, status);
        return false;
    }

    if (p.m_pfc_bitmask != pfc_bitmask)
    {
        p.m_pfc_bitmask = pfc_bitmask;
        m_portList[p.m_alias] = p;
    }

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
        SWSS_LOG_ERROR("Failed to set PFC mode %d to port id 0x%lx (rc:%d)", port.m_pfc_asym, port.m_port_id, status);
        return false;
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
            SWSS_LOG_ERROR("Failed to set RX PFC 0x%x to port id 0x%lx (rc:%d)", attr.value.u8, port.m_port_id, status);
            return false;
        }
    }

    SWSS_LOG_INFO("Set asymmetric PFC %s to port id 0x%lx", pfc_asym.c_str(), port.m_port_id);

    return true;
}

bool PortsOrch::createBindAclTableGroup(sai_object_id_t id, sai_object_id_t &group_oid, acl_stage_type_t acl_stage)
{
    SWSS_LOG_ENTER();

    if (acl_stage == ACL_STAGE_UNKNOWN)
    {
        SWSS_LOG_ERROR("unknown ACL stage for table group creation");
        return false;
    }

    Port port;
    if (!getPort(id, port))
    {
        SWSS_LOG_ERROR("Failed to get port by port ID %lx", id);
        return false;
    }

    sai_status_t status;
    if ((acl_stage == ACL_STAGE_INGRESS) && (port.m_ingress_acl_table_group_id != 0))
    {
        group_oid = port.m_ingress_acl_table_group_id;
    }
    else if ((acl_stage == ACL_STAGE_EGRESS) && (port.m_egress_acl_table_group_id != 0))
    {
        group_oid = port.m_egress_acl_table_group_id;
    }
    // Port ACL table group does not exist, create one
    else if (acl_stage == ACL_STAGE_INGRESS or acl_stage == ACL_STAGE_EGRESS)
    {
        bool ingress = acl_stage == ACL_STAGE_INGRESS ? true : false;

        sai_acl_bind_point_type_t bind_type;
        switch (port.m_type)
        {
            case Port::PHY:
                bind_type = SAI_ACL_BIND_POINT_TYPE_PORT;
                break;
            case Port::LAG:
                bind_type = SAI_ACL_BIND_POINT_TYPE_LAG;
                break;
            case Port::VLAN:
                bind_type = SAI_ACL_BIND_POINT_TYPE_VLAN;
                break;
            default:
                SWSS_LOG_ERROR("Failed to bind ACL table to port %s with unknown type %d",
                        port.m_alias.c_str(), port.m_type);
                return false;
        }

        sai_object_id_t bp_list[] = { bind_type };

        vector<sai_attribute_t> group_attrs;
        sai_attribute_t group_attr;

        group_attr.id = SAI_ACL_TABLE_GROUP_ATTR_ACL_STAGE;
        group_attr.value.s32 = ingress ? SAI_ACL_STAGE_INGRESS : SAI_ACL_STAGE_EGRESS;
        group_attrs.push_back(group_attr);

        group_attr.id = SAI_ACL_TABLE_GROUP_ATTR_ACL_BIND_POINT_TYPE_LIST;
        group_attr.value.objlist.count = 1;
        group_attr.value.objlist.list = bp_list;
        group_attrs.push_back(group_attr);

        group_attr.id = SAI_ACL_TABLE_GROUP_ATTR_TYPE;
        group_attr.value.s32 = SAI_ACL_TABLE_GROUP_TYPE_PARALLEL;
        group_attrs.push_back(group_attr);

        status = sai_acl_api->create_acl_table_group(&group_oid, gSwitchId, (uint32_t)group_attrs.size(), group_attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create ACL table group, rv:%d", status);
            return false;
        }

        if (ingress)
        {
            port.m_ingress_acl_table_group_id = group_oid;
        }
        else
        {
            port.m_egress_acl_table_group_id = group_oid;
        }

        setPort(port.m_alias, port);

        gCrmOrch->incCrmAclUsedCounter(CrmResourceType::CRM_ACL_GROUP, ingress ? SAI_ACL_STAGE_INGRESS : SAI_ACL_STAGE_EGRESS, bind_type);

        switch (port.m_type)
        {
            case Port::PHY:
            {
                // Bind this ACL group to physical port
                sai_attribute_t port_attr;
                port_attr.id = ingress ? SAI_PORT_ATTR_INGRESS_ACL : SAI_PORT_ATTR_EGRESS_ACL;
                port_attr.value.oid = group_oid;

                status = sai_port_api->set_port_attribute(port.m_port_id, &port_attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to bind port %s to ACL table group %lx, rv:%d",
                            port.m_alias.c_str(), group_oid, status);
                    return false;
                }
                break;
            }
            case Port::LAG:
            {
                // Bind this ACL group to LAG
                sai_attribute_t lag_attr;
                lag_attr.id = ingress ? SAI_LAG_ATTR_INGRESS_ACL : SAI_LAG_ATTR_EGRESS_ACL;
                lag_attr.value.oid = group_oid;

                status = sai_lag_api->set_lag_attribute(port.m_lag_id, &lag_attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to bind LAG %s to ACL table group %lx, rv:%d",
                            port.m_alias.c_str(), group_oid, status);
                    return false;
                }
                break;
            }
            case Port::VLAN:
            {
                // Bind this ACL group to VLAN
                sai_attribute_t vlan_attr;
                vlan_attr.id = ingress ? SAI_VLAN_ATTR_INGRESS_ACL : SAI_VLAN_ATTR_EGRESS_ACL;
                vlan_attr.value.oid = group_oid;

                status = sai_vlan_api->set_vlan_attribute(port.m_vlan_info.vlan_oid, &vlan_attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to bind VLAN %s to ACL table group %lx, rv:%d",
                            port.m_alias.c_str(), group_oid, status);
                    return false;
                }
                break;
            }
            default:
            {
                SWSS_LOG_ERROR("Failed to bind %s port with type %d", port.m_alias.c_str(), port.m_type);
                return false;
            }
        }

        SWSS_LOG_NOTICE("Create %s ACL table group and bind port %s to it", ingress ? "ingress" : "egress", port.m_alias.c_str());
    }

    return true;
}

bool PortsOrch::bindAclTable(sai_object_id_t id, sai_object_id_t table_oid, sai_object_id_t &group_member_oid, acl_stage_type_t acl_stage)
{
    SWSS_LOG_ENTER();

    if (table_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Invalid ACL table %lx", table_oid);
        return false;
    }

    sai_status_t status;
    sai_object_id_t groupOid;

    // Create an ACL table group and bind to port
    if (!createBindAclTableGroup(id, groupOid, acl_stage))
    {
        SWSS_LOG_ERROR("Fail to create or bind to port %lx ACL table group", id);
        return false;
    }

    // Create an ACL group member with table_oid and groupOid
    vector<sai_attribute_t> member_attrs;

    sai_attribute_t member_attr;
    member_attr.id = SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID;
    member_attr.value.oid = groupOid;
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
        SWSS_LOG_ERROR("Failed to create member in ACL table group %lx for ACL table %lx, rv:%d",
                groupOid, table_oid, status);
        return false;
    }

    return true;
}

bool PortsOrch::setPortPvid(Port &port, sai_uint32_t pvid)
{
    SWSS_LOG_ENTER();

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
            return false;
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
            return false;
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
            return false;
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

    sai_attribute_t attr;
    sai_status_t status;

    // "Lazy" query of supported speeds for given port
    // Once received the list will be stored in m_portSupportedSpeeds
    if (!m_portSupportedSpeeds.count(port_id))
    {
        const auto size_guess = 25; // Guess the size which could be enough

        std::vector<sai_uint32_t> speeds(size_guess);

        for (int attempt = 0; attempt < 2; ++attempt) // two attempts to get our value
        {                                             // first with the guess,
                                                      // other with the returned value
            attr.id = SAI_PORT_ATTR_SUPPORTED_SPEED;
            attr.value.u32list.count = static_cast<uint32_t>(speeds.size());
            attr.value.u32list.list = speeds.data();

            status = sai_port_api->get_port_attribute(port_id, 1, &attr);
            if (status != SAI_STATUS_BUFFER_OVERFLOW)
            {
                break;
            }

            speeds.resize(attr.value.u32list.count); // if our guess was wrong
                                                     // retry with the correct value
        }

        if (status == SAI_STATUS_SUCCESS)
        {
                speeds.resize(attr.value.u32list.count);
                m_portSupportedSpeeds[port_id] = speeds;
        }
        else
        {
            if (status == SAI_STATUS_BUFFER_OVERFLOW)
            {
                // something went wrong in SAI implementation
                SWSS_LOG_ERROR("Failed to get supported speed list for port %s id=%lx. Not enough container size",
                               alias.c_str(), port_id);
            }
            else if (SAI_STATUS_IS_ATTR_NOT_SUPPORTED(status) ||
                     SAI_STATUS_IS_ATTR_NOT_IMPLEMENTED(status) ||
                     status == SAI_STATUS_NOT_IMPLEMENTED)
            {
                // unable to validate speed if attribute is not supported on platform
                // assuming input value is correct
                SWSS_LOG_WARN("Unable to validate speed for port %s id=%lx. Not supported by platform",
                              alias.c_str(), port_id);
            }
            else
            {
                SWSS_LOG_ERROR("Failed to get a list of supported speeds for port %s id=%lx. Error=%d",
                               alias.c_str(), port_id, status);
            }
            m_portSupportedSpeeds[port_id] = {}; // use an empty list,
                                                 // we don't want to get the port speed for this port again
            return true; // we can't check if the speed is valid, so return true to change the speed
        }

    }

    const PortSupportedSpeeds &supp_speeds = m_portSupportedSpeeds[port_id];
    if (supp_speeds.size() == 0)
    {
        // we don't have the list for this port, so return true to change speed anyway
        return true;
    }

    return std::find(supp_speeds.begin(), supp_speeds.end(), speed) != supp_speeds.end();
}

bool PortsOrch::setPortSpeed(sai_object_id_t port_id, sai_uint32_t speed)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    sai_status_t status;

    attr.id = SAI_PORT_ATTR_SPEED;
    attr.value.u32 = speed;

    status = sai_port_api->set_port_attribute(port_id, &attr);

    return status == SAI_STATUS_SUCCESS;
}

bool PortsOrch::getPortSpeed(sai_object_id_t port_id, sai_uint32_t &speed)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    sai_status_t status;

    attr.id = SAI_PORT_ATTR_SPEED;
    attr.value.u32 = 0;

    status = sai_port_api->get_port_attribute(port_id, 1, &attr);

    if (status == SAI_STATUS_SUCCESS)
        speed = attr.value.u32;

    return status == SAI_STATUS_SUCCESS;
}

bool PortsOrch::setPortAdvSpeed(sai_object_id_t port_id, sai_uint32_t speed)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    sai_status_t status;
    vector<sai_uint32_t> speeds;
    speeds.push_back(speed);

    attr.id = SAI_PORT_ATTR_ADVERTISED_SPEED;
    attr.value.u32list.list  = speeds.data();
    attr.value.u32list.count = static_cast<uint32_t>(speeds.size());

    status = sai_port_api->set_port_attribute(port_id, &attr);

    return status == SAI_STATUS_SUCCESS;
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
        SWSS_LOG_ERROR("Failed to get queue type and index for queue %lu rv:%d", queue_id, status);
        return false;
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
        SWSS_LOG_ERROR("Got unsupported queue type %d for %lu queue", attr[0].value.s32, queue_id);
        throw runtime_error("Got unsupported queue type");
    }

    index = attr[1].value.u8;

    return true;
}

bool PortsOrch::setPortAutoNeg(sai_object_id_t id, int an)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_AUTO_NEG_MODE;
    switch(an) {
      case 1:
        attr.value.booldata = true;
        break;
      default:
        attr.value.booldata = false;
        break;
    }

    sai_status_t status = sai_port_api->set_port_attribute(id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set AutoNeg %u to port pid:%lx", attr.value.booldata, id);
        return false;
    }
    SWSS_LOG_INFO("Set AutoNeg %u to port pid:%lx", attr.value.booldata, id);
    return true;
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

void PortsOrch::updateDbPortFlapCounter(const Port& port, vector<FieldValueTuple>& old_tuples, vector<FieldValueTuple>& new_tuples) const
{
  SWSS_LOG_ENTER();

  string flap_value;
  bool check = false;

  /* Fetching current flap counters from redis DB and incrementing it by 1 under any UP or DOWN event */

  for (auto const &i : old_tuples)
  {
    if (fvField(i) == "flap_counter")
    {
      flap_value = fvValue(i);
      check = true;
      break;
    }
  }

  if (!check)
  {
    return;
  }

  long flap_val = stol(flap_value);
  flap_val = flap_val + 1;
  string flaps;
  stringstream flap_stream;
  flap_stream << flap_val;
  flaps = flap_stream.str();
  FieldValueTuple flap_tuple("flap_counter", flaps);
  new_tuples.push_back(flap_tuple);
}

void PortsOrch::updateDbPortLastFlapTime(vector<FieldValueTuple>& new_tuples) const
{
  SWSS_LOG_ENTER();

  time_t now = time(0);
  string date = ctime(&now);

  // convert now to tm struct for UTC
  tm *gmtm = gmtime(&now);
  date = asctime(gmtm);
  FieldValueTuple tuple("last_flap", date);
  new_tuples.push_back(tuple);
}

void PortsOrch::updateDbPortOperStatus(const Port& port, sai_port_oper_status_t status) const
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> old_tuples;
    vector<FieldValueTuple> new_tuples;

    bool exist = m_portTable->get(port.m_alias, old_tuples);
    if (!exist)
    {
      return;
    }
    updateDbPortLastFlapTime(new_tuples);
    updateDbPortFlapCounter(port, old_tuples, new_tuples);
    FieldValueTuple tuple("oper_status", oper_status_strings.at(status));
    new_tuples.push_back(tuple);
    m_portTable->set(port.m_alias, new_tuples);
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
        return false;
    }

    m_portListLaneMap[lane_set] = port_id;

    SWSS_LOG_NOTICE("Create port %lx with the speed %u", port_id, speed);

    return true;
}

bool PortsOrch::removePort(sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    Port p;
    if (getPort(port_id, p))
    {
        PortUpdate update = {p, false };
        notify(SUBJECT_TYPE_PORT_CHANGE, static_cast<void *>(&update));
    }

    sai_status_t status = sai_port_api->remove_port(port_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove port %lx, rv:%d", port_id, status);
        return false;
    }
    removeAclTableGroup(p);
    SWSS_LOG_NOTICE("Remove port %lx", port_id);

    return true;
}

string PortsOrch::getPortFlexCounterTableKey(string key)
{
    return string(PORT_STAT_COUNTER_FLEX_COUNTER_GROUP) + ":" + key;
}

string PortsOrch::getQueueFlexCounterTableKey(string key)
{
    return string(QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP) + ":" + key;
}

string PortsOrch::getQueueWatermarkFlexCounterTableKey(string key)
{
    return string(QUEUE_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP) + ":" + key;
}

string PortsOrch::getPriorityGroupWatermarkFlexCounterTableKey(string key)
{
    return string(PG_WATERMARK_STAT_COUNTER_FLEX_COUNTER_GROUP) + ":" + key;
}

bool PortsOrch::initPort(const string &alias, const set<int> &lane_set)
{
    SWSS_LOG_ENTER();

    /* Determine if the lane combination exists in switch */
    if (m_portListLaneMap.find(lane_set) != m_portListLaneMap.end())
    {
        sai_object_id_t id = m_portListLaneMap[lane_set];

        /* Determine if the port has already been initialized before */
        if (m_portList.find(alias) != m_portList.end() && m_portList[alias].m_port_id == id)
        {
            SWSS_LOG_INFO("Port has already been initialized before alias:%s", alias.c_str());
        }
        else
        {
            Port p(alias, Port::PHY);

            p.m_index = static_cast<int32_t>(m_portList.size()); // TODO: Assume no deletion of physical port
            p.m_port_id = id;

            /* Initialize the port and create corresponding host interface */
            if (initializePort(p))
            {
                /* Add port to port list */
                m_portList[alias] = p;
                /* Add port name map to counter table */
                FieldValueTuple tuple(p.m_alias, sai_serialize_object_id(p.m_port_id));
                vector<FieldValueTuple> fields;
                fields.push_back(tuple);
                m_counterTable->set("", fields);

                /* Add port to flex_counter for updating stat counters  */
                string key = getPortFlexCounterTableKey(sai_serialize_object_id(p.m_port_id));
                std::string delimiter = "";
                std::ostringstream counters_stream;
                for (const auto &id: portStatIds)
                {
                    counters_stream << delimiter << sai_serialize_port_stat(id);
                    delimiter = comma;
                }

                fields.clear();
                fields.emplace_back(PORT_COUNTER_ID_LIST, counters_stream.str());

                m_flexCounterTable->set(key, fields);

                PortUpdate update = {p, true };
                notify(SUBJECT_TYPE_PORT_CHANGE, static_cast<void *>(&update));

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

bool PortsOrch::bake()
{
    SWSS_LOG_ENTER();

    // Check the APP_DB port table for warm reboot
    vector<FieldValueTuple> tuples;
    string value;
    bool foundPortConfigDone = m_portTable->hget("PortConfigDone", "count", value);
    unsigned long portCount;
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

    portCount = stoul(value);
    SWSS_LOG_NOTICE("portCount = %lu, m_portCount = %u", portCount, m_portCount);
    if (portCount != keys.size() - 2)
    {
        // Invalid port table
        SWSS_LOG_ERROR("Invalid port table: portCount, expecting %lu, got %lu",
                portCount, keys.size() - 2);

        cleanPortTable(keys);
        return false;
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
            if (m_portConfigDone)
            {
                // Already done, ignore this task
                it = consumer.m_toSync.erase(it);
                continue;
            }

            m_portConfigDone = true;

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
                m_initDone = true;
                SWSS_LOG_INFO("Get PortInitDone notification from portsyncd.");
            }

            it = consumer.m_toSync.erase(it);
            return;

        }

        if (op == SET_COMMAND)
        {
            set<int> lane_set;
            vector<uint32_t> pre_emphasis;
            vector<uint32_t> idriver;
            vector<uint32_t> ipredriver;
            string admin_status;
            string fec_mode;
            string pfc_asym;
            uint32_t mtu = 0;
            uint32_t speed = 0;
            int an = -1;

            for (auto i : kfvFieldsValues(t))
            {
                /* Get lane information of a physical port and initialize the port */
                if (fvField(i) == "lanes")
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
                if (fvField(i) == "admin_status")
                {
                    admin_status = fvValue(i);
                }

                /* Set port MTU */
                if (fvField(i) == "mtu")
                {
                    mtu = (uint32_t)stoul(fvValue(i));
                }

                /* Set port speed */
                if (fvField(i) == "speed")
                {
                    speed = (uint32_t)stoul(fvValue(i));
                }

                /* Set port fec */
                if (fvField(i) == "fec")
                {
                    fec_mode = fvValue(i);
                }

                /* Set port asymmetric PFC */
                if (fvField(i) == "pfc_asym")
                    pfc_asym = fvValue(i);

                /* Set autoneg and ignore the port speed setting */
                if (fvField(i) == "autoneg")
                {
                    an = (int)stoul(fvValue(i));
                }

                /* Set port serdes Pre-emphasis */
                if (fvField(i) == "preemphasis")
                {
                    getPortSerdesVal(fvValue(i), pre_emphasis);
                }

                /* Set port serdes idriver */
                if (fvField(i) == "idriver")
                {
                    getPortSerdesVal(fvValue(i), idriver);
                }

                /* Set port serdes ipredriver */
                if (fvField(i) == "ipredriver")
                {
                    getPortSerdesVal(fvValue(i), ipredriver);
                }
            }

            /* Collect information about all received ports */
            if (lane_set.size())
            {
                m_lanesAliasSpeedMap[lane_set] = make_tuple(alias, speed, an, fec_mode);
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
            if (m_portConfigDone && (m_lanesAliasSpeedMap.size() == m_portCount))
            {
                for (auto it = m_portListLaneMap.begin(); it != m_portListLaneMap.end();)
                {
                    if (m_lanesAliasSpeedMap.find(it->first) == m_lanesAliasSpeedMap.end())
                    {
                        char *platform = getenv("platform");
                        if (platform && (strstr(platform, BFN_PLATFORM_SUBSTRING) || strstr(platform, MLNX_PLATFORM_SUBSTRING)))
                        {
                            if (!removePort(it->second))
                            {
                                throw runtime_error("PortsOrch initialization failure.");
                            }
                        }
                        else
                        {
                            SWSS_LOG_NOTICE("Failed to remove Port %lx due to missing SAI remove_port API.", it->second);
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
                    bool port_created = false;

                    if (m_portListLaneMap.find(it->first) == m_portListLaneMap.end())
                    {
                        // work around to avoid syncd termination on SAI error due missing create_port SAI API
                        // can be removed when SAI redis return NotImplemented error
                        char *platform = getenv("platform");
                        if (platform && (strstr(platform, BFN_PLATFORM_SUBSTRING) || strstr(platform, MLNX_PLATFORM_SUBSTRING)))
                        {
                            if (!addPort(it->first, get<1>(it->second), get<2>(it->second), get<3>(it->second)))
                            {
                                throw runtime_error("PortsOrch initialization failure.");
                            }

                            port_created = true;
                        }
                        else
                        {
                            SWSS_LOG_NOTICE("Failed to create Port %s due to missing SAI create_port API.", get<0>(it->second).c_str());
                        }
                    }
                    else
                    {
                        port_created = true;
                    }

                    if (port_created)
                    {
                        if (!initPort(get<0>(it->second), it->first))
                        {
                            throw runtime_error("PortsOrch initialization failure.");
                        }
                    }

                    it = m_lanesAliasSpeedMap.erase(it);
                }
            }

            if (!m_portConfigDone)
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
                if (an != -1 && an != p.m_autoneg)
                {
                    if (setPortAutoNeg(p.m_port_id, an))
                    {
                        SWSS_LOG_NOTICE("Set port %s AutoNeg to %u", alias.c_str(), an);
                        p.m_autoneg = an;
                        m_portList[alias] = p;

                        // Once AN is changed
                        // - no speed specified: need to reapply the port speed or port adv speed accordingly
                        // - speed specified: need to apply the port speed or port adv speed by the specified one
                        // Note: one special case is
                        // - speed specified as existing m_speed: need to apply even they are the same
                        auto old_speed = p.m_speed;
                        p.m_speed = 0;
                        auto new_speed = speed ? speed : old_speed;
                        if (new_speed)
                        {
                            // Modify the task in place
                            kfvFieldsValues(t).emplace_back("speed", to_string(new_speed));
                            // Fallthrough to process `speed'
                            speed = new_speed;
                        }
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to set port %s AN to %u", alias.c_str(), an);
                        it++;
                        continue;
                    }
                }

                /*
                 * When AN is enabled, set the port adv speed, otherwise change the port speed.
                 *
                 * 1. Get supported speed list and validate if the target speed is within the list
                 * 2. Get the current port speed and check if it is the same as the target speed
                 * 3. Set port admin status to DOWN before changing the speed
                 * 4. Set port speed
                 */
                if (speed != 0 && speed != p.m_speed)
                {
                    m_portList[alias] = p;

                    if (p.m_autoneg)
                    {
                        if (setPortAdvSpeed(p.m_port_id, speed))
                        {
                            SWSS_LOG_NOTICE("Set port %s advertised speed to %u", alias.c_str(), speed);
                        }
                        else
                        {
                            SWSS_LOG_ERROR("Failed to set port %s advertised speed to %u", alias.c_str(), speed);
                            it++;
                            continue;
                        }
                    }
                    else
                    {
                        if (!isSpeedSupported(alias, p.m_port_id, speed))
                        {
                            it++;
                            continue;
                        }

                        if (p.m_admin_state_up)
                        {
                            /* Bring port down before applying speed */
                            if (!setPortAdminStatus(p.m_port_id, false))
                            {
                                SWSS_LOG_ERROR("Failed to set port %s admin status DOWN to set speed", alias.c_str());
                                it++;
                                continue;
                            }

                            p.m_admin_state_up = false;
                            m_portList[alias] = p;

                            if (!setPortSpeed(p.m_port_id, speed))
                            {
                                SWSS_LOG_ERROR("Failed to set port %s speed to %u", alias.c_str(), speed);
                                it++;
                                continue;
                            }
                        }
                        else
                        {
                            /* Port is already down, setting speed */
                            if (!setPortSpeed(p.m_port_id, speed))
                            {
                                SWSS_LOG_ERROR("Failed to set port %s speed to %u", alias.c_str(), speed);
                                it++;
                                continue;
                            }
                        }
                        SWSS_LOG_NOTICE("Set port %s speed to %u", alias.c_str(), speed);
                    }
                    p.m_speed = speed;
                    m_portList[alias] = p;
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
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to set port %s MTU to %u", alias.c_str(), mtu);
                        it++;
                        continue;
                    }
                }

                if (!fec_mode.empty())
                {
                    if (fec_mode_map.find(fec_mode) != fec_mode_map.end())
                    {
                        /* reset fec mode upon mode change */
                        if (p.m_fec_mode != fec_mode_map[fec_mode])
                        {
                            p.m_fec_mode = fec_mode_map[fec_mode];
                            if (setPortFec(p.m_port_id, p.m_fec_mode))
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
                    else
                    {
                        SWSS_LOG_ERROR("Unknown fec mode %s", fec_mode.c_str());
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

                if (pre_emphasis.size() != 0)
                {
                    if (setPortSerdesAttribute(p.m_port_id, SAI_PORT_ATTR_SERDES_PREEMPHASIS, pre_emphasis))
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

                if (idriver.size() != 0)
                {
                    if (setPortSerdesAttribute(p.m_port_id, SAI_PORT_ATTR_SERDES_IDRIVER, idriver))
                    {
                        SWSS_LOG_NOTICE("Set port %s idriver is success", alias.c_str());
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to set port %s idriver", alias.c_str());
                        it++;
                        continue;
                    }

                }

                if (ipredriver.size() != 0)
                {
                    if (setPortSerdesAttribute(p.m_port_id, SAI_PORT_ATTR_SERDES_IPREDRIVER, ipredriver))
                    {
                        SWSS_LOG_NOTICE("Set port %s ipredriver is success", alias.c_str());
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to set port %s ipredriver", alias.c_str());
                        it++;
                        continue;
                    }

                }

                /* Last step set port admin status */
                if (!admin_status.empty() && (p.m_admin_state_up != (admin_status == "up")))
                {
                    if (setPortAdminStatus(p.m_port_id, admin_status == "up"))
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
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "mtu")
                {
                    mtu = (uint32_t)stoul(fvValue(i));
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
                    if (port.m_vlan_members.empty())
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
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "mtu")
                {
                    mtu = (uint32_t)stoul(fvValue(i));
                }
            }

            // Create a new LAG when the new alias comes
            if (m_portList.find(alias) == m_portList.end())
            {
                if (!addLag(alias))
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
                if (mtu != 0)
                {
                    l.m_mtu = mtu;
                    m_portList[alias] = l;
                    if (l.m_rif_id)
                    {
                        gIntfsOrch->setRouterIntfsMtu(l);
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

        /* Update a LAG member */
        if (op == SET_COMMAND)
        {
            string status;
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "status")
                    status = fvValue(i);
            }

            /* Sync an enabled member */
            if (status == "enabled")
            {
                /* Duplicate entry */
                if (lag.m_members.find(port_alias) != lag.m_members.end())
                {
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                /* Assert the port doesn't belong to any LAG */
                assert(!port.m_lag_id && !port.m_lag_member_id);

                if (addLagMember(lag, port))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            /* Sync an disabled member */
            else /* status == "disabled" */
            {
                /* "status" is "disabled" at start when m_lag_id and
                 * m_lag_member_id are absent */
                if (!port.m_lag_id || !port.m_lag_member_id)
                {
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                if (removeLagMember(lag, port))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
        }
        /* Remove a LAG member */
        else if (op == DEL_COMMAND)
        {
            /* Assert the LAG member exists */
            assert(lag.m_members.find(port_alias) != lag.m_members.end());

            if (!port.m_lag_id || !port.m_lag_member_id)
            {
                SWSS_LOG_WARN("Member %s not found in LAG %s lid:%lx lmid:%lx,",
                        port.m_alias.c_str(), lag.m_alias.c_str(), lag.m_lag_id, port.m_lag_member_id);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (removeLagMember(lag, port))
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
        if (!isPortReady())
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
        else if (table_name == APP_LAG_TABLE_NAME)
        {
            doLagTask(consumer);
        }
        else if (table_name == APP_LAG_MEMBER_TABLE_NAME)
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
        throw runtime_error("PortsOrch initialization failure.");
    }
    SWSS_LOG_INFO("Get %d queues for port %s", attr.value.u32, port.m_alias.c_str());

    port.m_queue_ids.resize(attr.value.u32);

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
        throw runtime_error("PortsOrch initialization failure.");
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
        throw runtime_error("PortsOrch initialization failure.");
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
        throw runtime_error("PortsOrch initialization failure.");
    }
    SWSS_LOG_INFO("Get priority groups for port %s", port.m_alias.c_str());
}

bool PortsOrch::initializePort(Port &port)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("Initializing port alias:%s pid:%lx", port.m_alias.c_str(), port.m_port_id);

    initializePriorityGroups(port);
    initializeQueues(port);

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
    m_portTable->hset(port.m_alias, "flap_counter", "0");
    m_portTable->hset(port.m_alias, "last_flap", "N/A");

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
    attrs.push_back(attr);

    sai_status_t status = sai_hostif_api->create_hostif(&host_intfs_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create host interface for port %s", alias.c_str());
        return false;
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
        SWSS_LOG_ERROR("Failed to set bridge port %lx learning_mode attribute: %d", bridge_port_id, status);
        return false;
    }
    SWSS_LOG_NOTICE("Disable FDB learning on bridge port %s(%lx)", port.m_alias.c_str(), bridge_port_id);
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

    attr.id = SAI_BRIDGE_PORT_ATTR_TYPE;
    attr.value.s32 = SAI_BRIDGE_PORT_TYPE_PORT;
    attrs.push_back(attr);

    attr.id = SAI_BRIDGE_PORT_ATTR_PORT_ID;
    if (port.m_type == Port::PHY)
    {
        attr.value.oid = port.m_port_id;
    }
    else if  (port.m_type == Port::LAG)
    {
        attr.value.oid = port.m_lag_id;
    }
    else
    {
        SWSS_LOG_ERROR("Failed to add bridge port %s to default 1Q bridge, invalid porty type %d",
            port.m_alias.c_str(), port.m_type);
        return false;
    }
    attrs.push_back(attr);

    /* Create a bridge port with admin status set to UP */
    attr.id = SAI_BRIDGE_PORT_ATTR_ADMIN_STATE;
    attr.value.booldata = true;
    attrs.push_back(attr);

    /* And with hardware FDB learning mode set to HW (explicit default value) */
    attr.id = SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE;
    attr.value.s32 = SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW;
    attrs.push_back(attr);

    sai_status_t status = sai_bridge_api->create_bridge_port(&port.m_bridge_port_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to add bridge port %s to default 1Q bridge, rv:%d",
            port.m_alias.c_str(), status);
        return false;
    }

    if (!setHostIntfsStripTag(port, SAI_HOSTIF_VLAN_TAG_KEEP))
    {
        SWSS_LOG_ERROR("Failed to set %s for hostif of port %s",
                hostif_vlan_tag[SAI_HOSTIF_VLAN_TAG_KEEP], port.m_alias.c_str());
        return false;
    }
    m_portList[port.m_alias] = port;
    SWSS_LOG_NOTICE("Add bridge port %s to default 1Q bridge", port.m_alias.c_str());

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
        return false;
    }

    if (!setHostIntfsStripTag(port, SAI_HOSTIF_VLAN_TAG_STRIP))
    {
        SWSS_LOG_ERROR("Failed to set %s for hostif of port %s",
                hostif_vlan_tag[SAI_HOSTIF_VLAN_TAG_STRIP], port.m_alias.c_str());
        return false;
    }

    /* Flush FDB entries pointing to this bridge port */
    // TODO: Remove all FDB entries associated with this bridge port before
    //       removing the bridge port itself

    /* Remove bridge port */
    status = sai_bridge_api->remove_bridge_port(port.m_bridge_port_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove bridge port %s from default 1Q bridge, rv:%d",
            port.m_alias.c_str(), status);
        return false;
    }
    port.m_bridge_port_id = SAI_NULL_OBJECT_ID;

    SWSS_LOG_NOTICE("Remove bridge port %s from default 1Q bridge", port.m_alias.c_str());

    m_portList[port.m_alias] = port;
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
        return false;
    }

    SWSS_LOG_NOTICE("Create an empty VLAN %s vid:%hu", vlan_alias.c_str(), vlan_id);

    Port vlan(vlan_alias, Port::VLAN);
    vlan.m_vlan_info.vlan_oid = vlan_oid;
    vlan.m_vlan_info.vlan_id = vlan_id;
    vlan.m_members = set<string>();
    m_portList[vlan_alias] = vlan;

    return true;
}

bool PortsOrch::removeVlan(Port vlan)
{
    SWSS_LOG_ENTER();

    /* Vlan removing is not allowed when the VLAN still has members */
    if (vlan.m_members.size() > 0)
    {
        SWSS_LOG_ERROR("Failed to remove non-empty VLAN %s", vlan.m_alias.c_str());
        return false;
    }

    sai_status_t status = sai_vlan_api->remove_vlan(vlan.m_vlan_info.vlan_oid);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove VLAN %s vid:%hu",
                vlan.m_alias.c_str(), vlan.m_vlan_info.vlan_id);
        return false;
    }

    removeAclTableGroup(vlan);

    SWSS_LOG_NOTICE("Remove VLAN %s vid:%hu", vlan.m_alias.c_str(),
            vlan.m_vlan_info.vlan_id);

    m_portList.erase(vlan.m_alias);

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

bool PortsOrch::addVlanMember(Port &vlan, Port &port, string &tagging_mode)
{
    SWSS_LOG_ENTER();

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
        SWSS_LOG_ERROR("Failed to add member %s to VLAN %s vid:%hu pid:%lx",
                port.m_alias.c_str(), vlan.m_alias.c_str(), vlan.m_vlan_info.vlan_id, port.m_port_id);
        return false;
    }
    SWSS_LOG_NOTICE("Add member %s to VLAN %s vid:%hu pid%lx",
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
    port.m_vlan_members[vlan.m_vlan_info.vlan_id] = vme;
    m_portList[port.m_alias] = port;
    vlan.m_members.insert(port.m_alias);
    m_portList[vlan.m_alias] = vlan;

    VlanMemberUpdate update = { vlan, port, true };
    notify(SUBJECT_TYPE_VLAN_MEMBER_CHANGE, static_cast<void *>(&update));

    return true;
}

bool PortsOrch::removeVlanMember(Port &vlan, Port &port)
{
    SWSS_LOG_ENTER();

    sai_object_id_t vlan_member_id;
    sai_vlan_tagging_mode_t sai_tagging_mode;
    auto vlan_member = port.m_vlan_members.find(vlan.m_vlan_info.vlan_id);

    /* Assert the port belongs to this VLAN */
    assert (vlan_member != port.m_vlan_members.end());
    sai_tagging_mode = vlan_member->second.vlan_mode;
    vlan_member_id = vlan_member->second.vlan_member_id;

    sai_status_t status = sai_vlan_api->remove_vlan_member(vlan_member_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove member %s from VLAN %s vid:%hx vmid:%lx",
                port.m_alias.c_str(), vlan.m_alias.c_str(), vlan.m_vlan_info.vlan_id, vlan_member_id);
        return false;
    }
    port.m_vlan_members.erase(vlan_member);
    SWSS_LOG_NOTICE("Remove member %s from VLAN %s lid:%hx vmid:%lx",
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

bool PortsOrch::addLag(string lag_alias)
{
    SWSS_LOG_ENTER();

    sai_object_id_t lag_id;
    sai_status_t status = sai_lag_api->create_lag(&lag_id, gSwitchId, 0, NULL);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create LAG %s lid:%lx", lag_alias.c_str(), lag_id);
        return false;
    }

    SWSS_LOG_NOTICE("Create an empty LAG %s lid:%lx", lag_alias.c_str(), lag_id);

    Port lag(lag_alias, Port::LAG);
    lag.m_lag_id = lag_id;
    lag.m_members = set<string>();
    m_portList[lag_alias] = lag;

    PortUpdate update = { lag, true };
    notify(SUBJECT_TYPE_PORT_CHANGE, static_cast<void *>(&update));

    return true;
}

bool PortsOrch::removeLag(Port lag)
{
    SWSS_LOG_ENTER();

    /* Retry when the LAG still has members */
    if (lag.m_members.size() > 0)
    {
        SWSS_LOG_ERROR("Failed to remove non-empty LAG %s", lag.m_alias.c_str());
        return false;
    }
    if (lag.m_vlan_members.size() > 0)
    {
        SWSS_LOG_ERROR("Failed to remove LAG %s, it is still in VLAN", lag.m_alias.c_str());
        return false;
    }

    sai_status_t status = sai_lag_api->remove_lag(lag.m_lag_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove LAG %s lid:%lx", lag.m_alias.c_str(), lag.m_lag_id);
        return false;
    }

    removeAclTableGroup(lag);

    SWSS_LOG_NOTICE("Remove LAG %s lid:%lx", lag.m_alias.c_str(), lag.m_lag_id);

    m_portList.erase(lag.m_alias);

    PortUpdate update = { lag, false };
    notify(SUBJECT_TYPE_PORT_CHANGE, static_cast<void *>(&update));

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

bool PortsOrch::addLagMember(Port &lag, Port &port)
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

    sai_object_id_t lag_member_id;
    sai_status_t status = sai_lag_api->create_lag_member(&lag_member_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to add member %s to LAG %s lid:%lx pid:%lx",
                port.m_alias.c_str(), lag.m_alias.c_str(), lag.m_lag_id, port.m_port_id);
        return false;
    }

    SWSS_LOG_NOTICE("Add member %s to LAG %s lid:%lx pid:%lx",
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

    LagMemberUpdate update = { lag, port, true };
    notify(SUBJECT_TYPE_LAG_MEMBER_CHANGE, static_cast<void *>(&update));

    return true;
}

bool PortsOrch::removeLagMember(Port &lag, Port &port)
{
    sai_status_t status = sai_lag_api->remove_lag_member(port.m_lag_member_id);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove member %s from LAG %s lid:%lx lmid:%lx",
                port.m_alias.c_str(), lag.m_alias.c_str(), lag.m_lag_id, port.m_lag_member_id);
        return false;
    }

    SWSS_LOG_NOTICE("Remove member %s from LAG %s lid:%lx lmid:%lx",
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
    LagMemberUpdate update = { lag, port, false };
    notify(SUBJECT_TYPE_LAG_MEMBER_CHANGE, static_cast<void *>(&update));

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

        /* add ordinary Queue stat counters */
        string key = getQueueFlexCounterTableKey(id);

        std::string delimiter = "";
        std::ostringstream counters_stream;
        for (const auto& it: queueStatIds)
        {
            counters_stream << delimiter << sai_serialize_queue_stat(it);
            delimiter = comma;
        }

        vector<FieldValueTuple> fieldValues;
        fieldValues.emplace_back(QUEUE_COUNTER_ID_LIST, counters_stream.str());

        m_flexCounterTable->set(key, fieldValues);

        /* add watermark queue counters */
        key = getQueueWatermarkFlexCounterTableKey(id);

        delimiter = "";
        counters_stream.str("");
        for (const auto& it: queueWatermarkStatIds)
        {
            counters_stream << delimiter << sai_serialize_queue_stat(it);
            delimiter = comma;
        }

        fieldValues.clear();
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
    }

    m_pgTable->set("", pgVector);
    m_pgPortTable->set("", pgPortVector);
    m_pgIndexTable->set("", pgIndexVector);

    CounterCheckOrch::getInstance().addPort(port);
}

void PortsOrch::doTask(NotificationConsumer &consumer)
{
    SWSS_LOG_ENTER();

    /* Wait for all ports to be initialized */
    if (!isPortReady())
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

            SWSS_LOG_NOTICE("Get port state change notification id:%lx status:%d", id, status);

            Port port;

            if (!getPort(id, port))
            {
                SWSS_LOG_ERROR("Failed to get port object for port id 0x%lx", id);
                continue;
            }

            updatePortOperStatus(port, status);

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
        return ;
    }

    updateDbPortOperStatus(port, status);
    port.m_oper_status = status;

    bool isUp = status == SAI_PORT_OPER_STATUS_UP;
    if (!setHostIntfsOperStatus(port, isUp))
    {
        SWSS_LOG_ERROR("Failed to set host interface %s operational status %s", port.m_alias.c_str(),
                isUp ? "up" : "down");
    }
    if (!gNeighOrch->ifChangeInformNextHop(port.m_alias, isUp))
    {
        SWSS_LOG_WARN("Inform nexthop operation failed for interface %s", port.m_alias.c_str());
    }
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

bool PortsOrch::removeAclTableGroup(const Port &p)
{
    sai_acl_bind_point_type_t bind_type;
    switch (p.m_type)
    {
        case Port::PHY:
            bind_type = SAI_ACL_BIND_POINT_TYPE_PORT;
            break;
        case Port::LAG:
            bind_type = SAI_ACL_BIND_POINT_TYPE_LAG;
            break;
        case Port::VLAN:
            bind_type = SAI_ACL_BIND_POINT_TYPE_VLAN;
            break;
        default:
            // Dealing with port, lag and vlan for now.
            return true;
    }
    sai_status_t ret;
    if (p.m_ingress_acl_table_group_id != 0)
    {
        ret = sai_acl_api->remove_acl_table_group(p.m_ingress_acl_table_group_id);
        if (ret != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove ingress acl table group for %s", p.m_alias.c_str());
            return false;
        }
        gCrmOrch->decCrmAclUsedCounter(CrmResourceType::CRM_ACL_GROUP, SAI_ACL_STAGE_INGRESS, bind_type, p.m_ingress_acl_table_group_id);
    }

    if (p.m_egress_acl_table_group_id != 0)
    {
        ret = sai_acl_api->remove_acl_table_group(p.m_egress_acl_table_group_id);
        if (ret != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove egress acl table group for %s", p.m_alias.c_str());
            return false;
        }
        gCrmOrch->decCrmAclUsedCounter(CrmResourceType::CRM_ACL_GROUP, SAI_ACL_STAGE_EGRESS, bind_type, p.m_egress_acl_table_group_id);
    }
    return true;
}

bool PortsOrch::setPortSerdesAttribute(sai_object_id_t port_id, sai_attr_id_t attr_id,
                                       vector<uint32_t> &serdes_val)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    memset(&attr, 0, sizeof(attr));
    attr.id = attr_id;

    attr.value.u32list.count = (uint32_t)serdes_val.size();
    attr.value.u32list.list = serdes_val.data();

    sai_status_t status = sai_port_api->set_port_attribute(port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set serdes attribute %d to port pid:%lx",
                       attr_id, port_id);
        return false;
    }
    return true;
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
