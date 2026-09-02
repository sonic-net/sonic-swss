#include "protnhg.h"
#include "neighorch.h"
#include "nhgorch.h"
#include "logger.h"
#include "sai_serialize.h"

extern NeighOrch *gNeighOrch;
extern NhgOrch *gNhgOrch;

static inline const char* roleToString(ProtNhgRole role)
{
    return (role == ProtNhgRole::PRIMARY) ? "primary" : "standby";
}

ProtNhgMember::ProtNhgMember(ProtNhgRole role,
                             ProtNhgMemberType type,
                             const NextHopKey &nh_key,
                             const string &nhg_index) :
    NhgMember(role),
    m_type(type),
    m_nh_key(nh_key),
    m_nhg_index(nhg_index),
    m_programmed_nh_id(SAI_NULL_OBJECT_ID),
    m_monitored_oid(SAI_NULL_OBJECT_ID)
{
    SWSS_LOG_ENTER();
}

ProtNhgMember ProtNhgMember::nextHop(ProtNhgRole role, const NextHopKey &nh_key)
{
    SWSS_LOG_ENTER();

    return ProtNhgMember(role, ProtNhgMemberType::NEXT_HOP, nh_key, "");
}

ProtNhgMember ProtNhgMember::sharedNhg(ProtNhgRole role, const string &nhg_index)
{
    SWSS_LOG_ENTER();

    return ProtNhgMember(role, ProtNhgMemberType::SHARED_NHG, NextHopKey(),
                         nhg_index);
}

ProtNhgMember::ProtNhgMember(ProtNhgMember &&nhgm) :
    NhgMember(move(nhgm)),
    m_type(nhgm.m_type),
    m_nh_key(move(nhgm.m_nh_key)),
    m_nhg_index(move(nhgm.m_nhg_index)),
    m_programmed_nh_id(nhgm.m_programmed_nh_id),
    m_monitored_oid(nhgm.m_monitored_oid)
{
    SWSS_LOG_ENTER();
    nhgm.m_programmed_nh_id = SAI_NULL_OBJECT_ID;
    nhgm.m_monitored_oid = SAI_NULL_OBJECT_ID;
}

ProtNhgMember::~ProtNhgMember()
{
    SWSS_LOG_ENTER();
}

void ProtNhgMember::sync(sai_object_id_t gm_id)
{
    SWSS_LOG_ENTER();
    NhgMember::sync(gm_id);

    /* The SAI member was just created pointing here, so this is what is
     * programmed until updateNhId() says otherwise. */
    m_programmed_nh_id = getNhId();

    switch (m_type)
    {
        case ProtNhgMemberType::NEXT_HOP:
            gNeighOrch->increaseNextHopRefCount(m_nh_key);
            break;

        case ProtNhgMemberType::SHARED_NHG:
            /* incNhgRefCount() throws if the index is gone, and it can be:
             * getNhId() resolved it earlier in this same call chain, but
             * nothing pins it between then and here. */
            if (gNhgOrch->hasNhg(m_nhg_index))
            {
                gNhgOrch->incNhgRefCount(m_nhg_index);
            }
            else
            {
                SWSS_LOG_WARN("Shared NHG %s vanished before member %s could "
                              "reference it",
                              m_nhg_index.c_str(), to_string().c_str());
            }
            break;
    }
}

void ProtNhgMember::remove()
{
    SWSS_LOG_ENTER();

    if (!isSynced())
    {
        return;
    }

    switch (m_type)
    {
        case ProtNhgMemberType::NEXT_HOP:
            gNeighOrch->decreaseNextHopRefCount(m_nh_key);
            break;

        case ProtNhgMemberType::SHARED_NHG:
            if (gNhgOrch->hasNhg(m_nhg_index))
            {
                gNhgOrch->decNhgRefCount(m_nhg_index);
            }
            break;
    }

    NhgMember::remove();
}

sai_object_id_t ProtNhgMember::getNhId() const
{
    SWSS_LOG_ENTER();

    switch (m_type)
    {
        case ProtNhgMemberType::NEXT_HOP:
            if (gNeighOrch->hasNextHop(m_nh_key))
            {
                return gNeighOrch->getNextHopId(m_nh_key);
            }
            return SAI_NULL_OBJECT_ID;

        case ProtNhgMemberType::SHARED_NHG:
            if (gNhgOrch->hasNhg(m_nhg_index))
            {
                return gNhgOrch->getNhg(m_nhg_index).getId();
            }
            SWSS_LOG_WARN("Shared NHG %s not found for %s member",
                          m_nhg_index.c_str(), roleToString(m_key));
            return SAI_NULL_OBJECT_ID;
    }

    return SAI_NULL_OBJECT_ID;
}

bool ProtNhgMember::updateMonitoredObject(sai_object_id_t oid)
{
    SWSS_LOG_ENTER();

    if (!isSynced())
    {
        m_monitored_oid = oid;
        return true;
    }

    sai_attribute_t attr;
    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_MONITORED_OBJECT;
    attr.value.oid = oid;

    sai_status_t status =
        sai_next_hop_group_api->set_next_hop_group_member_attribute(m_gm_id, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to update monitored object for member %s, rv: %d",
                       to_string().c_str(), status);
        return false;
    }

    m_monitored_oid = oid;
    return true;
}

bool ProtNhgMember::updateNhId()
{
    SWSS_LOG_ENTER();

    /* Nothing is programmed yet; sync() will pick up the current ID. */
    if (!isSynced())
    {
        return true;
    }

    sai_object_id_t nh_id = getNhId();

    if (nh_id == m_programmed_nh_id)
    {
        return true;
    }

    /*
     * The target went away rather than moving. Leave the member pointing at
     * the old ID: there is nothing valid to point it at, and sync of a
     * replacement group will come back through here.
     */
    if (nh_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("Member %s has no next hop to re-point at",
                      to_string().c_str());
        return false;
    }

    sai_attribute_t attr;
    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
    attr.value.oid = nh_id;

    sai_status_t status =
        sai_next_hop_group_api->set_next_hop_group_member_attribute(m_gm_id, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to re-point member %s at next hop %s, rv: %d",
                       to_string().c_str(),
                       sai_serialize_object_id(nh_id).c_str(), status);
        return false;
    }

    SWSS_LOG_NOTICE("Re-pointed member %s from next hop %s to %s",
                    to_string().c_str(),
                    sai_serialize_object_id(m_programmed_nh_id).c_str(),
                    sai_serialize_object_id(nh_id).c_str());

    m_programmed_nh_id = nh_id;
    return true;
}

bool ProtNhgMember::getObservedRole(
    sai_next_hop_group_member_observed_role_t &observed_role) const
{
    SWSS_LOG_ENTER();

    if (!isSynced())
    {
        SWSS_LOG_WARN("Cannot query observed role on unsynced member %s",
                       to_string().c_str());
        return false;
    }

    sai_attribute_t attr;
    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_OBSERVED_ROLE;

    sai_status_t status =
        sai_next_hop_group_api->get_next_hop_group_member_attribute(m_gm_id, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get observed role for member %s, rv: %d",
                       to_string().c_str(), status);
        return false;
    }

    observed_role =
        static_cast<sai_next_hop_group_member_observed_role_t>(attr.value.s32);

    return true;
}

string ProtNhgMember::to_string() const
{
    string target;

    switch (m_type)
    {
        case ProtNhgMemberType::NEXT_HOP:
            target = m_nh_key.to_string();
            break;

        case ProtNhgMemberType::SHARED_NHG:
            target = "shared NHG " + m_nhg_index;
            break;
    }

    return target + " [" + roleToString(m_key) + "], SAI ID: " +
           std::to_string(m_gm_id);
}

/* ----------------------------------------------------------------------- */

ProtNhg::ProtNhg(const string &key,
                 ProtNhgMember &&primary,
                 ProtNhgMember &&standby) :
    NhgCommon(key)
{
    SWSS_LOG_ENTER();

    ProtNhgRole primary_role = primary.getRole();
    ProtNhgRole standby_role = standby.getRole();

    /* Members are keyed by role, so two members of one role would silently
     * collapse into one. Leave the group empty instead; sync() rejects it. */
    if (primary_role == standby_role)
    {
        SWSS_LOG_ERROR("Protection NHG %s was given two %s members, it must be "
                       "a primary/standby pair",
                       key.c_str(), roleToString(primary_role));
        return;
    }

    m_members.emplace(primary_role, move(primary));
    m_members.emplace(standby_role, move(standby));
}

ProtNhg::ProtNhg(const string &key,
                 const NextHopKey &primary_nh,
                 const NextHopKey &standby_nh) :
    ProtNhg(key,
            ProtNhgMember::nextHop(ProtNhgRole::PRIMARY, primary_nh),
            ProtNhgMember::nextHop(ProtNhgRole::STANDBY, standby_nh))
{
    SWSS_LOG_ENTER();
}

ProtNhg::ProtNhg(ProtNhg &&nhg) :
    NhgCommon(move(nhg))
{
    SWSS_LOG_ENTER();
}

bool ProtNhg::sync()
{
    SWSS_LOG_ENTER();

    if (isSynced())
    {
        return true;
    }

    /* Members are keyed by role, so a primary/standby collision cannot produce
     * two members of the same role -- it produces a missing one instead. */
    auto primary_it = m_members.find(ProtNhgRole::PRIMARY);
    auto standby_it = m_members.find(ProtNhgRole::STANDBY);

    if (primary_it == m_members.end() || standby_it == m_members.end())
    {
        SWSS_LOG_ERROR("Protection NHG %s must have exactly one primary and one "
                       "standby member, has %zu member(s)",
                       m_key.c_str(), m_members.size());
        return false;
    }

    /*
     * Both legs resolving to the same SAI object would be a protection group
     * that cannot protect anything.  Comparing the resolved IDs catches this
     * whatever the members are made of.  Two unresolved members both read as
     * null, which is not a collision, so require a real ID first.
     */
    sai_object_id_t primary_nh_id = primary_it->second.getNhId();

    if (primary_nh_id != SAI_NULL_OBJECT_ID &&
        primary_nh_id == standby_it->second.getNhId())
    {
        SWSS_LOG_ERROR("Protection NHG %s has the same next hop for its primary "
                       "and standby members", m_key.c_str());
        return false;
    }

    sai_attribute_t nhg_attr;
    vector<sai_attribute_t> nhg_attrs;

    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
    nhg_attr.value.s32 = SAI_NEXT_HOP_GROUP_TYPE_PROTECTION;
    nhg_attrs.push_back(nhg_attr);

    sai_status_t status = sai_next_hop_group_api->create_next_hop_group(
        &m_id,
        gSwitchId,
        static_cast<uint32_t>(nhg_attrs.size()),
        nhg_attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create protection NHG %s, rv: %d",
                       m_key.c_str(), status);
        return false;
    }

    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
    incSyncedCount();

    set<ProtNhgRole> member_keys;
    for (const auto &mbr : m_members)
    {
        member_keys.insert(mbr.first);
    }

    if (!syncMembers(member_keys))
    {
        SWSS_LOG_WARN("Failed to sync members of protection NHG %s", m_key.c_str());
        return false;
    }

    return true;
}

bool ProtNhg::remove()
{
    SWSS_LOG_ENTER();

    if (!isSynced())
    {
        return true;
    }

    return NhgCommon::remove();
}

/* Sync whichever members nh_key becoming resolved has unblocked. */
bool ProtNhg::validateNextHop(const NextHopKey &nh_key)
{
    SWSS_LOG_ENTER();

    if (!isSynced())
    {
        return true;
    }

    set<ProtNhgRole> to_sync;

    for (const auto &entry : m_members)
    {
        const ProtNhgMember &mbr = entry.second;

        if (mbr.isSynced())
        {
            continue;
        }

        switch (mbr.getType())
        {
            case ProtNhgMemberType::NEXT_HOP:
                if (mbr.getNextHopKey() == nh_key)
                {
                    to_sync.insert(entry.first);
                }
                break;

            case ProtNhgMemberType::SHARED_NHG:
                /*
                 * NhgOrch resolves the shared group itself, through its own
                 * map. All that can be left for us is a member we could not
                 * sync earlier because the group was not ready yet.
                 */
                if (mbr.getNhId() != SAI_NULL_OBJECT_ID)
                {
                    to_sync.insert(entry.first);
                }
                break;
        }
    }

    if (to_sync.empty())
    {
        return true;
    }

    return syncMembers(to_sync);
}

bool ProtNhg::updateSharedMember(const string &nhg_index)
{
    SWSS_LOG_ENTER();

    bool success = true;

    for (auto &entry : m_members)
    {
        ProtNhgMember &mbr = entry.second;

        if (mbr.getType() != ProtNhgMemberType::SHARED_NHG ||
            mbr.getNhgIndex() != nhg_index)
        {
            continue;
        }

        if (!mbr.updateNhId())
        {
            SWSS_LOG_ERROR("Failed to re-point %s member of protection NHG %s "
                           "at shared NHG %s",
                           roleToString(entry.first), m_key.c_str(),
                           nhg_index.c_str());
            success = false;
        }
    }

    return success;
}

bool ProtNhg::isHwAutonomous() const
{
    SWSS_LOG_ENTER();

    const ProtNhgMember *primary = getPrimaryMember();

    return primary != nullptr &&
           primary->getMonitoredObject() != SAI_NULL_OBJECT_ID;
}

bool ProtNhg::setAdminRole(sai_next_hop_group_admin_role_t admin_role)
{
    SWSS_LOG_ENTER();

    if (!isHwAutonomous())
    {
        SWSS_LOG_ERROR("Protection NHG %s is SW-driven (no monitored object "
                       "attached); use setSwitchover() instead of the admin "
                       "role override",
                       m_key.c_str());
        return false;
    }

    if (!isSynced())
    {
        SWSS_LOG_ERROR("Cannot set admin role on unsynced protection NHG %s",
                       m_key.c_str());
        return false;
    }

    sai_attribute_t attr;
    attr.id = SAI_NEXT_HOP_GROUP_ATTR_ADMIN_ROLE;
    attr.value.s32 = admin_role;

    sai_status_t status =
        sai_next_hop_group_api->set_next_hop_group_attribute(m_id, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set admin role %d on protection NHG %s, rv: %d",
                       admin_role, m_key.c_str(), status);
        return false;
    }

    SWSS_LOG_NOTICE("Set admin role %d on protection NHG %s",
                    admin_role, m_key.c_str());

    return true;
}

bool ProtNhg::setSwitchover(bool enable)
{
    SWSS_LOG_ENTER();

    if (isHwAutonomous())
    {
        SWSS_LOG_ERROR("Protection NHG %s is HW-autonomous (monitored object "
                       "attached); the hardware owns the switchover, use "
                       "setAdminRole() to override it",
                       m_key.c_str());
        return false;
    }

    if (!isSynced())
    {
        SWSS_LOG_ERROR("Cannot set switchover on unsynced protection NHG %s",
                       m_key.c_str());
        return false;
    }

    sai_attribute_t attr;
    attr.id = SAI_NEXT_HOP_GROUP_ATTR_SET_SWITCHOVER;
    attr.value.booldata = enable;

    sai_status_t status =
        sai_next_hop_group_api->set_next_hop_group_attribute(m_id, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set switchover %s on protection NHG %s, rv: %d",
                       enable ? "true" : "false", m_key.c_str(), status);
        return false;
    }

    SWSS_LOG_NOTICE("Set switchover %s on protection NHG %s",
                    enable ? "true" : "false", m_key.c_str());

    return true;
}

bool ProtNhg::updateMemberMonitoredObject(ProtNhgRole role,
                                           sai_object_id_t monitored_oid)
{
    SWSS_LOG_ENTER();

    auto it = m_members.find(role);
    if (it == m_members.end())
    {
        SWSS_LOG_ERROR("Protection NHG %s has no %s member",
                       m_key.c_str(), roleToString(role));
        return false;
    }

    return it->second.updateMonitoredObject(monitored_oid);
}

bool ProtNhg::updateMemberMonitoredObject(const NextHopKey &nh_key,
                                           sai_object_id_t monitored_oid)
{
    SWSS_LOG_ENTER();

    ProtNhgMember *mbr = findMemberByNextHop(nh_key);

    if (mbr == nullptr)
    {
        SWSS_LOG_ERROR("Next hop %s does not address a member of protection "
                       "NHG %s", nh_key.to_string().c_str(), m_key.c_str());
        return false;
    }

    return mbr->updateMonitoredObject(monitored_oid);
}

const ProtNhgMember* ProtNhg::getMember(ProtNhgRole role) const
{
    SWSS_LOG_ENTER();

    auto it = m_members.find(role);

    return (it == m_members.end()) ? nullptr : &it->second;
}

const ProtNhgMember* ProtNhg::getPrimaryMember() const
{
    SWSS_LOG_ENTER();
    return getMember(ProtNhgRole::PRIMARY);
}

const ProtNhgMember* ProtNhg::getStandbyMember() const
{
    SWSS_LOG_ENTER();
    return getMember(ProtNhgRole::STANDBY);
}

bool ProtNhg::getMemberRole(const NextHopKey &nh_key, ProtNhgRole &role) const
{
    SWSS_LOG_ENTER();

    const ProtNhgMember *mbr = findMemberByNextHop(nh_key);

    if (mbr == nullptr)
    {
        return false;
    }

    role = mbr->getRole();
    return true;
}

const ProtNhgMember* ProtNhg::findMemberByNextHop(const NextHopKey &nh_key) const
{
    SWSS_LOG_ENTER();

    for (const auto &mbr : m_members)
    {
        if (mbr.second.getType() == ProtNhgMemberType::NEXT_HOP &&
            mbr.second.getNextHopKey() == nh_key)
        {
            return &mbr.second;
        }
    }

    return nullptr;
}

ProtNhgMember* ProtNhg::findMemberByNextHop(const NextHopKey &nh_key)
{
    SWSS_LOG_ENTER();

    return const_cast<ProtNhgMember*>(
        static_cast<const ProtNhg*>(this)->findMemberByNextHop(nh_key));
}

bool ProtNhg::getMemberObservedRole(
    ProtNhgRole role,
    sai_next_hop_group_member_observed_role_t &observed_role) const
{
    SWSS_LOG_ENTER();

    const ProtNhgMember *mbr = getMember(role);

    if (mbr == nullptr)
    {
        SWSS_LOG_ERROR("Protection NHG %s has no %s member",
                       m_key.c_str(), roleToString(role));
        return false;
    }

    return mbr->getObservedRole(observed_role);
}

bool ProtNhg::getMemberObservedRole(
    const NextHopKey &nh_key,
    sai_next_hop_group_member_observed_role_t &observed_role) const
{
    SWSS_LOG_ENTER();

    const ProtNhgMember *mbr = findMemberByNextHop(nh_key);

    if (mbr == nullptr)
    {
        SWSS_LOG_ERROR("Next hop %s does not address a member of protection "
                       "NHG %s", nh_key.to_string().c_str(), m_key.c_str());
        return false;
    }

    return mbr->getObservedRole(observed_role);
}

bool ProtNhg::getAllMemberObservedRoles(
    map<ProtNhgRole, sai_next_hop_group_member_observed_role_t> &observed_roles) const
{
    SWSS_LOG_ENTER();

    observed_roles.clear();

    bool success = true;
    for (const auto &mbr : m_members)
    {
        if (!mbr.second.isSynced())
        {
            continue;
        }

        sai_next_hop_group_member_observed_role_t role;
        if (mbr.second.getObservedRole(role))
        {
            observed_roles[mbr.first] = role;
        }
        else
        {
            SWSS_LOG_WARN("Failed to get observed role for member %s in NHG %s",
                          mbr.second.to_string().c_str(), m_key.c_str());
            success = false;
        }
    }

    return success;
}

bool ProtNhg::syncMembers(const set<ProtNhgRole> &member_keys)
{
    SWSS_LOG_ENTER();

    ObjectBulker<sai_next_hop_group_api_t> bulker(sai_next_hop_group_api,
                                                   gSwitchId,
                                                   gMaxBulkSize);
    map<ProtNhgRole, sai_object_id_t> syncing;

    /* Unresolved members are skipped but still fail this call, so the
     * caller knows to retry them later via validateNextHop(). */
    bool success = true;

    for (const auto &role : member_keys)
    {
        ProtNhgMember &nhgm = m_members.at(role);

        if (nhgm.isSynced())
        {
            continue;
        }

        if (nhgm.getNhId() == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("Member %s not resolved for protection NHG %s",
                          nhgm.to_string().c_str(), m_key.c_str());
            success = false;
            continue;
        }

        vector<sai_attribute_t> attrs = createNhgmAttrs(nhgm);
        bulker.create_entry(&syncing[role],
                            static_cast<uint32_t>(attrs.size()),
                            attrs.data());
    }

    bulker.flush();

    /*
     * Go through the synced members and, for the successful ones, call sync()
     * which records the SAI member ID and increments the CRM_NEXTHOP_GROUP_MEMBER
     * ref count (via NhgMember::sync()). The matching decrement happens in
     * NhgMember::remove() through ProtNhg::remove() -> NhgCommon::removeMembers().
     */
    for (const auto &entry : syncing)
    {
        if (entry.second == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("Failed to create %s member of protection NHG %s",
                           roleToString(entry.first), m_key.c_str());
            success = false;
        }
        else
        {
            m_members.at(entry.first).sync(entry.second);
        }
    }

    return success;
}

vector<sai_attribute_t> ProtNhg::createNhgmAttrs(const ProtNhgMember &member) const
{
    SWSS_LOG_ENTER();

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
    attr.value.oid = m_id;
    attrs.push_back(attr);

    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
    attr.value.oid = member.getNhId();
    attrs.push_back(attr);

    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_CONFIGURED_ROLE;
    attr.value.s32 = (member.getRole() == ProtNhgRole::PRIMARY)
        ? SAI_NEXT_HOP_GROUP_MEMBER_CONFIGURED_ROLE_PRIMARY
        : SAI_NEXT_HOP_GROUP_MEMBER_CONFIGURED_ROLE_STANDBY;
    attrs.push_back(attr);

    if (member.getMonitoredObject() != SAI_NULL_OBJECT_ID)
    {
        attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_MONITORED_OBJECT;
        attr.value.oid = member.getMonitoredObject();
        attrs.push_back(attr);
    }

    return attrs;
}
