#include "srv6orch.h"

void Srv6Orch::srv6TunnelUpdateNexthops(const string srv6_source, const NextHopKey nhkey, bool insert)
{
}

size_t Srv6Orch::srv6TunnelNexthopSize(const string srv6_source)
{
    return 0;
}

bool Srv6Orch::createSrv6Tunnel(const string srv6_source)
{
    return true;
}

bool Srv6Orch::srv6NexthopExists(const NextHopKey &nhKey)
{
    return true;
}

bool Srv6Orch::removeSrv6Nexthops(const NextHopGroupKey &nhg)
{
    return true;
}

bool Srv6Orch::createSrv6Nexthop(const NextHopKey &nh)
{
    return true;
}

bool Srv6Orch::srv6Nexthops(const NextHopGroupKey &nhgKey, sai_object_id_t &nexthop_id)
{
    return true;
}

bool Srv6Orch::createUpdateSidList(const string sid_name, const string sid_list, const string sidlist_type)
{
    return true;
}

bool Srv6Orch::deleteSidList(const string sid_name)
{
    return true;
}

void Srv6Orch::doTaskSidTable(const KeyOpFieldsValuesTuple & tuple)
{
}

bool Srv6Orch::mySidExists(string my_sid_string)
{
    return false;
}

void Srv6Orch::updateNeighbor(const NeighborUpdate& update)
{
}

void Srv6Orch::update(SubjectType type, void *cntx)
{
}

bool Srv6Orch::sidEntryEndpointBehavior(string action, sai_my_sid_entry_endpoint_behavior_t &end_behavior,
                                        sai_my_sid_entry_endpoint_behavior_flavor_t &end_flavor)
{
    return true;
}

bool Srv6Orch::mySidVrfRequired(const sai_my_sid_entry_endpoint_behavior_t end_behavior)
{
    return false;
}

bool Srv6Orch::mySidNextHopRequired(const sai_my_sid_entry_endpoint_behavior_t end_behavior)
{
    return false;
}

bool Srv6Orch::createUpdateMysidEntry(string my_sid_string, const string dt_vrf, const string adj, const string end_action)
{
    return true;
}

bool Srv6Orch::deleteMysidEntry(const string my_sid_string)
{
    return true;
}

void Srv6Orch::doTaskMySidTable(const KeyOpFieldsValuesTuple & tuple)
{
}

void Srv6Orch::doTask(Consumer &consumer)
{
}