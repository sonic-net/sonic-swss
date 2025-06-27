#include "dashportmaporch.h"
#include "orch.h"
#include "dashorch.h"
#include "taskworker.h"
#include "bulker.h"

extern size_t gMaxBulkSize;
extern sai_dash_outbound_port_map_api_t* sai_dash_outbound_port_map_api;
extern sai_object_id_t gSwitchId;

DashPortMapOrch::DashPortMapOrch(swss::DBConnector *db, std::vector<std::string> &tables, swss::DBConnector *app_state_db, swss::ZmqServer *zmqServer) :
    ZmqOrch(db, tables, zmqServer),
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

    const auto& tn = consumer.getTableName();

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
            DashPortMapBulkContext> toBulk;
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
    auto& object_ids = ctxt.port_map_oids;
    object_ids.emplace_back();
    port_map_bulker_.create_entry(&object_ids.back(), (uint32_t) attrs.size(), attrs.data());
    SWSS_LOG_INFO("Adding port map %s to bulker", port_map_id.c_str());
    return false;
}

bool DashPortMapOrch::addPortMapPost(const std::string &port_map_id, DashPortMapBulkContext &ctxt)
{
    SWSS_LOG_ENTER();

    auto& object_ids = ctxt.port_map_oids;
    if (object_ids.empty())
    {
        SWSS_LOG_ERROR("No port map OIDs found for port map %s", port_map_id.c_str());
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

    auto& object_statuses = ctxt.port_map_statuses;
    object_statuses.emplace_back();
    sai_object_id_t port_map_oid = port_map_table_[port_map_id];
    port_map_bulker_.remove_entry(&object_statuses.back(), port_map_oid);
    SWSS_LOG_NOTICE("Removing port map %s with OID 0x%" PRIx64, port_map_id.c_str(), port_map_oid);

    return false;
}

bool DashPortMapOrch::removePortMapPost(const std::string &port_map_id, DashPortMapBulkContext &ctxt)
{
    SWSS_LOG_ENTER();

    auto& object_statuses = ctxt.port_map_statuses;
    if (object_statuses.empty())
    {
        SWSS_LOG_ERROR("No port map statuses found for port map %s", port_map_id.c_str());
        return false;
    }

    auto it_status = object_statuses.begin();
    sai_status_t status = *it_status++;
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove port map %s, status: %s", port_map_id.c_str(), sai_serialize_status(status).c_str());
        return false;
    }

    port_map_table_.erase(port_map_id);
    SWSS_LOG_NOTICE("Removed port map %s", port_map_id.c_str());
    return true;
}

void DashPortMapOrch::doTaskPortMapRangeTable(ConsumerBase &consumer)
{
    // SWSS_LOG_ENTER();

    // auto it = consumer.m_toSync.begin();
    // uint32_t result;

    // std::map<std::pair<std::string, std::string>,
    //         DashPortMapBulkContext> toBulk;
    // while (it != consumer.m_toSync.end())
    // {
    //     swss::KeyOpFieldsValuesTuple tuple = it->second;
    //     std::string port_map_id = kfvKey(tuple);
    //     std::string op = kfvOp(tuple);
    //     auto rc = toBulk.emplace(std::piecewise_construct,
    //         std::forward_as_tuple(port_map_id, op),
    //         std::forward_as_tuple());
    //     bool inserted = rc.second;
    //     auto &ctxt = rc.first->second;
    //     result = DASH_RESULT_SUCCESS;
    //     SWSS_LOG_INFO("Processing port map range entry: %s, operation: %s", port_map_id.c_str(), op.c_str());

    //     if (!inserted)
    //     {
    //         ctxt.clear();
    //     }

    //     if (op == SET_COMMAND)
    //     {
    //         // the only info we need is the port map ID which is provided in the key
    //         // no need to parse protobuf message here

    //         if (addPortMap(port_map_id, ctxt))
    //         {
    //             it = consumer.m_toSync.erase(it);
    //             writeResultToDB(dash_port_map_range_result_table_, port_map_id, result);
    //         }
    //         else
    //         {
    //             it++;
    //         }
    //     }
    //     else if (op == DEL_COMMAND)
    //     {
    //         if (removePortMap(port_map_id, ctxt))
    //         {
    //             it = consumer.m_toSync.erase(it);
    //             removeResultFromDB(dash_port_map_range_result_table_, port_map_id);
    //         }
    //         else
    //         {
    //             it++;
    //         }
    //     }
    //     else
    //     {
    //         SWSS_LOG_ERROR("Unknown operation %s", op.c_str());
    //         it = consumer.m_toSync.erase(it);
    //     }

    // }

    // port_map_range_bulker_.flush();

    // auto it_prev = consumer.m_toSync.begin();
    // while (it_prev != it)
    // {
    //     swss::KeyOpFieldsValuesTuple tuple = it_prev->second;
    //     std::string port_map_id = kfvKey(tuple);
    //     std::string op = kfvOp(tuple);
    //     result = DASH_RESULT_SUCCESS;
    //     auto found = toBulk;
    // }
}