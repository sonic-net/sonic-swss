// includes -----------------------------------------------------------------------------------------------------------

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>

#include "pbhschema.h"
#include "schema.h"
#include "logger.h"

#include "pbhcap.h"

using namespace swss;

// defines ------------------------------------------------------------------------------------------------------------

#define PBH_PLATFORM_ENV_VAR  "ASIC_VENDOR"
#define PBH_PLATFORM_GENERIC  "generic"
#define PBH_PLATFORM_MELLANOX "mellanox"
#define PBH_PLATFORM_UNKN     "unknown"

#define PBH_TABLE_CAPABILITIES_KEY      "table"
#define PBH_RULE_CAPABILITIES_KEY       "rule"
#define PBH_HASH_CAPABILITIES_KEY       "hash"
#define PBH_HASH_FIELD_CAPABILITIES_KEY "hash-field"

#define PBH_FIELD_CAPABILITY_ADD    "ADD"
#define PBH_FIELD_CAPABILITY_UPDATE "UPDATE"
#define PBH_FIELD_CAPABILITY_REMOVE "REMOVE"
#define PBH_FIELD_CAPABILITY_UNKN   "UNKNOWN"

#define PBH_STATE_DB_NAME    "STATE_DB"
#define PBH_STATE_DB_TIMEOUT 0

// constants ----------------------------------------------------------------------------------------------------------

static const std::map<PbhAsicVendor, std::string> pbhAsicVendorMap =
{
    { PbhAsicVendor::GENERIC,  PBH_PLATFORM_GENERIC },
    { PbhAsicVendor::MELLANOX, PBH_PLATFORM_MELLANOX }
};

static const std::map<PbhFieldCapability, std::string> pbhFieldCapabilityMap =
{
    { PbhFieldCapability::ADD,    PBH_FIELD_CAPABILITY_ADD },
    { PbhFieldCapability::UPDATE, PBH_FIELD_CAPABILITY_UPDATE },
    { PbhFieldCapability::REMOVE, PBH_FIELD_CAPABILITY_REMOVE }
};

// functions ----------------------------------------------------------------------------------------------------------

static std::string toStr(PbhAsicVendor value) noexcept
{
    const auto &cit = pbhAsicVendorMap.find(value);
    if (cit != pbhAsicVendorMap.cend())
    {
        return cit->second;
    }

    return PBH_PLATFORM_UNKN;
}

static std::string toStr(PbhFieldCapability value) noexcept
{
    const auto &cit = pbhFieldCapabilityMap.find(value);
    if (cit != pbhFieldCapabilityMap.cend())
    {
        return cit->second;
    }

    return PBH_FIELD_CAPABILITY_UNKN;
}

static std::string toStr(const std::set<PbhFieldCapability> &value) noexcept
{
    std::stringstream ss;
    bool separator = false;

    for (const auto &cit : value)
    {
        if (!separator)
        {
            ss << toStr(cit);
            separator = true;
        }
        else
        {
            ss << "," << toStr(cit);
        }
    }

    return ss.str();
}

// PBH field capabilities ---------------------------------------------------------------------------------------------

void PbhVendorFieldCapabilities::setPbhDefaults(std::set<PbhFieldCapability> &fieldCap) noexcept
{
    fieldCap.insert(PbhFieldCapability::ADD);
    fieldCap.insert(PbhFieldCapability::UPDATE);
    fieldCap.insert(PbhFieldCapability::REMOVE);
}

PbhGenericFieldCapabilities::PbhGenericFieldCapabilities() noexcept
{
    this->table.interface_list.insert(PbhFieldCapability::UPDATE);
    this->table.description.insert(PbhFieldCapability::UPDATE);

    this->rule.priority.insert(PbhFieldCapability::UPDATE);
    this->setPbhDefaults(this->rule.gre_key);
    this->setPbhDefaults(this->rule.ether_type);
    this->setPbhDefaults(this->rule.ip_protocol);
    this->setPbhDefaults(this->rule.ipv6_next_header);
    this->setPbhDefaults(this->rule.l4_dst_port);
    this->setPbhDefaults(this->rule.inner_ether_type);
    this->rule.hash.insert(PbhFieldCapability::UPDATE);
    this->setPbhDefaults(this->rule.packet_action);
    this->setPbhDefaults(this->rule.flow_counter);

    this->hash.hash_field_list.insert(PbhFieldCapability::UPDATE);
}

PbhMellanoxFieldCapabilities::PbhMellanoxFieldCapabilities() noexcept
{
    this->table.interface_list.insert(PbhFieldCapability::UPDATE);
    this->table.description.insert(PbhFieldCapability::UPDATE);

    this->rule.priority.insert(PbhFieldCapability::UPDATE);
    this->setPbhDefaults(this->rule.gre_key);
    this->setPbhDefaults(this->rule.ether_type);
    this->setPbhDefaults(this->rule.ip_protocol);
    this->setPbhDefaults(this->rule.ipv6_next_header);
    this->setPbhDefaults(this->rule.l4_dst_port);
    this->setPbhDefaults(this->rule.inner_ether_type);
    this->rule.hash.insert(PbhFieldCapability::UPDATE);
    this->setPbhDefaults(this->rule.packet_action);
    this->setPbhDefaults(this->rule.flow_counter);
}

// PBH entity capabilities --------------------------------------------------------------------------------------------

PbhEntityCapabilities::PbhEntityCapabilities(const std::shared_ptr<PbhVendorFieldCapabilities> &fieldCap) noexcept :
    fieldCap(fieldCap)
{

}

bool PbhEntityCapabilities::validate(const std::set<PbhFieldCapability> &fieldCap, PbhFieldCapability value) const
{
    const auto &cit = fieldCap.find(value);
    if (cit == fieldCap.cend())
    {
        return false;
    }

    return true;
}

PbhTableCapabilities::PbhTableCapabilities(const std::shared_ptr<PbhVendorFieldCapabilities> &fieldCap) noexcept :
    PbhEntityCapabilities(fieldCap)
{

}

bool PbhTableCapabilities::validatePbhInterfaceList(PbhFieldCapability value) const
{
    return this->validate(this->getPbhCap().interface_list, value);
}

bool PbhTableCapabilities::validatePbhDescription(PbhFieldCapability value) const
{
    return  this->validate(this->getPbhCap().description, value);
}

auto PbhTableCapabilities::getPbhCap() const -> const decltype(PbhVendorFieldCapabilities::table) &
{
    return this->fieldCap->table;
}

PbhRuleCapabilities::PbhRuleCapabilities(const std::shared_ptr<PbhVendorFieldCapabilities> &fieldCap) noexcept :
    PbhEntityCapabilities(fieldCap)
{

}

bool PbhRuleCapabilities::validatePbhPriority(PbhFieldCapability value) const
{
    return this->validate(this->getPbhCap().priority, value);
}

bool PbhRuleCapabilities::validatePbhGreKey(PbhFieldCapability value) const
{
    return this->validate(this->getPbhCap().gre_key, value);
}

bool PbhRuleCapabilities::validatePbhEtherType(PbhFieldCapability value) const
{
    return this->validate(this->getPbhCap().ether_type, value);
}

bool PbhRuleCapabilities::validatePbhIpProtocol(PbhFieldCapability value) const
{
    return this->validate(this->getPbhCap().ip_protocol, value);
}

bool PbhRuleCapabilities::validatePbhIpv6NextHeader(PbhFieldCapability value) const
{
    return this->validate(this->getPbhCap().ipv6_next_header, value);
}

bool PbhRuleCapabilities::validatePbhL4DstPort(PbhFieldCapability value) const
{
    return this->validate(this->getPbhCap().l4_dst_port, value);
}

bool PbhRuleCapabilities::validatePbhInnerEtherType(PbhFieldCapability value) const
{
    return this->validate(this->getPbhCap().inner_ether_type, value);
}

bool PbhRuleCapabilities::validatePbhHash(PbhFieldCapability value) const
{
    return this->validate(this->getPbhCap().hash, value);
}

bool PbhRuleCapabilities::validatePbhPacketAction(PbhFieldCapability value) const
{
    return this->validate(this->getPbhCap().packet_action, value);
}

bool PbhRuleCapabilities::validatePbhFlowCounter(PbhFieldCapability value) const
{
    return this->validate(this->getPbhCap().flow_counter, value);
}

auto PbhRuleCapabilities::getPbhCap() const -> const decltype(PbhVendorFieldCapabilities::rule) &
{
    return this->fieldCap->rule;
}

PbhHashCapabilities::PbhHashCapabilities(const std::shared_ptr<PbhVendorFieldCapabilities> &fieldCap) noexcept :
    PbhEntityCapabilities(fieldCap)
{

}

bool PbhHashCapabilities::validatePbhHashFieldList(PbhFieldCapability value) const
{
    return this->validate(this->getPbhCap().hash_field_list, value);
}

auto PbhHashCapabilities::getPbhCap() const -> const decltype(PbhVendorFieldCapabilities::hash) &
{
    return this->fieldCap->hash;
}

PbhHashFieldCapabilities::PbhHashFieldCapabilities(const std::shared_ptr<PbhVendorFieldCapabilities> &fieldCap) noexcept :
    PbhEntityCapabilities(fieldCap)
{

}

bool PbhHashFieldCapabilities::validatePbhHashField(PbhFieldCapability value) const
{
    return this->validate(this->getPbhCap().hash_field, value);
}

bool PbhHashFieldCapabilities::validatePbhIpMask(PbhFieldCapability value) const
{
    return this->validate(this->getPbhCap().ip_mask, value);
}

bool PbhHashFieldCapabilities::validatePbhSequenceId(PbhFieldCapability value) const
{
    return this->validate(this->getPbhCap().sequence_id, value);
}

auto PbhHashFieldCapabilities::getPbhCap() const -> const decltype(PbhVendorFieldCapabilities::hashField) &
{
    return this->fieldCap->hashField;
}

// PBH capabilities ---------------------------------------------------------------------------------------------------

DBConnector PbhCapabilities::stateDb(PBH_STATE_DB_NAME, PBH_STATE_DB_TIMEOUT);
Table PbhCapabilities::capTable(&stateDb, STATE_PBH_CAPABILITIES_TABLE_NAME);

PbhCapabilities::PbhCapabilities() noexcept
{
    SWSS_LOG_ENTER();

    if (!this->parsePbhAsicVendor())
    {
        SWSS_LOG_WARN("Failed to parse ASIC vendor: fallback to %s platform", PBH_PLATFORM_GENERIC);
        this->asicVendor = PbhAsicVendor::GENERIC;
    }

    this->initPbhVendorCapabilities();
    this->writePbhVendorCapabilitiesToDb();
}

PbhAsicVendor PbhCapabilities::getAsicVendor() const noexcept
{
    return this->asicVendor;
}

bool PbhCapabilities::parsePbhAsicVendor()
{
    SWSS_LOG_ENTER();

    const auto *envVar = std::getenv(PBH_PLATFORM_ENV_VAR);
    if (envVar == nullptr)
    {
        SWSS_LOG_WARN("Failed to detect ASIC vendor: environmental variable(%s) is not found", PBH_PLATFORM_ENV_VAR);
        return false;
    }

    std::string platform(envVar);

    if (platform == PBH_PLATFORM_MELLANOX)
    {
        this->asicVendor = PbhAsicVendor::MELLANOX;
    }
    else
    {
        this->asicVendor = PbhAsicVendor::GENERIC;
    }

    SWSS_LOG_NOTICE("Parsed PBH ASIC vendor: %s", toStr(this->asicVendor).c_str());

    return true;
}

void PbhCapabilities::initPbhVendorCapabilities()
{
    std::shared_ptr<PbhVendorFieldCapabilities> fieldCap;

    switch (this->asicVendor)
    {
        case PbhAsicVendor::GENERIC:
            fieldCap = std::make_shared<PbhGenericFieldCapabilities>();
            break;

        case PbhAsicVendor::MELLANOX:
            fieldCap = std::make_shared<PbhMellanoxFieldCapabilities>();
            break;

        default:
            SWSS_LOG_WARN("Unknown ASIC vendor: skipping ...");
            break;
    }

    if (!fieldCap)
    {
        SWSS_LOG_ERROR("Failed to initialize PBH capabilities: unknown ASIC vendor");
        return;
    }

    this->table = std::make_shared<PbhTableCapabilities>(fieldCap);
    this->rule = std::make_shared<PbhRuleCapabilities>(fieldCap);
    this->hash = std::make_shared<PbhHashCapabilities>(fieldCap);
    this->hashField = std::make_shared<PbhHashFieldCapabilities>(fieldCap);

    SWSS_LOG_NOTICE("Initialized PBH capabilities: %s platform", toStr(this->asicVendor).c_str());
}

template<>
void PbhCapabilities::writePbhEntityCapabilitiesToDb(const std::shared_ptr<PbhTableCapabilities> &entityCap)
{
    SWSS_LOG_ENTER();

    auto key = PbhCapabilities::capTable.getKeyName(PBH_TABLE_CAPABILITIES_KEY);
    std::vector<FieldValueTuple> fvList;

    fvList.push_back(FieldValueTuple(PBH_TABLE_INTERFACE_LIST, toStr(entityCap->getPbhCap().interface_list)));
    fvList.push_back(FieldValueTuple(PBH_TABLE_DESCRIPTION, toStr(entityCap->getPbhCap().description)));

    PbhCapabilities::capTable.set(PBH_TABLE_CAPABILITIES_KEY, fvList);

    SWSS_LOG_NOTICE("Wrote PBH table capabilities to State DB: %s key", key.c_str());
}

template<>
void PbhCapabilities::writePbhEntityCapabilitiesToDb(const std::shared_ptr<PbhRuleCapabilities> &entityCap)
{
    SWSS_LOG_ENTER();

    auto key = PbhCapabilities::capTable.getKeyName(PBH_RULE_CAPABILITIES_KEY);
    std::vector<FieldValueTuple> fvList;

    fvList.push_back(FieldValueTuple(PBH_RULE_PRIORITY, toStr(entityCap->getPbhCap().priority)));
    fvList.push_back(FieldValueTuple(PBH_RULE_GRE_KEY, toStr(entityCap->getPbhCap().gre_key)));
    fvList.push_back(FieldValueTuple(PBH_RULE_ETHER_TYPE, toStr(entityCap->getPbhCap().ether_type)));
    fvList.push_back(FieldValueTuple(PBH_RULE_IP_PROTOCOL, toStr(entityCap->getPbhCap().ip_protocol)));
    fvList.push_back(FieldValueTuple(PBH_RULE_IPV6_NEXT_HEADER, toStr(entityCap->getPbhCap().ipv6_next_header)));
    fvList.push_back(FieldValueTuple(PBH_RULE_L4_DST_PORT, toStr(entityCap->getPbhCap().l4_dst_port)));
    fvList.push_back(FieldValueTuple(PBH_RULE_INNER_ETHER_TYPE, toStr(entityCap->getPbhCap().inner_ether_type)));
    fvList.push_back(FieldValueTuple(PBH_RULE_HASH, toStr(entityCap->getPbhCap().hash)));
    fvList.push_back(FieldValueTuple(PBH_RULE_PACKET_ACTION, toStr(entityCap->getPbhCap().packet_action)));
    fvList.push_back(FieldValueTuple(PBH_RULE_FLOW_COUNTER, toStr(entityCap->getPbhCap().flow_counter)));

    PbhCapabilities::capTable.set(PBH_RULE_CAPABILITIES_KEY, fvList);

    SWSS_LOG_NOTICE("Wrote PBH rule capabilities to State DB: %s key", key.c_str());
}

template<>
void PbhCapabilities::writePbhEntityCapabilitiesToDb(const std::shared_ptr<PbhHashCapabilities> &entityCap)
{
    SWSS_LOG_ENTER();

    auto key = PbhCapabilities::capTable.getKeyName(PBH_HASH_CAPABILITIES_KEY);
    std::vector<FieldValueTuple> fvList;

    fvList.push_back(FieldValueTuple(PBH_HASH_HASH_FIELD_LIST, toStr(entityCap->getPbhCap().hash_field_list)));

    PbhCapabilities::capTable.set(PBH_HASH_CAPABILITIES_KEY, fvList);

    SWSS_LOG_NOTICE("Wrote PBH hash capabilities to State DB: %s key", key.c_str());
}

template<>
void PbhCapabilities::writePbhEntityCapabilitiesToDb(const std::shared_ptr<PbhHashFieldCapabilities> &entityCap)
{
    SWSS_LOG_ENTER();

    auto key = PbhCapabilities::capTable.getKeyName(PBH_HASH_FIELD_CAPABILITIES_KEY);
    std::vector<FieldValueTuple> fvList;

    fvList.push_back(FieldValueTuple(PBH_HASH_FIELD_HASH_FIELD, toStr(entityCap->getPbhCap().hash_field)));
    fvList.push_back(FieldValueTuple(PBH_HASH_FIELD_IP_MASK, toStr(entityCap->getPbhCap().ip_mask)));
    fvList.push_back(FieldValueTuple(PBH_HASH_FIELD_SEQUENCE_ID, toStr(entityCap->getPbhCap().sequence_id)));

    PbhCapabilities::capTable.set(PBH_HASH_FIELD_CAPABILITIES_KEY, fvList);

    SWSS_LOG_NOTICE("Wrote PBH hash field capabilities to State DB: %s key", key.c_str());
}

void PbhCapabilities::writePbhVendorCapabilitiesToDb()
{
    SWSS_LOG_ENTER();

    this->writePbhEntityCapabilitiesToDb(this->table);
    this->writePbhEntityCapabilitiesToDb(this->rule);
    this->writePbhEntityCapabilitiesToDb(this->hash);
    this->writePbhEntityCapabilitiesToDb(this->hashField);

    SWSS_LOG_NOTICE("Wrote PBH capabilities to State DB: %s table", STATE_PBH_CAPABILITIES_TABLE_NAME);
}

bool PbhCapabilities::validatePbhTableCap(const std::vector<std::string> &fieldList, PbhFieldCapability value) const
{
    SWSS_LOG_ENTER();

    for (const auto &cit : fieldList)
    {
        if (cit == PBH_TABLE_INTERFACE_LIST)
        {
            if (!this->table->validatePbhInterfaceList(value))
            {
                SWSS_LOG_ERROR("Failed to validate field(%s): capability(%s) is not supported",
                    cit.c_str(),
                    toStr(value).c_str()
                );
                return false;
            }
        }
        else if (cit == PBH_TABLE_DESCRIPTION)
        {
            if (!this->table->validatePbhDescription(value))
            {
                SWSS_LOG_ERROR("Failed to validate field(%s): capability(%s) is not supported",
                    cit.c_str(),
                    toStr(value).c_str()
                );
                return false;
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown field(%s): skipping ...", cit.c_str());
        }
    }

    return true;
}

bool PbhCapabilities::validatePbhRuleCap(const std::vector<std::string> &fieldList, PbhFieldCapability value) const
{
    SWSS_LOG_ENTER();

    for (const auto &cit : fieldList)
    {
        if (cit == PBH_RULE_PRIORITY)
        {
            if (!this->rule->validatePbhPriority(value))
            {
                SWSS_LOG_ERROR("Failed to validate field(%s): capability(%s) is not supported",
                    cit.c_str(),
                    toStr(value).c_str()
                );
                return false;
            }
        }
        else if (cit == PBH_RULE_GRE_KEY)
        {
            if (!this->rule->validatePbhGreKey(value))
            {
                SWSS_LOG_ERROR("Failed to validate field(%s): capability(%s) is not supported",
                    cit.c_str(),
                    toStr(value).c_str()
                );
                return false;
            }
        }
        else if (cit == PBH_RULE_ETHER_TYPE)
        {
            if (!this->rule->validatePbhEtherType(value))
            {
                SWSS_LOG_ERROR("Failed to validate field(%s): capability(%s) is not supported",
                    cit.c_str(),
                    toStr(value).c_str()
                );
                return false;
            }
        }
        else if (cit == PBH_RULE_IP_PROTOCOL)
        {
            if (!this->rule->validatePbhIpProtocol(value))
            {
                SWSS_LOG_ERROR("Failed to validate field(%s): capability(%s) is not supported",
                    cit.c_str(),
                    toStr(value).c_str()
                );
                return false;
            }
        }
        else if (cit == PBH_RULE_IPV6_NEXT_HEADER)
        {
            if (!this->rule->validatePbhIpv6NextHeader(value))
            {
                SWSS_LOG_ERROR("Failed to validate field(%s): capability(%s) is not supported",
                    cit.c_str(),
                    toStr(value).c_str()
                );
                return false;
            }
        }
        else if (cit == PBH_RULE_L4_DST_PORT)
        {
            if (!this->rule->validatePbhL4DstPort(value))
            {
                SWSS_LOG_ERROR("Failed to validate field(%s): capability(%s) is not supported",
                    cit.c_str(),
                    toStr(value).c_str()
                );
                return false;
            }
        }
        else if (cit == PBH_RULE_INNER_ETHER_TYPE)
        {
            if (!this->rule->validatePbhInnerEtherType(value))
            {
                SWSS_LOG_ERROR("Failed to validate field(%s): capability(%s) is not supported",
                    cit.c_str(),
                    toStr(value).c_str()
                );
                return false;
            }
        }
        else if (cit == PBH_RULE_HASH)
        {
            if (!this->rule->validatePbhHash(value))
            {
                SWSS_LOG_ERROR("Failed to validate field(%s): capability(%s) is not supported",
                    cit.c_str(),
                    toStr(value).c_str()
                );
                return false;
            }
        }
        else if (cit == PBH_RULE_PACKET_ACTION)
        {
            if (!this->rule->validatePbhPacketAction(value))
            {
                SWSS_LOG_ERROR("Failed to validate field(%s): capability(%s) is not supported",
                    cit.c_str(),
                    toStr(value).c_str()
                );
                return false;
            }
        }
        else if (cit == PBH_RULE_FLOW_COUNTER)
        {
            if (!this->rule->validatePbhFlowCounter(value))
            {
                SWSS_LOG_ERROR("Failed to validate field(%s): capability(%s) is not supported",
                    cit.c_str(),
                    toStr(value).c_str()
                );
                return false;
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown field(%s): skipping ...", cit.c_str());
        }
    }

    return true;
}

bool PbhCapabilities::validatePbhHashCap(const std::vector<std::string> &fieldList, PbhFieldCapability value) const
{
    SWSS_LOG_ENTER();

    for (const auto &cit : fieldList)
    {
        if (cit == PBH_HASH_HASH_FIELD_LIST)
        {
            if (!this->hash->validatePbhHashFieldList(value))
            {
                SWSS_LOG_ERROR("Failed to validate field(%s): capability(%s) is not supported",
                    cit.c_str(),
                    toStr(value).c_str()
                );
                return false;
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown field(%s): skipping ...", cit.c_str());
        }
    }

    return true;
}

bool PbhCapabilities::validatePbhHashFieldCap(const std::vector<std::string> &fieldList, PbhFieldCapability value) const
{
    SWSS_LOG_ENTER();

    for (const auto &cit : fieldList)
    {
        if (cit == PBH_HASH_FIELD_HASH_FIELD)
        {
            if (!this->hashField->validatePbhHashField(value))
            {
                SWSS_LOG_ERROR("Failed to validate field(%s): capability(%s) is not supported",
                    cit.c_str(),
                    toStr(value).c_str()
                );
                return false;
            }
        }
        else if (cit == PBH_HASH_FIELD_IP_MASK)
        {
            if (!this->hashField->validatePbhIpMask(value))
            {
                SWSS_LOG_ERROR("Failed to validate field(%s): capability(%s) is not supported",
                    cit.c_str(),
                    toStr(value).c_str()
                );
                return false;
            }
        }
        else if (cit == PBH_HASH_FIELD_SEQUENCE_ID)
        {
            if (!this->hashField->validatePbhSequenceId(value))
            {
                SWSS_LOG_ERROR("Failed to validate field(%s): capability(%s) is not supported",
                    cit.c_str(),
                    toStr(value).c_str()
                );
                return false;
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown field(%s): skipping ...", cit.c_str());
        }
    }

    return true;
}
