#include "p4orch/mirror_session_manager.h"

#include <map>
#include <nlohmann/json.hpp>

#include "SaiAttributeList.h"
#include "dbconnector.h"
#include "p4orch/p4orch_util.h"
#include "portsorch.h"
#include "sai_serialize.h"
#include "swss/logger.h"
#include "swssnet.h"
#include "table.h"

using ::p4orch::kTableKeyDelimiter;

extern PortsOrch *gPortsOrch;
extern sai_mirror_api_t *sai_mirror_api;
extern sai_object_id_t gSwitchId;

namespace p4orch
{

ReturnCode MirrorSessionManager::getSaiObject(const std::string &json_key, sai_object_type_t &object_type,
                                              std::string &object_key)
{
    std::string value;

    try
    {
        nlohmann::json j = nlohmann::json::parse(json_key);
        if (j.find(prependMatchField(p4orch::kMirrorSessionId)) != j.end())
        {
            value = j.at(prependMatchField(p4orch::kMirrorSessionId)).get<std::string>();
            object_key = KeyGenerator::generateMirrorSessionKey(value);
            object_type = SAI_OBJECT_TYPE_MIRROR_SESSION;
            return ReturnCode();
        }
        else
        {
            SWSS_LOG_ERROR("%s match parameter absent: required for dependent object query", p4orch::kMirrorSessionId);
        }
    }
    catch (std::exception &ex)
    {
        SWSS_LOG_ERROR("json_key parse error");
    }

    return StatusCode::SWSS_RC_INVALID_PARAM;
}

void MirrorSessionManager::enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry)
{
    SWSS_LOG_ENTER();
    m_entries.push_back(entry);
}

void MirrorSessionManager::drainWithNotExecuted() {
  drainMgmtWithNotExecuted(m_entries, m_publisher);
}

ReturnCode MirrorSessionManager::drain() {
  SWSS_LOG_ENTER();

  ReturnCode status;
  while (!m_entries.empty()) {
    auto key_op_fvs_tuple = m_entries.front();
    m_entries.pop_front();
    std::string table_name;
    std::string key;
    parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &key);
    const std::vector<swss::FieldValueTuple>& attributes =
        kfvFieldsValues(key_op_fvs_tuple);

    auto app_db_entry_or =
        deserializeP4MirrorSessionAppDbEntry(key, attributes);
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

    const std::string mirror_session_key =
        KeyGenerator::generateMirrorSessionKey(app_db_entry.mirror_session_id);

    // Fulfill the operation.
    const std::string& operation = kfvOp(key_op_fvs_tuple);
    status = validateMirrorSessionEntry(app_db_entry, operation);
    if (!status.ok()) {
      SWSS_LOG_ERROR(
          "Validation failed for mirror session APP DB entry with key  %s: %s",
          QuotedVar(table_name + ":" + key).c_str(), status.message().c_str());
      m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple),
                           kfvFieldsValues(key_op_fvs_tuple), status,
                           /*replace=*/true);
      break;
    }

    if (operation == SET_COMMAND) {
      auto* mirror_session_entry = getMirrorSessionEntry(mirror_session_key);
      if (mirror_session_entry == nullptr) {
        // Create new mirror session.
        status = processAddRequest(app_db_entry);
      } else {
        // Modify existing mirror session.
        status = processUpdateRequest(app_db_entry, mirror_session_entry);
      }
    } else if (operation == DEL_COMMAND) {
      // Delete mirror session.
      status = processDeleteRequest(mirror_session_key);
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

ReturnCodeOr<std::vector<sai_attribute_t>> prepareSaiAttrs(
    const P4MirrorSessionEntry& mirror_session_entry) {
  swss::Port port;
  if (!gPortsOrch->getPort(mirror_session_entry.port, port)) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                         << "Failed to get port info for port "
                         << QuotedVar(mirror_session_entry.port));
  }
  if (port.m_type != Port::Type::PHY) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                         << "Port " << QuotedVar(mirror_session_entry.port)
                         << "'s type " << port.m_type
                         << " is not physical and is invalid as destination "
                            "port for mirror packet.");
  }

  std::vector<sai_attribute_t> attrs;
  sai_attribute_t attr;

  if (mirror_session_entry.action == p4orch::kMirrorAsIpv4Erspan) {
    attr.id = SAI_MIRROR_SESSION_ATTR_MONITOR_PORT;
    attr.value.oid = port.m_port_id;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_TYPE;
    attr.value.s32 = SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE;
    attr.value.s32 = SAI_ERSPAN_ENCAPSULATION_TYPE_MIRROR_L3_GRE_TUNNEL;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION;
    attr.value.u8 = MIRROR_SESSION_DEFAULT_IP_HDR_VER;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_TOS;
    attr.value.u8 = mirror_session_entry.tos;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_TTL;
    attr.value.u8 = mirror_session_entry.ttl;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS;
    swss::copy(attr.value.ipaddr, mirror_session_entry.src_ip);
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS;
    swss::copy(attr.value.ipaddr, mirror_session_entry.dst_ip);
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, mirror_session_entry.src_mac.getMac(),
           sizeof(sai_mac_t));
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, mirror_session_entry.dst_mac.getMac(),
           sizeof(sai_mac_t));
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE;
    attr.value.u16 = GRE_PROTOCOL_ERSPAN;
    attrs.push_back(attr);

  } else if (mirror_session_entry.action ==
             p4orch::kMirrorWithVlanTagAndIpfixEncapsulation) {
    #ifdef SAI_MIRROR_SESSION_TYPE_IPFIX
        attr.id = SAI_MIRROR_SESSION_ATTR_TYPE;
        attr.value.s32 = SAI_MIRROR_SESSION_TYPE_IPFIX;
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_IPFIX_ENCAPSULATION_TYPE;
        attr.value.s32 = SAI_IPFIX_ENCAPSULATION_TYPE_EXTENDED;
        attrs.push_back(attr);

    #endif
    attr.id = SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION;
    attr.value.u8 = 6;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_TOS;
    attr.value.u8 = 0;  // Required, but not programmable from P4.
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_MONITOR_PORT;
    attr.value.oid = port.m_port_id;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, mirror_session_entry.src_mac.getMac(),
           sizeof(sai_mac_t));
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, mirror_session_entry.dst_mac.getMac(),
           sizeof(sai_mac_t));
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_TPID;
    // Required, but not programmable from P4.
    attr.value.u16 = MIRROR_SESSION_DEFAULT_VLAN_TPID;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_ID;
    attr.value.u16 = mirror_session_entry.vlan_id;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS;
    swss::copy(attr.value.ipaddr, mirror_session_entry.src_ip);
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS;
    swss::copy(attr.value.ipaddr, mirror_session_entry.dst_ip);
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_UDP_SRC_PORT;
    attr.value.u16 = mirror_session_entry.udp_src_port;
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_UDP_DST_PORT;
    attr.value.u16 = mirror_session_entry.udp_dst_port;
    attrs.push_back(attr);
  }

  return attrs;
}

ReturnCodeOr<P4MirrorSessionAppDbEntry> MirrorSessionManager::deserializeP4MirrorSessionAppDbEntry(
    const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
{
    SWSS_LOG_ENTER();

    P4MirrorSessionAppDbEntry app_db_entry = {};

    try
    {
        nlohmann::json j = nlohmann::json::parse(key);
        app_db_entry.mirror_session_id = j[prependMatchField(p4orch::kMirrorSessionId)];
    }
    catch (std::exception &ex)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Failed to deserialize mirror session id";
    }

    for (const auto &it : attributes)
    {
        const auto &field = fvField(it);
        const auto &value = fvValue(it);
        if (field == prependParamField(p4orch::kPort) ||
            field == prependParamField(p4orch::kMonitorPort))
        {
            swss::Port port;
            if (!gPortsOrch->getPort(value, port))
            {
                return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                       << "Failed to get port info for port " << QuotedVar(value);
            }
            if (port.m_type != Port::Type::PHY)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Port " << QuotedVar(value) << "'s type " << port.m_type
                       << " is not physical and is invalid as destination port for "
                          "mirror packet.";
            }
            app_db_entry.port = value;
            app_db_entry.has_port = true;

        } else if (field == prependParamField(p4orch::kMonitorFailoverPort)) {
      swss::Port failover_port;
      if (!gPortsOrch->getPort(value, failover_port)) {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
               << "Failed to get failover_port info for port "
               << QuotedVar(value);
      }
      if (failover_port.m_type != Port::Type::PHY) {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Failover port " << QuotedVar(value) << "'s type "
               << failover_port.m_type
               << " is not physical and is invalid as destination port for "
                  "mirror packet.";
      }
      app_db_entry.failover_port = value;
      app_db_entry.has_failover_port = true;
    } else if (field == prependParamField(p4orch::kSrcIp) ||
               field == prependParamField(p4orch::kMirrorEncapSrcIp)) {
            try
            {
                app_db_entry.src_ip = swss::IpAddress(value);
                app_db_entry.has_src_ip = true;
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid IP address " << QuotedVar(value) << " of field " << QuotedVar(field);
            }
        } else if (field == prependParamField(p4orch::kDstIp) ||
                   field == prependParamField(p4orch::kMirrorEncapDstIp)) {
            try
            {
                app_db_entry.dst_ip = swss::IpAddress(value);
                app_db_entry.has_dst_ip = true;
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid IP address " << QuotedVar(value) << " of field " << QuotedVar(field);
            }
        }
        else if (field == prependParamField(p4orch::kSrcMac) ||
                 field == prependParamField(p4orch::kMirrorEncapSrcMac))
        {
            try
            {
                app_db_entry.src_mac = swss::MacAddress(value);
                app_db_entry.has_src_mac = true;
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid MAC address " << QuotedVar(value) << " of field " << QuotedVar(field);
            }
        }
        else if (field == prependParamField(p4orch::kDstMac) ||
                 field == prependParamField(p4orch::kMirrorEncapDstMac))
        {
            try
            {
                app_db_entry.dst_mac = swss::MacAddress(value);
                app_db_entry.has_dst_mac = true;
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid MAC address " << QuotedVar(value) << " of field " << QuotedVar(field);
            }
        }
        else if (field == prependParamField(p4orch::kTtl))
        {
            try
            {
                app_db_entry.ttl = static_cast<uint8_t>(std::stoul(value, 0, /*base=*/16));
                app_db_entry.has_ttl = true;
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid TTL " << QuotedVar(value) << " of field " << QuotedVar(field);
            }
        }
        else if (field == prependParamField(p4orch::kTos))
        {
            try
            {
                app_db_entry.tos = static_cast<uint8_t>(std::stoul(value, 0, /*base=*/16));
                app_db_entry.has_tos = true;
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid TOS " << QuotedVar(value) << " of field " << QuotedVar(field);
            }
        } else if (field == prependParamField(p4orch::kMirrorEncapVlanId)) {
      try {
        app_db_entry.vlan_id =
            static_cast<uint16_t>(std::stoul(value, 0, /*base=*/16));
        app_db_entry.has_vlan_id = true;
      } catch (std::exception& ex) {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Invalid VLAN ID " << QuotedVar(value) << " of field "
               << QuotedVar(field);
      }
    } else if (field == prependParamField(p4orch::kMirrorEncapUdpSrcPort)) {
      try {
        app_db_entry.udp_src_port =
            static_cast<uint16_t>(std::stoul(value, 0, /*base=*/16));
        app_db_entry.has_udp_src_port = true;
      } catch (std::exception& ex) {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Invalid UDP source port " << QuotedVar(value) << " of field "
               << QuotedVar(field);
      }
    } else if (field == prependParamField(p4orch::kMirrorEncapUdpDstPort)) {
      try {
        app_db_entry.udp_dst_port =
            static_cast<uint16_t>(std::stoul(value, 0, /*base=*/16));
        app_db_entry.has_udp_dst_port = true;
      } catch (std::exception& ex) {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Invalid UDP dst port " << QuotedVar(value) << " of field "
               << QuotedVar(field);
      }
    }
    else if (field == p4orch::kAction)
      {
        if (value == p4orch::kMirrorAsIpv4Erspan ||
            value == p4orch::kMirrorWithVlanTagAndIpfixEncapsulation) {
          app_db_entry.action = value;
          app_db_entry.has_action = true;
      } else {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Action value " << QuotedVar(value) << " is not mirror_as_ipv4_erspan.";
            }
        }
        else if (field != p4orch::kControllerMetadata)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Unexpected field " << QuotedVar(field) << " in table entry";
        }
    }

    return app_db_entry;
}

P4MirrorSessionEntry *MirrorSessionManager::getMirrorSessionEntry(const std::string &mirror_session_key)
{
    auto it = m_mirrorSessionTable.find(mirror_session_key);

    if (it == m_mirrorSessionTable.end())
    {
        return nullptr;
    }
    else
    {
        return &it->second;
    }
}

// Performs mirror session entry validation.
ReturnCode MirrorSessionManager::validateMirrorSessionEntry(
    const P4MirrorSessionAppDbEntry& mirror_session_entry,
    const std::string& operation)
{
    SWSS_LOG_ENTER();

    // Confirm match fields are populated.
  if (mirror_session_entry.mirror_session_id.empty()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No match field mirror_session_id provided";
  }

  if (operation == SET_COMMAND) {
    return validateSetMirrorSessionEntry(mirror_session_entry);
  } else if (operation == DEL_COMMAND) {
    return validateDelMirrorSessionEntry(mirror_session_entry);
  }
  return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
         << "Unknown operation type " << QuotedVar(operation);
}

ReturnCode MirrorSessionManager::validateSetMirrorSessionEntry(
    const P4MirrorSessionAppDbEntry& mirror_session_entry) {
  SWSS_LOG_ENTER();
  // Verify action parameters exist.
  if (!mirror_session_entry.has_action) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No action provided for mirror session ID "
           << QuotedVar(mirror_session_entry.mirror_session_id);
  }

  if (!mirror_session_entry.has_port) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No port provided for mirror session ID "
           << QuotedVar(mirror_session_entry.mirror_session_id);
  }

  if (!mirror_session_entry.has_failover_port &&
      mirror_session_entry.action ==
          p4orch::kMirrorWithVlanTagAndIpfixEncapsulation) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No failover port provided for mirror session ID "
           << QuotedVar(mirror_session_entry.mirror_session_id);
  }

  if (!mirror_session_entry.has_src_ip) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No source IP provided for mirror session ID "
           << QuotedVar(mirror_session_entry.mirror_session_id);
  }
  if (mirror_session_entry.action ==
          p4orch::kMirrorWithVlanTagAndIpfixEncapsulation &&
      mirror_session_entry.src_ip.isV4()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Source IP address "
           << QuotedVar(mirror_session_entry.src_ip.to_string())
           << " for mirror session action "
           << p4orch::kMirrorWithVlanTagAndIpfixEncapsulation
           << " must be an IPv6 address.";
  }

  if (!mirror_session_entry.has_dst_ip) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No destination IP provided for mirror session ID "
           << QuotedVar(mirror_session_entry.mirror_session_id);
  }
  if (mirror_session_entry.action ==
          p4orch::kMirrorWithVlanTagAndIpfixEncapsulation &&
      mirror_session_entry.dst_ip.isV4()) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "Destination IP address "
           << QuotedVar(mirror_session_entry.dst_ip.to_string())
           << " for mirror session action "
           << p4orch::kMirrorWithVlanTagAndIpfixEncapsulation
           << " must be an IPv6 address.";
  }

  if (!mirror_session_entry.has_src_mac) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No source MAC provided for mirror session ID "
           << QuotedVar(mirror_session_entry.mirror_session_id);
  }

  if (!mirror_session_entry.has_dst_mac) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No source MAC provided for mirror session ID "
           << QuotedVar(mirror_session_entry.mirror_session_id);
  }
  if (!mirror_session_entry.has_ttl &&
      mirror_session_entry.action == p4orch::kMirrorAsIpv4Erspan) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No TTL provided for mirror session ID "
           << QuotedVar(mirror_session_entry.mirror_session_id);
  }

    if (!mirror_session_entry.has_tos &&
        mirror_session_entry.action == p4orch::kMirrorAsIpv4Erspan) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No TOS provided for mirror session ID "
           << QuotedVar(mirror_session_entry.mirror_session_id);
  }

  if (!mirror_session_entry.has_vlan_id &&
      mirror_session_entry.action ==
          p4orch::kMirrorWithVlanTagAndIpfixEncapsulation) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No VLAN ID provided for mirror session ID "
           << QuotedVar(mirror_session_entry.mirror_session_id);
  }

  if (!mirror_session_entry.has_udp_src_port &&
      mirror_session_entry.action ==
          p4orch::kMirrorWithVlanTagAndIpfixEncapsulation) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No UDP source port provided for mirror session ID "
           << QuotedVar(mirror_session_entry.mirror_session_id);
  }
  if (!mirror_session_entry.has_udp_dst_port &&
      mirror_session_entry.action ==
          p4orch::kMirrorWithVlanTagAndIpfixEncapsulation) {
    return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
           << "No UDP destination port provided for mirror session ID "
           << QuotedVar(mirror_session_entry.mirror_session_id);
  }

  // Check the existence of the mirror session in mirror manager and centralized
  // mapper if this is an update operation.
  const std::string key = KeyGenerator::generateMirrorSessionKey(
      mirror_session_entry.mirror_session_id);
  const P4MirrorSessionEntry* mirror_session_entry_ptr =
      getMirrorSessionEntry(key);
  bool is_update = mirror_session_entry_ptr != nullptr;

  if (is_update) {
    if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_MIRROR_SESSION, key)) {
      RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
          "Mirror session with key " << QuotedVar(key)
                                     << " doesn't exist in centralized mapper");
    }
    // We do not support switching actions, since the Erspan action is
    // deprecated.
    if (mirror_session_entry.action != mirror_session_entry_ptr->action) {
      return ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
             << "Mirror session entry with mirror_session_id "
             << QuotedVar(mirror_session_entry.mirror_session_id)
             << " cannot currently change the action from "
             << QuotedVar(mirror_session_entry_ptr->action) << " to "
             << QuotedVar(mirror_session_entry.action);
    }
  } else {
    if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_MIRROR_SESSION, key)) {
      RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
          "Mirror session with key "
          << QuotedVar(key) << " already exists in centralized mapper");
    }
    }

    return ReturnCode();
}

ReturnCode MirrorSessionManager::validateDelMirrorSessionEntry(
    const P4MirrorSessionAppDbEntry& mirror_session_entry)
{
    SWSS_LOG_ENTER();
    const std::string key = KeyGenerator::generateMirrorSessionKey(
        mirror_session_entry.mirror_session_id);
    const P4MirrorSessionEntry* mirror_session_entry_ptr =
        getMirrorSessionEntry(key);
    if (mirror_session_entry_ptr == nullptr) {
      return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
             << "Mirror session with key " << QuotedVar(key)
             << " does not exist in mirror session manager";
    }

    // Confirm the mirror session object ID exists in central mapper.
    if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_MIRROR_SESSION, key)) {
      RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
          "Mirror session with key " << QuotedVar(key)
                                     << " does not exist in the central map");
    }

    // Check if there is anything referring to the mirror session before deletion.
    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_MIRROR_SESSION, key,
                                  &ref_count)) {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL(
        "Failed to get reference count for mirror session " << QuotedVar(key));
  }
  if (ref_count > 0) {
    LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_IN_USE)
                         << "Mirror session " << QuotedVar(key)
                         << " referenced by other objects (ref_count = "
                         << ref_count);
    }
    return ReturnCode();
}

ReturnCode MirrorSessionManager::processAddRequest(
    const P4MirrorSessionAppDbEntry& app_db_entry) {
  SWSS_LOG_ENTER();

  ReturnCode status;

  P4MirrorSessionEntry mirror_session_entry(
      KeyGenerator::generateMirrorSessionKey(app_db_entry.mirror_session_id),
      /*mirror_session_oid=*/0, app_db_entry.mirror_session_id,
      app_db_entry.action, app_db_entry.port, app_db_entry.src_ip,
      app_db_entry.dst_ip, app_db_entry.src_mac, app_db_entry.dst_mac,
      app_db_entry.ttl, app_db_entry.tos, app_db_entry.failover_port,
      app_db_entry.vlan_id, app_db_entry.udp_src_port,
      app_db_entry.udp_dst_port);
  return status = createMirrorSession(std::move(mirror_session_entry));
}

ReturnCode MirrorSessionManager::createMirrorSession(
    P4MirrorSessionEntry mirror_session_entry) {
  SWSS_LOG_ENTER();
    // Prepare attributes for the SAI creation call.
    ASSIGN_OR_RETURN(std::vector<sai_attribute_t> attrs,
                     prepareSaiAttrs(mirror_session_entry));

    // Call SAI API.
    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_mirror_api->create_mirror_session(&mirror_session_entry.mirror_session_oid, gSwitchId,
                                              (uint32_t)attrs.size(), attrs.data()),
        "Failed to create mirror session " << QuotedVar(mirror_session_entry.mirror_session_key));

    // On successful creation, increment ref count.
    gPortsOrch->increasePortRefCount(mirror_session_entry.port);

    // Add the key to OID map to centralized mapper.
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_MIRROR_SESSION, mirror_session_entry.mirror_session_key,
                          mirror_session_entry.mirror_session_oid);

    // Add created entry to internal table.
    m_mirrorSessionTable.emplace(mirror_session_entry.mirror_session_key, mirror_session_entry);

    return ReturnCode();
}

ReturnCode MirrorSessionManager::processUpdateRequest(const P4MirrorSessionAppDbEntry &app_db_entry,
                                                      P4MirrorSessionEntry *existing_mirror_session_entry)
{
    SWSS_LOG_ENTER();
    P4MirrorSessionEntry mirror_session_entry_before_update(*existing_mirror_session_entry);

    // Because SAI mirror set API sets attr one at a time, it is possible attr
    // updates fail in the middle. Up on failure, all successful operations need
    // to be undone.
    ReturnCode ret;
    bool update_fail_in_middle = false;
    if (!update_fail_in_middle && app_db_entry.has_port)
    {
        ret = setPort(app_db_entry.port, existing_mirror_session_entry);
        if (!ret.ok())
            update_fail_in_middle = true;
    }
    if (!update_fail_in_middle && app_db_entry.has_src_ip)
    {
        ret = setSrcIp(app_db_entry.src_ip, existing_mirror_session_entry);
        if (!ret.ok())
            update_fail_in_middle = true;
    }
    if (!update_fail_in_middle && app_db_entry.has_dst_ip)
    {
        ret = setDstIp(app_db_entry.dst_ip, existing_mirror_session_entry);
        if (!ret.ok())
            update_fail_in_middle = true;
    }
    if (!update_fail_in_middle && app_db_entry.has_src_mac)
    {
        ret = setSrcMac(app_db_entry.src_mac, existing_mirror_session_entry);
        if (!ret.ok())
            update_fail_in_middle = true;
    }
    if (!update_fail_in_middle && app_db_entry.has_dst_mac)
    {
        ret = setDstMac(app_db_entry.dst_mac, existing_mirror_session_entry);
        if (!ret.ok())
            update_fail_in_middle = true;
    }
    if (!update_fail_in_middle && app_db_entry.has_ttl)
    {
        ret = setTtl(app_db_entry.ttl, existing_mirror_session_entry);
        if (!ret.ok())
            update_fail_in_middle = true;
    }
    if (!update_fail_in_middle && app_db_entry.has_tos)
    {
        ret = setTos(app_db_entry.tos, existing_mirror_session_entry);
        if (!ret.ok())
            update_fail_in_middle = true;
    }
    if (!update_fail_in_middle && app_db_entry.has_vlan_id) {
        ret = setVlanId(app_db_entry.vlan_id, existing_mirror_session_entry);
        if (!ret.ok()) update_fail_in_middle = true;
    }
    if (!update_fail_in_middle && app_db_entry.has_udp_src_port) {
        ret =
            setUdpSrcPort(app_db_entry.udp_src_port, existing_mirror_session_entry);
        if (!ret.ok()) update_fail_in_middle = true;
    }
    if (!update_fail_in_middle && app_db_entry.has_udp_dst_port) {
        ret =
            setUdpDstPort(app_db_entry.udp_dst_port, existing_mirror_session_entry);
        if (!ret.ok()) update_fail_in_middle = true;
    }

    if (update_fail_in_middle)
    {
        ReturnCode status = setMirrorSessionEntry(mirror_session_entry_before_update, existing_mirror_session_entry);
        if (!status.ok())
        {
            ret << "Failed to recover mirror session entry to the state before "
                   "update operation.";
            SWSS_RAISE_CRITICAL_STATE("Failed to recover mirror session entry to the state before update "
                                      "operation.");
        }
    }

    return ret;
}

ReturnCode MirrorSessionManager::setPort(const std::string &new_port_name,
                                         P4MirrorSessionEntry *existing_mirror_session_entry)
{
    SWSS_LOG_ENTER();

    if (new_port_name == existing_mirror_session_entry->port)
    {
        return ReturnCode();
    }

    swss::Port new_port;
    if (!gPortsOrch->getPort(new_port_name, new_port))
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "Failed to get port info for port " << QuotedVar(new_port_name));
    }
    if (new_port.m_type != Port::Type::PHY)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << "Port " << QuotedVar(new_port.m_alias) << "'s type " << new_port.m_type
                             << " is not physical and is invalid as destination "
                                "port for mirror packet.");
    }

    sai_attribute_t attr;
    attr.id = SAI_MIRROR_SESSION_ATTR_MONITOR_PORT;
    attr.value.oid = new_port.m_port_id;

    // Call SAI API.
    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_mirror_api->set_mirror_session_attribute(existing_mirror_session_entry->mirror_session_oid, &attr),
        "Failed to set new port " << QuotedVar(new_port.m_alias) << " for mirror session "
                                  << QuotedVar(existing_mirror_session_entry->mirror_session_key));

    // Update ref count.
    gPortsOrch->decreasePortRefCount(existing_mirror_session_entry->port);
    gPortsOrch->increasePortRefCount(new_port.m_alias);

    // Update the entry in table
    existing_mirror_session_entry->port = new_port_name;

    return ReturnCode();
}

ReturnCode MirrorSessionManager::setSrcIp(const swss::IpAddress &new_src_ip,
                                          P4MirrorSessionEntry *existing_mirror_session_entry)
{
    SWSS_LOG_ENTER();

    if (new_src_ip == existing_mirror_session_entry->src_ip)
    {
        return ReturnCode();
    }

    sai_attribute_t attr;
    attr.id = SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS;
    swss::copy(attr.value.ipaddr, new_src_ip);

    // Call SAI API.
    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_mirror_api->set_mirror_session_attribute(existing_mirror_session_entry->mirror_session_oid, &attr),
        "Failed to set new src_ip " << QuotedVar(new_src_ip.to_string()) << " for mirror session "
                                    << QuotedVar(existing_mirror_session_entry->mirror_session_key));

    // Update the entry in table
    existing_mirror_session_entry->src_ip = new_src_ip;

    return ReturnCode();
}

ReturnCode MirrorSessionManager::setDstIp(const swss::IpAddress &new_dst_ip,
                                          P4MirrorSessionEntry *existing_mirror_session_entry)
{
    SWSS_LOG_ENTER();

    if (new_dst_ip == existing_mirror_session_entry->dst_ip)
    {
        return ReturnCode();
    }

    sai_attribute_t attr;
    attr.id = SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS;
    swss::copy(attr.value.ipaddr, new_dst_ip);

    // Call SAI API.
    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_mirror_api->set_mirror_session_attribute(existing_mirror_session_entry->mirror_session_oid, &attr),
        "Failed to set new dst_ip " << QuotedVar(new_dst_ip.to_string()) << " for mirror session "
                                    << QuotedVar(existing_mirror_session_entry->mirror_session_key));

    // Update the entry in table
    existing_mirror_session_entry->dst_ip = new_dst_ip;

    return ReturnCode();
}

ReturnCode MirrorSessionManager::setSrcMac(const swss::MacAddress &new_src_mac,
                                           P4MirrorSessionEntry *existing_mirror_session_entry)
{
    SWSS_LOG_ENTER();

    if (new_src_mac == existing_mirror_session_entry->src_mac)
    {
        return ReturnCode();
    }

    sai_attribute_t attr;
    attr.id = SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS;
    memcpy(attr.value.mac, new_src_mac.getMac(), sizeof(sai_mac_t));

    // Call SAI API.
    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_mirror_api->set_mirror_session_attribute(existing_mirror_session_entry->mirror_session_oid, &attr),
        "Failed to set new src_mac " << QuotedVar(new_src_mac.to_string()) << " for mirror session "
                                     << QuotedVar(existing_mirror_session_entry->mirror_session_key));

    // Update the entry in table
    existing_mirror_session_entry->src_mac = new_src_mac;

    return ReturnCode();
}

ReturnCode MirrorSessionManager::setDstMac(const swss::MacAddress &new_dst_mac,
                                           P4MirrorSessionEntry *existing_mirror_session_entry)
{
    SWSS_LOG_ENTER();

    if (new_dst_mac == existing_mirror_session_entry->dst_mac)
    {
        return ReturnCode();
    }

    sai_attribute_t attr;
    attr.id = SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, new_dst_mac.getMac(), sizeof(sai_mac_t));

    // Call SAI API.
    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_mirror_api->set_mirror_session_attribute(existing_mirror_session_entry->mirror_session_oid, &attr),
        "Failed to set new dst_mac " << QuotedVar(new_dst_mac.to_string()) << " for mirror session "
                                     << QuotedVar(existing_mirror_session_entry->mirror_session_key));

    // Update the entry in table
    existing_mirror_session_entry->dst_mac = new_dst_mac;

    return ReturnCode();
}

ReturnCode MirrorSessionManager::setTtl(uint8_t new_ttl, P4MirrorSessionEntry *existing_mirror_session_entry)
{
    SWSS_LOG_ENTER();

    if (new_ttl == existing_mirror_session_entry->ttl)
    {
        return ReturnCode();
    }

    sai_attribute_t attr;
    attr.id = SAI_MIRROR_SESSION_ATTR_TTL;
    attr.value.u8 = new_ttl;

    // Call SAI API.
    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_mirror_api->set_mirror_session_attribute(existing_mirror_session_entry->mirror_session_oid, &attr),
        "Failed to set new ttl " << QuotedVar(std::to_string(new_ttl)) << " for mirror session "
                                 << QuotedVar(existing_mirror_session_entry->mirror_session_key));

    // Update the entry in table
    existing_mirror_session_entry->ttl = new_ttl;

    return ReturnCode();
}

ReturnCode MirrorSessionManager::setTos(uint8_t new_tos, P4MirrorSessionEntry *existing_mirror_session_entry)
{
    SWSS_LOG_ENTER();

    if (new_tos == existing_mirror_session_entry->tos)
    {
        return ReturnCode();
    }

    sai_attribute_t attr;
    attr.id = SAI_MIRROR_SESSION_ATTR_TOS;
    attr.value.u8 = new_tos;

    // Call SAI API.
    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_mirror_api->set_mirror_session_attribute(existing_mirror_session_entry->mirror_session_oid, &attr),
        "Failed to set new tos " << QuotedVar(std::to_string(new_tos)) << " for mirror session "
                                 << QuotedVar(existing_mirror_session_entry->mirror_session_key));

    // Update the entry in table
    existing_mirror_session_entry->tos = new_tos;

    return ReturnCode();
}

ReturnCode MirrorSessionManager::setVlanId(
    uint16_t new_vlan_id, P4MirrorSessionEntry* existing_mirror_session_entry) {
  SWSS_LOG_ENTER();

  if (new_vlan_id == existing_mirror_session_entry->vlan_id) {
    return ReturnCode();
  }

  sai_attribute_t attr;
  attr.id = SAI_MIRROR_SESSION_ATTR_VLAN_ID;
  attr.value.u16 = new_vlan_id;

  // Call SAI API.
  CHECK_ERROR_AND_LOG_AND_RETURN(
      sai_mirror_api->set_mirror_session_attribute(
          existing_mirror_session_entry->mirror_session_oid, &attr),
      "Failed to set new VLAN ID "
          << QuotedVar(std::to_string(new_vlan_id)) << " for mirror session "
          << QuotedVar(existing_mirror_session_entry->mirror_session_key));

  // Update the entry in table
  existing_mirror_session_entry->vlan_id = new_vlan_id;

  return ReturnCode();
}

ReturnCode MirrorSessionManager::setUdpSrcPort(
    uint16_t new_udp_src_port,
    P4MirrorSessionEntry* existing_mirror_session_entry) {
  SWSS_LOG_ENTER();

  if (new_udp_src_port == existing_mirror_session_entry->udp_src_port) {
    return ReturnCode();
  }

  sai_attribute_t attr;
  attr.id = SAI_MIRROR_SESSION_ATTR_UDP_SRC_PORT;
  attr.value.u16 = new_udp_src_port;

  // Call SAI API.
  CHECK_ERROR_AND_LOG_AND_RETURN(
      sai_mirror_api->set_mirror_session_attribute(
          existing_mirror_session_entry->mirror_session_oid, &attr),
      "Failed to set new UDP source port "
          << QuotedVar(std::to_string(new_udp_src_port))
          << " for mirror session "
          << QuotedVar(existing_mirror_session_entry->mirror_session_key));

  // Update the entry in table
  existing_mirror_session_entry->udp_src_port = new_udp_src_port;

  return ReturnCode();
}

ReturnCode MirrorSessionManager::setUdpDstPort(
    uint16_t new_udp_dst_port,
    P4MirrorSessionEntry* existing_mirror_session_entry) {
  SWSS_LOG_ENTER();

  if (new_udp_dst_port == existing_mirror_session_entry->udp_dst_port) {
    return ReturnCode();
  }

  sai_attribute_t attr;
  attr.id = SAI_MIRROR_SESSION_ATTR_UDP_DST_PORT;
  attr.value.u16 = new_udp_dst_port;

  // Call SAI API.
  CHECK_ERROR_AND_LOG_AND_RETURN(
      sai_mirror_api->set_mirror_session_attribute(
          existing_mirror_session_entry->mirror_session_oid, &attr),
      "Failed to set new UDP destination port "
          << QuotedVar(std::to_string(new_udp_dst_port))
          << " for mirror session "
          << QuotedVar(existing_mirror_session_entry->mirror_session_key));

  // Update the entry in table
  existing_mirror_session_entry->udp_dst_port = new_udp_dst_port;

  return ReturnCode();
}

ReturnCode MirrorSessionManager::setMirrorSessionEntry(const P4MirrorSessionEntry &intent_mirror_session_entry,
                                                       P4MirrorSessionEntry *existing_mirror_session_entry)
{
    SWSS_LOG_ENTER();

    ReturnCode status;

    if (intent_mirror_session_entry.port != existing_mirror_session_entry->port)
    {
        status = setPort(intent_mirror_session_entry.port, existing_mirror_session_entry);
        if (!status.ok())
            return status;
    }
    if (intent_mirror_session_entry.src_ip != existing_mirror_session_entry->src_ip)
    {
        status = setSrcIp(intent_mirror_session_entry.src_ip, existing_mirror_session_entry);
        if (!status.ok())
            return status;
    }
    if (intent_mirror_session_entry.dst_ip != existing_mirror_session_entry->dst_ip)
    {
        status = setDstIp(intent_mirror_session_entry.dst_ip, existing_mirror_session_entry);
        if (!status.ok())
            return status;
    }
    if (intent_mirror_session_entry.src_mac != existing_mirror_session_entry->src_mac)
    {
        status = setSrcMac(intent_mirror_session_entry.src_mac, existing_mirror_session_entry);
        if (!status.ok())
            return status;
    }
    if (intent_mirror_session_entry.dst_mac != existing_mirror_session_entry->dst_mac)
    {
        status = setDstMac(intent_mirror_session_entry.dst_mac, existing_mirror_session_entry);
        if (!status.ok())
            return status;
    }
    if (intent_mirror_session_entry.ttl != existing_mirror_session_entry->ttl)
    {
        status = setTtl(intent_mirror_session_entry.ttl, existing_mirror_session_entry);
        if (!status.ok())
            return status;
    }
    if (intent_mirror_session_entry.tos != existing_mirror_session_entry->tos)
    {
        status = setTos(intent_mirror_session_entry.tos, existing_mirror_session_entry);
        if (!status.ok())
            return status;
    }
    if (intent_mirror_session_entry.vlan_id !=
        existing_mirror_session_entry->vlan_id) {
        status = setVlanId(intent_mirror_session_entry.vlan_id,
                           existing_mirror_session_entry);
        if (!status.ok()) return status;
    }
    if (intent_mirror_session_entry.udp_src_port !=
        existing_mirror_session_entry->udp_src_port) {
        status = setUdpSrcPort(intent_mirror_session_entry.udp_src_port,
                               existing_mirror_session_entry);
        if (!status.ok()) return status;
    }
    if (intent_mirror_session_entry.udp_dst_port !=
        existing_mirror_session_entry->udp_dst_port) {
        status = setUdpDstPort(intent_mirror_session_entry.udp_dst_port,
                               existing_mirror_session_entry);
        if (!status.ok()) return status;
    }

    return status;
}

ReturnCode MirrorSessionManager::processDeleteRequest(const std::string &mirror_session_key)
{
    SWSS_LOG_ENTER();

    const P4MirrorSessionEntry *mirror_session_entry = getMirrorSessionEntry(mirror_session_key);
    if (mirror_session_entry == nullptr)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "Mirror session with key " << QuotedVar(mirror_session_key)
                             << " does not exist in mirror session manager");
    }

    // Call SAI API.
    CHECK_ERROR_AND_LOG_AND_RETURN(sai_mirror_api->remove_mirror_session(mirror_session_entry->mirror_session_oid),
                                   "Failed to remove mirror session "
                                       << QuotedVar(mirror_session_entry->mirror_session_key));

    // On successful deletion, decrement ref count.
    gPortsOrch->decreasePortRefCount(mirror_session_entry->port);

    // Delete the key to OID map from centralized mapper.
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_MIRROR_SESSION, mirror_session_entry->mirror_session_key);

    // Delete entry from internal table.
    m_mirrorSessionTable.erase(mirror_session_entry->mirror_session_key);

    return ReturnCode();
}

std::string MirrorSessionManager::verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
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
    if (table_name != APP_P4RT_MIRROR_SESSION_TABLE_NAME)
    {
        return std::string("Invalid key: ") + key;
    }

    ReturnCode status;
    auto app_db_entry_or = deserializeP4MirrorSessionAppDbEntry(key_content, tuple);
    if (!app_db_entry_or.ok())
    {
        status = app_db_entry_or.status();
        std::stringstream msg;
        msg << "Unable to deserialize key " << QuotedVar(key) << ": " << status.message();
        return msg.str();
    }
    auto &app_db_entry = *app_db_entry_or;
    const std::string mirror_session_key = KeyGenerator::generateMirrorSessionKey(app_db_entry.mirror_session_id);
    auto *mirror_session_entry = getMirrorSessionEntry(mirror_session_key);
    if (mirror_session_entry == nullptr)
    {
        std::stringstream msg;
        msg << "No entry found with key " << QuotedVar(key);
        return msg.str();
    }

    std::string cache_result = verifyStateCache(app_db_entry, mirror_session_entry);
    std::string asic_db_result = verifyStateAsicDb(mirror_session_entry);
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

std::string MirrorSessionManager::verifyStateCache(const P4MirrorSessionAppDbEntry &app_db_entry,
                                                   const P4MirrorSessionEntry *mirror_session_entry)
{
    const std::string mirror_session_key = KeyGenerator::generateMirrorSessionKey(app_db_entry.mirror_session_id);

    ReturnCode status = validateMirrorSessionEntry(app_db_entry, SET_COMMAND);
    if (!status.ok()) {
        std::stringstream msg;
        msg << "Validation failed for mirror session DB entry with key "
            << QuotedVar(mirror_session_key) << ": " << status.message();
        return msg.str();
    }
    if (mirror_session_entry->mirror_session_key != mirror_session_key)
    {
        std::stringstream msg;
        msg << "Mirror session with key " << QuotedVar(mirror_session_key)
            << " does not match internal cache "
            << QuotedVar(mirror_session_entry->mirror_session_key)
            << " in mirror session manager.";
        return msg.str();
    }
    if (mirror_session_entry->mirror_session_id != app_db_entry.mirror_session_id)
    {
        std::stringstream msg;
        msg << "Mirror session " << QuotedVar(app_db_entry.mirror_session_id)
            << " does not match internal cache "
            << QuotedVar(mirror_session_entry->mirror_session_id)
            << " in mirror session manager.";
        return msg.str();
    }
    if (mirror_session_entry->action != app_db_entry.action) {
        std::stringstream msg;
        msg << "Mirror session " << QuotedVar(app_db_entry.mirror_session_id)
            << " with action " << QuotedVar(app_db_entry.action)
            << " does not match internal cache "
            << QuotedVar(mirror_session_entry->action)
            << " in mirror session manager.";
        return msg.str();
    }
    if (mirror_session_entry->port != app_db_entry.port)
    {
        std::stringstream msg;
        msg << "Mirror session " << QuotedVar(app_db_entry.mirror_session_id)
            << " with port " << QuotedVar(app_db_entry.port)
            << " does not match internal cache "
            << QuotedVar(mirror_session_entry->port)
            << " in mirror session manager.";
        return msg.str();
    }
    if (mirror_session_entry->src_ip.to_string() != app_db_entry.src_ip.to_string())
    {
        std::stringstream msg;
        msg << "Mirror session " << QuotedVar(app_db_entry.mirror_session_id)
            << " with source IP " << QuotedVar(app_db_entry.src_ip.to_string())
            << " does not match internal cache "
            << QuotedVar(mirror_session_entry->src_ip.to_string())
            << " in mirror session manager.";
        return msg.str();
    }
    if (mirror_session_entry->dst_ip.to_string() != app_db_entry.dst_ip.to_string())
    {
        std::stringstream msg;
        msg << "Mirror session " << QuotedVar(app_db_entry.mirror_session_id)
            << " with dest IP " << QuotedVar(app_db_entry.dst_ip.to_string())
            << " does not match internal cache "
            << QuotedVar(mirror_session_entry->dst_ip.to_string())
            << " in mirror session manager.";
        return msg.str();
    }
    if (mirror_session_entry->src_mac.to_string() != app_db_entry.src_mac.to_string())
    {
        std::stringstream msg;
        msg << "Mirror session " << QuotedVar(app_db_entry.mirror_session_id)
            << " with source MAC " << QuotedVar(app_db_entry.src_mac.to_string())
            << " does not match internal cache "
            << QuotedVar(mirror_session_entry->src_mac.to_string())
            << " in mirror session manager.";
        return msg.str();
    }
    if (mirror_session_entry->dst_mac.to_string() != app_db_entry.dst_mac.to_string())
    {
        std::stringstream msg;
        msg << "Mirror session " << QuotedVar(app_db_entry.mirror_session_id)
            << " with dest MAC " << QuotedVar(app_db_entry.dst_mac.to_string())
            << " does not match internal cache "
            << QuotedVar(mirror_session_entry->dst_mac.to_string())
            << " in mirror session manager.";
        return msg.str();
    }
    if (mirror_session_entry->ttl != app_db_entry.ttl)
    {
        std::stringstream msg;
        msg << "Mirror session " << QuotedVar(app_db_entry.mirror_session_id)
            << " with ttl " << QuotedVar(std::to_string(app_db_entry.ttl))
            << " does not match internal cache "
            << QuotedVar(std::to_string(mirror_session_entry->ttl))
            << " in mirror session manager.";
        return msg.str();
    }
    if (mirror_session_entry->tos != app_db_entry.tos)
    {
        std::stringstream msg;
        msg << "Mirror session " << QuotedVar(app_db_entry.mirror_session_id)
            << " with tos " << QuotedVar(std::to_string(app_db_entry.tos))
            << " does not match internal cache "
            << QuotedVar(std::to_string(mirror_session_entry->tos))
            << " in mirror session manager.";
        return msg.str();
    }
    if (mirror_session_entry->failover_port != app_db_entry.failover_port) {
        std::stringstream msg;
        msg << "Mirror session " << QuotedVar(app_db_entry.mirror_session_id)
            << " with failover_port " << QuotedVar(app_db_entry.failover_port)
            << " does not match internal cache "
            << QuotedVar(mirror_session_entry->failover_port)
            << " in mirror session manager.";
        return msg.str();
    }
    if (mirror_session_entry->vlan_id != app_db_entry.vlan_id) {
        std::stringstream msg;
        msg << "Mirror session " << QuotedVar(app_db_entry.mirror_session_id)
            << " with vlan_id " << QuotedVar(std::to_string(app_db_entry.vlan_id))
            << " does not match internal cache "
            << QuotedVar(std::to_string(mirror_session_entry->vlan_id))
            << " in mirror session manager.";
        return msg.str();
    }
    if (mirror_session_entry->udp_src_port != app_db_entry.udp_src_port) {
        std::stringstream msg;
        msg << "Mirror session " << QuotedVar(app_db_entry.mirror_session_id)
            << " with udp_src_port "
            << QuotedVar(std::to_string(app_db_entry.udp_src_port))
            << " does not match internal cache "
            << QuotedVar(std::to_string(mirror_session_entry->udp_src_port))
            << " in mirror session manager.";
        return msg.str();
    }
    if (mirror_session_entry->udp_dst_port != app_db_entry.udp_dst_port) {
        std::stringstream msg;
        msg << "Mirror session " << QuotedVar(app_db_entry.mirror_session_id)
            << " with udp_dst_port "
            << QuotedVar(std::to_string(app_db_entry.udp_dst_port))
            << " does not match internal cache "
            << QuotedVar(std::to_string(mirror_session_entry->udp_dst_port))
            << " in mirror session manager.";
        return msg.str();
    }

    return m_p4OidMapper->verifyOIDMapping(SAI_OBJECT_TYPE_MIRROR_SESSION, mirror_session_entry->mirror_session_key,
                                           mirror_session_entry->mirror_session_oid);
}

std::string MirrorSessionManager::verifyStateAsicDb(const P4MirrorSessionEntry *mirror_session_entry)
{
  auto attrs_or = prepareSaiAttrs(*mirror_session_entry);
  if (!attrs_or.ok()) {
    return std::string("Failed to get SAI attrs: ") +
           attrs_or.status().message();
  }
    std::vector<sai_attribute_t> attrs = *attrs_or;
    std::vector<swss::FieldValueTuple> exp = saimeta::SaiAttributeList::serialize_attr_list(
        SAI_OBJECT_TYPE_MIRROR_SESSION, (uint32_t)attrs.size(), attrs.data(),
        /*countOnly=*/false);

    swss::DBConnector db("ASIC_DB", 0);
    swss::Table table(&db, "ASIC_STATE");
    std::string key = sai_serialize_object_type(SAI_OBJECT_TYPE_MIRROR_SESSION) + ":" +
                      sai_serialize_object_id(mirror_session_entry->mirror_session_oid);
    std::vector<swss::FieldValueTuple> values;
    if (!table.get(key, values))
    {
        return std::string("ASIC DB key not found ") + key;
    }

    return verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                       /*allow_unknown=*/false);
}

} // namespace p4orch
