#include "pfcwdhworch.h"
#include "notifications.h"
#include "notifier.h"
#include "schema.h"
#include "switchorch.h"
#include "portsorch.h"
#include "sai_serialize.h"
#include <algorithm>
#include <set>

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t* sai_switch_api;
extern sai_port_api_t* sai_port_api;
extern sai_queue_api_t* sai_queue_api;
extern SwitchOrch *gSwitchOrch;
extern PortsOrch *gPortsOrch;

// Global instance pointer for SAI callback

PfcWdHwOrch::PfcWdHwOrch(DBConnector *db, vector<string> &tableNames,
                         const vector<sai_port_stat_t> &portStatIds,
                         const vector<sai_queue_stat_t> &queueStatIds,
                         const vector<sai_queue_attr_t> &queueAttrIds):
    PfcWdBaseOrch(db, tableNames),
    c_portStatIds(portStatIds),
    c_queueStatIds(queueStatIds),
    c_queueAttrIds(queueAttrIds),
    m_detectionTimeMin(0),
    m_detectionTimeMax(0),
    m_restorationTimeMin(0),
    m_restorationTimeMax(0),
    m_pfcWdHwStateTable(make_shared<Table>(getStateDb().get(), STATE_PFC_WD_HW_STATE_TABLE_NAME))
{
    SWSS_LOG_ENTER();

    // Set global instance pointer

    // Mark hardware watchdog recovery in STATE_DB
    this->updateStateTable(PFC_WD_RECOVERY_MECHANISM, PFC_WD_RECOVERY_HARDWARE);
    this->updateDlrPacketActionInStateTable();

    SWSS_LOG_NOTICE("Initializing hardware-based PFC watchdog");

    initializeTimerRanges();
    registerCallbacks();
    recoverWarmReboot(db);

    SWSS_LOG_NOTICE("Hardware-based PFC watchdog initialization complete");
}

PfcWdHwOrch::~PfcWdHwOrch(void)
{
    SWSS_LOG_ENTER();

}

void PfcWdHwOrch::initializeTimerRanges()
{
    SWSS_LOG_ENTER();

    // Query hardware timer range capabilities
    sai_attribute_t attr_dld, attr_dlr;
    attr_dld.id = SAI_SWITCH_ATTR_PFC_TC_DLD_INTERVAL_RANGE;
    attr_dlr.id = SAI_SWITCH_ATTR_PFC_TC_DLR_INTERVAL_RANGE;

    sai_status_t status_dld = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr_dld);
    sai_status_t status_dlr = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr_dlr);

    if (status_dld == SAI_STATUS_SUCCESS && status_dlr == SAI_STATUS_SUCCESS)
    {
        m_detectionTimeMin = attr_dld.value.u32range.min;
        m_detectionTimeMax = attr_dld.value.u32range.max;
        m_restorationTimeMin = attr_dlr.value.u32range.min;
        m_restorationTimeMax = attr_dlr.value.u32range.max;

        SWSS_LOG_NOTICE("Hardware timer ranges - Detection: %u-%u ms, Restoration: %u-%u ms",
                       m_detectionTimeMin, m_detectionTimeMax,
                       m_restorationTimeMin, m_restorationTimeMax);

        // Store ranges in STATE_DB
        vector<FieldValueTuple> fvs = {
            { PFC_WD_HW_DETECTION_TIME_MIN,   to_string(m_detectionTimeMin)   },
            { PFC_WD_HW_DETECTION_TIME_MAX,   to_string(m_detectionTimeMax)   },
            { PFC_WD_HW_RESTORATION_TIME_MIN, to_string(m_restorationTimeMin) },
            { PFC_WD_HW_RESTORATION_TIME_MAX, to_string(m_restorationTimeMax) },
        };
        this->updateStateTable(fvs);

        m_timerRangesValid = true;
    }
    else
    {
        // Without the ranges the configured times cannot be checked against
        // the hardware, so they are accepted as given and SAI reports any
        // value it will not take.
        SWSS_LOG_WARN("Failed to query PFC watchdog hardware timer ranges (detection: %d, restoration: %d), "
                      "configured times will not be range checked",
                     status_dld, status_dlr);
    }
}

void PfcWdHwOrch::registerCallbacks()
{
    SWSS_LOG_ENTER();

    // Register PFC deadlock notification callback
    bool supported = gSwitchOrch->querySwitchCapability(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_QUEUE_PFC_DEADLOCK_NOTIFY);
    if (supported)
    {
        /* The SAI notification runs on a libsairedis thread, so it is forwarded to
         * the ASIC_DB NOTIFICATIONS channel and consumed here on the orchagent
         * thread instead, keeping the watchdog state single-threaded. */
        m_notificationsDb = make_shared<DBConnector>("ASIC_DB", 0);
        m_deadlockNotificationConsumer = new swss::NotificationConsumer(
            m_notificationsDb.get(), "NOTIFICATIONS");
        m_deadlockNotificationConsumer->setOpAllowList({SAI_SWITCH_NOTIFICATION_NAME_QUEUE_PFC_DEADLOCK});
        m_deadlockNotificationConsumer->setStatsLabel("PfcWdHwOrch:" SAI_SWITCH_NOTIFICATION_NAME_QUEUE_PFC_DEADLOCK);
        auto deadlockNotifier = new Notifier(m_deadlockNotificationConsumer, this,
                                             "PFC_WD_HW_DEADLOCK_NOTIFICATIONS");
        Orch::addExecutor(deadlockNotifier);

        sai_attribute_t attr;
        attr.id = SAI_SWITCH_ATTR_QUEUE_PFC_DEADLOCK_NOTIFY;
        attr.value.ptr = (void *)on_queue_pfc_deadlock;   // forwards to ASIC_DB NOTIFICATIONS

        sai_status_t status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to register PFC deadlock notification callback (status: %d)", status);
        }
        else
        {
            SWSS_LOG_NOTICE("Registered PFC deadlock notification callback");
        }
    }
    else
    {
        SWSS_LOG_NOTICE("PFC deadlock notification not supported by hardware");
    }
}

void PfcWdHwOrch::recoverWarmReboot(DBConnector *db)
{
    SWSS_LOG_ENTER();

    // Re-apply existing CONFIG_DB entries after warm reboot
    Table cfgPfcWdTable(db, CFG_PFC_WD_TABLE_NAME);
    vector<string> keys;
    cfgPfcWdTable.getKeys(keys);

    if (!keys.empty())
    {
        SWSS_LOG_NOTICE("Found %zu existing PFC watchdog configuration(s), will re-apply after ports are ready",
                       keys.size());
        addExistingData(&cfgPfcWdTable);
    }
    else
    {
        SWSS_LOG_INFO("No existing PFC watchdog configuration found");
    }
}

void PfcWdHwOrch::doTask(NotificationConsumer &consumer)
{
    SWSS_LOG_ENTER();

    if (&consumer != m_deadlockNotificationConsumer)
    {
        return;
    }

    std::deque<KeyOpFieldsValuesTuple> entries;
    consumer.pops(entries);

    for (auto &entry : entries)
    {
        string op   = kfvOp(entry);
        string data = kfvKey(entry);

        if (op != SAI_SWITCH_NOTIFICATION_NAME_QUEUE_PFC_DEADLOCK)
        {
            continue;
        }

        uint32_t count = 0;
        sai_queue_deadlock_notification_data_t *deadlockData = nullptr;

        sai_deserialize_queue_deadlock_ntf(data, count, &deadlockData);

        onQueuePfcDeadlock(count, deadlockData);

        sai_deserialize_free_queue_deadlock_ntf(count, deadlockData);
    }
}

void PfcWdHwOrch::onQueuePfcDeadlock(uint32_t count, sai_queue_deadlock_notification_data_t *data)
{
    SWSS_LOG_ENTER();

    for (uint32_t i = 0; i < count; i++)
    {
        auto& notification = data[i];

        // Look up port information for this queue
        sai_object_id_t port_id = 0;
        string port_alias;
        uint8_t queue_index = 0;
        bool port_found = false;

        auto it = m_queueToPortMap.find(notification.queue_id);
        if (it != m_queueToPortMap.end())
        {
            port_id = it->second.port_id;
            port_alias = it->second.port_alias;
            queue_index = it->second.queue_index;
            port_found = true;
        }

        if (notification.event == SAI_QUEUE_PFC_DEADLOCK_EVENT_TYPE_DETECTED)
        {
            // Initialize counters on storm detection
            if (port_found)
            {
                m_stormedQueues.insert(notification.queue_id);
                SWSS_LOG_DEBUG("PfcWdHwOrch: Queue 0x%" PRIx64 " entered storm", notification.queue_id);

                this->report_pfc_storm(notification.queue_id, port_id,
                                      queue_index, port_alias, "");
            }
            else
            {
                SWSS_LOG_WARN("PFC deadlock DETECTED on queue 0x%" PRIx64 " (no port info)", notification.queue_id);
            }

            // Let SAI/SDK manage recovery automatically
            notification.app_managed_recovery = false;
        }
        else if (notification.event == SAI_QUEUE_PFC_DEADLOCK_EVENT_TYPE_RECOVERED)
        {
            // Update counters on recovery
            if (port_found)
            {
                m_stormedQueues.erase(notification.queue_id);
                SWSS_LOG_DEBUG("PfcWdHwOrch: Queue 0x%" PRIx64 " left storm", notification.queue_id);

                this->report_pfc_restored(notification.queue_id, port_id,
                                         queue_index, port_alias);
            }
            else
            {
                SWSS_LOG_NOTICE("PFC deadlock RECOVERED on queue 0x%" PRIx64 " (no port info)", notification.queue_id);
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown PFC deadlock event type %d on queue 0x%" PRIx64,
                          notification.event, notification.queue_id);
        }
    }
}

task_process_status PfcWdHwOrch::createEntry(const string& key, const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();

    // GLOBAL configuration not supported for hardware watchdog
    if (key == PFC_WD_GLOBAL)
    {
        SWSS_LOG_INFO("Ignoring %s entry: the hardware watchdog takes its configuration per port",
                      key.c_str());

        return task_process_status::task_invalid_entry;
    }

    uint32_t detectionTime = 0;
    uint32_t restorationTime = 0;
    // Default action is drop
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

    // Parse configuration fields
    for (auto i : data)
    {
        const auto &field = fvField(i);
        const auto &value = fvValue(i);

        try
        {
            if (field == PFC_WD_DETECTION_TIME)
            {
                detectionTime = static_cast<uint32_t>(stoul(value));
            }
            else if (field == PFC_WD_RESTORATION_TIME)
            {
                restorationTime = static_cast<uint32_t>(stoul(value));
            }
            else if (field == PFC_WD_ACTION)
            {
                action = deserializeAction(value);
                if (action == PfcWdAction::PFC_WD_ACTION_UNKNOWN)
                {
                    SWSS_LOG_ERROR("Invalid PFC Watchdog action %s", value.c_str());
                    return task_process_status::task_invalid_entry;
                }
            }
            else if (field == PFC_STAT_HISTORY)
            {
                pfcStatHistory = value;
            }
            else
            {
                SWSS_LOG_ERROR("Unknown PFC Watchdog configuration field %s", field.c_str());
                return task_process_status::task_invalid_entry;
            }
        }
        catch (const invalid_argument& e)
        {
            SWSS_LOG_ERROR("Failed to parse PFC Watchdog %s attribute %s invalid argument error",
                          key.c_str(), field.c_str());
            return task_process_status::task_invalid_entry;
        }
        catch (const out_of_range& e)
        {
            SWSS_LOG_ERROR("Failed to parse PFC Watchdog %s attribute %s out of range error",
                          key.c_str(), field.c_str());
            return task_process_status::task_invalid_entry;
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Failed to parse PFC Watchdog %s attribute %s. Unknown error has been occurred",
                          key.c_str(), field.c_str());
            return task_process_status::task_invalid_entry;
        }
    }

    // Validation
    if (detectionTime == 0)
    {
        SWSS_LOG_ERROR("%s missing", PFC_WD_DETECTION_TIME);
        return task_process_status::task_invalid_entry;
    }
    if (restorationTime == 0)
    {
        SWSS_LOG_ERROR("%s missing", PFC_WD_RESTORATION_TIME);
        return task_process_status::task_invalid_entry;
    }

    // Both times are checked here rather than while parsing, so that a field
    // left out of the configuration is still validated.
    if (m_timerRangesValid)
    {
        if (detectionTime < m_detectionTimeMin || detectionTime > m_detectionTimeMax)
        {
            SWSS_LOG_ERROR("Detection time %u ms is outside the supported range [%u-%u] ms on port %s",
                           detectionTime, m_detectionTimeMin, m_detectionTimeMax, key.c_str());
            return task_process_status::task_invalid_entry;
        }

        if (restorationTime < m_restorationTimeMin || restorationTime > m_restorationTimeMax)
        {
            SWSS_LOG_ERROR("Restoration time %u ms is outside the supported range [%u-%u] ms on port %s",
                           restorationTime, m_restorationTimeMin, m_restorationTimeMax, key.c_str());
            return task_process_status::task_invalid_entry;
        }
    }
    if (pfcStatHistory != "enable" && pfcStatHistory != "disable")
    {
        SWSS_LOG_ERROR("%s is invalid value for %s", pfcStatHistory.c_str(), PFC_STAT_HISTORY);
        return task_process_status::task_invalid_entry;
    }

    // All ports must use the same switch-level PFC DLR packet action.
    // Action can only change when no ports are configured, or when
    // reconfiguring the single existing port.
    PfcWdAction currentAction = this->getPfcDlrPacketAction();

    bool isSinglePortReconfiguration = (m_pfcwd_ports.size() == 1 &&
                                        m_pfcwd_ports.find(port.m_alias) != m_pfcwd_ports.end());

    if (currentAction != PfcWdAction::PFC_WD_ACTION_UNKNOWN &&
        currentAction != action &&
        !isSinglePortReconfiguration)
    {
        SWSS_LOG_ERROR("PFC DLR packet action mismatch on port %s: current=%s, requested=%s. "
                      "All ports must use the same action. "
                      "Action can only be changed when no ports are configured or when reconfiguring the only configured port.",
                      port.m_alias.c_str(),
                      serializeAction(currentAction).c_str(),
                      serializeAction(action).c_str());
        return task_process_status::task_invalid_entry;
    }

    // Check if port is already configured and has any queue in stormed state
    if (m_pfcwd_ports.find(port.m_alias) != m_pfcwd_ports.end())
    {
        if (isPortInStormedState(port))
        {
            SWSS_LOG_ERROR("Cannot modify PFC watchdog configuration on port %s: port is in stormed state. "
                          "Wait for storm to pass before making changes.",
                          port.m_alias.c_str());
            return task_process_status::task_invalid_entry;
        }
    }

    // The hardware watchdog has no in-place update, so an existing
    // configuration is torn down first. A port that was never configured has
    // nothing to stop, and issuing the disable anyway would clear intervals
    // and DLDR on queues that were never programmed.
    if (m_pfcwd_ports.find(port.m_alias) != m_pfcwd_ports.end())
    {
        SWSS_LOG_INFO("Disabling the existing hardware PFC watchdog on port %s before reconfiguring",
                      port.m_alias.c_str());
        stopWdOnPort(port);
    }

    if (!startWdOnPort(port, detectionTime, restorationTime, action, pfcStatHistory))
    {
        return handleStartWdOnPortFailure(port);
    }

    clearPfcWdPending(port);
    SWSS_LOG_NOTICE("Started PFC Watchdog on port %s", port.m_alias.c_str());
    // Port is tracked in m_pfcwd_ports by configureHwWatchdog
    return task_process_status::task_success;
}

task_process_status PfcWdHwOrch::deleteEntry(const string& key)
{
	SWSS_LOG_ENTER();

	Port port;
	if (!gPortsOrch->getPort(key, port))
	{
		SWSS_LOG_ERROR("Invalid port interface %s", key.c_str());
		return task_process_status::task_invalid_entry;
	}

	// If hardware watchdog is configured on this port, disallow deletion
	// while any lossless queue is still in stormed state.
	if (m_pfcwd_ports.find(port.m_alias) != m_pfcwd_ports.end())
	{
		if (isPortInStormedState(port))
		{
			SWSS_LOG_ERROR(
				"Cannot delete PFC watchdog configuration on port %s: port is in stormed state. "
				"Wait for storm to pass before making changes.",
				port.m_alias.c_str());
			return task_process_status::task_invalid_entry;
		}
	}

	// Delegate to base implementation to stop watchdog on the port and
	// update common bookkeeping.
	task_process_status status = PfcWdBaseOrch::deleteEntry(key);

	// A port deferred for a missing lossless TC is only removed from the pending
	// set when it starts, so a port deleted while still deferred would stay there
	// for the lifetime of the process.
	if (status == task_process_status::task_success)
	{
		clearPfcWdPending(port);
	}

	return status;
}

bool PfcWdHwOrch::startWdOnPort(const Port& port,
	    uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action, string pfcStatHistory)
{
	SWSS_LOG_ENTER();

	// For hardware-based watchdog, all hardware programming and flex counter
	// registration are handled in configureHwWatchdog().
	// Any existing configuration is cleaned up via stopWdOnPort() before this
	// function is invoked from createEntry().
	return configureHwWatchdog(port, detectionTime, restorationTime, action);
}

bool PfcWdHwOrch::stopWdOnPort(const Port& port)
{
    SWSS_LOG_ENTER();

    return disableHwWatchdog(port);
}

bool PfcWdHwOrch::startWdActionOnQueue(const string &event, sai_object_id_t queueId, const string &info)
{
    SWSS_LOG_ENTER();

    // Not used - hardware watchdog reports storms via SAI notifications

    SWSS_LOG_ERROR("startWdActionOnQueue is not supported for hardware-based PFC watchdog. "
                  "Queue 0x%" PRIx64 ", event: %s. Hardware handles actions automatically.",
                  queueId, event.c_str());

    return false;
}

bool PfcWdHwOrch::readBackTimerValue(const Port& port, sai_port_attr_t attrId,
                                     const set<uint8_t>& losslessTc, uint32_t expected,
                                     uint32_t& actual, const string& timerName)
{
    vector<sai_map_t> readBack(PFC_WD_TC_MAX);
    for (uint32_t i = 0; i < PFC_WD_TC_MAX; i++)
    {
        readBack[i].key = i;
        readBack[i].value = 0;
    }

    sai_attribute_t attr;
    attr.id = attrId;
    attr.value.maplist.count = PFC_WD_TC_MAX;
    attr.value.maplist.list = readBack.data();

    sai_status_t status = sai_port_api->get_port_attribute(port.m_port_id, 1, &attr);
    if (status == SAI_STATUS_SUCCESS && attr.value.maplist.count > 0)
    {
        for (uint32_t i = 0; i < attr.value.maplist.count; i++)
        {
            uint8_t tcKey = static_cast<uint8_t>(attr.value.maplist.list[i].key);
            if (losslessTc.find(tcKey) != losslessTc.end())
            {
                actual = attr.value.maplist.list[i].value;
                break;
            }
        }

        if (actual != expected)
        {
            SWSS_LOG_WARN("%s time mismatch on port %s: sent %u, hardware has %u",
                          timerName.c_str(), port.m_alias.c_str(), expected, actual);
        }
        return true;
    }
    else
    {
        SWSS_LOG_WARN("Failed to read back %s time on port %s (status: %d)",
                      timerName.c_str(), port.m_alias.c_str(), status);
        return false;
    }
}

void PfcWdHwOrch::mapQueuesToPort(const Port& port, const set<uint8_t>& losslessTc)
{
    SWSS_LOG_ENTER();

    // The deadlock notification carries only a queue id, so keep the port and
    // TC each watched queue belongs to for the lookup in onQueuePfcDeadlock().
    for (auto tc : losslessTc)
    {
        if (tc >= port.m_queue_ids.size())
        {
            continue;
        }

        sai_object_id_t queueId = port.m_queue_ids[tc];

        PortQueueInfo info;
        info.port_id = port.m_port_id;
        info.port_alias = port.m_alias;
        info.queue_index = tc;
        m_queueToPortMap[queueId] = info;

        SWSS_LOG_NOTICE("Watching queue 0x%" PRIx64 " on port %s TC %d",
                        queueId, port.m_alias.c_str(), tc);
    }
}

bool PfcWdHwOrch::configureHwWatchdog(const Port& port, uint32_t detectionTime,
                                      uint32_t restorationTime, PfcWdAction action)
{
    SWSS_LOG_ENTER();

    // Validate port has lossless TCs before configuring hardware
    set<uint8_t> losslessTc;
    if (!getLosslessTcsForPort(port, losslessTc))
    {
        writeFailureStatus(port);
        return false;
    }

    SWSS_LOG_NOTICE("Configuring hardware watchdog on port %s: detection=%ums, restoration=%ums, action=%s",
                    port.m_alias.c_str(), detectionTime, restorationTime, serializeAction(action).c_str());

    // Cleanup handler called on configuration failure
    auto handleFailure = [this, &port](const string& errorMsg) -> bool {
        SWSS_LOG_ERROR("%s", errorMsg.c_str());
        disableHwWatchdog(port);
        writeFailureStatus(port);
        return false;
    };

    // Configure switch-level action if needed
    if (!configureSwitchAction(port, action, handleFailure))
    {
        return false;
    }

    // Configure detection and restoration intervals
    if (!configureTimerIntervals(port, losslessTc, detectionTime, restorationTime, handleFailure))
    {
        return false;
    }

    // Enable deadlock detection/recovery on queues
    if (!enableDldrOnLosslessQueues(port, losslessTc, detectionTime, restorationTime, handleFailure))
    {
        return false;
    }

    // Keep the queue to port mapping the deadlock notification needs
    mapQueuesToPort(port, losslessTc);

    // Track this port
    m_pfcwd_ports.insert(port.m_alias);

    SWSS_LOG_NOTICE("Successfully configured hardware PFC watchdog on port %s with %zu lossless TCs",
                   port.m_alias.c_str(), losslessTc.size());

    // Read back and verify timer values from hardware
    uint32_t actualHwDetectionTime = detectionTime;
    uint32_t actualHwRestorationTime = restorationTime;

    readBackTimerValue(port, SAI_PORT_ATTR_PFC_TC_DLD_INTERVAL, losslessTc,
                      detectionTime, actualHwDetectionTime, "Detection");
    readBackTimerValue(port, SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL, losslessTc,
                      restorationTime, actualHwRestorationTime, "Restoration");

    // Write success to STATE_DB
    vector<FieldValueTuple> fvs;
    fvs.emplace_back("recovery_type", "hardware");
    fvs.emplace_back("status", "configured");
    fvs.emplace_back("hw_detection_time", to_string(actualHwDetectionTime));
    fvs.emplace_back("hw_restoration_time", to_string(actualHwRestorationTime));
    fvs.emplace_back("configured_detection_time", to_string(detectionTime));
    fvs.emplace_back("configured_restoration_time", to_string(restorationTime));

    fvs.emplace_back("action", serializeAction(action));
    m_pfcWdHwStateTable->set(port.m_alias, fvs);

    return true;
}

bool PfcWdHwOrch::configureSwitchAction(const Port& port, PfcWdAction action,
                                        const function<bool(const string&)>& handleFailure)
{
    SWSS_LOG_ENTER();

    // Only set action if not already configured
    if (this->getPfcDlrPacketAction() != PfcWdAction::PFC_WD_ACTION_UNKNOWN)
    {
        return true;
    }

    sai_packet_action_t sai_action;
    if (action == PfcWdAction::PFC_WD_ACTION_DROP)
    {
        sai_action = SAI_PACKET_ACTION_DROP;
    }
    else if (action == PfcWdAction::PFC_WD_ACTION_FORWARD || action == PfcWdAction::PFC_WD_ACTION_ALERT)
    {
        sai_action = SAI_PACKET_ACTION_FORWARD;
    }
    else
    {
        return handleFailure("Unsupported PFC DLR packet action: " + serializeAction(action));
    }

    sai_attribute_t attr;
    attr.id = SAI_SWITCH_ATTR_PFC_DLR_PACKET_ACTION;
    attr.value.u32 = sai_action;

    sai_status_t status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        return handleFailure("Failed to set switch level PFC DLR packet action to " +
                           serializeAction(action) + " on port " + port.m_alias +
                           ": " + to_string(status));
    }

    SWSS_LOG_NOTICE("Set PFC DLR packet action to %s at switch level (SAI action: %d)",
                   serializeAction(action).c_str(), sai_action);

    this->setPfcDlrPacketAction(action);
    this->updateDlrPacketActionInStateTable();

    return true;
}

bool PfcWdHwOrch::configureTimerIntervals(const Port& port, const set<uint8_t>& losslessTc,
                                          uint32_t detectionTime, uint32_t restorationTime,
                                          const function<bool(const string&)>& handleFailure)
{
    SWSS_LOG_ENTER();

    // Build map lists for detection and restoration intervals
    std::vector<sai_map_t> dld_map_list;
    std::vector<sai_map_t> dlr_map_list;

    for (auto tc : losslessTc)
    {
        sai_map_t dld_map;
        dld_map.key = tc;
        dld_map.value = detectionTime;
        dld_map_list.push_back(dld_map);

        sai_map_t dlr_map;
        dlr_map.key = tc;
        dlr_map.value = restorationTime;
        dlr_map_list.push_back(dlr_map);
    }

    // Set detection interval on port
    sai_attribute_t attr_dld;
    attr_dld.id = SAI_PORT_ATTR_PFC_TC_DLD_INTERVAL;
    attr_dld.value.maplist.count = static_cast<uint32_t>(dld_map_list.size());
    attr_dld.value.maplist.list = dld_map_list.data();

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr_dld);
    if (status != SAI_STATUS_SUCCESS)
    {
        return handleFailure("Failed to set PFC DLD interval on port " + port.m_alias +
                           ": " + to_string(status));
    }

    SWSS_LOG_NOTICE("Set PFC DLD (detection) interval on port %s to %u ms for %zu TCs",
                   port.m_alias.c_str(), detectionTime, losslessTc.size());

    // Set restoration interval on port
    sai_attribute_t attr_dlr;
    attr_dlr.id = SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL;
    attr_dlr.value.maplist.count = static_cast<uint32_t>(dlr_map_list.size());
    attr_dlr.value.maplist.list = dlr_map_list.data();

    status = sai_port_api->set_port_attribute(port.m_port_id, &attr_dlr);
    if (status != SAI_STATUS_SUCCESS)
    {
        return handleFailure("Failed to set PFC DLR interval on port " + port.m_alias +
                           ": " + to_string(status));
    }

    SWSS_LOG_NOTICE("Set PFC DLR (restoration) interval on port %s to %u ms for %zu TCs",
                   port.m_alias.c_str(), restorationTime, losslessTc.size());

    return true;
}

bool PfcWdHwOrch::enableDldrOnLosslessQueues(const Port& port, const set<uint8_t>& losslessTc,
                                             uint32_t detectionTime, uint32_t restorationTime,
                                             const function<bool(const string&)>& handleFailure)
{
    SWSS_LOG_ENTER();

    // Enable PFC DLDR on each lossless queue
    for (auto tc : losslessTc)
    {
        if (tc >= port.m_queue_ids.size())
        {
            SWSS_LOG_ERROR("TC %d exceeds queue count %zu on port %s",
                          tc, port.m_queue_ids.size(), port.m_alias.c_str());
            continue;
        }

        sai_object_id_t queueId = port.m_queue_ids[tc];

        sai_attribute_t attr_enable;
        attr_enable.id = SAI_QUEUE_ATTR_ENABLE_PFC_DLDR;
        attr_enable.value.booldata = true;

        sai_status_t status = sai_queue_api->set_queue_attribute(queueId, &attr_enable);
        if (status != SAI_STATUS_SUCCESS)
        {
            return handleFailure("Failed to enable PFC DLDR on port " + port.m_alias +
                               " queue " + to_string(tc) + " (0x" +
                               sai_serialize_object_id(queueId) + "): " + to_string(status));
        }

        SWSS_LOG_NOTICE("Enabled PFC DLDR on port %s TC %d queue 0x%" PRIx64 " (detection: %u ms, restoration: %u ms)",
                       port.m_alias.c_str(), tc, queueId, detectionTime, restorationTime);
    }

    return true;
}

bool PfcWdHwOrch::disableHwWatchdog(const Port& port)
{
    SWSS_LOG_ENTER();

    // A PORT_QOS_MAP update can clear the PFC mask while the watchdog is running,
    // so an empty lossless TC set only means there is no queue left to reprogram.
    // The port level teardown and the bookkeeping below still have to run, or the
    // port keeps its intervals and the tracked action stays set with no port
    // configured, which then rejects every later action change.
    std::set<uint8_t> losslessTc;
    if (!getLosslessTcsForPort(port, losslessTc))
    {
        SWSS_LOG_INFO("No lossless TC on port %s, clearing the port level watchdog state only",
                      port.m_alias.c_str());
    }

    SWSS_LOG_NOTICE("Disabling hardware watchdog on port %s", port.m_alias.c_str());

    // Disable PFC DLDR on each lossless queue and remove from queue→port mapping
    for (auto tc : losslessTc)
    {
        if (tc >= port.m_queue_ids.size())
        {
            SWSS_LOG_ERROR("TC %d exceeds queue count %zu on port %s",
                          tc, port.m_queue_ids.size(), port.m_alias.c_str());
            continue;
        }

        sai_object_id_t queueId = port.m_queue_ids[tc];

        sai_attribute_t attr_enable;
        attr_enable.id = SAI_QUEUE_ATTR_ENABLE_PFC_DLDR;
        attr_enable.value.booldata = false;

        sai_status_t status = sai_queue_api->set_queue_attribute(queueId, &attr_enable);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to disable PFC DLDR on port %s queue %d (0x%" PRIx64 "): %d",
                          port.m_alias.c_str(), tc, queueId, status);
        }
        else
        {
            SWSS_LOG_INFO("Disabled PFC DLDR on port %s TC %d queue 0x%" PRIx64,
                         port.m_alias.c_str(), tc, queueId);
        }

        // Remove from queue→port mapping
        m_queueToPortMap.erase(queueId);

        m_stormedQueues.erase(queueId);
    }

    // The lossless TC set may have shrunk since the port was configured, so drop
    // anything still mapped to this port instead of only the TCs seen above.
    for (auto it = m_queueToPortMap.begin(); it != m_queueToPortMap.end(); )
    {
        if (it->second.port_alias == port.m_alias)
        {
            m_stormedQueues.erase(it->first);
            it = m_queueToPortMap.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Clear detection and restoration intervals on port level
    std::vector<sai_map_t> empty_map_list;

    // Clear detection interval
    sai_attribute_t attr_dld;
    attr_dld.id = SAI_PORT_ATTR_PFC_TC_DLD_INTERVAL;
    attr_dld.value.maplist.count = 0;
    attr_dld.value.maplist.list = nullptr;

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr_dld);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to clear PFC DLD interval on port %s: %d", port.m_alias.c_str(), status);
    }

    // Clear restoration interval
    sai_attribute_t attr_dlr;
    attr_dlr.id = SAI_PORT_ATTR_PFC_TC_DLR_INTERVAL;
    attr_dlr.value.maplist.count = 0;
    attr_dlr.value.maplist.list = nullptr;

    status = sai_port_api->set_port_attribute(port.m_port_id, &attr_dlr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to clear PFC DLR interval on port %s: %d", port.m_alias.c_str(), status);
    }

    // Remove port from tracking set
    m_pfcwd_ports.erase(port.m_alias);

    // Remove entry from STATE_DB
    m_pfcWdHwStateTable->del(port.m_alias);

    // If no ports have hardware watchdog configured, reset action to unknown.
    // Only the tracked action is cleared: the switch level attribute is left as
    // programmed so that reconfiguring the last port does not flap the action
    // through FORWARD, and it is rewritten by configureSwitchAction() as soon as
    // a port is configured again.
    if (m_pfcwd_ports.empty())
    {
        this->setPfcDlrPacketAction(PfcWdAction::PFC_WD_ACTION_UNKNOWN);
        this->updateDlrPacketActionInStateTable();
        SWSS_LOG_NOTICE("All hardware PFC watchdog ports disabled, reset action to UNKNOWN");
    }

    SWSS_LOG_NOTICE("Successfully disabled hardware PFC watchdog on port %s",
                   port.m_alias.c_str());

    return true;
}

void PfcWdHwOrch::writeFailureStatus(const Port& port)
{
    vector<FieldValueTuple> fvs;
    fvs.emplace_back("recovery_type", "hardware");
    fvs.emplace_back("status", "failed");
    m_pfcWdHwStateTable->set(port.m_alias, fvs);
}

bool PfcWdHwOrch::isPortInStormedState(const Port& port)
{
    SWSS_LOG_ENTER();

    // Get PFC mask to identify lossless queues
    uint8_t pfcMask = 0;
    if (!gPortsOrch->getPortPfcWatchdogStatus(port.m_port_id, &pfcMask))
    {
        // The mask is unreadable, so no lossless queue can be inspected. The port
        // is reported as not stormed: treating it as stormed instead would make
        // the configuration undeletable for as long as the read keeps failing.
        SWSS_LOG_WARN("Failed to get PFC mask on port %s", port.m_alias.c_str());
        return false;
    }

    // Check each lossless queue to see if any is in stormed state
    for (uint8_t i = 0; i < PFC_WD_TC_MAX; i++)
    {
        if ((pfcMask & (1 << i)) == 0)
        {
            continue;  // Skip non-lossless queues
        }

        if (i >= port.m_queue_ids.size())
        {
            continue;
        }

        sai_object_id_t queueId = port.m_queue_ids[i];

        if (m_stormedQueues.count(queueId) != 0)
        {
            SWSS_LOG_WARN("Port %s has queue %d (0x%" PRIx64 ") in stormed state",
                         port.m_alias.c_str(), i, queueId);
            return true;
        }
    }

    return false;
}

