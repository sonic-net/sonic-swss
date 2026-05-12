#include "logger.h"
#include "schema.h"
#include "tokenize.h"
#include "subscriberstatetable.h"
#include "timer.h"
#include "monitorlinkgroupmgr.h"

#include <sstream>
#include <functional>

using namespace std;
using namespace swss;

MonitorLinkGroupMgr::MonitorLinkGroupMgr(DBConnector *cfgDb, DBConnector *stateDb, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_cfgMonitorLinkGroupTable(cfgDb, CFG_MONITOR_LINK_GROUP_TABLE_NAME),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME),
        m_stateLagTable(stateDb, STATE_LAG_TABLE_NAME),
        m_stateMonitorLinkGroupTable(stateDb, STATE_MONITOR_LINK_GROUP_STATE_TABLE_NAME),
        m_stateMonitorLinkGroupMemberTable(stateDb, STATE_MONITOR_LINK_GROUP_MEMBER_TABLE_NAME)
{
    auto subscriberStateTable = new swss::SubscriberStateTable(stateDb,
            STATE_PORT_TABLE_NAME, TableConsumable::DEFAULT_POP_BATCH_SIZE, 100);
    auto stateConsumer = new Consumer(subscriberStateTable, this, STATE_PORT_TABLE_NAME);
    Orch::addExecutor(stateConsumer);

    auto subscriberStateLagTable = new swss::SubscriberStateTable(stateDb,
            STATE_LAG_TABLE_NAME, TableConsumable::DEFAULT_POP_BATCH_SIZE, 200);
    auto stateLagConsumer = new Consumer(subscriberStateLagTable, this, STATE_LAG_TABLE_NAME);
    Orch::addExecutor(stateLagConsumer);
}

void MonitorLinkGroupMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        if ((table_name == STATE_PORT_TABLE_NAME) || (table_name == STATE_LAG_TABLE_NAME))
        {
            doPortTableTask(kfvKey(t), kfvFieldsValues(t), kfvOp(t));
        }
        else if (table_name == CFG_MONITOR_LINK_GROUP_TABLE_NAME)
        {
            vector<string> keys = tokenize(kfvKey(t), config_db_key_delimiter);
            const vector<FieldValueTuple>& data = kfvFieldsValues(t);
            string op = kfvOp(t);

            if (keys.size() == 1)
            {
                doMonitorLinkGroupTask(keys, data, op);
            }
            else
            {
                SWSS_LOG_ERROR("Invalid key %s", kfvKey(t).c_str());
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

void MonitorLinkGroupMgr::doPortTableTask(const string& key, vector<FieldValueTuple> data, string op)
{
    if (op == SET_COMMAND)
    {
        string oper_status_val, netdev_oper_status_val;
        for (auto idx : data)
        {
            const auto &field = fvField(idx);
            const auto &value = fvValue(idx);
            if (field == "oper_status")
                oper_status_val = value;
            else if (field == "netdev_oper_status")
                netdev_oper_status_val = value;
        }
        // netdev_oper_status is the authoritative kernel-netdev state for Ethernet;
        // oper_status is the LAG operational state for PortChannels.
        if (!netdev_oper_status_val.empty())
            updateMonitorLinkInterfaceState(key, netdev_oper_status_val == "up");
        else if (!oper_status_val.empty())
            updateMonitorLinkInterfaceState(key, oper_status_val == "up");
    }
    else if (op == DEL_COMMAND)
    {
        updateMonitorLinkInterfaceState(key, false);
    }
}

void MonitorLinkGroupMgr::doMonitorLinkGroupTask(const vector<string>& keys, const vector<FieldValueTuple>& data, const string& op)
{
    SWSS_LOG_ENTER();

    string group_name = keys[0];

    SWSS_LOG_INFO("Processing monitor link group: %s, operation: %s", group_name.c_str(), op.c_str());

    if (op == SET_COMMAND)
    {
        handleMonitorLinkGroupSet(group_name, data);
    }
    else if (op == DEL_COMMAND)
    {
        handleMonitorLinkGroupDel(group_name);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown operation: %s for monitor link group", op.c_str());
    }
}

bool MonitorLinkGroupMgr::handleMonitorLinkGroupSet(const string& group_name, const vector<FieldValueTuple>& data)
{
    string description;
    uint32_t min_monitored_links = 1;
    uint32_t linkup_delay = 0;
    string monitored_interfaces;
    string managed_interfaces;

    for (auto idx : data)
    {
        const auto &field = fvField(idx);
        const auto &value = fvValue(idx);

        if (field == "description")
        {
            description = value;
        }
        else if (field == "min-monitored-links")
        {
            try
            {
                min_monitored_links = static_cast<uint32_t>(std::stoul(value));
            }
            catch (const std::exception&)
            {
                SWSS_LOG_WARN("Monitor link group %s: invalid min-monitored-links '%s', using 1",
                              group_name.c_str(), value.c_str());
                min_monitored_links = 1;
            }
        }
        else if (field == "link-up-delay")
        {
            try
            {
                linkup_delay = static_cast<uint32_t>(std::stoul(value));
            }
            catch (const std::exception&)
            {
                SWSS_LOG_WARN("Monitor link group %s: invalid link-up-delay '%s', using 0",
                              group_name.c_str(), value.c_str());
                linkup_delay = 0;
            }
        }
        else if (field == "monitored-links")
        {
            monitored_interfaces = value;
        }
        else if (field == "managed-links")
        {
            managed_interfaces = value;
        }
    }

    SWSS_LOG_INFO("Monitor link group %s: description='%s', min-monitored-links=%u, link-up-delay=%u, monitored-links='%s', managed-links='%s'",
                  group_name.c_str(), description.c_str(), min_monitored_links, linkup_delay,
                  monitored_interfaces.c_str(), managed_interfaces.c_str());

    // R-6: reject configs that would form a dependency cycle between groups.
    // Edge G -> H: a port in G.monitored is in H.managed (G's recovery waits on H).
    // A cycle means the involved groups deadlock with no path to recovery.
    set<string> monitored_set = parseInterfaceList(monitored_interfaces);
    set<string> managed_set = parseInterfaceList(managed_interfaces);
    if (wouldCreateDependencyCycle(group_name, monitored_set, managed_set))
    {
        SWSS_LOG_ERROR("Monitor link group %s rejected: monitored/managed interface lists "
                       "would form a dependency cycle with existing groups",
                       group_name.c_str());
        return false;
    }

    bool is_new_group = (m_monitorLinkGroups.find(group_name) == m_monitorLinkGroups.end());

    if (!is_new_group)
    {
        auto& existingGroup = m_monitorLinkGroups[group_name];
        if (existingGroup.linkup_delay != linkup_delay)
            handleGroupDelayChange(group_name, linkup_delay);
    }

    updateGroupConfiguration(group_name, description, min_monitored_links, linkup_delay);
    updateGroupInterfaceLists(group_name, monitored_interfaces, managed_interfaces, is_new_group);
    return true;
}

set<string> MonitorLinkGroupMgr::parseInterfaceList(const string& csv)
{
    set<string> result;
    if (csv.empty()) return result;
    stringstream ss(csv);
    string token;
    while (std::getline(ss, token, ','))
    {
        // Trim ASCII whitespace
        auto a = token.find_first_not_of(" \t");
        if (a == string::npos) continue;
        auto b = token.find_last_not_of(" \t");
        result.insert(token.substr(a, b - a + 1));
    }
    return result;
}

bool MonitorLinkGroupMgr::wouldCreateDependencyCycle(const string& group_name,
                                                     const set<string>& monitored,
                                                     const set<string>& managed)
{
    // Build a directed graph over groups: edge G -> H iff some port appears in
    // G.monitored AND H.managed. Cycle in this graph == deadlock recovery scenario.
    //
    // The existing m_monitorLinkGroups is acyclic by induction (we reject SETs that
    // would introduce a cycle). So any newly introduced cycle must involve `group_name`.
    // DFS from `group_name`; if we revisit `group_name` via a back-edge, reject.

    // Precompute port -> set of groups in which it is a managed-link.
    // For `group_name`, use the proposed `managed` set (not its current state if any).
    std::map<string, set<string>> port_to_managed_groups;
    for (const auto& kv : m_monitorLinkGroups)
    {
        if (kv.first == group_name) continue;  // exclude old version of the changing group
        for (const auto& port : kv.second.managed_interfaces)
            port_to_managed_groups[port].insert(kv.first);
    }
    for (const auto& port : managed)
        port_to_managed_groups[port].insert(group_name);

    set<string> visited;
    set<string> on_stack;

    std::function<bool(const string&)> dfs = [&](const string& g) -> bool
    {
        if (on_stack.count(g)) return true;   // back-edge: cycle
        if (visited.count(g)) return false;
        on_stack.insert(g);

        // For each port in g's monitored set, follow edges to groups that manage it.
        const set<string>& g_monitored = (g == group_name)
            ? monitored
            : m_monitorLinkGroups.at(g).monitored_interfaces;
        for (const auto& port : g_monitored)
        {
            auto it = port_to_managed_groups.find(port);
            if (it == port_to_managed_groups.end()) continue;
            for (const auto& h : it->second)
            {
                if (h == g) continue;  // self-loop on a single port is also a cycle, but
                                       // we treat that as a config-disjointness violation
                                       // (covered by the YANG must constraint).
                if (dfs(h)) return true;
            }
        }

        on_stack.erase(g);
        visited.insert(g);
        return false;
    };

    return dfs(group_name);
}

void MonitorLinkGroupMgr::handleGroupDelayChange(const string& group_name, uint32_t new_delay)
{
    auto& existingGroup = m_monitorLinkGroups[group_name];
    SWSS_LOG_INFO("Monitor link group %s link-up-delay changed from %u to %u",
                  group_name.c_str(), existingGroup.linkup_delay, new_delay);

    if (existingGroup.pending_up)
    {
        if (new_delay == 0)
        {
            SWSS_LOG_INFO("Monitor link group %s delay changed to 0, bringing UP immediately", group_name.c_str());
            stopLinkupDelayTimer(group_name);
            existingGroup.pending_up = false;
            existingGroup.is_up = true;
            updateManagedInterfacesForGroupStateChange(group_name, false, true);
            writeMonitorLinkGroupStateToDb(group_name);
        }
        else
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                now - existingGroup.delay_start_time).count();

            if (elapsed_seconds >= new_delay)
            {
                SWSS_LOG_INFO("Monitor link group %s delay reduced to %u but %ld seconds already elapsed, bringing UP immediately",
                              group_name.c_str(), new_delay, elapsed_seconds);
                stopLinkupDelayTimer(group_name);
                existingGroup.pending_up = false;
                existingGroup.is_up = true;
                updateManagedInterfacesForGroupStateChange(group_name, false, true);
                writeMonitorLinkGroupStateToDb(group_name);
            }
            else
            {
                uint32_t remaining_seconds = new_delay - static_cast<uint32_t>(elapsed_seconds);
                SWSS_LOG_INFO("Monitor link group %s delay reduced, restarting timer with %u seconds remaining (new delay: %u, elapsed: %ld)",
                              group_name.c_str(), remaining_seconds, new_delay, elapsed_seconds);

                stopLinkupDelayTimer(group_name);

                if (existingGroup.linkup_delay_timer != nullptr)
                {
                    auto interv = timespec { .tv_sec = static_cast<time_t>(remaining_seconds), .tv_nsec = 0 };
                    existingGroup.linkup_delay_timer->setInterval(interv);
                    existingGroup.linkup_delay_timer->start();
                    // Keep original delay_start_time so future reductions work correctly
                    SWSS_LOG_DEBUG("MonitorLinkGroupMgr: Started timer with %u seconds for group %s", remaining_seconds, group_name.c_str());
                }
            }
        }
    }
}

void MonitorLinkGroupMgr::updateGroupConfiguration(const string& group_name, const string& description,
                                       uint32_t min_monitored_links, uint32_t linkup_delay)
{
    MonitorLinkGroupInfo& groupInfo = m_monitorLinkGroups[group_name];
    groupInfo.description = description;
    groupInfo.min_monitored_links = min_monitored_links;
    groupInfo.linkup_delay = linkup_delay;

    SWSS_LOG_NOTICE("Monitor link group %s configured successfully (total groups: %zu)",
                    group_name.c_str(), m_monitorLinkGroups.size());
}

void MonitorLinkGroupMgr::updateGroupInterfaceLists(const string& group_name, const string& monitored_interfaces, const string& managed_interfaces, bool initial_creation)
{
    MonitorLinkGroupInfo& groupInfo = m_monitorLinkGroups[group_name];

    std::set<std::string> new_monitored_interfaces;
    if (!monitored_interfaces.empty())
    {
        std::stringstream ss(monitored_interfaces);
        std::string interface;
        while (std::getline(ss, interface, ','))
        {
            interface.erase(0, interface.find_first_not_of(" \t"));
            interface.erase(interface.find_last_not_of(" \t") + 1);
            if (!interface.empty())
            {
                new_monitored_interfaces.insert(interface);
            }
        }
    }

    std::set<std::string> new_managed_interfaces;
    if (!managed_interfaces.empty())
    {
        std::stringstream ss(managed_interfaces);
        std::string interface;
        while (std::getline(ss, interface, ','))
        {
            interface.erase(0, interface.find_first_not_of(" \t"));
            interface.erase(interface.find_last_not_of(" \t") + 1);
            if (!interface.empty())
            {
                new_managed_interfaces.insert(interface);
            }
        }
    }

    // Create a copy to avoid iterator invalidation when removeInterfaceFromGroup modifies the set
    std::set<std::string> current_monitored_interfaces = groupInfo.monitored_interfaces;
    for (const auto& interface : current_monitored_interfaces)
    {
        if (new_monitored_interfaces.find(interface) == new_monitored_interfaces.end())
        {
            removeInterfaceFromGroup(group_name, interface, "monitored");
        }
    }

    // Create a copy to avoid iterator invalidation when removeInterfaceFromGroup modifies the set
    std::set<std::string> current_managed_interfaces = groupInfo.managed_interfaces;
    for (const auto& interface : current_managed_interfaces)
    {
        if (new_managed_interfaces.find(interface) == new_managed_interfaces.end())
        {
            removeInterfaceFromGroup(group_name, interface, "managed");
        }
    }

    for (const auto& interface : new_monitored_interfaces)
    {
        if (groupInfo.monitored_interfaces.find(interface) == groupInfo.monitored_interfaces.end())
        {
            addInterfaceToGroup(group_name, interface, "monitored");
        }
    }

    for (const auto& interface : new_managed_interfaces)
    {
        if (groupInfo.managed_interfaces.find(interface) == groupInfo.managed_interfaces.end())
        {
            addInterfaceToGroup(group_name, interface, "managed");
        }
    }

    groupInfo.monitored_interfaces = new_monitored_interfaces;
    groupInfo.managed_interfaces = new_managed_interfaces;

    SWSS_LOG_INFO("Monitor link group %s interface lists updated: %zu monitored, %zu managed",
                  group_name.c_str(), new_monitored_interfaces.size(), new_managed_interfaces.size());

    updateMonitorLinkGroupState(group_name, initial_creation);
}

bool MonitorLinkGroupMgr::handleMonitorLinkGroupDel(const string& group_name)
{
    SWSS_LOG_INFO("Deleting monitor link group: %s", group_name.c_str());

    auto groupIt = m_monitorLinkGroups.find(group_name);
    if (groupIt != m_monitorLinkGroups.end())
    {
        uint32_t removed_monitored = static_cast<uint32_t>(groupIt->second.monitored_interfaces.size());
        uint32_t removed_managed = static_cast<uint32_t>(groupIt->second.managed_interfaces.size());
        uint32_t removed_total = removed_monitored + removed_managed;

        // Remove monitored-link interfaces: erase this group from their monitored_groups;
        // only delete the interface entry if it belongs to no group at all anymore.
        for (const auto& interface_name : groupIt->second.monitored_interfaces)
        {
            auto interfaceIt = m_monitorLinkInterfaces.find(interface_name);
            if (interfaceIt == m_monitorLinkInterfaces.end()) continue;
            interfaceIt->second.monitored_groups.erase(group_name);
            if (!interfaceIt->second.in_any_group())
                m_monitorLinkInterfaces.erase(interfaceIt);
            SWSS_LOG_INFO("Removed monitored-link interface %s from deleted group %s",
                          interface_name.c_str(), group_name.c_str());
        }

        // Remove managed-link interfaces: erase this group from their managed_groups;
        // delete STATE_DB entry and interface entry only if no groups remain.
        for (const auto& interface_name : groupIt->second.managed_interfaces)
        {
            auto interfaceIt = m_monitorLinkInterfaces.find(interface_name);
            if (interfaceIt != m_monitorLinkInterfaces.end())
            {
                interfaceIt->second.managed_groups.erase(group_name);

                bool group_is_up = groupIt->second.is_up && !groupIt->second.pending_up;
                if (!group_is_up && interfaceIt->second.down_group_count > 0)
                {
                    interfaceIt->second.down_group_count--;
                }

                if (!interfaceIt->second.in_any_group())
                {
                    m_stateMonitorLinkGroupMemberTable.del(interface_name);
                    SWSS_LOG_NOTICE("Removed STATE_DB entry for managed-link interface %s to restore config state",
                                    interface_name.c_str());
                    m_monitorLinkInterfaces.erase(interfaceIt);
                }
                else
                {
                    writeMonitorLinkGroupMemberStateToDb(interface_name);
                    SWSS_LOG_INFO("Updated managed-link interface %s state (still in %zu other groups)",
                                  interface_name.c_str(),
                                  interfaceIt->second.monitored_groups.size() + interfaceIt->second.managed_groups.size());
                }
            }
            SWSS_LOG_INFO("Removed managed-link interface %s from deleted group %s",
                          interface_name.c_str(), group_name.c_str());
        }

        stopLinkupDelayTimer(group_name);

        // Note: We do NOT erase the executor from m_consumerMap here.
        // The executor/timer remains registered in the Select loop.
        // If the group is recreated, startLinkupDelayTimer() will recover
        // the timer pointer from the existing executor and reuse it.
        // This avoids issues with fd reuse where the new timer's fd might
        // conflict with the old entry in Select's m_objects map.
        if (groupIt->second.linkup_delay_timer != nullptr)
        {
            SWSS_LOG_NOTICE("MonitorLinkGroupMgr: Keeping executor for deleted group %s (timer stopped, will be reused if group is recreated)",
                            group_name.c_str());
            m_timerToGroup.erase(groupIt->second.linkup_delay_timer);
            groupIt->second.linkup_delay_timer = nullptr;
        }

        m_monitorLinkGroups.erase(groupIt);
        m_stateMonitorLinkGroupTable.del(group_name);

        SWSS_LOG_NOTICE("Monitor link group %s deleted successfully (removed %u monitored, %u managed, %u total interfaces, remaining groups: %zu)",
                        group_name.c_str(), removed_monitored, removed_managed, removed_total, m_monitorLinkGroups.size());
    }
    else
    {
        SWSS_LOG_WARN("Attempted to delete non-existent monitor link group: %s", group_name.c_str());
    }

    return true;
}

bool MonitorLinkGroupMgr::getInterfaceOperState(const string& interface_name)
{
    vector<FieldValueTuple> interface_values;
    bool interface_exists_in_state = false;

    interface_exists_in_state = m_statePortTable.get(interface_name, interface_values);
    if (!interface_exists_in_state)
        interface_exists_in_state = m_stateLagTable.get(interface_name, interface_values);

    if (!interface_exists_in_state)
        return false;

    string oper_val, netdev_oper_val;
    for (const auto& fv : interface_values)
    {
        if (fvField(fv) == "oper_status")
            oper_val = fvValue(fv);
        else if (fvField(fv) == "netdev_oper_status")
            netdev_oper_val = fvValue(fv);
    }
    if (!netdev_oper_val.empty())
        return netdev_oper_val == "up";
    if (!oper_val.empty())
        return oper_val == "up";
    return false;
}

void MonitorLinkGroupMgr::addInterfaceToGroup(const string& group_name, const string& interface_name, const string& link_type)
{
    SWSS_LOG_INFO("Adding interface %s as %s to group %s", interface_name.c_str(), link_type.c_str(), group_name.c_str());

    bool interface_exists = (m_monitorLinkInterfaces.find(interface_name) != m_monitorLinkInterfaces.end());
    MonitorLinkInterfaceInfo& interfaceInfo = m_monitorLinkInterfaces[interface_name];

    if (!interface_exists)
    {
        interfaceInfo.is_up = getInterfaceOperState(interface_name);
        SWSS_LOG_INFO("Monitor link interface %s initial state: %s",
                      interface_name.c_str(), interfaceInfo.is_up ? "up" : "down");
    }

    MonitorLinkGroupInfo& groupInfo = m_monitorLinkGroups[group_name];
    if (link_type == "monitored")
    {
        interfaceInfo.monitored_groups.insert(group_name);
        groupInfo.monitored_interfaces.insert(interface_name);
        if (interfaceInfo.is_up)
        {
            groupInfo.monitored_up_count++;
        }
    }
    else if (link_type == "managed")
    {
        interfaceInfo.managed_groups.insert(group_name);
        groupInfo.managed_interfaces.insert(interface_name);

        bool group_is_up = groupInfo.is_up && !groupInfo.pending_up;
        if (!group_is_up)
        {
            interfaceInfo.down_group_count++;
        }

        writeMonitorLinkGroupMemberStateToDb(interface_name);
    }

    SWSS_LOG_INFO("Added interface %s (%s) to group %s (state: %s, %u monitored up)",
                  interface_name.c_str(), link_type.c_str(), group_name.c_str(),
                  interfaceInfo.is_up ? "up" : "down", groupInfo.monitored_up_count);
}

void MonitorLinkGroupMgr::removeInterfaceFromGroup(const string& group_name, const string& interface_name, const string& link_type)
{
    SWSS_LOG_INFO("Removing interface %s (%s) from group %s", interface_name.c_str(), link_type.c_str(), group_name.c_str());

    auto interfaceIt = m_monitorLinkInterfaces.find(interface_name);
    if (interfaceIt == m_monitorLinkInterfaces.end())
    {
        SWSS_LOG_WARN("Attempted to remove non-existent monitor link interface: %s", interface_name.c_str());
        return;
    }

    // Verify the interface belongs to the specified group in the specified role
    bool member = (link_type == "monitored")
        ? interfaceIt->second.monitored_groups.count(group_name) > 0
        : interfaceIt->second.managed_groups.count(group_name) > 0;
    if (!member)
    {
        SWSS_LOG_WARN("Interface %s does not belong to group %s as %s. Cannot remove.",
                       interface_name.c_str(), group_name.c_str(), link_type.c_str());
        return;
    }

    auto groupIt = m_monitorLinkGroups.find(group_name);
    if (groupIt != m_monitorLinkGroups.end())
    {
        if (link_type == "monitored")
        {
            groupIt->second.monitored_interfaces.erase(interface_name);
            // Decrement up count if interface was up
            if (interfaceIt->second.is_up && groupIt->second.monitored_up_count > 0)
            {
                groupIt->second.monitored_up_count--;
                SWSS_LOG_INFO("Decremented monitored-link count for group %s to %u after removing interface %s",
                              group_name.c_str(), groupIt->second.monitored_up_count, interface_name.c_str());
            }
        }
        else if (link_type == "managed")
        {
            groupIt->second.managed_interfaces.erase(interface_name);

            // Decrement down_group_count if this group was blocking the interface
            bool group_is_up = groupIt->second.is_up && !groupIt->second.pending_up;
            if (!group_is_up && interfaceIt->second.down_group_count > 0)
            {
                interfaceIt->second.down_group_count--;
            }
        }

    }

    if (link_type == "monitored")
        interfaceIt->second.monitored_groups.erase(group_name);
    else
        interfaceIt->second.managed_groups.erase(group_name);

    if (link_type == "managed")
    {
        writeMonitorLinkGroupMemberStateToDb(interface_name);
    }

    if (!interfaceIt->second.in_any_group())
    {
        m_stateMonitorLinkGroupMemberTable.del(interface_name);
        m_monitorLinkInterfaces.erase(interfaceIt);
        SWSS_LOG_INFO("Monitor link interface %s removed from all groups", interface_name.c_str());
    }
    else
    {
        size_t remaining = interfaceIt->second.monitored_groups.size() + interfaceIt->second.managed_groups.size();
        SWSS_LOG_INFO("Monitor link interface %s removed from group %s (still in %zu other groups)",
                      interface_name.c_str(), group_name.c_str(), remaining);
    }
}

void MonitorLinkGroupMgr::updateMonitorLinkGroupState(const std::string& group_name, bool skip_delay)
{
    auto groupIt = m_monitorLinkGroups.find(group_name);
    if (groupIt == m_monitorLinkGroups.end())
    {
        SWSS_LOG_INFO("Group %s not found, cannot update state", group_name.c_str());
        return;
    }

    uint32_t threshold = groupIt->second.min_monitored_links;
    uint32_t delay_seconds = groupIt->second.linkup_delay;

    bool should_be_up = (groupIt->second.monitored_up_count >= threshold);
    bool current_state = groupIt->second.is_up;
    bool is_pending = groupIt->second.pending_up;

    if (should_be_up && !current_state && !is_pending)
    {
        if (delay_seconds > 0 && !skip_delay)
        {
            groupIt->second.pending_up = true;
            updateManagedInterfacesForGroupStateChange(group_name, false, false);
            startLinkupDelayTimer(group_name);
            SWSS_LOG_NOTICE("Monitor link group %s starting %u second linkup delay before going UP (%u monitored up, threshold: %u)",
                            group_name.c_str(), delay_seconds, groupIt->second.monitored_up_count, threshold);
        }
        else
        {
            groupIt->second.is_up = true;
            updateManagedInterfacesForGroupStateChange(group_name, false, true);
            SWSS_LOG_NOTICE("Monitor link group %s state changed: UP (%u monitored up, threshold: %u)",
                            group_name.c_str(), groupIt->second.monitored_up_count, threshold);
        }
    }
    else if (!should_be_up && (current_state || is_pending))
    {
        stopLinkupDelayTimer(group_name);
        if (is_pending)
        {
            groupIt->second.pending_up = false;
            SWSS_LOG_NOTICE("Monitor link group %s cancelled linkup delay (%u monitored up, threshold: %u)",
                            group_name.c_str(), groupIt->second.monitored_up_count, threshold);
        }
        if (current_state)
        {
            groupIt->second.is_up = false;
            updateManagedInterfacesForGroupStateChange(group_name, true, false);
            SWSS_LOG_NOTICE("Monitor link group %s state changed: DOWN (%u monitored up, threshold: %u)",
                            group_name.c_str(), groupIt->second.monitored_up_count, threshold);
        }
    }

    writeMonitorLinkGroupStateToDb(group_name);
}

void MonitorLinkGroupMgr::doTask(SelectableTimer &timer)
{
    timer.stop();

    auto it = m_timerToGroup.find(&timer);
    if (it != m_timerToGroup.end())
    {
        SWSS_LOG_NOTICE("Monitor link group %s linkup delay timer expired", it->second.c_str());
        handleLinkupDelayExpired(it->second);
        return;
    }

    SWSS_LOG_WARN("MonitorLinkGroupMgr: Linkup delay timer expired but no matching group found");
}

void MonitorLinkGroupMgr::startLinkupDelayTimer(const std::string& group_name)
{
    auto groupIt = m_monitorLinkGroups.find(group_name);
    if (groupIt == m_monitorLinkGroups.end())
    {
        SWSS_LOG_ERROR("MonitorLinkGroupMgr: Cannot start linkup delay timer for unknown group %s", group_name.c_str());
        return;
    }

    MonitorLinkGroupInfo& group_info = groupIt->second;
    uint32_t delay_seconds = group_info.linkup_delay;

    if (delay_seconds > 0)
    {
        string executor_name = "MONITOR_LINK_LINKUP_DELAY_" + group_name;
        Executor* existing_executor = Orch::getExecutor(executor_name);
        if (existing_executor != nullptr)
        {
            if (group_info.linkup_delay_timer == nullptr)
            {
                // Group was deleted and recreated — recover timer pointer from executor
                ExecutableTimer* exec_timer = dynamic_cast<ExecutableTimer*>(existing_executor);
                if (exec_timer != nullptr)
                {
                    group_info.linkup_delay_timer = exec_timer->getSelectableTimer();
                    SWSS_LOG_NOTICE("MonitorLinkGroupMgr: Recovered timer pointer from existing executor for group %s",
                                    group_name.c_str());
                }
                else
                {
                    SWSS_LOG_ERROR("MonitorLinkGroupMgr: Executor %s exists but is not ExecutableTimer for group %s",
                                   executor_name.c_str(), group_name.c_str());
                    return;
                }
            }
            group_info.linkup_delay_timer->stop();
        }
        else if (group_info.linkup_delay_timer == nullptr)
        {
            auto interv = timespec { .tv_sec = static_cast<time_t>(delay_seconds), .tv_nsec = 0 };
            group_info.linkup_delay_timer = new SelectableTimer(interv);

            auto executor = new ExecutableTimer(group_info.linkup_delay_timer, this, executor_name);

            SWSS_LOG_NOTICE("MonitorLinkGroupMgr: Creating linkup delay timer for group %s (executor=%s)",
                            group_name.c_str(), executor_name.c_str());

            try {
                Orch::addExecutor(executor);
            } catch (const std::exception& e) {
                SWSS_LOG_ERROR("MonitorLinkGroupMgr: Failed to add linkup delay executor for group %s: %s", group_name.c_str(), e.what());
                // Executor destructor will delete the timer (m_selectable)
                delete executor;
                group_info.linkup_delay_timer = nullptr;
                throw;
            }
        }
        else
        {
            group_info.linkup_delay_timer->stop();
        }

        m_timerToGroup[group_info.linkup_delay_timer] = group_name;
        auto interv = timespec { .tv_sec = static_cast<time_t>(delay_seconds), .tv_nsec = 0 };
        group_info.linkup_delay_timer->setInterval(interv);
        group_info.linkup_delay_timer->start();
        group_info.delay_start_time = std::chrono::steady_clock::now();

        SWSS_LOG_NOTICE("MonitorLinkGroupMgr: Started %u second linkup delay timer for group %s",
                        delay_seconds, group_name.c_str());
    }
}

void MonitorLinkGroupMgr::stopLinkupDelayTimer(const std::string& group_name)
{
    auto groupIt = m_monitorLinkGroups.find(group_name);
    if (groupIt == m_monitorLinkGroups.end())
    {
        return;
    }

    if (groupIt->second.linkup_delay_timer != nullptr)
    {
        groupIt->second.linkup_delay_timer->stop();
        SWSS_LOG_DEBUG("MonitorLinkGroupMgr: Stopped linkup delay timer for group %s", group_name.c_str());
    }
}

void MonitorLinkGroupMgr::handleLinkupDelayExpired(const std::string& group_name)
{
    auto groupIt = m_monitorLinkGroups.find(group_name);
    if (groupIt == m_monitorLinkGroups.end())
    {
        SWSS_LOG_ERROR("MonitorLinkGroupMgr: Cannot handle linkup delay expiry for unknown group %s", group_name.c_str());
        return;
    }

    MonitorLinkGroupInfo& group_info = groupIt->second;

    if (!group_info.pending_up)
    {
        SWSS_LOG_WARN("MonitorLinkGroupMgr: Linkup delay timer expired for group %s but group is not pending UP", group_name.c_str());
        return;
    }

    uint32_t threshold = group_info.min_monitored_links;

    if (group_info.monitored_up_count >= threshold)
    {
        group_info.pending_up = false;
        group_info.is_up = true;

        updateManagedInterfacesForGroupStateChange(group_name, false, true);
        writeMonitorLinkGroupStateToDb(group_name);

        SWSS_LOG_NOTICE("MonitorLinkGroupMgr: Monitor link group %s state changed: UP after linkup delay (%u monitored up, threshold: %u)",
                        group_name.c_str(), group_info.monitored_up_count, threshold);
    }
    else
    {
        group_info.pending_up = false;
        writeMonitorLinkGroupStateToDb(group_name);

        SWSS_LOG_NOTICE("MonitorLinkGroupMgr: Monitor link group %s cancelled linkup delay - threshold no longer met (%u monitored up, threshold: %u)",
                        group_name.c_str(), group_info.monitored_up_count, threshold);
    }
}

void MonitorLinkGroupMgr::writeMonitorLinkGroupStateToDb(const std::string& group_name)
{
    auto groupIt = m_monitorLinkGroups.find(group_name);
    if (groupIt == m_monitorLinkGroups.end())
        return;

    std::string state;
    if (groupIt->second.pending_up)
        state = "pending";
    else if (groupIt->second.is_up)
        state = "up";
    else
        state = "down";

    std::string monitored_interfaces_str;
    bool first = true;
    for (const auto& interface : groupIt->second.monitored_interfaces)
    {
        if (!first) monitored_interfaces_str += ",";
        monitored_interfaces_str += interface;
        first = false;
    }

    std::string managed_interfaces_str;
    first = true;
    for (const auto& interface : groupIt->second.managed_interfaces)
    {
        if (!first) managed_interfaces_str += ",";
        managed_interfaces_str += interface;
        first = false;
    }

    std::vector<FieldValueTuple> fvVector;
    fvVector.emplace_back("state", state);
    fvVector.emplace_back("description", groupIt->second.description);
    fvVector.emplace_back("monitored-links", monitored_interfaces_str);
    fvVector.emplace_back("managed-links", managed_interfaces_str);
    fvVector.emplace_back("link_up_threshold", std::to_string(groupIt->second.min_monitored_links));
    fvVector.emplace_back("link_up_delay", std::to_string(groupIt->second.linkup_delay));

    SWSS_LOG_INFO("Writing to STATE_DB for group %s: state='%s', monitored-links='%s', managed-links='%s'",
                  group_name.c_str(), state.c_str(), monitored_interfaces_str.c_str(), managed_interfaces_str.c_str());

    m_stateMonitorLinkGroupTable.set(group_name, fvVector);
}

void MonitorLinkGroupMgr::writeMonitorLinkGroupMemberStateToDb(const std::string& interface_name)
{
    std::vector<FieldValueTuple> fvVector;

    auto interfaceIt = m_monitorLinkInterfaces.find(interface_name);
    if (interfaceIt == m_monitorLinkInterfaces.end() || interfaceIt->second.managed_groups.empty())
        return;

    std::string state = (interfaceIt->second.down_group_count == 0) ? "allow_up" : "force_down";
    fvVector.emplace_back("state", state);

    // Build down_due_to: groups in this interface's managed_groups that are currently DOWN/PENDING
    std::string down_due_to_str;
    bool first = true;
    for (const auto& group_name : interfaceIt->second.managed_groups)
    {
        auto groupIt = m_monitorLinkGroups.find(group_name);
        if (groupIt != m_monitorLinkGroups.end())
        {
            bool group_is_up = groupIt->second.is_up && !groupIt->second.pending_up;
            if (!group_is_up)
            {
                if (!first) down_due_to_str += ",";
                down_due_to_str += group_name;
                first = false;
            }
        }
    }

    fvVector.emplace_back("down_due_to", down_due_to_str);

    SWSS_LOG_INFO("Writing to STATE_DB for interface %s: state='%s', down_due_to='%s'",
                  interface_name.c_str(), state.c_str(), down_due_to_str.c_str());

    m_stateMonitorLinkGroupMemberTable.set(interface_name, fvVector);
}

bool MonitorLinkGroupMgr::updateMonitorLinkInterfaceState(const std::string& interface_name, bool is_up)
{
    auto interfaceIt = m_monitorLinkInterfaces.find(interface_name);
    if (interfaceIt == m_monitorLinkInterfaces.end())
        return false;

    bool previous_state = interfaceIt->second.is_up;
    if (previous_state == is_up)
        return true;

    interfaceIt->second.is_up = is_up;

    // Monitored-link oper change drives group state; managed-link oper change does not
    for (const auto& group_name : interfaceIt->second.monitored_groups)
    {
        auto groupIt = m_monitorLinkGroups.find(group_name);
        if (groupIt == m_monitorLinkGroups.end()) continue;

        if (is_up && !previous_state)
        {
            groupIt->second.monitored_up_count++;
            SWSS_LOG_INFO("Monitor link interface %s (monitored-link) in group %s went UP (%u monitored up)",
                          interface_name.c_str(), group_name.c_str(), groupIt->second.monitored_up_count);
        }
        else if (!is_up && previous_state)
        {
            if (groupIt->second.monitored_up_count > 0) groupIt->second.monitored_up_count--;
            SWSS_LOG_INFO("Monitor link interface %s (monitored-link) in group %s went DOWN (%u monitored up)",
                          interface_name.c_str(), group_name.c_str(), groupIt->second.monitored_up_count);
        }
        updateMonitorLinkGroupState(group_name);
    }

    return true;
}

void MonitorLinkGroupMgr::updateManagedInterfacesForGroupStateChange(const std::string& group_name, bool group_was_up, bool group_is_up)
{
    auto groupIt = m_monitorLinkGroups.find(group_name);
    if (groupIt == m_monitorLinkGroups.end())
        return;

    if (group_was_up == group_is_up)
        return;

    // No link_type guard needed: iterating the group's own managed_interfaces set,
    // so every interface here is definitionally a managed-link of this group.
    for (const auto& interface_name : groupIt->second.managed_interfaces)
    {
        auto interfaceIt = m_monitorLinkInterfaces.find(interface_name);
        if (interfaceIt == m_monitorLinkInterfaces.end()) continue;

        if (!group_was_up && group_is_up)
        {
            if (interfaceIt->second.down_group_count > 0)
                interfaceIt->second.down_group_count--;
        }
        else if (group_was_up && !group_is_up)
        {
            interfaceIt->second.down_group_count++;
        }

        writeMonitorLinkGroupMemberStateToDb(interface_name);

        SWSS_LOG_INFO("MonitorLinkGroupMgr: Updated STATE_DB for interface %s due to group %s state change (%s → %s)",
                     interface_name.c_str(), group_name.c_str(),
                     group_was_up ? "UP" : "DOWN", group_is_up ? "UP" : "DOWN");
    }

    SWSS_LOG_DEBUG("MonitorLinkGroupMgr: Updated down_group_count for %zu managed-link interfaces in group %s (%s → %s)",
                   groupIt->second.managed_interfaces.size(), group_name.c_str(),
                   group_was_up ? "UP" : "DOWN", group_is_up ? "UP" : "DOWN");
}
