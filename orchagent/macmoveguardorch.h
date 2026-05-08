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

/* Identifies a flow whose moves we count: a (VLAN, MAC) pair.
   bv_id is the SAI bridge-vlan OID, which uniquely identifies the VLAN. */
struct MacFlowKey
{
    MacAddress mac;
    sai_object_id_t bv_id;

    bool operator<(const MacFlowKey &other) const
    {
        return std::tie(mac, bv_id) < std::tie(other.mac, other.bv_id);
    }

    bool operator==(const MacFlowKey &other) const
    {
        return std::tie(mac, bv_id) == std::tie(other.mac, other.bv_id);
    }
};

/* Hash functor for MacFlowKey, used by std::unordered_map. Packs the 6 MAC
   bytes into a uint64 and combines with bv_id using boost-style mixing. */
struct MacFlowKeyHash
{
    std::size_t operator()(const MacFlowKey &k) const noexcept
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

/* Per-flow state: sliding window of move timestamps and any disable bookkeeping. */
struct MacFlowState
{
    std::deque<std::chrono::steady_clock::time_point> move_timestamps;
    bool port_disabled = false;                              /* did this flow trigger a port-disable? */
    std::string disabled_port_name;                          /* port we asked PortsOrch to disable */
    std::chrono::steady_clock::time_point disable_time;      /* when we issued the disable */
};

class MacMoveGuardOrch : public Orch, public Observer
{
public:
    MacMoveGuardOrch(DBConnector *configDb, const std::string &tableName,
                     PortsOrch *portsOrch);
    ~MacMoveGuardOrch();

    /* Observer callback: receives MAC move notifications from FdbOrch */
    void update(SubjectType type, void *cntx) override;

private:
    PortsOrch *m_portsOrch;

    /* Configuration. Defaults applied when the feature is enabled. */
    bool m_enabled = false;
    uint32_t m_threshold = 10000;     /* max mac moves allowed in window */
    uint32_t m_durationSeconds = 5;   /* sliding window in seconds */
    uint32_t m_recoverySeconds = 120; /* recovery period in seconds */

    /* Per-flow move tracking, keyed by (mac, bv_id). */
    std::map<MacFlowKey, MacFlowState> m_flowState;

    /* For each port we have admin-disabled, the set of flows currently
       holding it down. The port is re-enabled only when the set becomes empty. */
    std::map<std::string, std::set<MacFlowKey>> m_disabledPortFlows;

    /* Last-known port for every (mac, bv_id) we have seen LEARNED. We never
       erase on AGED — a subsequent LEARN on a different port is recognized as
       a move via port comparison. Bounded in steady state by SDK MAC-table size. */
    std::unordered_map<MacFlowKey, std::string, MacFlowKeyHash> m_learntMac;

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
    void pruneWindow(MacFlowState &state);
    void disablePort(const std::string &portName, const MacFlowKey &key, MacFlowState &state);
    void releaseFlowFromPort(const MacFlowKey &key, MacFlowState &state);
    void clearAllState();
    void checkRecovery();
};

#endif /* SWSS_MACMOVEGUARDORCH_H */
