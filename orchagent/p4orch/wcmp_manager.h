#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "notificationconsumer.h"
#include "orch.h"
#include "p4orch/object_manager_interface.h"
#include "p4orch/p4oidmapper.h"
#include "response_publisher_interface.h"
#include "return_code.h"
extern "C"
{
#include "sai.h"
}
#include "selectableevent.h"

namespace p4orch
{
namespace test
{
class WcmpManagerTest;
} // namespace test

constexpr uint32_t kMaxWatchportGroupBulkSize = 30;

struct P4WcmpGroupMemberEntry
{
    std::string next_hop_id;
    // Default ECMP(weight=1)
    int weight = 1;
    std::string watch_port;
    bool pruned = false;
    sai_object_id_t next_hop_oid = SAI_NULL_OBJECT_ID;
    std::string wcmp_group_id;
};

struct P4WcmpGroupEntry
{
    std::string wcmp_group_id;
    // next_hop_id: P4WcmpGroupMemberEntry
    std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>> wcmp_group_members;
    sai_object_id_t wcmp_group_oid = SAI_NULL_OBJECT_ID;
    std::vector<sai_object_id_t> nexthop_ids;
    std::vector<uint32_t> nexthop_weights;
};

// WcmpManager listens to changes in table APP_P4RT_WCMP_GROUP_TABLE_NAME and
// creates/updates/deletes next hop group SAI object accordingly. Below is
// an example WCMP group table entry in APPL_DB.
//
// P4RT_TABLE:FIXED_WCMP_GROUP_TABLE:{"match/wcmp_group_id":"group-1"}
//   "actions" =[
//     {
//       "action": "set_nexthop_id",
//       "param/nexthop_id": "node-1234:eth-1/2/3",
//       "weight": 3,
//       "watch_port": "Ethernet0",
//     },
//     {
//       "action": "set_nexthop_id",
//       "param/nexthop_id": "node-2345:eth-1/2/3",
//       "weight": 4,
//       "watch_port": "Ethernet8",
//     },
//   ]
//   "controller_metadata" = "..."
class WcmpManager : public ObjectManagerInterface
{
  public:
   WcmpManager(P4OidMapper* p4oidMapper, ResponsePublisherInterface* publisher,
               swss::SelectableEvent* watchport_event) {
     SWSS_LOG_ENTER();

     assert(p4oidMapper != nullptr);
     m_p4OidMapper = p4oidMapper;
     assert(publisher != nullptr);
     m_publisher = publisher;
     assert(watchport_event != nullptr);
     m_watchport_event = watchport_event;
   }

    virtual ~WcmpManager() = default;

    void enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry) override;
    ReturnCode drain() override;
    void drainWithNotExecuted() override;
    std::string verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple) override;
    ReturnCode getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                            std::string &object_key) override;

    // Updates m_port_oper_status_map and enqueues watchport events.
    // This call only queues the group update event into m_watchport_groups.
    // The real watchport update will happen in processWatchPortEvent().
    void updateWatchPort(const std::string& port, sai_port_oper_status_t status);

    // Update the WCMP group member if the member is also a member of an LACP
    // LAG. Prune or restore this WCMP group member based on the LACP state.
    void updateLagMemberWatchPort(const std::string& port, bool lacp_enable);

    // Process group update in m_watchport_groups, maximum of
    // kMaxWatchportGroupBulkSize groups per call.
    void processWatchPortEvent();

    // Refreshes port oper-status with the latest values from PortsOrch.
    void refreshPortOperStatus();

  private:
    // Gets the internal cached WCMP group entry by its key.
    // Return nullptr if corresponding WCMP group entry is not cached.
    P4WcmpGroupEntry *getWcmpGroupEntry(const std::string &wcmp_group_id);

    // Deserializes an entry from table APP_P4RT_WCMP_GROUP_TABLE_NAME.
    ReturnCodeOr<P4WcmpGroupEntry> deserializeP4WcmpGroupAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes);

    // Performs validation on WCMP group entry. Returns a SWSS status code
    ReturnCode validateWcmpGroupEntry(const P4WcmpGroupEntry& app_db_entry,
                                      const std::string& operation);

    // Processes a list of entries of the same operation type.
    // Returns an overall status code.
    // This method also sends the response to the application.
    ReturnCode processEntries(
        std::vector<P4WcmpGroupEntry>& entries,
        const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
        const std::string& op, bool update);

    // Creates a list of WCMP groups in the WCMP group table.
    std::vector<ReturnCode> createWcmpGroups(
        std::vector<P4WcmpGroupEntry>& entries);

    // Deletes a list of WCMP groups in the WCMP group table.
    std::vector<ReturnCode> removeWcmpGroups(
        const std::vector<P4WcmpGroupEntry>& entries);

    // Updates a list of WCMP groups in the WCMP group table.
    std::vector<ReturnCode> updateWcmpGroups(
        std::vector<P4WcmpGroupEntry>& entries, bool watchport = false);

    // Fetches oper-status of port from portsorch.
    void fetchPortOperStatus(const P4WcmpGroupEntry& wcmp_group);

    // Inserts a next hop member in m_port_name_to_wcmp_group_member_map
    void insertMemberInPortNameToWcmpGroupMemberMap(std::shared_ptr<P4WcmpGroupMemberEntry> member);

    // Removes a next hop member from m_port_name_to_wcmp_group_member_map
    void removeMemberFromPortNameToWcmpGroupMemberMap(std::shared_ptr<P4WcmpGroupMemberEntry> member);

    // Populates group member info (pruned status, nexthop OID) that is required
    // before create or update.
    void populateMemberInfo(P4WcmpGroupEntry& wcmp_group);

    // Verifies the internal cache for an entry.
    std::string verifyStateCache(const P4WcmpGroupEntry &app_db_entry, const P4WcmpGroupEntry *wcmp_group_entry);

    // Verifies the ASIC DB for an entry.
    std::string verifyStateAsicDb(P4WcmpGroupEntry* wcmp_group_entry);

    // Maps wcmp_group_id to P4WcmpGroupEntry
    std::unordered_map<std::string, P4WcmpGroupEntry> m_wcmpGroupTable;

    // Maps port name to P4WcmpGroupMemberEntry
    std::unordered_map<std::string, std::unordered_set<std::shared_ptr<P4WcmpGroupMemberEntry>>>
        m_port_name_to_wcmp_group_member_map;

    // Maps port name to oper-status
    std::unordered_map<std::string, sai_port_oper_status_t>
      m_port_oper_status_map;

    // Maps port name to LACP enable status if the port is a LAG member
    std::unordered_map<std::string, bool> m_port_lacp_enable_status_map;

    // Owners of pointers below must outlive this class's instance.
    P4OidMapper *m_p4OidMapper;
    std::deque<swss::KeyOpFieldsValuesTuple> m_entries;
    ResponsePublisherInterface* m_publisher;
    swss::SelectableEvent* m_watchport_event;

    // A set that stores the groups that need to be updated due to watchport
    // events.
    std::unordered_set<std::string> m_watchport_groups;

    friend class p4orch::test::WcmpManagerTest;
};

} // namespace p4orch
