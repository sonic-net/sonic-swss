/*
 * Copyright 2024 GlobalLogic.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>
#include <vector>

#include "l2nhgorch.h"
#include "directory.h"
#include "vxlanorch.h"
#include "tokenize.h"

#define L2NHG_FIELD_REMOTE_VTEP   "remote_vtep"
#define L2NHG_FIELD_NEXTHOP_GROUP "nexthop_group"

#define L2_ECMP_GROUP_PORT_PREFIX "Port_L2_ECMP_Grp_"

using namespace std;
using namespace swss;

extern sai_l2_ecmp_group_api_t    *sai_l2_ecmp_group_api;

extern PortsOrch*       gPortsOrch;
extern Directory<Orch*> gDirectory;
extern sai_object_id_t gSwitchId;

uint64_t L2NhgOrch::m_max_group_count = 0;

static bool getVxlanPortFromIP(string ip, Port& port)
{
    VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
    string vtep_port = vxlan_orch->getTunnelPortName(ip, false);
    return gPortsOrch->getPort(vtep_port, port);
}

L2NhgOrch::L2NhgOrch(DBConnector *db, const vector<string> &tableNames) : Orch(db, tableNames)
{
    SWSS_LOG_ENTER();

    /*
     * Get the maximum number of L2 ECMP groups.
     */
    if (sai_object_type_get_availability(gSwitchId,
                                        SAI_OBJECT_TYPE_L2_ECMP_GROUP,
                                        0,
                                        nullptr,
                                        &m_max_group_count) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Switch does not support L2 ECMP Groups");
        m_max_group_count = 0;
    }
}

L2NhgOrch::~L2NhgOrch()
{
    SWSS_LOG_ENTER();
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
        doL2NhgTask(consumer);
    else
        SWSS_LOG_ERROR("L2NhgOrch receives invalid table %s", table_name.c_str());
}

void L2NhgOrch::doL2NhgTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        bool is_group = false;

        string op = kfvOp(t);
        string key = kfvKey(t);

        size_t sep_loc = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
        string nhid = key.substr(0, sep_loc);

        if (op == SET_COMMAND)
        {
            string fv_value;
            for (auto i : kfvFieldsValues(t))
            {
                string fv_name = fvField(i);
                if (fv_name == L2NHG_FIELD_NEXTHOP_GROUP)
                {
                    is_group = true;
                    fv_value = fvValue(i);
                    break;
                }
                else if (fv_name == L2NHG_FIELD_REMOTE_VTEP)
                {
                    fv_value = fvValue(i);
                    try {
                        IpAddress valid_ip = IpAddress(fv_value);
                        (void)valid_ip;
                    } catch (exception &e) {
                        SWSS_LOG_ERROR("Invalid IP address in L2 Nexthop %s", fv_value.c_str());
                        fv_value = "";
                    }
                    break;
                }
                else
                {
                    SWSS_LOG_ERROR("Unknown field %s", fv_name.c_str());
                }
            }
            if (!fv_value.empty())
            {
                if (!is_group)
                {
                    m_nh[nhid].ip = fv_value;
                    it = consumer.m_toSync.erase(it);
                }
                else
                {
                    if (addL2NexthopGroup(nhid, fv_value))
                        it = consumer.m_toSync.erase(it);
                    else
                        it++;
                }
            }
            else
            {
                it = consumer.m_toSync.erase(it);
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (m_nh.count(nhid))
            {
                if (delL2Nexthop(nhid))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else if (m_nhg.count(nhid))
            {
                if (delL2NexthopGroup(nhid))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else
            {
                SWSS_LOG_ERROR("Can't delete L2NHG '%s': does not exist", nhid.c_str());
                it = consumer.m_toSync.erase(it);
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

bool L2NhgOrch::addL2NexthopGroup(string nhg_id, string nh_ids)
{
    vector<string> v_new = tokenize(nh_ids, ',');

    vector<string> v_del;
    for (auto i : m_nhg[nhg_id].hops)
    {
        auto it = find(v_new.begin(), v_new.end(), i.first);
        if (it == v_new.end())
        {
            if (m_nh.count(i.first))
                v_del.push_back(i.first);
            else
                SWSS_LOG_ERROR("L2 Nexthop %s was not created", i.first.c_str());
        }
    }

    vector<string> v_add;
    if (m_nhg[nhg_id].hops.empty())
    {
        v_add = v_new;
        if (addL2EcmpGroup(m_nhg[nhg_id].oid) != SAI_STATUS_SUCCESS)
        {
            m_nhg.erase(nhg_id);
            return false;
        }
    }
    else
    {
        for (string i : v_new)
        {
            if (m_nhg[nhg_id].hops.count(i) == 0)
            {
                if (m_nh.count(i))
                    v_add.push_back(i);
                else
                    SWSS_LOG_ERROR("L2 Nexthop %s was not created", i.c_str());
            }
        }
    }

    for (string i : v_del)
    {
        Port tunnel;
        if (getVxlanPortFromIP(m_nh[i].ip, tunnel) && tunnel.m_tunnel_id != 0)
        {
            delL2EcmpGroupMember(m_nhg[nhg_id].hops[i]);
            m_nhg[nhg_id].hops.erase(i);
        }
        else
        {
            SWSS_LOG_ERROR("P2P Tunnel to %s is does not exist", m_nh[i].ip.c_str());
        }
    }
    for (string i : v_add)
    {
        Port tunnel;
        if (getVxlanPortFromIP(m_nh[i].ip, tunnel) && tunnel.m_tunnel_id != 0)
        {
            addL2EcmpGroupMember(m_nhg[nhg_id].oid, tunnel.m_tunnel_id, m_nhg[nhg_id].hops[i]);
        }
        else
        {
            SWSS_LOG_ERROR("P2P Tunnel to %s is does not exist", m_nh[i].ip.c_str());
        }
    }

    Port grp_port;
    string port_name = getL2EcmpGroupPortName(nhg_id);
    if (!gPortsOrch->getPort(port_name, grp_port)) {
        gPortsOrch->addL2EcmpGroup(port_name, m_nhg[nhg_id].oid);
        gPortsOrch->getPort(port_name, grp_port);
        gPortsOrch->addBridgePort(grp_port);
    }

    return true;
}

string L2NhgOrch::getL2EcmpGroupPortName(const std::string& nhg_id)
{
    std::string grpPortName;
    grpPortName = L2_ECMP_GROUP_PORT_PREFIX + nhg_id;
    return grpPortName;
}

bool L2NhgOrch::delL2NexthopGroup(string nhg_id)
{
    if (m_nhg.count(nhg_id))
    {
        for (auto it = m_nhg[nhg_id].hops.begin(); it != m_nhg[nhg_id].hops.end();)
        {
            delL2EcmpGroupMember(it->second);
            it = m_nhg[nhg_id].hops.erase(it);
        }
        Port grp_port;
        string port_name = getL2EcmpGroupPortName(nhg_id);
        if (gPortsOrch->getPort(port_name, grp_port)) {
            bool ret = gPortsOrch->removeBridgePort(grp_port);
            if (!ret)
            {
                SWSS_LOG_ERROR("Remove Bridge port failed for L2 ECMP Grp %s", nhg_id.c_str());
                return false;
            }
            gPortsOrch->removeL2EcmpGroup(grp_port);
        }
        delL2EcmpGroup(m_nhg[nhg_id].oid);
        m_nhg.erase(nhg_id);
    }
    return true;
}

bool L2NhgOrch::delL2Nexthop(string nhid)
{
    auto has_hop = [&nhid](const auto &it) -> bool { return it.second.hops.count(nhid); };
    if (m_nh.count(nhid))
    {
        if (count_if(m_nhg.begin(), m_nhg.end(), has_hop) > 0)
        {
            for (auto it = find_if(m_nhg.begin(), m_nhg.end(), has_hop); it != m_nhg.end(); )
            {
                delL2EcmpGroupMember(it->second.hops[nhid]);
                it->second.hops.erase(nhid);
                it = find_if(it, m_nhg.end(), has_hop);
                SWSS_LOG_NOTICE("L2 Nexthop %s is still referenced in grp %s",
                                nhid.c_str(), it->first.c_str());
            }
        }
        m_nh.erase(nhid);
    }
    return false;
}

sai_status_t L2NhgOrch::addL2EcmpGroup(sai_object_id_t &oid)
{
    if (m_nhg.size() >= m_max_group_count)
    {
        SWSS_LOG_WARN("Failed to create L2 ECMP Group: hardware limit of groups reached (%luu)",
                      m_max_group_count);
        return 0;
    }

    sai_status_t status = sai_l2_ecmp_group_api->create_l2_ecmp_group(&oid, gSwitchId, 0, NULL);
    if (status != SAI_STATUS_SUCCESS)
        SWSS_LOG_ERROR("Failed to create an empty L2 ECMP Group: rc: %d", status);

    return status;
}

void L2NhgOrch::delL2EcmpGroup(sai_object_id_t l2_ecmp_group_id)
{
    sai_status_t status = sai_l2_ecmp_group_api->remove_l2_ecmp_group(l2_ecmp_group_id);
    if (status != SAI_STATUS_SUCCESS)
        SWSS_LOG_ERROR("Failed to delete L2 ECMP Group 0x%" PRIx64 ": rc: %d", l2_ecmp_group_id, status);
}

sai_status_t L2NhgOrch::addL2EcmpGroupMember(sai_object_id_t l2_ecmp_group_id,
                                             sai_object_id_t tunnel_id, sai_object_id_t &oid)
{
    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    attr.id = SAI_L2_ECMP_GROUP_MEMBER_ATTR_L2_ECMP_GROUP_ID;
    attr.value.oid = l2_ecmp_group_id;
    attrs.push_back(attr);
    attr.id = SAI_L2_ECMP_GROUP_MEMBER_ATTR_TUNNEL_ID;
    attr.value.oid = tunnel_id;
    attrs.push_back(attr);
    sai_status_t status = sai_l2_ecmp_group_api->create_l2_ecmp_group_member(&oid, gSwitchId,
                                                                             (uint32_t)attrs.size(),
                                                                             attrs.data());

    if (status != SAI_STATUS_SUCCESS)
        SWSS_LOG_ERROR("Failed to create L2 ECMP Group member for 0x%" PRIx64 ": rc: %d", tunnel_id, status);

    return status;
}

void L2NhgOrch::delL2EcmpGroupMember(sai_object_id_t member_id)
{
    sai_status_t status = sai_l2_ecmp_group_api->remove_l2_ecmp_group_member(member_id);
    if (status != SAI_STATUS_SUCCESS)
        SWSS_LOG_ERROR("Failed to delete L2 ECMP Group member 0x%" PRIx64 ": rc: %d", member_id, status);
}
