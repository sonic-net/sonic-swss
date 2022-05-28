#include "p4orch/route_manager.h"

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "crmorch.h"
#include "json.hpp"
#include "logger.h"
#include "p4orch/p4orch_util.h"
#include "swssnet.h"

extern sai_object_id_t gSwitchId;
extern sai_object_id_t gVirtualRouterId;

extern sai_route_api_t *sai_route_api;

extern CrmOrch *gCrmOrch;

namespace
{

// This function will perform a route update. A route update will have two
// attribute update. If the second attribut update fails, the function will try
// to revert the first attribute. If the revert fails, the function will raise
// critical state.
ReturnCode UpdateRouteAttrs(sai_packet_action_t old_action, sai_packet_action_t new_action, sai_object_id_t old_nexthop,
                            sai_object_id_t new_nexthop, const std::string &route_entry_key,
                            sai_route_entry_t *rotue_entry)
{
    SWSS_LOG_ENTER();
    // For drop action, we will update the action attribute first.
    bool action_first = (new_action == SAI_PACKET_ACTION_DROP);

    // First attribute
    sai_attribute_t route_attr;
    route_attr.id = (action_first) ? SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION : SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    if (action_first)
    {
        route_attr.value.s32 = new_action;
    }
    else
    {
        route_attr.value.oid = new_nexthop;
    }
    CHECK_ERROR_AND_LOG_AND_RETURN(sai_route_api->set_route_entry_attribute(rotue_entry, &route_attr),
                                   "Failed to set SAI attribute "
                                       << (action_first ? "SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION"
                                                        : "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID")
                                       << " when updating route " << QuotedVar(route_entry_key));

    // Second attribute
    route_attr.id = (action_first) ? SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID : SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    if (action_first)
    {
        route_attr.value.oid = new_nexthop;
    }
    else
    {
        route_attr.value.s32 = new_action;
    }
    ReturnCode status;
    auto sai_status = sai_route_api->set_route_entry_attribute(rotue_entry, &route_attr);
    if (sai_status == SAI_STATUS_SUCCESS)
    {
        return ReturnCode();
    }
    status = ReturnCode(sai_status) << "Failed to set SAI attribute "
                                    << (action_first ? "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"
                                                     : "SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION")
                                    << " when updating route " << QuotedVar(route_entry_key);
    SWSS_LOG_ERROR("%s SAI_STATUS: %s", status.message().c_str(), sai_serialize_status(sai_status).c_str());

    // Revert the first attribute
    route_attr.id = (action_first) ? SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION : SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    if (action_first)
    {
        route_attr.value.s32 = old_action;
    }
    else
    {
        route_attr.value.oid = old_nexthop;
    }
    sai_status = sai_route_api->set_route_entry_attribute(rotue_entry, &route_attr);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        // Raise critical state if we fail to recover.
        std::stringstream msg;
        msg << "Failed to revert route attribute "
            << (action_first ? "SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION" : "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID")
            << " for route " << QuotedVar(route_entry_key);
        SWSS_LOG_ERROR("%s SAI_STATUS: %s", msg.str().c_str(), sai_serialize_status(sai_status).c_str());
        SWSS_RAISE_CRITICAL_STATE(msg.str());
    }

    return status;
}

} // namespace

bool RouteManager::mergeRouteEntry(const P4RouteEntry &dest, const P4RouteEntry &src, P4RouteEntry *ret)
{
    SWSS_LOG_ENTER();

    *ret = src;
    ret->sai_route_entry = dest.sai_route_entry;
    if (ret->action.empty())
    {
        ret->action = dest.action;
    }
    if (ret->action != dest.action || ret->nexthop_id != dest.nexthop_id || ret->wcmp_group != dest.wcmp_group)
    {
        return true;
    }
    return false;
}

ReturnCodeOr<P4RouteEntry> RouteManager::deserializeRouteEntry(const std::string &key,
                                                               const std::vector<swss::FieldValueTuple> &attributes,
                                                               const std::string &table_name)
{
    SWSS_LOG_ENTER();

    P4RouteEntry route_entry = {};
    std::string route_prefix;
    try
    {
        nlohmann::json j = nlohmann::json::parse(key);
        route_entry.vrf_id = j[prependMatchField(p4orch::kVrfId)];
        if (table_name == APP_P4RT_IPV4_TABLE_NAME)
        {
            if (j.find(prependMatchField(p4orch::kIpv4Dst)) != j.end())
            {
                route_prefix = j[prependMatchField(p4orch::kIpv4Dst)];
            }
            else
            {
                route_prefix = "0.0.0.0/0";
            }
        }
        else
        {
            if (j.find(prependMatchField(p4orch::kIpv6Dst)) != j.end())
            {
                route_prefix = j[prependMatchField(p4orch::kIpv6Dst)];
            }
            else
            {
                route_prefix = "::/0";
            }
        }
    }
    catch (std::exception &ex)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Failed to deserialize route key";
    }
    try
    {
        route_entry.route_prefix = swss::IpPrefix(route_prefix);
    }
    catch (std::exception &ex)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Invalid IP prefix " << QuotedVar(route_prefix);
    }

    route_entry.route_entry_key = KeyGenerator::generateRouteKey(route_entry.vrf_id, route_entry.route_prefix);

    for (const auto &it : attributes)
    {
        const auto &field = fvField(it);
        const auto &value = fvValue(it);
        if (field == p4orch::kAction)
        {
            route_entry.action = value;
        }
        else if (field == prependParamField(p4orch::kNexthopId))
        {
            route_entry.nexthop_id = value;
        }
        else if (field == prependParamField(p4orch::kWcmpGroupId))
        {
            route_entry.wcmp_group = value;
        }
        else if (field != p4orch::kControllerMetadata)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Unexpected field " << QuotedVar(field) << " in " << table_name;
        }
    }

    return route_entry;
}

P4RouteEntry *RouteManager::getRouteEntry(const std::string &route_entry_key)
{
    SWSS_LOG_ENTER();

    if (m_routeTable.find(route_entry_key) == m_routeTable.end())
        return nullptr;

    return &m_routeTable[route_entry_key];
}

ReturnCode RouteManager::validateRouteEntry(const P4RouteEntry &route_entry)
{
    SWSS_LOG_ENTER();

    if (!route_entry.nexthop_id.empty())
    {
        auto nexthop_key = KeyGenerator::generateNextHopKey(route_entry.nexthop_id);
        if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_NEXT_HOP, nexthop_key))
        {
            return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                   << "Nexthop ID " << QuotedVar(route_entry.nexthop_id) << " does not exist";
        }
    }
    if (!route_entry.wcmp_group.empty())
    {
        auto wcmp_group_key = KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group);
        if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key))
        {
            return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                   << "WCMP group " << QuotedVar(route_entry.wcmp_group) << " does not exist";
        }
    }
    if (!route_entry.vrf_id.empty() && !m_vrfOrch->isVRFexists(route_entry.vrf_id))
    {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND) << "No VRF found with name " << QuotedVar(route_entry.vrf_id);
    }
    return ReturnCode();
}

ReturnCode RouteManager::validateSetRouteEntry(const P4RouteEntry &route_entry)
{
    auto *route_entry_ptr = getRouteEntry(route_entry.route_entry_key);
    bool exist_in_mapper = m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry.route_entry_key);
    if (route_entry_ptr == nullptr && exist_in_mapper)
    {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND) << "Route entry does not exist in manager but exists in the "
                                                            "centralized map";
    }
    if (route_entry_ptr != nullptr && !exist_in_mapper)
    {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND) << "Route entry exists in manager but does not exist in the "
                                                            "centralized map";
    }
    std::string action = route_entry.action;
    // If action is empty, this could be an update.
    if (action.empty())
    {
        if (route_entry_ptr == nullptr)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Empty action for route";
        }
        action = route_entry_ptr->action;
    }
    if (action == p4orch::kSetNexthopId)
    {
        if (route_entry.nexthop_id.empty())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Empty nexthop_id for route with nexthop_id action";
        }
        if (!route_entry.wcmp_group.empty())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Non-empty wcmp_group_id for route with nexthop_id action";
        }
    }
    else if (action == p4orch::kSetWcmpGroupId)
    {
        if (!route_entry.nexthop_id.empty())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Non-empty nexthop_id for route with wcmp_group action";
        }
        if (route_entry.wcmp_group.empty())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Empty wcmp_group_id for route with wcmp_group action";
        }
    }
    else if (action == p4orch::kDrop)
    {
        if (!route_entry.nexthop_id.empty())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Non-empty nexthop_id for route with drop action";
        }
        if (!route_entry.wcmp_group.empty())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Non-empty wcmp_group_id for route with drop action";
        }
    }
    else
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Invalid action " << QuotedVar(action);
    }
    return ReturnCode();
}

ReturnCode RouteManager::validateDelRouteEntry(const P4RouteEntry &route_entry)
{
    if (getRouteEntry(route_entry.route_entry_key) == nullptr)
    {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND) << "Route entry does not exist";
    }
    if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry.route_entry_key))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Route entry does not exist in the centralized map");
    }
    if (!route_entry.action.empty())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Non-empty action for Del route";
    }
    if (!route_entry.nexthop_id.empty())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Non-empty nexthop_id for Del route";
    }
    if (!route_entry.wcmp_group.empty())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Non-empty wcmp_group for Del route";
    }
    return ReturnCode();
}

ReturnCode RouteManager::createRouteEntry(const P4RouteEntry &route_entry)
{
    SWSS_LOG_ENTER();

    sai_route_entry_t sai_route_entry;
    sai_route_entry.vr_id = m_vrfOrch->getVRFid(route_entry.vrf_id);
    sai_route_entry.switch_id = gSwitchId;
    copy(sai_route_entry.destination, route_entry.route_prefix);
    if (route_entry.action == p4orch::kSetNexthopId)
    {
        auto nexthop_key = KeyGenerator::generateNextHopKey(route_entry.nexthop_id);
        sai_object_id_t next_hop_oid;
        m_p4OidMapper->getOID(SAI_OBJECT_TYPE_NEXT_HOP, nexthop_key, &next_hop_oid);
        sai_attribute_t route_attr;
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        route_attr.value.oid = next_hop_oid;
        // Default SAI_ROUTE_ATTR_PACKET_ACTION is SAI_PACKET_ACTION_FORWARD.
        CHECK_ERROR_AND_LOG_AND_RETURN(sai_route_api->create_route_entry(&sai_route_entry, /*size=*/1, &route_attr),
                                       "Failed to create route " << QuotedVar(route_entry.route_entry_key)
                                                                 << " with next hop "
                                                                 << QuotedVar(route_entry.nexthop_id));
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP, nexthop_key);
    }
    else if (route_entry.action == p4orch::kSetWcmpGroupId)
    {
        auto wcmp_group_key = KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group);
        sai_object_id_t wcmp_group_oid;
        m_p4OidMapper->getOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key, &wcmp_group_oid);
        sai_attribute_t route_attr;
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        route_attr.value.oid = wcmp_group_oid;
        // Default SAI_ROUTE_ATTR_PACKET_ACTION is SAI_PACKET_ACTION_FORWARD.
        CHECK_ERROR_AND_LOG_AND_RETURN(sai_route_api->create_route_entry(&sai_route_entry, /*size=*/1, &route_attr),
                                       "Failed to create route " << QuotedVar(route_entry.route_entry_key)
                                                                 << " with wcmp group "
                                                                 << QuotedVar(route_entry.wcmp_group));
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key);
    }
    else
    {
        sai_attribute_t route_attr;
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        route_attr.value.s32 = SAI_PACKET_ACTION_DROP;
        CHECK_ERROR_AND_LOG_AND_RETURN(sai_route_api->create_route_entry(&sai_route_entry, /*size=*/1, &route_attr),
                                       "Failed to create route " << QuotedVar(route_entry.route_entry_key)
                                                                 << " with action drop");
    }

    m_routeTable[route_entry.route_entry_key] = route_entry;
    m_routeTable[route_entry.route_entry_key].sai_route_entry = sai_route_entry;
    m_p4OidMapper->setDummyOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry.route_entry_key);
    if (route_entry.route_prefix.isV4())
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
    }
    else
    {
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
    }
    m_vrfOrch->increaseVrfRefCount(route_entry.vrf_id);
    return ReturnCode();
}

ReturnCodeOr<sai_object_id_t> RouteManager::getNexthopOid(const P4RouteEntry &route_entry)
{
    sai_object_id_t oid = SAI_NULL_OBJECT_ID;
    if (route_entry.action == p4orch::kSetNexthopId)
    {
        auto nexthop_key = KeyGenerator::generateNextHopKey(route_entry.nexthop_id);
        if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_NEXT_HOP, nexthop_key, &oid))
        {
            RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Nexthop " << QuotedVar(route_entry.nexthop_id)
                                                                << " does not exist");
        }
    }
    else if (route_entry.action == p4orch::kSetWcmpGroupId)
    {
        auto wcmp_group_key = KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group);
        if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key, &oid))
        {
            RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("WCMP group " << QuotedVar(route_entry.wcmp_group)
                                                                   << " does not exist");
        }
    }
    return oid;
}

ReturnCode RouteManager::updateRouteEntry(const P4RouteEntry &route_entry)
{
    SWSS_LOG_ENTER();

    auto *route_entry_ptr = getRouteEntry(route_entry.route_entry_key);
    P4RouteEntry new_route_entry;
    if (!mergeRouteEntry(*route_entry_ptr, route_entry, &new_route_entry))
    {
        return ReturnCode();
    }

    ASSIGN_OR_RETURN(sai_object_id_t old_nexthop, getNexthopOid(*route_entry_ptr));
    ASSIGN_OR_RETURN(sai_object_id_t new_nexthop, getNexthopOid(new_route_entry));
    RETURN_IF_ERROR(UpdateRouteAttrs(
        (route_entry_ptr->action == p4orch::kDrop) ? SAI_PACKET_ACTION_DROP : SAI_PACKET_ACTION_FORWARD,
        (new_route_entry.action == p4orch::kDrop) ? SAI_PACKET_ACTION_DROP : SAI_PACKET_ACTION_FORWARD, old_nexthop,
        new_nexthop, new_route_entry.route_entry_key, &new_route_entry.sai_route_entry));

    if (new_route_entry.action == p4orch::kSetNexthopId)
    {
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                        KeyGenerator::generateNextHopKey(new_route_entry.nexthop_id));
    }
    if (new_route_entry.action == p4orch::kSetWcmpGroupId)
    {
        m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                        KeyGenerator::generateWcmpGroupKey(new_route_entry.wcmp_group));
    }

    if (route_entry_ptr->action == p4orch::kSetNexthopId)
    {
        if (new_route_entry.action != p4orch::kSetNexthopId ||
            new_route_entry.nexthop_id != route_entry_ptr->nexthop_id)
        {
            m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                            KeyGenerator::generateNextHopKey(route_entry_ptr->nexthop_id));
        }
    }
    if (route_entry_ptr->action == p4orch::kSetWcmpGroupId)
    {
        if (new_route_entry.action != p4orch::kSetWcmpGroupId ||
            new_route_entry.wcmp_group != route_entry_ptr->wcmp_group)
        {
            m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                            KeyGenerator::generateWcmpGroupKey(route_entry_ptr->wcmp_group));
        }
    }
    m_routeTable[route_entry.route_entry_key] = new_route_entry;
    return ReturnCode();
}

ReturnCode RouteManager::deleteRouteEntry(const P4RouteEntry &route_entry)
{
    SWSS_LOG_ENTER();

    auto *route_entry_ptr = getRouteEntry(route_entry.route_entry_key);
    CHECK_ERROR_AND_LOG_AND_RETURN(sai_route_api->remove_route_entry(&route_entry_ptr->sai_route_entry),
                                   "Failed to delete route " << QuotedVar(route_entry.route_entry_key));

    if (route_entry_ptr->action == p4orch::kSetNexthopId)
    {
        m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                        KeyGenerator::generateNextHopKey(route_entry_ptr->nexthop_id));
    }
    if (route_entry_ptr->action == p4orch::kSetWcmpGroupId)
    {
        m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                        KeyGenerator::generateWcmpGroupKey(route_entry_ptr->wcmp_group));
    }
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_ROUTE_ENTRY, route_entry.route_entry_key);
    if (route_entry.route_prefix.isV4())
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
    }
    else
    {
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
    }
    m_vrfOrch->decreaseVrfRefCount(route_entry.vrf_id);
    m_routeTable.erase(route_entry.route_entry_key);
    return ReturnCode();
}

void RouteManager::enqueue(const swss::KeyOpFieldsValuesTuple &entry)
{
    m_entries.push_back(entry);
}

void RouteManager::drain()
{
    SWSS_LOG_ENTER();

    for (const auto &key_op_fvs_tuple : m_entries)
    {
        std::string table_name;
        std::string key;
        parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &key);
        const std::vector<swss::FieldValueTuple> &attributes = kfvFieldsValues(key_op_fvs_tuple);

        ReturnCode status;
        auto route_entry_or = deserializeRouteEntry(key, attributes, table_name);
        if (!route_entry_or.ok())
        {
            status = route_entry_or.status();
            SWSS_LOG_ERROR("Unable to deserialize APP DB entry with key %s: %s",
                           QuotedVar(table_name + ":" + key).c_str(), status.message().c_str());
            m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                 status,
                                 /*replace=*/true);
            continue;
        }
        auto &route_entry = *route_entry_or;

        status = validateRouteEntry(route_entry);
        if (!status.ok())
        {
            SWSS_LOG_ERROR("Validation failed for Route APP DB entry with key  %s: %s",
                           QuotedVar(table_name + ":" + key).c_str(), status.message().c_str());
            m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                 status,
                                 /*replace=*/true);
            continue;
        }

        const std::string &operation = kfvOp(key_op_fvs_tuple);
        if (operation == SET_COMMAND)
        {
            status = validateSetRouteEntry(route_entry);
            if (!status.ok())
            {
                SWSS_LOG_ERROR("Validation failed for Set Route APP DB entry with key %s: %s",
                               QuotedVar(table_name + ":" + key).c_str(), status.message().c_str());
            }
            else if (getRouteEntry(route_entry.route_entry_key) == nullptr)
            {
                status = createRouteEntry(route_entry);
            }
            else
            {
                status = updateRouteEntry(route_entry);
            }
        }
        else if (operation == DEL_COMMAND)
        {
            status = validateDelRouteEntry(route_entry);
            if (!status.ok())
            {
                SWSS_LOG_ERROR("Validation failed for Del Route APP DB entry with key %s: %s",
                               QuotedVar(table_name + ":" + key).c_str(), status.message().c_str());
            }
            else
            {
                status = deleteRouteEntry(route_entry);
            }
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
