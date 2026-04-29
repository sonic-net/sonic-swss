#include "icmporch_sai_wrap.h"
#include "mock_orch_test.h"
#include "schema.h"
#include "icmporch.h"

#include <gtest/gtest.h>
#include <vector>

namespace icmporch_test
{
    using namespace std;
    using namespace swss;

    class IcmpOrchStatsCountModeTest : public mock_orch_test::MockOrchTest
    {
    protected:
        void TearDown() override
        {
            icmporch_sai_wrap_ut::setIcmpSaiHookNone();
            MockOrchTest::TearDown();
        }
    };

    static vector<FieldValueTuple> makeMinimalIcmpSessionFvs()
    {
        return {
            {"session_cookie", "12345"},
            {"src_ip", "10.0.0.1"},
            {"dst_ip", "10.0.0.2"},
            {"tx_interval", "10"},
            {"rx_interval", "10"},
        };
    }

    /**
     * icmporch.cpp (IcmpSaiSessionHandler::do_create, stats count mode block):
     *   if (!meta) { SWSS_LOG_ERROR("sai_metadata_get_attr_metadata ..."); return FAILED_VALID_ENTRY; }
     */
    TEST_F(IcmpOrchStatsCountModeTest, DoCreate_failsWhenStatsCountModeMetadataNull)
    {
        icmporch_sai_wrap_ut::IcmpSaiHookGuard g(icmporch_sai_wrap_ut::setIcmpSaiHookMetadataNull);
        IcmpOrch icmpOrch(m_app_db.get(), APP_ICMP_ECHO_SESSION_TABLE_NAME,
                TableConnector(m_state_db.get(), STATE_ICMP_ECHO_SESSION_TABLE_NAME));
        IcmpSaiSessionHandler h(icmpOrch);
        ASSERT_EQ(h.init(sai_icmp_echo_api, "default:default:5000:NORMAL"), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
        EXPECT_EQ(h.create(makeMinimalIcmpSessionFvs()), SaiOffloadHandlerStatus::FAILED_VALID_ENTRY);
    }

    /**
     * Same block: if (!meta->isenum) { SWSS_LOG_ERROR("sai_metadata_get_attr_metadata ..."); return FAILED_VALID_ENTRY; }
     */
    TEST_F(IcmpOrchStatsCountModeTest, DoCreate_failsWhenStatsCountModeMetadataNotEnum)
    {
        icmporch_sai_wrap_ut::IcmpSaiHookGuard g(icmporch_sai_wrap_ut::setIcmpSaiHookMetadataNotEnum);
        IcmpOrch icmpOrch(m_app_db.get(), APP_ICMP_ECHO_SESSION_TABLE_NAME,
                TableConnector(m_state_db.get(), STATE_ICMP_ECHO_SESSION_TABLE_NAME));
        IcmpSaiSessionHandler h(icmpOrch);
        ASSERT_EQ(h.init(sai_icmp_echo_api, "default:default:5000:NORMAL"), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
        EXPECT_EQ(h.create(makeMinimalIcmpSessionFvs()), SaiOffloadHandlerStatus::FAILED_VALID_ENTRY);
    }

    /**
     *   if (status != SAI_STATUS_SUCCESS) { SWSS_LOG_ERROR("sai_query_attribute_enum_values_capability ..."); }
     */
    TEST_F(IcmpOrchStatsCountModeTest, DoCreate_failsWhenQueryAttributeEnumValuesCapabilityFails)
    {
        icmporch_sai_wrap_ut::IcmpSaiHookGuard g(icmporch_sai_wrap_ut::setIcmpSaiHookQueryEnumFail);
        IcmpOrch icmpOrch(m_app_db.get(), APP_ICMP_ECHO_SESSION_TABLE_NAME,
                TableConnector(m_state_db.get(), STATE_ICMP_ECHO_SESSION_TABLE_NAME));
        IcmpSaiSessionHandler h(icmpOrch);
        ASSERT_EQ(h.init(sai_icmp_echo_api, "default:default:5000:NORMAL"), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
        EXPECT_EQ(h.create(makeMinimalIcmpSessionFvs()), SaiOffloadHandlerStatus::FAILED_VALID_ENTRY);
    }

    /**
     *   else { SWSS_LOG_ERROR("No supported stats count mode found"); return FAILED_VALID_ENTRY; }
     * when SAI reports a mode that is neither PACKET_AND_BYTE nor PACKET (e.g. BYTE only).
     */
    TEST_F(IcmpOrchStatsCountModeTest, DoCreate_failsWhenNoPacketStatsCountModeSupported)
    {
        icmporch_sai_wrap_ut::IcmpSaiHookGuard g(icmporch_sai_wrap_ut::setIcmpSaiHookQueryEnumByteOnlyNoPacketModes);
        IcmpOrch icmpOrch(m_app_db.get(), APP_ICMP_ECHO_SESSION_TABLE_NAME,
                TableConnector(m_state_db.get(), STATE_ICMP_ECHO_SESSION_TABLE_NAME));
        IcmpSaiSessionHandler h(icmpOrch);
        ASSERT_EQ(h.init(sai_icmp_echo_api, "default:default:5000:NORMAL"), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
        EXPECT_EQ(h.create(makeMinimalIcmpSessionFvs()), SaiOffloadHandlerStatus::FAILED_VALID_ENTRY);
    }

    /**
     *   else if (values.list[i] == SAI_STATS_COUNT_MODE_PACKET) { val.s32 = SAI_STATS_COUNT_MODE_PACKET; ... }
     * PACKET is supported but not PACKET_AND_BYTE; do_create should still succeed and session creation may proceed.
     */
    TEST_F(IcmpOrchStatsCountModeTest, DoCreate_acceptsPacketOnlyWhenPacketAndByteNotReported)
    {
        icmporch_sai_wrap_ut::IcmpSaiHookGuard g(icmporch_sai_wrap_ut::setIcmpSaiHookQueryEnumPacketOnly);
        IcmpOrch icmpOrch(m_app_db.get(), APP_ICMP_ECHO_SESSION_TABLE_NAME,
                TableConnector(m_state_db.get(), STATE_ICMP_ECHO_SESSION_TABLE_NAME));
        IcmpSaiSessionHandler h(icmpOrch);
        ASSERT_EQ(h.init(sai_icmp_echo_api, "default:default:5000:NORMAL"), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
        EXPECT_EQ(h.create(makeMinimalIcmpSessionFvs()), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
    }
} // namespace icmporch_test
