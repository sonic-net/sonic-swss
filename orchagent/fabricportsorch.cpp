#include "fabricportsorch.h"

#include <inttypes.h>
#include <fstream>
#include <sstream>
#include <tuple>

#include "logger.h"
#include "schema.h"
#include "sai_serialize.h"
#include "timer.h"

#define FABRIC_POLLING_INTERVAL_DEFAULT   (30)
#define FABRIC_PORT_ERROR     0
#define FABRIC_PORT_SUCCESS   1
#define FABRIC_PORT_STAT_COUNTER_FLEX_COUNTER_GROUP         "FABRIC_PORT_STAT_COUNTER"
#define FABRIC_PORT_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS   10000
#define FABRIC_QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP        "FABRIC_QUEUE_STAT_COUNTER"
#define FABRIC_QUEUE_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS  100000
#define FABRIC_PORT_TABLE "FABRIC_PORT_TABLE"

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t *sai_switch_api;
extern sai_port_api_t *sai_port_api;

const vector<sai_port_stat_t> port_stat_ids =
{
    SAI_PORT_STAT_IF_IN_OCTETS,
    SAI_PORT_STAT_IF_IN_ERRORS,
    SAI_PORT_STAT_IF_IN_FABRIC_DATA_UNITS,
    SAI_PORT_STAT_IF_IN_FEC_CORRECTABLE_FRAMES,
    SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES,
    SAI_PORT_STAT_IF_IN_FEC_SYMBOL_ERRORS,
    SAI_PORT_STAT_IF_OUT_OCTETS,
    SAI_PORT_STAT_IF_OUT_FABRIC_DATA_UNITS,
};

static const vector<sai_queue_stat_t> queue_stat_ids =
{
    SAI_QUEUE_STAT_WATERMARK_LEVEL,
    SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES,
    SAI_QUEUE_STAT_CURR_OCCUPANCY_LEVEL,
};

FabricPortsOrch::FabricPortsOrch(DBConnector *appl_db, vector<table_name_with_pri_t> &tableNames) :
        Orch(appl_db, tableNames),
        port_stat_manager(FABRIC_PORT_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ,
                          FABRIC_PORT_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, true),
        queue_stat_manager(FABRIC_QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ,
                           FABRIC_QUEUE_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, true),
        m_timer(new SelectableTimer(timespec { .tv_sec = FABRIC_POLLING_INTERVAL_DEFAULT, .tv_nsec = 0 }))
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE( "FabricPortsOrch constructor" );

    m_state_db = shared_ptr<DBConnector>(new DBConnector("STATE_DB", 0));
    m_stateTable = unique_ptr<Table>(new Table(m_state_db.get(), FABRIC_PORT_TABLE));

    m_counter_db = shared_ptr<DBConnector>(new DBConnector("COUNTERS_DB", 0));
    m_laneQueueCounterTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_QUEUE_NAME_MAP));
    m_lanePortCounterTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_QUEUE_PORT_MAP));

    m_flex_db = shared_ptr<DBConnector>(new DBConnector("FLEX_COUNTER_DB", 0));
    m_flexCounterTable = unique_ptr<ProducerTable>(new ProducerTable(m_flex_db.get(), FABRIC_PORT_TABLE));

    getFabricPortList();

    auto executor = new ExecutableTimer(m_timer, this, "FABRIC_POLL");
    Orch::addExecutor(executor);
    m_timer->start();
}

int FabricPortsOrch::getFabricPortList()
{
    SWSS_LOG_ENTER();

    if (m_getFabricPortListDone) {
        return FABRIC_PORT_SUCCESS;
    }

    uint32_t i;
    sai_status_t status;
    sai_attribute_t attr;

    attr.id = SAI_SWITCH_ATTR_NUMBER_OF_FABRIC_PORTS;
    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get fabric port number, rv:%d", status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_SWITCH, status);
        if (handle_status != task_process_status::task_success)
        {
            return FABRIC_PORT_ERROR;
        }
    }
    m_fabricPortCount = attr.value.u32;
    SWSS_LOG_NOTICE("Get %d fabric ports", m_fabricPortCount);

    vector<sai_object_id_t> fabric_port_list;
    fabric_port_list.resize(m_fabricPortCount);
    attr.id = SAI_SWITCH_ATTR_FABRIC_PORT_LIST;
    attr.value.objlist.count = (uint32_t)fabric_port_list.size();
    attr.value.objlist.list = fabric_port_list.data();
    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        task_process_status handle_status = handleSaiGetStatus(SAI_API_SWITCH, status);
        if (handle_status != task_process_status::task_success)
        {
            throw runtime_error("FabricPortsOrch get port list failure");
        }
    }

    for (i = 0; i < m_fabricPortCount; i++)
    {
        sai_uint32_t lanes[1] = { 0 };
        attr.id = SAI_PORT_ATTR_HW_LANE_LIST;
        attr.value.u32list.count = 1;
        attr.value.u32list.list = lanes;
        status = sai_port_api->get_port_attribute(fabric_port_list[i], 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
            if (handle_status != task_process_status::task_success)
            {
                throw runtime_error("FabricPortsOrch get port lane failure");
            }
        }
        int lane = attr.value.u32list.list[0];
        m_fabricLanePortMap[lane] = fabric_port_list[i];
    }

    generatePortStats();

    m_getFabricPortListDone = true;

    updateFabricPortState();

    return FABRIC_PORT_SUCCESS;
}

bool FabricPortsOrch::allPortsReady()
{
    return m_getFabricPortListDone;
}

void FabricPortsOrch::generatePortStats()
{
    // FIX_ME: This function installs flex counters for port stats
    // on fabric ports for fabric asics and voq asics (that connect
    // to fabric asics via fabric ports). These counters will be
    // installed in FLEX_COUNTER_DB, and queried by syncd and updated
    // to COUNTERS_DB.
    // However, currently BCM SAI doesn't update its code to query
    // port stats (metrics in list port_stat_ids) yet.
    // Also, BCM sets too low value for "Max logical port count" (256),
    // causing syncd to crash on voq asics that now include regular front
    // panel ports, fabric ports, and multiple logical ports.
    // So, this function will just do nothing for now, and we will readd
    // code to install port stats counters when BCM completely supports.
}

void FabricPortsOrch::generateQueueStats()
{
    if (m_isQueueStatsGenerated) return;
    if (!m_getFabricPortListDone) return;

    // FIX_ME: Similar to generatePortStats(), generateQueueStats() installs
    // flex counters for queue stats on fabric ports for fabric asics and voq asics.
    // However, currently BCM SAI doesn't fully support queue stats query.
    // Query on queue type and index is not supported for fabric asics while
    // voq asics are not completely supported.
    // So, this function will just do nothing for now, and we will readd
    // code to install queue stats counters when BCM completely supports.

    m_isQueueStatsGenerated = true;
}

void FabricPortsOrch::updateFabricPortState()
{
    if (!m_getFabricPortListDone) return;

    SWSS_LOG_ENTER();

    sai_status_t status;
    sai_attribute_t attr;

    time_t now;
    struct timespec time_now;
    if (clock_gettime(CLOCK_MONOTONIC, &time_now) < 0)
    {
        return;
    }
    now = time_now.tv_sec;

    for (auto p : m_fabricLanePortMap)
    {
        int lane = p.first;
        sai_object_id_t port = p.second;

        string key = "PORT" + to_string(lane);
        std::vector<FieldValueTuple> values;
        uint32_t remote_peer;
        uint32_t remote_port;

        attr.id = SAI_PORT_ATTR_FABRIC_ATTACHED;
        status = sai_port_api->get_port_attribute(port, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            // Port may not be ready for query
            SWSS_LOG_ERROR("Failed to get fabric port (%d) status, rv:%d", lane, status);
            task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
            if (handle_status != task_process_status::task_success)
            {
                return;
            }
        }

        if (m_portStatus.find(lane) != m_portStatus.end() &&
            m_portStatus[lane] && !attr.value.booldata)
        {
            m_portDownCount[lane] ++;
            m_portDownSeenLastTime[lane] = now;
        }
        m_portStatus[lane] = attr.value.booldata;

        if (m_portStatus[lane])
        {
            attr.id = SAI_PORT_ATTR_FABRIC_ATTACHED_SWITCH_ID;
            status = sai_port_api->get_port_attribute(port, 1, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
                if (handle_status != task_process_status::task_success)
                {
                    throw runtime_error("FabricPortsOrch get remote id failure");
                }
            }
            remote_peer = attr.value.u32;

            attr.id = SAI_PORT_ATTR_FABRIC_ATTACHED_PORT_INDEX;
            status = sai_port_api->get_port_attribute(port, 1, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                task_process_status handle_status = handleSaiGetStatus(SAI_API_PORT, status);
                if (handle_status != task_process_status::task_success)
                {
                    throw runtime_error("FabricPortsOrch get remote port index failure");
                }
            }
            remote_port = attr.value.u32;
        }

        values.emplace_back("STATUS", m_portStatus[lane] ? "up" : "down");
        if (m_portStatus[lane])
        {
            values.emplace_back("REMOTE_MOD", to_string(remote_peer));
            values.emplace_back("REMOTE_PORT", to_string(remote_port));
        }
        if (m_portDownCount[lane] > 0)
        {
            values.emplace_back("PORT_DOWN_COUNT", to_string(m_portDownCount[lane]));
            values.emplace_back("PORT_DOWN_SEEN_LAST_TIME",
                                to_string(m_portDownSeenLastTime[lane]));
        }
        m_stateTable->set(key, values);
    }
}

void FabricPortsOrch::doTask()
{
}

void FabricPortsOrch::doTask(Consumer &consumer)
{
}

void FabricPortsOrch::doTask(swss::SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    if (!m_getFabricPortListDone)
    {
        getFabricPortList();
    }

    if (m_getFabricPortListDone)
    {
        updateFabricPortState();
    }
}
