import base64
import socket
import pytest
import time
import uuid

from ipaddress import ip_address as IP

from dash_api.appliance_pb2 import *
from dash_api.vnet_pb2 import *
from dash_api.eni_pb2 import *
from dash_api.route_pb2 import *
from dash_api.route_rule_pb2 import *
from dash_api.vnet_mapping_pb2 import *
from dash_api.route_type_pb2 import *
from dash_api.types_pb2 import *
from google.protobuf.json_format import ParseDict, MessageToDict

from dash_utils import dash_db
from sai_attrs import *

DVS_ENV = ["HWSKU=DPU-2P"]
NUM_PORTS = 2

APPLIANCE = {
    "sip": {
        "ipv4": socket.htonl(int(IP("10.0.0.1")))
    },
    "vm_vni": 4321
}

VNET = {
    "vni": "45654",
    "guid": {
        "value": base64.b64encode(bytes.fromhex(uuid.UUID("559c6ce8-26ab-4193-b946-ccc6e8f930b2").hex))
    }
}

PL_ENCODING_IP = "2001:0:20::"
PL_ENCODING_MASK = "::ffff:ffff"
PL_UNDERLAY_SIP = "55.1.2.3"

ENI = {
    "vnet": "Vnet1",
    "underlay_ip": {
        "ipv4": socket.htonl(int(IP("25.1.1.1")))
    },
    "mac_address": bytes.fromhex("F4939FEFC47E"),
    "eni_id": "497f23d7-f0ac-4c99-a98f-59b470e8c7bd",
    "admin_state": State.STATE_ENABLED,
    "pl_underlay_sip": {
        "ipv4": socket.htonl(int(IP(PL_UNDERLAY_SIP)))
    },
    "pl_sip_encoding": {
        "ip": {
            "ipv6": base64.b64encode(IP(PL_ENCODING_IP).packed)
        },
        "mask": {
            "ipv6": base64.b64encode(IP(PL_ENCODING_MASK).packed)
        }
    }
}

PL_OVERLAY_SIP = "fd40:108:0:d204:0:200::0"
PL_OVERLAY_DIP = "2603:10e1:100:2::3401:203"
VNET_MAPPING = {
    "mac_address": bytes.fromhex("F4939FEFC47E"),
    "action_type": RoutingType.ROUTING_TYPE_PRIVATELINK,
    "underlay_ip": {
        "ipv4": socket.htonl(int(IP("101.1.2.3")))
    },
    "overlay_sip": {
        "ipv6": base64.b64encode(IP(PL_OVERLAY_SIP).packed)
    },
    "overlay_dip": {
        "ipv6": base64.b64encode(IP(PL_OVERLAY_DIP).packed)
    },
}

ROUTE = {
    "action_type": RoutingType.ROUTING_TYPE_VNET,
    "vnet": "Vnet1",
}

ENCAP_VNI = 100
ROUTING_TYPE = {
    "items": [
        {
            "action_name": "action1",
            "action_type": ActionType.ACTION_TYPE_4_to_6
        },
        {
            "action_name": "action2",
            "action_type": ActionType.ACTION_TYPE_STATICENCAP,
            "encap_type": EncapType.ENCAP_TYPE_NVGRE,
            "vni": ENCAP_VNI
        }
    ]
}

@pytest.fixture(scope='module', autouse=True)
def common_setup_teardown(dash_db):
    pb = Vnet()
    pb.vni = int("45654")
    pb.guid.value = bytes.fromhex(uuid.UUID("559c6ce8-26ab-4193-b946-ccc6e8f930b2").hex)
    appliance_id = "100"
    appliance_msg = ParseDict(APPLIANCE, Appliance())
    dash_db.create_appliance(appliance_id, {"pb": appliance_msg.SerializeToString()})
    vnet_msg = ParseDict(VNET, Vnet())
    vnet = "Vnet1"
    dash_db.create_vnet(vnet, {"pb": vnet_msg.SerializeToString()})
    eni_msg = ParseDict(ENI, Eni())
    eni = "F4939FEFC47E"
    dash_db.create_eni(eni, {"pb": eni_msg.SerializeToString()})
    routing_type_msg = ParseDict(ROUTING_TYPE, RouteType())
    dash_db.create_routing_type("privatelink", {"pb": routing_type_msg.SerializeToString()})
    vnet_mapping_msg = ParseDict(VNET_MAPPING, VnetMapping())
    vnet_map_ip = "10.1.1.1"
    dash_db.create_vnet_map(vnet, vnet_map_ip, {"pb": vnet_mapping_msg.SerializeToString()})
    route_msg = ParseDict(ROUTE, Route())
    route_prefix = "10.1.0.8/32"
    dash_db.create_outbound_routing(eni, route_prefix, {"pb": route_msg.SerializeToString()})

    time.sleep(3)

def test_pl_eni_attrs(dash_db):
    enis = dash_db.asic_eni_table.get_keys()
    assert enis
    eni_attrs = dash_db.asic_eni_table[enis[0]]
    assert SAI_ENI_ATTR_PL_SIP in eni_attrs
    assert eni_attrs[SAI_ENI_ATTR_PL_SIP] == PL_ENCODING_IP
    assert SAI_ENI_ATTR_PL_SIP_MASK in eni_attrs
    actual_mask = IP(eni_attrs[SAI_ENI_ATTR_PL_SIP_MASK])
    assert actual_mask == IP(PL_ENCODING_MASK)
    assert SAI_ENI_ATTR_PL_UNDERLAY_SIP in eni_attrs
    assert eni_attrs[SAI_ENI_ATTR_PL_UNDERLAY_SIP] == PL_UNDERLAY_SIP

def test_pl_outbound_ca_to_pa_attrs(dash_db):
    outbound_ca_to_pa_keys = dash_db.asic_dash_outbound_ca_to_pa_table.get_keys()
    assert outbound_ca_to_pa_keys
    outbound_attrs = dash_db.asic_dash_outbound_ca_to_pa_table[outbound_ca_to_pa_keys[0]]

    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_ACTION in outbound_attrs
    assert outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_ACTION] == SAI_OUTBOUND_CA_TO_PA_ENTRY_ACTION_SET_PRIVATE_LINK_MAPPING
    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_SIP in outbound_attrs
    actual_overlay_sip = IP(outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_SIP])
    assert actual_overlay_sip == IP(PL_OVERLAY_SIP)
    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_DIP in outbound_attrs
    actual_overlay_dip = IP(outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_DIP])
    assert actual_overlay_dip == IP(PL_OVERLAY_DIP)
    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_TUNNEL_KEY in outbound_attrs
    assert int(outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_TUNNEL_KEY]) == ENCAP_VNI
    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_DASH_ENCAPSULATION in outbound_attrs
    assert outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_DASH_ENCAPSULATION] == SAI_DASH_ENCAPSULATION_NVGRE
