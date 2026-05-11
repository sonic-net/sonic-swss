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
     * IcmpOrch::resolve_stats_count_mode() (constructor): metadata unavailable.
     */
    TEST_F(IcmpOrchStatsCountModeTest, DoCreate_continuesWhenStatsCountModeMetadataNull)
    {
        icmporch_sai_wrap_ut::IcmpSaiHookGuard g(icmporch_sai_wrap_ut::setIcmpSaiHookMetadataNull);
        IcmpOrch icmpOrch(m_app_db.get(), APP_ICMP_ECHO_SESSION_TABLE_NAME,
                TableConnector(m_state_db.get(), STATE_ICMP_ECHO_SESSION_TABLE_NAME));
        IcmpSaiSessionHandler h(icmpOrch);
        ASSERT_EQ(h.init(sai_icmp_echo_api, "default:default:5000:NORMAL"), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
        EXPECT_EQ(h.create(makeMinimalIcmpSessionFvs()), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
    }

    /**
     * resolve_stats_count_mode: metadata not marked enum false.
     */
    TEST_F(IcmpOrchStatsCountModeTest, DoCreate_continuesWhenStatsCountModeMetadataNotEnum)
    {
        icmporch_sai_wrap_ut::IcmpSaiHookGuard g(icmporch_sai_wrap_ut::setIcmpSaiHookMetadataNotEnum);
        IcmpOrch icmpOrch(m_app_db.get(), APP_ICMP_ECHO_SESSION_TABLE_NAME,
                TableConnector(m_state_db.get(), STATE_ICMP_ECHO_SESSION_TABLE_NAME));
        IcmpSaiSessionHandler h(icmpOrch);
        ASSERT_EQ(h.init(sai_icmp_echo_api, "default:default:5000:NORMAL"), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
        EXPECT_EQ(h.create(makeMinimalIcmpSessionFvs()), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
    }

    /**
     * resolve_stats_count_mode: sai_query_attribute_enum_values_capability fails false;
     */
    TEST_F(IcmpOrchStatsCountModeTest, DoCreate_continuesWhenQueryAttributeEnumValuesCapabilityFails)
    {
        icmporch_sai_wrap_ut::IcmpSaiHookGuard g(icmporch_sai_wrap_ut::setIcmpSaiHookQueryEnumFail);
        IcmpOrch icmpOrch(m_app_db.get(), APP_ICMP_ECHO_SESSION_TABLE_NAME,
                TableConnector(m_state_db.get(), STATE_ICMP_ECHO_SESSION_TABLE_NAME));
        IcmpSaiSessionHandler h(icmpOrch);
        ASSERT_EQ(h.init(sai_icmp_echo_api, "default:default:5000:NORMAL"), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
        EXPECT_EQ(h.create(makeMinimalIcmpSessionFvs()), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
    }

    /**
     * resolve_stats_count_mode: sai_query_attribute_enum_values_capability success with an empty list.
     */
    TEST_F(IcmpOrchStatsCountModeTest, DoCreate_continuesWhenCapabilityEnumListEmpty)
    {
        icmporch_sai_wrap_ut::IcmpSaiHookGuard g(icmporch_sai_wrap_ut::setIcmpSaiHookQueryEnumEmptyList);
        IcmpOrch icmpOrch(m_app_db.get(), APP_ICMP_ECHO_SESSION_TABLE_NAME,
                TableConnector(m_state_db.get(), STATE_ICMP_ECHO_SESSION_TABLE_NAME));
        IcmpSaiSessionHandler h(icmpOrch);
        ASSERT_EQ(h.init(sai_icmp_echo_api, "default:default:5000:NORMAL"), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
        EXPECT_EQ(h.create(makeMinimalIcmpSessionFvs()), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
    }

    /**
     * resolve_stats_count_mode: capability includes PACKET_AND_BYTE (first entry in preferred modes) and create succeeds.
     */
    TEST_F(IcmpOrchStatsCountModeTest, DoCreate_resolvesPacketAndByteWhenReported)
    {
        icmporch_sai_wrap_ut::IcmpSaiHookGuard g(
                icmporch_sai_wrap_ut::setIcmpSaiHookQueryEnumPacketAndByteOnly);
        IcmpOrch icmpOrch(m_app_db.get(), APP_ICMP_ECHO_SESSION_TABLE_NAME,
                TableConnector(m_state_db.get(), STATE_ICMP_ECHO_SESSION_TABLE_NAME));
        IcmpSaiSessionHandler h(icmpOrch);
        ASSERT_EQ(h.init(sai_icmp_echo_api, "default:default:5000:NORMAL"),
                SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
        EXPECT_EQ(h.create(makeMinimalIcmpSessionFvs()), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
    }
} // namespace icmporch_test
