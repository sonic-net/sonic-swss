#pragma once

#include <string>
#include "dash_api/tunnel.pb.h"
#include "bulker.h"
#include "dbconnector.h"
#include "zmqorch.h"
#include "zmqserver.h"

struct DashTunnelEndpointEntry
{
    sai_object_id_t tunnel_nhop_oid;
    sai_object_id_t tunnel_member_oid;
};
struct DashTunnelEntry
{
    sai_object_id_t tunnel_oid;
    std::map<std::string, DashTunnelEndpointEntry> endpoints;
    std::string endpoint;
};

struct DashTunnelBulkContext
{
    std::deque<sai_object_id_t> tunnel_object_ids;
    std::deque<sai_status_t> tunnel_object_statuses;
    std::deque<sai_object_id_t> tunnel_member_object_ids;
    std::deque<sai_status_t> tunnel_member_object_statuses;
    std::deque<sai_object_id_t> tunnel_nhop_object_ids;
    std::deque<sai_status_t> tunnel_nhop_object_statuses;
    dash::tunnel::Tunnel metadata;

    DashTunnelBulkContext() {}
    DashTunnelBulkContext(const DashTunnelBulkContext&) = delete;
    DashTunnelBulkContext(DashTunnelBulkContext&&) = delete;

    void clear()
    {
        tunnel_object_ids.clear();
        tunnel_object_statuses.clear();
        tunnel_member_object_ids.clear();
        tunnel_member_object_statuses.clear();
        tunnel_nhop_object_ids.clear();
        tunnel_nhop_object_statuses.clear();
    }
};

class DashTunnelOrch : public ZmqOrch
{
public:
    DashTunnelOrch(
        swss::DBConnector *db,
        std::vector<std::string> &tables,
        swss::ZmqServer *zmqServer);

    sai_object_id_t getTunnelOid(const std::string& tunnel_name);

private:
    ObjectBulker<sai_dash_tunnel_api_t> tunnel_bulker_;
    ObjectBulker<sai_dash_tunnel_api_t> tunnel_member_bulker_;
    ObjectBulker<sai_dash_tunnel_api_t> tunnel_nhop_bulker_;
    std::unordered_map<std::string, DashTunnelEntry> tunnel_table_;

    void doTask(ConsumerBase &consumer);
    bool addTunnel(const std::string& tunnel_name, DashTunnelBulkContext& ctxt);
    bool addTunnelPost(const std::string& tunnel_name, DashTunnelBulkContext& ctxt);
    void addTunnelNextHops(const std::string& tunnel_name, DashTunnelBulkContext& ctxt);
    bool addTunnelNextHopsPost(const std::string& tunnel_name, DashTunnelBulkContext& ctxt, const bool tunnel_succeess);
    void addTunnelMember(const sai_object_id_t tunnel_oid, const sai_object_id_t nhop_oid, DashTunnelBulkContext& ctxt);
    bool addTunnelMemberPost(const std::string& tunnel_name, const DashTunnelBulkContext& ctxt);
    bool removeTunnel(const std::string& tunnel_name, DashTunnelBulkContext& ctxt);
    bool removeTunnelPost(const std::string& tunnel_name, const DashTunnelBulkContext& ctxt);
    bool removeTunnelNextHop(const std::string& tunnel_name, DashTunnelBulkContext& ctxt);
    bool removeTunnelNextHopPost(const std::string& tunnel_name, const DashTunnelBulkContext& ctxt);
    void removeTunnelEndpoints(const std::string& tunnel_name, DashTunnelBulkContext& ctxt);
    bool removeTunnelEndpointsPost(const std::string& tunnel_name, const DashTunnelBulkContext& ctxt);
};
