#include "twamporch.h"
#include "vrforch.h"
#include "crmorch.h"
#include "logger.h"
#include "swssnet.h"
#include "converter.h"
#include "sai_serialize.h"
#include "tokenize.h"
#include "notifier.h"
#include "notifications.h"

#include <exception>

using namespace std;
using namespace swss;

/* TWAMP infor */
#define TWAMP_SESSION_MODE                       "MODE"
#define TWAMP_SESSION_ROLE                       "ROLE"
#define TWAMP_SESSION_VRF_NAME                   "VRF_NAME"
#define TWAMP_SESSION_HW_LOOKUP                  "HW_LOOKUP"

/* TWAMP-test packet */
#define TWAMP_SESSION_SRC_IP                     "SRC_IP"
#define TWAMP_SESSION_SRC_UDP_PORT               "SRC_UDP_PORT"
#define TWAMP_SESSION_DST_IP                     "DST_IP"
#define TWAMP_SESSION_DST_UDP_PORT               "DST_UDP_PORT"
#define TWAMP_SESSION_DSCP                       "DSCP"
#define TWAMP_SESSION_TTL                        "TTL"
#define TWAMP_SESSION_PACKET_TIMESTAMP_FORMAT    "TIMESTAMP_FORMAT"
#define TWAMP_SESSION_PACKET_PADDING_SIZE        "PADDING_SIZE"

/* Session-Sender */
#define TWAMP_SESSION_TX_PACKET_COUNT            "PACKET_COUNT"
#define TWAMP_SESSION_TX_MONITOR_TIME            "MONITOR_TIME"
#define TWAMP_SESSION_TX_INTERVAL                "TX_INTERVAL"
#define TWAMP_SESSION_TIMEOUT                    "TIMEOUT"
#define TWAMP_SESSION_STATISTICS_INTERVAL        "STATISTICS_INTERVAL"
#define TWAMP_SESSION_ADMIN_STATE                "ADMIN_STATE"

/* TWAMP session status */
#define TWAMP_SESSION_STATUS                "status"
#define TWAMP_SESSION_STATUS_ACTIVE         "active"
#define TWAMP_SESSION_STATUS_INACTIVE       "inactive"

#define TWAMP_SESSION_TX_MODE_PACKET_NUM    "packet_num"
#define TWAMP_SESSION_TX_MODE_CONTINUOUS    "continuous"

#define TWAMP_SESSION_DSCP_MIN    0
#define TWAMP_SESSION_DSCP_MAX    63

#define TWAMP_SESSION_TIMEOUT_MIN    1
#define TWAMP_SESSION_TIMEOUT_MAX    10

static map<string, sai_twamp_session_role_t> twamp_role_map =
{
    { "SENDER",       SAI_TWAMP_SESSION_ROLE_SENDER    },
    { "REFLECTOR",    SAI_TWAMP_SESSION_ROLE_REFLECTOR }
};

static map<string, sai_twamp_mode_t> twamp_mode_map =
{
    { "FULL",     SAI_TWAMP_MODE_FULL    },
    { "LIGHT",    SAI_TWAMP_MODE_LIGHT   }
};

static map<string, sai_twamp_timestamp_format_t> timestamp_format_map =
{
    { "NTP",    SAI_TWAMP_TIMESTAMP_FORMAT_NTP },
    { "PTP",    SAI_TWAMP_TIMESTAMP_FORMAT_PTP }
};

static map<string, bool> session_admin_state_map =
{
    { "ENABLED",  true  },
    { "DISABLED", false }
};

static map<string, bool> hw_lookup_map =
{
    { "TRUE",  true  },
    { "FALSE", false }
};

/* Global variables */
extern sai_object_id_t     gSwitchId;
extern sai_object_id_t     gVirtualRouterId;
extern sai_switch_api_t   *sai_switch_api;
extern sai_twamp_api_t    *sai_twamp_api;
extern CrmOrch            *gCrmOrch;

const vector<sai_twamp_session_stat_t> twamp_session_stat_ids =
{
    SAI_TWAMP_SESSION_STAT_RX_PACKETS,
    SAI_TWAMP_SESSION_STAT_RX_BYTE,
    SAI_TWAMP_SESSION_STAT_TX_PACKETS,
    SAI_TWAMP_SESSION_STAT_TX_BYTE,
    SAI_TWAMP_SESSION_STAT_DROP_PACKETS,
    SAI_TWAMP_SESSION_STAT_MAX_LATENCY,
    SAI_TWAMP_SESSION_STAT_MIN_LATENCY,
    SAI_TWAMP_SESSION_STAT_AVG_LATENCY,
    SAI_TWAMP_SESSION_STAT_MAX_JITTER,
    SAI_TWAMP_SESSION_STAT_MIN_JITTER,
    SAI_TWAMP_SESSION_STAT_AVG_JITTER
};



TwampOrch::TwampOrch(TableConnector confDbConnector, TableConnector stateDbConnector, SwitchOrch *switchOrch, PortsOrch *portOrch, VRFOrch *vrfOrch) :
        Orch(confDbConnector.first, confDbConnector.second),
        m_stateDbTwampTable(stateDbConnector.first, stateDbConnector.second),
        m_switchOrch(switchOrch),
        m_portsOrch(portOrch),
        m_vrfOrch(vrfOrch)
{
    /* Set entries count to 0 */
    m_maxTwampSessionCount = m_twampSessionCount = 0;

    /* Get the Maximum supported TWAMP sessions */
    SWSS_LOG_INFO("Get the Maximum supported TWAMP sessions");
    sai_attribute_t attr;
    attr.id = SAI_SWITCH_ATTR_MAX_TWAMP_SESSION;
    sai_status_t status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_NOTICE("Twamp session resource availability is not supported. Skipping ...");
        return;
    }
    else
    {
        m_maxTwampSessionCount = attr.value.u32;
    }

    /* Set MAX entries to state DB */
    if (m_maxTwampSessionCount)
    {
        vector<FieldValueTuple> fvTuple;
        fvTuple.emplace_back("MAX_TWAMP_SESSION_COUNT", to_string(m_maxTwampSessionCount));
        m_switchOrch->set_switch_capability(fvTuple);
    }
    else
    {
        SWSS_LOG_NOTICE("Twamp session resource availability is not supported. Skipping ...");
        return;
    }

    /* Add TWAMP session event notification support */
    DBConnector *notificationsDb = new DBConnector("ASIC_DB", 0);
    m_twampNotificationConsumer = new swss::NotificationConsumer(notificationsDb, "NOTIFICATIONS");
    auto twampNotifier = new Notifier(m_twampNotificationConsumer, this, "TWAMP_NOTIFICATIONS");
    Orch::addExecutor(twampNotifier);
    register_event_notif = false;

    /* Initialize DB connectors */
    m_asicDb = shared_ptr<DBConnector>(new DBConnector("ASIC_DB", 0));
    m_countersDb = shared_ptr<DBConnector>(new DBConnector("COUNTERS_DB", 0));

    /* Initialize VIDTORID table */
    m_vidToRidTable = unique_ptr<Table>(new Table(m_asicDb.get(), "VIDTORID"));

    /* Initialize counter tables */
    m_counterTwampSessionNameMapTable = unique_ptr<Table>(new Table(m_countersDb.get(), COUNTERS_TWAMP_SESSION_NAME_MAP));
    m_countersTable = unique_ptr<Table>(new Table(m_countersDb.get(), COUNTERS_TABLE));
}

bool TwampOrch::isSessionExists(const string& name)
{
    SWSS_LOG_ENTER();

    return m_twampEntries.find(name) != m_twampEntries.end();
}

bool TwampOrch::getSessionName(const sai_object_id_t oid, string& name)
{
    SWSS_LOG_ENTER();

    for (const auto& it: m_twampEntries)
    {
        if (it.second.session_id == oid)
        {
            name = it.first;
            return true;
        }
    }

    return false;
}

bool TwampOrch::validateUdpPort(uint16_t udp_port)
{
    if (udp_port == 862)
    {
        return true;
    }
    if (udp_port == 863)
    {
        return true;
    }
    if (udp_port >= 1025)
    {
        return true;
    }
    return false;
}

void TwampOrch::increaseTwampSessionCount(void)
{
    m_twampSessionCount++;
}

void TwampOrch::decreaseTwampSessionCount(void)
{
    m_twampSessionCount--;
}

bool TwampOrch::checkTwampSessionCount(void)
{
    return m_twampSessionCount < m_maxTwampSessionCount;
}

void TwampOrch::setSessionStatus(const string& name, const string& status)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> fvVector;
    fvVector.emplace_back(TWAMP_SESSION_STATUS, status);
    m_stateDbTwampTable.set(name, fvVector);
}

bool TwampOrch::getSessionStatus(const string &name, string& status)
{
    SWSS_LOG_ENTER();

    if (m_stateDbTwampTable.hget(name, TWAMP_SESSION_STATUS, status))
    {
        return true;
    }
    return false;
}

void TwampOrch::removeSessionStatus(const string& name)
{
    SWSS_LOG_ENTER();

    m_stateDbTwampTable.del(name);
}

void TwampOrch::removeSessionCounter(const sai_object_id_t session_id)
{
    SWSS_LOG_ENTER();

    string key_pattern = "COUNTERS:" + sai_serialize_object_id(session_id) + "*";
    auto keys = m_countersDb->keys(key_pattern);
    for (auto& k : keys)
    {
        m_countersDb->del(k);
    }
}

void TwampOrch::initSessionStats(const string& name)
{
    SWSS_LOG_ENTER();

    auto it = m_twampStatistics.find(name);
    if (it == m_twampStatistics.end())
    {
        SWSS_LOG_ERROR("Failed to init non-existent twamp session %s stat info", name.c_str());
        return;
    }

    TwampStats& total_stats = it->second;

    total_stats.rx_packets        = 0;
    total_stats.rx_bytes          = 0;
    total_stats.tx_packets        = 0;
    total_stats.tx_bytes          = 0;
    total_stats.drop_packets      = 0;
    total_stats.max_latency       = 0;
    total_stats.min_latency       = 0;
    total_stats.avg_latency       = 0;
    total_stats.max_jitter        = 0;
    total_stats.min_jitter        = 0;
    total_stats.avg_jitter        = 0;
    total_stats.avg_latency_total = 0;
    total_stats.avg_jitter_total  = 0;
}

bool TwampOrch::registerTwampEventNotification(void)
{
    sai_attribute_t attr;
    sai_status_t status;
    sai_attr_capability_t capability;

    status = sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_SWITCH,
                                            SAI_SWITCH_ATTR_TWAMP_SESSION_EVENT_NOTIFY,
                                            &capability);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_NOTICE("Unable to query the TWAMP event notification capability");
        return false;
    }

    if (!capability.set_implemented)
    {
        SWSS_LOG_NOTICE("TWAMP register event notification not supported");
        return false;
    }

    attr.id = SAI_SWITCH_ATTR_TWAMP_SESSION_EVENT_NOTIFY;
    attr.value.ptr = (void *)on_twamp_session_event;

    status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to register TWAMP notification handler");
        return false;
    }

    return true;
}

bool TwampOrch::activateSession(const string& name, TwampEntry& entry)
{
    SWSS_LOG_ENTER();

    sai_status_t status;
    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_TWAMP_SESSION_ATTR_TWAMP_MODE;
    attr.value.s32 = entry.mode;
    attrs.emplace_back(attr);

    attr.id = SAI_TWAMP_SESSION_ATTR_SESSION_ROLE;
    attr.value.s32 = entry.role;
    attrs.emplace_back(attr);

    attr.id = SAI_TWAMP_SESSION_ATTR_HW_LOOKUP_VALID;
    attr.value.booldata = entry.hw_lookup;
    attrs.emplace_back(attr);

    if (entry.vrf_id)
    {
        attr.id = SAI_TWAMP_SESSION_ATTR_VIRTUAL_ROUTER;
        attr.value.oid = entry.vrf_id;
        attrs.emplace_back(attr);
    }

    attr.id = SAI_TWAMP_SESSION_ATTR_SRC_IP;
    copy(attr.value.ipaddr, entry.src_ip);
    attrs.emplace_back(attr);

    attr.id = SAI_TWAMP_SESSION_ATTR_DST_IP;
    copy(attr.value.ipaddr, entry.dst_ip);
    attrs.emplace_back(attr);

    attr.id = SAI_TWAMP_SESSION_ATTR_UDP_SRC_PORT;
    attr.value.u32 = entry.src_udp_port;
    attrs.emplace_back(attr);

    attr.id = SAI_TWAMP_SESSION_ATTR_UDP_DST_PORT;
    attr.value.u32 = entry.dst_udp_port;
    attrs.emplace_back(attr);

    if (entry.role == SAI_TWAMP_SESSION_ROLE_SENDER)
    {
        if (entry.tx_mode == TWAMP_SESSION_TX_MODE_PACKET_NUM)
        {
            attr.id = SAI_TWAMP_SESSION_ATTR_TWAMP_PKT_TX_MODE;
            attr.value.s32 = SAI_TWAMP_PKT_TX_MODE_PACKET_COUNT;
            attrs.emplace_back(attr);

            attr.id = SAI_TWAMP_SESSION_ATTR_TX_PKT_CNT;
            attr.value.u32 = entry.packet_count;
            attrs.emplace_back(attr);
        }
        else if (entry.tx_mode == TWAMP_SESSION_TX_MODE_CONTINUOUS)
        {
            if (entry.monitor_time)
            {
                attr.id = SAI_TWAMP_SESSION_ATTR_TWAMP_PKT_TX_MODE;
                attr.value.u32 = SAI_TWAMP_PKT_TX_MODE_PERIOD;
                attrs.emplace_back(attr);

                attr.id = SAI_TWAMP_SESSION_ATTR_TX_PKT_PERIOD;
                attr.value.u32 = entry.monitor_time;
                attrs.emplace_back(attr);
            }
            else
            {
                attr.id = SAI_TWAMP_SESSION_ATTR_TWAMP_PKT_TX_MODE;
                attr.value.u32 = SAI_TWAMP_PKT_TX_MODE_CONTINUOUS;
                attrs.emplace_back(attr);
            }
        }

        attr.id = SAI_TWAMP_SESSION_ATTR_TX_INTERVAL;
        attr.value.u32 = entry.tx_interval;
        attrs.emplace_back(attr);

        attr.id = SAI_TWAMP_SESSION_ATTR_TIMEOUT;
        attr.value.u32 = entry.timeout;
        attrs.emplace_back(attr);

        attr.id = SAI_TWAMP_SESSION_ATTR_STATISTICS_INTERVAL;
        attr.value.u32 = entry.statistics_interval;
        attrs.emplace_back(attr);

        attr.id = SAI_TWAMP_SESSION_ATTR_SESSION_ENABLE_TRANSMIT;
        attr.value.booldata = entry.admin_state;
        attrs.emplace_back(attr);
    }

    setSessionStatus(name, TWAMP_SESSION_STATUS_INACTIVE);

    status = sai_twamp_api->create_twamp_session(&entry.session_id, gSwitchId, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create twamp session %s, status %d", name.c_str(), status);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TWAMP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    /* increase VRF reference count */
    m_vrfOrch->increaseVrfRefCount(entry.vrf_id);
    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_TWAMP_ENTRY);

    increaseTwampSessionCount();

    if (entry.role == SAI_TWAMP_SESSION_ROLE_REFLECTOR)
    {
        setSessionStatus(name, TWAMP_SESSION_STATUS_ACTIVE);
    }

    return true;
}

bool TwampOrch::deactivateSession(const string& name, TwampEntry& entry)
{
    SWSS_LOG_ENTER();
    sai_status_t status;

    status = sai_twamp_api->remove_twamp_session(entry.session_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove twamp session %s, status %d", name.c_str(), status);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TWAMP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    /* decrease VRF reference count */
    m_vrfOrch->decreaseVrfRefCount(entry.vrf_id);
    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_TWAMP_ENTRY);

    decreaseTwampSessionCount();

    setSessionStatus(name, TWAMP_SESSION_STATUS_INACTIVE);

    return true;
}

bool TwampOrch::setSessionTransmitEn(TwampEntry& entry, string admin_state)
{
    SWSS_LOG_ENTER();

    if (entry.role != SAI_TWAMP_SESSION_ROLE_SENDER)
    {
        return false;
    }

    auto found = session_admin_state_map.find(admin_state);
    if (found == session_admin_state_map.end())
    {
        SWSS_LOG_ERROR("Incorrect transmit value: %s", admin_state.c_str());
        return false;
    }

    sai_attribute_t attr;
    attr.id = SAI_TWAMP_SESSION_ATTR_SESSION_ENABLE_TRANSMIT;
    attr.value.booldata = found->second;
    sai_status_t status = sai_twamp_api->set_twamp_session_attribute(entry.session_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set twamp session %" PRIx64 " %s transmit, status %d",
                        entry.session_id, admin_state.c_str(), status);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_TWAMP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    return true;
}

task_process_status TwampOrch::createEntry(const string& key, const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();

    if (!register_event_notif)
    {
        if (!registerTwampEventNotification())
        {
            SWSS_LOG_ERROR("TWAMP session for %s cannot be created", key.c_str());
            return task_process_status::task_failed;
        }
        register_event_notif = true;
    }

    if (!checkTwampSessionCount())
    {
        SWSS_LOG_NOTICE("Failed to create twamp session %s: resources are not available", key.c_str());
        return task_process_status::task_failed;
    }

    TwampEntry entry;
    for (auto i : data)
    {
        try {
            string attr_name  = to_upper(fvField(i));
            string attr_value = fvValue(i);

            if (attr_name == TWAMP_SESSION_MODE)
            {
                string value = to_upper(attr_value);
                if (twamp_mode_map.find(value) == twamp_mode_map.end())
                {
                    SWSS_LOG_ERROR("Failed to parse valid mode %s", attr_value.c_str());
                    return task_process_status::task_invalid_entry;
                }
                entry.mode = twamp_mode_map[value];
            }
            else if (attr_name == TWAMP_SESSION_ROLE)
            {
                string value = to_upper(attr_value);
                if (twamp_role_map.find(value) == twamp_role_map.end())
                {
                    SWSS_LOG_ERROR("Failed to parse valid role %s", attr_value.c_str());
                    return task_process_status::task_invalid_entry;
                }
                entry.role = twamp_role_map[value];
            }
            else if (attr_name == TWAMP_SESSION_SRC_IP)
            {
                entry.src_ip = attr_value;
            }
            else if (attr_name == TWAMP_SESSION_DST_IP)
            {
                entry.dst_ip = attr_value;
            }
            else if (attr_name == TWAMP_SESSION_SRC_UDP_PORT)
            {
                uint16_t value = to_uint<uint16_t>(attr_value);
                if (!validateUdpPort(value))
                {
                    SWSS_LOG_ERROR("Failed to parse valid souce udp port %d", value);
                    return task_process_status::task_invalid_entry;
                }
                entry.src_udp_port = value;
            }
            else if (attr_name == TWAMP_SESSION_DST_UDP_PORT)
            {
                uint16_t value = to_uint<uint16_t>(attr_value);
                if (!validateUdpPort(value))
                {
                    SWSS_LOG_ERROR("Failed to parse valid destination udp port %d", value);
                    return task_process_status::task_invalid_entry;
                }
                entry.dst_udp_port = to_uint<uint16_t>(attr_value);
            }
            else if (attr_name == TWAMP_SESSION_VRF_NAME)
            {
                if (attr_value == "default")
                {
                    entry.vrf_id = gVirtualRouterId;
                }
                else
                {
                    if (!m_vrfOrch->isVRFexists(attr_value))
                    {
                        SWSS_LOG_WARN("Vrf '%s' hasn't been created yet", attr_value.c_str());
                        return task_process_status::task_invalid_entry;
                    }
                    entry.vrf_id = m_vrfOrch->getVRFid(attr_value);
                }
            }
            else if (attr_name == TWAMP_SESSION_DSCP)
            {
                entry.dscp = to_uint<uint8_t>(attr_value, TWAMP_SESSION_DSCP_MIN, TWAMP_SESSION_DSCP_MAX);
            }
            else if (attr_name == TWAMP_SESSION_TTL)
            {
                entry.ttl = to_uint<uint8_t>(attr_value);
            }
            else if (attr_name == TWAMP_SESSION_PACKET_TIMESTAMP_FORMAT)
            {
                string value = to_upper(attr_value);
                if (timestamp_format_map.find(value) == timestamp_format_map.end())
                {
                    SWSS_LOG_ERROR("Failed to parse timestamp format value: %s", attr_value.c_str());
                    return task_process_status::task_invalid_entry;
                }
                entry.timestamp_format = timestamp_format_map[value];
            }
            else if (attr_name == TWAMP_SESSION_PACKET_PADDING_SIZE)
            {
                entry.padding_size = to_uint<uint16_t>(attr_value);
            }
            else if (attr_name == TWAMP_SESSION_TX_PACKET_COUNT)
            {
                if (entry.tx_mode == TWAMP_SESSION_TX_MODE_CONTINUOUS)
                {
                    SWSS_LOG_ERROR("Configured packet count %s is conflict with monitor time", attr_value.c_str());
                    return task_process_status::task_invalid_entry;
                }

                entry.packet_count = to_uint<uint32_t>(attr_value);
                entry.tx_mode = TWAMP_SESSION_TX_MODE_PACKET_NUM;
            }
            else if (attr_name == TWAMP_SESSION_TX_MONITOR_TIME)
            {
                if (entry.tx_mode == TWAMP_SESSION_TX_MODE_PACKET_NUM)
                {
                    SWSS_LOG_ERROR("Configured monitor time %s is conflict with packet count", attr_value.c_str());
                    return task_process_status::task_invalid_entry;
                }

                entry.monitor_time = to_uint<uint32_t>(attr_value);
                entry.tx_mode = TWAMP_SESSION_TX_MODE_CONTINUOUS;
            }
            else if (attr_name == TWAMP_SESSION_TX_INTERVAL)
            {
                entry.tx_interval = to_uint<uint32_t>(attr_value);
            }
            else if (attr_name == TWAMP_SESSION_STATISTICS_INTERVAL)
            {
                entry.statistics_interval = to_uint<uint32_t>(attr_value);
            }
            else if (attr_name == TWAMP_SESSION_TIMEOUT)
            {
                entry.timeout = to_uint<uint8_t>(attr_value, TWAMP_SESSION_TIMEOUT_MIN, TWAMP_SESSION_TIMEOUT_MAX);
            }
            else if (attr_name == TWAMP_SESSION_ADMIN_STATE)
            {
                string value = to_upper(attr_value);
                if (session_admin_state_map.find(value) == session_admin_state_map.end())
                {
                    SWSS_LOG_ERROR("Failed to parse transmit mode value: %s", attr_value.c_str());
                    return task_process_status::task_invalid_entry;
                }
                entry.admin_state = session_admin_state_map[value];
            }
            else if (attr_name == TWAMP_SESSION_HW_LOOKUP)
            {
                string value = to_upper(attr_value);
                if (hw_lookup_map.find(value) == hw_lookup_map.end())
                {
                    SWSS_LOG_ERROR("Failed to parse hw lookup value: %s", attr_value.c_str());
                    return task_process_status::task_invalid_entry;
                }
                entry.hw_lookup = hw_lookup_map[value];
            }
            else
            {
                SWSS_LOG_ERROR("Failed to parse session %s configuration. Unknown attribute %s", key.c_str(), attr_name.c_str());
                return task_process_status::task_invalid_entry;
            }
        }
        catch (const exception& e)
        {
            SWSS_LOG_ERROR("Failed to parse session %s attribute %s error: %s.", key.c_str(), fvField(i).c_str(), e.what());
            return task_process_status::task_invalid_entry;
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Failed to parse session %s attribute %s. Unknown error has been occurred", key.c_str(), fvField(i).c_str());
            return task_process_status::task_failed;
        }
    }

    m_twampEntries.emplace(key, entry);

    if (entry.role == SAI_TWAMP_SESSION_ROLE_SENDER)
    {
        TwampStats hw_stats;
        m_twampStatistics.emplace(key, hw_stats);
        initSessionStats(key);
    }

    auto &session = m_twampEntries.find(key)->second;
    if (!activateSession(key, session))
    {
        SWSS_LOG_ERROR("Failed to create twamp session %s", key.c_str());
        return task_process_status::task_failed;
    }

    return task_process_status::task_success;
}

task_process_status TwampOrch::updateEntry(const string& key, const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();

    auto it = m_twampEntries.find(key);
    if (it == m_twampEntries.end())
    {
        SWSS_LOG_NOTICE("Failed to set twamp session, session %s not exists", key.c_str());
        return task_process_status::task_invalid_entry;
    }
    TwampEntry& entry = it->second;

    for (auto i : data)
    {
        try {
            const auto &attr_field = to_upper(fvField(i));
            const auto &attr_value = fvValue(i);

            if (attr_field == TWAMP_SESSION_ADMIN_STATE)
            {
                string value = to_upper(attr_value);
                if (setSessionTransmitEn(entry, value))
                {
                    entry.admin_state = session_admin_state_map[value];
                    if (entry.admin_state)
                    {
                        string running_status;
                        getSessionStatus(key, running_status);
                        if (running_status == TWAMP_SESSION_STATUS_INACTIVE)
                        {
                            removeSessionCounter(entry.session_id);
                            initSessionStats(key);
                        }
                        setSessionStatus(key, TWAMP_SESSION_STATUS_ACTIVE);
                        SWSS_LOG_NOTICE("Activated twamp session %s", key.c_str());
                    }
                    else
                    {
                        setSessionStatus(key, TWAMP_SESSION_STATUS_INACTIVE);
                        SWSS_LOG_NOTICE("Deactivated twamp session %s", key.c_str());
                    }
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to set twamp session %s transmit %s", key.c_str(), attr_value.c_str());
                }
            }
            else
            {
                SWSS_LOG_DEBUG("Ignore to parse session %s configuration attribute %s", key.c_str(), fvField(i).c_str());
            }
        }
        catch (const exception& e)
        {
            SWSS_LOG_ERROR("Failed to parse session %s attribute %s error: %s.", key.c_str(), fvField(i).c_str(), e.what());
            return task_process_status::task_invalid_entry;
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Failed to parse session %s attribute %s. Unknown error has been occurred", key.c_str(), fvField(i).c_str());
            return task_process_status::task_failed;
        }
    }

    return task_process_status::task_success;
}

task_process_status TwampOrch::deleteEntry(const string& key)
{
    SWSS_LOG_ENTER();

    auto it = m_twampEntries.find(key);
    if (it == m_twampEntries.end())
    {
        SWSS_LOG_ERROR("Failed to remove non-existent twamp session %s", key.c_str());
        return task_process_status::task_invalid_entry;
    }

    TwampEntry& entry = it->second;

    if (!deactivateSession(key, entry))
    {
        SWSS_LOG_ERROR("Failed to remove twamp session %s", key.c_str());
        return task_process_status::task_failed;
    }

    /* remove TWAMP session in STATE_DB */
    removeSessionStatus(key);

    /* remove TWAMP session maps in COUNTERS_DB */
    m_counterTwampSessionNameMapTable->hdel("", key);

    /* remove TWAMP session in COUNTER_DB */
    removeSessionCounter(entry.session_id);

    /* remove soft table in orchagent */
    m_twampEntries.erase(key);
    m_twampStatistics.erase(key);

    SWSS_LOG_NOTICE("Removed twamp session %s", key.c_str());

    return task_process_status::task_success;
}

void TwampOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    if (!m_portsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        auto data = kfvFieldsValues(t);
        task_process_status task_status = task_process_status::task_failed;

        if (op == SET_COMMAND)
        {
            if (!isSessionExists(key))
            {
                task_status = createEntry(key, data);
            }
            else
            {
                task_status = updateEntry(key, data);
            }
        }
        else if (op == DEL_COMMAND)
        {
            task_status = deleteEntry(key);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }

        /* Specifically retry the task when asked */
        if (task_status == task_process_status::task_need_retry)
        {
            it++;
        }
        else
        {
            it = consumer.m_toSync.erase(it);
        }
    }
}

bool TwampOrch::addCounterNameMap(const string& name, const sai_object_id_t session_id)
{
    SWSS_LOG_ENTER();

    string value;
    const auto id = sai_serialize_object_id(session_id);

    if (m_vidToRidTable->hget("", id, value))
    {
        vector<FieldValueTuple> fields;
        fields.emplace_back(name, id);
        m_counterTwampSessionNameMapTable->set("", fields);

        return true;
    }
    else
    {
        SWSS_LOG_NOTICE("TWAMP session counter %s already exists.", name.c_str());
        return true;
    }

    return false;
}

void TwampOrch::saveSessionStatsLatest(const sai_object_id_t session_id, const uint32_t index, const vector<uint64_t>& stats)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> values;

    for (const auto& it: twamp_session_stat_ids)
    {
        values.emplace_back(sai_serialize_twamp_session_stat(it), to_string(stats[it]));
    }

    m_countersTable->set(sai_serialize_object_id(session_id) + ":INDEX:" + to_string(index), values);

    return;
}

void TwampOrch::calculateCounters(const string& name, const uint32_t index, const vector<uint64_t>& stats)
{
    SWSS_LOG_ENTER();

    auto it = m_twampStatistics.find(name);
    if (it == m_twampStatistics.end())
    {
        SWSS_LOG_ERROR("Failed to caculate non-existent twamp session %s", name.c_str());
        return;
    }

    TwampStats& total_stats = it->second;
    /* packets */
    total_stats.rx_packets   += stats[SAI_TWAMP_SESSION_STAT_RX_PACKETS];
    total_stats.rx_bytes     += stats[SAI_TWAMP_SESSION_STAT_RX_BYTE];
    total_stats.tx_packets   += stats[SAI_TWAMP_SESSION_STAT_TX_PACKETS];
    total_stats.tx_bytes     += stats[SAI_TWAMP_SESSION_STAT_TX_BYTE];
    total_stats.drop_packets += stats[SAI_TWAMP_SESSION_STAT_DROP_PACKETS];

    /* latency */
    total_stats.max_latency = (stats[SAI_TWAMP_SESSION_STAT_MAX_LATENCY] > total_stats.max_latency) ?
                               stats[SAI_TWAMP_SESSION_STAT_MAX_LATENCY] : total_stats.max_latency;
    total_stats.min_latency = (index == 1) ? stats[SAI_TWAMP_SESSION_STAT_MIN_LATENCY] :
                               ((stats[SAI_TWAMP_SESSION_STAT_MIN_LATENCY] < total_stats.min_latency) ?
                                 stats[SAI_TWAMP_SESSION_STAT_MIN_LATENCY] : total_stats.min_latency);
    total_stats.avg_latency_total += stats[SAI_TWAMP_SESSION_STAT_AVG_LATENCY];
    total_stats.avg_latency = total_stats.avg_latency_total / index;

    /* jitter */
    total_stats.max_jitter = (stats[SAI_TWAMP_SESSION_STAT_MAX_JITTER] > total_stats.max_jitter) ?
                              stats[SAI_TWAMP_SESSION_STAT_MAX_JITTER] : total_stats.max_jitter;
    total_stats.min_jitter = (index == 1) ? stats[SAI_TWAMP_SESSION_STAT_MIN_JITTER] :
                              ((stats[SAI_TWAMP_SESSION_STAT_MIN_JITTER] < total_stats.min_jitter) ?
                                stats[SAI_TWAMP_SESSION_STAT_MIN_JITTER] : total_stats.min_jitter);
    total_stats.avg_jitter_total += stats[SAI_TWAMP_SESSION_STAT_AVG_JITTER];
    total_stats.avg_jitter = total_stats.avg_jitter_total / index;
}

void TwampOrch::saveCountersTotal(const string& name, const sai_object_id_t session_id)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> values;

    auto it = m_twampStatistics.find(name);
    if (it == m_twampStatistics.end())
    {
        SWSS_LOG_ERROR("Failed to caculate non-existent twamp session %s",
                name.c_str());
        return;
    }

    TwampStats& total_stats = it->second;

    values.emplace_back(sai_serialize_twamp_session_stat(SAI_TWAMP_SESSION_STAT_RX_PACKETS), to_string(total_stats.rx_packets));
    values.emplace_back(sai_serialize_twamp_session_stat(SAI_TWAMP_SESSION_STAT_RX_BYTE), to_string(total_stats.rx_bytes));
    values.emplace_back(sai_serialize_twamp_session_stat(SAI_TWAMP_SESSION_STAT_TX_PACKETS), to_string(total_stats.tx_packets));
    values.emplace_back(sai_serialize_twamp_session_stat(SAI_TWAMP_SESSION_STAT_TX_BYTE), to_string(total_stats.tx_bytes));
    values.emplace_back(sai_serialize_twamp_session_stat(SAI_TWAMP_SESSION_STAT_DROP_PACKETS), to_string(total_stats.drop_packets));
    values.emplace_back(sai_serialize_twamp_session_stat(SAI_TWAMP_SESSION_STAT_MAX_LATENCY), to_string(total_stats.max_latency));
    values.emplace_back(sai_serialize_twamp_session_stat(SAI_TWAMP_SESSION_STAT_MIN_LATENCY), to_string(total_stats.min_latency));
    values.emplace_back(sai_serialize_twamp_session_stat(SAI_TWAMP_SESSION_STAT_AVG_LATENCY), to_string(total_stats.avg_latency));
    values.emplace_back(sai_serialize_twamp_session_stat(SAI_TWAMP_SESSION_STAT_MAX_JITTER), to_string(total_stats.max_jitter));
    values.emplace_back(sai_serialize_twamp_session_stat(SAI_TWAMP_SESSION_STAT_MIN_JITTER), to_string(total_stats.min_jitter));
    values.emplace_back(sai_serialize_twamp_session_stat(SAI_TWAMP_SESSION_STAT_AVG_JITTER), to_string(total_stats.avg_jitter));

    m_countersTable->set(sai_serialize_object_id(session_id), values);
}

void TwampOrch::doTask(NotificationConsumer& consumer)
{
    SWSS_LOG_ENTER();

    if (!m_portsOrch->allPortsReady())
    {
        return;
    }

    std::string op;
    std::string data;
    std::vector<swss::FieldValueTuple> values;

    consumer.pop(op, data, values);

    if (&consumer != m_twampNotificationConsumer)
    {
        return;
    }

    if (op == "twamp_session_event")
    {
        uint32_t count = 0;
        sai_twamp_session_event_notification_data_t *twamp_session = nullptr;

        sai_deserialize_twamp_session_event_ntf(data, count, &twamp_session);

        for (uint32_t i = 0; i < count; i++)
        {
            string name;
            sai_object_id_t session_id = twamp_session[i].twamp_session_id;
            sai_twamp_session_state_t session_state = twamp_session[i].session_state;
            uint32_t stats_index = twamp_session[i].session_stats.index;

            if (!getSessionName(session_id, name))
            {
                continue;
            }

            /* update state db */
            if (session_state == SAI_TWAMP_SESSION_STATE_ACTIVE)
            {
                setSessionStatus(name, TWAMP_SESSION_STATUS_ACTIVE);
            }
            else
            {
                setSessionStatus(name, TWAMP_SESSION_STATUS_INACTIVE);
            }

            /* save counter db */
            if (twamp_session[i].session_stats.number_of_counters)
            {
                if (0 == stats_index)
                {
                    continue;
                }
                else if (1 == stats_index)
                {
                    addCounterNameMap(name, session_id);
                }

                vector<uint64_t> hw_stats;
                hw_stats.resize(twamp_session_stat_ids.size());
                for (uint32_t j = 0; j < twamp_session[i].session_stats.number_of_counters; j++)
                {
                    uint32_t counters_id = twamp_session[i].session_stats.counters_ids[j];
                    auto it = find(twamp_session_stat_ids.begin(), twamp_session_stat_ids.end(), counters_id);
                    if (it != twamp_session_stat_ids.end())
                    {
                        hw_stats[counters_id] = twamp_session[i].session_stats.counters[j];
                    }
                }

                saveSessionStatsLatest(session_id, stats_index, hw_stats);
                calculateCounters(name, stats_index, hw_stats);
                saveCountersTotal(name, session_id);
            }
        }

        sai_deserialize_free_twamp_session_event_ntf(count, twamp_session);
    }
}
