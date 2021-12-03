#include <string>
#include <vector>

#include "crmorch.h"

CrmOrch::CrmOrch(swss::DBConnector *db, std::string tableName) : Orch(db, std::vector<std::string>{})
{
}

void CrmOrch::incCrmResUsedCounter(CrmResourceType resource)
{
}

void CrmOrch::decCrmResUsedCounter(CrmResourceType resource)
{
}

void CrmOrch::incCrmAclUsedCounter(CrmResourceType resource, sai_acl_stage_t stage, sai_acl_bind_point_type_t point)
{
}

void CrmOrch::decCrmAclUsedCounter(CrmResourceType resource, sai_acl_stage_t stage, sai_acl_bind_point_type_t point,
                                   sai_object_id_t oid)
{
}

void CrmOrch::incCrmAclTableUsedCounter(CrmResourceType resource, sai_object_id_t tableId)
{
}

void CrmOrch::decCrmAclTableUsedCounter(CrmResourceType resource, sai_object_id_t tableId)
{
}

void CrmOrch::doTask(Consumer &consumer)
{
}

void CrmOrch::handleSetCommand(const std::string &key, const std::vector<swss::FieldValueTuple> &data)
{
}

void CrmOrch::doTask(swss::SelectableTimer &timer)
{
}

void CrmOrch::getResAvailableCounters()
{
}

void CrmOrch::updateCrmCountersTable()
{
}

void CrmOrch::checkCrmThresholds()
{
}

std::string CrmOrch::getCrmAclKey(sai_acl_stage_t stage, sai_acl_bind_point_type_t bindPoint)
{
    return "";
}

std::string CrmOrch::getCrmAclTableKey(sai_object_id_t id)
{
    return "";
}
