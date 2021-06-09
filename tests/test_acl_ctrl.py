TABLE_NAME = "CTRL_ACL_TEST"
RULE_NAME = "CTRL_ACL_TEST_RULE"


class TestPortChannelAcl:
    def test_AclCtrl(self, dvs_acl):
        # Create ACL table and ACL rule
        dvs_acl.create_control_plane_acl_table(TABLE_NAME, ["SNMP"])
        dvs_acl.create_acl_rule(TABLE_NAME, RULE_NAME, {"L4_SRC_PORT": "8888"}, priority="88")

        # Verify that no ASIC rules are created
        dvs_acl.verify_acl_table_count(0)
        dvs_acl.verify_no_acl_rules()

        # Cleanup ACL data from Config DB
        dvs_acl.remove_acl_rule(TABLE_NAME, RULE_NAME)
        dvs_acl.remove_acl_table(TABLE_NAME)

        # Verify that the ASIC DB is clean
        dvs_acl.verify_acl_table_count(0)
        dvs_acl.verify_no_acl_rules()


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
