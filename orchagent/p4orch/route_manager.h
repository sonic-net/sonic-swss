#pragma once

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "bulker.h"
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
    std::string route_metadata;  // go/gpins-pinball-vip-stats
    sai_route_entry_t sai_route_entry;
};

// P4RouteTable: Route ID, P4RouteEntry
typedef std::unordered_map<std::string, P4RouteEntry> P4RouteTable;

// RouteUpdater is a helper class in performing route update.
// It keeps track of the state of the route update. It provides the next SAI
// attribute required in the route update.
// RouteUpdater will raise critical state if recovery fails or nexthop OID
// cannot be found.
class RouteUpdater
{
  public:
    RouteUpdater(const P4RouteEntry &old_route, const P4RouteEntry &new_route, P4OidMapper *mapper);
    ~RouteUpdater() = default;

    P4RouteEntry getOldEntry() const;
    P4RouteEntry getNewEntry() const;
    sai_route_entry_t prepareSaiEntry() const;
    // Returns the next SAI attribute that should be performed.
    sai_attribute_t prepareSaiAttr() const;
    // Updates the state by the given SAI result.
    // Returns true if all operations are completed.
    // This method will raise critical state if a recovery action fails.
    bool updateResult(sai_status_t sai_status);
    // Returns the overall status of the route update.
    // This method should only be called after UpdateResult returns true.
    ReturnCode getStatus() const;
    // Returns a list of SAI attributes that the update needs to preform from
    // the current state to the final state. If the updater is in revert mode,
    // the final state is the old route entry.
    std::vector<sai_attribute_t> GetSaiAttrList() const;

   private:
    // Updates the action index.
    // Returns true if there are no more actions.
    bool updateIdx();
    bool updateIdx(int& idx) const;
    // Checks if the current action should be performed or not.
    // Returns true if the action should be performed.
    bool checkAction(int idx) const;
    // Returns the SAI attribute that should be performed by the given index.
    sai_attribute_t prepareSaiAttr(int idx) const;

    P4OidMapper *m_p4OidMapper;
    P4RouteEntry m_oldRoute;
    P4RouteEntry m_newRoute;
    ReturnCode m_status;
    std::vector<sai_route_entry_attr_t> m_actions;
    bool m_revert = false;
    int m_idx = -1;
};

class RouteManager : public ObjectManagerInterface
{
  public:
    RouteManager(P4OidMapper *p4oidMapper, VRFOrch *vrfOrch, ResponsePublisherInterface *publisher);
    virtual ~RouteManager() = default;

    void enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry) override;
    ReturnCode drain() override;
    void drainWithNotExecuted() override;
    std::string verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple) override;
    ReturnCode getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                            std::string &object_key) override;

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

    // Performs route entry validation.
    ReturnCode validateRouteEntry(const P4RouteEntry &route_entry, const std::string &operation);

    // Performs route entry validation for SET command.
    ReturnCode validateSetRouteEntry(const P4RouteEntry &route_entry);

    // Performs route entry validation for DEL command.
    ReturnCode validateDelRouteEntry(const P4RouteEntry &route_entry);

    // Creates a list of route entries.
    std::vector<ReturnCode> createRouteEntries(const std::vector<P4RouteEntry> &route_entries);

    // Updates a list of route entries.
    std::vector<ReturnCode> updateRouteEntries(const std::vector<P4RouteEntry> &route_entries);

    // Deletes a list of route entries.
    std::vector<ReturnCode> deleteRouteEntries(const std::vector<P4RouteEntry> &route_entries);

    // Process a list of route entries by the given operation.
    ReturnCode processRouteEntries(
        const std::vector<P4RouteEntry>& route_entries,
        const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
        const std::string& op, bool update);

    // On a successful route entry update, updates the reference counters and
    // internal data.
    void updateRouteEntriesMeta(const P4RouteEntry &old_entry, const P4RouteEntry &new_entry);

    // Auxiliary method to perform route update.
    void updateRouteAttrs(int size, const std::vector<std::unique_ptr<RouteUpdater>> &updaters,
                          std::vector<size_t> &indice, std::vector<ReturnCode> &statuses);

    // Verifies internal cache for an entry.
    std::string verifyStateCache(const P4RouteEntry &app_db_entry, const P4RouteEntry *route_entry);

    // Verifies ASIC DB for an entry.
    std::string verifyStateAsicDb(const P4RouteEntry *route_entry);

    // Returns the SAI entry.
    sai_route_entry_t prepareSaiEntry(const P4RouteEntry& route_entry);

    P4RouteTable m_routeTable;
    P4OidMapper *m_p4OidMapper;
    VRFOrch *m_vrfOrch;
    EntityBulker<sai_route_api_t> m_routerBulker;
    ResponsePublisherInterface *m_publisher;
    std::deque<swss::KeyOpFieldsValuesTuple> m_entries;

    friend class RouteManagerTest;
};
