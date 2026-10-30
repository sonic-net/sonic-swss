#include "bpprotorch.h"
#include "portsorch.h"
#include <limits>
#include <ctime>
#include <sstream>
#include <cstring>

extern sai_bridge_api_t *sai_bridge_api;
using namespace swss;
namespace
{
constexpr auto groupAttr = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID;
constexpr auto modeAttr = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_MODE;
constexpr auto adminAttr = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE;
constexpr auto stateAttr = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_STATE;
const char *adminName(int32_t mode)
{
    if (mode == SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY) return "PRIMARY";
    if (mode == SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION) return "PROTECTION";
    return "UNKNOWN";
}
bool parseGroup(const std::vector<FieldValueTuple> &fields, uint32_t &id)
{
    if (fields.size() != 1 || fvField(fields.front()) != "nexthop_group") return false;
    const auto &value = fvValue(fields.front());
    if (value.empty() || (value.size() > 1 && value.front() == '0')) return false;
    id = 0;
    for (char ch : value)
    {
        if (ch < '0' || ch > '9') return false;
        uint32_t digit = ch - '0';
        if (id > (std::numeric_limits<uint32_t>::max() - digit) / 10) return false;
        id = id * 10 + digit;
    }
    return id != 0;
}
bool unsupported(sai_status_t status)
{
    return status == SAI_STATUS_NOT_SUPPORTED || status == SAI_STATUS_NOT_IMPLEMENTED ||
        (status <= SAI_STATUS_ATTR_NOT_SUPPORTED_0 && status >= SAI_STATUS_ATTR_NOT_SUPPORTED_MAX) ||
        (status <= SAI_STATUS_ATTR_NOT_IMPLEMENTED_0 && status >= SAI_STATUS_ATTR_NOT_IMPLEMENTED_MAX) ||
        (status <= SAI_STATUS_UNKNOWN_ATTRIBUTE_0 && status >= SAI_STATUS_UNKNOWN_ATTRIBUTE_MAX);
}
}

void BpProtOrch::writeState(const std::string &ifname, const std::vector<FieldValueTuple> &fields)
{
    std::set<std::string> names;
    for (const auto &field : fields) names.insert(fvField(field));
    std::vector<FieldValueTuple> previous;
    m_stateTable.get(ifname, previous);
    // Remove fields left by an older writer, including an unavailable path.
    // Table SET merges hashes, so omission alone would preserve stale fields.
    for (const auto &field : previous)
        if (!names.count(fvField(field))) m_stateTable.hdel(ifname, fvField(field));
    m_stateTable.set(ifname, fields);
}

void BpProtOrch::recordObservation(const std::string &ifname, Attachment &attachment, const std::string &observed)
{
    Port current;
    if (!m_ports->getPort(ifname, current)) return;
    if (current.m_bridge_port_id != attachment.port) return;
    if (current.m_bridge_port_generation != attachment.generation) return;
    attachment.observed = observed;
    if (observed == "PRIMARY" || observed == "PROTECTION") attachment.lastConfirmedPath = observed;
    // User-approved software interpretation: latest successful SAI path
    // readback, including a read that reveals a programming mismatch.
    char timestamp[32]; std::time_t now = std::time(nullptr); std::tm utc{};
    gmtime_r(&now, &utc); std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    attachment.lastResync = timestamp;
}

void BpProtOrch::publishRuntime(const std::string &ifname, const std::string &state,
    const std::string &reason, const std::string &observed, const std::string &programmed)
{
    auto found = m_attachments.find(ifname);
    Attachment *attachment = found == m_attachments.end() ? nullptr : &found->second;
    std::string cached = attachment ? attachment->lastConfirmedPath : "UNKNOWN";
    const std::string health = state == "STEADY" ? "STEADY" : "INCONSISTENT";
    auto policy = m_entries.find(ifname);
    const char *admin = policy == m_entries.end() || policy->second.adminMode == BpProtAdminMode::AUTO ? "AUTO" :
        policy->second.adminMode == BpProtAdminMode::PRIMARY ? "PRIMARY" : "PROTECTION";
    std::vector<FieldValueTuple> fields = {
        {"protection_type", "sw_protection"}, {"admin_mode", admin},
        {"reconciliation_state", health}, {"convergence_mode", "n/a"},
        {"t_reconcile_armed_at", ""}, {"last_arbitration_outcome", "n/a"},
        {"last_arbitration_source", "n/a"}, {"last_arbitration_ts", ""},
        {"last_cache_update_ts", ""}, {"last_resync_ts", attachment ? attachment->lastResync : ""}
    };
    if (cached == "PRIMARY" || cached == "PROTECTION") fields.emplace_back("protection_state", cached);
    std::vector<FieldValueTuple> previousFields;
    m_stateTable.get(ifname, previousFields);
    std::map<std::string, std::string> previous(previousFields.begin(), previousFields.end());
    const bool reasonChanged = attachment && attachment->lastReason != reason;
    const std::string dependencies = m_bindingTable ? pendingDependencies(ifname) : "";
    std::ostringstream diagnosticStream;
    diagnosticStream << reason << '|' << observed << '|' << programmed << '|' << dependencies;
    if (attachment)
        diagnosticStream << '|' << attachment->wtrDeadline << '|' << attachment->retryAttempts
                         << '|' << attachment->retryAt << '|' << attachment->selected;
    const std::string diagnostic = diagnosticStream.str();
    const bool diagnosticChanged = attachment && attachment->lastDiagnostic != diagnostic;
    bool changed = previous.size() != fields.size();
    for (const auto &field : fields)
        if (previous[fvField(field)] != fvValue(field)) changed = true;
    if (attachment) { attachment->lastReason = reason; attachment->lastDiagnostic = diagnostic; }
    if (changed) writeState(ifname, fields);
    if (changed || reasonChanged || diagnosticChanged)
        BP_PROT_LOG(SWSS_NOTICE, "event=%s ifname=%s reconciliation_state=%s protection_state=%s read_back=%s programmed_admin=%s dependencies=%s desired_admin=%s wtr_state=%s wtr_deadline_monotonic_ms=%llu retry_attempts=%u retry_at_monotonic_ms=%llu retry_backoff_capped=%s",
            reason.c_str(), ifname.c_str(), health.c_str(), cached.c_str(), observed.c_str(), programmed.c_str(),
            dependencies.c_str(),
            attachment && attachment->inputsKnown ? adminName(attachment->selected) : "UNKNOWN",
            attachment && attachment->wtrRunning ? "WAIT_TO_RESTORE" : "IDLE",
            static_cast<unsigned long long>(attachment ? attachment->wtrDeadline : 0),
            attachment ? attachment->retryAttempts : 0,
            static_cast<unsigned long long>(attachment ? attachment->retryAt : 0),
            attachment && attachment->retryAttempts >= 7 ? "true" : "false");
}

bool BpProtOrch::terminalResult(const std::string &ifname, Attachment &attachment,
    sai_status_t status, const char *operation, const sai_attribute_t *attrs, unsigned count, bool cleanup, bool partial)
{
    if (!unsupported(status)) return false;
    attachment.blocked = true;
    attachment.partialFailure = attachment.partialFailure || partial;
    if (!cleanup && std::strcmp(operation, "set") == 0) attachment.verificationPending = false;
    if (cleanup) attachment.cleanupBlocked = true;
    attachment.wtrRunning = false; attachment.wtrDeadline = 0;
    attachment.retryAt = 0; attachment.retryEvent = false;
    attachment.observed = attachment.programmed = "UNKNOWN";
    attachment.terminalOperation = operation; attachment.terminalStatus = status;
    std::ostringstream requested;
    for (unsigned i = 0; i < count; ++i)
    {
        if (i) requested << ',';
        switch (attrs[i].id)
        {
        case groupAttr: requested << "SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID"; break;
        case modeAttr: requested << "SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_MODE"; break;
        case adminAttr: requested << "SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE"; break;
        case stateAttr: requested << "SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_STATE"; break;
        default: requested << attrs[i].id; break;
        }
    }
    attachment.terminalAttributes = requested.str();
    const char *statusClass = status == SAI_STATUS_NOT_SUPPORTED ? "NOT_SUPPORTED" :
        status == SAI_STATUS_NOT_IMPLEMENTED ? "NOT_IMPLEMENTED" :
        (status <= SAI_STATUS_ATTR_NOT_SUPPORTED_0 && status >= SAI_STATUS_ATTR_NOT_SUPPORTED_MAX) ? "ATTR_NOT_SUPPORTED" :
        (status <= SAI_STATUS_ATTR_NOT_IMPLEMENTED_0 && status >= SAI_STATUS_ATTR_NOT_IMPLEMENTED_MAX) ? "ATTR_NOT_IMPLEMENTED" : "UNKNOWN_ATTRIBUTE";
    publishRuntime(ifname, "INCONSISTENT", "sai_unsupported");
    BP_PROT_LOG(SWSS_ERROR, "event=sai_terminal_failure ifname=%s operation=%s stage=%s attributes=%s status=%d status_class=%s reapply_required=true",
        ifname.c_str(), operation, cleanup ? "cleanup" : "programming", attachment.terminalAttributes.c_str(), status, statusClass);
    return true;
}

bool BpProtOrch::result(const std::string &ifname, Attachment &attachment, sai_status_t status,
    const char *operation, const sai_attribute_t *attrs, unsigned count)
{
    if (status == SAI_STATUS_SUCCESS) return true;
    if (terminalResult(ifname, attachment, status, operation, attrs, count, true)) return false;
    publishRuntime(ifname, "INCONSISTENT", "sai_retry");
    BP_PROT_LOG(SWSS_ERROR, "event=sai_failure ifname=%s operation=%s status=%d", ifname.c_str(), operation, status);
    return false;
}

bool BpProtOrch::clearAttachment(const std::string &ifname, Attachment &attachment)
{
    if (attachment.reference.token || attachment.replacement.token)
    {
        attachment.cleanupPending = true;
        if (attachment.cleanupBlocked) return false;
        // A changed PortsOrch OID is not proof that the old object was removed.
        // Always clear the stored object; never write a recreated port by alias.
        Port current;
        bool located = m_ports->getPort(attachment.port, current);
        if (!located) located = m_ports->getPort(ifname, current);
        if (located && current.m_bridge_port_id == attachment.port &&
            (current.m_alias != ifname || current.m_bridge_port_generation != attachment.generation))
        {
            // A reused OID is a different object. Only an old-incarnation
            // removal acknowledgement permits releasing these orphaned pins.
            publishRuntime(ifname, "INCONSISTENT", "bridge_port_incarnation_conflict");
            return false;
        }
        sai_attribute_t clear{}; clear.id = groupAttr; clear.value.oid = SAI_NULL_OBJECT_ID;
        auto status = sai_bridge_api->set_bridge_port_attribute(attachment.port, &clear);
        if (status != SAI_STATUS_ITEM_NOT_FOUND && !result(ifname, attachment, status, "set", &clear, 1)) return false;
        sai_attribute_t observed[2]{}; observed[0].id = groupAttr; observed[1].id = stateAttr;
        status = sai_bridge_api->get_bridge_port_attribute(attachment.port, 2, observed);
        if (status != SAI_STATUS_ITEM_NOT_FOUND && !result(ifname, attachment, status, "get", observed, 2)) return false;
        if (status == SAI_STATUS_SUCCESS)
            recordObservation(ifname, attachment,
                observed[1].value.s32 == SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY ? "PRIMARY" :
                observed[1].value.s32 == SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION ? "PROTECTION" : "UNKNOWN");
        if (status != SAI_STATUS_ITEM_NOT_FOUND && (observed[0].value.oid != SAI_NULL_OBJECT_ID ||
            observed[1].value.s32 != SAI_BRIDGE_PORT_PROTECTION_STATE_NOT_APPLICABLE))
        {
            publishRuntime(ifname, "INCONSISTENT", "clear_mismatch");
            return false;
        }
        attachment.observed = attachment.programmed = attachment.lastConfirmedPath = "UNKNOWN";
        m_groups->unrefNhg(attachment.reference);
        m_groups->unrefNhg(attachment.replacement);
        attachment.reference = {}; attachment.replacement = {};
    }
    attachment.partialFailure = false;
    attachment.cleanupPending = false;
    attachment.cleanupBlocked = false;
    if (!attachment.removing)
    {
        attachment.port = SAI_NULL_OBJECT_ID;
        attachment.generation = 0;
    }
    return true;
}

void BpProtOrch::consumeBinding(Consumer &consumer)
{
    for (auto it = consumer.m_toSync.begin(); it != consumer.m_toSync.end();)
    {
        const auto &ifname = kfvKey(it->second);
        const auto &op = kfvOp(it->second);
        bool accepted = false;
        if (op == DEL_COMMAND)
        {
            m_bindings.erase(ifname); accepted = true;
        }
        else if (op == SET_COMMAND)
        {
            // Use the current row, not the potentially coalesced notification.
            std::vector<FieldValueTuple> fields; uint32_t id;
            if (!m_bindingTable->get(ifname, fields))
            {
                m_bindings.erase(ifname); accepted = true;
            }
            else if (ifname.compare(0, 11, "PortChannel") == 0 &&
                ifname.size() > 11 && ifname.size() <= 15 &&
                ifname.find_first_not_of("0123456789", 11) == std::string::npos && parseGroup(fields, id))
            {
                m_bindings[ifname] = id; accepted = true;
            }
        }
        if (accepted)
        {
            auto attachment = m_attachments.find(ifname);
            if (attachment != m_attachments.end())
            {
                attachment->second.dirty = attachment->second.dirty ||
                    m_bindings.find(ifname) == m_bindings.end() ||
                    attachment->second.reference.nhg_id != m_bindings.at(ifname);
                attachment->second.retryEvent = true;
            }
            reconcile(ifname);
            updateProtectionIntent(ifname);
        }
        else
            BP_PROT_LOG(SWSS_ERROR, "event=binding_reject ifname=%s", ifname.c_str());
        it = consumer.m_toSync.erase(it);
    }
}

void BpProtOrch::doTask()
{
    Orch::doTask();
    if (!m_bindingTable) return;
    // Failed callbacks and SAI acknowledgement waits retry without requiring
    // another dependency edge. Work is coalesced to one entry per CONFIG row.
    std::vector<std::string> work;
    for (const auto &entry : m_attachments) work.push_back(entry.first);
    for (const auto &name : work) reconcile(name);
}

void BpProtOrch::reconcileAttachment(const std::string &ifname)
{
    auto policy = m_entries.find(ifname);
    auto found = m_attachments.find(ifname);
    if (found == m_attachments.end()) return; // Binding can precede CONFIG.
    auto &attachment = found->second;
    const auto binding = m_bindings.find(ifname);
    Port port; sai_object_id_t group = SAI_NULL_OBJECT_ID;
    const bool portKnown = m_ports->getPort(ifname, port) && port.m_type == Port::LAG;
    bool portReady = !attachment.removing && portKnown && port.m_bridge_port_id;
    bool groupReady = binding != m_bindings.end() && m_groups->getNhgOid(binding->second, group);
    if (groupReady)
    {
        if (attachment.wtrGroup && attachment.wtrGroup != group)
        {
            attachment.wtrRunning = false; attachment.wtrDeadline = 0;
            attachment.wtrEligible = false; attachment.wtrCompleted = false;
        }
        attachment.wtrGroup = group;
    }
    // Policy timing starts with known aggregate state, even before objects/SAI resolve.
    if (policy != m_entries.end() && portKnown && !attachment.removing && !attachment.blocked)
        evaluatePath(attachment, policy->second, port.m_oper_status == SAI_PORT_OPER_STATUS_UP);
    bool portChanged = portReady && attachment.port &&
        (attachment.port != port.m_bridge_port_id || attachment.generation != port.m_bridge_port_generation);
    bool heldUnready = false;
    if (attachment.blocked && attachment.active)
        for (const auto &reference : {attachment.reference, attachment.replacement})
        {
            sai_object_id_t live;
            if (reference.token && (!m_groups->getNhgOid(reference.nhg_id, live) || live != reference.oid)) heldUnready = true;
        }
    if (policy == m_entries.end() || !groupReady || !portReady || portChanged || heldUnready ||
        (attachment.cleanupPending && !attachment.next.token) ||
        (attachment.blocked && attachment.partialFailure))
    {
        bool blocked = attachment.blocked;
        const bool cleanup = attachment.cleanupPending;
        // Lifecycle teardown cancels timing; an initial dependency wait must not reset it.
        if (policy == m_entries.end() || !portKnown || attachment.removing || portChanged ||
            attachment.reference.token || attachment.replacement.token || attachment.next.token || attachment.active)
            resetFsm(attachment);
        if (!clearAttachment(ifname, attachment)) return;
        if (attachment.next.token)
        {
            m_groups->unrefNhg(attachment.next); attachment.next = {};
        }
        attachment.active = false; attachment.dirty = true;
        attachment.pathPending = false; attachment.verificationPending = false;
        attachment.repairAttributes = false; attachment.retryAttempts = 0; attachment.retryAt = 0;
        if (policy == m_entries.end())
        {
            m_stateTable.del(ifname); m_attachments.erase(found);
        }
        else
            publishRuntime(ifname, "INCONSISTENT", blocked ? "sai_unsupported" :
                attachment.removing ? "bridge_port_removing" : binding == m_bindings.end() ? "binding_pending" :
                !portReady ? "bridge_port_pending" :
                portChanged ? "bridge_port_replaced" : cleanup ? "cleanup_pending" : "nhg_pending");
        return;
    }
    if (attachment.blocked)
    {
        // Track aggregate state, but do not retry an unavailable feature on ordinary events.
        attachment.linkUp = port.m_oper_status == SAI_PORT_OPER_STATUS_UP;
        attachment.wtrRunning = false; attachment.wtrDeadline = 0;
        attachment.retryEvent = false;
        publishRuntime(ifname, "INCONSISTENT", "sai_unsupported");
        return;
    }
    // Oper DOWN is a path-selection input, not an attachment dependency.
    // Initial configuration and cold replay can restore the backup while
    // the local link remains down, once the bridge port and NHG exist.
    if (attachment.next.token && attachment.next.oid != group)
    {
        // next has never been written; replacing this reservation is safe.
        m_groups->unrefNhg(attachment.next); attachment.next = {};
    }
    if (attachment.replacement.token && attachment.replacement.oid != group)
    {
        // The previous replacement may already be in hardware. Reserve the
        // latest target before clearing, and retain every possibly written pin
        // until NULL/NOT_APPLICABLE is confirmed. Only next is unwritten.
        if (!attachment.next.token && !m_groups->reserveNhg(binding->second, attachment.next))
        {
            publishRuntime(ifname, "INCONSISTENT", "nhg_reservation_pending"); return;
        }
        if (!clearAttachment(ifname, attachment)) return;
        attachment.reference = attachment.next; attachment.next = {};
        attachment.dirty = true;
    }
    if (attachment.reference.token && attachment.reference.oid != group)
    {
        if (!attachment.replacement.token && !m_groups->reserveNhg(binding->second, attachment.replacement))
        {
            publishRuntime(ifname, "INCONSISTENT", "nhg_reservation_pending"); return;
        }
        attachment.dirty = true;
    }
    if (!attachment.reference.token && !m_groups->reserveNhg(binding->second, attachment.reference))
    {
        publishRuntime(ifname, "INCONSISTENT", "nhg_reservation_pending"); return;
    }
    attachment.port = port.m_bridge_port_id;
    attachment.generation = port.m_bridge_port_generation;
    if (!attachment.dirty && attachment.active)
    {
        reconcilePath(ifname, attachment, group);
        return;
    }
    if (!reconcileProgramming(ifname, attachment, group, true)) return;
    const auto &target = attachment.replacement.token ? attachment.replacement : attachment.reference;
    if (!m_groups->commitNhgRef(target))
    {
        publishRuntime(ifname, "INCONSISTENT", "nhg_commit_pending"); return;
    }
    if (attachment.replacement.token)
    {
        // Readback confirms the old group is no longer associated and the new
        // path is committed. Release the old pin only after that confirmation.
        m_groups->unrefNhg(attachment.reference);
        attachment.reference = attachment.replacement; attachment.replacement = {};
    }
    attachment.active = true; attachment.dirty = false;
    attachment.verificationPending = false; attachment.repairAttributes = false;
    attachment.pathPending = false; attachment.retryAttempts = 0; attachment.retryAt = 0;
    publishRuntime(ifname, "STEADY", "path_confirmed", attachment.observed, attachment.programmed);
}
