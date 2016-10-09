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

extern sai_object_id_t gVirtualRouterId;

extern sai_router_interface_api_t*  sai_router_intfs_api;
extern sai_route_api_t*             sai_route_api;

extern PortsOrch *gPortsOrch;

IntfsOrch::IntfsOrch(DBConnector *db, string tableName) :
        Orch(db, tableName)
{
    SWSS_LOG_ENTER();
}

sai_object_id_t IntfsOrch::getRouterIntfsId(string alias)
{
    Port port;
    gPortsOrch->getPort(alias, port);
    assert(port.m_rif_id);
    return port.m_rif_id;
}

void IntfsOrch::increaseRouterIntfsRefCount(const string alias)
{
    m_syncdIntfses[alias].ref_count++;
}

void IntfsOrch::decreaseRouterIntfsRefCount(const string alias)
{
    m_syncdIntfses[alias].ref_count--;
}

void IntfsOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

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
            if (alias == "lo")
            {
                addIp2MeRoute(ip_prefix);
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

            if (m_syncdIntfses.find(alias) == m_syncdIntfses.end() ||
                !m_syncdIntfses[alias].ip_addresses.contains(ip_prefix.getIp()))
            {
                if (addRouterIntfs(port))
                {
                    IntfsEntry intfs_entry;
                    intfs_entry.ref_count = 0;
                    m_syncdIntfses[alias] = intfs_entry;

                    addSubnetRoute(port, ip_prefix);
                    addIp2MeRoute(ip_prefix);

                    m_syncdIntfses[alias].ip_addresses.add(ip_prefix.getIp());
                    it = consumer.m_toSync.erase(it);
                }
                else
                    it++;
            }
            else
                /* Duplicate entry */
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
            if (!gPortsOrch->getPort(alias, port))
            {
                SWSS_LOG_ERROR("Failed to locate interface %s", alias.c_str());
                throw logic_error("Failed to locate interface.");
            }

            if (m_syncdIntfses.find(alias) != m_syncdIntfses.end())
            {
                if (m_syncdIntfses[alias].ip_addresses.contains(ip_prefix.getIp()))
                {
                    removeSubnetRoute(port, ip_prefix);
                    removeIp2MeRoute(ip_prefix);

                    m_syncdIntfses[alias].ip_addresses.remove(ip_prefix.getIp());
                }

                if (removeRouterIntfs(port))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
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
        return true;

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
            attr.value.u16 = port.m_vlan_id;
            break;
        default:
            SWSS_LOG_ERROR("Unsupported port type: %d", port.m_type);
            break;
    }
    attrs.push_back(attr);

    sai_status_t status = sai_router_intfs_api->create_router_interface(&port.m_rif_id, attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create router interface for port %s, rv:%d", port.m_alias.c_str(), status);
        throw runtime_error("Failed to create router interface.");
    }

    gPortsOrch->setPort(port.m_alias, port);

    SWSS_LOG_NOTICE("Create router interface for port %s", port.m_alias.c_str());

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
    sai_unicast_route_entry_t unicast_route_entry;
    unicast_route_entry.vr_id = gVirtualRouterId;
    copy(unicast_route_entry.destination, ip_prefix);
    subnet(unicast_route_entry.destination, unicast_route_entry.destination);

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_ROUTE_ATTR_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
    attrs.push_back(attr);

    attr.id = SAI_ROUTE_ATTR_NEXT_HOP_ID;
    attr.value.oid = port.m_rif_id;
    attrs.push_back(attr);

    sai_status_t status = sai_route_api->create_route(&unicast_route_entry, attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create subnet route pre:%s, rv:%d", ip_prefix.to_string().c_str(), status);
        throw runtime_error("Failed to create subnet route.");
    }

    SWSS_LOG_NOTICE("Create subnet route pre:%s", ip_prefix.to_string().c_str());
    increaseRouterIntfsRefCount(port.m_alias);
}

void IntfsOrch::removeSubnetRoute(const Port &port, const IpPrefix &ip_prefix)
{
    sai_unicast_route_entry_t unicast_route_entry;
    unicast_route_entry.vr_id = gVirtualRouterId;
    copy(unicast_route_entry.destination, ip_prefix);
    subnet(unicast_route_entry.destination, unicast_route_entry.destination);

    sai_status_t status = sai_route_api->remove_route(&unicast_route_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove subnet route pre:%s, rv:%d", ip_prefix.to_string().c_str(), status);
        throw runtime_error("Failed to remove subnet route.");
    }

    SWSS_LOG_NOTICE("Remove subnet route with prefix:%s", ip_prefix.to_string().c_str());
    decreaseRouterIntfsRefCount(port.m_alias);
}

void IntfsOrch::addIp2MeRoute(const IpPrefix &ip_prefix)
{
    sai_unicast_route_entry_t unicast_route_entry;
    unicast_route_entry.vr_id = gVirtualRouterId;
    copy(unicast_route_entry.destination, ip_prefix.getIp());

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_ROUTE_ATTR_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
    attrs.push_back(attr);

    attr.id = SAI_ROUTE_ATTR_NEXT_HOP_ID;
    attr.value.oid = gPortsOrch->getCpuPort();
    attrs.push_back(attr);

    sai_status_t status = sai_route_api->create_route(&unicast_route_entry, attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create IP2me route ip:%s, rv:%d", ip_prefix.getIp().to_string().c_str(), status);
        throw runtime_error("Failed to create IP2me route.");
    }

    SWSS_LOG_NOTICE("Create IP2me route ip:%s", ip_prefix.getIp().to_string().c_str());
}

void IntfsOrch::removeIp2MeRoute(const IpPrefix &ip_prefix)
{
    sai_unicast_route_entry_t unicast_route_entry;
    unicast_route_entry.vr_id = gVirtualRouterId;
    copy(unicast_route_entry.destination, ip_prefix.getIp());

    sai_status_t status = sai_route_api->remove_route(&unicast_route_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove IP2me route ip:%s, rv:%d", ip_prefix.getIp().to_string().c_str(), status);
        throw runtime_error("Failed to remove IP2me route.");
    }

    SWSS_LOG_NOTICE("Remove packet action trap route ip:%s", ip_prefix.getIp().to_string().c_str());
}
