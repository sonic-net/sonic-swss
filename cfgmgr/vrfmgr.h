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
    VrfMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);
    using Orch::doTask;

private:
    bool delLink(const std::string& vrfName);
    bool setLink(const std::string& vrfName);
    bool isVrfObjExist(const std::string& vrfName);
    void recycleTable(uint32_t table);
    uint32_t getFreeTable(void);
    void handleVnetConfigSet(KeyOpFieldsValuesTuple &t);
    void doTask(Consumer &consumer);

    std::map<std::string, uint32_t> m_vrfTableMap;
    std::set<uint32_t> m_freeTables;

    Table m_stateVrfTable;
    ProducerStateTable m_appVrfTableProducer, m_appVnetTableProducer;
};

}

#endif
