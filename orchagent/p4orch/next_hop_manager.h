#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "ipaddress.h"
#include "orch.h"
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
    swss::IpAddress neighbor_id;

    // SAI OID associated with this entry.
    sai_object_id_t next_hop_oid = 0;

    P4NextHopEntry(const std::string &next_hop_id, const std::string &router_interface_id,
                   const swss::IpAddress &neighbor_id);
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

    void enqueue(const swss::KeyOpFieldsValuesTuple &entry) override;
    void drain() override;

  private:
    // Gets the internal cached next hop entry by its key.
    // Return nullptr if corresponding next hop entry is not cached.
    P4NextHopEntry *getNextHopEntry(const std::string &next_hop_key);

    // Deserializes an entry from table APP_P4RT_NEXTHOP_TABLE_NAME.
    ReturnCodeOr<P4NextHopAppDbEntry> deserializeP4NextHopAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes);

    // Processes add operation for an entry.
    ReturnCode processAddRequest(const P4NextHopAppDbEntry &app_db_entry);

    // Creates an next hop in the next hop table. Return true on success.
    ReturnCode createNextHop(P4NextHopEntry &next_hop_entry);

    // Processes update operation for an entry.
    ReturnCode processUpdateRequest(const P4NextHopAppDbEntry &app_db_entry, P4NextHopEntry *next_hop_entry);

    // Processes delete operation for an entry.
    ReturnCode processDeleteRequest(const std::string &next_hop_key);

    // Deletes an next hop in the next hop table. Return true on success.
    ReturnCode removeNextHop(const std::string &next_hop_key);

    // m_nextHopTable: next_hop_key, P4NextHopEntry
    std::unordered_map<std::string, P4NextHopEntry> m_nextHopTable;

    // Owners of pointers below must outlive this class's instance.
    P4OidMapper *m_p4OidMapper;
    ResponsePublisherInterface *m_publisher;
    std::deque<swss::KeyOpFieldsValuesTuple> m_entries;

    friend class NextHopManagerTest;
};
