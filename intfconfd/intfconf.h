#ifndef __INTFCONF__
#define __INTFCONF__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "cfgorch.h"

#include <map>
#include <string>

namespace swss {

class IntfConf : public CfgOrch
{
public:
    IntfConf(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, vector<string> tableNames);
    void syncCfgDB();

private:
	ProducerStateTable m_appIntfTableProducer;
	Table m_cfgIntfTable, m_cfgVlanIntfTable;
	Table m_statePortTable, m_stateLagTable, m_stateVlanTable;

    bool setIntfIp(string &alias, string &opCmd, string &ipPrefixStr);
    void doTask(Consumer &consumer);
    bool isIntfStateOk(string &alias);
};

}

#endif
