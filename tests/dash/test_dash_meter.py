from dash_api.appliance_pb2 import *
from dash_api.vnet_pb2 import *
from dash_api.eni_pb2 import *
from dash_api.eni_route_pb2 import *
from dash_api.route_pb2 import *
from dash_api.route_group_pb2 import *
from dash_api.route_rule_pb2 import *
from dash_api.vnet_mapping_pb2 import *
from dash_api.route_type_pb2 import *
from dash_api.meter_policy_pb2 import *
from dash_api.meter_rule_pb2 import *
from dash_api.types_pb2 import *
from dvslib.dvs_flex_counter import TestFlexCountersBase

from dash_db import DashDB, dash_db_module as dash_db
from dash_db import ASIC_METER_POLICY_TABLE, ASIC_METER_RULE_TABLE, ASIC_ENI_TABLE
from dash_configs import *

import time
import uuid
import ipaddress
import socket

from dvslib.sai_utils import assert_sai_attribute_exists
from dvslib.dvs_common import PollingConfig, wait_for_result

from swsscommon.swsscommon import (
    APP_DASH_METER_POLICY_TABLE_NAME,
    APP_DASH_METER_RULE_TABLE_NAME,
    APP_DASH_ENI_TABLE_NAME,
    APP_DASH_VNET_TABLE_NAME,
    APP_DASH_APPLIANCE_TABLE_NAME,
)

eni_counter_group_meta = {
    'key': 'ENI',
    'group_name': 'ENI_STAT_COUNTER',
    'name_map': 'COUNTERS_ENI_NAME_MAP',
    'post_test': 'post_eni_counter_test'
}

meter_counter_group_meta = {
    'key': 'DASH_METER',
    'group_name': 'METER_STAT_COUNTER',
    'name_map': 'COUNTERS_ENI_NAME_MAP',
    'post_test': 'post_meter_counter_test'
}

DVS_ENV = ["HWSKU=DPU-2P"]
NUM_PORTS = 2

ENTRIES = 2
policy_v4_oid = 0
policy_v6_oid = 0
rule_v4_oid = 0
rule_v6_oid = 0

class TestDashMeter(TestFlexCountersBase):

    def test_v4_meter(self, dash_db: DashDB):
        global policy_v4_oid
        global rule_v4_oid

        dash_db.set_app_db_entry(APP_DASH_METER_POLICY_TABLE_NAME, METER_POLICY_V4, METER_POLICY_V4_CONFIG)
        policy_v4_oid = dash_db.wait_for_asic_db_keys(ASIC_METER_POLICY_TABLE)[0]
        policy_attrs = dash_db.get_asic_db_entry(ASIC_METER_POLICY_TABLE, policy_v4_oid)
        assert_sai_attribute_exists("SAI_METER_POLICY_ATTR_IP_ADDR_FAMILY", policy_attrs, "SAI_IP_ADDR_FAMILY_IPV4")

        dash_db.set_app_db_entry(APP_DASH_METER_RULE_TABLE_NAME, METER_POLICY_V4, METER_RULE_1_NUM, METER_RULE_1_CONFIG)
        rule_v4_oid = dash_db.wait_for_asic_db_keys(ASIC_METER_RULE_TABLE)[0]
        rule_attrs = dash_db.get_asic_db_entry(ASIC_METER_RULE_TABLE, rule_v4_oid)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_PRIORITY", rule_attrs, METER_RULE_1_PRIORITY)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_METER_CLASS", rule_attrs, METER_RULE_1_METERING_CLASS)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_METER_POLICY_ID", rule_attrs, policy_v4_oid)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_DIP", rule_attrs, METER_RULE_1_IP)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_DIP_MASK", rule_attrs, METER_RULE_1_IP_MASK)

    def test_v6_meter(self, dash_db: DashDB):
        global policy_v6_oid
        global rule_v6_oid

        dash_db.set_app_db_entry(APP_DASH_METER_POLICY_TABLE_NAME, METER_POLICY_V6, METER_POLICY_V6_CONFIG)
        oids = dash_db.wait_for_asic_db_keys(ASIC_METER_POLICY_TABLE, min_keys=ENTRIES)
        for oid in oids:
            if oid != policy_v4_oid:
                policy_v6_oid = oid
                break
        policy_attrs = dash_db.get_asic_db_entry(ASIC_METER_POLICY_TABLE, policy_v6_oid)
        assert_sai_attribute_exists("SAI_METER_POLICY_ATTR_IP_ADDR_FAMILY", policy_attrs, "SAI_IP_ADDR_FAMILY_IPV6")

        dash_db.set_app_db_entry(APP_DASH_METER_RULE_TABLE_NAME, METER_POLICY_V6, METER_RULE_2_NUM, METER_RULE_2_CONFIG)
        oids = dash_db.wait_for_asic_db_keys(ASIC_METER_RULE_TABLE, min_keys=ENTRIES)
        for oid in oids:
            if oid != rule_v4_oid:
                rule_v6_oid = oid
                break
        rule_attrs = dash_db.get_asic_db_entry(ASIC_METER_RULE_TABLE, rule_v6_oid)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_METER_CLASS", rule_attrs, METER_RULE_2_METERING_CLASS)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_METER_POLICY_ID", rule_attrs, policy_v6_oid)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_DIP", rule_attrs, METER_RULE_2_IP)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_DIP_MASK", rule_attrs, METER_RULE_2_IP_MASK)

    def post_eni_counter_test(self, meta_data):
        counters_keys = self.counters_db.db_connection.hgetall(meta_data['name_map'])
        self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'], 'disable')

        for counter_entry in counters_keys.items():
            self.wait_for_id_list_remove(meta_data['group_name'], counter_entry[0], counter_entry[1])
        self.wait_for_table_empty(meta_data['name_map'])

    def post_meter_counter_test(self, meta_data):
        counters_keys = self.counters_db.db_connection.hgetall(meta_data['name_map'])
        self.set_flex_counter_group_status(meta_data['key'], meta_data['name_map'], 'disable')

        for counter_entry in counters_keys.items():
            self.wait_for_id_list_remove(meta_data['group_name'], counter_entry[0], counter_entry[1])
        self.wait_for_table_empty(meta_data['name_map'])

    def test_eni(self, dash_db: DashDB):
        dash_db.set_app_db_entry(APP_DASH_APPLIANCE_TABLE_NAME, APPLIANCE_ID, APPLIANCE_CONFIG)
        dash_db.set_app_db_entry(APP_DASH_VNET_TABLE_NAME, VNET1, VNET_CONFIG)
        self.mac_string = "F4939FEFC47E"
        self.mac_address = "F4:93:9F:EF:C4:7E"
        pb = Eni()
        pb.eni_id = "497f23d7-f0ac-4c99-a98f-59b470e8c7bd"
        pb.mac_address = bytes.fromhex(self.mac_address.replace(":", ""))
        pb.underlay_ip.ipv4 = socket.htonl(int(ipaddress.ip_address(UNDERLAY_IP)))
        pb.admin_state = State.STATE_ENABLED
        pb.vnet = VNET1
        pb.v4_meter_policy_id = METER_POLICY_V4
        pb.v6_meter_policy_id = METER_POLICY_V6
        dash_db.create_eni(self.mac_string, {"pb": pb.SerializeToString()})

        eni_oid = dash_db.wait_for_asic_db_keys(ASIC_ENI_TABLE)[0]
        attrs = dash_db.get_asic_db_entry(ASIC_ENI_TABLE, eni_oid)
        assert_sai_attribute_exists("SAI_ENI_ATTR_V4_METER_POLICY_ID", attrs, policy_v4_oid);
        assert_sai_attribute_exists("SAI_ENI_ATTR_V6_METER_POLICY_ID", attrs, policy_v6_oid);

        time.sleep(1)
        self.verify_flex_counter_flow(dash_db.dvs, eni_counter_group_meta)
        self.setup_dbs(dash_db.dvs)
        self.set_flex_counter_group_status(eni_counter_group_meta['key'], eni_counter_group_meta['name_map'], 'enable')
        time.sleep(1)
        self.verify_flex_counter_flow(dash_db.dvs, meter_counter_group_meta)

    def test_remove(self, dash_db: DashDB):
        self.meter_policy_id = METER_POLICY_V4
        self.meter_rule_num = METER_RULE_1_NUM
        self.mac_string = "F4939FEFC47E"
        policy_found = False
        rule_found = False

        ### verify meter policy cannot be removed with ENI bound to policy
        dash_db.remove_app_db_entry(APP_DASH_METER_RULE_TABLE_NAME, self.meter_policy_id, self.meter_rule_num)
        dash_db.remove_app_db_entry(APP_DASH_METER_POLICY_TABLE_NAME, self.meter_policy_id)
        time.sleep(20)
        meter_rule_oids = dash_db.wait_for_asic_db_keys(ASIC_METER_RULE_TABLE, min_keys=ENTRIES)
        meter_policy_oids = dash_db.wait_for_asic_db_keys(ASIC_METER_POLICY_TABLE, min_keys=ENTRIES)
        for oid in meter_policy_oids:
            if oid == policy_v4_oid:
                policy_found = True
                break
        for oid in meter_rule_oids:
            if oid == rule_v4_oid:
                rule_found = True
                break
        assert(policy_found)

        ### remove ENI to allow meter rule/policy delete.
        dash_db.remove_eni(self.mac_string)
        dash_db.remove_app_db_entry(APP_DASH_VNET_TABLE_NAME, VNET1)
        dash_db.remove_app_db_entry(APP_DASH_APPLIANCE_TABLE_NAME, APPLIANCE_ID)

        dash_db.remove_app_db_entry(APP_DASH_METER_RULE_TABLE_NAME, METER_POLICY_V4, METER_RULE_1_NUM)
        dash_db.remove_app_db_entry(APP_DASH_METER_POLICY_TABLE_NAME, METER_POLICY_V4)
        dash_db.wait_for_asic_db_key_del(ASIC_METER_RULE_TABLE, rule_v4_oid)
        dash_db.wait_for_asic_db_key_del(ASIC_METER_POLICY_TABLE, policy_v4_oid)
        meter_policy_oids = dash_db.wait_for_asic_db_keys(ASIC_METER_POLICY_TABLE)
        meter_rule_oids = dash_db.wait_for_asic_db_keys(ASIC_METER_RULE_TABLE)
        assert meter_policy_oids[0] == policy_v6_oid
        assert meter_rule_oids[0] == rule_v6_oid
        dash_db.remove_app_db_entry(APP_DASH_METER_RULE_TABLE_NAME, METER_POLICY_V6, METER_RULE_2_NUM)
        dash_db.remove_app_db_entry(APP_DASH_METER_POLICY_TABLE_NAME, METER_POLICY_V6)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down
# before retrying
def test_nonflaky_dummy():
    pass
