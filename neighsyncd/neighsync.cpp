#include <string>
#include <netinet/in.h>
#include <netlink/route/link.h>
#include <netlink/route/neighbour.h>
#include <unistd.h>

#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "ipaddress.h"
#include "netmsg.h"
#include "linkcache.h"
#include "macaddress.h"

#include "neighsync.h"
#include "warm_restart.h"
#include <algorithm>
#include <linux/neighbour.h>

using namespace std;
using namespace swss;

#define VRF_PREFIX              "Vrf"
#define TENMS                   10000

NeighSync::NeighSync(RedisPipeline *pipelineAppDB, DBConnector *stateDb, DBConnector *cfgDb, DBConnector *appDb) :
    m_neighTable(pipelineAppDB, APP_NEIGH_TABLE_NAME),
    m_routeTable(pipelineAppDB, APP_ROUTE_TABLE_NAME, false),
    m_routeCheckTable(appDb, APP_ROUTE_TABLE_NAME),
    m_stateNeighRestoreTable(stateDb, STATE_NEIGH_RESTORE_TABLE_NAME),
    m_cfgInterfaceTable(cfgDb, CFG_INTF_TABLE_NAME),
    m_cfgLagInterfaceTable(cfgDb, CFG_LAG_INTF_TABLE_NAME),
    m_cfgVlanInterfaceTable(cfgDb, CFG_VLAN_INTF_TABLE_NAME),
    m_cfgPeerSwitchTable(cfgDb, CFG_PEER_SWITCH_TABLE_NAME),
    m_nl_sock(NULL), m_link_cache(NULL)
{
    m_AppRestartAssist = new AppRestartAssist(pipelineAppDB, "neighsyncd", "swss", DEFAULT_NEIGHSYNC_WARMSTART_TIMER);
    if (m_AppRestartAssist)
    {
        m_AppRestartAssist->registerAppTable(APP_NEIGH_TABLE_NAME, &m_neighTable);
    }
    m_nl_sock = nl_socket_alloc();
    nl_connect(m_nl_sock, NETLINK_ROUTE);
    rtnl_link_alloc_cache(m_nl_sock, AF_UNSPEC, &m_link_cache);
}

NeighSync::~NeighSync()
{
    if (m_AppRestartAssist)
    {
        delete m_AppRestartAssist;
    }
}

/*
 * Get interface/VRF name based on interface/VRF index
 * @arg if_index          Interface/VRF index
 * @arg if_name           String to store interface name
 * @arg name_len          Length of destination string, including terminating zero byte
 *
 * Return true if we successfully gets the interface/VRF name.
 */
bool NeighSync::getIfName(int if_index, char *if_name, size_t name_len)
{
    if (!if_name || name_len == 0)
    {
        return false;
    }

    memset(if_name, 0, name_len);

    /* Cannot get interface name. Possibly the interface gets re-created. */
    if (!rtnl_link_i2name(m_link_cache, if_index, if_name, name_len))
    {
        /* Trying to refill cache */
        nl_cache_refill(m_nl_sock, m_link_cache);
        if (!rtnl_link_i2name(m_link_cache, if_index, if_name, name_len))
        {
            return false;
        }
    }

    return true;
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
    string intfName;
    std::vector<std::string> peerSwitchKeys;
    m_cfgPeerSwitchTable.getKeys(peerSwitchKeys);
    bool is_dualtor = peerSwitchKeys.size() > 0;

    if ((nlmsg_type != RTM_NEWNEIGH) && (nlmsg_type != RTM_GETNEIGH) &&
        (nlmsg_type != RTM_DELNEIGH))
        return;

    if (rtnl_neigh_get_family(neigh) == AF_INET)
        family = IPV4_NAME;
    else if (rtnl_neigh_get_family(neigh) == AF_INET6)
        family = IPV6_NAME;
    else
        return;

    key+= LinkCache::getInstance().ifindexToName(rtnl_neigh_get_ifindex(neigh));
    intfName = key;
    key+= ":";

    /* Get the vrf name */
    int ifindex = rtnl_neigh_get_ifindex(neigh);
    char master_name[IFNAMSIZ] = {0};
    if (ifindex > 0)
    {
        struct rtnl_link *link = rtnl_link_get(m_link_cache, ifindex);
        if (!link)
        {
            /* Trying to refill cache */
            nl_cache_refill(m_nl_sock, m_link_cache);
            link = rtnl_link_get(m_link_cache, ifindex);
        }

        if (link)
        {
            int master_index = rtnl_link_get_master(link);
            if (master_index)
            {
                /* Get the name of the master device */
                getIfName(master_index, master_name, IFNAMSIZ);
            }
			rtnl_link_put(link);
        }
    }

    nl_addr2str(rtnl_neigh_get_dst(neigh), ipStr, MAX_ADDR_SIZE);

    /* Ignore IPv4 link-local addresses as neighbors if subtype is dualtor */
    IpAddress ipAddress(ipStr);
    if (family == IPV4_NAME && ipAddress.getAddrScope() == IpAddress::AddrScope::LINK_SCOPE && is_dualtor)
    {
        SWSS_LOG_INFO("Link Local address received on dualtor, ignoring for %s", ipStr);
        return;
    }

    /* Ignore IPv6 link-local addresses as neighbors, if ipv6 link local mode is disabled */
    if (family == IPV6_NAME && IN6_IS_ADDR_LINKLOCAL(nl_addr_get_binary_addr(rtnl_neigh_get_dst(neigh))))
    {
        if ((isLinkLocalEnabled(intfName) == false) && (nlmsg_type != RTM_DELNEIGH))
        {
            SWSS_LOG_INFO("LinkLocal address received, ignoring for %s", ipStr);
            return;
        }
    }
    /* Ignore IPv6 multicast link-local addresses as neighbors */
    if (family == IPV6_NAME && IN6_IS_ADDR_MC_LINKLOCAL(nl_addr_get_binary_addr(rtnl_neigh_get_dst(neigh))))
    {
        SWSS_LOG_INFO("Multicast LinkLocal address received, ignoring for %s", ipStr);
        return;
    }
    key+= ipStr;

    int state = rtnl_neigh_get_state(neigh);
    /* Ignore probe msg */
    if ((nlmsg_type == RTM_NEWNEIGH) && (state == NUD_PROBE))
    {
        return;
    }

    SWSS_LOG_INFO("Get neighbor msg %s, state %d, type %d", ipStr, state, nlmsg_type);
    if (state == NUD_NOARP)
    {
        /* For externally learned neighbors, e.g. VXLAN EVPN, we want to keep
         * these neighbors. */
        if (!(rtnl_neigh_get_flags(neigh) & NTF_EXT_LEARNED))
        {
            SWSS_LOG_INFO("NOARP address received, ignoring for %s", ipStr);
            return;
        }
    }

    bool delete_key = false;
    bool use_zero_mac = false;
    if (is_dualtor && (state == NUD_INCOMPLETE || state == NUD_FAILED))
    {
        SWSS_LOG_INFO("Unable to resolve %s, setting zero MAC", key.c_str());
        use_zero_mac = true;

        // Unresolved neighbor deletion on dual ToR devices must be handled
        // separately, otherwise delete_key is never set to true
        // and neighorch is never able to remove the neighbor
        if (nlmsg_type == RTM_DELNEIGH)
        {
            delete_key = true;
        }
    }
    else if ((nlmsg_type == RTM_DELNEIGH) ||
             (state == NUD_INCOMPLETE) || (state == NUD_FAILED) || (state == NUD_NOARP))
    {
        /* NUD_NOARP means this neighbor is moved to remote, we should delete the local neighbor */
        delete_key = true;
    }

    if (use_zero_mac)
    {
        std::string zero_mac = "00:00:00:00:00:00";
        strncpy(macStr, zero_mac.c_str(), zero_mac.length());
    }
    else
    {
        nl_addr2str(rtnl_neigh_get_lladdr(neigh), macStr, MAX_ADDR_SIZE);
    }

    if (!delete_key && !strncmp(macStr, "none", MAX_ADDR_SIZE))
    {
        SWSS_LOG_NOTICE("Mac address is 'none' for ADD op, ignoring for %s", ipStr);
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
            return;
        }

        string hostRoute;
        /* Always try to del the host route before add neighbor */
        if (string(master_name).compare(0, 3, VRF_PREFIX) == 0)
        {
            hostRoute += master_name;
            hostRoute += ":";
            hostRoute += ipStr;

            SWSS_LOG_INFO("Remove host route before adding neighbor %s", hostRoute.c_str());
            m_routeTable.del(hostRoute);
            vector<FieldValueTuple> values;
            while (m_routeCheckTable.get(hostRoute, values))
            {
                SWSS_LOG_DEBUG("Host route still exists: %s", hostRoute.c_str());
                m_routeTable.del(hostRoute);
				usleep(TENMS);
            }
        }

        m_neighTable.set(key, fvVector);
    }
}

/* To check the ipv6 link local is enabled on a given port */
bool NeighSync::isLinkLocalEnabled(const string &port)
{
    vector<FieldValueTuple> values;

    if (!port.compare(0, strlen("Vlan"), "Vlan"))
    {
        if (!m_cfgVlanInterfaceTable.get(port, values))
        {
            SWSS_LOG_INFO("IPv6 Link local is not enabled on %s", port.c_str());
            return false;
        }
    }
    else if (!port.compare(0, strlen("PortChannel"), "PortChannel"))
    {
        if (!m_cfgLagInterfaceTable.get(port, values))
        {
            SWSS_LOG_INFO("IPv6 Link local is not enabled on %s", port.c_str());
            return false;
        }
    }
    else if (!port.compare(0, strlen("Ethernet"), "Ethernet"))
    {
        if (!m_cfgInterfaceTable.get(port, values))
        {
            SWSS_LOG_INFO("IPv6 Link local is not enabled on %s", port.c_str());
            return false;
        }
    }
    else
    {
        SWSS_LOG_INFO("IPv6 Link local is not supported for %s ", port.c_str());
        return false;
    }

    auto it = std::find_if(values.begin(), values.end(), [](const FieldValueTuple& t){ return t.first == "ipv6_use_link_local_only";});
    if (it != values.end())
    {
        if (it->second == "enable")
        {
            SWSS_LOG_INFO("IPv6 Link local is enabled on %s", port.c_str());
            return true;
        }
    }

    SWSS_LOG_INFO("IPv6 Link local is not enabled on %s", port.c_str());
    return false;
}
