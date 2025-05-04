import pytest
from requests import request
import time
from swsscommon import swsscommon
from dvslib.dvs_common import PollingConfig, wait_for_result
import pdb
import json

TABLE_TYPE = "INNER_SRC_MAC_REWRITE_TABLE_TYPE"
CUSTOM_TABLE_TYPE_MATCHES = [
    "TUNNEL_VNI",
    "INNER_SRC_IP"
]
CUSTOM_TABLE_TYPE_BPOINT_TYPES = ["PORT","PORTCHANNEL"]
CUSTOM_TABLE_TYPE_ACTIONS = ["INNER_SRC_MAC_REWRITE_ACTION"]
EXPECTED_ACTION_LIST = ["SAI_ACL_ACTION_TYPE_SET_INNER_SRC_MAC"]
ASIC_STATE_ACL = "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY"
TABLE_NAME = "INNER_SRC_MAC_REWRITE_TEST"
BIND_PORTS = ["Ethernet0", "Ethernet4"]
RULE_NAME = "INNER_SRC_MAC_REWRITE_TEST_RULE"


class TestInnerSrcMacRewriteAclTable:

    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.ctdb = swsscommon.DBConnector(2, dvs.redis_sock, 0)
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

    def remove_acl_rule(self, dvs, table_name, rule_name):
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        tbl._del(table_name + "|" + rule_name)

    def validate_asic_acl_entries(self, dvs_acl, asic_db, rule_id, expected_qualifier, update=False):
        def _access_function():
            false_ret = (False, '')

            if update == False:
                key = rule_id     
            else:
                keys = asic_db.get_keys(ASIC_STATE_ACL)
                for k in keys:
                    if k == rule_id:
                        return false_ret
                    
                key = dvs_acl.get_acl_rule_id()
                
            fvs = asic_db.get_entry(ASIC_STATE_ACL, key)
            if not fvs:
                return false_ret

            for qualifer in expected_qualifier:
                if qualifer not in fvs:
                    return false_ret

                if fvs[qualifer] != expected_qualifier[qualifer]:
                    return false_ret

            return (True, key)
        val, result = wait_for_result(_access_function, polling_config = PollingConfig(polling_interval=1, timeout=1.0, strict=True),
           failure_message="Inner-src-mac-rewrite ACL rule not updated")

    def update_acl_rule(self, dvs, table_name, rule_name, qualifier):
        table = swsscommon.Table(self.cdb, "ACL_RULE")
        status, fvs=table.get(table_name+"|"+rule_name)
        fvs_pairs= dict(fvs)
        for k, v in qualifier.items():
            fvs_pairs[k] = v
        formatted_entry = swsscommon.FieldValuePairs(list(fvs_pairs.items()))
        table.set(table_name + "|" + rule_name, formatted_entry)
    
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
            dvs_acl.verify_acl_table_status(TABLE_NAME, "Active")
        finally:
            dvs_acl.remove_acl_table(TABLE_NAME)
            dvs_acl.remove_acl_table_type(TABLE_TYPE)
            dvs_acl.verify_acl_table_count(0)

    def test_InnerSrcMacRewriteAclRuleCreationDeletion(self, dvs, dvs_acl, innersrcmacrewrite_acl_table):

        # This test checks for ACL rule creation(more than one) deletion for the table type inner src mac rewrite
        self.setup_db(dvs)

        # Add the first rule and verify status in STATE_DB
        config_qualifiers = {"INNER_SRC_IP": "10.10.10.10/32", "TUNNEL_VNI": "5000"}
        self.create_acl_rule(dvs, TABLE_NAME, RULE_NAME, config_qualifiers, priority="1000", action="60:BB:AA:C3:3E:AB")
        dvs_acl.verify_acl_rule_status(TABLE_NAME, RULE_NAME, "Active")

        # Add second rule and verify status in STATE_DB
        config_qualifiers = {"INNER_SRC_IP": "9.9.9.9/30", "TUNNEL_VNI": "5000"}
        self.create_acl_rule(dvs, TABLE_NAME, RULE_NAME+"2", config_qualifiers, priority="9990", action="AB:BB:AA:C3:3E:AB")
        dvs_acl.verify_acl_rule_status(TABLE_NAME, RULE_NAME+"2", "Active")
        
        # Remove first rule and check status in STATE_DB
        self.remove_acl_rule(dvs, TABLE_NAME, RULE_NAME)
        dvs_acl.verify_acl_rule_status(TABLE_NAME, RULE_NAME, None)
        dvs_acl.verify_acl_rule_status(TABLE_NAME, RULE_NAME+"2", "Active")

        # Remove second rule and check status in STATE_DB
        self.remove_acl_rule(dvs, TABLE_NAME, RULE_NAME+"2")
        dvs_acl.verify_acl_rule_status(TABLE_NAME, RULE_NAME+"2", None)

        # Verify no rules in ASIC_DB
        dvs_acl.verify_no_acl_rules()

    def test_InnerSrcMacRewriteAclRuleUpdate(self, dvs, dvs_acl, innersrcmacrewrite_acl_table):

        # This test checks for ACL rule update for the table type inner src mac rewrite

        try :
            self.setup_db(dvs)

            # Add the rule
            config_qualifiers = {"INNER_SRC_IP": "10.10.10.10/32", "TUNNEL_VNI": "4000"}
            self.create_acl_rule(dvs, TABLE_NAME, RULE_NAME, config_qualifiers, priority="1001", action="66:BB:AA:C3:3E:AB")
            dvs_acl.verify_acl_rule_status(TABLE_NAME, RULE_NAME, "Active")
            rule_id = dvs_acl.get_acl_rule_id()

            # SAI entries for the rule creation
            new_expected_sai_qualifiers={"SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IP": "10.10.10.10&mask:255.255.255.255",
                                        "SAI_ACL_ENTRY_ATTR_FIELD_TUNNEL_VNI": "4000&mask:0xffffffff",
                                        "SAI_ACL_ENTRY_ATTR_PRIORITY": "1001",
                                        "SAI_ACL_ENTRY_ATTR_ACTION_SET_INNER_SRC_MAC": "66:BB:AA:C3:3E:AB"} 

            # Verify the rule with SAI entries
            self.validate_asic_acl_entries(dvs_acl, dvs_acl.asic_db, rule_id, new_expected_sai_qualifiers)
            
            # Verify the rule with counter id to be present in ASIC DB
            counter_id = dvs_acl.get_acl_counter_oid()
            assert counter_id in dvs_acl.get_acl_counter_ids(1)
            
            # Update the rule with inner src ip
            self.update_acl_rule(dvs, TABLE_NAME, RULE_NAME, {"INNER_SRC_IP": "15.15.15.15/20"})

            # Expected SAI entries for the rule update #1 
            new_expected_sai_qualifiers={"SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IP": "15.15.15.15&mask:255.255.240.0"}

            # Verify the rule id and SAI entries are updated in ASIC DB
            self.validate_asic_acl_entries(dvs_acl, dvs_acl.asic_db, rule_id, new_expected_sai_qualifiers, True)
        
            # Verify the rule with counter id to be present in ASIC DB
            counter_id_2 = dvs_acl.get_acl_counter_oid()
            rule_id_2 = dvs_acl.get_acl_rule_id()
            assert counter_id_2 in dvs_acl.get_acl_counter_ids(1)

            # Update the rule with tunnel vni and inner src ip
            self.update_acl_rule(dvs, TABLE_NAME, RULE_NAME, {"TUNNEL_VNI": "111", "INNER_SRC_IP": "20.20.20.20/20"} )

            # Expected SAI entries for the rule update #2 
            new_expected_sai_qualifiers={"SAI_ACL_ENTRY_ATTR_FIELD_INNER_SRC_IP": "20.20.20.20&mask:255.255.240.0",
                                         "SAI_ACL_ENTRY_ATTR_FIELD_TUNNEL_VNI": "111&mask:0xffffffff"} 

            # Verify the rule id and SAI entries are updated in ASIC DB 
            self.validate_asic_acl_entries(dvs_acl, dvs_acl.asic_db, rule_id_2, new_expected_sai_qualifiers, True)

            # Verify the rule with counter id to be present in ASIC DB
            counter_id_3 = dvs_acl.get_acl_counter_oid()
            rule_id_3 = dvs_acl.get_acl_rule_id()
            assert counter_id_3 in dvs_acl.get_acl_counter_ids(1)

            # Update the rule with action
            self.update_acl_rule(dvs, TABLE_NAME, RULE_NAME, {"INNER_SRC_MAC_REWRITE_ACTION": "11:BB:AA:C3:3E:AB"} )

            # Expected SAI entries for the rule update #3
            new_expected_sai_qualifiers={"SAI_ACL_ENTRY_ATTR_ACTION_SET_INNER_SRC_MAC": "11:BB:AA:C3:3E:AB"} 

            # Verify the rule id and SAI entries are updated in ASIC DB
            self.validate_asic_acl_entries(dvs_acl, dvs_acl.asic_db, rule_id_3, new_expected_sai_qualifiers, True)

            # Verify the rule with counter id to be present in ASIC DB
            counter_id_4= dvs_acl.get_acl_counter_oid()
            assert counter_id_4 in dvs_acl.get_acl_counter_ids(1)

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