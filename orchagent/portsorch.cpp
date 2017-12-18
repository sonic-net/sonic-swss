#include "portsorch.h"

#include <cassert>
#include <fstream>
#include <sstream>
#include <set>
#include <algorithm>
#include <tuple>
#include <sstream>

#include <netinet/if_ether.h>
#include "net/if.h"

#include "logger.h"
#include "schema.h"
#include "converter.h"
#include "saiserialize.h"


extern sai_switch_api_t *sai_switch_api;
extern sai_bridge_api_t *sai_bridge_api;
extern sai_port_api_t *sai_port_api;
extern sai_vlan_api_t *sai_vlan_api;
extern sai_lag_api_t *sai_lag_api;
extern sai_hostif_api_t* sai_hostif_api;
extern sai_acl_api_t* sai_acl_api;
extern sai_queue_api_t *sai_queue_api;
extern sai_object_id_t gSwitchId;

#define VLAN_PREFIX         "Vlan"
#define DEFAULT_VLAN_ID     1
#define FLEX_STAT_COUNTER_POLL_MSECS "1000"

static map<string, sai_port_fec_mode_t> fec_mode_map =
{
    { "none",  SAI_PORT_FEC_MODE_NONE },
    { "rs", SAI_PORT_FEC_MODE_RS },
    { "fc", SAI_PORT_FEC_MODE_FC }
};

static const vector<sai_queue_stat_t> queueStatIds =
{
    SAI_QUEUE_STAT_PACKETS,
    SAI_QUEUE_STAT_BYTES,
    SAI_QUEUE_STAT_DROPPED_PACKETS,
    SAI_QUEUE_STAT_DROPPED_BYTES
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
PortsOrch::PortsOrch(DBConnector *db, vector<string> tableNames) :
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

    m_flex_db = shared_ptr<DBConnector>(new DBConnector(PFC_WD_DB, DBConnector::DEFAULT_UNIXSOCKET, 0));
    m_flexCounterTable = unique_ptr<ProducerStateTable>(new ProducerStateTable(m_flex_db.get(), PFC_WD_STATE_TABLE));

    uint32_t i, j;
    sai_status_t status;
    sai_attribute_t attr;

    /* Get CPU port */
    attr.id = SAI_SWITCH_ATTR_CPU_PORT;

    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get CPU port, rv:%d", status);
        throw "PortsOrch initialization failure";
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
        throw "PortsOrch initialization failure";
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
        throw "PortsOrch initialization failure";
    }

    /* Get port hardware lane info */
    for (i = 0; i < m_portCount; i++)
    {
        sai_uint32_t lanes[4] = { 0,0,0,0 };
        attr.id = SAI_PORT_ATTR_HW_LANE_LIST;
        attr.value.u32list.count = 4;
        attr.value.u32list.list = lanes;

        status = sai_port_api->get_port_attribute(port_list[i], 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to get hardware lane list pid:%lx", port_list[i]);
            throw "PortsOrch initialization failure";
        }

        set<int> tmp_lane_set;
        for (j = 0; j < attr.value.u32list.count; j++)
            tmp_lane_set.insert(attr.value.u32list.list[j]);

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
        throw "PortsOrch initialization failure";
    }

    m_default1QBridge = attrs[0].value.oid;
    m_defaultVlan = attrs[1].value.oid;

    removeDefaultVlanMembers();
    removeDefaultBridgePorts();
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
        throw "PortsOrch initialization failure";
    }

    /* Remove VLAN members in default VLAN */
    for (uint32_t i = 0; i < attr.value.objlist.count; i++)
    {
        status = sai_vlan_api->remove_vlan_member(vlan_member_list[i]);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove VLAN member, rv:%d", status);
            throw "PortsOrch initialization failure";
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
        throw "PortsOrch initialization failure";
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
            throw "PortsOrch initialization failure";
        }
        if (attr.value.s32 == SAI_BRIDGE_PORT_TYPE_PORT)
        {
            status = sai_bridge_api->remove_bridge_port(bridge_port_list[i]);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove bridge port, rv:%d", status);
                throw "PortsOrch initialization failure";
            }
        }
    }

    SWSS_LOG_NOTICE("Remove bridge ports from default 1Q bridge");
}

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
        SWSS_LOG_ERROR("Failed to set MTU %u to port pid:%lx", attr.value.u32, id);
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

bool PortsOrch::bindAclTable(sai_object_id_t id, sai_object_id_t table_oid, sai_object_id_t &group_member_oid, acl_stage_type_t acl_stage)
{
    SWSS_LOG_ENTER();

    if (acl_stage == ACL_STAGE_UNKNOWN)
    {
        SWSS_LOG_ERROR("Unknown Acl stage for Acl table %lx", table_oid);
        return false;
    }

    sai_status_t status;
    sai_object_id_t groupOid;

    Port p;
    if (!getPort(id, p))
    {
        return false;
    }

    auto &port = m_portList.find(p.m_alias)->second;

    if (acl_stage == ACL_STAGE_INGRESS && port.m_ingress_acl_table_group_id != 0)
    {
        groupOid = port.m_ingress_acl_table_group_id;
    }
    else if (acl_stage == ACL_STAGE_EGRESS && port.m_egress_acl_table_group_id != 0)
    {
        groupOid = port.m_egress_acl_table_group_id;
    }
    else if (acl_stage == ACL_STAGE_INGRESS or acl_stage == ACL_STAGE_EGRESS)
    {
        bool ingress = acl_stage == ACL_STAGE_INGRESS ? true : false;
        // If port ACL table group does not exist, create one
        sai_object_id_t bp_list[] = { SAI_ACL_BIND_POINT_TYPE_PORT };

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

        status = sai_acl_api->create_acl_table_group(&groupOid, gSwitchId, (uint32_t)group_attrs.size(), group_attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create ACL table group, rv:%d", status);
            return false;
        }

        if (ingress)
        {
            port.m_ingress_acl_table_group_id = groupOid;
        }
        else
        {
            port.m_egress_acl_table_group_id = groupOid;
        }

        // Bind this ACL group to port OID
        sai_attribute_t port_attr;
        port_attr.id = ingress ? SAI_PORT_ATTR_INGRESS_ACL : SAI_PORT_ATTR_EGRESS_ACL;
        port_attr.value.oid = groupOid;

        status = sai_port_api->set_port_attribute(port.m_port_id, &port_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to bind port %lx(%s) to ACL table group %lx, rv:%d",
                    port.m_port_id, port.m_alias.c_str(), groupOid, status);
            return false;
        }

        SWSS_LOG_NOTICE("Create ACL table group and bind port %s to it", port.m_alias.c_str());
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
    if (status != SAI_STATUS_SUCCESS) {
        SWSS_LOG_ERROR("Failed to create member in ACL table group %lx for ACL table group %lx, rv:%d",
                table_oid, groupOid, status);
        return false;
    }

    return true;
}

bool PortsOrch::setPortPvid(Port &port, sai_uint32_t pvid)
{
    SWSS_LOG_ENTER();

    if (port.m_rif_id)
    {
        SWSS_LOG_ERROR("pvid setting for router interface is not allowed");
        return false;
    }

    vector<Port> portv;
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
        SWSS_LOG_ERROR("PortsOrch::setPortPvid port type %d not supported", port.m_type);
        return false;
    }

    for (const auto p: portv)
    {
        sai_attribute_t attr;
        attr.id = SAI_PORT_ATTR_PORT_VLAN_ID;
        attr.value.u32 = pvid;

        sai_status_t status = sai_port_api->set_port_attribute(p.m_port_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set pvid %u to port: %s", attr.value.u32, p.m_alias.c_str());
            return false;
        }
        SWSS_LOG_NOTICE("Set pvid %u to port: %s", attr.value.u32, p.m_alias.c_str());
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

bool PortsOrch::validatePortSpeed(sai_object_id_t port_id, sai_uint32_t speed)
{
    sai_attribute_t attr;
    sai_status_t status;

    // "Lazy" query of supported speeds for given port
    // Once received the list will be stored in m_portSupportedSpeeds
    if (!m_portSupportedSpeeds.count(port_id))
    {
        attr.id = SAI_PORT_ATTR_SUPPORTED_SPEED;
        attr.value.u32list.count = 0;
        attr.value.u32list.list = NULL;

        status = sai_port_api->get_port_attribute(port_id, 1, &attr);
        if (status == SAI_STATUS_BUFFER_OVERFLOW)
        {
            std::vector<sai_uint32_t> speeds(attr.value.u32list.count);
            attr.value.u32list.list = speeds.data();
            status = sai_port_api->get_port_attribute(port_id, 1, &attr);
            if (status == SAI_STATUS_SUCCESS)
            {
                m_portSupportedSpeeds[port_id] = speeds;
            }
            else
            {
                SWSS_LOG_ERROR("Failed to get supported speed list for port %lx", port_id);
                return false;
            }
        }
        // TODO: change to macro SAI_STATUS_IS_ATTR_NOT_SUPPORTED once it is fixed in SAI
        // https://github.com/opencomputeproject/SAI/pull/710
        else if (((status) & (~0xFFFF)) == SAI_STATUS_ATTR_NOT_SUPPORTED_0)
        {
            // unable to validate speed if attribute is not supported on platform
            // assuming input value is correct
            SWSS_LOG_WARN("Unable to validate speed for port %lx. Not supported by platform", port_id);
            return true;
        }
        else
        {
            SWSS_LOG_ERROR("Failed to get number of supported speeds for port %lx", port_id);
            return false;
        }
    }

    PortSupportedSpeeds &supp_speeds = m_portSupportedSpeeds[port_id];

    return std::find(supp_speeds.begin(), supp_speeds.end(), speed) != supp_speeds.end();
}

bool PortsOrch::setPortSpeed(sai_object_id_t id, sai_uint32_t speed)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_SPEED;
    attr.value.u32 = speed;

    sai_status_t status = sai_port_api->set_port_attribute(id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set speed %u to port pid:%lx", attr.value.u32, id);
        return false;
    }
    SWSS_LOG_INFO("Set speed %u to port pid:%lx", attr.value.u32, id);
    return true;
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

bool PortsOrch::getQueueType(sai_object_id_t queue_id, string &type)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_TYPE;

    sai_status_t status = sai_queue_api->get_queue_attribute(queue_id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get queue type for queue %lu rv:%d", queue_id, status);
        return false;
    }

    switch (attr.value.s32)
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
        SWSS_LOG_ERROR("Got unsupported queue type %d for %lu queue", attr.value.s32, queue_id);
        throw runtime_error("Got unsupported queue type");
    }

    return true;
}

bool PortsOrch::setPortAutoNeg(sai_object_id_t id, int an)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_AUTO_NEG_MODE;
    switch(an) {
      case 0: // leave it as is?
        return true;
      case 1:
        attr.value.booldata = true;
        break;
      case 2:
        attr.value.booldata = false;
        break;
    }

    sai_status_t status = sai_port_api->set_port_attribute(id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set AN %u to port pid:%lx", attr.value.u32, id);
        return false;
    }
    SWSS_LOG_INFO("Set AN %u to port pid:%lx", attr.value.u32, id);
    return true;
}

bool PortsOrch::setPortFecMode(sai_object_id_t id, int fec)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_FEC_MODE;
    switch(fec) {
      case 0:
        attr.value.u32 = SAI_PORT_FEC_MODE_NONE;
        break;
      case 1:
        attr.value.u32 = SAI_PORT_FEC_MODE_RS;
        break;
      case 2:
        attr.value.u32 = SAI_PORT_FEC_MODE_FC;
        break;
    }

    sai_status_t status = sai_port_api->set_port_attribute(id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set FEC %u to port pid:%lx", attr.value.u32, id);
        return false;
    }
    SWSS_LOG_INFO("Set FEC %u to port pid:%lx", attr.value.u32, id);


    return true;
}

bool PortsOrch::setHostIntfsOperStatus(sai_object_id_t port_id, bool up)
{
    SWSS_LOG_ENTER();

    for (auto it = m_portList.begin(); it != m_portList.end(); it++)
    {
        if (it->second.m_port_id == port_id)
        {
            sai_attribute_t attr;
            attr.id = SAI_HOSTIF_ATTR_OPER_STATUS;
            attr.value.booldata = up;

            sai_status_t status = sai_hostif_api->set_hostif_attribute(it->second.m_hif_id, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_WARN("Failed to set operation status %s to host interface %s",
                              up ? "UP" : "DOWN", it->second.m_alias.c_str());
                return false;
            }
            SWSS_LOG_NOTICE("Set operation status %s to host interface %s",
                            up ? "UP" : "DOWN", it->second.m_alias.c_str());
            return true;
        }
    }
    return false;
}

void PortsOrch::updateDbPortOperStatus(sai_object_id_t id, sai_port_oper_status_t status)
{
    SWSS_LOG_ENTER();

    for (auto it = m_portList.begin(); it != m_portList.end(); it++)
    {
        if (it->second.m_port_id == id)
        {
            vector<FieldValueTuple> tuples;
            FieldValueTuple tuple("oper_status", oper_status_strings.at(status));
            tuples.push_back(tuple);
            m_portTable->set(it->first, tuples);
        }
    }
}

bool PortsOrch::addPort(const set<int> &lane_set, uint32_t speed, int an, int fec)
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

    attr.id = SAI_PORT_ATTR_AUTO_NEG_MODE;
    attr.value.booldata = (an == 0) ? false : true;
    attrs.push_back(attr);

    attr.id = SAI_PORT_ATTR_FEC_MODE;
    attr.value.u32 = fec;
    attrs.push_back(attr);

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

    sai_status_t status = sai_port_api->remove_port(port_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove port %lx, rv:%d", port_id, status);
        return false;
    }

    SWSS_LOG_NOTICE("Remove port %lx", port_id);

    return true;
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
                string key = sai_serialize_object_id(p.m_port_id) + ":" + FLEX_STAT_COUNTER_POLL_MSECS;

                std::string delimiter = "";
                std::ostringstream counters_stream;
                for (int cntr = SAI_PORT_STAT_IF_IN_OCTETS; cntr <= SAI_PORT_STAT_PFC_7_ON2OFF_RX_PKTS; ++cntr)
                {
                    counters_stream << delimiter << sai_serialize_port_stat(static_cast<sai_port_stat_t>(cntr));
                    delimiter = ",";
                }

                fields.clear();
                fields.emplace_back(PFC_WD_PORT_COUNTER_ID_LIST, counters_stream.str());

                m_flexCounterTable->set(key, fields);

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

        if (op == "SET")
        {
            set<int> lane_set;
            string admin_status;
            string fec_mode;
            uint32_t mtu = 0;
            uint32_t speed = 0;
            int an = -1;
            int fec = -1;

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
                    admin_status = fvValue(i);

                /* Set port MTU */
                if (fvField(i) == "mtu")
                    mtu = (uint32_t)stoul(fvValue(i));

                /* Set port speed */
                if (fvField(i) == "speed")
                    speed = (uint32_t)stoul(fvValue(i));

#if 0
                /* Set port fec */
                if (fvField(i) == "fec")
                    fec_mode = fvValue(i);
#endif
                /* Set autoneg */
                if (fvField(i) == "autoneg")
                    an = (int)stoul(fvValue(i));

                /* Set autoneg */
                if (fvField(i) == "fec")
                    fec = (int)stoul(fvValue(i));
            }

            /* Collect information about all received ports */
            if (lane_set.size())
            {
                m_lanesAliasSpeedMap[lane_set] = make_tuple(alias, speed, an, fec);
            }

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
                        if (platform && (strstr(platform, "barefoot") || strstr(platform, MLNX_PLATFORM_SUBSTRING)))
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
                        if (platform && (strstr(platform, "barefoot") || strstr(platform, MLNX_PLATFORM_SUBSTRING)))
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
                it = consumer.m_toSync.erase(it);
                continue;
            }

            Port p;
            if (!getPort(alias, p))
            {
                SWSS_LOG_ERROR("Failed to get port id by alias:%s", alias.c_str());
            }
            else
            {
                /* Set port speed
                 * 1. Get supported speed list and validate if the target speed is within the list
                 * 2. Get the current port speed and check if it is the same as the target speed
                 * 3. Set port admin status to DOWN before changing the speed
                 * 4. Set port speed
                 */
                if (speed != 0)
                {
                    sai_uint32_t current_speed;

                    if (!validatePortSpeed(p.m_port_id, speed))
                    {
                        SWSS_LOG_ERROR("Failed to set speed %u for port %s. The value is not supported", speed, alias.c_str());
                        it++;
                        continue;
                    }

                    if (getPortSpeed(p.m_port_id, current_speed))
                    {
                        if (speed != current_speed)
                        {
                            if (setPortAdminStatus(p.m_port_id, false))
                            {
                                if (setPortSpeed(p.m_port_id, speed))
                                {
                                    SWSS_LOG_NOTICE("Set port %s speed to %u", alias.c_str(), speed);
                                }
                                else
                                {
                                    SWSS_LOG_ERROR("Failed to set port %s speed to %u", alias.c_str(), speed);
                                    it++;
                                    continue;
                                }
                            }
                            else
                            {
                                SWSS_LOG_ERROR("Failed to set port admin status DOWN to set speed");
                            }
                        }
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to get current speed for port %s", alias.c_str());
                    }
                }

                if (admin_status != "")
                {
                    if (setPortAdminStatus(p.m_port_id, admin_status == "up"))
                    {
                        SWSS_LOG_NOTICE("Set port %s admin status to %s", alias.c_str(), admin_status.c_str());
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to set port %s admin status to %s", alias.c_str(), admin_status.c_str());
                        it++;
                        continue;
                    }
                }

                if (mtu != 0)
                {
                    if (setPortMtu(p.m_port_id, mtu))
                    {
                        p.m_mtu = mtu;
                        SWSS_LOG_NOTICE("Set port %s MTU to %u", alias.c_str(), mtu);
                    }
                    else
                    {
                        SWSS_LOG_ERROR("Failed to set port %s MTU to %u", alias.c_str(), mtu);
                        it++;
                        continue;
                    }
                }


                if(an != -1) {
                    if (setPortAutoNeg(p.m_port_id, an))
                        SWSS_LOG_NOTICE("Set port %s AN to %u", alias.c_str(), an);
                    else
                    {
                        SWSS_LOG_ERROR("Failed to set port %s AN to %u", alias.c_str(), an);
                        it++;
                        continue;
                    }
                }

                if(fec != -1) {
                    if (setPortFecMode(p.m_port_id, fec))
                        SWSS_LOG_NOTICE("Set port %s FEC to %u", alias.c_str(), fec);
                    else
                    {
                        SWSS_LOG_ERROR("Failed to set port %s FEC to %u", alias.c_str(), fec);
                        it++;
                        continue;
                    }
                }
#if 0
                if (fec_mode != "")
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
#endif
            }
        }
        else
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());

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

        string vlan_alias, port_alias;
        vlan_alias = VLAN_PREFIX + to_string(vlan_id);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
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
            SWSS_LOG_ERROR("Failed to locate port %s", port_alias.c_str());
            it = consumer.m_toSync.erase(it);
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

        string lag_alias = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            /* Duplicate entry */
            if (m_portList.find(lag_alias) != m_portList.end())
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (addLag(lag_alias))
                it = consumer.m_toSync.erase(it);
            else
                it++;
        }
        else if (op == DEL_COMMAND)
        {
            Port lag;
            /* Cannot locate LAG */
            if (!getPort(lag_alias, lag))
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
        if (!isInitDone())
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
        FieldValueTuple tuple(name.str(), sai_serialize_object_id(port.m_queue_ids[queueIndex]));
        queueVector.push_back(tuple);

        FieldValueTuple queuePortTuple(
                sai_serialize_object_id(port.m_queue_ids[queueIndex]),
                sai_serialize_object_id(port.m_port_id));
        queuePortVector.push_back(queuePortTuple);

        FieldValueTuple queueIndexTuple(
                sai_serialize_object_id(port.m_queue_ids[queueIndex]),
                to_string(queueIndex));
        queueIndexVector.push_back(queueIndexTuple);


        string queueType;
        if (getQueueType(port.m_queue_ids[queueIndex], queueType))
        {
            FieldValueTuple queueTypeTuple(
                    sai_serialize_object_id(port.m_queue_ids[queueIndex]),
                    queueType);
            queueTypeVector.push_back(queueTypeTuple);
        }

        string key = sai_serialize_object_id(port.m_queue_ids[queueIndex]) + ":" + FLEX_STAT_COUNTER_POLL_MSECS;

        std::string delimiter = "";
        std::ostringstream counters_stream;
        for (auto it = queueStatIds.begin(); it != queueStatIds.end(); it++)
        {
            counters_stream << delimiter << sai_serialize_queue_stat(*it);
            delimiter = ",";
        }

        vector<FieldValueTuple> fieldValues;
        fieldValues.emplace_back(PFC_WD_QUEUE_COUNTER_ID_LIST, counters_stream.str());

        m_flexCounterTable->set(key, fieldValues);
    }

    m_queueTable->set("", queueVector);
    m_queuePortTable->set("", queuePortVector);
    m_queueIndexTable->set("", queueIndexVector);
    m_queueTypeTable->set("", queueTypeVector);
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

bool PortsOrch::initializePort(Port &p)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("Initializing port alias:%s pid:%lx", p.m_alias.c_str(), p.m_port_id);

    initializePriorityGroups(p);
    initializeQueues(p);

    /* Create host interface */
    addHostIntfs(p, p.m_alias, p.m_hif_id);

#if 0
    // TODO: Assure if_nametoindex(p.m_alias.c_str()) != 0
    p.m_ifindex = if_nametoindex(p.m_alias.c_str());
    if (p.m_ifindex == 0)
    {
        SWSS_LOG_ERROR("Failed to get netdev index alias:%s", p.m_alias.c_str());
        return false;
    }
#endif

    /* Set default port admin status to DOWN */
    /* FIXME: Do we need this? The default port admin status is false */
    setPortAdminStatus(p.m_port_id, false);

    /**
     * Create default database port oper status as DOWN
     * This status will be updated when receiving port_oper_status_notification.
     */
    vector<FieldValueTuple> vector;
    FieldValueTuple tuple("oper_status", "down");
    vector.push_back(tuple);
    m_portTable->set(p.m_alias, vector);

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
    strncpy((char *)&attr.value.chardata, alias.c_str(), HOSTIF_NAME_SIZE);
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

    SWSS_LOG_NOTICE("Remove VLAN %s vid:%hu", vlan.m_alias.c_str(),
            vlan.m_vlan_info.vlan_id);

    m_portList.erase(vlan.m_alias);

    return true;
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

    SWSS_LOG_NOTICE("Remove LAG %s lid:%lx", lag.m_alias.c_str(), lag.m_lag_id);

    m_portList.erase(lag.m_alias);

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
    sai_uint32_t pvid;
    if (getPortPvid(lag, pvid))
    {
        setPortPvid (port, pvid);
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
