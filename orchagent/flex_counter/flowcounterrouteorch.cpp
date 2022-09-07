#include "dbconnector.h"
#include "directory.h"
#include "flow_counter_handler.h"
#include "logger.h"
#include "routeorch.h"
#include "flowcounterrouteorch.h"
#include "schema.h"
#include "swssnet.h"
#include "table.h"
#include "vnetorch.h"

#include <string>

extern Directory<Orch*>  gDirectory;
extern RouteOrch*        gRouteOrch;
extern size_t            gMaxBulkSize;
extern sai_route_api_t*  sai_route_api;
extern sai_object_id_t   gVirtualRouterId;
extern sai_object_id_t   gSwitchId;

#define FLEX_COUNTER_UPD_INTERVAL                   1
#define FLOW_COUNTER_ROUTE_KEY                      "route"
#define FLOW_COUNTER_SUPPORT_FIELD                  "support"
#define ROUTE_PATTERN_MAX_MATCH_COUNT_FIELD         "max_match_count"
#define ROUTE_PATTERN_DEFAULT_MAX_MATCH_COUNT       30
#define ROUTE_FLOW_COUNTER_POLLING_INTERVAL_MS      10000

FlowCounterRouteOrch::FlowCounterRouteOrch(swss::DBConnector *db, const std::vector<std::string> &tableNames):
Orch(db, tableNames),
mAsicDb(std::shared_ptr<DBConnector>(new DBConnector("ASIC_DB", 0))),
mCounterDb(std::shared_ptr<DBConnector>(new DBConnector("COUNTERS_DB", 0))),
mVidToRidTable(std::unique_ptr<Table>(new Table(mAsicDb.get(), "VIDTORID"))),
mPrefixToCounterTable(std::unique_ptr<Table>(new Table(mCounterDb.get(), COUNTERS_ROUTE_NAME_MAP))),
mPrefixToPatternTable(std::unique_ptr<Table>(new Table(mCounterDb.get(), COUNTERS_ROUTE_TO_PATTERN_MAP))),
mRouteFlowCounterMgr(ROUTE_FLOW_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ, ROUTE_FLOW_COUNTER_POLLING_INTERVAL_MS, false),
gRouteBulker(sai_route_api, gMaxBulkSize)
{
    SWSS_LOG_ENTER();
    initRouteFlowCounterCapability();

    if (mRouteFlowCounterSupported)
    {
        auto intervT = timespec { .tv_sec = FLEX_COUNTER_UPD_INTERVAL , .tv_nsec = 0 };
        mFlexCounterUpdTimer = new SelectableTimer(intervT);
        auto executorT = new ExecutableTimer(mFlexCounterUpdTimer, this, "FLEX_COUNTER_UPD_TIMER");
        Orch::addExecutor(executorT);
    }
}

FlowCounterRouteOrch::~FlowCounterRouteOrch()
{
    SWSS_LOG_ENTER();
}

void FlowCounterRouteOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    if (!gRouteOrch || !mRouteFlowCounterSupported)
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        const auto &key =  kfvKey(t);
        const auto &op = kfvOp(t);
        const auto &data = kfvFieldsValues(t);
        if (op == SET_COMMAND)
        {
            size_t maxMatchCount = ROUTE_PATTERN_DEFAULT_MAX_MATCH_COUNT;
            for (auto valuePair : data)
            {
                const auto &field = fvField(valuePair);
                const auto &value = fvValue(valuePair);
                if (field == ROUTE_PATTERN_MAX_MATCH_COUNT_FIELD)
                {
                    maxMatchCount = (size_t)std::stoul(value);
                    if (maxMatchCount == 0)
                    {
                        SWSS_LOG_WARN("Max match count for route pattern cannot be 0, set it to default value 30");
                        maxMatchCount = ROUTE_PATTERN_DEFAULT_MAX_MATCH_COUNT;
                    }
                }
            }

            addRoutePattern(key, maxMatchCount);
        }
        else if (op == DEL_COMMAND)
        {
            removeRoutePattern(key);
        }
        consumer.m_toSync.erase(it++);
    }
}

void FlowCounterRouteOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("Add flex counters, pending in queue: %zu", mPendingAddToFlexCntr.size());
    string value;
    std::string nameMapKey;
    std::string pattern;
    vector<FieldValueTuple> prefixToCounterMap;
    vector<FieldValueTuple> prefixToPatternMap;
    for (auto it = mPendingAddToFlexCntr.begin(); it != mPendingAddToFlexCntr.end(); )
    {
        const auto& route_pattern = it->first;
        auto vrf_id = route_pattern.vrf_id;

        for(auto inner_iter = it->second.begin(); inner_iter != it->second.end(); )
        {
            const auto id = sai_serialize_object_id(inner_iter->second);
            if (mVidToRidTable->hget("", id, value))
            {
                auto ip_prefix = inner_iter->first;
                SWSS_LOG_INFO("Registering %s, id %s", ip_prefix.to_string().c_str(), id.c_str());

                std::unordered_set<std::string> counter_stats;
                FlowCounterHandler::getGenericCounterStatIdList(counter_stats);
                mRouteFlowCounterMgr.setCounterIdList(inner_iter->second, CounterType::ROUTE, counter_stats);

                getRouteFlowCounterNameMapKey(vrf_id, ip_prefix, nameMapKey);
                prefixToCounterMap.emplace_back(nameMapKey, id);

                getRouteFlowCounterNameMapKey(vrf_id, route_pattern.ip_prefix, pattern);
                prefixToPatternMap.emplace_back(nameMapKey, pattern);

                updateRouterFlowCounterCache(route_pattern, ip_prefix, inner_iter->second, mBoundRouteCounters);
                inner_iter = it->second.erase(inner_iter);
            }
            else
            {
                ++inner_iter;
            }
        }

        if (it->second.empty())
        {
            it = mPendingAddToFlexCntr.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (!prefixToCounterMap.empty())
    {
        mPrefixToCounterTable->set("", prefixToCounterMap);
    }

    if (!prefixToPatternMap.empty())
    {
        mPrefixToPatternTable->set("", prefixToPatternMap);
    }

    if (mPendingAddToFlexCntr.empty())
    {
        mFlexCounterUpdTimer->stop();
    }
}

void FlowCounterRouteOrch::initRouteFlowCounterCapability()
{
    SWSS_LOG_ENTER();
    mRouteFlowCounterSupported = FlowCounterHandler::queryRouteFlowCounterCapability();
    if (!mRouteFlowCounterSupported)
    {
        SWSS_LOG_NOTICE("Route flow counter is not supported on this platform");
    }
    swss::DBConnector state_db("STATE_DB", 0);
    swss::Table capability_table(&state_db, STATE_FLOW_COUNTER_CAPABILITY_TABLE_NAME);
    std::vector<FieldValueTuple> fvs;
    fvs.emplace_back(FLOW_COUNTER_SUPPORT_FIELD, mRouteFlowCounterSupported ? "true" : "false");
    capability_table.set(FLOW_COUNTER_ROUTE_KEY, fvs);
}

void FlowCounterRouteOrch::generateRouteFlowStats()
{
    SWSS_LOG_ENTER();
    if (!mRouteFlowCounterSupported)
    {
        return;
    }

    for (const auto &route_pattern : mRoutePatternSet)
    {
        createRouteFlowCounterByPattern(route_pattern, 0);
    }
}

void FlowCounterRouteOrch::clearRouteFlowStats()
{
    SWSS_LOG_ENTER();
    if (!mBoundRouteCounters.empty() || !mPendingAddToFlexCntr.empty())
    {
        for (auto &entry : mBoundRouteCounters)
        {
            const auto& route_pattern = entry.first;
            for (auto &inner_entry : entry.second)
            {
                removeRouteFlowCounterFromDB(route_pattern.vrf_id, inner_entry.first, inner_entry.second);
                unbindFlowCounter(route_pattern, route_pattern.vrf_id, inner_entry.first, inner_entry.second);
            }
        }

        for (auto &entry : mPendingAddToFlexCntr)
        {
            const auto& route_pattern = entry.first;
            for (auto &inner_entry : entry.second)
            {
                unbindFlowCounter(route_pattern, route_pattern.vrf_id, inner_entry.first, inner_entry.second);
            }
        }

        mBoundRouteCounters.clear();
        mPendingAddToFlexCntr.clear();
    }
}

void FlowCounterRouteOrch::addRoutePattern(const std::string &pattern, size_t max_match_count)
{
    SWSS_LOG_ENTER();
    sai_object_id_t vrf_id;
    IpPrefix ip_prefix;
    std::string vrf_name;
    if (!parseRouteKeyForRoutePattern(pattern, '|', vrf_id, ip_prefix, vrf_name))
    {
        vrf_id = SAI_NULL_OBJECT_ID;
    }

    auto insert_result = mRoutePatternSet.emplace(vrf_name, vrf_id, ip_prefix, max_match_count);
    if (insert_result.second)
    {
        SWSS_LOG_NOTICE("Inserting route pattern %s, max match count is %zu", pattern.c_str(), max_match_count);
        if (!validateRoutePattern(*insert_result.first))
        {
            mRoutePatternSet.erase(insert_result.first);
            return;
        }

        createRouteFlowCounterByPattern(*insert_result.first, 0);
    }
    else
    {
        SWSS_LOG_NOTICE("Updating route pattern %s max match count to %zu", pattern.c_str(), max_match_count);
        RoutePattern &existing = const_cast<RoutePattern &>(*insert_result.first);
        onRoutePatternMaxMatchCountChange(existing, max_match_count);
    }
}

void FlowCounterRouteOrch::removeRoutePattern(const std::string& pattern)
{
    SWSS_LOG_ENTER();
    sai_object_id_t vrf_id;
    IpPrefix ip_prefix;
    std::string vrf_name;
    if (!parseRouteKeyForRoutePattern(pattern, '|', vrf_id, ip_prefix, vrf_name))
    {
        vrf_id = SAI_NULL_OBJECT_ID;
    }

    SWSS_LOG_NOTICE("Removing route pattern %s", pattern.c_str());
    RoutePattern route_pattern(vrf_name, vrf_id, ip_prefix, 0);
    auto iter = mRoutePatternSet.find(route_pattern);
    if (iter == mRoutePatternSet.end())
    {
        // Should not go to this branch, just in case
        SWSS_LOG_ERROR("Trying to remove route pattern %s, but it does not exist", pattern.c_str());
        return;
    }
    mRoutePatternSet.erase(iter);

    removeRoutePattern(route_pattern);
}

void FlowCounterRouteOrch::removeRoutePattern(const RoutePattern &route_pattern)
{
    SWSS_LOG_ENTER();
    auto cache_iter = mBoundRouteCounters.find(route_pattern);
    if (cache_iter != mBoundRouteCounters.end())
    {
        for (auto &entry : cache_iter->second)
        {
            removeRouteFlowCounterFromDB(route_pattern.vrf_id, entry.first, entry.second);
            unbindFlowCounter(route_pattern, route_pattern.vrf_id, entry.first, entry.second);
        }
        mBoundRouteCounters.erase(cache_iter);
    }

    auto pending_iter = mPendingAddToFlexCntr.find(route_pattern);
    if (pending_iter != mPendingAddToFlexCntr.end())
    {
        for (auto &entry : pending_iter->second)
        {
            unbindFlowCounter(route_pattern, route_pattern.vrf_id, entry.first, entry.second);
        }
        mPendingAddToFlexCntr.erase(pending_iter);
    }
}

void FlowCounterRouteOrch::onAddMiscRouteEntry(sai_object_id_t vrf_id, const sai_ip_prefix_t& ip_pfx, bool add_to_cache)
{
    SWSS_LOG_ENTER();
    if (!mRouteFlowCounterSupported)
    {
        return;
    }

    IpPrefix ip_prefix = getIpPrefixFromSaiPrefix(ip_pfx);
    onAddMiscRouteEntry(vrf_id, ip_prefix, add_to_cache);
}

void FlowCounterRouteOrch::onAddMiscRouteEntry(sai_object_id_t vrf_id, const IpPrefix& ip_prefix, bool add_to_cache)
{
    SWSS_LOG_ENTER();
    if (!mRouteFlowCounterSupported)
    {
        return;
    }

    if (add_to_cache)
    {
        auto iter = mMiscRoutes.find(vrf_id);
        if (iter == mMiscRoutes.end())
        {
            mMiscRoutes.emplace(vrf_id, std::set<IpPrefix>({ip_prefix}));
        }
        else
        {
            iter->second.insert(ip_prefix);
        }
    }

    if (!isRouteFlowCounterEnabled())
    {
        return;
    }

    if (mRoutePatternSet.empty())
    {
        return;
    }

    handleRouteAdd(vrf_id, ip_prefix);
}

void FlowCounterRouteOrch::onRemoveMiscRouteEntry(sai_object_id_t vrf_id, const sai_ip_prefix_t& ip_pfx, bool remove_from_cache)
{
    SWSS_LOG_ENTER();
    if (!mRouteFlowCounterSupported)
    {
        return;
    }

    IpPrefix ip_prefix = getIpPrefixFromSaiPrefix(ip_pfx);
    onRemoveMiscRouteEntry(vrf_id, ip_prefix, remove_from_cache);
}

void FlowCounterRouteOrch::onRemoveMiscRouteEntry(sai_object_id_t vrf_id, const IpPrefix& ip_prefix, bool remove_from_cache)
{
    SWSS_LOG_ENTER();
    if (!mRouteFlowCounterSupported)
    {
        return;
    }

    if (remove_from_cache)
    {
        auto iter = mMiscRoutes.find(vrf_id);
        if (iter != mMiscRoutes.end())
        {
            auto prefix_iter = iter->second.find(ip_prefix);
            if (prefix_iter != iter->second.end())
            {
                iter->second.erase(prefix_iter);
                if (iter->second.empty())
                {
                    mMiscRoutes.erase(iter);
                }
            }
        }
    }

    if (!isRouteFlowCounterEnabled())
    {
        return;
    }

    if (mRoutePatternSet.empty())
    {
        return;
    }

    handleRouteRemove(vrf_id, ip_prefix);
}

void FlowCounterRouteOrch::onAddVR(sai_object_id_t vrf_id)
{
    SWSS_LOG_ENTER();
    if (!mRouteFlowCounterSupported)
    {
        return;
    }

    assert(vrf_id != gVirtualRouterId);
    auto *vrf_orch = gDirectory.get<VRFOrch*>();
    std::string vrf_name = vrf_orch->getVRFname(vrf_id);
    if (vrf_name == "")
    {
        getVnetNameByVrfId(vrf_id, vrf_name);
    }

    if (vrf_name == "")
    {
        SWSS_LOG_WARN("Failed to get VRF name for vrf id %s", sai_serialize_object_id(vrf_id).c_str());
    }

    for (auto &route_pattern : mRoutePatternSet)
    {
        if (route_pattern.vrf_name == vrf_name)
        {
            RoutePattern &existing = const_cast<RoutePattern &>(route_pattern);
            existing.vrf_id = vrf_id;
            createRouteFlowCounterByPattern(existing, 0);
            break;
        }
    }
}

void FlowCounterRouteOrch::onRemoveVR(sai_object_id_t vrf_id)
{
    SWSS_LOG_ENTER();
    if (!mRouteFlowCounterSupported)
    {
        return;
    }

    for (auto &route_pattern : mRoutePatternSet)
    {
        if (route_pattern.vrf_id == vrf_id)
        {
            SWSS_LOG_NOTICE("Removing route pattern %s and all related counters due to VRF %s has been removed", route_pattern.to_string().c_str(), route_pattern.vrf_name.c_str());
            removeRoutePattern(route_pattern);
            RoutePattern &existing = const_cast<RoutePattern &>(route_pattern);
            existing.vrf_id = SAI_NULL_OBJECT_ID;
        }
    }
}

bool FlowCounterRouteOrch::bindFlowCounter(const RoutePattern &route_pattern, sai_object_id_t vrf_id, const IpPrefix& ip_prefix)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("Binding route entry vrf=%s prefix=%s to flow counter", route_pattern.vrf_name.c_str(), ip_prefix.to_string().c_str());

    sai_object_id_t counter_oid;
    if (!FlowCounterHandler::createGenericCounter(counter_oid))
    {
        SWSS_LOG_ERROR("Failed to create generic counter");
        return false;
    }

    sai_route_entry_t route_entry;
    route_entry.switch_id = gSwitchId;
    route_entry.vr_id = route_pattern.vrf_id;
    copy(route_entry.destination, ip_prefix);
    sai_attribute_t attr;
    attr.id = SAI_ROUTE_ENTRY_ATTR_COUNTER_ID;
    attr.value.oid = counter_oid;

    auto status = sai_route_api->set_route_entry_attribute(&route_entry, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        FlowCounterHandler::removeGenericCounter(counter_oid);
        SWSS_LOG_WARN("Failed to bind route entry vrf=%s prefix=%s to flow counter", route_pattern.vrf_name.c_str(), ip_prefix.to_string().c_str());
        return false;
    }

    pendingUpdateFlexDb(route_pattern, ip_prefix, counter_oid);
    return true;
}

void FlowCounterRouteOrch::unbindFlowCounter(const RoutePattern &route_pattern, sai_object_id_t vrf_id, const IpPrefix& ip_prefix, sai_object_id_t counter_oid)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("Unbinding route entry vrf=%s prefix=%s to flow counter", route_pattern.vrf_name.c_str(), ip_prefix.to_string().c_str());

    sai_route_entry_t route_entry;
    route_entry.switch_id = gSwitchId;
    route_entry.vr_id = route_pattern.vrf_id;
    copy(route_entry.destination, ip_prefix);
    sai_attribute_t attr;
    attr.id = SAI_ROUTE_ENTRY_ATTR_COUNTER_ID;
    attr.value.oid = SAI_NULL_OBJECT_ID;

    auto status = sai_route_api->set_route_entry_attribute(&route_entry, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to unbind route entry vrf=%s prefix=%s from flow counter", route_pattern.vrf_name.c_str(), ip_prefix.to_string().c_str());
    }

    FlowCounterHandler::removeGenericCounter(counter_oid);
}

bool FlowCounterRouteOrch::removeRouteFlowCounter(const RoutePattern &route_pattern, sai_object_id_t vrf_id, const IpPrefix& ip_prefix)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("Removing route entry vrf=%s prefix=%s from flow counter", route_pattern.vrf_name.c_str(), ip_prefix.to_string().c_str());

    // Check if the entry is in mPendingAddToFlexCntr
    sai_object_id_t counter_oid = SAI_NULL_OBJECT_ID;
    auto pending_iter = mPendingAddToFlexCntr.find(route_pattern);
    if (pending_iter != mPendingAddToFlexCntr.end())
    {
        auto iter_prefix = pending_iter->second.find(ip_prefix);
        if (iter_prefix != pending_iter->second.end())
        {
            counter_oid = iter_prefix->second;
            pending_iter->second.erase(iter_prefix);
            if (pending_iter->second.empty())
            {
                mPendingAddToFlexCntr.erase(pending_iter);
            }
        }
    }

    if (counter_oid == SAI_NULL_OBJECT_ID)
    {
        // Check if the entry is in mBoundRouteCounters
        auto cache_iter = mBoundRouteCounters.find(route_pattern);
        if (cache_iter != mBoundRouteCounters.end())
        {
            auto iter_prefix = cache_iter->second.find(ip_prefix);
            if (iter_prefix != cache_iter->second.end())
            {
                counter_oid = iter_prefix->second;
                removeRouteFlowCounterFromDB(vrf_id, ip_prefix, counter_oid);
                cache_iter->second.erase(iter_prefix);
                if (cache_iter->second.empty())
                {
                    mBoundRouteCounters.erase(cache_iter);
                }
            }
        }
    }

    // No need unbind because the route entry has been removed, just remove the generic counter
    if (counter_oid != SAI_NULL_OBJECT_ID)
    {
        FlowCounterHandler::removeGenericCounter(counter_oid);
        return true;
    }

    return false;
}

void FlowCounterRouteOrch::pendingUpdateFlexDb(const RoutePattern &route_pattern, const IpPrefix& ip_prefix, sai_object_id_t counter_oid)
{
    SWSS_LOG_ENTER();
    bool was_empty = mPendingAddToFlexCntr.empty();
    updateRouterFlowCounterCache(route_pattern, ip_prefix, counter_oid, mPendingAddToFlexCntr);
    if (was_empty)
    {
        mFlexCounterUpdTimer->start();
    }
}

bool FlowCounterRouteOrch::validateRoutePattern(const RoutePattern &route_pattern) const
{
    SWSS_LOG_ENTER();

    for (const auto& existing : mRoutePatternSet)
    {
        if (existing.is_overlap_with(route_pattern))
        {
            SWSS_LOG_ERROR("Configured route pattern %s is conflict with existing one %s", route_pattern.to_string().c_str(), existing.to_string().c_str());
            return false;
        }
    }

    return true;
}

size_t FlowCounterRouteOrch::getRouteFlowCounterSizeByPattern(const RoutePattern &route_pattern) const
{
    SWSS_LOG_ENTER();

    auto cache_iter = mBoundRouteCounters.find(route_pattern);
    auto cache_count = cache_iter == mBoundRouteCounters.end() ? 0 : cache_iter->second.size();
    auto pending_iter = mPendingAddToFlexCntr.find(route_pattern);
    auto pending_count = pending_iter == mPendingAddToFlexCntr.end() ? 0 : pending_iter->second.size();
    return cache_count + pending_count;
}

bool FlowCounterRouteOrch::isRouteAlreadyBound(const RoutePattern &route_pattern, const IpPrefix &ip_prefix) const
{
    SWSS_LOG_ENTER();

    auto iter = mBoundRouteCounters.find(route_pattern);
    if (iter == mBoundRouteCounters.end())
    {
        auto pending_iter = mPendingAddToFlexCntr.find(route_pattern);
        if (pending_iter != mPendingAddToFlexCntr.end())
        {
            return pending_iter->second.find(ip_prefix) != pending_iter->second.end();
        }
        return false;
    }

    return iter->second.find(ip_prefix) != iter->second.end();
}

void FlowCounterRouteOrch::createRouteFlowCounterByPattern(const RoutePattern &route_pattern, size_t current_bound_count)
{
    SWSS_LOG_ENTER();
    if (!isRouteFlowCounterEnabled())
    {
        return;
    }

    auto &syncdRoutes = gRouteOrch->getSyncdRoutes();
    auto iter = syncdRoutes.find(route_pattern.vrf_id);
    if (iter != syncdRoutes.end())
    {
        SWSS_LOG_NOTICE("Creating route flow counter for pattern %s", route_pattern.to_string().c_str());

        for (auto &entry : iter->second)
        {
            if (current_bound_count == route_pattern.max_match_count)
            {
                return;
            }
            
            if (route_pattern.is_match(route_pattern.vrf_id, entry.first))
            {
                if (isRouteAlreadyBound(route_pattern, entry.first))
                {
                    continue;
                }

                if (bindFlowCounter(route_pattern, route_pattern.vrf_id, entry.first))
                {
                    ++current_bound_count;
                }
            }
        }
    }

    createRouteFlowCounterFromVnetRoutes(route_pattern, current_bound_count);

    auto misc_iter = mMiscRoutes.find(route_pattern.vrf_id);
    if (misc_iter != mMiscRoutes.end())
    {
        SWSS_LOG_NOTICE("Creating route flow counter for pattern %s for other type route entries", route_pattern.to_string().c_str());

        for (auto ip_prefix : misc_iter->second)
        {
            if (current_bound_count == route_pattern.max_match_count)
            {
                return;
            }

            if (route_pattern.is_match(route_pattern.vrf_id, ip_prefix))
            {
                if (isRouteAlreadyBound(route_pattern, ip_prefix))
                {
                    continue;
                }

                if (bindFlowCounter(route_pattern, route_pattern.vrf_id, ip_prefix))
                {
                    ++current_bound_count;
                }
            }
        }
    }
}

void FlowCounterRouteOrch::createRouteFlowCounterFromVnetRoutes(const RoutePattern &route_pattern, size_t& current_bound_count)
{
    SWSS_LOG_ENTER();

    auto *vnet_orch = gDirectory.get<VNetOrch*>();
    assert(vnet_orch); // VnetOrch instance is created before RouteOrch

    if (!vnet_orch->isVnetExists(route_pattern.vrf_name))
    {
        return;
    }

    SWSS_LOG_NOTICE("Creating route flow counter for pattern %s for VNET route entries", route_pattern.to_string().c_str());

    auto *vrf_obj = vnet_orch->getTypePtr<VNetVrfObject>(route_pattern.vrf_name);
    const auto &route_map = vrf_obj->getRouteMap();
    for (const auto &entry : route_map)
    {
        if (current_bound_count == route_pattern.max_match_count)
        {
            return;
        }

        if (route_pattern.is_match(route_pattern.vrf_id, entry.first))
        {
            if (isRouteAlreadyBound(route_pattern, entry.first))
            {
                continue;
            }

            if (bindFlowCounter(route_pattern, route_pattern.vrf_id, entry.first))
            {
                ++current_bound_count;
            }
        }
    }

    const auto &tunnel_routes = vrf_obj->getTunnelRoutes();
    for (const auto &entry : tunnel_routes)
    {
        if (current_bound_count == route_pattern.max_match_count)
        {
            return;
        }
        if (route_pattern.is_match(route_pattern.vrf_id, entry.first))
        {
            if (isRouteAlreadyBound(route_pattern, entry.first))
            {
                continue;
            }

            if (bindFlowCounter(route_pattern, route_pattern.vrf_id, entry.first))
            {
                ++current_bound_count;
            }
        }
    }
}

void FlowCounterRouteOrch::reapRouteFlowCounterByPattern(const RoutePattern &route_pattern, size_t current_bound_count)
{
    SWSS_LOG_ENTER();

    auto pending_iter = mPendingAddToFlexCntr.find(route_pattern);
    auto iter = mBoundRouteCounters.find(route_pattern);
    if (iter == mBoundRouteCounters.end() && pending_iter == mPendingAddToFlexCntr.end())
    {
        return;
    }

    // Remove from pending cache first
    if (pending_iter != mPendingAddToFlexCntr.end())
    {
        while(current_bound_count > route_pattern.max_match_count)
        {
            auto bound_iter = pending_iter->second.begin();
            if (bound_iter == pending_iter->second.end())
            {
                break;
            }
            unbindFlowCounter(route_pattern, route_pattern.vrf_id, bound_iter->first, bound_iter->second);
            pending_iter->second.erase(bound_iter);
            --current_bound_count;
        }
    }

    // Remove from bound cache
    if (iter != mBoundRouteCounters.end())
    {
        while(current_bound_count > route_pattern.max_match_count)
        {
            auto bound_iter = iter->second.begin();
            if (bound_iter == iter->second.end())
            {
                break;
            }

            removeRouteFlowCounterFromDB(route_pattern.vrf_id, bound_iter->first, bound_iter->second);
            unbindFlowCounter(route_pattern, route_pattern.vrf_id, bound_iter->first, bound_iter->second);
            iter->second.erase(bound_iter);
            --current_bound_count;
        }
    }
}

void FlowCounterRouteOrch::onRoutePatternMaxMatchCountChange(RoutePattern &route_pattern, size_t new_max_match_count)
{
    SWSS_LOG_ENTER();

    if (route_pattern.max_match_count != new_max_match_count)
    {
        auto old_max_match_count = route_pattern.max_match_count;
        route_pattern.max_match_count = new_max_match_count;

        if (!isRouteFlowCounterEnabled())
        {
            return;
        }

        auto current_bound_count = getRouteFlowCounterSizeByPattern(route_pattern);
        SWSS_LOG_NOTICE("Current bound route flow counter count is %zu, new limit is %zu, old limit is %zu", current_bound_count, new_max_match_count, old_max_match_count);
        if (new_max_match_count > old_max_match_count)
        {
            if (current_bound_count == old_max_match_count)
            {
                createRouteFlowCounterByPattern(route_pattern, current_bound_count);
            }
        }
        else
        {
            if (current_bound_count > new_max_match_count)
            {
                reapRouteFlowCounterByPattern(route_pattern, current_bound_count);
            }
        }
    }
}

void FlowCounterRouteOrch::getRouteFlowCounterNameMapKey(sai_object_id_t vrf_id, const IpPrefix& ip_prefix, std::string &key)
{
    SWSS_LOG_ENTER();
    std::ostringstream oss;
    if (gVirtualRouterId != vrf_id)
    {
        auto *vrf_orch = gDirectory.get<VRFOrch*>();
        auto vrf_name = vrf_orch->getVRFname(vrf_id);
        if (vrf_name == "")
        {
            getVnetNameByVrfId(vrf_id, vrf_name);
        }

        if (vrf_name != "")
        {
            oss << vrf_name;
            oss << "|";
        }
        else
        {
            // Should not happen, just in case
            SWSS_LOG_ERROR("Failed to get VRF/VNET name for vrf id %s", sai_serialize_object_id(vrf_id).c_str());
        }
    }
    oss << ip_prefix.to_string();
    key = oss.str();
}

void FlowCounterRouteOrch::handleRouteAdd(sai_object_id_t vrf_id, const IpPrefix& ip_prefix)
{
    if (!mRouteFlowCounterSupported)
    {
        return;
    }

    if (!isRouteFlowCounterEnabled())
    {
        return;
    }

    for (const auto &route_pattern : mRoutePatternSet)
    {
        if (route_pattern.is_match(vrf_id, ip_prefix))
        {
            auto current_bound_count = getRouteFlowCounterSizeByPattern(route_pattern);
            if (current_bound_count < route_pattern.max_match_count)
            {
                bindFlowCounter(route_pattern, vrf_id, ip_prefix);
            }
            break;
        }
    }
}

void FlowCounterRouteOrch::handleRouteRemove(sai_object_id_t vrf_id, const IpPrefix& ip_prefix)
{
    if (!mRouteFlowCounterSupported)
    {
        return;
    }

    if (!isRouteFlowCounterEnabled())
    {
        return;
    }
    
    for (const auto &route_pattern : mRoutePatternSet)
    {
        if (route_pattern.is_match(vrf_id, ip_prefix))
        {
            if (isRouteAlreadyBound(route_pattern, ip_prefix))
            {
                if (removeRouteFlowCounter(route_pattern, vrf_id, ip_prefix))
                {
                    auto current_bound_count = getRouteFlowCounterSizeByPattern(route_pattern);
                    if (current_bound_count == route_pattern.max_match_count - 1)
                    {
                        createRouteFlowCounterByPattern(route_pattern, current_bound_count);
                    }
                }
            }
            break;
        }
    }
}

void FlowCounterRouteOrch::removeRouteFlowCounterFromDB(sai_object_id_t vrf_id, const IpPrefix& ip_prefix, sai_object_id_t counter_oid)
{
    SWSS_LOG_ENTER();
    std::string nameMapKey;
    getRouteFlowCounterNameMapKey(vrf_id, ip_prefix, nameMapKey);
    mPrefixToPatternTable->hdel("", nameMapKey);
    mPrefixToCounterTable->hdel("", nameMapKey);
    mRouteFlowCounterMgr.clearCounterIdList(counter_oid);
}

void FlowCounterRouteOrch::updateRouterFlowCounterCache(
    const RoutePattern &route_pattern,
    const IpPrefix &ip_prefix,
    sai_object_id_t counter_oid,
    RouterFlowCounterCache &cache)
{
    SWSS_LOG_ENTER();
    auto iter = cache.find(route_pattern);
    if (iter == cache.end())
    {
        cache.emplace(route_pattern, std::map<IpPrefix, sai_object_id_t>({{ip_prefix, counter_oid}}));
    }
    else
    {
        iter->second.emplace(ip_prefix, counter_oid);
    }
}

bool FlowCounterRouteOrch::isRouteFlowCounterEnabled() const
{
    SWSS_LOG_ENTER();
    FlexCounterOrch *flexCounterOrch = gDirectory.get<FlexCounterOrch*>();
    return flexCounterOrch && flexCounterOrch->getRouteFlowCountersState();
}

bool FlowCounterRouteOrch::parseRouteKeyForRoutePattern(const std::string &key, char sep, sai_object_id_t &vrf_id, IpPrefix &ip_prefix, std::string &vrf_name)
{
    size_t found = key.find(sep);
    if (found == std::string::npos)
    {
        vrf_id = gVirtualRouterId;
        ip_prefix = IpPrefix(key);
        vrf_name = "";
    }
    else
    {
        vrf_name = key.substr(0, found);
        auto *vrf_orch = gDirectory.get<VRFOrch*>();
        if (!key.compare(0, strlen(VRF_PREFIX), VRF_PREFIX) && vrf_orch->isVRFexists(vrf_name))
        {
            vrf_id = vrf_orch->getVRFid(vrf_name);
        }
        else
        {
            if (!getVrfIdByVnetName(vrf_name, vrf_id))
            {
                SWSS_LOG_NOTICE("VRF/VNET name %s is not resolved", vrf_name.c_str());
                return false;
            }
        }

        ip_prefix = IpPrefix(key.substr(found+1));
    }

    return true;
}

bool FlowCounterRouteOrch::getVrfIdByVnetName(const std::string& vnet_name, sai_object_id_t &vrf_id)
{
    auto *vnet_orch = gDirectory.get<VNetOrch*>();
    assert(vnet_orch); // VnetOrch instance is created before RouteOrch

    return vnet_orch->getVrfIdByVnetName(vnet_name, vrf_id);
}

bool FlowCounterRouteOrch::getVnetNameByVrfId(sai_object_id_t vrf_id, std::string& vnet_name)
{
    auto *vnet_orch = gDirectory.get<VNetOrch*>();
    assert(vnet_orch); // VnetOrch instance is created before RouteOrch

    return vnet_orch->getVnetNameByVrfId(vrf_id, vnet_name);
}

