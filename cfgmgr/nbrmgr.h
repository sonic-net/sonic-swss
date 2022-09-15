#ifndef __NBRMGR__
#define __NBRMGR__

#include <string>
#include <map>
#include <set>
#include <time.h>

#include "dbconnector.h"
#include "producerstatetable.h"
#include "orch.h"
#include "netmsg.h"
#include "timestamp.h"


#define TIMEOUT_MIN_PERCENT         30
#define TIMEOUT_MAX_PERCENT         70

using namespace std;

namespace swss {


struct NeighCacheEntry
{
    MacAddress          mac_address;
    int                 state;
    unsigned long long  start_time;
    unsigned long long  refresh_timeout;
    int                 num_refresh_sent;
};

struct NeighCacheKey
{
    IpAddress       ip_address;     // neighbor IP address
    string          alias;          // Interface name

    bool operator<(const NeighCacheKey &o) const
    {
        return tie(ip_address, alias) < tie(o.ip_address, o.alias);
    }

    bool operator==(const NeighCacheKey &o) const
    {
        return ((ip_address == o.ip_address) && (alias == o.alias));
    }

    bool operator!=(const NeighCacheKey &o) const
    {
        return !(*this == o);
    }
};

typedef map<NeighCacheKey, NeighCacheEntry> NeighCacheTable;

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

    bool addNeighToCache(IpAddress ipAddress, string alias, string mac, int state);
    bool delNeighFrmCache(IpAddress ipAddress, string alias);
    bool updateNeighCache(string op, int type, IpAddress ip, string ifname, string mac, int state, int flags);
    bool isRefreshRequired(int type, IpAddress ip, string ifname, string mac, int state, int flags);
    void doNeighCacheTask(Consumer &consumer);
    unsigned long long get_currtime_ms(void);
    unsigned long long get_refresh_timeout(bool isV4);

    Table m_statePortTable, m_stateLagTable, m_stateVlanTable, m_stateIntfTable, m_stateNeighRestoreTable;
    struct nl_sock *m_nl_sock;

    NeighCacheTable m_neighCache;
    unsigned long long m_ref_timeout_v4;
    unsigned long long m_ref_timeout_v6;

};

}

#endif // __NBRMGR__
