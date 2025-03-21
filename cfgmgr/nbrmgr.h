#ifndef __NBRMGR__
#define __NBRMGR__

#include <string>
#include <map>
#include <set>

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orchagent/orch.h"
#include "netmsg.h"

using namespace std;

namespace swss {

class NbrMgr : public Orch
{
public:
    NbrMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);
    using Orch::doTask;

    bool isNeighRestoreDone();

private:
    bool isIntfStateOk(const std::string &alias);
    bool setNeighbor(const std::string& alias, const IpAddress& ip, const MacAddress& mac);

    vector<string> parseAliasIp(const string &app_db_nbr_tbl_key, const char *delimiter);

    void doResolveNeighTask(Consumer &consumer);
    void doSetNeighTask(Consumer &consumer);
    void doTask(Consumer &consumer);
    void doStateSystemNeighTask(Consumer &consumer);
    bool getVoqInbandInterfaceName(string &nbr_odev, string &ibiftype);
    bool addKernelRoute(string odev, IpAddress ip_addr);
    bool delKernelRoute(IpAddress ip_addr);
    bool addKernelNeigh(string odev, IpAddress ip_addr, MacAddress mac_addr);
    bool delKernelNeigh(string odev, IpAddress ip_addr);
    bool isIntfOperUp(const std::string &alias);
    unique_ptr<Table> m_cfgVoqInbandInterfaceTable;

    Table m_statePortTable, m_stateLagTable, m_stateVlanTable, m_stateIntfTable, m_stateNeighRestoreTable;
    struct nl_sock *m_nl_sock;
};

}

#endif // __NBRMGR__
