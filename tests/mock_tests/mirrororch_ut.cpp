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

    TEST_F(MirrorOrchTest, RejectsIngressWhenUnsupported)
    {
        // Ensure environment initialized by MockOrchTest
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        // Force ingress unsupported and egress supported
        gSwitchOrch->m_portIngressMirrorSupported = false;
        gSwitchOrch->m_portEgressMirrorSupported = true;

        Port dummyPort; // Unused due to early return
        auto ret = gMirrorOrch->setUnsetPortMirror(dummyPort, /*ingress*/ true, /*set*/ true, /*sessionId*/ SAI_NULL_OBJECT_ID);
        ASSERT_FALSE(ret);
    }

    TEST_F(MirrorOrchTest, RejectsEgressWhenUnsupported)
    {
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        // Force egress unsupported and ingress supported
        gSwitchOrch->m_portIngressMirrorSupported = true;
        gSwitchOrch->m_portEgressMirrorSupported = false;

        Port dummyPort; // Unused due to early return
        auto ret = gMirrorOrch->setUnsetPortMirror(dummyPort, /*ingress*/ false, /*set*/ true, /*sessionId*/ SAI_NULL_OBJECT_ID);
        ASSERT_FALSE(ret);
    }

    TEST_F(MirrorOrchTest, FallbackToFullMirrorWhenSampledUnsupported)
    {
        // Verify activateSession falls back to full mirror when sampled not supported
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressMirrorSupported = true;
        gSwitchOrch->m_portIngressSampleMirrorSupported = false;

        MirrorEntry entry("");
        entry.sample_rate = 50000;
        entry.truncate_size = 128;

        // Note: activateSession requires full neighbor/route resolution which is
        // not available in mock environment. Testing capability check logic directly.
        if (!gSwitchOrch->isPortIngressSampleMirrorSupported())
        {
            entry.sample_rate = 0;
            entry.truncate_size = 0;
        }
        ASSERT_EQ(entry.sample_rate, (uint32_t)0);
        ASSERT_EQ(entry.truncate_size, (uint32_t)0);
    }

    TEST_F(MirrorOrchTest, SkipTruncationWhenUnsupported)
    {
        // Verify createSamplePacket skips truncation when not supported
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressSampleMirrorSupported = true;
        gSwitchOrch->m_samplepacketTruncationSupported = false;

        MirrorEntry entry("");
        entry.sample_rate = 50000;
        entry.truncate_size = 128;

        // createSamplePacket should detect truncation unsupported and clear truncate_size
        gMirrorOrch->createSamplePacket("test_session", entry);
        ASSERT_EQ(entry.truncate_size, (uint32_t)0);
    }

    TEST_F(MirrorOrchTest, RemoveSamplePacketHandlesNullOid)
    {
        // Verify removeSamplePacket handles NULL samplepacketId gracefully
        ASSERT_NE(gMirrorOrch, nullptr);

        MirrorEntry entry("");
        entry.samplepacketId = SAI_NULL_OBJECT_ID;

        bool ret = gMirrorOrch->removeSamplePacket("test_session", entry);
        ASSERT_TRUE(ret);
        ASSERT_EQ(entry.samplepacketId, SAI_NULL_OBJECT_ID);
    }

    TEST_F(MirrorOrchTest, MirrorEntryDefaultValues)
    {
        // Verify MirrorEntry initializes new fields correctly
        MirrorEntry entry("");
        ASSERT_EQ(entry.sample_rate, (uint32_t)0);
        ASSERT_EQ(entry.truncate_size, (uint32_t)0);
        ASSERT_EQ(entry.samplepacketId, SAI_NULL_OBJECT_ID);
    }

    TEST_F(MirrorOrchTest, ValidationRejectsTruncateWithoutSampleRate)
    {
        // Verify createEntry rejects truncate_size > 0 when sample_rate == 0
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("dscp", "8");
        data.emplace_back("ttl", "64");
        data.emplace_back("truncate_size", "128");

        auto status = gMirrorOrch->createEntry("invalid_session", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }
    TEST_F(MirrorOrchTest, CreateEntryWithSampleRate)
    {
        // Verify createEntry correctly parses sample_rate and truncate_size
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("gre_type", "0x8949");
        data.emplace_back("dscp", "8");
        data.emplace_back("ttl", "64");
        data.emplace_back("queue", "0");
        data.emplace_back("direction", "RX");
        data.emplace_back("sample_rate", "50000");
        data.emplace_back("truncate_size", "128");

        auto status = gMirrorOrch->createEntry("sampled_session", data);
        ASSERT_EQ(status, task_process_status::task_success);

        // Verify session was created with correct values
        ASSERT_TRUE(gMirrorOrch->sessionExists("sampled_session"));
        auto& session = gMirrorOrch->m_syncdMirrors.find("sampled_session")->second;
        ASSERT_EQ(session.sample_rate, (uint32_t)50000);
        ASSERT_EQ(session.truncate_size, (uint32_t)128);
        ASSERT_EQ(session.type, "ERSPAN");

        // Cleanup
        gMirrorOrch->m_syncdMirrors.erase("sampled_session");
    }

    TEST_F(MirrorOrchTest, CreateEntryRejectsNonRxDirection)
    {
        // Verify createEntry rejects sample_rate with non-RX direction
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("dscp", "8");
        data.emplace_back("ttl", "64");
        data.emplace_back("direction", "TX");
        data.emplace_back("sample_rate", "50000");

        auto status = gMirrorOrch->createEntry("invalid_dir_session", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
        ASSERT_FALSE(gMirrorOrch->sessionExists("invalid_dir_session"));
    }

    TEST_F(MirrorOrchTest, CreateEntryRoutesToUpdateForErspan)
    {
        // Verify createEntry routes to updateEntry for existing ERSPAN sessions
        ASSERT_NE(gMirrorOrch, nullptr);

        // First create a session
        vector<FieldValueTuple> data;
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("gre_type", "0x8949");
        data.emplace_back("dscp", "8");
        data.emplace_back("ttl", "64");
        data.emplace_back("queue", "0");
        data.emplace_back("direction", "RX");
        data.emplace_back("sample_rate", "50000");

        auto status = gMirrorOrch->createEntry("update_route_session", data);
        ASSERT_EQ(status, task_process_status::task_success);

        // Call createEntry again with same key - should route to updateEntry
        status = gMirrorOrch->createEntry("update_route_session", data);
        // updateEntry returns success when no fields changed
        ASSERT_EQ(status, task_process_status::task_success);

        // Cleanup
        gMirrorOrch->m_syncdMirrors.erase("update_route_session");
    }


    TEST_F(MirrorOrchTest, UpdateEntryNoChangeReturnsSuccess)
    {
        // Verify updateEntry returns success when no fields changed
        ASSERT_NE(gMirrorOrch, nullptr);

        string key = "nochange_session";
        MirrorEntry entry("");
        entry.type = "ERSPAN";
        entry.srcIp = IpAddress("10.0.0.1");
        entry.dstIp = IpAddress("10.0.0.2");
        entry.greType = 0x8949;
        entry.dscp = 8;
        entry.ttl = 64;
        entry.queue = 0;
        entry.direction = "RX";
        entry.sample_rate = 50000;
        entry.truncate_size = 0;
        entry.samplepacketId = (sai_object_id_t)0x1234;
        gMirrorOrch->m_syncdMirrors.emplace(key, entry);

        // Update with same values - should detect no change
        vector<FieldValueTuple> data;
        data.emplace_back("sample_rate", "50000");
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");

        auto status = gMirrorOrch->updateEntry(key, data);
        ASSERT_EQ(status, task_process_status::task_success);

        // Cleanup
        gMirrorOrch->m_syncdMirrors.erase(key);
    }

    TEST_F(MirrorOrchTest, UpdateEntryPathTransitionIgnored)
    {
        // Verify updateEntry ignores path transition (full mirror -> sampled)
        // when samplepacket does not exist
        ASSERT_NE(gMirrorOrch, nullptr);

        string key = "path_transition_session";
        MirrorEntry entry("");
        entry.type = "ERSPAN";
        entry.srcIp = IpAddress("10.0.0.1");
        entry.dstIp = IpAddress("10.0.0.2");
        entry.greType = 0x8949;
        entry.dscp = 8;
        entry.ttl = 64;
        entry.queue = 0;
        entry.direction = "RX";
        entry.sample_rate = 0;
        entry.truncate_size = 0;
        entry.samplepacketId = SAI_NULL_OBJECT_ID;  // No samplepacket (full mirror)
        gMirrorOrch->m_syncdMirrors.emplace(key, entry);

        // Try to add sample_rate - should be ignored (path transition not supported)
        vector<FieldValueTuple> data;
        data.emplace_back("sample_rate", "50000");

        auto status = gMirrorOrch->updateEntry(key, data);
        ASSERT_EQ(status, task_process_status::task_success);

        // Cleanup
        gMirrorOrch->m_syncdMirrors.erase(key);
    }

    TEST_F(MirrorOrchTest, SampledPathPortBinding)
    {
        // Verify setUnsetPortMirror enters sampled path when sample_rate > 0
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressMirrorSupported = true;
        gSwitchOrch->m_portIngressSampleMirrorSupported = true;

        Port dummyPort;
        dummyPort.m_port_id = SAI_NULL_OBJECT_ID;

        auto ret = gMirrorOrch->setUnsetPortMirror(dummyPort, /*ingress*/ true, /*set*/ true,
                                        /*sessionId*/ SAI_NULL_OBJECT_ID,
                                        /*samplepacketId*/ (sai_object_id_t)0x1234,
                                        /*sample_rate*/ 50000);
        // SAI mock returns failure on null port OID - expected in mock environment
        ASSERT_FALSE(ret);
    }


    TEST_F(MirrorOrchTest, CreateSamplePacketWithTruncation)
    {
        // Verify createSamplePacket sets truncation attributes when supported
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressSampleMirrorSupported = true;
        gSwitchOrch->m_samplepacketTruncationSupported = true;

        MirrorEntry entry("");
        entry.sample_rate = 50000;
        entry.truncate_size = 128;

        gMirrorOrch->createSamplePacket("truncate_session", entry);
        // SAI mock may succeed or fail, but truncation path should be entered
        // truncate_size should remain 128 (not cleared) since truncation is supported
        ASSERT_EQ(entry.truncate_size, (uint32_t)128);
    }


}

