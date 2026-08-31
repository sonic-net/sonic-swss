#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_orch_test.h"
#include "vxlanorch.h"
#include "vnetorch.h"
#include "gtest/gtest.h"
#include <string>

namespace vnetorch_test
{
    using namespace std;
    using namespace mock_orch_test;

    static const string TUNNEL_NAME = "tunnel_ut";
    static const string VNET_NAME   = "Vnet_ut";

    /*
     * Drives VNetOrch::addOperation and VNetOrch::delOperation through
     * the SET_COMMAND / DEL_COMMAND happy paths so the new
     * STATE_VRF_OBJECT_TABLE writes (hset on add, del + the trailing
     * "VNET '%s' was removed" SWSS_LOG_NOTICE on remove) are exercised.
     *
     * Mirrors the MockOrchTest pattern used by mux_rollback_ut.cpp.
     * The MockOrchTest base already wires VxlanTunnelOrch and VNetOrch
     * (with a non-null state DB so STATE_VRF_OBJECT_TABLE is constructed),
     * so the only fixture-specific setup is to pre-register a Vxlan tunnel
     * via VxlanTunnelOrch::addTunnel() — no SAI calls are needed for that
     * registration; the SAI tunnel HW is materialized lazily by
     * createVxlanTunnelMap() inside VNetOrch::addOperation against the
     * SAI VS backend brought up by ut_helper::initSaiApi().
     */
    class VNetOrchTest : public MockOrchTest
    {
    protected:
        void ApplyInitialConfigs() override
        {
            auto src_ip = swss::IpAddress("10.1.0.1");
            auto dst_ip = swss::IpAddress("0.0.0.0");
            auto *tunnel = new VxlanTunnel(TUNNEL_NAME,
                                           src_ip, dst_ip, TNL_CREATION_SRC_CLI);
            m_VxlanTunnelOrch->addTunnel(TUNNEL_NAME, tunnel);
        }

        void driveVNetTask(const std::string &key, const std::string &op,
                           const std::vector<swss::FieldValueTuple> &fvs)
        {
            std::deque<swss::KeyOpFieldsValuesTuple> entries;
            entries.push_back({key, op, fvs});
            auto *consumer = dynamic_cast<Consumer *>(
                m_vnetOrch->getExecutor(APP_VNET_TABLE_NAME));
            ASSERT_NE(consumer, nullptr);
            consumer->addToSync(entries);
            static_cast<Orch *>(m_vnetOrch)->doTask();
        }
    };

    /*
     * Verifies the STATE_VRF_OBJECT_TABLE handshake VNetOrch publishes for
     * vrfmgrd. Without the fix in this commit, addOperation would not
     * populate the row and delOperation would not clear it, breaking the
     * "defer kernel delLink() until VNetOrch is done" coordination that
     * vrfmgrd polls via isVrfObjExist().
     */
    TEST_F(VNetOrchTest, VnetAddDelStateVrfObjectTableLifecycle)
    {
        swss::Table state_vrf_obj_table(m_state_db.get(),
                                        STATE_VRF_OBJECT_TABLE_NAME);

        std::vector<swss::FieldValueTuple> fvs;
        EXPECT_FALSE(state_vrf_obj_table.get(VNET_NAME, fvs));

        /*
         * scope is left empty (i.e. "default") on purpose — that keeps
         * VNetVrfObject::createObj() on the gVirtualRouterId path and
         * avoids exercising the SAI virtual_router_api in this fixture,
         * which is not the subject of this test.
         */
        driveVNetTask(VNET_NAME, SET_COMMAND, {
            {"vxlan_tunnel",   TUNNEL_NAME},
            {"vni",            "5037"},
            {"scope",          ""},
            {"src_mac",        "00:11:22:33:44:55"},
        });

        EXPECT_TRUE(m_vnetOrch->isVnetExists(VNET_NAME));

        fvs.clear();
        ASSERT_TRUE(state_vrf_obj_table.get(VNET_NAME, fvs))
            << "STATE_VRF_OBJECT_TABLE['" << VNET_NAME
            << "'] missing after VNetOrch::addOperation";
        bool seen_state_ok = false;
        for (const auto &fv : fvs)
        {
            if (fvField(fv) == "state" && fvValue(fv) == "ok")
            {
                seen_state_ok = true;
                break;
            }
        }
        EXPECT_TRUE(seen_state_ok)
            << "Expected STATE_VRF_OBJECT_TABLE['" << VNET_NAME
            << "']['state'] == 'ok' after VNetOrch::addOperation";

        driveVNetTask(VNET_NAME, DEL_COMMAND, {});

        EXPECT_FALSE(m_vnetOrch->isVnetExists(VNET_NAME));

        fvs.clear();
        EXPECT_FALSE(state_vrf_obj_table.get(VNET_NAME, fvs))
            << "STATE_VRF_OBJECT_TABLE['" << VNET_NAME
            << "'] still present after VNetOrch::delOperation -- vrfmgrd "
               "would block waiting for it to clear";
    }
}
