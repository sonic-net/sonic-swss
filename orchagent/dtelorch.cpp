#include "dtelorch.h"

using namespace std;
using namespace swss;


dtel_event_lookup_t dTelEventLookup =
{
    { DTEL_EVENT_TYPE_FLOW_STATE                         SAI_DTEL_EVENT_TYPE_FLOW_STATE },
    { DTEL_EVENT_TYPE_FLOW_REPORT_ALL_PACKETS            SAI_DTEL_EVENT_TYPE_FLOW_REPORT_ALL_PACKETS },
    { DTEL_EVENT_TYPE_FLOW_TCPFLAG                       SAI_DTEL_EVENT_TYPE_FLOW_TCPFLAG },
    { DTEL_EVENT_TYPE_QUEUE_REPORT_THRESHOLD_BREACH      SAI_DTEL_EVENT_TYPE_QUEUE_REPORT_THRESHOLD_BREACH },
    { DTEL_EVENT_TYPE_QUEUE_REPORT_TAIL_DROP             SAI_DTEL_EVENT_TYPE_QUEUE_REPORT_TAIL_DROP },
    { DTEL_EVENT_TYPE_DROP_REPORT                        SAI_DTEL_EVENT_TYPE_DROP_REPORT }
};

DTelOrch::DTelOrch(DBConnector *db, vector<string> tableNames, PortsOrch *portOrch) :
        Orch(db, tableNames),
        m_portOrch(portOrch),
{
    SWSS_LOG_ENTER();
}

DTelOrch::~DTelOrch()
{
}

bool DTelOrch::intSessionExists(const string& name)
{
    SWSS_LOG_ENTER();

    return m_dTelINTSessionTable.find(name) != m_dTelINTSessionTable.end();
}

bool DTelOrch::getINTSessionOid(const string& name, sai_object_id_t& oid)
{
    SWSS_LOG_ENTER();

    if (!intSessionExists(name))
    {
        return false;
    }

    oid = m_dTelINTSessionTable[name].intSessionOid;

    return true;
}

bool DTelOrch::increaseINTSessionRefCount(const string& name)
{
    SWSS_LOG_ENTER();

    if (!intSessionExists(name))
    {
        SWSS_LOG_ERROR("DTEL ERROR: INT session does not exist");
        return false;
    }

    ++m_dTelINTSessionTable[name].refCount;

    return true;
}

bool DTelOrch::decreaseINTSessionRefCount(const string& name)
{
    SWSS_LOG_ENTER();

    if (!intSessionExists(name))
    {
        SWSS_LOG_ERROR("DTEL ERROR: INT session does not exist");
        return false;
    }

    if (m_dTelINTSessionTable[name].refCount <= 0)
    {
        SWSS_LOG_ERROR("DTEL ERROR: Session reference counter could not be less or equal than 0");
        return false;
    }

    --m_dTelINTSessionTable[name].refCount;

    return true;
}

int64_t DTelOrch::getINTSessionRefCount(const string& name)
{
    if (!intSessionExists(name))
    {
        SWSS_LOG_ERROR("DTEL ERROR: INT session does not exist");
        return -1;
    }

    return m_dTelINTSessionTable[name].refCount;
}

bool DTelOrch::reportSessionExists(const string& name)
{
    SWSS_LOG_ENTER();

    return m_dTelReportSessionTable.find(name) != m_dTelReportSessionTable.end();
}

bool DTelOrch::getReportSessionOid(const string& name, sai_object_id_t& oid)
{
    SWSS_LOG_ENTER();

    if (!reportSessionExists(name))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Report session does not exist");
        return false;
    }

    oid = m_dTelReportSessionTable[name].reportSessionOid;

    return true;
}

bool DTelOrch::increaseReportSessionRefCount(const string& name)
{
    SWSS_LOG_ENTER();

    if (!reportSessionExists(name))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Report session does not exist");
        return false;
    }

    ++m_dTelReportSessionTable[name].refCount;

    return true;
}

bool DTelOrch::decreaseReportSessionRefCount(const string& name)
{
    SWSS_LOG_ENTER();

    if (!reportSessionExists(name))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Report session does not exist");
        return false;
    }

    if (m_dTelReportSessionTable[name].refCount <= 0)
    {
        SWSS_LOG_ERROR("DTEL ERROR: Session reference counter could not be less or equal than 0");
        return false;
    }

    --m_dTelReportSessionTable[name].refCount;

    return true;
}

int64_t DTelOrch::getReportSessionRefCount(const string& name)
{
    if (!reportSessionExists(name))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Report session does not exist");
        return -1;
    }

    return m_dTelReportSessionTable[name].refCount;
}

bool DTelOrch::isQueueReportEnabled(const string& port, const string& queue)
{
    SWSS_LOG_ENTER();

    DTelPortEntry port_entry = m_dTelPortTable.find(port);

    if (port_entry == m_dTelPortTable.end() || 
        port_entry.queueTable.find(queue) == port_entry.queueTable.end())
    {
        return false;
    }

    return true;
}

bool DTelOrch::getQueueReportOid(const string& port, const string& queue)
{
    SWSS_LOG_ENTER();

    if (!isQueueReportEnabled(port, queue))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Queue report not enabled on port %s, queue %s", port, queue);
        return false;
    }

    oid = m_dTelPortTable[port].queueTable[queue];

    return true;
}

void DTelOrch::removePortQueue(const string& port, const string& queue)
{
    if (!isQueueReportEnabled(port, queue))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Queue report not enabled on port %s, queue %s", port, queue);
        return false;
    }

    m_dTelPortTable[port].queueTable.erase(queue);

    if (m_dTelPortTable[port].queueTable.empty())
    {
        m_dTelPortTable.erase(port);
    }
}

void DTelOrch::addPortQueue(const string& port, const string& queue, const sai_object_id_t& queue_report_oid)
{
    if (isQueueReportEnabled(port, queue))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Queue report already enabled on port %s, queue %s", port, queue);
        return false;
    }

    m_dTelPortTable[port] = new DTelPortEntry();
    m_dTelPortTable[port].queueTable[queue] = queue_report_oid;
}

bool DTelOrch::isEventConfigured(const string& event)
{
    SWSS_LOG_ENTER();

    if (m_dtelEventTable.find(event) == m_dtelEventTable.end())
    {
        return false;
    }

    return true;
}

bool DTelOrch::getEventOid(const string& event)
{
    SWSS_LOG_ENTER();

    if (!isEventConfigured(event))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Event not configured %s", event);
        return false;
    }

    oid = m_dtelEventTable[event].eventOid;

    return true;
}

void DTelOrch::addEvent(const string& event, const sai_object_id_t& event_oid, const string& report_session_id)
{
    if (isEventConfigured(event))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Event is already configured %s", event);
        return;
    }

    m_dtelEventTable[event] = new m_dtelEventTable(event_oid, report_session_id);

    increaseReportSessionRefCount(report_session_id);
}

void DTelOrch::removeEvent(const string& event)
{
    if (!isEventConfigured(event))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Event not configured %s", event);
        return;
    }

    string& report_session_id = m_dtelEventTable[event].reportSessionId;

    decreaseReportSessionRefCount(report_session_id);

    m_dtelEventTable.erase(event);
}

void DTelOrch::doDtelTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find(':');
        string table_id = key.substr(0, found);
        string attr = key.substr(found + 1);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            if (attr == INT_ENDPOINT)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_INT_ENDPOINT_ENABLE;
                attr.value.booldata = true;
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to enable INT endpoint mode");
                    return;
                }
            }
            else if (attr == INT_TRANSIT)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_INT_TRANSIT_ENABLE;
                attr.value.booldata = true;
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to enable INT transit mode");
                    return;
                }
            }
            else if (attr == POSTCARD)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_POSTCARD_ENABLE;
                attr.value.booldata = true;
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to enable DTel postcard");
                    return;
                }
            }
            else if (attr == DROP_REPORT)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_DROP_REPORT_ENABLE;
                attr.value.booldata = true;
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to enable drop report");
                    return;
                }
            }
            else if (attr == QUEUE_REPORT)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_QUEUE_REPORT_ENABLE;
                attr.value.booldata = true;
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to enable queue report");
                    return;
                }
            }
            else if (attr == SWITCH_ID)
            {
                auto i = kfvFieldsValues(t);

                attr.id = SAI_SWITCH_ATTR_DTEL_SWITCH_ID;
                attr.value.u32 = to_uint<uint32_t>(fvValue(i));
                status = sai_switch_api->set_switch_attribute(0, &attr);  
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to set switch id");
                    return;
                } 
            }
            else if (attr == FLOW_STATE_CLEAR_CYCLE)
            {
                auto i = kfvFieldsValues(t);

                attr.id = SAI_SWITCH_ATTR_DTEL_FLOW_STATE_CLEAR_CYCLE;
                attr.value.u16 = to_uint<uint16_t>(fvValue(i));
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to set Dtel flow state clear cycle");
                    return;
                }   
            }
            else if (attr == LATENCY_SENSITIVITY)
            {
                auto i = kfvFieldsValues(t);

                attr.id = SAI_SWITCH_ATTR_DTEL_LATENCY_SENSITIVITY;
                attr.value.u16 = to_uint<uint16_t>(fvValue(i));
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to set Dtel latency sensitivity");
                    return;
                }
            }
            else if (attr == SINK_PORT_LIST)
            {
                vector<sai_object_id_t> port_list;
                Port port;
                attr.id = = SAI_SWITCH_ATTR_DTEL_SINK_PORT_LIST;

                for (auto i : kfvFieldsValues(t))
                {
                    if (!m_portOrch->getPort(fvField(i), port))
                    {
                        SWSS_LOG_ERROR("DTEL ERROR: Failed to process port for INT sink port list. Port %s doesn't exist", fvField(i).c_str());
                        return;
                    }

                    port_list.push_back(port.m_port_id);
                }

                if (port_list.size() == 0)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Invalid INT sink port list");
                    return;
                }

                attr.value.objlist.count = port_list.size();
                attr.value.objlist.list = port_list;
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to set INT sink port list");
                    return;
                }
            }
            else if (attr == INT_L4_DSCP)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_INT_L4_DSCP;

                if (((uint32_t)kfvFieldsValues(tuple).size()) != 2)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: INT L4 DSCP attribute requires value and mask");
                    return;
                }

                for (auto i : kfvFieldsValues(t))
                {
                    if (fvField(i) == INT_L4_DSCP_VALUE)
                    {
                        attr.value.ternaryfield.value = to_uint<uint8_t>(fvValue(i));
                    }
                    else if (fvField(i) == INT_L4_DSCP_MASK)
                    {
                        attr.value.ternaryfield.mask = to_uint<uint8_t>(fvValue(i));
                    }
                }

                if (attr.value.ternaryfield.value == 0 ||
                    attr.value.ternaryfield.mask == 0)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Invalid INT L4 DSCP value/mask");
                    return;
                }

                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to set INT L4 DSCP value/mask");
                    return;
                }
            }

        }
        else if (op == DEL_COMMAND)
        {
            if (attr == INT_ENDPOINT)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_INT_ENDPOINT_ENABLE;
                attr.value.booldata = false;
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to disable INT endpoint mode");
                    return;
                }
            }
            else if (attr == INT_TRANSIT)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_INT_TRANSIT_ENABLE;
                attr.value.booldata = false;
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to disable INT transit mode");
                    return;
                }
            }
            else if (attr == POSTCARD)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_POSTCARD_ENABLE;
                attr.value.booldata = false;
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to disable postcard mode");
                    return;
                }
            }
            else if (attr == DROP_REPORT)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_DROP_REPORT_ENABLE;
                attr.value.booldata = false;
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to disable drop report");
                    return;
                }
            }
            else if (attr == QUEUE_REPORT)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_QUEUE_REPORT_ENABLE;
                attr.value.booldata = false;
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to disable queue report");
                    return;
                }
            }
            else if (attr == SWITCH_ID)
            {
                auto i = kfvFieldsValues(t);

                attr.id = SAI_SWITCH_ATTR_DTEL_SWITCH_ID;
                attr.value.u32 = 0;
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to reset switch id");
                    return;
                }  
            }
            else if (attr == FLOW_STATE_CLEAR_CYCLE)
            {
                auto i = kfvFieldsValues(t);

                attr.id = SAI_SWITCH_ATTR_DTEL_FLOW_STATE_CLEAR_CYCLE;
                attr.value.u16 = 0;
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to reset flow state clear cycle");
                    return;
                } 
            }
            else if (attr == LATENCY_SENSITIVITY)
            {
                auto i = kfvFieldsValues(t);

                attr.id = SAI_SWITCH_ATTR_DTEL_LATENCY_SENSITIVITY;
                attr.value.u16 = 0;
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to reset latency sensitivity");
                    return;
                }
            }
            else if (attr == SINK_PORT_LIST)
            {
                attr.id = = SAI_SWITCH_ATTR_DTEL_SINK_PORT_LIST;

                attr.value.objlist.count = 0;
                attr.value.objlist.list = {};
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to reset sink port list");
                    return;
                }
            }
            else if (attr == INT_L4_DSCP)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_INT_L4_DSCP;
                attr.value.ternaryfield.value = 0;
                attr.value.ternaryfield.mask = 0;
                status = sai_switch_api->set_switch_attribute(0, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to reset INT L4 DSCP value/mask");
                    return;
                }
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

void DTelOrch::deleteReportSession(string &report_session_id)
{
    sai_object_id_t report_session_oid;

    if (!reportSessionExists(report_session_id))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Report session %s does not exist", report_session_id);
        return;
    }

    report_session_oid = getReportSessionOid(report_session_id);

    status = sai_dtel_api->remove_dtel_report_session(report_session_oid);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("DTEL ERROR: Failed to delete report session %s", report_session_id);
        return;
    }

    m_dTelReportSessionTable.erase(report_session_id);
}

void DTelOrch::doDtelReportSessionTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        sai_object_id_t report_session_oid;

        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find(':');
        string table_id = key.substr(0, found);
        string report_session_id = key.substr(found + 1);

        /* If report session already exists, delete it first */
        if (reportSessionExists(report_session_id))
        {
            deleteReportSession(report_session_id);
        }

        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            vector<sai_attribute_t> report_session_attr;
            vector<sai_ip4_t> dst_ip_list;
            sai_attribute_t rs_attr;
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == SRC_IP)
                {
                    IpPrefix ip(fvValue(i));
                    rs_attr.id = SAI_SWITCH_ATTR_DTEL_REPORT_SRC_IP;
                    rs_attr.value.ip4 = ip.getIp().getV4Addr();
                    report_session_attr.push_back(rs_attr);
                }
                else if (fvField(i) == DST_IP_LIST)
                {
                    size_t prev = 0;
                    size_t next = fvValue(i).find(';');
                    while (next != npos)
                    {
                        IpPrefix ip(fvValue(i).substr(prev, next - prev));
                        dst_ip_list.push_back(ip.getIp().getV4Addr());
                        prev = next + 1;
                        next = fvValue(i).find(';', prev);
                    }

                    /* Add the last IP */
                    IpPrefix ip(fvValue(i).substr(prev));
                    dst_ip_list.push_back(ip.getIp().getV4Addr());

                    rs_attr.id = SAI_SWITCH_ATTR_DTEL_REPORT_DST_IP_LIST;
                    rs_attr.value.ip4list.count = dst_ip_list.size();
                    rs_attr.value.ip4list.list = dst_ip_list;
                    report_session_attr.push_back(rs_attr);
                }
                else if (fvField(i) == VRF)
                {
                    rs_attr.id = SAI_SWITCH_ATTR_DTEL_REPORT_VIRTUAL_ROUTER_ID;
                    /* TODO: find a way to convert vrf to oid */
                    rs_attr.id.value.oid = gVirtualRouterId;
                    report_session_attr.push_back(rs_attr);
                }
                else if (fvField(i) == TRUNCATE_SIZE)
                {
                    rs_attr.id = SAI_SWITCH_ATTR_DTEL_REPORT_TRUNCATE_SIZE;
                    rs_attr.value.u16 = to_uint<uint16_t>(fvValue(i));
                    report_session_attr.push_back(rs_attr);
                }
                else if (fvField(i) == UDP_DEST_PORT)
                {
                    rs_attr.id = SAI_SWITCH_ATTR_DTEL_REPORT_UDP_DST_PORT;
                    rs_attr.value.u16 = to_uint<uint16_t>(fvValue(i));
                    report_session_attr.push_back(rs_attr);
                }
            }

            status = sai_dtel_api->create_dtel_report_session(&report_session_oid, 
                report_session_attr.size(), report_session_attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("DTEL ERROR: Failed to set INT EP report session %s", report_session_id);
                return;
            }

            m_dTelReportSessionTable[report_session_id] = new DTelReportSessionEntry(report_session_oid, 0);
        }
        else if (op == DEL_COMMAND)
        {
            if(getReportSessionRefCount(report_session_id) != 0)
            {
                SWSS_LOG_ERROR("DTEL ERROR: References still exist on report session %s", report_session_id);
                return;
            }

            deleteReportSession(report_session_id);
        }

        it = consumer.m_toSync.erase(it);
    }
}

void DTelOrch::deleteINTSession(string &int_session_id)
{
    sai_object_id_t int_session_oid;

    if (!intSessionExists(int_session_id))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Failed to get INT session oid %s", int_session_id);
        return;
    }

    int_session_oid = getINTSessionOid(int_session_id);

    status = sai_dtel_api->remove_dtel_int_session(int_session_oid);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("DTEL ERROR: Failed to delete INT session %s", int_session_id);
        return;
    }

    m_dTelINTSessionTable.erase(int_session_id);

    /* Notify all interested parties about INT session being deleted */
    DTelINTSessionUpdate update = {int_session_id, false};
    notify(SUBJECT_TYPE_INT_SESSION_CHANGE, static_cast<void *>(&update));
}

void DTelOrch::doDtelINTSessionTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        sai_object_id_t int_session_oid;

        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find(':');
        string table_id = key.substr(0, found);
        string int_session_id = key.substr(found + 1);

        /* If INT session already exists, delete it first */
        if (intSessionExists(int_session_id))
        {
            deleteINTSession(int_session_id);
        }

        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            vector<sai_attribute_t> int_session_attr;
            sai_attribute_t s_attr;
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == COLLECT_SWITCH_ID)
                {
                    s_attr.id = SAI_DTEL_INT_SESSION_ATTR_COLLECT_SWITCH_ID;
                    s_attr.value.booldata = true;
                    int_session_attr.push_back(s_attr);
                }
                else if (fvField(i) == COLLECT_INGRESS_TIMESTAMP)
                {
                    s_attr.id = SAI_DTEL_INT_SESSION_ATTR_COLLECT_INGRESS_TIMESTAMP;
                    s_attr.value.booldata = true;
                    int_session_attr.push_back(s_attr);
                }
                else if (fvField(i) == COLLECT_EGRESS_TIMESTAMP)
                {
                    s_attr.id = SAI_DTEL_INT_SESSION_ATTR_COLLECT_EGRESS_TIMESTAMP;
                    s_attr.value.booldata = true;
                    int_session_attr.push_back(s_attr);
                }
                else if (fvField(i) == COLLECT_SWITCH_PORTS)
                {
                    s_attr.id = SAI_DTEL_INT_SESSION_ATTR_COLLECT_SWITCH_PORTS;
                    s_attr.value.booldata = true;
                    int_session_attr.push_back(s_attr);
                }
                else if (fvField(i) == COLLECT_QUEUE_INFO)
                {
                    s_attr.id = SAI_DTEL_INT_SESSION_ATTR_COLLECT_QUEUE_INFO;
                    s_attr.value.booldata = true;
                    int_session_attr.push_back(s_attr);
                }
                else if (fvField(i) == MAX_HOP_COUNT)
                {
                    s_attr.id = SAI_DTEL_INT_SESSION_ATTR_MAX_HOP_COUNT;
                    s_attr.value.u16 = to_uint<uint16_t>(fvValue(i));
                    int_session_attr.push_back(s_attr);
                }
            }

            status = sai_dtel_api->create_dtel_int_session(&int_session_oid, 
                int_session_attr.size(), int_session_attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("DTEL ERROR: Failed to set INT session %s", int_session_id);
                return;
            }

            m_dTelINTSessionTable[int_session_id] = new DTelINTSessionEntry(int_session_oid, 0);

            /* Notify all interested parties about INT session being added */
            DTelINTSessionUpdate update = {int_session_id, true};
            notify(SUBJECT_TYPE_INT_SESSION_CHANGE, static_cast<void *>(&update));
        }
        else if (op == DEL_COMMAND)
        {
            if(getINTSessionRefCount(int_session_id) != 0)
            {
                SWSS_LOG_ERROR("DTEL ERROR: References still exist on INT session %s", int_session_id);
                return;
            }
            
            deleteINTSession(int_session_id);
        }

        it = consumer.m_toSync.erase(it);
    }
}

void DTelOrch::disableQueueReport(string &port, string &queue)
{
    sai_object_id_t queue_report_oid;

    if (!isQueueReportEnabled(port, queue))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Failed to get queue report oid for port %s, queue %s", port, queue);
        return;
    }

    queue_report_oid = getQueueReportOid(port, queue);

    status = sai_dtel_api->remove_dtel_queue_report(queue_report_oid);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("DTEL ERROR: Failed to disable queue report for port %s, queue %s", port, queue);
        return;
    }

    removePortQueue(port, queue);
}

void DTelOrch::doDtelQueueReportTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        sai_object_id_t queue_report_oid;

        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t prev = key.find(':');
        string table_id = key.substr(0, prev);
        prev ++;
        size_t next = key.find(':', prev);
        string port = key.substr(prev, next - prev);
        string queue_id = key.substr(next + 1);
        Port port_obj;
        uint32_t q_ind = stoi(queue_id);

        if (!m_portOrch->getPort(port, port_obj))
        {
            SWSS_LOG_ERROR("DTEL ERROR: Failed to process port for INT sink port list. Port %s doesn't exist", fvField(i).c_str());
            return;
        }

        /* If queue report is already enabled in port/queue, disable it first */
        if (isQueueReportEnabled(port, queue_id))
        {
            disableQueueReport(port, queue_id);
        }

        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            vector<sai_attribute_t> queue_report_attr;
            sai_attribute_t qr_attr;
            qr_attr.id = SAI_DTEL_QUEUE_REPORT_ATTR_QUEUE_ID;
            qr_attr.value.oid = port_obj.m_queue_ids[q_ind];
            queue_report_attr.push_back(qr_attr);

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == REPORT_TAIL_DROP)
                {
                    qr_attr.id = SAI_DTEL_QUEUE_REPORT_ATTR_TAIL_DROP;
                    qr_attr.value.booldata = true;
                    queue_report_attr.push_back(qr_attr);
                }
                else if (fvField(i) == QUEUE_DEPTH_THRESHOLD)
                {
                    qr_attr.id = SAI_DTEL_QUEUE_REPORT_ATTR_DEPTH_THRESHOLD;
                    qr_attr.value.u32 = to_uint<uint32_t>(fvValue(i));
                    queue_report_attr.push_back(qr_attr);
                }
                else if (fvField(i) == QUEUE_LATENCY_THRESHOLD)
                {
                    qr_attr.id = SAI_DTEL_QUEUE_REPORT_ATTR_LATENCY_THRESHOLD;
                    qr_attr.value.u32 = to_uint<uint32_t>(fvValue(i));
                    queue_report_attr.push_back(qr_attr);
                }
                else if (fvField(i) == THRESHOLD_BREACH_QUOTA)
                {
                    qr_attr.id = SAI_DTEL_QUEUE_REPORT_ATTR_BREACH_QUOTA;
                    qr_attr.value.u32 = to_uint<uint32_t>(fvValue(i));
                    queue_report_attr.push_back(qr_attr);
                }
            }

            status = sai_dtel_api->create_dtel_queue_report(&queue_report_oid, 
                queue_report_attr.size(), queue_report_attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("DTEL ERROR: Failed to enable queue report on port %s, queue %s", port, queue_id);
                return;
            }

            addPortQueue(port, queue_id, queue_report_oid);
        }
        else if (op == DEL_COMMAND)
        {   
            disableQueueReport(port, queue_id);
        }

        it = consumer.m_toSync.erase(it);
    }
}

void DTelOrch::unConfigureEvent(string &event)
{
    sai_object_id_t event_oid;

    if (!isEventConfigured(event))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Event is not configured %s", event);
        return;
    }

    event_oid = getEventOid(event);

    status = sai_dtel_api->remove_dtel_event(event_oid);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("DTEL ERROR: Failed to remove event %s", event);
        return;
    }

    removeEvent(event);
}

void DTelOrch::doDtelEventTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        sai_object_id_t event_oid;
        string report_session_id;

        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find(':');
        string table_id = key.substr(0, found);
        string event = key.substr(found + 1);

        /* If event is already configured, un-configure it first */
        if (isEventConfigured(event))
        {
            unConfigureEvent(event);
        }

        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            vector<sai_attribute_t> event_attr;
            sai_attribute_t e_attr;
            e_attr.id = SAI_DTEL_EVENT_ATTR_TYPE;
            e_attr.value.s32 = dTelEventLookup[fvField(i)];
            event_attr.push_back(e_attr);

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == EVENT_REPORT_SESSION)
                {
                    if (!reportSessionExists(fvValue(i)))
                    {
                        SWSS_LOG_ERROR("DTEL ERROR: Report session %s used by event %s does not exist", fvValue(i), event);
                        return;
                    }

                    e_attr.id = SAI_DTEL_EVENT_ATTR_REPORT_SESSION;
                    e_attr.value.oid = getReportSessionOid(fvValue(i));
                    event_attr.push_back(e_attr);
                    report_session_id = fvValue(i)
                }
                else if (fvField(i) == EVENT_DSCP_VALUE)
                {
                    e_attr.id = SAI_DTEL_EVENT_ATTR_DSCP_VALUE;
                    e_attr.value.u8 = to_uint<uint8_t>(fvValue(i));
                    event_attr.push_back(e_attr);
                }
            }

            status = sai_dtel_api->create_dtel_event(&event_oid, 
                event_attr.size(), event_attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("DTEL ERROR: Failed to create event %s", event);
                return;
            }

            addEvent(event, event_oid, report_session_id);
        }
        else if (op == DEL_COMMAND)
        {   
            unConfigureEvent(event);
        }

        it = consumer.m_toSync.erase(it);
    }
}

void DTelOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.m_consumer->getTableName();

    if (table_name == APP_DTEL_TABLE_NAME)
    {
        doDtelTableTask(consumer);
    }
    else if (table_name == APP_DTEL_REPORT_SESSION_TABLE_NAME)
    {
        doDtelReportSessionTableTask(consumer);
    }
    else if (table_name == APP_DTEL_INT_SESSION_TABLE_NAME)
    {
        doDtelINTSessionTableTask(consumer);
    }
    else if (table_name == APP_DTEL_QUEUE_REPORT_TABLE_NAME)
    {
        doDtelQueueReportTableTask(consumer);
    }
    else if (table_name == APP_DTEL_EVENT_ATTR_TABLE_NAME)
    {
        doDtelEventTableTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("DTEL ERROR: Invalid table %s", table_name.c_str());
    }
}