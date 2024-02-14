from dash_api.appliance_pb2 import *
from dash_api.vnet_pb2 import *
from dash_api.eni_pb2 import *
from dash_api.route_pb2 import *
from dash_api.route_rule_pb2 import *
from dash_api.vnet_mapping_pb2 import *
from dash_api.route_type_pb2 import *
from dash_api.types_pb2 import *

from dash_utils import DashDB

import time
import uuid
import ipaddress
import socket

DVS_ENV = ["HWSKU=DPU-2P"]
NUM_PORTS = 2

class TestDash(object):
    def test_appliance(self, dvs):
        dashobj = DashDB(dvs)
        self.appliance_id = "100"
        self.sip = "10.0.0.1"
        self.vm_vni = "4321"
        pb = Appliance()
        pb.sip.ipv4 = socket.htonl(int(ipaddress.ip_address(self.sip)))
        pb.vm_vni = int(self.vm_vni)
        dashobj.create_appliance(self.appliance_id, {"pb": pb.SerializeToString()})
        time.sleep(3)

        direction_entries = dashobj.asic_direction_lookup_table.get_keys()
        assert direction_entries
        fvs = dashobj.asic_direction_lookup_table[direction_entries[0]]
        for fv in fvs.items():
            if fv[0] == "SAI_DIRECTION_LOOKUP_ENTRY_ATTR_ACTION":
                assert fv[1] == "SAI_DIRECTION_LOOKUP_ENTRY_ACTION_SET_OUTBOUND_DIRECTION"
        vip_entries = dashobj.asic_vip_table.get_keys()
        assert vip_entries
        fvs = dashobj.asic_vip_table[vip_entries[0]]
        for fv in fvs.items():
            if fv[0] == "SAI_VIP_ENTRY_ATTR_ACTION":
                assert fv[1] == "SAI_VIP_ENTRY_ACTION_ACCEPT"

    def test_vnet(self, dvs):
        dashobj = DashDB(dvs)
        self.vnet = "Vnet1"
        self.vni = "45654"
        self.guid = "559c6ce8-26ab-4193-b946-ccc6e8f930b2"
        pb = Vnet()
        pb.vni = int(self.vni)
        pb.guid.value = bytes.fromhex(uuid.UUID(self.guid).hex)
        dashobj.create_vnet(self.vnet, {"pb": pb.SerializeToString()})
        time.sleep(3)
        vnets = dashobj.asic_dash_vnet_table.get_keys()
        assert vnets
        self.vnet_oid = vnets[0]
        vnet_attr = dashobj.asic_dash_vnet_table[self.vnet_oid]
        assert vnet_attr["SAI_VNET_ATTR_VNI"] == "45654"

    def test_eni(self, dvs):
        dashobj = DashDB(dvs)
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
        dashobj.create_eni(self.mac_string, {"pb": pb.SerializeToString()})
        time.sleep(3)
        vnets = dashobj.asic_dash_vnet_table.get_keys()
        assert vnets
        self.vnet_oid = vnets[0]
        enis = dashobj.asic_eni_table.get_keys()
        assert enis
        self.eni_oid = enis[0];
        fvs = dashobj.asic_eni_table[enis[0]]
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
        eni_addr_maps = dashobj.asic_eni_ether_addr_map_table.get_keys()
        assert eni_addr_maps
        fvs = dashobj.asic_eni_ether_addr_map_table[eni_addr_maps[0]]
        for fv in fvs.items():
            if fv[0] == "SAI_ENI_ETHER_ADDRESS_MAP_ENTRY_ATTR_ENI_ID":
                assert fv[1] == str(self.eni_oid)

    def test_vnet_map(self, dvs):
        dashobj = DashDB(dvs)
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
        dashobj.create_routing_type(self.routing_type, {"pb": route_type_msg.SerializeToString()})
        pb = VnetMapping()
        pb.mac_address = bytes.fromhex(self.mac_address.replace(":", ""))
        pb.action_type = RoutingType.ROUTING_TYPE_VNET_ENCAP
        pb.underlay_ip.ipv4 = socket.htonl(int(ipaddress.ip_address(self.underlay_ip)))

        dashobj.create_vnet_map(self.vnet, self.ip1, {"pb": pb.SerializeToString()})
        dashobj.create_vnet_map(self.vnet, self.ip2, {"pb": pb.SerializeToString()})
        time.sleep(3)

        vnet_ca_to_pa_maps = dashobj.asic_dash_outbound_ca_to_pa_table.get_keys()
        assert len(vnet_ca_to_pa_maps) >= 2
        fvs = dashobj.asic_dash_outbound_ca_to_pa_table[vnet_ca_to_pa_maps[0]]
        for fv in fvs.items():
            if fv[0] == "SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_UNDERLAY_DIP":
                assert fv[1] == "101.1.2.3"
            if fv[0] == "SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_DMAC":
                assert fv[1] == "F4:93:9F:EF:C4:7E"

        vnet_pa_validation_maps = dashobj.asic_pa_validation_table.get_keys()
        assert vnet_pa_validation_maps
        fvs = dashobj.asic_pa_validation_table[vnet_pa_validation_maps[0]]
        for fv in fvs.items():
            if fv[0] == "SAI_PA_VALIDATION_ENTRY_ATTR_ACTION":
                assert fv[1] == "SAI_PA_VALIDATION_ENTRY_ACTION_PERMIT"

    def test_outbound_routing(self, dvs):
        dashobj = DashDB(dvs)
        self.vnet = "Vnet1"
        self.mac_string = "F4939FEFC47E"
        self.ip = "10.1.0.0/24"
        self.action_type = "vnet_direct"
        self.overlay_ip= "10.0.0.6"
        pb = Route()
        pb.action_type = RoutingType.ROUTING_TYPE_VNET_DIRECT
        pb.vnet_direct.vnet = self.vnet
        pb.vnet_direct.overlay_ip.ipv4 = socket.htonl(int(ipaddress.ip_address(self.overlay_ip)))
        dashobj.create_outbound_routing(self.mac_string, self.ip, {"pb": pb.SerializeToString()})
        time.sleep(3)

        outbound_routing_entries = dashobj.asic_outbound_routing_table.get_keys()
        assert outbound_routing_entries
        fvs = dashobj.asic_outbound_routing_table[outbound_routing_entries[0]]
        for fv in fvs.items():
            if fv[0] == "SAI_OUTBOUND_ROUTING_ENTRY_ATTR_ACTION":
                assert fv[1] == "SAI_OUTBOUND_ROUTING_ENTRY_ACTION_ROUTE_VNET_DIRECT"
            if fv[0] == "SAI_OUTBOUND_ROUTING_ENTRY_ATTR_OVERLAY_IP":
                assert fv[1] == "10.0.0.6"
        assert "SAI_OUTBOUND_ROUTING_ENTRY_ATTR_DST_VNET_ID" in fvs

    def test_inbound_routing(self, dvs):
        dashobj = DashDB(dvs)
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

        dashobj.create_inbound_routing(self.mac_string, self.vni, self.ip, {"pb": pb.SerializeToString()})
        time.sleep(3)

        inbound_routing_entries = dashobj.asic_inbound_routing_rule_table.get_keys()
        assert inbound_routing_entries
        fvs = dashobj.asic_inbound_routing_rule_table[inbound_routing_entries[0]]
        for fv in fvs.items():
            if fv[0] == "SAI_INBOUND_ROUTING_ENTRY_ATTR_ACTION":
                assert fv[1] == "SAI_INBOUND_ROUTING_ENTRY_ACTION_VXLAN_DECAP_PA_VALIDATE"

    def test_cleanup(self, dvs):
        dashobj = DashDB(dvs)
        self.vnet = "Vnet1"
        self.mac_string = "F4939FEFC47E"
        self.vni = "3251"
        self.sip = "10.1.1.1"
        self.dip = "10.1.0.0/24"
        self.appliance_id = "100"
        dashobj.remove_inbound_routing(self.mac_string, self.vni, self.sip)
        dashobj.remove_outbound_routing(self.mac_string, self.dip)
        dashobj.remove_eni(self.mac_string)
        dashobj.remove_vnet_map(self.vnet, self.sip)
        dashobj.remove_vnet(self.vnet)
        dashobj.remove_appliance(self.appliance_id)

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down
# before retrying
def test_nonflaky_dummy():
    pass
