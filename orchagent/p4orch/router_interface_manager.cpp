#include "p4orch/router_interface_manager.h"

#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "SaiAttributeList.h"
#include "dbconnector.h"
#include "directory.h"
#include "logger.h"
#include "orch.h"
#include "p4orch/p4orch_util.h"
#include "portsorch.h"
#include "sai_serialize.h"
#include "table.h"
#include "vrforch.h"

using ::p4orch::kTableKeyDelimiter;

extern sai_object_id_t gSwitchId;
extern sai_object_id_t gVirtualRouterId;

extern sai_router_interface_api_t *sai_router_intfs_api;

extern PortsOrch *gPortsOrch;
extern Directory<Orch *> gDirectory;

namespace
{

ReturnCodeOr<std::vector<sai_attribute_t>> prepareSaiAttrs(
    const P4RouterInterfaceEntry& router_intf_entry) {
  Port port;
  if (!gPortsOrch->getPort(router_intf_entry.port_name, port)) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                         << "Failed to get port info for port "
                         << QuotedVar(router_intf_entry.port_name));
  }

  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  // Map all P4 router interfaces to default VRF as virtual router is mandatory
  // parameter for creation of router interfaces in SAI.
  attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
  attr.value.oid = gVirtualRouterId;
  attrs.push_back(attr);

  // If mac address is not set then swss::MacAddress initializes mac address
  // to 00:00:00:00:00:00.
  if (router_intf_entry.src_mac_address.to_string() != "00:00:00:00:00:00") {
    attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, router_intf_entry.src_mac_address.getMac(),
           sizeof(sai_mac_t));
    attrs.push_back(attr);
  }

  if (router_intf_entry.has_vlan_id) {
    attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_SUB_PORT;
    attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
    attr.value.oid = port.m_port_id;
    attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID;
    attr.value.u16 = router_intf_entry.vlan_id;
    attrs.push_back(attr);
  } else {
    attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    switch (port.m_type) {
      case Port::PHY:
        attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
        attrs.push_back(attr);
        attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
        attr.value.oid = port.m_port_id;
        break;
      case Port::LAG:
        attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
        attrs.push_back(attr);
        attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
        attr.value.oid = port.m_lag_id;
        break;
      case Port::VLAN:
        attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_VLAN;
        attrs.push_back(attr);
        attr.id = SAI_ROUTER_INTERFACE_ATTR_VLAN_ID;
        attr.value.oid = port.m_vlan_info.vlan_oid;
        break;
      case Port::SUBPORT:
        attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_SUB_PORT;
        attrs.push_back(attr);
        attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
        attr.value.oid = port.m_port_id;
        attrs.push_back(attr);
        attr.id = SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID;
        attr.value.u16 = port.m_vlan_info.vlan_id;
        break;

      default:
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << "Unsupported port type: " << port.m_type);
      }
      attrs.push_back(attr);
    }

    // Enable multicast.
    attr.id = SAI_ROUTER_INTERFACE_ATTR_V4_MCAST_ENABLE;
    attr.value.booldata = true;
    attrs.push_back(attr);

    attr.id = SAI_ROUTER_INTERFACE_ATTR_V6_MCAST_ENABLE;
    attr.value.booldata = true;
    attrs.push_back(attr);

    // Configure port MTU on router interface
    attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
    attr.value.u32 = port.m_mtu;
    attrs.push_back(attr);

    return attrs;
}

} // namespace

ReturnCodeOr<P4RouterInterfaceAppDbEntry> RouterInterfaceManager::deserializeRouterIntfEntry(
    const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
{
    SWSS_LOG_ENTER();

    P4RouterInterfaceAppDbEntry app_db_entry = {};
    try
    {
        nlohmann::json j = nlohmann::json::parse(key);
        app_db_entry.router_interface_id = j[prependMatchField(p4orch::kRouterInterfaceId)];
    }
    catch (std::exception &ex)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Failed to deserialize router interface id";
    }

    for (const auto &it : attributes)
    {
        const auto &field = fvField(it);
        const auto &value = fvValue(it);
        if (field == prependParamField(p4orch::kPort))
        {
            app_db_entry.port_name = value;
            app_db_entry.is_set_port_name = true;
        }
        else if (field == prependParamField(p4orch::kSrcMac))
        {
            try
            {
                app_db_entry.src_mac_address = swss::MacAddress(value);
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid MAC address " << QuotedVar(value) << " of field " << QuotedVar(field);
            }
            app_db_entry.is_set_src_mac = true;
	} 
	else if (field == prependParamField(p4orch::kVlanId)) {
            try 
	    {
                app_db_entry.vlan_id =
                static_cast<uint16_t>(std::stoul(value, 0, /*base=*/16));
	    }
            catch (std::exception& ex) 
	    {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid VLAN ID " << QuotedVar(value) << " of field "
                       << QuotedVar(field);
            }
            app_db_entry.is_set_vlan_id = true;
        }
        else if (field != p4orch::kAction && field != p4orch::kControllerMetadata)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Unexpected field " << QuotedVar(field) << " in table entry";
        }
    }

    return app_db_entry;
}

ReturnCode RouterInterfaceManager::validateRouterInterfaceAppDbEntry(
    const P4RouterInterfaceAppDbEntry& app_db_entry) {
  // Perform generic APP DB entry validations. Operation specific validations
  // will be done by the respective request process methods.

  if (app_db_entry.is_set_port_name) {
    Port port;
    // Set fail_on_lock to reject the P4 request in case the port is locked.
    // It doesn't need to be set in subsequent getPort() requests since the
    // lock status won't change as Orchagent is single threaded.
    if (!gPortsOrch->getPort(app_db_entry.port_name, port)) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Port " << QuotedVar(app_db_entry.port_name)
             << " does not exist";
    }
  }

  if ((app_db_entry.is_set_src_mac) &&
      (app_db_entry.src_mac_address.to_string() == "00:00:00:00:00:00")) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Invalid source mac address "
           << QuotedVar(app_db_entry.src_mac_address.to_string());
  }

  // If VLAN is going to be set, port and src mac also have to be set.
  if (app_db_entry.is_set_vlan_id) {
    if (!app_db_entry.is_set_port_name || !app_db_entry.is_set_src_mac) {
      return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
             << "To set a VLAN, a port and source mac address must be provided";
    }
  }

  return ReturnCode();
}

ReturnCode RouterInterfaceManager::validateRouterInterfaceEntryOperation(
    const P4RouterInterfaceAppDbEntry& app_db_entry,
    const std::string& operation) {
  SWSS_LOG_ENTER();

  RETURN_IF_ERROR(validateRouterInterfaceAppDbEntry(app_db_entry));
  const std::string router_intf_key = KeyGenerator::generateRouterInterfaceKey(
      app_db_entry.router_interface_id);
  auto* router_intf_entry = getRouterInterfaceEntry(router_intf_key);
  if (operation == SET_COMMAND) {
    if (router_intf_entry == nullptr) {
      if (!app_db_entry.is_set_port_name) {
        LOG_ERROR_AND_RETURN(
            ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
            << p4orch::kPort
            << " is mandatory to create router interface. Failed to create "
               "router interface "
            << QuotedVar(app_db_entry.router_interface_id));
      }
      if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                   router_intf_key)) {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
            "Router interface " << QuotedVar(router_intf_key)
                                << " already exists in the centralized map");
      }
    } else {
      // TODO: port_id is a create_only parameter in SAI. In order
      // to update port name, current interface needs to be deleted and a new
      // interface with updated parameters needs to be created.
      if (app_db_entry.is_set_port_name &&
          router_intf_entry->port_name != app_db_entry.port_name) {
        LOG_ERROR_AND_RETURN(
            ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
            << "Updating port name for existing router interface is not "
               "supported. Cannot update port name to "
            << QuotedVar(app_db_entry.port_name) << " for router interface "
            << QuotedVar(router_intf_entry->router_interface_id));
      }

      // Similar to port ID above, VLAN ID is a CREATE_ONLY parameter in SAI.
      if (app_db_entry.is_set_vlan_id &&
          router_intf_entry->vlan_id != app_db_entry.vlan_id) {
        LOG_ERROR_AND_RETURN(
            ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
            << "Updating VLAN ID for existing router interface is not "
               "supported. Cannot update VLAN ID from "
            << QuotedVar(std::to_string(router_intf_entry->vlan_id)) << " to "
            << QuotedVar(std::to_string(app_db_entry.vlan_id))
            << " for router interface "
            << QuotedVar(router_intf_entry->router_interface_id));
      }
    }
  } else if (operation == DEL_COMMAND) {
    if (router_intf_entry == nullptr) {
      LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                           << "Router interface entry with key "
                           << QuotedVar(router_intf_key) << " does not exist");
    }
    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                    router_intf_key, &ref_count)) {
      RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
          "Failed to get reference count for router interface "
          << QuotedVar(router_intf_entry->router_interface_id));
    }
    if (ref_count > 0) {
      LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                           << "Router interface "
                           << QuotedVar(router_intf_entry->router_interface_id)
                           << " referenced by other objects (ref_count = "
                           << ref_count << ")");
    }
  } else {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Unknown operation type " << QuotedVar(operation);
  }

  return ReturnCode();
}

P4RouterInterfaceEntry* RouterInterfaceManager::getRouterInterfaceEntry(
    const std::string& router_intf_key) {
  SWSS_LOG_ENTER();

  if (m_routerIntfTable.find(router_intf_key) == m_routerIntfTable.end())
    return nullptr;

  return &m_routerIntfTable[router_intf_key];
}

std::vector<ReturnCode> RouterInterfaceManager::createRouterInterfaces(
    const std::vector<P4RouterInterfaceAppDbEntry>& router_intf_entries) {
  SWSS_LOG_ENTER();

  std::vector<P4RouterInterfaceEntry> entries(router_intf_entries.size());
  std::vector<std::vector<sai_attribute_t>> sai_attrs(
      router_intf_entries.size());
  std::vector<uint32_t> attrs_cnt(router_intf_entries.size());
  std::vector<const sai_attribute_t*> attrs_ptr(router_intf_entries.size());
  std::vector<sai_object_id_t> oids(router_intf_entries.size());
  std::vector<sai_status_t> object_statuses(router_intf_entries.size());
  std::vector<ReturnCode> statuses(
      router_intf_entries.size(), ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED));

  for (size_t i = 0; i < router_intf_entries.size(); ++i) {
    entries[i] = P4RouterInterfaceEntry(
        router_intf_entries[i].router_interface_id,
        router_intf_entries[i].port_name,
        router_intf_entries[i].src_mac_address, router_intf_entries[i].vlan_id,
        router_intf_entries[i].is_set_vlan_id);
    auto attrs = prepareSaiAttrs(entries[i]);
    if (!attrs.ok()) {
      statuses[i] = ReturnCode(attrs.status());
      return statuses;
    }
    sai_attrs[i] = *attrs;
    attrs_cnt[i] = static_cast<uint32_t>(sai_attrs[i].size());
    attrs_ptr[i] = sai_attrs[i].data();
  }
  sai_router_intfs_api->create_router_interfaces(
      gSwitchId, static_cast<uint32_t>(router_intf_entries.size()),
      attrs_cnt.data(), attrs_ptr.data(), SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR,
      oids.data(), object_statuses.data());

  for (size_t i = 0; i < router_intf_entries.size(); ++i) {
    CHECK_ERROR_AND_LOG(object_statuses[i],
                        "Failed to create router interface "
                            << QuotedVar(entries[i].router_interface_id));

    if (object_statuses[i] == SAI_STATUS_SUCCESS) {
      entries[i].router_interface_oid = oids[i];
      gPortsOrch->increasePortRefCount(entries[i].port_name);
      gDirectory.get<VRFOrch*>()->increaseVrfRefCount(gVirtualRouterId);
      const std::string router_intf_key =
          KeyGenerator::generateRouterInterfaceKey(
              entries[i].router_interface_id);
      m_routerIntfTable[router_intf_key] = entries[i];
      m_p4OidMapper->setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key,
                            entries[i].router_interface_oid);
      statuses[i] = ReturnCode();
    } else {
      statuses[i] = ReturnCode(object_statuses[i])
                    << "Failed to create router interface "
                    << QuotedVar(entries[i].router_interface_id);
    }
  }

  return statuses;
}

std::vector<ReturnCode> RouterInterfaceManager::removeRouterInterfaces(
    const std::vector<P4RouterInterfaceAppDbEntry>& router_intf_entries) {
  SWSS_LOG_ENTER();

  std::vector<std::string> router_intf_keys(router_intf_entries.size());
  std::vector<P4RouterInterfaceEntry*> entries(router_intf_entries.size());
  std::vector<sai_object_id_t> oids(router_intf_entries.size());
  std::vector<sai_status_t> object_statuses(router_intf_entries.size());
  std::vector<ReturnCode> statuses(router_intf_entries.size());

  for (size_t i = 0; i < router_intf_entries.size(); ++i) {
    router_intf_keys[i] = KeyGenerator::generateRouterInterfaceKey(
        router_intf_entries[i].router_interface_id);
    entries[i] = getRouterInterfaceEntry(router_intf_keys[i]);
    oids[i] = entries[i]->router_interface_oid;
  }
  sai_router_intfs_api->remove_router_interfaces(
      static_cast<uint32_t>(router_intf_entries.size()), oids.data(),
      SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, object_statuses.data());

  for (size_t i = 0; i < router_intf_entries.size(); ++i) {
    CHECK_ERROR_AND_LOG(object_statuses[i],
                        "Failed to remove router interface "
                            << QuotedVar(entries[i]->router_interface_id));

    if (object_statuses[i] == SAI_STATUS_SUCCESS) {
      gPortsOrch->decreasePortRefCount(entries[i]->port_name);
      gDirectory.get<VRFOrch*>()->decreaseVrfRefCount(gVirtualRouterId);
      m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                              router_intf_keys[i]);
      m_routerIntfTable.erase(router_intf_keys[i]);
      statuses[i] = ReturnCode();
    } else {
      statuses[i] = ReturnCode(object_statuses[i])
                    << "Failed to remove router interface "
                    << QuotedVar(entries[i]->router_interface_id);
    }
  }

  return statuses;
}

std::vector<ReturnCode> RouterInterfaceManager::updateRouterInterfaces(
    const std::vector<P4RouterInterfaceAppDbEntry>& router_intf_entries) {
  SWSS_LOG_ENTER();

  // Currently, we only support update of MAC address.
  // The max number of attributes updated will be the same as the number of
  // entries.
  std::vector<P4RouterInterfaceEntry*> entries(router_intf_entries.size());
  std::vector<size_t> indice(router_intf_entries.size());
  std::vector<sai_object_id_t> oids(router_intf_entries.size());
  std::vector<sai_attribute_t> sai_attr(router_intf_entries.size());
  std::vector<sai_status_t> object_statuses(router_intf_entries.size());
  std::vector<ReturnCode> statuses(router_intf_entries.size());

  size_t size = 0;
  for (size_t i = 0; i < router_intf_entries.size(); ++i) {
    entries[i] =
        getRouterInterfaceEntry(KeyGenerator::generateRouterInterfaceKey(
            router_intf_entries[i].router_interface_id));
    statuses[i] = ReturnCode();
    if (!router_intf_entries[i].is_set_src_mac ||
        entries[i]->src_mac_address == router_intf_entries[i].src_mac_address) {
      continue;
    }
    oids[size] = entries[i]->router_interface_oid;
    sai_attr[size].id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
    memcpy(sai_attr[size].value.mac,
           router_intf_entries[i].src_mac_address.getMac(), sizeof(sai_mac_t));
    indice[size++] = i;
  }
  if (size == 0) {
    return statuses;
  }
  sai_router_intfs_api->set_router_interfaces_attribute(
      static_cast<uint32_t>(size), oids.data(), sai_attr.data(),
      SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, object_statuses.data());

  for (size_t i = 0; i < size; ++i) {
    CHECK_ERROR_AND_LOG(
        object_statuses[i],
        "Failed to set mac address "
            << QuotedVar(
                   router_intf_entries[indice[i]].src_mac_address.to_string())
            << " on router interface "
            << QuotedVar(entries[indice[i]]->router_interface_id));

    if (object_statuses[i] == SAI_STATUS_SUCCESS) {
      entries[indice[i]]->src_mac_address =
          router_intf_entries[indice[i]].src_mac_address;
    } else {
      statuses[indice[i]] =
          ReturnCode(object_statuses[i])
          << "Failed to set mac address "
          << QuotedVar(
                 router_intf_entries[indice[i]].src_mac_address.to_string())
          << " on router interface "
          << QuotedVar(entries[indice[i]]->router_interface_id);
    }
  }

  return statuses;
}

ReturnCode RouterInterfaceManager::processEntries(
    const std::vector<P4RouterInterfaceAppDbEntry>& entries,
    const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
    const std::string& op, bool update) {
  SWSS_LOG_ENTER();

  ReturnCode status;
  std::vector<ReturnCode> statuses;
  // In syncd, bulk SAI calls use mode SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR.
  if (op == SET_COMMAND) {
    if (!update) {
      statuses = createRouterInterfaces(entries);
    } else {
      statuses = updateRouterInterfaces(entries);
    }
  } else {
    statuses = removeRouterInterfaces(entries);
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

ReturnCode RouterInterfaceManager::getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                                                std::string &object_key)
{
    std::string value;

    try
    {
        nlohmann::json j = nlohmann::json::parse(json_key);
        if (j.find(prependMatchField(p4orch::kRouterInterfaceId)) != j.end())
        {
            value = j.at(prependMatchField(p4orch::kRouterInterfaceId)).get<std::string>();
            object_key = KeyGenerator::generateRouterInterfaceKey(value);
            object_type = SAI_OBJECT_TYPE_ROUTER_INTERFACE;
            return ReturnCode();
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

void RouterInterfaceManager::enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry)
{
    m_entries.push_back(entry);
}

void RouterInterfaceManager::drainWithNotExecuted() {
  drainMgmtWithNotExecuted(m_entries, m_publisher);
}

ReturnCode RouterInterfaceManager::drain() {
  SWSS_LOG_ENTER();
  
  std::vector<P4RouterInterfaceAppDbEntry> entry_list;
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

    auto app_db_entry_or = deserializeRouterIntfEntry(db_key, attributes);
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

    const std::string router_intf_key =
        KeyGenerator::generateRouterInterfaceKey(
            app_db_entry.router_interface_id);
    const std::string& operation = kfvOp(key_op_fvs_tuple);
    bool update = (getRouterInterfaceEntry(router_intf_key) != nullptr);

    status = validateRouterInterfaceEntryOperation(app_db_entry, operation);
    if (!status.ok()) {
      SWSS_LOG_ERROR(
          "Validation failed for Router Interface APP DB entry with key %s: %s",
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

std::string RouterInterfaceManager::verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
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
    if (table_name != APP_P4RT_ROUTER_INTERFACE_TABLE_NAME)
    {
        return std::string("Invalid key: ") + key;
    }

    auto app_db_entry_or = deserializeRouterIntfEntry(key_content, tuple);
    if (!app_db_entry_or.ok())
    {
        ReturnCode status = app_db_entry_or.status();
        std::stringstream msg;
        msg << "Unable to deserialize key " << QuotedVar(key) << ": " << status.message();
        return msg.str();
    }
    auto &app_db_entry = *app_db_entry_or;

    const std::string router_intf_key = KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_interface_id);
    auto *router_intf_entry = getRouterInterfaceEntry(router_intf_key);
    if (router_intf_entry == nullptr)
    {
        std::stringstream msg;
        msg << "No entry found with key " << QuotedVar(key);
        return msg.str();
    }

    std::string cache_result = verifyStateCache(app_db_entry, router_intf_entry);
    std::string asic_db_result = verifyStateAsicDb(router_intf_entry);
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

std::string RouterInterfaceManager::verifyStateCache(const P4RouterInterfaceAppDbEntry &app_db_entry,
                                                     const P4RouterInterfaceEntry *router_intf_entry)
{
    const std::string router_intf_key = KeyGenerator::generateRouterInterfaceKey(app_db_entry.router_interface_id);
    ReturnCode status = validateRouterInterfaceAppDbEntry(app_db_entry);
    if (!status.ok())
    {
        std::stringstream msg;
        msg << "Validation failed for Router Interface DB entry with key " << QuotedVar(router_intf_key) << ": "
            << status.message();
        return msg.str();
    }
    if (router_intf_entry->router_interface_id != app_db_entry.router_interface_id)
    {
        std::stringstream msg;
        msg << "Router interface ID " << QuotedVar(app_db_entry.router_interface_id)
            << " does not match internal cache " << QuotedVar(router_intf_entry->router_interface_id)
            << " in router interface manager.";
        return msg.str();
    }
    if (router_intf_entry->port_name != app_db_entry.port_name)
    {
        std::stringstream msg;
        msg << "Port name " << QuotedVar(app_db_entry.port_name) << " does not match internal cache "
            << QuotedVar(router_intf_entry->port_name) << " in router interface manager.";
        return msg.str();
    }
    if (router_intf_entry->src_mac_address.to_string() != app_db_entry.src_mac_address.to_string())
    {
        std::stringstream msg;
        msg << "Source MAC address " << app_db_entry.src_mac_address.to_string() << " does not match internal cache "
            << router_intf_entry->src_mac_address.to_string() << " in router interface manager.";
        return msg.str();
    }
    if (router_intf_entry->has_vlan_id != app_db_entry.is_set_vlan_id) {
      std::stringstream msg;
      msg << "Setting a VLAN ID " << app_db_entry.is_set_vlan_id
          << " does not match internal cache " << router_intf_entry->has_vlan_id
          << " in router interface manager.";
      return msg.str();
    }
    if (router_intf_entry->vlan_id != app_db_entry.vlan_id) {
      std::stringstream msg;
      msg << "VLAN ID " << app_db_entry.vlan_id
          << " does not match internal cache " << router_intf_entry->vlan_id
          << " in router interface manager.";
      return msg.str();
    }
    return m_p4OidMapper->verifyOIDMapping(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key,
                                           router_intf_entry->router_interface_oid);
}

std::string RouterInterfaceManager::verifyStateAsicDb(const P4RouterInterfaceEntry *router_intf_entry)
{
  auto attrs_or = prepareSaiAttrs(*router_intf_entry);
  if (!attrs_or.ok()) {
    return std::string("Failed to get SAI attrs: ") +
           attrs_or.status().message();
  }
    std::vector<sai_attribute_t> attrs = *attrs_or;
    std::vector<swss::FieldValueTuple> exp = saimeta::SaiAttributeList::serialize_attr_list(
        SAI_OBJECT_TYPE_ROUTER_INTERFACE, (uint32_t)attrs.size(), attrs.data(), /*countOnly=*/false);

    swss::DBConnector db("ASIC_DB", 0);
    swss::Table table(&db, "ASIC_STATE");
    std::string key = sai_serialize_object_type(SAI_OBJECT_TYPE_ROUTER_INTERFACE) + ":" +
                      sai_serialize_object_id(router_intf_entry->router_interface_oid);
    std::vector<swss::FieldValueTuple> values;
    if (!table.get(key, values))
    {
        return std::string("ASIC DB key not found ") + key;
    }

    return verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                       /*allow_unknown=*/false);
}

void RouterInterfaceManager::setRouterIntfsMtu(const std::string &port, uint32_t mtu)
{
    // Update MTU of all router interfaces that are mapped to this port.
    SWSS_LOG_NOTICE("Update MTU to %u for all router interfaces mapped to port: %s", mtu, port.c_str());
    for (auto &it : m_routerIntfTable)
    {
        if (it.second.port_name != port)
        {
            continue;
        }
        sai_attribute_t attr;
        attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
        attr.value.u32 = mtu;
        sai_status_t status =
            sai_router_intfs_api->set_router_interface_attribute(it.second.router_interface_oid, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to update MTU for router interface: %s (SAI_STATUS: %s)",
                           QuotedVar(it.second.router_interface_id).c_str(), sai_serialize_status(status).c_str());
            continue;
        }
        SWSS_LOG_NOTICE("Router interface: %s MTU updated to %u", QuotedVar(it.second.router_interface_id).c_str(),
                        mtu);
    }
}
