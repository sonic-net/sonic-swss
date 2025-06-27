#pragma once

#include "dbconnector.h"
#include "zmqorch.h"
#include "dash_api/outbound_port_map.pb.h"
#include "bulker.h"

struct DashPortMapBulkContext
{
    std::deque<sai_object_id_t> port_map_oids;
    std::deque<sai_status_t> port_map_statuses;
    std::deque<sai_status_t> port_map_range_statuses;

    DashPortMapBulkContext() {}
    DashPortMapBulkContext(const DashPortMapBulkContext&) = delete;
    DashPortMapBulkContext(DashPortMapBulkContext&) = delete;

    void clear()
    {
        port_map_oids.clear();
        port_map_statuses.clear();
        port_map_range_statuses.clear();
    }
};

class DashPortMapOrch : public ZmqOrch
{
public:
    DashPortMapOrch(swss::DBConnector *db, std::vector<std::string> &tables, swss::DBConnector *app_state_db, swss::ZmqServer *zmqServer);
    
private:
    void doTask(ConsumerBase &consumer);
    void doTaskPortMapTable(ConsumerBase &consumer);
    void doTaskPortMapRangeTable(ConsumerBase &consumer);
    bool addPortMap(const std::string &port_map_id, DashPortMapBulkContext &ctxt);
    bool addPortMapPost(const std::string &port_map_id, DashPortMapBulkContext &ctxt);
    bool removePortMap(const std::string &port_map_id, DashPortMapBulkContext &ctxt);
    bool removePortMapPost(const std::string &port_map_id, DashPortMapBulkContext &ctxt);
    
    ObjectBulker<sai_dash_outbound_port_map_api_t> port_map_bulker_;
    EntityBulker<sai_dash_outbound_port_map_api_t> port_map_range_bulker_;

    std::unordered_map<std::string, sai_object_id_t> port_map_table_;
    std::unique_ptr<swss::Table> dash_port_map_result_table_;
    std::unique_ptr<swss::Table> dash_port_map_range_result_table_;
};
