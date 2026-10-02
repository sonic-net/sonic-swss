#include "saiextensions.h"
#include "bulker.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <deque>
#include <tuple>
#include <utility>

namespace
{
    enum class BulkOperation
    {
        Create,
        Remove,
        Set
    };
    enum class BulkResult
    {
        Success,
        FailureWithoutStatuses,
        Exception
    };

    std::array<BulkResult, 3> results;
    std::array<size_t, 3> calls;
    std::array<uint32_t, 3> lastCounts;

    size_t index(BulkOperation op)
    {
        return static_cast<size_t>(op);
    }

    sai_status_t completeBulk(BulkOperation op, uint32_t count, sai_status_t *statuses)
    {
        ++calls[index(op)];
        lastCounts[index(op)] = count;
        if (results[index(op)] == BulkResult::Exception)
        {
            throw std::runtime_error("mock bulk exception");
        }
        if (results[index(op)] == BulkResult::FailureWithoutStatuses)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                EXPECT_EQ(SAI_STATUS_FAILURE, statuses[i]);
            }
            return SAI_STATUS_FAILURE;
        }
        std::fill_n(statuses, count, SAI_STATUS_SUCCESS);
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t createRoutes(uint32_t count, const sai_route_entry_t *, const uint32_t *,
                              const sai_attribute_t **, sai_bulk_op_error_mode_t, sai_status_t *statuses)
    {
        return completeBulk(BulkOperation::Create, count, statuses);
    }

    sai_status_t removeRoutes(uint32_t count, const sai_route_entry_t *,
                              sai_bulk_op_error_mode_t, sai_status_t *statuses)
    {
        return completeBulk(BulkOperation::Remove, count, statuses);
    }

    sai_status_t setRoutes(uint32_t count, const sai_route_entry_t *, const sai_attribute_t *,
                           sai_bulk_op_error_mode_t, sai_status_t *statuses)
    {
        return completeBulk(BulkOperation::Set, count, statuses);
    }

    sai_status_t createObjects(sai_object_id_t, uint32_t count, const uint32_t *,
                               const sai_attribute_t **, sai_bulk_op_error_mode_t, sai_object_id_t *oids,
                               sai_status_t *statuses)
    {
        auto status = completeBulk(BulkOperation::Create, count, statuses);
        if (status == SAI_STATUS_SUCCESS)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                oids[i] = 0x100 + i;
            }
        }
        return status;
    }

    sai_status_t removeObjects(uint32_t count, const sai_object_id_t *,
                               sai_bulk_op_error_mode_t, sai_status_t *statuses)
    {
        return completeBulk(BulkOperation::Remove, count, statuses);
    }

    sai_status_t setObjects(uint32_t count, const sai_object_id_t *, const sai_attribute_t *,
                            sai_bulk_op_error_mode_t, sai_status_t *statuses)
    {
        return completeBulk(BulkOperation::Set, count, statuses);
    }

    class BulkerFailureTest : public ::testing::TestWithParam<std::tuple<BulkOperation, uint32_t>>
    {
    protected:
        void SetUp() override
        {
            results.fill(BulkResult::Success);
            calls.fill(0);
            lastCounts.fill(0);
            routeApi.create_route_entries = createRoutes;
            routeApi.remove_route_entries = removeRoutes;
            routeApi.set_route_entries_attribute = setRoutes;
            objectApi.create_next_hops = createObjects;
            objectApi.remove_next_hops = removeObjects;
            objectApi.set_next_hops_attribute = setObjects;
        }

        void queueRoutes(EntityBulker<sai_route_api_t> &bulker, BulkOperation op,
                         uint32_t count, std::deque<sai_status_t> &statuses)
        {
            sai_attribute_t attr{};
            attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
            attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
            for (uint32_t i = 0; i < count; ++i)
            {
                sai_route_entry_t entry{};
                entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
                entry.destination.addr.ip4 = 0x0a000000 + i;
                entry.destination.mask.ip4 = 0xfffffffe;
                entry.vr_id = index(op) + 1;
                statuses.push_back(SAI_STATUS_SUCCESS);
                if (op == BulkOperation::Create)
                {
                    bulker.create_entry(&statuses.back(), &entry, 1, &attr);
                }
                else if (op == BulkOperation::Remove)
                {
                    bulker.remove_entry(&statuses.back(), &entry);
                }
                else
                {
                    bulker.set_entry_attribute(&statuses.back(), &entry, &attr);
                }
            }
        }

        void queueObjects(ObjectBulker<sai_next_hop_api_t> &bulker, BulkOperation op,
                          uint32_t count, std::deque<sai_object_id_t> &oids,
                          std::deque<sai_status_t> &statuses)
        {
            sai_attribute_t attr{};
            attr.id = SAI_NEXT_HOP_ATTR_TYPE;
            attr.value.s32 = SAI_NEXT_HOP_TYPE_IP;
            for (uint32_t i = 0; i < count; ++i)
            {
                if (op == BulkOperation::Create)
                {
                    oids.push_back(0x123);
                    bulker.create_entry(&oids.back(), 1, &attr);
                }
                else if (op == BulkOperation::Remove)
                {
                    statuses.push_back(SAI_STATUS_SUCCESS);
                    bulker.remove_entry(&statuses.back(), 0x1000 + i);
                }
                else
                {
                    bulker.set_entry_attribute(0x1000 + i, &attr);
                }
            }
        }

        template <typename Bulker>
        void expectEmpty(const Bulker &bulker)
        {
            EXPECT_EQ(0u, bulker.creating_entries_count());
            EXPECT_EQ(0u, bulker.removing_entries_count());
            EXPECT_EQ(0u, bulker.setting_entries_count());
        }

        sai_route_api_t routeApi{};
        sai_next_hop_api_t objectApi{};
    };

    TEST_P(BulkerFailureTest, EntityFailureWithoutStatusesDoesNotReportSuccess)
    {
        auto op = std::get<0>(GetParam());
        results[index(op)] = BulkResult::FailureWithoutStatuses;
        EntityBulker<sai_route_api_t> bulker(&routeApi, 1000);
        std::deque<sai_status_t> statuses;
        queueRoutes(bulker, op, std::get<1>(GetParam()), statuses);
        EXPECT_NO_THROW(bulker.flush());
        EXPECT_EQ(std::deque<sai_status_t>(statuses.size(), SAI_STATUS_FAILURE), statuses);
        expectEmpty(bulker);
        statuses.clear();

        results[index(op)] = BulkResult::Success;
        calls.fill(0);
        queueRoutes(bulker, op, 1, statuses);
        bulker.flush();
        EXPECT_EQ(1u, calls[index(op)]);
        EXPECT_EQ(1u, lastCounts[index(op)]);
        ASSERT_EQ(1u, statuses.size());
        EXPECT_EQ(SAI_STATUS_SUCCESS, statuses.front());
        expectEmpty(bulker);
    }

    TEST_P(BulkerFailureTest, EntityExceptionClearsAllPendingStateBeforeStorageDies)
    {
        auto op = std::get<0>(GetParam());
        EntityBulker<sai_route_api_t> bulker(&routeApi, 1000);
        {
            std::array<std::deque<sai_status_t>, 3> statuses;
            for (auto stage : { BulkOperation::Create, BulkOperation::Remove, BulkOperation::Set })
            {
                queueRoutes(bulker, stage, std::get<1>(GetParam()), statuses[index(stage)]);
            }
            results[index(op)] = BulkResult::Exception;
            EXPECT_THROW(bulker.flush(), std::runtime_error);
            expectEmpty(bulker);
        }
        results.fill(BulkResult::Success);
        calls.fill(0);
        EXPECT_NO_THROW(bulker.flush());
        EXPECT_EQ((std::array<size_t, 3>{ 0, 0, 0 }), calls);

        std::deque<sai_status_t> statuses;
        queueRoutes(bulker, op, 1, statuses);
        bulker.flush();
        EXPECT_EQ(1u, calls[index(op)]);
        EXPECT_EQ(1u, lastCounts[index(op)]);
        EXPECT_EQ(SAI_STATUS_SUCCESS, statuses.front());
        expectEmpty(bulker);
    }

    TEST_P(BulkerFailureTest, ObjectFailureWithoutStatusesDoesNotReportSuccess)
    {
        auto op = std::get<0>(GetParam());
        results[index(op)] = BulkResult::FailureWithoutStatuses;
        ObjectBulker<sai_next_hop_api_t> bulker(&objectApi, 0, 1000);
        std::deque<sai_object_id_t> oids;
        std::deque<sai_status_t> statuses;
        queueObjects(bulker, op, std::get<1>(GetParam()), oids, statuses);
        EXPECT_NO_THROW(bulker.flush());
        expectEmpty(bulker);
        EXPECT_EQ(std::deque<sai_status_t>(statuses.size(), SAI_STATUS_FAILURE), statuses);
        EXPECT_EQ(std::deque<sai_object_id_t>(oids.size(), SAI_NULL_OBJECT_ID), oids);
        if (op == BulkOperation::Create)
        {
            EXPECT_EQ(SAI_STATUS_FAILURE, bulker.create_status(SAI_NULL_OBJECT_ID));
        }

        results[index(op)] = BulkResult::Success;
        calls.fill(0);
        oids.clear();
        statuses.clear();
        queueObjects(bulker, op, 1, oids, statuses);
        bulker.flush();
        EXPECT_EQ(1u, calls[index(op)]);
        EXPECT_EQ(1u, lastCounts[index(op)]);
        if (op == BulkOperation::Create)
        {
            EXPECT_NE(sai_object_id_t{ SAI_NULL_OBJECT_ID }, oids.front());
            EXPECT_EQ(SAI_STATUS_SUCCESS, bulker.create_status(oids.front()));
        }
        if (op == BulkOperation::Remove)
        {
            EXPECT_EQ(SAI_STATUS_SUCCESS, statuses.front());
        }
        expectEmpty(bulker);
    }

    TEST_P(BulkerFailureTest, ObjectExceptionClearsAllPendingStateBeforeStorageDies)
    {
        auto op = std::get<0>(GetParam());
        ObjectBulker<sai_next_hop_api_t> bulker(&objectApi, 0, 1000);
        {
            std::deque<sai_object_id_t> oids;
            std::deque<sai_status_t> statuses;
            for (auto stage : { BulkOperation::Create, BulkOperation::Remove, BulkOperation::Set })
            {
                queueObjects(bulker, stage, std::get<1>(GetParam()), oids, statuses);
            }
            results[index(op)] = BulkResult::Exception;
            EXPECT_THROW(bulker.flush(), std::runtime_error);
            expectEmpty(bulker);
        }
        results.fill(BulkResult::Success);
        calls.fill(0);
        EXPECT_NO_THROW(bulker.flush());
        EXPECT_EQ((std::array<size_t, 3>{ 0, 0, 0 }), calls);

        std::deque<sai_object_id_t> oids;
        std::deque<sai_status_t> statuses;
        queueObjects(bulker, op, 1, oids, statuses);
        bulker.flush();
        EXPECT_EQ(1u, calls[index(op)]);
        EXPECT_EQ(1u, lastCounts[index(op)]);
        expectEmpty(bulker);
    }

    TEST_P(BulkerFailureTest, EntityScopeGuardClearsOnExceptionBeforeFlush)
    {
        EntityBulker<sai_route_api_t> bulker(&routeApi, 1000);
        EXPECT_THROW({
        std::deque<sai_status_t> statuses;
        BulkerClearGuard<EntityBulker<sai_route_api_t>> guard(bulker);
        queueRoutes(bulker, std::get<0>(GetParam()), std::get<1>(GetParam()), statuses);
        throw std::runtime_error("exception before flush"); }, std::runtime_error);
        expectEmpty(bulker);
        EXPECT_NO_THROW(bulker.flush());
        EXPECT_EQ((std::array<size_t, 3>{ 0, 0, 0 }), calls);
    }

    TEST_P(BulkerFailureTest, ObjectScopeGuardClearsOnExceptionBeforeFlush)
    {
        ObjectBulker<sai_next_hop_api_t> bulker(&objectApi, 0, 1000);
        EXPECT_THROW({
        std::deque<sai_object_id_t> oids;
        std::deque<sai_status_t> statuses;
        BulkerClearGuard<ObjectBulker<sai_next_hop_api_t>> guard(bulker);
        queueObjects(bulker, std::get<0>(GetParam()), std::get<1>(GetParam()), oids, statuses);
        throw std::runtime_error("exception before flush"); }, std::runtime_error);
        expectEmpty(bulker);
        EXPECT_NO_THROW(bulker.flush());
        EXPECT_EQ((std::array<size_t, 3>{ 0, 0, 0 }), calls);
    }

    TEST(BulkerClearGuardTest, MovedGuardClearsOnlyOnce)
    {
        struct CountingBulker
        {
            void clear()
            {
                ++count;
            }
            size_t count = 0;
        } bulker;
        {
            BulkerClearGuard<CountingBulker> guard(bulker);
            {
                auto movedGuard = std::move(guard);
                EXPECT_EQ(0u, bulker.count);
            }
            EXPECT_EQ(1u, bulker.count);
        }
        EXPECT_EQ(1u, bulker.count);
    }

    INSTANTIATE_TEST_SUITE_P(CreateRemoveSetOneAndMany, BulkerFailureTest,
                             ::testing::Combine(::testing::Values(BulkOperation::Create, BulkOperation::Remove, BulkOperation::Set),
                                                ::testing::Values(1u, 3u)));
}
