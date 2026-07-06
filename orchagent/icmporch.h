/*
 * icmporch.h
 *
 *  Created on: Feb 21, 2025
 *      Author: Manas Kumar Mandal
 */
#ifndef SWSS_ICMPORCH_H
#define SWSS_ICMPORCH_H

#include "orch.h"
#include "observer.h"
#include "saioffloadsession.h"
#include <vector>
#include <tuple>
#include <string>
#include <cctype>

extern sai_icmp_echo_api_t* sai_icmp_echo_api;

#define ICMP_SESSION_STAT_COUNTER_FLEX_COUNTER_GROUP "ICMP_SESSION_STAT_COUNTER"
#define COUNTERS_ICMP_ECHO_SESSION_NAME_MAP          "COUNTERS_ICMP_ECHO_SESSION_NAME_MAP"
#define ICMP_SESSION_STAT_COUNTER_POLLING_INTERVAL_MS 10000
#define ICMP_SESSION_FLEX_COUNTER_UPDATE_TIMER_SEC    1

extern void sai_deserialize_icmp_session_state_ntf( const std::string& s,
        uint32_t &count,
        sai_icmp_echo_session_state_notification_t** bfd_session_state);
extern void sai_deserialize_free_icmp_session_state_ntf(uint32_t count,
        sai_icmp_echo_session_state_notification_t* icmp_session_state);

constexpr uint32_t time_msec_to_usec(const uint32_t val)
{
    return val * 1000;
}

/**
 *@struct IcmpUpdate
 *
 *@brief structure used for mapping icmp session id and state
 */
struct IcmpUpdate
{
    std::string db_key;
    sai_icmp_echo_session_state_t state;
    bool init_state;
};

/**
 *@struct IcmpSessionDataCache
 *
 *@brief structure used for mapping icmp session key and session data
 */
struct IcmpSessionDataCache
{
    sai_object_id_t session_id;
    fv_map_t fv_map;
};

// forward declaration of icmp sai handler
struct IcmpSaiSessionHandler;

/**
 *@class IcmpOrch
 *
 *@brief Orchestrator class that handles ICMP sessions
 */
class IcmpOrch: public Orch, public Subject
{
public:
    /**
     *@method IcmpOrch
     *
     *@brief class constructor
     *
     *@param db(in) pointer to DBConnector object
     *@param tableName(in) consumer table name
     *@param stateDbIcmpSessionTable(in) producer state db table
     */
    IcmpOrch(swss::DBConnector *db, std::string tableName, TableConnector stateDbIcmpSessionTable);

    /**
     *@method ~IcmpOrch
     *
     *@brief class destructor
     */
    virtual ~IcmpOrch(void);

    /**
     *@method doTask
     *
     *@brief overriden method that consumes SET/DEL
     *       operations on consumer table entries
     *
     *@param consumer(in) reference to consumer
     */
    void doTask(Consumer &consumer) override;

    /**
     *@method doTask
     *
     *@brief overriden method that consumes notifications
     *       from asic db
     *
     *@param consumer(in) reference to notification consumer
     */
    void doTask(swss::NotificationConsumer &consumer) override;

    /**
     *@method doTask
     *
     *@brief Drives pending counter registration in traditional flex-counter mode.
     */
    void doTask(swss::SelectableTimer &timer) override;

    /**
     *@method setCountersState
     *
     *@brief Enable/disable ICMP echo session counters from FLEX_COUNTER_GROUP state.
     */
    void setCountersState(bool enable);

    // friend handler have access to IcmpOrch
    friend struct IcmpSaiSessionHandler;

    static inline std::string get_state_db_key(const std::string& vrf, const std::string& alias,
            const std::string& guid, const std::string& session_type) {
        return vrf + state_db_key_delimiter + alias + state_db_key_delimiter + guid +
               state_db_key_delimiter + session_type;
    }

private:

    /**
     *@method create_icmp_session
     *
     *@brief creates icmp echo sessions in hardware
     *
     *@param key(in)  reference to session key
     *@param data(in) vector of session parameters from APP_DB
     *                table as field value tuples
     *
     *@return false for retries
     *        true for all other cases where session entry is consumed
     */
    bool create_icmp_session(const string& key, const vector<FieldValueTuple>& data);

    /**
     *@method remove_icmp_session
     *
     *@brief removes icmp echo sessions from hardware
     *
     *@param key(in)  reference to session key
     *
     *@return false for retries
     *        true for all other cases
     */
    bool remove_icmp_session(const string& key);

    /**
     *@method update_icmp_session
     *
     *@brief updates icmp echo sessions in hardware
     *
     *@param key(in)  reference to session key
     *@param data(in) vector of session parameters from APP_DB
     *                table as field value tuples
     *
     *@return false for retries
     *        true for all other cases where session entry is consumed
     */
    bool update_icmp_session(const string& key, const vector<FieldValueTuple>& data);

    /**
     *@method initializeCounters
     *
     *@brief Construct the stats handler and query platform capability.
     */
    void initializeCounters();

    /**
     *@method get_existing_session_map
     *
     *@brief session_key -> SAI session OID snapshot for setState().
     */
    std::map<std::string, sai_object_id_t> get_existing_session_map() const;

    // map of session key to session data cache
    std::map<std::string, IcmpSessionDataCache> m_icmp_session_map;
    // map of session object id to update data for handling notification from asic db 
    std::map<sai_object_id_t, IcmpUpdate> m_icmp_session_lookup;

    // Icmp session state table produced by IcmpOrch
    swss::Table m_stateIcmpSessionTable;

    // ASIC_DB ICMP state notification consumer
    swss::NotificationConsumer* m_icmpStateNotificationConsumer;
    // indicates notification registration is done
    bool m_register_state_change_notif;

    // keeps track of number of sessions
    uint32_t m_num_sessions = 0;

    // max number of sessions
    static const uint32_t m_max_sessions;

    // map of sai icmp session state to string
    static const std::map<sai_icmp_echo_session_state_t, std::string> m_session_state_lkup;
    // map of icmp session state string to sai icmp session state
    static const std::map<std::string, sai_icmp_echo_session_state_t> m_session_state_str_lkup;

    // Manages per-session SAI selective counters and FlexCounterManager
    // registration; instantiated once capability has been queried.
    std::unique_ptr<SaiOffloadStatsHandler<IcmpSaiSessionHandler, sai_icmp_echo_api_t>> m_stats_handler;
};

/**
 *@struct IcmpSaiSessionHandler
 *
 *@brief Sai session handler for ICMP sessions
 */
struct IcmpSaiSessionHandler : public SaiOffloadSessionHandler<IcmpSaiSessionHandler, sai_icmp_echo_api_t>
{
    /**
    *@method IcmpSaiSessionHandler
    *
    *@brief class default constructor
    */
    IcmpSaiSessionHandler() = delete;

    /**
    *@method IcmpSaiSessionHandler copy constructor
    *
    *@brief class copy constructor
    *
    *@param IcmpSaiSessionHandler(in)  reference to IcmpSaiSessionHandler object to be copied
    */
    IcmpSaiSessionHandler(const IcmpSaiSessionHandler &) = delete;

    /**
     *@method IcmpSaiSessionHandler
     *
     *@brief class constructor
     *
     *@param IcmpOrch(in) reference to IcmpOrch object
     */
    IcmpSaiSessionHandler(IcmpOrch& orch) : m_orch(orch) { }

    // enum that maps ICMP ECHO SESSION SAI attribute IDs to common SaiOffload IDs
    enum class SAI_ATTR_ID {
        HW_LOOKUP_ID    = SAI_ICMP_ECHO_SESSION_ATTR_HW_LOOKUP_VALID,
        VRF_ATTR_ID     = SAI_ICMP_ECHO_SESSION_ATTR_VIRTUAL_ROUTER,
        PORT_ID         = SAI_ICMP_ECHO_SESSION_ATTR_PORT,
        IPVER_ID        = SAI_ICMP_ECHO_SESSION_ATTR_IPHDR_VERSION,
        TX_INTERVAL_ID  = SAI_ICMP_ECHO_SESSION_ATTR_TX_INTERVAL,
        RX_INTERVAL_ID  = SAI_ICMP_ECHO_SESSION_ATTR_RX_INTERVAL,
        SRC_IP_ID       = SAI_ICMP_ECHO_SESSION_ATTR_SRC_IP_ADDRESS,
        DST_IP_ID       = SAI_ICMP_ECHO_SESSION_ATTR_DST_IP_ADDRESS,
        SRC_MAC_ID      = SAI_ICMP_ECHO_SESSION_ATTR_SRC_MAC_ADDRESS,
        DST_MAC_ID      = SAI_ICMP_ECHO_SESSION_ATTR_DST_MAC_ADDRESS,
        TOS_ID          = SAI_ICMP_ECHO_SESSION_ATTR_TOS,
        TTL_ID          = SAI_ICMP_ECHO_SESSION_ATTR_TTL,
        COUNT_MODE_ID   = SAI_ICMP_ECHO_SESSION_ATTR_STATS_COUNT_MODE,
        COUNTER_LIST_ID = SAI_ICMP_ECHO_SESSION_ATTR_SELECTIVE_COUNTER_LIST,
    };

    // enum that maps ICMP Notification attribute IDs to common SaiOffload notification IDs
    enum class SAI_NOTIF_ATTR_ID {
        STATE_CHANGE = SAI_SWITCH_ATTR_ICMP_ECHO_SESSION_STATE_CHANGE_NOTIFY,
        AVAILABLE_SESSIONS = SAI_SWITCH_ATTR_AVAILABLE_ICMP_ECHO_SESSION
    };

    // enum that maps ICMP State to common SaiOffload State
    enum class SESSION_STATE {
        STATE_DOWN    = SAI_ICMP_ECHO_SESSION_STATE_DOWN,
        STATE_UP      = SAI_ICMP_ECHO_SESSION_STATE_UP,
    };

    enum class SAI_API_TYPE {
        API_TYPE = SAI_API_ICMP_ECHO
    };

    /**
     *@method make_counter_label
     *
     *@brief Build SAI_COUNTER_ATTR_LABEL for an ICMP echo session. The
     *       full session_key exceeds SAI_HOSTIF_NAME_SIZE-1, so extract
     *       the first "0x"-prefixed hex token (the session GUID, unique
     *       per device). Falls back to trailing-char truncation if no
     *       GUID-shaped token is found. The full key is preserved in
     *       COUNTERS_ICMP_ECHO_SESSION_NAME_MAP.
     */
    static std::string make_counter_label(const std::string& session_key)
    {
        constexpr size_t kMaxLabelLen = SAI_HOSTIF_NAME_SIZE - 1;

        // Scan for the first "0x"/"0X" hex token regardless of ':' or '|'
        // separator usage.
        for (size_t i = 0; i + 2 < session_key.size(); ++i)
        {
            if (session_key[i] != '0') continue;
            if (session_key[i + 1] != 'x' && session_key[i + 1] != 'X') continue;
            if (!std::isxdigit(static_cast<unsigned char>(session_key[i + 2]))) continue;

            // Token must start at a non-alnum boundary to avoid matching
            // inside arbitrary identifiers.
            if (i > 0)
            {
                unsigned char prev = static_cast<unsigned char>(session_key[i - 1]);
                if (std::isalnum(prev)) continue;
            }

            size_t end = i + 2;
            while (end < session_key.size() &&
                   std::isxdigit(static_cast<unsigned char>(session_key[end])))
            {
                ++end;
            }

            std::string guid = session_key.substr(i, end - i);
            if (guid.size() > kMaxLabelLen)
            {
                guid.resize(kMaxLabelLen);
            }
            return guid;
        }

        return SaiOffloadStatsHandler<IcmpSaiSessionHandler,
                                      sai_icmp_echo_api_t>::default_counter_label(session_key);
    }

    /**
     *@method do_init
     *
     *@brief initializes the icmp sai session handler
     *
     *@param api(in) pointer to sai_icmp_echo_api_t
     *
     *@return SUCCESS_VALID_ENTRY when valid key and successfully initialized
     *        FAILED_INVALID_ENTRY when key is invalid
     *        FAILED_VALID_ENTRY when initialization fails for valid key
     */
    SaiOffloadHandlerStatus do_init(sai_icmp_echo_api_t *api);

    /**
     *@method do_create
     *
     *@brief auxilary create method for icmp echo session
     *
     *@return SUCCESS_VALID_ENTRY session parameters valid and created with success
     *        FAILED_INVALID_ENTRY session parameters are invalid
     *        FAILED_VALID_ENTRY session creation fails for valid key
     *        RETRY_VALID_ENTRY retry session creation for valid key
     */
    SaiOffloadHandlerStatus do_create();

    /**
     *@method do_remove
     *
     *@brief auxilary remove method for icmp echo session
     *
     *@return SUCCESS_VALID_ENTRY session id found and removed
     *        FAILED_INVALID_ENTRY session id not found
     *        FAILED_VALID_ENTRY unable to remove session for a found id
     *        RETRY_VALID_ENTRY retry session removal for a found id
     */
    SaiOffloadHandlerStatus do_remove();

    /**
     *@method do_update
     *
     *@brief auxilary update method for icmp echo session
     *
     *@return SUCCESS_VALID_ENTRY session parameters valid and updated with success
     *        FAILED_INVALID_ENTRY session parameters are invalid
     *        FAILED_VALID_ENTRY session update fails for valid key
     *        RETRY_VALID_ENTRY retry session update for valid key
     */
    SaiOffloadHandlerStatus do_update();

    // stored reference to the IcmpOrch
    IcmpOrch& m_orch;
    // icmp echo session type, NORMAL/RX
    std::string m_session_type;
    // icmp echo session guid string from key
    std::string m_guid;

    // function registered for icmp session notification
    static void on_state_change(uint32_t count, sai_icmp_echo_session_state_notification_t *data);

    // map of sai attributes and its handlers
    static sai_attr_handler_map_t m_handler_map;

    // unordered set of fields that are updatable
    static const std::unordered_set<std::string> m_update_fields; 
    // name of the icmp orch
    static const std::string m_name;

    // handlers for icmp echo session app_db fields
    static void handle_tx_interval_field(std::string& sval, sai_attr_id_val_map_t& id_val_map, fv_vector_t& fvVector);
    static void handle_rx_interval_field(std::string& sval, sai_attr_id_val_map_t& id_val_map, fv_vector_t& fvVector);
    static void handle_src_ip_field(std::string& sval, sai_attr_id_val_map_t& id_val_map, fv_vector_t& fvVector);
    static void handle_dst_ip_field(std::string& sval, sai_attr_id_val_map_t& id_val_map, fv_vector_t& fvVector);
    static void handle_src_mac_field(std::string& sval, sai_attr_id_val_map_t& id_val_map, fv_vector_t& fvVector);
    static void handle_dst_mac_field(std::string& sval, sai_attr_id_val_map_t& id_val_map, fv_vector_t& fvVector);
    static void handle_tos_field(std::string& sval, sai_attr_id_val_map_t& id_val_map, fv_vector_t& fvVector);
    static void handle_ttl_field(std::string& sval, sai_attr_id_val_map_t& id_val_map, fv_vector_t& fvVector);
    static void handle_session_guid_field(std::string& sval, sai_attr_id_val_map_t& id_val_map, fv_vector_t& fvVector);
    static void handle_session_cookie_field(std::string& sval, sai_attr_id_val_map_t& id_val_map, fv_vector_t& fvVector);
    static void handle_hw_lookup_field(std::string& sval, sai_attr_id_val_map_t& id_val_map, fv_vector_t& fvVector);

    // fieldname strings used by app_db and state_db for icmp echo sessions
    static const std::string m_tx_interval_fname;
    static const std::string m_rx_interval_fname;
    static const std::string m_src_ip_fname;
    static const std::string m_dst_ip_fname;
    static const std::string m_src_mac_fname;
    static const std::string m_dst_mac_fname;
    static const std::string m_tos_fname;
    static const std::string m_ttl_fname;
    static const std::string m_state_fname;
    static const std::string m_session_cookie_fname;
    static const std::string m_session_guid_fname;
    static const std::string m_hw_lookup_fname;
    static const std::string m_nexthop_switchover_fname;
    static const std::string m_session_type_normal;
    static const std::string m_session_type_rx;

    static const uint32_t m_max_tx_interval_usec;
    static const uint32_t m_min_tx_interval_usec;
    static const uint32_t m_max_rx_interval_usec;
    static const uint32_t m_min_rx_interval_usec;

    // Selective counter variants exposed per session. Two entries keep RX
    // and TX visible as separate rows in COUNTERS_DB / `show icmp stats`.
    static constexpr sai_object_type_t session_object_type = SAI_OBJECT_TYPE_ICMP_ECHO_SESSION;
    static const std::vector<SelectiveCounterVariant> m_selective_counter_variants;

    // Native FlexCounter fallback (required by SaiOffloadStatsHandler).
    // Used when SAI_ICMP_ECHO_SESSION_ATTR_SELECTIVE_COUNTER_LIST is not
    // implemented; syncd polls sai_get_icmp_echo_session_stats_ext() into
    // COUNTERS_DB directly.
    static CounterType native_counter_type();
    static std::unordered_set<std::string> native_counter_stats();

    // Thin wrapper so SaiOffloadStatsHandler stays API-agnostic.
    static sai_status_t set_session_attribute(sai_icmp_echo_api_t* api,
                                              sai_object_id_t session_id,
                                              const sai_attribute_t* attr);
};

#endif /* SWSS_ICMPORCH_H */
