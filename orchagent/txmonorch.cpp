#include <linux/if_ether.h>
#include <unordered_map>
#include <vector>
#include <memory>
#include <utility>
#include <exception>
#include "txmonorch.h"
#include "orch.h"
#include "port.h"
#include "logger.h"
#include "sai_serialize.h"
#include "converter.h"
#include "portsorch.h"

extern sai_port_api_t *sai_port_api;
extern PortsOrch* gPortsOrch;

using namespace std;

string tx_status_name[] = {"ok", "error", "unknown"};

TxMonOrch::TxMonOrch(TableConnector appDbConnector, TableConnector confDbConnector, TableConnector stateDbConnector) :
    Orch(confDbConnector.first, confDbConnector.second),
    m_countersDb(COUNTERS_DB, DBConnector::DEFAULT_UNIXSOCKET, 0),
    m_countersTable(&m_countersDb, COUNTERS_TABLE),
    m_pollTimer(new SelectableTimer(timespec { .tv_sec = 0, .tv_nsec = 0 })),
    m_TxErrorTable(appDbConnector.first, appDbConnector.second),
    m_stateTxErrorTable(stateDbConnector.first, stateDbConnector.second),
    m_pollPeriod(0)
{
    // executor and m_pollTimer will both be released by Orch
    // Design assumption
    // 1. one Orch can have one or more Executor
    // 2. one Executor must belong to one and only one Orch
    // 3. Executor will hold an pointer to new-ed selectable, and delete it during dtor
    auto executor = new ExecutableTimer(m_pollTimer, this, TXMONORCH_SEL_TIMER);
    Orch::addExecutor(executor);
    SWSS_LOG_NOTICE("TxMonOrch initialized with table %s %s %s\n",
                    appDbConnector.second.c_str(),
                    stateDbConnector.second.c_str(),
                    confDbConnector.second.c_str());
}

void TxMonOrch::startTimer(uint32_t interval)
{
    SWSS_LOG_ENTER();

    try
    {
        timespec interv{ .tv_sec = interval, .tv_nsec = 0 };
        SWSS_LOG_INFO("startTimer, find executor %p\n", m_pollTimer);
        // Is it ok to stop without having it started?
        m_pollTimer->stop();
        m_pollTimer->setInterval(interv);
        m_pollTimer->start();
        m_pollPeriod = interval;
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Failed to start timer which might be due to failed to get timer\n");
    }
}

int TxMonOrch::handlePeriodUpdate(const vector<FieldValueTuple>& data)
{
    bool needStart = false;
    uint32_t periodToSet = 0;

    SWSS_LOG_ENTER();

    // Is it possible for redis to combine multiple updates and notify once?
    // If so, we handle it in this way.
    // However, in case of that, does it respect the order in which multiple updates comming?
    // Suppose it does.
    for (const auto& i : data)
    {
        try
        {
            if (fvField(i) == TXMONORCH_FIELD_CFG_PERIOD)
            {
                periodToSet = to_uint<uint32_t>(fvValue(i));
                needStart |= (periodToSet != m_pollPeriod);
                SWSS_LOG_INFO("TX_ERR handle cfg update period new %d\n", periodToSet);
            }
            else
            {
                SWSS_LOG_ERROR("Unknown field type %s\n", fvField(i).c_str());
                return -1;
            }
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Failed to handle period update\n");
        }
    }

    if (needStart)
    {
        startTimer(periodToSet);
        SWSS_LOG_INFO("TX_ERR poll timer restarted with interval %d\n", periodToSet);
    }

    return 0;
}

int TxMonOrch::handleThresholdUpdate(const string &port, const vector<FieldValueTuple>& data, bool clear)
{
    SWSS_LOG_ENTER();
    try
    {
        if (clear)
        {
            //attention, for clear, no data is empty
            //txErrThreshold(m_PortsTxErrStat[port]) = 0;
            m_PortsTxErrStat.erase(port);
            m_TxErrorTable.del(port);
            m_stateTxErrorTable.del(port);
            // TODO: remove data from state_db and appl_db
            SWSS_LOG_INFO("TX_ERR threshold cleared for port %s\n", port.c_str());
            return 0;
        }

        for (auto i : data)
        {
            if (TXMONORCH_FIELD_CFG_THRESHOLD == fvField(i))
            {
                TxErrorStatistics &stat = m_PortsTxErrStat[port];
                if (stat.txErrPortId == TXMONORCH_INVALID_PORT_ID)
                {
                    //the first time this port is configured
                    Port saiport;
                    //what if port doesn't stand for a valid port?
                    //that is, getPort returns false?
                    //what if the interface is removed with threshold configured?
                    if (gPortsOrch->getPort(port, saiport))
                    {
                        stat.txErrPortId = saiport.m_port_id;
                    }
                    stat.txErrState = TXMONORCH_PORT_STATE_UNKNOWN;
                }
                stat.txErrThreshold = to_uint<uint64_t>(fvValue(i));
                SWSS_LOG_INFO("TX_ERR threshold reset to %ld for port %s\n",
                              stat.txErrThreshold, port.c_str());
            }
            else
            {
                SWSS_LOG_ERROR("Unknown field type %s when handle threshold for %s\n",
                               fvField(i).c_str(), port.c_str());
                return -1;
            }
        }
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Fail to startTimer handle periodic update\n");
    }

    return 0;
}

/*handle configuration update*/
void TxMonOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_INFO("TxMonOrch doTask consumer\n");

    if (!gPortsOrch->isPortReady())
    {
        SWSS_LOG_INFO("Ports not ready\n");
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple& t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        vector<FieldValueTuple> fvs = kfvFieldsValues(t);

        int rc = -1;

        SWSS_LOG_INFO("TX_ERR %s operation %s set %s del %s\n",
                      key.c_str(),
                      op.c_str(), SET_COMMAND, DEL_COMMAND);
        if (key == TXMONORCH_KEY_CFG_PERIOD)
        {
            if (op == SET_COMMAND)
            {
                rc = handlePeriodUpdate(fvs);
            }
            else
            {
                SWSS_LOG_ERROR("Unknown operation type %s when set period\n", op.c_str());
            }
        }
        else //key should be the alias of interface
        {
            if (op == SET_COMMAND)
            {
                //fetch the value which reprsents threshold
                rc = handleThresholdUpdate(key, fvs, false);
            }
            else if (op == DEL_COMMAND)
            {
                //reset to default
                rc = handleThresholdUpdate(key, fvs, true);
            }
            else
            {
                SWSS_LOG_ERROR("Unknown operation type %s when set threshold\n", op.c_str());
            }
        }

        if (rc)
        {
            SWSS_LOG_ERROR("Handle configuration update failed index %s\n", key.c_str());
        }

        consumer.m_toSync.erase(it++);
    }
}

int TxMonOrch::pollOnePortErrorStatistics(const string &port, TxErrorStatistics &stat)
{
    uint64_t txErrStat = 0;
    uint64_t txErrStatLasttime = stat.txErrStat;
    uint64_t txErrStatThreshold = stat.txErrThreshold;
    int txErrPortState;
    int txErrPortStateLastTime = stat.txErrState;

    SWSS_LOG_ENTER();

    static const vector<sai_stat_id_t> txErrStatId = {SAI_PORT_STAT_IF_OUT_ERRORS};
    //get statistics from hal
    //check FlexCounter::saiUpdateSupportedPortCounters in sai-redis for reference
    sai_port_api->get_port_stats(stat.txErrPortId,
                                static_cast<uint32_t>(txErrStatId.size()),
                                txErrStatId.data(),
                                &txErrStat);
    SWSS_LOG_INFO("TX_ERR_POLL: got port %s txErr Stat %ld, lasttime %ld threshold %ld\n",
                    port.c_str(), txErrStat, txErrStatLasttime, txErrStatThreshold);

    if (txErrStat - txErrStatLasttime > txErrStatThreshold)
    {
        txErrPortState = TXMONORCH_PORT_STATE_ERROR;
    }
    else
    {
        txErrPortState = TXMONORCH_PORT_STATE_OK;
    }

    if (txErrPortState != txErrPortStateLastTime)
    {
        stat.txErrState = txErrPortState;
        //set status in STATE_DB
        vector<FieldValueTuple> fvs;
        if (txErrPortState < TXMONORCH_PORT_STATE_MAX)
            fvs.emplace_back(TXMONORCH_FIELD_STATE_TX_STATE, tx_status_name[txErrPortState]);
        else
            fvs.emplace_back(TXMONORCH_FIELD_STATE_TX_STATE, "invalid");
        m_stateTxErrorTable.set(port, fvs);
        SWSS_LOG_INFO("TX_ERR_CFG: port %s state changed to %d, push to db\n", port.c_str(), txErrPortState);
    }

    //refresh the local copy of last time statistics
    stat.txErrStat = txErrStat;

    return 0;
}

void TxMonOrch::pollErrorStatistics()
{
    SWSS_LOG_ENTER();

    for (auto& i : m_PortsTxErrStat) {
        auto& port = i.first;
        auto& stat = i.second;
        vector<FieldValueTuple> fields;

        SWSS_LOG_INFO("TX_ERR_APPL: port %s tx_err_stat %ld, before get\n", port.c_str(), stat.txErrStat);
        int rc = pollOnePortErrorStatistics(port, stat);
        if (rc != 0) {
            SWSS_LOG_ERROR("TX_ERR_APPL: got port %s tx_err_stat failed with rc %d\n", port.c_str(), rc);
            continue;
        }
        fields.emplace_back(TXMONORCH_FIELD_APPL_STATI, to_string(stat.txErrStat));
        fields.emplace_back(TXMONORCH_FIELD_APPL_TIMESTAMP, "0");
        fields.emplace_back(TXMONORCH_FIELD_APPL_SAIPORTID, to_string(stat.txErrPortId));
        m_TxErrorTable.set(port, fields);
        SWSS_LOG_INFO("TX_ERR_APPL: port %s tx_err_stat %ld, push to db\n", port.c_str(),
                      stat.txErrStat);
    }

    m_TxErrorTable.flush();
    m_stateTxErrorTable.flush();
    SWSS_LOG_INFO("TX_ERR_APPL: flush tables finish\n");
}

void TxMonOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_INFO("TxMonOrch doTask selectable timer\n");
    // For each port, check the statisticis
    pollErrorStatistics();
}