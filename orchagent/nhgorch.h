#pragma once

#include "cbf/cbfnhgorch.h"
#include "protnhg.h"
#include "vector"
#include "portsorch.h"
#include "routeorch.h"

using namespace std;

extern PortsOrch *gPortsOrch;
extern RouteOrch *gRouteOrch;

class NextHopGroupMember : public NhgMember<NextHopKey>
{
public:
    /* Constructors / Assignment operators. */
    NextHopGroupMember(const NextHopKey& nh_key) :
        NhgMember(nh_key) {}

    NextHopGroupMember(NextHopGroupMember&& nhgm) :
        NhgMember(move(nhgm)) {}

    /* Destructor. */
    ~NextHopGroupMember();

    /* Update member's weight and update the SAI attribute as well. */
    bool updateWeight(uint32_t weight);

    /* Sync / Remove. */
    void sync(sai_object_id_t gm_id) override;
    void remove() override;

    /* Getters / Setters. */
    inline uint32_t getWeight() const { return m_key.weight; }
    sai_object_id_t getNhId() const;

    /* Check if the next hop is labeled. */
    inline bool isLabeled() const { return !m_key.label_stack.empty(); }

    /* Convert member's details to string. */
    string to_string() const override
    {
        return m_key.to_string() + ", SAI ID: " + std::to_string(m_gm_id);
    }
};

/*
 * NextHopGroup class representing a next hop group object.
 */
class NextHopGroup : public NhgCommon<NextHopGroupKey, NextHopKey, NextHopGroupMember>
{
public:
    /* Constructors. */
    explicit NextHopGroup(const NextHopGroupKey& key, bool is_temp);

    NextHopGroup(NextHopGroup&& nhg) :
        NhgCommon(move(nhg)), m_is_temp(nhg.m_is_temp), m_is_recursive(nhg.m_is_recursive)
    { SWSS_LOG_ENTER(); }

    NextHopGroup& operator=(NextHopGroup&& nhg);

    /* Destructor. */
    virtual ~NextHopGroup() { remove(); }

    /* Sync the group, creating the group's and members SAI IDs. */
    bool sync() override;

    /* Remove the group, reseting the group's and members SAI IDs.  */
    bool remove() override;

    /*
     * Update the group based on a new next hop group key.  This will also
     * perform any sync / remove necessary.
     */
    bool update(const NextHopGroupKey& nhg_key);

    /* Validate a next hop in the group, syncing it. */
    bool validateNextHop(const NextHopKey& nh_key);

    /* Invalidate a next hop in the group, removing it. */
    bool invalidateNextHop(const NextHopKey& nh_key);

    /* Getters / Setters. */
    inline bool isTemp() const override { return m_is_temp; }

    inline bool isRecursive() const { return m_is_recursive; }

    inline void setRecursive(bool is_recursive) { m_is_recursive = is_recursive; }

    NextHopGroupKey getNhgKey() const override { return m_key; }

    /* Convert NHG's details to a string. */
    std::string to_string() const override
    {
        return m_key.to_string() + ", SAI ID: " + std::to_string(m_id);
    }

private:
    /* Whether the group is temporary or not. */
    bool m_is_temp;

    /* Whether the group is recursive i.e. having other nexthop group(s) as members */
    bool m_is_recursive;

    /* Add group's members over the SAI API for the given keys. */
    bool syncMembers(const set<NextHopKey>& nh_keys) override;

    /* Create the attributes vector for a next hop group member. */
    vector<sai_attribute_t> createNhgmAttrs(
                                const NextHopGroupMember& nhgm) const override;
};

/*
 * Next Hop Group Orchestrator class that handles NEXTHOP_GROUP_TABLE
 * updates.
 */
class NhgOrch : public NhgOrchCommon<NextHopGroup>
{
public:
    /*
     * Constructor.
     */
    NhgOrch(DBConnector *db, string tableName);

    /* Add a temporary next hop group when resources are exhausted. */
    NextHopGroup createTempNhg(const NextHopGroupKey& nhg_key);

    /* Validate / Invalidate a next hop. */
    bool validateNextHop(const NextHopKey& nh_key);
    bool invalidateNextHop(const NextHopKey& nh_key);

    /* Check if the ASIC supports protection NHGs
     * (SAI_NEXT_HOP_GROUP_TYPE_PROTECTION). */
    bool isProtectionSupported();

    /* Check if the ASIC can switch over autonomously, i.e. whether it accepts
     * any object as a SAI_NEXT_HOP_GROUP_MEMBER_ATTR_MONITORED_OBJECT. */
    bool isHwSwitchoverSupported();

    /* The object types the ASIC accepts as a monitored object. This list is
     * authoritative: a type absent from it is rejected without a SAI call.
     * Empty means the ASIC cannot switch over on its own. */
    const set<sai_object_type_t>& getSupportedMonitoredObjectTypes();

    /* Check if the ASIC accepts SAI_NEXT_HOP_GROUP_TYPE_HW_PROTECTION for the
     * standby leg's recursive NHG. This is a backup-group hint only: it says
     * nothing about protection or switchover support, and no protection NHG is
     * ever created with that type. */
    bool isBackupGroupHintSupported();

    /*
     * Protection NHG APIs.
     * MuxOrch is the primary consumer of these for dual-ToR protection
     * switching. Capacity accounting is shared with ECMP NHGs.
     *
     * All createProtNhg overloads are idempotent: re-creating with an
     * existing canonical key is a no-op that returns true. Membership
     * is immutable once created -- callers wishing to change membership
     * must removeProtNhg() first.
     *
     * Return value reflects registration, not full member sync: a group
     * with an unresolved next hop still returns true and self-heals via
     * validateNextHop(); query member state directly (e.g. getProtNhg())
     * if that distinction matters to the caller.
     *
     * Every group is created as SAI_NEXT_HOP_GROUP_TYPE_PROTECTION and starts
     * out SW-driven; attachProtNhgMonitoredObject() hands switchover control
     * to the hardware later. Each overload rejects creation up front if the
     * ASIC does not support protection NHGs (see isProtectionSupported()).
     */

    /* Create a protection NHG as a strict pair: one primary and one standby
     * next hop. Individual NHs are resolved via NeighOrch at sync time.
     * For N:M, use the recursive NextHopGroupKey-pair overloads below.
     */
    bool createProtNhg(const string &key,
                       const NextHopKey &primary_nh,
                       const NextHopKey &standby_nh);

    /* Auto-keyed convenience overload -- key is derived from the members. */
    bool createProtNhg(const NextHopKey &primary_nh,
                       const NextHopKey &standby_nh);

    /* Create a protection NHG where each role is an existing ECMP NHG.
     * NHG OIDs are dynamically resolved via NhgOrch at sync time.
     */
    bool createProtNhg(const string &key,
                       const NextHopGroupKey &primary_nhg_key,
                       const NextHopGroupKey &standby_nhg_key);

    /* Auto-keyed convenience overload -- key is derived from the group keys. */
    bool createProtNhg(const NextHopGroupKey &primary_nhg_key,
                       const NextHopGroupKey &standby_nhg_key);

    /* Build the deterministic key for a protection NHG from its members.
     * Membership alone identifies the group: the key carries no type or mode
     * discriminator, so it survives attach/detach of the monitored object. */
    static string buildProtNhgKey(const NextHopKey &primary_nh,
                                  const NextHopKey &standby_nh);
    static string buildProtNhgKey(const NextHopGroupKey &primary_nhg_key,
                                  const NextHopGroupKey &standby_nhg_key);

    /* Remove a protection NHG by key. */
    bool removeProtNhg(const string &key);

    /* Check if a protection NHG exists. */
    bool hasProtNhg(const string &key) const;

    /* Get a const reference to a protection NHG. */
    const ProtNhg& getProtNhg(const string &key) const;

    /* Get the SAI object ID of a protection NHG. */
    sai_object_id_t getProtNhgId(const string &key) const;

    /* Override the leg the hardware picked -- HW-autonomous groups only. */
    bool setProtNhgAdminRole(const string &key,
                             sai_next_hop_group_admin_role_t admin_role);

    /* Trigger switchover from primary to standby -- SW-driven groups only. */
    bool setProtNhgSwitchover(const string &key, bool enable);

    /*
     * Attach a monitored object to a protection NHG member, promoting the
     * group from SW-driven to HW-autonomous. Rejected if the object's type is
     * not in getSupportedMonitoredObjectTypes(); the group is left SW-driven
     * and forwarding is unaffected, since both legs are already programmed.
     */
    bool attachProtNhgMonitoredObject(const string &key,
                                      const NextHopKey &nh_key,
                                      sai_object_id_t monitored_oid);

    /* Detach the monitored object, demoting the group back to SW-driven. */
    bool detachProtNhgMonitoredObject(const string &key,
                                      const NextHopKey &nh_key);

    /* Query the hardware-observed role (active/inactive) of a protection NHG member. */
    bool getProtNhgMemberObservedRole(const string &key,
                                      const NextHopKey &nh_key,
                                      sai_next_hop_group_member_observed_role_t &observed_role) const;

    /* Query observed roles for all synced members of a protection NHG. */
    bool getProtNhgAllObservedRoles(
        const string &key,
        map<NextHopKey, sai_next_hop_group_member_observed_role_t> &observed_roles) const;

    /* Ref counting for protection NHGs. */
    void incProtNhgRefCount(const string &key);
    void decProtNhgRefCount(const string &key);

private:
    void doTask(Consumer& consumer) override;

    /* Probe the ASIC once for protection-NHG support, hardware switchover
     * support, and the backup-group hint, then publish the results to
     * STATE_DB|SWITCH_CAPABILITY. Subsequent calls are no-ops. */
    void probeProtectionCapabilities();

    /* Read the object types the ASIC accepts as a monitored object. */
    void probeMonitoredObjectTypes();

    /* Serialize m_monitoredObjectTypes for STATE_DB. */
    string monitoredObjectTypesToString() const;

    /* Cached protection-capability probe results. */
    bool m_protCapChecked = false;
    bool m_protectionSupported = false;
    bool m_backupGroupHintSupported = false;
    set<sai_object_type_t> m_monitoredObjectTypes;

    /* Storage for protection NHGs, keyed by a string identifier (e.g., port name). */
    unordered_map<string, NhgEntry<ProtNhg>> m_protNhgs;
};
