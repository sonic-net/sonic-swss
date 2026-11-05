#include <limits.h>
#include <inttypes.h>
#include <cstring>
#include <unordered_map>
#include <sys/time.h>
#include "pfcwdorch.h"
#include "sai_serialize.h"
#include "portsorch.h"
#include "converter.h"
#include "redisapi.h"
#include "select.h"
#include "notifier.h"
#include "schema.h"
#include "subscriberstatetable.h"
#include "warm_restart.h"

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t* sai_switch_api;
extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;

extern event_handle_t g_events_handle;

extern SwitchOrch *gSwitchOrch;
extern PortsOrch *gPortsOrch;

static const map<string, sai_packet_action_t> packet_action_map = {
    {"drop", SAI_PACKET_ACTION_DROP},
    {"forward", SAI_PACKET_ACTION_FORWARD},
    {"alert", SAI_PACKET_ACTION_FORWARD}
};

PfcWdBaseOrch::PfcWdBaseOrch(DBConnector *db, vector<string> &tableNames):
    Orch(db, tableNames),
    m_countersDb(new DBConnector("COUNTERS_DB", 0)),
    m_countersTable(new Table(m_countersDb.get(), COUNTERS_TABLE)),
    m_stateDb(new DBConnector("STATE_DB", 0)),
    m_stateTable(new Table(m_stateDb.get(), PFC_WD_STATE_TABLE)),
    m_queueStateTable(new Table(m_stateDb.get(), PFC_WD_QUEUE_STATE_TABLE)),
    m_stormHistoryTable(new Table(m_stateDb.get(), PFC_WD_STORM_HISTORY_TABLE)),
    m_platform(getenv("platform") ? getenv("platform") : "")
{
    SWSS_LOG_ENTER();

    if (!WarmStart::isWarmStart())
    {
        closeStormsOnColdStart();
    }

    if (m_platform == "")
    {
        SWSS_LOG_ERROR("Platform environment variable is not defined");
        return;
    }

}


PfcWdBaseOrch::~PfcWdBaseOrch(void)
{
    SWSS_LOG_ENTER();
}

bool PfcWdBaseOrch::getTimerRange(PfcWdTimerRange& range) const
{
    SWSS_LOG_ENTER();

    range.detectionMin = PFC_WD_DETECTION_TIME_MIN;
    range.detectionMax = PFC_WD_DETECTION_TIME_MAX;
    range.restorationMin = PFC_WD_RESTORATION_TIME_MIN;
    range.restorationMax = PFC_WD_RESTORATION_TIME_MAX;

    return true;
}

task_process_status PfcWdBaseOrch::handleStartWdOnPortFailure(const Port& port)
{
    SWSS_LOG_ENTER();

    // A port with no lossless TCs is not ready for the PFC watchdog yet, so the entry stays pending and starts once the port becomes PFC-ready.
    set<uint8_t> losslessTc;
    if (!getLosslessTcsForPort(port, losslessTc))
    {
        if (m_pfcwdPendingPorts.insert(port.m_alias).second)
        {
            SWSS_LOG_INFO("Deferring PFC Watchdog on port %s: no lossless TC yet", port.m_alias.c_str());
        }
        return task_process_status::task_need_retry;
    }

    // The port is PFC ready, so the failure is in the programming itself. The
    // entry is kept for retry: doTask() has no case for task_failed and would
    // drop the configuration outright, leaving CONFIG_DB and the ASIC out of sync.
    SWSS_LOG_ERROR("Failed to start PFC Watchdog on port %s", port.m_alias.c_str());
    return task_process_status::task_need_retry;
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
        SWSS_LOG_NOTICE("No lossless TC found on port %s", port.m_alias.c_str());
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

    PfcWdTimerRange timerRange;
    if (!getTimerRange(timerRange))
    {
        SWSS_LOG_WARN("PFC Watchdog timer range is not available yet for port %s, retry it", key.c_str());
        return task_process_status::task_need_retry;
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
                        timerRange.detectionMin,
                        timerRange.detectionMax);
            }
            else if (field == PFC_WD_RESTORATION_TIME)
            {
                restorationTime = to_uint<uint32_t>(value,
                        timerRange.restorationMin,
                        timerRange.restorationMax);
            }
            else if (field == PFC_WD_ACTION)
            {
                action = deserializeAction(value);
                if (action == PfcWdAction::PFC_WD_ACTION_UNKNOWN)
                {
                    SWSS_LOG_ERROR("Invalid PFC Watchdog action %s", value.c_str());
                    return task_process_status::task_invalid_entry;
                }
                if (isCiscoPlatform(m_platform) && (action == PfcWdAction::PFC_WD_ACTION_FORWARD)) {
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

    // GLOBAL is a valid config key (handled specially in createEntry), not a port.
    // Deleting PFC_WD|GLOBAL is a legitimate no-op for the per-port teardown path,
    // so short-circuit it here before the port lookup. This both avoids the
    // null-deref crash and prevents a spurious "Invalid port interface" error on
    // routine GLOBAL removal.
    if (name == PFC_WD_GLOBAL)
    {
        return task_process_status::task_success;
    }

    Port port;
    if (!gPortsOrch->getPort(name, port))
    {
        SWSS_LOG_ERROR("Invalid port interface %s", name.c_str());
        return task_process_status::task_invalid_entry;
    }

    if (!stopWdOnPort(port))
    {
        SWSS_LOG_ERROR("Failed to stop PFC Watchdog on port %s", name.c_str());
        return task_process_status::task_failed;
    }

    SWSS_LOG_NOTICE("Stopped PFC Watchdog on port %s", name.c_str());
    // Only a config removal clears storms; createEntry's internal restart keeps them.
    clearStormLog(port.m_alias);
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

    recordStormDetected(portAlias, queueIndex);
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

    recordStormRestored(portAlias, queueIndex, queueId);
}

// A malformed value reads as zero instead of throwing out of the watchdog
// timer.
static uint64_t parseQueueStat(const string& field, const string& value)
{
    try
    {
        return stoull(value);
    }
    catch (const logic_error&)
    {
        SWSS_LOG_ERROR("Bad PFC WD counter value %s=%s", field.c_str(), value.c_str());
        return 0;
    }
}

bool PfcWdBaseOrch::readQueueSaiStats(sai_object_id_t queueId, PfcWdHwStats& counters)
{
    SWSS_LOG_ENTER();

    // Zeroed up front so a failed read leaves no stale value behind.
    counters = {};

    static const vector<sai_stat_id_t> queueStatIds =
    {
        SAI_QUEUE_STAT_PACKETS,
        SAI_QUEUE_STAT_DROPPED_PACKETS,
    };
    vector<uint64_t> queueStats(queueStatIds.size());

    sai_status_t status = sai_queue_api->get_queue_stats(
            queueId, static_cast<uint32_t>(queueStatIds.size()), queueStatIds.data(), queueStats.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to fetch queue 0x%" PRIx64 " stats: %d", queueId, status);
        return false;
    }

    counters.txPkt = queueStats[0];
    counters.txDropPkt = queueStats[1];

    return true;
}

PfcWdQueueStats PfcWdBaseOrch::getQueueStats(const shared_ptr<Table>& countersTable,
                                             const string& queueIdStr)
{
    SWSS_LOG_ENTER();

    PfcWdQueueStats stats;
    memset(&stats, 0, sizeof(PfcWdQueueStats));
    stats.operational = true;
    vector<FieldValueTuple> fieldValues;

    if (!countersTable->get(queueIdStr, fieldValues))
    {
        return stats;
    }

    for (const auto& fv : fieldValues)
    {
        const auto field = fvField(fv);
        const auto value = fvValue(fv);

        if (field == PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED)
        {
            stats.detectCount = parseQueueStat(field, value);
        }
        else if (field == PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED)
        {
            stats.restoreCount = parseQueueStat(field, value);
        }
        else if (field == PFC_WD_QUEUE_STATUS)
        {
            stats.operational = value == PFC_WD_QUEUE_STATUS_OPERATIONAL ? true : false;
        }
        else if (field == PFC_WD_QUEUE_STATS_TX_PACKETS)
        {
            stats.txPkt = parseQueueStat(field, value);
        }
        else if (field == PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS)
        {
            stats.txDropPkt = parseQueueStat(field, value);
        }
        else if (field == PFC_WD_QUEUE_STATS_RX_PACKETS)
        {
            stats.rxPkt = parseQueueStat(field, value);
        }
        else if (field == PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS)
        {
            stats.rxDropPkt = parseQueueStat(field, value);
        }
        else if (field == PFC_WD_QUEUE_STATS_TX_PACKETS_LAST)
        {
            stats.txPktLast = parseQueueStat(field, value);
        }
        else if (field == PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS_LAST)
        {
            stats.txDropPktLast = parseQueueStat(field, value);
        }
        else if (field == PFC_WD_QUEUE_STATS_RX_PACKETS_LAST)
        {
            stats.rxPktLast = parseQueueStat(field, value);
        }
        else if (field == PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS_LAST)
        {
            stats.rxDropPktLast = parseQueueStat(field, value);
        }
    }

    return stats;
}

void PfcWdBaseOrch::updateQueueStats(const shared_ptr<Table>& countersTable,
                                     const string& queueIdStr,
                                     const PfcWdQueueStats& stats)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> resultFvValues;

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED, to_string(stats.detectCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED, to_string(stats.restoreCount));

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_TX_PACKETS, to_string(stats.txPkt));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS, to_string(stats.txDropPkt));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_RX_PACKETS, to_string(stats.rxPkt));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS, to_string(stats.rxDropPkt));

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_TX_PACKETS_LAST, to_string(stats.txPktLast));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS_LAST, to_string(stats.txDropPktLast));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_RX_PACKETS_LAST, to_string(stats.rxPktLast));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS_LAST, to_string(stats.rxDropPktLast));

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATUS, stats.operational ?
                                                     PFC_WD_QUEUE_STATUS_OPERATIONAL :
                                                     PFC_WD_QUEUE_STATUS_STORMED);

    countersTable->set(queueIdStr, resultFvValues);
}

static bool parseDecimal(const string& text, uint64_t& number)
{
    if (text.empty() || text.size() > 19 || text.find_first_not_of("0123456789") != string::npos)
    {
        return false;
    }
    number = stoull(text);
    return true;
}

static string stormKey(const string& portAlias, uint8_t queueIndex)
{
    return portAlias + "|" + to_string(static_cast<unsigned>(queueIndex));
}

static string stormField(uint64_t number, const char* suffix)
{
    return "storm_" + to_string(number) + "_" + suffix;
}

static uint64_t nowMicroseconds()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000ULL + static_cast<uint64_t>(tv.tv_usec);
}

// Storm last_storm is detected and neither restored nor interrupted.
static bool openStorm(const shared_ptr<Table>& history, const string& key,
                      uint64_t& number, string& detected)
{
    string last, ignored;
    return history->hget(key, PFC_WD_STORM_HISTORY_LAST_STORM, last) &&
           parseDecimal(last, number) &&
           history->hget(key, stormField(number, PFC_WD_STORM_HISTORY_DETECTED), detected) &&
           !history->hget(key, stormField(number, PFC_WD_STORM_HISTORY_RESTORED), ignored) &&
           !history->hget(key, stormField(number, PFC_WD_STORM_HISTORY_INTERRUPTED), ignored);
}

static uint64_t storedCount(const shared_ptr<Table>& state, const string& key, const char* field)
{
    string text;
    uint64_t count = 0;
    if (!state->hget(key, field, text) || !parseDecimal(text, count))
    {
        return 0;
    }
    return count;
}

void PfcWdBaseOrch::clearStormLog(const string& portAlias)
{
    SWSS_LOG_ENTER();

    try
    {
        for (uint8_t i = 0; i < PFC_WD_TC_MAX; i++)
        {
            m_queueStateTable->del(stormKey(portAlias, i));
            m_stormHistoryTable->del(stormKey(portAlias, i));
        }
    }
    catch (const std::exception& e)
    {
        SWSS_LOG_WARN("Failed to clear PFC watchdog storm records for %s: %s",
                      portAlias.c_str(), e.what());
    }
}

void PfcWdBaseOrch::closeStormsOnColdStart()
{
    SWSS_LOG_ENTER();

    try
    {
        vector<string> keys;
        m_queueStateTable->getKeys(keys);
        for (const auto& key : keys)
        {
            m_queueStateTable->del(key);
        }

        const string now = to_string(nowMicroseconds());
        keys.clear();
        m_stormHistoryTable->getKeys(keys);
        for (const auto& key : keys)
        {
            uint64_t n = 0;
            string detected;
            if (openStorm(m_stormHistoryTable, key, n, detected))
            {
                m_stormHistoryTable->hset(key, stormField(n, PFC_WD_STORM_HISTORY_INTERRUPTED), now);
            }
        }
    }
    catch (const std::exception& e)
    {
        SWSS_LOG_WARN("Failed to close PFC watchdog storm records on cold start: %s", e.what());
    }
}

void PfcWdBaseOrch::interruptStorm(const string& portAlias, uint8_t queueIndex)
{
    SWSS_LOG_ENTER();

    const string key = stormKey(portAlias, queueIndex);
    try
    {
        uint64_t n = 0;
        string detected;
        if (openStorm(m_stormHistoryTable, key, n, detected))
        {
            m_stormHistoryTable->hset(key, stormField(n, PFC_WD_STORM_HISTORY_INTERRUPTED),
                                      to_string(nowMicroseconds()));
        }
    }
    catch (const std::exception& e)
    {
        SWSS_LOG_WARN("Failed to interrupt PFC watchdog storm record for %s: %s",
                      key.c_str(), e.what());
    }
}

void PfcWdBaseOrch::recordStormDetected(const string& portAlias, uint8_t queueIndex)
{
    SWSS_LOG_ENTER();

    const string key = stormKey(portAlias, queueIndex);
    try
    {
        uint64_t n = 0;
        string detected;
        if (openStorm(m_stormHistoryTable, key, n, detected))
        {
            // Still open, e.g. replayed after a warm restart: same storm.
            return;
        }

        const string now = to_string(nowMicroseconds());

        m_queueStateTable->set(key, {
            {PFC_WD_QUEUE_STATE_STATUS, PFC_WD_QUEUE_STATE_STATUS_DETECTED},
            {PFC_WD_QUEUE_STATE_DETECTION_COUNT,
             to_string(storedCount(m_queueStateTable, key, PFC_WD_QUEUE_STATE_DETECTION_COUNT) + 1)},
            {PFC_WD_QUEUE_STATE_RESTORATION_COUNT,
             to_string(storedCount(m_queueStateTable, key, PFC_WD_QUEUE_STATE_RESTORATION_COUNT))},
            {PFC_WD_QUEUE_STATE_LAST_DETECTION_TIME, now},
        });

        string last;
        if (!m_stormHistoryTable->hget(key, PFC_WD_STORM_HISTORY_LAST_STORM, last) ||
            !parseDecimal(last, n))
        {
            n = 0;
        }
        n++;

        m_stormHistoryTable->set(key, {
            {PFC_WD_STORM_HISTORY_LAST_STORM, to_string(n)},
            {stormField(n, PFC_WD_STORM_HISTORY_DETECTED), now},
        });

        if (n > PFC_WD_STORM_HISTORY_DEPTH)
        {
            const uint64_t old = n - PFC_WD_STORM_HISTORY_DEPTH;
            for (const char* suffix : {PFC_WD_STORM_HISTORY_DETECTED, PFC_WD_STORM_HISTORY_RESTORED,
                                       PFC_WD_STORM_HISTORY_DURATION_MS, PFC_WD_STORM_HISTORY_TX_PACKETS,
                                       PFC_WD_STORM_HISTORY_TX_DROPPED, PFC_WD_STORM_HISTORY_RX_PACKETS,
                                       PFC_WD_STORM_HISTORY_RX_DROPPED, PFC_WD_STORM_HISTORY_INTERRUPTED})
            {
                m_stormHistoryTable->hdel(key, stormField(old, suffix));
            }
        }
    }
    catch (const std::exception& e)
    {
        SWSS_LOG_WARN("Failed to record PFC watchdog storm detection for %s: %s",
                      key.c_str(), e.what());
    }
}

void PfcWdBaseOrch::recordStormRestored(const string& portAlias, uint8_t queueIndex,
                                        sai_object_id_t queueId)
{
    SWSS_LOG_ENTER();

    const string key = stormKey(portAlias, queueIndex);
    try
    {
        const uint64_t nowUs = nowMicroseconds();
        const string now = to_string(nowUs);

        vector<FieldValueTuple> state = {
            {PFC_WD_QUEUE_STATE_STATUS, PFC_WD_QUEUE_STATE_STATUS_RESTORED},
            {PFC_WD_QUEUE_STATE_RESTORATION_COUNT,
             to_string(storedCount(m_queueStateTable, key, PFC_WD_QUEUE_STATE_RESTORATION_COUNT) + 1)},
            {PFC_WD_QUEUE_STATE_LAST_RESTORATION_TIME, now},
        };

        uint64_t n = 0;
        string detected;
        if (openStorm(m_stormHistoryTable, key, n, detected))
        {
            uint64_t detectUs = 0;
            const string durationMs = to_string((parseDecimal(detected, detectUs) && nowUs >= detectUs)
                                                ? (nowUs - detectUs) / 1000 : 0);
            const auto stats = getQueueStats(m_countersTable, sai_serialize_object_id(queueId));

            m_stormHistoryTable->set(key, {
                {stormField(n, PFC_WD_STORM_HISTORY_RESTORED), now},
                {stormField(n, PFC_WD_STORM_HISTORY_DURATION_MS), durationMs},
                {stormField(n, PFC_WD_STORM_HISTORY_TX_PACKETS), to_string(stats.txPktLast)},
                {stormField(n, PFC_WD_STORM_HISTORY_TX_DROPPED), to_string(stats.txDropPktLast)},
                {stormField(n, PFC_WD_STORM_HISTORY_RX_PACKETS), to_string(stats.rxPktLast)},
                {stormField(n, PFC_WD_STORM_HISTORY_RX_DROPPED), to_string(stats.rxDropPktLast)},
            });
            state.emplace_back(PFC_WD_QUEUE_STATE_STORM_DURATION_MS, durationMs);
        }

        m_queueStateTable->set(key, state);
    }
    catch (const std::exception& e)
    {
        SWSS_LOG_WARN("Failed to record PFC watchdog storm restoration for %s: %s",
                      key.c_str(), e.what());
    }
}

void PfcWdBaseOrch::beginStormCounters(const shared_ptr<Table>& countersTable,
                                       const string& queueIdStr,
                                       PfcWdHwStats hwStats,
                                       PfcWdHwStats& baseline)
{
    SWSS_LOG_ENTER();

    auto stats = getQueueStats(countersTable, queueIdStr);

    // Only a genuinely new storm bumps the detect count. If a storm was already
    // in progress we are resuming it, so the counters carry over untouched.
    if (!(stats.detectCount > stats.restoreCount))
    {
        stats.detectCount++;

        stats.txPktLast = 0;
        stats.txDropPktLast = 0;
        stats.rxPktLast = 0;
        stats.rxDropPktLast = 0;
    }
    stats.operational = false;

    baseline = hwStats;

    updateQueueStats(countersTable, queueIdStr, stats);
}

void PfcWdBaseOrch::accumulateStormCounters(const shared_ptr<Table>& countersTable,
                                            const string& queueIdStr,
                                            PfcWdHwStats hwStats,
                                            PfcWdHwStats& baseline,
                                            bool periodic,
                                            bool baselineValid)
{
    SWSS_LOG_ENTER();

    auto stats = getQueueStats(countersTable, queueIdStr);

    if (!periodic)
    {
        stats.restoreCount++;
    }
    stats.operational = !periodic;

    if (baselineValid)
    {
        // A backward jump is read as a counter reset rather than wrapping the
        // subtraction: the storm gets what the counter has reached since.
        auto delta = [&](uint64_t cur, uint64_t base, const char *name) -> uint64_t
        {
            if (cur >= base)
            {
                return cur - base;
            }

            SWSS_LOG_WARN("PFC WD counter reset on queue %s: %s went from %" PRIu64 " to %" PRIu64,
                          queueIdStr.c_str(), name, base, cur);
            return cur;
        };

        const uint64_t dTxPkt     = delta(hwStats.txPkt,     baseline.txPkt,     "txPkt");
        const uint64_t dTxDropPkt = delta(hwStats.txDropPkt, baseline.txDropPkt, "txDropPkt");
        const uint64_t dRxPkt     = delta(hwStats.rxPkt,     baseline.rxPkt,     "rxPkt");
        const uint64_t dRxDropPkt = delta(hwStats.rxDropPkt, baseline.rxDropPkt, "rxDropPkt");

        stats.txPktLast     += dTxPkt;
        stats.txDropPktLast += dTxDropPkt;
        stats.rxPktLast     += dRxPkt;
        stats.rxDropPktLast += dRxDropPkt;

        stats.txPkt     += dTxPkt;
        stats.txDropPkt += dTxDropPkt;
        stats.rxPkt     += dRxPkt;
        stats.rxDropPkt += dRxDropPkt;
    }

    baseline = hwStats;

    updateQueueStats(countersTable, queueIdStr, stats);
}
