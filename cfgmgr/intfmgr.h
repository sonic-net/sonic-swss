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
    Table m_statePortTable, m_stateLagTable, m_stateVlanTable, m_stateVrfTable;

    bool setIntfIp(const string &alias, const string &opCmd, const string &ipPrefixStr, const bool ipv4 = true);
    bool setIntfVrf(const string &alias, const string vrfName);
    bool doIntfGeneralTask(const vector<string>& keys, const vector<FieldValueTuple>& data, const string& op);
    bool doIntfAddrTask(const vector<string>& keys, const vector<FieldValueTuple>& data, const string& op);
    void doTask(Consumer &consumer);
    bool isIntfStateOk(const string &alias);
};

}

#endif
