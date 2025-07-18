#ifndef SWSS_TXMONORCH_H
#define SWSS_TXMONORCH_H

#include "orch.h"
#include "producerstatetable.h"
#include "observer.h"
#include "portsorch.h"
#include "selectabletimer.h"
#include "table.h"
#include "select.h"
#include "timer.h"

#include <map>
#include <algorithm>
#include <tuple>
#include <inttypes.h>

extern "C" {
#include "sai.h"
}

/*fields definition*/
#define TXMONORCH_FIELD_CFG_PERIOD      "tx_error_check_period"
#define TXMONORCH_FIELD_CFG_THRESHOLD       "tx_error_threshold"

#define TXMONORCH_FIELD_APPL_STATI      "tx_error_stati"
#define TXMONORCH_FIELD_APPL_TIMESTAMP  "tx_error_timestamp"
#define TXMONORCH_FIELD_APPL_SAIPORTID  "tx_error_portid"

#define TXMONORCH_FIELD_STATE_TX_STATE  "tx_status"

/*key name definition*/
/*key name for each port is its intf name*/
/*key name of global period*/
#define TXMONORCH_KEY_CFG_PERIOD    "GLOBAL_PERIOD" 

/*table names are defined in schema.h*/

#define TXMONORCH_ERR_STATE     "tx_status"

#define TXMONORCH_SEL_TIMER     "TX_ERR_COUNTERS_POLL"

/*tx state definition*/
#define TXMONORCH_PORT_STATE_OK         0
#define TXMONORCH_PORT_STATE_ERROR      1
#define TXMONORCH_PORT_STATE_UNKNOWN    2
#define TXMONORCH_PORT_STATE_MAX        3

typedef std::tuple<int, sai_object_id_t, uint64_t, uint64_t> TxErrorStatistics;//state, stati, threshold
typedef std::map<std::string, TxErrorStatistics> TxErrorStatMap;

#define tesState std::get<0>
#define tesPortId std::get<1>
#define tesStatistics std::get<2>
#define tesThreshold std::get<3>

class TxMonOrch : public Orch
{
public:
    TxMonOrch(TableConnector appDbConnector, 
              TableConnector confDbConnector, 
              TableConnector stateDbConnector);

private:
    //ProducerStateTable is designed to provide a IPC ability, 
    //Table is designed for data persistence.
    //representing PORT_TX_STATISTICS_TABLE in APPL_DB
    Table m_TxErrorTable;
    //representing PORT_TX_STAT_TABLE in STATE_DB
    Table m_stateTxErrorTable;

    //for fetching statistics
    DBConnector m_countersDb;
    Table m_countersTable;

    TxErrorStatMap m_PortsTxErrStat;

    /*should be accessed via an atomic approach?*/
    uint32_t m_pollPeriod;
    int m_poolPeriodChanged;
    SelectableTimer *m_pollTimer;

    void doTask(Consumer& consumer);
    void doTask(SelectableTimer &timer);

    void startTimer(uint32_t interval);
    int handlePeriodUpdate(const vector<FieldValueTuple>& data);
    int handleThresholdUpdate(const string &key, const vector<FieldValueTuple>& data, bool clear);
    int pollOnePortErrorStatistics(const string &port, TxErrorStatistics &stat);
    void pollErrorStatistics();
};
#endif /* SWSS_TXMONORCH_H */