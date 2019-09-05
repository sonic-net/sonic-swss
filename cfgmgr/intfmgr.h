#ifndef __INTFMGR__
#define __INTFMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#include <map>
#include <string>

namespace swss {

class IntfMgr : public Orch
{
public:
    IntfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames);
    using Orch::doTask;

private:
    ProducerStateTable m_appIntfTableProducer;
    Table m_cfgIntfTable, m_cfgVlanIntfTable;
    Table m_statePortTable, m_stateLagTable, m_stateVlanTable, m_stateVrfTable, m_stateIntfTable;

    void setIntfIp(const string &alias, const string &opCmd, const IpPrefix &ipPrefix);
    void setIntfVrf(const string &alias, const string &vrfName);
    bool doIntfGeneralTask(const vector<string>& keys, const vector<FieldValueTuple>& data, const string& op);
    bool doIntfAddrTask(const vector<string>& keys, const vector<FieldValueTuple>& data, const string& op);
    void doTask(Consumer &consumer);
    bool isIntfStateOk(const string &alias);
    bool isIntfGeneralDone(const string &alias);
    bool isIntfChangeVrf(const string &alias, const string &vrfName);
    int getIntfIpCount(const string &alias);
    void addLoopbackIntf(const string &alias);
    void delLoopbackIntf(const string &alias);
};

}

#endif
