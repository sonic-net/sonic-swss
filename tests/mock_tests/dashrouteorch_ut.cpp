#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_dash_orch_test.h"
#include "dash_api/appliance.pb.h"
#include "dash_api/route_type.pb.h"
#include "dash_api/eni.pb.h"
#include "dash_api/qos.pb.h"
#include "dash_api/eni_route.pb.h"

EXTERN_MOCK_FNS
namespace dashrouteorch_test
{
    DEFINE_SAI_API_MOCK(dash_outbound_routing, outbound_routing);
    using namespace mock_orch_test;
    using ::testing::InSequence;
    class DashRouteOrchTest : public MockDashOrchTest
    {
        void PostSetUp()
        {
            CreateApplianceEntry();
            CreateVnet();
        }

        void ApplySaiMock()
        {
            INIT_SAI_API_MOCK(dash_outbound_routing);
            MockSaiApis();
        }

        void PreTearDown()
        {
            RestoreSaiApis();
            DEINIT_SAI_API_MOCK(dash_outbound_routing);
        }
    };

    TEST_F(DashRouteOrchTest, RouteWithMissingTunnelNotAdded)
    {
        {
            InSequence seq;
            EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries).Times(0);
            EXPECT_CALL(*mock_sai_dash_outbound_routing_api, create_outbound_routing_entries).Times(1);
        }
        AddOutboundRoutingGroup();
        AddOutboundRoutingEntry(false);
        
        AddTunnel();
        AddOutboundRoutingEntry();
    }
}