#define private public
#include "directory.h"
#undef private
#define protected public
#include "orch.h"
#undef protected
#include "ut_helper.h"
#include "mock_orchagent_main.h"
#include "mock_sai_api.h"
#include "mock_orch_test.h"
#include "dash_api/appliance.pb.h"
#include "dash_api/route_type.pb.h"
#include "dash_api/eni.pb.h"
#include "dash_api/qos.pb.h"
#include "dash_api/eni_route.pb.h"


EXTERN_MOCK_FNS

namespace dashorch_test
{
    using namespace mock_orch_test;
    class DashOrchTest : public MockOrchTest
    {
        protected:
            bool GetRouteTypeActions(dash::route_type::RoutingType routing_type, dash::route_type::RouteType& route_type)
            {
                return m_DashOrch->getRouteTypeActions(routing_type, route_type);
            }
    };

    TEST_F(DashOrchTest, GetNonExistRoutingType)
    {   
        dash::route_type::RouteType route_type;
        bool success = m_DashOrch->getRouteTypeActions(dash::route_type::RoutingType::ROUTING_TYPE_DIRECT, route_type);
        EXPECT_FALSE(success);
    }

    TEST_F(DashOrchTest, RemoveNonExistRoutingType)
    {
        bool success = m_DashOrch->removeRoutingTypeEntry(dash::route_type::RoutingType::ROUTING_TYPE_VNET);
        EXPECT_TRUE(success);
    }
}