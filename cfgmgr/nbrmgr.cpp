#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <unistd.h>
#include <netlink/cache.h>

#include "logger.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "macaddress.h"
#include "nbrmgr.h"
#include "exec.h"
#include "shellcmd.h"
#include "subscriberstatetable.h"
#include <net/ethernet.h>
#include <netpacket/packet.h>

using namespace swss;

static bool send_message(struct nl_sock *sk, struct nl_msg *msg)
{
    bool rc = false;
    int err = 0;

    do
    {
        if (!sk)
        {
            SWSS_LOG_ERROR("Netlink socket null pointer");
            break;
        }

        if ((err = nl_send_auto(sk, msg)) < 0)
        {
            SWSS_LOG_ERROR("Netlink send message failed, error '%s'", nl_geterror(err));
            break;
        }

        rc = true;
    } while(0);

    nlmsg_free(msg);
    return rc;
}

NbrMgr::NbrMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME),
        m_stateVlanTable(stateDb, STATE_VLAN_TABLE_NAME),
        m_stateIntfTable(stateDb, STATE_INTERFACE_TABLE_NAME),
        m_stateNeighRestoreTable(stateDb, STATE_NEIGH_RESTORE_TABLE_NAME)
{
    int err = 0;

    m_nl_sock = nl_socket_alloc();
    if (!m_nl_sock)
    {
        SWSS_LOG_ERROR("Netlink socket alloc failed");
    }
    else if ((err = nl_connect(m_nl_sock, NETLINK_ROUTE)) < 0)
    {
        SWSS_LOG_ERROR("Netlink socket connect failed, error '%s'", nl_geterror(err));
    }

    auto consumerStateTable = new swss::ConsumerStateTable(appDb, APP_NEIGH_RESOLVE_TABLE_NAME,
                              TableConsumable::DEFAULT_POP_BATCH_SIZE, default_orch_pri);
    auto consumer = new Consumer(consumerStateTable, this, APP_NEIGH_RESOLVE_TABLE_NAME);
    Orch::addExecutor(consumer);
          
    string swtype;
    Table cfgDeviceMetaDataTable(cfgDb, CFG_DEVICE_METADATA_TABLE_NAME);
    if(cfgDeviceMetaDataTable.hget("localhost", "switch_type", swtype))
    {
        //If this is voq system, let the neighbor manager subscribe to state of SYSTEM_NEIGH
        //entries. This is used to program static neigh and static route in kernel for remote neighbors.
        if(swtype == "voq")
        {
            string tableName = STATE_SYSTEM_NEIGH_TABLE_NAME;
            Orch::addExecutor(new Consumer(new SubscriberStateTable(stateDb, tableName, TableConsumable::DEFAULT_POP_BATCH_SIZE, 0), this, tableName));
            m_cfgVoqInbandInterfaceTable = unique_ptr<Table>(new Table(cfgDb, CFG_VOQ_INBAND_INTERFACE_TABLE_NAME));
        }
    }

    neighRefreshInit();
}

bool NbrMgr::isIntfStateOk(const string &alias)
{
    vector<FieldValueTuple> temp;

    if (m_stateIntfTable.get(alias, temp))
    {
        SWSS_LOG_DEBUG("Intf %s is ready", alias.c_str());
        return true;
    }

    return false;
}

bool NbrMgr::isNeighRestoreDone()
{
    string value;

    m_stateNeighRestoreTable.hget("Flags", "restored", value);
    if (value == "true")
    {
        SWSS_LOG_INFO("Kernel neighbor table restore is done");
        return true;
    }
    return false;
}

bool NbrMgr::setNeighbor(const string& alias, const IpAddress& ip, const MacAddress& mac)
{
    SWSS_LOG_ENTER();

    struct nl_msg *msg = nlmsg_alloc();
    if (!msg)
    {
        SWSS_LOG_ERROR("Netlink message alloc failed for '%s'", ip.to_string().c_str());
        return false;
    }

    auto flags = (NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE);

    struct nlmsghdr *hdr = nlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, RTM_NEWNEIGH, 0, flags);
    if (!hdr)
    {
        SWSS_LOG_ERROR("Netlink message header alloc failed for '%s'", ip.to_string().c_str());
        nlmsg_free(msg);
        return false;
    }

    struct ndmsg *nd_msg = static_cast<struct ndmsg *>
                           (nlmsg_reserve(msg, sizeof(struct ndmsg), NLMSG_ALIGNTO));
    if (!nd_msg)
    {
        SWSS_LOG_ERROR("Netlink ndmsg reserve failed for '%s'", ip.to_string().c_str());
        nlmsg_free(msg);
        return false;
    }

    memset(nd_msg, 0, sizeof(struct ndmsg));

    nd_msg->ndm_ifindex = if_nametoindex(alias.c_str());

    auto addr_len = ip.isV4()? sizeof(struct in_addr) : sizeof(struct in6_addr);

    struct rtattr *rta = static_cast<struct rtattr *>
                         (nlmsg_reserve(msg, sizeof(struct rtattr) + addr_len, NLMSG_ALIGNTO));
    if (!rta)
    {
        SWSS_LOG_ERROR("Netlink rtattr (IP) failed for '%s'", ip.to_string().c_str());
        nlmsg_free(msg);
        return false;
    }

    rta->rta_type = NDA_DST;
    rta->rta_len = static_cast<short>(RTA_LENGTH(addr_len));

    nd_msg->ndm_type = RTN_UNICAST;
    auto ip_addr = ip.getIp();

    if (ip.isV4())
    {
        nd_msg->ndm_family = AF_INET;
        memcpy(RTA_DATA(rta), &ip_addr.ip_addr.ipv4_addr, addr_len);
    }
    else
    {
        nd_msg->ndm_family = AF_INET6;
        memcpy(RTA_DATA(rta), &ip_addr.ip_addr.ipv6_addr, addr_len);
    }

    if (!mac)
    {
        /*
         * If mac is not provided, expected to resolve the MAC
         */
        nd_msg->ndm_state = NUD_DELAY;
        nd_msg->ndm_flags = NTF_USE;

        SWSS_LOG_INFO("Resolve request for '%s'", ip.to_string().c_str());
    }
    else
    {
        SWSS_LOG_INFO("Set mac address '%s'", mac.to_string().c_str());

        nd_msg->ndm_state = NUD_PERMANENT;

        auto mac_len = ETHER_ADDR_LEN;
        auto mac_addr = mac.getMac();

        struct rtattr *rta = static_cast<struct rtattr *>
                             (nlmsg_reserve(msg, sizeof(struct rtattr) + mac_len, NLMSG_ALIGNTO));
        if (!rta)
        {
            SWSS_LOG_ERROR("Netlink rtattr (MAC) failed for '%s'", ip.to_string().c_str());
            nlmsg_free(msg);
            return false;
        }

        rta->rta_type = NDA_LLADDR;
        rta->rta_len = static_cast<short>(RTA_LENGTH(mac_len));
        memcpy(RTA_DATA(rta), mac_addr, mac_len);
    }

    return send_message(m_nl_sock, msg);
}

/**
 * Parse APPL_DB neighbors resolve table.
 *
 * @param [app_db_nbr_tbl_key], key from APPL_DB - APP_NEIGH_RESOLVE_TABLE_NAME
 * @param [delimiter], APPL_DB delimiter ":"
 *
 * @return the string vector which contain the VLAN alias and IP address
 */
vector<string> NbrMgr::parseAliasIp(const string &app_db_nbr_tbl_key, const char *delimiter)
{
    vector<string> ret;
    size_t found = app_db_nbr_tbl_key.find(delimiter);
    string alias = app_db_nbr_tbl_key.substr(0, found);
    string ip_address = app_db_nbr_tbl_key.substr(found + 1, app_db_nbr_tbl_key.size() - 1);

    ret.push_back(alias);
    ret.push_back(ip_address);

    return ret;
}

void NbrMgr::doResolveNeighTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple    t = it->second;
        if (kfvOp(t) == DEL_COMMAND)
        {
            SWSS_LOG_INFO("Received DEL operation for %s, skipping", kfvKey(t).c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        vector<string>            keys = parseAliasIp(kfvKey(t), consumer.getConsumerTable()->getTableNameSeparator().c_str());

        MacAddress                mac;
        IpAddress                 ip(keys[1]);
        string                    alias(keys[0]);

        if (!setNeighbor(alias, ip, mac))
        {
            SWSS_LOG_ERROR("Neigh entry resolve failed for '%s'", kfvKey(t).c_str());
        }
        it = consumer.m_toSync.erase(it);
    }
}

void NbrMgr::doSetNeighTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        vector<string> keys = tokenize(kfvKey(t), config_db_key_delimiter);
        const vector<FieldValueTuple>& data = kfvFieldsValues(t);

        string alias(keys[0]);
        IpAddress ip(keys[1]);
        string op = kfvOp(t);
        MacAddress mac;
        bool invalid_mac = false;

        for (auto idx : data)
        {
            const auto &field = fvField(idx);
            const auto &value = fvValue(idx);
            if (field == "neigh")
            {
                try
                {
                    mac = value;
                }
                catch (const std::invalid_argument& e)
                {
                    SWSS_LOG_ERROR("Invalid Mac addr '%s' for '%s'", value.c_str(), kfvKey(t).c_str());
                    invalid_mac = true;
                    break;
                }
            }
        }

        if (invalid_mac)
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            if (!isIntfStateOk(alias))
            {
                SWSS_LOG_DEBUG("Interface is not yet ready, skipping '%s'", kfvKey(t).c_str());
                it++;
                continue;
            }

            if (!setNeighbor(alias, ip, mac))
            {
                SWSS_LOG_ERROR("Neigh entry add failed for '%s'", kfvKey(t).c_str());
            }
            else
            {
                SWSS_LOG_NOTICE("Neigh entry added for '%s'", kfvKey(t).c_str());
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("Not yet implemented, key '%s'", kfvKey(t).c_str());
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation: '%s'", op.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

void NbrMgr::doTask(Consumer &consumer)
{
    string table_name = consumer.getTableName();

    if (table_name == CFG_NEIGH_TABLE_NAME)
    {
        doSetNeighTask(consumer);
    } else if (table_name == APP_NEIGH_RESOLVE_TABLE_NAME)
    {
        doResolveNeighTask(consumer);
    } else if(table_name == STATE_SYSTEM_NEIGH_TABLE_NAME)
    {
        doStateSystemNeighTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown REDIS table %s ", table_name.c_str());
    }
}

void NbrMgr::doStateSystemNeighTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    //Get the name of the device on which the neigh and route are
    //going to be programmed.
    string nbr_odev;
    string ibif_type;
    if(!getVoqInbandInterfaceName(nbr_odev, ibif_type))
    {
        //The inband interface is not available yet
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);

        size_t found = key.find_last_of(state_db_key_delimiter);
        if (found == string::npos)
        {
            SWSS_LOG_ERROR("Failed to parse key %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        IpAddress ip_address(key.substr(found+1));
        if (op == SET_COMMAND)
        {
            MacAddress mac_address;
            for (auto i = kfvFieldsValues(t).begin();
                 i  != kfvFieldsValues(t).end(); i++)
            {
                if (fvField(*i) == "neigh")
                    mac_address = MacAddress(fvValue(*i));
            }

            if (ibif_type == "port" && !isIntfOperUp(nbr_odev))
            {
                SWSS_LOG_DEBUG("Device %s is not oper up, skipping system neigh %s'", nbr_odev.c_str(), kfvKey(t).c_str());
                it++;
                continue;
            }

            if (!addKernelNeigh(nbr_odev, ip_address, mac_address))
            {
                SWSS_LOG_INFO("Neigh entry add on dev %s failed for '%s'", nbr_odev.c_str(), kfvKey(t).c_str());
                // Delete neigh to take care of deletion of exiting nbr for mac change. This makes sure that
                // re-try will be successful and route addtion (below) will be attempted and be successful
                delKernelNeigh(nbr_odev, ip_address);
                it++;
                continue;
            }
            else
            {
                SWSS_LOG_NOTICE("Neigh entry added on dev %s for '%s'", nbr_odev.c_str(), kfvKey(t).c_str());
            }

            if (!addKernelRoute(nbr_odev, ip_address))
            {
                SWSS_LOG_INFO("Route entry add on dev %s failed for '%s'", nbr_odev.c_str(), kfvKey(t).c_str());
                delKernelNeigh(nbr_odev, ip_address);
                // Delete route to take care of deletion of exiting route of nbr for mac change.
                delKernelRoute(ip_address);
                it++;
                continue;
            }
            else
            {
                SWSS_LOG_NOTICE("Route entry added on dev %s for '%s'", nbr_odev.c_str(), kfvKey(t).c_str());
            }
            SWSS_LOG_NOTICE("Added voq neighbor %s to kernel", kfvKey(t).c_str());
        }
        else if (op == DEL_COMMAND)
        {
            if (!delKernelRoute(ip_address))
            {
                SWSS_LOG_ERROR("Route entry on dev %s delete failed for '%s'", nbr_odev.c_str(), kfvKey(t).c_str());
            }
            else
            {
                SWSS_LOG_NOTICE("Route entry on dev %s deleted for '%s'", nbr_odev.c_str(), kfvKey(t).c_str());
            }

            if (!delKernelNeigh(nbr_odev, ip_address))
            {
                SWSS_LOG_ERROR("Neigh entry on dev %s delete failed for '%s'", nbr_odev.c_str(), kfvKey(t).c_str());
            }
            else
            {
                SWSS_LOG_NOTICE("Neigh entry on dev %s deleted for '%s'", nbr_odev.c_str(), kfvKey(t).c_str());
            }
            SWSS_LOG_DEBUG("Deleted voq neighbor %s from kernel", kfvKey(t).c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

bool NbrMgr::isIntfOperUp(const string &alias)
{
    string oper;

    if (m_statePortTable.hget(alias, "netdev_oper_status", oper))
    {
        if (oper == "up")
        {
            SWSS_LOG_DEBUG("NetDev %s is oper up", alias.c_str());
            return true;
        }
    }

    return false;
}

bool NbrMgr::getVoqInbandInterfaceName(string &ibif, string &type)
{
    vector<string> keys;
    m_cfgVoqInbandInterfaceTable->getKeys(keys);

    if (keys.empty())
    {
        SWSS_LOG_NOTICE("Voq Inband interface is not configured!");
        return false;
    }

    // key:"alias" = inband interface name

    vector<string> if_keys = tokenize(keys[0], config_db_key_delimiter);

    ibif = if_keys[0];

    // Get the type of the inband interface

    if (!m_cfgVoqInbandInterfaceTable->hget(ibif, "inband_type", type))
    {
        SWSS_LOG_ERROR("Getting Voq Inband interface type failed for %s", ibif.c_str());
        return false;
    }

    return true;
}

bool NbrMgr::addKernelRoute(string odev, IpAddress ip_addr)
{
    string cmd, res;

    SWSS_LOG_ENTER();

    string ip_str = ip_addr.to_string();

    if(ip_addr.isV4())
    {
        cmd = string("") + IP_CMD + " route add " + ip_str + "/32 dev " + odev;
        SWSS_LOG_NOTICE("IPv4 Route Add cmd: %s",cmd.c_str());
    }
    else
    {
        // In voq system, We need the static route to the remote neighbor and connected
        // route to have the same metric to enable BGP to choose paths from routes learned
        // via eBGP and iBGP over the internal inband port be part of same ecmp group.
        // For v4 both the metrics (connected and static) are default 0 so we do not need
        // to set the metric explicitly.
        cmd = string("") + IP_CMD + " -6 route add " + ip_str + "/128 dev " + odev + " metric 256";
        SWSS_LOG_NOTICE("IPv6 Route Add cmd: %s",cmd.c_str());
    }

    int32_t ret = swss::exec(cmd, res);

    if(ret)
    {
        /* This failure the caller expects is due to mac move */
        SWSS_LOG_INFO("Failed to add route for %s, error: %d", ip_str.c_str(), ret);
        return false;
    }

    SWSS_LOG_INFO("Added route for %s on device %s", ip_str.c_str(), odev.c_str());
    return true;
}

bool NbrMgr::delKernelRoute(IpAddress ip_addr)
{
    string cmd, res;

    SWSS_LOG_ENTER();

    string ip_str = ip_addr.to_string();

    if(ip_addr.isV4())
    {
        cmd = string("") + IP_CMD + " route del " + ip_str + "/32";
        SWSS_LOG_NOTICE("IPv4 Route Del cmd: %s",cmd.c_str());
    }
    else
    {
        cmd = string("") + IP_CMD + " -6 route del " + ip_str + "/128";
        SWSS_LOG_NOTICE("IPv6 Route Del cmd: %s",cmd.c_str());
    }

    int32_t ret = swss::exec(cmd, res);

    if(ret)
    {
        /* Just log error and return */
        SWSS_LOG_ERROR("Failed to delete route for %s, error: %d", ip_str.c_str(), ret);
        return false;
    }

    SWSS_LOG_INFO("Deleted route for %s", ip_str.c_str());
    return true;
}

bool NbrMgr::addKernelNeigh(string odev, IpAddress ip_addr, MacAddress mac_addr)
{
    SWSS_LOG_ENTER();

    string cmd, res;
    string ip_str = ip_addr.to_string();
    string mac_str = mac_addr.to_string();

    if(ip_addr.isV4())
    {
        cmd = string("") + IP_CMD + " neigh add " + ip_str + " lladdr " + mac_str + " dev " + odev;
        SWSS_LOG_NOTICE("IPv4 Nbr Add cmd: %s",cmd.c_str());
    }
    else
    {
        cmd = string("") + IP_CMD + " -6 neigh add " + ip_str + " lladdr " + mac_str + " dev " + odev;
        SWSS_LOG_NOTICE("IPv6 Nbr Add cmd: %s",cmd.c_str());
    }

    int32_t ret = swss::exec(cmd, res);

    if(ret)
    {
        /* This failure the caller expects is due to mac move */
        SWSS_LOG_INFO("Failed to add Nbr for %s, error: %d", ip_str.c_str(), ret);
        return false;
    }

    SWSS_LOG_INFO("Added Nbr for %s on interface %s", ip_str.c_str(), odev.c_str());
    return true;
}

bool NbrMgr::delKernelNeigh(string odev, IpAddress ip_addr)
{
    string cmd, res;

    SWSS_LOG_ENTER();

    string ip_str = ip_addr.to_string();

    if(ip_addr.isV4())
    {
        cmd = string("") + IP_CMD + " neigh del " + ip_str + " dev " + odev;
        SWSS_LOG_NOTICE("IPv4 Nbr Del cmd: %s",cmd.c_str());
    }
    else
    {
        cmd = string("") + IP_CMD + " -6 neigh del " + ip_str + " dev " + odev;
        SWSS_LOG_NOTICE("IPv6 Nbr Del cmd: %s",cmd.c_str());
    }

    int32_t ret = swss::exec(cmd, res);

    if(ret)
    {
        /* Just log error and return */
        SWSS_LOG_ERROR("Failed to delete Nbr for %s, error: %d", ip_str.c_str(), ret);
        return false;
    }

    SWSS_LOG_INFO("Deleted Nbr for %s on interface %s", ip_str.c_str(), odev.c_str());
    return true;
}

unsigned long long NbrMgr::get_currtime_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    {
        return ((((unsigned long long)ts.tv_sec) * 1000) + (((unsigned long long)ts.tv_nsec)/1000000));
    }
    else
    {
        SWSS_LOG_NOTICE("NbrMgrCache: GetTime Failed");
    }

    return 0;
}


bool NbrMgr::is_time_elapsed_ms(unsigned long long time_to_chk, unsigned long long elaspe_time)
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

// Send Arp Refresh / Neigh Solicitation message periodically
void NbrMgr::refreshTimer()
{
    int num_refresh = 0;
    static int timer = 0;

    timer++;

    if(!(timer % NEIGH_REFRESH_INTERVAL))
    {
        auto it = m_neighCache.begin();
        while (it != m_neighCache.end())
        {
            if (!(it->second.state & NUD_IN_TIMER))
            {
                it++;
                continue;
            }
            if(is_time_elapsed_ms(it->second.start_time, it->second.refresh_timeout))
            {
                sendRefresh(it->first.ip_address , it->first.alias, it->second.mac_address);

                it->second.start_time        = get_currtime_ms();
                it->second.refresh_timeout  = get_refresh_timeout(it->first.ip_address.isV4());
                it->second.num_refresh_sent++;
                num_refresh++;

                if(!(num_refresh % NEIGH_REFRESH_TX_THRESHOLD))
                {
                    SWSS_LOG_NOTICE("NbrRefresh: Neighbor Refresh Timeout - Exceeded Threshold, total sent so far - %d", num_refresh);
                }
            }
            it++;
        }
        timer = 0;
        SWSS_LOG_INFO("NbrRefresh: Neighbor Refresh Timeout - End, total sent - %d", num_refresh);
    }

    return;
}

bool NbrMgr::neighRefreshInit()
{
    //Socket to send ARP request packet
    m_sock_fd[0] = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if(m_sock_fd[0] < 0)
    {
        SWSS_LOG_ERROR("NbrRefresh: Arp Refresh socket create failed %d, err(%d):%s ",m_sock_fd[0], errno, strerror(errno));
        return false;
    }

    SWSS_LOG_NOTICE("NbrRefresh: Arp Refresh socket created sock_fd - %d ",m_sock_fd[0]);

    /*TODO: Enable after merging neigh ageout config
    m_ipv4_arp_timeout = DEFAULT_ARP_AGEOUT;
    m_ipv6_nd_timeout  = DEFAULT_NS_AGEOUT;

    updateReferenceAgeout();
    */

    return true;
}

int NbrMgr::buildArpPkt(unsigned char* pkt, IpAddress tgt_ip, MacAddress tgt_mac, IpAddress src_ip, MacAddress src_mac)
{
    struct ethhdr eth_hdr;
    struct arphdr arp_hdr;
    uint32_t ipaddr = 0;
    uint8_t *pkt_buff = pkt;

    memset(&eth_hdr, 0x0, sizeof(eth_hdr));
    memcpy(&eth_hdr.src, src_mac.getMac(), 6);
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

bool NbrMgr::sendRefresh(IpAddress ip, string ifname, MacAddress dstMac)
{
    MacAddress src_mac;
    IpAddress src_ip;
    uint8_t pkt[256];
    int pkt_len = 0;
    long int ret = 0;
    uint32_t ifidx = 0;

    if(ip.getIp().family != AF_INET)
    {
        return true;
    }

    src_mac = getIntfsMac(ifname);

    ifidx = if_nametoindex(ifname.c_str());
    if(!ifidx)
    {
        SWSS_LOG_ERROR("NbrRefresh: IFname to ifidx failed %s", ifname.c_str());
        return false;
    }

    src_ip = getIntfIpAddress(ifname, ip);
    if(src_ip == IpAddress("0.0.0.0"))
    {
        SWSS_LOG_ERROR("NbrRefresh: Src IP Not Found: Ifindex - %s (%d) Src-mac - %s, Tgt-ip - %s, Tgt-mac - %s ",
                ifname.c_str(),ifidx, src_mac.to_string().c_str(), ip.to_string().c_str(), dstMac.to_string().c_str());

        return false;
    }

    //TODO: Change Notice to Info
    SWSS_LOG_NOTICE("NbrRefresh: Ifindex - %s (%d) Src-ip - %s, Src-mac - %s, Tgt-ip - %s, Tgt-mac - %s ",
    ifname.c_str(),ifidx, src_ip.to_string().c_str(), src_mac.to_string().c_str(), ip.to_string().c_str(), dstMac.to_string().c_str());

    if(ip.getIp().family == AF_INET)
    {
        struct sockaddr_ll dst;

        //build the ARP packet
        pkt_len =  buildArpPkt(pkt, ip, dstMac, src_ip, src_mac);

        memset (&dst, 0x0, sizeof(dst));
        dst.sll_family      = AF_PACKET;
        dst.sll_ifindex     = ifidx;
        dst.sll_protocol    = htons(ETH_P_ALL);
        dst.sll_halen       = ETH_ALEN;
        dstMac.getMac(dst.sll_addr);

        ret = sendto(m_sock_fd[0], pkt, pkt_len, 0, (struct sockaddr *)&dst, sizeof(dst));
        if(ret < 0)
            SWSS_LOG_ERROR("NbrRefresh: Arp Send failed for IP %s intf %s, err(%d): %s", ip.to_string().c_str(), ifname.c_str(), errno, strerror(errno));
        else
            SWSS_LOG_DEBUG("NbrRefresh: Arp Sent for IP %s intf %s, ret %ld ", ip.to_string().c_str(), ifname.c_str(), ret);
    }

    return true;
}

MacAddress NbrMgr::getIntfsMac(const string& ifname)
{
    MacAddress intfMac = getSystemMac();

    std::string intfName = ifname.c_str();
    std::string res;
    string file = "/sys/class/net/"+intfName+"/address";
    bool exists = false;

    res = readLineFromFile(file, &exists);
    if (!exists)
    {
        SWSS_LOG_ERROR("NbrRefresh: intf %s doesnt exist", ifname.c_str());
    }
    else
    {
        if(!res.empty())
        {
            intfMac = MacAddress(res);
        }
        else
        {
            SWSS_LOG_INFO("NbrRefresh: intf %s doesnt have valid MAC", ifname.c_str());
        }
    }
    return intfMac;
}

MacAddress NbrMgr::getSystemMac(void)
{
    return m_system_mac;
}

string NbrMgr::readLineFromFile(const string file, bool *exists)
{
    ifstream fp(file);
    if (!fp.is_open())
    {
        SWSS_LOG_ERROR("NbrRefresh: File %s is not readable", file.c_str());
        *exists = false;
        return "";
    }
    string line="";
    getline(fp, line);
    if (line.empty())
    {
        SWSS_LOG_NOTICE("NbrRefresh: File %s is empty", file.c_str());
    }
    *exists = true;
    fp.close();

    return string(line);
}


unsigned long long NbrMgr::get_refresh_timeout(bool isV4)
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

IpAddress NbrMgr::getIntfIpAddress(string ifname, IpAddress dst_ip)
{
    //TODO: function defined in Interface Cache PR
    return IpAddress("0.0.0.0");
}
