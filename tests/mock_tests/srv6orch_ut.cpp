#include "mock_orch_test.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "ut_helper.h"

#include <gtest/gtest.h>
#include <deque>

using namespace std;
using namespace swss;

EXTERN_MOCK_FNS

namespace srv6orch_test
{

DEFINE_SAI_GENERIC_API_MOCK(tunnel, tunnel);
DEFINE_SAI_API_MOCK(srv6, my_sid);

using ::testing::_;
using ::testing::AtLeast;
using namespace mock_orch_test;

class Srv6OrchMySidTest : public MockOrchTest
{
protected:
    void PostSetUp() override
    {
        INIT_SAI_API_MOCK(tunnel);
        INIT_SAI_API_MOCK(srv6);
        MockSaiApis();
    }

    void PreTearDown() override
    {
        RestoreSaiApis();
        DEINIT_SAI_API_MOCK(srv6);
        DEINIT_SAI_API_MOCK(tunnel);
    }

    void addLocatorConfig(const string& locator_name)
    {
        Table locator_table(m_config_db.get(), CFG_SRV6_MY_LOCATOR_TABLE_NAME);
        vector<FieldValueTuple> fvs = {
            {"block_len", "32"},
            {"node_len", "16"},
            {"func_len", "16"},
            {"arg_len", "0"}
        };
        locator_table.set(locator_name, fvs);
    }

    void runCfgMySidTask(const string& key, const vector<FieldValueTuple>& fvs, bool is_set = true)
    {
        auto* executor = static_cast<Orch*>(gSrv6Orch)->getExecutor(CFG_SRV6_MY_SID_TABLE_NAME);
        auto* consumer = dynamic_cast<Consumer*>(executor);
        ASSERT_NE(consumer, nullptr);
        deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({key, is_set ? SET_COMMAND : DEL_COMMAND, fvs});
        consumer->addToSync(entries);
        static_cast<Orch*>(gSrv6Orch)->doTask(*consumer);
    }

    void runAppMySidTask(const string& key, const string& action, const string& vrf,
                        const string& adj, bool is_set = true)
    {
        auto* executor = static_cast<Orch*>(gSrv6Orch)->getExecutor(APP_SRV6_MY_SID_TABLE_NAME);
        auto* consumer = dynamic_cast<Consumer*>(executor);
        ASSERT_NE(consumer, nullptr);
        vector<FieldValueTuple> fvs = {{"action", action}};
        if (!vrf.empty())
            fvs.push_back({"vrf", vrf});
        if (!adj.empty())
            fvs.push_back({"adj", adj});
        deque<KeyOpFieldsValuesTuple> entries;
        entries.push_back({key, is_set ? SET_COMMAND : DEL_COMMAND, fvs});
        consumer->addToSync(entries);
        static_cast<Orch*>(gSrv6Orch)->doTask(*consumer);
    }
};

TEST_F(Srv6OrchMySidTest, MySidEntryCreation_WithDecapDscpMode)
{
    ASSERT_NE(gSrv6Orch, nullptr);

    const string locator = "loc1";
    const string my_sid_prefix = "fc00:0:1:1::/64";
    const string cfg_key = locator + "|" + my_sid_prefix;
    const string app_key = "32:16:16:0:fc00:0:1:1::";

    addLocatorConfig(locator);

    EXPECT_CALL(*mock_sai_tunnel_api, create_tunnel(_, _, _, _)).Times(AtLeast(1));

    runCfgMySidTask(cfg_key, {{"decap_dscp_mode", "uniform"}});
    runAppMySidTask(app_key, "un", "default", "");
}

TEST_F(Srv6OrchMySidTest, MySidEntryCreation_WithoutDecapDscpMode)
{
    ASSERT_NE(gSrv6Orch, nullptr);

    const string locator = "loc1";
    const string my_sid_prefix = "fc00:0:1:1::/64";
    const string cfg_key = locator + "|" + my_sid_prefix;
    const string app_key = "32:16:16:0:fc00:0:1:1::";

    addLocatorConfig(locator);

    EXPECT_CALL(*mock_sai_tunnel_api, create_tunnel(_, _, _, _)).Times(0);

    runCfgMySidTask(cfg_key, {});
    runAppMySidTask(app_key, "un", "default", "");
}

} // namespace srv6orch_test
