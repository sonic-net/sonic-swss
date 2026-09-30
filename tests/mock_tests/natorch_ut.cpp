#include <gtest/gtest.h>
#include <gmock/gmock.h>

#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_orch_test.h"
#include "natorch.h"

namespace natorch_test
{
    using namespace std;
    using namespace swss;
    using namespace mock_orch_test;

    class NatOrchTest : public MockOrchTest {};

    /* Malformed L4 ports / timeouts in NAT config must be dropped, not thrown out
       of doTask and left to stall the table. */
    TEST_F(NatOrchTest, NatMalformedNumericFieldsAreDropped)
    {
        vector<table_name_with_pri_t> natTables = {
            { APP_NAT_TABLE_NAME,           5 },
            { APP_NAPT_TABLE_NAME,          5 },
            { APP_NAT_TWICE_TABLE_NAME,     5 },
            { APP_NAPT_TWICE_TABLE_NAME,    5 },
            { APP_NAT_GLOBAL_TABLE_NAME,    5 },
            { APP_NAT_DNAT_POOL_TABLE_NAME, 5 }
        };
        NatOrch natOrch(m_app_db.get(), m_state_db.get(), natTables, gRouteOrch, gNeighOrch);
        auto *orch = static_cast<Orch *>(&natOrch);

        // NAPT: non-numeric L4 port in the key.
        auto *naptConsumer = dynamic_cast<Consumer *>(orch->getExecutor(APP_NAPT_TABLE_NAME));
        ASSERT_NE(naptConsumer, nullptr);
        deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"TCP:10.0.0.1:abc", SET_COMMAND,
                           {{"translated_ip", "20.0.0.1"}, {"translated_l4_port", "6000"},
                            {"nat_type", "snat"}, {"entry_type", "static"}}});
        naptConsumer->addToSync(entries);
        entries.clear();

        // NAT_GLOBAL: non-numeric timeout.
        auto *globalConsumer = dynamic_cast<Consumer *>(orch->getExecutor(APP_NAT_GLOBAL_TABLE_NAME));
        ASSERT_NE(globalConsumer, nullptr);
        entries.push_back({"Values", SET_COMMAND,
                           {{"admin_mode", "disabled"}, {"nat_timeout", "abc"}}});
        globalConsumer->addToSync(entries);
        entries.clear();

        orch->doTask();

        vector<string> pending;
        orch->dumpPendingTasks(pending);
        EXPECT_TRUE(pending.empty());
    }
}
