#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "orch.h"
#include "p4orch/object_manager_interface.h"
#include "p4orch/p4oidmapper.h"
#include "p4orch/p4orch_util.h"
#include "response_publisher_interface.h"
#include "return_code.h"
#include "vrforch.h"
extern "C" {
#include "sai.h"
}

// Ipv6TunnelTermTableEntry holds TunnelDecapGroupManager's internal cache of
// tunnel termination table entry. Example:
// P4RT:FIXED_IPV6_TUNNEL_TERMINATION_TABLE:{"match/dst_ipv6_64bit":
//   "2607:f8b0:c145:9300:: & ffff:ffff:ffff:ff00::"}
//   "action" = "mark_for_tunnel_decap_and_set_vrf",
//   "param/vrf_id" = "b4-traffic",
//   "controller_metadata" = "..."
// LINT.IfChange
struct Ipv6TunnelTermTableEntry {
  // Unique key of this entry.
  std::string ipv6_tunnel_term_key;

  // Fields from P4 table.
  // Match
  swss::IpAddress dst_ipv6_ip;
  swss::IpAddress dst_ipv6_mask;
  // Action
  std::string vrf_id;

  // SAI OID associated with this entry.
  sai_object_id_t ipv6_tunnel_term_oid = SAI_NULL_OBJECT_ID;
  // SAI OID of the vrf_id for SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID
  sai_object_id_t vrf_oid = SAI_NULL_OBJECT_ID;

  Ipv6TunnelTermTableEntry(const swss::IpAddress& dst_ipv6_ip,
                           const swss::IpAddress& dst_ipv6_mask,
                           const std::string& vrf_id);
};
// LINT.ThenChange(tunnel_decap_group_manager.cpp:verify_state_cache)

// TunnelDecapGroupManager listens to changes in table
// APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME and creates/updates/deletes
// IPv6 tunnel termination table entry SAI object accordingly.
class TunnelDecapGroupManager : public ObjectManagerInterface {
 public:
  TunnelDecapGroupManager(P4OidMapper* p4oidMapper, VRFOrch* vrfOrch,
                          ResponsePublisherInterface* publisher);

  virtual ~TunnelDecapGroupManager() = default;

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
  // Gets the internal cached IPv6 tunnel termination table entry by its key.
  // Return nullptr if corresponding IPv6 tunnel termination table entry is
  // not cached.
  Ipv6TunnelTermTableEntry* getIpv6TunnelTermEntry(
      const std::string& ipv6_tunnel_term_key);

  // Deserializes an entry from table
  // APP_P4RT_IPV6_TUNNEL_TERMINATION_TABLE_NAME.
  ReturnCodeOr<Ipv6TunnelTermAppDbEntry> deserializeIpv6TunnelTermAppDbEntry(
      const std::string& key,
      const std::vector<swss::FieldValueTuple>& attributes);

  ReturnCode validateIpv6TunnelTermAppDbEntry(
      const Ipv6TunnelTermAppDbEntry& app_db_entry);

  ReturnCode validateIpv6TunnelTermAppDbEntry(
      const Ipv6TunnelTermAppDbEntry& app_db_entry,
      const std::string& operation);

  // Creates IPv6 tunnel termination table entries in the IPv6 tunnel
  // termination table.
  std::vector<ReturnCode> createIpv6TunnelTermEntries(
      const std::vector<Ipv6TunnelTermAppDbEntry>& ipv6_tunnel_term_entries);

  // Deletes IPv6 tunnel termination table entries in the IPv6 tunnel
  // termination table.
  std::vector<ReturnCode> removeIpv6TunnelTermEntries(
      const std::vector<Ipv6TunnelTermAppDbEntry>& ipv6_tunnel_term_entries);

  ReturnCode processEntries(
      const std::vector<Ipv6TunnelTermAppDbEntry>& entries,
      const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
      const std::string& op, bool update);

  std::string verifyStateCache(
      const Ipv6TunnelTermAppDbEntry& app_db_entry,
      const Ipv6TunnelTermTableEntry* ipv6_tunnel_term_entries);
  std::string verifyStateAsicDb(
      const Ipv6TunnelTermTableEntry* ipv6_tunnel_term_entries);

  // m_ipv6TunnelTermTable: ipv6_tunnel_term_key, Ipv6TunnelTermTableEntry
  std::unordered_map<std::string, Ipv6TunnelTermTableEntry>
      m_ipv6TunnelTermTable;

  // Owners of pointers below must outlive this class's instance.
  P4OidMapper* m_p4OidMapper;
  VRFOrch* m_vrfOrch;
  ResponsePublisherInterface* m_publisher;
  std::deque<swss::KeyOpFieldsValuesTuple> m_entries;

  friend class TunnelDecapGroupManagerTest;
};

