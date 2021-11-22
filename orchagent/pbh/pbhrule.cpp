#include "sai_serialize.h"

#include "pbhrule.h"

AclRulePbh::AclRulePbh(AclOrch *pAclOrch, string rule, string table, bool createCounter) :
    AclRule(pAclOrch, rule, table, ACL_TABLE_PBH, createCounter)
{
}

bool AclRulePbh::validateAddPriority(const sai_uint32_t &value)
{
    SWSS_LOG_ENTER();

    return setPriority(value);
}

bool AclRulePbh::validateAddMatch(const sai_attribute_t &attr)
{
    SWSS_LOG_ENTER();

    bool validate = false;

    auto attrId = static_cast<sai_acl_entry_attr_t>(attr.id);
    auto attrName = sai_serialize_enum(attrId, &sai_metadata_enum_sai_acl_entry_attr_t);

    switch (attrId)
    {
        case SAI_ACL_ENTRY_ATTR_FIELD_GRE_KEY:
        case SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE:
        case SAI_ACL_ENTRY_ATTR_FIELD_IP_PROTOCOL:
        case SAI_ACL_ENTRY_ATTR_FIELD_IPV6_NEXT_HEADER:
        case SAI_ACL_ENTRY_ATTR_FIELD_L4_DST_PORT:
        case SAI_ACL_ENTRY_ATTR_FIELD_INNER_ETHER_TYPE:
            validate = true;
            break;

        default:
            break;
    }

    if (!validate)
    {
        SWSS_LOG_ERROR("Failed to validate match field: invalid attribute %s", attrName.c_str());
        return false;
    }

    return setMatch(attrId, attr.value.aclfield);
}

bool AclRulePbh::validateAddAction(const sai_attribute_t &attr)
{
    SWSS_LOG_ENTER();

    bool validate = false;

    auto attrId = static_cast<sai_acl_entry_attr_t>(attr.id);
    auto attrName = sai_serialize_enum(attrId, &sai_metadata_enum_sai_acl_entry_attr_t);

    switch (attrId)
    {
        case SAI_ACL_ENTRY_ATTR_ACTION_SET_ECMP_HASH_ID:
        case SAI_ACL_ENTRY_ATTR_ACTION_SET_LAG_HASH_ID:
            validate = true;
            break;

        default:
            break;
    }

    if (!validate)
    {
        SWSS_LOG_ERROR("Failed to validate action field: invalid attribute %s", attrName.c_str());
        return false;
    }

    return setAction(attrId, attr.value.aclaction);
}

bool AclRulePbh::validate()
{
    SWSS_LOG_ENTER();

    if (m_matches.size() == 0 || m_actions.size() != 1)
    {
        SWSS_LOG_ERROR("Failed to validate rule: invalid parameters");
        return false;
    }

    return true;
}

void AclRulePbh::onUpdate(SubjectType, void *)
{
    // Do nothing
}

bool AclRulePbh::validateAddAction(string attr_name, string attr_value)
{
    SWSS_LOG_THROW("This API should not be used on PbhRule");
}
