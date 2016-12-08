#include <linux/if_ether.h>

#include <utility>

#include "logger.h"
#include "swssnet.h"
#include "mirrororch.h"

#define MIRROR_SESSION_STATUS           "status"
#define MIRROR_SESSION_STATUS_ACTIVE    "active"
#define MIRROR_SESSION_STATUS_INACTIVE  "inactive"
#define MIRROR_SESSION_SRC_IP           "src_ip"
#define MIRROR_SESSION_DST_IP           "dst_ip"
#define MIRROR_SESSION_GRE_TYPE         "gre_type"
#define MIRROR_SESSION_DSCP             "dscp"
#define MIRROR_SESSION_TTL              "ttl"
#define MIRROR_SESSION_QUEUE            "queue"

#define MIRROR_SESSION_DEFAULT_VLAN_PRI 0
#define MIRROR_SESSION_DEFAULT_VLAN_CFI 0
#define MIRROR_SESSION_DEFAULT_IP_HDR_VER 4
#define MIRROR_SESSION_DEFAULT_TOS      0

extern sai_mirror_api_t *sai_mirror_api;

using namespace std::rel_ops;

MirrorOrch::MirrorOrch(DBConnector *db, string tableName,
        PortsOrch *portOrch, RouteOrch *routeOrch, NeighOrch *neighOrch, FdbOrch *fdbOrch) :
        Orch(db, tableName),
        m_portsOrch(portOrch),
        m_routeOrch(routeOrch),
        m_neighOrch(neighOrch),
        m_fdbOrch(fdbOrch),
        m_mirrorTableProducer(db, tableName)
{
    m_portsOrch->attach(this);
    m_neighOrch->attach(this);
    m_fdbOrch->attach(this);
}

void MirrorOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    assert(cntx);

    switch(type) {
    case SUBJECT_TYPE_NEXTHOP_CHANGE:
    {
        NextHopUpdate *update = static_cast<NextHopUpdate *>(cntx);
        updateNextHop(*update);
        break;
    }
    case SUBJECT_TYPE_NEIGH_CHANGE:
    {
        NeighborUpdate *update = static_cast<NeighborUpdate *>(cntx);
        updateNeighbor(*update);
        break;
    }
    case SUBJECT_TYPE_FDB_CHANGE:
    {
        FdbUpdate *update = static_cast<FdbUpdate *>(cntx);
        updateFdb(*update);
        break;
    }
    case SUBJECT_TYPE_LAG_MEMBER_CHANGE:
    {
        LagMemberUpdate *update = static_cast<LagMemberUpdate *>(cntx);
        updateLagMember(*update);
        break;
    }
    case SUBJECT_TYPE_VLAN_MEMBER_CHANGE:
    {
        VlanMemberUpdate *update = static_cast<VlanMemberUpdate *>(cntx);
        updateVlanMember(*update);
        break;
    }
    default:
        // Received update in which we are not interested
        // Ignore it
        return;
    }
}

void MirrorOrch::createEntry(const string& key, const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();

    MirrorEntry entry = { };

    for (auto i : data)
    {
        if (fvField(i) == MIRROR_SESSION_SRC_IP)
        {
            entry.srcIp = fvValue(i);
            if (!entry.srcIp.isV4())
            {
                SWSS_LOG_ERROR("Unsupported version of sessions %s source IP address\n", key.c_str());
                return;
            }
        }
        else if (fvField(i) == MIRROR_SESSION_DST_IP)
        {
            entry.dstIp = fvValue(i);
            if (!entry.dstIp.isV4())
            {
                SWSS_LOG_ERROR("Unsupported version of sessions %s destination IP address\n", key.c_str());
                return;
            }
        }
        else if (fvField(i) == MIRROR_SESSION_GRE_TYPE)
        {
            entry.greType = stoul(fvValue(i), NULL, 0);
        }
        else if (fvField(i) == MIRROR_SESSION_DSCP)
        {
            entry.dscp = stoul(fvValue(i), NULL, 0);
        }
        else if (fvField(i) == MIRROR_SESSION_TTL)
        {
            entry.ttl = stoul(fvValue(i), NULL, 0);
        }
        else if (fvField(i) == MIRROR_SESSION_QUEUE)
        {
            entry.queue = stoul(fvValue(i), NULL, 0);
        }
        else if (fvField(i) == MIRROR_SESSION_STATUS)
        {
            // Status update always caused by MirrorOrch and should
            // not be changed by users. Ignore it.
            return;
        }
        else
        {
            SWSS_LOG_ERROR("Failed to parse session %s configuration. Unknown attribute %s.\n", key.c_str(), fvField(i).c_str());
            return;
        }
    }

    SWSS_LOG_INFO("Creating mirroring sessions %s\n", key.c_str());

    auto session = m_syncdMirrors.find(key);
    if (session != m_syncdMirrors.end())
    {
        SWSS_LOG_ERROR("Failed to create session. Session %s already exists.\n", key.c_str());
        return;
    }

    m_syncdMirrors.emplace(key, entry);

    setSessionState(key, entry);

    m_routeOrch->attach(this, entry.dstIp);
}

void MirrorOrch::deleteEntry(const string& name)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Deleting mirroring sessions %s\n", name.c_str());

    auto sessionIter = m_syncdMirrors.find(name);
    if (sessionIter == m_syncdMirrors.end())
    {
        SWSS_LOG_ERROR("Failed to delete session. Session %s doesn't exist.\n", name.c_str());
        return;
    }

    auto& session = sessionIter->second;

    if (session.status)
    {
        m_routeOrch->detach(this, session.dstIp);
        deactivateSession(name, session);
    }

    m_syncdMirrors.erase(sessionIter);
}

bool MirrorOrch::setSessionState(const string& name, MirrorEntry& session)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Setting mirroring sessions %s state\n", name.c_str());

    vector<FieldValueTuple> fvVector;

    string status = session.status ? MIRROR_SESSION_STATUS_ACTIVE : MIRROR_SESSION_STATUS_INACTIVE;

    FieldValueTuple t(MIRROR_SESSION_STATUS, status);
    fvVector.push_back(t);

    m_mirrorTableProducer.set(name, fvVector);

    return true;
}

bool MirrorOrch::getNeighborInfo(const string& name, MirrorEntry& session)
{
    SWSS_LOG_ENTER();

    NeighborEntry neighbor;
    MacAddress mac;

    assert(session.nexthopInfo.resolved);

    SWSS_LOG_INFO("Getting neighbor info for %s session\n", name.c_str());

    if (!m_neighOrch->getNeighborEntry(session.nexthopInfo.nexthop, neighbor, mac))
    {
        session.neighborInfo.resolved = false;
        return false;
    }

    return getNeighborInfo(name, session, neighbor, mac);
}

bool MirrorOrch::getNeighborInfo(const string& name, MirrorEntry& session, const NeighborEntry& neighbor, const MacAddress& mac)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Getting neighbor info for %s session\n", name.c_str());

    session.neighborInfo.resolved = false;
    session.neighborInfo.neighbor = neighbor;
    session.neighborInfo.mac = mac;

    // Get port operation should not fail;
    if (!m_portsOrch->getPort(session.neighborInfo.neighbor.alias, session.neighborInfo.port))
    {
        throw runtime_error("Failed to get port for " + session.neighborInfo.neighbor.alias + " alias");
    }

    if (session.neighborInfo.port.m_type == Port::VLAN)
    {
        session.neighborInfo.vlanId = session.neighborInfo.port.m_vlan_id;

        Port member;
        if (!m_fdbOrch->getPort(session.neighborInfo.mac, session.neighborInfo.vlanId, member))
        {
            return false;
        }

        session.neighborInfo.portId = member.m_port_id;
        session.neighborInfo.resolved = true;

        return true;
    }
    else if (session.neighborInfo.port.m_type == Port::LAG)
    {
        session.neighborInfo.vlanId = session.neighborInfo.port.m_port_vlan_id;

        if (session.neighborInfo.port.m_members.empty())
        {
            return false;
        }

        const auto& firstMember = *session.neighborInfo.port.m_members.begin();
        Port lagMember;
        if (!m_portsOrch->getPort(firstMember, lagMember))
        {
            throw runtime_error("Failed to get port for " + firstMember + " alias");
        }

        session.neighborInfo.portId = lagMember.m_port_id;
        session.neighborInfo.resolved = true;
    }
    else
    {
        session.neighborInfo.portId = session.neighborInfo.port.m_port_id;
        session.neighborInfo.vlanId = session.neighborInfo.port.m_port_vlan_id;
        session.neighborInfo.resolved = true;
    }

    return true;
}

bool MirrorOrch::activateSession(const string& name, MirrorEntry& session)
{
    sai_status_t status = SAI_STATUS_SUCCESS;
    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    SWSS_LOG_INFO("Activating mirror session %s\n", name.c_str());

    assert(!session.status);

    attr.id = SAI_MIRROR_SESSION_ATTR_TC;
    attr.value.u8 = session.queue;
    attrs.push_back(attr);

    attr.id =  SAI_MIRROR_SESSION_ATTR_MONITOR_PORT;
    attr.value.oid = session.neighborInfo.portId;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_TYPE;
    attr.value.s32 = SAI_MIRROR_TYPE_ENHANCED_REMOTE;
    attrs.push_back(attr);

    attr.id =SAI_MIRROR_SESSION_ATTR_VLAN_TPID;
    attr.value.u16 = ETH_P_8021Q;
    attrs.push_back(attr);

    attr.id =SAI_MIRROR_SESSION_ATTR_VLAN_ID;
    attr.value.u16 = session.neighborInfo.vlanId;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_PRI;
    attr.value.u8 = MIRROR_SESSION_DEFAULT_VLAN_PRI;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_CFI;
    attr.value.u8 = MIRROR_SESSION_DEFAULT_VLAN_CFI;
    attrs.push_back(attr);

    attr.id =SAI_MIRROR_SESSION_ATTR_ENCAP_TYPE;
    attr.value.s32 = SAI_MIRROR_L3_GRE_TUNNEL;
    attrs.push_back(attr);

    attr.id =SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION;
    attr.value.u8 = MIRROR_SESSION_DEFAULT_IP_HDR_VER;
    attrs.push_back(attr);

    attr.id =SAI_MIRROR_SESSION_ATTR_TOS;
    attr.value.u16 = MIRROR_SESSION_DEFAULT_TOS;
    attrs.push_back(attr);

    attr.id =SAI_MIRROR_SESSION_ATTR_TTL;
    attr.value.u8 = session.ttl;
    attrs.push_back(attr);

    attr.id =SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS;
    copy(attr.value.ipaddr, session.srcIp);
    attrs.push_back(attr);

    attr.id =SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS;
    copy(attr.value.ipaddr, session.dstIp);
    attrs.push_back(attr);

    attr.id =SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, gMacAddress.getMac(), sizeof(sai_mac_t));
    attrs.push_back(attr);

    attr.id =SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, session.neighborInfo.mac.getMac(), sizeof(sai_mac_t));
    attrs.push_back(attr);

    attr.id =SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE;
    attr.value.u16 = session.greType;
    attrs.push_back(attr);

    session.status = true;

    status = sai_mirror_api->create_mirror_session(&session.sessionId, attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to activate mirroring session %s\n", name.c_str());
        session.status = false;
    }

    return setSessionState(name, session);
}

bool MirrorOrch::deactivateSession(const string& name, MirrorEntry& session)
{
    SWSS_LOG_INFO("Deactivating mirror session %s\n", name.c_str());

    assert(session.status);

    sai_status_t status = sai_mirror_api->remove_mirror_session(session.sessionId);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to deactivate mirroring session %s\n", name.c_str());
        return false;
    }

    session.status = false;

    return setSessionState(name, session);
}

bool MirrorOrch::updateSessionDstMac(const string& name, MirrorEntry& session)
{
    SWSS_LOG_ENTER();

    assert(session.sessionId != SAI_NULL_OBJECT_ID);

    SWSS_LOG_INFO("Updating mirror session %s destination MAC address\n", name.c_str());

    sai_status_t status = SAI_STATUS_SUCCESS;
    sai_attribute_t attr;

    attr.id =SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, session.neighborInfo.mac.getMac(), sizeof(sai_mac_t));

    status = sai_mirror_api->set_mirror_session_attribute(session.sessionId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to update mirroring session %s destination MAC address\n", name.c_str());
        return false;
    }

    return true;
}

bool MirrorOrch::updateSessionDstPort(const string& name, MirrorEntry& session)
{
    SWSS_LOG_ENTER();

    assert(session.sessionId != SAI_NULL_OBJECT_ID);

    SWSS_LOG_INFO("Updating mirror session %s destination port\n", name.c_str());

    sai_status_t status = SAI_STATUS_SUCCESS;
    sai_attribute_t attr;

    attr.id =  SAI_MIRROR_SESSION_ATTR_MONITOR_PORT;
    attr.value.oid = session.neighborInfo.portId;

    status = sai_mirror_api->set_mirror_session_attribute(session.sessionId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to update mirroring session %s destination port\n", name.c_str());
        return false;
    }

    return true;
}

void MirrorOrch::updateNextHop(const NextHopUpdate& update)
{
    SWSS_LOG_ENTER();

    auto sessionIter = m_syncdMirrors.begin();
    for (; sessionIter != m_syncdMirrors.end(); ++sessionIter)
    {
        if (!update.prefix.isAddressInSubnet(sessionIter->second.dstIp))
        {
            continue;
        }

        const auto& name = sessionIter->first;
        auto& session = sessionIter->second;

        SWSS_LOG_INFO("Updating mirror session %s next hop\n", name.c_str());

        if (session.nexthopInfo.resolved)
        {
            // Check for ECMP route next hop update. If route prefix is the same
            // and current next hop is still in next hop group - do nothing.
            if (session.nexthopInfo.prefix == update.prefix && update.nexthopGroup.getIpAddresses().count(session.nexthopInfo.nexthop))
            {
                return;
            }
        }

        session.nexthopInfo.nexthop = *update.nexthopGroup.getIpAddresses().begin();
        session.nexthopInfo.prefix = update.prefix;
        session.nexthopInfo.resolved = true;

        // Get neighbor
        if (!getNeighborInfo(name, session))
        {
            // Next hop changed. New neighbor is not resolved. Remove session.
            if (session.status)
            {
                deactivateSession(name, session);
            }
            return;
        }

        if (session.status)
        {
            if (!updateSessionDstMac(name, session))
            {
                return;
            }

            if (!updateSessionDstPort(name, session))
            {
                return;
            }
        }
        else
        {
            if (!activateSession(name, session))
            {
                return;
            }
        }
    }
}

void MirrorOrch::updateNeighbor(const NeighborUpdate& update)
{
    SWSS_LOG_ENTER();

    auto sessionIter = m_syncdMirrors.begin();
    for (; sessionIter != m_syncdMirrors.end(); ++sessionIter)
    {
        if (!sessionIter->second.nexthopInfo.resolved)
        {
            continue;
        }

        // It is possible to have few sessions that points to one next hop
        if (sessionIter->second.nexthopInfo.nexthop != update.entry.ip_address)
        {
            continue;
        }

        SWSS_LOG_INFO("Updating neighbor info %s %s\n", update.entry.ip_address.to_string().c_str(), update.entry.alias.c_str());

        const auto& name = sessionIter->first;
        auto& session = sessionIter->second;

        if (update.add)
        {
            if (!getNeighborInfo(name, session, update.entry, update.mac))
            {
                if (session.status)
                {
                    deactivateSession(name, session);
                }
               return;
            }

            if (session.status)
            {
                if (!updateSessionDstMac(name, session))
                {
                    return;
                }

                if (!updateSessionDstPort(name, session))
                {
                    return;
                }
            }
            else
            {
                activateSession(name, session);
            }
        }
        else if (session.status)
        {
            deactivateSession(name, session);
            session.neighborInfo.resolved = false;
        }
    }
}

void MirrorOrch::updateFdb(const FdbUpdate& update)
{
    SWSS_LOG_ENTER();

    auto sessionIter = m_syncdMirrors.begin();
    for (; sessionIter != m_syncdMirrors.end(); ++sessionIter)
    {
        if (!sessionIter->second.neighborInfo.resolved ||
                sessionIter->second.neighborInfo.port.m_type != Port::VLAN)
        {
            continue;
        }

        // It is possible to have few session that points to one FDB entry
        if (sessionIter->second.neighborInfo.mac != update.entry.mac ||
                sessionIter->second.neighborInfo.vlanId != update.entry.vlan)
        {
            continue;
        }

        const auto& name = sessionIter->first;
        auto& session = sessionIter->second;

        if (update.add)
        {
            if (session.status)
            {
                // update port if changed
                if (session.neighborInfo.portId != update.port.m_port_id)
                {
                    session.neighborInfo.portId = update.port.m_port_id;
                    updateSessionDstPort(name, session);
                }
            }
            else
            {
                //activate session
                session.neighborInfo.resolved = true;
                session.neighborInfo.mac = update.entry.mac;
                session.neighborInfo.portId = update.port.m_port_id;

                activateSession(name, session);
            }
        }
        else
        {
            deactivateSession(name, session);
            session.neighborInfo.portId = SAI_NULL_OBJECT_ID;
        }
    }
}

void MirrorOrch::updateLagMember(const LagMemberUpdate& update)
{
    SWSS_LOG_ENTER();
    auto sessionIter = m_syncdMirrors.begin();
    for (; sessionIter != m_syncdMirrors.end(); ++sessionIter)
    {
        if (!sessionIter->second.neighborInfo.resolved)
        {
            continue;
        }

        if (sessionIter->second.neighborInfo.port.m_type != Port::LAG)
        {
            continue;
        }

        // It is possible to have few session that points to one LAG member
        if (sessionIter->second.neighborInfo.port != update.lag || sessionIter->second.neighborInfo.portId != update.member.m_port_id)
        {
            continue;
        }

        const auto& name = sessionIter->first;
        auto& session = sessionIter->second;

        if (update.add)
        {
            // We interesting only in first LAG member
            if (update.lag.m_members.size() > 1)
            {
                return;
            }

            const string& memberName = *update.lag.m_members.begin();
            Port member;
            if (!m_portsOrch->getPort(memberName, member))
            {
                SWSS_LOG_ERROR("Failed to get port for %s alias\n", memberName.c_str());
                assert(false);
            }

            session.neighborInfo.portId = member.m_port_id;

            activateSession(name, session);
        }
        else
        {
            // If LAG is empty deactivate session
            if (update.lag.m_members.empty())
            {
                deactivateSession(name, session);
                session.neighborInfo.portId = SAI_OBJECT_TYPE_NULL;

                return;
            }

            // Get another LAG member and update session
            const string& memberName = *update.lag.m_members.begin();

            Port member;
            if (!m_portsOrch->getPort(memberName, member))
            {
                SWSS_LOG_ERROR("Failed to get port for %s alias\n", memberName.c_str());
                assert(false);
            }

            session.neighborInfo.portId = member.m_port_id;

            updateSessionDstPort(name, session);
        }
    }
}

void MirrorOrch::updateVlanMember(const VlanMemberUpdate& update)
{
    SWSS_LOG_ENTER();

    // We looking only for removed members
    if (update.add)
    {
        return;
    }

    auto sessionIter = m_syncdMirrors.begin();
    for (; sessionIter != m_syncdMirrors.end(); ++sessionIter)
    {
        if (!sessionIter->second.neighborInfo.resolved)
        {
            continue;
        }

        if (sessionIter->second.neighborInfo.port.m_type != Port::VLAN)
        {
            continue;
        }

        // It is possible to have few session that points to one VLAN member
        if (sessionIter->second.neighborInfo.port != update.vlan || sessionIter->second.neighborInfo.portId != update.member.m_port_id)
        {
            continue;
        }

        const auto& name = sessionIter->first;
        auto& session = sessionIter->second;

        // Deactivate session. Wait for FDB event to activate session
        deactivateSession(name, session);
        session.neighborInfo.portId = SAI_OBJECT_TYPE_NULL;
    }
}

void MirrorOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            createEntry(key, kfvFieldsValues(t));
        }
        else if (op == DEL_COMMAND)
        {
            deleteEntry(key);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
        }

        consumer.m_toSync.erase(it++);
    }
}
