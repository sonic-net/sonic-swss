import pytest
from requests import request
import time

L3_TABLE_TYPE = "L3"
L3_TABLE_NAME = "L3_TEST"
L3_BIND_PORTS = ["Ethernet0", "Ethernet4", "Ethernet8", "Ethernet12"]
L3_RULE_NAME = "L3_TEST_RULE"

L3V6_TABLE_TYPE = "L3V6"
L3V6_TABLE_NAME = "L3_V6_TEST"
L3V6_BIND_PORTS = ["Ethernet0", "Ethernet4", "Ethernet8"]
L3V6_RULE_NAME = "L3V6_TEST_RULE"

MCLAG_TABLE_TYPE = "MCLAG"
MCLAG_TABLE_NAME = "MCLAG_TEST"
MCLAG_BIND_PORTS = ["Ethernet0", "Ethernet4", "Ethernet8", "Ethernet12"]
MCLAG_RULE_NAME = "MCLAG_TEST_RULE"

MIRROR_TABLE_TYPE = "MIRROR"
MIRROR_TABLE_NAME = "MIRROR_TEST"
MIRROR_BIND_PORTS = ["Ethernet0", "Ethernet4", "Ethernet8", "Ethernet12"]
MIRROR_RULE_NAME = "MIRROR_TEST_RULE"

PFCWD_TABLE_TYPE = "PFCWD"
PFCWD_TABLE_NAME = "PFCWD_TEST"
PFCWD_BIND_PORTS = ["Ethernet0", "Ethernet4", "Ethernet8", "Ethernet12"]
class TestAcl:
    @pytest.fixture(params=['ingress', 'egress'])
    def l3_acl_table(self, dvs_acl, request):
        try:
            dvs_acl.create_acl_table(L3_TABLE_NAME, L3_TABLE_TYPE, L3_BIND_PORTS, stage=request.param)
            yield dvs_acl.get_acl_table_ids(1)[0], request.param
        finally:
            dvs_acl.remove_acl_table(L3_TABLE_NAME)
            dvs_acl.verify_acl_table_count(0)

    @pytest.fixture
    def l3v6_acl_table(self, dvs_acl):
        try:
            dvs_acl.create_acl_table(L3V6_TABLE_NAME,
                                     L3V6_TABLE_TYPE,
                                     L3V6_BIND_PORTS)
            yield dvs_acl.get_acl_table_ids(1)[0]
        finally:
            dvs_acl.remove_acl_table(L3V6_TABLE_NAME)
            dvs_acl.verify_acl_table_count(0)

    @pytest.fixture
    def mclag_acl_table(self, dvs_acl):
        try:
            dvs_acl.create_acl_table(MCLAG_TABLE_NAME, MCLAG_TABLE_TYPE, MCLAG_BIND_PORTS)
            yield dvs_acl.get_acl_table_ids(1)[0]
        finally:
            dvs_acl.remove_acl_table(MCLAG_TABLE_NAME)
            dvs_acl.verify_acl_table_count(0)

    @pytest.fixture
    def mirror_acl_table(self, dvs_acl):
        try:
            dvs_acl.create_acl_table(MIRROR_TABLE_NAME, MIRROR_TABLE_TYPE, MIRROR_BIND_PORTS)
            yield dvs_acl.get_acl_table_ids(1)[0]
        finally:
            dvs_acl.remove_acl_table(MIRROR_TABLE_NAME)
            dvs_acl.verify_acl_table_count(0)

    @pytest.fixture(params=['ingress', 'egress'])
    def pfcwd_acl_table(self, dvs_acl, request):
        try:
            dvs_acl.create_acl_table(PFCWD_TABLE_NAME, PFCWD_TABLE_TYPE, PFCWD_BIND_PORTS, request.param)
            yield dvs_acl.get_acl_table_ids(1)[0], request.param
        finally:
            dvs_acl.remove_acl_table(PFCWD_TABLE_NAME)
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

    def test_AclTableCreationDeletion(self, dvs_acl):
        try:
            dvs_acl.create_acl_table(L3_TABLE_NAME, L3_TABLE_TYPE, L3_BIND_PORTS)

            acl_table_id = dvs_acl.get_acl_table_ids(1)[0]
            acl_table_group_ids = dvs_acl.get_acl_table_group_ids(len(L3_BIND_PORTS))

            dvs_acl.verify_acl_table_group_members(acl_table_id, acl_table_group_ids, 1)
            dvs_acl.verify_acl_table_port_binding(acl_table_id, L3_BIND_PORTS, 1)
            # Verify status is written into STATE_DB
            dvs_acl.verify_acl_table_status(L3_TABLE_NAME, "Active")
        finally:
            dvs_acl.remove_acl_table(L3_TABLE_NAME)
            dvs_acl.verify_acl_table_count(0)
            # Verify the STATE_DB entry is removed
            dvs_acl.verify_acl_table_status(L3_TABLE_NAME, None)

    def test_InvalidAclTableCreationDeletion(self, dvs_acl):
        try:
            dvs_acl.create_acl_table("INVALID_ACL_TABLE", L3_TABLE_TYPE, "dummy_port", "invalid_stage")
            # Verify status is written into STATE_DB
            dvs_acl.verify_acl_table_status("INVALID_ACL_TABLE", "Inactive")
        finally:
            dvs_acl.remove_acl_table("INVALID_ACL_TABLE")
            dvs_acl.verify_acl_table_count(0)
            # Verify the STATE_DB entry is removed
            dvs_acl.verify_acl_table_status("INVALID_ACL_TABLE", None)

    def test_InvalidAclRuleCreation(self, dvs_acl, l3_acl_table):
        config_qualifiers = {"INVALID_QUALIFIER": "TEST"}

        dvs_acl.create_acl_rule(L3_TABLE_NAME, "INVALID_RULE", config_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, "INVALID_RULE", "Inactive")

        dvs_acl.remove_acl_rule(L3_TABLE_NAME, "INVALID_RULE")
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, "INVALID_RULE", None)
        dvs_acl.verify_no_acl_rules()

    def test_AclRuleUpdate(self, dvs_acl, l3_acl_table):
        """The test is to verify there is no duplicated flex counter when updating an ACL rule
        """
        config_qualifiers = {"SRC_IP": "10.10.10.10/32"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP": dvs_acl.get_simple_qualifier_comparator("10.10.10.10&mask:255.255.255.255")
        }

        dvs_acl.create_acl_rule(L3_TABLE_NAME, L3_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        
        acl_rule_id = dvs_acl.get_acl_rule_id()
        counter_id = dvs_acl.get_acl_counter_oid()
        
        new_config_qualifiers = {"SRC_IP": "10.10.10.11/32"}
        new_expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP": dvs_acl.get_simple_qualifier_comparator("10.10.10.11&mask:255.255.255.255")
        }
        dvs_acl.update_acl_rule(L3_TABLE_NAME, L3_RULE_NAME, new_config_qualifiers)
        # Verify the rule has been updated
        retry = 5
        while dvs_acl.get_acl_rule_id() == acl_rule_id and retry >= 0:
            retry -= 1
            time.sleep(1)
        assert retry > 0
        dvs_acl.verify_acl_rule(new_expected_sai_qualifiers)
        # Verify the previous counter is removed
        if counter_id:
            dvs_acl.check_acl_counter_not_in_counters_map(counter_id)
        dvs_acl.remove_acl_rule(L3_TABLE_NAME, L3_RULE_NAME)
        dvs_acl.verify_no_acl_rules()

    def test_AclRuleL4SrcPort(self, dvs_acl, l3_acl_table):
        config_qualifiers = {"L4_SRC_PORT": "65000"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT": dvs_acl.get_simple_qualifier_comparator("65000&mask:0xffff")
        }

        dvs_acl.create_acl_rule(L3_TABLE_NAME, L3_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(L3_TABLE_NAME, L3_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_AclRuleIpProtocol(self, dvs_acl, l3_acl_table):
        config_qualifiers = {"IP_PROTOCOL": "6"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_IP_PROTOCOL": dvs_acl.get_simple_qualifier_comparator("6&mask:0xff")
        }

        dvs_acl.create_acl_rule(L3_TABLE_NAME, L3_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(L3_TABLE_NAME, L3_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_AclRuleTCPProtocolAppendedForTCPFlags(self, dvs_acl, l3_acl_table):
        """
        Verify TCP Protocol number (6) will be appended for ACL rules matching TCP_FLAGS
        """
        config_qualifiers = {"TCP_FLAGS": "0x07/0x3f"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_TCP_FLAGS":
                dvs_acl.get_simple_qualifier_comparator("7&mask:0x3f"),
            "SAI_ACL_ENTRY_ATTR_FIELD_IP_PROTOCOL":
                dvs_acl.get_simple_qualifier_comparator("6&mask:0xff")
        }
        dvs_acl.create_acl_rule(L3_TABLE_NAME, L3_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(L3_TABLE_NAME, L3_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_AclRuleNextHeader(self, dvs_acl, l3_acl_table):
        config_qualifiers = {"NEXT_HEADER": "6"}

        # Shouldn't allow NEXT_HEADER on vanilla L3 tables.
        dvs_acl.create_acl_rule(L3_TABLE_NAME, L3_RULE_NAME, config_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, "Inactive")
        dvs_acl.verify_no_acl_rules()

        dvs_acl.remove_acl_rule(L3_TABLE_NAME, L3_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleNextHeaderAppendedForTCPFlags(self, dvs_acl, l3v6_acl_table):
        """
        Verify next heder (6) will be appended for IPv6 ACL rules matching TCP_FLAGS
        """
        config_qualifiers = {"TCP_FLAGS": "0x07/0x3f"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_TCP_FLAGS":
                dvs_acl.get_simple_qualifier_comparator("7&mask:0x3f"),
            "SAI_ACL_ENTRY_ATTR_FIELD_IPV6_NEXT_HEADER":
                dvs_acl.get_simple_qualifier_comparator("6&mask:0xff")
        }

        dvs_acl.create_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_AclRuleInPorts(self, dvs_acl, mirror_acl_table):
        """
        Verify IN_PORTS matches on ACL rule.
        Using MIRROR table type for IN_PORTS matches.
        """
        config_qualifiers = {
            "IN_PORTS": "Ethernet8,Ethernet12",
        }

        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS": dvs_acl.get_port_list_comparator(["Ethernet8", "Ethernet12"])
        }

        dvs_acl.create_acl_rule(MIRROR_TABLE_NAME, MIRROR_RULE_NAME, config_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(MIRROR_TABLE_NAME, MIRROR_RULE_NAME, "Active")
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        dvs_acl.remove_acl_rule(MIRROR_TABLE_NAME, MIRROR_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(MIRROR_TABLE_NAME, MIRROR_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_AclRuleOutPorts(self, dvs_acl, mclag_acl_table):
        """
        Verify OUT_PORTS matches on ACL rule.
        Using MCLAG table type for OUT_PORTS matches.
        """
        config_qualifiers = {
            "OUT_PORTS": "Ethernet8,Ethernet12",
        }

        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_OUT_PORTS": dvs_acl.get_port_list_comparator(["Ethernet8", "Ethernet12"])
        }

        dvs_acl.create_acl_rule(MCLAG_TABLE_NAME, MCLAG_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(MCLAG_TABLE_NAME, MCLAG_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(MCLAG_TABLE_NAME, MCLAG_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(MCLAG_TABLE_NAME, MCLAG_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_AclRuleInPortsNonExistingInterface(self, dvs_acl, mirror_acl_table):
        """
        Using MIRROR table type as it has IN_PORTS matches.
        """
        config_qualifiers = {
            "IN_PORTS": "FOO_BAR_BAZ"
        }

        dvs_acl.create_acl_rule(MIRROR_TABLE_NAME, MIRROR_RULE_NAME, config_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(MIRROR_TABLE_NAME, MIRROR_RULE_NAME, "Inactive")
        dvs_acl.verify_no_acl_rules()
        dvs_acl.remove_acl_rule(MIRROR_TABLE_NAME, MIRROR_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(MIRROR_TABLE_NAME, MIRROR_RULE_NAME, None)

    def test_AclRuleOutPortsNonExistingInterface(self, dvs_acl, mclag_acl_table):
        """
        Using MCLAG table type as it has OUT_PORTS matches.
        """
        config_qualifiers = {
            "OUT_PORTS": "FOO_BAR_BAZ"
        }

        dvs_acl.create_acl_rule(MCLAG_TABLE_NAME, MCLAG_RULE_NAME, config_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(MCLAG_TABLE_NAME, MCLAG_RULE_NAME, "Inactive")
        dvs_acl.verify_no_acl_rules()
        dvs_acl.remove_acl_rule(MCLAG_TABLE_NAME, MCLAG_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(MCLAG_TABLE_NAME, MCLAG_RULE_NAME, None)

    def test_AclRuleVlanId(self, dvs_acl, l3_acl_table):
        config_qualifiers = {"VLAN_ID": "100"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_OUTER_VLAN_ID": dvs_acl.get_simple_qualifier_comparator("100&mask:0xfff")
        }

        dvs_acl.create_acl_rule(L3_TABLE_NAME, L3_RULE_NAME, config_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, "Active")
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        dvs_acl.remove_acl_rule(L3_TABLE_NAME, L3_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_AclRuleIPTypeNonIpv4(self, dvs_acl, l3_acl_table):
        config_qualifiers = {"IP_TYPE": "NON_IPv4"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE": dvs_acl.get_simple_qualifier_comparator("SAI_ACL_IP_TYPE_NON_IPV4&mask:0xffffffffffffffff")
        }

        dvs_acl.create_acl_rule(L3_TABLE_NAME, L3_RULE_NAME, config_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, "Active")
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        dvs_acl.remove_acl_rule(L3_TABLE_NAME, L3_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_V6AclTableCreationDeletion(self, dvs_acl):
        try:
            dvs_acl.create_acl_table(L3V6_TABLE_NAME,
                                     L3V6_TABLE_TYPE,
                                     L3V6_BIND_PORTS)

            acl_table_id = dvs_acl.get_acl_table_ids(1)[0]
            acl_table_group_ids = dvs_acl.get_acl_table_group_ids(len(L3V6_BIND_PORTS))
            dvs_acl.verify_acl_table_group_members(acl_table_id, acl_table_group_ids, 1)
            dvs_acl.verify_acl_table_port_binding(acl_table_id, L3V6_BIND_PORTS, 1)
            # Verify status is written into STATE_DB
            dvs_acl.verify_acl_table_status(L3V6_TABLE_NAME, "Active")
        finally:
            dvs_acl.remove_acl_table(L3V6_TABLE_NAME)
            # Verify the STATE_DB entry is cleared
            dvs_acl.verify_acl_table_status(L3V6_TABLE_NAME, None)
            dvs_acl.verify_acl_table_count(0)

    def test_V6AclRuleIPTypeNonIpv6(self, dvs_acl, l3v6_acl_table):
        config_qualifiers = {"IP_TYPE": "NON_IPv6"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE": dvs_acl.get_simple_qualifier_comparator("SAI_ACL_IP_TYPE_NON_IPV6&mask:0xffffffffffffffff")
        }

        dvs_acl.create_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME, config_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, "Active")
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        dvs_acl.remove_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleIPv6Any(self, dvs_acl, l3v6_acl_table):
        config_qualifiers = {"IP_TYPE": "IPv6ANY"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE": dvs_acl.get_simple_qualifier_comparator("SAI_ACL_IP_TYPE_IPV6ANY&mask:0xffffffffffffffff")
        }

        dvs_acl.create_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME, config_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, "Active")
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        dvs_acl.remove_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleIPv6AnyDrop(self, dvs_acl, l3v6_acl_table):
        config_qualifiers = {"IP_TYPE": "IPv6ANY"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE": dvs_acl.get_simple_qualifier_comparator("SAI_ACL_IP_TYPE_IPV6ANY&mask:0xffffffffffffffff")
        }

        dvs_acl.create_acl_rule(L3V6_TABLE_NAME,
                                L3V6_RULE_NAME,
                                config_qualifiers,
                                action="DROP")
        dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP")
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    # This test validates that backwards compatibility works as expected, it should
    # be converted to a negative test after the 202012 release.
    def test_V6AclRuleIpProtocol(self, dvs_acl, l3v6_acl_table):
        config_qualifiers = {"IP_PROTOCOL": "6"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_IPV6_NEXT_HEADER": dvs_acl.get_simple_qualifier_comparator("6&mask:0xff")
        }

        dvs_acl.create_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleNextHeader(self, dvs_acl, l3v6_acl_table):
        config_qualifiers = {"NEXT_HEADER": "6"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_IPV6_NEXT_HEADER": dvs_acl.get_simple_qualifier_comparator("6&mask:0xff")
        }

        dvs_acl.create_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleSrcIPv6(self, dvs_acl, l3v6_acl_table):

        config_qualifiers = {"SRC_IPV6": "2777::0/64"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6": dvs_acl.get_simple_qualifier_comparator("2777::&mask:ffff:ffff:ffff:ffff::")
        }

        dvs_acl.create_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleDstIPv6(self, dvs_acl, l3v6_acl_table):
        config_qualifiers = {"DST_IPV6": "2002::2/128"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_DST_IPV6": dvs_acl.get_simple_qualifier_comparator("2002::2&mask:ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff")
        }

        dvs_acl.create_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleL4SrcPort(self, dvs_acl, l3v6_acl_table):
        config_qualifiers = {"L4_SRC_PORT": "65000"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT": dvs_acl.get_simple_qualifier_comparator("65000&mask:0xffff")
        }

        dvs_acl.create_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleL4DstPort(self, dvs_acl, l3v6_acl_table):
        config_qualifiers = {"L4_DST_PORT": "65001"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_L4_DST_PORT": dvs_acl.get_simple_qualifier_comparator("65001&mask:0xffff")
        }

        dvs_acl.create_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleL4SrcPortRange(self, dvs_acl, l3v6_acl_table):
        config_qualifiers = {"L4_SRC_PORT_RANGE": "1-100"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE": dvs_acl.get_acl_range_comparator("SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE", "1,100")
        }

        dvs_acl.create_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleL4DstPortRange(self, dvs_acl, l3v6_acl_table):
        config_qualifiers = {"L4_DST_PORT_RANGE": "101-200"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE": dvs_acl.get_acl_range_comparator("SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE", "101,200")
        }

        dvs_acl.create_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleVlanId(self, dvs_acl, l3v6_acl_table):
        config_qualifiers = {"VLAN_ID": "100"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_OUTER_VLAN_ID": dvs_acl.get_simple_qualifier_comparator("100&mask:0xfff")
        }

        dvs_acl.create_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_InsertAclRuleBetweenPriorities(self, dvs_acl, l3_acl_table):
        rule_priorities = ["10", "20", "30", "40"]

        config_qualifiers = {
            "10": {"SRC_IP": "10.0.0.0/32"},
            "20": {"DST_IP": "104.44.94.0/23"},
            "30": {"DST_IP": "192.168.0.16/32"},
            "40": {"DST_IP": "100.64.0.0/10"},
        }

        config_actions = {
            "10": "DROP",
            "20": "DROP",
            "30": "DROP",
            "40": "FORWARD",
        }

        expected_sai_qualifiers = {
            "10": {"SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP": dvs_acl.get_simple_qualifier_comparator("10.0.0.0&mask:255.255.255.255")},
            "20": {"SAI_ACL_ENTRY_ATTR_FIELD_DST_IP": dvs_acl.get_simple_qualifier_comparator("104.44.94.0&mask:255.255.254.0")},
            "30": {"SAI_ACL_ENTRY_ATTR_FIELD_DST_IP": dvs_acl.get_simple_qualifier_comparator("192.168.0.16&mask:255.255.255.255")},
            "40": {"SAI_ACL_ENTRY_ATTR_FIELD_DST_IP": dvs_acl.get_simple_qualifier_comparator("100.64.0.0&mask:255.192.0.0")},
        }

        for rule in rule_priorities:
            dvs_acl.create_acl_rule(L3_TABLE_NAME,
                                    f"PRIORITY_TEST_RULE_{rule}",
                                    config_qualifiers[rule], action=config_actions[rule],
                                    priority=rule)
            # Verify status is written into STATE_DB
            dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, f"PRIORITY_TEST_RULE_{rule}", "Active")

        dvs_acl.verify_acl_rule_set(rule_priorities, config_actions, expected_sai_qualifiers)

        odd_priority = "21"
        odd_rule = {"ETHER_TYPE": "4660"}
        odd_sai_qualifier = {"SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE": dvs_acl.get_simple_qualifier_comparator("4660&mask:0xffff")}

        rule_priorities.append(odd_priority)
        config_actions[odd_priority] = "DROP"
        expected_sai_qualifiers[odd_priority] = odd_sai_qualifier

        dvs_acl.create_acl_rule(L3_TABLE_NAME,
                                f"PRIORITY_TEST_RULE_{odd_priority}",
                                odd_rule,
                                action="DROP",
                                priority=odd_priority)
        dvs_acl.verify_acl_rule_set(rule_priorities, config_actions, expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, f"PRIORITY_TEST_RULE_{odd_priority}", "Active")
        for rule in rule_priorities:
            dvs_acl.remove_acl_rule(L3_TABLE_NAME, f"PRIORITY_TEST_RULE_{rule}")
            # Verify the STATE_DB entry is removed
            dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, f"PRIORITY_TEST_RULE_{rule}", None)
        dvs_acl.verify_no_acl_rules()

    def test_RulesWithDiffMaskLengths(self, dvs_acl, l3_acl_table):
        rule_priorities = ["10", "20", "30", "40", "50", "60"]

        config_qualifiers = {
            "10": {"SRC_IP": "23.103.0.0/18"},
            "20": {"SRC_IP": "104.44.94.0/23"},
            "30": {"DST_IP": "172.16.0.0/12"},
            "40": {"DST_IP": "100.64.0.0/10"},
            "50": {"DST_IP": "104.146.32.0/19"},
            "60": {"SRC_IP": "21.0.0.0/8"},
        }

        config_actions = {
            "10": "FORWARD",
            "20": "FORWARD",
            "30": "FORWARD",
            "40": "FORWARD",
            "50": "FORWARD",
            "60": "FORWARD",
        }

        expected_sai_qualifiers = {
            "10": {"SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP": dvs_acl.get_simple_qualifier_comparator("23.103.0.0&mask:255.255.192.0")},
            "20": {"SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP": dvs_acl.get_simple_qualifier_comparator("104.44.94.0&mask:255.255.254.0")},
            "30": {"SAI_ACL_ENTRY_ATTR_FIELD_DST_IP": dvs_acl.get_simple_qualifier_comparator("172.16.0.0&mask:255.240.0.0")},
            "40": {"SAI_ACL_ENTRY_ATTR_FIELD_DST_IP": dvs_acl.get_simple_qualifier_comparator("100.64.0.0&mask:255.192.0.0")},
            "50": {"SAI_ACL_ENTRY_ATTR_FIELD_DST_IP": dvs_acl.get_simple_qualifier_comparator("104.146.32.0&mask:255.255.224.0")},
            "60": {"SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP": dvs_acl.get_simple_qualifier_comparator("21.0.0.0&mask:255.0.0.0")},
        }

        for rule in rule_priorities:
            dvs_acl.create_acl_rule(L3_TABLE_NAME,
                                    f"MASK_TEST_RULE_{rule}",
                                    config_qualifiers[rule],
                                    action=config_actions[rule],
                                    priority=rule)
            # Verify status is written into STATE_DB
            dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, f"MASK_TEST_RULE_{rule}", "Active")
        dvs_acl.verify_acl_rule_set(rule_priorities, config_actions, expected_sai_qualifiers)

        for rule in rule_priorities:
            dvs_acl.remove_acl_rule(L3_TABLE_NAME, f"MASK_TEST_RULE_{rule}")
            # Verify the STATE_DB entry is removed
            dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, f"MASK_TEST_RULE_{rule}", None)
        dvs_acl.verify_no_acl_rules()

    def test_AclRuleIcmp(self, dvs_acl, l3_acl_table):
        config_qualifiers = {
            "ICMP_TYPE": "8",
            "ICMP_CODE": "9"
        }

        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ICMP_TYPE": dvs_acl.get_simple_qualifier_comparator("8&mask:0xff"),
            "SAI_ACL_ENTRY_ATTR_FIELD_ICMP_CODE": dvs_acl.get_simple_qualifier_comparator("9&mask:0xff")
        }

        dvs_acl.create_acl_rule(L3_TABLE_NAME, L3_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(L3_TABLE_NAME, L3_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

        dvs_acl.remove_acl_table(L3_TABLE_NAME)
        dvs_acl.verify_acl_table_count(0)

    def test_AclRuleIcmpV6(self, dvs_acl, l3v6_acl_table):
        config_qualifiers = {
            "ICMPV6_TYPE": "8",
            "ICMPV6_CODE": "9"
        }

        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_TYPE": dvs_acl.get_simple_qualifier_comparator("8&mask:0xff"),
            "SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_CODE": dvs_acl.get_simple_qualifier_comparator("9&mask:0xff")
        }

        dvs_acl.create_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME, config_qualifiers)
        dvs_acl.verify_acl_rule(expected_sai_qualifiers)
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, "Active")

        dvs_acl.remove_acl_rule(L3V6_TABLE_NAME, L3V6_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3V6_TABLE_NAME, L3V6_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

    def test_AclRuleRedirect(self, dvs, dvs_acl, l3_acl_table, setup_teardown_neighbor):
        config_qualifiers = {"L4_SRC_PORT": "65000"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT": dvs_acl.get_simple_qualifier_comparator("65000&mask:0xffff")
        }

        dvs_acl.create_redirect_acl_rule(L3_TABLE_NAME,
                                         L3_RULE_NAME,
                                         config_qualifiers,
                                         intf="Ethernet4",
                                         ip="10.0.0.2",
                                         priority="20")

        next_hop_id = setup_teardown_neighbor
        dvs_acl.verify_redirect_acl_rule(expected_sai_qualifiers, next_hop_id, priority="20")
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, "Active")
        dvs_acl.remove_acl_rule(L3_TABLE_NAME, L3_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()

        dvs_acl.create_redirect_acl_rule(L3_TABLE_NAME,
                                         L3_RULE_NAME,
                                         config_qualifiers,
                                         intf="Ethernet4",
                                         priority="20")

        intf_id = dvs.asic_db.port_name_map["Ethernet4"]
        dvs_acl.verify_redirect_acl_rule(expected_sai_qualifiers, intf_id, priority="20")
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, "Active")
        dvs_acl.remove_acl_rule(L3_TABLE_NAME, L3_RULE_NAME)
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, L3_RULE_NAME, None)
        dvs_acl.verify_no_acl_rules()
    
    def test_AclTableMandatoryMatchFields(self, dvs, pfcwd_acl_table):
        """
        The test case is to verify stage particular matching fields is applied
        """
        table_oid, stage = pfcwd_acl_table
        match_in_ports = False
        entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE", table_oid)
        for k, v in entry.items():
            if k == "SAI_ACL_TABLE_ATTR_FIELD_IN_PORTS" and v == "true":
                match_in_ports = True
        
        if stage == "ingress":
            assert match_in_ports
        else:
            assert not match_in_ports

    def test_AclTableMandatoryRangeFields(self, dvs, l3_acl_table):
        """
        The test case is to verify range qualifier is not applied for egress ACL
        """
        table_oid, stage = l3_acl_table
        match_range_qualifier = False
        entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE", table_oid)
        for k, v in entry.items():
            if k == "SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE" and v == "true":
                match_range_qualifier = True

        assert not match_range_qualifier

    def test_AclRulePriorityOrderProcessing(self, dvs, dvs_acl, l3_acl_table):
        """
        Test that ACL rules are processed in descending priority order (higher priority value first).
        """
        # Create rules with non-sequential priorities to test sorting
        rule_priorities = ["100", "50", "200", "10", "150"]

        config_qualifiers = {
            "100": {"SRC_IP": "10.0.0.100/32"},
            "50": {"SRC_IP": "10.0.0.50/32"},
            "200": {"SRC_IP": "10.0.0.200/32"},
            "10": {"SRC_IP": "10.0.0.10/32"},
            "150": {"SRC_IP": "10.0.0.150/32"},
        }

        config_actions = {
            "100": "DROP",
            "50": "DROP",
            "200": "DROP",
            "10": "DROP",
            "150": "DROP",
        }

        expected_sai_qualifiers = {
            "100": {"SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP": dvs_acl.get_simple_qualifier_comparator("10.0.0.100&mask:255.255.255.255")},
            "50": {"SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP": dvs_acl.get_simple_qualifier_comparator("10.0.0.50&mask:255.255.255.255")},
            "200": {"SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP": dvs_acl.get_simple_qualifier_comparator("10.0.0.200&mask:255.255.255.255")},
            "10": {"SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP": dvs_acl.get_simple_qualifier_comparator("10.0.0.10&mask:255.255.255.255")},
            "150": {"SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP": dvs_acl.get_simple_qualifier_comparator("10.0.0.150&mask:255.255.255.255")},
        }

        # Create all rules in CONFIG_DB atomically using redis transaction
        # This ensures all rules are batched together when they reach aclorch
        redis_cmds = []
        redis_cmds.append("MULTI")

        for priority in rule_priorities:
            rule_name = f"PRIORITY_ORDER_TEST_RULE_{priority}"
            key = f"{L3_TABLE_NAME}|{rule_name}"

            redis_cmds.append(f"HSET \"ACL_RULE|{key}\" \"priority\" \"{priority}\"")
            redis_cmds.append(f"HSET \"ACL_RULE|{key}\" \"PACKET_ACTION\" \"DROP\"")
            redis_cmds.append(f"HSET \"ACL_RULE|{key}\" \"SRC_IP\" \"10.0.0.{priority}/32\"")

        redis_cmds.append("EXEC")
        dvs.runcmd(['sh', '-c', f"redis-cli -n 4 <<EOF\n" + "\n".join(redis_cmds) + "\nEOF"])

        # Give time for config to propagate through the system
        time.sleep(3)

        # Verify all rules are created
        for priority in rule_priorities:
            dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, f"PRIORITY_ORDER_TEST_RULE_{priority}", "Active")

        # Verify all rules are created with correct priorities
        dvs_acl.verify_acl_rule_set(rule_priorities, config_actions, expected_sai_qualifiers)

        # Verify the order of SAI create operations from sairedis.rec
        exitcode, rec_output = dvs.runcmd('sh -c "tail -n 1000 /var/log/swss/sairedis.rec | grep \'c|SAI_OBJECT_TYPE_ACL_ENTRY\'"')

        # Parse the create operations and extract priorities
        created_priorities = []
        if exitcode == 0 and rec_output:
            for line in rec_output.strip().split('\n'):
                if '|c|SAI_OBJECT_TYPE_ACL_ENTRY' in line and 'SAI_ACL_ENTRY_ATTR_PRIORITY' in line:
                    for part in line.split('|'):
                        if 'SAI_ACL_ENTRY_ATTR_PRIORITY=' in part:
                            priority = part.split('=')[1].strip()
                            # Check if this is one of our test rules by looking for the SRC_IP
                            if any(f'10.0.0.{priority}' in line for priority in rule_priorities):
                                created_priorities.append(priority)
                            break

        # Verify that the creates happened in descending priority order
        expected_order = sorted([int(p) for p in rule_priorities], reverse=True)
        expected_order_str = [str(p) for p in expected_order]

        # The created_priorities should match expected_order (last 5 entries)
        actual_order = created_priorities[-5:]
        assert actual_order == expected_order_str, \
            f"ACL rules not created in priority order. Expected {expected_order_str}, got {actual_order}"

        # Clean up
        for priority in rule_priorities:
            dvs_acl.remove_acl_rule(L3_TABLE_NAME, f"PRIORITY_ORDER_TEST_RULE_{priority}")
            dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, f"PRIORITY_ORDER_TEST_RULE_{priority}", None)
        dvs_acl.verify_no_acl_rules()

    def test_AclRuleDeletionPriorityOrder(self, dvs, dvs_acl, l3_acl_table):
        """
        Test that ACL rules are deleted in ascending priority order (lower priority value first).
        """
        # Create rules with different priorities in random order
        rule_priorities = ["100", "50", "200", "10", "150"]

        config_qualifiers = {
            "100": {"DST_IP": "192.168.0.100/32"},
            "50": {"DST_IP": "192.168.0.50/32"},
            "200": {"DST_IP": "192.168.0.200/32"},
            "10": {"DST_IP": "192.168.0.10/32"},
            "150": {"DST_IP": "192.168.0.150/32"},
        }

        # Map to store OID for each priority (for SAI recording verification)
        priority_to_oid = {}
        # Create all rules
        for priority in rule_priorities:
            dvs_acl.create_acl_rule(L3_TABLE_NAME,
                                    f"DELETE_ORDER_TEST_RULE_{priority}",
                                    config_qualifiers[priority],
                                    action="FORWARD",
                                    priority=priority)
            dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, f"DELETE_ORDER_TEST_RULE_{priority}", "Active")

            # Get the OID for this rule from ASIC_DB for later verification
            keys = dvs_acl.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
            for key in keys:
                if key not in dvs_acl.asic_db.default_acl_entries:
                    entry = dvs_acl.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", key)
                    entry_priority = entry.get("SAI_ACL_ENTRY_ATTR_PRIORITY")
                    # Check if this is our rule by matching priority and DST_IP
                    expected_dst_ip = config_qualifiers[priority]["DST_IP"].split("/")[0]
                    entry_dst_ip = entry.get("SAI_ACL_ENTRY_ATTR_FIELD_DST_IP", "")
                    if entry_priority == priority and expected_dst_ip in entry_dst_ip:
                        # Extract OID from key (format is "oid:0x...")
                        priority_to_oid[priority] = key.split(":")[1]
                        break

        # Get initial count of ACL entries in ASIC_DB
        initial_count = len(dvs_acl.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY"))

        # Delete all rules atomically using Redis transaction in CONFIG_DB
        # This ensures all deletions are batched together and processed in one doTask call
        redis_cmds = []
        redis_cmds.append("MULTI")

        for priority in rule_priorities:
            rule_name = f"DELETE_ORDER_TEST_RULE_{priority}"
            key = f"{L3_TABLE_NAME}|{rule_name}"
            redis_cmds.append(f"DEL \"ACL_RULE|{key}\"")

        redis_cmds.append("EXEC")

        # Execute as a single atomic transaction in CONFIG_DB (database 4)
        dvs.runcmd(['sh', '-c', f"redis-cli -n 4 <<EOF\n" + "\n".join(redis_cmds) + "\nEOF"])

        # Wait for all rules to be deleted from ASIC_DB
        expected_final_count = initial_count - len(rule_priorities)
        dvs_acl.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", expected_final_count)

        # Verify all rules are deleted from CONFIG_DB and STATE_DB
        for priority in rule_priorities:
            dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, f"DELETE_ORDER_TEST_RULE_{priority}", None)
        dvs_acl.verify_no_acl_rules()

        # Verify the order of SAI remove operations from sairedis.rec
        exitcode, rec_output = dvs.runcmd('sh -c "tail -n 1000 /var/log/swss/sairedis.rec | grep \'r|SAI_OBJECT_TYPE_ACL_ENTRY\'"')

        # Parse the remove operations and extract OIDs
        removed_oids = []
        if exitcode == 0 and rec_output:
            for line in rec_output.strip().split('\n'):
                if '|r|SAI_OBJECT_TYPE_ACL_ENTRY:oid:' in line:
                    # Extract OID from line (format: timestamp|r|SAI_OBJECT_TYPE_ACL_ENTRY:oid:0x...)
                    oid = line.split('SAI_OBJECT_TYPE_ACL_ENTRY:oid:')[1].strip()
                    removed_oids.append(oid)

        # Find the sequence of our test rule removals by matching OIDs
        test_removal_sequence = []
        for oid in removed_oids[-10:]:  # Look at last 10 removals
            for priority, recorded_oid in priority_to_oid.items():
                if oid == recorded_oid:
                    test_removal_sequence.append(int(priority))
                    break

        expected_order = sorted([int(p) for p in rule_priorities])

        assert test_removal_sequence == expected_order, \
            f"ACL rules not removed in priority order. Expected {expected_order}, got {test_removal_sequence}"

    def test_AclRuleMixedPriorityOperations(self, dvs, dvs_acl, l3_acl_table):
        """
        Test mixed SET and DEL operations with different priorities.
        Verifies that DEL operations (ascending order) are processed before SET operations (descending order).
        """
        # First, create initial set of rules
        initial_priorities = ["100", "50", "150"]
        priority_to_oid = {}

        for priority in initial_priorities:
            dvs_acl.create_acl_rule(L3_TABLE_NAME,
                                    f"MIXED_OP_RULE_{priority}",
                                    {"SRC_IP": f"10.0.{priority}.0/32"},
                                    action="DROP",
                                    priority=priority)
            dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, f"MIXED_OP_RULE_{priority}", "Active")

            # Get the OID for this rule from ASIC_DB for later verification
            keys = dvs_acl.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
            for key in keys:
                if key not in dvs_acl.asic_db.default_acl_entries:
                    entry = dvs_acl.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", key)
                    entry_priority = entry.get("SAI_ACL_ENTRY_ATTR_PRIORITY")
                    # Check if this is our rule by matching priority and SRC_IP
                    expected_src_ip = f"10.0.{priority}.0"
                    entry_src_ip = entry.get("SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP", "")
                    if entry_priority == priority and expected_src_ip in entry_src_ip:
                        priority_to_oid[priority] = key.split(":")[1]
                        break

        # Perform bulk deletion and creation using Redis transaction
        redis_cmds = []
        redis_cmds.append("MULTI")

        delete_priorities = ["50", "150"]
        for priority in delete_priorities:
            rule_name = f"MIXED_OP_RULE_{priority}"
            key = f"{L3_TABLE_NAME}|{rule_name}"
            redis_cmds.append(f"DEL \"ACL_RULE|{key}\"")

        new_priorities = ["200", "25"]
        for priority in new_priorities:
            rule_name = f"MIXED_OP_RULE_{priority}"
            key = f"{L3_TABLE_NAME}|{rule_name}"
            redis_cmds.append(f"HSET \"ACL_RULE|{key}\" \"priority\" \"{priority}\"")
            redis_cmds.append(f"HSET \"ACL_RULE|{key}\" \"PACKET_ACTION\" \"DROP\"")
            redis_cmds.append(f"HSET \"ACL_RULE|{key}\" \"SRC_IP\" \"10.0.{priority}.0/32\"")

        redis_cmds.append("EXEC")
        dvs.runcmd(['sh', '-c', f"redis-cli -n 4 <<EOF\n" + "\n".join(redis_cmds) + "\nEOF"])

        # Wait for operations to complete
        time.sleep(2)
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, "MIXED_OP_RULE_50", None)
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, "MIXED_OP_RULE_150", None)
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, "MIXED_OP_RULE_100", "Active")
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, "MIXED_OP_RULE_200", "Active")
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, "MIXED_OP_RULE_25", "Active")

        # Verify the order of SAI operations from sairedis.rec
        exitcode, rec_output = dvs.runcmd('sh -c "tail -n 1000 /var/log/swss/sairedis.rec | grep \'SAI_OBJECT_TYPE_ACL_ENTRY\'"')

        # Parse operations and extract sequence
        operations = []
        if exitcode == 0 and rec_output:
            for line in rec_output.strip().split('\n'):
                # Look for remove operations
                if '|r|SAI_OBJECT_TYPE_ACL_ENTRY:oid:' in line:
                    oid = line.split('SAI_OBJECT_TYPE_ACL_ENTRY:oid:')[1].strip()
                    # Check if this is one of our deleted rules
                    for priority in delete_priorities:
                        if priority in priority_to_oid and oid == priority_to_oid[priority]:
                            operations.append(('DEL', int(priority)))
                            break
                # Look for create operations
                elif '|c|SAI_OBJECT_TYPE_ACL_ENTRY' in line and 'SAI_ACL_ENTRY_ATTR_PRIORITY' in line:
                    for part in line.split('|'):
                        if 'SAI_ACL_ENTRY_ATTR_PRIORITY=' in part:
                            priority = part.split('=')[1].strip()
                            # Check if this is one of our new rules
                            if priority in new_priorities and f'10.0.{priority}.0' in line:
                                operations.append(('SET', int(priority)))
                            break

        # Extract the last operations that match our test (2 DELs + 2 SETs = 4 operations)
        test_operations = operations[-4:]

        # Verify the order: DELs should be in ascending order (50, 150), SETs in descending order (200, 25)
        expected_operations = [('DEL', 50), ('DEL', 150), ('SET', 200), ('SET', 25)]

        assert test_operations == expected_operations, \
            f"Mixed operations not processed in correct order. Expected {expected_operations}, got {test_operations}"

        # Clean up
        dvs_acl.remove_acl_rule(L3_TABLE_NAME, "MIXED_OP_RULE_100")
        dvs_acl.remove_acl_rule(L3_TABLE_NAME, "MIXED_OP_RULE_200")
        dvs_acl.remove_acl_rule(L3_TABLE_NAME, "MIXED_OP_RULE_25")
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, "MIXED_OP_RULE_100", None)
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, "MIXED_OP_RULE_200", None)
        dvs_acl.verify_acl_rule_status(L3_TABLE_NAME, "MIXED_OP_RULE_25", None)

class TestAclCrmUtilization:
    @pytest.fixture(scope="class", autouse=True)
    def configure_crm_polling_interval_for_test(self, dvs):
        dvs.crm_poll_set("1")

        yield

        dvs.crm_poll_set("300")

    def test_ValidateAclTableBindingCrmUtilization(self, dvs, dvs_acl):
        counter_db = dvs.get_counters_db()

        crm_port_stats = counter_db.get_entry("CRM", "ACL_STATS:INGRESS:PORT")
        initial_acl_table_port_bindings_used = int(crm_port_stats.get("crm_stats_acl_table_used", 0))

        crm_lag_stats = counter_db.get_entry("CRM", "ACL_STATS:INGRESS:LAG")
        initial_acl_table_lag_bindings_used = int(crm_lag_stats.get("crm_stats_acl_table_used", 0))

        dvs_acl.create_acl_table(L3_TABLE_NAME, L3_TABLE_TYPE, L3_BIND_PORTS)
        dvs_acl.verify_acl_table_count(1)

        counter_db.wait_for_field_match(
            "CRM",
            "ACL_STATS:INGRESS:PORT",
            {"crm_stats_acl_table_used": str(initial_acl_table_port_bindings_used + 1)}
        )

        counter_db.wait_for_field_match(
            "CRM",
            "ACL_STATS:INGRESS:LAG",
            {"crm_stats_acl_table_used": str(initial_acl_table_lag_bindings_used + 1)}
        )

        dvs_acl.remove_acl_table(L3_TABLE_NAME)
        dvs_acl.verify_acl_table_count(0)

        counter_db.wait_for_field_match(
            "CRM",
            "ACL_STATS:INGRESS:PORT",
            {"crm_stats_acl_table_used": str(initial_acl_table_port_bindings_used)}
        )

        counter_db.wait_for_field_match(
            "CRM",
            "ACL_STATS:INGRESS:LAG",
            {"crm_stats_acl_table_used": str(initial_acl_table_lag_bindings_used)}
        )


# TODO: Need to improve the clean-up/post-checks for these tests as currently we can't run anything
# afterwards.
class TestAclRuleValidation:
    """Test class for cases that check if orchagent corectly validates ACL rules input."""

    ACL_STAGE_CAPABILITY_TABLE_NAME = "ACL_STAGE_CAPABILITY_TABLE"
    ACL_ACTION_LIST_FIELD_NAME = "action_list"

    def get_acl_actions_supported(self, dvs_acl, stage):
        switch = dvs_acl.state_db.wait_for_entry(self.ACL_STAGE_CAPABILITY_TABLE_NAME, stage.upper())
        supported_actions = switch.get(self.ACL_ACTION_LIST_FIELD_NAME, None)

        if supported_actions:
            supported_actions = supported_actions.split(",")
        else:
            supported_actions = []

        return supported_actions

    def test_AclActionValidation(self, dvs, dvs_acl):
        """
            The test overrides R/O SAI_SWITCH_ATTR_ACL_STAGE_INGRESS/EGRESS switch attributes
            to check the case when orchagent refuses to process rules with action that is not
            supported by the ASIC.
        """

        stage_name_map = {
            "ingress": "SAI_SWITCH_ATTR_ACL_STAGE_INGRESS",
            "egress": "SAI_SWITCH_ATTR_ACL_STAGE_EGRESS",
        }

        for stage in stage_name_map:
            action_values = self.get_acl_actions_supported(dvs_acl, stage)

            # virtual switch supports all actions
            assert action_values
            assert "PACKET_ACTION" in action_values

            sai_acl_stage = stage_name_map[stage]

            # mock switch attribute in VS so only REDIRECT action is supported on this stage
            dvs.setReadOnlyAttr("SAI_OBJECT_TYPE_SWITCH",
                                sai_acl_stage,
                                # FIXME: here should use sai_serialize_value() for acl_capability_t
                                #        but it is not available in VS testing infrastructure
                                "false:1:SAI_ACL_ACTION_TYPE_REDIRECT")

            # restart SWSS so orchagent will query updated switch attributes
            dvs.stop_swss()
            dvs.start_swss()
            # reinit ASIC DB validator object
            dvs.init_asic_db_validator()

            action_values = self.get_acl_actions_supported(dvs_acl, stage)
            # Now, PACKET_ACTION is not supported and REDIRECT_ACTION is supported
            assert "PACKET_ACTION" not in action_values
            assert "REDIRECT_ACTION" in action_values

            # try to create a forward rule
            try:
                dvs_acl.create_acl_table(L3_TABLE_NAME,
                                         L3_TABLE_TYPE,
                                         L3_BIND_PORTS,
                                         stage=stage)

                config_qualifiers = {
                    "ICMP_TYPE": "8"
                }

                dvs_acl.create_acl_rule(L3_TABLE_NAME, L3_RULE_NAME, config_qualifiers)
                dvs_acl.verify_no_acl_rules()
            finally:
                dvs_acl.remove_acl_rule(L3_TABLE_NAME, L3_RULE_NAME)
                dvs_acl.remove_acl_table(L3_TABLE_NAME)

                dvs.runcmd("supervisorctl restart syncd")
                dvs.stop_swss()
                dvs.start_swss()


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
