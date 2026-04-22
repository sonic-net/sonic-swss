#include <algorithm>
extern "C" {
#include "sai.h"
}
#include "swssnet.h"
#include "directory.h"
#include "routeorch.h"
#include "portsorch.h"
#include "vxlanorch.h"
#include "nhgorch.h"
#include "l2nhgorch.h"

extern Directory<Orch*> gDirectory;
extern RouteOrch*       gRouteOrch;
extern PortsOrch*       gPortsOrch;
extern CrmOrch*         gCrmOrch;
extern NhgOrch*         gNhgOrch;

extern sai_object_id_t gSwitchId;

extern sai_next_hop_group_api_t* sai_next_hop_group_api;
extern sai_next_hop_api_t*       sai_next_hop_api;

#define NHG_DELIMITER ','
#define NEXTHOP_GROUP_PORT_PREFIX "Port_Nexthop_Group_"

L2NhgOrch::L2NhgOrch(DBConnector* appDbConnector, string appL2NhgTable) :
    NhgOrchCommon(appDbConnector, appL2NhgTable)
{
    SWSS_LOG_ENTER();

    m_appTables.push_back(new Table(appDbConnector, appL2NhgTable));
}

L2NhgOrch::~L2NhgOrch()
{
    SWSS_LOG_ENTER();

    auto it = m_appTables.begin();

    while (it != m_appTables.end())
    {
        delete *it;
        it = m_appTables.erase(it);
    }
}

sai_object_id_t L2NhgOrch::createSaiNextHopGroup()
{
    SWSS_LOG_ENTER();
    sai_object_id_t nhg_oid = SAI_NULL_OBJECT_ID;

    /*
     * Check for the total ECMP groups created and maximum available
     */
    if (gRouteOrch->getNhgCount() + NhgOrch::getSyncedNhgCount() + getL2NhgCount() >= gRouteOrch->getMaxNhgCount())
    {
        SWSS_LOG_WARN("Failed to create L2 ECMP Group: hardware limit of groups reached (%u)",
                      gRouteOrch->getMaxNhgCount());
        return SAI_NULL_OBJECT_ID;
    }

    /* Creating a next hop group of type bridge port */
    sai_attribute_t nhg_attr;
    vector<sai_attribute_t> nhg_attrs;

    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
    nhg_attr.value.s32 = SAI_NEXT_HOP_GROUP_TYPE_BRIDGE_PORT;
    nhg_attrs.push_back(nhg_attr);

    sai_status_t status = sai_next_hop_group_api->create_next_hop_group(&nhg_oid,
                                                                        gSwitchId,
                                                                        (uint32_t)nhg_attrs.size(),
                                                                        nhg_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create an L2 Next Hop Group of type Bridge port: rc: %d", status);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_NEXT_HOP_GROUP, status);
        if (handle_status != task_success)
        {
            parseHandleSaiStatusFailure(handle_status);
        }
        return SAI_NULL_OBJECT_ID;
    }

    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
    return nhg_oid;
}

bool L2NhgOrch::removeSaiNextHopGroup(sai_object_id_t nhg_id)
{
    SWSS_LOG_ENTER();

    sai_status_t status = sai_next_hop_group_api->remove_next_hop_group(nhg_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete L2 Nexthop Group 0x%" PRIx64 ": rc: %d", nhg_id, status);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_NEXT_HOP_GROUP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
        return false;
    }
    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
    return true;
}

bool L2NhgOrch::deleteL2NextHopGroup(string nhg_id)
{
    bool status = false;
    EvpnNvoOrch* evpn_orch = gDirectory.get<EvpnNvoOrch*>();

    if (m_nhg_nh.count(nhg_id))
    {
        /* Remove bridge port and portsorch l2 nexthop group cache*/
        Port nhgPort;
        auto nhg_port_name = getNextHopGroupPortName(nhg_id);
        if (gPortsOrch->getPort(nhg_port_name, nhgPort))
        {
            status = gPortsOrch->removeBridgePort(nhgPort);
            if (!status)
            {
                SWSS_LOG_ERROR("Failed to remove bridge port of type nexthop group %s", nhg_id.c_str());
                return false;
            }
            gPortsOrch->removeL2NexthopGroup(nhgPort);
        }

        for (auto it = m_nhg_nh[nhg_id].next_hops.begin(); it !=  m_nhg_nh[nhg_id].next_hops.end();)
        {
            /* Get vtep_ptr from individual nexthop's source_vtep */
            string nh_source_vtep = m_nhg_vtep[it->first].source_vtep;
            auto vtep_ptr = evpn_orch->getEVPNVtep();
            if (!vtep_ptr)
            {
                SWSS_LOG_ERROR("Unable to find EVPN VTEP %s for nexthop %s", nh_source_vtep.c_str(), it->first.c_str());
                return false;
            }

            /* Remove all the next hops under this next hop group */
            status = removeSaiNextHop(it->second);
            if (!status)
            {
                SWSS_LOG_ERROR("Failed to remove SAI next hop for %s", nhg_id.c_str());
                return false;
            }
            m_nhg_vtep[it->first].ref_count -= 1;
            vtep_ptr->updateRemoteEndPointIpRef(m_nhg_vtep[it->first].ip, false);
            vtep_ptr->cleanupDynamicDIPTunnel(m_nhg_vtep[it->first].ip);
            if (m_nhg_vtep[it->first].ref_count == 0)
            {
                SWSS_LOG_DEBUG("Removing VTEP %s from L2NhgOrch cache", m_nhg_vtep[it->first].ip.c_str());
                m_nhg_vtep.erase(it->first);
            }
            it = m_nhg_nh[nhg_id].next_hops.erase(it);
        }

        /* Remove the empty next hop group*/
        status = removeSaiNextHopGroup(m_nhg_nh[nhg_id].oid);
        if (!status)
        {
            SWSS_LOG_ERROR("Failed to remove SAI next hop group %s", nhg_id.c_str());
            return false;
        }
        m_nhg_nh.erase(nhg_id);
    }

    return true;
}

pair<sai_object_id_t, sai_object_id_t> L2NhgOrch::createSaiNextHop(sai_object_id_t l2_nhg_id,
                                                                   sai_object_id_t tunnel_oid,
                                                                   const string& remote_vtep_ip)
{
    SWSS_LOG_ENTER();

    sai_object_id_t nh_oid = SAI_NULL_OBJECT_ID;
    sai_object_id_t nhgm_oid = SAI_NULL_OBJECT_ID;

    /* Creating a next hop of type bridge port */
    sai_attribute_t nh_attr;
    vector<sai_attribute_t> nh_attrs;

    nh_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
    nh_attr.value.s32 = SAI_NEXT_HOP_TYPE_BRIDGE_PORT;
    nh_attrs.push_back(nh_attr);

    nh_attr.id = SAI_NEXT_HOP_ATTR_IP;
    swss::copy(nh_attr.value.ipaddr, swss::IpAddress(remote_vtep_ip));
    nh_attrs.push_back(nh_attr);

    nh_attr.id = SAI_NEXT_HOP_ATTR_TUNNEL_ID;
    nh_attr.value.oid = tunnel_oid;
    nh_attrs.push_back(nh_attr);

    sai_status_t status = sai_next_hop_api->create_next_hop(&nh_oid,
                                                            gSwitchId,
                                                            (uint32_t)(nh_attrs.size()),
                                                            nh_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create an L2 Next Hop of type Bridge port: rc: %d", status);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_NEXT_HOP, status);
        if (handle_status != task_success)
        {
            parseHandleSaiStatusFailure(handle_status);
        }
        return {SAI_NULL_OBJECT_ID, SAI_NULL_OBJECT_ID};
    }

    /* Add this l2 next hop to the next hop group */
    vector<sai_attribute_t> nhgm_attrs;
    sai_attribute_t nhgm_attr;

    nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
    nhgm_attr.value.oid =  l2_nhg_id;
    nhgm_attrs.push_back(nhgm_attr);

    nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
    nhgm_attr.value.oid = nh_oid;
    nhgm_attrs.push_back(nhgm_attr);

    status = sai_next_hop_group_api->create_next_hop_group_member(&nhgm_oid,
                                                                  gSwitchId,
                                                                  (uint32_t)nhgm_attrs.size(),
                                                                  nhgm_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create an L2 Next Hop Group Member to group 0x%" PRIx64 ": rc: %d", l2_nhg_id, status);
        bool ret = removeSaiNextHop({nhgm_oid, nh_oid});
        if (!ret)
        {
            SWSS_LOG_ERROR("Failed to remove SAI next hop for 0x%" PRIx64, l2_nhg_id);
            task_process_status handle_status = handleSaiRemoveStatus(SAI_API_NEXT_HOP_GROUP, status);
            if (handle_status != task_success)
            {
                parseHandleSaiStatusFailure(handle_status);
            }
            return {SAI_NULL_OBJECT_ID, SAI_NULL_OBJECT_ID};;
        }
    }

    return {nhgm_oid, nh_oid};
}

bool L2NhgOrch::removeSaiNextHop(NhIds nh_ids)
{
    SWSS_LOG_ENTER();
    sai_status_t status;

    if (nh_ids.nhgm_oid != SAI_NULL_OBJECT_ID)
    {
        status = sai_next_hop_group_api->remove_next_hop_group_member(nh_ids.nhgm_oid);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to delete L2 Next Hop group member of type Bridge port 0x%" PRIx64 ": rc: %d", nh_ids.nhgm_oid, status);
            task_process_status handle_status = handleSaiRemoveStatus(SAI_API_NEXT_HOP, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
            return false;
        }
    }

    if (nh_ids.nh_oid != SAI_NULL_OBJECT_ID)
    {
        status = sai_next_hop_api->remove_next_hop(nh_ids.nh_oid);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to delete L2 Next Hop of type Bridge port 0x%" PRIx64 ": rc: %d", nh_ids.nh_oid, status);
            task_process_status handle_status = handleSaiRemoveStatus(SAI_API_NEXT_HOP, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
            return false;
        }
    }

    return true;
}

bool L2NhgOrch::addL2NextHopGroupEntry(string nhg_id, string nh_ids, string source_vtep)
{
    VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
    EvpnNvoOrch*     evpn_orch  = gDirectory.get<EvpnNvoOrch*>();
    vector<string>   v_nh_ids   = tokenize(nh_ids, NHG_DELIMITER);
    Port tunnel;
    vector<string> v_add_nh;
    vector<string> v_del_stale_nh;
    bool is_new_nhg = false;  // Track if we created the NHG in this call

    /*
     * Empty nexthop group. Add the new next hop group and its next hops
     */
    if (m_nhg_nh.find(nhg_id) == m_nhg_nh.end())
    {
        v_add_nh = v_nh_ids;
        sai_object_id_t nhg_oid = createSaiNextHopGroup();
        if (nhg_oid != SAI_NULL_OBJECT_ID)
        {
            m_nhg_nh[nhg_id].oid = nhg_oid;
            is_new_nhg = true;
        }
        else
        {
            SWSS_LOG_INFO("Failed to create new next hop group");
            return false;
        }
    }
    else
    {
        for (auto i : m_nhg_nh[nhg_id].next_hops)
        {
            /* Incase of an update of l2 next hop groups next hops.
             * Collect the stale nexthops for deletion
             */
            auto it = find(v_nh_ids.begin(), v_nh_ids.end(), i.first);
            if (it == v_nh_ids.end())
            {
                if (m_nhg_vtep.count(i.first))
                {
                    v_del_stale_nh.push_back(i.first);
                }
                else
                {
                    SWSS_LOG_INFO("L2 Nexthop %s pointing to a tunnel does not exist", i.first.c_str());
                    return false;
                }
            }
        }

        // Update the existing Nexthop group with the new member nexthops
        for (string i : v_nh_ids)
        {
            if (m_nhg_nh[nhg_id].next_hops.count(i) == 0)
            {
                if (m_nhg_vtep.count(i))
                {
                    v_add_nh.push_back(i);
                }
                else
                {
                    SWSS_LOG_INFO("L2 Nexthop %s was not created mapping to a tunnel", i.c_str());
                    return false;
                }
            }
        }
    }

    /*
     * Delete the stale nexthops before adding the nexthops
     */
    for (string i : v_del_stale_nh)
    {
        if (vxlan_orch->getTunnelPort(m_nhg_vtep[i].ip, tunnel, false) && tunnel.m_tunnel_id != 0)
        {
            bool ret = removeSaiNextHop(m_nhg_nh[nhg_id].next_hops[i]);
            if (!ret)
            {
                SWSS_LOG_INFO("Failed to remove SAI next hop %s for %s", i.c_str(), nhg_id.c_str());
                return false;
            }
        }
        else
        {
            SWSS_LOG_INFO("P2P Tunnel to %s does not exist", m_nhg_vtep[i].ip.c_str());
            return false;
        }
        m_nhg_nh[nhg_id].next_hops.erase(i);
        
        /* Get vtep_ptr from individual nexthop's source_vtep */
        string nh_source_vtep = m_nhg_vtep[i].source_vtep;
        auto vtep_ptr = evpn_orch->getEVPNVtep();
        if (vtep_ptr)
        {
            vtep_ptr->updateRemoteEndPointIpRef(m_nhg_vtep[i].ip, false);
            vtep_ptr->cleanupDynamicDIPTunnel(m_nhg_vtep[i].ip);
        }
        m_nhg_vtep[i].ref_count -= 1;
    }

    bool all_nh_added = true; // True if all next hops were added successfully
    for (string i : v_add_nh)
    {
        if (m_nhg_vtep.find(i) == m_nhg_vtep.end())
        {
            SWSS_LOG_INFO("L2 Nexthop %s not found in m_nhg_vtep cache, will retry", i.c_str());
            all_nh_added = false;
            continue; // Skip this NH but continue with others
        }

        if (vxlan_orch->getTunnelPort(m_nhg_vtep[i].ip, tunnel, false) && tunnel.m_tunnel_id != 0)
        {
            /* Get vtep_ptr from individual nexthop's source_vtep */
            string nh_source_vtep = m_nhg_vtep[i].source_vtep;
            auto vtep_ptr = evpn_orch->getEVPNVtep();
            if (!vtep_ptr)
            {
                SWSS_LOG_ERROR("Unable to find EVPN VTEP %s for nexthop %s", nh_source_vtep.c_str(), i.c_str());
                all_nh_added = false;
                continue;
            }

            /* Create and add next hop group member */
            auto nh_info = createSaiNextHop(m_nhg_nh[nhg_id].oid, tunnel.m_tunnel_id, m_nhg_vtep[i].ip);
            if (nh_info.first != SAI_NULL_OBJECT_ID && nh_info.second != SAI_NULL_OBJECT_ID)
            {
                // Storing next hop group member id and next hop id
                m_nhg_nh[nhg_id].next_hops[i].nhgm_oid = nh_info.first;
                m_nhg_nh[nhg_id].next_hops[i].nh_oid = nh_info.second;
                vtep_ptr->updateRemoteEndPointIpRef(m_nhg_vtep[i].ip, true);
                m_nhg_vtep[i].ref_count += 1;
            }
            else
            {
                SWSS_LOG_INFO("Failed to create next hop %s for group 0x%" PRIx64 ", will retry", i.c_str(), m_nhg_nh[nhg_id].oid);
                /* Keep already created next hops and continue with remaining */
                all_nh_added = false;
            }
        }
        else
        {
            /* Tunnel doesn't exist yet, keep already created next hops and continue with remaining */
            SWSS_LOG_INFO("P2P Tunnel to %s does not exist yet for nexthop %s part of %s, will retry", m_nhg_vtep[i].ip.c_str(), i.c_str(), nhg_id.c_str());
            all_nh_added = false;
        }
    }

    /* If we have at least one valid next hop, create/find the bridge port */
    if (!m_nhg_nh[nhg_id].next_hops.empty())
    {
        Port nhgPort;
        auto nhg_port_name = getNextHopGroupPortName(nhg_id);
        if (!gPortsOrch->getPort(nhg_port_name, nhgPort))
        {
            gPortsOrch->addL2NexthopGroup(nhg_port_name, m_nhg_nh[nhg_id].oid);
            gPortsOrch->getPort(nhg_port_name, nhgPort);
            if (!gPortsOrch->addBridgePort(nhgPort))
            {
                SWSS_LOG_ERROR("Failed to add bridge port of type next hop group for %s", nhg_id.c_str());

                /* Clean up the next hops that were just added since bridge port creation failed */
                bool cleanup_failed = false;
                for (string i : v_add_nh)
                {
                    if (m_nhg_nh[nhg_id].next_hops.count(i))
                    {
                        bool ret = removeSaiNextHop(m_nhg_nh[nhg_id].next_hops[i]);
                        if (!ret)
                        {
                            SWSS_LOG_ERROR("Failed to remove SAI next hop %s for %s during cleanup", i.c_str(), nhg_id.c_str());
                            cleanup_failed = true;
                            continue; // Try to clean up remaining NHs
                        }
                        m_nhg_nh[nhg_id].next_hops.erase(i);
                        
                        /* Get vtep_ptr from individual nexthop's source_vtep */
                        string nh_source_vtep = m_nhg_vtep[i].source_vtep;
                        auto vtep_ptr = evpn_orch->getEVPNVtep();
                        if (vtep_ptr)
                        {
                            vtep_ptr->updateRemoteEndPointIpRef(m_nhg_vtep[i].ip, false);
                            vtep_ptr->cleanupDynamicDIPTunnel(m_nhg_vtep[i].ip);
                        }
                        m_nhg_vtep[i].ref_count -= 1;
                    }
                }

                /* Remove L2NexthopGroup from PortsOrch cache */
                gPortsOrch->removeL2NexthopGroup(nhgPort);

                /* This is a newly created NHG and now empty, remove it */
                if (m_nhg_nh[nhg_id].next_hops.empty())
                {
                    removeSaiNextHopGroup(m_nhg_nh[nhg_id].oid);
                    m_nhg_nh.erase(nhg_id);
                }
                else if (cleanup_failed)
                {
                    SWSS_LOG_ERROR("Partial cleanup failure for NHG %s - some SAI objects may have leaked", nhg_id.c_str());
                }
                return false;
            }
            SWSS_LOG_NOTICE("Created bridge port for next hop group %s with %zu next hops",
                            nhg_id.c_str(), m_nhg_nh[nhg_id].next_hops.size());
            m_nhg_nh[nhg_id].is_active = true;
        }
        else
        {
            /* Bridge port already exists, mark as active */
            m_nhg_nh[nhg_id].is_active = true;
        }
    }
    else
    {
        /* No next hops were successfully added */
        if (is_new_nhg)
        {
            SWSS_LOG_INFO("Cleaning up empty next hop group 0x%" PRIx64 " that was just created", m_nhg_nh[nhg_id].oid);
            removeSaiNextHopGroup(m_nhg_nh[nhg_id].oid);
            m_nhg_nh.erase(nhg_id);
        }
        return false;
    }

    /* Return false if not all next hops were added. Trigger a retry */
    if (!all_nh_added)
    {
        SWSS_LOG_NOTICE("L2 next hop group %s is active with only %zu/%zu next hops, will retry for remaining next hops",
                        nhg_id.c_str(), m_nhg_nh[nhg_id].next_hops.size(), v_add_nh.size());
        return false;
    }

    return true;
}

string L2NhgOrch::getNextHopGroupPortName(const string& nhg_id)
{
    return (NEXTHOP_GROUP_PORT_PREFIX + nhg_id);
}

bool L2NhgOrch::deleteL2NextHop(string nh_id)
{
    EvpnNvoOrch* evpn_orch = gDirectory.get<EvpnNvoOrch*>();
    string source_vtep = m_nhg_vtep.count(nh_id) ? m_nhg_vtep[nh_id].source_vtep : "";
    auto vtep_ptr = evpn_orch->getEVPNVtep();
    if (!vtep_ptr)
    {
        SWSS_LOG_ERROR("Unable to find EVPN VTEP %s", source_vtep.c_str());
        return false;
    }


    auto has_next_hop = [&nh_id](const auto &it) -> bool { return it.second.next_hops.count(nh_id); };
    if (count_if(m_nhg_nh.begin(), m_nhg_nh.end(), has_next_hop) > 0)
    {
        for (auto it = find_if(m_nhg_nh.begin(), m_nhg_nh.end(), has_next_hop); it != m_nhg_nh.end();)
        {
            SWSS_LOG_INFO("L2 nexthop %s is referenced in nexthop group %s",
                          nh_id.c_str(), it->first.c_str());
            bool ret = removeSaiNextHop(it->second.next_hops[nh_id]);
            if (!ret)
            {
                SWSS_LOG_ERROR("Failed to remove SAI next hop %s for %s", nh_id.c_str(), it->first.c_str());
                return false;
            }
            it->second.next_hops.erase(nh_id);
            m_nhg_vtep[nh_id].ref_count -= 1;
            vtep_ptr->updateRemoteEndPointIpRef(m_nhg_vtep[nh_id].ip, false);
            vtep_ptr->cleanupDynamicDIPTunnel(m_nhg_vtep[nh_id].ip);
            it = find_if(it, m_nhg_nh.end(), has_next_hop);
        }
    }

    if (m_nhg_vtep[nh_id].ref_count != 0)
    {
        if (m_nhg_vtep[nh_id].ref_count < 0)
        {
            SWSS_LOG_ERROR("L2 nexthop %s pointing to the tunnel for %s has NEGATIVE ref count : %d - indicates ref counting bug",
                           nh_id.c_str(),
                           m_nhg_vtep[nh_id].ip.c_str(),
                           m_nhg_vtep[nh_id].ref_count);
        }
        else
        {
            SWSS_LOG_ERROR("L2 nexthop %s pointing to the tunnel for %s has non-zero ref count : %d",
                           nh_id.c_str(),
                           m_nhg_vtep[nh_id].ip.c_str(),
                           m_nhg_vtep[nh_id].ref_count);
        }
        return false;
    }

    m_nhg_vtep.erase(nh_id);
    return true;
}

bool L2NhgOrch::updateL2NhgVtepIp(string nh_id, string new_vtep_ip)
{
    VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
    EvpnNvoOrch* evpn_orch = gDirectory.get<EvpnNvoOrch*>();
    string source_vtep = m_nhg_vtep.count(nh_id) ? m_nhg_vtep[nh_id].source_vtep : "";
    auto vtep_ptr = evpn_orch->getEVPNVtep();
    Port tunnel;

    if (!vtep_ptr)
    {
        SWSS_LOG_ERROR("Unable to find EVPN VTEP %s", source_vtep.c_str());
        return false;
    }


    // Loop through the groups using the L2 NH, update the SAI next hop
    auto has_next_hop = [&nh_id](const auto &it) -> bool { return it.second.next_hops.count(nh_id); };
    if (count_if(m_nhg_nh.begin(), m_nhg_nh.end(), has_next_hop) > 0)
    {
        for (auto it = find_if(m_nhg_nh.begin(), m_nhg_nh.end(), has_next_hop); it != m_nhg_nh.end();)
        {
            SWSS_LOG_INFO("L2 nexthop %s is referenced in nexthop group %s",
                          nh_id.c_str(), it->first.c_str());

            // Delete the SAI next hop for old vtep and add the SAI next hop for new vtep
            if (vxlan_orch->getTunnelPort(m_nhg_vtep[nh_id].ip, tunnel, false) && tunnel.m_tunnel_id != 0)
            {
                bool ret = removeSaiNextHop(it->second.next_hops[nh_id]);
                if (!ret)
                {
                    SWSS_LOG_ERROR("Failed to remove SAI next hop %s for %s", nh_id.c_str(), it->first.c_str());
                    return false;
                }
            }
            else
            {
                SWSS_LOG_ERROR("updateL2NhgVtepIp: P2P Tunnel to %s does not exist", m_nhg_vtep[nh_id].ip.c_str());
                return false;
            }
            it->second.next_hops.erase(nh_id);
            m_nhg_vtep[nh_id].ref_count -= 1;
            vtep_ptr->updateRemoteEndPointIpRef(m_nhg_vtep[nh_id].ip, false);
            vtep_ptr->cleanupDynamicDIPTunnel(m_nhg_vtep[nh_id].ip);

            if (vxlan_orch->getTunnelPort(new_vtep_ip, tunnel, false) && tunnel.m_tunnel_id != 0)
            {
                /* Create and add next hop group member */
                auto nh_info = createSaiNextHop(it->second.oid, tunnel.m_tunnel_id, new_vtep_ip);
                if (nh_info.first != SAI_NULL_OBJECT_ID && nh_info.second != SAI_NULL_OBJECT_ID)
                {
                    // Storing next hop group member id and next hop id
                    it->second.next_hops[nh_id].nhgm_oid = nh_info.first;
                    it->second.next_hops[nh_id].nh_oid = nh_info.second;
                    vtep_ptr->updateRemoteEndPointIpRef(m_nhg_vtep[nh_id].ip, true);
                    m_nhg_vtep[nh_id].ref_count += 1;
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to create new next hop and a member to next hop group 0x%" PRIx64, it->second.oid);
                    return false;
                }
            }
            else
            {
                SWSS_LOG_ERROR("Cannot create next hop group member. P2P Tunnel to %s does not exist", new_vtep_ip.c_str());
                return false;
            }
            auto next_it = std::next(it);
            it = find_if(next_it, m_nhg_nh.end(), has_next_hop);
        }
    }

    m_nhg_vtep[nh_id].ip = new_vtep_ip;
    return true;
}

/*
 * If it is a nexthop group pointing to a vtep then we just record it and move on.
 * If it is a nexthop group pointing to one or more nexthops group ids then we create the required sai object.
 */
bool L2NhgOrch::updateL2Nhg(string& key, KeyOpFieldsValuesTuple& t, Consumer& consumer)
{
    SWSS_LOG_ENTER();

    string nh_value;
    string nh_type;
    bool is_remote_vtep = false;
    string source_vtep = "";
    size_t sep_loc = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
    string nhg_id = key.substr(0, sep_loc);
    VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
    Port tunnel;

    for (auto i : kfvFieldsValues(t))
    {
        nh_type = fvField(i);
        if (nh_type  == "nexthop_group")
        {
            nh_value = fvValue(i);
            break;
        }
        else if (nh_type == "remote_vtep")
        {
            nh_value = fvValue(i);
            is_remote_vtep = true;
            try {
                IpAddress valid_ip = IpAddress(nh_value);
                // Creating an IpAddress object to validate if remote_ip is valid
                // if invalid it will throw the exception and we will ignore the event
                (void)valid_ip; // To avoid g++ warning

                // Check if the tunnel exists
                if (!vxlan_orch->getTunnelPort(nh_value, tunnel, false) || tunnel.m_tunnel_id == 0)
                {
                    SWSS_LOG_INFO("updateL2Nhg: P2P Tunnel to %s does not exist", nh_value.c_str());
                    return false;
                }
            } catch (exception &e) {
                SWSS_LOG_ERROR("Invalid IP address in L2 Nexthop Group %s", nh_value.c_str());
                nh_value = "";
                break;
            }
        }
        else if (nh_type == "source_vtep")
        {
            source_vtep = fvValue(i);
            SWSS_LOG_NOTICE("source_vtep %s", source_vtep.c_str());
        }
        else
        {
            SWSS_LOG_ERROR("Unknown field %s", nh_type.c_str());
        }
    }

    if (!nh_value.empty())
    {
        if (is_remote_vtep)
        {
            SWSS_LOG_NOTICE("nexthop vtep ip %s, source vtep %s", nh_value.c_str(), source_vtep.c_str());
            if (m_nhg_vtep.count(nhg_id)) {
                // Existing, do not reset the reference count.
                SWSS_LOG_INFO("updateL2Nhg: L2 nexthop %s pointing to the tunnel for %s has been existing with ref count : %d",
                              nhg_id.c_str(),
                              m_nhg_vtep[nhg_id].ip.c_str(),
                              m_nhg_vtep[nhg_id].ref_count);
                if (m_nhg_vtep[nhg_id].ip != nh_value) {
                    // This is the case of changing vtep ip.
                    SWSS_LOG_INFO("updateL2Nhg: adding L2 nexthop %s pointing to the tunnel with different IP %s",
                                  nhg_id.c_str(),
                                  m_nhg_vtep[nhg_id].ip.c_str());
                    if (!updateL2NhgVtepIp(nhg_id, nh_value))
                    {
                        return false;
                    }
                }
                // Update source_vtep for existing entry
                m_nhg_vtep[nhg_id].source_vtep = source_vtep;
            } else {
                m_nhg_vtep[nhg_id].ip = nh_value;
                m_nhg_vtep[nhg_id].ref_count = 0;
                m_nhg_vtep[nhg_id].source_vtep = source_vtep;
            }
        }
        else
        {
            //Adding L2 Next hop members to the next hop group
            if (!addL2NextHopGroupEntry(nhg_id, nh_value, source_vtep))
            {
                return false;
            }
        }
    }

    return true;
}

/*
 * 1. If the nexthop group id belongs to  m_nhg_vtep then
 *    it is a nexthop pointing to vtep. Delete the nexthop and clean up the nexthop groups
 * 2. If the nexthop group id belongs to m_nhg_nh then
 *    the entire nexthop group pointing to nexthops is removed and cleaned up
 */
bool L2NhgOrch::deleteL2Nhg(string& key, Consumer& consumer)
{
    SWSS_LOG_ENTER();

    size_t sep_loc = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
    string nh_id = key.substr(0, sep_loc);

    if (m_nhg_vtep.count(nh_id))
    {
        /* If there is a existing NHG id pointing to a tunnel
         * delete the nexthop and clean references
         */
        if (!deleteL2NextHop(nh_id))
        {
            return false;
        }
    }
    else if (m_nhg_nh.count(nh_id))
    {
        /* if there is a NHG id pointing to a list of NHG
         * delete that nexthop group
         */
        if (!deleteL2NextHopGroup(nh_id))
        {
            return false;
        }

    }
    else
    {
        SWSS_LOG_ERROR("Can't delete L2NHG '%s': does not exist", nh_id.c_str());
    }
    return true;
}

void L2NhgOrch::doL2NhgTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    bool status = true;
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            status = updateL2Nhg(key, t, consumer);
        }
        else if (op == DEL_COMMAND)
        {
            status = deleteL2Nhg(key, consumer);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation %s", op.c_str());
        }

        if (!status)
        {
            it++;
            continue;
        }
        it = consumer.m_toSync.erase(it);
    }
}

void L2NhgOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    string table_name = consumer.getTableName();
    if (table_name == APP_L2_NEXTHOP_GROUP_TABLE_NAME)
    {
        doL2NhgTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Received an invalid table %s in L2NhgOrch", table_name.c_str());
    }
}

/*
 * Getters.
 */
unsigned long L2NhgOrch::getL2NhgCount()
{
    unsigned long count = 0;
    for (const auto &it : m_nhg_nh)
    {
        if (it.second.is_active)
        {
            count++;
        }
    }
    return count;
}

unsigned long L2NhgOrch::getL2NhVtepRefCount(const std::string &nhg_id)
{
    return m_nhg_vtep[nhg_id].ref_count;
}

bool L2NhgOrch::hasActiveL2Nhg(const std::string &nhg_id)
{
    return (m_nhg_nh.find(nhg_id) != m_nhg_nh.end() && m_nhg_nh[nhg_id].is_active);
}

bool L2NhgOrch::isL2NextHop(const std::string &nhg_id)
{
    return (m_nhg_vtep.find(nhg_id) != m_nhg_vtep.end());
}

unsigned long L2NhgOrch::getNumL2NhgNextHops(const std::string &nhg_id)
{
    if (m_nhg_nh.find(nhg_id) != m_nhg_nh.end())
    {
        return m_nhg_nh[nhg_id].next_hops.size();
    }
    return 0;
}
