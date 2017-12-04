#include "dtelorch.h"

#include "logger.h"
#include "schema.h"
#include "converter.h"
#include "ipprefix.h"
#include "swssnet.h"

using namespace std;
using namespace swss;

extern sai_switch_api_t* sai_switch_api;
extern sai_dtel_api_t* sai_dtel_api;
extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t gSwitchId;

dtelEventLookup_t dTelEventLookup =
{
    { EVENT_TYPE_FLOW_STATE,                         SAI_DTEL_EVENT_TYPE_FLOW_STATE },
    { EVENT_TYPE_FLOW_REPORT_ALL_PACKETS,            SAI_DTEL_EVENT_TYPE_FLOW_REPORT_ALL_PACKETS },
    { EVENT_TYPE_FLOW_TCPFLAG,                       SAI_DTEL_EVENT_TYPE_FLOW_TCPFLAG },
    { EVENT_TYPE_QUEUE_REPORT_THRESHOLD_BREACH,      SAI_DTEL_EVENT_TYPE_QUEUE_REPORT_THRESHOLD_BREACH },
    { EVENT_TYPE_QUEUE_REPORT_TAIL_DROP,             SAI_DTEL_EVENT_TYPE_QUEUE_REPORT_TAIL_DROP },
    { EVENT_TYPE_DROP_REPORT,                        SAI_DTEL_EVENT_TYPE_DROP_REPORT }
};

DTelOrch::DTelOrch(DBConnector *db, vector<string> tableNames, PortsOrch *portOrch) :
        Orch(db, tableNames),
        m_portOrch(portOrch)
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

    auto port_entry_iter = m_dTelPortTable.find(port);

    if (port_entry_iter == m_dTelPortTable.end() || 
        (port_entry_iter->second.queueTable).find(queue) == (port_entry_iter->second.queueTable).end())
    {
        return false;
    }

    return true;
}

bool DTelOrch::getQueueReportOid(const string& port, const string& queue, sai_object_id_t& oid)
{
    SWSS_LOG_ENTER();

    if (!isQueueReportEnabled(port, queue))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Queue report not enabled on port %s, queue %s", port.c_str(), queue.c_str());
        return false;
    }

    oid = m_dTelPortTable[port].queueTable[queue];

    return true;
}

void DTelOrch::removePortQueue(const string& port, const string& queue)
{
    if (!isQueueReportEnabled(port, queue))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Queue report not enabled on port %s, queue %s", port.c_str(), queue.c_str());
        return;
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
        SWSS_LOG_ERROR("DTEL ERROR: Queue report already enabled on port %s, queue %s", port.c_str(), queue.c_str());
        return;
    }

    m_dTelPortTable[port] = DTelPortEntry();
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

bool DTelOrch::getEventOid(const string& event, sai_object_id_t& oid)
{
    SWSS_LOG_ENTER();

    if (!isEventConfigured(event))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Event not configured %s", event.c_str());
        return false;
    }

    oid = m_dtelEventTable[event].eventOid;

    return true;
}

void DTelOrch::addEvent(const string& event, const sai_object_id_t& event_oid, const string& report_session_id)
{
    if (isEventConfigured(event))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Event is already configured %s", event.c_str());
        return;
    }

    DTelEventEntry event_entry;
    event_entry.eventOid = event_oid;
    event_entry.reportSessionId = report_session_id;
    m_dtelEventTable[event] = event_entry;

    increaseReportSessionRefCount(report_session_id);
}

void DTelOrch::removeEvent(const string& event)
{
    if (!isEventConfigured(event))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Event not configured %s", event.c_str());
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
    sai_status_t status;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string table_attr = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            if (table_attr == INT_ENDPOINT)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_INT_ENDPOINT_ENABLE;
                attr.value.booldata = true;
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to enable INT endpoint mode");
		    goto dtel_table_continue;
                }
            }
            else if (table_attr == INT_TRANSIT)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_INT_TRANSIT_ENABLE;
                attr.value.booldata = true;
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to enable INT transit mode");
		    goto dtel_table_continue;
                }
            }
            else if (table_attr == POSTCARD)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_POSTCARD_ENABLE;
                attr.value.booldata = true;
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to enable DTel postcard");
		    goto dtel_table_continue;
                }
            }
            else if (table_attr == DROP_REPORT)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_DROP_REPORT_ENABLE;
                attr.value.booldata = true;
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to enable drop report");
		    goto dtel_table_continue;
                }
            }
            else if (table_attr == QUEUE_REPORT)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_QUEUE_REPORT_ENABLE;
                attr.value.booldata = true;
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to enable queue report");
		    goto dtel_table_continue;
                }
            }
            else if (table_attr == SWITCH_ID)
            {
                FieldValueTuple e = kfvFieldsValues(t)[0];

                attr.id = SAI_SWITCH_ATTR_DTEL_SWITCH_ID;
                attr.value.u32 = to_uint<uint32_t>(fvValue(e));
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);  
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to set switch id");
		    goto dtel_table_continue;
                } 
            }
            else if (table_attr == FLOW_STATE_CLEAR_CYCLE)
            {
                FieldValueTuple e = kfvFieldsValues(t)[0];

                attr.id = SAI_SWITCH_ATTR_DTEL_FLOW_STATE_CLEAR_CYCLE;
                attr.value.u16 = to_uint<uint16_t>(fvValue(e));
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to set Dtel flow state clear cycle");
		    goto dtel_table_continue;
                }   
            }
            else if (table_attr == LATENCY_SENSITIVITY)
            {
                FieldValueTuple e = kfvFieldsValues(t)[0];

                attr.id = SAI_SWITCH_ATTR_DTEL_LATENCY_SENSITIVITY;
                attr.value.u16 = to_uint<uint16_t>(fvValue(e));
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to set Dtel latency sensitivity");
		    goto dtel_table_continue;
                }
            }
            else if (table_attr == SINK_PORT_LIST)
            {
                vector<sai_object_id_t> port_list;
                Port port;
                attr.id = SAI_SWITCH_ATTR_DTEL_SINK_PORT_LIST;

                for (auto i : kfvFieldsValues(t))
                {
                    if (!m_portOrch->getPort(fvField(i), port))
                    {
                        SWSS_LOG_ERROR("DTEL ERROR: Failed to process port for INT sink port list. Port %s doesn't exist", fvField(i).c_str());
		        goto dtel_table_continue;
                    }

                    port_list.push_back(port.m_port_id);
                }

                if (port_list.size() == 0)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Invalid INT sink port list");
		    goto dtel_table_continue;
                }

                attr.value.objlist.count = (uint32_t)port_list.size();
                attr.value.objlist.list = port_list.data();
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to set INT sink port list");
		    goto dtel_table_continue;
                }
            }
            else if (table_attr == INT_L4_DSCP)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_INT_L4_DSCP;

                if ((uint32_t)(kfvFieldsValues(t).size()) != 2)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: INT L4 DSCP attribute requires value and mask");
		    goto dtel_table_continue;
                }

                for (auto i : kfvFieldsValues(t))
                {
                    if (fvField(i) == INT_L4_DSCP_VALUE)
                    {
                        attr.value.ternaryfield.value.u8 = to_uint<uint8_t>(fvValue(i));
                    }
                    else if (fvField(i) == INT_L4_DSCP_MASK)
                    {
                        attr.value.ternaryfield.mask.u8 = to_uint<uint8_t>(fvValue(i));
                    }
                }

                if (attr.value.ternaryfield.value.u8 == 0 ||
                    attr.value.ternaryfield.mask.u8 == 0)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Invalid INT L4 DSCP value/mask");
		    goto dtel_table_continue;
                }

                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to set INT L4 DSCP value/mask");
		    goto dtel_table_continue;
                }
            }

        }
        else if (op == DEL_COMMAND)
        {
            if (table_attr == INT_ENDPOINT)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_INT_ENDPOINT_ENABLE;
                attr.value.booldata = false;
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to disable INT endpoint mode");
		    goto dtel_table_continue;
                }
            }
            else if (table_attr == INT_TRANSIT)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_INT_TRANSIT_ENABLE;
                attr.value.booldata = false;
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to disable INT transit mode");
		    goto dtel_table_continue;
                }
            }
            else if (table_attr == POSTCARD)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_POSTCARD_ENABLE;
                attr.value.booldata = false;
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to disable postcard mode");
		    goto dtel_table_continue;
                }
            }
            else if (table_attr == DROP_REPORT)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_DROP_REPORT_ENABLE;
                attr.value.booldata = false;
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to disable drop report");
		    goto dtel_table_continue;
                }
            }
            else if (table_attr == QUEUE_REPORT)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_QUEUE_REPORT_ENABLE;
                attr.value.booldata = false;
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to disable queue report");
		    goto dtel_table_continue;
                }
            }
            else if (table_attr == SWITCH_ID)
            {
                auto i = kfvFieldsValues(t);

                attr.id = SAI_SWITCH_ATTR_DTEL_SWITCH_ID;
                attr.value.u32 = 0;
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to reset switch id");
		    goto dtel_table_continue;
                }  
            }
            else if (table_attr == FLOW_STATE_CLEAR_CYCLE)
            {
                auto i = kfvFieldsValues(t);

                attr.id = SAI_SWITCH_ATTR_DTEL_FLOW_STATE_CLEAR_CYCLE;
                attr.value.u16 = 0;
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to reset flow state clear cycle");
		    goto dtel_table_continue;
                } 
            }
            else if (table_attr == LATENCY_SENSITIVITY)
            {
                auto i = kfvFieldsValues(t);

                attr.id = SAI_SWITCH_ATTR_DTEL_LATENCY_SENSITIVITY;
                attr.value.u16 = 0;
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to reset latency sensitivity");
		    goto dtel_table_continue;
                }
            }
            else if (table_attr == SINK_PORT_LIST)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_SINK_PORT_LIST;

                attr.value.objlist.count = 0;
                attr.value.objlist.list = {};
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to reset sink port list");
		    goto dtel_table_continue;
                }
            }
            else if (table_attr == INT_L4_DSCP)
            {
                attr.id = SAI_SWITCH_ATTR_DTEL_INT_L4_DSCP;
                attr.value.ternaryfield.value.u8 = 0;
                attr.value.ternaryfield.mask.u8 = 0;
                status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("DTEL ERROR: Failed to reset INT L4 DSCP value/mask");
		    goto dtel_table_continue;
                }
            }
        }

dtel_table_continue:
        it = consumer.m_toSync.erase(it);
    }
}

bool DTelOrch::deleteReportSession(string &report_session_id)
{
    sai_object_id_t report_session_oid;
    sai_status_t status;

    if (!reportSessionExists(report_session_id))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Report session %s does not exist", report_session_id.c_str());
        return false;
    }

    if (!getReportSessionOid(report_session_id, report_session_oid))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Could not get report session oid for session %s", report_session_id.c_str());
        return false;
    }

    status = sai_dtel_api->remove_dtel_report_session(report_session_oid);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("DTEL ERROR: Failed to delete report session %s", report_session_id.c_str());
        return false;
    }

    m_dTelReportSessionTable.erase(report_session_id);
    return true;
}

void DTelOrch::doDtelReportSessionTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    sai_status_t status;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        sai_object_id_t report_session_oid;

        KeyOpFieldsValuesTuple t = it->second;
        string report_session_id = kfvKey(t);

        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            vector<sai_attribute_t> report_session_attr;
            vector<sai_ip_address_t> dst_ip_list;
            sai_ip_address_t dst_ip;
            sai_attribute_t rs_attr;

	    /* If report session already exists, delete it first */
	    if (reportSessionExists(report_session_id))
	    {
	        if (!deleteReportSession(report_session_id))
                {
                    goto report_session_table_continue;
                }
	    }

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == SRC_IP)
                {
                    IpAddress ip(fvValue(i));
                    rs_attr.id = SAI_DTEL_REPORT_SESSION_ATTR_SRC_IP;
		    copy(rs_attr.value.ipaddr, ip);
                    report_session_attr.push_back(rs_attr);
                }
                else if (fvField(i) == DST_IP_LIST)
                {
                    dst_ip.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
                    size_t prev = 0;
                    size_t next = fvValue(i).find(';');
                    while (next != std::string::npos)
                    {
                        IpAddress ip(fvValue(i).substr(prev, next - prev));
			copy(dst_ip, ip);
                        dst_ip_list.push_back(dst_ip);
                        prev = next + 1;
                        next = fvValue(i).find(';', prev);
                    }

                    /* Add the last IP */
                    IpAddress ip(fvValue(i).substr(prev));
	            copy(dst_ip, ip);
                    dst_ip_list.push_back(dst_ip);

                    rs_attr.id = SAI_DTEL_REPORT_SESSION_ATTR_DST_IP_LIST;
                    rs_attr.value.ipaddrlist.count = (uint32_t)dst_ip_list.size();
                    rs_attr.value.ipaddrlist.list = dst_ip_list.data();
                    report_session_attr.push_back(rs_attr);
                }
                else if (fvField(i) == VRF)
                {
                    rs_attr.id = SAI_DTEL_REPORT_SESSION_ATTR_VIRTUAL_ROUTER_ID;
                    /* TODO: find a way to convert vrf to oid */
                    rs_attr.value.oid = gVirtualRouterId;
                    report_session_attr.push_back(rs_attr);
                }
                else if (fvField(i) == TRUNCATE_SIZE)
                {
                    rs_attr.id = SAI_DTEL_REPORT_SESSION_ATTR_TRUNCATE_SIZE;
                    rs_attr.value.u16 = to_uint<uint16_t>(fvValue(i));
                    report_session_attr.push_back(rs_attr);
                }
                else if (fvField(i) == UDP_DEST_PORT)
                {
                    rs_attr.id = SAI_DTEL_REPORT_SESSION_ATTR_UDP_DST_PORT;
                    rs_attr.value.u16 = to_uint<uint16_t>(fvValue(i));
                    report_session_attr.push_back(rs_attr);
                }
            }

            status = sai_dtel_api->create_dtel_report_session(&report_session_oid, 
                gSwitchId, (uint32_t)report_session_attr.size(), report_session_attr.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("DTEL ERROR: Failed to set INT EP report session %s", report_session_id.c_str());
                goto report_session_table_continue;
            }

            DTelReportSessionEntry rs_entry = { };
            rs_entry.reportSessionOid = report_session_oid;
            m_dTelReportSessionTable[report_session_id] = rs_entry;
        }
        else if (op == DEL_COMMAND)
        {
            if(getReportSessionRefCount(report_session_id) != 0)
            {
                SWSS_LOG_ERROR("DTEL ERROR: References still exist on report session %s", report_session_id.c_str());
                goto report_session_table_continue;
            }

            if (!deleteReportSession(report_session_id))
            {
                goto report_session_table_continue;
            } 
        }

report_session_table_continue:
        it = consumer.m_toSync.erase(it);
    }
}

bool DTelOrch::deleteINTSession(string &int_session_id)
{
    sai_object_id_t int_session_oid;
    sai_status_t status;

    if (!intSessionExists(int_session_id))
    {
        SWSS_LOG_ERROR("DTEL ERROR: INT session %s does not exist", int_session_id.c_str());
        return false;
    }

    if (!getINTSessionOid(int_session_id, int_session_oid))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Failed to get INT session oid %s", int_session_id.c_str());
        return false;
    }

    status = sai_dtel_api->remove_dtel_int_session(int_session_oid);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("DTEL ERROR: Failed to delete INT session %s", int_session_id.c_str());
        return false;
    }

    m_dTelINTSessionTable.erase(int_session_id);

    /* Notify all interested parties about INT session being deleted */
    DTelINTSessionUpdate update = {int_session_id, false};
    notify(SUBJECT_TYPE_INT_SESSION_CHANGE, static_cast<void *>(&update));
    return true;
}

void DTelOrch::doDtelINTSessionTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    sai_status_t status;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        sai_object_id_t int_session_oid;

        KeyOpFieldsValuesTuple t = it->second;
        string int_session_id = kfvKey(t);

        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            vector<sai_attribute_t> int_session_attr;
            sai_attribute_t s_attr;

            /* If INT session already exists, delete it first */
            if (intSessionExists(int_session_id))
            {
                if (!deleteINTSession(int_session_id))
                {
                    goto int_session_table_continue;
                }
            }

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
                gSwitchId, (uint32_t)int_session_attr.size(), int_session_attr.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("DTEL ERROR: Failed to set INT session %s", int_session_id.c_str());
                goto int_session_table_continue;
            }

            DTelINTSessionEntry is_entry;
            is_entry.intSessionOid = int_session_oid;
            m_dTelINTSessionTable[int_session_id] = is_entry;

            /* Notify all interested parties about INT session being added */
            DTelINTSessionUpdate update = {int_session_id, true};
            notify(SUBJECT_TYPE_INT_SESSION_CHANGE, static_cast<void *>(&update));
        }
        else if (op == DEL_COMMAND)
        {
            if(getINTSessionRefCount(int_session_id) != 0)
            {
                SWSS_LOG_ERROR("DTEL ERROR: References still exist on INT session %s", int_session_id.c_str());
                goto int_session_table_continue;
            }
            
            if (!deleteINTSession(int_session_id))
            {
                goto int_session_table_continue;
            }
        }

int_session_table_continue:
        it = consumer.m_toSync.erase(it);
    }
}

bool DTelOrch::disableQueueReport(string &port, string &queue)
{
    sai_object_id_t queue_report_oid;
    sai_status_t status;

    if (!isQueueReportEnabled(port, queue))
    {
        SWSS_LOG_ERROR("DTEL ERROR: queue report not enabled for port %s, queue %s", port.c_str(), queue.c_str());
        return false;
    }

    if (!getQueueReportOid(port, queue, queue_report_oid))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Failed to get queue report oid for port %s, queue %s", port.c_str(), queue.c_str());
        return false;
    }

    status = sai_dtel_api->remove_dtel_queue_report(queue_report_oid);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("DTEL ERROR: Failed to disable queue report for port %s, queue %s", port.c_str(), queue.c_str());
        return false;
    }

    removePortQueue(port, queue);
    return true;
}

void DTelOrch::doDtelQueueReportTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    sai_status_t status;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        sai_object_id_t queue_report_oid;

        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        size_t found = key.find(':');
        string port = key.substr(0, found);
        string queue_id = key.substr(found + 1);
        Port port_obj;
        uint32_t q_ind = stoi(queue_id);
        string op = kfvOp(t);

        if (!m_portOrch->getPort(port, port_obj))
        {
            SWSS_LOG_ERROR("DTEL ERROR: Failed to process port for INT sink port list. Port %s doesn't exist", port.c_str());
            goto queue_report_table_continue;
        }

        if (op == SET_COMMAND)
        {
            vector<sai_attribute_t> queue_report_attr;
            sai_attribute_t qr_attr;

            /* If queue report is already enabled in port/queue, disable it first */
            if (isQueueReportEnabled(port, queue_id))
            {
                if (!disableQueueReport(port, queue_id))
                {
                    goto queue_report_table_continue;
                }
            }

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
                gSwitchId, (uint32_t)queue_report_attr.size(), queue_report_attr.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("DTEL ERROR: Failed to enable queue report on port %s, queue %s", port.c_str(), queue_id.c_str());
                goto queue_report_table_continue;
            }

            addPortQueue(port, queue_id, queue_report_oid);
        }
        else if (op == DEL_COMMAND)
        {   
            if (!disableQueueReport(port, queue_id))
            {
                goto queue_report_table_continue;
            }
        }

queue_report_table_continue:
        it = consumer.m_toSync.erase(it);
    }
}

bool DTelOrch::unConfigureEvent(string &event)
{
    sai_object_id_t event_oid;
    sai_status_t status;

    if (!isEventConfigured(event))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Event is not configured %s", event.c_str());
        return false;
    }

    if (!getEventOid(event, event_oid))
    {
        SWSS_LOG_ERROR("DTEL ERROR: Could not get event oid for event %s", event.c_str());
        return false;
    }

    status = sai_dtel_api->remove_dtel_event(event_oid);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("DTEL ERROR: Failed to remove event %s", event.c_str());
        return false;
    }

    removeEvent(event);
    return true;
}

void DTelOrch::doDtelEventTableTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    sai_status_t status;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        sai_object_id_t event_oid;
        string report_session_id;

        KeyOpFieldsValuesTuple t = it->second;
        string event = kfvKey(t);

        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            vector<sai_attribute_t> event_attr;
            sai_attribute_t e_attr;
            e_attr.id = SAI_DTEL_EVENT_ATTR_TYPE;
            e_attr.value.s32 = dTelEventLookup[event];
            event_attr.push_back(e_attr);

            /* If event is already configured, un-configure it first */
            if (isEventConfigured(event))
            {
                if (!unConfigureEvent(event))
                {
		    goto event_table_continue;
                }
            }

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == EVENT_REPORT_SESSION)
                {
                    if (!reportSessionExists(fvValue(i)))
                    {
                        SWSS_LOG_ERROR("DTEL ERROR: Report session %s used by event %s does not exist", fvValue(i).c_str(), event.c_str());
		        goto event_table_continue;
                    }

                    e_attr.id = SAI_DTEL_EVENT_ATTR_REPORT_SESSION;
                    if (!getReportSessionOid(fvValue(i), e_attr.value.oid))
                    {
                        SWSS_LOG_ERROR("DTEL ERROR: Could not get report session oid for event %s, session %s", fvValue(i).c_str(), event.c_str());
		        goto event_table_continue;
                    }
                    event_attr.push_back(e_attr);
                    report_session_id = fvValue(i);
                }
                else if (fvField(i) == EVENT_DSCP_VALUE)
                {
                    e_attr.id = SAI_DTEL_EVENT_ATTR_DSCP_VALUE;
                    e_attr.value.u8 = to_uint<uint8_t>(fvValue(i));
                    event_attr.push_back(e_attr);
                }
            }

            status = sai_dtel_api->create_dtel_event(&event_oid, 
                gSwitchId, (uint32_t)event_attr.size(), event_attr.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("DTEL ERROR: Failed to create event %s", event.c_str());
		goto event_table_continue;
            }

            addEvent(event, event_oid, report_session_id);
        }
        else if (op == DEL_COMMAND)
        {   
            if (!unConfigureEvent(event))
            {
                goto event_table_continue;
            }
        }

event_table_continue:
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
    else if (table_name == APP_DTEL_EVENT_TABLE_NAME)
    {
        doDtelEventTableTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("DTEL ERROR: Invalid table %s", table_name.c_str());
    }
}
