#include <assert.h>
#include <algorithm>
#include "l2mcorch.h"
#include "logger.h"
#include "swssnet.h"
#include  "tokenize.h"
//#include  "errororch.h"
#include "sai_serialize.h"
#include "redisclient.h"


using namespace swss;

#define VLAN_PREFIX "Vlan"

extern sai_object_id_t gSwitchId;

extern sai_vlan_api_t *sai_vlan_api;
extern sai_switch_api_t*            sai_switch_api;
extern sai_l2mc_api_t*              sai_l2mc_entry_api;
extern sai_l2mc_group_api_t*        sai_l2mc_group_api;

extern PortsOrch *gPortsOrch;

extern L2mcOrch *gL2mcOrch;
extern PortsOrch*        gPortsOrch;

L2mcOrch::L2mcOrch(DBConnector * appDb, vector<string> &tableNames) :
    Orch(appDb, tableNames)
{
    SWSS_LOG_ENTER();
    gPortsOrch->attach(this);
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

bool L2mcOrch::isSuppressionEnabled(string vlan_alias) const
{
    auto it = m_vlan_suppress_info.find(vlan_alias);
    return it != m_vlan_suppress_info.end() &&
           (it->second.unknown_ipv4_enabled || it->second.link_local_enabled);
}

bool L2mcOrch::bake()
{
    SWSS_LOG_ENTER();

    addExistingData(APP_L2MC_VLAN_TABLE_NAME);
    addExistingData(APP_L2MC_MEMBER_TABLE_NAME);
    addExistingData(APP_L2MC_MROUTER_TABLE_NAME);

    return true;
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
        return false;
    }

    l2mc_group->second.l2mc_group_members[port.m_alias].l2mc_member_id = l2mc_member_id;
    l2mc_group->second.l2mc_group_members[port.m_alias].is_l2mc_member = true;
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
        return false;
    }

    auto l2mc_group = m_syncdL2mcEntries.at(vlan_alias).find(l2mc_GrpKey);
    auto l2mc_group_member = l2mc_group->second.l2mc_group_members.find(port.m_alias);
    /* l2mc group member already deleted or doesn't exists */
    if(l2mc_group_member == l2mc_group->second.l2mc_group_members.end())
        return true;
    
    /* Retain l2mc group member from deletion if mrouter port learnt on the
     * same member port where delete request recieved due to IGMP leave*/
    auto mrouter_ports = mrouter_ports_per_vlan[vlan_alias];
    if(!mrouter_ports.empty())
    {
        auto v = find(mrouter_ports.begin(), mrouter_ports.end(), port.m_alias);

        if(v != mrouter_ports.end())
        {
            SWSS_LOG_NOTICE("L2mc mrouter port is same as group member port (%s, %s, %d)", l2mc_GrpKey.source_address.to_string().c_str(),
                    l2mc_GrpKey.group_address.to_string().c_str(), vlan.m_vlan_info.vlan_id);
        
            if (l2mc_group_member != l2mc_group->second.l2mc_group_members.end())
            {
                l2mc_group->second.l2mc_group_members[port.m_alias].is_l2mc_member = false;
            }
            /* If it is the last memember, IGMP leave should be allowed to delete the member */
            if(m_syncdL2mcEntries[vlan_alias][l2mc_GrpKey].ref_count != 1)
                return true;
        }
    }

    SWSS_LOG_NOTICE("RemoveL2mcGroupMember: (%s,%s,%s) Delete l2mc group member %s from l2mc_gid:%lx", 
                    l2mc_GrpKey.source_address.to_string().c_str(), l2mc_GrpKey.group_address.to_string().c_str(), 
                    l2mc_GrpKey.vlan_alias.c_str(), port.m_alias.c_str(), l2mc_group->second.l2mc_group_id);
    
    if (l2mc_group_member != l2mc_group->second.l2mc_group_members.end())
    {
        status = sai_l2mc_group_api->remove_l2mc_group_member(l2mc_group_member->second.l2mc_member_id);
        
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("RemoveL2mcGroupMember: Failed to remove l2mc group member %lx, from l2mc-grp:%lx rv:%d",
                           l2mc_group_member->second.l2mc_member_id, l2mc_group->second.l2mc_group_id, status);
            return false;
        }
        decreaseL2mcMemberRefCount(vlan_alias, l2mc_GrpKey);
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

bool L2mcOrch::RemoveL2mcGroupMembers(const L2mcGroupKey &l2mc_GrpKey, string vlan_alias)
{
    sai_status_t status;
    Port vlan;

    SWSS_LOG_ENTER();

    if (!gPortsOrch->getPort(vlan_alias, vlan))
    {
        SWSS_LOG_NOTICE("RemoveL2mcGroupMembers: Failed to locate vlan %s", vlan_alias.c_str());
        return false;
    }

    auto l2mc_group = m_syncdL2mcEntries.at(vlan_alias).find(l2mc_GrpKey);

    SWSS_LOG_NOTICE("RemoveL2mcGroupMembers: (%s,%s,%s) Delete l2mc group members from l2mc_gid:%lx", 
                    l2mc_GrpKey.source_address.to_string().c_str(), l2mc_GrpKey.group_address.to_string().c_str(), 
                    l2mc_GrpKey.vlan_alias.c_str(), l2mc_group->second.l2mc_group_id);
    
    auto l2mc_group_member = l2mc_group->second.l2mc_group_members.begin();
    while(l2mc_group_member != l2mc_group->second.l2mc_group_members.end())
    {
        Port port;
        if (!gPortsOrch->getPort(l2mc_group_member->first, port))
        {
            SWSS_LOG_NOTICE("RemoveL2mcGroupMembers: Failed to locate port %s", port.m_alias.c_str());
            continue;
        }
        SWSS_LOG_NOTICE("RemoveL2mcGroupMembers: (%s,%s,%s) Delete l2mc group member %s from l2mc_gid:%lx", 
                    l2mc_GrpKey.source_address.to_string().c_str(), l2mc_GrpKey.group_address.to_string().c_str(), 
                    l2mc_GrpKey.vlan_alias.c_str(), port.m_alias.c_str(), l2mc_group->second.l2mc_group_id);
        
        status = sai_l2mc_group_api->remove_l2mc_group_member(l2mc_group_member->second.l2mc_member_id);
        
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("RemoveL2mcGroupMembers: Failed to remove l2mc group member %lx, from l2mc-grp:%lx rv:%d",
                           l2mc_group_member->second.l2mc_member_id, l2mc_group->second.l2mc_group_id, status);
            return false;
        }
        decreaseL2mcMemberRefCount(vlan_alias, l2mc_GrpKey);
        l2mc_group_member = l2mc_group->second.l2mc_group_members.erase(l2mc_group_member);

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

    // if (port.m_bridge_port_id == SAI_NULL_OBJECT_ID)
    // {
    //     SWSS_LOG_NOTICE("AddL2mcEntry: NULL port OID"); 
    //     return false;
    // }

    if (vlan.m_vlan_info.vlan_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_NOTICE("AddL2mcEntry: NULL vlan OID");
        return false;
    }

    if (!hasL2mcGroup(vlan.m_alias,l2mcGrpKey))
    {
        if (m_pend_l2mc_group_id != SAI_NULL_OBJECT_ID )
        {
            l2mc_group_id = m_pend_l2mc_group_id;
            m_pend_l2mc_group_id = SAI_NULL_OBJECT_ID;
        }
        else
        {
            status = sai_l2mc_group_api->create_l2mc_group(&l2mc_group_id, gSwitchId, 0 , NULL);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("AddL2mcEntry: Failed to add l2mc group %lx, rv:%d", l2mc_group_id, status);
                return false;
            }
            SWSS_LOG_NOTICE("AddL2mcEntry: success add l2mc group %lx, rv:%d", l2mc_group_id, status);
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


    SWSS_LOG_NOTICE("AddL2mcEntry: create l2mc entry  (%s, %s, %d) ",
                        l2mcGrpKey.source_address.to_string().c_str(), l2mcGrpKey.group_address.to_string().c_str(), vlan.m_vlan_info.vlan_id);
    

    status = sai_l2mc_entry_api->create_l2mc_entry(&l2mc_entry, (uint32_t)l2mc_entry_attrs.size(),
                                                    l2mc_entry_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("AddL2mcEntry: Failed to create l2mc entry  (%s, %s, %d) rv:%d",
                        l2mcGrpKey.source_address.to_string().c_str(), l2mcGrpKey.group_address.to_string().c_str(), vlan.m_vlan_info.vlan_id, status);
        m_pend_l2mc_group_id = l2mc_group_id;
        return false;
    }

    if (!hasL2mcGroup(vlan.m_alias, l2mcGrpKey))
        m_syncdL2mcEntries[vlan.m_alias][l2mcGrpKey] = l2mc_group_entry;

    
    if (port.m_bridge_port_id != SAI_NULL_OBJECT_ID)
    {
        AddL2mcGroupMember(l2mcGrpKey, vlan.m_alias, port);
    }
    
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
        return false;
    }
    l2mc_group_id = getL2mcGroupId(vlan.m_alias, key);

    SWSS_LOG_NOTICE("RemoveL2mcEntry: (%s,%s,%d) Delete l2mc group %lx", key.source_address.to_string().c_str(),
                    key.group_address.to_string().c_str(), vlan.m_vlan_info.vlan_id, l2mc_group_id);

    /* Remove all mrouter ports added as group members */
    for (auto l2mc_group_member = l2mc_group->second.l2mc_group_members.begin();
         l2mc_group_member != l2mc_group->second.l2mc_group_members.end();)
    {
        status = sai_l2mc_group_api->remove_l2mc_group_member(l2mc_group_member->second.l2mc_member_id);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("RemoveL2mcEntry: Failed to remove mrouter from l2mc group member %lx, rv:%d",
                           l2mc_group_member->second.l2mc_member_id, status);
            return false;
        }
        l2mc_group_member = l2mc_group->second.l2mc_group_members.erase(l2mc_group_member);
    }
    /* Remove l2mc group */
    status = sai_l2mc_group_api->remove_l2mc_group(l2mc_group_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("RemoveL2mcEntry: Failed to remove l2mc group %lx, rv:%d", l2mc_group_id, status);
        return false;
    }

    m_syncdL2mcEntries.at(vlan.m_alias).erase(key);

    return true;
}

bool L2mcOrch::RemoveL2mcEntrys(Port &vlan)
{
    sai_l2mc_entry_t l2mc_entry;
    sai_object_id_t l2mc_group_id;
    sai_status_t status;

    SWSS_LOG_ENTER();

    auto l2mc_group = m_syncdL2mcEntries.at(vlan.m_alias).begin();

    while(l2mc_group != m_syncdL2mcEntries.at(vlan.m_alias).end())
    {
        auto key = l2mc_group->first;
        /* Remove all ports added as group members */
        RemoveL2mcGroupMembers(key,vlan.m_alias);

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
            SWSS_LOG_ERROR("RemoveL2mcEntrys: Failed to remove l2mc entry  (%s, %s, %d) rv:%d",
                            key.source_address.to_string().c_str(), key.group_address.to_string().c_str(), vlan.m_vlan_info.vlan_id, status);
            return false;
        }
        l2mc_group_id = getL2mcGroupId(vlan.m_alias, key);

        SWSS_LOG_NOTICE("RemoveL2mcEntrys: (%s,%s,%d) Delete l2mc group %lx", key.source_address.to_string().c_str(),
                        key.group_address.to_string().c_str(), vlan.m_vlan_info.vlan_id, l2mc_group_id);

        /* Remove all mrouter ports added as group members */
        for (auto l2mc_group_member = l2mc_group->second.l2mc_group_members.begin();
            l2mc_group_member != l2mc_group->second.l2mc_group_members.end();)
        {
            status = sai_l2mc_group_api->remove_l2mc_group_member(l2mc_group_member->second.l2mc_member_id);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("RemoveL2mcEntrys: Failed to remove mrouter from l2mc group member %lx, rv:%d",
                            l2mc_group_member->second.l2mc_member_id, status);
                return false;
            }
            l2mc_group_member = l2mc_group->second.l2mc_group_members.erase(l2mc_group_member);
        }
        /* Remove l2mc group */
        status = sai_l2mc_group_api->remove_l2mc_group(l2mc_group_id);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("RemoveL2mcEntrys: Failed to remove l2mc group %lx, rv:%d", l2mc_group_id, status);
            return false;
        }

        l2mc_group = m_syncdL2mcEntries.at(vlan.m_alias).erase(l2mc_group);
    }

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
        return false;
    }
    l2mc_group->second.l2mc_group_members[port.m_alias].l2mc_member_id = l2mc_member_id;
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
    if (l2mc_group_member != l2mc_group->second.l2mc_group_members.end() && !(l2mc_group->second.l2mc_group_members[port].is_l2mc_member))
    {
        status = sai_l2mc_group_api->remove_l2mc_group_member(l2mc_group_member->second.l2mc_member_id);
        
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("RemoveL2mcMrouterPort: Failed to remove l2mc mrouter group member %lx, from l2mc-grp:%lx rv:%d",
                           l2mc_group_member->second.l2mc_member_id, l2mc_group->second.l2mc_group_id, status);
            return false;
        }
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
        auto v = find(m_pend_snoop_enabled_vlans.begin(), m_pend_snoop_enabled_vlans.end(), vlan_alias);
        if (v == m_pend_snoop_enabled_vlans.end())
            m_pend_snoop_enabled_vlans.push_back(vlan_alias);
        return false;
    }

    if (vlan.m_vlan_info.vlan_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_NOTICE("EnableIgmpSnooping: NULL vlan OID");
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
    auto v = find(m_pend_snoop_enabled_vlans.begin(), m_pend_snoop_enabled_vlans.end(), vlan_alias);
    if (v != m_pend_snoop_enabled_vlans.end())
        m_pend_snoop_enabled_vlans.erase(v);
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

bool L2mcOrch::handleL2mcSuppression(const std::string &vlan_alias, bool enable, L2mcSuppressType type)
{
    SWSS_LOG_ENTER();

    Port vlan, port, mrport;
    sai_attribute_t attr;
    bzero(&attr, sizeof(attr));

    if (!gPortsOrch->getPort(vlan_alias, vlan))
    {
        SWSS_LOG_WARN("VLAN %s not found", vlan_alias.c_str());
        return false;
    }

    auto v = find(m_snoop_enabled_vlans.begin(), m_snoop_enabled_vlans.end(), vlan_alias);
    if (v == m_snoop_enabled_vlans.end()) 
    {
        if (enable) 
        {
            SWSS_LOG_WARN("VLAN %s not enable IGMP Snooping", vlan_alias.c_str());
            return false;
        } else 
        {
            SWSS_LOG_WARN("VLAN %s disable IGMP Snooping, L2MC suppression false, ignore", vlan_alias.c_str());
            return true;
        }
    }
    
    auto &info = m_vlan_suppress_info[vlan_alias];

    if ((type == L2mcSuppressType::UNKNOWN_IPV4 && info.unknown_ipv4_enabled == enable) ||
        (type == L2mcSuppressType::LINK_LOCAL && info.link_local_enabled == enable))
    {
        SWSS_LOG_NOTICE("L2MC suppression status unchanged for VLAN %s [%s]", 
                        vlan_alias.c_str(),
                        (type == L2mcSuppressType::UNKNOWN_IPV4 ? "Unknown IPv4" : "Link-local"));
        return true;
    }


    if (type == L2mcSuppressType::UNKNOWN_IPV4 )
    {
        info.unknown_ipv4_enabled = enable;
    }
    else if (type == L2mcSuppressType::LINK_LOCAL)
    {
        info.link_local_enabled = enable;
    }  

    
    if (enable && info.group_id == SAI_NULL_OBJECT_ID)
    {
        auto l2mcKey = L2mcGroupKey(vlan_alias, IpAddress("0.0.0.0"), IpAddress("0.0.0.0"));
        if (m_syncdL2mcEntries.find(vlan.m_alias) == m_syncdL2mcEntries.end())
        {
            m_syncdL2mcEntries.emplace(vlan.m_alias, L2mcEntryTable());
        }

        auto &mrouter_ports = mrouter_ports_per_vlan[vlan_alias];
        bool groupCreated = false;

        if (mrouter_ports.empty())
        {
            if (!hasL2mcGroup(vlan_alias, l2mcKey))
            {
                if (!AddL2mcEntry(l2mcKey, vlan, port))
                {
                    return false;
                }
            }
        }
        else
        {
            for (const auto &mrouter : mrouter_ports)
            {
                if (!hasL2mcGroup(vlan_alias, l2mcKey))
                {
                    if (gPortsOrch->getPort(mrouter, mrport))
                    {
                        if (AddL2mcEntry(l2mcKey, vlan, mrport))
                        {
                            groupCreated = true;
                        }
                    }
                }
                else if (gPortsOrch->getPort(mrouter, mrport) && groupCreated)
                {
                    AddL2mcGroupMember(l2mcKey, vlan_alias, mrport);
                }
            }
        }

        info.group_id = getL2mcGroupId(vlan_alias, l2mcKey);
    }

    
    attr.value.oid = enable ? info.group_id : SAI_NULL_OBJECT_ID;
    attr.id = (type == L2mcSuppressType::UNKNOWN_IPV4) ?
              SAI_VLAN_ATTR_UNKNOWN_IPV4_MCAST_OUTPUT_GROUP_ID :
              SAI_VLAN_ATTR_UNKNOWN_LINKLOCAL_MCAST_OUTPUT_GROUP_ID;

    sai_status_t status = sai_vlan_api->set_vlan_attribute(vlan.m_vlan_info.vlan_oid, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set L2MC suppression for VLAN %s, attr %d, status %u",
                       vlan_alias.c_str(), attr.id, status);
        return false;
    }

    
    if (!info.unknown_ipv4_enabled && !info.link_local_enabled)
    {
        
        m_vlan_suppress_info.erase(vlan_alias);
        SWSS_LOG_NOTICE("Suppression group for VLAN %s fully disabled and removed", vlan_alias.c_str());
    }

    SWSS_LOG_NOTICE("L2MC suppression %s for VLAN %s: %s",
                    (type == L2mcSuppressType::UNKNOWN_IPV4) ? "Unknown IPv4" : "Link-local",
                    vlan_alias.c_str(), enable ? "enabled" : "disabled");

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
        SWSS_LOG_NOTICE("doL2mcMemberTask: vlan_alias %s enable op %s", vlan_alias.c_str(),op.c_str());

        if (op == SET_COMMAND)
        {
            auto v = find(m_snoop_enabled_vlans.begin(), m_snoop_enabled_vlans.end(), vlan_alias);
            if (v == m_snoop_enabled_vlans.end())
            {
                m_snoop_enabled_vlans.push_back(vlan_alias);
                if(EnableIgmpSnooping(vlan_alias))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else
                it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            if (m_syncdL2mcEntries.find(vlan_alias) == m_syncdL2mcEntries.end())
            {
                SWSS_LOG_NOTICE("doL2mcTask: l2mc already deleted from vlan %s",vlan_alias.c_str());
            }
            else 
            {
                Port vlan;
                if (!gPortsOrch->getPort(vlan_alias, vlan))
                {
                    SWSS_LOG_NOTICE("doL2mcTask: Failed to locate vlan %s", vlan_alias.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                /*Remove all l2mc form vlan*/
                RemoveL2mcEntrys(vlan);
            }
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
            continue;
        }

        if (!gPortsOrch->getPort(vlan_alias, vlan))
        {
            SWSS_LOG_NOTICE("doL2mcMemberTask: Failed to locate vlan %s", vlan_alias.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        auto l2mcKey = L2mcGroupKey(vlan_alias, IpAddress(keys[1]), IpAddress(keys[2]));

        if (m_syncdL2mcEntries.find(vlan.m_alias) == m_syncdL2mcEntries.end())
        {
            m_syncdL2mcEntries.emplace(vlan.m_alias, L2mcEntryTable());
        }
        SWSS_LOG_NOTICE("doL2mcMemberTask: set vlan %s op %s", vlan_alias.c_str(),op.c_str());

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

bool L2mcOrch::addMrouterPortToL2mcEntries(string vlan, Port &port)
{
    if (m_syncdL2mcEntries.find(vlan) == m_syncdL2mcEntries.end())
        return true;
    
    if (port.m_bridge_port_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_NOTICE("AddL2mcMrouterPort: NULL port OID"); 
        return false;
    }
    /* Add mrouter port to all groups learnt on this vlan*/
    auto itr = m_syncdL2mcEntries.at(vlan).begin(); 
    while (itr != m_syncdL2mcEntries.at(vlan).end())
    {
        auto l2mcKey = itr->first;
        AddL2mcMrouterPort(l2mcKey, vlan, port);
        itr++;
    }
    return true;
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
            continue;
        }

        if (!gPortsOrch->getPort(vlan_alias, vlan))
        {
            SWSS_LOG_NOTICE("doL2mcMrouterTask: Failed to locate vlan %s", vlan_alias.c_str());
            continue;
        }

        if (op == SET_COMMAND)
        {
            mrouter_ports_per_vlan[vlan_alias].push_back(port_alias);
            if (isSuppressionEnabled(vlan_alias))
            {
                auto l2mcKey = L2mcGroupKey(vlan_alias, IpAddress("0.0.0.0"), IpAddress("0.0.0.0"));

                if (!AddL2mcGroupMember(l2mcKey, vlan_alias, port))
                {
                    SWSS_LOG_WARN("Failed to add L2MC group member for VLAN %s port %s", vlan_alias.c_str(), port_alias.c_str());
                }
            }

            if (addMrouterPortToL2mcEntries(vlan_alias, port))
            {
                SWSS_LOG_NOTICE("doL2mcMrouterTask: Mrouter port %s added to vlan %s", port_alias.c_str(), vlan_alias.c_str());
                it = consumer.m_toSync.erase(it);
            }
            else 
                it++;
        }
        else if (op == DEL_COMMAND)
        { 
            auto mrouter_ports = mrouter_ports_per_vlan[vlan_alias];

            if (!mrouter_ports.empty() && isSuppressionEnabled(vlan_alias))
            {
                auto l2mcKey = L2mcGroupKey(vlan_alias, IpAddress("0.0.0.0"), IpAddress("0.0.0.0"));
                if (!RemoveL2mcGroupMember(l2mcKey, vlan_alias, port))
                {
                    SWSS_LOG_WARN("Failed to remove L2MC group member for VLAN %s port %s", vlan_alias.c_str(), port_alias.c_str());
                }
            }

            if(!mrouter_ports.empty())
                removeMrouterPortFromL2mcEntries(vlan_alias, port_alias);
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("doL2mcMrouterTask: Unknown operation type %s\n", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void L2mcOrch::doL2mcSuppressTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple &tuple = it->second;
        string key = kfvKey(tuple);
        string op = kfvOp(tuple);

        if (strncmp(key.c_str(), VLAN_PREFIX, strlen(VLAN_PREFIX)) != 0)
        {
            SWSS_LOG_ERROR("Invalid key format. Expected prefix 'Vlan': %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        string vlan_alias = key;
        bool need_retry  = false;

        if (op == SET_COMMAND)
        {
            for (const auto &fv : kfvFieldsValues(tuple))
            {
                const string &attr = fvField(fv);
                const string &value = fvValue(fv);
                bool enable = false;

                if (value == "enable")
                {
                    enable = true;
                }
                else if (value == "disable")
                {
                    enable = false;
                }
                else
                {
                    SWSS_LOG_ERROR("Invalid value '%s' for attribute '%s' in VLAN %s", value.c_str(), attr.c_str(), vlan_alias.c_str());
                    continue;
                }

                if (attr == "optimised-multicast-flood")
                {
                    SWSS_LOG_NOTICE("Setting handleL2mcSuppression-multicast-flood on %s to %s", vlan_alias.c_str(), value.c_str());
                    if(!handleL2mcSuppression(vlan_alias, enable, L2mcSuppressType::UNKNOWN_IPV4))
                    {
                        SWSS_LOG_NOTICE("Retry Setting optimised-multicast-flood on %s to %s", vlan_alias.c_str(), value.c_str());
                        need_retry  = true;
                    }
                }
                else if (attr == "link-local-groups-suppression")
                {
                    SWSS_LOG_NOTICE("Setting link-local-groups-suppression on %s to %s", vlan_alias.c_str(), value.c_str());

                    if(!handleL2mcSuppression(vlan_alias, enable, L2mcSuppressType::LINK_LOCAL))
                    {
                        SWSS_LOG_NOTICE("Retry Setting link-local-groups-suppression on %s to %s", vlan_alias.c_str(), value.c_str());
                        need_retry  = true;
                    }
                }
                else
                {
                    SWSS_LOG_WARN("Unknown attribute '%s' for VLAN %s", attr.c_str(), vlan_alias.c_str());
                }
            }
            if (need_retry)
            {
                it++;
            }
            else
            {
                it = consumer.m_toSync.erase(it); 
            }
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("Deleting suppression config for VLAN %s (reset to default)", vlan_alias.c_str());
            // Reset both suppression types and track failures
            bool retry_unknown = !handleL2mcSuppression(vlan_alias, false, L2mcSuppressType::UNKNOWN_IPV4);
            bool retry_link_local = !handleL2mcSuppression(vlan_alias, false, L2mcSuppressType::LINK_LOCAL);
            
            if (retry_unknown || retry_link_local)
            {
                SWSS_LOG_NOTICE("Retry Deleting suppression config for VLAN %s (reset to default)", vlan_alias.c_str());
                ++it;
            }
            else
            {
                it = consumer.m_toSync.erase(it);
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation: %s", op.c_str());
            it = consumer.m_toSync.erase(it); 
        }

    }
}

bool L2mcOrch::removeL2mcFromVlan(string vlan_alias)
{
    auto v = find(m_snoop_enabled_vlans.begin(), m_snoop_enabled_vlans.end(), vlan_alias);
    if (v == m_snoop_enabled_vlans.end())
    {
        SWSS_LOG_NOTICE("removeL2mcFromVlan: %s is igmp snooping disable",vlan_alias.c_str());
        return true;
    }

    if (m_syncdL2mcEntries.find(vlan_alias) == m_syncdL2mcEntries.end())
    {
        SWSS_LOG_NOTICE("removeL2mcFromVlan: l2mc already deleted from vlan %s",vlan_alias.c_str());
        return true;
    }
    Port vlan;
    if (!gPortsOrch->getPort(vlan_alias, vlan))
    {
        SWSS_LOG_NOTICE("removeL2mcFromVlan: Failed to locate vlan %s", vlan_alias.c_str());
        return false;
    }
    /*Remove all l2mc form vlan*/
    RemoveL2mcEntrys(vlan);
    
    /*disable igmp vlan snooping before deleting vlan*/
    DisableIgmpSnooping(vlan_alias);
    auto it = find(m_pend_snoop_enabled_vlans.begin(), m_pend_snoop_enabled_vlans.end(), vlan_alias);
    if (it == m_pend_snoop_enabled_vlans.end())
        m_pend_snoop_enabled_vlans.push_back(vlan_alias);
    
    return true;

}

void L2mcOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    if (type != SUBJECT_TYPE_PORT_CHANGE)
    {
        return;
    }
    if (type == SUBJECT_TYPE_PORT_CHANGE)
    {
        PortUpdate *update = static_cast<PortUpdate *>(cntx);
        if(update->port.m_type == Port::VLAN)
        {
            string vlan_alias = update->port.m_alias;
            SWSS_LOG_INFO("L2mcOrch : Update VLAN %s", vlan_alias.c_str());
            if(update->add)
            {
                auto v = find(m_pend_snoop_enabled_vlans.begin(), m_pend_snoop_enabled_vlans.end(), vlan_alias);
                if (v == m_pend_snoop_enabled_vlans.end())
                {
                    SWSS_LOG_NOTICE("Update: %s is igmp snooping disable",vlan_alias.c_str());
                    return;
                }
                else 
                {
                    /*enable igmp vlan snooping after add vlan*/
                    SWSS_LOG_NOTICE("Update: %s is igmp snooping enable",vlan_alias.c_str());
                    EnableIgmpSnooping(vlan_alias);
                }
                return;
            }
        }
    }
    return;
    
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
    else if (table_name == APP_L2MC_SUPPRESS_TABLE_NAME)
    {
        SWSS_LOG_INFO("Received APP_L2MC_SUPPRESS_TABLE_NAME update");
        doL2mcSuppressTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown L2mc Table");
        return;
    }
}
