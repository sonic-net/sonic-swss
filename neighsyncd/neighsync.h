#ifndef __NEIGHSYNC__
#define __NEIGHSYNC__

#include "ipaddress.h"
#include "macaddress.h"

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"
#include "warmRestartAssist.h"
#include "ipprefix.h"
#include <string>
#include <time.h>
#include "timestamp.h"

using namespace std;

// The timeout value (in seconds) for neighsyncd reconcilation logic
#define DEFAULT_NEIGHSYNC_WARMSTART_TIMER 5

/*
 * This is the timer value (in seconds) that the neighsyncd waits for restore_neighbors
 * service to finish, should be longer than the restore_neighbors timeout value (110)
 * This should not happen, if happens, system is in a unknown state, we should exit.
 */
#define RESTORE_NEIGH_WAIT_TIME_OUT 600
#define TIMEOUT_MIN_PERCENT         30
#define TIMEOUT_MAX_PERCENT         70
#define REFERENCE_AGEOUT            600000      //10mins In MilliSeconds
#define DEFAULT_FDB_AGEOUT          600         //10mins In Seconds
#define DEFAULT_ARP_AGEOUT          1800        //30mins In Seconds
#define DEFAULT_NS_AGEOUT           1800        //30mins In seconds


#define NEIGH_REFRESH_TIMER_TICK 1
#define NEIGH_REFRESH_INTERVAL  (30/NEIGH_REFRESH_TIMER_TICK)

namespace swss {

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

struct NeighCacheVal
{
    string          mac_address;
    IpAddress       ip_address;     //neighbor IP address
    string          alias;          //Interface name
    int             state;          //state of entry in kernel
    int             flags;          //flags while receiving entry from Netlink
    int             type;           //Type of Netlink msg
};

struct NeighCacheEntry
{
    MacAddress          mac_address;
    int                 state;
    unsigned long long  curr_time;
    unsigned long long  refresh_timeout;
    string              lst_updated;
    string              lst_refresh_sent;
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

struct IntfsEntry
{
    bool ipv6_enable_config = false;
    bool is_ip_unnm = false;
    MacAddress source_mac;
    std::set<IpPrefix>  ip_addresses;
    string donar_intf;
};

typedef map<string, IntfsEntry> IntfsTable;

typedef map<NeighCacheKey, NeighCacheEntry> NeighCacheTable;

class NeighSync : public NetMsg
{
public:
    enum { MAX_ADDR_SIZE = 64 };

    NeighSync(RedisPipeline *pipelineAppDB, DBConnector *stateDb);
    ~NeighSync();

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

    bool isNeighRestoreDone();

    AppRestartAssist *getRestartAssist()
    {
        return m_AppRestartAssist;
    }

    void refreshTimer();
    bool neighRefreshInit(); // create sockets used to tx arp/ns
    bool sendRefresh(IpAddress ip, string ifname, MacAddress dst_mac);
    int buildArpPkt(unsigned char* pkt, IpAddress tgt_ip, MacAddress tgt_mac, IpAddress src_ip, MacAddress src_mac);
    int buildNSPkt(unsigned char* pkt, IpAddress ip, MacAddress mac);
    int buildIcmp6EchoReqPkt(unsigned char* pkt, IpAddress sip, IpAddress dip);
    bool updateNeighCache(int type, IpAddress ip, string ifname, string mac, int state, int flags);
    bool addNeighToCache(IpAddress ip, string ifname, string mac, int state);
    bool delNeighFrmCache(IpAddress ip, string ifname);
    bool isRefreshRequired(int type, IpAddress ip, string ifname, string mac, int state, int flags);
    bool hasNeigh(NeighCacheKey);
    MacAddress getSystemMac(void);
    bool isIpValid(const string& ipStr, const bool ipv4);
    void processIpInterfaceTask(std::deque<KeyOpFieldsValuesTuple> &entries);
    void doSystemMacTask(std::deque<KeyOpFieldsValuesTuple> &entries);
    void doSwitchTask(std::deque<KeyOpFieldsValuesTuple> &entries);
    void doNeighGlobalTask(std::deque<KeyOpFieldsValuesTuple> &entries);
    IpAddress getIntfIpAddress(string ifname, IpAddress dst_ip);
    IpAddress getV6LLIpaddr(MacAddress mac);
    unsigned long long get_currtime_ms(void);
    unsigned long long get_refresh_timeout(bool isV4);
    void setall_refresh_timeout(void);
    void updateReferenceAgeout(void);
    bool is_time_elapsed_ms(unsigned long long time_to_chk, unsigned long long elaspe_time);
    void startArpRefreshThread();
    static void* arpRefreshThread(void *arg);
    void processNeighfrmQueue(void);
    bool isNeighQueueEmpty(void);
    long unsigned int getNeighQueueSize(void);
    unsigned int ifNameToIndex(const char *name);
    void getNeighFrmQueue(NeighCacheVal *neigh);
    void addNeighToQueue(int type, IpAddress ip, string ifname, string mac, int state, int flags);
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;


private:
    Table m_stateNeighRestoreTable;
    ProducerStateTable m_neighTable;
    AppRestartAssist  *m_AppRestartAssist;
    int m_sock_fd[5];
    IntfsTable m_IntfsTable;
    MacAddress m_system_mac;
    NeighCacheTable m_neighCache;
    std::deque<NeighCacheVal> m_neighQueue;
    unsigned long long m_ref_timeout_v4;
    unsigned long long m_ref_timeout_v6;
    int m_ipv4_arp_timeout;
    int m_ipv6_nd_cache_expiry;
    int m_fdb_aging_time;
};

}

#endif
