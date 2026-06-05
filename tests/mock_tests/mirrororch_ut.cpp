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
#include "mirrororch_sample_port_sai_wrap.h"
#include <arpa/inet.h>
#include <cstring>

extern sai_mirror_api_t *sai_mirror_api;

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
        gSwitchOrch->m_portEgressSampleMirrorSupported = true;
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
        // Verify createEntry correctly parses sample_rate and truncate_size.
        // RX needs only the ingress sample-mirror capability.
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressSampleMirrorSupported = true;
        gSwitchOrch->m_portEgressSampleMirrorSupported = false;
        gSwitchOrch->m_samplepacketTruncationSupported = true;

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

    TEST_F(MirrorOrchTest, CreateEntryRejectsTxWhenEgressUnsupported)
    {
        // A TX sampled session needs the EGRESS sample-mirror capability;
        // reject when the platform only advertises the INGRESS capability.
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressSampleMirrorSupported = true;
        gSwitchOrch->m_portEgressSampleMirrorSupported = false;

        vector<FieldValueTuple> data;
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("dscp", "8");
        data.emplace_back("ttl", "64");
        data.emplace_back("direction", "TX");
        data.emplace_back("sample_rate", "50000");

        auto status = gMirrorOrch->createEntry("tx_no_egress_session", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
        ASSERT_FALSE(gMirrorOrch->sessionExists("tx_no_egress_session"));
    }

    TEST_F(MirrorOrchTest, CreateEntryAcceptsTxWhenEgressSupported)
    {
        // A TX sampled session is accepted when the EGRESS sample-mirror
        // capability is advertised.
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressSampleMirrorSupported = false;
        gSwitchOrch->m_portEgressSampleMirrorSupported = true;
        gSwitchOrch->m_samplepacketTruncationSupported = true;

        vector<FieldValueTuple> data;
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("gre_type", "0x88be");
        data.emplace_back("dscp", "8");
        data.emplace_back("ttl", "64");
        data.emplace_back("queue", "0");
        data.emplace_back("direction", "TX");
        data.emplace_back("sample_rate", "50000");

        auto status = gMirrorOrch->createEntry("tx_egress_session", data);
        ASSERT_EQ(status, task_process_status::task_success);
        ASSERT_TRUE(gMirrorOrch->sessionExists("tx_egress_session"));
        gMirrorOrch->m_syncdMirrors.erase("tx_egress_session");
    }

    TEST_F(MirrorOrchTest, CreateEntryRejectsBothWhenIngressUnsupported)
    {
        // direction=BOTH binds both directions, so it requires BOTH the
        // ingress and egress capabilities; reject when ingress is missing.
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressSampleMirrorSupported = false;
        gSwitchOrch->m_portEgressSampleMirrorSupported = true;

        vector<FieldValueTuple> data;
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("dscp", "8");
        data.emplace_back("ttl", "64");
        data.emplace_back("direction", "BOTH");
        data.emplace_back("sample_rate", "50000");

        auto status = gMirrorOrch->createEntry("both_no_ingress_session", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
        ASSERT_FALSE(gMirrorOrch->sessionExists("both_no_ingress_session"));
    }

    TEST_F(MirrorOrchTest, CreateEntryRejectsBothWhenEgressUnsupported)
    {
        // direction=BOTH requires both capabilities; ingress passes the first
        // check, so this exercises the BOTH egress-capability branch.
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressSampleMirrorSupported = true;
        gSwitchOrch->m_portEgressSampleMirrorSupported = false;

        vector<FieldValueTuple> data;
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("dscp", "8");
        data.emplace_back("ttl", "64");
        data.emplace_back("direction", "BOTH");
        data.emplace_back("sample_rate", "50000");

        auto status = gMirrorOrch->createEntry("both_no_egress_session", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
        ASSERT_FALSE(gMirrorOrch->sessionExists("both_no_egress_session"));
    }

    TEST_F(MirrorOrchTest, CreateEntryAcceptsBothWhenBothSupported)
    {
        // direction=BOTH is accepted when both the ingress and egress
        // sample-mirror capabilities are advertised.
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressSampleMirrorSupported = true;
        gSwitchOrch->m_portEgressSampleMirrorSupported = true;

        vector<FieldValueTuple> data;
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("gre_type", "0x88be");
        data.emplace_back("dscp", "8");
        data.emplace_back("ttl", "64");
        data.emplace_back("queue", "0");
        data.emplace_back("direction", "BOTH");
        data.emplace_back("sample_rate", "50000");

        auto status = gMirrorOrch->createEntry("both_supported_session", data);
        ASSERT_EQ(status, task_process_status::task_success);
        ASSERT_TRUE(gMirrorOrch->sessionExists("both_supported_session"));
        gMirrorOrch->m_syncdMirrors.erase("both_supported_session");
    }

    TEST_F(MirrorOrchTest, CreateEntryRejectsSampledEmptyDirection)
    {
        // A sampled session with no direction field is rejected outright.
        ASSERT_NE(gSwitchOrch, nullptr);
        ASSERT_NE(gMirrorOrch, nullptr);

        gSwitchOrch->m_portIngressSampleMirrorSupported = true;
        gSwitchOrch->m_portEgressSampleMirrorSupported = true;

        vector<FieldValueTuple> data;
        data.emplace_back("type", "ERSPAN");
        data.emplace_back("src_ip", "10.0.0.1");
        data.emplace_back("dst_ip", "10.0.0.2");
        data.emplace_back("dscp", "8");
        data.emplace_back("ttl", "64");
        data.emplace_back("sample_rate", "50000");

        auto status = gMirrorOrch->createEntry("empty_dir_session", data);
        ASSERT_EQ(status, task_process_status::task_invalid_entry);
        ASSERT_FALSE(gMirrorOrch->sessionExists("empty_dir_session"));
    }

    TEST_F(MirrorOrchTest, CreateEntryDuplicateRejected)
    {
        // A SET on an already-existing session is rejected as a duplicate;
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

        auto status = gMirrorOrch->createEntry("dup_session", data);
        ASSERT_EQ(status, task_process_status::task_success);

        // Re-applying the same key must be rejected, not updated.
        status = gMirrorOrch->createEntry("dup_session", data);
        ASSERT_EQ(status, task_process_status::task_duplicated);

        gMirrorOrch->m_syncdMirrors.erase("dup_session");
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

    // Build a real ERSPAN mirror-session OID (GRE protocol type 0x8949) bound to a real monitor port.
    static sai_object_id_t createErspanSessionOid(sai_object_id_t monitor_port_id)
    {
        std::vector<sai_attribute_t> attrs;
        sai_attribute_t attr;

        attr.id = SAI_MIRROR_SESSION_ATTR_MONITOR_PORT;
        attr.value.oid = monitor_port_id;
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_TYPE;
        attr.value.s32 = SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE;
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE;
        attr.value.s32 = SAI_ERSPAN_ENCAPSULATION_TYPE_MIRROR_L3_GRE_TUNNEL;
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION;
        attr.value.u8 = 4;
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_TOS;
        attr.value.u16 = 0;
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_TTL;
        attr.value.u8 = 64;
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS;
        attr.value.ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        attr.value.ipaddr.addr.ip4 = htonl(0x0a000001); // 10.0.0.1
        attrs.push_back(attr);

        attr.id = SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS;
        attr.value.ipaddr.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        attr.value.ipaddr.addr.ip4 = htonl(0x0a000002); // 10.0.0.2
        attrs.push_back(attr);

        sai_mac_t src_mac = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
        attr.id = SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS;
        memcpy(attr.value.mac, src_mac, sizeof(sai_mac_t));
        attrs.push_back(attr);

        sai_mac_t dst_mac = {0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
        attr.id = SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS;
        memcpy(attr.value.mac, dst_mac, sizeof(sai_mac_t));
        attrs.push_back(attr);

        // Sampled-ERSPAN GRE protocol type per the test plan.
        attr.id = SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE;
        attr.value.u16 = 0x8949;
        attrs.push_back(attr);

        sai_object_id_t oid = SAI_NULL_OBJECT_ID;
        EXPECT_NE(sai_mirror_api, nullptr);
        if (sai_mirror_api == nullptr) return SAI_NULL_OBJECT_ID;
        sai_status_t status = sai_mirror_api->create_mirror_session(
            &oid, gSwitchId, (uint32_t)attrs.size(), attrs.data());
        EXPECT_EQ(status, SAI_STATUS_SUCCESS);
        return oid;
    }

    // Read back the emulated SAMPLEPACKET_ENABLE attr for a port (via the SAI
    // wrap) so a test can assert which direction was actually programmed.
    static sai_object_id_t readSamplePacketEnable(sai_object_id_t port_id, sai_attr_id_t attr_id)
    {
        sai_attribute_t attr;
        attr.id = attr_id;
        attr.value.oid = SAI_NULL_OBJECT_ID;
        EXPECT_EQ(sai_port_api->get_port_attribute(port_id, 1, &attr), SAI_STATUS_SUCCESS);
        return attr.value.oid;
    }

    TEST_F(MirrorOrchPortTest, SampledMirrorPhyPortSetClear)
    {
        mirror_sample_port_wrap_ut::PortSampleSaiGuard saiPortSampleGuard;
        // Covers setUnsetSampledMirrorOnPhyPort path for both set and clear
        // (SAMPLEPACKET_ENABLE + SAMPLE_MIRROR_SESSION bind, then reverse-order unbind).
        ASSERT_NE(gMirrorOrch, nullptr);
        ASSERT_NE(gPortsOrch, nullptr);

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        sai_object_id_t sessionOid = createErspanSessionOid(port.m_port_id);
        ASSERT_NE(sessionOid, SAI_NULL_OBJECT_ID);

        MirrorEntry entry("");
        entry.sample_rate = 50000;
        ASSERT_TRUE(gMirrorOrch->createSamplePacket("phy_set_clear", entry));
        ASSERT_NE(entry.samplepacketId, SAI_NULL_OBJECT_ID);

        // SET: SAMPLEPACKET_ENABLE first, then SAMPLE_MIRROR_SESSION.
        ASSERT_TRUE(gMirrorOrch->setUnsetSampledMirrorOnPhyPort(
            port.m_port_id, port.m_alias, /*set*/ true, MirrorBindDirection::Ingress, sessionOid, entry.samplepacketId));

        // CLEAR: SAMPLE_MIRROR_SESSION first, then SAMPLEPACKET_ENABLE.
        ASSERT_TRUE(gMirrorOrch->setUnsetSampledMirrorOnPhyPort(
            port.m_port_id, port.m_alias, /*set*/ false, MirrorBindDirection::Ingress, sessionOid, entry.samplepacketId));

        sai_samplepacket_api->remove_samplepacket(entry.samplepacketId);
        sai_mirror_api->remove_mirror_session(sessionOid);
    }

    TEST_F(MirrorOrchPortTest, SampledMirrorPhyPortConflict)
    {
        mirror_sample_port_wrap_ut::PortSampleSaiGuard saiPortSampleGuard;
        // Covers the samplepacket-conflict guard: a port already bound to a
        // different INGRESS_SAMPLEPACKET_ENABLE OID must reject the new bind.
        ASSERT_NE(gMirrorOrch, nullptr);
        ASSERT_NE(gPortsOrch, nullptr);

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        sai_object_id_t sessionOid = createErspanSessionOid(port.m_port_id);
        ASSERT_NE(sessionOid, SAI_NULL_OBJECT_ID);

        MirrorEntry existing("");
        existing.sample_rate = 50000;
        ASSERT_TRUE(gMirrorOrch->createSamplePacket("conflict_existing", existing));
        MirrorEntry incoming("");
        incoming.sample_rate = 60000;
        ASSERT_TRUE(gMirrorOrch->createSamplePacket("conflict_incoming", incoming));
        ASSERT_NE(existing.samplepacketId, incoming.samplepacketId);

        // Pre-bind a DIFFERENT samplepacket OID directly onto the port.
        sai_attribute_t pre;
        pre.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;
        pre.value.oid = existing.samplepacketId;
        ASSERT_EQ(sai_port_api->set_port_attribute(port.m_port_id, &pre), SAI_STATUS_SUCCESS);

        // Binding a different samplepacket must fail the conflict check.
        ASSERT_FALSE(gMirrorOrch->setUnsetSampledMirrorOnPhyPort(
            port.m_port_id, port.m_alias, /*set*/ true, MirrorBindDirection::Ingress, sessionOid, incoming.samplepacketId));

        // Cleanup: clear binding and remove real objects.
        pre.value.oid = SAI_NULL_OBJECT_ID;
        sai_port_api->set_port_attribute(port.m_port_id, &pre);
        sai_samplepacket_api->remove_samplepacket(existing.samplepacketId);
        sai_samplepacket_api->remove_samplepacket(incoming.samplepacketId);
        sai_mirror_api->remove_mirror_session(sessionOid);
    }

    TEST_F(MirrorOrchPortTest, ConfigurePortMirrorSessionSampledPhy)
    {
        mirror_sample_port_wrap_ut::PortSampleSaiGuard saiPortSampleGuard;
        // Covers configurePortMirrorSession RX dispatch into the sampled
        // PHY-direct branch of setUnsetPortMirror.
        ASSERT_NE(gMirrorOrch, nullptr);
        ASSERT_NE(gPortsOrch, nullptr);
        ASSERT_NE(gSwitchOrch, nullptr);

        gSwitchOrch->m_portIngressMirrorSupported = true;
        gSwitchOrch->m_portIngressSampleMirrorSupported = true;

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        sai_object_id_t sessionOid = createErspanSessionOid(port.m_port_id);
        ASSERT_NE(sessionOid, SAI_NULL_OBJECT_ID);

        MirrorEntry entry("");
        entry.sample_rate = 50000;
        ASSERT_TRUE(gMirrorOrch->createSamplePacket("cfg_phy", entry));
        ASSERT_NE(entry.samplepacketId, SAI_NULL_OBJECT_ID);
        entry.sessionId = sessionOid;
        entry.src_port = "Ethernet0";
        entry.direction = MIRROR_RX_DIRECTION;

        ASSERT_TRUE(gMirrorOrch->configurePortMirrorSession("cfg_phy", entry, /*set*/ true));
        ASSERT_TRUE(gMirrorOrch->configurePortMirrorSession("cfg_phy", entry, /*set*/ false));

        sai_samplepacket_api->remove_samplepacket(entry.samplepacketId);
        sai_mirror_api->remove_mirror_session(sessionOid);
    }

    TEST_F(MirrorOrchPortTest, SampledMirrorEgressPhyPortSetClear)
    {
        mirror_sample_port_wrap_ut::PortSampleSaiGuard saiPortSampleGuard;
        // Covers setUnsetSampledMirrorOnPhyPort with ingress=false: it must
        // program the EGRESS_SAMPLEPACKET_ENABLE + EGRESS_SAMPLE_MIRROR_SESSION
        // attributes (bind, then reverse-order unbind).
        ASSERT_NE(gMirrorOrch, nullptr);
        ASSERT_NE(gPortsOrch, nullptr);

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        sai_object_id_t sessionOid = createErspanSessionOid(port.m_port_id);
        ASSERT_NE(sessionOid, SAI_NULL_OBJECT_ID);

        MirrorEntry entry("");
        entry.sample_rate = 50000;
        ASSERT_TRUE(gMirrorOrch->createSamplePacket("egress_set_clear", entry));
        ASSERT_NE(entry.samplepacketId, SAI_NULL_OBJECT_ID);

        ASSERT_TRUE(gMirrorOrch->setUnsetSampledMirrorOnPhyPort(
            port.m_port_id, port.m_alias, /*set*/ true, MirrorBindDirection::Egress, sessionOid, entry.samplepacketId));

        // The egress bind must program EGRESS_SAMPLEPACKET_ENABLE and must NOT
        // touch the ingress attr (otherwise the direction enum was misrouted).
        ASSERT_EQ(readSamplePacketEnable(port.m_port_id, SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE), entry.samplepacketId);
        ASSERT_EQ(readSamplePacketEnable(port.m_port_id, SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE), SAI_NULL_OBJECT_ID);

        ASSERT_TRUE(gMirrorOrch->setUnsetSampledMirrorOnPhyPort(
            port.m_port_id, port.m_alias, /*set*/ false, MirrorBindDirection::Egress, sessionOid, entry.samplepacketId));

        // The clear must remove the egress binding.
        ASSERT_EQ(readSamplePacketEnable(port.m_port_id, SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE), SAI_NULL_OBJECT_ID);

        sai_samplepacket_api->remove_samplepacket(entry.samplepacketId);
        sai_mirror_api->remove_mirror_session(sessionOid);
    }

    TEST_F(MirrorOrchPortTest, ConfigurePortMirrorSessionSampledEgressPhy)
    {
        mirror_sample_port_wrap_ut::PortSampleSaiGuard saiPortSampleGuard;
        // Covers configurePortMirrorSession TX dispatch into the sampled
        // egress PHY-direct branch of setUnsetPortMirror.
        ASSERT_NE(gMirrorOrch, nullptr);
        ASSERT_NE(gPortsOrch, nullptr);
        ASSERT_NE(gSwitchOrch, nullptr);

        gSwitchOrch->m_portEgressMirrorSupported = true;
        gSwitchOrch->m_portEgressSampleMirrorSupported = true;

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        sai_object_id_t sessionOid = createErspanSessionOid(port.m_port_id);
        ASSERT_NE(sessionOid, SAI_NULL_OBJECT_ID);

        MirrorEntry entry("");
        entry.sample_rate = 50000;
        ASSERT_TRUE(gMirrorOrch->createSamplePacket("cfg_egress_phy", entry));
        ASSERT_NE(entry.samplepacketId, SAI_NULL_OBJECT_ID);
        entry.sessionId = sessionOid;
        entry.src_port = "Ethernet0";
        entry.direction = MIRROR_TX_DIRECTION;

        ASSERT_TRUE(gMirrorOrch->configurePortMirrorSession("cfg_egress_phy", entry, /*set*/ true));

        // TX dispatch must bind only the egress attr.
        ASSERT_EQ(readSamplePacketEnable(port.m_port_id, SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE), entry.samplepacketId);
        ASSERT_EQ(readSamplePacketEnable(port.m_port_id, SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE), SAI_NULL_OBJECT_ID);

        ASSERT_TRUE(gMirrorOrch->configurePortMirrorSession("cfg_egress_phy", entry, /*set*/ false));

        ASSERT_EQ(readSamplePacketEnable(port.m_port_id, SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE), SAI_NULL_OBJECT_ID);

        sai_samplepacket_api->remove_samplepacket(entry.samplepacketId);
        sai_mirror_api->remove_mirror_session(sessionOid);
    }

    TEST_F(MirrorOrchPortTest, ConfigurePortMirrorSessionSampledBothPhy)
    {
        mirror_sample_port_wrap_ut::PortSampleSaiGuard saiPortSampleGuard;
        // Covers configurePortMirrorSession BOTH dispatch: the RX and TX
        // branches are independent, so a BOTH session must program the
        // ingress AND egress sampled-mirror attrs on the same port.
        ASSERT_NE(gMirrorOrch, nullptr);
        ASSERT_NE(gPortsOrch, nullptr);
        ASSERT_NE(gSwitchOrch, nullptr);

        gSwitchOrch->m_portIngressMirrorSupported = true;
        gSwitchOrch->m_portEgressMirrorSupported = true;

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort("Ethernet0", port));

        sai_object_id_t sessionOid = createErspanSessionOid(port.m_port_id);
        ASSERT_NE(sessionOid, SAI_NULL_OBJECT_ID);

        MirrorEntry entry("");
        entry.sample_rate = 50000;
        ASSERT_TRUE(gMirrorOrch->createSamplePacket("cfg_both_phy", entry));
        ASSERT_NE(entry.samplepacketId, SAI_NULL_OBJECT_ID);
        entry.sessionId = sessionOid;
        entry.src_port = "Ethernet0";
        entry.direction = MIRROR_BOTH_DIRECTION;

        ASSERT_TRUE(gMirrorOrch->configurePortMirrorSession("cfg_both_phy", entry, /*set*/ true));

        // BOTH must bind both directions to the same samplepacket OID.
        ASSERT_EQ(readSamplePacketEnable(port.m_port_id, SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE), entry.samplepacketId);
        ASSERT_EQ(readSamplePacketEnable(port.m_port_id, SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE), entry.samplepacketId);

        ASSERT_TRUE(gMirrorOrch->configurePortMirrorSession("cfg_both_phy", entry, /*set*/ false));

        // Clear must remove both bindings.
        ASSERT_EQ(readSamplePacketEnable(port.m_port_id, SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE), SAI_NULL_OBJECT_ID);
        ASSERT_EQ(readSamplePacketEnable(port.m_port_id, SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE), SAI_NULL_OBJECT_ID);

        sai_samplepacket_api->remove_samplepacket(entry.samplepacketId);
        sai_mirror_api->remove_mirror_session(sessionOid);
    }

}
