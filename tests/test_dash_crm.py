import os
import re
import time
import json
import pytest

from swsscommon import swsscommon


DVS_ENV = ["HWSKU=Nvidia-MBF2H536C"]
NUM_PORTS = 2


def crm_update(dvs, field, value):
    cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    tbl = swsscommon.Table(cfg_db, "CRM")
    fvs = swsscommon.FieldValuePairs([(field, value)])
    tbl.set("Config", fvs)


@pytest.fixture(scope="module")
def dpu_only(dvs):
    config_db = dvs.get_config_db()
    metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")
    if metatbl.get("switch_type") != "dpu":
        pytest.skip("The test can be run only on the DPU")


def to_string(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


class DashTable:

    def __init__(self, dvs, name) -> None:
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        self.table = swsscommon.ProducerStateTable(app_db, name)
        self.keys = set()

    def add(self, key, value) -> None:
        key = to_string(key)
        assert key not in self.keys

        fvs = swsscommon.FieldValuePairs([(to_string(k), to_string(v)) for k, v in value.items()])
        self.table.set(key, fvs)
        self.keys.add(key)

    def remove(self, key) -> None:
        key = str(key)
        assert key in self.keys
        self.table.delete(str(key))
        self.keys.remove(key)

    def remove_all(self):
        for key in list(self.keys):
            self.remove(key)


class Resource:

    counters = None

    def __init__(self, dvs, low_th=0, high_th=2) -> None:
        self.dvs = dvs
        self.low_th = low_th
        self.high_th = high_th
        self.polling_interval = 1
        self.marker = self.dvs.add_log_marker()

    def set_tresholds(self) -> None:
        for counter in self.counters:
            crm_update(self.dvs, "polling_interval", str(self.polling_interval))
            crm_update(self.dvs, f"dash_{counter}_threshold_type", "used")
            crm_update(self.dvs, f"dash_{counter}_low_threshold", str(self.low_th))
            crm_update(self.dvs, f"dash_{counter}_high_threshold", str(self.high_th))

    def check_used_counters(self, used):
        for counter, value in self.counters.items():
            entry_used_counter = self.dvs.getCrmCounterValue('STATS', f'crm_stats_dash_{counter}_used')
            assert entry_used_counter == used, f"crm_stats_dash_{counter}_used is not equal to expected {used} value"

    def check_threshold_exceeded_message(self):
        for counter, value in self.counters.items():
            self.check_syslog(f"{value['th_name']} THRESHOLD_EXCEEDED for TH_USED", 1)
            self.check_syslog(f"{value['th_name']} THRESHOLD_CLEAR for TH_USED", 0)

    def check_threshold_cleared_message(self):
        for counter, value in self.counters.items():
            self.check_syslog(f"{value['th_name']} THRESHOLD_CLEAR for TH_USED", 1)

    def check_treshold_exceeded(self) -> None:
        # Wait for CrmOrch to update counters
        time.sleep(self.polling_interval + 1)

        self.check_used_counters(self.high_th)
        self.check_threshold_exceeded_message()

    def check_treshold_cleared(self) -> None:
        # Wait for CrmOrch to update counters
        time.sleep(self.polling_interval + 1)

        self.check_used_counters(self.low_th)
        self.check_threshold_cleared_message()

    def configure(self):
        raise NotImplementedError()

    def clear(self):
        raise NotImplementedError()

    def check_syslog(self, err_log, expected_cnt):
        (exitcode, num) = self.dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep \"%s\" | wc -l" % (self.marker, err_log)])
        assert int(num.strip()) >= expected_cnt, f"Expexted message is not found: '{err_log}'"


class Vnet(Resource):

    counters = {
        'vnet': {'th_name': 'VNET'}
    }

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)

        self.appliance_table = DashTable(self.dvs, swsscommon.APP_DASH_APPLIANCE_TABLE_NAME)
        self.vnet_table = DashTable(self.dvs, swsscommon.APP_DASH_VNET_TABLE_NAME)

    def configure(self):
        self.appliance_table.add("123", {'sip': '10.1.0.32', 'vm_vni': '123'})

        for i in range(1, self.high_th + 1):
            self.vnet_table.add(f'vnet{i}', {'vni': i, 'guid': i})

    def clear(self):
        self.vnet_table.remove_all()
        self.appliance_table.remove_all()


class Eni(Resource):

    counters = {
        'eni': {'th_name': 'ENI'},
        'eni_ether_address_map': {'th_name': 'ENI_ETHER_ADDRESS_MAP'}
    }

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)

        self.appliance_table = DashTable(self.dvs, swsscommon.APP_DASH_APPLIANCE_TABLE_NAME)
        self.vnet_table = DashTable(self.dvs, swsscommon.APP_DASH_VNET_TABLE_NAME)
        self.eni_table = DashTable(self.dvs, swsscommon.APP_DASH_ENI_TABLE_NAME)

    def configure(self):
        self.appliance_table.add('123', {'sip': '10.1.0.32', 'vm_vni': 123})
        self.vnet_table.add('vnet1', {'vni': 1, 'guid': 1})

        for i in range(1, self.high_th + 1):
            self.eni_table.add(f'eni{i}', {"eni_id":f"eni{i}",
                                "mac_address":f"00:00:00:00:00:{i:02x}",
                                "underlay_ip":"10.0.1.1",
                                "admin_state":"enabled",
                                "vnet":"vnet1",
                                "qos":"qos100"})

    def clear(self):
        self.eni_table.remove_all()
        self.vnet_table.remove_all()
        self.appliance_table.remove_all()


class VnetMapping(Resource):

    addr_family = None

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)

        self.appliance_table = DashTable(self.dvs, swsscommon.APP_DASH_APPLIANCE_TABLE_NAME)
        self.vnet_table = DashTable(self.dvs, swsscommon.APP_DASH_VNET_TABLE_NAME)
        self.eni_table = DashTable(self.dvs, swsscommon.APP_DASH_ENI_TABLE_NAME)
        self.route_table = DashTable(self.dvs, swsscommon.APP_DASH_ROUTE_TABLE_NAME)
        self.vnet_mapping_table = DashTable(self.dvs, swsscommon.APP_DASH_VNET_MAPPING_TABLE_NAME)

        self.counters = {
            f'{self.addr_family}_outbound_ca_to_pa': {'th_name': 'OUTBOUND_CA_TO_PA'},
            f'{self.addr_family}_pa_validation': {'th_name': 'PA_VALIDATION'},
        }

    def configure(self):
        self.appliance_table.add('123', {'sip': '10.1.0.32', 'vm_vni': 123})
        self.vnet_table.add('vnet1', {'vni': 1, 'guid': 1})

        src_pa_ip = "10.0.1.1"
        if self.addr_family == 'ipv6':
            src_pa_ip = "2001::1011"

        self.eni_table.add(f'eni1', {
            "eni_id":f"eni1",
            "mac_address":f"00:00:00:00:00:01",
            "underlay_ip": src_pa_ip,
            "admin_state":"enabled",
            "vnet":"vnet1",
            "qos":"qos100"
            })

        for i in range(1, self.high_th + 1):
            dst_ca_ip = f'20.2.{i}.1'
            dst_pa_ip = f"10.0.{i}.2"
            if self.addr_family == 'ipv6':
                dst_ca_ip = f'2001::{i}:1011'
                dst_pa_ip = f'2002::{i}:1011'
            self.vnet_mapping_table.add(f'vnet1:{dst_ca_ip}', {
                "routing_type":"vnet_encap",
                "underlay_ip":f"{dst_pa_ip}",
                "mac_address":"F9:22:83:99:22:A2",
                "use_dst_vni":"true"
            })

    def clear(self):
        self.vnet_mapping_table.remove_all()
        self.eni_table.remove_all()
        self.vnet_table.remove_all()
        self.appliance_table.remove_all()


class Ipv4VnetMapping(VnetMapping):

    addr_family = 'ipv4'


class Ipv6VnetMapping(VnetMapping):

    addr_family = 'ipv6'


class OutboundRouting(Resource):

    addr_family = None

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)

        self.appliance_table = DashTable(self.dvs, swsscommon.APP_DASH_APPLIANCE_TABLE_NAME)
        self.vnet_table = DashTable(self.dvs, swsscommon.APP_DASH_VNET_TABLE_NAME)
        self.eni_table = DashTable(self.dvs, swsscommon.APP_DASH_ENI_TABLE_NAME)
        self.route_table = DashTable(self.dvs, swsscommon.APP_DASH_ROUTE_TABLE_NAME)

        self.counters = {
            f'{self.addr_family}_outbound_routing': {'th_name': 'OUTBOUND_ROUTING'},
        }

    def configure(self):
        self.appliance_table.add('123', {'sip': '10.1.0.32', 'vm_vni': 123})
        self.vnet_table.add('vnet1', {'vni': 1, 'guid': 1})

        src_pa_ip = "10.0.1.1"
        if self.addr_family == 'ipv6':
            src_pa_ip = "2001::1011"

        self.eni_table.add(f'eni1', {
            "eni_id":f"eni1",
            "mac_address":f"00:00:00:00:00:01",
            "underlay_ip": src_pa_ip,
            "admin_state":"enabled",
            "vnet":"vnet1",
            "qos":"qos100"
            })

        for i in range(1, self.high_th + 1):
            prefix = f"20.2.{i}.0/24"
            if self.addr_family == 'ipv6':
                prefix = f'2002::{i}:1011/126'
            self.route_table.add(f"eni1:{prefix}", {"action_type":"vnet", "vnet":"vnet1"})

    def clear(self):
        self.route_table.remove_all()
        self.eni_table.remove_all()
        self.vnet_table.remove_all()
        self.appliance_table.remove_all()


class Ipv4OutboundRouting(OutboundRouting):

    addr_family = 'ipv4'


class Ipv6OutboundRouting(OutboundRouting):

    addr_family = 'ipv6'


class InboundRouting(Resource):

    addr_family = None

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)

        self.appliance_table = DashTable(self.dvs, swsscommon.APP_DASH_APPLIANCE_TABLE_NAME)
        self.vnet_table = DashTable(self.dvs, swsscommon.APP_DASH_VNET_TABLE_NAME)
        self.eni_table = DashTable(self.dvs, swsscommon.APP_DASH_ENI_TABLE_NAME)
        self.route_rule_table = DashTable(self.dvs, swsscommon.APP_DASH_ROUTE_RULE_TABLE_NAME)

        self.counters = {
            f'{self.addr_family}_inbound_routing': {'th_name': 'INBOUND_ROUTING'},
        }

    def configure(self):
        self.appliance_table.add('123', {'sip': '10.1.0.32', 'vm_vni': 123})
        self.vnet_table.add('vnet1', {'vni': 1, 'guid': 1})

        src_pa_ip = "10.0.1.1"
        if self.addr_family == 'ipv6':
            src_pa_ip = "2001::1011"

        self.eni_table.add(f'eni1', {
            "eni_id":f"eni1",
            "mac_address":f"00:00:00:00:00:01",
            "underlay_ip": src_pa_ip,
            "admin_state":"enabled",
            "vnet":"vnet1",
            "qos":"qos100"
            })

        for i in range(1, self.high_th + 1):
            dst_pa_prefix = f"11.2.{i}.0/24"
            if self.addr_family == 'ipv6':
                dst_pa_prefix = f'2003::{i}:1011/126'

            self.route_rule_table.add(f"eni1:1:{dst_pa_prefix}", {
                "action_type":"decap",
                "priority":"1",
                "pa_validation":"true",
                "vnet":"vnet1"
            })

    def clear(self):
        self.route_rule_table.remove_all()
        self.eni_table.remove_all()
        self.vnet_table.remove_all()
        self.appliance_table.remove_all()


class Ipv4InboundRouting(InboundRouting):

    addr_family = 'ipv4'


class Ipv6InboundRouting(InboundRouting):

    addr_family = 'ipv6'


class AclGroup(Resource):

    addr_family = None

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)

        self.acl_group_table = DashTable(self.dvs, swsscommon.APP_DASH_ACL_GROUP_TABLE_NAME)

        self.counters = {
            f'{self.addr_family}_acl_group': {'th_name': 'ACL_GROUP'},
        }

    def configure(self):
        for i in range(1, self.high_th + 1):
            self.acl_group_table.add(f"group{i}", {"ip_version": self.addr_family, "guid": f"dash-group-{i}"})

    def clear(self):
        self.acl_group_table.remove_all()


class Ipv4AclGroup(AclGroup):

    addr_family = 'ipv4'


class Ipv6AclGroup(AclGroup):

    addr_family = 'ipv6'


class AclRule(Resource):

    addr_family = None

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)

        self.acl_group_table = DashTable(self.dvs, swsscommon.APP_DASH_ACL_GROUP_TABLE_NAME)
        self.acl_rule_table = DashTable(self.dvs, swsscommon.APP_DASH_ACL_RULE_TABLE_NAME)

        self.counters = {
            f'{self.addr_family}_acl_rule': {'th_name': 'ACL_RULE'},
        }

        self.pubsub = self.dvs.SubscribeAsicDbObject("SAI_OBJECT_TYPE_DASH_ACL_GROUP")
        self.acl_group_id = None

    def get_acl_group_counters(self, oid, counter):
        return self.dvs.getCrmCounterValue(f"DASH_ACL_GROUP_STATS:{oid}", counter)

    def check_used_counters(self, used):
        for counter, value in self.counters.items():
            entry_used_counter = self.get_acl_group_counters(self.acl_group_id, f'crm_stats_dash_{counter}_used')
            if used:
                assert entry_used_counter == used, f"crm_stats_dash_{counter}_used is not equal to expected {used} value"
            else:
                # Verify that counter is removed from the DB
                assert entry_used_counter == None, f"crm_stats_dash_{counter}_used is not removed from DB"

    def configure(self):
        self.acl_group_table.add(f"group1", {"ip_version": self.addr_family, "guid": f"dash-group-1"})

        (added, deleted) = self.dvs.GetSubscribedAsicDbObjects(self.pubsub)
        assert len(added) == 1
        assert len(deleted) == 0

        oid = added[0]['key'].replace("oid:", "")
        self.acl_group_id = (oid)

        for i in range(1, self.high_th + 1):
            self.acl_rule_table.add(f"group1:rule{i}", {
                "priority": i,
                "action": "allow",
                "terminating": "true",
                "src_addr": f"{i}.0.0.0/0",
                "dst_addr": f"{i}.0.0.0/0",
                "src_port": "0-65535",
                "dst_port": "0-65535"
                })

    def clear(self):
        self.acl_rule_table.remove_all()
        # Wait for the counters update by CrmOrch before removing the ACL group.
        # If the ACL group will be removed immediately after removing its rules the CrmOrch
        # won't send a "threshold cleared" message to Syslog.
        time.sleep(self.polling_interval + 1)

        # Verify that used counter is 0 after removal of all ACL rules
        for counter, value in self.counters.items():
            entry_used_counter = self.get_acl_group_counters(self.acl_group_id, f'crm_stats_dash_{counter}_used')
            assert entry_used_counter == 0

        self.acl_group_table.remove_all()


class Ipv4AclRule(AclRule):

    addr_family = 'ipv4'


class Ipv6AclRule(AclRule):

    addr_family = 'ipv6'


class TestCrmDash:

    @pytest.mark.parametrize('dash_entry', [
        Vnet,
        Eni,
        Ipv4VnetMapping,
        Ipv6VnetMapping,
        Ipv4OutboundRouting,
        Ipv6OutboundRouting,
        Ipv4InboundRouting,
        Ipv6InboundRouting,
        Ipv4AclGroup,
        Ipv6AclGroup,
        Ipv4AclRule,
        Ipv6AclRule
    ])
    def test_crm_dash_entry(self, dash_entry, dvs, testlog, dpu_only):
        entry = dash_entry(dvs)
        entry.set_tresholds()
        entry.configure()
        entry.check_treshold_exceeded()
        entry.clear()
        entry.check_treshold_cleared()
