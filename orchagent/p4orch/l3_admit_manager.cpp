#include "p4orch/l3_admit_manager.h"

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "SaiAttributeList.h"
#include "dbconnector.h"
#include <nlohmann/json.hpp>
#include "logger.h"
#include "p4orch/p4orch_util.h"
#include "portsorch.h"
#include "return_code.h"
#include "sai_serialize.h"
#include "table.h"
#include "tokenize.h"
extern "C"
{
#include "sai.h"
}

using ::p4orch::kTableKeyDelimiter;

extern PortsOrch *gPortsOrch;
extern sai_object_id_t gSwitchId;
extern sai_my_mac_api_t *sai_my_mac_api;

namespace
{

ReturnCodeOr<std::vector<sai_attribute_t>> getSaiAttrs(const P4L3AdmitEntry &l3_admit_entry)
{
    std::vector<sai_attribute_t> l3_admit_attrs;
    sai_attribute_t l3_admit_attr;

    l3_admit_attr.id = SAI_MY_MAC_ATTR_MAC_ADDRESS;
    memcpy(l3_admit_attr.value.mac, l3_admit_entry.mac_address_data.getMac(), sizeof(sai_mac_t));
    l3_admit_attrs.push_back(l3_admit_attr);

    l3_admit_attr.id = SAI_MY_MAC_ATTR_MAC_ADDRESS_MASK;
    memcpy(l3_admit_attr.value.mac, l3_admit_entry.mac_address_mask.getMac(), sizeof(sai_mac_t));
    l3_admit_attrs.push_back(l3_admit_attr);

    l3_admit_attr.id = SAI_MY_MAC_ATTR_PRIORITY;
    l3_admit_attr.value.u32 = l3_admit_entry.priority;
    l3_admit_attrs.push_back(l3_admit_attr);

    if (!l3_admit_entry.port_name.empty())
    {
        Port port;
        if (!gPortsOrch->getPort(l3_admit_entry.port_name, port))
        {
            LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                                 << "Failed to get port info for port " << QuotedVar(l3_admit_entry.port_name));
        }
        l3_admit_attr.id = SAI_MY_MAC_ATTR_PORT_ID;
        l3_admit_attr.value.oid = port.m_port_id;
        l3_admit_attrs.push_back(l3_admit_attr);
    }

    return l3_admit_attrs;
}

} // namespace

ReturnCode L3AdmitManager::getSaiObject(const std::string &json_key, sai_object_type_t &object_type, std::string &object_key)
{
    return StatusCode::SWSS_RC_UNIMPLEMENTED;
}

void L3AdmitManager::enqueue(const std::string &table_name, const swss::KeyOpFieldsValuesTuple &entry)
{
    m_entries.push_back(entry);
}

void L3AdmitManager::drain()
{
    SWSS_LOG_ENTER();

    for (const auto &key_op_fvs_tuple : m_entries)
    {
        std::string table_name;
        std::string key;
        parseP4RTKey(kfvKey(key_op_fvs_tuple), &table_name, &key);
        const std::vector<swss::FieldValueTuple> &attributes = kfvFieldsValues(key_op_fvs_tuple);

        ReturnCode status;
        auto app_db_entry_or = deserializeP4L3AdmitAppDbEntry(key, attributes);
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

        const std::string l3_admit_key =
            KeyGenerator::generateL3AdmitKey(app_db_entry.mac_address_data, app_db_entry.mac_address_mask,
                                             app_db_entry.port_name, app_db_entry.priority);

        // Fulfill the operation.
        const std::string &operation = kfvOp(key_op_fvs_tuple);
        if (operation == SET_COMMAND)
        {
            auto *l3_admit_entry = getL3AdmitEntry(l3_admit_key);
            if (l3_admit_entry == nullptr)
            {
                // Create new l3 admit.
                status = processAddRequest(app_db_entry, l3_admit_key);
            }
            else
            {
                // Duplicate l3 admit entry, no-op
                status = ReturnCode(StatusCode::SWSS_RC_SUCCESS)
                         << "L3 Admit entry with the same key received: " << QuotedVar(l3_admit_key);
            }
        }
        else if (operation == DEL_COMMAND)
        {
            // Delete l3 admit.
            status = processDeleteRequest(l3_admit_key);
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

P4L3AdmitEntry *L3AdmitManager::getL3AdmitEntry(const std::string &l3_admit_key)
{
    SWSS_LOG_ENTER();

    auto it = m_l3AdmitTable.find(l3_admit_key);

    if (it == m_l3AdmitTable.end())
    {
        return nullptr;
    }
    else
    {
        return &it->second;
    }
}

ReturnCodeOr<P4L3AdmitAppDbEntry> L3AdmitManager::deserializeP4L3AdmitAppDbEntry(
    const std::string &key, const std::vector<swss::FieldValueTuple> &attributes)
{
    SWSS_LOG_ENTER();

    P4L3AdmitAppDbEntry app_db_entry = {};

    try
    {
        nlohmann::json j = nlohmann::json::parse(key);
        // "match/dst_mac":"00:02:03:04:00:00&ff:ff:ff:ff:00:00"
        if (j.find(prependMatchField(p4orch::kDstMac)) != j.end())
        {
            std::string dst_mac_data_and_mask = j[prependMatchField(p4orch::kDstMac)];
            const auto &data_and_mask = swss::tokenize(dst_mac_data_and_mask, p4orch::kDataMaskDelimiter);
            app_db_entry.mac_address_data = swss::MacAddress(trim(data_and_mask[0]));
            if (data_and_mask.size() > 1)
            {
                app_db_entry.mac_address_mask = swss::MacAddress(trim(data_and_mask[1]));
            }
            else
            {
                app_db_entry.mac_address_mask = swss::MacAddress("ff:ff:ff:ff:ff:ff");
            }
        }
        else
        {
            // P4RT set "don't care" value for dst_mac - mask should be all 0
            app_db_entry.mac_address_data = swss::MacAddress("00:00:00:00:00:00");
            app_db_entry.mac_address_mask = swss::MacAddress("00:00:00:00:00:00");
        }

        // "priority":2030
        auto priority_j = j[p4orch::kPriority];
        if (!priority_j.is_number_unsigned())
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Invalid l3 admit entry priority type: should be uint32_t";
        }
        app_db_entry.priority = static_cast<uint32_t>(priority_j);

        // "match/in_port":"Ethernet0"
        if (j.find(prependMatchField(p4orch::kInPort)) != j.end())
        {
            app_db_entry.port_name = j[prependMatchField(p4orch::kInPort)];
        }
    }
    catch (std::exception &ex)
    {
        return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM) << "Failed to deserialize l3 admit key";
    }

    for (const auto &it : attributes)
    {
        const auto &field = fvField(it);
        const auto &value = fvValue(it);
        // "action": "admit_to_l3"
        if (field == p4orch::kAction)
        {
            if (value != p4orch::kL3AdmitAction)
            {
                return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                       << "Unexpected action " << QuotedVar(value) << " in L3 Admit table entry";
            }
        }
        else if (field != p4orch::kControllerMetadata)
        {
            return ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                   << "Unexpected field " << QuotedVar(field) << " in L3 Admit table entry";
        }
    }

    return app_db_entry;
}

ReturnCode L3AdmitManager::processAddRequest(const P4L3AdmitAppDbEntry &app_db_entry, const std::string &l3_admit_key)
{
    SWSS_LOG_ENTER();

    // Check the existence of the l3 admit in l3 admit manager and centralized
    // mapper.
    if (getL3AdmitEntry(l3_admit_key) != nullptr)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_EXISTS) << "l3 admit with key " << QuotedVar(l3_admit_key)
                                                                    << " already exists in l3 admit manager");
    }
    if (m_p4OidMapper->existsOID(SAI_OBJECT_TYPE_MY_MAC, l3_admit_key))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("l3 admit with key " << QuotedVar(l3_admit_key)
                                                                      << " already exists in centralized mapper");
    }
    // Create L3 admit entry
    P4L3AdmitEntry l3_admit_entry(app_db_entry.mac_address_data, app_db_entry.mac_address_mask, app_db_entry.priority,
                                  app_db_entry.port_name);
    auto status = createL3Admit(l3_admit_entry);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to create l3 admit with key %s", QuotedVar(l3_admit_key).c_str());
        return status;
    }
    // Increase reference count to port
    if (!l3_admit_entry.port_name.empty())
    {
        gPortsOrch->increasePortRefCount(l3_admit_entry.port_name);
    }
    // Add created entry to internal table.
    m_l3AdmitTable.emplace(l3_admit_key, l3_admit_entry);

    // Add the key to OID map to centralized mapper.
    m_p4OidMapper->setOID(SAI_OBJECT_TYPE_MY_MAC, l3_admit_key, l3_admit_entry.l3_admit_oid);
    return status;
}

ReturnCode L3AdmitManager::createL3Admit(P4L3AdmitEntry &l3_admit_entry)
{
    SWSS_LOG_ENTER();

    ASSIGN_OR_RETURN(std::vector<sai_attribute_t> l3_admit_attrs, getSaiAttrs(l3_admit_entry));
    // Call SAI API.
    CHECK_ERROR_AND_LOG_AND_RETURN(
        sai_my_mac_api->create_my_mac(&l3_admit_entry.l3_admit_oid, gSwitchId, (uint32_t)l3_admit_attrs.size(),
                                      l3_admit_attrs.data()),
        "Failed to create l3 admit with mac:" << QuotedVar(l3_admit_entry.mac_address_data.to_string())
                                              << "; mac_mask:" << QuotedVar(l3_admit_entry.mac_address_mask.to_string())
                                              << "; priority:" << QuotedVar(std::to_string(l3_admit_entry.priority))
                                              << "; in_port:" << QuotedVar(l3_admit_entry.port_name));

    return ReturnCode();
}

ReturnCode L3AdmitManager::processDeleteRequest(const std::string &l3_admit_key)
{
    SWSS_LOG_ENTER();

    // Check the existence of the l3 admit in l3 admit manager and centralized
    // mapper.
    auto *l3_admit_entry = getL3AdmitEntry(l3_admit_key);
    if (l3_admit_entry == nullptr)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_NOT_FOUND)
                             << "l3 admit with key " << QuotedVar(l3_admit_key)
                             << " does not exist in l3 admit manager");
    }

    // Check if there is anything referring to the l3 admit before deletion.
    uint32_t ref_count;
    if (!m_p4OidMapper->getRefCount(SAI_OBJECT_TYPE_MY_MAC, l3_admit_key, &ref_count))
    {
        RETURN_INTERNAL_ERROR_AND_RAISE_CRITICAL("Failed to get reference count for l3 admit "
                                                 << QuotedVar(l3_admit_key));
    }
    if (ref_count > 0)
    {
        LOG_ERROR_AND_RETURN(ReturnCode(StatusCode::SWSS_RC_INVALID_PARAM)
                             << "l3 admit " << QuotedVar(l3_admit_key)
                             << " referenced by other objects (ref_count = " << ref_count);
    }

    // Call SAI API
    auto status = removeL3Admit(l3_admit_key);
    if (!status.ok())
    {
        SWSS_LOG_ERROR("Failed to remove l3 admit with key %s", QuotedVar(l3_admit_key).c_str());
        return status;
    }

    // Decrease reference count to port
    if (!l3_admit_entry->port_name.empty())
    {
        gPortsOrch->decreasePortRefCount(l3_admit_entry->port_name);
    }
    // Remove the key to OID map to centralized mapper.
    m_p4OidMapper->eraseOID(SAI_OBJECT_TYPE_MY_MAC, l3_admit_key);

    // Remove the entry from internal table.
    m_l3AdmitTable.erase(l3_admit_key);
    return status;
}

ReturnCode L3AdmitManager::removeL3Admit(const std::string &l3_admit_key)
{
    SWSS_LOG_ENTER();

    auto *l3_admit_entry = getL3AdmitEntry(l3_admit_key);
    CHECK_ERROR_AND_LOG_AND_RETURN(sai_my_mac_api->remove_my_mac(l3_admit_entry->l3_admit_oid),
                                   "Failed to remove l3 admit " << QuotedVar(l3_admit_key));

    return ReturnCode();
}

std::string L3AdmitManager::verifyState(const std::string &key, const std::vector<swss::FieldValueTuple> &tuple)
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
    if (table_name != APP_P4RT_L3_ADMIT_TABLE_NAME)
    {
        return std::string("Invalid key: ") + key;
    }

    ReturnCode status;
    auto app_db_entry_or = deserializeP4L3AdmitAppDbEntry(key_content, tuple);
    if (!app_db_entry_or.ok())
    {
        status = app_db_entry_or.status();
        std::stringstream msg;
        msg << "Unable to deserialize key " << QuotedVar(key) << ": " << status.message();
        return msg.str();
    }
    auto &app_db_entry = *app_db_entry_or;
    const std::string l3_admit_key = KeyGenerator::generateL3AdmitKey(
        app_db_entry.mac_address_data, app_db_entry.mac_address_mask, app_db_entry.port_name, app_db_entry.priority);
    auto *l3_admit_entry = getL3AdmitEntry(l3_admit_key);
    if (l3_admit_entry == nullptr)
    {
        std::stringstream msg;
        msg << "No entry found with key " << QuotedVar(key);
        return msg.str();
    }

    std::string cache_result = verifyStateCache(app_db_entry, l3_admit_entry);
    std::string asic_db_result = verifyStateAsicDb(l3_admit_entry);
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

std::string L3AdmitManager::verifyStateCache(const P4L3AdmitAppDbEntry &app_db_entry,
                                             const P4L3AdmitEntry *l3_admit_entry)
{
    const std::string l3_admit_key = KeyGenerator::generateL3AdmitKey(
        app_db_entry.mac_address_data, app_db_entry.mac_address_mask, app_db_entry.port_name, app_db_entry.priority);

    if (l3_admit_entry->port_name != app_db_entry.port_name)
    {
        std::stringstream msg;
        msg << "L3 admit " << QuotedVar(l3_admit_key) << " with port " << QuotedVar(app_db_entry.port_name)
            << " does not match internal cache " << QuotedVar(l3_admit_entry->port_name) << " in L3 admit manager.";
        return msg.str();
    }
    if (l3_admit_entry->mac_address_data.to_string() != app_db_entry.mac_address_data.to_string())
    {
        std::stringstream msg;
        msg << "L3 admit " << QuotedVar(l3_admit_key) << " with MAC addr " << app_db_entry.mac_address_data.to_string()
            << " does not match internal cache " << l3_admit_entry->mac_address_data.to_string()
            << " in L3 admit manager.";
        return msg.str();
    }
    if (l3_admit_entry->mac_address_mask.to_string() != app_db_entry.mac_address_mask.to_string())
    {
        std::stringstream msg;
        msg << "L3 admit " << QuotedVar(l3_admit_key) << " with MAC mask " << app_db_entry.mac_address_mask.to_string()
            << " does not match internal cache " << l3_admit_entry->mac_address_mask.to_string()
            << " in L3 admit manager.";
        return msg.str();
    }
    if (l3_admit_entry->priority != app_db_entry.priority)
    {
        std::stringstream msg;
        msg << "L3 admit " << QuotedVar(l3_admit_key) << " with priority " << app_db_entry.priority
            << " does not match internal cache " << l3_admit_entry->priority << " in L3 admit manager.";
        return msg.str();
    }

    return m_p4OidMapper->verifyOIDMapping(SAI_OBJECT_TYPE_MY_MAC, l3_admit_key, l3_admit_entry->l3_admit_oid);
}

std::string L3AdmitManager::verifyStateAsicDb(const P4L3AdmitEntry *l3_admit_entry)
{
    auto attrs_or = getSaiAttrs(*l3_admit_entry);
    if (!attrs_or.ok())
    {
        return std::string("Failed to get SAI attrs: ") + attrs_or.status().message();
    }
    std::vector<sai_attribute_t> attrs = *attrs_or;
    std::vector<swss::FieldValueTuple> exp =
        saimeta::SaiAttributeList::serialize_attr_list(SAI_OBJECT_TYPE_MY_MAC, (uint32_t)attrs.size(), attrs.data(),
                                                       /*countOnly=*/false);

    swss::DBConnector db("ASIC_DB", 0);
    swss::Table table(&db, "ASIC_STATE");
    std::string key =
        sai_serialize_object_type(SAI_OBJECT_TYPE_MY_MAC) + ":" + sai_serialize_object_id(l3_admit_entry->l3_admit_oid);
    std::vector<swss::FieldValueTuple> values;
    if (!table.get(key, values))
    {
        return std::string("ASIC DB key not found ") + key;
    }

    return verifyAttrs(values, exp, std::vector<swss::FieldValueTuple>{},
                       /*allow_unknown=*/false);
}
