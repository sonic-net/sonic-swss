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
#include "routeorch.h"
#include "crmorch.h"
#include "bufferorch.h"
#include "directory.h"
#include "vnetorch.h"

extern sai_object_id_t gVirtualRouterId;
extern Directory<Orch*> gDirectory;

extern sai_router_interface_api_t*  sai_router_intfs_api;
extern sai_route_api_t*             sai_route_api;
extern sai_neighbor_api_t*          sai_neighbor_api;

extern sai_object_id_t gSwitchId;
extern PortsOrch *gPortsOrch;
extern RouteOrch *gRouteOrch;
extern CrmOrch *gCrmOrch;
extern BufferOrch *gBufferOrch;

const int intfsorch_pri = 35;

IntfsOrch::IntfsOrch(DBConnector *db, string tableName, VRFOrch *vrf_orch) :
        Orch(db, tableName, intfsorch_pri), m_vrfOrch(vrf_orch)
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

bool IntfsOrch::setRouterIntfsMtu(Port &port)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
    attr.value.u32 = port.m_mtu;

    sai_status_t status = sai_router_intfs_api->
            set_router_interface_attribute(port.m_rif_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set router interface %s MTU to %u, rv:%d",
                port.m_alias.c_str(), port.m_mtu, status);
        return false;
    }
    SWSS_LOG_NOTICE("Set router interface %s MTU to %u",
            port.m_alias.c_str(), port.m_mtu);
    return true;
}

set<IpPrefix> IntfsOrch:: getSubnetRoutes()
{
    SWSS_LOG_ENTER();

    set<IpPrefix> subnet_routes;

    for (auto it = m_syncdIntfses.begin(); it != m_syncdIntfses.end(); it++)
    {
        for (auto prefix : it->second.ip_addresses)
        {
            subnet_routes.emplace(prefix);
        }
    }

    return subnet_routes;
}

bool IntfsOrch::setIntf(const string& alias, sai_object_id_t vrf_id, const IpPrefix *ip_prefix)
{
    SWSS_LOG_ENTER();

    Port port;
    gPortsOrch->getPort(alias, port);

    auto it_intfs = m_syncdIntfses.find(alias);
    if (it_intfs == m_syncdIntfses.end())
    {
        if (addRouterIntfs(vrf_id, port))
        {
            IntfsEntry intfs_entry;
            intfs_entry.ref_count = 0;
            m_syncdIntfses[alias] = intfs_entry;
        }
        else
        {
            return false;
        }
    }

    if (!ip_prefix || m_syncdIntfses[alias].ip_addresses.count(*ip_prefix))
    {
        /* Request to create router interface, no prefix present or Duplicate entry */
        return true;
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
        if (prefixIt.isAddressInSubnet(ip_prefix->getIp()) ||
                ip_prefix->isAddressInSubnet(prefixIt.getIp()))
        {
            overlaps = true;
            SWSS_LOG_NOTICE("Router interface %s IP %s overlaps with %s.", port.m_alias.c_str(),
                    prefixIt.to_string().c_str(), ip_prefix->to_string().c_str());
            break;
        }
    }

    if (overlaps)
    {
        /* Overlap of IP address network */
        return false;
    }

    addSubnetRoute(port, *ip_prefix);
    addIp2MeRoute(vrf_id, *ip_prefix);

    if (port.m_type == Port::VLAN && ip_prefix->isV4())
    {
        addDirectedBroadcast(port, ip_prefix->getBroadcastIp());
    }

    return true;
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
        IpPrefix ip_prefix;
        bool ip_prefix_in_key = false;

        if (keys.size() > 1)
        {
            ip_prefix = kfvKey(t).substr(kfvKey(t).find(':')+1);
            ip_prefix_in_key = true;
        }

        const vector<FieldValueTuple>& data = kfvFieldsValues(t);
        string vrf_name = "", vnet_name = "";

        for (auto idx : data)
        {
            const auto &field = fvField(idx);
            const auto &value = fvValue(idx);
            if (field == "vrf_name")
            {
                vrf_name = value;
            }
            else if (field == "vnet_name")
            {
                vnet_name = value;
            }
        }

        if (alias == "eth0" || alias == "docker0")
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        sai_object_id_t vrf_id = gVirtualRouterId;
        if (!vrf_name.empty())
        {
            if (m_vrfOrch->isVRFexists(vrf_name))
            {
                it++;
                continue;
            }
            vrf_id = m_vrfOrch->getVRFid(vrf_name);
        }

        string op = kfvOp(t);
        if (op == SET_COMMAND)
        {
            if (alias == "lo")
            {
                if (!ip_prefix_in_key)
                {
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                bool addIp2Me = false;
                // set request for lo may come after warm start restore.
                // It is also to prevent dupicate set requests in normal running case.
                auto it_intfs = m_syncdIntfses.find(alias);
                if (it_intfs == m_syncdIntfses.end())
                {
                    IntfsEntry intfs_entry;

                    intfs_entry.ref_count = 0;
                    intfs_entry.ip_addresses.insert(ip_prefix);
                    m_syncdIntfses[alias] = intfs_entry;
                    addIp2Me = true;
                }
                else
                {
                     if (m_syncdIntfses[alias].ip_addresses.count(ip_prefix) == 0)
                     {
                        m_syncdIntfses[alias].ip_addresses.insert(ip_prefix);
                        addIp2Me = true;
                     }
                }
                if (addIp2Me)
                {
                    addIp2MeRoute(vrf_id, ip_prefix);
                }

                it = consumer.m_toSync.erase(it);
                continue;
            }

            Port port;
            if (!gPortsOrch->getPort(alias, port))
            {
                /* TODO: Resolve the dependency relationship and add ref_count to port */
                it++;
                continue;
            }

            // buffer configuration hasn't been applied yet, hold from intf config.
            if (!gBufferOrch->isPortReady(alias))
            {
                it++;
                continue;
            }

            if (!vnet_name.empty())
            {
                VNetOrch* vnet_orch = gDirectory.get<VNetOrch*>();
                if (!vnet_orch->isVnetExists(vnet_name))
                {
                    it++;
                    continue;
                }
                if (!vnet_orch->setIntf(alias, vnet_name, ip_prefix_in_key ? &ip_prefix : nullptr))
                {
                    it++;
                    continue;
                }
            }
            else
            {
                if (!setIntf(alias, vrf_id, ip_prefix_in_key ? &ip_prefix : nullptr))
                {
                    it++;
                    continue;
                }
            }

            m_syncdIntfses[alias].ip_addresses.insert(ip_prefix);
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            if (alias == "lo")
            {
                // TODO: handle case for which lo is not in default vrf gVirtualRouterId
                if (m_syncdIntfses.find(alias) != m_syncdIntfses.end())
                {
                    if (m_syncdIntfses[alias].ip_addresses.count(ip_prefix))
                    {
                        m_syncdIntfses[alias].ip_addresses.erase(ip_prefix);
                        removeIp2MeRoute(vrf_id, ip_prefix);
                    }
                    if (m_syncdIntfses[alias].ip_addresses.size() == 0)
                    {
                        m_syncdIntfses.erase(alias);
                    }
                }

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

            vrf_id = port.m_vr_id;
            if (m_syncdIntfses.find(alias) != m_syncdIntfses.end())
            {
                if (m_syncdIntfses[alias].ip_addresses.count(ip_prefix))
                {
                    removeSubnetRoute(port, ip_prefix);
                    removeIp2MeRoute(vrf_id, ip_prefix);
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

bool IntfsOrch::addRouterIntfs(sai_object_id_t vrf_id, Port &port)
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
    attr.value.oid = vrf_id;
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
        SWSS_LOG_ERROR("Failed to create router interface %s, rv:%d",
                port.m_alias.c_str(), status);
        throw runtime_error("Failed to create router interface.");
    }

    port.m_vr_id = vrf_id;

    gPortsOrch->setPort(port.m_alias, port);

    SWSS_LOG_NOTICE("Create router interface %s MTU %u", port.m_alias.c_str(), port.m_mtu);

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
    port.m_vr_id = 0;
    gPortsOrch->setPort(port.m_alias, port);

    SWSS_LOG_NOTICE("Remove router interface for port %s", port.m_alias.c_str());

    return true;
}

void IntfsOrch::addSubnetRoute(const Port &port, const IpPrefix &ip_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = port.m_vr_id;
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

    gRouteOrch->notifyNextHopChangeObservers(ip_prefix, IpAddresses(), true);
}

void IntfsOrch::removeSubnetRoute(const Port &port, const IpPrefix &ip_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = port.m_vr_id;
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

    gRouteOrch->notifyNextHopChangeObservers(ip_prefix, IpAddresses(), false);
}

void IntfsOrch::addIp2MeRoute(sai_object_id_t vrf_id, const IpPrefix &ip_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = vrf_id;
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

void IntfsOrch::removeIp2MeRoute(sai_object_id_t vrf_id, const IpPrefix &ip_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = vrf_id;
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
