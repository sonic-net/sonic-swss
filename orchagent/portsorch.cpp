#include "portsorch.h"

#include <fstream>
#include <sstream>
#include <set>
#include "assert.h"

#include "net/if.h"

#include "logger.h"
#include "schema.h"

extern sai_switch_api_t *sai_switch_api;
extern sai_port_api_t *sai_port_api;
extern sai_vlan_api_t *sai_vlan_api;
extern sai_lag_api_t *sai_lag_api;
extern sai_hostif_api_t* sai_hostif_api;

#define VLAN_PREFIX         "Vlan"
#define DEFAULT_VLAN_ID     1

PortsOrch::PortsOrch(DBConnector *db, vector<string> tableNames) :
        Orch(db, tableNames)
{
    SWSS_LOG_ENTER();

    /* Initialize Counter Table */
    DBConnector *counter_db = new DBConnector(COUNTERS_DB, "localhost", 6379, 0);
    m_counterTable = new Table(counter_db, COUNTERS_PORT_NAME_MAP);

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
        SWSS_LOG_ERROR("Failed to get port list");
    }

    /* Get port hardware lane info */
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

    /* Set port to hardware learn mode */
    for (i = 0; i < (int)m_portCount; i++)
    {
        attr.id = SAI_PORT_ATTR_FDB_LEARNING;
        attr.value.s32 = SAI_PORT_LEARN_MODE_HW;

        status = sai_port_api->set_port_attribute(port_list[i], &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set port to hardware learn mode pid:%llx\n", port_list[i]);
        }
    }

    /* Get default VLAN member list */
    sai_object_id_t *vlan_member_list = new sai_object_id_t[m_portCount];
    attr.id = SAI_VLAN_ATTR_MEMBER_LIST;
    attr.value.objlist.count = m_portCount;
    attr.value.objlist.list = vlan_member_list;

    status = sai_vlan_api->get_vlan_attribute(DEFAULT_VLAN_ID, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get default VLAN member list");
    }

    /* Remove port from default VLAN */
    for (i = 0; i < (int)m_portCount; i++)
    {
        status = sai_vlan_api->remove_vlan_member(vlan_member_list[i]);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove port from default VLAN %d", i);
        }
    }
}

bool PortsOrch::isInitDone()
{
    return m_initDone;
}

bool PortsOrch::getPort(string alias, Port &p)
{
    if (m_portList.find(alias) == m_portList.end())
        return false;
    p = m_portList[alias];
    return true;
}

void PortsOrch::setPort(string alias, Port p)
{
    m_portList[alias] = p;
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

void PortsOrch::doPortTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string alias = kfvKey(t);
        string op = kfvOp(t);

        /* Get notification from application */
        /* portsyncd application:
         * When portsorch receives 'ConfigDone' message, it indicates port initialization
         * procedure is done. Before port initialization procedure, none of other tasks
         * are executed.
         */
        if (alias == "ConfigDone")
        {
            /* portsyncd restarting case:
             * When portsyncd restarts, duplicate notifications may be received.
             */
            if (!m_initDone)
            {
                m_initDone = true;
                SWSS_LOG_INFO("Get ConfigDone notification from portsyncd.\n");
            }

            it = consumer.m_toSync.erase(it);
            return;
        }

        if (op == "SET")
        {
            set<int> lane_set;
            string admin_status;
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
                        Port p(alias, Port::PHY);

                        p.m_index = m_portList.size(); // TODO: Assume no deletion of physical port
                        p.m_port_id = id;

                        /* Initialize the port and create router interface and host interface */
                        if (initializePort(p))
                        {
                            /* Add port to port list */
                            m_portList[alias] = p;
                            /* Add port name map to counter table */
                            std::stringstream ss;
                            ss << hex << p.m_port_id;
                            FieldValueTuple tuple(p.m_alias, ss.str());
                            vector<FieldValueTuple> vector;
                            vector.push_back(tuple);
                            m_counterTable->set("", vector);

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

        it = consumer.m_toSync.erase(it);
    }
}

void PortsOrch::doVlanTask(Consumer &consumer)
{
    if (!isInitDone())
        return;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);

        /* Assert the key starts with "Vlan" */
        assert(!strncmp(key.c_str(), VLAN_PREFIX, 4));

        key = key.substr(4);
        size_t found = key.find(':');
        int vlan_id;
        string vlan_alias, port_alias;
        if (found == string::npos)
            vlan_id = stoi(key);
        else
        {
            vlan_id = stoi(key.substr(0, found));
            port_alias = key.substr(found+1);
        }

        vlan_alias = VLAN_PREFIX + to_string(vlan_id);
        string op = kfvOp(t);

        /* Manipulate VLAN when port_alias is empty */
        if (port_alias == "")
        {
            if (op == SET_COMMAND)
            {
                /* Duplicate entry */
                if (m_portList.find(vlan_alias) != m_portList.end())
                {
                    SWSS_LOG_ERROR("Duplicate VLAN entry alias:%s", vlan_alias.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                if (addVlan(vlan_alias))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else if (op == DEL_COMMAND)
            {
                Port vlan;
                assert(getPort(vlan_alias, vlan));

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
        /* Manipulate member */
        else
        {
            assert(m_portList.find(vlan_alias) != m_portList.end());
            Port vlan, port;
            assert(getPort(vlan_alias, vlan));
            assert(getPort(port_alias, port));

            if (op == SET_COMMAND)
            {
                /* Duplicate entry */
                if (vlan.m_members.find(port_alias) != vlan.m_members.end())
                {
                    SWSS_LOG_ERROR("Duplicate VLAN member entry vlan:%s port:%s",
                            vlan_alias.c_str(), port_alias.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                /* Assert the port doesn't belong to any VLAN */
                assert(!port.m_vlan_id && !port.m_vlan_member_id);

                if (addVlanMember(vlan, port))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else if (op == DEL_COMMAND)
            {
                assert(vlan.m_members.find(port_alias) != vlan.m_members.end());

                /* Assert the port belongs the a VLAN */
                assert(port.m_vlan_id && port.m_vlan_member_id);

                if (removeVlanMember(vlan, port))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else
            {
                SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
                it = consumer.m_toSync.erase(it);
            }
        }
    }
}

void PortsOrch::doLagTask(Consumer &consumer)
{
    if (!isInitDone())
        return;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        size_t found = key.find(':');
        string lag_alias, port_alias;
        if (found == string::npos)
            lag_alias = key;
        else
        {
            lag_alias = key.substr(0, found);
            port_alias = key.substr(found+1);
        }

        string op = kfvOp(t);

        /* Manipulate LAG when port_alias is empty */
        if (port_alias == "")
        {
            if (op == SET_COMMAND)
            {
                /* Duplicate entry */
                if (m_portList.find(lag_alias) != m_portList.end())
                {
                    SWSS_LOG_ERROR("Duplicate LAG entry alias:%s", lag_alias.c_str());
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
                assert(getPort(lag_alias, lag));

                if (removeLag(lag))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else
            {
                SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
                it = consumer.m_toSync.erase(it);
            }
        }
        /* Manipulate member */
        else
        {
            assert(m_portList.find(lag_alias) != m_portList.end());
            Port lag, port;
            assert(getPort(lag_alias, lag));
            assert(getPort(port_alias, port));

            if (op == SET_COMMAND)
            {
                /* Duplicate entry */
                if (lag.m_members.find(port_alias) != lag.m_members.end())
                {
                    SWSS_LOG_ERROR("Duplicate LAG member entry lag:%s port:%s",
                            lag_alias.c_str(), port_alias.c_str());
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
            else if (op == DEL_COMMAND)
            {
                assert(lag.m_members.find(port_alias) != lag.m_members.end());

                /* Assert the port belongs to a LAG */
                assert(port.m_lag_id && port.m_lag_member_id);

                if (removeLagMember(lag, port))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else
            {
                SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
                it = consumer.m_toSync.erase(it);
            }
        }
    }
}

void PortsOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.m_consumer->getTableName();

    if (table_name == APP_PORT_TABLE_NAME)
        doPortTask(consumer);
    else if (table_name == APP_VLAN_TABLE_NAME)
        doVlanTask(consumer);
    else if (table_name == APP_LAG_TABLE_NAME)
        doLagTask(consumer);
}

bool PortsOrch::initializePort(Port &p)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("Initializing port alias:%s pid:%llx\n", p.m_alias.c_str(), p.m_port_id);

    /* Set up host interface */
    if (!addHostIntfs(p.m_port_id, p.m_alias, p.m_hif_id))
    {
        SWSS_LOG_ERROR("Failed to set up host interface pid:%llx alias:%s\n", p.m_port_id, p.m_alias.c_str());
        return false;
    }

    // TODO: Assure if_nametoindex(p.m_alias.c_str()) != 0
    // TODO: Get port oper status

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

bool PortsOrch::addHostIntfs(sai_object_id_t id, string alias, sai_object_id_t &host_intfs_id)
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

bool PortsOrch::addVlan(string vlan_alias)
{
    SWSS_LOG_ENTER();

    sai_vlan_id_t vlan_id = stoi(vlan_alias.substr(4));
    sai_status_t status = sai_vlan_api->create_vlan(vlan_id);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create VLAN %s vid:%hu", vlan_alias.c_str(), vlan_id);
        return false;
    }

    SWSS_LOG_NOTICE("Create an empty VLAN %s vid:%hu", vlan_alias.c_str(), vlan_id);

    Port vlan(vlan_alias, Port::VLAN);
    vlan.m_vlan_id = vlan_id;
    vlan.m_members = set<string>();
    m_portList[vlan_alias] = vlan;

    return true;
}

bool PortsOrch::removeVlan(Port vlan)
{
    SWSS_LOG_ENTER();

    /* Retry when the VLAN still has members */
    if (vlan.m_members.size() > 0)
    {
        SWSS_LOG_ERROR("Failed to remove non-empty VLAN %s", vlan.m_alias.c_str());
        return false;
    }

    sai_status_t status = sai_vlan_api->remove_vlan(vlan.m_vlan_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove VLAN %s vid:%hu", vlan.m_alias.c_str(), vlan.m_vlan_id);
        return false;
    }

    SWSS_LOG_NOTICE("Remove VLAN %s vid:%hu", vlan.m_alias.c_str(), vlan.m_vlan_id);

    m_portList.erase(vlan.m_alias);

    return true;
}

bool PortsOrch::addVlanMember(Port vlan, Port port)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_VLAN_MEMBER_ATTR_VLAN_ID;
    attr.value.u16 = vlan.m_vlan_id;
    attrs.push_back(attr);

    attr.id = SAI_VLAN_MEMBER_ATTR_PORT_ID;
    attr.value.oid = port.m_port_id;
    attrs.push_back(attr);

    sai_object_id_t vlan_member_id;
    sai_status_t status = sai_vlan_api->create_vlan_member(&vlan_member_id, attrs.size(), attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to add member %s to VLAN %s vid:%hu pid:%llx",
                port.m_alias.c_str(), vlan.m_alias.c_str(), vlan.m_vlan_id, port.m_port_id);
        return false;
    }

    SWSS_LOG_NOTICE("Add member %s to VLAN %s vid:%hu pid%llx",
            port.m_alias.c_str(), vlan.m_alias.c_str(), vlan.m_vlan_id, port.m_port_id);

    attr.id = SAI_PORT_ATTR_PORT_VLAN_ID;
    attr.value.u16 = vlan.m_vlan_id;

    status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set port VLAN ID vid:%hu pid:%llx",
                vlan.m_vlan_id, port.m_port_id);
        return false;
    }

    SWSS_LOG_NOTICE("Set port %s VLAN ID to %hu", port.m_alias.c_str(), vlan.m_vlan_id);

    port.m_vlan_id = vlan.m_vlan_id;
    port.m_port_vlan_id = vlan.m_vlan_id;
    port.m_vlan_member_id = vlan_member_id;
    m_portList[port.m_alias] = port;
    vlan.m_members.insert(port.m_alias);
    m_portList[vlan.m_alias] = vlan;

    return true;
}

bool PortsOrch::removeVlanMember(Port vlan, Port port)
{
    SWSS_LOG_ENTER();

    sai_status_t status = sai_vlan_api->remove_vlan_member(port.m_vlan_member_id);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove member %s from VLAN %s vid:%hx vmid:%llx",
                port.m_alias.c_str(), vlan.m_alias.c_str(), vlan.m_vlan_id, port.m_vlan_member_id);
        return false;
    }

    SWSS_LOG_ERROR("Remove member %s from VLAN %s lid:%hx vmid:%llx",
            port.m_alias.c_str(), vlan.m_alias.c_str(), vlan.m_vlan_id, port.m_vlan_member_id);

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_PORT_VLAN_ID;
    attr.value.u16 = DEFAULT_PORT_VLAN_ID;

    status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to reset port VLAN ID to DEFAULT_PORT_VLAN_ID pid:%llx",
                port.m_port_id);
        return false;
    }

    port.m_vlan_id = 0;
    port.m_port_vlan_id = DEFAULT_PORT_VLAN_ID;
    port.m_vlan_member_id = 0;
    m_portList[port.m_alias] = port;
    vlan.m_members.erase(port.m_alias);
    m_portList[vlan.m_alias] = vlan;

    return true;
}

bool PortsOrch::addLag(string lag_alias)
{
    SWSS_LOG_ENTER();

    sai_object_id_t lag_id;
    sai_status_t status = sai_lag_api->create_lag(&lag_id, 0, NULL);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create LAG %s lid:%llx", lag_alias.c_str(), lag_id);
        return false;
    }

    SWSS_LOG_ERROR("Create an empty LAG %s lid:%llx", lag_alias.c_str(), lag_id);

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

    sai_status_t status = sai_lag_api->remove_lag(lag.m_lag_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove LAG %s lid:%llx\n", lag.m_alias.c_str(), lag.m_lag_id);
        return false;
    }

    SWSS_LOG_ERROR("Remove LAG %s lid:%llx\n", lag.m_alias.c_str(), lag.m_lag_id);

    m_portList.erase(lag.m_alias);

    return true;
}

bool PortsOrch::addLagMember(Port lag, Port port)
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
    sai_status_t status = sai_lag_api->create_lag_member(&lag_member_id, attrs.size(), attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to add member %s to LAG %s lid:%llx pid:%llx\n",
                port.m_alias.c_str(), lag.m_alias.c_str(), lag.m_lag_id, port.m_port_id);
        return false;
    }

    SWSS_LOG_ERROR("Add member %s to LAG %s lid:%llx pid:%llx\n",
            port.m_alias.c_str(), lag.m_alias.c_str(), lag.m_lag_id, port.m_port_id);

    port.m_lag_id = lag.m_lag_id;
    port.m_lag_member_id = lag_member_id;
    m_portList[port.m_alias] = port;
    lag.m_members.insert(port.m_alias);

    m_portList[lag.m_alias] = lag;

    return true;
}

bool PortsOrch::removeLagMember(Port lag, Port port)
{
    sai_status_t status = sai_lag_api->remove_lag_member(port.m_lag_member_id);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove member %s from LAG %s lid:%llx lmid:%llx",
                port.m_alias.c_str(), lag.m_alias.c_str(), lag.m_lag_id, port.m_lag_member_id);
        return false;
    }

    SWSS_LOG_ERROR("Remove member %s from LAG %s lid:%llx lmid:%llx",
            port.m_alias.c_str(), lag.m_alias.c_str(), lag.m_lag_id, port.m_lag_member_id);

    port.m_lag_id = 0;
    port.m_lag_member_id = 0;
    m_portList[port.m_alias] = port;
    lag.m_members.erase(port.m_alias);
    m_portList[lag.m_alias] = lag;

    return true;
}
