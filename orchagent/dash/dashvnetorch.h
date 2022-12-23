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

struct VnetEntry
{
    sai_object_id_t vni;
    std::string guid;
};

struct VnetMapEntry
{
    sai_object_id_t dst_vnet_id;
    std::string routing_type;
    swss::IpAddress dip;
    swss::IpAddress underlay_ip;
    swss::MacAddress mac_address;
    uint32_t metering_bucket;
    bool use_dst_vni;
};

typedef std::unordered_map<std::string, VnetEntry> DashVnetTable;
typedef std::unordered_map<std::string, VnetMapEntry> DashVnetMapTable;

struct DashVnetBulkContext
{
    std::string vnet_name;
    uint32_t vni;
    std::string guid;
    std::deque<sai_object_id_t> object_ids;
    std::deque<sai_status_t> object_statuses;
    DashVnetBulkContext() {}

    DashVnetBulkContext(const DashVnetBulkContext&) = delete;
    DashVnetBulkContext(DashVnetBulkContext&&) = delete;

    void clear()
    {
        object_ids.clear();
        object_statuses.clear();
    }
};

struct VnetMapBulkContext
{
    std::string vnet_name;
    swss::IpAddress dip;
    std::string routing_type;
    swss::IpAddress underlay_ip;
    swss::MacAddress mac_address;
    uint32_t metering_bucket;
    bool use_dst_vni = false;
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

class DashVnetOrch : public Orch
{
public:
    DashVnetOrch(swss::DBConnector *db, std::vector<std::string> &tables);

private:
    DashVnetTable vnet_table_;
    DashVnetMapTable vnet_map_table_;
    ObjectBulker<sai_dash_vnet_api_t> vnet_bulker_;
    EntityBulker<sai_dash_outbound_ca_to_pa_api_t> outbound_ca_to_pa_bulker_;
    EntityBulker<sai_dash_pa_validation_api_t> pa_validation_bulker_;

    void doTask(Consumer &consumer);
    void doTaskVnetTable(Consumer &consumer);
    void doTaskVnetMapTable(Consumer &consumer);
    bool addVnet(const std::string& key, DashVnetBulkContext& ctxt);
    bool addVnetPost(const std::string& key, const DashVnetBulkContext& ctxt);
    bool removeVnet(const std::string& key, DashVnetBulkContext& ctxt);
    bool removeVnetPost(const std::string& key, const DashVnetBulkContext& ctxt);
    bool addOutboundCaToPa(const std::string& key, VnetMapBulkContext& ctxt);
    bool addOutboundCaToPaPost(const std::string& key, const VnetMapBulkContext& ctxt);
    bool removeOutboundCaToPa(const std::string& key, VnetMapBulkContext& ctxt);
    bool removeOutboundCaToPaPost(const std::string& key, const VnetMapBulkContext& ctxt);
    bool addPaValidation(const std::string& key, VnetMapBulkContext& ctxt);
    bool addPaValidationPost(const std::string& key, const VnetMapBulkContext& ctxt);
    bool removePaValidation(const std::string& key, VnetMapBulkContext& ctxt);
    bool removePaValidationPost(const std::string& key, const VnetMapBulkContext& ctxt);
    bool addVnetMap(const std::string& key, VnetMapBulkContext& ctxt);
    bool addVnetMapPost(const std::string& key, const VnetMapBulkContext& ctxt);
    bool removeVnetMap(const std::string& key, VnetMapBulkContext& ctxt);
    bool removeVnetMapPost(const std::string& key, const VnetMapBulkContext& ctxt);
};
