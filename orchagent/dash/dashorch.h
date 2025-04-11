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
#include "zmqorch.h"
#include "zmqserver.h"
#include "flex_counter_manager.h"

#include "dash_api/appliance.pb.h"
#include "dash_api/route_type.pb.h"
#include "dash_api/eni.pb.h"
#include "dash_api/qos.pb.h"
#include "dash_api/eni_route.pb.h"

#define ENI_STAT_COUNTER_FLEX_COUNTER_GROUP "ENI_STAT_COUNTER"
#define ENI_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS 10000

struct EniEntry
{
    sai_object_id_t eni_id;
    dash::eni::Eni metadata;
};

struct ApplianceEntry
{
    sai_object_id_t appliance_id;
    dash::appliance::Appliance metadata;
};

typedef std::map<std::string, ApplianceEntry> ApplianceTable;
typedef std::map<dash::route_type::RoutingType, dash::route_type::RouteType> RoutingTypeTable;
typedef std::map<std::string, EniEntry> EniTable;
typedef std::map<std::string, dash::qos::Qos> QosTable;
typedef std::map<std::string, dash::eni_route::EniRoute> EniRouteTable;

class DashOrch : public ZmqOrch
{
public:
    DashOrch(swss::DBConnector *db, std::vector<std::string> &tables, swss::ZmqServer *zmqServer);
    const EniEntry *getEni(const std::string &eni) const;
    bool getRouteTypeActions(dash::route_type::RoutingType routing_type, dash::route_type::RouteType& route_type);
    void handleFCStatusUpdate(bool is_enabled);

private:
    ApplianceTable appliance_entries_;
    RoutingTypeTable routing_type_entries_;
    EniTable eni_entries_;
    QosTable qos_entries_;
    EniRouteTable eni_route_entries_;
    void doTask(ConsumerBase &consumer);
    void doTaskApplianceTable(ConsumerBase &consumer);
    void doTaskRoutingTypeTable(ConsumerBase &consumer);
    void doTaskEniTable(ConsumerBase &consumer);
    void doTaskQosTable(ConsumerBase &consumer);
    void doTaskEniRouteTable(ConsumerBase &consumer);
    void doTaskRouteGroupTable(ConsumerBase &consumer);
    bool addApplianceEntry(const std::string& appliance_id, const dash::appliance::Appliance &entry);
    bool removeApplianceEntry(const std::string& appliance_id);
    bool addRoutingTypeEntry(const dash::route_type::RoutingType &routing_type, const dash::route_type::RouteType &entry);
    bool removeRoutingTypeEntry(const dash::route_type::RoutingType &routing_type);
    bool addEniObject(const std::string& eni, EniEntry& entry);
    bool addEniAddrMapEntry(const std::string& eni, const EniEntry& entry);
    bool addEni(const std::string& eni, EniEntry &entry);
    bool removeEniObject(const std::string& eni);
    bool removeEniAddrMapEntry(const std::string& eni);
    bool removeEni(const std::string& eni);
    bool setEniAdminState(const std::string& eni, const EniEntry& entry);
    bool addQosEntry(const std::string& qos_name, const dash::qos::Qos &entry);
    bool removeQosEntry(const std::string& qos_name);
    bool setEniRoute(const std::string& eni, const dash::eni_route::EniRoute& entry);
    bool removeEniRoute(const std::string& eni);

private:
    std::map<sai_object_id_t, std::string> m_eni_stat_work_queue;
    FlexCounterManager m_eni_stat_manager;
    bool m_eni_fc_status = false;
    std::unordered_set<std::string> m_counter_stats;
    std::unique_ptr<swss::Table> m_eni_name_table;
    std::unique_ptr<swss::Table> m_vid_to_rid_table;
    std::shared_ptr<swss::DBConnector> m_counter_db;
    std::shared_ptr<swss::DBConnector> m_asic_db;
    swss::SelectableTimer* m_fc_update_timer = nullptr;

    void doTask(swss::SelectableTimer&);
    void addEniToFC(sai_object_id_t oid, const std::string& name);
    void removeEniFromFC(sai_object_id_t oid, const std::string& name);
    void refreshEniFCStats(bool);
};
