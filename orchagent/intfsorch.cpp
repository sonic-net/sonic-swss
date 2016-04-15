#include "intfsorch.h"

#include "ipprefix.h"
#include "logger.h"

#include <fstream>
#include <sstream>
#include <map>

#include <net/if.h>

extern sai_object_id_t gVirtualRouterId;

extern sai_route_api_t*         sai_route_api;

IntfsOrch::IntfsOrch(DBConnector *db, string tableName, PortsOrch *portsOrch) :
        Orch(db, tableName), m_portsOrch(portsOrch)
{
}

void IntfsOrch::doTask(Consumer &consumer)
{
    if (consumer.m_toSync.empty())
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
            if (m_intfs.find(alias) != m_intfs.end() && m_intfs[alias] == ip_prefix.getIp())
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }

            Port p;
            if (!m_portsOrch->getPort(alias, p))
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

            sai_attribute_t attr;
            vector<sai_attribute_t> attrs;

            attr.id = SAI_ROUTE_ATTR_PACKET_ACTION;
            attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
            attrs.push_back(attr);

            attr.id = SAI_ROUTE_ATTR_NEXT_HOP_ID;
            attr.value.oid = p.m_rif_id;
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
                m_intfs[alias] = ip_prefix.getIp();
                it = consumer.m_toSync.erase(it);
            }
        }
        else if (op == DEL_COMMAND)
        {
            Port p;
            if (!m_portsOrch->getPort(alias, p))
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
                m_intfs.erase(alias);
                it = consumer.m_toSync.erase(it);
            }
        }
    }
}

