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

struct EniEntry
{
    sai_object_id_t eni_id;
    std::string mac_address;
    std::string qos_name;
    swss::IpAddress underlay_ip;
    bool admin_state;
    std::string vnet;
};

struct QosEntry
{
    std::string qos_id;
    uint32_t bw;
    uint32_t cps;
    uint32_t flows;
};

typedef std::map<std::string, ApplianceEntry> ApplianceTable;
typedef std::map<std::string, RoutingTypeEntry> RoutingTypeTable;
typedef std::map<std::string, EniEntry> EniTable;
typedef std::map<std::string, QosEntry> QosTable;

class DashOrch : public Orch
{
public:
    DashOrch(swss::DBConnector *db, std::vector<std::string> &tables);
    const EniEntry *getEni(const std::string &eni) const;

private:
    ApplianceTable appliance_entries_;
    RoutingTypeTable routing_type_entries_;
    EniTable eni_entries_;
    QosTable qos_entries_;
    void doTask(Consumer &consumer);
    void doTaskApplianceTable(Consumer &consumer);
    void doTaskRoutingTypeTable(Consumer &consumer);
    void doTaskEniTable(Consumer &consumer);
    void doTaskQosTable(Consumer &consumer);
    bool addApplianceEntry(const std::string& appliance_id, const ApplianceEntry &entry);
    bool removeApplianceEntry(const std::string& appliance_id);
    bool addRoutingTypeEntry(const std::string& routing_type, const RoutingTypeEntry &entry);
    bool removeRoutingTypeEntry(const std::string& routing_type);
    bool addEniObject(const std::string& eni, EniEntry& entry);
    bool addEniAddrMapEntry(const std::string& eni, const EniEntry& entry);
    bool addEni(const std::string& eni, EniEntry &entry);
    bool removeEniObject(const std::string& eni);
    bool removeEniAddrMapEntry(const std::string& eni);
    bool removeEni(const std::string& eni);
    bool addQosEntry(const std::string& qos_name, const QosEntry &entry);
    bool removeQosEntry(const std::string& qos_name);
};
