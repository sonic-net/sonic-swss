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

using namespace std;
using namespace swss;

RouteSync::RouteSync(RedisPipeline *pipeline) :
    m_routeTable(pipeline, APP_ROUTE_TABLE_NAME, true),
    m_warmStartHelper(pipeline, &m_routeTable, "bgp", "bgp")
{
    m_nl_sock = nl_socket_alloc();
    nl_connect(m_nl_sock, NETLINK_ROUTE);
    rtnl_link_alloc_cache(m_nl_sock, AF_UNSPEC, &m_link_cache);
}

void RouteSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    struct rtnl_route *route_obj = (struct rtnl_route *)obj;
    struct nl_addr *dip;
    char destipprefix[MAX_ADDR_SIZE + 1] = {0};

    dip = rtnl_route_get_dst(route_obj);
    nl_addr2str(dip, destipprefix, MAX_ADDR_SIZE);
    SWSS_LOG_DEBUG("Receive new route message dest ip prefix: %s\n", destipprefix);
    /* Supports IPv4 or IPv6 address, otherwise return immediately */
    auto family = rtnl_route_get_family(route_obj);
    if (family != AF_INET && family != AF_INET6)
    {
        SWSS_LOG_INFO("Unknown route family support: %s (object: %s)\n", destipprefix, nl_object_get_type(obj));
        return;
    }

    /*
     * Upon arrival of a delete msg we could either push the change right away,
     * or we could opt to defer it if we are in the middle of a warm-reboot
     * process. The goal here is to avoid unnecessary churn in swss/syncd layers.
     */
    auto warmState = m_warmStartHelper.getState();

    if (nlmsg_type == RTM_DELROUTE)
    {
        if (warmState == WarmStart::INITIALIZED ||
            warmState == WarmStart::RECONCILED)
        {
            m_routeTable.del(destipprefix);
            return;
        }
        else
        {
            SWSS_LOG_INFO("Warm-Restart: Receiving delete msg: %s\n", destipprefix);

            vector<FieldValueTuple> fvVector;
            const KeyOpFieldsValuesTuple kfv = std::make_tuple(destipprefix, "", fvVector);
            m_warmStartHelper.removeRecoveryMap(kfv, WarmStartHelper::DELETE);
            return;
        }
    }
    else if (nlmsg_type != RTM_NEWROUTE)
    {
        SWSS_LOG_INFO("Unknown message-type: %d for %s\n", nlmsg_type, destipprefix);
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
            SWSS_LOG_INFO("BUM routes aren't supported yet (%s)\n", destipprefix);
            return;

        default:
            return;
    }

    /* Geting nexthop lists */
    string nexthops;
    string ifnames;

    struct nl_list_head *nhs = rtnl_route_get_nexthops(route_obj);
    if (!nhs)
    {
        SWSS_LOG_INFO("Nexthop list is empty for %s\n", destipprefix);
        return;
    }

    char ifname[IFNAMSIZ] = {0};
    for (int i = 0; i < rtnl_route_get_nnexthops(route_obj); i++)
    {
        struct rtnl_nexthop *nexthop = rtnl_route_nexthop_n(route_obj, i);
        struct nl_addr *addr = rtnl_route_nh_get_gateway(nexthop);
        unsigned int ifindex = rtnl_route_nh_get_ifindex(nexthop);

        if (addr != NULL)
        {
            char gwipprefix[MAX_ADDR_SIZE + 1] = {0};
            nl_addr2str(addr, gwipprefix, MAX_ADDR_SIZE);
            nexthops += gwipprefix;
        }

        rtnl_link_i2name(m_link_cache, ifindex, ifname, IFNAMSIZ);
        /* Cannot get ifname. Possibly interfaces get re-created. */
        if (!strlen(ifname))
        {
            rtnl_link_alloc_cache(m_nl_sock, AF_UNSPEC, &m_link_cache);
            rtnl_link_i2name(m_link_cache, ifindex, ifname, IFNAMSIZ);
            if (!strlen(ifname))
                strcpy(ifname, "unknown");
        }
        ifnames += ifname;

        if (i + 1 < rtnl_route_get_nnexthops(route_obj))
        {
            nexthops += string(",");
            ifnames += string(",");
        }
    }

    vector<FieldValueTuple> fvVector;
    FieldValueTuple nh("nexthop", nexthops);
    FieldValueTuple idx("ifname", ifnames);

    fvVector.push_back(nh);
    fvVector.push_back(idx);

    if (warmState == WarmStart::INITIALIZED ||
        warmState == WarmStart::RECONCILED)
    {
        m_routeTable.set(destipprefix, fvVector);
        SWSS_LOG_DEBUG("RouteTable set msg: %s %s %s\n",
                       destipprefix, nexthops.c_str(), ifnames.c_str());
    }

    /*
     * During routing-stack restarting scenarios route-updates will be temporarily
     * put on hold by warm-reboot logic.
     */
    else
    {
        SWSS_LOG_INFO("Warm-Restart: RouteTable set msg: %s %s %s\n",
                      destipprefix, nexthops.c_str(), ifnames.c_str());

        const KeyOpFieldsValuesTuple kfv = std::make_tuple(destipprefix, "", fvVector);
        m_warmStartHelper.insertRecoveryMap(kfv, WarmStartHelper::CLEAN);
    }
}
