/*
 * llrorch_ut.cpp — mock gtest unit tests for LlrOrch (orchagent LLR handler).
 *
 * These tests exercise the APPL_DB → SAI programming path inside LlrOrch
 * without running a full VS docker. They hook the relevant sai_port_api
 * function pointers to intercept and record SAI calls.
 */

/* Expose private/protected members so tests can read and override them.
 * Order matters: orch.h must be included under `protected→public` BEFORE
 * llrorch.h, because llrorch.h transitively includes orch.h and the include
 * guard would prevent a second inclusion.
 */
#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"          /* Orch::getExecutor becomes publicly accessible */
#undef protected
#define private public
#include "llrorch.h"       /* LlrOrch::capability, m_llrPortState become publicly accessible */
#undef private

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_table.h"

extern string gMySwitchType;
extern sai_object_id_t gSwitchId;
extern redisReply *mockReply;

namespace llrorch_test
{
    using namespace std;

    /* ------------------------------------------------------------------ */
    /* SAI stub state                                                       */
    /* ------------------------------------------------------------------ */

    int g_create_llr_profile_count;
    int g_remove_llr_profile_count;
    int g_set_llr_profile_attr_count;

    /* Each entry records one set_port_attribute call involving an LLR attr id. */
    struct SetPortAttrRecord
    {
        sai_object_id_t   port_id;
        sai_attr_id_t     attr_id;
        sai_attribute_value_t value;
    };
    vector<SetPortAttrRecord> g_set_port_attr_calls;

    /* Attributes captured from the last create_port_llr_profile call. */
    vector<sai_attribute_t> g_create_llr_profile_attrs;

    /* Fake OID counter for created LLR profiles. */
    static sai_object_id_t g_next_llr_profile_oid = 0x100;

    /* ------------------------------------------------------------------ */
    /* Saved real function pointers                                         */
    /* ------------------------------------------------------------------ */

    sai_port_api_t  ut_sai_port_api;
    sai_port_api_t *pold_sai_port_api;

    /* ------------------------------------------------------------------ */
    /* SAI stub implementations                                             */
    /* ------------------------------------------------------------------ */

    sai_status_t _ut_stub_sai_create_port_llr_profile(
        _Out_ sai_object_id_t       *llr_profile_id,
        _In_  sai_object_id_t        switch_id,
        _In_  uint32_t               attr_count,
        _In_  const sai_attribute_t *attr_list)
    {
        *llr_profile_id = g_next_llr_profile_oid++;
        g_create_llr_profile_count++;

        g_create_llr_profile_attrs.clear();
        for (uint32_t i = 0; i < attr_count; ++i)
        {
            g_create_llr_profile_attrs.push_back(attr_list[i]);
        }
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_remove_port_llr_profile(
        _In_ sai_object_id_t llr_profile_id)
    {
        g_remove_llr_profile_count++;
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t _ut_stub_sai_set_port_llr_profile_attribute(
        _In_ sai_object_id_t        llr_profile_id,
        _In_ const sai_attribute_t *attr)
    {
        g_set_llr_profile_attr_count++;
        return SAI_STATUS_SUCCESS;
    }

    /*
     * Intercept only LLR-related port attributes; forward everything else
     * to the real implementation so PortsOrch init is unaffected.
     */
    sai_status_t _ut_stub_sai_set_port_attribute(
        _In_ sai_object_id_t        port_id,
        _In_ const sai_attribute_t *attr)
    {
        if (attr->id == SAI_PORT_ATTR_LLR_MODE_LOCAL  ||
            attr->id == SAI_PORT_ATTR_LLR_MODE_REMOTE ||
            attr->id == SAI_PORT_ATTR_LLR_PROFILE)
        {
            g_set_port_attr_calls.push_back({port_id, attr->id, attr->value});
            return SAI_STATUS_SUCCESS;
        }
        return pold_sai_port_api->set_port_attribute(port_id, attr);
    }

    /* ------------------------------------------------------------------ */
    /* Hook / unhook helpers                                                */
    /* ------------------------------------------------------------------ */

    void _hook_sai_port_api()
    {
        ut_sai_port_api = *sai_port_api;
        pold_sai_port_api = sai_port_api;

        ut_sai_port_api.create_port_llr_profile          = _ut_stub_sai_create_port_llr_profile;
        ut_sai_port_api.remove_port_llr_profile          = _ut_stub_sai_remove_port_llr_profile;
        ut_sai_port_api.set_port_llr_profile_attribute   = _ut_stub_sai_set_port_llr_profile_attribute;
        ut_sai_port_api.set_port_attribute               = _ut_stub_sai_set_port_attribute;

        sai_port_api = &ut_sai_port_api;
    }

    void _unhook_sai_port_api()
    {
        sai_port_api = pold_sai_port_api;
    }

    void _reset_stubs()
    {
        g_create_llr_profile_count     = 0;
        g_remove_llr_profile_count     = 0;
        g_set_llr_profile_attr_count   = 0;
        g_set_port_attr_calls.clear();
        g_create_llr_profile_attrs.clear();
        g_next_llr_profile_oid         = 0x100;

        /* Clear static LLR object reference maps between tests. */
        for (auto &kv : LlrOrch::m_llr_type_maps)
        {
            kv.second->clear();
        }
    }

    /* ================================================================== */
    /* MockLlrOrch — wrapper to drive consumer events into LlrOrch         */
    /* ================================================================== */

    class MockLlrOrch final
    {
    public:
        MockLlrOrch()
        {
            this->appDb   = make_shared<DBConnector>("APPL_DB",  0);
            this->stateDb = make_shared<DBConnector>("STATE_DB", 0);

            const vector<string> llr_tables = {
                APP_LLR_PROFILE_TABLE_NAME,
                APP_LLR_PORT_TABLE_NAME
            };

            this->llrOrch = make_shared<LlrOrch>(
                this->appDb.get(), this->stateDb.get(), llr_tables);

            /*
             * Force LLR capability on so that doTask() processes APPL_DB events.
             * vslib does not implement sai_query_attribute_capability for LLR, so
             * all capability fields default to false after construction.
             * Populate all profile attrs as supported in both create and set paths.
             */
            this->llrOrch->capability.llr_supported  = true;
            this->llrOrch->capability.supported_profile_attrs = {
                SAI_PORT_LLR_PROFILE_ATTR_OUTSTANDING_FRAMES_MAX,
                SAI_PORT_LLR_PROFILE_ATTR_OUTSTANDING_BYTES_MAX,
                SAI_PORT_LLR_PROFILE_ATTR_REPLAY_TIMER_MAX,
                SAI_PORT_LLR_PROFILE_ATTR_REPLAY_COUNT_MAX,
                SAI_PORT_LLR_PROFILE_ATTR_PCS_LOST_TIMEOUT,
                SAI_PORT_LLR_PROFILE_ATTR_DATA_AGE_TIMEOUT,
                SAI_PORT_LLR_PROFILE_ATTR_INIT_LLR_FRAME_ACTION,
                SAI_PORT_LLR_PROFILE_ATTR_FLUSH_LLR_FRAME_ACTION,
                SAI_PORT_LLR_PROFILE_ATTR_RE_INIT_ON_FLUSH,
                SAI_PORT_LLR_PROFILE_ATTR_CTLOS_TARGET_SPACING,
            };
        }

        ~MockLlrOrch() = default;

        void doProfileTask(const deque<KeyOpFieldsValuesTuple> &entries)
        {
            auto *consumer = dynamic_cast<Consumer *>(
                llrOrch->getExecutor(APP_LLR_PROFILE_TABLE_NAME));
            consumer->addToSync(entries);
            static_cast<Orch *>(llrOrch.get())->doTask(*consumer);
        }

        void doPortTask(const deque<KeyOpFieldsValuesTuple> &entries)
        {
            auto *consumer = dynamic_cast<Consumer *>(
                llrOrch->getExecutor(APP_LLR_PORT_TABLE_NAME));
            consumer->addToSync(entries);
            static_cast<Orch *>(llrOrch.get())->doTask(*consumer);
        }

        /* Re-drain pending port consumer entries without adding new ones. */
        void retryPortTask()
        {
            auto *consumer = dynamic_cast<Consumer *>(
                llrOrch->getExecutor(APP_LLR_PORT_TABLE_NAME));
            static_cast<Orch *>(llrOrch.get())->doTask(*consumer);
        }

        /* Re-drain pending profile consumer entries without adding new ones. */
        void retryProfileTask()
        {
            auto *consumer = dynamic_cast<Consumer *>(
                llrOrch->getExecutor(APP_LLR_PROFILE_TABLE_NAME));
            static_cast<Orch *>(llrOrch.get())->doTask(*consumer);
        }

        shared_ptr<LlrOrch> llrOrch;

    private:
        shared_ptr<DBConnector> appDb;
        shared_ptr<DBConnector> stateDb;
    };

    /* ================================================================== */
    /* LlrOrchTest fixture                                                  */
    /* ================================================================== */

    class LlrOrchTest : public ::testing::Test
    {
    public:
        LlrOrchTest() { this->initDb(); }
        virtual ~LlrOrchTest() = default;

        void SetUp() override
        {
            this->initSaiApi();
            this->initSwitch();
            this->initOrch();
            this->initPorts();
            _hook_sai_port_api();
            _reset_stubs();
        }

        void TearDown() override
        {
            _reset_stubs();
            _unhook_sai_port_api();
            this->deinitOrch();
            this->deinitSwitch();
            this->deinitSaiApi();
        }

    private:
        void initSaiApi()
        {
            map<string, string> profileMap = {
                { "SAI_VS_SWITCH_TYPE",    "SAI_VS_SWITCH_TYPE_BCM56850" },
                { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00"          }
            };
            auto status = ut_helper::initSaiApi(profileMap);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
        }

        void deinitSaiApi()
        {
            auto status = ut_helper::uninitSaiApi();
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
        }

        void initSwitch()
        {
            sai_attribute_t attr;

            attr.id             = SAI_SWITCH_ATTR_INIT_SWITCH;
            attr.value.booldata = true;
            auto status = sai_switch_api->create_switch(&gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);

            attr.id = SAI_SWITCH_ATTR_SRC_MAC_ADDRESS;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gMacAddress = attr.value.mac;

            attr.id = SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID;
            status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gVirtualRouterId = attr.value.oid;
        }

        void deinitSwitch()
        {
            auto status = sai_switch_api->remove_switch(gSwitchId);
            ASSERT_EQ(status, SAI_STATUS_SUCCESS);
            gSwitchId        = SAI_NULL_OBJECT_ID;
            gVirtualRouterId = SAI_NULL_OBJECT_ID;
        }

        void initOrch()
        {
            /* SwitchOrch */
            TableConnector state_switch_table(stateDb.get(), "SWITCH_CAPABILITY");
            TableConnector app_switch_table(appDb.get(), APP_SWITCH_TABLE_NAME);
            TableConnector conf_asic_sensors(configDb.get(), CFG_ASIC_SENSORS_TABLE_NAME);

            vector<TableConnector> switchTableList = {
                conf_asic_sensors,
                app_switch_table
            };

            ASSERT_EQ(gSwitchOrch, nullptr);
            gSwitchOrch = new SwitchOrch(appDb.get(), switchTableList, state_switch_table);
            gDirectory.set(gSwitchOrch);
            resourcesList.push_back(gSwitchOrch);

            /* PortsOrch */
            const int portsorch_base_pri = 40;
            vector<table_name_with_pri_t> ports_tables = {
                { APP_PORT_TABLE_NAME,        portsorch_base_pri + 5 },
                { APP_VLAN_TABLE_NAME,        portsorch_base_pri + 2 },
                { APP_VLAN_MEMBER_TABLE_NAME, portsorch_base_pri     },
                { APP_LAG_TABLE_NAME,         portsorch_base_pri + 4 },
                { APP_LAG_MEMBER_TABLE_NAME,  portsorch_base_pri     }
            };

            ASSERT_EQ(gPortsOrch, nullptr);
            gPortsOrch = new PortsOrch(appDb.get(), stateDb.get(),
                                        ports_tables, chassisAppDb.get());
            gDirectory.set(gPortsOrch);
            resourcesList.push_back(gPortsOrch);

            /* VrfOrch */
            ASSERT_EQ(gVrfOrch, nullptr);
            gVrfOrch = new VRFOrch(appDb.get(), APP_VRF_TABLE_NAME,
                                    stateDb.get(), STATE_VRF_OBJECT_TABLE_NAME);
            resourcesList.push_back(gVrfOrch);

            /* BufferOrch */
            vector<string> bufferTableList = {
                APP_BUFFER_POOL_TABLE_NAME,
                APP_BUFFER_PROFILE_TABLE_NAME,
                APP_BUFFER_QUEUE_TABLE_NAME,
                APP_BUFFER_PG_TABLE_NAME,
                APP_BUFFER_PORT_INGRESS_PROFILE_LIST_NAME,
                APP_BUFFER_PORT_EGRESS_PROFILE_LIST_NAME
            };
            gBufferOrch = new BufferOrch(appDb.get(), configDb.get(),
                                          stateDb.get(), bufferTableList);
            gDirectory.set(gBufferOrch);
            resourcesList.push_back(gBufferOrch);

            /* FlexCounterOrch */
            vector<string> flexCounterTableList = { CFG_FLEX_COUNTER_TABLE_NAME };
            auto *flexCounterOrch = new FlexCounterOrch(configDb.get(), flexCounterTableList);
            gDirectory.set(flexCounterOrch);
            resourcesList.push_back(flexCounterOrch);

            /* CrmOrch */
            ASSERT_EQ(gCrmOrch, nullptr);
            gCrmOrch = new CrmOrch(configDb.get(), CFG_CRM_TABLE_NAME);
            gDirectory.set(gCrmOrch);
            resourcesList.push_back(gCrmOrch);
        }

        void deinitOrch()
        {
            reverse(resourcesList.begin(), resourcesList.end());
            for (auto &p : resourcesList) delete p;

            gSwitchOrch  = nullptr;
            gPortsOrch   = nullptr;
            gVrfOrch     = nullptr;
            gBufferOrch  = nullptr;
            gCrmOrch     = nullptr;

            Portal::DirectoryInternal::clear(gDirectory);
            EXPECT_TRUE(Portal::DirectoryInternal::empty(gDirectory));
        }

        void initPorts()
        {
            auto portTable = Table(appDb.get(), APP_PORT_TABLE_NAME);

            auto ports = ut_helper::getInitialSaiPorts();
            for (const auto &cit : ports)
            {
                portTable.set(cit.first, cit.second);
            }

            portTable.set("PortConfigDone",
                          { { "count", to_string(ports.size()) } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch *>(gPortsOrch)->doTask();

            portTable.set("PortInitDone", { { "lanes", "0" } });
            gPortsOrch->addExistingData(&portTable);
            static_cast<Orch *>(gPortsOrch)->doTask();
        }

        void initDb()
        {
            appDb        = make_shared<DBConnector>("APPL_DB",        0);
            configDb     = make_shared<DBConnector>("CONFIG_DB",      0);
            stateDb      = make_shared<DBConnector>("STATE_DB",       0);
            countersDb   = make_shared<DBConnector>("COUNTERS_DB",    0);
            chassisAppDb = make_shared<DBConnector>("CHASSIS_APP_DB", 0);
            asicDb       = make_shared<DBConnector>("ASIC_DB",        0);
        }

        shared_ptr<DBConnector> appDb;
        shared_ptr<DBConnector> configDb;
        shared_ptr<DBConnector> stateDb;
        shared_ptr<DBConnector> countersDb;
        shared_ptr<DBConnector> chassisAppDb;
        shared_ptr<DBConnector> asicDb;

        vector<Orch *> resourcesList;
    };

    /* ================================================================== */
    /* Helper: build a single-entry deque                                  */
    /* ================================================================== */

    static deque<KeyOpFieldsValuesTuple> makeEntries(
        const string &key,
        const string &op,
        const vector<pair<string, string>> &fvs)
    {
        deque<KeyOpFieldsValuesTuple> d;
        d.push_back({ key, op, fvs });
        return d;
    }

    /* Profile attrs sufficient to trigger a create_port_llr_profile call */
    static vector<pair<string, string>> basicProfileFvs()
    {
        return {
            { "max_outstanding_frames", "128"  },
            { "max_outstanding_bytes",  "65536" },
            { "max_replay_count",       "3"    },
            { "max_replay_timer",       "1000" },
            { "init_action",            "best_effort" },
            { "flush_action",           "discard"     }
        };
    }

    /* ================================================================== */
    /* End-to-end: deferred port enable, profile lifecycle                  */
    /*   1. Port enable w/ uncreated profile → no SAI calls (deferred)      */
    /*   2. Profile create → profile SAI + pending port drains (bind+enable) */
    /*   3. Profile DEL while bound → blocked (pendingRemove)              */
    /*   4. Port DEL → unbinds, releases ref                               */
    /*   5. Re-drain profile consumer → deferred profile removed           */
    /* ================================================================== */

    TEST_F(LlrOrchTest, EndToEndDeferredEnablementAndProfileLifecycle)
    {
        MockLlrOrch mock;

        /* ---- Step 1: Enable LLR on Ethernet0 before profile exists ---- */
        mock.doPortTask(
            makeEntries("Ethernet0", SET_COMMAND,
                {
                    { "llr_local",   "enabled"  },
                    { "llr_remote",  "enabled"  },
                    { "llr_profile", "llr_800000_40m_profile" }
                }));

        /* No SAI calls yet — profile is unresolved, port event stays queued. */
        EXPECT_EQ(g_create_llr_profile_count, 0);
        EXPECT_EQ(g_set_port_attr_calls.size(), 0u)
            << "No SAI calls expected when profile does not exist yet";

        /* Internal state: port must NOT appear in m_llrPortState (no bound profile). */
        EXPECT_TRUE(mock.llrOrch->m_llrPortState.find("Ethernet0") == mock.llrOrch->m_llrPortState.end()
                    || mock.llrOrch->m_llrPortState["Ethernet0"].bound_profile.empty())
            << "Port must not have a bound profile before profile is created";

        /* ---- Step 2: Create the profile ---- */
        mock.doProfileTask(
            makeEntries("llr_800000_40m_profile", SET_COMMAND, basicProfileFvs()));

        EXPECT_EQ(g_create_llr_profile_count, 1)
            << "create_port_llr_profile expected after profile SET";

        /* RE_INIT_ON_FLUSH should be injected since it is in supported_profile_attrs. */
        bool reinitFound = false;
        for (const auto &attr : g_create_llr_profile_attrs)
        {
            if (attr.id == SAI_PORT_LLR_PROFILE_ATTR_RE_INIT_ON_FLUSH)
            {
                EXPECT_TRUE(attr.value.booldata);
                reinitFound = true;
            }
        }
        EXPECT_TRUE(reinitFound)
            << "RE_INIT_ON_FLUSH must be injected on profile create";

        /* Now re-drain the port consumer — the queued port enable should resolve. */
        g_set_port_attr_calls.clear();
        mock.retryPortTask();

        /* Expected: bind profile + enable remote + enable local = 3 SAI calls. */
        ASSERT_EQ(g_set_port_attr_calls.size(), 3u)
            << "Deferred port enable must produce 3 set_port_attribute calls";
        EXPECT_EQ(g_set_port_attr_calls[0].attr_id, SAI_PORT_ATTR_LLR_PROFILE);
        EXPECT_NE(g_set_port_attr_calls[0].value.oid, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(g_set_port_attr_calls[1].attr_id, SAI_PORT_ATTR_LLR_MODE_REMOTE);
        EXPECT_TRUE(g_set_port_attr_calls[1].value.booldata);
        EXPECT_EQ(g_set_port_attr_calls[2].attr_id, SAI_PORT_ATTR_LLR_MODE_LOCAL);
        EXPECT_TRUE(g_set_port_attr_calls[2].value.booldata);

        /* Internal state: port must be fully enabled with profile bound. */
        auto &ps = mock.llrOrch->m_llrPortState["Ethernet0"];
        EXPECT_EQ(ps.bound_profile, "llr_800000_40m_profile");
        EXPECT_TRUE(ps.local_enabled);
        EXPECT_TRUE(ps.remote_enabled);

        /* ---- Step 3: Delete profile while port is still bound ---- */
        g_remove_llr_profile_count = 0;
        mock.doProfileTask(
            makeEntries("llr_800000_40m_profile", DEL_COMMAND, {}));

        EXPECT_EQ(g_remove_llr_profile_count, 0)
            << "Profile must NOT be removed while a port still references it";

        /* Verify pendingRemove flag is set. */
        auto profIt = LlrOrch::m_llr_type_maps[APP_LLR_PROFILE_TABLE_NAME]->find("llr_800000_40m_profile");
        ASSERT_NE(profIt, LlrOrch::m_llr_type_maps[APP_LLR_PROFILE_TABLE_NAME]->end());
        EXPECT_TRUE(profIt->second.m_pendingRemove)
            << "Profile should be marked pendingRemove";

        /* ---- Step 4: Delete the port → releases profile ref ---- */
        g_set_port_attr_calls.clear();
        mock.doPortTask(
            makeEntries("Ethernet0", DEL_COMMAND, {}));

        /* Expect 3 calls: remote=false, local=false, profile=NULL */
        ASSERT_EQ(g_set_port_attr_calls.size(), 3u);
        EXPECT_EQ(g_set_port_attr_calls[0].attr_id, SAI_PORT_ATTR_LLR_MODE_REMOTE);
        EXPECT_FALSE(g_set_port_attr_calls[0].value.booldata);
        EXPECT_EQ(g_set_port_attr_calls[1].attr_id, SAI_PORT_ATTR_LLR_MODE_LOCAL);
        EXPECT_FALSE(g_set_port_attr_calls[1].value.booldata);
        EXPECT_EQ(g_set_port_attr_calls[2].attr_id, SAI_PORT_ATTR_LLR_PROFILE);
        EXPECT_EQ(g_set_port_attr_calls[2].value.oid, SAI_NULL_OBJECT_ID);

        /* Internal state: port should be gone from tracking map. */
        EXPECT_EQ(mock.llrOrch->m_llrPortState.count("Ethernet0"), 0u)
            << "Port must be erased from m_llrPortState after DEL";

        /* ---- Step 5: Re-drain profile consumer → deferred remove fires ---- */
        mock.retryProfileTask();

        EXPECT_EQ(g_remove_llr_profile_count, 1)
            << "Deferred profile remove must fire after last port ref is gone";
    }

    /* ================================================================== */
    /* Profile reference count                                              */
    /*   1. Create profile, enable Ethernet0 and Ethernet4 on same profile  */
    /*   2. Delete Ethernet0 → profile must NOT be removed (Ethernet4 ref)  */
    /*   3. Profile DEL → blocked (Ethernet4 still references it)           */
    /*   4. Delete Ethernet4 → releases last reference                      */
    /*   5. Re-drain profile consumer → deferred profile removed            */
    /* ================================================================== */

    TEST_F(LlrOrchTest, ProfileRefcountDeletedOnLastPortRemoved)
    {
        MockLlrOrch llrOrch;

        /* Create profile and enable two ports on the same profile. */
        llrOrch.doProfileTask(
            makeEntries("llr_800000_40m_profile", SET_COMMAND, basicProfileFvs()));

        llrOrch.doPortTask(
            makeEntries("Ethernet0", SET_COMMAND,
                {
                    { "llr_local",   "enabled"               },
                    { "llr_remote",  "enabled"               },
                    { "llr_profile", "llr_800000_40m_profile" }
                }));

        llrOrch.doPortTask(
            makeEntries("Ethernet4", SET_COMMAND,
                {
                    { "llr_local",   "enabled"               },
                    { "llr_remote",  "enabled"               },
                    { "llr_profile", "llr_800000_40m_profile" }
                }));

        g_remove_llr_profile_count = 0;

        /* Delete the first port — profile must NOT be removed (Ethernet4 holds ref). */
        llrOrch.doPortTask(makeEntries("Ethernet0", DEL_COMMAND, {}));

        /* Profile DEL while Ethernet4 still references it — must be blocked. */
        llrOrch.doProfileTask(
            makeEntries("llr_800000_40m_profile", DEL_COMMAND, {}));
        EXPECT_EQ(g_remove_llr_profile_count, 0)
            << "Profile must NOT be removed while Ethernet4 still holds a reference";

        /* Delete the second port — releases the last reference. */
        llrOrch.doPortTask(makeEntries("Ethernet4", DEL_COMMAND, {}));

        /* Profile is no longer referenced; llrmgrd sends profile DEL to APPL_DB.
         * Re-drain the profile consumer to process the deferred DEL event. */
        llrOrch.retryProfileTask();

        EXPECT_EQ(g_remove_llr_profile_count, 1)
            << "Profile must be removed after last port ref is gone and profile DEL is processed";
    }

    /* ================================================================== */
    /* Profile switch while LLR is active                                   */
    /*   1. Create old and new profiles                                     */
    /*   2. Enable Ethernet0 on old profile                                 */
    /*   3. Switch Ethernet0 to new profile while still enabled             */
    /*   4. Verify SAI sequence: local=false → remote=false → bind new      */
    /*      → remote=true → local=true                                     */
    /* ================================================================== */

    TEST_F(LlrOrchTest, ProfileSwitchWhileActiveDisablesFirst)
    {
        MockLlrOrch llrOrch;

        /* Create both the old and new profiles. */
        llrOrch.doProfileTask(
            makeEntries("llr_800000_40m_profile", SET_COMMAND, basicProfileFvs()));
        llrOrch.doProfileTask(
            makeEntries("llr_400000_80m_profile", SET_COMMAND, basicProfileFvs()));
        ASSERT_EQ(g_create_llr_profile_count, 2);

        /* Enable LLR on Ethernet0 with the old profile. */
        llrOrch.doPortTask(
            makeEntries("Ethernet0", SET_COMMAND,
                {
                    { "llr_local",   "enabled"     },
                    { "llr_remote",  "enabled"     },
                    { "llr_profile", "llr_800000_40m_profile" }
                }));

        /* Reset — we only care about the profile-switch SAI calls. */
        g_set_port_attr_calls.clear();

        /* Switch to a new profile while still enabled (simulates cable-length change). */
        llrOrch.doPortTask(
            makeEntries("Ethernet0", SET_COMMAND,
                {
                    { "llr_local",   "enabled"               },
                    { "llr_remote",  "enabled"               },
                    { "llr_profile", "llr_400000_80m_profile" }
                }));

        /*
         * Expected SAI call sequence for profile switch while active:
         *   [0] SAI_PORT_ATTR_LLR_MODE_LOCAL   = false   (disable local first)
         *   [1] SAI_PORT_ATTR_LLR_MODE_REMOTE  = false   (disable remote)
         *   [2] SAI_PORT_ATTR_LLR_PROFILE      = <new OID>  (rebind)
         *   [3] SAI_PORT_ATTR_LLR_MODE_REMOTE  = true    (re-enable remote)
         *   [4] SAI_PORT_ATTR_LLR_MODE_LOCAL   = true    (re-enable local)
         */
        ASSERT_EQ(g_set_port_attr_calls.size(), 5u)
            << "Expected 5 set_port_attribute calls for profile switch while active";

        EXPECT_EQ(g_set_port_attr_calls[0].attr_id, SAI_PORT_ATTR_LLR_MODE_LOCAL)
            << "Step 1: local must be disabled before rebind";
        EXPECT_FALSE(g_set_port_attr_calls[0].value.booldata);

        EXPECT_EQ(g_set_port_attr_calls[1].attr_id, SAI_PORT_ATTR_LLR_MODE_REMOTE)
            << "Step 2: remote must be disabled before rebind";
        EXPECT_FALSE(g_set_port_attr_calls[1].value.booldata);

        EXPECT_EQ(g_set_port_attr_calls[2].attr_id, SAI_PORT_ATTR_LLR_PROFILE)
            << "Step 3: new profile must be bound after both modes are disabled";
        EXPECT_NE(g_set_port_attr_calls[2].value.oid, SAI_NULL_OBJECT_ID);

        EXPECT_EQ(g_set_port_attr_calls[3].attr_id, SAI_PORT_ATTR_LLR_MODE_REMOTE)
            << "Step 4: remote must be re-enabled after rebind";
        EXPECT_TRUE(g_set_port_attr_calls[3].value.booldata);

        EXPECT_EQ(g_set_port_attr_calls[4].attr_id, SAI_PORT_ATTR_LLR_MODE_LOCAL)
            << "Step 5: local must be re-enabled last";
        EXPECT_TRUE(g_set_port_attr_calls[4].value.booldata);
    }

    /* ================================================================== */
    /* Port bind to pending-removal profile is deferred                     */
    /*   1. Create profile, enable Ethernet0 on it                         */
    /*   2. Profile DEL → blocked (pendingRemove)                          */
    /*   3. Ethernet4 SET referencing same profile → deferred (no SAI)     */
    /*   4. Delete Ethernet0 → releases ref                                */
    /*   5. Re-drain profile consumer → profile removed                    */
    /*   6. Re-create profile                                              */
    /*   7. Re-drain port consumer → Ethernet4 binds and enables           */
    /* ================================================================== */

    TEST_F(LlrOrchTest, PortBindToPendingRemoveProfileDeferred)
    {
        MockLlrOrch mock;

        /* ---- Step 1: Create profile and enable Ethernet0 ---- */
        mock.doProfileTask(
            makeEntries("llr_800000_40m_profile", SET_COMMAND, basicProfileFvs()));
        ASSERT_EQ(g_create_llr_profile_count, 1);

        mock.doPortTask(
            makeEntries("Ethernet0", SET_COMMAND,
                {
                    { "llr_local",   "enabled"               },
                    { "llr_remote",  "enabled"               },
                    { "llr_profile", "llr_800000_40m_profile" }
                }));

        auto &ps0 = mock.llrOrch->m_llrPortState["Ethernet0"];
        ASSERT_EQ(ps0.bound_profile, "llr_800000_40m_profile");

        /* ---- Step 2: Profile DEL → blocked (pendingRemove) ---- */
        g_remove_llr_profile_count = 0;
        mock.doProfileTask(
            makeEntries("llr_800000_40m_profile", DEL_COMMAND, {}));
        EXPECT_EQ(g_remove_llr_profile_count, 0)
            << "Profile must not be removed while Ethernet0 still references it";

        auto profIt = LlrOrch::m_llr_type_maps[APP_LLR_PROFILE_TABLE_NAME]->find("llr_800000_40m_profile");
        ASSERT_NE(profIt, LlrOrch::m_llr_type_maps[APP_LLR_PROFILE_TABLE_NAME]->end());
        ASSERT_TRUE(profIt->second.m_pendingRemove);

        /* ---- Step 3: Ethernet4 SET references pending-removal profile → deferred ---- */
        g_set_port_attr_calls.clear();
        mock.doPortTask(
            makeEntries("Ethernet4", SET_COMMAND,
                {
                    { "llr_local",   "enabled"               },
                    { "llr_remote",  "enabled"               },
                    { "llr_profile", "llr_800000_40m_profile" }
                }));

        EXPECT_EQ(g_set_port_attr_calls.size(), 0u)
            << "No SAI calls expected — profile is pending removal, port bind must be deferred";
        EXPECT_TRUE(mock.llrOrch->m_llrPortState.find("Ethernet4") == mock.llrOrch->m_llrPortState.end()
                    || mock.llrOrch->m_llrPortState["Ethernet4"].bound_profile.empty())
            << "Ethernet4 must not have a bound profile while profile is pending removal";

        /* ---- Step 4: Delete Ethernet0 → frees last reference ---- */
        mock.doPortTask(makeEntries("Ethernet0", DEL_COMMAND, {}));
        EXPECT_EQ(mock.llrOrch->m_llrPortState.count("Ethernet0"), 0u);

        /* ---- Step 5: Re-drain profile consumer → deferred remove fires ---- */
        mock.retryProfileTask();
        EXPECT_EQ(g_remove_llr_profile_count, 1)
            << "Profile must be removed after last port ref is gone";

        /* ---- Step 6: Re-create the profile ---- */
        mock.doProfileTask(
            makeEntries("llr_800000_40m_profile", SET_COMMAND, basicProfileFvs()));
        EXPECT_EQ(g_create_llr_profile_count, 2)
            << "Profile must be re-created after removal";

        /* ---- Step 7: Re-drain port consumer → Ethernet4 binds ---- */
        g_set_port_attr_calls.clear();
        mock.retryPortTask();

        ASSERT_EQ(g_set_port_attr_calls.size(), 3u)
            << "Deferred Ethernet4 must produce 3 SAI calls (bind + enable remote + enable local)";
        EXPECT_EQ(g_set_port_attr_calls[0].attr_id, SAI_PORT_ATTR_LLR_PROFILE);
        EXPECT_NE(g_set_port_attr_calls[0].value.oid, SAI_NULL_OBJECT_ID);
        EXPECT_EQ(g_set_port_attr_calls[1].attr_id, SAI_PORT_ATTR_LLR_MODE_REMOTE);
        EXPECT_TRUE(g_set_port_attr_calls[1].value.booldata);
        EXPECT_EQ(g_set_port_attr_calls[2].attr_id, SAI_PORT_ATTR_LLR_MODE_LOCAL);
        EXPECT_TRUE(g_set_port_attr_calls[2].value.booldata);

        EXPECT_EQ(mock.llrOrch->m_llrPortState["Ethernet4"].bound_profile, "llr_800000_40m_profile");
    }

} /* namespace llrorch_test */
