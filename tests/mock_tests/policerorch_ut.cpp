#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_orch_test.h"

#include <gtest/gtest.h>

EXTERN_MOCK_FNS

namespace policerorch_test
{
    // sai_policer_api supports create/remove/set but has no bulk operations, so
    // it needs the WITH_SET (no-bulk) generic mock variant.
    DEFINE_SAI_GENERIC_API_MOCK_WITH_SET(policer, policer);

    using namespace ::testing;
    using namespace std;
    using namespace swss;
    using namespace mock_orch_test;

    // Fixed OID handed back by the mocked create so the subsequent set/remove can
    // be checked against it.
    static constexpr sai_object_id_t kPolicerOid = 0x3000000000001;

    // The mock-test harness runs orchagent against libsaivs in-process; there is
    // no syncd and no populated ASIC_DB to read back (unlike a VS run). We
    // therefore verify what PolicerOrch programs by capturing the attributes it
    // passes to the mocked SAI policer API -- the mock-test equivalent of the VS
    // test's ASIC_DB attribute assertions.
    struct PolicerSaiMock
    {
        vector<sai_attribute_t> create_attrs;
        vector<pair<sai_object_id_t, sai_attribute_t>> set_attrs;
        sai_object_id_t removed_oid = SAI_NULL_OBJECT_ID;

        sai_status_t handleCreate(sai_object_id_t *policer_id, sai_object_id_t,
                                  uint32_t attr_count, const sai_attribute_t *attr_list)
        {
            *policer_id = kPolicerOid;
            create_attrs.assign(attr_list, attr_list + attr_count);
            return SAI_STATUS_SUCCESS;
        }

        sai_status_t handleSet(sai_object_id_t policer_id, const sai_attribute_t *attr)
        {
            set_attrs.emplace_back(policer_id, *attr);
            return SAI_STATUS_SUCCESS;
        }

        sai_status_t handleRemove(sai_object_id_t policer_id)
        {
            removed_oid = policer_id;
            return SAI_STATUS_SUCCESS;
        }

        bool findCreateAttr(sai_attr_id_t id, sai_attribute_value_t &out) const
        {
            for (const auto &attr : create_attrs)
            {
                if (attr.id == id)
                {
                    out = attr.value;
                    return true;
                }
            }
            return false;
        }
    };

    class PolicerOrchTest : public MockOrchTest
    {
    protected:
        unique_ptr<PolicerSaiMock> m_policerMock;

        void ApplyInitialConfigs() override
        {
            // PolicerOrch::doTask() no-ops until gPortsOrch->allPortsReady(), so
            // bring the default ports up first (same idiom the other
            // MockOrchTest-based suites use).
            Table port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
            auto ports = ut_helper::getInitialSaiPorts();
            for (const auto &it : ports)
            {
                port_table.set(it.first, it.second);
            }
            port_table.set("PortConfigDone", {{"count", to_string(ports.size())}});
            port_table.set("PortInitDone", {{}});
            gPortsOrch->addExistingData(&port_table);
            static_cast<Orch *>(gPortsOrch)->doTask();
        }

        void PostSetUp() override
        {
            INIT_SAI_API_MOCK(policer);
            MockSaiApis();
            m_policerMock = make_unique<PolicerSaiMock>();
        }

        void PreTearDown() override
        {
            m_policerMock.reset();
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(policer);
        }

        // Deliver a single CONFIG_DB event to PolicerOrch and run it
        // synchronously. Deletes go through an explicit DEL_COMMAND on the
        // consumer queue -- removing the row and replaying would never fire the
        // delete handler.
        void doPolicerConfig(const string &key, const string &op,
                             const vector<FieldValueTuple> &fvs)
        {
            auto *executor = static_cast<Orch *>(gPolicerOrch)->getExecutor(CFG_POLICER_TABLE_NAME);
            auto *consumer = dynamic_cast<Consumer *>(executor);
            ASSERT_NE(consumer, nullptr);

            deque<KeyOpFieldsValuesTuple> entries;
            entries.push_back({key, op, fvs});
            consumer->addToSync(entries);
            static_cast<Orch *>(gPolicerOrch)->doTask(*consumer);
        }
    };

    TEST_F(PolicerOrchTest, PolicerBasic)
    {
        const string policer = "POLICER";

        // The VS test verifies the policer is created once and removed once over
        // its lifecycle, and updated in place (never recreated). create/remove
        // are pinned to exactly one call; the update may reprogram one or more
        // rate/size attributes, so set is AtLeast(1) and the body asserts which
        // attributes are (and are not) reprogrammed.
        EXPECT_CALL(*mock_sai_policer_api, create_policer)
            .Times(1)
            .WillOnce(Invoke(m_policerMock.get(), &PolicerSaiMock::handleCreate));
        EXPECT_CALL(*mock_sai_policer_api, set_policer_attribute)
            .Times(AtLeast(1))
            .WillRepeatedly(Invoke(m_policerMock.get(), &PolicerSaiMock::handleSet));
        EXPECT_CALL(*mock_sai_policer_api, remove_policer)
            .Times(1)
            .WillOnce(Invoke(m_policerMock.get(), &PolicerSaiMock::handleRemove));

        // --- CREATE ---
        doPolicerConfig(policer, SET_COMMAND,
                        {
                            {"meter_type", "packets"},
                            {"mode", "sr_tcm"},
                            {"cir", "600"},
                            {"cbs", "600"},
                            {"red_packet_action", "drop"},
                        });

        // The VS test asserts exactly five attributes on the created policer.
        ASSERT_EQ(m_policerMock->create_attrs.size(), 5U);

        sai_attribute_value_t v;
        ASSERT_TRUE(m_policerMock->findCreateAttr(SAI_POLICER_ATTR_METER_TYPE, v));
        EXPECT_EQ(v.s32, SAI_METER_TYPE_PACKETS);
        ASSERT_TRUE(m_policerMock->findCreateAttr(SAI_POLICER_ATTR_MODE, v));
        EXPECT_EQ(v.s32, SAI_POLICER_MODE_SR_TCM);
        ASSERT_TRUE(m_policerMock->findCreateAttr(SAI_POLICER_ATTR_CIR, v));
        EXPECT_EQ(v.u64, 600U);
        ASSERT_TRUE(m_policerMock->findCreateAttr(SAI_POLICER_ATTR_CBS, v));
        EXPECT_EQ(v.u64, 600U);
        ASSERT_TRUE(m_policerMock->findCreateAttr(SAI_POLICER_ATTR_RED_PACKET_ACTION, v));
        EXPECT_EQ(v.s32, SAI_PACKET_ACTION_DROP);

        // --- UPDATE (cir 600 -> 800) ---
        // Mirror the VS test: only the cir field is pushed. PolicerOrch updates
        // the existing object in place and only re-programs rate/size attributes
        // (CIR/CBS/PBS/PIR); create-only attributes are left untouched. The
        // create_policer Times(1) expectation above guarantees no recreate.
        doPolicerConfig(policer, SET_COMMAND, {{"cir", "800"}});

        // Every set targets the existing policer, and PolicerOrch only ever
        // reprograms rate/size attributes -- create-only attributes such as
        // METER_TYPE/MODE/RED_PACKET_ACTION must never be re-set on update.
        bool cir_updated_to_800 = false;
        for (const auto &s : m_policerMock->set_attrs)
        {
            EXPECT_EQ(s.first, kPolicerOid);
            EXPECT_TRUE(s.second.id == SAI_POLICER_ATTR_CIR ||
                        s.second.id == SAI_POLICER_ATTR_CBS ||
                        s.second.id == SAI_POLICER_ATTR_PBS ||
                        s.second.id == SAI_POLICER_ATTR_PIR)
                << "unexpected create-only attribute reprogrammed on update: " << s.second.id;
            if (s.second.id == SAI_POLICER_ATTR_CIR)
            {
                EXPECT_EQ(s.second.value.u64, 800U);
                cir_updated_to_800 = true;
            }
        }
        EXPECT_TRUE(cir_updated_to_800);

        // --- DELETE (via DEL_COMMAND) ---
        doPolicerConfig(policer, DEL_COMMAND, {});
        EXPECT_EQ(m_policerMock->removed_oid, kPolicerOid);
    }
}
