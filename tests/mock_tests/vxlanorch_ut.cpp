#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#define private public
#include "vxlanorch.h"
#undef private
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_orch_test.h"

EXTERN_MOCK_FNS

namespace vxlanorch_test
{
    DEFINE_SAI_GENERIC_API_MOCK(tunnel, tunnel);
    using namespace std;
    using namespace mock_orch_test;
    using ::testing::_;
    using ::testing::Return;
    using ::testing::DoAll;
    using ::testing::SetArgPointee;
    using ::testing::Throw;


    class VxlanOrchTest : public MockOrchTest
    {
        protected:
            void PostSetUp() override
            {
                INIT_SAI_API_MOCK(tunnel);
                MockSaiApis();
            }

            void PreTearDown() override
            {
                RestoreSaiApis();
                DEINIT_SAI_API_MOCK(tunnel);
            }
    };

    TEST_F(VxlanOrchTest, TunnelCreationFailure)
    {
        VxlanTunnelOrch* vxlan_orch = gDirectory.get<VxlanTunnelOrch*>();
        VxlanTunnel* tunnel = nullptr;
        auto src_ip = IpAddress("10.1.0.1");
        auto dst_ip = IpAddress("20.1.0.1");
        tunnel = new VxlanTunnel("vxlan_tunnel_1", src_ip, dst_ip, TNL_CREATION_SRC_CLI);
        vxlan_orch->addTunnel("vxlan_tunnel_1", tunnel);
        EXPECT_CALL(*mock_sai_tunnel_api, create_tunnel(_, _, _, _))
            .WillOnce(DoAll(
                        SetArgPointee<0>(SAI_NULL_OBJECT_ID),
                        Return(SAI_STATUS_FAILURE)
                        ));
        EXPECT_NO_THROW({
                //sai_object_id_t oid = create_tunnel_map(MAP_T::VNI_TO_VLAN_ID);
                //EXPECT_EQ(oid, SAI_NULL_OBJECT_ID);
                vxlan_orch->createVxlanTunnelMap("vxlan_tunnel_1", TUNNEL_MAP_T_VIRTUAL_ROUTER, 1000, 0x1001, 0x1002, 64);
                });
       // EXPECT_EQ(vxlan_orch->getVxlanTunnel("vxlan_tunnel_1"), nullptr);
    }
}
