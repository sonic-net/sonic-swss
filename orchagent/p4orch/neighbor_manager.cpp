#include "p4orch/neighbor_manager.h"

#include <sstream>
#include <string>
#include <vector>

#include "crmorch.h"
#include "json.hpp"
#include "logger.h"
#include "orch.h"
#include "p4orch/p4orch_util.h"
#include "swssnet.h"
extern "C"
{
#include "sai.h"
}

extern sai_object_id_t gSwitchId;

extern sai_neighbor_api_t *sai_neighbor_api;

extern CrmOrch *gCrmOrch;

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
    // Perform generic APP DB entry validations. Operation specific validations
    // will be done by the respective request process methods.

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

P4NeighborEntry *NeighborManager::getNeighborEntry(const std::string &neighbor_key)
{
    SWSS_LOG_ENTER();

    if (m_neighborTable.find(neighbor_key) == m_neighborTable.end())
        return nullptr;

    return &m_neighborTable[neighbor_key];
}

ReturnCode NeighborManager::createNeighbor(P4NeighborEntry &neighbor_entry)
{
    SWSS_LOG_ENTER();

    const std::string &neighbor_key = neighbor_entry.neighbor_key;
    if (getNeighborEntry(neighbor_key) != nullptr)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_EXISTS)
                             << "Neighbor entry with key " << QuotedVar(neighbor_key) << " already exists");
    }

    if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Neighbor entry with key " << QuotedVar(neighbor_key)
                                                                            << " already exists in centralized map");
    }

    const std::string &router_intf_key = neighbor_entry.router_intf_key;
    sai_object_id_t router_intf_oid;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key, &router_intf_oid))
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "Router intf key " << QuotedVar(router_intf_key)
                             << " does not exist in certralized map");
    }

    neighbor_entry.neigh_entry.switch_id = gSwitchId;
    copy(neighbor_entry.neigh_entry.ip_address, neighbor_entry.neighbor_id);
    neighbor_entry.neigh_entry.rif_id = router_intf_oid;

    std::vector<sai_attribute_t> neigh_attrs;
    sai_attribute_t neigh_attr;
    neigh_attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
    memcpy(neigh_attr.value.mac, neighbor_entry.dst_mac_address.getMac(), sizeof(sai_mac_t));
    neigh_attrs.push_back(neigh_attr);

    // Do not program host route.
    // This is mainly for neighbor with IPv6 link-local addresses.
    neigh_attr.id = SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE;
    neigh_attr.value.booldata = true;
    neigh_attrs.push_back(neigh_attr);

    CHECK_ERROR_AND_LOG_AND_RETURN(sai_neighbor_api->create_neighbor_entry(&neighbor_entry.neigh_entry,
                                                                           static_cast<uint32_t>(neigh_attrs.size()),
                                                                           neigh_attrs.data()),
                                   "Failed to create neighbor with key " << QuotedVar(neighbor_key));

    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_intf_key);
    if (neighbor_entry.neighbor_id.isV4())
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEIGHBOR);
    }
    else
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEIGHBOR);
    }

    m_neighborTable[neighbor_key] = neighbor_entry;
    m_p4OidMapper->setDummyOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key);
    return ReturnCode();
}

ReturnCode NeighborManager::removeNeighbor(const std::string &neighbor_key)
{
    SWSS_LOG_ENTER();

    auto *neighbor_entry = getNeighborEntry(neighbor_key);
    if (neighbor_entry == nullptr)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "Neighbor with key " << QuotedVar(neighbor_key) << " does not exist");
    }

    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key, &ref_count))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Failed to get reference count of neighbor with key "
                                                 << QuotedVar(neighbor_key));
    }
    if (ref_count > 0)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << "Neighbor with key " << QuotedVar(neighbor_key)
                             << " referenced by other objects (ref_count = " << ref_count << ")");
    }

    CHECK_ERROR_AND_LOG_AND_RETURN(sai_neighbor_api->remove_neighbor_entry(&neighbor_entry->neigh_entry),
                                   "Failed to remove neighbor with key " << QuotedVar(neighbor_key));

    m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE, neighbor_entry->router_intf_key);
    if (neighbor_entry->neighbor_id.isV4())
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEIGHBOR);
    }
    else
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEIGHBOR);
    }

    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key);
    m_neighborTable.erase(neighbor_key);
    return ReturnCode();
}

ReturnCode NeighborManager::setDstMacAddress(P4NeighborEntry *neighbor_entry, const swss::MacAddress &mac_address)
{
    SWSS_LOG_ENTER();

    if (neighbor_entry->dst_mac_address == mac_address)
        return ReturnCode();

    sai_attribute_t neigh_attr;
    neigh_attr.id = SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS;
    memcpy(neigh_attr.value.mac, mac_address.getMac(), sizeof(sai_mac_t));
    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_neighbor_api->set_neighbor_entry_attribute(&neighbor_entry->neigh_entry, &neigh_attr),
        "Failed to set mac address " << QuotedVar(mac_address.to_string()) << " for neighbor with key "
                                     << QuotedVar(neighbor_entry->neighbor_key));

    neighbor_entry->dst_mac_address = mac_address;
    return ReturnCode();
}

ReturnCode NeighborManager::processAddRequest(const P4NeighborAppDbEntry &app_db_entry, const std::string &neighbor_key)
{
    SWSS_LOG_ENTER();

    // Perform operation specific validations.
    if (!app_db_entry.is_set_dst_mac)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << p4orch::kDstMac
                             << " is mandatory to create neighbor entry. Failed to create "
                                "neighbor with key "
                             << QuotedVar(neighbor_key));
    }

    P4NeighborEntry neighbor_entry(app_db_entry.router_intf_id, app_db_entry.neighbor_id, app_db_entry.dst_mac_address);
    auto status = createNeighbor(neighbor_entry);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to create neighbor with key %s", QuotedVar(neighbor_key).c_str());
    }

    return status;
}

ReturnCode NeighborManager::processUpdateRequest(const P4NeighborAppDbEntry &app_db_entry,
                                                 P4NeighborEntry *neighbor_entry)
{
    SWSS_LOG_ENTER();

    if (app_db_entry.is_set_dst_mac)
    {
        auto status = setDstMacAddress(neighbor_entry, app_db_entry.dst_mac_address);
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Failed to set destination mac address for neighbor with key %s",
                           QuotedVar(neighbor_entry->neighbor_key).c_str());
            return status;
        }
    }

    return ReturnCode();
}

ReturnCode NeighborManager::processDeleteRequest(const std::string &neighbor_key)
{
    SWSS_LOG_ENTER();

    auto status = removeNeighbor(neighbor_key);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to remove neighbor with key %s", QuotedVar(neighbor_key).c_str());
    }

    return status;
}

void NeighborManager::enqueue(const swss::KeyOpFieldsValuesTuple &entry)
{
    m_entries.push_back(entry);
}

void NeighborManager::drain()
{
    SWSS_LOG_ENTER();

    for (const auto &key_op_fvs_tuple : m_entries)
    {
        std::string table_name;
        std::string db_key;
        parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &db_key);
        const std::vector<swss::FieldValueTuple> &attributes = kfvFieldsValues(key_op_fvs_tuple);

        ReturnCode status;
        auto app_db_entry_or = deserializeNeighborEntry(db_key, attributes);
        if (!app_db_entry_or.ok())
        {
            status = app_db_entry_or.status();
            SWSS_LOG_ERROR("Unable to deserialize APP DB entry with key %s: %s",
                           QuotedVar(table_name + ":" + db_key).c_str(), status.message().c_str());
            m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                 status,
                                 /*replace=*/true);
            continue;
        }
        auto &app_db_entry = *app_db_entry_or;

        status = validateNeighborAppDbEntry(app_db_entry);
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Validation failed for Neighbor APP DB entry with key %s: %s",
                           QuotedVar(table_name + ":" + db_key).c_str(), status.message().c_str());
            m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                 status,
                                 /*replace=*/true);
            continue;
        }

        const std::string neighbor_key =
            KeyGenerator::generateNeighborKey(app_db_entry.router_intf_id, app_db_entry.neighbor_id);

        const std::string &operation = kfvOp(key_op_fvs_tuple);
        if (operation == SET_COMMAND)
        {
            auto *neighbor_entry = getNeighborEntry(neighbor_key);
            if (neighbor_entry == nullptr)
            {
                // Create neighbor
                status = processAddRequest(app_db_entry, neighbor_key);
            }
            else
            {
                // Modify existing neighbor
                status = processUpdateRequest(app_db_entry, neighbor_entry);
            }
        }
        else if (operation == DEL_COMMAND)
        {
            // Delete neighbor
            status = processDeleteRequest(neighbor_key);
        }
        else
        {
            status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Unknown operation type " << QuotedVar(operation);
            SWSS_LOG_ERROR("%s", status.message().c_str());
        }
        m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple), status,
                             /*replace=*/true);
    }
    m_entries.clear();
}
