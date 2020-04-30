#include <linux/if_ether.h>

#include <unordered_map>
#include <utility>
#include <exception>

#include "txmonorch.h"
#include "orch.h"
#include "port.h"
#include "logger.h"
#include "sai_serialize.h"
#include "converter.h"
#include "portsorch.h"
#include <vector>

extern sai_port_api_t *sai_port_api;
extern PortsOrch*       gPortsOrch;

using namespace std::rel_ops;

string tx_status_name [] = {"ok", "error", "unknown"};

TxMonOrch::TxMonOrch(TableConnector appDbConnector, 
                     TableConnector confDbConnector, 
                     TableConnector stateDbConnector) :
    Orch(confDbConnector.first, confDbConnector.second),
    m_TxDropTable(appDbConnector.first, appDbConnector.second),
    m_stateTxDropTable(stateDbConnector.first, stateDbConnector.second),
    m_countersDb(COUNTERS_DB, DBConnector::DEFAULT_UNIXSOCKET, 0),
    m_countersTable(&m_countersDb, COUNTERS_TABLE),
    m_pollTimer(new SelectableTimer(timespec { .tv_sec = 0, .tv_nsec = 0 })),
    m_PortsTxDrpStat(),
    m_pollPeriod(0)
{
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
        auto interv = timespec { .tv_sec = interval, .tv_nsec = 0 };

        SWSS_LOG_INFO("startTimer,  find executor %p\n", m_pollTimer);
        m_pollTimer->setInterval(interv);
        //is it ok to stop without having it started?
        m_pollTimer->stop();
        m_pollTimer->start();
        m_pollPeriod = interval;
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Failed to startTimer which might be due to failed to get timer\n");
    }
}

int TxMonOrch::handlePeriodUpdate(const vector<FieldValueTuple>& data)
{
    bool needStart = false;
    uint32_t periodToSet = 0;

    SWSS_LOG_ENTER();

    //is it possible for redis to combine multiple updates and notify once?
    //if so, we handle it in this way.
    //however, in case of that, does it respect the order in which multiple updates comming?
    //suppose it does.
    for (auto i : data)
    {
        try {
            if (fvField(i) == TXMONORCH_FIELD_CFG_PERIOD)
            {
                periodToSet = to_uint<uint32_t>(fvValue(i));

                needStart |= (periodToSet != m_pollPeriod);
                SWSS_LOG_INFO("TX_DRP handle cfg update period new %d\n", periodToSet);
            }
            else
            {
                SWSS_LOG_ERROR("Unknown field type %s\n", fvField(i).c_str());
                return -1;
            }
        }
        catch (...) {
            SWSS_LOG_ERROR("Failed to handle period update\n");
        }
    }

    if (needStart)
    {
        startTimer(periodToSet);
        SWSS_LOG_INFO("TX_DRP poll timer restarted with interval %d\n", periodToSet);
    }

    return 0;
}

int TxMonOrch::handleThresholdUpdate(const string &port, const vector<FieldValueTuple>& data, bool clear)
{
    SWSS_LOG_ENTER();

    try {
        if (clear)
        {
            //attention, for clear, no data is empty
            //tesThreshold(m_PortsTxDrpStat[port]) = 0;
            m_PortsTxDrpStat.erase(port);
            m_TxDropTable.del(port);
            m_stateTxDropTable.del(port);
            //todo, remove data from state_db and appl_db??
            SWSS_LOG_INFO("TX_DRP threshold cleared for port %s\n", port.c_str());
        }
        else
        {
            for (auto i : data)
            {
                if (TXMONORCH_FIELD_CFG_THRESHOLD == fvField(i))
                {
                    TxDropStatistics &tes = m_PortsTxDrpStat[port];
                    if (tesPortId(tes) == 0/*invalid id??*/)
                    {
                        //the first time this port is configured
                        Port saiport;
                        //what if port doesn't stand for a valid port? 
                        //that is, getPort returns false?
                        //what if the interface is removed with threshold configured?
                        if (gPortsOrch->getPort(port, saiport))
                        {
                            tesPortId(tes) = saiport.m_port_id;
                        }
                        tesState(tes) = TXMONORCH_PORT_STATE_UNKNOWN;
                    }
                    tesThreshold(tes) = to_uint<uint64_t>(fvValue(i));
                    SWSS_LOG_INFO("TX_DRP threshold reset to %ld for port %s\n", 
                                  tesThreshold(tes), port.c_str());
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown field type %s when handle threshold for %s\n", 
                                   fvField(i).c_str(), port.c_str());
                    return -1;
                }
            }
        }
    }
    catch (...) {
        SWSS_LOG_ERROR("Fail to startTimer handle periodic update\n");
    }

    return 0;
}

/*handle configuration update*/
void TxMonOrch::doTask(Consumer& consumer)
{
    int rc = 0;

    SWSS_LOG_ENTER();
    SWSS_LOG_INFO("TxMonOrch doTask consumer\n");

    if (!gPortsOrch->allPortsReady())
    {
        SWSS_LOG_INFO("Ports not ready\n");
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        vector<FieldValueTuple> fvs = kfvFieldsValues(t);

        rc = -1;

        SWSS_LOG_INFO("TX_DRP %s operation %s set %s del %s\n", 
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

int TxMonOrch::pollOnePortDropStatistics(const string &port, TxDropStatistics &stat)
{
    uint64_t txDrpStatistics = 0,
        txDrpStatLasttime = tesStatistics(stat),
        txDrpStatThreshold = tesThreshold(stat);
    int tx_drop_state,
        tx_drop_state_lasttime = tesState(stat);

    SWSS_LOG_ENTER();

#if 0
    {
        //generate a random value instead :D
        static uint64_t seed = 1;
        txDrpStatistics = seed;
        txDrpStatistics = txDrpStatistics * 991 % 997;
        seed = txDrpStatistics;
        txDrpStatistics += txDrpStatLasttime;
        SWSS_LOG_INFO("TX_DRP_POLL: got port %s tx_drp stati %d, lasttime %d threshold %d\n", 
                      port.c_str(), txDrpStatistics, txDrpStatLasttime, txDrpStatThreshold);
    }
#else
#if 0
    {
        uint64_t tx_drp = 0;
        SWSS_LOG_INFO("TX_DRP_POLL: got port %s %lx tx_drp stati %ld, lasttime %ld threshold %ld\n", 
                      port.c_str(), tesPortId(stat),
                      txDrpStatistics, txDrpStatLasttime, txDrpStatThreshold);
        vector<FieldValueTuple> fieldValues;

        if (m_countersTable.get(sai_serialize_object_id(tesPortId(stat)), fieldValues))
        {
            SWSS_LOG_INFO("TX_DRP_POLL: got port %s %lx statistics, parsing... \n", port.c_str(), tesPortId(stat));
            for (const auto& fv : fieldValues)
            {
                const auto field = fvField(fv);
                const auto value = fvValue(fv);


                if (field == "SAI_PORT_STAT_IF_OUT_DISCARDS")
                {
                    tx_drp = stoul(value);
                    SWSS_LOG_INFO("    TX_DRP_POLL: %s found %ld %s\n", 
                                  field.c_str(), tx_drp, value.c_str());
                    break;
                }
            }
        }
        else
        {
            SWSS_LOG_INFO("TX_DRP_POLL: failed to get port %s %lx \n", port.c_str(), tesPortId(stat));
        }
        txDrpStatistics = tx_drp;
        SWSS_LOG_INFO("TX_DRP_POLL: got port %s tx_drp stati %ld, lasttime %ld threshold %ld\n", 
                      port.c_str(), txDrpStatistics, txDrpStatLasttime, txDrpStatThreshold);
    }
#endif
    {
        static const vector<sai_stat_id_t> txDrpStatId = {SAI_PORT_STAT_IF_OUT_DISCARDS};
        uint64_t tx_drp = -1;
        //get statistics from hal
        //check FlexCounter::saiUpdateSupportedPortCounters in sai-redis for reference
        sai_port_api->get_port_stats(tesPortId(stat),
                                     static_cast<uint32_t>(txDrpStatId.size()),
                                     txDrpStatId.data(),
                                     &tx_drp);
        txDrpStatistics = tx_drp;
        SWSS_LOG_INFO("TX_DRP_POLL: got port %s tx_drp stati %ld, lasttime %ld threshold %ld\n", 
                      port.c_str(), txDrpStatistics, txDrpStatLasttime, txDrpStatThreshold);
    }

#endif

    if (txDrpStatistics - txDrpStatLasttime > txDrpStatThreshold)
    {
        tx_drop_state = TXMONORCH_PORT_STATE_ERROR;
    }
    else
    {
        tx_drop_state = TXMONORCH_PORT_STATE_OK;
    }
    if (tx_drop_state != tx_drop_state_lasttime)
    {
        tesState(stat) = tx_drop_state;
        //set status in STATE_DB
        vector<FieldValueTuple> fvs;
		if (tx_drop_state < TXMONORCH_PORT_STATE_MAX)
            fvs.emplace_back(TXMONORCH_FIELD_STATE_TX_STATE, tx_status_name[tx_drop_state]);
		else
            fvs.emplace_back(TXMONORCH_FIELD_STATE_TX_STATE, "invalid");
        m_stateTxDropTable.set(port, fvs);
        SWSS_LOG_INFO("TX_DRP_CFG: port %s state changed to %d, push to db\n", port.c_str(), tx_drop_state);
    }

    //refresh the local copy of last time statistics
    tesStatistics(stat) = txDrpStatistics;

    return 0;
}

void TxMonOrch::pollDropStatistics()
{
    SWSS_LOG_ENTER();

    KeyOpFieldsValuesTuple portEntry;

    for (auto i : m_PortsTxDrpStat)
    {
        vector<FieldValueTuple> fields;
        int rc;

        SWSS_LOG_INFO("TX_DRP_APPL: port %s tx_drp_stat %ld, before get\n", i.first.c_str(),
                        tesStatistics(i.second));
        rc = pollOnePortDropStatistics(i.first, i.second);
        if (rc != 0)
            SWSS_LOG_ERROR("TX_DRP_APPL: got port %s tx_drp_stat failed %d\n", i.first.c_str(), rc);
        fields.emplace_back(TXMONORCH_FIELD_APPL_STATI, to_string(tesStatistics(i.second)));
        fields.emplace_back(TXMONORCH_FIELD_APPL_TIMESTAMP, "0");
        fields.emplace_back(TXMONORCH_FIELD_APPL_SAIPORTID, to_string(tesPortId(i.second)));
        m_TxDropTable.set(i.first, fields);
        SWSS_LOG_INFO("TX_DRP_APPL: port %s tx_drp_stat %ld, push to db\n", i.first.c_str(),
                        tesStatistics(i.second));
    }

    m_TxDropTable.flush();
    m_stateTxDropTable.flush();
    SWSS_LOG_INFO("TX_DRP_APPL: flushing tables\n");
}

void TxMonOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_INFO("TxMonOrch doTask selectable timer\n");
    //for each ports, check the statisticis 
    pollDropStatistics();
}
