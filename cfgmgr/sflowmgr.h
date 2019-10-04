#pragma once

#include "dbconnector.h"
#include "orch.h"
#include "producerstatetable.h"

#include <map>
#include <set>
#include <string>

namespace swss {

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

#define SFLOW_ERROR_SPEED_STR "error"

struct SflowLocalPortInfo
{
    bool   local_conf;
    string speed;
    string local_rate;
    string local_admin;
};

/* Port to Local config map  */
typedef map<string, SflowLocalPortInfo> SflowPortLocalConfMap;

class SflowMgr : public Orch
{
public:
    SflowMgr(DBConnector *cfgDb, DBConnector *appDb, const vector<string> &tableNames);

    using Orch::doTask;
private:
    Table                  m_cfgSflowTable;
    Table                  m_cfgSflowSessionTable;
    ProducerStateTable     m_appSflowTable;
    ProducerStateTable     m_appSflowSessionTable;
    SflowPortLocalConfMap  m_sflowPortLocalConfMap;
    bool                   intf_all_conf;
    bool                   gEnable;

    void doTask(Consumer &consumer);
    void sflowHandleService(bool enable);
    void sflowUpdatePortInfo(Consumer &consumer);
    void sflowHandleSessionAll(bool enable);
    void sflowHandleSessionLocal(bool enable);
    void sflowCheckAndFillRate(string alias, vector<FieldValueTuple> &fvs);
    void sflowGetLocalFvs(vector<FieldValueTuple> &fvs, SflowLocalPortInfo &local_info);
    void sflowGetGlobalFvs(vector<FieldValueTuple> &fvs, string speed);
    void sflowUpdateLocalPortInfo(string alias, vector<FieldValueTuple> &fvs);
};

}
