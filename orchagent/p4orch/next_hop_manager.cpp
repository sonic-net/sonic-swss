#include "p4orch/next_hop_manager.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "SaiAttributeList.h"
#include "crmorch.h"
#include "dbconnector.h"
#include "ipaddress.h"
#include "logger.h"
#include "p4orch/p4orch.h"
#include "p4orch/p4orch_util.h"
#include "sai_serialize.h"
#include "swssnet.h"
#include "table.h"
extern "C"
{
#include "sai.h"
}

using ::p4orch::kTableKeyDelimiter;

extern sai_object_id_t gSwitchId;
extern sai_next_hop_api_t *sai_next_hop_api;
extern CrmOrch *gCrmOrch;
extern P4Orch *gP4Orch;

P4NextHopEntry::P4NextHopEntry(
    const std::string& next_hop_id, const std::string& router_interface_id,
    const std::string& gre_tunnel_id, const swss::IpAddress& neighbor_id,
    bool disable_decrement_ttl, bool disable_src_mac_rewrite,
    bool disable_dst_mac_rewrite, bool disable_vlan_rewrite)
    : next_hop_id(next_hop_id),
      router_interface_id(router_interface_id),
      gre_tunnel_id(gre_tunnel_id),
      neighbor_id(neighbor_id),
      disable_decrement_ttl(disable_decrement_ttl),
      disable_src_mac_rewrite(disable_src_mac_rewrite),
      disable_dst_mac_rewrite(disable_dst_mac_rewrite),
      disable_vlan_rewrite(disable_vlan_rewrite) {
  SWSS_LOG_ENTER();
  next_hop_key = KeyGenerator::generateNextHopKey(next_hop_id);
}

ReturnCode NextHopManager::validateAppDbEntry(
    const P4NextHopAppDbEntry& app_db_entry) {
  if (app_db_entry.action_str != p4orch::kSetIpNexthop &&
      app_db_entry.action_str != p4orch::kSetIpNexthopAndDisableRewrites &&
      app_db_entry.action_str != p4orch::kSetNexthop &&
      app_db_entry.action_str != p4orch::kSetTunnelNexthop) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Invalid action " << QuotedVar(app_db_entry.action_str)
           << " of Nexthop App DB entry";
  }
  if ((app_db_entry.action_str == p4orch::kSetIpNexthop ||
       app_db_entry.action_str == p4orch::kSetIpNexthopAndDisableRewrites) &&
      app_db_entry.neighbor_id.isZero()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Missing field "
           << QuotedVar(prependParamField(p4orch::kNeighborId))
           << " for action " << QuotedVar(app_db_entry.action_str)
           << " in table entry";
  }
  if (app_db_entry.action_str == p4orch::kSetIpNexthop ||
      app_db_entry.action_str == p4orch::kSetIpNexthopAndDisableRewrites ||
      app_db_entry.action_str == p4orch::kSetNexthop) {
    if (!app_db_entry.gre_tunnel_id.empty()) {
      return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
             << "Unexpected field "
             << QuotedVar(prependParamField(p4orch::kTunnelId))
             << " for action " << QuotedVar(app_db_entry.action_str)
             << " in table entry";
    }
    if (app_db_entry.router_interface_id.empty()) {
      return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
             << "Missing field "
             << QuotedVar(prependParamField(p4orch::kRouterInterfaceId))
             << " for action " << QuotedVar(app_db_entry.action_str)
             << " in table entry";
    }
  }

  if (app_db_entry.action_str == p4orch::kSetTunnelNexthop) {
    if (!app_db_entry.router_interface_id.empty()) {
      return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
             << "Unexpected field "
             << QuotedVar(prependParamField(p4orch::kRouterInterfaceId))
             << " for action " << QuotedVar(p4orch::kSetTunnelNexthop)
             << " in table entry";
    }
    if (app_db_entry.gre_tunnel_id.empty()) {
      return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
             << "Missing field "
             << QuotedVar(prependParamField(p4orch::kTunnelId))
             << " for action " << QuotedVar(p4orch::kSetTunnelNexthop)
             << " in table entry";
    }
  }
  return ReturnCode();
}

ReturnCode NextHopManager::validateAppDbEntry(
    const P4NextHopAppDbEntry& app_db_entry, const std::string& operation) {
  SWSS_LOG_ENTER();

  P4NextHopEntry next_hop_entry(
      app_db_entry.next_hop_id, app_db_entry.router_interface_id,
      app_db_entry.gre_tunnel_id, app_db_entry.neighbor_id);

  if (operation == SET_COMMAND) {
    RETURN_IF_ERROR(validateAppDbEntry(app_db_entry));
    if (getNextHopEntry(next_hop_entry.next_hop_key) == nullptr) {
      if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_NEXT_HOP,
                                   next_hop_entry.next_hop_key)) {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
            "Next hop with key " << QuotedVar(next_hop_entry.next_hop_key)
                                 << " already exists in centralized mapper");
      }

      if (!next_hop_entry.gre_tunnel_id.empty()) {
        auto gre_tunnel_or =
            gP4Orch->getGreTunnelManager()->getConstGreTunnelEntry(
                KeyGenerator::generateTunnelKey(next_hop_entry.gre_tunnel_id));
        if (!gre_tunnel_or.ok()) {
          LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                               << "GRE Tunnel "
                               << QuotedVar(next_hop_entry.gre_tunnel_id)
                               << " does not exist in GRE Tunnel Manager");
        }

        if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_TUNNEL,
                                      KeyGenerator::generateTunnelKey(
                                          next_hop_entry.gre_tunnel_id))) {
          LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                               << "GRE Tunnel "
                               << QuotedVar(next_hop_entry.gre_tunnel_id)
                               << " does not exist in mapper");
        }

        next_hop_entry.router_interface_id =
            (*gre_tunnel_or).router_interface_id;
        // BRCM requires neighbor object to be created before GRE tunnel,
        // referring to the one in GRE tunnel object when creating
        // next_hop_entry_with setTunnelAction
        next_hop_entry.neighbor_id = (*gre_tunnel_or).neighbor_id;

      } else {
        if (!m_p4OidMapper->existsOID(
                SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                KeyGenerator::generateRouterInterfaceKey(
                    next_hop_entry.router_interface_id))) {
          LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                               << "Router intf "
                               << QuotedVar(next_hop_entry.router_interface_id)
                               << " does not exist in mapper");
        }
      }

      // Neighbor doesn't have OID and the IP addr needed in next hop creation
      // is neighbor_id, so only check neighbor existence in centralized mapper.
      const auto neighbor_key = KeyGenerator::generateNeighborKey(
          next_hop_entry.router_interface_id, next_hop_entry.neighbor_id);
      if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
                                    neighbor_key)) {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "Neighbor with key " << QuotedVar(neighbor_key)
                             << " does not exist in centralized mapper");
      }
    }
  } else if (operation == DEL_COMMAND) {
    if (getNextHopEntry(next_hop_entry.next_hop_key) == nullptr) {
      LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                           << "Next hop with key "
                           << QuotedVar(next_hop_entry.next_hop_key)
                           << " does not exist in next hop manager");
    }
    // Check if there is anything referring to the next hop before deletion.
    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                    next_hop_entry.next_hop_key, &ref_count)) {
      RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
          "Failed to get reference count for next hop "
          << QuotedVar(next_hop_entry.next_hop_key));
    }
    if (ref_count > 0) {
      LOG_ERROR_AND_RETURN(
          ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
          << "Next hop " << QuotedVar(next_hop_entry.next_hop_key)
          << " referenced by other objects (ref_count = " << ref_count);
    }
  } else {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                         << "Invalid OP " << operation);
  }

  return ReturnCode();
}

ReturnCodeOr<bool> parseFlag(std::string name, std::string value) {
  try {
    int flag = std::stoi(value);
    if (flag == 1)
      return true;
    else if (flag == 0)
      return false;
  } catch (std::exception& e) {
    // Nothing
  }
  return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
         << "Invalid " << QuotedVar(name) << " value: " << QuotedVar(value);
}

std::vector<sai_attribute_t> NextHopManager::getSaiAttrs(
    const P4NextHopEntry& next_hop_entry) {
  std::vector<sai_attribute_t> next_hop_attrs;
  sai_attribute_t next_hop_attr;

  if (!next_hop_entry.gre_tunnel_id.empty()) {
    // From centralized mapper and, get gre tunnel that next hop depends on. Get
    // underlay router interface from gre tunnel manager,
    sai_object_id_t tunnel_oid;
    m_p4OidMapper->getOID(
        SAI_OBJECT_TYPE_TUNNEL,
        KeyGenerator::generateTunnelKey(next_hop_entry.gre_tunnel_id),
        &tunnel_oid);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
    next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP;
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_TUNNEL_ID;
    next_hop_attr.value.oid = tunnel_oid;
    next_hop_attrs.push_back(next_hop_attr);
  } else {
    // From centralized mapper, get OID of router interface that next hop
    // depends on.
    sai_object_id_t rif_oid;
    m_p4OidMapper->getOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                          KeyGenerator::generateRouterInterfaceKey(
                              next_hop_entry.router_interface_id),
                          &rif_oid);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
    next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_IP;
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
    next_hop_attr.value.oid = rif_oid;
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_DISABLE_DECREMENT_TTL;
    next_hop_attr.value.booldata = next_hop_entry.disable_decrement_ttl;
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_DISABLE_SRC_MAC_REWRITE;
    next_hop_attr.value.booldata = next_hop_entry.disable_src_mac_rewrite;
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_DISABLE_DST_MAC_REWRITE;
    next_hop_attr.value.booldata = next_hop_entry.disable_dst_mac_rewrite;
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_DISABLE_VLAN_REWRITE;
    next_hop_attr.value.booldata = next_hop_entry.disable_vlan_rewrite;
    next_hop_attrs.push_back(next_hop_attr);
  }

  next_hop_attr.id = SAI_NEXT_HOP_ATTR_IP;
  swss::copy(next_hop_attr.value.ipaddr, next_hop_entry.neighbor_id);
  next_hop_attrs.push_back(next_hop_attr);

  return next_hop_attrs;
}

ReturnCode NextHopManager::getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                                        std::string &object_key)
{
    std::string value;

    try
    {
        nlohmann::json j = nlohmann::json::parse(json_key);
        if (j.find(prependMatchField(p4orch::kNexthopId)) != j.end())
        {
            value = j.at(prependMatchField(p4orch::kNexthopId)).get<std::string>();
            object_key = KeyGenerator::generateNextHopKey(value);
            object_type = SAI_OBJECT_TYPE_NEXT_HOP;
            return ReturnCode();
        }
        else
        {
            SWSS_LOG_ERROR("%s match parameter absent: required for dependent object query", p4orch::kNexthopId);
        }
    }
    catch (std::exception &ex)
    {
        SWSS_LOG_ERROR("json_key parse error");
    }

    return StatusCode::SWSS_RC_INVALID_PARAM;
}

void NextHopManager::enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry)
{
    m_entries.push_back(entry);
}

void NextHopManager::drainWithNotExecuted() {
  drainMgmtWithNotExecuted(m_entries, m_publisher);
}

ReturnCode NextHopManager::drain() {
  SWSS_LOG_ENTER();

  std::vector<P4NextHopAppDbEntry> entry_list;
  std::vector<swss::KeyOpFieldsValuesTuple> tuple_list;

  ReturnCode status;
  std::string prev_op;
  bool prev_update = false;
  while (!m_entries.empty()) {
    auto key_op_fvs_tuple = m_entries.front();
    m_entries.pop_front();
    std::string table_name;
    std::string key;
    parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &key);
    const std::vector<swss::FieldValueTuple>& attributes =
        kfvFieldsValues(key_op_fvs_tuple);

    auto app_db_entry_or = deserializeP4NextHopAppDbEntry(key, attributes);
    if (!app_db_entry_or.ok()) {
      status = app_db_entry_or.status();
      SWSS_LOG_ERROR("Unable to deserialize APP DB entry with key %s: %s",
                     QuotedVar(table_name + ":" + key).c_str(),
                     status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }
    auto& app_db_entry = *app_db_entry_or;

    const std::string next_hop_key =
        KeyGenerator::generateNextHopKey(app_db_entry.next_hop_id);

    // Fulfill the operation.
    const std::string& operation = kfvOp(key_op_fvs_tuple);
    status = validateAppDbEntry(app_db_entry, operation);
    if (!status.ok()) {
      SWSS_LOG_ERROR(
          "Validation failed for Nexthop APP DB entry with key %s: %s",
          QuotedVar(kfvKey(key_op_fvs_tuple)).c_str(),
          status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }

    bool update = (getNextHopEntry(next_hop_key) != nullptr);
    if (prev_op == "") {
      prev_op = operation;
      prev_update = update;
    }
    // Process the entries if the operation type changes.
    if (operation != prev_op || update != prev_update) {
      status = processEntries(entry_list, tuple_list, prev_op, prev_update);
      entry_list.clear();
      tuple_list.clear();
      prev_op = operation;
      prev_update = update;
    }

    if (!status.ok()) {
      // Return SWSS_RC_NOT_EXECUTED if failure has occured.
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple),
                           ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED),
                           /*replace=*/true);

      break;
    }

    if (operation == SET_COMMAND && update) {
      status = ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
               << "Currently next hop doesn't support update by SAI."
               << "Next hop key " << QuotedVar(next_hop_key);
      SWSS_LOG_ERROR("%s", status.message().c_str());

      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    } else {
      entry_list.push_back(app_db_entry);
      tuple_list.push_back(key_op_fvs_tuple);
    }
  }

  if (!entry_list.empty()) {
    auto rc = processEntries(entry_list, tuple_list, prev_op, prev_update);
    if (!rc.ok()) {
      status = rc;
    }
  }
  drainWithNotExecuted();
  return status;
}

P4NextHopEntry *NextHopManager::getNextHopEntry(const std::string &next_hop_key)
{
    SWSS_LOG_ENTER();

    auto it = m_nextHopTable.find(next_hop_key);

    if (it == m_nextHopTable.end())
    {
        return nullptr;
    }
    else
    {
        return &it->second;
    }
}

ReturnCodeOr<P4NextHopAppDbEntry> NextHopManager::deserializeP4NextHopAppDbEntry(
    const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
{
    SWSS_LOG_ENTER();

    P4NextHopAppDbEntry app_db_entry = {};
    app_db_entry.neighbor_id = swss::IpAddress("0.0.0.0");

    try
    {
        nlohmann::json j = nlohmann::json::parse(key);
        app_db_entry.next_hop_id = j[prependMatchField(p4orch::kNexthopId)];
    }
    catch (std::exception &ex)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Failed to deserialize next hop id";
    }

    for (const auto &it : attributes)
    {
        const auto &field = fvField(it);
        const auto &value = fvValue(it);
        if (field == prependParamField(p4orch::kRouterInterfaceId))
        {
            app_db_entry.router_interface_id = value;
        }
        else if (field == prependParamField(p4orch::kNeighborId))
        {
            try
            {
                app_db_entry.neighbor_id = swss::IpAddress(value);
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid IP address " << QuotedVar(value) << " of field " << QuotedVar(field);
            }
        }
        else if (field == prependParamField(p4orch::kTunnelId))
        {
            app_db_entry.gre_tunnel_id = value;
        }
        else if (field == prependParamField(p4orch::kDisableDecrementTtl))
        {
          ASSIGN_OR_RETURN(app_db_entry.disable_decrement_ttl,
                           parseFlag(p4orch::kDisableDecrementTtl, value));
        }
        else if (field == prependParamField(p4orch::kDisableSrcMacRewrite))
        {
          ASSIGN_OR_RETURN(app_db_entry.disable_src_mac_rewrite,
                           parseFlag(p4orch::kDisableSrcMacRewrite, value));
        }
        else if (field == prependParamField(p4orch::kDisableDstMacRewrite))
        {
          ASSIGN_OR_RETURN(app_db_entry.disable_dst_mac_rewrite,
                           parseFlag(p4orch::kDisableDstMacRewrite, value));
        }
        else if (field == prependParamField(p4orch::kDisableVlanRewrite))
        {
          ASSIGN_OR_RETURN(app_db_entry.disable_vlan_rewrite,
                           parseFlag(p4orch::kDisableVlanRewrite, value));
        }
        else if (field == p4orch::kAction)
        {
          app_db_entry.action_str = value;
        }
        else if (field != p4orch::kControllerMetadata)
        {
          return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                 << "Unexpected field " << QuotedVar(field)
                 << " in table entry";
        }
    }

    return app_db_entry;
}

std::vector<ReturnCode> NextHopManager::createNextHops(
    const std::vector<P4NextHopAppDbEntry>& next_hop_entries) {
  std::vector<P4NextHopEntry> entries;
  std::vector<sai_object_id_t> next_hop_oids(next_hop_entries.size());
  std::vector<std::vector<sai_attribute_t>> sai_attrs(next_hop_entries.size());
  std::vector<uint32_t> attrs_cnt(next_hop_entries.size());
  std::vector<const sai_attribute_t*> attrs_ptr(next_hop_entries.size());
  std::vector<sai_status_t> object_statuses(next_hop_entries.size());
  std::vector<ReturnCode> statuses(next_hop_entries.size());

  for (size_t i = 0; i < next_hop_entries.size(); ++i) {
    entries.push_back(P4NextHopEntry(
        next_hop_entries[i].next_hop_id,
        next_hop_entries[i].router_interface_id,
        next_hop_entries[i].gre_tunnel_id, next_hop_entries[i].neighbor_id,
        next_hop_entries[i].disable_decrement_ttl,
        next_hop_entries[i].disable_src_mac_rewrite,
        next_hop_entries[i].disable_dst_mac_rewrite,
        next_hop_entries[i].disable_vlan_rewrite));
    if (!entries[i].gre_tunnel_id.empty()) {
      auto gre_tunnel_or =
          gP4Orch->getGreTunnelManager()->getConstGreTunnelEntry(
              KeyGenerator::generateTunnelKey(entries[i].gre_tunnel_id));
      entries[i].router_interface_id = (*gre_tunnel_or).router_interface_id;
      // BRCM requires neighbor object to be created before GRE tunnel,
      // referring to the one in GRE tunnel object when creating
      // next_hop_entry_with setTunnelAction
      entries[i].neighbor_id = (*gre_tunnel_or).neighbor_id;
    }

    sai_attrs[i] = getSaiAttrs(entries[i]);
    attrs_cnt[i] = static_cast<uint32_t>(sai_attrs[i].size());
    attrs_ptr[i] = sai_attrs[i].data();
  }

  // Call bulk SAI API.
  sai_next_hop_api->create_next_hops(
      gSwitchId, (uint32_t)entries.size(), attrs_cnt.data(), attrs_ptr.data(),
      SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, next_hop_oids.data(),
      object_statuses.data());

  for (size_t i = 0; i < next_hop_entries.size(); ++i) {
    CHECK_ERROR_AND_LOG(object_statuses[i],
                        "Failed to create next hop with key "
                            << QuotedVar(entries[i].next_hop_key));

    if (object_statuses[i] == SAI_STATUS_SUCCESS) {
      statuses[i] = StatusCode::SWSS_RC_SUCCESS;
      entries[i].next_hop_oid = next_hop_oids[i];

      if (!entries[i].gre_tunnel_id.empty()) {
        // On successful creation, increment ref count for tunnel object
        m_p4OidMapper->increaseRefCount(
            SAI_OBJECT_TYPE_TUNNEL,
            KeyGenerator::generateTunnelKey(entries[i].gre_tunnel_id));
      } else {
        // On successful creation, increment ref count for router intf object
        m_p4OidMapper->increaseRefCount(
            SAI_OBJECT_TYPE_ROUTER_INTERFACE,
            KeyGenerator::generateRouterInterfaceKey(
                entries[i].router_interface_id));
      }

      const auto neighbor_key = KeyGenerator::generateNeighborKey(
          entries[i].router_interface_id, entries[i].neighbor_id);
      m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
                                      neighbor_key);
      if (entries[i].neighbor_id.isV4()) {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEXTHOP);
      } else {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEXTHOP);
      }

      // Add created entry to internal table.
      m_nextHopTable.emplace(entries[i].next_hop_key, entries[i]);

      // Add the key to OID map to centralized mapper.
      m_p4OidMapper->setOID(SAI_OBJECT_TYPE_NEXT_HOP, entries[i].next_hop_key,
                            entries[i].next_hop_oid);
    } else {
      statuses[i] = ReturnCode(object_statuses[i])
                    << "Failed to create next hop with key "
                    << QuotedVar(entries[i].next_hop_key);
    }
  }
  return statuses;
}

std::vector<ReturnCode> NextHopManager::removeNextHops(
    const std::vector<P4NextHopAppDbEntry>& next_hop_entries) {
  SWSS_LOG_ENTER();

  std::vector<P4NextHopEntry*> entries(next_hop_entries.size());
  std::vector<std::string> next_hop_keys(next_hop_entries.size());
  std::vector<sai_object_id_t> next_hop_oids(next_hop_entries.size());
  std::vector<sai_status_t> object_statuses(next_hop_entries.size());
  std::vector<ReturnCode> statuses(next_hop_entries.size());

  for (size_t i = 0; i < next_hop_entries.size(); ++i) {
    statuses[i] = StatusCode::SWSS_RC_UNKNOWN;
    next_hop_keys[i] =
        KeyGenerator::generateNextHopKey(next_hop_entries[i].next_hop_id);

    entries[i] = getNextHopEntry(next_hop_keys[i]);
    if (entries[i] == nullptr) {
      SWSS_LOG_ERROR("Nonexist next hop key %s", next_hop_keys[i].c_str());
      return statuses;
    }
    next_hop_oids[i] = entries[i]->next_hop_oid;
  }

  // Call bulk SAI API.
  sai_next_hop_api->remove_next_hops(
      (uint32_t)next_hop_entries.size(), next_hop_oids.data(),
      SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, object_statuses.data());
  for (size_t i = 0; i < next_hop_entries.size(); ++i) {
    CHECK_ERROR_AND_LOG(object_statuses[i], "Failed to remove next hop "
                                                << QuotedVar(next_hop_keys[i]));

    if (object_statuses[i] == SAI_STATUS_SUCCESS) {
      statuses[i] = StatusCode::SWSS_RC_SUCCESS;
      if (!entries[i]->gre_tunnel_id.empty()) {
        // On successful deletion, decrement ref count for tunnel object
        m_p4OidMapper->decreaseRefCount(
            SAI_OBJECT_TYPE_TUNNEL,
            KeyGenerator::generateTunnelKey(entries[i]->gre_tunnel_id));
      } else {
        // On successful deletion, decrement ref count for router intf object
        m_p4OidMapper->decreaseRefCount(
            SAI_OBJECT_TYPE_ROUTER_INTERFACE,
            KeyGenerator::generateRouterInterfaceKey(
                entries[i]->router_interface_id));
      }

      std::string router_interface_id = entries[i]->router_interface_id;
      if (!entries[i]->gre_tunnel_id.empty()) {
        auto gre_tunnel_or =
            gP4Orch->getGreTunnelManager()->getConstGreTunnelEntry(
                KeyGenerator::generateTunnelKey(entries[i]->gre_tunnel_id));
        router_interface_id = (*gre_tunnel_or).router_interface_id;
      }
      m_p4OidMapper->decreaseRefCount(
          SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
          KeyGenerator::generateNeighborKey(router_interface_id,
                                            entries[i]->neighbor_id));
      if (entries[i]->neighbor_id.isV4()) {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEXTHOP);
      } else {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEXTHOP);
      }

      // Remove the key to OID map to centralized mapper.
      m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_keys[i]);

      // Remove the entry from internal table.
      m_nextHopTable.erase(next_hop_keys[i]);
    } else {
      statuses[i] = ReturnCode(object_statuses[i])
                    << "Failed to remove next hop "
                    << QuotedVar(next_hop_keys[i]);
    }
  }

  return statuses;
}

ReturnCode NextHopManager::processEntries(
    const std::vector<P4NextHopAppDbEntry>& entries,
    const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
    const std::string& op, bool update) {
  SWSS_LOG_ENTER();

  ReturnCode status;
  std::vector<ReturnCode> statuses;
  // In syncd, bulk SAI calls use mode SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR.
  if (op == SET_COMMAND) {
    if (!update) {
      statuses = createNextHops(entries);
    } else {
      LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
                           << "Currently next hop doesn't support update.");
    }
  } else {
    statuses = removeNextHops(entries);
  }

  for (size_t i = 0; i < entries.size(); ++i) {
    m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(tuple_list[i]),
                         kfvFieldsValues(tuple_list[i]), statuses[i],
                         /*replace=*/true);
    if (status.ok() && !statuses[i].ok()) {
      status = statuses[i];
    }
  }

  return status;
}

std::string NextHopManager::verifyState(
    const std::string& key, const std::vector<swss::FieldValueTuple>& tuple) {
  SWSS_LOG_ENTER();

  auto pos = key.find_first_of(kTableKeyDelimiter);
  if (pos == std::string::npos) {
    return std::string("Invalid key: ") + key;
  }
  std::string p4rt_table = key.substr(0, pos);
  std::string p4rt_key = key.substr(pos + 1);
  if (p4rt_table != APP_P4RT_TABLE_NAME) {
    return std::string("Invalid key: ") + key;
  }
  std::string table_name;
  std::string key_content;
  parseP4RTKey(p4rt_key, &table_name, &key_content);
  if (table_name != APP_P4RT_NEXTHOP_TABLE_NAME) {
    return std::string("Invalid key: ") + key;
  }

  ReturnCode status;
  auto app_db_entry_or = deserializeP4NextHopAppDbEntry(key_content, tuple);
  if (!app_db_entry_or.ok()) {
    status = app_db_entry_or.status();
    std::stringstream msg;
    msg << "Unable to deserialize key " << QuotedVar(key) << ": "
        << status.message();
    return msg.str();
  }
  auto& app_db_entry = *app_db_entry_or;
  const std::string next_hop_key =
      KeyGenerator::generateNextHopKey(app_db_entry.next_hop_id);
  auto* next_hop_entry = getNextHopEntry(next_hop_key);
  if (next_hop_entry == nullptr) {
    std::stringstream msg;
    msg << "No entry found with key " << QuotedVar(key);
    return msg.str();
  }

  std::string cache_result = verifyStateCache(app_db_entry, next_hop_entry);
  std::string asic_db_result = verifyStateAsicDb(next_hop_entry);
  if (cache_result.empty()) {
    return asic_db_result;
  }
  if (asic_db_result.empty()) {
    return cache_result;
  }
  return cache_result + "; " + asic_db_result;
}

std::string NextHopManager::verifyStateCache(const P4NextHopAppDbEntry &app_db_entry,
                                             const P4NextHopEntry *next_hop_entry)
{
    const std::string next_hop_key = KeyGenerator::generateNextHopKey(app_db_entry.next_hop_id);
    if (next_hop_entry->next_hop_key != next_hop_key)
    {
        std::stringstream msg;
        msg << "Nexthop with key " << QuotedVar(next_hop_key) << " does not match internal cache "
            << QuotedVar(next_hop_entry->next_hop_key) << " in nexthop manager.";
        return msg.str();
    }
    if (next_hop_entry->next_hop_id != app_db_entry.next_hop_id)
    {
        std::stringstream msg;
        msg << "Nexthop " << QuotedVar(app_db_entry.next_hop_id) << " does not match internal cache "
            << QuotedVar(next_hop_entry->next_hop_id) << " in nexthop manager.";
        return msg.str();
    }
    if (app_db_entry.action_str == p4orch::kSetIpNexthop &&
        next_hop_entry->router_interface_id != app_db_entry.router_interface_id)
    {
        std::stringstream msg;
        msg << "Nexthop " << QuotedVar(app_db_entry.next_hop_id) << " with ritf ID "
            << QuotedVar(app_db_entry.router_interface_id) << " does not match internal cache "
            << QuotedVar(next_hop_entry->router_interface_id) << " in nexthop manager.";
        return msg.str();
    }
    if (app_db_entry.action_str == p4orch::kSetIpNexthop &&
        next_hop_entry->neighbor_id.to_string() != app_db_entry.neighbor_id.to_string())
    {
        std::stringstream msg;
        msg << "Nexthop " << QuotedVar(app_db_entry.next_hop_id) << " with neighbor ID "
            << app_db_entry.neighbor_id.to_string() << " does not match internal cache "
            << next_hop_entry->neighbor_id.to_string() << " in nexthop manager.";
        return msg.str();
    }

    if (app_db_entry.action_str == p4orch::kSetTunnelNexthop &&
        next_hop_entry->gre_tunnel_id != app_db_entry.gre_tunnel_id)
    {
        std::stringstream msg;
        msg << "Nexthop " << QuotedVar(app_db_entry.next_hop_id) << " with GRE tunnel ID "
            << QuotedVar(app_db_entry.gre_tunnel_id) << " does not match internal cache "
            << QuotedVar(next_hop_entry->gre_tunnel_id) << " in nexthop manager.";
        return msg.str();
    }
    if (next_hop_entry->disable_decrement_ttl !=
        app_db_entry.disable_decrement_ttl) {
      std::stringstream msg;
      msg << "Nexthop " << QuotedVar(app_db_entry.next_hop_id)
          << " with flag disable_decrement_ttl set to "
          << QuotedVar(app_db_entry.disable_decrement_ttl ? "true" : "false")
          << " does not match internal cache "
          << QuotedVar(next_hop_entry->disable_decrement_ttl ? "true" : "false")
          << " in nexthop manager.";
      return msg.str();
    }
    if (next_hop_entry->disable_src_mac_rewrite !=
        app_db_entry.disable_src_mac_rewrite) {
      std::stringstream msg;
      msg << "Nexthop " << QuotedVar(app_db_entry.next_hop_id)
          << " with flag disable_src_mac_rewrite set to "
          << QuotedVar(app_db_entry.disable_src_mac_rewrite ? "true" : "false")
          << " does not match internal cache "
          << QuotedVar(next_hop_entry->disable_src_mac_rewrite ? "true"
                                                               : "false")
          << " in nexthop manager.";
      return msg.str();
    }
    if (next_hop_entry->disable_dst_mac_rewrite !=
        app_db_entry.disable_dst_mac_rewrite) {
      std::stringstream msg;
      msg << "Nexthop " << QuotedVar(app_db_entry.next_hop_id)
          << " with flag disable_dst_mac_rewrite set to "
          << QuotedVar(app_db_entry.disable_dst_mac_rewrite ? "true" : "false")
          << " does not match internal cache "
          << QuotedVar(next_hop_entry->disable_dst_mac_rewrite ? "true"
                                                               : "false")
          << " in nexthop manager.";
      return msg.str();
    }
    if (next_hop_entry->disable_vlan_rewrite !=
        app_db_entry.disable_vlan_rewrite) {
      std::stringstream msg;
      msg << "Nexthop " << QuotedVar(app_db_entry.next_hop_id)
          << " with flag disable_vlan_rewrite set to "
          << QuotedVar(app_db_entry.disable_vlan_rewrite ? "true" : "false")
          << " does not match internal cache "
          << QuotedVar(next_hop_entry->disable_vlan_rewrite ? "true" : "false")
          << " in nexthop manager.";
      return msg.str();
    }
    if (!next_hop_entry->gre_tunnel_id.empty())
    {
        auto gre_tunnel_or = gP4Orch->getGreTunnelManager()->getConstGreTunnelEntry(
            KeyGenerator::generateTunnelKey(next_hop_entry->gre_tunnel_id));
        if (!gre_tunnel_or.ok())
        {
            std::stringstream msg;
            msg << "GRE Tunnel " << QuotedVar(next_hop_entry->gre_tunnel_id) << " does not exist in GRE Tunnel Manager";
            return msg.str();
        }
        P4GreTunnelEntry gre_tunnel = *gre_tunnel_or;
        if (gre_tunnel.neighbor_id.to_string() != next_hop_entry->neighbor_id.to_string())
        {
            std::stringstream msg;
            msg << "Nexthop " << QuotedVar(next_hop_entry->next_hop_id) << " with neighbor ID "
                << QuotedVar(next_hop_entry->neighbor_id.to_string())
                << " in nexthop manager does not match internal cache " << QuotedVar(gre_tunnel.neighbor_id.to_string())
                << " with tunnel ID " << QuotedVar(gre_tunnel.tunnel_id) << " in GRE tunnel manager.";
            return msg.str();
        }
        if (gre_tunnel.router_interface_id != next_hop_entry->router_interface_id)
        {
            std::stringstream msg;
            msg << "Nexthop " << QuotedVar(next_hop_entry->next_hop_id) << " with rif ID "
                << QuotedVar(next_hop_entry->router_interface_id)
                << " in nexthop manager does not match internal cache " << QuotedVar(gre_tunnel.router_interface_id)
                << " with tunnel ID " << QuotedVar(gre_tunnel.tunnel_id) << " in GRE tunnel manager.";
            return msg.str();
        }
    }

    return m_p4OidMapper->verifyOIDMapping(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_entry->next_hop_key,
                                           next_hop_entry->next_hop_oid);
}

std::string NextHopManager::verifyStateAsicDb(const P4NextHopEntry *next_hop_entry)
{
  std::vector<sai_attribute_t> attrs = getSaiAttrs(*next_hop_entry);
  std::vector<swss::FieldValueTuple> exp =
      saimeta::SaiAttributeList::serialize_attr_list(
          SAI_OBJECT_TYPE_NEXT_HOP, (uint32_t)attrs.size(), attrs.data(),
          /*countOnly=*/false);

  swss::DBConnector db("ASIC_DB", 0);
  swss::Table table(&db, "ASIC_STATE");
  std::string key = sai_serialize_object_type(SAI_OBJECT_TYPE_NEXT_HOP) + ":" +
                    sai_serialize_object_id(next_hop_entry->next_hop_oid);
  std::vector<swss::FieldValueTuple> values;
  if (!table.get(key, values)) {
    return std::string("ASIC DB key not found ") + key;
  }

    return verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                       /*allow_unknown=*/false);
}
