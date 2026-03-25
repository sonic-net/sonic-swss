#include "protnhg.h"
#include "neighorch.h"
#include "logger.h"
#include "sai_serialize.h"

extern NeighOrch *gNeighOrch;

ProtNhgMember::ProtNhgMember(const NextHopKey &key, ProtNhgRole role,
                             sai_object_id_t nh_id_override) :
    NhgMember(key),
    m_role(role),
    m_monitored_oid(SAI_NULL_OBJECT_ID),
    m_nh_id_override(nh_id_override)
{
    SWSS_LOG_ENTER();
}

ProtNhgMember::ProtNhgMember(ProtNhgMember &&nhgm) :
    NhgMember(move(nhgm)),
    m_role(nhgm.m_role),
    m_monitored_oid(nhgm.m_monitored_oid),
    m_nh_id_override(nhgm.m_nh_id_override)
{
    SWSS_LOG_ENTER();
    nhgm.m_monitored_oid = SAI_NULL_OBJECT_ID;
    nhgm.m_nh_id_override = SAI_NULL_OBJECT_ID;
}

ProtNhgMember::~ProtNhgMember()
{
    SWSS_LOG_ENTER();
}

void ProtNhgMember::sync(sai_object_id_t gm_id)
{
    SWSS_LOG_ENTER();
    NhgMember::sync(gm_id);

    if (m_nh_id_override == SAI_NULL_OBJECT_ID)
    {
        gNeighOrch->increaseNextHopRefCount(m_key);
    }
}

void ProtNhgMember::remove()
{
    SWSS_LOG_ENTER();

    if (!isSynced())
    {
        return;
    }

    if (m_nh_id_override == SAI_NULL_OBJECT_ID)
    {
        gNeighOrch->decreaseNextHopRefCount(m_key);
    }
    NhgMember::remove();
}

sai_object_id_t ProtNhgMember::getNhId() const
{
    SWSS_LOG_ENTER();

    if (m_nh_id_override != SAI_NULL_OBJECT_ID)
    {
        return m_nh_id_override;
    }

    if (gNeighOrch->hasNextHop(m_key))
    {
        return gNeighOrch->getNextHopId(m_key);
    }

    return SAI_NULL_OBJECT_ID;
}

bool ProtNhgMember::updateMonitoredObject(sai_object_id_t oid)
{
    SWSS_LOG_ENTER();

    m_monitored_oid = oid;

    if (!isSynced())
    {
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

    return true;
}

bool ProtNhgMember::getObservedRole(
    sai_next_hop_group_member_observed_role_t &observed_role) const
{
    SWSS_LOG_ENTER();

    if (!isSynced())
    {
        SWSS_LOG_WARN("Cannot query observed role on unsynced member %s",
                       m_key.to_string().c_str());
        return false;
    }

    sai_attribute_t attr;
    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_OBSERVED_ROLE;

    sai_status_t status =
        sai_next_hop_group_api->get_next_hop_group_member_attribute(m_gm_id, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to get observed role for member %s, rv: %d",
                       m_key.to_string().c_str(), status);
        return false;
    }

    observed_role =
        static_cast<sai_next_hop_group_member_observed_role_t>(attr.value.s32);

    return true;
}

string ProtNhgMember::to_string() const
{
    string role_str = (m_role == ProtNhgRole::PRIMARY) ? "primary" : "standby";
    return m_key.to_string() + " [" + role_str + "], SAI ID: " + std::to_string(m_gm_id);
}

/* ----------------------------------------------------------------------- */

ProtNhg::ProtNhg(const string &key,
                  const vector<NextHopKey> &primary_nhs,
                  const NextHopKey &standby_nh,
                  sai_object_id_t standby_nh_id) :
    NhgCommon(key)
{
    SWSS_LOG_ENTER();

    for (const auto &nh : primary_nhs)
    {
        m_members.emplace(nh, ProtNhgMember(nh, ProtNhgRole::PRIMARY));
    }
    m_members.emplace(standby_nh,
                      ProtNhgMember(standby_nh, ProtNhgRole::STANDBY, standby_nh_id));
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

    if (m_members.size() < 2)
    {
        SWSS_LOG_ERROR("Protection NHG %s must have at least 2 members, has %zu",
                       m_key.c_str(), m_members.size());
        return false;
    }

    sai_attribute_t nhg_attr;
    vector<sai_attribute_t> nhg_attrs;

    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
    nhg_attr.value.s32 = SAI_NEXT_HOP_GROUP_TYPE_HW_PROTECTION;
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

    set<NextHopKey> member_keys;
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

bool ProtNhg::setAdminRole(sai_int32_t admin_role)
{
    SWSS_LOG_ENTER();

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

bool ProtNhg::updateMemberMonitoredObject(const NextHopKey &nh_key,
                                           sai_object_id_t monitored_oid)
{
    SWSS_LOG_ENTER();

    auto it = m_members.find(nh_key);
    if (it == m_members.end())
    {
        SWSS_LOG_ERROR("Member %s not found in protection NHG %s",
                       nh_key.to_string().c_str(), m_key.c_str());
        return false;
    }

    return it->second.updateMonitoredObject(monitored_oid);
}

vector<const ProtNhgMember*> ProtNhg::getPrimaryMembers() const
{
    SWSS_LOG_ENTER();

    vector<const ProtNhgMember*> primaries;
    for (const auto &mbr : m_members)
    {
        if (mbr.second.getRole() == ProtNhgRole::PRIMARY)
        {
            primaries.push_back(&mbr.second);
        }
    }

    return primaries;
}

const ProtNhgMember* ProtNhg::getStandbyMember() const
{
    SWSS_LOG_ENTER();

    for (const auto &mbr : m_members)
    {
        if (mbr.second.getRole() == ProtNhgRole::STANDBY)
        {
            return &mbr.second;
        }
    }

    return nullptr;
}

bool ProtNhg::getMemberObservedRole(
    const NextHopKey &nh_key,
    sai_next_hop_group_member_observed_role_t &observed_role) const
{
    SWSS_LOG_ENTER();

    auto it = m_members.find(nh_key);
    if (it == m_members.end())
    {
        SWSS_LOG_ERROR("Member %s not found in protection NHG %s",
                       nh_key.to_string().c_str(), m_key.c_str());
        return false;
    }

    return it->second.getObservedRole(observed_role);
}

bool ProtNhg::getAllMemberObservedRoles(
    map<NextHopKey, sai_next_hop_group_member_observed_role_t> &observed_roles) const
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
                          mbr.first.to_string().c_str(), m_key.c_str());
            success = false;
        }
    }

    return success;
}

bool ProtNhg::syncMembers(const set<NextHopKey> &member_keys)
{
    SWSS_LOG_ENTER();

    ObjectBulker<sai_next_hop_group_api_t> bulker(sai_next_hop_group_api,
                                                   gSwitchId,
                                                   gMaxBulkSize);
    map<NextHopKey, sai_object_id_t> syncing;

    for (const auto &nh_key : member_keys)
    {
        ProtNhgMember &nhgm = m_members.at(nh_key);

        if (nhgm.isSynced())
        {
            continue;
        }

        if (nhgm.getNhId() == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("Next hop %s not resolved for protection NHG %s",
                          nh_key.to_string().c_str(), m_key.c_str());
            continue;
        }

        vector<sai_attribute_t> attrs = createNhgmAttrs(nhgm);
        bulker.create_entry(&syncing[nh_key],
                            static_cast<uint32_t>(attrs.size()),
                            attrs.data());
    }

    bulker.flush();

    bool success = true;
    for (const auto &entry : syncing)
    {
        if (entry.second == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("Failed to create member %s of protection NHG %s",
                           entry.first.to_string().c_str(), m_key.c_str());
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
