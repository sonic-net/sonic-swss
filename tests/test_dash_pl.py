import pytest

from ipaddress import ip_address as IP

from dash_api.appliance_pb2 import *
from dash_api.vnet_pb2 import *
from dash_api.eni_pb2 import *
from dash_api.route_pb2 import *
from dash_api.route_rule_pb2 import *
from dash_api.vnet_mapping_pb2 import *
from dash_api.route_type_pb2 import *
from dash_api.types_pb2 import *

from dash_utils.dash_db import dash_db, DashDB
from dash_utils.dash_utils import *
from dash_utils.dash_configs import *
from sai_attrs import *
from swsscommon.swsscommon import APP_DASH_APPLIANCE_TABLE_NAME, APP_DASH_ENI_TABLE_NAME, APP_DASH_VNET_TABLE_NAME, APP_DASH_VNET_MAPPING_TABLE_NAME, APP_DASH_ROUTE_TABLE_NAME, APP_DASH_ROUTE_RULE_TABLE_NAME, APP_DASH_ENI_ROUTE_TABLE_NAME, APP_DASH_ROUTING_TYPE_TABLE_NAME, APP_DASH_ROUTE_GROUP_TABLE_NAME

DVS_ENV = ["HWSKU=DPU-2P"]
NUM_PORTS = 2

@pytest.fixture
def apply_base_pl_route(dash_db: DashDB):
    
    dash_db.set_app_db_entry(APP_DASH_ROUTE_TABLE_NAME, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX, ROUTE_PL_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_ENI_ROUTE_TABLE_NAME, ENI_ID, ENI_ROUTE_CONFIG)
    yield
    dash_db.remove_app_db_entry(APP_DASH_ENI_ROUTE_TABLE_NAME, ENI_ID )
    dash_db.remove_app_db_entry(APP_DASH_ROUTE_TABLE_NAME, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX)

@pytest.fixture
def apply_pl_route_with_underlay_sip(dash_db: DashDB):
    dash_db.set_app_db_entry(APP_DASH_ROUTE_TABLE_NAME, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX, ROUTE_PL_CONFIG_WITH_UNDERLAY_SIP)
    dash_db.set_app_db_entry(APP_DASH_ENI_ROUTE_TABLE_NAME, ENI_ID, ENI_ROUTE_CONFIG)
    yield
    dash_db.remove_app_db_entry(APP_DASH_ENI_ROUTE_TABLE_NAME, ENI_ID)
    dash_db.remove_app_db_entry(APP_DASH_ROUTE_TABLE_NAME, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX)


@pytest.fixture(scope='module', autouse=True)
def common_setup_teardown(dash_db: DashDB):
    dash_db.set_app_db_entry(APP_DASH_ROUTING_TYPE_TABLE_NAME, PRIVATELINK, ROUTING_TYPE_PL_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_APPLIANCE_TABLE_NAME, APPLIANCE_ID, APPLIANCE_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_VNET_TABLE_NAME, VNET1, VNET_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_ENI_TABLE_NAME, ENI_ID, ENI_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_VNET_MAPPING_TABLE_NAME, VNET1, VNET_MAP_IP1, VNET_MAPPING_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_ROUTE_GROUP_TABLE_NAME, ROUTE_GROUP1, ROUTE_GROUP_CONFIG)

    yield

    dash_db.remove_app_db_entry(APP_DASH_ROUTE_GROUP_TABLE_NAME, ROUTE_GROUP1)
    dash_db.remove_app_db_entry(APP_DASH_VNET_MAPPING_TABLE_NAME, VNET1, VNET_MAP_IP1)
    dash_db.remove_app_db_entry(APP_DASH_ENI_TABLE_NAME, ENI_ID)
    dash_db.remove_app_db_entry(APP_DASH_VNET_TABLE_NAME, VNET1)
    dash_db.remove_app_db_entry(APP_DASH_APPLIANCE_TABLE_NAME, APPLIANCE_ID)
    dash_db.remove_app_db_entry(APP_DASH_ROUTING_TYPE_TABLE_NAME, PRIVATELINK)


def test_pl_eni_attrs(dash_db: DashDB, apply_base_pl_route):
    enis= dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_ENI")
    assert enis
    eni_attrs = dash_db.get_asic_db_entry("ASIC_STATE:SAI_OBJECT_TYPE_ENI", enis[0])
    assert SAI_ENI_ATTR_PL_UNDERLAY_SIP in eni_attrs
    assert eni_attrs[SAI_ENI_ATTR_PL_UNDERLAY_SIP] == PL_UNDERLAY_SIP1

def test_pl_eni_override_underlay_sip(dash_db: DashDB, apply_pl_route_with_underlay_sip):
    outbound_routing_keys = dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_ROUTING_ENTRY")
    assert outbound_routing_keys
    outbound_routing_attrs = dash_db.get_asic_db_entry("ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_ROUTING_ENTRY", outbound_routing_keys[0])
    assert SAI_OUTBOUND_ROUTING_ENTRY_ATTR_UNDERLAY_SIP in outbound_routing_attrs
    assert IP(outbound_routing_attrs[SAI_OUTBOUND_ROUTING_ENTRY_ATTR_UNDERLAY_SIP]) == IP(PL_UNDERLAY_SIP2)

def test_pl_outbound_ca_to_pa_attrs(dash_db: DashDB):
    outbound_ca_to_pa_keys = dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_CA_TO_PA_ENTRY")
    assert outbound_ca_to_pa_keys
    outbound_attrs = dash_db.get_asic_db_entry("ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_CA_TO_PA_ENTRY", outbound_ca_to_pa_keys[0])

    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_ACTION in outbound_attrs
    assert outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_ACTION] == SAI_OUTBOUND_CA_TO_PA_ENTRY_ACTION_SET_PRIVATE_LINK_MAPPING

    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_SIP in outbound_attrs
    actual_overlay_sip = IP(outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_SIP])
    assert actual_overlay_sip == IP(PL_OVERLAY_SIP)
    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_SIP_MASK in outbound_attrs
    actual_overlay_sip_mask = IP(outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_SIP_MASK])
    assert actual_overlay_sip_mask == IP(PL_OVERLAY_SIP_MASK)

    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_DIP in outbound_attrs
    actual_overlay_dip = IP(outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_DIP])
    assert actual_overlay_dip == IP(PL_OVERLAY_DIP)
    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_DIP_MASK in outbound_attrs
    actual_overlay_dip_mask = IP(outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_DIP_MASK])
    assert actual_overlay_dip_mask == IP(PL_OVERLAY_DIP_MASK)

    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_TUNNEL_KEY in outbound_attrs
    assert int(outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_TUNNEL_KEY]) == ENCAP_VNI
    assert SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_DASH_ENCAPSULATION in outbound_attrs
    assert outbound_attrs[SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_DASH_ENCAPSULATION] == SAI_DASH_ENCAPSULATION_NVGRE
