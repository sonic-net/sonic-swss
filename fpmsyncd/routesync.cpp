#include <netlink/route/link.h>
#include <netlink/route/route.h>
#include <netlink/route/nexthop.h>
#include "logger.h"
#include "select.h"
#include "netmsg.h"
#include "ipprefix.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "fpmsyncd/fpmlink.h"
#include "fpmsyncd/routesync.h"
#include <string.h>

using namespace std;
using namespace swss;

#define VXLAN_IF_NAME_PREFIX    "Brvxlan"
#define VNET_PREFIX             "Vnet"

RouteSync::RouteSync(RedisPipeline *pipeline) :
    m_routeTable(pipeline, APP_ROUTE_TABLE_NAME, true),
    m_vnet_routeTable(pipeline, APP_VNET_RT_TABLE_NAME, true),
    m_vnet_tunnelTable(pipeline, APP_VNET_RT_TUNNEL_TABLE_NAME, true),
    m_warmStartHelper(pipeline, &m_routeTable, APP_ROUTE_TABLE_NAME, "bgp", "bgp"),
    m_nl_sock(NULL), m_link_cache(NULL)
{
    m_nl_sock = nl_socket_alloc();
    nl_connect(m_nl_sock, NETLINK_ROUTE);
    rtnl_link_alloc_cache(m_nl_sock, AF_UNSPEC, &m_link_cache);
}

void RouteSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    struct rtnl_route *route_obj = (struct rtnl_route *)obj;

    /* Supports IPv4 or IPv6 address, otherwise return immediately */
    auto family = rtnl_route_get_family(route_obj);
    if (family != AF_INET && family != AF_INET6)
    {
        SWSS_LOG_INFO("Unknown route family support (object: %s)", nl_object_get_type(obj));
        return;
    }

    /* Get the index of the master device */
    unsigned int master_index = rtnl_route_get_table(route_obj);
    char master_name[IFNAMSIZ] = {0};

    /* if the table_id is not set in the route obj then route is for default vrf. */
    if (master_index)
    {
        /* Get the name of the master device */
        getIfName(master_index, master_name, IFNAMSIZ);
    
        /* If the master device name starts with VNET_PREFIX, it is a VNET route.
           The VNET name is exactly the name of the associated master device. */
        if (string(master_name).find(VNET_PREFIX) == 0)
        {
            onVnetRouteMsg(nlmsg_type, obj, string(master_name));
        }
        /* Otherwise, it is a regular route (include VRF route). */
        else
        {
            onRouteMsg(nlmsg_type, obj);
        }

    }
    else
    {
        onRouteMsg(nlmsg_type, obj);
    }
}

/* 
 * Handle regular route (include VRF route) 
 * @arg nlmsg_type      Netlink message type
 * @arg obj             Netlink object
 */
void RouteSync::onRouteMsg(int nlmsg_type, struct nl_object *obj)
{
    struct rtnl_route *route_obj = (struct rtnl_route *)obj;
    struct nl_addr *dip;
    char destipprefix[MAX_ADDR_SIZE + 1] = {0};

    dip = rtnl_route_get_dst(route_obj);
    nl_addr2str(dip, destipprefix, MAX_ADDR_SIZE);
    SWSS_LOG_DEBUG("Receive new route message dest ip prefix: %s", destipprefix);

    /*
     * Upon arrival of a delete msg we could either push the change right away,
     * or we could opt to defer it if we are going through a warm-reboot cycle.
     */
    bool warmRestartInProgress = m_warmStartHelper.inProgress();

    if (nlmsg_type == RTM_DELROUTE)
    {
        if (!warmRestartInProgress)
        {
            m_routeTable.del(destipprefix);
            return;
        }
        else
        {
            SWSS_LOG_INFO("Warm-Restart mode: Receiving delete msg: %s",
                          destipprefix);

            vector<FieldValueTuple> fvVector;
            const KeyOpFieldsValuesTuple kfv = std::make_tuple(destipprefix,
                                                               DEL_COMMAND,
                                                               fvVector);
            m_warmStartHelper.insertRefreshMap(kfv);
            return;
        }
    }
    else if (nlmsg_type != RTM_NEWROUTE)
    {
        SWSS_LOG_INFO("Unknown message-type: %d for %s", nlmsg_type, destipprefix);
        return;
    }

    switch (rtnl_route_get_type(route_obj))
    {
        case RTN_BLACKHOLE:
        {
            vector<FieldValueTuple> fvVector;
            FieldValueTuple fv("blackhole", "true");
            fvVector.push_back(fv);
            m_routeTable.set(destipprefix, fvVector);
            return;
        }
        case RTN_UNICAST:
            break;

        case RTN_MULTICAST:
        case RTN_BROADCAST:
        case RTN_LOCAL:
            SWSS_LOG_INFO("BUM routes aren't supported yet (%s)", destipprefix);
            return;

        default:
            return;
    }

    struct nl_list_head *nhs = rtnl_route_get_nexthops(route_obj);
    if (!nhs)
    {
        SWSS_LOG_INFO("Nexthop list is empty for %s", destipprefix);
        return;
    }

    /* Get nexthop lists */
    string nexthops = getNextHopGw(route_obj);
    string ifnames = getNextHopIf(route_obj);

    vector<FieldValueTuple> fvVector;
    FieldValueTuple nh("nexthop", nexthops);
    FieldValueTuple idx("ifname", ifnames);

    fvVector.push_back(nh);
    fvVector.push_back(idx);

    if (!warmRestartInProgress)
    {
        m_routeTable.set(destipprefix, fvVector);
        SWSS_LOG_DEBUG("RouteTable set msg: %s %s %s",
                       destipprefix, nexthops.c_str(), ifnames.c_str());
    }

    /*
     * During routing-stack restarting scenarios route-updates will be temporarily
     * put on hold by warm-reboot logic.
     */
    else
    {
        SWSS_LOG_INFO("Warm-Restart mode: RouteTable set msg: %s %s %s",
                      destipprefix, nexthops.c_str(), ifnames.c_str());

        const KeyOpFieldsValuesTuple kfv = std::make_tuple(destipprefix,
                                                           SET_COMMAND,
                                                           fvVector);
        m_warmStartHelper.insertRefreshMap(kfv);
    }
}

/* 
 * Handle vnet route 
 * @arg nlmsg_type      Netlink message type
 * @arg obj             Netlink object
 * @arg vnet            Vnet name
 */     
void RouteSync::onVnetRouteMsg(int nlmsg_type, struct nl_object *obj, string vnet)
{
    struct rtnl_route *route_obj = (struct rtnl_route *)obj;

    /* Get the destination IP prefix */
    struct nl_addr *dip = rtnl_route_get_dst(route_obj);
    char destipprefix[MAX_ADDR_SIZE + 1] = {0};
    nl_addr2str(dip, destipprefix, MAX_ADDR_SIZE);

    string vnet_dip =  vnet + string(":") + destipprefix;
    SWSS_LOG_DEBUG("Receive new vnet route message %s", vnet_dip.c_str());

    if (nlmsg_type == RTM_DELROUTE)
    {
        /* Duplicated delete as we do not know if it is a VXLAN tunnel route*/
        m_vnet_routeTable.del(vnet_dip);
        m_vnet_tunnelTable.del(vnet_dip);
        return;
    }
    else if (nlmsg_type != RTM_NEWROUTE)
    {
        SWSS_LOG_INFO("Unknown message-type: %d for %s", nlmsg_type, vnet_dip.c_str());
        return;
    }

    switch (rtnl_route_get_type(route_obj))
    {
        case RTN_UNICAST:
            break;

        /* We may support blackhole in the future */
        case RTN_BLACKHOLE:
            SWSS_LOG_INFO("Blackhole route is supported yet (%s)", vnet_dip.c_str());
            return;

        case RTN_MULTICAST:
        case RTN_BROADCAST:
        case RTN_LOCAL:
            SWSS_LOG_INFO("BUM routes aren't supported yet (%s)", vnet_dip.c_str());
            return;

        default:
            return;
    }

    struct nl_list_head *nhs = rtnl_route_get_nexthops(route_obj);
    if (!nhs)
    {
        SWSS_LOG_INFO("Nexthop list is empty for %s", vnet_dip.c_str());
        return;
    }

    /* Get nexthop lists */
    string nexthops = getNextHopGw(route_obj);
    string ifnames = getNextHopIf(route_obj);

    /* If the the first interface name starts with VXLAN_IF_NAME_PREFIX,
       the route is a VXLAN tunnel route. */
    if (ifnames.find(VXLAN_IF_NAME_PREFIX) == 0)
    {
        vector<FieldValueTuple> fvVector;
        FieldValueTuple ep("endpoint", nexthops);
        fvVector.push_back(ep);

        m_vnet_tunnelTable.set(vnet_dip, fvVector);
        SWSS_LOG_DEBUG("%s set msg: %s %s",
                       APP_VNET_RT_TUNNEL_TABLE_NAME, vnet_dip.c_str(), nexthops.c_str());
        return;
    }
    /* Regular VNET route */
    else
    {
        vector<FieldValueTuple> fvVector;
        FieldValueTuple idx("ifname", ifnames);
        fvVector.push_back(idx);

        /* If the route has at least one next hop gateway, e.g., nexthops does not only have ',' */
        if (nexthops.length() + 1 > (unsigned int)rtnl_route_get_nnexthops(route_obj))
        {
            FieldValueTuple nh("nexthop", nexthops);
            fvVector.push_back(nh);
            SWSS_LOG_DEBUG("%s set msg: %s %s %s",
                           APP_VNET_RT_TABLE_NAME, vnet_dip.c_str(), ifnames.c_str(), nexthops.c_str());
        }
        else
        {
            SWSS_LOG_DEBUG("%s set msg: %s %s",
                           APP_VNET_RT_TABLE_NAME, vnet_dip.c_str(), ifnames.c_str());
        }

        m_vnet_routeTable.set(vnet_dip, fvVector);
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
bool RouteSync::getIfName(int if_index, char *if_name, size_t name_len)
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

/*
 * Get next hop gateway IP addresses
 * @arg route_obj     route object
 *
 * Return concatenation of IP addresses: gw0 + "," + gw1 + .... + "," + gwN
 */
string RouteSync::getNextHopGw(struct rtnl_route *route_obj)
{
    string result = "";

    for (int i = 0; i < rtnl_route_get_nnexthops(route_obj); i++)
    {
        struct rtnl_nexthop *nexthop = rtnl_route_nexthop_n(route_obj, i);
        struct nl_addr *addr = rtnl_route_nh_get_gateway(nexthop);

        /* Next hop gateway is not empty */
        if (addr)
        {
            char gw_ip[MAX_ADDR_SIZE + 1] = {0};
            nl_addr2str(addr, gw_ip, MAX_ADDR_SIZE);
            result += gw_ip;
        }

        if (i + 1 < rtnl_route_get_nnexthops(route_obj))
        {
            result += string(",");
        }
    }

    return result;
}

/*
 * Get next hop interface names
 * @arg route_obj     route object
 *
 * Return concatenation of interface names: if0 + "," + if1 + .... + "," + ifN
 */
string RouteSync::getNextHopIf(struct rtnl_route *route_obj)
{
    string result = "";

    for (int i = 0; i < rtnl_route_get_nnexthops(route_obj); i++)
    {
        struct rtnl_nexthop *nexthop = rtnl_route_nexthop_n(route_obj, i);
        /* Get the ID of next hop interface */
        unsigned if_index = rtnl_route_nh_get_ifindex(nexthop);
        char if_name[IFNAMSIZ] = "0";

        /* If we cannot get the interface name */
        if (!getIfName(if_index, if_name, IFNAMSIZ))
        {
            strcpy(if_name, "unknown");
        }

        result += if_name;

        if (i + 1 < rtnl_route_get_nnexthops(route_obj))
        {
            result += string(",");
        }
    }

    return result;
}
