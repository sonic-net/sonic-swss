#pragma once

#include "nhgbase.h"
#include "nexthopkey.h"
#include "vector"

using namespace std;

extern sai_object_id_t gSwitchId;
extern sai_next_hop_group_api_t* sai_next_hop_group_api;

enum class ProtNhgRole
{
    PRIMARY,
    STANDBY
};

/*
 * ProtNhgMember represents a member of a protection next hop group.
 * Each member has a configured role (primary or standby) and an optional
 * monitored object (e.g., ICMP echo session OID). A monitored object on the
 * primary member is what hands switchover control to the hardware.
 */
class ProtNhgMember : public NhgMember<NextHopKey>
{
public:
    ProtNhgMember(const NextHopKey &key, ProtNhgRole role,
                  const string &nhg_key = "");

    ProtNhgMember(ProtNhgMember &&nhgm);

    ~ProtNhgMember();

    void sync(sai_object_id_t gm_id) override;
    void remove() override;

    inline ProtNhgRole getRole() const { return m_role; }
    sai_object_id_t getNhId() const;

    /* True when this member represents a recursive NHG rather than an individual NH. */
    inline bool isRecursive() const { return !m_nhg_key.empty(); }

    inline sai_object_id_t getMonitoredObject() const { return m_monitored_oid; }
    void setMonitoredObject(sai_object_id_t oid) { m_monitored_oid = oid; }

    bool updateMonitoredObject(sai_object_id_t oid);

    /* Query the hardware-observed role (active/inactive) from SAI. */
    bool getObservedRole(sai_next_hop_group_member_observed_role_t &observed_role) const;

    string to_string() const override;

private:
    ProtNhgRole m_role;
    sai_object_id_t m_monitored_oid;
    string m_nhg_key;   /* Non-empty when member is a recursive NHG (resolved via NhgOrch). */
};

/*
 * ProtNhg represents a SAI protection next hop group. There is one SAI type
 * for every protection NHG, SAI_NEXT_HOP_GROUP_TYPE_PROTECTION.
 * It is a strict pair: exactly one primary next hop and exactly one standby
 * next hop, matching the SAI protection-group model (a primary-backup pair).
 * Enforced in sync(), not just by convention.
 *
 * The switchover mode is a runtime property of the group, derived from whether
 * a monitored object is attached to the primary member, not a second type:
 *
 *   - SW-driven (no monitored object): software decides and triggers the
 *     switchover via SAI_NEXT_HOP_GROUP_ATTR_SET_SWITCHOVER.
 *   - HW-autonomous (monitored object attached): the hardware switches
 *     between primary and standby on its own, driven by the monitored
 *     object's state. Software relinquishes the trigger and keeps only the
 *     SAI_NEXT_HOP_GROUP_ATTR_ADMIN_ROLE override.
 *
 * Attaching or detaching the monitored object moves the group between the two
 * modes in place; it never recreates the SAI group.
 *
 * Multi-primary (N:M) is expressed via the recursive NextHopGroupKey-pair
 * constructor, where the primary/standby members each point to an ECMP (or
 * fine-grained) NHG resolved through NhgOrch.
 */
class ProtNhg : public NhgCommon<string, NextHopKey, ProtNhgMember>
{
public:
    ProtNhg(const string &key,
            const NextHopKey &primary_nh,
            const NextHopKey &standby_nh);


    /* Construct from NextHopGroupKey pairs (recursive/nested NHG).
     * Each group becomes a single protection member whose SAI next-hop ID
     * is dynamically resolved via NhgOrch at sync time.
     */
    ProtNhg(const string &key,
            const NextHopGroupKey &primary_nhg_key,
            const NextHopGroupKey &standby_nhg_key);

    ProtNhg(ProtNhg &&nhg);

    ~ProtNhg() { SWSS_LOG_ENTER(); remove(); }

    bool sync() override;
    bool remove() override;

    /* Sync a member once its next hop becomes valid.  There is no invalidate
     * counterpart: a protection NHG keeps both legs programmed and switches
     * over rather than dropping a member. */
    bool validateNextHop(const NextHopKey &nh_key);

    inline bool isTemp() const override { return false; }
    inline NextHopGroupKey getNhgKey() const override { return {}; }

    /*
     * True while a monitored object is attached to the primary member, i.e.
     * while the hardware owns the switchover decision for this group.
     */
    bool isHwAutonomous() const;

    /* Override the hardware's choice of active leg -- HW-autonomous only.
     * SAI_NEXT_HOP_GROUP_ADMIN_ROLE_AUTO hands the choice back to hardware. */
    bool setAdminRole(sai_next_hop_group_admin_role_t admin_role);

    /* Trigger switchover from primary to standby -- SW-driven only. */
    bool setSwitchover(bool enable);

    bool updateMemberMonitoredObject(const NextHopKey &nh_key,
                                     sai_object_id_t monitored_oid);

    const ProtNhgMember* getPrimaryMember() const;
    const ProtNhgMember* getStandbyMember() const;

    /* Query a specific member's observed role from SAI. */
    bool getMemberObservedRole(const NextHopKey &nh_key,
                               sai_next_hop_group_member_observed_role_t &observed_role) const;

    /* Query observed roles for all synced members at once. */
    bool getAllMemberObservedRoles(
        map<NextHopKey, sai_next_hop_group_member_observed_role_t> &observed_roles) const;

    string to_string() const override { return m_key; }

private:
    bool syncMembers(const set<NextHopKey> &member_keys) override;
    vector<sai_attribute_t> createNhgmAttrs(const ProtNhgMember &member) const override;
};
