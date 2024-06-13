extern "C"
{
#include "sai.h"
}
#include <inttypes.h>
#include "routeorch.h"
#include "nhgorch.h"
#include "table.h"

/* Default maximum number of next hop groups */
#define DEFAULT_NUMBER_OF_ECMP_GROUPS   128
#define DEFAULT_MAX_ECMP_GROUP_SIZE     32

extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t gSwitchId;

extern sai_next_hop_group_api_t*    sai_next_hop_group_api;
extern sai_route_api_t*             sai_route_api;
extern sai_mpls_api_t*              sai_mpls_api;
extern sai_switch_api_t*            sai_switch_api;
extern size_t gMaxBulkSize;

RouteOrch::RouteOrch(DBConnector *db, vector<table_name_with_pri_t> &tableNames, SwitchOrch *switchOrch, NeighOrch *neighOrch, IntfsOrch *intfsOrch, VRFOrch *vrfOrch, FgNhgOrch *fgNhgOrch, Srv6Orch *srv6Orch) :
        gRouteBulker(sai_route_api, gMaxBulkSize),
        gLabelRouteBulker(sai_mpls_api, gMaxBulkSize),
        gNextHopGroupMemberBulker(sai_next_hop_group_api, gSwitchId, gMaxBulkSize),
        Orch(db, tableNames)
{

}

std::string RouteOrch::getLinkLocalEui64Addr(const MacAddress &mac)
{
    SWSS_LOG_ENTER();

    string        ip_prefix;
    const uint8_t *gmac = mac.getMac();

    uint8_t        eui64_interface_id[EUI64_INTF_ID_LEN];
    char           ipv6_ll_addr[INET6_ADDRSTRLEN] = {0};

    /* Link-local IPv6 address autogenerated by kernel with eui64 interface-id
     * derived from the MAC address of the host interface.
     */
    eui64_interface_id[0] = gmac[0] ^ 0x02;
    eui64_interface_id[1] = gmac[1];
    eui64_interface_id[2] = gmac[2];
    eui64_interface_id[3] = 0xff;
    eui64_interface_id[4] = 0xfe;
    eui64_interface_id[5] = gmac[3];
    eui64_interface_id[6] = gmac[4];
    eui64_interface_id[7] = gmac[5];

    snprintf(ipv6_ll_addr, INET6_ADDRSTRLEN, "fe80::%02x%02x:%02x%02x:%02x%02x:%02x%02x",
             eui64_interface_id[0], eui64_interface_id[1], eui64_interface_id[2],
             eui64_interface_id[3], eui64_interface_id[4], eui64_interface_id[5],
             eui64_interface_id[6], eui64_interface_id[7]);

    ip_prefix = string(ipv6_ll_addr);

    return ip_prefix;
}

void RouteOrch::addLinkLocalRouteToMe(sai_object_id_t vrf_id, IpPrefix linklocal_prefix)
{

}

void RouteOrch::delLinkLocalRouteToMe(sai_object_id_t vrf_id, IpPrefix linklocal_prefix)
{

}

void RouteOrch::updateDefRouteState(string ip, bool add)
{
}

bool RouteOrch::hasNextHopGroup(const NextHopGroupKey& nexthops) const
{
    return true;
}

void RouteOrch::attach(Observer *observer, const IpAddress& dstAddr, sai_object_id_t vrf_id)
{
}

void RouteOrch::detach(Observer *observer, const IpAddress& dstAddr, sai_object_id_t vrf_id)
{

}

bool RouteOrch::validnexthopinNextHopGroup(const NextHopKey &nexthop, uint32_t& count)
{
    return true;
}

bool RouteOrch::invalidnexthopinNextHopGroup(const NextHopKey &nexthop, uint32_t& count)
{
    return true;
}

void RouteOrch::doTask(Consumer& consumer)
{

}

void RouteOrch::notifyNextHopChangeObservers(sai_object_id_t vrf_id, const IpPrefix &prefix, const NextHopGroupKey &nexthops, bool add)
{

}

void RouteOrch::increaseNextHopRefCount(const NextHopGroupKey &nexthops)
{

}

void RouteOrch::decreaseNextHopRefCount(const NextHopGroupKey &nexthops)
{

}

bool RouteOrch::isRefCounterZero(const NextHopGroupKey &nexthops) const
{
    return true;
}

bool RouteOrch::createFineGrainedNextHopGroup(sai_object_id_t &next_hop_group_id, vector<sai_attribute_t> &nhg_attrs)
{
    return true;
}

bool RouteOrch::removeFineGrainedNextHopGroup(sai_object_id_t &next_hop_group_id)
{
    return true;
}

bool RouteOrch::addNextHopGroup(const NextHopGroupKey &nexthops)
{
    return true;
}

bool RouteOrch::removeNextHopGroup(const NextHopGroupKey &nexthops)
{
    return true;
}

void RouteOrch::addNextHopRoute(const NextHopKey& nextHop, const RouteKey& routeKey)
{

}

void RouteOrch::removeNextHopRoute(const NextHopKey& nextHop, const RouteKey& routeKey)
{

}

bool RouteOrch::updateNextHopRoutes(const NextHopKey& nextHop, uint32_t& numRoutes)
{
    return true;
}

bool RouteOrch::getRoutesForNexthop(std::set<RouteKey>& routeKeys, const NextHopKey& nexthopKey)
{
    return true;
}

void RouteOrch::addTempRoute(RouteBulkContext& ctx, const NextHopGroupKey &nextHops)
{

}

bool RouteOrch::addRoute(RouteBulkContext& ctx, const NextHopGroupKey &nextHops)
{
    return true;
}

bool RouteOrch::addRoutePost(const RouteBulkContext& ctx, const NextHopGroupKey &nextHops)
{
    return true;
}

bool RouteOrch::removeRoute(RouteBulkContext& ctx)
{
    return true;
}

bool RouteOrch::removeRoutePost(const RouteBulkContext& ctx)
{
    return true;
}

bool RouteOrch::createRemoteVtep(sai_object_id_t vrf_id, const NextHopKey &nextHop)
{
    return true;
}

bool RouteOrch::deleteRemoteVtep(sai_object_id_t vrf_id, const NextHopKey &nextHop)
{
    return true;
}

bool RouteOrch::removeOverlayNextHops(sai_object_id_t vrf_id, const NextHopGroupKey &ol_nextHops)
{
    return true;
}

void RouteOrch::increaseNextHopGroupCount()
{
}

void RouteOrch::decreaseNextHopGroupCount()
{
}

bool RouteOrch::checkNextHopGroupCount()
{
    return true;
}


void RouteOrch::incNhgRefCount(const std::string &nhg_index)
{
}

void RouteOrch::decNhgRefCount(const std::string &nhg_index)
{

}

void RouteOrch::publishRouteState(const RouteBulkContext& ctx, const ReturnCode& status)
{

}
