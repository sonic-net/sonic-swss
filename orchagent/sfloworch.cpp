#include "sai.h"
#include "sfloworch.h"
#include "tokenize.h"

using namespace std;
using namespace swss;

extern sai_hostif_api_t*       sai_hostif_api;
extern sai_policer_api_t*      sai_policer_api;
extern sai_samplepacket_api_t* sai_samplepacket_api;
extern sai_switch_api_t*       sai_switch_api;
extern sai_tam_api_t*          sai_tam_api;
extern sai_port_api_t*         sai_port_api;
extern sai_object_id_t         gSwitchId;
extern PortsOrch*              gPortsOrch;

// TODO: Add the value to copp_cfg.j2
#define SFLOW_DROP_MONITOR_CPU_QUEUE 47

bool SflowDropMonitor::enableDropMonitor(int32_t limit_rate)
{
    // Check drop monitor limit rate
    if (isEnabled())
    {
        if (getLimitRate() == limit_rate)
        {
            return true;
        }

        // Reenable drop monitor when rate limit is changed
        if (!disableDropMonitor())
        {
            SWSS_LOG_ERROR("Failed to disable drop monitor for reconfiguration.");
            return false;
        }

        return enableDropMonitor(limit_rate);
    }

    if (!initializeDropMonitor(limit_rate))
    {
        SWSS_LOG_ERROR("Failed to initialize drop monitor.");
        cleanupDropMonitor();
        return false;
    }

    // Bind the TAM object to switch
    {
        sai_status_t    status;
        sai_attribute_t attr;
        sai_object_id_t tam = m_tam;

        attr.id = SAI_SWITCH_ATTR_TAM_OBJECT_ID;
        attr.value.objlist.count = 1;
        attr.value.objlist.list = &tam;

        status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to enable drop monitor when binding the TAM object to switch, rv:%d", status);
            cleanupDropMonitor();
            return false;
        }
    }

    m_limitRate = limit_rate;
    m_enable = true;
    return true;
}

bool SflowDropMonitor::disableDropMonitor()
{
    sai_attribute_t attr;
    sai_status_t    status;
    sai_object_id_t null_oid = SAI_NULL_OBJECT_ID;

    if (!isEnabled())
    {
        return true;
    }

    attr.id = SAI_SWITCH_ATTR_TAM_OBJECT_ID;
    attr.value.objlist.count = 0;
    attr.value.objlist.list = &null_oid;
    status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to disable drop monitor when unbinding the TAM object from switch, rv:%d", status);
        return false;
    }

    cleanupDropMonitor();

    m_limitRate = 0;
    m_enable = false;
    return true;
}

bool SflowDropMonitor::createTamReport()
{
    sai_status_t            status;
    sai_attribute_t         attr;
    vector<sai_attribute_t> attributes;
    sai_object_id_t         tam_report = SAI_NULL_OBJECT_ID;

    if (m_tamReport != SAI_NULL_OBJECT_ID)
    {
        return false;
    }

    attr.id = SAI_TAM_REPORT_ATTR_TYPE;
    attr.value.s32 = SAI_TAM_REPORT_TYPE_GENETLINK;
    attributes.push_back(attr);

    status = sai_tam_api->create_tam_report(&tam_report, gSwitchId, (uint32_t)attributes.size(), attributes.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create TAM report, rv:%d", status);
        return false;
    }

    m_tamReport = tam_report;
    return true;
}

bool SflowDropMonitor::removeTamReport()
{
    sai_status_t status;

    if (m_tamReport == SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    status = sai_tam_api->remove_tam_report(m_tamReport);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove TAM report, rv:%d", status);
        return false;
    }

    m_tamReport = SAI_NULL_OBJECT_ID;
    return true;
}

bool SflowDropMonitor::createTamEventAction()
{
    sai_status_t            status;
    sai_attribute_t         attr;
    vector<sai_attribute_t> attributes;
    sai_object_id_t         tam_event_action = SAI_NULL_OBJECT_ID;

    if (m_tamEventAction != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("The TAM event action has already been created.");
        return false;
    }

    if (m_tamReport == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("The TAM report must be created before the TAM event action.");
        return false;
    }

    attr.id = SAI_TAM_EVENT_ACTION_ATTR_REPORT_TYPE;
    attr.value.oid = m_tamReport;
    attributes.push_back(attr);
    status = sai_tam_api->create_tam_event_action(&tam_event_action, gSwitchId, (uint32_t)attributes.size(), attributes.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create TAM event action, rv:%d", status);
        return false;
    }

    m_tamEventAction = tam_event_action;
    return true;
}

bool SflowDropMonitor::removeTamEventAction()
{
    sai_status_t status;

    if (m_tamEventAction == SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    status = sai_tam_api->remove_tam_event_action(m_tamEventAction);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove TAM event action, rv:%d", status);
        return false;
    }

    m_tamEventAction = SAI_NULL_OBJECT_ID;
    return true;
}

bool SflowDropMonitor::createTamTransport()
{
    sai_status_t            status;
    sai_attribute_t         attr;
    vector<sai_attribute_t> attributes;
    sai_object_id_t         tam_transport = SAI_NULL_OBJECT_ID;

    if (m_tamTransport != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("The TAM transport has already been created.");
        return false;
    }

    attr.id = SAI_TAM_TRANSPORT_ATTR_TRANSPORT_TYPE;
    attr.value.s32 = SAI_TAM_TRANSPORT_TYPE_NONE;
    attributes.push_back(attr);

    status = sai_tam_api->create_tam_transport(&tam_transport, gSwitchId, (uint32_t)attributes.size(), attributes.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create TAM transport, rv:%d", status);
        return false;
    }

    m_tamTransport = tam_transport;
    return true;
}

bool SflowDropMonitor::removeTamTransport()
{
    sai_status_t status;

    if (m_tamTransport == SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    status = sai_tam_api->remove_tam_transport(m_tamTransport);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove TAM transport, rv:%d", status);
        return false;
    }

    m_tamTransport = SAI_NULL_OBJECT_ID;
    return true;
}

bool SflowDropMonitor::createTamEvent()
{
    sai_status_t            status;
    sai_attribute_t         attr;
    vector<sai_attribute_t> attributes;
    sai_object_id_t         tam_event = SAI_NULL_OBJECT_ID;
    sai_object_id_t         tam_collector = m_tamCollector;
    sai_object_id_t         tam_event_action = m_tamEventAction;

    if (m_tamEvent != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("The TAM event has already been created.");
        return false;
    }

    if (tam_collector == SAI_NULL_OBJECT_ID || tam_event_action == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("The TAM collector and TAM event action must be created before the TAM event.");
        return false;
    }

    attr.id = SAI_TAM_EVENT_ATTR_TYPE;
    attr.value.s32 = SAI_TAM_EVENT_TYPE_PACKET_DROP;
    attributes.push_back(attr);
    attr.id = SAI_TAM_EVENT_ATTR_COLLECTOR_LIST;
    attr.value.objlist.count = 1;
    attr.value.objlist.list = &tam_collector;
    attributes.push_back(attr);
    attr.id = SAI_TAM_EVENT_ATTR_ACTION_LIST;
    attr.value.objlist.count = 1;
    attr.value.objlist.list = &tam_event_action;
    attributes.push_back(attr);

    status = sai_tam_api->create_tam_event(&tam_event, gSwitchId, (uint32_t)attributes.size(), attributes.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create TAM event, rv:%d", status);
        return false;
    }

    m_tamEvent = tam_event;
    return true;
}

bool SflowDropMonitor::removeTamEvent()
{
    sai_status_t status;

    if (m_tamEvent == SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    status = sai_tam_api->remove_tam_event(m_tamEvent);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove TAM event, rv:%d", status);
        return false;
    }

    m_tamEvent = SAI_NULL_OBJECT_ID;
    return true;
}

bool SflowDropMonitor::createTamCollector()
{
    sai_status_t            status;
    sai_attribute_t         attr;
    vector<sai_attribute_t> attributes;
    sai_object_id_t         tam_collector = SAI_NULL_OBJECT_ID;
    sai_object_id_t         tam_transport = m_tamTransport;
    sai_object_id_t         hostif_user_defined_trap = m_hostifUserDefinedTrap;

    if (m_tamCollector != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("The TAM collector has already been created.");
        return false;
    }

    if (tam_transport == SAI_NULL_OBJECT_ID || hostif_user_defined_trap == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("The TAM transport and HOSTIF user defined trap must be created before the TAM collector.");
        return false;
    }

    // The SAI_TAM_COLLECTOR_ATTR_LOCALHOST is set as true, however, the
    // SAI_TAM_COLLECTOR_ATTR_SRC_IP and SAI_TAM_COLLECTOR_ATTR_DST_IP
    // values are still required on creation by SAI definition.
    attr.id = SAI_TAM_COLLECTOR_ATTR_SRC_IP;
    attr.value.ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    attr.value.ipaddr.addr.ip4 = 0x7f000001; // 127.0.0.1
    attributes.push_back(attr);
    attr.id = SAI_TAM_COLLECTOR_ATTR_DST_IP;
    attr.value.ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    attr.value.ipaddr.addr.ip4 = 0x7f000001; // 127.0.0.1
    attributes.push_back(attr);
    attr.id = SAI_TAM_COLLECTOR_ATTR_DSCP_VALUE;
    attr.value.u8 = 0;
    attributes.push_back(attr);
    attr.id = SAI_TAM_COLLECTOR_ATTR_TRANSPORT;
    attr.value.oid = tam_transport;
    attributes.push_back(attr);
    attr.id = SAI_TAM_COLLECTOR_ATTR_LOCALHOST;
    attr.value.booldata = true;
    attributes.push_back(attr);
    attr.id = SAI_TAM_COLLECTOR_ATTR_HOSTIF_TRAP;
    attr.value.oid = hostif_user_defined_trap;
    attributes.push_back(attr);

    status = sai_tam_api->create_tam_collector(&tam_collector, gSwitchId, (uint32_t)attributes.size(), attributes.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create TAM collector, rv:%d", status);
        return false;
    }

    m_tamCollector = tam_collector;
    return true;
}

bool SflowDropMonitor::removeTamCollector()
{
    sai_status_t status;

    if (m_tamCollector == SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    status = sai_tam_api->remove_tam_collector(m_tamCollector);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove TAM collector, rv:%d", status);
        return false;
    }

    m_tamCollector = SAI_NULL_OBJECT_ID;
    return true;
}

bool SflowDropMonitor::createTam()
{
    sai_status_t            status;
    sai_attribute_t         attr;
    vector<sai_attribute_t> attributes;
    sai_object_id_t         tam = SAI_NULL_OBJECT_ID;
    sai_object_id_t         tam_event = m_tamEvent;
    int32_t                 bind_point = SAI_TAM_BIND_POINT_TYPE_SWITCH;

    if (m_tam != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("The TAM has already been created.");
        return false;
    }

    if (tam_event == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("The TAM event must be created before the TAM.");
        return false;
    }

    attr.id = SAI_TAM_ATTR_EVENT_OBJECTS_LIST;
    attr.value.objlist.count = 1;
    attr.value.objlist.list = &tam_event;
    attributes.push_back(attr);
    attr.id = SAI_TAM_ATTR_TAM_BIND_POINT_TYPE_LIST;
    attr.value.s32list.count = 1;
    attr.value.s32list.list = &bind_point;
    attributes.push_back(attr);

    status = sai_tam_api->create_tam(&tam, gSwitchId, (uint32_t)attributes.size(), attributes.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create TAM, rv:%d", status);
        return false;
    }

    m_tam = tam;
    return true;
}

bool SflowDropMonitor::removeTam()
{
    sai_status_t status;

    if (m_tam == SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    status = sai_tam_api->remove_tam(m_tam);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove TAM, rv:%d", status);
        return false;
    }

    m_tam = SAI_NULL_OBJECT_ID;
    return true;
}

bool SflowDropMonitor::createPolicer(int32_t limit_rate)
{
    sai_status_t            status;
    sai_attribute_t         attr;
    vector<sai_attribute_t> attributes;
    sai_object_id_t         policer = SAI_NULL_OBJECT_ID;

    if (m_policer != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("The policer for sFlow drop monitor has already been created.");
        return false;
    }

    attr.id = SAI_POLICER_ATTR_METER_TYPE;
    attr.value.s32 = SAI_METER_TYPE_PACKETS;
    attributes.push_back(attr);
    attr.id = SAI_POLICER_ATTR_MODE;
    attr.value.s32 = SAI_POLICER_MODE_SR_TCM;
    attributes.push_back(attr);
    attr.id = SAI_POLICER_ATTR_CIR;
    attr.value.u64 = (sai_uint64_t)limit_rate;
    attributes.push_back(attr);
    attr.id = SAI_POLICER_ATTR_CBS;
    attr.value.u64 = (sai_uint64_t)limit_rate;
    attributes.push_back(attr);
    attr.id = SAI_POLICER_ATTR_GREEN_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
    attributes.push_back(attr);
    attr.id = SAI_POLICER_ATTR_YELLOW_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_DROP;
    attributes.push_back(attr);
    attr.id = SAI_POLICER_ATTR_RED_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_DROP;
    attributes.push_back(attr);

    status = sai_policer_api->create_policer(&policer, gSwitchId, (uint32_t)attributes.size(), attributes.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create policer, rv:%d", status);
        return false;
    }

    m_policer = policer;
    return true;
}

bool SflowDropMonitor::removePolicer()
{
    sai_status_t status;

    if (m_policer == SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    status = sai_policer_api->remove_policer(m_policer);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove policer, rv:%d", status);
        return false;
    }

    m_policer = SAI_NULL_OBJECT_ID;
    return true;
}

bool SflowDropMonitor::createHostifTrapGroup()
{
    sai_status_t            status;
    sai_attribute_t         attr;
    vector<sai_attribute_t> attributes;
    sai_object_id_t         hostif_trap_group = SAI_NULL_OBJECT_ID;

    if (m_hostifTrapGroup != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("The HOSTIF trap group has already been created.");
        return false;
    }

    attr.id = SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE;
    attr.value.u32 = SFLOW_DROP_MONITOR_CPU_QUEUE;
    attributes.push_back(attr);
    if (m_policer != SAI_NULL_OBJECT_ID)
    {
        attr.id = SAI_HOSTIF_TRAP_GROUP_ATTR_POLICER;
        attr.value.oid = m_policer;
        attributes.push_back(attr);
    }
    status = sai_hostif_api->create_hostif_trap_group(&hostif_trap_group, gSwitchId, (uint32_t)attributes.size(), attributes.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create HOSTIF trap group, rv:%d", status);
        return false;
    }

    m_hostifTrapGroup = hostif_trap_group;
    return true;
}

bool SflowDropMonitor::removeHostifTrapGroup()
{
    sai_status_t status;

    if (m_hostifTrapGroup == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("The HOSTIF trap group does not exist or has already been removed.");
        return true;
    }

    status = sai_hostif_api->remove_hostif_trap_group(m_hostifTrapGroup);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove HOSTIF trap group, rv:%d", status);
        return false;
    }

    m_hostifTrapGroup = SAI_NULL_OBJECT_ID;
    return true;
}

bool SflowDropMonitor::createHostifUserDefinedTrap()
{
    sai_status_t            status;
    sai_attribute_t         attr;
    vector<sai_attribute_t> attributes;
    sai_object_id_t         hostif_user_defined_trap = SAI_NULL_OBJECT_ID;
    sai_object_id_t         hostif_trap_group = m_hostifTrapGroup;

    if (m_hostifUserDefinedTrap != SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("The HOSTIF user defined trap has already been created.");
        return false;
    }

    if (hostif_trap_group == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("The HOSTIF trap group must be created before the HOSTIF user defined trap.");
        return false;
    }

    attr.id = SAI_HOSTIF_USER_DEFINED_TRAP_ATTR_TRAP_GROUP;
    attr.value.oid = hostif_trap_group;
    attributes.push_back(attr);
    attr.id = SAI_HOSTIF_USER_DEFINED_TRAP_ATTR_TYPE;
    attr.value.s32 = SAI_HOSTIF_USER_DEFINED_TRAP_TYPE_TAM;
    attributes.push_back(attr);

    status = sai_hostif_api->create_hostif_user_defined_trap(&hostif_user_defined_trap, gSwitchId, (uint32_t)attributes.size(), attributes.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create HOSTIF user defined trap, rv:%d", status);
        return false;
    }

    m_hostifUserDefinedTrap = hostif_user_defined_trap;
    return true;
}

bool SflowDropMonitor::removeHostifUserDefinedTrap()
{
    sai_status_t status;

    if (m_hostifUserDefinedTrap == SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    status = sai_hostif_api->remove_hostif_user_defined_trap(m_hostifUserDefinedTrap);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove HOSTIF user defined trap, rv:%d", status);
        return false;
    }

    m_hostifUserDefinedTrap = SAI_NULL_OBJECT_ID;
    return true;
}

bool SflowDropMonitor::initializeDropMonitor(int32_t limit_rate)
{
    return (createTamReport() &&
            createTamEventAction() &&
            createTamTransport() &&
            createPolicer(limit_rate) &&
            createHostifTrapGroup() &&
            createHostifUserDefinedTrap() &&
            createTamCollector() &&
            createTamEvent() &&
            createTam());
}

void SflowDropMonitor::cleanupDropMonitor()
{
    // Remove all created SAI objects
    removeTam();
    removeTamEvent();
    removeTamCollector();
    removeHostifUserDefinedTrap();
    removeHostifTrapGroup();
    removePolicer();
    removeTamTransport();
    removeTamEventAction();
    removeTamReport();
}

SflowOrch::SflowOrch(DBConnector* db, vector<string> &tableNames) :
    Orch(db, tableNames)
{
    SWSS_LOG_ENTER();
    m_sflowStatus = false;
}

bool SflowOrch::sflowCreateSession(uint32_t rate, SflowSession &session)
{
    sai_attribute_t attr;
    sai_object_id_t session_id = SAI_NULL_OBJECT_ID;
    sai_status_t    sai_rc;

    attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
    attr.value.u32 = rate;

    sai_rc = sai_samplepacket_api->create_samplepacket(&session_id, gSwitchId,
                                                       1, &attr);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create sample packet session with rate %d", rate);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_SAMPLEPACKET, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    session.m_sample_id = session_id;
    session.ref_count = 0;
    return true;
}

bool SflowOrch::sflowDestroySession(SflowSession &session)
{
    sai_status_t    sai_rc;

    sai_rc = sai_samplepacket_api->remove_samplepacket(session.m_sample_id);
    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to destroy sample packet session with id %" PRIx64 "",
                       session.m_sample_id);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_SAMPLEPACKET, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }
    return true;
}

bool SflowOrch::sflowUpdateRate(sai_object_id_t port_id, uint32_t rate)
{
    auto         port_info = m_sflowPortInfoMap.find(port_id);
    auto         session = m_sflowRateSampleMap.find(rate);
    SflowSession new_session;
    uint32_t     old_rate = sflowSessionGetRate(port_info->second.m_sample_id);

    if (session == m_sflowRateSampleMap.end())
    {
        if (!sflowCreateSession(rate, new_session))
        {
            SWSS_LOG_ERROR("Creating sflow session with rate %d failed", rate);
            return false;
        }
        m_sflowRateSampleMap[rate] = new_session;
    }
    else
    {
        new_session = session->second;
    }

    if (port_info->second.admin_state)
    {
        if (!sflowAddPort(new_session.m_sample_id, port_id, port_info->second.m_sample_dir))
        {
            return false;
        }
    }
    port_info->second.m_sample_id = new_session.m_sample_id;

    m_sflowRateSampleMap[rate].ref_count++;
    m_sflowRateSampleMap[old_rate].ref_count--;
    if (m_sflowRateSampleMap[old_rate].ref_count == 0)
    {
        if (!sflowDestroySession(m_sflowRateSampleMap[old_rate]))
        {
            SWSS_LOG_ERROR("Failed to clean old session %" PRIx64 " with rate %d",
                           m_sflowRateSampleMap[old_rate].m_sample_id, old_rate);
        }
        else
        {
            m_sflowRateSampleMap.erase(old_rate);
        }
    }
    return true;
}

bool SflowOrch::sflowAddPort(sai_object_id_t sample_id, sai_object_id_t port_id, string direction)
{
    sai_attribute_t attr;
    sai_status_t    sai_rc;

    SWSS_LOG_DEBUG("sflowAddPort  %" PRIx64 " portOid %" PRIx64 " dir %s",
                           sample_id, port_id, direction.c_str());

    if (direction == "both" || direction == "rx")
    {
        attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = sample_id;
        sai_rc = sai_port_api->set_port_attribute(port_id, &attr);

        if (sai_rc != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set session %" PRIx64 " on port %" PRIx64, sample_id, port_id);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, sai_rc);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }

    if (direction == "both" || direction == "tx")
    {
        attr.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = sample_id;
        sai_rc = sai_port_api->set_port_attribute(port_id, &attr);

        if (sai_rc != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set session %" PRIx64 " on port %" PRIx64, sample_id, port_id);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, sai_rc);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }
    return true;
}

bool SflowOrch::sflowDelPort(sai_object_id_t port_id, string direction)
{
    sai_attribute_t attr;
    sai_status_t    sai_rc;

    SWSS_LOG_DEBUG("sflowDelPort  portOid %" PRIx64 " dir %s",
                           port_id, direction.c_str());

    if (direction == "both" || direction == "rx")
    {
        attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = SAI_NULL_OBJECT_ID;
        sai_rc = sai_port_api->set_port_attribute(port_id, &attr);

        if (sai_rc != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to delete session on port %" PRIx64, port_id);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, sai_rc);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }

    if (direction == "both" || direction == "tx")
    {
        attr.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = SAI_NULL_OBJECT_ID;
        sai_rc = sai_port_api->set_port_attribute(port_id, &attr);

        if (sai_rc != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to delete session on port %" PRIx64, port_id);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, sai_rc);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }
    return true;
}

bool SflowOrch::sflowUpdateSampleDirection(sai_object_id_t port_id, string old_dir, string new_dir)
{
    sai_object_id_t ing_sample_oid = SAI_NULL_OBJECT_ID;
    sai_object_id_t egr_sample_oid = SAI_NULL_OBJECT_ID;
    sai_attribute_t attr;
    sai_status_t    sai_rc;
    auto            port_info = m_sflowPortInfoMap.find(port_id);

    SWSS_LOG_DEBUG("sflowUpdateSampleDirection  portOid %" PRIx64 " old dir %s new dir %s",
                           port_id, old_dir.c_str(), new_dir.c_str());

    if ((new_dir == "tx") && (old_dir == "rx" || old_dir == "both"))
    {
        ing_sample_oid = SAI_NULL_OBJECT_ID;
        egr_sample_oid = port_info->second.m_sample_id;
    }

    if ((new_dir == "rx") && (old_dir == "tx" || old_dir == "both"))
    {
        ing_sample_oid = port_info->second.m_sample_id;
        egr_sample_oid = SAI_NULL_OBJECT_ID;
    }

    if ((new_dir == "both") && (old_dir == "tx" || old_dir == "rx"))
    {
        ing_sample_oid = port_info->second.m_sample_id;
        egr_sample_oid = port_info->second.m_sample_id;
    }

    attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
    attr.value.oid = ing_sample_oid;
    sai_rc = sai_port_api->set_port_attribute(port_id, &attr);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to Ingress session on port %" PRIx64, port_id);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    attr.id = SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
    attr.value.oid = egr_sample_oid;
    sai_rc = sai_port_api->set_port_attribute(port_id, &attr);

    if (sai_rc != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to Update Egress session on port %" PRIx64, port_id);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_PORT, sai_rc);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

void SflowOrch::sflowExtractInfo(vector<FieldValueTuple> &fvs, bool &admin, uint32_t &rate, string &dir)
{
    for (auto i : fvs)
    {
        if (fvField(i) == "admin_state")
        {
            if (fvValue(i) == "up")
            {
                admin = true;
            }
            else if (fvValue(i) == "down")
            {
                admin = false;
            }
        }
        else if (fvField(i) == "sample_rate")
        {
            if (fvValue(i) != "error")
            {
                rate = (uint32_t)stoul(fvValue(i));
            }
            else
            {
                rate = 0;
            }
        }
        else if (fvField(i) == "sample_direction")
        {
            if (fvValue(i) != "error")
            {
                dir = fvValue(i);
            }
        }
    }
}

void SflowOrch::sflowExtractGlobalInfo(vector<FieldValueTuple> &fvs, bool &admin, uint32_t &rate, string &dir, int32_t &drop_monitor_limit)
{
    for (auto i : fvs)
    {
        if (fvField(i) == "admin_state")
        {
            if (fvValue(i) == "up")
            {
                admin = true;
            }
            else if (fvValue(i) == "down")
            {
                admin = false;
            }
        }
        else if (fvField(i) == "sample_rate")
        {
            if (fvValue(i) != "error")
            {
                rate = (uint32_t)stoul(fvValue(i));
            }
            else
            {
                rate = 0;
            }
        }
        else if (fvField(i) == "sample_direction")
        {
            if (fvValue(i) != "error")
            {
                dir = fvValue(i);
            }
        }
        else if (fvField(i) == "drop_monitor_limit")
        {
            try
            {
                drop_monitor_limit = stoi(fvValue(i));
            }
            catch (...)
            {
                stringstream err_msg;
                err_msg << "Incorrect drop_monitor_limit:" << fvValue(i) << ".";
                SWSS_LOG_ERROR("%s", err_msg.str().c_str());

                drop_monitor_limit = 0;
                continue;
            }
        }
    }
}

void SflowOrch::sflowStatusSet(Consumer &consumer)
{
    bool sflow_status = false;
    int32_t sflow_drop_monitor_limit = 0;

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        auto tuple = it->second;
        string op = kfvOp(tuple);
        uint32_t rate = 0;
        string dir = "";

        if (op == SET_COMMAND)
        {
            sflowExtractGlobalInfo(kfvFieldsValues(tuple), sflow_status, rate, dir, sflow_drop_monitor_limit);
        }
        else if (op == DEL_COMMAND)
        {
            sflow_status = false;
            sflow_drop_monitor_limit = 0;
        }
        it = consumer.m_toSync.erase(it);
    }

    // Enable, disable or change drop monitor limit when configuration changes
    if (m_sflowStatus != sflow_status ||
        m_sflowDropMonitor.getLimitRate() != sflow_drop_monitor_limit)
    {
        bool is_succ = true;

        // Drop monitor only enabled when sFlow is enabled
        if (sflow_status && sflow_drop_monitor_limit > 0)
        {
            is_succ = m_sflowDropMonitor.enableDropMonitor(sflow_drop_monitor_limit);
        }
        else
        {
            is_succ = m_sflowDropMonitor.disableDropMonitor();
        }

        if (is_succ)
        {
            m_sflowStatus = sflow_status;
        }
    }
}

uint32_t SflowOrch::sflowSessionGetRate(sai_object_id_t m_sample_id)
{
    for (auto it: m_sflowRateSampleMap)
    {
        if (it.second.m_sample_id == m_sample_id)
        {
            return it.first;
        }
    }
    return 0;
}

bool SflowOrch::handleSflowSessionDel(sai_object_id_t port_id)
{
    auto sflowInfo = m_sflowPortInfoMap.find(port_id);

    if (sflowInfo != m_sflowPortInfoMap.end())
    {
        uint32_t rate = sflowSessionGetRate(sflowInfo->second.m_sample_id);
        if (sflowInfo->second.admin_state)
        {
            if (!sflowDelPort(port_id, sflowInfo->second.m_sample_dir))
            {
                return false;
            }
            sflowInfo->second.admin_state = false;
        }

        m_sflowPortInfoMap.erase(port_id);
        m_sflowRateSampleMap[rate].ref_count--;
        if (m_sflowRateSampleMap[rate].ref_count == 0)
        {
            if (!sflowDestroySession(m_sflowRateSampleMap[rate]))
            {
                return false;
            }
            m_sflowRateSampleMap.erase(rate);
        }
    }
    return true;
}

void SflowOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    Port   port;
    string table_name = consumer.getTableName();

    if (table_name == APP_SFLOW_TABLE_NAME)
    {
        sflowStatusSet(consumer);
        return;
    }
    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto tuple = it->second;
        string op = kfvOp(tuple);
        string alias = kfvKey(tuple);

        gPortsOrch->getPort(alias, port);
        if (op == SET_COMMAND)
        {
            bool      admin_state = m_sflowStatus;
            uint32_t  rate       = 0;
            string dir = "rx";

            if (!m_sflowStatus)
            {
                return;
            }
            auto sflowInfo = m_sflowPortInfoMap.find(port.m_port_id);
            if (sflowInfo != m_sflowPortInfoMap.end())
            {
                rate = sflowSessionGetRate(sflowInfo->second.m_sample_id);
                admin_state = sflowInfo->second.admin_state;
            }

            SWSS_LOG_DEBUG(" Existing Cfg portOid %" PRIx64 " admin %d rate %d dir %s", 
                            port.m_port_id, (unsigned int)admin_state, rate, 
                            sflowInfo->second.m_sample_dir.c_str());

            sflowExtractInfo(kfvFieldsValues(tuple), admin_state, rate, dir);

            SWSS_LOG_DEBUG("New Cfg  portOid %" PRIx64 " admin %d rate %d dir %s", 
                            port.m_port_id, (unsigned int)admin_state, rate, dir.c_str());

            if (sflowInfo == m_sflowPortInfoMap.end())
            {
                if (rate == 0)
                {
                    it++;
                    continue;
                }

                SflowPortInfo port_info;
                auto          session_info = m_sflowRateSampleMap.find(rate);
                if (session_info != m_sflowRateSampleMap.end())
                {
                    port_info.m_sample_id = session_info->second.m_sample_id;
                }
                else
                {
                    SflowSession  session;
                    if (!sflowCreateSession(rate, session))
                    {
                        it++;
                        continue;
                    }
                    m_sflowRateSampleMap[rate] = session;
                    port_info.m_sample_id = session.m_sample_id;
                }
                port_info.m_sample_dir = dir;

                if (admin_state)
                {
                    if (!sflowAddPort(port_info.m_sample_id, port.m_port_id, port_info.m_sample_dir))
                    {
                        it++;
                        continue;
                    }
                }
                port_info.admin_state = admin_state;
                m_sflowPortInfoMap[port.m_port_id] = port_info;
                m_sflowRateSampleMap[rate].ref_count++;
            }
            else
            {
                if (rate != sflowSessionGetRate(sflowInfo->second.m_sample_id))
                {
                    if (!sflowUpdateRate(port.m_port_id, rate))
                    {
                        it++;
                        continue;
                    }
                }
                if (admin_state != sflowInfo->second.admin_state)
                {
                    bool ret = false;
                    if (admin_state)
                    {
                        ret = sflowAddPort(sflowInfo->second.m_sample_id, port.m_port_id,
                                            sflowInfo->second.m_sample_dir);
                    }
                    else
                    {
                        ret = sflowDelPort(port.m_port_id, sflowInfo->second.m_sample_dir);
                    }
                    if (!ret)
                    {
                        it++;
                        continue;
                    }
                    sflowInfo->second.admin_state = admin_state;
                }

                if (dir !=  sflowInfo->second.m_sample_dir)
                {
                    string old_dir = sflowInfo->second.m_sample_dir;
                    if (!sflowUpdateSampleDirection(port.m_port_id, old_dir, dir))
                    {
                        it++;
                        continue;
                    }
                    sflowInfo->second.m_sample_dir = dir;
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (!handleSflowSessionDel(port.m_port_id))
            {
                it++;
                continue;
            }
        }
        it = consumer.m_toSync.erase(it);
    }
}
