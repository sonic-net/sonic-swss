#include <sstream>
#define private public
#define protected public
#include "udforch.h"
#undef protected
#undef private

#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "cfg_schema.h"

extern sai_udf_api_t *sai_udf_api;
extern sai_object_id_t gSwitchId;

namespace udforch_test
{

using namespace std;

/* --- SAI stubs ---------------------------------------------------------- */

static sai_udf_api_t  ut_sai_udf_api;
static sai_udf_api_t *pold_sai_udf_api;

static int g_create_group_calls  = 0;
static int g_remove_group_calls  = 0;
static int g_create_match_calls  = 0;
static int g_remove_match_calls  = 0;
static int g_create_udf_calls    = 0;
static int g_remove_udf_calls    = 0;
static sai_object_id_t g_next_oid = 0x1000;
static sai_status_t g_create_group_status = SAI_STATUS_SUCCESS;
static sai_status_t g_remove_group_status = SAI_STATUS_SUCCESS;
static sai_status_t g_create_match_status = SAI_STATUS_SUCCESS;
static sai_status_t g_remove_match_status = SAI_STATUS_SUCCESS;
static sai_status_t g_create_udf_status   = SAI_STATUS_SUCCESS;
static sai_status_t g_remove_udf_status   = SAI_STATUS_SUCCESS;

// Captured from last create_udf_group call
static sai_udf_group_type_t g_last_group_type = SAI_UDF_GROUP_TYPE_GENERIC;
// Set bits seen in last create_udf_match call (L2/L3/GRE/L4)
static bool g_last_match_has_l2   = false;
static bool g_last_match_has_l3   = false;
static bool g_last_match_has_gre  = false;
static bool g_last_match_has_l4   = false;

static void reset_counters()
{
    g_create_group_calls  = g_remove_group_calls = 0;
    g_create_match_calls  = g_remove_match_calls = 0;
    g_create_udf_calls    = g_remove_udf_calls   = 0;
    g_last_group_type     = SAI_UDF_GROUP_TYPE_GENERIC;
    g_last_match_has_l2   = g_last_match_has_l3 = g_last_match_has_gre = g_last_match_has_l4 = false;
    g_create_group_status = SAI_STATUS_SUCCESS;
    g_remove_group_status = SAI_STATUS_SUCCESS;
    g_create_match_status = SAI_STATUS_SUCCESS;
    g_remove_match_status = SAI_STATUS_SUCCESS;
    g_create_udf_status   = SAI_STATUS_SUCCESS;
    g_remove_udf_status   = SAI_STATUS_SUCCESS;
}

static sai_status_t stub_create_udf_group(sai_object_id_t *oid, sai_object_id_t,
                                           uint32_t attr_count, const sai_attribute_t *attrs)
{
    if (g_create_group_status != SAI_STATUS_SUCCESS)
        return g_create_group_status;
    *oid = ++g_next_oid;
    g_create_group_calls++;
    for (uint32_t i = 0; i < attr_count; i++)
        if (attrs[i].id == SAI_UDF_GROUP_ATTR_TYPE)
            g_last_group_type = static_cast<sai_udf_group_type_t>(attrs[i].value.s32);
    return SAI_STATUS_SUCCESS;
}
static sai_status_t stub_remove_udf_group(sai_object_id_t)
{
    if (g_remove_group_status != SAI_STATUS_SUCCESS)
        return g_remove_group_status;
    g_remove_group_calls++;
    return SAI_STATUS_SUCCESS;
}
static sai_status_t stub_create_udf_match(sai_object_id_t *oid, sai_object_id_t,
                                            uint32_t attr_count, const sai_attribute_t *attrs)
{
    if (g_create_match_status != SAI_STATUS_SUCCESS)
        return g_create_match_status;
    *oid = ++g_next_oid;
    g_create_match_calls++;
    g_last_match_has_l2 = g_last_match_has_l3 = g_last_match_has_gre = g_last_match_has_l4 = false;
    for (uint32_t i = 0; i < attr_count; i++)
    {
        if (attrs[i].id == SAI_UDF_MATCH_ATTR_L2_TYPE) g_last_match_has_l2 = true;
        if (attrs[i].id == SAI_UDF_MATCH_ATTR_L3_TYPE) g_last_match_has_l3 = true;
        if (attrs[i].id == SAI_UDF_MATCH_ATTR_GRE_TYPE) g_last_match_has_gre = true;
        if (attrs[i].id == SAI_UDF_MATCH_ATTR_L4_DST_PORT_TYPE) g_last_match_has_l4 = true;
    }
    return SAI_STATUS_SUCCESS;
}
static sai_status_t stub_remove_udf_match(sai_object_id_t)
{
    if (g_remove_match_status != SAI_STATUS_SUCCESS)
        return g_remove_match_status;
    g_remove_match_calls++;
    return SAI_STATUS_SUCCESS;
}
static sai_status_t stub_create_udf(sai_object_id_t *oid, sai_object_id_t, uint32_t, const sai_attribute_t *)
{
    if (g_create_udf_status != SAI_STATUS_SUCCESS)
        return g_create_udf_status;
    *oid = ++g_next_oid;
    g_create_udf_calls++;
    return SAI_STATUS_SUCCESS;
}
static sai_status_t stub_remove_udf(sai_object_id_t)
{
    if (g_remove_udf_status != SAI_STATUS_SUCCESS)
        return g_remove_udf_status;
    g_remove_udf_calls++;
    return SAI_STATUS_SUCCESS;
}

/* --- Fixture ------------------------------------------------------------ */

class UdfOrchTest : public ::testing::Test
{
protected:
    shared_ptr<swss::DBConnector> m_config_db;
    UdfOrch *m_orch = nullptr;

    void SetUp() override
    {
        map<string, string> profile = {
            { "SAI_VS_SWITCH_TYPE", "SAI_VS_SWITCH_TYPE_BCM56850" },
            { "KV_DEVICE_MAC_ADDRESS", "20:03:04:05:06:00" }
        };
        ut_helper::initSaiApi(profile);

        sai_attribute_t attr;
        attr.id = SAI_SWITCH_ATTR_INIT_SWITCH;
        attr.value.booldata = true;
        ASSERT_EQ(sai_switch_api->create_switch(&gSwitchId, 1, &attr), SAI_STATUS_SUCCESS);

        pold_sai_udf_api = sai_udf_api;
        ut_sai_udf_api = *sai_udf_api;
        ut_sai_udf_api.create_udf_group = stub_create_udf_group;
        ut_sai_udf_api.remove_udf_group = stub_remove_udf_group;
        ut_sai_udf_api.create_udf_match = stub_create_udf_match;
        ut_sai_udf_api.remove_udf_match = stub_remove_udf_match;
        ut_sai_udf_api.create_udf       = stub_create_udf;
        ut_sai_udf_api.remove_udf       = stub_remove_udf;
        sai_udf_api = &ut_sai_udf_api;

        m_config_db = make_shared<swss::DBConnector>("CONFIG_DB", 0);
        m_orch = new UdfOrch(m_config_db.get(),
                             { CFG_UDF_TABLE_NAME, CFG_UDF_SELECTOR_TABLE_NAME });
        reset_counters();
    }

    void TearDown() override
    {
        delete m_orch;
        m_orch = nullptr;
        sai_udf_api = pold_sai_udf_api;
        sai_switch_api->remove_switch(gSwitchId);
        gSwitchId = SAI_NULL_OBJECT_ID;
        ut_helper::uninitSaiApi();
    }

    void pushUdf(const string &key, const vector<FieldValueTuple> &fvs,
                 const string &op = "SET")
    {
        deque<KeyOpFieldsValuesTuple> q;
        q.push_back({ key, op, fvs });
        auto *c = dynamic_cast<Consumer *>(m_orch->getExecutor(CFG_UDF_TABLE_NAME));
        c->addToSync(q);
        static_cast<Orch *>(m_orch)->doTask();
    }

    void pushSelector(const string &key, const vector<FieldValueTuple> &fvs,
                      const string &op = "SET")
    {
        deque<KeyOpFieldsValuesTuple> q;
        q.push_back({ key, op, fvs });
        auto *c = dynamic_cast<Consumer *>(m_orch->getExecutor(CFG_UDF_SELECTOR_TABLE_NAME));
        c->addToSync(q);
        static_cast<Orch *>(m_orch)->doTask();
    }
};

/* --- Tests -------------------------------------------------------------- */

TEST_F(UdfOrchTest, CreateAndRemoveUdfGroup)
{
    pushUdf("udf1", {{ "length", "4" }, { "field_type", "GENERIC" }});

    ASSERT_EQ(m_orch->m_udfGroups.size(), 1u);
    ASSERT_EQ(g_create_group_calls, 1);
    ASSERT_NE(m_orch->m_udfGroups.at("udf1")->getOid(), SAI_NULL_OBJECT_ID);

    pushUdf("udf1", {}, "DEL");

    ASSERT_EQ(m_orch->m_udfGroups.size(), 0u);
    ASSERT_EQ(g_remove_group_calls, 1);
}

TEST_F(UdfOrchTest, MatchDedup)
{
    pushUdf("udf1", {{ "length", "4" }});

    // Two selectors with identical match criteria must share one UDF_MATCH OID.
    vector<FieldValueTuple> udp_l3 = {
        {"select_base", "L3"}, {"select_offset", "0"},
        {"match_l3_type", "0x11"}, {"match_l3_type_mask", "0xFF"}, {"match_priority", "10"}
    };
    pushSelector("udf1|sel_a", udp_l3);
    pushSelector("udf1|sel_b", udp_l3);

    ASSERT_EQ(g_create_match_calls, 1);
    ASSERT_EQ(m_orch->m_udfMatches.size(), 1u);
    ASSERT_EQ(m_orch->m_udfs.size(), 2u);

    // Selector with different match criteria must produce a distinct UDF_MATCH.
    vector<FieldValueTuple> tcp_l3 = {
        {"select_base", "L3"}, {"select_offset", "0"},
        {"match_l3_type", "0x06"}, {"match_l3_type_mask", "0xFF"}, {"match_priority", "20"}
    };
    pushSelector("udf1|sel_c", tcp_l3);

    ASSERT_EQ(g_create_match_calls, 2);
    ASSERT_EQ(m_orch->m_udfMatches.size(), 2u);
}

TEST_F(UdfOrchTest, DependencyRetryResolvesAfterUdfCreated)
{
    // Push selector before its parent UDF group exists.
    vector<FieldValueTuple> udp_l3 = {
        {"select_base", "L3"}, {"select_offset", "0"},
        {"match_l3_type", "0x11"}, {"match_l3_type_mask", "0xFF"}, {"match_priority", "10"}
    };
    pushSelector("udf1|sel_a", udp_l3);

    // Entry must not have been consumed (no UDF_MATCH or UDF created yet).
    ASSERT_EQ(g_create_match_calls, 0);
    ASSERT_EQ(g_create_udf_calls,   0);

    // Now create the parent UDF; the pending selector should resolve.
    pushUdf("udf1", {{ "length", "4" }});
    // doTask on the selector consumer to flush the retry.
    auto *c = dynamic_cast<Consumer *>(m_orch->getExecutor(CFG_UDF_SELECTOR_TABLE_NAME));
    deque<KeyOpFieldsValuesTuple> q;
    c->addToSync(q);
    static_cast<Orch *>(m_orch)->doTask();

    ASSERT_EQ(g_create_match_calls, 1);
    ASSERT_EQ(g_create_udf_calls,   1);
    ASSERT_EQ(m_orch->m_udfs.size(), 1u);
}

TEST_F(UdfOrchTest, RefcountGuardBlocksDelete)
{
    pushUdf("udf1", {{ "length", "4" }});
    ASSERT_EQ(m_orch->m_udfGroups.size(), 1u);

    // Simulate an ACL table holding a reference.
    m_orch->incrementGroupRefCount("udf1");

    pushUdf("udf1", {}, "DEL");

    // Group must still be present; SAI remove must not have been called.
    ASSERT_EQ(m_orch->m_udfGroups.size(), 1u);
    ASSERT_EQ(g_remove_group_calls, 0);

    // Release reference; delete should now succeed.
    m_orch->decrementGroupRefCount("udf1");
    pushUdf("udf1", {}, "DEL");
    ASSERT_EQ(m_orch->m_udfGroups.size(), 0u);
    ASSERT_EQ(g_remove_group_calls, 1);
}

TEST_F(UdfOrchTest, InvalidFieldTypeIsRejected)
{
    pushUdf("udf1", {{ "length", "4" }, { "field_type", "BOGUS" }});

    // Entry must be dropped; no SAI call, no internal state.
    ASSERT_EQ(g_create_group_calls, 0);
    ASSERT_EQ(m_orch->m_udfGroups.size(), 0u);

    // A valid entry submitted afterwards must succeed.
    pushUdf("udf1", {{ "length", "4" }, { "field_type", "GENERIC" }});
    ASSERT_EQ(g_create_group_calls, 1);
    ASSERT_EQ(m_orch->m_udfGroups.size(), 1u);
}

/* --- Basic sanity -------------------------------------------------------- */

TEST_F(UdfOrchTest, ResourceExhaustionRetries)
{
    // SAI returns INSUFFICIENT_RESOURCES — entry must stay in m_toSync for retry.
    g_create_group_status = SAI_STATUS_INSUFFICIENT_RESOURCES;
    pushUdf("udf1", {{ "length", "4" }});

    ASSERT_EQ(g_create_group_calls, 0);
    ASSERT_EQ(m_orch->m_udfGroups.size(), 0u);

    auto *c = dynamic_cast<Consumer *>(m_orch->getExecutor(CFG_UDF_TABLE_NAME));
    ASSERT_EQ(c->m_toSync.size(), 1u);

    // Resource becomes available — retry must now succeed.
    g_create_group_status = SAI_STATUS_SUCCESS;
    static_cast<Orch *>(m_orch)->doTask();

    ASSERT_EQ(g_create_group_calls, 1);
    ASSERT_EQ(m_orch->m_udfGroups.size(), 1u);
    ASSERT_EQ(c->m_toSync.size(), 0u);
}

TEST_F(UdfOrchTest, HashTypeUdfGroup)
{
    pushUdf("udf1", {{ "length", "8" }, { "field_type", "HASH" }});
    ASSERT_EQ(g_create_group_calls, 1);
    ASSERT_EQ(g_last_group_type, SAI_UDF_GROUP_TYPE_HASH);
}

TEST_F(UdfOrchTest, DuplicateSetIsIdempotent)
{
    pushUdf("udf1", {{ "length", "4" }});
    ASSERT_EQ(g_create_group_calls, 1);

    // Same config again — must not create a second SAI object.
    pushUdf("udf1", {{ "length", "4" }});
    ASSERT_EQ(g_create_group_calls, 1);
    ASSERT_EQ(m_orch->m_udfGroups.size(), 1u);
}

TEST_F(UdfOrchTest, ImmutableLengthReject)
{
    pushUdf("udf1", {{ "length", "4" }});
    ASSERT_EQ(g_create_group_calls, 1);

    // Attempt to change length — must be rejected; group must remain.
    pushUdf("udf1", {{ "length", "8" }});
    ASSERT_EQ(g_create_group_calls, 1);
    ASSERT_EQ(m_orch->m_udfGroups.at("udf1")->getConfig().length, 4);
}

TEST_F(UdfOrchTest, MissingLengthDropped)
{
    pushUdf("udf1", {{ "field_type", "GENERIC" }});  // no length field
    ASSERT_EQ(g_create_group_calls, 0);
    ASSERT_EQ(m_orch->m_udfGroups.size(), 0u);
}

TEST_F(UdfOrchTest, SelectorReplayDoesNotLeakRefcount)
{
    pushUdf("udf1", {{ "length", "4" }});

    vector<FieldValueTuple> udp_l3 = {
        {"select_base", "L3"}, {"select_offset", "0"},
        {"match_l3_type", "0x11"}, {"match_l3_type_mask", "0xFF"}, {"match_priority", "10"}
    };
    pushSelector("udf1|sel_a", udp_l3);
    ASSERT_EQ(g_create_match_calls, 1);
    ASSERT_EQ(m_orch->m_matchRefCount.size(), 1u);
    uint32_t refcount = m_orch->m_matchRefCount.begin()->second;
    ASSERT_EQ(refcount, 1u);

    // Replay same selector — must not increment refcount.
    pushSelector("udf1|sel_a", udp_l3);
    ASSERT_EQ(g_create_match_calls, 1);
    ASSERT_EQ(m_orch->m_matchRefCount.begin()->second, 1u);

    // Delete must now fully release the match (refcount → 0, SAI remove fires).
    pushSelector("udf1|sel_a", {}, "DEL");
    ASSERT_EQ(g_remove_match_calls, 1);
    ASSERT_EQ(m_orch->m_udfMatches.size(), 0u);
}

TEST_F(UdfOrchTest, SelectorRemoveReleasesMatch)
{
    pushUdf("udf1", {{ "length", "4" }});

    vector<FieldValueTuple> udp_l3 = {
        {"select_base", "L3"}, {"select_offset", "0"},
        {"match_l3_type", "0x11"}, {"match_l3_type_mask", "0xFF"}, {"match_priority", "10"}
    };
    pushSelector("udf1|sel_a", udp_l3);
    ASSERT_EQ(m_orch->m_udfs.size(), 1u);
    ASSERT_EQ(m_orch->m_udfMatches.size(), 1u);

    pushSelector("udf1|sel_a", {}, "DEL");
    ASSERT_EQ(g_remove_udf_calls, 1);
    ASSERT_EQ(m_orch->m_udfs.size(), 0u);
    // Match refcount hits zero — SAI UDF_MATCH should also be removed.
    ASSERT_EQ(g_remove_match_calls, 1);
    ASSERT_EQ(m_orch->m_udfMatches.size(), 0u);
}

TEST_F(UdfOrchTest, PortZeroMatchingSendsL4Attr)
{
    // Regression for _set bool fix: l4_dst_port=0 with explicit mask must
    // produce a SAI UDF_MATCH with the L4_DST_PORT_TYPE attr, not be dropped.
    pushUdf("udf1", {{ "length", "2" }});

    vector<FieldValueTuple> port_zero = {
        {"select_base", "L4"}, {"select_offset", "0"},
        {"match_l4_dst_port", "0"}, {"match_l4_dst_port_mask", "0xFFFF"}, {"match_priority", "5"}
    };
    pushSelector("udf1|sel_a", port_zero);

    ASSERT_EQ(g_create_match_calls, 1);
    ASSERT_TRUE(g_last_match_has_l4);
    ASSERT_EQ(m_orch->m_udfs.size(), 1u);
}

TEST_F(UdfOrchTest, AutoFillMaskCreatesSelector)
{
    // l3_type without l3_type_mask — orchagent auto-fills mask=0xFF.
    pushUdf("udf1", {{ "length", "1" }});

    vector<FieldValueTuple> auto_mask = {
        {"select_base", "L3"}, {"select_offset", "0"},
        {"match_l3_type", "0x11"}, {"match_priority", "10"}
    };
    pushSelector("udf1|sel_a", auto_mask);

    ASSERT_EQ(g_create_match_calls, 1);
    ASSERT_TRUE(g_last_match_has_l3);
    ASSERT_EQ(m_orch->m_udfs.size(), 1u);
}

/* --- ACL integration (UdfOrch side) ------------------------------------- */

TEST_F(UdfOrchTest, GetUdfGroupOidMatchesCreatedGroup)
{
    pushUdf("udf1", {{ "length", "4" }});

    sai_object_id_t oid = m_orch->getUdfGroupOid("udf1");
    ASSERT_NE(oid, SAI_NULL_OBJECT_ID);
    ASSERT_EQ(oid, m_orch->m_udfGroups.at("udf1")->getOid());

    // Non-existent group returns null OID.
    ASSERT_EQ(m_orch->getUdfGroupOid("nonexistent"), SAI_NULL_OBJECT_ID);
}

TEST_F(UdfOrchTest, AclRefcountIncrementDecrement)
{
    pushUdf("udf1", {{ "length", "4" }});

    m_orch->incrementGroupRefCount("udf1");
    m_orch->incrementGroupRefCount("udf1");
    ASSERT_EQ(m_orch->getGroupRefCount("udf1"), 2u);

    m_orch->decrementGroupRefCount("udf1");
    ASSERT_EQ(m_orch->getGroupRefCount("udf1"), 1u);

    // Still referenced — DEL must be blocked.
    pushUdf("udf1", {}, "DEL");
    ASSERT_EQ(m_orch->m_udfGroups.size(), 1u);

    m_orch->decrementGroupRefCount("udf1");
    pushUdf("udf1", {}, "DEL");
    ASSERT_EQ(m_orch->m_udfGroups.size(), 0u);
}

TEST_F(UdfOrchTest, UdfRuleRefcountBlocksLastSelectorDelete)
{
    pushUdf("udf1", {{ "length", "4" }});

    vector<FieldValueTuple> udp_l3 = {
        {"select_base", "L3"}, {"select_offset", "0"},
        {"match_l3_type", "0x11"}, {"match_l3_type_mask", "0xFF"}, {"match_priority", "10"}
    };
    pushSelector("udf1|sel_a", udp_l3);
    ASSERT_EQ(m_orch->m_udfs.size(), 1u);

    // ACL rule holds a reference to this UDF.
    m_orch->incrementUdfRuleRefCount("udf1");
    ASSERT_EQ(m_orch->getUdfRuleRefCount("udf1"), 1u);

    // DEL of the only selector must be blocked while a rule references it.
    pushSelector("udf1|sel_a", {}, "DEL");
    ASSERT_EQ(m_orch->m_udfs.size(), 1u);
    ASSERT_EQ(g_remove_udf_calls, 0);

    // Release the rule reference — DEL must now succeed.
    m_orch->decrementUdfRuleRefCount("udf1");
    pushSelector("udf1|sel_a", {}, "DEL");
    ASSERT_EQ(m_orch->m_udfs.size(), 0u);
    ASSERT_EQ(g_remove_udf_calls, 1);
}

/* --- Direct object-level negative paths --------------------------------- */

TEST_F(UdfOrchTest, UdfGroup_Create_AlreadyExists)
{
    UdfGroupConfig cfg = {"g", SAI_UDF_GROUP_TYPE_GENERIC, 4};
    UdfGroup g(cfg);
    ASSERT_EQ(g.create(), SAI_STATUS_SUCCESS);
    ASSERT_EQ(g.create(), SAI_STATUS_ITEM_ALREADY_EXISTS);
}

TEST_F(UdfOrchTest, UdfGroup_Create_InvalidLength)
{
    UdfGroupConfig zero_len = {"g0", SAI_UDF_GROUP_TYPE_GENERIC, 0};
    UdfGroup g_zero(zero_len);
    ASSERT_EQ(g_zero.create(), SAI_STATUS_INVALID_PARAMETER);

    UdfGroupConfig over_len = {"g1", SAI_UDF_GROUP_TYPE_GENERIC, UDF_GROUP_MAX_LENGTH + 1};
    UdfGroup g_over(over_len);
    ASSERT_EQ(g_over.create(), SAI_STATUS_INVALID_PARAMETER);
}

TEST_F(UdfOrchTest, UdfGroup_Create_NullSaiApi)
{
    UdfGroupConfig cfg = {"g", SAI_UDF_GROUP_TYPE_GENERIC, 4};
    UdfGroup g(cfg);
    auto *saved = sai_udf_api;
    sai_udf_api = nullptr;
    ASSERT_EQ(g.create(), SAI_STATUS_FAILURE);
    sai_udf_api = saved;
}

TEST_F(UdfOrchTest, UdfGroup_Create_SaiFailure)
{
    UdfGroupConfig cfg = {"g", SAI_UDF_GROUP_TYPE_GENERIC, 4};
    UdfGroup g(cfg);
    g_create_group_status = SAI_STATUS_FAILURE;
    ASSERT_EQ(g.create(), SAI_STATUS_FAILURE);
}

TEST_F(UdfOrchTest, UdfGroup_Remove_NullSaiApiAndSaiFailure)
{
    UdfGroupConfig cfg = {"g", SAI_UDF_GROUP_TYPE_GENERIC, 4};
    {
        UdfGroup g(cfg);
        ASSERT_EQ(g.create(), SAI_STATUS_SUCCESS);
        auto *saved = sai_udf_api;
        sai_udf_api = nullptr;
        ASSERT_FALSE(g.remove());
        sai_udf_api = saved;

        g_remove_group_status = SAI_STATUS_FAILURE;
        ASSERT_FALSE(g.remove());
        g_remove_group_status = SAI_STATUS_SUCCESS;
    }
    UdfGroup uncreated(cfg);
    ASSERT_TRUE(uncreated.remove());  // no-op path: m_oid still NULL
}

TEST_F(UdfOrchTest, UdfMatch_Create_AllBranchesCaptured)
{
    UdfMatchConfig cfg = {};
    cfg.name = "m_all";
    cfg.l2_type_set = true;  cfg.l2_type = 0x0800; cfg.l2_type_mask = 0xFFFF;
    cfg.l3_type_set = true;  cfg.l3_type = 0x06;   cfg.l3_type_mask = 0xFF;
    cfg.gre_type_set = true; cfg.gre_type = 0x6558; cfg.gre_type_mask = 0xFFFF;
    cfg.l4_dst_port_set = true; cfg.l4_dst_port = 80; cfg.l4_dst_port_mask = 0xFFFF;
    cfg.priority = 5;
    UdfMatch m(cfg);
    ASSERT_TRUE(m.create());
    ASSERT_TRUE(g_last_match_has_l2);
    ASSERT_TRUE(g_last_match_has_l3);
    ASSERT_TRUE(g_last_match_has_gre);
    ASSERT_TRUE(g_last_match_has_l4);

    // Re-create on same object hits the already-exists branch.
    ASSERT_FALSE(m.create());
}

TEST_F(UdfOrchTest, UdfMatch_NullSaiApiAndSaiFailure)
{
    UdfMatchConfig cfg = {};
    cfg.name = "m"; cfg.l3_type_set = true; cfg.l3_type = 0x11; cfg.l3_type_mask = 0xFF;

    UdfMatch m_null(cfg);
    auto *saved = sai_udf_api;
    sai_udf_api = nullptr;
    ASSERT_FALSE(m_null.create());
    sai_udf_api = saved;

    UdfMatch m_fail(cfg);
    g_create_match_status = SAI_STATUS_FAILURE;
    ASSERT_FALSE(m_fail.create());
    g_create_match_status = SAI_STATUS_SUCCESS;

    UdfMatch m_rm(cfg);
    ASSERT_TRUE(m_rm.create());
    sai_udf_api = nullptr;
    ASSERT_FALSE(m_rm.remove());
    sai_udf_api = saved;
    g_remove_match_status = SAI_STATUS_FAILURE;
    ASSERT_FALSE(m_rm.remove());
    g_remove_match_status = SAI_STATUS_SUCCESS;

    UdfMatch m_noop(cfg);
    ASSERT_TRUE(m_noop.remove());  // m_oid still NULL
}

TEST_F(UdfOrchTest, Udf_Create_NegativePaths)
{
    UdfConfig cfg = {};
    cfg.name = "u"; cfg.base = SAI_UDF_BASE_L3; cfg.offset = 0;

    // Null match_id rejected before SAI call.
    cfg.match_id = SAI_NULL_OBJECT_ID; cfg.group_id = 0x100;
    { Udf u(cfg); ASSERT_FALSE(u.create()); }

    // Null group_id rejected before SAI call.
    cfg.match_id = 0x100; cfg.group_id = SAI_NULL_OBJECT_ID;
    { Udf u(cfg); ASSERT_FALSE(u.create()); }

    // Invalid offset rejected before SAI call.
    cfg.match_id = 0x100; cfg.group_id = 0x200; cfg.offset = UDF_MAX_OFFSET + 1;
    { Udf u(cfg); ASSERT_FALSE(u.create()); }

    // Null SAI api.
    cfg.offset = 0;
    {
        Udf u(cfg);
        auto *saved = sai_udf_api;
        sai_udf_api = nullptr;
        ASSERT_FALSE(u.create());
        sai_udf_api = saved;
    }

    // SAI create failure.
    {
        Udf u(cfg);
        g_create_udf_status = SAI_STATUS_FAILURE;
        ASSERT_FALSE(u.create());
        g_create_udf_status = SAI_STATUS_SUCCESS;
    }

    // Already-exists path.
    {
        Udf u(cfg);
        ASSERT_TRUE(u.create());
        ASSERT_FALSE(u.create());
    }
}

TEST_F(UdfOrchTest, Udf_RemoveNegativePaths)
{
    UdfConfig cfg = {};
    cfg.name = "u"; cfg.match_id = 0x100; cfg.group_id = 0x200;
    cfg.base = SAI_UDF_BASE_L3; cfg.offset = 0;

    Udf u(cfg);
    ASSERT_TRUE(u.create());

    auto *saved = sai_udf_api;
    sai_udf_api = nullptr;
    ASSERT_FALSE(u.remove());
    sai_udf_api = saved;

    g_remove_udf_status = SAI_STATUS_FAILURE;
    ASSERT_FALSE(u.remove());
    g_remove_udf_status = SAI_STATUS_SUCCESS;

    Udf u_noop(cfg);
    ASSERT_TRUE(u_noop.remove());  // m_oid still NULL
}

/* --- Orch task-handler negative paths ----------------------------------- */

TEST_F(UdfOrchTest, FieldTask_UnknownOp)
{
    pushUdf("udf1", {{ "length", "4" }}, "BOGUS_OP");
    ASSERT_EQ(g_create_group_calls, 0);
    ASSERT_EQ(m_orch->m_udfGroups.size(), 0u);
}

TEST_F(UdfOrchTest, FieldTask_RemoveHardwareFails_Retries)
{
    pushUdf("udf1", {{ "length", "4" }});
    g_remove_group_status = SAI_STATUS_FAILURE;
    pushUdf("udf1", {}, "DEL");
    ASSERT_EQ(m_orch->m_udfGroups.size(), 1u);
    auto *c = dynamic_cast<Consumer *>(m_orch->getExecutor(CFG_UDF_TABLE_NAME));
    ASSERT_EQ(c->m_toSync.size(), 1u);

    g_remove_group_status = SAI_STATUS_SUCCESS;
    static_cast<Orch *>(m_orch)->doTask();
    ASSERT_EQ(m_orch->m_udfGroups.size(), 0u);
}

TEST_F(UdfOrchTest, Selector_InvalidKeyFormat)
{
    pushSelector("nopipe", {{"select_base", "L3"}, {"select_offset", "0"},
                            {"match_l3_type", "0x11"}});
    ASSERT_EQ(g_create_match_calls, 0);
}

TEST_F(UdfOrchTest, Selector_MissingSelectBaseOrOffset)
{
    pushUdf("udf1", {{ "length", "4" }});
    pushSelector("udf1|s1", {{"select_offset", "0"}, {"match_l3_type", "0x11"}});
    pushSelector("udf1|s2", {{"select_base",   "L3"}, {"match_l3_type", "0x11"}});
    ASSERT_EQ(g_create_match_calls, 0);
}

TEST_F(UdfOrchTest, Selector_ParseErrorAndInvalidBaseOrOffset)
{
    pushUdf("udf1", {{ "length", "4" }});
    pushSelector("udf1|s1", {{"select_base", "L3"}, {"select_offset", "notanumber"},
                              {"match_l3_type", "0x11"}});
    pushSelector("udf1|s2", {{"select_base", "BOGUS"}, {"select_offset", "0"},
                              {"match_l3_type", "0x11"}});
    pushSelector("udf1|s3", {{"select_base", "L3"}, {"select_offset", "256"},
                              {"match_l3_type", "0x11"}});
    ASSERT_EQ(g_create_match_calls, 0);
    ASSERT_EQ(g_create_udf_calls, 0);
}

TEST_F(UdfOrchTest, Selector_NoMatchCriteria)
{
    pushUdf("udf1", {{ "length", "4" }});
    pushSelector("udf1|s1", {{"select_base", "L3"}, {"select_offset", "0"}});
    ASSERT_EQ(g_create_match_calls, 0);
}

TEST_F(UdfOrchTest, Selector_UnknownOp)
{
    pushUdf("udf1", {{ "length", "4" }});
    pushSelector("udf1|s1", {{"select_base", "L3"}, {"select_offset", "0"},
                              {"match_l3_type", "0x11"}}, "BOGUS_OP");
    ASSERT_EQ(g_create_udf_calls, 0);
}

TEST_F(UdfOrchTest, Selector_L2AndGreBranches)
{
    pushUdf("udf1", {{ "length", "2" }});
    pushSelector("udf1|l2", {{"select_base", "L2"}, {"select_offset", "0"},
                              {"match_l2_type", "0x0800"}});
    ASSERT_TRUE(g_last_match_has_l2);

    pushSelector("udf1|gre", {{"select_base", "L4"}, {"select_offset", "0"},
                               {"match_gre_type", "0x6558"}});
    ASSERT_TRUE(g_last_match_has_gre);
}

TEST_F(UdfOrchTest, Selector_IdempotentReplayShortCircuits)
{
    pushUdf("udf1", {{ "length", "4" }});
    vector<FieldValueTuple> fv = {
        {"select_base", "L3"}, {"select_offset", "0"},
        {"match_l3_type", "0x11"}, {"match_priority", "10"}
    };
    pushSelector("udf1|s1", fv);
    ASSERT_EQ(g_create_match_calls, 1);

    int prior = g_create_match_calls;
    pushSelector("udf1|s1", fv);  // replay must short-circuit before parsing
    ASSERT_EQ(g_create_match_calls, prior);
}

/* --- UdfOrch add/remove API negative paths ------------------------------ */

TEST_F(UdfOrchTest, AddUdfGroup_ImmutableType)
{
    pushUdf("udf1", {{ "length", "4" }, { "field_type", "GENERIC" }});
    ASSERT_EQ(g_create_group_calls, 1);

    // Same name, different type — must be rejected; existing config preserved.
    pushUdf("udf1", {{ "length", "4" }, { "field_type", "HASH" }});
    ASSERT_EQ(g_create_group_calls, 1);
    ASSERT_EQ(m_orch->m_udfGroups.at("udf1")->getConfig().type,
              SAI_UDF_GROUP_TYPE_GENERIC);
}

TEST_F(UdfOrchTest, RemoveUdfGroup_NotFoundReturnsTrue)
{
    pushUdf("never_existed", {}, "DEL");  // drained without error
    ASSERT_EQ(g_remove_group_calls, 0);
}

TEST_F(UdfOrchTest, RemoveUdfGroup_ReferencedByUdfBlocks)
{
    pushUdf("udf1", {{ "length", "4" }});
    pushSelector("udf1|sel", {{"select_base", "L3"}, {"select_offset", "0"},
                               {"match_l3_type", "0x11"}});
    ASSERT_EQ(m_orch->m_udfs.size(), 1u);

    // Direct call bypasses the refcount path and hits the UDF-references check.
    ASSERT_FALSE(m_orch->removeUdfGroup("udf1"));
    ASSERT_EQ(m_orch->m_udfGroups.size(), 1u);
}

TEST_F(UdfOrchTest, AddUdfMatch_NoCriteriaRejected)
{
    UdfMatchConfig cfg = {};
    cfg.name = "m_empty";
    ASSERT_FALSE(m_orch->addUdfMatch("m_empty", cfg));
}

TEST_F(UdfOrchTest, AddUdfMatch_ImmutableFields)
{
    UdfMatchConfig cfg = {};
    cfg.name = "m"; cfg.l3_type_set = true; cfg.l3_type = 0x11; cfg.l3_type_mask = 0xFF;
    ASSERT_TRUE(m_orch->addUdfMatch("m", cfg));

    UdfMatchConfig modified = cfg;
    modified.l3_type = 0x06;
    ASSERT_FALSE(m_orch->addUdfMatch("m", modified));

    // Same config again is a no-op success.
    ASSERT_TRUE(m_orch->addUdfMatch("m", cfg));
}

TEST_F(UdfOrchTest, AddUdfMatch_SaiCreateFailureCleansUp)
{
    UdfMatchConfig cfg = {};
    cfg.name = "m"; cfg.l3_type_set = true; cfg.l3_type = 0x11; cfg.l3_type_mask = 0xFF;
    g_create_match_status = SAI_STATUS_FAILURE;
    ASSERT_FALSE(m_orch->addUdfMatch("m", cfg));
    ASSERT_EQ(m_orch->m_udfMatches.count("m"), 0u);
}

TEST_F(UdfOrchTest, RemoveUdfMatch_NotFoundReturnsTrue)
{
    ASSERT_TRUE(m_orch->removeUdfMatch("nonexistent"));
}

TEST_F(UdfOrchTest, RemoveUdfMatch_ReferencedByUdfBlocks)
{
    pushUdf("udf1", {{ "length", "4" }});
    pushSelector("udf1|sel", {{"select_base", "L3"}, {"select_offset", "0"},
                               {"match_l3_type", "0x11"}});

    const string &matchName = m_orch->m_selectorToMatchName.at("udf1|sel");
    ASSERT_FALSE(m_orch->removeUdfMatch(matchName));
}

TEST_F(UdfOrchTest, AddUdf_NullMatchAndGroupIdsRejected)
{
    UdfConfig cfg = {};
    cfg.name = "u"; cfg.base = SAI_UDF_BASE_L3; cfg.offset = 0;

    cfg.match_id = SAI_NULL_OBJECT_ID; cfg.group_id = 0x100;
    ASSERT_FALSE(m_orch->addUdf("u", cfg));

    cfg.match_id = 0x100; cfg.group_id = SAI_NULL_OBJECT_ID;
    ASSERT_FALSE(m_orch->addUdf("u", cfg));
}

TEST_F(UdfOrchTest, AddUdf_ImmutableFields)
{
    pushUdf("udf1", {{ "length", "4" }});
    pushSelector("udf1|sel", {{"select_base", "L3"}, {"select_offset", "0"},
                               {"match_l3_type", "0x11"}});
    ASSERT_EQ(m_orch->m_udfs.count("udf1|sel"), 1u);

    UdfConfig modified = m_orch->m_udfs.at("udf1|sel")->getConfig();
    modified.offset = 8;
    ASSERT_FALSE(m_orch->addUdf("udf1|sel", modified));

    // Identical config is a no-op success.
    UdfConfig same = m_orch->m_udfs.at("udf1|sel")->getConfig();
    ASSERT_TRUE(m_orch->addUdf("udf1|sel", same));
}

TEST_F(UdfOrchTest, RemoveUdf_NotFoundReturnsTrue)
{
    ASSERT_TRUE(m_orch->removeUdf("nonexistent"));
}

TEST_F(UdfOrchTest, DecrementGroupAndUdfRuleRefCount_Underflow)
{
    // Unknown name — warn and return.
    m_orch->decrementGroupRefCount("never_added");
    m_orch->decrementUdfRuleRefCount("never_added");

    // Zero refcount — warn and return.
    m_orch->incrementGroupRefCount("g");
    m_orch->decrementGroupRefCount("g");
    m_orch->decrementGroupRefCount("g");
    ASSERT_EQ(m_orch->getGroupRefCount("g"), 0u);

    m_orch->incrementUdfRuleRefCount("u");
    m_orch->decrementUdfRuleRefCount("u");
    m_orch->decrementUdfRuleRefCount("u");
    ASSERT_EQ(m_orch->getUdfRuleRefCount("u"), 0u);
}

TEST_F(UdfOrchTest, ReleaseSharedMatch_UnknownNameIsSafe)
{
    m_orch->releaseSharedMatch("nonexistent");  // log warn and return
    ASSERT_EQ(g_remove_match_calls, 0);
}

} // namespace udforch_test
