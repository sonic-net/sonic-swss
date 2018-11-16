#ifndef __NBRMGR__
#define __NBRMGR__

#include <string>
#include <map>
#include <set>

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"
#include "netmsg.h"

using namespace std;

namespace swss {

class NbrMgr : public Orch
{
public:
    NbrMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames);
    using Orch::doTask;

private:
    bool isIntfStateOk(const string &alias);
    bool setNeighbor(const string& alias, IpAddress& ip, MacAddress& mac);
    bool sendMsg(struct nlmsghdr *msg);

    void doTask(Consumer &consumer);

    Table m_statePortTable, m_stateLagTable, m_stateVlanTable, m_stateIntfTable;
};

}

#endif // __NBRMGR__
