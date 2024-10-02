#pragma once

// defines ------------------------------------------------------------------------------------------------------------

#define PBH_TABLE_INTERFACE_LIST "interface_list"
#define PBH_TABLE_DESCRIPTION    "description"

#define PBH_RULE_PACKET_ACTION_SET_ECMP_HASH "SET_ECMP_HASH"
#define PBH_RULE_PACKET_ACTION_SET_LAG_HASH  "SET_LAG_HASH"

#define PBH_RULE_FLOW_COUNTER_ENABLED  "ENABLED"
#define PBH_RULE_FLOW_COUNTER_DISABLED "DISABLED"

#define PBH_RULE_PRIORITY         "priority"
#define PBH_RULE_GRE_KEY          "gre_key"
#define PBH_RULE_ETHER_TYPE       "ether_type"
#define PBH_RULE_IP_PROTOCOL      "ip_protocol"
#define PBH_RULE_IPV6_NEXT_HEADER "ipv6_next_header"
#define PBH_RULE_L4_DST_PORT      "l4_dst_port"
#define PBH_RULE_INNER_ETHER_TYPE "inner_ether_type"
#define PBH_RULE_HASH             "hash"
#define PBH_RULE_PACKET_ACTION    "packet_action"
#define PBH_RULE_FLOW_COUNTER     "flow_counter"

#define PBH_HASH_HASH_FIELD_LIST "hash_field_list"

#define PBH_HASH_FIELD_HASH_FIELD_INNER_IP_PROTOCOL "INNER_IP_PROTOCOL"
#define PBH_HASH_FIELD_HASH_FIELD_INNER_L4_DST_PORT "INNER_L4_DST_PORT"
#define PBH_HASH_FIELD_HASH_FIELD_INNER_L4_SRC_PORT "INNER_L4_SRC_PORT"
#define PBH_HASH_FIELD_HASH_FIELD_INNER_DST_IPV4    "INNER_DST_IPV4"
#define PBH_HASH_FIELD_HASH_FIELD_INNER_SRC_IPV4    "INNER_SRC_IPV4"
#define PBH_HASH_FIELD_HASH_FIELD_INNER_DST_IPV6    "INNER_DST_IPV6"
#define PBH_HASH_FIELD_HASH_FIELD_INNER_SRC_IPV6    "INNER_SRC_IPV6"

#define PBH_HASH_FIELD_HASH_FIELD  "hash_field"
#define PBH_HASH_FIELD_IP_MASK     "ip_mask"
#define PBH_HASH_FIELD_SEQUENCE_ID "sequence_id"
