#include "evpnmhorch.h"

#include "portsorch.h"

extern PortsOrch *gPortsOrch;

extern sai_vlan_api_t *sai_vlan_api;

#define VLAN_PREFIX "Vlan"

EvpnMhOrch::EvpnMhOrch(vector<TableConnector> &connectors) : Orch(connectors)
{
    SWSS_LOG_ENTER();
}

EvpnMhOrch::~EvpnMhOrch()
{
    SWSS_LOG_ENTER();
}

EsCacheEntry *EvpnMhOrch::getEsCache(const std::string &key)
{
    auto entry_it = m_esDataMap.find(key);
    return (entry_it != m_esDataMap.end()) ? entry_it->second.get() : nullptr;
}

static std::string getPortFromEsKey(const std::string &key)
{
    auto pos = key.find(':');
    return (pos != std::string::npos) ? key.substr(pos + 1) : "Unknown";
}

static std::string getVlanFromEsKey(const std::string &key)
{
    auto pos = key.find(':');
    return (pos != std::string::npos) ? key.substr(0, pos) : "Unknown";
}

bool EvpnMhOrch::updateEsCache(string &key, KeyOpFieldsValuesTuple &t)
{
    bool is_df = false;
    struct EsCacheEntry *existing_entry = nullptr;

    // Note: KeyOpFieldsValuesTuple is the standard swss framework type for receiving
    // data from Redis tables. Although not optimal for lookups, it's part of the API contract.
    for (const auto &i : kfvFieldsValues(t))
    {
        if (fvField(i) == "df")
        {
            is_df = (fvValue(i) == "true");
            break;  // Found the field we need, no need to continue
        }
    }

    existing_entry = getEsCache(key);
    if (existing_entry)
    {
        existing_entry->is_df = is_df;
    }
    else
    {
        m_esDataMap[key] = std::make_unique<EsCacheEntry>(is_df);
        existing_entry = m_esDataMap[key].get();
    }

    /*
     * DESIGN NOTE: The YANG schema takes only the interface name, but the implementation
     * uses a composite key format "VlanX:InterfaceName" for cache lookups. This design
     * has performance implications:
     *
     * 1. Each VLAN in the bridge triggers an attribute update against the main port
     * 2. Current flat map structure requires string parsing on every lookup
     *
     * TODO: Consider alternative data structures for better performance:
     * - Nested map: map<vlan_id, map<port_name, EsCacheEntry*>>
     * - Multimap indexed by port for O(1) port-based lookups
     * - Parent/child cache relationship to reduce redundant updates
     */
    std::string port_name = getPortFromEsKey(key);
    std::string vlan_id = getVlanFromEsKey(key);
    Port port;
    sai_object_id_t vlan_member_id;

    SWSS_LOG_NOTICE("updateEsCache: SET oper: %s, vlan: %s, port_name: %s, is_df: %d", key.c_str(), vlan_id.c_str(), port_name.c_str(), existing_entry->is_df);

    if (!gPortsOrch->getPort(vlan_id, port))
    {
        SWSS_LOG_ERROR("updateEsCache: interface: %s, Vlan is not not yet created, returning", key.c_str());
        return false;
    }

    if (gPortsOrch->getVlanMember(port_name, port, vlan_member_id))
    {
        /*
         * TODO: Verify the correct SAI attribute to use.
         * Current: SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP (workaround)
         * HLD suggests: SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_NEXT_HOP_GROUP_ID
         * Need to confirm with SAI implementation and HLD requirements.
         *
         * FIXME: Verify logic correctness - attribute name suggests BUM traffic should
         * be DROPPED on non-DF. If true, this should be: !existing_entry->is_df
         * (DF=false → DROP=true, DF=true → DROP=false)
         */
        sai_attribute_t attr;
        attr.id = SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP;
        attr.value.booldata = existing_entry->is_df;

        auto status = sai_vlan_api->set_vlan_member_attribute(vlan_member_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("updateEsCache: Failed to set VLAN member attribute for %s, SAI status: %d. "
                          "BUM traffic forwarding state may be incorrect. Will retry.", key.c_str(), status);
            return false;
        }
    }
    else
    {
        SWSS_LOG_ERROR("updateEsCache: interface %s vlan_member_id doesnt exit", key.c_str());
        return false;
    }
    return true;
}

bool EvpnMhOrch::deleteEsCache(string &key)
{
    EsCacheEntry *entry = getEsCache(key);

    if (!entry)
    {
        SWSS_LOG_WARN("deleteEsCache: Entry not found for key: %s", key.c_str());
        return true;  // Nothing to delete, consider it successful
    }

    SWSS_LOG_NOTICE("deleteEsCache: DEL oper: intf: %s, is_df: %d", key.c_str(), entry->is_df);
    std::string port_name = getPortFromEsKey(key);
    std::string vlan_id = getVlanFromEsKey(key);
    Port port;
    sai_object_id_t vlan_member_id;

    if (!gPortsOrch->getPort(vlan_id, port))
    {
        SWSS_LOG_ERROR("deleteEsCache: interface: %s, Vlan is not not yet created, returning", key.c_str());
        return false;
    }
    if (gPortsOrch->getVlanMember(port_name, port, vlan_member_id))
    {
        /*
         * TODO: Verify the correct SAI attribute to use.
         * Current: SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP (workaround)
         * HLD suggests: SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_NEXT_HOP_GROUP_ID
         * Need to confirm with SAI implementation and HLD requirements.
         *
         * Note: Setting to false on delete to restore default forwarding behavior.
         */
        sai_attribute_t attr;
        attr.id = SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP;
        attr.value.booldata = false;

        auto status = sai_vlan_api->set_vlan_member_attribute(vlan_member_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("deleteEsCache: Failed to reset VLAN member attribute for %s, SAI status: %d. "
                          "BUM traffic forwarding state may be incorrect. Will retry.", key.c_str(), status);
            return false;
        }
    }
    else
    {
        SWSS_LOG_ERROR("deleteEsCache: interface %s vlan_member_id doesnt exit", key.c_str());
        return false;
    }
    m_esDataMap.erase(key);
    return true;
}

bool EvpnMhOrch::vlanMembersApplyNonDF(string port_name)
{
    vlan_members_t vlan_members;
    Port port;
    if (!gPortsOrch->getPort(port_name, port))
    {
        SWSS_LOG_ERROR("vlanMembersApplyNonDF: getPort() fails for port_name:%s", port_name.c_str());
        return false;
    }
    gPortsOrch->getPortVlanMembers(port, vlan_members);
    for (const auto &member : vlan_members)
    {
        auto vlan_id = member.first;
        auto vlan_mem_entry = member.second;
        /*
         * TODO: Verify the correct SAI attribute to use.
         * Current: SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP (workaround)
         * HLD suggests: SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_NEXT_HOP_GROUP_ID
         * Need to confirm with SAI implementation and HLD requirements.
         *
         * FIXME: Verify logic correctness - attribute name suggests BUM traffic should
         * be DROPPED on non-DF. If true, this should be: !isInterfaceDF(...)
         * (DF=false → DROP=true, DF=true → DROP=false)
         */
        sai_attribute_t attr;
        attr.id = SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP;
        attr.value.booldata = isInterfaceDF(port_name, vlan_id);
        SWSS_LOG_NOTICE("vlanMembersApplyNonDF: set Non-DF for port: %s, vlan: %d", port_name.c_str(), vlan_id);

        auto status = sai_vlan_api->set_vlan_member_attribute(vlan_mem_entry.vlan_member_id, &attr);
        if (status == SAI_STATUS_NOT_SUPPORTED || status == SAI_STATUS_NOT_IMPLEMENTED)
        {
            SWSS_LOG_WARN("vlanMembersApplyNonDF: SAI attribute not supported for port %s vlan %d, "
                          "Non-DF BUM suppression unavailable on this platform",
                          port_name.c_str(), vlan_id);
            continue;
        }
        else if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("vlanMembersApplyNonDF: failed to set Non-DF for port %s vlan %d (status %d)",
                           port_name.c_str(), vlan_id, status);
            return false;
        }
    }
    return true;
}
void EvpnMhOrch::doEvpnEsIntfTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);

        SWSS_LOG_NOTICE("doEvpnEsIntfTask: %s oper: ESI intf: %s", op.c_str(), key.c_str());

        if (!vlanMembersApplyNonDF(key))
        {
            // SAI operation failed, leave in m_toSync for retry
            // Do not modify m_esIntfMap until operation succeeds
            ++it;
        }
        else
        {
            // SAI operation succeeded, update internal state and remove from queue
            if (op == SET_COMMAND)
            {
                m_esIntfMap[key] = true;
            }
            else if (op == DEL_COMMAND)
            {
                m_esIntfMap.erase(key);
            }
            it = consumer.m_toSync.erase(it);
        }
    }
}

void EvpnMhOrch::doEvpnEsDfTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);

        bool success = false;
        if (op == SET_COMMAND)
        {
            success = updateEsCache(key, t);
        }
        else if (op == DEL_COMMAND)
        {
            success = deleteEsCache(key);
        }

        if (!success)
        {
            // SAI operation failed, leave in m_toSync for retry
            ++it;
        }
        else
        {
            it = consumer.m_toSync.erase(it);
        }
    }
}

void EvpnMhOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();
    if (table_name == "EVPN_DF_TABLE")
    {
        doEvpnEsDfTask(consumer);
    }
    else
    {
        if (table_name == "EVPN_ETHERNET_SEGMENT")
        {
            doEvpnEsIntfTask(consumer);
        }
    }
}

bool EvpnMhOrch::isInterfaceDF(const std::string &port_name, sai_vlan_id_t vlan_id)
{
    std::string df_key = VLAN_PREFIX + std::to_string(vlan_id) + ":" + port_name;
    if (EsCacheEntry *entry = getEsCache(df_key))
        return entry->is_df;
    return false;
}

bool EvpnMhOrch::isPortAndVlanAssociatedToEs(const std::string &port_name, sai_vlan_id_t vlan_id)
{
    if (isPortInterfaceAssociatedToEs(port_name))
        return true;

    std::string df_key = VLAN_PREFIX + std::to_string(vlan_id) + ":" + port_name;
    return getEsCache(df_key) != nullptr;
}

bool EvpnMhOrch::isPortInterfaceAssociatedToEs(const std::string &port_name)
{
    return (m_esIntfMap.find(port_name) != m_esIntfMap.end());
}
