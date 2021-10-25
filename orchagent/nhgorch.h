#pragma once

#include "cbf/cbfnhgorch.h"
#include "vector"
#include "portsorch.h"
#include "routeorch.h"

using namespace std;

extern PortsOrch *gPortsOrch;
extern RouteOrch *gRouteOrch;
extern CbfNhgOrch *gCbfNhgOrch;

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
        NhgCommon(move(nhg)), m_is_temp(nhg.m_is_temp)
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

    NextHopGroupKey getNhgKey() const override { return m_key; }

    /* Convert NHG's details to a string. */
    std::string to_string() const override
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
                                const NextHopGroupMember& nhgm) const override;
};

/*
 * Next Hop Group Orchestrator class that handles NEXTHOP_GROUP_TABLE
 * updates.
 */
class NhgOrch : public NhgOrchCommon<NextHopGroup>, public Orch
{
public:
    /*
     * Constructor.
     */
    NhgOrch(DBConnector *db, string tableName);

    /* Check if the next hop group given by it's index exists. */
    inline bool hasNhg(const std::string& index) const
    {
        return NhgOrchCommon::hasNhg(index) || gCbfNhgOrch->hasNhg(index);
    }

    /*
     * Get the next hop group with the given index.
     */
    inline const NhgBase& getNhg(const std::string &index) const
    {
        try
        {
            return NhgOrchCommon::getNhg(index);
        }
        catch(const std::out_of_range &e)
        {
            return gCbfNhgOrch->getNhg(index);
        }
    }

    /* Add a temporary next hop group when resources are exhausted. */
    NextHopGroup createTempNhg(const NextHopGroupKey& nhg_key);

    /* Getters / Setters. */
    static inline unsigned getSyncedNhgCount() { return NhgBase::getSyncedCount(); }

    /* Increase / Decrease the number of synced next hop groups. */
    inline void incSyncedNhgCount()
    {
        assert(gRouteOrch->getNhgCount() + NhgBase::getSyncedCount() < gRouteOrch->getMaxNhgCount());
        NhgBase::incSyncedCount();
    }
    inline void decSyncedNhgCount() { NhgBase::decSyncedCount(); }

    /* Increase / Decrease ref count for a NHG given by it's index. */
    void incNhgRefCount(const std::string& index);
    void decNhgRefCount(const std::string& index);

    /* Validate / Invalidate a next hop. */
    bool validateNextHop(const NextHopKey& nh_key);
    bool invalidateNextHop(const NextHopKey& nh_key);

    /* Handling SAI status*/
    task_process_status handleSaiCreateStatus(sai_api_t api, sai_status_t status, void *context = nullptr)
        { return Orch::handleSaiCreateStatus(api, status, context); }
    task_process_status handleSaiRemoveStatus(sai_api_t api, sai_status_t status, void *context = nullptr)
        { return Orch::handleSaiRemoveStatus(api, status, context); }
    bool parseHandleSaiStatusFailure(task_process_status status)
        { return Orch::parseHandleSaiStatusFailure(status); }
private:

    void doTask(Consumer& consumer);
};
