#include "tokenize.h"
#include "sai.h"
#include "policerorch.h"

#include "converter.h"
#include <inttypes.h>

using namespace std;
using namespace swss;

#define ETHERNET_PREFIX "Ethernet"
#define LAG_PREFIX      "PortChannel"
#define VLAN_PREFIX     "Vlan"

extern sai_policer_api_t*   sai_policer_api;

extern sai_object_id_t gSwitchId;
extern PortsOrch* gPortsOrch;
extern sai_port_api_t *sai_port_api;

static const string meter_type_field           = "METER_TYPE";
static const string mode_field                 = "MODE";
static const string color_source_field         = "COLOR_SOURCE";
static const string cbs_field                  = "CBS";
static const string cir_field                  = "CIR";
static const string pbs_field                  = "PBS";
static const string pir_field                  = "PIR";
static const string green_packet_action_field  = "GREEN_PACKET_ACTION";
static const string red_packet_action_field    = "RED_PACKET_ACTION";
static const string yellow_packet_action_field = "YELLOW_PACKET_ACTION";
static const string storm_control_kbps         = "KBPS";
static const string storm_control_enabled      = "ENABLED";
static const string storm_broadcast            = "broadcast";
static const string storm_unknown_unicast      = "unknown-unicast";
static const string storm_unknown_mcast        = "unknown-multicast";

static const map<string, sai_meter_type_t> meter_type_map = {
    {"PACKETS", SAI_METER_TYPE_PACKETS},
    {"BYTES", SAI_METER_TYPE_BYTES}
};

static const map<string, sai_policer_mode_t> policer_mode_map = {
    {"SR_TCM", SAI_POLICER_MODE_SR_TCM},
    {"TR_TCM", SAI_POLICER_MODE_TR_TCM},
    {"STORM_CONTROL", SAI_POLICER_MODE_STORM_CONTROL}
};

static const map<string, sai_policer_color_source_t> policer_color_source_map = {
    {"AWARE", SAI_POLICER_COLOR_SOURCE_AWARE},
    {"BLIND", SAI_POLICER_COLOR_SOURCE_BLIND}
};

static const map<string, sai_packet_action_t> packet_action_map = {
    {"DROP", SAI_PACKET_ACTION_DROP},
    {"FORWARD", SAI_PACKET_ACTION_FORWARD},
    {"COPY", SAI_PACKET_ACTION_COPY},
    {"COPY_CANCEL", SAI_PACKET_ACTION_COPY_CANCEL},
    {"TRAP", SAI_PACKET_ACTION_TRAP},
    {"LOG", SAI_PACKET_ACTION_LOG},
    {"DENY", SAI_PACKET_ACTION_DENY},
    {"TRANSIT", SAI_PACKET_ACTION_TRANSIT}
};

static const map<string, sai_port_attr_t> storm_to_attr_map = {
    {storm_broadcast, SAI_PORT_ATTR_BROADCAST_STORM_CONTROL_POLICER_ID},
    {storm_unknown_unicast, SAI_PORT_ATTR_FLOOD_STORM_CONTROL_POLICER_ID},
    {storm_unknown_mcast, SAI_PORT_ATTR_MULTICAST_STORM_CONTROL_POLICER_ID}
};

bool PolicerOrch::policerExists(const string &name)
{
    SWSS_LOG_ENTER();

    return m_syncdPolicers.find(name) != m_syncdPolicers.end();
}

bool PolicerOrch::getPolicerOid(const string &name, sai_object_id_t &oid)
{
    SWSS_LOG_ENTER();

    if (policerExists(name))
    {
        oid = m_syncdPolicers[name].policerOid;
        SWSS_LOG_NOTICE("Get policer %s oid:%" PRIx64, name.c_str(), oid);
        return true;
    }

    return false;
}

bool PolicerOrch::increaseRefCount(const string &name)
{
    SWSS_LOG_ENTER();

    if (!policerExists(name))
    {
        SWSS_LOG_WARN("Policer %s does not exist", name.c_str());
        return false;
    }

    ++m_policerRefCounts[name];

    SWSS_LOG_INFO("Policer %s reference count is increased to %d",
            name.c_str(), m_policerRefCounts[name]);
    return true;
}

bool PolicerOrch::decreaseRefCount(const string &name)
{
    SWSS_LOG_ENTER();

    if (!policerExists(name))
    {
        SWSS_LOG_WARN("Policer %s does not exist", name.c_str());
        return false;
    }

    --m_policerRefCounts[name];

    SWSS_LOG_INFO("Policer %s reference count is decreased to %d",
            name.c_str(), m_policerRefCounts[name]);
    return true;
}

PolicerOrch::PolicerOrch(vector<TableConnector> &tableNames, PortsOrch *portOrch) : Orch(tableNames), m_portsOrch(portOrch)
{
    SWSS_LOG_ENTER();
    initPolicerTypeTableHandlers();
    m_portsOrch->attach(this);
}

void PolicerOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        /* Make sure the handler is initialized for the task */
        auto table_name = consumer.getTableName();
        if (m_policer_type_table_handler_map.find(table_name) == m_policer_type_table_handler_map.end())
        {
            SWSS_LOG_ERROR("Task %s handler is not initialized", table_name.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        auto task_status = (this->*(m_policer_type_table_handler_map[table_name]))(consumer);
        switch(task_status)
        {
            case task_process_status::task_success :
                it = consumer.m_toSync.erase(it);
                break;
            case task_process_status::task_invalid_entry :
                SWSS_LOG_ERROR("Failed to process invalid Policer task");
                it = consumer.m_toSync.erase(it);
                break;
            case task_process_status::task_failed :
                SWSS_LOG_ERROR("Failed to process Policer task, drop it");
                it = consumer.m_toSync.erase(it);
                return;
            case task_process_status::task_need_retry :
                SWSS_LOG_INFO("Failed to process Policer task, retry it");
                it++;
                break;
            case task_process_status::task_ignore:
                SWSS_LOG_INFO("Ignore Policer task");
                it = consumer.m_toSync.erase(it);
                break;
            default:
                SWSS_LOG_ERROR("Invalid task status %d", task_status);
                it = consumer.m_toSync.erase(it);
                break;
        }
    }
}

void PolicerOrch::initPolicerTypeTableHandlers()
{
    SWSS_LOG_ENTER();
    m_policer_type_table_handler_map.insert(policer_type_table_handler_pair(
                CFG_PORT_STORM_CONTROL_TABLE_NAME, 
                &PolicerOrch::handlePortStormControlTable));
    m_policer_type_table_handler_map.insert(policer_type_table_handler_pair(
                CFG_POLICER_TABLE_NAME, 
                &PolicerOrch::handlePolicerTable));
}

/*Handler for "POLICER" table*/
task_process_status PolicerOrch::handlePolicerTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    KeyOpFieldsValuesTuple tuple = consumer.m_toSync.begin()->second;

    string key = kfvKey(tuple);
    string op = kfvOp(tuple);

    if (op == SET_COMMAND)
    {
        // Mark the operation as an 'update', if the policer exists.
        bool update = m_syncdPolicers.find(key) != m_syncdPolicers.end();

        vector<sai_attribute_t> attrs;
        bool meter_type = false, mode = false;

        for (auto i = kfvFieldsValues(tuple).begin();
                i != kfvFieldsValues(tuple).end(); ++i)
        {
            auto field = to_upper(fvField(*i));
            auto value = to_upper(fvValue(*i));

            SWSS_LOG_DEBUG("attribute: %s value: %s", field.c_str(), value.c_str());

            sai_attribute_t attr;

            if (field == meter_type_field)
            {
                attr.id = SAI_POLICER_ATTR_METER_TYPE;
                attr.value.s32 = (sai_meter_type_t) meter_type_map.at(value);
                meter_type = true;
            }
            else if (field == mode_field)
            {
                attr.id = SAI_POLICER_ATTR_MODE;
                attr.value.s32 = (sai_policer_mode_t) policer_mode_map.at(value);
                mode = true;
            }
            else if (field == color_source_field)
            {
                attr.id = SAI_POLICER_ATTR_COLOR_SOURCE;
                attr.value.s32 = policer_color_source_map.at(value);
            }
            else if (field == cbs_field)
            {
                attr.id = SAI_POLICER_ATTR_CBS;
                attr.value.u64 = stoul(value);
            }
            else if (field == cir_field)
            {
                attr.id = SAI_POLICER_ATTR_CIR;
                attr.value.u64 = stoul(value);
            }
            else if (field == pbs_field)
            {
                attr.id = SAI_POLICER_ATTR_PBS;
                attr.value.u64 = stoul(value);
            }
            else if (field == pir_field)
            {
                attr.id = SAI_POLICER_ATTR_PIR;
                attr.value.u64 = stoul(value);
            }
            else if (field == red_packet_action_field)
            {
                attr.id = SAI_POLICER_ATTR_RED_PACKET_ACTION;
                attr.value.s32 = packet_action_map.at(value);
            }
            else if (field == green_packet_action_field)
            {
                attr.id = SAI_POLICER_ATTR_GREEN_PACKET_ACTION;
                attr.value.s32 = packet_action_map.at(value);
            }
            else if (field == yellow_packet_action_field)
            {
                attr.id = SAI_POLICER_ATTR_YELLOW_PACKET_ACTION;
                attr.value.s32 = packet_action_map.at(value);
            }
            else
            {
                SWSS_LOG_ERROR("Unknown policer attribute %s specified",
                        field.c_str());
                continue;
            }

            attrs.push_back(attr);
        }

        // Create a new policer
        if (!update)
        {
            if (!meter_type || !mode)
            {
                SWSS_LOG_ERROR("Failed to create policer %s,\
                        missing mandatory fields", key.c_str());
                return task_process_status::task_invalid_entry;
            }

            sai_object_id_t policer_id;
            sai_status_t status = sai_policer_api->create_policer(
                    &policer_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to create policer %s, rv:%d",
                        key.c_str(), status);
                return task_process_status::task_need_retry;
            }

            SWSS_LOG_NOTICE("Created policer %s", key.c_str());
            m_syncdPolicers[key].policerOid = policer_id;
            m_policerRefCounts[key] = 0;
        }
        else
        {
            auto policer_id = m_syncdPolicers[key].policerOid;

            // The update operation has limitations that it could only update
            // the rate and the size accordingly.
            // SR_TCM: CIR, CBS, PBS
            // TR_TCM: CIR, CBS, PIR, PBS
            // STORM_CONTROL: CIR, CBS
            for (auto & attr: attrs)
            {
                if (attr.id != SAI_POLICER_ATTR_CBS &&
                        attr.id != SAI_POLICER_ATTR_CIR &&
                        attr.id != SAI_POLICER_ATTR_PBS &&
                        attr.id != SAI_POLICER_ATTR_PIR)
                {
                    continue;
                }

                sai_status_t status = sai_policer_api->set_policer_attribute(
                        policer_id, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to update policer %s attribute, rv:%d",
                            key.c_str(), status);
                    return task_process_status::task_need_retry;
                }
            }
            SWSS_LOG_NOTICE("Update policer %s attributes", key.c_str());
        }
    }
    else if (op == DEL_COMMAND)
    {
        if (m_syncdPolicers.find(key) == m_syncdPolicers.end())
        {
            SWSS_LOG_ERROR("Policer %s does not exists", key.c_str());
            return task_process_status::task_invalid_entry;
        }

        if (m_policerRefCounts[key] > 0)
        {
            SWSS_LOG_INFO("Policer %s is still referenced", key.c_str());
            return task_process_status::task_need_retry;
        }

        sai_status_t status = sai_policer_api->remove_policer(
                m_syncdPolicers[key].policerOid);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove policer %s, rv:%d",
                    key.c_str(), status);
            return task_process_status::task_need_retry;
        }

        SWSS_LOG_NOTICE("Removed policer %s", key.c_str());
        m_syncdPolicers.erase(key);
        m_policerRefCounts.erase(key);
    }

    return task_process_status::task_success;
}

bool PolicerOrch::isStormControlPolicer(string policer_name)
{
    /*
     * Temporary logic to identify storm-control policer
     * To update the PolicerTable structure to include
     * policer_type
     */
    if (policer_name[0] == '_')
        return true;

    return false;
}

/*Handler for "PORT_STORM_CONTROL" table*/
task_process_status PolicerOrch::handlePortStormControlTable(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    KeyOpFieldsValuesTuple tuple = consumer.m_toSync.begin()->second;

    string key = kfvKey(tuple);
    string op = kfvOp(tuple);

    /*<interface_name1>:<storm_type1>, <interface_name1>:<storm_type2>, ... (OR)
      <interface_name1>:<storm_type1>, <interface_name2>:<storm_type1>, ...*/
    vector<string> storm_control_keys = tokenize(key, list_item_delimiter);
    for (string storm_key : storm_control_keys)
    {
        std::size_t delimiter = storm_key.find_first_of("|");
        if (delimiter == std::string::npos)
        {
            SWSS_LOG_ERROR("Unable to parse key %s",storm_key.c_str());
            /*continue here as there can be more interfaces*/
            continue;
        }
        const auto interface_name = storm_key.substr(0,delimiter);
        const auto storm_type = storm_key.substr(delimiter+1);
        Port port;

        /*Only proceed for Ethernet interfaces*/
        if (strncmp(interface_name.c_str(), ETHERNET_PREFIX, strlen(ETHERNET_PREFIX)))
        {
            SWSS_LOG_ERROR("%s: Unsupported / Invalid interface %s",
                    storm_type.c_str(), interface_name.c_str());
            /*continue here as there can be more interfaces*/
            continue;
        }

        if (!gPortsOrch->getPort(interface_name, port))
        {
            SWSS_LOG_ERROR("Failed to apply storm-control %s to port %s. Port not found",
                    storm_type.c_str(), interface_name.c_str());
            /*continue here as there can be more interfaces*/
            continue; 
        }

        /*Policer Name: _<interface_name>_<storm_type>*/
        const auto storm_policer_name = std::string("_").append(interface_name).append("_").append(storm_type);

        if (op == SET_COMMAND)
        {
            // Mark the opeartion as an 'update', if the policer exists.
            bool update = m_syncdPolicers.find(key) != m_syncdPolicers.end();
            vector <sai_attribute_t> attrs;
            bool cir = false;
            sai_attribute_t attr;

            /*Meter type hardcoded to BYTES*/
            attr.id = SAI_POLICER_ATTR_METER_TYPE;
            attr.value.s32 = (sai_meter_type_t) meter_type_map.at("BYTES");
            attrs.push_back(attr);

            /*Meter mode hardcoded to STORM_CONTROL*/
            attr.id = SAI_POLICER_ATTR_MODE;
            attr.value.s32 = (sai_policer_mode_t) policer_mode_map.at("STORM_CONTROL");
            attrs.push_back(attr);

            /*Red Packet Action hardcoded to DROP*/
            attr.id = SAI_POLICER_ATTR_RED_PACKET_ACTION;
            attr.value.s32 = packet_action_map.at("DROP");
            attrs.push_back(attr);

            for (auto i = kfvFieldsValues(tuple).begin();
                    i != kfvFieldsValues(tuple).end(); ++i)
            {
                auto field = to_upper(fvField(*i));
                auto value = to_upper(fvValue(*i));

                /*BPS value is used as CIR*/
                if (field == storm_control_kbps)
                {
                    attr.id = SAI_POLICER_ATTR_CIR;
                    /*convert kbps to bps*/
                    attr.value.u64 = (stoul(value)*1000/8);
                    cir = true;
                    attrs.push_back(attr);
                    SWSS_LOG_INFO("CIR %s",value.c_str());
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown storm control attribute %s specified",
                            field.c_str());
                    continue;
                }
            }
            /*CIR is mandatory parameter*/
            if (!cir)
            {
                SWSS_LOG_ERROR("Failed to create storm control policer %s,\
                        missing madatory fields", storm_policer_name.c_str());
                /*
                 * return here as the same error 
                 * would be seen for the complete iteration
                 */
                return task_process_status::task_invalid_entry;
            }

            /*Enabling storm-control on port*/
            sai_attribute_t port_attr;
            if (storm_type == storm_broadcast)
            {
                port_attr.id = SAI_PORT_ATTR_BROADCAST_STORM_CONTROL_POLICER_ID;
            }
            else if (storm_type == storm_unknown_unicast)
            {
                port_attr.id = SAI_PORT_ATTR_FLOOD_STORM_CONTROL_POLICER_ID;
            }
            else if (storm_type == storm_unknown_mcast)
            {
                port_attr.id = SAI_PORT_ATTR_MULTICAST_STORM_CONTROL_POLICER_ID;
            }
            else
            {
                SWSS_LOG_ERROR("Unknown storm_type %s", storm_type.c_str());
                /*continue as there can be more interfaces*/
                continue;
            }

            sai_object_id_t policer_id;
            // Create a new policer
            if (!update)
            {
                sai_status_t status = sai_policer_api->create_policer(
                        &policer_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to create policer %s, rv:%d",
                            storm_policer_name.c_str(), status);
                    /* 
                     * Returning here since the same failure 
                     * would be seen for the full iteration
                     */
                    return task_process_status::task_need_retry;
                }

                SWSS_LOG_NOTICE("Created storm-control policer %s", storm_policer_name.c_str());
                m_syncdPolicers[storm_policer_name].policerOid = policer_id;
                m_policerRefCounts[storm_policer_name] = 0;
            }
            // Update an existing policer
            else
            {
                policer_id = m_syncdPolicers[key].policerOid;

                // The update operation has limitations that it could only update
                // the rate and the size accordingly.
                // SR_TCM: CIR, CBS, PBS
                // TR_TCM: CIR, CBS, PIR, PBS
                // STORM_CONTROL: CIR, CBS
                for (auto & attr: attrs)
                {
                    if (attr.id != SAI_POLICER_ATTR_CBS &&
                            attr.id != SAI_POLICER_ATTR_CIR &&
                            attr.id != SAI_POLICER_ATTR_PBS &&
                            attr.id != SAI_POLICER_ATTR_PIR)
                    {
                        continue;
                    }

                    sai_status_t status = sai_policer_api->set_policer_attribute(
                            policer_id, &attr);
                    if (status != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("Failed to update policer %s attribute, rv:%d",
                                key.c_str(), status);
                        continue;
                    }
                }

                SWSS_LOG_NOTICE("Update policer %s attributes", key.c_str());
            }
            policer_id = m_syncdPolicers[storm_policer_name].policerOid;

            if(update)
            {
                SWSS_LOG_NOTICE("update storm-control policer %s id:%ld", storm_policer_name.c_str(),policer_id);
            }
            port_attr.value.oid = policer_id;

            sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &port_attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to apply storm-control %s to port %s, rv:%d",
                        storm_type.c_str(), interface_name.c_str(),status);

                /*TODO: Do the below policer cleanup in an API*/
                /*Remove the already created policer*/
                if (SAI_STATUS_SUCCESS != sai_policer_api->remove_policer(
                            m_syncdPolicers[storm_policer_name].policerOid))
                {
                    SWSS_LOG_ERROR("Failed to remove policer %s, rv:%d",
                            storm_policer_name.c_str(), status);
                    /*TODO: Just doing a syslog. */
                }

                SWSS_LOG_NOTICE("Removed policer %s as set_port_attribute for %s failed", 
                        storm_policer_name.c_str(),interface_name.c_str());
                m_syncdPolicers.erase(storm_policer_name);
                m_policerRefCounts.erase(storm_policer_name);

                /*continue as there can be more interfaces*/
                continue;
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_syncdPolicers.find(storm_policer_name) == m_syncdPolicers.end())
            {
                SWSS_LOG_ERROR("Policer %s not configured", storm_policer_name.c_str());
                /*continue as there can be more interfaces*/
                continue;
            }

            sai_attribute_t port_attr;
            if (storm_type == storm_broadcast)
            {
                port_attr.id = SAI_PORT_ATTR_BROADCAST_STORM_CONTROL_POLICER_ID;
            }
            else if (storm_type == storm_unknown_unicast)
            {
                port_attr.id = SAI_PORT_ATTR_FLOOD_STORM_CONTROL_POLICER_ID;
            }
            else if (storm_type == storm_unknown_mcast)
            {
                port_attr.id = SAI_PORT_ATTR_MULTICAST_STORM_CONTROL_POLICER_ID;
            }
            else
            {
                SWSS_LOG_ERROR("Unknown storm_type %s", storm_type.c_str());
                /*continue as there can be more interfaces*/
                continue;
            }

            port_attr.value.oid = SAI_NULL_OBJECT_ID;

            sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &port_attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove storm-control %s from port %s, rv:%d",
                        storm_type.c_str(), interface_name.c_str(), status);
                /*continue as there can be more interfaces*/
                continue;
            }

            status = sai_policer_api->remove_policer(
                    m_syncdPolicers[storm_policer_name].policerOid);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove policer %s, rv:%d",
                        storm_policer_name.c_str(), status);
                /*Continue as there can be more interfaces*/
                continue;
            }

            SWSS_LOG_NOTICE("Removed policer %s", storm_policer_name.c_str());
            m_syncdPolicers.erase(storm_policer_name);
            m_policerRefCounts.erase(storm_policer_name);

        }
    }
    /*TODO: To return appropriate errors*/
    return task_process_status::task_success;
}

bool PolicerOrch::handlePhyDelete(Port &port)
{
    string storm_policer_name;
    sai_attribute_t port_attr;
    sai_status_t status;

    SWSS_LOG_NOTICE("Policer: Received delete for port %s", port.m_alias.c_str());

    for (auto it = storm_to_attr_map.begin(); it != storm_to_attr_map.end(); it++)
    {
        storm_policer_name = std::string("_").append(port.m_alias.c_str()).append("_").append(it->first);
        SWSS_LOG_NOTICE("Policer %s", storm_policer_name.c_str());
        if (m_syncdPolicers.find(storm_policer_name) != m_syncdPolicers.end())
        {
            port_attr.id = it->second;
            port_attr.value.oid = SAI_NULL_OBJECT_ID;
            status = sai_port_api->set_port_attribute(port.m_port_id, &port_attr);
            SWSS_LOG_NOTICE("Removing %s policer from port %s", 
                    storm_policer_name.c_str(), port.m_alias.c_str());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_NOTICE("Failed to remove policer %s from port %s", 
                        storm_policer_name.c_str(), port.m_alias.c_str());
            }
            status = sai_policer_api->remove_policer(
                    m_syncdPolicers[storm_policer_name].policerOid);
            SWSS_LOG_NOTICE("Deleting policer %s created for port %s", 
                    storm_policer_name.c_str(), port.m_alias.c_str());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_NOTICE("Failed to delete policer %s created for port %s", 
                        storm_policer_name.c_str(), port.m_alias.c_str());
            }
            m_syncdPolicers.erase(storm_policer_name);
            m_policerRefCounts.erase(storm_policer_name);
        }
    }

    return true;
}

void PolicerOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();
    assert(cntx);

    switch(type)
    {
        case SUBJECT_TYPE_PORT_CHANGE:
        {
            PortUpdate *update = static_cast<PortUpdate *>(cntx);
            Port &port = update->port;

            if (port.m_type == Port::PHY)
            {
                if (update->add)
                {
                    SWSS_LOG_NOTICE("Add port %s", port.m_alias.c_str());
                }
                else
                {
                    SWSS_LOG_NOTICE("Del port %s", port.m_alias.c_str());
                    if (!handlePhyDelete(port))
                    {
                        SWSS_LOG_INFO("Unable to handle QoS Mapping during port delete %s",
                                port.m_alias.c_str());
                    }
                }
            }
            break;
        }
        default:
            break;
    }
    return;
}
