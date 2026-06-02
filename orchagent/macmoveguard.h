#ifndef SWSS_MACMOVEGUARD_H
#define SWSS_MACMOVEGUARD_H

#include "orch.h"
#include "portsorch.h"
#include "fdborch.h"
#include "timer.h"

#include <boost/functional/hash.hpp>
#include <deque>
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <cstdint>

// Acronyms used throughout this header / implementation:
//   MMG    = MAC Move Guard (this feature)
//   DLOMWA = Disable Learn On Mac With Acl
//            One of MMG's mitigation actions: install a pre-ingress ACL entry
//            (vlan, smac) -> SAI_ACL_ACTION_TYPE_SET_DO_NOT_LEARN to suppress
//            learning of the offending source MAC while forwarding via the
//            existing FDB lookup continues.

#define CFG_MAC_MOVE_GUARD_TABLE_NAME       "MAC_MOVE_GUARD"
#define STATE_MAC_MOVE_GUARD_TABLE_NAME     "MAC_MOVE_GUARD"

// STATE_DB table where MacMoveGuard publishes which actions it supports on
// this platform. Single row "ACTIONS"; per-action boolean fields. DISABLE_PORT
// is always "true"; DISABLE_LEARN_ON_MAC_WITH_ACL depends on a SAI capability
// probe done in the guard's constructor.
#define STATE_MMG_CAPABILITY_TABLE_NAME     "MMG_CAPABILITY_TABLE"
#define MMG_CAPABILITY_ACTIONS_KEY          "ACTIONS"
#define MMG_ACTION_DISABLE_PORT             "DISABLE_PORT"
#define MMG_ACTION_DISABLE_LEARN_ON_MAC_WITH_ACL  "DISABLE_LEARN_ON_MAC_WITH_ACL"

// Name under which MacMoveGuard registers its recovery SelectableTimer with the
// owning FdbOrch's executor list. Exposed so FdbOrch can dispatch timer ticks
// back to the guard without hard-coding the string in multiple places.
#define MAC_MOVE_GUARD_RECOVERY_TIMER_NAME  "MAC_MOVE_GUARD_RECOVERY"

// Action types for MAC move guard
enum class MacMoveGuardAction
{
    DISABLE_PORT,           // Administratively disable the port where MAC was last seen
    DISABLE_LEARN_ON_MAC_WITH_ACL,   // Install a pre-ingress ACL entry (vlan,smac)->SET_DO_NOT_LEARN to suppress learning
    // Future actions can be added here, e.g.:
    //    LOG_ONLY,       Alert/log but take no action
    //    RATE_LIMIT,     Rate-limit traffic from this MAC
    //    DROP_MAC,       Drop all traffic from this MAC
};

// Identifies a MAC we're tracking: a (VLAN, MAC) pair.
// bv_id is the SAI bridge-vlan OID, which uniquely identifies the VLAN.
struct MacKey
{
    MacAddress mac;
    sai_object_id_t bv_id;

    bool operator<(const MacKey &other) const
    {
        return std::tie(mac, bv_id) < std::tie(other.mac, other.bv_id);
    }

    bool operator==(const MacKey &other) const
    {
        return std::tie(mac, bv_id) == std::tie(other.mac, other.bv_id);
    }
};

// Hash functor for MacKey, used by std::unordered_map. Built on
// boost::hash_combine (the same pattern bulker.h uses for composite SAI
// keys); portable across 32-bit and 64-bit targets.
struct MacKeyHash
{
    std::size_t operator()(const MacKey &k) const noexcept
    {
        std::size_t seed = 0;
        const uint8_t *m = k.mac.getMac();
        for (int i = 0; i < 6; ++i)
        {
            boost::hash_combine(seed, m[i]);
        }
        boost::hash_combine(seed, k.bv_id);
        return seed;
    }
};

// Per-MAC tracking state: move history, bad MAC status, and action tracking.
struct MacMoveTrackingState
{
    // Sliding window of move timestamps (for counting moves within detect_interval)
    std::deque<std::chrono::steady_clock::time_point> move_timestamps;

    // Track each port and when MAC was last seen on it (for detect_interval window)
    std::map<std::string, std::chrono::steady_clock::time_point> ports_seen;

    size_t move_count = 0;                                              // total MAC moves within detect_interval
    bool is_bad_mac = false;                                            // is this MAC identified as bad?
    MacMoveGuardAction action = MacMoveGuardAction::DISABLE_PORT;       // action used when this MAC was marked bad (sticky so cleanup matches the original action)
    std::chrono::steady_clock::time_point action_expiry_time;           // when the action interval expires
    std::string pinned_port;                                            // the ONE port we keep active for this bad MAC (DISABLE_PORT action)
    std::set<std::string> disabled_ports;                               // all other ports we disabled for this bad MAC (DISABLE_PORT action)
    std::string last_port;                                              // most recent port this MAC was seen on
    sai_object_id_t learn_disable_acl_entry_id = SAI_NULL_OBJECT_ID;    // ACL entry installed for this bad MAC (DISABLE_LEARN_ON_MAC_WITH_ACL action)
};

// Cached learnt-MAC entry: which port the MAC was last seen on and when.
// The timestamp lets us prune entries older than the detection window so
// the cache does not grow unbounded for MACs that have gone quiet.
struct LearntMacEntry
{
    std::string port;
    std::chrono::steady_clock::time_point last_seen;
};

class FdbOrch;

// MacMoveGuard: detects MACs flapping between ports faster than a configured
// threshold and applies a remediation (admin-disable port, or install an ACL
// entry that suppresses learning for the offending MAC). It is owned by
// FdbOrch via composition: FdbOrch calls onMacMove()/onMacLearn() inline when
// emitting FDB updates, and routes the guard's config-table consumer and
// recovery SelectableTimer (both registered with FdbOrch's executor list)
// back to doConfigTask()/doRecoveryTimerTask().
class MacMoveGuard
{
public:
    MacMoveGuard(DBConnector *configDb, DBConnector *stateDb,
                 const std::string &tableName,
                 PortsOrch *portsOrch, FdbOrch *fdbOrch);
    ~MacMoveGuard();

    // FDB event entry points. Called by FdbOrch when emitting MAC learn/move
    // updates. No-op if the feature is disabled.
    void onMacMove(const MacMoveNotification &notif);
    void onMacLearn(const MacLearnNotification &notif);

    // Dispatched by FdbOrch::doTask(Consumer&) when the consumer belongs to
    // the MAC_MOVE_GUARD config table.
    void doConfigTask(Consumer &consumer);

    // Dispatched by FdbOrch::doTask(SelectableTimer&) when the recovery timer
    // fires.
    void doRecoveryTimerTask();

    // Identity check used by FdbOrch::doTask(SelectableTimer&) so the guard's
    // private timer pointer does not have to be exposed.
    bool isMyTimer(const swss::SelectableTimer *t) const { return t == m_recoveryTimer; }

private:
    PortsOrch *m_portsOrch;
    FdbOrch *m_fdbOrch;
    std::string m_tableName;

    // Configuration. Defaults applied when the feature is enabled.
    bool m_enabled = false;
    uint32_t m_threshold = 1000;                       // max mac moves allowed in window
    uint32_t m_durationSeconds = 5;                    // detect_interval: sliding window in seconds
    uint32_t m_recoverySeconds = 600;                  // action_interval: recovery period in seconds
    MacMoveGuardAction m_action = MacMoveGuardAction::DISABLE_PORT;  // action to take on bad MAC

    // Per-MAC move tracking state, keyed by (mac, bv_id).
    std::map<MacKey, MacMoveTrackingState> m_macTrackingState;

    // For each port we have admin-disabled, the set of bad MACs currently
    // requiring it to be disabled. The port is re-enabled only when the set becomes empty.
    std::map<std::string, std::set<MacKey>> m_disabledPorts;

    // Last-known port (and timestamp) for every (mac, bv_id) we have seen
    // LEARNED. We never erase on AGED — a subsequent LEARN on a different
    // port is recognized as a move via port comparison. Entries older than
    // detect_interval are pruned by checkRecovery() to bound memory use.
    std::unordered_map<MacKey, LearntMacEntry, MacKeyHash> m_learntMac;

    // Recovery timer: fires periodically to check recovery conditions.
    // Owned by the executor registered on the parent FdbOrch.
    swss::SelectableTimer *m_recoveryTimer = nullptr;
    static const int RECOVERY_CHECK_INTERVAL_SECS = 30;

    // STATE_DB table used only to remember which ports we admin-disabled, so
    // they can be re-enabled if orchagent restarts. A single row
    // (HW_RESOURCES_KEY) holds a CSV of port aliases; no per-MAC state and no
    // expiry timing is persisted. DLOMWA leaves no STATE_DB footprint — its
    // ACL table is rediscovered on restart by signature-matching the table
    // bound to SAI_SWITCH_ATTR_PRE_INGRESS_ACL.
    std::unique_ptr<swss::Table> m_stateTable;
    static constexpr const char *HW_RESOURCES_KEY    = "HW_RESOURCES";
    static constexpr const char *DISABLED_PORTS_FIELD = "disabled_ports";

    // STATE_DB table where we publish which mitigation actions this platform
    // supports. Single row, written once from the constructor.
    std::unique_ptr<swss::Table> m_capabilityTable;

    // Shared ACL resources for the DISABLE_LEARN_ON_MAC_WITH_ACL action. The table
    // lifecycle is tied to CONFIG_DB: it is created when the feature is configured
    // with this action and destroyed when the feature is disabled or the action
    // changes away from DISABLE_LEARN_ON_MAC_WITH_ACL. The bind to the switch
    // pre-ingress slot (SAI_SWITCH_ATTR_PRE_INGRESS_ACL) follows the same lifecycle.
    sai_object_id_t m_learnDisableAclTable      = SAI_NULL_OBJECT_ID;
    size_t          m_learnDisableAclEntryCount = 0;

    // Platform capability for SAI_ACL_ACTION_TYPE_SET_DO_NOT_LEARN at
    // the PRE_INGRESS stage. -1 = not yet queried, 0 = unsupported (action is
    // soft-disabled), 1 = supported. Probed once via SAI from the constructor
    // and cached for the lifetime of the process.
    int m_aclSetDoNotLearnSupported = -1;

    // Latches to true the first time a native SAI_FDB_EVENT_MOVE is observed
    // (i.e. handleMacMove is invoked from FdbOrch's MOVE path). On platforms
    // that emit native MOVE, this disables the LEARN-path synthesis in
    // handleMacLearn() — otherwise a single SAI move would count twice (once
    // via native MOVE, once via the synthesized LEARN-after-different-port).
    // Platforms that emit AGE+LEARN instead of MOVE never set this and the
    // synthesis path stays active. Intentionally not persisted across
    // restarts: it re-latches naturally on the first move after restart.
    bool m_nativeMovesSeen = false;

    // Set once the one-shot post-ports-ready reconcile (restore from STATE_DB
    // if the feature is enabled, otherwise clean up any leftover HW state)
    // has run. The recovery timer callback drives this on its first tick
    // after allPortsReady() — keeping reconcile out of the constructor so
    // PortsOrch has time to populate port OIDs from APP_DB.
    bool m_reconcileDone = false;

    // Core logic
    void handleMacMove(const MacMoveNotification &notif);
    void handleMacLearn(const MacLearnNotification &notif);
    void pruneWindow(MacMoveTrackingState &state);
    void pruneLearntMacCache();
    void markBadMac(const MacKey &key, MacMoveTrackingState &state, const std::string &portName);
    void releaseBadMac(const MacKey &key, MacMoveTrackingState &state);
    void clearAllState();
    void checkRecovery();
    void reapplyActionIntervalToBadMacs(uint32_t prev_recovery_seconds);

    // STATE_DB persistence helpers (cleanup-on-restart model — only port
    // admin-disable bookkeeping survives a restart, and only so we can revert
    // it on startup).
    void persistDisabledPorts();
    void restoreHwResources();
    bool aclTableMatchesOurSignature(sai_object_id_t table_oid) const;

    // DISABLE_LEARN_ON_MAC_WITH_ACL helpers
    void publishActionCapabilities();
    bool isAclSetDoNotLearnSupported();
    bool ensureLearnDisableAclTable();
    void destroyLearnDisableAclTable();
    void reconcileLearnDisableAclTable(bool prev_enabled, MacMoveGuardAction prev_action);
    bool installLearnDisableAclEntry(const MacKey &key, MacMoveTrackingState &state);
    void removeLearnDisableAclEntry(MacMoveTrackingState &state);

    // Pre-ingress ACL slot helpers. The bind / unbind / CRM operations on
    // SAI_SWITCH_ATTR_PRE_INGRESS_ACL appear in three callers
    // (ensureLearnDisableAclTable, destroyLearnDisableAclTable,
    // restoreHwResources); these wrappers keep the SAI get-then-set dance
    // and the CRM bookkeeping in one place.
    bool bindPreIngressAclTable(sai_object_id_t table_oid);
    bool unbindPreIngressAclTable(sai_object_id_t expected_table_oid);
    void crmPreIngressAclTableInc();
    void crmPreIngressAclTableDec(sai_object_id_t table_oid);
};

#endif  // SWSS_MACMOVEGUARD_H
