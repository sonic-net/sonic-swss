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
#include "netlink.h"
#include "ipprefix.h"
#include "timestamp.h"


#define NUD_IN_TIMER (NUD_INCOMPLETE|NUD_REACHABLE|NUD_DELAY|NUD_PROBE|NUD_FAILED|NUD_STALE)

#define TIMEOUT_MIN_PERCENT         30
#define TIMEOUT_MAX_PERCENT         70

#define NEIGH_REFRESH_TX_THRESHOLD  1500        //Tx threshold per Refreshtimer iteration

#define NEIGH_REFRESH_TIMER_TICK    2           // 1sec conflicts with select timeout 
#define NEIGH_REFRESH_INTERVAL      (30/NEIGH_REFRESH_TIMER_TICK)

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

struct arphdr
{
    uint16_t ar_hdr;        // Hardware address Format = ETH
    uint16_t ar_pro;        // Protocol address Type = 0x800 for IP
    uint8_t  ar_hln;        // Size of Hardware address = 6 [MAC]
    uint8_t  ar_pln;        // Size of Protcol address = 4 [IP]
    uint16_t ar_op;         // ARP Opcode = ARP_REQUEST / ARP_REPLY

    uint8_t ar_sha[6];      // Sender Hardware Address
    uint8_t ar_sip[4];      // Sender IP address
    uint8_t ar_tha[6];      // Target Hardware Address
    uint8_t ar_tip[4];      // Target IP address
};

struct ethhdr
{
    uint8_t     dst[6];
    uint8_t     src[6];
    uint16_t    type;
};


class NbrMgr : public Orch
{
public:
    NbrMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);
    using Orch::doTask;

    bool isNeighRestoreDone();
    void refreshTimer();

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

    int buildArpPkt(unsigned char* pkt, IpAddress tgt_ip, MacAddress tgt_mac, IpAddress src_ip, MacAddress src_mac);
    bool sendRefresh(IpAddress ip, string ifname, MacAddress dstMac);
    bool neighRefreshInit(void);
    unsigned long long get_currtime_ms(void);
    unsigned long long get_refresh_timeout(bool isV4);
    bool is_time_elapsed_ms(unsigned long long time_to_chk, unsigned long long elaspe_time);
    MacAddress getSystemMac(void);
    MacAddress getIntfsMac(const string& ifname);
    string readLineFromFile(const string file, bool *exists);
    IpAddress getIntfIpAddress(string ifname, IpAddress dst_ip);

    Table m_statePortTable, m_stateLagTable, m_stateVlanTable, m_stateIntfTable, m_stateNeighRestoreTable;
    struct nl_sock *m_nl_sock;

    unsigned long long m_ref_timeout_v4;
    unsigned long long m_ref_timeout_v6;
    MacAddress m_system_mac;
    int m_sock_fd[5];

    NeighCacheTable m_neighCache;
};

}

#endif // __NBRMGR__
