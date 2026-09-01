// Pre-include standard library and third-party headers that conflict with
// the #define private public hack (they use 'private' internally).
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <memory>
#include <algorithm>
#include <type_traits>
#include <cstring>

#define private public
#define protected public
#include "high_frequency_telemetry/hftelorch.h"
#include "high_frequency_telemetry/hftelprofile.h"
#undef private
#undef protected

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include <gtest/gtest.h>

extern sai_tam_api_t *sai_tam_api;

namespace hftelprofile_ut
{
    using namespace std;

    /*
     * Mock state for sai_tam_api->get_tam_tel_type_attribute.
     * Controls what the mock returns on each successive call.
     */
    struct MockGetTelTypeAttrState
    {
        sai_status_t first_call_status = SAI_STATUS_BUFFER_OVERFLOW;
        uint32_t     first_call_count  = 0;
        sai_status_t second_call_status = SAI_STATUS_SUCCESS;
        vector<uint8_t> template_data;
        int call_count = 0;
    };

    static MockGetTelTypeAttrState g_mock;

    static sai_status_t mock_get_tam_tel_type_attribute(
        sai_object_id_t /*id*/, uint32_t attr_count, sai_attribute_t *attr_list)
    {
        ++g_mock.call_count;

        if (attr_count != 1 || !attr_list ||
            attr_list[0].id != SAI_TAM_TEL_TYPE_ATTR_IPFIX_TEMPLATES)
        {
            return SAI_STATUS_INVALID_PARAMETER;
        }

        if (g_mock.call_count == 1)
        {
            /* First call — size query (count=0, list=nullptr). */
            attr_list[0].value.u8list.count = g_mock.first_call_count;
            return g_mock.first_call_status;
        }

        /* Second call — data fetch. */
        if (g_mock.second_call_status == SAI_STATUS_SUCCESS &&
            !g_mock.template_data.empty())
        {
            auto n = min(static_cast<uint32_t>(g_mock.template_data.size()),
                         attr_list[0].value.u8list.count);
            memcpy(attr_list[0].value.u8list.list,
                   g_mock.template_data.data(), n);
            attr_list[0].value.u8list.count = n;
        }
        return g_mock.second_call_status;
    }

    /*
     * Fixture: swaps sai_tam_api->get_tam_tel_type_attribute with our mock
     * for each test, restoring the original on tear-down.
     */
    struct UpdateTemplatesTest : public ::testing::Test
    {
        sai_tam_api_t  ut_api;
        sai_tam_api_t *orig_api;

        /* Minimal state to call updateTemplates(). */
        sai_object_id_t fake_tel_type_oid = 0x100;
        HFTelProfile::sai_guard_t guard;
        CounterNameCache empty_cache;

        void SetUp() override
        {
            if (sai_tam_api == nullptr)
            {
                static sai_tam_api_t default_tam_api{};
                sai_tam_api = &default_tam_api;
            }
            ut_api   = *sai_tam_api;
            orig_api =  sai_tam_api;
            ut_api.get_tam_tel_type_attribute = mock_get_tam_tel_type_attribute;
            sai_tam_api = &ut_api;

            g_mock = MockGetTelTypeAttrState{};
            guard  = make_shared<sai_object_id_t>(fake_tel_type_oid);
        }

        void TearDown() override { sai_tam_api = orig_api; }

        /*
         * Build a *partially-constructed* HFTelProfile that is just enough
         * for updateTemplates() to run.  We skip the real constructor
         * (which calls initTelemetry → SAI) by using raw allocation +
         * placement construction of only the members we need.
         *
         * This is deliberately minimal; we only touch the three maps that
         * updateTemplates() and getObjectType() read/write.
         */
        struct Stub
        {
            alignas(HFTelProfile) unsigned char buf[sizeof(HFTelProfile)];
            HFTelProfile *p = nullptr;

            void init(HFTelProfile::sai_guard_t &guard)
            {
                /*
                 * Zero the storage so any incidental reads of uninitialised
                 * scalar members are safe (e.g. m_poll_interval).
                 */
                memset(buf, 0, sizeof(buf));
                p = reinterpret_cast<HFTelProfile *>(static_cast<void *>(buf));

                /* Placement-new the containers and strings that may be
                 * accessed (directly or via logging) by updateTemplates().
                 * Today that means:
                 *   - m_profile_name
                 *   - m_sai_tam_tel_type_objs
                 *   - m_sai_tam_tel_type_templates
                 * If updateTemplates() starts touching additional members,
                 * extend this partial construction accordingly. */
                new (const_cast<string*>(&p->m_profile_name)) string();
                new (&p->m_sai_tam_tel_type_objs)
                    decay_t<decltype(p->m_sai_tam_tel_type_objs)>();
                new (&p->m_sai_tam_tel_type_templates)
                    decay_t<decltype(p->m_sai_tam_tel_type_templates)>();

                p->m_sai_tam_tel_type_objs[SAI_OBJECT_TYPE_PORT] = guard;
            }

            ~Stub()
            {
                if (!p) return;
                p->m_profile_name.~basic_string();
                p->m_sai_tam_tel_type_objs.~unordered_map();
                p->m_sai_tam_tel_type_templates.~unordered_map();
                p = nullptr;
            }
        };
    };

    /* ---- SAI returns BUFFER_OVERFLOW then SUCCESS (happy path) ---- */
    TEST_F(UpdateTemplatesTest, BufferOverflow_ThenSuccess)
    {
        Stub s;
        s.init(guard);

        g_mock.first_call_status  = SAI_STATUS_BUFFER_OVERFLOW;
        g_mock.first_call_count   = 4;
        g_mock.second_call_status = SAI_STATUS_SUCCESS;
        g_mock.template_data      = {0xAA, 0xBB, 0xCC, 0xDD};

        ASSERT_NO_THROW(s.p->updateTemplates(fake_tel_type_oid));

        auto &tpl = s.p->m_sai_tam_tel_type_templates[SAI_OBJECT_TYPE_PORT];
        ASSERT_EQ(tpl.size(), 4u);
        EXPECT_EQ(tpl[0], 0xAA);
        EXPECT_EQ(tpl[3], 0xDD);
    }

    /* ---- SAI returns SUCCESS on first call (count stays 0) ---- */
    TEST_F(UpdateTemplatesTest, Success_EmptyTemplate)
    {
        Stub s;
        s.init(guard);

        g_mock.first_call_status = SAI_STATUS_SUCCESS;
        g_mock.first_call_count  = 0;

        ASSERT_NO_THROW(s.p->updateTemplates(fake_tel_type_oid));

        auto &tpl = s.p->m_sai_tam_tel_type_templates[SAI_OBJECT_TYPE_PORT];
        EXPECT_TRUE(tpl.empty());
    }

    /* ---- BUFFER_OVERFLOW with count=0 stores an empty template ---- */
    TEST_F(UpdateTemplatesTest, BufferOverflow_EmptyTemplate)
    {
        Stub s;
        s.init(guard);

        g_mock.first_call_status = SAI_STATUS_BUFFER_OVERFLOW;
        g_mock.first_call_count  = 0;

        ASSERT_NO_THROW(s.p->updateTemplates(fake_tel_type_oid));

        auto &tpl = s.p->m_sai_tam_tel_type_templates[SAI_OBJECT_TYPE_PORT];
        EXPECT_TRUE(tpl.empty());
        EXPECT_EQ(g_mock.call_count, 1);
    }

    /* ---- First query fails with unexpected status ---- */
    TEST_F(UpdateTemplatesTest, FirstCall_UnexpectedFailure)
    {
        Stub s;
        s.init(guard);

        g_mock.first_call_status = SAI_STATUS_FAILURE;

        EXPECT_THROW(s.p->updateTemplates(fake_tel_type_oid), runtime_error);
    }

    /* ---- Second call (data fetch) fails ---- */
    TEST_F(UpdateTemplatesTest, SecondCall_Failure)
    {
        Stub s;
        s.init(guard);

        g_mock.first_call_status  = SAI_STATUS_BUFFER_OVERFLOW;
        g_mock.first_call_count   = 4;
        g_mock.second_call_status = SAI_STATUS_FAILURE;

        EXPECT_THROW(s.p->updateTemplates(fake_tel_type_oid), runtime_error);
    }

    /* ---- Unknown tel-type OID → object type not found ---- */
    TEST_F(UpdateTemplatesTest, UnknownOID_Throws)
    {
        Stub s;
        s.init(guard);

        sai_object_id_t bad_oid = 0xDEAD;
        EXPECT_THROW(s.p->updateTemplates(bad_oid), runtime_error);
    }

    /*
     * Fixture for clearGroup() tests.
     * Constructs all members that clearGroup() reads/writes.
     */
    struct ClearGroupTest : public ::testing::Test
    {
        struct ClearGroupStub
        {
            alignas(HFTelProfile) unsigned char buf[sizeof(HFTelProfile)];
            HFTelProfile *p = nullptr;

            void init()
            {
                memset(buf, 0, sizeof(buf));
                p = reinterpret_cast<HFTelProfile *>(static_cast<void *>(buf));

                new (const_cast<string*>(&p->m_profile_name)) string("test_profile");
                new (&p->m_groups) decay_t<decltype(p->m_groups)>();
                new (&p->m_sai_tam_tel_type_templates) decay_t<decltype(p->m_sai_tam_tel_type_templates)>();
                new (&p->m_sai_tam_counter_subscription_objs) decay_t<decltype(p->m_sai_tam_counter_subscription_objs)>();
                new (&p->m_sai_tam_tel_type_objs) decay_t<decltype(p->m_sai_tam_tel_type_objs)>();
                new (&p->m_sai_tam_tel_type_states) decay_t<decltype(p->m_sai_tam_tel_type_states)>();
                new (&p->m_sai_tam_report_objs) decay_t<decltype(p->m_sai_tam_report_objs)>();
                new (&p->m_name_sai_map) decay_t<decltype(p->m_name_sai_map)>();
            }

            ~ClearGroupStub()
            {
                if (!p) return;
                p->m_profile_name.~basic_string();
                p->m_groups.~map();
                p->m_sai_tam_tel_type_templates.~unordered_map();
                p->m_sai_tam_counter_subscription_objs.~unordered_map();
                p->m_sai_tam_tel_type_objs.~unordered_map();
                p->m_sai_tam_tel_type_states.~unordered_map();
                p->m_sai_tam_report_objs.~unordered_map();
                p->m_name_sai_map.~unordered_map();
                p = nullptr;
            }
        };
    };

    /* clearGroup on empty profile — exercises the find-based cleanup path */
    TEST_F(ClearGroupTest, ClearGroup_EmptyProfile)
    {
        ClearGroupStub s;
        s.init();

        // clearGroup with no existing data — should not crash
        ASSERT_NO_THROW(s.p->clearGroup("port"));
    }

    /* clearGroup with tel_type_obj present — covers the if(find) erase branch */
    TEST_F(ClearGroupTest, ClearGroup_WithTelTypeObj)
    {
        ClearGroupStub s;
        s.init();

        auto guard = make_shared<sai_object_id_t>(0x200);
        s.p->m_sai_tam_tel_type_objs[SAI_OBJECT_TYPE_PORT] = guard;
        s.p->m_sai_tam_tel_type_states[guard] = SAI_TAM_TEL_TYPE_STATE_STOP_STREAM;
        s.p->m_sai_tam_tel_type_templates[SAI_OBJECT_TYPE_PORT] = {0x01, 0x02};
        s.p->m_sai_tam_report_objs[SAI_OBJECT_TYPE_PORT] = make_shared<sai_object_id_t>(0x300);

        ASSERT_NO_THROW(s.p->clearGroup("port"));

        EXPECT_TRUE(s.p->m_sai_tam_tel_type_objs.empty());
        EXPECT_TRUE(s.p->m_sai_tam_tel_type_states.empty());
        EXPECT_TRUE(s.p->m_sai_tam_tel_type_templates.empty());
        EXPECT_TRUE(s.p->m_sai_tam_report_objs.empty());
    }

    struct SetStatsIDsTest : public ::testing::Test
    {
        struct SetStatsIDsStub
        {
            alignas(HFTelProfile) unsigned char buf[sizeof(HFTelProfile)];
            HFTelProfile *p = nullptr;

            void init()
            {
                memset(buf, 0, sizeof(buf));
                p = reinterpret_cast<HFTelProfile *>(static_cast<void *>(buf));

                new (const_cast<string*>(&p->m_profile_name)) string("test_profile");
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
            }

            ~SetStatsIDsStub()
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
    };

    TEST_F(SetStatsIDsTest, SetStatsIDsCreatesAndUpdatesGroup)
    {
        SetStatsIDsStub s;
        s.init();

        s.p->setStatsIDs("port", {"IF_IN_OCTETS"});
        ASSERT_EQ(s.p->m_groups.size(), 1u);
        EXPECT_EQ(s.p->m_groups.at(SAI_OBJECT_TYPE_PORT).getStatsIDs(),
                  set<sai_stat_id_t>({SAI_PORT_STAT_IF_IN_OCTETS}));

        s.p->setStatsIDs("port", {"IF_OUT_OCTETS"});
        ASSERT_EQ(s.p->m_groups.size(), 1u);
        EXPECT_EQ(s.p->m_groups.at(SAI_OBJECT_TYPE_PORT).getStatsIDs(),
                  set<sai_stat_id_t>({SAI_PORT_STAT_IF_OUT_OCTETS}));
    }

    TEST_F(SetStatsIDsTest, DelObjectSAIIDMissingTypeDoesNotInsertMap)
    {
        SetStatsIDsStub s;
        s.init();

        HFTelGroup group("port");
        group.updateObjects({"Ethernet0"});
        group.updateStatsIDs({SAI_PORT_STAT_IF_IN_OCTETS});
        s.p->m_groups.emplace(SAI_OBJECT_TYPE_PORT, move(group));

        EXPECT_TRUE(s.p->m_name_sai_map.empty());
        EXPECT_FALSE(s.p->delObjectSAIID(SAI_OBJECT_TYPE_PORT, "Ethernet0"));
        EXPECT_TRUE(s.p->m_name_sai_map.empty());
    }

    struct SaiAttrTest : public ::testing::Test
    {
        sai_tam_api_t ut_api;
        sai_tam_api_t *orig_api = nullptr;

        struct SaiAttrStub
        {
            alignas(HFTelProfile) unsigned char buf[sizeof(HFTelProfile)];
            HFTelProfile *p = nullptr;

            void init()
            {
                memset(buf, 0, sizeof(buf));
                p = reinterpret_cast<HFTelProfile *>(static_cast<void *>(buf));

                new (const_cast<string*>(&p->m_profile_name)) string("test_profile");
                p->m_setting_state = SAI_TAM_TEL_TYPE_STATE_STOP_STREAM;
                p->m_poll_interval = 100;
                new (&p->m_sai_tam_counter_subscription_objs)
                    decay_t<decltype(p->m_sai_tam_counter_subscription_objs)>();
                new (&p->m_sai_tam_tel_type_objs)
                    decay_t<decltype(p->m_sai_tam_tel_type_objs)>();
                new (&p->m_sai_tam_report_objs)
                    decay_t<decltype(p->m_sai_tam_report_objs)>();

                p->m_sai_tam_tel_type_objs[SAI_OBJECT_TYPE_PORT] =
                    make_shared<sai_object_id_t>(0x200);
            }

            ~SaiAttrStub()
            {
                if (!p) return;
                p->m_profile_name.~basic_string();
                p->m_sai_tam_counter_subscription_objs.~unordered_map();
                p->m_sai_tam_tel_type_objs.~unordered_map();
                p->m_sai_tam_report_objs.~unordered_map();
                p = nullptr;
            }
        };

        static vector<sai_attribute_t> report_attrs;
        static vector<sai_attribute_t> counter_attrs;

        static sai_status_t mock_create_tam_report(
            sai_object_id_t *report_id,
            sai_object_id_t /*switch_id*/,
            uint32_t attr_count,
            const sai_attribute_t *attr_list)
        {
            report_attrs.assign(attr_list, attr_list + attr_count);
            *report_id = 0x500;
            return SAI_STATUS_SUCCESS;
        }

        static sai_status_t mock_remove_tam_report(sai_object_id_t /*report_id*/)
        {
            return SAI_STATUS_SUCCESS;
        }

        static sai_status_t mock_create_tam_counter_subscription(
            sai_object_id_t *counter_subscription_id,
            sai_object_id_t /*switch_id*/,
            uint32_t attr_count,
            const sai_attribute_t *attr_list)
        {
            counter_attrs.assign(attr_list, attr_list + attr_count);
            *counter_subscription_id = 0x600;
            return SAI_STATUS_SUCCESS;
        }

        static sai_status_t mock_remove_tam_counter_subscription(
            sai_object_id_t /*counter_subscription_id*/)
        {
            return SAI_STATUS_SUCCESS;
        }

        void SetUp() override
        {
            if (sai_tam_api == nullptr)
            {
                static sai_tam_api_t default_tam_api{};
                sai_tam_api = &default_tam_api;
            }
            ut_api = *sai_tam_api;
            orig_api = sai_tam_api;
            ut_api.create_tam_report = mock_create_tam_report;
            ut_api.remove_tam_report = mock_remove_tam_report;
            ut_api.create_tam_counter_subscription = mock_create_tam_counter_subscription;
            ut_api.remove_tam_counter_subscription = mock_remove_tam_counter_subscription;
            sai_tam_api = &ut_api;
            report_attrs.clear();
            counter_attrs.clear();
        }

        void TearDown() override
        {
            sai_tam_api = orig_api;
        }
    };

    vector<sai_attribute_t> SaiAttrTest::report_attrs;
    vector<sai_attribute_t> SaiAttrTest::counter_attrs;

    TEST_F(SaiAttrTest, GetTAMReportAddsIntervalUnit)
    {
        SaiAttrStub s;
        s.init();

        ASSERT_EQ(s.p->getTAMReportObjID(SAI_OBJECT_TYPE_PORT), 0x500ULL);

        auto itr = find_if(report_attrs.begin(), report_attrs.end(), [](const auto &attr)
        {
            return attr.id == SAI_TAM_REPORT_ATTR_REPORT_INTERVAL_UNIT;
        });
        ASSERT_NE(itr, report_attrs.end());
        EXPECT_EQ(itr->value.s32, SAI_TAM_REPORT_INTERVAL_UNIT_USEC);
    }

    TEST_F(SaiAttrTest, DeployCounterSubscriptionUsesStatIdU32)
    {
        SaiAttrStub s;
        s.init();

        s.p->deployCounterSubscription(
            SAI_OBJECT_TYPE_PORT,
            0x1000000000001ULL,
            SAI_PORT_STAT_IF_IN_OCTETS,
            7);

        auto itr = find_if(counter_attrs.begin(), counter_attrs.end(), [](const auto &attr)
        {
            return attr.id == SAI_TAM_COUNTER_SUBSCRIPTION_ATTR_STAT_ID;
        });
        ASSERT_NE(itr, counter_attrs.end());
        EXPECT_EQ(itr->value.u32, static_cast<uint32_t>(SAI_PORT_STAT_IF_IN_OCTETS));
    }

    struct LocallyNotifyStartedProfileTest : public ::testing::Test
    {
        struct ProfileStub
        {
            alignas(HFTelProfile) unsigned char buf[sizeof(HFTelProfile)];
            HFTelProfile *p = nullptr;

            void init()
            {
                memset(buf, 0, sizeof(buf));
                p = reinterpret_cast<HFTelProfile *>(static_cast<void *>(buf));

                new (const_cast<string*>(&p->m_profile_name)) string("test_profile");
                p->m_setting_state = SAI_TAM_TEL_TYPE_STATE_START_STREAM;
                p->m_poll_interval = 0;
                new (&p->m_groups) decay_t<decltype(p->m_groups)>();
                new (&p->m_name_sai_map) decay_t<decltype(p->m_name_sai_map)>();
                new (&p->m_sai_tam_counter_subscription_objs)
                    decay_t<decltype(p->m_sai_tam_counter_subscription_objs)>();
                new (&p->m_sai_tam_tel_type_objs)
                    decay_t<decltype(p->m_sai_tam_tel_type_objs)>();
                new (&p->m_sai_tam_tel_type_states)
                    decay_t<decltype(p->m_sai_tam_tel_type_states)>();

                HFTelGroup group("port");
                group.updateObjects({"Ethernet0"});
                group.updateStatsIDs({SAI_PORT_STAT_IF_IN_OCTETS});
                p->m_groups.emplace(SAI_OBJECT_TYPE_PORT, move(group));
                constexpr sai_object_id_t port_oid = 0x1000000000001ULL;
                p->m_name_sai_map[SAI_OBJECT_TYPE_PORT]["Ethernet0"] = port_oid;
                p->m_sai_tam_counter_subscription_objs[SAI_OBJECT_TYPE_PORT][port_oid][SAI_PORT_STAT_IF_IN_OCTETS] =
                    make_shared<sai_object_id_t>(0x300);

                auto guard = make_shared<sai_object_id_t>(0x200);
                p->m_sai_tam_tel_type_objs[SAI_OBJECT_TYPE_PORT] = guard;
                p->m_sai_tam_tel_type_states[guard] = SAI_TAM_TEL_TYPE_STATE_START_STREAM;
            }

            ~ProfileStub()
            {
                if (!p) return;
                using ProfileName = decay_t<decltype(p->m_profile_name)>;
                using Groups = decay_t<decltype(p->m_groups)>;
                using NameSaiMap = decay_t<decltype(p->m_name_sai_map)>;
                using CounterSubscriptionObjs = decay_t<decltype(p->m_sai_tam_counter_subscription_objs)>;
                using TelTypeObjs = decay_t<decltype(p->m_sai_tam_tel_type_objs)>;
                using TelTypeStates = decay_t<decltype(p->m_sai_tam_tel_type_states)>;

                p->m_profile_name.~ProfileName();
                p->m_groups.~Groups();
                p->m_name_sai_map.~NameSaiMap();
                p->m_sai_tam_counter_subscription_objs.~CounterSubscriptionObjs();
                p->m_sai_tam_tel_type_objs.~TelTypeObjs();
                p->m_sai_tam_tel_type_states.~TelTypeStates();
                p = nullptr;
            }
        };

        struct OrchStub
        {
            alignas(HFTelOrch) unsigned char buf[sizeof(HFTelOrch)];
            HFTelOrch *p = nullptr;

            void init(const shared_ptr<HFTelProfile> &profile)
            {
                memset(buf, 0, sizeof(buf));
                p = reinterpret_cast<HFTelOrch *>(static_cast<void *>(buf));

                new (&p->m_type_profile_mapping) decay_t<decltype(p->m_type_profile_mapping)>();
                new (&p->m_counter_name_cache) decay_t<decltype(p->m_counter_name_cache)>();
                p->m_type_profile_mapping[SAI_OBJECT_TYPE_PORT].insert(profile);
            }

            ~OrchStub()
            {
                if (!p) return;
                using TypeProfileMapping = decay_t<decltype(p->m_type_profile_mapping)>;
                using CounterNameCacheType = decay_t<decltype(p->m_counter_name_cache)>;

                p->m_type_profile_mapping.~TypeProfileMapping();
                p->m_counter_name_cache.~CounterNameCacheType();
                p = nullptr;
            }
        };
    };

    TEST_F(LocallyNotifyStartedProfileTest, UnrelatedObjectUpdateDoesNotRecommitStartedProfile)
    {
        ProfileStub profileStub;
        profileStub.init();

        ASSERT_EQ(profileStub.p->getStreamState(SAI_OBJECT_TYPE_PORT), SAI_TAM_TEL_TYPE_STATE_START_STREAM);
        ASSERT_TRUE(profileStub.p->isMonitoringObjectReady(SAI_OBJECT_TYPE_PORT));
        ASSERT_TRUE(profileStub.p->canBeUpdated(SAI_OBJECT_TYPE_PORT));
        ASSERT_THROW(profileStub.p->tryCommitConfig(SAI_OBJECT_TYPE_PORT), runtime_error);

        shared_ptr<HFTelProfile> profile(profileStub.p, [](HFTelProfile *) {});
        OrchStub orchStub;
        orchStub.init(profile);

        CounterNameMapUpdater::Message msg;
        msg.m_table_name = "COUNTERS_PORT_NAME_MAP";
        msg.m_operation = CounterNameMapUpdater::SET;
        msg.m_counter_name = "Ethernet4";
        msg.m_oid = 0x1000000000002ULL;

        EXPECT_NO_THROW(orchStub.p->locallyNotify(msg));
        EXPECT_EQ(orchStub.p->m_counter_name_cache[SAI_OBJECT_TYPE_PORT]["Ethernet4"], msg.m_oid);
        EXPECT_EQ(profileStub.p->getStreamState(SAI_OBJECT_TYPE_PORT), SAI_TAM_TEL_TYPE_STATE_START_STREAM);
    }

    TEST_F(LocallyNotifyStartedProfileTest, UnrelatedObjectDeleteDoesNotRecommitStartedProfile)
    {
        ProfileStub profileStub;
        profileStub.init();

        ASSERT_EQ(profileStub.p->getStreamState(SAI_OBJECT_TYPE_PORT), SAI_TAM_TEL_TYPE_STATE_START_STREAM);
        ASSERT_TRUE(profileStub.p->isMonitoringObjectReady(SAI_OBJECT_TYPE_PORT));
        ASSERT_TRUE(profileStub.p->canBeUpdated(SAI_OBJECT_TYPE_PORT));
        ASSERT_THROW(profileStub.p->tryCommitConfig(SAI_OBJECT_TYPE_PORT), runtime_error);

        shared_ptr<HFTelProfile> profile(profileStub.p, [](HFTelProfile *) {});
        OrchStub orchStub;
        orchStub.init(profile);
        orchStub.p->m_counter_name_cache[SAI_OBJECT_TYPE_PORT]["Ethernet4"] = 0x1000000000002ULL;

        CounterNameMapUpdater::Message msg;
        msg.m_table_name = "COUNTERS_PORT_NAME_MAP";
        msg.m_operation = CounterNameMapUpdater::DEL;
        msg.m_counter_name = "Ethernet4";

        EXPECT_NO_THROW(orchStub.p->locallyNotify(msg));
        EXPECT_EQ(orchStub.p->m_counter_name_cache[SAI_OBJECT_TYPE_PORT].count("Ethernet4"), 0);
        EXPECT_EQ(profileStub.p->getStreamState(SAI_OBJECT_TYPE_PORT), SAI_TAM_TEL_TYPE_STATE_START_STREAM);
    }
}
