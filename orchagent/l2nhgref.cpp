#include "l2nhgorch.h"
#include <limits>

bool L2NhgOrch::getNhgOid(uint32_t nhg_id, sai_object_id_t &oid) const
{
    // Caveat: readiness is local cache
    // state, without an FPM session generation. Recovery currently assumes
    // orchagent restarts with BGP. If orchagent survives an FPM reset, a replayed
    // binding can resolve the old ready group before object updates reconcile.
    // Session invalidation/withdrawal ordering is deferred, not guaranteed here.
    oid = SAI_NULL_OBJECT_ID;
    auto group = m_protectionGroups.find(std::to_string(nhg_id));
    if (group == m_protectionGroups.end() || !group->second.ready ||
        group->second.deleted || group->second.withdrawing) return false;
    oid = group->second.oid;
    return oid != SAI_NULL_OBJECT_ID;
}

bool L2NhgOrch::refNhg(uint32_t nhg_id)
{
    sai_object_id_t oid;
    if (!getNhgOid(nhg_id, oid)) return false;
    auto &group = m_protectionGroups.at(std::to_string(nhg_id));
    if (group.legacyReferences == std::numeric_limits<uint32_t>::max()) return false;
    ++group.legacyReferences;
    return true;
}

void L2NhgOrch::unrefNhg(uint32_t nhg_id)
{
    auto group = m_protectionGroups.find(std::to_string(nhg_id));
    if (group != m_protectionGroups.end() && group->second.legacyReferences)
        --group->second.legacyReferences;
}

bool L2NhgOrch::reserveNhg(uint32_t nhg_id, L2NhgReference &reference)
{
    // Never overwrite a live handle and lose the caller's original pin.
    if (reference.token) return false;
    sai_object_id_t oid;
    if (!getNhgOid(nhg_id, oid) || m_nextProtectionReference == std::numeric_limits<uint64_t>::max()) return false;
    auto &group = m_protectionGroups.at(std::to_string(nhg_id));
    const auto token = ++m_nextProtectionReference;
    group.references.emplace(token, false);
    reference = {nhg_id, oid, token};
    return true;
}

L2NhgOrch::ProtectionGroup *L2NhgOrch::findProtectionReference(const L2NhgReference &reference)
{
    auto group = m_protectionGroups.find(std::to_string(reference.nhg_id));
    if (group == m_protectionGroups.end() || group->second.oid != reference.oid ||
        !group->second.references.count(reference.token)) return nullptr;
    return &group->second;
}

bool L2NhgOrch::commitNhgRef(const L2NhgReference &reference)
{
    auto group = findProtectionReference(reference);
    if (!group || !group->ready || group->deleted || group->withdrawing) return false;
    group->references.at(reference.token) = true;
    return true;
}

void L2NhgOrch::unrefNhg(const L2NhgReference &reference)
{
    auto group = findProtectionReference(reference);
    if (group) group->references.erase(reference.token);
    // No inline deletion: observer callbacks may release pins during iteration.
}

bool L2NhgOrch::hasProtectionGroupRefs(const ProtectionGroup &group) const
{
    return group.legacyReferences || !group.references.empty();
}

void L2NhgOrch::notifyProtectionGroup(const ProtectionGroup &group, L2NhgChange change)
{
    L2NhgUpdate update{group.id, group.oid, change};
    notify(SUBJECT_TYPE_L2_NHG_CHANGE, &update);
}

bool L2NhgOrch::prepareProtectionGroup(ProtectionGroup &group)
{
    const bool usable = hasUsableProtectionMember(group);
    if (group.deleted || (group.announcedReady && !usable))
    {
        group.ready = false;
        if (!group.withdrawing)
        {
            group.withdrawing = true;
            group.announcedReady = false;
            if (group.oid != SAI_NULL_OBJECT_ID) notifyProtectionGroup(group, L2NhgChange::WITHDRAWING);
        }
    }
    // The callback runs before this check, so a synchronous successful detach
    // can release its pins immediately. Failed/asynchronous clear keeps the
    // whole group and its last path intact until a later task retry.
    if (group.withdrawing && hasProtectionGroupRefs(group))
    {
        group.ready = false;
        return false;
    }
    return true;
}

void L2NhgOrch::finishProtectionGroup(ProtectionGroup &group)
{
    if (!group.deleted && group.ready)
    {
        group.withdrawing = false;
        if (!group.announcedReady)
        {
            group.announcedReady = true;
            notifyProtectionGroup(group, L2NhgChange::READY);
        }
    }
    else if (group.announcedReady)
    {
        // Actual availability loss withdraws consumers before destructive cleanup.
        // A failed partial removal with a usable peer does not take this path.
        group.announcedReady = false;
        group.withdrawing = true;
        notifyProtectionGroup(group, L2NhgChange::WITHDRAWING);
    }
}

bool L2NhgOrch::hasUsableProtectionMember(const ProtectionGroup &group) const
{
    if (group.deleted) return false;
    for (const auto &member : group.members)
        if (group.desired.count(member.first) && protectionMemberReady(member.first)) return true;
    return false;
}
