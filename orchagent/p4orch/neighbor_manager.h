#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ipaddress.h"
#include "macaddress.h"
#include "orch.h"
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

struct P4NeighborEntry
{
    std::string router_intf_id;
    swss::IpAddress neighbor_id;
    swss::MacAddress dst_mac_address;
    std::string router_intf_key;
    std::string neighbor_key;
    sai_neighbor_entry_t neigh_entry;

    P4NeighborEntry() = default;
    P4NeighborEntry(const std::string &router_interface_id, const swss::IpAddress &ip_address,
                    const swss::MacAddress &mac_address);
};

// P4NeighborTable: Neighbor key string, P4NeighborEntry
typedef std::unordered_map<std::string, P4NeighborEntry> P4NeighborTable;

class NeighborManager : public ObjectManagerInterface
{
  public:
    NeighborManager(P4OidMapper *p4oidMapper, ResponsePublisherInterface *publisher)
    {
        SWSS_LOG_ENTER();

        assert(p4oidMapper != nullptr);
        m_p4OidMapper = p4oidMapper;
        assert(publisher != nullptr);
        m_publisher = publisher;
    }
    virtual ~NeighborManager() = default;

    void enqueue(const swss::KeyOpFieldsValuesTuple &entry) override;
    void drain() override;

  private:
    ReturnCodeOr<P4NeighborAppDbEntry> deserializeNeighborEntry(const std::string &key,
                                                                const std::vector<swss::FieldValueTuple> &attributes);
    ReturnCode validateNeighborAppDbEntry(const P4NeighborAppDbEntry &app_db_entry);
    P4NeighborEntry *getNeighborEntry(const std::string &neighbor_key);
    ReturnCode createNeighbor(P4NeighborEntry &neighbor_entry);
    ReturnCode removeNeighbor(const std::string &neighbor_key);
    ReturnCode setDstMacAddress(P4NeighborEntry *neighbor_entry, const swss::MacAddress &mac_address);
    ReturnCode processAddRequest(const P4NeighborAppDbEntry &app_db_entry, const std::string &neighbor_key);
    ReturnCode processUpdateRequest(const P4NeighborAppDbEntry &app_db_entry, P4NeighborEntry *neighbor_entry);
    ReturnCode processDeleteRequest(const std::string &neighbor_key);

    P4OidMapper *m_p4OidMapper;
    P4NeighborTable m_neighborTable;
    ResponsePublisherInterface *m_publisher;
    std::deque<swss::KeyOpFieldsValuesTuple> m_entries;

    friend class NeighborManagerTest;
};
