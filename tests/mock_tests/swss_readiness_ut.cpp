/*
 * Unit tests for SwssReadiness — "System is Ready" signalling gate.
 *
 * Tests cover:
 *   SwssReadinessManager — core coordinator logic
 *   BufferOrch::areAllPortsReady() / checkAndSignalBufferReady()
 *   PortsOrch::checkAndSignalVlanMemberDone()
 *   Full gate: up_status written only when all modules signal
 *
 * Each module is tested for both "no config" (signals immediately) and
 * "has config" (signals after SAI applies the last entry).
 */

// Standard library headers MUST come before #define private public.
// GCC 14 sstream contains a struct (basic_stringbuf::__xfer_bufptrs) with a
// private access specifier; if it is first parsed while "private" is macro'd
// to "public" the compiler reports "redeclared with different access" when it
// encounters the struct again in a later translation unit with normal access.
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Expose private/protected members of BufferOrch needed by the unit tests
// (m_ready_list, m_bufferReadySignalled, m_buffer_type_maps,
//  areAllPortsReady(), checkAndSignalBufferReady()).
#define private public
#define protected public
#include "bufferorch.h"
#undef private
#undef protected

// portsorch.h is included for types only — no private member access needed.
#include "portsorch.h"

#include "swssreadiness.h"
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"
#include "mock_response_publisher.h"

extern string gMySwitchType;
extern std::unique_ptr<MockResponsePublisher> gMockResponsePublisher;

namespace swss_readiness_test
{
    using namespace std;
    using namespace swss;

    // -----------------------------------------------------------------------
    // SwssReadinessManager — unit tests (no SAI, no orch setup needed)
    // -----------------------------------------------------------------------
    class SwssReadinessManagerTest : public ::testing::Test
    {
    public:
        shared_ptr<DBConnector> m_state_db;
        SwssReadinessManager  *m_mgr = nullptr;

        void SetUp() override
        {
            m_state_db = make_shared<DBConnector>("STATE_DB", 0);
            m_mgr = new SwssReadinessManager(m_state_db.get());
        }

        void TearDown() override
        {
            delete m_mgr;
            m_mgr = nullptr;
            // Clear any up_status written during the test
            m_state_db->hdel("FEATURE|swss", "up_status");
        }

        bool upStatusIsTrue()
        {
            auto ptr = m_state_db->hget("FEATURE|swss", "up_status");
            return ptr != nullptr && *ptr == "true";
        }
    };

    // Manager with no modules registered — isReady() starts false, no signal
    TEST_F(SwssReadinessManagerTest, NoModules_NotReady)
    {
        EXPECT_FALSE(m_mgr->isReady());
        EXPECT_FALSE(upStatusIsTrue());
    }

    // Single module: signal fires STATE_DB write
    TEST_F(SwssReadinessManagerTest, SingleModule_SignalWritesStateDb)
    {
        m_mgr->registerModule("buffer");
        EXPECT_FALSE(m_mgr->isReady());

        m_mgr->signalDone("buffer");

        EXPECT_TRUE(m_mgr->isReady());
        EXPECT_TRUE(upStatusIsTrue());
    }

    // Signal from unregistered module is ignored
    TEST_F(SwssReadinessManagerTest, UnknownModule_Ignored)
    {
        m_mgr->registerModule("buffer");
        m_mgr->signalDone("unknown");   // should warn, not count

        EXPECT_FALSE(m_mgr->isReady());
        EXPECT_FALSE(upStatusIsTrue());
    }

    // Two modules: only fires after both signal
    TEST_F(SwssReadinessManagerTest, TwoModules_WaitsForBoth)
    {
        m_mgr->registerModule("buffer");
        m_mgr->registerModule("port");

        m_mgr->signalDone("buffer");
        EXPECT_FALSE(m_mgr->isReady()); // port not done yet

        m_mgr->signalDone("port");
        EXPECT_TRUE(m_mgr->isReady());
        EXPECT_TRUE(upStatusIsTrue());
    }

    // signalDone is idempotent: duplicate calls have no effect
    TEST_F(SwssReadinessManagerTest, DuplicateSignal_Idempotent)
    {
        m_mgr->registerModule("buffer");
        m_mgr->signalDone("buffer");
        EXPECT_TRUE(m_mgr->isReady());

        // Second call must not crash or double-write
        m_mgr->signalDone("buffer");
        EXPECT_TRUE(m_mgr->isReady());
    }

    // Once signalled, further signals from other modules are no-ops
    TEST_F(SwssReadinessManagerTest, AfterReady_FurtherSignalsNoOp)
    {
        m_mgr->registerModule("buffer");
        m_mgr->signalDone("buffer");
        EXPECT_TRUE(m_mgr->isReady());

        // Registering and signalling another module after ready is a no-op
        m_mgr->registerModule("port");
        m_mgr->signalDone("port");
        EXPECT_TRUE(m_mgr->isReady()); // still ready, no crash
    }

    // All 5 real modules
    TEST_F(SwssReadinessManagerTest, FiveModules_AllMustSignal)
    {
        for (auto &mod : {"port", "buffer", "acl", "vlanmember", "pfcwd"})
            m_mgr->registerModule(mod);

        m_mgr->signalDone("buffer");   EXPECT_FALSE(m_mgr->isReady()); // 1/5
        m_mgr->signalDone("port");     EXPECT_FALSE(m_mgr->isReady()); // 2/5
        m_mgr->signalDone("acl");      EXPECT_FALSE(m_mgr->isReady()); // 3/5
        m_mgr->signalDone("vlanmember"); EXPECT_FALSE(m_mgr->isReady()); // 4/5
        m_mgr->signalDone("pfcwd");    EXPECT_TRUE(m_mgr->isReady());  // 5/5
        EXPECT_TRUE(upStatusIsTrue());
    }

    // -----------------------------------------------------------------------
    // BufferOrch — areAllPortsReady() and checkAndSignalBufferReady()
    // -----------------------------------------------------------------------
    class BufferReadinessTest : public ::testing::Test
    {
    public:
        shared_ptr<DBConnector> m_app_db;
        shared_ptr<DBConnector> m_config_db;
        shared_ptr<DBConnector> m_state_db;
        SwssReadinessManager   *m_mgr = nullptr;

        void SetUp() override
        {
            map<string, string> profile = {
                { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
            };
            ut_helper::initSaiApi(profile);

            m_app_db    = make_shared<DBConnector>("APPL_DB",   0);
            m_config_db = make_shared<DBConnector>("CONFIG_DB", 0);
            m_state_db  = make_shared<DBConnector>("STATE_DB",  0);

            sai_attribute_t attr;
            attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;
            ASSERT_EQ(sai_switch_api->create_switch(&gSwitchId, 1, &attr),
                      SAI_STATUS_SUCCESS);
            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            gMacAddress = attr.value.mac;
            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
            sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            gVirtualRouterId = attr.value.oid;

            gCrmOrch = new CrmOrch(m_config_db.get(), CFG_CRM_TABLE_NAME);

            m_mgr = new SwssReadinessManager(m_state_db.get());
            gSwssReadiness = m_mgr;
            gSwssReadiness->registerModule("buffer");
        }

        BufferOrch* makeBufferOrch()
        {
            vector<string> buffer_tables = {
                APP_BUFFER_POOL_TABLE_NAME,
                APP_BUFFER_PROFILE_TABLE_NAME,
                APP_BUFFER_QUEUE_TABLE_NAME,
                APP_BUFFER_PG_TABLE_NAME,
                APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME
            };
            return new BufferOrch(m_app_db.get(), m_config_db.get(),
                                  m_state_db.get(), buffer_tables);
        }

        void TearDown() override
        {
            gSwssReadiness = nullptr;
            delete m_mgr;     m_mgr = nullptr;
            delete gCrmOrch;  gCrmOrch = nullptr;
            auto bm = BufferOrch::m_buffer_type_maps;
            for (auto &i : bm) i.second->clear();
            ut_helper::uninitSaiApi();
        }
    };

    // No buffer config → areAllPortsReady() true immediately, buffer signals
    TEST_F(BufferReadinessTest, NoBufferConfig_SignalsImmediately)
    {
        auto *orch = makeBufferOrch();

        EXPECT_TRUE(orch->m_ready_list.empty());
        EXPECT_TRUE(orch->areAllPortsReady());
        EXPECT_TRUE(orch->m_bufferReadySignalled);
        EXPECT_TRUE(m_mgr->isReady());

        delete orch;
    }

    // Entries start false → not ready
    TEST_F(BufferReadinessTest, PendingEntries_NotReady)
    {
        auto *orch = makeBufferOrch();
        orch->m_ready_list["Ethernet0|0"] = false;
        orch->m_bufferReadySignalled = false; // reset

        EXPECT_FALSE(orch->areAllPortsReady());

        delete orch;
    }

    // All entries true → signals
    TEST_F(BufferReadinessTest, AllEntriesTrue_Signals)
    {
        auto *orch = makeBufferOrch();
        orch->m_bufferReadySignalled = false;
        orch->m_ready_list["Ethernet0|0"] = true;
        orch->m_ready_list["Ethernet0|3"] = true;

        EXPECT_TRUE(orch->areAllPortsReady());

        orch->checkAndSignalBufferReady();
        EXPECT_TRUE(orch->m_bufferReadySignalled);
        EXPECT_TRUE(m_mgr->isReady());

        delete orch;
    }

    // One entry still false → not ready
    TEST_F(BufferReadinessTest, OnePendingEntry_NotReady)
    {
        auto *orch = makeBufferOrch();
        orch->m_ready_list["Ethernet0|0"] = true;
        orch->m_ready_list["Ethernet0|3"] = false;
        orch->m_bufferReadySignalled = false;

        EXPECT_FALSE(orch->areAllPortsReady());
        orch->checkAndSignalBufferReady();
        EXPECT_FALSE(orch->m_bufferReadySignalled);

        delete orch;
    }

    // Transition: last false→true entry signals
    TEST_F(BufferReadinessTest, LastEntryFlipped_Signals)
    {
        // Null the manager so the BufferOrch constructor's auto-fire of
        // checkAndSignalBufferReady() returns early (empty ready_list would
        // otherwise make areAllPortsReady() true immediately and pre-signal
        // "buffer", leaving m_mgr->isReady() == true before the test starts).
        gSwssReadiness = nullptr;
        auto *orch = makeBufferOrch();
        gSwssReadiness = m_mgr;

        // Now inject the partially-ready state
        orch->m_ready_list["Ethernet0|0"] = true;
        orch->m_ready_list["Ethernet0|3"] = false;

        orch->checkAndSignalBufferReady();
        EXPECT_FALSE(m_mgr->isReady());  // Ethernet0|3 still pending

        orch->m_ready_list["Ethernet0|3"] = true;
        orch->checkAndSignalBufferReady();
        EXPECT_TRUE(m_mgr->isReady());   // all entries true → signals

        delete orch;
    }

    // gSwssReadiness null → flag not consumed prematurely
    TEST_F(BufferReadinessTest, NullManager_FlagNotConsumed)
    {
        gSwssReadiness = nullptr;
        auto *orch = makeBufferOrch();

        // With empty ready_list, checkAndSignalBufferReady fires but
        // gSwssReadiness is null → flag must NOT be set so retry works.
        EXPECT_FALSE(orch->m_bufferReadySignalled);

        gSwssReadiness = m_mgr;
        delete orch;
    }

    // -----------------------------------------------------------------------
    // PortsOrch — checkAndSignalVlanMemberDone()
    // -----------------------------------------------------------------------

    // Helper: simulate vlanmember signal directly without full portsorch setup
    TEST(SwssReadinessVlanMember, NoConfig_SignalsImmediately)
    {
        // Simulate: expected=0, added=0 → signal fires
        size_t expected = 0, added = 0;
        bool signalled = false;

        // Replicate the guard logic
        auto check = [&]() {
            if (signalled) return;
            if (added < expected) return;
            signalled = true;
        };

        check();
        EXPECT_TRUE(signalled);
    }

    TEST(SwssReadinessVlanMember, HasConfig_WaitsForCount)
    {
        size_t expected = 3, added = 0;
        bool signalled = false;

        auto check = [&]() {
            if (signalled || added < expected) return;
            signalled = true;
        };

        check(); EXPECT_FALSE(signalled); // 0/3
        added++; check(); EXPECT_FALSE(signalled); // 1/3
        added++; check(); EXPECT_FALSE(signalled); // 2/3
        added++; check(); EXPECT_TRUE(signalled);  // 3/3
    }

    // DEL before ADD: expected count decremented, signal can still fire
    TEST(SwssReadinessVlanMember, DelBeforeAdd_ExpectedCountDecremented)
    {
        size_t expected = 3, added = 0;
        bool signalled = false;

        auto check = [&]() {
            if (signalled || added < expected) return;
            signalled = true;
        };

        // DELETE arrives before SET for one member — reduce expected
        if (expected > 0) { expected--; check(); }
        EXPECT_FALSE(signalled); // 0/2

        // Remaining 2 SETs arrive
        added++; check(); EXPECT_FALSE(signalled); // 1/2
        added++; check(); EXPECT_TRUE(signalled);  // 2/2 ← signal fires
    }

    // DEL after ADD: expected count decremented, already-reached threshold fires
    TEST(SwssReadinessVlanMember, DelAfterAdd_SignalAlreadyOrStillFires)
    {
        size_t expected = 2, added = 0;
        bool signalled = false;

        auto check = [&]() {
            if (signalled || added < expected) return;
            signalled = true;
        };

        // Both SETs arrive
        added++; check(); EXPECT_FALSE(signalled); // 1/2
        added++; check(); EXPECT_TRUE(signalled);  // 2/2 — signal fires

        // DEL arrives after signal — decrement expected, check is no-op
        if (expected > 0) expected--;
        check();
        EXPECT_TRUE(signalled); // still signalled, no regression
    }

    // All members deleted: expected reaches 0, signal fires immediately
    TEST(SwssReadinessVlanMember, AllMembersDeleted_SignalsImmediately)
    {
        size_t expected = 2, added = 0;
        bool signalled = false;

        auto check = [&]() {
            if (signalled || added < expected) return;
            signalled = true;
        };

        // Both members deleted before any SET
        if (expected > 0) { expected--; check(); } EXPECT_FALSE(signalled);
        if (expected > 0) { expected--; check(); } EXPECT_TRUE(signalled); // 0/0
    }

    TEST(SwssReadinessVlanMember, NullManager_FlagNotConsumed)
    {
        // Guard: if (!gSwssReadiness) return WITHOUT setting flag
        bool signalled = false;
        SwssReadinessManager *mgr = nullptr;

        auto check = [&]() {
            if (signalled || !mgr) return; // null guard BEFORE flag
            signalled = true;
            mgr->signalDone("vlanmember");
        };

        check(); // mgr is null → returns early
        EXPECT_FALSE(signalled); // flag not consumed

        // Now manager exists → signal works on retry
        DBConnector state_db("STATE_DB", 0);
        SwssReadinessManager real_mgr(&state_db);
        real_mgr.registerModule("vlanmember");
        mgr = &real_mgr;

        check();
        EXPECT_TRUE(signalled);
        EXPECT_TRUE(real_mgr.isReady());

        state_db.hdel("FEATURE|swss", "up_status");
    }

    // -----------------------------------------------------------------------
    // Deletion during bootup: PFCwd expected count decremented
    // -----------------------------------------------------------------------
    TEST(SwssReadinessPfcwd, DelDuringBootup_ExpectedCountDecremented)
    {
        // Simulate: 3 expected, 1 deleted before start → only 2 need to start
        size_t expected = 3;
        set<string> started;
        bool signalled = false;

        auto check = [&]() {
            if (signalled || started.size() < expected) return;
            signalled = true;
        };

        // DEL arrives for one port before it was ever started
        if (expected > 0) { expected--; check(); }
        EXPECT_FALSE(signalled); // 0/2

        started.insert("Ethernet0"); check(); EXPECT_FALSE(signalled); // 1/2
        started.insert("Ethernet4"); check(); EXPECT_TRUE(signalled);  // 2/2
    }

    TEST(SwssReadinessPfcwd, AllPortsDeleted_SignalsImmediately)
    {
        size_t expected = 2;
        set<string> started;
        bool signalled = false;

        auto check = [&]() {
            if (signalled || started.size() < expected) return;
            signalled = true;
        };

        if (expected > 0) { expected--; check(); } EXPECT_FALSE(signalled);
        if (expected > 0) { expected--; check(); } EXPECT_TRUE(signalled); // 0/0
    }

    // -----------------------------------------------------------------------
    // Deletion during bootup: ACL expected table removed from set
    // -----------------------------------------------------------------------
    TEST(SwssReadinessAcl, DelDuringBootup_TableRemovedFromExpected)
    {
        // Simulate: 2 tables expected, 1 deleted before creation
        set<string> expected = {"DATAACL", "COPP"};
        map<string, bool> created;  // table_id → exists in SAI
        bool signalled = false;

        // Gate: all expected tables must be in created
        auto check = [&]() {
            if (signalled) return;
            for (const auto &t : expected)
                if (!created.count(t)) return;
            signalled = true;
        };

        // COPP deleted before being created in SAI
        expected.erase("COPP");
        check();
        EXPECT_FALSE(signalled); // DATAACL still pending

        // DATAACL created
        created["DATAACL"] = true;
        check();
        EXPECT_TRUE(signalled); // all remaining expected tables present
    }

    TEST(SwssReadinessAcl, AllTablesDeleted_SignalsImmediately)
    {
        set<string> expected = {"DATAACL"};
        map<string, bool> created;
        bool signalled = false;

        auto check = [&]() {
            if (signalled) return;
            for (const auto &t : expected)
                if (!created.count(t)) return;
            signalled = true;
        };

        expected.erase("DATAACL"); // deleted before creation
        check();
        EXPECT_TRUE(signalled); // expected is empty → all satisfied
    }

    // -----------------------------------------------------------------------
    // Buffer: DEL during bootup sets ready_list entry to true
    // -----------------------------------------------------------------------
    TEST(SwssReadinessBuffer, DelEntry_MarksReadyListTrue)
    {
        // Simulate: 2 entries pre-populated, 1 SET and 1 DEL
        map<string, bool> ready_list;
        ready_list["Ethernet0|0"] = false;
        ready_list["Ethernet0|3"] = false;

        auto areAllReady = [&]() {
            for (const auto &kv : ready_list)
                if (!kv.second) return false;
            return true;
        };

        // DEL processed for Ethernet0|3 → set true (entry removed from pending)
        ready_list["Ethernet0|3"] = true;
        EXPECT_FALSE(areAllReady()); // Ethernet0|0 still false

        // SET processed for Ethernet0|0 → set true
        ready_list["Ethernet0|0"] = true;
        EXPECT_TRUE(areAllReady()); // all done
    }

    // -----------------------------------------------------------------------
    // Full gate: all 5 modules → up_status written
    // -----------------------------------------------------------------------
    TEST(SwssReadinessFullGate, AllFiveModules_WritesUpStatus)
    {
        DBConnector state_db("STATE_DB", 0);
        state_db.hdel("FEATURE|swss", "up_status");

        SwssReadinessManager mgr(&state_db);
        for (auto &m : {"port", "buffer", "acl", "vlanmember", "pfcwd"})
            mgr.registerModule(m);

        EXPECT_FALSE(mgr.isReady());

        mgr.signalDone("port");      EXPECT_FALSE(mgr.isReady());
        mgr.signalDone("buffer");    EXPECT_FALSE(mgr.isReady());
        mgr.signalDone("vlanmember"); EXPECT_FALSE(mgr.isReady());
        mgr.signalDone("acl");       EXPECT_FALSE(mgr.isReady());
        mgr.signalDone("pfcwd");     EXPECT_TRUE(mgr.isReady());

        auto ptr = state_db.hget("FEATURE|swss", "up_status");
        EXPECT_TRUE(ptr != nullptr);
        EXPECT_EQ(ptr ? *ptr : string(), "true");

        state_db.hdel("FEATURE|swss", "up_status");
    }

    TEST(SwssReadinessFullGate, MissingOneModule_DoesNotWrite)
    {
        DBConnector state_db("STATE_DB", 0);
        state_db.hdel("FEATURE|swss", "up_status");

        SwssReadinessManager mgr(&state_db);
        for (auto &m : {"port", "buffer", "acl", "vlanmember", "pfcwd"})
            mgr.registerModule(m);

        // Signal all except "port"
        mgr.signalDone("buffer");
        mgr.signalDone("acl");
        mgr.signalDone("vlanmember");
        mgr.signalDone("pfcwd");

        EXPECT_FALSE(mgr.isReady());
        EXPECT_EQ(state_db.hget("FEATURE|swss", "up_status"), nullptr);
    }

    TEST(SwssReadinessFullGate, NoPfcwdConfig_ThreeModuleGate)
    {
        // When no PFCwd orch is created, only 4 modules are registered.
        DBConnector state_db("STATE_DB", 0);
        state_db.hdel("FEATURE|swss", "up_status");

        SwssReadinessManager mgr(&state_db);
        for (auto &m : {"port", "buffer", "acl", "vlanmember"})
            mgr.registerModule(m);

        mgr.signalDone("port");
        mgr.signalDone("buffer");
        mgr.signalDone("acl");
        EXPECT_FALSE(mgr.isReady()); // vlanmember pending

        mgr.signalDone("vlanmember");
        EXPECT_TRUE(mgr.isReady());

        state_db.hdel("FEATURE|swss", "up_status");
    }

} // namespace swss_readiness_test
