import pytest

from dvslib.sai_utils import assert_sai_attribute_exists

from dash_api.appliance_pb2 import *
from dash_api.vnet_pb2 import *
from dash_api.eni_pb2 import *
from dash_api.route_pb2 import *
from dash_api.route_rule_pb2 import *
from dash_api.vnet_mapping_pb2 import *
from dash_api.route_type_pb2 import *
from dash_api.types_pb2 import *

from dash_db import dash_db, DashDB
from dash_configs import *
from sai_attrs import *
from swsscommon.swsscommon import (
    APP_DASH_APPLIANCE_TABLE_NAME,
    APP_DASH_ENI_TABLE_NAME,
    APP_DASH_VNET_TABLE_NAME,
    APP_DASH_VNET_MAPPING_TABLE_NAME,
    APP_DASH_ROUTE_TABLE_NAME,
    APP_DASH_ENI_ROUTE_TABLE_NAME,
    APP_DASH_ROUTING_TYPE_TABLE_NAME,
    APP_DASH_ROUTE_GROUP_TABLE_NAME,
)

DVS_ENV = ["HWSKU=DPU-2P"]
NUM_PORTS = 2

@pytest.fixture(autouse=True)
def common_setup_teardown(dash_db: DashDB):
    dash_db.set_app_db_entry(APP_DASH_APPLIANCE_TABLE_NAME, APPLIANCE_ID, APPLIANCE_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_VNET_TABLE_NAME, VNET1, VNET_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_ENI_TABLE_NAME, ENI_ID, ENI_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_VNET_MAPPING_TABLE_NAME, VNET1, VNET_MAP_IP1, VNET_MAPPING_CONFIG_PRIVATELINK)
    dash_db.set_app_db_entry(APP_DASH_ROUTE_GROUP_TABLE_NAME, ROUTE_GROUP1, ROUTE_GROUP1_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_ROUTING_TYPE_TABLE_NAME, PRIVATELINK, ROUTING_TYPE_PL_CONFIG)
    # Don't set DASH_ROUTE_TABLE and DASH_ENI_ROUTE_TABLE entries here for flexibility, test cases will set them as needed

    yield

    dash_db.remove_app_db_entry(APP_DASH_ENI_ROUTE_TABLE_NAME, ENI_ID)
    dash_db.remove_app_db_entry(APP_DASH_ROUTE_TABLE_NAME, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX1)
    dash_db.remove_app_db_entry(APP_DASH_ROUTE_GROUP_TABLE_NAME, ROUTE_GROUP1)
    dash_db.remove_app_db_entry(APP_DASH_VNET_MAPPING_TABLE_NAME, VNET1, VNET_MAP_IP1)
    dash_db.remove_app_db_entry(APP_DASH_ENI_TABLE_NAME, ENI_ID)
    dash_db.remove_app_db_entry(APP_DASH_VNET_TABLE_NAME, VNET1)
    dash_db.remove_app_db_entry(APP_DASH_APPLIANCE_TABLE_NAME, APPLIANCE_ID)
    dash_db.remove_app_db_entry(APP_DASH_ROUTING_TYPE_TABLE_NAME, PRIVATELINK)


def test_pl_eni_attrs(dash_db: DashDB):
    dash_db.set_app_db_entry(APP_DASH_ROUTE_TABLE_NAME, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX1, ROUTE_VNET_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_ENI_ROUTE_TABLE_NAME, ENI_ID, ENI_ROUTE_GROUP1_CONFIG)

    enis = dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_ENI")
    eni_attrs = dash_db.get_asic_db_entry("ASIC_STATE:SAI_OBJECT_TYPE_ENI", enis[0])
    assert_sai_attribute_exists(SAI_ENI_ATTR_PL_UNDERLAY_SIP, eni_attrs, PL_UNDERLAY_SIP1)
    assert_sai_attribute_exists(SAI_ENI_ATTR_PL_SIP, eni_attrs, PL_ENCODING_IP)
    assert_sai_attribute_exists(SAI_ENI_ATTR_PL_SIP_MASK, eni_attrs, PL_ENCODING_MASK)

def test_pl_eni_override_underlay_sip(dash_db: DashDB):
    dash_db.set_app_db_entry(APP_DASH_ROUTE_TABLE_NAME, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX1, ROUTE_VNET_CONFIG_UNDERLAY_SIP)
    dash_db.set_app_db_entry(APP_DASH_ENI_ROUTE_TABLE_NAME, ENI_ID, ENI_ROUTE_GROUP1_CONFIG)

    outbound_routing_keys = dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_ROUTING_ENTRY")
    outbound_routing_attrs = dash_db.get_asic_db_entry("ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_ROUTING_ENTRY", outbound_routing_keys[0])
    assert_sai_attribute_exists(SAI_OUTBOUND_ROUTING_ENTRY_ATTR_UNDERLAY_SIP, outbound_routing_attrs, PL_UNDERLAY_SIP2)

def test_pl_outbound_ca_to_pa_attrs(dash_db: DashDB):
    outbound_ca_to_pa_keys = dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_CA_TO_PA_ENTRY")
    outbound_attrs = dash_db.get_asic_db_entry("ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_CA_TO_PA_ENTRY", outbound_ca_to_pa_keys[0])

    assert_sai_attribute_exists(SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_ACTION, outbound_attrs, SAI_OUTBOUND_CA_TO_PA_ENTRY_ACTION_SET_PRIVATE_LINK_MAPPING)
    assert_sai_attribute_exists(SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_SIP, outbound_attrs, PL_OVERLAY_SIP)
    assert_sai_attribute_exists(SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_SIP_MASK, outbound_attrs, PL_OVERLAY_SIP_MASK)
    assert_sai_attribute_exists(SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_DIP, outbound_attrs, PL_OVERLAY_DIP)
    assert_sai_attribute_exists(SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_OVERLAY_DIP_MASK, outbound_attrs, PL_OVERLAY_DIP_MASK)
    assert_sai_attribute_exists(SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_TUNNEL_KEY, outbound_attrs, ENCAP_VNI)
    assert_sai_attribute_exists(SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_DASH_ENCAPSULATION, outbound_attrs, SAI_DASH_ENCAPSULATION_NVGRE)
    assert_sai_attribute_exists(SAI_OUTBOUND_CA_TO_PA_ENTRY_ATTR_UNDERLAY_DIP, outbound_attrs, UNDERLAY_IP)
