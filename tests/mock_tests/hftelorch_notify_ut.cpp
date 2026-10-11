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
#include <type_traits>
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
extern sai_tam_api_t *sai_tam_api;

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

    struct HFTelProfileStub
    {
        alignas(HFTelProfile) unsigned char buf[sizeof(HFTelProfile)];
        HFTelProfile *p = nullptr;

        void init(bool block_updates = false)
        {
            memset(buf, 0, sizeof(buf));
            p = reinterpret_cast<HFTelProfile *>(static_cast<void *>(buf));

            new (const_cast<string*>(&p->m_profile_name)) string("profile");
            p->m_setting_state = SAI_TAM_TEL_TYPE_STATE_STOP_STREAM;
            p->m_poll_interval = 0;

            new (&p->m_groups) decay_t<decltype(p->m_groups)>();
            new (&p->m_name_sai_map) decay_t<decltype(p->m_name_sai_map)>();
            new (&p->m_sai_tam_counter_subscription_objs)
                decay_t<decltype(p->m_sai_tam_counter_subscription_objs)>();
            new (&p->m_sai_tam_tel_type_objs)
                decay_t<decltype(p->m_sai_tam_tel_type_objs)>();
            new (&p->m_sai_tam_tel_type_states)
                decay_t<decltype(p->m_sai_tam_tel_type_states)>();

            HFTelGroup group("PORT");
            group.updateObjects({"Ethernet0"});
            p->m_groups.emplace(SAI_OBJECT_TYPE_PORT, group);

            if (block_updates)
            {
                auto guard = make_shared<sai_object_id_t>(0x100);
                p->m_sai_tam_tel_type_objs[SAI_OBJECT_TYPE_PORT] = guard;
                p->m_sai_tam_tel_type_states[guard] = SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG;
            }
        }

        ~HFTelProfileStub()
        {
            if (!p) return;
            p->m_profile_name.~basic_string();
            p->m_groups.~map();
            p->m_name_sai_map.~unordered_map();
            p->m_sai_tam_counter_subscription_objs.~unordered_map();
            p->m_sai_tam_tel_type_objs.~unordered_map();
            p->m_sai_tam_tel_type_states.~unordered_map();
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

    TEST_F(LocallyNotifyTest, SetAndDelUpdateProfile)
    {
        HFTelProfileStub profile;
        profile.init();
        stub.p->m_type_profile_mapping[SAI_OBJECT_TYPE_PORT].insert(
            shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {}));

        CounterNameMapUpdater::Message set_msg;
        set_msg.m_table_name = COUNTERS_PORT_NAME_MAP;
        set_msg.m_operation = CounterNameMapUpdater::SET;
        set_msg.m_counter_name = "Ethernet0";
        set_msg.m_oid = 0x1000000000001ULL;

        ASSERT_NO_THROW(stub.p->locallyNotify(set_msg));
        ASSERT_EQ(profile.p->m_name_sai_map[SAI_OBJECT_TYPE_PORT]["Ethernet0"],
                  0x1000000000001ULL);

        CounterNameMapUpdater::Message del_msg;
        del_msg.m_table_name = COUNTERS_PORT_NAME_MAP;
        del_msg.m_operation = CounterNameMapUpdater::DEL;
        del_msg.m_counter_name = "Ethernet0";

        ASSERT_NO_THROW(stub.p->locallyNotify(del_msg));
        auto objs = profile.p->m_name_sai_map.find(SAI_OBJECT_TYPE_PORT);
        EXPECT_TRUE(objs == profile.p->m_name_sai_map.end() || objs->second.empty());
    }

    TEST_F(LocallyNotifyTest, SkipsProfileWhenConfigIsGenerating)
    {
        HFTelProfileStub profile;
        profile.init(true);
        stub.p->m_type_profile_mapping[SAI_OBJECT_TYPE_PORT].insert(
            shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {}));

        CounterNameMapUpdater::Message msg;
        msg.m_table_name = COUNTERS_PORT_NAME_MAP;
        msg.m_operation = CounterNameMapUpdater::SET;
        msg.m_counter_name = "Ethernet0";
        msg.m_oid = 0x1000000000001ULL;

        ASSERT_NO_THROW(stub.p->locallyNotify(msg));
        EXPECT_TRUE(profile.p->m_name_sai_map.empty());
    }

    /* An HFT SAI create failure must not propagate into the orch that
     * updates the counter name map (e.g. PortsOrch, BufferOrch). */
    TEST_F(LocallyNotifyTest, CounterNameUpdater_SaiCreateFailureDoesNotPropagate)
    {
        HFTelProfileStub profile;
        profile.init();
        profile.p->m_groups.at(SAI_OBJECT_TYPE_PORT).updateStatsIDs({SAI_PORT_STAT_IF_IN_OCTETS});
        auto tel_type = make_shared<sai_object_id_t>(0x100);
        profile.p->m_sai_tam_tel_type_objs[SAI_OBJECT_TYPE_PORT] = tel_type;
        profile.p->m_sai_tam_tel_type_states[tel_type] = SAI_TAM_TEL_TYPE_STATE_STOP_STREAM;
        stub.p->m_type_profile_mapping[SAI_OBJECT_TYPE_PORT].insert(
            shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {}));

        static sai_tam_api_t default_tam_api{};
        sai_tam_api_t *orig_api = sai_tam_api ? sai_tam_api : &default_tam_api;
        sai_tam_api_t ut_api = *orig_api;
        ut_api.create_tam_counter_subscription = [](sai_object_id_t *, sai_object_id_t, uint32_t, const sai_attribute_t *) -> sai_status_t
        {
            return SAI_STATUS_NO_MEMORY;
        };
        sai_tam_api_t *saved_api = sai_tam_api;
        sai_tam_api = &ut_api;

        CounterNameMapUpdater updater("COUNTERS_DB", COUNTERS_PORT_NAME_MAP);
        EXPECT_NO_THROW(updater.setCounterNameMap("Ethernet0", 0x1000000000001ULL));
        EXPECT_TRUE(profile.p->m_sai_tam_counter_subscription_objs[SAI_OBJECT_TYPE_PORT].empty());

        sai_tam_api = saved_api;
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
