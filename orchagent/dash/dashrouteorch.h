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
#include "dashorch.h"
#include "zmqorch.h"
#include "zmqserver.h"

#include "dash_api/route.pb.h"
#include "dash_api/route_rule.pb.h"


struct OutboundRoutingEntry
{
    sai_object_id_t eni;
    swss::IpPrefix destination;
    dash::route::Route metadata;
};

struct InboundRoutingEntry
{
    sai_object_id_t eni;
    uint32_t vni;   
    swss::IpAddress sip;
    swss::IpAddress sip_mask;
    dash::route_rule::RouteRule metadata;
};

typedef std::map<std::string, OutboundRoutingEntry> RoutingTable;
typedef std::map<std::string, InboundRoutingEntry> RoutingRuleTable;

struct OutboundRoutingBulkContext
{
    std::string eni;
    swss::IpPrefix destination;
    dash::route::Route metadata;
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
    std::string eni;
    uint32_t vni;
    swss::IpAddress sip;
    swss::IpAddress sip_mask;
    dash::route_rule::RouteRule metadata;
    std::deque<sai_status_t> object_statuses;
    InboundRoutingBulkContext() {}
    InboundRoutingBulkContext(const InboundRoutingBulkContext&) = delete;
    InboundRoutingBulkContext(InboundRoutingBulkContext&&) = delete;

    void clear()
    {
        object_statuses.clear();
    }
};

class DashRouteOrch : public ZmqOrch
{
public:
    DashRouteOrch(swss::DBConnector *db, std::vector<std::string> &tables, DashOrch *dash_orch, swss::ZmqServer *zmqServer);

private:
    RoutingTable routing_entries_;
    RoutingRuleTable routing_rule_entries_;
    EntityBulker<sai_dash_outbound_routing_api_t> outbound_routing_bulker_;
    EntityBulker<sai_dash_inbound_routing_api_t> inbound_routing_bulker_;
    DashOrch *dash_orch_;

    void doTask(ConsumerBase &consumer);
    void doTaskRouteTable(ConsumerBase &consumer);
    void doTaskRouteRuleTable(ConsumerBase &consumer);
    bool addOutboundRouting(const std::string& key, OutboundRoutingBulkContext& ctxt);
    bool addOutboundRoutingPost(const std::string& key, const OutboundRoutingBulkContext& ctxt);
    bool removeOutboundRouting(const std::string& key, OutboundRoutingBulkContext& ctxt);
    bool removeOutboundRoutingPost(const std::string& key, const OutboundRoutingBulkContext& ctxt);
    bool addInboundRouting(const std::string& key, InboundRoutingBulkContext& ctxt);
    bool addInboundRoutingPost(const std::string& key, const InboundRoutingBulkContext& ctxt);
    bool removeInboundRouting(const std::string& key, InboundRoutingBulkContext& ctxt);
    bool removeInboundRoutingPost(const std::string& key, const InboundRoutingBulkContext& ctxt);
};
