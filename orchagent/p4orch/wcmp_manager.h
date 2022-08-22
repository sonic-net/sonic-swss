#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "bulker.h"
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
    bool pruned;
    sai_object_id_t member_oid = SAI_NULL_OBJECT_ID;
    std::string wcmp_group_id;
};

struct P4WcmpGroupEntry
{
    std::string wcmp_group_id;
    // next_hop_id: P4WcmpGroupMemberEntry
    std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>> wcmp_group_members;
    sai_object_id_t wcmp_group_oid = SAI_NULL_OBJECT_ID;
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
    WcmpManager(P4OidMapper *p4oidMapper, ResponsePublisherInterface *publisher);

    virtual ~WcmpManager() = default;

    void enqueue(const swss::KeyOpFieldsValuesTuple &entry) override;
    void drain() override;
    std::string verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple) override;

    // Prunes next hop members egressing through the given port.
    void pruneNextHops(const std::string &port);

    // Restores pruned next hop members on link up. Returns an SWSS status code.
    void restorePrunedNextHops(const std::string &port);

    // Inserts into/updates port_oper_status_map
    void updatePortOperStatusMap(const std::string &port, const sai_port_oper_status_t &status);

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

    // Creates WCMP group member in the WCMP group.
    ReturnCode createWcmpGroupMember(std::shared_ptr<P4WcmpGroupMemberEntry> wcmp_group_member,
                                     const sai_object_id_t group_oid, const std::string &wcmp_group_key);

    // Performs watchport related addition operations and creates WCMP group
    // members.
    ReturnCode processWcmpGroupMembersAddition(
        const std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>> &members, const std::string &wcmp_group_key,
        sai_object_id_t wcmp_group_oid,
        std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>> &created_wcmp_group_members);

    // Performs watchport related removal operations and removes WCMP group
    // members.
    ReturnCode processWcmpGroupMembersRemoval(
        const std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>> &members, const std::string &wcmp_group_key,
        std::vector<std::shared_ptr<P4WcmpGroupMemberEntry>> &removed_wcmp_group_members);

    // Processes update operation for a WCMP group entry.
    ReturnCode processUpdateRequest(P4WcmpGroupEntry *wcmp_group_entry);

    // Clean up group members when request fails
    void recoverGroupMembers(
        p4orch::P4WcmpGroupEntry *wcmp_group, const std::string &wcmp_group_key,
        const std::vector<std::shared_ptr<p4orch::P4WcmpGroupMemberEntry>> &created_wcmp_group_members,
        const std::vector<std::shared_ptr<p4orch::P4WcmpGroupMemberEntry>> &removed_wcmp_group_members);

    // Deletes a WCMP group in the WCMP group table.
    ReturnCode removeWcmpGroup(const std::string &wcmp_group_id);

    // Deletes a WCMP group member in the WCMP group table.
    ReturnCode removeWcmpGroupMember(const std::shared_ptr<P4WcmpGroupMemberEntry> wcmp_group_member,
                                     const std::string &wcmp_group_id);

    // Fetches oper-status of port using port_oper_status_map or SAI.
    ReturnCode fetchPortOperStatus(const std::string &port, sai_port_oper_status_t *oper_status);

    // Inserts a next hop member in port_name_to_wcmp_group_member_map
    void insertMemberInPortNameToWcmpGroupMemberMap(std::shared_ptr<P4WcmpGroupMemberEntry> member);

    // Removes a next hop member from port_name_to_wcmp_group_member_map
    void removeMemberFromPortNameToWcmpGroupMemberMap(std::shared_ptr<P4WcmpGroupMemberEntry> member);

    // Gets port oper-status from port_oper_status_map if present
    bool getPortOperStatusFromMap(const std::string &port, sai_port_oper_status_t *status);

    // Verifies the internal cache for an entry.
    std::string verifyStateCache(const P4WcmpGroupEntry &app_db_entry, const P4WcmpGroupEntry *wcmp_group_entry);

    // Verifies the ASIC DB for an entry.
    std::string verifyStateAsicDb(const P4WcmpGroupEntry *wcmp_group_entry);

    // Returns the SAI attributes for a group member.
    std::vector<sai_attribute_t> getSaiMemberAttrs(const P4WcmpGroupMemberEntry &wcmp_member_entry,
                                                   const sai_object_id_t group_oid);

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
    ResponsePublisherInterface *m_publisher;
    ObjectBulker<sai_next_hop_group_api_t> gNextHopGroupMemberBulker;

    friend class p4orch::test::WcmpManagerTest;
};

} // namespace p4orch
