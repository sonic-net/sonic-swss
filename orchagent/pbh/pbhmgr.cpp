// includes -----------------------------------------------------------------------------------------------------------

#include <cstdint>

#include <unordered_map>
#include <unordered_set>
#include <exception>
#include <string>

#include "pbhschema.h"
#include "ipaddress.h"
#include "converter.h"
#include "tokenize.h"
#include "logger.h"

#include "pbhmgr.h"

using namespace swss;

// constants ----------------------------------------------------------------------------------------------------------

static const std::unordered_map<std::string, sai_acl_entry_attr_t> pbhRulePacketActionMap =
{
    { PBH_RULE_PACKET_ACTION_SET_ECMP_HASH, SAI_ACL_ENTRY_ATTR_ACTION_SET_ECMP_HASH_ID },
    { PBH_RULE_PACKET_ACTION_SET_LAG_HASH,  SAI_ACL_ENTRY_ATTR_ACTION_SET_LAG_HASH_ID  }
};

static const std::unordered_map<std::string, bool> pbhRuleFlowCounterMap =
{
    { PBH_RULE_FLOW_COUNTER_ENABLED,  true  },
    { PBH_RULE_FLOW_COUNTER_DISABLED, false }
};

static const std::unordered_map<std::string, sai_native_hash_field_t> pbhHashFieldHashFieldMap =
{
    { PBH_HASH_FIELD_HASH_FIELD_INNER_IP_PROTOCOL, SAI_NATIVE_HASH_FIELD_INNER_IP_PROTOCOL },
    { PBH_HASH_FIELD_HASH_FIELD_INNER_L4_DST_PORT, SAI_NATIVE_HASH_FIELD_INNER_L4_DST_PORT },
    { PBH_HASH_FIELD_HASH_FIELD_INNER_L4_SRC_PORT, SAI_NATIVE_HASH_FIELD_INNER_L4_SRC_PORT },
    { PBH_HASH_FIELD_HASH_FIELD_INNER_DST_IPV4,    SAI_NATIVE_HASH_FIELD_INNER_DST_IPV4    },
    { PBH_HASH_FIELD_HASH_FIELD_INNER_SRC_IPV4,    SAI_NATIVE_HASH_FIELD_INNER_SRC_IPV4    },
    { PBH_HASH_FIELD_HASH_FIELD_INNER_DST_IPV6,    SAI_NATIVE_HASH_FIELD_INNER_DST_IPV6    },
    { PBH_HASH_FIELD_HASH_FIELD_INNER_SRC_IPV6,    SAI_NATIVE_HASH_FIELD_INNER_SRC_IPV6    }
};

// functions ----------------------------------------------------------------------------------------------------------

template<typename T>
static inline T toUInt(const std::string &hexStr)
{
    if (hexStr.substr(0, 2) != "0x")
    {
        throw std::invalid_argument("Invalid argument: '" + hexStr + "'");
    }

    return to_uint<T>(hexStr);
}

static inline std::uint8_t toUInt8(const std::string &hexStr)
{
    return toUInt<std::uint8_t>(hexStr);
}

static inline std::uint16_t toUInt16(const std::string &hexStr)
{
    return toUInt<std::uint16_t>(hexStr);
}

static inline std::uint32_t toUInt32(const std::string &hexStr)
{
    return toUInt<std::uint32_t>(hexStr);
}

// PBH helper ---------------------------------------------------------------------------------------------------------

bool PbhHelper::hasDependencies(const PbhContainer &obj) const
{
    return obj.getRefCount() > 0;
}

template<>
bool PbhHelper::validateDependencies(const PbhRule &obj) const
{
    const auto &tCit = this->tableMap.find(obj.table);
    if (tCit == this->tableMap.cend())
    {
        return false;
    }

    const auto &hCit = this->hashMap.find(obj.hash.value);
    if (hCit == this->hashMap.cend())
    {
        return false;
    }

    return true;
}

template<>
bool PbhHelper::validateDependencies(const PbhHash &obj) const
{
    for (const auto &cit : obj.hash_field_list.value)
    {
        const auto &hfCit = this->hashFieldMap.find(cit);
        if (hfCit == this->hashFieldMap.cend())
        {
            return false;
        }
    }

    return true;
}

template<>
bool PbhHelper::incRefCount(const PbhRule &obj)
{
    const auto &tCit = this->tableMap.find(obj.table);
    if (tCit == this->tableMap.cend())
    {
        return false;
    }

    const auto &hCit = this->hashMap.find(obj.hash.value);
    if (hCit == this->hashMap.cend())
    {
        return false;
    }

    auto &table = tCit->second;
    table.incrementRefCount();

    auto &hash = hCit->second;
    hash.incrementRefCount();

    return true;
}

template<>
bool PbhHelper::incRefCount(const PbhHash &obj)
{
    std::vector<std::unordered_map<std::string, PbhHashField>::iterator> itList;

    for (const auto &cit : obj.hash_field_list.value)
    {
        const auto &hfCit = this->hashFieldMap.find(cit);
        if (hfCit == this->hashFieldMap.cend())
        {
            return false;
        }

        itList.push_back(hfCit);
    }

    for (auto &it : itList)
    {
        auto &hashField = it->second;
        hashField.incrementRefCount();
    }

    return true;
}

template<>
bool PbhHelper::decRefCount(const PbhRule &obj)
{
    const auto &tCit = this->tableMap.find(obj.table);
    if (tCit == this->tableMap.cend())
    {
        return false;
    }

    const auto &hCit = this->hashMap.find(obj.hash.value);
    if (hCit == this->hashMap.cend())
    {
        return false;
    }

    auto &table = tCit->second;
    table.decrementRefCount();

    auto &hash = hCit->second;
    hash.decrementRefCount();

    return true;
}

template<>
bool PbhHelper::decRefCount(const PbhHash &obj)
{
    std::vector<std::unordered_map<std::string, PbhHashField>::iterator> itList;

    for (const auto &cit : obj.hash_field_list.value)
    {
        const auto &hfCit = this->hashFieldMap.find(cit);
        if (hfCit == this->hashFieldMap.cend())
        {
            return false;
        }

        itList.push_back(hfCit);
    }

    for (auto &it : itList)
    {
        auto &hashField = it->second;
        hashField.decrementRefCount();
    }

    return true;
}

template<>
auto PbhHelper::getPbhObjMap() const -> const std::unordered_map<std::string, PbhTable>&
{
    return this->tableMap;
}

template<>
auto PbhHelper::getPbhObjMap() const -> const std::unordered_map<std::string, PbhRule>&
{
    return this->ruleMap;
}

template<>
auto PbhHelper::getPbhObjMap() const -> const std::unordered_map<std::string, PbhHash>&
{
    return this->hashMap;
}

template<>
auto PbhHelper::getPbhObjMap() const -> const std::unordered_map<std::string, PbhHashField>&
{
    return this->hashFieldMap;
}

template<typename T>
bool PbhHelper::getPbhObj(T &obj, const std::string &key) const
{
    const auto &objMap = this->getPbhObjMap<T>();

    const auto &cit = objMap.find(key);
    if (cit == objMap.cend())
    {
        return false;
    }

    obj = cit->second;

    return true;
}

template bool PbhHelper::getPbhObj(PbhTable &obj, const std::string &key) const;
template bool PbhHelper::getPbhObj(PbhRule &obj, const std::string &key) const;
template bool PbhHelper::getPbhObj(PbhHash &obj, const std::string &key) const;
template bool PbhHelper::getPbhObj(PbhHashField &obj, const std::string &key) const;

bool PbhHelper::getPbhTable(PbhTable &table, const std::string &key) const
{
    return this->getPbhObj(table, key);
}

bool PbhHelper::getPbhRule(PbhRule &rule, const std::string &key) const
{
    return this->getPbhObj(rule, key);
}

bool PbhHelper::getPbhHash(PbhHash &hash, const std::string &key) const
{
    return this->getPbhObj(hash, key);
}

bool PbhHelper::getPbhHashField(PbhHashField &hashField, const std::string &key) const
{
    return this->getPbhObj(hashField, key);
}

template<>
auto PbhHelper::getPbhObjMap() -> std::unordered_map<std::string, PbhTable>&
{
    return this->tableMap;
}

template<>
auto PbhHelper::getPbhObjMap() -> std::unordered_map<std::string, PbhRule>&
{
    return this->ruleMap;
}

template<>
auto PbhHelper::getPbhObjMap() -> std::unordered_map<std::string, PbhHash>&
{
    return this->hashMap;
}

template<>
auto PbhHelper::getPbhObjMap() -> std::unordered_map<std::string, PbhHashField>&
{
    return this->hashFieldMap;
}

template <typename T>
bool PbhHelper::addPbhObj(const T &obj)
{
    auto &objMap = this->getPbhObjMap<T>();

    const auto &cit = objMap.find(obj.key);
    if (cit != objMap.cend())
    {
        return false;
    }

    objMap[obj.key] = obj;

    return true;
}

template bool PbhHelper::addPbhObj(const PbhTable &obj);
template bool PbhHelper::addPbhObj(const PbhRule &obj);
template bool PbhHelper::addPbhObj(const PbhHash &obj);
template bool PbhHelper::addPbhObj(const PbhHashField &obj);

bool PbhHelper::addPbhTable(const PbhTable &table)
{
    return this->addPbhObj(table);
}

bool PbhHelper::addPbhRule(const PbhRule &rule)
{
    return this->addPbhObj(rule);
}

bool PbhHelper::addPbhHash(const PbhHash &hash)
{
    return this->addPbhObj(hash);
}

bool PbhHelper::addPbhHashField(const PbhHashField &hashField)
{
    return this->addPbhObj(hashField);
}

template <typename T>
bool PbhHelper::updatePbhObj(const T &obj)
{
    auto &objMap = this->getPbhObjMap<T>();

    const auto &cit = objMap.find(obj.key);
    if (cit == objMap.cend())
    {
        return false;
    }

    objMap[obj.key] = obj;

    return true;
}

template bool PbhHelper::updatePbhObj(const PbhTable &obj);
template bool PbhHelper::updatePbhObj(const PbhRule &obj);
template bool PbhHelper::updatePbhObj(const PbhHash &obj);
template bool PbhHelper::updatePbhObj(const PbhHashField &obj);

bool PbhHelper::updatePbhTable(const PbhTable &table)
{
    return this->updatePbhObj(table);
}

bool PbhHelper::updatePbhRule(const PbhRule &rule)
{
    return this->updatePbhObj(rule);
}

bool PbhHelper::updatePbhHash(const PbhHash &hash)
{
    return this->updatePbhObj(hash);
}

bool PbhHelper::updatePbhHashField(const PbhHashField &hashField)
{
    return this->updatePbhObj(hashField);
}

template <typename T>
bool PbhHelper::removePbhObj(const std::string &key)
{
    auto &objMap = this->getPbhObjMap<T>();

    const auto &cit = objMap.find(key);
    if (cit == objMap.cend())
    {
        return false;
    }

    objMap.erase(cit);

    return true;
}

template bool PbhHelper::removePbhObj<PbhTable>(const std::string &key);
template bool PbhHelper::removePbhObj<PbhRule>(const std::string &key);
template bool PbhHelper::removePbhObj<PbhHash>(const std::string &key);
template bool PbhHelper::removePbhObj<PbhHashField>(const std::string &key);

bool PbhHelper::removePbhTable(const std::string &key)
{
    return this->removePbhObj<PbhTable>(key);
}

bool PbhHelper::removePbhRule(const std::string &key)
{
    return this->removePbhObj<PbhRule>(key);
}

bool PbhHelper::removePbhHash(const std::string &key)
{
    return this->removePbhObj<PbhHash>(key);
}

bool PbhHelper::removePbhHashField(const std::string &key)
{
    return this->removePbhObj<PbhHashField>(key);
}

bool PbhHelper::parsePbhTableInterfaceList(PbhTable &table, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    const auto &ifList = tokenize(value, ',');

    if (ifList.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty list is prohibited", field.c_str());
        return false;
    }

    table.interface_list.value = std::unordered_set<std::string>(ifList.cbegin(), ifList.cend());
    table.interface_list.is_set = true;

    if (table.interface_list.value.size() != ifList.size())
    {
        SWSS_LOG_WARN("Duplicate interfaces in field(%s): unexpected value(%s)", field.c_str(), value.c_str());
    }

    return true;
}

bool PbhHelper::parsePbhTableDescription(PbhTable &table, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty string is prohibited", field.c_str());
        return false;
    }

    table.description.value = value;
    table.description.is_set = true;

    return true;
}

bool PbhHelper::parsePbhTable(PbhTable &table) const
{
    SWSS_LOG_ENTER();

    for (const auto &cit : table.fieldValueMap)
    {
        const auto &field = cit.first;
        const auto &value = cit.second;

        if (field == PBH_TABLE_INTERFACE_LIST)
        {
            if (!this->parsePbhTableInterfaceList(table, field, value))
            {
                return false;
            }
        }
        else if (field == PBH_TABLE_DESCRIPTION)
        {
            if (!this->parsePbhTableDescription(table, field, value))
            {
                return false;
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown field(%s): skipping ...", field.c_str());
        }
    }

    return this->validatePbhTable(table);
}

bool PbhHelper::parsePbhRulePriority(PbhRule &rule, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    try
    {
        rule.priority.value = to_uint<sai_uint32_t>(value);
        rule.priority.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    return true;
}

bool PbhHelper::parsePbhRuleGreKey(PbhRule &rule, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    const auto &vmList = tokenize(value, '/');

    if (vmList.size() != 2)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): invalid value(%s)", field.c_str(), value.c_str());
        return false;
    }

    try
    {
        rule.gre_key.value = toUInt32(vmList.at(0));
        rule.gre_key.mask = toUInt32(vmList.at(1));
        rule.gre_key.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    return true;
}

bool PbhHelper::parsePbhRuleEtherType(PbhRule &rule, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    try
    {
        rule.ether_type.value = toUInt16(value);
        rule.ether_type.mask = 0xFFFF;
        rule.ether_type.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    return true;
}

bool PbhHelper::parsePbhRuleIpProtocol(PbhRule &rule, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    try
    {
        rule.ip_protocol.value = toUInt8(value);
        rule.ip_protocol.mask = 0xFF;
        rule.ip_protocol.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    return true;
}

bool PbhHelper::parsePbhRuleIpv6NextHeader(PbhRule &rule, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    try
    {
        rule.ipv6_next_header.value = toUInt8(value);
        rule.ipv6_next_header.mask = 0xFF;
        rule.ipv6_next_header.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    return true;
}

bool PbhHelper::parsePbhRuleL4DstPort(PbhRule &rule, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    try
    {
        rule.l4_dst_port.value = toUInt16(value);
        rule.l4_dst_port.mask = 0xFFFF;
        rule.l4_dst_port.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    return true;
}

bool PbhHelper::parsePbhRuleInnerEtherType(PbhRule &rule, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    try
    {
        rule.inner_ether_type.value = toUInt16(value);
        rule.inner_ether_type.mask = 0xFFFF;
        rule.inner_ether_type.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    return true;
}

bool PbhHelper::parsePbhRuleHash(PbhRule &rule, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    rule.hash.meta.name = field;
    rule.hash.value = value;
    rule.hash.is_set = true;

    return true;
}

bool PbhHelper::parsePbhRulePacketAction(PbhRule &rule, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    const auto &cit = pbhRulePacketActionMap.find(value);
    if (cit == pbhRulePacketActionMap.cend())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): invalid value(%s)", field.c_str(), value.c_str());
        return false;
    }

    rule.packet_action.meta.name = field;
    rule.packet_action.value = cit->second;
    rule.packet_action.is_set = true;

    return true;
}

bool PbhHelper::parsePbhRuleFlowCounter(PbhRule &rule, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    const auto &cit = pbhRuleFlowCounterMap.find(value);
    if (cit == pbhRuleFlowCounterMap.cend())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): invalid value(%s)", field.c_str(), value.c_str());
        return false;
    }

    rule.flow_counter.value = cit->second;
    rule.flow_counter.is_set = true;

    return true;
}

bool PbhHelper::parsePbhRule(PbhRule &rule) const
{
    SWSS_LOG_ENTER();

    for (const auto &cit : rule.fieldValueMap)
    {
        const auto &field = cit.first;
        const auto &value = cit.second;

        if (field == PBH_RULE_PRIORITY)
        {
            if (!this->parsePbhRulePriority(rule, field, value))
            {
                return false;
            }
        }
        else if (field == PBH_RULE_GRE_KEY)
        {
            if (!this->parsePbhRuleGreKey(rule, field, value))
            {
                return false;
            }
        }
        else if (field == PBH_RULE_ETHER_TYPE)
        {
            if (!this->parsePbhRuleEtherType(rule, field, value))
            {
                return false;
            }
        }
        else if (field == PBH_RULE_IP_PROTOCOL)
        {
            if (!this->parsePbhRuleIpProtocol(rule, field, value))
            {
                return false;
            }
        }
        else if (field == PBH_RULE_IPV6_NEXT_HEADER)
        {
            if (!this->parsePbhRuleIpv6NextHeader(rule, field, value))
            {
                return false;
            }
        }
        else if (field == PBH_RULE_L4_DST_PORT)
        {
            if (!this->parsePbhRuleL4DstPort(rule, field, value))
            {
                return false;
            }
        }
        else if (field == PBH_RULE_INNER_ETHER_TYPE)
        {
            if (!this->parsePbhRuleInnerEtherType(rule, field, value))
            {
                return false;
            }
        }
        else if (field == PBH_RULE_HASH)
        {
            if (!this->parsePbhRuleHash(rule, field, value))
            {
                return false;
            }
        }
        else if (field == PBH_RULE_PACKET_ACTION)
        {
            if (!this->parsePbhRulePacketAction(rule, field, value))
            {
                return false;
            }
        }
        else if (field == PBH_RULE_FLOW_COUNTER)
        {
            if (!this->parsePbhRuleFlowCounter(rule, field, value))
            {
                return false;
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown field(%s): skipping ...", field.c_str());
        }
    }

    return this->validatePbhRule(rule);
}

bool PbhHelper::parsePbhHashHashFieldList(PbhHash &hash, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    const auto &hfList = tokenize(value, ',');

    if (hfList.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty list is prohibited", field.c_str());
        return false;
    }

    hash.hash_field_list.value = std::unordered_set<std::string>(hfList.cbegin(), hfList.cend());
    hash.hash_field_list.is_set = true;

    if (hash.hash_field_list.value.size() != hfList.size())
    {
        SWSS_LOG_WARN("Duplicate hash fields in field(%s): unexpected value(%s)", field.c_str(), value.c_str());
    }

    return true;
}

bool PbhHelper::parsePbhHash(PbhHash &hash) const
{
    SWSS_LOG_ENTER();

    for (const auto &cit : hash.fieldValueMap)
    {
        const auto &field = cit.first;
        const auto &value = cit.second;

        if (field == PBH_HASH_HASH_FIELD_LIST)
        {
            if (!this->parsePbhHashHashFieldList(hash, field, value))
            {
                return false;
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown field(%s): skipping ...", field.c_str());
        }
    }

    return this->validatePbhHash(hash);
}

bool PbhHelper::parsePbhHashFieldHashField(PbhHashField &hashField, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    const auto &cit = pbhHashFieldHashFieldMap.find(value);
    if (cit == pbhHashFieldHashFieldMap.cend())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): invalid value(%s)", field.c_str(), value.c_str());
        return false;
    }

    hashField.hash_field.value = cit->second;
    hashField.hash_field.is_set = true;

    return true;
}

bool PbhHelper::parsePbhHashFieldIpMask(PbhHashField &hashField, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    try
    {
        hashField.ip_mask.value = IpAddress(value);
        hashField.ip_mask.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    return true;
}

bool PbhHelper::parsePbhHashFieldSequenceId(PbhHashField &hashField, const std::string &field, const std::string &value) const
{
    SWSS_LOG_ENTER();

    if (value.empty())
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): empty value is prohibited", field.c_str());
        return false;
    }

    try
    {
        hashField.sequence_id.value = to_uint<sai_uint32_t>(value);
        hashField.sequence_id.is_set = true;
    }
    catch (const std::exception &e)
    {
        SWSS_LOG_ERROR("Failed to parse field(%s): %s", field.c_str(), e.what());
        return false;
    }

    return true;
}

bool PbhHelper::parsePbhHashField(PbhHashField &hashField) const
{
    SWSS_LOG_ENTER();

    for (const auto &cit : hashField.fieldValueMap)
    {
        const auto &field = cit.first;
        const auto &value = cit.second;

        if (field == PBH_HASH_FIELD_HASH_FIELD)
        {
            if (!this->parsePbhHashFieldHashField(hashField, field, value))
            {
                return false;
            }
        }
        else if (field == PBH_HASH_FIELD_IP_MASK)
        {
            if (!this->parsePbhHashFieldIpMask(hashField, field, value))
            {
                return false;
            }
        }
        else if (field == PBH_HASH_FIELD_SEQUENCE_ID)
        {
            if (!this->parsePbhHashFieldSequenceId(hashField, field, value))
            {
                return false;
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown field(%s): skipping ...", field.c_str());
        }
    }

    return this->validatePbhHashField(hashField);
}

bool PbhHelper::validatePbhTable(PbhTable &table) const
{
    SWSS_LOG_ENTER();

    if (!table.interface_list.is_set)
    {
        SWSS_LOG_ERROR("Validation error: missing mandatory field(%s)", PBH_TABLE_INTERFACE_LIST);
        return false;
    }

    if (!table.description.is_set)
    {
        SWSS_LOG_ERROR("Validation error: missing mandatory field(%s)", PBH_TABLE_DESCRIPTION);
        return false;
    }

    return true;
}

bool PbhHelper::validatePbhRule(PbhRule &rule) const
{
    SWSS_LOG_ENTER();

    if (!rule.priority.is_set)
    {
        SWSS_LOG_ERROR("Validation error: missing mandatory field(%s)", PBH_RULE_PRIORITY);
        return false;
    }

    if (!rule.hash.is_set)
    {
        SWSS_LOG_ERROR("Validation error: missing mandatory field(%s)", PBH_RULE_HASH);
        return false;
    }

    if (!rule.packet_action.is_set)
    {
        SWSS_LOG_NOTICE(
            "Missing non mandatory field(%s): setting default value(%s)",
            PBH_RULE_PACKET_ACTION,
            PBH_RULE_PACKET_ACTION_SET_ECMP_HASH
        );

        rule.packet_action.meta.name = PBH_RULE_PACKET_ACTION;
        rule.packet_action.value = SAI_ACL_ENTRY_ATTR_ACTION_SET_ECMP_HASH_ID;
        rule.packet_action.is_set = true;

        rule.fieldValueMap[PBH_RULE_PACKET_ACTION] = PBH_RULE_PACKET_ACTION_SET_ECMP_HASH;
    }

    if (!rule.flow_counter.is_set)
    {
        SWSS_LOG_NOTICE(
            "Missing non mandatory field(%s): setting default value(%s)",
            PBH_RULE_FLOW_COUNTER,
            PBH_RULE_FLOW_COUNTER_DISABLED
        );

        rule.flow_counter.value = false;
        rule.flow_counter.is_set = true;

        rule.fieldValueMap[PBH_RULE_FLOW_COUNTER] = PBH_RULE_FLOW_COUNTER_DISABLED;
    }

    return true;
}

bool PbhHelper::validatePbhHash(PbhHash &hash) const
{
    SWSS_LOG_ENTER();

    if (!hash.hash_field_list.is_set)
    {
        SWSS_LOG_ERROR("Validation error: missing mandatory field(%s)", PBH_HASH_HASH_FIELD_LIST);
        return false;
    }

    return true;
}

bool PbhHelper::validatePbhHashField(PbhHashField &hashField) const
{
    SWSS_LOG_ENTER();

    if (!hashField.hash_field.is_set)
    {
        SWSS_LOG_ERROR("Validation error: missing mandatory field(%s)", PBH_HASH_FIELD_HASH_FIELD);
        return false;
    }

    if (hashField.ip_mask.is_set)
    {
        if (hashField.ip_mask.value.isV4())
        {
            if (!this->isIpv4MaskRequired(hashField.hash_field.value))
            {
                SWSS_LOG_ERROR("Validation error: field(%s) is prohibited", PBH_HASH_FIELD_IP_MASK);
                return false;
            }
        }
        else
        {
            if (!this->isIpv6MaskRequired(hashField.hash_field.value))
            {
                SWSS_LOG_ERROR("Validation error: field(%s) is prohibited", PBH_HASH_FIELD_IP_MASK);
                return false;
            }
        }
    }

    if (!hashField.sequence_id.is_set)
    {
        SWSS_LOG_ERROR("Validation error: missing mandatory field(%s)", PBH_HASH_FIELD_SEQUENCE_ID);
        return false;
    }

    return true;
}

bool PbhHelper::isIpv4MaskRequired(const sai_native_hash_field_t &value) const
{
    switch (value)
    {
        case SAI_NATIVE_HASH_FIELD_INNER_DST_IPV4:
        case SAI_NATIVE_HASH_FIELD_INNER_SRC_IPV4:
            return true;

        default:
            break;
    }

    return false;
}

bool PbhHelper::isIpv6MaskRequired(const sai_native_hash_field_t &value) const
{
    switch (value)
    {
        case SAI_NATIVE_HASH_FIELD_INNER_DST_IPV6:
        case SAI_NATIVE_HASH_FIELD_INNER_SRC_IPV6:
            return true;

        default:
            break;
    }

    return false;
}
