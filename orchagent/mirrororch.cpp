#include <linux/if_ether.h>

#include <unordered_map>
#include <algorithm>
#include <utility>
#include <exception>

#include "sai_serialize.h"
#include "orch.h"
#include "logger.h"
#include "swssnet.h"
#include "converter.h"
#include "mirrororch.h"
#include "tokenize.h"
#include "warm_restart.h"

#define MIRROR_SESSION_STATUS               "status"
#define MIRROR_SESSION_STATUS_ACTIVE        "active"
#define MIRROR_SESSION_STATUS_INACTIVE      "inactive"
#define MIRROR_SESSION_NEXT_HOP_IP          "next_hop_ip"
#define MIRROR_SESSION_SRC_IP               "src_ip"
#define MIRROR_SESSION_DST_IP               "dst_ip"
#define MIRROR_SESSION_GRE_TYPE             "gre_type"
#define MIRROR_SESSION_DSCP                 "dscp"
#define MIRROR_SESSION_TTL                  "ttl"
#define MIRROR_SESSION_QUEUE                "queue"
#define MIRROR_SESSION_DST_MAC_ADDRESS      "dst_mac"
#define MIRROR_SESSION_MONITOR_PORT         "monitor_port"
#define MIRROR_SESSION_ROUTE_PREFIX         "route_prefix"
#define MIRROR_SESSION_VLAN_ID              "vlan_id"
#define MIRROR_SESSION_POLICER              "policer"
#define MIRROR_SESSION_SRC_PORT             "src_port"
#define MIRROR_SESSION_DST_PORT             "dst_port"
#define MIRROR_SESSION_DIRECTION            "direction"
#define MIRROR_SESSION_TYPE                 "type"
#define PORT_MIRROR_SAMPLE_TABLE "PORT_MIRROR_SAMPLE"

#define MIRROR_SESSION_DEFAULT_VLAN_PRI 0
#define MIRROR_SESSION_DEFAULT_VLAN_CFI 0
#define MIRROR_SESSION_IP_HDR_VER_4     4
#define MIRROR_SESSION_IP_HDR_VER_6     6
#define MIRROR_SESSION_DSCP_SHIFT       2
#define MIRROR_SESSION_DSCP_MIN         0
#define MIRROR_SESSION_DSCP_MAX         63

// 15 is a typical value, but if vendor's SAI does not supply the maximum value,
// allow all 8-bit numbers, effectively cancelling validation by orchagent.
#define MIRROR_SESSION_DEFAULT_NUM_TC   255

extern sai_switch_api_t *sai_switch_api;
extern sai_mirror_api_t *sai_mirror_api;
extern sai_port_api_t *sai_port_api;
extern sai_samplepacket_api_t *sai_samplepacket_api;

extern sai_object_id_t  gSwitchId;
extern PortsOrch*       gPortsOrch;
extern string           gMySwitchType;

using namespace std::rel_ops;

MirrorEntry::MirrorEntry(const string& platform) :
        status(false),
        type(MIRROR_SESSION_ERSPAN),
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

    string alias = "";
    nexthopInfo.prefix = IpPrefix("0.0.0.0/0");
    nexthopInfo.nexthop = NextHopKey("0.0.0.0", alias);
}

MirrorOrch::MirrorOrch(TableConnector stateDbConnector, TableConnector confDbConnector,
        PortsOrch *portOrch, RouteOrch *routeOrch, NeighOrch *neighOrch, FdbOrch *fdbOrch, PolicerOrch *policerOrch, SwitchOrch *switchOrch) :
        Orch(confDbConnector.first, vector<string>{confDbConnector.second, PORT_MIRROR_SAMPLE_TABLE}),
        m_portsOrch(portOrch),
        m_routeOrch(routeOrch),
        m_neighOrch(neighOrch),
        m_fdbOrch(fdbOrch),
        m_policerOrch(policerOrch),
        m_switchOrch(switchOrch),
        m_mirrorTable(stateDbConnector.first, stateDbConnector.second),
        m_sampleConfigTable(confDbConnector.first, PORT_MIRROR_SAMPLE_TABLE),
        m_sflowConfigTable(confDbConnector.first, "SFLOW")
{
    sai_status_t status;
    sai_attribute_t attr;

    if (swss::SonicDBConfig::isGlobalInit())
    {
        for (const auto& ns : swss::SonicDBConfig::getNamespaces())
        {
            if (ns != confDbConnector.first->getNamespace())
                m_otherConfigDbs.emplace_back(new swss::DBConnector("CONFIG_DB", 1000, false, ns));
        }
    }

    m_portsOrch->attach(this);
    m_neighOrch->attach(this);
    m_fdbOrch->attach(this);

    // Retrieve the number of valid values for queue, starting at 0
    attr.id = SAI_SWITCH_ATTR_QOS_MAX_NUMBER_OF_TRAFFIC_CLASSES;
    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to get switch attribute number of traffic classes. \
                       Use default value. rv:%d", status);
        m_maxNumTC = MIRROR_SESSION_DEFAULT_NUM_TC;
    }
    else
    {
        m_maxNumTC = attr.value.u8;
    }

    m_isWarmRestoreStage = WarmStart::isWarmStart();
}

void MirrorOrch::onWarmBootEnd()
{
    SWSS_LOG_ENTER();

    m_isWarmRestoreStage = false;
}

bool MirrorOrch::bake()
{
    SWSS_LOG_ENTER();

    deque<KeyOpFieldsValuesTuple> entries;
    vector<string> keys;
    m_mirrorTable.getKeys(keys);
    for (const auto &key : keys)
    {
        vector<FieldValueTuple> tuples;
        m_mirrorTable.get(key, tuples);

        bool active = false;
        string monitor_port;
        string next_hop_ip;

        for (const auto &tuple : tuples)
        {
            if (fvField(tuple) == MIRROR_SESSION_STATUS)
            {
                active = fvValue(tuple) == MIRROR_SESSION_STATUS_ACTIVE;
            }
            else if (fvField(tuple) == MIRROR_SESSION_MONITOR_PORT)
            {
                monitor_port = fvValue(tuple);
            }
            else if (fvField(tuple) == MIRROR_SESSION_NEXT_HOP_IP)
            {
                next_hop_ip = fvValue(tuple);
            }
        }

        if (active)
        {
            SWSS_LOG_NOTICE("Found mirror session %s active before warm reboot",
                    key.c_str());

            // Recover saved active session's monitor port
            m_recoverySessionMap.emplace(
                    key, monitor_port + state_db_key_delimiter + next_hop_ip);
        }

        removeSessionState(key);
    }

    return Orch::bake();
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

bool MirrorOrch::validateDstPort(const string& dstPort)
{
    Port port;
    if (!m_portsOrch->getPort(dstPort, port))
    {
        SWSS_LOG_ERROR("Not supported port %s type %d", dstPort.c_str(), port.m_type);
        return false;
    }
    if (port.m_type != Port::PHY)
    {
        SWSS_LOG_ERROR("Not supported port %s", dstPort.c_str());
        return false;
    }
    return true;
}

bool MirrorOrch::checkPortExistsInSrcPortList(const string& port, const string& srcPortList)
{
    auto ports = tokenize(srcPortList, ',');
    if (ports.size() != 0)
    {
        for (auto alias : ports)
        {
            if(port == alias)
            {
                return true;
            }
        }
    }

    return false;
}

bool MirrorOrch::validateSrcPortList(const string& srcPortList)
{
    auto ports = tokenize(srcPortList, ',');

    if (ports.size() != 0)
    {
        for (auto alias : ports)
        {
            Port port;
            if (!gPortsOrch->getPort(alias, port))
            {
                SWSS_LOG_ERROR("Failed to locate Port/LAG %s", alias.c_str());
                return false;
            }

            if(!(port.m_type == Port::PHY || port.m_type == Port::LAG))
            {
                SWSS_LOG_ERROR("Not supported port %s", alias.c_str());
                return false;
            }

            // Check if the ports in LAG are part of source port list
            if (port.m_type == Port::LAG)
            {
                vector<Port> portv;
                int portCount = 0;
                m_portsOrch->getLagMember(port, portv);
                for (const auto &p : portv)
                {
                    if (checkPortExistsInSrcPortList(p.m_alias, srcPortList))
                    {
                        SWSS_LOG_ERROR("Port %s in LAG %s is also part of src_port config %s",
                                  p.m_alias.c_str(), port.m_alias.c_str(), srcPortList.c_str());
                        return false;
                    }
                    portCount++;
                }
                if (!portCount)
                {
                    SWSS_LOG_ERROR("Source LAG %s is empty. set mirror session to inactive",
                             port.m_alias.c_str());;
                    return false;
                }
            }
        }
    }

    return true;
}

bool MirrorOrch::isHwResourcesAvailable()
{
    uint64_t availCount = 0;

    sai_status_t status = sai_object_type_get_availability(
        gSwitchId, SAI_OBJECT_TYPE_MIRROR_SESSION, 0, nullptr, &availCount
    );
    if (status != SAI_STATUS_SUCCESS)
    {
        if ((status == SAI_STATUS_NOT_SUPPORTED) ||
            (status == SAI_STATUS_NOT_IMPLEMENTED) ||
            SAI_STATUS_IS_ATTR_NOT_SUPPORTED(status) ||
            SAI_STATUS_IS_ATTR_NOT_IMPLEMENTED(status))
        {
            SWSS_LOG_WARN("Mirror session resource availability monitoring is not supported. Skipping ...");
            return true;
        }

        return parseHandleSaiStatusFailure(handleSaiGetStatus(SAI_API_MIRROR, status));
    }

    return availCount > 0;
}

task_process_status MirrorOrch::createEntry(const string& key, const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();


    if (m_syncdMirrors.find(key) != m_syncdMirrors.end())
    {
        SWSS_LOG_NOTICE("Failed to create session %s: object already exists", key.c_str());
        return task_process_status::task_duplicated;
    }

    bool src_ip_initialized = false;
    bool dst_ip_initialized = false;

    string platform = getenv("platform") ? getenv("platform") : "";
    MirrorEntry entry(platform);

    for (auto i : data)
    {
        try {
            if (fvField(i) == MIRROR_SESSION_SRC_IP)
            {
                entry.srcIp = fvValue(i);
                src_ip_initialized = true;
            }
            else if (fvField(i) == MIRROR_SESSION_DST_IP)
            {
                entry.dstIp = fvValue(i);
                dst_ip_initialized = true;
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
                if (entry.queue >= m_maxNumTC)
                {
                    SWSS_LOG_ERROR("Failed to get valid queue %s", fvValue(i).c_str());
                    return task_process_status::task_invalid_entry;
                }
            }
            else if (fvField(i) == MIRROR_SESSION_POLICER)
            {
                if (!m_policerOrch->policerExists(fvValue(i)))
                {
                    SWSS_LOG_ERROR("Failed to get policer %s",
                            fvValue(i).c_str());
                    return task_process_status::task_need_retry;
                }

                m_policerOrch->increaseRefCount(fvValue(i));
                entry.policer = fvValue(i);
            }
            else if (fvField(i) == MIRROR_SESSION_SRC_PORT)
            {
                if (!validateSrcPortList(fvValue(i)))
                {
                    SWSS_LOG_ERROR("Failed to get valid source port list %s", fvValue(i).c_str());
                    return task_process_status::task_invalid_entry;
                }
                entry.src_port = fvValue(i);
                entry.sourceConfigured = true;
            }
            else if (fvField(i) == MIRROR_SESSION_DST_PORT)
            {
                if (!validateDstPort(fvValue(i)))
                {
                    SWSS_LOG_ERROR("Failed to get valid destination port %s", fvValue(i).c_str());
                    return task_process_status::task_invalid_entry;
                }
                entry.dst_port = fvValue(i);
            }
            else if (fvField(i) == MIRROR_SESSION_DIRECTION)
            {
                if (!(fvValue(i) == MIRROR_RX_DIRECTION || fvValue(i) == MIRROR_TX_DIRECTION
                        || fvValue(i) == MIRROR_BOTH_DIRECTION))
                {
                    SWSS_LOG_ERROR("Failed to get valid direction %s", fvValue(i).c_str());
                    return task_process_status::task_invalid_entry;
                }
                entry.direction = fvValue(i);
            }
            else if (fvField(i) == MIRROR_SESSION_TYPE)
            {
                entry.type = fvValue(i);
            }
            else
            {
                SWSS_LOG_ERROR("Failed to parse session %s configuration. Unknown attribute %s", key.c_str(), fvField(i).c_str());
                return task_process_status::task_invalid_entry;
            }
        }
        catch (const exception& e)
        {
            SWSS_LOG_ERROR("Failed to parse session %s attribute %s error: %s.", key.c_str(), fvField(i).c_str(), e.what());
            return task_process_status::task_invalid_entry;
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Failed to parse session %s attribute %s. Unknown error has been occurred", key.c_str(), fvField(i).c_str());
            return task_process_status::task_failed;
        }
    }
    // Entry validation as a whole
    if (src_ip_initialized && dst_ip_initialized && entry.srcIp.getIp().family != entry.dstIp.getIp().family)
    {
        SWSS_LOG_ERROR("Address family of source and destination IPs is different");
        return task_process_status::task_invalid_entry;
    }

    if (entry.type != MIRROR_SESSION_ERSPAN || entry.sourceConfigured)
    {
        vector<string> bindings;
        m_sampleConfigTable.getKeys(bindings);
        for (const auto& binding : bindings)
        {
            string referenced;
            if (m_sampleConfigTable.hget(binding, "mirror_session", referenced) && referenced == key)
                return task_invalid_entry;
        }
    }
    for (const auto& binding : m_sampleBindings)
    {
        if (binding.second.session == key &&
            (entry.type != MIRROR_SESSION_ERSPAN || entry.sourceConfigured))
        {
            SWSS_LOG_ERROR("Export %s is referenced by a sampled binding", key.c_str());
            return task_invalid_entry;
        }
    }

    if (!isHwResourcesAvailable())
    {
        SWSS_LOG_ERROR("Failed to create session %s: HW resources are not available", key.c_str());
        return task_process_status::task_failed;
    }

    m_syncdMirrors.emplace(key, entry);
    setSessionState(key, entry);

    if (entry.type == MIRROR_SESSION_SPAN && !entry.dst_port.empty())
    {
        auto &session1 = m_syncdMirrors.find(key)->second;
        activateSession(key, session1);
    }
    else
    {
        // Attach the destination IP to the routeOrch
        m_routeOrch->attach(this, entry.dstIp);
    }

    SWSS_LOG_NOTICE("Created mirror session %s", key.c_str());

    return task_process_status::task_success;
}

task_process_status MirrorOrch::deleteEntry(const string& name)
{
    SWSS_LOG_ENTER();

    auto sessionIter = m_syncdMirrors.find(name);
    if (sessionIter == m_syncdMirrors.end())
    {
        SWSS_LOG_ERROR("Failed to remove non-existent mirror session %s",
                name.c_str());
        return task_process_status::task_invalid_entry;
    }

    auto& session = sessionIter->second;

    for (const auto& binding : m_sampleBindings)
        if (binding.second.session == name) return task_need_retry;
    vector<string> configuredBindings;
    m_sampleConfigTable.getKeys(configuredBindings);
    for (const auto& key : configuredBindings)
    {
        string referenced;
        if (m_sampleConfigTable.hget(key, "mirror_session", referenced) && referenced == name)
            return task_need_retry;
    }

    if (session.refCount)
    {
        SWSS_LOG_WARN("Failed to remove still referenced mirror session %s, retry...",
                name.c_str());
        return task_process_status::task_need_retry;
    }

    if (session.status)
    {
        if (!deactivateSession(name, session))
        {
            SWSS_LOG_ERROR("Failed to remove mirror session %s", name.c_str());
            return task_process_status::task_failed;
        }
    }

    if (session.type != MIRROR_SESSION_SPAN)
    {
        m_routeOrch->detach(this, session.dstIp);
    }

    if (!session.policer.empty())
    {
        m_policerOrch->decreaseRefCount(session.policer);
    }

    removeSessionState(name);

    m_syncdMirrors.erase(sessionIter);

    SWSS_LOG_NOTICE("Removed mirror session %s", name.c_str());

    return task_process_status::task_success;
}

void MirrorOrch::setSessionState(const string& name, const MirrorEntry& session, const string& attr)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Update mirroring sessions %s state", name.c_str());

    vector<FieldValueTuple> fvVector;
    string value;

    if (attr.empty() || attr == MIRROR_SESSION_STATUS)
    {
        value = session.status ? MIRROR_SESSION_STATUS_ACTIVE : MIRROR_SESSION_STATUS_INACTIVE;
        fvVector.emplace_back(MIRROR_SESSION_STATUS, value);
    }

    if (attr.empty() || attr == MIRROR_SESSION_MONITOR_PORT)
    {
        Port port;
        if ((gMySwitchType == "voq") && (session.type == MIRROR_SESSION_ERSPAN))
        {
             if (!m_portsOrch->getRecircPort(port, Port::Role::Rec))
             {
                 SWSS_LOG_ERROR("Failed to get recirc port for mirror session %s", name.c_str());
                 return;
             }
	}
	else
	{
           m_portsOrch->getPort(session.neighborInfo.portId, port);
	}
        fvVector.emplace_back(MIRROR_SESSION_MONITOR_PORT, port.m_alias);
    }

    if (attr.empty() || attr == MIRROR_SESSION_DST_MAC_ADDRESS)
    {
        if ((gMySwitchType == "voq") && (session.type == MIRROR_SESSION_ERSPAN))
        {
             value = gMacAddress.to_string();
        } else
        {
             value = session.neighborInfo.mac.to_string();
        }
        fvVector.emplace_back(MIRROR_SESSION_DST_MAC_ADDRESS, value);
    }

    if (attr.empty() || attr == MIRROR_SESSION_ROUTE_PREFIX)
    {
        value = session.nexthopInfo.prefix.to_string();
        fvVector.emplace_back(MIRROR_SESSION_ROUTE_PREFIX, value);
    }

    if (attr.empty() || attr == MIRROR_SESSION_VLAN_ID)
    {
        value = to_string(session.neighborInfo.port.m_vlan_info.vlan_id);
        fvVector.emplace_back(MIRROR_SESSION_VLAN_ID, value);
    }

    if (attr.empty() || attr == MIRROR_SESSION_NEXT_HOP_IP)
    {
     value = session.nexthopInfo.nexthop.to_string();
     fvVector.emplace_back(MIRROR_SESSION_NEXT_HOP_IP, value);
    }

    m_mirrorTable.set(name, fvVector);
}

void MirrorOrch::removeSessionState(const string& name)
{
	SWSS_LOG_ENTER();

	m_mirrorTable.del(name);
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
            (session.nexthopInfo.nexthop.ip_address.isZero() ||
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

            // Recover the LAG member monitor port picked before warm reboot
            // to minimalize the data plane changes across warm reboot.
            if (m_recoverySessionMap.find(name) != m_recoverySessionMap.end())
            {
                string alias = tokenize(m_recoverySessionMap[name],
                        state_db_key_delimiter, 1)[0];
                Port member;
                m_portsOrch->getPort(alias, member);

                SWSS_LOG_NOTICE("Recover mirror session %s with LAG member port %s",
                        name.c_str(), alias.c_str());
                session.neighborInfo.portId = member.m_port_id;
            }
            else
            {
                // Get the first member of the LAG
                Port member;
                string first_member_alias = *session.neighborInfo.port.m_members.begin();
                m_portsOrch->getPort(first_member_alias, member);

                session.neighborInfo.portId = member.m_port_id;
            }

            return true;
        }
        case Port::VLAN:
        {
            SWSS_LOG_NOTICE("Get mirror session destination IP neighbor VLAN %d",
                    session.neighborInfo.port.m_vlan_info.vlan_id);

            // Recover the VLAN member monitor port picked before warm reboot
            // since the FDB entries are not yet learned on the hardware
            if (m_recoverySessionMap.find(name) != m_recoverySessionMap.end())
            {
                string alias = tokenize(m_recoverySessionMap[name],
                        state_db_key_delimiter, 1)[0];
                Port member;
                m_portsOrch->getPort(alias, member);

                SWSS_LOG_NOTICE("Recover mirror session %s with VLAN member port %s",
                        name.c_str(), alias.c_str());
                session.neighborInfo.portId = member.m_port_id;
            }
            else
            {
                Port member;
                if (!m_fdbOrch->getPort(session.neighborInfo.mac,
                            session.neighborInfo.port.m_vlan_info.vlan_id, member))
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
                }
            }

            return true;
        }
        case Port::SYSTEM:
        {
            return true;
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

bool MirrorOrch::setUnsetPortMirror(Port port, bool ingress, bool set, sai_object_id_t sessionId)
{
    // Check if the mirror direction is supported by the ASIC
    if (ingress && !m_switchOrch->isPortIngressMirrorSupported())
    {
        SWSS_LOG_ERROR("Port ingress mirror is not supported by the ASIC");
        return false;
    }
    if (!ingress && !m_switchOrch->isPortEgressMirrorSupported())
    {
        SWSS_LOG_ERROR("Port egress mirror is not supported by the ASIC");
        return false;
    }

    sai_status_t status;

    // Full mirror path (existing behavior)
    sai_attribute_t port_attr;
    port_attr.id = ingress ? SAI_PORT_ATTR_INGRESS_MIRROR_SESSION:
                           SAI_PORT_ATTR_EGRESS_MIRROR_SESSION;
    if (set)
    {
        port_attr.value.objlist.count = 1;
        port_attr.value.objlist.list = &sessionId;
    }
    else
    {
        port_attr.value.objlist.count = 0;
    }

    if (port.m_type == Port::LAG)
    {
        vector<Port> portv;
        m_portsOrch->getLagMember(port, portv);
        for (const auto &p : portv)
        {
            if (p.m_type != Port::PHY)
            {
                SWSS_LOG_ERROR("Failed to locate port %s", p.m_alias.c_str());
                return false;
            }
            status = sai_port_api->set_port_attribute(p.m_port_id, &port_attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to configure %s session on port %s: %s, status %d, sessionId %lx",
                                ingress ? "RX" : "TX", port.m_alias.c_str(),
                                p.m_alias.c_str(), status, sessionId);
                task_process_status handle_status =  handleSaiSetStatus(SAI_API_PORT, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
        }
    }
    else if (port.m_type == Port::PHY)
    {
        status = sai_port_api->set_port_attribute(port.m_port_id, &port_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to configure %s session on port %s, status %d, sessionId %lx",
                            ingress ? "RX" : "TX", port.m_alias.c_str(), status, sessionId);
            task_process_status handle_status =  handleSaiSetStatus(SAI_API_PORT, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }
    return true;
}

bool MirrorOrch::configurePortMirrorSession(const string& name, MirrorEntry& session, bool set)
{
    auto ports = tokenize(session.src_port, ',');
    if (ports.size() != 0)
    {
        for (auto alias : ports)
        {
            Port port;
            if (!gPortsOrch->getPort(alias, port))
            {
                SWSS_LOG_ERROR("Failed to locate port/LAG %s", alias.c_str());
                return false;
            }
            if (session.direction == MIRROR_RX_DIRECTION  || session.direction == MIRROR_BOTH_DIRECTION)
            {
                if (!setUnsetPortMirror(port, true, set, session.sessionId))
                {
                    SWSS_LOG_ERROR("Failed to configure mirror session %s port %s",
                        name.c_str(), port.m_alias.c_str());
                    return false;
                }
            }
            if (session.direction == MIRROR_TX_DIRECTION || session.direction == MIRROR_BOTH_DIRECTION)
            {
                if (!setUnsetPortMirror(port, false, set, session.sessionId))
                {
                    SWSS_LOG_ERROR("Failed to configure mirror session %s port %s",
                        name.c_str(), port.m_alias.c_str());
                    return false;
                }
            }
        }
    }

    return true;
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

    if (session.type == MIRROR_SESSION_SPAN)
    {
        Port dst_port;
        if (!m_portsOrch->getPort(session.dst_port, dst_port))
        {
            SWSS_LOG_ERROR("Failed to locate Port/LAG %s", session.dst_port.c_str());
            return false;
        }

        attr.id = SAI_MIRROR_SESSION_ATTR_MONITOR_PORT;
        attr.value.oid = dst_port.m_port_id;
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_TYPE;
        attr.value.oid = SAI_MIRROR_SESSION_TYPE_LOCAL;
        attrs.push_back(attr);
    }
    else
    {
        attr.id = SAI_MIRROR_SESSION_ATTR_MONITOR_PORT;
        // Set monitor port to recirc port in voq switch.
        if (gMySwitchType == "voq")
        {
            Port recirc_port;
            if (!m_portsOrch->getRecircPort(recirc_port, Port::Role::Rec))
            {
                SWSS_LOG_ERROR("Failed to get recirc port");
                return false;
            }
            attr.value.oid = recirc_port.m_port_id;
        }
        else
        {
            attr.value.oid = session.neighborInfo.portId;
        }
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
        attr.value.u8 = session.dstIp.isV4() ? MIRROR_SESSION_IP_HDR_VER_4 : MIRROR_SESSION_IP_HDR_VER_6;
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
        // Use router mac as mirror dst mac in voq switch.
        if ((gMySwitchType == "voq") && (session.type == MIRROR_SESSION_ERSPAN))
        {
             memcpy(attr.value.mac, gMacAddress.getMac(), sizeof(sai_mac_t));
        }
        else
        {
            memcpy(attr.value.mac, session.neighborInfo.mac.getMac(), sizeof(sai_mac_t));
        }
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE;
        attr.value.u16 = session.greType;
        attrs.push_back(attr);
    }

    if (!session.policer.empty())
    {
        sai_object_id_t oid = SAI_NULL_OBJECT_ID;
        if (!m_policerOrch->getPolicerOid(session.policer, oid))
        {
            SWSS_LOG_ERROR("Failed to get policer %s", session.policer.c_str());
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
        SWSS_LOG_ERROR("Failed to activate mirroring session %s", name.c_str());
        session.status = false;

        task_process_status handle_status =  handleSaiCreateStatus(SAI_API_MIRROR, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    session.status = true;

    if (!session.src_port.empty() && !session.direction.empty())
    {
        status = configurePortMirrorSession(name, session, true);
        if (status == false)
        {
            SWSS_LOG_ERROR("Failed to activate port mirror session %s", name.c_str());
            sai_mirror_api->remove_mirror_session(session.sessionId);
            session.status = false;
            return false;
        }
    }

    setSessionState(name, session);

    MirrorSessionUpdate update = { name, true };
    notify(SUBJECT_TYPE_MIRROR_SESSION_CHANGE, static_cast<void *>(&update));

    syncSampleBindings();
    SWSS_LOG_NOTICE("Activated mirror session %s", name.c_str());

    return true;
}

bool MirrorOrch::deactivateSession(const string& name, MirrorEntry& session)
{
    SWSS_LOG_ENTER();
    sai_status_t status;

    assert(session.status);

    for (auto& item : m_sampleBindings)
    {
        if (item.second.session != name) continue;
        m_sampleSuspendedExports.insert(name);
        if (!clearSampleBinding(item.first, item.second)) return false;
    }

    MirrorSessionUpdate update = { name, false };
    notify(SUBJECT_TYPE_MIRROR_SESSION_CHANGE, static_cast<void *>(&update));

    if (!session.src_port.empty() && !session.direction.empty())
    {
        status = configurePortMirrorSession(name, session, false);
        if (status == false)
        {
            SWSS_LOG_ERROR("Failed to deactivate port mirror session %s", name.c_str());
            return false;
        }
    }

    status = sai_mirror_api->remove_mirror_session(session.sessionId);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to deactivate mirroring session %s", name.c_str());
        task_process_status handle_status =  handleSaiRemoveStatus(SAI_API_MIRROR, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    session.status = false;
    session.sessionId = SAI_NULL_OBJECT_ID;
    m_sampleSuspendedExports.erase(name);

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
    if ((gMySwitchType == "voq") && (session.type == MIRROR_SESSION_ERSPAN))
    {
         memcpy(attr.value.mac, gMacAddress.getMac(), sizeof(sai_mac_t));
    } else
    {
         memcpy(attr.value.mac, session.neighborInfo.mac.getMac(), sizeof(sai_mac_t));
    }

    sai_status_t status = sai_mirror_api->set_mirror_session_attribute(session.sessionId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to update mirror session %s destination MAC to %s, rv:%d",
                name.c_str(), sai_serialize_mac(attr.value.mac).c_str(), status);
        task_process_status handle_status =  handleSaiSetStatus(SAI_API_MIRROR, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("Update mirror session %s destination MAC to %s",
            name.c_str(), sai_serialize_mac(attr.value.mac).c_str());

    setSessionState(name, session, MIRROR_SESSION_DST_MAC_ADDRESS);

    return true;
}


// Sampled source bindings are independent of the export session lifecycle.
bool MirrorOrch::hasSampleBindings()
{
    vector<string> keys;
    m_sampleConfigTable.getKeys(keys);
    if (!keys.empty() || !m_sampleBindings.empty()) return true;
    for (const auto& db : m_otherConfigDbs)
    {
        Table table(db.get(), PORT_MIRROR_SAMPLE_TABLE);
        keys.clear();
        table.getKeys(keys);
        if (!keys.empty()) return true;
    }
    return false;
}

bool MirrorOrch::hasSflowGlobal()
{
    vector<string> keys;
    m_sflowConfigTable.getKeys(keys);
    if (std::find(keys.begin(), keys.end(), "global") != keys.end()) return true;
    for (const auto& db : m_otherConfigDbs)
    {
        Table table(db.get(), "SFLOW");
        keys.clear();
        table.getKeys(keys);
        if (std::find(keys.begin(), keys.end(), "global") != keys.end()) return true;
    }
    return false;
}

task_process_status MirrorOrch::createSampleBinding(const string& key, const vector<FieldValueTuple>& fields)
{
    if (m_sampleBindings.count(key))
    {
        if (m_sampleBindings.at(key).deleting) return task_need_retry;
        SWSS_LOG_NOTICE("Failed to create session %s: object already exists", key.c_str());
        return task_duplicated;
    }
    auto separator = key.find('|');
    if (separator == string::npos || separator == 0 || key.find('|', separator + 1) != string::npos)
        return task_invalid_entry;
    SampleBinding binding;
    binding.port = key.substr(0, separator);
    auto direction = key.substr(separator + 1);
    if (direction != "rx" && direction != "tx")
        return task_invalid_entry;
    binding.ingress = direction == "rx";
    bool ratePresent = false;
    try
    {
        for (const auto& field : fields)
        {
            const auto& name = fvField(field);
            const auto& value = fvValue(field);
            if (name == "mirror_session") binding.session = value;
            else if (name == "admin_state")
            {
                if (value != "up" && value != "down") return task_invalid_entry;
                binding.enabled = value == "up";
            }
            else if (name == "sample_rate")
            {
                binding.rate = to_uint<uint32_t>(value);
                ratePresent = true;
                if (binding.rate < 2) return task_invalid_entry;
            }
            else if (name == "truncate_size")
            {
                binding.truncate = to_uint<uint32_t>(value);
                if (binding.truncate < 64 || binding.truncate > 9216) return task_invalid_entry;
            }
            else return task_invalid_entry;
        }
    }
    catch (const exception& error)
    {
        SWSS_LOG_ERROR("Invalid sampled binding %s: %s", key.c_str(), error.what());
        return task_invalid_entry;
    }
    if (!ratePresent || binding.session.empty()) return task_invalid_entry;
    if (hasSflowGlobal())
    {
        SWSS_LOG_ERROR("SFLOW global conflicts with sampled binding %s", key.c_str());
        return task_need_retry;
    }
    auto session = m_syncdMirrors.find(binding.session);
    if (session != m_syncdMirrors.end() &&
        (session->second.type != MIRROR_SESSION_ERSPAN || session->second.sourceConfigured || !session->second.src_port.empty()))
        return task_invalid_entry;
    Port port;
    if (m_portsOrch->getPort(binding.port, port) && port.m_type != Port::PHY && port.m_type != Port::LAG)
        return task_invalid_entry;
    m_sampleBindings.emplace(key, std::move(binding));
    syncSampleBindings();
    return task_success;
}

bool MirrorOrch::createSamplePacket(const string& name, SampleBinding& binding)
{
    if (binding.samplepacketId != SAI_NULL_OBJECT_ID) return true;
    vector<sai_attribute_t> attrs;
    sai_attribute_t attr{};
    attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;
    attr.value.u32 = binding.rate;
    attrs.push_back(attr);
    attr.id = SAI_SAMPLEPACKET_ATTR_TYPE;
    attr.value.s32 = SAI_SAMPLEPACKET_TYPE_MIRROR_SESSION;
    attrs.push_back(attr);
    attr.id = SAI_SAMPLEPACKET_ATTR_MODE;
    attr.value.s32 = SAI_SAMPLEPACKET_MODE_EXCLUSIVE;
    attrs.push_back(attr);
    if (binding.truncate)
    {
        attr.id = SAI_SAMPLEPACKET_ATTR_TRUNCATE_ENABLE;
        attr.value.booldata = true;
        attrs.push_back(attr);
        attr.id = SAI_SAMPLEPACKET_ATTR_TRUNCATE_SIZE;
        attr.value.u32 = binding.truncate;
        attrs.push_back(attr);
    }
    sai_object_id_t oid = SAI_NULL_OBJECT_ID;
    auto status = sai_samplepacket_api->create_samplepacket(&oid, gSwitchId,
                                                           static_cast<uint32_t>(attrs.size()), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create SamplePacket for binding %s: %d", name.c_str(), status);
        return false;
    }
    binding.samplepacketId = oid;
    return true;
}

bool MirrorOrch::clearSampleAttachment(const string& key, SampleBinding& binding, sai_object_id_t port)
{
    auto ownerKey = std::make_pair(port, binding.ingress);
    auto owner = m_sampleOwners.find(ownerKey);
    if (owner == m_sampleOwners.end() || owner->second != key)
    {
        SWSS_LOG_ERROR("Lost attachment ownership for sampled binding %s", key.c_str());
        return false;
    }
    auto& attachment = binding.attachments.at(port);
    sai_attribute_t attr{};
    // Track each successful operation so retries never free a still-attached object.
    if (attachment.mirrorSet)
    {
        attr.id = binding.ingress ? SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION :
                                   SAI_PORT_ATTR_EGRESS_SAMPLE_MIRROR_SESSION;
        attr.value.objlist.count = 0;
        attr.value.objlist.list = nullptr;
        if (sai_port_api->set_port_attribute(port, &attr) != SAI_STATUS_SUCCESS) return false;
        attachment.mirrorSet = false;
    }
    if (attachment.sampleSet)
    {
        attr.id = binding.ingress ? SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE :
                                   SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
        attr.value.oid = SAI_NULL_OBJECT_ID;
        if (sai_port_api->set_port_attribute(port, &attr) != SAI_STATUS_SUCCESS) return false;
        attachment.sampleSet = false;
    }
    m_portsOrch->decreasePortRefCount(attachment.alias);
    m_sampleOwners.erase(owner);
    binding.attachments.erase(port);
    return true;
}

bool MirrorOrch::clearSampleBinding(const string& key, SampleBinding& binding)
{
    vector<sai_object_id_t> ports;
    for (const auto& attachment : binding.attachments) ports.push_back(attachment.first);
    bool cleared = true;
    for (auto port : ports)
        if (!clearSampleAttachment(key, binding, port)) cleared = false;
    if (!cleared) return false;
    if (binding.samplepacketId != SAI_NULL_OBJECT_ID)
    {
        auto status = sai_samplepacket_api->remove_samplepacket(binding.samplepacketId);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove SamplePacket for binding %s: %d", key.c_str(), status);
            return false;
        }
        binding.samplepacketId = SAI_NULL_OBJECT_ID;
    }
    if (binding.exportReferenced)
    {
        decreaseRefCount(binding.session);
        binding.exportReferenced = false;
    }
    binding.cleanupPending = false;
    return true;
}

void MirrorOrch::syncSampleBindings()
{
    const bool sflowConfigured = hasSflowGlobal();
    for (auto it = m_sampleBindings.begin(); it != m_sampleBindings.end();)
    {
        const auto& key = it->first;
        auto& binding = it->second;
        auto session = m_syncdMirrors.find(binding.session);
        bool ready = binding.enabled && !binding.deleting && !sflowConfigured &&
                     !m_sampleSuspendedExports.count(binding.session) &&
                     session != m_syncdMirrors.end() && session->second.status &&
                     session->second.type == MIRROR_SESSION_ERSPAN && !session->second.sourceConfigured &&
                     session->second.src_port.empty();
        if (binding.cleanupPending || !ready)
        {
            if (!clearSampleBinding(key, binding))
            {
                binding.cleanupPending = true;
                ++it;
                continue;
            }
            if (binding.deleting)
            {
                it = m_sampleBindings.erase(it);
                continue;
            }
            if (!ready) { ++it; continue; }
        }
        if ((binding.ingress && !m_switchOrch->isPortIngressSampleMirrorSupported()) ||
            (!binding.ingress && !m_switchOrch->isPortEgressSampleMirrorSupported()) ||
            (binding.truncate && !m_switchOrch->isSamplepacketTruncationSupported()))
        {
            if (!binding.unsupportedLogged)
                SWSS_LOG_ERROR("Unsupported sampling capability for binding %s", key.c_str());
            binding.unsupportedLogged = true;
            ++it;
            continue;
        }
        map<sai_object_id_t, string> desired;
        Port source;
        if (m_portsOrch->getPort(binding.port, source))
        {
            vector<Port> ports;
            if (source.m_type == Port::LAG) m_portsOrch->getLagMember(source, ports);
            else if (source.m_type == Port::PHY) ports.push_back(source);
            for (const auto& port : ports)
                if (port.m_type == Port::PHY) desired.emplace(port.m_port_id, port.m_alias);
        }
        vector<sai_object_id_t> obsolete;
        for (const auto& attachment : binding.attachments)
            if (!desired.count(attachment.first)) obsolete.push_back(attachment.first);
        bool clean = true;
        for (auto port : obsolete)
            if (!clearSampleAttachment(key, binding, port)) clean = false;
        if (!clean) { ++it; continue; }
        if (desired.empty())
        {
            if (!clearSampleBinding(key, binding)) binding.cleanupPending = true;
            ++it;
            continue;
        }
        // Reserve physical-port/direction ownership before programming anything.
        bool available = true;
        for (const auto& port : desired)
        {
            auto owner = m_sampleOwners.find({port.first, binding.ingress});
            if (owner != m_sampleOwners.end() && owner->second != key) { available = false; break; }
            if (binding.attachments.count(port.first)) continue;
            if (!m_isWarmRestoreStage)
            {
                sai_attribute_t attr{};
                attr.id = binding.ingress ? SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE :
                                           SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
                auto status = sai_port_api->get_port_attribute(port.first, 1, &attr);
                if (status != SAI_STATUS_SUCCESS ||
                    attr.value.oid != SAI_NULL_OBJECT_ID)
                {
                    available = false;
                    break;
                }
            }
        }
        if (!available) { ++it; continue; }
        if (!createSamplePacket(key, binding)) { ++it; continue; }
        if (!binding.exportReferenced)
        {
            increaseRefCount(binding.session);
            binding.exportReferenced = true;
        }
        bool success = true;
        for (const auto& port : desired)
        {
            if (binding.attachments.count(port.first)) continue;
            m_sampleOwners[{port.first, binding.ingress}] = key;
            auto& attachment = binding.attachments[port.first];
            attachment.alias = port.second;
            m_portsOrch->increasePortRefCount(port.second);
            sai_attribute_t attr{};
            attr.id = binding.ingress ? SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE :
                                       SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE;
            attr.value.oid = binding.samplepacketId;
            if (sai_port_api->set_port_attribute(port.first, &attr) != SAI_STATUS_SUCCESS)
            { success = false; break; }
            attachment.sampleSet = true;
            attr.id = binding.ingress ? SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION :
                                       SAI_PORT_ATTR_EGRESS_SAMPLE_MIRROR_SESSION;
            attr.value.objlist.count = 1;
            attr.value.objlist.list = &session->second.sessionId;
            if (sai_port_api->set_port_attribute(port.first, &attr) != SAI_STATUS_SUCCESS)
            { success = false; break; }
            attachment.mirrorSet = true;
        }
        if (!success)
        {
            SWSS_LOG_ERROR("Failed to attach sampled binding %s; rolling back", key.c_str());
            binding.cleanupPending = true;
            clearSampleBinding(key, binding);
        }
        ++it;
    }
}

void MirrorOrch::doTask()
{
    Orch::doTask();
    if (!m_portsOrch->allPortsReady()) return;
    // Retry export deactivation if a previous member detach or SAI removal failed.
    auto pending = m_sampleSuspendedExports;
    for (const auto& name : pending)
    {
        auto session = m_syncdMirrors.find(name);
        if (session != m_syncdMirrors.end() && session->second.status && deactivateSession(name, session->second))
            updateSession(name, session->second);
    }
    syncSampleBindings();
}

bool MirrorOrch::updateSessionDstPort(const string& name, MirrorEntry& session)
{
    SWSS_LOG_ENTER();

    assert(session.sessionId != SAI_NULL_OBJECT_ID);

    Port port;
    m_portsOrch->getPort(session.neighborInfo.portId, port);

    sai_attribute_t attr;
    attr.id = SAI_MIRROR_SESSION_ATTR_MONITOR_PORT;
    // Set monitor port to recirc port in voq switch.
    if ((gMySwitchType == "voq") && (session.type == MIRROR_SESSION_ERSPAN))
    {
         if (!m_portsOrch->getRecircPort(port, Port::Role::Rec))
         {
             SWSS_LOG_ERROR("Failed to get recirc port for mirror session %s", name.c_str());
             return false;
         }
         attr.value.oid = port.m_port_id;
    }
    else
    {
         attr.value.oid = session.neighborInfo.portId;
    }

    sai_status_t status = sai_mirror_api->
        set_mirror_session_attribute(session.sessionId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to update mirror session %s monitor port to %s, rv:%d",
                name.c_str(), port.m_alias.c_str(), status);
        task_process_status handle_status =  handleSaiSetStatus(SAI_API_MIRROR, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
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
            task_process_status handle_status =  handleSaiSetStatus(SAI_API_MIRROR, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }
    }

    SWSS_LOG_NOTICE("Update mirror session %s VLAN to %s",
            name.c_str(), session.neighborInfo.port.m_alias.c_str());

    setSessionState(name, session, MIRROR_SESSION_VLAN_ID);

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
        if (update.nexthopGroup != NextHopGroupKey() &&
                update.nexthopGroup.getNextHops().count(session.nexthopInfo.nexthop))

        {
            continue;
        }

        SWSS_LOG_NOTICE("Updating mirror session %s with route %s",
                name.c_str(), update.prefix.to_string().c_str());

        if (update.nexthopGroup != NextHopGroupKey())
        {
            SWSS_LOG_NOTICE("    next hop IPs: %s", update.nexthopGroup.to_string().c_str());

            // Recover the session based on the state database information
            if (m_recoverySessionMap.find(name) != m_recoverySessionMap.end())
            {
                NextHopKey nexthop = NextHopKey(tokenize(m_recoverySessionMap[name],
                            state_db_key_delimiter, 1)[1]);

                // Check if recovered next hop IP is within the update's next hop IPs
                if (update.nexthopGroup.getNextHops().count(nexthop))
                {
                    SWSS_LOG_NOTICE("Recover mirror session %s with next hop %s",
                            name.c_str(), nexthop.to_string().c_str());
                    session.nexthopInfo.nexthop = nexthop;
                }
                else
                {
                    // Correct the next hop IP
                    SWSS_LOG_NOTICE("Correct mirror session %s next hop from %s to %s",
                            name.c_str(), session.nexthopInfo.nexthop.to_string().c_str(),
                    nexthop.to_string().c_str());
                    session.nexthopInfo.nexthop = *update.nexthopGroup.getNextHops().begin();
                }
            }
            else
            {
                // Pick the first one from the next hop group
                session.nexthopInfo.nexthop = *update.nexthopGroup.getNextHops().begin();
            }
        }
        else
        {
            string alias = "";
            session.nexthopInfo.nexthop = session.dstIp.isV4() ? NextHopKey("0.0.0.0", alias) : NextHopKey("::", alias);
        }

        // Update State DB Nexthop
        setSessionState(name, session, MIRROR_SESSION_NEXT_HOP_IP);

        SWSS_LOG_NOTICE("Updated mirror session state db %s nexthop to %s",
                        name.c_str(), session.nexthopInfo.nexthop.to_string().c_str());


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
                session.nexthopInfo.nexthop.ip_address != update.entry.ip_address)
        {
            continue;
        }

        SWSS_LOG_NOTICE("Updating mirror session %s with neighbor %s",
                name.c_str(), update.entry.alias.c_str());

        updateSession(name, session);
    }
}

// The function is called when SUBJECT_TYPE_FDB_CHANGE is received.
// This function will handle the case when new FDB entry is learned/added in the VLAN,
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
        // 3) the destination MAC matches the FDB notification MAC
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
        // Remove the monitor port
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

    syncSampleBindings();

    for (auto it = m_syncdMirrors.begin(); it != m_syncdMirrors.end(); it++)
    {
        const auto& name = it->first;
        auto& session = it->second;

        // Check the following conditions:
        // 1) Session is active
        // 2) LAG is part of mirror session source ports.
        // 3) Member port is not part of session source ports.
        // if the above condition matches then set/unset mirror configuration to new member port.
        if (session.status &&
            !session.src_port.empty() &&
            session.src_port.find(update.lag.m_alias.c_str()) != std::string::npos &&
            !checkPortExistsInSrcPortList(update.member.m_alias, session.src_port))
        {
            if (session.direction == MIRROR_RX_DIRECTION  || session.direction == MIRROR_BOTH_DIRECTION)
            {
                setUnsetPortMirror(update.member, true, update.add, session.sessionId);
            }
            if (session.direction == MIRROR_TX_DIRECTION || session.direction == MIRROR_BOTH_DIRECTION)
            {
                setUnsetPortMirror(update.member, false, update.add, session.sessionId);
            }
        }

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
                if (session.status)
                {
                    deactivateSession(name, session);
                }
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

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    if (consumer.getTableName() == PORT_MIRROR_SAMPLE_TABLE)
    {
        auto it = consumer.m_toSync.begin();
        while (it != consumer.m_toSync.end())
        {
            const auto& key = kfvKey(it->second);
            const auto& op = kfvOp(it->second);
            auto result = task_invalid_entry;
            if (op == SET_COMMAND)
                result = createSampleBinding(key, kfvFieldsValues(it->second));
            else if (op == DEL_COMMAND)
            {
                auto binding = m_sampleBindings.find(key);
                if (binding != m_sampleBindings.end()) binding->second.deleting = true;
                syncSampleBindings();
                result = m_sampleBindings.count(key) ? task_need_retry : task_success;
            }
            if (result == task_need_retry) ++it;
            else it = consumer.m_toSync.erase(it);
        }
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        task_process_status task_status = task_process_status::task_failed;

        if (op == SET_COMMAND)
        {
            task_status = createEntry(key, kfvFieldsValues(t));
        }
        else if (op == DEL_COMMAND)
        {
            task_status = deleteEntry(key);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }

        // Specifically retry the task when asked
        if (task_status == task_process_status::task_need_retry)
        {
            it++;
        }
        else
        {
            consumer.m_toSync.erase(it++);
        }
    }

    // Clear any recovery state that might be leftover from warm reboot
    m_recoverySessionMap.clear();
}
