from swsscommon import swsscommon
from dvslib.dvs_database import DVSDatabase
from typing import Union
import time

import pytest

DVS_ENV = ["HWSKU=Nvidia-MBF2H536C"]

ACL_GROUP_1 = "acl_group_1"
ACL_GROUP_2 = "acl_group_2"
ACL_RULE_1 = "1"
ACL_RULE_2 = "2"
ACL_RULE_3 = "3"
ACL_STAGE_1 = "1"
ACL_STAGE_2 = "2"

SAI_NULL_OID = "oid:0x0"

def to_string(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def get_sai_stage(outbound, v4, stage_num):
    direction = "OUTBOUND" if outbound else "INBOUND"
    ip_version = "V4" if v4 else "V6"
    return "SAI_ENI_ATTR_{}_{}_STAGE{}_DASH_ACL_GROUP_ID".format(direction, ip_version, stage_num)


class ProduceStateTable(object):
    def __init__(self, database, table_name: str):
        self.table = swsscommon.ProducerStateTable(
            database.db_connection,
            table_name)
        self.keys = set()

    def __setitem__(self, key: str, pairs: Union[dict, list, tuple]):
        pairs_str = []
        if isinstance(pairs, dict):
            pairs = pairs.items()
        for k, v in pairs:
            pairs_str.append((to_string(k), to_string(v)))
        self.table.set(key, pairs_str)
        self.keys.add(key)

    def __delitem__(self, key: str):
        self.table.delete(str(key))
        self.keys.discard(key)

    def get_keys(self):
        return self.keys


class Table(object):
    def __init__(self, database: DVSDatabase, table_name: str):
        self.table_name = table_name
        self.db = database
        self.table = swsscommon.Table(database.db_connection, self.table_name)

        # Overload verification methods in DVSDatabase so we can use them per-table
        # All methods from DVSDatabase that do not start with '_' are overloaded
        # See the DVSDatabase class for info about the use of each method
        # For each `func` in DVSDatabase, equivalent to:
        #     def func(self, **kwargs):
        #         return self.db.func(table_name=self.table_name, **kwargs)
        # This means that we can call e.g.
        #     table_object.wait_for_n_keys(num_keys=1)
        # instead of
        #     dvs.get_asic_db().wait_for_n_keys(table_name="ASIC_STATE:SAI_EXAMPLE_TABLE", num_keys=1)
        overload_methods = [
            attr for attr in dir(DVSDatabase)
                if not attr.startswith('_') and callable(getattr(DVSDatabase, attr))
        ]
        for method_name in overload_methods:
            setattr(
                self, method_name, lambda method_name=method_name,
                **kwargs: getattr(self.db, method_name)(table_name=self.table_name, **kwargs)
            )

    def __getitem__(self, key: str):
        exists, result = self.table.get(str(key))
        if not exists:
            return None
        else:
            return dict(result)


APPL_DB_TABLE_LIST = [
    swsscommon.APP_DASH_ACL_IN_TABLE_NAME,
    swsscommon.APP_DASH_ACL_OUT_TABLE_NAME,
    swsscommon.APP_DASH_ACL_GROUP_TABLE_NAME,
    swsscommon.APP_DASH_ACL_RULE_TABLE_NAME,
    swsscommon.APP_DASH_ENI_TABLE_NAME,
    swsscommon.APP_DASH_VNET_TABLE_NAME,
    swsscommon.APP_DASH_APPLIANCE_TABLE_NAME
]


# TODO: At some point, orchagent will be update to write to some DB to indicate that it's finished
#       processing updates for a given table. Once this is implemented, we can remove all the `sleep`
#       statements in these tests and instead proactively check for the finished signal from orchagent
class DashAcl(object):
    def __init__(self, dvs):
        self.dvs = dvs
        self.app_db_tables = []

        for table in APPL_DB_TABLE_LIST:
            pst = ProduceStateTable(
                self.dvs.get_app_db(), table
            )
            table_variable_name = "app_{}".format(table.lower())
            # Based on swsscommon convention for table names, assume 
            # e.g. swsscommon.APP_DASH_ENI_TABLE_NAME == "DASH_ENI_TABLE", therefore
            # the ProducerStateTable object for swsscommon.APP_DASH_ENI_TABLE_NAME
            # will be accessible as `self.app_dash_eni_table`
            setattr(self, table_variable_name, pst)
            self.app_db_tables.append(pst)

        self.asic_dash_acl_rule_table = Table(
            self.dvs.get_asic_db(), "ASIC_STATE:SAI_OBJECT_TYPE_DASH_ACL_RULE")
        self.asic_dash_acl_group_table = Table(
            self.dvs.get_asic_db(), "ASIC_STATE:SAI_OBJECT_TYPE_DASH_ACL_GROUP")
        self.asic_eni_table = Table(
            self.dvs.get_asic_db(), "ASIC_STATE:SAI_OBJECT_TYPE_ENI")
        self.asic_vip_table = Table(
            self.dvs.get_asic_db(), "ASIC_STATE:SAI_OBJECT_TYPE_VIP_ENTRY")
        self.asic_vnet_table = Table(
            self.dvs.get_asic_db(), "ASIC_STATE:SAI_OBJECT_TYPE_VNET")

        self.asic_db_tables = [
            self.asic_dash_acl_group_table,
            self.asic_dash_acl_rule_table,
            self.asic_eni_table,
            self.asic_vip_table,
            self.asic_vnet_table
        ]

    def create_acl_rule(self, group_id, rule_id, attr_maps: dict):
        self.app_dash_acl_rule_table[str(
            group_id) + ":" + str(rule_id)] = attr_maps

    def remove_acl_rule(self, group_id, rule_id):
        del self.app_dash_acl_rule_table[str(group_id) + ":" + str(rule_id)]

    def create_acl_group(self, group_id, attr_maps: dict):
        self.app_dash_acl_group_table[str(group_id)] = attr_maps

    def remove_acl_group(self, group_id):
        del self.app_dash_acl_group_table[str(group_id)]

    def create_appliance(self, name, attr_maps: dict):
        self.app_dash_appliance_table[str(name)] = attr_maps

    def remove_appliance(self, name):
        del self.app_dash_appliance_table[str(name)]

    def create_eni(self, eni, attr_maps: dict):
        self.app_dash_eni_table[str(eni)] = attr_maps

    def remove_eni(self, eni):
        del self.app_dash_eni_table[str(eni)]

    def create_vnet(self, vnet, attr_maps: dict):
        self.app_dash_vnet_table[str(vnet)] = attr_maps

    def remove_vnet(self, vnet):
        del self.app_dash_vnet_table[str(vnet)]

    def bind_acl_in(self, eni, stage, group_id):
        self.app_dash_acl_in_table[str(
            eni) + ":" + str(stage)] = {"acl_group_id": str(group_id)}

    def unbind_acl_in(self, eni, stage):
        del self.app_dash_acl_in_table[str(eni) + ":" + str(stage)]

    def bind_acl_out(self, eni, stage, group_id):
        self.app_dash_acl_out_table[str(
            eni) + ":" + str(stage)] = {"acl_group_id": str(group_id)}

    def unbind_acl_out(self, eni, stage):
        del self.app_dash_acl_out_table[str(eni) + ":" + str(stage)]


class TestAcl(object):
    @pytest.fixture
    def ctx(self, dvs):
        self.vnet_name = "vnet1"
        self.eni_name = "eth0"
        self.appliance_name = "default_app"
        self.vm_vni = "4321"
        self.appliance_sip = "10.20.30.40"
        self.vni = "1"
        self.mac_address = "01:23:45:67:89:ab"
        self.underlay_ip = "1.1.1.1"

        acl_context = DashAcl(dvs)
        acl_context.create_appliance(self.appliance_name, {"sip": self.appliance_sip, "vm_vni": self.vm_vni})
        acl_context.create_vnet(self.vnet_name, {"vni": self.vni})
        acl_context.create_eni(self.eni_name, {"vnet": self.vnet_name,
                      "mac_address": self.mac_address, "underlay_ip": self.underlay_ip})

        acl_context.asic_vip_table.wait_for_n_keys(num_keys=1)
        acl_context.asic_vnet_table.wait_for_n_keys(num_keys=1)
        acl_context.asic_eni_table.wait_for_n_keys(num_keys=1)

        yield acl_context

        # Manually cleanup by deleting all remaining APPL_DB keys
        for table in acl_context.app_db_tables:
            keys = table.get_keys()
            for key in list(keys):
                del table[key]

        for table in acl_context.asic_db_tables:
            table.wait_for_n_keys(num_keys=0)

    def test_acl_flow(self, ctx):
        ctx.create_acl_group(ACL_GROUP_1, {"ip_version": "ipv4"})
        ctx.create_acl_rule(ACL_GROUP_1, ACL_RULE_1, {"priority": "1", "action": "allow", "terminating": "false",
                           "src_addr": "192.168.0.1/32,192.168.1.2/30", "dst_addr": "192.168.0.1/32,192.168.1.2/30", "src_port": "0-1", "dst_port": "0-1"})

        rule1_id= ctx.asic_dash_acl_rule_table.wait_for_n_keys(num_keys=1)[0]
        group1_id= ctx.asic_dash_acl_group_table.wait_for_n_keys(num_keys=1)[0]
        rule1_attr = ctx.asic_dash_acl_rule_table[rule1_id]
        assert rule1_attr["SAI_DASH_ACL_RULE_ATTR_PRIORITY"] == "1"
        assert rule1_attr["SAI_DASH_ACL_RULE_ATTR_ACTION"] == "SAI_DASH_ACL_RULE_ACTION_PERMIT_AND_CONTINUE"
        assert rule1_attr["SAI_DASH_ACL_RULE_ATTR_DASH_ACL_GROUP_ID"] == group1_id
        assert rule1_attr["SAI_DASH_ACL_RULE_ATTR_DIP"] == "2:192.168.0.1/32,192.168.1.2/30"
        assert rule1_attr["SAI_DASH_ACL_RULE_ATTR_SIP"] == "2:192.168.0.1/32,192.168.1.2/30"
        assert rule1_attr["SAI_DASH_ACL_RULE_ATTR_DST_PORT"] == "1:0,1"
        assert rule1_attr["SAI_DASH_ACL_RULE_ATTR_SRC_PORT"] == "1:0,1"
        assert rule1_attr["SAI_DASH_ACL_RULE_ATTR_PROTOCOL"].split(":")[0] == "256"
        group1_attr = ctx.asic_dash_acl_group_table[group1_id]
        assert group1_attr["SAI_DASH_ACL_GROUP_ATTR_IP_ADDR_FAMILY"] == "SAI_IP_ADDR_FAMILY_IPV4"

        # Create multiple rules
        ctx.create_acl_rule(ACL_GROUP_1, ACL_RULE_2, {"priority": "2", "action": "allow", "terminating": "false",
                           "src_addr": "192.168.0.1/32,192.168.1.2/30", "dst_addr": "192.168.0.1/32,192.168.1.2/30", "src_port": "0-1", "dst_port": "0-1"})
        ctx.create_acl_rule(ACL_GROUP_1, ACL_RULE_3, {"priority": "2", "action": "allow", "terminating": "false",
                           "src_addr": "192.168.0.1/32,192.168.1.2/30", "dst_addr": "192.168.0.1/32,192.168.1.2/30", "src_port": "0-1", "dst_port": "0-1"})
        ctx.asic_dash_acl_rule_table.wait_for_n_keys(num_keys=3)
        ctx.unbind_acl_in(self.eni_name, ACL_STAGE_1)
        ctx.remove_acl_rule(ACL_GROUP_1, ACL_RULE_1)
        ctx.remove_acl_rule(ACL_GROUP_1, ACL_RULE_2)
        ctx.remove_acl_rule(ACL_GROUP_1, ACL_RULE_3)
        ctx.remove_acl_group(ACL_GROUP_1)
        ctx.asic_dash_acl_rule_table.wait_for_n_keys(num_keys=0)
        ctx.asic_dash_acl_group_table.wait_for_n_keys(num_keys=0)

    def test_acl_group(self, ctx):
        ctx.create_acl_group(ACL_GROUP_1, {"ip_version": "ipv6"})
        ctx.create_acl_rule(ACL_GROUP_1, ACL_RULE_1, {"priority": "1", "action": "allow", "terminating": "false",
                           "src_addr": "192.168.0.1/32,192.168.1.2/30", "dst_addr": "192.168.0.1/32,192.168.1.2/30", "src_port": "0-1", "dst_port": "0-1"})
        ctx.asic_dash_acl_group_table.wait_for_n_keys(num_keys=1)

        # Remove group before removing its rule
        ctx.remove_acl_group(ACL_GROUP_1)
        # Wait a few seconds to make sure no changes are made
        # since group still contains a rule
        time.sleep(3)
        ctx.asic_dash_acl_group_table.wait_for_n_keys(num_keys=1)

        ctx.remove_acl_rule(ACL_GROUP_1, ACL_RULE_1)
        ctx.asic_dash_acl_group_table.wait_for_n_keys(num_keys=0)

    def test_empty_acl_group_binding(self, ctx):
        """
        Verifies behavior when binding ACL groups
        """
        eni_key = ctx.asic_eni_table.get_keys()[0]
        sai_stage = get_sai_stage(outbound=False, v4=True, stage_num=ACL_STAGE_1)

        ctx.create_acl_group(ACL_GROUP_1, {"ip_version": "ipv4"})
        acl_group_key = ctx.asic_dash_acl_group_table.wait_for_n_keys(num_keys=1)[0]
        ctx.bind_acl_in(self.eni_name, ACL_STAGE_1, ACL_GROUP_1)
        time.sleep(3)
        # Binding should not happen yet because the ACL group is empty
        assert sai_stage not in ctx.asic_eni_table[eni_key]

        ctx.create_acl_rule(ACL_GROUP_1, ACL_RULE_1, {"priority": "1", "action": "allow", "terminating": "false",
                           "src_addr": "192.168.0.1/32,192.168.1.2/30", "dst_addr": "192.168.0.1/32,192.168.1.2/30", "src_port": "0-1", "dst_port": "0-1"})
        # Now that the group contains a rule, expect binding to occur
        ctx.asic_eni_table.wait_for_field_match(key=eni_key, expected_fields={sai_stage: acl_group_key})

        # Unbinding should occur immediately
        ctx.unbind_acl_in(self.eni_name, ACL_STAGE_1)
        ctx.asic_eni_table.wait_for_field_match(key=eni_key, expected_fields={sai_stage: SAI_NULL_OID})

    def test_acl_group_binding(self, ctx):
        eni_key = ctx.asic_eni_table.get_keys()[0]
        sai_stage = get_sai_stage(outbound=False, v4=True, stage_num=ACL_STAGE_2)

        ctx.create_acl_group(ACL_GROUP_2, {"ip_version": "ipv4"})
        acl_group_key = ctx.asic_dash_acl_group_table.wait_for_n_keys(num_keys=1)[0]
        ctx.create_acl_rule(ACL_GROUP_2, ACL_RULE_1, {"priority": "1", "action": "allow", "terminating": "false",
                           "src_addr": "192.168.0.1/32,192.168.1.2/30", "dst_addr": "192.168.0.1/32,192.168.1.2/30", "src_port": "0-1", "dst_port": "0-1"})
        ctx.bind_acl_in(self.eni_name, ACL_STAGE_2, ACL_GROUP_2)
        # Binding should occurr immediately since we added a rule to the group prior to binding
        ctx.asic_eni_table.wait_for_field_match(key=eni_key, expected_fields={sai_stage: acl_group_key})

        ctx.unbind_acl_in(self.eni_name, ACL_STAGE_2)
        ctx.asic_eni_table.wait_for_field_match(key=eni_key, expected_fields={sai_stage: SAI_NULL_OID})

    def test_acl_rule(self, ctx):
        # Create acl rule before acl group
        ctx.create_acl_rule(ACL_GROUP_1, ACL_RULE_1, {"priority": "1", "action": "allow", "terminating": "false",
                           "src_addr": "192.168.0.1/32,192.168.1.2/30", "dst_addr": "192.168.0.1/32,192.168.1.2/30", "src_port": "0-1", "dst_port": "0-1"})
        time.sleep(3)
        ctx.asic_dash_acl_rule_table.wait_for_n_keys(num_keys=0)
        ctx.create_acl_group(ACL_GROUP_1, {"ip_version": "ipv4"})

        ctx.asic_dash_acl_rule_table.wait_for_n_keys(num_keys=1)

        # Create acl rule with nonexistent acl group, which should never get programmed to ASIC_DB
        ctx.create_acl_rule("0", "0", {"priority": "1", "action": "allow", "terminating": "false",
                           "src_addr": "192.168.0.1/32,192.168.1.2/30", "dst_addr": "192.168.0.1/32,192.168.1.2/30", "src_port": "0-1", "dst_port": "0-1"})
        time.sleep(3)
        ctx.asic_dash_acl_rule_table.wait_for_n_keys(num_keys=1)

        # Create acl with invalid attribute
        ctx.create_acl_rule(ACL_GROUP_1, ACL_RULE_2, {"priority": "abc"})
        time.sleep(3)
        ctx.asic_dash_acl_rule_table.wait_for_n_keys(num_keys=1)

        # Create acl without some mandatory attributes at first
        ctx.create_acl_rule(ACL_GROUP_1, ACL_RULE_2, {"priority": "1", "action": "allow", "terminating": "false"})
        time.sleep(3)
        ctx.asic_dash_acl_rule_table.wait_for_n_keys(num_keys=1)
        # Expect the rule to be created only after all the mandatory attributes are added
        ctx.create_acl_rule(ACL_GROUP_1, ACL_RULE_2, {"src_addr": "", "dst_addr": "192.168.0.1/32,192.168.1.2/30", "src_port": "", "dst_port": "0-1"})
        time.sleep(3)
        ctx.asic_dash_acl_rule_table.wait_for_n_keys(num_keys=2)

        ctx.remove_acl_rule(ACL_GROUP_1, ACL_RULE_1)
        ctx.remove_acl_rule(ACL_GROUP_1, ACL_RULE_2)
        ctx.remove_acl_group(ACL_GROUP_1)
        ctx.asic_dash_acl_rule_table.wait_for_n_keys(num_keys=0)
        ctx.asic_dash_acl_group_table.wait_for_n_keys(num_keys=0)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down
# before retrying
def test_nonflaky_dummy():
    pass
