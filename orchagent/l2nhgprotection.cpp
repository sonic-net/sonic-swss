#include "l2nhgorch.h"
#include "vxlanorch.h"
#include "portsorch.h"
#include "directory.h"
#include "swssnet.h"
#include <cinttypes>

extern Directory<Orch*> gDirectory;
extern PortsOrch *gPortsOrch;
extern sai_object_id_t gSwitchId;
extern sai_next_hop_api_t *sai_next_hop_api;
extern sai_next_hop_group_api_t *sai_next_hop_group_api;

namespace
{
const string fieldType = "type";
const string fieldVtep = "remote_vtep";
const string fieldRmac = "router_mac";
const string fieldGroup = "nexthop_group";
bool protectionId(const string &text, uint32_t &id)
{
    if (text.empty() || text.front() == '0') return false;
    uint64_t value = 0;
    for (const auto digit : text)
    {
        if (digit < '0' || digit > '9') return false;
        value = value * 10 + static_cast<unsigned>(digit - '0');
        if (value > UINT32_MAX) return false;
    }
    id = static_cast<uint32_t>(value);
    return true;
}
}

sai_object_id_t L2NhgOrch::createSaiBridgeNextHop(sai_object_id_t tunnel, const string &ip, bool protection)
{
    sai_attribute_t attrs[3]{};
    attrs[0].id = SAI_NEXT_HOP_ATTR_TYPE;
    attrs[0].value.s32 = SAI_NEXT_HOP_TYPE_BRIDGE_PORT;
    attrs[1].id = SAI_NEXT_HOP_ATTR_IP;
    swss::copy(attrs[1].value.ipaddr, swss::IpAddress(ip));
    attrs[2].id = SAI_NEXT_HOP_ATTR_TUNNEL_ID;
    attrs[2].value.oid = tunnel;
    sai_object_id_t oid = SAI_NULL_OBJECT_ID;
    const auto status = sai_next_hop_api->create_next_hop(&oid, gSwitchId, 3, attrs);
    if (status != SAI_STATUS_SUCCESS)
    {
        if (!protection) parseHandleSaiStatusFailure(handleSaiCreateStatus(SAI_API_NEXT_HOP, status));
        return SAI_NULL_OBJECT_ID;
    }
    return oid;
}

bool L2NhgOrch::updateProtectionRow(const KeyOpFieldsValuesTuple &row)
{
    const auto &key = kfvKey(row);
    uint32_t id = 0;
    map<string, string> fields;
    for (const auto &fv : kfvFieldsValues(row)) fields[fvField(fv)] = fvValue(fv);
    try
    {
        if (!protectionId(key, id) || m_nhg_vtep.count(key) || m_nhg_nh.count(key))
            throw std::invalid_argument("invalid or occupied protection id");
        if (fields.count(fieldType))
        {
            if (m_protectionGroups.count(key) || fields.count(fieldGroup) ||
                (fields[fieldType] != "l3" && fields[fieldType] != "l2"))
                throw std::invalid_argument("invalid protection Member type");
            swss::IpAddress ip(fields[fieldVtep]);
            if (ip.isZero()) throw std::invalid_argument("zero protection VTEP");
            swss::MacAddress mac;
            if (!fields[fieldRmac].empty())
            {
                mac = swss::MacAddress(fields[fieldRmac]);
                if (!mac || (mac.getMac()[0] & 1)) throw std::invalid_argument("invalid protection RMAC");
            }
            // A replay must not cancel an observed withdrawal while old
            // group/attachment references still own this Member incarnation.
            auto retiring = m_protectionMembers.find(key);
            if (retiring != m_protectionMembers.end() && retiring->second.deleted) return false;
            auto &member = m_protectionMembers[key];
            member.owner = id;
            member.ip = ip.to_string();
            member.mac = mac;
            member.l3 = fields[fieldType] == "l3";
            member.deleted = false;
        }
        else
        {
            if (m_protectionMembers.count(key) || !fields.count(fieldGroup) ||
                fields.count(fieldVtep) || fields.count(fieldRmac))
                throw std::invalid_argument("invalid protection Group row");
            std::set<string> desired;
            const auto &list = fields[fieldGroup];
            if (!list.empty())
            {
                if (list.front() == ',' || list.back() == ',' || list.find(",,") != string::npos)
                    throw std::invalid_argument("empty protection Group member");
                for (const auto &item : tokenize(list, ','))
                {
                    uint32_t memberId = 0;
                    if (!protectionId(item, memberId) || !desired.insert(item).second || desired.size() > 4096)
                        throw std::invalid_argument("invalid protection Group membership");
                }
            }
            // Keep a valid replacement queued until the old SAI group has
            // actually been removed. In particular, an FPM replay cannot turn
            // a consumed DEL back into retention of the previous incarnation.
            auto retiring = m_protectionGroups.find(key);
            if (retiring != m_protectionGroups.end() && retiring->second.deleted) return false;
            auto &group = m_protectionGroups[key];
            group.id = id;
            group.desired = std::move(desired);
            group.deleted = false;
            group.ready = false;
        }
    }
    catch (const std::exception &error)
    {
        SWSS_LOG_ERROR("Reject protection row %s: %s", key.c_str(), error.what());
    }
    // Invalid rows are consumed without replacing a previously accepted object.
    return true;
}

bool L2NhgOrch::deleteProtectionRow(const string &key)
{
    auto member = m_protectionMembers.find(key);
    if (member != m_protectionMembers.end()) member->second.deleted = true;
    auto group = m_protectionGroups.find(key);
    if (group != m_protectionGroups.end()) group->second.deleted = true;
    return true;
}

bool L2NhgOrch::protectionMemberReady(const string &key) const
{
    auto member = m_protectionMembers.find(key);
    if (member == m_protectionMembers.end()) return false;
    const auto &m = member->second;
    auto peer = m_protectionPeers.find(m.peer);
    if (m.deleted || !m.l3 || m.peer != m.ip || peer == m_protectionPeers.end() ||
        peer->second.nh == SAI_NULL_OBJECT_ID) return false;
    if (peer->second.mac != m.mac)
    {
        // An agreed RMAC-only update keeps the last installed forwarding path
        // during SET retries. This does not certify the requested MAC installed.
        swss::MacAddress target;
        if (!getProtectionRmacTarget(m.peer, target) || target != m.mac) return false;
    }
    auto nvo = gDirectory.get<EvpnNvoOrch*>();
    auto source = nvo ? nvo->getEVPNVtep() : nullptr;
    return source && source->isActive() && !source->del_tnl_hw_pending &&
           source->getTunnelName() == peer->second.source;
}

bool L2NhgOrch::getProtectionRmacTarget(const string &ip, swss::MacAddress &mac) const
{
    const auto &peer = m_protectionPeers.at(ip);
    if (peer.owners.empty()) return false;
    bool first = true;
    for (const auto owner : peer.owners)
    {
        const auto found = m_protectionMembers.find(std::to_string(owner));
        if (found == m_protectionMembers.end()) return false;
        const auto &member = found->second;
        if (member.deleted || !member.l3 || !member.mac || member.peer != ip || member.ip != ip) return false;
        if (!first && mac != member.mac) return false;
        mac = member.mac;
        first = false;
    }
    return true;
}

void L2NhgOrch::reconcileProtectionRmac()
{
    auto tunnels = gDirectory.get<VxlanTunnelOrch*>();
    auto nvo = gDirectory.get<EvpnNvoOrch*>();
    auto source = nvo ? nvo->getEVPNVtep() : nullptr;
    if (!source || !source->isActive() || source->del_tnl_hw_pending) return;
    for (auto &entry : m_protectionPeers)
    {
        auto &peer = entry.second;
        swss::MacAddress target;
        if (peer.nh == SAI_NULL_OBJECT_ID || peer.source != source->getTunnelName() ||
            !getProtectionRmacTarget(entry.first, target) || target == peer.mac ||
            !tunnels->isTunnelExists(peer.tunnel)) continue;
        if (tunnels->getVxlanTunnel(peer.tunnel)->updateProtectionRmac(peer.owners, target))
            peer.mac = target; // Actual successful SET; newer intent is checked again below/later.
    }
}

bool L2NhgOrch::releaseProtectionPeer(const string &ip, ProtectionPeer &peer)
{
    auto tunnels = gDirectory.get<VxlanTunnelOrch*>();
    if (!tunnels->isTunnelExists(peer.source)) return false;
    auto source = tunnels->getVxlanTunnel(peer.source);
    if (!source->deleteDynamicDIPTunnel(ip, TUNNEL_USER_PROTECTION)) return false;
    source->cleanupDynamicDIPTunnel(ip);
    source->deletePendingSIPTunnel();
    return true;
}

bool L2NhgOrch::releaseProtectionMember(ProtectionMember &member)
{
    if (member.peer.empty()) return true;
    const auto id = std::to_string(member.owner);
    for (const auto &group : m_protectionGroups)
        if (group.second.members.count(id)) return false;
    auto &peer = m_protectionPeers.at(member.peer);
    auto tunnels = gDirectory.get<VxlanTunnelOrch*>();
    if (!tunnels->isTunnelExists(peer.tunnel)) return false;
    if (peer.owners.size() == 1 && peer.nh != SAI_NULL_OBJECT_ID)
    {
        if (sai_next_hop_api->remove_next_hop(peer.nh) != SAI_STATUS_SUCCESS) return false;
        peer.nh = SAI_NULL_OBJECT_ID;
    }
    if (!tunnels->getVxlanTunnel(peer.tunnel)->releaseProtectionRmac(member.owner)) return false;
    peer.owners.erase(member.owner);
    member.peer.clear();
    // Empty peers retain their pin until the next reconciliation can remove it.
    return true;
}

bool L2NhgOrch::ensureProtectionMember(ProtectionMember &member)
{
    if (member.deleted || !member.l3 || !member.mac) return false;
    auto tunnels = gDirectory.get<VxlanTunnelOrch*>();
    auto nvo = gDirectory.get<EvpnNvoOrch*>();
    auto source = nvo ? nvo->getEVPNVtep() : nullptr;
    if (!source || !source->isActive() || source->del_tnl_hw_pending || !tunnels->isDipTunnelsSupported()) return false;
    if (IpAddress(member.ip).isV4() != source->getSrcIP().isV4()) return false;
    if (!member.peer.empty()) return protectionMemberReady(std::to_string(member.owner));
    auto existing = m_protectionPeers.find(member.ip);
    if (existing != m_protectionPeers.end())
    {
        if (existing->second.mac != member.mac || existing->second.owners.empty() ||
            existing->second.source != source->getTunnelName()) return false;
    }
    else
    {
        if (!source->createDynamicDIPTunnel(member.ip, TUNNEL_USER_PROTECTION)) return false;
        ProtectionPeer peer;
        peer.source = source->getTunnelName();
        tunnels->getTunnelNameFromDIP(member.ip, peer.tunnel);
        peer.mac = member.mac;
        existing = m_protectionPeers.emplace(member.ip, std::move(peer)).first;
    }
    auto &peer = existing->second;
    peer.owners.insert(member.owner);
    member.peer = member.ip;
    auto tunnel = tunnels->getVxlanTunnel(peer.tunnel);
    if (!tunnel->setProtectionRmac(member.owner, member.mac))
    {
        releaseProtectionMember(member);
        return false;
    }
    if (peer.nh == SAI_NULL_OBJECT_ID)
    {
        peer.nh = createSaiBridgeNextHop(tunnel->getTunnelId(), member.ip, true);
        if (peer.nh == SAI_NULL_OBJECT_ID)
        {
            releaseProtectionMember(member);
            return false;
        }
    }
    return true;
}

bool L2NhgOrch::reconcileProtectionGroup(ProtectionGroup &group, bool withdrawOnly, bool removeMembers)
{
    bool complete = true;
    std::set<sai_object_id_t> failedRemovals;
    for (auto it = group.members.begin(); removeMembers && it != group.members.end();)
    {
        if (group.deleted || !group.desired.count(it->first) || !protectionMemberReady(it->first))
        {
            // Multiple context IDs may describe the same physical peer. Keep
            // its group member while any other desired context still uses it.
            bool retained = false;
            for (const auto &other : group.members)
                if (!group.deleted && other.first != it->first && other.second == it->second &&
                    group.desired.count(other.first) && protectionMemberReady(other.first)) retained = true;
            if (!retained)
            {
                // Successful later removals restart iteration to handle aliases.
                // Do not retry the same failed physical object within this pass.
                if (failedRemovals.count(it->second)) { complete = false; ++it; continue; }
                const auto status = sai_next_hop_group_api->remove_next_hop_group_member(it->second);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("event=protection_member_remove_failed nhg_id=%u nhg_oid=0x%" PRIx64
                        " member_id=%s member_oid=0x%" PRIx64 " status=%d action=retain_and_continue",
                        group.id, group.oid, it->first.c_str(), it->second, status);
                    failedRemovals.insert(it->second);
                    complete = false;
                    ++it;
                    continue;
                }
            }
            const auto oid = it->second;
            it = group.members.erase(it);
            if (!retained)
            {
                for (auto other = group.members.begin(); other != group.members.end();)
                    if (other->second == oid) other = group.members.erase(other);
                    else ++other;
                it = group.members.begin();
            }
        }
        else ++it;
    }
    if (withdrawOnly)
    {
        // Other groups' READY callbacks can resolve an unchanged group before
        // its full pass; do not transiently hide an already usable path.
        group.ready = !group.withdrawing && hasUsableProtectionMember(group);
        return complete;
    }
    group.ready = false;
    if (group.deleted)
    {
        // A failed member still owns its group and all downstream resources.
        if (!group.members.empty()) return false;
        if (group.oid != SAI_NULL_OBJECT_ID)
        {
            if (!removeSaiNextHopGroup(group.oid, true)) return false;
            // Physical removal is distinct from the earlier detach request.
            notifyProtectionGroup(group, L2NhgChange::REMOVED);
            group.oid = SAI_NULL_OBJECT_ID;
        }
        return true;
    }
    if (group.oid == SAI_NULL_OBJECT_ID)
    {
        group.oid = createSaiNextHopGroup(true);
        if (group.oid == SAI_NULL_OBJECT_ID) return false;
    }
    for (const auto &key : group.desired)
    {
        if (group.members.count(key) || !protectionMemberReady(key)) continue;
        const auto &member = m_protectionMembers.at(key);
        const auto &peer = m_protectionPeers.at(member.peer);
        sai_object_id_t shared = SAI_NULL_OBJECT_ID;
        for (const auto &installed : group.members)
            if (m_protectionMembers.at(installed.first).peer == member.peer) shared = installed.second;
        if (shared != SAI_NULL_OBJECT_ID)
        {
            group.members[key] = shared;
            continue;
        }
        sai_attribute_t attrs[2]{};
        attrs[0].id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
        attrs[0].value.oid = group.oid;
        attrs[1].id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
        attrs[1].value.oid = peer.nh;
        sai_object_id_t oid = SAI_NULL_OBJECT_ID;
        if (sai_next_hop_group_api->create_next_hop_group_member(&oid, gSwitchId, 2, attrs) != SAI_STATUS_SUCCESS)
        {
            group.ready = hasUsableProtectionMember(group);
            return false;
        }
        group.members[key] = oid;
    }
    group.ready = hasUsableProtectionMember(group);
    return complete;
}

void L2NhgOrch::reconcileProtection()
{
    // Finish failed last-user tunnel cleanup before accepting another pin.
    for (auto it = m_protectionPeers.begin(); it != m_protectionPeers.end();)
    {
        if (it->second.owners.empty() && releaseProtectionPeer(it->first, it->second))
            it = m_protectionPeers.erase(it);
        else ++it;
    }
    // Mutate agreed tunnel attributes before any readiness/withdrawal decision.
    // Each shared peer is attempted once; failed SET retains its installed path.
    reconcileProtectionRmac();
    // Withdraw group references before mutating/releasing any Member resource.
    std::set<string> blockedWithdrawals, failedRemovals;
    for (auto &group : m_protectionGroups)
    {
        if (!prepareProtectionGroup(group.second)) blockedWithdrawals.insert(group.first);
        else if (!reconcileProtectionGroup(group.second, true)) failedRemovals.insert(group.first);
    }
    for (auto it = m_protectionMembers.begin(); it != m_protectionMembers.end();)
    {
        auto &m = it->second;
        if (!m.peer.empty() && !protectionMemberReady(it->first))
        {
            if (!releaseProtectionMember(m)) { ++it; continue; }
        }
        if (m.deleted) it = m_protectionMembers.erase(it);
        else { ensureProtectionMember(m); ++it; }
    }
    for (auto it = m_protectionGroups.begin(); it != m_protectionGroups.end();)
    {
        // Partial deletion failure must not starve unrelated ready additions.
        // The first pass already attempted every removable physical member.
        if (!blockedWithdrawals.count(it->first))
            reconcileProtectionGroup(it->second, false, !failedRemovals.count(it->first));
        finishProtectionGroup(it->second);
        if (it->second.deleted && it->second.oid == SAI_NULL_OBJECT_ID) it = m_protectionGroups.erase(it);
        else ++it;
    }
}

void L2NhgOrch::doTask()
{
    Orch::doTask();
    if (gPortsOrch->allPortsReady()) reconcileProtection();
}
