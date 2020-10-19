import pytest

TABLE_TYPE = "L3"
TABLE_NAME = "EGRESS_TEST"
BIND_PORTS = ["Ethernet0", "Ethernet4"]
RULE_NAME = "EGRESS_TEST_RULE"


class TestEgressAclTable:
    @pytest.yield_fixture
    def egress_acl_table(self, dvs_acl):
        try:
            dvs_acl.create_acl_table(TABLE_NAME, TABLE_TYPE, BIND_PORTS, stage="egress")
            yield dvs_acl.get_acl_table_ids(1)[0]
        finally:
            dvs_acl.remove_acl_table(TABLE_NAME)
            dvs_acl.verify_acl_table_count(0)

    def test_EgressAclTableCreationDeletion(self, dvs_acl):
        try:
            dvs_acl.create_acl_table(TABLE_NAME, TABLE_TYPE, BIND_PORTS, stage="egress")

            acl_table_id = dvs_acl.get_acl_table_ids(1)[0]
            acl_table_group_ids = dvs_acl.get_acl_table_group_ids(len(BIND_PORTS))

            dvs_acl.verify_acl_table_group_members(acl_table_id, acl_table_group_ids, 1)
            dvs_acl.verify_acl_table_port_binding(acl_table_id, BIND_PORTS, 1, stage="egress")
        finally:
            dvs_acl.remove_acl_table(TABLE_NAME)
            dvs_acl.verify_acl_table_count(0)

    def test_EgressAclRuleL4SrcPortRange(self, dvs_acl, egress_acl_table):
        config_qualifiers = {"L4_SRC_PORT_RANGE": "0-1001"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE": dvs_acl.get_acl_range_comparator("SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE", "0,1001")
        }

        dvs_acl.create_acl_rule(TABLE_NAME, RULE_NAME, config_qualifiers, priority="999")
        dvs_acl.verify_acl_rule(expected_sai_qualifiers, priority="999")

        dvs_acl.remove_acl_rule(TABLE_NAME, RULE_NAME)
        dvs_acl.verify_no_acl_rules()

    def test_EgressAclRuleL4DstPortRange(self, dvs_acl, egress_acl_table):
        config_qualifiers = {"L4_DST_PORT_RANGE": "1003-6666"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE": dvs_acl.get_acl_range_comparator("SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE", "1003,6666")
        }

        dvs_acl.create_acl_rule(TABLE_NAME, RULE_NAME, config_qualifiers, priority="999")
        dvs_acl.verify_acl_rule(expected_sai_qualifiers, priority="999")

        dvs_acl.remove_acl_rule(TABLE_NAME, RULE_NAME)
        dvs_acl.verify_no_acl_rules()

    def test_EgressAclRuleL2EthType(self, dvs_acl, egress_acl_table):
        config_qualifiers = {"ETHER_TYPE": "8000"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE": dvs_acl.get_simple_qualifier_comparator("8000&mask:0xffff")
        }

        dvs_acl.create_acl_rule(TABLE_NAME, RULE_NAME, config_qualifiers, action="DROP", priority="1000")
        dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP", priority="1000")

        dvs_acl.remove_acl_rule(TABLE_NAME, RULE_NAME)
        dvs_acl.verify_no_acl_rules()

    def test_EgressAclRuleTunnelVNI(self, dvs_acl, egress_acl_table):
        config_qualifiers = {"TUNNEL_VNI": "5000"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_TUNNEL_VNI": dvs_acl.get_simple_qualifier_comparator("5000&mask:0xffffffff")
        }

        dvs_acl.create_acl_rule(TABLE_NAME, RULE_NAME, config_qualifiers, action="DROP", priority="1000")
        dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP", priority="1000")

        dvs_acl.remove_acl_rule(TABLE_NAME, RULE_NAME)
        dvs_acl.verify_no_acl_rules()

    def test_EgressAclRuleTC(self, dvs_acl, egress_acl_table):
        config_qualifiers = {"TC": "1"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_TC": dvs_acl.get_simple_qualifier_comparator("1&mask:0xff")
        }

        dvs_acl.create_acl_rule(TABLE_NAME, RULE_NAME, config_qualifiers, action="DROP", priority="1000")
        dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP", priority="1000")

        dvs_acl.remove_acl_rule(TABLE_NAME, RULE_NAME)
        dvs_acl.verify_no_acl_rules()

    def test_EgressAclInnerIPProtocol(self, dvs_acl, egress_acl_table):
        config_qualifiers = {"INNER_IP_PROTOCOL": "8"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_INNER_IP_PROTOCOL": dvs_acl.get_simple_qualifier_comparator("8&mask:0xff")
        }

        dvs_acl.create_acl_rule(TABLE_NAME, RULE_NAME, config_qualifiers, action="DROP", priority="1000")
        dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP", priority="1000")

        dvs_acl.remove_acl_rule(TABLE_NAME, RULE_NAME)
        dvs_acl.verify_no_acl_rules()

    def test_EgressAclInnerEthType(self, dvs_acl, egress_acl_table):
        config_qualifiers = {"INNER_ETHER_TYPE": "8000"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_INNER_ETHER_TYPE": dvs_acl.get_simple_qualifier_comparator("8000&mask:0xffff")
        }

        dvs_acl.create_acl_rule(TABLE_NAME, RULE_NAME, config_qualifiers, action="DROP", priority="1000")
        dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP", priority="1000")

        dvs_acl.remove_acl_rule(TABLE_NAME, RULE_NAME)
        dvs_acl.verify_no_acl_rules()

    def test_EgressAclInnerL4SrcPort(self, dvs_acl, egress_acl_table):
        config_qualifiers = {"INNER_L4_SRC_PORT": "999"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_INNER_L4_SRC_PORT": dvs_acl.get_simple_qualifier_comparator("999&mask:0xffff")
        }

        dvs_acl.create_acl_rule(TABLE_NAME, RULE_NAME, config_qualifiers, action="DROP", priority="1000")
        dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP", priority="1000")

        dvs_acl.remove_acl_rule(TABLE_NAME, RULE_NAME)
        dvs_acl.verify_no_acl_rules()

    def test_EgressAclInnerL4DstPort(self, dvs_acl, egress_acl_table):
        config_qualifiers = {"INNER_L4_DST_PORT": "999"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_INNER_L4_DST_PORT": dvs_acl.get_simple_qualifier_comparator("999&mask:0xffff")
        }

        dvs_acl.create_acl_rule(TABLE_NAME, RULE_NAME, config_qualifiers, action="DROP", priority="1000")
        dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP", priority="1000")

        dvs_acl.remove_acl_rule(TABLE_NAME, RULE_NAME)
        dvs_acl.verify_no_acl_rules()


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
