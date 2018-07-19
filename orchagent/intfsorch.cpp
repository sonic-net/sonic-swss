#include <cassert>
#include <fstream>
#include <sstream>
#include <map>
#include <net/if.h>

#include "intfsorch.h"
#include "ipprefix.h"
#include "logger.h"
#include "swssnet.h"
#include "tokenize.h"
#include "crmorch.h"

extern sai_object_id_t gVirtualRouterId;

extern sai_router_interface_api_t*  sai_router_intfs_api;
extern sai_route_api_t*             sai_route_api;
extern sai_neighbor_api_t*          sai_neighbor_api;

extern PortsOrch *gPortsOrch;
extern sai_object_id_t gSwitchId;
extern CrmOrch *gCrmOrch;

const int intfsorch_pri = 35;

IntfsOrch::IntfsOrch(DBConnector *db, string tableName) :
        Orch(db, tableName, intfsorch_pri)
{
    SWSS_LOG_ENTER();
}

sai_object_id_t IntfsOrch::getRouterIntfsId(const string &alias)
{
    Port port;
    gPortsOrch->getPort(alias, port);
    assert(port.m_rif_id);
    return port.m_rif_id;
}

void IntfsOrch::increaseRouterIntfsRefCount(const string &alias)
{
    SWSS_LOG_ENTER();

    m_syncdIntfses[alias].ref_count++;
    SWSS_LOG_DEBUG("Router interface %s ref count is increased to %d",
                  alias.c_str(), m_syncdIntfses[alias].ref_count);
}

void IntfsOrch::decreaseRouterIntfsRefCount(const string &alias)
{
    SWSS_LOG_ENTER();

    m_syncdIntfses[alias].ref_count--;
    SWSS_LOG_DEBUG("Router interface %s ref count is decreased to %d",
                  alias.c_str(), m_syncdIntfses[alias].ref_count);
}

void IntfsOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->isInitDone())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        vector<string> keys = tokenize(kfvKey(t), ':');
        string alias(keys[0]);
        IpPrefix ip_prefix(kfvKey(t).substr(kfvKey(t).find(':')+1));

        if (alias == "eth0" || alias == "docker0")
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        string op = kfvOp(t);
        if (op == SET_COMMAND)
        {
            Port port;
            if (!gPortsOrch->getPort(alias, port))
            {
                /* TODO: Resolve the dependency relationship and add ref_count to port */
                it++;
                continue;
            }

            auto it_intfs = m_syncdIntfses.find(alias);
            if (it_intfs == m_syncdIntfses.end())
            {
                /* Do not create router interface when it is a loopback interface */
                if (port.m_type == Port::LOOPBACK || addRouterIntfs(port))
                {
                    IntfsEntry intfs_entry;
                    intfs_entry.ref_count = 0;
                    m_syncdIntfses[alias] = intfs_entry;
                }
                else
                {
                    it++;
                    continue;
                }
            }

            if (m_syncdIntfses[alias].ip_addresses.count(ip_prefix))
            {
                /* Duplicate entry */
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /* NOTE: Overlap checking is required to handle ifconfig weird behavior.
             * When set IP address using ifconfig command it applies it in two stages.
             * On stage one it sets IP address with netmask /8. On stage two it
             * changes netmask to specified in command. As DB is async event to
             * add IP address with original netmask may come before event to
             * delete IP with netmask /8. To handle this we in case of overlap
             * we should wait until entry with /8 netmask will be removed.
             * Time frame between those event is quite small.*/
            bool overlaps = false;
            for (const auto &prefixIt: m_syncdIntfses[alias].ip_addresses)
            {
                if (prefixIt.isAddressInSubnet(ip_prefix.getIp()) ||
                        ip_prefix.isAddressInSubnet(prefixIt.getIp()))
                {
                    overlaps = true;
                    SWSS_LOG_NOTICE("Router interface %s IP %s overlaps with %s.", port.m_alias.c_str(),
                            prefixIt.to_string().c_str(), ip_prefix.to_string().c_str());
                    break;
                }
            }

            if (overlaps)
            {
                /* Overlap of IP address network */
                ++it;
                continue;
            }

            addIp2MeRoute(ip_prefix);

            /* Add subnet route for non-loopback interfaces */
            if (port.m_type != Port::LOOPBACK)
            {
                addSubnetRoute(port, ip_prefix);
            }

            /* Add directed broadcast route for VLAN IPv4 interfaces */
            if (port.m_type == Port::VLAN && ip_prefix.isV4())
            {
                addDirectedBroadcast(port, ip_prefix.getBroadcastIp());
            }

            m_syncdIntfses[alias].ip_addresses.insert(ip_prefix);
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            if (alias == "lo")
            {
                removeIp2MeRoute(ip_prefix);
                it = consumer.m_toSync.erase(it);
                continue;
            }

            Port port;
            /* Cannot locate interface */
            if (!gPortsOrch->getPort(alias, port))
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (m_syncdIntfses.find(alias) != m_syncdIntfses.end())
            {
                if (m_syncdIntfses[alias].ip_addresses.count(ip_prefix))
                {
                    removeSubnetRoute(port, ip_prefix);
                    removeIp2MeRoute(ip_prefix);
                    if(port.m_type == Port::VLAN && ip_prefix.isV4())
                    {
                        removeDirectedBroadcast(port, ip_prefix.getBroadcastIp());
                    }

                    m_syncdIntfses[alias].ip_addresses.erase(ip_prefix);
                }

                /* Remove router interface that no IP addresses are associated with */
                if (m_syncdIntfses[alias].ip_addresses.size() == 0)
                {
                    if (removeRouterIntfs(port))
                    {
                        m_syncdIntfses.erase(alias);
                        it = consumer.m_toSync.erase(it);
                    }
                    else
                        it++;
                }
                else
                {
                    it = consumer.m_toSync.erase(it);
                }
            }
            else
                /* Cannot locate the interface */
                it = consumer.m_toSync.erase(it);
        }
    }
}

bool IntfsOrch::addRouterIntfs(Port &port)
{
    SWSS_LOG_ENTER();

    /* Return true if the router interface exists */
    if (port.m_rif_id)
    {
        SWSS_LOG_WARN("Router interface already exists on %s",
                      port.m_alias.c_str());
        return true;
    }

    /* Create router interface if the router interface doesn't exist */
    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    attr.value.oid = gVirtualRouterId;
    attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, gMacAddress.getMac(), sizeof(sai_mac_t));
    attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    switch(port.m_type)
    {
        case Port::PHY:
        case Port::LAG:
            attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
            break;
        case Port::VLAN:
            attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_VLAN;
            break;
        default:
            SWSS_LOG_ERROR("Unsupported port type: %d", port.m_type);
            break;
    }
    attrs.push_back(attr);

    switch(port.m_type)
    {
        case Port::PHY:
            attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
            attr.value.oid = port.m_port_id;
            break;
        case Port::LAG:
            attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
            attr.value.oid = port.m_lag_id;
            break;
        case Port::VLAN:
            attr.id = SAI_ROUTER_INTERFACE_ATTR_VLAN_ID;
            attr.value.oid = port.m_vlan_info.vlan_oid;
            break;
        default:
            SWSS_LOG_ERROR("Unsupported port type: %d", port.m_type);
            break;
    }
    attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
    attr.value.u32 = port.m_mtu;
    attrs.push_back(attr);

    sai_status_t status = sai_router_intfs_api->create_router_interface(&port.m_rif_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create router interface for port %s, rv:%d", port.m_alias.c_str(), status);
        throw runtime_error("Failed to create router interface.");
    }

    gPortsOrch->setPort(port.m_alias, port);

    SWSS_LOG_NOTICE("Create router interface for port %s mtu %u", port.m_alias.c_str(), port.m_mtu);

    return true;
}

bool IntfsOrch::removeRouterIntfs(Port &port)
{
    SWSS_LOG_ENTER();

    if (m_syncdIntfses[port.m_alias].ref_count > 0)
    {
        SWSS_LOG_NOTICE("Router interface is still referenced");
        return false;
    }

    sai_status_t status = sai_router_intfs_api->remove_router_interface(port.m_rif_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove router interface for port %s, rv:%d", port.m_alias.c_str(), status);
        throw runtime_error("Failed to remove router interface.");
    }

    port.m_rif_id = 0;
    gPortsOrch->setPort(port.m_alias, port);

    SWSS_LOG_NOTICE("Remove router interface for port %s", port.m_alias.c_str());

    return true;
}

void IntfsOrch::addSubnetRoute(const Port &port, const IpPrefix &ip_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = gVirtualRouterId;
    copy(unicast_route_entry.destination, ip_prefix);
    subnet(unicast_route_entry.destination, unicast_route_entry.destination);

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
    attrs.push_back(attr);

    attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    attr.value.oid = port.m_rif_id;
    attrs.push_back(attr);

    sai_status_t status = sai_route_api->create_route_entry(&unicast_route_entry, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create subnet route to %s from %s, rv:%d",
                       ip_prefix.to_string().c_str(), port.m_alias.c_str(), status);
        throw runtime_error("Failed to create subnet route.");
    }

    SWSS_LOG_NOTICE("Create subnet route to %s from %s",
                    ip_prefix.to_string().c_str(), port.m_alias.c_str());
    increaseRouterIntfsRefCount(port.m_alias);

    if (unicast_route_entry.destination.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
    }
    else
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
    }
}

void IntfsOrch::removeSubnetRoute(const Port &port, const IpPrefix &ip_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = gVirtualRouterId;
    copy(unicast_route_entry.destination, ip_prefix);
    subnet(unicast_route_entry.destination, unicast_route_entry.destination);

    sai_status_t status = sai_route_api->remove_route_entry(&unicast_route_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove subnet route to %s from %s, rv:%d",
                       ip_prefix.to_string().c_str(), port.m_alias.c_str(), status);
        throw runtime_error("Failed to remove subnet route.");
    }

    SWSS_LOG_NOTICE("Remove subnet route to %s from %s",
                    ip_prefix.to_string().c_str(), port.m_alias.c_str());
    decreaseRouterIntfsRefCount(port.m_alias);

    if (unicast_route_entry.destination.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
    }
    else
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
    }
}

void IntfsOrch::addIp2MeRoute(const IpPrefix &ip_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = gVirtualRouterId;
    copy(unicast_route_entry.destination, ip_prefix.getIp());

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
    attrs.push_back(attr);

    Port cpu_port;
    gPortsOrch->getCpuPort(cpu_port);

    attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    attr.value.oid = cpu_port.m_port_id;
    attrs.push_back(attr);

    sai_status_t status = sai_route_api->create_route_entry(&unicast_route_entry, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create IP2me route ip:%s, rv:%d", ip_prefix.getIp().to_string().c_str(), status);
        throw runtime_error("Failed to create IP2me route.");
    }

    SWSS_LOG_NOTICE("Create IP2me route ip:%s", ip_prefix.getIp().to_string().c_str());

    if (unicast_route_entry.destination.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
    }
    else
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
    }
}

void IntfsOrch::removeIp2MeRoute(const IpPrefix &ip_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = gVirtualRouterId;
    copy(unicast_route_entry.destination, ip_prefix.getIp());

    sai_status_t status = sai_route_api->remove_route_entry(&unicast_route_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove IP2me route ip:%s, rv:%d", ip_prefix.getIp().to_string().c_str(), status);
        throw runtime_error("Failed to remove IP2me route.");
    }

    SWSS_LOG_NOTICE("Remove packet action trap route ip:%s", ip_prefix.getIp().to_string().c_str());

    if (unicast_route_entry.destination.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
    }
    else
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
    }
}

void IntfsOrch::addDirectedBroadcast(const Port &port, const IpAddress &ip_addr)
{
    sai_status_t status;
    sai_neighbor_entry_t neighbor_entry;
    neighbor_entry.rif_id = port.m_rif_id;
    neighbor_entry.switch_id = gSwitchId;
    copy(neighbor_entry.ip_address, ip_addr);

    sai_attribute_t neighbor_attr;
    neighbor_attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
    memcpy(neighbor_attr.value.mac, MacAddress("ff:ff:ff:ff:ff:ff").getMac(), 6);

    status = sai_neighbor_api->create_neighbor_entry(&neighbor_entry, 1, &neighbor_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create broadcast entry %s rv:%d",
                       ip_addr.to_string().c_str(), status);
        return;
    }

    SWSS_LOG_NOTICE("Add broadcast route for ip:%s", ip_addr.to_string().c_str());
}

void IntfsOrch::removeDirectedBroadcast(const Port &port, const IpAddress &ip_addr)
{
    sai_status_t status;
    sai_neighbor_entry_t neighbor_entry;
    neighbor_entry.rif_id = port.m_rif_id;
    neighbor_entry.switch_id = gSwitchId;
    copy(neighbor_entry.ip_address, ip_addr);

    status = sai_neighbor_api->remove_neighbor_entry(&neighbor_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_ITEM_NOT_FOUND)
        {
            SWSS_LOG_ERROR("No broadcast entry found for %s", ip_addr.to_string().c_str());
        }
        else
        {
            SWSS_LOG_ERROR("Failed to remove broadcast entry %s rv:%d",
                           ip_addr.to_string().c_str(), status);
        }
        return;
    }

    SWSS_LOG_NOTICE("Remove broadcast route ip:%s", ip_addr.to_string().c_str());
}
