#pragma once

#include <deque>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "ipaddress.h"
#include "ipprefix.h"
#include "macaddress.h"
#include "response_publisher_interface.h"
#include "return_code.h"
#include "table.h"
extern "C"
{
#include "saitypes.h"
}

namespace p4orch
{

// Field names in P4RT APP DB entry.
constexpr char *kTablePrefixEXT = "EXT_";
constexpr char *kRouterInterfaceId = "router_interface_id";
constexpr char* kMulticastGroupId = "multicast_group_id";
constexpr char *kPort = "port";
constexpr char *kInPort = "in_port";
constexpr char* kMulticastReplicaPort = "multicast_replica_port";
constexpr char* kMulticastReplicaInstance = "multicast_replica_instance";
constexpr char *kSrcMac = "src_mac";
constexpr char *kAction = "action";
constexpr char *kActions = "actions";
constexpr char *kWeight = "weight";
constexpr char *kWatchPort = "watch_port";
constexpr char *kNeighborId = "neighbor_id";
constexpr char *kDstMac = "dst_mac";
constexpr char *kNexthopId = "nexthop_id";
constexpr char *kTunnelId = "tunnel_id";
constexpr char *kVrfId = "vrf_id";
constexpr char *kIpv4Dst = "ipv4_dst";
constexpr char *kIpv6Dst = "ipv6_dst";
constexpr char *kWcmpGroupId = "wcmp_group_id";
constexpr char *kRouteMetadata = "route_metadata";
constexpr char* kMulticastMetadata = "multicast_metadata";
constexpr char *kSetNexthopId = "set_nexthop_id";
constexpr char *kSetWcmpGroupId = "set_wcmp_group_id";
constexpr char* kSetMulticastGroupId = "set_multicast_group_id";
constexpr char* kSetSrcMac = "set_src_mac";
constexpr char *kSetNexthopIdAndMetadata = "set_nexthop_id_and_metadata";
constexpr char *kSetWcmpGroupIdAndMetadata = "set_wcmp_group_id_and_metadata";
constexpr char *kSetMetadataAndDrop = "set_metadata_and_drop";
constexpr char *kSetNexthop = "set_nexthop";
constexpr char *kSetIpNexthop = "set_ip_nexthop";
constexpr char *kSetTunnelNexthop = "set_p2p_tunnel_encap_nexthop";
constexpr char *kDrop = "drop";
constexpr char *kTrap = "trap";
constexpr char *kStage = "stage";
constexpr char *kSize = "size";
constexpr char *kPriority = "priority";
constexpr char *kPacketColor = "packet_color";
constexpr char *kMeterUnit = "meter/unit";
constexpr char *kCounterUnit = "counter/unit";
constexpr char kFieldDelimiter = '/';
constexpr char kTableKeyDelimiter = ':';
constexpr char kDataMaskDelimiter = '&';
constexpr char kPortsDelimiter = ',';
constexpr char *kMatchPrefix = "match";
constexpr char *kActionParamPrefix = "param";
constexpr char *kMeterPrefix = "meter";
constexpr char *kMeterCir = "cir";
constexpr char *kMeterCburst = "cburst";
constexpr char *kMeterPir = "pir";
constexpr char *kMeterPburst = "pburst";
constexpr char *kControllerMetadata = "controller_metadata";
constexpr char *kAclMatchFieldKind = "kind";
constexpr char *kAclMatchFieldFormat = "format";
constexpr char *kAclMatchFieldBitwidth = "bitwidth";
constexpr char *kAclMatchFieldElements = "elements";
constexpr char *kAclMatchFieldSaiField = "sai_field";
constexpr char *kAclMatchFieldKindComposite = "composite";
constexpr char *kAclMatchFieldKindUdf = "udf";
constexpr char *kAclUdfBase = "base";
constexpr char *kAclUdfOffset = "offset";
constexpr char *kMirrorSessionId = "mirror_session_id";
constexpr char *kSrcIp = "src_ip";
constexpr char *kDstIp = "dst_ip";
constexpr char *kEncapSrcIp = "encap_src_ip";
constexpr char *kEncapDstIp = "encap_dst_ip";
constexpr char *kTtl = "ttl";
constexpr char *kTos = "tos";
constexpr char *kMirrorAsIpv4Erspan = "mirror_as_ipv4_erspan";
constexpr char *kL3AdmitAction = "admit_to_l3";
constexpr char *kTunnelAction = "mark_for_p2p_tunnel_encap";

// Field names in P4RT TABLE DEFINITION APP DB entry.
constexpr char *kTables = "tables";
constexpr char *kId = "id";
constexpr char *kName = "name";
constexpr char *kAlias = "alias";
constexpr char *kBitwidth = "bitwidth";
constexpr char *kFormat = "format";
constexpr char *kmatchFields = "matchFields";
constexpr char *kActionParams = "params";
constexpr char *kReferences = "references";
constexpr char *kTableRef = "table";
constexpr char *kMatchRef = "match";
} // namespace p4orch

// Prepends "match/" to the input string str to construct a new string.
std::string prependMatchField(const std::string &str);

// Prepends "param/" to the input string str to construct a new string.
std::string prependParamField(const std::string &str);

struct ActionParamInfo
{
    std::string name;
    std::string fieldtype;
    std::string datatype;
    std::unordered_map<std::string, std::string> table_reference_map;
};

struct ActionInfo
{
    std::string name;
    std::unordered_map<std::string, ActionParamInfo> params;
    bool refers_to;
};

struct TableMatchInfo
{
    std::string name;
    std::string fieldtype;
    std::string datatype;
    std::unordered_map<std::string, std::string> table_reference_map;
};

/**
 * Dervied table definition
 * This is a derived state out of table definition provided by P4RT-APP
 */
struct TableInfo
{
    std::string name;
    int id;
    int precedence;
    std::unordered_map<std::string, TableMatchInfo> match_fields;
    std::unordered_map<std::string, ActionInfo> action_fields;
    bool counter_bytes_enabled;
    bool counter_packets_enabled;
    std::vector<std::string> action_ref_tables;
    // list of tables across all actions, of current table, refer to
};

/**
 * table-name to table-definition map
 */
typedef std::unordered_map<std::string, TableInfo> TableInfoMap;

struct TablesInfoAppDbEntry
{
    std::string context;
    std::string info;
};

struct P4RouterInterfaceAppDbEntry
{
    std::string router_interface_id;
    std::string port_name;
    swss::MacAddress src_mac_address;
    bool is_set_port_name = false;
    bool is_set_src_mac = false;
};

struct P4NeighborAppDbEntry
{
    std::string router_intf_id;
    swss::IpAddress neighbor_id;
    swss::MacAddress dst_mac_address;
    bool is_set_dst_mac = false;
};

struct P4GreTunnelAppDbEntry
{
    // Match
    std::string tunnel_id;
    // Action
    std::string router_interface_id;
    swss::IpAddress encap_src_ip;
    swss::IpAddress encap_dst_ip;
    std::string action_str;
};

// P4NextHopAppDbEntry holds entry deserialized from table
// APP_P4RT_NEXTHOP_TABLE_NAME.
struct P4NextHopAppDbEntry
{
    // Key
    std::string next_hop_id;
    // Fields
    std::string router_interface_id;
    std::string gre_tunnel_id;
    swss::IpAddress neighbor_id;
    std::string action_str;
};

// P4L3AdmitAppDbEntry holds entry deserialized from table
// APP_P4RT_L3_ADMIT_TABLE_NAME.
struct P4L3AdmitAppDbEntry
{
    // Key (match parameters)
    std::string port_name; // Optional
    swss::MacAddress mac_address_data;
    swss::MacAddress mac_address_mask;
    uint32_t priority;
};

struct P4MirrorSessionAppDbEntry
{
    // Key (match field)
    std::string mirror_session_id;

    // fields (action parameters)
    std::string port;
    bool has_port = false;

    swss::IpAddress src_ip;
    bool has_src_ip = false;

    swss::IpAddress dst_ip;
    bool has_dst_ip = false;

    swss::MacAddress src_mac;
    bool has_src_mac = false;

    swss::MacAddress dst_mac;
    bool has_dst_mac = false;

    uint8_t ttl = 0;
    bool has_ttl = false;

    uint8_t tos = 0;
    bool has_tos = false;
};

struct P4ActionParamName
{
    std::string sai_action;
    std::string p4_param_name;
};

struct P4PacketActionWithColor
{
    std::string packet_action;
    std::string packet_color;
};

struct P4AclTableDefinitionAppDbEntry
{
    // Key
    std::string acl_table_name;
    // Fields
    std::string stage;
    uint32_t size;
    uint32_t priority;
    std::map<std::string, std::string> match_field_lookup;
    std::map<std::string, std::vector<P4ActionParamName>> action_field_lookup;
    std::map<std::string, std::vector<P4PacketActionWithColor>> packet_action_color_lookup;
    std::string meter_unit;
    std::string counter_unit;
};

struct P4AclMeterAppDb
{
    bool enabled;
    uint64_t cir;
    uint64_t cburst;
    uint64_t pir;
    uint64_t pburst;

    P4AclMeterAppDb() : enabled(false)
    {
    }
};

struct P4AclRuleAppDbEntry
{
    // Key
    std::string acl_table_name;
    std::map<std::string, std::string> match_fvs;
    uint32_t priority;
    std::string db_key;
    // Fields
    std::string action;
    std::map<std::string, std::string> action_param_fvs;
    P4AclMeterAppDb meter;
};

struct DepObject
{
    sai_object_type_t sai_object;
    std::string key;
    sai_object_id_t oid;
};

struct P4ExtTableAppDbEntry
{
    std::string db_key;
    std::string table_name;
    std::string table_key;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> action_params;
    std::unordered_map<std::string, DepObject> action_dep_objects;
};

TableInfo *getTableInfo(const std::string &table_name);
ActionInfo *getTableActionInfo(TableInfo *table, const std::string &action_name);

// Get the table name and key content from the given P4RT key.
// Outputs will be empty strings in case of error.
// Example: FIXED_NEIGHBOR_TABLE:{content}
// Table name: FIXED_NEIGHBOR_TABLE
// Key content: {content}
void parseP4RTKey(const std::string &key, std::string *table_name, std::string *key_content);

// State verification function that verifies the table attributes.
// Returns a non-empty string if verification fails.
//
// targets: the table attributes that we need to verify.
// exp: the attributes that must be included and have correct value.
// opt: the attributes that can be excluded, but must have correct value if
//      included.
// allow_unknown: if set to false, verification will fail if there is an
//                attribute that is not in exp or opt.
std::string verifyAttrs(const std::vector<swss::FieldValueTuple> &targets,
                        const std::vector<swss::FieldValueTuple> &exp, const std::vector<swss::FieldValueTuple> &opt,
                        bool allow_unknown);

// Helper function to drain all entries in the manager without execution.
void drainMgmtWithNotExecuted(std::deque<swss::KeyOpFieldsValuesTuple>& entries,
                              ResponsePublisherInterface* publisher);

// class KeyGenerator includes member functions to generate keys for entries
// stored in P4 Orch managers.
class KeyGenerator
{
  public:
    static std::string generateTablesInfoKey(const std::string &context);

    static std::string generateRouteKey(const std::string &vrf_id, const swss::IpPrefix &ip_prefix);

    static std::string generateRouterInterfaceKey(const std::string &router_intf_id);

    static std::string generateNeighborKey(const std::string &router_intf_id, const swss::IpAddress &neighbor_id);

    static std::string generateNextHopKey(const std::string &next_hop_id);

    static std::string generateMirrorSessionKey(const std::string &mirror_session_id);

    static std::string generateMulticastRouterInterfaceKey(
        const std::string& multicast_replica_port,
        const std::string& multicast_replica_instance);

    static std::string generateMulticastReplicationKey(
        const std::string& multicast_group_id,
        const std::string& multicast_replica_port,
        const std::string& multicast_replica_instance);

    static std::string generateMulticastRouterInterfaceRifKey(
        const std::string& multicast_replica_port,
        const swss::MacAddress& src_mac);

    static std::string generateWcmpGroupKey(const std::string &wcmp_group_id);

    static std::string generateAclRuleKey(const std::map<std::string, std::string> &match_fields,
                                          const std::string &priority);

    static std::string generateL3AdmitKey(const swss::MacAddress &mac_address_data,
                                          const swss::MacAddress &mac_address_mask, const std::string &port_name,
                                          const uint32_t &priority);

    static std::string generateTunnelKey(const std::string &tunnel_id);

    static std::string generateExtTableKey(const std::string &table_name, const std::string &table_key);

    // Generates key used by object managers and centralized mapper.
    // Takes map of <id, value> as input and returns a concatenated string
    // of the form id1=value1:id2=value2...
    static std::string generateKey(const std::map<std::string, std::string> &fv_map);
};

// Inserts single quote for a variable name.
// Returns a string.
template <typename T> std::string QuotedVar(T name)
{
    std::ostringstream ss;
    ss << std::quoted(name, '\'');
    return ss.str();
}

// Trim tailing and leading whitespace
std::string trim(const std::string &s);
