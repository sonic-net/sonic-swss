#include "txmonorch.h"
#include "schema.h"
#include "logger.h"
#include "timer.h"
#include "converter.h"
#include "portsorch.h"

extern PortsOrch* gPortsOrch;

TxMonOrch::TxMonOrch(TableConnector confDbConnector, TableConnector stateDbConnector) :
    Orch(confDbConnector.first, confDbConnector.second),
    m_countersDb(COUNTERS_DB, DBConnector::DEFAULT_UNIXSOCKET, 0),
    m_countersTable(&m_countersDb, COUNTERS_TABLE),
    m_countersPortNameTable(&m_countersDb, COUNTERS_PORT_NAME_MAP),
    m_portsStateTable(stateDbConnector.first, stateDbConnector.second),
    m_pollPeriod(0),
    m_threshold(0)
{
    auto interv = timespec { .tv_sec = 0, .tv_nsec = 0 };
    m_timer = new SelectableTimer(interv);
    auto executor = new ExecutableTimer(m_timer, this, "POLLING_TIMER");
    Orch::addExecutor(executor);
}

TxMonOrch::~TxMonOrch()
{
    SWSS_LOG_ENTER();
}

void TxMonOrch::setTimer(uint32_t interval)
{
    auto interv = timespec { .tv_sec = interval, .tv_nsec = 0 };

    m_timer->setInterval(interv);
    m_timer->reset();
    m_pollPeriod = interval;
    SWSS_LOG_NOTICE("liora.debug m_pollPeriod set [%u]", m_pollPeriod);
}

void TxMonOrch::handlePeriodUpdate(const vector<FieldValueTuple>& data)
{
    uint32_t periodToSet = 0;

    for (auto element : data) {
        const auto &field = fvField(element);
        const auto &value = fvValue(element);

        if (field == "value") {
            try {
                periodToSet = stoi(value);
            } catch (...){
                return;
            }
            if(periodToSet != m_pollPeriod){
                setTimer(periodToSet);
            }
        } else {
            SWSS_LOG_ERROR("Unexpected field %s", field.c_str());
        }
    }
}

void TxMonOrch::handleThresholdUpdate(const vector<FieldValueTuple>& data)
{
    for (auto element : data) {
        const auto &field = fvField(element);
        const auto &value = fvValue(element);

        if (field == "value") {
            try {
                m_threshold = stoi(value);
            } catch (...){
                return;
            }
            SWSS_LOG_NOTICE("liora.debug m_threshold set [%u]", m_threshold);
        } else {
            SWSS_LOG_ERROR("Unexpected field %s", field.c_str());
        }
    }
}

bool TxMonOrch::createPortsMap()
{
    map<string, Port> &portsList =  gPortsOrch->getAllPorts();
    for (auto &entry : portsList)
    {
        string name = entry.first;

        Port p = entry.second;
        if (p.m_type != Port::PHY)
        {
            continue;
        }
        string oidStr;
        if (!m_countersPortNameTable.hget("", name, oidStr))
        {
            SWSS_LOG_ERROR("Failed to get port oid from counters DB");
            return false;
        }
        m_portsMap.emplace(p.m_alias, oidStr);
    }
    return true;
}

void TxMonOrch::setPortsStateDb(string portAlias, PortState state)
{
    vector<FieldValueTuple> fieldValuesVector;
    fieldValuesVector.emplace_back("port_state", stateNames[state]);
    m_portsStateTable.set(portAlias, fieldValuesVector);
}

bool TxMonOrch::getTxErrCountersByAlias(string portAlias)
{
    string strValue;
    string oidStr = m_portsMap.find(portAlias)->second;

    if (!m_countersTable.hget(oidStr, "SAI_PORT_STAT_IF_OUT_ERRORS", strValue)) {
        SWSS_LOG_ERROR("Failed to get port %s tx error counters from counters DB", oidStr.c_str());
        setPortsStateDb(portAlias, UNKNOWN);
        return false;
    }

    m_currTxErrCounters[portAlias] = stoul(strValue);

    return true;
}

bool TxMonOrch::getTxErrCounters()
{
    for (auto &entry : m_portsMap) {
        string portAlias = entry.first;
        if (!getTxErrCountersByAlias(portAlias)) {
            return false;
        }
    }
    return true;
}

void TxMonOrch::updatePortsStateDb()
{
    for (auto &entry : m_currTxErrCounters)
    {
        PortState portState = OK;
        string portAlias = entry.first;
        uint64_t curr = entry.second;
        uint64_t prev = 0;

        prev = m_prevTxErrCounters[portAlias];

        /* save data for the next update */
        m_prevTxErrCounters[portAlias] = curr;

        if(portAlias.compare("Ethernet0") == 0){
            static uint32_t cnt = 0;
            SWSS_LOG_NOTICE("liora.debug [%u] port %s curr [%lu] prev [%lu] m_threshold [%u]",
                            cnt++, portAlias.c_str(), curr, prev, m_threshold);
        }

        if ((curr - prev) >= m_threshold)
        {
            portState = NOT_OK;
        }
        setPortsStateDb(portAlias, portState);
    }
}

void TxMonOrch::doTask(Consumer& consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end()) {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        vector<FieldValueTuple> fvs = kfvFieldsValues(t);

        if (op == SET_COMMAND) {
            if (key == "polling_period") {
                handlePeriodUpdate(fvs);
            } else if (key == "threshold"){
                handleThresholdUpdate(fvs);
            }
        } else {
            SWSS_LOG_ERROR("Unexpected operation %s", op.c_str());
        }

        consumer.m_toSync.erase(it++);
    }
}

void TxMonOrch::doTask(SelectableTimer &timer)
{
    if (!gPortsOrch->allPortsReady()) {
        SWSS_LOG_WARN("Ports are not ready yet");
        return;
    }

    if (!isPortsMapInitialized) {
        if (!createPortsMap()) {
            SWSS_LOG_WARN("Failed to create ports names internal cache");
            return;
        }
        isPortsMapInitialized = true;
    }

    if(getTxErrCounters()){
        updatePortsStateDb();
    } else {
        SWSS_LOG_WARN("Failed to create ports counters internal cache");
        return;
    }
}
