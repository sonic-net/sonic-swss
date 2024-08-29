from dash_api.appliance_pb2 import *
from dash_api.vnet_pb2 import *
from dash_api.eni_pb2 import *
from dash_api.eni_route_pb2 import *
from dash_api.route_pb2 import *
from dash_api.route_group_pb2 import *
from dash_api.route_rule_pb2 import *
from dash_api.vnet_mapping_pb2 import *
from dash_api.route_type_pb2 import *
from dash_api.types_pb2 import *

from dash_db import dash_db
from dash_configs import *

import time
import uuid
import ipaddress
import socket

DVS_ENV = ["HWSKU=DPU-2P"]
NUM_PORTS = 2

class TestDash(object):
    def test_appliance(self, dash_db):
        self.appliance_id = "100"
        self.sip = "10.0.0.1"
        self.vm_vni = "4321"
        pb = Appliance()
        pb.sip.ipv4 = socket.htonl(int(ipaddress.ip_address(self.sip)))
        pb.vm_vni = int(self.vm_vni)
        dash_db.create_appliance(self.appliance_id, {"pb": pb.SerializeToString()})
        time.sleep(3)

        direction_entries = dash_db.asic_direction_lookup_table.get_keys()
        assert direction_entries
        fvs = dash_db.asic_direction_lookup_table[direction_entries[0]]
        for fv in fvs.items():
            if fv[0] == "SAI_DIRECTION_LOOKUP_ENTRY_ATTR_ACTION":
                assert fv[1] == "SAI_DIRECTION_LOOKUP_ENTRY_ACTION_SET_OUTBOUND_DIRECTION"
        vip_entries = dash_db.asic_vip_table.get_keys()
        assert vip_entries
        fvs = dash_db.asic_vip_table[vip_entries[0]]
        for fv in fvs.items():
            if fv[0] == "SAI_VIP_ENTRY_ATTR_ACTION":
                assert fv[1] == "SAI_VIP_ENTRY_ACTION_ACCEPT"

    def test_vnet(self, dash_db):
        self.vnet = "Vnet1"
        self.vni = "45654"
        self.guid = "559c6ce8-26ab-4193-b946-ccc6e8f930b2"
        pb = Vnet()
        pb.vni = int(self.vni)
        pb.guid.value = bytes.fromhex(uuid.UUID(self.guid).hex)
        dash_db.create_vnet(self.vnet, {"pb": pb.SerializeToString()})
        time.sleep(3)
        vnets = dash_db.asic_dash_vnet_table.get_keys()
        assert vnets
        self.vnet_oid = vnets[0]
        vnet_attr = dash_db.asic_dash_vnet_table[self.vnet_oid]
        assert vnet_attr["SAI_VNET_ATTR_VNI"] == "45654"

    def test_eni(self, dash_db):
        self.vnet = "Vnet1"
        self.mac_string = "F4939FEFC47E"
        self.mac_address = "F4:93:9F:EF:C4:7E"
        self.eni_id = "497f23d7-f0ac-4c99-a98f-59b470e8c7bd"
        self.underlay_ip = "25.1.1.1"
        self.admin_state = "enabled"
        pb = Eni()
        pb.eni_id = self.eni_id
        pb.mac_address = bytes.fromhex(self.mac_address.replace(":", ""))
        pb.underlay_ip.ipv4 = socket.htonl(int(ipaddress.ip_address(self.underlay_ip)))
        pb.admin_state = State.STATE_ENABLED
        pb.vnet = self.vnet
        dash_db.create_eni(self.mac_string, {"pb": pb.SerializeToString()})
        time.sleep(3)
        vnets = dash_db.asic_dash_vnet_table.get_keys()
        assert vnets
        self.vnet_oid = vnets[0]
        enis = dash_db.asic_eni_table.get_keys()
        assert enis
        self.eni_oid = enis[0]
        fvs = dash_db.asic_eni_table[enis[0]]
        for fv in fvs.items():
            if fv[0] == "SAI_ENI_ATTR_VNET_ID":
                assert fv[1] == str(self.vnet_oid)
            if fv[0] == "SAI_ENI_ATTR_PPS":
                assert fv[1] == 0
            if fv[0] == "SAI_ENI_ATTR_CPS":
                assert fv[1] == 0
            if fv[0] == "SAI_ENI_ATTR_FLOWS":
                assert fv[1] == 0
            if fv[0] == "SAI_ENI_ATTR_ADMIN_STATE":
                assert fv[1] == "true"

        time.sleep(3)
        eni_addr_maps = dash_db.asic_eni_ether_addr_map_table.get_keys()
        assert eni_addr_maps
        fvs = dash_db.asic_eni_ether_addr_map_table[eni_addr_maps[0]]
        for fv in fvs.items():
            if fv[0] == "SAI_ENI_ETHER_ADDRESS_MAP_ENTRY_ATTR_ENI_ID":
                assert fv[1] == str(self.eni_oid)

        # test admin state update
        pb.admin_state = State.STATE_DISABLED
        dash_db.create_eni(self.mac_string, {"pb": pb.SerializeToString()})
        time.sleep(3)
        enis = dash_db.asic_eni_table.get_keys()
        assert len(enis) == 1
        assert enis[0] == self.eni_oid
        eni_attrs = dash_db.asic_eni_table[self.eni_oid]
        assert eni_attrs["SAI_ENI_ATTR_ADMIN_STATE"] == "false"

    def test_vnet_map(self, dash_db):
        self.vnet = "Vnet1"
        self.ip1 = "10.1.1.1"
        self.ip2 = "10.1.1.2"
        self.mac_address = "F4:93:9F:EF:C4:7E"
        self.routing_type = "vnet_encap"
        self.underlay_ip = "101.1.2.3"
        route_type_msg = RouteType()
        route_action = RouteTypeItem()
        route_action.action_name = "action1"
        route_action.action_type = ACTION_TYPE_MAPROUTING
        route_type_msg.items.append(route_action)
        dash_db.create_routing_type(self.routing_type, {"pb": route_type_msg.SerializeToString()})
        pb = VnetMapping()
        pb.mac_address = bytes.fromhex(self.mac_address.replace(":", ""))
        pb.action_type = RoutingType.ROUTING_TYPE_VNET_ENCAP
        pb.underlay_ip.ipv4 = socket.htonl(int(ipaddress.ip_address(self.underlay_ip)))

        dash_db.create_vnet_mapping(self.vnet, self.ip1, {"pb": pb.SerializeToString()})
        dash_db.create_vnet_mapping(self.vnet, self.ip2, {"pb": pb.SerializeToString()})
        time.sleep(3)

        vnet_ca_to_pa_maps = dash_db.asic_dash_outbound_ca_to_pa_table.get_keys()
        assert len(vnet_ca_to_pa_maps) >= 2
        fvs = dash_db.asic_dash_outbound_ca_to_pa_table[vnet_ca_to_pa_maps[0]]
        for fv in fvs.items():
            if fv[0] == "SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_UNDERLAY_DIP":
                assert fv[1] == "101.1.2.3"
            if fv[0] == "SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_DMAC":
                assert fv[1] == "F4:93:9F:EF:C4:7E"

        vnet_pa_validation_maps = dash_db.asic_pa_validation_table.get_keys()
        assert vnet_pa_validation_maps
        fvs = dash_db.asic_pa_validation_table[vnet_pa_validation_maps[0]]
        for fv in fvs.items():
            if fv[0] == "SAI_PA_VALIDATION_ENTRY_ATTR_ACTION":
                assert fv[1] == "SAI_PA_VALIDATION_ENTRY_ACTION_PERMIT"

    def test_outbound_routing(self, dash_db):
        pb = RouteGroup()
        self.group_id = ROUTE_GROUP1
        dash_db.create_route_group(self.group_id, {"pb": pb.SerializeToString()})
        time.sleep(3)
        outbound_routing_group_entries = dash_db.asic_outbound_routing_group_table.get_keys()
        assert outbound_routing_group_entries

        self.vnet = "Vnet1"
        self.ip = "10.1.0.0/24"
        self.action_type = "vnet_direct"
        self.overlay_ip = "10.0.0.6"
        pb = Route()
        pb.action_type = RoutingType.ROUTING_TYPE_VNET_DIRECT
        pb.vnet_direct.vnet = self.vnet
        pb.vnet_direct.overlay_ip.ipv4 = socket.htonl(int(ipaddress.ip_address(self.overlay_ip)))
        dash_db.create_route(self.group_id, self.ip, {"pb": pb.SerializeToString()})
        time.sleep(3)
        outbound_routing_entries = dash_db.asic_outbound_routing_table.get_keys()
        assert outbound_routing_entries
        fvs = dash_db.asic_outbound_routing_table[outbound_routing_entries[0]]
        for fv in fvs.items():
            if fv[0] == "SAI_OUTBOUND_ROUTING_ENTRY_ATTR_ACTION":
                assert fv[1] == "SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET_DIRECT"
            if fv[0] == "SAI_OUTBOUND_ROUTING_ENTRY_ATTR_OVERLAY_IP":
                assert fv[1] == "10.0.0.6"
        assert "SAI_OUTBOUND_ROUTING_ENTRY_ATTR_DST_VNET_ID" in fvs

        pb = EniRoute()
        pb.group_id = self.group_id
        self.mac_string = "F4939FEFC47E"
        dash_db.create_eni_route(self.mac_string, {"pb": pb.SerializeToString()})
        time.sleep(3)
        eni_entries = dash_db.asic_eni_table.get_keys()
        fvs = dash_db.asic_eni_table[eni_entries[0]]
        for fv in fvs.items():
            if fv[0] == "SAI_ENI_ATTR_OUTBOUND_ROUTING_GROUP_ID":
                assert fv[1] == outbound_routing_group_entries[0]

    def test_inbound_routing(self, dash_db):
        self.mac_string = "F4939FEFC47E"
        self.vnet = "Vnet1"
        self.vni = "3251"
        self.ip = "10.1.1.1"
        self.action_type = "decap"
        self.pa_validation = "true"
        self.priority = "1"
        self.protocol = "0"
        pb = RouteRule()
        pb.pa_validation = True
        pb.priority = int(self.priority)
        pb.protocol = int(self.protocol)
        pb.vnet = self.vnet

        dash_db.create_inbound_routing(self.mac_string, self.vni, self.ip, {"pb": pb.SerializeToString()})
        time.sleep(3)

        inbound_routing_entries = dash_db.asic_inbound_routing_rule_table.get_keys()
        assert inbound_routing_entries
        fvs = dash_db.asic_inbound_routing_rule_table[inbound_routing_entries[0]]
        for fv in fvs.items():
            if fv[0] == "SAI_INBOUND_ROUTING_ENTRY_ATTR_ACTION":
                assert fv[1] == "SAI_INBOUND_ROUTING_ENTRY_ACTION_TUNNEL_DECAP_PA_VALIDATE"

    def test_cleanup(self, dash_db):
        self.vnet = "Vnet1"
        self.mac_string = "F4939FEFC47E"
        self.group_id = ROUTE_GROUP1
        self.vni = "3251"
        self.sip = "10.1.1.1"
        self.dip = "10.1.0.0/24"
        self.ip2 = "10.1.1.2"
        self.appliance_id = "100"
        self.routing_type = "vnet_encap"
        dash_db.remove_inbound_routing(self.mac_string, self.vni, self.sip)
        dash_db.remove_eni_route(self.mac_string)
        dash_db.remove_route(self.group_id, self.dip)
        dash_db.remove_route_group(self.group_id)
        dash_db.remove_vnet_mapping(self.vnet, self.sip)
        dash_db.remove_vnet_mapping(self.vnet, self.ip2)
        dash_db.remove_routing_type(self.routing_type)
        dash_db.remove_eni(self.mac_string)
        dash_db.remove_vnet(self.vnet)
        dash_db.remove_appliance(self.appliance_id)

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down
# before retrying
def test_nonflaky_dummy():
    pass
