#include <assert.h>
#include "neighorch.h"
#include "logger.h"
#include "swssnet.h"
#include "crmorch.h"
#include "routeorch.h"
#include "directory.h"
#include "muxorch.h"
#include "observer.h"

extern sai_neighbor_api_t*         sai_neighbor_api;
extern sai_next_hop_api_t*         sai_next_hop_api;

extern PortsOrch *gPortsOrch;
extern sai_object_id_t gSwitchId;
extern CrmOrch *gCrmOrch;
extern RouteOrch *gRouteOrch;
extern FgNhgOrch *gFgNhgOrch;
extern Directory<Orch*> gDirectory;

#define MUX_TUNNEL "MuxTunnel0"

const int neighorch_pri = 30;

NeighOrch::NeighOrch(DBConnector *appDb, string tableName, IntfsOrch *intfsOrch, FdbOrch *fdbOrch, PortsOrch *portsOrch) :
        Orch(appDb, tableName, neighorch_pri),
        m_intfsOrch(intfsOrch),
        m_fdbOrch(fdbOrch),
        m_portsOrch(portsOrch),
        m_appNeighResolveProducer(appDb, APP_NEIGH_RESOLVE_TABLE_NAME)
{
    SWSS_LOG_ENTER();

    m_fdbOrch->attach(this);
}

NeighOrch::~NeighOrch()
{
    if (m_fdbOrch)
    {
        m_fdbOrch->detach(this);
    }
}

bool NeighOrch::resolveNeighborEntry(const NeighborEntry &entry, const MacAddress &mac)
{
    vector<FieldValueTuple>    data;
    IpAddress                  ip = entry.ip_address;
    string                     key, alias = entry.alias;

    key = alias + ":" + entry.ip_address.to_string();
    // We do NOT need to populate mac field as its NOT
    // even used in nbrmgr during ARP resolve. But just keeping here.
    FieldValueTuple fvTuple("mac", mac.to_string().c_str());
    data.push_back(fvTuple);

    SWSS_LOG_INFO("Flushing ARP entry '%s:%s --> %s'",
                  alias.c_str(), ip.to_string().c_str(), mac.to_string().c_str());
    m_appNeighResolveProducer.set(key, data);
    return true;
}

void NeighOrch::resolveNeighbor(const NeighborEntry &entry)
{
    if (m_neighborToResolve.find(entry) == m_neighborToResolve.end()) // TODO: Allow retry for unresolved neighbors
    {
        resolveNeighborEntry(entry, MacAddress());
        m_neighborToResolve.insert(entry);
    }

    return;
}

void NeighOrch::clearResolvedNeighborEntry(const NeighborEntry &entry)
{
    string key, alias = entry.alias;
    key = alias + ":" + entry.ip_address.to_string();
    m_appNeighResolveProducer.del(key);
    return;
}

/*
 * Function Name: processFDBFlushUpdate
 * Description:
 *     Goal of this function is to delete neighbor/ARP entries
 * when a port belonging to a VLAN gets removed.
 * This function is called whenever neighbor orchagent receives
 * SUBJECT_TYPE_FDB_FLUSH_CHANGE notification. Currently we only care for
 * deleted FDB entries. We flush neighbor entry that matches its
 * in-coming interface and MAC with FDB entry's VLAN name and MAC
 * respectively.
 */
void NeighOrch::processFDBFlushUpdate(const FdbFlushUpdate& update)
{
    SWSS_LOG_INFO("processFDBFlushUpdate port: %s",
                    update.port.m_alias.c_str());

    for (auto entry : update.entries)
    {
        // Get Vlan object
        Port vlan;
        if (!m_portsOrch->getPort(entry.bv_id, vlan))
        {
            SWSS_LOG_NOTICE("FdbOrch notification: Failed to locate vlan port \
                             from bv_id 0x%" PRIx64 ".", entry.bv_id);
            continue;
        }
        SWSS_LOG_INFO("Flushing ARP for port: %s, VLAN: %s",
                      vlan.m_alias.c_str(), update.port.m_alias.c_str());

        // If the FDB entry MAC matches with neighbor/ARP entry MAC,
        // and ARP entry incoming interface matches with VLAN name,
        // flush neighbor/arp entry.
        for (const auto &neighborEntry : m_syncdNeighbors)
        {
            if (neighborEntry.first.alias == vlan.m_alias &&
                neighborEntry.second.mac == entry.mac)
            {
                resolveNeighborEntry(neighborEntry.first, neighborEntry.second.mac);
            }
        }
    }
    return;
}

void NeighOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    assert(cntx);

    switch(type) {
        case SUBJECT_TYPE_FDB_FLUSH_CHANGE:
        {
            FdbFlushUpdate *update = reinterpret_cast<FdbFlushUpdate *>(cntx);
            processFDBFlushUpdate(*update);
            break;
        }
        default:
            break;
    }

    return;
}

bool NeighOrch::hasNextHop(const NextHopKey &nexthop)
{
    // First check if mux has NH
    MuxOrch* mux_orch = gDirectory.get<MuxOrch*>();
    sai_object_id_t nhid = mux_orch->getNextHopId(nexthop);
    if (nhid != SAI_NULL_OBJECT_ID)
    {
        return true;
    }

    return m_syncdNextHops.find(nexthop) != m_syncdNextHops.end();
}

bool NeighOrch::addNextHop(const IpAddress &ipAddress, const string &alias)
{
    SWSS_LOG_ENTER();

    Port p;
    if (!gPortsOrch->getPort(alias, p))
    {
        SWSS_LOG_ERROR("Neighbor %s seen on port %s which doesn't exist",
                        ipAddress.to_string().c_str(), alias.c_str());
        return false;
    }
    if (p.m_type == Port::SUBPORT)
    {
        if (!gPortsOrch->getPort(p.m_parent_port_id, p))
        {
            SWSS_LOG_ERROR("Neighbor %s seen on sub interface %s whose parent port doesn't exist",
                            ipAddress.to_string().c_str(), alias.c_str());
            return false;
        }
    }

    NextHopKey nexthop = { ipAddress, alias };
    assert(!hasNextHop(nexthop));
    sai_object_id_t rif_id = m_intfsOrch->getRouterIntfsId(alias);

    vector<sai_attribute_t> next_hop_attrs;

    sai_attribute_t next_hop_attr;
    next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
    next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_IP;
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_IP;
    copy(next_hop_attr.value.ipaddr, ipAddress);
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
    next_hop_attr.value.oid = rif_id;
    next_hop_attrs.push_back(next_hop_attr);

    sai_object_id_t next_hop_id;
    sai_status_t status = sai_next_hop_api->create_next_hop(&next_hop_id, gSwitchId, (uint32_t)next_hop_attrs.size(), next_hop_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create next hop %s on %s, rv:%d",
                       ipAddress.to_string().c_str(), alias.c_str(), status);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_NEXT_HOP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("Created next hop %s on %s",
                    ipAddress.to_string().c_str(), alias.c_str());
    if (m_neighborToResolve.find(nexthop) != m_neighborToResolve.end())
    {
        clearResolvedNeighborEntry(nexthop);
        m_neighborToResolve.erase(nexthop);
        SWSS_LOG_INFO("Resolved neighbor for %s", nexthop.to_string().c_str());
    }

    NextHopEntry next_hop_entry;
    next_hop_entry.next_hop_id = next_hop_id;
    next_hop_entry.ref_count = 0;
    next_hop_entry.nh_flags = 0;
    m_syncdNextHops[nexthop] = next_hop_entry;

    m_intfsOrch->increaseRouterIntfsRefCount(alias);

    if (ipAddress.isV4())
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEXTHOP);
    }
    else
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEXTHOP);
    }

    gFgNhgOrch->validNextHopInNextHopGroup(nexthop);

    // For nexthop with incoming port which has down oper status, NHFLAGS_IFDOWN
    // flag should be set on it.
    // This scenario may happen under race condition where buffered neighbor event
    // is processed after incoming port is down.
    if (p.m_oper_status == SAI_PORT_OPER_STATUS_DOWN)
    {
        if (setNextHopFlag(nexthop, NHFLAGS_IFDOWN) == false)
        {
            SWSS_LOG_WARN("Failed to set NHFLAGS_IFDOWN on nexthop %s for interface %s",
                ipAddress.to_string().c_str(), alias.c_str());
        }
    }
    return true;
}

bool NeighOrch::setNextHopFlag(const NextHopKey &nexthop, const uint32_t nh_flag)
{
    SWSS_LOG_ENTER();

    auto nhop = m_syncdNextHops.find(nexthop);
    bool rc = false;

    assert(nhop != m_syncdNextHops.end());

    if (nhop->second.nh_flags & nh_flag)
    {
        return true;
    }

    nhop->second.nh_flags |= nh_flag;
    uint32_t count;
    switch (nh_flag)
    {
        case NHFLAGS_IFDOWN:
            rc = gRouteOrch->invalidnexthopinNextHopGroup(nexthop, count);
            break;
        default:
            assert(0);
            break;
    }

    return rc;
}

bool NeighOrch::clearNextHopFlag(const NextHopKey &nexthop, const uint32_t nh_flag)
{
    SWSS_LOG_ENTER();

    auto nhop = m_syncdNextHops.find(nexthop);
    bool rc = false;

    assert(nhop != m_syncdNextHops.end());

    if (!(nhop->second.nh_flags & nh_flag))
    {
        return true;
    }

    nhop->second.nh_flags &= ~nh_flag;
    uint32_t count;
    switch (nh_flag)
    {
        case NHFLAGS_IFDOWN:
            rc = gRouteOrch->validnexthopinNextHopGroup(nexthop, count);
            break;
        default:
            assert(0);
            break;
    }

    return rc;
}

bool NeighOrch::isNextHopFlagSet(const NextHopKey &nexthop, const uint32_t nh_flag)
{
    SWSS_LOG_ENTER();

    auto nhop = m_syncdNextHops.find(nexthop);

    if (nhop == m_syncdNextHops.end())
    {
        return false;
    }

    if (nhop->second.nh_flags & nh_flag)
    {
        return true;
    }

    return false;
}

bool NeighOrch::ifChangeInformNextHop(const string &alias, bool if_up)
{
    SWSS_LOG_ENTER();
    bool rc = true;

    for (auto nhop = m_syncdNextHops.begin(); nhop != m_syncdNextHops.end(); ++nhop)
    {
        if (nhop->first.alias != alias)
        {
            continue;
        }

        if (if_up)
        {
            rc = clearNextHopFlag(nhop->first, NHFLAGS_IFDOWN);
        }
        else
        {
            rc = setNextHopFlag(nhop->first, NHFLAGS_IFDOWN);
        }

        if (rc == true)
        {
            continue;
        }
        else
        {
            break;
        }
    }

    return rc;
}

bool NeighOrch::removeNextHop(const IpAddress &ipAddress, const string &alias)
{
    SWSS_LOG_ENTER();

    NextHopKey nexthop = { ipAddress, alias };
    assert(hasNextHop(nexthop));

    gFgNhgOrch->invalidNextHopInNextHopGroup(nexthop);

    if (m_syncdNextHops[nexthop].ref_count > 0)
    {
        SWSS_LOG_ERROR("Failed to remove still referenced next hop %s on %s",
                       ipAddress.to_string().c_str(), alias.c_str());
        return false;
    }

    m_syncdNextHops.erase(nexthop);
    m_intfsOrch->decreaseRouterIntfsRefCount(alias);
    return true;
}

bool NeighOrch::removeOverlayNextHop(const NextHopKey &nexthop)
{
    SWSS_LOG_ENTER();

    assert(hasNextHop(nexthop));

    if (m_syncdNextHops[nexthop].ref_count > 0)
    {
        SWSS_LOG_ERROR("Failed to remove still referenced next hop %s on %s",
                   nexthop.ip_address.to_string().c_str(), nexthop.alias.c_str());
        return false;
    }

    m_syncdNextHops.erase(nexthop);
    return true;
}

sai_object_id_t NeighOrch::getLocalNextHopId(const NextHopKey& nexthop)
{
    if (m_syncdNextHops.find(nexthop) == m_syncdNextHops.end())
    {
        return SAI_NULL_OBJECT_ID;
    }

    return m_syncdNextHops[nexthop].next_hop_id;
}

sai_object_id_t NeighOrch::getNextHopId(const NextHopKey &nexthop)
{
    assert(hasNextHop(nexthop));

    /*
     * The nexthop id could be varying depending on the use-case
     * For e.g, a route could have a direct neighbor but may require
     * to be tx via tunnel nexthop
     */
    MuxOrch* mux_orch = gDirectory.get<MuxOrch*>();
    sai_object_id_t nhid = mux_orch->getNextHopId(nexthop);
    if (nhid != SAI_NULL_OBJECT_ID)
    {
        return nhid;
    }
    return m_syncdNextHops[nexthop].next_hop_id;
}

int NeighOrch::getNextHopRefCount(const NextHopKey &nexthop)
{
    assert(hasNextHop(nexthop));
    return m_syncdNextHops[nexthop].ref_count;
}

void NeighOrch::increaseNextHopRefCount(const NextHopKey &nexthop, uint32_t count)
{
    assert(hasNextHop(nexthop));
    if (m_syncdNextHops.find(nexthop) != m_syncdNextHops.end())
    {
        m_syncdNextHops[nexthop].ref_count += count;
    }
}

void NeighOrch::decreaseNextHopRefCount(const NextHopKey &nexthop, uint32_t count)
{
    assert(hasNextHop(nexthop));
    if (m_syncdNextHops.find(nexthop) != m_syncdNextHops.end())
    {
        m_syncdNextHops[nexthop].ref_count -= count;
    }
}

bool NeighOrch::getNeighborEntry(const NextHopKey &nexthop, NeighborEntry &neighborEntry, MacAddress &macAddress)
{
    if (!hasNextHop(nexthop))
    {
        return false;
    }

    for (const auto &entry : m_syncdNeighbors)
    {
        if (entry.first.ip_address == nexthop.ip_address && entry.first.alias == nexthop.alias)
        {
            neighborEntry = entry.first;
            macAddress = entry.second.mac;
            return true;
        }
    }

    return false;
}

bool NeighOrch::getNeighborEntry(const IpAddress &ipAddress, NeighborEntry &neighborEntry, MacAddress &macAddress)
{
    string alias = m_intfsOrch->getRouterIntfsAlias(ipAddress);
    if (alias.empty())
    {
        return false;
    }

    NextHopKey nexthop(ipAddress, alias);
    return getNeighborEntry(nexthop, neighborEntry, macAddress);
}

void NeighOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        size_t found = key.find(':');
        if (found == string::npos)
        {
            SWSS_LOG_ERROR("Failed to parse key %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        string alias = key.substr(0, found);

        if (alias == "eth0" || alias == "lo" || alias == "docker0")
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        IpAddress ip_address(key.substr(found+1));

        NeighborEntry neighbor_entry = { ip_address, alias };

        if (op == SET_COMMAND)
        {
            Port p;
            if (!gPortsOrch->getPort(alias, p))
            {
                SWSS_LOG_INFO("Port %s doesn't exist", alias.c_str());
                it++;
                continue;
            }

            if (!p.m_rif_id)
            {
                SWSS_LOG_INFO("Router interface doesn't exist on %s", alias.c_str());
                it++;
                continue;
            }

            MacAddress mac_address;
            for (auto i = kfvFieldsValues(t).begin();
                 i  != kfvFieldsValues(t).end(); i++)
            {
                if (fvField(*i) == "neigh")
                    mac_address = MacAddress(fvValue(*i));
            }

            if (m_syncdNeighbors.find(neighbor_entry) == m_syncdNeighbors.end()
                    || m_syncdNeighbors[neighbor_entry].mac != mac_address)
            {
                // only for unresolvable neighbors that are new
                if (mac_address == MacAddress("00:00:00:00:00:00")) 
                {
                    if (m_syncdNeighbors.find(neighbor_entry) == m_syncdNeighbors.end()) {
                        addZeroMacTunnelRoute(neighbor_entry, mac_address);
                    }
                    it = consumer.m_toSync.erase(it);
                }
                else if (addNeighbor(neighbor_entry, mac_address))
                {
                    it = consumer.m_toSync.erase(it);
                }
                else
                {
                    it++;
                    continue;
                }
            }
            else
            {
                /* Duplicate entry */
                it = consumer.m_toSync.erase(it);
            }

            /* Remove remaining DEL operation in m_toSync for the same neighbor.
             * Since DEL operation is supposed to be executed before SET for the same neighbor
             * A remaining DEL after the SET operation means the DEL operation failed previously and should not be executed anymore
             */
            auto rit = make_reverse_iterator(it);
            while (rit != consumer.m_toSync.rend() && rit->first == key && kfvOp(rit->second) == DEL_COMMAND)
            {
                consumer.m_toSync.erase(next(rit).base());
                SWSS_LOG_NOTICE("Removed pending neighbor DEL operation for %s after SET operation", key.c_str());
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_syncdNeighbors.find(neighbor_entry) != m_syncdNeighbors.end())
            {
                if (removeNeighbor(neighbor_entry))
                {
                    it = consumer.m_toSync.erase(it);
                }
                else
                {
                    it++;
                }
            }
            else
                /* Cannot locate the neighbor */
                it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

bool NeighOrch::addNeighbor(const NeighborEntry &neighborEntry, const MacAddress &macAddress)
{
    SWSS_LOG_ENTER();

    sai_status_t status;
    IpAddress ip_address = neighborEntry.ip_address;
    string alias = neighborEntry.alias;

    sai_object_id_t rif_id = m_intfsOrch->getRouterIntfsId(alias);
    if (rif_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_INFO("Failed to get rif_id for %s", alias.c_str());
        return false;
    }

    sai_neighbor_entry_t neighbor_entry;
    neighbor_entry.rif_id = rif_id;
    neighbor_entry.switch_id = gSwitchId;
    copy(neighbor_entry.ip_address, ip_address);

    vector<sai_attribute_t> neighbor_attrs;
    sai_attribute_t neighbor_attr;

    neighbor_attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
    memcpy(neighbor_attr.value.mac, macAddress.getMac(), 6);
    neighbor_attrs.push_back(neighbor_attr);

    MuxOrch* mux_orch = gDirectory.get<MuxOrch*>();
    bool hw_config = isHwConfigured(neighborEntry);

    if (!hw_config && mux_orch->isNeighborActive(ip_address, macAddress, alias))
    {
        status = sai_neighbor_api->create_neighbor_entry(&neighbor_entry,
                                   (uint32_t)neighbor_attrs.size(), neighbor_attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            if (status == SAI_STATUS_ITEM_ALREADY_EXISTS)
            {
                SWSS_LOG_ERROR("Entry exists: neighbor %s on %s, rv:%d",
                           macAddress.to_string().c_str(), alias.c_str(), status);
                /* Returning True so as to skip retry */
                return true;
            }
            else
            {
                SWSS_LOG_ERROR("Failed to create neighbor %s on %s, rv:%d",
                           macAddress.to_string().c_str(), alias.c_str(), status);
                task_process_status handle_status = handleSaiCreateStatus(SAI_API_NEIGHBOR, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
        }
        SWSS_LOG_NOTICE("Created neighbor ip %s, %s on %s", ip_address.to_string().c_str(),
                macAddress.to_string().c_str(), alias.c_str());
        m_intfsOrch->increaseRouterIntfsRefCount(alias);

        if (neighbor_entry.ip_address.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
        {
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEIGHBOR);
        }
        else
        {
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEIGHBOR);
        }

        if (!addNextHop(ip_address, alias))
        {
            status = sai_neighbor_api->remove_neighbor_entry(&neighbor_entry);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove neighbor %s on %s, rv:%d",
                               macAddress.to_string().c_str(), alias.c_str(), status);
                task_process_status handle_status = handleSaiRemoveStatus(SAI_API_NEIGHBOR, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
            m_intfsOrch->decreaseRouterIntfsRefCount(alias);

            if (neighbor_entry.ip_address.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
            {
                gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEIGHBOR);
            }
            else
            {
                gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEIGHBOR);
            }

            return false;
        }
        hw_config = true;
    }
    else if (isHwConfigured(neighborEntry))
    {
        status = sai_neighbor_api->set_neighbor_entry_attribute(&neighbor_entry, &neighbor_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to update neighbor %s on %s, rv:%d",
                           macAddress.to_string().c_str(), alias.c_str(), status);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_NEIGHBOR, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
        SWSS_LOG_NOTICE("Updated neighbor %s on %s", macAddress.to_string().c_str(), alias.c_str());
    }

    m_syncdNeighbors[neighborEntry] = { macAddress, hw_config };

    NeighborUpdate update = { neighborEntry, macAddress, true };
    notify(SUBJECT_TYPE_NEIGH_CHANGE, static_cast<void *>(&update));

    return true;
}

bool NeighOrch::removeNeighbor(const NeighborEntry &neighborEntry, bool disable)
{
    SWSS_LOG_ENTER();

    sai_status_t status;
    IpAddress ip_address = neighborEntry.ip_address;
    string alias = neighborEntry.alias;
    NextHopKey nexthop = { ip_address, alias };

    if (m_syncdNeighbors.find(neighborEntry) == m_syncdNeighbors.end())
    {
        return true;
    }

    if (m_syncdNextHops.find(nexthop) != m_syncdNextHops.end() && m_syncdNextHops[nexthop].ref_count > 0)
    {
        SWSS_LOG_INFO("Failed to remove still referenced neighbor %s on %s",
                      m_syncdNeighbors[neighborEntry].mac.to_string().c_str(), alias.c_str());
        return false;
    }

    if (isHwConfigured(neighborEntry))
    {
        sai_object_id_t rif_id = m_intfsOrch->getRouterIntfsId(alias);

        sai_neighbor_entry_t neighbor_entry;
        neighbor_entry.rif_id = rif_id;
        neighbor_entry.switch_id = gSwitchId;
        copy(neighbor_entry.ip_address, ip_address);

        sai_object_id_t next_hop_id = m_syncdNextHops[nexthop].next_hop_id;
        status = sai_next_hop_api->remove_next_hop(next_hop_id);
        if (status != SAI_STATUS_SUCCESS)
        {
            /* When next hop is not found, we continue to remove neighbor entry. */
            if (status == SAI_STATUS_ITEM_NOT_FOUND)
            {
                SWSS_LOG_ERROR("Failed to locate next hop %s on %s, rv:%d",
                               ip_address.to_string().c_str(), alias.c_str(), status);
            }
            else
            {
                SWSS_LOG_ERROR("Failed to remove next hop %s on %s, rv:%d",
                               ip_address.to_string().c_str(), alias.c_str(), status);
                task_process_status handle_status = handleSaiRemoveStatus(SAI_API_NEXT_HOP, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
        }

        if (status != SAI_STATUS_ITEM_NOT_FOUND)
        {
            if (neighbor_entry.ip_address.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
            {
                gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEXTHOP);
            }
            else
            {
                gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEXTHOP);
            }
        }

        SWSS_LOG_NOTICE("Removed next hop %s on %s",
                        ip_address.to_string().c_str(), alias.c_str());

        status = sai_neighbor_api->remove_neighbor_entry(&neighbor_entry);
        if (status != SAI_STATUS_SUCCESS)
        {
            if (status == SAI_STATUS_ITEM_NOT_FOUND)
            {
                SWSS_LOG_ERROR("Failed to locate neighbor %s on %s, rv:%d",
                        m_syncdNeighbors[neighborEntry].mac.to_string().c_str(), alias.c_str(), status);
                return true;
            }
            else
            {
                SWSS_LOG_ERROR("Failed to remove neighbor %s on %s, rv:%d",
                        m_syncdNeighbors[neighborEntry].mac.to_string().c_str(), alias.c_str(), status);
                task_process_status handle_status = handleSaiRemoveStatus(SAI_API_NEIGHBOR, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
        }

        if (neighbor_entry.ip_address.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
        {
            gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEIGHBOR);
        }
        else
        {
            gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEIGHBOR);
        }

        removeNextHop(ip_address, alias);
        m_intfsOrch->decreaseRouterIntfsRefCount(alias);
    }

    SWSS_LOG_NOTICE("Removed neighbor %s on %s",
            m_syncdNeighbors[neighborEntry].mac.to_string().c_str(), alias.c_str());

    /* Do not delete entry from cache if its disable request */
    if (disable)
    {
        m_syncdNeighbors[neighborEntry].hw_configured = false;
        return true;
    }

    m_syncdNeighbors.erase(neighborEntry);

    NeighborUpdate update = { neighborEntry, MacAddress(), false };
    notify(SUBJECT_TYPE_NEIGH_CHANGE, static_cast<void *>(&update));

    return true;
}

bool NeighOrch::isHwConfigured(const NeighborEntry& neighborEntry)
{
    if (m_syncdNeighbors.find(neighborEntry) == m_syncdNeighbors.end())
    {
        return false;
    }

    return (m_syncdNeighbors[neighborEntry].hw_configured);
}

bool NeighOrch::enableNeighbor(const NeighborEntry& neighborEntry)
{
    SWSS_LOG_NOTICE("Neighbor enable request for %s ", neighborEntry.ip_address.to_string().c_str());

    if (m_syncdNeighbors.find(neighborEntry) == m_syncdNeighbors.end())
    {
        SWSS_LOG_INFO("Neighbor %s not found", neighborEntry.ip_address.to_string().c_str());
        return true;
    }

    if (isHwConfigured(neighborEntry))
    {
        SWSS_LOG_INFO("Neighbor %s is already programmed to HW", neighborEntry.ip_address.to_string().c_str());
        return true;
    }

    return addNeighbor(neighborEntry, m_syncdNeighbors[neighborEntry].mac);
}

bool NeighOrch::disableNeighbor(const NeighborEntry& neighborEntry)
{
    SWSS_LOG_NOTICE("Neighbor disable request for %s ", neighborEntry.ip_address.to_string().c_str());

    if (m_syncdNeighbors.find(neighborEntry) == m_syncdNeighbors.end())
    {
        SWSS_LOG_INFO("Neighbor %s not found", neighborEntry.ip_address.to_string().c_str());
        return true;
    }

    if (!isHwConfigured(neighborEntry))
    {
        SWSS_LOG_INFO("Neighbor %s is not programmed to HW", neighborEntry.ip_address.to_string().c_str());
        return true;
    }

    return removeNeighbor(neighborEntry, true);
}

sai_object_id_t NeighOrch::addTunnelNextHop(const NextHopKey& nh)
{
    SWSS_LOG_ENTER();
    sai_object_id_t nh_id = SAI_NULL_OBJECT_ID;

    EvpnNvoOrch* evpn_orch = gDirectory.get<EvpnNvoOrch*>();
    auto vtep_ptr = evpn_orch->getEVPNVtep();

    if(!vtep_ptr)
    {
        SWSS_LOG_ERROR("Add Tunnel next hop unable to find EVPN VTEP");
        return nh_id;
    }

    auto tun_name = vtep_ptr->getTunnelName();

    VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
    IpAddress tnl_dip = nh.ip_address;
    nh_id = vxlan_orch->createNextHopTunnel(tun_name, tnl_dip, nh.mac_address, nh.vni);

    if (nh_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Failed to create Tunnel next hop %s, %s@%d@%s", tun_name.c_str(), nh.ip_address.to_string().c_str(),
            nh.vni, nh.mac_address.to_string().c_str());
        return nh_id;
    }

    SWSS_LOG_NOTICE("Created Tunnel next hop %s, %s@%d@%s", tun_name.c_str(), nh.ip_address.to_string().c_str(),
            nh.vni, nh.mac_address.to_string().c_str());

    NextHopEntry next_hop_entry;
    next_hop_entry.next_hop_id = nh_id;
    next_hop_entry.ref_count = 0;
    next_hop_entry.nh_flags = 0;
    m_syncdNextHops[nh] = next_hop_entry;

    return nh_id;
}

bool NeighOrch::removeTunnelNextHop(const NextHopKey& nh)
{
    SWSS_LOG_ENTER();

    EvpnNvoOrch* evpn_orch = gDirectory.get<EvpnNvoOrch*>();
    auto vtep_ptr = evpn_orch->getEVPNVtep();

    if(!vtep_ptr)
    {
        SWSS_LOG_ERROR("Remove Tunnel next hop unable to find EVPN VTEP");
        return false;
    }

    auto tun_name = vtep_ptr->getTunnelName();

    VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();

    IpAddress tnl_dip = nh.ip_address;
    if (!vxlan_orch->removeNextHopTunnel(tun_name, tnl_dip, nh.mac_address, nh.vni))
    {
        SWSS_LOG_ERROR("Failed to remove Tunnel next hop %s, %s@%d@%s", tun_name.c_str(), nh.ip_address.to_string().c_str(),
            nh.vni, nh.mac_address.to_string().c_str());
        return false;
    }

    SWSS_LOG_NOTICE("Removed Tunnel next hop %s, %s@%d@%s", tun_name.c_str(), nh.ip_address.to_string().c_str(),
            nh.vni, nh.mac_address.to_string().c_str());
    return true;
}

void NeighOrch::addZeroMacTunnelRoute(const NeighborEntry& entry, const MacAddress& mac) {
    SWSS_LOG_NOTICE("Creating tunnel route for neighbor %s", entry.ip_address.to_string().c_str());
    MuxOrch* mux_orch = gDirectory.get<MuxOrch*>();
    NeighborUpdate update = {entry, mac, true};
    mux_orch->update(SUBJECT_TYPE_NEIGH_CHANGE, static_cast<void *>(&update));
    m_syncdNeighbors[entry] = { mac, false };
}

