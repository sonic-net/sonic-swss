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

#include "shlorch.h"
#include "intfsorch.h"
#include "directory.h"
#include "vxlanorch.h"
#include "tokenize.h"

using namespace std;
using namespace swss;

extern PortsOrch*           gPortsOrch;
extern Directory<Orch*>     gDirectory;
extern sai_object_id_t      gSwitchId;
extern sai_isolation_group_api_t*  sai_isolation_group_api;
extern sai_vlan_api_t *sai_vlan_api;

ShlOrch::ShlOrch(DBConnector *db, const vector<string> &tableNames) : Orch(db, tableNames)
{
    SWSS_LOG_ENTER();
}

ShlOrch::~ShlOrch()
{
    SWSS_LOG_ENTER();
}

void ShlOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }
    string table_name = consumer.getTableName();
    if (table_name == APP_EVPN_SH_TABLE_NAME)
    {
        doShlTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("SHL receives invalid table %s", table_name.c_str());
    }
}

void ShlOrch::doShlTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    string attr_value;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string op = kfvOp(t);
        string key = kfvKey(t);

        size_t sep_loc = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
        string name = key.substr(0, sep_loc);

        if (op == SET_COMMAND)
        {
            for (auto i : kfvFieldsValues(t))
            {
                string attr_name = fvField(i);
                if (attr_name == SHL_VTEPS)
                {
                    attr_value = fvValue(i);
                    break;
                }
            }
            if (!attr_value.empty())
            {
                if (addSplitHorizonList(name, attr_value))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else
                it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            if (delSplitHorizonList(name))
                it = consumer.m_toSync.erase(it);
            else
                it++;
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

shared_ptr<IsolationGroup>
ShlOrch::getIsolationGroup(string name_iso_grp)
{
    SWSS_LOG_ENTER();
    shared_ptr<IsolationGroup> ret = nullptr;

    auto grp = m_ShlGrps.find(name_iso_grp);
    if (grp != m_ShlGrps.end())
    {
        ret = grp->second;
    }

    return ret;
}

bool ShlOrch::addIsolationGroupMember(string owner_port, string member_port)
{
    SWSS_LOG_ENTER();

    string name_iso_grp = ISO_GRP_PREFIX + owner_port;
    bool status = true;
    do {
        auto grp = getIsolationGroup(name_iso_grp);
        if (!grp)
        {
            auto grp = make_shared<IsolationGroup>(name_iso_grp,
                                                   ISOLATION_GROUP_TYPE_BRIDGE_PORT,
                                                   "EVPN ES isolation group");

            isolation_group_status_t result = grp->create();
            if (ISO_GRP_STATUS_SUCCESS != result)
            {
                status = false;
                break;
            }
            grp->setMembers(member_port);
            result = grp->setBindPorts(owner_port);
            if (ISO_GRP_STATUS_SUCCESS != result)
            {
                if (ISO_GRP_STATUS_INVALID_PARAM == result)
                {
                    SWSS_LOG_ERROR("Invalid param: %s", owner_port.c_str());
                }
                status = false;
                break;
            }
            m_ShlGrps[name_iso_grp] = grp;
            m_ShlGrp_members[member_port].push_back(owner_port);
        }
        else if (grp->getType() == ISOLATION_GROUP_TYPE_BRIDGE_PORT)
        {
            Port port;
            if (!gPortsOrch->getPort(member_port, port))
            {
                SWSS_LOG_ERROR("Port %s not found", member_port.c_str());
            }
            else {
                grp->addMember(port);
                m_ShlGrp_members[member_port].push_back(owner_port);
            }
        }
        else
        {
            SWSS_LOG_ERROR("Isolation group type update to %d not permitted",
                           ISOLATION_GROUP_TYPE_BRIDGE_PORT);
            status = false;
        }
    } while(0);

    return status;
}

bool ShlOrch::delIsolationGroupMember(string owner_port, string member_port)
{
    SWSS_LOG_ENTER();

    string name_iso_grp = ISO_GRP_PREFIX + owner_port;
    auto grp = getIsolationGroup(name_iso_grp);
    if (grp)
    {
        vector<string> members = grp->getMembers();
        Port port;
        if (!gPortsOrch->getPort(member_port, port))
        {
            SWSS_LOG_ERROR("Port %s not found", port.m_alias.c_str());
        }
        else {
            grp->delMember(port);
        }
        auto it = std::find(members.begin(), members.end(), member_port);

        if (it != members.end()) {
            members.erase(it);
        }

        if (!members.size())
        {
            grp->destroy();
            m_ShlGrps.erase(name_iso_grp);
        }

        return true;
    }
    SWSS_LOG_ERROR("Failed to delete isolation group %s member %s",
                   name_iso_grp.c_str(), member_port.c_str());

    return false;
}

bool ShlOrch::addSplitHorizonList(string lag_port, string vteps)
{
    SWSS_LOG_ENTER();
    bool status = true;
    Port port;
    if (!gPortsOrch->getPort(lag_port, port))
    {
        SWSS_LOG_ERROR("Failed to locate port %s", lag_port.c_str());
        return false;
    }

    try {
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();

        vector<string> hosts = tokenize(vteps, ',');
        for( size_t i = 0; i < hosts.size(); i++)
        {
            string vtep_port = vxlan_orch->getTunnelPortName(hosts.at(i), false);
            hosts.at(i) = vtep_port;

            Port port_vtep;
            if (gPortsOrch->getPort(vtep_port, port_vtep))
            {
                if (!addIsolationGroupMember(vtep_port, lag_port))
                {
                    SWSS_LOG_ERROR("Failed to add member %s to isolation group %s",
                                   lag_port.c_str(), vtep_port.c_str());
                }
            }
            else {
                SWSS_LOG_ERROR("Unknown VTEP %s", vtep_port.c_str());
            }
        }

        vector<string> non_exist_members;
        for (auto vtep_port : m_ShlGrp_members[lag_port])
        {
            auto it = find(hosts.begin(), hosts.end(), vtep_port);
            if(it == hosts.end())
            {
                non_exist_members.push_back(vtep_port);
            }
        }

        for (auto vtep_port : non_exist_members)
        {
            if (!delIsolationGroupMember(vtep_port, lag_port)) {
                SWSS_LOG_ERROR("Failed to del member %s from isolation group %s",
                               lag_port.c_str(), vtep_port.c_str());
            }
            auto it = find(m_ShlGrp_members[lag_port].begin(), m_ShlGrp_members[lag_port].end(), vtep_port);
            if(it != m_ShlGrp_members[lag_port].end())
            {
                m_ShlGrp_members[lag_port].erase(it);
            }
        }

    }
    catch (exception &e)
    {
        SWSS_LOG_ERROR("Exception: %s", e.what());
        status = false;
    }

    return status;
}

bool ShlOrch::delSplitHorizonList(string lag_port)
{
    SWSS_LOG_ENTER();
    bool status = true;

    for (auto vtep_port : m_ShlGrp_members[lag_port])
    {
        delIsolationGroupMember(vtep_port, lag_port);
    }
    m_ShlGrp_members[lag_port].clear();

    return status;
}

