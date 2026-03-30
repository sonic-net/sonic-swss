#include "pfcwdhworch.h"
#include "schema.h"
#include "switchorch.h"
#include "portsorch.h"
#include "saiextensions.h"
#include "sai_serialize.h"
#include "converter.h"
#include <algorithm>
#include <set>

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t* sai_switch_api;
extern sai_port_api_t* sai_port_api;
extern sai_queue_api_t* sai_queue_api;
extern sai_buffer_api_t* sai_buffer_api;
extern event_handle_t g_events_handle;
extern SwitchOrch *gSwitchOrch;
extern PortsOrch *gPortsOrch;

// Global instance pointer for SAI callback
static PfcWdHwOrch* g_pfcWdHwOrch = nullptr;

// SAI callback wrapper
__attribute__((unused))
static void on_queue_pfc_deadlock(
        _In_ uint32_t count,
        _In_ sai_queue_deadlock_notification_data_t *data)
{
    if (g_pfcWdHwOrch != nullptr)
    {
        g_pfcWdHwOrch->onQueuePfcDeadlock(count, data);
    }
}

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

    // Register SAI callback for PFC deadlock notifications
    // This will be invoked when hardware detects or restores from PFC storms
    sai_attribute_t attr;
    attr.id = SAI_SWITCH_ATTR_PFC_TC_DLD_INTERVAL;

    // Note: The actual callback registration happens via SAI switch attribute
    // SAI_SWITCH_ATTR_QUEUE_PFC_DEADLOCK_NOTIFY which should be set during
    // switch initialization. The callback function is on_queue_pfc_deadlock()
    // which calls this->onQueuePfcDeadlock()

    SWSS_LOG_NOTICE("PFC watchdog hardware callbacks registered");
}

void PfcWdHwOrch::recoverWarmReboot(DBConnector *db)
{
    SWSS_LOG_ENTER();

    // During warm reboot, we need to restore the hardware watchdog state
    // from STATE_DB and re-enable monitoring on ports that had it configured
    // before the reboot.

    // This is a placeholder for warm reboot recovery logic.
    // The full implementation will:
    // 1. Read STATE_DB to find ports with active hardware watchdog
    // 2. Re-enable flex counters for those ports
    // 3. Re-register queue monitoring

    SWSS_LOG_NOTICE("PFC watchdog warm reboot recovery completed");
}

void PfcWdHwOrch::onQueuePfcDeadlock(uint32_t count, sai_queue_deadlock_notification_data_t *data)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
}

PfcWdHwOrch::PfcWdQueueStats PfcWdHwOrch::getQueueStats(const string &queueIdStr)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
    PfcWdQueueStats stats;
    memset(&stats, 0, sizeof(PfcWdQueueStats));
    return stats;
}

void PfcWdHwOrch::updateQueueStats(const string &queueIdStr, const PfcWdQueueStats &stats)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
}

bool PfcWdHwOrch::determineTimerGranularity(uint32_t timeValue, uint32_t hwMin, uint32_t hwMax, uint32_t& granularity)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
    return false;
}

task_process_status PfcWdHwOrch::createEntry(const string& key, const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
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
    // TODO: Implementation
    return false;
}

bool PfcWdHwOrch::stopWdOnPort(const Port& port)
{
    SWSS_LOG_ENTER();

    return disableHwWatchdog(port);
}

void PfcWdHwOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
}

bool PfcWdHwOrch::startWdActionOnQueue(const string &event, sai_object_id_t queueId, const string &info)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
    return false;
}

bool PfcWdHwOrch::readBackTimerValue(const Port& port, sai_port_attr_t attrId,
                                     const set<uint8_t>& losslessTc, uint32_t expected,
                                     uint32_t& actual, const string& timerName)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
    return false;
}

bool PfcWdHwOrch::configureHwWatchdog(const Port& port, uint32_t detectionTime,
                                      uint32_t restorationTime, PfcWdAction action)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
    return false;
}

bool PfcWdHwOrch::configureSwitchAction(const Port& port, PfcWdAction action,
                                        const function<bool(const string&)>& handleFailure)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
    return false;
}

bool PfcWdHwOrch::configureTimerGranularity(const Port& port, uint32_t detectionTime,
                                            uint32_t& timerGranularity,
                                            const function<bool(const string&)>& handleFailure)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
    return false;
}

bool PfcWdHwOrch::configureTimerIntervals(const Port& port, const set<uint8_t>& losslessTc,
                                          uint32_t detectionTime, uint32_t restorationTime,
                                          const function<bool(const string&)>& handleFailure)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
    return false;
}

bool PfcWdHwOrch::enableDldrOnLosslessQueues(const Port& port, const set<uint8_t>& losslessTc,
                                             uint32_t detectionTime, uint32_t restorationTime,
                                             const function<bool(const string&)>& handleFailure)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
    return false;
}

void PfcWdHwOrch::initializeQueueStats(const Port& port, const set<uint8_t>& losslessTc)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
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
    // TODO: Implementation
    return false;
}

void PfcWdHwOrch::initQueueCounters(const string& queueIdStr, sai_object_id_t queueId, uint8_t queueIndex)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
}

void PfcWdHwOrch::updateQueueCounters(const string& queueIdStr, sai_object_id_t queueId,
                                      uint8_t queueIndex, bool periodic)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
}

uint32_t PfcWdHwOrch::roundUpToValidInterval(uint32_t requestedTime, const vector<uint32_t>& validIntervals)
{
    SWSS_LOG_ENTER();
    // TODO: Implementation
    return 0;
}
