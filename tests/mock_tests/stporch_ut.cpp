#include <gtest/gtest.h>
#include <gmock/gmock.h>

#define private public // make Directory::m_values available to clean it.
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "dbconnector.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_orch_test.h"
#include "mock_table.h"
#define private public
#include "stporch.h"
#undef private
#include "mock_sai_stp.h"


namespace stporch_test
{
    using namespace std;
    using namespace swss;
    using namespace mock_orch_test;
    using ::testing::StrictMock;

    using ::testing::_;
    using ::testing::Return;
    using ::gStpOrch;

    sai_status_t _ut_stub_sai_set_vlan_attribute(_In_ sai_object_id_t vlan_oid,
                    _In_ const sai_attribute_t *attr)
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_flush_fdb_entries(_In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count, _In_ const sai_attribute_t *attr_list)
    {
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_get_switch_attribute(
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _Inout_ sai_attribute_t *attr_list)
    {
        return SAI_STATUS_SUCCESS;
        if (attr_count == 2)
        {
            if (attr_list[0].id == SAI_SWITCH_ATTR_DEFAULT_STP_INST_ID)
            {
                attr_list[0].value.oid = 0;
            }
            if (attr_list[1].id == SAI_SWITCH_ATTR_MAX_STP_INSTANCE)
            {
                attr_list[1].value.u32 = 510;
            }
        }
        return SAI_STATUS_SUCCESS;
    }

    // BPDU-guard err-disable tests below drive setPortAdminStatusByAlias() ->
    // sai_port_api->set_port_attribute(SAI_PORT_ATTR_ADMIN_STATE). Record the
    // last requested admin state per port OID rather than mocking the whole
    // API, since StpOrch's PortsOrch/Port objects are real (built on the VS
    // SAI) - only the admin-state write itself needs to be observable.
    // sai_port_id_t -> last requested admin state (true = up).
    map<sai_object_id_t, bool> g_port_admin_status;
    int g_port_admin_status_call_count = 0;

    sai_status_t _ut_stub_sai_set_port_attribute(_In_ sai_object_id_t port_id,
                    _In_ const sai_attribute_t *attr)
    {
        if (attr && attr->id == SAI_PORT_ATTR_ADMIN_STATE)
        {
            g_port_admin_status[port_id] = attr->value.booldata;
            g_port_admin_status_call_count++;
            return SAI_STATUS_SUCCESS;
        }
        return SAI_STATUS_SUCCESS;
    }


    class StpOrchTest : public MockOrchTest {
    protected:
        void ApplyInitialConfigs()
        {
            Table port_table = Table(m_app_db.get(), APP_PORT_TABLE_NAME);
            Table vlan_table = Table(m_app_db.get(), APP_VLAN_TABLE_NAME);
            Table vlan_member_table = Table(m_app_db.get(), APP_VLAN_MEMBER_TABLE_NAME);

            auto ports = ut_helper::getInitialSaiPorts();
            port_table.set(ETHERNET0, ports[ETHERNET0]);
            port_table.set(ETHERNET4, ports[ETHERNET4]);
            port_table.set(ETHERNET8, ports[ETHERNET8]);
            port_table.set("PortConfigDone", { { "count", to_string(1) } });
            port_table.set("PortInitDone", { {} });

            vlan_table.set(VLAN_1000, { { "admin_status", "up" },
                                        { "mtu", "9100" },
                                        { "mac", "00:aa:bb:cc:dd:ee" } });
            vlan_member_table.set(
                VLAN_1000 + vlan_member_table.getTableNameSeparator() + ETHERNET0,
                { { "tagging_mode", "untagged" } });

            gPortsOrch->addExistingData(&port_table);
            gPortsOrch->addExistingData(&vlan_table);
            gPortsOrch->addExistingData(&vlan_member_table);
            static_cast<Orch *>(gPortsOrch)->doTask();
        }
        void PostSetUp() override
        {
            vector<string> tableNames =
                {"STP_TABLE",
                "STP_VLAN_INSTANCE_TABLE",
                "STP_PORT_TABLE",
                "STP_PORT_STATE_TABLE",
                "STP_FASTAGEING_FLUSH_TABLE",
                "STP_INST_PORT_FLUSH_TABLE"};
            _hook_sai_switch_api();
            // STATE_DB is shared across tests. Start each one without a
            // BPDU-guard record, or StpOrch replays the last test's ports.
            Table(m_state_db.get(), STATE_STP_TABLE_NAME).del("BPDU_GUARD");
            gStpOrch = new StpOrch(m_app_db.get(), m_state_db.get(), tableNames);
        }
        void PreTearDown() override
        {
            delete gStpOrch;
            gStpOrch = nullptr;
            _unhook_sai_switch_api();
        }

        sai_stp_api_t ut_sai_stp_api;
        sai_stp_api_t *org_sai_stp_api;

        void _hook_sai_stp_api()
        {
            ut_sai_stp_api = *sai_stp_api;
            org_sai_stp_api = sai_stp_api;
            sai_stp_api = &ut_sai_stp_api;
        }

        void _unhook_sai_stp_api()
        {
            sai_stp_api = org_sai_stp_api;
        }

        sai_vlan_api_t ut_sai_vlan_api;
        sai_vlan_api_t *org_sai_vlan_api;

        void _hook_sai_vlan_api()
        {
            ut_sai_vlan_api = *sai_vlan_api;
            org_sai_vlan_api = sai_vlan_api;
            ut_sai_vlan_api.set_vlan_attribute = _ut_stub_sai_set_vlan_attribute;
            sai_vlan_api = &ut_sai_vlan_api;
        }

        void _unhook_sai_vlan_api()
        {
            sai_vlan_api = org_sai_vlan_api;
        }

        sai_fdb_api_t ut_sai_fdb_api;
        sai_fdb_api_t *org_sai_fdb_api;
        void _hook_sai_fdb_api()
        {
            ut_sai_fdb_api = *sai_fdb_api;
            org_sai_fdb_api = sai_fdb_api;
            ut_sai_fdb_api.flush_fdb_entries = _ut_stub_sai_flush_fdb_entries;
            sai_fdb_api = &ut_sai_fdb_api;
        }

        void _unhook_sai_fdb_api()
        {
            sai_fdb_api = org_sai_fdb_api;
        }

        sai_switch_api_t ut_sai_switch_api;
        sai_switch_api_t *pold_sai_switch_api;
        void _hook_sai_switch_api()
        {
            ut_sai_switch_api = *sai_switch_api;
            pold_sai_switch_api = sai_switch_api;
            ut_sai_switch_api.get_switch_attribute = _ut_stub_sai_get_switch_attribute;
            sai_switch_api = &ut_sai_switch_api;
        }
        void _unhook_sai_switch_api()
        {
            sai_switch_api = pold_sai_switch_api;
        }

        sai_port_api_t ut_sai_port_api;
        sai_port_api_t *pold_sai_port_api;
        void _hook_sai_port_api()
        {
            g_port_admin_status.clear();
            g_port_admin_status_call_count = 0;
            ut_sai_port_api = *sai_port_api;
            pold_sai_port_api = sai_port_api;
            ut_sai_port_api.set_port_attribute = _ut_stub_sai_set_port_attribute;
            sai_port_api = &ut_sai_port_api;
        }
        void _unhook_sai_port_api()
        {
            sai_port_api = pold_sai_port_api;
        }
    };

    TEST_F(StpOrchTest, TestAddRemoveStpPort) {
        _hook_sai_stp_api();
        _hook_sai_vlan_api();
        _hook_sai_fdb_api();

        StrictMock<MockSaiStp> mock_sai_stp_;
        mock_sai_stp = &mock_sai_stp_;
        sai_stp_api->create_stp = mock_create_stp;
        sai_stp_api->remove_stp = mock_remove_stp;
        sai_stp_api->create_stp_port = mock_create_stp_port;
        sai_stp_api->remove_stp_port = mock_remove_stp_port;
        sai_stp_api->set_stp_port_attribute = mock_set_stp_port_attribute;

        Port port;
        Port port1;
        sai_uint16_t stp_instance = 1;
        sai_object_id_t stp_port_oid = 67890;
        sai_object_id_t stp_oid = 98765;
        bool result;

        ASSERT_TRUE(gPortsOrch->getPort(ETHERNET0, port));
        ASSERT_TRUE(gPortsOrch->getPort(ETHERNET4, port1));

        EXPECT_CALL(mock_sai_stp_,
            create_stp(_, _, _, _)).WillOnce(::testing::DoAll(::testing::SetArgPointee<0>(stp_oid),
                                        ::testing::Return(SAI_STATUS_SUCCESS)));
        result = gStpOrch->addVlanToStpInstance(VLAN_1000, stp_instance);
        ASSERT_TRUE(result);

        EXPECT_CALL(mock_sai_stp_,
            create_stp_port(_, _, 3, _)).WillOnce(::testing::DoAll(::testing::SetArgPointee<0>(stp_port_oid),
                                        ::testing::Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(mock_sai_stp_,
            set_stp_port_attribute(_,_)).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
        port.m_bridge_port_id = 1234;
        result = gStpOrch->updateStpPortState(port, stp_instance, STP_STATE_FORWARDING);
        ASSERT_TRUE(result);

        result = gStpOrch->stpVlanFdbFlush(VLAN_1000);
        ASSERT_TRUE(result);

        EXPECT_CALL(mock_sai_stp_,
            remove_stp_port(_)).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
        result = gStpOrch->removeStpPort(port, stp_instance);
        ASSERT_TRUE(result);

        EXPECT_CALL(mock_sai_stp_,
            create_stp_port(_, _, 3, _)).WillOnce(::testing::DoAll(::testing::SetArgPointee<0>(stp_port_oid),
                                        ::testing::Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(mock_sai_stp_,
            set_stp_port_attribute(_,_)).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
        port1.m_bridge_port_id = 1111;
        result = gStpOrch->updateStpPortState(port1, stp_instance, STP_STATE_BLOCKING);
        ASSERT_TRUE(result);

        EXPECT_CALL(mock_sai_stp_,
            remove_stp_port(_)).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
        result = gStpOrch->removeStpPorts(port1);
        ASSERT_TRUE(result);

        EXPECT_CALL(mock_sai_stp_,
            remove_stp(_)).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
        result = gStpOrch->removeVlanFromStpInstance(VLAN_1000, stp_instance);
        ASSERT_TRUE(result);

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"Vlan1000", "SET", { {"stp_instance", "1"}}});
        EXPECT_CALL(mock_sai_stp_,
            create_stp(_, _, _, _)).WillOnce(::testing::DoAll(::testing::SetArgPointee<0>(stp_oid),
                                        ::testing::Return(SAI_STATUS_SUCCESS)));

        auto consumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor("STP_VLAN_INSTANCE_TABLE"));
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();

        entries.clear();
        EXPECT_CALL(mock_sai_stp_,
            create_stp_port(_, _, 3, _)).WillOnce(::testing::DoAll(::testing::SetArgPointee<0>(stp_port_oid),
                                        ::testing::Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(mock_sai_stp_,
            set_stp_port_attribute(_,_)).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
        entries.push_back({"Ethernet0:1", "SET", { {"state", "4"}}});
        consumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor("STP_PORT_STATE_TABLE"));
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();

        entries.clear();
        entries.push_back({"Ethernet0:1", "SET", { {"state", "true"}}});
        consumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor("STP_FASTAGEING_FLUSH_TABLE"));
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();

        entries.clear();
        entries.push_back({"Ethernet0:1", "DEL", { {} }});
        EXPECT_CALL(mock_sai_stp_,
            remove_stp_port(_)).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
        consumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor("STP_PORT_STATE_TABLE"));
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();

        entries.clear();
        entries.push_back({"Vlan1000", "DEL", { {} }});
        EXPECT_CALL(mock_sai_stp_,
            remove_stp(_)).WillOnce(::testing::Return(SAI_STATUS_SUCCESS));
        consumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor("STP_VLAN_INSTANCE_TABLE"));
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();

        entries.clear();
        entries.push_back({"1:Ethernet0", "SET", { {"state", "true"} }});
        consumer = dynamic_cast<Consumer*>(gStpOrch->getExecutor(APP_STP_INST_PORT_FLUSH_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();

        _unhook_sai_stp_api();
        _unhook_sai_vlan_api();
        _unhook_sai_fdb_api();
    }

    // A create the hardware refuses (the STP table is full) is not retried on
    // every pass; it waits until an instance has been removed.
    TEST_F(StpOrchTest, TestRefusedInstanceWaitsForARemoval) {
        _hook_sai_stp_api();
        _hook_sai_vlan_api();
        _hook_sai_fdb_api();

        StrictMock<MockSaiStp> mock_sai_stp_;
        mock_sai_stp = &mock_sai_stp_;
        sai_stp_api->create_stp = mock_create_stp;
        sai_stp_api->remove_stp = mock_remove_stp;
        sai_stp_api->create_stp_port = mock_create_stp_port;
        sai_stp_api->remove_stp_port = mock_remove_stp_port;
        sai_stp_api->set_stp_port_attribute = mock_set_stp_port_attribute;

        // A second VLAN, and a bridge port on Ethernet0 so its STP port can
        // be created.
        Table vlan_table = Table(m_app_db.get(), APP_VLAN_TABLE_NAME);
        vlan_table.set(VLAN_2000, { { "admin_status", "up" }, { "mtu", "9100" },
                                    { "mac", "00:aa:bb:cc:dd:ef" } });
        gPortsOrch->addExistingData(&vlan_table);
        static_cast<Orch *>(gPortsOrch)->doTask();
        Port port;
        ASSERT_TRUE(gPortsOrch->getPort(ETHERNET0, port));
        port.m_bridge_port_id = 1234;
        gPortsOrch->setPort(ETHERNET0, port);

        sai_object_id_t stp_oid_1 = 98765;
        sai_object_id_t stp_oid_2 = 98766;
        auto vlanConsumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor("STP_VLAN_INSTANCE_TABLE"));
        auto portConsumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor("STP_PORT_STATE_TABLE"));
        std::deque<KeyOpFieldsValuesTuple> entries;

        ::testing::InSequence seq;

        // Instance 1 is refused once, then left alone however often the
        // consumers run, for the VLAN and for its port state.
        EXPECT_CALL(mock_sai_stp_, create_stp(_, _, _, _)).WillOnce(Return(SAI_STATUS_TABLE_FULL));
        entries.push_back({VLAN_1000, "SET", {{"stp_instance", "1"}}});
        vlanConsumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();

        entries.clear();
        entries.push_back({"Ethernet0:1", "SET", {{"state", "4"}}});
        portConsumer->addToSync(entries);
        for (int i = 0; i < 3; i++)
            static_cast<Orch *>(gStpOrch)->doTask();

        // Instance 2 still gets created, and removing it is what lets
        // instance 1 be tried again, VLAN first and then its port.
        EXPECT_CALL(mock_sai_stp_, create_stp(_, _, _, _))
            .WillOnce(::testing::DoAll(::testing::SetArgPointee<0>(stp_oid_2), Return(SAI_STATUS_SUCCESS)));
        entries.clear();
        entries.push_back({VLAN_2000, "SET", {{"stp_instance", "2"}}});
        vlanConsumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();

        EXPECT_CALL(mock_sai_stp_, remove_stp(stp_oid_2)).WillOnce(Return(SAI_STATUS_SUCCESS));
        EXPECT_CALL(mock_sai_stp_, create_stp(_, _, _, _))
            .WillOnce(::testing::DoAll(::testing::SetArgPointee<0>(stp_oid_1), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(mock_sai_stp_, create_stp_port(_, _, 3, _))
            .WillOnce(::testing::DoAll(::testing::SetArgPointee<0>(67890), Return(SAI_STATUS_SUCCESS)));
        EXPECT_CALL(mock_sai_stp_, set_stp_port_attribute(_, _)).WillOnce(Return(SAI_STATUS_SUCCESS));
        entries.clear();
        entries.push_back({VLAN_2000, "DEL", {{}}});
        vlanConsumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();
        static_cast<Orch *>(gStpOrch)->doTask();

        EXPECT_CALL(mock_sai_stp_, remove_stp_port(_)).WillOnce(Return(SAI_STATUS_SUCCESS));
        EXPECT_CALL(mock_sai_stp_, remove_stp(stp_oid_1)).WillOnce(Return(SAI_STATUS_SUCCESS));
        entries.clear();
        entries.push_back({"Ethernet0:1", "DEL", {{}}});
        portConsumer->addToSync(entries);
        entries.clear();
        entries.push_back({VLAN_1000, "DEL", {{}}});
        vlanConsumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();

        _unhook_sai_stp_api();
        _unhook_sai_vlan_api();
        _unhook_sai_fdb_api();
    }

    // Any other create failure is retried on the next pass, as before.
    TEST_F(StpOrchTest, TestFailedCreateIsRetried) {
        _hook_sai_stp_api();
        _hook_sai_vlan_api();
        _hook_sai_fdb_api();

        StrictMock<MockSaiStp> mock_sai_stp_;
        mock_sai_stp = &mock_sai_stp_;
        sai_stp_api->create_stp = mock_create_stp;
        sai_stp_api->remove_stp = mock_remove_stp;
        sai_stp_api->create_stp_port = mock_create_stp_port;
        sai_stp_api->remove_stp_port = mock_remove_stp_port;
        sai_stp_api->set_stp_port_attribute = mock_set_stp_port_attribute;

        sai_object_id_t stp_oid = 98767;
        auto vlanConsumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor("STP_VLAN_INSTANCE_TABLE"));
        std::deque<KeyOpFieldsValuesTuple> entries;

        ::testing::InSequence seq;

        EXPECT_CALL(mock_sai_stp_, create_stp(_, _, _, _)).WillOnce(Return(SAI_STATUS_FAILURE));
        EXPECT_CALL(mock_sai_stp_, create_stp(_, _, _, _))
            .WillOnce(::testing::DoAll(::testing::SetArgPointee<0>(stp_oid), Return(SAI_STATUS_SUCCESS)));
        entries.push_back({VLAN_1000, "SET", {{"stp_instance", "1"}}});
        vlanConsumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();
        static_cast<Orch *>(gStpOrch)->doTask();
        static_cast<Orch *>(gStpOrch)->doTask();

        EXPECT_CALL(mock_sai_stp_, remove_stp(stp_oid)).WillOnce(Return(SAI_STATUS_SUCCESS));
        entries.clear();
        entries.push_back({VLAN_1000, "DEL", {{}}});
        vlanConsumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();

        _unhook_sai_stp_api();
        _unhook_sai_vlan_api();
        _unhook_sai_fdb_api();
    }

    // ---------------------------------------------------------------------
    // BPDU-guard err-disable: StpOrch consumes APP_STP_PORT_TABLE (stpd's
    // bpdu_guard_shutdown field) and administratively disables/enables the
    // port via PortsOrch::setPortAdminStatusByAlias() - SAI level, no
    // CONFIG_DB writes.
    // ---------------------------------------------------------------------

    TEST_F(StpOrchTest, BpduGuardShutdownAdminDownsPortViaSai) {
        _hook_sai_port_api();

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort(ETHERNET0, port));

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ETHERNET0, "SET", { {"bpdu_guard_shutdown", "yes"} }});
        auto consumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor(APP_STP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();

        ASSERT_TRUE(g_port_admin_status.count(port.m_port_id));
        EXPECT_FALSE(g_port_admin_status[port.m_port_id]);

        // The SET was consumed, not left pending for retry.
        vector<string> ts;
        consumer->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        // Tracking is persisted to STATE_DB, never to CONFIG_DB.
        Table stpStateTable(m_state_db.get(), STATE_STP_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(stpStateTable.get("BPDU_GUARD", fvs));
        ASSERT_EQ(fvs.size(), 1u);
        EXPECT_EQ(fvField(fvs[0]), "err_disabled_ports");
        EXPECT_EQ(fvValue(fvs[0]), ETHERNET0);

        _unhook_sai_port_api();
    }

    TEST_F(StpOrchTest, BpduGuardShutdownNoReleasesPort) {
        _hook_sai_port_api();

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort(ETHERNET0, port));

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ETHERNET0, "SET", { {"bpdu_guard_shutdown", "yes"} }});
        auto consumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor(APP_STP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();
        ASSERT_FALSE(g_port_admin_status[port.m_port_id]);

        entries.clear();
        entries.push_back({ETHERNET0, "SET", { {"bpdu_guard_shutdown", "no"} }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();

        EXPECT_TRUE(g_port_admin_status[port.m_port_id]);

        // Released port drops out of the persisted set entirely.
        Table stpStateTable(m_state_db.get(), STATE_STP_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        EXPECT_FALSE(stpStateTable.get("BPDU_GUARD", fvs));

        _unhook_sai_port_api();
    }

    TEST_F(StpOrchTest, BpduGuardDelWhileTrackedReleasesPort) {
        // STP config removed for a port that's currently err-disabled: nobody
        // owns the disable any more, so StpOrch must hand the port back
        // rather than stranding it down with CONFIG_DB claiming it's up.
        _hook_sai_port_api();

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort(ETHERNET0, port));

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ETHERNET0, "SET", { {"bpdu_guard_shutdown", "yes"} }});
        auto consumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor(APP_STP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();
        ASSERT_FALSE(g_port_admin_status[port.m_port_id]);

        entries.clear();
        entries.push_back({ETHERNET0, "DEL", { {} }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();

        EXPECT_TRUE(g_port_admin_status[port.m_port_id]);

        Table stpStateTable(m_state_db.get(), STATE_STP_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        EXPECT_FALSE(stpStateTable.get("BPDU_GUARD", fvs));

        _unhook_sai_port_api();
    }

    TEST_F(StpOrchTest, BpduGuardShutdownIsIdempotent) {
        // Re-enabling brings the port oper-up, which makes stpd republish
        // bpdu_guard_shutdown="no". The tracking set must make repeated
        // "yes"/"no" for a port that's already in that state a no-op, or the
        // release -> oper-up -> republish sequence would loop forever.
        _hook_sai_port_api();

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort(ETHERNET0, port));

        auto consumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor(APP_STP_PORT_TABLE_NAME));

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ETHERNET0, "SET", { {"bpdu_guard_shutdown", "yes"} }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();
        int callsAfterFirstShutdown = g_port_admin_status_call_count;
        ASSERT_GT(callsAfterFirstShutdown, 0);

        entries.clear();
        entries.push_back({ETHERNET0, "SET", { {"bpdu_guard_shutdown", "yes"} }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();
        EXPECT_EQ(g_port_admin_status_call_count, callsAfterFirstShutdown);

        entries.clear();
        entries.push_back({ETHERNET0, "SET", { {"bpdu_guard_shutdown", "no"} }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();
        int callsAfterFirstRelease = g_port_admin_status_call_count;
        EXPECT_GT(callsAfterFirstRelease, callsAfterFirstShutdown);

        entries.clear();
        entries.push_back({ETHERNET0, "SET", { {"bpdu_guard_shutdown", "no"} }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();
        EXPECT_EQ(g_port_admin_status_call_count, callsAfterFirstRelease);

        _unhook_sai_port_api();
    }

    TEST_F(StpOrchTest, BpduGuardUpdateWithoutShutdownFieldIsIgnored) {
        // stpd publishes only the fields it changed. An update that doesn't
        // carry bpdu_guard_shutdown expresses no opinion about err-disable
        // and must not touch admin state.
        _hook_sai_port_api();

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ETHERNET0, "SET", { {"some_other_field", "x"} }});
        auto consumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor(APP_STP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();

        EXPECT_EQ(g_port_admin_status_call_count, 0);

        vector<string> ts;
        consumer->dumpPendingTasks(ts);
        ASSERT_TRUE(ts.empty());

        _unhook_sai_port_api();
    }

    TEST_F(StpOrchTest, BpduGuardShutdownOnLagCascadesToMembers) {
        // stpd err-disables PortChannels too. A LAG owns no SAI port object,
        // so setPortAdminStatusByAlias() must cascade to member ports rather
        // than handing the LAG OID to sai_port_api directly.
        _hook_sai_port_api();

        Table lagTable(m_app_db.get(), APP_LAG_TABLE_NAME);
        Table lagMemberTable(m_app_db.get(), APP_LAG_MEMBER_TABLE_NAME);

        lagTable.set("PortChannel0001", { {"admin_status", "up"}, {"mtu", "9100"} });
        lagMemberTable.set(
            string("PortChannel0001") + lagMemberTable.getTableNameSeparator() + ETHERNET4,
            { {"status", "enabled"} });
        lagMemberTable.set(
            string("PortChannel0001") + lagMemberTable.getTableNameSeparator() + ETHERNET8,
            { {"status", "enabled"} });

        gPortsOrch->addExistingData(&lagTable);
        gPortsOrch->addExistingData(&lagMemberTable);
        static_cast<Orch *>(gPortsOrch)->doTask();

        Port lagPort, member4, member8;
        ASSERT_TRUE(gPortsOrch->getPort("PortChannel0001", lagPort));
        ASSERT_TRUE(gPortsOrch->getPort(ETHERNET4, member4));
        ASSERT_TRUE(gPortsOrch->getPort(ETHERNET8, member8));
        ASSERT_EQ(lagPort.m_type, Port::LAG);

        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({"PortChannel0001", "SET", { {"bpdu_guard_shutdown", "yes"} }});
        auto consumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor(APP_STP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();

        EXPECT_FALSE(g_port_admin_status[member4.m_port_id]);
        EXPECT_FALSE(g_port_admin_status[member8.m_port_id]);
        // The LAG itself owns no SAI port object; only its members are
        // touched at the SAI level.
        EXPECT_EQ(g_port_admin_status.count(lagPort.m_port_id), 0u);

        _unhook_sai_port_api();
    }

    TEST_F(StpOrchTest, BpduGuardStateRestoredAfterRestart) {
        // Simulate an orchagent restart: STATE_DB already tracks Ethernet0 as
        // err-disabled from a prior lifetime (stpd still holds it in
        // protect_disabled_mask), but this fresh StpOrch's in-memory tracking
        // set knows nothing about it yet. The first doTask() dispatch must
        // re-assert the disable before doing anything else.
        _hook_sai_port_api();

        Table stpStateTable(m_state_db.get(), STATE_STP_TABLE_NAME);
        stpStateTable.set("BPDU_GUARD", { {"err_disabled_ports", ETHERNET0} });

        Port port0;
        ASSERT_TRUE(gPortsOrch->getPort(ETHERNET0, port0));
        ASSERT_EQ(g_port_admin_status.count(port0.m_port_id), 0u);

        // Restore only fires from inside StpOrch::doTask(Consumer&), which
        // only runs when some consumer has pending work. Drive an unrelated
        // port's request so the dispatch actually happens.
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ETHERNET4, "SET", { {"bpdu_guard_shutdown", "yes"} }});
        auto consumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor(APP_STP_PORT_TABLE_NAME));
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();

        Port port4;
        ASSERT_TRUE(gPortsOrch->getPort(ETHERNET4, port4));

        // Re-asserted on restart (Ethernet0) and freshly requested (Ethernet4)
        // both land admin-down via SAI.
        ASSERT_TRUE(g_port_admin_status.count(port0.m_port_id));
        EXPECT_FALSE(g_port_admin_status[port0.m_port_id]);
        EXPECT_FALSE(g_port_admin_status[port4.m_port_id]);

        vector<FieldValueTuple> fvs;
        ASSERT_TRUE(stpStateTable.get("BPDU_GUARD", fvs));
        ASSERT_EQ(fvs.size(), 1u);
        EXPECT_EQ(fvField(fvs[0]), "err_disabled_ports");
        string joined = fvValue(fvs[0]);
        EXPECT_NE(joined.find(ETHERNET0), string::npos);
        EXPECT_NE(joined.find(ETHERNET4), string::npos);

        _unhook_sai_port_api();
    }

    TEST_F(StpOrchTest, BpduGuardConfigAdminUpDropsRecordAndReArms) {
        // config interface shutdown then startup brings an err-disabled port
        // up through PortsOrch. The tracking record has to go with it, or the
        // next trip from stpd is skipped as already applied and the port
        // keeps forwarding.
        _hook_sai_port_api();

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort(ETHERNET0, port));

        auto consumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor(APP_STP_PORT_TABLE_NAME));
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ETHERNET0, "SET", { {"bpdu_guard_shutdown", "yes"} }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();
        EXPECT_FALSE(g_port_admin_status[port.m_port_id]);
        EXPECT_TRUE(gPortsOrch->isAdminDownHeld(ETHERNET0));

        // A write that still says up (an MTU edit, a reload replay) is not an
        // operator bringing the port up: the hold stays.
        Table port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
        port_table.set(ETHERNET0, { {"admin_status", "up"}, {"mtu", "9100"} });
        gPortsOrch->addExistingData(&port_table);
        static_cast<Orch *>(gPortsOrch)->doTask();
        EXPECT_FALSE(g_port_admin_status[port.m_port_id]);
        EXPECT_TRUE(gPortsOrch->isAdminDownHeld(ETHERNET0));

        // The operator's shutdown, then startup.
        port_table.set(ETHERNET0, { {"admin_status", "down"} });
        gPortsOrch->addExistingData(&port_table);
        static_cast<Orch *>(gPortsOrch)->doTask();
        port_table.set(ETHERNET0, { {"admin_status", "up"} });
        gPortsOrch->addExistingData(&port_table);
        static_cast<Orch *>(gPortsOrch)->doTask();

        EXPECT_TRUE(g_port_admin_status[port.m_port_id]);
        EXPECT_FALSE(gPortsOrch->isAdminDownHeld(ETHERNET0));
        Table stpStateTable(m_state_db.get(), STATE_STP_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        EXPECT_FALSE(stpStateTable.get("BPDU_GUARD", fvs));

        // The guard is still armed in stpd: its next trip must shut the port again.
        int calls = g_port_admin_status_call_count;
        entries.clear();
        entries.push_back({ETHERNET0, "SET", { {"bpdu_guard_shutdown", "yes"} }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();
        EXPECT_GT(g_port_admin_status_call_count, calls);
        EXPECT_FALSE(g_port_admin_status[port.m_port_id]);
        EXPECT_TRUE(stpStateTable.get("BPDU_GUARD", fvs));

        _unhook_sai_port_api();
    }

    TEST_F(StpOrchTest, BpduGuardHoldSurvivesAPortBounce) {
        // A speed, FEC or autoneg change brings the port down and back up
        // around the write. On an err-disabled port that bounce is not the
        // operator's startup: the port stays down, held, and recorded.
        _hook_sai_port_api();

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort(ETHERNET0, port));
        ASSERT_NE(port.m_speed, 10000u);

        auto consumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor(APP_STP_PORT_TABLE_NAME));
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ETHERNET0, "SET", { {"bpdu_guard_shutdown", "yes"} }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();
        EXPECT_FALSE(g_port_admin_status[port.m_port_id]);
        EXPECT_TRUE(gPortsOrch->isAdminDownHeld(ETHERNET0));

        Table port_table(m_app_db.get(), APP_PORT_TABLE_NAME);
        port_table.set(ETHERNET0, { {"admin_status", "up"}, {"speed", "10000"} });
        gPortsOrch->addExistingData(&port_table);
        static_cast<Orch *>(gPortsOrch)->doTask();

        ASSERT_TRUE(gPortsOrch->getPort(ETHERNET0, port));
        EXPECT_EQ(port.m_speed, 10000u);
        EXPECT_FALSE(g_port_admin_status[port.m_port_id]);
        EXPECT_TRUE(gPortsOrch->isAdminDownHeld(ETHERNET0));
        Table stpStateTable(m_state_db.get(), STATE_STP_TABLE_NAME);
        vector<FieldValueTuple> fvs;
        EXPECT_TRUE(stpStateTable.get("BPDU_GUARD", fvs));

        // A second unrelated write must not release it either.
        port_table.set(ETHERNET0, { {"admin_status", "up"}, {"mtu", "9100"} });
        gPortsOrch->addExistingData(&port_table);
        static_cast<Orch *>(gPortsOrch)->doTask();
        EXPECT_FALSE(g_port_admin_status[port.m_port_id]);
        EXPECT_TRUE(gPortsOrch->isAdminDownHeld(ETHERNET0));

        // The operator's shutdown, then startup, still releases it.
        port_table.set(ETHERNET0, { {"admin_status", "down"} });
        gPortsOrch->addExistingData(&port_table);
        static_cast<Orch *>(gPortsOrch)->doTask();
        port_table.set(ETHERNET0, { {"admin_status", "up"} });
        gPortsOrch->addExistingData(&port_table);
        static_cast<Orch *>(gPortsOrch)->doTask();
        EXPECT_TRUE(g_port_admin_status[port.m_port_id]);
        EXPECT_FALSE(gPortsOrch->isAdminDownHeld(ETHERNET0));
        EXPECT_FALSE(stpStateTable.get("BPDU_GUARD", fvs));

        _unhook_sai_port_api();
    }

    TEST_F(StpOrchTest, BpduGuardShutdownReappliesWhenHoldIsGone) {
        // A port that lost its hold without StpOrch hearing about it (the port
        // went away and came back) is up while the record still says shut.
        // The record alone must not make the next trip a no-op.
        _hook_sai_port_api();

        Port port;
        ASSERT_TRUE(gPortsOrch->getPort(ETHERNET0, port));

        auto consumer = dynamic_cast<Consumer *>(gStpOrch->getExecutor(APP_STP_PORT_TABLE_NAME));
        std::deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({ETHERNET0, "SET", { {"bpdu_guard_shutdown", "yes"} }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();
        EXPECT_FALSE(g_port_admin_status[port.m_port_id]);

        ASSERT_TRUE(gPortsOrch->setPortAdminStatusByAlias(
            ETHERNET0, true, PortsOrch::ADMIN_DOWN_HOLDER_STP_BPDU_GUARD));
        EXPECT_TRUE(g_port_admin_status[port.m_port_id]);

        int calls = g_port_admin_status_call_count;
        entries.clear();
        entries.push_back({ETHERNET0, "SET", { {"bpdu_guard_shutdown", "yes"} }});
        consumer->addToSync(entries);
        static_cast<Orch *>(gStpOrch)->doTask();
        EXPECT_GT(g_port_admin_status_call_count, calls);
        EXPECT_FALSE(g_port_admin_status[port.m_port_id]);

        _unhook_sai_port_api();
    }
}
