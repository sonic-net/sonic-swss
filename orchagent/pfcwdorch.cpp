#include "pfcwdorch.h"
#include "saiserialize.h"
#include "portsorch.h"
#include "converter.h"
#include "redisapi.h"

#define PFC_WD_ACTION                   "action"
#define PFC_WD_DETECTION_TIME           "detection_time"
#define PFC_WD_RESTORATION_TIME         "restoration_time"

#define PFC_WD_QUEUE_STATUS             "PFC_WD_STATUS"
#define PFC_WD_QUEUE_STATUS_OPERATIONAL "operational"
#define PFC_WD_QUEUE_STATUS_STORMED     "stormed"

#define PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED "PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED"
#define PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED "PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED"

#define PFC_WD_DETECTION_TIME_MAX       (5 * 1000)
#define PFC_WD_DETECTION_TIME_MIN       100
#define PFC_WD_RESTORATION_TIME_MAX     (60 * 1000)
#define PFC_WD_RESTORATION_TIME_MIN     100
#define PFC_WD_TC_MAX                   8

extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;
extern PortsOrch *gPortsOrch;

PfcWdOrch::PfcWdOrch(DBConnector *db, vector<string> &tableNames):
    Orch(db, tableNames),
    m_pfcWdDb(PFC_WD_DB, DBConnector::DEFAULT_UNIXSOCKET, 0),
    m_countersDb(COUNTERS_DB, DBConnector::DEFAULT_UNIXSOCKET, 0),
    m_pfcWdTable(&m_pfcWdDb, PFC_WD_STATE_TABLE),
    m_countersTable(&m_countersDb, COUNTERS_TABLE)
{
    SWSS_LOG_ENTER();
}

PfcWdOrch::~PfcWdOrch(void)
{
    SWSS_LOG_ENTER();
}

void PfcWdOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            createEntry(key, kfvFieldsValues(t));
        }
        else if (op == DEL_COMMAND)
        {
            deleteEntry(key);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
        }

        consumer.m_toSync.erase(it++);
    }
}

void PfcWdOrch::updateWdCounters(const string& queueIdStr, bool operational)
{
    uint32_t detectCount = 0;
    uint32_t restoreCount = 0;
    vector<FieldValueTuple> resultFvValues;

    getWdCounters(queueIdStr, detectCount, restoreCount);

    if (operational)
    {
        restoreCount++;
    }
    else
    {
        detectCount++;
    }

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED, to_string(detectCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED, to_string(restoreCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATUS, operational ?
                                                     PFC_WD_QUEUE_STATUS_OPERATIONAL :
                                                     PFC_WD_QUEUE_STATUS_STORMED);

    m_countersTable.set(queueIdStr, resultFvValues);
}

void PfcWdOrch::initWdCounters(const string &queueIdStr)
{
    uint32_t detectCount = 0;
    uint32_t restoreCount = 0;
    vector<FieldValueTuple> resultFvValues;

    getWdCounters(queueIdStr, detectCount, restoreCount);

    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED, to_string(detectCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED, to_string(restoreCount));
    resultFvValues.emplace_back(PFC_WD_QUEUE_STATUS, PFC_WD_QUEUE_STATUS_OPERATIONAL);

    m_countersTable.set(queueIdStr, resultFvValues);
}

void PfcWdOrch::getWdCounters(const string& queueIdStr, uint32_t& detectCount, uint32_t& restoreCount)
{
    vector<FieldValueTuple> fieldValues;
    detectCount = 0;
    restoreCount = 0;

    if (!m_countersTable.get(queueIdStr, fieldValues))
    {
        return;
    }

    for (const auto& fv : fieldValues)
    {
        const auto field = fvField(fv);
        const auto value = fvValue(fv);

        if (field == PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED)
        {
            detectCount = stoul(value);
        }
        else if (field == PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED)
        {
            restoreCount = stoul(value);
        }
    }
}

PfcWdOrch::PfcWdAction PfcWdOrch::deserializeAction(const string& key)
{
    SWSS_LOG_ENTER();

    const map<string, PfcWdAction> actionMap =
    {
        { "forward", PfcWdAction::PFC_WD_ACTION_FORWARD },
        { "drop", PfcWdAction::PFC_WD_ACTION_DROP },
    };

    if (actionMap.find(key) == actionMap.end())
    {
        return PfcWdAction::PFC_WD_ACTION_UNKNOWN;
    }

    return actionMap.at(key);
}

void PfcWdOrch::createEntry(const string& key, const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();

    uint32_t detectionTime = 0;
    uint32_t restorationTime = 0;
    // According to requirements, drop action is default
    PfcWdAction action = PfcWdAction::PFC_WD_ACTION_DROP;

    Port port;
    if (!gPortsOrch->getPort(key, port))
    {
        SWSS_LOG_ERROR("Invalid port interface %s", key.c_str());
        return;
    }

    if (port.m_type != Port::PHY)
    {
        SWSS_LOG_ERROR("Interface %s is not physical port", key.c_str());
        return;
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
                    return;
                }
            }
            else
            {
                SWSS_LOG_ERROR(
                        "Failed to parse PFC Watchdog %s configuration. Unknown attribute %s.\n",
                        key.c_str(),
                        field.c_str());
                return;
            }
        }
        catch (const exception& e)
        {
            SWSS_LOG_ERROR(
                    "Failed to parse PFC Watchdog %s attribute %s error: %s.",
                    key.c_str(),
                    field.c_str(),
                    e.what());
            return;
        }
        catch (...)
        {
            SWSS_LOG_ERROR(
                    "Failed to parse PFC Watchdog %s attribute %s. Unknown error has been occurred",
                    key.c_str(),
                    field.c_str());
            return;
        }
    }

    // Validation
    if (detectionTime == 0)
    {
        SWSS_LOG_ERROR("%s missing", PFC_WD_DETECTION_TIME);
        return;
    }
    
    if (restorationTime == 0)
    {
        SWSS_LOG_ERROR("%s missing", PFC_WD_RESTORATION_TIME);
        return;
    }

    // Start Watchdog on every lossless queue
    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL;

    sai_status_t status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get PFC mask on port %s: %d", port.m_alias.c_str(), status);
        return;
    }

    uint8_t pfcMask = attr.value.u8;
    for (uint8_t i = 0; i < PFC_WD_TC_MAX; i++)
    {
        if ((pfcMask & (1 << i)) == 0)
        {
            continue;
        }

        sai_object_id_t queueId = port.m_queue_ids[i];

        initWdCounters(sai_serialize_object_id(queueId));

        if (!startWd(queueId, port.m_port_id, detectionTime, restorationTime, action))
        {
            SWSS_LOG_ERROR("Failed to start PFC Watchdog on port %s queue %d", key.c_str(), i);
            continue;
        }

        SWSS_LOG_NOTICE("Starting PFC Watchdog on port %s queue %d", key.c_str(), i);
    }

}

void PfcWdOrch::deleteEntry(const string& name)
{
    SWSS_LOG_ENTER();

    Port port;
    gPortsOrch->getPort(name, port);

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL;

    sai_status_t status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get PFC mask on port %s: %d", port.m_alias.c_str(), status);
        return;
    }

    uint8_t pfcMask = attr.value.u8;
    for (uint8_t i = 0; i < PFC_WD_TC_MAX; i++)
    {
        if ((pfcMask & (1 << i)) == 0)
        {
            continue;
        }

        sai_object_id_t queueId = port.m_queue_ids[i];

        if (!stopWd(queueId))
        {
            SWSS_LOG_ERROR("Failed to stop PFC Watchdog on port %s queue %d", name.c_str(), i);
            continue;
        }

        SWSS_LOG_NOTICE("Stopped PFC Watchdog on port %s queue %d", name.c_str(), i);
    }
}

PfcWdSwOrch::PfcWdSwOrch(DBConnector *db, vector<string> &tableNames):
    PfcWdOrch(db, tableNames)
{
    SWSS_LOG_ENTER();
}

PfcWdSwOrch::~PfcWdSwOrch(void)
{
    SWSS_LOG_ENTER();
}

PfcWdSwOrch::PfcWdQueueEntry::PfcWdQueueEntry(uint32_t detectionTime, uint32_t restorationTime,
        PfcWdAction action, sai_object_id_t port):
    c_detectionTime(detectionTime),
    c_restorationTime(restorationTime),
    c_action(action),
    pollTimeLeft(c_detectionTime),
    portId(port)
{
    SWSS_LOG_ENTER();
}

bool PfcWdSwOrch::startWd(sai_object_id_t queueId, sai_object_id_t portId,
        uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action)
{
    SWSS_LOG_ENTER();

    {
        unique_lock<mutex> lk(m_pfcWdMutex);

        if (!addToWatchdogDb(queueId, portId, detectionTime, restorationTime, action))
        {
            return false;
        }
    }

    if (!m_runPfcWdSwOrchThread.load())
    {
        startWatchdogThread();
    }

    return true;
}

bool PfcWdSwOrch::stopWd(sai_object_id_t queueId)
{
    SWSS_LOG_ENTER();

    {
        unique_lock<mutex> lk(m_pfcWdMutex);

        removeFromWatchdogDb(queueId);
    }

    if (m_entryMap.empty())
    {
        endWatchdogThread();
    }

    return true;
}

template <typename T>
string PfcWdSwOrch::counterIdsToStr(const vector<T> ids, string (*convert)(T))
{
    SWSS_LOG_ENTER();

    string str;

    for (const auto& i: ids)
    {
        str += convert(i) + ",";
    }

    // Remove trailing ','
    if (!str.empty())
    {
        str.pop_back();
    }

    return str;
}

bool PfcWdSwOrch::addToWatchdogDb(sai_object_id_t queueId, sai_object_id_t portId,
        uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action)
{
    SWSS_LOG_ENTER();

    // We register our queues in PFC_WD table so that syncd will know that it must poll them
    vector<FieldValueTuple> fieldValues;

    if (m_entryMap.find(queueId) != m_entryMap.end())
    {
        SWSS_LOG_ERROR("PFC Watchdog already running on queue 0x%lx", queueId);
        return false;
    }

    const auto& portCounterIds = getPortCounterIds(queueId);
    if (!portCounterIds.empty())
    {
        string str = counterIdsToStr(portCounterIds, &sai_serialize_port_stat);
        fieldValues.emplace_back(PFC_WD_PORT_COUNTER_ID_LIST, str);
    }

    const auto& queueCounterIds = getQueueCounterIds(queueId);
    if (!queueCounterIds.empty())
    {
        string str = counterIdsToStr(queueCounterIds, sai_serialize_queue_stat);
        fieldValues.emplace_back(PFC_WD_QUEUE_COUNTER_ID_LIST, str);
    }

    m_entryMap.emplace(queueId, PfcWdQueueEntry(detectionTime,
                restorationTime,
                action,
                portId));

    string queueIdStr = sai_serialize_object_id(queueId);
    getPfcWdTable().set(queueIdStr, fieldValues);

    return true;
}

bool PfcWdSwOrch::removeFromWatchdogDb(sai_object_id_t queueId)
{
    SWSS_LOG_ENTER();

    // Remove from internal DB
    m_entryMap.erase(queueId);
    // Unregister in syncd
    getPfcWdTable().del(sai_serialize_object_id(queueId));

    return true;
}

uint32_t PfcWdSwOrch::getNearestPollTime(void)
{
    SWSS_LOG_ENTER();

    uint32_t nearestTime = 0;

    for (const auto& queueKv : m_entryMap)
    {
        const PfcWdQueueEntry& queueEntry = queueKv.second;

        // First check regular polling intervals
        if (nearestTime == 0 || queueEntry.pollTimeLeft < nearestTime)
        {
            nearestTime = queueEntry.pollTimeLeft;
        }
    }

    return nearestTime;
}

void PfcWdSwOrch::pollQueues(uint32_t nearestTime, DBConnector& db,
        string detectSha, string restoreSha)
{
    SWSS_LOG_ENTER();

    unique_lock<mutex> lk(m_pfcWdMutex);

    // Select those queues for which timer expired
    vector<string> normalQueues;
    vector<string> stormedQueues;
    for (const auto& queueKv : m_entryMap)
    {
        sai_object_id_t queueId = queueKv.first;
        const PfcWdQueueEntry& queueEntry = queueKv.second;

        if (queueEntry.pollTimeLeft == nearestTime)
        {
            // Queue is being stormed
            if (queueEntry.handler != nullptr)
            {
                stormedQueues.push_back(sai_serialize_object_id(queueId));
            }
            // Queue is not stormed
            else
            {
                normalQueues.push_back(sai_serialize_object_id(queueId));
            }
        }
    }

    // Run scripts for selected queues to see if their state changed
    // from normal to stormed and vice versa
    set<string> stormCheckReply;
    set<string> restoreCheckReply;
    static const vector<string> argv =
    {
        to_string(COUNTERS_DB),
        COUNTERS_TABLE
    };

    if (!normalQueues.empty())
    {
        stormCheckReply = runRedisScript(db, detectSha, normalQueues, argv);
    }

    if (!stormedQueues.empty())
    {
        restoreCheckReply = runRedisScript(db, restoreSha, stormedQueues, argv);
    }

    // Update internal state of queues and their time
    for (auto& queueKv : m_entryMap)
    {
        sai_object_id_t queueId = queueKv.first;
        PfcWdQueueEntry& queueEntry = queueKv.second;

        string queueIdStr = sai_serialize_object_id(queueId);

        // Queue became stormed
        if (stormCheckReply.find(queueIdStr) != stormCheckReply.end())
        {
            if (queueEntry.c_action == PfcWdAction::PFC_WD_ACTION_DROP)
            {
                queueEntry.handler = createDropHandler(
                        queueEntry.portId,
                        queueId,
                        queueEntry.index);
            }
            else if (queueEntry.c_action == PfcWdAction::PFC_WD_ACTION_FORWARD)
            {
                queueEntry.handler = createForwardHandler(
                        queueEntry.portId,
                        queueId,
                        queueEntry.index);
            }
            else
            {
                throw runtime_error("Invalid PFC WD Action");
            }

            updateWdCounters(queueIdStr, false);
            queueEntry.pollTimeLeft = queueEntry.c_restorationTime;
        }
        // Queue is restored
        else if (restoreCheckReply.find(queueIdStr) != restoreCheckReply.end())
        {
            queueEntry.handler = nullptr;
            queueEntry.pollTimeLeft = queueEntry.c_detectionTime;
            updateWdCounters(queueIdStr, true);
        }
        // Update queue poll timer
        else
        {
            queueEntry.pollTimeLeft = queueEntry.pollTimeLeft == nearestTime ?
                (queueEntry.handler == nullptr ?
                 queueEntry.c_detectionTime :
                 queueEntry.c_restorationTime) :
                queueEntry.pollTimeLeft - nearestTime;
        }
    }
}

void PfcWdSwOrch::pfcWatchdogThread(void)
{
    DBConnector db(COUNTERS_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);

    // Load script for storm detection
    string detectScriptName = getStormDetectionCriteria();
    string detectLuaScript = loadLuaScript(detectScriptName);
    string detectSha = loadRedisScript(&db, detectLuaScript);

    // Load script for restoration check
    string restoreLuaScript = loadLuaScript("pfc_restore_check.lua");
    string restoreSha = loadRedisScript(&db, restoreLuaScript);

    while(m_runPfcWdSwOrchThread)
    {
        unique_lock<mutex> lk(m_mtxSleep);

        uint32_t sleepTime = getNearestPollTime();

        m_cvSleep.wait_for(lk, chrono::milliseconds(sleepTime));

        pollQueues(sleepTime, db, detectSha, restoreSha);
    }
}

void PfcWdSwOrch::startWatchdogThread(void)
{
    SWSS_LOG_ENTER();

    if (m_runPfcWdSwOrchThread.load())
    {
        return;
    }

    m_runPfcWdSwOrchThread = true;

    m_pfcWatchdogThread = shared_ptr<thread>(
            new thread(&PfcWdSwOrch::pfcWatchdogThread,
            this));

    SWSS_LOG_INFO("PFC Watchdog thread started");
}

void PfcWdSwOrch::endWatchdogThread(void)
{
    SWSS_LOG_ENTER();

    if (!m_runPfcWdSwOrchThread.load())
    {
        return;
    }

    m_runPfcWdSwOrchThread = false;

    m_cvSleep.notify_all();

    if (m_pfcWatchdogThread != nullptr)
    {
        SWSS_LOG_INFO("Wait for PFC Watchdog thread to end");

        m_pfcWatchdogThread->join();
    }

    SWSS_LOG_INFO("PFC Watchdog thread ended");
}

PfcDurationWatchdog::PfcDurationWatchdog(DBConnector *db, vector<string> &tableNames):
    PfcWdSwOrch(db, tableNames)
{
    SWSS_LOG_ENTER();
}

PfcDurationWatchdog::~PfcDurationWatchdog(void)
{
    SWSS_LOG_ENTER();
}

vector<sai_port_stat_t> PfcDurationWatchdog::getPortCounterIds(sai_object_id_t queueId)
{
    SWSS_LOG_ENTER();

    static const vector<sai_port_stat_t> PfcDurationIdMap =
    {
        SAI_PORT_STAT_PFC_0_RX_PAUSE_DURATION,
        SAI_PORT_STAT_PFC_1_RX_PAUSE_DURATION,
        SAI_PORT_STAT_PFC_2_RX_PAUSE_DURATION,
        SAI_PORT_STAT_PFC_3_RX_PAUSE_DURATION,
        SAI_PORT_STAT_PFC_4_RX_PAUSE_DURATION,
        SAI_PORT_STAT_PFC_5_RX_PAUSE_DURATION,
        SAI_PORT_STAT_PFC_6_RX_PAUSE_DURATION,
        SAI_PORT_STAT_PFC_7_RX_PAUSE_DURATION,
    };

    static const vector<sai_port_stat_t> PfcRxPktsIdMap =
    {
        SAI_PORT_STAT_PFC_0_RX_PKTS,
        SAI_PORT_STAT_PFC_1_RX_PKTS,
        SAI_PORT_STAT_PFC_2_RX_PKTS,
        SAI_PORT_STAT_PFC_3_RX_PKTS,
        SAI_PORT_STAT_PFC_4_RX_PKTS,
        SAI_PORT_STAT_PFC_5_RX_PKTS,
        SAI_PORT_STAT_PFC_6_RX_PKTS,
        SAI_PORT_STAT_PFC_7_RX_PKTS,
    };

    sai_attribute_t attr;
    attr.id = SAI_QUEUE_ATTR_INDEX;

    sai_status_t status = sai_queue_api->get_queue_attribute(queueId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get queue index 0x%lx: %d", queueId, status);
        return { };
    }

    size_t index = attr.value.u8;
    vector<sai_port_stat_t> portStatIds =
    {
        PfcDurationIdMap[index],
        PfcRxPktsIdMap[index],
    };

    return move(portStatIds);
}

vector<sai_queue_stat_t> PfcDurationWatchdog::getQueueCounterIds(sai_object_id_t queueId)
{
    SWSS_LOG_ENTER();

    vector<sai_queue_stat_t> queueStatIds =
    {
        SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES,
        SAI_QUEUE_STAT_PACKETS,
    };

    return move(queueStatIds);
}

string PfcDurationWatchdog::getStormDetectionCriteria(void)
{
    SWSS_LOG_ENTER();

    return "duration_criteria.lua";
}

shared_ptr<PfcWdActionHandler> PfcDurationWatchdog::createForwardHandler(sai_object_id_t port,
        sai_object_id_t queue, uint32_t queueId)
{
    return make_shared<PfcWdLossyHandler>(port, queue, queueId);
}

shared_ptr<PfcWdActionHandler> PfcDurationWatchdog::createDropHandler(sai_object_id_t port,
        sai_object_id_t queue, uint32_t queueId)
{
    return make_shared<PfcWdZeroBufferHandler>(port, queue, queueId);
}
