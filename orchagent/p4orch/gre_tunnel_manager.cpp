#include "p4orch/gre_tunnel_manager.h"

#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "SaiAttributeList.h"
#include "crmorch.h"
#include "dbconnector.h"
#include "ipaddress.h"
#include "logger.h"
#include "p4orch/p4orch_util.h"
#include "sai_serialize.h"
#include "swssnet.h"
#include "table.h"
#include "neighbor_manager.h"

extern "C"
{
#include "sai.h"
}

using ::p4orch::kTableKeyDelimiter;

extern sai_object_id_t gSwitchId;
extern sai_tunnel_api_t *sai_tunnel_api;
extern sai_router_interface_api_t *sai_router_intfs_api;
extern CrmOrch *gCrmOrch;
extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t gUnderlayIfId;

namespace
{

std::vector<sai_attribute_t> prepareSaiAttrs(const P4GreTunnelEntry &gre_tunnel_entry)
{
    std::vector<sai_attribute_t> tunnel_attrs;
    sai_attribute_t tunnel_attr;
    tunnel_attr.id = SAI_TUNNEL_ATTR_TYPE;
    tunnel_attr.value.s32 = SAI_TUNNEL_TYPE_IPINIP_GRE;
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_TUNNEL_ATTR_PEER_MODE;
    tunnel_attr.value.s32 = SAI_TUNNEL_PEER_MODE_P2P;
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
    tunnel_attr.value.oid = gre_tunnel_entry.underlay_if_oid;
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_TUNNEL_ATTR_OVERLAY_INTERFACE;
    tunnel_attr.value.oid = gre_tunnel_entry.overlay_if_oid;
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
    swss::copy(tunnel_attr.value.ipaddr, gre_tunnel_entry.encap_src_ip);
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_TUNNEL_ATTR_ENCAP_DST_IP;
    swss::copy(tunnel_attr.value.ipaddr, gre_tunnel_entry.encap_dst_ip);
    tunnel_attrs.push_back(tunnel_attr);
    return tunnel_attrs;
}

} // namespace

P4GreTunnelEntry* GreTunnelManager::getGreTunnelEntry(
    const std::string& tunnel_key) {
  SWSS_LOG_ENTER();

  auto it = m_greTunnelTable.find(tunnel_key);
  if (it == m_greTunnelTable.end()) {
    return nullptr;
  } else {
    return &it->second;
  }
};

ReturnCode GreTunnelManager::validateGreTunnelAppDbEntry(
    const P4GreTunnelAppDbEntry& app_db_entry) {
  if (app_db_entry.action_str != p4orch::kTunnelAction) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Invalid action " << QuotedVar(app_db_entry.action_str)
           << " of GRE Tunnel App DB entry";
  }
  if (app_db_entry.router_interface_id.empty()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << QuotedVar(prependParamField(p4orch::kRouterInterfaceId))
           << " field is missing in table entry";
  }
  if (app_db_entry.encap_src_ip.isZero()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << QuotedVar(prependParamField(p4orch::kEncapSrcIp))
           << " field is missing in table entry";
  }
  if (app_db_entry.encap_dst_ip.isZero()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << QuotedVar(prependParamField(p4orch::kEncapDstIp))
           << " field is missing in table entry";
  }
  return ReturnCode();
}

ReturnCode GreTunnelManager::validateGreTunnelAppDbEntry(
    const P4GreTunnelAppDbEntry& app_db_entry, const std::string& operation) {
  SWSS_LOG_ENTER();

  P4GreTunnelEntry entry =
      P4GreTunnelEntry(app_db_entry.tunnel_id, app_db_entry.router_interface_id,
                       app_db_entry.encap_src_ip, app_db_entry.encap_dst_ip,
                       app_db_entry.encap_dst_ip);

  const auto router_interface_key =
      KeyGenerator::generateRouterInterfaceKey(entry.router_interface_id);

  if (operation == SET_COMMAND) {
    RETURN_IF_ERROR(validateGreTunnelAppDbEntry(app_db_entry));
    if (getGreTunnelEntry(entry.tunnel_key) == nullptr) {
      if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_TUNNEL, entry.tunnel_key)) {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
            "GRE tunnel with key " << QuotedVar(entry.tunnel_key)
                                   << " already exists in centralized mapper");
      }

      // From centralized mapper, get OID of router interface that GRE tunnel
      // depends on.
      if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                 router_interface_key,
                                 &entry.underlay_if_oid)) {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "Router intf "
                             << QuotedVar(entry.router_interface_id)
                             << " does not exist");
      }
      // From centralized mapper, get neighbor key that GRE tunnel
      // depends on.
      const auto neighbor_key = KeyGenerator::generateNeighborKey(
          entry.router_interface_id, entry.neighbor_id);
      if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
                                    neighbor_key)) {
        LOG_ERROR_AND_RETURN(
            ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
            << "Neighbor with rif=" << QuotedVar(entry.router_interface_id)
            << ", neighbor_ip=" << QuotedVar(entry.neighbor_id.to_string())
            << " does not exist");
      }
    }
  } else if (operation == DEL_COMMAND) {
    // Check the existence of the GRE tunnel in GRE tunnel manager and
    // centralized mapper.
    if (getGreTunnelEntry(entry.tunnel_key) == nullptr) {
      LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                           << "GRE tunnel with key "
                           << QuotedVar(entry.tunnel_key)
                           << " does not exist in GRE tunnel manager");
    }

    // Check if there is anything referring to the GRE tunnel before deletion.
    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_TUNNEL, entry.tunnel_key,
                                    &ref_count)) {
      RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
          "Failed to get reference count for GRE tunnel "
          << QuotedVar(entry.tunnel_key));
    }
    if (ref_count > 0) {
      LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "GRE tunnel " << QuotedVar(entry.tunnel_key)
                           << " referenced by other objects (ref_count = "
                           << ref_count);
    }
  }

  return ReturnCode();
}

P4GreTunnelEntry::P4GreTunnelEntry(const std::string &tunnel_id, const std::string &router_interface_id,
                                   const swss::IpAddress &encap_src_ip, const swss::IpAddress &encap_dst_ip,
                                   const swss::IpAddress &neighbor_id)
    : tunnel_id(tunnel_id), router_interface_id(router_interface_id), encap_src_ip(encap_src_ip),
      encap_dst_ip(encap_dst_ip), neighbor_id(neighbor_id)
{
    SWSS_LOG_ENTER();
    tunnel_key = KeyGenerator::generateTunnelKey(tunnel_id);
}

ReturnCode GreTunnelManager::getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                                          std::string &object_key)
{
    return StatusCode::SWSS_RC_UNIMPLEMENTED;
}

void GreTunnelManager::enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry)
{
    m_entries.push_back(entry);
}

void GreTunnelManager::drainWithNotExecuted() {
  drainMgmtWithNotExecuted(m_entries, m_publisher);
}

ReturnCode GreTunnelManager::drain() {
  SWSS_LOG_ENTER();

  std::vector<P4GreTunnelAppDbEntry> entry_list;
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
    const std::string& operation = kfvOp(key_op_fvs_tuple);

    auto app_db_entry_or = deserializeP4GreTunnelAppDbEntry(key, attributes);
    if (!app_db_entry_or.ok()) {
      status = app_db_entry_or.status();
      SWSS_LOG_ERROR(
          "Unable to deserialize  GRE Tunnel APP DB entry with key %s: %s",
          QuotedVar(kfvKey(key_op_fvs_tuple)).c_str(),
          status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }
    auto& app_db_entry = *app_db_entry_or;

    const std::string tunnel_key =
        KeyGenerator::generateTunnelKey(app_db_entry.tunnel_id);

    bool update = (getGreTunnelEntry(tunnel_key) != nullptr);

    status = validateGreTunnelAppDbEntry(app_db_entry, operation);
    if (!status.ok()) {
      SWSS_LOG_ERROR(
          "Validation failed for GRE Tunnel APP DB entry with key %s: %s",
          QuotedVar(kfvKey(key_op_fvs_tuple)).c_str(),
          status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }

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
               << "Currently GRE tunnel doesn't support update by SAI."
               << "GRE tunnel key " << QuotedVar(tunnel_key);
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

ReturnCodeOr<const P4GreTunnelEntry> GreTunnelManager::getConstGreTunnelEntry(const std::string &tunnel_key)
{
    SWSS_LOG_ENTER();

    auto *tunnel = getGreTunnelEntry(tunnel_key);
    if (tunnel == nullptr)
    {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
               << "GRE Tunnel with key " << QuotedVar(tunnel_key) << " was not found.";
    }
    else
    {
        return *tunnel;
    }
}

ReturnCodeOr<P4GreTunnelAppDbEntry> GreTunnelManager::deserializeP4GreTunnelAppDbEntry(
    const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
{
    SWSS_LOG_ENTER();

    P4GreTunnelAppDbEntry app_db_entry = {};
    app_db_entry.encap_src_ip = swss::IpAddress("0.0.0.0");
    app_db_entry.encap_dst_ip = swss::IpAddress("0.0.0.0");

    try
    {
        nlohmann::json j = nlohmann::json::parse(key);
        app_db_entry.tunnel_id = j[prependMatchField(p4orch::kTunnelId)];
    }
    catch (std::exception &ex)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Failed to deserialize GRE tunnel id";
    }

    for (const auto &it : attributes)
    {
        const auto &field = fvField(it);
        const auto &value = fvValue(it);
        if (field == prependParamField(p4orch::kRouterInterfaceId))
        {
            app_db_entry.router_interface_id = value;
        }
        else if (field == prependParamField(p4orch::kEncapSrcIp))
        {
            try
            {
                app_db_entry.encap_src_ip = swss::IpAddress(value);
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid IP address " << QuotedVar(value) << " of field " << QuotedVar(field);
            }
        }
        else if (field == prependParamField(p4orch::kEncapDstIp))
        {
            try
            {
                app_db_entry.encap_dst_ip = swss::IpAddress(value);
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid IP address " << QuotedVar(value) << " of field " << QuotedVar(field);
            }
        }
        else if (field == p4orch::kAction)
        {
            app_db_entry.action_str = value;
        }
        else if (field != p4orch::kControllerMetadata)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Unexpected field " << QuotedVar(field) << " in table entry";
        }
    }

    return app_db_entry;
}

std::vector<ReturnCode> GreTunnelManager::createGreTunnels(
    const std::vector<P4GreTunnelAppDbEntry>& gre_tunnel_entries)
{
    SWSS_LOG_ENTER();

  std::vector<P4GreTunnelEntry> entries;
  std::vector<std::string> router_interface_keys(gre_tunnel_entries.size());
  std::vector<sai_object_id_t> tunnel_oids(gre_tunnel_entries.size());
  std::vector<std::vector<sai_attribute_t>> sai_attrs(
      gre_tunnel_entries.size());
  std::vector<uint32_t> attrs_cnt(gre_tunnel_entries.size());
  std::vector<const sai_attribute_t*> attrs_ptr(gre_tunnel_entries.size());
  std::vector<sai_status_t> object_statuses(gre_tunnel_entries.size());
  std::vector<ReturnCode> statuses(gre_tunnel_entries.size());

  for (size_t i = 0; i < gre_tunnel_entries.size(); ++i) {
    statuses[i] = StatusCode::SWSS_RC_UNKNOWN;
    entries.push_back(P4GreTunnelEntry(
        gre_tunnel_entries[i].tunnel_id,
        gre_tunnel_entries[i].router_interface_id,
        gre_tunnel_entries[i].encap_src_ip, gre_tunnel_entries[i].encap_dst_ip,
        gre_tunnel_entries[i].encap_dst_ip));

    // From centralized mapper, get OID of router interface that GRE tunnel
    // depends on.
    router_interface_keys[i] = KeyGenerator::generateRouterInterfaceKey(
        entries[i].router_interface_id);
    m_p4OidMapper->getOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                          router_interface_keys[i],
                          &entries[i].underlay_if_oid);

    // Prepare attributes for the SAI creation call.
    // TODO: Remove when SAI_TUNNEL_ATTR_OVERLAY_INTERFACE is not
    // mandatory Use gUnderlayIfId, a shared global loopback rif, for encap
    // tunnels
    entries[i].overlay_if_oid = gUnderlayIfId;

    sai_attrs[i] = prepareSaiAttrs(entries[i]);
    attrs_cnt[i] = static_cast<uint32_t>(sai_attrs[i].size());
    attrs_ptr[i] = sai_attrs[i].data();
  }


  // Call bulk SAI API.
  sai_tunnel_api->create_tunnels(gSwitchId, (uint32_t)entries.size(),
                                 attrs_cnt.data(), attrs_ptr.data(),
                                 SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR,
                                 tunnel_oids.data(), object_statuses.data());

  for (size_t i = 0; i < gre_tunnel_entries.size(); ++i) {
    CHECK_ERROR_AND_LOG(object_statuses[i],
                        "Failed to create GRE tunnel "
                        << QuotedVar(entries[i].tunnel_key) << " on rif "
                        << QuotedVar(entries[i].router_interface_id));

    if (object_statuses[i] == SAI_STATUS_SUCCESS) {
      statuses[i] = StatusCode::SWSS_RC_SUCCESS;
      entries[i].tunnel_oid = tunnel_oids[i];

      // On successful creation, increment ref count.
      m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      router_interface_keys[i]);

      // On successful creation, increment ref count on neighbor object.
      m_p4OidMapper->increaseRefCount(
          SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
          KeyGenerator::generateNeighborKey(entries[i].router_interface_id,
                                            entries[i].neighbor_id));

      // Add created entry to internal table.
      m_greTunnelTable.emplace(entries[i].tunnel_key, entries[i]);

      // Add the key to OID map to centralized mapper.
      m_p4OidMapper->setOID(SAI_OBJECT_TYPE_TUNNEL, entries[i].tunnel_key,
                            entries[i].tunnel_oid);
    } else {
      statuses[i] = ReturnCode(object_statuses[i])
                    << "Failed to create GRE tunnel "
                    << QuotedVar(entries[i].tunnel_key) << " on rif "
                    << QuotedVar(entries[i].router_interface_id);
    }
  }

  return statuses;
}

std::vector<ReturnCode> GreTunnelManager::removeGreTunnels(
    const std::vector<P4GreTunnelAppDbEntry>& gre_tunnel_entries) {
  SWSS_LOG_ENTER();

  std::vector<P4GreTunnelEntry*> entries(gre_tunnel_entries.size());
  std::vector<sai_object_id_t> tunnel_oids(gre_tunnel_entries.size());
  std::vector<sai_status_t> object_statuses(gre_tunnel_entries.size());
  std::vector<ReturnCode> statuses(gre_tunnel_entries.size());

  for (size_t i = 0; i < gre_tunnel_entries.size(); ++i) {
    statuses[i] = StatusCode::SWSS_RC_UNKNOWN;

    const std::string tunnel_key =
      KeyGenerator::generateTunnelKey(gre_tunnel_entries[i].tunnel_id);

    entries[i] = getGreTunnelEntry(tunnel_key);
    tunnel_oids[i] = entries[i]->tunnel_oid;
  }

  // Call bulk SAI API.
  sai_tunnel_api->remove_tunnels(
      (uint32_t)gre_tunnel_entries.size(), tunnel_oids.data(),
      SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, object_statuses.data());

  for (size_t i = 0; i < gre_tunnel_entries.size(); ++i) {
    CHECK_ERROR_AND_LOG(
        object_statuses[i],
        "Failed to remove GRE tunnel " << QuotedVar(entries[i]->tunnel_key));

    if (object_statuses[i] == SAI_STATUS_SUCCESS) {
      statuses[i] = StatusCode::SWSS_RC_SUCCESS;

      // On successful deletion, decrement ref count.
      m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      KeyGenerator::generateRouterInterfaceKey(
                                          entries[i]->router_interface_id));
      // On successful deletion, decrement ref count on Neighbor Key.
      m_p4OidMapper->decreaseRefCount(
          SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
          KeyGenerator::generateNeighborKey(entries[i]->router_interface_id,
                                            entries[i]->neighbor_id));

      // Remove the key to OID map to centralized mapper.
      m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_TUNNEL, entries[i]->tunnel_key);

      // Remove the entry from internal table.
      m_greTunnelTable.erase(entries[i]->tunnel_key);
    } else {
      statuses[i] = ReturnCode(object_statuses[i])
                    << "Failed to remove GRE tunnel "
                    << QuotedVar(entries[i]->tunnel_key);
    }
  }

  return statuses;
}

ReturnCode GreTunnelManager::processEntries(
    const std::vector<P4GreTunnelAppDbEntry>& entries,
    const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
    const std::string& op, bool update) {
  SWSS_LOG_ENTER();

  ReturnCode status;
  std::vector<ReturnCode> statuses;
  // In syncd, bulk SAI calls use mode SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR.
  if (op == SET_COMMAND) {
    if (!update) {
      statuses = createGreTunnels(entries);
    } else {
      // Should never happen, as validateGreTunnelAppDbEntry() should fail if
      // the operation is update.
      LOG_ERROR_AND_RETURN(
          ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
          << "Currently GRE tunnel doesn't support update by SAI.");
    }
  } else {
    statuses = removeGreTunnels(entries);
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

std::string GreTunnelManager::verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
{
    SWSS_LOG_ENTER();

    auto pos = key.find_first_of(kTableKeyDelimiter);
    if (pos == std::string::npos)
    {
        return std::string("Invalid key: ") + key;
    }
    std::string p4rt_table = key.substr(0, pos);
    std::string p4rt_key = key.substr(pos + 1);
    if (p4rt_table != APP_P4RT_TABLE_NAME)
    {
        return std::string("Invalid key: ") + key;
    }
    std::string table_name;
    std::string key_content;
    parseP4RTKey(p4rt_key, &table_name, &key_content);
    if (table_name != APP_P4RT_TUNNEL_TABLE_NAME)
    {
        return std::string("Invalid key: ") + key;
    }

    ReturnCode status;
    auto app_db_entry_or = deserializeP4GreTunnelAppDbEntry(key_content, tuple);
    if (!app_db_entry_or.ok())
    {
        status = app_db_entry_or.status();
        std::stringstream msg;
        msg << "Unable to deserialize key " << QuotedVar(key) << ": " << status.message();
        return msg.str();
    }
    auto &app_db_entry = *app_db_entry_or;
    const std::string tunnel_key = KeyGenerator::generateTunnelKey(app_db_entry.tunnel_id);
    auto *gre_tunnel_entry = getGreTunnelEntry(tunnel_key);
    if (gre_tunnel_entry == nullptr)
    {
        std::stringstream msg;
        msg << "No entry found with key " << QuotedVar(key);
        return msg.str();
    }

    std::string cache_result = verifyStateCache(app_db_entry, gre_tunnel_entry);
    std::string asic_db_result = verifyStateAsicDb(gre_tunnel_entry);
    if (cache_result.empty())
    {
        return asic_db_result;
    }
    if (asic_db_result.empty())
    {
        return cache_result;
    }
    return cache_result + "; " + asic_db_result;
}

std::string GreTunnelManager::verifyStateCache(const P4GreTunnelAppDbEntry &app_db_entry,
                                               const P4GreTunnelEntry *gre_tunnel_entry)
{
    const std::string tunnel_key = KeyGenerator::generateTunnelKey(app_db_entry.tunnel_id);
    ReturnCode status = validateGreTunnelAppDbEntry(app_db_entry, SET_COMMAND);
    if (!status.ok())
    {
        std::stringstream msg;
        msg << "Validation failed for GRE Tunnel DB entry with key " << QuotedVar(tunnel_key) << ": "
            << status.message();
        return msg.str();
    }

    if (gre_tunnel_entry->tunnel_key != tunnel_key)
    {
        std::stringstream msg;
        msg << "GreTunnel with key " << QuotedVar(tunnel_key) << " does not match internal cache "
            << QuotedVar(gre_tunnel_entry->tunnel_key) << " in Gre Tunnel manager.";
        return msg.str();
    }
    if (gre_tunnel_entry->tunnel_id != app_db_entry.tunnel_id)
    {
        std::stringstream msg;
        msg << "GreTunnel " << QuotedVar(app_db_entry.tunnel_id) << " does not match internal cache "
            << QuotedVar(gre_tunnel_entry->tunnel_id) << " in GreTunnel manager.";
        return msg.str();
    }
    if (gre_tunnel_entry->router_interface_id != app_db_entry.router_interface_id)
    {
        std::stringstream msg;
        msg << "GreTunnel " << QuotedVar(app_db_entry.tunnel_id) << " with ritf ID "
            << QuotedVar(app_db_entry.router_interface_id) << " does not match internal cache "
            << QuotedVar(gre_tunnel_entry->router_interface_id) << " in GreTunnel manager.";
        return msg.str();
    }
    if (gre_tunnel_entry->encap_src_ip.to_string() != app_db_entry.encap_src_ip.to_string())
    {
        std::stringstream msg;
        msg << "GreTunnel " << QuotedVar(app_db_entry.tunnel_id) << " with source IP "
            << QuotedVar(app_db_entry.encap_src_ip.to_string()) << " does not match internal cache "
            << QuotedVar(gre_tunnel_entry->encap_src_ip.to_string()) << " in GreTunnel manager.";
        return msg.str();
    }

    if (gre_tunnel_entry->encap_dst_ip.to_string() != app_db_entry.encap_dst_ip.to_string())
    {
        std::stringstream msg;
        msg << "GreTunnel " << QuotedVar(app_db_entry.tunnel_id) << " with destination IP "
            << QuotedVar(app_db_entry.encap_dst_ip.to_string()) << " does not match internal cache "
            << QuotedVar(gre_tunnel_entry->encap_dst_ip.to_string()) << " in GreTunnel manager.";
        return msg.str();
    }

    if (gre_tunnel_entry->neighbor_id.to_string() != app_db_entry.encap_dst_ip.to_string())
    {
        std::stringstream msg;
        msg << "GreTunnel " << QuotedVar(app_db_entry.tunnel_id) << " with destination IP "
            << QuotedVar(app_db_entry.encap_dst_ip.to_string()) << " does not match internal cache "
            << QuotedVar(gre_tunnel_entry->neighbor_id.to_string()) << " fo neighbor_id in GreTunnel manager.";
        return msg.str();
    }

    return m_p4OidMapper->verifyOIDMapping(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_entry->tunnel_key,
                                           gre_tunnel_entry->tunnel_oid);
}

std::string GreTunnelManager::verifyStateAsicDb(const P4GreTunnelEntry *gre_tunnel_entry)
{
    swss::DBConnector db("ASIC_DB", 0);
    swss::Table table(&db, "ASIC_STATE");

    // Verify Tunnel ASIC DB attributes
    std::vector<sai_attribute_t> attrs = prepareSaiAttrs(*gre_tunnel_entry);
    std::vector<swss::FieldValueTuple> exp =
        saimeta::SaiAttributeList::serialize_attr_list(
            SAI_OBJECT_TYPE_TUNNEL, (uint32_t)attrs.size(), attrs.data(),
            /*countOnly=*/false);
    std::string key = sai_serialize_object_type(SAI_OBJECT_TYPE_TUNNEL) + ":" +
                      sai_serialize_object_id(gre_tunnel_entry->tunnel_oid);
    std::vector<swss::FieldValueTuple> values;
    if (!table.get(key, values))
    {
        return std::string("ASIC DB key not found ") + key;
    }

    return verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                       /*allow_unknown=*/false);
}
