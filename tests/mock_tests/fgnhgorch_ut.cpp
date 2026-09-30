#include <gtest/gtest.h>
#include <gmock/gmock.h>

#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_orch_test.h"
#include "fgnhgorch.h"

namespace fgnhgorch_test
{
    using namespace std;
    using namespace swss;
    using namespace mock_orch_test;

    class FgNhgOrchTest : public MockOrchTest {};

    /* Malformed numeric FG_NHG/FG_NHG_MEMBER fields must be dropped, not thrown
       out of doTask and left to stall the table. */
    TEST_F(FgNhgOrchTest, FgNhgMalformedNumericFieldsAreDropped)
    {
        // gFgNhgOrch and its CFG_FG_NHG* consumers are created by MockOrchTest.
        auto *nhgConsumer = dynamic_cast<Consumer *>(
            static_cast<Orch *>(gFgNhgOrch)->getExecutor(CFG_FG_NHG));
        ASSERT_NE(nhgConsumer, nullptr);

        deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"bad_bucket", SET_COMMAND,
                           {{"bucket_size", "abc"}, {"match_mode", "route-based"}}});
        entries.push_back({"bad_maxnh", SET_COMMAND,
                           {{"bucket_size", "128"}, {"max_next_hops", "abc"}, {"match_mode", "prefix-based"}}});
        nhgConsumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gFgNhgOrch)->doTask();

        vector<string> pending;
        static_cast<Orch *>(gFgNhgOrch)->dumpPendingTasks(pending);
        EXPECT_TRUE(pending.empty());

        auto *memberConsumer = dynamic_cast<Consumer *>(
            static_cast<Orch *>(gFgNhgOrch)->getExecutor(CFG_FG_NHG_MEMBER));
        ASSERT_NE(memberConsumer, nullptr);
        entries.push_back({"1.1.1.1", SET_COMMAND, {{"FG_NHG", "fgnhg1"}, {"bank", "xyz"}}});
        memberConsumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gFgNhgOrch)->doTask();

        pending.clear();
        static_cast<Orch *>(gFgNhgOrch)->dumpPendingTasks(pending);
        EXPECT_TRUE(pending.empty());
    }
}
