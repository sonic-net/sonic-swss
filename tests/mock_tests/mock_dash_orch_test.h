#include "mock_orch_test.h"
#include <google/protobuf/message.h>

namespace mock_orch_test
{
    class MockDashOrchTest : public MockOrchTest
    {
        protected:
            // Orchs may not be initialized yet so we need double pointers to access them once they are initialized
            std::unordered_map<std::string, Orch**> dash_table_orch_map = {
                {APP_DASH_VNET_TABLE_NAME, (Orch**) &m_dashVnetOrch},
                {APP_DASH_VNET_MAPPING_TABLE_NAME, (Orch**) &m_dashVnetOrch},
                {APP_DASH_APPLIANCE_TABLE_NAME, (Orch**) &m_DashOrch},
                {APP_DASH_ROUTING_TYPE_TABLE_NAME, (Orch**) &m_DashOrch},
                {APP_DASH_ROUTE_GROUP_TABLE_NAME, (Orch**) &m_DashRouteOrch},
                {APP_DASH_ROUTE_TABLE_NAME, (Orch**) &m_DashRouteOrch},
                {APP_DASH_TUNNEL_TABLE_NAME, (Orch**) &m_DashTunnelOrch},
                {APP_DASH_ENI_TABLE_NAME, (Orch**) &m_DashOrch},
            };
            void SetDashTable(std::string table_name, std::string key, const google::protobuf::Message &message, bool set = true, bool expect_empty = true);
            dash::appliance::Appliance BuildApplianceEntry();
            void CreateApplianceEntry();
            void AddRoutingType(dash::route_type::EncapType encap_type);
            void CreateVnet();
            void RemoveVnet(bool expect_empty = true);
            void AddVnetMap(bool expect_empty = true);
            void RemoveVnetMap();
            void AddOutboundRoutingGroup();
            void AddOutboundRoutingEntry(bool expect_empty = true);
            void AddTunnel();
            dash::eni::Eni BuildEniEntry();

            std::string vnet1 = "VNET_1";
            std::string vnet_map_ip1 = "2.2.2.2";
            std::string appliance1 = "APPLIANCE_1";
            std::string route_group1 = "ROUTE_GROUP_1";
            std::string tunnel1 = "TUNNEL_1";
            std::string eni1 = "ENI_1";
    };
}