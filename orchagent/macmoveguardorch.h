#ifndef SWSS_MACMOVEGUARDORCH_H
#define SWSS_MACMOVEGUARDORCH_H

#include "orch.h"
#include "observer.h"
#include "portsorch.h"
#include "fdborch.h"
#include "timer.h"

#include <deque>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <cstdint>

#define CFG_MAC_MOVE_GUARD_TABLE_NAME "MAC_MOVE_GUARD"

/* Action types for MAC move guard */
enum class MacMoveGuardAction
{
    DISABLE_PORT,      /* Administratively disable the port where MAC was last seen */
    DISABLE_MAC_MOVE,  /* Program MAC as static with ALLOW_MAC_MOVE=false to prevent further moves */
    /* Future actions can be added here, e.g.:
       LOG_ONLY,       Alert/log but take no action
       RATE_LIMIT,     Rate-limit traffic from this MAC
       DROP_MAC,       Drop all traffic from this MAC
    */
};

/* Identifies a MAC we're tracking: a (VLAN, MAC) pair.
   bv_id is the SAI bridge-vlan OID, which uniquely identifies the VLAN. */
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

/* Hash functor for MacKey, used by std::unordered_map. Packs the 6 MAC
   bytes into a uint64 and combines with bv_id using boost-style mixing. */
struct MacKeyHash
{
    std::size_t operator()(const MacKey &k) const noexcept
    {
        const uint8_t *m = k.mac.getMac();
        uint64_t mac_int =
            (static_cast<uint64_t>(m[0]) << 40) |
            (static_cast<uint64_t>(m[1]) << 32) |
            (static_cast<uint64_t>(m[2]) << 24) |
            (static_cast<uint64_t>(m[3]) << 16) |
            (static_cast<uint64_t>(m[4]) << 8)  |
             static_cast<uint64_t>(m[5]);
        std::size_t h1 = std::hash<uint64_t>()(mac_int);
        std::size_t h2 = std::hash<sai_object_id_t>()(k.bv_id);
        // boost::hash_combine pattern
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

/* Per-MAC tracking state: move history, bad MAC status, and action tracking. */
struct MacMoveTrackingState
{
    /* Sliding window of move timestamps (for counting moves within detect_interval) */
    std::deque<std::chrono::steady_clock::time_point> move_timestamps;

    /* Track each port and when MAC was last seen on it (for detect_interval window) */
    std::map<std::string, std::chrono::steady_clock::time_point> ports_seen;

    size_t move_count = 0;                                               /* total MAC moves within detect_interval */
    bool is_bad_mac = false;                                             /* is this MAC identified as bad? */
    std::chrono::steady_clock::time_point action_expiry_time;            /* when the action interval expires */
    std::string pinned_port;                                             /* the ONE port we keep active for this bad MAC (DISABLE_PORT action) */
    std::set<std::string> disabled_ports;                                /* all other ports we disabled for this bad MAC (DISABLE_PORT action) */
    std::string last_port;                                               /* most recent port this MAC was seen on */
    sai_object_id_t static_fdb_bridge_port_id = SAI_NULL_OBJECT_ID;     /* bridge_port_id where static FDB was programmed (DISABLE_MAC_MOVE action) */
};

class MacMoveGuardOrch : public Orch, public Observer
{
public:
    MacMoveGuardOrch(DBConnector *configDb, const std::string &tableName,
                     PortsOrch *portsOrch, FdbOrch *fdbOrch);
    ~MacMoveGuardOrch();

    /* Observer callback: receives MAC move notifications from FdbOrch */
    void update(SubjectType type, void *cntx) override;

private:
    PortsOrch *m_portsOrch;
    FdbOrch *m_fdbOrch;

    /* Configuration. Defaults applied when the feature is enabled. */
    bool m_enabled = false;
    uint32_t m_threshold = 10000;                      /* max mac moves allowed in window */
    uint32_t m_durationSeconds = 5;                    /* detect_interval: sliding window in seconds */
    uint32_t m_recoverySeconds = 120;                  /* action_interval: recovery period in seconds */
    MacMoveGuardAction m_action = MacMoveGuardAction::DISABLE_PORT;  /* action to take on bad MAC */

    /* Per-MAC move tracking state, keyed by (mac, bv_id). */
    std::map<MacKey, MacMoveTrackingState> m_macTrackingState;

    /* For each port we have admin-disabled, the set of bad MACs currently
       requiring it to be disabled. The port is re-enabled only when the set becomes empty. */
    std::map<std::string, std::set<MacKey>> m_disabledPorts;

    /* Last-known port for every (mac, bv_id) we have seen LEARNED. We never
       erase on AGED — a subsequent LEARN on a different port is recognized as
       a move via port comparison. Bounded in steady state by SDK MAC-table size. */
    std::unordered_map<MacKey, std::string, MacKeyHash> m_learntMac;

    /* Recovery timer: fires periodically to check recovery conditions. */
    swss::SelectableTimer *m_recoveryTimer = nullptr;
    static const int RECOVERY_CHECK_INTERVAL_SECS = 30;

    /* Config DB task handler */
    void doTask(Consumer &consumer) override;

    /* Recovery timer handler */
    void doTask(swss::SelectableTimer &timer) override;

    /* Core logic */
    void handleMacMove(const MacMoveNotification &notif);
    void handleMacLearn(const MacLearnNotification &notif);
    void pruneWindow(MacMoveTrackingState &state);
    void markBadMac(const MacKey &key, MacMoveTrackingState &state, const std::string &portName);
    void releaseBadMac(const MacKey &key, MacMoveTrackingState &state);
    void clearAllState();
    void checkRecovery();
};

#endif /* SWSS_MACMOVEGUARDORCH_H */
