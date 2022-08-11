#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include "bulker.h"
#include "dbconnector.h"
#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"
#include "macaddress.h"
#include "timer.h"

using namespace std;
using namespace swss;

struct EniEntry
{
    sai_object_id_t eni_id;
    string mac_address;
    string qos_name;
    IpAddress underlay_ip;
    bool admin_state;
    string vnet;
};

struct Eni
{
    EniEntry eni_entry;
    bool eniEntryAdded;
    bool eniAddrMapEntryAdded;
};

struct QosEntry
{
    string qos_id;
    uint32_t bw;
    uint32_t cps;
    uint32_t flows;
};

struct OutboundRoutingEntry
{
    sai_object_id_t eni;
    IpPrefix destination;
    string action_type;
    string vnet;
    IpAddress overlay_ip;
};

struct InboundRoutingEntry
{
    sai_object_id_t eni;
    uint32_t vni;
    IpAddress sip;
    IpAddress sip_mask;
    string action_type;
    string vnet;
    bool pa_validation;
    uint32_t priority;
};

typedef std::map<string, Eni> EniTable;
typedef std::map<string, QosEntry> QosTable;
typedef std::map<string, OutboundRoutingEntry> RoutingTable;
typedef std::map<string, InboundRoutingEntry> RoutingRuleTable;

struct OutboundRoutingBulkContext
{
    string eni;
    IpPrefix destination;
    string action_type;
    string vnet;
    IpAddress overlay_ip;
    std::deque<sai_status_t> object_statuses;
    OutboundRoutingBulkContext() {}
    OutboundRoutingBulkContext(const OutboundRoutingBulkContext&) = delete;
    OutboundRoutingBulkContext(OutboundRoutingBulkContext&&) = delete;

    void clear()
    {
        object_statuses.clear();
    }
};

struct InboundRoutingBulkContext
{
    string eni;
    uint32_t vni;
    string action_type;
    string vnet;
    IpAddress sip;
    IpAddress sip_mask;
    uint32_t priority;
    bool pa_validation;
    std::deque<sai_status_t> object_statuses;
    InboundRoutingBulkContext() {}
    InboundRoutingBulkContext(const InboundRoutingBulkContext&) = delete;
    InboundRoutingBulkContext(InboundRoutingBulkContext&&) = delete;

    void clear()
    {
        object_statuses.clear();
    }
};

class DashRouteOrch : public Orch
{
public:
    DashRouteOrch(DBConnector *db, std::vector<std::string> &tables);

private:
    EniTable eni_entries_;
    QosTable qos_entries_;
    RoutingTable routing_entries_;
    RoutingRuleTable routing_rule_entries_;
    EntityBulker<sai_dash_outbound_routing_api_t> outbound_routing_bulker_;
    EntityBulker<sai_dash_inbound_routing_api_t> inbound_routing_bulker_;

    void doTaskEniTable(Consumer &consumer);
    void doTaskQosTable(Consumer &consumer);
    void doTaskRouteTable(Consumer &consumer);
    void doTaskRouteRuleTable(Consumer &consumer);
    void doTask(Consumer &consumer);
    bool addEniEntry(string eni);
    bool addEniAddrMapEntry(string eni);
    bool addEni(string eni, EniEntry &entry);
    bool removeEniEntry(string eni);
    bool removeEniAddrMapEntry(string eni);
    bool removeEni(string eni);
    bool addQosEntry(string qos_name, const QosEntry &entry);
    bool removeQosEntry(string qos_name);
    bool addOutboundRouting(string key, OutboundRoutingBulkContext& ctxt);
    bool addOutboundRoutingPost(string key, const OutboundRoutingBulkContext& ctxt);
    bool removeOutboundRouting(string key, OutboundRoutingBulkContext& ctxt);
    bool removeOutboundRoutingPost(string key, const OutboundRoutingBulkContext& ctxt);
    bool addInboundRouting(string key, InboundRoutingBulkContext& ctxt);
    bool addInboundRoutingPost(string key, const InboundRoutingBulkContext& ctxt);
    bool removeInboundRouting(string key, InboundRoutingBulkContext& ctxt);
    bool removeInboundRoutingPost(string key, const InboundRoutingBulkContext& ctxt);
};
