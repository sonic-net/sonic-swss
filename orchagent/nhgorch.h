#pragma once

#include "orch.h"
#include "nexthopgroupkey.h"

/* Default maximum number of next hop groups */
#define DEFAULT_NUMBER_OF_ECMP_GROUPS   128
#define DEFAULT_MAX_ECMP_GROUP_SIZE     32

class NextHopGroupMember
{
public:
    /* Constructors / Assignment operators. */
    NextHopGroupMember(const NextHopKey& nh_key) :
        m_nh_key(nh_key),
        m_gm_id(SAI_NULL_OBJECT_ID) {}

    NextHopGroupMember(NextHopGroupMember&& nhgm) :
        m_nh_key(std::move(nhgm.m_nh_key)),
        m_gm_id(nhgm.m_gm_id)
    { nhgm.m_gm_id = SAI_NULL_OBJECT_ID; }

    NextHopGroupMember& operator=(NextHopGroupMember&& nhgm);

    /*
     * Prevent object copying so we don't end up having multiple objects
     * referencing the same SAI objects.
     */
    NextHopGroupMember(const NextHopGroupMember&) = delete;
    void operator=(const NextHopGroupMember&) = delete;

    /* Destructor. */
    virtual ~NextHopGroupMember();

    /* Update member's weight and update the SAI attribute as well. */
    bool updateWeight(uint32_t weight);

    /* Sync / Remove. */
    void sync(sai_object_id_t gm_id);
    void remove();

    /* Getters / Setters. */
    inline const NextHopKey& getNhKey() const { return m_nh_key; }
    inline uint32_t getWeight() const { return m_nh_key.weight; }
    sai_object_id_t getNhId() const;
    inline sai_object_id_t getGmId() const { return m_gm_id; }
    inline bool isSynced() const { return m_gm_id != SAI_NULL_OBJECT_ID; }

    /* Check if the next hop is labeled. */
    inline bool isLabeled() const { return !m_nh_key.label_stack.empty(); }

    /* Convert member's details to string. */
    std::string to_string() const
    {
        return m_nh_key.to_string() + ", SAI ID: " + std::to_string(m_gm_id);
    }

private:
    /* The key of the next hop of this member. */
    NextHopKey m_nh_key;

    /* The group member SAI ID for this member. */
    sai_object_id_t m_gm_id;
};

/* Map indexed by NextHopKey, containing the SAI ID of the group member. */
typedef std::map<NextHopKey, NextHopGroupMember> NhgMembers;

/*
 * NextHopGroup class representing a next hop group object.
 */
class NextHopGroup
{
public:
    /* Constructors. */
    explicit NextHopGroup(const NextHopGroupKey& key, bool is_temp);
    NextHopGroup(NextHopGroup&& nhg);
    NextHopGroup& operator=(NextHopGroup&& nhg);

    /* Destructor. */
    virtual ~NextHopGroup() { remove(); }

    /* Sync the group, creating the group's and members SAI IDs. */
    bool sync();

    /* Remove the group, reseting the group's and members SAI IDs.  */
    bool remove();

    /*
     * Update the group based on a new next hop group key.  This will also
     * perform any sync / remove necessary.
     */
    bool update(const NextHopGroupKey& nhg_key);

    /* Check if the group contains the given next hop. */
    inline bool hasNextHop(const NextHopKey& nh_key) const
    {
        return m_members.find(nh_key) != m_members.end();
    }

    /* Validate a next hop in the group, syncing it. */
    bool validateNextHop(const NextHopKey& nh_key);

    /* Invalidate a next hop in the group, removing it. */
    bool invalidateNextHop(const NextHopKey& nh_key);

    /* Increment the number of existing groups. */
    static inline void incCount() { ++m_count; }

    /* Decrement the number of existing groups. */
    static inline void decCount() { assert(m_count > 0); --m_count; }

    /* Getters / Setters. */
    inline const NextHopGroupKey& getKey() const { return m_key; }
    inline sai_object_id_t getId() const { return m_id; }
    static inline unsigned int getCount() { return m_count; }
    inline bool isTemp() const { return m_is_temp; }
    inline bool isSynced() const { return m_id != SAI_NULL_OBJECT_ID; }
    inline size_t getSize() const { return m_members.size(); }

    /* Convert NHG's details to a string. */
    std::string to_string() const
    {
        return m_key.to_string() + ", SAI ID: " + std::to_string(m_id);
    }

private:

    /* The next hop group key of this group. */
    NextHopGroupKey m_key;

    /* The SAI ID of the group. */
    sai_object_id_t m_id;

    /* Members of this next hop group. */
    NhgMembers m_members;

    /* Whether the group is temporary or not. */
    bool m_is_temp;

    /*
     * Number of existing groups.  Incremented when an object is created and
     * decremented when an object is destroyed.  This will also account for the
     * groups created by RouteOrch.
     */
    static unsigned int m_count;

    /* Add group's members over the SAI API for the given keys. */
    bool syncMembers(const std::set<NextHopKey>& nh_keys);

    /* Remove group's members the SAI API from the given keys. */
    bool removeMembers(const std::set<NextHopKey>& nh_keys);

    /* Create the attributes vector for a next hop group member. */
    vector<sai_attribute_t> createNhgmAttrs(const NextHopGroupMember& nhgm) const;
};

/*
 * Structure describing a next hop group which NhgOrch owns.  Beside having a
 * unique pointer to that next hop group, we also want to keep a ref count so
 * NhgOrch knows how many other objects reference the next hop group in order
 * not to remove them while still being referenced.
 */
struct NhgEntry
{
    /* Pointer to the next hop group.  NhgOrch is the sole owner of it. */
    std::unique_ptr<NextHopGroup> nhg;

    /* Number of external objects referencing this next hop group. */
    unsigned int ref_count;

    NhgEntry() = default;
    explicit NhgEntry(std::unique_ptr<NextHopGroup>&& _nhg,
                      unsigned int _ref_count = 0) :
        nhg(std::move(_nhg)), ref_count(_ref_count) {}
};

/*
 * Map indexed by next hop group's CP ID, containing the next hop group for
 * that ID and the number of objects referencing it.
 */
typedef std::unordered_map<std::string, NhgEntry> NhgTable;

/*
 * Next Hop Group Orchestrator class that handles NEXTHOP_GROUP_TABLE
 * updates.
 */
class NhgOrch : public Orch
{
public:
    /*
     * Constructor.
     */
    NhgOrch(DBConnector *db, string tableName);

    /* Check if the next hop group given by it's index exists. */
    inline bool hasNhg(const std::string& index) const
    {
        return m_syncdNextHopGroups.find(index) != m_syncdNextHopGroups.end();
    }

    /*
     * Get the next hop group given by it's index.  If the index does not exist
     * in map, a std::out_of_range exception will be thrown.
     */
    inline const NextHopGroup& getNhg(const std::string& index) const
                                { return *m_syncdNextHopGroups.at(index).nhg; }

    /* Add a temporary next hop group when resources are exhausted. */
    NextHopGroup createTempNhg(const NextHopGroupKey& nhg_key);

    /* Getters / Setters. */
    inline unsigned int getMaxNhgCount() const { return m_maxNhgCount; }
    static inline unsigned int getNhgCount() { return NextHopGroup::getCount(); }

    /* Validate / Invalidate a next hop. */
    bool validateNextHop(const NextHopKey& nh_key);
    bool invalidateNextHop(const NextHopKey& nh_key);

    /* Increase / Decrease the number of next hop groups. */
    inline void incNhgCount()
    {
        assert(NextHopGroup::getCount() < m_maxNhgCount);
        NextHopGroup::incCount();
    }
    inline void decNhgCount() { NextHopGroup::decCount(); }

    /* Increase / Decrease ref count for a NHG given by it's index. */
    void incNhgRefCount(const std::string& index);
    void decNhgRefCount(const std::string& index);

    /* Handling SAI status*/
    task_process_status handleSaiCreateStatus(sai_api_t api, sai_status_t status, void *context = nullptr)
        { return Orch::handleSaiCreateStatus(api, status, context); }
    task_process_status handleSaiRemoveStatus(sai_api_t api, sai_status_t status, void *context = nullptr)
        { return Orch::handleSaiRemoveStatus(api, status, context); }
    bool parseHandleSaiStatusFailure(task_process_status status)
        { return Orch::parseHandleSaiStatusFailure(status); }

private:

    /*
     * Switch's maximum number of next hop groups capacity.
     */
    unsigned int m_maxNhgCount;

    /*
     * The next hop group table.
     */
    NhgTable m_syncdNextHopGroups;

    void doTask(Consumer& consumer);
};
