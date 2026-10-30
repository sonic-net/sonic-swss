#include "bpprotorch.h"
#include "portsorch.h"

namespace
{
// A SAI read/callback may synchronously withdraw its group. Do not invalidate
// the in-flight attachment or its references; a failed commit retries later.
struct ReconcileGuard
{
    std::set<std::string> &running;
    std::string name;
    ~ReconcileGuard() { running.erase(name); }
};
bool samePort(const Port &left, const Port &right)
{
    return left.m_bridge_port_id == right.m_bridge_port_id &&
        left.m_bridge_port_generation == right.m_bridge_port_generation;
}
}

BpProtOrch::~BpProtOrch()
{
    detachObservers();
}

void BpProtOrch::detachObservers()
{
    // OrchDaemon calls this before destroying any subject; standalone fixtures
    // may call it explicitly. Do not deprogram hardware during process exit.
    if (!m_observersAttached) return;
    m_fsmTimer->stop();
    m_ports->detach(this);
    m_groups->detach(this);
    m_observersAttached = false;
}

void BpProtOrch::reconcile(const std::string &ifname)
{
    if (!m_reconciling.insert(ifname).second) return;
    ReconcileGuard guard{m_reconciling, ifname};
    reconcileAttachment(ifname);
}

void BpProtOrch::groupEvent(const L2NhgUpdate &event)
{
    sai_object_id_t current = SAI_NULL_OBJECT_ID;
    const bool ready = m_groups->getNhgOid(event.nhg_id, current);
    if (event.change == L2NhgChange::READY && (!ready || current != event.oid)) return;
    std::vector<std::string> affected;
    for (const auto &entry : m_attachments)
    {
        auto binding = m_bindings.find(entry.first);
        bool desired = binding != m_bindings.end() && binding->second == event.nhg_id;
        bool held = false;
        for (const auto &reference : {entry.second.reference, entry.second.replacement, entry.second.next})
            if (reference.token && reference.nhg_id == event.nhg_id && reference.oid == event.oid) held = true;
        // An old physical-removal event cannot disturb a recreated ID/OID.
        if (ready && current != event.oid) desired = false;
        if (desired || held) affected.push_back(entry.first);
    }
    for (const auto &name : affected) reconcile(name);
}

void BpProtOrch::update(SubjectType type, void *context)
{
    if (!m_observersAttached || !context) return;
    if (type == SUBJECT_TYPE_L2_NHG_CHANGE)
    {
        groupEvent(*static_cast<L2NhgUpdate *>(context));
        return;
    }
    if (type == SUBJECT_TYPE_BRIDGE_PORT_PRE_REMOVE)
    {
        auto &event = *static_cast<BridgePortRemovalUpdate *>(context);
        auto found = m_attachments.find(event.port.m_alias);
        Port current;
        if (found == m_attachments.end() || event.port.m_type != Port::LAG ||
            !m_ports->getPort(event.port.m_alias, current) || !samePort(current, event.port)) return;
        if (found->second.port && (found->second.port != event.port.m_bridge_port_id ||
            found->second.generation != event.port.m_bridge_port_generation))
        {
            // Drain an older attachment before recording this removal identity.
            reconcile(event.port.m_alias);
            found = m_attachments.find(event.port.m_alias);
            if (found == m_attachments.end()) return;
            if (found->second.port)
            {
                event.ready = false;
                return;
            }
        }
        auto &attachment = found->second;
        attachment.removing = true;
        // Remember the removal identity even if the binding has not attached.
        if (!attachment.port)
        {
            attachment.port = event.port.m_bridge_port_id;
            attachment.generation = event.port.m_bridge_port_generation;
        }
        reconcile(event.port.m_alias);
        found = m_attachments.find(event.port.m_alias);
        if (m_reconciling.count(event.port.m_alias) || (found != m_attachments.end() &&
            (found->second.reference.token || found->second.replacement.token || found->second.next.token)))
            event.ready = false;
        return;
    }
    if (type == SUBJECT_TYPE_BRIDGE_PORT_CHANGE || type == SUBJECT_TYPE_PORT_CHANGE)
    {
        const auto &event = *static_cast<PortUpdate *>(context);
        if (event.port.m_type != Port::LAG) return;
        auto found = m_attachments.find(event.port.m_alias);
        if (found == m_attachments.end()) return;
        auto &attachment = found->second;
        if (type == SUBJECT_TYPE_BRIDGE_PORT_CHANGE && !event.add)
        {
            if (attachment.port != event.removedBridgePortId ||
                attachment.generation != event.removedBridgePortGeneration) return;
            // PortsOrch has confirmed physical removal. No SAI write may now
            // target this OID, since an allocator can immediately reuse it.
            m_groups->unrefNhg(attachment.reference);
            m_groups->unrefNhg(attachment.replacement);
            m_groups->unrefNhg(attachment.next);
            // Physical-removal proof releases old pins, not a platform capability failure.
            const bool blocked = attachment.blocked;
            const auto operation = attachment.terminalOperation;
            const auto attributes = attachment.terminalAttributes;
            const auto status = attachment.terminalStatus;
            attachment = {};
            attachment.blocked = blocked; attachment.terminalOperation = operation;
            attachment.terminalAttributes = attributes; attachment.terminalStatus = status;
        }
        else if (event.add)
        {
            Port current;
            if (!m_ports->getPort(event.port.m_alias, current) || !samePort(current, event.port)) return;
            attachment.removing = false; // Includes an explicit removal cancellation.
        }
        reconcile(event.port.m_alias);
        return;
    }
    if (type == SUBJECT_TYPE_PORT_OPER_STATE_CHANGE)
    {
        const auto &event = *static_cast<PortOperStateUpdate *>(context);
        Port current;
        if (event.port.m_type != Port::LAG || !m_ports->getPort(event.port.m_alias, current) ||
            !samePort(current, event.port) || current.m_oper_status != event.operStatus) return;
        // Aggregate state is authoritative; duplicate events cannot restart WTR.
        auto found = m_attachments.find(event.port.m_alias);
        if (found != m_attachments.end() &&
            found->second.linkUp != (event.operStatus == SAI_PORT_OPER_STATUS_UP))
            found->second.retryEvent = true;
        reconcile(event.port.m_alias);
    }
}

std::string BpProtOrch::pendingDependencies(const std::string &ifname) const
{
    std::vector<std::string> pending;
    auto binding = m_bindings.find(ifname);
    sai_object_id_t group;
    if (binding == m_bindings.end()) pending.push_back("BINDING");
    else if (!m_groups->getNhgOid(binding->second, group)) pending.push_back("NHG");
    auto attachment = m_attachments.find(ifname);
    Port port;
    if (attachment != m_attachments.end() && attachment->second.removing)
        pending.push_back("BRIDGE_PORT_REMOVAL");
    else if (!m_ports->getPort(ifname, port) || port.m_type != Port::LAG || !port.m_bridge_port_id)
        pending.push_back("BRIDGE_PORT");
    std::string result;
    for (const auto &dependency : pending)
    {
        if (!result.empty()) result += ',';
        result += dependency;
    }
    return result;
}
