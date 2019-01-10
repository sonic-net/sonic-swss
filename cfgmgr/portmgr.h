#pragma once

#include "dbconnector.h"
#include "orch.h"
#include "producerstatetable.h"

#include <map>
#include <set>
#include <string>

namespace swss {

class PortMgr : public Orch
{
public:
    PortMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames);

    using Orch::doTask;
private:
    Table m_cfgPortTable;
    Table m_cfgLagMemberTable;
    Table m_statePortTable;
    ProducerStateTable m_appPortTable;

    set<string> m_portList;

    void doTask(Consumer &consumer);
    bool setPortMtu(const string &alias, const string &mtu);
    bool setPortAdminStatus(const string &alias, const bool up);
    bool isPortStateOk(const string &alias);
};

}
