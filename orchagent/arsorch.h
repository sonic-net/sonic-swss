#ifndef SWSS_ARSORCH_H
#define SWSS_ARSORCH_H

#include "orch.h"
#include "observer.h"
#include "intfsorch.h"
#include "neighorch.h"
#include "producerstatetable.h"

#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"
#include "nexthopgroupkey.h"

#include <map>

#define ARS_NEXTHOP_GROUP_FLEX_COUNTER_GROUP    "ARS_NEXTHOP_GROUP_FLEX_STAT_COUNTER"
#define ARS_LAG_FLEX_COUNTER_GROUP              "ARS_LAG_FLEX_STAT_COUNTER"
#define ARS_STAT_FLEX_COUNTER_POLLING_INTERVAL_MS 10000

enum ArsAssignMode
{
    PER_FLOWLET_QUALITY,
    PER_PACKET
};

typedef struct ArsObjectEntry
{
    std::string profile_name;                       // Name of ARS profile this object belong to
    ArsAssignMode assign_mode;                      // Stores an assign_mode from ArsAssignModes
    uint32_t flowlet_idle_time;                     // Flowlet idle time in micro seconds
    uint32_t max_flows;                             // Maximum number of flows in a flowlet
    struct
    {
        uint32_t primary_threshold;                 // Primary path threshold
    } quality_threshold;
    std::set<NextHopGroupKey> nexthops;             // NHG that are configured to hw
    std::set<string> lags;                          // ARS-enabled LAGs
    sai_object_id_t ars_object_id;                  // ARS Object ID if already created
} ArsObjectEntry;

typedef enum
{
    ARS_ALGORITHM_EWMA
} ArsAlgorithm;

typedef struct ArsProfileEntry
{
    string profile_name;                                    // Name of ARS profile configured by user
    ArsAlgorithm algorithm;                                 // ARS algorithm
    uint32_t max_flows;                                     // Maximum number of supported flows
    uint32_t sampling_interval;                             // Sampling interval in micro seconds
    struct
    {
        struct {
            uint32_t min_value;                             // Minimum value of the load
            uint32_t max_value;                             // Maximum value of the load
            uint32_t weight;                                // Weight of the metric
        } past_load;

        struct {
            uint32_t min_value;                             // Minimum value of the load
            uint32_t max_value;                             // Maximum value of the load
        } current_load;

        struct {
            uint32_t min_value;                             // Minimum value of the load
            uint32_t max_value;                             // Maximum value of the load
            uint32_t weight;                                // Weight of the metric
        } future_load;
    } path_metrics;

    bool ipv4_enabled;                                      // Enable IPv4
    bool ipv6_enabled;                                      // Enable IPv6
    uint32_t ref_count;                                     // Number of objects using this profile
    sai_object_id_t m_sai_ars_id;                           // SAI ARS profile OID
} ArsProfileEntry;

/* Map from IP prefix to ARS object */
typedef std::map<IpPrefix, ArsObjectEntry> ArsNexthopGroupPrefixes; 
typedef std::map<std::string, ArsNexthopGroupPrefixes> ArsPrefixesTables;
/* Map from LAG name to ARS object */
typedef std::map<std::string, ArsObjectEntry> ArsLags;
/* Main structure to hold user configuration */
typedef std::map<std::string, ArsProfileEntry> ArsProfiles;
typedef std::map<std::string, ArsObjectEntry> ArsObjects;
/* list of ARS-enabled Interfaces */
typedef std::set<string> ArsEnabledInterfaces;             // ARS-Enabled interfaces for the profile

typedef struct _ars_sai_attr_t
{
    _ars_sai_attr_t(std::string name):
        attr_name(name), create_implemented(false), set_implemented(false), get_implemented(false) {}
    std::string attr_name;
    bool create_implemented;
    bool set_implemented;
    bool get_implemented;
} ars_sai_attr_t;
typedef std::map<uint32_t, ars_sai_attr_t> ars_sai_attr_lookup_t;
typedef struct _ars_sai_feature_data_t {
    std::string name;
    ars_sai_attr_lookup_t attrs;
 } ars_sai_feature_data_t;
typedef std::map<uint32_t, ars_sai_feature_data_t> ars_sai_feature_lookup_t;


class ArsOrch : public Orch, public Observer
{
public:
    ArsOrch(DBConnector *db, DBConnector *appDb, DBConnector *stateDb, vector<string> &tableNames, VRFOrch *vrfOrch);

    bool isRouteArs(sai_object_id_t vrf_id, const IpPrefix &ipPrefix, sai_object_id_t * ars_object_id);
    bool isLagArs(const std::string if_name, sai_object_id_t * ars_object_id);

    // warm reboot support
    bool bake() override;
    void update(SubjectType type, void *cntx);

    void generateLagCounterMap();
    void generateNexthopGroupCounterMap();

protected:
    VRFOrch *m_vrfOrch;
    std::shared_ptr<DBConnector> m_counter_db;
    std::shared_ptr<DBConnector> m_asic_db;
    std::unique_ptr<Table> m_lag_counter_table;
    std::unique_ptr<Table> m_nhg_counter_table;
    std::unique_ptr<Table> m_vidToRidTable;

    SelectableTimer* m_LagFlexCounterUpdTimer = nullptr;
    SelectableTimer* m_NhgFlexCounterUpdTimer = nullptr;

    std::map<sai_object_id_t, std::string> m_pendingLagAddToFlexCntr;
    std::map<sai_object_id_t, std::string> m_pendingNhgAddToFlexCntr;

private:
    bool m_isArsSupported = false;
    ArsProfiles m_arsProfiles;
    ArsObjects m_arsObjects;
    ArsPrefixesTables m_arsNexthopGroupPrefixes;
    ArsEnabledInterfaces m_arsEnabledInterfaces;
    ArsLags m_arsLags;
    FlexCounterManager  m_lag_counter_manager;
    FlexCounterManager  m_nhg_counter_manager;
    std::unique_ptr<Table> m_arsProfileStateTable;
    std::unique_ptr<Table> m_arsIfStateTable;
    std::unique_ptr<Table> m_arsNhgStateTable;
    std::unique_ptr<Table> m_arsLagStateTable;
    ars_sai_feature_lookup_t ars_features;
    bool m_isLagCounterMapGenerated = false;
    bool m_isNhgCounterMapGenerated = false;

    bool setArsProfile(ArsProfileEntry &profile, vector<sai_attribute_t> &ars_attrs);
    bool createArsProfile(ArsProfileEntry &profile, vector<sai_attribute_t> &ars_attrs);
    bool createArsObject(ArsObjectEntry *object, vector<sai_attribute_t> &ars_attrs);
    bool setArsObject(ArsObjectEntry *object, vector<sai_attribute_t> &ars_attrs);    
    bool delArsObject(ArsObjectEntry *object);
    bool updateArsEnabledInterface(const Port &port, const bool is_enable);

    bool updateArsInterface(ArsProfileEntry &profile, const Port &port, const bool is_enable);
    bool doTaskArsObject(Consumer &consumer);
    bool doTaskArsProfile(Consumer &consumer);
    bool doTaskArsInterfaces(Consumer &consumer);
    bool doTaskArsNexthopGroup(Consumer &consumer);
    bool doTaskArsLag(Consumer &consumer);
    void doTask(Consumer& consumer);
    void doTask(swss::SelectableTimer&) override;

    bool isSetImplemented(sai_object_type_t object_type, sai_attr_id_t attr_id)
    {
        auto feature = ars_features.find((uint32_t)object_type);
        if (feature == ars_features.end())
        {
            return false;
        }
        auto attr = feature->second.attrs.find(attr_id);
        if (attr == feature->second.attrs.end())
        {
            return false;
        }
        return attr->second.set_implemented;
    }

    bool isCreateImplemented(sai_object_type_t object_type, sai_attr_id_t attr_id)
    {
        auto feature = ars_features.find((uint32_t)object_type);
        if (feature == ars_features.end())
        {
            return false;
        }
        auto attr = feature->second.attrs.find(attr_id);
        if (attr == feature->second.attrs.end())
        {
            return false;
        }
        return attr->second.create_implemented;
    }

    bool isGetImplemented(sai_object_type_t object_type, sai_attr_id_t attr_id)
    {
        auto feature = ars_features.find((uint32_t)object_type);
        if (feature == ars_features.end())
        {
            return false;
        }
        auto attr = feature->second.attrs.find(attr_id);
        if (attr == feature->second.attrs.end())
        {
            return false;
        }
        return attr->second.get_implemented;
    }

    void initCapabilities()
    {
        SWSS_LOG_ENTER();

        sai_attr_capability_t capability;
        for (auto it = ars_features.begin(); it != ars_features.end(); it++)
        {
            for (auto it2 = it->second.attrs.begin(); it2 != it->second.attrs.end(); it2++)
            {
                if (sai_query_attribute_capability(gSwitchId, (sai_object_type_t)it->first,
                                                    (sai_attr_id_t)it2->first,
                                                    &capability) == SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_NOTICE("Feature %s Attr %s is supported. Create %s Set %s Get %s", it->second.name.c_str(), it2->second.attr_name.c_str(), capability.create_implemented ? "Y" : "N", capability.set_implemented ? "Y" : "N", capability.get_implemented ? "Y" : "N");
                }
                else
                {
                    SWSS_LOG_NOTICE("Feature %s Attr %s is NOT supported", it->second.name.c_str(), it2->second.attr_name.c_str());
                }

                it2->second.create_implemented = capability.create_implemented;
                it2->second.set_implemented = capability.set_implemented;
                it2->second.get_implemented = capability.get_implemented;
            }
        }

        m_isArsSupported = isCreateImplemented(SAI_OBJECT_TYPE_SWITCH, SAI_SWITCH_ATTR_ARS_PROFILE);
    }

    void removeLagFromFlexCounter(sai_object_id_t id, const string &name);
    void addLagToFlexCounter(sai_object_id_t oid, const string &name);
    void addNhgToFlexCounter(sai_object_id_t oid, const IpPrefix &prefix, const string &vrf);
    void removeNhgFromFlexCounter(sai_object_id_t id, const IpPrefix &prefix, const string &vrf);
    std::unordered_set<std::string> generateLagCounterStats();
    std::unordered_set<std::string> generateNhgCounterStats();

};

#endif /* SWSS_ARSORCH_H */
