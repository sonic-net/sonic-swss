#include "gtest/gtest.h"
#include <string>
#include <vector>
#include "../mock_table.h"
#include "warm_restart.h"
#define private public
#include "vxlanmgr.h"
#undef private

extern int (*callback)(const std::string &cmd, std::string &stdout);
extern std::vector<std::string> mockCallArgs;

static int cb(const std::string &cmd, std::string &stdout)
{
    mockCallArgs.push_back(cmd);
    stdout.clear();
    return 0;
}

namespace vxlanmgr_ut
{
    struct VxlanMgrTest : public ::testing::Test
    {
        std::shared_ptr<swss::DBConnector> m_config_db;
        std::shared_ptr<swss::DBConnector> m_app_db;
        std::shared_ptr<swss::DBConnector> m_state_db;
        std::shared_ptr<swss::VxlanMgr> m_vxlanmgr;

        virtual void SetUp() override
        {
            testing_db::reset();
            m_config_db = std::make_shared<swss::DBConnector>("CONFIG_DB", 0);
            m_app_db = std::make_shared<swss::DBConnector>("APPL_DB", 0);
            m_state_db = std::make_shared<swss::DBConnector>("STATE_DB", 0);

            swss::WarmStart::initialize("vxlanmgrd", "swss");

            mockCallArgs.clear();
            callback = cb;

            std::vector<std::string> tables = {
                CFG_VNET_TABLE_NAME,
                CFG_VXLAN_TUNNEL_TABLE_NAME,
                CFG_VXLAN_TUNNEL_MAP_TABLE_NAME,
                CFG_VXLAN_EVPN_NVO_TABLE_NAME,
            };
            m_vxlanmgr = std::make_shared<swss::VxlanMgr>(m_config_db.get(), m_app_db.get(), m_state_db.get(), tables);

            /* Vxlan tunnel, vnet VRFs and router mac must be ready before a vnet netdev is created */
            ASSERT_TRUE(m_vxlanmgr->doVxlanTunnelCreateTask(
                swss::KeyOpFieldsValuesTuple{"tunnel1", SET_COMMAND, {{"src_ip", "10.1.0.32"}}}));

            std::vector<swss::FieldValueTuple> state = {{"state", "ok"}};
            m_vxlanmgr->m_stateVrfTable.set("Vnet1", state);
            m_vxlanmgr->m_stateVrfTable.set("Vnet2", state);
            m_vxlanmgr->m_appSwitchTable.set("switch", {{"vxlan_router_mac", "00:01:02:03:04:05"}});

            mockCallArgs.clear();
        }

        virtual void TearDown() override
        {
            m_vxlanmgr.reset();
            callback = nullptr;
        }

        bool setVnet(const std::string &vnet, const std::string &vni)
        {
            return m_vxlanmgr->doVxlanCreateTask(swss::KeyOpFieldsValuesTuple{
                vnet, SET_COMMAND, {{"vxlan_tunnel", "tunnel1"}, {"vni", vni}}});
        }

        bool delVnet(const std::string &vnet)
        {
            return m_vxlanmgr->doVxlanDeleteTask(swss::KeyOpFieldsValuesTuple{vnet, DEL_COMMAND, {}});
        }

        bool isVxlanStateOk(const std::string &vxlan)
        {
            std::vector<swss::FieldValueTuple> temp;
            return m_vxlanmgr->m_stateVxlanTable.get(vxlan, temp);
        }
    };

    static bool commandWasIssued(const std::string &needle)
    {
        for (const auto &cmd : mockCallArgs)
        {
            if (cmd.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    TEST_F(VxlanMgrTest, VnetsWithSameVniShareKernelNetdev)
    {
        ASSERT_TRUE(setVnet("Vnet1", "1000"));
        ASSERT_TRUE(commandWasIssued("link add \"Vxlan1000\""));
        ASSERT_TRUE(isVxlanStateOk("Vxlan1000"));

        /* Second vnet with the same vni reuses the netdev instead of recreating it */
        mockCallArgs.clear();
        ASSERT_TRUE(setVnet("Vnet2", "1000"));
        ASSERT_TRUE(mockCallArgs.empty());
        ASSERT_EQ(m_vxlanmgr->m_vnetCache.size(), 2);
        ASSERT_EQ(m_vxlanmgr->m_vnetCache["Vnet2"].m_vxlan, "Vxlan1000");

        /* Re-applying the shared vnet keeps sharing the netdev */
        ASSERT_TRUE(setVnet("Vnet2", "1000"));
        ASSERT_TRUE(mockCallArgs.empty());

        /* Removing one of the vnets keeps the netdev for the remaining one */
        ASSERT_TRUE(delVnet("Vnet1"));
        ASSERT_TRUE(mockCallArgs.empty());
        ASSERT_TRUE(isVxlanStateOk("Vxlan1000"));
        ASSERT_EQ(m_vxlanmgr->m_vnetCache.count("Vnet1"), 0);
        ASSERT_EQ(m_vxlanmgr->m_vnetCache.count("Vnet2"), 1);

        /* Removing the last vnet tears the netdev down */
        ASSERT_TRUE(delVnet("Vnet2"));
        ASSERT_TRUE(commandWasIssued("link del \"Brvxlan1000\""));
        ASSERT_TRUE(commandWasIssued("link del dev \"Vxlan1000\""));
        ASSERT_FALSE(isVxlanStateOk("Vxlan1000"));
        ASSERT_TRUE(m_vxlanmgr->m_vnetCache.empty());
    }

    TEST_F(VxlanMgrTest, VnetMovesToSharedVniReleasesOwnNetdev)
    {
        ASSERT_TRUE(setVnet("Vnet1", "1000"));
        ASSERT_TRUE(setVnet("Vnet2", "2000"));
        ASSERT_TRUE(isVxlanStateOk("Vxlan1000"));
        ASSERT_TRUE(isVxlanStateOk("Vxlan2000"));

        /* Vnet2 switches to the vni already owned by Vnet1 */
        mockCallArgs.clear();
        ASSERT_TRUE(setVnet("Vnet2", "1000"));
        ASSERT_TRUE(commandWasIssued("link del dev \"Vxlan2000\""));
        ASSERT_FALSE(commandWasIssued("link add"));
        ASSERT_FALSE(isVxlanStateOk("Vxlan2000"));
        ASSERT_TRUE(isVxlanStateOk("Vxlan1000"));
        ASSERT_EQ(m_vxlanmgr->m_vnetCache["Vnet2"].m_vxlan, "Vxlan1000");
    }

    TEST_F(VxlanMgrTest, VnetUpdateRecreatesOwnNetdev)
    {
        ASSERT_TRUE(setVnet("Vnet1", "1000"));

        /* Not shared: an update deletes and recreates the netdev */
        mockCallArgs.clear();
        ASSERT_TRUE(setVnet("Vnet1", "1000"));
        ASSERT_TRUE(commandWasIssued("link del dev \"Vxlan1000\""));
        ASSERT_TRUE(commandWasIssued("link add \"Vxlan1000\""));
        ASSERT_TRUE(isVxlanStateOk("Vxlan1000"));

        ASSERT_TRUE(delVnet("Vnet1"));
        ASSERT_FALSE(isVxlanStateOk("Vxlan1000"));
    }
}
