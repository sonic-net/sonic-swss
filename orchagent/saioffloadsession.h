/*
 * saioffloadsession.h
 *
 *  Created on: Feb 21, 2025
 *      Author: Manas Kumar Mandal
 */
#ifndef SWSS_SAIOFFLOADSESSION_H
#define SWSS_SAIOFFLOADSESSION_H

#include <vector>
#include <string>
#include <tuple>
#include <unordered_map>
#include "portsorch.h"
#include "vrforch.h"

using namespace std;
using namespace swss;

using sai_attr_id_val_map_t = std::unordered_map<sai_attr_id_t, sai_attribute_value_t>;
using fv_vector_t = std::vector<FieldValueTuple>;
using fv_map_t = std::map<std::string, std::string>;
using session_fv_map_t = std::map<std::string, fv_map_t>;
// handler type for setting sai attr map and FieldValues
using sai_attr_handler_map_t = std::unordered_map<string,
          std::tuple<sai_attr_id_t,
          void (*)(std::string&, sai_attr_id_val_map_t&, fv_vector_t&)>>;


extern sai_object_id_t      gSwitchId;
extern sai_object_id_t      gVirtualRouterId;
extern PortsOrch*           gPortsOrch;
extern sai_switch_api_t*    sai_switch_api;
extern Directory<Orch*>     gDirectory;

// saioffload handler types for BFD and ICMP
template<typename T>
struct SaiOffloadHandlerTraits { };

template<>
struct SaiOffloadHandlerTraits<sai_bfd_api_t>
{
    using api_t = sai_bfd_api_t;
    using create_session_fn = sai_create_bfd_session_fn;
    using remove_session_fn = sai_remove_bfd_session_fn;
    using set_session_attribute_fn = sai_set_bfd_session_attribute_fn;
    using get_session_attribute_fn = sai_get_bfd_session_attribute_fn;
    using get_session_stats_fn = sai_get_bfd_session_stats_fn;
    using get_session_stats_ext_fn = sai_get_bfd_session_stats_ext_fn;
    using clear_session_stats_fn = sai_clear_bfd_session_stats_fn;
    using notif_t = sai_bfd_session_state_notification_t;
};

template<>
struct SaiOffloadHandlerTraits<sai_icmp_echo_api_t>
{
    using api_t = sai_icmp_echo_api_t;
    using create_session_fn = sai_create_icmp_echo_session_fn;
    using remove_session_fn = sai_remove_icmp_echo_session_fn;
    using set_session_attribute_fn = sai_set_icmp_echo_session_attribute_fn;
    using get_session_attribute_fn = sai_get_icmp_echo_session_attribute_fn;
    using get_session_stats_fn = sai_get_icmp_echo_session_stats_fn;
    using get_session_stats_ext_fn = sai_get_icmp_echo_session_stats_ext_fn;
    using clear_session_stats_fn = sai_clear_icmp_echo_session_stats_fn;
    using notif_t = sai_icmp_echo_session_state_notification_t;
};

/**
 *@enum SaiOffloadHandlerStatus
 *
 *@brief Enumerated status used by SaiOffloadSessionHandler
 */
enum class SaiOffloadHandlerStatus {
    SUCCESS_VALID_ENTRY  = 0,
    RETRY_VALID_ENTRY    = 1,
    FAILED_VALID_ENTRY   = 2,
    FAILED_INVALID_ENTRY = 3
};

const std::unordered_map<SaiOffloadHandlerStatus, std::string> SaiOffloadStatusStrMap =
{
    {SaiOffloadHandlerStatus::RETRY_VALID_ENTRY, "RETRY_VALID_ENTRY"},
    {SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY, "SUCCESS_VALID_ENTRY"},
    {SaiOffloadHandlerStatus::FAILED_VALID_ENTRY, "FAILED_VALID_ENTRY"},
    {SaiOffloadHandlerStatus::FAILED_INVALID_ENTRY, "FAILED_INVALID_ENTRY"}
};

/**
 *@struct SaiOffloadSessionHandler
 *
 *@brief Common Sai Offload session handler used as CRTP
 */
template <class SaiOrchHandlerClass, typename T>
struct SaiOffloadSessionHandler {
    using Tapis = SaiOffloadHandlerTraits<T>;

    /**
     *@method init
     *
     *@brief Initialize the handler
     *
     *@param api(in)  SAI API function pointers
     *@param key(in)  Session key
     *
     *@return SUCCESS_VALID_ENTRY when valid key and successfully initialized
     *        FAILED_INVALID_ENTRY when key is invalid 
     *        FAILED_VALID_ENTRY when initialization fails for valid key
     */
    SaiOffloadHandlerStatus init(typename Tapis::api_t *api, const string &key);

    /**
     *@method create
     *
     *@brief Create SAI offload session
     *
     *@param fv_data(in)  session parameters as Field Value tuples
     *
     *@return SUCCESS_VALID_ENTRY session parameters valid and created with success
     *        FAILED_INVALID_ENTRY session parameters are invalid
     *        FAILED_VALID_ENTRY session creation fails for valid key
     *        RETRY_VALID_ENTRY retry session creation for valid key
     */
    SaiOffloadHandlerStatus create(const fv_vector_t& fv_data);

    /**
     *@method handle_hwlookup
     *
     *@brief Set the hwlookup session attrib based on other session attribs
     *
     *@return SUCCESS_VALID_ENTRY session parameters valid for hwlookup and consumed without error
     *        FAILED_INVALID_ENTRY session parameters are invalid for hwlookup
     *        FAILED_VALID_ENTRY failure in handling hwlookup for valid parameters
     */
    SaiOffloadHandlerStatus handle_hwlookup();

    /**
     *@method remove
     *
     *@brief Remove the SAI offload session
     *
     *@param id(in)  sai session object id to delete
     *
     *@return SUCEES_VALID_ENTRY session id found and removed
     *        FAILED_INVALID_ENTRY session id not found
     *        FAILED_VALID_ENTRY unable to remove session for a found id
     *        RETRY_VALID_ENTRY retry session removal for a found id
     */
    SaiOffloadHandlerStatus remove(sai_object_id_t id);

    /**
     *@method update
     *
     *@brief Update SAI offload session
     *
     *@param id(in)  sai session object id to update
     *       fv_data(in)  session parameters as Field Value tuples
     *       fv_map(in)   existing map of session parameters Field Value
     *
     *@return SUCCESS_VALID_ENTRY session parameters valid and updated with success
     *        FAILED_INVALID_ENTRY session parameters are invalid
     *        FAILED_VALID_ENTRY session update fails for valid key
     *        RETRY_VALID_ENTRY retry session update for valid key
     */
    SaiOffloadHandlerStatus update(sai_object_id_t session_id, const fv_vector_t& fv_data, const fv_map_t& fv_map);

    /**
     *@method register_state_change_notification
     *
     *@brief Registers function pointer to SAI state change notification
     *
     *@return True on success, False on failure
     */
    bool register_state_change_notification();

    /**
     *@method get_fv_vector
     *
     *@brief Return the vector of field value tuples of a session
     *
     *@return vector of field value tuples
     */
    inline fv_vector_t& get_fv_vector() {
        return m_fv_vector;
    }

    /**
     *@method get_fv_map
     *
     *@brief Return the map of field value of a session
     *
     *@return map of field value
     */
    inline fv_map_t& get_fv_map() {
        return m_fv_map;
    }
    /**
     *@method get_state_db_key
     *
     *@brief Returns the formatted state db key of the session
     *
     *@return reference to string of formatted state db key
     */
    inline std::string& get_state_db_key() {
        return m_state_db_key;
    }

    /**
     *@method get_session_id
     *
     *@brief Returns the session id
     *
     *@return SAI object id of the session
     */
    inline sai_object_id_t get_session_id() {
        return m_session_id;
    }

protected:
    SaiOffloadSessionHandler() = default;

    typename Tapis::create_session_fn           sai_create_session;
    typename Tapis::remove_session_fn           sai_remove_session;
    typename Tapis::set_session_attribute_fn    sai_set_session_attrib;
    typename Tapis::get_session_attribute_fn    sai_get_session_attrib;

    string m_key;
    // field value vector
    fv_vector_t m_data;
    // field value vector for state db
    fv_vector_t m_fv_vector;
    // field value map for session cache 
    fv_map_t m_fv_map;
    string m_alias;
    string m_vrf_name;
    string m_state_db_key;
    uint32_t m_port_id;
    uint32_t m_vrf_id;
    // session id
    sai_object_id_t m_session_id;
    // map of sai attribute id and its value
    sai_attr_id_val_map_t m_attr_val_map;
    // attribute vector used for session creation
    std::vector<sai_attribute_t> m_attrs;
};

template <class SaiOrchHandlerClass, typename T>
SaiOffloadHandlerStatus SaiOffloadSessionHandler<SaiOrchHandlerClass, T>::init(typename Tapis::api_t *api, const string &key)
{
    m_key = key;
    return static_cast<SaiOrchHandlerClass *>(this)->do_init(api);
}

template <class SaiOrchHandlerClass, typename T>
SaiOffloadHandlerStatus SaiOffloadSessionHandler<SaiOrchHandlerClass, T>::create(const fv_vector_t& fv_data)
{
    constexpr auto atype = static_cast<sai_api_t>(SaiOrchHandlerClass::SAI_API_TYPE::API_TYPE);
    constexpr auto& name = static_cast<SaiOrchHandlerClass *>(this)->m_name;
    auto& handler_map = static_cast<SaiOrchHandlerClass *>(this)->m_handler_map;

    m_data = fv_data;

    // call the handler for each field-value tuple
    // and fill the m_attr_val_map and m_fv_vector
    for (auto& data : m_data)
    {
        auto field = fvField(data);
        auto value = fvValue(data);
        m_fv_map[field] = value;
        auto hsearch = handler_map.find(field);
        if (hsearch != handler_map.end())
        {
            auto& htuple = hsearch->second;
            auto& handler = std::get<1>(htuple);
            handler(value, m_attr_val_map, m_fv_vector);
        }
        else
        {
            SWSS_LOG_ERROR("%s, Unsupported sai attribute handler for %s", name.c_str(), field.c_str());
            continue;
        }
    }

    // set the SAI hwlookup attribute based on other sai attributes
    auto hwlookup_status = handle_hwlookup();
    if (hwlookup_status != SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY)
    {
        return hwlookup_status;
    }

    // call the derived orch's create
    auto do_create_status = static_cast<SaiOrchHandlerClass *>(this)->do_create();
    if (do_create_status != SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY)
    {
        return do_create_status;
    }

    // for the sai attribute vector for create
    sai_attribute_t attr;
    for (auto it = m_attr_val_map.begin(); it != m_attr_val_map.end(); it++)
    {
        attr.id = it->first;
        attr.value = it->second;
        m_attrs.emplace_back(attr);
    }

    m_session_id = SAI_NULL_OBJECT_ID;
    sai_status_t status = sai_create_session(&m_session_id, gSwitchId, (uint32_t)m_attrs.size(), m_attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("%s, SAI create offload session failed %s, rv:%d", name.c_str(), m_key.c_str(), status);
        task_process_status handle_status = handleSaiCreateStatus(atype, status);
        if (handle_status != task_success)
        {
            // check for retries
            if (parseHandleSaiStatusFailure(handle_status))
            {
                return SaiOffloadHandlerStatus::FAILED_VALID_ENTRY;
            }
            else
            {
                return SaiOffloadHandlerStatus::RETRY_VALID_ENTRY;
            }
        }
    }
    return SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY;
}

template <class SaiOrchHandlerClass, typename T>
SaiOffloadHandlerStatus SaiOffloadSessionHandler<SaiOrchHandlerClass, T>::handle_hwlookup()
{

    constexpr auto dst_mac_attr_id    = static_cast<sai_attr_id_t>(SaiOrchHandlerClass::SAI_ATTR_ID::DST_MAC_ID);
    constexpr auto src_mac_attr_id    = static_cast<sai_attr_id_t>(SaiOrchHandlerClass::SAI_ATTR_ID::SRC_MAC_ID);
    constexpr auto hw_lookup_attr_id  = static_cast<sai_attr_id_t>(SaiOrchHandlerClass::SAI_ATTR_ID::HW_LOOKUP_ID);
    constexpr auto port_attr_id       = static_cast<sai_attr_id_t>(SaiOrchHandlerClass::SAI_ATTR_ID::PORT_ID);
    constexpr auto vrf_attr_id        = static_cast<sai_attr_id_t>(SaiOrchHandlerClass::SAI_ATTR_ID::VRF_ATTR_ID);

    constexpr auto& name = static_cast<SaiOrchHandlerClass *>(this)->m_name;
    auto dmac_it = m_attr_val_map.find(dst_mac_attr_id);

    // hw lookup is not needed when outgoing port is specified
    if (m_alias != "default")
    {
        Port port;
        if (!gPortsOrch->getPort(m_alias, port))
        {
            SWSS_LOG_ERROR("%s, Failed to locate port %s", name.c_str(), m_alias.c_str());
            return SaiOffloadHandlerStatus::RETRY_VALID_ENTRY;
        }

        // dmac is needed as no lookup is performed in hardware
        if (dmac_it == m_attr_val_map.end())
        {
            SWSS_LOG_ERROR("%s, Failed to create offload session %s: destination MAC address required when hardware lookup not valid",
                            name.c_str(), m_key.c_str());
            return SaiOffloadHandlerStatus::FAILED_INVALID_ENTRY;
        }

        // supported only for default vrf
        if (m_vrf_name != "default")
        {
            SWSS_LOG_ERROR("%s, Failed to create offload session %s: vrf is not supported when hardware lookup not valid",
                            name.c_str(), m_key.c_str());
            return SaiOffloadHandlerStatus::FAILED_INVALID_ENTRY;
        }

        sai_attribute_value_t val;
        val.booldata = false;
        m_attr_val_map[hw_lookup_attr_id] = val;

        sai_attribute_value_t val_port;
        val_port.oid = port.m_port_id;
        m_attr_val_map[port_attr_id] = val_port;

        sai_attribute_value_t val_smac;
        auto smac_it = m_attr_val_map.find(src_mac_attr_id);
        if (smac_it == m_attr_val_map.end())
        {
            memcpy(val_smac.mac, port.m_mac.getMac(), sizeof(sai_mac_t));
        }
        else
        {
            val_smac = smac_it->second;
        }
        m_attr_val_map[src_mac_attr_id] = val_smac;
    }
    else
    {
        // dmac is obtained by hardware lookup
        if (dmac_it != m_attr_val_map.end())
        {
            SWSS_LOG_ERROR("%s, Failed to create session %s: destination MAC address not supported when hardware lookup valid",
                            name.c_str(), m_key.c_str());
            return SaiOffloadHandlerStatus::FAILED_INVALID_ENTRY;
        }

        // vrf id needed when hardware lookup is enabled
        sai_attribute_value_t vrf_val;
        if (m_vrf_name == "default")
        {
            vrf_val.oid = gVirtualRouterId;
        }
        else
        {
            VRFOrch* vrf_orch = gDirectory.get<VRFOrch*>();
            vrf_val.oid = vrf_orch->getVRFid(m_vrf_name);
        }
        m_attr_val_map[vrf_attr_id] = vrf_val;

        sai_attribute_value_t hw_val;
        hw_val.booldata = true;
        m_attr_val_map[hw_lookup_attr_id] = hw_val;
    }

    return SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY;
}

template <class SaiOrchHandlerClass, typename T>
SaiOffloadHandlerStatus SaiOffloadSessionHandler<SaiOrchHandlerClass, T>::remove(sai_object_id_t id)
{
    constexpr auto& name = static_cast<SaiOrchHandlerClass *>(this)->m_name;
    constexpr auto atype = static_cast<sai_api_t>(SaiOrchHandlerClass::SAI_API_TYPE::API_TYPE);

    sai_status_t status = sai_remove_session(id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("%s, Failed to remove offload session %s, rv:%d", name.c_str(),
                m_key.c_str(), status);
        task_process_status handle_status = handleSaiRemoveStatus(atype, status);
        if (handle_status != task_success)
        {
            // check for retries
            if (parseHandleSaiStatusFailure(handle_status))
            {
                return SaiOffloadHandlerStatus::FAILED_VALID_ENTRY;
            }
            else
            {
                return SaiOffloadHandlerStatus::RETRY_VALID_ENTRY;
            }
        }
    }

    // call the derived orch's remove
    auto do_remove_status = static_cast<SaiOrchHandlerClass *>(this)->do_remove();
    if (do_remove_status != SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY)
    {
        return do_remove_status;
    }

    return SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY;
}

template <class SaiOrchHandlerClass, typename T>
SaiOffloadHandlerStatus SaiOffloadSessionHandler<SaiOrchHandlerClass, T>::update(sai_object_id_t session_id, const fv_vector_t& fv_data, const fv_map_t& fv_map)
{
    constexpr auto& name = static_cast<SaiOrchHandlerClass *>(this)->m_name;
    auto& handler_map = static_cast<SaiOrchHandlerClass *>(this)->m_handler_map;
    auto& update_fields = static_cast<SaiOrchHandlerClass *>(this)->m_update_fields;

    m_data = fv_data;
    m_session_id = session_id;

    // call the handler for field if updatable and
    // fill the m_attr_val_map and m_fv_vector
    for (auto& data : m_data)
    {
        auto field = fvField(data);
        auto value = fvValue(data);
        m_fv_map[field] = value;

        // check for new update field
        if (fv_map.find(field) == fv_map.end())
        {
            SWSS_LOG_ERROR("%s, Unsupported new field update %s:%s for %s",
                    name.c_str(), field.c_str(), value.c_str(), m_key.c_str());
            return SaiOffloadHandlerStatus::FAILED_INVALID_ENTRY;
        }

        // check if field needs update
        if (fv_map.at(field) == value)
        {
            continue;
        }

        // check if this field update supported
        if (update_fields.find(field) == update_fields.end())
        {
            SWSS_LOG_ERROR("%s, Unsupported field update %s:%s for %s",
                    name.c_str(), field.c_str(), value.c_str(), m_key.c_str());
            return SaiOffloadHandlerStatus::FAILED_INVALID_ENTRY;
        }

        SWSS_LOG_INFO("%s, field update %s:%s for %s", name.c_str(),
                field.c_str(), value.c_str(), m_key.c_str());

        auto hsearch = handler_map.find(field);
        if (hsearch != handler_map.end())
        {
            auto& htuple = hsearch->second;
            auto& handler = std::get<1>(htuple);
            handler(value, m_attr_val_map, m_fv_vector);
        }
        else
        {
            SWSS_LOG_ERROR("%s, Unsupported sai attribute handler field %s for %s",
                    name.c_str(), field.c_str(), m_key.c_str());
        }
    }

    // call the derived orch's update
    auto do_update_status = static_cast<SaiOrchHandlerClass *>(this)->do_update();
    if (do_update_status != SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY)
    {
        return do_update_status;
    }

    // update the session attributes
    // for the sai attribute vector for create
    sai_attribute_t attr;
    for (auto it = m_attr_val_map.begin(); it != m_attr_val_map.end(); it++)
    {
        attr.id = it->first;
        attr.value = it->second;

        sai_status_t status = sai_set_session_attrib(m_session_id, &attr);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("%s, SAI offload session attrib id %u set failed %s, rv:%d",
                    name.c_str(), attr.id, m_key.c_str(), status);
            return SaiOffloadHandlerStatus::FAILED_VALID_ENTRY;
        }
    }

    return SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY;
}

template <class SaiOrchHandlerClass, typename T>
bool SaiOffloadSessionHandler<SaiOrchHandlerClass, T>::register_state_change_notification()
{
    constexpr auto& name = static_cast<SaiOrchHandlerClass *>(this)->m_name;
    constexpr auto notify_attr_id = static_cast<sai_attr_id_t>(SaiOrchHandlerClass::SAI_NOTIF_ATTR_ID::STATE_CHANGE);
    sai_attribute_t  attr;
    sai_status_t status;
    sai_attr_capability_t capability;

    status = sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_SWITCH, 
                                            notify_attr_id,
                                            &capability);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("%s, Unable to query the change notification capability", name.c_str());
        return false;
    }

    if (!capability.set_implemented)
    {
        SWSS_LOG_ERROR("%s, register change notification not supported", name.c_str());
        return false;
    }

    attr.id = notify_attr_id;
    attr.value.ptr = (void *)&SaiOrchHandlerClass::on_state_change;

    status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("%s, Failed to register notification handler", name.c_str());
        return false;
    }
    return true;
}

#endif
