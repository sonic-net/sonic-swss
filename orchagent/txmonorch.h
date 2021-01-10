#ifndef __TXMONORCH__
#define __TXMONORCH__

#include "table.h"
#include "orch.h"
#include "selectabletimer.h"

using namespace swss;
using namespace std;

enum PortState {OK, NOT_OK, UNKNOWN};
#define STATES_NUMBER 3
static const array<string, STATES_NUMBER> stateNames = {"OK", "NOT_OK", "UNKNOWN"};

class TxMonOrch: public Orch
{
public:
    TxMonOrch(TableConnector confDbConnector, TableConnector stateDbConnector);
    ~TxMonOrch(void);

private:
    DBConnector m_countersDb;
    Table m_countersTable;
    Table m_countersPortNameTable;

    /* port alias to port state */
    Table m_portsStateTable;

    SelectableTimer *m_timer = nullptr;
    uint32_t m_pollPeriod;
    uint32_t m_threshold;

    bool isPortsMapInitialized = false;
    /* port alias to port oid */
    map<string, string> m_portsMap;

    /* port alias to port tx-error stats */
    map<string, uint64_t> m_currTxErrCounters;
    map<string, uint64_t> m_prevTxErrCounters;

    void doTask(Consumer &consumer);
    void doTask(SelectableTimer &timer);

    void handlePeriodUpdate(const vector<FieldValueTuple>& data);
    void handleThresholdUpdate(const vector<FieldValueTuple>& data);
    void setTimer(uint32_t interval);
    bool createPortsMap();
    bool getTxErrCounters();
    bool getTxErrCountersByAlias(string portAlias);
    void setPortsStateDb(string portAlias, PortState state);
    void updatePortsStateDb();


};




#endif
