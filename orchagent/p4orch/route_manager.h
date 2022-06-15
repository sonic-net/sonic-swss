#pragma once

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

#include "ipprefix.h"
#include "orch.h"
#include "p4orch/next_hop_manager.h"
#include "p4orch/object_manager_interface.h"
#include "p4orch/p4oidmapper.h"
#include "p4orch/p4orch_util.h"
#include "response_publisher_interface.h"
#include "return_code.h"
#include "vrforch.h"
extern "C"
{
#include "sai.h"
}

struct P4RouteEntry
{
    std::string route_entry_key; // Unique key of a route entry.
    std::string vrf_id;
    swss::IpPrefix route_prefix;
    std::string action;
    std::string nexthop_id;
    std::string wcmp_group;
    sai_route_entry_t sai_route_entry;
};

// P4RouteTable: Route ID, P4RouteEntry
typedef std::unordered_map<std::string, P4RouteEntry> P4RouteTable;

class RouteManager : public ObjectManagerInterface
{
  public:
    RouteManager(P4OidMapper *p4oidMapper, VRFOrch *vrfOrch, ResponsePublisherInterface *publisher) : m_vrfOrch(vrfOrch)
    {
        SWSS_LOG_ENTER();

        assert(p4oidMapper != nullptr);
        m_p4OidMapper = p4oidMapper;
        assert(publisher != nullptr);
        m_publisher = publisher;
    }
    virtual ~RouteManager() = default;

    void enqueue(const swss::KeyOpFieldsValuesTuple &entry) override;
    void drain() override;

  private:
    // Applies route entry updates from src to dest. The merged result will be
    // stored in ret.
    // The src should have passed all validation checks.
    // Return true if there are updates, false otherwise.
    bool mergeRouteEntry(const P4RouteEntry &dest, const P4RouteEntry &src, P4RouteEntry *ret);

    // Converts db table entry into P4RouteEntry.
    ReturnCodeOr<P4RouteEntry> deserializeRouteEntry(const std::string &key,
                                                     const std::vector<swss::FieldValueTuple> &attributes,
                                                     const std::string &table_name);

    // Gets the internal cached route entry by its key.
    // Return nullptr if corresponding route entry is not cached.
    P4RouteEntry *getRouteEntry(const std::string &route_entry_key);

    // Validated non-empty fields in a route entry.
    ReturnCode validateRouteEntry(const P4RouteEntry &route_entry);

    // Performs route entry validation for SET command.
    ReturnCode validateSetRouteEntry(const P4RouteEntry &route_entry);

    // Performs route entry validation for DEL command.
    ReturnCode validateDelRouteEntry(const P4RouteEntry &route_entry);

    // Creates a route entry.
    // Returns a SWSS status code.
    ReturnCode createRouteEntry(const P4RouteEntry &route_entry);

    // Updates a route entry.
    // Returns a SWSS status code.
    ReturnCode updateRouteEntry(const P4RouteEntry &route_entry);

    // Deletes a route entry.
    // Returns a SWSS status code.
    ReturnCode deleteRouteEntry(const P4RouteEntry &route_entry);

    // Returns the nexthop OID for a given route entry.
    // This method will raise critical state if the OID cannot be found. So this
    // should only be called after validation.
    ReturnCodeOr<sai_object_id_t> getNexthopOid(const P4RouteEntry &route_entry);

    P4RouteTable m_routeTable;
    P4OidMapper *m_p4OidMapper;
    VRFOrch *m_vrfOrch;
    ResponsePublisherInterface *m_publisher;
    std::deque<swss::KeyOpFieldsValuesTuple> m_entries;

    friend class RouteManagerTest;
};
