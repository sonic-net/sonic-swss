#pragma once

#include "bulker.h"
#include "dbconnector.h"
#include "ipprefix.h"
#include "orch.h"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#define ROUTE_FLOW_COUNTER_FLEX_COUNTER_GROUP "ROUTE_FLOW_COUNTER"

struct RoutePattern
{
    RoutePattern(const std::string& input_vrf_name, sai_object_id_t vrf, IpPrefix prefix, size_t max_match_count)
        :vrf_name(input_vrf_name), vrf_id(vrf), ip_prefix(prefix), max_match_count(max_match_count), exact_match(prefix.isDefaultRoute())
    {
    }

    std::string                         vrf_name;
    sai_object_id_t                     vrf_id;
    IpPrefix                            ip_prefix;
    size_t                              max_match_count;
    bool                                exact_match;

    bool operator < (const RoutePattern &other) const
    {
        // We don't compare the vrf id here because:
        // 1. vrf id could be SAI_NULL_OBJECT_ID if the VRF name is not resolved, two pattern with different VRF name and vrf_id=SAI_NULL_OBJECT_ID
        //    and same prefix will be treat as same route pattern, which is not expected
        // 2. vrf name must be different
        auto vrf_name_compare = vrf_name.compare(other.vrf_name);
        if (vrf_name_compare < 0)
        {
            return true;
        }
        else if (vrf_name_compare == 0 && ip_prefix < other.ip_prefix)
        {
            return true;
        }
        else
        {
            return false;
        }
    }

    bool is_match(sai_object_id_t vrf, IpPrefix prefix) const
    {
        // No need compare VRF name here because:
        // 1. If the VRF is not resolved, the vrf_id shall be SAI_NULL_OBJECT_ID, it cannot match any input vrf_id
        // 2. If the VRF is resolved, different vrf must have different vrf id
        if (vrf_id != vrf)
        {
            return false;
        }

        if (!exact_match)
        {
            return (ip_prefix.getMaskLength() <= prefix.getMaskLength() && ip_prefix.isAddressInSubnet(prefix.getIp()));
        }
        else
        {
            return prefix == ip_prefix;
        }
    }

    bool is_overlap_with(const RoutePattern &other) const
    {
        if (this == &other)
        {
            return false;
        }

        if (vrf_name != other.vrf_name)
        {
            return false;
        }

        if (vrf_name != other.vrf_name)
        {
            return false;
        }

        return is_match(other.vrf_id, other.ip_prefix) || other.is_match(vrf_id, ip_prefix);
    }

    std::string to_string() const
    {
        std::ostringstream oss;
        oss << "RoutePattern(vrf_id=" << vrf_id << ",ip_prefix=" << ip_prefix.to_string() << ")";
        return oss.str();
    }
};



typedef std::set<RoutePattern> RoutePatternSet;
/* RoutePattern to <prefix, counter OID> */
typedef std::map<RoutePattern, std::map<IpPrefix, sai_object_id_t>> RouterFlowCounterCache;
/* IP2ME, MUX, VNET route entries */
typedef std::map<sai_object_id_t, std::set<IpPrefix>> MiscRouteEntryMap;

class FlowCounterRouteOrch : public Orch
{
public:
    FlowCounterRouteOrch(swss::DBConnector *db, const std::vector<std::string> &tableNames);
    virtual ~FlowCounterRouteOrch(void);

    bool getRouteFlowCounterSupported() const { return mRouteFlowCounterSupported; }
    void generateRouteFlowStats();
    void clearRouteFlowStats();
    void addRoutePattern(const std::string &pattern, size_t);
    void removeRoutePattern(const std::string &pattern);
    void onAddMiscRouteEntry(sai_object_id_t vrf_id, const IpPrefix& ip_prefix, bool add_to_cache = true);
    void onAddMiscRouteEntry(sai_object_id_t vrf_id, const sai_ip_prefix_t& ip_pfx, bool add_to_cache = true);
    void onRemoveMiscRouteEntry(sai_object_id_t vrf_id, const IpPrefix& ip_prefix, bool remove_from_cache = true);
    void onRemoveMiscRouteEntry(sai_object_id_t vrf_id, const sai_ip_prefix_t& ip_pfx, bool remove_from_cache = true);
    void onAddVR(sai_object_id_t vrf_id);
    void onRemoveVR(sai_object_id_t vrf_id);
    void handleRouteAdd(sai_object_id_t vrf_id, const IpPrefix& ip_prefix);
    void handleRouteRemove(sai_object_id_t vrf_id, const IpPrefix& ip_prefix);
    void processRouteFlowCounterBinding();

protected:
    void doTask(Consumer &consumer) override;
    void doTask(SelectableTimer &timer) override;

private:
    std::shared_ptr<DBConnector> mAsicDb;
    std::shared_ptr<DBConnector> mCounterDb;
    std::unique_ptr<Table> mVidToRidTable;
    std::unique_ptr<Table> mPrefixToCounterTable;
    std::unique_ptr<Table> mPrefixToPatternTable;

    bool mRouteFlowCounterSupported = false;
    /* Route pattern set, store configured route patterns */
    RoutePatternSet mRoutePatternSet;
    /* Cache for those bound route flow counters*/
    RouterFlowCounterCache mBoundRouteCounters;
    /* Cache for those route flow counters pending update to FLEX DB */
    RouterFlowCounterCache mPendingAddToFlexCntr;
    /* IP2ME, MUX */ // TODO: remove MUX support
    MiscRouteEntryMap mMiscRoutes; // Save here for route flow counter
    /* Flex counter manager for route flow counter */
    FlexCounterManager mRouteFlowCounterMgr;
    /* Timer to create flex counter and update counters DB */
    SelectableTimer *mFlexCounterUpdTimer = nullptr;

    EntityBulker<sai_route_api_t> gRouteBulker;

    void initRouteFlowCounterCapability();
    void removeRoutePattern(const RoutePattern &route_pattern);
    void removeRouteFlowCounterFromDB(sai_object_id_t vrf_id, const IpPrefix& ip_prefix, sai_object_id_t counter_oid);
    bool bindFlowCounter(const RoutePattern &route_pattern, sai_object_id_t vrf_id, const IpPrefix& ip_prefix);
    void unbindFlowCounter(const RoutePattern &route_pattern, sai_object_id_t vrf_id, const IpPrefix& ip_prefix, sai_object_id_t counter_oid);
    void pendingUpdateFlexDb(const RoutePattern &route_pattern, const IpPrefix &ip_prefix, sai_object_id_t counter_oid);
    void updateRouterFlowCounterCache(
        const RoutePattern &route_pattern,
        const IpPrefix& ip_prefix,
        sai_object_id_t counter_oid,
        RouterFlowCounterCache &cache);
    bool validateRoutePattern(const RoutePattern &route_pattern) const;
    void onRoutePatternMaxMatchCountChange(RoutePattern &route_pattern, size_t new_max_match_count);
    bool isRouteAlreadyBound(const RoutePattern &route_pattern, const IpPrefix &ip_prefix) const;
    void createRouteFlowCounterByPattern(const RoutePattern &route_pattern, size_t currentBoundCount);
    /* Return true if it actaully removed a counter so that caller need to fill the hole if possible*/
    bool removeRouteFlowCounter(const RoutePattern &route_pattern, sai_object_id_t vrf_id, const IpPrefix& ip_prefix);
    void createRouteFlowCounterFromVnetRoutes(const RoutePattern &route_pattern, size_t& current_bound_count);
    void reapRouteFlowCounterByPattern(const RoutePattern &route_pattern, size_t currentBoundCount);
    bool isRouteFlowCounterEnabled() const;
    void getRouteFlowCounterNameMapKey(sai_object_id_t vrf_id, const IpPrefix &ip_prefix, std::string &key);
    size_t getRouteFlowCounterSizeByPattern(const RoutePattern &route_pattern) const;
    bool parseRouteKeyForRoutePattern(const std::string &key, char sep, sai_object_id_t &vrf_id, IpPrefix &ip_prefix, std::string& vrf_name);
    bool getVrfIdByVnetName(const std::string& vnet_name, sai_object_id_t &vrf_id);
    bool getVnetNameByVrfId(sai_object_id_t vrf_id, std::string& vnet_name);
};
