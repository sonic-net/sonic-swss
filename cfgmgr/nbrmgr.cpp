#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <unistd.h>

#include "logger.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "macaddress.h"
#include "nbrmgr.h"
#include "exec.h"
#include "shellcmd.h"

using namespace swss;

const int IP4_ADDR_LEN  = 4;
const int IP6_ADDR_LEN  = 16;
const int NL_MSG_BUF_SZ = 512;

#define VLAN_PREFIX "Vlan"
#define LAG_PREFIX  "PortChannel"

NbrMgr::NbrMgr(DBConnector *cfgDb, DBConnector *appDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME),
        m_stateVlanTable(stateDb, STATE_VLAN_TABLE_NAME),
        m_stateIntfTable(stateDb, STATE_INTERFACE_TABLE_NAME)
{

}

bool NbrMgr::isIntfStateOk(const string &alias)
{
    vector<FieldValueTuple> temp;

    if (!alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX))
    {
        if (m_stateVlanTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Vlan %s is ready", alias.c_str());
            return true;
        }
    }
    else if (!alias.compare(0, strlen(LAG_PREFIX), LAG_PREFIX))
    {
        if (m_stateLagTable.get(alias, temp))
        {
            SWSS_LOG_DEBUG("Lag %s is ready", alias.c_str());
            return true;
        }
    }
    else if (m_statePortTable.get(alias, temp))
    {
        SWSS_LOG_DEBUG("Port %s is ready", alias.c_str());
        return true;
    }

    return false;
}

bool NbrMgr::sendMsg(struct nlmsghdr *msg)
{
    SWSS_LOG_ENTER();

    int sk = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (sk < 0)
    {
        SWSS_LOG_ERROR("Socket create error");
        return false;
    }

    struct sockaddr_nl nl_addr ;
    memset(&nl_addr, 0, sizeof(nl_addr));

    nl_addr.nl_family = AF_NETLINK;

    struct iovec iov[1] =
    {
        {
          .iov_base = msg,
          .iov_len = msg->nlmsg_len
        }
    };

    struct msghdr msg_hdr =
    {
        .msg_name = &nl_addr,
        .msg_namelen = sizeof(nl_addr),
        .msg_iov = iov,
        .msg_iovlen = 1,
        .msg_control = NULL,
        .msg_controllen = 0,
        .msg_flags =0
    };

    if (sendmsg(sk, &msg_hdr, 0) != (msg->nlmsg_len))
    {
        SWSS_LOG_ERROR("Error sending message to kernel");
        close(sk);
        return false;
    }

    close(sk);

    SWSS_LOG_INFO("Neighbor add msg sent to kernel");
    return true;
}

bool NbrMgr::setNeighbor(const string& alias, IpAddress& ip, MacAddress& mac)
{
    SWSS_LOG_ENTER();

    char m_buf[NL_MSG_BUF_SZ];
    memset(m_buf, 0, sizeof(struct nlmsghdr));

    auto alloc_fn = [&] (struct nlmsghdr *msg, uint32_t len) {
        auto *tail = (void *) (((char*)msg) + NLMSG_ALIGN(msg->nlmsg_len));;
        msg->nlmsg_len = NLMSG_ALIGN(msg->nlmsg_len) + RTA_ALIGN(len);
        return tail;
    };

    struct nlmsghdr *nl_msg = (struct nlmsghdr *) alloc_fn((struct nlmsghdr *)m_buf, sizeof(struct nlmsghdr));
    struct ndmsg *nd_msg = (struct ndmsg *) alloc_fn(nl_msg, sizeof(struct ndmsg));

    memset(nd_msg, 0, sizeof(struct ndmsg));

    nd_msg->ndm_ifindex = if_nametoindex(alias.c_str());

    int addr_len = ip.isV4()?IP4_ADDR_LEN : IP6_ADDR_LEN;

    struct rtattr *rta = (struct rtattr *) alloc_fn(nl_msg, uint32_t(RTA_LENGTH(addr_len)));

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
        struct rtattr *rta = (struct rtattr *) alloc_fn(nl_msg, uint32_t(RTA_LENGTH(mac_len)));
        rta->rta_type = NDA_LLADDR;
        rta->rta_len = static_cast<short>(RTA_LENGTH(mac_len));
        memcpy(RTA_DATA(rta), mac_addr, mac_len);
    }

    nl_msg->nlmsg_flags = (NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE);
    nl_msg->nlmsg_type = RTM_NEWNEIGH;

    return sendMsg(nl_msg);
}

void NbrMgr::doTask(Consumer &consumer)
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

        for (auto idx : data)
        {
            const auto &field = fvField(idx);
            const auto &value = fvValue(idx);
            if (field == "neigh")
            {
                mac = value;
            }
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
