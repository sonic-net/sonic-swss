#include "pfcwdhworch.h"
#include "schema.h"
#include "switchorch.h"
#include "portsorch.h"
#include "saiextensions.h"
#include "sai_serialize.h"
#include <algorithm>
#include <set>

#define PFC_WD_COUNTER_POLL_TIMEOUT_SEC  1
#define PFC_HW_WD_POLL_MSECS 50

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t* sai_switch_api;
extern sai_port_api_t* sai_port_api;
extern sai_queue_api_t* sai_queue_api;
extern sai_buffer_api_t* sai_buffer_api;
extern SwitchOrch *gSwitchOrch;
extern PortsOrch *gPortsOrch;

// Global instance pointer for SAI callback
static PfcWdHwOrch* g_pfcWdHwOrch = nullptr;

// SAI callback wrapper
static void on_queue_pfc_deadlock(
        _In_ uint32_t count,
        _In_ sai_queue_deadlock_notification_data_t *data)
{
    if (g_pfcWdHwOrch != nullptr)
    {
        g_pfcWdHwOrch->onQueuePfcDeadlock(count, data);
    }
}

namespace {

// Maximum tick count for hardware timers.
// TODO: Hard-coded value, replace with SAI query when API support is available.
constexpr uint32_t MAX_TICK_COUNT = 15;

} // anonymous namespace

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
    m_stateDb(make_shared<DBConnector>("STATE_DB", 0)),
    m_pfcWdHwStateTable(make_shared<Table>(m_stateDb.get(), STATE_PFC_WD_HW_STATE_TABLE_NAME)),
    m_portLevelGranularitySupported(false)
{
    SWSS_LOG_ENTER();

    // Set global instance pointer
    g_pfcWdHwOrch = this;

    // Mark hardware watchdog recovery in STATE_DB
    this->updateStateTable(PFC_WD_RECOVERY_MECHANISM, PFC_WD_RECOVERY_HARDWARE);

    SWSS_LOG_NOTICE("Initializing hardware-based PFC watchdog");

    initializeCapabilities();
    initializeTimerRanges();
    registerCallbacks();
    recoverWarmReboot(db);

    SWSS_LOG_NOTICE("Hardware-based PFC watchdog initialization complete");
}

PfcWdHwOrch::~PfcWdHwOrch(void)
{
    SWSS_LOG_ENTER();

    g_pfcWdHwOrch = nullptr;
}

void PfcWdHwOrch::initializeCapabilities()
{
    SWSS_LOG_ENTER();

    // Detect platform capability for port-level timer granularity
    sai_attr_capability_t attr_capability;
    sai_status_t cap_status = sai_query_attribute_capability(
        gSwitchId,
        SAI_OBJECT_TYPE_PORT,
        SAI_PORT_ATTR_PFC_TC_DLD_TIMER_INTERVAL,
        &attr_capability);

    if (cap_status == SAI_STATUS_SUCCESS &&
        attr_capability.set_implemented &&
        attr_capability.get_implemented)
    {
        m_portLevelGranularitySupported = true;
        SWSS_LOG_NOTICE("Port-level PFC DLD timer granularity supported");
    }
    else
    {
        m_portLevelGranularitySupported = false;
        SWSS_LOG_NOTICE("Port-level PFC DLD timer granularity not supported, using default 100ms");
    }
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
        this->updateStateTable(PFC_WD_HW_DETECTION_TIME_MIN, to_string(m_detectionTimeMin));
        this->updateStateTable(PFC_WD_HW_DETECTION_TIME_MAX, to_string(m_detectionTimeMax));
        this->updateStateTable(PFC_WD_HW_RESTORATION_TIME_MIN, to_string(m_restorationTimeMin));
        this->updateStateTable(PFC_WD_HW_RESTORATION_TIME_MAX, to_string(m_restorationTimeMax));
    }
    else
    {
        SWSS_LOG_WARN("Failed to query PFC watchdog hardware timer ranges (detection: %d, restoration: %d)",
                     status_dld, status_dlr);
    }
}

void PfcWdHwOrch::registerCallbacks()
{
    SWSS_LOG_ENTER();

    // FlexCounter manager for PFC_WD queue statistics
    m_pfcwdFlexCounterManager = make_shared<FlexCounterTaggedCachedManager<sai_object_type_t>>(
        PFC_WD_FLEX_COUNTER_GROUP,
        StatsMode::READ,
        PFC_HW_WD_POLL_MSECS,
        true,
        make_pair("", ""));

    SWSS_LOG_NOTICE("Initialized FlexCounter for hardware PFC watchdog with %d ms poll interval",
                   PFC_HW_WD_POLL_MSECS);

    // Create timer for periodic counter updates
    auto interv = timespec { .tv_sec = PFC_WD_COUNTER_POLL_TIMEOUT_SEC, .tv_nsec = 0 };
    auto timer = new SelectableTimer(interv);
    auto executor = new ExecutableTimer(timer, this, "PFC_WD_HW_COUNTERS_POLL");
    Orch::addExecutor(executor);
    timer->start();
    SWSS_LOG_NOTICE("Started periodic counter update timer with %d second interval",
                   PFC_WD_COUNTER_POLL_TIMEOUT_SEC);

    // Register PFC deadlock notification callback
    bool supported = gSwitchOrch->querySwitchCapability(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_QUEUE_PFC_DEADLOCK_NOTIFY);
    if (supported)
    {
        sai_attribute_t attr;
        attr.id = SAI_SWITCH_ATTR_QUEUE_PFC_DEADLOCK_NOTIFY;
        attr.value.ptr = (void *)on_queue_pfc_deadlock;

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
                string queueIdStr = sai_serialize_object_id(notification.queue_id);
                initQueueCounters(queueIdStr, notification.queue_id, queue_index);
                SWSS_LOG_DEBUG("PfcWdHwOrch: Initialized counters for queue 0x%" PRIx64 " on storm detection",
                              notification.queue_id);

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
                string queueIdStr = sai_serialize_object_id(notification.queue_id);
                updateQueueCounters(queueIdStr, notification.queue_id, queue_index, false);  // false = not periodic (recovery)
                SWSS_LOG_DEBUG("PfcWdHwOrch: Updated counters for queue 0x%" PRIx64 " on storm restoration",
                              notification.queue_id);

                // Remove baseline stats
                m_queueBaselineStats.erase(notification.queue_id);

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

PfcWdHwOrch::PfcWdQueueStats PfcWdHwOrch::getQueueStats(const string &queueIdStr)
{
    SWSS_LOG_ENTER();

    PfcWdQueueStats stats;
    memset(&stats, 0, sizeof(PfcWdQueueStats));
    stats.operational = true;
    vector<FieldValueTuple> fieldValues;

    if (!this->getCountersTable()->get(queueIdStr, fieldValues))
    {
        // Return zeros if entry doesn't exist
        return stats;
    }

    for (const auto& fv : fieldValues)
    {
        const auto field = fvField(fv);
        const auto value = fvValue(fv);

        if (field == "PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED")
        {
            stats.detectCount = stoul(value);
        }
        else if (field == "PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED")
        {
            stats.restoreCount = stoul(value);
        }
        else if (field == "PFC_WD_STATUS")
        {
            stats.operational = (value == "operational");
        }
        else if (field == "PFC_WD_QUEUE_STATS_TX_PACKETS")
        {
            stats.txPkt = stoul(value);
        }
        else if (field == "PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS")
        {
            stats.txDropPkt = stoul(value);
        }
        else if (field == "PFC_WD_QUEUE_STATS_RX_PACKETS")
        {
            stats.rxPkt = stoul(value);
        }
        else if (field == "PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS")
        {
            stats.rxDropPkt = stoul(value);
        }
        else if (field == "PFC_WD_QUEUE_STATS_TX_PACKETS_LAST")
        {
            stats.txPktLast = stoul(value);
        }
        else if (field == "PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS_LAST")
        {
            stats.txDropPktLast = stoul(value);
        }
        else if (field == "PFC_WD_QUEUE_STATS_RX_PACKETS_LAST")
        {
            stats.rxPktLast = stoul(value);
        }
        else if (field == "PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS_LAST")
        {
            stats.rxDropPktLast = stoul(value);
        }
    }

    return stats;
}

void PfcWdHwOrch::updateQueueStats(const string &queueIdStr, const PfcWdQueueStats &stats)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> resultFvValues;

    resultFvValues.emplace_back("PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED", to_string(stats.detectCount));
    resultFvValues.emplace_back("PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED", to_string(stats.restoreCount));

    resultFvValues.emplace_back("PFC_WD_QUEUE_STATS_TX_PACKETS", to_string(stats.txPkt));
    resultFvValues.emplace_back("PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS", to_string(stats.txDropPkt));
    resultFvValues.emplace_back("PFC_WD_QUEUE_STATS_RX_PACKETS", to_string(stats.rxPkt));
    resultFvValues.emplace_back("PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS", to_string(stats.rxDropPkt));

    resultFvValues.emplace_back("PFC_WD_QUEUE_STATS_TX_PACKETS_LAST", to_string(stats.txPktLast));
    resultFvValues.emplace_back("PFC_WD_QUEUE_STATS_TX_DROPPED_PACKETS_LAST", to_string(stats.txDropPktLast));
    resultFvValues.emplace_back("PFC_WD_QUEUE_STATS_RX_PACKETS_LAST", to_string(stats.rxPktLast));
    resultFvValues.emplace_back("PFC_WD_QUEUE_STATS_RX_DROPPED_PACKETS_LAST", to_string(stats.rxDropPktLast));

    resultFvValues.emplace_back("PFC_WD_STATUS", stats.operational ? "operational" : "stormed");

    this->getCountersTable()->set(queueIdStr, resultFvValues);
}





bool PfcWdHwOrch::determineTimerGranularity(uint32_t timeValue, uint32_t hwMin, uint32_t hwMax, uint32_t& granularity)
{
    SWSS_LOG_ENTER();

    // Fixed granularity options: 1ms, 10ms, 100ms
    // TODO: Query supported granularities from hardware
    const vector<uint32_t> granularities = {1, 10, 100};

    // Find granularity where timeValue fits in range [gran*1, gran*MAX_TICK_COUNT]
    for (uint32_t gran : granularities)
    {
        uint32_t minTime = gran * 1;
        uint32_t maxTime = gran * MAX_TICK_COUNT;

        // Skip if not in hardware range
        if (gran < hwMin || minTime > hwMax)
        {
            SWSS_LOG_DEBUG("Skipping granularity %u ms (not in hardware range [%u, %u] ms)",
                          gran, hwMin, hwMax);
            continue;
        }

        if (timeValue < minTime)
        {
            SWSS_LOG_ERROR("Time value %u ms is less than minimum supported time %u ms for granularity %u ms",
                          timeValue, minTime, gran);
            return false;
        }

        if (timeValue <= maxTime)
        {
            granularity = gran;
            uint32_t ticks = (timeValue + gran - 1) / gran; // Round up to nearest tick
            SWSS_LOG_DEBUG("Time %u ms can be represented with granularity %u ms (range: %u-%u ms, ticks: %u)",
                          timeValue, gran, minTime, maxTime, ticks);
            return true;
        }
    }

    SWSS_LOG_ERROR("Time value %u ms exceeds maximum supported time %u ms",
                  timeValue, 100 * MAX_TICK_COUNT);
    return false;
}

task_process_status PfcWdHwOrch::createEntry(const string& key, const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();

    // GLOBAL configuration not supported for hardware watchdog
    if (key == PFC_WD_GLOBAL)
    {
        SWSS_LOG_WARN("GLOBAL configuration is not supported for hardware-based PFC watchdog");

        for (const auto& field : data)
        {
            SWSS_LOG_WARN("Field '%s' with value '%s' is not supported for hardware-based PFC watchdog",
                         fvField(field).c_str(), fvValue(field).c_str());
        }

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

                // Check detection time is within supported range
                if (m_detectionTimeMin > 0 && m_detectionTimeMax > 0)
                {
                    if (detectionTime < m_detectionTimeMin)
                    {
                        SWSS_LOG_ERROR("Detection time %u ms is below minimum supported value %u ms on port %s",
                                      detectionTime, m_detectionTimeMin, key.c_str());
                        return task_process_status::task_invalid_entry;
                    }

                    if (detectionTime > m_detectionTimeMax)
                    {
                        SWSS_LOG_ERROR("Detection time %u ms exceeds maximum supported value %u ms on port %s",
                                      detectionTime, m_detectionTimeMax, key.c_str());
                        return task_process_status::task_invalid_entry;
                    }
                }
            }
            else if (field == PFC_WD_RESTORATION_TIME)
            {
                restorationTime = static_cast<uint32_t>(stoul(value));

                // Check restoration time is within supported range
                if (m_restorationTimeMin > 0 && m_restorationTimeMax > 0)
                {
                    if (restorationTime < m_restorationTimeMin)
                    {
                        SWSS_LOG_ERROR("Restoration time %u ms is below minimum supported value %u ms on port %s",
                                      restorationTime, m_restorationTimeMin, key.c_str());
                        return task_process_status::task_invalid_entry;
                    }

                    if (restorationTime > m_restorationTimeMax)
                    {
                        SWSS_LOG_ERROR("Restoration time %u ms exceeds maximum supported value %u ms on port %s",
                                      restorationTime, m_restorationTimeMax, key.c_str());
                        return task_process_status::task_invalid_entry;
                    }
                }
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
    if (pfcStatHistory != "enable" && pfcStatHistory != "disable")
    {
        SWSS_LOG_ERROR("%s is invalid value for %s", pfcStatHistory.c_str(), PFC_STAT_HISTORY);
        return task_process_status::task_invalid_entry;
    }

    // All ports must use the same switch-level PFC DLR packet action.
    // Action can only change when no ports are configured, or when
    // reconfiguring the single existing port.
    PfcWdAction currentAction = this->getPfcDlrPacketAction();

    bool isSinglePortReconfiguration = (m_hwWdPorts.size() == 1 &&
                                        m_hwWdPorts.find(port.m_alias) != m_hwWdPorts.end());

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
    if (m_hwWdPorts.find(port.m_alias) != m_hwWdPorts.end())
    {
        if (isPortInStormedState(port))
        {
            SWSS_LOG_ERROR("Cannot modify PFC watchdog configuration on port %s: port is in stormed state. "
                          "Wait for storm to pass before making changes.",
                          port.m_alias.c_str());
            return task_process_status::task_invalid_entry;
        }
    }

    // Hardware watchdog doesn't support in-place updates, so stop
    // existing configuration before applying new one
    SWSS_LOG_INFO("Attempting to disable any existing hardware PFC watchdog on port %s before applying new configuration",
                  port.m_alias.c_str());
    stopWdOnPort(port);

    if (!startWdOnPort(port, detectionTime, restorationTime, action, pfcStatHistory))
    {
        SWSS_LOG_ERROR("Failed to start PFC Watchdog on port %s", port.m_alias.c_str());
        return task_process_status::task_need_retry;
    }

    SWSS_LOG_NOTICE("Started PFC Watchdog on port %s", port.m_alias.c_str());
    // Port is tracked in m_hwWdPorts by configureHwWatchdog
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
	if (m_hwWdPorts.find(port.m_alias) != m_hwWdPorts.end())
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
	return PfcWdBaseOrch::deleteEntry(key);
}

bool PfcWdHwOrch::startWdOnPort(const Port& port,
	    uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action, string pfcStatHistory)
{
	SWSS_LOG_ENTER();

	// For hardware-based watchdog, all hardware programming and flex counter
	// registration are handled in configureHwWatchdog()/initializeQueueStats().
	// Any existing configuration is cleaned up via stopWdOnPort() before this
	// function is invoked from createEntry().
	return configureHwWatchdog(port, detectionTime, restorationTime, action);
}

bool PfcWdHwOrch::stopWdOnPort(const Port& port)
{
    SWSS_LOG_ENTER();

    return disableHwWatchdog(port);
}

void PfcWdHwOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    // Update counters for queues in storm
    for (auto& entry : m_queueBaselineStats)
    {
        sai_object_id_t queueId = entry.first;
        auto it = m_queueToPortMap.find(queueId);
        if (it != m_queueToPortMap.end())
        {
            string queueIdStr = sai_serialize_object_id(queueId);
            updateQueueCounters(queueIdStr, queueId, it->second.queue_index, true);  // true = periodic
            SWSS_LOG_DEBUG("Periodic counter update for queue 0x%" PRIx64, queueId);
        }
        else
        {
            SWSS_LOG_WARN("Queue 0x%" PRIx64 " has baseline stats but no port mapping", queueId);
        }
    }
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

bool PfcWdHwOrch::configureHwWatchdog(const Port& port, uint32_t detectionTime,
                                      uint32_t restorationTime, PfcWdAction action)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("Configuring hardware watchdog on port %s: detection=%ums, restoration=%ums, action=%s",
                    port.m_alias.c_str(), detectionTime, restorationTime, serializeAction(action).c_str());

    // Validate port has lossless TCs before configuring hardware
    set<uint8_t> losslessTc;
    if (!getLosslessTcsForPort(port, losslessTc))
    {
        SWSS_LOG_NOTICE("No lossless TC found on port %s", port.m_alias.c_str());
        writeFailureStatus(port);
        return false;
    }

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

    // Configure timer granularity if supported
    uint32_t timerGranularity = 0;
    if (!configureTimerGranularity(port, detectionTime, timerGranularity, handleFailure))
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

    // Initialize queue statistics in COUNTERS_DB
    initializeQueueStats(port, losslessTc);

    // Track this port
    m_hwWdPorts.insert(port.m_alias);

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

    // Add granularity field if port-level granularity is supported
    if (m_portLevelGranularitySupported && timerGranularity > 0)
    {
        fvs.emplace_back("detection_granularity", to_string(timerGranularity));
    }

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

bool PfcWdHwOrch::configureTimerGranularity(const Port& port, uint32_t detectionTime,
                                            uint32_t& timerGranularity,
                                            const function<bool(const string&)>& handleFailure)
{
    SWSS_LOG_ENTER();

    // SAI_PORT_ATTR_PFC_TC_DLD_TIMER_INTERVAL only applies to detection timer
    if (!m_portLevelGranularitySupported)
    {
        SWSS_LOG_INFO("Port-level granularity not supported on port %s, ASIC will configure best available granularity",
                     port.m_alias.c_str());
        timerGranularity = 0;
        return true;
    }

    // Find granularity that supports the detection time
    if (!determineTimerGranularity(detectionTime, m_detectionTimeMin, m_detectionTimeMax, timerGranularity))
    {
        return handleFailure("Failed to determine suitable timer granularity for detection time " +
                           to_string(detectionTime) + " ms on port " + port.m_alias);
    }

    SWSS_LOG_INFO("Using timer granularity %u ms for detection time %u ms on port %s",
                 timerGranularity, detectionTime, port.m_alias.c_str());

    // Build map with granularity for all TCs (0-7)
    std::vector<sai_map_t> granularity_map_list;
    for (uint8_t tc = 0; tc < PFC_WD_TC_MAX; tc++)
    {
        sai_map_t gran_map;
        gran_map.key = tc;
        gran_map.value = timerGranularity;
        granularity_map_list.push_back(gran_map);
    }

    sai_attribute_t attr_granularity;
    attr_granularity.id = SAI_PORT_ATTR_PFC_TC_DLD_TIMER_INTERVAL;
    attr_granularity.value.maplist.count = static_cast<uint32_t>(granularity_map_list.size());
    attr_granularity.value.maplist.list = granularity_map_list.data();

    sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr_granularity);
    if (status != SAI_STATUS_SUCCESS)
    {
        return handleFailure("Failed to set PFC DLD timer granularity to " + to_string(timerGranularity) +
                           " ms on port " + port.m_alias + ": " + to_string(status));
    }

    SWSS_LOG_NOTICE("Set PFC DLD timer granularity to %u ms for detection timer on port %s",
                   timerGranularity, port.m_alias.c_str());

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

void PfcWdHwOrch::initializeQueueStats(const Port& port, const set<uint8_t>& losslessTc)
{
    SWSS_LOG_ENTER();

    // For each lossless queue, register PFC_WD FlexCounters, initialize
    // watchdog counters, and populate the queue→port mapping.
    for (auto tc : losslessTc)
    {
        if (tc >= port.m_queue_ids.size())
        {
            continue;
        }

        sai_object_id_t queueId = port.m_queue_ids[tc];
        string queueIdStr = sai_serialize_object_id(queueId);

        // Register queue stats/attributes in the dedicated PFC_WD FlexCounter group.
        if (m_pfcwdFlexCounterManager)
        {
            if (!c_queueStatIds.empty())
            {
                auto queueStatIdSet = PfcWdBaseOrch::counterIdsToStr(c_queueStatIds, sai_serialize_queue_stat);
                if (queueStatIdSet.empty())
                {
                    SWSS_LOG_WARN("Failed to convert queue stat IDs for queue 0x%" PRIx64 " on port %s",
                                  queueId, port.m_alias.c_str());
                }
                else
                {
                    m_pfcwdFlexCounterManager->setCounterIdList(queueId,
                                                                 CounterType::QUEUE,
                                                                 queueStatIdSet,
                                                                 SAI_OBJECT_TYPE_QUEUE);
                    SWSS_LOG_DEBUG("Registered %zu queue stats for queue 0x%" PRIx64 " on port %s",
                                   queueStatIdSet.size(), queueId, port.m_alias.c_str());
                }
            }

            if (!c_queueAttrIds.empty())
            {
                auto queueAttrIdSet = PfcWdBaseOrch::counterIdsToStr(c_queueAttrIds, sai_serialize_queue_attr);
                if (queueAttrIdSet.empty())
                {
                    SWSS_LOG_WARN("Failed to convert queue attr IDs for queue 0x%" PRIx64 " on port %s",
                                  queueId, port.m_alias.c_str());
                }
                else
                {
                    auto *fcMgr = dynamic_cast<FlexCounterManager*>(m_pfcwdFlexCounterManager.get());
                    if (fcMgr)
                    {
                        fcMgr->setCounterIdList(queueId,
                                                CounterType::QUEUE_ATTR,
                                                queueAttrIdSet);
                        SWSS_LOG_DEBUG("Registered %zu queue attrs for queue 0x%" PRIx64 " on port %s",
                                       queueAttrIdSet.size(), queueId, port.m_alias.c_str());
                    }
                    else
                    {
                        SWSS_LOG_WARN("PFC WD FlexCounter manager is not FlexCounterManager for queue 0x%" PRIx64 " on port %s",
                                      queueId, port.m_alias.c_str());
                    }
                }
            }
        }

        // Add to queue→port mapping for fast lookup in the notification callback.
        PortQueueInfo info;
        info.port_id = port.m_port_id;
        info.port_alias = port.m_alias;
        info.queue_index = tc;
        m_queueToPortMap[queueId] = info;

        // Initialize PFC WD counters in COUNTERS_DB (status operational, packet counters cleared).
        // Preserve existing detect/restore counts from warm-reboot if present.
        auto stats = getQueueStats(queueIdStr);
        stats.operational = true;
        stats.txPkt = 0;
        stats.txDropPkt = 0;
        stats.rxPkt = 0;
        stats.rxDropPkt = 0;
        stats.txPktLast = 0;
        stats.txDropPktLast = 0;
        stats.rxPktLast = 0;
        stats.rxDropPktLast = 0;
        updateQueueStats(queueIdStr, stats);

        // Baseline stats will be created on-demand when storm is detected.

        SWSS_LOG_NOTICE("Initialized PFC watchdog for queue 0x%" PRIx64 " on port %s TC %d",
                       queueId, port.m_alias.c_str(), tc);
    }
}

bool PfcWdHwOrch::disableHwWatchdog(const Port& port)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("Disabling hardware watchdog on port %s", port.m_alias.c_str());

    // Get lossless TCs for this port
    std::set<uint8_t> losslessTc;
    if (!getLosslessTcsForPort(port, losslessTc))
    {
        SWSS_LOG_NOTICE("No lossless TC found on port %s", port.m_alias.c_str());
        return true;  // Nothing to disable
    }

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

        // Clear this queue's registration in the dedicated PFC_WD FlexCounter group.
        if (m_pfcwdFlexCounterManager)
        {
            m_pfcwdFlexCounterManager->clearCounterIdList(queueId, SAI_OBJECT_TYPE_QUEUE);
            SWSS_LOG_DEBUG("Cleared FlexCounter registration for queue 0x%" PRIx64 " on port %s",
                           queueId, port.m_alias.c_str());
        }

        // Remove from queue→port mapping
        m_queueToPortMap.erase(queueId);

        // Remove baseline stats
        m_queueBaselineStats.erase(queueId);
        SWSS_LOG_DEBUG("Removed baseline stats for queue 0x%" PRIx64, queueId);
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
    m_hwWdPorts.erase(port.m_alias);

    // Remove entry from STATE_DB
    m_pfcWdHwStateTable->del(port.m_alias);

    // If no ports have hardware watchdog configured, reset action to unknown
    if (m_hwWdPorts.empty())
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
        string queueIdStr = sai_serialize_object_id(queueId);

        // Get queue statistics
        auto stats = getQueueStats(queueIdStr);

        // If operational is false, the queue is in stormed state
        if (!stats.operational)
        {
            SWSS_LOG_WARN("Port %s has queue %d (0x%" PRIx64 ") in stormed state",
                         port.m_alias.c_str(), i, queueId);
            return true;
        }
    }

    return false;
}

bool PfcWdHwOrch::readHwCounters(sai_object_id_t queueId, uint8_t queueIndex, PfcWdHwStats& counters)
{
    SWSS_LOG_ENTER();

    // For HW PFC watchdog, read queue/PG counters from COUNTERS_DB (via FlexCounter)
    // instead of querying SAI directly.

    string queueIdStr = sai_serialize_object_id(queueId);
    vector<FieldValueTuple> fieldValues;

    auto countersTable = getCountersTable();
    if (!countersTable || !countersTable->get(queueIdStr, fieldValues))
    {
        SWSS_LOG_DEBUG("No counter entry found for queue 0x%" PRIx64, queueId);
        memset(&counters, 0, sizeof(PfcWdHwStats));
        return true;
    }

    // Initialize to zero
    counters.txPkt = 0;
    counters.txDropPkt = 0;
    counters.rxPkt = 0;
    counters.rxDropPkt = 0;

    // Read TX counters (queue stats)
    for (const auto& fv : fieldValues)
    {
        const auto field = fvField(fv);
        const auto value = fvValue(fv);

        if (field == "SAI_QUEUE_STAT_PACKETS")
        {
            counters.txPkt = stoull(value);
        }
        else if (field == "SAI_QUEUE_STAT_DROPPED_PACKETS")
        {
            counters.txDropPkt = stoull(value);
        }
    }

    // Read RX counters from the priority group mapped to this queue.
    Port portInstance;
    auto it = m_queueToPortMap.find(queueId);
    if (it == m_queueToPortMap.end())
    {
        SWSS_LOG_ERROR("Queue 0x%" PRIx64 " not found in queue-to-port map", queueId);
        return false;
    }

    if (!gPortsOrch->getPort(it->second.port_id, portInstance))
    {
        SWSS_LOG_ERROR("Cannot get port by ID 0x%" PRIx64, it->second.port_id);
        return false;
    }

    if (queueIndex >= portInstance.m_priority_group_ids.size())
    {
        SWSS_LOG_ERROR("Invalid queue index %u for port 0x%" PRIx64, queueIndex, it->second.port_id);
        return false;
    }

    sai_object_id_t pg = portInstance.m_priority_group_ids[static_cast<size_t>(queueIndex)];
    string pgIdStr = sai_serialize_object_id(pg);

    fieldValues.clear();
    if (countersTable->get(pgIdStr, fieldValues))
    {
        for (const auto& fv : fieldValues)
        {
            const auto field = fvField(fv);
            const auto value = fvValue(fv);

            if (field == "SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS")
            {
                counters.rxPkt = stoull(value);
            }
            else if (field == "SAI_INGRESS_PRIORITY_GROUP_STAT_DROPPED_PACKETS")
            {
                counters.rxDropPkt = stoull(value);
            }
        }
    }

    SWSS_LOG_DEBUG("Read HW counters for queue 0x%" PRIx64 ": txPkt=%" PRIu64 ", txDropPkt=%" PRIu64 ", rxPkt=%" PRIu64 ", rxDropPkt=%" PRIu64,
                   queueId, counters.txPkt, counters.txDropPkt, counters.rxPkt, counters.rxDropPkt);

    return true;
}

void PfcWdHwOrch::initQueueCounters(const string& queueIdStr, sai_object_id_t queueId, uint8_t queueIndex)
{
    SWSS_LOG_ENTER();

    PfcWdHwStats hwStats;
    if (!readHwCounters(queueId, queueIndex, hwStats))
    {
        return;
    }

    // Read current stats from COUNTERS_DB
    auto wdQueueStats = getQueueStats(queueIdStr);

    // Only bump detectCount for a new storm; if storm persisted across
    // warm-reboot (detectCount > restoreCount), keep counters as-is.
    if (!(wdQueueStats.detectCount > wdQueueStats.restoreCount))
    {
        wdQueueStats.detectCount++;
        wdQueueStats.txPktLast = 0;
        wdQueueStats.txDropPktLast = 0;
        wdQueueStats.rxPktLast = 0;
        wdQueueStats.rxDropPktLast = 0;
    }
    wdQueueStats.operational = false;

    // Store baseline for delta calculation
    m_queueBaselineStats[queueId] = hwStats;

    // Write to COUNTERS_DB
    updateQueueStats(queueIdStr, wdQueueStats);
}

void PfcWdHwOrch::updateQueueCounters(const string& queueIdStr, sai_object_id_t queueId,
                                      uint8_t queueIndex, bool periodic)
{
    SWSS_LOG_ENTER();

    PfcWdHwStats hwStats;
    if (!readHwCounters(queueId, queueIndex, hwStats))
    {
        return;
    }

    auto finalStats = getQueueStats(queueIdStr);

    if (!periodic)
    {
        finalStats.restoreCount++;
    }
    finalStats.operational = !periodic;

    // Get baseline (stored in initQueueCounters or previous update)
    auto it = m_queueBaselineStats.find(queueId);
    if (it == m_queueBaselineStats.end())
    {
        SWSS_LOG_WARN("No baseline stats found for queue 0x%" PRIx64 ", initializing", queueId);
        m_queueBaselineStats[queueId] = hwStats;
        updateQueueStats(queueIdStr, finalStats);
        return;
    }

    auto& baseline = it->second;

    // Calculate deltas with underflow protection
    // If hardware counters are less than baseline, it means counters were reset
    // In this case, skip the update to avoid huge negative values
    if (hwStats.txPkt >= baseline.txPkt)
    {
        finalStats.txPktLast += hwStats.txPkt - baseline.txPkt;
        finalStats.txPkt += hwStats.txPkt - baseline.txPkt;
    }
    else
    {
        SWSS_LOG_WARN("Counter reset detected for queue 0x%" PRIx64 ": txPkt went from %" PRIu64 " to %" PRIu64,
                     queueId, baseline.txPkt, hwStats.txPkt);
    }

    if (hwStats.txDropPkt >= baseline.txDropPkt)
    {
        finalStats.txDropPktLast += hwStats.txDropPkt - baseline.txDropPkt;
        finalStats.txDropPkt += hwStats.txDropPkt - baseline.txDropPkt;
    }
    else
    {
        SWSS_LOG_WARN("Counter reset detected for queue 0x%" PRIx64 ": txDropPkt went from %" PRIu64 " to %" PRIu64,
                     queueId, baseline.txDropPkt, hwStats.txDropPkt);
    }

    if (hwStats.rxPkt >= baseline.rxPkt)
    {
        finalStats.rxPktLast += hwStats.rxPkt - baseline.rxPkt;
        finalStats.rxPkt += hwStats.rxPkt - baseline.rxPkt;
    }
    else
    {
        SWSS_LOG_WARN("Counter reset detected for queue 0x%" PRIx64 ": rxPkt went from %" PRIu64 " to %" PRIu64,
                     queueId, baseline.rxPkt, hwStats.rxPkt);
    }

    if (hwStats.rxDropPkt >= baseline.rxDropPkt)
    {
        finalStats.rxDropPktLast += hwStats.rxDropPkt - baseline.rxDropPkt;
        finalStats.rxDropPkt += hwStats.rxDropPkt - baseline.rxDropPkt;
    }
    else
    {
        SWSS_LOG_WARN("Counter reset detected for queue 0x%" PRIx64 ": rxDropPkt went from %" PRIu64 " to %" PRIu64,
                     queueId, baseline.rxDropPkt, hwStats.rxDropPkt);
    }

    // Update baseline
    baseline = hwStats;

    // Write to COUNTERS_DB
    updateQueueStats(queueIdStr, finalStats);
}
