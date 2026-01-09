#include "routeorch.h"
#include "nhgorch.h"
#include "cbf/cbfnhgorch.h"

extern size_t gMaxBulkSize;
extern sai_object_id_t gSwitchId;
extern sai_next_hop_group_api_t*    sai_next_hop_group_api;
extern sai_route_api_t*             sai_route_api;
extern sai_mpls_api_t*              sai_mpls_api;
extern NhgOrch *gNhgOrch;
extern CbfNhgOrch *gCbfNhgOrch;

RouteOrch::RouteOrch(DBConnector *db, vector<table_name_with_pri_t> &tableNames, SwitchOrch *switchOrch, NeighOrch *neighOrch, IntfsOrch *intfsOrch, VRFOrch *vrfOrch, FgNhgOrch *fgNhgOrch, Srv6Orch *srv6Orch) :
        gRouteBulker(sai_route_api, gMaxBulkSize),
        gLabelRouteBulker(sai_mpls_api, gMaxBulkSize),
        gNextHopGroupMemberBulker(sai_next_hop_group_api, gSwitchId, gMaxBulkSize),
        Orch(db, tableNames),
        m_switchOrch(switchOrch),
        m_neighOrch(neighOrch),
        m_intfsOrch(intfsOrch),
        m_vrfOrch(vrfOrch),
        m_fgNhgOrch(fgNhgOrch),
        m_nextHopGroupCount(0),
        m_srv6Orch(srv6Orch),
        m_resync(false),
        m_appTunnelDecapTermProducer(db, APP_TUNNEL_DECAP_TERM_TABLE_NAME)
{
}

std::string RouteOrch::getLinkLocalEui64Addr(void)
{
    return "";
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

sai_object_id_t RouteOrch::getNextHopGroupId(const NextHopGroupKey& nexthops)
{
    return m_syncdNextHopGroups[nexthops].next_hop_group_id;
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

const NextHopGroupKey RouteOrch::getSyncdRouteNhgKey(sai_object_id_t vrf_id, const IpPrefix& ipPrefix)
{
    NextHopGroupKey nhg;
    return nhg;
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

const NhgBase &RouteOrch::getNhg(const std::string &nhg_index)
{
    try
    {
        return gNhgOrch->getNhg(nhg_index);
    }
    catch (const std::out_of_range& e)
    {
        return gCbfNhgOrch->getNhg(nhg_index);
    }
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

inline bool RouteOrch::isVipRoute(const IpPrefix &ipPrefix, const NextHopGroupKey &nextHops)
{
    return true;
}

inline void RouteOrch::createVipRouteSubnetDecapTerm(const IpPrefix &ipPrefix)
{
}

inline void RouteOrch::removeVipRouteSubnetDecapTerm(const IpPrefix &ipPrefix)
{
}