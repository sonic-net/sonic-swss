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

struct IntfsEntry
{
    bool ipv6_enable_config = false;
    bool is_sag = false;
    bool is_ip_unnm = false;
    MacAddress source_mac;
    std::set<IpPrefix>  ip_addresses;
    string donar_intf;
    string vrf_name;
    int v4_addr_cnt;
    int v6_addr_cnt;
};

typedef map<string, IntfsEntry> IntfsTable;


class NbrMgr : public Orch
{
public:
    NbrMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<TableConnector> &tableNames);
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

    void doIpInterfaceTask(Consumer &consumer);
    void doSystemMacTask(Consumer &consumer);
    MacAddress getSystemMac(void);
    MacAddress getIntfsMac(const string& ifname);
    string getSubnetInterfaceName(const string &alias, const IpAddress &ip);
    IpAddress getIntfIpAddress(string ifname, IpAddress dst_ip);
    bool isPrefixSubnet(const IpAddress &ip, const string &alias);
    bool hasIntfIpAddress(string ifname, bool isV4);
    string readLineFromFile(const string file, bool *exists);

    Table m_statePortTable, m_stateLagTable, m_stateVlanTable, m_stateIntfTable, m_stateNeighRestoreTable;
    struct nl_sock *m_nl_sock;

    IntfsTable m_IntfsTable;
    MacAddress m_system_mac;

};

}

#endif // __NBRMGR__
