#include <vector>

#include "macsecmgr.h"

using namespace std;
using namespace swss;

MACsecMgr::MACsecMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<std::string> &tables) :
        Orch(cfgDb, tables)
{
}

void MACsecMgr::doTask(Consumer &consumer)
{

}
