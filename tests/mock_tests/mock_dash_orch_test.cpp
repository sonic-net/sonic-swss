#include "mock_dash_orch_test.h"

namespace mock_orch_test
{
    void MockDashOrchTest::CreateApplianceEntry()
    {
        swss::IpAddress sip("1.1.1.1");
        Table appliance_table = Table(m_app_db.get(), APP_DASH_APPLIANCE_TABLE_NAME);
        dash::appliance::Appliance appliance = dash::appliance::Appliance();
        appliance.mutable_sip()->set_ipv4(sip.getV4Addr());
        appliance.set_local_region_id(100);
        appliance.set_vm_vni(9999);
        appliance_table.set("APPLIANCE_1", { { "pb", appliance.SerializeAsString() } });
        m_DashOrch->addExistingData(&appliance_table);
        static_cast<Orch *>(m_DashOrch)->doTask();
    }

    void MockDashOrchTest::CreateVnet()
    {
        Table vnet_table = Table(m_app_db.get(), APP_DASH_VNET_TABLE_NAME);
        dash::vnet::Vnet vnet = dash::vnet::Vnet();
        vnet.set_vni(5555);
        vnet_table.set("VNET_1", { { "pb", vnet.SerializeAsString() } });
        m_dashVnetOrch->addExistingData(&vnet_table);
        static_cast<Orch *>(m_dashVnetOrch)->doTask();
    }

    void MockDashOrchTest::AddRoutingType(dash::route_type::EncapType encap_type)
    {
        Table route_type_table = Table(m_app_db.get(), APP_DASH_ROUTING_TYPE_TABLE_NAME);
        dash::route_type::RouteType route_type = dash::route_type::RouteType();
        dash::route_type::RouteTypeItem *rt_item = route_type.add_items();
        rt_item->set_action_type(dash::route_type::ACTION_TYPE_STATICENCAP);
        rt_item->set_encap_type(encap_type);
        route_type_table.set("VNET_ENCAP", { { "pb", route_type.SerializeAsString() } });
        m_DashOrch->addExistingData(&route_type_table);
        static_cast<Orch *>(m_DashOrch)->doTask();
    }
}