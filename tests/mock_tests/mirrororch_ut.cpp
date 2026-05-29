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

    TEST_F(MirrorOrchTest, CreateEntryRejectsSampledWhenUnsupported)
    {
        // Verify createEntry rejects sample_rate > 0 when platform does not
        // advertise PORT_INGRESS_SAMPLE_MIRROR capability.
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressMirrorSupported = true;
        gSwitchOrch->m_portIngressSampleMirrorSupported = false;
        gSwitchOrch->m_samplepacketTruncationSupported = true;

        std::vector<swss::FieldValueTuple> data;
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("gre_type", "0x88be");
        data.emplace_back("dscp", "0");
        data.emplace_back("ttl", "64");
        data.emplace_back("queue", "0");
        data.emplace_back("src_port", "Ethernet0");
        data.emplace_back("direction", "RX");
        data.emplace_back("sample_rate", "50000");

        auto status = gMirrorOrch->createEntry("test_reject_sampled", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);

        // No session should have been registered
        ASSERT_EQ(gMirrorOrch->m_syncdMirrors.count("test_reject_sampled"), (size_t)0);
    }

    TEST_F(MirrorOrchTest, CreateEntryRejectsTruncationWhenUnsupported)
    {
        // Verify createEntry rejects truncate_size > 0 when platform does not
        // advertise SAMPLEPACKET_TRUNCATION capability.
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressMirrorSupported = true;
        gSwitchOrch->m_portIngressSampleMirrorSupported = true;
        gSwitchOrch->m_samplepacketTruncationSupported = false;

        std::vector<swss::FieldValueTuple> data;
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("gre_type", "0x88be");
        data.emplace_back("dscp", "0");
        data.emplace_back("ttl", "64");
        data.emplace_back("queue", "0");
        data.emplace_back("src_port", "Ethernet0");
        data.emplace_back("direction", "RX");
        data.emplace_back("sample_rate", "50000");
        data.emplace_back("truncate_size", "128");

        auto status = gMirrorOrch->createEntry("test_reject_trunc", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);

        ASSERT_EQ(gMirrorOrch->m_syncdMirrors.count("test_reject_trunc"), (size_t)0);
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



    TEST_F(MirrorOrchTest, UpdateEntryAllFieldsBranches)
    {
        // Verify updateEntry covers all field comparison branches and
        // performs in-place update of both sample_rate and truncate_size
        ASSERT_NE(gMirrorOrch, nullptr);
        ASSERT_NE(gSwitchOrch, nullptr);

        gSwitchOrch->m_portIngressSampleMirrorSupported = true;
        gSwitchOrch->m_samplepacketTruncationSupported = true;

        // Create a real samplepacket OID via createSamplePacket
        string key = "update_all_fields_session";
        MirrorEntry entry("");
        entry.type = "ERSPAN";
        entry.srcIp = IpAddress("10.0.0.1");
        entry.dstIp = IpAddress("10.0.0.2");
        entry.greType = 0x8949;
        entry.dscp = 8;
        entry.ttl = 64;
        entry.queue = 0;
        entry.src_port = "Ethernet0";
        entry.direction = "RX";
        entry.policer = "";
        entry.sample_rate = 50000;
        entry.truncate_size = 128;
        entry.status = false;
        entry.refCount = 0;

        gMirrorOrch->createSamplePacket(key, entry);
        ASSERT_NE(entry.samplepacketId, SAI_NULL_OBJECT_ID);

        gMirrorOrch->m_syncdMirrors.emplace(key, entry);

        // Simulate HGETALL data: all fields present, only mutable fields changed
        vector<FieldValueTuple> data;
        data.emplace_back("sample_rate", "100000");
        data.emplace_back("truncate_size", "256");
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("gre_type", "0x8949");
        data.emplace_back("dscp", "8");
        data.emplace_back("ttl", "64");
        data.emplace_back("queue", "0");
        data.emplace_back("src_port", "Ethernet0");
        data.emplace_back("direction", "RX");
        data.emplace_back("policer", "");

        auto status = gMirrorOrch->updateEntry(key, data);
        ASSERT_EQ(status, task_process_status::task_success);

        // Verify both mutable fields were updated
        auto& updated = gMirrorOrch->m_syncdMirrors.find(key)->second;
        ASSERT_EQ(updated.sample_rate, (uint32_t)100000);
        ASSERT_EQ(updated.truncate_size, (uint32_t)256);

        // Cleanup
        sai_samplepacket_api->remove_samplepacket(updated.samplepacketId);
        gMirrorOrch->m_syncdMirrors.erase(key);
    }

    TEST_F(MirrorOrchTest, UpdateEntryNonExistentSession)
    {
        // Verify updateEntry returns invalid_entry for non-existent session
        ASSERT_NE(gMirrorOrch, nullptr);

        vector<FieldValueTuple> data;
        data.emplace_back("sample_rate", "50000");

        auto status = gMirrorOrch->updateEntry("non_existent_session", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
    }

    class MirrorOrchPortTest : public MirrorOrchTest
    {
        void ApplyInitialConfigs() override
        {
            Table port_table = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
            auto ports = ut_helper::getInitialSaiPorts();
            for (const auto &it : ports)
            {
                port_table.set(it.first, it.second);
            }
            port_table.set("PortConfigDone", { { "count", to_string(ports.size()) } });
            port_table.set("PortInitDone", { {} });

            gPortsOrch->addExistingData(&port_table);
            static_cast<Orch *>(gPortsOrch)->doTask();
        }
    };

    TEST_F(MirrorOrchPortTest, UpdateEntryImmutableFieldChange)
    {
        // Verify updateEntry detects immutable field changes and triggers delete+recreate
        ASSERT_NE(gMirrorOrch, nullptr);

        // Create session via createEntry (routeOrch->attach called)
        vector<FieldValueTuple> create_data;
        create_data.emplace_back("type", "ERSPAN");
        create_data.emplace_back("src_ip", "10.0.0.1");
        create_data.emplace_back("dst_ip", "10.0.0.2");
        create_data.emplace_back("gre_type", "0x8949");
        create_data.emplace_back("dscp", "8");
        create_data.emplace_back("ttl", "64");
        create_data.emplace_back("queue", "0");
        create_data.emplace_back("src_port", "Ethernet0");
        create_data.emplace_back("direction", "RX");
        create_data.emplace_back("sample_rate", "50000");

        auto status = gMirrorOrch->createEntry("immutable_test", create_data);
        ASSERT_EQ(status, task_process_status::task_success);

        // Change all immutable fields except direction and policer
        vector<FieldValueTuple> update_data;
        update_data.emplace_back("type", "ERSPAN");
        update_data.emplace_back("src_ip", "10.0.0.99");
        update_data.emplace_back("dst_ip", "10.0.0.99");
        update_data.emplace_back("gre_type", "0x1234");
        update_data.emplace_back("dscp", "16");
        update_data.emplace_back("ttl", "128");
        update_data.emplace_back("queue", "1");
        update_data.emplace_back("src_port", "Ethernet4");
        update_data.emplace_back("direction", "RX");
        update_data.emplace_back("sample_rate", "50000");

        status = gMirrorOrch->updateEntry("immutable_test", update_data);
        ASSERT_EQ(status, task_process_status::task_success);

        // Verify recreated session has new values
        auto& session = gMirrorOrch->m_syncdMirrors.find("immutable_test")->second;
        ASSERT_EQ(session.srcIp, IpAddress("10.0.0.99"));
        ASSERT_EQ(session.dstIp, IpAddress("10.0.0.99"));
        ASSERT_EQ(session.greType, (uint16_t)0x1234);
        ASSERT_EQ(session.dscp, (uint8_t)16);
        ASSERT_EQ(session.ttl, (uint8_t)128);
        ASSERT_EQ(session.queue, (uint8_t)1);
        ASSERT_EQ(session.src_port, "Ethernet4");

        // Cleanup
        gMirrorOrch->m_syncdMirrors.erase("immutable_test");
    }




}
