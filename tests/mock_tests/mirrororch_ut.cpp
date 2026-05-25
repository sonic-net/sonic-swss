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
#include "mock_table.h"

extern sai_mirror_api_t *sai_mirror_api;
extern sai_counter_api_t *sai_counter_api;

namespace mirrororch_test
{
    using namespace mock_orch_test;

    // --- Mock SAI Mirror API to prevent segfault during session activation ---
    sai_mirror_api_t ut_sai_mirror_api;
    sai_mirror_api_t *pold_sai_mirror_api = nullptr;

    sai_counter_api_t ut_sai_counter_api;
    sai_counter_api_t *pold_sai_counter_api = nullptr;

    sai_status_t mock_create_mirror_session(
        _Out_ sai_object_id_t *session_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        static sai_object_id_t next_oid = 0x100000;
        *session_id = next_oid++;
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t mock_remove_mirror_session(
        _In_ sai_object_id_t session_id)
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t mock_set_mirror_session_attribute(
        _In_ sai_object_id_t session_id,
        _In_ const sai_attribute_t *attr)
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t mock_create_counter(
        _Out_ sai_object_id_t *counter_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        return SAI_STATUS_NOT_SUPPORTED;
    }

    sai_status_t mock_remove_counter(
        _In_ sai_object_id_t counter_id)
    {
        return SAI_STATUS_SUCCESS;
    }

    class MirrorOrchTest : public MockOrchTest
    {
    };

    // Extended fixture for tests that need real ports + SAI mirror mock
    class MirrorOrchPortTest : public MockOrchTest
    {
    protected:
        void ApplyInitialConfigs() override
        {
            // Register minimal port entries directly (avoids SAI port/hostif creation)
            Port p0("Ethernet0", Port::PHY);
            p0.m_port_id = 0x10000001;
            gPortsOrch->setPort("Ethernet0", p0);

            Port p4("Ethernet4", Port::PHY);
            p4.m_port_id = 0x10000002;
            gPortsOrch->setPort("Ethernet4", p4);

            Port p8("Ethernet8", Port::PHY);
            p8.m_port_id = 0x10000003;
            gPortsOrch->setPort("Ethernet8", p8);

            // Mock SAI mirror API (NULL in mock tests — SAI_API_MIRROR not queried)
            pold_sai_mirror_api = sai_mirror_api;
            memset(&ut_sai_mirror_api, 0, sizeof(ut_sai_mirror_api));
            ut_sai_mirror_api.create_mirror_session = mock_create_mirror_session;
            ut_sai_mirror_api.remove_mirror_session = mock_remove_mirror_session;
            ut_sai_mirror_api.set_mirror_session_attribute = mock_set_mirror_session_attribute;
            sai_mirror_api = &ut_sai_mirror_api;

            // Mock SAI counter API (return NOT_SUPPORTED to skip counter attachment)
            pold_sai_counter_api = sai_counter_api;
            memset(&ut_sai_counter_api, 0, sizeof(ut_sai_counter_api));
            if (sai_counter_api)
            {
                ut_sai_counter_api = *sai_counter_api;
            }
            ut_sai_counter_api.create_counter = mock_create_counter;
            ut_sai_counter_api.remove_counter = mock_remove_counter;
            sai_counter_api = &ut_sai_counter_api;
        }

        void PreTearDown() override
        {
            sai_mirror_api = pold_sai_mirror_api;
            pold_sai_mirror_api = nullptr;

            sai_counter_api = pold_sai_counter_api;
            pold_sai_counter_api = nullptr;
        }
    };

    // --- Port mirror capability gating tests (UPSW-1733) ---

    TEST_F(MirrorOrchTest, RejectsIngressWhenUnsupported)
    {
        // UPSW-1733: Direction capability gating — ingress mirror unsupported
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
        // UPSW-1733: Direction capability gating — egress mirror unsupported
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressMirrorSupported = true;
        gSwitchOrch->m_portEgressMirrorSupported = false;

        Port dummyPort;
        auto ret = gMirrorOrch->setUnsetPortMirror(dummyPort, /*ingress*/ false, /*set*/ true, /*sessionId*/ SAI_NULL_OBJECT_ID);
        ASSERT_FALSE(ret);
    }

    // --- Sampled mirror capability gating (UPSW-1760) ---

    TEST_F(MirrorOrchTest, RejectsSampledMirrorWhenUnsupported)
    {
        // UPSW-1760: Sampled mirror rejected when platform doesn't support it
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressSampleMirrorSupported = false;

        Port dummyPort;
        auto ret = gMirrorOrch->setUnsetPortSampleMirror(
            dummyPort, /*ingress*/ true, /*set*/ true,
            /*sessionId*/ SAI_NULL_OBJECT_ID, /*samplePacketId*/ SAI_NULL_OBJECT_ID);
        ASSERT_FALSE(ret);
    }

    // --- ERSPAN ID capability gating (UPSW-1736) ---

    TEST_F(MirrorOrchTest, ErspanIdCapabilityGateReportsCorrectly)
    {
        // UPSW-1736: ERSPAN session ID capability query
        ASSERT_NE(gSwitchOrch, nullptr);

        // Default should be false (from initialization)
        gSwitchOrch->m_mirrorErspanSessionIdSupported = false;
        ASSERT_FALSE(gSwitchOrch->isMirrorErspanSessionIdSupported());

        gSwitchOrch->m_mirrorErspanSessionIdSupported = true;
        ASSERT_TRUE(gSwitchOrch->isMirrorErspanSessionIdSupported());
    }

    // --- Sampled mirroring direction validation (UPSW-1760) ---

    TEST_F(MirrorOrchTest, SampledMirrorRejectsTxDirection)
    {
        // UPSW-1760: Sampled mirroring only supports RX (ingress)
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("direction", "TX");
        data.emplace_back("sample_rate", "1000");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_sampled_tx", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchTest, SampledMirrorRejectsBothDirection)
    {
        // UPSW-1760: Sampled mirroring rejects BOTH direction
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("direction", "BOTH");
        data.emplace_back("sample_rate", "500");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_sampled_both", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchTest, SampledMirrorAcceptsRxDirection)
    {
        // UPSW-1760: Sampled mirroring accepts RX direction
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

    // --- Truncation validation (UPSW-1735) ---

    TEST_F(MirrorOrchTest, TruncateSizeValidWithoutSampling)
    {
        // UPSW-1735: Valid truncation size accepted for ERSPAN
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("truncate_size", "128");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_trunc_no_sample", data);
        ASSERT_NE(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchTest, TruncateSizeBelowMinRejected)
    {
        // UPSW-1735: Truncation below minimum rejected
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("truncate_size", "20");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_trunc_too_small", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    // --- SPAN session truncation (UPSW-1735) ---

    TEST_F(MirrorOrchTest, SpanTruncationAllowed)
    {
        // UPSW-1735: SPAN truncation with valid 4-byte aligned size
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("direction", "RX");
        data.emplace_back("truncate_size", "24");
        data.emplace_back("type", "SPAN");

        auto status = gMirrorOrch->createEntry("test_span_trunc", data);
        ASSERT_NE(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchTest, SpanTruncationBelowMinRejected)
    {
        // UPSW-1735: SPAN truncation below minimum rejected
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
        // UPSW-1735: Non-4-byte-aligned truncation size rejected
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("truncate_size", "65");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_trunc_misaligned", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    // --- Congestion mode validation (UPSW-1737) ---

    TEST_F(MirrorOrchTest, CongestionModeInvalidRejected)
    {
        // UPSW-1737: Invalid congestion mode string rejected
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
        // UPSW-1737: "independent" congestion mode accepted
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
        // UPSW-1737: "correlated" congestion mode accepted
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("congestion_mode", "correlated");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_congestion_cor", data);
        ASSERT_NE(status, task_process_status::task_invalid_entry);
    }

    // --- ERSPAN ID validation (UPSW-1736) ---

    TEST_F(MirrorOrchTest, ErspanIdMaxExceededRejected)
    {
        // UPSW-1736: ERSPAN ID > 1023 rejected
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
        // UPSW-1736: Valid ERSPAN ID (0-1023) accepted
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("erspan_id", "100");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_erspan_id_valid", data);
        ASSERT_NE(status, task_process_status::task_invalid_entry);
    }

    // --- Loop detection (UPSW-1747) ---

    TEST_F(MirrorOrchTest, SpanLoopDetected)
    {
        // UPSW-1747: SPAN dst_port == src_port loop detection
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
        // UPSW-1747: ERSPAN src_ip == dst_ip loop detection
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.1");
        data.emplace_back("type", "ERSPAN");

        auto status = gMirrorOrch->createEntry("test_erspan_loop", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    // --- updateEntry tests (UPSW-1735) ---

    TEST_F(MirrorOrchTest, UpdateEntryOnNonexistentSessionFails)
    {
        // UPSW-1735: Update non-existent session returns failure
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("truncate_size", "128");

        auto status = gMirrorOrch->updateEntry("nonexistent_session", data);
        ASSERT_EQ(status, task_process_status::task_failed);
    }

    TEST_F(MirrorOrchTest, UpdateEntryErspanIdOverMaxRejected)
    {
        // UPSW-1736: updateEntry rejects ERSPAN ID > 1023
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
        // UPSW-1735: In-place update of mutable fields (truncate_size) succeeds
        ASSERT_NE(gMirrorOrch, nullptr);

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
        // UPSW-1734: Changing immutable field (src_ip) triggers session rebuild
        ASSERT_NE(gMirrorOrch, nullptr);

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

    // --- MirrorEntry field initialization (UPSW-1760) ---

    TEST_F(MirrorOrchTest, MirrorEntryDefaultsSampleRateZero)
    {
        // UPSW-1760: Default sample_rate is zero (no sampling)
        MirrorEntry entry("");
        ASSERT_EQ(entry.sample_rate, 0u);
        ASSERT_EQ(entry.samplePacketId, SAI_NULL_OBJECT_ID);
    }

    // --- Duplicate session handling (UPSW-1735: routes through updateEntry) ---

    TEST_F(MirrorOrchTest, DuplicateCreateRoutesToUpdate)
    {
        // UPSW-1735: Duplicate CREATE routes to updateEntry for in-place update
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

    // --- Samplepacket truncation capability gating (UPSW-1760) ---

    TEST_F(MirrorOrchTest, SamplepacketTruncationCapabilityGating)
    {
        // UPSW-1760: Samplepacket truncation capability query
        ASSERT_NE(gSwitchOrch, nullptr);

        gSwitchOrch->m_samplepacketTruncationSupported = false;
        ASSERT_FALSE(gSwitchOrch->isSamplepacketTruncationSupported());

        gSwitchOrch->m_samplepacketTruncationSupported = true;
        ASSERT_TRUE(gSwitchOrch->isSamplepacketTruncationSupported());
    }

    // --- Direct path (monitor_port) tests — use MirrorOrchPortTest fixture ---

    TEST_F(MirrorOrchPortTest, MonitorPortParsedInCreateEntry)
    {
        // UPSW-1755: monitor_port field enables direct activation path
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("monitor_port", "Ethernet0");

        auto status = gMirrorOrch->createEntry("test_monitor_port", data);
        ASSERT_NE(status, task_process_status::task_invalid_entry);

        auto it = gMirrorOrch->m_syncdMirrors.find("test_monitor_port");
        ASSERT_NE(it, gMirrorOrch->m_syncdMirrors.end());
        ASSERT_EQ(it->second.monitor_port_cfg, "Ethernet0");
        ASSERT_TRUE(it->second.direct_path);
    }

    TEST_F(MirrorOrchPortTest, DstMacParsedInCreateEntry)
    {
        // UPSW-1754: dst_mac for outer GRE encapsulation header
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("monitor_port", "Ethernet4");
        data.emplace_back("dst_mac", "AA:BB:CC:DD:EE:FF");

        auto status = gMirrorOrch->createEntry("test_dst_mac", data);
        ASSERT_NE(status, task_process_status::task_invalid_entry);

        auto it = gMirrorOrch->m_syncdMirrors.find("test_dst_mac");
        ASSERT_NE(it, gMirrorOrch->m_syncdMirrors.end());
        ASSERT_TRUE(it->second.direct_path);
        ASSERT_EQ(it->second.dst_mac_cfg.to_string(), "aa:bb:cc:dd:ee:ff");
    }

    TEST_F(MirrorOrchPortTest, InvalidMonitorPortRejected)
    {
        // UPSW-1755: Invalid monitor_port value must be rejected at parse time
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("monitor_port", "InvalidPort99");

        auto status = gMirrorOrch->createEntry("test_invalid_monitor", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchPortTest, InvalidDstMacRejected)
    {
        // UPSW-1754: Invalid dst_mac format must be rejected
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("dst_mac", "not-a-mac");

        auto status = gMirrorOrch->createEntry("test_invalid_mac", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchPortTest, DirectPathSkipsRouteOrchAttach)
    {
        // UPSW-1755: Direct path sessions bypass route/neighbor resolution
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("monitor_port", "Ethernet8");
        data.emplace_back("direction", "RX");

        auto status = gMirrorOrch->createEntry("test_direct_no_attach", data);
        ASSERT_NE(status, task_process_status::task_invalid_entry);

        auto it = gMirrorOrch->m_syncdMirrors.find("test_direct_no_attach");
        ASSERT_NE(it, gMirrorOrch->m_syncdMirrors.end());
        ASSERT_TRUE(it->second.direct_path);
    }

    TEST_F(MirrorOrchPortTest, MonitorPortInSrcPortLoopRejected)
    {
        // UPSW-1755: monitor_port overlapping src_port must be rejected (loop)
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("src_port", "Ethernet0,Ethernet4");
        data.emplace_back("monitor_port", "Ethernet4");
        data.emplace_back("direction", "RX");

        auto status = gMirrorOrch->createEntry("test_loop_erspan", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    TEST_F(MirrorOrchPortTest, MirrorEntryDefaultsDirectPathFalse)
    {
        // UPSW-1755: MirrorEntry should default direct_path to false (resolved path)
        MirrorEntry entry("");
        ASSERT_FALSE(entry.direct_path);
        ASSERT_TRUE(entry.monitor_port_cfg.empty());
    }
}

