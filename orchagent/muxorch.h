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

    bool enable(bool update_rt);
    bool disable(sai_object_id_t);
    void update(NextHopKey nh, sai_object_id_t, bool = true, MuxState = MuxState::MUX_STATE_INIT);

    sai_object_id_t getNextHopId(const NextHopKey);
    MuxNeighbor getNeighbors() const { return neighbors_; };
    string getAlias() const { return alias_; };

private:
    bool removeRoutes(std::list<MuxRouteBulkContext>& bulk_ctx_list);
    bool addRoutes(std::list<MuxRouteBulkContext>& bulk_ctx_list);

    inline void updateTunnelRoute(NextHopKey, bool = true);

private:
    MuxNeighbor neighbors_;
    string alias_;
    EntityBulker<sai_route_api_t> gRouteBulker;
};

// Mux Cable object
class MuxCable
{
public:
    MuxCable(string name, IpPrefix& srv_ip4, IpPrefix& srv_ip6, IpAddress peer_ip, MuxCableType cable_type);

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
    void updateNeighbor(NextHopKey nh, bool add);
    void updateRoutes();
    sai_object_id_t getNextHopId(const NextHopKey nh)
    {
        return nbr_handler_->getNextHopId(nh);
    }

private:
    bool stateActive();
    bool stateInitActive();
    bool stateStandby();

    bool aclHandler(sai_object_id_t port, string alias, bool add = true);
    bool nbrHandler(bool enable, bool update_routes = true);

    string mux_name_;
    MuxCableType cable_type_;

    MuxState state_ = MuxState::MUX_STATE_INIT;
    MuxState prev_state_;
    bool st_chg_in_progress_ = false;
    bool st_chg_failed_ = false;

    IpPrefix srv_ip4_, srv_ip6_;
    IpAddress peer_ip4_;

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
typedef std::map<NextHopKey, std::string> NextHopTb;
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

    bool isSkipNeighbor(const IpAddress& nbr)
    {
        return (skip_neighbors_.find(nbr) != skip_neighbors_.end());
    }

    MuxCable* findMuxCableInSubnet(IpAddress);
    bool isNeighborActive(const IpAddress&, const MacAddress&, string&);
    void update(SubjectType, void *);

    void addNexthop(NextHopKey, string = "");
    void removeNexthop(NextHopKey);
    bool containsNextHop(const NextHopKey&);
    bool isMuxNexthops(const NextHopGroupKey&);
    string getNexthopMuxName(NextHopKey);
    sai_object_id_t getNextHopId(const NextHopKey&);

    sai_object_id_t createNextHopTunnel(std::string tunnelKey, IpAddress& ipAddr);
    bool removeNextHopTunnel(std::string tunnelKey, IpAddress& ipAddr);
    sai_object_id_t getNextHopTunnelId(std::string tunnelKey, IpAddress& ipAddr);
    sai_object_id_t getTunnelNextHopId();

    void updateRoute(const IpPrefix &pfx, bool add);
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

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    bool handleMuxCfg(const Request&);
    bool handlePeerSwitch(const Request&);

    void updateNeighbor(const NeighborUpdate&);
    void updateFdb(const FdbUpdate&);

    bool getMuxPort(const MacAddress&, const string&, string&);

    /***
     * Methods for managing tunnel routes for neighbor IPs not associated
     * with a specific mux cable
    ***/
    void createStandaloneTunnelRoute(IpAddress neighborIp);
    void removeStandaloneTunnelRoute(IpAddress neighborIp);

    void addSkipNeighbors(const std::set<IpAddress> &neighbors)
    {
        skip_neighbors_.insert(neighbors.begin(), neighbors.end());
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

    handler_map handler_map_;

    TunnelDecapOrch *decap_orch_;
    NeighOrch *neigh_orch_;
    FdbOrch *fdb_orch_;

    MuxCfgRequest request_;
    std::set<IpAddress> standalone_tunnel_neighbors_;
    std::set<IpAddress> skip_neighbors_;

    bool enable_cache_neigh_updates_ = false;
    std::vector<NeighborUpdate> cached_neigh_updates_;
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
