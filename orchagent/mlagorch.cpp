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

#include "orchagent/portsorch.h"
#include "orchagent/mlagorch.h"

using namespace std;
using namespace swss;

extern PortsOrch *gPortsOrch;
extern MlagOrch *gMlagOrch;

MlagOrch::MlagOrch(DBConnector *db, vector<string> &tableNames):
    Orch(db, tableNames)
{
    SWSS_LOG_ENTER();
}

MlagOrch::~MlagOrch()
{
    SWSS_LOG_ENTER();
}

void MlagOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();
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
