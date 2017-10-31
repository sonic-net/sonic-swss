#include "pfcwdorch.h"
#include "saiserialize.h"
#include "portsorch.h"
#include "converter.h"
#include "redisapi.h"
#include "select.h"

#define PFC_WD_ACTION                   "action"
#define PFC_WD_DETECTION_TIME           "detection_time"
#define PFC_WD_RESTORATION_TIME         "restoration_time"

#define PFC_WD_DETECTION_TIME_MAX       (5 * 1000)
#define PFC_WD_DETECTION_TIME_MIN       100
#define PFC_WD_RESTORATION_TIME_MAX     (60 * 1000)
#define PFC_WD_RESTORATION_TIME_MIN     100
#define PFC_WD_TC_MAX                   8
#define PFC_WD_POLL_TIMEOUT             5000

extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;
extern PortsOrch *gPortsOrch;

template <typename DropHandler, typename ForwardHandler>
PfcWdOrch<DropHandler, ForwardHandler>::PfcWdOrch(DBConnector *db, vector<string> &tableNames):
    Orch(db, tableNames),
    m_countersDb(new DBConnector(COUNTERS_DB, DBConnector::DEFAULT_UNIXSOCKET, 0)),
    m_countersTable(new Table(m_countersDb.get(), COUNTERS_TABLE))
{
    SWSS_LOG_ENTER();
}

template <typename DropHandler, typename ForwardHandler>
PfcWdOrch<DropHandler, ForwardHandler>::~PfcWdOrch(void)
{
    SWSS_LOG_ENTER();
}

template <typename DropHandler, typename ForwardHandler>
void PfcWdOrch<DropHandler, ForwardHandler>::doTask(Consumer& consumer)
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

template <typename DropHandler, typename ForwardHandler>
template <typename T>
string PfcWdSwOrch<DropHandler, ForwardHandler>::counterIdsToStr(
        const vector<T> ids, string (*convert)(T))
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

template <typename DropHandler, typename ForwardHandler>
PfcWdAction PfcWdOrch<DropHandler, ForwardHandler>::deserializeAction(const string& key)
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

template <typename DropHandler, typename ForwardHandler>
string PfcWdOrch<DropHandler, ForwardHandler>::serializeAction(const PfcWdAction& action)
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


template <typename DropHandler, typename ForwardHandler>
void PfcWdOrch<DropHandler, ForwardHandler>::createEntry(const string& key,
        const vector<FieldValueTuple>& data)
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

    if (!startWdOnPort(port, detectionTime, restorationTime, action))
    {
        SWSS_LOG_ERROR("Failed to start PFC Watchdog on port %s", port.m_alias.c_str());
        return;
    }

    SWSS_LOG_NOTICE("Started PFC Watchdog on port %s", port.m_alias.c_str());
}

template <typename DropHandler, typename ForwardHandler>
void PfcWdOrch<DropHandler, ForwardHandler>::deleteEntry(const string& name)
{
    SWSS_LOG_ENTER();

    Port port;
    gPortsOrch->getPort(name, port);

    if (!stopWdOnPort(port))
    {
        SWSS_LOG_ERROR("Failed to stop PFC Watchdog on port %s", name.c_str());
        return;
    }

    SWSS_LOG_NOTICE("Stopped PFC Watchdog on port %s", name.c_str());
}

template <typename DropHandler, typename ForwardHandler>
void PfcWdSwOrch<DropHandler, ForwardHandler>::registerInWdDb(const Port& port,
        uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL;

    sai_status_t status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get PFC mask on port %s: %d", port.m_alias.c_str(), status);
        return;
    }

    if (!c_portStatIds.empty())
    {
        vector<FieldValueTuple> fieldValues;
        string str = counterIdsToStr(c_portStatIds, &sai_serialize_port_stat);
        fieldValues.emplace_back(PFC_WD_PORT_COUNTER_ID_LIST, str);

        m_pfcWdTable->set(
                sai_serialize_object_id(port.m_port_id),
                fieldValues);
    }

    uint8_t pfcMask = attr.value.u8;
    for (uint8_t i = 0; i < PFC_WD_TC_MAX; i++)
    {
        if ((pfcMask & (1 << i)) == 0)
        {
            continue;
        }

        sai_object_id_t queueId = port.m_queue_ids[i];
        string queueIdStr = sai_serialize_object_id(queueId);

        // Store detection and restoration time for plugins
        vector<FieldValueTuple> countersFieldValues;
        countersFieldValues.emplace_back("PFC_WD_DETECTION_TIME", to_string(detectionTime * 1000));
        countersFieldValues.emplace_back("PFC_WD_RESTORATION_TIME", to_string(restorationTime * 1000));
        countersFieldValues.emplace_back("PFC_WD_ACTION", this->serializeAction(action));

        PfcWdOrch<DropHandler, ForwardHandler>::getCountersTable()->set(queueIdStr, countersFieldValues);

        // We register our queues in PFC_WD table so that syncd will know that it must poll them
        vector<FieldValueTuple> queueFieldValues;

        if (!c_queueStatIds.empty())
        {
            string str = counterIdsToStr(c_queueStatIds, sai_serialize_queue_stat);
            queueFieldValues.emplace_back(PFC_WD_QUEUE_COUNTER_ID_LIST, str);
        }

        if (!c_queueAttrIds.empty())
        {
            string str = counterIdsToStr(c_queueAttrIds, sai_serialize_queue_attr);
            queueFieldValues.emplace_back(PFC_WD_QUEUE_ATTR_ID_LIST, str);
        }

        // Create internal entry
        m_entryMap.emplace(queueId, PfcWdQueueEntry(action, port.m_port_id, i));

        m_pfcWdTable->set(queueIdStr, queueFieldValues);

        // Initialize PFC WD related counters
        PfcWdActionHandler::initWdCounters(
                PfcWdOrch<DropHandler, ForwardHandler>::getCountersTable(),
                sai_serialize_object_id(queueId));
    }
}

template <typename DropHandler, typename ForwardHandler>
void PfcWdSwOrch<DropHandler, ForwardHandler>::unregisterFromWdDb(const Port& port)
{
    SWSS_LOG_ENTER();

    for (uint8_t i = 0; i < PFC_WD_TC_MAX; i++)
    {
        sai_object_id_t queueId = port.m_queue_ids[i];

        // Unregister in syncd
        m_pfcWdTable->del(sai_serialize_object_id(queueId));
        m_entryMap.erase(queueId);
    }
}

template <typename DropHandler, typename ForwardHandler>
PfcWdSwOrch<DropHandler, ForwardHandler>::PfcWdSwOrch(
        DBConnector *db,
        vector<string> &tableNames,
        const vector<sai_port_stat_t> &portStatIds,
        const vector<sai_queue_stat_t> &queueStatIds,
        const vector<sai_queue_attr_t> &queueAttrIds):
    PfcWdOrch<DropHandler, ForwardHandler>(db, tableNames),
    m_pfcWdDb(new DBConnector(PFC_WD_DB, DBConnector::DEFAULT_UNIXSOCKET, 0)),
    m_pfcWdTable(new ProducerStateTable(m_pfcWdDb.get(), PFC_WD_STATE_TABLE)),
    c_portStatIds(portStatIds),
    c_queueStatIds(queueStatIds),
    c_queueAttrIds(queueAttrIds)
{
    SWSS_LOG_ENTER();

    string platform = getenv("platform") ? getenv("platform") : "";
    if (platform == "")
    {
        SWSS_LOG_ERROR("Platform environment variable is not defined");
        return;
    }

    string detectSha, restoreSha;
    string detectPluginName = "pfc_detect_" + platform + ".lua";
    string restorePluginName = "pfc_restore.lua";

    try
    {
        string detectLuaScript = swss::loadLuaScript(detectPluginName);
        detectSha = swss::loadRedisScript(
                PfcWdOrch<DropHandler, ForwardHandler>::getCountersDb().get(),
                detectLuaScript);

        string restoreLuaScript = swss::loadLuaScript(restorePluginName);
        restoreSha = swss::loadRedisScript(
                PfcWdOrch<DropHandler, ForwardHandler>::getCountersDb().get(),
                restoreLuaScript);

        vector<FieldValueTuple> fieldValues;
        fieldValues.emplace_back(SAI_OBJECT_TYPE, sai_serialize_object_type(SAI_OBJECT_TYPE_QUEUE));

        auto pluginTable = ProducerStateTable(m_pfcWdDb.get(), PLUGIN_TABLE);
        pluginTable.set(detectSha, fieldValues);
        pluginTable.set(restoreSha, fieldValues);
    }
    catch (...)
    {
        SWSS_LOG_WARN("Lua scripts for PFC watchdog were not loaded");
    }
}

template <typename DropHandler, typename ForwardHandler>
PfcWdSwOrch<DropHandler, ForwardHandler>::~PfcWdSwOrch(void)
{
    SWSS_LOG_ENTER();
}

template <typename DropHandler, typename ForwardHandler>
PfcWdSwOrch<DropHandler, ForwardHandler>::PfcWdQueueEntry::PfcWdQueueEntry(
        PfcWdAction action, sai_object_id_t port, uint8_t idx):
    action(action),
    portId(port),
    index(idx)
{
    SWSS_LOG_ENTER();
}

template <typename DropHandler, typename ForwardHandler>
bool PfcWdSwOrch<DropHandler, ForwardHandler>::startWdOnPort(const Port& port,
        uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action)
{
    SWSS_LOG_ENTER();

    registerInWdDb(port, detectionTime, restorationTime, action);

    if (!m_runPfcWdSwOrchThread.load())
    {
        startWatchdogThread();
    }

    return true;
}

template <typename DropHandler, typename ForwardHandler>
bool PfcWdSwOrch<DropHandler, ForwardHandler>::stopWdOnPort(const Port& port)
{
    SWSS_LOG_ENTER();

    if (m_runPfcWdSwOrchThread.load())
    {
        endWatchdogThread();
    }

    unregisterFromWdDb(port);

    return true;
}

template <typename DropHandler, typename ForwardHandler>
void PfcWdSwOrch<DropHandler, ForwardHandler>::handleWdNotification(swss::NotificationConsumer& wdNotification)
{
    SWSS_LOG_ENTER();

    string queueIdStr;
    string event;
    vector<swss::FieldValueTuple> values;

    wdNotification.pop(queueIdStr, event, values);

    sai_object_id_t queueId = SAI_NULL_OBJECT_ID;
    sai_deserialize_object_id(queueIdStr, queueId);

    auto entry = m_entryMap.find(queueId);
    if (entry == m_entryMap.end())
    {
        SWSS_LOG_ERROR("Queue %s is not registered", queueIdStr.c_str());
        return;
    }

    SWSS_LOG_NOTICE("Receive notification, %s", event.c_str());
    if (event == "storm")
    {
        if (entry->second.action == PfcWdAction::PFC_WD_ACTION_ALERT)
        {
            if (entry->second.handler == nullptr)
            {
                entry->second.handler = make_shared<PfcWdActionHandler>(
                        entry->second.portId,
                        entry->first,
                        entry->second.index,
                        PfcWdOrch<DropHandler, ForwardHandler>::getCountersTable());
            }
        }
        else if (entry->second.action == PfcWdAction::PFC_WD_ACTION_DROP)
        {
            entry->second.handler = make_shared<DropHandler>(
                    entry->second.portId,
                    entry->first,
                    entry->second.index,
                    PfcWdOrch<DropHandler, ForwardHandler>::getCountersTable());
        }
        else if (entry->second.action == PfcWdAction::PFC_WD_ACTION_FORWARD)
        {
            entry->second.handler = make_shared<ForwardHandler>(
                    entry->second.portId,
                    entry->first,
                    entry->second.index,
                    PfcWdOrch<DropHandler, ForwardHandler>::getCountersTable());
        }
        else
        {
            throw runtime_error("Unknown PFC WD action");
        }
        entry->second.handler->initCounters();
    }
    else if (event == "restore")
    {
        entry->second.handler->commitCounters();
        entry->second.handler = nullptr;
    }
    else
    {
        SWSS_LOG_ERROR("Received unknown event from plugin, %s", event.c_str());
    }
}

template <typename DropHandler, typename ForwardHandler>
void PfcWdSwOrch<DropHandler, ForwardHandler>::pfcWatchdogThread(void)
{
    SWSS_LOG_ENTER();

    DBConnector db(COUNTERS_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);

    swss::Select s;
    shared_ptr<swss::NotificationConsumer> wdNotification =
        make_shared<swss::NotificationConsumer>(
                PfcWdSwOrch<DropHandler, ForwardHandler>::getCountersDb().get(),
                "PFC_WD");

    s.addSelectable(wdNotification.get());

    while(m_runPfcWdSwOrchThread)
    {
        unique_lock<mutex> lk(m_pfcWdMutex);

        swss::Selectable *sel = NULL;
        int fd;

        int result = s.select(&sel, &fd, PFC_WD_POLL_TIMEOUT);

        if (sel == wdNotification.get())
        {
            handleWdNotification(*wdNotification.get());
        }
        else if (result == swss::Select::TIMEOUT)
        {
            // Do nothing
        }
        else
        {
            SWSS_LOG_ERROR("Received unexpected object on select");
        }
    }
}

template <typename DropHandler, typename ForwardHandler>
void PfcWdSwOrch<DropHandler, ForwardHandler>::startWatchdogThread(void)
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

template <typename DropHandler, typename ForwardHandler>
void PfcWdSwOrch<DropHandler, ForwardHandler>::endWatchdogThread(void)
{
    SWSS_LOG_ENTER();

    if (!m_runPfcWdSwOrchThread.load())
    {
        return;
    }

    m_runPfcWdSwOrchThread = false;

    if (m_pfcWatchdogThread != nullptr)
    {
        SWSS_LOG_INFO("Wait for PFC Watchdog thread to end");

        m_pfcWatchdogThread->join();
    }

    SWSS_LOG_INFO("PFC Watchdog thread ended");
}

// Trick to keep member functions in a separate file
template class PfcWdSwOrch<PfcWdZeroBufferHandler, PfcWdLossyHandler>;
template class PfcWdSwOrch<PfcWdActionHandler, PfcWdActionHandler>;
