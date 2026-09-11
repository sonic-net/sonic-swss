#ifndef __MONITORLINKGROUPMGR__
#define __MONITORLINKGROUPMGR__

// Monitor Link Group table-name macros are added to swss-common by
// sonic-net/sonic-swss-common#1181 (STATE_*) and generated from YANG in
// sonic-net/sonic-buildimage#27004 (CFG_*). Until both land, the
// sonic-swss-common artifact pulled by CI does not carry these macros and
// this swss build fails to compile. Define here as a transitional shim with
// #ifndef guards so the build succeeds standalone. Once the dependencies
// merge and schema.h / cfg_schema.h carry these macros, the #ifndef guards
// skip the fallback and the upstream definitions take over; this block can
// then be removed as a cleanup.
#ifndef CFG_MONITOR_LINK_GROUP_TABLE_NAME
#define CFG_MONITOR_LINK_GROUP_TABLE_NAME          "MONITOR_LINK_GROUP"
#endif
#ifndef STATE_MONITOR_LINK_GROUP_STATE_TABLE_NAME
#define STATE_MONITOR_LINK_GROUP_STATE_TABLE_NAME  "MONITOR_LINK_GROUP_STATE"
#endif
#ifndef STATE_MONITOR_LINK_GROUP_MEMBER_TABLE_NAME
#define STATE_MONITOR_LINK_GROUP_MEMBER_TABLE_NAME "MONITOR_LINK_GROUP_MEMBER"
#endif

#include "dbconnector.h"
#include "orch.h"
#include "selectabletimer.h"

#include <map>
#include <set>
#include <string>
#include <chrono>

namespace swss {

class MonitorLinkGroupMgr : public Orch
{
public:
    MonitorLinkGroupMgr(DBConnector *cfgDb, DBConnector *stateDb, const std::vector<std::string> &tableNames);
    using Orch::doTask;
    void doTask(SelectableTimer &timer) override;

private:
    Table m_cfgMonitorLinkGroupTable;
    Table m_statePortTable;
    Table m_stateLagTable;
    Table m_stateMonitorLinkGroupTable;
    Table m_stateMonitorLinkGroupMemberTable;

    // Monitor link group and interface mapping storage
    struct MonitorLinkGroupInfo {
        std::string description;
        uint32_t min_monitored_links;                       // Minimum monitored-links needed for group UP (parsed once at SET time)
        uint32_t linkup_delay;                      // Seconds to wait after monitored-links recover before bringing group UP
        std::set<std::string> monitored_interfaces;    // Set of monitored interfaces in this group
        std::set<std::string> managed_interfaces;  // Set of managed interfaces in this group
        uint32_t monitored_up_count;                   // Number of monitored interfaces that are up
        bool is_up;                                 // Group state: true if monitored_up_count >= threshold, false otherwise
        bool pending_up;                            // True if group is waiting for link-up-delay before going UP
        // Non-owning reference: the ExecutableTimer registered in m_consumerMap owns the underlying SelectableTimer.
        // This pointer is a borrowed reference used only for stop()/setInterval()/start() calls.
        SelectableTimer* linkup_delay_timer;
        std::chrono::steady_clock::time_point delay_start_time;  // When the delay timer was started

        // PR-A: transition tracking. Operators can answer "when did this last change?",
        // "how far into the link-up-delay are we?", and "is this flapping?" by reading
        // STATE_DB without grepping syslog.
        bool has_state_change;                                         // false until first transition observed
        std::string last_state_change_from;                            // "up" | "down" | "pending"
        std::string last_state_change_to;                              // "up" | "down" | "pending"
        std::chrono::system_clock::time_point last_state_change_time;  // wall-clock of last transition
        std::chrono::system_clock::time_point pending_start_time;      // wall-clock when current PENDING started
        uint64_t total_transitions;                                    // count of all recorded transitions

        MonitorLinkGroupInfo()
            : min_monitored_links(1), linkup_delay(0), monitored_up_count(0),
              is_up(false), pending_up(false), linkup_delay_timer(nullptr),
              has_state_change(false), total_transitions(0) {}
    };

    struct MonitorLinkInterfaceInfo {
        std::set<std::string> monitored_groups;   // groups where this port is a monitored-link
        std::set<std::string> managed_groups; // groups where this port is a managed-link
        bool is_up;                            // current operational state
        uint32_t down_group_count;             // managed-role groups that are DOWN/PENDING

        MonitorLinkInterfaceInfo() : is_up(false), down_group_count(0) {}

        bool in_any_group() const { return !monitored_groups.empty() || !managed_groups.empty(); }
    };

    // Group name -> Group info mapping
    std::map<std::string, MonitorLinkGroupInfo> m_monitorLinkGroups;
    // Interface name -> Interface info mapping
    std::map<std::string, MonitorLinkInterfaceInfo> m_monitorLinkInterfaces;
    // Timer pointer -> Group name reverse map for O(1) timer expiry lookup
    std::map<SelectableTimer*, std::string> m_timerToGroup;

    void doTask(Consumer &consumer);
    void doPortTableTask(const std::string& key, std::vector<FieldValueTuple> data, std::string op);
    void doMonitorLinkGroupTask(const std::vector<std::string>& keys, const std::vector<FieldValueTuple>& data, const std::string& op);

    // Monitor link interface state methods
    bool updateMonitorLinkInterfaceState(const std::string& interface_name, bool is_up);
    void updateManagedInterfacesForGroupStateChange(const std::string& group_name, bool group_was_up, bool group_is_up);
    void writeMonitorLinkGroupMemberStateToDb(const std::string& interface_name);

    // Monitor link group state methods
    void updateMonitorLinkGroupState(const std::string& group_name, bool skip_delay = false);
    void writeMonitorLinkGroupStateToDb(const std::string& group_name);

    // PR-A: record a state transition (from -> to) on the group. Updates
    // last_state_change_{from,to,time}, sets pending_start_time when entering
    // PENDING, and bumps total_transitions. No-op when from == to. Emits
    // SWSS_LOG_NOTICE.
    void recordStateTransition(const std::string& group_name,
                               const std::string& from_state,
                               const std::string& to_state);

    // PR-A: on first sighting of a group in this process, reload total_transitions
    // from STATE_DB so the counter survives intfmgrd restart. Leaves the counter
    // at 0 if the STATE_DB field is absent or malformed.
    void restoreGroupCountersFromStateDb(const std::string& group_name);

    // SelectableTimer management for linkup delay
    void startLinkupDelayTimer(const std::string& group_name);     // Start linkup delay timer
    void stopLinkupDelayTimer(const std::string& group_name);      // Stop linkup delay timer
    void handleLinkupDelayExpired(const std::string& group_name);  // Handle timer expiration

    // Monitor link group helper methods
    bool handleMonitorLinkGroupSet(const std::string& group_name, const std::vector<FieldValueTuple>& data);
    bool handleMonitorLinkGroupDel(const std::string& group_name);
    void handleGroupDelayChange(const std::string& group_name, uint32_t new_delay);
    void updateGroupConfiguration(const std::string& group_name, const std::string& description,
                                  uint32_t min_monitored_links, uint32_t linkup_delay);
    void updateGroupInterfaceLists(const std::string& group_name, const std::string& monitored_interfaces,
                                   const std::string& managed_interfaces, bool initial_creation = false);
    void addInterfaceToGroup(const std::string& group_name, const std::string& interface_name, const std::string& link_type);
    void removeInterfaceFromGroup(const std::string& group_name, const std::string& interface_name, const std::string& link_type);
    bool getInterfaceOperState(const std::string& interface_name);

    // R-6: detect dependency cycles introduced by a proposed group config.
    // Edge G -> H: some port in G.monitored is in H.managed (G waits on H to be UP).
    // A cycle implies a deadlock recovery scenario; we reject the SET.
    bool wouldCreateDependencyCycle(const std::string& group_name,
                                    const std::set<std::string>& monitored,
                                    const std::set<std::string>& managed);

    // Parse a comma-separated interface list into a set (trims whitespace, drops empties).
    static std::set<std::string> parseInterfaceList(const std::string& csv);
};

}

#endif
