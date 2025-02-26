#pragma once

#include <string>
#include "dash_api/tunnel.pb.h"
#include "bulker.h"
#include "dbconnector.h"
#include "zmqorch.h"
#include "zmqserver.h"

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

private:
    ObjectBulker<sai_dash_tunnel_api_t> tunnel_bulker_;
    ObjectBulker<sai_dash_tunnel_api_t> tunnel_member_bulker_;
    ObjectBulker<sai_dash_tunnel_api_t> tunnel_nhop_bulker_;

    void doTask(ConsumerBase &consumer);
    void doTaskTunnelTable(ConsumerBase &consumer);
};