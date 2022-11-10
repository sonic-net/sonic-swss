#include "p4orch/gre_tunnel_manager.h"

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "SaiAttributeList.h"
#include "crmorch.h"
#include "dbconnector.h"
#include "ipaddress.h"
#include "json.hpp"
#include "logger.h"
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
extern sai_tunnel_api_t *sai_tunnel_api;
extern sai_router_interface_api_t *sai_router_intfs_api;
extern CrmOrch *gCrmOrch;
extern sai_object_id_t gVirtualRouterId;

namespace
{

ReturnCode validateGreTunnelAppDbEntry(const P4GreTunnelAppDbEntry &app_db_entry)
{
    if (app_db_entry.action_str != p4orch::kTunnelAction)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << "Invalid action " << QuotedVar(app_db_entry.action_str) << " of GRE Tunnel App DB entry";
    }
    if (app_db_entry.router_interface_id.empty())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << QuotedVar(prependParamField(p4orch::kTunnelId)) << " field is missing in table entry";
    }
    if (app_db_entry.encap_src_ip.isZero())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << QuotedVar(prependParamField(p4orch::kEncapSrcIp)) << " field is missing in table entry";
    }
    if (app_db_entry.encap_dst_ip.isZero())
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
               << QuotedVar(prependParamField(p4orch::kEncapDstIp)) << " field is missing in table entry";
    }
    return ReturnCode();
}

std::vector<sai_attribute_t> getSaiAttrs(const P4GreTunnelEntry &gre_tunnel_entry)
{
    std::vector<sai_attribute_t> tunnel_attrs;
    sai_attribute_t tunnel_attr;
    tunnel_attr.id = SAI_TUNNEL_ATTR_TYPE;
    tunnel_attr.value.s32 = SAI_TUNNEL_TYPE_IPINIP_GRE;
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_TUNNEL_ATTR_PEER_MODE;
    tunnel_attr.value.s32 = SAI_TUNNEL_PEER_MODE_P2P;
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
    tunnel_attr.value.oid = gre_tunnel_entry.underlay_if_oid;
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_TUNNEL_ATTR_OVERLAY_INTERFACE;
    tunnel_attr.value.oid = gre_tunnel_entry.overlay_if_oid;
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
    swss::copy(tunnel_attr.value.ipaddr, gre_tunnel_entry.encap_src_ip);
    tunnel_attrs.push_back(tunnel_attr);

    tunnel_attr.id = SAI_TUNNEL_ATTR_ENCAP_DST_IP;
    swss::copy(tunnel_attr.value.ipaddr, gre_tunnel_entry.encap_dst_ip);
    tunnel_attrs.push_back(tunnel_attr);
    return tunnel_attrs;
}

} // namespace

P4GreTunnelEntry::P4GreTunnelEntry(const std::string &tunnel_id, const std::string &router_interface_id,
                                   const swss::IpAddress &encap_src_ip, const swss::IpAddress &encap_dst_ip,
                                   const swss::IpAddress &neighbor_id)
    : tunnel_id(tunnel_id), router_interface_id(router_interface_id), encap_src_ip(encap_src_ip),
      encap_dst_ip(encap_dst_ip), neighbor_id(neighbor_id)
{
    SWSS_LOG_ENTER();
    tunnel_key = KeyGenerator::generateTunnelKey(tunnel_id);
}

void GreTunnelManager::enqueue(const swss::KeyOpFieldsValuesTuple &entry)
{
    m_entries.push_back(entry);
}

void GreTunnelManager::drain()
{
    SWSS_LOG_ENTER();

    for (const auto &key_op_fvs_tuple : m_entries)
    {
        std::string table_name;
        std::string key;
        parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &key);
        const std::vector<swss::FieldValueTuple> &attributes = kfvFieldsValues(key_op_fvs_tuple);

        const std::string &operation = kfvOp(key_op_fvs_tuple);

        ReturnCode status;
        auto app_db_entry_or = deserializeP4GreTunnelAppDbEntry(key, attributes);
        if (!app_db_entry_or.ok())
        {
            status = app_db_entry_or.status();
            SWSS_LOG_ERROR("Unable to deserialize  GRE Tunnel APP DB entry with key %s: %s",
                           QuotedVar(kfvKey(key_op_fvs_tuple)).c_str(), status.message().c_str());
            m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                 status,
                                 /*replace=*/true);
            continue;
        }
        auto &app_db_entry = *app_db_entry_or;

        const std::string tunnel_key = KeyGenerator::generateTunnelKey(app_db_entry.tunnel_id);

        // Fulfill the operation.
        if (operation == SET_COMMAND)
        {
            status = validateGreTunnelAppDbEntry(app_db_entry);
            if (!status.ok())
            {
                SWSS_LOG_ERROR("Validation failed for GRE Tunnel APP DB entry with key %s: %s",
                               QuotedVar(kfvKey(key_op_fvs_tuple)).c_str(), status.message().c_str());
                m_publisher->publish(APP_P4RT_TABLE_NAME, kfvKey(key_op_fvs_tuple), kfvFieldsValues(key_op_fvs_tuple),
                                     status,
                                     /*replace=*/true);
                continue;
            }
            auto *gre_tunnel_entry = getGreTunnelEntry(tunnel_key);
            if (gre_tunnel_entry == nullptr)
            {
                // Create new GRE tunnel.
                status = processAddRequest(app_db_entry);
            }
            else
            {
                // Modify existing GRE tunnel.
                status = processUpdateRequest(app_db_entry, gre_tunnel_entry);
            }
        }
        else if (operation == DEL_COMMAND)
        {
            // Delete GRE tunnel.
            status = processDeleteRequest(tunnel_key);
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

P4GreTunnelEntry *GreTunnelManager::getGreTunnelEntry(const std::string &tunnel_key)
{
    SWSS_LOG_ENTER();

    auto it = m_greTunnelTable.find(tunnel_key);

    if (it == m_greTunnelTable.end())
    {
        return nullptr;
    }
    else
    {
        return &it->second;
    }
};

ReturnCodeOr<const P4GreTunnelEntry> GreTunnelManager::getConstGreTunnelEntry(const std::string &tunnel_key)
{
    SWSS_LOG_ENTER();

    auto *tunnel = getGreTunnelEntry(tunnel_key);
    if (tunnel == nullptr)
    {
        return ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
               << "GRE Tunnel with key " << QuotedVar(tunnel_key) << " was not found.";
    }
    else
    {
        return *tunnel;
    }
}

ReturnCodeOr<P4GreTunnelAppDbEntry> GreTunnelManager::deserializeP4GreTunnelAppDbEntry(
    const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
{
    SWSS_LOG_ENTER();

    P4GreTunnelAppDbEntry app_db_entry = {};
    app_db_entry.encap_src_ip = swss::IpAddress("0.0.0.0");
    app_db_entry.encap_dst_ip = swss::IpAddress("0.0.0.0");

    try
    {
        nlohmann::json j = nlohmann::json::parse(key);
        app_db_entry.tunnel_id = j[prependMatchField(p4orch::kTunnelId)];
    }
    catch (std::exception &ex)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Failed to deserialize GRE tunnel id";
    }

    for (const auto &it : attributes)
    {
        const auto &field = fvField(it);
        const auto &value = fvValue(it);
        if (field == prependParamField(p4orch::kRouterInterfaceId))
        {
            app_db_entry.router_interface_id = value;
        }
        else if (field == prependParamField(p4orch::kEncapSrcIp))
        {
            try
            {
                app_db_entry.encap_src_ip = swss::IpAddress(value);
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid IP address " << QuotedVar(value) << " of field " << QuotedVar(field);
            }
        }
        else if (field == prependParamField(p4orch::kEncapDstIp))
        {
            try
            {
                app_db_entry.encap_dst_ip = swss::IpAddress(value);
            }
            catch (std::exception &ex)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Invalid IP address " << QuotedVar(value) << " of field " << QuotedVar(field);
            }
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

ReturnCode GreTunnelManager::processAddRequest(const P4GreTunnelAppDbEntry &app_db_entry)
{
    SWSS_LOG_ENTER();

    P4GreTunnelEntry gre_tunnel_entry(app_db_entry.tunnel_id, app_db_entry.router_interface_id,
                                      app_db_entry.encap_src_ip, app_db_entry.encap_dst_ip, app_db_entry.encap_dst_ip);
    auto status = createGreTunnel(gre_tunnel_entry);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to create GRE tunnel with key %s", QuotedVar(gre_tunnel_entry.tunnel_key).c_str());
    }
    return status;
}

ReturnCode GreTunnelManager::createGreTunnel(P4GreTunnelEntry &gre_tunnel_entry)
{
    SWSS_LOG_ENTER();

    // Check the existence of the GRE tunnel in GRE tunnel manager and centralized
    // mapper.
    if (getGreTunnelEntry(gre_tunnel_entry.tunnel_key) != nullptr)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_EXISTS)
                             << "GRE tunnel with key " << QuotedVar(gre_tunnel_entry.tunnel_key)
                             << " already exists in GRE tunnel manager");
    }
    if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_entry.tunnel_key))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("GRE tunnel with key " << QuotedVar(gre_tunnel_entry.tunnel_key)
                                                                        << " already exists in centralized mapper");
    }

    // From centralized mapper, get OID of router interface that GRE tunnel
    // depends on.
    const auto router_interface_key = KeyGenerator::generateRouterInterfaceKey(gre_tunnel_entry.router_interface_id);
    if (!m_p4OidMapper->getOID(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_interface_key,
                               &gre_tunnel_entry.underlay_if_oid))
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "Router intf " << QuotedVar(gre_tunnel_entry.router_interface_id) << " does not exist");
    }

    std::vector<sai_attribute_t> overlay_intf_attrs;

    sai_attribute_t overlay_intf_attr;
    overlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    overlay_intf_attr.value.oid = gVirtualRouterId;
    overlay_intf_attrs.push_back(overlay_intf_attr);

    overlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    overlay_intf_attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_LOOPBACK;
    overlay_intf_attrs.push_back(overlay_intf_attr);

    // Call SAI API.
    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_router_intfs_api->create_router_interface(&gre_tunnel_entry.overlay_if_oid, gSwitchId,
                                                      (uint32_t)overlay_intf_attrs.size(), overlay_intf_attrs.data()),
        "Failed to create the Loopback router interface for GRE tunnel "
        "SAI_TUNNEL_ATTR_OVERLAY_INTERFACE attribute"
            << QuotedVar(gre_tunnel_entry.tunnel_key));

    // Prepare attributes for the SAI creation call.
    std::vector<sai_attribute_t> tunnel_attrs = getSaiAttrs(gre_tunnel_entry);

    // Call SAI API.
    auto sai_status = sai_tunnel_api->create_tunnel(&gre_tunnel_entry.tunnel_oid, gSwitchId,
                                                    (uint32_t)tunnel_attrs.size(), tunnel_attrs.data());
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        auto status = ReturnCode(sai_status) << "Failed to create GRE tunnel " << QuotedVar(gre_tunnel_entry.tunnel_key)
                                             << " on rif " << QuotedVar(gre_tunnel_entry.router_interface_id);
        SWSS_LOG_ERROR("%s", status.message().c_str());
        auto recovery_status = sai_router_intfs_api->remove_router_interface(gre_tunnel_entry.overlay_if_oid);
        if (recovery_status != SAI_STATUS_SUCCESS)
        {
            auto rc = ReturnCode(recovery_status) << "Failed to recover overlay router interface due to SAI call "
                                                     "failure: Failed to remove loopback router interface "
                                                  << QuotedVar(sai_serialize_object_id(gre_tunnel_entry.overlay_if_oid))
                                                  << " while clean up dependencies.";
            SWSS_LOG_ERROR("%s", rc.message().c_str());
            SWSS_RAISE_CRITICAL_STATE(rc.message());
        }
        return status;
    }

    // On successful creation, increment ref count.
    m_p4OidMapper->increaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE, router_interface_key);

    // Add created entry to internal table.
    m_greTunnelTable.emplace(gre_tunnel_entry.tunnel_key, gre_tunnel_entry);

    // Add the key to OID map to centralized mapper.
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_entry.tunnel_key, gre_tunnel_entry.tunnel_oid);

    return ReturnCode();
}

ReturnCode GreTunnelManager::processUpdateRequest(const P4GreTunnelAppDbEntry &app_db_entry,
                                                  P4GreTunnelEntry *gre_tunnel_entry)
{
    SWSS_LOG_ENTER();

    ReturnCode status = ReturnCode(StatusCode::SWSS_RC_UNIMPLEMENTED)
                        << "Currently GRE tunnel doesn't support update by SAI. GRE tunnel key "
                        << QuotedVar(gre_tunnel_entry->tunnel_key);
    SWSS_LOG_ERROR("%s", status.message().c_str());
    return status;
}

ReturnCode GreTunnelManager::processDeleteRequest(const std::string &tunnel_key)
{
    SWSS_LOG_ENTER();

    auto status = removeGreTunnel(tunnel_key);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to remove GRE tunnel with key %s", QuotedVar(tunnel_key).c_str());
    }

    return status;
}

ReturnCode GreTunnelManager::removeGreTunnel(const std::string &tunnel_key)
{
    SWSS_LOG_ENTER();

    // Check the existence of the GRE tunnel in GRE tunnel manager and centralized
    // mapper.
    auto *gre_tunnel_entry = getGreTunnelEntry(tunnel_key);
    if (gre_tunnel_entry == nullptr)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "GRE tunnel with key " << QuotedVar(tunnel_key)
                             << " does not exist in GRE tunnel manager");
    }

    // Check if there is anything referring to the GRE tunnel before deletion.
    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_TUNNEL, tunnel_key, &ref_count))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Failed to get reference count for GRE tunnel "
                                                 << QuotedVar(tunnel_key));
    }
    if (ref_count > 0)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << "GRE tunnel " << QuotedVar(gre_tunnel_entry->tunnel_key)
                             << " referenced by other objects (ref_count = " << ref_count);
    }

    // Call SAI API.
    CHECK_ERROR_AND_LOG_AND_RETURN(sai_tunnel_api->remove_tunnel(gre_tunnel_entry->tunnel_oid),
                                   "Failed to remove GRE tunnel " << QuotedVar(gre_tunnel_entry->tunnel_key));

    auto sai_status = sai_router_intfs_api->remove_router_interface(gre_tunnel_entry->overlay_if_oid);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        auto status = ReturnCode(sai_status) << "Failed to remove loopback router interface "
                                             << QuotedVar(sai_serialize_object_id(gre_tunnel_entry->overlay_if_oid))
                                             << " when removing GRE tunnel " << QuotedVar(gre_tunnel_entry->tunnel_key);
        SWSS_LOG_ERROR("%s", status.message().c_str());

        // Try to recreate the GRE tunnel
        std::vector<sai_attribute_t> tunnel_attrs = getSaiAttrs(*gre_tunnel_entry);

        // Call SAI API.
        auto recovery_status = sai_tunnel_api->create_tunnel(&gre_tunnel_entry->tunnel_oid, gSwitchId,
                                                             (uint32_t)tunnel_attrs.size(), tunnel_attrs.data());
        if (recovery_status != SAI_STATUS_SUCCESS)
        {
            auto rc = ReturnCode(recovery_status) << "Failed to recover the GRE tunnel due to SAI call failure : "
                                                     "Failed to create GRE tunnel "
                                                  << QuotedVar(gre_tunnel_entry->tunnel_key) << " on rif "
                                                  << QuotedVar(gre_tunnel_entry->router_interface_id);
            SWSS_LOG_ERROR("%s", rc.message().c_str());
            SWSS_RAISE_CRITICAL_STATE(rc.message());
        }
        return status;
    }

    // On successful deletion, decrement ref count.
    m_p4OidMapper->decreaseRefCount(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                    KeyGenerator::generateRouterInterfaceKey(gre_tunnel_entry->router_interface_id));

    // Remove the key to OID map to centralized mapper.
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_TUNNEL, tunnel_key);

    // Remove the entry from internal table.
    m_greTunnelTable.erase(tunnel_key);

    return ReturnCode();
}

std::string GreTunnelManager::verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
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
    if (table_name != APP_P4RT_TUNNEL_TABLE_NAME)
    {
        return std::string("Invalid key: ") + key;
    }

    ReturnCode status;
    auto app_db_entry_or = deserializeP4GreTunnelAppDbEntry(key_content, tuple);
    if (!app_db_entry_or.ok())
    {
        status = app_db_entry_or.status();
        std::stringstream msg;
        msg << "Unable to deserialize key " << QuotedVar(key) << ": " << status.message();
        return msg.str();
    }
    auto &app_db_entry = *app_db_entry_or;
    const std::string tunnel_key = KeyGenerator::generateTunnelKey(app_db_entry.tunnel_id);
    auto *gre_tunnel_entry = getGreTunnelEntry(tunnel_key);
    if (gre_tunnel_entry == nullptr)
    {
        std::stringstream msg;
        msg << "No entry found with key " << QuotedVar(key);
        return msg.str();
    }

    std::string cache_result = verifyStateCache(app_db_entry, gre_tunnel_entry);
    std::string asic_db_result = verifyStateAsicDb(gre_tunnel_entry);
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

std::string GreTunnelManager::verifyStateCache(const P4GreTunnelAppDbEntry &app_db_entry,
                                               const P4GreTunnelEntry *gre_tunnel_entry)
{
    const std::string tunnel_key = KeyGenerator::generateTunnelKey(app_db_entry.tunnel_id);
    ReturnCode status = validateGreTunnelAppDbEntry(app_db_entry);
    if (!status.ok())
    {
        std::stringstream msg;
        msg << "Validation failed for GRE Tunnel DB entry with key " << QuotedVar(tunnel_key) << ": "
            << status.message();
        return msg.str();
    }

    if (gre_tunnel_entry->tunnel_key != tunnel_key)
    {
        std::stringstream msg;
        msg << "GreTunnel with key " << QuotedVar(tunnel_key) << " does not match internal cache "
            << QuotedVar(gre_tunnel_entry->tunnel_key) << " in Gre Tunnel manager.";
        return msg.str();
    }
    if (gre_tunnel_entry->tunnel_id != app_db_entry.tunnel_id)
    {
        std::stringstream msg;
        msg << "GreTunnel " << QuotedVar(app_db_entry.tunnel_id) << " does not match internal cache "
            << QuotedVar(gre_tunnel_entry->tunnel_id) << " in GreTunnel manager.";
        return msg.str();
    }
    if (gre_tunnel_entry->router_interface_id != app_db_entry.router_interface_id)
    {
        std::stringstream msg;
        msg << "GreTunnel " << QuotedVar(app_db_entry.tunnel_id) << " with ritf ID "
            << QuotedVar(app_db_entry.router_interface_id) << " does not match internal cache "
            << QuotedVar(gre_tunnel_entry->router_interface_id) << " in GreTunnel manager.";
        return msg.str();
    }
    if (gre_tunnel_entry->encap_src_ip.to_string() != app_db_entry.encap_src_ip.to_string())
    {
        std::stringstream msg;
        msg << "GreTunnel " << QuotedVar(app_db_entry.tunnel_id) << " with source IP "
            << QuotedVar(app_db_entry.encap_src_ip.to_string()) << " does not match internal cache "
            << QuotedVar(gre_tunnel_entry->encap_src_ip.to_string()) << " in GreTunnel manager.";
        return msg.str();
    }

    if (gre_tunnel_entry->encap_dst_ip.to_string() != app_db_entry.encap_dst_ip.to_string())
    {
        std::stringstream msg;
        msg << "GreTunnel " << QuotedVar(app_db_entry.tunnel_id) << " with destination IP "
            << QuotedVar(app_db_entry.encap_dst_ip.to_string()) << " does not match internal cache "
            << QuotedVar(gre_tunnel_entry->encap_dst_ip.to_string()) << " in GreTunnel manager.";
        return msg.str();
    }

    if (gre_tunnel_entry->neighbor_id.to_string() != app_db_entry.encap_dst_ip.to_string())
    {
        std::stringstream msg;
        msg << "GreTunnel " << QuotedVar(app_db_entry.tunnel_id) << " with destination IP "
            << QuotedVar(app_db_entry.encap_dst_ip.to_string()) << " does not match internal cache "
            << QuotedVar(gre_tunnel_entry->neighbor_id.to_string()) << " fo neighbor_id in GreTunnel manager.";
        return msg.str();
    }

    return m_p4OidMapper->verifyOIDMapping(SAI_OBJECT_TYPE_TUNNEL, gre_tunnel_entry->tunnel_key,
                                           gre_tunnel_entry->tunnel_oid);
}

std::string GreTunnelManager::verifyStateAsicDb(const P4GreTunnelEntry *gre_tunnel_entry)
{
    swss::DBConnector db("ASIC_DB", 0);
    swss::Table table(&db, "ASIC_STATE");

    // Verify Overlay router interface ASIC DB attributes
    std::string key = sai_serialize_object_type(SAI_OBJECT_TYPE_ROUTER_INTERFACE) + ":" +
                      sai_serialize_object_id(gre_tunnel_entry->overlay_if_oid);
    std::vector<swss::FieldValueTuple> values;
    if (!table.get(key, values))
    {
        return std::string("ASIC DB key not found ") + key;
    }

    std::vector<sai_attribute_t> overlay_intf_attrs;
    sai_attribute_t overlay_intf_attr;
    overlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID;
    overlay_intf_attr.value.oid = gVirtualRouterId;
    overlay_intf_attrs.push_back(overlay_intf_attr);
    overlay_intf_attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    overlay_intf_attr.value.s32 = SAI_ROUTER_INTERFACE_TYPE_LOOPBACK;
    overlay_intf_attrs.push_back(overlay_intf_attr);
    std::vector<swss::FieldValueTuple> exp = saimeta::SaiAttributeList::serialize_attr_list(
        SAI_OBJECT_TYPE_ROUTER_INTERFACE, (uint32_t)overlay_intf_attrs.size(), overlay_intf_attrs.data(),
        /*countOnly=*/false);
    verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                /*allow_unknown=*/false);

    // Verify Tunnel ASIC DB attributes
    std::vector<sai_attribute_t> attrs = getSaiAttrs(*gre_tunnel_entry);
    exp = saimeta::SaiAttributeList::serialize_attr_list(SAI_OBJECT_TYPE_TUNNEL, (uint32_t)attrs.size(), attrs.data(),
                                                         /*countOnly=*/false);
    key =
        sai_serialize_object_type(SAI_OBJECT_TYPE_TUNNEL) + ":" + sai_serialize_object_id(gre_tunnel_entry->tunnel_oid);
    values.clear();
    if (!table.get(key, values))
    {
        return std::string("ASIC DB key not found ") + key;
    }

    return verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                       /*allow_unknown=*/false);
}
