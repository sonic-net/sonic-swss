#pragma once

#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// Table entries for multicast_router_interface_table.
struct P4MulticastRouterInterfaceEntry {
  std::string multicast_router_interface_entry_key;  // Unique key of the entry.
  std::string multicast_replica_port;
  std::string multicast_replica_instance;
  swss::MacAddress src_mac;
  std::string multicast_metadata;
  sai_object_id_t router_interface_oid = SAI_OBJECT_TYPE_NULL;

  P4MulticastRouterInterfaceEntry() = default;
  P4MulticastRouterInterfaceEntry(const std::string& port,
                                  const std::string& instance,
                                  const swss::MacAddress& mac,
                                  const std::string& metadata)
      : multicast_replica_port(port),
        multicast_replica_instance(instance),
        src_mac(mac),
        multicast_metadata(metadata) {}
};

// Table entries for replication_multicast_table.
struct P4MulticastReplicationEntry {
  std::string multicast_replication_key;  // Unique key of the entry.
  std::string multicast_group_id;
  std::string multicast_replica_port;
  std::string multicast_replica_instance;
  std::string multicast_metadata;
  sai_object_id_t multicast_group_oid = SAI_OBJECT_TYPE_NULL;
  sai_object_id_t multicast_group_member_oid = SAI_OBJECT_TYPE_NULL;

  P4MulticastReplicationEntry() = default;
  P4MulticastReplicationEntry(const std::string& group_id,
                              const std::string& port,
                              const std::string& instance,
                              const std::string& metadata)
      : multicast_group_id(group_id),
        multicast_replica_port(port),
        multicast_replica_instance(instance),
        multicast_metadata(metadata) {}
};

// P4MulticastRouterInterfaceTable:
//   Multicast router interface key, P4MulticastRouterInterfaceEntry
typedef std::unordered_map<std::string, P4MulticastRouterInterfaceEntry>
    P4MulticastRouterInterfaceTable;
// P4MulticastReplicationTable: Replication key, P4MulticastReplicationEntry
typedef std::unordered_map<std::string, P4MulticastReplicationEntry>
    P4MulticastReplicationTable;

// The L3MulticastManager handles updates to two P4 tables:
// * The "fixed" table multicast_router_interface_table, which defines a single
//   action set_src_mac to map output port and egress instance ID to a Ethernet
//   source MAC address to use for replicated packets.  Entries in this table
//   create router interface (RIF) objects in the ASIC.
// * The new "packet replication" table replication_multicast_table, which
//   is modeled as an action-less table, where the table key of
//   multicast group ID, egress instance, and output port will create entries
//   in the ASIC's packet replication table.  Packet replication defines a
//   one-to-many mapping from multicast group IDs to replicas.

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
  /*
  // Drains entries associated with the multicast router interface table.
  ReturnCode drainMulticastRouterInterfaceEntries(
      std::deque<swss::KeyOpFieldsValuesTuple>& router_interface_tuples);

  // Drains entries associated with the multicast replication table.
  ReturnCode drainMulticastReplicationEntries(
      std::deque<swss::KeyOpFieldsValuesTuple>& replication_tuples);
  */

  // Converts db table entry into P4MulticastRouterInterfaceEntry.
  ReturnCodeOr<P4MulticastRouterInterfaceEntry>
  deserializeMulticastRouterInterfaceEntry(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& attributes,
      const std::string& table_name);

  // Converts db table entry into P4MulticastReplicationEntry.
  ReturnCodeOr<P4MulticastReplicationEntry>
  deserializeMulticastReplicationEntry(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& attributes,
      const std::string& table_name);

  // Performs multicast router interface entry validation.
  ReturnCode validateMulticastRouterInterfaceEntry(
      const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry,
      const std::string& operation);

  /*
  // Performs multicast router interface entry validation for SET command.
  ReturnCode validateSetMulticastRouterInterfaceEntry(
      const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry);

  // Performs multicast router interface entry validation for DEL command.
  ReturnCode validateDelMulticastRouterInterfaceEntry(
      const P4MulticastRouterInterfaceEntry& multicast_router_interface_entry);
  */

  // Performs multicast replication entry validation.
  ReturnCode validateMulticastReplicationEntry(
      const P4MulticastReplicationEntry& multicast_replication_entry,
      const std::string& operation);

  /*
  // Performs multicast replication entry validation for SET command.
  ReturnCode validateSetMulticastReplicationEntry(
      const P4MulticastReplicationEntry& multicast_router_interface_entry);

  // Performs multicast replication entry validation for DEL command.
  ReturnCode validateDelMulticastReplicationEntry(
      const P4MulticastReplicationEntry& multicast_router_interface_entry);
  */

  // Processes a list of entries of the same operation type for the multicast
  // router interface table.
  // Returns an overall status code.
  // This method also sends the response to the application.
  ReturnCode processMulticastRouterInterfaceEntries(
      std::vector<P4MulticastRouterInterfaceEntry>& entries,
      const std::deque<swss::KeyOpFieldsValuesTuple>& tuple_list,
      const std::string& op, bool update);

  /*
  // Processes a list of entries of the same operation type for the replication
  // multicast table.
  // Returns an overall status code.
  // This method also sends the response to the application.
  ReturnCode processMulticastReplicationEntries(
      std::vector<P4MulticastReplicationEntry>& entries,
      const std::deque<swss::KeyOpFieldsValuesTuple>& tuple_list,
      const std::string& op, bool update);
  */

  // Wrapper around SAI setup and call, for easy mocking.
  ReturnCode createRouterInterface(const std::string& rif_key,
                                   P4MulticastRouterInterfaceEntry& entry,
                                   sai_object_id_t* rif_oid);
  ReturnCode deleteRouterInterface(const std::string& rif_key,
                                   sai_object_id_t rif_oid);

  // Wrapper around SAI setup and call to create multicast group.
  ReturnCode createMulticastGroup(P4MulticastReplicationEntry& entry,
                                  sai_object_id_t* mcast_group_oid);
  ReturnCode deleteMulticastGroup(const std::string& multicast_group_id,
                                  sai_object_id_t mcast_group_oid);

  // Wrapper around SAI setup and call to create multicast group members.
  ReturnCode createMulticastGroupMember(
      const P4MulticastReplicationEntry& entry, const sai_object_id_t rif_oid,
      sai_object_id_t* mcast_group_member_oid);

  // Add new multicast router interface table entries.
  std::vector<ReturnCode> addMulticastRouterInterfaceEntries(
      std::vector<P4MulticastRouterInterfaceEntry>& entries);
  // Update existing multicast router interface table entries.
  std::vector<ReturnCode> updateMulticastRouterInterfaceEntries(
      std::vector<P4MulticastRouterInterfaceEntry>& entries);
  // Delete existing multicast router interface table entries.
  std::vector<ReturnCode> deleteMulticastRouterInterfaceEntries(
      const std::vector<P4MulticastRouterInterfaceEntry>& entries);

  // Add new multicast replication table entries.
  std::vector<ReturnCode> addMulticastReplicationEntries(
      std::vector<P4MulticastReplicationEntry>& entries);
  // Update existing multicast replication table entries.
  /*
  std::vector<ReturnCode> updateMulticastReplicationEntries(
      std::vector<P4MulticastReplicationEntry>& entries);
  */
  // Delete existing multicast replication table entries.
  std::vector<ReturnCode> deleteMulticastReplicationEntries(
      const std::vector<P4MulticastReplicationEntry>& entries);

  /*
  std::string verifyMulticastRouterInterfaceState(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& tuple);
  std::string verifyMulticastReplicationState(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& tuple);
  */

  // Verifies internal cache for a multicast router interface entry.
  std::string verifyMulticastRouterInterfaceStateCache(
      const P4MulticastRouterInterfaceEntry& app_db_entry,
      const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry);
  // Verifies internal cache for a multicast replication entry.
  std::string verifyMulticastReplicationStateCache(
      const P4MulticastReplicationEntry& app_db_entry,
      const P4MulticastReplicationEntry* multicast_replication_entry);

  /*
  // Verifies ASIC DB for a multicast router interface entry.
  std::string verifyMulticastRouterInterfaceStateAsicDb(
      const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry);
  */
  // Verifies ASIC DB for a multicast replication entry.
  std::string verifyMulticastReplicationStateAsicDb(
      const P4MulticastReplicationEntry* multicast_replication_entry);

  // Gets the internal cached multicast router interface entry.
  // Return nullptr if corresponding multicast router interface entry is not
  // cached.
  P4MulticastRouterInterfaceEntry* getMulticastRouterInterfaceEntry(
      const std::string& multicast_router_interface_entry_key);

  // Gets the internal cached multicast replication entry.
  // Return nullptr if corresponding multicast replication entry is not cached.
  P4MulticastReplicationEntry* getMulticastReplicationEntry(
      const std::string& multicast_replication_key);

  // Fetches the RIF OID for a given multicast router interface entry.
  // Return SAI_NULL_OBJECT_ID if not found.
  // A RIF is unique for each egress multicast_replica_port and Ethernet
  // src mac pair.  The multicast_replica_instance is ignored as controller
  // bookkeeping.
  sai_object_id_t getRifOid(
      const P4MulticastRouterInterfaceEntry* multicast_router_interface_entry);
  /*
  // Fetches the RIF OID that will be used by a given multicast repelication
  // entry.  This would be the value used by the replication group member.
  sai_object_id_t getRifOid(
      const P4MulticastReplicationEntry* multicast_replication_entry);
  */

  // Internal cache of entries.
  P4MulticastRouterInterfaceTable m_multicastRouterInterfaceTable;
  P4MulticastReplicationTable m_multicastReplicationTable;

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
  // multicast group IDs -> set of member entries (P4MulticastReplicationEntry
  // keys)
  std::unordered_map<std::string, std::unordered_set<std::string>>
      m_multicastGroupMembers;

  P4OidMapper* m_p4OidMapper;
  VRFOrch* m_vrfOrch;
  ResponsePublisherInterface* m_publisher;
  std::deque<swss::KeyOpFieldsValuesTuple> m_entries;

  friend class L3MulticastManagerTest;
};

}  // namespace p4orch
