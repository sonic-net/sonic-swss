#include <string>
#include <net/if.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <netlink/route/link.h>
#include <netlink/route/neighbour.h>
#include <net/ethernet.h>
#include <netpacket/packet.h>
#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "subscriberstatetable.h"
#include "ipaddress.h"
#include "netmsg.h"
#include "linkcache.h"
#include "macaddress.h"
#include "ipaddress.h"
#include "ipprefix.h"
#include "tokenize.h"
#include "neighsync.h"
#include "warm_restart.h"

using namespace std;
using namespace swss;

extern class NeighSync *g_neighsync;

const char config_db_key_delimiter = '|';
NeighSync* gDbgNeighSyncd;
NeighSync::NeighSync(RedisPipeline *pipelineAppDB, DBConnector *stateDb) :
    m_neighTable(pipelineAppDB, APP_NEIGH_TABLE_NAME),
    m_stateNeighRestoreTable(stateDb, STATE_NEIGH_RESTORE_TABLE_NAME)
{
    m_AppRestartAssist = new AppRestartAssist(pipelineAppDB, "neighsyncd", "swss", DEFAULT_NEIGHSYNC_WARMSTART_TIMER);
    if (m_AppRestartAssist)
    {
        m_AppRestartAssist->registerAppTable(APP_NEIGH_TABLE_NAME, &m_neighTable);
    }
    neighRefreshInit();
    m_ref_timeout_v4        = REFERENCE_AGEOUT;
    m_ref_timeout_v6        = REFERENCE_AGEOUT;
    m_fdb_aging_time        = DEFAULT_FDB_AGEOUT;
    m_ipv4_arp_timeout      = DEFAULT_ARP_AGEOUT;
    m_ipv6_nd_cache_expiry  = DEFAULT_NS_AGEOUT;


}

NeighSync::~NeighSync()
{
    if (m_AppRestartAssist)
    {
        delete m_AppRestartAssist;
    }
}

// Check if neighbor table is restored in kernel
bool NeighSync::isNeighRestoreDone()
{
    string value;

    m_stateNeighRestoreTable.hget("Flags", "restored", value);
    if (value == "true")
    {
        SWSS_LOG_NOTICE("neighbor table restore to kernel is done");
        return true;
    }
    return false;
}

void NeighSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    char ipStr[MAX_ADDR_SIZE + 1] = {0};
    char macStr[MAX_ADDR_SIZE + 1] = {0};
    struct rtnl_neigh *neigh = (struct rtnl_neigh *)obj;
    string key;
    string family;
    string ifName, ifNameRcvd;

    if ((nlmsg_type != RTM_NEWNEIGH) && (nlmsg_type != RTM_GETNEIGH) &&
        (nlmsg_type != RTM_DELNEIGH))
        return;

    ifNameRcvd = ifName = LinkCache::getInstance().ifindexToName(rtnl_neigh_get_ifindex(neigh));

    if (rtnl_neigh_get_family(neigh) == AF_INET)
        family = IPV4_NAME;
    else if (rtnl_neigh_get_family(neigh) == AF_INET6)
        family = IPV6_NAME;
    else
        return;

    key+= LinkCache::getInstance().ifindexToName(rtnl_neigh_get_ifindex(neigh));
    key+= ":";

    nl_addr2str(rtnl_neigh_get_dst(neigh), ipStr, MAX_ADDR_SIZE);
    /* Ignore IPv6 link-local addresses as neighbors */
    if (family == IPV6_NAME && IN6_IS_ADDR_LINKLOCAL(nl_addr_get_binary_addr(rtnl_neigh_get_dst(neigh))))
        return;
    /* Ignore IPv6 multicast link-local addresses as neighbors */
    if (family == IPV6_NAME && IN6_IS_ADDR_MC_LINKLOCAL(nl_addr_get_binary_addr(rtnl_neigh_get_dst(neigh))))
        return;
    key+= ipStr;

    int state = rtnl_neigh_get_state(neigh);
    if (state == NUD_NOARP)
    {
        return;
    }

    bool delete_key = false;
    int flags = rtnl_neigh_get_flags(neigh);
    if ((nlmsg_type == RTM_DELNEIGH) || (state == NUD_INCOMPLETE) ||
        (state == NUD_FAILED))
    {
	    delete_key = true;
    }

    nl_addr2str(rtnl_neigh_get_lladdr(neigh), macStr, MAX_ADDR_SIZE);
    if (!delete_key && !strcmp(macStr,"none"))
    {
        SWSS_LOG_NOTICE("No MAC address received for neighbor %s", ipStr);
        return;
    }
 

    /* Ignore neighbor entries with Broadcast Mac - Trigger for directed broadcast */
    if (!delete_key && (MacAddress(macStr) == MacAddress("ff:ff:ff:ff:ff:ff")))
    {
        SWSS_LOG_INFO("Broadcast Mac received, ignoring for %s", ipStr);
        return;
    }

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple f("family", family);
    FieldValueTuple nh("neigh", macStr);
    fvVector.push_back(nh);
    fvVector.push_back(f);

    // If warmstart is in progress, we take all netlink changes into the cache map
    if (m_AppRestartAssist->isWarmStartInProgress())
    {
        m_AppRestartAssist->insertToMap(APP_NEIGH_TABLE_NAME, key, fvVector, delete_key);
    }
    else
    {
        if (delete_key == true)
        {
            m_neighTable.del(key);
        }
        else
        {
            m_neighTable.set(key, fvVector);
        }
    }

    //Add Neigh entries to cache irrespective of warm reboot status
    addNeighToQueue(nlmsg_type, IpAddress(ipStr), ifNameRcvd.c_str(), macStr, state, flags);
}

bool NeighSync::neighRefreshInit()
{
    int ttl = 255, val = 1;

    //Socket to send ARP request packet
    m_sock_fd[0] = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if(m_sock_fd[0] < 0)
    {
        SWSS_LOG_ERROR("NeighSynCache: Arp Refresh socket create failed %d, err(%d):%s ",m_sock_fd[0], errno, strerror(errno));
        return false;
    }

    //Socket to send ICMPv6 NS packet
    m_sock_fd[1] = socket (AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    if(m_sock_fd[1] < 0)
    {
        SWSS_LOG_ERROR("NeighSynCache: IPv6 Neigh Refresh socket create failed %d, err(%d):%s ",m_sock_fd[1], errno, strerror(errno));
        return false;
    }

    // set socket options
    setsockopt (m_sock_fd[1], IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &ttl, sizeof(ttl));

    //Socket to send ICMPv6 Echo Request packet
    m_sock_fd[2] = socket (AF_INET6, SOCK_RAW, IPPROTO_IPV6);
    if(m_sock_fd[2] < 0)
    {
        SWSS_LOG_ERROR("NeighSynCache: IPv6 Neigh Refresh socket create failed %d, err(%d):%s ",m_sock_fd[2], errno, strerror(errno));
        return false;
    }

    // set socket options to include ipv6hdr from application
    setsockopt (m_sock_fd[2], IPPROTO_IPV6, IPV6_HDRINCL, &val, sizeof(val));

    return true;
}

int NeighSync::buildArpPkt(unsigned char* pkt, IpAddress tgt_ip, MacAddress tgt_mac, IpAddress src_ip, MacAddress src_mac)
{
    struct ethhdr eth_hdr;
    struct arphdr arp_hdr;
    uint32_t ipaddr = 0;
    //uint8_t broadcast_mac[6] = {0xff,0xff,0xff,0xff,0xff,0xff };
    uint8_t *pkt_buff = pkt;

    memset(&eth_hdr, 0x0, sizeof(eth_hdr));
    memcpy(&eth_hdr.src, src_mac.getMac(), 6);
    //memcpy(&eth_hdr.dst, &broadcast_mac, 6);
    memcpy(&eth_hdr.dst, tgt_mac.getMac(), 6);
    eth_hdr.type = htons(0x806);
    memcpy (pkt_buff, &eth_hdr, sizeof(eth_hdr));

    pkt_buff = pkt_buff + sizeof(eth_hdr);
    memset(&arp_hdr, 0x0, sizeof(arp_hdr));
    arp_hdr.ar_hdr = htons(0x1);
    arp_hdr.ar_pro = htons(0x800);
    arp_hdr.ar_hln = 6;
    arp_hdr.ar_pln = 4;
    arp_hdr.ar_op  = htons(0x1);

    memcpy( &arp_hdr.ar_sha, src_mac.getMac(), 6);
    ipaddr = src_ip.getV4Addr();
    memcpy( &arp_hdr.ar_sip, (uint8_t*)(&ipaddr), 4);
    ipaddr = tgt_ip.getV4Addr();
    memcpy( &arp_hdr.ar_tip, (uint8_t*)(&ipaddr), 4);

    memcpy(pkt_buff, &arp_hdr, sizeof(arp_hdr));

    return (sizeof(eth_hdr) +sizeof(arp_hdr));
}

int NeighSync::buildNSPkt(unsigned char* pkt, IpAddress ip, MacAddress mac)
{
    struct nd_neighbor_solicit ns_hdr;
    struct nd_opt_hdr opt_hdr = {0,0};
    uint8_t mac_addr[6];
    uint8_t *pkt_buff = pkt;

    memset(&ns_hdr, 0x0, sizeof(ns_hdr));

    ns_hdr.nd_ns_type       = ND_NEIGHBOR_SOLICIT;
    ns_hdr.nd_ns_code       = 0;
    ns_hdr.nd_ns_cksum      = 0;
    ns_hdr.nd_ns_reserved   = 0;
    memcpy (&ns_hdr.nd_ns_target, (ip.getIp().ip_addr.ipv6_addr), 16);
    memcpy (pkt_buff, &ns_hdr, sizeof(ns_hdr));

    pkt_buff = pkt_buff + sizeof(ns_hdr);
    opt_hdr.nd_opt_type = ND_OPT_SOURCE_LINKADDR;
    opt_hdr.nd_opt_len  = 1;
    memcpy (pkt_buff, &opt_hdr, sizeof(opt_hdr));

    pkt_buff = pkt_buff + sizeof(opt_hdr);
    mac.getMac(mac_addr);
    memcpy (pkt_buff, mac_addr, sizeof(mac_addr));

    return (sizeof(ns_hdr) + sizeof(opt_hdr) + sizeof(mac_addr));
}

int NeighSync::buildIcmp6EchoReqPkt(unsigned char* pkt, IpAddress sip, IpAddress dip)
{
    uint8_t *pkt_buff = pkt;
    struct ip6_hdr ip6hdr;
    struct icmp6_hdr icmp6hdr;

    memset(&ip6hdr, 0x0, sizeof(ip6hdr));
    memset(&icmp6hdr, 0x0, sizeof(icmp6hdr));

    ip6hdr.ip6_nxt        = 58;
    ip6hdr.ip6_hlim       = 255;
    ip6hdr.ip6_plen       = htons(sizeof(icmp6hdr));
    ip6hdr.ip6_vfc        = 0x6 << 4;
    memcpy (&ip6hdr.ip6_src, (sip.getIp().ip_addr.ipv6_addr), 16);
    memcpy (&ip6hdr.ip6_dst, (dip.getIp().ip_addr.ipv6_addr), 16);
    memcpy (pkt_buff, &ip6hdr, sizeof(ip6hdr));

    pkt_buff = pkt_buff + sizeof(ip6hdr);

    icmp6hdr.icmp6_type   = ICMP6_ECHO_REQUEST;
    memcpy( &icmp6hdr.icmp6_data8, "\x0a\xbb\xcc\xdd", 4);
    memcpy (pkt_buff, &icmp6hdr, sizeof(icmp6hdr));

    return (sizeof(ip6hdr) + sizeof(icmp6hdr));
}

unsigned int NeighSync::ifNameToIndex(const char *name)
{
  unsigned idx;
  int saved_errno = 0;

  if (name == NULL)
  {
    SWSS_LOG_ERROR("Interface name passed NULL\n");
    return false;
  }

  idx = if_nametoindex(name);

  if (idx == 0)
  {
    saved_errno = errno;
    sscanf(name, "if%u", &idx);
  }

  SWSS_LOG_DEBUG("Interface name %s has index %u ernno %d\n", name, idx, saved_errno);

  return idx;
}


bool NeighSync::sendRefresh(IpAddress ip, string ifname, MacAddress dstMac)
{
    MacAddress src_mac;
    IpAddress src_ip;
    uint8_t pkt[256];
    int pkt_len = 0;
    long int ret = 0;

    uint32_t ifidx = 0;
    struct rtnl_link *link = NULL;
    char addrStr[MAX_ADDR_SIZE+1] = {0};

    // Get Source Interface MAC from Outgoing interface
    link = LinkCache::getInstance().getLinkByName(ifname.c_str());
    if (!link)
    {
        SWSS_LOG_ERROR("NeighSynCache: IFname to MacAddr failed %s", ifname.c_str());
        return false;
    }
    nl_addr2str(rtnl_link_get_addr(link), addrStr, MAX_ADDR_SIZE);
    src_mac = MacAddress(addrStr);

    ifidx = ifNameToIndex(ifname.c_str());
    
    if(!ifidx)
    {
        SWSS_LOG_ERROR("NeighSynCache: IFname to ifidx failed %s", ifname.c_str());
        return false;
    }

    if((!ip.isV4()) && (ip.getAddrScope() == IpAddress::LINK_SCOPE))
    {
        //create linklocal address
        src_ip = getV6LLIpaddr(src_mac);
    }
    else
    {
        src_ip = getIntfIpAddress(ifname, ip);
        if(src_ip == IpAddress("0.0.0.0"))
        {
            SWSS_LOG_ERROR("NeighSynCache: Src IP Not Found: Ifindex - %s (%d) Src-mac - %s, Tgt-ip - %s, Tgt-mac - %s ",
                    ifname.c_str(),ifidx, src_mac.to_string().c_str(), ip.to_string().c_str(), dstMac.to_string().c_str());

            return false;
        }
    }

    SWSS_LOG_INFO("NeighSynCache: Ifindex - %s (%d) Src-ip - %s, Src-mac - %s, Tgt-ip - %s, Tgt-mac - %s ",
    ifname.c_str(),ifidx, src_ip.to_string().c_str(), src_mac.to_string().c_str(), ip.to_string().c_str(), dstMac.to_string().c_str());

    if(ip.getIp().family == AF_INET)
    {
        struct sockaddr_ll dst;

        //build the ARP packet
        pkt_len =  buildArpPkt(pkt, ip, dstMac, src_ip, src_mac);

        //Send the packet
        memset (&dst, 0x0, sizeof(dst));
        dst.sll_family      = AF_PACKET;
        dst.sll_ifindex     = ifidx;
        dst.sll_protocol    = htons(ETH_P_ALL);
        dst.sll_halen       = ETH_ALEN;
        //set dst mac
        dstMac.getMac(dst.sll_addr);
        //memcpy (&dst.sll_addr, "\xff\xff\xff\xff\xff\xff", 6);

        ret = sendto(m_sock_fd[0], pkt, pkt_len, 0, (struct sockaddr *)&dst, sizeof(dst));
        if(ret < 0)
            SWSS_LOG_ERROR("NeighSynCache: Arp Send failed for IP %s intf %s, err:%s", ip.to_string().c_str(), ifname.c_str(), strerror(errno));
        else
            SWSS_LOG_DEBUG("NeighSynCache: Arp Sent for IP %s intf %s, ret %ld ", ip.to_string().c_str(), ifname.c_str(), ret);

    }
    else if (ip.getIp().family == AF_INET6)
    {
        struct sockaddr_in6 dst;

        memset (&dst, 0x0, sizeof(dst));
        dst.sin6_scope_id           = ifidx;
        dst.sin6_family             = AF_INET6;

        if(!(dstMac ==  MacAddress("FF:FF:FF:FF:FF:FF")))
        {
            //build the NS packet
            pkt_len = buildNSPkt(pkt, ip, src_mac);

            struct ip_addr_t ipaddr = ip.getIp();
            memcpy (&dst.sin6_addr.s6_addr, "\xff\x02\x00\x00\x00\x00\x00\x00"
                    "\x00\x00\x00\x01\xff", 13);
            dst.sin6_addr.s6_addr[13]   = ipaddr.ip_addr.ipv6_addr[13];
            dst.sin6_addr.s6_addr[14]   = ipaddr.ip_addr.ipv6_addr[14];
            dst.sin6_addr.s6_addr[15]   = ipaddr.ip_addr.ipv6_addr[15];

            //Send the NS packet
            ret = sendto (m_sock_fd[1], &pkt, pkt_len, MSG_DONTROUTE,
                    (const struct sockaddr *)&dst, sizeof (dst));
            if(ret < 0)
                SWSS_LOG_ERROR("NeighSynCache: NS Send failed for IP %s intf %s, err:%s", ip.to_string().c_str(), ifname.c_str(), strerror(errno));
            else
                SWSS_LOG_DEBUG("NeighSynCache: NS Sent for IP %s intf %s, ret %ld ", ip.to_string().c_str(), ifname.c_str(), ret);
        }
        else
        {
            //For entries in failed state trigger NS/NA frm kernel, tx over vrf interface needs BINDTODEVICE option
            ret = setsockopt(m_sock_fd[2], SOL_SOCKET, SO_BINDTODEVICE, ifname.c_str(), (unsigned int)strlen(ifname.c_str())+1);
            if(ret < 0)
                SWSS_LOG_ERROR("NeighSynCache: setsockopt for IP %s intf %s, err:%s", ip.to_string().c_str(), ifname.c_str(), strerror(errno)); 

            //build the icmp6 echo request packet
            pkt_len = buildIcmp6EchoReqPkt(pkt, src_ip, ip);

            memcpy (&dst.sin6_addr.s6_addr, (ip.getIp().ip_addr.ipv6_addr), 16);

            //Send the NS packet
            ret = sendto (m_sock_fd[2], &pkt, pkt_len, 0,
                    (const struct sockaddr *)&dst, sizeof (dst));
            if(ret < 0)
                SWSS_LOG_ERROR("NeighSynCache: Icmp6 EchoReq Send failed for IP %s intf %s, err:%s", ip.to_string().c_str(), ifname.c_str(), strerror(errno));
            else
                SWSS_LOG_DEBUG("NeighSynCache: Icmp6 EchoReq Sent for IP %s intf %s, ret %ld ", ip.to_string().c_str(), ifname.c_str(), ret);
        }
    }
    else
    {
        SWSS_LOG_ERROR("NeighSynCache: Invalid IP %s", ip.to_string().c_str());
        return false;
    }

    return true;
}

/*bool NeighSync::hasNeigh(NeighCacheKey neigh)
 * {
 *     return m_neighCache.find(neigh) != m_neighCache.end();
 * }
 */


void NeighSync::addNeighToQueue(int type, IpAddress ip, string ifname, string mac, int state, int flags)
{
    NeighCacheVal c_entry;
    c_entry.type            = type;
    c_entry.ip_address      = ip;
    c_entry.alias           = ifname;
    c_entry.mac_address     = mac;
    c_entry.state           = state;
    c_entry.flags           = flags;

    if(!isRefreshRequired(type, ip, ifname, mac, state, flags))
        return;

    pthread_mutex_lock(&mutex);
    m_neighQueue.push_back(c_entry);
    pthread_mutex_unlock(&mutex);
}

void NeighSync::getNeighFrmQueue(NeighCacheVal *neigh)
{
    pthread_mutex_lock(&g_neighsync->mutex);
    *neigh = m_neighQueue.front();
    m_neighQueue.pop_front();
    pthread_mutex_unlock(&g_neighsync->mutex);
}

long unsigned int NeighSync::getNeighQueueSize(void)
{
    long unsigned int qsize = 0;
    pthread_mutex_lock(&g_neighsync->mutex);
    qsize = m_neighQueue.size();
    pthread_mutex_unlock(&g_neighsync->mutex);
    return qsize;
}

bool NeighSync::isNeighQueueEmpty(void)
{
    bool is_empty = false;
    pthread_mutex_lock(&g_neighsync->mutex);
    is_empty = m_neighQueue.empty();
    pthread_mutex_unlock(&g_neighsync->mutex);
    return is_empty;
}

void NeighSync::processNeighfrmQueue(void)
{
    if(!isNeighQueueEmpty())
    {
        long unsigned int cnt = getNeighQueueSize();

        while(cnt--)
        {
            NeighCacheVal neigh;

            getNeighFrmQueue(&neigh);

            updateNeighCache(neigh.type, neigh.ip_address, neigh.alias, neigh.mac_address, neigh.state, neigh.flags);
        }
    }

    return;
}

bool NeighSync::isRefreshRequired(int type, IpAddress ip, string ifname, string mac, int state, int flags)
{
    /* Ignore neighbors received from eth0 interfaces */
    if ((ifname.compare(0, strlen("eth"), "eth") == 0 ) ||
       (ifname.compare(0, strlen("lo"), "lo") == 0 )) 
    {
        SWSS_LOG_INFO("NeighSynCache: Ignore neighbor(%s) interface(%s)",ip.to_string().c_str(), ifname.c_str());
        return false;
    }

    //Skip IPv4 LL entries created for BGP unnumbered, correponding ipv6ll will be refreshed
    if(ip.getAddrScope() == IpAddress::LINK_SCOPE)
    {
        if(ip.isV4())
        {
            SWSS_LOG_INFO("NeighSynCache: Ignore Ipv4LL neighbor(%s) interface(%s)",ip.to_string().c_str(), ifname.c_str());
            return false; 
        }
    }

    if(state == NUD_NOARP)
    {
        delNeighFrmCache(ip, ifname);
        SWSS_LOG_INFO("NeighSynCache: Ignore NUD_NOARP neighbor(%s) interface(%s)",ip.to_string().c_str(), ifname.c_str());
        return false;
    }

    return true;
}

bool NeighSync::updateNeighCache(int type, IpAddress ip, string ifname, string mac, int state, int flags)
{
    bool del = false;

    SWSS_LOG_INFO("NeighSynCache: Update neighbor(%s) intf(%s) mac(%s), state(%d), flags(%d), type(%d)",
                    ip.to_string().c_str(), ifname.c_str(), mac.c_str(), state, flags, type);

    if (type == RTM_DELNEIGH) /*|| (state == NUD_INCOMPLETE) ||
        (state == NUD_FAILED))*/
    {
        del = true;
    }

    if (state == NUD_PERMANENT && flags & NTF_EXT_LEARNED)
    {
        SWSS_LOG_NOTICE("NeighSynCache: neighbor(%s) intf(%s) learned from remote nodes ",ip.to_string().c_str(), ifname.c_str());
        del = true;
    }

    if (!del)
    {
        //In case of Failed entries MAC may not be present, but we want to store them and refresh them
        if(!strcmp(mac.c_str(),"none")) 
        {
            if(!(state == NUD_FAILED) || (state == NUD_INCOMPLETE))
            {
                SWSS_LOG_NOTICE("NeighSynCache: Ignore this update as no MAC address received for neighbor %s", ip.to_string().c_str());
                return true;
            }
        }
        else //MAC is not none
        {
            /* Ignore neighbor entries with Broadcast Mac - Trigger for directed broadcast */
            if(MacAddress(mac) == MacAddress("ff:ff:ff:ff:ff:ff"))
            {
                SWSS_LOG_NOTICE("NeighSynCache: Broadcast Mac recieved, deleting %s", ip.to_string().c_str());
                del = true;
            }
        }
    }

    if (del)
    {
        delNeighFrmCache(ip, ifname);
    }
    else
    {
        addNeighToCache(ip, ifname, mac, state);
    }

    return true;
}

bool NeighSync::addNeighToCache(IpAddress ipAddress, string alias, string mac, int state)
{
    NeighCacheKey neigh = { ipAddress, alias };

    NeighCacheEntry neigh_cache_entry;
    if((state == NUD_FAILED) || (state == NUD_INCOMPLETE))
        neigh_cache_entry.mac_address       = MacAddress("ff:ff:ff:ff:ff:ff");
    else
        neigh_cache_entry.mac_address       = MacAddress(mac);

    neigh_cache_entry.state             = state;
    neigh_cache_entry.curr_time         = get_currtime_ms();
    neigh_cache_entry.refresh_timeout   = get_refresh_timeout(ipAddress.isV4());
    neigh_cache_entry.lst_updated       = getTimestamp();
    neigh_cache_entry.num_refresh_sent  = 0;
    m_neighCache[neigh] = neigh_cache_entry;

    SWSS_LOG_NOTICE("NeighSynCache: Add neighbor %s interface %s MAC:%s state:(%d) curr_time(%lld), timeout(%lld)", 
                    ipAddress.to_string().c_str(), alias.c_str(), mac.c_str(), state, neigh_cache_entry.curr_time,
                    neigh_cache_entry.refresh_timeout);

    return true;
}

bool NeighSync::delNeighFrmCache(IpAddress ipAddress, string alias)
{
    NeighCacheKey neigh = { ipAddress, alias };

    if(m_neighCache.find(neigh) == m_neighCache.end())
    {
        SWSS_LOG_NOTICE("NeighSynCache: EntryNotFound - neighbor %s interface %s ", ipAddress.to_string().c_str(), alias.c_str() );
        return false;
    }

    SWSS_LOG_NOTICE("NeighSynCache: Del neighbor %s interface %s ", ipAddress.to_string().c_str(), alias.c_str());

    m_neighCache.erase(neigh);

    return true;
}

void NeighSync::refreshTimer()
{
    int num_refresh = 0;
    static int timer = 0;

    timer++;

    processNeighfrmQueue();

    if(!(timer % NEIGH_REFRESH_INTERVAL))
    {
        SWSS_LOG_INFO("NeighSynCache: Neighbor Refresh Timeout - Start");

        auto it = m_neighCache.begin();
        while (it != m_neighCache.end())
        {
            if(is_time_elapsed_ms(it->second.curr_time, it->second.refresh_timeout))
            {
                sendRefresh(it->first.ip_address , it->first.alias, it->second.mac_address);
                it->second.curr_time = get_currtime_ms();
                it->second.refresh_timeout = get_refresh_timeout(it->first.ip_address.isV4());
                it->second.lst_refresh_sent = getTimestamp();
                it->second.num_refresh_sent++;
                num_refresh++;
            }
            it++;
        }
        timer = 0;
        SWSS_LOG_INFO("NeighSynCache: Neighbor Refresh Timeout - End, total sent - %d", num_refresh);
    }
    return;
}

unsigned long long NeighSync::get_currtime_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    {
        return ((((unsigned long long)ts.tv_sec) * 1000) + (((unsigned long long)ts.tv_nsec)/1000000));
    }
    else
    {
        SWSS_LOG_NOTICE("NeighSynCache: GetTime Failed");
    }

    return 0;
}

unsigned long long NeighSync::get_refresh_timeout(bool isV4)
{
    if(isV4)
    {
        return (((1 + rand ()) % (((TIMEOUT_MAX_PERCENT - TIMEOUT_MIN_PERCENT) * m_ref_timeout_v4 )/100)) +  ((TIMEOUT_MIN_PERCENT * m_ref_timeout_v4)/100));
    }
    else
    {
        return (((1 + rand ()) % (((TIMEOUT_MAX_PERCENT - TIMEOUT_MIN_PERCENT) * m_ref_timeout_v6 )/100)) +  ((TIMEOUT_MIN_PERCENT * m_ref_timeout_v6)/100));
    }
}

void NeighSync::setall_refresh_timeout(void)
{
    SWSS_LOG_NOTICE("NeighSynCache: ChangeAllNeighRefreshTimeout- Start");

    auto it = m_neighCache.begin();
    while (it != m_neighCache.end())
    {
        it->second.curr_time = get_currtime_ms();
        it->second.refresh_timeout = get_refresh_timeout(it->first.ip_address.isV4());
        it++;
    }

    SWSS_LOG_NOTICE("NeighSynCache: ChangeAllNeighRefreshTimeout- End");
}

void NeighSync::updateReferenceAgeout(void)
{
    // fdb/v4/v6 ageout are in sec, convert to ms
    if(m_fdb_aging_time)
    {
        m_ref_timeout_v4 = ((m_fdb_aging_time <= m_ipv4_arp_timeout) ? (m_fdb_aging_time*1000) : (m_ipv4_arp_timeout*1000));
        m_ref_timeout_v6 = ((m_fdb_aging_time <= m_ipv6_nd_cache_expiry) ? (m_fdb_aging_time*1000) : (m_ipv6_nd_cache_expiry*1000));
    }
    else
    {
        //If fdb_aging_time is 0 then use only arp/nd_timeout
        m_ref_timeout_v4 = (m_ipv4_arp_timeout*1000);
        m_ref_timeout_v6 = (m_ipv6_nd_cache_expiry*1000);
    }

    SWSS_LOG_NOTICE("NeighSynCache: FdbAge(%ds), v4Age(%ds), v6Age(%ds), v4RefAge(%lldms), v6RefAge(%lldms)",
                    m_fdb_aging_time, m_ipv4_arp_timeout, m_ipv6_nd_cache_expiry, m_ref_timeout_v4, m_ref_timeout_v6);

    //Update refresh timeout in existing neighbor cache
    setall_refresh_timeout();
}

bool NeighSync::is_time_elapsed_ms(unsigned long long time_to_chk, unsigned long long elaspe_time)
{
    unsigned long long curr_time_ms = 0;

    curr_time_ms = get_currtime_ms();
    if(curr_time_ms)
    {
        if((curr_time_ms - time_to_chk) > elaspe_time)
            return true;
    }

    return false;
}


//Interface Cache Management
bool NeighSync::isIpValid(const string& ipStr, const bool ipv4)
{
    if (ipv4 == true)
    {
        if (ipStr.find(':') != std::string::npos)
        {
            return false;
        }
        else
        {
            return true;
        }
    }
    else
    {
        if (ipStr.find(':') != std::string::npos)
        {
            return true;
        }
        else
        {
            return false;
        }
    }
}

void NeighSync::processIpInterfaceTask(std::deque<KeyOpFieldsValuesTuple> &entries)
{
    if (entries.empty())
    {
        return;
    }

    for (auto entry: entries)
    {
        vector<string> keys = tokenize(kfvKey(entry), config_db_key_delimiter);
        string op = kfvOp(entry);
        string key = kfvKey(entry);

        if (keys.size() == 2)
        {
            string alias(keys[0]);
            IpPrefix ip_prefix(keys[1]);

            if (op == SET_COMMAND)
            {
                SWSS_LOG_NOTICE(" SET commands received with interace %s IP address %s", alias.c_str(), ip_prefix.to_string().c_str());
                if (m_IntfsTable.find(alias) == m_IntfsTable.end())
                {
                    IntfsEntry intfs_entry;
                    m_IntfsTable[alias] = intfs_entry;
                }
                m_IntfsTable[alias].ip_addresses.insert(ip_prefix);
                m_IntfsTable[alias].source_mac = m_system_mac;
            }
            else if (op == DEL_COMMAND)
            {
                SWSS_LOG_NOTICE(" DEL commands received with interace %s IP address %s", alias.c_str(), ip_prefix.to_string().c_str());
                if (m_IntfsTable.find(alias) != m_IntfsTable.end())
                {
                    if(m_IntfsTable[alias].ip_addresses.find(ip_prefix) != (m_IntfsTable[alias].ip_addresses.end()))
                    {
                        m_IntfsTable[alias].ip_addresses.erase(ip_prefix);
                        if(m_IntfsTable[alias].ip_addresses.size() == 0 && !(m_IntfsTable[alias].ipv6_enable_config))
                        {
                           m_IntfsTable.erase(alias);
                        }
                    }
                }
            }
        }
        else if (keys.size() == 1)
        {
            string alias(keys[0]);
            const vector<FieldValueTuple>& data = kfvFieldsValues(entry);
            string ipv6_link_local_mode = "";
            string donar_intf = "";
            bool is_ip_unnm = false;
            for (auto idx : data)
            {
                const auto &field = fvField(idx);
                const auto &value = fvValue(idx);
                if (field == "ipv6_use_link_local_only")
                {
                    ipv6_link_local_mode = value;
                    break;
                }
                if (field == "unnumbered")
                {
                    is_ip_unnm = true;
                    donar_intf = value;
                    break;
                }
            }
            if (op == SET_COMMAND)
            {
                if((ipv6_link_local_mode == "enable") || is_ip_unnm)
                {
                    if (m_IntfsTable.find(alias) == m_IntfsTable.end())
                    {
                        IntfsEntry intfs_entry;
                        m_IntfsTable[alias] = intfs_entry;
                    }

                    if (ipv6_link_local_mode == "enable")
                        m_IntfsTable[alias].ipv6_enable_config = true;

                    if(is_ip_unnm)
                        m_IntfsTable[alias].donar_intf = donar_intf;
                        
                    m_IntfsTable[alias].is_ip_unnm = is_ip_unnm;
                    m_IntfsTable[alias].source_mac = m_system_mac;
                }
            }
            if (op == DEL_COMMAND || ipv6_link_local_mode == "disable")
            {
                if (m_IntfsTable.find(alias) != m_IntfsTable.end())
                {
                    if(m_IntfsTable[alias].ip_addresses.size() == 0)
                    {
                        m_IntfsTable.erase(alias);
                    }
                    else
                    {
                        m_IntfsTable[alias].ipv6_enable_config = false;
                    }
                }
            }
        }
    }
    return;
}

void NeighSync::doSystemMacTask(std::deque<KeyOpFieldsValuesTuple> &entries)
{
    if (entries.empty())
    {
        return;
    }

    SWSS_LOG_ENTER();
    for (auto entry: entries)
    {
        vector<string> keys = tokenize(kfvKey(entry), config_db_key_delimiter);
        string op = kfvOp(entry);
        string key = kfvKey(entry);

        if (op == SET_COMMAND)
        {
            for (auto i : kfvFieldsValues(entry))
            {
                SWSS_LOG_INFO("Field: %s Val %s", fvField(i).c_str(), fvValue(i).c_str());
                if (fvField(i) == "mac") {
                    m_system_mac = MacAddress(fvValue(i));
                    SWSS_LOG_NOTICE("System MAC %s", m_system_mac.to_string().c_str());

                    auto itr = m_IntfsTable.begin();
                    while (itr != m_IntfsTable.end())
                    {
                        itr++;
                    }

                    break;
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            m_system_mac = MacAddress();
        }
    }
}

MacAddress NeighSync::getSystemMac(void)
{
    return m_system_mac;
}

IpAddress NeighSync::getV6LLIpaddr(MacAddress mac)
{
    const uint8_t *mac_addr = mac.getMac();
    uint8_t        eui64_id[8];
    char           ipv6_addr[INET6_ADDRSTRLEN] = {0};

    eui64_id[0] = mac_addr[0] ^ 0x02;
    eui64_id[1] = mac_addr[1];
    eui64_id[2] = mac_addr[2];
    eui64_id[3] = 0xff;
    eui64_id[4] = 0xfe;
    eui64_id[5] = mac_addr[3];
    eui64_id[6] = mac_addr[4];
    eui64_id[7] = mac_addr[5];

    snprintf(ipv6_addr, INET6_ADDRSTRLEN, "fe80::%02x%02x:%02x%02x:%02x%02x:%02x%02x",
             eui64_id[0], eui64_id[1], eui64_id[2], eui64_id[3], eui64_id[4], eui64_id[5],
             eui64_id[6], eui64_id[7]);

    return IpAddress(string(ipv6_addr));
}

IpAddress NeighSync::getIntfIpAddress(string ifname, IpAddress dst_ip)
{
    //Get Interface from Interface Cache
    auto it =  m_IntfsTable.find(ifname);

    if(it != m_IntfsTable.end())
    {
        struct IntfsEntry intfs = it->second;
        if (intfs.is_ip_unnm)
        {
            auto it_unnm  =  m_IntfsTable.find(intfs.donar_intf);
            if(it_unnm !=  m_IntfsTable.end())
            {
                for (auto &prefixIt: m_IntfsTable[intfs.donar_intf].ip_addresses)
                {
                    return prefixIt.getIp();
                }
            }
        }
        else
        {
            for (auto &prefixIt: m_IntfsTable[ifname].ip_addresses)
            {
                if (prefixIt.isAddressInSubnet(dst_ip))
                {
                    return prefixIt.getIp();
                }
            }
        }
    }
    return IpAddress("0.0.0.0");
}

void NeighSync::doNeighGlobalTask(std::deque<KeyOpFieldsValuesTuple> &entries)
{
    int ipv4_arp_timeout = DEFAULT_ARP_AGEOUT;
    int ipv6_nd_cache_expiry = DEFAULT_NS_AGEOUT;
    if (entries.empty())
    {
        return;
    }
    for (auto entry: entries)
    {
        vector<string> keys = tokenize(kfvKey(entry), config_db_key_delimiter);
        string op = kfvOp(entry);
        string key = kfvKey(entry);
        for (auto i : kfvFieldsValues(entry))
        {
            if (fvField(i) == "ipv4_arp_timeout")
            {
                ipv4_arp_timeout = stoi(fvValue(i).c_str());
            }
            else if (fvField(i) == "ipv6_nd_cache_expiry")
            {
                ipv6_nd_cache_expiry = stoi(fvValue(i).c_str());
            }
        }
    }
    m_ipv4_arp_timeout = ipv4_arp_timeout;
    m_ipv6_nd_cache_expiry = ipv6_nd_cache_expiry;
    SWSS_LOG_NOTICE(" ipv4_arp_timeout %d ipv6_nd_cache_expiry %d", ipv4_arp_timeout, ipv6_nd_cache_expiry);

    updateReferenceAgeout();
    return;
}

void NeighSync::doSwitchTask(std::deque<KeyOpFieldsValuesTuple> &entries)
{
    int FdbAgingTime = DEFAULT_FDB_AGEOUT;
    if (entries.empty())
    {
        return;
    }

    for (auto entry: entries)
    {
        //vector<string> keys = tokenize(kfvKey(entry), config_db_key_delimiter);
        string op = kfvOp(entry);
        string key = kfvKey(entry);

        for (auto i : kfvFieldsValues(entry))
        {
            if (fvField(i) == "fdb_aging_time")
            {
                if (op == SET_COMMAND)
                {
                    FdbAgingTime = atoi(fvValue(i).c_str());
                    if(FdbAgingTime < 0)
                    {
                        SWSS_LOG_ERROR("Invalid fdb_aging_time %s", fvValue(i).c_str());
                        break;
                    }
                }
                else if (op == DEL_COMMAND)
                {
                    SWSS_LOG_DEBUG("operation:del");
                    FdbAgingTime = 600;
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
                    break;
                }
            }
        }
    }
    m_fdb_aging_time = FdbAgingTime;
    SWSS_LOG_NOTICE("Received fdb aging time %d", FdbAgingTime);

    //Update reference ageout
    updateReferenceAgeout();
    return;
}



