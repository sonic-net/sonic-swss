// Make selected privates visible for unit testing
#define private public
#include "directory.h"
#undef private

#define protected public
#include "orch.h"
#undef protected

#define private public
#include "switchorch.h"
#undef private

#include "portsorch.h"
#define private public
#include "mirrororch.h"
#undef private
#include "mock_orch_test.h"

namespace mirrororch_test
{
    using namespace mock_orch_test;

    class MirrorOrchTest : public MockOrchTest
    {
    };

    // --- Port mirror capability gating tests ---

    TEST_F(MirrorOrchTest, RejectsIngressWhenUnsupported)
    {
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressMirrorSupported = false;
        gSwitchOrch->m_portEgressMirrorSupported = true;

        Port dummyPort;
        auto ret = gMirrorOrch->setUnsetPortMirror(dummyPort, /*ingress*/ true, /*set*/ true, /*sessionId*/ SAI_NULL_OBJECT_ID);
        ASSERT_FALSE(ret);
    }

    TEST_F(MirrorOrchTest, RejectsEgressWhenUnsupported)
    {
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressMirrorSupported = true;
        gSwitchOrch->m_portEgressMirrorSupported = false;

        Port dummyPort;
        auto ret = gMirrorOrch->setUnsetPortMirror(dummyPort, /*ingress*/ false, /*set*/ true, /*sessionId*/ SAI_NULL_OBJECT_ID);
        ASSERT_FALSE(ret);
    }

    // --- Sampled mirror capability gating ---

    TEST_F(MirrorOrchTest, RejectsSampledMirrorWhenUnsupported)
    {
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressSampleMirrorSupported = false;

        Port dummyPort;
        auto ret = gMirrorOrch->setUnsetPortSampleMirror(
            dummyPort, /*ingress*/ true, /*set*/ true,
            /*sessionId*/ SAI_NULL_OBJECT_ID, /*samplePacketId*/ SAI_NULL_OBJECT_ID);
        ASSERT_FALSE(ret);
    }

    // --- ERSPAN ID capability gating ---

    TEST_F(MirrorOrchTest, ErspanIdCapabilityGateReportsCorrectly)
    {
        ASSERT_NE(gSwitchOrch, nullptr);

        // Default should be false (from initialization)
        gSwitchOrch->m_mirrorErspanSessionIdSupported = false;
        ASSERT_FALSE(gSwitchOrch->isMirrorErspanSessionIdSupported());

        gSwitchOrch->m_mirrorErspanSessionIdSupported = true;
        ASSERT_TRUE(gSwitchOrch->isMirrorErspanSessionIdSupported());
    }

    // --- Sampled mirroring direction validation ---

    TEST_F(MirrorOrchTest, SampledMirrorRejectsTxDirection)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("src_port", "Ethernet0");
        data.emplace_back("direction", "TX");
        data.emplace_back("sample_rate", "1000");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_sampled_tx", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchTest, SampledMirrorRejectsBothDirection)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("src_port", "Ethernet0");
        data.emplace_back("direction", "BOTH");
        data.emplace_back("sample_rate", "500");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_sampled_both", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchTest, SampledMirrorAcceptsRxDirection)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("direction", "RX");
        data.emplace_back("sample_rate", "1000");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_sampled_rx", data);
        // Should pass validation (may fail on route resolution, which is fine)
        ASSERT_NE(status, task_process_status::task_invalid_entry);
    }

    // --- Truncation validation (independent of sampling) ---

    TEST_F(MirrorOrchTest, TruncateSizeValidWithoutSampling)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("truncate_size", "128");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_trunc_no_sample", data);
        // Truncation without sampling is allowed
        ASSERT_NE(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchTest, TruncateSizeBelowMinRejected)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("truncate_size", "20");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_trunc_too_small", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    // --- SPAN session truncation (should work independently) ---

    TEST_F(MirrorOrchTest, SpanTruncationAllowed)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("direction", "RX");
        data.emplace_back("truncate_size", "24");
        data.emplace_back("type", "SPAN");

        auto status = gMirrorOrch->createEntry("test_span_trunc", data);
        // SPAN truncation: 24 < 38 (ERSPAN min) but >= 20 (SPAN min) and 4-byte aligned
        ASSERT_NE(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchTest, SpanTruncationBelowMinRejected)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("direction", "RX");
        data.emplace_back("truncate_size", "16");
        data.emplace_back("type", "SPAN");

        auto status = gMirrorOrch->createEntry("test_span_trunc_small", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchTest, TruncateSizeNotAlignedRejected)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("truncate_size", "65");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_trunc_misaligned", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    // --- Congestion mode validation ---

    TEST_F(MirrorOrchTest, CongestionModeInvalidRejected)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("congestion_mode", "invalid_mode");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_bad_congestion", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchTest, CongestionModeIndependentAccepted)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("congestion_mode", "independent");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_congestion_ind", data);
        ASSERT_NE(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchTest, CongestionModeCorrelatedAccepted)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("congestion_mode", "correlated");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_congestion_cor", data);
        ASSERT_NE(status, task_process_status::task_invalid_entry);
    }

    // --- ERSPAN ID validation ---

    TEST_F(MirrorOrchTest, ErspanIdMaxExceededRejected)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("erspan_id", "1024");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_erspan_id_max", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchTest, ErspanIdValidAccepted)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("erspan_id", "100");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_erspan_id_valid", data);
        ASSERT_NE(status, task_process_status::task_invalid_entry);
    }

    // --- Loop detection ---

    TEST_F(MirrorOrchTest, SpanLoopDetected)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("dst_port", "Ethernet0");
        data.emplace_back("src_port", "Ethernet0");
        data.emplace_back("direction", "RX");
        data.emplace_back("type", "SPAN");

        auto status = gMirrorOrch->createEntry("test_span_loop", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchTest, ErspanSrcDstSameIpRejected)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.1");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_erspan_loop", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    // --- updateEntry tests ---

    TEST_F(MirrorOrchTest, UpdateEntryOnNonexistentSessionFails)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("truncate_size", "128");

        auto status = gMirrorOrch->updateEntry("nonexistent_session", data);
        ASSERT_EQ(status, task_process_status::task_failed);
    }

    TEST_F(MirrorOrchTest, UpdateEntryErspanIdOverMaxRejected)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        // First create a session
        vector<FieldValueTuple> create_data;
        create_data.emplace_back("src_ip", "10.0.0.1");
        create_data.emplace_back("dst_ip", "10.0.0.2");
        create_data.emplace_back("type", "ERSPAN");
        gMirrorOrch->createEntry("test_update_erspan", create_data);

        // Now try to update erspan_id > 1023
        vector<FieldValueTuple> update_data;
        update_data.emplace_back("erspan_id", "2000");

        auto status = gMirrorOrch->updateEntry("test_update_erspan", update_data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchTest, UpdateEntryMutableFieldsSuccess)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        // Create a session
        vector<FieldValueTuple> create_data;
        create_data.emplace_back("src_ip", "10.0.0.1");
        create_data.emplace_back("dst_ip", "10.0.0.2");
        create_data.emplace_back("type", "ERSPAN");
        gMirrorOrch->createEntry("test_mutable_update", create_data);

        // Update truncate_size (mutable)
        vector<FieldValueTuple> update_data;
        update_data.emplace_back("truncate_size", "128");

        auto status = gMirrorOrch->updateEntry("test_mutable_update", update_data);
        ASSERT_EQ(status, task_process_status::task_success);

        // Verify the value was stored
        auto it = gMirrorOrch->m_syncdMirrors.find("test_mutable_update");
        ASSERT_NE(it, gMirrorOrch->m_syncdMirrors.end());
        ASSERT_EQ(it->second.truncate_size, 128);
    }

    TEST_F(MirrorOrchTest, UpdateEntryImmutableFieldTriggersRebuild)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        // Create a session
        vector<FieldValueTuple> create_data;
        create_data.emplace_back("src_ip", "10.0.0.1");
        create_data.emplace_back("dst_ip", "10.0.0.2");
        create_data.emplace_back("type", "ERSPAN");
        gMirrorOrch->createEntry("test_immutable_update", create_data);

        ASSERT_TRUE(gMirrorOrch->m_syncdMirrors.count("test_immutable_update") > 0);

        // Update src_ip (immutable) — triggers teardown + recreate
        vector<FieldValueTuple> update_data;
        update_data.emplace_back("src_ip", "10.0.0.3");
        update_data.emplace_back("dst_ip", "10.0.0.4");
        update_data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->updateEntry("test_immutable_update", update_data);
        // After teardown and recreate, the session should still exist
        // (createEntry called internally)
        ASSERT_NE(status, task_process_status::task_invalid_entry);
    }

    // --- MirrorEntry field initialization ---

    TEST_F(MirrorOrchTest, MirrorEntryDefaultsSampleRateZero)
    {
        MirrorEntry entry("");
        ASSERT_EQ(entry.sample_rate, 0u);
        ASSERT_EQ(entry.samplePacketId, SAI_NULL_OBJECT_ID);
    }

    // --- Duplicate session handling (routes through updateEntry) ---

    TEST_F(MirrorOrchTest, DuplicateCreateRoutesToUpdate)
    {
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_duplicate", data);
        ASSERT_NE(status, task_process_status::task_invalid_entry);

        // Second create should route to updateEntry
        vector<FieldValueTuple> data2;
        data2.emplace_back("truncate_size", "64");
        auto status2 = gMirrorOrch->createEntry("test_duplicate", data2);
        ASSERT_EQ(status2, task_process_status::task_success);
    }

    // --- Samplepacket truncation capability gating ---

    TEST_F(MirrorOrchTest, SamplepacketTruncationCapabilityGating)
    {
        ASSERT_NE(gSwitchOrch, nullptr);

        gSwitchOrch->m_samplepacketTruncationSupported = false;
        ASSERT_FALSE(gSwitchOrch->isSamplepacketTruncationSupported());

        gSwitchOrch->m_samplepacketTruncationSupported = true;
        ASSERT_TRUE(gSwitchOrch->isSamplepacketTruncationSupported());
    }
}

