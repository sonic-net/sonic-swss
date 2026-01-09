#include "fgnhgorch.h"

FgNhgOrch::FgNhgOrch(DBConnector *db, DBConnector *appDb, DBConnector *stateDb, vector<table_name_with_pri_t> &tableNames, NeighOrch *neighOrch, IntfsOrch *intfsOrch, VRFOrch *vrfOrch) :
        Orch(db, tableNames),
        m_neighOrch(neighOrch),
        m_intfsOrch(intfsOrch),
        m_vrfOrch(vrfOrch),
        m_stateWarmRestartRouteTable(stateDb, STATE_FG_ROUTE_TABLE_NAME),
        m_routeTable(appDb, APP_ROUTE_TABLE_NAME)
{
}

void FgNhgOrch::update(SubjectType type, void *cntx)
{
}

bool FgNhgOrch::bake()
{
    return true;
}

void FgNhgOrch::calculateBankHashBucketStartIndices(FgNhgEntry *fgNhgEntry)
{
}

void FgNhgOrch::setStateDbRouteEntry(const IpPrefix &ipPrefix, uint32_t index, NextHopKey nextHop)
{
}

bool FgNhgOrch::writeHashBucketChange(FGNextHopGroupEntry *syncd_fg_route_entry, uint32_t index, sai_object_id_t nh_oid,
        const IpPrefix &ipPrefix, NextHopKey nextHop)
{
    return true;
}

bool FgNhgOrch::createFineGrainedNextHopGroup(FGNextHopGroupEntry &syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        const NextHopGroupKey &nextHops)
{
    return true;
}

bool FgNhgOrch::removeFineGrainedNextHopGroup(FGNextHopGroupEntry *syncd_fg_route_entry)
{
    return true;
}

bool FgNhgOrch::modifyRoutesNextHopId(sai_object_id_t vrf_id, const IpPrefix &ipPrefix, sai_object_id_t next_hop_id)
{
    return true;
}

bool FgNhgOrch::validNextHopInNextHopGroup(const NextHopKey& nexthop)
{
    return true;
}

bool FgNhgOrch::invalidNextHopInNextHopGroup(const NextHopKey& nexthop)
{
    return true;
}

bool FgNhgOrch::setActiveBankHashBucketChanges(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        uint32_t bank, uint32_t syncd_bank, std::vector<BankMemberChanges> bank_member_changes,
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix &ipPrefix)
{
    return true;
}

bool FgNhgOrch::setInactiveBankToNextAvailableActiveBank(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        uint32_t bank, std::vector<BankMemberChanges> bank_member_changes,
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix &ipPrefix)
{
    return true;
}

bool FgNhgOrch::setInactiveBankHashBucketChanges(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        uint32_t bank,std::vector<BankMemberChanges> &bank_member_changes,
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix &ipPrefix)
{
    return true;
}

bool FgNhgOrch::computeAndSetHashBucketChanges(FGNextHopGroupEntry *syncd_fg_route_entry,
        FgNhgEntry *fgNhgEntry, std::vector<BankMemberChanges> &bank_member_changes,
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set,
        const IpPrefix &ipPrefix)
{
    return true;
}

bool FgNhgOrch::setNewNhgMembers(FGNextHopGroupEntry &syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        std::vector<BankMemberChanges> &bank_member_changes, std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set,
        const IpPrefix &ipPrefix)
{
    return true;
}

bool FgNhgOrch::isRouteFineGrained(sai_object_id_t vrf_id, const IpPrefix &ipPrefix, const NextHopGroupKey &nextHops)
{
    return true;
}

bool FgNhgOrch::syncdContainsFgNhg(sai_object_id_t vrf_id, const IpPrefix &ipPrefix)
{
    return true;
}

bool FgNhgOrch::setFgNhg(sai_object_id_t vrf_id, const IpPrefix &ipPrefix, const NextHopGroupKey &nextHops,
                                    sai_object_id_t &next_hop_id, bool &isNextHopIdChanged)
{
    return true;
}

bool FgNhgOrch::removeFgNhg(sai_object_id_t vrf_id, const IpPrefix &ipPrefix)
{
    return true;
}

vector<FieldValueTuple> FgNhgOrch::generateRouteTableFromNhgKey(NextHopGroupKey nhg)
{
    vector<FieldValueTuple> fvVector;
    return fvVector;
}

void FgNhgOrch::cleanupIpInLinkToIpMap(const string &link, const IpAddress &ip, FgNhgEntry &fgNhg_entry)
{
}

bool FgNhgOrch::doTaskFgNhg(const KeyOpFieldsValuesTuple & t)
{
    return true;
}

bool FgNhgOrch::doTaskFgNhgPrefix(const KeyOpFieldsValuesTuple & t)
{
    return true;
}

bool FgNhgOrch::doTaskFgNhgMember(const KeyOpFieldsValuesTuple & t)
{
    return true;
}

void FgNhgOrch::doTask(Consumer& consumer)
{
}