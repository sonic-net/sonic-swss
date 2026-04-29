// Pre-include standard library and third-party headers that conflict with
// the #define private public hack (they use 'private' internally).
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <memory>
#include <functional>
#include <cstring>

#define private public
#define protected public
#include "high_frequency_telemetry/hftelorch.h"
#include "high_frequency_telemetry/counternameupdater.h"
#undef private
#undef protected

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include <gtest/gtest.h>

extern HFTelOrch *gHFTOrch;

namespace hftelorch_notify_test
{
    using namespace std;

    /*
     * Stub HFTelOrch that only constructs the members used by locallyNotify().
     * Avoids calling the real constructor which requires full SAI/orch infrastructure.
     *
     * locallyNotify() accesses:
     *   - HFTelOrch::SUPPORT_COUNTER_TABLES (static, always valid)
     *   - m_counter_name_cache
     *   - m_type_profile_mapping
     */
    struct HFTelOrchStub
    {
        alignas(HFTelOrch) unsigned char buf[sizeof(HFTelOrch)];
        HFTelOrch *p = nullptr;

        void init()
        {
            memset(buf, 0, sizeof(buf));
            p = reinterpret_cast<HFTelOrch *>(static_cast<void *>(buf));

            new (&p->m_counter_name_cache)
                decay_t<decltype(p->m_counter_name_cache)>();
            new (&p->m_type_profile_mapping)
                decay_t<decltype(p->m_type_profile_mapping)>();
        }

        ~HFTelOrchStub()
        {
            if (!p) return;
            using CacheType = decay_t<decltype(p->m_counter_name_cache)>;
            using ProfileMapType = decay_t<decltype(p->m_type_profile_mapping)>;
            p->m_counter_name_cache.~CacheType();
            p->m_type_profile_mapping.~ProfileMapType();
            p = nullptr;
        }
    };

    struct LocallyNotifyTest : public ::testing::Test
    {
        HFTelOrchStub stub;
        HFTelOrch *saved_gHFTOrch = nullptr;

        void SetUp() override
        {
            saved_gHFTOrch = gHFTOrch;
            stub.init();
            gHFTOrch = stub.p;
        }

        void TearDown() override
        {
            gHFTOrch = saved_gHFTOrch;
        }
    };

    /* locallyNotify with unsupported table — early return.
     * Covers: msg.m_table_name.c_str() log line. */
    TEST_F(LocallyNotifyTest, UnsupportedTable)
    {
        CounterNameMapUpdater::Message msg;
        msg.m_table_name = "UNSUPPORTED_TABLE";
        msg.m_operation = CounterNameMapUpdater::SET;
        msg.m_counter_name = "Ethernet0";
        msg.m_oid = 0x1000000000001ULL;

        ASSERT_NO_THROW(stub.p->locallyNotify(msg));
    }

    /* locallyNotify SET with supported table, no profiles — cache update path.
     * Covers: msg.m_counter_name, msg.m_oid cache lines. */
    TEST_F(LocallyNotifyTest, SetNoProfile)
    {
        CounterNameMapUpdater::Message msg;
        msg.m_table_name = COUNTERS_PORT_NAME_MAP;
        msg.m_operation = CounterNameMapUpdater::SET;
        msg.m_counter_name = "Ethernet0";
        msg.m_oid = 0x1000000000001ULL;

        ASSERT_NO_THROW(stub.p->locallyNotify(msg));
    }

    /* locallyNotify DEL with supported table, no profiles — cache erase path.
     * Covers: msg.m_counter_name erase line. */
    TEST_F(LocallyNotifyTest, DelNoProfile)
    {
        // First SET
        CounterNameMapUpdater::Message set_msg;
        set_msg.m_table_name = COUNTERS_QUEUE_NAME_MAP;
        set_msg.m_operation = CounterNameMapUpdater::SET;
        set_msg.m_counter_name = "Ethernet0|0";
        set_msg.m_oid = 0x1500000000001ULL;
        stub.p->locallyNotify(set_msg);

        // Then DEL
        CounterNameMapUpdater::Message del_msg;
        del_msg.m_table_name = COUNTERS_QUEUE_NAME_MAP;
        del_msg.m_operation = CounterNameMapUpdater::DEL;
        del_msg.m_counter_name = "Ethernet0|0";

        ASSERT_NO_THROW(stub.p->locallyNotify(del_msg));
    }

    /* CounterNameMapUpdater::setCounterNameMap with gHFTOrch non-null.
     * Covers the Message construction lines in counternameupdater.cpp SET path. */
    TEST_F(LocallyNotifyTest, CounterNameUpdater_SetWithHFT)
    {
        CounterNameMapUpdater updater("COUNTERS_DB", COUNTERS_PORT_NAME_MAP);
        ASSERT_NO_THROW(updater.setCounterNameMap("Ethernet0", 0x1000000000001ULL));
    }

    /* CounterNameMapUpdater::delCounterNameMap with gHFTOrch non-null.
     * Covers the Message construction lines in counternameupdater.cpp DEL path. */
    TEST_F(LocallyNotifyTest, CounterNameUpdater_DelWithHFT)
    {
        CounterNameMapUpdater updater("COUNTERS_DB", COUNTERS_PORT_NAME_MAP);
        ASSERT_NO_THROW(updater.delCounterNameMap("Ethernet0"));
    }
}
