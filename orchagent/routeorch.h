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
    bool                                fallback_to_default_route;

    std::string                         key;       // Key in database table
    std::string                         protocol;  // Protocol string
    bool                                is_set;    // True if set operation

    RouteBulkContext(const std::string& key, bool is_set)
        : key(key), excp_intfs_flag(false), using_temp_nhg(false), is_set(is_set),
          fallback_to_default_route(false)
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
        key.clear();
        protocol.clear();
        fallback_to_default_route = false;
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
    bool removeNextHopGroup(const NextHopGroupKey&, const bool is_default_route_nh_swap=false);

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

    void addLinkLocalRouteToMe(sai_object_id_t vrf_id, IpPrefix linklocal_prefix);
    void delLinkLocalRouteToMe(sai_object_id_t vrf_id, IpPrefix linklocal_prefix);
    std::string getLinkLocalEui64Addr(void);

    unsigned int getNhgCount() { return m_nextHopGroupCount; }
    unsigned int getMaxNhgCount() { return m_maxNextHopGroupCount; }

    void increaseNextHopGroupCount();
    void decreaseNextHopGroupCount();
    bool checkNextHopGroupCount();
    const RouteTables& getSyncdRoutes() const { return m_syncdRoutes; }

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

    std::set<IpPrefix> m_SubnetDecapTermsCreated;
    ProducerStateTable m_appTunnelDecapTermProducer;

    NextHopObserverTable m_nextHopObservers;

    EntityBulker<sai_route_api_t>           gRouteBulker;
    EntityBulker<sai_mpls_api_t>            gLabelRouteBulker;
    ObjectBulker<sai_next_hop_group_api_t>  gNextHopGroupMemberBulker;

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
    void doLabelTask(Consumer& consumer);

    const NhgBase &getNhg(const std::string& nhg_index);
    void incNhgRefCount(const std::string& nhg_index);
    void decNhgRefCount(const std::string& nhg_index);

    void publishRouteState(const RouteBulkContext& ctx, const ReturnCode& status = ReturnCode(SAI_STATUS_SUCCESS));

    bool isVipRoute(const IpPrefix &ipPrefix, const NextHopGroupKey &nextHops);
    void createVipRouteSubnetDecapTerm(const IpPrefix &ipPrefix);
    void removeVipRouteSubnetDecapTerm(const IpPrefix &ipPrefix);
    bool addDefaultRouteNexthopsInNextHopGroup(NextHopGroupEntry& original_next_hop_group, std::set<NextHopKey>& default_route_next_hop_set);
    void updateDefaultRouteSwapSet(const NextHopGroupKey default_nhg_key, std::set<NextHopKey>& active_default_route_nhops);
};

#endif /* SWSS_ROUTEORCH_H */
