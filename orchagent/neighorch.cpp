#include <assert.h>
#include "neighorch.h"
#include "logger.h"
#include "swssnet.h"
#include "crmorch.h"
#include "routeorch.h"
#include "errororch.h"
#include "tokenize.h"
#include "sai_serialize.h"
#include "redisclient.h"

#include "swss/json.hpp"
using json = nlohmann::json;

extern sai_neighbor_api_t*         sai_neighbor_api;
extern sai_next_hop_api_t*         sai_next_hop_api;

extern PortsOrch *gPortsOrch;
extern sai_object_id_t gSwitchId;
extern CrmOrch *gCrmOrch;
extern RouteOrch *gRouteOrch;
extern ErrorOrch *gErrorOrch;
extern std::shared_ptr<swss::RedisClient>   g_redisClientAsicDb;
extern std::shared_ptr<swss::RedisClient>   g_redisClientCountersDb;

const int neighorch_pri = 30;

NeighOrch::NeighOrch(DBConnector *db, string tableName, IntfsOrch *intfsOrch) :
        Orch(db, tableName, neighorch_pri), m_intfsOrch(intfsOrch)
{
    SWSS_LOG_ENTER();

    if(gErrorOrch->mappingHandlerRegister(APP_NEIGH_TABLE_NAME, this) == false)
    {
        SWSS_LOG_ERROR("Failed to register with Error Handling Framework for %s",
                APP_NEIGH_TABLE_NAME);
    }
}

bool NeighOrch::hasNextHop(const NextHopKey &nexthop)
{
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
        return false;
    }

    SWSS_LOG_NOTICE("Created next hop %s on %s",
                    ipAddress.to_string().c_str(), alias.c_str());

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

    // For nexthop with incoming port which has down oper status, NHFLAGS_IFDOWN
    // flag Should be set on it.
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

    switch (nh_flag)
    {
        case NHFLAGS_IFDOWN:
            rc = gRouteOrch->invalidnexthopinNextHopGroup(nexthop);
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

    switch (nh_flag)
    {
        case NHFLAGS_IFDOWN:
            rc = gRouteOrch->validnexthopinNextHopGroup(nexthop);
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

    assert(nhop != m_syncdNextHops.end());

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

sai_object_id_t NeighOrch::getNextHopId(const NextHopKey &nexthop)
{
    assert(hasNextHop(nexthop));
    return m_syncdNextHops[nexthop].next_hop_id;
}

int NeighOrch::getNextHopRefCount(const NextHopKey &nexthop)
{
    assert(hasNextHop(nexthop));
    return m_syncdNextHops[nexthop].ref_count;
}

void NeighOrch::increaseNextHopRefCount(const NextHopKey &nexthop)
{
    assert(hasNextHop(nexthop));
    m_syncdNextHops[nexthop].ref_count ++;
}

void NeighOrch::decreaseNextHopRefCount(const NextHopKey &nexthop)
{
    assert(hasNextHop(nexthop));
    m_syncdNextHops[nexthop].ref_count --;
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
            macAddress = entry.second;
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

            if (m_syncdNeighbors.find(neighbor_entry) == m_syncdNeighbors.end() || m_syncdNeighbors[neighbor_entry] != mac_address)
            {
                if (addNeighbor(neighbor_entry, mac_address))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else
                /* Duplicate entry */
                it = consumer.m_toSync.erase(it);
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

    sai_attribute_t neighbor_attr;
    neighbor_attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
    memcpy(neighbor_attr.value.mac, macAddress.getMac(), 6);

    if (m_syncdNeighbors.find(neighborEntry) == m_syncdNeighbors.end())
    {
        status = sai_neighbor_api->create_neighbor_entry(&neighbor_entry, 1, &neighbor_attr);
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
                return false;
            }
        }

        SWSS_LOG_NOTICE("Created neighbor %s on %s", macAddress.to_string().c_str(), alias.c_str());
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
                return false;
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
    }
    else
    {
        status = sai_neighbor_api->set_neighbor_entry_attribute(&neighbor_entry, &neighbor_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to update neighbor %s on %s, rv:%d",
                           macAddress.to_string().c_str(), alias.c_str(), status);
            return false;
        }
        SWSS_LOG_NOTICE("Updated neighbor %s on %s", macAddress.to_string().c_str(), alias.c_str());
    }

    m_syncdNeighbors[neighborEntry] = macAddress;

    NeighborUpdate update = { neighborEntry, macAddress, true };
    notify(SUBJECT_TYPE_NEIGH_CHANGE, static_cast<void *>(&update));

    return true;
}

bool NeighOrch::removeNeighbor(const NeighborEntry &neighborEntry)
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

    if (m_syncdNextHops[nexthop].ref_count > 0)
    {
        SWSS_LOG_INFO("Failed to remove still referenced neighbor %s on %s",
                      m_syncdNeighbors[neighborEntry].to_string().c_str(), alias.c_str());
        return false;
    }

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
            return false;
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
            SWSS_LOG_ERROR("Failed to locate neigbor %s on %s, rv:%d",
                    m_syncdNeighbors[neighborEntry].to_string().c_str(), alias.c_str(), status);
            return true;
        }
        else
        {
            SWSS_LOG_ERROR("Failed to remove neighbor %s on %s, rv:%d",
                    m_syncdNeighbors[neighborEntry].to_string().c_str(), alias.c_str(), status);
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

    SWSS_LOG_NOTICE("Removed neighbor %s on %s",
            m_syncdNeighbors[neighborEntry].to_string().c_str(), alias.c_str());

    m_syncdNeighbors.erase(neighborEntry);
    m_intfsOrch->decreaseRouterIntfsRefCount(alias);

    NeighborUpdate update = { neighborEntry, MacAddress(), false };
    notify(SUBJECT_TYPE_NEIGH_CHANGE, static_cast<void *>(&update));

    removeNextHop(ip_address, alias);

    return true;
}

bool NeighOrch::mapToErrorDbFormat(sai_object_type_t& object_type, std::vector<FieldValueTuple> &asicValues,
        std::vector<FieldValueTuple> &appValues)
{
    SWSS_LOG_ENTER();

    if(object_type != SAI_OBJECT_TYPE_NEIGHBOR_ENTRY)
    {
        return false;
    }

    /*
       127.0.0.1:6379> hgetall "NEIGH_TABLE:Ethernet0:2.2.2.2"
       1) "neigh"
       2) "00:00:3a:3e:9e:a7"
       3) "family"
       4) "IPv4"

       127.0.0.1:6379[1]> hgetall "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY:{\"ip\":\"2.2.2.2\",\"rif\":\"oid:0x60000000006f3\",\"switch_id\":\"oid:0x21000000000000\"}"
       1) "SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS"
       2) "00:00:3A:3E:9E:A7"
       127.0.0.1:6379[1]>

       127.0.0.1:6379[2]> hgetall "COUNTERS_PORT_NAME_MAP"
       25) "Ethernet0"
       26) "oid:0x100000000000e"
    */

    const auto& values = asicValues;
    std::string asicKV, strNbrIP, strRifOid, strMac;
    std::string strIntfName, strRtrIntfType;
    for (size_t i = 0; i < values.size(); i++)
    {
        if(fvField(values[i]) == "key")
        {
            /* Extract Neighbor IP and Router Interface ID from the "key" field */
            asicKV = fvValue(values[i]);
            auto tokens = tokenize(asicKV, ':', 1);
            json j = json::parse(tokens[1]);
            strNbrIP = j["ip"];
            strRifOid = j["rif"];
            SWSS_LOG_DEBUG("Neighbor IP is %s, router interface ID is %s",
                    strNbrIP.c_str(), strRifOid.c_str());

            /* Extract Port OID from Router Interface OID */
            std::string strRtrIfKey = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE:" + strRifOid;
            std::string strIntfOid;
            /* OID in neighbor entry points to ROUTER_INTERFACE
             * Port based routing interface
             * ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x60000000006d9
             *   SAI_ROUTER_INTERFACE_ATTR_TYPE = SAI_ROUTER_INTERFACE_TYPE_PORT
             *   SAI_ROUTER_INTERFACE_ATTR_PORT_ID = oid:0x100000000000e
             *   Check above oid in COUNTERS_PORT_NAME_MAP in COUNTERS_DB to get the physical interface name
             *
             * VLAN based routing interface
             * ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x60000000006f6
             *  SAI_ROUTER_INTERFACE_ATTR_TYPE -> SAI_ROUTER_INTERFACE_TYPE_VLAN
             *  SAI_ROUTER_INTERFACE_ATTR_VLAN_ID -> oid:0x260000000006f3
             *  Check above oid in "ASIC_STATE:SAI_OBJECT_TYPE_VLAN:oid:0x260000000006f3"
             */

            auto hashRif = g_redisClientAsicDb->hgetall(strRtrIfKey);
            for (auto &kv: hashRif)
            {
                const std::string &skey = kv.first;
                const std::string &svalue = kv.second;

                if(skey == "SAI_ROUTER_INTERFACE_ATTR_TYPE")
                {
                    strRtrIntfType = svalue;
                }
                if(skey == "SAI_ROUTER_INTERFACE_ATTR_PORT_ID" || skey == "SAI_ROUTER_INTERFACE_ATTR_VLAN_ID")
                {
                    strIntfOid = svalue;
                }
            }
            SWSS_LOG_DEBUG("Router interface type is %s, interface ID is %s",
                    strRtrIntfType.c_str(), strIntfOid.c_str());

            /* Extract Port name from the Port OID */
            if(strRtrIntfType == "SAI_ROUTER_INTERFACE_TYPE_PORT")
            {
                auto hashCntr = g_redisClientCountersDb->hgetall("COUNTERS_PORT_NAME_MAP");
                for (auto &kv: hashCntr)
                {
                    const std::string &skey = kv.first;
                    const std::string &svalue = kv.second;
                    if(svalue == strIntfOid)
                    {
                        strIntfName = skey;
                        break;
                    }
                }
            }
            else if (strRtrIntfType == "SAI_ROUTER_INTERFACE_TYPE_VLAN")
            {
                std::string strVlanKey = "ASIC_STATE:SAI_OBJECT_TYPE_VLAN:" + strIntfOid;
                auto hashVlan = g_redisClientAsicDb->hgetall(strVlanKey);
                for (auto &kv: hashVlan)
                {
                    const std::string &skey = kv.first;
                    const std::string &svalue = kv.second;
                    if(skey == "SAI_VLAN_ATTR_VLAN_ID")
                    {
                        strIntfName = "Vlan" + svalue;
                        break;
                    }
                }
            }
            SWSS_LOG_DEBUG("Interface name is %s", strIntfName.c_str());
        } /* End of if(fvField(values[i]) == "key") */

        /* Extract MAC address */
        if(fvField(values[i]) == "SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS")
        {
            strMac = fvValue(values[i]);
        }
    } /* End of for (size_t i = 0; i < values.size(); i++) */

    std::string appKey = strIntfName + ":" + strNbrIP;
    appValues.emplace_back("key", appKey);
    appValues.emplace_back("neigh", strMac);

    return true;
}

