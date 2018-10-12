#ifndef __VRFMGR__
#define __VRFMGR__

#include <string>
#include <map>
#include <set>
#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

using namespace std;

namespace swss {

class VrfMgr : public Orch
{
public:
    VrfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames);
    using Orch::doTask;

private:
    bool delLink(const string& vrfName);
    bool setLink(const string& vrfName);
    void recycleTable(uint32_t table);
    uint32_t getFreeTable(void);
    void doTask(Consumer &consumer);

    map<string, uint32_t> m_vrfTableMap;
    set<uint32_t> m_freeTables;

    Table m_stateVrfTable;
};

}

#endif
