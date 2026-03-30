#include <limits.h>
#include <inttypes.h>
#include <unordered_map>
#include "pfcwdorch.h"
#include "sai_serialize.h"
#include "portsorch.h"
#include "converter.h"
#include "redisapi.h"
#include "select.h"
#include "notifier.h"
#include "schema.h"
#include "subscriberstatetable.h"

#define BIG_RED_SWITCH_FIELD            "BIG_RED_SWITCH"
#define PFC_WD_IN_STORM                 "storm"
#define PFC_WD_POLL_TIMEOUT             5000
#define SAI_PORT_STAT_PFC_PREFIX        "SAI_PORT_STAT_PFC_"
#define COUNTER_CHECK_POLL_TIMEOUT_SEC  1

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t* sai_switch_api;
extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;
extern sai_buffer_api_t *sai_buffer_api;

extern event_handle_t g_events_handle;

extern SwitchOrch *gSwitchOrch;
extern PortsOrch *gPortsOrch;

PfcWdBaseOrch::PfcWdBaseOrch(DBConnector *db, vector<string> &tableNames):
    Orch(db, tableNames),
    m_countersDb(new DBConnector("COUNTERS_DB", 0)),
    m_countersTable(new Table(m_countersDb.get(), COUNTERS_TABLE)),
    m_stateDb(new DBConnector("STATE_DB", 0)),
    m_stateTable(new Table(m_stateDb.get(), PFC_WD_STATE_TABLE)),
    m_platform(getenv("platform") ? getenv("platform") : "")
{
    SWSS_LOG_ENTER();
    if (m_platform == "")
    {
        SWSS_LOG_ERROR("Platform environment variable is not defined");
        return;
    }

    // Add PfcDlrPacketAction to state table
    string dlrAction = PfcWdBaseOrch::serializeAction(this->getPfcDlrPacketAction());
    this->updateStateTable(PFC_WD_DLR_PACKET_ACTION, dlrAction);
}


PfcWdBaseOrch::~PfcWdBaseOrch(void)
{
    SWSS_LOG_ENTER();
}

bool PfcWdBaseOrch::getLosslessTcsForPort(const Port& port, set<uint8_t>& losslessTc)
{
    SWSS_LOG_ENTER();

    // Get PFC mask to identify lossless queues
    uint8_t pfcMask = 0;
    if (!gPortsOrch->getPortPfcWatchdogStatus(port.m_port_id, &pfcMask))
    {
        SWSS_LOG_ERROR("Failed to get PFC mask on port %s", port.m_alias.c_str());
        return false;
    }

    // Collect lossless TCs (traffic classes with PFC enabled)
    losslessTc.clear();
    for (uint8_t i = 0; i < PFC_WD_TC_MAX; i++)
    {
        if ((pfcMask & (1 << i)) != 0)
        {
            losslessTc.insert(i);
        }
    }

    if (losslessTc.empty())
    {
        return false;
    }

    SWSS_LOG_INFO("Found %zu lossless TCs on port %s", losslessTc.size(), port.m_alias.c_str());
    return true;
}

void PfcWdBaseOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    if ((consumer.getDbName() == "CONFIG_DB") && (consumer.getTableName() == CFG_PFC_WD_TABLE_NAME))
    {
        auto it = consumer.m_toSync.begin();
        while (it != consumer.m_toSync.end())
        {
            KeyOpFieldsValuesTuple t = it->second;

            string key = kfvKey(t);
            string op = kfvOp(t);

            task_process_status task_status = task_process_status::task_ignore;
            if (op == SET_COMMAND)
            {
                task_status = createEntry(key, kfvFieldsValues(t));
            }
            else if (op == DEL_COMMAND)
            {
                task_status = deleteEntry(key);
            }
            else
            {
                task_status = task_process_status::task_invalid_entry;
                SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
            }
            switch (task_status)
            {
                case task_process_status::task_success:
                    consumer.m_toSync.erase(it++);
                    break;
                case task_process_status::task_need_retry:
                    SWSS_LOG_INFO("Failed to process PFC watchdog %s task, retry it", op.c_str());
                    ++it;
                    break;
                case task_process_status::task_invalid_entry:
                    SWSS_LOG_ERROR("Failed to process PFC watchdog %s task, invalid entry", op.c_str());
                    consumer.m_toSync.erase(it++);
                    break;
                default:
                    SWSS_LOG_ERROR("Invalid task status %d", task_status);
                    consumer.m_toSync.erase(it++);
                    break;
            }
        }

        if (m_pfcwdFlexCounterManager != nullptr)
        {
            m_pfcwdFlexCounterManager->flush();
        }
    }
}

PfcWdAction PfcWdBaseOrch::deserializeAction(const string& key)
{
    SWSS_LOG_ENTER();

    const map<string, PfcWdAction> actionMap =
    {
        { "forward", PfcWdAction::PFC_WD_ACTION_FORWARD },
        { "drop", PfcWdAction::PFC_WD_ACTION_DROP },
        { "alert", PfcWdAction::PFC_WD_ACTION_ALERT },
    };

    if (actionMap.find(key) == actionMap.end())
    {
        return PfcWdAction::PFC_WD_ACTION_UNKNOWN;
    }

    return actionMap.at(key);
}

string PfcWdBaseOrch::serializeAction(const PfcWdAction& action)
{
    SWSS_LOG_ENTER();

    const map<PfcWdAction, string> actionMap =
    {
        { PfcWdAction::PFC_WD_ACTION_FORWARD, "forward" },
        { PfcWdAction::PFC_WD_ACTION_DROP, "drop" },
        { PfcWdAction::PFC_WD_ACTION_ALERT, "alert" },
    };

    if (actionMap.find(action) == actionMap.end())
    {
        return "unknown";
    }

    return actionMap.at(action);
}


task_process_status PfcWdBaseOrch::createEntry(const string& key,
        const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();

    uint32_t detectionTime = 0;
    uint32_t restorationTime = 0;
    // According to requirements, drop action is default
    PfcWdAction action = PfcWdAction::PFC_WD_ACTION_DROP; 
    string pfcStatHistory = "disable";
    Port port;
    if (!gPortsOrch->getPort(key, port))
    {
        SWSS_LOG_ERROR("Invalid port interface %s", key.c_str());
        return task_process_status::task_invalid_entry;
    }

    if (port.m_type != Port::PHY)
    {
        SWSS_LOG_ERROR("Interface %s is not physical port", key.c_str());
        return task_process_status::task_invalid_entry;
    }

    for (auto i : data)
    {
        const auto &field = fvField(i);
        const auto &value = fvValue(i);

        try
        {
            if (field == PFC_WD_DETECTION_TIME)
            {
                detectionTime = to_uint<uint32_t>(
                        value,
                        PFC_WD_DETECTION_TIME_MIN,
                        PFC_WD_DETECTION_TIME_MAX);
            }
            else if (field == PFC_WD_RESTORATION_TIME)
            {
                restorationTime = to_uint<uint32_t>(value,
                        PFC_WD_RESTORATION_TIME_MIN,
                        PFC_WD_RESTORATION_TIME_MAX);
            }
            else if (field == PFC_WD_ACTION)
            {
                action = deserializeAction(value);
                if (action == PfcWdAction::PFC_WD_ACTION_UNKNOWN)
                {
                    SWSS_LOG_ERROR("Invalid PFC Watchdog action %s", value.c_str());
                    return task_process_status::task_invalid_entry;
                }
                if ((m_platform == CISCO_8000_PLATFORM_SUBSTRING) && (action == PfcWdAction::PFC_WD_ACTION_FORWARD)) {
                    SWSS_LOG_ERROR("Unsupported action %s for platform %s", value.c_str(), m_platform.c_str());
                    return task_process_status::task_invalid_entry;
                }
                if(m_platform == BRCM_PLATFORM_SUBSTRING)
                {
                    if(gSwitchOrch->checkPfcDlrInitEnable())
                    {
                        if(m_pfcwd_ports.empty())
                        {
                            sai_attribute_t attr;
                            attr.id = SAI_SWITCH_ATTR_PFC_DLR_PACKET_ACTION;
                            attr.value.u32 = packet_action_map.at(value);

                            sai_status_t status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
                            if(status != SAI_STATUS_SUCCESS)
                            {
                                SWSS_LOG_ERROR("Failed to set switch level PFC DLR packet action rv : %d", status);
                                return task_process_status::task_invalid_entry;
                            }
                            setPfcDlrPacketAction(action);
                        }
                        else
                        {
                            if(getPfcDlrPacketAction() != action)
                            {
                                string DlrPacketAction = serializeAction(getPfcDlrPacketAction());
                                SWSS_LOG_ERROR("Invalid PFC Watchdog action %s as switch level action %s is set",
                                               value.c_str(), DlrPacketAction.c_str());
                                return task_process_status::task_invalid_entry;
                            }
                        }
                    }
                }
            }
            else if(field == PFC_STAT_HISTORY){
                pfcStatHistory = value;
            }
            else
            {
                SWSS_LOG_ERROR(
                        "Failed to parse PFC Watchdog %s configuration. Unknown attribute %s.\n",
                        key.c_str(),
                        field.c_str());
                return task_process_status::task_invalid_entry;
            }
        }
        catch (const exception& e)
        {
            SWSS_LOG_ERROR(
                    "Failed to parse PFC Watchdog %s attribute %s error: %s.",
                    key.c_str(),
                    field.c_str(),
                    e.what());
            return task_process_status::task_invalid_entry;
        }
        catch (...)
        {
            SWSS_LOG_ERROR(
                    "Failed to parse PFC Watchdog %s attribute %s. Unknown error has been occurred",
                    key.c_str(),
                    field.c_str());
            return task_process_status::task_invalid_entry;
        }
    }

    // Validation
    if (detectionTime == 0)
    {
        SWSS_LOG_ERROR("%s missing", PFC_WD_DETECTION_TIME);
        return task_process_status::task_invalid_entry;
    }
    if (pfcStatHistory != "enable" && pfcStatHistory != "disable")
    {
        SWSS_LOG_ERROR("%s is invalid value for %s", pfcStatHistory.c_str(), PFC_STAT_HISTORY);
        return task_process_status::task_invalid_entry;
    }

    if (!startWdOnPort(port, detectionTime, restorationTime, action, pfcStatHistory))
    {
        SWSS_LOG_ERROR("Failed to start PFC Watchdog on port %s", port.m_alias.c_str());
        return task_process_status::task_need_retry;
    }

    SWSS_LOG_NOTICE("Started PFC Watchdog on port %s", port.m_alias.c_str());
    m_pfcwd_ports.insert(port.m_alias);
    return task_process_status::task_success;
}

task_process_status PfcWdBaseOrch::deleteEntry(const string& name)
{
    SWSS_LOG_ENTER();

    Port port;
    gPortsOrch->getPort(name, port);

    if (!stopWdOnPort(port))
    {
        SWSS_LOG_ERROR("Failed to stop PFC Watchdog on port %s", name.c_str());
        return task_process_status::task_failed;
    }

    SWSS_LOG_NOTICE("Stopped PFC Watchdog on port %s", name.c_str());
    m_pfcwd_ports.erase(port.m_alias);
    return task_process_status::task_success;
}

void PfcWdBaseOrch::report_pfc_storm(sai_object_id_t queueId, sai_object_id_t portId,
                                     uint8_t queueIndex, const string& portAlias,
                                     const string& info)
{
    event_params_t params = {
        { "ifname", portAlias },
        { "queue_index", to_string(queueIndex) },
        { "queue_id", to_string(queueId) },
        { "port_id", to_string(portId) }};

    if (info.empty())
    {
        SWSS_LOG_NOTICE(
            "PFC Watchdog detected PFC storm on port %s, queue index %d, queue id 0x%" PRIx64 " and port id 0x%" PRIx64,
            portAlias.c_str(),
            queueIndex,
            queueId,
            portId);
    }
    else
    {
        SWSS_LOG_NOTICE(
            "PFC Watchdog detected PFC storm on port %s, queue index %d, queue id 0x%" PRIx64 " and port id 0x%" PRIx64 ", additional info: %s.",
            portAlias.c_str(),
            queueIndex,
            queueId,
            portId,
            info.c_str());
        params["additional_info"] = info;
    }

    event_publish(g_events_handle, "pfc-storm", &params);
}

void PfcWdBaseOrch::report_pfc_restored(sai_object_id_t queueId, sai_object_id_t portId,
                                        uint8_t queueIndex, const string& portAlias)
{
    SWSS_LOG_NOTICE(
        "PFC Watchdog storm restored on port %s, queue index %d, queue id 0x%" PRIx64 " and port id 0x%" PRIx64 ".",
        portAlias.c_str(),
        queueIndex,
        queueId,
        portId);
}
