#include "srv6orch.h"

#define SRV6_STAT_COUNTER_POLLING_INTERVAL_MS 10000

Srv6Orch::Srv6Orch(DBConnector *cfgDb, DBConnector *applDb, const vector<TableConnector>& tables, SwitchOrch *switchOrch, VRFOrch *vrfOrch, NeighOrch *neighOrch):
    Orch(tables),
    m_vrfOrch(vrfOrch),
    m_switchOrch(switchOrch),
    m_neighOrch(neighOrch),
    m_sidTable(applDb, APP_SRV6_SID_LIST_TABLE_NAME),
    m_mysidTable(applDb, APP_SRV6_MY_SID_TABLE_NAME),
    m_piccontextTable(applDb, APP_PIC_CONTEXT_TABLE_NAME),
    m_mysidCfgTable(cfgDb, CFG_SRV6_MY_SID_TABLE_NAME),
    m_locatorCfgTable(cfgDb, CFG_SRV6_MY_LOCATOR_TABLE_NAME),
    m_counter_manager(SRV6_STAT_COUNTER_FLEX_COUNTER_GROUP, StatsMode::READ, SRV6_STAT_COUNTER_POLLING_INTERVAL_MS, false)
{
}

Srv6Orch::~Srv6Orch()
{
}

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

bool Srv6Orch::removeSrv6Nexthops(const std::vector<NextHopGroupKey>& nhgv)
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

task_process_status Srv6Orch::deleteSidList(const string sid_name)
{
    return task_success;
}

task_process_status Srv6Orch::doTaskSidTable(const KeyOpFieldsValuesTuple & tuple)
{
    return task_success;
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

void Srv6Orch::doTask(SelectableTimer &timer)
{
}

void Srv6Orch::doTask(Consumer &consumer)
{
}