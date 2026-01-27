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

namespace p4orch
{
namespace test
{
class WcmpManagerTest;
} // namespace test

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
   WcmpManager(P4OidMapper* p4oidMapper,
               ResponsePublisherInterface* publisher) {
     SWSS_LOG_ENTER();

     assert(p4oidMapper != nullptr);
     m_p4OidMapper = p4oidMapper;
     assert(publisher != nullptr);
     m_publisher = publisher;
   }

    virtual ~WcmpManager() = default;

    void enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry) override;
    ReturnCode drain() override;
    void drainWithNotExecuted() override;
    std::string verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple) override;
    ReturnCode getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                            std::string &object_key) override;

    // Prunes or restores next hop members.
    void updateWatchPort(const std::string& port, bool prune);

    // Inserts into/updates port_oper_status_map
    void updatePortOperStatusMap(const std::string &port, const sai_port_oper_status_t &status);

    // Refreshes port oper-status with the latest values from PortsOrch.
    void refreshPortOperStatus();

  private:
    // Gets the internal cached WCMP group entry by its key.
    // Return nullptr if corresponding WCMP group entry is not cached.
    P4WcmpGroupEntry *getWcmpGroupEntry(const std::string &wcmp_group_id);

    // Deserializes an entry from table APP_P4RT_WCMP_GROUP_TABLE_NAME.
    ReturnCodeOr<P4WcmpGroupEntry> deserializeP4WcmpGroupAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes);

    // Perform validation on WCMP group entry. Return a SWSS status code
    ReturnCode validateWcmpGroupEntry(const P4WcmpGroupEntry &app_db_entry);

    // Processes add operation for an entry.
    ReturnCode processAddRequest(P4WcmpGroupEntry *app_db_entry);

    // Creates an WCMP group in the WCMP group table.
    // validateWcmpGroupEntry() is required in caller function before
    // createWcmpGroup() is called
    ReturnCode createWcmpGroup(P4WcmpGroupEntry *wcmp_group_entry);

    // Processes update operation for a WCMP group entry.
    ReturnCode processUpdateRequest(P4WcmpGroupEntry *wcmp_group_entry);

    // Deletes a WCMP group in the WCMP group table.
    ReturnCode removeWcmpGroup(const std::string &wcmp_group_id);

    // Fetches oper-status of port using port_oper_status_map or SAI.
    ReturnCode fetchPortOperStatus(const std::string &port, sai_port_oper_status_t *oper_status);

    // Inserts a next hop member in port_name_to_wcmp_group_member_map
    void insertMemberInPortNameToWcmpGroupMemberMap(std::shared_ptr<P4WcmpGroupMemberEntry> member);

    // Removes a next hop member from port_name_to_wcmp_group_member_map
    void removeMemberFromPortNameToWcmpGroupMemberMap(std::shared_ptr<P4WcmpGroupMemberEntry> member);

    // Gets port oper-status from port_oper_status_map if present
    bool getPortOperStatusFromMap(const std::string &port, sai_port_oper_status_t *status);

    // Fetches group member info (pruned status, nexthop OID) that is required
    // before create or update.
    ReturnCode fetchMemberInfo(P4WcmpGroupEntry* wcmp_group);

    // Verifies the internal cache for an entry.
    std::string verifyStateCache(const P4WcmpGroupEntry &app_db_entry, const P4WcmpGroupEntry *wcmp_group_entry);

    // Verifies the ASIC DB for an entry.
    std::string verifyStateAsicDb(P4WcmpGroupEntry* wcmp_group_entry);

    // Maps wcmp_group_id to P4WcmpGroupEntry
    std::unordered_map<std::string, P4WcmpGroupEntry> m_wcmpGroupTable;

    // Maps port name to P4WcmpGroupMemberEntry
    std::unordered_map<std::string, std::unordered_set<std::shared_ptr<P4WcmpGroupMemberEntry>>>
        port_name_to_wcmp_group_member_map;

    // Maps port name to oper-status
    std::unordered_map<std::string, sai_port_oper_status_t> port_oper_status_map;

    // Owners of pointers below must outlive this class's instance.
    P4OidMapper *m_p4OidMapper;
    std::deque<swss::KeyOpFieldsValuesTuple> m_entries;
    ResponsePublisherInterface* m_publisher;

    friend class p4orch::test::WcmpManagerTest;
};

} // namespace p4orch
