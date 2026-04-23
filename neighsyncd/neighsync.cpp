#include <string>
#include <netinet/in.h>
#include <netlink/route/link.h>
#include <netlink/route/neighbour.h>

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

#define VLAN_SUB_INTERFACE_SEPARATOR   "."
#define RESERVED_IPV4_LL    "169.254.0.1"

NeighSync::NeighSync(RedisPipeline *pipelineAppDB, DBConnector *stateDb, DBConnector *cfgDb) :
    m_neighTable(pipelineAppDB, APP_NEIGH_TABLE_NAME),
    m_stateNeighRestoreTable(stateDb, STATE_NEIGH_RESTORE_TABLE_NAME),
    m_cfgInterfaceTable(cfgDb, CFG_INTF_TABLE_NAME),
    m_cfgLagInterfaceTable(cfgDb, CFG_LAG_INTF_TABLE_NAME),
    m_cfgVlanInterfaceTable(cfgDb, CFG_VLAN_INTF_TABLE_NAME),
    m_cfgPeerSwitchTable(cfgDb, CFG_PEER_SWITCH_TABLE_NAME)
{
    m_AppRestartAssist = new AppRestartAssist(pipelineAppDB, "neighsyncd", "swss", DEFAULT_NEIGHSYNC_WARMSTART_TIMER);
    if (m_AppRestartAssist)
    {
        m_AppRestartAssist->registerAppTable(APP_NEIGH_TABLE_NAME, &m_neighTable);
    }
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

Table *NeighSync::getInterfaceTable(const std::string &intfName)
{
    if (intfName.find(FRONT_PANEL_PORT_PREFIX) != string::npos)
    {
        if (intfName.find(VLAN_SUB_INTERFACE_SEPARATOR) != string::npos)
            return &m_cfgSubInterfaceTable;
        return &m_cfgInterfaceTable;
    }
    else if (intfName.find(PORTCHANNEL_PREFIX) != string::npos)
    {
        if (intfName.find(VLAN_SUB_INTERFACE_SEPARATOR) != string::npos)
            return &m_cfgSubInterfaceTable;
        return &m_cfgLagInterfaceTable;
    }
    else if (intfName.find(VLAN_PREFIX) != string::npos)
    {
        return &m_cfgVlanInterfaceTable;
    }
    else if (intfName.find(VLAN_SUB_INTERFACE_SEPARATOR) != string::npos)
    {
        if ((intfName.find(FRONT_PANEL_PORT_PREFIX) != string::npos) || (intfName.find(PORTCHANNEL_PREFIX) != string::npos))
            return &m_cfgSubInterfaceTable;
    }
    return nullptr;
}

bool NeighSync::isRouterInterface(const std::string &intfName)
{
    vector<FieldValueTuple> values;
    Table *intfTable_p = nullptr;
    intfTable_p = getInterfaceTable(intfName);
    if ((intfTable_p != nullptr) && intfTable_p->get(intfName, values))
    {
        return true;
    }
    return false;
}

void NeighSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    char ipStr[MAX_ADDR_SIZE + 1] = {0};
    char macStr[MAX_ADDR_SIZE + 1] = {0};
    struct rtnl_neigh *neigh = (struct rtnl_neigh *)obj;
    struct nl_addr *lladdr = NULL;
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
             (state == NUD_INCOMPLETE) || (state == NUD_FAILED))
    {
	    delete_key = true;
    }

    /* Ignore the following neighbor entries
     * - learned on non-router interface
     * - learned on SAG disabled interface
     * For reserved IPv4 link-local address (169.254.0.1) used as next hop for BGP unnumber,
     * this entry shouldn't be ignored even if it satisfy the above condition
     * */
    //if (!delete_key && !isRouterInterface(intfName) && !isSagEnabled(intfName) && strcmp(ipStr, RESERVED_IPV4_LL)) ///FIXME, isSagEnabled
    if (!delete_key && !isRouterInterface(intfName) && strcmp(ipStr, RESERVED_IPV4_LL))
    {
        SWSS_LOG_INFO("Ignore neighbor %s on non-router-interface %s", ipStr, intfName.c_str());
        return;
    }

    if (use_zero_mac)
    {
        std::string zero_mac = "00:00:00:00:00:00";
        strncpy(macStr, zero_mac.c_str(), zero_mac.length());
    }
    else
    {
        /* Get the link-layer address */
        lladdr = rtnl_neigh_get_lladdr(neigh);

        /* Check if the link-layer address is NULL */
        if (lladdr != NULL)
        {
            nl_addr2str(lladdr, macStr, MAX_ADDR_SIZE);
        }
    }

    if (!delete_key && !strncmp(macStr, "none", MAX_ADDR_SIZE))
    {
        SWSS_LOG_NOTICE("Mac address is 'none' for ADD op, ignoring for %s", ipStr);
        return;
    }

    /* Ignore neighbor entries with Broadcast/Null Mac */
    if (!delete_key
        && ((lladdr == NULL)
            || (MacAddress(macStr) == MacAddress("ff:ff:ff:ff:ff:ff"))))
    {
        SWSS_LOG_INFO("Broadcast/Null Mac received, ignoring for %s", ipStr);
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
