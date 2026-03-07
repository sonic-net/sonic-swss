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

#define ARS_OBJECT_DEFAULT_FLOWLET_IDLE_TIME_US     256
#define ARS_OBJECT_DEFAULT_MAX_FLOWS                512
#define ARS_OBJECT_DEFAULT_PRIMARY_PATH_THRESHOLD   16
#define ARS_OBJECT_DEFAULT_ALTERNATIVE_PATH_COST    0

enum ArsAssignMode
{
    PER_FLOWLET_QUALITY = SAI_ARS_MODE_FLOWLET_QUALITY,
    PER_PACKET_QUALITY = SAI_ARS_MODE_PER_PACKET_QUALITY
};

typedef enum ArsAlgorithm
{
    ARS_ALGORITHM_EWMA
} ArsAlgorithm;

enum class ArsSelectorModeNhg
{
    ARS_SELECTOR_MODE_NHG_GLOBAL     = 0,
    ARS_SELECTOR_MODE_NHG_INTERFACE  = 1,
    ARS_SELECTOR_MODE_NHG_NEXTHOP    = 2,
    ARS_SELECTOR_MODE_NHG_INVALID    = 3
};

enum class ArsSelectorModeLag
{
    ARS_SELECTOR_MODE_LAG_GLOBAL     = 0,
    ARS_SELECTOR_MODE_LAG_INTERFACE  = 1,
    ARS_SELECTOR_MODE_LAG_INVALID    = 2
};

typedef struct ArsObjectEntry
{
    std::string ars_obj_name;
    ArsAssignMode assign_mode;                      // Stores an assign_mode from ArsAssignModes
    uint32_t flowlet_idle_time;                     // Flowlet idle time in micro seconds
    uint32_t max_flows;                             // Maximum number of flows in a flowlet
    sai_object_id_t ars_object_id;                  // ARS Object ID if already created
    uint32_t primary_path_threshold;                // Primary path threshold value
    uint32_t alternative_path_cost;                 // Alternative path cost
} ArsObjectEntry;

typedef struct ArsNexthopEntry
{
    std::string ars_obj_name;
    std::string role;
} ArsNexthopEntry;

typedef struct ArsProfileEntry
{
    string profile_name;                                    // Name of ARS profile configured by user
    ArsAlgorithm algorithm;                                 // ARS algorithm
    ArsSelectorModeNhg nhg_selector_mode = ArsSelectorModeNhg::ARS_SELECTOR_MODE_NHG_INTERFACE;
    ArsSelectorModeLag lag_selector_mode = ArsSelectorModeLag::ARS_SELECTOR_MODE_LAG_INTERFACE;
    std::string default_ars_object;
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
typedef std::map<std::pair<std::string, IpAddress>, ArsNexthopEntry> ArsNexthops;
/* Main structure to hold user configuration */
typedef std::map<std::string, ArsProfileEntry> ArsProfiles;
typedef std::map<std::string, ArsObjectEntry> ArsObjects;
/* list of ARS-enabled Interfaces */
typedef std::map<std::string, std::pair<std::uint32_t, std::string>> ArsEnabledInterfaces;  // ARS-Enabled interfaces for the profile

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
    ~ArsOrch() { deinit(); }
    // warm reboot support
    bool bake() override;
    void update(SubjectType type, void *cntx);
    bool isArsProfileEnabled() const;
    bool validateNexthopsForArs(sai_object_id_t vrf_id, const NextHopGroupKey& nextHops, sai_object_id_t &ars_obj_id);
protected:
    VRFOrch *m_vrfOrch;

private:
    bool m_isArsSupported = false;
    ArsProfiles m_arsProfiles;
    ArsObjects m_arsObjects;
    ArsNexthops m_arsNexthops;
    ArsEnabledInterfaces m_arsEnabledInterfaces;
    std::unique_ptr<Table> m_arsProfileStateTable;
    std::unique_ptr<Table> m_arsIfStateTable;
    std::unique_ptr<Table> m_arsNhgStateTable;
    std::unique_ptr<Table> m_arsCapabilityStateTable;
    std::unique_ptr<Table> m_arsObjectStateTable;
  
    bool setArsProfile(ArsProfileEntry &profile, vector<sai_attribute_t> &ars_attrs);
    bool createArsProfile(ArsProfileEntry &profile, vector<sai_attribute_t> &ars_attrs);
    bool setArsObject(ArsObjectEntry *object);    
    bool createArsObject(ArsObjectEntry *object);
    bool deleteArsProfile(ArsProfileEntry &profile);
    std::vector<sai_attribute_t> buildArsAttributesFromObject(ArsObjectEntry *object);
    bool delArsObject(ArsObjectEntry *object);
    bool updateArsEnabledInterface(const Port &port, const uint32_t scaling_factor, const bool is_enable);

    bool validatePortOrSubPorts(const std::string &alias, std::string &ars_object);

    bool validateInterfaceModeArs(const std::set<NextHopKey> &nextHops, 
                                  ArsSelectorModeNhg selector_mode, 
                                  std::string &common_ars_obj);

    ArsSelectorModeNhg getNhgSelectorMode() const;

    bool isDefaultArsObjectValid(ArsSelectorModeNhg selector_mode, std::string &default_ars_object) const;

    bool isPortArsCapable(const std::string &if_name, std::string &ars_object);

    sai_object_id_t getArsObjectId(const std::string &ars_obj_name) const;

    bool findDefaultArsObject(const ArsSelectorModeNhg &selector_mode, std::string &ars_obj_name);

    bool updateArsInterface(ArsProfileEntry &profile, const Port &port, const bool is_enable);
    ArsSelectorModeNhg parseNhgSelectorMode(const std::string &modeStr) const;
    ArsSelectorModeLag parseLagSelectorMode(const std::string &modeStr) const;
    void createArsProfileSelectorMode(ArsProfileEntry &profile, ArsSelectorModeNhg new_nhg_mode, ArsSelectorModeNhg prev_nhg_mode);

    void processArsInterfaces(bool enable, std::uint32_t scaling_factor);

    bool doTaskArsObject(Consumer &consumer);
    bool doTaskArsProfile(Consumer &consumer);
    bool doTaskArsInterfaces(Consumer &consumer);
    bool doTaskArsNexthop(Consumer &consumer);
    void doTask(Consumer& consumer);

    bool isSetImplemented(sai_object_type_t object_type, sai_attr_id_t attr_id);
    bool isCreateImplemented(sai_object_type_t object_type, sai_attr_id_t attr_id);
    bool isGetImplemented(sai_object_type_t object_type, sai_attr_id_t attr_id);
    void initCapabilities();
    void deinit();
    void deinitCapabilities();
};

#endif /* SWSS_ARSORCH_H */
