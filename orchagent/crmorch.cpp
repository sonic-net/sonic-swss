#include <sstream>
#include <inttypes.h>

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
#define CRM_ACL_RESOURCE_COUNT 256

extern sai_object_id_t gSwitchId;
extern sai_switch_api_t *sai_switch_api;
extern sai_acl_api_t *sai_acl_api;

using namespace std;
using namespace swss;


const map<CrmResourceType, string> crmResTypeNameMap =
{
    { CrmResourceType::CRM_IPV4_ROUTE, "IPV4_ROUTE" },
    { CrmResourceType::CRM_IPV6_ROUTE, "IPV6_ROUTE" },
    { CrmResourceType::CRM_IPV4_NEXTHOP, "IPV4_NEXTHOP" },
    { CrmResourceType::CRM_IPV6_NEXTHOP, "IPV6_NEXTHOP" },
    { CrmResourceType::CRM_IPV4_NEIGHBOR, "IPV4_NEIGHBOR" },
    { CrmResourceType::CRM_IPV6_NEIGHBOR, "IPV6_NEIGHBOR" },
    { CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER, "NEXTHOP_GROUP_MEMBER" },
    { CrmResourceType::CRM_NEXTHOP_GROUP, "NEXTHOP_GROUP" },
    { CrmResourceType::CRM_ACL_TABLE, "ACL_TABLE" },
    { CrmResourceType::CRM_ACL_GROUP, "ACL_GROUP" },
    { CrmResourceType::CRM_ACL_ENTRY, "ACL_ENTRY" },
    { CrmResourceType::CRM_ACL_COUNTER, "ACL_COUNTER" },
    { CrmResourceType::CRM_FDB_ENTRY, "FDB_ENTRY" },
    { CrmResourceType::CRM_IPMC_ENTRY, "IPMC_ENTRY" },
    { CrmResourceType::CRM_SNAT_ENTRY, "SNAT_ENTRY" },
    { CrmResourceType::CRM_DNAT_ENTRY, "DNAT_ENTRY" },
    { CrmResourceType::CRM_MPLS_INSEG, "MPLS_INSEG" },
    { CrmResourceType::CRM_MPLS_NEXTHOP, "MPLS_NEXTHOP" },
    { CrmResourceType::CRM_SRV6_MY_SID_ENTRY, "SRV6_MY_SID_ENTRY" },
    { CrmResourceType::CRM_SRV6_NEXTHOP, "SRV6_NEXTHOP" },
    { CrmResourceType::CRM_NEXTHOP_GROUP_MAP, "NEXTHOP_GROUP_MAP" },
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
    { CrmResourceType::CRM_FDB_ENTRY, SAI_SWITCH_ATTR_AVAILABLE_FDB_ENTRY },
    { CrmResourceType::CRM_IPMC_ENTRY, SAI_SWITCH_ATTR_AVAILABLE_IPMC_ENTRY},
    { CrmResourceType::CRM_SNAT_ENTRY, SAI_SWITCH_ATTR_AVAILABLE_SNAT_ENTRY },
    { CrmResourceType::CRM_DNAT_ENTRY, SAI_SWITCH_ATTR_AVAILABLE_DNAT_ENTRY },
};

const map<CrmResourceType, sai_object_type_t> crmResSaiObjAttrMap =
{
    { CrmResourceType::CRM_IPV4_ROUTE, SAI_OBJECT_TYPE_ROUTE_ENTRY },
    { CrmResourceType::CRM_IPV6_ROUTE, SAI_OBJECT_TYPE_ROUTE_ENTRY },
    { CrmResourceType::CRM_IPV4_NEXTHOP, SAI_OBJECT_TYPE_NULL },
    { CrmResourceType::CRM_IPV6_NEXTHOP, SAI_OBJECT_TYPE_NULL },
    { CrmResourceType::CRM_IPV4_NEIGHBOR, SAI_OBJECT_TYPE_NEIGHBOR_ENTRY },
    { CrmResourceType::CRM_IPV6_NEIGHBOR, SAI_OBJECT_TYPE_NEIGHBOR_ENTRY },
    { CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER, SAI_OBJECT_TYPE_NULL },
    { CrmResourceType::CRM_NEXTHOP_GROUP, SAI_OBJECT_TYPE_NEXT_HOP_GROUP },
    { CrmResourceType::CRM_ACL_TABLE, SAI_OBJECT_TYPE_NULL },
    { CrmResourceType::CRM_ACL_GROUP, SAI_OBJECT_TYPE_NULL },
    { CrmResourceType::CRM_ACL_ENTRY, SAI_OBJECT_TYPE_NULL },
    { CrmResourceType::CRM_ACL_COUNTER, SAI_OBJECT_TYPE_NULL },
    { CrmResourceType::CRM_FDB_ENTRY, SAI_OBJECT_TYPE_FDB_ENTRY },
    { CrmResourceType::CRM_IPMC_ENTRY, SAI_OBJECT_TYPE_NULL},
    { CrmResourceType::CRM_SNAT_ENTRY, SAI_OBJECT_TYPE_NULL },
    { CrmResourceType::CRM_DNAT_ENTRY, SAI_OBJECT_TYPE_NULL },
    { CrmResourceType::CRM_MPLS_INSEG, SAI_OBJECT_TYPE_INSEG_ENTRY },
    { CrmResourceType::CRM_MPLS_NEXTHOP, SAI_OBJECT_TYPE_NEXT_HOP },
    { CrmResourceType::CRM_SRV6_MY_SID_ENTRY, SAI_OBJECT_TYPE_MY_SID_ENTRY },
    { CrmResourceType::CRM_SRV6_NEXTHOP, SAI_OBJECT_TYPE_NEXT_HOP },
    { CrmResourceType::CRM_NEXTHOP_GROUP_MAP, SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MAP },
};

const map<CrmResourceType, sai_attr_id_t> crmResAddrFamilyAttrMap =
{
    { CrmResourceType::CRM_IPV4_ROUTE, SAI_ROUTE_ENTRY_ATTR_IP_ADDR_FAMILY },
    { CrmResourceType::CRM_IPV6_ROUTE, SAI_ROUTE_ENTRY_ATTR_IP_ADDR_FAMILY },
    { CrmResourceType::CRM_IPV4_NEIGHBOR, SAI_NEIGHBOR_ENTRY_ATTR_IP_ADDR_FAMILY },
    { CrmResourceType::CRM_IPV6_NEIGHBOR, SAI_NEIGHBOR_ENTRY_ATTR_IP_ADDR_FAMILY },
};

const map<CrmResourceType, sai_ip_addr_family_t> crmResAddrFamilyValMap =
{
    { CrmResourceType::CRM_IPV4_ROUTE, SAI_IP_ADDR_FAMILY_IPV4 },
    { CrmResourceType::CRM_IPV6_ROUTE, SAI_IP_ADDR_FAMILY_IPV6 },
    { CrmResourceType::CRM_IPV4_NEIGHBOR, SAI_IP_ADDR_FAMILY_IPV4 },
    { CrmResourceType::CRM_IPV6_NEIGHBOR, SAI_IP_ADDR_FAMILY_IPV6 },
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
    { "fdb_entry_threshold_type", CrmResourceType::CRM_FDB_ENTRY },
    { "ipmc_entry_threshold_type", CrmResourceType::CRM_IPMC_ENTRY },
    { "snat_entry_threshold_type", CrmResourceType::CRM_SNAT_ENTRY },
    { "dnat_entry_threshold_type", CrmResourceType::CRM_DNAT_ENTRY },
    { "mpls_inseg_threshold_type", CrmResourceType::CRM_MPLS_INSEG },
    { "mpls_nexthop_threshold_type", CrmResourceType::CRM_MPLS_NEXTHOP },
    { "srv6_my_sid_entry_threshold_type", CrmResourceType::CRM_SRV6_MY_SID_ENTRY },
    { "srv6_nexthop_threshold_type", CrmResourceType::CRM_SRV6_NEXTHOP },
    { "nexthop_group_map_threshold_type", CrmResourceType::CRM_NEXTHOP_GROUP_MAP },
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
    {"ipmc_entry_low_threshold", CrmResourceType::CRM_IPMC_ENTRY },
    {"snat_entry_low_threshold", CrmResourceType::CRM_SNAT_ENTRY },
    {"dnat_entry_low_threshold", CrmResourceType::CRM_DNAT_ENTRY },
    {"mpls_inseg_low_threshold", CrmResourceType::CRM_MPLS_INSEG },
    {"mpls_nexthop_low_threshold", CrmResourceType::CRM_MPLS_NEXTHOP },
    {"srv6_my_sid_entry_low_threshold", CrmResourceType::CRM_SRV6_MY_SID_ENTRY },
    {"srv6_nexthop_low_threshold", CrmResourceType::CRM_SRV6_NEXTHOP },
    {"nexthop_group_map_low_threshold", CrmResourceType::CRM_NEXTHOP_GROUP_MAP },
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
    {"fdb_entry_high_threshold", CrmResourceType::CRM_FDB_ENTRY },
    {"ipmc_entry_high_threshold", CrmResourceType::CRM_IPMC_ENTRY },
    {"snat_entry_high_threshold", CrmResourceType::CRM_SNAT_ENTRY },
    {"dnat_entry_high_threshold", CrmResourceType::CRM_DNAT_ENTRY },
    {"mpls_inseg_high_threshold", CrmResourceType::CRM_MPLS_INSEG },
    {"mpls_nexthop_high_threshold", CrmResourceType::CRM_MPLS_NEXTHOP },
    {"srv6_my_sid_entry_high_threshold", CrmResourceType::CRM_SRV6_MY_SID_ENTRY },
    {"srv6_nexthop_high_threshold", CrmResourceType::CRM_SRV6_NEXTHOP },
    {"nexthop_group_map_high_threshold", CrmResourceType::CRM_NEXTHOP_GROUP_MAP },
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
    { "crm_stats_fdb_entry_available", CrmResourceType::CRM_FDB_ENTRY },
    { "crm_stats_ipmc_entry_available", CrmResourceType::CRM_IPMC_ENTRY },
    { "crm_stats_snat_entry_available", CrmResourceType::CRM_SNAT_ENTRY },
    { "crm_stats_dnat_entry_available", CrmResourceType::CRM_DNAT_ENTRY },
    { "crm_stats_mpls_inseg_available", CrmResourceType::CRM_MPLS_INSEG },
    { "crm_stats_mpls_nexthop_available", CrmResourceType::CRM_MPLS_NEXTHOP },
    { "crm_stats_srv6_my_sid_entry_available", CrmResourceType::CRM_SRV6_MY_SID_ENTRY },
    { "crm_stats_srv6_nexthop_available", CrmResourceType::CRM_SRV6_NEXTHOP },
    { "crm_stats_nexthop_group_map_available", CrmResourceType::CRM_NEXTHOP_GROUP_MAP },
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
    { "crm_stats_fdb_entry_used", CrmResourceType::CRM_FDB_ENTRY },
    { "crm_stats_ipmc_entry_used", CrmResourceType::CRM_IPMC_ENTRY },
    { "crm_stats_snat_entry_used", CrmResourceType::CRM_SNAT_ENTRY },
    { "crm_stats_dnat_entry_used", CrmResourceType::CRM_DNAT_ENTRY },
    { "crm_stats_mpls_inseg_used", CrmResourceType::CRM_MPLS_INSEG },
    { "crm_stats_mpls_nexthop_used", CrmResourceType::CRM_MPLS_NEXTHOP },
    { "crm_stats_srv6_my_sid_entry_used", CrmResourceType::CRM_SRV6_MY_SID_ENTRY },
    { "crm_stats_srv6_nexthop_used", CrmResourceType::CRM_SRV6_NEXTHOP },
    { "crm_stats_nexthop_group_map_used", CrmResourceType::CRM_NEXTHOP_GROUP_MAP },
};

CrmOrch::CrmOrch(DBConnector *db, string tableName):
    Orch(db, tableName),
    m_countersDb(new DBConnector("COUNTERS_DB", 0)),
    m_countersCrmTable(new Table(m_countersDb.get(), COUNTERS_CRM_TABLE)),
    m_timer(new SelectableTimer(timespec { .tv_sec = CRM_POLLING_INTERVAL_DEFAULT, .tv_nsec = 0 }))
{
    SWSS_LOG_ENTER();

    m_pollingInterval = chrono::seconds(CRM_POLLING_INTERVAL_DEFAULT);

    for (const auto &res : crmResTypeNameMap)
    {
        m_resourcesMap.emplace(res.first, CrmResourceEntry(res.second, CRM_THRESHOLD_TYPE_DEFAULT, CRM_THRESHOLD_LOW_DEFAULT, CRM_THRESHOLD_HIGH_DEFAULT));
    }

    // The CRM stats needs to be populated again
    m_countersCrmTable->del(CRM_COUNTERS_TABLE_KEY);

    // Note: ExecutableTimer will hold m_timer pointer and release the object later
    auto executor = new ExecutableTimer(m_timer, this, "CRM_COUNTERS_POLL");
    Orch::addExecutor(executor);
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
                auto interv = timespec { .tv_sec = (time_t)m_pollingInterval.count(), .tv_nsec = 0 };
                m_timer->setInterval(interv);
                m_timer->reset();
            }
            else if (crmThreshTypeResMap.find(field) != crmThreshTypeResMap.end())
            {
                auto resourceType = crmThreshTypeResMap.at(field);
                auto thresholdType = crmThreshTypeMap.at(value);

                if (m_resourcesMap.at(resourceType).thresholdType != thresholdType)
                {
                    m_resourcesMap.at(resourceType).thresholdType = thresholdType;
                    m_resourcesMap.at(resourceType).exceededLogCounter = 0;
                }
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

        // remove acl_entry and acl_counter in this acl table
        if (resource == CrmResourceType::CRM_ACL_TABLE)
        {
            for (auto &resourcesMap : m_resourcesMap)
            {
                if ((resourcesMap.first == (CrmResourceType::CRM_ACL_ENTRY))
                    || (resourcesMap.first == (CrmResourceType::CRM_ACL_COUNTER)))
                {
                    auto &cntMap = resourcesMap.second.countersMap;
                    for (auto it = cntMap.begin(); it != cntMap.end(); ++it)
                    {
                        if (it->second.id == oid)
                        {
                            cntMap.erase(it);
                            break;
                        }
                    }
                }
            }

            // remove ACL_TABLE_STATS in crm database
            m_countersCrmTable->del(getCrmAclTableKey(oid));
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
        SWSS_LOG_ERROR("Failed to increment \"used\" counter for the %s CRM resource (tableId:%" PRIx64 ").", crmResTypeNameMap.at(resource).c_str(), tableId);
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
        SWSS_LOG_ERROR("Failed to decrement \"used\" counter for the %s CRM resource (tableId:%" PRIx64 ").", crmResTypeNameMap.at(resource).c_str(), tableId);
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

bool CrmOrch::getResAvailability(CrmResourceType type, CrmResourceEntry &res)
{
    sai_attribute_t attr;
    uint64_t availCount = 0;
    sai_status_t status = SAI_STATUS_SUCCESS;

    sai_object_type_t objType = crmResSaiObjAttrMap.at(type);

    if (objType != SAI_OBJECT_TYPE_NULL)
    {
        uint32_t attrCount = 0;

        if ((type == CrmResourceType::CRM_IPV4_ROUTE) || (type == CrmResourceType::CRM_IPV6_ROUTE) ||
            (type == CrmResourceType::CRM_IPV4_NEIGHBOR) || (type == CrmResourceType::CRM_IPV6_NEIGHBOR))
        {
            attr.id = crmResAddrFamilyAttrMap.at(type);
            attr.value.s32 = crmResAddrFamilyValMap.at(type);
            attrCount = 1;
        }
        else if (type == CrmResourceType::CRM_MPLS_NEXTHOP)
        {
            attr.id = SAI_NEXT_HOP_ATTR_TYPE;
            attr.value.s32 = SAI_NEXT_HOP_TYPE_MPLS;
            attrCount = 1;
        }
        else if (type == CrmResourceType::CRM_SRV6_NEXTHOP)
        {
            attr.id = SAI_NEXT_HOP_ATTR_TYPE;
            attr.value.s32 = SAI_NEXT_HOP_TYPE_SRV6_SIDLIST;
            attrCount = 1;
        }

        status = sai_object_type_get_availability(gSwitchId, objType, attrCount, &attr, &availCount);
    }

    if ((status != SAI_STATUS_SUCCESS) || (objType == SAI_OBJECT_TYPE_NULL))
    {
        if (crmResSaiAvailAttrMap.find(type) != crmResSaiAvailAttrMap.end())
        {
            attr.id = crmResSaiAvailAttrMap.at(type);
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
        }

        if ((status == SAI_STATUS_NOT_SUPPORTED) ||
            (status == SAI_STATUS_NOT_IMPLEMENTED) ||
            SAI_STATUS_IS_ATTR_NOT_SUPPORTED(status) ||
            SAI_STATUS_IS_ATTR_NOT_IMPLEMENTED(status))
        {
            // mark unsupported resources
            res.resStatus = CrmResourceStatus::CRM_RES_NOT_SUPPORTED;
            SWSS_LOG_NOTICE("CRM resource %s not supported", crmResTypeNameMap.at(type).c_str());
            return false;
        }

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to get availability counter for %s CRM resourse", crmResTypeNameMap.at(type).c_str());
            return false;
        }

        availCount = attr.value.u32;
    }

    res.countersMap[CRM_COUNTERS_TABLE_KEY].availableCounter = static_cast<uint32_t>(availCount);

    return true;
}

void CrmOrch::getResAvailableCounters()
{
    SWSS_LOG_ENTER();

    for (auto &res : m_resourcesMap)
    {
        // ignore unsupported resources
        if (res.second.resStatus != CrmResourceStatus::CRM_RES_SUPPORTED)
        {
            continue;
        }

        switch (res.first)
        {
            case CrmResourceType::CRM_IPV4_ROUTE:
            case CrmResourceType::CRM_IPV6_ROUTE:
            case CrmResourceType::CRM_IPV4_NEXTHOP:
            case CrmResourceType::CRM_IPV6_NEXTHOP:
            case CrmResourceType::CRM_IPV4_NEIGHBOR:
            case CrmResourceType::CRM_IPV6_NEIGHBOR:
            case CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER:
            case CrmResourceType::CRM_NEXTHOP_GROUP:
            case CrmResourceType::CRM_FDB_ENTRY:
            case CrmResourceType::CRM_IPMC_ENTRY:
            case CrmResourceType::CRM_SNAT_ENTRY:
            case CrmResourceType::CRM_DNAT_ENTRY:
            case CrmResourceType::CRM_MPLS_INSEG:
            case CrmResourceType::CRM_NEXTHOP_GROUP_MAP:
            case CrmResourceType::CRM_SRV6_MY_SID_ENTRY:
            case CrmResourceType::CRM_MPLS_NEXTHOP:
            case CrmResourceType::CRM_SRV6_NEXTHOP:
            {
                getResAvailability(res.first, res.second);
                break;
            }

            case CrmResourceType::CRM_ACL_TABLE:
            case CrmResourceType::CRM_ACL_GROUP:
            {
                sai_attribute_t attr;
                attr.id = crmResSaiAvailAttrMap.at(res.first);

                vector<sai_acl_resource_t> resources(CRM_ACL_RESOURCE_COUNT);

                attr.value.aclresource.count = CRM_ACL_RESOURCE_COUNT;
                attr.value.aclresource.list = resources.data();
                sai_status_t status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
                if (status == SAI_STATUS_BUFFER_OVERFLOW)
                {
                    resources.resize(attr.value.aclresource.count);
                    attr.value.aclresource.list = resources.data();
                    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
                }

                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to get switch attribute %u , rv:%d", attr.id, status);
                    task_process_status handle_status = handleSaiGetStatus(SAI_API_SWITCH, status);
                    if (handle_status != task_process_status::task_success)
                    {
                        break;
                    }
                }

                for (uint32_t i = 0; i < attr.value.aclresource.count; i++)
                {
                    string key = getCrmAclKey(attr.value.aclresource.list[i].stage, attr.value.aclresource.list[i].bind_point);
                    res.second.countersMap[key].availableCounter = attr.value.aclresource.list[i].avail_num;
                }

                break;
            }

            case CrmResourceType::CRM_ACL_ENTRY:
            case CrmResourceType::CRM_ACL_COUNTER:
            {
                sai_attribute_t attr;
                attr.id = crmResSaiAvailAttrMap.at(res.first);

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
                SWSS_LOG_ERROR("Failed to get CRM resource type %u. Unknown resource type.\n", static_cast<uint32_t>(res.first));
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
        try
        {
            for (const auto &cnt : m_resourcesMap.at(i.second).countersMap)
            {
                FieldValueTuple attr(i.first, to_string(cnt.second.usedCounter));
                vector<FieldValueTuple> attrs = { attr };
                m_countersCrmTable->set(cnt.first, attrs);
            }
        }
        catch(const out_of_range &e)
        {
            // expected when a resource is unavailable
        }
    }

    // Update CRM available counters in COUNTERS_DB
    for (const auto &i : crmAvailCntsTableMap)
    {
        try
        {
            for (const auto &cnt : m_resourcesMap.at(i.second).countersMap)
            {
                FieldValueTuple attr(i.first, to_string(cnt.second.availableCounter));
                vector<FieldValueTuple> attrs = { attr };
                m_countersCrmTable->set(cnt.first, attrs);
            }
        }
        catch(const out_of_range &e)
        {
            // expected when a resource is unavailable
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
                uint32_t dvsr = cnt.usedCounter + cnt.availableCounter;
                if (dvsr != 0)
                {
                    percentageUtil = (cnt.usedCounter * 100) / dvsr;
                }
                else
                {
                    SWSS_LOG_WARN("%s Exception occurred (div by Zero): Used count %u free count %u",
                                  res.name.c_str(), cnt.usedCounter, cnt.availableCounter);
                }
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
        case SAI_ACL_BIND_POINT_TYPE_ROUTER_INTERFACE:
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
