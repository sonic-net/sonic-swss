#include "pfcwdhworch.h"
#include "schema.h"
#include "switchorch.h"
#include "portsorch.h"
#include "saiextensions.h"
#include "sai_serialize.h"
#include <algorithm>
#include <set>

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
    // TODO: Implementation
}

PfcWdHwOrch::~PfcWdHwOrch(void)
{
    // TODO: Implementation
}

void PfcWdHwOrch::onQueuePfcDeadlock(uint32_t count, sai_queue_deadlock_notification_data_t *data)
{
    // TODO: Implementation
}

task_process_status PfcWdHwOrch::createEntry(const string& key, const vector<FieldValueTuple>& data)
{
    // TODO: Implementation
    SWSS_LOG_ERROR("Hardware PFC watchdog is not implemented, ignoring config on %s", key.c_str());
    return task_process_status::task_invalid_entry;
}

task_process_status PfcWdHwOrch::deleteEntry(const string& key)
{
    // TODO: Implementation
    SWSS_LOG_ERROR("Hardware PFC watchdog is not implemented, ignoring delete on %s", key.c_str());
    return task_process_status::task_invalid_entry;
}

bool PfcWdHwOrch::startWdOnPort(const Port& port,
        uint32_t detectionTime, uint32_t restorationTime, PfcWdAction action, string pfcStatHistory)
{
    // TODO: Implementation
    return false;
}

bool PfcWdHwOrch::stopWdOnPort(const Port& port)
{
    // TODO: Implementation
    return false;
}

void PfcWdHwOrch::doTask(SelectableTimer &timer)
{
    // TODO: Implementation
}

bool PfcWdHwOrch::startWdActionOnQueue(const string &event, sai_object_id_t queueId, const string &info)
{
    // TODO: Implementation
    return false;
}
