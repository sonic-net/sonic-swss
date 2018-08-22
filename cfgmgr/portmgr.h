#ifndef __INTFMGR__
#define __INTFMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#include <map>
#include <string>

namespace swss {

class PortMgr : public Orch
{
public:
    PortMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames);
    using Orch::doTask;

private:
    Table m_cfgPortTable;
    Table m_cfgLagTable;
    Table m_statePortTable;
    Table m_stateLagTable;
    ProducerStateTable m_appPortTable;
    ProducerStateTable m_appLagTable;

    void doTask(Consumer &consumer);
    bool setPortMtu(const string &table, const string &alias, const string &mtu);
    bool setPortAdminStatus(const string &alias, const bool up);
    bool isPortStateOk(const string &table, const string &alias);
};

}

#endif
