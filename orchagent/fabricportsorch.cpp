#include "fabricportsorch.h"

#include <inttypes.h>
#include <fstream>
#include <sstream>
#include <tuple>

#include "logger.h"
#include "schema.h"
#include "sai_serialize.h"
#include "timer.h"
#include "saihelper.h"
#include "converter.h"
#include "stringutility.h"

#define FABRIC_POLLING_INTERVAL_DEFAULT   (30)
#define FABRIC_PORT_PREFIX    "PORT"
#define FABRIC_PORT_ERROR     0
#define FABRIC_PORT_SUCCESS   1
#define FABRIC_PORT_STAT_COUNTER_FLEX_COUNTER_GROUP         "FABRIC_PORT_STAT_COUNTER"
#define FABRIC_PORT_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS   10000
#define FABRIC_QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP        "FABRIC_QUEUE_STAT_COUNTER"
#define FABRIC_QUEUE_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS  100000
#define FABRIC_DEBUG_POLLING_INTERVAL_DEFAULT   (60)

// constants for link monitoring
#define MAX_SKIP_CRCERR_ON_LNKUP_POLLS 20
#define MAX_SKIP_FECERR_ON_LNKUP_POLLS 20
// the follow constants will be replaced with the number in config_db
#define FEC_ISOLATE_POLLS 2
#define FEC_UNISOLATE_POLLS 8
#define ISOLATION_POLLS_CFG 1
#define RECOVERY_POLLS_CFG 8
#define ERROR_RATE_CRC_CELLS_CFG 1
#define ERROR_RATE_RX_CELLS_CFG 61035156

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t *sai_switch_api;
extern sai_port_api_t *sai_port_api;
extern sai_queue_api_t *sai_queue_api;

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

FabricPortsOrch::FabricPortsOrch(DBConnector *appl_db, vector<table_name_with_pri_t> &tableNames,
                                 bool fabricPortStatEnabled, bool fabricQueueStatEnabled) :
        Orch(appl_db, tableNames),
        port_stat_manager(FABRIC_PORT_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ,
                          FABRIC_PORT_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, true),
        queue_stat_manager(FABRIC_QUEUE_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ,
                           FABRIC_QUEUE_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS, true),
        m_timer(new SelectableTimer(timespec { .tv_sec = FABRIC_POLLING_INTERVAL_DEFAULT, .tv_nsec = 0 })),
        m_debugTimer(new SelectableTimer(timespec { .tv_sec = FABRIC_DEBUG_POLLING_INTERVAL_DEFAULT, .tv_nsec = 0 }))
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE( "FabricPortsOrch constructor" );

    m_state_db = shared_ptr<DBConnector>(new DBConnector("STATE_DB", 0));
    m_stateTable = unique_ptr<Table>(new Table(m_state_db.get(), APP_FABRIC_PORT_TABLE_NAME));

    m_counter_db = shared_ptr<DBConnector>(new DBConnector("COUNTERS_DB", 0));
    m_portNameQueueCounterTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_FABRIC_QUEUE_NAME_MAP));
    m_portNamePortCounterTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_FABRIC_PORT_NAME_MAP));
    m_fabricCounterTable = unique_ptr<Table>(new Table(m_counter_db.get(), COUNTERS_TABLE));

    m_flex_db = shared_ptr<DBConnector>(new DBConnector("FLEX_COUNTER_DB", 0));
    m_flexCounterTable = unique_ptr<ProducerTable>(new ProducerTable(m_flex_db.get(), APP_FABRIC_PORT_TABLE_NAME));
    m_appl_db = shared_ptr<DBConnector>(new DBConnector("APPL_DB", 0));
    m_applTable = unique_ptr<Table>(new Table(m_appl_db.get(), APP_FABRIC_MONITOR_PORT_TABLE_NAME));

    m_fabricPortStatEnabled = fabricPortStatEnabled;
    m_fabricQueueStatEnabled = fabricQueueStatEnabled;

    getFabricPortList();

    auto executor = new ExecutableTimer(m_timer, this, "FABRIC_POLL");
    Orch::addExecutor(executor);
    m_timer->start();

    auto debug_executor = new ExecutableTimer(m_debugTimer, this, "FABRIC_DEBUG_POLL");
    Orch::addExecutor(debug_executor);
    m_debugTimer->start();
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

    return FABRIC_PORT_SUCCESS;
}

bool FabricPortsOrch::allPortsReady()
{
    return m_getFabricPortListDone;
}

void FabricPortsOrch::generatePortStats()
{
    if (!m_fabricPortStatEnabled) return;

    SWSS_LOG_NOTICE("Generate fabric port stats");

    vector<FieldValueTuple> portNamePortCounterMap;
    for (auto p : m_fabricLanePortMap)
    {
        int lane = p.first;
        sai_object_id_t port = p.second;

        std::ostringstream portName;
        portName << FABRIC_PORT_PREFIX << lane;
        portNamePortCounterMap.emplace_back(portName.str(), sai_serialize_object_id(port));

        // Install flex counters for port stats
        std::unordered_set<std::string> counter_stats;
        for (const auto& it: port_stat_ids)
        {
            counter_stats.emplace(sai_serialize_port_stat(it));
        }
        port_stat_manager.setCounterIdList(port, CounterType::PORT, counter_stats);
    }
    m_portNamePortCounterTable->set("", portNamePortCounterMap);
}

void FabricPortsOrch::generateQueueStats()
{
    if (!m_fabricQueueStatEnabled) return;
    if (m_isQueueStatsGenerated) return;
    if (!m_getFabricPortListDone) return;

    SWSS_LOG_NOTICE("Generate queue map for fabric ports");

    sai_status_t status;
    sai_attribute_t attr;

    for (auto p : m_fabricLanePortMap)
    {
        int lane = p.first;
        sai_object_id_t port = p.second;

        // Each serdes has some pipes (queues) for unicast and multicast.
        // But normally fabric serdes uses only one pipe.
        attr.id = SAI_PORT_ATTR_QOS_NUMBER_OF_QUEUES;
        status = sai_port_api->get_port_attribute(port, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            throw runtime_error("FabricPortsOrch get port queue number failure");
        }
        int num_queues = attr.value.u32;

        if (num_queues > 0)
        {
            vector<sai_object_id_t> m_queue_ids;
            m_queue_ids.resize(num_queues);

            attr.id = SAI_PORT_ATTR_QOS_QUEUE_LIST;
            attr.value.objlist.count = (uint32_t) num_queues;
            attr.value.objlist.list = m_queue_ids.data();

            status = sai_port_api->get_port_attribute(port, 1, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                throw runtime_error("FabricPortsOrch get port queue list failure");
            }

            // Maintain queue map and install flex counters for queue stats
            vector<FieldValueTuple> portNameQueueMap;

            // Fabric serdes queue type is SAI_QUEUE_TYPE_FABRIC_TX. Since we always
            // maintain only one queue for fabric serdes, m_queue_ids size is 1.
            // And so, there is no need to query  SAI_QUEUE_ATTR_TYPE and SAI_QUEUE_ATTR_INDEX
            // for queue. Actually, SAI does not support query these attributes on fabric serdes.
            int queueIndex = 0;
            std::ostringstream portName;
            portName << FABRIC_PORT_PREFIX << lane << ":" << queueIndex;
            const auto queue = sai_serialize_object_id(m_queue_ids[queueIndex]);
            portNameQueueMap.emplace_back(portName.str(), queue);

            // We collect queue counters like occupancy level
            std::unordered_set<string> counter_stats;
            for (const auto& it: queue_stat_ids)
            {
                counter_stats.emplace(sai_serialize_queue_stat(it));
            }
            queue_stat_manager.setCounterIdList(m_queue_ids[queueIndex], CounterType::QUEUE, counter_stats);

            m_portNameQueueCounterTable->set("", portNameQueueMap);
        }
    }

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

        string key = FABRIC_PORT_PREFIX + to_string(lane);
        std::vector<FieldValueTuple> values;
        uint32_t remote_peer = 0;
        uint32_t remote_port = 0;

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

void FabricPortsOrch::updateFabricDebugCounters()
{
    if (!m_getFabricPortListDone) return;

    SWSS_LOG_ENTER();

    // Get time
    time_t now;
    struct timespec time_now;
    if (clock_gettime(CLOCK_MONOTONIC, &time_now) < 0)
    {
        return;
    }
    now = time_now.tv_sec;

    int fecIsolatedPolls = FEC_ISOLATE_POLLS;            // monPollThreshIsolation
    int fecUnisolatePolls = FEC_UNISOLATE_POLLS;         // monPollThreshRecovery
    int isolationPollsCfg = ISOLATION_POLLS_CFG;         // monPollThreshIsolation
    int recoveryPollsCfg = RECOVERY_POLLS_CFG;           // monPollThreshRecovery
    int errorRateCrcCellsCfg = ERROR_RATE_CRC_CELLS_CFG; // monErrThreshCrcCells
    int errorRateRxCellsCfg = ERROR_RATE_RX_CELLS_CFG;   // monErrThreshRxCells
    std::vector<FieldValueTuple> constValues;
    SWSS_LOG_INFO("updateFabricDebugCounters");

    // Get debug countesrs (e.g. # of cells with crc errors, # of cells)
    for (auto p : m_fabricLanePortMap)
    {
        int lane = p.first;
        sai_object_id_t port = p.second;

        string key = FABRIC_PORT_PREFIX + to_string(lane);
        // so basically port is the oid
        vector<FieldValueTuple> fieldValues;
        static const array<string, 3> cntNames =
        {
            "SAI_PORT_STAT_IF_IN_ERRORS", // cells with crc errors
            "SAI_PORT_STAT_IF_IN_FABRIC_DATA_UNITS", // rx data cells
            "SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES"  // cell with uncorrectable errors
        };
        if (!m_fabricCounterTable->get(sai_serialize_object_id(port), fieldValues))
        {
           SWSS_LOG_INFO("no port %s", sai_serialize_object_id(port).c_str());
        }

        uint64_t rxCells = 0;
        uint64_t crcErrors = 0;
        uint64_t codeErrors = 0;
        for (const auto& fv : fieldValues)
        {
            const auto field = fvField(fv);
            const auto value = fvValue(fv);
            for (size_t cnt = 0; cnt != cntNames.size(); cnt++)
            {
                if (field == "SAI_PORT_STAT_IF_IN_ERRORS")
                {
                    crcErrors = stoull(value);
                }
                else if (field == "SAI_PORT_STAT_IF_IN_FABRIC_DATA_UNITS")
                {
                    rxCells = stoull(value);
                }
                else if (field == "SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES")
                {
                    codeErrors = stoull(value);
                }
                SWSS_LOG_INFO("port %s %s %lld %lld %lld at %s",
                         sai_serialize_object_id(port).c_str(), field.c_str(), (long long)crcErrors,
                         (long long)rxCells, (long long)codeErrors, asctime(gmtime(&now)));
            }
        }
        // now we get the values of:
        // *totalNumCells *cellsWithCrcErrors *cellsWithUncorrectableErrors
        //
        // Check if the error rate (crcErrors/numRxCells) is greater than configured error threshold
        // (errorRateCrcCellsCfg/errorRateRxCellsCfg).
        // This is changing to check (crcErrors * errorRateRxCellsCfg) > (numRxCells * errorRateCrcCellsCfg)
        // Default value is: (crcErrors * 61035156) > (numRxCells * 1)
        // numRxCells = snmpBcmRxDataCells + snmpBcmRxControlCells
        // As we don't have snmpBcmRxControlCells polled right now,
        // we can use snmpBcmRxDataCells only and add snmpBcmRxControlCells later when it is getting polled.
        //
        // In STATE_DB, add several new attribute for each port:
        //    consecutivePollsWithErrors      POLL_WITH_ERRORS
        //    consecutivePollsWithNoErrors    POLL_WITH_NO_ERRORS
        //    consecutivePollsWithFecErrs     POLL_WITH_FEC_ERRORS
        //    consecutivePollsWithNoFecErrs   POLL_WITH_NOFEC_ERRORS
        //
        //    skipErrorsOnLinkupCount         SKIP_ERR_ON_LNKUP_CNT -- for skip all errors during boot up time
        //    skipCrcErrorsOnLinkupCount      SKIP_CRC_ERR_ON_LNKUP_CNT
        //    skipFecErrorsOnLinkupCount      SKIP_FEC_ERR_ON_LNKUP_CNT
        //    removeProblemLinkCount          RM_PROBLEM_LNK_CNT -- this is for feature of remove a flaky link permanently

        int consecutivePollsWithErrors = 0;
        int consecutivePollsWithNoErrors = 0;
        int consecutivePollsWithFecErrs = 0;
        int consecutivePollsWithNoFecErrs = 0;

        int skipCrcErrorsOnLinkupCount = 0;
        int skipFecErrorsOnLinkupCount = 0;
        uint64_t prevRxCells = 0;
        uint64_t prevCrcErrors = 0;
        uint64_t prevCodeErrors = 0;

        uint64_t testCrcErrors = 0;
        uint64_t testCodeErrors = 0;

        int autoIsolated = 0;
        string lnkStatus = "down";
        string testState = "product";

        // Get the consecutive polls from the state db
        std::vector<FieldValueTuple> values;
        string valuePt;
        bool exist = m_stateTable->get(key, values);
        if (!exist)
        {
            SWSS_LOG_INFO("No state infor for port %s", key.c_str());
            return;
        }
        for (auto val : values)
        {
            valuePt = fvValue(val);
            if (fvField(val) == "STATUS")
            {
                lnkStatus = valuePt;
                continue;
            }
            if (fvField(val) == "POLL_WITH_ERRORS")
            {
                consecutivePollsWithErrors = to_uint<uint8_t>(valuePt);
                continue;
            }
            if (fvField(val) == "POLL_WITH_NO_ERRORS")
            {
                consecutivePollsWithNoErrors = to_uint<uint8_t>(valuePt);
                continue;
            }
            if (fvField(val) == "POLL_WITH_FEC_ERRORS")
            {
                consecutivePollsWithFecErrs = to_uint<uint8_t>(valuePt);
                continue;
            }
            if (fvField(val) == "POLL_WITH_NOFEC_ERRORS")
            {
                consecutivePollsWithNoFecErrs = to_uint<uint8_t>(valuePt);
                continue;
            }
            if (fvField(val) == "SKIP_CRC_ERR_ON_LNKUP_CNT")
            {
                skipCrcErrorsOnLinkupCount = to_uint<uint8_t>(valuePt);
                continue;
            }
            if (fvField(val) == "SKIP_FEC_ERR_ON_LNKUP_CNT")
            {
                skipFecErrorsOnLinkupCount = to_uint<uint8_t>(valuePt);
                continue;
            }
            if (fvField(val) == "RX_CELLS")
            {
                prevRxCells = to_uint<uint64_t>(valuePt);
                continue;
            }
            if (fvField(val) == "CRC_ERRORS")
            {
                prevCrcErrors = to_uint<uint64_t>(valuePt);
                continue;
            }
            if (fvField(val) == "CODE_ERRORS")
            {
                prevCodeErrors = to_uint<uint64_t>(valuePt);
                continue;
            }
            if (fvField(val) == "AUTO_ISOLATED")
            {
                autoIsolated = to_uint<uint8_t>(valuePt);
                SWSS_LOG_INFO("port %s currently isolated: %s", key.c_str(),valuePt.c_str());
                continue;
            }
            if (fvField(val) == "TEST_CRC_ERRORS")
            {
                testCrcErrors = to_uint<uint64_t>(valuePt);
                continue;
            }
            if (fvField(val) == "TEST_CODE_ERRORS")
            {
                testCodeErrors = to_uint<uint64_t>(valuePt);
                continue;
            }
            if (fvField(val) == "TEST")
            {
                testState = valuePt;
                continue;
            }
        }

        // checking crc errors
        int maxSkipCrcCnt = MAX_SKIP_CRCERR_ON_LNKUP_POLLS;
        if (testState == "TEST"){
            maxSkipCrcCnt = 2;
        }
        if (skipCrcErrorsOnLinkupCount < maxSkipCrcCnt)
        {
            skipCrcErrorsOnLinkupCount += 1;
            valuePt = to_string(skipCrcErrorsOnLinkupCount);
            m_stateTable->hset(key, "SKIP_CRC_ERR_ON_LNKUP_CNT", valuePt.c_str());
            SWSS_LOG_INFO("port %s updates SKIP_CRC_ERR_ON_LNKUP_CNT to %s %d",
                          key.c_str(), valuePt.c_str(), skipCrcErrorsOnLinkupCount);
            // update error counters.
            prevCrcErrors = crcErrors;
        }
        else
        {
            uint64_t diffRxCells = 0;
            uint64_t diffCrcCells = 0;

            diffRxCells = rxCells - prevRxCells;
            if (testState == "TEST"){
                diffCrcCells = testCrcErrors - prevCrcErrors;
                prevCrcErrors = 0;
                isolationPollsCfg = isolationPollsCfg + 1;
            }
            else
            {
                diffCrcCells = crcErrors - prevCrcErrors;
                prevCrcErrors = crcErrors;
            }
            bool isErrorRateMore =
               ((diffCrcCells * errorRateRxCellsCfg) >
                (diffRxCells * errorRateCrcCellsCfg));
            if (isErrorRateMore)
            {
                if (consecutivePollsWithErrors < isolationPollsCfg)
                {
                    consecutivePollsWithErrors += 1;
                    consecutivePollsWithNoErrors = 0;
                }
            } else {
                if (consecutivePollsWithNoErrors < recoveryPollsCfg)
                {
                    consecutivePollsWithNoErrors += 1;
                    consecutivePollsWithErrors = 0;
                }
            }
            SWSS_LOG_INFO("port %s diffCrcCells %lld", key.c_str(), (long long)diffCrcCells);
            SWSS_LOG_INFO("consecutivePollsWithCRCErrs %d consecutivePollsWithNoCRCErrs %d",
                           consecutivePollsWithErrors, consecutivePollsWithNoErrors);
        }

        // checking FEC errors
        int maxSkipFecCnt = MAX_SKIP_FECERR_ON_LNKUP_POLLS;
        if (testState == "TEST"){
            maxSkipFecCnt = 2;
        }
        if (skipFecErrorsOnLinkupCount < maxSkipFecCnt)
        {
            skipFecErrorsOnLinkupCount += 1;
            valuePt = to_string(skipFecErrorsOnLinkupCount);
            m_stateTable->hset(key, "SKIP_FEC_ERR_ON_LNKUP_CNT", valuePt.c_str());
            SWSS_LOG_INFO("port %s updates SKIP_FEC_ERR_ON_LNKUP_CNT to %s",
                           key.c_str(), valuePt.c_str());
            // update error counters
            prevCodeErrors = codeErrors;
        }
        else
        {
            uint64_t diffCodeErrors = 0;
            if (testState == "TEST"){
                diffCodeErrors = testCodeErrors - prevCodeErrors;
                prevCodeErrors = 0;
                fecIsolatedPolls = fecIsolatedPolls + 1;
            }
            else
            {
                diffCodeErrors = codeErrors - prevCodeErrors;
                prevCodeErrors = codeErrors;
            }
            SWSS_LOG_INFO("port %s diffCodeErrors %lld", key.c_str(), (long long)diffCodeErrors);
            if (diffCodeErrors > 0)
            {
                if (consecutivePollsWithFecErrs < fecIsolatedPolls)
                {
                    consecutivePollsWithFecErrs += 1;
                    consecutivePollsWithNoFecErrs = 0;
                }
            }
            else if (diffCodeErrors <= 0)
            {
                if (consecutivePollsWithNoFecErrs < fecUnisolatePolls)
                {
                    consecutivePollsWithNoFecErrs += 1;
                    consecutivePollsWithFecErrs = 0;
                }
            }
            SWSS_LOG_INFO("consecutivePollsWithFecErrs %d consecutivePollsWithNoFecErrs %d",
                          consecutivePollsWithFecErrs,consecutivePollsWithNoFecErrs);
            SWSS_LOG_INFO("fecUnisolatePolls %d", fecUnisolatePolls);
        }

        // take care serdes link shut state setting
        if (lnkStatus == "up")
        {
            // debug information
            SWSS_LOG_INFO("port %s status up autoIsolated %d",
                          key.c_str(), autoIsolated);
            SWSS_LOG_INFO("consecutivePollsWithErrors %d consecutivePollsWithFecErrs %d",
                          consecutivePollsWithErrors, consecutivePollsWithFecErrs);
            SWSS_LOG_INFO("consecutivePollsWithNoErrors %d consecutivePollsWithNoFecErrs %d",
                          consecutivePollsWithNoErrors, consecutivePollsWithNoFecErrs);
            if (autoIsolated == 0 && (consecutivePollsWithErrors >= isolationPollsCfg
                                   || consecutivePollsWithFecErrs >= fecIsolatedPolls))
            {
                // Link needs to be isolated.
                SWSS_LOG_INFO("port %s auto isolated", key.c_str());
                autoIsolated = 1;
                valuePt = to_string(autoIsolated);
                m_stateTable->hset(key, "AUTO_ISOLATED", valuePt);
                SWSS_LOG_NOTICE("port %s set AUTO_ISOLATED %s", key.c_str(), valuePt.c_str());
                // Call SAI api here to actually isolated the link
            }
            else if (autoIsolated == 1 && consecutivePollsWithNoErrors >= recoveryPollsCfg
                  && consecutivePollsWithNoFecErrs >= fecUnisolatePolls)
            {
                // Link is isolated, but no longer needs to be.
                SWSS_LOG_INFO("port %s healthy again", key.c_str());
                autoIsolated = 0;
                valuePt = to_string(autoIsolated);
                m_stateTable->hset(key, "AUTO_ISOLATED", valuePt);
                SWSS_LOG_NOTICE("port %s set AUTO_ISOLATED %s", key.c_str(), valuePt.c_str());
                // Can we call SAI api here to unisolate the link?
            }
        }
        else
        {
            SWSS_LOG_INFO("link down");
        }

        // Update state_db with new data
        valuePt = to_string(consecutivePollsWithErrors);
        m_stateTable->hset(key, "POLL_WITH_ERRORS", valuePt.c_str());
        SWSS_LOG_INFO("port %s set POLL_WITH_ERRORS %s", key.c_str(), valuePt.c_str());

        valuePt = to_string(consecutivePollsWithNoErrors);
        m_stateTable->hset(key, "POLL_WITH_NO_ERRORS", valuePt.c_str());
        SWSS_LOG_INFO("port %s set POLL_WITH_NO_ERRORS %s", key.c_str(), valuePt.c_str());

        valuePt = to_string(consecutivePollsWithFecErrs);
        m_stateTable->hset(key, "POLL_WITH_FEC_ERRORS", valuePt.c_str());
        SWSS_LOG_INFO("port %s set POLL_WITH_FEC_ERRORS %s", key.c_str(), valuePt.c_str());

        valuePt = to_string(consecutivePollsWithNoFecErrs);
        m_stateTable->hset(key, "POLL_WITH_NOFEC_ERRORS", valuePt.c_str());
        SWSS_LOG_INFO("port %s set POLL_WITH_NOFEC_ERRORS %s",
                      key.c_str(), valuePt.c_str());

        valuePt = to_string(rxCells);
        m_stateTable->hset(key, "RX_CELLS", valuePt.c_str());
        SWSS_LOG_INFO("port %s set RX_CELLS %s",
                      key.c_str(), valuePt.c_str());

        valuePt = to_string(prevCrcErrors);
        m_stateTable->hset(key, "CRC_ERRORS", valuePt.c_str());
        SWSS_LOG_INFO("port %s set CRC_ERRORS %s",
                      key.c_str(), valuePt.c_str());

        valuePt = to_string(prevCodeErrors);
        m_stateTable->hset(key, "CODE_ERRORS", valuePt.c_str());
        SWSS_LOG_INFO("port %s set CODE_ERRORS %s",
                      key.c_str(), valuePt.c_str());
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

    if (timer.getFd() == m_timer->getFd())
    {
        if (!m_getFabricPortListDone)
        {
            getFabricPortList();
        }

        if (m_getFabricPortListDone)
        {
            updateFabricPortState();
        }
    }
    else if (timer.getFd() == m_debugTimer->getFd())
    {
        if (!m_getFabricPortListDone)
        {
            // Skip collecting debug information
            // as we don't have all fabric ports yet.
            return;
        }   

        if (m_getFabricPortListDone)
        {
            updateFabricDebugCounters();
        }
    }
}
