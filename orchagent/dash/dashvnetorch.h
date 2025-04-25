#pragma once

#include <map>
#include <unordered_map>
#include <set>
#include <string>
#include <memory>
#include "bulker.h"
#include "dbconnector.h"
#include "ipaddress.h"
#include "ipaddresses.h"
#include "macaddress.h"
#include "timer.h"
#include "zmqorch.h"
#include "zmqserver.h"

#include "dash_api/vnet.pb.h"
#include "dash_api/vnet_mapping.pb.h"

struct VnetEntry
{
    sai_object_id_t vni;
    dash::vnet::Vnet metadata;
    std::set<std::string> underlay_ips;
};

typedef std::unordered_map<std::string, VnetEntry> DashVnetTable;

struct DashVnetBulkContext
{
    std::string vnet_name;
    dash::vnet::Vnet metadata;
    std::deque<sai_object_id_t> object_ids;
    std::deque<sai_status_t> vnet_statuses;
    std::deque<sai_status_t> pa_validation_statuses;
    DashVnetBulkContext() {}

    DashVnetBulkContext(const DashVnetBulkContext&) = delete;
    DashVnetBulkContext(DashVnetBulkContext&&) = delete;

    void clear()
    {
        object_ids.clear();
        vnet_statuses.clear();
        pa_validation_statuses.clear();
    }
};

struct VnetMapBulkContext
{
    std::string vnet_name;
    swss::IpAddress dip;
    dash::vnet_mapping::VnetMapping metadata;
    std::deque<sai_status_t> outbound_ca_to_pa_object_statuses;
    std::deque<sai_status_t> pa_validation_object_statuses;
    VnetMapBulkContext() {}

    VnetMapBulkContext(const VnetMapBulkContext&) = delete;
    VnetMapBulkContext(VnetMapBulkContext&&) = delete;

    void clear()
    {
        outbound_ca_to_pa_object_statuses.clear();
        pa_validation_object_statuses.clear();
    }
};

class DashVnetOrch : public ZmqOrch
{
public:
    DashVnetOrch(swss::DBConnector *db, std::vector<std::string> &tables, swss::ZmqServer *zmqServer);

private:
    DashVnetTable vnet_table_;
    ObjectBulker<sai_dash_vnet_api_t> vnet_bulker_;
    EntityBulker<sai_dash_outbound_ca_to_pa_api_t> outbound_ca_to_pa_bulker_;
    EntityBulker<sai_dash_pa_validation_api_t> pa_validation_bulker_;

    void doTask(ConsumerBase &consumer);
    void doTaskVnetTable(ConsumerBase &consumer);
    void doTaskVnetMapTable(ConsumerBase &consumer);

    // The following add/remove methods will return true if the provided key should be removed from the
    // consumer (i.e. task is done and no retries are required) and false otherwise.
    // Methods which only have one possible outcome will have return type void.
    bool addVnet(const std::string& key, DashVnetBulkContext& ctxt);
    bool addVnetPost(const std::string& key, const DashVnetBulkContext& ctxt);
    bool removeVnet(const std::string& key, DashVnetBulkContext& ctxt);
    bool removeVnetPost(const std::string& key, const DashVnetBulkContext& ctxt);
    bool addOutboundCaToPa(const std::string& key, VnetMapBulkContext& ctxt);
    bool addOutboundCaToPaPost(const std::string& key, const VnetMapBulkContext& ctxt);
    void removeOutboundCaToPa(const std::string& key, VnetMapBulkContext& ctxt);
    bool removeOutboundCaToPaPost(const std::string& key, const VnetMapBulkContext& ctxt);
    void addPaValidation(const std::string& key, VnetMapBulkContext& ctxt);
    bool addPaValidationPost(const std::string& key, const VnetMapBulkContext& ctxt);
    void removePaValidation(const std::string& key, DashVnetBulkContext& ctxt);
    bool removePaValidationPost(const std::string& key, const DashVnetBulkContext& ctxt);
    bool addVnetMap(const std::string& key, VnetMapBulkContext& ctxt);
    bool addVnetMapPost(const std::string& key, const VnetMapBulkContext& ctxt);
    bool removeVnetMap(const std::string& key, VnetMapBulkContext& ctxt);
    bool removeVnetMapPost(const std::string& key, const VnetMapBulkContext& ctxt);
};
