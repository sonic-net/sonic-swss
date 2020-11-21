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

    auto subscriberStateTable = new swss::SubscriberStateTable(appDb, APP_NEIGH_RESOLVE_TABLE_NAME, TableConsumable::DEFAULT_POP_BATCH_SIZE, default_orch_pri);
    auto subscriberStateConsumer = new Consumer(subscriberStateTable, this, APP_NEIGH_RESOLVE_TABLE_NAME);
    Orch::addExecutor(subscriberStateConsumer);
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


bool NbrMgr::addIp(struct nl_msg* msg_p, const IpAddress& ip)
{
    auto ip_addr = ip.getIp();
    auto addr_len = ip.isV4()? sizeof(struct in_addr) : sizeof(struct in6_addr);

    struct rtattr *rta_p = static_cast<struct rtattr *>
                           (nlmsg_reserve(msg_p, sizeof(struct rtattr) + addr_len, NLMSG_ALIGNTO));
    if (!rta_p)
    {
        SWSS_LOG_ERROR("Netlink rtattr (IP) failed for '%s'", ip.to_string().c_str());
        return false;
    }

    rta_p->rta_type = NDA_DST;
    rta_p->rta_len = static_cast<short>(RTA_LENGTH(addr_len));
    ip.isV4() ? memcpy(RTA_DATA(rta_p), &ip_addr.ip_addr.ipv4_addr, addr_len) :
                memcpy(RTA_DATA(rta_p), &ip_addr.ip_addr.ipv6_addr, addr_len);

    return true;
}

bool NbrMgr::addMac(struct nl_msg* msg_p, const MacAddress& mac)
{
    auto mac_len = ETHER_ADDR_LEN;
    auto mac_addr = mac.getMac();

    struct rtattr *rta_p = static_cast<struct rtattr *>
                         (nlmsg_reserve(msg_p, sizeof(struct rtattr) + mac_len, NLMSG_ALIGNTO));
    if (!rta_p)
    {
        SWSS_LOG_ERROR("Netlink rtattr (MAC) failed for '%s'", mac.to_string().c_str());
        return false;
    }

    rta_p->rta_type = NDA_LLADDR;
    rta_p->rta_len = static_cast<short>(RTA_LENGTH(mac_len));
    memcpy(RTA_DATA(rta_p), mac_addr, mac_len);

    return true;
}

bool NbrMgr::deleteNeighbor(const string& alias, const IpAddress& ip, const MacAddress& mac)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("Remove ARP entry '%s:%s -> %s'",
                   alias.c_str(), ip.to_string().c_str(), mac.to_string().c_str());

    struct nl_msg *msg_p = nlmsg_alloc();
    if (!msg_p)
    {
        SWSS_LOG_ERROR("Netlink message alloc failed for '%s'", ip.to_string().c_str());
        return false;
    }

    auto flags = (NLM_F_REQUEST | NLM_F_ACK);
    struct nlmsghdr *hdr_p = nlmsg_put(msg_p, NL_AUTO_PORT, NL_AUTO_SEQ, RTM_DELNEIGH, 0, flags);
    if (!hdr_p)
    {
        SWSS_LOG_ERROR("Netlink message header alloc failed for '%s'", ip.to_string().c_str());
        nlmsg_free(msg_p);
        return false;
    }

    // Prepare ndmsg to remove an entry from neighbor table
    struct ndmsg *nd_msg_p = static_cast<struct ndmsg *>
                           (nlmsg_reserve(msg_p, sizeof(struct ndmsg), NLMSG_ALIGNTO));
    if (!nd_msg_p)
    {
        SWSS_LOG_ERROR("Netlink ndmsg reserve failed for '%s'", ip.to_string().c_str());
        nlmsg_free(msg_p);
        return false;
    }
    memset(nd_msg_p, 0, sizeof(struct ndmsg));

    // Fill in the interface
    nd_msg_p->ndm_ifindex = if_nametoindex(alias.c_str());

    // Fill in the IPv4/IPv6 address
    if (!addIp(msg_p, ip))
    {
        nlmsg_free(msg_p);
        return false;
    }
    nd_msg_p->ndm_type = RTN_UNICAST;
    nd_msg_p->ndm_family = ip.isV4() ? AF_INET : AF_INET6;

    // Fill in the MAC address
    if (!addMac(msg_p, mac))
    {
        nlmsg_free(msg_p);
        return false;
    }

    // Send ndmsg
    return send_message(m_nl_sock, msg_p);
}

bool NbrMgr::setNeighbor(const string& alias, const IpAddress& ip, const MacAddress& mac)
{
    SWSS_LOG_ENTER();

    struct nl_msg *msg_p = nlmsg_alloc();
    if (!msg_p)
    {
        SWSS_LOG_ERROR("Netlink message alloc failed for '%s'", ip.to_string().c_str());
        return false;
    }

    auto flags = (NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE);

    struct nlmsghdr *hdr_p = nlmsg_put(msg_p, NL_AUTO_PORT, NL_AUTO_SEQ, RTM_NEWNEIGH, 0, flags);
    if (!hdr_p)
    {
        SWSS_LOG_ERROR("Netlink message header alloc failed for '%s'", ip.to_string().c_str());
        nlmsg_free(msg_p);
        return false;
    }

    // Prepare ndmsg to add an entry to neighbor table
    struct ndmsg *nd_msg_p = static_cast<struct ndmsg *>
                           (nlmsg_reserve(msg_p, sizeof(struct ndmsg), NLMSG_ALIGNTO));
    if (!nd_msg_p)
    {
        SWSS_LOG_ERROR("Netlink ndmsg reserve failed for '%s'", ip.to_string().c_str());
        nlmsg_free(msg_p);
        return false;
    }
    memset(nd_msg_p, 0, sizeof(struct ndmsg));

    // Populate interface
    nd_msg_p->ndm_ifindex = if_nametoindex(alias.c_str());

    // Populate IP
    if (!addIp(msg_p, ip))
    {
        nlmsg_free(msg_p);
        return false;
    }
    nd_msg_p->ndm_type = RTN_UNICAST;
    nd_msg_p->ndm_family = ip.isV4() ? AF_INET : AF_INET6;

    // Populate MAC is provided
    if (!mac)
    {
        /*
         * If mac is not provided, expected to resolve the MAC
         */
        nd_msg_p->ndm_state = NUD_DELAY;
        nd_msg_p->ndm_flags = NTF_USE;

        SWSS_LOG_INFO("Resolve request for '%s'", ip.to_string().c_str());
    }
    else
    {
        SWSS_LOG_INFO("Set mac address '%s'", mac.to_string().c_str());

        if (!addMac(msg_p, mac))
        {
            nlmsg_free(msg_p);
            return false;
        }

        nd_msg_p->ndm_state = NUD_PERMANENT;
    }

    // Send ndmsg
    return send_message(m_nl_sock, msg_p);
}

void NbrMgr::doDeleteNeighTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        vector<string> keys = tokenize(kfvKey(t),delimiter);
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
            if (field == "mac")
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

        if (!deleteNeighbor(alias, ip, mac))
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
    }

    if (table_name == APP_NEIGH_RESOLVE_TABLE_NAME)
    {
        doDeleteNeighTask(consumer);
    }

    SWSS_LOG_ERROR("Unknown REDIS table %s ", table_name.c_str());
}
