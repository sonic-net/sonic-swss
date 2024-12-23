#include "dashoffloadpavalidation.h"

#include "logger_utils.h"
#include "dash/taskworker.h"
#include "dash/pbutils.h"

#include <sstream>
#include <vector>
#include <algorithm>

#include <boost/functional/hash.hpp>

#define ACL_OFFLOAD_TABLE_NAME_PREFIX   "DASH_PA_VALIDATION_DPU"
#define ACL_OFFLOAD_TABLE_TYPE          "DASH_PA_VALIDATION"
#define ACL_RULE_FORWARD_PRIO           "10"
#define ACL_RULE_DROP_PRIO              "1"

using namespace swss;
using namespace std;

std::once_flag DashPaVlidationOffloadOrch::table_type_create_once;

size_t IpAddressHash::operator()(swss::IpAddress addr) const
{
    size_t seed = 0;
    const auto &inner = addr.getIp();
    boost::hash_combine(seed, inner.family);
    if (inner.family == AF_INET)
    {
        boost::hash_combine(seed, inner.ip_addr.ipv4_addr);
    }
    else if (inner.family == AF_INET6)
    {
        boost::hash_combine(seed, inner.ip_addr.ipv6_addr);
    }
    return seed;
}

PaValidationEntry::PaValidationEntry(const std::string& key, const dash::pa_validation::PaValidation &pbEntry)
    : vni(key)
{
    addresses.reserve(pbEntry.addresses().size());

    for (const auto& addr : pbEntry.addresses())
    {
        if (addr.has_ipv4())
        {
            swss::ip_addr_t addrv4 = {.family = AF_INET, .ip_addr = {.ipv4_addr = addr.ipv4()}};
            addresses.push_back(IpAddress(addrv4));
        }

        if (addr.has_ipv6())
        {
            swss::ip_addr_t addrv6 = {.family = AF_INET6, .ip_addr = {}};
            memcpy(addrv6.ip_addr.ipv6_addr, addr.ipv6().c_str(), sizeof(addrv6.ip_addr.ipv6_addr));
            addresses.push_back(IpAddress(addrv6));
        }
    }
}

DashPaVlidationOffloadOrch::DashPaVlidationOffloadOrch(const DpuInfo& dpuInfo, swss::DBConnector *applDb, ZmqServer *zmqServer)
    : ZmqOrch(applDb, vector<string>{APP_DASH_PA_VALIDATION_TABLE_NAME}, zmqServer),
    m_dpuInfo(dpuInfo),
    m_dpuLogKey(makeDpuLogKey(dpuInfo.dpuId)),
    m_applAclTableTypeTable(applDb, APP_ACL_TABLE_TYPE_TABLE_NAME),
    m_applAclTableTable(applDb, APP_ACL_TABLE_TABLE_NAME),
    m_applAclRuleTable(applDb, APP_ACL_RULE_TABLE_NAME)
{
    SWSS_LOG_ENTER();

    DASH_MNG_LOG_NOTICE("Started PA Validation offload");

    initializeAclConfig();
}

DashPaVlidationOffloadOrch::~DashPaVlidationOffloadOrch()
{
    cleanup();
}

void DashPaVlidationOffloadOrch::createAclTableType()
{
    SWSS_LOG_ENTER();

    const static vector<swss::FieldValueTuple> config = {
        {"MATCHES", "TUNNEL_VNI,SRC_IP,SRC_IPV6"},
        {"ACTIONS", "PACKET_ACTION"},
        {"BIND_POINTS", "PORT"}
    };

    DASH_MNG_LOG_NOTICE("Creating ACL Table type for PA Validation offload");

    m_applAclTableTypeTable.set(ACL_OFFLOAD_TABLE_TYPE, config);
}

void DashPaVlidationOffloadOrch::initializeAclConfig()
{
    call_once(DashPaVlidationOffloadOrch::table_type_create_once, &DashPaVlidationOffloadOrch::createAclTableType, this);
}

void DashPaVlidationOffloadOrch::addOffloadAclTable()
{
    SWSS_LOG_ENTER();

    if (!m_offloadAclTable.empty())
    {
        return;
    }

    m_offloadAclTable = ACL_OFFLOAD_TABLE_NAME_PREFIX + to_string(m_dpuInfo.dpuId);

    vector<FieldValueTuple> table_attrs = {
        FieldValueTuple("policy_desc", "PA Validation offload table"),
        FieldValueTuple("type", ACL_OFFLOAD_TABLE_TYPE),
        FieldValueTuple("stage", "egress"),
        FieldValueTuple("ports", m_dpuInfo.interfaces),
    };

    DASH_MNG_LOG_NOTICE("Creating ACL Table for PA Validation offload - %s (%s)", m_offloadAclTable.c_str(), m_dpuInfo.interfaces.c_str());

    m_applAclTableTable.set(m_offloadAclTable, table_attrs);
}

void DashPaVlidationOffloadOrch::cleanup()
{
    SWSS_LOG_ENTER();

    if (m_offloadAclTable.empty())
    {
        return;
    }

    for (const auto& vni_info : m_vni_map)
    {
        const auto& vni = vni_info.first;
        const auto& addresses = vni_info.second;

        removeOffloadAclConfig(vni, addresses.size());
    }

    DASH_MNG_LOG_NOTICE("Removing ACL Table for PA Validation offload - %s", m_offloadAclTable.c_str());

    m_applAclTableTable.del(m_offloadAclTable);
    m_offloadAclTable.clear();
}

bool DashPaVlidationOffloadOrch::validateEntryAdd(PaValidationEntry& entry, size_t& num_of_existing_addresses) const
{
    SWSS_LOG_ENTER();

    auto vni_ips = m_vni_map.find(entry.vni);
    if (vni_ips == m_vni_map.end())
    {
        num_of_existing_addresses = 0;
        return true;
    }

    entry.addresses.erase(remove_if(entry.addresses.begin(), entry.addresses.end(),
            [&vni_ips, &entry, this](const auto &addr) {
                bool exists = vni_ips->second.find(addr) != vni_ips->second.end();
                if (exists)
                {
                    DASH_MNG_LOG_WARN("Address %s is already added to VNI %s PA Validation offload", addr.to_string().c_str(), entry.vni.c_str());
                }

                return exists;
            }),
        entry.addresses.end()
    );

    num_of_existing_addresses = vni_ips->second.size();

    return false;
}

void DashPaVlidationOffloadOrch::entryAddToVniMap(const PaValidationEntry& entry)
{
    m_vni_map[entry.vni].insert(entry.addresses.begin(), entry.addresses.end());
}

std::string DashPaVlidationOffloadOrch::makeAclRuleKey(const std::string &vni, size_t rule_idx) const
{
    return m_offloadAclTable + ":RULE_VNI_" + vni + "_" + to_string(rule_idx);
}

std::string DashPaVlidationOffloadOrch::makeAclDropRuleKey(const std::string &vni) const
{
    return m_offloadAclTable + ":RULE_VNI_" + vni + "_DROP";
}

void DashPaVlidationOffloadOrch::buildAclRules(const PaValidationEntry& entry, size_t rule_index_base, std::vector<AclRule> &forward_rules, AclRule &drop_rule)
{
    SWSS_LOG_ENTER();

    forward_rules.reserve(entry.addresses.size() + 1);

    for (const auto &addr : entry.addresses)
    {
        auto ip_match_pype = addr.isV4() ? "SRC_IP" : "SRC_IPV6";
        auto mask = addr.isV4() ? "/32" : "/128";
        auto ip = addr.to_string();

        vector<FieldValueTuple> rule_attrs = {
            FieldValueTuple("priority", ACL_RULE_FORWARD_PRIO),
            FieldValueTuple("PACKET_ACTION", "FORWARD"),
            FieldValueTuple("TUNNEL_VNI", entry.vni),
            FieldValueTuple(ip_match_pype, ip + mask)
        };

        auto rule_key = makeAclRuleKey(entry.vni, rule_index_base++);

        DASH_MNG_LOG_INFO("Creating ACL forward rule %s (vni:%s address:%s)", rule_key.c_str(), entry.vni.c_str(), ip.c_str());

        forward_rules.push_back({rule_key, rule_attrs});
    }

    auto rule_key = makeAclDropRuleKey(entry.vni);

    vector<FieldValueTuple> rule_attrs = {
        FieldValueTuple("priority", ACL_RULE_DROP_PRIO),
        FieldValueTuple("PACKET_ACTION", "DROP"),
        FieldValueTuple("TUNNEL_VNI", entry.vni)
    };

    DASH_MNG_LOG_INFO("Creating ACL drop rule %s (vni:%s)", rule_key.c_str(), entry.vni.c_str());

    drop_rule = {rule_key, rule_attrs};
}

void DashPaVlidationOffloadOrch::addOffloadAclConfig(const PaValidationEntry& entry, bool new_vni, size_t rule_index_base)
{
    SWSS_LOG_ENTER();

    addOffloadAclTable();

    std::vector<AclRule> forward_rules;
    AclRule drop_rule;

    buildAclRules(entry, rule_index_base, forward_rules, drop_rule);

    for (const auto& rule : forward_rules)
    {
        const auto& key = get<0>(rule);
        const auto& attrs = get<1>(rule);

        m_applAclRuleTable.set(key, attrs);
    }

    if (new_vni)
    {
        const auto& key = get<0>(drop_rule);
        const auto& attrs = get<1>(drop_rule);

        m_applAclRuleTable.set(key, attrs);
    }
}

void DashPaVlidationOffloadOrch::removeOffloadAclConfig(const string& vni, size_t num_of_rules)
{
    SWSS_LOG_ENTER();

    for (size_t rule_idx = 0; rule_idx < num_of_rules; rule_idx++)
    {
        auto rule_key = makeAclRuleKey(vni, rule_idx);
        DASH_MNG_LOG_INFO("Removing ACL forward rule %s", rule_key.c_str());
        m_applAclRuleTable.del(rule_key);
    }

    auto rule_key = makeAclDropRuleKey(vni);
    DASH_MNG_LOG_INFO("Removing ACL drop rule %s", rule_key.c_str());
    m_applAclRuleTable.del(rule_key);
}

void DashPaVlidationOffloadOrch::addPaValidationEntry(PaValidationEntry &entry)
{
    SWSS_LOG_ENTER();

    size_t num_of_existing_addresses;
    bool new_vni = validateEntryAdd(entry, num_of_existing_addresses);

    addOffloadAclConfig(entry, new_vni, num_of_existing_addresses);

    entryAddToVniMap(entry);
}

void DashPaVlidationOffloadOrch::removePaValidationEntry(const string& paValidationKey)
{
    SWSS_LOG_ENTER();

    auto vni = m_vni_map.find(paValidationKey);
    if (vni == m_vni_map.end())
    {
        DASH_MNG_LOG_WARN("VNI %s is removed or not created yet", paValidationKey.c_str());
        return;
    }

    removeOffloadAclConfig(paValidationKey, vni->second.size());

    m_vni_map.erase(vni);
}

void DashPaVlidationOffloadOrch::doTask(ConsumerBase& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;
        string paValidationKey = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            dash::pa_validation::PaValidation pbEntry;

            if (!parsePbMessage(kfvFieldsValues(t), pbEntry))
            {
                DASH_MNG_LOG_WARN("Failed to parse protobuff messaage for PA Validation: %s", paValidationKey.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            auto entry = PaValidationEntry(paValidationKey, pbEntry);
            addPaValidationEntry(entry);
        }
        else if (op == DEL_COMMAND)
        {
            removePaValidationEntry(paValidationKey);
        }
        else
        {
            DASH_MNG_LOG_ERROR("Unknown operation %s", op.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}
