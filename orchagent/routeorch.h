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
#include <chrono>
#include <functional>
#include "zmqrouteorch.h"
#include "zmqrouteserver.h"
#include <unordered_map>

extern bool gRouteStateAsyncPublish;

/* Maximum next hop group number */
#define NHGRP_MAX_SIZE 128
/* Length of the Interface Id value in EUI64 format */
#define EUI64_INTF_ID_LEN 8

#define LOOPBACK_PREFIX     "Loopback"
#define VLAN_PREFIX         "Vlan"

struct NextHopGroupMemberEntry
{
    sai_object_id_t  next_hop_id; // next hop sai oid
    uint32_t         seq_id; // Sequence Id of nexthop in the group
};

typedef std::map<NextHopKey, NextHopGroupMemberEntry> NextHopGroupMembers;

struct NhgBase;

struct NextHopGroupEntry
{
    NextHopGroupEntry() :
        next_hop_group_id(SAI_NULL_OBJECT_ID),
        ref_count(0),
        nh_member_install_count(0),
        eligible_for_default_route_nh_swap(false),
        is_default_route_nh_swap(false)
    {
    }

    sai_object_id_t         next_hop_group_id;      // next hop group id
    int                     ref_count;              // reference count
    NextHopGroupMembers     nhopgroup_members;      // ids of members indexed by <ip_address, if_alias>
    NextHopGroupMembers     default_route_nhopgroup_members;      // ids of members indexed by <ip_address, if_alias>
    uint32_t                nh_member_install_count;
    bool                    eligible_for_default_route_nh_swap;
    bool                    is_default_route_nh_swap;
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

    std::string context_index;

    /*
     * When a route is using a temporary single next hop (because the desired
     * NHG could not be created), this records the original desired NHG key.
     * Used to detect NHG membership changes and allow re-randomization.
     */
    NextHopGroupKey desired_nhg_key;

    RouteNhg() = default;
    RouteNhg(const NextHopGroupKey& key, const std::string& index, const std::string &context_index = "") :
        nhg_key(key), nhg_index(index), context_index(context_index) {}

    bool operator==(const RouteNhg& rnhg)
       { return ((nhg_key == rnhg.nhg_key) && (nhg_index == rnhg.nhg_index) && (context_index == rnhg.context_index)); }
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
typedef std::unordered_map<NextHopGroupKey, NextHopGroupEntry> NextHopGroupTable;
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

/* What a staged route pins (RouteOrch::pinStagedRoute), named by the
 * RouteBulkContext field that holds its key: kNhgIndex is nhg_index (with
 * context_index), kNextHops nhg and kTempNextHop tmp_next_hop. kNone: nothing
 * is pinned. */
enum class StagedNextHop { kNone, kNhgIndex, kNextHops, kTempNextHop };

struct RouteBulkContext
{
    std::deque<sai_status_t>            object_statuses;    // Bulk statuses
    NextHopGroupKey                     tmp_next_hop;       // Temporary next hop
    NextHopGroupKey                     nhg;
    std::string                         nhg_index;
    std::string                         context_index;
    sai_object_id_t                     vrf_id;
    IpPrefix                            ip_prefix;
    bool                                excp_intfs_flag;
    // using_temp_nhg will track if the NhgOrch's owned NHG is temporary or not
    bool                                using_temp_nhg;
    bool                                fallback_to_default_route;
    std::vector<string>                 ipv;
    std::vector<string>                 alsv;
    std::vector<string>                 vni_labelv;
    std::vector<string>                 rmacv;
    bool                                vrf_group_flag;

    std::string                         key;       // Key in database table
    std::string                         protocol;  // Protocol string
    bool                                is_set;    // True if set operation

    Constraint                          retry_cst;

    /* What the route holds a reference on from staging until its status is
     * handled (RouteOrch::pinStagedRoute). */
    StagedNextHop                       staged = StagedNextHop::kNone;

    RouteBulkContext(const std::string& key, bool is_set)
        : key(key), excp_intfs_flag(false), using_temp_nhg(false), is_set(is_set),
          fallback_to_default_route(false), retry_cst(DUMMY_CONSTRAINT)
    {
    }

    // Disable any copy constructors
    RouteBulkContext(const RouteBulkContext&) = delete;
    RouteBulkContext(RouteBulkContext&&) = delete;

    /* Reset every per-attempt field for a fresh attempt at the same route.
     * key and is_set are kept: publishRouteState() reads key after parsing. */
    void clear()
    {
        object_statuses.clear();
        tmp_next_hop.clear();
        nhg.clear();
        nhg_index.clear();
        context_index.clear();
        ipv.clear();
        alsv.clear();
        vni_labelv.clear();
        rmacv.clear();
        vrf_id = SAI_NULL_OBJECT_ID;
        ip_prefix = IpPrefix();
        excp_intfs_flag = false;
        using_temp_nhg = false;
        vrf_group_flag = false;
        protocol.clear();
        fallback_to_default_route = false;
        retry_cst = DUMMY_CONSTRAINT;
        staged = StagedNextHop::kNone;
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
        nhg_index.clear();
        excp_intfs_flag = false;
        using_temp_nhg = false;
        vrf_id = SAI_NULL_OBJECT_ID;
        pop_count = 0;
    }
};

class RouteOrch : public ZmqRouteOrch, public Subject
{
public:
    RouteOrch(DBConnector *db, vector<table_name_with_pri_t> &tableNames, SwitchOrch *switchOrch, NeighOrch *neighOrch, IntfsOrch *intfsOrch, VRFOrch *vrfOrch, FgNhgOrch *fgNhgOrch, Srv6Orch *srv6Orch, swss::ZmqRouteServer *zmqServer = nullptr);

    bool hasNextHopGroup(const NextHopGroupKey&) const;
    sai_object_id_t getNextHopGroupId(const NextHopGroupKey&);

    void attach(Observer *, const IpAddress&, sai_object_id_t vrf_id = gVirtualRouterId);
    void detach(Observer *, const IpAddress&, sai_object_id_t vrf_id = gVirtualRouterId);

    void increaseNextHopRefCount(const NextHopGroupKey&);
    void decreaseNextHopRefCount(const NextHopGroupKey&);
    bool isRefCounterZero(const NextHopGroupKey&) const;

    int getNextHopGroupRefCount(const NextHopGroupKey& key) { return m_syncdNextHopGroups[key].ref_count; }
    std::set<std::pair<NextHopGroupKey, sai_object_id_t>> &getBulkNhgReducedRefCnt() { return m_bulkNhgReducedRefCnt; }
    std::set<NextHopKey> &getBulkMplsNhReducedRefCnt() { return m_bulkMplsNhReducedRefCnt; }

    bool addNextHopGroup(const NextHopGroupKey&);
    bool removeNextHopGroup(const NextHopGroupKey&, const bool is_default_route_nh_swap=false);

    /* addRoute/removeRoute stage into a route bulker. `bulker` selects
     * which one: nullptr uses RouteOrch's shared gRouteBulker (the default
     * for RouteOrch's own route tasks); VNetRouteOrch passes its private
     * bulker so a VNET flush never touches entries RouteOrch has staged.
     * The Post functions read ctx.object_statuses and are bulker-agnostic. */
    bool addRoute(RouteBulkContext& ctx, const NextHopGroupKey &nextHops,
                  EntityBulker<sai_route_api_t> *bulker = nullptr);
    bool removeRoute(RouteBulkContext& ctx,
                     EntityBulker<sai_route_api_t> *bulker = nullptr);
    /* The Post functions collect the next hop groups, SRv6 next hops and MPLS
     * next hops whose last route reference they drop, for the caller to tear
     * down. `sink` selects where: the yielding route drain passes its batch,
     * which owns them across yields; the other callers (label drain,
     * VNetRouteOrch) pass nothing and use the shared m_bulk* members, which
     * they fill and drain in one call. */
private:
    struct RouteDrainBatch;
public:
    bool addRoutePost(const RouteBulkContext& ctx, const NextHopGroupKey &nextHops,
                      RouteDrainBatch *sink = nullptr);
    bool removeRoutePost(const RouteBulkContext& ctx, RouteDrainBatch *sink = nullptr);

    void addNextHopRoute(const NextHopKey&, const RouteKey&);
    void removeNextHopRoute(const NextHopKey&, const RouteKey&);
    bool updateNextHopRoutes(const NextHopKey&, uint32_t&);
    bool getRoutesForNexthop(std::set<RouteKey>&, const NextHopKey&);
    bool swapnexthopinNextHopGroup(sai_object_id_t next_hop_group_id, sai_object_id_t default_next_hop_id);

    bool validnexthopinNextHopGroup(const NextHopKey&, uint32_t&);
    bool invalidnexthopinNextHopGroup(const NextHopKey&, uint32_t&);

    bool createRemoteVtep(sai_object_id_t, const NextHopKey&);
    bool deleteRemoteVtep(sai_object_id_t, const NextHopKey&);
    bool removeOverlayNextHops(sai_object_id_t, const NextHopGroupKey&);

    void notifyNextHopChangeObservers(sai_object_id_t, const IpPrefix&, const NextHopGroupKey&, bool);
    const NextHopGroupKey getSyncdRouteNhgKey(sai_object_id_t vrf_id, const IpPrefix& ipPrefix);
    bool createFineGrainedNextHopGroup(sai_object_id_t &next_hop_group_id, vector<sai_attribute_t> &nhg_attrs);
    bool removeFineGrainedNextHopGroup(sai_object_id_t &next_hop_group_id);
    bool isRouteExists(sai_object_id_t vrf_id, const IpPrefix& prefix);
    bool removeRoutePrefix(const IpPrefix& prefix);

    void addLinkLocalRouteToMe(sai_object_id_t vrf_id, IpPrefix linklocal_prefix);
    void delLinkLocalRouteToMe(sai_object_id_t vrf_id, IpPrefix linklocal_prefix);
    std::string getLinkLocalEui64Addr(void);
    std::string getLinkLocalEui64Addr(const MacAddress &mac);

    unsigned int getNhgCount() { return m_nextHopGroupCount; }
    unsigned int getMaxNhgCount() { return m_maxNextHopGroupCount; }

    /* While set, the yielding route drain ignores the quantum and drains to
     * completion. OrchDaemon::warmRestoreAndSyncUp() sets it for warm restore,
     * which calls doTask() a fixed number of times with no select loop to
     * resume a yielded drain. */
    void setWarmStartDrainToCompletion(bool enable) { m_warmStartDrainToCompletion = enable; }

    void increaseNextHopGroupCount();
    void decreaseNextHopGroupCount();
    bool checkNextHopGroupCount();
    const RouteTables& getSyncdRoutes() const { return m_syncdRoutes; }
    /* Point every route and label route using NhgOrch group nhg_index at
     * nhg_id. Routes staged by a drain in flight (isRouteDrainInFlight())
     * still carry the old ID and are missed, so callers wait for it. */
    bool moveNhgIndexRoutes(const std::string& nhg_index, sai_object_id_t nhg_id);
    bool isRouteDrainInFlight() const { return drainInFlight(APP_ROUTE_TABLE_NAME); }

    void flushResponses() override;

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

    std::set<NextHopKey> v4_active_default_route_nhops;
    std::set<NextHopKey> v6_active_default_route_nhops;
    shared_ptr<DBConnector> m_stateDb;
    unique_ptr<swss::Table> m_stateDefaultRouteTb;

    RouteTables m_syncdRoutes;
    LabelRouteTables m_syncdLabelRoutes;
    NextHopGroupTable m_syncdNextHopGroups;
    NextHopRouteTable m_nextHops;

    std::set<std::pair<NextHopGroupKey, sai_object_id_t>> m_bulkNhgReducedRefCnt;
    /* m_bulkNhgReducedRefCnt: nexthop, vrf_id */
    std::set<NextHopKey> m_bulkMplsNhReducedRefCnt;

    std::set<IpPrefix> m_SubnetDecapTermsCreated;
    ProducerStateTable m_appTunnelDecapTermProducer;
    std::vector<NextHopGroupKey> m_bulkSrv6NhgReducedVec;

    NextHopObserverTable m_nextHopObservers;

    EntityBulker<sai_route_api_t>           gRouteBulker;
    EntityBulker<sai_mpls_api_t>            gLabelRouteBulker;
    ObjectBulker<sai_next_hop_group_api_t>  gNextHopGroupMemberBulker;

    // Dedicated APPL_STATE_DB publisher for route state (publishAsync path).
    // Keep this distinct from Orch::m_publisher to avoid shadowing confusion.
    ResponsePublisher m_routeStatePublisher{"APPL_STATE_DB", /*buffered=*/false, gRouteStateAsyncPublish};

    void addTempRoute(RouteBulkContext& ctx, const NextHopGroupKey&);

    void addTempLabelRoute(LabelRouteBulkContext& ctx, const NextHopGroupKey&);
    bool addLabelRoute(LabelRouteBulkContext& ctx, const NextHopGroupKey&);
    bool removeLabelRoute(LabelRouteBulkContext& ctx);
    bool addLabelRoutePost(const LabelRouteBulkContext& ctx, const NextHopGroupKey &nextHops);
    bool removeLabelRoutePost(const LabelRouteBulkContext& ctx);

    void updateDefRouteState(string ip, bool add=false);

    void doTask(ConsumerBase& consumer) override;
    void doTask(ZmqRouteConsumer& consumer) override;

    /* Shared task bodies for both consumer types: ConsumerBase (SyncMap, no
     * ZMQ server) and ZmqRouteConsumer (RouteSyncMap). doRouteTaskImpl is in
     * routeorch.cpp and doLabelTaskImpl in mplsrouteorch.cpp, each explicitly
     * instantiated there for exactly these two types. */
    template <typename ConsumerT> void doRouteTaskImpl(ConsumerT& consumer);
    template <typename ConsumerT> void doLabelTaskImpl(ConsumerT& consumer);

    bool drainInFlight(const std::string &tableName) const override
    {
        return tableName == APP_ROUTE_TABLE_NAME &&
               m_zmqDrainBatch.phase != RouteDrainBatch::Phase::kIdle;
    }

    /* Bulk contexts for one drain batch, keyed by (table key, op). */
    using RouteBulkContextMap = std::map<std::pair<std::string, std::string>, RouteBulkContext>;

    /* One drain batch. toBulkTask moves the entries it stages out of m_toSync
     * into `entries`, so no iterator into m_toSync is held across a yield,
     * while retry cache promotion can still add to m_toSync. */
    struct RouteDrainBatch
    {
        /* The phase the next call resumes; kIdle means no batch in flight.
         * kStaging resumes its scan of m_toSync (staged entries have left
         * it), kFlushing resumes gRouteBulker.flush_until(), and kResponding
         * resumes at respondCursor, or in the epilogue once every status is
         * handled. */
        enum class Phase { kIdle, kStaging, kFlushing, kResponding };
        Phase phase = Phase::kIdle;

        std::deque<swss::KeyOpFieldsValuesTuple> entries;
        RouteBulkContextMap toBulk;

        /* Index of the next entry whose bulk status is to be handled. */
        size_t respondCursor = 0;

        /* Entries to merge back into m_toSync when the batch ends. */
        std::deque<swss::KeyOpFieldsValuesTuple> retained;
        /* Resync DELs, merged into m_toSync when the scan ends. */
        std::deque<swss::KeyOpFieldsValuesTuple> resyncDels;
        /* Entries the scan found not ready (resync in progress, VRF, L3 VNI
         * or next hop group index not there yet). Moving them out of m_toSync
         * lets a scan that restarts at begin() after each yield reach the
         * end; they merge back when the batch ends. */
        std::deque<swss::KeyOpFieldsValuesTuple> deferred;

        /* Teardowns the Post functions collect with this batch as sink. A
         * label or VNET drain running between quanta clears the shared
         * m_bulk* members, so the batch keeps its own. */
        std::set<std::pair<NextHopGroupKey, sai_object_id_t>> nhgReducedRefCnt;
        std::vector<NextHopGroupKey> srv6NhgReducedVec;
        std::set<NextHopKey> mplsNhReducedRefCnt;

        /* Next entries of srv6NhgReducedVec for the epilogue's VPN SID pass
         * and next hop pass. The next hop pass starts when the VPN SID pass
         * has covered every group. */
        size_t srv6VpnCursor = 0;
        size_t srv6NexthopCursor = 0;

        NextHopGroupKey v4DefaultNhgKey;
        NextHopGroupKey v6DefaultNhgKey;

        void clear()
        {
            phase = Phase::kIdle;
            entries.clear();
            toBulk.clear();
            respondCursor = 0;
            retained.clear();
            resyncDels.clear();
            deferred.clear();
            nhgReducedRefCnt.clear();
            srv6NhgReducedVec.clear();
            mplsNhReducedRefCnt.clear();
            srv6VpnCursor = 0;
            srv6NexthopCursor = 0;
            v4DefaultNhgKey = NextHopGroupKey();
            v6DefaultNhgKey = NextHopGroupKey();
        }
    };

    /* The batch of the APP_ROUTE_TABLE ZmqRouteConsumer, the only consumer
     * whose drain yields. */
    RouteDrainBatch m_zmqDrainBatch;

    /* drainRoutes(ConsumerBase&) runs whole batches without yielding (the
     * non-ZMQ consumer). drainRoutes(ZmqRouteConsumer&) runs the batch state
     * machine for one quantum and re-arms the consumer when work is left. */
    void drainRoutes(ConsumerBase& consumer);
    void drainRoutes(ZmqRouteConsumer& consumer);

    /* The phases of one batch (route programming HLD section 7.3).
     * toBulkTask scans m_toSync: entries it stages into gRouteBulker move
     * into the batch, entries that are done are erased, and entries that are
     * not ready move to batch.deferred. flushTask ships the staged operations
     * to syncd. responseHandlerTask handles each entry's SAI status through
     * addRoutePost/removeRoutePost, adds entries to retry to `retained`, and
     * runs the batch epilogue; keepDraining is false when the batch updated no
     * default route, which ends the drain for this call.
     * toBulkTask and responseHandlerTask check `expired` after each entry and
     * return false when it is true; each call handles at least one entry
     * first. The epilogue checks it after each group or next hop it removes.
     * The non-yielding drain passes kNeverExpired. */
    static const std::function<bool()> kNeverExpired;

    template <typename ConsumerT>
    bool toBulkTask(ConsumerT& consumer, RouteDrainBatch& batch,
                    const std::function<bool()>& expired);
    void flushTask();
    bool responseHandlerTask(ConsumerBaseCommon& consumer, RouteDrainBatch& batch,
                             std::deque<swss::KeyOpFieldsValuesTuple>& retained,
                             const std::function<bool()>& expired,
                             bool& keepDraining);

    /* Time quantum of the yielding drain (orchagent -Q, see
     * gRouteDrainQuantumMsec); 0 yields at every check. Tests replace
     * m_drainNow to control expiry. */
    std::chrono::milliseconds m_drainQuantum{20};
    std::function<std::chrono::steady_clock::time_point()> m_drainNow =
        []() { return std::chrono::steady_clock::now(); };

    bool m_warmStartDrainToCompletion = false;

    /* Mux updates re-point routes as neighbors change state and cannot wait
     * for an idle drain, so a box with mux drains without a quantum. Decided
     * once, on the first yielding drain after ports are ready: mux configured
     * after that is not detected. */
    bool m_muxDrainKnown = false;
    bool m_muxDrainToCompletion = false;

    /* A staged route carries its next hop's SAI ID before its status is
     * handled and the Post functions take their references, and other orchs
     * run between drain quanta. pinStagedRoute() takes a reference on what
     * was staged so no owner deletes it meanwhile; releaseStagedPin() drops
     * it before the Post functions run. */
    void pinStagedNextHop(const RouteBulkContext& ctx, bool pin);
    void pinStagedRoute(RouteBulkContext& ctx);
    void releaseStagedPin(RouteBulkContext& ctx);

    const NhgBase &getNhg(const std::string& nhg_index);

    void publishRouteState(const RouteBulkContext& ctx, const ReturnCode& status = ReturnCode(SAI_STATUS_SUCCESS));

    bool isVipRoute(const IpPrefix &ipPrefix, const NextHopGroupKey &nextHops);
    void createVipRouteSubnetDecapTerm(const IpPrefix &ipPrefix);
    void removeVipRouteSubnetDecapTerm(const IpPrefix &ipPrefix);
    bool addDefaultRouteNexthopsInNextHopGroup(NextHopGroupEntry& original_next_hop_group, std::set<NextHopKey>& default_route_next_hop_set);
    void updateDefaultRouteSwapSet(const NextHopGroupKey default_nhg_key, std::set<NextHopKey>& active_default_route_nhops);
    void incNhgRefCount(const std::string& nhg_index, const std::string &context_index = "");
    void decNhgRefCount(const std::string& nhg_index, const std::string &context_index = "");
    bool isStandbyMuxNextHop(const NextHopGroupKey &nexthops);
    void increaseRouteNextHopRefCount(const NextHopGroupKey &nexthops);
    void decreaseRouteNextHopRefCount(const NextHopGroupKey &nexthops);
};

#endif /* SWSS_ROUTEORCH_H */
