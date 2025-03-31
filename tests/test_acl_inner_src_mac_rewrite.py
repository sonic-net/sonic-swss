import pytest
from requests import request
import time
from swsscommon import swsscommon
import pdb

TABLE_TYPE = "INNER_SRC_MAC_REWRITE_TABLE_TYPE"
CUSTOM_TABLE_TYPE_MATCHES = [
    "TUNNEL_VNI",
    "INNER_SRC_IP"
]
CUSTOM_TABLE_TYPE_BPOINT_TYPES = ["PORT","PORTCHANNEL"]
CUSTOM_TABLE_TYPE_ACTIONS = ["INNER_SRC_MAC_REWRITE_ACTION"]
EXPECTED_ACTION_LIST = ['SAI_ACL_ACTION_TYPE_SET_SRC_MAC']
TABLE_NAME = "INNER_SRC_MAC_REWRITE_TEST"
BIND_PORTS = ["Ethernet0", "Ethernet4"]
RULE_NAME = "INNER_SRC_MAC_REWRITE_TEST_RULE"


class TestInnerSrcMacRewriteAclTable:

    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        self.sdb = swsscommon.DBConnector(6, dvs.redis_sock, 0)

    @pytest.fixture
    def innersrcmacrewrite_acl_table(self, dvs_acl):
        try:
            dvs_acl.create_acl_table_type(TABLE_TYPE, CUSTOM_TABLE_TYPE_MATCHES, CUSTOM_TABLE_TYPE_BPOINT_TYPES, CUSTOM_TABLE_TYPE_ACTIONS)
            dvs_acl.create_acl_table(TABLE_NAME, TABLE_TYPE, BIND_PORTS, stage="egress")
            yield dvs_acl.get_acl_table_ids(1)[0]
        finally:
            dvs_acl.remove_acl_table(TABLE_NAME)
            dvs_acl.remove_acl_table_type(TABLE_TYPE)
            dvs_acl.verify_acl_table_count(0)
    
    def create_entry(self, tbl, key, pairs):
        fvs = swsscommon.FieldValuePairs(pairs)
        tbl.set(key, fvs)
        time.sleep(1)
    
    def create_acl_rule(self, dvs, table_name, rule_name, qualifiers, priority:str="1000", action:str="AA:BB:CC:DD:EE:FF"):
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")

        fvs={
            "PRIORITY": priority,
            "INNER_SRC_MAC_REWRITE_ACTION": action
        }
        for k, v in qualifiers.items():
            fvs[k] = v

        formatted_entry = swsscommon.FieldValuePairs(list(fvs.items()))
        tbl.set(table_name + "|" + rule_name, formatted_entry)
        time.sleep(1)

    def remove_acl_rule(self, dvs, table_name, rule_name):
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        tbl._del(table_name + "|" + rule_name)
        time.sleep(1)

    def update_acl_rule(self, dvs, table_name, rule_name, qualifier):
        table = swsscommon.Table(self.cdb, "ACL_RULE")
        status, fvs=table.get('INNER_SRC_MAC_REWRITE_TEST|INNER_SRC_MAC_REWRITE_TEST_RULE')
        fvs_pairs= dict(fvs)
        for k, v in qualifier.items():
            fvs_pairs[k] = v
        formatted_entry = swsscommon.FieldValuePairs(list(fvs_pairs.items()))
        table.set(table_name + "|" + rule_name, formatted_entry)
        time.sleep(1)     

    def test_InnerSrcMacRewriteAclTableCreationDeletion(self, dvs_acl):

        # This test checks for ACL table and table type creation deletion for inner src mac rewrite
        try:
            dvs_acl.create_acl_table_type(TABLE_TYPE, CUSTOM_TABLE_TYPE_MATCHES, CUSTOM_TABLE_TYPE_BPOINT_TYPES, CUSTOM_TABLE_TYPE_ACTIONS)
            dvs_acl.create_acl_table(TABLE_NAME, TABLE_TYPE, BIND_PORTS, stage="egress")
            acl_table_id = dvs_acl.get_acl_table_ids(1)[0]
            assert acl_table_id is not None
            acl_table_group_ids = dvs_acl.get_acl_table_group_ids(len(BIND_PORTS))

            dvs_acl.verify_acl_table_group_members(acl_table_id, acl_table_group_ids, 1)
            dvs_acl.verify_acl_table_port_binding(acl_table_id, BIND_PORTS, 1, stage="egress")
            dvs_acl.verify_acl_table_action_list(acl_table_id, EXPECTED_ACTION_LIST)
        finally:
            dvs_acl.remove_acl_table(TABLE_NAME)
            dvs_acl.remove_acl_table_type(TABLE_TYPE)
            dvs_acl.verify_acl_table_count(0)

    def test_InnerSrcMacRewriteAclRuleCreationDeletion(self, dvs, dvs_acl, innersrcmacrewrite_acl_table):
        
        # This test checks for ACL rule creation(more than one) deletion for the table type
        
        self.setup_db(dvs)
        # Add the rule
        config_qualifiers = {"INNER_SRC_IP": "10.10.10.10/32", "TUNNEL_VNI": "5000"}
        self.create_acl_rule(dvs, TABLE_NAME, RULE_NAME, config_qualifiers, priority="1000", action="60:BB:AA:C3:3E:AB")
        dvs_acl.verify_acl_rule_status(TABLE_NAME, RULE_NAME, "Active")

        # Verify the rule with SAI entries
        members = dvs_acl.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", 1)
        for member in members:
            fvs = dvs_acl.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", member)
            assert fvs.get("SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IP") == "10.10.10.10&mask:255.255.255.255"
            assert fvs.get("SAI_ACL_ENTRY_ATTR_FIELD_TUNNEL_VNI") == "5000&mask:0xffffffff"
            assert fvs.get("SAI_ACL_ENTRY_ATTR_PRIORITY") == "1000"
            assert fvs.get("SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_MAC") == "60:BB:AA:C3:3E:AB"

        # Add second rule
        config_qualifiers = {"INNER_SRC_IP": "10.10.10.10/32", "TUNNEL_VNI": "5000"}
        self.create_acl_rule(dvs, TABLE_NAME, RULE_NAME+"2", config_qualifiers, priority="9990", action="AB:BB:AA:C3:3E:AB")
        dvs_acl.verify_acl_rule_status(TABLE_NAME, RULE_NAME, "Active")

        # Verify the rule2 status in ASIC_DB
        members = dvs_acl.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", 2)

        # Remove rule and check status in ASIC_DB
        self.remove_acl_rule(dvs, TABLE_NAME, RULE_NAME)
        dvs_acl.verify_acl_rule_status(TABLE_NAME, RULE_NAME, None)

        # Remove second rule and check status in ASIC_DB
        self.remove_acl_rule(dvs, TABLE_NAME, RULE_NAME+"2")
        dvs_acl.verify_acl_rule_status(TABLE_NAME, RULE_NAME+"2", None)

        dvs_acl.verify_no_acl_rules()


    def test_InnerSrcMacRewriteAclRuleUpdate(self, dvs, dvs_acl, innersrcmacrewrite_acl_table):

        # This test checks for ACL rule update for the table type
        
        try :
            self.setup_db(dvs)
            # Add the rule
            config_qualifiers = {"INNER_SRC_IP": "10.10.10.10/32", "TUNNEL_VNI": "4000"}
            self.create_acl_rule(dvs, TABLE_NAME, RULE_NAME, config_qualifiers, priority="1001", action="66:BB:AA:C3:3E:AB")
            dvs_acl.verify_acl_rule_status(TABLE_NAME, RULE_NAME, "Active")

            # Verify the rule with SAI entries
            members = dvs_acl.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", 1)
            for member in members:
                fvs = dvs_acl.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", member)
                assert fvs.get("SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IP") == "10.10.10.10&mask:255.255.255.255"
                assert fvs.get("SAI_ACL_ENTRY_ATTR_FIELD_TUNNEL_VNI") == "4000&mask:0xffffffff"
                assert fvs.get("SAI_ACL_ENTRY_ATTR_PRIORITY") == "1001"
                assert fvs.get("SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_MAC") == "66:BB:AA:C3:3E:AB"

            # Update the rule with inner src ip
            self.update_acl_rule(dvs, TABLE_NAME, RULE_NAME, {"INNER_SRC_IP": "15.15.15.11/32"} )
            members = dvs_acl.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", 1)
            for member in members:
                fvs = dvs_acl.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", member)
                assert fvs.get("SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IP") == "15.15.15.11&mask:255.255.255.255"
                assert fvs.get("SAI_ACL_ENTRY_ATTR_FIELD_TUNNEL_VNI") == "4000&mask:0xffffffff"
                assert fvs.get("SAI_ACL_ENTRY_ATTR_PRIORITY") == "1001"
                assert fvs.get("SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_MAC") == "66:BB:AA:C3:3E:AB"

            # Update the rule with tunnel vni 
            self.update_acl_rule(dvs,  TABLE_NAME, RULE_NAME, {"TUNNEL_VNI": "111"} )
            members = dvs_acl.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", 1)
            for member in members:
                fvs = dvs_acl.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", member)
                assert fvs.get("SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IP") == "15.15.15.11&mask:255.255.255.255"
                assert fvs.get("SAI_ACL_ENTRY_ATTR_FIELD_TUNNEL_VNI") == "111&mask:0xffffffff"
                assert fvs.get("SAI_ACL_ENTRY_ATTR_PRIORITY") == "1001"
                assert fvs.get("SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_MAC") == "66:BB:AA:C3:3E:AB"

            # Update the rule with action
            self.update_acl_rule(dvs, TABLE_NAME, RULE_NAME, {"INNER_SRC_MAC_REWRITE_ACTION": "11:BB:AA:C3:3E:AB"} )
            members = dvs_acl.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", 1)
            for member in members:
                fvs = dvs_acl.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", member)
                assert fvs.get("SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IP") == "15.15.15.11&mask:255.255.255.255"
                assert fvs.get("SAI_ACL_ENTRY_ATTR_FIELD_TUNNEL_VNI") == "111&mask:0xffffffff"
                assert fvs.get("SAI_ACL_ENTRY_ATTR_PRIORITY") == "1001"
                assert fvs.get("SAI_ACL_ENTRY_ATTR_ACTION_SET_SRC_MAC") == "11:BB:AA:C3:3E:AB"
        
        finally:
            # Remove the rule
            self.remove_acl_rule(dvs, TABLE_NAME, RULE_NAME)
            dvs_acl.verify_no_acl_rules()
            dvs_acl.remove_acl_table(TABLE_NAME)
            dvs_acl.remove_acl_table_type(TABLE_TYPE)
            dvs_acl.verify_acl_table_count(0)
        

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass

