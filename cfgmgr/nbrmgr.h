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

class NbrMgr : public Orch, public NetMsg
{
public:
    NbrMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);
    using Orch::doTask;

    bool isNeighRestoreDone();

    /* RTM_DELNEIGH / RTM_NEWLINK: put back static neighbours the kernel flushed */
    void onMsg(int nlmsg_type, struct nl_object *obj) override;

private:
    void reconcileNeighResolveTable(DBConnector *appDb);
    bool isIntfStateOk(const std::string &alias);
    bool setNeighbor(const std::string& alias, const IpAddress& ip, const MacAddress& mac);
    bool setFailedNeighborIncomplete(const std::string& alias, const IpAddress& ip);
    bool sendNeighborSolicitation(const std::string& alias, const IpAddress& ip);
    void processKernelFailedNeighbor(const std::string& key, const std::string& tableSeparator);

    vector<string> parseAliasIp(const string &app_db_nbr_tbl_key, const char *delimiter);

    void doResolveNeighTask(Consumer &consumer);
    void doKernelFailedNeighTask(Consumer &consumer);
    void doSetNeighTask(Consumer &consumer);
    void doTask(Consumer &consumer);
    void doStateSystemNeighTask(Consumer &consumer);
    bool getVoqInbandInterfaceName(string &nbr_odev, string &ibiftype);
    bool addKernelRoute(string odev, IpAddress ip_addr);
    bool delKernelRoute(IpAddress ip_addr);
    bool addKernelNeigh(string odev, IpAddress ip_addr, MacAddress mac_addr);
    bool delKernelNeigh(string odev, IpAddress ip_addr);
    bool isIntfOperUp(const std::string &alias);
    bool isNetdevUp(const std::string &alias);
    void reinstallStaticNeighbors(const std::vector<std::string> &cfgKeys);
    unique_ptr<Table> m_cfgVoqInbandInterfaceTable;

    Table m_statePortTable, m_stateLagTable, m_stateVlanTable, m_stateIntfTable, m_stateNeighRestoreTable;
    struct nl_sock *m_nl_sock;

    Table m_cfgNeighTable;
    /* "<alias>|<canonical ip>" -> CONFIG_DB NEIGH key, for entries with a MAC */
    std::map<std::string, std::string> m_staticNeigh;
    /* alias -> CONFIG_DB NEIGH keys flushed while the netdev was admin down */
    std::map<std::string, std::set<std::string>> m_flushedStatic;
};

}

#endif // __NBRMGR__
