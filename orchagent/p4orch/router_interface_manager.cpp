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

ReturnCode validateRouterInterfaceAppDbEntry(const P4RouterInterfaceAppDbEntry &app_db_entry)
{
    // Perform generic APP DB entry validations. Operation specific validations
    // will be done by the respective request process methods.

    if (app_db_entry.is_set_port_name)
    {
        Port port;
        if (!gPortsOrch->getPort(app_db_entry.port_name, port))
        {
            return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                   << "Port " << QuotedVar(app_db_entry.port_name) << " does not exist";
        }
    }

    if ((app_db_entry.is_set_src_mac) && (app_db_entry.src_mac_address.to_string() == "00:00:00:00:00:00"))
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Invalid source mac address " << QuotedVar(app_db_entry.src_mac_address.to_string());
    }

    return ReturnCode();
}

ReturnCodeOr<std::vector<sai_attribute_t>> getSaiAttrs(const P4RouterInterfaceEntry &router_intf_entry)
{
    Port port;
    if (!gPortsOrch->getPort(router_intf_entry.port_name, port))
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "Failed to get port info for port " << QuotedVar(router_intf_entry.port_name));
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
    if (router_intf_entry.src_mac_address.to_string() != "00:00:00:00:00:00")
    {
        attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
        memcpy(attr.value.mac, router_intf_entry.src_mac_address.getMac(), sizeof(sai_mac_t));
        attrs.push_back(attr);
    }

    attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    switch (port.m_type)
    {
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
        attr.value.oid = port.m_vlan_info.vlan_oid;
        break;

    default:
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Unsupported port type: " << port.m_type);
    }
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
        else if (field != p4orch::kAction && field != p4orch::kControllerMetadata)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Unexpected field " << QuotedVar(field) << " in table entry";
        }
    }

    return app_db_entry;
}

P4RouterInterfaceEntry *RouterInterfaceManager::getRouterInterfaceEntry(const std::string &router_intf_key)
{
    SWSS_LOG_ENTER();

    if (m_routerIntfTable.find(router_intf_key) == m_routerIntfTable.end())
        return nullptr;

    return &m_routerIntfTable[router_intf_key];
}

ReturnCode RouterInterfaceManager::createRouterInterface(const std::string &router_intf_key,
                                                         P4RouterInterfaceEntry &router_intf_entry)
{
    SWSS_LOG_ENTER();

    if (getRouterInterfaceEntry(router_intf_key) != nullptr)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_EXISTS)
                             << "Router interface " << QuotedVar(router_intf_entry.router_interface_id)
                             << " already exists");
    }

    if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Router interface " << QuotedVar(router_intf_key)
                                                                     << " already exists in the centralized map");
    }

    ASSIGN_OR_RETURN(std::vector<sai_attribute_t> attrs, getSaiAttrs(router_intf_entry));

    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_router_intfs_api->create_router_interface(&router_intf_entry.router_interface_oid, gSwitchId,
                                                      (uint32_t)attrs.size(), attrs.data()),
        "Failed to create router interface " << QuotedVar(router_intf_entry.router_interface_id));

    gPortsOrch->increasePortRefCount(router_intf_entry.port_name);
    gDirectory.get<VRFOrch *>()->increaseVrfRefCount(gVirtualRouterId);

    m_routerIntfTable[router_intf_key] = router_intf_entry;
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key, router_intf_entry.router_interface_oid);
    return ReturnCode();
}

ReturnCode RouterInterfaceManager::removeRouterInterface(const std::string &router_intf_key)
{
    SWSS_LOG_ENTER();

    auto *router_intf_entry = getRouterInterfaceEntry(router_intf_key);
    if (router_intf_entry == nullptr)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "Router interface entry with key " << QuotedVar(router_intf_key) << " does not exist");
    }

    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key, &ref_count))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Failed to get reference count for router interface "
                                                 << QuotedVar(router_intf_entry->router_interface_id));
    }
    if (ref_count > 0)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << "Router interface " << QuotedVar(router_intf_entry->router_interface_id)
                             << " referenced by other objects (ref_count = " << ref_count << ")");
    }

    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_router_intfs_api->remove_router_interface(router_intf_entry->router_interface_oid),
        "Failed to remove router interface " << QuotedVar(router_intf_entry->router_interface_id));

    gPortsOrch->decreasePortRefCount(router_intf_entry->port_name);
    gDirectory.get<VRFOrch *>()->decreaseVrfRefCount(gVirtualRouterId);

    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key);
    m_routerIntfTable.erase(router_intf_key);
    return ReturnCode();
}

ReturnCode RouterInterfaceManager::setSourceMacAddress(P4RouterInterfaceEntry *router_intf_entry,
                                                       const swss::MacAddress &mac_address)
{
    SWSS_LOG_ENTER();

    if (router_intf_entry->src_mac_address == mac_address)
        return ReturnCode();

    sai_attribute_t attr;
    attr.id = SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, mac_address.getMac(), sizeof(sai_mac_t));
    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_router_intfs_api->set_router_interface_attribute(router_intf_entry->router_interface_oid, &attr),
        "Failed to set mac address " << QuotedVar(mac_address.to_string()) << " on router interface "
                                     << QuotedVar(router_intf_entry->router_interface_id));

    router_intf_entry->src_mac_address = mac_address;
    return ReturnCode();
}

ReturnCode RouterInterfaceManager::processAddRequest(const P4RouterInterfaceAppDbEntry &app_db_entry,
                                                     const std::string &router_intf_key)
{
    SWSS_LOG_ENTER();

    // Perform operation specific validations.
    if (!app_db_entry.is_set_port_name)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << p4orch::kPort
                             << " is mandatory to create router interface. Failed to create "
                                "router interface "
                             << QuotedVar(app_db_entry.router_interface_id));
    }

    P4RouterInterfaceEntry router_intf_entry(app_db_entry.router_interface_id, app_db_entry.port_name,
                                             app_db_entry.src_mac_address);
    auto status = createRouterInterface(router_intf_key, router_intf_entry);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to create router interface with key %s", QuotedVar(router_intf_key).c_str());
    }

    return status;
}

ReturnCode RouterInterfaceManager::processUpdateRequest(const P4RouterInterfaceAppDbEntry &app_db_entry,
                                                        P4RouterInterfaceEntry *router_intf_entry)
{
    SWSS_LOG_ENTER();

    // TODO: port_id is a create_only parameter in SAI. In order
    // to update port name, current interface needs to be deleted and a new
    // interface with updated parameters needs to be created.
    if (app_db_entry.is_set_port_name && router_intf_entry->port_name != app_db_entry.port_name)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
                             << "Updating port name for existing router interface is not "
                                "supported. Cannot update port name to "
                             << QuotedVar(app_db_entry.port_name) << " for router interface "
                             << QuotedVar(router_intf_entry->router_interface_id));
    }

    if (app_db_entry.is_set_src_mac)
    {
        auto status = setSourceMacAddress(router_intf_entry, app_db_entry.src_mac_address);
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Failed to update source mac address with key %s",
                           QuotedVar(router_intf_entry->router_interface_id).c_str());
            return status;
        }
    }

    return ReturnCode();
}

ReturnCode RouterInterfaceManager::processDeleteRequest(const std::string &router_intf_key)
{
    SWSS_LOG_ENTER();

    auto status = removeRouterInterface(router_intf_key);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to remove router interface with key %s", QuotedVar(router_intf_key).c_str());
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

  ReturnCode status;
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

    status = validateRouterInterfaceAppDbEntry(app_db_entry);
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

    const std::string router_intf_key =
        KeyGenerator::generateRouterInterfaceKey(
            app_db_entry.router_interface_id);

    const std::string& operation = kfvOp(key_op_fvs_tuple);
    if (operation == SET_COMMAND) {
      auto* router_intf_entry = getRouterInterfaceEntry(router_intf_key);
      if (router_intf_entry == nullptr) {
        // Create router interface
        status = processAddRequest(app_db_entry, router_intf_key);
      } else {
        // Modify existing router interface
        status = processUpdateRequest(app_db_entry, router_intf_entry);
      }
    } else if (operation == DEL_COMMAND) {
      // Delete router interface
      status = processDeleteRequest(router_intf_key);
    } else {
      status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Unknown operation type " << QuotedVar(operation);
      SWSS_LOG_ERROR("%s", status.message().c_str());
    }
    m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                         kfvFieldsValues(key_op_fvs_tuple), status,
                         /*replace=*/true);
    if (!status.ok()) {
      break;
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
    return m_p4OidMapper->verifyOIDMapping(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key,
                                           router_intf_entry->router_interface_oid);
}

std::string RouterInterfaceManager::verifyStateAsicDb(const P4RouterInterfaceEntry *router_intf_entry)
{
    auto attrs_or = getSaiAttrs(*router_intf_entry);
    if (!attrs_or.ok())
    {
        return std::string("Failed to get SAI attrs: ") + attrs_or.status().message();
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

void RouterInterfaceManager::setRouterIntfsMtu(const std::string& port,
                                               uint32_t mtu) {
  // Update MTU of all router interfaces that are mapped to this port.
  SWSS_LOG_NOTICE(
      "Update MTU to %u for all router interfaces mapped to port: %s", mtu,
      port.c_str());
  for (auto& it : m_routerIntfTable) {
    if (it.second.port_name != port) {
      continue;
    }
    sai_attribute_t attr;
    attr.id = SAI_ROUTER_INTERFACE_ATTR_MTU;
    attr.value.u32 = mtu;
    sai_status_t status = sai_router_intfs_api->set_router_interface_attribute(
        it.second.router_interface_oid, &attr);
    if (status != SAI_STATUS_SUCCESS) {
      SWSS_LOG_ERROR(
          "Failed to update MTU for router interface: %s (SAI_STATUS: %s)",
          QuotedVar(it.second.router_interface_id).c_str(),
          sai_serialize_status(status).c_str());
      continue;
    }
    SWSS_LOG_NOTICE("Router interface: %s MTU updated to %u",
                    QuotedVar(it.second.router_interface_id).c_str(), mtu);
  }
}
