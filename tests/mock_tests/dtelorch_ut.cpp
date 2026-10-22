#include <gtest/gtest.h>
#include <gmock/gmock.h>

#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_orch_test.h"
#include "dtelorch.h"

extern sai_dtel_api_t *sai_dtel_api;

namespace dtelorch_test
{
    using namespace std;
    using namespace swss;
    using namespace mock_orch_test;

    static sai_status_t _ut_stub_create_dtel(sai_object_id_t *oid, sai_object_id_t,
                                             uint32_t, const sai_attribute_t *)
    {
        *oid = 0x1234;
        return SAI_STATUS_SUCCESS;
    }
    static sai_status_t _ut_stub_set_dtel_attribute(sai_object_id_t, const sai_attribute_t *)
    {
        return SAI_STATUS_SUCCESS;
    }
    static sai_status_t _ut_stub_remove_dtel(sai_object_id_t)
    {
        return SAI_STATUS_SUCCESS;
    }

    class DtelOrchTest : public MockOrchTest {};

    /* Malformed queue id / threshold in DTEL_QUEUE_REPORT must be dropped, not thrown
       out of doTask and left to stall the table. */
    TEST_F(DtelOrchTest, DtelMalformedQueueReportFieldsAreDropped)
    {
        // Stub sai_dtel_api so DTelOrch's constructor (create_dtel) succeeds regardless
        // of VS DTEL support; the malformed path never reaches any other DTEL SAI call.
        sai_dtel_api_t ut_dtel_api = {};
        ut_dtel_api.create_dtel = _ut_stub_create_dtel;
        ut_dtel_api.set_dtel_attribute = _ut_stub_set_dtel_attribute;
        ut_dtel_api.remove_dtel = _ut_stub_remove_dtel;
        sai_dtel_api_t *org_dtel_api = sai_dtel_api;
        sai_dtel_api = &ut_dtel_api;

        vector<string> dtelTables = {
            CFG_DTEL_TABLE_NAME,
            CFG_DTEL_REPORT_SESSION_TABLE_NAME,
            CFG_DTEL_INT_SESSION_TABLE_NAME,
            CFG_DTEL_QUEUE_REPORT_TABLE_NAME,
            CFG_DTEL_EVENT_TABLE_NAME
        };
        {
            DTelOrch dtelOrch(m_config_db.get(), dtelTables, gPortsOrch);
            auto *orch = static_cast<Orch *>(&dtelOrch);

            auto *consumer = dynamic_cast<Consumer *>(orch->getExecutor(CFG_DTEL_QUEUE_REPORT_TABLE_NAME));
            ASSERT_NE(consumer, nullptr);

            deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({"Ethernet0|abc", SET_COMMAND, {{"queue_depth_threshold", "1000"}}});
            entries.push_back({"Ethernet0|1", SET_COMMAND, {{"queue_depth_threshold", "xyz"}}});
            consumer->addToSync(entries);
            entries.clear();

            orch->doTask();

            vector<string> pending;
            orch->dumpPendingTasks(pending);
            EXPECT_TRUE(pending.empty());
        }

        sai_dtel_api = org_dtel_api;
    }
}
