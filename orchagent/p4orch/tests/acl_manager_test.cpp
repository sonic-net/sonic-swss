#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>

#include "acl_rule_manager.h"
#include "acl_table_manager.h"
#include "acl_util.h"
#include "acltable.h"
#include "json.hpp"
#include "mock_sai_acl.h"
#include "mock_sai_hostif.h"
#include "mock_sai_policer.h"
#include "mock_sai_serialize.h"
#include "mock_sai_switch.h"
#include "mock_sai_udf.h"
#include "p4orch.h"
#include "return_code.h"
#include "switchorch.h"
#include "table.h"
#include "tokenize.h"
#include "vrforch.h"

extern swss::DBConnector *gAppDb;
extern swss::DBConnector *gStateDb;
extern swss::DBConnector *gCountersDb;
extern swss::DBConnector *gConfigDb;
extern sai_acl_api_t *sai_acl_api;
extern sai_policer_api_t *sai_policer_api;
extern sai_hostif_api_t *sai_hostif_api;
extern sai_switch_api_t *sai_switch_api;
extern sai_udf_api_t *sai_udf_api;
extern int gBatchSize;
extern VRFOrch *gVrfOrch;
extern P4Orch *gP4Orch;
extern SwitchOrch *gSwitchOrch;
extern sai_object_id_t gSwitchId;
extern sai_object_id_t gVrfOid;
extern sai_object_id_t gTrapGroupStartOid;
extern sai_object_id_t gHostifStartOid;
extern sai_object_id_t gUserDefinedTrapStartOid;
extern char *gVrfName;
extern char *gMirrorSession1;
extern sai_object_id_t kMirrorSessionOid1;
extern char *gMirrorSession2;
extern sai_object_id_t kMirrorSessionOid2;
extern bool gIsNatSupported;

namespace p4orch
{
namespace test
{

using ::testing::_;
using ::testing::DoAll;
using ::testing::Eq;
using ::testing::Gt;
using ::testing::Invoke;
using ::testing::NotNull;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::StrictMock;
using ::testing::Truly;

namespace
{
constexpr sai_object_id_t kAclGroupIngressOid = 0xb00000000058f;
constexpr sai_object_id_t kAclGroupEgressOid = 0xb000000000591;
constexpr sai_object_id_t kAclGroupLookupOid = 0xb000000000592;
constexpr sai_object_id_t kAclTableIngressOid = 0x7000000000606;
constexpr sai_object_id_t kAclGroupMemberIngressOid = 0xc000000000607;
constexpr sai_object_id_t kAclIngressRuleOid1 = 1001;
constexpr sai_object_id_t kAclIngressRuleOid2 = 1002;
constexpr sai_object_id_t kAclMeterOid1 = 2001;
constexpr sai_object_id_t kAclMeterOid2 = 2002;
constexpr sai_object_id_t kAclCounterOid1 = 3001;
constexpr sai_object_id_t kUdfGroupOid1 = 4001;
constexpr sai_object_id_t kUdfMatchOid1 = 5001;
constexpr char *kAclIngressTableName = "ACL_PUNT_TABLE";

// Check the ACL stage sai_attribute_t list for ACL table group
bool MatchSaiAttributeAclGroupStage(const sai_acl_stage_t expected_stage, const sai_attribute_t *attr_list)
{
    if (attr_list == nullptr)
    {
        return false;
    }
    for (int i = 0; i < 3; ++i)
    {
        switch (attr_list[i].id)
        {
        case SAI_ACL_TABLE_GROUP_ATTR_ACL_STAGE:
            if (attr_list[i].value.s32 != expected_stage)
            {
                return false;
            }
            break;
        case SAI_ACL_TABLE_GROUP_ATTR_TYPE:
            if (attr_list[i].value.s32 != SAI_ACL_TABLE_GROUP_TYPE_PARALLEL)
            {
                return false;
            }
            break;
        case SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST:
            if (attr_list[i].value.s32list.count != 1 ||
                attr_list[i].value.s32list.list[0] != SAI_ACL_BIND_POINT_TYPE_SWITCH)
            {
                return false;
            }
            break;
        default:
            return false;
        }
    }
    return true;
}

// Check the ACL stage sai_attribute_t list for ACL table
bool MatchSaiAttributeAclTableStage(const sai_acl_stage_t expected_stage, const sai_attribute_t *attr_list)
{
    if (attr_list == nullptr)
    {
        return false;
    }
    if (attr_list[0].id != SAI_ACL_TABLE_GROUP_ATTR_ACL_STAGE || attr_list[0].value.s32 != expected_stage)
    {
        return false;
    }

    return true;
}

bool MatchSaiSwitchAttrByAclStage(const sai_switch_attr_t expected_switch_attr, const sai_object_id_t group_oid,
                                  const sai_attribute_t *attr)
{
    if (attr->id != expected_switch_attr || attr->value.oid != group_oid)
    {
        return false;
    }
    return true;
}

std::string BuildMatchFieldJsonStrKindSaiField(std::string sai_field, std::string format = P4_FORMAT_HEX_STRING,
                                               uint32_t bitwidth = 0)
{
    nlohmann::json match_json;
    match_json[kAclMatchFieldKind] = kAclMatchFieldSaiField;
    match_json[kAclMatchFieldSaiField] = sai_field;
    match_json[kAclMatchFieldFormat] = format;
    if (format != P4_FORMAT_STRING)
    {
        match_json[kAclMatchFieldBitwidth] = bitwidth;
    }
    return match_json.dump();
}

std::string BuildMatchFieldJsonStrKindComposite(std::vector<nlohmann::json> elements,
                                                std::string format = P4_FORMAT_HEX_STRING, uint32_t bitwidth = 0)
{
    nlohmann::json match_json;
    match_json[kAclMatchFieldKind] = kAclMatchFieldKindComposite;
    for (const auto element : elements)
    {
        match_json[kAclMatchFieldElements].push_back(element);
    }
    match_json[kAclMatchFieldFormat] = format;
    match_json[kAclMatchFieldBitwidth] = bitwidth;
    return match_json.dump();
}

std::string BuildMatchFieldJsonStrKindUdf(std::string base, uint32_t offset, std::string format = P4_FORMAT_HEX_STRING,
                                          uint32_t bitwidth = 0)
{
    nlohmann::json match_json;
    match_json[kAclMatchFieldKind] = kAclMatchFieldKindUdf;
    match_json[kAclUdfBase] = base;
    match_json[kAclUdfOffset] = offset;
    match_json[kAclMatchFieldFormat] = format;
    match_json[kAclMatchFieldBitwidth] = bitwidth;
    return match_json.dump();
}

// Check if P4AclTableDefinitionAppDbEntry to P4AclTableDefinition mapping is as
// expected
void IsExpectedAclTableDefinitionMapping(const P4AclTableDefinition &acl_table_def,
                                         const P4AclTableDefinitionAppDbEntry &app_db_entry)
{
    EXPECT_EQ(app_db_entry.acl_table_name, acl_table_def.acl_table_name);
    EXPECT_EQ(app_db_entry.priority, acl_table_def.priority);
    EXPECT_EQ(app_db_entry.size, acl_table_def.size);
    EXPECT_EQ(app_db_entry.meter_unit, acl_table_def.meter_unit);
    EXPECT_EQ(app_db_entry.counter_unit, acl_table_def.counter_unit);
    for (const auto &raw_match_field : app_db_entry.match_field_lookup)
    {
        const auto &p4_match = fvField(raw_match_field);
        const auto &aggr_match_str = fvValue(raw_match_field);
        try
        {
            auto aggr_match_json = nlohmann::json::parse(aggr_match_str);
            ASSERT_TRUE(aggr_match_json.is_object());
            auto kind = aggr_match_json[kAclMatchFieldKind];
            ASSERT_TRUE(!kind.is_null() && kind.is_string());
            if (kind == kAclMatchFieldKindComposite)
            {
                auto format_str = aggr_match_json[kAclMatchFieldFormat];
                ASSERT_FALSE(format_str.is_null() || !format_str.is_string());
                auto format_it = formatLookup.find(format_str);
                ASSERT_NE(formatLookup.end(), format_it);
                if (format_it->second != Format::STRING)
                {
                    // bitwidth is required if the format is not "STRING"
                    auto bitwidth = aggr_match_json[kAclMatchFieldBitwidth];
                    ASSERT_FALSE(bitwidth.is_null() || !bitwidth.is_number());
                }
                auto elements = aggr_match_json[kAclMatchFieldElements];
                ASSERT_TRUE(!elements.is_null() && elements.is_array());
                if (elements[0][kAclMatchFieldKind] == kAclMatchFieldSaiField)
                {
                    const auto &composite_sai_match_it = acl_table_def.composite_sai_match_fields_lookup.find(p4_match);
                    ASSERT_NE(acl_table_def.composite_sai_match_fields_lookup.end(), composite_sai_match_it);
                    for (const auto &element : composite_sai_match_it->second)
                    {
                        EXPECT_EQ(BYTE_BITWIDTH * IPV6_SINGLE_WORD_BYTES_LENGTH, element.bitwidth);
                    }
                }
                else if (elements[0][kAclMatchFieldKind] == kAclMatchFieldKindUdf)
                {
                    const auto &composite_udf_match_it = acl_table_def.udf_fields_lookup.find(p4_match);
                    ASSERT_NE(acl_table_def.udf_fields_lookup.end(), composite_udf_match_it);
                    for (size_t i = 0; i < composite_udf_match_it->second.size(); i++)
                    {
                        EXPECT_EQ(elements[i][kAclMatchFieldBitwidth],
                                  composite_udf_match_it->second[i].length * BYTE_BITWIDTH);
                        EXPECT_EQ(elements[i][kAclUdfOffset], composite_udf_match_it->second[i].offset);
                        EXPECT_EQ(app_db_entry.acl_table_name + "-" + p4_match + "-" + std::to_string(i),
                                  composite_udf_match_it->second[i].group_id);
                        ASSERT_NE(udfBaseLookup.end(), udfBaseLookup.find(elements[i][kAclUdfBase]));
                        EXPECT_EQ(udfBaseLookup.find(elements[i][kAclUdfBase])->second,
                                  composite_udf_match_it->second[i].base);
                    }
                }
                else
                {
                    FAIL() << "Invalid kind for composite field element: " << elements[0][kAclMatchFieldKind];
                }
            }
            else if (kind == kAclMatchFieldKindUdf)
            {
                const auto &udf_match_it = acl_table_def.udf_fields_lookup.find(p4_match);
                ASSERT_NE(acl_table_def.udf_fields_lookup.end(), udf_match_it);
                EXPECT_EQ(1, udf_match_it->second.size());
                EXPECT_EQ(aggr_match_json[kAclMatchFieldBitwidth], udf_match_it->second[0].length * BYTE_BITWIDTH);
                EXPECT_EQ(aggr_match_json[kAclUdfOffset], udf_match_it->second[0].offset);
                EXPECT_EQ(app_db_entry.acl_table_name + "-" + p4_match + "-0", udf_match_it->second[0].group_id);
                ASSERT_NE(udfBaseLookup.end(), udfBaseLookup.find(aggr_match_json[kAclUdfBase]));
                EXPECT_EQ(udfBaseLookup.find(aggr_match_json[kAclUdfBase])->second, udf_match_it->second[0].base);
            }
            else
            {
                EXPECT_EQ(kAclMatchFieldSaiField, kind);
                auto match_field = aggr_match_json[kAclMatchFieldSaiField];
                ASSERT_TRUE(!match_field.is_null() && match_field.is_string());
                auto field_suffix = swss::tokenize(match_field, kFieldDelimiter);
                const auto &sai_field = field_suffix[0];
                ASSERT_NE(aclMatchEntryAttrLookup.end(), aclMatchEntryAttrLookup.find(sai_field));
                ASSERT_NE(aclMatchTableAttrLookup.end(), aclMatchTableAttrLookup.find(sai_field));
                EXPECT_EQ(aclMatchEntryAttrLookup.find(sai_field)->second,
                          acl_table_def.sai_match_field_lookup.find(p4_match)->second.entry_attr);
                EXPECT_EQ(aclMatchTableAttrLookup.find(sai_field)->second,
                          acl_table_def.sai_match_field_lookup.find(p4_match)->second.table_attr);
            }
        }
        catch (std::exception &ex)
        {
            FAIL() << "Exception when parsing match field. ex: " << ex.what();
        }
    }
    for (const auto &action_field : app_db_entry.action_field_lookup)
    {
        const auto &sai_action_param_it = acl_table_def.rule_action_field_lookup.find(fvField(action_field));
        ASSERT_NE(acl_table_def.rule_action_field_lookup.end(), sai_action_param_it);

        for (size_t i = 0; i < fvValue(action_field).size(); ++i)
        {
            ASSERT_NE(aclActionLookup.end(), aclActionLookup.find(fvValue(action_field)[i].sai_action));
            EXPECT_EQ(sai_action_param_it->second[i].action,
                      aclActionLookup.find(fvValue(action_field)[i].sai_action)->second);
        }
    }

    for (const auto &packet_action_color : app_db_entry.packet_action_color_lookup)
    {
        const auto &sai_action_color_it =
            acl_table_def.rule_packet_action_color_lookup.find(fvField(packet_action_color));
        ASSERT_NE(acl_table_def.rule_packet_action_color_lookup.end(), sai_action_color_it);
        for (size_t i = 0; i < fvValue(packet_action_color).size(); ++i)
        {
            if (fvValue(packet_action_color)[i].packet_color.empty())
            {
                // Not a colored packet action, should be ACL entry attribute
                // SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION instead of ACL policy
                // attribute
                const auto rule_action_it = acl_table_def.rule_action_field_lookup.find(fvField(packet_action_color));
                ASSERT_NE(acl_table_def.rule_action_field_lookup.end(), rule_action_it);
                bool found_packet_action = false;
                for (const auto &action_with_param : rule_action_it->second)
                {
                    if (action_with_param.param_value == fvValue(packet_action_color)[i].packet_action)
                    {
                        // Only one packet action is allowed and no parameter should be
                        // added for SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION attribute.
                        // Return false if multiple packet actions are found or
                        // parameter name is not empty
                        EXPECT_FALSE(found_packet_action || !action_with_param.param_name.empty());
                        found_packet_action = true;
                    }
                }
                // No packet action was found, return false.
                EXPECT_TRUE(found_packet_action);
                continue;
            }
            const auto &packet_color_policer_attr_it =
                aclPacketColorPolicerAttrLookup.find(fvValue(packet_action_color)[i].packet_color);
            const auto &packet_action_it = aclPacketActionLookup.find(fvValue(packet_action_color)[i].packet_action);
            ASSERT_NE(aclPacketColorPolicerAttrLookup.end(), packet_color_policer_attr_it);
            ASSERT_NE(aclPacketActionLookup.end(), packet_action_it);

            const auto &sai_packet_action_it = sai_action_color_it->second.find(packet_color_policer_attr_it->second);
            ASSERT_NE(sai_action_color_it->second.end(), sai_packet_action_it);
            EXPECT_EQ(sai_packet_action_it->second, packet_action_it->second);
        }
    }
}

// Check if P4AclRuleAppDbEntry to P4AclRule field mapping is as
// expected given table definition. Specific match and action value
// validation should be done in caller method
void IsExpectedAclRuleMapping(const P4AclRule *acl_rule, const P4AclRuleAppDbEntry &app_db_entry,
                              const P4AclTableDefinition &table_def)
{
    // Check table name and priority
    EXPECT_EQ(app_db_entry.acl_table_name, acl_rule->acl_table_name);
    EXPECT_EQ(app_db_entry.priority, acl_rule->priority);
    // Check match field
    for (const auto &app_db_match_fv : app_db_entry.match_fvs)
    {
        // TODO: Fake UDF field until SAI supports it
        if (table_def.udf_fields_lookup.find(fvField(app_db_match_fv)) != table_def.udf_fields_lookup.end())
            continue;
        const auto &composite_sai_match_fields_it =
            table_def.composite_sai_match_fields_lookup.find(fvField(app_db_match_fv));
        if (composite_sai_match_fields_it != table_def.composite_sai_match_fields_lookup.end())
        {
            for (const auto &composite_sai_match_field : composite_sai_match_fields_it->second)
            {
                const auto &match_fv_it = acl_rule->match_fvs.find(composite_sai_match_field.entry_attr);
                ASSERT_NE(acl_rule->match_fvs.end(), match_fv_it);
                EXPECT_TRUE(match_fv_it->second.aclfield.enable);
            }
            continue;
        }
        const auto &match_field_it = table_def.sai_match_field_lookup.find(fvField(app_db_match_fv));
        ASSERT_NE(table_def.sai_match_field_lookup.end(), match_field_it);
        const auto &match_fv_it = acl_rule->match_fvs.find(match_field_it->second.entry_attr);
        ASSERT_NE(acl_rule->match_fvs.end(), match_fv_it);
        EXPECT_TRUE(match_fv_it->second.aclfield.enable);
    }
    // Check action field
    ASSERT_EQ(acl_rule->p4_action, app_db_entry.action);
    const auto &actions_field_it = table_def.rule_action_field_lookup.find(app_db_entry.action);
    const auto &packet_action_color_it = table_def.rule_packet_action_color_lookup.find(app_db_entry.action);
    ASSERT_NE(table_def.rule_action_field_lookup.end(), actions_field_it);
    ASSERT_NE(table_def.rule_packet_action_color_lookup.end(), packet_action_color_it);
    if (actions_field_it != table_def.rule_action_field_lookup.end())
    {
        for (const auto &action_field : actions_field_it->second)
        {
            ASSERT_NE(acl_rule->action_fvs.find(action_field.action), acl_rule->action_fvs.end());
        }
    }
    // Check meter field value
    if (packet_action_color_it != table_def.rule_packet_action_color_lookup.end() &&
        !packet_action_color_it->second.empty())
    {
        ASSERT_EQ(acl_rule->meter.packet_color_actions, packet_action_color_it->second);
    }
    if (!table_def.meter_unit.empty())
    {
        EXPECT_TRUE(acl_rule->meter.enabled);
        EXPECT_EQ(SAI_POLICER_MODE_TR_TCM, acl_rule->meter.mode);
        EXPECT_EQ(app_db_entry.meter.cir, acl_rule->meter.cir);
        EXPECT_EQ(app_db_entry.meter.cburst, acl_rule->meter.cburst);
        EXPECT_EQ(app_db_entry.meter.pir, acl_rule->meter.pir);
        EXPECT_EQ(app_db_entry.meter.pburst, acl_rule->meter.pburst);
        if (table_def.meter_unit == P4_METER_UNIT_BYTES)
        {
            EXPECT_EQ(SAI_METER_TYPE_BYTES, acl_rule->meter.type);
        }
        if (table_def.meter_unit == P4_METER_UNIT_PACKETS)
        {
            EXPECT_EQ(SAI_METER_TYPE_PACKETS, acl_rule->meter.type);
        }
    }
    // Check counter field value
    if (table_def.counter_unit.empty())
    {
        EXPECT_FALSE(acl_rule->counter.packets_enabled);
        EXPECT_FALSE(acl_rule->counter.bytes_enabled);
        return;
    }
    if (table_def.counter_unit == P4_COUNTER_UNIT_BOTH)
    {
        EXPECT_TRUE(acl_rule->counter.bytes_enabled && acl_rule->counter.packets_enabled);
    }
    else if (table_def.counter_unit == P4_COUNTER_UNIT_BYTES)
    {
        EXPECT_TRUE(acl_rule->counter.bytes_enabled && !acl_rule->counter.packets_enabled);
    }
    else
    {
        EXPECT_TRUE(table_def.counter_unit == P4_COUNTER_UNIT_PACKETS && !acl_rule->counter.bytes_enabled &&
                    acl_rule->counter.packets_enabled);
    }
}

std::vector<swss::FieldValueTuple> getDefaultTableDefFieldValueTuples()
{
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{kStage, STAGE_INGRESS});
    attributes.push_back(swss::FieldValueTuple{kSize, "123"});
    attributes.push_back(swss::FieldValueTuple{kPriority, "234"});
    attributes.push_back(swss::FieldValueTuple{"meter/unit", P4_METER_UNIT_BYTES});
    attributes.push_back(swss::FieldValueTuple{"counter/unit", P4_COUNTER_UNIT_BOTH});
    attributes.push_back(
        swss::FieldValueTuple{"match/ether_type", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_ETHER_TYPE)});
    attributes.push_back(
        swss::FieldValueTuple{"match/ether_dst", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_DST_MAC, P4_FORMAT_MAC)});
    attributes.push_back(
        swss::FieldValueTuple{"match/ipv6_dst", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_DST_IPV6, P4_FORMAT_IPV6)});
    attributes.push_back(swss::FieldValueTuple{
        "match/is_ip",
        BuildMatchFieldJsonStrKindSaiField(std::string(P4_MATCH_IP_TYPE) + kFieldDelimiter + P4_IP_TYPE_BIT_IP)});
    attributes.push_back(swss::FieldValueTuple{
        "match/is_ipv4",
        BuildMatchFieldJsonStrKindSaiField(std::string(P4_MATCH_IP_TYPE) + kFieldDelimiter + P4_IP_TYPE_BIT_IPV4ANY)});
    attributes.push_back(swss::FieldValueTuple{
        "match/is_ipv6",
        BuildMatchFieldJsonStrKindSaiField(std::string(P4_MATCH_IP_TYPE) + kFieldDelimiter + P4_IP_TYPE_BIT_IPV6ANY)});
    attributes.push_back(swss::FieldValueTuple{
        "match/is_arp",
        BuildMatchFieldJsonStrKindSaiField(std::string(P4_MATCH_IP_TYPE) + kFieldDelimiter + P4_IP_TYPE_BIT_ARP)});
    attributes.push_back(swss::FieldValueTuple{
        "match/is_arp_request", BuildMatchFieldJsonStrKindSaiField(std::string(P4_MATCH_IP_TYPE) + kFieldDelimiter +
                                                                   P4_IP_TYPE_BIT_ARP_REQUEST)});
    attributes.push_back(swss::FieldValueTuple{
        "match/is_arp_reply", BuildMatchFieldJsonStrKindSaiField(std::string(P4_MATCH_IP_TYPE) + kFieldDelimiter +
                                                                 P4_IP_TYPE_BIT_ARP_REPLY)});
    attributes.push_back(
        swss::FieldValueTuple{"match/ipv6_next_header", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_IPV6_NEXT_HEADER)});
    attributes.push_back(swss::FieldValueTuple{"match/ttl", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_TTL)});
    attributes.push_back(
        swss::FieldValueTuple{"match/icmp_type", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_ICMP_TYPE)});
    attributes.push_back(
        swss::FieldValueTuple{"match/l4_dst_port", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_L4_DST_PORT)});
    attributes.push_back(swss::FieldValueTuple{"action/copy_and_set_tc",
                                               "[{\"action\":\"SAI_PACKET_ACTION_COPY\",\"packet_color\":\"SAI_"
                                               "PACKET_"
                                               "COLOR_GREEN\"},{\"action\":\"SAI_ACL_ENTRY_ATTR_ACTION_SET_TC\","
                                               "\"param\":\"traffic_class\"}]"});
    attributes.push_back(swss::FieldValueTuple{"action/punt_and_set_tc",
                                               "[{\"action\":\"SAI_PACKET_ACTION_TRAP\"},{\"action\":\"SAI_ACL_"
                                               "ENTRY_"
                                               "ATTR_ACTION_SET_TC\",\"param\":\"traffic_class\"}]"});
    return attributes;
}

P4AclTableDefinitionAppDbEntry getDefaultAclTableDefAppDbEntry()
{
    P4AclTableDefinitionAppDbEntry app_db_entry;
    app_db_entry.acl_table_name = kAclIngressTableName;
    app_db_entry.size = 123;
    app_db_entry.stage = STAGE_INGRESS;
    app_db_entry.priority = 234;
    app_db_entry.meter_unit = P4_METER_UNIT_BYTES;
    app_db_entry.counter_unit = P4_COUNTER_UNIT_BYTES;
    // Match field mapping from P4 program to SAI entry attribute
    app_db_entry.match_field_lookup["ether_type"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_ETHER_TYPE);
    app_db_entry.match_field_lookup["ether_dst"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_DST_MAC, P4_FORMAT_MAC);
    app_db_entry.match_field_lookup["ether_src"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_SRC_MAC, P4_FORMAT_MAC);
    app_db_entry.match_field_lookup["ipv6_dst"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_DST_IPV6, P4_FORMAT_IPV6);
    app_db_entry.match_field_lookup["ipv6_next_header"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_IPV6_NEXT_HEADER);
    app_db_entry.match_field_lookup["ttl"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_TTL);
    app_db_entry.match_field_lookup["in_ports"] =
        BuildMatchFieldJsonStrKindSaiField(P4_MATCH_IN_PORTS, P4_FORMAT_STRING);
    app_db_entry.match_field_lookup["in_port"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_IN_PORT, P4_FORMAT_STRING);
    app_db_entry.match_field_lookup["out_ports"] =
        BuildMatchFieldJsonStrKindSaiField(P4_MATCH_OUT_PORTS, P4_FORMAT_STRING);
    app_db_entry.match_field_lookup["out_port"] =
        BuildMatchFieldJsonStrKindSaiField(P4_MATCH_OUT_PORT, P4_FORMAT_STRING);
    app_db_entry.match_field_lookup["is_ip"] =
        BuildMatchFieldJsonStrKindSaiField(std::string(P4_MATCH_IP_TYPE) + kFieldDelimiter + P4_IP_TYPE_BIT_IP);
    app_db_entry.match_field_lookup["is_ipv4"] =
        BuildMatchFieldJsonStrKindSaiField(std::string(P4_MATCH_IP_TYPE) + kFieldDelimiter + P4_IP_TYPE_BIT_IPV4ANY);
    app_db_entry.match_field_lookup["is_ipv6"] =
        BuildMatchFieldJsonStrKindSaiField(std::string(P4_MATCH_IP_TYPE) + kFieldDelimiter + P4_IP_TYPE_BIT_IPV6ANY);
    app_db_entry.match_field_lookup["is_arp"] =
        BuildMatchFieldJsonStrKindSaiField(std::string(P4_MATCH_IP_TYPE) + kFieldDelimiter + P4_IP_TYPE_BIT_ARP);
    app_db_entry.match_field_lookup["is_arp_request"] = BuildMatchFieldJsonStrKindSaiField(
        std::string(P4_MATCH_IP_TYPE) + kFieldDelimiter + P4_IP_TYPE_BIT_ARP_REQUEST);
    app_db_entry.match_field_lookup["is_arp_reply"] =
        BuildMatchFieldJsonStrKindSaiField(std::string(P4_MATCH_IP_TYPE) + kFieldDelimiter + P4_IP_TYPE_BIT_ARP_REPLY);
    app_db_entry.match_field_lookup["tcp_flags"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_TCP_FLAGS);
    app_db_entry.match_field_lookup["ip_flags"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_IP_FLAGS);
    app_db_entry.match_field_lookup["l4_src_port"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_L4_SRC_PORT);
    app_db_entry.match_field_lookup["l4_dst_port"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_L4_DST_PORT);
    app_db_entry.match_field_lookup["ip_id"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_IP_ID);
    app_db_entry.match_field_lookup["inner_l4_src_port"] =
        BuildMatchFieldJsonStrKindSaiField(P4_MATCH_INNER_L4_SRC_PORT);
    app_db_entry.match_field_lookup["inner_l4_dst_port"] =
        BuildMatchFieldJsonStrKindSaiField(P4_MATCH_INNER_L4_DST_PORT);
    app_db_entry.match_field_lookup["dscp"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_DSCP);
    app_db_entry.match_field_lookup["inner_ip_src"] =
        BuildMatchFieldJsonStrKindSaiField(P4_MATCH_INNER_SRC_IP, P4_FORMAT_IPV4);
    app_db_entry.match_field_lookup["inner_ip_dst"] =
        BuildMatchFieldJsonStrKindSaiField(P4_MATCH_INNER_DST_IP, P4_FORMAT_IPV4);
    app_db_entry.match_field_lookup["inner_ipv6_src"] =
        BuildMatchFieldJsonStrKindSaiField(P4_MATCH_INNER_SRC_IPV6, P4_FORMAT_IPV6);
    app_db_entry.match_field_lookup["inner_ipv6_dst"] =
        BuildMatchFieldJsonStrKindSaiField(P4_MATCH_INNER_DST_IPV6, P4_FORMAT_IPV6);
    app_db_entry.match_field_lookup["ip_src"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_SRC_IP, P4_FORMAT_IPV4);
    app_db_entry.match_field_lookup["ip_dst"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_DST_IP, P4_FORMAT_IPV4);
    app_db_entry.match_field_lookup["ipv6_src"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_SRC_IPV6, P4_FORMAT_IPV6);
    app_db_entry.match_field_lookup["tc"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_TRAFFIC_CLASS);
    app_db_entry.match_field_lookup["icmp_type"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_ICMP_TYPE);
    app_db_entry.match_field_lookup["icmp_code"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_ICMP_CODE);
    app_db_entry.match_field_lookup["tos"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_TOS);
    app_db_entry.match_field_lookup["icmpv6_type"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_ICMPV6_TYPE);
    app_db_entry.match_field_lookup["icmpv6_code"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_ICMPV6_CODE);
    app_db_entry.match_field_lookup["ecn"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_ECN);
    app_db_entry.match_field_lookup["inner_ip_protocol"] =
        BuildMatchFieldJsonStrKindSaiField(P4_MATCH_INNER_IP_PROTOCOL);
    app_db_entry.match_field_lookup["ip_protocol"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_IP_PROTOCOL);
    app_db_entry.match_field_lookup["ipv6_flow_label"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_IPV6_FLOW_LABEL);
    app_db_entry.match_field_lookup["tunnel_vni"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_TUNNEL_VNI);
    app_db_entry.match_field_lookup["ip_frag"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_IP_FRAG, P4_FORMAT_STRING);
    app_db_entry.match_field_lookup["packet_vlan"] =
        BuildMatchFieldJsonStrKindSaiField(P4_MATCH_PACKET_VLAN, P4_FORMAT_STRING);
    app_db_entry.match_field_lookup["outer_vlan_pri"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_OUTER_VLAN_PRI);
    app_db_entry.match_field_lookup["outer_vlan_id"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_OUTER_VLAN_ID);
    app_db_entry.match_field_lookup["outer_vlan_cfi"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_OUTER_VLAN_CFI);
    app_db_entry.match_field_lookup["inner_vlan_pri"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_INNER_VLAN_PRI);
    app_db_entry.match_field_lookup["inner_vlan_id"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_INNER_VLAN_ID);
    app_db_entry.match_field_lookup["inner_vlan_cfi"] = BuildMatchFieldJsonStrKindSaiField(P4_MATCH_INNER_VLAN_CFI);
    app_db_entry.match_field_lookup["src_ipv6_64bit"] = BuildMatchFieldJsonStrKindComposite(
        {nlohmann::json::parse(BuildMatchFieldJsonStrKindSaiField(P4_MATCH_SRC_IPV6_WORD3, P4_FORMAT_IPV6, 32)),
         nlohmann::json::parse(BuildMatchFieldJsonStrKindSaiField(P4_MATCH_SRC_IPV6_WORD2, P4_FORMAT_IPV6, 32))},
        P4_FORMAT_IPV6, 64);
    app_db_entry.match_field_lookup["arp_tpa"] = BuildMatchFieldJsonStrKindComposite(
        {nlohmann::json::parse(BuildMatchFieldJsonStrKindUdf("SAI_UDF_BASE_L3", 24, P4_FORMAT_HEX_STRING, 16)),
         nlohmann::json::parse(BuildMatchFieldJsonStrKindUdf("SAI_UDF_BASE_L3", 26, P4_FORMAT_HEX_STRING, 16))},
        P4_FORMAT_HEX_STRING, 32);
    app_db_entry.match_field_lookup["udf2"] =
        BuildMatchFieldJsonStrKindUdf("SAI_UDF_BASE_L3", 56, P4_FORMAT_HEX_STRING, 16);

    // Action field mapping, from P4 action to SAI action
    app_db_entry.action_field_lookup["set_packet_action"].push_back(
        {.sai_action = P4_ACTION_PACKET_ACTION, .p4_param_name = "packet_action"});
    app_db_entry.action_field_lookup["copy_and_set_tc"].push_back(
        {.sai_action = P4_ACTION_SET_TRAFFIC_CLASS, .p4_param_name = "traffic_class"});
    app_db_entry.action_field_lookup["punt_and_set_tc"].push_back(
        {.sai_action = P4_ACTION_SET_TRAFFIC_CLASS, .p4_param_name = "traffic_class"});
    app_db_entry.packet_action_color_lookup["copy_and_set_tc"].push_back(
        {.packet_action = P4_PACKET_ACTION_COPY, .packet_color = P4_PACKET_COLOR_GREEN});
    app_db_entry.packet_action_color_lookup["punt_and_set_tc"].push_back(
        {.packet_action = P4_PACKET_ACTION_PUNT, .packet_color = EMPTY_STRING});
    app_db_entry.packet_action_color_lookup["punt_non_green_pk"].push_back(
        {.packet_action = P4_PACKET_ACTION_PUNT, .packet_color = P4_PACKET_COLOR_YELLOW});
    app_db_entry.packet_action_color_lookup["punt_non_green_pk"].push_back(
        {.packet_action = P4_PACKET_ACTION_PUNT, .packet_color = P4_PACKET_COLOR_RED});
    app_db_entry.action_field_lookup["redirect"].push_back(
        {.sai_action = P4_ACTION_REDIRECT, .p4_param_name = "target"});
    app_db_entry.action_field_lookup["endpoint_ip"].push_back(
        {.sai_action = P4_ACTION_ENDPOINT_IP, .p4_param_name = "ip_address"});
    app_db_entry.action_field_lookup["mirror_ingress"].push_back(
        {.sai_action = P4_ACTION_MIRROR_INGRESS, .p4_param_name = "target"});
    app_db_entry.action_field_lookup["mirror_egress"].push_back(
        {.sai_action = P4_ACTION_MIRROR_EGRESS, .p4_param_name = "target"});
    app_db_entry.action_field_lookup["set_packet_color"].push_back(
        {.sai_action = P4_ACTION_SET_PACKET_COLOR, .p4_param_name = "packet_color"});
    app_db_entry.action_field_lookup["set_src_mac"].push_back(
        {.sai_action = P4_ACTION_SET_SRC_MAC, .p4_param_name = "mac_address"});
    app_db_entry.action_field_lookup["set_dst_mac"].push_back(
        {.sai_action = P4_ACTION_SET_DST_MAC, .p4_param_name = "mac_address"});
    app_db_entry.action_field_lookup["set_src_ip"].push_back(
        {.sai_action = P4_ACTION_SET_SRC_IP, .p4_param_name = "ip_address"});
    app_db_entry.action_field_lookup["set_dst_ip"].push_back(
        {.sai_action = P4_ACTION_SET_DST_IP, .p4_param_name = "ip_address"});
    app_db_entry.action_field_lookup["set_src_ipv6"].push_back(
        {.sai_action = P4_ACTION_SET_SRC_IPV6, .p4_param_name = "ip_address"});
    app_db_entry.action_field_lookup["set_dst_ipv6"].push_back(
        {.sai_action = P4_ACTION_SET_DST_IPV6, .p4_param_name = "ip_address"});
    app_db_entry.action_field_lookup["set_dscp_and_ecn"].push_back(
        {.sai_action = P4_ACTION_SET_DSCP, .p4_param_name = "dscp"});
    app_db_entry.action_field_lookup["set_dscp_and_ecn"].push_back(
        {.sai_action = P4_ACTION_SET_ECN, .p4_param_name = "ecn"});
    app_db_entry.action_field_lookup["set_inner_vlan"].push_back(
        {.sai_action = P4_ACTION_SET_INNER_VLAN_PRIORITY, .p4_param_name = "vlan_pri"});
    app_db_entry.action_field_lookup["set_inner_vlan"].push_back(
        {.sai_action = P4_ACTION_SET_INNER_VLAN_ID, .p4_param_name = "vlan_id"});
    app_db_entry.action_field_lookup["set_outer_vlan"].push_back(
        {.sai_action = P4_ACTION_SET_OUTER_VLAN_PRIORITY, .p4_param_name = "vlan_pri"});
    app_db_entry.action_field_lookup["set_outer_vlan"].push_back(
        {.sai_action = P4_ACTION_SET_OUTER_VLAN_ID, .p4_param_name = "vlan_id"});
    app_db_entry.action_field_lookup["set_l4_src_port"].push_back(
        {.sai_action = P4_ACTION_SET_L4_SRC_PORT, .p4_param_name = "port"});
    app_db_entry.action_field_lookup["set_l4_dst_port"].push_back(
        {.sai_action = P4_ACTION_SET_L4_DST_PORT, .p4_param_name = "port"});
    app_db_entry.action_field_lookup["flood"].push_back({.sai_action = P4_ACTION_FLOOD, .p4_param_name = EMPTY_STRING});
    app_db_entry.action_field_lookup["decrement_ttl"].push_back(
        {.sai_action = P4_ACTION_DECREMENT_TTL, .p4_param_name = EMPTY_STRING});
    app_db_entry.action_field_lookup["do_not_learn"].push_back(
        {.sai_action = P4_ACTION_SET_DO_NOT_LEARN, .p4_param_name = EMPTY_STRING});
    app_db_entry.action_field_lookup["set_vrf"].push_back({.sai_action = P4_ACTION_SET_VRF, .p4_param_name = "vrf"});
    app_db_entry.action_field_lookup["qos_queue"].push_back(
        {.sai_action = P4_ACTION_SET_QOS_QUEUE, .p4_param_name = "cpu_queue"});
    return app_db_entry;
}

std::vector<swss::FieldValueTuple> getDefaultRuleFieldValueTuples()
{
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{kAction, "copy_and_set_tc"});
    attributes.push_back(swss::FieldValueTuple{"param/traffic_class", "0x20"});
    attributes.push_back(swss::FieldValueTuple{"meter/cir", "80"});
    attributes.push_back(swss::FieldValueTuple{"meter/cburst", "80"});
    attributes.push_back(swss::FieldValueTuple{"meter/pir", "200"});
    attributes.push_back(swss::FieldValueTuple{"meter/pburst", "200"});
    attributes.push_back(swss::FieldValueTuple{"controller_metadata", "..."});
    return attributes;
}

P4AclRuleAppDbEntry getDefaultAclRuleAppDbEntryWithoutAction()
{
    P4AclRuleAppDbEntry app_db_entry;
    app_db_entry.acl_table_name = kAclIngressTableName;
    app_db_entry.priority = 100;
    // ACL rule match fields
    app_db_entry.match_fvs["ether_type"] = "0x0800";
    app_db_entry.match_fvs["ipv6_dst"] = "fdf8:f53b:82e4::53";
    app_db_entry.match_fvs["ether_dst"] = "AA:BB:CC:DD:EE:FF";
    app_db_entry.match_fvs["ether_src"] = "AA:BB:CC:DD:EE:FF";
    app_db_entry.match_fvs["ipv6_next_header"] = "1";
    app_db_entry.match_fvs["src_ipv6_64bit"] = "fdf8:f53b:82e4::";
    app_db_entry.match_fvs["arp_tpa"] = "0xff112231";
    app_db_entry.match_fvs["udf2"] = "0x9876 & 0xAAAA";
    app_db_entry.db_key = "ACL_PUNT_TABLE:{\"match/ether_type\": \"0x0800\",\"match/ipv6_dst\": "
                          "\"fdf8:f53b:82e4::53\",\"match/ether_dst\": \"AA:BB:CC:DD:EE:FF\", "
                          "\"match/ether_src\": \"AA:BB:CC:DD:EE:FF\", \"match/ipv6_next_header\": "
                          "\"1\", \"match/src_ipv6_64bit\": "
                          "\"fdf8:f53b:82e4::\",\"match/arp_tpa\": \"0xff11223\",\"match/udf2\": "
                          "\"0x9876 & 0xAAAA\",\"priority\":100}";
    // ACL meter fields
    app_db_entry.meter.enabled = true;
    app_db_entry.meter.cir = 80;
    app_db_entry.meter.cburst = 80;
    app_db_entry.meter.pir = 200;
    app_db_entry.meter.pburst = 200;
    return app_db_entry;
}

const std::string concatTableNameAndRuleKey(const std::string &table_name, const std::string &rule_key)
{
    return table_name + kTableKeyDelimiter + rule_key;
}

} // namespace

class AclManagerTest : public ::testing::Test
{
  protected:
    AclManagerTest()
    {
        setUpMockApi();
        setUpCoppOrch();
        setUpSwitchOrch();
        setUpP4Orch();
        // const auto& acl_groups = gSwitchOrch->getAclGroupOidsBindingToSwitch();
        // EXPECT_EQ(3, acl_groups.size());
        // EXPECT_NE(acl_groups.end(), acl_groups.find(SAI_ACL_STAGE_INGRESS));
        // EXPECT_EQ(kAclGroupIngressOid, acl_groups.at(SAI_ACL_STAGE_INGRESS));
        // EXPECT_NE(acl_groups.end(), acl_groups.find(SAI_ACL_STAGE_EGRESS));
        // EXPECT_EQ(kAclGroupEgressOid, acl_groups.at(SAI_ACL_STAGE_EGRESS));
        // EXPECT_NE(acl_groups.end(), acl_groups.find(SAI_ACL_STAGE_PRE_INGRESS));
        // EXPECT_EQ(kAclGroupLookupOid, acl_groups.at(SAI_ACL_STAGE_PRE_INGRESS));
        p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_MIRROR_SESSION, KeyGenerator::generateMirrorSessionKey(gMirrorSession1),
                               kMirrorSessionOid1);
        p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_MIRROR_SESSION, KeyGenerator::generateMirrorSessionKey(gMirrorSession2),
                               kMirrorSessionOid2);
    }

    ~AclManagerTest()
    {
        cleanupAclManagerTest();
    }

    void cleanupAclManagerTest()
    {
        EXPECT_CALL(mock_sai_udf_, remove_udf_match(_)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
        delete gP4Orch;
        delete copp_orch_;
        delete gSwitchOrch;
    }

    void setUpMockApi()
    {
        mock_sai_acl = &mock_sai_acl_;
        mock_sai_serialize = &mock_sai_serialize_;
        mock_sai_policer = &mock_sai_policer_;
        mock_sai_hostif = &mock_sai_hostif_;
        mock_sai_switch = &mock_sai_switch_;
        mock_sai_udf = &mock_sai_udf_;
        sai_acl_api->create_acl_table = create_acl_table;
        sai_acl_api->remove_acl_table = remove_acl_table;
        sai_acl_api->create_acl_table_group = create_acl_table_group;
        sai_acl_api->remove_acl_table_group = remove_acl_table_group;
        sai_acl_api->create_acl_table_group_member = create_acl_table_group_member;
        sai_acl_api->remove_acl_table_group_member = remove_acl_table_group_member;
        sai_acl_api->get_acl_counter_attribute = get_acl_counter_attribute;
        sai_acl_api->create_acl_entry = create_acl_entry;
        sai_acl_api->remove_acl_entry = remove_acl_entry;
        sai_acl_api->set_acl_entry_attribute = set_acl_entry_attribute;
        sai_acl_api->create_acl_counter = create_acl_counter;
        sai_acl_api->remove_acl_counter = remove_acl_counter;
        sai_policer_api->create_policer = create_policer;
        sai_policer_api->remove_policer = remove_policer;
        sai_policer_api->get_policer_stats = get_policer_stats;
        sai_policer_api->set_policer_attribute = set_policer_attribute;
        sai_hostif_api->create_hostif_table_entry = mock_create_hostif_table_entry;
        sai_hostif_api->remove_hostif_table_entry = mock_remove_hostif_table_entry;
        sai_hostif_api->create_hostif_trap_group = mock_create_hostif_trap_group;
        sai_hostif_api->create_hostif_trap = mock_create_hostif_trap;
        sai_hostif_api->create_hostif = mock_create_hostif;
        sai_hostif_api->remove_hostif = mock_remove_hostif;
        sai_hostif_api->create_hostif_user_defined_trap = mock_create_hostif_user_defined_trap;
        sai_hostif_api->remove_hostif_user_defined_trap = mock_remove_hostif_user_defined_trap;
        sai_switch_api->get_switch_attribute = mock_get_switch_attribute;
        sai_switch_api->set_switch_attribute = mock_set_switch_attribute;
        sai_udf_api->remove_udf = remove_udf;
        sai_udf_api->create_udf = create_udf;
        sai_udf_api->remove_udf_group = remove_udf_group;
        sai_udf_api->create_udf_group = create_udf_group;
        sai_udf_api->remove_udf_match = remove_udf_match;
        sai_udf_api->create_udf_match = create_udf_match;
    }

    void setUpCoppOrch()
    {
        // init copp orch
        EXPECT_CALL(mock_sai_hostif_, create_hostif_table_entry(_, _, _, _)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
        EXPECT_CALL(mock_sai_hostif_, create_hostif_trap(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
        EXPECT_CALL(mock_sai_switch_, get_switch_attribute(_, _, _)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
        copp_orch_ = new CoppOrch(gAppDb, APP_COPP_TABLE_NAME);
        // add trap group and genetlink for each CPU queue
        swss::Table app_copp_table(gAppDb, APP_COPP_TABLE_NAME);
        for (uint64_t queue_num = 1; queue_num <= P4_CPU_QUEUE_MAX_NUM; queue_num++)
        {
            std::vector<swss::FieldValueTuple> attrs;
            attrs.push_back({"queue", std::to_string(queue_num)});
            attrs.push_back({"genetlink_name", "genl_packet"});
            attrs.push_back({"genetlink_mcgrp_name", "packets"});
            app_copp_table.set(GENL_PACKET_TRAP_GROUP_NAME_PREFIX + std::to_string(queue_num), attrs);
        }
        sai_object_id_t trap_group_oid = gTrapGroupStartOid;
        sai_object_id_t hostif_oid = gHostifStartOid;
        EXPECT_CALL(mock_sai_hostif_, create_hostif_trap_group(_, _, _, _))
            .Times(P4_CPU_QUEUE_MAX_NUM)
            .WillRepeatedly(
                DoAll(Invoke([&trap_group_oid](sai_object_id_t *oid, sai_object_id_t switch_id, uint32_t attr_count,
                                               const sai_attribute_t *attr_list) { *oid = ++trap_group_oid; }),
                      Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(mock_sai_hostif_, create_hostif(_, _, _, _))
            .Times(P4_CPU_QUEUE_MAX_NUM)
            .WillRepeatedly(
                DoAll(Invoke([&hostif_oid](sai_object_id_t *oid, sai_object_id_t switch_id, uint32_t attr_count,
                                           const sai_attribute_t *attr_list) { *oid = ++hostif_oid; }),
                      Return(SAI_STATUS_SUCCESS)));

        copp_orch_->addExistingData(&app_copp_table);
        static_cast<Orch *>(copp_orch_)->doTask();
    }

    void setUpSwitchOrch()
    {
        EXPECT_CALL(mock_sai_serialize_, sai_serialize_object_id(_)).WillRepeatedly(Return(EMPTY_STRING));
        TableConnector stateDbSwitchTable(gStateDb, "SWITCH_CAPABILITY");
        TableConnector app_switch_table(gAppDb, APP_SWITCH_TABLE_NAME);
        TableConnector conf_asic_sensors(gConfigDb, CFG_ASIC_SENSORS_TABLE_NAME);
        std::vector<TableConnector> switch_tables = {conf_asic_sensors, app_switch_table};
        gSwitchOrch = new SwitchOrch(gAppDb, switch_tables, stateDbSwitchTable);
    }

    void setUpP4Orch()
    {
        EXPECT_CALL(mock_sai_serialize_, sai_serialize_object_id(_)).WillRepeatedly(Return(EMPTY_STRING));
        EXPECT_CALL(mock_sai_acl_,
                    create_acl_table_group(
                        _, Eq(gSwitchId), Eq(3),
                        Truly(std::bind(MatchSaiAttributeAclGroupStage, SAI_ACL_STAGE_INGRESS, std::placeholders::_1))))
            .WillRepeatedly(DoAll(SetArgPointee<0>(kAclGroupIngressOid), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(mock_sai_acl_,
                    create_acl_table_group(
                        _, Eq(gSwitchId), Eq(3),
                        Truly(std::bind(MatchSaiAttributeAclGroupStage, SAI_ACL_STAGE_EGRESS, std::placeholders::_1))))
            .WillRepeatedly(DoAll(SetArgPointee<0>(kAclGroupEgressOid), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(mock_sai_acl_,
                    create_acl_table_group(_, Eq(gSwitchId), Eq(3),
                                           Truly(std::bind(MatchSaiAttributeAclGroupStage, SAI_ACL_STAGE_PRE_INGRESS,
                                                           std::placeholders::_1))))
            .WillRepeatedly(DoAll(SetArgPointee<0>(kAclGroupLookupOid), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(mock_sai_switch_,
                    set_switch_attribute(Eq(gSwitchId),
                                         Truly(std::bind(MatchSaiSwitchAttrByAclStage, SAI_SWITCH_ATTR_INGRESS_ACL,
                                                         kAclGroupIngressOid, std::placeholders::_1))))
            .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
        EXPECT_CALL(mock_sai_switch_,
                    set_switch_attribute(Eq(gSwitchId),
                                         Truly(std::bind(MatchSaiSwitchAttrByAclStage, SAI_SWITCH_ATTR_EGRESS_ACL,
                                                         kAclGroupEgressOid, std::placeholders::_1))))
            .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
        EXPECT_CALL(mock_sai_switch_,
                    set_switch_attribute(Eq(gSwitchId),
                                         Truly(std::bind(MatchSaiSwitchAttrByAclStage, SAI_SWITCH_ATTR_PRE_INGRESS_ACL,
                                                         kAclGroupLookupOid, std::placeholders::_1))))
            .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
        EXPECT_CALL(mock_sai_udf_, create_udf_match(_, _, _, _))
            .WillOnce(DoAll(SetArgPointee<0>(kUdfMatchOid1), Return(SAI_STATUS_SUCCESS)));
        std::vector<std::string> p4_tables;
        gP4Orch = new P4Orch(gAppDb, p4_tables, gVrfOrch, copp_orch_);
        acl_table_manager_ = gP4Orch->getAclTableManager();
        acl_rule_manager_ = gP4Orch->getAclRuleManager();
        p4_oid_mapper_ = acl_table_manager_->m_p4OidMapper;
    }

    void AddDefaultUserTrapsSaiCalls(sai_object_id_t *user_defined_trap_oid)
    {
        EXPECT_CALL(mock_sai_hostif_, create_hostif_user_defined_trap(_, _, _, _))
            .Times(P4_CPU_QUEUE_MAX_NUM)
            .WillRepeatedly(DoAll(Invoke([user_defined_trap_oid](
                                             sai_object_id_t *oid, sai_object_id_t switch_id, uint32_t attr_count,
                                             const sai_attribute_t *attr_list) { *oid = ++(*user_defined_trap_oid); }),
                                  Return(SAI_STATUS_SUCCESS)));
    }

    void AddDefaultIngressTable()
    {
        const auto &app_db_entry = getDefaultAclTableDefAppDbEntry();
        EXPECT_CALL(mock_sai_acl_, create_acl_table(_, _, _, _))
            .WillOnce(DoAll(SetArgPointee<0>(kAclTableIngressOid), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(mock_sai_acl_, create_acl_table_group_member(_, _, _, _))
            .WillOnce(DoAll(SetArgPointee<0>(kAclGroupMemberIngressOid), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(mock_sai_udf_, create_udf_group(_, _, _, _))
            .Times(3)
            .WillRepeatedly(DoAll(SetArgPointee<0>(kUdfGroupOid1), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(mock_sai_udf_, create_udf(_, _, _, _)).Times(3).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
        sai_object_id_t user_defined_trap_oid = gUserDefinedTrapStartOid;
        AddDefaultUserTrapsSaiCalls(&user_defined_trap_oid);
        ASSERT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddTableRequest(app_db_entry));
        ASSERT_NO_FATAL_FAILURE(
            IsExpectedAclTableDefinitionMapping(*GetAclTable(app_db_entry.acl_table_name), app_db_entry));
    }

    void DrainTableTuples()
    {
        acl_table_manager_->drain();
    }
    void EnqueueTableTuple(const swss::KeyOpFieldsValuesTuple &entry)
    {
        acl_table_manager_->enqueue(entry);
    }

    void DrainRuleTuples()
    {
        acl_rule_manager_->drain();
    }
    void EnqueueRuleTuple(const swss::KeyOpFieldsValuesTuple &entry)
    {
        acl_rule_manager_->enqueue(entry);
    }

    ReturnCodeOr<P4AclTableDefinitionAppDbEntry> DeserializeAclTableDefinitionAppDbEntry(
        const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
    {
        return acl_table_manager_->deserializeAclTableDefinitionAppDbEntry(key, attributes);
    }

    ReturnCodeOr<P4AclRuleAppDbEntry> DeserializeAclRuleAppDbEntry(const std::string &acl_table_name,
                                                                   const std::string &key,
                                                                   const std::vector<swss::FieldValueTuple> &attributes)
    {
        return acl_rule_manager_->deserializeAclRuleAppDbEntry(acl_table_name, key, attributes);
    }

    P4AclTableDefinition *GetAclTable(const std::string &acl_table_name)
    {
        return acl_table_manager_->getAclTable(acl_table_name);
    }

    P4AclRule *GetAclRule(const std::string &acl_table_name, const std::string &acl_rule_key)
    {
        return acl_rule_manager_->getAclRule(acl_table_name, acl_rule_key);
    }

    ReturnCode ProcessAddTableRequest(const P4AclTableDefinitionAppDbEntry &app_db_entry)
    {
        return acl_table_manager_->processAddTableRequest(app_db_entry);
    }

    ReturnCode ProcessDeleteTableRequest(const std::string &acl_table_name)
    {
        return acl_table_manager_->processDeleteTableRequest(acl_table_name);
    }

    ReturnCode ProcessAddRuleRequest(const std::string &acl_rule_key, const P4AclRuleAppDbEntry &app_db_entry)
    {
        return acl_rule_manager_->processAddRuleRequest(acl_rule_key, app_db_entry);
    }

    ReturnCode ProcessUpdateRuleRequest(const P4AclRuleAppDbEntry &app_db_entry, const P4AclRule &old_acl_rule)
    {
        return acl_rule_manager_->processUpdateRuleRequest(app_db_entry, old_acl_rule);
    }

    ReturnCode ProcessDeleteRuleRequest(const std::string &acl_table_name, const std::string &acl_rule_key)
    {
        return acl_rule_manager_->processDeleteRuleRequest(acl_table_name, acl_rule_key);
    }

    void DoAclCounterStatsTask()
    {
        acl_rule_manager_->doAclCounterStatsTask();
    }

    StrictMock<MockSaiAcl> mock_sai_acl_;
    StrictMock<MockSaiSerialize> mock_sai_serialize_;
    StrictMock<MockSaiPolicer> mock_sai_policer_;
    StrictMock<MockSaiHostif> mock_sai_hostif_;
    StrictMock<MockSaiSwitch> mock_sai_switch_;
    StrictMock<MockSaiUdf> mock_sai_udf_;
    CoppOrch *copp_orch_;
    P4OidMapper *p4_oid_mapper_;
    p4orch::AclTableManager *acl_table_manager_;
    p4orch::AclRuleManager *acl_rule_manager_;
};

TEST_F(AclManagerTest, DrainTableTuplesToProcessSetDelRequestSucceeds)
{
    const auto &p4rtAclTableName =
        std::string(APP_P4RT_ACL_TABLE_DEFINITION_NAME) + kTableKeyDelimiter + kAclIngressTableName;
    EnqueueTableTuple(
        swss::KeyOpFieldsValuesTuple({p4rtAclTableName, SET_COMMAND, getDefaultTableDefFieldValueTuples()}));

    // Drain table tuples to process SET request
    EXPECT_CALL(mock_sai_acl_, create_acl_table(_, Eq(gSwitchId), Gt(2),
                                                Truly(std::bind(MatchSaiAttributeAclTableStage, SAI_ACL_STAGE_INGRESS,
                                                                std::placeholders::_1))))
        .WillOnce(DoAll(SetArgPointee<0>(kAclTableIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group_member(_, Eq(gSwitchId), Eq(3), NotNull()))
        .WillOnce(DoAll(SetArgPointee<0>(kAclGroupMemberIngressOid), Return(SAI_STATUS_SUCCESS)));
    DrainTableTuples();
    EXPECT_NE(nullptr, GetAclTable(kAclIngressTableName));

    // Drain table tuples to process DEL request
    EXPECT_CALL(mock_sai_acl_, remove_acl_table(Eq(kAclTableIngressOid))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table_group_member(Eq(kAclGroupMemberIngressOid)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EnqueueTableTuple(swss::KeyOpFieldsValuesTuple({p4rtAclTableName, DEL_COMMAND, {}}));
    DrainTableTuples();
    EXPECT_EQ(nullptr, GetAclTable(kAclIngressTableName));
}

TEST_F(AclManagerTest, DrainTableTuplesToProcessUpdateRequestExpectFails)
{
    const auto &p4rtAclTableName =
        std::string(APP_P4RT_ACL_TABLE_DEFINITION_NAME) + kTableKeyDelimiter + kAclIngressTableName;
    auto attributes = getDefaultTableDefFieldValueTuples();
    EnqueueTableTuple(swss::KeyOpFieldsValuesTuple({p4rtAclTableName, SET_COMMAND, attributes}));

    // Drain table tuples to process SET request, create a table with priority
    // 234
    EXPECT_CALL(mock_sai_acl_, create_acl_table(_, Eq(gSwitchId), Gt(2),
                                                Truly(std::bind(MatchSaiAttributeAclTableStage, SAI_ACL_STAGE_INGRESS,
                                                                std::placeholders::_1))))
        .WillOnce(DoAll(SetArgPointee<0>(kAclTableIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group_member(_, Eq(gSwitchId), Eq(3), NotNull()))
        .WillOnce(DoAll(SetArgPointee<0>(kAclGroupMemberIngressOid), Return(SAI_STATUS_SUCCESS)));
    DrainTableTuples();
    EXPECT_NE(nullptr, GetAclTable(kAclIngressTableName));

    // Drain table tuples to process SET request, try to update table priority
    // to 100: should fail to update.
    attributes.push_back(swss::FieldValueTuple{kPriority, "100"});
    EnqueueTableTuple(swss::KeyOpFieldsValuesTuple({p4rtAclTableName, SET_COMMAND, attributes}));
    DrainTableTuples();
    EXPECT_EQ(234, GetAclTable(kAclIngressTableName)->priority);
}

TEST_F(AclManagerTest, DrainTableTuplesWithInvalidTableNameOpsFails)
{
    auto p4rtAclTableName = std::string("UNDEFINED") + kTableKeyDelimiter + kAclIngressTableName;
    EnqueueTableTuple(
        swss::KeyOpFieldsValuesTuple({p4rtAclTableName, SET_COMMAND, getDefaultTableDefFieldValueTuples()}));
    // Drain table tuples to process SET request on invalid ACL definition table
    // name: "UNDEFINED"
    DrainTableTuples();
    EXPECT_EQ(nullptr, GetAclTable(kAclIngressTableName));

    p4rtAclTableName = std::string(APP_P4RT_ACL_TABLE_DEFINITION_NAME) + kTableKeyDelimiter + kAclIngressTableName;
    EnqueueTableTuple(swss::KeyOpFieldsValuesTuple({p4rtAclTableName, "UPDATE", getDefaultTableDefFieldValueTuples()}));
    // Drain table tuples to process invalid operation: "UPDATE"
    DrainTableTuples();
    EXPECT_EQ(nullptr, GetAclTable(kAclIngressTableName));
}

TEST_F(AclManagerTest, DrainTableTuplesWithInvalidFieldFails)
{
    auto attributes = getDefaultTableDefFieldValueTuples();
    const auto &p4rtAclTableName =
        std::string(APP_P4RT_ACL_TABLE_DEFINITION_NAME) + kTableKeyDelimiter + kAclIngressTableName;

    // Invalid attribute field
    attributes.push_back(swss::FieldValueTuple{"undefined", "undefined"});
    EnqueueTableTuple(swss::KeyOpFieldsValuesTuple({p4rtAclTableName, SET_COMMAND, attributes}));
    // Drain table tuples to process SET request
    DrainTableTuples();
    EXPECT_EQ(nullptr, GetAclTable(kAclIngressTableName));

    // Invalid attribute field
    attributes.pop_back();
    attributes.push_back(swss::FieldValueTuple{"undefined/undefined", "undefined"});
    EnqueueTableTuple(swss::KeyOpFieldsValuesTuple({p4rtAclTableName, SET_COMMAND, attributes}));
    // Drain table tuples to process SET request
    DrainTableTuples();
    EXPECT_EQ(nullptr, GetAclTable(kAclIngressTableName));

    // Invalid meter unit value
    attributes.pop_back();
    attributes.push_back(swss::FieldValueTuple{"meter/unit", "undefined"});
    EnqueueTableTuple(swss::KeyOpFieldsValuesTuple({p4rtAclTableName, SET_COMMAND, attributes}));
    // Drain table tuples to process SET request
    DrainTableTuples();
    EXPECT_EQ(nullptr, GetAclTable(kAclIngressTableName));

    // Invalid counter unit value
    attributes.pop_back();
    attributes.push_back(swss::FieldValueTuple{"counter/unit", "undefined"});
    EnqueueTableTuple(swss::KeyOpFieldsValuesTuple({p4rtAclTableName, SET_COMMAND, attributes}));
    // Drain table tuples to process SET request
    DrainTableTuples();
    EXPECT_EQ(nullptr, GetAclTable(kAclIngressTableName));
}

TEST_F(AclManagerTest, CreateIngressPuntTableSucceeds)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto acl_table = GetAclTable(kAclIngressTableName);
    EXPECT_NE(nullptr, acl_table);
}

TEST_F(AclManagerTest, CreatePuntTableFailsWhenUserTrapsSaiCallFails)
{
    auto app_db_entry = getDefaultAclTableDefAppDbEntry();
    EXPECT_CALL(mock_sai_hostif_, create_hostif_user_defined_trap(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(gUserDefinedTrapStartOid + 1), Return(SAI_STATUS_SUCCESS)))
        .WillOnce(Return(SAI_STATUS_INSUFFICIENT_RESOURCES));
    EXPECT_CALL(mock_sai_hostif_, create_hostif_table_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(gHostifStartOid + 1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_hostif_, remove_hostif_table_entry(Eq(gHostifStartOid + 1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_hostif_, remove_hostif_user_defined_trap(Eq(gUserDefinedTrapStartOid + 1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    // The user defined traps fail to create
    EXPECT_EQ(StatusCode::SWSS_RC_FULL, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));
    sai_object_id_t oid;
    EXPECT_FALSE(p4_oid_mapper_->getOID(SAI_OBJECT_TYPE_HOSTIF_USER_DEFINED_TRAP,
                                        std::to_string(gUserDefinedTrapStartOid + 1), &oid));

    EXPECT_CALL(mock_sai_hostif_, create_hostif_user_defined_trap(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(gUserDefinedTrapStartOid + 1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_hostif_, create_hostif_table_entry(_, _, _, _))
        .WillOnce(Return(SAI_STATUS_INSUFFICIENT_RESOURCES));
    EXPECT_CALL(mock_sai_hostif_, remove_hostif_user_defined_trap(Eq(gUserDefinedTrapStartOid + 1)))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    // The hostif table entry fails to create
    EXPECT_EQ(StatusCode::SWSS_RC_FULL, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    EXPECT_CALL(mock_sai_hostif_, create_hostif_user_defined_trap(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(gUserDefinedTrapStartOid + 1), Return(SAI_STATUS_SUCCESS)))
        .WillOnce(Return(SAI_STATUS_INSUFFICIENT_RESOURCES));
    EXPECT_CALL(mock_sai_hostif_, create_hostif_table_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(gHostifStartOid + 1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_hostif_, remove_hostif_table_entry(Eq(gHostifStartOid + 1)))
        .WillOnce(Return(SAI_STATUS_OBJECT_IN_USE));
    // The 2nd user defined trap fails to create, the 1st hostif table entry fails
    // to remove
    EXPECT_EQ(StatusCode::SWSS_RC_FULL, ProcessAddTableRequest(app_db_entry));
}

TEST_F(AclManagerTest, DISABLED_CreatePuntTableFailsWhenUserTrapGroupOrHostifNotFound)
{
    auto app_db_entry = getDefaultAclTableDefAppDbEntry();
    const auto skip_cpu_queue = 1;
    // init copp orch
    EXPECT_CALL(mock_sai_hostif_, create_hostif_table_entry(_, _, _, _)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_hostif_, create_hostif_trap(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_switch_, get_switch_attribute(_, _, _)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    swss::Table app_copp_table(gAppDb, APP_COPP_TABLE_NAME);
    // Clean up APP_COPP_TABLE_NAME table entries
    for (int queue_num = 1; queue_num <= P4_CPU_QUEUE_MAX_NUM; queue_num++)
    {
        app_copp_table.del(GENL_PACKET_TRAP_GROUP_NAME_PREFIX + std::to_string(queue_num));
    }
    cleanupAclManagerTest();
    copp_orch_ = new CoppOrch(gAppDb, APP_COPP_TABLE_NAME);
    setUpSwitchOrch();
    // Update p4orch to use new copp orch
    setUpP4Orch();
    // Fail to create ACL table because the trap group is absent
    EXPECT_EQ("Trap group was not found given trap group name: " + std::string(GENL_PACKET_TRAP_GROUP_NAME_PREFIX) +
                  std::to_string(skip_cpu_queue),
              ProcessAddTableRequest(app_db_entry).message());
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Create the trap group for CPU queue 1 without host interface(genl
    // attributes)
    std::vector<swss::FieldValueTuple> attrs;
    attrs.push_back({"queue", std::to_string(skip_cpu_queue)});
    // Add one COPP_TABLE entry with trap group info, without hostif info
    app_copp_table.set(GENL_PACKET_TRAP_GROUP_NAME_PREFIX + std::to_string(skip_cpu_queue), attrs);
    copp_orch_->addExistingData(&app_copp_table);
    EXPECT_CALL(mock_sai_hostif_, create_hostif_trap_group(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(gTrapGroupStartOid + skip_cpu_queue), Return(SAI_STATUS_SUCCESS)));
    static_cast<Orch *>(copp_orch_)->doTask();
    // Fail to create ACL table because the host interface is absent
    EXPECT_EQ("Hostif object id was not found given trap group - " + std::string(GENL_PACKET_TRAP_GROUP_NAME_PREFIX) +
                  std::to_string(skip_cpu_queue),
              ProcessAddTableRequest(app_db_entry).message());
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));
}

TEST_F(AclManagerTest, CreateIngressPuntTableFailsWhenCapabilityExceeds)
{
    auto app_db_entry = getDefaultAclTableDefAppDbEntry();
    sai_object_id_t user_defined_trap_oid = gUserDefinedTrapStartOid;
    AddDefaultUserTrapsSaiCalls(&user_defined_trap_oid);
    EXPECT_CALL(mock_sai_udf_, create_udf_group(_, _, _, _))
        .WillRepeatedly(DoAll(SetArgPointee<0>(kUdfGroupOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_udf_, create_udf(_, _, _, _)).Times(3).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, create_acl_table(_, _, _, _)).WillOnce(Return(SAI_STATUS_INSUFFICIENT_RESOURCES));
    EXPECT_CALL(mock_sai_udf_, remove_udf_group(Eq(kUdfGroupOid1))).Times(3).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf(_)).Times(3).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_FULL, ProcessAddTableRequest(app_db_entry));
}

TEST_F(AclManagerTest, CreateIngressPuntTableFailsWhenFailedToCreateTableGroupMember)
{
    auto app_db_entry = getDefaultAclTableDefAppDbEntry();
    sai_object_id_t user_defined_trap_oid = gUserDefinedTrapStartOid;
    AddDefaultUserTrapsSaiCalls(&user_defined_trap_oid);
    EXPECT_CALL(mock_sai_udf_, create_udf_group(_, _, _, _))
        .WillRepeatedly(DoAll(SetArgPointee<0>(kUdfGroupOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_udf_, create_udf(_, _, _, _)).Times(3).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, create_acl_table(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclTableIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group_member(_, _, _, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf_group(Eq(kUdfGroupOid1))).Times(3).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf(_)).Times(3).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddTableRequest(app_db_entry));
}

TEST_F(AclManagerTest, CreateIngressPuntTableRaisesCriticalStateWhenAclTableRecoveryFails)
{
    auto app_db_entry = getDefaultAclTableDefAppDbEntry();
    sai_object_id_t user_defined_trap_oid = gUserDefinedTrapStartOid;
    AddDefaultUserTrapsSaiCalls(&user_defined_trap_oid);
    EXPECT_CALL(mock_sai_udf_, create_udf_group(_, _, _, _))
        .WillRepeatedly(DoAll(SetArgPointee<0>(kUdfGroupOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_udf_, create_udf(_, _, _, _)).Times(3).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, create_acl_table(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclTableIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group_member(_, _, _, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table(_)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_udf_, remove_udf_group(Eq(kUdfGroupOid1))).Times(3).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf(_)).Times(3).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddTableRequest(app_db_entry));
}

TEST_F(AclManagerTest, CreateIngressPuntTableRaisesCriticalStateWhenUdfGroupRecoveryFails)
{
    auto app_db_entry = getDefaultAclTableDefAppDbEntry();
    sai_object_id_t user_defined_trap_oid = gUserDefinedTrapStartOid;
    AddDefaultUserTrapsSaiCalls(&user_defined_trap_oid);
    EXPECT_CALL(mock_sai_udf_, create_udf_group(_, _, _, _))
        .WillRepeatedly(DoAll(SetArgPointee<0>(kUdfGroupOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_udf_, create_udf(_, _, _, _)).Times(3).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, create_acl_table(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclTableIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group_member(_, _, _, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf_group(Eq(kUdfGroupOid1))).Times(3).WillRepeatedly(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_udf_, remove_udf(_)).Times(3).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    // (TODO): Expect critical state x3.
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddTableRequest(app_db_entry));
}

TEST_F(AclManagerTest, CreateIngressPuntTableRaisesCriticalStateWhenUdfRecoveryFails)
{
    auto app_db_entry = getDefaultAclTableDefAppDbEntry();
    sai_object_id_t user_defined_trap_oid = gUserDefinedTrapStartOid;
    AddDefaultUserTrapsSaiCalls(&user_defined_trap_oid);
    EXPECT_CALL(mock_sai_udf_, create_udf_group(_, _, _, _))
        .WillRepeatedly(DoAll(SetArgPointee<0>(kUdfGroupOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_udf_, create_udf(_, _, _, _)).Times(3).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, create_acl_table(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclTableIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group_member(_, _, _, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    // UDF recovery failure will also cause UDF group recovery failure since the
    // reference count will not be zero if UDF failed to be removed.
    EXPECT_CALL(mock_sai_udf_, remove_udf(_)).Times(3).WillRepeatedly(Return(SAI_STATUS_FAILURE));
    // (TODO): Expect critical state x6.
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddTableRequest(app_db_entry));
}

TEST_F(AclManagerTest, CreateIngressPuntTableFailsWhenFailedToCreateUdf)
{
    auto app_db_entry = getDefaultAclTableDefAppDbEntry();
    sai_object_id_t user_defined_trap_oid = gUserDefinedTrapStartOid;
    AddDefaultUserTrapsSaiCalls(&user_defined_trap_oid);
    // Fail to create the first UDF, and success to remove the first UDF
    // group
    EXPECT_CALL(mock_sai_udf_, create_udf_group(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, create_udf(_, _, _, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_udf_, remove_udf_group(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddTableRequest(app_db_entry));
    EXPECT_FALSE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_UDF,
                                           std::string(kAclIngressTableName) + "-arp_tpa-0-base1-offset24"));
    EXPECT_FALSE(
        p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_UDF_GROUP, std::string(kAclIngressTableName) + "-arp_tpa-0"));

    // Fail to create the second UDF group, and success to remove the first UDF
    // group and UDF
    EXPECT_CALL(mock_sai_udf_, create_udf_group(_, _, _, _))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_udf_, create_udf(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf_group(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddTableRequest(app_db_entry));
    EXPECT_FALSE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_UDF,
                                           std::string(kAclIngressTableName) + "-arp_tpa-0-base1-offset24"));
    EXPECT_FALSE(
        p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_UDF_GROUP, std::string(kAclIngressTableName) + "-arp_tpa-0"));

    // Fail to create the second UDF group, and fail to remove the first UDF
    // group
    EXPECT_CALL(mock_sai_udf_, create_udf_group(_, _, _, _))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_udf_, create_udf(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf_group(_)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_udf_, remove_udf(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddTableRequest(app_db_entry));
    EXPECT_FALSE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_UDF,
                                           std::string(kAclIngressTableName) + "-arp_tpa-0-base1-offset24"));
    EXPECT_TRUE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_UDF_GROUP, std::string(kAclIngressTableName) + "-arp_tpa-0"));
    p4_oid_mapper_->eraseOID(SAI_OBJECT_TYPE_UDF_GROUP, std::string(kAclIngressTableName) + "-arp_tpa-0");

    // Fail to create the second UDF group, and fail to remove the first UDF and
    // UDF group
    EXPECT_CALL(mock_sai_udf_, create_udf_group(_, _, _, _))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_udf_, create_udf(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf(_)).WillOnce(Return(SAI_STATUS_FAILURE));
    // (TODO): Expect critical state x2.
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessAddTableRequest(app_db_entry));
    EXPECT_TRUE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_UDF,
                                          std::string(kAclIngressTableName) + "-arp_tpa-0-base1-offset24"));
    EXPECT_TRUE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_UDF_GROUP, std::string(kAclIngressTableName) + "-arp_tpa-0"));
}

TEST_F(AclManagerTest, CreatePuntTableWithInvalidStageFails)
{
    auto app_db_entry = getDefaultAclTableDefAppDbEntry();
    // Invalid stage
    app_db_entry.stage = "RANDOM";

    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));
}

TEST_F(AclManagerTest, CreatePuntTableWithInvalidSaiMatchFieldFails)
{
    auto app_db_entry = getDefaultAclTableDefAppDbEntry();
    // Invalid SAI match field
    app_db_entry.match_field_lookup["random"] = BuildMatchFieldJsonStrKindSaiField("RANDOM");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));
    app_db_entry.match_field_lookup.erase("random");

    // Invalid match field str - should be JSON str
    app_db_entry.match_field_lookup["ether_type"] = P4_MATCH_ETHER_TYPE;
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - should be object instead of array
    app_db_entry.match_field_lookup["ether_type"] = "[{\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_"
                                                    "ETHER_TYPE\",\"format\":\"HEX_STRING\",\"bitwidth\":8}]";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - missing kind
    app_db_entry.match_field_lookup["ether_type"] = "{\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE\",\"format\":"
                                                    "\"HEX_STRING\",\"bitwidth\":8}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - invalid kind
    app_db_entry.match_field_lookup["ether_type"] =
        "{\"kind\":\"field\",\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_ETHER_"
        "TYPE\",\"format\":\"HEX_STRING\",\"bitwidth\":8}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - missing format
    app_db_entry.match_field_lookup["ether_type"] = "{\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_"
                                                    "ETHER_TYPE\",\"bitwidth\":8}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - invalid format
    app_db_entry.match_field_lookup["ether_type"] = "{\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_"
                                                    "ETHER_TYPE\",\"format\":\"INVALID_TYPE\",\"bitwidth\":8}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - not expected format for the field
    app_db_entry.match_field_lookup["ether_type"] = "{\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_"
                                                    "ETHER_TYPE\",\"format\":\"IPV4\",\"bitwidth\":8}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - missing bitwidth
    app_db_entry.match_field_lookup["ether_type"] = "{\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_"
                                                    "ETHER_TYPE\",\"format\":\"HEX_STRING\"}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - missing sai_field
    app_db_entry.match_field_lookup["ether_type"] = "{\"kind\":\"sai_field\",\"format\":\"HEX_STRING\",\"bitwidth\":8}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Unsupported IP_TYPE bit type
    app_db_entry.match_field_lookup.erase("ether_type");
    app_db_entry.match_field_lookup["is_non_ip"] =
        BuildMatchFieldJsonStrKindSaiField(std::string(P4_MATCH_IP_TYPE) + kFieldDelimiter + "NONIP");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));
}

TEST_F(AclManagerTest, CreatePuntTableWithInvalidCompositeSaiMatchFieldFails)
{
    auto app_db_entry = getDefaultAclTableDefAppDbEntry();

    // Invalid match field str - missing format
    app_db_entry.match_field_lookup["src_ipv6_64bit"] =
        "{\"kind\":\"composite\",\"bitwidth\":64,"
        "\"elements\":[{\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_"
        "ATTR_FIELD_SRC_IPV6_WORD3\",\"bitwidth\":32},{\"kind\":\"sai_field\","
        "\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD2\",\"bitwidth\":"
        "32}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - invalid format
    app_db_entry.match_field_lookup["src_ipv6_64bit"] =
        "{\"kind\":\"composite\",\"format\":\"IPV4\",\"bitwidth\":64,"
        "\"elements\":[{\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_"
        "ATTR_FIELD_SRC_IPV6_WORD3\",\"bitwidth\":32},{\"kind\":\"sai_field\","
        "\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD2\",\"bitwidth\":"
        "32}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - invalid total bitwidth
    app_db_entry.match_field_lookup["src_ipv6_64bit"] =
        "{\"kind\":\"composite\",\"format\":\"IPV6\",\"bitwidth\":65,"
        "\"elements\":[{\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_"
        "ATTR_FIELD_SRC_IPV6_WORD3\",\"bitwidth\":32},{\"kind\":\"sai_field\","
        "\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD2\",\"bitwidth\":"
        "32}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - missing element.bitwidth
    app_db_entry.match_field_lookup["src_ipv6_64bit"] =
        "{\"kind\":\"composite\",\"format\":\"IPV6\",\"bitwidth\":64,"
        "\"elements\":[{\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_"
        "ATTR_FIELD_SRC_IPV6_WORD3\"},{\"kind\":\"sai_field\","
        "\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD2\",\"bitwidth\":"
        "32}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - invalid element.bitwidth
    app_db_entry.match_field_lookup["src_ipv6_64bit"] =
        "{\"kind\":\"composite\",\"format\":\"IPV6\",\"bitwidth\":63,"
        "\"elements\":[{\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_"
        "ATTR_FIELD_SRC_IPV6_WORD3\",\"bitwidth\":31},{\"kind\":\"sai_field\","
        "\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD2\",\"bitwidth\":"
        "32}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - invalid element kind
    app_db_entry.match_field_lookup["src_ipv6_64bit"] =
        "{\"kind\":\"composite\",\"format\":\"IPV6\",\"bitwidth\":64,"
        "\"elements\":[{\"kind\":\"field\",\"sai_field\":\"SAI_ACL_TABLE_"
        "ATTR_FIELD_SRC_IPV6_WORD3\",\"bitwidth\":32},{\"kind\":\"sai_field\","
        "\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD2\",\"bitwidth\":"
        "32}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - missing element.sai_field
    app_db_entry.match_field_lookup["src_ipv6_64bit"] =
        "{\"kind\":\"composite\",\"format\":\"IPV6\",\"bitwidth\":64,"
        "\"elements\":[{\"kind\":\"sai_field\",\"bitwidth\":32},{\"kind\":\"sai_"
        "field\",\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD2\","
        "\"bitwidth\":32}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - invalid element.sai_field
    app_db_entry.match_field_lookup["src_ipv6_64bit"] =
        "{\"kind\":\"composite\",\"format\":\"IPV6\",\"bitwidth\":64,"
        "\"elements\":[{\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_"
        "ATTR_FIELD_SRC_IPV6\",\"bitwidth\":32},{\"kind\":\"sai_field\","
        "\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD2\",\"bitwidth\":"
        "32}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - invalid elements length
    app_db_entry.match_field_lookup["src_ipv6_64bit"] =
        "{\"kind\":\"composite\",\"format\":\"IPV6\",\"bitwidth\":64,"
        "\"elements\":[{\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_"
        "ATTR_FIELD_SRC_IPV6_WORD3\",\"bitwidth\":32}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - invalid first element.sai_field
    app_db_entry.match_field_lookup["src_ipv6_64bit"] =
        "{\"kind\":\"composite\",\"format\":\"IPV6\",\"bitwidth\":64,"
        "\"elements\":[{\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_"
        "ATTR_FIELD_SRC_IPV6_WORD2\",\"bitwidth\":32},{\"kind\":\"sai_field\","
        "\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD2\",\"bitwidth\":"
        "32}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - invalid second element.sai_field
    app_db_entry.match_field_lookup["src_ipv6_64bit"] =
        "{\"kind\":\"composite\",\"format\":\"IPV6\",\"bitwidth\":64,"
        "\"elements\":[{\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_"
        "ATTR_FIELD_SRC_IPV6_WORD3\",\"bitwidth\":32},{\"kind\":\"sai_field\","
        "\"sai_field\":\"SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD3\",\"bitwidth\":"
        "32}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));
}

TEST_F(AclManagerTest, CreatePuntTableWithInvalidCompositeUdfMatchFieldFails)
{
    auto app_db_entry = getDefaultAclTableDefAppDbEntry();

    // Invalid match field str - missing format
    app_db_entry.match_field_lookup["arp_tpa"] =
        "{\"kind\":\"composite\",\"bitwidth\":32,"
        "\"elements\":[{\"kind\":\"udf\",\"base\":\"SAI_UDF_BASE_L3\",\"format\":"
        "\"IPV4\",\"bitwidth\":16,\"offset\":24},{\"kind\":\"udf\",\"base\":"
        "\"SAI_UDF_BASE_L3\",\"format\":\"IPV4\",\"bitwidth\":"
        "16,\"offset\":26}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - invalid format
    app_db_entry.match_field_lookup["arp_tpa"] =
        "{\"kind\":\"composite\",\"format\":\"IP\",\"bitwidth\":32,"
        "\"elements\":[{\"kind\":\"udf\",\"base\":\"SAI_UDF_BASE_L3\",\"format\":"
        "\"IPV6\",\"bitwidth\":16,\"offset\":24},{\"kind\":\"udf\",\"base\":"
        "\"SAI_UDF_BASE_L3\",\"format\":\"IPV4\",\"bitwidth\":"
        "16,\"offset\":26}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - invalid total bitwidth
    app_db_entry.match_field_lookup["arp_tpa"] =
        "{\"kind\":\"composite\",\"format\":\"IPV4\",\"bitwidth\":33,"
        "\"elements\":[{\"kind\":\"udf\",\"base\":\"SAI_UDF_BASE_L3\",\"format\":"
        "\"IPV4\",\"bitwidth\":16,\"offset\":24},{\"kind\":"
        "\"udf\",\"base\":\"SAI_UDF_BASE_L3\",\"format\":\"IPV4\",\"bitwidth\":"
        "16,\"offset\":26}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - missing element.kind
    app_db_entry.match_field_lookup["arp_tpa"] = "{\"kind\":\"composite\",\"format\":\"IPV4\",\"bitwidth\":32,"
                                                 "\"elements\":[{\"base\":\"SAI_UDF_BASE_L3\",\"format\":"
                                                 "\"IPV4\",\"bitwidth\":15,\"offset\":24},{\"kind\":\"udf\",\"base\":"
                                                 "\"SAI_UDF_BASE_L3\",\"format\":\"IPV4\",\"bitwidth\":"
                                                 "16,\"offset\":26}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - inconsistent element.kind
    app_db_entry.match_field_lookup["arp_tpa"] =
        "{\"kind\":\"composite\",\"format\":\"IPV4\",\"bitwidth\":32,"
        "\"elements\":[{\"kind\":\"sai_field\",\"sai_field\":\"SAI_ACL_TABLE_"
        "ATTR_FIELD_SRC_IPV6_WORD3\",\"bitwidth\":32},{\"kind\":\"udf\",\"base\":"
        "\"SAI_UDF_BASE_L3\",\"bitwidth\":"
        "16,\"offset\":26}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - missing element.bitwidth
    app_db_entry.match_field_lookup["arp_tpa"] =
        "{\"kind\":\"composite\",\"format\":\"HEX_STRING\",\"bitwidth\":32,"
        "\"elements\":[{\"kind\":\"udf\",\"base\":\"SAI_UDF_BASE_L3\",\"offset\":"
        "24},{\"kind\":\"udf\",\"base\":\"SAI_UDF_BASE_L3\",\"bitwidth\":"
        "16,\"offset\":26}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - invalid element.bitwidth
    app_db_entry.match_field_lookup["arp_tpa"] = "{\"kind\":\"composite\",\"format\":\"HEX_STRING\",\"bitwidth\":31,"
                                                 "\"elements\":[{\"kind\":\"udf\",\"base\":\"SAI_UDF_BASE_L3\","
                                                 "\"bitwidth\":15,\"offset\":24},{\"kind\":\"udf\",\"base\":\"SAI_UDF_"
                                                 "BASE_L3\",\"bitwidth\":"
                                                 "16,\"offset\":26}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - invalid element.base
    app_db_entry.match_field_lookup["arp_tpa"] =
        "{\"kind\":\"composite\",\"format\":\"HEX_STRING\",\"bitwidth\":32,"
        "\"elements\":[{\"kind\":\"udf\",\"base\":\"SAI_UDF_BASE\",\"bitwidth\":"
        "16,\"offset\":24},{\"kind\":\"udf\",\"base\":\"SAI_UDF_BASE_L3\","
        "\"bitwidth\":"
        "16,\"offset\":26}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - missing element.base
    app_db_entry.match_field_lookup["arp_tpa"] = "{\"kind\":\"composite\",\"format\":\"HEX_STRING\",\"bitwidth\":32,"
                                                 "\"elements\":[{\"kind\":\"udf\",\"bitwidth\":16,\"offset\":24},{"
                                                 "\"kind\":\"udf\",\"base\":\"SAI_UDF_BASE_L3\",\"bitwidth\":"
                                                 "16,\"offset\":26}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - missing element.offset
    app_db_entry.match_field_lookup["arp_tpa"] = "{\"kind\":\"composite\",\"format\":\"HEX_STRING\",\"bitwidth\":32,"
                                                 "\"elements\":[{\"kind\":\"udf\",\"base\":\"SAI_UDF_BASE_L3\","
                                                 "\"bitwidth\":16},{\"kind\":"
                                                 "\"udf\",\"base\":\"SAI_UDF_BASE_L3\",\"bitwidth\":"
                                                 "16,\"offset\":26}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - elements is empty
    app_db_entry.match_field_lookup["arp_tpa"] = "{\"kind\":\"composite\",\"format\":\"HEX_STRING\",\"bitwidth\":32,"
                                                 "\"elements\":[]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - invalid element, should be an object
    app_db_entry.match_field_lookup["arp_tpa"] = "{\"kind\":\"composite\",\"format\":\"HEX_STRING\",\"bitwidth\":32,"
                                                 "\"elements\":[\"group-1\"]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid match field str - missing bitwidth
    app_db_entry.match_field_lookup["arp_tpa"] = "{\"kind\":\"composite\",\"format\":\"HEX_STRING\","
                                                 "\"elements\":[{\"kind\":\"udf\",\"base\":\"SAI_UDF_BASE_L3\","
                                                 "\"bitwidth\":16,\"offset\":24},{\"kind\":"
                                                 "\"udf\",\"base\":\"SAI_UDF_BASE_L3\",\"bitwidth\":"
                                                 "16,\"offset\":26}]}";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));
}

TEST_F(AclManagerTest, CreatePuntTableWithInvalidActionFieldFails)
{
    auto app_db_entry = getDefaultAclTableDefAppDbEntry();

    // Invalid action field
    app_db_entry.action_field_lookup["random_action"].push_back(
        {.sai_action = "RANDOM_ACTION", .p4_param_name = "DUMMY"});

    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));
}

TEST_F(AclManagerTest, CreatePuntTableWithInvalidPacketColorFieldFails)
{
    auto app_db_entry = getDefaultAclTableDefAppDbEntry();
    sai_object_id_t user_defined_trap_oid = gUserDefinedTrapStartOid;
    AddDefaultUserTrapsSaiCalls(&user_defined_trap_oid);

    // Invalid packet_color field
    app_db_entry.packet_action_color_lookup["invalid_packet_color"].push_back(
        {.packet_action = P4_PACKET_ACTION_PUNT, .packet_color = "DUMMY"});

    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));

    // Invalid packet_action field
    app_db_entry.packet_action_color_lookup.erase("invalid_packet_color");
    app_db_entry.packet_action_color_lookup["invalid_packet_action"].push_back(
        {.packet_action = "PUNT", .packet_color = P4_PACKET_COLOR_GREEN});

    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddTableRequest(app_db_entry));
    EXPECT_EQ(nullptr, GetAclTable(app_db_entry.acl_table_name));
}

TEST_F(AclManagerTest, DeserializeValidAclTableDefAppDbSucceeds)
{
    auto app_db_entry_or =
        DeserializeAclTableDefinitionAppDbEntry(kAclIngressTableName, getDefaultTableDefFieldValueTuples());
    EXPECT_TRUE(app_db_entry_or.ok());
    auto &app_db_entry = *app_db_entry_or;
    EXPECT_EQ(kAclIngressTableName, app_db_entry.acl_table_name);
    EXPECT_EQ(123, app_db_entry.size);
    EXPECT_EQ(STAGE_INGRESS, app_db_entry.stage);
    EXPECT_EQ(234, app_db_entry.priority);
    EXPECT_EQ(P4_METER_UNIT_BYTES, app_db_entry.meter_unit);
    EXPECT_EQ(P4_COUNTER_UNIT_BOTH, app_db_entry.counter_unit);
    EXPECT_EQ(BuildMatchFieldJsonStrKindSaiField(P4_MATCH_ETHER_TYPE),
              app_db_entry.match_field_lookup.find("ether_type")->second);
    EXPECT_EQ(BuildMatchFieldJsonStrKindSaiField(P4_MATCH_DST_MAC, P4_FORMAT_MAC),
              app_db_entry.match_field_lookup.find("ether_dst")->second);
    EXPECT_EQ(BuildMatchFieldJsonStrKindSaiField(P4_MATCH_DST_IPV6, P4_FORMAT_IPV6),
              app_db_entry.match_field_lookup.find("ipv6_dst")->second);
    EXPECT_EQ(BuildMatchFieldJsonStrKindSaiField(P4_MATCH_IPV6_NEXT_HEADER),
              app_db_entry.match_field_lookup.find("ipv6_next_header")->second);
    EXPECT_EQ(BuildMatchFieldJsonStrKindSaiField(P4_MATCH_TTL), app_db_entry.match_field_lookup.find("ttl")->second);
    EXPECT_EQ(BuildMatchFieldJsonStrKindSaiField(P4_MATCH_ICMP_TYPE),
              app_db_entry.match_field_lookup.find("icmp_type")->second);
    EXPECT_EQ(BuildMatchFieldJsonStrKindSaiField(P4_MATCH_L4_DST_PORT),
              app_db_entry.match_field_lookup.find("l4_dst_port")->second);
    EXPECT_EQ(P4_ACTION_SET_TRAFFIC_CLASS,
              app_db_entry.action_field_lookup.find("copy_and_set_tc")->second[0].sai_action);
    EXPECT_EQ("traffic_class", app_db_entry.action_field_lookup.find("copy_and_set_tc")->second[0].p4_param_name);
    EXPECT_EQ(P4_ACTION_SET_TRAFFIC_CLASS,
              app_db_entry.action_field_lookup.find("punt_and_set_tc")->second[0].sai_action);
    EXPECT_EQ("traffic_class", app_db_entry.action_field_lookup.find("punt_and_set_tc")->second[0].p4_param_name);
    EXPECT_EQ(P4_PACKET_ACTION_COPY,
              app_db_entry.packet_action_color_lookup.find("copy_and_set_tc")->second[0].packet_action);
    EXPECT_EQ(P4_PACKET_COLOR_GREEN,
              app_db_entry.packet_action_color_lookup.find("copy_and_set_tc")->second[0].packet_color);
    EXPECT_EQ(P4_PACKET_ACTION_PUNT,
              app_db_entry.packet_action_color_lookup.find("punt_and_set_tc")->second[0].packet_action);
    EXPECT_EQ(EMPTY_STRING, app_db_entry.packet_action_color_lookup.find("punt_and_set_tc")->second[0].packet_color);
}

TEST_F(AclManagerTest, DeserializeAclTableDefAppDbWithInvalidJsonFails)
{
    std::string acl_table_name = kAclIngressTableName;
    auto attributes = getDefaultTableDefFieldValueTuples();

    // Invalid action JSON
    attributes.push_back(swss::FieldValueTuple{"action/drop_and_set_tc",
                                               "[{\"action\":\"SAI_PACKET_ACTION_COPY\";\"packet_color\":\"SAI_PACKET_"
                                               "COLOR_GREEN\"};{\"action\":\"SAI_ACL_ENTRY_ATTR_ACTION_SET_TC\";"
                                               "\"param\":\"traffic_class\"}]"});
    EXPECT_FALSE(DeserializeAclTableDefinitionAppDbEntry(acl_table_name, attributes).ok());

    attributes.pop_back();
    attributes.push_back(swss::FieldValueTuple{"action/drop_and_set_tc",
                                               "[{\"action\"-\"SAI_PACKET_ACTION_COPY\",\"packet_color\"-\"SAI_PACKET_"
                                               "COLOR_GREEN\"},{\"action\"-\"SAI_ACL_ENTRY_ATTR_ACTION_SET_TC\","
                                               "\"param\"-\"traffic_class\"}]"});
    EXPECT_FALSE(DeserializeAclTableDefinitionAppDbEntry(acl_table_name, attributes).ok());

    attributes.pop_back();
    attributes.push_back(swss::FieldValueTuple{"action/drop_and_set_tc", "[\"action\":\"SAI_PACKET_ACTION_COPY\"]"});
    EXPECT_FALSE(DeserializeAclTableDefinitionAppDbEntry(acl_table_name, attributes).ok());
}

TEST_F(AclManagerTest, DeserializeAclTableDefAppDbWithInvalidSizeFails)
{
    std::string acl_table_name = kAclIngressTableName;
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{kStage, STAGE_INGRESS});
    attributes.push_back(swss::FieldValueTuple{kPriority, "234"});
    attributes.push_back(swss::FieldValueTuple{"meter/unit", P4_METER_UNIT_BYTES});
    attributes.push_back(swss::FieldValueTuple{"counter/unit", P4_COUNTER_UNIT_BOTH});
    attributes.push_back(
        swss::FieldValueTuple{"match/ether_type", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_ETHER_TYPE)});
    attributes.push_back(
        swss::FieldValueTuple{"match/ether_dst", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_DST_MAC)});
    attributes.push_back(
        swss::FieldValueTuple{"match/ipv6_dst", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_DST_IPV6)});
    attributes.push_back(
        swss::FieldValueTuple{"match/ipv6_next_header", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_IPV6_NEXT_HEADER)});
    attributes.push_back(swss::FieldValueTuple{"match/ttl", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_TTL)});
    attributes.push_back(
        swss::FieldValueTuple{"match/icmp_type", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_ICMP_TYPE)});
    attributes.push_back(
        swss::FieldValueTuple{"match/l4_dst_port", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_L4_DST_PORT)});
    attributes.push_back(swss::FieldValueTuple{"action/copy_and_set_tc",
                                               "[{\"action\":\"SAI_PACKET_ACTION_COPY\",\"packet_color\":\"SAI_PACKET_"
                                               "COLOR_GREEN\"},{\"action\":\"SAI_ACL_ENTRY_ATTR_ACTION_SET_TC\","
                                               "\"param\":\"traffic_class\"}]"});
    attributes.push_back(swss::FieldValueTuple{"action/punt_and_set_tc",
                                               "[{\"action\":\"SAI_PACKET_ACTION_TRAP\"},{\"action\":\"SAI_ACL_ENTRY_"
                                               "ATTR_ACTION_SET_TC\",\"param\":\"traffic_class\"}]"});

    // Invalid table size
    attributes.push_back(swss::FieldValueTuple{kSize, "-123"});
    EXPECT_FALSE(DeserializeAclTableDefinitionAppDbEntry(acl_table_name, attributes).ok());
}

TEST_F(AclManagerTest, DeserializeAclTableDefAppDbWithInvalidPriorityFails)
{
    std::string acl_table_name = kAclIngressTableName;
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{kStage, STAGE_INGRESS});
    attributes.push_back(swss::FieldValueTuple{kSize, "234"});
    attributes.push_back(swss::FieldValueTuple{"meter/unit", P4_METER_UNIT_BYTES});
    attributes.push_back(swss::FieldValueTuple{"counter/unit", P4_COUNTER_UNIT_BOTH});
    attributes.push_back(
        swss::FieldValueTuple{"match/ether_type", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_ETHER_TYPE)});
    attributes.push_back(
        swss::FieldValueTuple{"match/ether_dst", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_DST_MAC)});
    attributes.push_back(
        swss::FieldValueTuple{"match/ipv6_dst", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_DST_IPV6)});
    attributes.push_back(
        swss::FieldValueTuple{"match/ipv6_next_header", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_IPV6_NEXT_HEADER)});
    attributes.push_back(swss::FieldValueTuple{"match/ttl", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_TTL)});
    attributes.push_back(
        swss::FieldValueTuple{"match/icmp_type", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_ICMP_TYPE)});
    attributes.push_back(
        swss::FieldValueTuple{"match/l4_dst_port", BuildMatchFieldJsonStrKindSaiField(P4_MATCH_L4_DST_PORT)});
    attributes.push_back(swss::FieldValueTuple{"action/copy_and_set_tc",
                                               "[{\"action\":\"SAI_PACKET_ACTION_COPY\",\"packet_color\":\"SAI_PACKET_"
                                               "COLOR_GREEN\"},{\"action\":\"SAI_ACL_ENTRY_ATTR_ACTION_SET_TC\","
                                               "\"param\":\"traffic_class\"}]"});
    attributes.push_back(swss::FieldValueTuple{"action/punt_and_set_tc",
                                               "[{\"action\":\"SAI_PACKET_ACTION_TRAP\"},{\"action\":\"SAI_ACL_ENTRY_"
                                               "ATTR_ACTION_SET_TC\",\"param\":\"traffic_class\"}]"});

    // Invalid table priority
    attributes.push_back(swss::FieldValueTuple{kPriority, "-123"});
    EXPECT_FALSE(DeserializeAclTableDefinitionAppDbEntry(acl_table_name, attributes).ok());
}

TEST_F(AclManagerTest, RemoveIngressPuntTableSucceeds)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    EXPECT_CALL(mock_sai_acl_, remove_acl_table_group_member(_)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table(_)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf_group(Eq(kUdfGroupOid1))).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf(_)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteTableRequest(kAclIngressTableName));
}

TEST_F(AclManagerTest, RemoveIngressPuntRuleFails)
{
    // Create ACL table
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());

    // Insert the first ACL rule
    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    auto acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    app_db_entry.action = "set_dst_ipv6";
    app_db_entry.action_param_fvs["ip_address"] = "fdf8:f53b:82e4::53";
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclCounterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));

    // Fails to remove ACL rule when sai_acl_api->remove_acl_entry() fails
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(_)).WillOnce(Return(SAI_STATUS_OBJECT_IN_USE));
    EXPECT_EQ(StatusCode::SWSS_RC_IN_USE, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    const auto &table_name_and_rule_key = concatTableNameAndRuleKey(kAclIngressTableName, acl_rule_key);
    // Fails to remove ACL rule when rule does not exist
    p4_oid_mapper_->eraseOID(SAI_OBJECT_TYPE_ACL_ENTRY, table_name_and_rule_key);
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_ACL_ENTRY, table_name_and_rule_key, kAclIngressRuleOid1);

    // Fails to remove ACL rule when reference count > 0
    p4_oid_mapper_->increaseRefCount(SAI_OBJECT_TYPE_ACL_ENTRY, table_name_and_rule_key);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
    p4_oid_mapper_->decreaseRefCount(SAI_OBJECT_TYPE_ACL_ENTRY, table_name_and_rule_key);

    // Fails to remove ACL rule when sai_policer_api->remove_policer() fails
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_OBJECT_IN_USE));
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_IN_USE, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Fails to remove ACL rule when sai_policer_api->remove_policer() fails and
    // recovery fails
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_OBJECT_IN_USE));
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_FAILURE)));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_IN_USE, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Fails to remove ACL rule when the counter does not exist.
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    p4_oid_mapper_->decreaseRefCount(SAI_OBJECT_TYPE_ACL_COUNTER, table_name_and_rule_key);
    p4_oid_mapper_->eraseOID(SAI_OBJECT_TYPE_ACL_COUNTER, table_name_and_rule_key);
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_ACL_COUNTER, table_name_and_rule_key, kAclCounterOid1, 1);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, kAclMeterOid1);

    // Fails to remove ACL rule when sai_acl_api->remove_acl_counter() fails
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_OBJECT_IN_USE));
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_IN_USE, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Fails to remove ACL rule when sai_acl_api->remove_acl_counter() fails and
    // ACL rule recovery fails.
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_OBJECT_IN_USE));
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_FAILURE)));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_IN_USE, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Fails to remove ACL rule when sai_acl_api->remove_acl_counter() fails and
    // meter recovery fails
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_OBJECT_IN_USE));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_FAILURE)));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_IN_USE, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Fails to remove ACL rule when the meter does not exist.
    // The previous test fails to recover the ACL meter, and hence the meter does
    // not exist.
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, kAclMeterOid1);
}

TEST_F(AclManagerTest, RemoveNonExistingPuntTableFails)
{
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ProcessDeleteTableRequest(kAclIngressTableName));
}

TEST_F(AclManagerTest, RemoveAclTableFailsWhenAclRuleExists)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());

    // Insert the first ACL rule
    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    auto acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    app_db_entry.action = "set_dst_ipv6";
    app_db_entry.action_param_fvs["ip_address"] = "fdf8:f53b:82e4::53";
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));

    // Fails to remove ACL table when the table is nonempty
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessDeleteTableRequest(kAclIngressTableName));
}

TEST_F(AclManagerTest, RemoveAclTableFailsWhenTableDoesNotExist)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    p4_oid_mapper_->eraseOID(SAI_OBJECT_TYPE_ACL_TABLE, kAclIngressTableName);
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, ProcessDeleteTableRequest(kAclIngressTableName));
}

TEST_F(AclManagerTest, RemoveAclTableFailsWhenTableRefCountIsNotZero)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    p4_oid_mapper_->increaseRefCount(SAI_OBJECT_TYPE_ACL_TABLE, kAclIngressTableName);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessDeleteTableRequest(kAclIngressTableName));
}

TEST_F(AclManagerTest, RemoveAclTableFailsWhenSaiCallFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    EXPECT_CALL(mock_sai_acl_, remove_acl_table_group_member(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table(_)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group_member(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclGroupMemberIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessDeleteTableRequest(kAclIngressTableName));
}

TEST_F(AclManagerTest, RemoveAclTableRaisesCriticalStateWhenAclGroupMemberRecoveryFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    EXPECT_CALL(mock_sai_acl_, remove_acl_table_group_member(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table(_)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group_member(_, _, _, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessDeleteTableRequest(kAclIngressTableName));
}

TEST_F(AclManagerTest, RemoveAclTableFailsWhenAclTableGroupMemberDoesNotExist)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    p4_oid_mapper_->eraseOID(SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER, kAclIngressTableName);
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessDeleteTableRequest(kAclIngressTableName));
}

TEST_F(AclManagerTest, RemoveAclTableFailsWhenRemoveAclTableGroupMemberSaiCallFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    EXPECT_CALL(mock_sai_acl_, remove_acl_table_group_member(_)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessDeleteTableRequest(kAclIngressTableName));
}

TEST_F(AclManagerTest, RemoveAclTableFailsWhenRemoveUdfSaiCallFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    EXPECT_CALL(mock_sai_acl_, remove_acl_table_group_member(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf(_)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_acl_, create_acl_table(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclTableIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group_member(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclGroupMemberIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessDeleteTableRequest(kAclIngressTableName));
}

TEST_F(AclManagerTest, RemoveAclTableFailsWhenRemoveUdfGroupSaiCallFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    EXPECT_CALL(mock_sai_acl_, remove_acl_table_group_member(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf_group(_)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_udf_, create_udf(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, create_acl_table(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclTableIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group_member(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclGroupMemberIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessDeleteTableRequest(kAclIngressTableName));
}

TEST_F(AclManagerTest, RemoveAclTableFailsRaisesCriticalStateWhenUdfRecoveryFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    EXPECT_CALL(mock_sai_acl_, remove_acl_table_group_member(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf_group(_)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_udf_, create_udf(_, _, _, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_acl_, create_acl_table(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclTableIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group_member(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclGroupMemberIngressOid), Return(SAI_STATUS_SUCCESS)));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessDeleteTableRequest(kAclIngressTableName));
}

TEST_F(AclManagerTest, RemoveAclTableFailsRaisesCriticalStateWhenUdfGroupRecoveryFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    EXPECT_CALL(mock_sai_acl_, remove_acl_table_group_member(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf(_)).WillOnce(Return(SAI_STATUS_SUCCESS)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_udf_, remove_udf_group(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    // If UDF group recovery fails, UDF recovery and ACL table recovery will also
    // fail since they depend on the UDF group.
    EXPECT_CALL(mock_sai_udf_, create_udf_group(_, _, _, _)).WillOnce(Return(SAI_STATUS_FAILURE));
    // (TODO): Expect critical state x3.
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessDeleteTableRequest(kAclIngressTableName));
}

TEST_F(AclManagerTest, RemoveAclTableFailsWhenUdfHasNonZeroRefCount)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    EXPECT_CALL(mock_sai_acl_, remove_acl_table_group_member(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, create_acl_table(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclTableIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group_member(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclGroupMemberIngressOid), Return(SAI_STATUS_SUCCESS)));
    p4_oid_mapper_->increaseRefCount(SAI_OBJECT_TYPE_UDF,
                                     std::string(kAclIngressTableName) + "-arp_tpa-0-base1-offset24");
    EXPECT_EQ(StatusCode::SWSS_RC_IN_USE, ProcessDeleteTableRequest(kAclIngressTableName));
}

TEST_F(AclManagerTest, RemoveAclTableFailsWhenUdfGroupHasNonZeroRefCount)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    EXPECT_CALL(mock_sai_acl_, remove_acl_table_group_member(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, create_udf(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, create_acl_table(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclTableIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group_member(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclGroupMemberIngressOid), Return(SAI_STATUS_SUCCESS)));
    p4_oid_mapper_->increaseRefCount(SAI_OBJECT_TYPE_UDF_GROUP, std::string(kAclIngressTableName) + "-arp_tpa-0");
    EXPECT_EQ(StatusCode::SWSS_RC_IN_USE, ProcessDeleteTableRequest(kAclIngressTableName));
}

TEST_F(AclManagerTest, RemoveAclGroupsSucceedsAfterCleanup)
{
    // Create ACL table
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    // Insert the first ACL rule
    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    auto acl_rule_key1 = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    app_db_entry.action = "set_dst_ipv6";
    app_db_entry.action_param_fvs["ip_address"] = "fdf8:f53b:82e4::53";
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key1, app_db_entry));
    // Insert the second ACL rule
    app_db_entry.match_fvs["tc"] = "1";
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid2), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid2), Return(SAI_STATUS_SUCCESS)));
    auto acl_rule_key2 = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key2, app_db_entry));
    // There are 3 groups created, only group in INGRESS stage is nonempty.
    // Other groups can be deleted in below RemoveAllGroups()
    EXPECT_CALL(mock_sai_switch_, set_switch_attribute(Eq(gSwitchId), _)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table_group(_)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));

    // Remove ACL groups
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid2))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid2))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_table_group_member(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf_group(Eq(kUdfGroupOid1))).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_udf_, remove_udf(_)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    // Remove rules
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key1));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key2));
    // Remove table
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteTableRequest(kAclIngressTableName));
}

TEST_F(AclManagerTest, DrainRuleTuplesToProcessSetRequestSucceeds)
{
    auto app_db_entry = getDefaultAclTableDefAppDbEntry();
    app_db_entry.counter_unit = P4_COUNTER_UNIT_PACKETS;
    app_db_entry.meter_unit = P4_METER_UNIT_PACKETS;
    EXPECT_CALL(mock_sai_acl_, create_acl_table(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclTableIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group_member(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclGroupMemberIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_udf_, create_udf_group(_, _, _, _))
        .WillRepeatedly(DoAll(SetArgPointee<0>(kUdfGroupOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_udf_, create_udf(_, _, _, _)).Times(3).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    sai_object_id_t user_defined_trap_oid = gUserDefinedTrapStartOid;
    AddDefaultUserTrapsSaiCalls(&user_defined_trap_oid);
    ASSERT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddTableRequest(app_db_entry));
    ASSERT_NO_FATAL_FAILURE(
        IsExpectedAclTableDefinitionMapping(*GetAclTable(app_db_entry.acl_table_name), app_db_entry));
    const auto &acl_rule_json_key = "{\"match/ether_type\":\"0x0800\",\"match/"
                                    "ipv6_dst\":\"fdf8:f53b:82e4::53 & "
                                    "fdf8:f53b:82e4::53\",\"priority\":15}";
    const auto &rule_tuple_key = std::string(kAclIngressTableName) + kTableKeyDelimiter + acl_rule_json_key;
    EnqueueRuleTuple(swss::KeyOpFieldsValuesTuple({rule_tuple_key, SET_COMMAND, getDefaultRuleFieldValueTuples()}));

    // Update request on exact rule without change will not need SAI call
    EnqueueRuleTuple(swss::KeyOpFieldsValuesTuple({rule_tuple_key, SET_COMMAND, getDefaultRuleFieldValueTuples()}));

    // Drain rule tuples to process SET request
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    DrainRuleTuples();

    const auto &acl_rule_key = "match/ether_type=0x0800:match/ipv6_dst=fdf8:f53b:82e4::53 & "
                               "fdf8:f53b:82e4::53:priority=15";
    const auto *acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    EXPECT_EQ(kAclIngressRuleOid1, acl_rule->acl_entry_oid);
}

TEST_F(AclManagerTest, DrainRuleTuplesToProcessSetDelRequestSucceeds)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto attributes = getDefaultRuleFieldValueTuples();
    const auto &acl_rule_json_key = "{\"match/ether_type\":\"0x0800\",\"match/"
                                    "ipv6_dst\":\"fdf8:f53b:82e4::53 & "
                                    "fdf8:f53b:82e4::53\",\"priority\":15}";
    const auto &rule_tuple_key = std::string(kAclIngressTableName) + kTableKeyDelimiter + acl_rule_json_key;
    EnqueueRuleTuple(swss::KeyOpFieldsValuesTuple({rule_tuple_key, SET_COMMAND, attributes}));

    // Drain ACL rule tuple to process SET request
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    DrainRuleTuples();
    // Populate counter stats
    EXPECT_CALL(mock_sai_policer_, get_policer_stats(Eq(kAclMeterOid1), _, _, _))
        .WillOnce(DoAll(Invoke([](sai_object_id_t policer_id, uint32_t number_of_counters,
                                  const sai_stat_id_t *counter_ids, uint64_t *counters) {
                            counters[0] = 100; // green_bytes
                        }),
                        Return(SAI_STATUS_SUCCESS)));
    DoAclCounterStatsTask();
    auto counters_table = std::make_unique<swss::Table>(gCountersDb, std::string(COUNTERS_TABLE) +
                                                                         DEFAULT_KEY_SEPARATOR + APP_P4RT_TABLE_NAME);
    std::vector<swss::FieldValueTuple> values;
    EXPECT_TRUE(counters_table->get(rule_tuple_key, values));

    const auto &acl_rule_key = "match/ether_type=0x0800:match/ipv6_dst=fdf8:f53b:82e4::53 & "
                               "fdf8:f53b:82e4::53:priority=15";
    const auto *acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    EXPECT_EQ(kAclIngressRuleOid1, acl_rule->acl_entry_oid);
    EXPECT_EQ(rule_tuple_key, acl_rule->db_key);

    // Drain ACL rule tuple to process DEL request
    attributes.clear();
    EnqueueRuleTuple(swss::KeyOpFieldsValuesTuple({rule_tuple_key, DEL_COMMAND, attributes}));

    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    DrainRuleTuples();
    EXPECT_EQ(nullptr, GetAclRule(kAclIngressTableName, acl_rule_key));
}

TEST_F(AclManagerTest, DrainRuleTuplesToProcessSetRequestInvalidTableNameRuleKeyFails)
{
    auto attributes = getDefaultRuleFieldValueTuples();
    auto acl_rule_json_key = "{\"match/ether_type\":\"0x0800\",\"match/"
                             "ipv6_dst\":\"fdf8:f53b:82e4::53 & "
                             "fdf8:f53b:82e4::53\",\"priority\":15}";
    auto rule_tuple_key = std::string("INVALID_TABLE_NAME") + kTableKeyDelimiter + acl_rule_json_key;
    EnqueueRuleTuple(swss::KeyOpFieldsValuesTuple({rule_tuple_key, SET_COMMAND, attributes}));
    // Drain rule tuple to process SET request with invalid ACL table name:
    // "INVALID_TABLE_NAME"
    DrainRuleTuples();

    auto acl_rule_key = "match/ether_type=0x0800:match/ipv6_dst=fdf8:f53b:82e4::53 & "
                        "fdf8:f53b:82e4::53:priority=15";
    EXPECT_EQ(nullptr, GetAclRule("INVALID_TABLE_NAME", acl_rule_key));

    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    acl_rule_json_key = "{\"match/ether_type\":\"0x0800\",\"match/"
                        "ipv6_dst\":\"fdf8:f53b:82e4::53 & "
                        "fdf8:f53b:82e4::53\"}";
    rule_tuple_key = std::string(kAclIngressTableName) + kTableKeyDelimiter + acl_rule_json_key;
    acl_rule_key = "match/ether_type=0x0800:match/ipv6_dst=fdf8:f53b:82e4::53 & "
                   "fdf8:f53b:82e4::53";
    EnqueueRuleTuple(swss::KeyOpFieldsValuesTuple({rule_tuple_key, SET_COMMAND, attributes}));
    // Drain rule tuple to process SET request without priority field in rule
    // JSON key
    DrainRuleTuples();
    EXPECT_EQ(nullptr, GetAclRule(kAclIngressTableName, acl_rule_key));
}

TEST_F(AclManagerTest, DeserializeAclRuleAppDbWithInvalidPriorityFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto attributes = getDefaultRuleFieldValueTuples();

    // ACL rule json key has invalid priority
    const auto &acl_rule_json_key = "{\"match/ether_type\":\"0x0800\",\"match/"
                                    "ipv6_dst\":\"fdf8:f53b:82e4::53 & "
                                    "fdf8:f53b:82e4::53\",\"priority\":-15}";

    EXPECT_FALSE(DeserializeAclRuleAppDbEntry(kAclIngressTableName, acl_rule_json_key, attributes).ok());
}

TEST_F(AclManagerTest, DeserializeAclRuleAppDbWithInvalidMeterFieldFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    std::string acl_table_name = kAclIngressTableName;
    std::vector<swss::FieldValueTuple> attributes;
    attributes.push_back(swss::FieldValueTuple{kAction, "copy_and_set_tc"});
    attributes.push_back(swss::FieldValueTuple{"param/traffic_class", "0x20"});
    attributes.push_back(swss::FieldValueTuple{"meter/cburst", "80"});
    attributes.push_back(swss::FieldValueTuple{"meter/pir", "200"});
    attributes.push_back(swss::FieldValueTuple{"meter/pburst", "200"});
    const auto &acl_rule_json_key = "{\"match/ether_type\":\"0x0800\",\"match/"
                                    "ipv6_dst\":\"fdf8:f53b:82e4::53 & "
                                    "fdf8:f53b:82e4::53\",\"priority\":15}";

    // ACL rule has invalid cir value in meter field
    attributes.push_back(swss::FieldValueTuple{"meter/cir", "-80"});
    EXPECT_FALSE(DeserializeAclRuleAppDbEntry(acl_table_name, acl_rule_json_key, attributes).ok());

    // ACL rule has invalid meter field
    attributes.pop_back();
    attributes.push_back(swss::FieldValueTuple{"meter/undefined", "80"});
    EXPECT_FALSE(DeserializeAclRuleAppDbEntry(acl_table_name, acl_rule_json_key, attributes).ok());

    // ACL rule has invalid field
    attributes.pop_back();
    attributes.push_back(swss::FieldValueTuple{"undefined/undefined", "80"});
    EXPECT_FALSE(DeserializeAclRuleAppDbEntry(acl_table_name, acl_rule_json_key, attributes).ok());

    // ACL rule has invalid meter field
    attributes.pop_back();
    attributes.push_back(swss::FieldValueTuple{"undefined", "80"});
    EXPECT_FALSE(DeserializeAclRuleAppDbEntry(acl_table_name, acl_rule_json_key, attributes).ok());
}

TEST_F(AclManagerTest, DrainRuleTuplesWithInvalidCommand)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto attributes = getDefaultRuleFieldValueTuples();
    const auto &acl_rule_json_key = "{\"match/ether_type\":\"0x0800\",\"match/"
                                    "ipv6_dst\":\"fdf8:f53b:82e4::53 & "
                                    "fdf8:f53b:82e4::53\",\"priority\":15}";
    const auto &rule_tuple_key = std::string(kAclIngressTableName) + kTableKeyDelimiter + acl_rule_json_key;
    EnqueueRuleTuple(swss::KeyOpFieldsValuesTuple({rule_tuple_key, "INVALID_COMMAND", attributes}));
    DrainRuleTuples();
    const auto &acl_rule_key = "match/ether_type=0x0800:match/ipv6_dst=fdf8:f53b:82e4::53 & "
                               "fdf8:f53b:82e4::53:priority=15";
    EXPECT_EQ(nullptr, GetAclRule(kAclIngressTableName, acl_rule_key));
}

TEST_F(AclManagerTest, DeserializeAclRuleAppDbWithInvalidMatchFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    // ACL rule json key has invalid match field
    auto acl_rule_json_key = "{\"undefined/undefined\":\"0x0800\",\"priority\":15}";
    EXPECT_FALSE(
        DeserializeAclRuleAppDbEntry(kAclIngressTableName, acl_rule_json_key, getDefaultRuleFieldValueTuples()).ok());
    // ACL rule json key is missing match prefix
    acl_rule_json_key = "{\"ipv6_dst\":\"0x0800\",\"priority\":15}";
    EXPECT_FALSE(
        DeserializeAclRuleAppDbEntry(kAclIngressTableName, acl_rule_json_key, getDefaultRuleFieldValueTuples()).ok());
}

TEST_F(AclManagerTest, DeserializeAclRuleAppDbWithInvalidJsonFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto attributes = getDefaultRuleFieldValueTuples();
    // ACL rule json key is an invalid JSON
    EXPECT_FALSE(DeserializeAclRuleAppDbEntry(kAclIngressTableName, "{\"undefined\"}", attributes).ok());
    EXPECT_FALSE(
        DeserializeAclRuleAppDbEntry(kAclIngressTableName, "[{\"ipv6_dst\":\"0x0800\",\"priority\":15}]", attributes)
            .ok());
}

TEST_F(AclManagerTest, CreateAclRuleWithInvalidSaiMatchFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    app_db_entry.action = "punt_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "0x20";
    auto acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    // ACL rule has invalid in/out port(s)
    app_db_entry.match_fvs["in_port"] = "Eth0";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs.erase("in_port");
    app_db_entry.match_fvs["out_port"] = "Eth0";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs.erase("out_port");
    app_db_entry.match_fvs["in_ports"] = "Eth0,Eth1";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs["in_ports"] = "";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs.erase("in_ports");
    app_db_entry.match_fvs["out_ports"] = "";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs["out_ports"] = "Eth0,Eth1";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs.erase("out_ports");

    // ACL rule has invalid ipv6_dst
    app_db_entry.match_fvs["ipv6_dst"] = "10.0.0.2";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs["ipv6_dst"] = "10.0.0.2 & 255.255.255.0";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs["ipv6_dst"] = "fdf8:f53b:82e4::53 & 255.255.255.0";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs["ipv6_dst"] = "null";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs["ipv6_dst"] = "fdf8:f53b:82e4::53";

    // ACL rule has invalid ip_src
    app_db_entry.match_fvs["ip_src"] = "fdf8:f53b:82e4::53";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs["ip_src"] = "fdf8:f53b:82e4::53 & ffff:ffff:ffff::";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs["ip_src"] = "10.0.0.2 & ffff:ffff:ffff::";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs["ip_src"] = "null";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs["ip_src"] = "10.0.0.2";

    // ACL rule has invalid ether_type
    app_db_entry.match_fvs["ether_type"] = "0x88800";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs["ether_type"] = "0x0800";

    // ACL rule has invalid ip_frag
    app_db_entry.match_fvs["ip_frag"] = "invalid";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs.erase("ip_frag");

    // ACL rule has invalid packet_vlan
    app_db_entry.match_fvs["packet_vlan"] = "invalid";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs.erase("packet_vlan");

    // ACL rule has invalid UDF field: should be HEX_STRING
    app_db_entry.match_fvs["arp_tpa"] = "invalid";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));

    // ACL rule has invalid UDF field: invalid HEX_STRING length
    app_db_entry.match_fvs["arp_tpa"] = "0xff";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));

    // ACL table misses UDF group definition
    const auto &acl_table_acl_tableapp_db_entry = getDefaultAclTableDefAppDbEntry();
    auto *acl_table = GetAclTable(acl_table_acl_tableapp_db_entry.acl_table_name);
    std::map<std::string, uint16_t> saved_udf_group_attr_index_lookup = acl_table->udf_group_attr_index_lookup;
    acl_table->udf_group_attr_index_lookup.clear();
    app_db_entry.match_fvs["arp_tpa"] = "0xff112231";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs.erase("arp_tpa");
    acl_table->udf_group_attr_index_lookup = saved_udf_group_attr_index_lookup;

    // ACL rule has undefined match field
    app_db_entry.match_fvs["undefined"] = "1";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs.erase("undefined");
}

TEST_F(AclManagerTest, CreateAclRuleWithInvalidCompositeSaiMatchFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    app_db_entry.action = "punt_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "0x20";
    auto acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    // ACL rule has invalid src_ipv6_64bit(composite SAI field) - should be ipv6
    // address
    app_db_entry.match_fvs["src_ipv6_64bit"] = "Eth0";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs["src_ipv6_64bit"] = "10.0.0.1";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs["src_ipv6_64bit"] = "10.0.0.1 & ffff:ffff::";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.match_fvs["src_ipv6_64bit"] = "fdf8:f53b:82e4:: & 255.255.255.255";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
}

TEST_F(AclManagerTest, AclRuleWithValidMatchFields)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    app_db_entry.action = "punt_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "0x20";

    // Match fields registered in table definition
    app_db_entry.match_fvs["ether_dst"] = "AA:BB:CC:DD:EE:FF & FF:FF:FF:FF:FF:FF";
    app_db_entry.match_fvs["ttl"] = "0x2 & 0xFF";
    app_db_entry.match_fvs["in_ports"] = "Ethernet1,Ethernet2";
    app_db_entry.match_fvs["in_port"] = "Ethernet3";
    app_db_entry.match_fvs["out_ports"] = "Ethernet4,Ethernet5";
    app_db_entry.match_fvs["out_port"] = "Ethernet6";
    app_db_entry.match_fvs["tcp_flags"] = " 0x2 & 0x3F ";
    app_db_entry.match_fvs["ip_flags"] = "0x2";
    app_db_entry.match_fvs["l4_src_port"] = "0x2e90 & 0xFFF0";
    app_db_entry.match_fvs["l4_dst_port"] = "0x2e98";
    app_db_entry.match_fvs["ip_id"] = "2";
    app_db_entry.match_fvs["inner_l4_src_port"] = "1212";
    app_db_entry.match_fvs["inner_l4_dst_port"] = "1212";
    app_db_entry.match_fvs["dscp"] = "8";
    app_db_entry.match_fvs["inner_ip_src"] = "192.50.128.0";
    app_db_entry.match_fvs["inner_ip_dst"] = "192.50.128.0/17";
    app_db_entry.match_fvs["inner_ipv6_src"] = "1234:5678::";
    app_db_entry.match_fvs["inner_ipv6_dst"] = "2001:db8:3c4d:15::/64";
    app_db_entry.match_fvs["ip_src"] = " 192.50.128.0 & 255.255.255.0 ";
    app_db_entry.match_fvs["ip_dst"] = "192.50.128.0/17";
    app_db_entry.match_fvs["ipv6_src"] = "2001:db8:3c4d:15::/64";
    app_db_entry.match_fvs["tc"] = "1";
    app_db_entry.match_fvs["icmp_type"] = "9"; // RA
    app_db_entry.match_fvs["icmp_code"] = "0"; // Normal RA
    app_db_entry.match_fvs["tos"] = "32";
    app_db_entry.match_fvs["icmpv6_type"] = "134"; // RA
    app_db_entry.match_fvs["icmpv6_code"] = "0";   // Normal RA
    app_db_entry.match_fvs["ecn"] = "0";
    app_db_entry.match_fvs["inner_ip_protocol"] = "0x01"; // ICMP
    app_db_entry.match_fvs["ip_protocol"] = "0x6";        // TCP
    app_db_entry.match_fvs["ipv6_flow_label"] = "0x88 & 0xFFFFFFFF";
    app_db_entry.match_fvs["tunnel_vni"] = "88";
    app_db_entry.match_fvs["ip_frag"] = P4_IP_FRAG_HEAD;
    app_db_entry.match_fvs["packet_vlan"] = P4_PACKET_VLAN_SINGLE_OUTER_TAG;
    app_db_entry.match_fvs["outer_vlan_pri"] = "100";
    app_db_entry.match_fvs["outer_vlan_id"] = "100";
    app_db_entry.match_fvs["outer_vlan_cfi"] = "100";
    app_db_entry.match_fvs["inner_vlan_pri"] = "200";
    app_db_entry.match_fvs["inner_vlan_id"] = "200";
    app_db_entry.match_fvs["inner_vlan_cfi"] = "200";

    const auto &acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    auto *acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    auto *acl_table = GetAclTable(kAclIngressTableName);
    ASSERT_NE(nullptr, acl_rule);
    ASSERT_NE(nullptr, acl_table);
    EXPECT_NO_FATAL_FAILURE(IsExpectedAclRuleMapping(acl_rule, app_db_entry, *acl_table));
    EXPECT_TRUE(acl_rule->meter.packet_color_actions.empty());

    // Check match field value
    EXPECT_EQ(2, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS].aclfield.data.objlist.count);
    EXPECT_EQ(0x112233, acl_rule->in_ports_oids[0]);
    EXPECT_EQ(0x1fed3, acl_rule->in_ports_oids[1]);
    EXPECT_EQ(2, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_OUT_PORTS].aclfield.data.objlist.count);
    EXPECT_EQ(0x9988, acl_rule->out_ports_oids[0]);
    EXPECT_EQ(0x56789abcdef, acl_rule->out_ports_oids[1]);
    EXPECT_EQ(0xaabbccdd, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_IN_PORT].aclfield.data.oid);
    EXPECT_EQ(0x56789abcdff, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_OUT_PORT].aclfield.data.oid);
    EXPECT_EQ(0x2, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_TCP_FLAGS].aclfield.data.u8);
    EXPECT_EQ(0x3F, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_TCP_FLAGS].aclfield.mask.u8);
    EXPECT_EQ(0x2, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_IP_FLAGS].aclfield.data.u8);
    EXPECT_EQ(0x3F, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_IP_FLAGS].aclfield.mask.u8);
    EXPECT_EQ(8, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_DSCP].aclfield.data.u8);
    EXPECT_EQ(0x3F, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_DSCP].aclfield.mask.u8);
    EXPECT_EQ(0x0800, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE].aclfield.data.u16);
    EXPECT_EQ(0xFFFF, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE].aclfield.mask.u16);
    EXPECT_EQ(0x2e90, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT].aclfield.data.u16);
    EXPECT_EQ(0xFFF0, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT].aclfield.mask.u16);
    EXPECT_EQ(2, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_IP_IDENTIFICATION].aclfield.data.u16);
    EXPECT_EQ(0xFFFF, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_IP_IDENTIFICATION].aclfield.mask.u16);
    EXPECT_EQ(100, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_OUTER_VLAN_ID].aclfield.data.u16);
    EXPECT_EQ(0xFFFF, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_OUTER_VLAN_ID].aclfield.mask.u16);
    // 192.50.128.0
    EXPECT_EQ(swss::IpPrefix("192.50.128.0").getIp().getV4Addr(),
              acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IP].aclfield.data.ip4);
    EXPECT_EQ(swss::IpPrefix("192.50.128.0").getMask().getV4Addr(),
              acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IP].aclfield.mask.ip4);
    // 192.50.128.0 & 255.255.255.0
    EXPECT_EQ(swss::IpAddress("192.50.128.0").getV4Addr(),
              acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP].aclfield.data.ip4);
    EXPECT_EQ(swss::IpAddress("255.255.255.0").getV4Addr(),
              acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP].aclfield.mask.ip4);
    EXPECT_EQ(0, memcmp(acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6].aclfield.data.ip6,
                        swss::IpPrefix("2001:db8:3c4d:15::/64").getIp().getV6Addr(), 16));
    EXPECT_EQ(0, memcmp(acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6].aclfield.mask.ip6,
                        swss::IpPrefix("2001:db8:3c4d:15::/64").getMask().getV6Addr(), 16));

    EXPECT_EQ(0, memcmp(acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_SRC_MAC].aclfield.data.mac,
                        swss::MacAddress("AA:BB:CC:DD:EE:FF").getMac(), 6));
    EXPECT_EQ(0, memcmp(acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_DST_MAC].aclfield.data.mac,
                        swss::MacAddress("AA:BB:CC:DD:EE:FF").getMac(), 6));
    EXPECT_EQ(0, memcmp(acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_DST_MAC].aclfield.mask.mac,
                        swss::MacAddress("FF:FF:FF:FF:FF:FF").getMac(), 6));
    EXPECT_EQ(1, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_TC].aclfield.data.u8);
    EXPECT_EQ(0xFF, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_TC].aclfield.mask.u8);
    EXPECT_EQ(0x88, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_IPV6_FLOW_LABEL].aclfield.data.u32);
    EXPECT_EQ(0xFFFFFFFF, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_IPV6_FLOW_LABEL].aclfield.mask.u32);
    EXPECT_EQ(SAI_ACL_IP_FRAG_HEAD, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_FRAG].aclfield.data.u32);
    EXPECT_EQ(SAI_PACKET_VLAN_SINGLE_OUTER_TAG,
              acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_PACKET_VLAN].aclfield.data.u32);

    // Check action field value
    EXPECT_EQ(SAI_PACKET_ACTION_TRAP,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.parameter.s32);
    EXPECT_EQ(0x20, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
}

TEST_F(AclManagerTest, AclRuleWithValidAction)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());

    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    const auto &acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    // Redirect action
    app_db_entry.action = "redirect";
    app_db_entry.action_param_fvs["target"] = "Ethernet1";
    // Install rule
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    auto *acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(/*port_oid=*/0x112233, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT].aclaction.parameter.oid);
    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
    EXPECT_EQ(nullptr, GetAclRule(kAclIngressTableName, acl_rule_key));

    // Redirect action
    app_db_entry.action = "redirect";
    app_db_entry.action_param_fvs["target"] = "Ethernet7";
    // Install rule
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(/*port_oid=*/0x1234, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT].aclaction.parameter.oid);
    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
    EXPECT_EQ(nullptr, GetAclRule(kAclIngressTableName, acl_rule_key));

    // Set up an next hop mapping
    const std::string next_hop_id = "ju1u32m1.atl11:qe-3/7";
    const auto &next_hop_key = KeyGenerator::generateNextHopKey(next_hop_id);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key,
                           /*next_hop_oid=*/1);
    app_db_entry.action = "redirect";
    app_db_entry.action_param_fvs["target"] = next_hop_id;
    // Install rule
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(/*next_hop_oid=*/1, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT].aclaction.parameter.oid);
    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
    EXPECT_EQ(nullptr, GetAclRule(kAclIngressTableName, acl_rule_key));

    // Set endpoint Ip action
    app_db_entry.action = "endpoint_ip";
    app_db_entry.action_param_fvs["ip_address"] = "127.0.0.1";
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(swss::IpAddress("127.0.0.1").getV4Addr(),
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_ENDPOINT_IP].aclaction.parameter.ip4);
    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
    // Install rule
    app_db_entry.action_param_fvs["ip_address"] = "fdf8:f53b:82e4::53";
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(0, memcmp(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_ENDPOINT_IP].aclaction.parameter.ip6,
                        swss::IpAddress("fdf8:f53b:82e4::53").getV6Addr(), 16));
    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Mirror ingress action
    app_db_entry.action = "mirror_ingress";
    app_db_entry.action_param_fvs["target"] = gMirrorSession1;
    // Install rule
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);

    // Check action field value
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS].aclaction.enable);
    EXPECT_EQ(kMirrorSessionOid1, acl_rule->action_mirror_sessions[SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS].oid);
    EXPECT_EQ(gMirrorSession1, acl_rule->action_mirror_sessions[SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS].name);

    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Mirror egress action
    app_db_entry.action = "mirror_egress";
    app_db_entry.action_param_fvs["target"] = gMirrorSession2;
    // Install rule
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);

    // Check action field value
    EXPECT_EQ(acl_rule->action_fvs.end(), acl_rule->action_fvs.find(SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS));
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_EGRESS].aclaction.enable);
    EXPECT_EQ(kMirrorSessionOid2, acl_rule->action_mirror_sessions[SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_EGRESS].oid);
    EXPECT_EQ(gMirrorSession2, acl_rule->action_mirror_sessions[SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_EGRESS].name);

    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Set Packet Color
    app_db_entry.action = "set_packet_color";
    app_db_entry.action_param_fvs["packet_color"] = "SAI_PACKET_COLOR_YELLOW";
    // Install rule
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(SAI_PACKET_COLOR_YELLOW,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_PACKET_COLOR].aclaction.parameter.s32);
    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Set Src Mac
    app_db_entry.action = "set_src_mac";
    app_db_entry.action_param_fvs["mac_address"] = "AA:BB:CC:DD:EE:FF";

    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(0, memcmp(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_MAC].aclaction.parameter.mac,
                        swss::MacAddress("AA:BB:CC:DD:EE:FF").getMac(), 6));
    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Set src IP
    app_db_entry.action = "set_src_ip";
    app_db_entry.action_param_fvs["ip_address"] = "10.0.0.1";

    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(swss::IpAddress("10.0.0.1").getV4Addr(),
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_IP].aclaction.parameter.ip4);
    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Set IPv6 Dst
    app_db_entry.action = "set_dst_ipv6";
    app_db_entry.action_param_fvs["ip_address"] = "fdf8:f53b:82e4::53";

    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(0, memcmp(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_DST_IPV6].aclaction.parameter.ip6,
                        swss::IpAddress("fdf8:f53b:82e4::53").getV6Addr(), 16));
    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Set DSCP and ECN
    app_db_entry.action = "set_dscp_and_ecn";
    app_db_entry.action_param_fvs["dscp"] = "8";
    app_db_entry.action_param_fvs["ecn"] = "0";

    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(8, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_DSCP].aclaction.parameter.u8);
    EXPECT_EQ(0, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_ECN].aclaction.parameter.u8);
    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Set Inner VLAN
    app_db_entry.action = "set_inner_vlan";
    app_db_entry.action_param_fvs["vlan_pri"] = "100";
    app_db_entry.action_param_fvs["vlan_id"] = "100";

    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(100, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_INNER_VLAN_ID].aclaction.parameter.u32);
    EXPECT_EQ(100, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_INNER_VLAN_PRI].aclaction.parameter.u8);
    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Set L4Src Port
    app_db_entry.action = "set_l4_src_port";
    app_db_entry.action_param_fvs["port"] = "1212";

    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(1212, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_L4_SRC_PORT].aclaction.parameter.u16);
    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Flood action
    app_db_entry.action = "flood";

    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_FLOOD].aclaction.enable);
    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Set user defined trap for QOS_QUEUE
    int queue_num = 2;
    app_db_entry.action = "qos_queue";
    app_db_entry.action_param_fvs["cpu_queue"] = std::to_string(queue_num);
    // Install rule
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(gUserDefinedTrapStartOid + queue_num,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_USER_TRAP_ID].aclaction.parameter.oid);
    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
    EXPECT_EQ(nullptr, GetAclRule(kAclIngressTableName, acl_rule_key));
}

TEST_F(AclManagerTest, AclRuleWithVrfAction)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());

    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    const auto &acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    // Set vrf
    app_db_entry.action = "set_vrf";
    app_db_entry.action_param_fvs["vrf"] = gVrfName;
    // Install rule
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    auto *acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(gVrfOid, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_VRF].aclaction.parameter.oid);
    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
    EXPECT_EQ(nullptr, GetAclRule(kAclIngressTableName, acl_rule_key));
}

TEST_F(AclManagerTest, AclRuleWithIpTypeBitEncoding)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());

    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    // Set src IP
    app_db_entry.action = "set_src_ip";
    app_db_entry.action_param_fvs["ip_address"] = "10.0.0.1";

    // Successful cases
    // Wildcard match on IP_TYPE: SAI_ACL_IP_TYPE_ANY
    auto acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillRepeatedly(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillRepeatedly(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    auto acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check match field IP_TYPE
    EXPECT_NE(acl_rule->match_fvs.end(), acl_rule->match_fvs.find(SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE));
    EXPECT_EQ(SAI_ACL_IP_TYPE_ANY, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE].aclfield.data.u32);
    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // is_ip { value: 0x1 mask: 0x1 } = SAI_ACL_IP_TYPE_IP
    app_db_entry.match_fvs["is_ip"] = "0x1";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check match field IP_TYPE
    EXPECT_EQ(SAI_ACL_IP_TYPE_IP, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE].aclfield.data.u32);
    // Remove rule
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // is_ip { value: 0x0 mask: 0x1 } = SAI_ACL_IP_TYPE_NON_IP
    app_db_entry.match_fvs["is_ip"] = "0x0 & 0x1";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check match field IP_TYPE
    EXPECT_EQ(SAI_ACL_IP_TYPE_NON_IP, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE].aclfield.data.u32);
    // Remove rule
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // is_ipv4 { value: 0x1 mask: 0x1 } = SAI_ACL_IP_TYPE_IPV4ANY
    app_db_entry.match_fvs.erase("is_ip");
    app_db_entry.match_fvs["is_ipv4"] = "0x1 & 0x1";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check match field IP_TYPE
    EXPECT_EQ(SAI_ACL_IP_TYPE_IPV4ANY, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE].aclfield.data.u32);
    // Remove rule
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // is_ipv4 { value: 0x0 mask: 0x1 } = SAI_ACL_IP_TYPE_NON_IPV4
    app_db_entry.match_fvs["is_ipv4"] = "0x0";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check match field IP_TYPE
    EXPECT_EQ(SAI_ACL_IP_TYPE_NON_IPV4, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE].aclfield.data.u32);
    // Remove rule
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // is_ipv6 { value: 0x1 mask: 0x1 } = SAI_ACL_IP_TYPE_IPV6ANY
    app_db_entry.match_fvs.erase("is_ipv4");
    app_db_entry.match_fvs["is_ipv6"] = "0x1";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check match field IP_TYPE
    EXPECT_EQ(SAI_ACL_IP_TYPE_IPV6ANY, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE].aclfield.data.u32);
    // Remove rule
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // is_ipv6 { value: 0x0 mask: 0x1 } = SAI_ACL_IP_TYPE_NON_IPV6
    app_db_entry.match_fvs["is_ipv6"] = "0x0";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check match field IP_TYPE
    EXPECT_EQ(SAI_ACL_IP_TYPE_NON_IPV6, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE].aclfield.data.u32);
    // Remove rule
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // is_arp { value: 0x1 mask: 0x1 } = SAI_ACL_IP_TYPE_ARP
    app_db_entry.match_fvs.erase("is_ipv6");
    app_db_entry.match_fvs["is_arp"] = "0x1 & 0x1";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check match field IP_TYPE
    EXPECT_EQ(SAI_ACL_IP_TYPE_ARP, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE].aclfield.data.u32);
    // Remove rule
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // is_arp_request { value: 0x1 mask: 0x1 } = SAI_ACL_IP_TYPE_ARP_REQUEST
    app_db_entry.match_fvs.erase("is_arp");
    app_db_entry.match_fvs["is_arp_request"] = "0x1 & 0x1";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check match field IP_TYPE
    EXPECT_EQ(SAI_ACL_IP_TYPE_ARP_REQUEST, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE].aclfield.data.u32);
    // Remove rule
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // is_arp_reply { value: 0x1 mask: 0x1 } = SAI_ACL_IP_TYPE_ARP_REPLY
    app_db_entry.match_fvs.erase("is_arp_request");
    app_db_entry.match_fvs["is_arp_reply"] = "0x1 & 0x1";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check match field IP_TYPE
    EXPECT_EQ(SAI_ACL_IP_TYPE_ARP_REPLY, acl_rule->match_fvs[SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE].aclfield.data.u32);
    // Remove rule
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));

    // Failed cases
    // is_arp_reply { value: 0x0 mask: 0x1 } = N/A
    app_db_entry.match_fvs["is_arp_reply"] = "0x0 & 0x1";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_EQ(nullptr, acl_rule);

    // is_arp_request { value: 0x0 mask: 0x1 } = N/A
    app_db_entry.match_fvs.erase("is_arp_reply");
    app_db_entry.match_fvs["is_arp_request"] = "0x0";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_EQ(nullptr, acl_rule);

    // is_arp { value: 0x0 mask: 0x1 } = N/A
    app_db_entry.match_fvs.erase("is_arp_request");
    app_db_entry.match_fvs["is_arp"] = "0x0";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_EQ(nullptr, acl_rule);

    // is_ip { value: 0x1 mask: 0x0 } = N/A
    app_db_entry.match_fvs.erase("is_arp");
    app_db_entry.match_fvs["is_ip"] = "0x1 & 0x0";
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_EQ(nullptr, acl_rule);
}

TEST_F(AclManagerTest, UpdateAclRuleWithActionMeterChange)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());

    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    const auto &acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    sai_object_id_t meter_oid;
    uint32_t ref_cnt;
    const auto &table_name_and_rule_key = concatTableNameAndRuleKey(kAclIngressTableName, acl_rule_key);
    app_db_entry.action = "punt_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "1";
    // Install rule
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    auto *acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(SAI_PACKET_ACTION_TRAP,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.parameter.s32);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.enable);
    EXPECT_EQ(1, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.enable);
    EXPECT_TRUE(p4_oid_mapper_->getOID(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &meter_oid));
    EXPECT_EQ(kAclMeterOid1, meter_oid);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Update action parameter value
    app_db_entry.action = "punt_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "2";
    // Update rule
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(SAI_PACKET_ACTION_TRAP,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.parameter.s32);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.enable);
    EXPECT_EQ(2, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.enable);
    EXPECT_TRUE(acl_rule->meter.packet_color_actions.empty());

    EXPECT_TRUE(p4_oid_mapper_->getOID(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &meter_oid));
    EXPECT_EQ(kAclMeterOid1, meter_oid);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Update packet action: Copy green packet
    app_db_entry.action = "copy_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "2";
    app_db_entry.meter.cburst = 500;
    app_db_entry.meter.cir = 500;
    app_db_entry.meter.pburst = 600;
    app_db_entry.meter.pir = 600;
    // Update meter attribute for green packet action
    EXPECT_CALL(mock_sai_policer_, set_policer_attribute(Eq(kAclMeterOid1), _))
        .Times(5)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(acl_rule->action_fvs.end(), acl_rule->action_fvs.find(SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION));
    EXPECT_EQ(2, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.enable);
    EXPECT_FALSE(acl_rule->meter.packet_color_actions.empty());
    EXPECT_NE(acl_rule->meter.packet_color_actions.find(SAI_POLICER_ATTR_GREEN_PACKET_ACTION),
              acl_rule->meter.packet_color_actions.end());
    EXPECT_EQ(SAI_PACKET_ACTION_COPY, acl_rule->meter.packet_color_actions[SAI_POLICER_ATTR_GREEN_PACKET_ACTION]);
    EXPECT_EQ(500, acl_rule->meter.cburst);
    EXPECT_EQ(500, acl_rule->meter.cir);
    EXPECT_EQ(600, acl_rule->meter.pburst);
    EXPECT_EQ(600, acl_rule->meter.pir);
    EXPECT_TRUE(p4_oid_mapper_->getOID(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &meter_oid));
    EXPECT_EQ(kAclMeterOid1, meter_oid);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Update ACL rule : disable rate limiting, packet action is still existing.
    app_db_entry.meter.enabled = false;
    // Update meter attribute for green packet action
    EXPECT_CALL(mock_sai_policer_, set_policer_attribute(Eq(kAclMeterOid1), _))
        .Times(4)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_FALSE(acl_rule->meter.packet_color_actions.empty());
    EXPECT_NE(acl_rule->meter.packet_color_actions.find(SAI_POLICER_ATTR_GREEN_PACKET_ACTION),
              acl_rule->meter.packet_color_actions.end());
    EXPECT_EQ(SAI_PACKET_ACTION_COPY, acl_rule->meter.packet_color_actions[SAI_POLICER_ATTR_GREEN_PACKET_ACTION]);
    EXPECT_EQ(2, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.enable);
    EXPECT_TRUE(p4_oid_mapper_->getOID(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &meter_oid));
    EXPECT_FALSE(acl_rule->meter.enabled);
    EXPECT_EQ(0, acl_rule->meter.cburst);
    EXPECT_EQ(0, acl_rule->meter.cir);
    EXPECT_EQ(0, acl_rule->meter.pburst);
    EXPECT_EQ(0, acl_rule->meter.pir);

    // Update meter: enable rate limiting and reset green packet action
    app_db_entry.action = "punt_and_set_tc";
    app_db_entry.meter.enabled = true;
    // Update meter and rule: reset color packet action and update entry
    // attribute
    EXPECT_CALL(mock_sai_policer_, set_policer_attribute(Eq(kAclMeterOid1), _))
        .Times(5)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(SAI_PACKET_ACTION_TRAP,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.parameter.s32);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.enable);
    EXPECT_EQ(2, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.enable);
    EXPECT_TRUE(acl_rule->meter.packet_color_actions.empty());
    EXPECT_EQ(500, acl_rule->meter.cburst);
    EXPECT_EQ(500, acl_rule->meter.cir);
    EXPECT_EQ(600, acl_rule->meter.pburst);
    EXPECT_EQ(600, acl_rule->meter.pir);
    EXPECT_TRUE(p4_oid_mapper_->getOID(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &meter_oid));
    EXPECT_EQ(kAclMeterOid1, meter_oid);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Update ACL rule : disable meter
    app_db_entry.meter.enabled = false;
    // Update meter attribute for green packet action
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(SAI_PACKET_ACTION_TRAP,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.parameter.s32);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.enable);
    EXPECT_EQ(2, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.enable);
    EXPECT_FALSE(p4_oid_mapper_->getOID(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &meter_oid));
    EXPECT_EQ(0, acl_rule->meter.cburst);
    EXPECT_EQ(0, acl_rule->meter.cir);
    EXPECT_EQ(0, acl_rule->meter.pburst);
    EXPECT_EQ(0, acl_rule->meter.pir);
    EXPECT_TRUE(acl_rule->meter.packet_color_actions.empty());
    EXPECT_FALSE(acl_rule->meter.enabled);

    // Update ACL rule : enable meter
    app_db_entry.meter.enabled = true;
    // Update meter attribute for green packet action
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid2), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(2, acl_rule->action_fvs.size());
    EXPECT_EQ(SAI_PACKET_ACTION_TRAP,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.parameter.s32);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.enable);
    EXPECT_EQ(2, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.enable);
    EXPECT_TRUE(p4_oid_mapper_->getOID(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &meter_oid));
    EXPECT_EQ(kAclMeterOid2, meter_oid);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
    EXPECT_TRUE(acl_rule->meter.packet_color_actions.empty());
    EXPECT_TRUE(acl_rule->meter.enabled);

    // Redirect action
    app_db_entry.action = "redirect";
    app_db_entry.action_param_fvs["target"] = "Ethernet1";
    // Update rule
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .Times(3)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_EQ(
        /*port_oid=*/0x112233, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT].aclaction.parameter.oid);

    // Set up an next hop mapping
    const std::string next_hop_id = "ju1u32m1.atl11:qe-3/7";
    const auto &next_hop_key = KeyGenerator::generateNextHopKey(next_hop_id);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key,
                           /*next_hop_oid=*/1);
    app_db_entry.action = "redirect";
    app_db_entry.action_param_fvs["target"] = next_hop_id;
    // Update rule
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_EQ(/*next_hop_oid=*/1, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT].aclaction.parameter.oid);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);
    EXPECT_EQ(next_hop_key, acl_rule->action_redirect_nexthop_key);

    // Set endpoint Ip action
    app_db_entry.action = "endpoint_ip";
    app_db_entry.action_param_fvs["ip_address"] = "127.0.0.1";
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .Times(2)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(swss::IpAddress("127.0.0.1").getV4Addr(),
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_ENDPOINT_IP].aclaction.parameter.ip4);
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key, &ref_cnt));
    EXPECT_EQ(0, ref_cnt);
    app_db_entry.action_param_fvs["ip_address"] = "fdf8:f53b:82e4::53";
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .Times(1)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_EQ(0, memcmp(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_ENDPOINT_IP].aclaction.parameter.ip6,
                        swss::IpAddress("fdf8:f53b:82e4::53").getV6Addr(), 16));

    // Set src Mac
    app_db_entry.action = "set_src_mac";
    app_db_entry.action_param_fvs["mac_address"] = "AA:BB:CC:DD:EE:FF";
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .Times(2)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_EQ(0, memcmp(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_MAC].aclaction.parameter.mac,
                        swss::MacAddress("AA:BB:CC:DD:EE:FF").getMac(), 6));
    app_db_entry.action_param_fvs["mac_address"] = "11:22:33:44:55:66";
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_EQ(0, memcmp(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_MAC].aclaction.parameter.mac,
                        swss::MacAddress("11:22:33:44:55:66").getMac(), 6));

    // Set Inner VLAN
    app_db_entry.action = "set_inner_vlan";
    app_db_entry.action_param_fvs["vlan_pri"] = "100";
    app_db_entry.action_param_fvs["vlan_id"] = "100";
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .Times(3)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(2, acl_rule->action_fvs.size());
    EXPECT_EQ(100, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_INNER_VLAN_ID].aclaction.parameter.u32);
    EXPECT_EQ(100, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_INNER_VLAN_PRI].aclaction.parameter.u8);
    app_db_entry.action_param_fvs["vlan_pri"] = "150";
    app_db_entry.action_param_fvs["vlan_id"] = "150";
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .Times(2)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(2, acl_rule->action_fvs.size());
    EXPECT_EQ(150, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_INNER_VLAN_ID].aclaction.parameter.u32);
    EXPECT_EQ(150, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_INNER_VLAN_PRI].aclaction.parameter.u8);

    // Set src IP
    app_db_entry.action = "set_src_ip";
    app_db_entry.action_param_fvs["ip_address"] = "10.0.0.1";
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .Times(3)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_EQ(swss::IpAddress("10.0.0.1").getV4Addr(),
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_IP].aclaction.parameter.ip4);
    app_db_entry.action_param_fvs["ip_address"] = "10.10.10.1";
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .Times(1)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_EQ(swss::IpAddress("10.10.10.1").getV4Addr(),
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_IP].aclaction.parameter.ip4);

    // Set IPv6 Dst
    app_db_entry.action = "set_dst_ipv6";
    app_db_entry.action_param_fvs["ip_address"] = "fdf8:f53b:82e4::53";
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .Times(2)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_EQ(0, memcmp(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_DST_IPV6].aclaction.parameter.ip6,
                        swss::IpAddress("fdf8:f53b:82e4::53").getV6Addr(), 16));
    app_db_entry.action_param_fvs["ip_address"] = "fdf8:f53b::53";
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .Times(1)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_EQ(0, memcmp(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_DST_IPV6].aclaction.parameter.ip6,
                        swss::IpAddress("fdf8:f53b::53").getV6Addr(), 16));

    // Mirror ingress action
    app_db_entry.action = "mirror_ingress";
    app_db_entry.action_param_fvs["target"] = gMirrorSession1;
    // Update rule
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .Times(3)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);

    // Check action field value
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS].aclaction.enable);
    EXPECT_EQ(kMirrorSessionOid1, acl_rule->action_mirror_sessions[SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS].oid);
    EXPECT_EQ(gMirrorSession1, acl_rule->action_mirror_sessions[SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS].name);

    app_db_entry.action_param_fvs["target"] = gMirrorSession2;

    // Update rule
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);

    // Check action field value
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS].aclaction.enable);
    EXPECT_EQ(kMirrorSessionOid2, acl_rule->action_mirror_sessions[SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS].oid);
    EXPECT_EQ(gMirrorSession2, acl_rule->action_mirror_sessions[SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS].name);

    // Flood action
    app_db_entry.action = "flood";

    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .Times(2)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_FLOOD].aclaction.enable);

    // QOS_QUEUE action to set user defined trap for CPU queue number 3
    int queue_num = 3;
    app_db_entry.action = "qos_queue";
    app_db_entry.action_param_fvs["cpu_queue"] = std::to_string(queue_num);

    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .Times(2)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_USER_TRAP_ID].aclaction.enable);
    EXPECT_EQ(gUserDefinedTrapStartOid + queue_num,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_USER_TRAP_ID].aclaction.parameter.oid);

    // QOS_QUEUE action to set user defined trap CPU queue number 4
    queue_num = 4;
    app_db_entry.action_param_fvs["cpu_queue"] = std::to_string(queue_num);

    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_USER_TRAP_ID].aclaction.enable);
    EXPECT_EQ(gUserDefinedTrapStartOid + queue_num,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_USER_TRAP_ID].aclaction.parameter.oid);
}

TEST_F(AclManagerTest, UpdateAclRuleWithVrfActionChange)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());

    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    const auto &acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");
    const auto &table_name_and_rule_key = concatTableNameAndRuleKey(kAclIngressTableName, acl_rule_key);
    app_db_entry.action = "punt_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "1";
    // Install rule
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    auto *acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);

    // Set VRF
    app_db_entry.action = "set_vrf";
    app_db_entry.action_param_fvs["vrf"] = gVrfName;
    // Update rule
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .Times(3)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_EQ(gVrfOid, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_VRF].aclaction.parameter.oid);
    app_db_entry.action_param_fvs["vrf"] = "";
    // Update rule
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(1, acl_rule->action_fvs.size());
    EXPECT_EQ(gVirtualRouterId, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_VRF].aclaction.parameter.oid);
}

TEST_F(AclManagerTest, UpdateAclRuleFailsWhenSaiCallFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    auto acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "15");

    sai_object_id_t meter_oid;
    uint32_t ref_cnt;
    const auto &table_name_and_rule_key = concatTableNameAndRuleKey(kAclIngressTableName, acl_rule_key);
    app_db_entry.action = "punt_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "1";
    // Install rule
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    auto *acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);

    // Update packet action: Copy green packet. Fails when update meter
    // attribute fails
    app_db_entry.action = "copy_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "2";
    // Update meter attribute for green packet action
    EXPECT_CALL(mock_sai_policer_, set_policer_attribute(Eq(kAclMeterOid1), _))
        .WillOnce(Return(SAI_STATUS_NOT_SUPPORTED));
    EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ("punt_and_set_tc", acl_rule->p4_action);
    EXPECT_EQ(SAI_PACKET_ACTION_TRAP,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.parameter.s32);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.enable);
    EXPECT_EQ(1, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.enable);
    EXPECT_TRUE(p4_oid_mapper_->getOID(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &meter_oid));
    EXPECT_EQ(kAclMeterOid1, meter_oid);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Update packet action and rate limiting. Fails when update meter attribute
    // fails plue meter attribute recovery fails.
    app_db_entry.action = "copy_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "2";
    app_db_entry.meter.cburst = 500;
    app_db_entry.meter.cir = 500;
    app_db_entry.meter.pburst = 600;
    app_db_entry.meter.pir = 600;
    // Update meter attribute for green packet action
    EXPECT_CALL(mock_sai_policer_, set_policer_attribute(Eq(kAclMeterOid1), _))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_FAILURE))
        .WillOnce(Return(SAI_STATUS_FAILURE));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_UNKNOWN, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ("punt_and_set_tc", acl_rule->p4_action);
    EXPECT_EQ(SAI_PACKET_ACTION_TRAP,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.parameter.s32);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.enable);
    EXPECT_EQ(1, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.enable);
    EXPECT_TRUE(p4_oid_mapper_->getOID(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &meter_oid));
    EXPECT_EQ(kAclMeterOid1, meter_oid);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Update packet action: Copy green packet. Fails when action param is
    // missing
    app_db_entry.action = "copy_and_set_tc";
    app_db_entry.action_param_fvs.erase("traffic_class");
    app_db_entry.meter.cburst = 80;
    app_db_entry.meter.cir = 80;
    app_db_entry.meter.pburst = 200;
    app_db_entry.meter.pir = 200;
    // Update meter attribute for green packet action
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ("punt_and_set_tc", acl_rule->p4_action);
    EXPECT_EQ(SAI_PACKET_ACTION_TRAP,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.parameter.s32);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.enable);
    EXPECT_EQ(1, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.enable);
    EXPECT_TRUE(p4_oid_mapper_->getOID(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &meter_oid));
    EXPECT_EQ(kAclMeterOid1, meter_oid);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Update packet action: Copy green packet. Fails when updating ACL rule
    // fails
    app_db_entry.action = "copy_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "2";
    // Update meter attribute for green packet action
    EXPECT_CALL(mock_sai_policer_, set_policer_attribute(Eq(kAclMeterOid1), _))
        .Times(2)
        .WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .WillOnce(Return(SAI_STATUS_NOT_SUPPORTED));
    EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ("punt_and_set_tc", acl_rule->p4_action);
    EXPECT_EQ(SAI_PACKET_ACTION_TRAP,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.parameter.s32);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.enable);
    EXPECT_EQ(1, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.enable);
    EXPECT_TRUE(p4_oid_mapper_->getOID(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &meter_oid));
    EXPECT_EQ(kAclMeterOid1, meter_oid);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Update packet action: Copy green packet. Fails when updating ACL rule
    // fails and meter recovery fails
    app_db_entry.action = "copy_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "2";
    // Update meter attribute for green packet action
    EXPECT_CALL(mock_sai_policer_, set_policer_attribute(Eq(kAclMeterOid1), _))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .WillOnce(Return(SAI_STATUS_NOT_SUPPORTED));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ("punt_and_set_tc", acl_rule->p4_action);
    EXPECT_EQ(SAI_PACKET_ACTION_TRAP,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.parameter.s32);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.enable);
    EXPECT_EQ(1, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.enable);
    EXPECT_TRUE(p4_oid_mapper_->getOID(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &meter_oid));
    EXPECT_EQ(kAclMeterOid1, meter_oid);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Remove meter in ACL rule: fails when deleting meter fails plus ACL rule
    // recovery fails.
    app_db_entry.meter.enabled = false;
    app_db_entry.action = "punt_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "2";
    // Remove meter
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_OBJECT_IN_USE));
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_SUCCESS))
        .WillOnce(Return(SAI_STATUS_FAILURE));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_IN_USE, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ(SAI_PACKET_ACTION_TRAP,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.parameter.s32);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.enable);
    EXPECT_EQ(1, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.enable);
    EXPECT_TRUE(acl_rule->meter.packet_color_actions.empty());
    EXPECT_TRUE(acl_rule->meter.enabled);
    EXPECT_TRUE(p4_oid_mapper_->getOID(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &meter_oid));
    EXPECT_EQ(kAclMeterOid1, meter_oid);
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key, &ref_cnt));
    EXPECT_EQ(1, ref_cnt);

    // Successfully remove meter
    app_db_entry.action = "punt_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "1";
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action field value
    EXPECT_EQ("punt_and_set_tc", acl_rule->p4_action);
    EXPECT_EQ(SAI_PACKET_ACTION_TRAP,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.parameter.s32);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.enable);
    EXPECT_EQ(1, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.enable);
    EXPECT_TRUE(acl_rule->meter.packet_color_actions.empty());
    EXPECT_FALSE(acl_rule->meter.enabled);
    EXPECT_FALSE(p4_oid_mapper_->existsOID(SAI_OBJECT_TYPE_POLICER, table_name_and_rule_key));

    // Add meter in ACL rule with packet color action
    app_db_entry.meter.enabled = true;
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid2), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .WillOnce(Return(SAI_STATUS_OBJECT_IN_USE));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid2))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_IN_USE, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action and meter
    EXPECT_EQ(SAI_PACKET_ACTION_TRAP,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.parameter.s32);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.enable);
    EXPECT_EQ(1, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.enable);
    EXPECT_TRUE(acl_rule->meter.packet_color_actions.empty());
    EXPECT_FALSE(acl_rule->meter.enabled);

    // Add meter in ACL rule with packet color action. Fails when updating ACL
    // rule fails and meter recovery fails
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid2), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, set_acl_entry_attribute(Eq(kAclIngressRuleOid1), _))
        .WillOnce(Return(SAI_STATUS_OBJECT_IN_USE));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid2))).WillOnce(Return(SAI_STATUS_FAILURE));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_IN_USE, ProcessUpdateRuleRequest(app_db_entry, *acl_rule));
    acl_rule = GetAclRule(kAclIngressTableName, acl_rule_key);
    ASSERT_NE(nullptr, acl_rule);
    // Check action and meter
    EXPECT_EQ(SAI_PACKET_ACTION_TRAP,
              acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.parameter.s32);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION].aclaction.enable);
    EXPECT_EQ(1, acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.parameter.u8);
    EXPECT_TRUE(acl_rule->action_fvs[SAI_ACL_ENTRY_ATTR_ACTION_SET_TC].aclaction.enable);
    EXPECT_TRUE(acl_rule->meter.packet_color_actions.empty());
    EXPECT_FALSE(acl_rule->meter.enabled);
}

TEST_F(AclManagerTest, CreateAclRuleWithInvalidActionFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    const auto &acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "15");

    // ACL rule has redirect action with invalid next_hop_id
    const std::string next_hop_id = "ju1u32m1.atl11:qe-3/7";
    app_db_entry.action = "redirect";
    app_db_entry.action_param_fvs["target"] = next_hop_id;
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.action_param_fvs.erase("target");
    // ACL rule has redirect action with wrong port type
    app_db_entry.action_param_fvs["target"] = "Ethernet8";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.action_param_fvs.erase("target");
    // ACL rule has invalid action
    app_db_entry.action = "set_tc";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    // ACL rule has invalid IP address
    app_db_entry.action = "endpoint_ip";
    app_db_entry.action_param_fvs["ip_address"] = "invalid";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.action_param_fvs.erase("ip_address");
    // ACL rule is missing action parameter
    app_db_entry.action = "set_src_ip";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    // ACL rule with invalid action parameter field "ipv4", should be "ip"
    app_db_entry.action = "set_src_ip";
    app_db_entry.action_param_fvs["ipv4"] = "10.0.0.1";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.action_param_fvs.erase("ipv4");
    // ACL rule has invalid MAC address
    app_db_entry.action = "set_src_mac";
    app_db_entry.action_param_fvs["mac_address"] = "invalid";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.action_param_fvs.erase("mac_address");
    // ACL rule with invalid packet action value
    app_db_entry.action = "set_packet_action";
    app_db_entry.action_param_fvs["packet_action"] = "PUNT"; // Invalid packet action str
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.action_param_fvs.erase("packet_action");
    // ACL rule with invalid packet color value
    app_db_entry.action = "set_packet_color";
    app_db_entry.action_param_fvs["packet_color"] = "YELLOW"; // Invalid color str
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.action_param_fvs.erase("packet_color");
    // Invalid Mirror ingress session
    app_db_entry.action = "mirror_ingress";
    app_db_entry.action_param_fvs["target"] = "Session";
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.action_param_fvs.erase("target");
    // Invalid cpu queue number
    app_db_entry.action = "qos_queue";
    app_db_entry.action_param_fvs["cpu_queue"] = "10";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.action_param_fvs["cpu_queue"] = "invalid";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.action_param_fvs.erase("cpu_queue");
    // ACL rule has invalid dscp
    app_db_entry.action = "set_dscp_and_ecn";
    app_db_entry.action_param_fvs["dscp"] = "invalid";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.action_param_fvs.erase("dscp");
    // ACL rule has invalid vlan id
    app_db_entry.action = "set_inner_vlan";
    app_db_entry.action_param_fvs["vlan_id"] = "invalid";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.action_param_fvs.erase("vlan_id");
    // ACL rule has invalid port number
    app_db_entry.action = "set_l4_src_port";
    app_db_entry.action_param_fvs["port"] = "invalid";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    app_db_entry.action_param_fvs.erase("port");
}

TEST_F(AclManagerTest, CreateAclRuleWithInvalidVrfActionFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    const auto &acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "15");

    // ACL rule with invalid VRF name
    app_db_entry.action = "set_vrf";
    app_db_entry.action_param_fvs["vrf"] = "Vrf-yellow";
    EXPECT_EQ(StatusCode::SWSS_RC_NOT_FOUND, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
}

TEST_F(AclManagerTest, CreateAclRuleWithInvalidUnitsInTableFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto *acl_table = GetAclTable(kAclIngressTableName);
    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    app_db_entry.action = "punt_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "0x20";
    const auto &acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "15");

    // Invalid meter unit
    acl_table->meter_unit = "INVALID";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_table->meter_unit = P4_METER_UNIT_BYTES;

    // Invalid counter unit
    acl_table->counter_unit = "INVALID";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    acl_table->counter_unit = P4_COUNTER_UNIT_BYTES;
}

TEST_F(AclManagerTest, CreateAclRuleFailsWhenSaiCallFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    auto acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "15");

    // Set up an next hop mapping
    const std::string next_hop_id = "ju1u32m1.atl11:qe-1/7";
    const auto &next_hop_key = KeyGenerator::generateNextHopKey(next_hop_id);
    p4_oid_mapper_->setOID(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key,
                           /*next_hop_oid=*/1);
    app_db_entry.action = "redirect";
    app_db_entry.action_param_fvs["target"] = next_hop_id;

    // Fails to create ACL rule when sai_acl_api->create_acl_entry() fails
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _)).WillOnce(Return(SAI_STATUS_NOT_IMPLEMENTED));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    uint32_t ref_count;
    EXPECT_TRUE(p4_oid_mapper_->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key, &ref_count));
    EXPECT_EQ(0, ref_count);

    // Set VRF action
    app_db_entry.action = "set_vrf";
    app_db_entry.action_param_fvs["vrf"] = gVrfName;
    acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "15");

    // Fails to create ACL rule when sai_acl_api->create_acl_entry() fails
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _)).WillOnce(Return(SAI_STATUS_NOT_IMPLEMENTED));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, ProcessAddRuleRequest(acl_rule_key, app_db_entry));

    // Fails to create ACL rule when sai_acl_api->create_acl_entry() fails plus
    // meter and counter recovery fails
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _)).WillOnce(Return(SAI_STATUS_NOT_IMPLEMENTED));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(_)).WillOnce(Return(SAI_STATUS_FAILURE));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillOnce(Return(SAI_STATUS_FAILURE));
    // (TODO): Expect critical state x2.
    EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, ProcessAddRuleRequest(acl_rule_key, app_db_entry));

    // Fails to create ACL rule when sai_acl_api->create_policer() fails
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _)).WillOnce(Return(SAI_STATUS_NOT_IMPLEMENTED));
    EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, ProcessAddRuleRequest(acl_rule_key, app_db_entry));

    // Fails to create ACL rule when sai_acl_api->create_acl_counter() fails
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_NOT_IMPLEMENTED));
    EXPECT_CALL(mock_sai_policer_, remove_policer(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, ProcessAddRuleRequest(acl_rule_key, app_db_entry));

    // Fails to create ACL rule when sai_acl_api->create_acl_counter() fails and
    // meter recovery fails
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _)).WillOnce(Return(SAI_STATUS_NOT_IMPLEMENTED));
    EXPECT_CALL(mock_sai_policer_, remove_policer(_)).WillOnce(Return(SAI_STATUS_FAILURE));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_UNIMPLEMENTED, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
}

TEST_F(AclManagerTest, CreateAclRuleWithInvalidSetSrcIpActionFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    const auto &acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    app_db_entry.action = "set_src_ip";
    app_db_entry.action_param_fvs["ip_address"] = "fdf8:f53b:82e4::53";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));

    app_db_entry.action_param_fvs["ip_address"] = "null";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
}

TEST_F(AclManagerTest, CreateAclRuleWithInvalidSetDstIpv6ActionFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    const auto &acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "100");

    app_db_entry.action = "set_dst_ipv6";
    app_db_entry.action_param_fvs["ip_address"] = "10.0.0.2";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));

    app_db_entry.action_param_fvs["ip_address"] = "null";
    EXPECT_EQ(StatusCode::SWSS_RC_INVALID_PARAM, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
}

TEST_F(AclManagerTest, DeleteAclRuleWhenTableDoesNotExistFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    app_db_entry.action = "punt_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "0x20";
    const auto &acl_rule_key = KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, "15");
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .Times(2)
        .WillRepeatedly(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclCounterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .Times(2)
        .WillRepeatedly(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));

    p4_oid_mapper_->decreaseRefCount(SAI_OBJECT_TYPE_ACL_TABLE, kAclIngressTableName);
    p4_oid_mapper_->decreaseRefCount(SAI_OBJECT_TYPE_ACL_TABLE, kAclIngressTableName);
    p4_oid_mapper_->eraseOID(SAI_OBJECT_TYPE_ACL_TABLE, kAclIngressTableName);
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
    // (TODO): Expect critical state.
    EXPECT_EQ(StatusCode::SWSS_RC_INTERNAL, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
}

TEST_F(AclManagerTest, DoAclCounterStatsTaskSucceeds)
{
    auto app_db_def_entry = getDefaultAclTableDefAppDbEntry();
    app_db_def_entry.counter_unit = P4_COUNTER_UNIT_BOTH;
    EXPECT_CALL(mock_sai_acl_, create_acl_table(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclTableIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group_member(_, _, _, _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclGroupMemberIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_udf_, create_udf_group(_, _, _, _))
        .WillRepeatedly(DoAll(SetArgPointee<0>(kUdfGroupOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_udf_, create_udf(_, _, _, _)).Times(3).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    sai_object_id_t user_defined_trap_oid = gUserDefinedTrapStartOid;
    AddDefaultUserTrapsSaiCalls(&user_defined_trap_oid);
    ASSERT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddTableRequest(app_db_def_entry));
    auto counters_table = std::make_unique<swss::Table>(gCountersDb, std::string(COUNTERS_TABLE) +
                                                                         DEFAULT_KEY_SEPARATOR + APP_P4RT_TABLE_NAME);

    // Insert the first ACL rule
    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    const auto &acl_rule_key =
        KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, std::to_string(app_db_entry.priority));
    const auto &counter_stats_key = app_db_entry.db_key;
    std::vector<swss::FieldValueTuple> values;
    std::string stats;
    app_db_entry.action = "set_dst_ipv6";
    app_db_entry.action_param_fvs["ip_address"] = "fdf8:f53b:82e4::53";
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillRepeatedly(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _))
        .WillRepeatedly(DoAll(SetArgPointee<0>(kAclCounterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillRepeatedly(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));

    // Populate counter stats in COUNTERS_DB
    EXPECT_CALL(mock_sai_acl_, get_acl_counter_attribute(Eq(kAclCounterOid1), _, _))
        .WillOnce(DoAll(Invoke([](sai_object_id_t acl_counter_id, uint32_t attr_count, sai_attribute_t *counter_attr) {
                            counter_attr[0].value.u64 = 50;  // packets
                            counter_attr[1].value.u64 = 500; // bytes
                        }),
                        Return(SAI_STATUS_SUCCESS)));
    DoAclCounterStatsTask();
    // Only packets and bytes are populated in COUNTERS_DB
    EXPECT_TRUE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_PACKETS, stats));
    EXPECT_EQ("50", stats);
    EXPECT_TRUE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_BYTES, stats));
    EXPECT_EQ("500", stats);
    EXPECT_FALSE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_GREEN_PACKETS, stats));
    EXPECT_FALSE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_GREEN_BYTES, stats));
    EXPECT_FALSE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_RED_PACKETS, stats));
    EXPECT_FALSE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_RED_BYTES, stats));
    EXPECT_FALSE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_YELLOW_PACKETS, stats));
    EXPECT_FALSE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_YELLOW_BYTES, stats));

    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(Eq(kAclCounterOid1))).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
    EXPECT_EQ(nullptr, GetAclRule(kAclIngressTableName, acl_rule_key));
    EXPECT_FALSE(counters_table->get(counter_stats_key, values));

    // Install rule with packet color GREEN
    app_db_entry.action = "copy_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "0x20";
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    // Populate counter stats in COUNTERS_DB
    EXPECT_CALL(mock_sai_policer_, get_policer_stats(Eq(kAclMeterOid1), _, _, _))
        .WillOnce(DoAll(Invoke([](sai_object_id_t policer_id, uint32_t number_of_counters,
                                  const sai_stat_id_t *counter_ids, uint64_t *counters) {
                            counters[0] = 10;  // green_packets
                            counters[1] = 100; // green_bytes
                        }),
                        Return(SAI_STATUS_SUCCESS)));
    DoAclCounterStatsTask();
    // Only green_packets and green_bytes are populated in COUNTERS_DB
    EXPECT_TRUE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_GREEN_PACKETS, stats));
    EXPECT_EQ("10", stats);

    EXPECT_TRUE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_GREEN_BYTES, stats));
    EXPECT_EQ("100", stats);
    EXPECT_FALSE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_RED_PACKETS, stats));
    EXPECT_FALSE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_RED_BYTES, stats));
    EXPECT_FALSE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_YELLOW_PACKETS, stats));
    EXPECT_FALSE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_YELLOW_BYTES, stats));
    EXPECT_FALSE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_PACKETS, stats));
    EXPECT_FALSE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_BYTES, stats));

    // Remove rule
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
    EXPECT_EQ(nullptr, GetAclRule(kAclIngressTableName, acl_rule_key));
    EXPECT_FALSE(counters_table->get(counter_stats_key, values));

    // Install rule with packet color YELLOW and RED
    app_db_entry.action = "punt_non_green_pk";
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    // Populate counter stats in COUNTERS_DB
    EXPECT_CALL(mock_sai_policer_, get_policer_stats(Eq(kAclMeterOid1), _, _, _))
        .WillOnce(DoAll(Invoke([](sai_object_id_t policer_id, uint32_t number_of_counters,
                                  const sai_stat_id_t *counter_ids, uint64_t *counters) {
                            counters[0] = 20;  // yellow_packets
                            counters[1] = 200; // yellow_bytes
                            counters[2] = 30;  // red_packets
                            counters[3] = 300; // red_bytes
                        }),
                        Return(SAI_STATUS_SUCCESS)));
    DoAclCounterStatsTask();
    // Only yellow/red_packets and yellow/red_bytes are populated in COUNTERS_DB
    EXPECT_TRUE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_YELLOW_PACKETS, stats));
    EXPECT_EQ("20", stats);
    EXPECT_TRUE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_YELLOW_BYTES, stats));
    EXPECT_EQ("200", stats);
    EXPECT_TRUE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_RED_PACKETS, stats));
    EXPECT_EQ("30", stats);
    EXPECT_TRUE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_RED_BYTES, stats));
    EXPECT_EQ("300", stats);
    EXPECT_FALSE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_GREEN_PACKETS, stats));
    EXPECT_FALSE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_GREEN_BYTES, stats));
    EXPECT_FALSE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_PACKETS, stats));
    EXPECT_FALSE(counters_table->hget(counter_stats_key, P4_COUNTER_STATS_BYTES, stats));
    // Remove rule
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
    EXPECT_EQ(nullptr, GetAclRule(kAclIngressTableName, acl_rule_key));
    EXPECT_FALSE(counters_table->get(counter_stats_key, values));
}

TEST_F(AclManagerTest, DoAclCounterStatsTaskFailsWhenSaiCallFails)
{
    ASSERT_NO_FATAL_FAILURE(AddDefaultIngressTable());
    auto counters_table = std::make_unique<swss::Table>(gCountersDb, std::string(COUNTERS_TABLE) +
                                                                         DEFAULT_KEY_SEPARATOR + APP_P4RT_TABLE_NAME);

    // Insert the ACL rule
    auto app_db_entry = getDefaultAclRuleAppDbEntryWithoutAction();
    const auto &acl_rule_key =
        KeyGenerator::generateAclRuleKey(app_db_entry.match_fvs, std::to_string(app_db_entry.priority));
    const auto &counter_stats_key = app_db_entry.db_key;
    std::vector<swss::FieldValueTuple> values;
    std::string stats;
    app_db_entry.action = "set_dst_ipv6";
    app_db_entry.action_param_fvs["ip_address"] = "fdf8:f53b:82e4::53";
    EXPECT_CALL(mock_sai_acl_, create_acl_entry(_, _, _, _))
        .WillRepeatedly(DoAll(SetArgPointee<0>(kAclIngressRuleOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_acl_, create_acl_counter(_, _, _, _))
        .WillRepeatedly(DoAll(SetArgPointee<0>(kAclCounterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_policer_, create_policer(_, _, _, _))
        .WillRepeatedly(DoAll(SetArgPointee<0>(kAclMeterOid1), Return(SAI_STATUS_SUCCESS)));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));

    // Populate counter stats in COUNTERS_DB
    EXPECT_CALL(mock_sai_acl_, get_acl_counter_attribute(Eq(kAclCounterOid1), _, _))
        .WillOnce(Return(SAI_STATUS_NOT_IMPLEMENTED));
    DoAclCounterStatsTask();
    EXPECT_FALSE(counters_table->get(counter_stats_key, values));

    // Remove rule
    EXPECT_CALL(mock_sai_acl_, remove_acl_entry(Eq(kAclIngressRuleOid1))).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_acl_, remove_acl_counter(Eq(kAclCounterOid1))).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_CALL(mock_sai_policer_, remove_policer(Eq(kAclMeterOid1))).WillRepeatedly(Return(SAI_STATUS_SUCCESS));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
    EXPECT_EQ(nullptr, GetAclRule(kAclIngressTableName, acl_rule_key));

    // Install rule with packet color GREEN
    app_db_entry.action = "copy_and_set_tc";
    app_db_entry.action_param_fvs["traffic_class"] = "0x20";
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessAddRuleRequest(acl_rule_key, app_db_entry));
    // Fails when get_policer_stats() is not implemented
    EXPECT_CALL(mock_sai_policer_, get_policer_stats(Eq(kAclMeterOid1), _, _, _))
        .WillOnce(Return(SAI_STATUS_NOT_SUPPORTED));
    DoAclCounterStatsTask();
    EXPECT_FALSE(counters_table->get(counter_stats_key, values));
    EXPECT_EQ(StatusCode::SWSS_RC_SUCCESS, ProcessDeleteRuleRequest(kAclIngressTableName, acl_rule_key));
    EXPECT_EQ(nullptr, GetAclRule(kAclIngressTableName, acl_rule_key));
}

TEST_F(AclManagerTest, DISABLED_InitCreateGroupFails)
{
    // Failed to create ACL groups
    EXPECT_CALL(mock_sai_serialize_, sai_serialize_object_id(_)).WillRepeatedly(Return(EMPTY_STRING));
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group(_, Eq(gSwitchId), Eq(3), _))
        .WillOnce(Return(SAI_STATUS_TABLE_FULL));
    TableConnector stateDbSwitchTable(gStateDb, "SWITCH_CAPABILITY");
    TableConnector app_switch_table(gAppDb, APP_SWITCH_TABLE_NAME);
    TableConnector conf_asic_sensors(gConfigDb, CFG_ASIC_SENSORS_TABLE_NAME);
    std::vector<TableConnector> switch_tables = {conf_asic_sensors, app_switch_table};
    EXPECT_THROW(new SwitchOrch(gAppDb, switch_tables, stateDbSwitchTable), std::runtime_error);
}

TEST_F(AclManagerTest, DISABLED_InitBindGroupToSwitchFails)
{
    EXPECT_CALL(mock_sai_serialize_, sai_serialize_object_id(_)).WillRepeatedly(Return(EMPTY_STRING));
    // Failed to bind ACL group to switch attribute.
    EXPECT_CALL(mock_sai_acl_, create_acl_table_group(_, Eq(gSwitchId), Eq(3), _))
        .WillOnce(DoAll(SetArgPointee<0>(kAclGroupIngressOid), Return(SAI_STATUS_SUCCESS)));
    EXPECT_CALL(mock_sai_switch_, set_switch_attribute(Eq(gSwitchId), _)).WillOnce(Return(SAI_STATUS_FAILURE));
    TableConnector stateDbSwitchTable(gStateDb, "SWITCH_CAPABILITY");
    TableConnector app_switch_table(gAppDb, APP_SWITCH_TABLE_NAME);
    TableConnector conf_asic_sensors(gConfigDb, CFG_ASIC_SENSORS_TABLE_NAME);
    std::vector<TableConnector> switch_tables = {conf_asic_sensors, app_switch_table};
    EXPECT_THROW(new SwitchOrch(gAppDb, switch_tables, stateDbSwitchTable), std::runtime_error);
}

} // namespace test
} // namespace p4orch
