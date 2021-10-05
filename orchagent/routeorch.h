#ifndef SWSS_ROUTEORCH_H
#define SWSS_ROUTEORCH_H

#include "orch.h"
#include "observer.h"
#include "switchorch.h"
#include "intfsorch.h"
#include "neighorch.h"
#include "vxlanorch.h"

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

typedef std::map<NextHopKey, sai_object_id_t> NextHopGroupMembers;

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
typedef std::map<IpPrefix, NextHopGroupKey> RouteTable;
/* RouteTables: vrf_id, RouteTable */
typedef std::map<sai_object_id_t, RouteTable> RouteTables;
/* LabelRouteTable: destination label, next hop address(es) */
typedef std::map<Label, NextHopGroupKey> LabelRouteTable;
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
    sai_object_id_t                     vrf_id;
    IpPrefix                            ip_prefix;
    bool                                excp_intfs_flag;

    RouteBulkContext()
        : excp_intfs_flag(false)
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
    }
};

struct LabelRouteBulkContext
{
    std::deque<sai_status_t>            object_statuses;    // Bulk statuses
    NextHopGroupKey                     tmp_next_hop;       // Temporary next hop
    NextHopGroupKey                     nhg;
    sai_object_id_t                     vrf_id;
    Label                               label;
    bool                                excp_intfs_flag;
    uint8_t                             pop_count;

    LabelRouteBulkContext()
        : excp_intfs_flag(false)
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
    RouteOrch(DBConnector *db, vector<table_name_with_pri_t> &tableNames, SwitchOrch *switchOrch, NeighOrch *neighOrch, IntfsOrch *intfsOrch, VRFOrch *vrfOrch, FgNhgOrch *fgNhgOrch);

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

private:
    SwitchOrch *m_switchOrch;
    NeighOrch *m_neighOrch;
    IntfsOrch *m_intfsOrch;
    VRFOrch *m_vrfOrch;
    FgNhgOrch *m_fgNhgOrch;

    int m_nextHopGroupCount;
    int m_maxNextHopGroupCount;
    bool m_resync;

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

    void doTask(Consumer& consumer);
    void doLabelTask(Consumer& consumer);
};

#endif /* SWSS_ROUTEORCH_H */
