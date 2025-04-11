#include "mock_orch_test.h"

namespace mock_orch_test
{
    class MockDashOrchTest : public MockOrchTest
    {
        protected:
            void CreateApplianceEntry();
            void AddRoutingType(dash::route_type::EncapType encap_type);
            void CreateVnet();
    };
}