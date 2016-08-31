#include <netlink/route/link.h>
#include <netlink/route/route.h>
#include <netlink/route/nexthop.h>
#include "logger.h"
#include "select.h"
#include "netmsg.h"
#include "ipprefix.h"
#include "dbconnector.h"
#include "producertable.h"
#include "fpmsyncd/fpmlink.h"
#include "fpmsyncd/routesync.h"

using namespace std;
using namespace swss;

RouteSync::RouteSync(DBConnector *db) :
    m_routeTable(db, APP_ROUTE_TABLE_NAME)
{
    m_nl_sock = nl_socket_alloc();
    nl_connect(m_nl_sock, NETLINK_ROUTE);
    rtnl_link_alloc_cache(m_nl_sock, AF_UNSPEC, &m_link_cache);
}

void RouteSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    struct rtnl_route *route_obj = (struct rtnl_route *)obj;
    struct nl_addr *dip;
    char addrStr[MAX_ADDR_SIZE + 1] = {0};
    uint32_t ipv4;
    int prefix;

    dip = rtnl_route_get_dst(route_obj);
    /* Supports IPv4 address only for now */
    if (rtnl_route_get_family(route_obj)  != AF_INET)
    {
        nl_addr2str(dip, addrStr, MAX_ADDR_SIZE);
        SWSS_LOG_INFO("%s: Unknown route family support: %s (object: %s)\n",
                      __FUNCTION__, addrStr, nl_object_get_type(obj));
        return;
    }

    prefix = nl_addr_get_prefixlen(dip);
    ipv4 = *(uint32_t*)nl_addr_get_binary_addr(dip);
    IpPrefix destip(ipv4, prefix);

    if (nlmsg_type == RTM_DELROUTE)
    {
        m_routeTable.del(destip.to_string());
        return;
    }
    else if (nlmsg_type != RTM_NEWROUTE)
    {
        nl_addr2str(dip, addrStr, MAX_ADDR_SIZE);
        SWSS_LOG_INFO("%s: Unknown message-type: %d for %s\n",
                      __FUNCTION__, nlmsg_type, addrStr);
        return;
    }

    switch (rtnl_route_get_type(route_obj))
    {
        case RTN_BLACKHOLE:
            {
                std::vector<FieldValueTuple> fvVector;
                FieldValueTuple fv("blackhole", "true");
                fvVector.push_back(fv);
                m_routeTable.set(destip.to_string(), fvVector);
                return;
            }
        case RTN_UNICAST:
            break;

        case RTN_MULTICAST:
        case RTN_BROADCAST:
        case RTN_LOCAL:
            nl_addr2str(dip, addrStr, MAX_ADDR_SIZE);
            SWSS_LOG_INFO("%s: BUM routes aren't supported yet (%s)\n",
                          __FUNCTION__, addrStr);
            return;

        default:
            return;
    }

    /* Geting nexthop lists */
    string nexthops;
    string ifindexes;

    struct nl_list_head *nhs = rtnl_route_get_nexthops(route_obj);
    if (!nhs)
    {
        nl_addr2str(dip, addrStr, MAX_ADDR_SIZE);
        SWSS_LOG_INFO("%s: Nexthop list is empty for %s\n",
                      __FUNCTION__, addrStr);
        return;
    }

    for (int i = 0; i < rtnl_route_get_nnexthops(route_obj); i++)
    {
        struct rtnl_nexthop *nexthop = rtnl_route_nexthop_n(route_obj, i);
        struct nl_addr *addr = rtnl_route_nh_get_gateway(nexthop);
        unsigned int ifindex = rtnl_route_nh_get_ifindex(nexthop);

        if (addr != NULL)
        {
            IpAddress nh(*(uint32_t*)nl_addr_get_binary_addr(addr));
            nexthops += nh.to_string();
        }

        /* 0 means no ifindex */
        if (m_link_cache)
        {
            rtnl_link_i2name(m_link_cache, ifindex, addrStr, MAX_ADDR_SIZE);
            if (!strlen(addrStr))
            {
                rtnl_link_alloc_cache(m_nl_sock, AF_UNSPEC, &m_link_cache);
            }
            ifindexes += addrStr;
        } else
        {
            ifindexes += to_string(ifindex);
        }


        if (i + 1 < rtnl_route_get_nnexthops(route_obj))
        {
            nexthops += string(",");
            ifindexes += string(",");
        }
    }

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple nh("nexthop", nexthops);
    FieldValueTuple idx("ifindex", ifindexes);
    fvVector.push_back(nh);
    fvVector.push_back(idx);
    m_routeTable.set(destip.to_string(), fvVector);
}
