#ifndef __POEMGR__
#define __POEMGR__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"

#include <string>
#include <vector>

namespace swss {

class PoeMgr : public Orch
{
public:
    PoeMgr(DBConnector *appDb, DBConnector *cfgDb, const std::vector<std::string> &poeTables);
    using Orch::doTask;

private:
    ProducerStateTable m_appPoeTable;

    void doTask(Consumer &consumer);

};

}

#endif
