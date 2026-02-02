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

  // Internal cache of entries.
  P4IpMulticastTable m_ipMulticastTable;

  P4OidMapper* m_p4OidMapper;
  VRFOrch* m_vrfOrch;
  ResponsePublisherInterface* m_publisher;
  std::deque<swss::KeyOpFieldsValuesTuple> m_entries;

  friend class IpMulticastManagerTest;
};

}  // namespace p4orch
