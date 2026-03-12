#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "ipaddress.h"
#include "orch.h"
#include "p4orch/gre_tunnel_manager.h"
#include "p4orch/neighbor_manager.h"
#include "p4orch/object_manager_interface.h"
#include "p4orch/p4oidmapper.h"
#include "p4orch/p4orch_util.h"
#include "p4orch/router_interface_manager.h"
#include "response_publisher_interface.h"
#include "return_code.h"
extern "C"
{
#include "sai.h"
}

// P4NextHopEntry holds NextHopManager's internal cache of P4 next hop entry.
struct P4NextHopEntry
{
    // Key of this entry, built from next_hop_id.
    std::string next_hop_key;

    // Fields from P4 table.
    // Match
    std::string next_hop_id;
    // Action
    std::string router_interface_id;
    std::string gre_tunnel_id;
    swss::IpAddress neighbor_id;
    bool disable_decrement_ttl = false;
    bool disable_src_mac_rewrite = false;
    bool disable_dst_mac_rewrite = false;
    bool disable_vlan_rewrite = false;

    // SAI OID associated with this entry.
    sai_object_id_t next_hop_oid = SAI_NULL_OBJECT_ID;

    P4NextHopEntry(const std::string& next_hop_id,
                   const std::string& router_interface_id,
                   const std::string& gre_tunnel_id,
                   const swss::IpAddress& neighbor_id,
                   bool disable_decrement_ttl = false,
                   bool disable_src_mac_rewrite = false,
                   bool disable_dst_mac_rewrite = false,
                   bool disable_vlan_rewrite = false);
};

// NextHopManager listens to changes in table APP_P4RT_NEXTHOP_TABLE_NAME and
// creates/updates/deletes next hop SAI object accordingly.
class NextHopManager : public ObjectManagerInterface
{
  public:
    NextHopManager(P4OidMapper *p4oidMapper, ResponsePublisherInterface *publisher)
    {
        SWSS_LOG_ENTER();

        assert(p4oidMapper != nullptr);
        m_p4OidMapper = p4oidMapper;
        assert(publisher != nullptr);
        m_publisher = publisher;
    }

    virtual ~NextHopManager() = default;

    void enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry) override;
    ReturnCode drain() override;
    void drainWithNotExecuted() override;
    std::string verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple) override;
    ReturnCode getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                            std::string &object_key) override;

  private:
    // Gets the internal cached next hop entry by its key.
    // Return nullptr if corresponding next hop entry is not cached.
    P4NextHopEntry *getNextHopEntry(const std::string &next_hop_key);

    // Deserializes an entry from table APP_P4RT_NEXTHOP_TABLE_NAME.
    ReturnCodeOr<P4NextHopAppDbEntry> deserializeP4NextHopAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes);

    ReturnCode validateAppDbEntry(const P4NextHopAppDbEntry& app_db_entry);

    ReturnCode validateAppDbEntry(const P4NextHopAppDbEntry& app_db_entry,
                                  const std::string& operation);

    // Creates a next hop in the next hop table.
    std::vector<ReturnCode> createNextHops(
        const std::vector<P4NextHopAppDbEntry>& next_hop_entries);

    // Deletes a next hop in the next hop table.
    std::vector<ReturnCode> removeNextHops(
        const std::vector<P4NextHopAppDbEntry>& next_hop_entries);

    // Processes delete operation for an entry.
    ReturnCode processEntries(
        const std::vector<P4NextHopAppDbEntry>& next_hop_entries,
        const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
        const std::string& op, bool update);

    // Verifies internal cache for an entry.
    std::string verifyStateCache(const P4NextHopAppDbEntry &app_db_entry, const P4NextHopEntry *next_hop_entry);

    // Verifies ASIC DB for an entry.
    std::string verifyStateAsicDb(const P4NextHopEntry *next_hop_entry);

    // Returns the SAI attributes for an entry.
    std::vector<sai_attribute_t> getSaiAttrs(
        const P4NextHopEntry& next_hop_entry);

    // m_nextHopTable: next_hop_key, P4NextHopEntry
    std::unordered_map<std::string, P4NextHopEntry> m_nextHopTable;

    // Owners of pointers below must outlive this class's instance.
    P4OidMapper *m_p4OidMapper;
    ResponsePublisherInterface *m_publisher;
    std::deque<swss::KeyOpFieldsValuesTuple> m_entries;

    friend class NextHopManagerTest;
};
