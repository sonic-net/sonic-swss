#include "mock_dash_orch_test.h"

namespace mock_orch_test
{
    
    void MockDashOrchTest::SetDashTable(std::string table_name, std::string key, const google::protobuf::Message &message, bool set, bool expect_empty)
    {
        auto it = dash_table_orch_map.find(table_name);
        if (it == dash_table_orch_map.end())
        {
            FAIL() << "Table " << table_name << " not found in dash_table_orch_map.";
        }
        Orch* target_orch = it->second;

        auto consumer = make_unique<Consumer>(
            new swss::ConsumerStateTable(m_app_db.get(), table_name),
            target_orch, table_name
        );
        auto op = set ? SET_COMMAND : DEL_COMMAND;
        consumer->addToSync(
            swss::KeyOpFieldsValuesTuple(key, op, {{"pb", message.SerializeAsString()}})
        );
        target_orch->doTask(*consumer.get());

        if (expect_empty)
        {
            auto it = consumer->m_toSync.begin();
            EXPECT_EQ(it, consumer->m_toSync.end())
                << "Expected consumer to be empty after operation on table " << table_name
                << " with key " << key;
        }
    }

    void MockDashOrchTest::CreateApplianceEntry()
    {
        swss::IpAddress sip("1.1.1.1");
        // Table appliance_table = Table(m_app_db.get(), APP_DASH_APPLIANCE_TABLE_NAME);
        dash::appliance::Appliance appliance = dash::appliance::Appliance();
        appliance.mutable_sip()->set_ipv4(sip.getV4Addr());
        appliance.set_local_region_id(100);
        appliance.set_vm_vni(9999);
        // appliance_table.set(appliance1, { { "pb", appliance.SerializeAsString() } });
        // m_DashOrch->addExistingData(&appliance_table);
        // static_cast<Orch *>(m_DashOrch)->doTask();
        SetDashTable(APP_DASH_APPLIANCE_TABLE_NAME, appliance1, appliance);
    }

    void MockDashOrchTest::CreateVnet()
    {
        // Table vnet_table = Table(m_app_db.get(), APP_DASH_VNET_TABLE_NAME);
        dash::vnet::Vnet vnet = dash::vnet::Vnet();
        vnet.set_vni(5555);
        // vnet_table.set(vnet1, { { "pb", vnet.SerializeAsString() } });
        // m_dashVnetOrch->addExistingData(&vnet_table);
        // static_cast<Orch *>(m_dashVnetOrch)->doTask();
        SetDashTable(APP_DASH_VNET_TABLE_NAME, vnet1, vnet);
    }

    void MockDashOrchTest::RemoveVnet(bool expect_empty)
    {
        SetDashTable(APP_DASH_VNET_TABLE_NAME, vnet1, dash::vnet::Vnet(), false, expect_empty);
        // auto consumer = make_unique<Consumer>(
            // new swss::ConsumerStateTable(m_app_db.get(), APP_DASH_VNET_TABLE_NAME),
            // m_dashVnetOrch, APP_DASH_VNET_TABLE_NAME
        // );
        // consumer->addToSync({vnet1, "DEL", {}});
        // static_cast<Orch *>(m_dashVnetOrch)->doTask(*consumer.get());
    }

    void MockDashOrchTest::AddRoutingType(dash::route_type::EncapType encap_type)
    {
        // Table route_type_table = Table(m_app_db.get(), APP_DASH_ROUTING_TYPE_TABLE_NAME);
        dash::route_type::RouteType route_type = dash::route_type::RouteType();
        dash::route_type::RouteTypeItem *rt_item = route_type.add_items();
        rt_item->set_action_type(dash::route_type::ACTION_TYPE_STATICENCAP);
        rt_item->set_encap_type(encap_type);
        // route_type_table.set("VNET_ENCAP", { { "pb", route_type.SerializeAsString() } });
        // m_DashOrch->addExistingData(&route_type_table);
        // static_cast<Orch *>(m_DashOrch)->doTask();
        SetDashTable(APP_DASH_ROUTING_TYPE_TABLE_NAME, "VNET_ENCAP", route_type);
    }

    void MockDashOrchTest::AddOutboundRoutingGroup()
    {
        // Table route_group_table = Table(m_app_db.get(), APP_DASH_ROUTE_GROUP_TABLE_NAME);
        dash::route_group::RouteGroup route_group = dash::route_group::RouteGroup();
        route_group.set_version("1");
        route_group.set_guid("group_guid");
        // route_group_table.set(route_group1, { { "pb", route_group.SerializeAsString() } });
        // m_DashRouteOrch->addExistingData(&route_group_table);
        // static_cast<Orch *>(m_DashOrch)->doTask();
        SetDashTable(APP_DASH_ROUTE_GROUP_TABLE_NAME, route_group1, route_group);
    }

    void MockDashOrchTest::AddOutboundRoutingEntry(bool expect_empty)
    {
        // Table route_table = Table(m_app_db.get(), APP_DASH_ROUTE_TABLE_NAME);
        dash::route::Route route = dash::route::Route();
        route.set_routing_type(dash::route_type::ROUTING_TYPE_VNET);
        route.set_vnet(vnet1);
        route.set_tunnel(tunnel1);
        // route_table.set(route_group1 + ":1.2.3.4/32", { { "pb", route.SerializeAsString() } });
        // m_DashRouteOrch->addExistingData(&route_table);
        // static_cast<Orch *>(m_DashRouteOrch)->doTask();
        SetDashTable(APP_DASH_ROUTE_TABLE_NAME, route_group1 + ":1.2.3.4/32", route, true, expect_empty);
    }

    void MockDashOrchTest::AddTunnel()
    {
        // Table tunnel_table = Table(m_app_db.get(), APP_DASH_TUNNEL_TABLE_NAME);
        dash::tunnel::Tunnel tunnel = dash::tunnel::Tunnel();
        tunnel.set_encap_type(dash::route_type::ENCAP_TYPE_VXLAN);
        tunnel.set_vni(5555);
        // tunnel_table.set(tunnel1, { { "pb", tunnel.SerializeAsString() } });
        // m_DashTunnelOrch->addExistingData(&tunnel_table);
        // static_cast<Orch *>(m_DashTunnelOrch)->doTask();
        SetDashTable(APP_DASH_TUNNEL_TABLE_NAME, tunnel1, tunnel);
    }

    void MockDashOrchTest::AddVnetMap(bool expect_empty)
    {
        // Table vnet_map_table = Table(m_app_db.get(), APP_DASH_VNET_MAPPING_TABLE_NAME);
        dash::vnet_mapping::VnetMapping vnet_map = dash::vnet_mapping::VnetMapping();
        vnet_map.set_routing_type(dash::route_type::ROUTING_TYPE_VNET_ENCAP);
        vnet_map.mutable_underlay_ip()->set_ipv4(swss::IpAddress("7.7.7.7").getV4Addr());
        // vnet_map_table.set(vnet1 + ":" + vnet_map_ip1, { { "pb", vnet_map.SerializeAsString() } });
        // m_dashVnetOrch->addExistingData(&vnet_map_table);
        // static_cast<Orch *>(m_dashVnetOrch)->doTask();
        SetDashTable(APP_DASH_VNET_MAPPING_TABLE_NAME, vnet1 + ":" + vnet_map_ip1, vnet_map, true, expect_empty);
    }

    void MockDashOrchTest::RemoveVnetMap()
    {
        SetDashTable(APP_DASH_VNET_MAPPING_TABLE_NAME, vnet1 + ":" + vnet_map_ip1, dash::vnet_mapping::VnetMapping(), false);
        // auto consumer = make_unique<Consumer>(
            // new swss::ConsumerStateTable(m_app_db.get(), APP_DASH_VNET_MAPPING_TABLE_NAME),
            // m_dashVnetOrch, APP_DASH_VNET_MAPPING_TABLE_NAME
        // );
        // consumer->addToSync({vnet1 + ":" + vnet_map_ip1, "DEL", {}});
        // static_cast<Orch *>(m_dashVnetOrch)->doTask(*consumer.get());
    }
}
