// Unit tests for MacMoveGuardOrch.
//
// These tests exercise the behaviors enumerated in MAC_MOVE_GUARD_HLD.md
// section 11.1. The orchestrator is instantiated directly against a mocked
// CONFIG_DB / STATE_DB and a real PortsOrch / FdbOrch built on the VS SAI.
// MAC move/learn events are delivered to the orch by calling its update()
// observer entrypoint with synthesized notifications so we don't need to
// drive SAI FDB events end-to-end.
//
// SAI ACL/switch/port APIs are hooked at the table level so we can:
//   - record ACL table/entry create/remove operations
//   - control whether SAI_ACL_ACTION_TYPE_SET_DO_NOT_LEARN appears in the
//     PRE_INGRESS action capability list (supported vs. soft-disabled paths)
//   - succeed admin-status writes on synthetic ports (no real ASIC)

#include "../ut_helper.h"
#include "../mock_orchagent_main.h"
#include "../mock_table.h"

#include "port.h"

#define private public
#define protected public
#include "portsorch.h"
#include "fdborch.h"
#include "macmoveguardorch.h"
#include "crmorch.h"
#undef protected
#undef private

#include <chrono>
#include <thread>

extern CrmOrch *gCrmOrch;

namespace macmoveguardorch_test
{
    using namespace std;
    using namespace swss;
    using namespace std::chrono;

    static const string ETH0  = "Ethernet0";
    static const string ETH1  = "Ethernet4";
    static const string ETH2  = "Ethernet8";
    static const string VLAN40 = "Vlan40";

    static const string MAC_A = "00:11:22:33:44:01";
    static const string MAC_B = "00:11:22:33:44:02";

    static const sai_object_id_t VLAN40_OID    = 0x26000000000796ULL;
    static const sai_object_id_t ETH0_OID      = 0x10000000004a4ULL;
    static const sai_object_id_t ETH1_OID      = 0x10000000004a5ULL;
    static const sai_object_id_t ETH2_OID      = 0x10000000004a6ULL;
    static const sai_object_id_t ETH0_BPORT_ID = 0x3a000000002c33ULL;
    static const sai_object_id_t ETH1_BPORT_ID = 0x3a000000002c34ULL;
    static const sai_object_id_t ETH2_BPORT_ID = 0x3a000000002c35ULL;

    // ---------- SAI hooks: switch, ACL, port ----------
    //
    // The orch under test relies on three SAI surfaces. We hook them so the
    // tests can:
    //   - flip platform capability for SET_DO_NOT_LEARN at PRE_INGRESS
    //   - track ACL table/entry lifecycle without needing real ACL objects
    //   - record admin-status writes for arbitrary fake port OIDs

    static sai_switch_api_t  ut_sai_switch_api;
    static sai_switch_api_t *pold_sai_switch_api = nullptr;
    static sai_acl_api_t     ut_sai_acl_api;
    static sai_acl_api_t    *pold_sai_acl_api    = nullptr;
    static sai_port_api_t    ut_sai_port_api;
    static sai_port_api_t   *pold_sai_port_api   = nullptr;

    // Capability toggle for SAI_ACL_ACTION_TYPE_SET_DO_NOT_LEARN at PRE_INGRESS.
    static bool g_set_do_not_learn_supported = true;
    // The OID currently bound to SAI_SWITCH_ATTR_PRE_INGRESS_ACL (we mock it).
    static sai_object_id_t g_pre_ingress_acl_bound = SAI_NULL_OBJECT_ID;

    // Counters used by tests to assert on lifecycle calls.
    static int g_acl_table_create_count = 0;
    static int g_acl_table_remove_count = 0;
    static int g_acl_entry_create_count = 0;
    static int g_acl_entry_remove_count = 0;
    static sai_object_id_t g_acl_oid_next = 0xa1000000ULL;

    // Per-port admin status: alias -> last requested admin state (true=UP).
    static map<sai_object_id_t, bool> g_port_admin_status;

    static sai_status_t _stub_get_switch_attribute(sai_object_id_t switch_id,
                                                   uint32_t attr_count,
                                                   sai_attribute_t *attr_list)
    {
        if (attr_count == 1)
        {
            switch (attr_list[0].id)
            {
                case SAI_SWITCH_ATTR_MAX_ACL_ACTION_COUNT:
                    attr_list[0].value.u32 = 8;
                    return SAI_STATUS_SUCCESS;

                case SAI_SWITCH_ATTR_ACL_STAGE_PRE_INGRESS:
                {
                    auto &cap = attr_list[0].value.aclcapability;
                    if (g_set_do_not_learn_supported && cap.action_list.list)
                    {
                        cap.action_list.list[0] = SAI_ACL_ACTION_TYPE_SET_DO_NOT_LEARN;
                        cap.action_list.count = 1;
                    }
                    else
                    {
                        cap.action_list.count = 0;
                    }
                    cap.is_action_list_mandatory = false;
                    return SAI_STATUS_SUCCESS;
                }

                case SAI_SWITCH_ATTR_PRE_INGRESS_ACL:
                    attr_list[0].value.oid = g_pre_ingress_acl_bound;
                    return SAI_STATUS_SUCCESS;
            }
        }
        return pold_sai_switch_api->get_switch_attribute(switch_id, attr_count, attr_list);
    }

    static sai_status_t _stub_set_switch_attribute(sai_object_id_t switch_id,
                                                   const sai_attribute_t *attr)
    {
        if (attr && attr->id == SAI_SWITCH_ATTR_PRE_INGRESS_ACL)
        {
            g_pre_ingress_acl_bound = attr->value.oid;
            return SAI_STATUS_SUCCESS;
        }
        return pold_sai_switch_api->set_switch_attribute(switch_id, attr);
    }

    static sai_status_t _stub_create_acl_table(sai_object_id_t *oid,
                                               sai_object_id_t,
                                               uint32_t, const sai_attribute_t *)
    {
        *oid = ++g_acl_oid_next;
        ++g_acl_table_create_count;
        return SAI_STATUS_SUCCESS;
    }

    static sai_status_t _stub_remove_acl_table(sai_object_id_t)
    {
        ++g_acl_table_remove_count;
        return SAI_STATUS_SUCCESS;
    }

    static sai_status_t _stub_create_acl_entry(sai_object_id_t *oid,
                                               sai_object_id_t,
                                               uint32_t, const sai_attribute_t *)
    {
        *oid = ++g_acl_oid_next;
        ++g_acl_entry_create_count;
        return SAI_STATUS_SUCCESS;
    }

    static sai_status_t _stub_remove_acl_entry(sai_object_id_t)
    {
        ++g_acl_entry_remove_count;
        return SAI_STATUS_SUCCESS;
    }

    static sai_status_t _stub_set_port_attribute(sai_object_id_t port_id,
                                                 const sai_attribute_t *attr)
    {
        if (attr && attr->id == SAI_PORT_ATTR_ADMIN_STATE)
        {
            g_port_admin_status[port_id] = attr->value.booldata;
            return SAI_STATUS_SUCCESS;
        }
        return pold_sai_port_api->set_port_attribute(port_id, attr);
    }

    static void _hook_apis()
    {
        ut_sai_switch_api  = *sai_switch_api;
        pold_sai_switch_api = sai_switch_api;
        ut_sai_switch_api.get_switch_attribute = _stub_get_switch_attribute;
        ut_sai_switch_api.set_switch_attribute = _stub_set_switch_attribute;
        sai_switch_api = &ut_sai_switch_api;

        ut_sai_acl_api  = *sai_acl_api;
        pold_sai_acl_api = sai_acl_api;
        ut_sai_acl_api.create_acl_table  = _stub_create_acl_table;
        ut_sai_acl_api.remove_acl_table  = _stub_remove_acl_table;
        ut_sai_acl_api.create_acl_entry  = _stub_create_acl_entry;
        ut_sai_acl_api.remove_acl_entry  = _stub_remove_acl_entry;
        sai_acl_api = &ut_sai_acl_api;

        ut_sai_port_api = *sai_port_api;
        pold_sai_port_api = sai_port_api;
        ut_sai_port_api.set_port_attribute = _stub_set_port_attribute;
        sai_port_api = &ut_sai_port_api;
    }

    static void _unhook_apis()
    {
        sai_switch_api = pold_sai_switch_api;
        sai_acl_api    = pold_sai_acl_api;
        sai_port_api   = pold_sai_port_api;
    }

    static void _reset_counters()
    {
        g_acl_table_create_count = 0;
        g_acl_table_remove_count = 0;
        g_acl_entry_create_count = 0;
        g_acl_entry_remove_count = 0;
        g_pre_ingress_acl_bound  = SAI_NULL_OBJECT_ID;
        g_set_do_not_learn_supported = true;
        g_port_admin_status.clear();
    }

    // ---------- Test fixture ----------

    struct MacMoveGuardOrchTest : public ::testing::Test
    {
        shared_ptr<DBConnector> m_config_db;
        shared_ptr<DBConnector> m_app_db;
        shared_ptr<DBConnector> m_state_db;
        shared_ptr<DBConnector> m_asic_db;
        shared_ptr<DBConnector> m_chassis_app_db;

        shared_ptr<PortsOrch>        m_portsOrch;
        shared_ptr<FdbOrch>          m_fdbOrch;
        shared_ptr<MacMoveGuardOrch> m_mmg;

        void SetUp() override
        {
            testing_db::reset();
            _reset_counters();

            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE",     "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS",  "20:03:04:05:06:00" }
            };
            ut_helper::initSaiApi(profile);

            sai_attribute_t attr;
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;
            ASSERT_EQ(sai_switch_api->create_switch(&gSwitchId, 1, &attr), SAI_STATUS_SUCCESS);

            _hook_apis();

            m_config_db      = make_shared<DBConnector>("CONFIG_DB", 0);
            m_app_db         = make_shared<DBConnector>("APPL_DB",   0);
            m_state_db       = make_shared<DBConnector>("STATE_DB",  0);
            m_asic_db        = make_shared<DBConnector>("ASIC_DB",   0);

            TableConnector stateDbSwitchTable(m_state_db.get(), "SWITCH_CAPABILITY");
            TableConnector app_switch_table  (m_app_db.get(),   APP_SWITCH_TABLE_NAME);
            TableConnector conf_asic_sensors (m_config_db.get(), CFG_ASIC_SENSORS_TABLE_NAME);
            vector<TableConnector> switch_tables = { conf_asic_sensors, app_switch_table };

            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(m_app_db.get(), switch_tables, stateDbSwitchTable);

            const int portsorch_base_pri = 40;
            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME,        portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME,        portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri },
                { APP_LAG_TABLE_NAME,         portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME,  portsorch_base_pri }
            };
            m_portsOrch = make_shared<PortsOrch>(m_app_db.get(), m_state_db.get(),
                                                 ports_tables, m_chassis_app_db.get());

            ASSERT_EQ(gCrmOrch, nullptr);
            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);

            vector<table_name_with_pri_t> app_fdb_tables = {
                { APP_FDB_TABLE_NAME,        FdbOrch::fdborch_pri },
                { APP_VXLAN_FDB_TABLE_NAME,  FdbOrch::fdborch_pri },
                { APP_MCLAG_FDB_TABLE_NAME,  FdbOrch::fdborch_pri }
            };
            TableConnector stateDbFdb(m_state_db.get(), STATE_FDB_TABLE_NAME);
            TableConnector stateMclagDbFdb(m_state_db.get(), STATE_MCLAG_REMOTE_FDB_TABLE_NAME);
            m_fdbOrch = make_shared<FdbOrch>(m_app_db.get(), app_fdb_tables,
                                             stateDbFdb, stateMclagDbFdb, m_portsOrch.get());

            seedPortsAndVlan();
        }

        void TearDown() override
        {
            m_mmg.reset();
            m_fdbOrch.reset();
            m_portsOrch.reset();

            delete gCrmOrch;    gCrmOrch    = nullptr;
            delete gSwitchOrch; gSwitchOrch = nullptr;

            _unhook_apis();

            sai_switch_api->remove_switch(gSwitchId);
            gSwitchId = SAI_NULL_OBJECT_ID;
            ut_helper::uninitSaiApi();
        }

        // Build the orch under test. Done as a separate step (not in SetUp)
        // so individual tests can pre-populate STATE_DB before the orch's
        // constructor runs restoreBadMacState().
        void buildOrch()
        {
            m_mmg = make_shared<MacMoveGuardOrch>(
                m_config_db.get(), m_state_db.get(),
                CFG_MAC_MOVE_GUARD_TABLE_NAME,
                m_portsOrch.get(), m_fdbOrch.get());
        }

        void seedPortsAndVlan()
        {
            // VLAN cache. The bv_id we use in test notifications matches the
            // VLAN's vlan_oid so PortsOrch::getPort(bv_id, ...) resolves.
            Port vlan(VLAN40, Port::VLAN);
            vlan.m_vlan_info.vlan_oid = VLAN40_OID;
            vlan.m_vlan_info.vlan_id  = 40;
            m_portsOrch->m_portList[VLAN40] = vlan;
            m_portsOrch->saiOidToAlias[VLAN40_OID] = VLAN40;

            auto seedPort = [&](const string &alias, sai_object_id_t oid,
                                sai_object_id_t bport)
            {
                Port p(alias, Port::PHY);
                p.m_port_id = oid;
                p.m_bridge_port_id = bport;
                p.m_admin_state_up = true;
                m_portsOrch->m_portList[alias] = p;
                m_portsOrch->saiOidToAlias[oid]   = alias;
                m_portsOrch->saiOidToAlias[bport] = alias;
                g_port_admin_status[oid] = true;
            };
            seedPort(ETH0, ETH0_OID, ETH0_BPORT_ID);
            seedPort(ETH1, ETH1_OID, ETH1_BPORT_ID);
            seedPort(ETH2, ETH2_OID, ETH2_BPORT_ID);
        }

        // Push a SET on the MAC_MOVE_GUARD CONFIG_DB table through the orch's
        // doTask(Consumer&) handler. Modeled on copporch_ut.cpp.
        void configure(const vector<FieldValueTuple> &fvs)
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_config_db.get(),
                                             CFG_MAC_MOVE_GUARD_TABLE_NAME, 1, 1),
                m_mmg.get(), CFG_MAC_MOVE_GUARD_TABLE_NAME));

            KeyOpFieldsValuesTuple kfv;
            kfvKey(kfv) = "GLOBAL";
            kfvOp(kfv)  = SET_COMMAND;
            kfvFieldsValues(kfv) = fvs;
            consumer->addToSync({ kfv });
            static_cast<Orch *>(m_mmg.get())->doTask(*consumer.get());
        }

        // As above but lets the caller pick the key (used for negative tests).
        void configureWithKey(const string &key, const vector<FieldValueTuple> &fvs)
        {
            auto consumer = unique_ptr<Consumer>(new Consumer(
                new swss::ConsumerStateTable(m_config_db.get(),
                                             CFG_MAC_MOVE_GUARD_TABLE_NAME, 1, 1),
                m_mmg.get(), CFG_MAC_MOVE_GUARD_TABLE_NAME));
            KeyOpFieldsValuesTuple kfv;
            kfvKey(kfv) = key;
            kfvOp(kfv)  = SET_COMMAND;
            kfvFieldsValues(kfv) = fvs;
            consumer->addToSync({ kfv });
            static_cast<Orch *>(m_mmg.get())->doTask(*consumer.get());
        }

        // Helpers to inject MAC events into the orch the way FdbOrch would.
        void injectMove(const string &mac, const string &old_alias,
                        const string &new_alias, sai_object_id_t bv_id = VLAN40_OID)
        {
            MacMoveNotification n;
            n.mac = MacAddress(mac);
            n.bv_id = bv_id;
            n.port_old = m_portsOrch->m_portList[old_alias];
            n.port_new = m_portsOrch->m_portList[new_alias];
            m_mmg->update(SUBJECT_TYPE_MAC_MOVE, &n);
        }

        void injectLearn(const string &mac, const string &alias,
                         sai_object_id_t bv_id = VLAN40_OID)
        {
            MacLearnNotification n;
            n.mac = MacAddress(mac);
            n.bv_id = bv_id;
            n.port = m_portsOrch->m_portList[alias];
            m_mmg->update(SUBJECT_TYPE_MAC_LEARN, &n);
        }

        // Look up tracking state by string MAC (the test always uses VLAN40).
        MacMoveTrackingState &state(const string &mac)
        {
            MacKey k{ MacAddress(mac), VLAN40_OID };
            return m_mmg->m_macTrackingState[k];
        }

        bool tracked(const string &mac)
        {
            MacKey k{ MacAddress(mac), VLAN40_OID };
            return m_mmg->m_macTrackingState.find(k) != m_mmg->m_macTrackingState.end();
        }
    };

    // -------- 11.1 #1: native move threshold ----------
    TEST_F(MacMoveGuardOrchTest, ThresholdTripsOnNativeMoves)
    {
        buildOrch();
        configure({
            {"enabled","true"}, {"threshold","3"}, {"detect_interval","60"},
            {"action_interval","60"}, {"action","DISABLE_PORT"}
        });

        injectMove(MAC_A, ETH0, ETH1);
        injectMove(MAC_A, ETH1, ETH0);
        EXPECT_FALSE(state(MAC_A).is_bad_mac);

        injectMove(MAC_A, ETH0, ETH1);
        EXPECT_TRUE(state(MAC_A).is_bad_mac);
        EXPECT_EQ(state(MAC_A).action, MacMoveGuardAction::DISABLE_PORT);
    }

    // -------- 11.1 #2: synthesized move from alternating LEARNs ----------
    TEST_F(MacMoveGuardOrchTest, ThresholdTripsOnSynthesizedMovesFromLearns)
    {
        buildOrch();
        configure({
            {"enabled","true"}, {"threshold","3"}, {"detect_interval","60"},
            {"action_interval","60"}, {"action","DISABLE_PORT"}
        });

        // First LEARN on ETH0: no move yet (empty prev_port).
        injectLearn(MAC_A, ETH0);
        EXPECT_FALSE(tracked(MAC_A));   // no move recorded yet

        // Each subsequent LEARN on a different port should synthesize a move.
        injectLearn(MAC_A, ETH1);
        injectLearn(MAC_A, ETH0);
        injectLearn(MAC_A, ETH1);

        ASSERT_TRUE(tracked(MAC_A));
        EXPECT_TRUE(state(MAC_A).is_bad_mac);
        EXPECT_GE(state(MAC_A).move_count, 3u);
    }

    // -------- 11.1 #3: sliding window forgets old moves ----------
    TEST_F(MacMoveGuardOrchTest, SlidingWindowForgetsOldMoves)
    {
        buildOrch();
        // Tiny detect_interval so we can sleep through it within the test.
        configure({
            {"enabled","true"}, {"threshold","3"}, {"detect_interval","1"},
            {"action_interval","60"}, {"action","DISABLE_PORT"}
        });

        injectMove(MAC_A, ETH0, ETH1);
        injectMove(MAC_A, ETH1, ETH0);
        EXPECT_FALSE(state(MAC_A).is_bad_mac);

        std::this_thread::sleep_for(std::chrono::milliseconds(1100));

        injectMove(MAC_A, ETH0, ETH1);
        injectMove(MAC_A, ETH1, ETH0);

        // First two are outside the window; pruning brings count back to 2.
        EXPECT_FALSE(state(MAC_A).is_bad_mac);
        EXPECT_LE(state(MAC_A).move_count, 2u);
    }

    // -------- 11.1 #4: DISABLE_PORT pinning + refcounting ----------
    TEST_F(MacMoveGuardOrchTest, DisablePortPinningAndRefcount)
    {
        buildOrch();
        configure({
            {"enabled","true"}, {"threshold","2"}, {"detect_interval","60"},
            {"action_interval","60"}, {"action","DISABLE_PORT"}
        });

        // Two MACs both bouncing between ETH0 and ETH1.
        injectMove(MAC_A, ETH0, ETH1);
        injectMove(MAC_A, ETH1, ETH0);
        injectMove(MAC_B, ETH0, ETH1);
        injectMove(MAC_B, ETH1, ETH0);

        EXPECT_TRUE(state(MAC_A).is_bad_mac);
        EXPECT_TRUE(state(MAC_B).is_bad_mac);

        // Each bad MAC pins one port and disables the other; both MACs picked
        // the same non-disabled port to pin (the first one in ports_seen). So
        // exactly one port is disabled and refcounted twice.
        ASSERT_EQ(m_mmg->m_disabledPorts.size(), 1u);
        auto &refset = m_mmg->m_disabledPorts.begin()->second;
        EXPECT_EQ(refset.size(), 2u);

        // The disabled port should be admin-down in SAI.
        const string &disabled_alias = m_mmg->m_disabledPorts.begin()->first;
        sai_object_id_t disabled_oid = m_portsOrch->m_portList[disabled_alias].m_port_id;
        EXPECT_FALSE(g_port_admin_status[disabled_oid]);
    }

    // -------- 11.1 #5: DISABLE_PORT recovery + shared refcount ----------
    TEST_F(MacMoveGuardOrchTest, DisablePortRecoveryRespectsRefcount)
    {
        buildOrch();
        configure({
            {"enabled","true"}, {"threshold","2"}, {"detect_interval","60"},
            {"action_interval","60"}, {"action","DISABLE_PORT"}
        });

        injectMove(MAC_A, ETH0, ETH1);
        injectMove(MAC_A, ETH1, ETH0);
        injectMove(MAC_B, ETH0, ETH1);
        injectMove(MAC_B, ETH1, ETH0);

        ASSERT_TRUE(state(MAC_A).is_bad_mac);
        ASSERT_TRUE(state(MAC_B).is_bad_mac);
        ASSERT_EQ(m_mmg->m_disabledPorts.size(), 1u);
        const string disabled_alias = m_mmg->m_disabledPorts.begin()->first;
        sai_object_id_t disabled_oid = m_portsOrch->m_portList[disabled_alias].m_port_id;

        // Force MAC_A's action to expire; MAC_B still requires the port down.
        state(MAC_A).action_expiry_time = steady_clock::now() - seconds(1);
        m_mmg->checkRecovery();

        EXPECT_FALSE(state(MAC_A).is_bad_mac);
        EXPECT_TRUE (state(MAC_B).is_bad_mac);
        EXPECT_FALSE(g_port_admin_status[disabled_oid]);   // still down

        // Expire MAC_B too: now the last reference goes away, port re-enabled.
        state(MAC_B).action_expiry_time = steady_clock::now() - seconds(1);
        m_mmg->checkRecovery();

        EXPECT_FALSE(state(MAC_B).is_bad_mac);
        EXPECT_TRUE(g_port_admin_status[disabled_oid]);
        EXPECT_EQ(m_mmg->m_disabledPorts.count(disabled_alias), 0u);
    }

    // -------- 11.1 #6: DLOMWA capability supported -> table created/bound ---
    TEST_F(MacMoveGuardOrchTest, DlomwaCreatesAndBindsTableWhenSupported)
    {
        buildOrch();
        g_set_do_not_learn_supported = true;

        configure({
            {"enabled","true"}, {"threshold","100"}, {"detect_interval","60"},
            {"action_interval","60"}, {"action","DISABLE_LEARN_ON_MAC_WITH_ACL"}
        });

        EXPECT_NE(m_mmg->m_learnDisableAclTable, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(g_acl_table_create_count, 1);
        EXPECT_EQ(g_pre_ingress_acl_bound, m_mmg->m_learnDisableAclTable);
        EXPECT_EQ(m_mmg->m_aclSetDoNotLearnSupported, 1);
    }

    // -------- 11.1 #7: DLOMWA capability unsupported -> soft-disabled ------
    TEST_F(MacMoveGuardOrchTest, DlomwaSoftDisabledWhenCapabilityMissing)
    {
        buildOrch();
        g_set_do_not_learn_supported = false;

        configure({
            {"enabled","true"}, {"threshold","2"}, {"detect_interval","60"},
            {"action_interval","60"}, {"action","DISABLE_LEARN_ON_MAC_WITH_ACL"}
        });

        // No table created, no switch bind, capability cached as unsupported.
        EXPECT_EQ(m_mmg->m_learnDisableAclTable, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(g_acl_table_create_count, 0);
        EXPECT_EQ(g_pre_ingress_acl_bound, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(m_mmg->m_aclSetDoNotLearnSupported, 0);

        // Detection still runs; bad MAC is persisted to STATE_DB though no
        // ACL entry is programmed.
        injectMove(MAC_A, ETH0, ETH1);
        injectMove(MAC_A, ETH1, ETH0);
        ASSERT_TRUE(state(MAC_A).is_bad_mac);
        EXPECT_EQ(state(MAC_A).learn_disable_acl_entry_id, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(g_acl_entry_create_count, 0);

        // STATE_DB row was written via persistBadMac().
        vector<string> keys;
        m_mmg->m_stateBadMacTable->getKeys(keys);
        EXPECT_EQ(keys.size(), 1u);
    }

    // -------- 11.1 #8: DLOMWA entry lifecycle ------------------------------
    TEST_F(MacMoveGuardOrchTest, DlomwaEntryLifecycle)
    {
        buildOrch();
        g_set_do_not_learn_supported = true;

        configure({
            {"enabled","true"}, {"threshold","2"}, {"detect_interval","60"},
            {"action_interval","60"}, {"action","DISABLE_LEARN_ON_MAC_WITH_ACL"}
        });
        sai_object_id_t shared_table = m_mmg->m_learnDisableAclTable;
        ASSERT_NE(shared_table, SAI_NULL_OBJECT_ID);

        // Trip threshold.
        injectMove(MAC_A, ETH0, ETH1);
        injectMove(MAC_A, ETH1, ETH0);
        ASSERT_TRUE(state(MAC_A).is_bad_mac);
        EXPECT_NE(state(MAC_A).learn_disable_acl_entry_id, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(g_acl_entry_create_count, 1);
        EXPECT_EQ(m_mmg->m_learnDisableAclEntryCount, 1u);

        // Force expiry: entry should be removed but the shared table stays.
        state(MAC_A).action_expiry_time = steady_clock::now() - seconds(1);
        m_mmg->checkRecovery();

        EXPECT_FALSE(state(MAC_A).is_bad_mac);
        EXPECT_EQ(g_acl_entry_remove_count, 1);
        EXPECT_EQ(m_mmg->m_learnDisableAclEntryCount, 0u);
        EXPECT_EQ(m_mmg->m_learnDisableAclTable, shared_table);
        EXPECT_EQ(g_acl_table_remove_count, 0);
    }

    // -------- 11.1 #9: action transition tears down resources --------------
    TEST_F(MacMoveGuardOrchTest, ActionTransitionTearsDownPreviousResources)
    {
        buildOrch();
        g_set_do_not_learn_supported = true;

        // Start with DLOMWA, install an entry.
        configure({
            {"enabled","true"}, {"threshold","2"}, {"detect_interval","60"},
            {"action_interval","60"}, {"action","DISABLE_LEARN_ON_MAC_WITH_ACL"}
        });
        ASSERT_NE(m_mmg->m_learnDisableAclTable, SAI_NULL_OBJECT_ID);
        injectMove(MAC_A, ETH0, ETH1);
        injectMove(MAC_A, ETH1, ETH0);
        ASSERT_TRUE(tracked(MAC_A));
        ASSERT_EQ(m_mmg->m_learnDisableAclEntryCount, 1u);

        // Switch to DISABLE_PORT: previous bad-MAC entry should be removed,
        // shared ACL table destroyed and unbound.
        configure({ {"action","DISABLE_PORT"} });

        EXPECT_EQ(m_mmg->m_learnDisableAclTable, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(m_mmg->m_learnDisableAclEntryCount, 0u);
        EXPECT_EQ(g_acl_entry_remove_count, 1);
        EXPECT_EQ(g_acl_table_remove_count, 1);
        EXPECT_EQ(g_pre_ingress_acl_bound, SAI_NULL_OBJECT_ID);
        EXPECT_FALSE(tracked(MAC_A));   // erased by reconcile

        // Switching back creates a fresh table.
        int prior_creates = g_acl_table_create_count;
        configure({ {"action","DISABLE_LEARN_ON_MAC_WITH_ACL"} });
        EXPECT_EQ(g_acl_table_create_count, prior_creates + 1);
        EXPECT_NE(m_mmg->m_learnDisableAclTable, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(g_pre_ingress_acl_bound, m_mmg->m_learnDisableAclTable);
    }

    // -------- 11.1 #10: feature disable cleans everything ------------------
    TEST_F(MacMoveGuardOrchTest, FeatureDisableCleanupReleasesAllResources)
    {
        buildOrch();
        g_set_do_not_learn_supported = true;

        configure({
            {"enabled","true"}, {"threshold","2"}, {"detect_interval","60"},
            {"action_interval","60"}, {"action","DISABLE_LEARN_ON_MAC_WITH_ACL"}
        });
        ASSERT_NE(m_mmg->m_learnDisableAclTable, SAI_NULL_OBJECT_ID);
        injectMove(MAC_A, ETH0, ETH1);
        injectMove(MAC_A, ETH1, ETH0);

        ASSERT_TRUE(tracked(MAC_A));
        ASSERT_GE(m_mmg->m_learnDisableAclEntryCount, 1u);

        // Pretend we also had DISABLE_PORT machinery in-flight by manually
        // seeding a disabled-port refcount. clearAllState() should also touch it.
        m_mmg->m_disabledPorts[ETH2].insert(MacKey{MacAddress(MAC_A), VLAN40_OID});
        g_port_admin_status[ETH2_OID] = false;

        configure({ {"enabled","false"} });

        EXPECT_TRUE(m_mmg->m_macTrackingState.empty());
        EXPECT_TRUE(m_mmg->m_disabledPorts.empty());
        EXPECT_TRUE(m_mmg->m_learntMac.empty());
        EXPECT_EQ(m_mmg->m_learnDisableAclTable, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(g_pre_ingress_acl_bound, SAI_NULL_OBJECT_ID);
        EXPECT_TRUE(g_port_admin_status[ETH2_OID]);   // re-enabled
        vector<string> keys;
        m_mmg->m_stateBadMacTable->getKeys(keys);
        EXPECT_TRUE(keys.empty());
    }

    // -------- 11.1 #11: GC of quiet MACs ------------------------------------
    TEST_F(MacMoveGuardOrchTest, GcQuietNonBadMac)
    {
        buildOrch();
        configure({
            {"enabled","true"}, {"threshold","100"}, {"detect_interval","60"},
            {"action_interval","60"}, {"action","DISABLE_PORT"}
        });

        // Single move; well below threshold, so MAC stays in TRACKED state.
        injectMove(MAC_A, ETH0, ETH1);
        ASSERT_TRUE(tracked(MAC_A));
        EXPECT_FALSE(state(MAC_A).is_bad_mac);

        // Age the tracked entry past the detection window by rewriting its
        // ports_seen timestamps (cheaper than sleeping detect_interval).
        for (auto &p : state(MAC_A).ports_seen) p.second = steady_clock::now() - seconds(120);
        for (auto &t : state(MAC_A).move_timestamps) t = steady_clock::now() - seconds(120);

        m_mmg->checkRecovery();
        EXPECT_FALSE(tracked(MAC_A));   // GC'd
    }

    // -------- 11.1 #12: config rejection -----------------------------------
    TEST_F(MacMoveGuardOrchTest, ConfigRejectionHandling)
    {
        buildOrch();

        // Non-GLOBAL key is dropped, no state change.
        configureWithKey("BOGUS", { {"enabled","true"} });
        EXPECT_FALSE(m_mmg->m_enabled);

        // Bad action falls back to DISABLE_PORT with a warning.
        configure({
            {"enabled","true"}, {"threshold","5"}, {"detect_interval","60"},
            {"action_interval","60"}, {"action","INVALID_ACTION_NAME"}
        });
        EXPECT_TRUE(m_mmg->m_enabled);
        EXPECT_EQ(m_mmg->m_action, MacMoveGuardAction::DISABLE_PORT);

        // Invalid integer value for threshold is logged at ERROR and skipped;
        // other fields in the same set should still apply (detect_interval).
        uint32_t prev_threshold = m_mmg->m_threshold;
        configure({
            {"threshold","not_a_number"}, {"detect_interval","7"}
        });
        EXPECT_EQ(m_mmg->m_threshold, prev_threshold);
        EXPECT_EQ(m_mmg->m_durationSeconds, 7u);
    }

    // 11.1 #13 (YANG validation) is exercised by the sonic-yang-models test
    // suite, not by this orch's unit tests. Skipped here on purpose.

    // -------- 11.1 #14: restart restore — DISABLE_PORT ---------------------
    TEST_F(MacMoveGuardOrchTest, RestartRestoreDisablePort)
    {
        // Pre-populate STATE_DB before the orch is constructed; expiry far
        // in the future so checkRecovery() should NOT revert on the first tick.
        auto epoch_future = duration_cast<seconds>(
            system_clock::now().time_since_epoch()).count() + 600;

        Table state_t(m_state_db.get(), STATE_MAC_MOVE_GUARD_TABLE_NAME);
        ostringstream key;
        key << "0x" << hex << VLAN40_OID << ":" << MAC_A;
        state_t.set(key.str(), {
            {"action",              "DISABLE_PORT"},
            {"action_expiry_epoch", to_string(epoch_future)},
            {"pinned_port",         ETH0},
            {"disabled_ports",      ETH1 + "," + ETH2},
        });

        buildOrch();

        // In-memory state rebuilt from STATE_DB.
        ASSERT_TRUE(tracked(MAC_A));
        EXPECT_TRUE(state(MAC_A).is_bad_mac);
        EXPECT_EQ(state(MAC_A).pinned_port, ETH0);
        EXPECT_EQ(state(MAC_A).disabled_ports.count(ETH1), 1u);
        EXPECT_EQ(state(MAC_A).disabled_ports.count(ETH2), 1u);
        EXPECT_EQ(m_mmg->m_disabledPorts[ETH1].size(), 1u);
        EXPECT_EQ(m_mmg->m_disabledPorts[ETH2].size(), 1u);

        // Enable feature so the recovery timer is allowed to do work.
        configure({
            {"enabled","true"}, {"threshold","100"}, {"detect_interval","60"},
            {"action_interval","60"}, {"action","DISABLE_PORT"}
        });

        // First tick should NOT revert (deadline far in the future).
        m_mmg->checkRecovery();
        EXPECT_TRUE(state(MAC_A).is_bad_mac);

        // Force expiry: re-enables ports, drops the STATE_DB row.
        state(MAC_A).action_expiry_time = steady_clock::now() - seconds(1);
        m_mmg->checkRecovery();
        EXPECT_FALSE(state(MAC_A).is_bad_mac);
        EXPECT_TRUE(g_port_admin_status[ETH1_OID]);
        EXPECT_TRUE(g_port_admin_status[ETH2_OID]);

        vector<string> keys;
        m_mmg->m_stateBadMacTable->getKeys(keys);
        EXPECT_TRUE(keys.empty());
    }

    // -------- 11.1 #15: restart restore — DLOMWA multiple entries ----------
    TEST_F(MacMoveGuardOrchTest, RestartRestoreDlomwaSharedTable)
    {
        auto epoch_future = duration_cast<seconds>(
            system_clock::now().time_since_epoch()).count() + 600;
        sai_object_id_t shared_tbl   = 0xa1000000ULL;
        sai_object_id_t entry_oid_a  = 0xb1000001ULL;
        sai_object_id_t entry_oid_b  = 0xb1000002ULL;

        Table state_t(m_state_db.get(), STATE_MAC_MOVE_GUARD_TABLE_NAME);
        ostringstream tbl_hex; tbl_hex << "0x" << hex << shared_tbl;
        ostringstream ea_hex;  ea_hex  << "0x" << hex << entry_oid_a;
        ostringstream eb_hex;  eb_hex  << "0x" << hex << entry_oid_b;
        ostringstream ka, kb;
        ka << "0x" << hex << VLAN40_OID << ":" << MAC_A;
        kb << "0x" << hex << VLAN40_OID << ":" << MAC_B;
        state_t.set(ka.str(), {
            {"action","DISABLE_LEARN_ON_MAC_WITH_ACL"},
            {"action_expiry_epoch", to_string(epoch_future)},
            {"acl_entry_id", ea_hex.str()},
            {"acl_table_id", tbl_hex.str()},
        });
        state_t.set(kb.str(), {
            {"action","DISABLE_LEARN_ON_MAC_WITH_ACL"},
            {"action_expiry_epoch", to_string(epoch_future)},
            {"acl_entry_id", eb_hex.str()},
            {"acl_table_id", tbl_hex.str()},
        });

        buildOrch();

        ASSERT_TRUE(tracked(MAC_A));
        ASSERT_TRUE(tracked(MAC_B));
        EXPECT_EQ(m_mmg->m_learnDisableAclTable, shared_tbl);
        EXPECT_EQ(m_mmg->m_learnDisableAclEntryCount, 2u);
        EXPECT_EQ(state(MAC_A).learn_disable_acl_entry_id, entry_oid_a);
        EXPECT_EQ(state(MAC_B).learn_disable_acl_entry_id, entry_oid_b);

        // Drive recovery by forcing expiry: each ACL entry should be removed
        // via sai_acl_api->remove_acl_entry.
        configure({
            {"enabled","true"}, {"threshold","100"}, {"detect_interval","60"},
            {"action_interval","60"}, {"action","DISABLE_LEARN_ON_MAC_WITH_ACL"}
        });
        state(MAC_A).action_expiry_time = steady_clock::now() - seconds(1);
        state(MAC_B).action_expiry_time = steady_clock::now() - seconds(1);
        m_mmg->checkRecovery();

        EXPECT_FALSE(state(MAC_A).is_bad_mac);
        EXPECT_FALSE(state(MAC_B).is_bad_mac);
        EXPECT_EQ(g_acl_entry_remove_count, 2);
        EXPECT_EQ(m_mmg->m_learnDisableAclEntryCount, 0u);
    }

    // -------- 11.1 #16: restart restore — expired action ------------------
    TEST_F(MacMoveGuardOrchTest, RestartRestoreExpiredActionRevertsImmediately)
    {
        // Expiry already in the past: remaining clamps to 0, next checkRecovery
        // should revert immediately.
        auto epoch_past = duration_cast<seconds>(
            system_clock::now().time_since_epoch()).count() - 30;

        Table state_t(m_state_db.get(), STATE_MAC_MOVE_GUARD_TABLE_NAME);
        ostringstream key;
        key << "0x" << hex << VLAN40_OID << ":" << MAC_A;
        state_t.set(key.str(), {
            {"action","DISABLE_PORT"},
            {"action_expiry_epoch", to_string(epoch_past)},
            {"pinned_port",   ETH0},
            {"disabled_ports", ETH1},
        });

        buildOrch();
        configure({
            {"enabled","true"}, {"threshold","100"}, {"detect_interval","60"},
            {"action_interval","60"}, {"action","DISABLE_PORT"}
        });

        // First tick reverts.
        m_mmg->checkRecovery();
        EXPECT_FALSE(state(MAC_A).is_bad_mac);
        EXPECT_TRUE(g_port_admin_status[ETH1_OID]);

        vector<string> keys;
        m_mmg->m_stateBadMacTable->getKeys(keys);
        EXPECT_TRUE(keys.empty());
    }

    // -------- 11.1 #17: restart restore — feature now disabled ------------
    TEST_F(MacMoveGuardOrchTest, RestartRestoreWithFeatureDisabledClearsAll)
    {
        auto epoch_future = duration_cast<seconds>(
            system_clock::now().time_since_epoch()).count() + 600;
        Table state_t(m_state_db.get(), STATE_MAC_MOVE_GUARD_TABLE_NAME);
        ostringstream key;
        key << "0x" << hex << VLAN40_OID << ":" << MAC_A;
        state_t.set(key.str(), {
            {"action","DISABLE_PORT"},
            {"action_expiry_epoch", to_string(epoch_future)},
            {"pinned_port",   ETH0},
            {"disabled_ports", ETH1},
        });

        buildOrch();
        ASSERT_TRUE(tracked(MAC_A));
        ASSERT_EQ(m_mmg->m_disabledPorts[ETH1].size(), 1u);

        // First doTask sees enabled=false and triggers clearAllState() which
        // re-enables the ports and drops the STATE_DB row.
        configure({ {"enabled","false"} });

        EXPECT_FALSE(tracked(MAC_A));
        EXPECT_TRUE(m_mmg->m_disabledPorts.empty());
        EXPECT_TRUE(g_port_admin_status[ETH1_OID]);

        vector<string> keys;
        m_mmg->m_stateBadMacTable->getKeys(keys);
        EXPECT_TRUE(keys.empty());
    }

    // -------- 11.1 #18: restart restore — malformed STATE_DB key ----------
    TEST_F(MacMoveGuardOrchTest, RestartRestoreDropsMalformedRows)
    {
        Table state_t(m_state_db.get(), STATE_MAC_MOVE_GUARD_TABLE_NAME);

        // Key missing the ':' separator.
        state_t.set("no_colon_key", {
            {"action","DISABLE_PORT"}, {"action_expiry_epoch","0"}
        });
        // bv_id parses as hex but the rest is not a MAC.
        state_t.set("0xabc:not_a_mac", {
            {"action","DISABLE_PORT"}, {"action_expiry_epoch","0"}
        });
        // Valid key but missing the 'action' field.
        ostringstream good_key;
        good_key << "0x" << hex << VLAN40_OID << ":" << MAC_A;
        state_t.set(good_key.str(), {
            {"action_expiry_epoch","0"}
        });

        // Constructor must not throw.
        ASSERT_NO_THROW(buildOrch());

        // None of the malformed rows produced a tracked entry, and they were
        // all evicted from STATE_DB.
        EXPECT_FALSE(tracked(MAC_A));
        vector<string> keys;
        m_mmg->m_stateBadMacTable->getKeys(keys);
        EXPECT_TRUE(keys.empty());
    }
}
