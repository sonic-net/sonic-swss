#pragma once

#include "orchagent/cbf/cbfnhgorch.h"
#include "vector"
#include "orchagent/portsorch.h"
#include "orchagent/routeorch.h"

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

private:
    void doTask(Consumer& consumer) override;
};
