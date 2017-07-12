#include "pfcwdorch.h"
#include "saiserialize.h"
#include "portsorch.h"
#include "converter.h"
#include "redisapi.h"

#define PFC_WD_ACTION                   "action"
#define PFC_WD_DETECTION_TIME           "detection_time"
#define PFC_WD_RESTORATION_TIME         "restoration_time"

#define PFC_WD_QUEUE_STATUS             "status"
#define PFC_WD_QUEUE_STATUS_OPERATIONAL "operational"
#define PFC_WD_QUEUE_STATUS_STORMED     "stormed"

#define PFC_WD_DETECTION_TIME_MAX       (5 * 1000)
#define PFC_WD_DETECTION_TIME_MIN       100
#define PFC_WD_RESTORATION_TIME_MAX     (60 * 1000)
#define PFC_WD_RESTORATION_TIME_MIN     100
#define PFC_WD_TC_MAX                   8

extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;
extern PortsOrch *gPortsOrch;

PfcWdOrch::PfcWdOrch(DBConnector *db, vector<string> &tableNames):
    Orch(db, tableNames)
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

PfcWdOrch::PfcWdAction PfcWdOrch::deserializeAction(const string& key)
{
    SWSS_LOG_ENTER();

    const std::map<std::string, PfcWdAction> actionMap =
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
    PfcWdAction action = PfcWdAction::PFC_WD_ACTION_UNKNOWN;

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

    // According to requirements, drop action is default
    if (action == PfcWdAction::PFC_WD_ACTION_UNKNOWN)
    {
        action = PfcWdAction::PFC_WD_ACTION_DROP;
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

        SWSS_LOG_ERROR("Stopped PFC Watchdog on port %s queue %d", name.c_str(), i);
    }
}

PfcWdSwOrch::PfcWdSwOrch(DBConnector *db, vector<string> &tableNames):
    PfcWdOrch(db, tableNames),
    m_pfcWdDb(PFC_WD_DB, DBConnector::DEFAULT_UNIXSOCKET, 0),
    m_pfcWdTable(&m_pfcWdDb, PFC_WD_STATE_TABLE)
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
        std::unique_lock<std::mutex> lk(m_pfcWdMutex);

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
        std::unique_lock<std::mutex> lk(m_pfcWdMutex);

        removeFromWatchdogDb(queueId);
    }

    if (m_entryMap.empty())
    {
        endWatchdogThread();
    }

    return true;
}

template <typename T>
std::string PfcWdSwOrch::counterIdsToStr(const std::vector<T> ids, std::string (*convert)(T))
{
    SWSS_LOG_ENTER();

    std::string str;

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

std::set<std::string> PfcWdSwOrch::runRedisScript(DBConnector &db, const std::string& sha,
        const std::vector<std::string>& keys, const std::vector<std::string>& argv)
{
    SWSS_LOG_ENTER();

    std::vector<std::string> args;

    // Prepare EVALSHA command
    // Format is following:
    // EVALSHA <sha> <size of KEYS> <KEYS> <ARGV>
    args.push_back("EVALSHA");
    args.push_back(sha);
    args.push_back(std::to_string(keys.size()));
    args.insert(args.end(), keys.begin(), keys.end());
    args.insert(args.end(), argv.begin(), argv.end());
    args.push_back("''");

    // Convert to vector of char *
    std::vector<const char *> c_args;
    transform(
            args.begin(),
            args.end(),
            std::back_inserter(c_args),
            [](const string& s) { return s.c_str(); } );

    RedisCommand command;
    command.formatArgv(static_cast<int>(c_args.size()), c_args.data(), NULL);

    std::set<std::string> ret;
    try
    {
        RedisReply r(&db, command);
        auto ctx = r.getContext();
        SWSS_LOG_DEBUG("Running lua script %s", sha.c_str());

        if (ctx->type == REDIS_REPLY_NIL)
        {
            SWSS_LOG_ERROR("Got EMPTY response type from redis %d", ctx->type);
            return std::move(ret);
        }

        else if (ctx->type != REDIS_REPLY_ARRAY)
        {
            SWSS_LOG_ERROR("Got invalid response type from redis %d", ctx->type);
            return std::move(ret);
        }

        for (size_t i = 0; i < ctx->elements; i++)
        {
            SWSS_LOG_DEBUG("Got element %lu %s", i, ctx->element[i]->str);
            ret.insert(ctx->element[i]->str);
        }
    }
    catch (const exception& e)
    {
        SWSS_LOG_ERROR("Caught exception while running Redis lua script: %s", e.what());
    }
    catch(...)
    {
        SWSS_LOG_ERROR("Caught exception while running Redis lua script");
    }

    return std::move(ret);
}

void PfcWdSwOrch::setQueueDbStatus(const std::string& queueIdStr, bool operational)
{
    std::vector<FieldValueTuple> fieldValues;

    fieldValues.emplace_back(PFC_WD_QUEUE_STATUS, operational ?
                                                  PFC_WD_QUEUE_STATUS_OPERATIONAL :
                                                  PFC_WD_QUEUE_STATUS_STORMED);

    m_pfcWdTable.set(queueIdStr, fieldValues);
}

bool PfcWdSwOrch::addToWatchdogDb(sai_object_id_t queueId, sai_object_id_t portId,
        uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action)
{
    SWSS_LOG_ENTER();

    // We register our queues in PFC_WD table so that syncd will know that it must poll them
    std::vector<FieldValueTuple> fieldValues;

    if (m_entryMap.find(queueId) != m_entryMap.end())
    {
        SWSS_LOG_ERROR("PFC Watchdog already running on queue 0x%lx", queueId);
        return false;
    }

    const auto& portCounterIds = getPortCounterIds(queueId);
    if (!portCounterIds.empty())
    {
        std::string str = counterIdsToStr(portCounterIds, &sai_serialize_port_stat);
        fieldValues.emplace_back(PFC_WD_PORT_COUNTER_ID_LIST, str);
    }

    const auto& queueCounterIds = getQueueCounterIds(queueId);
    if (!queueCounterIds.empty())
    {
        std::string str = counterIdsToStr(queueCounterIds, sai_serialize_queue_stat);
        fieldValues.emplace_back(PFC_WD_QUEUE_COUNTER_ID_LIST, str);
    }

    m_entryMap.emplace(queueId, PfcWdQueueEntry(detectionTime,
                restorationTime,
                action,
                portId));

    fieldValues.emplace_back(PFC_WD_QUEUE_STATUS, PFC_WD_QUEUE_STATUS_OPERATIONAL);
    std::string queueIdStr = sai_serialize_object_id(queueId);
    m_pfcWdTable.set(queueIdStr, fieldValues);

    return true;
}

bool PfcWdSwOrch::removeFromWatchdogDb(sai_object_id_t queueId)
{
    SWSS_LOG_ENTER();

    // Remove from internal DB
    m_entryMap.erase(queueId);
    // Unregister in syncd
    m_pfcWdTable.del(sai_serialize_object_id(queueId));

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
        std::string detectSha, std::string restoreSha)
{
    SWSS_LOG_ENTER();

    std::unique_lock<std::mutex> lk(m_pfcWdMutex);

    SWSS_LOG_ERROR("%s %d", __FUNCTION__, nearestTime);
    // Select those queues for which timer expired
    std::vector<std::string> normalQueues;
    std::vector<std::string> stormedQueues;
    for (const auto& queueKv : m_entryMap)
    {
        sai_object_id_t queueId = queueKv.first;
        const PfcWdQueueEntry& queueEntry = queueKv.second;

        if (queueEntry.pollTimeLeft == nearestTime)
        {
            // Queue is being stormed
            if (queueEntry.pfcQueue != nullptr)
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
    std::set<std::string> stormCheckReply;
    std::set<std::string> restoreCheckReply;
    static const std::vector<std::string> argv =
    {
        std::to_string(COUNTERS_DB),
        "PFC_WD_COUNTERS"
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

        std::string queueIdStr = sai_serialize_object_id(queueId);

        // Queue became stormed
        if (stormCheckReply.find(queueIdStr) != stormCheckReply.end())
        {
            if (queueEntry.c_action == PfcWdAction::PFC_WD_ACTION_DROP)
            {
                queueEntry.pfcQueue = createDropQueue(
                        queueEntry.portId,
                        queueId,
                        queueEntry.index);
            }
            else if (queueEntry.c_action == PfcWdAction::PFC_WD_ACTION_FORWARD)
            {
                queueEntry.pfcQueue = createForwardQueue(
                        queueEntry.portId,
                        queueId,
                        queueEntry.index);
            }
            else
            {
                throw std::runtime_error("Invalid PFC WD Action");
            }

            setQueueDbStatus(queueIdStr, false);
            queueEntry.pollTimeLeft = queueEntry.c_restorationTime;
        }
        // Queue is restored
        else if (restoreCheckReply.find(queueIdStr) != restoreCheckReply.end())
        {
            queueEntry.pfcQueue = nullptr;
            queueEntry.pollTimeLeft = queueEntry.c_detectionTime;
            setQueueDbStatus(queueIdStr, true);
        }
        // Update queue poll timer
        else
        {
            queueEntry.pollTimeLeft = queueEntry.pollTimeLeft == nearestTime ?
                (queueEntry.pfcQueue == nullptr ?
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
    std::string detectScriptName = getStormDetectionCriteria();
    std::string detectLuaScript = loadLuaScript(detectScriptName);
    std::string detectSha = loadRedisScript(&db, detectLuaScript);

    // Load script for restoration check
    std::string restoreLuaScript = loadLuaScript("pfc_restore_check.lua");
    std::string restoreSha = loadRedisScript(&db, restoreLuaScript);

    while(m_runPfcWdSwOrchThread)
    {
        std::unique_lock<std::mutex> lk(m_mtxSleep);

        uint32_t sleepTime = getNearestPollTime();

        m_cvSleep.wait_for(lk, std::chrono::milliseconds(sleepTime));

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

    m_pfcWatchdogThread = std::shared_ptr<std::thread>(
            new std::thread(&PfcWdSwOrch::pfcWatchdogThread,
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

std::vector<sai_port_stat_t> PfcDurationWatchdog::getPortCounterIds(sai_object_id_t queueId)
{
    SWSS_LOG_ENTER();

    static const std::vector<sai_port_stat_t> PfcDurationIdMap =
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

    static const std::vector<sai_port_stat_t> PfcRxPktsIdMap =
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

    sai_attribute_t attr =
    {
        .id = SAI_QUEUE_ATTR_INDEX,
        .value = { },
    };

    sai_status_t status = sai_queue_api->get_queue_attribute(queueId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get queue index 0x%lx: %d", queueId, status);
        return { };
    }

    size_t index = attr.value.u8;
    std::vector<sai_port_stat_t> portStatIds =
    {
        PfcDurationIdMap[index],
        PfcRxPktsIdMap[index],
    };

    return std::move(portStatIds);
}

std::vector<sai_queue_stat_t> PfcDurationWatchdog::getQueueCounterIds(sai_object_id_t queueId)
{
    SWSS_LOG_ENTER();

    std::vector<sai_queue_stat_t> queueStatIds =
    {
        SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES,
        SAI_QUEUE_STAT_PACKETS,
    };

    return std::move(queueStatIds);
}

std::string PfcDurationWatchdog::getStormDetectionCriteria(void)
{
    SWSS_LOG_ENTER();

    return "duration_criteria.lua";
}

std::shared_ptr<PfcQueue> PfcDurationWatchdog::createForwardQueue(sai_object_id_t port,
        sai_object_id_t queue, uint32_t queueId)
{
    return std::make_shared<PfcLossyQueue>(port, queue, queueId);
}

std::shared_ptr<PfcQueue> PfcDurationWatchdog::createDropQueue(sai_object_id_t port,
        sai_object_id_t queue, uint32_t queueId)
{
    return std::make_shared<PfcZeroBufferQueue>(port, queue, queueId);
}
