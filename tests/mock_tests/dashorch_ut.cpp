#define private public
#include "orchagent/directory.h"
#undef private
#define protected public
#include "orchagent/orch.h"
#undef protected
#include "tests/mock_tests/ut_helper.h"
#include "tests/mock_tests/mock_orchagent_main.h"
#include "tests/mock_tests/mock_sai_api.h"
#include "tests/mock_tests/mock_dash_orch_test.h"
#include "dash_api/appliance.pb.h"
#include "dash_api/route_type.pb.h"
#include "dash_api/eni.pb.h"
#include "dash_api/qos.pb.h"
#include "dash_api/eni_route.pb.h"


EXTERN_MOCK_FNS

namespace dashorch_test
{
    using namespace mock_orch_test;
    class DashOrchTest : public MockDashOrchTest {};

    TEST_F(DashOrchTest, GetNonExistRoutingType)
    {   
        dash::route_type::RouteType route_type;
        bool success = m_DashOrch->getRouteTypeActions(dash::route_type::RoutingType::ROUTING_TYPE_DIRECT, route_type);
        EXPECT_FALSE(success);
    }

    TEST_F(DashOrchTest, DuplicateRoutingTypeEntry)
    {
        dash::route_type::RouteType route_type1;
        dash::route_type::RouteTypeItem *item1 = route_type1.add_items();
        item1->set_action_type(dash::route_type::ActionType::ACTION_TYPE_STATICENCAP);
        bool success = m_DashOrch->addRoutingTypeEntry(dash::route_type::RoutingType::ROUTING_TYPE_VNET, route_type1);
        EXPECT_TRUE(success);
        EXPECT_EQ(m_DashOrch->routing_type_entries_.size(), 1);
        EXPECT_EQ(m_DashOrch->routing_type_entries_[dash::route_type::RoutingType::ROUTING_TYPE_VNET].items()[0].action_type(), item1->action_type());

        dash::route_type::RouteType route_type2;
        dash::route_type::RouteTypeItem *item2 = route_type2.add_items();
        item2->set_action_type(dash::route_type::ActionType::ACTION_TYPE_DECAP);
        success = m_DashOrch->addRoutingTypeEntry(dash::route_type::RoutingType::ROUTING_TYPE_VNET, route_type2);
        EXPECT_TRUE(success);
        EXPECT_EQ(m_DashOrch->routing_type_entries_[dash::route_type::RoutingType::ROUTING_TYPE_VNET].items()[0].action_type(), item1->action_type());
    }

    TEST_F(DashOrchTest, RemoveNonExistRoutingType)
    {
        bool success = m_DashOrch->removeRoutingTypeEntry(dash::route_type::RoutingType::ROUTING_TYPE_DROP);
        EXPECT_TRUE(success);
    }
}