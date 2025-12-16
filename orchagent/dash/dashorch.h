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

#define METER_STAT_COUNTER_FLEX_COUNTER_GROUP "METER_STAT_COUNTER"
#define METER_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS 10000

#define DASH_RESULT_SUCCESS 0
#define DASH_RESULT_FAILURE 1

class DashHaOrch;

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
    DashOrch(swss::DBConnector *db, std::vector<std::string> &tables, swss::DBConnector *app_state_db, swss::ZmqServer *zmqServer);
    void setDashHaOrch(DashHaOrch *dash_ha_orch);
    const EniEntry *getEni(const std::string &eni) const;
    const EniTable *getEniTable() const { return &eni_entries_; };
    bool getRouteTypeActions(dash::route_type::RoutingType routing_type, dash::route_type::RouteType& route_type);
    dash::types::IpAddress getApplianceVip();
    bool hasApplianceEntry();    

private:
    ApplianceTable appliance_entries_;
    RoutingTypeTable routing_type_entries_;
    EniTable eni_entries_;
    QosTable qos_entries_;
    EniRouteTable eni_route_entries_;
    std::unique_ptr<swss::Table> dash_eni_result_table_;
    std::unique_ptr<swss::Table> dash_qos_result_table_;
    std::unique_ptr<swss::Table> dash_appliance_result_table_;
    std::unique_ptr<swss::Table> dash_eni_route_result_table_;
    std::unique_ptr<swss::Table> dash_routing_type_result_table_;
    void doTask(ConsumerBase &consumer);
    void doTaskApplianceTable(ConsumerBase &consumer);
    void doTaskRoutingTypeTable(ConsumerBase &consumer);
    void doTaskEniTable(ConsumerBase &consumer);
    void doTaskQosTable(ConsumerBase &consumer);
    void doTaskEniRouteTable(ConsumerBase &consumer);
    void doTaskRouteGroupTable(ConsumerBase &consumer);
    bool addApplianceEntry(const std::string& appliance_id, const dash::appliance::Appliance &entry);
    void addApplianceTrustedVni(const std::string& appliance_id, const dash::appliance::Appliance& entry);
    bool removeApplianceEntry(const std::string& appliance_id);
    void removeApplianceTrustedVni(const std::string& appliance_id, const dash::appliance::Appliance& entry);
    bool addRoutingTypeEntry(const dash::route_type::RoutingType &routing_type, const dash::route_type::RouteType &entry);
    bool removeRoutingTypeEntry(const dash::route_type::RoutingType &routing_type);
    bool addEniObject(const std::string& eni, EniEntry& entry);
    bool addEniAddrMapEntry(const std::string& eni, const EniEntry& entry);
    void addEniTrustedVnis(const std::string& eni, const EniEntry& entry);
    bool addEni(const std::string& eni, EniEntry &entry);
    bool removeEniObject(const std::string& eni);
    bool removeEniAddrMapEntry(const std::string& eni);
    void removeEniTrustedVnis(const std::string& eni, const EniEntry& entry);
    bool removeEni(const std::string& eni);
    bool setEniAdminState(const std::string& eni, const EniEntry& entry);
    bool addQosEntry(const std::string& qos_name, const dash::qos::Qos &entry);
    bool removeQosEntry(const std::string& qos_name);
    bool setEniRoute(const std::string& eni, const dash::eni_route::EniRoute& entry);
    bool removeEniRoute(const std::string& eni);

private:

    template<CounterType CT>
    struct DashCounter
    {
        FlexCounterManager stat_manager;
        bool fc_status = false;
        std::unordered_set<std::string> counter_stats;

        DashCounter() {}
        DashCounter(const std::string& group_name, StatsMode stats_mode, uint polling_interval, bool enabled) 
            : stat_manager(group_name, stats_mode, polling_interval, enabled) { fetchStats(); }
        void fetchStats();
        
        void addToFC(sai_object_id_t oid, const std::string& name)
        {
            if (!fc_status)
            {
                return;
            }

            if (oid == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_WARN("Cannot add counter on NULL OID for %s", name.c_str());
                return;
            }
            stat_manager.setCounterIdList(oid, CT, counter_stats);
        }

        void removeFromFC(sai_object_id_t oid, const std::string& name)
        {
            if (oid == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_WARN("Cannot remove counter on NULL OID for %s", name.c_str());
                return;
            }
            stat_manager.clearCounterIdList(oid);
        }

        void refreshStats(bool install, const EniTable& eni_entries)
        {
            for (auto it = eni_entries.begin(); it != eni_entries.end(); it++)
            {
                if (install)
                {
                    addToFC(it->second.eni_id, it->first);
                }
                else
                {
                    removeFromFC(it->second.eni_id, it->first);
                }
            }
        }

        void handleStatusUpdate(bool enabled, const EniTable& eni_entries)
        {
            bool prev_enabled = fc_status;
            fc_status = enabled;
            if (fc_status != prev_enabled)
            {
                refreshStats(fc_status, eni_entries);
            }
        }
    };

    std::unique_ptr<swss::Table> m_eni_name_table;
    std::shared_ptr<swss::DBConnector> m_counter_db;
    std::shared_ptr<swss::DBConnector> m_asic_db;
    DashHaOrch* m_dash_ha_orch = nullptr;

    void addEniMapEntry(sai_object_id_t oid, const std::string& name);
    void removeEniMapEntry(sai_object_id_t oid, const std::string& name);
    DashCounter<CounterType::ENI> EniCounter;
    DashCounter<CounterType::DASH_METER> MeterCounter;

public:
    void handleFCStatusUpdate(bool is_enabled) { EniCounter.handleStatusUpdate(is_enabled, eni_entries_); }
    void handleMeterFCStatusUpdate(bool is_enabled) { MeterCounter.handleStatusUpdate(is_enabled, eni_entries_); }
};
