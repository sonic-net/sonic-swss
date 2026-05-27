#pragma once

#include <map>
#include <unordered_map>
#include <set>
#include <memory>

#include "request_parser.h"
#include "portsorch.h"
#include "tunneldecaporch.h"
#include "aclorch.h"
#include "neighorch.h"
#include "routeorch.h"
#include "bulker.h"

enum MuxState
{
    MUX_STATE_INIT,
    MUX_STATE_ACTIVE,
    MUX_STATE_STANDBY,
    MUX_STATE_PENDING,
    MUX_STATE_FAILED,
};

enum MuxStateChange
{
    MUX_STATE_INIT_ACTIVE,
    MUX_STATE_INIT_STANDBY,
    MUX_STATE_ACTIVE_STANDBY,
    MUX_STATE_STANDBY_ACTIVE,
    MUX_STATE_UNKNOWN_STATE
};

enum MuxCableType
{
    ACTIVE_STANDBY,
    ACTIVE_ACTIVE
};

enum MuxNbrHandlerType
{
    NBR_HANDLER_HOST_ROUTE,
    NBR_HANDLER_PREFIX_BASED
};

struct MuxRouteBulkContext
{
    std::deque<sai_status_t>            object_statuses;            // Bulk statuses
    IpPrefix                            pfx;                        // Route prefix
    sai_object_id_t                     nh;                         // nexthop id

    MuxRouteBulkContext(IpPrefix pfx)
        : pfx(pfx)
    {
    }

    MuxRouteBulkContext(IpPrefix pfx, sai_object_id_t nh)
        : pfx(pfx), nh(nh)
    {
    }
};

extern size_t gMaxBulkSize;
extern sai_route_api_t* sai_route_api;

// Forward Declarations
class MuxOrch;
class MuxCableOrch;
class MuxStateOrch;

// Mux ACL Handler for adding/removing ACLs
class MuxAclHandler
{
public:
    MuxAclHandler(sai_object_id_t port, string alias);
    ~MuxAclHandler(void);

private:
    void createMuxAclTable(sai_object_id_t port, string strTable);
    void createMuxAclRule(shared_ptr<AclRulePacket> rule, string strTable);
    void bindAllPorts(AclTable &acl_table);

    sai_object_id_t port_ = SAI_NULL_OBJECT_ID;
    bool is_ingress_acl_ = true;
    string alias_;
};

// IP to nexthop index mapping
typedef std::map<IpAddress, sai_object_id_t> MuxNeighbor;

// Mux Neighbor Handler for adding/removing neighbors
class MuxNbrHandler
{
public:
    MuxNbrHandler() : gRouteBulker(sai_route_api, gMaxBulkSize) {};
    virtual ~MuxNbrHandler() = default;

    virtual bool enable(bool update_rt);
    virtual bool disable(sai_object_id_t);
    virtual void update(NextHopKey nh, sai_object_id_t, bool = true, MuxState = MuxState::MUX_STATE_INIT);

    virtual sai_object_id_t getNextHopId(const NextHopKey);
    MuxNeighbor getNeighbors() const { return neighbors_; };
    string getAlias() const { return alias_; };
    void clearBulkers() { gRouteBulker.clear(); };

protected:
    bool removeRoutes(std::list<MuxRouteBulkContext>& bulk_ctx_list);
    bool addRoutes(std::list<MuxRouteBulkContext>& bulk_ctx_list);
    bool setBulkRouteNH(std::list<MuxRouteBulkContext>& bulk_ctx_list);

    inline void updateTunnelRoute(NextHopKey, bool = true);

protected:
    MuxNeighbor neighbors_;
    string alias_;
    EntityBulker<sai_route_api_t> gRouteBulker;
};

// Mux Prefix-Based Neighbor Handler for adding/removing neighbors with prefix-based routing
class MuxPrefixBasedNbrHandler : public MuxNbrHandler
{
public:
    MuxPrefixBasedNbrHandler() = default;
    ~MuxPrefixBasedNbrHandler() override = default;

    bool enable(bool update_rt) override;
    bool disable(sai_object_id_t) override;
    void update(NextHopKey nh, sai_object_id_t, bool = true, MuxState = MuxState::MUX_STATE_INIT) override;
};

// Mux Cable object
class MuxCable
{
public:
    MuxCable(string name, IpPrefix& srv_ip4, IpPrefix& srv_ip6, IpAddress peer_ip, MuxCableType cable_type, MuxNbrHandlerType nbr_handler_type, IpPrefix slice_ip6 = IpPrefix("::/0"));

    bool isActive() const
    {
        return (state_ == MuxState::MUX_STATE_ACTIVE);
    }

    using handler_pair = pair<MuxStateChange, bool (MuxCable::*)()>;
    using state_machine_handlers = map<MuxStateChange, bool (MuxCable::*)()>;

    void setState(string state);
    void rollbackStateChange();
    string getState();
    bool isStateChangeInProgress() { return st_chg_in_progress_; }
    bool isStateChangeFailed() { return st_chg_failed_; }

    bool isIpInSubnet(IpAddress ip);

    // ---- subnet slicing (configuration view) -------------------------
    // hasSlicePrefix() is the per-cable flag derived from server_ipv6_subnet
    // being configured (non-zero base). isIpInSlice() answers "would this IP
    // be governed by this cable's slice prefix?" — anchor / skip-neighbor
    // policy is layered on top by MuxOrch::isInSliceSuppressed().
    bool hasSlicePrefix() const { return !slice_ip6_.getIp().isZero(); }
    const IpPrefix& getSlicePrefix() const { return slice_ip6_; }
    const IpPrefix& getServerIp6() const { return srv_ip6_; }
    bool isIpInSlice(const IpAddress& ip) const
    {
        return (hasSlicePrefix() && !ip.isV4() && slice_ip6_.isAddressInSubnet(ip));
    }

    void updateNeighbor(NextHopKey nh, bool add);
    void updateRoutes();
    void updateRoutesForNextHop(NextHopKey nh);
    sai_object_id_t getNextHopId(const NextHopKey nh)
    {
        return nbr_handler_->getNextHopId(nh);
    }

    // subnet slicing: anchor (supernet) route covering the slice prefix.
    // Installed at SAI level with the server_ipv6 anchor NH so traffic to
    // any in-slice IP forwards while per-host NHs stay SuppressedResolved.
    // Swap-in-place on ACTIVE<->STANDBY flips; withdraw on cable delete /
    // anchor NH null.
    void refreshSliceRoute();
    void withdrawSliceRoute();
    bool isSliceRouteInstalled() const { return slice_route_nh_oid_ != SAI_NULL_OBJECT_ID; }
    sai_object_id_t getSliceRouteNh() const { return slice_route_nh_oid_; }

    MuxNbrHandlerType getNbrHandlerType() const
    {
        return nbr_handler_type_;
    }

private:
    bool stateActive();
    bool stateInitActive();
    bool stateStandby();

    bool aclHandler(sai_object_id_t port, string alias, bool add = true);
    bool nbrHandler(bool enable, bool update_routes = true);

    string mux_name_;
    MuxCableType cable_type_;
    MuxNbrHandlerType nbr_handler_type_;

    MuxState state_ = MuxState::MUX_STATE_INIT;
    MuxState prev_state_;
    bool st_chg_in_progress_ = false;
    bool st_chg_failed_ = false;

    IpPrefix srv_ip4_, srv_ip6_;
    IpAddress peer_ip4_;

    // subnet slicing: optional per-cable IPv6 slice prefix sourced from
    // CONFIG_DB MUX_CABLE.server_ipv6_subnet. Default-constructed "::/0"
    // means "no slice configured" — checked via hasSlicePrefix().
    IpPrefix slice_ip6_;
    sai_object_id_t slice_route_nh_oid_ = SAI_NULL_OBJECT_ID;

    MuxOrch *mux_orch_;
    MuxCableOrch *mux_cb_orch_;
    MuxStateOrch *mux_state_orch_;

    shared_ptr<MuxAclHandler> acl_handler_ = { nullptr };
    unique_ptr<MuxNbrHandler> nbr_handler_;
    state_machine_handlers state_machine_handlers_;
};

const request_description_t mux_cfg_request_description = {
            { REQ_T_STRING },
            {
                { "state", REQ_T_STRING },
                { "server_ipv4", REQ_T_IP_PREFIX },
                { "server_ipv6", REQ_T_IP_PREFIX },
                { "address_ipv4", REQ_T_IP },
                { "soc_ipv4", REQ_T_IP_PREFIX },
                { "soc_ipv6", REQ_T_IP_PREFIX },
                { "cable_type", REQ_T_STRING },
                { "prober_type", REQ_T_STRING },
                { "neighbor_mode", REQ_T_STRING },
                { "server_ipv6_subnet", REQ_T_IP_PREFIX },
            },
            { }
};

struct NHTunnel
{
    sai_object_id_t nh_id;
    int             ref_count;
};

typedef std::unique_ptr<MuxCable> MuxCable_T;
typedef std::map<std::string, MuxCable_T> MuxCableTb;
typedef std::map<IpAddress, NHTunnel> MuxTunnelNHs;

// subnet-slicing state machine. Tracks the lifecycle of a NH known to
// MuxOrch so that route/neighbor flows can query a single source of truth
// instead of inferring from parallel boolean stores.
//
//   PlaceholderUnresolved - a route asked about this NH before any neighbor
//                           learn arrived. No SAI neighbor exists; the entry
//                           is a forward-reference so we can find it when
//                           the learn finally happens.
//   SuppressedResolved    - the neighbor is known/learned but intentionally
//                           NOT in the ASIC (slice-suppressed). SAI neighbor
//                           does not exist.
//   ProgrammedActive      - normal mux NH on the active path; SAI neighbor
//                           OID lives in MuxNbrHandler::neighbors_.
//   ProgrammedStandby     - normal mux NH on the standby path; OID is the
//                           tunnel NH.
//
// Invariants: ProgrammedActive/Standby ⇒ a real SAI OID exists in
// MuxNbrHandler::neighbors_. PlaceholderUnresolved / SuppressedResolved
// ⇒ no SAI OID. Callers MUST NOT use raw mux_nexthop_tb_ membership as
// a proxy for "has programmed OID"; use the typed query API below.
enum class MuxNhState
{
    PlaceholderUnresolved,
    SuppressedResolved,
    ProgrammedActive,
    ProgrammedStandby,
};

struct MuxNhEntry
{
    std::string port_name;
    MuxNhState  state;

    MuxNhEntry()
        : port_name(), state(MuxNhState::PlaceholderUnresolved) {}
    MuxNhEntry(const std::string& p)
        : port_name(p),
          state(p.empty() ? MuxNhState::PlaceholderUnresolved
                          : MuxNhState::ProgrammedActive) {}
    MuxNhEntry(const std::string& p, MuxNhState s)
        : port_name(p), state(s) {}
};

inline bool muxNhIsProgrammed(MuxNhState s)
{
    return s == MuxNhState::ProgrammedActive
        || s == MuxNhState::ProgrammedStandby;
}

typedef std::map<NextHopKey, MuxNhEntry> NextHopTb;
typedef std::map<IpPrefix, NextHopKey> MuxRouteTb;

class MuxCfgRequest : public Request
{
public:
    MuxCfgRequest() : Request(mux_cfg_request_description, '|') { }
};


class MuxOrch : public Orch2, public Observer, public Subject
{
public:
    MuxOrch(DBConnector *db, const std::vector<std::string> &tables, TunnelDecapOrch*, NeighOrch*, FdbOrch*);

    using handler_pair = pair<string, bool (MuxOrch::*) (const Request& )>;
    using handler_map = map<string, bool (MuxOrch::*) (const Request& )>;

    bool isMuxExists(const std::string& portName) const
    {
        return mux_cable_tb_.find(portName) != std::end(mux_cable_tb_);
    }

    MuxCable* getMuxCable(const std::string& portName)
    {
        return mux_cable_tb_.at(portName).get();
    }

    bool isSkipNeighbor(const IpAddress& nbr) const
    {
        return (skip_neighbors_.find(nbr) != skip_neighbors_.end());
    }

    // Get the port name for a skip neighbor, returns empty string if not found
    std::string getSkipNeighborPort(const IpAddress& nbr) const
    {
        auto it = skip_neighbors_.find(nbr);
        if (it != skip_neighbors_.end())
        {
            return it->second;
        }
        return "";
    }

    bool isMuxCablePrefixBased(const std::string& portName) const
    {
        if (!isMuxExists(portName))
        {
            return false;
        }
        return mux_cable_tb_.at(portName)->getNbrHandlerType() == MuxNbrHandlerType::NBR_HANDLER_PREFIX_BASED;
    }

    MuxCable* findMuxCableInSubnet(IpAddress);

    // ---- subnet slicing (lookup + decision helpers) ------------------
    // Find the cable whose slice prefix encloses `ip` (nullptr if none).
    MuxCable* findMuxCableBySlice(IpAddress);

    // True iff some cable's slice prefix encloses `ip` and `ip` is neither
    // the anchor (server_ipv6) nor a skip-neighbor (soc_ipv4 / soc_ipv6).
    // IP-only; callers needing port-affinity apply it separately.
    bool isInSliceSuppressed(const IpAddress& ip);

    // Fast gate for non-slice deployments: true iff at least one cable
    // has a slice prefix. Lets callers skip slice work at ~0 cost.
    bool isAnySliceConfigured() const { return m_slicedCableCount > 0; }

    bool isMuxPortPrefixNbr(const IpAddress&, const MacAddress&, string&);
    bool isNeighborActive(const IpAddress&, const MacAddress&, string&);
    void update(SubjectType, void *);

    void addNexthop(NextHopKey, string = "");
    void removeNexthop(NextHopKey);
    bool containsNextHop(const NextHopKey&);
    bool isMuxNexthops(const NextHopGroupKey&);
    bool hasPrefixBasedMuxNexthop(const std::set<NextHopKey>&);
    string getNexthopMuxName(NextHopKey);
    sai_object_id_t getNextHopId(const NextHopKey&);

    // ---- typed query API ---------------------------------------------
    // Prefer these over raw mux_nexthop_tb_ membership checks; they tolerate
    // PlaceholderUnresolved / SuppressedResolved entries safely.

    // True iff `nh` is tracked (any state).
    bool hasKnownMuxNextHop(const NextHopKey& nh) const;

    // True iff `nh` is currently programmed in SAI (Active or Standby).
    bool hasProgrammedMuxNextHop(const NextHopKey& nh) const;

    // SAI OID only for state ∈ {ProgrammedActive, ProgrammedStandby};
    // SAI_NULL_OBJECT_ID otherwise.
    sai_object_id_t getProgrammedNextHopId(const NextHopKey& nh);

    // Owning cable name regardless of state; empty if not tracked.
    std::string getMuxOwnerIfKnown(const NextHopKey& nh) const;

    // Current state for `nh`; PlaceholderUnresolved if not tracked.
    // Distinguish "not tracked" via hasKnownMuxNextHop().
    MuxNhState getNhState(const NextHopKey& nh) const;

    // ---- per-NH route reference tracking -----------------------------
    // Tracks routes that pin an in-slice NH as un-suppressed. The last
    // route removal drops the ref count to zero and queues the NH for
    // re-suppress via drainPendingReSuppress().
    void addRouteRefForNh(const NextHopKey& nh, const RouteKey& route);
    void removeRouteRefForNh(const NextHopKey& nh, const RouteKey& route);
    bool hasRouteRefsForNh(const NextHopKey& nh) const;
    size_t routeRefCountForNh(const NextHopKey& nh) const;

    // ---- slice route ↔ NH state hooks --------------------------------
    // RouteOrch calls these so slice NH state stays in lock-step with
    // route programming.
    //   onRouteAdd:             before bulker flush; un-suppresses the NH
    //                           so route_create finds a valid SAI OID.
    //   onRouteRemove:          drops the ref synchronously; queues the
    //                           NH for re-suppress.
    //   drainPendingReSuppress: runs after bulker flush + NHG teardown
    //                           so SAI neighbor_remove avoids OBJECT_IN_USE.
    // All three are no-ops when isAnySliceConfigured() is false.
    void onRouteAdd(const NextHopKey& nh, const RouteKey& route);
    void onRouteRemove(const NextHopKey& nh, const RouteKey& route);
    void drainPendingReSuppress();

    sai_object_id_t createNextHopTunnel(std::string tunnelKey, IpAddress& ipAddr);
    bool removeNextHopTunnel(std::string tunnelKey, IpAddress& ipAddr);
    sai_object_id_t getNextHopTunnelId(std::string tunnelKey, IpAddress& ipAddr);
    sai_object_id_t getTunnelNextHopId();

    void updateRoute(const IpPrefix &pfx);
    bool isStandaloneTunnelRouteInstalled(const IpAddress& neighborIp);

    void enableCachingNeighborUpdate()
    {
        enable_cache_neigh_updates_ = true;
    }
    void disableCachingNeighborUpdate()
    {
        enable_cache_neigh_updates_ = false;
    }
    void updateCachedNeighbors();
    bool getMuxPort(const MacAddress&, const string&, string&);

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    bool handleMuxCfg(const Request&);
    bool handlePeerSwitch(const Request&);

    void updateNeighbor(const NeighborUpdate&);
    void updateFdb(const FdbUpdate&);

    // Helper function to convert neighbor to MUX neighbor
    bool convertNeighborToMux(const NeighborEntry& neighbor_entry, const string& port_name, const string& context);


    /***
     * Methods for managing tunnel routes for neighbor IPs not associated
     * with a specific mux cable
    ***/
    void createStandaloneTunnelRoute(IpAddress neighborIp);
    void removeStandaloneTunnelRoute(IpAddress neighborIp);

    void addSkipNeighbors(const std::set<IpAddress> &neighbors, const std::string& port_name)
    {
        for (const auto& nbr : neighbors)
        {
            skip_neighbors_[nbr] = port_name;
        }
    }

    void removeSkipNeighbors(const std::set<IpAddress> &neighbors)
    {
        for (const IpAddress &neighbor : neighbors)
        {
            skip_neighbors_.erase(neighbor);
        }
    }

    IpAddress mux_peer_switch_ = 0x0;
    sai_object_id_t mux_tunnel_id_ = SAI_NULL_OBJECT_ID;

    MuxCableTb mux_cable_tb_;
    MuxTunnelNHs mux_tunnel_nh_;
    NextHopTb mux_nexthop_tb_;

    // per-NH route reference tracking. Populated/drained by the
    // RouteOrch hooks (added to a follow-up). MuxOrch uses this to
    // answer "is this NH still referenced by any route?" deterministically
    // without polling RouteOrch state. NextHopRouteTable is defined in
    // routeorch.h as map<NextHopKey, set<RouteKey>>.
    NextHopRouteTable nh_route_refs_;

    // deferred re-suppress queue. NHs are enqueued here when their last
    // route ref drops; drainPendingReSuppress() processes the queue at the
    // end of RouteOrch::doTask, after NHG teardown has released SAI refs.
    std::set<NextHopKey> m_pendingReSuppress;

    // Slice internal helpers — drive the state transitions. Owner
    // port + cable presence are validated inside; safe to call multiple
    // times.
    void unsuppressNeighbor(const NextHopKey& nh);
    void reSuppressNeighbor(const NextHopKey& nh);
    std::string findSliceOwnerPort(const IpAddress& ip) const;

    // subnet slicing fast gate: number of configured cables that have a
    // non-zero slice prefix. Maintained by handleMuxCfg on cable add/remove
    // so isAnySliceConfigured() is O(1).
    size_t m_slicedCableCount = 0;

    handler_map handler_map_;

    TunnelDecapOrch *decap_orch_;
    NeighOrch *neigh_orch_;
    FdbOrch *fdb_orch_;

    MuxCfgRequest request_;
    std::set<IpAddress> standalone_tunnel_neighbors_;
    std::map<IpAddress, std::string> skip_neighbors_;

    bool enable_cache_neigh_updates_ = false;
    std::vector<NeighborUpdate> cached_neigh_updates_;

    bool prefix_nbrs_supported_ = true;

    std::unique_ptr<Table> state_mux_cable_table_;
};

const request_description_t mux_cable_request_description = {
            { REQ_T_STRING },
            {
                { "state",  REQ_T_STRING },
            },
            { "state" }
};

class MuxCableRequest : public Request
{
public:
    MuxCableRequest() : Request(mux_cable_request_description, ':') { }
};

class MuxCableOrch : public Orch2
{
public:
    MuxCableOrch(DBConnector *db, DBConnector *sdb, const std::string& tableName);

    void updateMuxState(string portName, string muxState);
    void updateMuxMetricState(string portName, string muxState, bool start);
    void addTunnelRoute(const NextHopKey &nhKey);
    void removeTunnelRoute(const NextHopKey &nhKey);

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    unique_ptr<Table> mux_table_;
    MuxCableRequest request_;
    swss::Table mux_metric_table_;
    ProducerStateTable app_tunnel_route_table_;
};

const request_description_t mux_state_request_description = {
            { REQ_T_STRING },
            {
                { "state",  REQ_T_STRING },
                { "read_side", REQ_T_STRING },
                { "active_side", REQ_T_STRING },
            },
            { "state" }
};

class MuxStateRequest : public Request
{
public:
    MuxStateRequest() : Request(mux_state_request_description, '|') { }
};

class MuxStateOrch : public Orch2
{
public:
    MuxStateOrch(DBConnector *db, const std::string& tableName);

    void updateMuxState(string portName, string muxState);

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    swss::Table mux_state_table_;
    MuxStateRequest request_;
};
