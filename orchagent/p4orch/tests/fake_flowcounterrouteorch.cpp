#include "copporch.h"
#include "flowcounterrouteorch.h"

extern size_t gMaxBulkSize;
extern sai_route_api_t *sai_route_api;

#define ROUTE_FLOW_COUNTER_POLLING_INTERVAL_MS 10000

FlowCounterRouteOrch::FlowCounterRouteOrch(swss::DBConnector *db, const std::vector<std::string> &tableNames)
    : Orch(db, tableNames), mRouteFlowCounterMgr(ROUTE_FLOW_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ,
                                                 ROUTE_FLOW_COUNTER_POLLING_INTERVAL_MS, false),
      gRouteBulker(sai_route_api, gMaxBulkSize)
{
}

FlowCounterRouteOrch::~FlowCounterRouteOrch(void)
{
}

void FlowCounterRouteOrch::generateRouteFlowStats()
{
}

void FlowCounterRouteOrch::clearRouteFlowStats()
{
}

void FlowCounterRouteOrch::addRoutePattern(const std::string &pattern, size_t)
{
}

void FlowCounterRouteOrch::removeRoutePattern(const std::string &pattern)
{
}

void FlowCounterRouteOrch::onAddMiscRouteEntry(sai_object_id_t vrf_id, const IpPrefix &ip_prefix, bool add_to_cache)
{
}

void FlowCounterRouteOrch::onAddMiscRouteEntry(sai_object_id_t vrf_id, const sai_ip_prefix_t &ip_pfx, bool add_to_cache)
{
}

void FlowCounterRouteOrch::onRemoveMiscRouteEntry(sai_object_id_t vrf_id, const IpPrefix &ip_prefix,
                                                  bool remove_from_cache)
{
}

void FlowCounterRouteOrch::onRemoveMiscRouteEntry(sai_object_id_t vrf_id, const sai_ip_prefix_t &ip_pfx,
                                                  bool remove_from_cache)
{
}

void FlowCounterRouteOrch::onAddVR(sai_object_id_t vrf_id)
{
}

void FlowCounterRouteOrch::onRemoveVR(sai_object_id_t vrf_id)
{
}

void FlowCounterRouteOrch::handleRouteAdd(sai_object_id_t vrf_id, const IpPrefix &ip_prefix)
{
}

void FlowCounterRouteOrch::handleRouteRemove(sai_object_id_t vrf_id, const IpPrefix &ip_prefix)
{
}

void FlowCounterRouteOrch::processRouteFlowCounterBinding()
{
}

void FlowCounterRouteOrch::doTask(Consumer &consumer)
{
}

void FlowCounterRouteOrch::doTask(SelectableTimer &timer)
{
}

void FlowCounterRouteOrch::initRouteFlowCounterCapability()
{
}

void FlowCounterRouteOrch::removeRoutePattern(const RoutePattern &route_pattern)
{
}

void FlowCounterRouteOrch::removeRouteFlowCounterFromDB(sai_object_id_t vrf_id, const IpPrefix &ip_prefix,
                                                        sai_object_id_t counter_oid)
{
}

bool FlowCounterRouteOrch::bindFlowCounter(const RoutePattern &route_pattern, sai_object_id_t vrf_id,
                                           const IpPrefix &ip_prefix)
{
    return true;
}

void FlowCounterRouteOrch::unbindFlowCounter(const RoutePattern &route_pattern, sai_object_id_t vrf_id,
                                             const IpPrefix &ip_prefix, sai_object_id_t counter_oid)
{
}

void FlowCounterRouteOrch::pendingUpdateFlexDb(const RoutePattern &route_pattern, const IpPrefix &ip_prefix,
                                               sai_object_id_t counter_oid)
{
}

void FlowCounterRouteOrch::updateRouterFlowCounterCache(const RoutePattern &route_pattern, const IpPrefix &ip_prefix,
                                                        sai_object_id_t counter_oid, RouterFlowCounterCache &cache)
{
}

bool FlowCounterRouteOrch::validateRoutePattern(const RoutePattern &route_pattern) const
{
    return true;
}

void FlowCounterRouteOrch::onRoutePatternMaxMatchCountChange(RoutePattern &route_pattern, size_t new_max_match_count)
{
}

bool FlowCounterRouteOrch::isRouteAlreadyBound(const RoutePattern &route_pattern, const IpPrefix &ip_prefix) const
{
    return true;
}

void FlowCounterRouteOrch::createRouteFlowCounterByPattern(const RoutePattern &route_pattern, size_t currentBoundCount)
{
}

bool FlowCounterRouteOrch::removeRouteFlowCounter(const RoutePattern &route_pattern, sai_object_id_t vrf_id,
                                                  const IpPrefix &ip_prefix)
{
    return true;
}

void FlowCounterRouteOrch::createRouteFlowCounterFromVnetRoutes(const RoutePattern &route_pattern,
                                                                size_t &current_bound_count)
{
}

void FlowCounterRouteOrch::reapRouteFlowCounterByPattern(const RoutePattern &route_pattern, size_t currentBoundCount)
{
}

bool FlowCounterRouteOrch::isRouteFlowCounterEnabled() const
{
    return true;
}

void FlowCounterRouteOrch::getRouteFlowCounterNameMapKey(sai_object_id_t vrf_id, const IpPrefix &ip_prefix,
                                                         std::string &key)
{
}

size_t FlowCounterRouteOrch::getRouteFlowCounterSizeByPattern(const RoutePattern &route_pattern) const
{
    return 0;
}

bool FlowCounterRouteOrch::parseRouteKeyForRoutePattern(const std::string &key, char sep, sai_object_id_t &vrf_id,
                                                        IpPrefix &ip_prefix, std::string &vrf_name)
{
    return true;
}

bool FlowCounterRouteOrch::getVrfIdByVnetName(const std::string &vnet_name, sai_object_id_t &vrf_id)
{
    return true;
}

bool FlowCounterRouteOrch::getVnetNameByVrfId(sai_object_id_t vrf_id, std::string &vnet_name)
{
    return true;
}
