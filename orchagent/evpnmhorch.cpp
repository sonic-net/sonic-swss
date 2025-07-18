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

struct EsCacheEntry *EvpnMhOrch::getEsCache(const std::string &key)
{
    std::map<std::string, struct EsCacheEntry *>::iterator entry_it = m_esDataMap.find(key);
    struct EsCacheEntry *entry = nullptr;

    if (entry_it == m_esDataMap.end())
    {
        entry = nullptr;
    }
    else
    {
        entry = (*entry_it).second;
    }

    return entry;
}

struct EsCacheEntry *EvpnMhOrch::getEsCacheForPort(const std::string &key)
{
    std::map<std::string, struct EsCacheEntry *>::iterator entry_it = m_esDataMap.begin();
    struct EsCacheEntry *entry = nullptr;

    while (entry_it != m_esDataMap.end() && entry == nullptr)
    {
        if (((*entry_it).first).find(key) != std::string::npos)
        {
            entry = (*entry_it).second;
        }

        entry_it++;
    }

    return entry;
}

std::string getPortFromEsKey(string &key)
{
    std::string ret_port;
    std::size_t found = key.find(":");

    if (found != std::string::npos)
    {
        ret_port = key.substr(found + 1, std::string::npos);
    }
    else
    {
        ret_port = "Unknown";
    }

    return ret_port;
}

std::string getVlanFromEsKey(string &key)
{
    std::string ret_vlan;
    std::size_t found = key.find(":");

    if (found != std::string::npos)
    {
        ret_vlan = key.substr(0, found);
    }
    else
    {
        ret_vlan = "Unknown";
    }

    return ret_vlan;
}

void EvpnMhOrch::updateEsCache(string &key, KeyOpFieldsValuesTuple &t)
{
    bool is_df = false;
    struct EsCacheEntry *existing_entry = nullptr;

    for (auto i : kfvFieldsValues(t))
    {
        if (fvField(i) == "df")
        {
            is_df = (fvValue(i) == "true");
        }
    }

    existing_entry = getEsCache(key);
    if (existing_entry)
    {
        existing_entry->is_df = is_df;
    }
    else
    {
        existing_entry = new EsCacheEntry(is_df);
        m_esDataMap[key] = existing_entry;
    }

    if (existing_entry)
    {
        /*
         * This is going to get slow at scale as each vlan in the bridge
         * will trigger an attribute update against the main port.
         *
         * Might need to add a parent/child cache relationship to improve performance.
         */
        std::string port_name = getPortFromEsKey(key);
        std::string vlan_id = getVlanFromEsKey(key);
        Port port;
        sai_object_id_t vlan_member_id;

        SWSS_LOG_NOTICE("updateEsCache: SET oper: %s, vlan: %s, port_name: %s, is_df: %d", key.c_str(), vlan_id.c_str(), port_name.c_str(), existing_entry->is_df);

        if (!gPortsOrch->getPort(vlan_id, port))
        {
            SWSS_LOG_ERROR("updateEsCache: interface: %s, Vlan is not not yet created, returning", key.c_str());
            return;
        }

        if (gPortsOrch->getVlanMember(port_name, port, vlan_member_id))
        {
            /* TODO: Use proper attribute once its available in SAI */
            sai_attribute_t attr;
            attr.id = SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP;
            attr.value.booldata = existing_entry->is_df;

            auto status = sai_vlan_api->set_vlan_member_attribute(vlan_member_id, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                /* TODO: Error handling */
            }
        }
        else
        {
            SWSS_LOG_ERROR("updateEsCache: interface %s vlan_member_id doesnt exit", key.c_str());
            return;
        }
    }
}

void EvpnMhOrch::deleteEsCache(string &key)
{
    EsCacheEntry *entry = getEsCache(key);

    if (entry)
    {
        SWSS_LOG_NOTICE("deleteEsCache: DEL oper: intf: %s, is_df: %d", key.c_str(), entry->is_df);
        std::string port_name = getPortFromEsKey(key);
        std::string vlan_id = getVlanFromEsKey(key);
        Port port;
        sai_object_id_t vlan_member_id;

        if (!gPortsOrch->getPort(vlan_id, port))
        {
            SWSS_LOG_ERROR("deleteEsCache: interface: %s, Vlan is not not yet created, returning", key.c_str());
            return;
        }
        if (gPortsOrch->getVlanMember(port_name, port, vlan_member_id))
        {
            /* TODO: Use proper attribute once its available in SAI */
            sai_attribute_t attr;
            attr.id = SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP;
            attr.value.booldata = false;

            auto status = sai_vlan_api->set_vlan_member_attribute(vlan_member_id, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                /* TODO: Error handling */
            }
        }
        else
        {
            SWSS_LOG_ERROR("deleteEsCache: interface %s vlan_member_id doesnt exit\n", key.c_str());
            return;
        }
        m_esDataMap.erase(key);
        delete entry;
        entry = nullptr;
    }
}

void EvpnMhOrch::vlanMembersApplyNonDF(string port_name)
{
    vlan_members_t vlan_members;
    Port port;
    if (!gPortsOrch->getPort(port_name, port))
    {
        SWSS_LOG_ERROR("vlanMembersApplyNonDF: getPort() fails for port_name:%s", port_name.c_str());
        return;
    }
    gPortsOrch->getPortVlanMembers(port, vlan_members);
    for (const auto &member : vlan_members)
    {
        auto vlan_id = member.first;
        auto vlan_mem_entry = member.second;
        /* TODO: Use proper attribute once its available in SAI */
        sai_attribute_t attr;
        attr.id = SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP;
        attr.value.booldata = isInterfaceDF(port_name, vlan_id);
        SWSS_LOG_NOTICE("vlanMembersApplyNonDF: set Non-DF for port: %s, vlan: %d", port_name.c_str(), vlan_id);

        auto status = sai_vlan_api->set_vlan_member_attribute(vlan_mem_entry.vlan_member_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            /* TODO: Error handling */
        }
    }
    return;
}
void EvpnMhOrch::doEvpnEsIntfTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            SWSS_LOG_NOTICE("doEvpnEsIntfTask: SET oper: ESI intf: %s", key.c_str());
            m_esIntfMap[key] = true;
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("doEvpnEsIntfTask: DEL oper: ESI intf: %s", key.c_str());
            m_esIntfMap.erase(key);
        }
        vlanMembersApplyNonDF(key);
        it = consumer.m_toSync.erase(it);
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

        if (op == SET_COMMAND)
        {
            updateEsCache(key, t);
        }
        else if (op == DEL_COMMAND)
        {
            deleteEsCache(key);
        }
        it = consumer.m_toSync.erase(it);
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

bool EvpnMhOrch::isInterfaceDF(const std::string port_name, const sai_vlan_id_t vlan_id)
{
    bool is_df = false;
    string df_key = VLAN_PREFIX + to_string(vlan_id) + ":" + port_name;
    EsCacheEntry *entry = getEsCache(df_key);

    if (entry)
    {
        is_df = entry->is_df;
    }

    return (is_df);
}

bool EvpnMhOrch::isPortAndVlanAssociatedToEs(const std::string port_name, const sai_vlan_id_t vlan_id)
{
    if (isPortInterfaceAssociatedToEs(port_name))
    {
        return true;
    }
    string df_key = VLAN_PREFIX + to_string(vlan_id) + ":" + port_name;
    EsCacheEntry *entry = getEsCache(df_key);

    if (entry)
    {
        return true;
    }

    return false;
}

bool EvpnMhOrch::isPortInterfaceAssociatedToEs(const std::string port_name)
{
    return (m_esIntfMap.find(port_name) != m_esIntfMap.end());
}
