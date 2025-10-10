#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

#include "acltable.h"
#include "p4orch/p4orch_util.h"
#include "return_code.h"
extern "C"
{
#include "sai.h"
#include "saiextensions.h"
}

namespace p4orch
{

// Describes the format of a value.
enum Format
{
    // Hex string, e.g. 0x0a8b. All lowercase, and always of length
    // ceil(num_bits/4)+2 (1 character for every 4 bits, zero-padded to be
    // divisible by 4, and 2 characters for the '0x' prefix).
    HEX_STRING = 0,
    // MAC address, e.g. 00:11:ab:cd:ef:22. All lowercase, and always 17
    // characters long.
    MAC = 1,
    // IPv4 address, e.g. 10.0.0.2.
    IPV4 = 2,
    // IPv6 address, e.g. fe80::21a:11ff:fe17:5f80. All lowercase, formatted
    // according to RFC5952. This can be used for any bitwidth of 128 or less. If
    // the bitwidth n is less than 128, then by convention only the upper n bits
    // can be set.
    IPV6 = 3,
    // String format, only printable characters.
    STRING = 4,
};

struct P4AclCounter
{
    sai_object_id_t counter_oid;
    bool bytes_enabled;
    bool packets_enabled;
    P4AclCounter() : bytes_enabled(false), packets_enabled(false), counter_oid(SAI_NULL_OBJECT_ID)
    {
    }

    bool operator==(const P4AclCounter &entry) const
    {
        return bytes_enabled == entry.bytes_enabled && packets_enabled == entry.packets_enabled;
    }

    bool operator!=(const P4AclCounter &entry) const
    {
        return !(*this == entry);
    }
};

struct P4AclMeter
{
    sai_object_id_t meter_oid;
    bool enabled;
    sai_meter_type_t type;
    sai_policer_mode_t mode;
    sai_uint64_t cir;
    sai_uint64_t cburst;
    sai_uint64_t pir;
    sai_uint64_t pburst;
    std::string policer_label;

    std::map<sai_policer_attr_t, sai_packet_action_t> packet_color_actions;

    P4AclMeter()
        : enabled(false), meter_oid(SAI_NULL_OBJECT_ID), cir(0), cburst(0), pir(0), pburst(0),
          type(SAI_METER_TYPE_PACKETS), mode(SAI_POLICER_MODE_TR_TCM)
    {
    }

    bool operator==(const P4AclMeter &entry) const
    {
        return enabled == entry.enabled && type == entry.type && mode == entry.mode && cir == entry.cir &&
               cburst == entry.cburst && pir == entry.pir && pburst == entry.pburst &&
               packet_color_actions == entry.packet_color_actions;
    }

    bool operator!=(const P4AclMeter &entry) const
    {
        return !(*this == entry);
    }
};

struct P4AclMirrorSession
{
    std::string name;
    std::string key; // KeyGenerator::generateMirrorSessionKey(name)
    sai_object_id_t oid;

    bool operator==(const P4AclMirrorSession &entry) const
    {
        return name == entry.name && key == entry.key && oid == entry.oid;
    }

    bool operator!=(const P4AclMirrorSession &entry) const
    {
        return !(*this == entry);
    }
};

struct P4UdfDataMask
{
    std::vector<uint8_t> data;
    std::vector<uint8_t> mask;

    bool operator==(const P4UdfDataMask &entry) const
    {
        return data == entry.data && mask == entry.mask;
    }

    bool operator!=(const P4UdfDataMask &entry) const
    {
        return !(*this == entry);
    }
};

struct P4AclRule
{
    sai_object_id_t acl_table_oid;
    sai_object_id_t acl_entry_oid;
    std::string acl_table_name;
    std::string acl_rule_key;
    std::string db_key;

    sai_uint32_t priority;
    std::string p4_action;
    std::map<sai_acl_entry_attr_t, sai_attribute_value_t> match_fvs;
    std::map<sai_acl_entry_attr_t, sai_attribute_value_t> action_fvs;
    P4AclMeter meter;
    P4AclCounter counter;

    sai_uint32_t action_qos_queue_num;
    std::string action_redirect_nexthop_key;
    // SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS and
    // SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_EGRESS are allowed as key
    std::map<sai_acl_entry_attr_t, P4AclMirrorSession> action_mirror_sessions;
    // Stores mapping from SAI_ACL_TABLE_ATTR_USER_DEFINED_FIELD_GROUP_{number} to
    // udf data and masks pairs in two uin8_t list
    std::map<sai_acl_entry_attr_t, P4UdfDataMask> udf_data_masks;
    std::vector<std::string> in_ports;
    std::vector<std::string> out_ports;
    std::vector<sai_object_id_t> in_ports_oids;
    std::vector<sai_object_id_t> out_ports_oids;
};

struct SaiActionWithParam
{
  sai_acl_entry_attr_t action;
  std::string param_name;
  std::string param_value;

  bool operator==(const SaiActionWithParam& entry) const {
    return action == entry.action && param_name == entry.param_name &&
           param_value == entry.param_value;
  }

    bool operator!=(const SaiActionWithParam &entry) const
    {
        return !(*this == entry);
    }
};

struct SaiMatchField
{
  sai_acl_entry_attr_t entry_attr;
  sai_acl_table_attr_t table_attr;
  uint32_t bitwidth;
  Format format;

  bool operator==(const SaiMatchField& entry) const {
    return entry_attr == entry.entry_attr && table_attr == entry.table_attr &&
           bitwidth == entry.bitwidth && format == entry.format;
  }

    bool operator!=(const SaiMatchField &entry) const
    {
        return !(*this == entry);
    }
};

struct P4UdfField
{
    uint16_t length;      // in Bytes
    std::string group_id; // {ACL_TABLE_NAME}-{P4_MATCH_FIELD}-{INDEX}
    std::string udf_id;   // {group_id}-base{base}-offset{offset}
    uint16_t offset;      // in Bytes
    sai_udf_base_t base;

    bool operator==(const P4UdfField &entry) const
    {
        return length == entry.length && group_id == entry.group_id && udf_id == entry.udf_id &&
               offset == entry.offset && base == entry.base;
    }

    bool operator!=(const P4UdfField &entry) const
    {
        return !(*this == entry);
    }
};

struct P4AclTableDefinition
{
    std::string acl_table_name;
    sai_object_id_t table_oid;
    sai_object_id_t group_oid;
    sai_object_id_t group_member_oid;

    sai_acl_stage_t stage;
    sai_uint32_t size;
    sai_uint32_t priority;
    std::string meter_unit;
    std::string counter_unit;
    // go/p4-composite-fields
    // Only SAI attributes for IPv6-64bit(IPV6_WORDn) are supported as
    // sai_field elements in composite field
    std::map<std::string, std::vector<SaiMatchField>> composite_sai_match_fields_lookup;
    // go/gpins-acl-udf
    // p4_match string to a list of P4UdfFields mapping
    std::map<std::string, std::vector<P4UdfField>> udf_fields_lookup;
    // UDF group id to ACL entry attribute index mapping
    std::map<std::string, uint16_t> udf_group_attr_index_lookup;
    std::map<std::string, SaiMatchField> sai_match_field_lookup;
    std::map<std::string, std::string> ip_type_bit_type_lookup;
    std::map<std::string, std::vector<SaiActionWithParam>> rule_action_field_lookup;
    std::map<std::string, std::map<sai_policer_attr_t, sai_packet_action_t>> rule_packet_action_color_lookup;
    std::vector<sai_acl_action_type_t> acl_action_type_list;

    P4AclTableDefinition() = default;
    P4AclTableDefinition(const std::string &acl_table_name, const sai_acl_stage_t stage, const uint32_t priority,
                         const uint32_t size, const std::string &meter_unit, const std::string &counter_unit)
        : acl_table_name(acl_table_name), stage(stage), priority(priority), size(size), meter_unit(meter_unit),
          counter_unit(counter_unit) {};
};

struct P4UserDefinedTrapHostifTableEntry
{
    sai_object_id_t user_defined_trap;
    sai_object_id_t hostif_table_entry;
    P4UserDefinedTrapHostifTableEntry()
        : user_defined_trap(SAI_NULL_OBJECT_ID), hostif_table_entry(SAI_NULL_OBJECT_ID) {};
};

using acl_rule_attr_lookup_t = std::map<std::string, sai_acl_entry_attr_t>;
using acl_table_attr_lookup_t = std::map<std::string, sai_acl_table_attr_t>;
using acl_table_attr_format_lookup_t = std::map<sai_acl_table_attr_t, Format>;
using acl_packet_action_lookup_t = std::map<std::string, sai_packet_action_t>;
using acl_packet_color_lookup_t = std::map<std::string, sai_packet_color_t>;
using acl_packet_color_policer_attr_lookup_t = std::map<std::string, sai_policer_attr_t>;
using acl_ip_type_lookup_t = std::map<std::string, sai_acl_ip_type_t>;
using acl_ip_frag_lookup_t = std::map<std::string, sai_acl_ip_frag_t>;
using udf_base_lookup_t = std::map<std::string, sai_udf_base_t>;
using acl_packet_vlan_lookup_t = std::map<std::string, sai_packet_vlan_t>;
using P4AclTableDefinitions = std::map<std::string, P4AclTableDefinition>;
using P4AclRuleTables = std::map<std::string, std::map<std::string, P4AclRule>>;

#define P4_FORMAT_HEX_STRING "HEX_STRING"
#define P4_FORMAT_MAC "MAC"
#define P4_FORMAT_IPV4 "IPV4"
#define P4_FORMAT_IPV6 "IPV6"
#define P4_FORMAT_STRING "STRING"

#define P4_MATCH_IN_PORT "SAI_ACL_TABLE_ATTR_FIELD_IN_PORT"
#define P4_MATCH_OUT_PORT "SAI_ACL_TABLE_ATTR_FIELD_OUT_PORT"
#define P4_MATCH_IN_PORTS "SAI_ACL_TABLE_ATTR_FIELD_IN_PORTS"
#define P4_MATCH_OUT_PORTS "SAI_ACL_TABLE_ATTR_FIELD_OUT_PORTS"
#define P4_MATCH_SRC_IP "SAI_ACL_TABLE_ATTR_FIELD_SRC_IP"
#define P4_MATCH_DST_IP "SAI_ACL_TABLE_ATTR_FIELD_DST_IP"
#define P4_MATCH_INNER_SRC_IP "SAI_ACL_TABLE_ATTR_FIELD_INNER_SRC_IP"
#define P4_MATCH_INNER_DST_IP "SAI_ACL_TABLE_ATTR_FIELD_INNER_DST_IP"
#define P4_MATCH_SRC_IPV6 "SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6"
#define P4_MATCH_DST_IPV6 "SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6"
#define P4_MATCH_INNER_SRC_IPV6 "SAI_ACL_TABLE_ATTR_FIELD_INNER_SRC_IPV6"
#define P4_MATCH_INNER_DST_IPV6 "SAI_ACL_TABLE_ATTR_FIELD_INNER_DST_IPV6"
#define P4_MATCH_SRC_MAC "SAI_ACL_TABLE_ATTR_FIELD_SRC_MAC"
#define P4_MATCH_DST_MAC "SAI_ACL_TABLE_ATTR_FIELD_DST_MAC"
#define P4_MATCH_OUTER_VLAN_ID "SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_ID"
#define P4_MATCH_OUTER_VLAN_PRI "SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_PRI"
#define P4_MATCH_OUTER_VLAN_CFI "SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_CFI"
#define P4_MATCH_INNER_VLAN_ID "SAI_ACL_TABLE_ATTR_FIELD_INNER_VLAN_ID"
#define P4_MATCH_INNER_VLAN_PRI "SAI_ACL_TABLE_ATTR_FIELD_INNER_VLAN_PRI"
#define P4_MATCH_INNER_VLAN_CFI "SAI_ACL_TABLE_ATTR_FIELD_INNER_VLAN_CFI"
#define P4_MATCH_L4_SRC_PORT "SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT"
#define P4_MATCH_L4_DST_PORT "SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT"
#define P4_MATCH_INNER_L4_SRC_PORT "SAI_ACL_TABLE_ATTR_FIELD_INNER_L4_SRC_PORT"
#define P4_MATCH_INNER_L4_DST_PORT "SAI_ACL_TABLE_ATTR_FIELD_INNER_L4_DST_PORT"
#define P4_MATCH_ETHER_TYPE "SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE"
#define P4_MATCH_INNER_ETHER_TYPE "SAI_ACL_TABLE_ATTR_FIELD_INNER_ETHER_TYPE"
#define P4_MATCH_IP_PROTOCOL "SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL"
#define P4_MATCH_INNER_IP_PROTOCOL "SAI_ACL_TABLE_ATTR_FIELD_INNER_IP_PROTOCOL"
#define P4_MATCH_IP_ID "SAI_ACL_TABLE_ATTR_FIELD_IP_IDENTIFICATION"
#define P4_MATCH_DSCP "SAI_ACL_TABLE_ATTR_FIELD_DSCP"
#define P4_MATCH_ECN "SAI_ACL_TABLE_ATTR_FIELD_ECN"
#define P4_MATCH_TTL "SAI_ACL_TABLE_ATTR_FIELD_TTL"
#define P4_MATCH_TOS "SAI_ACL_TABLE_ATTR_FIELD_TOS"
#define P4_MATCH_IP_FLAGS "SAI_ACL_TABLE_ATTR_FIELD_IP_FLAGS"
#define P4_MATCH_TCP_FLAGS "SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS"
#define P4_MATCH_IP_TYPE "SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE"
#define P4_MATCH_IP_FRAG "SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_FRAG"
#define P4_MATCH_IPV6_FLOW_LABEL "SAI_ACL_TABLE_ATTR_FIELD_IPV6_FLOW_LABEL"
#define P4_MATCH_TRAFFIC_CLASS "SAI_ACL_TABLE_ATTR_FIELD_TC"
#define P4_MATCH_ICMP_TYPE "SAI_ACL_TABLE_ATTR_FIELD_ICMP_TYPE"
#define P4_MATCH_ICMP_CODE "SAI_ACL_TABLE_ATTR_FIELD_ICMP_CODE"
#define P4_MATCH_ICMPV6_TYPE "SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_TYPE"
#define P4_MATCH_ICMPV6_CODE "SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_CODE"
#define P4_MATCH_PACKET_VLAN "SAI_ACL_TABLE_ATTR_FIELD_PACKET_VLAN"
#define P4_MATCH_TUNNEL_VNI "SAI_ACL_TABLE_ATTR_FIELD_TUNNEL_VNI"
#define P4_MATCH_IPV6_NEXT_HEADER "SAI_ACL_TABLE_ATTR_FIELD_IPV6_NEXT_HEADER"
#define P4_MATCH_DST_IPV6_WORD3 "SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6_WORD3"
#define P4_MATCH_DST_IPV6_WORD2 "SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6_WORD2"
#define P4_MATCH_SRC_IPV6_WORD3 "SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD3"
#define P4_MATCH_SRC_IPV6_WORD2 "SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD2"
#define P4_MATCH_ROUTE_DST_USER_META "SAI_ACL_TABLE_ATTR_FIELD_ROUTE_DST_USER_META"
#define P4_MATCH_ACL_USER_META "SAI_ACL_TABLE_ATTR_FIELD_ACL_USER_META"
#define P4_MATCH_VRF_ID "SAI_ACL_TABLE_ATTR_FIELD_VRF_ID"
#define P4_MATCH_IPMC_TABLE_HIT "SAI_ACL_TABLE_ATTR_FIELD_IPMC_NPU_META_DST_HIT"

#define P4_ACTION_PACKET_ACTION "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION"
#define P4_ACTION_REDIRECT "SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT"
// Tunnel Endpoint IP. mandatory and valid only when redirect action is to
// SAI_BRIDGE_PORT_TYPE_TUNNEL
#define P4_ACTION_ENDPOINT_IP "SAI_ACL_ENTRY_ATTR_ACTION_ENDPOINT_IP"
#define P4_ACTION_MIRROR_INGRESS "SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS"
#define P4_ACTION_MIRROR_EGRESS "SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_EGRESS"
#define P4_ACTION_FLOOD "SAI_ACL_ENTRY_ATTR_ACTION_FLOOD"
#define P4_ACTION_DECREMENT_TTL "SAI_ACL_ENTRY_ATTR_ACTION_DECREMENT_TTL"
#define P4_ACTION_SET_TRAFFIC_CLASS "SAI_ACL_ENTRY_ATTR_ACTION_SET_TC"
#define P4_ACTION_SET_PACKET_COLOR "SAI_ACL_ENTRY_ATTR_ACTION_SET_PACKET_COLOR"
#define P4_ACTION_SET_INNER_VLAN_ID "SAI_ACL_ENTRY_ATTR_ACTION_SET_INNER_VLAN_ID"
#define P4_ACTION_SET_INNER_VLAN_PRIORITY "SAI_ACL_ENTRY_ATTR_ACTION_SET_INNER_VLAN_PRI"
#define P4_ACTION_SET_OUTER_VLAN_ID "SAI_ACL_ENTRY_ATTR_ACTION_SET_OUTER_VLAN_ID"
#define P4_ACTION_SET_OUTER_VLAN_PRIORITY "SAI_ACL_ENTRY_ATTR_ACTION_SET_OUTER_VLAN_PRI"
#define P4_ACTION_SET_SRC_MAC "SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_MAC"
#define P4_ACTION_SET_DST_MAC "SAI_ACL_ENTRY_ATTR_ACTION_SET_DST_MAC"
#define P4_ACTION_SET_SRC_IP "SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_IP"
#define P4_ACTION_SET_DST_IP "SAI_ACL_ENTRY_ATTR_ACTION_SET_DST_IP"
#define P4_ACTION_SET_SRC_IPV6 "SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_IPV6"
#define P4_ACTION_SET_DST_IPV6 "SAI_ACL_ENTRY_ATTR_ACTION_SET_DST_IPV6"
#define P4_ACTION_SET_DSCP "SAI_ACL_ENTRY_ATTR_ACTION_SET_DSCP"
#define P4_ACTION_SET_ECN "SAI_ACL_ENTRY_ATTR_ACTION_SET_ECN"
#define P4_ACTION_SET_L4_SRC_PORT "SAI_ACL_ENTRY_ATTR_ACTION_SET_L4_SRC_PORT"
#define P4_ACTION_SET_L4_DST_PORT "SAI_ACL_ENTRY_ATTR_ACTION_SET_L4_DST_PORT"
#define P4_ACTION_SET_DO_NOT_LEARN "SAI_ACL_ENTRY_ATTR_ACTION_SET_DO_NOT_LEARN"
#define P4_ACTION_SET_VRF "SAI_ACL_ENTRY_ATTR_ACTION_SET_VRF"
#define P4_ACTION_SET_ACL_META_DATA "SAI_ACL_ENTRY_ATTR_ACTION_SET_ACL_META_DATA"
#define P4_ACTION_SET_QOS_QUEUE "QOS_QUEUE"

#define P4_PACKET_ACTION_FORWARD "SAI_PACKET_ACTION_FORWARD"
#define P4_PACKET_ACTION_DROP "SAI_PACKET_ACTION_DROP"
#define P4_PACKET_ACTION_COPY "SAI_PACKET_ACTION_COPY"
#define P4_PACKET_ACTION_PUNT "SAI_PACKET_ACTION_TRAP"
#define P4_PACKET_ACTION_LOG "SAI_PACKET_ACTION_LOG"
#define P4_PACKET_ACTION_COPY_CANCEL "SAI_PACKET_ACTION_COPY_CANCEL"
#define P4_PACKET_ACTION_DENY "SAI_PACKET_ACTION_DENY"


#define P4_PACKET_ACTION_REDIRECT "REDIRECT"

#define P4_PACKET_COLOR_GREEN "SAI_PACKET_COLOR_GREEN"
#define P4_PACKET_COLOR_YELLOW "SAI_PACKET_COLOR_YELLOW"
#define P4_PACKET_COLOR_RED "SAI_PACKET_COLOR_RED"

#define P4_METER_UNIT_PACKETS "PACKETS"
#define P4_METER_UNIT_BYTES "BYTES"

#define P4_COUNTER_UNIT_PACKETS "PACKETS"
#define P4_COUNTER_UNIT_BYTES "BYTES"
#define P4_COUNTER_UNIT_BOTH "BOTH"

// IP_TYPE encode in p4. go/p4-ip-type
#define P4_IP_TYPE_BIT_IP "IP"
#define P4_IP_TYPE_BIT_IPV4ANY "IPV4ANY"
#define P4_IP_TYPE_BIT_IPV6ANY "IPV6ANY"
#define P4_IP_TYPE_BIT_ARP "ARP"
#define P4_IP_TYPE_BIT_ARP_REQUEST "ARP_REQUEST"
#define P4_IP_TYPE_BIT_ARP_REPLY "ARP_REPLY"

#define P4_IP_TYPE_ANY "SAI_ACL_IP_TYPE_ANY"
#define P4_IP_TYPE_IP "SAI_ACL_IP_TYPE_IP"
#define P4_IP_TYPE_NON_IP "SAI_ACL_IP_TYPE_NON_IP"
#define P4_IP_TYPE_IPV4ANY "SAI_ACL_IP_TYPE_IPV4ANY"
#define P4_IP_TYPE_NON_IPV4 "SAI_ACL_IP_TYPE_NON_IPV4"
#define P4_IP_TYPE_IPV6ANY "SAI_ACL_IP_TYPE_IPV6ANY"
#define P4_IP_TYPE_NON_IPV6 "SAI_ACL_IP_TYPE_NON_IPV6"
#define P4_IP_TYPE_ARP "SAI_ACL_IP_TYPE_ARP"
#define P4_IP_TYPE_ARP_REQUEST "SAI_ACL_IP_TYPE_ARP_REQUEST"
#define P4_IP_TYPE_ARP_REPLY "SAI_ACL_IP_TYPE_ARP_REPLY"

#define P4_IP_FRAG_ANY "SAI_ACL_IP_FRAG_ANY"
#define P4_IP_FRAG_NON_FRAG "SAI_ACL_IP_FRAG_NON_FRAG"
#define P4_IP_FRAG_NON_FRAG_OR_HEAD "SAI_ACL_IP_FRAG_NON_FRAG_OR_HEAD"
#define P4_IP_FRAG_HEAD "SAI_ACL_IP_FRAG_HEAD"
#define P4_IP_FRAG_NON_HEAD "SAI_ACL_IP_FRAG_NON_HEAD"

#define P4_PACKET_VLAN_UNTAG "SAI_PACKET_VLAN_UNTAG"
#define P4_PACKET_VLAN_SINGLE_OUTER_TAG "SAI_PACKET_VLAN_SINGLE_OUTER_TAG"
#define P4_PACKET_VLAN_DOUBLE_TAG "SAI_PACKET_VLAN_DOUBLE_TAG"

#define P4_UDF_MATCH_DEFAULT "acl_default_udf_match"

// ACL counters update interval in the COUNTERS_DB
// Value is in seconds. Should not be less than 5 seconds
// (in worst case update of 1265 counters takes almost 5 sec)
#define P4_COUNTERS_READ_INTERVAL 10

#define P4_COUNTER_STATS_PACKETS "packets"
#define P4_COUNTER_STATS_BYTES "bytes"
#define P4_COUNTER_STATS_GREEN_PACKETS "green_packets"
#define P4_COUNTER_STATS_GREEN_BYTES "green_bytes"
#define P4_COUNTER_STATS_YELLOW_PACKETS "yellow_packets"
#define P4_COUNTER_STATS_YELLOW_BYTES "yellow_bytes"
#define P4_COUNTER_STATS_RED_PACKETS "red_packets"
#define P4_COUNTER_STATS_RED_BYTES "red_bytes"

#define P4_UDF_BASE_L2 "SAI_UDF_BASE_L2"
#define P4_UDF_BASE_L3 "SAI_UDF_BASE_L3"
#define P4_UDF_BASE_L4 "SAI_UDF_BASE_L4"

#define GENL_PACKET_TRAP_GROUP_NAME_PREFIX "trap.group.cpu.queue."

#define EMPTY_STRING ""

// TODO :  To avoid existing p4 tests failure, extend the queue
// temporarily, should set to 7-14 later.
#define P4_CPU_QUEUE_MIN_NUM 1 // 7
#define P4_CPU_QUEUE_MAX_NUM 15 // 14

#define IPV6_SINGLE_WORD_BYTES_LENGTH 4
#define BYTE_BITWIDTH 8

static const std::map<std::string, Format> formatLookup = {
    {P4_FORMAT_HEX_STRING, Format::HEX_STRING},
    {P4_FORMAT_MAC, Format::MAC},
    {P4_FORMAT_IPV4, Format::IPV4},
    {P4_FORMAT_IPV6, Format::IPV6},
    {P4_FORMAT_STRING, Format::STRING},
};

static const acl_table_attr_lookup_t aclMatchTableAttrLookup = {
    {P4_MATCH_IN_PORT, SAI_ACL_TABLE_ATTR_FIELD_IN_PORT},
    {P4_MATCH_OUT_PORT, SAI_ACL_TABLE_ATTR_FIELD_OUT_PORT},
    {P4_MATCH_IN_PORTS, SAI_ACL_TABLE_ATTR_FIELD_IN_PORTS},
    {P4_MATCH_OUT_PORTS, SAI_ACL_TABLE_ATTR_FIELD_OUT_PORTS},
    {P4_MATCH_SRC_MAC, SAI_ACL_TABLE_ATTR_FIELD_SRC_MAC},
    {P4_MATCH_DST_MAC, SAI_ACL_TABLE_ATTR_FIELD_DST_MAC},
    {P4_MATCH_SRC_IP, SAI_ACL_TABLE_ATTR_FIELD_SRC_IP},
    {P4_MATCH_DST_IP, SAI_ACL_TABLE_ATTR_FIELD_DST_IP},
    {P4_MATCH_INNER_SRC_IP, SAI_ACL_TABLE_ATTR_FIELD_INNER_SRC_IP},
    {P4_MATCH_INNER_DST_IP, SAI_ACL_TABLE_ATTR_FIELD_INNER_DST_IP},
    {P4_MATCH_SRC_IPV6, SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6},
    {P4_MATCH_DST_IPV6, SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6},
    {P4_MATCH_INNER_SRC_IPV6, SAI_ACL_TABLE_ATTR_FIELD_INNER_SRC_IPV6},
    {P4_MATCH_INNER_DST_IPV6, SAI_ACL_TABLE_ATTR_FIELD_INNER_DST_IPV6},
    {P4_MATCH_OUTER_VLAN_ID, SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_ID},
    {P4_MATCH_OUTER_VLAN_PRI, SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_PRI},
    {P4_MATCH_OUTER_VLAN_CFI, SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_CFI},
    {P4_MATCH_INNER_VLAN_ID, SAI_ACL_TABLE_ATTR_FIELD_INNER_VLAN_ID},
    {P4_MATCH_INNER_VLAN_PRI, SAI_ACL_TABLE_ATTR_FIELD_INNER_VLAN_PRI},
    {P4_MATCH_INNER_VLAN_CFI, SAI_ACL_TABLE_ATTR_FIELD_INNER_VLAN_CFI},
    {P4_MATCH_L4_SRC_PORT, SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT},
    {P4_MATCH_L4_DST_PORT, SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT},
    {P4_MATCH_INNER_L4_SRC_PORT, SAI_ACL_TABLE_ATTR_FIELD_INNER_L4_SRC_PORT},
    {P4_MATCH_INNER_L4_DST_PORT, SAI_ACL_TABLE_ATTR_FIELD_INNER_L4_DST_PORT},
    {P4_MATCH_ETHER_TYPE, SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE},
    {P4_MATCH_IP_PROTOCOL, SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL},
    {P4_MATCH_INNER_IP_PROTOCOL, SAI_ACL_TABLE_ATTR_FIELD_INNER_IP_PROTOCOL},
    {P4_MATCH_IP_ID, SAI_ACL_TABLE_ATTR_FIELD_IP_IDENTIFICATION},
    {P4_MATCH_DSCP, SAI_ACL_TABLE_ATTR_FIELD_DSCP},
    {P4_MATCH_ECN, SAI_ACL_TABLE_ATTR_FIELD_ECN},
    {P4_MATCH_TTL, SAI_ACL_TABLE_ATTR_FIELD_TTL},
    {P4_MATCH_TOS, SAI_ACL_TABLE_ATTR_FIELD_TOS},
    {P4_MATCH_IP_FLAGS, SAI_ACL_TABLE_ATTR_FIELD_IP_FLAGS},
    {P4_MATCH_TCP_FLAGS, SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS},
    {P4_MATCH_IP_TYPE, SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE},
    {P4_MATCH_IP_FRAG, SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_FRAG},
    {P4_MATCH_IPV6_FLOW_LABEL, SAI_ACL_TABLE_ATTR_FIELD_IPV6_FLOW_LABEL},
    {P4_MATCH_TRAFFIC_CLASS, SAI_ACL_TABLE_ATTR_FIELD_TC},
    {P4_MATCH_ICMP_TYPE, SAI_ACL_TABLE_ATTR_FIELD_ICMP_TYPE},
    {P4_MATCH_ICMP_CODE, SAI_ACL_TABLE_ATTR_FIELD_ICMP_CODE},
    {P4_MATCH_ICMPV6_TYPE, SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_TYPE},
    {P4_MATCH_ICMPV6_CODE, SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_CODE},
    {P4_MATCH_PACKET_VLAN, SAI_ACL_TABLE_ATTR_FIELD_PACKET_VLAN},
    {P4_MATCH_TUNNEL_VNI, SAI_ACL_TABLE_ATTR_FIELD_TUNNEL_VNI},
    {P4_MATCH_IPV6_NEXT_HEADER, SAI_ACL_TABLE_ATTR_FIELD_IPV6_NEXT_HEADER},
    {P4_MATCH_ROUTE_DST_USER_META, SAI_ACL_TABLE_ATTR_FIELD_ROUTE_DST_USER_META},
    {P4_MATCH_ACL_USER_META, SAI_ACL_TABLE_ATTR_FIELD_ACL_USER_META},
    {P4_MATCH_VRF_ID, SAI_ACL_TABLE_ATTR_FIELD_VRF_ID},
    {P4_MATCH_IPMC_TABLE_HIT, SAI_ACL_TABLE_ATTR_FIELD_IPMC_NPU_META_DST_HIT},
};

static const acl_table_attr_format_lookup_t aclMatchTableAttrFormatLookup = {
    {SAI_ACL_TABLE_ATTR_FIELD_IN_PORT, Format::STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_OUT_PORT, Format::STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_IN_PORTS, Format::STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_OUT_PORTS, Format::STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_SRC_MAC, Format::MAC},
    {SAI_ACL_TABLE_ATTR_FIELD_DST_MAC, Format::MAC},
    {SAI_ACL_TABLE_ATTR_FIELD_SRC_IP, Format::IPV4},
    {SAI_ACL_TABLE_ATTR_FIELD_DST_IP, Format::IPV4},
    {SAI_ACL_TABLE_ATTR_FIELD_INNER_SRC_IP, Format::IPV4},
    {SAI_ACL_TABLE_ATTR_FIELD_INNER_DST_IP, Format::IPV4},
    {SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6, Format::IPV6},
    {SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6, Format::IPV6},
    {SAI_ACL_TABLE_ATTR_FIELD_INNER_SRC_IPV6, Format::IPV6},
    {SAI_ACL_TABLE_ATTR_FIELD_INNER_DST_IPV6, Format::IPV6},
    {SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_ID, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_PRI, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_OUTER_VLAN_CFI, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_INNER_VLAN_ID, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_INNER_VLAN_PRI, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_INNER_VLAN_CFI, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_INNER_L4_SRC_PORT, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_INNER_L4_DST_PORT, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_INNER_IP_PROTOCOL, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_IP_IDENTIFICATION, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_DSCP, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_ECN, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_TTL, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_TOS, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_IP_FLAGS, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_FRAG, Format::STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_IPV6_FLOW_LABEL, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_TC, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_ICMP_TYPE, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_ICMP_CODE, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_TYPE, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_CODE, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_PACKET_VLAN, Format::STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_TUNNEL_VNI, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_IPV6_NEXT_HEADER, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_ROUTE_DST_USER_META, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_ACL_USER_META, Format::HEX_STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_VRF_ID, Format::STRING},
    {SAI_ACL_TABLE_ATTR_FIELD_IPMC_NPU_META_DST_HIT, Format::HEX_STRING},
};

static const acl_table_attr_lookup_t aclCompositeMatchTableAttrLookup = {
    {P4_MATCH_DST_IPV6_WORD3, SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6_WORD3},
    {P4_MATCH_DST_IPV6_WORD2, SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6_WORD2},
    {P4_MATCH_SRC_IPV6_WORD3, SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD3},
    {P4_MATCH_SRC_IPV6_WORD2, SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6_WORD2},
};

static const acl_rule_attr_lookup_t aclMatchEntryAttrLookup = {
    {P4_MATCH_IN_PORT, SAI_ACL_ENTRY_ATTR_FIELD_IN_PORT},
    {P4_MATCH_OUT_PORT, SAI_ACL_ENTRY_ATTR_FIELD_OUT_PORT},
    {P4_MATCH_IN_PORTS, SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS},
    {P4_MATCH_OUT_PORTS, SAI_ACL_ENTRY_ATTR_FIELD_OUT_PORTS},
    {P4_MATCH_SRC_MAC, SAI_ACL_ENTRY_ATTR_FIELD_SRC_MAC},
    {P4_MATCH_DST_MAC, SAI_ACL_ENTRY_ATTR_FIELD_DST_MAC},
    {P4_MATCH_SRC_IP, SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP},
    {P4_MATCH_DST_IP, SAI_ACL_ENTRY_ATTR_FIELD_DST_IP},
    {P4_MATCH_INNER_SRC_IP, SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IP},
    {P4_MATCH_INNER_DST_IP, SAI_ACL_ENTRY_ATTR_FIELD_INNER_DST_IP},
    {P4_MATCH_SRC_IPV6, SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6},
    {P4_MATCH_DST_IPV6, SAI_ACL_ENTRY_ATTR_FIELD_DST_IPV6},
    {P4_MATCH_INNER_SRC_IPV6, SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IPV6},
    {P4_MATCH_INNER_DST_IPV6, SAI_ACL_ENTRY_ATTR_FIELD_INNER_DST_IPV6},
    {P4_MATCH_OUTER_VLAN_ID, SAI_ACL_ENTRY_ATTR_FIELD_OUTER_VLAN_ID},
    {P4_MATCH_OUTER_VLAN_PRI, SAI_ACL_ENTRY_ATTR_FIELD_OUTER_VLAN_PRI},
    {P4_MATCH_OUTER_VLAN_CFI, SAI_ACL_ENTRY_ATTR_FIELD_OUTER_VLAN_CFI},
    {P4_MATCH_INNER_VLAN_ID, SAI_ACL_ENTRY_ATTR_FIELD_INNER_VLAN_ID},
    {P4_MATCH_INNER_VLAN_PRI, SAI_ACL_ENTRY_ATTR_FIELD_INNER_VLAN_PRI},
    {P4_MATCH_INNER_VLAN_CFI, SAI_ACL_ENTRY_ATTR_FIELD_INNER_VLAN_CFI},
    {P4_MATCH_L4_SRC_PORT, SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT},
    {P4_MATCH_L4_DST_PORT, SAI_ACL_ENTRY_ATTR_FIELD_L4_DST_PORT},
    {P4_MATCH_INNER_L4_SRC_PORT, SAI_ACL_ENTRY_ATTR_FIELD_INNER_L4_SRC_PORT},
    {P4_MATCH_INNER_L4_DST_PORT, SAI_ACL_ENTRY_ATTR_FIELD_INNER_L4_DST_PORT},
    {P4_MATCH_ETHER_TYPE, SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE},
    {P4_MATCH_IP_PROTOCOL, SAI_ACL_ENTRY_ATTR_FIELD_IP_PROTOCOL},
    {P4_MATCH_INNER_IP_PROTOCOL, SAI_ACL_ENTRY_ATTR_FIELD_INNER_IP_PROTOCOL},
    {P4_MATCH_IP_ID, SAI_ACL_ENTRY_ATTR_FIELD_IP_IDENTIFICATION},
    {P4_MATCH_DSCP, SAI_ACL_ENTRY_ATTR_FIELD_DSCP},
    {P4_MATCH_ECN, SAI_ACL_ENTRY_ATTR_FIELD_ECN},
    {P4_MATCH_TTL, SAI_ACL_ENTRY_ATTR_FIELD_TTL},
    {P4_MATCH_TOS, SAI_ACL_ENTRY_ATTR_FIELD_TOS},
    {P4_MATCH_IP_FLAGS, SAI_ACL_ENTRY_ATTR_FIELD_IP_FLAGS},
    {P4_MATCH_TCP_FLAGS, SAI_ACL_ENTRY_ATTR_FIELD_TCP_FLAGS},
    {P4_MATCH_IP_TYPE, SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE},
    {P4_MATCH_IP_FRAG, SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_FRAG},
    {P4_MATCH_IPV6_FLOW_LABEL, SAI_ACL_ENTRY_ATTR_FIELD_IPV6_FLOW_LABEL},
    {P4_MATCH_TRAFFIC_CLASS, SAI_ACL_ENTRY_ATTR_FIELD_TC},
    {P4_MATCH_ICMP_TYPE, SAI_ACL_ENTRY_ATTR_FIELD_ICMP_TYPE},
    {P4_MATCH_ICMP_CODE, SAI_ACL_ENTRY_ATTR_FIELD_ICMP_CODE},
    {P4_MATCH_ICMPV6_TYPE, SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_TYPE},
    {P4_MATCH_ICMPV6_CODE, SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_CODE},
    {P4_MATCH_PACKET_VLAN, SAI_ACL_ENTRY_ATTR_FIELD_PACKET_VLAN},
    {P4_MATCH_TUNNEL_VNI, SAI_ACL_ENTRY_ATTR_FIELD_TUNNEL_VNI},
    {P4_MATCH_IPV6_NEXT_HEADER, SAI_ACL_ENTRY_ATTR_FIELD_IPV6_NEXT_HEADER},
    {P4_MATCH_ROUTE_DST_USER_META, SAI_ACL_ENTRY_ATTR_FIELD_ROUTE_DST_USER_META},
    {P4_MATCH_ACL_USER_META, SAI_ACL_ENTRY_ATTR_FIELD_ACL_USER_META},
    {P4_MATCH_VRF_ID, SAI_ACL_ENTRY_ATTR_FIELD_VRF_ID},
    {P4_MATCH_IPMC_TABLE_HIT, SAI_ACL_ENTRY_ATTR_FIELD_IPMC_NPU_META_DST_HIT},
};

static const acl_rule_attr_lookup_t aclCompositeMatchEntryAttrLookup = {
    {P4_MATCH_DST_IPV6_WORD3, SAI_ACL_ENTRY_ATTR_FIELD_DST_IPV6_WORD3},
    {P4_MATCH_DST_IPV6_WORD2, SAI_ACL_ENTRY_ATTR_FIELD_DST_IPV6_WORD2},
    {P4_MATCH_SRC_IPV6_WORD3, SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6_WORD3},
    {P4_MATCH_SRC_IPV6_WORD2, SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6_WORD2},
};

static const acl_packet_action_lookup_t aclPacketActionLookup = {
    {P4_PACKET_ACTION_FORWARD, SAI_PACKET_ACTION_FORWARD}, {P4_PACKET_ACTION_DROP, SAI_PACKET_ACTION_DROP},
    {P4_PACKET_ACTION_COPY, SAI_PACKET_ACTION_COPY},       {P4_PACKET_ACTION_PUNT, SAI_PACKET_ACTION_TRAP},
    {P4_PACKET_ACTION_LOG, SAI_PACKET_ACTION_LOG},
    {P4_PACKET_ACTION_COPY_CANCEL, SAI_PACKET_ACTION_COPY_CANCEL},
    {P4_PACKET_ACTION_DENY, SAI_PACKET_ACTION_DENY},
};

static const acl_rule_attr_lookup_t aclActionLookup = {
    {P4_ACTION_PACKET_ACTION, SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION},
    {P4_ACTION_REDIRECT, SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT},
    {P4_ACTION_ENDPOINT_IP, SAI_ACL_ENTRY_ATTR_ACTION_ENDPOINT_IP},
    {P4_ACTION_FLOOD, SAI_ACL_ENTRY_ATTR_ACTION_FLOOD},
    {P4_ACTION_MIRROR_INGRESS, SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS},
    {P4_ACTION_MIRROR_EGRESS, SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_EGRESS},
    {P4_ACTION_DECREMENT_TTL, SAI_ACL_ENTRY_ATTR_ACTION_DECREMENT_TTL},
    {P4_ACTION_SET_TRAFFIC_CLASS, SAI_ACL_ENTRY_ATTR_ACTION_SET_TC},
    {P4_ACTION_SET_PACKET_COLOR, SAI_ACL_ENTRY_ATTR_ACTION_SET_PACKET_COLOR},
    {P4_ACTION_SET_INNER_VLAN_ID, SAI_ACL_ENTRY_ATTR_ACTION_SET_INNER_VLAN_ID},
    {P4_ACTION_SET_INNER_VLAN_PRIORITY, SAI_ACL_ENTRY_ATTR_ACTION_SET_INNER_VLAN_PRI},
    {P4_ACTION_SET_OUTER_VLAN_ID, SAI_ACL_ENTRY_ATTR_ACTION_SET_OUTER_VLAN_ID},
    {P4_ACTION_SET_OUTER_VLAN_PRIORITY, SAI_ACL_ENTRY_ATTR_ACTION_SET_OUTER_VLAN_PRI},
    {P4_ACTION_SET_SRC_MAC, SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_MAC},
    {P4_ACTION_SET_DST_MAC, SAI_ACL_ENTRY_ATTR_ACTION_SET_DST_MAC},
    {P4_ACTION_SET_SRC_IP, SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_IP},
    {P4_ACTION_SET_DST_IP, SAI_ACL_ENTRY_ATTR_ACTION_SET_DST_IP},
    {P4_ACTION_SET_SRC_IPV6, SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_IPV6},
    {P4_ACTION_SET_DST_IPV6, SAI_ACL_ENTRY_ATTR_ACTION_SET_DST_IPV6},
    {P4_ACTION_SET_DSCP, SAI_ACL_ENTRY_ATTR_ACTION_SET_DSCP},
    {P4_ACTION_SET_ECN, SAI_ACL_ENTRY_ATTR_ACTION_SET_ECN},
    {P4_ACTION_SET_L4_SRC_PORT, SAI_ACL_ENTRY_ATTR_ACTION_SET_L4_SRC_PORT},
    {P4_ACTION_SET_L4_DST_PORT, SAI_ACL_ENTRY_ATTR_ACTION_SET_L4_DST_PORT},
    {P4_ACTION_SET_QOS_QUEUE, SAI_ACL_ENTRY_ATTR_ACTION_SET_USER_TRAP_ID},
    {P4_ACTION_SET_DO_NOT_LEARN, SAI_ACL_ENTRY_ATTR_ACTION_SET_DO_NOT_LEARN},
    {P4_ACTION_SET_VRF, SAI_ACL_ENTRY_ATTR_ACTION_SET_VRF},
    {P4_ACTION_SET_ACL_META_DATA, SAI_ACL_ENTRY_ATTR_ACTION_SET_ACL_META_DATA},
};

static const acl_packet_color_policer_attr_lookup_t aclPacketColorPolicerAttrLookup = {
    {P4_PACKET_COLOR_GREEN, SAI_POLICER_ATTR_GREEN_PACKET_ACTION},
    {P4_PACKET_COLOR_YELLOW, SAI_POLICER_ATTR_YELLOW_PACKET_ACTION},
    {P4_PACKET_COLOR_RED, SAI_POLICER_ATTR_RED_PACKET_ACTION},
};

static const acl_packet_color_lookup_t aclPacketColorLookup = {
    {P4_PACKET_COLOR_GREEN, SAI_PACKET_COLOR_GREEN},
    {P4_PACKET_COLOR_YELLOW, SAI_PACKET_COLOR_YELLOW},
    {P4_PACKET_COLOR_RED, SAI_PACKET_COLOR_RED},
};

static const std::set<std::string> aclIpTypeBitSet = {
    P4_IP_TYPE_BIT_IP,  P4_IP_TYPE_BIT_IPV4ANY,     P4_IP_TYPE_BIT_IPV6ANY,
    P4_IP_TYPE_BIT_ARP, P4_IP_TYPE_BIT_ARP_REQUEST, P4_IP_TYPE_BIT_ARP_REPLY,
};

static const acl_ip_type_lookup_t aclIpTypeLookup = {
    {P4_IP_TYPE_ANY, SAI_ACL_IP_TYPE_ANY},
    {P4_IP_TYPE_IP, SAI_ACL_IP_TYPE_IP},
    {P4_IP_TYPE_NON_IP, SAI_ACL_IP_TYPE_NON_IP},
    {P4_IP_TYPE_IPV4ANY, SAI_ACL_IP_TYPE_IPV4ANY},
    {P4_IP_TYPE_NON_IPV4, SAI_ACL_IP_TYPE_NON_IPV4},
    {P4_IP_TYPE_IPV6ANY, SAI_ACL_IP_TYPE_IPV6ANY},
    {P4_IP_TYPE_NON_IPV6, SAI_ACL_IP_TYPE_NON_IPV6},
    {P4_IP_TYPE_ARP, SAI_ACL_IP_TYPE_ARP},
    {P4_IP_TYPE_ARP_REQUEST, SAI_ACL_IP_TYPE_ARP_REQUEST},
    {P4_IP_TYPE_ARP_REPLY, SAI_ACL_IP_TYPE_ARP_REPLY},
};

static const acl_ip_frag_lookup_t aclIpFragLookup = {
    {P4_IP_FRAG_ANY, SAI_ACL_IP_FRAG_ANY},
    {P4_IP_FRAG_NON_FRAG, SAI_ACL_IP_FRAG_NON_FRAG},
    {P4_IP_FRAG_NON_FRAG_OR_HEAD, SAI_ACL_IP_FRAG_NON_FRAG_OR_HEAD},
    {P4_IP_FRAG_HEAD, SAI_ACL_IP_FRAG_HEAD},
    {P4_IP_FRAG_NON_HEAD, SAI_ACL_IP_FRAG_NON_HEAD},
};

static const acl_packet_vlan_lookup_t aclPacketVlanLookup = {
    {P4_PACKET_VLAN_UNTAG, SAI_PACKET_VLAN_UNTAG},
    {P4_PACKET_VLAN_SINGLE_OUTER_TAG, SAI_PACKET_VLAN_SINGLE_OUTER_TAG},
    {P4_PACKET_VLAN_DOUBLE_TAG, SAI_PACKET_VLAN_DOUBLE_TAG},
};

static const udf_base_lookup_t udfBaseLookup = {
    {P4_UDF_BASE_L2, SAI_UDF_BASE_L2},
    {P4_UDF_BASE_L3, SAI_UDF_BASE_L3},
    {P4_UDF_BASE_L4, SAI_UDF_BASE_L4},
};

static std::map<sai_policer_attr_t, sai_policer_stat_t> aclCounterColoredPacketsStatsIdMap = {
    {SAI_POLICER_ATTR_GREEN_PACKET_ACTION, SAI_POLICER_STAT_GREEN_PACKETS},
    {SAI_POLICER_ATTR_YELLOW_PACKET_ACTION, SAI_POLICER_STAT_YELLOW_PACKETS},
    {SAI_POLICER_ATTR_RED_PACKET_ACTION, SAI_POLICER_STAT_RED_PACKETS},
};

static std::map<sai_policer_attr_t, sai_policer_stat_t> aclCounterColoredBytesStatsIdMap = {
    {SAI_POLICER_ATTR_GREEN_PACKET_ACTION, SAI_POLICER_STAT_GREEN_BYTES},
    {SAI_POLICER_ATTR_YELLOW_PACKET_ACTION, SAI_POLICER_STAT_YELLOW_BYTES},
    {SAI_POLICER_ATTR_RED_PACKET_ACTION, SAI_POLICER_STAT_RED_BYTES},
};

static std::map<sai_stat_id_t, std::string> aclCounterStatsIdNameMap = {
    {SAI_POLICER_STAT_GREEN_PACKETS, P4_COUNTER_STATS_GREEN_PACKETS},
    {SAI_POLICER_STAT_YELLOW_PACKETS, P4_COUNTER_STATS_YELLOW_PACKETS},
    {SAI_POLICER_STAT_RED_PACKETS, P4_COUNTER_STATS_RED_PACKETS},
    {SAI_POLICER_STAT_GREEN_BYTES, P4_COUNTER_STATS_GREEN_BYTES},
    {SAI_POLICER_STAT_YELLOW_BYTES, P4_COUNTER_STATS_YELLOW_BYTES},
    {SAI_POLICER_STAT_RED_BYTES, P4_COUNTER_STATS_RED_BYTES},
};

static const std::map<sai_acl_stage_t, acl_stage_type_t>
    aclSaiStageAttrToEnumLookup = {
        {SAI_ACL_STAGE_INGRESS, ACL_STAGE_INGRESS},
        {SAI_ACL_STAGE_EGRESS, ACL_STAGE_EGRESS},
        {SAI_ACL_STAGE_PRE_INGRESS, ACL_STAGE_PRE_INGRESS},
};

// Parse ACL table definition APP DB entry action field to P4ActionParamName
// action_list and P4PacketActionWithColor action_color_list
bool parseAclTableAppDbActionField(const std::string &aggr_actions_str, std::vector<P4ActionParamName> *action_list,
                                   std::vector<P4PacketActionWithColor> *action_color_list);

// Validate and set match field with kind:sai_field. Caller methods are
// responsible to verify the kind before calling this method
ReturnCode validateAndSetSaiMatchFieldJson(const nlohmann::json &match_json, const std::string &p4_match,
                                           const std::string &aggr_match_str,
                                           std::map<std::string, SaiMatchField> *sai_match_field_lookup,
                                           std::map<std::string, std::string> *ip_type_bit_type_lookup);

// Validate and set composite match field element with kind:sai_field.
// Composite SAI field only support IPv6-64bit now (IPV6_WORDn)
ReturnCode validateAndSetCompositeElementSaiFieldJson(
    const nlohmann::json &element_match_json, const std::string &p4_match,
    std::map<std::string, std::vector<SaiMatchField>> *composite_sai_match_fields_lookup,
    const std::string &format_str = EMPTY_STRING);

// Validate and set UDF match field with kind:udf. Caller methods are
// responsible for verifying the kind and format before calling this method
ReturnCode validateAndSetUdfFieldJson(const nlohmann::json &match_json, const std::string &p4_match,
                                      const std::string &aggr_match_str, const std::string &acl_table_name,
                                      std::map<std::string, std::vector<P4UdfField>> *udf_fields_lookup,
                                      std::map<std::string, uint16_t> *udf_group_attr_index_lookup);

// Only two cases are allowed in composite fields:
// 1. IPV6_64bit(IPV6_WORD3 and IPV6_WORD2 in elements, kind:sai_field,
// format:IPV6)
// 2. Generic UDF(UDF in elements, kind:udf, format:HEX_STRING)
ReturnCode validateAndSetCompositeMatchFieldJson(
    const nlohmann::json &aggr_match_json, const std::string &p4_match, const std::string &aggr_match_str,
    const std::string &acl_table_name,
    std::map<std::string, std::vector<SaiMatchField>> *composite_sai_match_fields_lookup,
    std::map<std::string, std::vector<P4UdfField>> *udf_fields_lookup,
    std::map<std::string, uint16_t> *udf_group_attr_index_lookup);

ReturnCode buildAclTableDefinitionMatchFieldValues(const std::map<std::string, std::string> &match_field_lookup,
                                                   P4AclTableDefinition *acl_table);

// Build SaiActionWithParam action map for ACL table definition
// by P4ActionParamName action map
ReturnCode buildAclTableDefinitionActionFieldValues(
    const std::map<std::string, std::vector<P4ActionParamName>>&
        action_field_lookup,
    std::map<std::string, std::vector<SaiActionWithParam>>*
        aggr_sai_actions_lookup,
    std::set<sai_acl_action_type_t>* acl_action_type_set);

bool isSetUserTrapActionInAclTableDefinition(
    const std::map<std::string, std::vector<SaiActionWithParam>> &aggr_sai_actions_lookup);

// Build packet color(sai_policer_attr_t) to packet
// action(sai_packet_action_t) map for ACL table definition by
// P4PacketActionWithColor action map. If packet color is empty, then the
// packet action should add as a SaiActionWithParam
ReturnCode buildAclTableDefinitionActionColorFieldValues(
    const std::map<std::string, std::vector<P4PacketActionWithColor>>&
        action_color_lookup,
    std::map<std::string, std::vector<SaiActionWithParam>>*
        aggr_sai_actions_lookup,
    std::map<std::string, std::map<sai_policer_attr_t, sai_packet_action_t>>*
        aggr_sai_action_color_lookup,
    std::set<sai_acl_action_type_t>* acl_action_type_set);

// Set IP_TYPE in match field
bool setMatchFieldIpType(const std::string &attr_value, sai_attribute_value_t *value,
                         const std::string &ip_type_bit_type);

// Set composite match field with sai_field type. Currently only ACL entry
// attributes listed in aclCompositeMatchTableAttrLookup are supported
ReturnCode setCompositeSaiMatchValue(const sai_acl_entry_attr_t attr_name,
                                     const std::string& attr_value,
                                     sai_attribute_value_t* value);

// Set composite match field with sai_field type.
ReturnCode setUdfMatchValue(const P4UdfField &udf_field, const std::string &attr_value, sai_attribute_value_t *value,
                            P4UdfDataMask *udf_data_mask, uint16_t bytes_offset);

// Compares the action value difference if the action field is present in
// both new and old ACL rules. Returns true if action values are different.
bool isDiffActionFieldValue(const sai_acl_entry_attr_t attr_name,
                            const sai_attribute_value_t& value,
                            const sai_attribute_value_t& old_value,
                            const P4AclRule& acl_rule,
                            const P4AclRule& old_acl_rule);

// Compares the match value difference if the match field is present in
// both new and old ACL rules. Returns true if match values are different.
// This method is used in state verification only.
bool isDiffMatchFieldValue(const sai_acl_entry_attr_t attr_name,
                           const sai_attribute_value_t& value,
                           const sai_attribute_value_t& old_value,
                           const P4AclRule& acl_rule,
                           const P4AclRule& old_acl_rule);

} // namespace p4orch
