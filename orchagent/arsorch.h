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
    std::set<NextHopGroupKey> nexthops;             // NHG that are configured to hw
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
    uint32_t max_ecmp_groups;
    uint32_t max_ecmp_members_per_group;
    uint32_t ref_count;                                     // Number of objects using this profile
    sai_object_id_t m_sai_ars_id;                           // SAI ARS profile OID
} ArsProfileEntry;

/* Map from IP prefix to ARS object */
typedef std::map<IpPrefix, ArsObjectEntry> ArsNexthopGroupPrefixes; 
typedef std::map<std::string, ArsNexthopGroupPrefixes> ArsPrefixesTables;
/* Main structure to hold user configuration */
typedef std::map<std::string, ArsProfileEntry> ArsProfiles;
typedef std::map<std::string, ArsObjectEntry> ArsObjects;
/* list of ARS-enabled Interfaces */
typedef std::map<std::string, std::uint32_t> ArsEnabledInterfaces;          // ARS-Enabled interfaces for the profile

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

    // warm reboot support
    bool bake() override;
    void update(SubjectType type, void *cntx);

protected:
    VRFOrch *m_vrfOrch;

private:
    bool m_isArsSupported = false;
    ArsProfiles m_arsProfiles;
    ArsObjects m_arsObjects;
    ArsPrefixesTables m_arsNexthopGroupPrefixes;
    ArsEnabledInterfaces m_arsEnabledInterfaces;
    std::unique_ptr<Table> m_arsProfileStateTable;
    std::unique_ptr<Table> m_arsIfStateTable;
    std::unique_ptr<Table> m_arsNhgStateTable;
    std::unique_ptr<Table> m_arsCapabilityStateTable;

    bool setArsProfile(ArsProfileEntry &profile, vector<sai_attribute_t> &ars_attrs);
    bool createArsProfile(ArsProfileEntry &profile, vector<sai_attribute_t> &ars_attrs);
    bool createArsObject(ArsObjectEntry *object, vector<sai_attribute_t> &ars_attrs);
    bool setArsObject(ArsObjectEntry *object, vector<sai_attribute_t> &ars_attrs);    
    bool delArsObject(ArsObjectEntry *object);
    bool updateArsEnabledInterface(const Port &port, const uint32_t scaling_factor, const bool is_enable);

    bool updateArsInterface(ArsProfileEntry &profile, const Port &port, const bool is_enable);
    bool doTaskArsObject(Consumer &consumer);
    bool doTaskArsProfile(Consumer &consumer);
    bool doTaskArsInterfaces(Consumer &consumer);
    bool doTaskArsNexthopGroup(Consumer &consumer);
    void doTask(Consumer& consumer);

    bool isSetImplemented(sai_object_type_t object_type, sai_attr_id_t attr_id);
    bool isCreateImplemented(sai_object_type_t object_type, sai_attr_id_t attr_id);
    bool isGetImplemented(sai_object_type_t object_type, sai_attr_id_t attr_id);
    void initCapabilities();
};

#endif /* SWSS_ARSORCH_H */
