#pragma once

#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "macaddress.h"
#include "orch.h"
#include "p4orch/object_manager_interface.h"
#include "p4orch/p4oidmapper.h"
#include "response_publisher_interface.h"
#include "return_code.h"
#include "vrforch.h"

extern "C" {
#include "sai.h"
}

namespace p4orch {

// LINT.IfChange
// Table entries for multicast_router_interface_table.
struct P4MulticastRouterInterfaceEntry {
  std::string multicast_router_interface_entry_key;  // Unique key of the entry.
  std::string multicast_replica_port;
  std::string multicast_replica_instance;
  swss::MacAddress src_mac;
  std::string action;
  std::string multicast_metadata;
  sai_object_id_t router_interface_oid = SAI_OBJECT_TYPE_NULL;

  P4MulticastRouterInterfaceEntry() = default;
  P4MulticastRouterInterfaceEntry(const std::string& port,
                                  const std::string& instance,
                                  const swss::MacAddress& mac,
                                  const std::string& action,
                                  const std::string& metadata)
      : multicast_replica_port(port),
        multicast_replica_instance(instance),
        src_mac(mac),
        action(action),
        multicast_metadata(metadata) {}
};
// LINT.ThenChange(l3_multicast_manager.cpp:verify_state_ritf_cache)

struct P4Replica {
  std::string multicast_group_id;
  std::string port;
  std::string instance;
  std::string key;

  P4Replica() = default;
  P4Replica(const std::string& group_id, const std::string& port_name,
            const std::string& instance_number)
      : multicast_group_id(group_id),
        port(port_name),
        instance(instance_number) {
    key = group_id + ":" + port_name + ":" + instance_number;
  }
};

// LINT.IfChange
// Table entries for replication_multicast_group_table.
struct P4MulticastGroupEntry {
  std::string multicast_group_id;  // Also a unique key for the entry.
  std::vector<P4Replica> replicas;
  std::string multicast_metadata;
  std::string controller_metadata;
  sai_object_id_t multicast_group_oid = SAI_OBJECT_TYPE_NULL;
  std::unordered_map<std::string, sai_object_id_t> member_oids;

  P4MulticastGroupEntry() = default;
  P4MulticastGroupEntry(const std::string& group_id,
                        const std::string& metadata)
      : multicast_group_id(group_id), multicast_metadata(metadata) {}
};
// LINT.ThenChange(l3_multicast_manager.cpp:verify_state_group_cache)

// P4MulticastRouterInterfaceTable:
//   Multicast router interface key, P4MulticastRouterInterfaceEntry
typedef std::unordered_map<std::string, P4MulticastRouterInterfaceEntry>
    P4MulticastRouterInterfaceTable;
// P4MulticastGroupTable: multicast group ID, P4MulticastGroupEntry
typedef std::unordered_map<std::string, P4MulticastGroupEntry>
    P4MulticastGroupTable;

// The L3MulticastManager handles updates to two P4 tables:
// * The "fixed" table multicast_router_interface_table, which defines a single
//   action set_multicast_src_mac to map output port and egress instance ID to
//   an Ethernet source MAC address to use for replicated packets.  Entries in
//   this table create router interface (RIF) objects in the ASIC.
// * The new "multicast group" table replication_multicast_table, which
//   is modeled as an action-less table, where the table key is the multicast
//   group ID and the field values indicate the replicas.

class L3MulticastManager : public ObjectManagerInterface {
 public:
  L3MulticastManager(P4OidMapper* mapper, VRFOrch* vrfOrch,
                     ResponsePublisherInterface* publisher);
  virtual ~L3MulticastManager() = default;

  void enqueue(const std::string& table_name,
               const swss::KeyOpFieldsValuesTuple& entry) override;
  ReturnCode drain() override;
  void drainWithNotExecuted() override;
  std::string verifyState(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& tuple) override;
  ReturnCode getSaiObject(const std::string& json_key,
                          sai_object_type_t& object_type,
                          std::string& object_key) override;

 private:
  // Drains entries associated with the multicast router interface table.
  ReturnCode drainMulticastRouterInterfaceEntries(
      std::deque<swss::KeyOpFieldsValuesTuple>& router_interface_tuples);

  // Drains entries associated with the multicast group table.
  ReturnCode drainMulticastGroupEntries(
      std::deque<swss::KeyOpFieldsValuesTuple>& group_entry_tuples);

  // Converts db table entry into P4MulticastRouterInterfaceEntry.
  ReturnCodeOr<P4MulticastRouterInterfaceEntry>
  deserializeMulticastRouterInterfaceEntry(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& attributes);

  // Converts db table entry into P4MulticastGroupEntry.
  ReturnCodeOr<P4MulticastGroupEntry> deserializeMulticastGroupEntry(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& attributes);

  // Performs multicast router interface entry validation.
  ReturnCode validateMulticastRouterInterfaceEntry(
      const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
      const std::string& operation);

  // Performs multicast router interface entry validation for SET command.
  ReturnCode validateSetMulticastRouterInterfaceEntry(
      const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry);

  // Performs multicast router interface entry validation for DEL command.
  ReturnCode validateDelMulticastRouterInterfaceEntry(
      const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry);

  ReturnCode validateL3SetMulticastRouterInterfaceEntry(
      const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
      const P4MulticastRouterInterfaceEntry* router_interface_entry_ptr);
  ReturnCode validateL2SetMulticastRouterInterfaceEntry(
      const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
      const P4MulticastRouterInterfaceEntry* router_interface_entry_ptr);
  ReturnCode validateL3DelMulticastRouterInterfaceEntry(
      const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
      const P4MulticastRouterInterfaceEntry* router_interface_entry_ptr);
  ReturnCode validateL2DelMulticastRouterInterfaceEntry(
      const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
      const P4MulticastRouterInterfaceEntry* router_interface_entry_ptr);

  // Performs multicast group entry validation.
  ReturnCode validateMulticastGroupEntry(
      const P4MulticastGroupEntry& multicast_group_entry,
      const std::string& operation);

  // Performs multicast group entry validation for SET command.
  ReturnCode validateSetMulticastGroupEntry(
      const P4MulticastGroupEntry& multicast_group_entry);

  // Performs multicast group entry validation for DEL command.
  ReturnCode validateDelMulticastGroupEntry(
      const P4MulticastGroupEntry& multicast_group_entry);

  // Processes a list of entries of the same operation type for the multicast
  // router interface table.
  // Returns an overall status code.
  // This method also sends the response to the application.
  ReturnCode processMulticastRouterInterfaceEntries(
      std::vector<P4MulticastRouterInterfaceEntry>& entries,
      const std::deque<swss::KeyOpFieldsValuesTuple>& tuple_list,
      const std::string& op, bool update);

  // Processes a list of entries of the same operation type for the multicast
  // group table.
  // Returns an overall status code.
  // This method also sends the response to the application.
  ReturnCode processMulticastGroupEntries(
      std::vector<P4MulticastGroupEntry>& entries,
      const std::deque<swss::KeyOpFieldsValuesTuple>& tuple_list,
      const std::string& op, bool update);

  // Wrapper around SAI setup and call, for easy mocking.
  ReturnCode createBridgePort(P4MulticastRouterInterfaceEntry& entry,
                              sai_object_id_t* bridge_port_oid);
  ReturnCode deleteBridgePort(const std::string& port,
                              sai_object_id_t bridge_port_oid);
  ReturnCode createRouterInterface(const std::string& rif_key,
                                   P4MulticastRouterInterfaceEntry& entry,
                                   sai_object_id_t* rif_oid);
  ReturnCode deleteRouterInterface(const std::string& rif_key,
                                   sai_object_id_t rif_oid);

  // Wrapper around SAI setup and call to create multicast group.
  ReturnCode createMulticastGroup(P4MulticastGroupEntry& entry,
                                  sai_object_id_t* mcast_group_oid);

  ReturnCode deleteMulticastGroup(const std::string& multicast_group_id,
                                  sai_object_id_t mcast_group_oid);

  // Wrapper around SAI setup and call to create multicast group members.
  ReturnCode createMulticastGroupMember(
      const P4Replica& replica, const sai_object_id_t group_oid,
      const sai_object_id_t rif_oid, sai_object_id_t* mcast_group_member_oid);

  // Add new multicast router interface table entries.
  std::vector<ReturnCode> addMulticastRouterInterfaceEntries(
      std::vector<P4MulticastRouterInterfaceEntry>& entries);
  ReturnCode addL3MulticastRouterInterfaceEntry(
      P4MulticastRouterInterfaceEntry& entry);
  ReturnCode addL2MulticastRouterInterfaceEntry(
      P4MulticastRouterInterfaceEntry& entry);

  // Update existing multicast router interface table entries.
  std::vector<ReturnCode> updateMulticastRouterInterfaceEntries(
      std::vector<P4MulticastRouterInterfaceEntry>& entries);

  // Delete existing multicast router interface table entries.
  std::vector<ReturnCode> deleteMulticastRouterInterfaceEntries(
      const std::vector<P4MulticastRouterInterfaceEntry>& entries);
  ReturnCode deleteL3MulticastRouterInterfaceEntry(
      const P4MulticastRouterInterfaceEntry* entry);
  ReturnCode deleteL2MulticastRouterInterfaceEntry(
      const P4MulticastRouterInterfaceEntry* entry);

  // Add new multicast group table entries.
  std::vector<ReturnCode> addMulticastGroupEntries(
      std::vector<P4MulticastGroupEntry>& entries);
  // Update existing multicast group table entries.
  std::vector<ReturnCode> updateMulticastGroupEntries(
      std::vector<P4MulticastGroupEntry>& entries);
  // Used during failure scenarios where we try to revert to the previous state.
  ReturnCode restoreDeletedGroupMembers(
      const std::vector<P4Replica>& deleted_replicas,
      const std::unordered_map<std::string, sai_object_id_t>& replica_rif_map,
      const sai_object_id_t group_oid, const std::string& error_message,
      P4MulticastGroupEntry* old_entry);
  // Delete existing multicast group table entries.
  std::vector<ReturnCode> deleteMulticastGroupEntries(
      const std::vector<P4MulticastGroupEntry>& entries);

  std::string verifyMulticastRouterInterfaceState(
      const std::string& key, const std::vector<swss::FieldValueTuple>& tuple);
  std::string verifyMulticastGroupState(
      const std::string& key, const std::vector<swss::FieldValueTuple>& tuple);

  // Verifies internal cache for a multicast router interface entry.
  std::string verifyMulticastRouterInterfaceStateCache(
      const P4MulticastRouterInterfaceEntry& app_db_entry,
      const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry);
  // Verifies internal cache for a multicast group entry.
  std::string verifyMulticastGroupStateCache(
      const P4MulticastGroupEntry& app_db_entry,
      const P4MulticastGroupEntry* multicast_group_entry);

  // Verifies ASIC DB for a multicast router interface entry.
  std::string verifyMulticastRouterInterfaceStateAsicDb(
      const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry);
  std::string verifyL3MulticastRouterInterfaceStateAsicDb(
      const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry);
  std::string verifyL2MulticastRouterInterfaceStateAsicDb(
      const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry);
  // Verifies ASIC DB for a multicast group entry.
  std::string verifyMulticastGroupStateAsicDb(
      const P4MulticastGroupEntry* multicast_group_entry);

  // Gets the internal cached multicast router interface entry.
  // Return nullptr if corresponding multicast router interface entry is not
  // cached.
  P4MulticastRouterInterfaceEntry* getMulticastRouterInterfaceEntry(
      const std::string& multicast_router_interface_entry_key);

  // Gets the internal cached multicast group entry.
  // Return nullptr if corresponding multicast group entry is not cached.
  P4MulticastGroupEntry* getMulticastGroupEntry(
      const std::string& multicast_group_id);

  // Fetches the RIF OID for a given multicast router interface entry.
  // Return SAI_NULL_OBJECT_ID if not found.
  // A RIF is unique for each egress multicast_replica_port and Ethernet
  // src mac pair.  The multicast_replica_instance is ignored as controller
  // bookkeeping.
  sai_object_id_t getRifOid(
      const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry);

  // Fetches the RIF OID that will be used by a given multicast replica.
  // This would be the value used by the group member.
  sai_object_id_t getRifOid(const P4Replica& replica);

  // Fetches a bridge port OID for a port that will be used for L2 multicast
  // group members.
  sai_object_id_t getBridgePortOid(
      const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry);

  // Internal cache of entries.
  P4MulticastRouterInterfaceTable m_multicastRouterInterfaceTable;
  P4MulticastGroupTable m_multicastGroupEntryTable;

  // Several maps to keep track of entry relationships.

  // Egress port / router interface key -> RIF OID.
  // Note that we ignore multicast_replica_instance, because that is for
  // controller bookkeeping.
  std::unordered_map<std::string, sai_object_id_t> m_rifOids;
  // RIF OIDs -> which router interface entries are using it.
  std::unordered_map<sai_object_id_t,
                     std::vector<P4MulticastRouterInterfaceEntry>>
      m_rifOidToRouterInterfaceEntries;
  // RIF OIDs -> multicast group members using the RIF.
  std::unordered_map<sai_object_id_t, std::unordered_set<std::string>>
      m_rifOidToMulticastGroupMembers;

  P4OidMapper* m_p4OidMapper;
  VRFOrch* m_vrfOrch;
  ResponsePublisherInterface* m_publisher;
  std::deque<swss::KeyOpFieldsValuesTuple> m_entries;

  friend class L3MulticastManagerTest;
};

}  // namespace p4orch
