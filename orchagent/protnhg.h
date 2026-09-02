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

/* What a protection member resolves its SAI next hop ID from. */
enum class ProtNhgMemberType
{
    NEXT_HOP,     /* An individual next hop, resolved through NeighOrch. */
    SHARED_NHG,   /* A nested NHG held by NhgOrch, referenced by its index. */
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
    /* A member backed by an individual next hop, resolved via NeighOrch. */
    static ProtNhgMember nextHop(ProtNhgRole role, const NextHopKey &nh_key);

    /*
     * A member backed by a nested NHG that NhgOrch owns, named by its APP_DB
     * index. The index is the only identity NhgOrch can resolve; a group's
     * membership string is a different key space and deliberately not used
     * here. The nested group is ref counted for as long as this member is
     * synced, so NhgOrch will refuse to remove it out from under us.
     */
    static ProtNhgMember sharedNhg(ProtNhgRole role, const string &nhg_index);

    ProtNhgMember(ProtNhgMember &&nhgm);

    ~ProtNhgMember();

    void sync(sai_object_id_t gm_id) override;
    void remove() override;

    inline ProtNhgRole getRole() const { return m_key; }
    inline ProtNhgMemberType getType() const { return m_type; }

    /* True when this member resolves to a NHG rather than an individual NH. */
    inline bool isRecursive() const
        { return m_type != ProtNhgMemberType::NEXT_HOP; }

    /* Meaningful only for NEXT_HOP members; a default NextHopKey otherwise. */
    inline const NextHopKey& getNextHopKey() const { return m_nh_key; }

    /* Meaningful only for SHARED_NHG members; empty otherwise. */
    inline const string& getNhgIndex() const { return m_nhg_index; }

    /*
     * Resolved on every call rather than cached, so a member always programs
     * against the nested group's current SAI ID.
     */
    sai_object_id_t getNhId() const;

    inline sai_object_id_t getMonitoredObject() const { return m_monitored_oid; }
    void setMonitoredObject(sai_object_id_t oid) { m_monitored_oid = oid; }

    bool updateMonitoredObject(sai_object_id_t oid);

    /*
     * Re-point a synced member at its target's current SAI ID. NhgOrch may
     * replace a shared group's SAI object when its membership changes, and
     * the member's next hop is CREATE_AND_SET, so the member can follow it
     * without being torn down. A no-op when the ID has not moved.
     */
    bool updateNhId();

    /* Query the hardware-observed role (active/inactive) from SAI. */
    bool getObservedRole(sai_next_hop_group_member_observed_role_t &observed_role) const;

    string to_string() const override;

private:
    ProtNhgMember(ProtNhgRole role,
                  ProtNhgMemberType type,
                  const NextHopKey &nh_key,
                  const string &nhg_index);

    ProtNhgMemberType m_type;

    NextHopKey m_nh_key;    /* NEXT_HOP only. */
    string m_nhg_index;     /* SHARED_NHG only. */

    /* The next hop ID currently programmed into the SAI member. */
    sai_object_id_t m_programmed_nh_id;

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
 * Multi-primary (N:M) is expressed by giving either role a nested NHG member;
 * the two legs need not be the same member type.
 */
class ProtNhg : public NhgCommon<string, ProtNhgRole, ProtNhgMember>
{
public:
    /*
     * The general form: an explicit primary/standby member pair, so the two
     * legs can be of different member types.
     */
    ProtNhg(const string &key, ProtNhgMember &&primary, ProtNhgMember &&standby);

    /* Convenience form for the common case of two individual next hops. */
    ProtNhg(const string &key,
            const NextHopKey &primary_nh,
            const NextHopKey &standby_nh);

    ProtNhg(ProtNhg &&nhg);

    ~ProtNhg() { SWSS_LOG_ENTER(); remove(); }

    bool sync() override;
    bool remove() override;

    /*
     * Re-point whichever member resolves through the shared NHG named by
     * nhg_index, after NhgOrch has updated that group. Nothing to do for a
     * group this protection NHG does not reference.
     */
    bool updateSharedMember(const string &nhg_index);

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

    /* Role-addressed member operations; these reach every member type. */
    bool updateMemberMonitoredObject(ProtNhgRole role,
                                     sai_object_id_t monitored_oid);
    bool getMemberObservedRole(ProtNhgRole role,
                               sai_next_hop_group_member_observed_role_t &observed_role) const;

    /*
     * Next-hop-addressed convenience forms. They only ever match a NEXT_HOP
     * member, since a nested NHG has no single next hop to address it by.
     */
    bool updateMemberMonitoredObject(const NextHopKey &nh_key,
                                     sai_object_id_t monitored_oid);
    bool getMemberObservedRole(const NextHopKey &nh_key,
                               sai_next_hop_group_member_observed_role_t &observed_role) const;

    /* Query observed roles for all synced members at once. */
    bool getAllMemberObservedRoles(
        map<ProtNhgRole, sai_next_hop_group_member_observed_role_t> &observed_roles) const;

    const ProtNhgMember* getMember(ProtNhgRole role) const;
    const ProtNhgMember* getPrimaryMember() const;
    const ProtNhgMember* getStandbyMember() const;

    /*
     * Resolve the role of the NEXT_HOP member addressed by nh_key, so callers
     * holding a next hop can reach the role-addressed API.  False when no
     * NEXT_HOP member matches.
     */
    bool getMemberRole(const NextHopKey &nh_key, ProtNhgRole &role) const;

    string to_string() const override { return m_key; }

private:
    /* Find the NEXT_HOP member resolving to nh_key, or nullptr. */
    const ProtNhgMember* findMemberByNextHop(const NextHopKey &nh_key) const;
    ProtNhgMember* findMemberByNextHop(const NextHopKey &nh_key);

    bool syncMembers(const set<ProtNhgRole> &member_keys) override;
    vector<sai_attribute_t> createNhgmAttrs(const ProtNhgMember &member) const override;
};
