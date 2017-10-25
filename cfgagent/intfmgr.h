#ifndef __INTFMGR__
#define __INTFMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orchbase.h"

#include <map>
#include <string>

namespace swss {

class IntfMgr : public OrchBase
{
public:
    IntfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames);
    void syncCfgDB();

private:
    ProducerStateTable m_appIntfTableProducer;
    Table m_cfgIntfTable, m_cfgVlanIntfTable;
    Table m_statePortTable, m_stateLagTable, m_stateVlanTable;

    bool setIntfIp(const string &alias, const string &opCmd, const string &ipPrefixStr);
    void doTask(Consumer &consumer);
    bool isIntfStateOk(const string &alias);
};

}

#endif
