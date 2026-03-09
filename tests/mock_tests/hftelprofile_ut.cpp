// Pre-include standard library and third-party headers that conflict with
// the #define private public hack (they use 'private' internally).
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <memory>

#define private public
#define protected public
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
                p = reinterpret_cast<HFTelProfile *>(buf);

                /* Placement-new the containers and strings that may be
                 * accessed (directly or via logging) by updateTemplates(). */
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
}
