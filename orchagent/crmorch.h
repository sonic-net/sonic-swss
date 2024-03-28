#pragma once

#include <thread>
#include <chrono>
#include <map>
#include "orch.h"
#include "port.h"
#include "events.h"

extern "C" {
#include "sai.h"
}

enum class CrmResourceType
{
    CRM_IPV4_ROUTE,
    CRM_IPV6_ROUTE,
    CRM_IPV4_NEXTHOP,
    CRM_IPV6_NEXTHOP,
    CRM_IPV4_NEIGHBOR,
    CRM_IPV6_NEIGHBOR,
    CRM_NEXTHOP_GROUP_MEMBER,
    CRM_NEXTHOP_GROUP,
    CRM_ACL_TABLE,
    CRM_ACL_GROUP,
    CRM_ACL_ENTRY,
    CRM_ACL_COUNTER,
    CRM_FDB_ENTRY,
    CRM_IPMC_ENTRY,
    CRM_SNAT_ENTRY,
    CRM_DNAT_ENTRY,
    CRM_MPLS_INSEG,
    CRM_MPLS_NEXTHOP,
    CRM_SRV6_MY_SID_ENTRY,
    CRM_SRV6_NEXTHOP,
    CRM_NEXTHOP_GROUP_MAP,
    CRM_EXT_TABLE,
    CRM_DASH_VNET,
    CRM_DASH_ENI,
    CRM_DASH_ENI_ETHER_ADDRESS_MAP,
    CRM_DASH_IPV4_INBOUND_ROUTING,
    CRM_DASH_IPV6_INBOUND_ROUTING,
    CRM_DASH_IPV4_OUTBOUND_ROUTING,
    CRM_DASH_IPV6_OUTBOUND_ROUTING,
    CRM_DASH_IPV4_PA_VALIDATION,
    CRM_DASH_IPV6_PA_VALIDATION,
    CRM_DASH_IPV4_OUTBOUND_CA_TO_PA,
    CRM_DASH_IPV6_OUTBOUND_CA_TO_PA,
    CRM_DASH_IPV4_ACL_GROUP,
    CRM_DASH_IPV6_ACL_GROUP,
    CRM_DASH_IPV4_ACL_RULE,
    CRM_DASH_IPV6_ACL_RULE,
    CRM_TWAMP_ENTRY
};

enum class CrmThresholdType
{
    CRM_PERCENTAGE,
    CRM_USED,
    CRM_FREE,
};

enum class CrmResourceStatus
{
    CRM_RES_SUPPORTED,
    CRM_RES_NOT_SUPPORTED,
};

class CrmOrch : public Orch
{
public:
    CrmOrch(swss::DBConnector *db, std::string tableName);
    void incCrmResUsedCounter(CrmResourceType resource);
    void decCrmResUsedCounter(CrmResourceType resource);
    // Increment "used" counter for the ACL table/group CRM resources
    void incCrmAclUsedCounter(CrmResourceType resource, sai_acl_stage_t stage, sai_acl_bind_point_type_t point);
    // Decrement "used" counter for the ACL table/group CRM resources
    void decCrmAclUsedCounter(CrmResourceType resource, sai_acl_stage_t stage, sai_acl_bind_point_type_t point, sai_object_id_t oid);
    // Increment "used" counter for the per ACL table CRM resources (ACL entry/counter)
    void incCrmAclTableUsedCounter(CrmResourceType resource, sai_object_id_t tableId);
    // Decrement "used" counter for the per ACL table CRM resources (ACL entry/counter)
    void decCrmAclTableUsedCounter(CrmResourceType resource, sai_object_id_t tableId);
    // Increment "used" counter for the EXT table CRM resources
    void incCrmExtTableUsedCounter(CrmResourceType resource, std::string table_name);
    // Decrement "used" counter for the EXT table CRM resources
    void decCrmExtTableUsedCounter(CrmResourceType resource, std::string table_name);
    // Increment "used" counter for the per DASH ACL CRM resources (ACL group/rule)
    void incCrmDashAclUsedCounter(CrmResourceType resource, sai_object_id_t groupId);
    // Decrement "used" counter for the per DASH ACL CRM resources (ACL group/rule)
    void decCrmDashAclUsedCounter(CrmResourceType resource, sai_object_id_t groupId);

private:
    std::shared_ptr<swss::DBConnector> m_countersDb = nullptr;
    std::shared_ptr<swss::Table> m_countersCrmTable = nullptr;
    swss::SelectableTimer *m_timer = nullptr;

    struct CrmResourceCounter
    {
        sai_object_id_t id = 0;
        uint32_t availableCounter = 0;
        uint32_t usedCounter = 0;
        uint32_t exceededLogCounter = 0;
    };

    struct CrmResourceEntry
    {
        CrmResourceEntry(std::string name, CrmThresholdType thresholdType, uint32_t lowThreshold, uint32_t highThreshold);

        std::string name;

        CrmThresholdType thresholdType = CrmThresholdType::CRM_PERCENTAGE;
        uint32_t lowThreshold = 70;
        uint32_t highThreshold = 85;

        std::map<std::string, CrmResourceCounter> countersMap;

        CrmResourceStatus resStatus = CrmResourceStatus::CRM_RES_SUPPORTED;
    };

    std::chrono::seconds m_pollingInterval;

    std::map<CrmResourceType, CrmResourceEntry> m_resourcesMap;

    void doTask(Consumer &consumer);
    void handleSetCommand(const std::string& key, const std::vector<swss::FieldValueTuple>& data);
    void doTask(swss::SelectableTimer &timer);
    bool getResAvailability(CrmResourceType type, CrmResourceEntry &res);
    bool getDashAclGroupResAvailability(CrmResourceType type, CrmResourceEntry &res);
    void getResAvailableCounters();
    void updateCrmCountersTable();
    void checkCrmThresholds();
    std::string getCrmAclKey(sai_acl_stage_t stage, sai_acl_bind_point_type_t bindPoint);
    std::string getCrmAclTableKey(sai_object_id_t id);
    std::string getCrmP4rtTableKey(std::string table_name);
    std::string getCrmDashAclGroupKey(sai_object_id_t id);
};
