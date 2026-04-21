#include <limits.h>
#include <inttypes.h>
#include <unordered_map>
#include "pfcwdsworch.h"
#include "pfcwdorch.h"
#include "sai_serialize.h"
#include "portsorch.h"
#include "converter.h"
#include "redisapi.h"
#include "select.h"
#include "notifier.h"
#include "schema.h"
#include "subscriberstatetable.h"

#define PFC_WD_GLOBAL                   "GLOBAL"
#define PFC_WD_ACTION                   "action"
#define PFC_WD_DETECTION_TIME           "detection_time"
#define PFC_WD_RESTORATION_TIME         "restoration_time"
#define PFC_STAT_HISTORY                "pfc_stat_history"
#define BIG_RED_SWITCH_FIELD            "BIG_RED_SWITCH"
#define PFC_WD_IN_STORM                 "storm"

#define PFC_WD_DETECTION_TIME_MAX       (5 * 1000)
#define PFC_WD_DETECTION_TIME_MIN       100
#define PFC_WD_RESTORATION_TIME_MAX     (60 * 1000)
#define PFC_WD_RESTORATION_TIME_MIN     100
#define PFC_WD_POLL_TIMEOUT             5000
#define SAI_PORT_STAT_PFC_PREFIX        "SAI_PORT_STAT_PFC_"
#define COUNTER_CHECK_POLL_TIMEOUT_SEC  1

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t* sai_switch_api;
extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;

extern event_handle_t g_events_handle;

extern SwitchOrch *gSwitchOrch;
extern PortsOrch *gPortsOrch;

template <typename DropHandler, typename ForwardHandler>
task_process_status PfcWdSwOrch<DropHandler, ForwardHandler>::createEntry(const string& key,
        const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();
    
    if (key == PFC_WD_GLOBAL)
    {
        for (auto valuePair: data)
        {
            const auto &field = fvField(valuePair);
            const auto &value = fvValue(valuePair);

            if (field == POLL_INTERVAL_FIELD)
            {
                this->m_pfcwdFlexCounterManager->updateGroupPollingInterval(stoi(value));
            }
            else if (field == BIG_RED_SWITCH_FIELD)
            {
                SWSS_LOG_NOTICE("Receive brs mode set, %s", value.c_str());
                setBigRedSwitchMode(value);
            }
        }
    }
    else
    {
        return PfcWdBaseOrch::createEntry(key, data);
    }

    return task_process_status::task_success;

}

template <typename DropHandler, typename ForwardHandler>
void PfcWdSwOrch<DropHandler, ForwardHandler>::setBigRedSwitchMode(const string value)
{
    SWSS_LOG_ENTER();

    if (value == "enable")
    {
        // When BIG_RED_SWITCH mode is enabled, pfcwd is automatically disabled
        enableBigRedSwitchMode();
    }
    else if (value == "disable")
    {
        disableBigRedSwitchMode();
    }
    else
    {
        SWSS_LOG_NOTICE("Unsupported BIG_RED_SWITCH mode set input, please use enable or disable");
    }

}

template <typename DropHandler, typename ForwardHandler>
void PfcWdSwOrch<DropHandler, ForwardHandler>::disableBigRedSwitchMode()
{
    SWSS_LOG_ENTER();

    m_bigRedSwitchFlag = false;
    // Disable pfcwdaction handler on each queue if exists.
    for (auto &entry : m_brsEntryMap)
    {

        if (entry.second.handler != nullptr)
        {
            SWSS_LOG_NOTICE(
                    "PFC Watchdog BIG_RED_SWITCH mode disabled on port %s, queue index %d, queue id 0x%" PRIx64 " and port id 0x%" PRIx64 ".",
                    entry.second.portAlias.c_str(),
                    entry.second.index,
                    entry.first,
                    entry.second.portId);

            entry.second.handler->commitCounters();
            entry.second.handler = nullptr;
        }

        auto queueId = entry.first;
        string countersKey = this->getCountersTable()->getTableName() + this->getCountersTable()->getTableNameSeparator() + sai_serialize_object_id(queueId);
        this->getCountersDb()->hdel(countersKey, "BIG_RED_SWITCH_MODE");
    }

    m_brsEntryMap.clear();
}

template <typename DropHandler, typename ForwardHandler>
void PfcWdSwOrch<DropHandler, ForwardHandler>::enableBigRedSwitchMode()
{
    SWSS_LOG_ENTER();

    m_bigRedSwitchFlag =  true;
    // Write to database that each queue enables BIG_RED_SWITCH
    auto allPorts = gPortsOrch->getAllPorts();

    for (auto &it: allPorts)
    {
        Port port = it.second;
        uint8_t pfcMask = 0;

        if (port.m_type != Port::PHY)
        {
            SWSS_LOG_INFO("Skip non-phy port %s", port.m_alias.c_str());
            continue;
        }

        if (!gPortsOrch->getPortPfcWatchdogStatus(port.m_port_id, &pfcMask))
        {
            SWSS_LOG_ERROR("Failed to get PFC watchdog mask on port %s", port.m_alias.c_str());
            return;
        }

        for (uint8_t i = 0; i < PFC_WD_TC_MAX; i++)
        {
            sai_object_id_t queueId = port.m_queue_ids[i];
            if ((pfcMask & (1 << i)) == 0 && m_entryMap.find(queueId) == m_entryMap.end())
            {
                continue;
            }

            string queueIdStr = sai_serialize_object_id(queueId);

            vector<FieldValueTuple> countersFieldValues;
            countersFieldValues.emplace_back("BIG_RED_SWITCH_MODE", "enable");
            this->getCountersTable()->set(queueIdStr, countersFieldValues);
        }
    }

    // Disable pfcwdaction handler on each queue if exists.
    for (auto & entry: m_entryMap)
    {
        if (entry.second.handler != nullptr)
        {
            entry.second.handler->commitCounters();
            entry.second.handler = nullptr;
        }
    }

    // Create pfcwdaction handler on all the ports.
    for (auto & it: allPorts)
    {
        Port port = it.second;
        uint8_t pfcMask = 0;

        if (port.m_type != Port::PHY)
        {
            SWSS_LOG_INFO("Skip non-phy port %s", port.m_alias.c_str());
            continue;
        }

        if (!gPortsOrch->getPortPfcWatchdogStatus(port.m_port_id, &pfcMask))
        {
            SWSS_LOG_ERROR("Failed to get PFC watchdog mask on port %s", port.m_alias.c_str());
            return;
        }

        for (uint8_t i = 0; i < PFC_WD_TC_MAX; i++)
        {
            if ((pfcMask & (1 << i)) == 0)
            {
                continue;
            }

            sai_object_id_t queueId = port.m_queue_ids[i];
            string queueIdStr = sai_serialize_object_id(queueId);

            auto entry = m_brsEntryMap.emplace(queueId, PfcWdQueueEntry(PfcWdAction::PFC_WD_ACTION_DROP, port.m_port_id, i, port.m_alias)).first;

            if (entry->second.handler== nullptr)
            {
                SWSS_LOG_NOTICE(
                        "PFC Watchdog BIG_RED_SWITCH mode enabled on port %s, queue index %d, queue id 0x%" PRIx64 " and port id 0x%" PRIx64 ".",
                        entry->second.portAlias.c_str(),
                        entry->second.index,
                        entry->first,
                        entry->second.portId);

                entry->second.handler = make_shared<DropHandler>(
                        entry->second.portId,
                        entry->first,
                        entry->second.index,
                        this->getCountersTable());
                entry->second.handler->initCounters();
            }
        }
    }
}

template <typename DropHandler, typename ForwardHandler>
bool PfcWdSwOrch<DropHandler, ForwardHandler>::registerInWdDb(const Port& port,
        uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action, string pfcStatHistory)
{
    SWSS_LOG_ENTER();

    set<uint8_t> losslessTc;
    if (!getLosslessTcsForPort(port, losslessTc))
    {
        SWSS_LOG_NOTICE("No lossless TC found on port %s", port.m_alias.c_str());
        return false;
    }

    if (!c_portStatIds.empty())
    {
        auto portStatIdSet = filterPfcCounters(PfcWdBaseOrch::counterIdsToStr(c_portStatIds, &sai_serialize_port_stat), losslessTc);
        this->m_pfcwdFlexCounterManager->setCounterIdList(port.m_port_id, CounterType::PORT, portStatIdSet, SAI_OBJECT_TYPE_PORT);
    }

    for (auto i : losslessTc)
    {
        sai_object_id_t queueId = port.m_queue_ids[i];
        string queueIdStr = sai_serialize_object_id(queueId);

        // Store detection and restoration time for plugins
        vector<FieldValueTuple> countersFieldValues;
        countersFieldValues.emplace_back("PFC_WD_DETECTION_TIME", to_string(detectionTime * 1000));
        // Restoration time is optional
        countersFieldValues.emplace_back("PFC_WD_RESTORATION_TIME",
                restorationTime == 0 ?
                "" :
                to_string(restorationTime * 1000));
        countersFieldValues.emplace_back("PFC_WD_ACTION", this->serializeAction(action));
        countersFieldValues.emplace_back("PFC_STAT_HISTORY", pfcStatHistory);

        this->getCountersTable()->set(queueIdStr, countersFieldValues);

        // We register our queues in PFC_WD table so that syncd will know that it must poll them
        string key = getFlexCounterTableKey(queueIdStr);

        if (!c_queueStatIds.empty())
        {
            auto queueStatIdSet = PfcWdBaseOrch::counterIdsToStr(c_queueStatIds, sai_serialize_queue_stat);
            this->m_pfcwdFlexCounterManager->setCounterIdList(queueId, CounterType::QUEUE, queueStatIdSet, SAI_OBJECT_TYPE_QUEUE);
        }

        if (!c_queueAttrIds.empty())
        {
            auto queueAttrIdSet = PfcWdBaseOrch::counterIdsToStr(c_queueAttrIds, sai_serialize_queue_attr);
            (dynamic_cast<FlexCounterManager*>(this->m_pfcwdFlexCounterManager.get()))->setCounterIdList(queueId, CounterType::QUEUE_ATTR, queueAttrIdSet);
        }

        // Create internal entry
        m_entryMap.emplace(queueId, PfcWdQueueEntry(action, port.m_port_id, i, port.m_alias));

        // Initialize PFC WD related counters
        PfcWdActionHandler::initWdCounters(
                this->getCountersTable(),
                sai_serialize_object_id(queueId));
    }

    // We do NOT need to create ACL table group here. It will be
    // done when ACL tables are bound to ports
    return true;
}

template <typename DropHandler, typename ForwardHandler>
unordered_set<string> PfcWdSwOrch<DropHandler, ForwardHandler>::filterPfcCounters(const unordered_set<string> &counters, set<uint8_t>& losslessTc)
{
    SWSS_LOG_ENTER();

    unordered_set<string> filterCounters;

    for (auto &counter : counters)
    {
        //auto &counter = it.first;
        size_t index = 0;
        index = counter.find(SAI_PORT_STAT_PFC_PREFIX);
        if (index != 0)
        {
            filterCounters.emplace(counter);
        }
        else
        {
            uint8_t tc = (uint8_t)atoi(counter.substr(index + sizeof(SAI_PORT_STAT_PFC_PREFIX) - 1, 1).c_str());
            if (losslessTc.count(tc))
            {
                filterCounters.emplace(counter);
            }
        }
    }

    return filterCounters;
}

template <typename DropHandler, typename ForwardHandler>
string PfcWdSwOrch<DropHandler, ForwardHandler>::getFlexCounterTableKey(string key)
{
    SWSS_LOG_ENTER();

    return string(PFC_WD_FLEX_COUNTER_GROUP) + ":" + key;
}

template <typename DropHandler, typename ForwardHandler>
void PfcWdSwOrch<DropHandler, ForwardHandler>::unregisterFromWdDb(const Port& port)
{
    SWSS_LOG_ENTER();

    this->m_pfcwdFlexCounterManager->clearCounterIdList(port.m_port_id, SAI_OBJECT_TYPE_PORT);

    for (uint8_t i = 0; i < PFC_WD_TC_MAX; i++)
    {
        sai_object_id_t queueId = port.m_queue_ids[i];
        this->m_pfcwdFlexCounterManager->clearCounterIdList(queueId, SAI_OBJECT_TYPE_QUEUE);

        auto entry = m_entryMap.find(queueId);
        if (entry != m_entryMap.end() && entry->second.handler != nullptr)
        {
            entry->second.handler->commitCounters();
        }

        m_entryMap.erase(queueId);

        // Clean up
        string countersKey = this->getCountersTable()->getTableName() + this->getCountersTable()->getTableNameSeparator() + sai_serialize_object_id(queueId);
        this->getCountersDb()->hdel(countersKey, {"PFC_WD_DETECTION_TIME", "PFC_WD_RESTORATION_TIME", "PFC_WD_ACTION", "PFC_WD_STATUS"});
    }

}

template <typename DropHandler, typename ForwardHandler>
PfcWdSwOrch<DropHandler, ForwardHandler>::PfcWdSwOrch(
        DBConnector *db,
        vector<string> &tableNames,
        const vector<sai_port_stat_t> &portStatIds,
        const vector<sai_queue_stat_t> &queueStatIds,
        const vector<sai_queue_attr_t> &queueAttrIds,
        int pollInterval):
    PfcWdBaseOrch(db, tableNames),
    c_portStatIds(portStatIds),
    c_queueStatIds(queueStatIds),
    c_queueAttrIds(queueAttrIds),
    m_pollInterval(pollInterval),
    m_applDb(make_shared<DBConnector>("APPL_DB", 0)),
    m_applTable(make_shared<Table>(m_applDb.get(), APP_PFC_WD_TABLE_NAME "_INSTORM"))
{
    SWSS_LOG_ENTER();

    string detectSha, restoreSha;
    string detectPluginName = "pfc_detect_" + this->m_platform + ".lua";
    string restorePluginName;
    string pollIntervalStr = to_string(m_pollInterval);
    string plugins;
    if (this->m_platform == CISCO_8000_PLATFORM_SUBSTRING) {
        restorePluginName = "pfc_restore_" + this->m_platform + ".lua";
    } else {
        restorePluginName = "pfc_restore.lua";
    }

    try
    {
        string detectLuaScript = swss::loadLuaScript(detectPluginName);
        detectSha = swss::loadRedisScript(
                this->getCountersDb().get(),
                detectLuaScript);

        string restoreLuaScript = swss::loadLuaScript(restorePluginName);
        restoreSha = swss::loadRedisScript(
                this->getCountersDb().get(),
                restoreLuaScript);
        plugins = detectSha + "," + restoreSha;
    }
    catch (...)
    {
        SWSS_LOG_WARN("Lua scripts and polling interval for PFC watchdog were not set successfully");
    }

    this->m_pfcwdFlexCounterManager = make_shared<FlexCounterTaggedCachedManager<sai_object_type_t>>(
        "PFC_WD", StatsMode::READ, m_pollInterval, true, make_pair(QUEUE_PLUGIN_FIELD, plugins));

    auto consumer = new swss::NotificationConsumer(
            this->getCountersDb().get(),
            "PFC_WD_ACTION");
    auto wdNotification = new Notifier(consumer, this, "PFC_WD_ACTION");
    Orch::addExecutor(wdNotification);

    auto interv = timespec { .tv_sec = COUNTER_CHECK_POLL_TIMEOUT_SEC, .tv_nsec = 0 };
    auto timer = new SelectableTimer(interv);
    auto executor = new ExecutableTimer(timer, this, "PFC_WD_COUNTERS_POLL");
    Orch::addExecutor(executor);
    timer->start();

    auto ssTable = new swss::SubscriberStateTable(
            m_applDb.get(), APP_PFC_WD_TABLE_NAME, TableConsumable::DEFAULT_POP_BATCH_SIZE, default_orch_pri);
    auto ssConsumer = new Consumer(ssTable, this, APP_PFC_WD_TABLE_NAME);
    Orch::addExecutor(ssConsumer);
}

template <typename DropHandler, typename ForwardHandler>
PfcWdSwOrch<DropHandler, ForwardHandler>::~PfcWdSwOrch(void)
{
    SWSS_LOG_ENTER();
}

template <typename DropHandler, typename ForwardHandler>
PfcWdSwOrch<DropHandler, ForwardHandler>::PfcWdQueueEntry::PfcWdQueueEntry(
        PfcWdAction action, sai_object_id_t port, uint8_t idx, string alias):
    action(action),
    portId(port),
    index(idx),
    portAlias(alias)
{
    SWSS_LOG_ENTER();
}

template <typename DropHandler, typename ForwardHandler>
bool PfcWdSwOrch<DropHandler, ForwardHandler>::startWdOnPort(const Port& port,
        uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action, string pfcStatHistory)
{
    SWSS_LOG_ENTER();

    return registerInWdDb(port, detectionTime, restorationTime, action, pfcStatHistory);
}

template <typename DropHandler, typename ForwardHandler>
bool PfcWdSwOrch<DropHandler, ForwardHandler>::stopWdOnPort(const Port& port)
{
    SWSS_LOG_ENTER();

    unregisterFromWdDb(port);

    return true;
}

template <typename DropHandler, typename ForwardHandler>
void PfcWdSwOrch<DropHandler, ForwardHandler>::doTask(Consumer& consumer)
{
    PfcWdBaseOrch::doTask(consumer);

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    if ((consumer.getDbName() == "APPL_DB") && (consumer.getTableName() == APP_PFC_WD_TABLE_NAME))
    {
        auto it = consumer.m_toSync.begin();
        while (it != consumer.m_toSync.end())
        {
            KeyOpFieldsValuesTuple &t = it->second;

            string &key = kfvKey(t);
            Port port;
            if (!gPortsOrch->getPort(key, port))
            {
                SWSS_LOG_ERROR("Invalid port interface %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }
            if (port.m_type != Port::PHY)
            {
                SWSS_LOG_ERROR("Interface %s is not physical port", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            vector<FieldValueTuple> &fvTuples = kfvFieldsValues(t);
            for (const auto &fv : fvTuples)
            {
                int qIdx = -1;
                string q = fvField(fv);
                try
                {
                    qIdx = stoi(q);
                }
                catch (const std::invalid_argument &e)
                {
                    SWSS_LOG_ERROR("Invalid argument %s to %s()", q.c_str(), e.what());
                    continue;
                }
                catch (const std::out_of_range &e)
                {
                    SWSS_LOG_ERROR("Out of range argument %s to %s()", q.c_str(), e.what());
                    continue;
                }

                if ((qIdx < 0) || (static_cast<unsigned int>(qIdx) >= port.m_queue_ids.size()))
                {
                    SWSS_LOG_ERROR("Invalid queue index %d on port %s", qIdx, key.c_str());
                    continue;
                }

                string status = fvValue(fv);
                if (status != PFC_WD_IN_STORM)
                {
                    SWSS_LOG_ERROR("Port %s queue %s not in %s", key.c_str(), q.c_str(), PFC_WD_IN_STORM);
                    continue;
                }

                SWSS_LOG_INFO("Port %s queue %s in status %s ", key.c_str(), q.c_str(), status.c_str());
                if (!startWdActionOnQueue(PFC_WD_IN_STORM, port.m_queue_ids[qIdx]))
                {
                    SWSS_LOG_ERROR("Failed to start PFC watchdog %s action on port %s queue %d", PFC_WD_IN_STORM, key.c_str(), qIdx);
                    continue;
                }
            }

            it = consumer.m_toSync.erase(it);
        }
    }
}

template <typename DropHandler, typename ForwardHandler>
void PfcWdSwOrch<DropHandler, ForwardHandler>::doTask()
{
    SWSS_LOG_ENTER();

    // In the warm-reboot case with ongoing PFC storm,
    // we care about dependency.
    // PFC watchdog should be started on a port queue before
    // a storm action can be taken in effect. The PFC watchdog
    // configuration is stored in CONFIG_DB CFG_PFC_WD_TABLE_NAME,
    // while the ongoing storming port queue is recorded
    // in APPL_DB APP_PFC_WD_TABLE_NAME. We thus invoke the Executor
    // in this order.
    // In the cold-boot case, APP_PFC_WD_TABLE_NAME will not
    // be populated. No dependency is introduced in this case.
    auto *cfg_exec = this->getExecutor(CFG_PFC_WD_TABLE_NAME);
    cfg_exec->drain();

    auto *appl_exec = this->getExecutor(APP_PFC_WD_TABLE_NAME);
    appl_exec->drain();

    for (const auto &it : this->m_consumerMap)
    {
        auto *exec = it.second.get();

        if ((exec == cfg_exec) || (exec == appl_exec))
        {
            continue;
        }
        exec->drain();
    }
}

template <typename DropHandler, typename ForwardHandler>
void PfcWdSwOrch<DropHandler, ForwardHandler>::doTask(swss::NotificationConsumer& wdNotification)
{
    SWSS_LOG_ENTER();

    string queueIdStr;
    string event;
    vector<swss::FieldValueTuple> values;

    wdNotification.pop(queueIdStr, event, values);

    string info;
    for (auto &fv : values)
    {
        info += fvField(fv) + ":" + fvValue(fv) + "|";
    }
    if (!info.empty())
    {
        info.pop_back();
    }

    sai_object_id_t queueId = SAI_NULL_OBJECT_ID;
    sai_deserialize_object_id(queueIdStr, queueId);

    if (!startWdActionOnQueue(event, queueId, info))
    {
        SWSS_LOG_ERROR("Failed to start PFC watchdog %s event action on queue %s", event.c_str(), queueIdStr.c_str());
    }
}

template <typename DropHandler, typename ForwardHandler>
void PfcWdSwOrch<DropHandler, ForwardHandler>::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    for (auto& handlerPair : m_entryMap)
    {
        if (handlerPair.second.handler != nullptr)
        {
            handlerPair.second.handler->commitCounters(true);
        }
    }

}

template <typename DropHandler, typename ForwardHandler>
bool PfcWdSwOrch<DropHandler, ForwardHandler>::startWdActionOnQueue(const string &event, sai_object_id_t queueId, const string &info)
{
    auto entry = m_entryMap.find(queueId);
    if (entry == m_entryMap.end())
    {
        SWSS_LOG_ERROR("Queue 0x%" PRIx64 " is not registered", queueId);
        return false;
    }

    SWSS_LOG_NOTICE("Receive notification, %s", event.c_str());

    if (m_bigRedSwitchFlag)
    {
        SWSS_LOG_NOTICE("Big_RED_SWITCH mode is on, ignore syncd pfc watchdog notification");
    }
    else if (event == "storm")
    {
        if (entry->second.action == PfcWdAction::PFC_WD_ACTION_ALERT)
        {
            if (entry->second.handler == nullptr)
            {
                report_pfc_storm(entry->first, entry->second.portId, entry->second.index, entry->second.portAlias, info);

                entry->second.handler = make_shared<PfcWdActionHandler>(
                        entry->second.portId,
                        entry->first,
                        entry->second.index,
                        this->getCountersTable());
                entry->second.handler->initCounters();
                // Log storm event to APPL_DB for warm-reboot purpose
                string key = m_applTable->getTableName() + m_applTable->getTableNameSeparator() + entry->second.portAlias;
                m_applDb->hset(key, to_string(entry->second.index), PFC_WD_IN_STORM);
            }
        }
        else if (entry->second.action == PfcWdAction::PFC_WD_ACTION_DROP)
        {
            if (entry->second.handler == nullptr)
            {
                report_pfc_storm(entry->first, entry->second.portId, entry->second.index, entry->second.portAlias, info);

                entry->second.handler = make_shared<DropHandler>(
                        entry->second.portId,
                        entry->first,
                        entry->second.index,
                        this->getCountersTable());
                entry->second.handler->initCounters();
                // Log storm event to APPL_DB for warm-reboot purpose
                string key = m_applTable->getTableName() + m_applTable->getTableNameSeparator() + entry->second.portAlias;
                m_applDb->hset(key, to_string(entry->second.index), PFC_WD_IN_STORM);
            }
        }
        else if (entry->second.action == PfcWdAction::PFC_WD_ACTION_FORWARD)
        {
            if (entry->second.handler == nullptr)
            {
                report_pfc_storm(entry->first, entry->second.portId, entry->second.index, entry->second.portAlias, info);

                entry->second.handler = make_shared<ForwardHandler>(
                        entry->second.portId,
                        entry->first,
                        entry->second.index,
                        this->getCountersTable());
                entry->second.handler->initCounters();
                // Log storm event to APPL_DB for warm-reboot purpose
                string key = m_applTable->getTableName() + m_applTable->getTableNameSeparator() + entry->second.portAlias;
                m_applDb->hset(key, to_string(entry->second.index), PFC_WD_IN_STORM);
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown PFC WD action");
            return false;
        }
    }
    else if (event == "restore")
    {
        if (entry->second.handler != nullptr)
        {
            report_pfc_restored(entry->first, entry->second.portId, entry->second.index, entry->second.portAlias);

            entry->second.handler->commitCounters();
            entry->second.handler = nullptr;
            // Remove storm status in APPL_DB for warm-reboot purpose
            string key = m_applTable->getTableName() + m_applTable->getTableNameSeparator() + entry->second.portAlias;
            m_applDb->hdel(key, to_string(entry->second.index));
        }
    }
    else
    {
        SWSS_LOG_ERROR("Received unknown event from plugin, %s", event.c_str());
        return false;
    }

    return true;
}

template <typename DropHandler, typename ForwardHandler>
bool PfcWdSwOrch<DropHandler, ForwardHandler>::bake()
{
    // clean all *_last and *_LEFT fields in COUNTERS_TABLE
    // to allow warm-reboot pfc detect & restore state machine to enter the same init state as cold-reboot
    vector<string> cKeys;
    this->getCountersTable()->getKeys(cKeys);
    for (const auto &key : cKeys)
    {
        vector<FieldValueTuple> fvTuples;
        this->getCountersTable()->get(key, fvTuples);
        vector<string> wLasts;
        for (const auto &fv : fvTuples)
        {
            if ((fvField(fv).find("_last") != string::npos) || (fvField(fv).find("_LEFT") != string::npos))
            {
                wLasts.push_back(fvField(fv));
            }
        }
        if (!wLasts.empty())
        {
            this->getCountersDb()->hdel(
                this->getCountersTable()->getTableName()
                + this->getCountersTable()->getTableNameSeparator()
                + key,
                wLasts);
        }
    }

    Orch::bake();

    Consumer *consumer = dynamic_cast<Consumer *>(this->getExecutor(APP_PFC_WD_TABLE_NAME));
    if (consumer == NULL)
    {
        SWSS_LOG_ERROR("No consumer %s in Orch", APP_PFC_WD_TABLE_NAME);
        return false;
    }

    size_t refilled = consumer->refillToSync(m_applTable.get());
    SWSS_LOG_NOTICE("Add warm input PFC watchdog State: %s, %zd", APP_PFC_WD_TABLE_NAME, refilled);

    return true;
}

// Trick to keep member functions in a separate file
template class PfcWdSwOrch<PfcWdZeroBufferHandler, PfcWdLossyHandler>;
template class PfcWdSwOrch<PfcWdAclHandler, PfcWdLossyHandler>;
template class PfcWdSwOrch<PfcWdDlrHandler, PfcWdDlrHandler>;
template class PfcWdSwOrch<PfcWdSaiDlrInitHandler, PfcWdActionHandler>;

