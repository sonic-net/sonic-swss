#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_orch_test.h"
#include "common/mock_test_helpers.h"

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
            return mock_test_helpers::findAttr(create_attrs, id, out);
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

        // The VS test verifies the policer is created once, updated in place via
        // a single set (never recreated), and removed once over its lifecycle.
        // Only the cir field is pushed on update, so PolicerOrch issues exactly
        // one set_policer_attribute call; pin all three to Times(1).
        EXPECT_CALL(*mock_sai_policer_api, create_policer)
            .Times(1)
            .WillOnce(Invoke(m_policerMock.get(), &PolicerSaiMock::handleCreate));
        EXPECT_CALL(*mock_sai_policer_api, set_policer_attribute)
            .Times(1)
            .WillOnce(Invoke(m_policerMock.get(), &PolicerSaiMock::handleSet));
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
        // the existing object in place and reprograms exactly one attribute --
        // CIR. Create-only attributes (METER_TYPE/MODE/RED_PACKET_ACTION) are
        // never re-set on update, and the create_policer Times(1) expectation
        // above guarantees no recreate.
        doPolicerConfig(policer, SET_COMMAND, {{"cir", "800"}});

        // Exactly one attribute was set -- CIR=800 on the existing policer --
        // which also confirms no create-only attribute was reprogrammed.
        ASSERT_EQ(m_policerMock->set_attrs.size(), 1U);
        const auto &upd = m_policerMock->set_attrs.front();
        EXPECT_EQ(upd.first, kPolicerOid);
        EXPECT_EQ(upd.second.id, (sai_attr_id_t)SAI_POLICER_ATTR_CIR);
        EXPECT_EQ(upd.second.value.u64, 800U);

        // --- DELETE (via DEL_COMMAND) ---
        doPolicerConfig(policer, DEL_COMMAND, {});
        EXPECT_EQ(m_policerMock->removed_oid, kPolicerOid);
    }

    TEST_F(PolicerOrchTest, PolicerMalformedFieldsAreDropped)
    {
        // Only the valid policer may reach SAI. The malformed entries sort
        // ahead of it in m_toSync, so if any of them threw out of doTask() or
        // were left pending, the valid one would never be created.
        EXPECT_CALL(*mock_sai_policer_api, create_policer)
            .Times(1)
            .WillOnce(Invoke(m_policerMock.get(), &PolicerSaiMock::handleCreate));
        EXPECT_CALL(*mock_sai_policer_api, set_policer_attribute).Times(0);
        EXPECT_CALL(*mock_sai_policer_api, remove_policer).Times(0);

        auto *policerConsumer = dynamic_cast<Consumer *>(
            static_cast<Orch *>(gPolicerOrch)->getExecutor(CFG_POLICER_TABLE_NAME));
        ASSERT_NE(policerConsumer, nullptr);

        deque<KeyOpFieldsValuesTuple> entries;
        // Non-numeric rate.
        entries.push_back({"BAD_CIR", SET_COMMAND,
                           {
                               {"meter_type", "packets"},
                               {"mode", "sr_tcm"},
                               {"cir", "abc"},
                               {"cbs", "600"},
                           }});
        // Trailing garbage: stoul() used to silently accept this as 600.
        entries.push_back({"BAD_CIR_TRAILING", SET_COMMAND,
                           {
                               {"meter_type", "packets"},
                               {"mode", "sr_tcm"},
                               {"cir", "600kbps"},
                               {"cbs", "600"},
                           }});
        // Float.
        entries.push_back({"BAD_CBS", SET_COMMAND,
                           {
                               {"meter_type", "packets"},
                               {"mode", "sr_tcm"},
                               {"cir", "600"},
                               {"cbs", "600.5"},
                           }});
        // Empty.
        entries.push_back({"BAD_PIR", SET_COMMAND,
                           {
                               {"meter_type", "packets"},
                               {"mode", "tr_tcm"},
                               {"cir", "600"},
                               {"cbs", "600"},
                               {"pir", ""},
                               {"pbs", "600"},
                           }});
        // Overflows uint64.
        entries.push_back({"BAD_PBS", SET_COMMAND,
                           {
                               {"meter_type", "packets"},
                               {"mode", "tr_tcm"},
                               {"cir", "600"},
                               {"cbs", "600"},
                               {"pir", "600"},
                               {"pbs", "99999999999999999999"},
                           }});
        // Unknown enum values: map::at() throws std::out_of_range.
        entries.push_back({"BAD_METER_TYPE", SET_COMMAND,
                           {
                               {"meter_type", "furlongs"},
                               {"mode", "sr_tcm"},
                               {"cir", "600"},
                               {"cbs", "600"},
                           }});
        entries.push_back({"BAD_MODE", SET_COMMAND,
                           {
                               {"meter_type", "packets"},
                               {"mode", "hyperspace"},
                               {"cir", "600"},
                               {"cbs", "600"},
                           }});
        entries.push_back({"GOOD", SET_COMMAND,
                           {
                               {"meter_type", "packets"},
                               {"mode", "sr_tcm"},
                               {"cir", "600"},
                               {"cbs", "600"},
                               {"red_packet_action", "drop"},
                           }});
        policerConsumer->addToSync(entries);
        entries.clear();

        // Storm control is served by the same orch; a malformed kbps must be
        // dropped as well (it fails before any SAI call is made).
        auto *stormConsumer = dynamic_cast<Consumer *>(
            static_cast<Orch *>(gPolicerOrch)->getExecutor(CFG_PORT_STORM_CONTROL_TABLE_NAME));
        ASSERT_NE(stormConsumer, nullptr);
        entries.push_back({"Ethernet0|broadcast", SET_COMMAND, {{"kbps", "abc"}}});
        stormConsumer->addToSync(entries);
        entries.clear();

        // Drain through the production path (Orch::doTask -> Consumer::drain).
        static_cast<Orch *>(gPolicerOrch)->doTask();

        vector<string> pending;
        static_cast<Orch *>(gPolicerOrch)->dumpPendingTasks(pending);
        EXPECT_TRUE(pending.empty());

        // The one create carries GOOD's five attributes.
        ASSERT_EQ(m_policerMock->create_attrs.size(), 5U);
        sai_attribute_value_t v;
        ASSERT_TRUE(m_policerMock->findCreateAttr(SAI_POLICER_ATTR_METER_TYPE, v));
        EXPECT_EQ(v.s32, SAI_METER_TYPE_PACKETS);
        ASSERT_TRUE(m_policerMock->findCreateAttr(SAI_POLICER_ATTR_CIR, v));
        EXPECT_EQ(v.u64, 600U);

        // A malformed update of the existing policer is dropped without
        // touching it: set_policer_attribute stays at Times(0).
        entries.push_back({"GOOD", SET_COMMAND, {{"cir", "abc"}}});
        policerConsumer->addToSync(entries);
        entries.clear();
        static_cast<Orch *>(gPolicerOrch)->doTask();

        pending.clear();
        static_cast<Orch *>(gPolicerOrch)->dumpPendingTasks(pending);
        EXPECT_TRUE(pending.empty());
    }
}
