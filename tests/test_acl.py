import time
import pytest

@pytest.mark.usefixtures('dvs_acl_manager')
class TestAcl():
    def test_AclTableCreation(self, dvs):

        bind_ports = ["Ethernet0", "Ethernet4"]
        self.dvs_acl.create_acl_table("test", "L3", bind_ports)

        self.dvs_acl.verify_acl_group_num(len(bind_ports))
        acl_group_ids = self.dvs_acl.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP", len(bind_ports))
        self.dvs_acl.verify_acl_group_member(acl_group_ids, self.dvs_acl.get_acl_table_id())
        self.dvs_acl.verify_acl_port_binding(bind_ports)

    def test_AclRuleL4SrcPort(self, dvs):

        config_qualifiers = {"L4_SRC_PORT": "65000"}
        expected_sai_qualifiers = {"SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT": self.dvs_acl.get_simple_qualifier_comparator("65000&mask:0xffff")}

        self.dvs_acl.create_acl_rule("test", "acl_test_rule", config_qualifiers)
        self.dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        self.dvs_acl.remove_acl_rule("test", "acl_test_rule")
        self.dvs_acl.verify_no_acl_rules()

    def test_AclRuleInOutPorts(self, dvs):

        config_qualifiers = {
            "IN_PORTS": "Ethernet0,Ethernet4",
            "OUT_PORTS": "Ethernet8,Ethernet12"
        }

        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS": self.dvs_acl.get_port_list_comparator(["Ethernet0", "Ethernet4"]),
            "SAI_ACL_ENTRY_ATTR_FIELD_OUT_PORTS": self.dvs_acl.get_port_list_comparator(["Ethernet8", "Ethernet12"])
        }

        self.dvs_acl.create_acl_rule("test", "acl_test_rule", config_qualifiers)
        self.dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        self.dvs_acl.remove_acl_rule("test", "acl_test_rule")
        self.dvs_acl.verify_no_acl_rules()

    def test_AclRuleInPortsNonExistingInterface(self, dvs):

        config_qualifiers = {
            "IN_PORTS": "FOO_BAR_BAZ"
        }

        self.dvs_acl.create_acl_rule("test", "acl_test_rule", config_qualifiers)

        self.dvs_acl.verify_no_acl_rules()
        self.dvs_acl.remove_acl_rule("test", "acl_test_rule")

    def test_AclRuleOutPortsNonExistingInterface(self, dvs):

        config_qualifiers = {
            "OUT_PORTS": "FOO_BAR_BAZ"
        }

        self.dvs_acl.create_acl_rule("test", "acl_test_rule", config_qualifiers)

        self.dvs_acl.verify_no_acl_rules()
        self.dvs_acl.remove_acl_rule("test", "acl_test_rule")

    def test_AclTableDeletion(self, dvs):

        self.dvs_acl.remove_acl_table("test")
        self.dvs_acl.verify_acl_table_count(0)

    def test_V6AclTableCreation(self, dvs):

        bind_ports = ["Ethernet0", "Ethernet4", "Ethernet8"]
        self.dvs_acl.create_acl_table("test_aclv6", "L3V6", bind_ports)

        self.dvs_acl.verify_acl_group_num(len(bind_ports))
        acl_group_ids = self.dvs_acl.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP", len(bind_ports))
        self.dvs_acl.verify_acl_group_member(acl_group_ids, self.dvs_acl.get_acl_table_id())
        self.dvs_acl.verify_acl_port_binding(bind_ports)

    def test_V6AclRuleIPv6Any(self, dvs):

        config_qualifiers = {"IP_TYPE": "IPv6ANY"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE": self.dvs_acl.get_simple_qualifier_comparator("SAI_ACL_IP_TYPE_IPV6ANY&mask:0xffffffffffffffff")
        }

        self.dvs_acl.create_acl_rule("test_aclv6", "acl_test_rule", config_qualifiers)
        self.dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        self.dvs_acl.remove_acl_rule("test_aclv6", "acl_test_rule")
        self.dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleIPv6AnyDrop(self, dvs):

        config_qualifiers = {"IP_TYPE": "IPv6ANY"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE": self.dvs_acl.get_simple_qualifier_comparator("SAI_ACL_IP_TYPE_IPV6ANY&mask:0xffffffffffffffff")
        }

        self.dvs_acl.create_acl_rule("test_aclv6", "acl_test_rule", config_qualifiers, action="DROP")
        self.dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP")

        self.dvs_acl.remove_acl_rule("test_aclv6", "acl_test_rule")
        self.dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleIpProtocol(self, dvs):

        config_qualifiers = {"IP_PROTOCOL": "6"}
        expected_sai_qualifiers = {"SAI_ACL_ENTRY_ATTR_FIELD_IP_PROTOCOL": self.dvs_acl.get_simple_qualifier_comparator("6&mask:0xff")}

        self.dvs_acl.create_acl_rule("test_aclv6", "acl_test_rule", config_qualifiers)
        self.dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        self.dvs_acl.remove_acl_rule("test_aclv6", "acl_test_rule")
        self.dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleSrcIPv6(self, dvs):

        config_qualifiers = {"SRC_IPV6": "2777::0/64"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6": self.dvs_acl.get_simple_qualifier_comparator("2777::&mask:ffff:ffff:ffff:ffff::")
        }

        self.dvs_acl.create_acl_rule("test_aclv6", "acl_test_rule", config_qualifiers)
        self.dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        self.dvs_acl.remove_acl_rule("test_aclv6", "acl_test_rule")
        self.dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleDstIPv6(self, dvs):

        config_qualifiers = {"DST_IPV6": "2002::2/128"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_DST_IPV6": self.dvs_acl.get_simple_qualifier_comparator("2002::2&mask:ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff")
        }

        self.dvs_acl.create_acl_rule("test_aclv6", "acl_test_rule", config_qualifiers)
        self.dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        self.dvs_acl.remove_acl_rule("test_aclv6", "acl_test_rule")
        self.dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleL4SrcPort(self, dvs):

        config_qualifiers = {"L4_SRC_PORT": "65000"}
        expected_sai_qualifiers = {"SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT": self.dvs_acl.get_simple_qualifier_comparator("65000&mask:0xffff")}

        self.dvs_acl.create_acl_rule("test_aclv6", "acl_test_rule", config_qualifiers)
        self.dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        self.dvs_acl.remove_acl_rule("test_aclv6", "acl_test_rule")
        self.dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleL4DstPort(self, dvs):

        config_qualifiers = {"L4_DST_PORT": "65001"}
        expected_sai_qualifiers = {"SAI_ACL_ENTRY_ATTR_FIELD_L4_DST_PORT": self.dvs_acl.get_simple_qualifier_comparator("65001&mask:0xffff")}

        self.dvs_acl.create_acl_rule("test_aclv6", "acl_test_rule", config_qualifiers)
        self.dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        self.dvs_acl.remove_acl_rule("test_aclv6", "acl_test_rule")
        self.dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleTCPFlags(self, dvs):

        config_qualifiers = {"TCP_FLAGS": "0x07/0x3f"}
        expected_sai_qualifiers = {"SAI_ACL_ENTRY_ATTR_FIELD_TCP_FLAGS": self.dvs_acl.get_simple_qualifier_comparator("7&mask:0x3f")}

        self.dvs_acl.create_acl_rule("test_aclv6", "acl_test_rule", config_qualifiers)
        self.dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        self.dvs_acl.remove_acl_rule("test_aclv6", "acl_test_rule")
        self.dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleL4SrcPortRange(self, dvs):

        config_qualifiers = {"L4_SRC_PORT_RANGE": "1-100"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE": self.dvs_acl.get_acl_range_comparator("SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE", "1,100")
        }

        self.dvs_acl.create_acl_rule("test_aclv6", "acl_test_rule", config_qualifiers)
        self.dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        self.dvs_acl.remove_acl_rule("test_aclv6", "acl_test_rule")
        self.dvs_acl.verify_no_acl_rules()

    def test_V6AclRuleL4DstPortRange(self, dvs):

        config_qualifiers = {"L4_DST_PORT_RANGE": "101-200"}
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE": self.dvs_acl.get_acl_range_comparator("SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE", "101,200")
        }

        self.dvs_acl.create_acl_rule("test_aclv6", "acl_test_rule", config_qualifiers)
        self.dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        self.dvs_acl.remove_acl_rule("test_aclv6", "acl_test_rule")
        self.dvs_acl.verify_no_acl_rules()

    def test_V6AclTableDeletion(self, dvs):

        self.dvs_acl.remove_acl_table("test_aclv6")
        self.dvs_acl.verify_acl_table_count(0)

    def test_InsertAclRuleBetweenPriorities(self, dvs):

        bind_ports = ["Ethernet0", "Ethernet4"]
        self.dvs_acl.create_acl_table("test_priorities", "L3", bind_ports)

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
            "10": {"SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP": self.dvs_acl.get_simple_qualifier_comparator("10.0.0.0&mask:255.255.255.255")},
            "20": {"SAI_ACL_ENTRY_ATTR_FIELD_DST_IP": self.dvs_acl.get_simple_qualifier_comparator("104.44.94.0&mask:255.255.254.0")},
            "30": {"SAI_ACL_ENTRY_ATTR_FIELD_DST_IP": self.dvs_acl.get_simple_qualifier_comparator("192.168.0.16&mask:255.255.255.255")},
            "40": {"SAI_ACL_ENTRY_ATTR_FIELD_DST_IP": self.dvs_acl.get_simple_qualifier_comparator("100.64.0.0&mask:255.192.0.0")},
        }

        for rule in rule_priorities:
            self.dvs_acl.create_acl_rule("test_priorities", "acl_test_rule_{}".format(rule),
                                 config_qualifiers[rule], action=config_actions[rule],
                                 priority=rule)

        self.dvs_acl.verify_acl_rule_set(rule_priorities, config_actions, expected_sai_qualifiers)

        odd_priority = "21"
        odd_rule = {"ETHER_TYPE": "4660"}
        odd_sai_qualifier = {"SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE": self.dvs_acl.get_simple_qualifier_comparator("4660&mask:0xffff")}

        rule_priorities.append(odd_priority)
        config_actions[odd_priority] = "DROP"
        expected_sai_qualifiers[odd_priority] = odd_sai_qualifier

        self.dvs_acl.create_acl_rule("test_priorities", "acl_test_rule_{}".format(odd_priority),
                             odd_rule, action="DROP", priority=odd_priority)
        self.dvs_acl.verify_acl_rule_set(rule_priorities, config_actions, expected_sai_qualifiers)

        for rule in rule_priorities:
            self.dvs_acl.remove_acl_rule("test_priorities", "acl_test_rule_{}".format(rule))
        self.dvs_acl.verify_no_acl_rules()

        self.dvs_acl.remove_acl_table("test_priorities")
        self.dvs_acl.verify_acl_table_count(0)

    def test_RulesWithDiffMaskLengths(self, dvs):

        bind_ports = ["Ethernet0", "Ethernet4"]
        self.dvs_acl.create_acl_table("test_masks", "L3", bind_ports)

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
            "10": {"SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP": self.dvs_acl.get_simple_qualifier_comparator("23.103.0.0&mask:255.255.192.0")},
            "20": {"SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP": self.dvs_acl.get_simple_qualifier_comparator("104.44.94.0&mask:255.255.254.0")},
            "30": {"SAI_ACL_ENTRY_ATTR_FIELD_DST_IP": self.dvs_acl.get_simple_qualifier_comparator("172.16.0.0&mask:255.240.0.0")},
            "40": {"SAI_ACL_ENTRY_ATTR_FIELD_DST_IP": self.dvs_acl.get_simple_qualifier_comparator("100.64.0.0&mask:255.192.0.0")},
            "50": {"SAI_ACL_ENTRY_ATTR_FIELD_DST_IP": self.dvs_acl.get_simple_qualifier_comparator("104.146.32.0&mask:255.255.224.0")},
            "60": {"SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP": self.dvs_acl.get_simple_qualifier_comparator("21.0.0.0&mask:255.0.0.0")},
        }

        for rule in rule_priorities:
            self.dvs_acl.create_acl_rule("test_masks", "acl_test_rule_{}".format(rule),
                                 config_qualifiers[rule], action=config_actions[rule],
                                 priority=rule)
        self.dvs_acl.verify_acl_rule_set(rule_priorities, config_actions, expected_sai_qualifiers)

        for rule in rule_priorities:
            self.dvs_acl.remove_acl_rule("test_masks", "acl_test_rule_{}".format(rule))
        self.dvs_acl.verify_no_acl_rules()

        self.dvs_acl.remove_acl_table("test_masks")
        self.dvs_acl.verify_acl_table_count(0)

    def test_AclRuleIcmp(self, dvs):

        bind_ports = ["Ethernet0", "Ethernet4"]
        self.dvs_acl.create_acl_table("test_icmp", "L3", bind_ports)

        config_qualifiers = {
            "ICMP_TYPE": "8",
            "ICMP_CODE": "9"
        }

        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ICMP_TYPE": self.dvs_acl.get_simple_qualifier_comparator("8&mask:0xff"),
            "SAI_ACL_ENTRY_ATTR_FIELD_ICMP_CODE": self.dvs_acl.get_simple_qualifier_comparator("9&mask:0xff")
        }

        self.dvs_acl.create_acl_rule("test_icmp", "test_icmp_fields", config_qualifiers)
        self.dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        self.dvs_acl.remove_acl_rule("test_icmp", "test_icmp_fields")
        self.dvs_acl.verify_no_acl_rules()

        self.dvs_acl.remove_acl_table("test_icmp")
        self.dvs_acl.verify_acl_table_count(0)

    def test_AclRuleIcmpV6(self, dvs):

        bind_ports = ["Ethernet0", "Ethernet4"]
        self.dvs_acl.create_acl_table("test_icmpv6", "L3V6", bind_ports)

        config_qualifiers = {
            "ICMPV6_TYPE": "8",
            "ICMPV6_CODE": "9"
        }

        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_TYPE": self.dvs_acl.get_simple_qualifier_comparator("8&mask:0xff"),
            "SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_CODE": self.dvs_acl.get_simple_qualifier_comparator("9&mask:0xff")
        }

        self.dvs_acl.create_acl_rule("test_icmpv6", "test_icmpv6_fields", config_qualifiers)
        self.dvs_acl.verify_acl_rule(expected_sai_qualifiers)

        self.dvs_acl.remove_acl_rule("test_icmpv6", "test_icmpv6_fields")
        self.dvs_acl.verify_no_acl_rules()

        self.dvs_acl.remove_acl_table("test_icmpv6")
        self.dvs_acl.verify_acl_table_count(0)

    def test_AclRuleRedirectToNextHop(self, dvs):
        # NOTE: set_interface_status has a dependency on cdb within dvs,
        # so we still need to setup the db. This should be refactored.
        dvs.setup_db()

        # Bring up an IP interface with a neighbor
        dvs.set_interface_status("Ethernet4", "up")
        dvs.add_ip_address("Ethernet4", "10.0.0.1/24")
        dvs.add_neighbor("Ethernet4", "10.0.0.2", "00:01:02:03:04:05")

        next_hop_id = self.dvs_acl.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", 1)[0]

        bind_ports = ["Ethernet0"]
        self.dvs_acl.create_acl_table("test_redirect", "L3", bind_ports)

        config_qualifiers = {"L4_SRC_PORT": "65000"}
        expected_sai_qualifiers = {"SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT": self.dvs_acl.get_simple_qualifier_comparator("65000&mask:0xffff")}

        self.dvs_acl.create_acl_rule("test_redirect", "redirect_rule", config_qualifiers, action="REDIRECT:10.0.0.2@Ethernet4", priority="20")

        acl_rule_id = self.dvs_acl.get_acl_rule_id()
        entry = self.dvs_acl.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", acl_rule_id)
        self.dvs_acl._check_acl_entry(entry, expected_sai_qualifiers, "REDIRECT:10.0.0.2@Ethernet4", "20")
        assert entry.get("SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT", None) == next_hop_id

        self.dvs_acl.remove_acl_rule("test_redirect", "redirect_rule")
        self.dvs_acl.verify_no_acl_rules()

        self.dvs_acl.remove_acl_table("test_redirect")
        self.dvs_acl.verify_acl_table_count(0)

        # Clean up the IP interface and neighbor
        dvs.remove_neighbor("Ethernet4", "10.0.0.2")
        dvs.remove_ip_address("Ethernet4", "10.0.0.1/24")
        dvs.set_interface_status("Ethernet4", "down")

    def test_AclRedirectRule(self, dvs):        
        dvs.setup_db()

        # Bring up an IP interface with a neighbor
        dvs.set_interface_status("Ethernet4", "up")
        dvs.add_ip_address("Ethernet4", "10.0.0.1/24")
        dvs.add_neighbor("Ethernet4", "10.0.0.2", "00:01:02:03:04:05")

        next_hop_id = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", 1)[0]
            
        bind_ports = ["Ethernet0", "Ethernet8"]
        self.create_acl_table("test_acl_table", "L3", bind_ports)

        config_qualifiers = {"L4_SRC_PORT": "65000"}
        expected_sai_qualifiers = {"SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT": self.get_simple_qualifier_comparator("65000&mask:0xffff")}
        self.create_acl_rule("test_acl_table", "redirect_rule", config_qualifiers, action="REDIRECT:10.0.0.2@Ethernet4", priority="20")
        acl_rule_id = self.get_acl_rule_id()
        entry = self.dvs_acl.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", acl_rule_id)
        self._check_acl_entry(entry, expected_sai_qualifiers, "REDIRECT:10.0.0.2@Ethernet4", "20")
        assert entry.get("SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT", None) == next_hop_id

        self.remove_acl_rule("test_acl_table", "redirect_rule")

        self.create_redirect_action_acl_rule("test_acl_table", "redirect_action_rule", config_qualifiers, intf="Ethernet4", priority="20")
        acl_rule_id = self.get_acl_rule_id()
        entry = self.dvs_acl.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", acl_rule_id)
        self._check_acl_entry(entry, expected_sai_qualifiers, "Ethernet4", "20")
        self.remove_acl_rule("test_acl_table", "redirect_action_rule")
        self.verify_no_acl_rules()
        self.remove_acl_table("test_acl_table")
        self.verify_no_acl_tables()

@pytest.mark.usefixtures('dvs_acl_manager')
class TestAclRuleValidation():
    """
        Test class for cases that check if orchagent corectly validates
        ACL rules input
    """

    SWITCH_CAPABILITY_TABLE = "SWITCH_CAPABILITY"

    def get_acl_actions_supported(self, stage):
        switch_id = self.dvs_acl.state_db.wait_for_n_keys(self.SWITCH_CAPABILITY_TABLE, 1)[0]
        switch = self.dvs_acl.state_db.wait_for_entry(self.SWITCH_CAPABILITY_TABLE, switch_id)

        field = "ACL_ACTIONS|{}".format(stage.upper())

        supported_actions = switch.get(field, None)

        if supported_actions:
            supported_actions = supported_actions.split(",")
        else:
            supported_actions = []

        return supported_actions

    def test_AclActionValidation(self, dvs):
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
            action_values = self.get_acl_actions_supported(stage)

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
            dvs.init_asicdb_validator()

            action_values = self.get_acl_actions_supported(stage)
            # now, PACKET_ACTION is not supported
            # and REDIRECT_ACTION is supported
            assert "PACKET_ACTION" not in action_values
            assert "REDIRECT_ACTION" in action_values

            # try to create a forward rule

            acl_table = "TEST_TABLE"
            acl_rule = "TEST_RULE"

            bind_ports = ["Ethernet0", "Ethernet4"]

            self.dvs_acl.create_acl_table(acl_table, "L3", bind_ports, stage=stage)

            config_qualifiers = {
                "ICMP_TYPE": "8"
            }

            self.dvs_acl.create_acl_rule(acl_table, acl_rule, config_qualifiers)
            self.dvs_acl.verify_no_acl_rules()
            self.dvs_acl.remove_acl_rule(acl_table, acl_rule)

            self.dvs_acl.remove_acl_table(acl_table)

            dvs.runcmd("supervisorctl restart syncd")
            dvs.stop_swss()
            dvs.start_swss()
