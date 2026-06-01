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

        // updateEntry rebuilds the session from CONFIG_DB on immutable changes
        swss::Table cfgMirrorTable(m_config_db.get(), CFG_MIRROR_SESSION_TABLE_NAME);
        cfgMirrorTable.set("immutable_test", update_data);

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
        cfgMirrorTable.del("immutable_test");
        gMirrorOrch->m_syncdMirrors.erase("immutable_test");
    }

    TEST_F(MirrorOrchTest, UpdateEntryTruncateEnableTransition)
    {
        // Cover the new TRUNCATE_ENABLE-in-sync-with-TRUNCATE_SIZE logic in
        // updateEntry: ENABLE must flip when SIZE crosses 0, and SIZE must
        // not be pushed to SAI when truncation is being disabled.
        ASSERT_NE(gMirrorOrch, nullptr);
        ASSERT_NE(gSwitchOrch, nullptr);

        gSwitchOrch->m_portIngressSampleMirrorSupported = true;
        gSwitchOrch->m_samplepacketTruncationSupported = true;

        string key = "trunc_transition_session";
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

        auto buildData = [](uint32_t trunc) {
            vector<FieldValueTuple> d;
            d.emplace_back("type", "ERSPAN");
            d.emplace_back("src_ip", "10.0.0.1");
            d.emplace_back("dst_ip", "10.0.0.2");
            d.emplace_back("gre_type", "0x8949");
            d.emplace_back("dscp", "8");
            d.emplace_back("ttl", "64");
            d.emplace_back("queue", "0");
            d.emplace_back("src_port", "Ethernet0");
            d.emplace_back("direction", "RX");
            d.emplace_back("policer", "");
            d.emplace_back("sample_rate", "50000");
            d.emplace_back("truncate_size", to_string(trunc));
            return d;
        };

        // Phase 1: 128 -> 0 (disable truncation; SIZE must NOT be set)
        auto status = gMirrorOrch->updateEntry(key, buildData(0));
        ASSERT_EQ(status, task_process_status::task_success);
        ASSERT_EQ(gMirrorOrch->m_syncdMirrors.find(key)->second.truncate_size, (uint32_t)0);

        // Phase 2: 0 -> 256 (enable truncation; ENABLE=true then SIZE=256)
        status = gMirrorOrch->updateEntry(key, buildData(256));
        ASSERT_EQ(status, task_process_status::task_success);
        ASSERT_EQ(gMirrorOrch->m_syncdMirrors.find(key)->second.truncate_size, (uint32_t)256);

        // Phase 3: 256 -> 512 (still enabled; ENABLE=true and SIZE=512)
        status = gMirrorOrch->updateEntry(key, buildData(512));
        ASSERT_EQ(status, task_process_status::task_success);
        ASSERT_EQ(gMirrorOrch->m_syncdMirrors.find(key)->second.truncate_size, (uint32_t)512);

        // Cleanup
        auto& final_entry = gMirrorOrch->m_syncdMirrors.find(key)->second;
        sai_samplepacket_api->remove_samplepacket(final_entry.samplepacketId);
        gMirrorOrch->m_syncdMirrors.erase(key);
    }

    TEST_F(MirrorOrchTest, SetSessionStateHdelsClearedSampleRate)
    {
        // When sample_rate becomes 0, setSessionState must HDEL the field
        // from STATE_DB so consumers do not see a stale rate.
        ASSERT_NE(gMirrorOrch, nullptr);

        string key = "hdel_sample_rate_session";
        swss::Table stateTable(m_state_db.get(), STATE_MIRROR_SESSION_TABLE_NAME);
        stateTable.hset(key, "sample_rate", "50000");

        string seeded;
        ASSERT_TRUE(stateTable.hget(key, "sample_rate", seeded));
        ASSERT_EQ(seeded, "50000");

        MirrorEntry session("");
        session.type = "ERSPAN";
        session.sample_rate = 0;
        session.truncate_size = 0;

        gMirrorOrch->setSessionState(key, session, "sample_rate");

        string after;
        ASSERT_FALSE(stateTable.hget(key, "sample_rate", after));

        stateTable.del(key);
    }

    TEST_F(MirrorOrchTest, SetSessionStateHdelsClearedTruncateSize)
    {
        // When truncate_size becomes 0, setSessionState must HDEL the field
        // from STATE_DB so consumers do not see a stale size.
        ASSERT_NE(gMirrorOrch, nullptr);

        string key = "hdel_truncate_session";
        swss::Table stateTable(m_state_db.get(), STATE_MIRROR_SESSION_TABLE_NAME);
        stateTable.hset(key, "truncate_size", "128");

        string seeded;
        ASSERT_TRUE(stateTable.hget(key, "truncate_size", seeded));
        ASSERT_EQ(seeded, "128");

        MirrorEntry session("");
        session.type = "ERSPAN";
        session.sample_rate = 0;
        session.truncate_size = 0;

        gMirrorOrch->setSessionState(key, session, "truncate_size");

        string after;
        ASSERT_FALSE(stateTable.hget(key, "truncate_size", after));

        stateTable.del(key);
    }

    TEST_F(MirrorOrchPortTest, UpdateEntryRecreateUsesCfgDbFullRow)
    {
        // When CONFIG_DB receives a partial HSET (single field), updateEntry
        // must rebuild the session from the full CONFIG_DB row instead of the
        // partial payload, otherwise the recreated session loses fields.
        ASSERT_NE(gMirrorOrch, nullptr);

        string key = "partial_hset_session";

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

        swss::Table cfgMirrorTable(m_config_db.get(), CFG_MIRROR_SESSION_TABLE_NAME);
        cfgMirrorTable.set(key, create_data);

        auto status = gMirrorOrch->createEntry(key, create_data);
        ASSERT_EQ(status, task_process_status::task_success);

        // Simulate user HSET sample_rate=50000: CONFIG_DB now has the full
        // row + new sample_rate, but the SET notification payload only
        // carries the single changed field.
        vector<FieldValueTuple> full_after_hset = create_data;
        full_after_hset.emplace_back("sample_rate", "50000");
        cfgMirrorTable.set(key, full_after_hset);

        vector<FieldValueTuple> partial;
        partial.emplace_back("sample_rate", "50000");

        status = gMirrorOrch->updateEntry(key, partial);
        ASSERT_EQ(status, task_process_status::task_success);

        // Recreated session must carry all original fields PLUS the new sample_rate
        auto& session = gMirrorOrch->m_syncdMirrors.find(key)->second;
        ASSERT_EQ(session.srcIp, IpAddress("10.0.0.1"));
        ASSERT_EQ(session.dstIp, IpAddress("10.0.0.2"));
        ASSERT_EQ(session.greType, (uint16_t)0x8949);
        ASSERT_EQ(session.dscp, (uint8_t)8);
        ASSERT_EQ(session.ttl, (uint8_t)64);
        ASSERT_EQ(session.src_port, "Ethernet0");
        ASSERT_EQ(session.sample_rate, (uint32_t)50000);

        cfgMirrorTable.del(key);
        gMirrorOrch->m_syncdMirrors.erase(key);
    }

    TEST_F(MirrorOrchPortTest, UpdateEntryRecreateFailsWhenCfgDbMissing)
    {
        // If the CONFIG_DB row is missing when an immutable change is detected,
        // updateEntry must return task_failed (defensive) instead of recreating
        // the session with a partial payload that would strip fields.
        ASSERT_NE(gMirrorOrch, nullptr);

        string key = "cfgdb_missing_session";
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
        entry.sample_rate = 0;
        entry.truncate_size = 0;
        entry.status = false;
        entry.refCount = 0;
        gMirrorOrch->m_syncdMirrors.emplace(key, entry);

        // Ensure CONFIG_DB has no row for this key
        swss::Table cfgMirrorTable(m_config_db.get(), CFG_MIRROR_SESSION_TABLE_NAME);
        cfgMirrorTable.del(key);

        // Trigger an immutable change (sample_rate 0 -> 50000 flips mode)
        vector<FieldValueTuple> partial;
        partial.emplace_back("sample_rate", "50000");

        auto status = gMirrorOrch->updateEntry(key, partial);
        ASSERT_EQ(status, task_process_status::task_failed);

        // Session must still be present (delete was not invoked on the failure path)
        ASSERT_TRUE(gMirrorOrch->sessionExists(key));

        gMirrorOrch->m_syncdMirrors.erase(key);
    }

    TEST_F(MirrorOrchTest, RemoveSamplePacketSuccess)
    {
        // Cover removeSamplePacket success path on a real (non-NULL) OID:
        // remove_samplepacket call, success branch, and OID reset.
        ASSERT_NE(gMirrorOrch, nullptr);

        MirrorEntry entry("");
        entry.type = "ERSPAN";
        entry.sample_rate = 50000;
        entry.truncate_size = 0;

        // createSamplePacket yields a real samplepacket OID from the VS switch.
        ASSERT_TRUE(gMirrorOrch->createSamplePacket("rm_sp_session", entry));
        ASSERT_NE(entry.samplepacketId, SAI_NULL_OBJECT_ID);

        // Exercise the real removal branch (non-NULL OID).
        ASSERT_TRUE(gMirrorOrch->removeSamplePacket("rm_sp_session", entry));
        ASSERT_EQ(entry.samplepacketId, SAI_NULL_OBJECT_ID);
    }


}
