#include <linux/if_ether.h>

#include <unordered_map>
#include <utility>
#include <exception>

#include "sai_serialize.h"
#include "orch.h"
#include "logger.h"
#include "swssnet.h"
#include "converter.h"
#include "mirrororch.h"

#define MIRROR_SESSION_STATUS               "status"
#define MIRROR_SESSION_STATUS_ACTIVE        "active"
#define MIRROR_SESSION_STATUS_INACTIVE      "inactive"
#define MIRROR_SESSION_SRC_IP               "src_ip"
#define MIRROR_SESSION_DST_IP               "dst_ip"
#define MIRROR_SESSION_GRE_TYPE             "gre_type"
#define MIRROR_SESSION_DSCP                 "dscp"
#define MIRROR_SESSION_TTL                  "ttl"
#define MIRROR_SESSION_QUEUE                "queue"
#define MIRROR_SESSION_DST_MAC_ADDRESS      "dst_mac"
#define MIRROR_SESSION_MONITOR_PORT         "monitor_port"
#define MIRROR_SESSION_ROUTE_PREFIX         "route_prefix"
#define MIRROR_SESSION_VLAN_HEADER_VALID    "vlan_header_valid"
#define MIRROR_SESSION_POLICER              "policer"

#define MIRROR_SESSION_DEFAULT_VLAN_PRI 0
#define MIRROR_SESSION_DEFAULT_VLAN_CFI 0
#define MIRROR_SESSION_DEFAULT_IP_HDR_VER 4
#define MIRROR_SESSION_DSCP_SHIFT       2
#define MIRROR_SESSION_DSCP_MIN         0
#define MIRROR_SESSION_DSCP_MAX         63

extern sai_mirror_api_t *sai_mirror_api;

extern sai_object_id_t  gSwitchId;
extern PortsOrch*       gPortsOrch;

using namespace std::rel_ops;

MirrorEntry::MirrorEntry(const string& platform) :
        status(false),
        dscp(8),
        ttl(255),
        queue(0),
        sessionId(0),
        refCount(0)
{
    if (platform == MLNX_PLATFORM_SUBSTRING)
    {
        greType = 0x8949;
    }
    else
    {
        greType = 0x88be;
    }

    nexthopInfo.prefix = IpPrefix("0.0.0.0/0");
}

MirrorOrch::MirrorOrch(TableConnector stateDbConnector, TableConnector confDbConnector,
        PortsOrch *portOrch, RouteOrch *routeOrch, NeighOrch *neighOrch, FdbOrch *fdbOrch, PolicerOrch *policerOrch) :
        Orch(confDbConnector.first, confDbConnector.second),
        m_portsOrch(portOrch),
        m_routeOrch(routeOrch),
        m_neighOrch(neighOrch),
        m_fdbOrch(fdbOrch),
        m_policerOrch(policerOrch),
        m_mirrorTable(stateDbConnector.first, stateDbConnector.second)
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

bool MirrorOrch::sessionExists(const string& name)
{
    SWSS_LOG_ENTER();

    return m_syncdMirrors.find(name) != m_syncdMirrors.end();
}

bool MirrorOrch::getSessionStatus(const string& name, bool& state)
{
    SWSS_LOG_ENTER();

    if (!sessionExists(name))
    {
        return false;
    }

    state = m_syncdMirrors.find(name)->second.status;

    return true;
}

bool MirrorOrch::getSessionOid(const string& name, sai_object_id_t& oid)
{
    SWSS_LOG_ENTER();

    if (!sessionExists(name))
    {
        return false;
    }

    oid = m_syncdMirrors.find(name)->second.sessionId;

    return true;
}

bool MirrorOrch::increaseRefCount(const string& name)
{
    SWSS_LOG_ENTER();

    if (!sessionExists(name))
    {
        return false;
    }

    ++m_syncdMirrors.find(name)->second.refCount;

    return true;
}

bool MirrorOrch::decreaseRefCount(const string& name)
{
    SWSS_LOG_ENTER();

    if (!sessionExists(name))
    {
        return false;
    }

    auto session = m_syncdMirrors.find(name);

    if (session->second.refCount <= 0)
    {
        throw runtime_error("Session reference counter could not be less or equal than 0");
    }

    --session->second.refCount;

    return true;
}

void MirrorOrch::createEntry(const string& key, const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();

    auto session = m_syncdMirrors.find(key);
    if (session != m_syncdMirrors.end())
    {
        SWSS_LOG_NOTICE("Failed to create session, session %s already exists",
                key.c_str());
        return;
    }

    string platform = getenv("platform") ? getenv("platform") : "";
    MirrorEntry entry(platform);

    for (auto i : data)
    {
        try {
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
                entry.greType = to_uint<uint16_t>(fvValue(i));
            }
            else if (fvField(i) == MIRROR_SESSION_DSCP)
            {
                entry.dscp = to_uint<uint8_t>(fvValue(i), MIRROR_SESSION_DSCP_MIN, MIRROR_SESSION_DSCP_MAX);
            }
            else if (fvField(i) == MIRROR_SESSION_TTL)
            {
                entry.ttl = to_uint<uint8_t>(fvValue(i));
            }
            else if (fvField(i) == MIRROR_SESSION_QUEUE)
            {
                entry.queue = to_uint<uint8_t>(fvValue(i));
            }
            else if (fvField(i) == MIRROR_SESSION_POLICER)
            {
                if (!m_policerOrch->policerExists(fvValue(i)))
                {
                    SWSS_LOG_ERROR("Failed to get policer %s",
                            fvValue(i).c_str());
                    return;
                }

                m_policerOrch->increaseRefCount(fvValue(i));
                entry.policer = fvValue(i);
            }
            else
            {
                SWSS_LOG_ERROR("Failed to parse session %s configuration. Unknown attribute %s.\n", key.c_str(), fvField(i).c_str());
                return;
            }
        }
        catch (const exception& e)
        {
            SWSS_LOG_ERROR("Failed to parse session %s attribute %s error: %s.", key.c_str(), fvField(i).c_str(), e.what());
            return;
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Failed to parse session %s attribute %s. Unknown error has been occurred", key.c_str(), fvField(i).c_str());
            return;
        }
    }

    m_syncdMirrors.emplace(key, entry);

    SWSS_LOG_NOTICE("Created mirror session %s", key.c_str());

    setSessionState(key, entry);

    // Attach the destination IP to the routeOrch
    m_routeOrch->attach(this, entry.dstIp);
}

void MirrorOrch::deleteEntry(const string& name)
{
    SWSS_LOG_ENTER();

    auto sessionIter = m_syncdMirrors.find(name);
    if (sessionIter == m_syncdMirrors.end())
    {
        SWSS_LOG_ERROR("Failed to delete session. Session %s doesn't exist.\n", name.c_str());
        return;
    }

    auto& session = sessionIter->second;

    if (session.refCount)
    {
        SWSS_LOG_ERROR("Failed to delete session. Session %s in use.\n", name.c_str());
        return;
    }

    if (session.status)
    {
        m_routeOrch->detach(this, session.dstIp);
        deactivateSession(name, session);
    }

    if (!session.policer.empty())
    {
        m_policerOrch->decreaseRefCount(session.policer);
    }

    m_syncdMirrors.erase(sessionIter);

    SWSS_LOG_NOTICE("Removed mirror session %s", name.c_str());
}

void MirrorOrch::setSessionState(const string& name, const MirrorEntry& session, const string& attr)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Setting mirroring sessions %s state\n", name.c_str());

    vector<FieldValueTuple> fvVector;
    string value;
    if (attr.empty() || attr == MIRROR_SESSION_STATUS)
    {
        value = session.status ? MIRROR_SESSION_STATUS_ACTIVE : MIRROR_SESSION_STATUS_INACTIVE;
        fvVector.emplace_back(MIRROR_SESSION_STATUS, value);
    }

    if (attr.empty() || attr == MIRROR_SESSION_MONITOR_PORT)
    {
        value = sai_serialize_object_id(session.neighborInfo.portId);
        fvVector.emplace_back(MIRROR_SESSION_MONITOR_PORT, value);
    }

    if (attr.empty() || attr == MIRROR_SESSION_DST_MAC_ADDRESS)
    {
        value = session.neighborInfo.mac.to_string();
        fvVector.emplace_back(MIRROR_SESSION_DST_MAC_ADDRESS, value);
    }

    if (attr.empty() || attr == MIRROR_SESSION_ROUTE_PREFIX)
    {
        value = session.nexthopInfo.prefix.to_string();
        fvVector.emplace_back(MIRROR_SESSION_ROUTE_PREFIX, value);
    }

    if (attr.empty() || attr == MIRROR_SESSION_VLAN_HEADER_VALID)
    {
        value = to_string(session.neighborInfo.port.m_type == Port::VLAN);
        fvVector.emplace_back(MIRROR_SESSION_VLAN_HEADER_VALID, value);
    }

    m_mirrorTable.set(name, fvVector);
}

bool MirrorOrch::getNeighborInfo(const string& name, MirrorEntry& session)
{
    SWSS_LOG_ENTER();

    // 1) If session destination IP is directly connected, and the neighbor
    //    information is retrieved successfully, then continue.
    // 2) If session has next hop, and the next hop's neighbor information is
    //    retrieved successfully, then continue.
    // 3) Otherwise, return false.
    if (!m_neighOrch->getNeighborEntry(session.dstIp,
                session.neighborInfo.neighbor, session.neighborInfo.mac) &&
            (session.nexthopInfo.nexthop.isZero() ||
            !m_neighOrch->getNeighborEntry(session.nexthopInfo.nexthop,
                session.neighborInfo.neighbor, session.neighborInfo.mac)))
    {
        return false;
    }

    SWSS_LOG_NOTICE("Mirror session %s neighbor is %s",
            name.c_str(), session.neighborInfo.neighbor.alias.c_str());

    // Get mirror session monitor port information
    m_portsOrch->getPort(session.neighborInfo.neighbor.alias,
            session.neighborInfo.port);

    switch (session.neighborInfo.port.m_type)
    {
        case Port::PHY:
        {
            session.neighborInfo.portId = session.neighborInfo.port.m_port_id;
            return true;
        }
        case Port::LAG:
        {
            if (session.neighborInfo.port.m_members.empty())
            {
                return false;
            }

            // Get the firt member of the LAG
            Port member;
            const auto& first_member_alias = *session.neighborInfo.port.m_members.begin();
            m_portsOrch->getPort(first_member_alias, member);

            session.neighborInfo.portId = member.m_port_id;
            return true;
        }
        case Port::VLAN:
        {
            SWSS_LOG_NOTICE("Get mirror session destination IP neighbor VLAN %d",
                    session.neighborInfo.port.m_vlan_info.vlan_id);
            Port member;
            if (!m_fdbOrch->getPort(session.neighborInfo.mac, session.neighborInfo.port.m_vlan_info.vlan_id, member))
            {
                SWSS_LOG_NOTICE("Waiting to get FDB entry MAC %s under VLAN %s",
                        session.neighborInfo.mac.to_string().c_str(),
                        session.neighborInfo.port.m_alias.c_str());
                return false;
            }
            else
            {
                // Update monitor port
                session.neighborInfo.portId = member.m_port_id;
                return true;
            }
        }
        default:
        {
            return false;
        }
    }
}

bool MirrorOrch::updateSession(const string& name, MirrorEntry& session)
{
    SWSS_LOG_ENTER();

    bool ret = true;
    MirrorEntry old_session(session);

    // Get neighbor information
    if (getNeighborInfo(name, session))
    {
        // Update corresponding attributes
        if (session.status)
        {
            if (old_session.neighborInfo.port.m_type !=
                    session.neighborInfo.port.m_type &&
                (old_session.neighborInfo.port.m_type == Port::VLAN ||
                 session.neighborInfo.port.m_type == Port::VLAN))
            {
                ret &= updateSessionType(name, session);
            }

            if (old_session.neighborInfo.mac !=
                    session.neighborInfo.mac)
            {
                ret &= updateSessionDstMac(name, session);
            }

            if (old_session.neighborInfo.portId !=
                    session.neighborInfo.portId)
            {
                ret &= updateSessionDstPort(name, session);
            }
        }
        // Activate mirror session
        else
        {
            ret &= activateSession(name, session);
        }
    }
    // Deactivate mirror session and wait for update
    else
    {
        if (session.status)
        {
            ret &= deactivateSession(name, session);
        }
    }

    return ret;
}

bool MirrorOrch::activateSession(const string& name, MirrorEntry& session)
{
    SWSS_LOG_ENTER();

    sai_status_t status;
    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    assert(!session.status);

    // Some platforms don't support SAI_MIRROR_SESSION_ATTR_TC and only
    // support global mirror session traffic class.
    if (session.queue != 0)
    {
        attr.id = SAI_MIRROR_SESSION_ATTR_TC;
        attr.value.u8 = session.queue;
        attrs.push_back(attr);
    }

    attr.id = SAI_MIRROR_SESSION_ATTR_MONITOR_PORT;
    attr.value.oid = session.neighborInfo.portId;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_TYPE;
    attr.value.s32 = SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE;
    attrs.push_back(attr);

    // Add the VLAN header when the packet is sent out from a VLAN
    if (session.neighborInfo.port.m_type == Port::VLAN)
    {
        attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_HEADER_VALID;
        attr.value.booldata = true;
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_TPID;
        attr.value.u16 = ETH_P_8021Q;
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_ID;
        attr.value.u16 = session.neighborInfo.port.m_vlan_info.vlan_id;
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_PRI;
        attr.value.u8 = MIRROR_SESSION_DEFAULT_VLAN_PRI;
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_CFI;
        attr.value.u8 = MIRROR_SESSION_DEFAULT_VLAN_CFI;
        attrs.push_back(attr);
    }

    attr.id = SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE;
    attr.value.s32 = SAI_ERSPAN_ENCAPSULATION_TYPE_MIRROR_L3_GRE_TUNNEL;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION;
    attr.value.u8 = MIRROR_SESSION_DEFAULT_IP_HDR_VER;
    attrs.push_back(attr);

    // TOS value format is the following:
    // DSCP 6 bits | ECN 2 bits
    attr.id = SAI_MIRROR_SESSION_ATTR_TOS;
    attr.value.u16 = (uint16_t)(session.dscp << MIRROR_SESSION_DSCP_SHIFT);
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_TTL;
    attr.value.u8 = session.ttl;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS;
    copy(attr.value.ipaddr, session.srcIp);
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS;
    copy(attr.value.ipaddr, session.dstIp);
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, gMacAddress.getMac(), sizeof(sai_mac_t));
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, session.neighborInfo.mac.getMac(), sizeof(sai_mac_t));
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE;
    attr.value.u16 = session.greType;
    attrs.push_back(attr);

    if (!session.policer.empty())
    {
        sai_object_id_t oid = SAI_NULL_OBJECT_ID;
        if (!m_policerOrch->getPolicerOid(session.policer, oid))
        {
            SWSS_LOG_ERROR("Faield to get policer %s", session.policer.c_str());
            return false;
        }

        attr.id = SAI_MIRROR_SESSION_ATTR_POLICER;
        attr.value.oid = oid;
        attrs.push_back(attr);
    }

    status = sai_mirror_api->
        create_mirror_session(&session.sessionId, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to activate mirroring session %s\n", name.c_str());
        session.status = false;

        return false;
    }

    session.status = true;
    setSessionState(name, session);

    MirrorSessionUpdate update = { name, true };
    notify(SUBJECT_TYPE_MIRROR_SESSION_CHANGE, static_cast<void *>(&update));

    SWSS_LOG_NOTICE("Activated mirror session %s", name.c_str());

    return true;
}

bool MirrorOrch::deactivateSession(const string& name, MirrorEntry& session)
{
    SWSS_LOG_ENTER();

    assert(session.status);

    MirrorSessionUpdate update = { name, false };
    notify(SUBJECT_TYPE_MIRROR_SESSION_CHANGE, static_cast<void *>(&update));

    sai_status_t status = sai_mirror_api->
        remove_mirror_session(session.sessionId);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to deactivate mirroring session %s\n", name.c_str());
        return false;
    }

    session.status = false;

    // Store whole state into StateDB, since it is far from that frequent it's durable
    setSessionState(name, session);

    SWSS_LOG_NOTICE("Deactivated mirror session %s", name.c_str());

    return true;
}

bool MirrorOrch::updateSessionDstMac(const string& name, MirrorEntry& session)
{
    SWSS_LOG_ENTER();

    assert(session.sessionId != SAI_NULL_OBJECT_ID);

    sai_attribute_t attr;
    attr.id = SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, session.neighborInfo.mac.getMac(), sizeof(sai_mac_t));

    sai_status_t status = sai_mirror_api->set_mirror_session_attribute(session.sessionId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to update mirror session %s destination MAC to %s, rv:%d",
                name.c_str(), session.neighborInfo.mac.to_string().c_str(), status);
        return false;
    }

    SWSS_LOG_NOTICE("Update mirror session %s destination MAC to %s",
            name.c_str(), session.neighborInfo.mac.to_string().c_str());

    setSessionState(name, session, MIRROR_SESSION_DST_MAC_ADDRESS);

    return true;
}

bool MirrorOrch::updateSessionDstPort(const string& name, MirrorEntry& session)
{
    SWSS_LOG_ENTER();

    assert(session.sessionId != SAI_NULL_OBJECT_ID);

    Port port;
    m_portsOrch->getPort(session.neighborInfo.portId, port);

    sai_attribute_t attr;
    attr.id = SAI_MIRROR_SESSION_ATTR_MONITOR_PORT;
    attr.value.oid = session.neighborInfo.portId;

    sai_status_t status = sai_mirror_api->
        set_mirror_session_attribute(session.sessionId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to update mirror session %s monitor port to %s, rv:%d",
                name.c_str(), port.m_alias.c_str(), status);
        return false;
    }

    SWSS_LOG_NOTICE("Update mirror session %s monitor port to %s",
            name.c_str(), port.m_alias.c_str());

    setSessionState(name, session, MIRROR_SESSION_MONITOR_PORT);

    return true;
}

bool MirrorOrch::updateSessionType(const string& name, MirrorEntry& session)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    if (session.neighborInfo.port.m_type == Port::VLAN)
    {
        attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_HEADER_VALID;
        attr.value.booldata = true;
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_TPID;
        attr.value.u16 = ETH_P_8021Q;
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_ID;
        attr.value.u16 = session.neighborInfo.port.m_vlan_info.vlan_id;
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_PRI;
        attr.value.u8 = MIRROR_SESSION_DEFAULT_VLAN_PRI;
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_CFI;
        attr.value.u8 = MIRROR_SESSION_DEFAULT_VLAN_CFI;
        attrs.push_back(attr);
    }
    else
    {
        attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_HEADER_VALID;
        attr.value.booldata = false;
        attrs.push_back(attr);
    }

    sai_status_t status;
    for (auto attr : attrs)
    {
        status = sai_mirror_api->
            set_mirror_session_attribute(session.sessionId, &attr);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to update mirror session %s VLAN to %s, rv:%d",
                    name.c_str(), session.neighborInfo.port.m_alias.c_str(), status);
            return false;
        }
    }

    SWSS_LOG_NOTICE("Update mirror session %s VLAN to %s",
            name.c_str(), session.neighborInfo.port.m_alias.c_str());

    setSessionState(name, session, MIRROR_SESSION_VLAN_HEADER_VALID);

    return true;
}

// The function is called when SUBJECT_TYPE_NEXTHOP_CHANGE is received
// This function will handle the case when the session's destination IP's
// next hop changes.
void MirrorOrch::updateNextHop(const NextHopUpdate& update)
{
    SWSS_LOG_ENTER();

    for (auto it = m_syncdMirrors.begin(); it != m_syncdMirrors.end(); it++)
    {
        const auto& name = it->first;
        auto& session = it->second;

        // Check if mirror session's destination IP is the update's destination IP
        if (session.dstIp != update.destination)
        {
            continue;
        }

        session.nexthopInfo.prefix = update.prefix;

        setSessionState(name, session, MIRROR_SESSION_ROUTE_PREFIX);

        // This is the ECMP scenario that the new next hop group contains the previous
        // next hop. There is no need to update this session's monitor port.
        if (update.nexthopGroup != IpAddresses() &&
                update.nexthopGroup.getIpAddresses().count(session.nexthopInfo.nexthop))

        {
            continue;
        }

        SWSS_LOG_NOTICE("Updating mirror session %s with route %s",
                name.c_str(), update.prefix.to_string().c_str());

        if (update.nexthopGroup != IpAddresses())
        {
            session.nexthopInfo.nexthop = *update.nexthopGroup.getIpAddresses().begin();
        }
        else
        {
            session.nexthopInfo.nexthop = IpAddress(0);
        }

        // Resolve the neighbor of the new next hop
        updateSession(name, session);
    }
}

// The function is called when SUBJECT_TYPE_NEIGH_CHANGE is received.
// This function will handle the case when the neighbor is created or removed.
void MirrorOrch::updateNeighbor(const NeighborUpdate& update)
{
    SWSS_LOG_ENTER();

    for (auto it = m_syncdMirrors.begin(); it != m_syncdMirrors.end(); it++)
    {
        const auto& name = it->first;
        auto& session = it->second;

        // Check if the session's destination IP matches the neighbor's update IP
        // or if the session's next hop IP matches the neighbor's update IP
        if (session.dstIp != update.entry.ip_address &&
                session.nexthopInfo.nexthop != update.entry.ip_address)
        {
            continue;
        }

        SWSS_LOG_NOTICE("Updating mirror session %s with neighbor %s",
                name.c_str(), update.entry.alias.c_str());

        updateSession(name, session);
    }
}

// The function is called when SUBJECT_TYPE_FDB_CHANGE is received.
// This function will handle the case when new FDB enty is learned/added in the VLAN,
// or when the old FDB entry gets removed. Only when the neighbor is VLAN will the case
// be handled.
void MirrorOrch::updateFdb(const FdbUpdate& update)
{
    SWSS_LOG_ENTER();

    for (auto it = m_syncdMirrors.begin(); it != m_syncdMirrors.end(); it++)
    {
        const auto& name = it->first;
        auto& session = it->second;

        // Check the following three conditions:
        // 1) mirror session is pointing to a VLAN
        // 2) the VLAN matches the FDB notification VLAN ID
        // 3) the destination MAC matches the FDB notifaction MAC
        if (session.neighborInfo.port.m_type != Port::VLAN ||
                session.neighborInfo.port.m_vlan_info.vlan_oid != update.entry.bv_id ||
                session.neighborInfo.mac != update.entry.mac)
        {
            continue;
        }

        SWSS_LOG_NOTICE("Updating mirror session %s with monitor port %s",
                name.c_str(), update.port.m_alias.c_str());

        // Get the new monitor port
        if (update.add)
        {
            if (session.status)
            {
                // Update port if changed
                if (session.neighborInfo.portId != update.port.m_port_id)
                {
                    session.neighborInfo.portId = update.port.m_port_id;
                    updateSessionDstPort(name, session);
                }
            }
            else
            {
                // Activate session
                session.neighborInfo.portId = update.port.m_port_id;
                activateSession(name, session);
            }
        }
        // Remvoe the monitor port
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

    for (auto it = m_syncdMirrors.begin(); it != m_syncdMirrors.end(); it++)
    {
        const auto& name = it->first;
        auto& session = it->second;

        // Check the following two conditions:
        // 1) the neighbor is LAG
        // 2) the neighbor LAG matches the update LAG
        if (session.neighborInfo.port.m_type != Port::LAG ||
                session.neighborInfo.port != update.lag)
        {
            continue;
        }

        if (update.add)
        {
            // Activate mirror session if it was deactivated due to the reason
            // that previously there was no member in the LAG. If the mirror
            // session is already activated, no further action is needed.
            if (!session.status)
            {
                assert(!update.lag.m_members.empty());
                const string& member_name = *update.lag.m_members.begin();
                Port member;
                m_portsOrch->getPort(member_name, member);

                session.neighborInfo.portId = member.m_port_id;
                activateSession(name, session);
            }
        }
        else
        {
            // If LAG is empty, deactivate session
            if (update.lag.m_members.empty())
            {
                deactivateSession(name, session);
                session.neighborInfo.portId = SAI_OBJECT_TYPE_NULL;
            }
            // Switch to a new member of the LAG
            else
            {
                const string& member_name = *update.lag.m_members.begin();
                Port member;
                m_portsOrch->getPort(member_name, member);

                session.neighborInfo.portId = member.m_port_id;
                // The destination MAC remains the same
                updateSessionDstPort(name, session);
            }
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

    for (auto it = m_syncdMirrors.begin(); it != m_syncdMirrors.end(); it++)
    {
        const auto& name = it->first;
        auto& session = it->second;

        // Check the following three conditions:
        // 1) mirror session is pointing to a VLAN
        // 2) the VLAN matches the update VLAN
        // 3) the monitor port matches the update VLAN member
        if (session.neighborInfo.port.m_type != Port::VLAN ||
                session.neighborInfo.port != update.vlan ||
                session.neighborInfo.portId != update.member.m_port_id)
        {
            continue;
        }

        // Deactivate session. Wait for FDB event to activate session
        session.neighborInfo.portId = SAI_OBJECT_TYPE_NULL;
        deactivateSession(name, session);
    }
}

void MirrorOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->isPortReady())
    {
        return;
    }

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
