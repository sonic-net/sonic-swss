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

using namespace std;
using namespace swss;

struct VnetEntry
{
    sai_object_id_t vni;
    string guid;
};

struct VnetMapEntry
{
    sai_object_id_t dst_vnet_id;
    string routing_type;
    IpAddress dip;
    IpAddress underlay_ip;
    MacAddress mac_address;
    uint32_t metering_bucket;
    bool use_dst_vni;
};

typedef std::unordered_map<std::string, VnetEntry> DashVnetTable;
typedef std::unordered_map<std::string, VnetMapEntry> DashVnetMapTable;

struct DashVnetBulkContext
{
    string vnet_name;
    uint32_t vni;
    string guid;
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
    string vnet_name;
    IpAddress dip;
    string routing_type;
    IpAddress underlay_ip;
    MacAddress mac_address;
    uint32_t metering_bucket;
    bool use_dst_vni;
    std::deque<sai_status_t> outbound_ca_to_pa_object_statuses;
    std::deque<sai_status_t> pa_validation_object_statuses;
    VnetMapBulkContext() {}

    VnetMapBulkContext(const VnetMapBulkContext&) = delete;
    VnetMapBulkContext(VnetMapBulkContext&&) = delete;

    void clear()
    {
        outbound_ca_to_pa_object_statuses.clear();
    }
};

class DashVnetOrch : public Orch
{
public:
    DashVnetOrch(DBConnector *db, std::vector<std::string> &tables);

private:
    DashVnetTable vnet_table_;
    DashVnetMapTable vnet_map_table_;
    ObjectBulker<sai_dash_vnet_api_t> vnet_bulker_;
    EntityBulker<sai_dash_outbound_ca_to_pa_api_t> outbound_ca_to_pa_bulker_;
    EntityBulker<sai_dash_pa_validation_api_t> pa_validation_bulker_;

    void doTask(Consumer &consumer);
    void doTaskVnetTable(Consumer &consumer);
    void doTaskVnetMapTable(Consumer &consumer);
    bool addVnet(string key, DashVnetBulkContext& ctxt);
    bool addVnetPost(string key, const DashVnetBulkContext& ctxt);
    bool removeVnet(string key, DashVnetBulkContext& ctxt);
    bool removeVnetPost(string key, const DashVnetBulkContext& ctxt);
    bool addOutboundCaToPa(string key, VnetMapBulkContext& ctxt);
    bool addOutboundCaToPaPost(string key, const VnetMapBulkContext& ctxt);
    bool removeOutboundCaToPa(string key, VnetMapBulkContext& ctxt);
    bool removeOutboundCaToPaPost(string key, const VnetMapBulkContext& ctxt);
    bool addPaValidation(string key, VnetMapBulkContext& ctxt);
    bool addPaValidationPost(string key, const VnetMapBulkContext& ctxt);
    bool removePaValidation(string key, VnetMapBulkContext& ctxt);
    bool removePaValidationPost(string key, const VnetMapBulkContext& ctxt);
    bool addVnetMap(string key, VnetMapBulkContext& ctxt);
    bool addVnetMapPost(string key, const VnetMapBulkContext& ctxt);
    bool removeVnetMap(string key, VnetMapBulkContext& ctxt);
    bool removeVnetMapPost(string key, const VnetMapBulkContext& ctxt);
};
