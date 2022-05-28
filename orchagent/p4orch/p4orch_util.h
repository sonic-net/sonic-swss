#pragma once

#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "ipaddress.h"
#include "ipprefix.h"
#include "macaddress.h"

namespace p4orch
{

// Field names in P4RT APP DB entry.
constexpr char *kRouterInterfaceId = "router_interface_id";
constexpr char *kPort = "port";
constexpr char *kSrcMac = "src_mac";
constexpr char *kAction = "action";
constexpr char *kActions = "actions";
constexpr char *kWeight = "weight";
constexpr char *kWatchPort = "watch_port";
constexpr char *kNeighborId = "neighbor_id";
constexpr char *kDstMac = "dst_mac";
constexpr char *kNexthopId = "nexthop_id";
constexpr char *kVrfId = "vrf_id";
constexpr char *kIpv4Dst = "ipv4_dst";
constexpr char *kIpv6Dst = "ipv6_dst";
constexpr char *kWcmpGroupId = "wcmp_group_id";
constexpr char *kSetNexthopId = "set_nexthop_id";
constexpr char *kSetWcmpGroupId = "set_wcmp_group_id";
constexpr char *kDrop = "drop";
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
constexpr char *kTtl = "ttl";
constexpr char *kTos = "tos";
constexpr char *kMirrorAsIpv4Erspan = "mirror_as_ipv4_erspan";
} // namespace p4orch

// Prepends "match/" to the input string str to construct a new string.
std::string prependMatchField(const std::string &str);

// Prepends "param/" to the input string str to construct a new string.
std::string prependParamField(const std::string &str);

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

// P4NextHopAppDbEntry holds entry deserialized from table
// APP_P4RT_NEXTHOP_TABLE_NAME.
struct P4NextHopAppDbEntry
{
    // Key
    std::string next_hop_id;
    // Fields
    std::string router_interface_id;
    swss::IpAddress neighbor_id;
    bool is_set_router_interface_id = false;
    bool is_set_neighbor_id = false;
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

// Get the table name and key content from the given P4RT key.
// Outputs will be empty strings in case of error.
// Example: FIXED_NEIGHBOR_TABLE:{content}
// Table name: FIXED_NEIGHBOR_TABLE
// Key content: {content}
void parseP4RTKey(const std::string &key, std::string *table_name, std::string *key_content);

// class KeyGenerator includes member functions to generate keys for entries
// stored in P4 Orch managers.
class KeyGenerator
{
  public:
    static std::string generateRouteKey(const std::string &vrf_id, const swss::IpPrefix &ip_prefix);

    static std::string generateRouterInterfaceKey(const std::string &router_intf_id);

    static std::string generateNeighborKey(const std::string &router_intf_id, const swss::IpAddress &neighbor_id);

    static std::string generateNextHopKey(const std::string &next_hop_id);

    static std::string generateMirrorSessionKey(const std::string &mirror_session_id);

    static std::string generateWcmpGroupKey(const std::string &wcmp_group_id);

    static std::string generateAclRuleKey(const std::map<std::string, std::string> &match_fields,
                                          const std::string &priority);

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
