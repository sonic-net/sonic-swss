#include "p4orch/neighbor_manager.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "SaiAttributeList.h"
#include "crmorch.h"
#include "dbconnector.h"
#include "logger.h"
#include "orch.h"
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

extern sai_neighbor_api_t *sai_neighbor_api;

extern CrmOrch *gCrmOrch;

namespace
{

std::vector<sai_attribute_t> prepareSaiAttrs(const P4NeighborEntry &neighbor_entry)
{
    std::vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, neighbor_entry.dst_mac_address.getMac(), sizeof(sai_mac_t));
    attrs.push_back(attr);

    // Do not program host route.
    // This is mainly for neighbor with IPv6 link-local addresses.
    attr.id = SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE;
    attr.value.booldata = true;
    attrs.push_back(attr);

    return attrs;
}

} // namespace

P4NeighborEntry::P4NeighborEntry(const std::string &router_interface_id, const swss::IpAddress &ip_address,
                                 const swss::MacAddress &mac_address)
{
    SWSS_LOG_ENTER();

    router_intf_id = router_interface_id;
    neighbor_id = ip_address;
    dst_mac_address = mac_address;

    router_intf_key = KeyGenerator::generateRouterInterfaceKey(router_intf_id);
    neighbor_key = KeyGenerator::generateNeighborKey(router_intf_id, neighbor_id);
}

sai_neighbor_entry_t NeighborManager::prepareSaiEntry(
	const P4NeighborEntry& neighbor_entry) {
    const std::string &router_intf_key = neighbor_entry.router_intf_key;
    sai_object_id_t router_intf_oid;
    m_p4OidMapper->getOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key,
                        &router_intf_oid);

    sai_neighbor_entry_t neigh_entry;
    neigh_entry.switch_id = gSwitchId;
    copy(neigh_entry.ip_address, neighbor_entry.neighbor_id);
    neigh_entry.rif_id = router_intf_oid;

    return neigh_entry;
}

ReturnCodeOr<P4NeighborAppDbEntry> NeighborManager::deserializeNeighborEntry(
    const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
{
    SWSS_LOG_ENTER();

    P4NeighborAppDbEntry app_db_entry = {};
    std::string ip_address;
    try
    {
        nlohmann::json j = nlohmann::json::parse(key);
        app_db_entry.router_intf_id = j[prependMatchField(p4orch::kRouterInterfaceId)];
        ip_address = j[prependMatchField(p4orch::kNeighborId)];
    }
    catch (std::exception &ex)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Failed to deserialize key";
    }
    try
    {
        app_db_entry.neighbor_id = swss::IpAddress(ip_address);
    }
    catch (std::exception &ex)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Invalid IP address " << QuotedVar(ip_address) << " of field "
               << QuotedVar(prependMatchField(p4orch::kNeighborId));
    }

    for (const auto &it : attributes)
    {
        const auto &field = fvField(it);
        const auto &value = fvValue(it);
        if (field == prependParamField(p4orch::kDstMac))
        {
            try
            {
                app_db_entry.dst_mac_address = swss::MacAddress(value);
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid MAC address " << QuotedVar(value) << " of field " << QuotedVar(field);
            }
            app_db_entry.is_set_dst_mac = true;
        }
        else if (field != p4orch::kAction && field != p4orch::kControllerMetadata)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Unexpected field " << QuotedVar(field) << " in table entry";
        }
    }

    return app_db_entry;
}

ReturnCode NeighborManager::validateNeighborAppDbEntry(const P4NeighborAppDbEntry &app_db_entry)
{
    SWSS_LOG_ENTER();
    // Perform generic APP DB entry validations.

    const std::string router_intf_key = KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_intf_id);
    if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key))
    {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
               << "Router interface id " << QuotedVar(app_db_entry.router_intf_id) << " does not exist";
    }

    if ((app_db_entry.is_set_dst_mac) && (app_db_entry.dst_mac_address.to_string() == "00:00:00:00:00:00"))
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Invalid dst mac address " << QuotedVar(app_db_entry.dst_mac_address.to_string());
    }

    return ReturnCode();
}

ReturnCode NeighborManager::validateNeighborEntryOperation(
    const P4NeighborAppDbEntry& app_db_entry, const std::string& operation)
{
    SWSS_LOG_ENTER();

    RETURN_IF_ERROR(validateNeighborAppDbEntry(app_db_entry));
    const std::string neighbor_key = KeyGenerator::generateNeighborKey(
        app_db_entry.router_intf_id, app_db_entry.neighbor_id);
    bool exist = (getNeighborEntry(neighbor_key) != nullptr);
    if (operation == SET_COMMAND) {
      if (!exist) {
        if (!app_db_entry.is_set_dst_mac) {
          return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                 << p4orch::kDstMac
                 << " is mandatory to create neighbor entry. Failed to create "
                    "neighbor with key "
                 << QuotedVar(neighbor_key);
        }
        if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
                                     neighbor_key)) {
          RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
              "Neighbor entry with key " << QuotedVar(neighbor_key)
                                         << " already exists in centralized map");
        }
      }
    } else if (operation == DEL_COMMAND) {
      if (!exist) {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
               << "Neighbor with key " << QuotedVar(neighbor_key)
               << " does not exist";
      }
      uint32_t ref_count;
      if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
                                      neighbor_key, &ref_count)) {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
            "Failed to get reference count of neighbor with key "
            << QuotedVar(neighbor_key));
      }
      if (ref_count > 0) {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Neighbor with key " << QuotedVar(neighbor_key)
               << " referenced by other objects (ref_count = " << ref_count
               << ")";
      }
    }
    else
    {
      return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
             << "Unknown operation type " << QuotedVar(operation);
    }

    return ReturnCode();
}

P4NeighborEntry* NeighborManager::getNeighborEntry(
    const std::string& neighbor_key) {
    SWSS_LOG_ENTER();

    if (m_neighborTable.find(neighbor_key) == m_neighborTable.end())
      return nullptr;

    return &m_neighborTable[neighbor_key];
}

std::vector<ReturnCode> NeighborManager::createNeighbors(
    const std::vector<P4NeighborAppDbEntry>& neighbor_entries) {
  SWSS_LOG_ENTER();

  std::vector<P4NeighborEntry> entries(neighbor_entries.size());
  std::vector<sai_neighbor_entry_t> sai_entries(neighbor_entries.size());
  std::vector<std::vector<sai_attribute_t>> sai_attrs(neighbor_entries.size());
  std::vector<uint32_t> attrs_cnt(neighbor_entries.size());
  std::vector<const sai_attribute_t*> attrs_ptr(neighbor_entries.size());
  std::vector<sai_status_t> object_statuses(neighbor_entries.size());
  std::vector<ReturnCode> statuses(neighbor_entries.size());

  for (size_t i = 0; i < neighbor_entries.size(); ++i) {
    entries[i] = P4NeighborEntry(neighbor_entries[i].router_intf_id,
                                 neighbor_entries[i].neighbor_id,
                                 neighbor_entries[i].dst_mac_address);
    sai_entries[i] = prepareSaiEntry(entries[i]);
    entries[i].neigh_entry = sai_entries[i];
    sai_attrs[i] = prepareSaiAttrs(entries[i]);
    attrs_cnt[i] = static_cast<uint32_t>(sai_attrs[i].size());
    attrs_ptr[i] = sai_attrs[i].data();
  }
  sai_neighbor_api->create_neighbor_entries(
      static_cast<uint32_t>(neighbor_entries.size()), sai_entries.data(),
      attrs_cnt.data(), attrs_ptr.data(), SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR,
      object_statuses.data());

  for (size_t i = 0; i < neighbor_entries.size(); ++i) {
    CHECK_ERROR_AND_LOG(object_statuses[i],
                        "Failed to create neighbor with key "
                            << QuotedVar(entries[i].neighbor_key));

    if (object_statuses[i] == SAI_STATUS_SUCCESS) {
      m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      entries[i].router_intf_key);
      if (entries[i].neighbor_id.isV4()) {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEIGHBOR);
      } else {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEIGHBOR);
      }
      m_neighborTable[entries[i].neighbor_key] = entries[i];
      m_p4OidMapper->setDummyOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
                                 entries[i].neighbor_key);
      statuses[i] = ReturnCode();
    } else {
      statuses[i] = ReturnCode(object_statuses[i])
                    << "Failed to create neighbor with key "
                    << QuotedVar(entries[i].neighbor_key);
    }
  }
  return statuses;
}

std::vector<ReturnCode> NeighborManager::removeNeighbors(
    const std::vector<P4NeighborAppDbEntry>& neighbor_entries) {
  SWSS_LOG_ENTER();

  std::vector<P4NeighborEntry*> entries(neighbor_entries.size());
  std::vector<sai_neighbor_entry_t> sai_entries(neighbor_entries.size());
  std::vector<sai_status_t> object_statuses(neighbor_entries.size());
  std::vector<ReturnCode> statuses(neighbor_entries.size());

  for (size_t i = 0; i < neighbor_entries.size(); ++i) {
    entries[i] = getNeighborEntry(KeyGenerator::generateNeighborKey(
        neighbor_entries[i].router_intf_id, neighbor_entries[i].neighbor_id));
    sai_entries[i] = entries[i]->neigh_entry;
  }
  sai_neighbor_api->remove_neighbor_entries(
      static_cast<uint32_t>(neighbor_entries.size()), sai_entries.data(),
      SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, object_statuses.data());

  for (size_t i = 0; i < neighbor_entries.size(); ++i) {
    CHECK_ERROR_AND_LOG(object_statuses[i],
                        "Failed to create neighbor with key "
                            << QuotedVar(entries[i]->neighbor_key));

    if (object_statuses[i] == SAI_STATUS_SUCCESS) {
      m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                      entries[i]->router_intf_key);
      if (entries[i]->neighbor_id.isV4()) {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEIGHBOR);
      } else {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEIGHBOR);
      }
      m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
                              entries[i]->neighbor_key);
      m_neighborTable.erase(entries[i]->neighbor_key);
      statuses[i] = ReturnCode();
    } else {
      statuses[i] = ReturnCode(object_statuses[i])
                    << "Failed to create neighbor with key "
                    << QuotedVar(entries[i]->neighbor_key);
    }
  }
  return statuses;
}

std::vector<ReturnCode> NeighborManager::updateNeighbors(
    const std::vector<P4NeighborAppDbEntry>& neighbor_entries) {

  SWSS_LOG_ENTER();

  std::vector<P4NeighborEntry*> entries(neighbor_entries.size());
  std::vector<size_t> indice(neighbor_entries.size());
  std::vector<sai_neighbor_entry_t> sai_entries(neighbor_entries.size());
  std::vector<sai_attribute_t> sai_attr(neighbor_entries.size());
  std::vector<sai_status_t> object_statuses(neighbor_entries.size());
  std::vector<ReturnCode> statuses(neighbor_entries.size());

  size_t size = 0;
  for (size_t i = 0; i < neighbor_entries.size(); ++i) {
    entries[i] = getNeighborEntry(KeyGenerator::generateNeighborKey(
        neighbor_entries[i].router_intf_id, neighbor_entries[i].neighbor_id));
    if (!neighbor_entries[i].is_set_dst_mac ||
        entries[i]->dst_mac_address == neighbor_entries[i].dst_mac_address) {
      statuses[i] = ReturnCode();
      continue;
    }
    sai_entries[size] = entries[i]->neigh_entry;
    sai_attr[size].id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
    memcpy(sai_attr[size].value.mac,
           neighbor_entries[i].dst_mac_address.getMac(), sizeof(sai_mac_t));
    indice[size++] = i;
  }
  if (size == 0) {
    return statuses;
  }
  sai_neighbor_api->set_neighbor_entries_attribute(
      static_cast<uint32_t>(size), sai_entries.data(), sai_attr.data(),
      SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, object_statuses.data());

  for (size_t i = 0; i < size; ++i) {
    CHECK_ERROR_AND_LOG(
        object_statuses[i],
        "Failed to set mac address "
            << QuotedVar(
                   neighbor_entries[indice[i]].dst_mac_address.to_string())
            << " for neighbor with key "
            << QuotedVar(entries[indice[i]]->neighbor_key));

    if (object_statuses[i] == SAI_STATUS_SUCCESS) {
      entries[indice[i]]->dst_mac_address =
          neighbor_entries[indice[i]].dst_mac_address;
    } else {
      statuses[indice[i]] =
          ReturnCode(object_statuses[i])
          << "Failed to set mac address "
          << QuotedVar(neighbor_entries[indice[i]].dst_mac_address.to_string())
          << " for neighbor with key "
          << QuotedVar(entries[indice[i]]->neighbor_key);
    }
  }
  return statuses;
}

ReturnCode NeighborManager::processEntries(
    const std::vector<P4NeighborAppDbEntry>& entries,
    const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
    const std::string& op, bool update) {
  SWSS_LOG_ENTER();

  ReturnCode status;
  std::vector<ReturnCode> statuses;
  // In syncd, bulk SAI calls use mode SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR.
  if (op == SET_COMMAND) {
    if (!update) {
      statuses = createNeighbors(entries);
    } else {
      statuses = updateNeighbors(entries);
    }
  } else {
    statuses = removeNeighbors(entries);
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

ReturnCode NeighborManager::getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                                         std::string &object_key)
{
    std::string router_intf_id, neighbor_id;
    swss::IpAddress neighbor;

    try
    {
        nlohmann::json j = nlohmann::json::parse(json_key);
        if (j.find(prependMatchField(p4orch::kRouterInterfaceId)) != j.end())
        {
            router_intf_id = j.at(prependMatchField(p4orch::kRouterInterfaceId)).get<std::string>();
            if (j.find(prependMatchField(p4orch::kNeighborId)) != j.end())
            {
                neighbor_id = j.at(prependMatchField(p4orch::kNeighborId)).get<std::string>();
                neighbor = swss::IpAddress(neighbor_id);
                object_key = KeyGenerator::generateNeighborKey(router_intf_id, neighbor);
                object_type = SAI_OBJECT_TYPE_NEIGHBOR_ENTRY;
                return ReturnCode();
            }
            else
            {
                SWSS_LOG_ERROR("%s match parameter absent: required for dependent object query", p4orch::kNeighborId);
            }
        }
        else
        {
            SWSS_LOG_ERROR("%s match parameter absent: required for dependent object query",
                           p4orch::kRouterInterfaceId);
        }
    }
    catch (std::exception &ex)
    {
        SWSS_LOG_ERROR("json_key parse error");
    }

    return StatusCode::SWSS_RC_INVALID_PARAM;
}

void NeighborManager::enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry)
{
    m_entries.push_back(entry);
}

void NeighborManager::drainWithNotExecuted() {
  drainMgmtWithNotExecuted(m_entries, m_publisher);
}

ReturnCode NeighborManager::drain() {
  SWSS_LOG_ENTER();

  std::vector<P4NeighborAppDbEntry> entry_list;
  std::vector<swss::KeyOpFieldsValuesTuple> tuple_list;
  ReturnCode status;
  std::string prev_op;
  bool prev_update = false;
  while (!m_entries.empty()) {
    auto key_op_fvs_tuple = m_entries.front();
    m_entries.pop_front();
    std::string table_name;
    std::string db_key;
    parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &db_key);
    const std::vector<swss::FieldValueTuple>& attributes =
        kfvFieldsValues(key_op_fvs_tuple);

    auto app_db_entry_or = deserializeNeighborEntry(db_key, attributes);
    if (!app_db_entry_or.ok()) {
      status = app_db_entry_or.status();
      SWSS_LOG_ERROR("Unable to deserialize APP DB entry with key %s: %s",
                     QuotedVar(table_name + ":" + db_key).c_str(),
                     status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }
    auto& app_db_entry = *app_db_entry_or;

    const std::string neighbor_key = KeyGenerator::generateNeighborKey(
        app_db_entry.router_intf_id, app_db_entry.neighbor_id);
    const std::string& operation = kfvOp(key_op_fvs_tuple);
    bool update = (getNeighborEntry(neighbor_key) != nullptr);

    status = validateNeighborEntryOperation(app_db_entry, operation);
    if (!status.ok()) {
      SWSS_LOG_ERROR(
          "Validation failed for Neighbor APP DB entry with key %s: %s",
          QuotedVar(table_name + ":" + db_key).c_str(),
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

std::string NeighborManager::verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
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
    if (table_name != APP_P4RT_NEIGHBOR_TABLE_NAME)
    {
        return std::string("Invalid key: ") + key;
    }

    ReturnCode status;
    auto app_db_entry_or = deserializeNeighborEntry(key_content, tuple);
    if (!app_db_entry_or.ok())
    {
        status = app_db_entry_or.status();
        std::stringstream msg;
        msg << "Unable to deserialize key " << QuotedVar(key) << ": " << status.message();
        return msg.str();
    }
    auto &app_db_entry = *app_db_entry_or;

    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(app_db_entry.router_intf_id, app_db_entry.neighbor_id);
    auto *neighbor_entry = getNeighborEntry(neighbor_key);
    if (neighbor_entry == nullptr)
    {
        std::stringstream msg;
        msg << "No entry found with key " << QuotedVar(key);
        return msg.str();
    }

    std::string cache_result = verifyStateCache(app_db_entry, neighbor_entry);
    std::string asic_db_result = verifyStateAsicDb(neighbor_entry);
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

std::string NeighborManager::verifyStateCache(const P4NeighborAppDbEntry &app_db_entry,
                                              const P4NeighborEntry *neighbor_entry)
{
    const std::string neighbor_key =
        KeyGenerator::generateNeighborKey(app_db_entry.router_intf_id, app_db_entry.neighbor_id);
    ReturnCode status = validateNeighborAppDbEntry(app_db_entry);
    if (!status.ok())
    {
        std::stringstream msg;
        msg << "Validation failed for neighbor DB entry with key " << QuotedVar(neighbor_key) << ": "
            << status.message();
        return msg.str();
    }

    if (neighbor_entry->router_intf_id != app_db_entry.router_intf_id)
    {
        std::stringstream msg;
        msg << "Neighbor " << QuotedVar(neighbor_key) << " with ritf ID " << QuotedVar(app_db_entry.router_intf_id)
            << " does not match internal cache " << QuotedVar(neighbor_entry->router_intf_id)
            << " in neighbor manager.";
        return msg.str();
    }
    if (neighbor_entry->neighbor_id.to_string() != app_db_entry.neighbor_id.to_string())
    {
        std::stringstream msg;
        msg << "Neighbor " << QuotedVar(neighbor_key) << " with neighbor ID " << app_db_entry.neighbor_id.to_string()
            << " does not match internal cache " << neighbor_entry->neighbor_id.to_string() << " in neighbor manager.";
        return msg.str();
    }
    if (neighbor_entry->dst_mac_address.to_string() != app_db_entry.dst_mac_address.to_string())
    {
        std::stringstream msg;
        msg << "Neighbor " << QuotedVar(neighbor_key) << " with dest MAC " << app_db_entry.dst_mac_address.to_string()
            << " does not match internal cache " << neighbor_entry->dst_mac_address.to_string()
            << " in neighbor manager.";
        return msg.str();
    }
    if (neighbor_entry->router_intf_key != KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_intf_id))
    {
        std::stringstream msg;
        msg << "Neighbor " << QuotedVar(neighbor_key) << " does not match internal cache on ritf key "
            << QuotedVar(neighbor_entry->router_intf_key) << " in neighbor manager.";
        return msg.str();
    }
    if (neighbor_entry->neighbor_key != neighbor_key)
    {
        std::stringstream msg;
        msg << "Neighbor " << QuotedVar(neighbor_key) << " does not match internal cache on neighbor key "
            << QuotedVar(neighbor_entry->neighbor_key) << " in neighbor manager.";
        return msg.str();
    }
    return "";
}

std::string NeighborManager::verifyStateAsicDb(const P4NeighborEntry *neighbor_entry)
{
    sai_neighbor_entry_t sai_entry = prepareSaiEntry(*neighbor_entry);
    auto attrs = prepareSaiAttrs(*neighbor_entry);
    std::vector<swss::FieldValueTuple> exp = saimeta::SaiAttributeList::serialize_attr_list(
        SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, (uint32_t)attrs.size(), attrs.data(),
        /*countOnly=*/false);

    swss::DBConnector db("ASIC_DB", 0);
    swss::Table table(&db, "ASIC_STATE");
    std::string key =
        sai_serialize_object_type(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY) + ":" + sai_serialize_neighbor_entry(sai_entry);
    std::vector<swss::FieldValueTuple> values;
    if (!table.get(key, values))
    {
        return std::string("ASIC DB key not found ") + key;
    }

    return verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                       /*allow_unknown=*/false);
}
