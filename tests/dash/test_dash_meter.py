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

from dash_db import *
from dash_configs import *

import time
import uuid
import ipaddress
import socket

from dvslib.sai_utils import assert_sai_attribute_exists
from dvslib.dvs_common import PollingConfig, wait_for_result

from swsscommon.swsscommon import (
    #APP_DASH_METER_POLICY_TABLE_NAME,
    #APP_DASH_METER_RULE_TABLE_NAME,
    APP_DASH_ENI_TABLE_NAME,
)

DVS_ENV = ["HWSKU=DPU-2P"]
NUM_PORTS = 2

ENTRIES = 2
policy_v4_oid = 0
policy_v6_oid = 0
rule_v4_oid = 0
rule_v6_oid = 0

class TestDash(TestFlexCountersBase):
    def test_appliance(self, dash_db: DashDB):
        self.appliance_id = APPLIANCE_ID
        self.sip = SIP
        self.vm_vni = VM_VNI
        self.local_region_id = "10"
        pb = Appliance()
        pb.sip.ipv4 = socket.htonl(int(ipaddress.ip_address(self.sip)))
        pb.vm_vni = int(self.vm_vni)
        dash_db.create_appliance(self.appliance_id, {"pb": pb.SerializeToString()})

        direction_keys = dash_db.wait_for_asic_db_keys(ASIC_DIRECTION_LOOKUP_TABLE)
        dl_attrs = dash_db.get_asic_db_entry(ASIC_DIRECTION_LOOKUP_TABLE, direction_keys[0])
        assert_sai_attribute_exists("SAI_DIRECTION_LOOKUP_ENTRY_ATTR_ACTION", dl_attrs, "SAI_DIRECTION_LOOKUP_ENTRY_ACTION_SET_OUTBOUND_DIRECTION")

        vip_keys = dash_db.wait_for_asic_db_keys(ASIC_VIP_TABLE)
        vip_attrs = dash_db.get_asic_db_entry(ASIC_VIP_TABLE, vip_keys[0])
        assert_sai_attribute_exists("SAI_VIP_ENTRY_ATTR_ACTION", vip_attrs, "SAI_VIP_ENTRY_ACTION_ACCEPT")

    def test_vnet(self, dash_db: DashDB):
        self.vnet = "Vnet1"
        self.vni = "45654"
        self.guid = "559c6ce8-26ab-4193-b946-ccc6e8f930b2"
        pb = Vnet()
        pb.vni = int(self.vni)
        pb.guid.value = bytes.fromhex(uuid.UUID(self.guid).hex)
        dash_db.create_vnet(self.vnet, {"pb": pb.SerializeToString()})

        vnet_keys = dash_db.wait_for_asic_db_keys(ASIC_VNET_TABLE)
        self.vnet_oid = vnet_keys[0]
        vnet_attr = dash_db.get_asic_db_entry(ASIC_VNET_TABLE, self.vnet_oid)
        assert_sai_attribute_exists("SAI_VNET_ATTR_VNI", vnet_attr, self.vni)

    def test_v4_meter(self, dash_db: DashDB):
        global policy_v4_oid
        global rule_v4_oid
        self.meter_policy_id = METER_POLICY_V4
        pb = MeterPolicy()
        pb.ip_version = IpVersion.IP_VERSION_IPV4
        dash_db.create_meter_policy(self.meter_policy_id, {"pb": pb.SerializeToString()})

        policy_v4_oid = dash_db.wait_for_asic_db_keys(ASIC_METER_POLICY_TABLE)[0]
        policy_attrs = dash_db.get_asic_db_entry(ASIC_METER_POLICY_TABLE, policy_v4_oid)
        assert_sai_attribute_exists("SAI_METER_POLICY_ATTR_IP_ADDR_FAMILY", policy_attrs, "SAI_IP_ADDR_FAMILY_IPV4")

        self.meter_rule_num_v4 = METER_RULE_1_NUM
        dash_db.set_app_db_entry("DASH_METER_RULE_TABLE", METER_POLICY_V4, METER_RULE_1_NUM, METER_RULE_1_CONFIG)

        rule_v4_oid = dash_db.wait_for_asic_db_keys(ASIC_METER_RULE_TABLE, min_keys=1)[0]
        rule_attrs = dash_db.get_asic_db_entry(ASIC_METER_RULE_TABLE, rule_v4_oid)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_PRIORITY", rule_attrs, METER_RULE_1_PRIORITY)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_METER_CLASS", rule_attrs, METER_RULE_1_METERING_CLASS)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_METER_POLICY_ID", rule_attrs, policy_v4_oid)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_DIP", rule_attrs, METER_RULE_1_IP)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_DIP_MASK", rule_attrs, METER_RULE_1_IP_MASK)

    def test_v6_meter(self, dash_db: DashDB):
        global policy_v6_oid
        global rule_v6_oid
        self.meter_policy_id = METER_POLICY_V6
        pb = MeterPolicy()
        pb.ip_version = IpVersion.IP_VERSION_IPV6
        dash_db.create_meter_policy(self.meter_policy_id, {"pb": pb.SerializeToString()})

        oids = dash_db.wait_for_asic_db_keys(ASIC_METER_POLICY_TABLE, min_keys=2)
        for oid in oids:
            if oid != policy_v4_oid:
                policy_v6_oid = oid
                break
        policy_attrs = dash_db.get_asic_db_entry(ASIC_METER_POLICY_TABLE, policy_v6_oid)

        assert_sai_attribute_exists("SAI_METER_POLICY_ATTR_IP_ADDR_FAMILY", policy_attrs, "SAI_IP_ADDR_FAMILY_IPV6")

        self.meter_rule_num_v6 = METER_RULE_2_NUM
        dash_db.set_app_db_entry("DASH_METER_RULE_TABLE", METER_POLICY_V6, self.meter_rule_num_v6, METER_RULE_2_CONFIG)
        time.sleep(10)

        oids = dash_db.wait_for_asic_db_keys(ASIC_METER_RULE_TABLE, min_keys=2)
        for oid in oids:
            if oid != rule_v4_oid:
                rule_v6_oid = oid
                break
        rule_attrs = dash_db.get_asic_db_entry(ASIC_METER_RULE_TABLE, rule_v6_oid)

        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_PRIORITY", rule_attrs, METER_RULE_2_PRIORITY)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_METER_CLASS", rule_attrs, METER_RULE_2_METERING_CLASS)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_METER_POLICY_ID", rule_attrs, policy_v6_oid)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_DIP", rule_attrs, METER_RULE_2_IP)
        assert_sai_attribute_exists("SAI_METER_RULE_ATTR_DIP_MASK", rule_attrs, METER_RULE_2_IP_MASK)

    def test_eni(self, dash_db: DashDB):
        self.vnet = VNET1
        self.mac_string = "F4939FEFC47E"
        self.mac_address = "F4:93:9F:EF:C4:7E"
        self.eni_id = "497f23d7-f0ac-4c99-a98f-59b470e8c7bd"
        self.underlay_ip = UNDERLAY_IP
        self.admin_state = "enabled"
        pb = Eni()
        pb.eni_id = self.eni_id
        pb.mac_address = bytes.fromhex(self.mac_address.replace(":", ""))
        pb.underlay_ip.ipv4 = socket.htonl(int(ipaddress.ip_address(self.underlay_ip)))
        pb.admin_state = State.STATE_ENABLED
        pb.vnet = self.vnet
        pb.v4_meter_policy_id = METER_POLICY_V4
        pb.v6_meter_policy_id = METER_POLICY_V6
        dash_db.create_eni(self.mac_string, {"pb": pb.SerializeToString()})
        
        enis = dash_db.wait_for_asic_db_keys(ASIC_ENI_TABLE)
        self.eni_oid = enis[0]
        attrs = dash_db.get_asic_db_entry(ASIC_ENI_TABLE, self.eni_oid)

        assert_sai_attribute_exists("SAI_ENI_ATTR_V4_METER_POLICY_ID", attrs, policy_v4_oid);
        assert_sai_attribute_exists("SAI_ENI_ATTR_V6_METER_POLICY_ID", attrs, policy_v6_oid);

    def test_unsupported(self, dash_db: DashDB):
        self.meter_policy_id = METER_POLICY_V4
        self.meter_rule_num = METER_RULE_1_NUM
        self.mac_string = "F4939FEFC47E"

	### verify meter rule/policy cannot be removed with ENI bound
        dash_db.remove_meter_rule(self.meter_policy_id, self.meter_rule_num)
        dash_db.remove_meter_policy(self.meter_policy_id)
        time.sleep(30)
        meter_rule_entries = dash_db.wait_for_asic_db_keys(ASIC_METER_RULE_TABLE, min_keys=ENTRIES)
        meter_policy_entries = dash_db.wait_for_asic_db_keys(ASIC_METER_POLICY_TABLE, min_keys=ENTRIES)
        assert meter_policy_entries[0] == policy_v4_oid
        assert meter_rule_entries[0] == rule_v4_oid

        ### Remove ENI to allow meter rule/policy delete.
        dash_db.remove_eni(self.mac_string)
        time.sleep(30)

        dash_db.remove_meter_rule(self.meter_policy_id, self.meter_rule_num)
        dash_db.remove_meter_policy(self.meter_policy_id)
        time.sleep(30)
        meter_rule_entries = dash_db.wait_for_asic_db_keys(ASIC_METER_RULE_TABLE, min_keys=ENTRIES-1)
        meter_policy_entries = dash_db.wait_for_asic_db_keys(ASIC_METER_POLICY_TABLE, min_keys=ENTRIES-1)
        assert meter_policy_entries[0] == policy_v6_oid
        assert meter_rule_entries[0] == rule_v6_oid

	# bind to not created policy..TODO

    def test_cleanup(self, dash_db: DashDB):
        self.vnet = VNET1
        self.mac_string = "F4939FEFC47E"
        self.vni = "3251"
        self.appliance_id = APPLIANCE_ID
        dash_db.remove_eni(self.mac_string)
        dash_db.remove_meter_rule(METER_POLICY_V4, METER_RULE_1_NUM)
        dash_db.remove_meter_rule(METER_POLICY_V6, METER_RULE_2_NUM)
        dash_db.remove_meter_policy(METER_POLICY_V4)
        dash_db.remove_meter_policy(METER_POLICY_V6)
        dash_db.remove_vnet(self.vnet)
        dash_db.remove_appliance(self.appliance_id)
        time.sleep(1)

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down
# before retrying
def test_nonflaky_dummy():
    pass
