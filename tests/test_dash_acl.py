from swsscommon import swsscommon
import typing
import time


class MockDvs(object):
    def __init__(self):
        pass
    def get_app_db(self):
        return swsscommon.DBConnector(swsscommon.APPL_DB, "/var/run/redis/redis.sock", 0)
    def get_asic_db(self):
        return swsscommon.DBConnector(swsscommon.ASIC_DB, "/var/run/redis/redis.sock", 0)

def to_string(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


class ProduceStateTable(object):
    def __init__(self, database, table_name: str):
        self.table = swsscommon.ProducerStateTable(
            database,
            table_name)
    def __setitem__(self, key: str, pairs: typing.Union[dict, list, tuple]):
        pairs_str = []
        if isinstance(pairs, dict):
            pairs = pairs.items()
        for k, v in pairs:
            pairs_str.append((to_string(k), to_string(v)))
        self.table.set(key, pairs_str)
    def __delitem__(self, key: str):
        self.table.delete(str(key))


class Table(object):
    def __init__(self, database, table_name: str):
        self.table_name = table_name
        self.table = swsscommon.Table(database, self.table_name)
    def __getitem__(self, key: str):
        exists, result = self.table.get(str(key))
        if not exists:
            return None
        else:
            return dict(result)
    def get_keys(self):
        return self.table.getKeys()



class DashAcl(object):
    def __init__(self, dvs):
        self.dvs = dvs
        self.app_dash_acl_in_table = ProduceStateTable(
            self.dvs.get_app_db(), swsscommon.APP_DASH_ACL_IN_TABLE_NAME)
        self.app_dash_acl_out_table = ProduceStateTable(
            self.dvs.get_app_db(), swsscommon.APP_DASH_ACL_OUT_TABLE_NAME)
        self.app_dash_acl_group_table = ProduceStateTable(
            self.dvs.get_app_db(), swsscommon.APP_DASH_ACL_GROUP_TABLE_NAME)
        self.app_dash_acl_rule_table = ProduceStateTable(
            self.dvs.get_app_db(), swsscommon.APP_DASH_ACL_RULE_TABLE_NAME)
        self.app_dash_eni_table = ProduceStateTable(
            self.dvs.get_app_db(), swsscommon.APP_DASH_ENI_TABLE_NAME)
        self.app_dash_vnet_table = ProduceStateTable(
            self.dvs.get_app_db(), swsscommon.APP_DASH_VNET_TABLE_NAME)
        self.asic_dash_acl_rule_table = Table(
            self.dvs.get_asic_db(), "ASIC_STATE:SAI_OBJECT_TYPE_DASH_ACL_RULE")
        self.asic_dash_acl_group_table = Table(
            self.dvs.get_asic_db(), "ASIC_STATE:SAI_OBJECT_TYPE_DASH_ACL_GROUP")
        self.asic_eni_table = Table(
            self.dvs.get_asic_db(), "ASIC_STATE:SAI_OBJECT_TYPE_ENI")
    def create_acl_rule(self, group_id, rule_id, attr_maps: dict):
        self.app_dash_acl_rule_table[str(group_id) + ":" + str(rule_id)] = attr_maps
    def remove_acl_rule(self, group_id, rule_id):
        del self.app_dash_acl_rule_table[str(group_id) + ":" + str(rule_id)]
    def create_acl_group(self, group_id, attr_maps: dict):
        self.app_dash_acl_group_table[str(group_id)] = attr_maps
    def remove_acl_group(self, group_id):
        del self.app_dash_acl_group_table[str(group_id)]
    def create_eni(self, eni, attr_maps: dict):
        self.app_dash_eni_table[str(eni)] = attr_maps
    def remove_eni(self, eni):
        del self.app_dash_eni_table[str(eni)]
    def create_vnet(self, vnet, attr_maps: dict):
        self.app_dash_vnet_table[str(vnet)] = attr_maps
    def remove_vnet(self, vnet):
        del self.app_dash_vnet_table[str(vnet)]
    def bind_acl_in(self, eni, stage, group_id):
        self.app_dash_acl_in_table[str(eni) + ":" + str(stage)] = {"acl_group_id": str(group_id)}
    def unbind_acl_in(self, eni, stage):
        del self.app_dash_acl_in_table[str(eni) + ":" + str(stage)]
    def bind_acl_out(self, eni, stage, group_id):
        self.app_dash_acl_out_table[str(eni) + ":" + str(stage)] = {"acl_group_id": str(group_id)}
    def unbind_acl_out(self, eni, stage):
        del self.app_dash_acl_out_table[str(eni) + ":" + str(stage)]


class TestAcl(object):
    def test_acl(self, dvs):
        da = DashAcl(dvs)
        vnet_name1 = "vnet1"
        vni = "1"
        eni_name0 = "eth0"
        mac_adress = "00:00:00:00:00:00"
        acl_group1 = "1"
        acl_rule1 = "1"
        stage1 = "1"
        da.create_vnet(vnet_name1, {"vni": vni})
        da.create_eni(eni_name0, {"vnet": vnet_name1, "mac_address": mac_adress})
        da.create_acl_group(acl_group1, {"ip_version": "ipv4"})
        da.create_acl_rule(acl_rule1, acl_group1, {"priority": "1", "action": "allow", "terminating": "false", "src_addr": "192.168.0.1,192.168.1.2", "dst_addr": "192.168.0.1,192.168.1.2", "src_port": "0-1", "dst_port": "0-1"})
        da.bind_acl_in(eni_name0, stage1, acl_group1)
        time.sleep(3)
        rule_ids = da.asic_dash_acl_rule_table.get_keys()
        assert rule_ids
        rule1_id = rule_ids[0]
        group_ids = da.asic_dash_acl_group_table.get_keys()
        assert group_ids
        group1_id = group_ids[0]
        rule1_attr = da.asic_dash_acl_rule_table[rule1_id]
        assert rule1_attr["SAI_DASH_ACL_RULE_ATTR_PRIORITY"] == "1"
        assert rule1_attr["SAI_DASH_ACL_RULE_ATTR_ACTION"] == "SAI_DASH_ACL_RULE_ACTION_PERMIT_AND_CONTINUE"
        assert rule1_attr["SAI_DASH_ACL_RULE_ATTR_DASH_ACL_GROUP_ID"] == group1_id
        assert rule1_attr["SAI_DASH_ACL_RULE_ATTR_DIP"] == "2:192.168.0.1,192.168.1.2"
        assert rule1_attr["SAI_DASH_ACL_RULE_ATTR_SIP"] == "2:192.168.0.1,192.168.1.2"
        assert rule1_attr["SAI_DASH_ACL_RULE_ATTR_DST_PORT"] == "1:0,1"
        assert rule1_attr["SAI_DASH_ACL_RULE_ATTR_SRC_PORT"] == "1:0,1"
        assert rule1_attr["SAI_DASH_ACL_RULE_ATTR_PROTOCOL"].split(":")[0] == "256"
        group1_attr = da.asic_dash_acl_group_table[group1_id]
        assert group1_attr["SAI_DASH_ACL_GROUP_ATTR_IP_ADDR_FAMILY"] == "SAI_IP_ADDR_FAMILY_IPV4"
        eni_ids = da.asic_eni_table.get_keys()
        assert eni_ids
        eni1_id = eni_ids[0]
        eni1_attr = da.asic_eni_table[eni1_id]
        assert eni1_attr["SAI_ENI_ATTR_INBOUND_V4_STAGE" + stage1 + "_DASH_ACL_GROUP_ID"] == group1_id
        da.unbind_acl_in(eni_name0, stage1)
        da.remove_acl_rule(acl_rule1, acl_group1)
        da.remove_acl_group(acl_group1)
        time.sleep(3)
        # Because the Acl group, ENI and VNET has been referred by other objects,
        # And their implementation can only support synchronous deletion, So we need to wait for a while.
        da.remove_eni(eni_name0)
        time.sleep(3)
        da.remove_vnet(vnet_name1)
        time.sleep(3)
        assert len(da.asic_dash_acl_rule_table.get_keys()) == 0
        assert len(da.asic_dash_acl_group_table.get_keys()) == 0


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down
# before retrying
def test_nonflaky_dummy():
    pass

