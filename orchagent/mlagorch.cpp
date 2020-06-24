/*
 * Copyright 2019 Broadcom.  The term "Broadcom" refers to Broadcom Inc.
 * and/or its subsidiaries.
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

#include "portsorch.h"
#include "mlagorch.h"

//Name of ISL isolation group must match with the name used by Mclagsyncd
#define MLAG_ISL_ISOLATION_GROUP_NAME  "MCLAG_ISO_GRP"
#define MLAG_ISL_ISOLATION_GROUP_DESCR "Isolation group for MCLAG"

using namespace std;
using namespace swss;

extern PortsOrch *gPortsOrch;
extern IsoGrpOrch *gIsoGrpOrch;
extern MlagOrch *gMlagOrch;

MlagOrch::MlagOrch(DBConnector *db, vector<string> &tableNames):
    Orch(db, tableNames)
{
    SWSS_LOG_ENTER();
    this->installDebugClis();
}

MlagOrch::~MlagOrch()
{
    SWSS_LOG_ENTER();
}

void MlagOrch::update(SubjectType type, void *cntx)
{
    IsolationGroupUpdate *update;

    if (type == SUBJECT_TYPE_ISOLATION_GROUP_CHANGE)
    {
        update = static_cast<IsolationGroupUpdate *>(cntx);

        if (update->add)
        {
            m_iccp_control_isolation_grp = true;
            SWSS_LOG_NOTICE("MLAG yields control of isolation group");
        }
        else
        {
            m_iccp_control_isolation_grp = false;
            addAllMlagInterfacesToIsolationGroup();
            SWSS_LOG_NOTICE("MLAG takes control of isolation group");
        }
    }
}

DEBUGSH_CLI(MlagOrchShowDebug,
            "show system internal orchagent mlag global",
            SHOW_COMMAND,
            SYSTEM_DEBUG_COMMAND,
            INTERNAL_COMMAND,
            "Orchagent related commands",
            "Mlag orch related commands",
            "Mlag global info")
{
    gMlagOrch->showDebugInfo(this);
}

void MlagOrch::installDebugClis()
{
    DebugShCmd::install(new MlagOrchShowDebug());
}

//------------------------------------------------------------------
//Private API section
//------------------------------------------------------------------
void MlagOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }
    string table_name = consumer.getTableName();
    if (table_name == CFG_MCLAG_TABLE_NAME)
    {
        doMlagDomainTask(consumer);
    }
    else if (table_name == CFG_MCLAG_INTF_TABLE_NAME)
    {
        doMlagInterfaceTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("MLAG receives invalid table %s", table_name.c_str());
    }
}

//Only interest in peer-link info from MLAG domain table
void MlagOrch::doMlagDomainTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string peer_link;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "peer_link")
                {
                    peer_link = fvValue(i);
                    break;
                }
            }
            if (!peer_link.empty())
            {
                if (addIslInterface(peer_link))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else
                it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            if (delIslInterface())
                it = consumer.m_toSync.erase(it);
            else
                it++;
        }
        else
        {
            SWSS_LOG_ERROR("MLAG receives unknown operation type %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

//MLAG interface table key format: MCLAG_INTF_TABLE|mclag<id>|ifname
void MlagOrch::doMlagInterfaceTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    size_t delimiter_pos;
    string mlag_if_name;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string op = kfvOp(t);
        string key = kfvKey(t);

        delimiter_pos = key.find_first_of("|");
        mlag_if_name = key.substr(delimiter_pos+1);

        if (op == SET_COMMAND)
        {
            if (addMlagInterface(mlag_if_name))
                it = consumer.m_toSync.erase(it);
            else
                it++;
        }
        else if (op == DEL_COMMAND)
        {
            if (delMlagInterface(mlag_if_name))
                it = consumer.m_toSync.erase(it);
            else
                it++;
        }
        else
        {
            SWSS_LOG_ERROR("MLAG receives unknown operation type %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

bool MlagOrch::addIslInterface(string isl_name)
{
    Port isl_port;
    MlagIslUpdate update;

    //No change
    if ((m_isl_name == isl_name) || (isl_name.empty()))
        return true;

    m_isl_name = isl_name;

    //Create isolation group if ICCPd has not created it yet
    addIslIsolationGroup();

    //Update observers
    update.isl_name = isl_name;
    update.is_add = true;
    notify(SUBJECT_TYPE_MLAG_ISL_CHANGE, static_cast<void *>(&update));
    return true;
}

bool MlagOrch::delIslInterface()
{
    MlagIslUpdate update;

    if (m_isl_name.empty())
        return true;

    update.isl_name = m_isl_name;
    update.is_add = false;

    m_isl_name.clear();
    //Delete isolation group if ICCP has not taken control yet
    deleteIslIsolationGroup();

    //Notify observer
    notify(SUBJECT_TYPE_MLAG_ISL_CHANGE, static_cast<void *>(&update));

    return true;
}

//Mlag interface can be added even before interface is configured
bool MlagOrch::addMlagInterface(string if_name)
{
    MlagIfUpdate update;

    //Duplicate add
    if (m_mlagIntfs.find(if_name) != m_mlagIntfs.end())
    {
        SWSS_LOG_ERROR("MLAG adds duplicate MLAG interface %s", if_name.c_str());
    }
    else
    {

        m_mlagIntfs.insert(if_name);

        //If this is the first MLAG interface added, create isolation group
        if (m_mlagIntfs.size() == 1)
            addIslIsolationGroup();
        else
            updateIslIsolationGroup(if_name, true);

        //Notify observer
        update.if_name = if_name;
        update.is_add = true;
        notify(SUBJECT_TYPE_MLAG_INTF_CHANGE, static_cast<void *>(&update));
    }
    return true;

}
bool MlagOrch::delMlagInterface(string if_name)
{
    MlagIfUpdate update;

    //Delete an unknown MLAG interface
    if (m_mlagIntfs.find(if_name) == m_mlagIntfs.end())
    {
        SWSS_LOG_ERROR("MLAG deletes unknown MLAG interface %s", if_name.c_str());
    }
    else
    {
        m_mlagIntfs.erase(if_name);

        //If this is the last MLAG interface added, delete isolation group
        if (m_mlagIntfs.size() == 0)
            deleteIslIsolationGroup();
        else
            updateIslIsolationGroup(if_name, false);

        //Notify observers
        update.if_name = if_name;
        update.is_add = false;
        notify(SUBJECT_TYPE_MLAG_INTF_CHANGE, static_cast<void *>(&update));
    }
    return true;
}

bool MlagOrch::isMlagInterface(string if_name)
{
    if (m_mlagIntfs.find(if_name) == m_mlagIntfs.end())
        return false;
    else
        return true;
}

bool MlagOrch::isIslInterface(string if_name)
{
    if (m_isl_name == if_name)
        return true;
    else
        return false;
}

/* Create isolation group based on MLAG configuration before ICCPd
 * takes control of the isolation group
 */
bool MlagOrch::addIslIsolationGroup()
{
    isolation_group_status_t status;
    string isolate_dst_ports;

    //Need peer link and at least one MLAG interface to create an isolation group
    if (m_isl_name.empty() || (m_mlagIntfs.size() == 0))
    {
        SWSS_LOG_NOTICE("MLAG skips adding isolation group: isl name(%s), num MLAG interface %lu",
            m_isl_name.empty() ? "invalid" : m_isl_name.c_str(), m_mlagIntfs.size());
        return false;
    }
    //During SwSS warm reboot, isolation group can be created first by ICCP
    //due to IsoGrpOrch processing info from APP_DB first
    auto isolation_grp = gIsoGrpOrch->getIsolationGroup(MLAG_ISL_ISOLATION_GROUP_NAME);
    if (isolation_grp)
    {
        m_iccp_control_isolation_grp = true;

        //Register with IsoGrpOrch to receive update when ICCP deletes the group
        if (!isolation_grp->isObserver(this))
        {
            isolation_grp->attach(SUBJECT_TYPE_ISOLATION_GROUP_CHANGE, this);
            ++m_num_isolation_grp_attach;
            m_attach_isolation_grp = true;
            SWSS_LOG_NOTICE("MLAG found ICCP-controlled isolation group. Attach to it");
        }
        else
            SWSS_LOG_NOTICE("MLAG is already attached to ICCP-controlled isolation group");
        return false;
    }
    //Create a new isolation group
    for (auto mlag_if = m_mlagIntfs.begin(); mlag_if != m_mlagIntfs.end(); ++mlag_if)
    {
        if (isolate_dst_ports.length())
        {
            isolate_dst_ports = isolate_dst_ports + ',' + *mlag_if;
        }
        else
        {
            isolate_dst_ports = *mlag_if;
        }
    }
    status = gIsoGrpOrch->addIsolationGroup(
        MLAG_ISL_ISOLATION_GROUP_NAME,
        ISOLATION_GROUP_TYPE_BRIDGE_PORT,
        MLAG_ISL_ISOLATION_GROUP_DESCR,
        m_isl_name, isolate_dst_ports);

    if (status != ISO_GRP_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("MLAG failed to create ISL isolation group, status %d", status);
        ++m_num_isolation_grp_add_error;
        return false;
    }
    //Register wiht IsoGrpOrch to receive update when ICCP adds the group
    isolation_grp = gIsoGrpOrch->getIsolationGroup(MLAG_ISL_ISOLATION_GROUP_NAME);
    if (isolation_grp)
    {
        isolation_grp->attach(SUBJECT_TYPE_ISOLATION_GROUP_CHANGE, this);
        ++m_num_isolation_grp_attach;
        m_attach_isolation_grp = true;
    }
    SWSS_LOG_NOTICE("MLAG creates ISL isolation group");
    return true;
}

bool MlagOrch::deleteIslIsolationGroup()
{
    isolation_group_status_t status;

    //Delete isolation group if either peer link or all MLAG interfaces are removed
    if (!m_isl_name.empty() && (m_mlagIntfs.size() > 0))
    {
        SWSS_LOG_NOTICE("MLAG skips deleting isolation group: isl name %s, num MLAG interface %lu",
            m_isl_name.c_str(), m_mlagIntfs.size());
        return false;
    }
    //The delete request is triggered when the last MLAG interface is deleted
    //or when peer link interface is deleted. If MlagOrch no longer attaches
    //to isolation group due to previous deletion, do not need to do anything
    if (!m_attach_isolation_grp)
        return false;

    auto isolation_grp = gIsoGrpOrch->getIsolationGroup(MLAG_ISL_ISOLATION_GROUP_NAME);

    if (!isolation_grp)
    {
        SWSS_LOG_ERROR("MLAG fails to find isolation group, iccp_control %d",
            m_iccp_control_isolation_grp);
        return false;
    }
    //Reset the ICCP control flag since MlagOrch will detach from the group
    //and won't receive any further update
    m_iccp_control_isolation_grp = false;

    //Only need to unregister with IsoGrpOrch if ICCP is in control
    if (m_iccp_control_isolation_grp)
    {
        //Mlag configuration is deleted while it is up
        if (m_attach_isolation_grp)
        {
            isolation_grp->detach(SUBJECT_TYPE_ISOLATION_GROUP_CHANGE, this);
            ++m_num_isolation_grp_detach;
            m_attach_isolation_grp = false;
            SWSS_LOG_NOTICE("MLAG detaches from ICCP-controlled isolation group");
        }
        else
            SWSS_LOG_ERROR("MLAG is not attached to ICCP-controlled isolation group");
    }
    else
    {
        //Unregister with IsoGrpOrch before deleting since the group is deleted
        //only after all observers are removed
        if (m_attach_isolation_grp)
        {
            isolation_grp->detach(SUBJECT_TYPE_ISOLATION_GROUP_CHANGE, this);
            ++m_num_isolation_grp_detach;
            m_attach_isolation_grp = false;
        }
        status = gIsoGrpOrch->delIsolationGroup(MLAG_ISL_ISOLATION_GROUP_NAME);
        if (status == ISO_GRP_STATUS_SUCCESS)
        {
            SWSS_LOG_NOTICE("MLAG deletes ISL isolation group");
            return true;
        }
        else
        {
            SWSS_LOG_ERROR("MLAG can't delete ISL isolation group, status %d", status);
            ++m_num_isolation_grp_del_error;
            return false;
        }
    }
    return true;
}

bool MlagOrch::updateIslIsolationGroup(string if_name, bool is_add)
{
    string isolate_dst_ports;
    isolation_group_status_t status;

    //Do not trigger any update if ICCP already tooks control
    if (m_iccp_control_isolation_grp)
        return false;

    auto isolation_grp = gIsoGrpOrch->getIsolationGroup(MLAG_ISL_ISOLATION_GROUP_NAME);
    if (isolation_grp)
    {
        //Isolation group can handle forward reference. Updated it with
        //the latest set of MLAG interfaces
        for (auto mlag_if = m_mlagIntfs.begin(); mlag_if != m_mlagIntfs.end(); ++mlag_if)
        {
            if (isolate_dst_ports.length())
                isolate_dst_ports = isolate_dst_ports + ',' + *mlag_if;
            else
                isolate_dst_ports = *mlag_if;
        }
        status = isolation_grp->setMembers(isolate_dst_ports);
        if (status != ISO_GRP_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("MLAG can't %s member %s for isolation group, status %d",
                is_add ? "add" : "delete", if_name.c_str(), status);
            ++m_num_isolation_grp_update_error;
            return false;
        }
        else
        {
            SWSS_LOG_NOTICE("MLAG %s member %s for isolation group",
                is_add ? "adds" : "deletes", if_name.c_str());
            return true;
        }
    }
    else
    {
        SWSS_LOG_ERROR("MLAG can't find MlagOrch-controlled isolation group to update");
    }
    return true;
}

bool MlagOrch::addAllMlagInterfacesToIsolationGroup()
{
    isolation_group_status_t status;
    string isolate_dst_ports;

    //Need peer link and at least one MLAG interface to create an isolation group
    if (m_isl_name.empty() || (m_mlagIntfs.size() == 0))
    {
        SWSS_LOG_NOTICE("MLAG skips adding mlag-if to isolation group: isl name(%s), mlag-if count %lu",
            m_isl_name.empty() ? "invalid" : m_isl_name.c_str(), m_mlagIntfs.size());
        return false;
    }
    auto isolation_grp = gIsoGrpOrch->getIsolationGroup(MLAG_ISL_ISOLATION_GROUP_NAME);
    if (!isolation_grp)
    {
        SWSS_LOG_ERROR("MLAG can't find ISL isolation group");
        return false;
    }
    for (auto mlag_if = m_mlagIntfs.begin(); mlag_if != m_mlagIntfs.end(); ++mlag_if)
    {
        if (isolate_dst_ports.length())
        {
            isolate_dst_ports = isolate_dst_ports + ',' + *mlag_if;
        }
        else
        {
            isolate_dst_ports = *mlag_if;
        }
    }
    //Update isolation group
    status = gIsoGrpOrch->addIsolationGroup(
        MLAG_ISL_ISOLATION_GROUP_NAME,
        ISOLATION_GROUP_TYPE_BRIDGE_PORT,
        MLAG_ISL_ISOLATION_GROUP_DESCR,
        m_isl_name, isolate_dst_ports);

    if (status != ISO_GRP_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("MLAG fails to update ISL isolation group, status %d", status);
        ++m_num_isolation_grp_update_error;
        return false;
    }
    else
    {
        SWSS_LOG_NOTICE("MLAG adds all MLAG interfaces to ISL isolation group");
        return true;
    }
}

void MlagOrch::showDebugInfo(DebugShCmd *cmd)
{
    Port mlag_port, isl_port;

    //Show Mlag interface info
    DEBUGSH_OUT(cmd, "Isolation group name: %s(%s)\n",
        MLAG_ISL_ISOLATION_GROUP_NAME,
        gIsoGrpOrch->getIsolationGroup(MLAG_ISL_ISOLATION_GROUP_NAME) ?
        "created" : "not created");
    DEBUGSH_OUT(cmd, "ICCP controls isolation group: %s\n",
        m_iccp_control_isolation_grp ? "yes" : "no");
    DEBUGSH_OUT(cmd, "MlagOrch attaches to isolation group: %s\n",
        m_attach_isolation_grp ? "yes" : "no");
    DEBUGSH_OUT(cmd, "Isolation group attach count: %u\n", m_num_isolation_grp_attach);
    DEBUGSH_OUT(cmd, "Isolation group detach count: %u\n", m_num_isolation_grp_detach);
    DEBUGSH_OUT(cmd, "Isolation group add error count: %u\n", m_num_isolation_grp_add_error);
    DEBUGSH_OUT(cmd, "Isolation group delete error count: %u\n", m_num_isolation_grp_del_error);
    DEBUGSH_OUT(cmd, "Isolation group update error count: %u\n\n", m_num_isolation_grp_update_error);

    DEBUGSH_OUT(cmd, "MLAG ISL interface: %s\n", m_isl_name.c_str());
    DEBUGSH_OUT(cmd, "MLAG interfaces: %lu\n", m_mlagIntfs.size());
    for (auto &name: m_mlagIntfs)
    {
        //MLAG interface is configured before the interface is configured
        if (!gPortsOrch->getPort(name, mlag_port))
        {
            DEBUGSH_OUT(cmd, "    %s\n", name.c_str());
        }
        else
        {
            DEBUGSH_OUT(cmd,
                "    %s: oper %s, traffic_disable %d, hw_pending_lag_mbrs %lu\n",
                mlag_port.m_alias.c_str(), 
                oper_status_strings.at(mlag_port.m_oper_status).c_str(),
                mlag_port.m_lag_traffic_disable,
                mlag_port.m_hw_pending_lag_members.size());
        }
    }
}
