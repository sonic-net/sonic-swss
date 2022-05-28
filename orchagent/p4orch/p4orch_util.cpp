#include "p4orch/p4orch_util.h"

#include "schema.h"

using ::p4orch::kTableKeyDelimiter;

// Prepends "match/" to the input string str to construct a new string.
std::string prependMatchField(const std::string &str)
{
    return std::string(p4orch::kMatchPrefix) + p4orch::kFieldDelimiter + str;
}

// Prepends "param/" to the input string str to construct a new string.
std::string prependParamField(const std::string &str)
{
    return std::string(p4orch::kActionParamPrefix) + p4orch::kFieldDelimiter + str;
}

void parseP4RTKey(const std::string &key, std::string *table_name, std::string *key_content)
{
    auto pos = key.find_first_of(kTableKeyDelimiter);
    if (pos == std::string::npos)
    {
        *table_name = "";
        *key_content = "";
        return;
    }
    *table_name = key.substr(0, pos);
    *key_content = key.substr(pos + 1);
}

std::string KeyGenerator::generateRouteKey(const std::string &vrf_id, const swss::IpPrefix &ip_prefix)
{
    std::map<std::string, std::string> fv_map = {
        {p4orch::kVrfId, vrf_id}, {ip_prefix.isV4() ? p4orch::kIpv4Dst : p4orch::kIpv6Dst, ip_prefix.to_string()}};
    return generateKey(fv_map);
}

std::string KeyGenerator::generateRouterInterfaceKey(const std::string &router_intf_id)
{
    std::map<std::string, std::string> fv_map = {{p4orch::kRouterInterfaceId, router_intf_id}};
    return generateKey(fv_map);
}

std::string KeyGenerator::generateNeighborKey(const std::string &router_intf_id, const swss::IpAddress &neighbor_id)
{
    std::map<std::string, std::string> fv_map = {{p4orch::kRouterInterfaceId, router_intf_id},
                                                 {p4orch::kNeighborId, neighbor_id.to_string()}};
    return generateKey(fv_map);
}

std::string KeyGenerator::generateNextHopKey(const std::string &next_hop_id)
{
    std::map<std::string, std::string> fv_map = {{p4orch::kNexthopId, next_hop_id}};
    return generateKey(fv_map);
}

std::string KeyGenerator::generateMirrorSessionKey(const std::string &mirror_session_id)
{
    std::map<std::string, std::string> fv_map = {{p4orch::kMirrorSessionId, mirror_session_id}};
    return generateKey(fv_map);
}

std::string KeyGenerator::generateWcmpGroupKey(const std::string &wcmp_group_id)
{
    std::map<std::string, std::string> fv_map = {{p4orch::kWcmpGroupId, wcmp_group_id}};
    return generateKey(fv_map);
}

std::string KeyGenerator::generateAclRuleKey(const std::map<std::string, std::string> &match_fields,
                                             const std::string &priority)
{
    std::map<std::string, std::string> fv_map = {};
    for (const auto &match_field : match_fields)
    {
        fv_map.emplace(std::string(p4orch::kMatchPrefix) + p4orch::kFieldDelimiter + match_field.first,
                       match_field.second);
    }
    fv_map.emplace(p4orch::kPriority, priority);
    return generateKey(fv_map);
}

std::string KeyGenerator::generateKey(const std::map<std::string, std::string> &fv_map)
{
    std::string key;
    bool append_delimiter = false;
    for (const auto &it : fv_map)
    {
        if (append_delimiter)
        {
            key.append(":");
        }
        else
        {
            append_delimiter = true;
        }
        key.append(it.first);
        key.append("=");
        key.append(it.second);
    }

    return key;
}
