#pragma once

#include "nhgbase.h"

using namespace std;

class WeightedNhgMember : public NhgMember<NextHopKey>
{
public:
    /* Constructors / Assignment operators. */
    WeightedNhgMember(const NextHopKey& nh_key) :
        NhgMember(nh_key) {}

    WeightedNhgMember(WeightedNhgMember&& nhgm) :
        NhgMember(move(nhgm)) {}

    /* Destructor. */
    ~WeightedNhgMember();

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
        return m_key.to_string() +
                ", SAI ID: " + std::to_string(m_id);
    }
};

/*
 * Nhg class representing a next hop group object.
 */
class Nhg : public NhgCommon<NextHopGroupKey, NextHopKey, WeightedNhgMember>
{
public:
    /* Constructors. */
    explicit Nhg(const NextHopGroupKey& key, bool is_temp);

    Nhg(Nhg&& nhg) :
        NhgCommon(move(nhg)), m_is_temp(nhg.m_is_temp)
    { SWSS_LOG_ENTER(); }

    Nhg& operator=(Nhg&& nhg);

    ~Nhg() { SWSS_LOG_ENTER(); remove(); }

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
    inline void setTemp(bool is_temp) { m_is_temp = is_temp; }

    NextHopGroupKey getNhgKey() const override { return m_key; }

    /* Convert NHG's details to a string. */
    string to_string() const override
    {
        return m_key.to_string() + ", SAI ID: " + std::to_string(m_id);
    }

private:
    /* Whether the group is temporary or not. */
    bool m_is_temp;

    /* Add group's members over the SAI API for the given keys. */
    bool syncMembers(const set<NextHopKey>& nh_keys) override;

    /* Create the attributes vector for a next hop group member. */
    vector<sai_attribute_t> createNhgmAttrs(
                                const WeightedNhgMember& nhgm) const override;
};

/*
 * Next Hop Group Orchestrator class that handles NEXT_HOP_GROUP_TABLE
 * updates.
 */
class NhgHandler : public NhgHandlerCommon<Nhg>
{
public:
    /* Add a temporary next hop group when resources are exhausted. */
    Nhg createTempNhg(const NextHopGroupKey& nhg_key);

    /* Validate / Invalidate a next hop. */
    bool validateNextHop(const NextHopKey& nh_key);
    bool invalidateNextHop(const NextHopKey& nh_key);

    void doTask(Consumer &consumer);
};
