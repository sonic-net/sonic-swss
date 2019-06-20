#include <sstream>

#include "crmorch.h"
#include "converter.h"
#include "timer.h"

#define CRM_POLLING_INTERVAL "polling_interval"
#define CRM_COUNTERS_TABLE_KEY "STATS"

#define CRM_POLLING_INTERVAL_DEFAULT (5 * 60)
#define CRM_THRESHOLD_TYPE_DEFAULT CrmThresholdType::CRM_PERCENTAGE
#define CRM_THRESHOLD_LOW_DEFAULT 70
#define CRM_THRESHOLD_HIGH_DEFAULT 85
#define CRM_EXCEEDED_MSG_MAX 10

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t *sai_switch_api;
extern sai_acl_api_t *sai_acl_api;

using namespace std;


const map<CrmResourceType, string> crmResTypeNameMap =
{
    { CrmResourceType::CRM_IPV4_ROUTE, "IPV4_ROUTE" },
    { CrmResourceType::CRM_IPV6_ROUTE, "IPV6_ROUTE" },
    { CrmResourceType::CRM_IPV4_NEXTHOP, "IPV4_NEXTHOP" },
    { CrmResourceType::CRM_IPV6_NEXTHOP, "IPV6_NEXTHOP" },
    { CrmResourceType::CRM_IPV4_NEIGHBOR, "IPV4_NEIGHBOR" },
    { CrmResourceType::CRM_IPV6_NEIGHBOR, "IPV6_Neighbor" },
    { CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER, "NEXTHOP_GROUP_MEMBER" },
    { CrmResourceType::CRM_NEXTHOP_GROUP, "NEXTHOP_GROUP" },
    { CrmResourceType::CRM_ACL_TABLE, "ACL_TABLE" },
    { CrmResourceType::CRM_ACL_GROUP, "ACL_GROUP" },
    { CrmResourceType::CRM_ACL_ENTRY, "ACL_ENTRY" },
    { CrmResourceType::CRM_ACL_COUNTER, "ACL_COUNTER" },
    { CrmResourceType::CRM_FDB_ENTRY, "FDB_ENTRY" }
};

const map<CrmResourceType, uint32_t> crmResSaiAvailAttrMap =
{
    { CrmResourceType::CRM_IPV4_ROUTE, SAI_SWITCH_ATTR_AVAILABLE_IPV4_ROUTE_ENTRY },
    { CrmResourceType::CRM_IPV6_ROUTE, SAI_SWITCH_ATTR_AVAILABLE_IPV6_ROUTE_ENTRY },
    { CrmResourceType::CRM_IPV4_NEXTHOP, SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEXTHOP_ENTRY },
    { CrmResourceType::CRM_IPV6_NEXTHOP, SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEXTHOP_ENTRY },
    { CrmResourceType::CRM_IPV4_NEIGHBOR, SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEIGHBOR_ENTRY },
    { CrmResourceType::CRM_IPV6_NEIGHBOR, SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEIGHBOR_ENTRY },
    { CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER, SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_MEMBER_ENTRY },
    { CrmResourceType::CRM_NEXTHOP_GROUP, SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_ENTRY },
    { CrmResourceType::CRM_ACL_TABLE, SAI_SWITCH_ATTR_AVAILABLE_ACL_TABLE },
    { CrmResourceType::CRM_ACL_GROUP, SAI_SWITCH_ATTR_AVAILABLE_ACL_TABLE_GROUP },
    { CrmResourceType::CRM_ACL_ENTRY, SAI_ACL_TABLE_ATTR_AVAILABLE_ACL_ENTRY },
    { CrmResourceType::CRM_ACL_COUNTER, SAI_ACL_TABLE_ATTR_AVAILABLE_ACL_COUNTER },
    { CrmResourceType::CRM_FDB_ENTRY, SAI_SWITCH_ATTR_AVAILABLE_FDB_ENTRY }
};

const map<string, CrmResourceType> crmThreshTypeResMap =
{
    { "ipv4_route_threshold_type", CrmResourceType::CRM_IPV4_ROUTE },
    { "ipv6_route_threshold_type", CrmResourceType::CRM_IPV6_ROUTE },
    { "ipv4_nexthop_threshold_type", CrmResourceType::CRM_IPV4_NEXTHOP },
    { "ipv6_nexthop_threshold_type", CrmResourceType::CRM_IPV6_NEXTHOP },
    { "ipv4_neighbor_threshold_type", CrmResourceType::CRM_IPV4_NEIGHBOR },
    { "ipv6_neighbor_threshold_type", CrmResourceType::CRM_IPV6_NEIGHBOR },
    { "nexthop_group_member_threshold_type", CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER },
    { "nexthop_group_threshold_type", CrmResourceType::CRM_NEXTHOP_GROUP },
    { "acl_table_threshold_type", CrmResourceType::CRM_ACL_TABLE },
    { "acl_group_threshold_type", CrmResourceType::CRM_ACL_GROUP },
    { "acl_entry_threshold_type", CrmResourceType::CRM_ACL_ENTRY },
    { "acl_counter_threshold_type", CrmResourceType::CRM_ACL_COUNTER },
    { "fdb_entry_threshold_type", CrmResourceType::CRM_FDB_ENTRY }
};

const map<string, CrmResourceType> crmThreshLowResMap =
{
    {"ipv4_route_low_threshold", CrmResourceType::CRM_IPV4_ROUTE },
    {"ipv6_route_low_threshold", CrmResourceType::CRM_IPV6_ROUTE },
    {"ipv4_nexthop_low_threshold", CrmResourceType::CRM_IPV4_NEXTHOP },
    {"ipv6_nexthop_low_threshold", CrmResourceType::CRM_IPV6_NEXTHOP },
    {"ipv4_neighbor_low_threshold", CrmResourceType::CRM_IPV4_NEIGHBOR },
    {"ipv6_neighbor_low_threshold", CrmResourceType::CRM_IPV6_NEIGHBOR },
    {"nexthop_group_member_low_threshold", CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER },
    {"nexthop_group_low_threshold", CrmResourceType::CRM_NEXTHOP_GROUP },
    {"acl_table_low_threshold", CrmResourceType::CRM_ACL_TABLE },
    {"acl_group_low_threshold", CrmResourceType::CRM_ACL_GROUP },
    {"acl_entry_low_threshold", CrmResourceType::CRM_ACL_ENTRY },
    {"acl_counter_low_threshold", CrmResourceType::CRM_ACL_COUNTER },
    {"fdb_entry_low_threshold", CrmResourceType::CRM_FDB_ENTRY },
};

const map<string, CrmResourceType> crmThreshHighResMap =
{
    {"ipv4_route_high_threshold", CrmResourceType::CRM_IPV4_ROUTE },
    {"ipv6_route_high_threshold", CrmResourceType::CRM_IPV6_ROUTE },
    {"ipv4_nexthop_high_threshold", CrmResourceType::CRM_IPV4_NEXTHOP },
    {"ipv6_nexthop_high_threshold", CrmResourceType::CRM_IPV6_NEXTHOP },
    {"ipv4_neighbor_high_threshold", CrmResourceType::CRM_IPV4_NEIGHBOR },
    {"ipv6_neighbor_high_threshold", CrmResourceType::CRM_IPV6_NEIGHBOR },
    {"nexthop_group_member_high_threshold", CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER },
    {"nexthop_group_high_threshold", CrmResourceType::CRM_NEXTHOP_GROUP },
    {"acl_table_high_threshold", CrmResourceType::CRM_ACL_TABLE },
    {"acl_group_high_threshold", CrmResourceType::CRM_ACL_GROUP },
    {"acl_entry_high_threshold", CrmResourceType::CRM_ACL_ENTRY },
    {"acl_counter_high_threshold", CrmResourceType::CRM_ACL_COUNTER },
    {"fdb_entry_high_threshold", CrmResourceType::CRM_FDB_ENTRY }
};

const map<string, CrmThresholdType> crmThreshTypeMap =
{
    { "percentage", CrmThresholdType::CRM_PERCENTAGE },
    { "used", CrmThresholdType::CRM_USED },
    { "free", CrmThresholdType::CRM_FREE }
};

const map<string, CrmResourceType> crmAvailCntsTableMap =
{
    { "crm_stats_ipv4_route_available", CrmResourceType::CRM_IPV4_ROUTE },
    { "crm_stats_ipv6_route_available", CrmResourceType::CRM_IPV6_ROUTE },
    { "crm_stats_ipv4_nexthop_available", CrmResourceType::CRM_IPV4_NEXTHOP },
    { "crm_stats_ipv6_nexthop_available", CrmResourceType::CRM_IPV6_NEXTHOP },
    { "crm_stats_ipv4_neighbor_available", CrmResourceType::CRM_IPV4_NEIGHBOR },
    { "crm_stats_ipv6_neighbor_available", CrmResourceType::CRM_IPV6_NEIGHBOR },
    { "crm_stats_nexthop_group_member_available", CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER },
    { "crm_stats_nexthop_group_available", CrmResourceType::CRM_NEXTHOP_GROUP },
    { "crm_stats_acl_table_available", CrmResourceType::CRM_ACL_TABLE },
    { "crm_stats_acl_group_available", CrmResourceType::CRM_ACL_GROUP },
    { "crm_stats_acl_entry_available", CrmResourceType::CRM_ACL_ENTRY },
    { "crm_stats_acl_counter_available", CrmResourceType::CRM_ACL_COUNTER },
    { "crm_stats_fdb_entry_available", CrmResourceType::CRM_FDB_ENTRY }
};

const map<string, CrmResourceType> crmUsedCntsTableMap =
{
    { "crm_stats_ipv4_route_used", CrmResourceType::CRM_IPV4_ROUTE },
    { "crm_stats_ipv6_route_used", CrmResourceType::CRM_IPV6_ROUTE },
    { "crm_stats_ipv4_nexthop_used", CrmResourceType::CRM_IPV4_NEXTHOP },
    { "crm_stats_ipv6_nexthop_used", CrmResourceType::CRM_IPV6_NEXTHOP },
    { "crm_stats_ipv4_neighbor_used", CrmResourceType::CRM_IPV4_NEIGHBOR },
    { "crm_stats_ipv6_neighbor_used", CrmResourceType::CRM_IPV6_NEIGHBOR },
    { "crm_stats_nexthop_group_member_used", CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER },
    { "crm_stats_nexthop_group_used", CrmResourceType::CRM_NEXTHOP_GROUP },
    { "crm_stats_acl_table_used", CrmResourceType::CRM_ACL_TABLE },
    { "crm_stats_acl_group_used", CrmResourceType::CRM_ACL_GROUP },
    { "crm_stats_acl_entry_used", CrmResourceType::CRM_ACL_ENTRY },
    { "crm_stats_acl_counter_used", CrmResourceType::CRM_ACL_COUNTER },
    { "crm_stats_fdb_entry_used", CrmResourceType::CRM_FDB_ENTRY }
};

CrmOrch::CrmOrch(DBConnector *db, string tableName):
    Orch(db, tableName),
    m_countersDb(new DBConnector(COUNTERS_DB, DBConnector::DEFAULT_UNIXSOCKET, 0)),
    m_countersCrmTable(new Table(m_countersDb.get(), COUNTERS_CRM_TABLE)),
    m_timer(new SelectableTimer(timespec { .tv_sec = CRM_POLLING_INTERVAL_DEFAULT, .tv_nsec = 0 }))
{
    SWSS_LOG_ENTER();

    m_pollingInterval = chrono::seconds(CRM_POLLING_INTERVAL_DEFAULT);

    for (const auto &res : crmResTypeNameMap)
    {
        m_resourcesMap.emplace(res.first, CrmResourceEntry(res.second, CRM_THRESHOLD_TYPE_DEFAULT, CRM_THRESHOLD_LOW_DEFAULT, CRM_THRESHOLD_HIGH_DEFAULT));
    }

    auto executor = new ExecutableTimer(m_timer.get(), this);
    Orch::addExecutor("CRM_COUNTERS_POLL", executor);
    m_timer->start();
}

CrmOrch::CrmResourceEntry::CrmResourceEntry(string name, CrmThresholdType thresholdType, uint32_t lowThreshold, uint32_t highThreshold):
    name(name),
    thresholdType(thresholdType),
    lowThreshold(lowThreshold),
    highThreshold(highThreshold)
{
    if ((thresholdType == CrmThresholdType::CRM_PERCENTAGE) && ((lowThreshold > 100) || (highThreshold > 100)))
    {
        throw runtime_error("CRM percentage threshold value must be <= 100%%");
    }

    if (!(lowThreshold < highThreshold))
    {
        throw runtime_error("CRM low threshold must be less then high threshold");
    }

}

void CrmOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    if (table_name != CFG_CRM_TABLE_NAME)
    {
        SWSS_LOG_ERROR("Invalid table %s", table_name.c_str());
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            handleSetCommand(key, kfvFieldsValues(t));
        }
        else if (op == DEL_COMMAND)
        {
            SWSS_LOG_ERROR("Unsupported operation type %s\n", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
        }

        consumer.m_toSync.erase(it++);
    }
}

void CrmOrch::handleSetCommand(const string& key, const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();

    for (auto i : data)
    {
        const auto &field = fvField(i);
        const auto &value = fvValue(i);

        try
        {
            if (field == CRM_POLLING_INTERVAL)
            {
                m_pollingInterval = chrono::seconds(to_uint<uint32_t>(value));
                auto interv = timespec { .tv_sec = m_pollingInterval.count(), .tv_nsec = 0 };
                m_timer->setInterval(interv);
                m_timer->reset();
            }
            else if (crmThreshTypeResMap.find(field) != crmThreshTypeResMap.end())
            {
                auto resourceType = crmThreshTypeResMap.at(field);
                auto thresholdType = crmThreshTypeMap.at(value);

                m_resourcesMap.at(resourceType).thresholdType = thresholdType;
            }
            else if (crmThreshLowResMap.find(field) != crmThreshLowResMap.end())
            {
                auto resourceType = crmThreshLowResMap.at(field);
                auto thresholdValue = to_uint<uint32_t>(value);

                m_resourcesMap.at(resourceType).lowThreshold = thresholdValue;
            }
            else if (crmThreshHighResMap.find(field) != crmThreshHighResMap.end())
            {
                auto resourceType = crmThreshHighResMap.at(field);
                auto thresholdValue = to_uint<uint32_t>(value);

                m_resourcesMap.at(resourceType).highThreshold = thresholdValue;
            }
            else
            {
                SWSS_LOG_ERROR("Failed to parse CRM %s configuration. Unknown attribute %s.\n", key.c_str(), field.c_str());
                return;
            }
        }
        catch (const exception& e)
        {
            SWSS_LOG_ERROR("Failed to parse CRM %s attribute %s error: %s.", key.c_str(), field.c_str(), e.what());
            return;
        }
        catch (...)
        {
            SWSS_LOG_ERROR("Failed to parse CRM %s attribute %s. Unknown error has been occurred", key.c_str(), field.c_str());
            return;
        }
    }
}

void CrmOrch::incCrmResUsedCounter(CrmResourceType resource)
{
    SWSS_LOG_ENTER();

    try
    {
        m_resourcesMap.at(resource).countersMap[CRM_COUNTERS_TABLE_KEY].usedCounter++;
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Failed to increment \"used\" counter for the %s CRM resource.", crmResTypeNameMap.at(resource).c_str());
        return;
    }
}

void CrmOrch::decCrmResUsedCounter(CrmResourceType resource)
{
    SWSS_LOG_ENTER();

    try
    {
        m_resourcesMap.at(resource).countersMap[CRM_COUNTERS_TABLE_KEY].usedCounter--;
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Failed to decrement \"used\" counter for the %s CRM resource.", crmResTypeNameMap.at(resource).c_str());
        return;
    }
}

void CrmOrch::incCrmAclUsedCounter(CrmResourceType resource, sai_acl_stage_t stage, sai_acl_bind_point_type_t point)
{
    SWSS_LOG_ENTER();

    try
    {
        m_resourcesMap.at(resource).countersMap[getCrmAclKey(stage, point)].usedCounter++;
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Failed to increment \"used\" counter for the %s CRM resource.", crmResTypeNameMap.at(resource).c_str());
        return;
    }
}

void CrmOrch::decCrmAclUsedCounter(CrmResourceType resource, sai_acl_stage_t stage, sai_acl_bind_point_type_t point, sai_object_id_t oid)
{
    SWSS_LOG_ENTER();

    try
    {
        m_resourcesMap.at(resource).countersMap[getCrmAclKey(stage, point)].usedCounter--;

        // Remove ACL table related counters 
        if (resource == CrmResourceType::CRM_ACL_TABLE)
        {
            auto & cntMap = m_resourcesMap.at(CrmResourceType::CRM_ACL_TABLE).countersMap;
            for (auto it = cntMap.begin(); it != cntMap.end();)
            {
                if (it->second.id == oid)
                {
                    it = cntMap.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Failed to decrement \"used\" counter for the %s CRM resource.", crmResTypeNameMap.at(resource).c_str());
        return;
    }
}

void CrmOrch::incCrmAclTableUsedCounter(CrmResourceType resource, sai_object_id_t tableId)
{
    SWSS_LOG_ENTER();

    try
    {
        m_resourcesMap.at(resource).countersMap[getCrmAclTableKey(tableId)].usedCounter++;
        m_resourcesMap.at(resource).countersMap[getCrmAclTableKey(tableId)].id = tableId;
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Failed to increment \"used\" counter for the %s CRM resource (tableId:%lx).", crmResTypeNameMap.at(resource).c_str(), tableId);
        return;
    }
}

void CrmOrch::decCrmAclTableUsedCounter(CrmResourceType resource, sai_object_id_t tableId)
{
    SWSS_LOG_ENTER();

    try
    {
        m_resourcesMap.at(resource).countersMap[getCrmAclTableKey(tableId)].usedCounter--;
    }
    catch (...)
    {
        SWSS_LOG_ERROR("Failed to decrement \"used\" counter for the %s CRM resource (tableId:%lx).", crmResTypeNameMap.at(resource).c_str(), tableId);
        return;
    }
}

void CrmOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    getResAvailableCounters();
    updateCrmCountersTable();
    checkCrmThresholds();
}

void CrmOrch::getResAvailableCounters()
{
    SWSS_LOG_ENTER();

    for (auto &res : m_resourcesMap)
    {
        sai_attribute_t attr;
        attr.id = crmResSaiAvailAttrMap.at(res.first);

        switch (attr.id)
        {
            case SAI_SWITCH_ATTR_AVAILABLE_IPV4_ROUTE_ENTRY:
            case SAI_SWITCH_ATTR_AVAILABLE_IPV6_ROUTE_ENTRY:
            case SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEXTHOP_ENTRY:
            case SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEXTHOP_ENTRY:
            case SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEIGHBOR_ENTRY:
            case SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEIGHBOR_ENTRY:
            case SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_MEMBER_ENTRY:
            case SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_ENTRY:
            case SAI_SWITCH_ATTR_AVAILABLE_FDB_ENTRY:
            {
                sai_status_t status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to get switch attribute %u , rv:%d", attr.id, status);
                    break;
                }

                res.second.countersMap[CRM_COUNTERS_TABLE_KEY].availableCounter = attr.value.u32;

                break;
            }

            case SAI_SWITCH_ATTR_AVAILABLE_ACL_TABLE:
            case SAI_SWITCH_ATTR_AVAILABLE_ACL_TABLE_GROUP:
            {
                attr.value.aclresource.count = 0;
                attr.value.aclresource.list = NULL;

                sai_status_t status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
                if ((status != SAI_STATUS_SUCCESS) && (status != SAI_STATUS_BUFFER_OVERFLOW))
                {
                    SWSS_LOG_ERROR("Failed to get switch attribute %u , rv:%d", attr.id, status);
                    break;
                }

                vector<sai_acl_resource_t> resources(attr.value.aclresource.count);
                attr.value.aclresource.list = resources.data();

                status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to get switch attribute %u , rv:%d", attr.id, status);
                    break;
                }

                for (uint32_t i = 0; i < attr.value.aclresource.count; i++)
                {
                    string key = getCrmAclKey(attr.value.aclresource.list[i].stage, attr.value.aclresource.list[i].bind_point);
                    res.second.countersMap[key].availableCounter = attr.value.aclresource.list[i].avail_num;
                }

                break;
            }

            case SAI_ACL_TABLE_ATTR_AVAILABLE_ACL_ENTRY:
            case SAI_ACL_TABLE_ATTR_AVAILABLE_ACL_COUNTER:
            {
                for (auto &cnt : res.second.countersMap)
                {
                    sai_status_t status = sai_acl_api->get_acl_table_attribute(cnt.second.id, 1, &attr);
                    if (status != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("Failed to get ACL table attribute %u , rv:%d", attr.id, status);
                        break;
                    }

                    cnt.second.availableCounter = attr.value.u32;
                }

                break;
            }

            default:
                SWSS_LOG_ERROR("Failed to get CRM attribute %u. Unknown attribute.\n", attr.id);
                return;
        }
    }
}

void CrmOrch::updateCrmCountersTable()
{
    SWSS_LOG_ENTER();

    // Update CRM used counters in COUNTERS_DB
    for (const auto &i : crmUsedCntsTableMap)
    {
        for (const auto &cnt : m_resourcesMap.at(i.second).countersMap)
        {
            FieldValueTuple attr(i.first, to_string(cnt.second.usedCounter));
            vector<FieldValueTuple> attrs = { attr };
            m_countersCrmTable->set(cnt.first, attrs);
        }
    }

    // Update CRM available counters in COUNTERS_DB
    for (const auto &i : crmAvailCntsTableMap)
    {
        for (const auto &cnt : m_resourcesMap.at(i.second).countersMap)
        {
            FieldValueTuple attr(i.first, to_string(cnt.second.availableCounter));
            vector<FieldValueTuple> attrs = { attr };
            m_countersCrmTable->set(cnt.first, attrs);
        }
    }
}

void CrmOrch::checkCrmThresholds()
{
    SWSS_LOG_ENTER();

    for (auto &i : m_resourcesMap)
    {
        auto &res = i.second;

        for (const auto &j : i.second.countersMap)
        {
            auto &cnt = j.second;
            uint64_t utilization = 0;
            uint32_t percentageUtil = 0;
            string threshType = "";

            if (cnt.usedCounter != 0)
            {
                percentageUtil = (cnt.usedCounter * 100) / (cnt.usedCounter + cnt.availableCounter);
            }

            switch (res.thresholdType)
            {
                case CrmThresholdType::CRM_PERCENTAGE:
                    utilization = percentageUtil;
                    threshType = "TH_PERCENTAGE";
                    break;
                case CrmThresholdType::CRM_USED:
                    utilization = cnt.usedCounter;
                    threshType = "TH_USED";
                    break;
                case CrmThresholdType::CRM_FREE:
                    utilization = cnt.availableCounter;
                    threshType = "TH_FREE";
                    break;
                default:
                    throw runtime_error("Unknown threshold type for CRM resource");
            }

            if ((utilization >= res.highThreshold) && (res.exceededLogCounter < CRM_EXCEEDED_MSG_MAX))
            {
                SWSS_LOG_WARN("%s THRESHOLD_EXCEEDED for %s %u%% Used count %u free count %u",
                              res.name.c_str(), threshType.c_str(), percentageUtil, cnt.usedCounter, cnt.availableCounter);

                res.exceededLogCounter++;
            }
            else if ((utilization <= res.lowThreshold) && (res.exceededLogCounter > 0))
            {
                SWSS_LOG_WARN("%s THRESHOLD_CLEAR for %s %u%% Used count %u free count %u",
                              res.name.c_str(), threshType.c_str(), percentageUtil, cnt.usedCounter, cnt.availableCounter);

                res.exceededLogCounter = 0;
            }
        } // end of counters loop
    } // end of resources loop
}


string CrmOrch::getCrmAclKey(sai_acl_stage_t stage, sai_acl_bind_point_type_t bindPoint)
{
    string key = "ACL_STATS";

    switch(stage)
    {
        case SAI_ACL_STAGE_INGRESS:
            key += ":INGRESS";
            break;
        case SAI_ACL_STAGE_EGRESS:
            key += ":EGRESS";
            break;
        default:
            return "";
    }

    switch(bindPoint)
    {
        case SAI_ACL_BIND_POINT_TYPE_PORT:
            key += ":PORT";
            break;
        case SAI_ACL_BIND_POINT_TYPE_LAG:
            key += ":LAG";
            break;
        case SAI_ACL_BIND_POINT_TYPE_VLAN:
            key += ":VLAN";
            break;
        case SAI_ACL_BIND_POINT_TYPE_ROUTER_INTFERFACE:
            key += ":RIF";
            break;
        case SAI_ACL_BIND_POINT_TYPE_SWITCH:
            key += ":SWITCH";
            break;
        default:
            return "";
    }

    return key;
}

string CrmOrch::getCrmAclTableKey(sai_object_id_t id)
{
    std::stringstream ss;
    ss << "ACL_TABLE_STATS:" << "0x" << std::hex << id;
    return ss.str();
}
