#include "mock_orch_test.h"

namespace mock_orch_test
{
    class MockDashOrchTest : public MockOrchTest
    {
        protected:
            void CreateApplianceEntry();
            void AddRoutingType(dash::route_type::EncapType encap_type);
            void CreateVnet();
            void RemoveVnet();
            void AddVnetMap();
            void RemoveVnetMap();
            void AddOutboundRoutingGroup();
            void AddOutboundRoutingEntry();
            void AddTunnel();

            std::string vnet1 = "VNET_1";
            std::string vnet_map_ip1 = "2.2.2.2";
            std::string appliance1 = "APPLIANCE_1";
            std::string route_group1 = "ROUTE_GROUP_1";
            std::string tunnel1 = "TUNNEL_1";
    };
}