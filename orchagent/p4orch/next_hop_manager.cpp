#include "p4orch/next_hop_manager.h"

#include <sstream>
#include <string>
#include <vector>

#include "crmorch.h"
#include "ipaddress.h"
#include "json.hpp"
#include "logger.h"
#include "p4orch/p4orch_util.h"
#include "swssnet.h"
extern "C"
{
#include "sai.h"
}

extern sai_object_id_t gSwitchId;
extern sai_next_hop_api_t *sai_next_hop_api;
extern CrmOrch *gCrmOrch;

P4NextHopEntry::P4NextHopEntry(const std::string &next_hop_id, const std::string &router_interface_id,
                               const swss::IpAddress &neighbor_id)
    : next_hop_id(next_hop_id), router_interface_id(router_interface_id), neighbor_id(neighbor_id)
{
    SWSS_LOG_ENTER();
    next_hop_key = KeyGenerator::generateNextHopKey(next_hop_id);
}

void NextHopManager::enqueue(const swss::KeyOpFieldsValuesTuple &entry)
{
    m_entries.push_back(entry);
}

void NextHopManager::drain()
{
    SWSS_LOG_ENTER();

    for (const auto &key_op_fvs_tuple : m_entries)
    {
        std::string table_name;
        std::string key;
        parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &key);
        const std::vector<swss::FieldValueTuple> &attributes = kfvFieldsValues(key_op_fvs_tuple);

        ReturnCode status;
        auto app_db_entry_or = deserializeP4NextHopAppDbEntry(key, attributes);
        if (!app_db_entry_or.ok())
        {
            status = app_db_entry_or.status();
            SWSS_LOG_ERROR("Unable to deserialize APP DB entry with key %s: %s",
                           QuotedVar(table_name + ":" + key).c_str(), status.message().c_str());
            m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                 status,
                                 /*replace=*/true);
            continue;
        }
        auto &app_db_entry = *app_db_entry_or;

        const std::string next_hop_key = KeyGenerator::generateNextHopKey(app_db_entry.next_hop_id);

        // Fulfill the operation.
        const std::string &operation = kfvOp(key_op_fvs_tuple);
        if (operation == SET_COMMAND)
        {
            auto *next_hop_entry = getNextHopEntry(next_hop_key);
            if (next_hop_entry == nullptr)
            {
                // Create new next hop.
                status = processAddRequest(app_db_entry);
            }
            else
            {
                // Modify existing next hop.
                status = processUpdateRequest(app_db_entry, next_hop_entry);
            }
        }
        else if (operation == DEL_COMMAND)
        {
            // Delete next hop.
            status = processDeleteRequest(next_hop_key);
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
            app_db_entry.is_set_router_interface_id = true;
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
            app_db_entry.is_set_neighbor_id = true;
        }
        else if (field != p4orch::kAction && field != p4orch::kControllerMetadata)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Unexpected field " << QuotedVar(field) << " in table entry";
        }
    }

    return app_db_entry;
}

ReturnCode NextHopManager::processAddRequest(const P4NextHopAppDbEntry &app_db_entry)
{
    SWSS_LOG_ENTER();

    P4NextHopEntry next_hop_entry(app_db_entry.next_hop_id, app_db_entry.router_interface_id, app_db_entry.neighbor_id);
    auto status = createNextHop(next_hop_entry);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to create next hop with key %s", QuotedVar(next_hop_entry.next_hop_key).c_str());
    }
    return status;
}

ReturnCode NextHopManager::createNextHop(P4NextHopEntry &next_hop_entry)
{
    SWSS_LOG_ENTER();

    // Check the existence of the next hop in next hop manager and centralized
    // mapper.
    if (getNextHopEntry(next_hop_entry.next_hop_key) != nullptr)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_EXISTS)
                             << "Next hop with key " << QuotedVar(next_hop_entry.next_hop_key)
                             << " already exists in next hop manager");
    }
    if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_entry.next_hop_key))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Next hop with key " << QuotedVar(next_hop_entry.next_hop_key)
                                                                      << " already exists in centralized mapper");
    }

    // From centralized mapper, get OID of router interface that next hop depends
    // on.
    const auto router_interface_key = KeyGenerator::generateRouterInterfaceKey(next_hop_entry.router_interface_id);
    sai_object_id_t rif_oid;
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_interface_key, &rif_oid))
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "Router intf " << QuotedVar(next_hop_entry.router_interface_id) << " does not exist");
    }

    // Neighbor doesn't have OID and the IP addr needed in next hop creation is
    // neighbor_id, so only check neighbor existence in centralized mapper.
    const auto neighbor_key =
        KeyGenerator::generateNeighborKey(next_hop_entry.router_interface_id, next_hop_entry.neighbor_id);
    if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key))
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "Neighbor with key " << QuotedVar(neighbor_key)
                             << " does not exist in centralized mapper");
    }

    // Prepare attributes for the SAI creation call.
    std::vector<sai_attribute_t> next_hop_attrs;
    sai_attribute_t next_hop_attr;

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
    next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_IP;
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_IP;
    swss::copy(next_hop_attr.value.ipaddr, next_hop_entry.neighbor_id);
    next_hop_attrs.push_back(next_hop_attr);

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
    next_hop_attr.value.oid = rif_oid;
    next_hop_attrs.push_back(next_hop_attr);

    // Call SAI API.
    CHECK_ERROR_AND_LOG_AND_RETURN(sai_next_hop_api->create_next_hop(&next_hop_entry.next_hop_oid, gSwitchId,
                                                                     (uint32_t)next_hop_attrs.size(),
                                                                     next_hop_attrs.data()),
                                   "Failed to create next hop " << QuotedVar(next_hop_entry.next_hop_key) << " on rif "
                                                                << QuotedVar(next_hop_entry.router_interface_id));

    // On successful creation, increment ref count.
    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_interface_key);
    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEIGHBOR_ENTRY, neighbor_key);
    if (next_hop_entry.neighbor_id.isV4())
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEXTHOP);
    }
    else
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEXTHOP);
    }

    // Add created entry to internal table.
    m_nextHopTable.emplace(next_hop_entry.next_hop_key, next_hop_entry);

    // Add the key to OID map to centralized mapper.
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_entry.next_hop_key, next_hop_entry.next_hop_oid);

    return ReturnCode();
}

ReturnCode NextHopManager::processUpdateRequest(const P4NextHopAppDbEntry &app_db_entry, P4NextHopEntry *next_hop_entry)
{
    SWSS_LOG_ENTER();

    ReturnCode status = ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
                        << "Currently next hop doesn't support update. Next hop key "
                        << QuotedVar(next_hop_entry->next_hop_key);
    SWSS_LOG_ERROR("%s", status.message().c_str());
    return status;
}

ReturnCode NextHopManager::processDeleteRequest(const std::string &next_hop_key)
{
    SWSS_LOG_ENTER();

    auto status = removeNextHop(next_hop_key);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to remove next hop with key %s", QuotedVar(next_hop_key).c_str());
    }

    return status;
}

ReturnCode NextHopManager::removeNextHop(const std::string &next_hop_key)
{
    SWSS_LOG_ENTER();

    // Check the existence of the next hop in next hop manager and centralized
    // mapper.
    auto *next_hop_entry = getNextHopEntry(next_hop_key);
    if (next_hop_entry == nullptr)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "Next hop with key " << QuotedVar(next_hop_key)
                             << " does not exist in next hop manager");
    }

    // Check if there is anything referring to the next hop before deletion.
    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key, &ref_count))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Failed to get reference count for next hop "
                                                 << QuotedVar(next_hop_key));
    }
    if (ref_count > 0)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << "Next hop " << QuotedVar(next_hop_entry->next_hop_key)
                             << " referenced by other objects (ref_count = " << ref_count);
    }

    // Call SAI API.
    CHECK_ERROR_AND_LOG_AND_RETURN(sai_next_hop_api->remove_next_hop(next_hop_entry->next_hop_oid),
                                   "Failed to remove next hop " << QuotedVar(next_hop_entry->next_hop_key));

    // On successful deletion, decrement ref count.
    m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                    KeyGenerator::generateRouterInterfaceKey(next_hop_entry->router_interface_id));
    m_p4OidMapper->decreaseRefCount(
        SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
        KeyGenerator::generateNeighborKey(next_hop_entry->router_interface_id, next_hop_entry->neighbor_id));
    if (next_hop_entry->neighbor_id.isV4())
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_NEXTHOP);
    }
    else
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_NEXTHOP);
    }

    // Remove the key to OID map to centralized mapper.
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_key);

    // Remove the entry from internal table.
    m_nextHopTable.erase(next_hop_key);

    return ReturnCode();
}
