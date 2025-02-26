#include "dashtunnelorch.h"

extern size_t gMaxBulkSize;
extern sai_dash_tunnel_api_t* sai_dash_tunnel_api;
extern sai_object_id_t gSwitchId;

DashTunnelOrch::DashTunnelOrch(
    swss::DBConnector *db,
    std::vector<std::string> &tables,
    swss::ZmqServer *zmqServer) :
    tunnel_bulker_(sai_dash_tunnel_api, gMaxBulkSize, SAI_OBJECT_TYPE_DASH_TUNNEL),
    tunnel_member_bulker_(sai_dash_tunnel_api, gMaxBulkSize, SAI_OBJECT_TYPE_DASH_TUNNEL_MEMBER),
    tunnel_nhop_bulker_(sai_dash_tunnel_api, gMaxBulkSize, SAI_OBJECT_TYPE_DASH_TUNNEL_NEXT_HOP),
    ZmqOrch(db, tables, zmqServer)
{
    SWSS_LOG_ENTER();
}