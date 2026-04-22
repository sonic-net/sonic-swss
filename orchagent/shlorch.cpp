#include "shlorch.h"
#include "converter.h"
#include "tokenize.h"
#include "portsorch.h"
#include "vxlanorch.h"
#include "directory.h"

extern sai_object_id_t gSwitchId;
extern PortsOrch *gPortsOrch;
extern sai_isolation_group_api_t*  sai_isolation_group_api;
extern sai_bridge_api_t *sai_bridge_api;
extern ShlOrch *gShlOrch;
extern Directory<Orch*> gDirectory;

ShlOrch::ShlOrch(vector<TableConnector> &connectors) : Orch(connectors)
{
    SWSS_LOG_ENTER();
    gPortsOrch->attach(this);
}

ShlOrch::~ShlOrch()
{
    SWSS_LOG_ENTER();
}

shared_ptr<ShlIsolationGroup>
ShlOrch::getIsolationGroup(string vtep_ip_addr)
{
    SWSS_LOG_ENTER();

    shared_ptr<ShlIsolationGroup> ret = nullptr;

    auto grp = m_isolationGrps.find(vtep_ip_addr);
    if (grp != m_isolationGrps.end())
    {
        ret = grp->second;
    }

    return ret;
}

long unsigned int ShlOrch::getIsolationGroupCount()
{
    SWSS_LOG_ENTER();

    return m_isolationGrps.size();
}

long unsigned int ShlOrch::getVtepsListCount()
{
    SWSS_LOG_ENTER();

    return m_vtep_list.size();
}

void
ShlOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        SWSS_LOG_ERROR("ports are not ready, exiting doTask");
        return;
    }

    string table_name = consumer.getTableName();
    if (table_name == APP_EVPN_SPLIT_HORIZON_TABLE_NAME)
    {
        doShlTblTask(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Invalid table %s", table_name.c_str());
    }
}

void
ShlOrch::doShlTblTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
    shl_isolation_group_status_t status = SHL_ISO_GRP_STATUS_SUCCESS;

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        /* format: <VLAN_name>:<Ifname> */
        vector<string> keys = tokenize(kfvKey(t), ':', 1);
        string op = kfvOp(t);

        Port port;

        if (!gPortsOrch->getPort(keys[1], port))
        {
            SWSS_LOG_ERROR("Port %s is not not yet created, delaying", keys[1].c_str());
            it++;
            continue;
        }

        string ifname(keys[1]);

        if (op == SET_COMMAND)
        {
            string vteps_list("");

            for (auto itp : kfvFieldsValues(t))
            {
                string attr_name = fvField(itp);
                string attr_value = fvValue(itp);

                if (attr_name == SHL_VTEPS_LIST)
                {
                    vteps_list = attr_value;

                    auto old_list = m_vtep_list.find(ifname);
                    auto new_list = tokenize(vteps_list, ',');
                    vector<string> del_list, add_list;

                    if (old_list != m_vtep_list.end()) {
                        del_list = m_vtep_list[ifname];

                        for (auto mem: new_list) {
                            auto iter = find(del_list.begin(), del_list.end(), mem);
                            if (iter != del_list.end()) {
                                del_list.erase(iter);
                            } else {
                                add_list.push_back(mem);
                            }
                        }
                    } else {
                        add_list = new_list;
                    }
                    m_vtep_list[ifname] = new_list;
                    addMemberToIsolationGroupPerVtep(add_list, port);
                    delMemberFromIsolationGroupPerVtep(del_list, port);
                }
                else
                    SWSS_LOG_ERROR("unknown Attr:%s", attr_name.c_str());
            }
        }
        else
        {
            delMemberFromIsolationGroupPerVtep(m_vtep_list[ifname], port);
            if (m_vtep_list.find(ifname) != m_vtep_list.end()) {
                m_vtep_list.erase(ifname);
            } else {
                SWSS_LOG_ERROR("entry for ifname:%s does not exist", ifname.c_str());
            }
        }

        if (status != SHL_ISO_GRP_STATUS_RETRY)
        {
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            it++;
        }
    }
}

/*
 * Walk through the Vtep list, do the following steps for each vtep
 * step 1. check if isolation group exists, otherwise create isolation group
 * stpe 2. add Port as member for this isolation group
 * step 3. bind the tunnel port of vtep to this isolation group if its just created
 */
shl_isolation_group_status_t
ShlOrch::addMemberToIsolationGroupPerVtep(vector<string> &addVtepList, Port &port) {
    shl_isolation_group_status_t status = SHL_ISO_GRP_STATUS_SUCCESS;

    for (auto vtep: addVtepList) {
        auto grp = m_isolationGrps.find(vtep);
        if (grp == m_isolationGrps.end()) {
            auto grp = make_shared<ShlIsolationGroup>(vtep);
            status = grp->create();
            if (SHL_ISO_GRP_STATUS_SUCCESS != status)
            {
                return status;
            }
            status = grp->addMember(port);
            if (SHL_ISO_GRP_STATUS_SUCCESS != status)
            {
                return status;
            }
            grp->bind(vtep);
            this->m_isolationGrps[vtep] = grp;
            grp->attach(this);
        } else {
            auto grp = this->m_isolationGrps[vtep];
            grp->addMember(port);
        }
    }
    return SHL_ISO_GRP_STATUS_SUCCESS;
}

/*
 * Walk through the Vtep list, do the following steps for each vtep
 * step 1. get the isolation group created for this vtep
 * stpe 2. delete Port as member for this isolation group
 * step 3. if no members exists, unbind the tunnel port and delete the isolation group
 */
shl_isolation_group_status_t
ShlOrch::delMemberFromIsolationGroupPerVtep(vector<string> &delVtepList, Port &port) {
    shl_isolation_group_status_t status = SHL_ISO_GRP_STATUS_SUCCESS;

    for (auto vtep: delVtepList) {
        auto grp = m_isolationGrps.find(vtep);
        if (grp != m_isolationGrps.end()) {
            auto grp = this->m_isolationGrps[vtep];
            status = grp->delMember(port);
            if (SHL_ISO_GRP_STATUS_SUCCESS != status)
            {
                return status;
            }
            if (grp->getNumOfMembers() == 0) {
                grp->unbind(vtep);
                grp->detach(this);
                grp->destroy();
                this->m_isolationGrps.erase(vtep);
            }
        }
    }
    return SHL_ISO_GRP_STATUS_SUCCESS;
}

long unsigned int
ShlIsolationGroup::getNumOfMembers() {
    return m_members.size();
}

long unsigned int
ShlIsolationGroup::getNumOfPendingMembers() {
    return m_pending_members.size();
}

long unsigned int
ShlIsolationGroup::getNumOfBindPorts() {
    return m_bind_ports.size();
}

long unsigned int
ShlIsolationGroup::getNumOfPendingBindports() {
    return m_pending_bind_ports.size();
}

shl_isolation_group_status_t
ShlIsolationGroup::addMember(Port &port)
{
    SWSS_LOG_ENTER();
    sai_object_id_t port_id = SAI_NULL_OBJECT_ID;

    port_id = port.m_bridge_port_id;
    if (SAI_NULL_OBJECT_ID == port_id)
    {
        SWSS_LOG_NOTICE("Port %s not ready for for isolation group %s",
                        port.m_alias.c_str(),
                        m_name.c_str());

        m_pending_members.push_back(port.m_alias);

        return SHL_ISO_GRP_STATUS_SUCCESS;
    }

    if (m_members.find(port.m_alias) != m_members.end())
    {
        SWSS_LOG_DEBUG("Port %s: 0x%" PRIx64 "already a member of isolation group", port.m_alias.c_str(), port_id);
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
            SWSS_LOG_ERROR("Unable to add %s:  0x%" PRIx64 " as member of %s:0x%" PRIx64, port.m_alias.c_str(), port_id,
                           m_name.c_str(), m_oid);
            return SHL_ISO_GRP_STATUS_FAIL;
        }
        else
        {
            m_members[port.m_alias] = mem_id;
            SWSS_LOG_NOTICE("Port %s: 0x%" PRIx64 " added as member of %s: 0x%" PRIx64 " with oid 0x%" PRIx64,
                            port.m_alias.c_str(),
                            port_id,
                            m_name.c_str(),
                            m_oid,
                            mem_id);
        }
    }

    return SHL_ISO_GRP_STATUS_SUCCESS;
}

shl_isolation_group_status_t
ShlIsolationGroup::delMember(Port &port, bool do_fwd_ref)
{
    SWSS_LOG_ENTER();

    if (m_members.find(port.m_alias) == m_members.end())
    {
        auto node = find(m_pending_members.begin(), m_pending_members.end(), port.m_alias);
        if (node != m_pending_members.end())
        {
            m_pending_members.erase(node);
        }

        return SHL_ISO_GRP_STATUS_SUCCESS;
    }

    sai_object_id_t mem_id = m_members[port.m_alias];
    sai_status_t status = SAI_STATUS_SUCCESS;

    status = sai_isolation_group_api->remove_isolation_group_member(mem_id);
    if (SAI_STATUS_SUCCESS != status)
    {
        SWSS_LOG_ERROR("Unable to delete isolation group member 0x%" PRIx64 " for port %s and iso group %s 0x%" PRIx64,
                       mem_id,
                       port.m_alias.c_str(),
                       m_name.c_str(),
                       m_oid);

        return SHL_ISO_GRP_STATUS_FAIL;
    }
    else
    {
        SWSS_LOG_NOTICE("Deleted isolation group member 0x%" PRIx64 "for port %s and iso group %s 0x%" PRIx64,
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

    return SHL_ISO_GRP_STATUS_SUCCESS;
}

shl_isolation_group_status_t
ShlIsolationGroup::create()
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;

    attr.id = SAI_ISOLATION_GROUP_ATTR_TYPE;
    attr.value.s32 = SAI_ISOLATION_GROUP_TYPE_BRIDGE_PORT;

    sai_status_t status = sai_isolation_group_api->create_isolation_group(&m_oid, gSwitchId, 1, &attr);
    if (SAI_STATUS_SUCCESS != status)
    {
        SWSS_LOG_ERROR("Error %d creating isolation group %s", status, m_name.c_str());
        return SHL_ISO_GRP_STATUS_FAIL;
    }
    else
    {
        SWSS_LOG_NOTICE("Isolation group %s has oid 0x%" PRIx64, m_name.c_str(), m_oid);
    }

    return SHL_ISO_GRP_STATUS_SUCCESS;
}

shl_isolation_group_status_t
ShlIsolationGroup::destroy()
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;

    // Remove all bindings
    attr.value.oid = SAI_NULL_OBJECT_ID;
    for (auto p : m_bind_ports)
    {
        Port port;
        gPortsOrch->getPort(p, port);

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
    m_bind_ports.clear();
    m_pending_bind_ports.clear();

    // Remove all members
    for (auto &kv : m_members)
    {
        if (SAI_STATUS_SUCCESS != sai_isolation_group_api->remove_isolation_group_member(kv.second))
        {
            SWSS_LOG_ERROR("Unable to delete isolation group member 0x%" PRIx64 " from %s: 0x%" PRIx64 " for port %s",
                           kv.second,
                           m_name.c_str(),
                           m_oid,
                           kv.first.c_str());
        }
        else
        {
            SWSS_LOG_NOTICE("Isolation group member 0x%" PRIx64 " deleted from %s: 0x%" PRIx64 " for port %s",
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
        SWSS_LOG_ERROR("Unable to delete issolation group %s with oid 0x%" PRIx64, m_name.c_str(), m_oid);
    }
    else
    {
        SWSS_LOG_NOTICE("Isolation group %s with oid 0x%" PRIx64 " deleted", m_name.c_str(), m_oid);
    }
    m_oid = SAI_NULL_OBJECT_ID;

    return SHL_ISO_GRP_STATUS_SUCCESS;
}

void
ShlOrch::update(SubjectType type, void *cntx)
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

shl_isolation_group_status_t
ShlIsolationGroup::bind(string vtep)
{
    SWSS_LOG_ENTER();

    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
    auto tunnel_port_name = tunnel_orch->getTunnelPortName(vtep);
    Port port;
    if (!gPortsOrch->getPort(tunnel_port_name, port)) {
        m_pending_bind_ports.push_back(tunnel_port_name);
        SWSS_LOG_NOTICE("Port %s saved in pending bind ports for isolation group %s",
                        tunnel_port_name.c_str(),
                        m_name.c_str());
        return SHL_ISO_GRP_STATUS_SUCCESS;
    }
    return bind(port);
}
shl_isolation_group_status_t
ShlIsolationGroup::bind(Port &port)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    sai_status_t status = SAI_STATUS_SUCCESS;

    if (find(m_bind_ports.begin(), m_bind_ports.end(), port.m_alias) != m_bind_ports.end())
    {
        SWSS_LOG_NOTICE("isolation group %s already bound to Port %s",
                        m_name.c_str(),
                        port.m_alias.c_str());

        return SHL_ISO_GRP_STATUS_SUCCESS;
    }

    attr.value.oid = m_oid;
    if (port.m_bridge_port_id != SAI_NULL_OBJECT_ID)
    {
        attr.id = SAI_BRIDGE_PORT_ATTR_ISOLATION_GROUP;
        status = sai_bridge_api->set_bridge_port_attribute(port.m_bridge_port_id, &attr);
        if (SAI_STATUS_SUCCESS != status)
        {
            SWSS_LOG_ERROR("Unable to set attribute %d value  0x%" PRIx64 "to %s",
                            attr.id,
                            attr.value.oid,
                            port.m_alias.c_str());
        }
        else
        {
            m_bind_ports.push_back(port.m_alias);
            SWSS_LOG_NOTICE("Bind function, bind port = %s, saved in m_bind_ports for iso grp: %s", port.m_alias.c_str(), m_name.c_str());
        }
    }
    else
    {
        m_pending_bind_ports.push_back(port.m_alias);
        SWSS_LOG_NOTICE("Port %s saved in pending bind ports for isolation group %s\n",
                        port.m_alias.c_str(),
                        m_name.c_str());
    }

    return SHL_ISO_GRP_STATUS_SUCCESS;
}

shl_isolation_group_status_t
ShlIsolationGroup::unbind(string vtep, bool do_fwd_ref)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    sai_status_t status = SAI_STATUS_SUCCESS;

    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
    auto tunnel_port_name = tunnel_orch->getTunnelPortName(vtep);
    Port port;
    if (!gPortsOrch->getPort(tunnel_port_name, port)) {
        SWSS_LOG_NOTICE("Tunnel Port %s doesnt exist for Isolation group %s",
                        tunnel_port_name.c_str(),
                        m_name.c_str());
        return SHL_ISO_GRP_STATUS_FAIL;
    }

    if (find(m_bind_ports.begin(), m_bind_ports.end(), port.m_alias) == m_bind_ports.end())
    {
        auto node = find(m_pending_bind_ports.begin(), m_pending_bind_ports.end(), port.m_alias);
        if (node != m_pending_bind_ports.end())
        {
            m_pending_bind_ports.erase(node);
            SWSS_LOG_DEBUG("Unbind function, pending port = %s, removed port in m_pending_bind_ports for iso grp: %s", port.m_alias.c_str(), m_name.c_str());
        }

        return SHL_ISO_GRP_STATUS_SUCCESS;
    }

    attr.value.oid = SAI_NULL_OBJECT_ID;

    attr.id = SAI_BRIDGE_PORT_ATTR_ISOLATION_GROUP;
    status = sai_bridge_api->set_bridge_port_attribute(port.m_bridge_port_id, &attr);

    if (SAI_STATUS_SUCCESS != status)
    {
        SWSS_LOG_ERROR("Unable to set attribute %d value 0x%" PRIx64 "to %s", attr.id, attr.value.oid, port.m_alias.c_str());
    }
    else
    {
        m_bind_ports.erase(find(m_bind_ports.begin(), m_bind_ports.end(), port.m_alias));
        SWSS_LOG_NOTICE("Unbind function, port = %s, removed port in m_bind_ports for iso grp: %s", port.m_alias.c_str(), m_name.c_str());
    }

    if (do_fwd_ref)
    {
        m_pending_bind_ports.push_back(port.m_alias);
    }

    return SHL_ISO_GRP_STATUS_SUCCESS;
}

void
ShlIsolationGroup::update(SubjectType, void *cntx)
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
            unbind(port.m_alias, true);
        }

        auto mem_node = m_members.find(port.m_alias);
        if (mem_node != m_members.end())
        {
            delMember(port, true);
        }
    }
}
