#include "p4orch/p4orch_util.h"

#include "p4orch/p4orch.h"
#include "schema.h"

using ::p4orch::kTableKeyDelimiter;
extern P4Orch *gP4Orch;

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

std::string verifyAttrs(const std::vector<swss::FieldValueTuple> &targets,
                        const std::vector<swss::FieldValueTuple> &exp, const std::vector<swss::FieldValueTuple> &opt,
                        bool allow_unknown)
{
    std::map<std::string, std::string> exp_map;
    for (const auto &fv : exp)
    {
        exp_map[fvField(fv)] = fvValue(fv);
    }
    std::map<std::string, std::string> opt_map;
    for (const auto &fv : opt)
    {
        opt_map[fvField(fv)] = fvValue(fv);
    }

    std::set<std::string> fields;
    for (const auto &fv : targets)
    {
        fields.insert(fvField(fv));
        bool found = false;
        if (exp_map.count(fvField(fv)))
        {
            found = true;
            if (fvValue(fv) != exp_map.at(fvField(fv)))
            {
                return fvField(fv) + " value mismatch, exp " + exp_map.at(fvField(fv)) + " got " + fvValue(fv);
            }
        }
        if (opt_map.count(fvField(fv)))
        {
            found = true;
            if (fvValue(fv) != opt_map.at(fvField(fv)))
            {
                return fvField(fv) + " value mismatch, exp " + opt_map.at(fvField(fv)) + " got " + fvValue(fv);
            }
        }
        if (!found && !allow_unknown)
        {
            return std::string("Unexpected field ") + fvField(fv);
        }
    }
    for (const auto &it : exp_map)
    {
        if (!fields.count(it.first))
        {
            return std::string("Missing field ") + it.first;
        }
    }
    return "";
}

TableInfo *getTableInfo(const std::string &table_name)
{
    if (!gP4Orch->tablesinfo)
    {
        return nullptr;
    }

    auto it = gP4Orch->tablesinfo->m_tableInfoMap.find(table_name);
    if (it == gP4Orch->tablesinfo->m_tableInfoMap.end())
    {
        return nullptr;
    }

    return &it->second;
}

ActionInfo *getTableActionInfo(TableInfo *table, const std::string &action_name)
{
    if (!table)
    {
        return nullptr;
    }

    auto it = table->action_fields.find(action_name);
    if (it == table->action_fields.end())
    {
        return nullptr;
    }

    return &it->second;
}

std::string KeyGenerator::generateTablesInfoKey(const std::string &context)
{
    std::map<std::string, std::string> fv_map = {{"context", context}};
    return generateKey(fv_map);
}

void drainMgmtWithNotExecuted(std::deque<swss::KeyOpFieldsValuesTuple>& entries,
                              ResponsePublisherInterface* publisher) {
  for (const auto& key_op_fvs_tuple : entries) {
    publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                       kfvFieldsValues(key_op_fvs_tuple),
                       ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED),
                       /*replace=*/true);
  }
  entries.clear();
  return;
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

std::string KeyGenerator::generateMulticastRouterInterfaceKey(
    const std::string& multicast_replica_port,
    const std::string& multicast_replica_instance) {
  std::map<std::string, std::string> fv_map = {};

  fv_map.emplace(std::string(p4orch::kMatchPrefix) + p4orch::kFieldDelimiter +
                     p4orch::kMulticastReplicaPort,
                 multicast_replica_port);
  fv_map.emplace(std::string(p4orch::kMatchPrefix) + p4orch::kFieldDelimiter +
                     p4orch::kMulticastReplicaInstance,
                 multicast_replica_instance);
  return generateKey(fv_map);
}

std::string KeyGenerator::generateMulticastReplicationKey(
    const std::string& multicast_group_id,
    const std::string& multicast_replica_port,
    const std::string& multicast_replica_instance) {
  std::map<std::string, std::string> fv_map = {};

  fv_map.emplace(std::string(p4orch::kMatchPrefix) + p4orch::kFieldDelimiter +
                     p4orch::kMulticastGroupId,
                 multicast_group_id);
  fv_map.emplace(std::string(p4orch::kMatchPrefix) + p4orch::kFieldDelimiter +
                     p4orch::kMulticastReplicaPort,
                 multicast_replica_port);
  fv_map.emplace(std::string(p4orch::kMatchPrefix) + p4orch::kFieldDelimiter +
                     p4orch::kMulticastReplicaInstance,
                 multicast_replica_instance);
  return generateKey(fv_map);
}

std::string KeyGenerator::generateMulticastRouterInterfaceRifKey(
    const std::string& multicast_replica_port,
    const swss::MacAddress& src_mac) {
  std::map<std::string, std::string> fv_map = {};

  fv_map.emplace(std::string(p4orch::kMatchPrefix) + p4orch::kFieldDelimiter +
                     p4orch::kMulticastReplicaPort,
                 multicast_replica_port);
  fv_map.emplace(std::string(p4orch::kActionParamPrefix) +
                     p4orch::kFieldDelimiter + p4orch::kSrcMac,
                 src_mac.to_string());
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

std::string KeyGenerator::generateL3AdmitKey(const swss::MacAddress &mac_address_data,
                                             const swss::MacAddress &mac_address_mask, const std::string &port_name,
                                             const uint32_t &priority)
{
    std::map<std::string, std::string> fv_map = {};
    fv_map.emplace(std::string(p4orch::kMatchPrefix) + p4orch::kFieldDelimiter + p4orch::kDstMac,
                   mac_address_data.to_string() + p4orch::kDataMaskDelimiter + mac_address_mask.to_string());
    if (!port_name.empty())
    {
        fv_map.emplace(std::string(p4orch::kMatchPrefix) + p4orch::kFieldDelimiter + p4orch::kInPort, port_name);
    }
    fv_map.emplace(p4orch::kPriority, std::to_string(priority));
    return generateKey(fv_map);
}

std::string KeyGenerator::generateTunnelKey(const std::string &tunnel_id)
{
    std::map<std::string, std::string> fv_map = {{p4orch::kTunnelId, tunnel_id}};
    return generateKey(fv_map);
}

std::string KeyGenerator::generateExtTableKey(const std::string &table_name, const std::string &table_key)
{
    std::string key;

    key.append(table_name);
    key.append(":");
    key.append(table_key);

    return key;
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

std::string trim(const std::string &s)
{
    size_t end = s.find_last_not_of(" ");
    size_t start = s.find_first_not_of(" ");
    return (end == std::string::npos) ? "" : s.substr(start, end - start + 1);
}
