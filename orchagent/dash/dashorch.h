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

struct ApplianceEntry
{
    IpAddress sip;
    uint32_t vm_vni;
};

struct RoutingTypeEntry
{
    string action_name;
    string action_type;
    string encap_type;
    uint32_t vni;
};

typedef std::map<string, ApplianceEntry> ApplianceTable;
typedef std::map<string, RoutingTypeEntry> RoutingTypeTable;

class DashOrch : public Orch
{
public:
    DashOrch(DBConnector *db, std::vector<std::string> &tables);

private:
    ApplianceTable appliance_entries_;
    RoutingTypeTable routing_type_entries_;
    void doTask(Consumer &consumer);
    void doTaskApplianceTable(Consumer &consumer);
    void doTaskRoutingTypeTable(Consumer &consumer);
    bool addApplianceEntry(const string& appliance_id, const ApplianceEntry &entry);
    bool removeApplianceEntry(const string& appliance_id);
    bool addRoutingTypeEntry(const string& routing_type, const RoutingTypeEntry &entry);
    bool removeRoutingTypeEntry(const string& routing_type);
};
