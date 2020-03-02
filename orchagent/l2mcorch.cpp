#include <assert.h>
#include <algorithm>
#include "l2mcorch.h"
#include "logger.h"
#include "swssnet.h"
#include  "tokenize.h"
#include  "errororch.h"
#include "sai_serialize.h"
#include "redisclient.h"
#include "debugframework.h"

using namespace swss;

#define VLAN_PREFIX "Vlan"

extern sai_object_id_t gSwitchId;

extern sai_vlan_api_t *sai_vlan_api;
extern sai_switch_api_t*            sai_switch_api;
extern sai_l2mc_api_t*              sai_l2mc_entry_api;
extern sai_l2mc_group_api_t*        sai_l2mc_group_api;

extern PortsOrch *gPortsOrch;
extern DebugDumpOrch *gDebugDumpOrch;

L2mcOrch::L2mcOrch(DBConnector * appDb, vector<string> &tableNames) :
    Orch(appDb, tableNames)
{
    SWSS_LOG_ENTER();

    /* Register with debug framework */
    this->gL2mcOrchDbgComp = "l2mcorch";
    gDebugDumpOrch->addDbgCompMap(gL2mcOrchDbgComp, this);
    memset (&l2mcdbg_counters, 0, sizeof (struct L2mcDebugCounters));
};

bool L2mcOrch::hasL2mcGroup(string vlan, const L2mcGroupKey &l2mckey)
{
    return m_syncdL2mcEntries.at(vlan).find(l2mckey) != m_syncdL2mcEntries.at(vlan).end();
}

sai_object_id_t L2mcOrch::getL2mcGroupId(string vlan, const L2mcGroupKey &l2mckey)
{
    return m_syncdL2mcEntries[vlan][l2mckey].l2mc_group_id;
}

void L2mcOrch::increaseL2mcMemberRefCount(string vlan, const L2mcGroupKey& l2mckey)
{
    m_syncdL2mcEntries[vlan][l2mckey].ref_count ++;
    return;
}

void L2mcOrch::decreaseL2mcMemberRefCount(string vlan, const L2mcGroupKey& l2mckey)
{
    m_syncdL2mcEntries[vlan][l2mckey].ref_count --;
    return;
}

bool L2mcOrch::isMemberRefCntZero(string vlan, const L2mcGroupKey& l2mckey) const
{
    return m_syncdL2mcEntries.at(vlan).at(l2mckey).ref_count == 0;
}

bool L2mcOrch::AddL2mcGroupMember(const L2mcGroupKey &l2mc_GrpKey, string vlan_alias, Port &port)
{
    sai_status_t status;
    sai_object_id_t l2mc_member_id;
    vector<sai_attribute_t> l2mcgm_attrs;
    sai_attribute_t l2mcgm_attr;
    Port vlan;

    SWSS_LOG_ENTER();

    if (!gPortsOrch->getPort(vlan_alias, vlan))
    {
        SWSS_LOG_NOTICE("AddL2mcGroupMember: Failed to locate vlan %s", vlan_alias.c_str());
        l2mcdbg_counters.l2mc_vlan_fail++;
        return false;
    }

    auto l2mc_group = m_syncdL2mcEntries.at(vlan_alias).find(l2mc_GrpKey);
    /*Group member already exists */
    if (l2mc_group->second.l2mc_group_members.find(port.m_alias) != l2mc_group->second.l2mc_group_members.end())
        return true;

    SWSS_LOG_NOTICE("AddL2mcGroupMember: (%s,%s,%s) Add l2mc group member %s to l2mc_gid:%lx", 
                    l2mc_GrpKey.source_address.to_string().c_str(), l2mc_GrpKey.group_address.to_string().c_str(), 
                    l2mc_GrpKey.vlan_alias.c_str(), port.m_alias.c_str(), l2mc_group->second.l2mc_group_id);
    
    bzero(&l2mcgm_attr, sizeof(l2mcgm_attr));

    l2mcgm_attr.id = SAI_L2MC_GROUP_MEMBER_ATTR_L2MC_GROUP_ID;
    l2mcgm_attr.value.oid = l2mc_group->second.l2mc_group_id;
    l2mcgm_attrs.push_back(l2mcgm_attr);

    l2mcgm_attr.id = SAI_L2MC_GROUP_MEMBER_ATTR_L2MC_OUTPUT_ID;
    l2mcgm_attr.value.oid = port.m_bridge_port_id;
    l2mcgm_attrs.push_back(l2mcgm_attr);

    status = sai_l2mc_group_api->create_l2mc_group_member(&l2mc_member_id, gSwitchId,
                                                        (uint32_t)l2mcgm_attrs.size(),
                                                        l2mcgm_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("AddL2mcGroupMember: Failed to add l2mc group member %s, to l2mc-grp:%lx rv:%d",
                        port.m_alias.c_str(), l2mc_group->second.l2mc_group_id, status);
        l2mcdbg_counters.l2mc_member_add_fail++;
        return false;
    }
    l2mc_group->second.l2mc_group_members[port.m_alias] = l2mc_member_id;
    l2mcdbg_counters.l2mc_member_add++;
    increaseL2mcMemberRefCount(vlan_alias, l2mc_GrpKey);

    port.m_l2mc_count++;
    gPortsOrch->setPort(port.m_alias, port);
    vlan.m_l2mc_count++;
    gPortsOrch->setPort(vlan.m_alias, vlan);
    SWSS_LOG_NOTICE("port %s l2mc_count %u vlan %s l2mc_count %u",
        port.m_alias.c_str(), port.m_l2mc_count, vlan.m_alias.c_str(), vlan.m_l2mc_count);

    return true;
}

bool L2mcOrch::RemoveL2mcGroupMember(const L2mcGroupKey &l2mc_GrpKey, string vlan_alias, Port &port)
{
    sai_status_t status;
    Port vlan;

    SWSS_LOG_ENTER();

    if (!gPortsOrch->getPort(vlan_alias, vlan))
    {
        SWSS_LOG_NOTICE("RemoveL2mcGroupMember: Failed to locate vlan %s", vlan_alias.c_str());
        l2mcdbg_counters.l2mc_vlan_fail++;
        return false;
    }

    auto l2mc_group = m_syncdL2mcEntries.at(vlan_alias).find(l2mc_GrpKey);

    /* l2mc group member already deleted or doesn't exists */
    if(l2mc_group->second.l2mc_group_members.find(port.m_alias) == l2mc_group->second.l2mc_group_members.end())
        return true;

    SWSS_LOG_NOTICE("RemoveL2mcGroupMember: (%s,%s,%s) Delete l2mc group member %s from l2mc_gid:%lx", 
                    l2mc_GrpKey.source_address.to_string().c_str(), l2mc_GrpKey.group_address.to_string().c_str(), 
                    l2mc_GrpKey.vlan_alias.c_str(), port.m_alias.c_str(), l2mc_group->second.l2mc_group_id);
    
    auto l2mc_group_member = l2mc_group->second.l2mc_group_members.find(port.m_alias);
    if (l2mc_group_member != l2mc_group->second.l2mc_group_members.end())
    {
        status = sai_l2mc_group_api->remove_l2mc_group_member(l2mc_group_member->second);
        
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("RemoveL2mcGroupMember: Failed to remove l2mc group member %lx, from l2mc-grp:%lx rv:%d",
                           l2mc_group_member->second, l2mc_group->second.l2mc_group_id, status);
            l2mcdbg_counters.l2mc_member_del_fail++;
            return false;
        }
        decreaseL2mcMemberRefCount(vlan_alias, l2mc_GrpKey);
        l2mcdbg_counters.l2mc_member_del++;
        l2mc_group->second.l2mc_group_members.erase(l2mc_group_member);

        port.m_l2mc_count--;
        gPortsOrch->setPort(port.m_alias, port);
        vlan.m_l2mc_count--;
        gPortsOrch->setPort(vlan.m_alias, vlan);
        SWSS_LOG_NOTICE("port %s l2mc_count %u vlan %s l2mc_count %u",
            port.m_alias.c_str(), port.m_l2mc_count, vlan.m_alias.c_str(), vlan.m_l2mc_count);
    }
    return true;
}

bool L2mcOrch::AddL2mcEntry(const L2mcGroupKey &l2mcGrpKey, Port &vlan, Port &port)
{
    sai_object_id_t l2mc_group_id;
    sai_status_t status;
    L2mcGroupEntry l2mc_group_entry;

    SWSS_LOG_ENTER();

    if (port.m_bridge_port_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_NOTICE("AddL2mcEntry: NULL port OID"); 
        l2mcdbg_counters.l2mc_port_oid_fail++;
        return false;
    }

    if (vlan.m_vlan_info.vlan_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_NOTICE("AddL2mcEntry: NULL vlan OID");
        l2mcdbg_counters.l2mc_vlan_oid_fail++;
        return false;
    }

    if (!hasL2mcGroup(vlan.m_alias,l2mcGrpKey))
    {
        status = sai_l2mc_group_api->create_l2mc_group(&l2mc_group_id, gSwitchId, 0 , NULL);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("AddL2mcEntry: Failed to add l2mc group %lx, rv:%d", l2mc_group_id, status);
            l2mcdbg_counters.l2mc_group_add_fail++;
            return false;
        }

        l2mc_group_entry.l2mc_group_id = l2mc_group_id;
        l2mc_group_entry.ref_count = 0;
    }
    else
    {
        l2mc_group_id = getL2mcGroupId(vlan.m_alias, l2mcGrpKey);
    }

    sai_attribute_t l2mc_entry_attr;
    sai_l2mc_entry_t l2mc_entry;
    vector<sai_attribute_t> l2mc_entry_attrs;

    bzero(&l2mc_entry_attr, sizeof(l2mc_entry_attr));
    
    /*Add l2mc entry */
    l2mc_entry_attr.id = SAI_L2MC_ENTRY_ATTR_OUTPUT_GROUP_ID;
    l2mc_entry_attr.value.oid = l2mc_group_id;
    l2mc_entry_attrs.push_back(l2mc_entry_attr);

    l2mc_entry_attr.id = SAI_L2MC_ENTRY_ATTR_PACKET_ACTION;
    l2mc_entry_attr.value.oid = SAI_PACKET_ACTION_FORWARD;
    l2mc_entry_attrs.push_back(l2mc_entry_attr);

    l2mc_entry.switch_id = gSwitchId;
    copy(l2mc_entry.source, l2mcGrpKey.source_address);
    copy(l2mc_entry.destination, l2mcGrpKey.group_address);
    l2mc_entry.source.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    l2mc_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    l2mc_entry.bv_id = vlan.m_vlan_info.vlan_oid;

    if(IpAddress(l2mcGrpKey.source_address.to_string()).isZero())
        l2mc_entry.type = SAI_L2MC_ENTRY_TYPE_XG;
    else
        l2mc_entry.type = SAI_L2MC_ENTRY_TYPE_SG;


    status = sai_l2mc_entry_api->create_l2mc_entry(&l2mc_entry, (uint32_t)l2mc_entry_attrs.size(),
                                                    l2mc_entry_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("AddL2mcEntry: Failed to create l2mc entry  (%s, %s, %d) rv:%d",
                        l2mcGrpKey.source_address.to_string().c_str(), l2mcGrpKey.group_address.to_string().c_str(), vlan.m_vlan_info.vlan_id, status);
        l2mcdbg_counters.l2mc_entry_add_fail++;
        return false;
    }

    if (!hasL2mcGroup(vlan.m_alias, l2mcGrpKey))
        m_syncdL2mcEntries[vlan.m_alias][l2mcGrpKey] = l2mc_group_entry;

    AddL2mcGroupMember(l2mcGrpKey, vlan.m_alias, port);
    
    SWSS_LOG_NOTICE("AddL2mcEntry: Added l2mc entry (%s,%s,%d) with l2mc-gid:%lx", l2mcGrpKey.source_address.to_string().c_str(),
                    l2mcGrpKey.group_address.to_string().c_str(), vlan.m_vlan_info.vlan_id, l2mc_group_id);

    return true;
}

bool L2mcOrch::RemoveL2mcEntry(const L2mcGroupKey &key, Port &vlan)
{
    sai_l2mc_entry_t l2mc_entry;
    sai_object_id_t l2mc_group_id;
    sai_status_t status;

    SWSS_LOG_ENTER();

    auto l2mc_group = m_syncdL2mcEntries.at(vlan.m_alias).find(key);

    if(l2mc_group == m_syncdL2mcEntries.at(vlan.m_alias).end())
    {
        SWSS_LOG_INFO("RemoveL2mcEntry: L2mc Entry not found. (%s, %s, %d)", key.source_address.to_string().c_str(),
                     key.group_address.to_string().c_str(), vlan.m_vlan_info.vlan_id);
        return true;
    }
    /*Remove l2mc entry */
    l2mc_entry.switch_id = gSwitchId;
    copy(l2mc_entry.source, key.source_address);
    copy(l2mc_entry.destination, key.group_address);
    l2mc_entry.source.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    l2mc_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    l2mc_entry.bv_id = vlan.m_vlan_info.vlan_oid;

    if(IpAddress(key.source_address.to_string()).isZero())
        l2mc_entry.type = SAI_L2MC_ENTRY_TYPE_XG;
    else
        l2mc_entry.type = SAI_L2MC_ENTRY_TYPE_SG;

    status = sai_l2mc_entry_api->remove_l2mc_entry(&l2mc_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("RemoveL2mcEntry: Failed to remove l2mc entry  (%s, %s, %d) rv:%d",
                        key.source_address.to_string().c_str(), key.group_address.to_string().c_str(), vlan.m_vlan_info.vlan_id, status);
        l2mcdbg_counters.l2mc_entry_del_fail++;
        return false;
    }
    l2mc_group_id = getL2mcGroupId(vlan.m_alias, key);

    SWSS_LOG_NOTICE("RemoveL2mcEntry: (%s,%s,%d) Delete l2mc group %lx", key.source_address.to_string().c_str(),
                    key.group_address.to_string().c_str(), vlan.m_vlan_info.vlan_id, l2mc_group_id);

    /* Remove all mrouter ports added as group members */
    for (auto l2mc_group_member = l2mc_group->second.l2mc_group_members.begin();
         l2mc_group_member != l2mc_group->second.l2mc_group_members.end();)
    {
        status = sai_l2mc_group_api->remove_l2mc_group_member(l2mc_group_member->second);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("RemoveL2mcEntry: Failed to remove mrouter from l2mc group member %lx, rv:%d",
                           l2mc_group_member->second, status);
            l2mcdbg_counters.l2mc_mrouter_del_fail++;
            return false;
        }
        l2mc_group_member = l2mc_group->second.l2mc_group_members.erase(l2mc_group_member);
    }
    /* Remove l2mc group */
    status = sai_l2mc_group_api->remove_l2mc_group(l2mc_group_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("RemoveL2mcEntry: Failed to remove l2mc group %lx, rv:%d", l2mc_group_id, status);
        l2mcdbg_counters.l2mc_group_del_fail++;
        return false;
    }

    m_syncdL2mcEntries.at(vlan.m_alias).erase(key);

    return true;
}

bool L2mcOrch::AddL2mcMrouterPort(const L2mcGroupKey &l2mc_GrpKey, string vlan, Port &port)
{
    sai_status_t status;
    sai_object_id_t l2mc_member_id;
    vector<sai_attribute_t> l2mcgm_attrs;
    sai_attribute_t l2mcgm_attr;

    SWSS_LOG_ENTER();

    if (port.m_bridge_port_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_NOTICE("AddL2mcMrouterPort: NULL port OID"); 
        l2mcdbg_counters.l2mc_port_oid_fail++;
        return false;
    }

    auto l2mc_group = m_syncdL2mcEntries.at(vlan).find(l2mc_GrpKey);
    /*Group member already exists */
    if (l2mc_group->second.l2mc_group_members.find(port.m_alias) != l2mc_group->second.l2mc_group_members.end())
        return true;

    SWSS_LOG_NOTICE("AddL2mcMrouterPort: (%s,%s,%s) Add mrouter port %s as l2mc group member to l2mc_gid:%lx", 
                    l2mc_GrpKey.source_address.to_string().c_str(), l2mc_GrpKey.group_address.to_string().c_str(), 
                    l2mc_GrpKey.vlan_alias.c_str(), port.m_alias.c_str(), l2mc_group->second.l2mc_group_id);
    
    bzero(&l2mcgm_attr, sizeof(l2mcgm_attr));

    l2mcgm_attr.id = SAI_L2MC_GROUP_MEMBER_ATTR_L2MC_GROUP_ID;
    l2mcgm_attr.value.oid = l2mc_group->second.l2mc_group_id;
    l2mcgm_attrs.push_back(l2mcgm_attr);

    l2mcgm_attr.id = SAI_L2MC_GROUP_MEMBER_ATTR_L2MC_OUTPUT_ID;
    l2mcgm_attr.value.oid = port.m_bridge_port_id;
    l2mcgm_attrs.push_back(l2mcgm_attr);

    status = sai_l2mc_group_api->create_l2mc_group_member(&l2mc_member_id, gSwitchId,
                                                        (uint32_t)l2mcgm_attrs.size(),
                                                        l2mcgm_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("AddL2mcMrouterPort: Failed to add mrouter port %s as l2mc group member to l2mc-grp:%lx rv:%d",
                        port.m_alias.c_str(), l2mc_group->second.l2mc_group_id, status);
        l2mcdbg_counters.l2mc_mrouter_add_fail++;
        return false;
    }
    l2mc_group->second.l2mc_group_members[port.m_alias] = l2mc_member_id;
    l2mcdbg_counters.l2mc_mrouter_add++;
    return true;
}

bool L2mcOrch::RemoveL2mcMrouterPort(const L2mcGroupKey &l2mc_GrpKey, string vlan, string port)
{
    sai_status_t status;

    SWSS_LOG_ENTER();

    auto l2mc_group = m_syncdL2mcEntries.at(vlan).find(l2mc_GrpKey);

    /* l2mc group member already deleted or doesn't exists */
    if(l2mc_group->second.l2mc_group_members.find(port) == l2mc_group->second.l2mc_group_members.end())
        return true;

    SWSS_LOG_NOTICE("RemoveL2mcMrouterPort: (%s,%s,%s) Delete l2mc mrouter group member %s from l2mc_gid:%lx", 
                    l2mc_GrpKey.source_address.to_string().c_str(), l2mc_GrpKey.group_address.to_string().c_str(), 
                    l2mc_GrpKey.vlan_alias.c_str(), port.c_str(), l2mc_group->second.l2mc_group_id);
    
    auto l2mc_group_member = l2mc_group->second.l2mc_group_members.find(port);
    if (l2mc_group_member != l2mc_group->second.l2mc_group_members.end())
    {
        status = sai_l2mc_group_api->remove_l2mc_group_member(l2mc_group_member->second);
        
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("RemoveL2mcMrouterPort: Failed to remove l2mc mrouter group member %lx, from l2mc-grp:%lx rv:%d",
                           l2mc_group_member->second, l2mc_group->second.l2mc_group_id, status);
            l2mcdbg_counters.l2mc_mrouter_del_fail++;
            return false;
        }
        l2mcdbg_counters.l2mc_mrouter_del++;
        l2mc_group->second.l2mc_group_members.erase(l2mc_group_member);
    }
    return true;
}

bool L2mcOrch::EnableIgmpSnooping(string vlan_alias)
{
    Port vlan;
    sai_attribute_t attr;

    SWSS_LOG_ENTER();

    if (!gPortsOrch->getPort(vlan_alias, vlan))
    {
        SWSS_LOG_INFO("Failed to locate VLAN %s", vlan_alias.c_str());
        l2mcdbg_counters.l2mc_vlan_fail++;
        return false;
    }

    if (vlan.m_vlan_info.vlan_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_NOTICE("EnableIgmpSnooping: NULL vlan OID");
        l2mcdbg_counters.l2mc_vlan_oid_fail++;
        return false;
    }

    bzero(&attr, sizeof(attr));

    attr.id = SAI_VLAN_ATTR_CUSTOM_IGMP_SNOOPING_ENABLE;
    attr.value.booldata = true;

    sai_status_t status = sai_vlan_api->set_vlan_attribute(vlan.m_vlan_info.vlan_oid, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(" Failed to Enable L2mc snooping on %s status %u", vlan_alias.c_str(), status);
        return false;
    }
    SWSS_LOG_NOTICE(" Enabled L2mc Snooping on %s ", vlan_alias.c_str());
    return true;
}

bool L2mcOrch::DisableIgmpSnooping(string vlan_alias)
{
    Port vlan;
    sai_attribute_t attr;

    SWSS_LOG_ENTER();

    if (!gPortsOrch->getPort(vlan_alias, vlan))
    {
        SWSS_LOG_INFO("Failed to locate VLAN %s", vlan_alias.c_str());
        l2mcdbg_counters.l2mc_vlan_fail++;
        return false;
    }

    bzero(&attr, sizeof(attr));

    attr.id = SAI_VLAN_ATTR_CUSTOM_IGMP_SNOOPING_ENABLE;
    attr.value.booldata = false;

    sai_status_t status = sai_vlan_api->set_vlan_attribute(vlan.m_vlan_info.vlan_oid, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR(" Failed to Disable L2mc snooping on %s status %u", vlan_alias.c_str(), status);
        return false;
    }
    SWSS_LOG_NOTICE(" Disabled L2mc Snooping on %s ", vlan_alias.c_str());
    return true;
}


void L2mcOrch::doL2mcTask(Consumer &consumer)
{
    unsigned short vlan_id;
    string vlan_alias;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);
        vector<string> keys = tokenize(kfvKey(t), ':');
        /* Key: <VLAN_name>*/

        /* Ensure the key size is 1 otherwise ignore */
        if (keys.size() != 1)
        {
            SWSS_LOG_ERROR("Invalid key size, skipping %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }
        
        /* Ensure the key starts with "Vlan" otherwise ignore */
        if (strncmp(keys[0].c_str(), VLAN_PREFIX, 4))
        {
            SWSS_LOG_ERROR("doL2mcTask:Invalid key format. No 'Vlan' prefix: %s", keys[0].c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        vlan_id = (unsigned short) stoi(keys[0].substr(4));
        vlan_alias = VLAN_PREFIX + to_string(vlan_id);

        if (op == SET_COMMAND)
        {
            m_snoop_enabled_vlans.push_back(vlan_alias);
            if(EnableIgmpSnooping(vlan_alias))
                it = consumer.m_toSync.erase(it);
            else
                it++;
        }
        else if (op == DEL_COMMAND)
        {
            auto v = find(m_snoop_enabled_vlans.begin(), m_snoop_enabled_vlans.end(), vlan_alias);
            if (v != m_snoop_enabled_vlans.end())
                m_snoop_enabled_vlans.erase(v);

            if(DisableIgmpSnooping(vlan_alias))
                it = consumer.m_toSync.erase(it);
            else
                it++;
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void L2mcOrch::doL2mcMemberTask(Consumer &consumer)
{
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
        vector<string> keys = tokenize(kfvKey(t), ':');
        /* Key: <VLAN_name>:<source_address> <group_address> <member_port> */

        /* Ensure the key size is 4 otherwise ignore */
        if (keys.size() != 4)
        {
            SWSS_LOG_ERROR("doL2mcMemberTask: Invalid key size, skipping %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }
        
        /* Ensure the key starts with "Vlan" otherwise ignore */
        if (strncmp(keys[0].c_str(), VLAN_PREFIX, 4))
        {
            SWSS_LOG_ERROR("doL2mcMemberTask: Invalid key format. No 'Vlan' prefix: %s", keys[0].c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        unsigned short vlan_id;
        std::string port_alias, vlan_alias;
        Port port, mrport, vlan;

        vlan_id = (unsigned short) stoi(keys[0].substr(4));
        vlan_alias = VLAN_PREFIX + to_string(vlan_id);
        port_alias = keys[3];

        if (!gPortsOrch->getPort(port_alias, port))
        {
            SWSS_LOG_ERROR("doL2mcMemberTask: Failed to get port for %s alias", port_alias.c_str());
            l2mcdbg_counters.l2mc_port_fail++;
            continue;
        }

        if (!gPortsOrch->getPort(vlan_alias, vlan))
        {
            SWSS_LOG_NOTICE("doL2mcMemberTask: Failed to locate vlan %s", vlan_alias.c_str());
            l2mcdbg_counters.l2mc_vlan_fail++;
            continue;
        }

        auto l2mcKey = L2mcGroupKey(vlan_alias, IpAddress(keys[1]), IpAddress(keys[2]));

        if (m_syncdL2mcEntries.find(vlan.m_alias) == m_syncdL2mcEntries.end())
        {
            m_syncdL2mcEntries.emplace(vlan.m_alias, L2mcEntryTable());
        }

        if (op == SET_COMMAND)
        {

            if (!hasL2mcGroup(vlan_alias, l2mcKey))
            {
                if (AddL2mcEntry(l2mcKey, vlan, port))
                {
                    /* Add all mrouter ports to the newly added l2mc entry */
                    auto mrouter_ports = mrouter_ports_per_vlan[vlan_alias];
        
                    if(!mrouter_ports.empty())
                    {
                        for (const auto& mrouter: mrouter_ports)
                        {
                            if (gPortsOrch->getPort(mrouter, mrport))
                                AddL2mcMrouterPort(l2mcKey, vlan_alias, mrport);
                        }
                    }
                    it = consumer.m_toSync.erase(it);
                }
                else
                    it++;  
            }
            else
            {
                if (AddL2mcGroupMember(l2mcKey, vlan_alias, port))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (hasL2mcGroup(vlan_alias, l2mcKey))
            {
                if (RemoveL2mcGroupMember(l2mcKey, vlan_alias, port))
                {
                    if (isMemberRefCntZero(vlan_alias, l2mcKey))
                        RemoveL2mcEntry(l2mcKey, vlan);

                    it = consumer.m_toSync.erase(it);
                }
                else
                    it++;
            }
            else
                it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("doL2mcMemberTask: Unknown operation type %s\n", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void L2mcOrch::addMrouterPortToL2mcEntries(string vlan, Port &port)
{
    if (m_syncdL2mcEntries.find(vlan) == m_syncdL2mcEntries.end())
        return;

    /* Add mrouter port to all groups learnt on this vlan*/
    auto itr = m_syncdL2mcEntries.at(vlan).begin(); 
    while (itr != m_syncdL2mcEntries.at(vlan).end())
    {
        auto l2mcKey = itr->first;
        AddL2mcMrouterPort(l2mcKey, vlan, port);
        itr++;
    }
    return;
}

void L2mcOrch::removeMrouterPortFromL2mcEntries(string vlan, string mrouterport)
{
    auto iter = mrouter_ports_per_vlan[vlan].begin();
    while(iter != mrouter_ports_per_vlan[vlan].end())
    {
        if (*iter == mrouterport)
        {
            mrouter_ports_per_vlan[vlan].erase(iter);
            SWSS_LOG_NOTICE("removeMrouterPortFromL2mcEntries: Mrouter port %s deleted from vlan %s", mrouterport.c_str(), vlan.c_str());

            /* When no l2mc entries learnt on this vlan*/
            if (m_syncdL2mcEntries.find(vlan) == m_syncdL2mcEntries.end())
                return;
           
            /* Remove mrouter port from all the groups learnt on this vlan if present*/
            auto itr = m_syncdL2mcEntries.at(vlan).begin(); 
            while (itr != m_syncdL2mcEntries.at(vlan).end())
            {
                auto l2mcKey = itr->first;
                RemoveL2mcMrouterPort(l2mcKey, vlan, mrouterport);
                itr++;
            }            
            break;
        }
        iter++;
    }
    return;
}

void L2mcOrch::doL2mcMrouterTask(Consumer &consumer)
{
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
        vector<string> keys = tokenize(kfvKey(t), ':');
        
        /* Key: <VLAN_name>:<mrouter_port> */

        /* Ensure the key size is 1 otherwise ignore */
        if (keys.size() != 2)
        {
            SWSS_LOG_ERROR("Invalid key size, skipping %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        /* Ensure the key starts with "Vlan" otherwise ignore */
        if (strncmp(keys[0].c_str(), VLAN_PREFIX, 4))
        {
            SWSS_LOG_ERROR("doL2mcMrouterTask: Invalid key format. No 'Vlan' prefix: %s", keys[0].c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        unsigned short vlan_id;
        std::string port_alias, vlan_alias;
        Port port, vlan;

        vlan_id = (unsigned short) stoi(keys[0].substr(4));
        vlan_alias = VLAN_PREFIX + to_string(vlan_id);
        port_alias = keys[1];

        if (!gPortsOrch->getPort(port_alias, port))
        {
            SWSS_LOG_ERROR("doL2mcMrouterTask: Failed to get port for %s alias", port_alias.c_str());
            l2mcdbg_counters.l2mc_port_fail++;
            continue;
        }

        if (!gPortsOrch->getPort(vlan_alias, vlan))
        {
            SWSS_LOG_NOTICE("doL2mcMrouterTask: Failed to locate vlan %s", vlan_alias.c_str());
            l2mcdbg_counters.l2mc_vlan_fail++;
            continue;
        }

        if (op == SET_COMMAND)
        {
            mrouter_ports_per_vlan[vlan_alias].push_back(port_alias);
            addMrouterPortToL2mcEntries(vlan_alias, port);
            SWSS_LOG_NOTICE("doL2mcMrouterTask: Mrouter port %s added to vlan %s", port_alias.c_str(), vlan_alias.c_str());
        }
        else if (op == DEL_COMMAND)
        { 
            auto mrouter_ports = mrouter_ports_per_vlan[vlan_alias];
            
            if(!mrouter_ports.empty())
                removeMrouterPortFromL2mcEntries(vlan_alias, port_alias);
        }
        else
        {
            SWSS_LOG_ERROR("doL2mcMrouterTask: Unknown operation type %s\n", op.c_str());
        }
        it = consumer.m_toSync.erase(it);
    }
}

void L2mcOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    if (table_name == APP_L2MC_VLAN_TABLE_NAME)
    {
        SWSS_LOG_INFO("Received APP_L2MC_VLAN_TABLE_NAME update");
        doL2mcTask(consumer);
    }
    else if (table_name == APP_L2MC_MEMBER_TABLE_NAME)
    {
        SWSS_LOG_INFO("Received APP_L2MC_MEMBER_TABLE_NAME update");
        doL2mcMemberTask(consumer);
    }
    else if (table_name == APP_L2MC_MROUTER_TABLE_NAME)
    {
        SWSS_LOG_INFO("Received APP_L2MC_MROUTER_TABLE_NAME update");
        doL2mcMrouterTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown L2mc Table");
        return;
    }
}



/*
 * L2mcOrch:
 * show debug l2mcOrch entries  ===>  dumps all l2mc entries
 * show debug l2mcOrch l2mcGrp2Mbrs ===>  L2mc group with members 
 * show debug l2mcOrch counters ===>  dump all internal counters
 * show debug l2mcOrch dumpall   ===> dump all internal db entries and counters
 **/


bool L2mcOrch::debugdumpCLI(KeyOpFieldsValuesTuple t)
{
    string keywd = kfvKey(t);
    string group = "dumpall";
    string vlan = "";

    SWSS_LOG_ENTER();

    if (keywd != gL2mcOrchDbgComp)
    {
        return true;
    }

    for (auto i : kfvFieldsValues(t))
    {
        if (fvField(i) == "group")
            group = fvValue(i);
        else if(fvField(i) == "vlan")
            vlan = fvValue(i);
        else
        {
            string field = fvField(i);
            string value = fvValue(i);
            SWSS_DEBUG_PRINT(gL2mcOrchDbgComp, "L2mcOrch: Rcvd field %s, Value %s", field.c_str(),value.c_str());
        }
    }

    if (group == "entries")
    {
        if (!vlan.empty())
            debugdump_l2mcEntries(vlan, false);
        else
        {
            for (auto &v : m_snoop_enabled_vlans)
                debugdump_l2mcEntries(v, false);
        }
    }
    else if (group == "grp2members")
    {
        if (!vlan.empty())
            debugdump_l2mcEntries(vlan, true);
        else
        {
            for (auto &v : m_snoop_enabled_vlans)
                debugdump_l2mcEntries(v, true);
        }
    }
    else if (group == "counters")
    {
        debugdump_l2mcDbgCounters();
    }
    else if (group == "mrouter_ports")
    {        
        if (!vlan.empty())
            debugdump_l2mcMrouterPorts(vlan);
        else
        {
            for (auto &v : m_snoop_enabled_vlans)
                debugdump_l2mcMrouterPorts(v);
        }
    }
    else if (group == "snoop_vlans")
    {
        debugShowSnoopVlans();
    }
    else if (group == "all")
    {
        l2mcDbgDumpAll();
    }
    else
    {
        SWSS_LOG_DEBUG("UnSupported Group/Option %s for L2mcOrch", group.c_str());
        SWSS_DEBUG_PRINT(gL2mcOrchDbgComp, "L2mcOrch: UnSupported Group/Option %s", group.c_str());
    }
    return true;
}


void L2mcOrch::clearAllL2mcDbgCounters()
{
    memset (&l2mcdbg_counters, 0, sizeof (struct L2mcDebugCounters));
    SWSS_LOG_INFO("L2mcOrch cleared all L2mc Debug counters");
    return;
}

void L2mcOrch::debugdump_l2mcGrps2Mbrs(string vlan, L2mcGroupMembers &l2mcgrpMembers)
{
    SWSS_LOG_ENTER();
    
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp, "%-20s %-25s %-20s Members_OID", "", "", "");
    
    auto it = l2mcgrpMembers.begin();               
    while(it != l2mcgrpMembers.end())
    {
        SWSS_DEBUG_PRINT(gL2mcOrchDbgComp, "%-20s %-25s %-20s 0x%lx", 
                                "", "", "", it->second);
        
        it++;
    }
    return;
}


void L2mcOrch::debugdump_l2mcEntries(string vlan, bool members)
{
    SWSS_LOG_ENTER();
   
    if (m_syncdL2mcEntries.find(vlan) == m_syncdL2mcEntries.end())
    {
        SWSS_DEBUG_PRINT(gL2mcOrchDbgComp, " No L2mc entries found on %s \n", vlan.c_str());
        return;
    }
    
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp, "------------L2mc Entry Table ------------------------------------------------------------\n");
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp, "Source               Group                     Vlan                 L2mcGrp-SAI-OID:MbrsCnt" );

    int l2mc_entry_count=0;
    auto it = m_syncdL2mcEntries.at(vlan).begin();
    while (it != m_syncdL2mcEntries.at(vlan).end())
    {
        SWSS_DEBUG_PRINT(gL2mcOrchDbgComp, "\n%-20s %-25s %-20s 0x%lx %d", 
            it->first.source_address.to_string().c_str(), it->first.group_address.to_string().c_str(), it->first.vlan_alias.c_str(), 
            it->second.l2mc_group_id, it->second.ref_count);
        
        if(members)
            debugdump_l2mcGrps2Mbrs(vlan, it->second.l2mc_group_members);

        it++;
        l2mc_entry_count++;
    }
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp, "\n Total number of L2mc entries : %d\n", l2mc_entry_count);
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp, "------------L2mc Entry Table End---------------------------------------------------------\n");
    return;
}

void L2mcOrch::debugdump_l2mcMrouterPorts(string vlan)
{
    SWSS_LOG_ENTER();

    auto mrouter_ports = mrouter_ports_per_vlan[vlan];

    if(!mrouter_ports.empty())
    {
        SWSS_DEBUG_PRINT(gL2mcOrchDbgComp, "%s Mrouter List:", vlan.c_str());

        for (const auto& mrouter: mrouter_ports)
        {
            SWSS_DEBUG_PRINT(gL2mcOrchDbgComp, "%s, ", mrouter.c_str());
        }
    }
    else
    {
        SWSS_DEBUG_PRINT(gL2mcOrchDbgComp, "L2mc Mrouter Port table is empty on vlan %s", vlan.c_str());
    }
    return;
}

void L2mcOrch::debugShowSnoopVlans()
{
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp, "IGMP Snooping Enabled Vlans:");
    for (auto &v : m_snoop_enabled_vlans)
    {
        SWSS_DEBUG_PRINT(gL2mcOrchDbgComp, "  %s", v.c_str());
    }
}

void L2mcOrch::debugdump_l2mcDbgCounters()
{    
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc Counters");
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"----------------------------------------------");
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"Total L2mc Entries                   : %d", (l2mcdbg_counters.l2mc_entry_add - l2mcdbg_counters.l2mc_entry_del));
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc Entry Add                       : %d", l2mcdbg_counters.l2mc_entry_add);
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc Entry Delete                    : %d", l2mcdbg_counters.l2mc_entry_del);
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc Group Add                       : %d", l2mcdbg_counters.l2mc_group_add);
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc Group Delete                    : %d", l2mcdbg_counters.l2mc_group_del);
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc Member Add                      : %d", l2mcdbg_counters.l2mc_member_add);
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc Member Delete                   : %d", l2mcdbg_counters.l2mc_member_del);
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc Entry Add Fail                  : %d", l2mcdbg_counters.l2mc_entry_add_fail);
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc Entry Delete Fail               : %d", l2mcdbg_counters.l2mc_entry_del_fail);
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc Group Add Fail                  : %d", l2mcdbg_counters.l2mc_group_add_fail);
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc Group Delete Fail               : %d", l2mcdbg_counters.l2mc_group_del_fail);
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc Member Add fail                 : %d", l2mcdbg_counters.l2mc_member_add_fail);
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc Member Delete Fail              : %d", l2mcdbg_counters.l2mc_member_del_fail);
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc Port get Fail                   : %d", l2mcdbg_counters.l2mc_port_fail);
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc Port OID NULL                   : %d", l2mcdbg_counters.l2mc_port_oid_fail);
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc vlan get Fail                   : %d", l2mcdbg_counters.l2mc_vlan_fail);
    SWSS_DEBUG_PRINT(gL2mcOrchDbgComp,"L2mc vlan OID NULL                   : %d", l2mcdbg_counters.l2mc_vlan_oid_fail);

    return;
}

void L2mcOrch::l2mcDbgDumpAll()
{
    debugShowSnoopVlans();

    for (auto &v : m_snoop_enabled_vlans)
    {
        debugdump_l2mcMrouterPorts(v);
        debugdump_l2mcEntries(v, true);
    }
    debugdump_l2mcDbgCounters();
}
