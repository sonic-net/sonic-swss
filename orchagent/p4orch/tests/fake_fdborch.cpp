#include "fdborch.h"

const int FdbOrch::fdborch_pri = 20;

FdbOrch::FdbOrch(DBConnector* applDbConnector, vector<table_name_with_pri_t> appFdbTables,
    TableConnector stateDbFdbConnector, TableConnector stateDbMclagFdbConnector, PortsOrch *port) :
    Orch(applDbConnector, appFdbTables),
    m_portsOrch(port),
    m_fdbStateTable(stateDbFdbConnector.first, stateDbFdbConnector.second),
    m_mclagFdbStateTable(stateDbMclagFdbConnector.first, stateDbMclagFdbConnector.second)
{
}

bool FdbOrch::bake()
{
    return true;
}

bool FdbOrch::storeFdbEntryState(const FdbUpdate& update)
{
    return true;
}

void FdbOrch::clearFdbEntry(const FdbEntry& entry)
{
}

void FdbOrch::handleSyncdFlushNotif(const sai_object_id_t& bv_id,
                                    const sai_object_id_t& bridge_port_id,
                                    const MacAddress& mac,
                                    const sai_fdb_entry_type_t& sai_fdb_type)
{
}

void FdbOrch::update(sai_fdb_event_t        type,
                     const sai_fdb_entry_t* entry,
                     sai_object_id_t        bridge_port_id,
                     const sai_fdb_entry_type_t   &sai_fdb_type)
{
}

void FdbOrch::update(SubjectType type, void *cntx)
{
}

bool FdbOrch::getPort(const MacAddress& mac, uint16_t vlan, Port& port)
{
    return true;
}

void FdbOrch::doTask(Consumer& consumer)
{
}

void FdbOrch::doTask(NotificationConsumer& consumer)
{
}

void FdbOrch::flushFDBEntries(sai_object_id_t bridge_port_oid,
                              sai_object_id_t vlan_oid)
{
}

void FdbOrch::notifyObserversFDBFlush(Port &port, sai_object_id_t& bvid)
{
}

void FdbOrch::updatePortOperState(const PortOperStateUpdate& update)
{
}

void FdbOrch::updateVlanMember(const VlanMemberUpdate& update)
{
}

bool FdbOrch::addFdbEntry(const FdbEntry& entry, const string& port_name,
        FdbData fdbData)
{
    return true;
}

bool FdbOrch::removeFdbEntry(const FdbEntry& entry, FdbOrigin origin)
{
    return true;
}

void FdbOrch::deleteFdbEntryFromSavedFDB(const MacAddress &mac,
        const unsigned short &vlanId, FdbOrigin origin, const string portName)
{
}

void FdbOrch::notifyTunnelOrch(Port& port)
{
}