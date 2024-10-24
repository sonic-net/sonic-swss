import pytest
import time
from dash_db import DashDB, dash_db
from dash_configs import *
from dvslib.sai_utils import assert_sai_attribute_exists
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

@pytest.fixture(autouse=True)
def common_setup_teardown(dash_db: DashDB):
    dash_db.set_app_db_entry(APP_DASH_APPLIANCE_TABLE_NAME, APPLIANCE_ID, APPLIANCE_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_ROUTING_TYPE_TABLE_NAME, PRIVATELINK, ROUTING_TYPE_PL_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_VNET_TABLE_NAME, VNET1, VNET_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_ENI_TABLE_NAME, ENI_ID, ENI_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_VNET_MAPPING_TABLE_NAME, VNET1, VNET_MAP_IP1, VNET_MAPPING_CONFIG_PRIVATELINK)
    # Don't set DASH_ROUTE_TABLE and DASH_ENI_ROUTE_TABLE entries here for flexibility, test cases will set them as needed

    yield

    dash_db.remove_app_db_entry(APP_DASH_ENI_ROUTE_TABLE_NAME, ENI_ID)
    dash_db.remove_app_db_entry(APP_DASH_ROUTE_TABLE_NAME, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX1)
    dash_db.remove_app_db_entry(APP_DASH_ROUTE_TABLE_NAME, ROUTE_GROUP2, OUTBOUND_ROUTE_PREFIX1)
    dash_db.remove_app_db_entry(APP_DASH_ROUTE_GROUP_TABLE_NAME, ROUTE_GROUP1)
    dash_db.remove_app_db_entry(APP_DASH_ROUTE_GROUP_TABLE_NAME, ROUTE_GROUP2)
    dash_db.remove_app_db_entry(APP_DASH_VNET_MAPPING_TABLE_NAME, VNET1, VNET_MAP_IP1)
    dash_db.remove_app_db_entry(APP_DASH_ENI_TABLE_NAME, ENI_ID)
    dash_db.remove_app_db_entry(APP_DASH_VNET_TABLE_NAME, VNET1)
    dash_db.remove_app_db_entry(APP_DASH_ROUTING_TYPE_TABLE_NAME, PRIVATELINK)
    dash_db.remove_app_db_entry(APP_DASH_APPLIANCE_TABLE_NAME, APPLIANCE_ID)

def test_rebind_eni_route_group(dash_db: DashDB):
    dash_db.set_app_db_entry(APP_DASH_ROUTE_GROUP_TABLE_NAME, ROUTE_GROUP1, ROUTE_GROUP1_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_ROUTE_TABLE_NAME, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX1, ROUTE_VNET_CONFIG)
    rg1_oid = dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_ROUTING_GROUP")[0]

    dash_db.set_app_db_entry(APP_DASH_ROUTE_GROUP_TABLE_NAME, ROUTE_GROUP2, ROUTE_GROUP2_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_ROUTE_TABLE_NAME, ROUTE_GROUP2, OUTBOUND_ROUTE_PREFIX1, ROUTE_VNET_CONFIG_UNDERLAY_SIP)
    rg2_oid = dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_ROUTING_GROUP", min_keys=2)[1]

    dash_db.set_app_db_entry(APP_DASH_ENI_ROUTE_TABLE_NAME, ENI_ID, ENI_ROUTE_GROUP1_CONFIG)

    eni_key = dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_ENI")[0]
    dash_db.wait_for_asic_db_field("ASIC_STATE:SAI_OBJECT_TYPE_ENI", eni_key, SAI_ENI_ATTR_OUTBOUND_ROUTING_GROUP_ID, rg1_oid)

    dash_db.set_app_db_entry(APP_DASH_ENI_ROUTE_TABLE_NAME, ENI_ID, ENI_ROUTE_GROUP2_CONFIG)
    dash_db.wait_for_asic_db_field("ASIC_STATE:SAI_OBJECT_TYPE_ENI", eni_key, SAI_ENI_ATTR_OUTBOUND_ROUTING_GROUP_ID, rg2_oid)

def test_duplicate_eni_route_group(dash_db: DashDB):
    dash_db.set_app_db_entry(APP_DASH_ROUTE_GROUP_TABLE_NAME, ROUTE_GROUP1, ROUTE_GROUP1_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_ROUTE_TABLE_NAME, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX1, ROUTE_VNET_CONFIG)
    rg1_oid = dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_ROUTING_GROUP")[0]

    dash_db.set_app_db_entry(APP_DASH_ENI_ROUTE_TABLE_NAME, ENI_ID, ENI_ROUTE_GROUP1_CONFIG)

    eni_key = dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_ENI")[0]
    dash_db.wait_for_asic_db_field("ASIC_STATE:SAI_OBJECT_TYPE_ENI", eni_key, SAI_ENI_ATTR_OUTBOUND_ROUTING_GROUP_ID, rg1_oid)

    dash_db.set_app_db_entry(APP_DASH_ENI_ROUTE_TABLE_NAME, ENI_ID, ENI_ROUTE_GROUP1_CONFIG)
    dash_db.wait_for_asic_db_field("ASIC_STATE:SAI_OBJECT_TYPE_ENI", eni_key, SAI_ENI_ATTR_OUTBOUND_ROUTING_GROUP_ID, rg1_oid)

def test_bound_route_group_immutable(dash_db: DashDB):
    dash_db.set_app_db_entry(APP_DASH_ROUTE_GROUP_TABLE_NAME, ROUTE_GROUP1, ROUTE_GROUP1_CONFIG)
    num_route_groups = len(dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_ROUTING_GROUP"))
    dash_db.set_app_db_entry(APP_DASH_ROUTE_TABLE_NAME, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX1, ROUTE_VNET_CONFIG)
    num_routes = len(dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_ROUTING_ENTRY"))

    dash_db.set_app_db_entry(APP_DASH_ENI_ROUTE_TABLE_NAME, ENI_ID, ENI_ROUTE_GROUP1_CONFIG)
    dash_db.set_app_db_entry(APP_DASH_ROUTE_TABLE_NAME, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX2, ROUTE_VNET_CONFIG_UNDERLAY_SIP)
    time.sleep(3)
    assert len(dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_ROUTING_ENTRY")) == num_routes

    dash_db.remove_app_db_entry(APP_DASH_ROUTE_TABLE_NAME, ROUTE_GROUP1, OUTBOUND_ROUTE_PREFIX1)
    time.sleep(3)
    assert len(dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_ROUTING_ENTRY")) == num_routes

    dash_db.remove_app_db_entry(APP_DASH_ROUTE_GROUP_TABLE_NAME, ROUTE_GROUP1)
    time.sleep(3)
    assert len(dash_db.wait_for_asic_db_keys("ASIC_STATE:SAI_OBJECT_TYPE_OUTBOUND_ROUTING_GROUP")) == num_route_groups
