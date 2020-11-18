#pragma once

#include <map>
#include <unordered_map>
#include <set>
#include <memory>

#include "request_parser.h"
#include "portsorch.h"
#include "tunneldecaporch.h"
#include "aclorch.h"

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

// Forward Declarations
class MuxOrch;
class MuxCableOrch;
class MuxStateOrch;

// Mux ACL Handler for adding/removing ACLs
class MuxAclHandler
{
    public:
        MuxAclHandler(sai_object_id_t port);
        ~MuxAclHandler(void);

    private:
        void createMuxAclTable(sai_object_id_t port, string strTable);
        void createMuxAclRule(shared_ptr<AclRuleMux> rule, string strTable);

        // class shared dict: ACL table name -> ACL table
        static std::map<std::string, AclTable> acl_table_;
        sai_object_id_t port_ = SAI_NULL_OBJECT_ID;
};

// Mux Neighbor Handler for adding/removing neigbhors
class MuxNbrHandler
{
    public:
        MuxNbrHandler() = default;

        bool enable();
        bool disable();
        void update(IpAddress, string alias = "", bool = true);

    private:
        IpAddresses neighbors_;
        string alias_;
};

// Mux Cable object
class MuxCable
{
public:
    MuxCable(string name, IpPrefix& srv_ip4, IpPrefix& srv_ip6, IpAddress peer_ip);

    bool isActive() const
    {
        return (state_ == MuxState::MUX_STATE_ACTIVE);
    }

    typedef pair<MuxStateChange, bool (MuxCable::*)()> handler_pair;
    typedef map<MuxStateChange, bool (MuxCable::*)()> state_machine_handlers;

    void setState(string state);
    string getState();
    bool isStateChangeInProgress() { return st_chg_in_progress_; }

    bool isIpInSubnet(IpAddress ip);
    void updateNeighbor(IpAddress ip, string alias, bool add)
    {
        nbr_handler_->update(ip, alias, add);
    }

private:
    bool stateActive();
    bool stateInitActive();
    bool stateStandby();

    bool aclHandler(sai_object_id_t, bool = true);
    bool nbrHandler(bool = true);

    string mux_name_;

    MuxState state_ = MuxState::MUX_STATE_INIT;
    bool st_chg_in_progress_ = false;

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
                { "server_ipv4", REQ_T_IP_PREFIX },
                { "server_ipv6", REQ_T_IP_PREFIX },
                { "address_ipv4", REQ_T_IP },
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

class MuxCfgRequest : public Request
{
public:
    MuxCfgRequest() : Request(mux_cfg_request_description, '|') { }
};


class MuxOrch : public Orch2, public Observer, public Subject
{
public:
    MuxOrch(DBConnector *db, const std::vector<std::string> &tables, TunnelDecapOrch*, NeighOrch*);

    typedef pair<string, bool (MuxOrch::*) (const Request& )> handler_pair;
    typedef map<string, bool (MuxOrch::*) (const Request& )> handler_map;

    bool isMuxExists(const std::string& portName) const
    {
        return mux_cable_tb_.find(portName) != std::end(mux_cable_tb_);
    }

    MuxCable* getMuxCable(const std::string& portName)
    {
        return mux_cable_tb_.at(portName).get();
    }

    MuxCable* findMuxCableInSubnet(IpAddress);
    bool isNeighborActive(IpAddress nbr, string alias);
    void update(SubjectType, void *);
    void updateNeighbor(const NeighborUpdate&);

    sai_object_id_t createNextHopTunnel(std::string tunnelKey, IpAddress& ipAddr);
    bool removeNextHopTunnel(std::string tunnelKey, IpAddress& ipAddr);

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    bool handleMuxCfg(const Request&);
    bool handlePeerSwitch(const Request&);

    IpAddress mux_peer_switch_ = 0x0;
    sai_object_id_t mux_tunnel_id_;

    MuxCableTb mux_cable_tb_;
    MuxTunnelNHs mux_tunnel_nh_;

    handler_map handler_map_;

    TunnelDecapOrch *decap_orch_;
    NeighOrch *neigh_orch_;

    MuxCfgRequest request_;
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
    MuxCableOrch(DBConnector *db, const std::string& tableName);

    void updateMuxState(string portName, string muxState);

private:
    virtual bool addOperation(const Request& request);
    virtual bool delOperation(const Request& request);

    unique_ptr<Table> mux_table_;
    MuxCableRequest request_;
};

const request_description_t mux_state_request_description = {
            { REQ_T_STRING },
            {
                { "state",  REQ_T_STRING },
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
