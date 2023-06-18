#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

#include <saitypes.h>

#include "bulker.h"
#include "dbconnector.h"
#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"
#include "macaddress.h"
#include "timer.h"
#include "dashorch.h"
#include "saihelper.h"

#include "proto/appliance.pb.h"
#include "proto/route_type.pb.h"
#include "proto/eni.pb.h"
#include "proto/qos.pb.h"

struct EniEntry
{
    sai_object_id_t eni_id;
    dash::eni::Eni metadata;
};

struct QosEntry
{
    std::string qos_id;
    uint32_t bw;
    uint32_t cps;
    uint32_t flows;
};

typedef std::map<std::string, dash::appliance::Appliance> ApplianceTable;
typedef std::map<std::string, dash::route_type::RouteType> RoutingTypeTable;
typedef std::map<std::string, EniEntry> EniTable;
typedef std::map<std::string, dash::qos::Qos> QosTable;

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
    bool addApplianceEntry(const std::string& appliance_id, const dash::appliance::Appliance &entry);
    bool removeApplianceEntry(const std::string& appliance_id);
    bool addRoutingTypeEntry(const std::string& routing_type, const dash::route_type::RouteType &entry);
    bool removeRoutingTypeEntry(const std::string& routing_type);
    bool addEniObject(const std::string& eni, EniEntry& entry);
    bool addEniAddrMapEntry(const std::string& eni, const EniEntry& entry);
    bool addEni(const std::string& eni, EniEntry &entry);
    bool removeEniObject(const std::string& eni);
    bool removeEniAddrMapEntry(const std::string& eni);
    bool removeEni(const std::string& eni);
    bool setEniAdminState(const std::string& eni, const EniEntry& entry);
    bool addQosEntry(const std::string& qos_name, const dash::qos::Qos &entry);
    bool removeQosEntry(const std::string& qos_name);
};
