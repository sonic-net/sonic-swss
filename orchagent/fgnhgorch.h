#ifndef SWSS_FGNHGORCH_H
#define SWSS_FGNHGORCH_H

#include "orch.h"
#include "observer.h"
#include "intfsorch.h"
#include "neighorch.h"

#include "ipaddress.h"
#include "ipaddresses.h"
#include "ipprefix.h"
#include "nexthopgroupkey.h"

#include <map>

/* Maximum next hop group number, TODO: enforcement of this */
#define NHGRP_MAX_SIZE 128

typedef uint32_t Bank;
typedef std::set<NextHopKey> ActiveNextHops;
typedef std::vector<sai_object_id_t> FGNextHopGroupMembers;
typedef std::vector<uint32_t> HashBuckets;
typedef std::map<NextHopKey, HashBuckets> FGNextHopGroupMap;
typedef std::vector<FGNextHopGroupMap> BankFGNextHopGroupMap;
typedef std::map<Bank,Bank> InactiveBankMapsToBank;

struct FGNextHopGroupEntry
{
    sai_object_id_t         next_hop_group_id;      // next hop group id
    int                     ref_count;              // reference count
    FGNextHopGroupMembers   nhopgroup_members;      // ids of members indexed by <ip_address, if_alias>
    ActiveNextHops          active_nexthops;
    BankFGNextHopGroupMap   syncd_fgnhg_map;
    NextHopGroupKey         nhg_key;
    InactiveBankMapsToBank  inactive_to_active_map;
};

/*TODO: can we make an optimization here when we get multiple routes pointing to a fgnhg */
typedef std::map<IpPrefix, FGNextHopGroupEntry> FGRouteTable;
/* RouteTables: vrf_id, RouteTable */
typedef std::map<sai_object_id_t, FGRouteTable> FGRouteTables;

typedef std::string FgNhg;

typedef std::map<IpAddress, Bank> NextHops;
typedef struct
{
    uint32_t start_index;
    uint32_t end_index;
} bank_index_range;

typedef struct FgNhgEntry
{
    string fgNhg_name;
    uint32_t configured_bucket_size;
    uint32_t real_bucket_size;
    NextHops nextHops;
    std::vector<IpPrefix> prefixes;
    std::vector<bank_index_range> hash_bucket_indices;
} FgNhgEntry;

typedef std::map<IpPrefix, FgNhgEntry*> FgNhgPrefixes; 

typedef std::map<FgNhg, FgNhgEntry> FgNhgs;

typedef struct
{
    std::vector<NextHopKey> nhs_to_del;
    std::vector<NextHopKey> nhs_to_add;
    std::vector<NextHopKey> active_nhs;
} Bank_Member_Changes;

class FgNhgOrch : public Orch
{
public:
    FgNhgPrefixes fgNhgPrefixes;
    FgNhgOrch(DBConnector *db, DBConnector *stateDb, vector<string> &tableNames, NeighOrch *neighOrch, IntfsOrch *intfsOrch, VRFOrch *vrfOrch);

    bool addRoute(sai_object_id_t, const IpPrefix&, const NextHopGroupKey&);
    bool removeRoute(sai_object_id_t, const IpPrefix&);
    bool validnexthopinNextHopGroup(const NextHopKey&);
    bool invalidnexthopinNextHopGroup(const NextHopKey&);

private:
    NeighOrch *m_neighOrch;
    IntfsOrch *m_intfsOrch;
    VRFOrch *m_vrfOrch;
    FgNhgs m_FgNhgs;
    FGRouteTables m_syncdFGRouteTables;
    Table m_stateWarmRestartRouteTable;

    bool set_new_nhg_members(FGNextHopGroupEntry &syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
                    std::vector<Bank_Member_Changes> &bank_member_changes, 
                    std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix&);
    bool compute_and_set_hash_bucket_changes(FGNextHopGroupEntry *syncd_fg_route_entry,
                    FgNhgEntry *fgNhgEntry, std::vector<Bank_Member_Changes> &bank_member_changes,
                    std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix&);
    bool set_active_bank_hash_bucket_changes(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
                    uint32_t bank, uint32_t syncd_bank, std::vector<Bank_Member_Changes> bank_member_changes,
                    std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix&);
    bool set_inactive_bank_hash_bucket_changes(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
                    uint32_t bank,std::vector<Bank_Member_Changes> &bank_member_changes,
                    std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix&);
    bool set_inactive_bank_to_next_available_active_bank(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
                        uint32_t bank, std::vector<Bank_Member_Changes> bank_member_changes,
                        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix&);
    bool remove_nhg(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry);
    void set_state_db_route_entry(const IpPrefix&, uint32_t index, NextHopKey nextHop);
    bool write_hash_bucket_change_to_sai(FGNextHopGroupEntry *syncd_fg_route_entry, uint32_t index, sai_object_id_t nh_oid,
            const IpPrefix &ipPrefix, NextHopKey nextHop);

    bool doTaskFgNhg(const KeyOpFieldsValuesTuple&);
    bool doTaskFgNhg_prefix(const KeyOpFieldsValuesTuple&);
    bool doTaskFgNhg_member(const KeyOpFieldsValuesTuple&);
    void doTask(Consumer& consumer);
};

#endif /* SWSS_FGNHGORCH_H */
