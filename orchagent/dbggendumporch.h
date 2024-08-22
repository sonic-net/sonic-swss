#ifndef SWSS_DBG_GEN_DUMP_H
#define SWSS_DBG_GEN_DUMP_H

#include "orch.h"
#include "dbconnector.h"
#include "table.h"
#include <string>

using namespace std;
using namespace swss;

extern "C" {
#include "sai.h"
}

class DbgGenDumpOrch : public Orch
{
public:
    DbgGenDumpOrch(TableConnector stateDbConnector);
    
    ~DbgGenDumpOrch();

    void doTask(Consumer& consumer);

private:
    Table m_stateDbDumpStatsTable;

};

#endif /* SWSS_DBG_GEN_DUMP_H */
