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
            new (&p->m_name_profile_mapping)
                decay_t<decltype(p->m_name_profile_mapping)>();
            new (&p->m_group_update_blocked)
                decay_t<decltype(p->m_group_update_blocked)>();
            new (&p->m_group_delete_blocked)
                decay_t<decltype(p->m_group_delete_blocked)>();
        }

        ~HFTelOrchStub()
        {
            if (!p) return;
            using CacheType = decay_t<decltype(p->m_counter_name_cache)>;
            using ProfileMapType = decay_t<decltype(p->m_type_profile_mapping)>;
            using NameProfileMapType = decay_t<decltype(p->m_name_profile_mapping)>;
            using BlockedSetType = decay_t<decltype(p->m_group_update_blocked)>;
            using DeleteBlockedSetType = decay_t<decltype(p->m_group_delete_blocked)>;
            p->m_name_profile_mapping.~NameProfileMapType();
            p->m_group_update_blocked.~BlockedSetType();
            p->m_group_delete_blocked.~DeleteBlockedSetType();
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

    /*
     * Minimal MIXED-mode HFTelProfile stub for groupTableSet()'s live-
     * reconfiguration gate. Deliberately does NOT construct
     * m_counter_name_cache (a `const CounterNameCache&` reference member -
     * placement-new can't safely rebind a reference in isolation the way it
     * can a value member), so these tests must only exercise the gate path,
     * which returns before ever calling setObjectNames()/
     * loadCounterNameCache() and so never dereferences it.
     */
    struct GroupTableSetProfileStub
    {
        alignas(HFTelProfile) unsigned char buf[sizeof(HFTelProfile)];
        HFTelProfile *p = nullptr;

        void init(sai_tam_tel_type_state_t state,
                  sai_tam_tel_type_mode_t mode = SAI_TAM_TEL_TYPE_MODE_MIXED_TYPE)
        {
            memset(buf, 0, sizeof(buf));
            p = reinterpret_cast<HFTelProfile *>(static_cast<void *>(buf));

            new (const_cast<string*>(&p->m_profile_name)) string("profile");
            new (const_cast<sai_tam_tel_type_mode_t*>(&p->m_tel_type_mode))
                sai_tam_tel_type_mode_t(mode);
            new (const_cast<unordered_set<sai_object_type_t>*>(&p->m_tel_type_supported_categories))
                unordered_set<sai_object_type_t>({SAI_OBJECT_TYPE_PORT});
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
            // Constructed even though no test here sets a report object:
            // setPollInterval() (reached by ProfileTableSetDisableTest)
            // unconditionally iterates this map, so it must be a valid
            // empty map, not raw zeroed memory.
            new (&p->m_sai_tam_report_objs)
                decay_t<decltype(p->m_sai_tam_report_objs)>();
            // Constructed even though no test here reads it directly:
            // notifyConfigReady() -> updateTemplates() unconditionally
            // assigns into this map.
            new (&p->m_sai_tam_tel_type_templates)
                decay_t<decltype(p->m_sai_tam_tel_type_templates)>();

            auto guard = make_shared<sai_object_id_t>(0x700);
            p->m_sai_tam_tel_type_objs[SAI_OBJECT_TYPE_NULL] = guard;
            p->m_sai_tam_tel_type_states[guard] = state;
        }

        ~GroupTableSetProfileStub()
        {
            if (!p) return;
            using CategoriesType = decay_t<decltype(p->m_tel_type_supported_categories)>;
            p->m_profile_name.~basic_string();
            const_cast<CategoriesType&>(p->m_tel_type_supported_categories).~CategoriesType();
            p->m_groups.~map();
            p->m_name_sai_map.~unordered_map();
            p->m_sai_tam_counter_subscription_objs.~unordered_map();
            p->m_sai_tam_tel_type_objs.~unordered_map();
            p->m_sai_tam_tel_type_states.~unordered_map();
            p->m_sai_tam_report_objs.~unordered_map();
            p->m_sai_tam_tel_type_templates.~unordered_map();
            p = nullptr;
        }
    };

    struct GroupTableSetLiveReconfigTest : public ::testing::Test
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

        vector<swss::FieldValueTuple> objectNamesValues() const
        {
            return {{"object_names", "Ethernet0"}};
        }

        vector<swss::FieldValueTuple> objectCountersValues() const
        {
            return {{"object_counters", "SAI_PORT_STAT_IF_IN_OCTETS"}};
        }
    };

    /* A MIXED-mode profile actively streaming must reject an object_names
     * update with task_need_retry - not apply it, not fail it outright - so
     * it's automatically retried once the profile stops. */
    TEST_F(GroupTableSetLiveReconfigTest, RejectsObjectNamesUpdateWhileStreaming)
    {
        GroupTableSetProfileStub profile;
        profile.init(SAI_TAM_TEL_TYPE_STATE_START_STREAM);
        auto profile_ptr = shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {});
        stub.p->m_name_profile_mapping.emplace("profile", profile_ptr);

        auto status = stub.p->groupTableSet("profile", "PORT", objectNamesValues());

        EXPECT_EQ(status, task_process_status::task_need_retry);
        // Rejected before ever touching the group - setObjectNames() must
        // not have run.
        EXPECT_TRUE(profile.p->m_groups.empty());
    }

    /* The block must be logged exactly once per "profile|group", not on
     * every retry poll (see m_group_update_blocked in hftelorch.h) -
     * repeated calls while still streaming must not grow or duplicate the
     * tracking entry. */
    TEST_F(GroupTableSetLiveReconfigTest, TracksBlockedKeyOnceAcrossRepeatedRetries)
    {
        GroupTableSetProfileStub profile;
        profile.init(SAI_TAM_TEL_TYPE_STATE_START_STREAM);
        auto profile_ptr = shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {});
        stub.p->m_name_profile_mapping.emplace("profile", profile_ptr);

        stub.p->groupTableSet("profile", "PORT", objectNamesValues());
        stub.p->groupTableSet("profile", "PORT", objectNamesValues());
        stub.p->groupTableSet("profile", "PORT", objectNamesValues());

        ASSERT_EQ(stub.p->m_group_update_blocked.size(), 1u);
        EXPECT_EQ(*stub.p->m_group_update_blocked.begin(), "profile|PORT");
    }

    /* Closes a gap the object_names-only gate left open: canBeUpdated(type)
     * now gates groupTableSet() as a whole (see hftelorch.cpp), so a
     * stats-only (object_counters) update is rejected the same way, where
     * previously it was entirely unguarded. */
    TEST_F(GroupTableSetLiveReconfigTest, RejectsObjectCountersUpdateWhileStreaming)
    {
        GroupTableSetProfileStub profile;
        profile.init(SAI_TAM_TEL_TYPE_STATE_START_STREAM);
        auto profile_ptr = shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {});
        stub.p->m_name_profile_mapping.emplace("profile", profile_ptr);

        auto status = stub.p->groupTableSet("profile", "PORT", objectCountersValues());

        EXPECT_EQ(status, task_process_status::task_need_retry);
        EXPECT_TRUE(profile.p->m_groups.empty());
    }

    /* The other gap closed by routing group deletion through the same
     * canBeUpdated(type) check: deleting a group from a profile that's
     * actively streaming must retry, not delete, the group. */
    TEST_F(GroupTableSetLiveReconfigTest, RejectsGroupDeletionWhileStreaming)
    {
        GroupTableSetProfileStub profile;
        profile.init(SAI_TAM_TEL_TYPE_STATE_START_STREAM);
        HFTelGroup port_group("PORT");
        port_group.updateObjects({"Ethernet0"});
        profile.p->m_groups.emplace(SAI_OBJECT_TYPE_PORT, move(port_group));
        auto profile_ptr = shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {});
        stub.p->m_name_profile_mapping.emplace("profile", profile_ptr);

        auto status = stub.p->groupTableDel("profile", "PORT");

        EXPECT_EQ(status, task_process_status::task_need_retry);
        EXPECT_FALSE(profile.p->m_groups.empty());
    }

    /* Delete-path equivalent of TracksBlockedKeyOnceAcrossRepeatedRetries:
     * repeated groupTableDel() calls while blocked must track "profile|PORT"
     * exactly once in m_group_delete_blocked (not grow or duplicate), and
     * must never touch m_group_update_blocked - the two sets track disjoint
     * operation kinds even though both are now read by
     * hasPendingGroupChange(). */
    TEST_F(GroupTableSetLiveReconfigTest, TracksGroupDeleteBlockedKeyOnce)
    {
        GroupTableSetProfileStub profile;
        profile.init(SAI_TAM_TEL_TYPE_STATE_START_STREAM);
        HFTelGroup port_group("PORT");
        port_group.updateObjects({"Ethernet0"});
        profile.p->m_groups.emplace(SAI_OBJECT_TYPE_PORT, move(port_group));
        auto profile_ptr = shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {});
        stub.p->m_name_profile_mapping.emplace("profile", profile_ptr);

        stub.p->groupTableDel("profile", "PORT");
        stub.p->groupTableDel("profile", "PORT");
        stub.p->groupTableDel("profile", "PORT");

        ASSERT_EQ(stub.p->m_group_delete_blocked.size(), 1u);
        EXPECT_EQ(*stub.p->m_group_delete_blocked.begin(), "profile|PORT");
        EXPECT_TRUE(stub.p->m_group_update_blocked.empty());
    }

    // Deliberately no "block clears, delete actually succeeds, key is
    // erased" test here, for the same reason groupTableSet()'s matching
    // apply path is untested in this file (see the comment near
    // TracksBlockedKeyOnceAcrossRepeatedRetries): groupTableDel()'s success
    // path reaches m_state_telemetry_session.del(), and m_state_telemetry_session
    // is a real swss::Table that needs a live DBConnector to construct -
    // HFTelOrchStub deliberately doesn't construct it, so driving
    // groupTableDel() all the way to success here would call a method on an
    // object this stub never actually built. The erase itself
    // (hftelorch.cpp's m_group_delete_blocked.erase(blocked_key) immediately
    // after the canBeUpdated(type) gate, before any SAI/Table call) is a
    // one-line mirror of m_group_update_blocked's identical erase two lines
    // below it, covered by code review rather than a unit test for that
    // reason.

    /* profileTableDel()'s blocked branch records the bare "profile|" key
     * (no group name) - distinct in shape from groupTableDel()'s
     * "profile|group" keys, but matched by the same profile_name + "|"
     * prefix hasPendingGroupChange() uses. */
    TEST_F(GroupTableSetLiveReconfigTest, TracksProfileLevelDeleteBlockedKey)
    {
        GroupTableSetProfileStub profile;
        profile.init(SAI_TAM_TEL_TYPE_STATE_START_STREAM);
        HFTelGroup port_group("PORT");
        port_group.updateObjects({"Ethernet0"});
        profile.p->m_groups.emplace(SAI_OBJECT_TYPE_PORT, move(port_group));
        auto profile_ptr = shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {});
        stub.p->m_name_profile_mapping.emplace("profile", profile_ptr);

        auto status = stub.p->profileTableDel("profile");

        EXPECT_EQ(status, task_process_status::task_need_retry);
        ASSERT_EQ(stub.p->m_group_delete_blocked.size(), 1u);
        EXPECT_EQ(*stub.p->m_group_delete_blocked.begin(), "profile|");
    }

    // No "SET clears a stale delete-blocked key" test here: a SET never
    // supersedes a pending DEL for the same key. ConsumerBase::addToSyncInternal()
    // (orch.cpp) only lets a later DEL evict an earlier queued SET from the
    // retry cache, not the reverse - a SET queued behind an already-pending
    // DEL is retried alongside it, not instead of it. groupTableDel() erases
    // its own m_group_delete_blocked entry once the DEL actually applies, so
    // there is no stale entry for a SET to clear; unconditionally erasing it
    // from groupTableSet()/profileTableSet() only reintroduces the
    // once-per-block WARN on every retry pass instead of once.

    /* profileTableSet()'s poll_interval is a configuration field, so it's
     * subject to the same stopped-only rule as group configuration, applied
     * before stream_state is handled (see hftelorch.cpp's comment). */
    TEST_F(GroupTableSetLiveReconfigTest, RejectsPollIntervalWhileStreaming)
    {
        GroupTableSetProfileStub profile;
        profile.init(SAI_TAM_TEL_TYPE_STATE_START_STREAM);
        // The no-arg canBeUpdated() profileTableSet uses iterates m_groups -
        // without a group it's vacuously true regardless of stream state,
        // so a group matching the configured tel_type is required for this
        // gate to mean anything.
        HFTelGroup port_group("PORT");
        port_group.updateObjects({"Ethernet0"});
        profile.p->m_groups.emplace(SAI_OBJECT_TYPE_PORT, move(port_group));
        auto profile_ptr = shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {});
        stub.p->m_name_profile_mapping.emplace("profile", profile_ptr);

        auto status = stub.p->profileTableSet("profile", {{"poll_interval", "500"}});

        EXPECT_EQ(status, task_process_status::task_need_retry);
        EXPECT_EQ(profile.p->m_poll_interval, 0u);
    }

    /* poll_interval alone while running (no stream_state field) must keep
     * retrying until a separate disable is issued - this is the "stays
     * blocked" half of the stopped-only rule, not the deadlock the next
     * test guards against. */
    TEST_F(GroupTableSetLiveReconfigTest, RejectsPollIntervalWithEnableWhileAlreadyStreaming)
    {
        GroupTableSetProfileStub profile;
        profile.init(SAI_TAM_TEL_TYPE_STATE_START_STREAM);
        HFTelGroup port_group("PORT");
        port_group.updateObjects({"Ethernet0"});
        profile.p->m_groups.emplace(SAI_OBJECT_TYPE_PORT, move(port_group));
        auto profile_ptr = shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {});
        stub.p->m_name_profile_mapping.emplace("profile", profile_ptr);

        // stream_state=enabled is a same-state no-op here (already
        // streaming) - it does not stop the profile, so it must not grant
        // poll_interval the requesting_disable exception either.
        auto status = stub.p->profileTableSet(
            "profile", {{"stream_state", "enabled"}, {"poll_interval", "500"}});

        EXPECT_EQ(status, task_process_status::task_need_retry);
        EXPECT_EQ(profile.p->m_poll_interval, 0u);
    }

    /*
     * Fixture for the one profileTableSet() path that reaches a real
     * sai_tam_api->set_tam_tel_type_attribute call (via setStreamState()):
     * mirrors hftelprofile_ut.cpp's ClearGroupRegenerationTest mock setup.
     */
    struct ProfileTableSetDisableTest : public GroupTableSetLiveReconfigTest
    {
        sai_tam_api_t ut_api;
        sai_tam_api_t *orig_api = nullptr;

        static sai_status_t mock_set_tam_tel_type_attribute(
            sai_object_id_t /*tam_tel_type_id*/, const sai_attribute_t * /*attr*/)
        {
            return SAI_STATUS_SUCCESS;
        }

        void SetUp() override
        {
            GroupTableSetLiveReconfigTest::SetUp();
            if (sai_tam_api == nullptr)
            {
                static sai_tam_api_t default_tam_api{};
                sai_tam_api = &default_tam_api;
            }
            ut_api = *sai_tam_api;
            orig_api = sai_tam_api;
            ut_api.set_tam_tel_type_attribute = mock_set_tam_tel_type_attribute;
            sai_tam_api = &ut_api;
        }

        void TearDown() override
        {
            sai_tam_api = orig_api;
            GroupTableSetLiveReconfigTest::TearDown();
        }
    };

    /* The deadlock this fixes: disabling a running profile together with a
     * poll_interval change in the same update must both succeed in one
     * pass, not retry forever waiting for a disable that the old gate
     * ordering never let through. */
    TEST_F(ProfileTableSetDisableTest, AppliesPollIntervalTogetherWithDisableWhileStreaming)
    {
        GroupTableSetProfileStub profile;
        profile.init(SAI_TAM_TEL_TYPE_STATE_START_STREAM);
        HFTelGroup port_group("PORT");
        port_group.updateObjects({"Ethernet0"});
        profile.p->m_groups.emplace(SAI_OBJECT_TYPE_PORT, move(port_group));
        auto profile_ptr = shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {});
        stub.p->m_name_profile_mapping.emplace("profile", profile_ptr);

        auto status = stub.p->profileTableSet(
            "profile", {{"stream_state", "disabled"}, {"poll_interval", "500"}});

        EXPECT_EQ(status, task_process_status::task_success);
        EXPECT_EQ(profile.p->m_poll_interval, 500u);
        EXPECT_EQ(profile.p->getStreamState(SAI_OBJECT_TYPE_PORT), SAI_TAM_TEL_TYPE_STATE_STOP_STREAM);
    }

    /* A disable must never reach setStreamState() while a tel_type is
     * mid-commit: the real TAM tel_type state machine only allows
     * CREATE_CONFIG -> STOP_STREAM after the config-ready notification
     * arrives, even though our own setStreamState() treats it as a no-op
     * and would send it to SAI immediately. This is the
     * GroupTableSetLiveReconfigTest fixture (no SAI mock) because the
     * whole point is that the SAI call must never be reached. */
    TEST_F(GroupTableSetLiveReconfigTest, RejectsDisableWhileCreateConfigInProgress)
    {
        GroupTableSetProfileStub profile;
        profile.init(SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG);
        HFTelGroup port_group("PORT");
        port_group.updateObjects({"Ethernet0"});
        profile.p->m_groups.emplace(SAI_OBJECT_TYPE_PORT, move(port_group));
        auto profile_ptr = shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {});
        stub.p->m_name_profile_mapping.emplace("profile", profile_ptr);

        auto status = stub.p->profileTableSet("profile", {{"stream_state", "disabled"}});

        EXPECT_EQ(status, task_process_status::task_need_retry);
        EXPECT_EQ(profile.p->getStreamState(SAI_OBJECT_TYPE_PORT), SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG);
    }

    /* An enable must never reach setStreamState() while a tel_type is
     * mid-commit either: our own setStreamState() treats CREATE_CONFIG ->
     * START_STREAM as a no-op transition and would send it to SAI
     * immediately, before the config-ready notification the real TAM
     * tel_type state machine requires first. Mirrors
     * RejectsDisableWhileCreateConfigInProgress for the enable direction. */
    TEST_F(GroupTableSetLiveReconfigTest, RejectsEnableWhileCreateConfigInProgress)
    {
        GroupTableSetProfileStub profile;
        profile.init(SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG);
        HFTelGroup port_group("PORT");
        port_group.updateObjects({"Ethernet0"});
        profile.p->m_groups.emplace(SAI_OBJECT_TYPE_PORT, move(port_group));
        auto profile_ptr = shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {});
        stub.p->m_name_profile_mapping.emplace("profile", profile_ptr);

        auto status = stub.p->profileTableSet("profile", {{"stream_state", "enabled"}});

        EXPECT_EQ(status, task_process_status::task_need_retry);
        EXPECT_EQ(profile.p->getStreamState(SAI_OBJECT_TYPE_PORT), SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG);
    }

    /* Same as RejectsEnableWhileCreateConfigInProgress, but in SINGLE mode,
     * where mapKey() keys each tel_type by its own object type instead of
     * sharing the SAI_OBJECT_TYPE_NULL key GroupTableSetProfileStub::init()
     * populates - so the CREATE_CONFIG state is seeded directly under the
     * group's own object type here. */
    TEST_F(GroupTableSetLiveReconfigTest, RejectsEnableWhileCreateConfigInProgressSingleMode)
    {
        GroupTableSetProfileStub profile;
        profile.init(SAI_TAM_TEL_TYPE_STATE_STOP_STREAM, SAI_TAM_TEL_TYPE_MODE_SINGLE_TYPE);
        HFTelGroup port_group("PORT");
        port_group.updateObjects({"Ethernet0"});
        profile.p->m_groups.emplace(SAI_OBJECT_TYPE_PORT, move(port_group));
        auto port_guard = make_shared<sai_object_id_t>(0x701);
        profile.p->m_sai_tam_tel_type_objs[SAI_OBJECT_TYPE_PORT] = port_guard;
        profile.p->m_sai_tam_tel_type_states[port_guard] = SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG;
        auto profile_ptr = shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {});
        stub.p->m_name_profile_mapping.emplace("profile", profile_ptr);

        auto status = stub.p->profileTableSet("profile", {{"stream_state", "enabled"}});

        EXPECT_EQ(status, task_process_status::task_need_retry);
        EXPECT_EQ(profile.p->getStreamState(SAI_OBJECT_TYPE_PORT), SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG);
    }

    /* Disabling together with a poll_interval change must still retry as a
     * whole while CREATE_CONFIG is in progress, not let the disable through
     * via the poll_interval exception. */
    TEST_F(GroupTableSetLiveReconfigTest, RejectsPollIntervalWithDisableWhileCreateConfigInProgress)
    {
        GroupTableSetProfileStub profile;
        profile.init(SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG);
        HFTelGroup port_group("PORT");
        port_group.updateObjects({"Ethernet0"});
        profile.p->m_groups.emplace(SAI_OBJECT_TYPE_PORT, move(port_group));
        auto profile_ptr = shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {});
        stub.p->m_name_profile_mapping.emplace("profile", profile_ptr);

        auto status = stub.p->profileTableSet(
            "profile", {{"stream_state", "disabled"}, {"poll_interval", "500"}});

        EXPECT_EQ(status, task_process_status::task_need_retry);
        EXPECT_EQ(profile.p->m_poll_interval, 0u);
        EXPECT_EQ(profile.p->getStreamState(SAI_OBJECT_TYPE_PORT), SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG);
    }

    // Deliberately no "gate releases once stopped, update actually applies"
    // test here: that path runs setObjectNames() -> loadCounterNameCache(),
    // which unconditionally dereferences m_counter_name_cache
    // (hftelprofile.cpp's loadCounterNameCache() calls
    // m_counter_name_cache.find(...) first thing) - a `const
    // CounterNameCache&` reference member this stub cannot safely
    // construct by placement-new in isolation (see GroupTableSetProfileStub's
    // comment). Every other HFTelProfile stub in this test suite
    // (hftelprofile_ut.cpp's ClearGroupTest, SetStatsIDsTest,
    // StreamingMutationTest, ClearGroupRegenerationTest) makes the same
    // trade-off: exercise lower-level methods instead of routing through
    // setObjectNames(). The "once stopped, setObjectNames() applies
    // correctly" behavior itself is already covered by hftelprofile_ut.cpp's
    // existing setObjectNames()-focused tests; what's new here is the gate
    // rejecting the update while streaming, which
    // RejectsObjectNamesUpdateWhileStreaming and
    // TracksBlockedKeyOnceAcrossRepeatedRetries earlier in this file cover.

    /* HFTelOrch::hasPendingGroupChange() is the lookup
     * doTask(NotificationConsumer&) uses to decide whether a MIXED profile
     * may advance to its configured stream state at config-ready, or must
     * first let another group's still-blocked update or delete commit -
     * see notifyConfigReady's hold_before_configured. */
    struct HasPendingGroupChangeTest : public ::testing::Test
    {
        HFTelOrchStub stub;

        void SetUp() override
        {
            stub.init();
        }
    };

    TEST_F(HasPendingGroupChangeTest, FalseWhenNothingBlocked)
    {
        EXPECT_FALSE(stub.p->hasPendingGroupChange("profile"));
    }

    TEST_F(HasPendingGroupChangeTest, TrueForBlockedProfile)
    {
        stub.p->m_group_update_blocked.insert("profile|QUEUE");
        EXPECT_TRUE(stub.p->hasPendingGroupChange("profile"));
    }

    TEST_F(HasPendingGroupChangeTest, FalseForDifferentProfile)
    {
        stub.p->m_group_update_blocked.insert("other_profile|QUEUE");
        EXPECT_FALSE(stub.p->hasPendingGroupChange("profile"));
    }

    TEST_F(HasPendingGroupChangeTest, DoesNotFalsePositiveOnNamePrefixCollision)
    {
        // "profile2" must not be mistaken for "profile" just because it
        // starts with the same characters - the delimiter is part of the
        // match.
        stub.p->m_group_update_blocked.insert("profile2|QUEUE");
        EXPECT_FALSE(stub.p->hasPendingGroupChange("profile"));
    }

    /* A pending delete has the identical dependency on the profile reaching
     * STOP_STREAM that a pending update does (both are gated by
     * canBeUpdated(type)), so it must be visible here too - a delete blocked
     * behind an in-flight commit that config-ready doesn't wait for would
     * never get a chance to retry successfully once the profile reaches its
     * configured state (see shouldHoldBeforeConfigured's MIXED-mode tests
     * below for the end-to-end version of this). */
    TEST_F(HasPendingGroupChangeTest, TrueForBlockedProfileViaPendingDelete)
    {
        stub.p->m_group_delete_blocked.insert("profile|QUEUE");
        EXPECT_TRUE(stub.p->hasPendingGroupChange("profile"));
    }

    /* profileTableDel()'s own blocked key has no group name ("profile|"
     * rather than "profile|group") - still matches the profile_name + "|"
     * prefix this function uses. */
    TEST_F(HasPendingGroupChangeTest, TrueForBlockedProfileViaPendingProfileDelete)
    {
        stub.p->m_group_delete_blocked.insert("profile|");
        EXPECT_TRUE(stub.p->hasPendingGroupChange("profile"));
    }

    /* HFTelOrch::shouldHoldBeforeConfigured() gates hasPendingGroupChange()
     * on MIXED mode. In SINGLE mode each group's tel_type is independent,
     * so an unrelated group still blocked says nothing about the one whose
     * config-ready just fired - holding it anyway would strand it at
     * STOP_STREAM with no later notification to advance it, since its own
     * commit has already completed (see the comment on the declaration). */
    struct ShouldHoldBeforeConfiguredTest : public ::testing::Test
    {
        HFTelOrchStub stub;
        GroupTableSetProfileStub profile;
        shared_ptr<HFTelProfile> profile_ptr;

        void init(sai_tam_tel_type_mode_t mode)
        {
            stub.init();
            profile.init(SAI_TAM_TEL_TYPE_STATE_STOP_STREAM, mode);
            profile_ptr = shared_ptr<HFTelProfile>(profile.p, [](HFTelProfile *) {});
        }
    };

    TEST_F(ShouldHoldBeforeConfiguredTest, MixedModeHoldsWhenAnotherGroupPending)
    {
        init(SAI_TAM_TEL_TYPE_MODE_MIXED_TYPE);
        stub.p->m_group_update_blocked.insert("profile|QUEUE");

        EXPECT_TRUE(stub.p->shouldHoldBeforeConfigured(profile_ptr));
    }

    /* The regression this closes: a group delete blocked behind this same
     * commit (m_group_delete_blocked) must hold the profile just as much as
     * a blocked update does - otherwise the profile jumps to its configured
     * (streaming) state here, and the delete's own canBeUpdated(type) gate
     * then never passes again in MIXED mode until an operator disables the
     * profile, even though the commit that was blocking it has already
     * completed. */
    TEST_F(ShouldHoldBeforeConfiguredTest, MixedModeHoldsWhenAnotherGroupDeletePending)
    {
        init(SAI_TAM_TEL_TYPE_MODE_MIXED_TYPE);
        stub.p->m_group_delete_blocked.insert("profile|QUEUE");

        EXPECT_TRUE(stub.p->shouldHoldBeforeConfigured(profile_ptr));
    }

    TEST_F(ShouldHoldBeforeConfiguredTest, MixedModeDoesNotHoldWhenNothingPending)
    {
        init(SAI_TAM_TEL_TYPE_MODE_MIXED_TYPE);

        EXPECT_FALSE(stub.p->shouldHoldBeforeConfigured(profile_ptr));
    }

    /* The regression this fix closes: a SINGLE-mode profile must never be
     * held back by another group's unrelated block, even though
     * hasPendingGroupChange() alone would report one - each group has its
     * own independent tel_type, so there's nothing for this group's
     * config-ready to wait on. */
    TEST_F(ShouldHoldBeforeConfiguredTest, SingleModeNeverHoldsEvenWithAnotherGroupPending)
    {
        init(SAI_TAM_TEL_TYPE_MODE_SINGLE_TYPE);
        stub.p->m_group_update_blocked.insert("profile|QUEUE");

        EXPECT_FALSE(stub.p->shouldHoldBeforeConfigured(profile_ptr));
    }

    /*
     * Fixture for HFTelProfile::notifyConfigReady()'s hold_before_configured
     * parameter: mirrors ProfileTableSetDisableTest's SAI mock, plus
     * get_tam_tel_type_attribute for updateTemplates() (also called
     * unconditionally by notifyConfigReady).
     */
    struct NotifyConfigReadyTest : public GroupTableSetLiveReconfigTest
    {
        sai_tam_api_t ut_api;
        sai_tam_api_t *orig_api = nullptr;

        static sai_status_t mock_set_tam_tel_type_attribute(
            sai_object_id_t /*tam_tel_type_id*/, const sai_attribute_t * /*attr*/)
        {
            return SAI_STATUS_SUCCESS;
        }

        static sai_status_t mock_get_tam_tel_type_attribute(
            sai_object_id_t /*tam_tel_type_id*/, uint32_t /*attr_count*/, sai_attribute_t *attr_list)
        {
            attr_list[0].value.u8list.count = 0;
            return SAI_STATUS_SUCCESS;
        }

        void SetUp() override
        {
            GroupTableSetLiveReconfigTest::SetUp();
            if (sai_tam_api == nullptr)
            {
                static sai_tam_api_t default_tam_api{};
                sai_tam_api = &default_tam_api;
            }
            ut_api = *sai_tam_api;
            orig_api = sai_tam_api;
            ut_api.set_tam_tel_type_attribute = mock_set_tam_tel_type_attribute;
            ut_api.get_tam_tel_type_attribute = mock_get_tam_tel_type_attribute;
            sai_tam_api = &ut_api;
        }

        void TearDown() override
        {
            sai_tam_api = orig_api;
            GroupTableSetLiveReconfigTest::TearDown();
        }
    };

    /* If another group of the same MIXED profile is still blocked behind
     * this commit, config-ready must settle at STOP_STREAM - not jump
     * straight to the profile's configured state - so the blocked group's
     * retry can apply and commit too, instead of silently missing the
     * stream. */
    TEST_F(NotifyConfigReadyTest, HoldsAtStopWhenAnotherGroupStillPending)
    {
        GroupTableSetProfileStub profile;
        profile.init(SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG);
        profile.p->m_setting_state = SAI_TAM_TEL_TYPE_STATE_START_STREAM;

        profile.p->notifyConfigReady(SAI_OBJECT_TYPE_NULL, /*hold_before_configured=*/true);

        EXPECT_EQ(profile.p->getStreamState(SAI_OBJECT_TYPE_PORT), SAI_TAM_TEL_TYPE_STATE_STOP_STREAM);
    }

    /* Once nothing is pending, config-ready advances to the profile's
     * configured state exactly as before this fix. */
    TEST_F(NotifyConfigReadyTest, AdvancesToConfiguredStateWhenNothingPending)
    {
        GroupTableSetProfileStub profile;
        profile.init(SAI_TAM_TEL_TYPE_STATE_CREATE_CONFIG);
        profile.p->m_setting_state = SAI_TAM_TEL_TYPE_STATE_START_STREAM;

        profile.p->notifyConfigReady(SAI_OBJECT_TYPE_NULL, /*hold_before_configured=*/false);

        EXPECT_EQ(profile.p->getStreamState(SAI_OBJECT_TYPE_PORT), SAI_TAM_TEL_TYPE_STATE_START_STREAM);
    }
}
