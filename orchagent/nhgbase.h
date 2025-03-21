#pragma once

#include "string"
#include "logger.h"
#include "saitypes.h"
#include "unordered_map"
#include "dbconnector.h"
#include "set"
#include "orchagent/orch.h"
#include "orchagent/crmorch.h"
#include "orchagent/routeorch.h"
#include "orchagent/nexthopgroupkey.h"
#include "orchagent/bulker.h"

using namespace std;

extern sai_object_id_t gSwitchId;

extern CrmOrch *gCrmOrch;
extern RouteOrch *gRouteOrch;

extern sai_next_hop_group_api_t* sai_next_hop_group_api;
extern size_t gMaxBulkSize;

/*
 * Base class for next hop groups, containing the common interface that every
 * next hop group should have based on what RouteOrch needs when working with
 * next hop groups.
 */
class NhgBase
{
public:
    NhgBase() : m_id(SAI_NULL_OBJECT_ID) { SWSS_LOG_ENTER(); }

    NhgBase(NhgBase &&nhg) : m_id(nhg.m_id)
        { SWSS_LOG_ENTER(); nhg.m_id = SAI_NULL_OBJECT_ID; }

    NhgBase& operator=(NhgBase &&nhg)
        { SWSS_LOG_ENTER(); swap(m_id, nhg.m_id); return *this; }

    /*
     * Prevent copying.
     */
    NhgBase(const NhgBase&) = delete;
    void operator=(const NhgBase&) = delete;

    virtual ~NhgBase();

    /*
     * Getters.
     */
    inline sai_object_id_t getId() const { SWSS_LOG_ENTER(); return m_id; }
    static inline unsigned getSyncedCount()
                                    { SWSS_LOG_ENTER(); return m_syncdCount; }

    /*
     * Check if the next hop group is synced or not.
     */
    inline bool isSynced() const { return m_id != SAI_NULL_OBJECT_ID; }

    /*
     * Check if the next hop group is temporary.
     */
    virtual bool isTemp() const = 0;

    /*
     * Get the NextHopGroupKey of this object.
     */
    virtual NextHopGroupKey getNhgKey() const = 0;

    /* Increment the number of existing groups. */
    static inline void incSyncedCount() { SWSS_LOG_ENTER(); ++m_syncdCount; }

    /* Decrement the number of existing groups. */
    static void decSyncedCount();

protected:
    /*
     * The SAI ID of this object.
     */
    sai_object_id_t m_id;

    /*
     * Number of synced NHGs.  Incremented when an object is synced and
     * decremented when an object is removed.  This will also account for the
     * groups created by RouteOrch.
     */
    static unsigned m_syncdCount;
};

/*
 * NhgMember class representing the common templated base class between
 * WeightedNhgMember and CbfNhgMember classes.
 */
template <typename Key>
class NhgMember
{
public:
    explicit NhgMember(const Key &key) :
        m_key(key), m_gm_id(SAI_NULL_OBJECT_ID) { SWSS_LOG_ENTER(); }

    NhgMember(NhgMember &&nhgm) : m_key(move(nhgm.m_key)), m_gm_id(nhgm.m_gm_id)
        { SWSS_LOG_ENTER(); nhgm.m_gm_id = SAI_NULL_OBJECT_ID; }

    NhgMember& operator=(NhgMember &&nhgm)
    {
        SWSS_LOG_ENTER();

        swap(m_key, nhgm.m_key);
        swap(m_gm_id, nhgm.m_gm_id);

        return *this;
    }

    /*
     * Prevent copying.
     */
    NhgMember(const NhgMember&) = delete;
    void operator=(const NhgMember&) = delete;

    /*
     * Sync the NHG member, setting its SAI ID.
     */
    virtual void sync(sai_object_id_t gm_id)
    {
        SWSS_LOG_ENTER();

        /* The SAI ID should be updated from invalid to something valid. */
        assert((m_gm_id == SAI_NULL_OBJECT_ID) && (gm_id != SAI_NULL_OBJECT_ID));

        m_gm_id = gm_id;
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
    }

    /*
     * Remove the group member, resetting its SAI ID.
     */
    virtual void remove()
    {
        SWSS_LOG_ENTER();

        /*
        * If the membeer is not synced, exit.
        */
        if (!isSynced())
        {
            return;
        }

        m_gm_id = SAI_NULL_OBJECT_ID;
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
    }

    /*
     * Getters.
     */
    inline Key getKey() const { return m_key; }
    inline sai_object_id_t getId() const { return m_gm_id; }

    /*
     * Check whether the group is synced.
     */
    inline bool isSynced() const { return m_gm_id != SAI_NULL_OBJECT_ID; }

    /*
     * Get a string form of the member.
     */
    virtual string to_string() const = 0;

protected:
    /*
     * The index / key of this NHG member.
     */
    Key m_key;

    /*
     * The SAI ID of this NHG member.
     */
    sai_object_id_t m_gm_id;
};

/*
 * NhgCommon class representing the common templated base class between
 * Nhg and CbfNhg classes.
 */
template <typename Key, typename MbrKey, typename Mbr>
class NhgCommon : public NhgBase
{
public:
    /*
     * Constructors.
     */
    explicit NhgCommon(const Key &key) : m_key(key) { SWSS_LOG_ENTER(); }

    NhgCommon(NhgCommon &&nhg) : NhgBase(move(nhg)),
                                m_key(move(nhg.m_key)),
                                m_members(move(nhg.m_members))
    { SWSS_LOG_ENTER(); }

    NhgCommon& operator=(NhgCommon &&nhg)
    {
        SWSS_LOG_ENTER();

        swap(m_key, nhg.m_key);
        swap(m_members, nhg.m_members);

        NhgBase::operator=(move(nhg));

        return *this;
    }

    /*
     * Check if the group contains the given member.
     */
    inline bool hasMember(const MbrKey &key) const
        { SWSS_LOG_ENTER(); return m_members.find(key) != m_members.end(); }

    /*
     * Getters.
     */
    inline Key getKey() const { SWSS_LOG_ENTER(); return m_key; }
    inline size_t getSize() const
                                { SWSS_LOG_ENTER(); return m_members.size(); }

    /*
     * Sync the group, generating a SAI ID.
     */
    virtual bool sync() = 0;

    /*
     * Remove the group, releasing the SAI ID.
     */
    virtual bool remove()
    {
        SWSS_LOG_ENTER();

        /*
         * If the group is already removed, there is nothing to be done.
         */
        if (!isSynced())
        {
            return true;
        }

        /*
         * Remove the group members.
         */
        set<MbrKey> members;

        for (const auto &member : m_members)
        {
            members.insert(member.first);
        }

        if (!removeMembers(members))
        {
            SWSS_LOG_ERROR("Failed to remove next hop group %s members",
                            to_string().c_str());
            return false;
        }

        /*
         * Remove the NHG over SAI.
         */
        auto status = sai_next_hop_group_api->remove_next_hop_group(m_id);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove next hop group %s, rv: %d",
                            to_string().c_str(), status);
            return false;
        }

        /*
         * Decrease the number of programmed NHGs.
         */
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
        decSyncedCount();

        /*
         * Reset the group ID.
         */
        m_id = SAI_NULL_OBJECT_ID;

        return true;
    }

    /*
     * Get a string representation of this next hop group.
     */
    virtual string to_string() const = 0;

protected:
    /*
     * The key indexing this object.
     */
    Key m_key;

    /*
     * The members of this group.
     */
    map<MbrKey, Mbr> m_members;

    /*
     * Sync the given members in the group.
     */
    virtual bool syncMembers(const set<MbrKey> &member_keys) = 0;

    /*
     * Remove the given members from the group.
     */
    virtual bool removeMembers(const set<MbrKey> &member_keys)
    {
        SWSS_LOG_ENTER();

        /*
         * Remove all the given members from the group.
         */
        ObjectBulker<sai_next_hop_group_api_t> bulker(sai_next_hop_group_api,
                                                      gSwitchId,
                                                      gMaxBulkSize);
        map<MbrKey, sai_status_t> statuses;

        for (const auto &key : member_keys)
        {
            const auto &nhgm = m_members.at(key);

            if (nhgm.isSynced())
            {
                bulker.remove_entry(&statuses[key], nhgm.getId());
            }
        }

        /*
         * Flush the bulker to remove the members.
         */
        bulker.flush();

        /*
         * Iterate over the returned statuses and check if the removal was
         * successful.  If it was, remove the member, otherwise log an error
         * message.
         */
        bool success = true;

        for (const auto &status : statuses)
        {
            auto &member = m_members.at(status.first);

            if (status.second == SAI_STATUS_SUCCESS)
            {
                member.remove();
            }
            else
            {
                SWSS_LOG_ERROR("Failed to remove next hop group member %s, rv: %d",
                                member.to_string().c_str(),
                                status.second);
                success = false;
            }
        }

        return success;
    }

    /*
     * Get the SAI attributes for creating a next hop group member over SAI.
     */
    virtual vector<sai_attribute_t> createNhgmAttrs(const Mbr &member)
                                                                    const = 0;
};

/*
 * Structure describing a next hop group which NhgOrch owns.  Beside having a
 * unique pointer to that next hop group, we also want to keep a ref count so
 * NhgOrch knows how many other objects reference the next hop group in order
 * not to remove them while still being referenced.
 */
template <typename NhgClass>
struct NhgEntry
{
    /* Pointer to the next hop group.  NhgOrch is the sole owner of it. */
    std::unique_ptr<NhgClass> nhg;

    /* Number of external objects referencing this next hop group. */
    unsigned ref_count;

    NhgEntry() = default;
    explicit NhgEntry(std::unique_ptr<NhgClass>&& _nhg,
                      unsigned int _ref_count = 0) :
        nhg(std::move(_nhg)), ref_count(_ref_count) {}
};

/*
 * Class providing the common functionality shared by all NhgOrch classes.
 */
template <typename NhgClass>
class NhgOrchCommon : public Orch
{
public:
    /*
     * Constructor.
     */
    NhgOrchCommon(DBConnector *db, string tableName) : Orch(db, tableName) {}

    /*
     * Check if the given next hop group index exists.
     */
    inline bool hasNhg(const string &index) const
    {
        SWSS_LOG_ENTER();
        return m_syncdNextHopGroups.find(index) != m_syncdNextHopGroups.end();
    }

    /*
     * Get the next hop group with the given index.  If the index does not
     * exist in the map, a out_of_range eexception will be thrown.
     */
    inline const NhgClass& getNhg(const string &index) const
                                { return *m_syncdNextHopGroups.at(index).nhg; }

    /* Increase the ref count for a NHG given by it's index. */
    void incNhgRefCount(const string& index)
    {
        SWSS_LOG_ENTER();

        auto& nhg_entry = m_syncdNextHopGroups.at(index);
        ++nhg_entry.ref_count;
    }

    /* Dencrease the ref count for a NHG given by it's index. */
    void decNhgRefCount(const string& index)
    {
        SWSS_LOG_ENTER();

        auto& nhg_entry = m_syncdNextHopGroups.at(index);

        /* Sanity check so we don't overflow. */
        assert(nhg_entry.ref_count > 0);
        --nhg_entry.ref_count;
    }

    /* Getters / Setters. */
    static inline unsigned getSyncedNhgCount() { return NhgBase::getSyncedCount(); }

    /* Increase / Decrease the number of synced next hop groups. */
    inline void incSyncedNhgCount()
    {
        assert(gRouteOrch->getNhgCount() + NhgBase::getSyncedCount() < gRouteOrch->getMaxNhgCount());
        NhgBase::incSyncedCount();
    }
    inline void decSyncedNhgCount() { NhgBase::decSyncedCount(); }

protected:
    /*
     * Map of synced next hop groups.
     */
    unordered_map<string, NhgEntry<NhgClass>> m_syncdNextHopGroups;
};
