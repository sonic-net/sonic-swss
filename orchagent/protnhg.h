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
 * A member is identified by its role, not by what it forwards to: the role is
 * the only identity that is known up front and never changes, whereas the
 * target may be an individual next hop or a nested NHG, and may not be
 * resolvable yet.
 *
 * A member may also carry a monitored object (e.g., ICMP echo session OID). A
 * monitored object on the primary member is what hands switchover control to
 * the hardware.
 */
class ProtNhgMember : public NhgMember<ProtNhgRole>
{
public:
    /*
     * A member backed by an individual next hop is created with nhg_key empty
     * and resolves through NeighOrch. A non-empty nhg_key makes the member
     * recursive: it resolves through NhgOrch instead, and nh_key is unused.
     */
    ProtNhgMember(ProtNhgRole role, const NextHopKey &nh_key,
                  const string &nhg_key = "");

    ProtNhgMember(ProtNhgMember &&nhgm);

    ~ProtNhgMember();

    void sync(sai_object_id_t gm_id) override;
    void remove() override;

    inline ProtNhgRole getRole() const { return m_key; }
    sai_object_id_t getNhId() const;

    /* True when this member represents a recursive NHG rather than an individual NH. */
    inline bool isRecursive() const { return !m_nhg_key.empty(); }

    /* The individual next hop this member resolves through. Meaningless for a
     * recursive member, which has no single next hop of its own. */
    inline const NextHopKey& getNextHopKey() const { return m_nh_key; }

    inline sai_object_id_t getMonitoredObject() const { return m_monitored_oid; }
    void setMonitoredObject(sai_object_id_t oid) { m_monitored_oid = oid; }

    bool updateMonitoredObject(sai_object_id_t oid);

    /* Query the hardware-observed role (active/inactive) from SAI. */
    bool getObservedRole(sai_next_hop_group_member_observed_role_t &observed_role) const;

    string to_string() const override;

private:
    NextHopKey m_nh_key;
    string m_nhg_key;   /* Non-empty when member is a recursive NHG (resolved via NhgOrch). */
    sai_object_id_t m_monitored_oid;
};

/*
 * ProtNhg represents a SAI protection next hop group. There is one SAI type
 * for every protection NHG, SAI_NEXT_HOP_GROUP_TYPE_PROTECTION.
 * It is a strict pair: exactly one primary next hop and exactly one standby
 * next hop, matching the SAI protection-group model (a primary-backup pair).
 * Keying members by role is what makes the pair structural rather than a
 * convention sync() has to police.
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
class ProtNhg : public NhgCommon<string, ProtNhgRole, ProtNhgMember>
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

    /*
     * Role is how a member is addressed; the next-hop forms are conveniences
     * for a group whose legs are individual next hops, and never match a
     * recursive member.
     */
    bool updateMemberMonitoredObject(ProtNhgRole role,
                                     sai_object_id_t monitored_oid);
    bool updateMemberMonitoredObject(const NextHopKey &nh_key,
                                     sai_object_id_t monitored_oid);

    /* Query a specific member's observed role from SAI. */
    bool getMemberObservedRole(ProtNhgRole role,
                               sai_next_hop_group_member_observed_role_t &observed_role) const;
    bool getMemberObservedRole(const NextHopKey &nh_key,
                               sai_next_hop_group_member_observed_role_t &observed_role) const;

    /* Query observed roles for all synced members at once. */
    bool getAllMemberObservedRoles(
        map<ProtNhgRole, sai_next_hop_group_member_observed_role_t> &observed_roles) const;

    const ProtNhgMember* getMember(ProtNhgRole role) const;
    const ProtNhgMember* getPrimaryMember() const;
    const ProtNhgMember* getStandbyMember() const;

    /* Resolve which role the member forwarding to nh_key holds. Fails when no
     * member resolves through that next hop. */
    bool getMemberRole(const NextHopKey &nh_key, ProtNhgRole &role) const;

    string to_string() const override { return m_key; }

private:
    const ProtNhgMember* findMemberByNextHop(const NextHopKey &nh_key) const;
    ProtNhgMember* findMemberByNextHop(const NextHopKey &nh_key);

    bool syncMembers(const set<ProtNhgRole> &member_keys) override;
    vector<sai_attribute_t> createNhgmAttrs(const ProtNhgMember &member) const override;
};
