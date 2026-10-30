#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "orch.h"
#include "logger.h"
#include "l2nhgorch.h"

class PortsOrch;

constexpr const char *CFG_BRIDGE_PORT_PROTECTION_TABLE_NAME = "BRIDGE_PORT_PROTECTION";
constexpr const char *APP_BP_PROTECTION_BINDING_TABLE_NAME = "EVPN_ES_BACKUP_NHG_TABLE";
constexpr const char *STATE_BRIDGE_PORT_PROTECTION_TABLE_NAME = "BRIDGE_PORT_PROTECTION_STATE";

// Independent filtering/output: Logger::write uses an orchagent-wide threshold.
// Static callbacks remain valid for the lifetime of the DB logging observer.
class BpProtLog
{
public:
    static void initialize();
    static void setPriority(const std::string &component, const std::string &value);
    static void setOutput(const std::string &component, const std::string &value);
    static void write(swss::Logger::Priority priority, const char *format, ...)
        __attribute__((format(printf, 2, 3)));
};

#define BP_PROT_LOG(priority, format, ...) \
    BpProtLog::write(swss::Logger::priority, ":- bpProtOrch: %s: " format, __FUNCTION__, ##__VA_ARGS__)
#define BP_PROT_LOG_ENTER() BP_PROT_LOG(SWSS_DEBUG, "enter")

enum class BpProtAdminMode
{
    AUTO,
    PRIMARY,
    PROTECTION
};

// Internal FDB lifecycle predicate, separate from public reconciliation state.
enum class BpProtectionStatus { NONE, PENDING, PROTECTED };

// Operator policy only. Runtime binding and observed SAI state are separate
// concerns, added when the protection data path is implemented.
struct BpProtEntry
{
    std::string protectionMode = "SOFTWARE";
    BpProtAdminMode adminMode = BpProtAdminMode::AUTO;
    bool revertive = false;
    uint32_t waitToRestoreTimeMs = 0;
};

class BpProtOrch : public Orch, public Observer
{
public:
    BpProtOrch(swss::DBConnector *configDb, swss::DBConnector *stateDb);
    BpProtOrch(swss::DBConnector *configDb, swss::DBConnector *stateDb,
        swss::DBConnector *appDb, PortsOrch *ports, L2NhgOrch *groups);
    ~BpProtOrch() override;
    void detachObservers() override;
    void update(SubjectType type, void *context) override;
    void doTask() override;
    void doTask(swss::SelectableTimer &timer) override;
    const BpProtEntry *getEntry(const std::string &ifname) const;
    BpProtectionStatus isProtectedBridgePort(const std::string &ifname);

private:
    struct Attachment
    {
        sai_object_id_t port = SAI_NULL_OBJECT_ID;
        uint64_t generation = 0;
        bool removing = false;
        L2NhgReference reference;
        L2NhgReference replacement;
        L2NhgReference next; // Unwritten reservation while cancelling a partial replacement.
        bool active = false;
        bool dirty = true;
        bool blocked = false; // Terminal programming failure; only validated CONFIG SET re-arms.
        bool partialFailure = false; // Terminal failure during unconfirmed attachment/replacement.
        bool cleanupBlocked = false; // Terminal clear/GET; keep references until explicit reapplication.
        bool cleanupPending = false;
        std::string terminalOperation, terminalAttributes;
        sai_status_t terminalStatus = SAI_STATUS_SUCCESS;
        int32_t selected = SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_PRIMARY;
        bool inputsKnown = false;
        bool linkUp = false;
        BpProtEntry policy;
        bool wtrRunning = false;
        bool wtrEligible = false;
        bool wtrCompleted = false; // One window per continuous eligible UP period.
        sai_object_id_t wtrGroup = SAI_NULL_OBJECT_ID; // Replacement resets timing once, not per SAI retry.
        uint64_t wtrDeadline = 0;
        bool pathPending = false;
        bool verificationPending = false; // Retry GET first after a successful write.
        bool repairAttributes = false; // Failed group/mode correction must be retried.
        sai_object_id_t verificationGroup = SAI_NULL_OBJECT_ID;
        bool retryEvent = false;
        unsigned retryAttempts = 0;
        uint64_t retryAt = 0;
        std::string observed = "UNKNOWN";
        std::string lastConfirmedPath = "UNKNOWN";
        std::string programmed = "UNKNOWN";
        std::string lastReason;
        std::string lastDiagnostic;
        std::string lastResync;
    };
    void initializeFsmTimer();
    static uint64_t monotonicMs();
    std::function<uint64_t()> m_now = monotonicMs;
    swss::SelectableTimer *m_fsmTimer = nullptr; // Owned by ExecutableTimer/Orch.
    void resetFsm(Attachment &attachment);
    void evaluatePath(Attachment &attachment, const BpProtEntry &policy, bool up);
    void reconcilePath(const std::string &ifname, Attachment &attachment, sai_object_id_t group);
    bool reconcileProgramming(const std::string &ifname, Attachment &attachment, sai_object_id_t group, bool attaching);
    PortsOrch *m_ports;
    L2NhgOrch *m_groups;
    bool m_observersAttached = false;
    std::set<std::string> m_reconciling;
    std::unique_ptr<swss::Table> m_bindingTable;
    std::map<std::string, uint32_t> m_bindings;
    std::map<std::string, Attachment> m_attachments;
    std::set<std::string> m_fdbProtectionIntent;
    bool hasProtectionIntent(const std::string &ifname);
    void updateProtectionIntent(const std::string &ifname);
    void consumeBinding(Consumer &consumer);
    void reconcile(const std::string &ifname);
    void reconcileAttachment(const std::string &ifname);
    void groupEvent(const L2NhgUpdate &event);
    std::string pendingDependencies(const std::string &ifname) const;
    bool clearAttachment(const std::string &ifname, Attachment &attachment);
    bool result(const std::string &ifname, Attachment &attachment, sai_status_t status,
        const char *operation, const sai_attribute_t *attrs, unsigned count);
    bool terminalResult(const std::string &ifname, Attachment &attachment, sai_status_t status,
        const char *operation, const sai_attribute_t *attrs, unsigned count, bool cleanup, bool partial = false);
    void publishRuntime(const std::string &ifname, const std::string &state, const std::string &reason,
        const std::string &observed = "UNKNOWN", const std::string &programmed = "UNKNOWN");
    void writeState(const std::string &ifname, const std::vector<swss::FieldValueTuple> &fields);
    void recordObservation(const std::string &ifname, Attachment &attachment, const std::string &observed);
    swss::Table m_configTable;
    swss::Table m_stateTable;
    std::map<std::string, BpProtEntry> m_entries;
    void publishConfig(const std::string &ifname, const BpProtEntry &entry);
    void doTask(Consumer &consumer) override;
};
