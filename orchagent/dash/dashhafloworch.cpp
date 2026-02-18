#include "dashhafloworch.h"

#include "orch.h"
#include "sai.h"
#include "saiextensions.h"
#include "saihelper.h"
#include "table.h"
#include "taskworker.h"
#include "converter.h"
#include "ipaddress.h"
#include "macaddress.h"
#include "swssnet.h"
#include "schema.h"
#include "schema.h"

#include <chrono>
#include <sstream>
#include <iomanip>
#include <map>
#include <algorithm>
#include <vector>

using namespace std;
using namespace swss;

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t* sai_switch_api;
extern sai_dash_flow_api_t* sai_dash_flow_api;

constexpr const char* DashHaFlowOrch::SESSION_TYPE_BULK_SYNC;
constexpr const char* DashHaFlowOrch::SESSION_TYPE_FLOW_DUMP;

static const map<string, sai_dash_flow_entry_bulk_get_session_filter_key_t> filter_key_map = {
    { "eni_addr", SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_KEY_ENI_ADDR },
    { "ip_protocol", SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_KEY_IP_PROTOCOL },
    { "src_ip_addr", SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_KEY_SRC_IP_ADDR },
    { "dst_ip_addr", SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_KEY_DST_IP_ADDR },
    { "src_l4_port", SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_KEY_SRC_L4_PORT },
    { "dst_l4_port", SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_KEY_DST_L4_PORT },
    { "key_version", SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_KEY_KEY_VERSION }
};

static const map<string, sai_dash_flow_entry_bulk_get_session_op_key_t> filter_op_map = {
    { "equal_to", SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_OP_KEY_FILTER_OP_EQUAL_TO },
    { "greater_than", SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_OP_KEY_FILTER_OP_GREATER_THAN },
    { "greater_than_or_equal_to", SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_OP_KEY_FILTER_OP_GREATER_THAN_OR_EQUAL_TO },
    { "less_than", SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_OP_KEY_FILTER_OP_LESS_THAN },
    { "less_than_or_equal_to", SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_OP_KEY_FILTER_OP_LESS_THAN_OR_EQUAL_TO }
};

task_process_status FlowDumpFilterManager::addFilter(const string &key, const vector<FieldValueTuple> &attrs)
{
    SWSS_LOG_ENTER();

    FlowDumpFilterEntry entry;
    entry.filter_id = SAI_NULL_OBJECT_ID;

    for (auto i = attrs.begin(); i != attrs.end(); i++)
    {
        const auto &attr = fvField(*i);
        const auto &value = fvValue(*i);

        if (attr == "key")
        {
            entry.key = value;
        }
        else if (attr == "op")
        {
            entry.op = value;
        }
        else if (attr == "value")
        {
            entry.value = value;
        }
    }

    if (entry.key.empty() || entry.op.empty() || entry.value.empty())
    {
        SWSS_LOG_ERROR("Missing required fields for flow dump filter %s", key.c_str());
        return task_failed;
    }

    sai_object_id_t filter_id = createFilterSAI(entry);
    if (filter_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Failed to create flow dump filter %s", key.c_str());
        return task_failed;
    }

    entry.filter_id = filter_id;
    m_filter_cache[key] = entry;

    SWSS_LOG_NOTICE("Created flow dump filter %s with filter_id 0x%lx", key.c_str(), filter_id);

    return task_success;
}

task_process_status FlowDumpFilterManager::removeFilter(const string &key)
{
    SWSS_LOG_ENTER();

    auto it = m_filter_cache.find(key);
    if (it == m_filter_cache.end())
    {
        SWSS_LOG_WARN("Flow dump filter %s not found in cache", key.c_str());
        return task_success;
    }

    if (it->second.filter_id != SAI_NULL_OBJECT_ID)
    {
        deleteFilterSAI(it->second.filter_id);
    }

    m_filter_cache.erase(it);

    SWSS_LOG_NOTICE("Removed flow dump filter %s from cache", key.c_str());

    return task_success;
}

vector<sai_object_id_t> FlowDumpFilterManager::getFilterIds(const vector<string> &required_filter_keys) const
{
    SWSS_LOG_ENTER();

    vector<sai_object_id_t> filter_ids;

    for (const auto &filter_key : required_filter_keys)
    {
        auto filter_it = m_filter_cache.find(filter_key);
        if (filter_it != m_filter_cache.end() && filter_it->second.filter_id != SAI_NULL_OBJECT_ID)
        {
            filter_ids.push_back(filter_it->second.filter_id);
        }
    }

    return filter_ids;
}

sai_object_id_t FlowDumpFilterManager::createFilterSAI(const FlowDumpFilterEntry &filter)
{
    SWSS_LOG_ENTER();

    try
    {
        sai_attribute_t attrs[3];
        uint32_t attr_count = 0;

        auto filter_key_it = filter_key_map.find(filter.key);
        if (filter_key_it == filter_key_map.end())
        {
            SWSS_LOG_ERROR("Invalid filter key: %s", filter.key.c_str());
            return SAI_NULL_OBJECT_ID;
        }
        sai_dash_flow_entry_bulk_get_session_filter_key_t filter_key = filter_key_it->second;

        attrs[attr_count].id = SAI_FLOW_ENTRY_BULK_GET_SESSION_FILTER_ATTR_DASH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_KEY;
        attrs[attr_count].value.s32 = filter_key;
        attr_count++;

        auto filter_op_it = filter_op_map.find(filter.op);
        if (filter_op_it == filter_op_map.end())
        {
            SWSS_LOG_ERROR("Invalid filter op: %s", filter.op.c_str());
            return SAI_NULL_OBJECT_ID;
        }
        sai_dash_flow_entry_bulk_get_session_op_key_t filter_op = filter_op_it->second;

        attrs[attr_count].id = SAI_FLOW_ENTRY_BULK_GET_SESSION_FILTER_ATTR_DASH_FLOW_ENTRY_BULK_GET_SESSION_OP_KEY;
        attrs[attr_count].value.s32 = filter_op;
        attr_count++;

        if (filter_key == SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_KEY_ENI_ADDR)
        {
            MacAddress mac(filter.value);
            attrs[attr_count].id = SAI_FLOW_ENTRY_BULK_GET_SESSION_FILTER_ATTR_MAC_VALUE;
            memcpy(attrs[attr_count].value.mac, mac.getMac(), 6);
            attr_count++;
        }
        else if (filter_key == SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_KEY_IP_PROTOCOL ||
                 filter_key == SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_KEY_SRC_L4_PORT ||
                 filter_key == SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_KEY_DST_L4_PORT ||
                 filter_key == SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_KEY_KEY_VERSION)
        {
            attrs[attr_count].id = SAI_FLOW_ENTRY_BULK_GET_SESSION_FILTER_ATTR_INT_VALUE;
            attrs[attr_count].value.u32 = static_cast<uint32_t>(stoul(filter.value));
            attr_count++;
        }
        else if (filter_key == SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_KEY_SRC_IP_ADDR ||
                 filter_key == SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_KEY_DST_IP_ADDR)
        {
            IpAddress ip(filter.value);
            attrs[attr_count].id = SAI_FLOW_ENTRY_BULK_GET_SESSION_FILTER_ATTR_IP_VALUE;
            swss::copy(attrs[attr_count].value.ipaddr, ip);
            attr_count++;
        }

        sai_object_id_t filter_id = SAI_NULL_OBJECT_ID;
        sai_status_t status = sai_dash_flow_api->create_flow_entry_bulk_get_session_filter(&filter_id, gSwitchId, attr_count, attrs);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create flow bulk get session filter, status: %d", status);
            return SAI_NULL_OBJECT_ID;
        }

        return filter_id;
    }
    catch (const exception &e)
    {
        SWSS_LOG_ERROR("Exception in FlowDumpFilterManager::createFilterSAI for filter key %s, op %s, value %s: %s", 
                       filter.key.c_str(), filter.op.c_str(), filter.value.c_str(), e.what());
        return SAI_NULL_OBJECT_ID;
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Unknown exception in FlowDumpFilterManager::createFilterSAI for filter key %s, op %s, value %s", 
                       filter.key.c_str(), filter.op.c_str(), filter.value.c_str());
        return SAI_NULL_OBJECT_ID;
    }
}

bool FlowDumpFilterManager::deleteFilterSAI(sai_object_id_t filter_id)
{
    SWSS_LOG_ENTER();

    sai_status_t status = sai_dash_flow_api->remove_flow_entry_bulk_get_session_filter(filter_id);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete flow bulk get session filter 0x%lx, status: %d", filter_id, status);
        return false;
    }

    return true;
}

FlowApiHandler::FlowApiHandler(DBConnector *dpu_state_db, SelectableTimer *timer) :
    m_session_id(SAI_NULL_OBJECT_ID),
    m_timer(timer)
{
    m_state_table = make_shared<Table>(dpu_state_db, STATE_DASH_FLOW_SYNC_SESSION_STATE_TABLE_NAME);
}

void FlowApiHandler::deleteSession()
{
    SWSS_LOG_ENTER();

    if (m_timer != nullptr)
    {
        m_timer->stop();
    }

    if (m_session_id != SAI_NULL_OBJECT_ID)
    {
        if (deleteSessionSAI())
        {
            m_session_id = SAI_NULL_OBJECT_ID;
        }
    }
}

bool FlowApiHandler::deleteSessionSAI()
{
    SWSS_LOG_ENTER();

    sai_status_t status = sai_dash_flow_api->remove_flow_entry_bulk_get_session(m_session_id);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete flow bulk get session 0x%lx, status: %d", m_session_id, status);
        return false;
    }

    return true;
}

void FlowApiHandler::updateState(const string &state, const string &key, vector<FieldValueTuple> fvs)
{
    chrono::steady_clock::time_point creation_time;
    chrono::steady_clock::time_point last_state_time = chrono::steady_clock::now();
    
    // Only update internal state if updating for the current session key
    if (key == m_key)
    {
        m_last_state_time = last_state_time;
        creation_time = m_creation_time;
    }
    else
    {
        // For different key, use current time for timestamps
        creation_time = chrono::steady_clock::now();
    }
    
    auto creation_time_ms = chrono::duration_cast<chrono::milliseconds>(creation_time.time_since_epoch()).count();
    auto last_state_time_ms = chrono::duration_cast<chrono::milliseconds>(last_state_time.time_since_epoch()).count();

    fvs.push_back(FieldValueTuple("state", state));
    fvs.push_back(FieldValueTuple("creation_time_in_ms", to_string(creation_time_ms)));
    fvs.push_back(FieldValueTuple("last_state_start_time_in_ms", to_string(last_state_time_ms)));

    m_state_table->set(key, fvs);
}

BulkSyncHandler::BulkSyncHandler(DBConnector *dpu_state_db, SelectableTimer *timer) :
    FlowApiHandler(dpu_state_db, timer)
{
}

bool BulkSyncHandler::initialize(const string &key, const vector<FieldValueTuple> &attrs)
{
    SWSS_LOG_ENTER();
    
    if (isActive())
    {
        SWSS_LOG_ERROR("BulkSyncHandler already active: %s. Cannot create new session: %s", m_key.c_str(), key.c_str());
        return false;
    }

    try
    {
        // m_key is already set in handleSet
        m_ha_set_id = SAI_NULL_OBJECT_ID;
        m_target_server_ip = "";
        m_target_server_port = 0;
        m_timeout_sec = DEFAULT_TIMEOUT_SEC;

        for (auto i = attrs.begin(); i != attrs.end(); i++)
        {
            const auto &attr = fvField(*i);
            const auto &value = fvValue(*i);
            
            if (attr == "target_server_ip")
            {
                m_target_server_ip = value;
            }
            else if (attr == "target_server_port")
            {
                m_target_server_port = static_cast<uint16_t>(stoul(value));
            }
            else if (attr == "timeout")
            {
                m_timeout_sec = static_cast<uint32_t>(stoul(value));
            }
        }
        return true;
    }
    catch (const exception &e)
    {
        SWSS_LOG_ERROR("Exception in BulkSyncHandler::initialize for key %s: %s", key.c_str(), e.what());
        return false;
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Unknown exception in BulkSyncHandler::initialize for key %s", key.c_str());
        return false;
    }
}

void BulkSyncHandler::reset()
{
    SWSS_LOG_ENTER();
    m_key = "";
    m_ha_set_id = SAI_NULL_OBJECT_ID;
    m_target_server_ip = "";
    m_target_server_port = 0;
    m_timeout_sec = BulkSyncHandler::DEFAULT_TIMEOUT_SEC;
}

task_process_status BulkSyncHandler::handleSet(const string &table_name, const string &key, const vector<FieldValueTuple> &attrs)
{
    SWSS_LOG_ENTER();

    if (table_name != APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME)
    {
        SWSS_LOG_ERROR("BulkSyncHandler::handleSet called with unknown table: %s", table_name.c_str());
        return task_failed;
    }

    if (isActive())
    {
        SWSS_LOG_ERROR("Flow sync session already exists: %s. Cannot create new session: %s", m_key.c_str(), key.c_str());
        FlowApiHandler::updateState("failed", key, {{"type", DashHaFlowOrch::SESSION_TYPE_BULK_SYNC}});
        return task_failed;
    }

    // Set key before initialize so state updates work correctly
    m_key = key;
    if (!initialize(key, attrs))
    {
        SWSS_LOG_ERROR("Failed to initialize BulkSyncHandler for key %s", key.c_str());
        FlowApiHandler::updateState("failed", key, {{"type", DashHaFlowOrch::SESSION_TYPE_BULK_SYNC}});
        reset();
        return task_failed;
    }

    task_process_status status = createSession();
    if (status != task_success && status != task_need_retry)
    {
        reset();
    }

    return status;
}

task_process_status BulkSyncHandler::handleDel(const string &table_name, const string &key)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_WARN("Deleting session %s is not supported, will be auto deleted when session is finished", key.c_str());
    return task_failed;
}

BulkSyncHandler::~BulkSyncHandler()
{
    deleteSession();
}

task_process_status BulkSyncHandler::createSession()
{
    SWSS_LOG_ENTER();

    if (m_target_server_ip.empty())
    {
        SWSS_LOG_ERROR("Missing target_server_ip for flow sync session %s", m_key.c_str());
        FlowApiHandler::updateState("failed", m_key, {{"type", DashHaFlowOrch::SESSION_TYPE_BULK_SYNC}});
        return task_failed;
    }

    if (m_target_server_port == 0)
    {
        SWSS_LOG_ERROR("Missing or invalid target_server_port for flow sync session %s", m_key.c_str());
        FlowApiHandler::updateState("failed", m_key, {{"type", DashHaFlowOrch::SESSION_TYPE_BULK_SYNC}});
        return task_failed;
    }

    sai_object_id_t session_id = createSessionSAI();
    if (session_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Failed to create flow sync session %s", m_key.c_str());
        FlowApiHandler::updateState("failed", m_key, {{"type", DashHaFlowOrch::SESSION_TYPE_BULK_SYNC}});
        return task_failed;
    }

    m_session_id = session_id;
    m_creation_time = chrono::steady_clock::now();
    m_last_state_time = m_creation_time;

    auto interval = timespec { .tv_sec = static_cast<time_t>(m_timeout_sec), .tv_nsec = 0 };
    m_timer->setInterval(interval);
    m_timer->start();

    FlowApiHandler::updateState("created", m_key, {{"type", DashHaFlowOrch::SESSION_TYPE_BULK_SYNC}});
    SWSS_LOG_NOTICE("Created flow sync session %s with session_id 0x%lx, timeout %u sec", m_key.c_str(), session_id, m_timeout_sec);

    return task_success;
}

void BulkSyncHandler::handleFinished()
{
    SWSS_LOG_ENTER();
    FlowApiHandler::updateState("completed", m_key, {{"type", DashHaFlowOrch::SESSION_TYPE_BULK_SYNC}});
    SWSS_LOG_NOTICE("Flow sync session %s completed successfully", m_key.c_str());
    deleteSession();
    reset();
}

void BulkSyncHandler::handleTimeout()
{
    SWSS_LOG_ENTER();
    FlowApiHandler::updateState("failed", m_key, {{"type", DashHaFlowOrch::SESSION_TYPE_BULK_SYNC}});
    SWSS_LOG_WARN("Flow sync session %s timed out", m_key.c_str());
    deleteSession();
    reset();
}

sai_object_id_t BulkSyncHandler::createSessionSAI()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attrs[4];
    uint32_t attr_count = 0;

    attrs[attr_count].id = SAI_FLOW_ENTRY_BULK_GET_SESSION_ATTR_DASH_FLOW_ENTRY_BULK_GET_SESSION_MODE;
    attrs[attr_count].value.s32 = SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_MODE_SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_MODE_VENDOR;
    attr_count++;

    IpAddress server_ip(m_target_server_ip);
    attrs[attr_count].id = SAI_FLOW_ENTRY_BULK_GET_SESSION_ATTR_BULK_GET_SESSION_SERVER_IP;
    swss::copy(attrs[attr_count].value.ipaddr, server_ip);
    attr_count++;

    attrs[attr_count].id = SAI_FLOW_ENTRY_BULK_GET_SESSION_ATTR_BULK_GET_SESSION_SERVER_PORT;
    attrs[attr_count].value.u16 = m_target_server_port;
    attr_count++;

    sai_object_id_t session_id = SAI_NULL_OBJECT_ID;
    sai_status_t status = sai_dash_flow_api->create_flow_entry_bulk_get_session(&session_id, gSwitchId, attr_count, attrs);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create flow sync session, status: %d", status);
        return SAI_NULL_OBJECT_ID;
    }

    return session_id;
}

FlowDumpHandler::FlowDumpHandler(DBConnector *dpu_state_db, SelectableTimer *timer, std::shared_ptr<FlowDumpFilterManager> filter_manager) :
    FlowApiHandler(dpu_state_db, timer),
    m_filter_manager(filter_manager)
{
}

bool FlowDumpHandler::initialize(const string &key, const vector<FieldValueTuple> &attrs)
{
    SWSS_LOG_ENTER();

    try
    {
        // m_key is already set in handleSet
        m_flow_state = false;
        m_max_flows = FlowDumpHandler::MAX_FLOWS_DEFAULT;
        m_timeout_sec = FlowDumpHandler::DEFAULT_TIMEOUT_SEC;
        m_output_file = "";
        m_required_filter_keys.clear();

        for (auto i = attrs.begin(); i != attrs.end(); i++)
        {
            const auto &attr = fvField(*i);
            const auto &value = fvValue(*i);

            if (attr == "flow_state")
            {
                string lower_value = value;
                transform(lower_value.begin(), lower_value.end(), lower_value.begin(), ::tolower);
                m_flow_state = (lower_value == "true");
            }
            else if (attr == "filter_1" || attr == "filter_2" || attr == "filter_3" || attr == "filter_4" || attr == "filter_5")
            {
                m_required_filter_keys.push_back(value);
            }
            else if (attr == "max_flows")
            {
                m_max_flows = static_cast<uint32_t>(stoul(value));
            }
            else if (attr == "timeout")
            {
                m_timeout_sec = static_cast<uint32_t>(stoul(value));
            }
        }
        return true;
    }
    catch (const exception &e)
    {
        SWSS_LOG_ERROR("Exception in FlowDumpHandler::initialize for key %s: %s", key.c_str(), e.what());
        return false;
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Unknown exception in FlowDumpHandler::initialize for key %s", key.c_str());
        return false;
    }
}

void FlowDumpHandler::reset()
{
    SWSS_LOG_ENTER();

    m_key = "";
    m_flow_state = true;
    m_required_filter_keys.clear();
    m_max_flows = 0;
    m_timeout_sec = DEFAULT_TIMEOUT_SEC;
    m_output_file = "";
}

FlowDumpHandler::~FlowDumpHandler()
{
    deleteSession();
}

task_process_status FlowDumpHandler::createSession()
{
    SWSS_LOG_ENTER();

    vector<sai_object_id_t> filter_ids = m_filter_manager->getFilterIds(m_required_filter_keys);

    if (filter_ids.size() != m_required_filter_keys.size())
    {
        SWSS_LOG_INFO("Flow dump session %s waiting for filters to become available (%zu/%zu)", m_key.c_str(), filter_ids.size(), m_required_filter_keys.size());
        FlowApiHandler::updateState("pending", m_key, {{"type", DashHaFlowOrch::SESSION_TYPE_FLOW_DUMP}});
        return task_need_retry;
    }

    sai_object_id_t session_id = createSessionSAI();
    if (session_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Failed to create flow dump session %s", m_key.c_str());
        FlowApiHandler::updateState("failed", m_key, {{"type", DashHaFlowOrch::SESSION_TYPE_FLOW_DUMP}});
        return task_failed;
    }

    m_session_id = session_id;

    auto interval = timespec { .tv_sec = static_cast<time_t>(m_timeout_sec), .tv_nsec = 0 };
    m_timer->setInterval(interval);
    m_timer->start();

    FlowApiHandler::updateState("created", m_key, {{"type", DashHaFlowOrch::SESSION_TYPE_FLOW_DUMP}});

    SWSS_LOG_NOTICE("Created flow dump session %s with session_id 0x%lx, timeout %u sec", m_key.c_str(), session_id, m_timeout_sec);

    return task_success;
}


task_process_status FlowDumpHandler::handleSet(const string &table_name, const string &key, const vector<FieldValueTuple> &attrs)
{
    SWSS_LOG_ENTER();

    if (table_name == APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME)
    {
        if (isActive())
        {
            SWSS_LOG_ERROR("Flow dump session already exists: %s. Cannot create new session: %s", m_key.c_str(), key.c_str());
            FlowApiHandler::updateState("failed", key, {{"type", DashHaFlowOrch::SESSION_TYPE_FLOW_DUMP}});
            return task_failed;
        }

        // Set key before initialize so state updates work correctly
        m_key = key;
        if (!initialize(key, attrs))
        {
            SWSS_LOG_ERROR("Failed to initialize FlowDumpHandler for key %s", key.c_str());
            FlowApiHandler::updateState("failed", key, {{"type", DashHaFlowOrch::SESSION_TYPE_FLOW_DUMP}});
            reset();
            return task_failed;
        }

        task_process_status status = createSession();
        if (status != task_success && status != task_need_retry)
        {
            reset();
        }
        return status;
    }
    else
    {
        SWSS_LOG_ERROR("FlowDumpHandler::handleSet called with unknown table: %s", table_name.c_str());
        return task_failed;
    }
}

task_process_status FlowDumpHandler::handleDel(const string &table_name, const string &key)
{
    SWSS_LOG_ENTER();

    if (table_name == APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME)
    {
        SWSS_LOG_WARN("Deleting session %s is not supported, will be auto deleted when session is finished", key.c_str());
        return task_failed;
    }
    else
    {
        SWSS_LOG_ERROR("FlowDumpHandler::handleDel called with unknown table: %s", table_name.c_str());
        return task_failed;
    }
}


void FlowDumpHandler::handleFinished()
{
    SWSS_LOG_ENTER();

    ostringstream oss;
    oss << "/var/dump/flows/flow_dump_0x" << hex << setfill('0') << setw(16) << m_session_id << ".jsonl.gz";
    m_output_file = oss.str();

    vector<FieldValueTuple> fvs;
    fvs.push_back(FieldValueTuple("type", DashHaFlowOrch::SESSION_TYPE_FLOW_DUMP));
    fvs.push_back(FieldValueTuple("output_file", m_output_file));
    FlowApiHandler::updateState("completed", m_key, fvs);
    SWSS_LOG_NOTICE("Flow dump session %s completed successfully, output file: %s", m_key.c_str(), m_output_file.c_str());

    deleteSession();
    reset();
}

void FlowDumpHandler::handleTimeout()
{
    SWSS_LOG_ENTER();

    FlowApiHandler::updateState("failed", m_key, {{"type", DashHaFlowOrch::SESSION_TYPE_FLOW_DUMP}});
    deleteSession();
    reset();

    SWSS_LOG_WARN("Flow dump session %s timed out", m_key.c_str());
}

sai_object_id_t FlowDumpHandler::createSessionSAI()
{
    SWSS_LOG_ENTER();

    vector<sai_attribute_t> attrs;

    sai_dash_flow_entry_bulk_get_session_mode_t mode;
    if (m_flow_state)
    {
        mode = SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_MODE_SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_MODE_EVENT;
    }
    else
    {
        mode = SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_MODE_SAI_DASH_FLOW_ENTRY_BULK_GET_SESSION_MODE_EVENT_WITHOUT_FLOW_STATE;
    }

    sai_attribute_t attr;
    attr.id = SAI_FLOW_ENTRY_BULK_GET_SESSION_ATTR_DASH_FLOW_ENTRY_BULK_GET_SESSION_MODE;
    attr.value.s32 = mode;
    attrs.push_back(attr);

    if (m_max_flows > 0)
    {
        attr.id = SAI_FLOW_ENTRY_BULK_GET_SESSION_ATTR_BULK_GET_ENTRY_LIMITATION;
        attr.value.u32 = m_max_flows;
        attrs.push_back(attr);
    }

    sai_attr_id_t filter_attr_ids[] = {
        SAI_FLOW_ENTRY_BULK_GET_SESSION_ATTR_FIRST_FLOW_ENTRY_BULK_GET_SESSION_FILTER_ID,
        SAI_FLOW_ENTRY_BULK_GET_SESSION_ATTR_SECOND_FLOW_ENTRY_BULK_GET_SESSION_FILTER_ID,
        SAI_FLOW_ENTRY_BULK_GET_SESSION_ATTR_THIRD_FLOW_ENTRY_BULK_GET_SESSION_FILTER_ID,
        SAI_FLOW_ENTRY_BULK_GET_SESSION_ATTR_FOURTH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_ID,
        SAI_FLOW_ENTRY_BULK_GET_SESSION_ATTR_FIFTH_FLOW_ENTRY_BULK_GET_SESSION_FILTER_ID
    };

    vector<sai_object_id_t> filter_ids = m_filter_manager->getFilterIds(m_required_filter_keys);
    for (size_t i = 0; i < filter_ids.size() && i < 5; i++)
    {
        attr.id = filter_attr_ids[i];
        attr.value.oid = filter_ids[i];
        attrs.push_back(attr);
    }

    sai_object_id_t session_id = SAI_NULL_OBJECT_ID;
    sai_status_t status = sai_dash_flow_api->create_flow_entry_bulk_get_session(&session_id, gSwitchId, static_cast<uint32_t>(attrs.size()), attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create flow dump session, status: %d", status);
        return SAI_NULL_OBJECT_ID;
    }

    return session_id;
}

DashHaFlowOrch::DashHaFlowOrch(DBConnector *db, const vector<string> &tableNames, DBConnector *app_state_db, ZmqServer *zmqServer) :
    ZmqOrch(db, tableNames, zmqServer)
{
    SWSS_LOG_ENTER();

    m_dpuStateDb = make_unique<DBConnector>("DPU_STATE_DB", 0, true);

    auto sync_interval = timespec { .tv_sec = 0, .tv_nsec = 0 };
    m_sync_timer = new SelectableTimer(sync_interval);
    m_sync_executor = new ExecutableTimer(m_sync_timer, this, "FLOW_SYNC_SESSION_TIMER");
    Orch::addExecutor(m_sync_executor);

    auto dump_interval = timespec { .tv_sec = 0, .tv_nsec = 0 };
    m_dump_timer = new SelectableTimer(dump_interval);
    m_dump_executor = new ExecutableTimer(m_dump_timer, this, "FLOW_DUMP_SESSION_TIMER");
    Orch::addExecutor(m_dump_executor);

    m_filter_manager = std::make_shared<FlowDumpFilterManager>();
    m_handlers[SESSION_TYPE_BULK_SYNC] = make_shared<BulkSyncHandler>(m_dpuStateDb.get(), m_sync_timer);
    m_handlers[SESSION_TYPE_FLOW_DUMP] = make_shared<FlowDumpHandler>(m_dpuStateDb.get(), m_dump_timer, m_filter_manager);

    DBConnector *notificationsDb = new DBConnector("ASIC_DB", 0);
    m_flowBulkGetSessionNotificationConsumer = new NotificationConsumer(notificationsDb, "NOTIFICATIONS");
    auto flowBulkGetSessionNotifier = new Notifier(m_flowBulkGetSessionNotificationConsumer, this, SAI_SWITCH_NOTIFICATION_NAME_FLOW_BULK_GET_SESSION_EVENT);

    Orch::addExecutor(flowBulkGetSessionNotifier);

    registerFlowBulkGetSessionNotifier();
}

bool DashHaFlowOrch::registerFlowBulkGetSessionNotifier()
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    sai_status_t status;
    sai_attr_capability_t capability;

    status = sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_SWITCH,
                                            SAI_SWITCH_ATTR_FLOW_BULK_GET_SESSION_EVENT_NOTIFY,
                                            &capability);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Unable to query the Flow Bulk Get Session event notification capability");
        return false;
    }

    if (!capability.set_implemented)
    {
        SWSS_LOG_INFO("Flow Bulk Get Session event notification not supported");
        return false;
    }

    attr.id = SAI_SWITCH_ATTR_FLOW_BULK_GET_SESSION_EVENT_NOTIFY;
    attr.value.ptr = (void *)on_flow_bulk_get_session_event;

    status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to register Flow Bulk Get Session event notification");
        return false;
    }

    return true;
}

void DashHaFlowOrch::doTask(ConsumerBase &consumer)
{
    SWSS_LOG_ENTER();

    const auto& tn = consumer.getTableName();

    if (tn == APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME)
    {
        doTaskFlowSyncSessionTable(consumer);
    }
    else if (tn == APP_DASH_FLOW_DUMP_FILTER_TABLE_NAME)
    {
        doTaskFlowDumpFilterTable(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown table: %s", tn.c_str());
    }
}

void DashHaFlowOrch::doTask(NotificationConsumer &consumer)
{
    SWSS_LOG_ENTER();

    std::string notification_name;
    std::string data;
    std::vector<FieldValueTuple> values;

    consumer.pop(notification_name, data, values);

    if (notification_name == SAI_SWITCH_NOTIFICATION_NAME_FLOW_BULK_GET_SESSION_EVENT)
    {
        handleSessionNotification(notification_name, data, values);
    }
    else
    {
        SWSS_LOG_WARN("Unknown notification: %s", notification_name.c_str());
    }
}

void DashHaFlowOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    handleTimerExpired(&timer);
}

void DashHaFlowOrch::doTaskFlowSyncSessionTable(ConsumerBase &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);
        task_process_status status = task_failed;

        if (op == SET_COMMAND)
        {
            string type = getTypeFromAttrs(kfvFieldsValues(t));
            auto h_it = m_handlers.find(type);
            if (h_it == m_handlers.end())
            {
                SWSS_LOG_ERROR("Invalid or missing type field in session %s. Expected '%s' or '%s'", key.c_str(), SESSION_TYPE_BULK_SYNC, SESSION_TYPE_FLOW_DUMP);
                status = task_failed;
            }
            else
            {
                status = h_it->second->handleSet(APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME, key, kfvFieldsValues(t));
            }
        }
        else if (op == DEL_COMMAND)
        {
            // Find which handler has this key
            bool found = false;
            for (auto &h_pair : m_handlers)
            {
                if (h_pair.second->getKey() == key)
                {
                    status = h_pair.second->handleDel(APP_DASH_FLOW_SYNC_SESSION_TABLE_NAME, key);
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                SWSS_LOG_WARN("Session %s not found in any handler", key.c_str());
                status = task_success;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation %s", op.c_str());
        }

        if (status == task_need_retry)
        {
            it++;
        }
        else
        {
            it = consumer.m_toSync.erase(it);
        }
    }
}

void DashHaFlowOrch::doTaskFlowDumpFilterTable(ConsumerBase &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        auto t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);
        task_process_status status = task_failed;

        if (op == SET_COMMAND)
        {
            status = m_filter_manager->addFilter(key, kfvFieldsValues(t));
        }
        else if (op == DEL_COMMAND)
        {
            status = m_filter_manager->removeFilter(key);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation %s", op.c_str());
        }

        if (status == task_need_retry)
        {
            it++;
        }
        else
        {
            it = consumer.m_toSync.erase(it);
        }
    }
}

string DashHaFlowOrch::getTypeFromAttrs(const vector<FieldValueTuple> &attrs)
{
    SWSS_LOG_ENTER();

    for (const auto &attr : attrs)
    {
        if (fvField(attr) == "type")
        {
            return fvValue(attr);
        }
    }

    return "";
}

void DashHaFlowOrch::handleSessionNotification(const string &notification_name, const string &data, const vector<FieldValueTuple> &values)
{
    SWSS_LOG_ENTER();

    sai_object_id_t flow_bulk_session_id;
    uint32_t count;
    sai_flow_bulk_get_session_event_data_t* event_data;

    sai_deserialize_flow_bulk_get_session_event_ntf(data, flow_bulk_session_id, count, &event_data);

    for (uint32_t i = 0; i < count; i++)
    {
        if (event_data[i].event_type == SAI_FLOW_BULK_GET_SESSION_EVENT_FINISHED)
        {
            handleSessionFinished(flow_bulk_session_id);
        }
    }

    sai_deserialize_free_flow_bulk_get_session_event_ntf(count, event_data);
}

void DashHaFlowOrch::handleSessionFinished(sai_object_id_t session_id)
{
    SWSS_LOG_ENTER();

    bool found = false;
    for (auto &h_pair : m_handlers)
    {
        if (h_pair.second->getSessionId() == session_id)
        {
            h_pair.second->handleFinished();
            SWSS_LOG_NOTICE("Session ID 0x%lx finished (type: %s)", session_id, h_pair.first.c_str());
            found = true;
            break;
        }
    }

    if (!found)
    {
        SWSS_LOG_WARN("Received FINISHED notification for unknown session 0x%lx", session_id);
    }
}

void DashHaFlowOrch::handleTimerExpired(SelectableTimer *timer)
{
    SWSS_LOG_ENTER();

    bool found = false;
    for (auto &h_pair : m_handlers)
    {
        if (h_pair.second->getTimer() == timer)
        {
            h_pair.second->handleTimeout();
            SWSS_LOG_NOTICE("Timer expired for handler (type: %s)", h_pair.first.c_str());
            found = true;
            break;
        }
    }

    if (!found)
    {
        SWSS_LOG_WARN("Timer not found in any handler");
    }
}
