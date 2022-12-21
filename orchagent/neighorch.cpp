#include <assert.h>
#include "neighorch.h"
#include "logger.h"
#include "swssnet.h"
#include "crmorch.h"
#include "routeorch.h"
#include "directory.h"
#include "muxorch.h"
#include "subscriberstatetable.h"
#include "nhgorch.h"

extern sai_neighbor_api_t*         sai_neighbor_api;
extern sai_next_hop_api_t*         sai_next_hop_api;

extern PortsOrch *gPortsOrch;
extern sai_object_id_t gSwitchId;
extern CrmOrch *gCrmOrch;
extern RouteOrch *gRouteOrch;
extern NhgOrch *gNhgOrch;
extern FgNhgOrch *gFgNhgOrch;
extern Directory<Orch*> gDirectory;
extern string gMySwitchType;
extern int32_t gVoqMySwitchId;

const int neighorch_pri = 30;

NeighOrch::NeighOrch(DBConnector *appDb, string tableName, IntfsOrch *intfsOrch, FdbOrch *fdbOrch, PortsOrch *portsOrch, DBConnector *chassisAppDb) :
        Orch(appDb, tableName, neighorch_pri),
        m_intfsOrch(intfsOrch),
        m_fdbOrch(fdbOrch),
        m_portsOrch(portsOrch),
        m_appNeighResolveProducer(appDb, APP_NEIGH_RESOLVE_TABLE_NAME)
{
    SWSS_LOG_ENTER();

    m_fdbOrch->attach(this);

    if(gMySwitchType == "voq")
    {
        //Add subscriber to process VOQ system neigh
        tableName = CHASSIS_APP_SYSTEM_NEIGH_TABLE_NAME;
        Orch::addExecutor(new Consumer(new SubscriberStateTable(chassisAppDb, tableName, TableConsumable::DEFAULT_POP_BATCH_SIZE, 0), this, tableName));
        m_tableVoqSystemNeighTable = unique_ptr<Table>(new Table(chassisAppDb, CHASSIS_APP_SYSTEM_NEIGH_TABLE_NAME));

        //STATE DB connection for setting state of the remote neighbor SAI programming
        unique_ptr<DBConnector> stateDb;
        stateDb = make_unique<DBConnector>("STATE_DB", 0);
        m_stateSystemNeighTable = unique_ptr<Table>(new Table(stateDb.get(), STATE_SYSTEM_NEIGH_TABLE_NAME));
    }
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

// Check if the underlying neighbor is resolved for a given next hop key.
bool NeighOrch::isNeighborResolved(const NextHopKey &nexthop)
{
    // Extract the IP address and interface from the next hop key, and check if the next hop
    // for just that pair exists.
    NextHopKey base_nexthop = NextHopKey(nexthop.ip_address, nexthop.alias);

    return hasNextHop(base_nexthop);
}

bool NeighOrch::addNextHop(const NextHopKey &nh)
{
    SWSS_LOG_ENTER();

    Port p;
    if (!gPortsOrch->getPort(nh.alias, p))
    {
        SWSS_LOG_ERROR("Neighbor %s seen on port %s which doesn't exist",
                        nh.ip_address.to_string().c_str(), nh.alias.c_str());
        return false;
    }
    if (p.m_type == Port::SUBPORT)
    {
        if (!gPortsOrch->getPort(p.m_parent_port_id, p))
        {
            SWSS_LOG_ERROR("Neighbor %s seen on sub interface %s whose parent port doesn't exist",
                            nh.ip_address.to_string().c_str(), nh.alias.c_str());
            return false;
        }
    }

    NextHopKey nexthop(nh);
    if (m_intfsOrch->isRemoteSystemPortIntf(nh.alias))
    {
        //For remote system ports kernel nexthops are always on inband. Change the key
        Port inbp;
        gPortsOrch->getInbandPort(inbp);
        assert(inbp.m_alias.length());

        nexthop.alias = inbp.m_alias;
    }

    assert(!hasNextHop(nexthop));
    sai_object_id_t rif_id = m_intfsOrch->getRouterIntfsId(nh.alias);

    vector<sai_attribute_t> next_hop_attrs;

    vector<Label> label_stack;
    sai_attribute_t next_hop_attr;
    if (nexthop.isMplsNextHop())
    {
        next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
        next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_MPLS;
        next_hop_attrs.push_back(next_hop_attr);

        next_hop_attr.id = SAI_NEXT_HOP_ATTR_OUTSEG_TYPE;
        next_hop_attr.value.s32 = nexthop.label_stack.m_outseg_type;
        next_hop_attrs.push_back(next_hop_attr);

        next_hop_attr.id = SAI_NEXT_HOP_ATTR_LABELSTACK;
        label_stack = nexthop.label_stack.getLabelStack();
        next_hop_attr.value.u32list.list = label_stack.data();
        next_hop_attr.value.u32list.count = static_cast<uint32_t>(nexthop.label_stack.getSize());
        next_hop_attrs.push_back(next_hop_attr);
    }
    else
    {
        next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
        next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_IP;
        next_hop_attrs.push_back(next_hop_attr);
    }

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_IP;
    copy(next_hop_attr.value.ipaddr, nexthop.ip_address);
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
    next_hop_attr.value.oid = rif_id;
    next_hop_attrs.push_back(next_hop_attr);

    sai_object_id_t next_hop_id;
    sai_status_t status = sai_next_hop_api->create_next_hop(&next_hop_id, gSwitchId, (uint32_t)next_hop_attrs.size(), next_hop_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create next hop %s on %s, rv:%d",
                       nexthop.ip_address.to_string().c_str(), nexthop.alias.c_str(), status);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_NEXT_HOP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("Created next hop %s on %s",
                    nexthop.ip_address.to_string().c_str(), nexthop.alias.c_str());
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

    m_intfsOrch->increaseRouterIntfsRefCount(nexthop.alias);

    if (nexthop.isMplsNextHop())
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_MPLS_NEXTHOP);
    }
    else
    {
        if (nexthop.ip_address.isV4())
        {
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEXTHOP);
        }
        else
        {
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEXTHOP);
        }
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
                nexthop.ip_address.to_string().c_str(), nexthop.alias.c_str());
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
            rc &= gNhgOrch->invalidateNextHop(nexthop);
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
            rc &= gNhgOrch->validateNextHop(nexthop);
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
    if(m_intfsOrch->isRemoteSystemPortIntf(alias))
    {
        //For remote system ports kernel nexthops are always on inband. Change the key
        Port inbp;
        gPortsOrch->getInbandPort(inbp);
        assert(inbp.m_alias.length());

        nexthop.alias = inbp.m_alias;
    }

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

bool NeighOrch::removeMplsNextHop(const NextHopKey& nh)
{
    SWSS_LOG_ENTER();

    NextHopKey nexthop(nh);
    if (m_intfsOrch->isRemoteSystemPortIntf(nexthop.alias))
    {
        //For remote system ports kernel nexthops are always on inband. Change the key
        Port inbp;
        gPortsOrch->getInbandPort(inbp);
        assert(inbp.m_alias.length());

        nexthop.alias = inbp.m_alias;
    }

    assert(hasNextHop(nexthop));

    SWSS_LOG_INFO("Removing next hop %s", nexthop.to_string().c_str());

    gFgNhgOrch->invalidNextHopInNextHopGroup(nexthop);

    if (m_syncdNextHops[nexthop].ref_count > 0)
    {
        SWSS_LOG_ERROR("Failed to remove still referenced next hop %s",
                       nexthop.to_string().c_str());
        return false;
    }

    sai_object_id_t next_hop_id = m_syncdNextHops[nexthop].next_hop_id;
    sai_status_t status = sai_next_hop_api->remove_next_hop(next_hop_id);

    /*
     * If the next hop removal fails and not because the next hop doesn't
     * exist, return false.
     */
    if (status != SAI_STATUS_SUCCESS)
    {
        /* When next hop is not found, we continue to remove neighbor entry. */
        if (status == SAI_STATUS_ITEM_NOT_FOUND)
        {
            SWSS_LOG_ERROR("Failed to locate next hop %s, rv:%d",
                           nexthop.to_string().c_str(), status);
        }
        else
        {
            SWSS_LOG_ERROR("Failed to remove next hop %s, rv:%d",
                           nexthop.to_string().c_str(), status);
            task_process_status handle_status = handleSaiRemoveStatus(SAI_API_NEXT_HOP, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }
    /* If we successfully removed the next hop, decrement the ref counters. */
    else if (status == SAI_STATUS_SUCCESS)
    {
        if (nexthop.isMplsNextHop())
        {
            gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_MPLS_NEXTHOP);
        }
    }

    m_syncdNextHops.erase(nexthop);
    m_intfsOrch->decreaseRouterIntfsRefCount(nexthop.alias);
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
    Port inbp;
    string nbr_alias;
    if (!hasNextHop(nexthop))
    {
        return false;
    }
    if (gMySwitchType == "voq")
    {
        gPortsOrch->getInbandPort(inbp);
        assert(inbp.m_alias.length());
    }

    for (const auto &entry : m_syncdNeighbors)
    {
        if (entry.first.ip_address == nexthop.ip_address)
        {
           if (m_intfsOrch->isRemoteSystemPortIntf(entry.first.alias))
           {
               //For remote system ports, nexthops are always on inband.
               nbr_alias = inbp.m_alias;
           }
           else
           {
               nbr_alias = entry.first.alias;
           }
           if (nbr_alias == nexthop.alias)
           {
              neighborEntry = entry.first;
              macAddress = entry.second.mac;
              return true;
           }
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

    string table_name = consumer.getTableName();
    if(table_name == CHASSIS_APP_SYSTEM_NEIGH_TABLE_NAME)
    {
        doVoqSystemNeighTask(consumer);
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

        if (alias == "eth0" || alias == "lo" || alias == "docker0"
            || ((op == SET_COMMAND) && m_intfsOrch->isInbandIntfInMgmtVrf(alias)))
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if(gPortsOrch->isInbandPort(alias))
        {
            Port ibport;
            gPortsOrch->getInbandPort(ibport);
            if(ibport.m_type != Port::VLAN)
            {
                //For "port" type Inband, the neighbors are only remote neighbors.
                //Hence, this is the neigh learned due to the kernel entry added on
                //Inband interface for the remote system port neighbors. Skip
                it = consumer.m_toSync.erase(it);
                continue;
            }
            //For "vlan" type inband, may identify the remote neighbors and skip
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
                if (!mac_address) 
                {
                    if (m_syncdNeighbors.find(neighbor_entry) == m_syncdNeighbors.end())
                    {
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

    if ((ip_address.getAddrScope() == IpAddress::LINK_SCOPE) && (ip_address.isV4()))
    {
        /* Check if this prefix is a configured ip, if not allow */
        IpPrefix ipll_prefix(ip_address.getV4Addr(), 16);
        if (!m_intfsOrch->isPrefixSubnet (ipll_prefix, alias))
        {
            neighbor_attr.id = SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE;
            neighbor_attr.value.booldata = 1;
            neighbor_attrs.push_back(neighbor_attr);
        }
    }

    MuxOrch* mux_orch = gDirectory.get<MuxOrch*>();
    bool hw_config = isHwConfigured(neighborEntry);

    if (gMySwitchType == "voq")
    {
        if (!addVoqEncapIndex(alias, ip_address, neighbor_attrs))
        {
            return false;
        }
    }

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

        if (!addNextHop(NextHopKey(ip_address, alias)))
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
        for (auto itr : neighbor_attrs)
        {
            status = sai_neighbor_api->set_neighbor_entry_attribute(&neighbor_entry, &itr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to update neighbor %s on %s, attr.id=0x%x, rv:%d",
                               macAddress.to_string().c_str(), alias.c_str(), itr.id, status);
                task_process_status handle_status = handleSaiSetStatus(SAI_API_NEIGHBOR, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
        }
        SWSS_LOG_NOTICE("Updated neighbor %s on %s", macAddress.to_string().c_str(), alias.c_str());
    }

    m_syncdNeighbors[neighborEntry] = { macAddress, hw_config };

    NeighborUpdate update = { neighborEntry, macAddress, true };
    notify(SUBJECT_TYPE_NEIGH_CHANGE, static_cast<void *>(&update));

    if(gMySwitchType == "voq")
    {
        //Sync the neighbor to add to the CHASSIS_APP_DB
        voqSyncAddNeigh(alias, ip_address, macAddress, neighbor_entry);
    }

    return true;
}

bool NeighOrch::removeNeighbor(const NeighborEntry &neighborEntry, bool disable)
{
    SWSS_LOG_ENTER();

    sai_status_t status;
    IpAddress ip_address = neighborEntry.ip_address;
    string alias = neighborEntry.alias;

    NextHopKey nexthop = { ip_address, alias };
    if(m_intfsOrch->isRemoteSystemPortIntf(alias))
    {
        //For remote system ports kernel nexthops are always on inband. Change the key
        Port inbp;
        gPortsOrch->getInbandPort(inbp);
        assert(inbp.m_alias.length());

        nexthop.alias = inbp.m_alias;
    }

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

    if(gMySwitchType == "voq")
    {
        //Sync the neighbor to delete from the CHASSIS_APP_DB
        voqSyncDelNeigh(alias, ip_address);
    }

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

void NeighOrch::doVoqSystemNeighTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    //Local inband port as the outgoing interface of the static neighbor and static route
    Port ibif;
    if(!gPortsOrch->getInbandPort(ibif))
    {
        //Inband port is not ready yet.
        return;
    }

    // For "port" type inband interface, wait till the Inband interface is both admin up and oper up
    if (ibif.m_type != Port::VLAN)
    {
        if (ibif.m_admin_state_up != true || ibif.m_oper_status != SAI_PORT_OPER_STATUS_UP)
        {
            // Inband port is not operational yet
            return;
        }
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);

        size_t found = key.find_last_of(consumer.getConsumerTable()->getTableNameSeparator().c_str());
        if (found == string::npos)
        {
            SWSS_LOG_ERROR("Failed to parse key %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        string alias = key.substr(0, found);

        if(gIntfsOrch->isLocalSystemPortIntf(alias))
        {
            //Synced local neighbor. Skip
            it = consumer.m_toSync.erase(it);
            continue;
        }

        IpAddress ip_address(key.substr(found+1));

        NeighborEntry neighbor_entry = { ip_address, alias };

        string state_key = alias + state_db_key_delimiter + ip_address.to_string();

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
            uint32_t encap_index = 0;
            for (auto i = kfvFieldsValues(t).begin();
                 i  != kfvFieldsValues(t).end(); i++)
            {
                if (fvField(*i) == "neigh")
                    mac_address = MacAddress(fvValue(*i));

                if(fvField(*i) == "encap_index")
                {
                    encap_index = (uint32_t)stoul(fvValue(*i));
                }
            }

            if(!encap_index)
            {
                //Encap index is not available yet. Since this is remote neighbor, we need to wait till
                //Encap index is made available either by dynamic syncing or by static config
                it++;
                continue;
            }

            if (m_syncdNeighbors.find(neighbor_entry) == m_syncdNeighbors.end() ||
                    m_syncdNeighbors[neighbor_entry].mac != mac_address ||
                    m_syncdNeighbors[neighbor_entry].voq_encap_index != encap_index)
            {

                if (m_syncdNeighbors.find(neighbor_entry) != m_syncdNeighbors.end() &&
                    m_syncdNeighbors[neighbor_entry].voq_encap_index != encap_index)
                {

                    // Encap index changed. Set encap index attribute with new encap index
                    if (!updateVoqNeighborEncapIndex(neighbor_entry, encap_index))
                    {
                        // Setting encap index failed. SAI does not support change of encap index for
                        // existing neighbors. Remove the neighbor but do not errase from consumer sync
                        // buffer. The next iteration will add the neighbor back with new encap index

                        SWSS_LOG_NOTICE("VOQ encap index set failed for neighbor %s. Removing and re-adding", kfvKey(t).c_str());

                        //Remove neigh from SAI
                        if (removeNeighbor(neighbor_entry))
                        {
                            //neigh successfully deleted from SAI. Set STATE DB to signal to remove entries from kernel
                            m_stateSystemNeighTable->del(state_key);
                        }
                        else
                        {
                            SWSS_LOG_ERROR("Failed to remove voq neighbor %s from SAI during encap index update", kfvKey(t).c_str());
                        }
                        it++;
                    }
                    else
                    {
                        SWSS_LOG_NOTICE("VOQ encap index updated for neighbor %s", kfvKey(t).c_str());
                        it = consumer.m_toSync.erase(it);
                    }
                    continue;
                }

                //Add neigh to SAI
                if (addNeighbor(neighbor_entry, mac_address))
                {
                    //neigh successfully added to SAI. Set STATE DB to signal kernel programming by neighbor manager

                    //If the inband interface type is not VLAN, same MAC can be used for the inband interface for
                    //kernel programming.
                    if(ibif.m_type != Port::VLAN)
                    {
                        mac_address = gMacAddress;

                        // For VS platforms, the mac of the static neigh should not be same as asic's own mac.
                        // This is because host originated packets will have same mac for both src and dst which
                        // will result in host NOT sending packet out. To address this problem which is specific
                        // to port type inband interfaces, set the mac to the neighbor's owner asic's mac. Since
                        // the owner asic's mac is not readily avaiable here, the owner asic mac is derived from
                        // the switch id and lower 5 bytes of asic mac which is assumed to be same for all asics
                        // in the VS system.
                        // Therefore to make VOQ chassis systems work in VS platform based setups like the setups
                        // using KVMs, it is required that all asics have same base mac in the format given below
                        // <lower 5 bytes of mac same for all asics>:<6th byte = switch_id>

                        string platform = getenv("ASIC_VENDOR") ? getenv("ASIC_VENDOR") : "";

                        if (platform == VS_PLATFORM_SUBSTRING)
                        {
                            int8_t sw_id = -1;
                            uint8_t egress_asic_mac[ETHER_ADDR_LEN];

                            gMacAddress.getMac(egress_asic_mac);

                            if (p.m_type == Port::LAG)
                            {
                                sw_id = (int8_t) p.m_system_lag_info.switch_id;
                            }
                            else if (p.m_type == Port::PHY || p.m_type == Port::SYSTEM)
                            {
                                sw_id = (int8_t) p.m_system_port_info.switch_id;
                            }

                            if(sw_id != -1)
                            {
                                egress_asic_mac[5] = sw_id;
                                mac_address = MacAddress(egress_asic_mac);
                            }
                        }
                    }
                    vector<FieldValueTuple> fvVector;
                    FieldValueTuple mac("neigh", mac_address.to_string());
                    fvVector.push_back(mac);
                    m_stateSystemNeighTable->set(state_key, fvVector);

                    it = consumer.m_toSync.erase(it);
                }
                else
                {
                    it++;
                }
            }
            else
            {
                /* Duplicate entry */
                SWSS_LOG_INFO("System neighbor %s already exists", kfvKey(t).c_str());
                it = consumer.m_toSync.erase(it);
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_syncdNeighbors.find(neighbor_entry) != m_syncdNeighbors.end())
            {
                //Remove neigh from SAI
                if (removeNeighbor(neighbor_entry))
                {
                    //neigh successfully deleted from SAI. Set STATE DB to signal to remove entries from kernel
                    m_stateSystemNeighTable->del(state_key);

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

bool NeighOrch::addInbandNeighbor(string alias, IpAddress ip_address)
{
    //Add neighbor record in SAI without adding host route for local inband to avoid route
    //looping for packets destined to the Inband interface if the Inband is port type

    if(gIntfsOrch->isRemoteSystemPortIntf(alias))
    {
        //Remote Inband interface. Skip
        return true;
    }

    sai_status_t status;
    MacAddress inband_mac = gMacAddress;

    sai_object_id_t rif_id = gIntfsOrch->getRouterIntfsId(alias);
    if (rif_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_INFO("Failed to get rif_id for %s", alias.c_str());
        return false;
    }

    //Make the object key
    sai_neighbor_entry_t neighbor_entry;
    neighbor_entry.rif_id = rif_id;
    neighbor_entry.switch_id = gSwitchId;
    copy(neighbor_entry.ip_address, ip_address);

    vector<sai_attribute_t> neighbor_attrs;
    sai_attribute_t attr;
    attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, inband_mac.getMac(), 6);
    neighbor_attrs.push_back(attr);

    //No host route for neighbor of the Inband IP address
    attr.id = SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE;
    attr.value.booldata = true;
    neighbor_attrs.push_back(attr);

    status = sai_neighbor_api->create_neighbor_entry(&neighbor_entry, static_cast<uint32_t>(neighbor_attrs.size()), neighbor_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_ITEM_ALREADY_EXISTS)
        {
            SWSS_LOG_ERROR("Entry exists: neighbor %s on %s, rv:%d", inband_mac.to_string().c_str(), alias.c_str(), status);
            return true;
        }
        else
        {
            SWSS_LOG_ERROR("Failed to create neighbor %s on %s, rv:%d", inband_mac.to_string().c_str(), alias.c_str(), status);
            return false;
        }
    }

    SWSS_LOG_NOTICE("Created inband neighbor %s on %s", inband_mac.to_string().c_str(), alias.c_str());

    gIntfsOrch->increaseRouterIntfsRefCount(alias);

    if (neighbor_entry.ip_address.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEIGHBOR);
    }
    else
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEIGHBOR);
    }

    //Sync the neighbor to add to the CHASSIS_APP_DB
    voqSyncAddNeigh(alias, ip_address, inband_mac, neighbor_entry);

    return true;
}

bool NeighOrch::delInbandNeighbor(string alias, IpAddress ip_address)
{
    // Remove local inband neighbor from SAI

    if(gIntfsOrch->isRemoteSystemPortIntf(alias))
    {
        //Remote Inband interface. Skip
        return true;
    }

    MacAddress inband_mac = gMacAddress;

    sai_object_id_t rif_id = gIntfsOrch->getRouterIntfsId(alias);

    sai_neighbor_entry_t neighbor_entry;
    neighbor_entry.rif_id = rif_id;
    neighbor_entry.switch_id = gSwitchId;
    copy(neighbor_entry.ip_address, ip_address);

    sai_status_t status;
    status = sai_neighbor_api->remove_neighbor_entry(&neighbor_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_ITEM_NOT_FOUND)
        {
            SWSS_LOG_ERROR("Failed to locate neigbor %s on %s, rv:%d", inband_mac.to_string().c_str(), alias.c_str(), status);
            return true;
        }
        else
        {
            SWSS_LOG_ERROR("Failed to remove neighbor %s on %s, rv:%d", inband_mac.to_string().c_str(), alias.c_str(), status);
            return false;
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

    SWSS_LOG_NOTICE("Removed neighbor %s on %s", inband_mac.to_string().c_str(), alias.c_str());

    gIntfsOrch->decreaseRouterIntfsRefCount(alias);

    //Sync the neighbor to delete from the CHASSIS_APP_DB
    voqSyncDelNeigh(alias, ip_address);

    return true;
}

bool NeighOrch::getSystemPortNeighEncapIndex(string &alias, IpAddress &ip, uint32_t &encap_index)
{
    string value;
    string key = alias + m_tableVoqSystemNeighTable->getTableNameSeparator().c_str() + ip.to_string();

    if(m_tableVoqSystemNeighTable->hget(key, "encap_index", value))
    {
        encap_index = (uint32_t) stoul(value);
        return true;
    }
    return false;
}

bool NeighOrch::addVoqEncapIndex(string &alias, IpAddress &ip, vector<sai_attribute_t> &neighbor_attrs)
{
    sai_attribute_t attr;
    uint32_t encap_index = 0;

    if(gIntfsOrch->isRemoteSystemPortIntf(alias))
    {
        if(getSystemPortNeighEncapIndex(alias, ip, encap_index))
        {
            attr.id = SAI_NEIGHBOR_ENTRY_ATTR_ENCAP_INDEX;
            attr.value.u32 = encap_index;
            neighbor_attrs.push_back(attr);

            attr.id = SAI_NEIGHBOR_ENTRY_ATTR_IS_LOCAL;
            attr.value.booldata = false;
            neighbor_attrs.push_back(attr);
        }
        else
        {
            //Encap index not available and the interface is remote. Return false to re-try
            SWSS_LOG_NOTICE("System port neigh encap index not available for %s|%s!", alias.c_str(), ip.to_string().c_str());
            return false;
        }
    }

    return true;
}

void NeighOrch::voqSyncAddNeigh(string &alias, IpAddress &ip_address, const MacAddress &mac, sai_neighbor_entry_t &neighbor_entry)
{
    sai_attribute_t attr;
    sai_status_t status;

    // Get the encap index and store it for handling change of
    // encap index for remote neighbors synced via CHASSIS_APP_DB

    attr.id = SAI_NEIGHBOR_ENTRY_ATTR_ENCAP_INDEX;

    status = sai_neighbor_api->get_neighbor_entry_attribute(&neighbor_entry, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get neighbor attribute for %s on %s, rv:%d", ip_address.to_string().c_str(), alias.c_str(), status);
        task_process_status handle_status = handleSaiGetStatus(SAI_API_NEIGHBOR, status);
        if (handle_status != task_process_status::task_success)
        {
            return;
        }
    }

    if (!attr.value.u32)
    {
        SWSS_LOG_ERROR("Invalid neighbor encap_index for %s on %s", ip_address.to_string().c_str(), alias.c_str());
        return;
    }

    NeighborEntry nbrEntry = {ip_address, alias};
    m_syncdNeighbors[nbrEntry].voq_encap_index = attr.value.u32;

    //Sync only local neigh. Confirm for the local neigh and
    //get the system port alias for key for syncing to CHASSIS_APP_DB
    Port port;
    if(gPortsOrch->getPort(alias, port))
    {
        if (port.m_type == Port::LAG)
        {
            if (port.m_system_lag_info.switch_id != gVoqMySwitchId)
            {
                return;
            }
            alias = port.m_system_lag_info.alias;
        }
        else
        {
            if(port.m_system_port_info.type == SAI_SYSTEM_PORT_TYPE_REMOTE)
            {
                return;
            }
            alias = port.m_system_port_info.alias;
        }
    }
    else
    {
        SWSS_LOG_ERROR("Port does not exist for %s!", alias.c_str());
        return;
    }

    vector<FieldValueTuple> attrs;

    FieldValueTuple eiFv ("encap_index", to_string(attr.value.u32));
    attrs.push_back(eiFv);

    FieldValueTuple macFv ("neigh", mac.to_string());
    attrs.push_back(macFv);

    string key = alias + m_tableVoqSystemNeighTable->getTableNameSeparator().c_str() + ip_address.to_string();
    m_tableVoqSystemNeighTable->set(key, attrs);
}

void NeighOrch::voqSyncDelNeigh(string &alias, IpAddress &ip_address)
{
    //Sync only local neigh. Confirm for the local neigh and
    //get the system port alias for key for syncing to CHASSIS_APP_DB
    Port port;
    if(gPortsOrch->getPort(alias, port))
    {
        if (port.m_type == Port::LAG)
        {
            if (port.m_system_lag_info.switch_id != gVoqMySwitchId)
            {
                return;
            }
            alias = port.m_system_lag_info.alias;
        }
        else
        {
            if(port.m_system_port_info.type == SAI_SYSTEM_PORT_TYPE_REMOTE)
            {
                return;
            }
            alias = port.m_system_port_info.alias;
        }
    }
    else
    {
        SWSS_LOG_ERROR("Port does not exist for %s!", alias.c_str());
        return;
    }

    string key = alias + m_tableVoqSystemNeighTable->getTableNameSeparator().c_str() + ip_address.to_string();
    m_tableVoqSystemNeighTable->del(key);
}

bool NeighOrch::updateVoqNeighborEncapIndex(const NeighborEntry &neighborEntry, uint32_t encap_index)
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

    sai_attribute_t neighbor_attr;
    neighbor_attr.id = SAI_NEIGHBOR_ENTRY_ATTR_ENCAP_INDEX;
    neighbor_attr.value.u32 = encap_index;

    status = sai_neighbor_api->set_neighbor_entry_attribute(&neighbor_entry, &neighbor_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to update voq encap index for neighbor %s on %s, rv:%d",
                       ip_address.to_string().c_str(), alias.c_str(), status);
        return false;
    }

    SWSS_LOG_NOTICE("Updated voq encap index for neighbor %s on %s", ip_address.to_string().c_str(), alias.c_str());

    m_syncdNeighbors[neighborEntry].voq_encap_index = encap_index;

    return true;
}

void NeighOrch::updateSrv6Nexthop(const NextHopKey &nh, const sai_object_id_t &nh_id)
{
    if (nh_id != SAI_NULL_OBJECT_ID)
    {
        NextHopEntry next_hop_entry;
        next_hop_entry.next_hop_id = nh_id;
        next_hop_entry.ref_count = 0;
        next_hop_entry.nh_flags = 0;
        m_syncdNextHops[nh] = next_hop_entry;
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_SRV6_NEXTHOP);
    }
    else
    {
        assert(m_syncdNextHops[nh].ref_count == 0);
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_SRV6_NEXTHOP);
        m_syncdNextHops.erase(nh);
    }
}
void NeighOrch::addZeroMacTunnelRoute(const NeighborEntry& entry, const MacAddress& mac)
{
    SWSS_LOG_INFO("Creating tunnel route for neighbor %s", entry.ip_address.to_string().c_str());
    MuxOrch* mux_orch = gDirectory.get<MuxOrch*>();
    NeighborUpdate update = {entry, mac, true};
    mux_orch->update(SUBJECT_TYPE_NEIGH_CHANGE, static_cast<void *>(&update));
    m_syncdNeighbors[entry] = { mac, false };
}

