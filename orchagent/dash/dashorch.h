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

struct ApplianceEntry
{
    swss::IpAddress sip;
    uint32_t vm_vni;
};

struct RoutingTypeEntry
{
    std::string action_name;
    std::string action_type;
    std::string encap_type;
    uint32_t vni;
};

typedef std::map<std::string, ApplianceEntry> ApplianceTable;
typedef std::map<std::string, RoutingTypeEntry> RoutingTypeTable;

class DashOrch : public Orch
{
public:
    DashOrch(swss::DBConnector *db, std::vector<std::string> &tables);

private:
    ApplianceTable appliance_entries_;
    RoutingTypeTable routing_type_entries_;
    void doTask(Consumer &consumer);
    void doTaskApplianceTable(Consumer &consumer);
    void doTaskRoutingTypeTable(Consumer &consumer);
    bool addApplianceEntry(const std::string& appliance_id, const ApplianceEntry &entry);
    bool removeApplianceEntry(const std::string& appliance_id);
    bool addRoutingTypeEntry(const std::string& routing_type, const RoutingTypeEntry &entry);
    bool removeRoutingTypeEntry(const std::string& routing_type);
};
