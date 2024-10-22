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

/* Name of the ARS profile */
typedef std::string ArsProfile;

enum ArsMatchMode
{
    MATCH_ROUTE_BASED,
    MATCH_NEXTHOP_BASED
};
enum ArsAssignMode
{
    PER_FLOWLET_QUALITY,
    PER_PACKET
};

typedef struct ArsProfileEntry
{
    string profile_name;                            // Name of ARS profile configured by user
    std::vector<IpPrefix> prefixes;                 // Prefix which desires FG behavior
    ArsMatchMode match_mode;                        // Stores a match_mode from ArsMatchModes
    ArsAssignMode assign_mode;                      // Stores an assign_mode from ArsAssignModes
    uint32_t flowlet_idle_time;                     // Flowlet idle time in micro seconds
    uint32_t max_flows;                             // Maximum number of flows in a flowlet
    std::set<string> minPathInterfaces;             // Min path interfaces for the profile
    sai_object_id_t ars_object_id;                  // ARS Object ID if already created
} ArsProfileEntry;

/* Map from IP prefix to user configured ARS entries */
typedef std::map<IpPrefix, ArsProfileEntry*> ArsNexthopGroupPrefixes; 
/* Main structure to hold user configuration */
typedef std::map<ArsProfile, ArsProfileEntry> ArsProfiles;


class ArsOrch : public Orch, public Observer
{
public:
    ArsOrch(DBConnector *db, DBConnector *appDb, DBConnector *stateDb, vector<table_name_with_pri_t> &tableNames);

    bool setArsProfile(ArsProfileEntry &profile);
    bool createArsProfile(ArsProfileEntry &profile);
    bool isRouteArs(sai_object_id_t vrf_id, const IpPrefix &ipPrefix, sai_object_id_t * ars_object_id);

    // warm reboot support
    bool bake() override;
    void update(SubjectType type, void *cntx);

private:
    sai_object_id_t m_sai_ars_id;
    sai_object_id_t m_sai_ars_profile_id;
    ArsProfiles m_arsProfiles;
    ArsNexthopGroupPrefixes m_arsNexthopGroupPrefixes;
    bool isArsConfigured;

    bool updateArsMinPathInterface(ArsProfileEntry &profile, const Port &port, const bool is_enable);
    bool doTaskArsProfile(const KeyOpFieldsValuesTuple&);
    bool doTaskArsMinPathInterfaces(const KeyOpFieldsValuesTuple&);
    bool doTaskArsNhgPrefix(const KeyOpFieldsValuesTuple&);
    void doTask(Consumer& consumer);
};

#endif /* SWSS_ARSORCH_H */
