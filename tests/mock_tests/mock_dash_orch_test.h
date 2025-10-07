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
                { APP_DASH_OUTBOUND_PORT_MAP_TABLE_NAME, (Orch **)&m_dashPortMapOrch },
                { APP_DASH_OUTBOUND_PORT_MAP_RANGE_TABLE_NAME, (Orch **)&m_dashPortMapOrch }
            };
            void SetDashTable(std::string table_name, std::string key, const google::protobuf::Message &message, bool set = true, bool expect_empty = true);
            dash::appliance::Appliance BuildApplianceEntry();
            void CreateApplianceEntry();
            void AddVnetEncapRoutingType(dash::route_type::EncapType encap_type);
            void AddPLRoutingType();
            void CreateVnet();
            void RemoveVnet(bool expect_empty = true);
            void AddVnetMap(bool expect_empty = true, std::string vnet_ip = vnet_map_ip1);
            void RemoveVnetMap(bool expect_empty = true, std::string vnet_ip = vnet_map_ip1);
            void AddOutboundRoutingGroup();
            void AddOutboundRoutingEntry(bool expect_empty = true);
            void AddTunnel();
            void AddVnetMapPL(bool expect_empty = true);
            void RemoveVnetMapPL(bool expect_empty = true);
            void AddPortMap();
            dash::eni::Eni BuildEniEntry();

            static std::string vnet1;
            static std::string vnet_map_ip1;
            static std::string vnet_map_ip2;
            static std::string vnet_map_underlay_ip;
            static std::string appliance1;
            static std::string route_group1;
            static std::string tunnel1;
            static std::string eni1;
            static std::string portmap1;
    };
}
