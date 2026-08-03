#define private public
#include "policerorch.h"
#undef private

#include "mock_orch_test.h"
#include "mock_table.h"
#include "saihelper.h"

extern sai_policer_api_t *sai_policer_api;

namespace policerorch_test
{
    using namespace mock_orch_test;

    sai_policer_api_t ut_sai_policer_api;
    sai_policer_api_t *pold_sai_policer_api = nullptr;
    bool fail_create_policer = false;
    bool set_policer_attribute_called = false;

    sai_status_t mock_create_policer(
        _Out_ sai_object_id_t *policer_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        if (fail_create_policer)
        {
            *policer_id = SAI_NULL_OBJECT_ID;
            return SAI_STATUS_OBJECT_IN_USE;
        }

        return pold_sai_policer_api->create_policer(policer_id, switch_id, attr_count, attr_list);
    }

    sai_status_t mock_set_policer_attribute(
        _In_ sai_object_id_t policer_id,
        _In_ const sai_attribute_t *attr)
    {
        set_policer_attribute_called = true;
        return pold_sai_policer_api->set_policer_attribute(policer_id, attr);
    }

    class PolicerOrchTest : public MockOrchTest
    {
    protected:
        void ApplySaiMock() override
        {
            ut_sai_policer_api = *sai_policer_api;
            pold_sai_policer_api = sai_policer_api;
            ut_sai_policer_api.create_policer = mock_create_policer;
            ut_sai_policer_api.set_policer_attribute = mock_set_policer_attribute;
            sai_policer_api = &ut_sai_policer_api;
        }

        void PostSetUp() override
        {
            auto consumer = std::unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_app_db.get(), APP_PORT_TABLE_NAME, 1, 1),
                gPortsOrch, APP_PORT_TABLE_NAME));

            consumer->addToSync({ { "PortInitDone", EMPTY_PREFIX, { { "", "" } } } });
            static_cast<Orch *>(gPortsOrch)->doTask(*consumer.get());
            ASSERT_TRUE(gPortsOrch->allPortsReady());
        }

        void doPolicerTask(const std::deque<KeyOpFieldsValuesTuple> &entries)
        {
            auto consumer = std::unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_config_db.get(), CFG_POLICER_TABLE_NAME, 1, 1),
                gPolicerOrch, CFG_POLICER_TABLE_NAME));

            consumer->addToSync(entries);
            static_cast<Orch *>(gPolicerOrch)->doTask(*consumer.get());
        }
    };

    TEST_F(PolicerOrchTest, CreateFailureDoesNotCacheInvalidOid)
    {
        const std::string policer_name = "failed_policer";
        fail_create_policer = true;
        set_policer_attribute_called = false;

        doPolicerTask({ { policer_name,
                           SET_COMMAND,
                           { { "meter_type", "packets" },
                             { "mode", "sr_tcm" },
                             { "cir", "600" },
                             { "cbs", "600" },
                             { "red_packet_action", "drop" } } } });

        EXPECT_FALSE(gPolicerOrch->policerExists(policer_name));

        fail_create_policer = false;
        set_policer_attribute_called = false;

        doPolicerTask({ { policer_name,
                           SET_COMMAND,
                           { { "meter_type", "packets" },
                             { "mode", "sr_tcm" },
                             { "cir", "800" },
                             { "cbs", "800" },
                             { "red_packet_action", "drop" } } } });

        EXPECT_FALSE(set_policer_attribute_called);
        EXPECT_TRUE(gPolicerOrch->policerExists(policer_name));

        fail_create_policer = false;
    }

    TEST_F(PolicerOrchTest, UpdateWithStaleInvalidOidDoesNotCallSet)
    {
        const std::string policer_name = "stale_policer";
        set_policer_attribute_called = false;
        fail_create_policer = false;

        gPolicerOrch->m_syncdPolicers[policer_name] = 5;
        gPolicerOrch->m_policerRefCounts[policer_name] = 0;

        doPolicerTask({ { policer_name,
                           SET_COMMAND,
                           { { "cir", "800" } } } });

        EXPECT_FALSE(set_policer_attribute_called);
        EXPECT_FALSE(gPolicerOrch->policerExists(policer_name));
    }
}
