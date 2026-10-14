#include "evpnmhorch.h"

#include <algorithm>

#include "fdborch.h"
#include "l2nhgorch.h"
#include "portsorch.h"

extern PortsOrch *gPortsOrch;
extern FdbOrch *gFdbOrch;
extern L2NhgOrch *gL2NhgOrch;
extern sai_object_id_t gSwitchId;

extern sai_vlan_api_t *sai_vlan_api;
extern sai_bridge_api_t *sai_bridge_api;

#define EVPN_ES_BACKUP_NHG_TABLE "EVPN_ES_BACKUP_NHG_TABLE"
/*
 * An ES port that comes back up stays switched over this long: its LAG member
 * starts distributing after the LAG goes oper up, and the host starts collecting
 * on the link later still, so switching back at oper up loses traffic. Traffic
 * keeps flowing through the peers meanwhile.
 */
#define ES_BACKUP_SWITCHBACK_HOLD_SEC 1

#define VLAN_PREFIX "Vlan"

EvpnMhOrch::EvpnMhOrch(vector<TableConnector> &connectors) : Orch(connectors)
{
    SWSS_LOG_ENTER();

    /* Port oper state and bridge port changes drive the ES backup switchover. */
    if (gPortsOrch)
    {
        gPortsOrch->attach(this);
    }
    m_esBackupSwitchbackTimer = new SelectableTimer(timespec { .tv_sec = ES_BACKUP_SWITCHBACK_HOLD_SEC,
                                                               .tv_nsec = 0 });
    Orch::addExecutor(new ExecutableTimer(m_esBackupSwitchbackTimer, this, "ES_BACKUP_SWITCHBACK"));
}

EvpnMhOrch::~EvpnMhOrch()
{
    SWSS_LOG_ENTER();

    if (gPortsOrch)
    {
        gPortsOrch->detach(this);
    }
}

EsCacheEntry *EvpnMhOrch::getEsCache(const std::string &key)
{
    auto entry_it = m_esDataMap.find(key);
    return (entry_it != m_esDataMap.end()) ? entry_it->second.get() : nullptr;
}

static std::string getPortFromEsKey(const std::string &key)
{
    auto pos = key.find(':');
    return (pos != std::string::npos) ? key.substr(pos + 1) : "Unknown";
}

static std::string getVlanFromEsKey(const std::string &key)
{
    auto pos = key.find(':');
    return (pos != std::string::npos) ? key.substr(0, pos) : "Unknown";
}

bool EvpnMhOrch::updateEsCache(string &key, KeyOpFieldsValuesTuple &t)
{
    bool is_df = false;
    struct EsCacheEntry *existing_entry = nullptr;

    // Note: KeyOpFieldsValuesTuple is the standard swss framework type for receiving
    // data from Redis tables. Although not optimal for lookups, it's part of the API contract.
    for (const auto &i : kfvFieldsValues(t))
    {
        if (fvField(i) == "df")
        {
            is_df = (fvValue(i) == "true");
            break;  // Found the field we need, no need to continue
        }
    }

    existing_entry = getEsCache(key);
    if (existing_entry)
    {
        existing_entry->is_df = is_df;
    }
    else
    {
        m_esDataMap[key] = std::make_unique<EsCacheEntry>(is_df);
        existing_entry = m_esDataMap[key].get();
    }

    /*
     * DESIGN NOTE: The YANG schema takes only the interface name, but the implementation
     * uses a composite key format "VlanX:InterfaceName" for cache lookups. This design
     * has performance implications:
     *
     * 1. Each VLAN in the bridge triggers an attribute update against the main port
     * 2. Current flat map structure requires string parsing on every lookup
     *
     * TODO: Consider alternative data structures for better performance:
     * - Nested map: map<vlan_id, map<port_name, EsCacheEntry*>>
     * - Multimap indexed by port for O(1) port-based lookups
     * - Parent/child cache relationship to reduce redundant updates
     */
    std::string port_name = getPortFromEsKey(key);
    std::string vlan_id = getVlanFromEsKey(key);
    Port port;
    sai_object_id_t vlan_member_id;

    SWSS_LOG_NOTICE("updateEsCache: SET oper: %s, vlan: %s, port_name: %s, is_df: %d", key.c_str(), vlan_id.c_str(), port_name.c_str(), existing_entry->is_df);

    if (!gPortsOrch->getPort(vlan_id, port))
    {
        SWSS_LOG_ERROR("updateEsCache: interface: %s, Vlan is not not yet created, returning", key.c_str());
        return false;
    }

    if (gPortsOrch->getVlanMember(port_name, port, vlan_member_id))
    {
        /*
         * TODO: Verify the correct SAI attribute to use.
         * Current: SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP (workaround)
         * HLD suggests: SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_NEXT_HOP_GROUP_ID
         * Need to confirm with SAI implementation and HLD requirements.
         *
         * FIXME: Verify logic correctness - attribute name suggests BUM traffic should
         * be DROPPED on non-DF. If true, this should be: !existing_entry->is_df
         * (DF=false → DROP=true, DF=true → DROP=false)
         */
        sai_attribute_t attr;
        attr.id = SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP;
        attr.value.booldata = existing_entry->is_df;

        auto status = sai_vlan_api->set_vlan_member_attribute(vlan_member_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("updateEsCache: Failed to set VLAN member attribute for %s, SAI status: %d. "
                          "BUM traffic forwarding state may be incorrect. Will retry.", key.c_str(), status);
            return false;
        }
    }
    else
    {
        SWSS_LOG_ERROR("updateEsCache: interface %s vlan_member_id doesnt exit", key.c_str());
        return false;
    }
    return true;
}

bool EvpnMhOrch::deleteEsCache(string &key)
{
    EsCacheEntry *entry = getEsCache(key);

    if (!entry)
    {
        SWSS_LOG_WARN("deleteEsCache: Entry not found for key: %s", key.c_str());
        return true;  // Nothing to delete, consider it successful
    }

    SWSS_LOG_NOTICE("deleteEsCache: DEL oper: intf: %s, is_df: %d", key.c_str(), entry->is_df);
    std::string port_name = getPortFromEsKey(key);
    std::string vlan_id = getVlanFromEsKey(key);
    Port port;
    sai_object_id_t vlan_member_id;

    if (!gPortsOrch->getPort(vlan_id, port))
    {
        SWSS_LOG_ERROR("deleteEsCache: interface: %s, Vlan is not not yet created, returning", key.c_str());
        return false;
    }
    if (gPortsOrch->getVlanMember(port_name, port, vlan_member_id))
    {
        /*
         * TODO: Verify the correct SAI attribute to use.
         * Current: SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP (workaround)
         * HLD suggests: SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_NEXT_HOP_GROUP_ID
         * Need to confirm with SAI implementation and HLD requirements.
         *
         * Note: Setting to false on delete to restore default forwarding behavior.
         */
        sai_attribute_t attr;
        attr.id = SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP;
        attr.value.booldata = false;

        auto status = sai_vlan_api->set_vlan_member_attribute(vlan_member_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("deleteEsCache: Failed to reset VLAN member attribute for %s, SAI status: %d. "
                          "BUM traffic forwarding state may be incorrect. Will retry.", key.c_str(), status);
            return false;
        }
    }
    else
    {
        SWSS_LOG_ERROR("deleteEsCache: interface %s vlan_member_id doesnt exit", key.c_str());
        return false;
    }
    m_esDataMap.erase(key);
    return true;
}

bool EvpnMhOrch::vlanMembersApplyNonDF(string port_name)
{
    vlan_members_t vlan_members;
    Port port;
    if (!gPortsOrch->getPort(port_name, port))
    {
        SWSS_LOG_ERROR("vlanMembersApplyNonDF: getPort() fails for port_name:%s", port_name.c_str());
        return false;
    }
    gPortsOrch->getPortVlanMembers(port, vlan_members);
    for (const auto &member : vlan_members)
    {
        auto vlan_id = member.first;
        auto vlan_mem_entry = member.second;
        /*
         * TODO: Verify the correct SAI attribute to use.
         * Current: SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP (workaround)
         * HLD suggests: SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_NEXT_HOP_GROUP_ID
         * Need to confirm with SAI implementation and HLD requirements.
         *
         * FIXME: Verify logic correctness - attribute name suggests BUM traffic should
         * be DROPPED on non-DF. If true, this should be: !isInterfaceDF(...)
         * (DF=false → DROP=true, DF=true → DROP=false)
         */
        sai_attribute_t attr;
        attr.id = SAI_VLAN_MEMBER_ATTR_TUNNEL_TERM_BUM_TX_DROP;
        attr.value.booldata = isInterfaceDF(port_name, vlan_id);
        SWSS_LOG_NOTICE("vlanMembersApplyNonDF: set Non-DF for port: %s, vlan: %d", port_name.c_str(), vlan_id);

        auto status = sai_vlan_api->set_vlan_member_attribute(vlan_mem_entry.vlan_member_id, &attr);
        if (status == SAI_STATUS_NOT_SUPPORTED || status == SAI_STATUS_NOT_IMPLEMENTED)
        {
            SWSS_LOG_WARN("vlanMembersApplyNonDF: SAI attribute not supported for port %s vlan %d, "
                          "Non-DF BUM suppression unavailable on this platform",
                          port_name.c_str(), vlan_id);
            continue;
        }
        else if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("vlanMembersApplyNonDF: failed to set Non-DF for port %s vlan %d (status %d)",
                           port_name.c_str(), vlan_id, status);
            return false;
        }
    }
    return true;
}
void EvpnMhOrch::doEvpnEsIntfTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);

        SWSS_LOG_NOTICE("doEvpnEsIntfTask: %s oper: ESI intf: %s", op.c_str(), key.c_str());

        // Update ES intent state immediately - this is a control-plane fact
        // independent of whether the port exists in SAI yet.
        if (op == SET_COMMAND)
        {
            m_esIntfMap[key] = true;
        }
        else if (op == DEL_COMMAND)
        {
            m_esIntfMap.erase(key);
            /* The port is no longer an ES: a held backup group DEL completes. */
            auto e = m_esBackup.find(key);
            if (e != m_esBackup.end() && e->second.deleted && !finishEsBackupDelete(key))
            {
                m_esBackupPending.insert(key);
            }
        }

        if (!vlanMembersApplyNonDF(key))
        {
            // SAI operation failed (e.g. port not yet created), leave in m_toSync for retry.
            // m_esIntfMap is already updated above so callers see correct ES state.
            ++it;
        }
        else
        {
            it = consumer.m_toSync.erase(it);
        }
    }
}

void EvpnMhOrch::doEvpnEsDfTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);

        bool success = false;
        if (op == SET_COMMAND)
        {
            success = updateEsCache(key, t);
        }
        else if (op == DEL_COMMAND)
        {
            success = deleteEsCache(key);
        }

        if (!success)
        {
            // SAI operation failed, leave in m_toSync for retry
            ++it;
        }
        else
        {
            it = consumer.m_toSync.erase(it);
        }
    }
}

void EvpnMhOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();
    if (table_name == "EVPN_DF_TABLE")
    {
        doEvpnEsDfTask(consumer);
    }
    else if (table_name == "EVPN_ETHERNET_SEGMENT")
    {
        doEvpnEsIntfTask(consumer);
    }
    else if (table_name == EVPN_ES_BACKUP_NHG_TABLE)
    {
        doEvpnEsBackupNhgTask(consumer);
    }
}

void EvpnMhOrch::doTask()
{
    Orch::doTask();

    /* Entries waiting for their backup group (no notification comes when L2NhgOrch
     * activates it) or for the port's bridge port. */
    for (auto it = m_esBackupPending.begin(); it != m_esBackupPending.end();)
    {
        if (applyEsBackup(*it))
        {
            it = m_esBackupPending.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

/*
 * The protection attributes are set on live bridge ports, so an unsupported one
 * would be refused by syncd's applyView after a warm restart, where no refusal
 * reaches orchagent. Use them only when the SAI says it implements them.
 */
bool EvpnMhOrch::isEsBackupSupported()
{
    if (m_esBackupCapChecked)
    {
        return m_esBackupSupported;
    }
    m_esBackupCapChecked = true;

    string platform = getenv("platform") ? getenv("platform") : "";
    if (platform == VS_PLATFORM_SUBSTRING)
    {
        m_esBackupSupported = true;
    }
    else
    {
        sai_attr_capability_t prot = {}, swo = {};
        sai_status_t s1 = sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_BRIDGE_PORT,
            SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID, &prot);
        sai_status_t s2 = sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_BRIDGE_PORT,
            SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER, &swo);
        m_esBackupSupported = (s1 == SAI_STATUS_SUCCESS) && (s2 == SAI_STATUS_SUCCESS) &&
                              prot.set_implemented && swo.set_implemented;
    }
    SWSS_LOG_NOTICE("ES backup next hop group (bridge port protection) is %s",
                    m_esBackupSupported ? "supported" :
                    "not supported: MACs on a down ES port are flushed and flood");
    return m_esBackupSupported;
}

void EvpnMhOrch::doEvpnEsBackupNhgTask(Consumer &consumer)
{
    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string port_name = kfvKey(t);
        string op = kfvOp(t);

        if (!isEsBackupSupported())
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            string nhg_id;
            for (const auto &fv : kfvFieldsValues(t))
            {
                if (fvField(fv) == "nexthop_group")
                {
                    nhg_id = fvValue(fv);
                }
            }
            if (nhg_id.empty())
            {
                SWSS_LOG_ERROR("ES %s: backup next hop group entry without a nexthop_group, ignored",
                               port_name.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }
            auto &entry = m_esBackup[port_name];
            if (entry.deleted)
            {
                SWSS_LOG_NOTICE("ES %s backup next hop group set again while its removal was held",
                                port_name.c_str());
                entry.deleted = false;
            }
            if (entry.nhg_id != nhg_id)
            {
                SWSS_LOG_NOTICE("ES %s backup next hop group %s -> %s", port_name.c_str(),
                                entry.nhg_id.c_str(), nhg_id.c_str());
                entry.nhg_id = nhg_id;
            }
            if (applyEsBackup(port_name))
            {
                m_esBackupPending.erase(port_name);
            }
            else
            {
                m_esBackupPending.insert(port_name);
            }
            it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            auto e = m_esBackup.find(port_name);
            if (e != m_esBackup.end() && holdEsBackupDelete(e->second, port_name))
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }
            if (e != m_esBackup.end() && !detachEsBackup(e->second, port_name))
            {
                ++it;
                continue;
            }
            SWSS_LOG_NOTICE("ES %s backup next hop group removed", port_name.c_str());
            m_esBackup.erase(port_name);
            m_esBackupPending.erase(port_name);
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            it = consumer.m_toSync.erase(it);
        }
    }
}

bool EvpnMhOrch::setEsBackupAttr(EsBackupEntry &entry, const std::string &port_name,
                                 sai_attr_id_t id, bool value, sai_object_id_t oid)
{
    sai_attribute_t attr;
    attr.id = id;
    if (id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER)
    {
        attr.value.booldata = value;
    }
    else
    {
        attr.value.oid = oid;
    }
    sai_status_t status = sai_bridge_api->set_bridge_port_attribute(entry.bridge_port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("ES %s: failed to set %s on bridge port 0x%" PRIx64 ", rv:%d",
                       port_name.c_str(),
                       id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER ?
                           "switchover" : "protection next hop group",
                       entry.bridge_port_id, status);
        return false;
    }
    if (id == SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER)
    {
        entry.switchover = value;
        SWSS_LOG_NOTICE("ES %s: %s backup next hop group %s", port_name.c_str(),
                        value ? "switched over to" : "switched back from", entry.nhg_id.c_str());
    }
    else
    {
        entry.nhg_oid = oid;
        SWSS_LOG_NOTICE("ES %s: protection next hop group 0x%" PRIx64 " (%s)", port_name.c_str(),
                        oid, entry.nhg_id.c_str());
    }
    return true;
}

/* Returns false while the entry still has to be retried. */
bool EvpnMhOrch::applyEsBackup(const std::string &port_name)
{
    auto e = m_esBackup.find(port_name);
    if (e == m_esBackup.end())
    {
        return true;
    }
    auto &entry = e->second;
    if (entry.deleted)
    {
        return finishEsBackupDelete(port_name);
    }

    Port port;
    if (!gPortsOrch->getPort(port_name, port) || port.m_bridge_port_id == SAI_NULL_OBJECT_ID)
    {
        /* Applied from the bridge port add notification. */
        entry.bridge_port_id = SAI_NULL_OBJECT_ID;
        entry.nhg_oid = SAI_NULL_OBJECT_ID;
        entry.switchover = false;
        return true;
    }
    if (entry.bridge_port_id != port.m_bridge_port_id)
    {
        /* A new bridge port starts with the attributes at their defaults. */
        entry.bridge_port_id = port.m_bridge_port_id;
        entry.nhg_oid = SAI_NULL_OBJECT_ID;
        entry.switchover = false;
    }

    sai_object_id_t want = gL2NhgOrch ? gL2NhgOrch->getL2NhgOid(entry.nhg_id) : SAI_NULL_OBJECT_ID;
    if (want == SAI_NULL_OBJECT_ID)
    {
        /* The attached group keeps carrying a switched over port until the new one is active. */
        if (entry.nhg_oid != SAI_NULL_OBJECT_ID && !isEsBackupInUse(entry, port_name, port))
        {
            detachEsBackup(entry, port_name);
        }
        SWSS_LOG_INFO("ES %s: backup next hop group %s not active yet", port_name.c_str(),
                      entry.nhg_id.c_str());
        return false;
    }
    if (entry.nhg_oid != want)
    {
        /* Replaced in place: a switched over port's traffic follows the group it now has. */
        if (!setEsBackupAttr(entry, port_name, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID,
                             false, want))
        {
            return false;
        }
    }
    bool down = isEsPortDown(port);
    if (!down && m_esBackupSwitchback.count(port_name))
    {
        /* Switched back by the hold timer. */
        return true;
    }
    if (entry.switchover != down &&
        !setEsBackupAttr(entry, port_name, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER, down,
                         SAI_NULL_OBJECT_ID))
    {
        return false;
    }
    return true;
}

bool EvpnMhOrch::detachEsBackup(EsBackupEntry &entry, const std::string &port_name)
{
    m_esBackupSwitchback.erase(port_name);
    if (entry.bridge_port_id == SAI_NULL_OBJECT_ID)
    {
        return true;
    }
    if (entry.switchover &&
        !setEsBackupAttr(entry, port_name, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER, false,
                         SAI_NULL_OBJECT_ID))
    {
        return false;
    }
    if (entry.nhg_oid != SAI_NULL_OBJECT_ID)
    {
        if (!setEsBackupAttr(entry, port_name,
                             SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID, false,
                             SAI_NULL_OBJECT_ID))
        {
            return false;
        }
        /* The port's MACs were kept for the group; on a port that does not forward they
         * now blackhole. */
        Port port;
        if (gFdbOrch && gPortsOrch->getPort(port_name, port) && isEsPortDown(port))
        {
            gFdbOrch->flushFDBEntries(entry.bridge_port_id, SAI_NULL_OBJECT_ID);
        }
    }
    return true;
}

/* Switched over, and the port does not forward yet (down, or within the switchback hold). */
bool EvpnMhOrch::isEsBackupInUse(const EsBackupEntry &entry, const std::string &port_name,
                                 const Port &port) const
{
    return entry.switchover && entry.nhg_oid != SAI_NULL_OBJECT_ID &&
           (isEsPortDown(port) || m_esBackupSwitchback.count(port_name));
}

/*
 * A DEL for an ES port whose traffic the group is carrying is held: switching back sends
 * that traffic, bridged and routed, to a port that does not forward.
 */
bool EvpnMhOrch::holdEsBackupDelete(EsBackupEntry &entry, const std::string &port_name)
{
    Port port;
    if (!isPortInterfaceAssociatedToEs(port_name) || !gPortsOrch->getPort(port_name, port) ||
        !isEsBackupInUse(entry, port_name, port))
    {
        return false;
    }
    SWSS_LOG_NOTICE("ES %s backup next hop group %s removed while switched over: kept until "
                    "the port forwards again or the group goes", port_name.c_str(), entry.nhg_id.c_str());
    entry.deleted = true;
    return true;
}

/*
 * Completes a held DEL once the group is no longer in use. Erases the entry, so a caller
 * iterating m_esBackup or m_esBackupSwitchback must not call it. Returns false to retry.
 */
bool EvpnMhOrch::finishEsBackupDelete(const std::string &port_name)
{
    auto e = m_esBackup.find(port_name);
    if (e == m_esBackup.end())
    {
        return true;
    }
    auto &entry = e->second;
    Port port;
    if (gPortsOrch->getPort(port_name, port) && port.m_bridge_port_id != SAI_NULL_OBJECT_ID &&
        port.m_bridge_port_id == entry.bridge_port_id)
    {
        if (isPortInterfaceAssociatedToEs(port_name) && isEsBackupInUse(entry, port_name, port))
        {
            return true;
        }
        if (!detachEsBackup(entry, port_name))
        {
            return false;
        }
    }
    /* Otherwise the bridge port is gone or new, and the attributes went with it. */
    SWSS_LOG_NOTICE("ES %s backup next hop group removed", port_name.c_str());
    m_esBackupSwitchback.erase(port_name);
    m_esBackup.erase(e);
    return true;
}

void EvpnMhOrch::onL2NhgActive(const std::string &nhg_id)
{
    for (auto it = m_esBackupPending.begin(); it != m_esBackupPending.end();)
    {
        auto e = m_esBackup.find(*it);
        if (e != m_esBackup.end() && e->second.nhg_id == nhg_id && applyEsBackup(*it))
        {
            it = m_esBackupPending.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool EvpnMhOrch::isEsBackupAttached(const std::string &port_name) const
{
    auto e = m_esBackup.find(port_name);
    return e != m_esBackup.end() && e->second.nhg_oid != SAI_NULL_OBJECT_ID;
}

/*
 * A LAG forwards only through members that are up and distributing. When its last
 * member stops distributing, the LAG stops forwarding before its oper state follows:
 * the member's collection disable and the LAG oper down come after it, which can
 * take milliseconds where the collection disable is slow. A member counts as forwarding unless
 * it is known to be oper down or not distributing. changed_member, when set, is a
 * member whose new oper state PortsOrch has not stored yet.
 */
bool EvpnMhOrch::isEsPortDown(const Port &port, const Port *changed_member) const
{
    if (port.m_oper_status == SAI_PORT_OPER_STATUS_DOWN)
    {
        return true;
    }
    if (port.m_type != Port::LAG || port.m_members.empty())
    {
        return false;
    }
    for (const auto &alias : port.m_members)
    {
        if (m_lagMembersNotDistributing.count(alias))
        {
            continue;
        }
        Port member;
        if (changed_member && changed_member->m_alias == alias)
        {
            member = *changed_member;
        }
        else if (!gPortsOrch->getPort(alias, member))
        {
            return false;
        }
        if (member.m_oper_status != SAI_PORT_OPER_STATUS_DOWN)
        {
            return false;
        }
    }
    return true;
}

/* Switch over as soon as the ES port stops forwarding; switch back after the hold. */
void EvpnMhOrch::updateEsBackupSwitchover(const std::string &port_name, bool down)
{
    auto e = m_esBackup.find(port_name);
    if (e == m_esBackup.end() || e->second.nhg_oid == SAI_NULL_OBJECT_ID)
    {
        return;
    }
    if (!down)
    {
        if (e->second.switchover)
        {
            m_esBackupSwitchback[port_name] = std::chrono::steady_clock::now() +
                                              std::chrono::seconds(ES_BACKUP_SWITCHBACK_HOLD_SEC);
            armEsBackupSwitchbackTimer();
        }
        return;
    }
    m_esBackupSwitchback.erase(port_name);
    if (!e->second.switchover &&
        !setEsBackupAttr(e->second, port_name, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER,
                         true, SAI_NULL_OBJECT_ID))
    {
        m_esBackupPending.insert(port_name);
    }
}

void EvpnMhOrch::onEsBackupPortOperChange(const Port &port)
{
    if (m_esBackup.count(port.m_alias))
    {
        updateEsBackupSwitchover(port.m_alias, isEsPortDown(port));
    }
    if (port.m_type == Port::PHY && port.m_lag_member_id != SAI_NULL_OBJECT_ID)
    {
        onEsBackupLagMemberChange(port.m_alias, &port);
    }
}

/* A LAG member's oper or distribution state changed: re-evaluate the ES LAGs it is in. */
void EvpnMhOrch::onEsBackupLagMemberChange(const std::string &member_alias, const Port *changed_member)
{
    for (const auto &kv : m_esBackup)
    {
        Port lag;
        if (kv.second.nhg_oid == SAI_NULL_OBJECT_ID || !gPortsOrch->getPort(kv.first, lag) ||
            lag.m_type != Port::LAG || !lag.m_members.count(member_alias))
        {
            continue;
        }
        updateEsBackupSwitchover(kv.first, isEsPortDown(lag, changed_member));
    }
}

/* The timer fires at the earliest switchback deadline; each port switches back only
 * once its own hold has passed. */
void EvpnMhOrch::armEsBackupSwitchbackTimer()
{
    m_esBackupSwitchbackTimer->stop();
    if (m_esBackupSwitchback.empty())
    {
        return;
    }
    auto first = m_esBackupSwitchback.begin()->second;
    for (const auto &kv : m_esBackupSwitchback)
    {
        first = std::min(first, kv.second);
    }
    auto wait = std::chrono::duration_cast<std::chrono::nanoseconds>(first - std::chrono::steady_clock::now());
    long long ns = std::max<long long>(wait.count(), 1000000);
    m_esBackupSwitchbackTimer->setInterval(timespec { .tv_sec = static_cast<time_t>(ns / 1000000000),
                                                      .tv_nsec = static_cast<long>(ns % 1000000000) });
    m_esBackupSwitchbackTimer->start();
}

void EvpnMhOrch::onEsBackupSwitchbackTimer()
{
    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> deleted;
    for (auto it = m_esBackupSwitchback.begin(); it != m_esBackupSwitchback.end();)
    {
        if (it->second > now)
        {
            ++it;
            continue;
        }
        const std::string port_name = it->first;
        it = m_esBackupSwitchback.erase(it);
        auto e = m_esBackup.find(port_name);
        Port port;
        if (e == m_esBackup.end() || !e->second.switchover ||
            !gPortsOrch->getPort(port_name, port) || isEsPortDown(port))
        {
            continue;
        }
        if (!setEsBackupAttr(e->second, port_name, SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_SET_SWITCHOVER,
                             false, SAI_NULL_OBJECT_ID))
        {
            m_esBackupPending.insert(port_name);
        }
        else if (e->second.deleted)
        {
            deleted.push_back(port_name);
        }
    }
    armEsBackupSwitchbackTimer();
    for (const auto &port_name : deleted)
    {
        if (!finishEsBackupDelete(port_name))
        {
            m_esBackupPending.insert(port_name);
        }
    }
}

void EvpnMhOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    if (&timer == m_esBackupSwitchbackTimer)
    {
        onEsBackupSwitchbackTimer();
    }
}

void EvpnMhOrch::onEsBackupBridgePortChange(const Port &port, bool add)
{
    auto e = m_esBackup.find(port.m_alias);
    if (e != m_esBackup.end())
    {
        if (add)
        {
            if (!applyEsBackup(port.m_alias))
            {
                m_esBackupPending.insert(port.m_alias);
            }
        }
        else
        {
            /*
             * PortsOrch notifies before it removes the bridge port: take the group off it
             * first, so nothing stays switched over if the removal fails and the bridge
             * port stays (it is then notified again as added, and attached again).
             */
            if (e->second.bridge_port_id == port.m_bridge_port_id)
            {
                detachEsBackup(e->second, port.m_alias);
            }
            m_esBackupSwitchback.erase(port.m_alias);
            if (e->second.deleted)
            {
                m_esBackupPending.erase(port.m_alias);
                m_esBackup.erase(e);
            }
            else
            {
                e->second.bridge_port_id = SAI_NULL_OBJECT_ID;
                e->second.nhg_oid = SAI_NULL_OBJECT_ID;
                e->second.switchover = false;
            }
        }
        return;
    }

    /*
     * A backup group's own bridge port is removed before the group: take the group
     * off the ES ports first, which the SAI requires while it is referenced.
     */
    if (add || !gL2NhgOrch)
    {
        return;
    }
    for (auto it = m_esBackup.begin(); it != m_esBackup.end();)
    {
        auto &kv = *it;
        if (kv.second.nhg_oid != SAI_NULL_OBJECT_ID && port.m_type == Port::NEXTHOP_GROUP &&
            port.m_nexthop_group_id == kv.second.nhg_oid)
        {
            if (detachEsBackup(kv.second, kv.first) && kv.second.deleted)
            {
                SWSS_LOG_NOTICE("ES %s backup next hop group removed", kv.first.c_str());
                m_esBackupPending.erase(kv.first);
                it = m_esBackup.erase(it);
                continue;
            }
            m_esBackupPending.insert(kv.first);
        }
        ++it;
    }
}

void EvpnMhOrch::update(SubjectType type, void *cntx)
{
    SWSS_LOG_ENTER();

    /* Tracked with or without ES backup groups, so it is complete when one is attached. */
    if (type == SUBJECT_TYPE_LAG_MEMBER_DISTRIBUTION_CHANGE)
    {
        auto *update = static_cast<LagMemberDistributionUpdate *>(cntx);
        if (update->distributing)
        {
            m_lagMembersNotDistributing.erase(update->member.m_alias);
        }
        else
        {
            m_lagMembersNotDistributing.insert(update->member.m_alias);
        }
        if (!m_esBackup.empty())
        {
            onEsBackupLagMemberChange(update->member.m_alias, nullptr);
        }
        return;
    }
    if (type == SUBJECT_TYPE_LAG_MEMBER_CHANGE)
    {
        auto *update = static_cast<LagMemberUpdate *>(cntx);
        /* A member created not distributing was reported just before its add. */
        if (!update->add)
        {
            m_lagMembersNotDistributing.erase(update->member.m_alias);
        }
        /* A member can be created distributing, with no distribution change to report. */
        if (m_esBackup.count(update->lag.m_alias) && !update->lag.m_members.empty())
        {
            updateEsBackupSwitchover(update->lag.m_alias, isEsPortDown(update->lag));
        }
        return;
    }

    if (m_esBackup.empty())
    {
        return;
    }
    if (type == SUBJECT_TYPE_PORT_OPER_STATE_CHANGE)
    {
        auto *update = static_cast<PortOperStateUpdate *>(cntx);
        Port port = update->port;
        port.m_oper_status = update->operStatus;
        onEsBackupPortOperChange(port);
    }
    else if (type == SUBJECT_TYPE_BRIDGE_PORT_CHANGE)
    {
        auto *update = static_cast<PortUpdate *>(cntx);
        onEsBackupBridgePortChange(update->port, update->add);
    }
}

bool EvpnMhOrch::isInterfaceDF(const std::string &port_name, sai_vlan_id_t vlan_id)
{
    std::string df_key = VLAN_PREFIX + std::to_string(vlan_id) + ":" + port_name;
    if (EsCacheEntry *entry = getEsCache(df_key))
        return entry->is_df;
    return false;
}

// Returns true if the port participates in EVPN-MH either:
//   (a) as a port-level Ethernet Segment interface association (any VLAN), OR
//   (b) as a port+VLAN DF (Designated Forwarder) entry for the specific VLAN.
// Either condition affects forwarding decisions for traffic on (port, vlan_id),
// so both paths must be reported.
bool EvpnMhOrch::isPortAndVlanAssociatedToEs(const std::string &port_name, sai_vlan_id_t vlan_id)
{
    if (isPortInterfaceAssociatedToEs(port_name))
        return true;

    std::string df_key = VLAN_PREFIX + std::to_string(vlan_id) + ":" + port_name;
    return getEsCache(df_key) != nullptr;
}

bool EvpnMhOrch::isPortInterfaceAssociatedToEs(const std::string &port_name)
{
    return (m_esIntfMap.find(port_name) != m_esIntfMap.end());
}
