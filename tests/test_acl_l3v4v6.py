import pytest
from requests import request

L3V4V6_TABLE_TYPE = "L3V4V6"
L3V4V6_TABLE_NAME = "L3_V4V6_TEST"
L3V4V6_BIND_PORTS = ["Ethernet0", "Ethernet4", "Ethernet8"]
L3V4V6_RULE_NAME = "L3V4V6_TEST_RULE"

class TestAcl:
    @pytest.fixture
    def l3v4v6_acl_table(self, dvs_acl):
        try:
            dvs_acl.create_acl_table(L3V4V6_TABLE_NAME,
                                     L3V4V6_TABLE_TYPE,
                                     L3V4V6_BIND_PORTS)
            yield dvs_acl.get_acl_table_ids(1)[0]
        finally:
            dvs_acl.remove_acl_table(L3V4V6_TABLE_NAME)
            dvs_acl.verify_acl_table_count(0)

    @pytest.fixture
    def setup_teardown_neighbor(self, dvs):
        try:
            # NOTE: set_interface_status has a dependency on cdb within dvs,
            # so we still need to setup the db. This should be refactored.
            dvs.setup_db()

            # Bring up an IP interface with a neighbor
            dvs.set_interface_status("Ethernet4", "up")
            dvs.add_ip_address("Ethernet4", "10.0.0.1/24")
            dvs.add_neighbor("Ethernet4", "10.0.0.2", "00:01:02:03:04:05")

            yield dvs.get_asic_db().wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", 1)[0]
        finally:
            # Clean up the IP interface and neighbor
            dvs.remove_neighbor("Ethernet4", "10.0.0.2")
            dvs.remove_ip_address("Ethernet4", "10.0.0.1/24")
            dvs.set_interface_status("Ethernet4", "down")

    def test_L3V4V6AclTableCreationDeletion(self, dvs_acl):
        try:
            dvs_acl.create_acl_table(L3V4V6_TABLE_NAME, L3V4V6_TABLE_TYPE, L3V4V6_BIND_PORTS)

            acl_table_id = dvs_acl.get_acl_table_ids(1)[0]
            acl_table_group_ids = dvs_acl.get_acl_table_group_ids(len(L3V4V6_BIND_PORTS))

            dvs_acl.verify_acl_table_group_members(acl_table_id, acl_table_group_ids, 1)
            dvs_acl.verify_acl_table_port_binding(acl_table_id, L3V4V6_BIND_PORTS, 1)
            # Verify status is written into STATE_DB
            dvs_acl.verify_acl_table_status(L3V4V6_TABLE_NAME, "Active")
        finally:
            dvs_acl.remove_acl_table(L3V4V6_TABLE_NAME)
            dvs_acl.verify_acl_table_count(0)
            # Verify the STATE_DB entry is removed
            dvs_acl.verify_acl_table_status(L3V4V6_TABLE_NAME, None)

    def test_ValidAclRuleCreation_sip_dip(self, dvs_acl, l3v4v6_acl_table):
        config_qualifiers = {"DST_IP": "20.0.0.1/32",
                             "SRC_IP": "10.0.0.0/32"};

        dvs_acl.create_acl_rule(L3V4V6_TABLE_NAME, "VALID_RULE", config_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V4V6_TABLE_NAME, "VALID_RULE", "Active")

        dvs_acl.remove_acl_rule(L3V4V6_TABLE_NAME, "VALID_RULE")
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V4V6_TABLE_NAME, "VALID_RULE", None)
        dvs_acl.verify_no_acl_rules()

    def test_InvalidAclRuleCreation_sip_sipv6(self, dvs_acl, l3v4v6_acl_table):
        config_qualifiers = {"SRC_IPV6": "2777::0/64",
                             "SRC_IP": "10.0.0.0/32"};

        dvs_acl.create_acl_rule(L3V4V6_TABLE_NAME, "INVALID_RULE", config_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V4V6_TABLE_NAME, "INVALID_RULE", "Inactive")

        dvs_acl.remove_acl_rule(L3V4V6_TABLE_NAME, "INVALID_RULE")
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V4V6_TABLE_NAME, "INVALID_RULE", None)
        dvs_acl.verify_no_acl_rules()

    def test_InvalidAclRuleCreation_dip_sipv6(self, dvs_acl, l3v4v6_acl_table):
        config_qualifiers = {"SRC_IPV6": "2777::0/64",
                             "DST_IP": "10.0.0.0/32"};

        dvs_acl.create_acl_rule(L3V4V6_TABLE_NAME, "INVALID_RULE", config_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V4V6_TABLE_NAME, "INVALID_RULE", "Inactive")

        dvs_acl.remove_acl_rule(L3V4V6_TABLE_NAME, "INVALID_RULE")
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V4V6_TABLE_NAME, "INVALID_RULE", None)
        dvs_acl.verify_no_acl_rules()

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
