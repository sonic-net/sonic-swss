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
 * ProtNhgMember represents a member of a hardware protection next hop group.
 * Each member has a configured role (primary or standby) and an optional
 * monitored object (e.g., ICMP echo session OID) for hardware-based failover.
 */
class ProtNhgMember : public NhgMember<NextHopKey>
{
public:
    ProtNhgMember(const NextHopKey &key, ProtNhgRole role,
                  sai_object_id_t nh_id_override = SAI_NULL_OBJECT_ID);

    ProtNhgMember(ProtNhgMember &&nhgm);

    ~ProtNhgMember();

    void sync(sai_object_id_t gm_id) override;
    void remove() override;

    inline ProtNhgRole getRole() const { return m_role; }
    sai_object_id_t getNhId() const;

    inline sai_object_id_t getMonitoredObject() const { return m_monitored_oid; }
    void setMonitoredObject(sai_object_id_t oid) { m_monitored_oid = oid; }

    bool updateMonitoredObject(sai_object_id_t oid);

    /* Query the hardware-observed role (active/inactive) from SAI. */
    bool getObservedRole(sai_next_hop_group_member_observed_role_t &observed_role) const;

    string to_string() const override;

private:
    ProtNhgRole m_role;
    sai_object_id_t m_monitored_oid;
    sai_object_id_t m_nh_id_override;
};

/*
 * ProtNhg represents a SAI_NEXT_HOP_GROUP_TYPE_HW_PROTECTION group.
 * It has one or more primary next hops and exactly one standby next hop.
 * Hardware toggles traffic between the primary set and the standby based on
 * the monitored object state. Administrative override is supported via
 * SAI_NEXT_HOP_GROUP_ATTR_ADMIN_ROLE.
 */
class ProtNhg : public NhgCommon<string, NextHopKey, ProtNhgMember>
{
public:
    ProtNhg(const string &key,
            const vector<NextHopKey> &primary_nhs,
            const NextHopKey &standby_nh,
            sai_object_id_t standby_nh_id = SAI_NULL_OBJECT_ID);

    ProtNhg(ProtNhg &&nhg);

    ~ProtNhg() { SWSS_LOG_ENTER(); remove(); }

    bool sync() override;
    bool remove() override;

    inline bool isTemp() const override { return false; }
    inline NextHopGroupKey getNhgKey() const override { return {}; }

    bool setAdminRole(sai_int32_t admin_role);

    bool updateMemberMonitoredObject(const NextHopKey &nh_key,
                                     sai_object_id_t monitored_oid);

    vector<const ProtNhgMember*> getPrimaryMembers() const;
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
