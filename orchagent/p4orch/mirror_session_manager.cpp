#include "p4orch/mirror_session_manager.h"

#include "json.hpp"
#include "p4orch/p4orch_util.h"
#include "portsorch.h"
#include "swss/logger.h"
#include "swssnet.h"

extern PortsOrch *gPortsOrch;
extern sai_mirror_api_t *sai_mirror_api;
extern sai_object_id_t gSwitchId;

namespace p4orch
{

void MirrorSessionManager::enqueue(const swss::KeyOpFieldsValuesTuple &entry)
{
    SWSS_LOG_ENTER();
    m_entries.push_back(entry);
}

void MirrorSessionManager::drain()
{
    SWSS_LOG_ENTER();

    for (const auto &key_op_fvs_tuple : m_entries)
    {
        std::string table_name;
        std::string key;
        parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &key);
        const std::vector<swss::FieldValueTuple> &attributes = kfvFieldsValues(key_op_fvs_tuple);

        ReturnCode status;
        auto app_db_entry_or = deserializeP4MirrorSessionAppDbEntry(key, attributes);
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

        const std::string mirror_session_key = KeyGenerator::generateMirrorSessionKey(app_db_entry.mirror_session_id);

        // Fulfill the operation.
        const std::string &operation = kfvOp(key_op_fvs_tuple);
        if (operation == SET_COMMAND)
        {
            auto *mirror_session_entry = getMirrorSessionEntry(mirror_session_key);
            if (mirror_session_entry == nullptr)
            {
                // Create new mirror session.
                status = processAddRequest(app_db_entry);
            }
            else
            {
                // Modify existing mirror session.
                status = processUpdateRequest(app_db_entry, mirror_session_entry);
            }
        }
        else if (operation == DEL_COMMAND)
        {
            // Delete mirror session.
            status = processDeleteRequest(mirror_session_key);
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
        if (field == prependParamField(p4orch::kPort))
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
        }
        else if (field == prependParamField(p4orch::kSrcIp))
        {
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
        }
        else if (field == prependParamField(p4orch::kDstIp))
        {
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
        else if (field == prependParamField(p4orch::kSrcMac))
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
        else if (field == prependParamField(p4orch::kDstMac))
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
        }
        else if (field == p4orch::kAction)
        {
            if (value != p4orch::kMirrorAsIpv4Erspan)
            {
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

ReturnCode MirrorSessionManager::processAddRequest(const P4MirrorSessionAppDbEntry &app_db_entry)
{
    SWSS_LOG_ENTER();

    ReturnCode status;
    // Check if all required fields for add operation are given in APP DB entry.
    if (app_db_entry.has_port && app_db_entry.has_src_ip && app_db_entry.has_dst_ip && app_db_entry.has_src_mac &&
        app_db_entry.has_dst_mac && app_db_entry.has_ttl && app_db_entry.has_tos)
    {
        P4MirrorSessionEntry mirror_session_entry(
            KeyGenerator::generateMirrorSessionKey(app_db_entry.mirror_session_id),
            /*mirror_session_oid=*/0, app_db_entry.mirror_session_id, app_db_entry.port, app_db_entry.src_ip,
            app_db_entry.dst_ip, app_db_entry.src_mac, app_db_entry.dst_mac, app_db_entry.ttl, app_db_entry.tos);
        status = createMirrorSession(std::move(mirror_session_entry));
    }
    else
    {
        status = ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                 << "Mirror session entry with mirror_session_id " << QuotedVar(app_db_entry.mirror_session_id)
                 << " doesn't specify all required fields for ADD operation.";
        SWSS_LOG_ERROR("%s", status.message().c_str());
    }

    return status;
}

ReturnCode MirrorSessionManager::createMirrorSession(P4MirrorSessionEntry mirror_session_entry)
{
    SWSS_LOG_ENTER();

    // Check the existence of the mirror session in centralized mapper.
    if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_MIRROR_SESSION, mirror_session_entry.mirror_session_key))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Mirror session with key "
                                                 << QuotedVar(mirror_session_entry.mirror_session_key)
                                                 << " already exists in centralized mapper");
    }

    swss::Port port;
    if (!gPortsOrch->getPort(mirror_session_entry.port, port))
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "Failed to get port info for port " << QuotedVar(mirror_session_entry.port));
    }
    if (port.m_type != Port::Type::PHY)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << "Port " << QuotedVar(mirror_session_entry.port) << "'s type " << port.m_type
                             << " is not physical and is invalid as destination "
                                "port for mirror packet.");
    }

    // Prepare attributes for the SAI creation call.
    std::vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

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
    memcpy(attr.value.mac, mirror_session_entry.src_mac.getMac(), sizeof(sai_mac_t));
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS;
    memcpy(attr.value.mac, mirror_session_entry.dst_mac.getMac(), sizeof(sai_mac_t));
    attrs.push_back(attr);

    attr.id = SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE;
    attr.value.u16 = GRE_PROTOCOL_ERSPAN;
    attrs.push_back(attr);

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

    // Check the existence of the mirror session in mirror manager and centralized
    // mapper.
    if (existing_mirror_session_entry == nullptr)
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("existing_mirror_session_entry is nullptr");
    }
    if (!m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_MIRROR_SESSION, existing_mirror_session_entry->mirror_session_key))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Mirror session with key "
                                                 << QuotedVar(existing_mirror_session_entry->mirror_session_key)
                                                 << " doesn't exist in centralized mapper");
    }

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
        "Failed to set new ttl " << new_ttl << " for mirror session "
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
        "Failed to set new tos " << new_tos << " for mirror session "
                                 << QuotedVar(existing_mirror_session_entry->mirror_session_key));

    // Update the entry in table
    existing_mirror_session_entry->tos = new_tos;

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

    // Check if there is anything referring to the mirror session before deletion.
    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_MIRROR_SESSION, mirror_session_key, &ref_count))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Failed to get reference count for mirror session "
                                                 << QuotedVar(mirror_session_key));
    }
    if (ref_count > 0)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_IN_USE)
                             << "Mirror session " << QuotedVar(mirror_session_entry->mirror_session_key)
                             << " referenced by other objects (ref_count = " << ref_count);
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

} // namespace p4orch
