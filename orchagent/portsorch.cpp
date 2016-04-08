#include <unistd.h>

#include "portsorch.h"

#include <fstream>
#include <sstream>
#include <set>

#include "net/if.h"

#include "logger.h"

extern sai_switch_api_t *sai_switch_api;
extern sai_vlan_api_t *sai_vlan_api;
extern sai_port_api_t *sai_port_api;
extern sai_router_interface_api_t* sai_router_intfs_api;
extern sai_hostif_api_t* sai_hostif_api;

extern sai_object_id_t gVirtualRouterId;
extern MacAddress gMacAddress;

#define FRONT_PANEL_PORT_VLAN_BASE 1024

PortsOrch::PortsOrch(DBConnector *db, string tableName) :
        Orch(db, tableName)
{
    SWSS_LOG_ENTER();

    int i, j;
    sai_status_t status;
    sai_attribute_t attr;

    /* Get CPU port */
    attr.id = SAI_SWITCH_ATTR_CPU_PORT;

    status = sai_switch_api->get_switch_attribute(1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get CPU port\n");
    }

    m_cpuPort = attr.value.oid;

    /* Set traps to CPU */
    sai_hostif_trap_id_t trap_ids[] = {
        SAI_HOSTIF_TRAP_ID_TTL_ERROR,
        SAI_HOSTIF_TRAP_ID_ARP_REQUEST,
        SAI_HOSTIF_TRAP_ID_ARP_RESPONSE,
        SAI_HOSTIF_TRAP_ID_LLDP
    };

    for (i = 0; i < 4; i++)
    {
        attr.id = SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION;
        attr.value.s32 = SAI_PACKET_ACTION_TRAP;
        status = sai_hostif_api->set_trap_attribute(trap_ids[i], &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set trap attribute\n");
        }
    }

    /* Get port number */
    attr.id = SAI_SWITCH_ATTR_PORT_NUMBER;

    status = sai_switch_api->get_switch_attribute(1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get port number\n");
    }

    m_portCount = attr.value.u32;

    SWSS_LOG_NOTICE("Get port number : %d\n", m_portCount);

    /* Get port list */
    sai_object_id_t *port_list = new sai_object_id_t[m_portCount];
    attr.id = SAI_SWITCH_ATTR_PORT_LIST;
    attr.value.objlist.count = m_portCount;
    attr.value.objlist.list = port_list;

    status = sai_switch_api->get_switch_attribute(1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("\n");
    }

    /* Get port lane info */
    for (i = 0; i < (int)m_portCount; i++)
    {
        sai_uint32_t lanes[4];
        attr.id = SAI_PORT_ATTR_HW_LANE_LIST;
        attr.value.u32list.count = 4;
        attr.value.u32list.list = lanes;

        status = sai_port_api->get_port_attribute(port_list[i], 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to get hardware lane list pid:%llx\n", port_list[i]);
        }

        set<int> tmp_lane_set;
        for (j = 0; j < (int)attr.value.u32list.count; j++)
            tmp_lane_set.insert(attr.value.u32list.list[j]);

        string tmp_lane_str = "";
        for (auto s : tmp_lane_set)
        {
            tmp_lane_str += to_string(s) + " ";
        }
        tmp_lane_str = tmp_lane_str.substr(0, tmp_lane_str.size()-1);

        SWSS_LOG_NOTICE("Get port with lanes pid:%llx lanes:%s\n", port_list[i], tmp_lane_str.c_str());
        m_portListLaneMap[tmp_lane_set] = port_list[i];
    }

    /* Set port hardware learn mode */
    for (i = 0; i < (int)m_portCount; i++)
    {
        attr.id = SAI_PORT_ATTR_FDB_LEARNING;
        attr.value.s32 = SAI_PORT_LEARN_MODE_HW;

        status = sai_port_api->set_port_attribute(port_list[i], &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set port hardware learn mode pid:%llx\n", port_list[i]);
        }
    }
}

bool PortsOrch::getPort(string alias, Port &p)
{
    SWSS_LOG_ENTER();

    if (m_portList.find(alias) == m_portList.end())
        return false;
    p = m_portList[alias];
    return true;
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
        return false;
    }

    return true;
}

void PortsOrch::doTask()
{
    SWSS_LOG_ENTER();

    if (m_toSync.empty())
        return;

    auto it = m_toSync.begin();
    while (it != m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string alias = kfvKey(t);
        string op = kfvOp(t);

        if (op == "SET")
        {
            set<int> lane_set;
            string admin_status;
            for (auto i = kfvFieldsValues(t).begin();
                 i != kfvFieldsValues(t).end(); i++)
            {
                /* Get lane information of a physical port and initialize the port */
                if (fvField(*i) == "lanes")
                {
                    string lane_str;
                    istringstream iss(fvValue(*i));

                    while (getline(iss, lane_str, ','))
                    {
                        int lane = stoi(lane_str);
                        lane_set.insert(lane);
                    }

                }

                /* Set port admin status */
                if (fvField(*i) == "admin_status")
                    admin_status = fvValue(*i);
            }

            if (lane_set.size())
            {
                /* Determine if the lane combination exists in switch */
                if (m_portListLaneMap.find(lane_set) !=
                    m_portListLaneMap.end())
                {
                    sai_object_id_t id = m_portListLaneMap[lane_set];

                    /* Determin if the port has already been initialized before */
                    if (m_portList.find(alias) != m_portList.end() && m_portList[alias].m_port_id == id)
                        SWSS_LOG_NOTICE("Port has already been initialized before alias:%s\n", alias.c_str());
                    else
                    {
                        Port p(alias, Port::PHY_PORT);

                        p.m_index = m_portList.size(); // XXX: Assume no deletion of physical port
                        p.m_port_id = id;

                        /* Initialize the port and create router interface and host interface */
                        if (initializePort(p))
                        {
                            /* Add port to port list */
                            m_portList[alias] = p;
                            SWSS_LOG_NOTICE("Port is initialized alias:%s\n", alias.c_str());

                        }
                        else
                            SWSS_LOG_ERROR("Failed to initialize port alias:%s\n", alias.c_str());
                    }
                }
                else
                    SWSS_LOG_ERROR("Failed to locate port lane combination alias:%s\n", alias.c_str());
            }

            if (admin_status != "")
            {
                Port p;
                if (getPort(alias, p))
                {
                    if (setPortAdminStatus(p.m_port_id, admin_status == "up"))
                        SWSS_LOG_NOTICE("Port is set to admin %s alias:%s\n", admin_status.c_str(), alias.c_str());
                    else
                    {
                        SWSS_LOG_ERROR("Failed to set port to admin %s alias:%s\n", admin_status.c_str(), alias.c_str());
                        it++;
                        continue;
                    }
                }
                else
                    SWSS_LOG_ERROR("Failed to get port id by alias:%s\n", alias.c_str());
            }
        }
        else
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());

        it = m_toSync.erase(it);
    }
}

bool PortsOrch::initializePort(Port &p)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("Initializing port alias:%s pid:%llx\n", p.m_alias.c_str(), p.m_port_id);

    p.m_vlan_id = FRONT_PANEL_PORT_VLAN_BASE + p.m_index;

    /* Set up VLAN */
    if (!setupVlan(p.m_vlan_id, p.m_port_id, p.m_vlan_member_id))
    {
        SWSS_LOG_ERROR("Failed to set up VLAN vid:%hu pid:%llx\n", p.m_vlan_id, p.m_port_id);
        return false;
    }

    /* Set up router interface */
    if (!setupRouterIntfs(gVirtualRouterId, gMacAddress, p.m_vlan_id, p.m_rif_id))
        return false;

    /* Set up host interface */
    if (!setupHostIntfs(p.m_port_id, p.m_alias, p.m_hif_id))
    {
        SWSS_LOG_ERROR("Failed to set up host interface pid:%llx alias:%s\n", p.m_port_id, p.m_alias.c_str());
        return false;
    }

    // XXX: Assure if_nametoindex(p.m_alias.c_str()) != 0
    // XXX: Get port oper status

#if 0
    p.m_ifindex = if_nametoindex(p.m_alias.c_str());
    if (p.m_ifindex == 0)
    {
        SWSS_LOG_ERROR("Failed to get netdev index alias:%s\n", p.m_alias.c_str());
        return false;
    }
#endif

    /* Set port admin status UP */
    if (!setPortAdminStatus(p.m_port_id, true))
    {
        SWSS_LOG_ERROR("Failed to set port admin status UP pid:%llx\n", p.m_port_id);
        return false;
    }
    return true;
}

bool PortsOrch::setupVlan(sai_vlan_id_t vlan_id, sai_object_id_t port_id, sai_object_id_t &vlan_member_id)
{
    SWSS_LOG_ENTER();

    sai_status_t status;

    status = sai_vlan_api->create_vlan(vlan_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create VLAN vid:%hu\n", vlan_id);
        return false;
    }

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_VLAN_MEMBER_ATTR_VLAN_ID;
    attr.value.u16 = vlan_id;
    attrs.push_back(attr);

    attr.id = SAI_VLAN_MEMBER_ATTR_PORT_ID;
    attr.value.oid = port_id;
    attrs.push_back(attr);

    status = sai_vlan_api->create_vlan_member(&vlan_member_id, attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create VLAN member vid:%hu pid:%llx\n", vlan_id, port_id);
        return false;
    }

    attr.id = SAI_PORT_ATTR_PORT_VLAN_ID;
    attr.value.u16 = vlan_id;
    status = sai_port_api->set_port_attribute(port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set port VLAN ID pid:%llx\n", port_id);
        return false;
    }

    return true;
}

bool PortsOrch::setupRouterIntfs(sai_object_id_t virtual_router_id, MacAddress mac_address,
                      sai_vlan_id_t vlan_id, sai_object_id_t &router_intfs_id)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    attr.value.oid = virtual_router_id;
    attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_VLAN;
    attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, mac_address.getMac(), sizeof(sai_mac_t));
    attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_VLAN_ID;
    attr.value.u16 = vlan_id;
    attrs.push_back(attr);

    sai_status_t status = sai_router_intfs_api->create_router_interface(&router_intfs_id, attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create router interface\n");
        return false;
    }

    return true;
}

bool PortsOrch::setupHostIntfs(sai_object_id_t id, string alias, sai_object_id_t &host_intfs_id)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_HOSTIF_ATTR_TYPE;
    attr.value.s32 = SAI_HOSTIF_TYPE_NETDEV;
    attrs.push_back(attr);

    attr.id = SAI_HOSTIF_ATTR_RIF_OR_PORT_ID;
    attr.value.oid = id;
    attrs.push_back(attr);

    attr.id = SAI_HOSTIF_ATTR_NAME;
    strncpy((char *)&attr.value.chardata, alias.c_str(), HOSTIF_NAME_SIZE);
    attrs.push_back(attr);

    sai_status_t status = sai_hostif_api->create_hostif(&host_intfs_id, attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create host interface\n");
        return false;
    }

    return true;
}
