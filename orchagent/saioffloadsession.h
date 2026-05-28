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
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstring>
#include "portsorch.h"
#include "vrforch.h"
#include "dbconnector.h"
#include "table.h"
#include "select.h"
#include "timer.h"
#include "sai_serialize.h"
#include "flex_counter/flex_counter_manager.h"

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
extern sai_counter_api_t*   sai_counter_api;
extern Directory<Orch*>     gDirectory;
extern bool                 gTraditionalFlexCounter;

/**
 *@struct SelectiveCounterVariant
 *
 *@brief One direction/aspect of a per-session selective counter. Each
 *       variant becomes a distinct SAI counter aggregating stat_ids via
 *       SAI_COUNTER_ATTR_STAT_ID_LIST. The suffix (e.g. "IN", "OUT") is
 *       appended to the SAI counter label and to the COUNTERS_DB name-map
 *       key; "<base_label>-<suffix>" must fit in SAI_HOSTIF_NAME_SIZE - 1.
 */
struct SelectiveCounterVariant
{
    std::string suffix;
    std::vector<sai_stat_id_t> stat_ids;
};

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

/**
 *@class SaiOffloadStatsHandler
 *
 *@brief Manages per-session SAI counters for offload sessions (ICMP Echo,
 *       BFD, ...) and registers them with FlexCounterManager so syncd
 *       polls them into COUNTERS_DB.
 *
 *       Mode is selected at initialize() based on platform capability:
 *       - Selective-counter mode: creates one SAI counter per
 *         Derived::m_selective_counter_variants, attaches via
 *         SAI_*_ATTR_SELECTIVE_COUNTER_LIST, polls SAI_COUNTER_STAT_*.
 *       - Native mode (fallback): registers the session OID itself with
 *         FlexCounterManager using Derived::native_counter_{type,stats}().
 *
 *       Derived must provide:
 *         - session_object_type, SAI_ATTR_ID::COUNTER_LIST_ID, m_name
 *         - m_selective_counter_variants (>= 1 entry)
 *         - make_counter_label() returning a printable ASCII base label
 *           (<= SAI_HOSTIF_NAME_SIZE - 1 - len("-<suffix>"))
 *         - native_counter_type() / native_counter_stats()
 *         - set_session_attribute() wrapper over the object-type SAI API
 */
template <class Derived, typename T>
class SaiOffloadStatsHandler
{
public:
    using Traits = SaiOffloadHandlerTraits<T>;
    using api_t = typename Traits::api_t;

    SaiOffloadStatsHandler(const std::string& flex_counter_group,
                           const std::string& counter_name_map,
                           uint32_t polling_interval_ms,
                           uint32_t update_timer_interval_sec = 1)
        : m_group_name(flex_counter_group),
          m_name_map_name(counter_name_map),
          m_polling_interval_ms(polling_interval_ms),
          m_update_timer_interval_sec(update_timer_interval_sec),
          m_counter_manager(flex_counter_group, StatsMode::READ, polling_interval_ms, false)
    {
    }

    /**
     *@method default_counter_label
     *
     *@brief Generic SAI counter label builder; returns the trailing
     *       SAI_HOSTIF_NAME_SIZE-1 chars of session_key. Deriveds with
     *       embedded GUIDs should override make_counter_label() since
     *       trailing truncation is not guaranteed to be unique.
     */
    static std::string default_counter_label(const std::string& session_key)
    {
        constexpr size_t kMaxLabelLen = SAI_HOSTIF_NAME_SIZE - 1;
        if (session_key.size() <= kMaxLabelLen)
        {
            return session_key;
        }
        return session_key.substr(session_key.size() - kMaxLabelLen);
    }

    /**
     *@method initialize
     *
     *@brief Query capability, pick mode, set up DB connectors. Call once
     *       before adding counters. Returns true if counters are usable.
     */
    bool initialize()
    {
        if (m_initialized)
        {
            return m_supported;
        }
        m_initialized = true;

        if (!resolve_stats_count_mode())
        {
            SWSS_LOG_WARN("%s, stats count mode resolution failed; sessions will be "
                          "created without explicit stats count mode",
                          Derived::m_name.c_str());
        }

        if (query_capability())
        {
            m_supported = true;
        }
        else
        {
            // Selective counter unsupported; fall back to native FlexCounter.
            m_native_mode = true;
            m_supported = true;
            SWSS_LOG_NOTICE("%s using native FlexCounter fallback "
                            "(selective counter unsupported on this platform)",
                            Derived::m_name.c_str());
        }

        m_asic_db = std::make_shared<DBConnector>("ASIC_DB", 0);
        m_counter_db = std::make_shared<DBConnector>("COUNTERS_DB", 0);
        m_counters_name_map = std::make_unique<Table>(m_counter_db.get(), m_name_map_name);

        if (gTraditionalFlexCounter)
        {
            m_vid_to_rid_table = std::make_unique<Table>(m_asic_db.get(), "VIDTORID");
        }

        return true;
    }

    /**
     *@method applyStatsCountMode
     *
     *@brief If the platform reported a supported stats count mode at
     *       initialize() time, inject it into the session create attribute
     *       map under Derived::SAI_ATTR_ID::COUNT_MODE_ID. Logs a warning
     *       and is a no-op when the capability was not resolved.
     */
    void applyStatsCountMode(sai_attr_id_val_map_t& attr_val_map) const
    {
        if (!m_stats_count_mode_initialized)
        {
            SWSS_LOG_WARN("%s, Stats count mode capability unresolved",
                          Derived::m_name.c_str());
            return;
        }

        constexpr sai_attr_id_t count_mode_attr =
            static_cast<sai_attr_id_t>(Derived::SAI_ATTR_ID::COUNT_MODE_ID);

        sai_attribute_value_t val{};
        val.s32 = m_stats_count_mode;
        attr_val_map[count_mode_attr] = val;
    }

    /**
     *@method createUpdateTimer
     *
     *@brief Returns a SelectableTimer used by the owning orch to drive
     *       processPending() in traditional flex counter mode. Caller
     *       registers it via Orch::addExecutor.
     */
    SelectableTimer* createUpdateTimer()
    {
        if (m_counter_update_timer == nullptr)
        {
            m_counter_update_timer = new SelectableTimer(
                timespec{ .tv_sec = m_update_timer_interval_sec, .tv_nsec = 0 });
        }
        return m_counter_update_timer;
    }

    /**
     *@method addSession
     *
     *@brief Create one SAI selective counter per variant, attach them via
     *       SAI_*_ATTR_SELECTIVE_COUNTER_LIST, record name-map entries,
     *       and register with FlexCounterManager (deferred in traditional
     *       mode until VID->RID maps). Rolls back on failure.
     */
    bool addSession(const std::string& session_key, sai_object_id_t session_id, api_t* api)
    {
        SWSS_LOG_ENTER();

        if (!m_enabled)
        {
            return false;
        }

        if (m_native_mode)
        {
            return addSessionNative(session_key, session_id);
        }

        const auto& variants = Derived::m_selective_counter_variants;
        if (variants.empty())
        {
            SWSS_LOG_ERROR("%s, no selective counter variants declared by Derived; "
                           "refusing to addSession(%s)",
                    Derived::m_name.c_str(), session_key.c_str());
            return false;
        }

        std::vector<sai_object_id_t> oids;
        oids.reserve(variants.size());

        for (const auto& v : variants)
        {
            sai_object_id_t oid = SAI_NULL_OBJECT_ID;
            if (!create_selective_counter(oid, session_key, v))
            {
                SWSS_LOG_ERROR("%s, Failed to create selective counter for %s "
                               "variant '%s'",
                        Derived::m_name.c_str(), session_key.c_str(),
                        v.suffix.c_str());
                for (auto created : oids)
                {
                    remove_selective_counter(created);
                }
                return false;
            }
            oids.push_back(oid);
        }

        if (!attach_counters_to_session(api, session_id, oids))
        {
            for (auto oid : oids)
            {
                remove_selective_counter(oid);
            }
            return false;
        }

        // Persist <session_key>|<SUFFIX> -> oid; '|' avoids colliding with
        // the ':' tokens already present in session_key.
        std::vector<FieldValueTuple> fvs;
        fvs.reserve(variants.size());
        for (size_t i = 0; i < variants.size(); ++i)
        {
            fvs.emplace_back(make_name_map_key(session_key, variants[i].suffix),
                             sai_serialize_object_id(oids[i]));
        }
        m_counters_name_map->set("", fvs);

        m_session_counters[session_key] = oids;

        if (!gTraditionalFlexCounter)
        {
            std::unordered_set<std::string> counter_stats;
            get_counter_stat_id_list(counter_stats);
            for (auto oid : oids)
            {
                m_counter_manager.setCounterIdList(oid, CounterType::OFFLOAD_SESSION, counter_stats);
            }
        }
        else
        {
            bool was_empty = m_pending_counters.empty();
            for (auto oid : oids)
            {
                m_pending_counters[oid] = session_key;
            }
            if (was_empty && !m_pending_counters.empty() &&
                m_counter_update_timer != nullptr)
            {
                m_counter_update_timer->start();
            }
        }

        return true;
    }

    /**
     *@method removeSession
     *
     *@brief Detach counters from the session, clear name-map entries,
     *       unregister from FlexCounterManager, and destroy SAI counters.
     */
    void removeSession(const std::string& session_key, sai_object_id_t session_id, api_t* api)
    {
        SWSS_LOG_ENTER();

        auto it = m_session_counters.find(session_key);
        if (it == m_session_counters.end())
        {
            return;
        }
        const auto oids = it->second;  // copy: we erase below

        if (m_native_mode)
        {
            // oids[0] is the session OID itself; nothing to detach/destroy.
            m_counters_name_map->hdel("", session_key);
            for (auto oid : oids)
            {
                bool was_pending = m_pending_counters.erase(oid) == 1;
                if (!was_pending)
                {
                    m_counter_manager.clearCounterIdList(oid);
                }
            }
            m_session_counters.erase(it);
            return;
        }

        detach_counter_from_session(api, session_id);

        const auto& variants = Derived::m_selective_counter_variants;
        for (const auto& v : variants)
        {
            m_counters_name_map->hdel("",
                    make_name_map_key(session_key, v.suffix));
        }

        for (auto oid : oids)
        {
            bool was_pending = m_pending_counters.erase(oid) == 1;
            if (!was_pending)
            {
                m_counter_manager.clearCounterIdList(oid);
            }
            remove_selective_counter(oid);
        }
        m_session_counters.erase(it);
    }

    /**
     *@method processPending
     *
     *@brief Register pending counters with FlexCounterManager once their
     *       VID->RID mapping resolves. Driven by the update timer.
     */
    void processPending()
    {
        SWSS_LOG_ENTER();

        const CounterType counter_type = m_native_mode
            ? Derived::native_counter_type()
            : CounterType::OFFLOAD_SESSION;
        std::unordered_set<std::string> counter_stats;
        if (m_native_mode)
        {
            counter_stats = Derived::native_counter_stats();
        }
        else
        {
            get_counter_stat_id_list(counter_stats);
        }

        std::string value;
        for (auto it = m_pending_counters.begin(); it != m_pending_counters.end();)
        {
            const auto oid_str = sai_serialize_object_id(it->first);
            if (!gTraditionalFlexCounter ||
                (m_vid_to_rid_table && m_vid_to_rid_table->hget("", oid_str, value)))
            {
                m_counter_manager.setCounterIdList(it->first, counter_type, counter_stats);
                it = m_pending_counters.erase(it);
            }
            else
            {
                ++it;
            }
        }

        if (m_pending_counters.empty() && m_counter_update_timer != nullptr)
        {
            m_counter_update_timer->stop();
        }
    }

    /**
     *@method setState
     *
     *@brief Enable/disable counter collection across existing_sessions.
     *       Idempotent.
     */
    void setState(bool enable,
                  const std::map<std::string, sai_object_id_t>& existing_sessions,
                  api_t* api)
    {
        SWSS_LOG_ENTER();

        if (!m_supported)
        {
            SWSS_LOG_WARN("%s selective counters not supported; ignoring state change",
                    Derived::m_name.c_str());
            return;
        }

        if (enable == m_enabled)
        {
            return;
        }

        SWSS_LOG_NOTICE("%s selective counters state -> %s",
                Derived::m_name.c_str(), enable ? "enabled" : "disabled");

        m_enabled = enable;

        if (enable)
        {
            for (const auto& kv : existing_sessions)
            {
                addSession(kv.first, kv.second, api);
            }
        }
        else
        {
            auto to_remove = m_session_counters;
            for (const auto& kv : to_remove)
            {
                auto sit = existing_sessions.find(kv.first);
                sai_object_id_t session_id = (sit != existing_sessions.end()) ? sit->second : SAI_NULL_OBJECT_ID;
                removeSession(kv.first, session_id, api);
            }
        }
    }

    bool isEnabled()   const { return m_enabled; }
    bool isSupported() const { return m_supported; }

private:
    bool query_capability() const
    {
        sai_attr_capability_t capability;
        constexpr sai_attr_id_t counter_list_attr =
            static_cast<sai_attr_id_t>(Derived::SAI_ATTR_ID::COUNTER_LIST_ID);
        sai_status_t status = sai_query_attribute_capability(
            gSwitchId, Derived::session_object_type, counter_list_attr, &capability);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN("%s, Failed to query selective counter capability: %d",
                    Derived::m_name.c_str(), status);
            return false;
        }
        return capability.set_implemented && capability.create_implemented;
    }

    /**
     *@method resolve_stats_count_mode
     *
     *@brief Query the platform for the supported enum values of
     *       Derived::SAI_ATTR_ID::COUNT_MODE_ID on Derived::session_object_type
     *       and pick the most informative supported mode. Result is stored
     *       in m_stats_count_mode for later use by applyStatsCountMode().
     */
    bool resolve_stats_count_mode()
    {
        m_stats_count_mode_initialized = false;

        constexpr sai_attr_id_t count_mode_attr =
            static_cast<sai_attr_id_t>(Derived::SAI_ATTR_ID::COUNT_MODE_ID);

        const auto *meta = sai_metadata_get_attr_metadata(
            Derived::session_object_type, count_mode_attr);
        if (!meta || !meta->isenum)
        {
            SWSS_LOG_WARN("%s, sai_metadata_get_attr_metadata for stats count mode failed",
                    Derived::m_name.c_str());
            return false;
        }

        std::vector<int32_t> values_list(meta->enummetadata->valuescount);
        sai_s32_list_t values;
        values.count = static_cast<uint32_t>(values_list.size());
        values.list = values_list.data();

        sai_status_t status = sai_query_attribute_enum_values_capability(
            gSwitchId,
            Derived::session_object_type,
            count_mode_attr,
            &values);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN("%s, sai_query_attribute_enum_values_capability for stats count mode failed",
                    Derived::m_name.c_str());
            return false;
        }

        auto *end = values.list + values.count;

        static const sai_stats_count_mode_t preferred_modes[] = {
            SAI_STATS_COUNT_MODE_PACKET_AND_BYTE,
            SAI_STATS_COUNT_MODE_PACKET,
            SAI_STATS_COUNT_MODE_BYTE,
            SAI_STATS_COUNT_MODE_NONE,
        };

        for (auto mode : preferred_modes)
        {
            if (std::find(values.list, end, static_cast<int32_t>(mode)) != end)
            {
                m_stats_count_mode = mode;
                m_stats_count_mode_initialized = true;
                return true;
            }
        }

        SWSS_LOG_WARN("%s, No supported stats count mode found",
                Derived::m_name.c_str());
        return false;
    }

    /**
     *@method make_name_map_key
     *
     *@brief Build "<session_key>|<variant_suffix>" name-map field. '|'
     *       avoids colliding with the ':' tokens inside session_key.
     */
    static std::string make_name_map_key(const std::string& session_key,
                                         const std::string& variant_suffix)
    {
        return session_key + "|" + variant_suffix;
    }

    bool create_selective_counter(sai_object_id_t& counter_oid,
                                  const std::string& session_key,
                                  const SelectiveCounterVariant& variant)
    {
        std::vector<sai_attribute_t> attrs;

        sai_attribute_t type_attr{};
        type_attr.id = SAI_COUNTER_ATTR_TYPE;
        type_attr.value.s32 = SAI_COUNTER_TYPE_SELECTIVE;
        attrs.push_back(type_attr);

        sai_attribute_t obj_type_attr{};
        obj_type_attr.id = SAI_COUNTER_ATTR_OBJECT_TYPE;
        obj_type_attr.value.s32 = Derived::session_object_type;
        attrs.push_back(obj_type_attr);

        std::vector<int32_t> id_list;
        id_list.reserve(variant.stat_ids.size());
        for (auto id : variant.stat_ids)
        {
            id_list.push_back(static_cast<int32_t>(id));
        }

        sai_attribute_t stat_ids_attr{};
        stat_ids_attr.id = SAI_COUNTER_ATTR_STAT_ID_LIST;
        stat_ids_attr.value.s32list.count = static_cast<uint32_t>(id_list.size());
        stat_ids_attr.value.s32list.list = id_list.data();
        attrs.push_back(stat_ids_attr);

        sai_attribute_t label_attr{};
        label_attr.id = SAI_COUNTER_ATTR_LABEL;

        // SAI Meta caps every CHARDATA attribute at SAI_HOSTIF_NAME_SIZE-1
        // printable ASCII chars; longer values are rejected with the
        // misleading "host interface name is too long" error. The full
        // session_key is still recorded in COUNTERS_DB name map.
        constexpr size_t kMaxLabelLen = SAI_HOSTIF_NAME_SIZE - 1;
        std::string base_label = Derived::make_counter_label(session_key);

        // Truncate the base, not the suffix, so the direction stays visible.
        const std::string suffix_part = variant.suffix.empty()
            ? std::string()
            : ("-" + variant.suffix);
        if (base_label.size() + suffix_part.size() > kMaxLabelLen)
        {
            const size_t budget = (suffix_part.size() < kMaxLabelLen)
                ? kMaxLabelLen - suffix_part.size()
                : 0;
            base_label.resize(budget);
        }
        const std::string compact_label = base_label + suffix_part;
        const size_t copy_len = std::min(compact_label.size(), kMaxLabelLen);

        // Reject non-printable bytes early; Meta would otherwise fail the
        // create with a generic invalid-parameter.
        for (size_t i = 0; i < copy_len; ++i)
        {
            char c = compact_label[i];
            if (c < 0x20 || c > 0x7e)
            {
                SWSS_LOG_ERROR(
                    "%s, counter label '%s' for session '%s' contains "
                    "non-printable character 0x%02x; aborting create",
                    Derived::m_name.c_str(),
                    compact_label.c_str(), session_key.c_str(),
                    static_cast<unsigned char>(c));
                return false;
            }
        }

        std::memcpy(label_attr.value.chardata, compact_label.data(), copy_len);
        label_attr.value.chardata[copy_len] = '\0';

        if (compact_label.size() > kMaxLabelLen)
        {
            SWSS_LOG_WARN(
                "%s, counter label '%s' (from session '%s') truncated "
                "to %zu chars to satisfy SAI Meta CHARDATA limit",
                Derived::m_name.c_str(),
                compact_label.c_str(), session_key.c_str(), kMaxLabelLen);
        }
        attrs.push_back(label_attr);

        sai_status_t status = sai_counter_api->create_counter(
            &counter_oid, gSwitchId,
            static_cast<uint32_t>(attrs.size()), attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("%s, Failed to create SAI selective counter "
                           "(session=%s variant=%s): %d",
                    Derived::m_name.c_str(),
                    session_key.c_str(), variant.suffix.c_str(), status);
            return false;
        }
        return true;
    }

    bool remove_selective_counter(sai_object_id_t counter_oid)
    {
        if (counter_oid == SAI_NULL_OBJECT_ID)
        {
            return true;
        }
        sai_status_t status = sai_counter_api->remove_counter(counter_oid);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("%s, Failed to remove SAI selective counter %s: %d",
                    Derived::m_name.c_str(),
                    sai_serialize_object_id(counter_oid).c_str(), status);
            return false;
        }
        return true;
    }

    bool attach_counters_to_session(api_t* api, sai_object_id_t session_id,
                                    const std::vector<sai_object_id_t>& counter_oids)
    {
        constexpr sai_attr_id_t counter_list_attr =
            static_cast<sai_attr_id_t>(Derived::SAI_ATTR_ID::COUNTER_LIST_ID);

        sai_attribute_t attr{};
        attr.id = counter_list_attr;
        // const_cast is safe: SAI only reads objlist.list during the set.
        attr.value.objlist.count = static_cast<uint32_t>(counter_oids.size());
        attr.value.objlist.list  = const_cast<sai_object_id_t *>(counter_oids.data());

        sai_status_t status = Derived::set_session_attribute(api, session_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("%s, Failed to attach %zu counter(s) to session %s: %d",
                    Derived::m_name.c_str(), counter_oids.size(),
                    sai_serialize_object_id(session_id).c_str(), status);
            return false;
        }
        return true;
    }

    bool detach_counter_from_session(api_t* api, sai_object_id_t session_id)
    {
        if (session_id == SAI_NULL_OBJECT_ID)
        {
            return true;
        }

        constexpr sai_attr_id_t counter_list_attr =
            static_cast<sai_attr_id_t>(Derived::SAI_ATTR_ID::COUNTER_LIST_ID);

        sai_attribute_t attr{};
        attr.id = counter_list_attr;
        attr.value.objlist.count = 0;
        attr.value.objlist.list = nullptr;

        sai_status_t status = Derived::set_session_attribute(api, session_id, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN("%s, Failed to detach counter from session %s: %d",
                    Derived::m_name.c_str(),
                    sai_serialize_object_id(session_id).c_str(), status);
            return false;
        }
        return true;
    }

    static void get_counter_stat_id_list(std::unordered_set<std::string>& counter_stats)
    {
        static const sai_counter_stat_t s_ids[] = {
            SAI_COUNTER_STAT_PACKETS,
            SAI_COUNTER_STAT_BYTES,
        };
        for (auto id : s_ids)
        {
            counter_stats.emplace(sai_serialize_counter_stat(id));
        }
    }

    /**
     *@method addSessionNative
     *
     *@brief Native FlexCounter path: register the session OID itself with
     *       Derived::native_counter_{type,stats}(). syncd polls via
     *       sai_get_<obj>_session_stats_ext() into COUNTERS_DB.
     */
    bool addSessionNative(const std::string& session_key, sai_object_id_t session_id)
    {
        if (session_id == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("%s, addSessionNative(%s) called with NULL session OID",
                    Derived::m_name.c_str(), session_key.c_str());
            return false;
        }

        auto counter_stats = Derived::native_counter_stats();
        if (counter_stats.empty())
        {
            SWSS_LOG_ERROR("%s, Derived::native_counter_stats() returned empty; "
                           "refusing to addSession(%s)",
                    Derived::m_name.c_str(), session_key.c_str());
            return false;
        }

        std::vector<FieldValueTuple> fvs;
        fvs.emplace_back(session_key, sai_serialize_object_id(session_id));
        m_counters_name_map->set("", fvs);

        m_session_counters[session_key] = { session_id };

        if (!gTraditionalFlexCounter)
        {
            m_counter_manager.setCounterIdList(session_id,
                                               Derived::native_counter_type(),
                                               counter_stats);
        }
        else
        {
            bool was_empty = m_pending_counters.empty();
            m_pending_counters[session_id] = session_key;
            if (was_empty && !m_pending_counters.empty() &&
                m_counter_update_timer != nullptr)
            {
                m_counter_update_timer->start();
            }
        }

        return true;
    }

    std::string m_group_name;
    std::string m_name_map_name;
    uint32_t    m_polling_interval_ms;
    uint32_t    m_update_timer_interval_sec;

    FlexCounterManager m_counter_manager;
    std::unique_ptr<Table> m_counters_name_map;
    std::unique_ptr<Table> m_vid_to_rid_table;
    std::shared_ptr<DBConnector> m_counter_db;
    std::shared_ptr<DBConnector> m_asic_db;

    // session_key -> counter OIDs (one per variant, same order).
    std::map<std::string, std::vector<sai_object_id_t>> m_session_counters;
    // counter OID -> owning session_key while awaiting VID->RID resolution.
    std::map<sai_object_id_t, std::string> m_pending_counters;
    SelectableTimer* m_counter_update_timer = nullptr;

    bool m_enabled     = false;
    bool m_supported   = false;
    bool m_initialized = false;
    // Selective counter unsupported; register session OIDs directly with
    // FlexCounterManager and skip SAI counter create/attach.
    bool m_native_mode = false;

    // Resolved per-platform stats count mode reused across session creates.
    sai_stats_count_mode_t m_stats_count_mode = SAI_STATS_COUNT_MODE_PACKET;
    bool m_stats_count_mode_initialized = false;
};

#endif
