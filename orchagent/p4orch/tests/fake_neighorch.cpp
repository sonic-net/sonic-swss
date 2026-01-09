
#include "neighorch.h"

extern sai_neighbor_api_t* sai_neighbor_api;
extern size_t gMaxBulkSize;
const int neighorch_pri = 30;

NeighOrch::NeighOrch(swss::DBConnector *appDb, string tableName, IntfsOrch *intfsOrch, FdbOrch *fdbOrch, PortsOrch *portsOrch, swss::DBConnector *chassisAppDb) :
        gNeighBulker(sai_neighbor_api, gMaxBulkSize),
        Orch(appDb, tableName, neighorch_pri),
        m_intfsOrch(intfsOrch),
        m_fdbOrch(fdbOrch),
        m_portsOrch(portsOrch),
        m_appNeighResolveProducer(appDb, APP_NEIGH_RESOLVE_TABLE_NAME)
{
}

NeighOrch::~NeighOrch()
{
}

bool NeighOrch::resolveNeighborEntry(const NeighborEntry &entry, const MacAddress &mac)
{
    return true;
}

void NeighOrch::resolveNeighbor(const NeighborEntry &entry)
{
}

void NeighOrch::clearResolvedNeighborEntry(const NeighborEntry &entry)
{
}

void NeighOrch::processFDBFlushUpdate(const FdbFlushUpdate& update)
{
}

void NeighOrch::update(SubjectType type, void *cntx)
{
}

bool NeighOrch::hasNextHop(const NextHopKey &nexthop)
{
    return true;
}

bool NeighOrch::isNeighborResolved(const NextHopKey &nexthop)
{
    return true;
}

bool NeighOrch::addNextHop(const NextHopKey &nh)
{
    return true;
}

bool NeighOrch::setNextHopFlag(const NextHopKey &nexthop, const uint32_t nh_flag)
{
    return true;
}

bool NeighOrch::clearNextHopFlag(const NextHopKey &nexthop, const uint32_t nh_flag)
{
    return true;
}

bool NeighOrch::isNextHopFlagSet(const NextHopKey &nexthop, const uint32_t nh_flag)
{
    return true;
}

bool NeighOrch::ifChangeInformNextHop(const string &alias, bool if_up)
{
    return true;
}

void NeighOrch::updateNextHop(const BfdUpdate& update)
{
}

bool NeighOrch::removeNextHop(const IpAddress &ipAddress, const string &alias)
{
    return true;
}

bool NeighOrch::removeMplsNextHop(const NextHopKey& nh)
{
    return true;
}

bool NeighOrch::removeOverlayNextHop(const NextHopKey &nexthop)
{
    return true;
}

sai_object_id_t NeighOrch::getLocalNextHopId(const NextHopKey& nexthop)
{
    return 0;
}

sai_object_id_t NeighOrch::getNextHopId(const NextHopKey &nexthop)
{
    return 0;
}

int NeighOrch::getNextHopRefCount(const NextHopKey &nexthop)
{
    return 0;
}

void NeighOrch::increaseNextHopRefCount(const NextHopKey &nexthop, uint32_t count)
{
}

void NeighOrch::decreaseNextHopRefCount(const NextHopKey &nexthop, uint32_t count)
{
}

bool NeighOrch::getNeighborEntry(const NextHopKey &nexthop, NeighborEntry &neighborEntry, MacAddress &macAddress)
{
    return true;
}

bool NeighOrch::getNeighborEntry(const IpAddress &ipAddress, NeighborEntry &neighborEntry, MacAddress &macAddress)
{
    return true;
}

void NeighOrch::doTask(Consumer &consumer)
{
}

bool NeighOrch::addNeighbor(NeighborContext& ctx)
{
    return true;
}

bool NeighOrch::removeNeighbor(NeighborContext& ctx, bool disable)
{
    return true;
}

bool NeighOrch::processBulkEnableNeighbor(NeighborContext& ctx)
{
    return true;
}

bool NeighOrch::processBulkDisableNeighbor(NeighborContext& ctx)
{
    return true;
}

bool NeighOrch::isHwConfigured(const NeighborEntry& neighborEntry)
{
    return true;
}

bool NeighOrch::enableNeighbor(const NeighborEntry& neighborEntry)
{
    return true;
}

bool NeighOrch::disableNeighbor(const NeighborEntry& neighborEntry)
{
    return true;
}

bool NeighOrch::enableNeighbors(std::list<NeighborContext>& bulk_ctx_list)
{
    return true;
}

bool NeighOrch::disableNeighbors(std::list<NeighborContext>& bulk_ctx_list)
{
    return true;
}

sai_object_id_t NeighOrch::addTunnelNextHop(const NextHopKey& nh)
{
    return 0;
}

bool NeighOrch::removeTunnelNextHop(const NextHopKey& nh)
{
    return true;
}

void NeighOrch::doVoqSystemNeighTask(Consumer &consumer)
{
}

bool NeighOrch::addInbandNeighbor(string alias, IpAddress ip_address)
{
    return true;
}

bool NeighOrch::delInbandNeighbor(string alias, IpAddress ip_address)
{
    return true;
}

bool NeighOrch::getSystemPortNeighEncapIndex(string &alias, IpAddress &ip, uint32_t &encap_index)
{
    return true;
}

bool NeighOrch::addVoqEncapIndex(string &alias, IpAddress &ip, vector<sai_attribute_t> &neighbor_attrs)
{
    return true;
}

void NeighOrch::voqSyncAddNeigh(string &alias, IpAddress &ip_address, const MacAddress &mac, sai_neighbor_entry_t &neighbor_entry)
{
}

void NeighOrch::voqSyncDelNeigh(string &alias, IpAddress &ip_address)
{
}

bool NeighOrch::updateVoqNeighborEncapIndex(const NeighborEntry &neighborEntry, uint32_t encap_index)
{
    return true;
}

void NeighOrch::updateSrv6Nexthop(const NextHopKey &nh, const sai_object_id_t &nh_id)
{
}

bool NeighOrch::addZeroMacTunnelRoute(const NeighborEntry& entry, const MacAddress& mac)
{
    return true;
}

bool NeighOrch::ifChangeInformRemoteNextHop(const string &alias, bool if_up)
{
    return true;
}