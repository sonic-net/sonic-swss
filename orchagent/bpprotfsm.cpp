#include "bpprotorch.h"
#include "portsorch.h"
#include "timer.h"
#include <algorithm>
#include <chrono>
#include <limits>

extern sai_bridge_api_t *sai_bridge_api;
namespace
{
constexpr auto groupAttr = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_NEXT_HOP_GROUP_ID;
constexpr auto modeAttr = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_MODE;
constexpr auto adminAttr = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE;
constexpr auto stateAttr = SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_STATE;
// Select registers the Executor wrapper, whose default priority is zero;
// setting only the underlying SelectableTimer priority would not order events.
class ProtectionTimer final : public swss::ExecutableTimer
{
public:
    using swss::ExecutableTimer::ExecutableTimer;
    int getPri() const override { return -1; }
};
uint64_t deadline(uint64_t now, uint64_t delay)
{
    return delay > std::numeric_limits<uint64_t>::max() - now ?
        std::numeric_limits<uint64_t>::max() : now + delay;
}
const char *pathName(int32_t mode)
{
    if (mode == SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY) return "PRIMARY";
    if (mode == SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION) return "PROTECTION";
    return "UNKNOWN";
}

}

void BpProtOrch::initializeFsmTimer()
{
    // Keep the timer owned if construction of its executor fails. Once added,
    // the normal Orch executor lifecycle owns and destroys the selectable.
    auto timer = std::make_unique<swss::SelectableTimer>(timespec{0, 100000000}, -1);
    auto executor = new ProtectionTimer(timer.get(), this, "BP_PROTECTION_FSM");
    m_fsmTimer = timer.release();
    addExecutor(executor);
    m_fsmTimer->start();
}

uint64_t BpProtOrch::monotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void BpProtOrch::doTask(swss::SelectableTimer &timer)
{
    if (&timer != m_fsmTimer || !m_observersAttached) return;
    // Untimed dependency waits retain the existing periodic task. This clock
    // wakes only countdown/retry work and never rescans all steady DB rows.
    std::vector<std::string> work;
    const auto now = m_now();
    for (const auto &entry : m_attachments)
    {
        const auto &attachment = entry.second;
        if ((attachment.wtrRunning && now >= attachment.wtrDeadline) ||
            (attachment.pathPending && !attachment.blocked && now >= attachment.retryAt))
            work.push_back(entry.first);
    }
    for (const auto &name : work) reconcile(name);
}

void BpProtOrch::resetFsm(Attachment &attachment)
{
    attachment.wtrRunning = false; attachment.wtrDeadline = 0;
    attachment.wtrEligible = false; attachment.wtrCompleted = false; attachment.wtrGroup = SAI_NULL_OBJECT_ID;
    attachment.inputsKnown = false; attachment.pathPending = false;
    attachment.verificationPending = false; attachment.repairAttributes = false; attachment.verificationGroup = SAI_NULL_OBJECT_ID;
    attachment.retryEvent = false;
    attachment.retryAttempts = 0; attachment.retryAt = 0;
    attachment.selected = SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY;
    attachment.observed = attachment.programmed = "UNKNOWN";
}

void BpProtOrch::evaluatePath(Attachment &attachment, const BpProtEntry &policy, bool up)
{
    const auto now = m_now();
    const bool inputChanged = !attachment.inputsKnown || attachment.linkUp != up ||
        attachment.policy.adminMode != policy.adminMode || attachment.policy.revertive != policy.revertive ||
        attachment.policy.waitToRestoreTimeMs != policy.waitToRestoreTimeMs;
    const bool durationChanged = attachment.inputsKnown &&
        attachment.policy.waitToRestoreTimeMs != policy.waitToRestoreTimeMs;
    const bool first = !attachment.inputsKnown;
    attachment.inputsKnown = true; attachment.linkUp = up; attachment.policy = policy;
    auto target = attachment.selected;
    if (first)
        target = policy.adminMode == BpProtAdminMode::PROTECTION ||
            (policy.adminMode == BpProtAdminMode::AUTO && !up) ?
            SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION : SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY;
    else if (policy.adminMode == BpProtAdminMode::PRIMARY)
        target = SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY;
    else if (policy.adminMode == BpProtAdminMode::PROTECTION || !up)
        target = SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PROTECTION;
    // WTR measures aggregate link stability; SAI path/health is not an input.
    const bool canRestore = policy.adminMode == BpProtAdminMode::AUTO && up && policy.revertive;
    if (!canRestore)
    {
        attachment.wtrRunning = false; attachment.wtrDeadline = 0;
        attachment.wtrCompleted = false;
    }
    else
    {
        if (!attachment.wtrEligible || durationChanged || (!attachment.wtrRunning && !attachment.wtrCompleted))
        {
            attachment.wtrRunning = true;
            attachment.wtrCompleted = false;
            attachment.wtrDeadline = deadline(now, uint64_t(policy.waitToRestoreTimeMs));
        }
        if (attachment.wtrRunning && now >= attachment.wtrDeadline)
        {
            target = SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY;
            attachment.wtrRunning = false; attachment.wtrDeadline = 0;
            attachment.wtrCompleted = true;
        }
    }
    attachment.wtrEligible = canRestore;
    if (target != attachment.selected)
    {
        attachment.selected = target; attachment.pathPending = true;
        attachment.verificationPending = false;
        attachment.retryEvent = true;
    }
    if (inputChanged || attachment.retryEvent)
    {
        attachment.retryAttempts = 0; attachment.retryAt = 0;
        attachment.retryEvent = false;
    }
}

bool BpProtOrch::reconcileProgramming(const std::string &ifname, Attachment &attachment,
    sai_object_id_t group, bool attaching)
{
    // A new association cannot inherit verification/backoff for an old group.
    if (attachment.verificationGroup != group)
    {
        attachment.verificationGroup = group; attachment.verificationPending = false;
        attachment.repairAttributes = false; attachment.retryAttempts = 0; attachment.retryAt = 0;
    }
    attachment.pathPending = true;
    if (attachment.blocked || m_now() < attachment.retryAt) return false;

    const auto port = attachment.port;
    const auto generation = attachment.generation;
    const auto selected = attachment.selected;
    const auto policy = attachment.policy;
    const bool up = attachment.linkUp;
    auto currentTarget = [&]() {
        Port current; sai_object_id_t live;
        auto binding = m_bindings.find(ifname);
        auto config = m_entries.find(ifname);
        if (attachment.removing || binding == m_bindings.end() || config == m_entries.end() ||
            !m_groups->getNhgOid(binding->second, live) || live != group ||
            !m_ports->getPort(ifname, current) || current.m_bridge_port_id != port ||
            current.m_bridge_port_generation != generation ||
            (current.m_oper_status == SAI_PORT_OPER_STATUS_UP) != up || config->second.adminMode != policy.adminMode ||
            config->second.revertive != policy.revertive ||
            config->second.waitToRestoreTimeMs != policy.waitToRestoreTimeMs)
        {
            attachment.wtrRunning = false; attachment.wtrDeadline = 0;
            attachment.wtrEligible = false; attachment.wtrCompleted = false;
            publishRuntime(ifname, "INCONSISTENT", attaching ? "bridge_port_dependency_changed" : "path_dependency_changed");
            return false;
        }
        return true;
    };
    sai_attribute_t expected[3]{};
    expected[0].id = groupAttr; expected[0].value.oid = group;
    expected[1].id = modeAttr; expected[1].value.s32 = SAI_BRIDGE_PORT_PROTECTION_MODE_SOFTWARE;
    expected[2].id = adminAttr; expected[2].value.s32 = selected;
    sai_attribute_t attrs[4]{};
    auto readState = [&]() {
        attrs[0].id = groupAttr; attrs[1].id = modeAttr; attrs[2].id = adminAttr; attrs[3].id = stateAttr;
        const auto status = sai_bridge_api->get_bridge_port_attribute(port, 4, attrs);
        attachment.observed = attachment.programmed = "UNKNOWN";
        if (status == SAI_STATUS_SUCCESS)
        {
            const auto path = attrs[3].value.s32 == SAI_BRIDGE_PORT_PROTECTION_STATE_PRIMARY ? "PRIMARY" :
                attrs[3].value.s32 == SAI_BRIDGE_PORT_PROTECTION_STATE_PROTECTION ? "PROTECTION" : "UNKNOWN";
            attachment.programmed = pathName(attrs[2].value.s32);
            // Observation is independent of intent; this validates bridge incarnation.
            // currentTarget below separately vetoes stale writes/STEADY certification.
            recordObservation(ifname, attachment, path);
        }
        return status;
    };
    bool needed[3] = {attaching || attachment.repairAttributes, attaching || attachment.repairAttributes, true};
    sai_status_t written = SAI_STATUS_SUCCESS;
    sai_status_t read = SAI_STATUS_SUCCESS;
    bool write = !attachment.verificationPending;
    if (attachment.verificationPending)
    {
        read = readState();
        if (!currentTarget()) return false;
        if (terminalResult(ifname, attachment, read, "get", attrs, 4, false, attaching)) return false;
        if (read == SAI_STATUS_SUCCESS)
        {
            needed[0] = attrs[0].value.oid != group;
            needed[1] = attrs[1].value.s32 != SAI_BRIDGE_PORT_PROTECTION_MODE_SOFTWARE;
            needed[2] = attrs[2].value.s32 != selected;
            write = needed[0] || needed[1] || needed[2];
            attachment.repairAttributes = needed[0] || needed[1];
        }
        // A failed GET has not proved that a corrective write is needed.
    }
    if (write)
    {
        for (unsigned i = 0; i < 3; ++i)
        {
            if (!needed[i]) continue;
            written = sai_bridge_api->set_bridge_port_attribute(port, &expected[i]);
            if (!currentTarget()) return false;
            if (terminalResult(ifname, attachment, written, "set", &expected[i], 1, false, attaching)) return false;
            if (written != SAI_STATUS_SUCCESS) break;
        }
        attachment.verificationPending = written == SAI_STATUS_SUCCESS;
        // Read even after a retryable rejected write so the public path stays observed.
        read = readState();
        if (!currentTarget()) return false;
        if (terminalResult(ifname, attachment, read, "get", attrs, 4, false, attaching)) return false;
    }
    const bool matched = read == SAI_STATUS_SUCCESS && attrs[0].value.oid == group &&
        attrs[1].value.s32 == SAI_BRIDGE_PORT_PROTECTION_MODE_SOFTWARE &&
        attrs[2].value.s32 == selected && attachment.observed == pathName(selected);
    if (written == SAI_STATUS_SUCCESS && matched)
    {
        attachment.retryAttempts = 0; attachment.retryAt = 0;
        attachment.verificationPending = true; // Keep verification until the caller commits ownership.
        return true;
    }
    // One attempt per eligible task pass: retain work and yield, never sleep/spin.
    attachment.verificationPending = written == SAI_STATUS_SUCCESS;
    if (attachment.retryAttempts < std::numeric_limits<unsigned>::max()) ++attachment.retryAttempts;
    attachment.retryAt = deadline(m_now(), std::min<uint64_t>(100ULL << std::min(attachment.retryAttempts - 1, 6u), 5000));
    const char *reason = attaching ?
        (written != SAI_STATUS_SUCCESS || read != SAI_STATUS_SUCCESS ? "sai_retry" : "attach_mismatch") :
        written != SAI_STATUS_SUCCESS ? "path_write_failed" : read != SAI_STATUS_SUCCESS ? "path_read_failed" : "path_mismatch";
    publishRuntime(ifname, "INCONSISTENT", reason, attachment.observed, attachment.programmed);
    BP_PROT_LOG(SWSS_ERROR, "event=program_retry ifname=%s write=%d read=%d attempt=%u",
        ifname.c_str(), written, read, attachment.retryAttempts);
    return false;
}

void BpProtOrch::reconcilePath(const std::string &ifname, Attachment &attachment, sai_object_id_t group)
{
    if (!attachment.pathPending)
    {
        publishRuntime(ifname, "STEADY", attachment.wtrRunning ? "wtr_started" : "path_confirmed",
            attachment.observed, attachment.programmed);
        return;
    }
    if (!reconcileProgramming(ifname, attachment, group, false)) return;
    attachment.pathPending = false; attachment.verificationPending = false; attachment.repairAttributes = false;
    publishRuntime(ifname, "STEADY", "path_confirmed", attachment.observed, attachment.programmed);
}
