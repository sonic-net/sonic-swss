#include <inttypes.h>
#include <vector>
#include "flow_counter_handler.h"
#include "logger.h"
#include "sai_serialize.h"

extern sai_object_id_t      gSwitchId;
extern sai_counter_api_t*   sai_counter_api;

const std::vector<sai_counter_stat_t> generic_counter_stat_ids =
{
    SAI_COUNTER_STAT_PACKETS,
    SAI_COUNTER_STAT_BYTES,
};

bool FlowCounterHandler::createGenericCounter(sai_object_id_t &counter_id)
{
    sai_attribute_t counter_attr;
    counter_attr.id = SAI_COUNTER_ATTR_TYPE;
    counter_attr.value.s32 = SAI_COUNTER_TYPE_REGULAR;
    sai_status_t sai_status = sai_counter_api->create_counter(&counter_id, gSwitchId, 1, &counter_attr);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to create generic counter");
        return false;
    }

    return true;
}

bool FlowCounterHandler::removeGenericCounter(sai_object_id_t counter_id)
{
    sai_status_t sai_status = sai_counter_api->remove_counter(counter_id);
    if (sai_status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove generic counter: %" PRId64 "", counter_id);
        return false;
    }

    return true;
}

void FlowCounterHandler::getGenericCounterStatIdList(std::unordered_set<std::string>& counter_stats)
{
    for (const auto& it: generic_counter_stat_ids)
    {
        counter_stats.emplace(sai_serialize_counter_stat(it));
    }
}

bool FlowCounterHandler::queryRouteFlowCounterCapability()
{
    sai_attr_capability_t capability;
    sai_status_t status = sai_query_attribute_capability(gSwitchId, SAI_OBJECT_TYPE_ROUTE_ENTRY, SAI_ROUTE_ENTRY_ATTR_COUNTER_ID, &capability);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Could not query route entry attribute SAI_ROUTE_ENTRY_ATTR_COUNTER_ID %d", status);
        return false;
    }

    return capability.set_implemented;
}
