#include "saiextensions.h"
#include "bulker.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <deque>
#include <stdexcept>

namespace
{
size_t removeCalls;
bool throwOnRemove;

sai_status_t createRoutes(
        uint32_t objectCount,
        const sai_route_entry_t *,
        const uint32_t *,
        const sai_attribute_t **,
        sai_bulk_op_error_mode_t,
        sai_status_t *objectStatuses)
{
    std::fill_n(objectStatuses, objectCount, SAI_STATUS_SUCCESS);
    return SAI_STATUS_SUCCESS;
}

sai_status_t removeRoutes(
        uint32_t objectCount,
        const sai_route_entry_t *,
        sai_bulk_op_error_mode_t,
        sai_status_t *objectStatuses)
{
    ++removeCalls;
    if (throwOnRemove)
    {
        throw std::runtime_error("mock route removal failure");
    }
    std::fill_n(objectStatuses, objectCount, SAI_STATUS_SUCCESS);
    return SAI_STATUS_SUCCESS;
}

sai_status_t setRoutes(
        uint32_t objectCount,
        const sai_route_entry_t *,
        const sai_attribute_t *,
        sai_bulk_op_error_mode_t,
        sai_status_t *objectStatuses)
{
    std::fill_n(objectStatuses, objectCount, SAI_STATUS_SUCCESS);
    return SAI_STATUS_SUCCESS;
}

void queueRouteRemovals(
        EntityBulker<sai_route_api_t>& bulker,
        uint32_t objectCount,
        std::deque<sai_status_t>& objectStatuses)
{
    for (uint32_t idx = 0; idx < objectCount; ++idx)
    {
        sai_route_entry_t route{};
        route.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        route.destination.addr.ip4 = 0x0a000000 + idx;
        route.destination.mask.ip4 = 0xfffffffe;
        route.vr_id = 1;
        objectStatuses.push_back(SAI_STATUS_SUCCESS);
        bulker.remove_entry(&objectStatuses.back(), &route);
    }
}

TEST(EntityBulkerFailureTest, ClearsBorrowedRouteStatusesAfterException)
{
    sai_route_api_t routeApi{};
    routeApi.create_route_entries = createRoutes;
    routeApi.remove_route_entries = removeRoutes;
    routeApi.set_route_entries_attribute = setRoutes;

    EntityBulker<sai_route_api_t> bulker(&routeApi, 1000);

    {
        std::deque<sai_status_t> objectStatuses;
        queueRouteRemovals(bulker, 3, objectStatuses);
        throwOnRemove = true;
        removeCalls = 0;

        EXPECT_THROW(bulker.flush(), std::runtime_error);
        EXPECT_EQ(1u, removeCalls);
        EXPECT_EQ(0u, bulker.removing_entries_count());
        EXPECT_EQ(0u, bulker.creating_entries_count());
        EXPECT_EQ(0u, bulker.setting_entries_count());
    }

    throwOnRemove = false;
    removeCalls = 0;
    EXPECT_NO_THROW(bulker.flush());
    EXPECT_EQ(0u, removeCalls);

    std::deque<sai_status_t> objectStatuses;
    queueRouteRemovals(bulker, 1, objectStatuses);
    EXPECT_NO_THROW(bulker.flush());
    EXPECT_EQ(1u, removeCalls);
    ASSERT_EQ(1u, objectStatuses.size());
    EXPECT_EQ(SAI_STATUS_SUCCESS, objectStatuses.front());
}
}
