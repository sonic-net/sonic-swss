#include "p4orch/route_manager.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "SaiAttributeList.h"
#include "converter.h"
#include "crmorch.h"
#include "dbconnector.h"
#include "logger.h"
#include "p4orch/p4orch_util.h"
#include "sai_serialize.h"
#include "swssnet.h"
#include "table.h"

using ::p4orch::kTableKeyDelimiter;

extern sai_object_id_t gSwitchId;
extern sai_object_id_t gVirtualRouterId;

extern sai_route_api_t *sai_route_api;

extern CrmOrch *gCrmOrch;

extern size_t gMaxBulkSize;

namespace
{

ReturnCode checkNextHopAndWcmpGroupAndRouteMetadataExistence(bool expected_next_hop_existence,
                                                             bool expected_wcmp_group_existence,
                                                             bool expected_route_metadata_existence,
                                                             const P4RouteEntry &route_entry)
{
    if (route_entry.nexthop_id.empty() && expected_next_hop_existence)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Empty nexthop_id for route with " << route_entry.action << " action";
    }
    if (!route_entry.nexthop_id.empty() && !expected_next_hop_existence)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Non-empty nexthop_id for route with " << route_entry.action << " action";
    }
    if (route_entry.wcmp_group.empty() && expected_wcmp_group_existence)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Empty wcmp_group_id for route with " << route_entry.action << " action";
    }
    if (!route_entry.wcmp_group.empty() && !expected_wcmp_group_existence)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Non-empty wcmp_group_id for route with " << route_entry.action << " action";
    }
    if (route_entry.route_metadata.empty() && expected_route_metadata_existence)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Empty route_metadata for route with " << route_entry.action << " action";
    }
    if (!route_entry.route_metadata.empty() && !expected_route_metadata_existence)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Non-empty route_metadata for route with " << route_entry.action << " action";
    }
    return ReturnCode();
}

// Returns the nexthop OID of the given entry.
// Raise critical state if OID cannot be found.
sai_object_id_t getNexthopOid(const P4RouteEntry &route_entry, const P4OidMapper &mapper)
{
    sai_object_id_t oid = SAI_NULL_OBJECT_ID;
    if (route_entry.action == p4orch::kSetNexthopId || route_entry.action == p4orch::kSetNexthopIdAndMetadata)
    {
        auto nexthop_key = KeyGenerator::generateNextHopKey(route_entry.nexthop_id);
        if (!mapper.getOID(SAI_OBJECT_TYPE_NEXT_HOP, nexthop_key, &oid))
        {
            std::stringstream msg;
            msg << "Nexthop " << QuotedVar(route_entry.nexthop_id) << " does not exist";
            SWSS_LOG_ERROR("%s", msg.str().c_str());
            SWSS_RAISE_CRITICAL_STATE(msg.str());
            return oid;
        }
    }
    else if (route_entry.action == p4orch::kSetWcmpGroupId || route_entry.action == p4orch::kSetWcmpGroupIdAndMetadata)
    {
        auto wcmp_group_key = KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group);
        if (!mapper.getOID(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, wcmp_group_key, &oid))
        {
            std::stringstream msg;
            msg << "WCMP group " << QuotedVar(route_entry.nexthop_id) << " does not exist";
            SWSS_LOG_ERROR("%s", msg.str().c_str());
            SWSS_RAISE_CRITICAL_STATE(msg.str());
            return oid;
        }
    }
    return oid;
}

// Returns the SAI action of the given entry.
sai_packet_action_t prepareSaiAction(const P4RouteEntry& route_entry) {
  if (route_entry.action == p4orch::kDrop ||
      route_entry.action == p4orch::kSetMetadataAndDrop) {
    return SAI_PACKET_ACTION_DROP;
  } else if (route_entry.action == p4orch::kTrap) {
    return SAI_PACKET_ACTION_TRAP;
  }
  return SAI_PACKET_ACTION_FORWARD;
}

// Returns the metadata of the given entry.
uint32_t getMetadata(const P4RouteEntry &route_entry)
{
    if (route_entry.route_metadata.empty())
    {
        return 0;
    }
    return swss::to_uint<uint32_t>(route_entry.route_metadata);
}

// Returns a list of SAI actions for route update.
std::vector<sai_route_entry_attr_t> prepareSaiActions(
    const std::string action) {
  static const auto* const kRouteActionToSaiActions = new std::unordered_map<
      std::string, std::vector<sai_route_entry_attr_t>>({
      {p4orch::kSetNexthopId,
       std::vector<sai_route_entry_attr_t>{SAI_ROUTE_ENTRY_ATTR_META_DATA,
                                           SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID,
                                           SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION}},
      {p4orch::kSetWcmpGroupId,
       std::vector<sai_route_entry_attr_t>{SAI_ROUTE_ENTRY_ATTR_META_DATA,
                                           SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID,
                                           SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION}},
      {p4orch::kSetNexthopIdAndMetadata,
       std::vector<sai_route_entry_attr_t>{SAI_ROUTE_ENTRY_ATTR_META_DATA,
                                           SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID,
                                           SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION}},
      {p4orch::kSetWcmpGroupIdAndMetadata,
       std::vector<sai_route_entry_attr_t>{SAI_ROUTE_ENTRY_ATTR_META_DATA,
                                           SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID,
                                           SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION}},
      {p4orch::kDrop,
       std::vector<sai_route_entry_attr_t>{SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION,
                                           SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID,
                                           SAI_ROUTE_ENTRY_ATTR_META_DATA}},
      {p4orch::kTrap,
       std::vector<sai_route_entry_attr_t>{SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION,
                                           SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID,
                                           SAI_ROUTE_ENTRY_ATTR_META_DATA}},
      {p4orch::kSetMetadataAndDrop,
       std::vector<sai_route_entry_attr_t>{SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION,
                                           SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID,
                                           SAI_ROUTE_ENTRY_ATTR_META_DATA}},
  });

  if (kRouteActionToSaiActions->count(action) == 0) {
    return std::vector<sai_route_entry_attr_t>{};
  }
  return kRouteActionToSaiActions->at(action);
}

} // namespace

RouteUpdater::RouteUpdater(const P4RouteEntry& old_route,
                           const P4RouteEntry& new_route, P4OidMapper* mapper)
    : m_oldRoute(old_route),
      m_newRoute(new_route),
      m_p4OidMapper(mapper),
      m_actions(prepareSaiActions(new_route.action)) {
  updateIdx();
}

P4RouteEntry RouteUpdater::getOldEntry() const
{
    return m_oldRoute;
}

P4RouteEntry RouteUpdater::getNewEntry() const
{
    return m_newRoute;
}

sai_route_entry_t RouteUpdater::prepareSaiEntry() const {
  return m_newRoute.sai_route_entry;
}

sai_attribute_t RouteUpdater::prepareSaiAttr() const {
  return prepareSaiAttr(m_idx);
}

sai_attribute_t RouteUpdater::prepareSaiAttr(int idx) const {
  sai_attribute_t route_attr = {};
  if (idx < 0 || idx >= static_cast<int>(m_actions.size())) {
    return route_attr;
  }
  route_attr.id = m_actions[idx];
  switch (m_actions[idx]) {
    case SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID:
        route_attr.value.oid =
            (m_revert) ? getNexthopOid(m_oldRoute, *m_p4OidMapper) : getNexthopOid(m_newRoute, *m_p4OidMapper);
        break;
    case SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION:
      route_attr.value.s32 = (m_revert) ? prepareSaiAction(m_oldRoute)
                                        : prepareSaiAction(m_newRoute);
      break;
    default:
        route_attr.value.u32 = (m_revert) ? getMetadata(m_oldRoute) : getMetadata(m_newRoute);
  }
    return route_attr;
}

bool RouteUpdater::updateResult(sai_status_t sai_status)
{
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        if (m_revert)
        {
            std::stringstream msg;
            msg << "Failed to revert SAI attribute for route entry " << QuotedVar(m_newRoute.route_entry_key);
            SWSS_LOG_ERROR("%s SAI_STATUS: %s", msg.str().c_str(), sai_serialize_status(sai_status).c_str());
            SWSS_RAISE_CRITICAL_STATE(msg.str());
        }
        else
        {
            m_status = ReturnCode(sai_status)
                       << "Failed to update route entry " << QuotedVar(m_newRoute.route_entry_key);
            m_revert = true;
        }
    }
    return updateIdx();
}

ReturnCode RouteUpdater::getStatus() const
{
    return m_status;
}

std::vector<sai_attribute_t> RouteUpdater::GetSaiAttrList() const {
  std::vector<sai_attribute_t> attrs;
  for (int idx = m_idx; idx >= 0 && idx < static_cast<int>(m_actions.size());) {
    attrs.push_back(prepareSaiAttr(idx));
    updateIdx(idx);
  }
  return attrs;
}

bool RouteUpdater::updateIdx() { return updateIdx(m_idx); }

bool RouteUpdater::updateIdx(int& idx) const {
  if (m_revert) {
    for (--idx; idx >= 0; --idx) {
      if (checkAction(idx)) {
        return false;
      }
    }
    return true;
  }
  for (++idx; idx < static_cast<int>(m_actions.size()); ++idx) {
    if (checkAction(idx)) {
      return false;
    }
  }
  return true;
}

bool RouteUpdater::checkAction(int idx) const {
  if (idx < 0 || idx >= static_cast<int>(m_actions.size())) {
    return false;
  }
  switch (m_actions[idx]) {
    case SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID:
        if (getNexthopOid(m_oldRoute, *m_p4OidMapper) == getNexthopOid(m_newRoute, *m_p4OidMapper))
        {
            return false;
        }
        return true;
    case SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION:
      if (prepareSaiAction(m_oldRoute) == prepareSaiAction(m_newRoute)) {
        return false;
      }
        return true;
    default:
        if (getMetadata(m_oldRoute) == getMetadata(m_newRoute))
        {
            return false;
        }
        return true;
  }
    return false;
}

RouteManager::RouteManager(P4OidMapper *p4oidMapper, VRFOrch *vrfOrch, ResponsePublisherInterface *publisher)
    : m_vrfOrch(vrfOrch), m_routerBulker(sai_route_api, gMaxBulkSize)
{
    SWSS_LOG_ENTER();

    assert(p4oidMapper != nullptr);
    m_p4OidMapper = p4oidMapper;
    assert(publisher != nullptr);
    m_publisher = publisher;
}

sai_route_entry_t RouteManager::prepareSaiEntry(
    const P4RouteEntry& route_entry) {
  sai_route_entry_t sai_entry;
  sai_entry.vr_id = m_vrfOrch->getVRFid(route_entry.vrf_id);
  sai_entry.switch_id = gSwitchId;
  copy(sai_entry.destination, route_entry.route_prefix);
  return sai_entry;
}

bool RouteManager::mergeRouteEntry(const P4RouteEntry &dest, const P4RouteEntry &src, P4RouteEntry *ret)
{
    SWSS_LOG_ENTER();

    *ret = src;
    ret->sai_route_entry = dest.sai_route_entry;
    if (ret->action != dest.action || ret->nexthop_id != dest.nexthop_id || ret->wcmp_group != dest.wcmp_group ||
        ret->route_metadata != dest.route_metadata)
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
        else if (field == prependParamField(p4orch::kRouteMetadata))
        {
            route_entry.route_metadata = value;
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

ReturnCode RouteManager::validateRouteEntry(const P4RouteEntry &route_entry, const std::string &operation)
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

    if (operation == SET_COMMAND)
    {
        return validateSetRouteEntry(route_entry);
    }
    else if (operation == DEL_COMMAND)
    {
        return validateDelRouteEntry(route_entry);
    }
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Unknown operation type " << QuotedVar(operation);
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
        RETURN_IF_ERROR(checkNextHopAndWcmpGroupAndRouteMetadataExistence(
            /*expected_next_hop_existence=*/true,
            /*expected_wcmp_group_existence=*/false,
            /*expected_route_metadata_existence=*/false, route_entry));
    }
    else if (action == p4orch::kSetWcmpGroupId)
    {
        RETURN_IF_ERROR(checkNextHopAndWcmpGroupAndRouteMetadataExistence(
            /*expected_next_hop_existence=*/false,
            /*expected_wcmp_group_existence=*/true,
            /*expected_route_metadata_existence=*/false, route_entry));
    }
    else if (action == p4orch::kSetNexthopIdAndMetadata)
    {
        RETURN_IF_ERROR(checkNextHopAndWcmpGroupAndRouteMetadataExistence(
            /*expected_next_hop_existence=*/true,
            /*expected_wcmp_group_existence=*/false,
            /*expected_route_metadata_existence=*/true, route_entry));
    }
    else if (action == p4orch::kSetWcmpGroupIdAndMetadata)
    {
        RETURN_IF_ERROR(checkNextHopAndWcmpGroupAndRouteMetadataExistence(
            /*expected_next_hop_existence=*/false,
            /*expected_wcmp_group_existence=*/true,
            /*expected_route_metadata_existence=*/true, route_entry));
    }
    else if (action == p4orch::kDrop || action == p4orch::kTrap)
    {
        RETURN_IF_ERROR(checkNextHopAndWcmpGroupAndRouteMetadataExistence(
            /*expected_next_hop_existence=*/false,
            /*expected_wcmp_group_existence=*/false,
            /*expected_route_metadata_existence=*/false, route_entry));
    }
    else if (action == p4orch::kSetMetadataAndDrop)
    {
        RETURN_IF_ERROR(checkNextHopAndWcmpGroupAndRouteMetadataExistence(
            /*expected_next_hop_existence=*/false,
            /*expected_wcmp_group_existence=*/false,
            /*expected_route_metadata_existence=*/true, route_entry));
    }
    else
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Invalid action " << QuotedVar(action);
    }

    if (!route_entry.route_metadata.empty())
    {
        try
        {
            swss::to_uint<uint32_t>(route_entry.route_metadata);
        }
        catch (std::exception &e)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Action attribute " << QuotedVar(p4orch::kRouteMetadata) << " is invalid for "
                   << QuotedVar(route_entry.route_entry_key) << ": Expect integer but got "
                   << QuotedVar(route_entry.route_metadata);
        }
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
    if (!route_entry.route_metadata.empty())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Non-empty route_metadata for Del route";
    }
    return ReturnCode();
}

ReturnCode RouteManager::processRouteEntries(
    const std::vector<P4RouteEntry>& route_entries,
    const std::vector<swss::KeyOpFieldsValuesTuple>& tuple_list,
    const std::string& op, bool update) {
  SWSS_LOG_ENTER();

  ReturnCode status;
  // In syncd, bulk SAI calls use mode SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR.
  if (op == SET_COMMAND) {
    if (!update) {
      auto statuses = createRouteEntries(route_entries);
      for (size_t i = 0; i < route_entries.size(); ++i) {
        m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(tuple_list[i]),
                             kfvFieldsValues(tuple_list[i]), statuses[i],
                             /*replace=*/true);
        if (status.ok() && !statuses[i].ok()) {
          status = statuses[i];
        }
      }
    } else {
      // TODO: Stop on first failure for batch update.
      auto statuses = updateRouteEntries(route_entries);
      for (size_t i = 0; i < route_entries.size(); ++i) {
        m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(tuple_list[i]),
                             kfvFieldsValues(tuple_list[i]), statuses[i],
                             /*replace=*/true);
        if (status.ok() && !statuses[i].ok()) {
          status = statuses[i];
        }
      }
    }
  } else {
    auto statuses = deleteRouteEntries(route_entries);
    for (size_t i = 0; i < route_entries.size(); ++i) {
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(tuple_list[i]),
                           kfvFieldsValues(tuple_list[i]), statuses[i],
                           /*replace=*/true);
      if (status.ok() && !statuses[i].ok()) {
        status = statuses[i];
      }
    }
  }

  return status;
}

std::vector<ReturnCode> RouteManager::createRouteEntries(const std::vector<P4RouteEntry> &route_entries)
{
    SWSS_LOG_ENTER();

    std::vector<sai_route_entry_t> sai_route_entries(route_entries.size());
    // Currently, there are maximum of 2 SAI attributes for route creation.
    // For drop and trap routes, there is one SAI attribute:
    // SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION.
    // For forwarding routes, the default SAI_ROUTE_ATTR_PACKET_ACTION is already
    // SAI_PACKET_ACTION_FORWARD, so we don't need SAI_ROUTE_ATTR_PACKET_ACTION.
    // But we need SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID and optionally
    // SAI_ROUTE_ENTRY_ATTR_META_DATA.
    std::vector<sai_attribute_t> sai_attrs(2 * route_entries.size());
    std::vector<sai_status_t> object_statuses(route_entries.size());
    std::vector<ReturnCode> statuses(route_entries.size());

    for (size_t i = 0; i < route_entries.size(); ++i)
    {
        const auto &route_entry = route_entries[i];
        sai_route_entries[i] = prepareSaiEntry(route_entry);
        uint32_t num_attrs = 1;
        if (route_entry.action == p4orch::kDrop)
        {
            sai_attrs[2 * i].id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
            sai_attrs[2 * i].value.s32 = SAI_PACKET_ACTION_DROP;
        }
        else if (route_entry.action == p4orch::kTrap)
        {
            sai_attrs[2 * i].id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
            sai_attrs[2 * i].value.s32 = SAI_PACKET_ACTION_TRAP;
        }
        else if (route_entry.action == p4orch::kSetMetadataAndDrop)
        {
            sai_attrs[2 * i].id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
            sai_attrs[2 * i].value.s32 = SAI_PACKET_ACTION_DROP;
            sai_attrs[2 * i + 1].id = SAI_ROUTE_ENTRY_ATTR_META_DATA;
            sai_attrs[2 * i + 1].value.u32 = swss::to_uint<uint32_t>(route_entry.route_metadata);
            num_attrs++;
        }
        else
        {
            // Default SAI_ROUTE_ATTR_PACKET_ACTION is SAI_PACKET_ACTION_FORWARD.
            sai_attrs[2 * i].id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
            sai_attrs[2 * i].value.oid = getNexthopOid(route_entry, *m_p4OidMapper);
            if (route_entry.action == p4orch::kSetNexthopIdAndMetadata ||
                route_entry.action == p4orch::kSetWcmpGroupIdAndMetadata)
            {
                sai_attrs[2 * i + 1].id = SAI_ROUTE_ENTRY_ATTR_META_DATA;
                sai_attrs[2 * i + 1].value.u32 = swss::to_uint<uint32_t>(route_entry.route_metadata);
                num_attrs++;
            }
        }
        object_statuses[i] =
            m_routerBulker.create_entry(&object_statuses[i], &sai_route_entries[i], num_attrs, &sai_attrs[2 * i]);
    }

    m_routerBulker.flush();

    for (size_t i = 0; i < route_entries.size(); ++i)
    {
        const auto &route_entry = route_entries[i];
        CHECK_ERROR_AND_LOG(object_statuses[i],
                            "Failed to create route entry " << QuotedVar(route_entry.route_entry_key));
        if (object_statuses[i] == SAI_STATUS_SUCCESS)
        {
            if (route_entry.action == p4orch::kSetNexthopId || route_entry.action == p4orch::kSetNexthopIdAndMetadata)
            {
                m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                                KeyGenerator::generateNextHopKey(route_entry.nexthop_id));
            }
            else if (route_entry.action == p4orch::kSetWcmpGroupId ||
                     route_entry.action == p4orch::kSetWcmpGroupIdAndMetadata)
            {
                m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                                KeyGenerator::generateWcmpGroupKey(route_entry.wcmp_group));
            }
            m_routeTable[route_entry.route_entry_key] = route_entry;
            m_routeTable[route_entry.route_entry_key].sai_route_entry = sai_route_entries[i];
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
            statuses[i] = ReturnCode();
        }
        else
        {
            statuses[i] = ReturnCode(object_statuses[i])
                          << "Failed to create route entry " << QuotedVar(route_entry.route_entry_key);
        }
    }

    return statuses;
}

void RouteManager::updateRouteEntriesMeta(const P4RouteEntry &old_entry, const P4RouteEntry &new_entry)
{
    if (getNexthopOid(old_entry, *m_p4OidMapper) != getNexthopOid(new_entry, *m_p4OidMapper))
    {
        if (new_entry.action == p4orch::kSetNexthopId || new_entry.action == p4orch::kSetNexthopIdAndMetadata)
        {
            m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                            KeyGenerator::generateNextHopKey(new_entry.nexthop_id));
        }
        else if (new_entry.action == p4orch::kSetWcmpGroupId || new_entry.action == p4orch::kSetWcmpGroupIdAndMetadata)
        {
            m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                            KeyGenerator::generateWcmpGroupKey(new_entry.wcmp_group));
        }
        if (old_entry.action == p4orch::kSetNexthopId || old_entry.action == p4orch::kSetNexthopIdAndMetadata)
        {
            m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                            KeyGenerator::generateNextHopKey(old_entry.nexthop_id));
        }
        else if (old_entry.action == p4orch::kSetWcmpGroupId || old_entry.action == p4orch::kSetWcmpGroupIdAndMetadata)
        {
            m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP_GROUP,
                                            KeyGenerator::generateWcmpGroupKey(old_entry.wcmp_group));
        }
    }
    m_routeTable[new_entry.route_entry_key] = new_entry;
}

void RouteManager::updateRouteAttrs(int size, const std::vector<std::unique_ptr<RouteUpdater>> &updaters,
                                    std::vector<size_t> &indice, std::vector<ReturnCode> &statuses)
{
  std::vector<sai_route_entry_t> sai_route_entries;
  std::vector<sai_attribute_t> sai_attrs;
  std::vector<size_t> updator_idx;
  // All SAI attribute update will be performed in a single bulk SAI call.
  // Syncd will stop on the first error.
  // If error occures, successful update will be reverted for that particular
  // request.
  for (int i = 0; i < size; ++i) {
    for (const auto& att : updaters[indice[i]]->GetSaiAttrList()) {
      sai_route_entries.push_back(updaters[indice[i]]->prepareSaiEntry());
      sai_attrs.push_back(att);
      updator_idx.push_back(indice[i]);
    }
    statuses[indice[i]] = ReturnCode();
  }
  std::vector<sai_status_t> object_statuses(updator_idx.size());
  for (size_t i = 0; i < updator_idx.size(); ++i) {
    m_routerBulker.set_entry_attribute(&object_statuses[i],
                                       &sai_route_entries[i], &sai_attrs[i]);
  }
  m_routerBulker.flush();

  for (size_t i = 0; i < updator_idx.size(); ++i) {
    updaters[updator_idx[i]]->updateResult(object_statuses[i]);
    if (object_statuses[i] != SAI_STATUS_SUCCESS) {
      auto revert_attrs = updaters[updator_idx[i]]->GetSaiAttrList();
      if (!revert_attrs.empty()) {
        std::vector<sai_route_entry_t> revert_entries(revert_attrs.size());
        std::vector<sai_status_t> revert_statuses(revert_attrs.size());
        for (size_t j = 0; j < revert_attrs.size(); ++j) {
          revert_entries[j] = updaters[updator_idx[i]]->prepareSaiEntry();
          m_routerBulker.set_entry_attribute(
              &revert_statuses[j], &revert_entries[j], &revert_attrs[j]);
        }
        m_routerBulker.flush();
        for (size_t j = 0; j < revert_attrs.size(); ++j) {
          updaters[updator_idx[i]]->updateResult(revert_statuses[j]);
        }
      }
      statuses[updator_idx[i]] = updaters[updator_idx[i]]->getStatus();
      for (size_t j = updator_idx[i] + 1; j < statuses.size(); ++j) {
        statuses[j] = ReturnCode(StatusCode::SWSS_RC_NOT_EXECUTED);
      }
      break;
    }
  }

  for (int i = 0; i < size; ++i) {
    if (statuses[indice[i]].ok()) {
      updateRouteEntriesMeta(updaters[indice[i]]->getOldEntry(),
                             updaters[indice[i]]->getNewEntry());
    } else {
      break;
    }
  }

    return;
}

std::vector<ReturnCode> RouteManager::updateRouteEntries(const std::vector<P4RouteEntry> &route_entries)
{
    SWSS_LOG_ENTER();

    std::vector<std::unique_ptr<RouteUpdater>> updaters(route_entries.size());
    std::vector<size_t> indice(route_entries.size()); // index to the route_entries
    std::vector<ReturnCode> statuses(route_entries.size());

    int size = 0;
    for (size_t i = 0; i < route_entries.size(); ++i)
    {
        const auto &route_entry = route_entries[i];
        auto *route_entry_ptr = getRouteEntry(route_entry.route_entry_key);
        P4RouteEntry new_entry;
        if (!mergeRouteEntry(*route_entry_ptr, route_entry, &new_entry))
        {
            statuses[i] = ReturnCode();
            continue;
        }
        updaters[i] = std::unique_ptr<RouteUpdater>(new RouteUpdater(*route_entry_ptr, new_entry, m_p4OidMapper));
        indice[size++] = i;
    }
    if (size == 0)
    {
        return statuses;
    }

    updateRouteAttrs(size, updaters, indice, statuses);
    return statuses;
}

std::vector<ReturnCode> RouteManager::deleteRouteEntries(const std::vector<P4RouteEntry> &route_entries)
{
    SWSS_LOG_ENTER();

    std::vector<sai_route_entry_t> sai_route_entries(route_entries.size());
    std::vector<sai_status_t> object_statuses(route_entries.size());
    std::vector<ReturnCode> statuses(route_entries.size());

    for (size_t i = 0; i < route_entries.size(); ++i)
    {
        const auto &route_entry = route_entries[i];
        auto *route_entry_ptr = getRouteEntry(route_entry.route_entry_key);
        sai_route_entries[i] = route_entry_ptr->sai_route_entry;
        object_statuses[i] = m_routerBulker.remove_entry(&object_statuses[i], &sai_route_entries[i]);
    }

    m_routerBulker.flush();

    for (size_t i = 0; i < route_entries.size(); ++i)
    {
        const auto &route_entry = route_entries[i];
        auto *route_entry_ptr = getRouteEntry(route_entry.route_entry_key);
        CHECK_ERROR_AND_LOG(object_statuses[i],
                            "Failed to delete route entry " << QuotedVar(route_entry.route_entry_key));
        if (object_statuses[i] == SAI_STATUS_SUCCESS)
        {
            if (route_entry_ptr->action == p4orch::kSetNexthopId ||
                route_entry_ptr->action == p4orch::kSetNexthopIdAndMetadata)
            {
                m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_NEXT_HOP,
                                                KeyGenerator::generateNextHopKey(route_entry_ptr->nexthop_id));
            }
            else if (route_entry_ptr->action == p4orch::kSetWcmpGroupId ||
                     route_entry_ptr->action == p4orch::kSetWcmpGroupIdAndMetadata)
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
            statuses[i] = ReturnCode();
        }
        else
        {
            statuses[i] = ReturnCode(object_statuses[i])
                          << "Failed to delete route entry " << QuotedVar(route_entry.route_entry_key);
        }
    }

    return statuses;
}

ReturnCode RouteManager::getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                                      std::string &object_key)
{
    return StatusCode::SWSS_RC_UNIMPLEMENTED;
}

void RouteManager::enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry)
{
    m_entries.push_back(entry);
}

void RouteManager::drainWithNotExecuted() {
  drainMgmtWithNotExecuted(m_entries, m_publisher);
}

ReturnCode RouteManager::drain() {
  SWSS_LOG_ENTER();

  std::vector<P4RouteEntry> route_list;
  std::vector<swss::KeyOpFieldsValuesTuple> tuple_list;
  std::unordered_set<std::string> route_entry_list;

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

    auto route_entry_or = deserializeRouteEntry(key, attributes, table_name);
    if (!route_entry_or.ok()) {
      status = route_entry_or.status();
      SWSS_LOG_ERROR("Unable to deserialize APP DB entry with key %s: %s",
                     QuotedVar(table_name + ":" + key).c_str(),
                     status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }
    auto& route_entry = *route_entry_or;

    // A single batch should not modify the same route more than once.
    if (route_entry_list.count(route_entry.route_entry_key) != 0) {
      status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Route entry has been included in the same batch";
      SWSS_LOG_ERROR("%s: %s", status.message().c_str(),
                     QuotedVar(route_entry.route_entry_key).c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }

    const std::string& operation = kfvOp(key_op_fvs_tuple);
    status = validateRouteEntry(route_entry, operation);
    if (!status.ok()) {
      SWSS_LOG_ERROR(
          "Validation failed for Route APP DB entry with key  %s: %s",
          QuotedVar(table_name + ":" + key).c_str(), status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }
    route_entry_list.insert(route_entry.route_entry_key);

    bool update = (getRouteEntry(route_entry.route_entry_key) != nullptr);
    if (prev_op == "") {
      prev_op = operation;
      prev_update = update;
    }
    // Process the entries if the operation type changes.
    if (operation != prev_op || update != prev_update) {
      status =
          processRouteEntries(route_list, tuple_list, prev_op, prev_update);
      route_list.clear();
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
      route_list.push_back(route_entry);
      tuple_list.push_back(key_op_fvs_tuple);
    }
  }

  if (!route_list.empty()) {
    auto rc = processRouteEntries(route_list, tuple_list, prev_op, prev_update);
    if (!rc.ok()) {
      status = rc;
    }
  }
  drainWithNotExecuted();
  return status;
}

std::string RouteManager::verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
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
    if (table_name != APP_P4RT_IPV4_TABLE_NAME && table_name != APP_P4RT_IPV6_TABLE_NAME)
    {
        return std::string("Invalid key: ") + key;
    }

    ReturnCode status;
    auto app_db_entry_or = deserializeRouteEntry(key_content, tuple, table_name);
    if (!app_db_entry_or.ok())
    {
        status = app_db_entry_or.status();
        std::stringstream msg;
        msg << "Unable to deserialize key " << QuotedVar(key) << ": " << status.message();
        return msg.str();
    }
    auto &app_db_entry = *app_db_entry_or;

    auto *route_entry = getRouteEntry(app_db_entry.route_entry_key);
    if (route_entry == nullptr)
    {
        std::stringstream msg;
        msg << "No entry found with key " << QuotedVar(key);
        return msg.str();
    }

    std::string cache_result = verifyStateCache(app_db_entry, route_entry);
    std::string asic_db_result = verifyStateAsicDb(route_entry);
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

std::string RouteManager::verifyStateCache(const P4RouteEntry &app_db_entry, const P4RouteEntry *route_entry)
{
    ReturnCode status = validateRouteEntry(app_db_entry, SET_COMMAND);
    if (!status.ok())
    {
        std::stringstream msg;
        msg << "Validation failed for route DB entry with key " << QuotedVar(app_db_entry.route_entry_key) << ": "
            << status.message();
        return msg.str();
    }

    if (route_entry->route_entry_key != app_db_entry.route_entry_key)
    {
        std::stringstream msg;
        msg << "Route entry " << QuotedVar(app_db_entry.route_entry_key) << " does not match internal cache "
            << QuotedVar(route_entry->route_entry_key) << " in route manager.";
        return msg.str();
    }
    if (route_entry->vrf_id != app_db_entry.vrf_id)
    {
        std::stringstream msg;
        msg << "Route entry " << QuotedVar(app_db_entry.route_entry_key) << " with VRF "
            << QuotedVar(app_db_entry.vrf_id) << " does not match internal cache " << QuotedVar(route_entry->vrf_id)
            << " in route manager.";
        return msg.str();
    }
    if (route_entry->route_prefix.to_string() != app_db_entry.route_prefix.to_string())
    {
        std::stringstream msg;
        msg << "Route entry " << QuotedVar(app_db_entry.route_entry_key) << " with route prefix "
            << app_db_entry.route_prefix.to_string() << " does not match internal cache "
            << route_entry->route_prefix.to_string() << " in route manager.";
        return msg.str();
    }
    if (route_entry->action != app_db_entry.action)
    {
        std::stringstream msg;
        msg << "Route entry " << QuotedVar(app_db_entry.route_entry_key) << " with action "
            << QuotedVar(app_db_entry.action) << " does not match internal cache " << QuotedVar(route_entry->action)
            << " in route manager.";
        return msg.str();
    }
    if (route_entry->nexthop_id != app_db_entry.nexthop_id)
    {
        std::stringstream msg;
        msg << "Route entry " << QuotedVar(app_db_entry.route_entry_key) << " with nexthop ID "
            << QuotedVar(app_db_entry.nexthop_id) << " does not match internal cache "
            << QuotedVar(route_entry->nexthop_id) << " in route manager.";
        return msg.str();
    }
    if (route_entry->wcmp_group != app_db_entry.wcmp_group)
    {
        std::stringstream msg;
        msg << "Route entry " << QuotedVar(app_db_entry.route_entry_key) << " with WCMP group "
            << QuotedVar(app_db_entry.wcmp_group) << " does not match internal cache "
            << QuotedVar(route_entry->wcmp_group) << " in route manager.";
        return msg.str();
    }
    if (route_entry->route_metadata != app_db_entry.route_metadata)
    {
        std::stringstream msg;
        msg << "Route entry " << QuotedVar(app_db_entry.route_entry_key) << " with metadata "
            << QuotedVar(app_db_entry.route_metadata) << " does not match internal cache "
            << QuotedVar(route_entry->route_metadata) << " in route manager.";
        return msg.str();
    }

    return "";
}

std::string RouteManager::verifyStateAsicDb(const P4RouteEntry *route_entry)
{
    std::vector<sai_attribute_t> exp_attrs;
    std::vector<sai_attribute_t> opt_attrs;
    sai_attribute_t attr;

    if (route_entry->action == p4orch::kDrop)
    {
        attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        attr.value.s32 = SAI_PACKET_ACTION_DROP;
        exp_attrs.push_back(attr);
    }
    else if (route_entry->action == p4orch::kTrap)
    {
        attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        attr.value.s32 = SAI_PACKET_ACTION_TRAP;
        exp_attrs.push_back(attr);
    }
    else if (route_entry->action == p4orch::kSetMetadataAndDrop)
    {
        attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        attr.value.s32 = SAI_PACKET_ACTION_DROP;
        exp_attrs.push_back(attr);
        attr.id = SAI_ROUTE_ENTRY_ATTR_META_DATA;
        attr.value.u32 = swss::to_uint<uint32_t>(route_entry->route_metadata);
        exp_attrs.push_back(attr);
    }
    else
    {
        attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        attr.value.oid = getNexthopOid(*route_entry, *m_p4OidMapper);
        exp_attrs.push_back(attr);
        if (route_entry->action == p4orch::kSetNexthopIdAndMetadata ||
            route_entry->action == p4orch::kSetWcmpGroupIdAndMetadata)
        {
            attr.id = SAI_ROUTE_ENTRY_ATTR_META_DATA;
            attr.value.u32 = swss::to_uint<uint32_t>(route_entry->route_metadata);
            exp_attrs.push_back(attr);
        }
    }

    if (route_entry->action == p4orch::kDrop || route_entry->action == p4orch::kTrap)
    {
        attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        attr.value.oid = SAI_NULL_OBJECT_ID;
        opt_attrs.push_back(attr);
        attr.id = SAI_ROUTE_ENTRY_ATTR_META_DATA;
        attr.value.u32 = 0;
        opt_attrs.push_back(attr);
    }
    else if (route_entry->action == p4orch::kSetMetadataAndDrop)
    {
        attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        attr.value.oid = SAI_NULL_OBJECT_ID;
        opt_attrs.push_back(attr);
    }
    else
    {
        attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
        opt_attrs.push_back(attr);
        if (route_entry->action != p4orch::kSetNexthopIdAndMetadata &&
            route_entry->action != p4orch::kSetWcmpGroupIdAndMetadata)
        {
            attr.id = SAI_ROUTE_ENTRY_ATTR_META_DATA;
            attr.value.u32 = 0;
            opt_attrs.push_back(attr);
        }
    }

    std::vector<swss::FieldValueTuple> exp = saimeta::SaiAttributeList::serialize_attr_list(
        SAI_OBJECT_TYPE_ROUTE_ENTRY, (uint32_t)exp_attrs.size(), exp_attrs.data(), /*countOnly=*/false);
    std::vector<swss::FieldValueTuple> opt = saimeta::SaiAttributeList::serialize_attr_list(
        SAI_OBJECT_TYPE_ROUTE_ENTRY, (uint32_t)opt_attrs.size(), opt_attrs.data(), /*countOnly=*/false);

    swss::DBConnector db("ASIC_DB", 0);
    swss::Table table(&db, "ASIC_STATE");
    std::string key = sai_serialize_object_type(SAI_OBJECT_TYPE_ROUTE_ENTRY) +
                      ":" +
                      sai_serialize_route_entry(prepareSaiEntry(*route_entry));
    std::vector<swss::FieldValueTuple> values;
    if (!table.get(key, values))
    {
        return std::string("ASIC DB key not found ") + key;
    }

    return verifyAttrs(values, exp, opt, /*allow_unknown=*/false);
}
