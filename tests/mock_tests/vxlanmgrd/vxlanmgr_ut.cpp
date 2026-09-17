#include "gtest/gtest.h"
#include <string>
#include <vector>
#include <sstream>
#include <memory>
#include "schema.h"
#include "warm_restart.h"
#include "table.h"
#include "macaddress.h"

#define private public
#include "vxlanmgr.h"
#undef private

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;

swss::MacAddress gMacAddress;

namespace vxlanmgr_ut
{

using namespace swss;

static int vxlan_cb(const std::string &cmd, std::string &stdout)
{
    mockCallArgs.push_back(cmd);
    stdout.clear();
    return 0;
}

static bool cmdHasTokens(const std::string &tokens)
{
    std::istringstream iss(tokens);
    std::vector<std::string> toks;
    std::string t;
    while (iss >> t) toks.push_back(t);
    for (const auto &c : mockCallArgs)
    {
        bool ok = true;
        for (const auto &tok : toks)
        {
            if (c.find(tok) == std::string::npos) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

struct VxlanMgrTest : public ::testing::Test
{
    std::shared_ptr<swss::DBConnector> m_cfg_db;
    std::shared_ptr<swss::DBConnector> m_app_db;
    std::shared_ptr<swss::DBConnector> m_state_db;
    std::vector<std::string> m_tables;

    void SetUp() override
    {
        m_cfg_db = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
        m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
        m_state_db = std::make_shared<swss::DBConnector>("STATE_DB", 0);
        m_cfg_db->flushdb();
        m_app_db->flushdb();
        m_state_db->flushdb();
        swss::WarmStart::initialize("vxlanmgrd", "swss");
        m_tables = {};
        mockCallArgs.clear();
        callback = vxlan_cb;
    }

    void TearDown() override
    {
        callback = nullptr;
    }

    void setSwitch(const std::vector<FieldValueTuple> &fvs)
    {
        swss::Table(m_app_db.get(), APP_SWITCH_TABLE_NAME).set("switch", fvs);
    }
};

TEST_F(VxlanMgrTest, SwitchTableConfigDefaultDstPortWhenOnlyMacSet)
{
    setSwitch({{"vxlan_router_mac", "aa:bb:cc:dd:ee:ff"}});
    VxlanMgr mgr(m_cfg_db.get(), m_app_db.get(), m_state_db.get(), m_tables);
    ASSERT_TRUE(mgr.getSwitchTableVxlanConfig());
    ASSERT_EQ(mgr.m_VxlanSwitchTableConfig.m_routerMac, "aa:bb:cc:dd:ee:ff");
    ASSERT_EQ(mgr.m_VxlanSwitchTableConfig.m_vxlanDstPort, "4789");
    ASSERT_TRUE(mgr.m_VxlanSwitchTableConfig.m_vxlanSrcPortRangeStart.empty());
    ASSERT_TRUE(mgr.m_VxlanSwitchTableConfig.m_vxlanSrcPortRangeEnd.empty());
}

TEST_F(VxlanMgrTest, SwitchTableConfigReadsCustomDstPort)
{
    setSwitch({{"vxlan_router_mac", "aa:bb:cc:dd:ee:ff"},
               {"vxlan_port", "4788"}});
    VxlanMgr mgr(m_cfg_db.get(), m_app_db.get(), m_state_db.get(), m_tables);
    ASSERT_TRUE(mgr.getSwitchTableVxlanConfig());
    ASSERT_EQ(mgr.m_VxlanSwitchTableConfig.m_vxlanDstPort, "4788");
}

TEST_F(VxlanMgrTest, SwitchTableConfigComputesSrcPortRange)
{
    setSwitch({{"vxlan_router_mac", "aa:bb:cc:dd:ee:ff"},
               {"vxlan_sport", "32768"},
               {"vxlan_mask", "4"}});
    VxlanMgr mgr(m_cfg_db.get(), m_app_db.get(), m_state_db.get(), m_tables);
    ASSERT_TRUE(mgr.getSwitchTableVxlanConfig());
    ASSERT_EQ(mgr.m_VxlanSwitchTableConfig.m_vxlanSrcPortRangeStart, "32768");
    ASSERT_EQ(mgr.m_VxlanSwitchTableConfig.m_vxlanSrcPortRangeEnd, "32783");
}

TEST_F(VxlanMgrTest, VxlanCreationProgramsCustomSrcAndDstPorts)
{
    setSwitch({{"vxlan_router_mac", "aa:bb:cc:dd:ee:ff"},
               {"vxlan_port", "4788"},
               {"vxlan_sport", "32775"},
               {"vxlan_mask", "4"}});
    swss::Table(m_state_db.get(), STATE_VRF_TABLE_NAME).set("Vnet1", {{"state", "ok"}});
    VxlanMgr mgr(m_cfg_db.get(), m_app_db.get(), m_state_db.get(), m_tables);
    KeyOpFieldsValuesTuple tunnel{"tunnel0", SET_COMMAND, {{"src_ip", "10.0.0.1"}}};
    ASSERT_TRUE(mgr.doVxlanTunnelCreateTask(tunnel));

    mockCallArgs.clear();
    KeyOpFieldsValuesTuple vnet{"Vnet1", SET_COMMAND,
                                {{"vxlan_tunnel", "tunnel0"}, {"vni", "1000"}}};
    ASSERT_TRUE(mgr.doVxlanCreateTask(vnet));

    ASSERT_TRUE(cmdHasTokens("link add Vxlan1000 type vxlan id 1000 local 10.0.0.1 "
                            "dstport 4788 srcport 32768 32783"));
    ASSERT_TRUE(mgr.isVxlanStateOk("Vxlan1000"));
}

TEST_F(VxlanMgrTest, SwitchTableConfigRejectsInvalidMask)
{
    setSwitch({{"vxlan_router_mac", "aa:bb:cc:dd:ee:ff"},
               {"vxlan_sport", "32768"},
               {"vxlan_mask", "17"}});
    VxlanMgr mgr(m_cfg_db.get(), m_app_db.get(), m_state_db.get(), m_tables);
    ASSERT_TRUE(mgr.getSwitchTableVxlanConfig());
    ASSERT_TRUE(mgr.m_VxlanSwitchTableConfig.m_vxlanSrcPortRangeStart.empty());
    ASSERT_TRUE(mgr.m_VxlanSwitchTableConfig.m_vxlanSrcPortRangeEnd.empty());
}

TEST_F(VxlanMgrTest, SwitchTableConfigRejectsOutOfRangeSport)
{
    setSwitch({{"vxlan_router_mac", "aa:bb:cc:dd:ee:ff"},
               {"vxlan_sport", "70000"},
               {"vxlan_mask", "4"}});
    VxlanMgr mgr(m_cfg_db.get(), m_app_db.get(), m_state_db.get(), m_tables);
    ASSERT_TRUE(mgr.getSwitchTableVxlanConfig());
    ASSERT_TRUE(mgr.m_VxlanSwitchTableConfig.m_vxlanSrcPortRangeStart.empty());
    ASSERT_TRUE(mgr.m_VxlanSwitchTableConfig.m_vxlanSrcPortRangeEnd.empty());
}

TEST_F(VxlanMgrTest, SwitchTableConfigDefersWhenRowMissing)
{
    VxlanMgr mgr(m_cfg_db.get(), m_app_db.get(), m_state_db.get(), m_tables);
    ASSERT_FALSE(mgr.getSwitchTableVxlanConfig());
    ASSERT_TRUE(mgr.m_VxlanSwitchTableConfig.m_routerMac.empty());
}

}  // namespace vxlanmgr_ut
