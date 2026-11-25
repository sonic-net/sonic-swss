#include "bpprotorch.h"
#include "fdborch.h"
#include "portsorch.h"

extern FdbOrch *gFdbOrch;

bool BpProtOrch::hasProtectionIntent(const std::string &ifname)
{
    if (m_entries.count(ifname) || m_bindings.count(ifname)) return true;
    // PortsOrch can publish DOWN before the policy/binding Consumer runs.
    // Existing Redis rows conservatively declare intent during that race.
    std::vector<swss::FieldValueTuple> fields;
    return m_configTable.get(ifname, fields) ||
        (m_bindingTable && m_bindingTable->get(ifname, fields));
}

BpProtectionStatus BpProtOrch::isProtectedBridgePort(const std::string &ifname)
{
    if (!hasProtectionIntent(ifname))
    {
        // A previously observed row may have vanished before its DEL is
        // consumed. Only updateProtectionIntent confirms the withdrawal edge.
        return m_fdbProtectionIntent.count(ifname) ? BpProtectionStatus::PENDING : BpProtectionStatus::NONE;
    }
    m_fdbProtectionIntent.insert(ifname);
    const auto binding = m_bindings.find(ifname);
    const auto attachment = m_attachments.find(ifname);
    if (!m_entries.count(ifname) || binding == m_bindings.end() || attachment == m_attachments.end())
        return BpProtectionStatus::PENDING;
    const auto &installed = attachment->second;
    swss::Port current;
    sai_object_id_t group = SAI_NULL_OBJECT_ID;
    if (!installed.active || installed.removing || !installed.reference.token ||
        binding->second != installed.reference.nhg_id ||
        !m_ports->getPort(ifname, current) || current.m_bridge_port_id != installed.port ||
        current.m_bridge_port_generation != installed.generation ||
        !m_groups->getNhgOid(binding->second, group) || group != installed.reference.oid)
        return BpProtectionStatus::PENDING;
    return BpProtectionStatus::PROTECTED;
}

void BpProtOrch::updateProtectionIntent(const std::string &ifname)
{
    if (hasProtectionIntent(ifname))
    {
        m_fdbProtectionIntent.insert(ifname);
        return;
    }
    // Erase before calling out: duplicates/reentry cannot repeat this edge.
    if (!m_fdbProtectionIntent.erase(ifname)) return;
    BP_PROT_LOG(SWSS_NOTICE, "event=protection_withdrawn ifname=%s fdb_predicate=NONE", ifname.c_str());
    if (m_ports && gFdbOrch) gFdbOrch->flushProtectedPortOnWithdraw(ifname);
}
