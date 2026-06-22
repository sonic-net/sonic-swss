#include "icmporch_sai_wrap.h"
#include "hftelorch_is_supported_sai_wrap.h"
#include "mock_orch_test.h"
#include "schema.h"
#define private public
#include "icmporch.h"
#undef private
#include "table.h"
#include "dbconnector.h"
#include "sai_serialize.h"
#include "saihelper.h"

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
            hftel_is_supported_ut::setSaiHookNone();
            gTraditionalFlexCounter = false;
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

    static bool nameMapHasField(const std::string& field, std::string& value)
    {
        DBConnector countersDb("COUNTERS_DB", 0);
        Table nameMapTable(&countersDb, COUNTERS_ICMP_ECHO_SESSION_NAME_MAP);
        return nameMapTable.hget("", field, value);
    }

    static void addVidtOridMap(sai_object_id_t oid)
    {
        DBConnector asicDb("ASIC_DB", 0);
        Table vidToRidTable(&asicDb, "VIDTORID");
        std::vector<FieldValueTuple> fvs = {
            {sai_serialize_object_id(oid), "0x1234"},
        };
        vidToRidTable.set("", fvs);
    }

    struct TestableIcmpSaiSessionHandler : public IcmpSaiSessionHandler
    {
        explicit TestableIcmpSaiSessionHandler(IcmpOrch& orch) : IcmpSaiSessionHandler(orch) {}

        bool hasStatsCountModeAttr() const
        {
            return m_attr_val_map.find(SAI_ICMP_ECHO_SESSION_ATTR_STATS_COUNT_MODE) != m_attr_val_map.end();
        }

        int32_t getStatsCountModeAttr() const
        {
            auto it = m_attr_val_map.find(SAI_ICMP_ECHO_SESSION_ATTR_STATS_COUNT_MODE);
            return (it == m_attr_val_map.end()) ? -1 : it->second.s32;
        }
    };

    TEST_F(IcmpOrchStatsCountModeTest, DoCreate_doesNotApplyStatsCountModeWhenCapabilityUnresolved)
    {
        icmporch_sai_wrap_ut::IcmpSaiHookGuard g(icmporch_sai_wrap_ut::setIcmpSaiHookMetadataNull);
        IcmpOrch icmpOrch(m_app_db.get(), APP_ICMP_ECHO_SESSION_TABLE_NAME,
                TableConnector(m_state_db.get(), STATE_ICMP_ECHO_SESSION_TABLE_NAME));
        TestableIcmpSaiSessionHandler h(icmpOrch);
        ASSERT_EQ(h.init(sai_icmp_echo_api, "default:default:5000:NORMAL"),
                SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
        EXPECT_EQ(h.create(makeMinimalIcmpSessionFvs()), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
        EXPECT_FALSE(h.hasStatsCountModeAttr());
    }

    TEST_F(IcmpOrchStatsCountModeTest, DoCreate_appliesPacketAndByteStatsCountModeWhenReported)
    {
        icmporch_sai_wrap_ut::IcmpSaiHookGuard g(
                icmporch_sai_wrap_ut::setIcmpSaiHookQueryEnumPacketAndByteOnly);
        IcmpOrch icmpOrch(m_app_db.get(), APP_ICMP_ECHO_SESSION_TABLE_NAME,
                TableConnector(m_state_db.get(), STATE_ICMP_ECHO_SESSION_TABLE_NAME));
        TestableIcmpSaiSessionHandler h(icmpOrch);
        ASSERT_EQ(h.init(sai_icmp_echo_api, "default:default:5000:NORMAL"),
                SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
        EXPECT_EQ(h.create(makeMinimalIcmpSessionFvs()), SaiOffloadHandlerStatus::SUCCESS_VALID_ENTRY);
        EXPECT_TRUE(h.hasStatsCountModeAttr());
        EXPECT_EQ(h.getStatsCountModeAttr(), SAI_STATS_COUNT_MODE_PACKET_AND_BYTE);
    }

    TEST_F(IcmpOrchStatsCountModeTest, CountersState_SelectiveModeUsesInOutNameMapFields)
    {
        IcmpOrch icmpOrch(m_app_db.get(), APP_ICMP_ECHO_SESSION_TABLE_NAME,
                TableConnector(m_state_db.get(), STATE_ICMP_ECHO_SESSION_TABLE_NAME));
        ASSERT_TRUE(icmpOrch.create_icmp_session("default:default:5100:NORMAL",
                makeMinimalIcmpSessionFvs()));

        icmpOrch.setCountersState(true);

        std::string value;
        EXPECT_TRUE(nameMapHasField("default:default:5100:NORMAL|IN", value));
        EXPECT_TRUE(nameMapHasField("default:default:5100:NORMAL|OUT", value));
        EXPECT_FALSE(nameMapHasField("default:default:5100:NORMAL", value));

        icmpOrch.setCountersState(false);
        EXPECT_FALSE(nameMapHasField("default:default:5100:NORMAL|IN", value));
        EXPECT_FALSE(nameMapHasField("default:default:5100:NORMAL|OUT", value));
    }

    TEST_F(IcmpOrchStatsCountModeTest, CountersState_NativeFallbackUsesSessionKeyNameMapField)
    {
        hftel_is_supported_ut::SaiHookGuard g(
                hftel_is_supported_ut::setSaiHookIcmpSessionCapabilityQueryFail);
        IcmpOrch icmpOrch(m_app_db.get(), APP_ICMP_ECHO_SESSION_TABLE_NAME,
                TableConnector(m_state_db.get(), STATE_ICMP_ECHO_SESSION_TABLE_NAME));
        ASSERT_TRUE(icmpOrch.create_icmp_session("default:default:5200:NORMAL",
                makeMinimalIcmpSessionFvs()));

        icmpOrch.setCountersState(true);

        std::string value;
        EXPECT_TRUE(nameMapHasField("default:default:5200:NORMAL", value));
        EXPECT_FALSE(nameMapHasField("default:default:5200:NORMAL|IN", value));
        EXPECT_FALSE(nameMapHasField("default:default:5200:NORMAL|OUT", value));
    }

    TEST_F(IcmpOrchStatsCountModeTest, CountersState_TraditionalModePendingCountersDrainAfterVidToRid)
    {
        gTraditionalFlexCounter = true;
        // Traditional flex counter mode writes through the global flex-counter
        // producer tables; allocate them (mirrors orchagent init ordering).
        initFlexCounterTables();

        IcmpOrch icmpOrch(m_app_db.get(), APP_ICMP_ECHO_SESSION_TABLE_NAME,
                TableConnector(m_state_db.get(), STATE_ICMP_ECHO_SESSION_TABLE_NAME));
        ASSERT_TRUE(icmpOrch.create_icmp_session("default:default:5300:NORMAL",
                makeMinimalIcmpSessionFvs()));

        icmpOrch.setCountersState(true);
        ASSERT_FALSE(icmpOrch.m_stats_handler->m_pending_counters.empty());

        // A session registers one counter per selective variant (IN/OUT), so
        // resolve VID->RID for every pending OID (as syncd would) before draining.
        std::vector<sai_object_id_t> pending_oids;
        for (const auto& kv : icmpOrch.m_stats_handler->m_pending_counters)
        {
            pending_oids.push_back(kv.first);
        }
        for (auto pending_oid : pending_oids)
        {
            addVidtOridMap(pending_oid);
        }
        icmpOrch.m_stats_handler->processPending();

        EXPECT_TRUE(icmpOrch.m_stats_handler->m_pending_counters.empty());
    }
} // namespace icmporch_test
