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

#include "isolationgrouporch.h"
#include "converter.h"
#include "tokenize.h"
#include "portsorch.h"

extern sai_object_id_t gSwitchId;
extern PortsOrch *gPortsOrch;
extern sai_isolation_group_api_t*  sai_isolation_group_api;
extern sai_bridge_api_t *sai_bridge_api;
extern sai_port_api_t *sai_port_api;
extern IsoGrpOrch *gIsoGrpOrch;

IsoGrpOrch::IsoGrpOrch(vector<TableConnector> &connectors) : Orch(connectors)
{
    SWSS_LOG_ENTER();
    this->installDebugClis();
    gPortsOrch->attach(this);
}

IsoGrpOrch::~IsoGrpOrch()
{
    SWSS_LOG_ENTER();
}

shared_ptr<IsolationGroup>
IsoGrpOrch::getIsolationGroup(string name)
{
    SWSS_LOG_ENTER();

    shared_ptr<IsolationGroup> ret = nullptr;

    auto grp = m_isolationGrps.find(name);
    if (grp != m_isolationGrps.end())
    {
        ret = grp->second;
    }

    return ret;
}

void
IsoGrpOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    string table_name = consumer.getTableName();
    if (table_name == APP_ISOLATION_GROUP_TABLE_NAME)
    {
        doIsoGrpTblTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Invalid table %s", table_name.c_str());
    }
}

void
IsoGrpOrch::doIsoGrpTblTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    isolation_group_status_t status = ISO_GRP_STATUS_SUCCESS;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string op = kfvOp(t);
        string key = kfvKey(t);

        size_t sep_loc = key.find(consumer.getConsumerTable()->getTableNameSeparator().c_str());
        string name = key.substr(0, sep_loc);

        SWSS_LOG_DEBUG("Op:%s IsoGrp:%s", op.c_str(), name.c_str());

        if (op == SET_COMMAND)
        {
            isolation_group_type_t type = ISOLATION_GROUP_TYPE_INVALID;
            string descr("");
            string bind_ports("");
            string mem_ports("");

            for (auto itp : kfvFieldsValues(t))
            {
                string attr_name = to_upper(fvField(itp));
                string attr_value = fvValue(itp);

                if (attr_name == ISOLATION_GRP_DESCRIPTION)
                {
                    descr = attr_value;
                }
                else if (attr_name == ISOLATION_GRP_TYPE)
                {
                    if (ISOLATION_GRP_TYPE_PORT == attr_value)
                    {
                        type = ISOLATION_GROUP_TYPE_PORT;
                    }
                    else if (ISOLATION_GRP_TYPE_BRIDGE_PORT == attr_value)
                    {
                        type = ISOLATION_GROUP_TYPE_BRIDGE_PORT;
                    }
                }
                else if (attr_name == ISOLATION_GRP_PORTS)
                {
                    bind_ports = attr_value;
                }
                else if (attr_name == ISOLATION_GRP_MEMBERS)
                {
                    mem_ports = attr_value;
                }
            }

            status = addIsolationGroup(name, type, descr, bind_ports, mem_ports);
            if (ISO_GRP_STATUS_SUCCESS == status)
            {
                auto grp = getIsolationGroup(name);
                if (!grp->isObserver(this))
                {
                    IsolationGroupUpdate update = {grp.get(), true};
                    grp->notifyObservers(SUBJECT_TYPE_ISOLATION_GROUP_CHANGE, &update);

                    grp->attach(this);
                }
            }
        }
        else
        {
            auto grp = getIsolationGroup(name);
            if (grp)
            {
                grp->detach(this);

                /* Send a notification and see if observers want to detach */
                IsolationGroupUpdate update = {grp.get(), false};
                grp->notifyObservers(SUBJECT_TYPE_ISOLATION_GROUP_CHANGE, &update);
            }

            // Finally delete it if it
            status = delIsolationGroup(name);
        }

        if (status != ISO_GRP_STATUS_RETRY)
        {
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            it++;
        }
    }
}

isolation_group_status_t
IsoGrpOrch::addIsolationGroup(string name, isolation_group_type_t type, string descr, string bindPorts, string memPorts)
{
    SWSS_LOG_ENTER();

    isolation_group_status_t status = ISO_GRP_STATUS_SUCCESS;

    // Add Or Update
    auto grp = getIsolationGroup(name);
    if (!grp)
    {
        // Add Case
        auto grp = make_shared<IsolationGroup>(name, type, descr);

        status = grp->create();
        if (ISO_GRP_STATUS_SUCCESS != status)
        {
            return status;
        }
        grp->setMembers(memPorts);
        grp->setBindPorts(bindPorts);
        this->m_isolationGrps[name] = grp;
    }
    else if ((grp) && (grp->getType() == type))
    {
        grp->m_description = descr;
        grp->setMembers(memPorts);
        grp->setBindPorts(bindPorts);
    }
    else
    {
        SWSS_LOG_ERROR("Isolation group type update to %d not permitted", type);
        status = ISO_GRP_STATUS_FAIL;
    }

    return status;
}

isolation_group_status_t
IsoGrpOrch::delIsolationGroup(string name)
{
    SWSS_LOG_ENTER();

    auto grp = m_isolationGrps.find(name);
    if (grp != m_isolationGrps.end())
    {
        if (!grp->second->hasObservers())
        {
            grp->second->destroy();
            m_isolationGrps.erase(name);
        }
        else
        {
            SWSS_LOG_NOTICE("%s group has observers. Not deleting", name.c_str());
        }
    }

    return ISO_GRP_STATUS_SUCCESS;
}


void
IsoGrpOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    if (type != SUBJECT_TYPE_BRIDGE_PORT_CHANGE)
    {
        return;
    }

    for (auto kv : m_isolationGrps)
    {
        kv.second->update(type, cntx);
    }
}


isolation_group_status_t
IsolationGroup::create()
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;

    attr.id = SAI_ISOLATION_GROUP_ATTR_TYPE;
    if (ISOLATION_GROUP_TYPE_BRIDGE_PORT == m_type)
    {
        attr.value.s32 = SAI_ISOLATION_GROUP_TYPE_BRIDGE_PORT;
    }
    else
    {
        attr.value.s32 = SAI_ISOLATION_GROUP_TYPE_PORT;
    }

    sai_status_t status = sai_isolation_group_api->create_isolation_group(&m_oid, gSwitchId, 1, &attr);
    if (SAI_STATUS_SUCCESS != status)
    {
        SWSS_LOG_ERROR("Error %d creating isolation group %s", status, m_name.c_str());
        return ISO_GRP_STATUS_FAIL;
    }
    else
    {
        SWSS_LOG_NOTICE("Isolation group %s has oid %lx", m_name.c_str(), m_oid);
    }

    return ISO_GRP_STATUS_SUCCESS;
}

isolation_group_status_t
IsolationGroup::destroy()
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;

    // Remove all bindings
    attr.value.oid = SAI_NULL_OBJECT_ID;
    for (auto p : m_bind_ports)
    {
        Port port;
        gPortsOrch->getPort(p, port);
        if (ISOLATION_GROUP_TYPE_BRIDGE_PORT == m_type)
        {
            attr.id = SAI_BRIDGE_PORT_ATTR_ISOLATION_GROUP;
            if (SAI_STATUS_SUCCESS != sai_bridge_api->set_bridge_port_attribute(port.m_bridge_port_id, &attr))
            {
                SWSS_LOG_ERROR("Unable to del SAI_BRIDGE_PORT_ATTR_ISOLATION_GROUP from %s", p.c_str());
            }
            else
            {
                SWSS_LOG_NOTICE("SAI_BRIDGE_PORT_ATTR_ISOLATION_GROUP removed from %s", p.c_str());
            }
        }
        else if (ISOLATION_GROUP_TYPE_PORT == m_type)
        {
            attr.id = SAI_PORT_ATTR_ISOLATION_GROUP;
            if (SAI_STATUS_SUCCESS != sai_port_api->set_port_attribute(
                                                        (port.m_type == Port::PHY ? port.m_port_id : port.m_lag_id),
                                                        &attr))
            {
                SWSS_LOG_ERROR("Unable to del SAI_PORT_ATTR_ISOLATION_GROUP from %s", p.c_str());
            }
            else
            {
                SWSS_LOG_NOTICE("SAI_PORT_ATTR_ISOLATION_GROUP removed from %s", p.c_str());
            }
        }
    }
    m_bind_ports.clear();
    m_pending_bind_ports.clear();

    // Remove all members
    for (auto &kv : m_members)
    {
        if (SAI_STATUS_SUCCESS != sai_isolation_group_api->remove_isolation_group_member(kv.second))
        {
            SWSS_LOG_ERROR("Unable to delete isolation group member %lx from %s:%lx for port %s",
                           kv.second,
                           m_name.c_str(),
                           m_oid,
                           kv.first.c_str());
        }
        else
        {
            SWSS_LOG_NOTICE("Isolation group member %lx deleted from %s:%lx for port %s",
                            kv.second,
                            m_name.c_str(),
                            m_oid,
                            kv.first.c_str());
        }
    }
    m_members.clear();

    sai_status_t status = sai_isolation_group_api->remove_isolation_group(m_oid);
    if (SAI_STATUS_SUCCESS != status)
    {
        SWSS_LOG_ERROR("Unable to delete isolation group %s with oid %lx", m_name.c_str(), m_oid);
    }
    else
    {
        SWSS_LOG_NOTICE("Isolation group %s with oid %lx deleted", m_name.c_str(), m_oid);
    }
    m_oid = SAI_NULL_OBJECT_ID;

    return ISO_GRP_STATUS_SUCCESS;
}

isolation_group_status_t
IsolationGroup::addMember(Port &port)
{
    SWSS_LOG_ENTER();
    sai_object_id_t port_id = SAI_NULL_OBJECT_ID;

    if (m_type == ISOLATION_GROUP_TYPE_BRIDGE_PORT)
    {
        port_id = port.m_bridge_port_id;
    }
    else if (m_type == ISOLATION_GROUP_TYPE_PORT)
    {
        port_id = (port.m_type == Port::PHY ? port.m_port_id : port.m_lag_id);
    }

    if (SAI_NULL_OBJECT_ID == port_id)
    {
        SWSS_LOG_NOTICE("Port %s not ready for for isolation group %s of type %d",
                        port.m_alias.c_str(),
                        m_name.c_str(),
                        m_type);

        m_pending_members.push_back(port.m_alias);

        return ISO_GRP_STATUS_SUCCESS;
    }

    if (m_members.find(port.m_alias) != m_members.end())
    {
        SWSS_LOG_DEBUG("Port %s:%lx already a member of %s", port.m_alias.c_str(), port_id, m_name.c_str());
    }
    else
    {
        sai_object_id_t mem_id = SAI_NULL_OBJECT_ID;
        sai_attribute_t mem_attr[2];
        sai_status_t status = SAI_STATUS_SUCCESS;

        mem_attr[0].id = SAI_ISOLATION_GROUP_MEMBER_ATTR_ISOLATION_GROUP_ID;
        mem_attr[0].value.oid = m_oid;
        mem_attr[1].id = SAI_ISOLATION_GROUP_MEMBER_ATTR_ISOLATION_OBJECT;
        mem_attr[1].value.oid = port_id;

        status = sai_isolation_group_api->create_isolation_group_member(&mem_id, gSwitchId, 2, mem_attr);
        if (SAI_STATUS_SUCCESS != status)
        {
            SWSS_LOG_ERROR("Unable to add %s:%lx as member of %s:%lx", port.m_alias.c_str(), port_id,
                           m_name.c_str(), m_oid);
            return ISO_GRP_STATUS_FAIL;
        }
        else
        {
            m_members[port.m_alias] = mem_id;
            SWSS_LOG_NOTICE("Port %s:%lx added as member of %s:%lx with oid %lx",
                            port.m_alias.c_str(),
                            port_id,
                            m_name.c_str(),
                            m_oid,
                            mem_id);
        }
    }

    return ISO_GRP_STATUS_SUCCESS;
}

isolation_group_status_t
IsolationGroup::delMember(Port &port, bool do_fwd_ref)
{
    SWSS_LOG_ENTER();

    if (m_members.find(port.m_alias) == m_members.end())
    {
        auto node = find(m_pending_members.begin(), m_pending_members.end(), port.m_alias);
        if (node != m_pending_members.end())
        {
            m_pending_members.erase(node);
        }

        return ISO_GRP_STATUS_SUCCESS;
    }

    sai_object_id_t mem_id = m_members[port.m_alias];
    sai_status_t status = SAI_STATUS_SUCCESS;

    status = sai_isolation_group_api->remove_isolation_group_member(mem_id);
    if (SAI_STATUS_SUCCESS != status)
    {
        SWSS_LOG_ERROR("Unable to delete isolation group member %lx for port %s and iso group %s %lx",
                       mem_id,
                       port.m_alias.c_str(),
                       m_name.c_str(),
                       m_oid);

        return ISO_GRP_STATUS_FAIL;
    }
    else
    {
        SWSS_LOG_NOTICE("Deleted isolation group member %lx for port %s and iso group %s %lx",
                       mem_id,
                       port.m_alias.c_str(),
                       m_name.c_str(),
                       m_oid);

        m_members.erase(port.m_alias);
    }

    if (do_fwd_ref)
    {
        m_pending_members.push_back(port.m_alias);
    }

    return ISO_GRP_STATUS_SUCCESS;
}

isolation_group_status_t
IsolationGroup::setMembers(string ports)
{
    SWSS_LOG_ENTER();
    auto port_list = tokenize(ports, ',');
    set<string> portList(port_list.begin(), port_list.end());
    vector<string> old_members = m_pending_members;

    for (auto mem : m_members)
    {
        old_members.emplace_back(mem.first);
    }

    for (auto alias : portList)
    {
        if ((0 == alias.find("Ethernet")) || (0 == alias.find("PortChannel")))
        {
            auto iter = find(old_members.begin(), old_members.end(), alias);
            if (iter != old_members.end())
            {
                SWSS_LOG_NOTICE("Port %s already part of %s. No change", alias.c_str(), m_name.c_str());
                old_members.erase(iter);
            }
            else
            {
                Port port;
                if (!gPortsOrch->getPort(alias, port))
                {
                    SWSS_LOG_NOTICE("Port %s not found. Added it to m_pending_members", alias.c_str());
                    m_pending_members.emplace_back(alias);
                    continue;
                }
                addMember(port);
            }
        }
        else
        {
            SWSS_LOG_ERROR("Port %s not supported", alias.c_str());
            continue;
        }
    }

    // Remove all the ports which are no longer needed
    for (auto alias : old_members)
    {
        Port port;
        if (!gPortsOrch->getPort(alias, port))
        {
            SWSS_LOG_ERROR("Port %s not found", alias.c_str());
            m_pending_members.erase(find(m_pending_members.begin(), m_pending_members.end(), port.m_alias));
        }
        else
        {
            delMember(port);
        }
    }

    return ISO_GRP_STATUS_SUCCESS;
}

isolation_group_status_t
IsolationGroup::bind(Port &port)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    sai_status_t status = SAI_STATUS_SUCCESS;

    if (find(m_bind_ports.begin(), m_bind_ports.end(), port.m_alias) != m_bind_ports.end())
    {
        SWSS_LOG_NOTICE("isolation group %s of type %d already bound to Port %s",
                        m_name.c_str(),
                        m_type,
                        port.m_alias.c_str());

        return ISO_GRP_STATUS_SUCCESS;
    }

    attr.value.oid = m_oid;
    if (m_type == ISOLATION_GROUP_TYPE_BRIDGE_PORT)
    {
        if (port.m_bridge_port_id != SAI_NULL_OBJECT_ID)
        {
            attr.id = SAI_BRIDGE_PORT_ATTR_ISOLATION_GROUP;
            status = sai_bridge_api->set_bridge_port_attribute(port.m_bridge_port_id, &attr);
            if (SAI_STATUS_SUCCESS != status)
            {
                SWSS_LOG_ERROR("Unable to set attribute %d value %lx to %s",
                               attr.id,
                               attr.value.oid,
                               port.m_alias.c_str());
            }
            else
            {
                m_bind_ports.push_back(port.m_alias);
            }
        }
        else
        {
            m_pending_bind_ports.push_back(port.m_alias);
            SWSS_LOG_NOTICE("Port %s saved in pending bind ports for isolation group %s of type %d",
                            port.m_alias.c_str(),
                            m_name.c_str(),
                            m_type);
        }
    }
    else if (m_type == ISOLATION_GROUP_TYPE_PORT)
    {
        if ((port.m_type == Port::PHY ? port.m_port_id : port.m_lag_id) != SAI_NULL_OBJECT_ID)
        {
            attr.id = SAI_PORT_ATTR_ISOLATION_GROUP;
            status = sai_port_api->set_port_attribute((port.m_type == Port::PHY ? port.m_port_id : port.m_lag_id), &attr);
            if (SAI_STATUS_SUCCESS != status)
            {
                SWSS_LOG_ERROR("Unable to set attribute %d value %lx to %s",
                               attr.id,
                               attr.value.oid,
                               port.m_alias.c_str());
            }
            else
            {
                m_bind_ports.push_back(port.m_alias);
            }
        }
        else
        {
            m_pending_bind_ports.push_back(port.m_alias);
            SWSS_LOG_NOTICE("Port %s saved in pending bind ports for isolation group %s of type %d",
                            port.m_alias.c_str(),
                            m_name.c_str(),
                            m_type);
        }
    }
    else
    {
        return ISO_GRP_STATUS_INVALID_PARAM;
    }

    return ISO_GRP_STATUS_SUCCESS;
}

isolation_group_status_t
IsolationGroup::unbind(Port &port, bool do_fwd_ref)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    sai_status_t status = SAI_STATUS_SUCCESS;

    if (find(m_bind_ports.begin(), m_bind_ports.end(), port.m_alias) == m_bind_ports.end())
    {
        auto node = find(m_pending_bind_ports.begin(), m_pending_bind_ports.end(), port.m_alias);
        if (node != m_pending_bind_ports.end())
        {
            m_pending_bind_ports.erase(node);
        }

        return ISO_GRP_STATUS_SUCCESS;
    }

    attr.value.oid = SAI_NULL_OBJECT_ID;
    if (m_type == ISOLATION_GROUP_TYPE_BRIDGE_PORT)
    {
        attr.id = SAI_BRIDGE_PORT_ATTR_ISOLATION_GROUP;
        status = sai_bridge_api->set_bridge_port_attribute(port.m_bridge_port_id, &attr);
    }
    else if (m_type == ISOLATION_GROUP_TYPE_PORT)
    {
        attr.id = SAI_PORT_ATTR_ISOLATION_GROUP;
        status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
    }
    else
    {
        return ISO_GRP_STATUS_INVALID_PARAM;
    }

    if (SAI_STATUS_SUCCESS != status)
    {
        SWSS_LOG_ERROR("Unable to set attribute %d value %lx to %s", attr.id, attr.value.oid, port.m_alias.c_str());
    }
    else
    {
        m_bind_ports.erase(find(m_bind_ports.begin(), m_bind_ports.end(), port.m_alias));
    }

    if (do_fwd_ref)
    {
        m_pending_bind_ports.push_back(port.m_alias);
    }

    return ISO_GRP_STATUS_SUCCESS;
}

isolation_group_status_t
IsolationGroup::setBindPorts(string ports)
{
    SWSS_LOG_ENTER();
    vector<string> old_bindports = m_pending_bind_ports;
    auto port_list = tokenize(ports, ',');
    set<string> portList(port_list.begin(), port_list.end());

    old_bindports.insert(old_bindports.end(), m_bind_ports.begin(), m_bind_ports.end());
    for (auto alias : portList)
    {
        if ((0 == alias.find("Ethernet")) || (0 == alias.find("PortChannel")))
        {
            auto iter = find(old_bindports.begin(), old_bindports.end(), alias);
            if (iter != old_bindports.end())
            {
                SWSS_LOG_NOTICE("%s is already bound to %s", m_name.c_str(), alias.c_str());
                old_bindports.erase(iter);
            }
            else
            {
                Port port;
                if (!gPortsOrch->getPort(alias, port))
                {
                    SWSS_LOG_NOTICE("Port %s not found. Added it to m_pending_bind_ports", alias.c_str());
                    m_pending_bind_ports.emplace_back(alias);
                    return ISO_GRP_STATUS_INVALID_PARAM;
                }
                bind(port);
            }
        }
        else
        {
            return ISO_GRP_STATUS_INVALID_PARAM;
        }
    }

    // Remove all the ports which are no longer needed
    for (auto alias : old_bindports)
    {
        Port port;
        if (!gPortsOrch->getPort(alias, port))
        {
            SWSS_LOG_ERROR("Port %s not found", alias.c_str());
            m_pending_bind_ports.erase(find(m_pending_bind_ports.begin(), m_pending_bind_ports.end(), port.m_alias));
        }
        else
        {
            unbind(port);
        }
    }

    return ISO_GRP_STATUS_SUCCESS;
}

void
IsolationGroup::update(SubjectType, void *cntx)
{
    PortUpdate *update = static_cast<PortUpdate *>(cntx);
    Port &port = update->port;

    if (update->add)
    {
        auto mem_node = find(m_pending_members.begin(), m_pending_members.end(), port.m_alias);
        if (mem_node != m_pending_members.end())
        {
            m_pending_members.erase(mem_node);
            addMember(port);
        }

        auto bind_node = find(m_pending_bind_ports.begin(), m_pending_bind_ports.end(), port.m_alias);
        if (bind_node != m_pending_bind_ports.end())
        {
            m_pending_bind_ports.erase(bind_node);
            bind(port);
        }
    }
    else
    {
        auto bind_node = find(m_bind_ports.begin(), m_bind_ports.end(), port.m_alias);
        if (bind_node != m_bind_ports.end())
        {
            unbind(port, true);
        }

        auto mem_node = m_members.find(port.m_alias);
        if (mem_node != m_members.end())
        {
            delMember(port, true);
        }
    }
}
