#include "p4orch/next_hop_manager.h"

#include <sstream>
#include <string>
#include <vector>

#include "SaiAttributeList.h"
#include "crmorch.h"
#include "dbconnector.h"
#include "ipaddress.h"
#include "json.hpp"
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

P4NextHopEntry::P4NextHopEntry(const std::string &next_hop_id, const std::string &router_interface_id,
                               const std::string &gre_tunnel_id, const swss::IpAddress &neighbor_id)
    : next_hop_id(next_hop_id), router_interface_id(router_interface_id), gre_tunnel_id(gre_tunnel_id),
      neighbor_id(neighbor_id)
{
    SWSS_LOG_ENTER();
    next_hop_key = KeyGenerator::generateNextHopKey(next_hop_id);
}

namespace
{

ReturnCode validateAppDbEntry(const P4NextHopAppDbEntry &app_db_entry)
{
    // TODO(b/225242372): remove kSetNexthop action after P4RT and Orion update
    // naming
    if (app_db_entry.action_str != p4orch::kSetIpNexthop && app_db_entry.action_str != p4orch::kSetNexthop &&
        app_db_entry.action_str != p4orch::kSetTunnelNexthop)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Invalid action " << QuotedVar(app_db_entry.action_str) << " of Nexthop App DB entry";
    }
    if (app_db_entry.action_str == p4orch::kSetIpNexthop && app_db_entry.neighbor_id.isZero())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Missing field " << QuotedVar(prependParamField(p4orch::kNeighborId)) << " for action "
               << QuotedVar(p4orch::kSetIpNexthop) << " in table entry";
    }
    // TODO(b/225242372): remove kSetNexthop action after P4RT and Orion update
    // naming
    if (app_db_entry.action_str == p4orch::kSetIpNexthop || app_db_entry.action_str == p4orch::kSetNexthop)
    {
        if (!app_db_entry.gre_tunnel_id.empty())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Unexpected field " << QuotedVar(prependParamField(p4orch::kTunnelId)) << " for action "
                   << QuotedVar(p4orch::kSetIpNexthop) << " in table entry";
        }
        if (app_db_entry.router_interface_id.empty())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Missing field " << QuotedVar(prependParamField(p4orch::kRouterInterfaceId)) << " for action "
                   << QuotedVar(p4orch::kSetIpNexthop) << " in table entry";
        }
    }

    if (app_db_entry.action_str == p4orch::kSetTunnelNexthop)
    {
        if (!app_db_entry.router_interface_id.empty())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Unexpected field " << QuotedVar(prependParamField(p4orch::kRouterInterfaceId)) << " for action "
                   << QuotedVar(p4orch::kSetTunnelNexthop) << " in table entry";
        }
        if (app_db_entry.gre_tunnel_id.empty())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Missing field " << QuotedVar(prependParamField(p4orch::kTunnelId)) << " for action "
                   << QuotedVar(p4orch::kSetTunnelNexthop) << " in table entry";
        }
    }
    return ReturnCode();
}

} // namespace

ReturnCodeOr<std::vector<sai_attribute_t>> NextHopManager::getSaiAttrs(const P4NextHopEntry &next_hop_entry)
{
    std::vector<sai_attribute_t> next_hop_attrs;
    sai_attribute_t next_hop_attr;

    if (!next_hop_entry.gre_tunnel_id.empty())
    {
        // From centralized mapper and, get gre tunnel that next hop depends on. Get
        // underlay router interface from gre tunnel manager,
        sai_object_id_t tunnel_oid;
        if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_TUNNEL,
                                   KeyGenerator::generateTunnelKey(next_hop_entry.gre_tunnel_id), &tunnel_oid))
        {
            LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                                 << "GRE Tunnel " << QuotedVar(next_hop_entry.gre_tunnel_id) << " does not exist");
        }

        next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
        next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP;
        next_hop_attrs.push_back(next_hop_attr);

        next_hop_attr.id = SAI_NEXT_HOP_ATTR_TUNNEL_ID;
        next_hop_attr.value.oid = tunnel_oid;
        next_hop_attrs.push_back(next_hop_attr);
    }
    else
    {
        // From centralized mapper, get OID of router interface that next hop
        // depends on.
        sai_object_id_t rif_oid;
        if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                   KeyGenerator::generateRouterInterfaceKey(next_hop_entry.router_interface_id),
                                   &rif_oid))
        {
            LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                                 << "Router intf " << QuotedVar(next_hop_entry.router_interface_id)
                                 << " does not exist");
        }
        next_hop_attr.id = SAI_NEXT_HOP_ATTR_TYPE;
        next_hop_attr.value.s32 = SAI_NEXT_HOP_TYPE_IP;
        next_hop_attrs.push_back(next_hop_attr);

        next_hop_attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
        next_hop_attr.value.oid = rif_oid;
        next_hop_attrs.push_back(next_hop_attr);
    }

    next_hop_attr.id = SAI_NEXT_HOP_ATTR_IP;
    swss::copy(next_hop_attr.value.ipaddr, next_hop_entry.neighbor_id);
    next_hop_attrs.push_back(next_hop_attr);

    return next_hop_attrs;
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
            status = validateAppDbEntry(app_db_entry);
            if (!status.ok())
            {
                SWSS_LOG_ERROR("Validation failed for Nexthop APP DB entry with key %s: %s",
                               QuotedVar(kfvKey(key_op_fvs_tuple)).c_str(), status.message().c_str());
                m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                     status,
                                     /*replace=*/true);
                continue;
            }
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

ReturnCode NextHopManager::processAddRequest(const P4NextHopAppDbEntry &app_db_entry)
{
    SWSS_LOG_ENTER();

    P4NextHopEntry next_hop_entry(app_db_entry.next_hop_id, app_db_entry.router_interface_id,
                                  app_db_entry.gre_tunnel_id, app_db_entry.neighbor_id);
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

    if (!next_hop_entry.gre_tunnel_id.empty())
    {
        auto gre_tunnel_or = gP4Orch->getGreTunnelManager()->getConstGreTunnelEntry(
            KeyGenerator::generateTunnelKey(next_hop_entry.gre_tunnel_id));
        if (!gre_tunnel_or.ok())
        {
            LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                                 << "GRE Tunnel " << QuotedVar(next_hop_entry.gre_tunnel_id)
                                 << " does not exist in GRE Tunnel Manager");
        }
        next_hop_entry.router_interface_id = (*gre_tunnel_or).router_interface_id;
        // BRCM requires neighbor object to be created before GRE tunnel, referring
        // to the one in GRE tunnel object when creating next_hop_entry_with
        // setTunnelAction
        next_hop_entry.neighbor_id = (*gre_tunnel_or).neighbor_id;
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

    ASSIGN_OR_RETURN(std::vector<sai_attribute_t> attrs, getSaiAttrs(next_hop_entry));

    // Call SAI API.
    CHECK_ERROR_AND_LOG_AND_RETURN(sai_next_hop_api->create_next_hop(&next_hop_entry.next_hop_oid, gSwitchId,
                                                                     (uint32_t)attrs.size(), attrs.data()),
                                   "Failed to create next hop " << QuotedVar(next_hop_entry.next_hop_key));

    if (!next_hop_entry.gre_tunnel_id.empty())
    {
        // On successful creation, increment ref count for tunnel object
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_TUNNEL,
                                        KeyGenerator::generateTunnelKey(next_hop_entry.gre_tunnel_id));
    }
    else
    {
        // On successful creation, increment ref count for router intf object
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                        KeyGenerator::generateRouterInterfaceKey(next_hop_entry.router_interface_id));
    }

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

    if (!next_hop_entry->gre_tunnel_id.empty())
    {
        // On successful deletion, decrement ref count for tunnel object
        m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_TUNNEL,
                                        KeyGenerator::generateTunnelKey(next_hop_entry->gre_tunnel_id));
    }
    else
    {
        // On successful deletion, decrement ref count for router intf object
        m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                        KeyGenerator::generateRouterInterfaceKey(next_hop_entry->router_interface_id));
    }

    std::string router_interface_id = next_hop_entry->router_interface_id;
    if (!next_hop_entry->gre_tunnel_id.empty())
    {
        auto gre_tunnel_or = gP4Orch->getGreTunnelManager()->getConstGreTunnelEntry(
            KeyGenerator::generateTunnelKey(next_hop_entry->gre_tunnel_id));
        if (!gre_tunnel_or.ok())
        {
            LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                                 << "GRE Tunnel " << QuotedVar(next_hop_entry->gre_tunnel_id)
                                 << " does not exist in GRE Tunnel Manager");
        }
        router_interface_id = (*gre_tunnel_or).router_interface_id;
    }
    m_p4OidMapper->decreaseRefCount(
        SAI_OBJECT_TYPE_NEIGHBOR_ENTRY,
        KeyGenerator::generateNeighborKey(router_interface_id, next_hop_entry->neighbor_id));
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

std::string NextHopManager::verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
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
    if (table_name != APP_P4RT_NEXTHOP_TABLE_NAME)
    {
        return std::string("Invalid key: ") + key;
    }

    ReturnCode status;
    auto app_db_entry_or = deserializeP4NextHopAppDbEntry(key_content, tuple);
    if (!app_db_entry_or.ok())
    {
        status = app_db_entry_or.status();
        std::stringstream msg;
        msg << "Unable to deserialize key " << QuotedVar(key) << ": " << status.message();
        return msg.str();
    }
    auto &app_db_entry = *app_db_entry_or;
    const std::string next_hop_key = KeyGenerator::generateNextHopKey(app_db_entry.next_hop_id);
    auto *next_hop_entry = getNextHopEntry(next_hop_key);
    if (next_hop_entry == nullptr)
    {
        std::stringstream msg;
        msg << "No entry found with key " << QuotedVar(key);
        return msg.str();
    }

    std::string cache_result = verifyStateCache(app_db_entry, next_hop_entry);
    std::string asic_db_result = verifyStateAsicDb(next_hop_entry);
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
    auto attrs_or = getSaiAttrs(*next_hop_entry);
    if (!attrs_or.ok())
    {
        return std::string("Failed to get SAI attrs: ") + attrs_or.status().message();
    }
    std::vector<sai_attribute_t> attrs = *attrs_or;
    std::vector<swss::FieldValueTuple> exp =
        saimeta::SaiAttributeList::serialize_attr_list(SAI_OBJECT_TYPE_NEXT_HOP, (uint32_t)attrs.size(), attrs.data(),
                                                       /*countOnly=*/false);

    swss::DBConnector db("ASIC_DB", 0);
    swss::Table table(&db, "ASIC_STATE");
    std::string key = sai_serialize_object_type(SAI_OBJECT_TYPE_NEXT_HOP) + ":" +
                      sai_serialize_object_id(next_hop_entry->next_hop_oid);
    std::vector<swss::FieldValueTuple> values;
    if (!table.get(key, values))
    {
        return std::string("ASIC DB key not found ") + key;
    }

    return verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                       /*allow_unknown=*/false);
}
