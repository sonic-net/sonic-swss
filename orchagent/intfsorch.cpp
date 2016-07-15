#include "intfsorch.h"

#include "ipprefix.h"
#include "logger.h"

#include "assert.h"
#include <fstream>
#include <sstream>
#include <map>

#include <net/if.h>

extern sai_object_id_t gVirtualRouterId;

extern sai_router_interface_api_t*  sai_router_intfs_api;
extern sai_route_api_t*             sai_route_api;

IntfsOrch::IntfsOrch(DBConnector *db, string tableName, PortsOrch *portsOrch) :
        Orch(db, tableName), m_portsOrch(portsOrch)
{
}

void IntfsOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!m_portsOrch->isInitDone())
        return;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        size_t found = key.find(':');
        if (found == string::npos)
        {
            SWSS_LOG_ERROR("Failed to parse task key %s\n", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }
        string alias = key.substr(0, found);

        /* TODO: Sync loopback address and trap all IP packets to loopback addressy */
        if (alias == "lo" || alias == "eth0" || alias == "docker0")
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        IpPrefix ip_prefix(key.substr(found+1));
        if (!ip_prefix.isV4())
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            /* Duplicate entry */
            if (m_intfs.find(alias) != m_intfs.end() && m_intfs[alias].contains(ip_prefix.getIp()))
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }

            Port port;
            if (!m_portsOrch->getPort(alias, port))
            {
                SWSS_LOG_ERROR("Failed to locate interface %s\n", alias.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (!port.m_rif_id)
            {
                addRouterIntfs(port);
                m_intfs[alias] = IpAddresses();
            }

            sai_unicast_route_entry_t unicast_route_entry;
            unicast_route_entry.vr_id = gVirtualRouterId;
            unicast_route_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
            unicast_route_entry.destination.addr.ip4 = ip_prefix.getIp().getV4Addr() & ip_prefix.getMask().getV4Addr();
            unicast_route_entry.destination.mask.ip4 = ip_prefix.getMask().getV4Addr();

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
                SWSS_LOG_ERROR("Failed to create subnet route pre:%s %d\n", ip_prefix.to_string().c_str(), status);
                it++;
                continue;
            }
            else
            {
                SWSS_LOG_NOTICE("Create subnet route pre:%s\n", ip_prefix.to_string().c_str());
            }

            unicast_route_entry.vr_id = gVirtualRouterId;
            unicast_route_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
            unicast_route_entry.destination.addr.ip4 = ip_prefix.getIp().getV4Addr();
            unicast_route_entry.destination.mask.ip4 = 0xFFFFFFFF;

            attr.id = SAI_ROUTE_ATTR_PACKET_ACTION;
            attr.value.s32 = SAI_PACKET_ACTION_TRAP;
            status = sai_route_api->create_route(&unicast_route_entry, 1, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to create packet action trap route ip:%s %d\n", ip_prefix.getIp().to_string().c_str(), status);
                it++;
            }
            else
            {
                SWSS_LOG_NOTICE("Create packet action trap route ip:%s\n", ip_prefix.getIp().to_string().c_str());
                m_intfs[alias].add(ip_prefix.getIp());
                it = consumer.m_toSync.erase(it);
            }
        }
        else if (op == DEL_COMMAND)
        {
            assert(m_intfs.find(alias) != m_intfs.end() && m_intfs[alias].contains(ip_prefix.getIp()));

            Port port;
            if (!m_portsOrch->getPort(alias, port))
            {
                SWSS_LOG_ERROR("Failed to locate interface %s\n", alias.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            sai_unicast_route_entry_t unicast_route_entry;
            unicast_route_entry.vr_id = gVirtualRouterId;
            unicast_route_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
            unicast_route_entry.destination.addr.ip4 = ip_prefix.getIp().getV4Addr() & ip_prefix.getMask().getV4Addr();
            unicast_route_entry.destination.mask.ip4 = ip_prefix.getMask().getV4Addr();

            sai_status_t status = sai_route_api->remove_route(&unicast_route_entry);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove subnet route pre:%s %d\n", ip_prefix.to_string().c_str(), status);
                it++;
                continue;
            }

            unicast_route_entry.vr_id = gVirtualRouterId;
            unicast_route_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
            unicast_route_entry.destination.addr.ip4 = ip_prefix.getIp().getV4Addr();
            unicast_route_entry.destination.mask.ip4 = 0xFFFFFFFF;

            status = sai_route_api->remove_route(&unicast_route_entry);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove action trap route ip:%s %d\n", ip_prefix.getIp().to_string().c_str(), status);
                it++;
            }
            else
            {
                SWSS_LOG_NOTICE("Remove packet action trap route ip:%s\n", ip_prefix.getIp().to_string().c_str());
                m_intfs[alias].remove(ip_prefix.getIp());
                it = consumer.m_toSync.erase(it);

                if (!m_intfs[alias].getSize())
                {
                    removeRouterIntfs(port);
                    m_intfs.erase(alias);
                }
            }
        }
    }
}

bool IntfsOrch::addRouterIntfs(Port &port, sai_object_id_t virtual_router_id, MacAddress mac_address)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    attr.value.oid = virtual_router_id;
    attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, mac_address.getMac(), sizeof(sai_mac_t));
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
        SWSS_LOG_ERROR("Failed to create router interface for port %s", port.m_alias.c_str());
        return false;
    }

    m_portsOrch->setPort(port.m_alias, port);

    SWSS_LOG_NOTICE("Create router interface for port %s", port.m_alias.c_str());

    return true;
}

bool IntfsOrch::removeRouterIntfs(Port &port)
{
    SWSS_LOG_ENTER();

    sai_status_t status = sai_router_intfs_api->remove_router_interface(port.m_rif_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove router interface for port %s", port.m_alias.c_str());
        return false;
    }

    port.m_rif_id = 0;
    m_portsOrch->setPort(port.m_alias, port);

    return true;
}

