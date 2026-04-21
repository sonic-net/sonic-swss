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

// Captured from last create_udf_group call
static sai_udf_group_type_t g_last_group_type = SAI_UDF_GROUP_TYPE_GENERIC;
// Set bits seen in last create_udf_match call (L2/L3/GRE/L4)
static bool g_last_match_has_l2   = false;
static bool g_last_match_has_l3   = false;
static bool g_last_match_has_l4   = false;

static void reset_counters()
{
    g_create_group_calls  = g_remove_group_calls = 0;
    g_create_match_calls  = g_remove_match_calls = 0;
    g_create_udf_calls    = g_remove_udf_calls   = 0;
    g_last_group_type     = SAI_UDF_GROUP_TYPE_GENERIC;
    g_last_match_has_l2   = g_last_match_has_l3 = g_last_match_has_l4 = false;
    g_create_group_status = SAI_STATUS_SUCCESS;
}

static sai_status_t g_create_group_status = SAI_STATUS_SUCCESS;

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
    g_remove_group_calls++;
    return SAI_STATUS_SUCCESS;
}
static sai_status_t stub_create_udf_match(sai_object_id_t *oid, sai_object_id_t,
                                            uint32_t attr_count, const sai_attribute_t *attrs)
{
    *oid = ++g_next_oid;
    g_create_match_calls++;
    g_last_match_has_l2 = g_last_match_has_l3 = g_last_match_has_l4 = false;
    for (uint32_t i = 0; i < attr_count; i++)
    {
        if (attrs[i].id == SAI_UDF_MATCH_ATTR_L2_TYPE) g_last_match_has_l2 = true;
        if (attrs[i].id == SAI_UDF_MATCH_ATTR_L3_TYPE) g_last_match_has_l3 = true;
        if (attrs[i].id == SAI_UDF_MATCH_ATTR_L4_DST_PORT_TYPE) g_last_match_has_l4 = true;
    }
    return SAI_STATUS_SUCCESS;
}
static sai_status_t stub_remove_udf_match(sai_object_id_t)
{
    g_remove_match_calls++;
    return SAI_STATUS_SUCCESS;
}
static sai_status_t stub_create_udf(sai_object_id_t *oid, sai_object_id_t, uint32_t, const sai_attribute_t *)
{
    *oid = ++g_next_oid;
    g_create_udf_calls++;
    return SAI_STATUS_SUCCESS;
}
static sai_status_t stub_remove_udf(sai_object_id_t)
{
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

} // namespace udforch_test
