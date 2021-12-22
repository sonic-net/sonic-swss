#include "p4orch/acl_util.h"

#include "converter.h"
#include "json.hpp"
#include "logger.h"
#include "sai_serialize.h"
#include "table.h"
#include "tokenize.h"

namespace p4orch
{

std::string trim(const std::string &s)
{
    size_t end = s.find_last_not_of(WHITESPACE);
    size_t start = s.find_first_not_of(WHITESPACE);
    return (end == std::string::npos) ? EMPTY_STRING : s.substr(start, end - start + 1);
}

bool parseAclTableAppDbActionField(const std::string &aggr_actions_str, std::vector<P4ActionParamName> *action_list,
                                   std::vector<P4PacketActionWithColor> *action_color_list)
{
    try
    {
        const auto &j = nlohmann::json::parse(aggr_actions_str);
        if (!j.is_array())
        {
            SWSS_LOG_ERROR("Invalid ACL table definition action %s, expecting an array.\n", aggr_actions_str.c_str());
            return false;
        }
        P4ActionParamName action_with_param;
        for (auto &action_item : j)
        {
            auto sai_action_it = action_item.find(kAction);
            if (sai_action_it == action_item.end())
            {
                SWSS_LOG_ERROR("Invalid ACL table definition action %s, missing 'action':\n", aggr_actions_str.c_str());
                return false;
            }
            if (aclPacketActionLookup.find(sai_action_it.value()) == aclPacketActionLookup.end())
            {
                action_with_param.sai_action = sai_action_it.value();
                auto action_param_it = action_item.find(kActionParamPrefix);
                if (action_param_it != action_item.end() && !action_param_it.value().is_null())
                {
                    action_with_param.p4_param_name = action_param_it.value();
                }
                action_list->push_back(action_with_param);
            }
            else
            {
                auto packet_color_it = action_item.find(kPacketColor);
                P4PacketActionWithColor packet_action_with_color;
                packet_action_with_color.packet_action = sai_action_it.value();
                if (packet_color_it != action_item.end() && !packet_color_it.value().is_null())
                {
                    packet_action_with_color.packet_color = packet_color_it.value();
                }
                action_color_list->push_back(packet_action_with_color);
            }
        }
        return true;
    }
    catch (std::exception &ex)
    {
        SWSS_LOG_ERROR("Failed to deserialize ACL table definition action fields: %s (%s)", aggr_actions_str.c_str(),
                       ex.what());
        return false;
    }
}

ReturnCode validateAndSetSaiMatchFieldJson(const nlohmann::json &match_json, const std::string &p4_match,
                                           const std::string &aggr_match_str,
                                           std::map<std::string, SaiMatchField> *sai_match_field_lookup,
                                           std::map<std::string, std::string> *ip_type_bit_type_lookup)
{
    SaiMatchField sai_match_field;
    auto format_str_it = match_json.find(kAclMatchFieldFormat);
    if (format_str_it == match_json.end() || format_str_it.value().is_null() || !format_str_it.value().is_string())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match field {" << p4_match << ": " << aggr_match_str
               << "} is an invalid ACL table attribute: " << kAclMatchFieldFormat
               << " value is required and should be a string";
    }
    auto format_it = formatLookup.find(format_str_it.value());
    if (format_it == formatLookup.end())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match field {" << p4_match << ": " << aggr_match_str
               << "} is an invalid ACL table attribute: " << kAclMatchFieldFormat
               << " value is invalid, should be one of {" << P4_FORMAT_HEX_STRING << ", " << P4_FORMAT_MAC << ", "
               << P4_FORMAT_IPV4 << ", " << P4_FORMAT_IPV6 << ", " << P4_FORMAT_STRING << "}";
    }
    sai_match_field.format = format_it->second;
    if (sai_match_field.format != Format::STRING)
    {
        // bitwidth is required if the format is not "STRING"
        auto bitwidth_it = match_json.find(kAclMatchFieldBitwidth);
        if (bitwidth_it == match_json.end() || bitwidth_it.value().is_null() || !bitwidth_it.value().is_number())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "ACL table match field {" << p4_match << ": " << aggr_match_str
                   << "} is an invalid ACL table attribute: " << kAclMatchFieldBitwidth
                   << " value is required and should be a number";
        }
        sai_match_field.bitwidth = bitwidth_it.value();
    }

    auto match_field_it = match_json.find(kAclMatchFieldSaiField);
    if (match_field_it == match_json.end() || match_field_it.value().is_null() || !match_field_it.value().is_string())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match field {" << p4_match << ": " << aggr_match_str
               << "} is an invalid ACL table attribute: " << kAclMatchFieldSaiField
               << " value is required and should be a string";
    }

    std::vector<std::string> tokenized_field = swss::tokenize(match_field_it.value(), kFieldDelimiter);
    const auto &sai_match_field_str = tokenized_field[0];
    auto table_attr_it = aclMatchTableAttrLookup.find(sai_match_field_str);
    auto rule_attr_it = aclMatchEntryAttrLookup.find(sai_match_field_str);
    if (table_attr_it == aclMatchTableAttrLookup.end() || rule_attr_it == aclMatchEntryAttrLookup.end())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match field {" << p4_match << ": " << aggr_match_str
               << "} is an invalid ACL table attribute: " << match_field_it.value() << " is not supported in P4Orch ";
    }
    const auto &expected_format_it = aclMatchTableAttrFormatLookup.find(table_attr_it->second);
    if (expected_format_it == aclMatchTableAttrFormatLookup.end() ||
        sai_match_field.format != expected_format_it->second)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match field {" << p4_match << ": " << aggr_match_str
               << "} is an invalid ACL table attribute: format for field " << match_field_it.value()
               << " is expected to be " << expected_format_it->second << ", but got " << format_it->first;
    }
    sai_match_field.table_attr = table_attr_it->second;
    sai_match_field.entry_attr = rule_attr_it->second;
    (*sai_match_field_lookup)[p4_match] = sai_match_field;

    if (rule_attr_it->second == SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE && tokenized_field.size() == 2)
    {
        // Get IP_TYPE suffix and save the bit mapping.
        if (aclIpTypeBitSet.find(tokenized_field[1]) == aclIpTypeBitSet.end())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "ACL table match field {" << p4_match << ": " << aggr_match_str
                   << "} has invalid IP_TYPE encode bit.";
        }
        (*ip_type_bit_type_lookup)[p4_match] = tokenized_field[1];
    }
    SWSS_LOG_INFO("ACL table built match field %s with kind:sai_field", sai_match_field_str.c_str());

    return ReturnCode();
}

ReturnCode validateAndSetCompositeElementSaiFieldJson(
    const nlohmann::json &element_match_json, const std::string &p4_match,
    std::map<std::string, std::vector<SaiMatchField>> *composite_sai_match_fields_lookup, const std::string &format_str)
{
    SaiMatchField sai_match_field;
    const auto &element_str = element_match_json.dump();
    if (format_str != P4_FORMAT_IPV6)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match field " << p4_match << " element: " << element_str
               << " is an invalid ACL table attribute: '" << kAclMatchFieldFormat << "' should be " << P4_FORMAT_IPV6;
    }
    sai_match_field.format = Format::IPV6;

    auto bitwidth_it = element_match_json.find(kAclMatchFieldBitwidth);
    if (bitwidth_it == element_match_json.end() || bitwidth_it.value().is_null() || !bitwidth_it.value().is_number())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match field " << p4_match << "element: " << element_str
               << " is an invalid ACL table attribute: " << kAclMatchFieldBitwidth
               << " value is required and should be a number";
    }
    sai_match_field.bitwidth = bitwidth_it.value();

    auto match_field_it = element_match_json.find(kAclMatchFieldSaiField);
    if (match_field_it == element_match_json.end() || match_field_it.value().is_null() ||
        !match_field_it.value().is_string())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match field " << p4_match << " element: " << element_str
               << " is an invalid ACL table attribute: " << kAclMatchFieldSaiField
               << " value is required in composite elements and should be a string";
    }
    const std::string &match_field_str = match_field_it.value();
    auto table_attr_it = aclCompositeMatchTableAttrLookup.find(match_field_str);
    auto rule_attr_it = aclCompositeMatchEntryAttrLookup.find(match_field_str);
    if (table_attr_it == aclCompositeMatchTableAttrLookup.end() ||
        rule_attr_it == aclCompositeMatchEntryAttrLookup.end())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match field " << p4_match << " element: " << element_str
               << " is an invalid ACL table attribute: not supported in P4Orch "
                  "as an element in composite match fields";
    }
    const uint32_t expected_bitwidth = BYTE_BITWIDTH * IPV6_SINGLE_WORD_BYTES_LENGTH;
    if (sai_match_field.bitwidth != expected_bitwidth)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match field " << p4_match << " element: " << element_str
               << " is an invalid ACL table attribute: element.bitwidth is "
                  "expected to be "
               << expected_bitwidth << " but got " << sai_match_field.bitwidth;
    }
    sai_match_field.table_attr = table_attr_it->second;
    sai_match_field.entry_attr = rule_attr_it->second;
    (*composite_sai_match_fields_lookup)[p4_match].push_back(sai_match_field);
    SWSS_LOG_INFO("ACL table built composite match field element %s with kind:sai_field", match_field_str.c_str());
    return ReturnCode();
}

ReturnCode validateAndSetUdfFieldJson(const nlohmann::json &match_json, const std::string &p4_match,
                                      const std::string &aggr_match_str, const std::string &acl_table_name,
                                      std::map<std::string, std::vector<P4UdfField>> *udf_fields_lookup,
                                      std::map<std::string, uint16_t> *udf_group_attr_index_lookup)
{
    P4UdfField udf_field;
    // Parse UDF bitwitdth
    auto bitwidth_json_it = match_json.find(kAclMatchFieldBitwidth);
    if (bitwidth_json_it == match_json.end() || bitwidth_json_it.value().is_null() ||
        !bitwidth_json_it.value().is_number())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match composite UDF field {" << p4_match << ": " << aggr_match_str
               << "} is an invalid ACL table attribute: " << kAclMatchFieldBitwidth
               << " value is required and should be a number";
    }
    uint32_t bitwidth = bitwidth_json_it.value();
    if (bitwidth % BYTE_BITWIDTH != 0)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match composite UDF field {" << p4_match << ": " << aggr_match_str
               << "} is an invalid ACL table attribute: " << kAclMatchFieldBitwidth
               << " value should be a multiple of 8.";
    }
    udf_field.length = (uint16_t)(bitwidth / BYTE_BITWIDTH);

    // Parse UDF offset
    auto udf_offset_it = match_json.find(kAclUdfOffset);
    if (udf_offset_it == match_json.end() || udf_offset_it.value().is_null() || !udf_offset_it.value().is_number())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match composite UDF field {" << p4_match << ": " << aggr_match_str
               << "} is an invalid ACL table attribute: " << kAclUdfOffset
               << " value is required in composite elements and should be a number";
    }
    udf_field.offset = udf_offset_it.value();

    // Parse UDF base
    auto udf_base_json_it = match_json.find(kAclUdfBase);
    if (udf_base_json_it == match_json.end() || udf_base_json_it.value().is_null() ||
        !udf_base_json_it.value().is_string())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match composite UDF field {" << p4_match << ": " << aggr_match_str
               << "} is an invalid ACL table attribute: " << kAclUdfBase
               << " value is required in composite elements and should be a string";
    }
    const auto &udf_base_it = udfBaseLookup.find(udf_base_json_it.value());
    if (udf_base_it == udfBaseLookup.end())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match composite UDF field {" << p4_match << ": " << aggr_match_str
               << "} is an invalid ACL table attribute: " << udf_base_json_it.value()
               << " is not supported in P4Orch "
                  "as a valid UDF base. Supported UDF bases are: "
               << P4_UDF_BASE_L2 << ", " << P4_UDF_BASE_L3 << " and " << P4_UDF_BASE_L4;
    }
    udf_field.base = udf_base_it->second;
    // Set UDF group id
    udf_field.group_id = acl_table_name + "-" + p4_match + "-" + std::to_string((*udf_fields_lookup)[p4_match].size());
    udf_field.udf_id =
        udf_field.group_id + "-base" + std::to_string(udf_field.base) + "-offset" + std::to_string(udf_field.offset);
    (*udf_fields_lookup)[p4_match].push_back(udf_field);
    // Assign UDF group to a new ACL entry attr index if it is a new group
    uint16_t index = 0;
    auto udf_group_attr_index_it = udf_group_attr_index_lookup->find(udf_field.group_id);
    if (udf_group_attr_index_it != udf_group_attr_index_lookup->end())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Error when building UDF field for ACL talbe: duplicated UDF "
                  "groups found for the same index.";
    }
    index = (uint16_t)udf_group_attr_index_lookup->size();
    (*udf_group_attr_index_lookup)[udf_field.group_id] = index;
    SWSS_LOG_INFO("ACL table built composite match field elelment %s with kind:udf", udf_field.group_id.c_str());
    return ReturnCode();
}

ReturnCode validateAndSetCompositeMatchFieldJson(
    const nlohmann::json &aggr_match_json, const std::string &p4_match, const std::string &aggr_match_str,
    const std::string &acl_table_name,
    std::map<std::string, std::vector<SaiMatchField>> *composite_sai_match_fields_lookup,
    std::map<std::string, std::vector<P4UdfField>> *udf_fields_lookup,
    std::map<std::string, uint16_t> *udf_group_attr_index_lookups)
{
    auto format_str_it = aggr_match_json.find(kAclMatchFieldFormat);
    if (format_str_it == aggr_match_json.end() || format_str_it.value().is_null() || !format_str_it.value().is_string())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match field {" << p4_match << ": " << aggr_match_str
               << "} is an invalid ACL table attribute: " << kAclMatchFieldFormat
               << " value is required and should be a string";
    }
    auto format_it = formatLookup.find(format_str_it.value());
    if (format_it == formatLookup.end() || format_it->second == Format::STRING)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match field {" << p4_match << ": " << aggr_match_str
               << "} is an invalid ACL table attribute: " << kAclMatchFieldFormat
               << " value is invalid, should be one of {" << P4_FORMAT_HEX_STRING << ", " << P4_FORMAT_IPV6 << "}";
    }
    auto bitwidth_it = aggr_match_json.find(kAclMatchFieldBitwidth);
    if (bitwidth_it == aggr_match_json.end() || bitwidth_it.value().is_null() || !bitwidth_it.value().is_number())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match field {" << p4_match << ": " << aggr_match_str
               << "} is an invalid ACL table attribute: " << kAclMatchFieldBitwidth
               << " value is required and should be a number";
    }
    uint32_t composite_bitwidth = bitwidth_it.value();

    auto elements_it = aggr_match_json.find(kAclMatchFieldElements);
    // b/175596733: temp disable verification on composite elements field until
    // p4rt implementation is added.
    if (elements_it == aggr_match_json.end())
    {
        (*udf_fields_lookup)[p4_match];
        return ReturnCode();
    }
    if (elements_it.value().is_null() || !elements_it.value().is_array())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match field {" << p4_match << ": " << aggr_match_str
               << "} is an invalid ACL table attribute: 'elements' value is "
                  "required and should be an array";
    }
    for (const auto &element : elements_it.value())
    {
        if (element.is_null() || !element.is_object())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "ACL table match field {" << p4_match << ": " << aggr_match_str
                   << "} is an invalid ACL table attribute: 'elements' member "
                      "should be an json";
        }
        const auto &element_kind_it = element.find(kAclMatchFieldKind);
        if (element_kind_it == element.end() || element_kind_it.value().is_null() ||
            !element_kind_it.value().is_string())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "ACL table match field {" << p4_match << ": " << aggr_match_str
                   << "} is an invalid ACL table attribute: composite element "
                      "'kind' value is required and should be a string";
        }
        ReturnCode rc;
        if (element_kind_it.value() == kAclMatchFieldSaiField)
        {
            rc = validateAndSetCompositeElementSaiFieldJson(element, p4_match, composite_sai_match_fields_lookup,
                                                            format_str_it.value());
        }
        else if (element_kind_it.value() == kAclMatchFieldKindUdf)
        {
            if (format_str_it.value() != P4_FORMAT_HEX_STRING)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "ACL table match field {" << p4_match << ": " << aggr_match_str
                       << "} is an invalid ACL table attribute: " << kAclMatchFieldFormat
                       << " value should be HEX_STRING for UDF field";
            }
            rc = validateAndSetUdfFieldJson(element, p4_match, aggr_match_str, acl_table_name, udf_fields_lookup,
                                            udf_group_attr_index_lookups);
        }
        else
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "ACL table match field {" << p4_match << ": " << aggr_match_str
                   << "} is an invalid ACL table attribute: composite element "
                      "'kind' should be either "
                   << kAclMatchFieldKindUdf << " or " << kAclMatchFieldSaiField;
        }
        if (!rc.ok())
            return rc;
    }
    // elements kind should be all sai_field or all udf.
    auto sai_field_it = composite_sai_match_fields_lookup->find(p4_match);
    auto udf_field_it = udf_fields_lookup->find(p4_match);
    if (sai_field_it != composite_sai_match_fields_lookup->end() && udf_field_it != udf_fields_lookup->end())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match field {" << p4_match << ": " << aggr_match_str
               << "} is an invalid ACL table attribute: composite element "
                  "'kind' should be consistent within all elements.";
    }
    // The sum of bitwidth of elements should equals overall bitwidth defined
    // in composite fields
    uint32_t total_bitwidth = 0;
    if (sai_field_it != composite_sai_match_fields_lookup->end())
    {
        // IPV6_64bit(IPV6_WORD3 and IPV6_WORD2 in elements, kind:sai_field,
        // format:IPV6)
        if (sai_field_it->second.size() != 2)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "ACL table match field {" << p4_match << ": " << aggr_match_str
                   << "} is an invalid ACL table attribute: composite match field "
                      "with sai_field in element kind should have 2 elements.";
        }
        if (!((sai_field_it->second[0].table_attr == SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6_WORD3 &&
               sai_field_it->second[1].table_attr == SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6_WORD2) ||
              (sai_field_it->second[0].table_attr == SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD3 &&
               sai_field_it->second[1].table_attr == SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD2)))
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "ACL table match field {" << p4_match << ": " << aggr_match_str
                   << "} is an invalid ACL table attribute: For composite match "
                      "field "
                      "with element.kind == sai_field, the SAI match field "
                      "in elements list should be either pair {"
                   << P4_MATCH_DST_IPV6_WORD3 << ", " << P4_MATCH_DST_IPV6_WORD2 << "} or pair {"
                   << P4_MATCH_SRC_IPV6_WORD3 << ", " << P4_MATCH_SRC_IPV6_WORD2 << "} with the correct sequence";
        }
        total_bitwidth = sai_field_it->second[0].bitwidth + sai_field_it->second[1].bitwidth;
    }
    if (udf_field_it != udf_fields_lookup->end())
    {
        for (const auto &udf_field : udf_field_it->second)
        {
            total_bitwidth += (uint32_t)udf_field.length * BYTE_BITWIDTH;
        }
    }
    if (total_bitwidth != composite_bitwidth)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "ACL table match field {" << p4_match << ": " << aggr_match_str
               << "} is an invalid ACL table attribute: composite bitwidth "
                  "does not equal with the sum of elements bitwidth.";
    }
    return ReturnCode();
}

ReturnCode buildAclTableDefinitionMatchFieldValues(const std::map<std::string, std::string> &match_field_lookup,
                                                   P4AclTableDefinition *acl_table)
{
    for (const auto &raw_match_field : match_field_lookup)
    {
        const auto &p4_match = fvField(raw_match_field);
        const auto &aggr_match_str = fvValue(raw_match_field);
        try
        {
            const auto &aggr_match_json = nlohmann::json::parse(aggr_match_str);
            if (!aggr_match_json.is_object())
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "ACL table match field {" << p4_match << ": " << aggr_match_str
                       << "} is an invalid ACL table attribute: expecting an json";
            }

            const auto &kind_it = aggr_match_json.find(kAclMatchFieldKind);
            if (kind_it == aggr_match_json.end() || kind_it.value().is_null() || !kind_it.value().is_string())
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "ACL table match field {" << p4_match << ": " << aggr_match_str
                       << "} is an invalid ACL table attribute: 'kind' value is "
                          "required and should be a string";
            }
            ReturnCode rc;
            if (kind_it.value() == kAclMatchFieldSaiField)
            {
                rc = validateAndSetSaiMatchFieldJson(aggr_match_json, p4_match, aggr_match_str,
                                                     &acl_table->sai_match_field_lookup,
                                                     &acl_table->ip_type_bit_type_lookup);
            }
            else if (kind_it.value() == kAclMatchFieldKindComposite)
            {
                rc = validateAndSetCompositeMatchFieldJson(
                    aggr_match_json, p4_match, aggr_match_str, acl_table->acl_table_name,
                    &acl_table->composite_sai_match_fields_lookup, &acl_table->udf_fields_lookup,
                    &acl_table->udf_group_attr_index_lookup);
            }
            else if (kind_it.value() == kAclMatchFieldKindUdf)
            {
                auto format_str_it = aggr_match_json.find(kAclMatchFieldFormat);
                if (format_str_it == aggr_match_json.end() || format_str_it.value().is_null() ||
                    !format_str_it.value().is_string())
                {
                    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "ACL table match field {" << p4_match << ": " << aggr_match_str
                           << "} is an invalid ACL table attribute: " << kAclMatchFieldFormat
                           << " value is required and should be a string";
                }
                if (format_str_it.value() != P4_FORMAT_HEX_STRING)
                {
                    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "ACL table match field {" << p4_match << ": " << aggr_match_str
                           << "} is an invalid ACL table attribute: " << kAclMatchFieldFormat
                           << " value should be HEX_STRING for UDF field";
                }
                rc = validateAndSetUdfFieldJson(aggr_match_json, p4_match, aggr_match_str, acl_table->acl_table_name,
                                                &acl_table->udf_fields_lookup, &acl_table->udf_group_attr_index_lookup);
            }
            else
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "ACL table match field {" << p4_match << ": " << aggr_match_str
                       << "} is an invalid ACL table attribute: 'kind' is expecting "
                          "one of {"
                       << kAclMatchFieldKindComposite << ", " << kAclMatchFieldSaiField << ", " << kAclMatchFieldKindUdf
                       << "}.";
            }
            if (!rc.ok())
                return rc;
        }
        catch (std::exception &ex)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "ACL table match field {" << p4_match << ": " << aggr_match_str
                   << "} is an invalid ACL table attribute: ex" << ex.what();
        }
    }
    return ReturnCode();
}

ReturnCode buildAclTableDefinitionActionFieldValues(
    const std::map<std::string, std::vector<P4ActionParamName>> &action_field_lookup,
    std::map<std::string, std::vector<SaiActionWithParam>> *aggr_sai_actions_lookup)
{
    SaiActionWithParam action_with_param;
    for (const auto &aggr_action_field : action_field_lookup)
    {
        auto &aggr_sai_actions = (*aggr_sai_actions_lookup)[fvField(aggr_action_field)];
        for (const auto &single_action : fvValue(aggr_action_field))
        {
            auto rule_action_it = aclActionLookup.find(single_action.sai_action);
            if (rule_action_it == aclActionLookup.end())
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "ACL table action is invalid: " << single_action.sai_action;
            }
            action_with_param.action = rule_action_it->second;
            action_with_param.param_name = single_action.p4_param_name;
            aggr_sai_actions.push_back(action_with_param);
        }
    }
    return ReturnCode();
}

ReturnCode buildAclTableDefinitionActionColorFieldValues(
    const std::map<std::string, std::vector<P4PacketActionWithColor>> &action_color_lookup,
    std::map<std::string, std::vector<SaiActionWithParam>> *aggr_sai_actions_lookup,
    std::map<std::string, std::map<sai_policer_attr_t, sai_packet_action_t>> *aggr_sai_action_color_lookup)
{
    for (const auto &aggr_action_color : action_color_lookup)
    {
        auto &aggr_sai_actions = (*aggr_sai_actions_lookup)[fvField(aggr_action_color)];
        auto &aggr_sai_action_color = (*aggr_sai_action_color_lookup)[fvField(aggr_action_color)];
        for (const auto &action_color : fvValue(aggr_action_color))
        {
            auto packet_action_it = aclPacketActionLookup.find(action_color.packet_action);
            if (packet_action_it == aclPacketActionLookup.end())
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "ACL table packet action is invalid: " << action_color.packet_action;
            }

            if (action_color.packet_color.empty())
            {
                // Handle packet action without packet color, set ACL entry attribute
                SaiActionWithParam action_with_param;
                action_with_param.action = SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION;
                action_with_param.param_name = EMPTY_STRING;
                action_with_param.param_value = action_color.packet_action;
                aggr_sai_actions.push_back(action_with_param);
                continue;
            }

            // Handle packet action with packet color, set ACL policer attribute
            auto packet_color_it = aclPacketColorPolicerAttrLookup.find(action_color.packet_color);
            if (packet_color_it == aclPacketColorPolicerAttrLookup.end())
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "ACL table packet color is invalid: " << action_color.packet_color;
            }
            aggr_sai_action_color[packet_color_it->second] = packet_action_it->second;
        }
    }
    return ReturnCode();
}

bool isSetUserTrapActionInAclTableDefinition(
    const std::map<std::string, std::vector<SaiActionWithParam>> &aggr_sai_actions_lookup)
{
    for (const auto &aggr_action : aggr_sai_actions_lookup)
    {
        for (const auto &sai_action : fvValue(aggr_action))
        {
            if (sai_action.action == SAI_ACL_ENTRY_ATTR_ACTION_SET_USER_TRAP_ID)
                return true;
        }
    }
    return false;
}

bool setMatchFieldIpType(const std::string &attr_value, sai_attribute_value_t *value,
                         const std::string &ip_type_bit_type)
{
    SWSS_LOG_ENTER();
    if (ip_type_bit_type == EMPTY_STRING)
    {
        SWSS_LOG_ERROR("Invalid IP type %s, bit type is not defined.", attr_value.c_str());
        return false;
    }
    // go/p4-ip-type
    const auto &ip_type_bit_data_mask = swss::tokenize(attr_value, kDataMaskDelimiter);
    if (ip_type_bit_data_mask.size() == 2 && swss::to_uint<uint16_t>(trim(ip_type_bit_data_mask[1])) == 0)
    {
        SWSS_LOG_ERROR("Invalid IP_TYPE mask %s for bit type %s: ip type bit mask "
                       "should not be zero.",
                       attr_value.c_str(), ip_type_bit_type.c_str());
        return false;
    }
    int ip_type_bit_data = std::stoi(ip_type_bit_data_mask[0], nullptr, 0);
    value->aclfield.mask.u32 = 0xFFFFFFFF;
    if (ip_type_bit_type == P4_IP_TYPE_BIT_IP)
    {
        if (ip_type_bit_data)
        {
            value->aclfield.data.u32 = SAI_ACL_IP_TYPE_IP;
        }
        else
        {
            value->aclfield.data.u32 = SAI_ACL_IP_TYPE_NON_IP;
        }
    }
    else if (ip_type_bit_type == P4_IP_TYPE_BIT_IPV4ANY)
    {
        if (ip_type_bit_data)
        {
            value->aclfield.data.u32 = SAI_ACL_IP_TYPE_IPV4ANY;
        }
        else
        {
            value->aclfield.data.u32 = SAI_ACL_IP_TYPE_NON_IPV4;
        }
    }
    else if (ip_type_bit_type == P4_IP_TYPE_BIT_IPV6ANY)
    {
        if (ip_type_bit_data)
        {
            value->aclfield.data.u32 = SAI_ACL_IP_TYPE_IPV6ANY;
        }
        else
        {
            value->aclfield.data.u32 = SAI_ACL_IP_TYPE_NON_IPV6;
        }
    }
    else if (ip_type_bit_type == P4_IP_TYPE_BIT_ARP && ip_type_bit_data)
    {
        value->aclfield.data.u32 = SAI_ACL_IP_TYPE_ARP;
    }
    else if (ip_type_bit_type == P4_IP_TYPE_BIT_ARP_REQUEST && ip_type_bit_data)
    {
        value->aclfield.data.u32 = SAI_ACL_IP_TYPE_ARP_REQUEST;
    }
    else if (ip_type_bit_type == P4_IP_TYPE_BIT_ARP_REPLY && ip_type_bit_data)
    {
        value->aclfield.data.u32 = SAI_ACL_IP_TYPE_ARP_REPLY;
    }
    else
    {
        SWSS_LOG_ERROR("Invalid IP_TYPE bit data %s for ip type %s", attr_value.c_str(), ip_type_bit_type.c_str());
        return false;
    }
    return true;
}

ReturnCode setCompositeSaiMatchValue(const acl_entry_attr_union_t attr_name, const std::string &attr_value,
                                     sai_attribute_value_t *value)
{
    try
    {
        const auto &tokenized_ip = swss::tokenize(attr_value, kDataMaskDelimiter);
        swss::IpAddress ip_data;
        swss::IpAddress ip_mask;
        if (tokenized_ip.size() == 2)
        {
            // data & mask
            ip_data = swss::IpAddress(trim(tokenized_ip[0]));
            if (ip_data.isV4())
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "IP data type should be v6 type: " << attr_value;
            }
            ip_mask = swss::IpAddress(trim(tokenized_ip[1]));
            if (ip_mask.isV4())
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "IP mask type should be v6 type: " << attr_value;
            }
        }
        else
        {
            // LPM annotated value
            swss::IpPrefix ip_prefix(trim(attr_value));
            if (ip_prefix.isV4())
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "IP type should be v6 type: " << attr_value;
            }
            ip_data = ip_prefix.getIp();
            ip_mask = ip_prefix.getMask();
        }
        switch (attr_name)
        {
        case SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6_WORD3:
        case SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD3: {
            // IPv6 Address 127:96 32 bits
            memcpy(&value->aclfield.data.ip6[0], &ip_data.getV6Addr()[0], IPV6_SINGLE_WORD_BYTES_LENGTH);
            memcpy(&value->aclfield.mask.ip6[0], &ip_mask.getV6Addr()[0], IPV6_SINGLE_WORD_BYTES_LENGTH);
            break;
        }
        case SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6_WORD2:
        case SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD2: {
            // IPv6 Address 95:64 32 bits
            memcpy(&value->aclfield.data.ip6[IPV6_SINGLE_WORD_BYTES_LENGTH],
                   &ip_data.getV6Addr()[IPV6_SINGLE_WORD_BYTES_LENGTH], IPV6_SINGLE_WORD_BYTES_LENGTH);
            memcpy(&value->aclfield.mask.ip6[IPV6_SINGLE_WORD_BYTES_LENGTH],
                   &ip_mask.getV6Addr()[IPV6_SINGLE_WORD_BYTES_LENGTH], IPV6_SINGLE_WORD_BYTES_LENGTH);
            break;
        }
        default: {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "ACL match field " << attr_name << " is not supported in composite match field sai_field type.";
        }
        }
    }
    catch (std::exception &e)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Failed to parse match attribute " << attr_name
                                                             << " (value: " << attr_value << "). Error:" << e.what();
    }
    value->aclfield.enable = true;
    return ReturnCode();
}

ReturnCode setUdfMatchValue(const P4UdfField &udf_field, const std::string &attr_value, sai_attribute_value_t *value,
                            P4UdfDataMask *udf_data_mask, uint16_t bytes_offset)
{
    if (!udf_data_mask->data.empty() || !udf_data_mask->mask.empty())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Failed to set UDF match field " << udf_field.udf_id << " with value " << attr_value
               << " in ACL rule: the UDF: duplicated UDF value found for the same "
                  "UDF field.";
    }
    try
    {
        // Extract UDF field values by length(in bytes) and offset(in bytes)
        const std::vector<std::string> &value_and_mask = swss::tokenize(attr_value, kDataMaskDelimiter);
        uint32_t data_str_offset = bytes_offset * 2, mask_str_offset = bytes_offset * 2;
        const auto &data = trim(value_and_mask[0]);
        if (data.size() > 2 && data[0] == '0' && (data[1] == 'x' || data[1] == 'X'))
        {
            data_str_offset += 2;
        }
        std::string mask = EMPTY_STRING;
        if (value_and_mask.size() > 1)
        {
            mask = trim(value_and_mask[1]);
            if (mask.size() > 2 && mask[0] == '0' && (mask[1] == 'x' || mask[1] == 'X'))
            {
                mask_str_offset += 2;
            }
        }
        for (uint16_t i = 0; i < udf_field.length; i++)
        {
            // Add to udf_data uint8_t list
            udf_data_mask->data.push_back(std::stoul(data.substr(data_str_offset, 2), nullptr, 16) & 0xFF);
            data_str_offset += 2;
            if (value_and_mask.size() > 1)
            {
                // Add to udf_mask uint8_t list
                udf_data_mask->mask.push_back((std::stoul(mask.substr(mask_str_offset, 2), nullptr, 16)) & 0xFF);
                mask_str_offset += 2;
            }
            else
            {
                udf_data_mask->mask.push_back(0xFF);
            }
        }
        value->aclfield.data.u8list.count = udf_field.length;
        value->aclfield.data.u8list.list = udf_data_mask->data.data();
        value->aclfield.mask.u8list.count = udf_field.length;
        value->aclfield.mask.u8list.list = udf_data_mask->mask.data();
    }
    catch (std::exception &ex)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Failed to set UDF match field " << udf_field.udf_id << " with value " << attr_value
               << " in ACL rule: " << ex.what();
    }
    value->aclfield.enable = true;
    return ReturnCode();
}

bool isDiffActionFieldValue(const acl_entry_attr_union_t attr_name, const sai_attribute_value_t &value,
                            const sai_attribute_value_t &old_value, const P4AclRule &acl_rule,
                            const P4AclRule &old_acl_rule)
{
    switch (attr_name)
    {
    case SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_PACKET_COLOR: {
        return value.aclaction.parameter.s32 != old_value.aclaction.parameter.s32;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT: {
        return value.aclaction.parameter.oid != old_value.aclaction.parameter.oid;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_ENDPOINT_IP:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_IP:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_DST_IP: {
        return value.aclaction.parameter.ip4 != old_value.aclaction.parameter.ip4;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS:
    case SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_EGRESS: {
        return acl_rule.action_mirror_sessions.at(attr_name).oid !=
               old_acl_rule.action_mirror_sessions.at(attr_name).oid;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_MAC:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_DST_MAC: {
        return memcmp(value.aclaction.parameter.mac, old_value.aclaction.parameter.mac, sizeof(sai_mac_t));
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_IPV6:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_DST_IPV6: {
        return memcmp(value.aclaction.parameter.ip6, old_value.aclaction.parameter.ip6, sizeof(sai_ip6_t));
    }

    case SAI_ACL_ENTRY_ATTR_ACTION_SET_TC:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_DSCP:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_ECN:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_INNER_VLAN_PRI:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_OUTER_VLAN_PRI: {
        return value.aclaction.parameter.u8 != old_value.aclaction.parameter.u8;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_INNER_VLAN_ID: {
        return value.aclaction.parameter.u32 != old_value.aclaction.parameter.u32;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_OUTER_VLAN_ID:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_L4_SRC_PORT:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_L4_DST_PORT: {
        return value.aclaction.parameter.u16 != old_value.aclaction.parameter.u16;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_VRF:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_USER_TRAP_ID: {
        return value.aclaction.parameter.oid != old_value.aclaction.parameter.oid;
    }
    case SAI_ACL_ENTRY_ATTR_ACTION_FLOOD:
    case SAI_ACL_ENTRY_ATTR_ACTION_DECREMENT_TTL:
    case SAI_ACL_ENTRY_ATTR_ACTION_SET_DO_NOT_LEARN: {
        // parameter is not needed
        return false;
    }
    default: {
        return false;
    }
    }
}

} // namespace p4orch
