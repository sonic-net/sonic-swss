#pragma once

#include "dbconnector.h"
#include "orch.h"
#include "producerstatetable.h"

#include <map>
#include <set>
#include <string>

namespace swss {

/* Port default admin status is down */
#define DEFAULT_ADMIN_STATUS_STR    "down"
#define DEFAULT_MTU_STR             "9100"

#define SFLOW_SAMPLE_RATE_KEY_400G "400000"
#define SFLOW_SAMPLE_RATE_KEY_100G "100000"
#define SFLOW_SAMPLE_RATE_KEY_50G  "50000"
#define SFLOW_SAMPLE_RATE_KEY_40G  "40000"
#define SFLOW_SAMPLE_RATE_KEY_25G  "25000"
#define SFLOW_SAMPLE_RATE_KEY_10G  "10000"
#define SFLOW_SAMPLE_RATE_KEY_1G   "1000"

#define SFLOW_SAMPLE_RATE_VALUE_400G "40000"
#define SFLOW_SAMPLE_RATE_VALUE_100G "10000"
#define SFLOW_SAMPLE_RATE_VALUE_50G  "5000"
#define SFLOW_SAMPLE_RATE_VALUE_40G  "4000"
#define SFLOW_SAMPLE_RATE_VALUE_25G  "2500"
#define SFLOW_SAMPLE_RATE_VALUE_10G  "1000"
#define SFLOW_SAMPLE_RATE_VALUE_1G   "100"

class SflowMgr : public Orch
{
public:
    SflowMgr(DBConnector *cfgDb, DBConnector *appDb, const vector<string> &tableNames);

    using Orch::doTask;
private:
    Table m_cfgSflowTable;
    Table m_cfgSflowSessionTable;
    Table m_appSflowSpeedRateTable;
    ProducerStateTable m_appSflowTable;
    ProducerStateTable m_appSflowSessionTable;

    void doTask(Consumer &consumer);
    void handleSflowTableConfig(Consumer &consumer);
};

}
