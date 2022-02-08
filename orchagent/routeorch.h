#ifndef SWSS_ROUTEORCH_H
#define SWSS_ROUTEORCH_H

#include "orch.h"
#include "observer.h"
#include "switchorch.h"
#include "intfsorch.h"
#include "neighorch.h"
#include "vxlanorch.h"
#include "srv6orch.h"

#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"
#include "nexthopgroupkey.h"
#include "bulker.h"
#include "fgnhgorch.h"
#include <map>
#include <memory>
#include <set>
#include <sstream>

/* Maximum next hop group number */
#define NHGRP_MAX_SIZE 128
/* Length of the Interface Id value in EUI64 format */
#define EUI64_INTF_ID_LEN 8

#define LOOPBACK_PREFIX     "Loopback"
#define ROUTE_FLOW_COUNTER_FLEX_COUNTER_GROUP "ROUTE_FLOW_COUNTER"

struct NextHopGroupMemberEntry
{
    sai_object_id_t  next_hop_id; // next hop sai oid
    uint32_t         seq_id; // Sequence Id of nexthop in the group
};

typedef std::map<NextHopKey, NextHopGroupMemberEntry> NextHopGroupMembers;

struct NhgBase;

struct NextHopGroupEntry
{
    sai_object_id_t         next_hop_group_id;      // next hop group id
    int                     ref_count;              // reference count
    NextHopGroupMembers     nhopgroup_members;      // ids of members indexed by <ip_address, if_alias>
};

struct NextHopUpdate
{
    sai_object_id_t vrf_id;
    IpAddress destination;
    IpPrefix prefix;
    NextHopGroupKey nexthopGroup;
};

/*
 * Structure describing the next hop group used by a route.  As the next hop
 * groups can either be owned by RouteOrch or by NhgOrch, we have to keep track
 * of the next hop group index, as it is the one telling us which one owns it.
 */
struct RouteNhg
{
    NextHopGroupKey nhg_key;

    /*
     * Index of the next hop group used.  Filled only if referencing a
     * NhgOrch's owned next hop group.
     */
    std::string nhg_index;

    RouteNhg() = default;
    RouteNhg(const NextHopGroupKey& key, const std::string& index) :
        nhg_key(key), nhg_index(index) {}

    bool operator==(const RouteNhg& rnhg)
       { return ((nhg_key == rnhg.nhg_key) && (nhg_index == rnhg.nhg_index)); }
    bool operator!=(const RouteNhg& rnhg) { return !(*this == rnhg); }
};

struct NextHopObserverEntry;

/* Route destination key for a nexthop */
struct RouteKey
{
    sai_object_id_t vrf_id;
    IpPrefix prefix;

    bool operator < (const RouteKey& rhs) const
    {
        return (vrf_id <= rhs.vrf_id && prefix < rhs.prefix);
    }
};

/* NextHopGroupTable: NextHopGroupKey, NextHopGroupEntry */
typedef std::map<NextHopGroupKey, NextHopGroupEntry> NextHopGroupTable;
/* RouteTable: destination network, NextHopGroupKey */
typedef std::map<IpPrefix, RouteNhg> RouteTable;
/* RouteTables: vrf_id, RouteTable */
typedef std::map<sai_object_id_t, RouteTable> RouteTables;
/* LabelRouteTable: destination label, next hop address(es) */
typedef std::map<Label, RouteNhg> LabelRouteTable;
/* LabelRouteTables: vrf_id, LabelRouteTable */
typedef std::map<sai_object_id_t, LabelRouteTable> LabelRouteTables;
/* Host: vrf_id, IpAddress */
typedef std::pair<sai_object_id_t, IpAddress> Host;
/* NextHopObserverTable: Host, next hop observer entry */
typedef std::map<Host, NextHopObserverEntry> NextHopObserverTable;
/* Single Nexthop to Routemap */
typedef std::map<NextHopKey, std::set<RouteKey>> NextHopRouteTable;

struct NextHopObserverEntry
{
    RouteTable routeTable;
    list<Observer *> observers;
};

struct RouteBulkContext
{
    std::deque<sai_status_t>            object_statuses;    // Bulk statuses
    NextHopGroupKey                     tmp_next_hop;       // Temporary next hop
    NextHopGroupKey                     nhg;
    std::string                         nhg_index;
    sai_object_id_t                     vrf_id;
    IpPrefix                            ip_prefix;
    bool                                excp_intfs_flag;
    // using_temp_nhg will track if the NhgOrch's owned NHG is temporary or not
    bool                                using_temp_nhg;

    RouteBulkContext()
        : excp_intfs_flag(false), using_temp_nhg(false)
    {
    }

    // Disable any copy constructors
    RouteBulkContext(const RouteBulkContext&) = delete;
    RouteBulkContext(RouteBulkContext&&) = delete;

    void clear()
    {
        object_statuses.clear();
        tmp_next_hop.clear();
        nhg.clear();
        excp_intfs_flag = false;
        vrf_id = SAI_NULL_OBJECT_ID;
        using_temp_nhg = false;
    }
};

struct LabelRouteBulkContext
{
    std::deque<sai_status_t>            object_statuses;    // Bulk statuses
    NextHopGroupKey                     tmp_next_hop;       // Temporary next hop
    NextHopGroupKey                     nhg;
    std::string                         nhg_index;
    sai_object_id_t                     vrf_id;
    Label                               label;
    bool                                excp_intfs_flag;
    uint8_t                             pop_count;
    // using_temp_nhg will track if the NhgOrch's owned NHG is temporary or not
    bool                                using_temp_nhg;

    LabelRouteBulkContext()
        : excp_intfs_flag(false), using_temp_nhg(false)
    {
    }

    // Disable any copy constructors
    LabelRouteBulkContext(const LabelRouteBulkContext&) = delete;
    LabelRouteBulkContext(LabelRouteBulkContext&&) = delete;

    void clear()
    {
        object_statuses.clear();
        tmp_next_hop.clear();
        nhg.clear();
        excp_intfs_flag = false;
        vrf_id = SAI_NULL_OBJECT_ID;
    }
};

struct RouteFlowCounterBulkContext
{
    sai_status_t                        bulk_status;    // Bulk statuses
    IpPrefix                            ip_prefix;
    sai_object_id_t                     counter_oid;

    RouteFlowCounterBulkContext(IpPrefix prefix, sai_object_id_t oid)
        :bulk_status(SAI_STATUS_SUCCESS), ip_prefix(prefix), counter_oid(oid)
    {
    }
};

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
/* RoutePattern to prefix set */
typedef std::map<RoutePattern, std::set<IpPrefix>> RouterPatternToPrefixMap;
/* IP2ME, MUX, VNET route entries */
typedef std::map<sai_object_id_t, std::set<IpPrefix>> MiscRouteEntryMap;

class RouteOrch : public Orch, public Subject
{
public:
    RouteOrch(DBConnector *db, vector<table_name_with_pri_t> &tableNames, SwitchOrch *switchOrch, NeighOrch *neighOrch, IntfsOrch *intfsOrch, VRFOrch *vrfOrch, FgNhgOrch *fgNhgOrch, Srv6Orch *srv6Orch);

    bool hasNextHopGroup(const NextHopGroupKey&) const;
    sai_object_id_t getNextHopGroupId(const NextHopGroupKey&);

    void attach(Observer *, const IpAddress&, sai_object_id_t vrf_id = gVirtualRouterId);
    void detach(Observer *, const IpAddress&, sai_object_id_t vrf_id = gVirtualRouterId);

    void increaseNextHopRefCount(const NextHopGroupKey&);
    void decreaseNextHopRefCount(const NextHopGroupKey&);
    bool isRefCounterZero(const NextHopGroupKey&) const;

    bool addNextHopGroup(const NextHopGroupKey&);
    bool removeNextHopGroup(const NextHopGroupKey&);

    void addNextHopRoute(const NextHopKey&, const RouteKey&);
    void removeNextHopRoute(const NextHopKey&, const RouteKey&);
    bool updateNextHopRoutes(const NextHopKey&, uint32_t&);

    bool validnexthopinNextHopGroup(const NextHopKey&, uint32_t&);
    bool invalidnexthopinNextHopGroup(const NextHopKey&, uint32_t&);

    bool createRemoteVtep(sai_object_id_t, const NextHopKey&);
    bool deleteRemoteVtep(sai_object_id_t, const NextHopKey&);
    bool removeOverlayNextHops(sai_object_id_t, const NextHopGroupKey&);

    void notifyNextHopChangeObservers(sai_object_id_t, const IpPrefix&, const NextHopGroupKey&, bool);
    const NextHopGroupKey getSyncdRouteNhgKey(sai_object_id_t vrf_id, const IpPrefix& ipPrefix);
    bool createFineGrainedNextHopGroup(sai_object_id_t &next_hop_group_id, vector<sai_attribute_t> &nhg_attrs);
    bool removeFineGrainedNextHopGroup(sai_object_id_t &next_hop_group_id);

    void addLinkLocalRouteToMe(sai_object_id_t vrf_id, IpPrefix linklocal_prefix);
    void delLinkLocalRouteToMe(sai_object_id_t vrf_id, IpPrefix linklocal_prefix);
    std::string getLinkLocalEui64Addr(void);

    unsigned int getNhgCount() { return m_nextHopGroupCount; }
    unsigned int getMaxNhgCount() { return m_maxNextHopGroupCount; }

    void increaseNextHopGroupCount();
    void decreaseNextHopGroupCount();
    bool checkNextHopGroupCount();

    bool getRouteFlowCounterSupported() const { return mRouteFlowCounterSupported; }
    void generateRouteFlowStats();
    void clearRouteFlowStats();
    void addRoutePattern(const std::string &pattern, size_t);
    void removeRoutePattern(const std::string &pattern);
    void onAddMiscRouteEntry(sai_object_id_t vrf_id, const IpPrefix& ip_prefix);
    void onRemoveMiscRouteEntry(sai_object_id_t vrf_id, const IpPrefix& ip_prefix);
    void onAddVR(sai_object_id_t vrf_id);
    void onRemoveVR(sai_object_id_t vrf_id);

private:
    SwitchOrch *m_switchOrch;
    NeighOrch *m_neighOrch;
    IntfsOrch *m_intfsOrch;
    VRFOrch *m_vrfOrch;
    FgNhgOrch *m_fgNhgOrch;
    Srv6Orch *m_srv6Orch;

    unsigned int m_nextHopGroupCount;
    unsigned int m_maxNextHopGroupCount;
    bool m_resync;

    shared_ptr<DBConnector> m_stateDb;
    unique_ptr<swss::Table> m_stateDefaultRouteTb;

    RouteTables m_syncdRoutes;
    LabelRouteTables m_syncdLabelRoutes;
    NextHopGroupTable m_syncdNextHopGroups;
    NextHopRouteTable m_nextHops;

    std::set<std::pair<NextHopGroupKey, sai_object_id_t>> m_bulkNhgReducedRefCnt;
    /* m_bulkNhgReducedRefCnt: nexthop, vrf_id */

    NextHopObserverTable m_nextHopObservers;

    EntityBulker<sai_route_api_t>           gRouteBulker;
    EntityBulker<sai_mpls_api_t>            gLabelRouteBulker;
    ObjectBulker<sai_next_hop_group_api_t>  gNextHopGroupMemberBulker;

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
    /* Cache for those routes which match pattern but not bind due to max limit */
    RouterPatternToPrefixMap mUnboundRoutes;
    /* Cache for those route flow counters pending update to FLEX DB */
    RouterFlowCounterCache mPendingAddToFlexCntr;
    /* Cache for those newly added routes which match pattern */
    RouterPatternToPrefixMap mPendingBindRoutes;
    /* Cache for those newly removed routes which match pattern */
    RouterPatternToPrefixMap mPendingUnbindRoutes; // Used to cache newly added routes in doTask function
    /* IP2ME, MUX, VNET route entries */
    MiscRouteEntryMap mMiscRoutes; // Save here for route flow counter
    /* Flex counter manager for route flow counter */
    FlexCounterManager mRouteFlowCounterMgr;
    /* Timer to create flex counter and update counters DB */
    SelectableTimer *mFlexCounterUpdTimer = nullptr;

    void addTempRoute(RouteBulkContext& ctx, const NextHopGroupKey&);
    bool addRoute(RouteBulkContext& ctx, const NextHopGroupKey&);
    bool removeRoute(RouteBulkContext& ctx);
    bool addRoutePost(const RouteBulkContext& ctx, const NextHopGroupKey &nextHops);
    bool removeRoutePost(const RouteBulkContext& ctx);

    void addTempLabelRoute(LabelRouteBulkContext& ctx, const NextHopGroupKey&);
    bool addLabelRoute(LabelRouteBulkContext& ctx, const NextHopGroupKey&);
    bool removeLabelRoute(LabelRouteBulkContext& ctx);
    bool addLabelRoutePost(const LabelRouteBulkContext& ctx, const NextHopGroupKey &nextHops);
    bool removeLabelRoutePost(const LabelRouteBulkContext& ctx);

    void updateDefRouteState(string ip, bool add=false);

    void doTask(Consumer& consumer);
    void doTask(SelectableTimer &timer);
    void doLabelTask(Consumer& consumer);

    const NhgBase &getNhg(const std::string& nhg_index);
    void incNhgRefCount(const std::string& nhg_index);
    void decNhgRefCount(const std::string& nhg_index);

    void initRouteFlowCounterCapability();
    void removeRoutePattern(const RoutePattern &route_pattern);
    void removeRouteFlowCounterFromDB(sai_object_id_t vrf_id, const IpPrefix& ip_prefix, sai_object_id_t counter_oid);
    void bindFlowCounter(const RoutePattern &route_pattern, sai_object_id_t vrf_id, const IpPrefix& ip_prefix);
    /* Return true if it actaully removed a counter so that caller need to fill the hole if possible*/
    bool unbindFlowCounter(const RoutePattern &route_pattern, sai_object_id_t vrf_id, const IpPrefix& ip_prefix);
    void pendingUpdateFlexDb(const RoutePattern &route_pattern, const IpPrefix &ip_prefix, sai_object_id_t counter_oid);
    bool bulkBindFlowCounters(
        sai_object_id_t vrf_id,
        const IpPrefix &ip_prefix,
        std::list<RouteFlowCounterBulkContext> &to_bind,
        size_t &bound_count);
    void bulkUnbindFlowCounters(
        sai_object_id_t vrf_id,
        sai_object_id_t counter_oid,
        const IpPrefix &ip_prefix,
        std::list<RouteFlowCounterBulkContext> &to_unbind);
    void flushBindFlowCounters(const RoutePattern &route_pattern, std::list<RouteFlowCounterBulkContext> &to_bind, size_t &bound_count);
    void flushUnbindFlowCounters(std::list<RouteFlowCounterBulkContext> &to_unbind);
    void cacheRouteForFlowCounter(sai_object_id_t vrf_id, const IpPrefix& ip_prefix, RouterPatternToPrefixMap &cache);
    void processRouteFlowCounterBinding();
    void insertOrUpdateRouterFlowCounterCache(
        const RoutePattern &route_pattern,
        const IpPrefix& ip_prefix,
        sai_object_id_t counter_oid,
        RouterFlowCounterCache &cache);
    bool validateRoutePattern(const RoutePattern &route_pattern) const;
    void onRoutePatternMaxMatchCountChange(RoutePattern &route_pattern, size_t new_max_match_count);
    void createRouteFlowCounterByPattern(const RoutePattern &route_pattern, size_t currentBoundCount);
    void createSingleRouteFlowCounterByPattern(
        const RoutePattern &route_pattern,
        const IpPrefix &ip_prefix,
        std::list<RouteFlowCounterBulkContext> &to_bind,
        size_t &current_bound_count);
    void createRouteFlowCounterFromUnboundCacheByPattern(const RoutePattern &route_pattern, size_t currentBoundCount);
    void removeRouteFlowCounterFromBoundCacheByPattern(const RoutePattern &route_pattern, size_t currentBoundCount);
    bool isRouteFlowCounterEnabled() const;
    void getRouteFlowCounterNameMapKey(sai_object_id_t vrf_id, const IpPrefix &ip_prefix, std::string &key);
    void addUnboundRoutesToCache(const RoutePattern &route_pattern, const IpPrefix &ip_prefix);
    size_t getRouteFlowCounterSizeByPattern(const RoutePattern &route_pattern) const;
    bool parseRouteKey(const std::string &key, char sep, sai_object_id_t &vrf_id, IpPrefix &ip_prefix);
    bool parseRouteKeyForRoutePattern(const std::string &key, char sep, sai_object_id_t &vrf_id, IpPrefix &ip_prefix, std::string& vrf_name);
    bool getVrfIdByVnetName(const std::string& vnet_name, sai_object_id_t &vrf_id);
    bool getVnetNameByVrfId(sai_object_id_t vrf_id, std::string& vnet_name);
};

#endif /* SWSS_ROUTEORCH_H */
