#include "dashportmaporch.h"
#include "orch.h"
#include "dashorch.h"
#include "taskworker.h"
#include "bulker.h"
#include "pbutils.h"

extern size_t gMaxBulkSize;
extern sai_dash_outbound_port_map_api_t *sai_dash_outbound_port_map_api;
extern sai_object_id_t gSwitchId;

static const std::unordered_map<dash::outbound_port_map_range::PortMapRangeAction,
                                sai_outbound_port_map_port_range_entry_action_t>
    gPortMapRangeActionMap = {
        {dash::outbound_port_map_range::PortMapRangeAction::ACTION_SKIP_MAPPING,
         SAI_OUTBOUND_PORT_MAP_PORT_RANGE_ENTRY_ACTION_SKIP_MAPPING},
        {dash::outbound_port_map_range::PortMapRangeAction::ACTION_MAP_PRIVATE_LINK_SERVICE,
         SAI_OUTBOUND_PORT_MAP_PORT_RANGE_ENTRY_ACTION_MAP_TO_PRIVATE_LINK_SERVICE}};

DashPortMapOrch::DashPortMapOrch(swss::DBConnector *db, std::vector<std::string> &tables, swss::DBConnector *app_state_db, swss::ZmqServer *zmqServer) : ZmqOrch(db, tables, zmqServer),
                                                                                                                                                         port_map_bulker_(sai_dash_outbound_port_map_api, gSwitchId, gMaxBulkSize),
                                                                                                                                                         port_map_range_bulker_(sai_dash_outbound_port_map_api, gMaxBulkSize)
{
    SWSS_LOG_ENTER();
    dash_port_map_result_table_ = std::make_unique<swss::Table>(app_state_db, APP_DASH_OUTBOUND_PORT_MAP_TABLE_NAME);
    dash_port_map_range_result_table_ = std::make_unique<swss::Table>(app_state_db, APP_DASH_OUTBOUND_PORT_MAP_RANGE_TABLE_NAME);
}

void DashPortMapOrch::doTask(ConsumerBase &consumer)
{
    SWSS_LOG_ENTER();

    const auto &tn = consumer.getTableName();

    SWSS_LOG_INFO("Table name: %s", tn.c_str());

    if (tn == APP_DASH_OUTBOUND_PORT_MAP_TABLE_NAME)
    {
        doTaskPortMapTable(consumer);
    }
    else if (tn == APP_DASH_OUTBOUND_PORT_MAP_RANGE_TABLE_NAME)
    {
        doTaskPortMapRangeTable(consumer);
    }
    else
    {
        SWSS_LOG_ERROR("Unknown table: %s", tn.c_str());
    }
}

void DashPortMapOrch::doTaskPortMapTable(ConsumerBase &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    uint32_t result;

    std::map<std::pair<std::string, std::string>,
             DashPortMapBulkContext>
        toBulk;
    while (it != consumer.m_toSync.end())
    {
        swss::KeyOpFieldsValuesTuple tuple = it->second;
        std::string port_map_id = kfvKey(tuple);
        std::string op = kfvOp(tuple);
        auto rc = toBulk.emplace(std::piecewise_construct,
                                 std::forward_as_tuple(port_map_id, op),
                                 std::forward_as_tuple());
        bool inserted = rc.second;
        auto &ctxt = rc.first->second;
        result = DASH_RESULT_SUCCESS;
        SWSS_LOG_INFO("Processing port map entry: %s, operation: %s", port_map_id.c_str(), op.c_str());

        if (!inserted)
        {
            ctxt.clear();
        }

        if (op == SET_COMMAND)
        {
            // the only info we need is the port map ID which is provided in the key
            // no need to parse protobuf message here

            if (addPortMap(port_map_id, ctxt))
            {
                it = consumer.m_toSync.erase(it);
                // the only reason to remove from consumer prior to flush is if the port map already exists,
                // so treat it like a success
                writeResultToDB(dash_port_map_result_table_, port_map_id, result);
            }
            else
            {
                it++;
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (removePortMap(port_map_id, ctxt))
            {
                it = consumer.m_toSync.erase(it);
                removeResultFromDB(dash_port_map_result_table_, port_map_id);
            }
            else
            {
                it++;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }

    port_map_bulker_.flush();

    auto it_prev = consumer.m_toSync.begin();
    while (it_prev != it)
    {
        swss::KeyOpFieldsValuesTuple tuple = it_prev->second;
        std::string port_map_id = kfvKey(tuple);
        std::string op = kfvOp(tuple);
        result = DASH_RESULT_SUCCESS;
        auto found = toBulk.find(std::make_pair(port_map_id, op));
        if (found == toBulk.end())
        {
            it_prev++;
            continue;
        }

        auto &ctxt = found->second;
        if (ctxt.port_map_oids.empty() && ctxt.port_map_statuses.empty())
        {
            it_prev++;
            continue;
        }

        if (op == SET_COMMAND)
        {
            if (addPortMapPost(port_map_id, ctxt))
            {
                it_prev = consumer.m_toSync.erase(it_prev);
            }
            else
            {
                result = DASH_RESULT_FAILURE;
                it_prev++;
            }
            writeResultToDB(dash_port_map_result_table_, port_map_id, result);
        }
        else if (op == DEL_COMMAND)
        {
            if (removePortMapPost(port_map_id, ctxt))
            {
                it_prev = consumer.m_toSync.erase(it_prev);
                removeResultFromDB(dash_port_map_result_table_, port_map_id);
            }
            else
            {
                it_prev++;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation %s", op.c_str());
            it_prev = consumer.m_toSync.erase(it_prev);
        }
    }
}

bool DashPortMapOrch::addPortMap(const std::string &port_map_id, DashPortMapBulkContext &ctxt)
{
    SWSS_LOG_ENTER();

    if (port_map_table_.find(port_map_id) != port_map_table_.end())
    {
        SWSS_LOG_WARN("Port map %s already exists", port_map_id.c_str());
        return true;
    }

    std::vector<sai_attribute_t> attrs;
    sai_attribute_t attr;
    attr.id = SAI_OUTBOUND_PORT_MAP_ATTR_COUNTER_ID;
    attr.value.oid = SAI_NULL_OBJECT_ID;
    attrs.push_back(attr);
    auto &object_ids = ctxt.port_map_oids;
    object_ids.emplace_back();
    port_map_bulker_.create_entry(&object_ids.back(), (uint32_t)attrs.size(), attrs.data());
    SWSS_LOG_INFO("Adding port map %s to bulker", port_map_id.c_str());
    return false;
}

bool DashPortMapOrch::addPortMapPost(const std::string &port_map_id, DashPortMapBulkContext &ctxt)
{
    SWSS_LOG_ENTER();

    auto &object_ids = ctxt.port_map_oids;
    if (object_ids.empty())
    {
        return false;
    }

    auto it_status = object_ids.begin();
    sai_object_id_t port_map_oid = *it_status++;
    if (port_map_oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("Failed to create port map %s", port_map_id.c_str());
        return false;
    }

    port_map_table_[port_map_id] = port_map_oid;
    SWSS_LOG_NOTICE("Created port map %s with OID 0x%" PRIx64, port_map_id.c_str(), port_map_oid);
    return true;
}

bool DashPortMapOrch::removePortMap(const std::string &port_map_id, DashPortMapBulkContext &ctxt)
{
    SWSS_LOG_ENTER();

    auto it = port_map_table_.find(port_map_id);
    if (it == port_map_table_.end())
    {
        SWSS_LOG_WARN("Port map %s not found for removal", port_map_id.c_str());
        return true;
    }

    auto &object_statuses = ctxt.port_map_statuses;
    object_statuses.emplace_back();
    sai_object_id_t port_map_oid = port_map_table_[port_map_id];
    port_map_bulker_.remove_entry(&object_statuses.back(), port_map_oid);
    SWSS_LOG_NOTICE("Removing port map %s with OID 0x%" PRIx64, port_map_id.c_str(), port_map_oid);

    return false;
}

bool DashPortMapOrch::removePortMapPost(const std::string &port_map_id, DashPortMapBulkContext &ctxt)
{
    SWSS_LOG_ENTER();

    auto &object_statuses = ctxt.port_map_statuses;
    if (object_statuses.empty())
    {
        return false;
    }

    auto it_status = object_statuses.begin();
    sai_status_t status = *it_status++;
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_NOT_EXECUTED)
        {
            SWSS_LOG_INFO("Port map %s not removed, will retry later", port_map_id.c_str());
            return false;
        }
        SWSS_LOG_ERROR("Failed to remove port map %s, status: %s", port_map_id.c_str(), sai_serialize_status(status).c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t)SAI_API_DASH_OUTBOUND_PORT_MAP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    port_map_table_.erase(port_map_id);
    SWSS_LOG_NOTICE("Removed port map %s", port_map_id.c_str());
    return true;
}

void DashPortMapOrch::doTaskPortMapRangeTable(ConsumerBase &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    uint32_t result;

    std::map<std::pair<std::string, std::string>,
             DashPortMapRangeBulkContext>
        toBulk;
    while (it != consumer.m_toSync.end())
    {
        swss::KeyOpFieldsValuesTuple tuple = it->second;
        std::string key = kfvKey(tuple);
        std::string op = kfvOp(tuple);
        auto rc = toBulk.emplace(std::piecewise_construct,
                                 std::forward_as_tuple(key, op),
                                 std::forward_as_tuple());
        bool inserted = rc.second;
        auto &ctxt = rc.first->second;
        result = DASH_RESULT_FAILURE;
        SWSS_LOG_INFO("Processing port map range entry: %s, operation: %s", key.c_str(), op.c_str());

        if (!inserted)
        {
            ctxt.clear();
        }

        if (!parsePortMapRange(key, ctxt))
        {
            SWSS_LOG_ERROR("Failed to parse port map range key: %s", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            if (!parsePbMessage(kfvFieldsValues(tuple), ctxt.metadata))
            {
                SWSS_LOG_ERROR("Failed to parse protobuf message for port map range %s", key.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (addPortMapRange(ctxt))
            {
                it = consumer.m_toSync.erase(it);
                // if we ever remove from consumer early, that means parsing was unsuccessful and a retry will not help,
                // so treat it as a failure
                writeResultToDB(dash_port_map_range_result_table_, key, result);
            }
            else
            {
                it++;
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (removePortMapRange(ctxt))
            {
                it = consumer.m_toSync.erase(it);
                removeResultFromDB(dash_port_map_range_result_table_, key);
            }
            else
            {
                it++;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation %s", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }

    port_map_range_bulker_.flush();
    auto it_prev = consumer.m_toSync.begin();
    while (it_prev != it)
    {
        swss::KeyOpFieldsValuesTuple tuple = it_prev->second;
        std::string key = kfvKey(tuple);
        std::string op = kfvOp(tuple);
        result = DASH_RESULT_SUCCESS;
        auto found = toBulk.find(std::make_pair(key, op));
        if (found == toBulk.end())
        {
            it_prev++;
            continue;
        }
        auto &ctxt = found->second;
        if (ctxt.port_map_range_statuses.empty())
        {
            it_prev++;
            continue;
        }

        if (op == SET_COMMAND)
        {
            if (addPortMapRangePost(ctxt))
            {
                it_prev = consumer.m_toSync.erase(it_prev);
            }
            else
            {
                result = DASH_RESULT_FAILURE;
                it_prev++;
            }
            writeResultToDB(dash_port_map_range_result_table_, key, result);
        }
        else if (op == DEL_COMMAND)
        {
            if (removePortMapRangePost(ctxt))
            {
                it_prev = consumer.m_toSync.erase(it_prev);
                removeResultFromDB(dash_port_map_range_result_table_, key);
            }
            else
            {
                it_prev++;
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation %s", op.c_str());
            it_prev = consumer.m_toSync.erase(it_prev);
        }
    }
}

bool DashPortMapOrch::addPortMapRange(DashPortMapRangeBulkContext &ctxt)
{
    SWSS_LOG_ENTER();

    auto parent_it = port_map_table_.find(ctxt.parent_map_id);
    if (parent_it == port_map_table_.end())
    {
        SWSS_LOG_INFO("Parent port map %s does not exist for port map range", ctxt.parent_map_id.c_str());
        return false;
    }

    sai_outbound_port_map_port_range_entry_t entry;
    entry.switch_id = gSwitchId;
    entry.outbound_port_map_id = parent_it->second;
    sai_u32_range_t port_range;
    port_range.min = ctxt.start_port;
    port_range.max = ctxt.end_port;
    entry.dst_port_range = port_range;

    std::vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    auto action_it = gPortMapRangeActionMap.find(ctxt.metadata.action());
    if (action_it == gPortMapRangeActionMap.end())
    {
        SWSS_LOG_ERROR("Unknown port map range action: %s", dash::outbound_port_map_range::PortMapRangeAction_Name(ctxt.metadata.action()).c_str());
        return true;
    }

    attr.id = SAI_OUTBOUND_PORT_MAP_PORT_RANGE_ENTRY_ATTR_ACTION;
    attr.value.s32 = action_it->second;
    attrs.push_back(attr);

    attr.id = SAI_OUTBOUND_PORT_MAP_PORT_RANGE_ENTRY_ATTR_BACKEND_IP;
    if (!to_sai(ctxt.metadata.backend_ip(), attr.value.ipaddr))
    {
        SWSS_LOG_ERROR("Failed to convert backend IP %s to SAI format", ctxt.metadata.backend_ip().DebugString().c_str());
        return true;
    }
    attrs.push_back(attr);

    attr.id = SAI_OUTBOUND_PORT_MAP_PORT_RANGE_ENTRY_ATTR_MATCH_PORT_BASE;
    attr.value.u32 = ctxt.start_port;
    attrs.push_back(attr);

    attr.id = SAI_OUTBOUND_PORT_MAP_PORT_RANGE_ENTRY_ATTR_BACKEND_PORT_BASE;
    attr.value.u32 = ctxt.metadata.backend_port_base();
    attrs.push_back(attr);

    auto &object_statuses = ctxt.port_map_range_statuses;
    object_statuses.emplace_back();
    port_map_range_bulker_.create_entry(&object_statuses.back(), &entry, (uint32_t)attrs.size(), attrs.data());
    SWSS_LOG_INFO("Adding port map range for %s: start=%d, end=%d", ctxt.parent_map_id.c_str(), ctxt.start_port, ctxt.end_port);
    return false;
}

bool DashPortMapOrch::addPortMapRangePost(DashPortMapRangeBulkContext &ctxt)
{
    SWSS_LOG_ENTER();

    auto &object_statuses = ctxt.port_map_range_statuses;
    if (object_statuses.empty())
    {
        return false;
    }

    auto it_status = object_statuses.begin();
    sai_status_t status = *it_status++;
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_NOT_EXECUTED)
        {
            SWSS_LOG_INFO("Port map range for %s not created, will retry later", ctxt.parent_map_id.c_str());
            return false;
        }
        SWSS_LOG_ERROR("Failed to create port map range for %s, status: %s", ctxt.parent_map_id.c_str(), sai_serialize_status(status).c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t)SAI_API_DASH_OUTBOUND_PORT_MAP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_INFO("Created port map range for %s: start=%d, end=%d", ctxt.parent_map_id.c_str(), ctxt.start_port, ctxt.end_port);
    return true;
}

bool DashPortMapOrch::removePortMapRange(DashPortMapRangeBulkContext &ctxt)
{
    SWSS_LOG_ENTER();

    auto parent_it = port_map_table_.find(ctxt.parent_map_id);
    if (parent_it == port_map_table_.end())
    {
        // this should never happen - it's not possible to create a port map range w/o first creating the parent port map,
        // and it's not possible to delete a port map while it still has child port map ranges
        SWSS_LOG_ERROR("Parent port map %s not found for port map range removal", ctxt.parent_map_id.c_str());
        return true;
    }

    sai_outbound_port_map_port_range_entry_t entry;
    entry.switch_id = gSwitchId;
    entry.outbound_port_map_id = parent_it->second;
    sai_u32_range_t port_range;
    port_range.min = ctxt.start_port;
    port_range.max = ctxt.end_port;
    entry.dst_port_range = port_range;

    auto &object_statuses = ctxt.port_map_range_statuses;
    object_statuses.emplace_back();
    port_map_range_bulker_.remove_entry(&object_statuses.back(), &entry);
    SWSS_LOG_NOTICE("Removing port map range for %s: start=%d, end=%d", ctxt.parent_map_id.c_str(), ctxt.start_port, ctxt.end_port);
    return false;
}

bool DashPortMapOrch::removePortMapRangePost(DashPortMapRangeBulkContext &ctxt)
{
    SWSS_LOG_ENTER();

    auto &object_statuses = ctxt.port_map_range_statuses;
    if (object_statuses.empty())
    {
        return false;
    }

    auto it_status = object_statuses.begin();
    sai_status_t status = *it_status++;
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_NOT_EXECUTED)
        {
            SWSS_LOG_INFO("Port map range for %s not removed, will retry later", ctxt.parent_map_id.c_str());
            return false;
        }
        SWSS_LOG_ERROR("Failed to remove port map range for %s, status: %s", ctxt.parent_map_id.c_str(), sai_serialize_status(status).c_str());
        task_process_status handle_status = handleSaiCreateStatus((sai_api_t)SAI_API_DASH_OUTBOUND_PORT_MAP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    SWSS_LOG_NOTICE("Removed port map range for %s: start=%d, end=%d", ctxt.parent_map_id.c_str(), ctxt.start_port, ctxt.end_port);
    return true;
}

bool DashPortMapOrch::parsePortMapRange(const std::string &key, DashPortMapRangeBulkContext &ctxt)
{
    SWSS_LOG_ENTER();

    // Example key format: PORT_MAP_1:1000-2000
    size_t pos = key.find(':');
    if (pos == std::string::npos)
    {
        SWSS_LOG_ERROR("Invalid port map range key format: %s", key.c_str());
        return false;
    }

    ctxt.parent_map_id = key.substr(0, pos);
    std::string range = key.substr(pos + 1);

    size_t dash_pos = range.find('-');
    if (dash_pos == std::string::npos)
    {
        SWSS_LOG_ERROR("Invalid port range format: %s", range.c_str());
        return false;
    }

    try
    {
        ctxt.start_port = std::stoi(range.substr(0, dash_pos));
        ctxt.end_port = std::stoi(range.substr(dash_pos + 1));
    }
    catch (const std::invalid_argument &e)
    {
        SWSS_LOG_ERROR("Invalid port range values in key %s: %s", key.c_str(), e.what());
        return false;
    }

    SWSS_LOG_INFO("Parsed port range for %s: start=%d, end=%d", ctxt.parent_map_id.c_str(), ctxt.start_port, ctxt.end_port);
    return true;
}