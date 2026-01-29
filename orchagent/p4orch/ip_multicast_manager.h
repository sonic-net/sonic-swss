#pragma once

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "ipaddress.h"
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

struct P4IpMulticastEntry {
  std::string ip_multicast_entry_key;  // Unique key of an IP multicast entry.
  std::string vrf_id;
  swss::IpAddress ip_dst;
  std::string action;
  std::string multicast_group_id;
  std::string controller_metadata;
  sai_ipmc_entry_t sai_ipmc_entry;
};

// P4IpMulticastTable: ip_multicast_entry_key, P4IpMulticastEntry
typedef std::unordered_map<std::string, P4IpMulticastEntry> P4IpMulticastTable;

// The IpMulticastManager handles updates to two "fixed" P4 tables:
// * ipv4_multicast_table
// * ipv6_multicast_table
// These tables are used to assign a IP packet to a multicast group.  At the
// SAI layer, this is achieved by creating IPMC entries.
class IpMulticastManager : public ObjectManagerInterface {
 public:
  IpMulticastManager(P4OidMapper* mapper, VRFOrch* vrfOrch,
                     ResponsePublisherInterface* publisher);
  virtual ~IpMulticastManager() = default;

  void enqueue(const std::string& table_name,
               const swss::KeyOpFieldsValuesTuple& entry) override;
  ReturnCode drain() override;
  void drainWithNotExecuted() override;
  std::string verifyState(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& tuples) override;
  ReturnCode getSaiObject(const std::string& json_key,
                          sai_object_type_t& object_type,
                          std::string& object_key) override;

 private:
  // Converts db table entry into P4IpMulticastEntry.
  ReturnCodeOr<P4IpMulticastEntry> deserializeIpMulticastEntry(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& attributes,
      const std::string& table_name);

  // Gets the internal cached IP multicast entry by its key.
  // Return nullptr if corresponding entry is not cached.
  P4IpMulticastEntry* getIpMulticastEntry(
      const std::string& ip_multicast_entry_key);

  // Returns the SAI IPMC entry (for multicast).
  sai_ipmc_entry_t prepareSaiIpmcEntry(
      const P4IpMulticastEntry& ip_multicast_entry) const;

  // Creates and assigns the empty private RPF group, to be used for all
  // IPMC entries.
  ReturnCode createDefaultRpfGroup();

  // We temporarily need a RPF group member and router interface.
  // Creates and adds a single RPF group member to the default RPF group.
  ReturnCode createDefaultRpfGroupMember();
  // Creates a router interface object to be used by the RPF group member.
  ReturnCode createRouterInterfaceForDefaultRpfGroupMember();

  // Creates a list of IP multicast entries.
  std::vector<ReturnCode> createIpMulticastEntries(
      const std::vector<P4IpMulticastEntry>& ip_multicast_entries);

  // Updates a list of IP multicast entries.
  std::vector<ReturnCode> updateIpMulticastEntries(
      const std::vector<P4IpMulticastEntry>& ip_multicast_entries);

  // Deletes a list of IP multicast entries.
  std::vector<ReturnCode> deleteIpMulticastEntries(
      const std::vector<P4IpMulticastEntry>& ip_multicast_entries);

  // Internal cache of entries.
  P4IpMulticastTable m_ipMulticastTable;

  P4OidMapper* m_p4OidMapper;
  VRFOrch* m_vrfOrch;
  ResponsePublisherInterface* m_publisher;
  std::deque<swss::KeyOpFieldsValuesTuple> m_entries;

  // OID for a valid RPF group, needed for creating IPMC entries.
  // This group will be created on first entry add.  Ideally, this group would
  // be empty, but at least one member is needed by the SDK at the moment.
  sai_object_id_t ipmc_rpf_group_oid_ = SAI_NULL_OBJECT_ID;
  sai_object_id_t unused_rpf_group_member_oid_ = SAI_NULL_OBJECT_ID;
  sai_object_id_t rif_for_rpf_group_member_oid_ = SAI_NULL_OBJECT_ID;

  friend class IpMulticastManagerTest;
};

}  // namespace p4orch
