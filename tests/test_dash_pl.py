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

from dash_utils.dash_db import dash_db
from dash_utils.dash_utils import *
from dash_utils.dash_configs import *
from sai_attrs import *

DVS_ENV = ["HWSKU=DPU-2P"]
NUM_PORTS = 2

@pytest.fixture
def apply_base_pl_route(dash_db):
    create_route(dash_db, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX, ROUTE_PL_CONFIG)
    yield
    remove_route(dash_db, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX)

@pytest.fixture
def apply_pl_route_with_underlay_sip(dash_db):
    create_route(dash_db, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX, ROUTE_PL_CONFIG_WITH_UNDERLAY_SIP)
    yield
    remove_route(dash_db, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX)


@pytest.fixture(scope='module', autouse=True)
def common_setup_teardown(dash_db):
    create_routing_type(dash_db, PRIVATELINK, ROUTING_TYPE_PL_CONFIG)
    create_appliance(dash_db, APPLIANCE_ID, APPLIANCE_CONFIG)
    create_vnet(dash_db, VNET1, VNET_CONFIG)
    create_eni(dash_db, ENI_ID, ENI_CONFIG)
    create_vnet_mapping(dash_db, VNET1, VNET_MAP_IP1, VNET_MAPPING_CONFIG)
    create_route_group(dash_db, ROUTE_GROUP1, ROUTE_GROUP_CONFIG)
    create_eni_route(dash_db, ENI_ID, ENI_ROUTE_CONFIG)

    time.sleep(3)

    yield

    remove_eni_route(dash_db, ENI_ID)
    remove_route_group(dash_db, ROUTE_GROUP1)
    remove_vnet_mapping(dash_db, VNET1, VNET_MAP_IP1)
    remove_eni(dash_db, ENI_ID)
    remove_vnet(dash_db, VNET1)
    remove_appliance(dash_db, APPLIANCE_ID)
    remove_routing_type(dash_db, PRIVATELINK)


def test_pl_eni_attrs(dash_db, apply_base_pl_route):
    enis = dash_db.asic_eni_table.get_keys()
    assert enis
    eni_attrs = dash_db.asic_eni_table[enis[0]]
    assert SAI_ENI_ATTR_PL_SIP in eni_attrs
    assert eni_attrs[SAI_ENI_ATTR_PL_SIP] == PL_ENCODING_IP
    assert SAI_ENI_ATTR_PL_SIP_MASK in eni_attrs
    actual_mask = IP(eni_attrs[SAI_ENI_ATTR_PL_SIP_MASK])
    assert actual_mask == IP(PL_ENCODING_MASK)
    assert SAI_ENI_ATTR_PL_UNDERLAY_SIP in eni_attrs
    assert eni_attrs[SAI_ENI_ATTR_PL_UNDERLAY_SIP] == PL_UNDERLAY_SIP1

def test_pl_eni_override_underlay_sip(dash_db, apply_pl_route_with_underlay_sip):
    enis = dash_db.asic_eni_table.get_keys()
    assert enis
    eni_attrs = dash_db.asic_eni_table[enis[0]]
    assert SAI_ENI_ATTR_PL_UNDERLAY_SIP in eni_attrs
    assert eni_attrs[SAI_ENI_ATTR_PL_UNDERLAY_SIP] == PL_UNDERLAY_SIP2

def test_pl_outbound_ca_to_pa_attrs(dash_db):
    outbound_ca_to_pa_keys = dash_db.asic_dash_outbound_ca_to_pa_table.get_keys()
    assert outbound_ca_to_pa_keys
    outbound_attrs = dash_db.asic_dash_outbound_ca_to_pa_table[outbound_ca_to_pa_keys[0]]

    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_ACTION in outbound_attrs
    assert outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_ACTION] == SAI_OUTBOUND_CA_TO_PA_ENTRY_ACTION_SET_PRIVATE_LINK_MAPPING
    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_SIP_MASK in outbound_attrs
    actual_overlay_sip = IP(outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_SIP_MASK])
    assert actual_overlay_sip == IP(PL_OVERLAY_SIP_PREFIX)
    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTRY_OVERLAY_DIP_MASK in outbound_attrs
    actual_overlay_dip = IP(outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTRY_OVERLAY_DIP_MASK])
    assert actual_overlay_dip == IP(PL_OVERLAY_DIP_PREFIX)
    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_TUNNEL_KEY in outbound_attrs
    assert int(outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_TUNNEL_KEY]) == ENCAP_VNI
    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_DASH_ENCAPSULATION in outbound_attrs
    assert outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_DASH_ENCAPSULATION] == SAI_DASH_ENCAPSULATION_NVGRE
