#ifndef __MONITORLINKGROUPMGR__
#define __MONITORLINKGROUPMGR__

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
        uint32_t min_uplinks;                       // Minimum uplinks needed for group UP (parsed once at SET time)
        uint32_t linkup_delay;                      // Seconds to wait after uplinks recover before bringing group UP
        std::set<std::string> uplink_interfaces;    // Set of uplink interfaces in this group
        std::set<std::string> downlink_interfaces;  // Set of downlink interfaces in this group
        uint32_t uplink_up_count;                   // Number of uplink interfaces that are up
        bool is_up;                                 // Group state: true if uplink_up_count >= threshold, false otherwise
        bool pending_up;                            // True if group is waiting for link-up-delay before going UP
        // Non-owning reference: the ExecutableTimer registered in m_consumerMap owns the underlying SelectableTimer.
        // This pointer is a borrowed reference used only for stop()/setInterval()/start() calls.
        SelectableTimer* linkup_delay_timer;
        std::chrono::steady_clock::time_point delay_start_time;  // When the delay timer was started

        MonitorLinkGroupInfo() : min_uplinks(1), linkup_delay(0), uplink_up_count(0), is_up(false), pending_up(false), linkup_delay_timer(nullptr) {}
    };

    struct MonitorLinkInterfaceInfo {
        std::set<std::string> uplink_groups;   // groups where this port is an uplink
        std::set<std::string> downlink_groups; // groups where this port is a downlink
        bool is_up;                            // current operational state
        uint32_t down_group_count;             // downlink-role groups that are DOWN/PENDING

        MonitorLinkInterfaceInfo() : is_up(false), down_group_count(0) {}

        bool in_any_group() const { return !uplink_groups.empty() || !downlink_groups.empty(); }
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
    void updateDownlinkInterfacesForGroupStateChange(const std::string& group_name, bool group_was_up, bool group_is_up);
    void writeMonitorLinkGroupMemberStateToDb(const std::string& interface_name);

    // Monitor link group state methods
    void updateMonitorLinkGroupState(const std::string& group_name, bool skip_delay = false);
    void writeMonitorLinkGroupStateToDb(const std::string& group_name);

    // SelectableTimer management for linkup delay
    void startLinkupDelayTimer(const std::string& group_name);     // Start linkup delay timer
    void stopLinkupDelayTimer(const std::string& group_name);      // Stop linkup delay timer
    void handleLinkupDelayExpired(const std::string& group_name);  // Handle timer expiration

    // Monitor link group helper methods
    bool handleMonitorLinkGroupSet(const std::string& group_name, const std::vector<FieldValueTuple>& data);
    bool handleMonitorLinkGroupDel(const std::string& group_name);
    void handleGroupDelayChange(const std::string& group_name, uint32_t new_delay);
    void updateGroupConfiguration(const std::string& group_name, const std::string& description,
                                  uint32_t min_uplinks, uint32_t linkup_delay);
    void updateGroupInterfaceLists(const std::string& group_name, const std::string& uplink_interfaces,
                                   const std::string& downlink_interfaces, bool initial_creation = false);
    void addInterfaceToGroup(const std::string& group_name, const std::string& interface_name, const std::string& link_type);
    void removeInterfaceFromGroup(const std::string& group_name, const std::string& interface_name, const std::string& link_type);
    bool getInterfaceOperState(const std::string& interface_name);
};

}

#endif
