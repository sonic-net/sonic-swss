"""Utilities for interacting with ACLs when writing VS tests."""
from typing import Callable, Dict, List


class DVSAcl:
    """Manage ACL tables and rules on the virtual switch."""

    CDB_ACL_TABLE_NAME = "ACL_TABLE"
    CDB_ACL_TABLE_TYPE_NAME = "ACL_TABLE_TYPE"

    CDB_MIRROR_ACTION_LOOKUP = {
        "ingress": "MIRROR_INGRESS_ACTION",
        "egress": "MIRROR_EGRESS_ACTION"
    }

    ADB_ACL_TABLE_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE"
    ADB_ACL_GROUP_TABLE_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP"
    ADB_ACL_GROUP_MEMBER_TABLE_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER"
    ADB_ACL_COUNTER_TABLE_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_ACL_COUNTER"

    ADB_ACL_STAGE_LOOKUP = {
        "ingress": "SAI_ACL_STAGE_INGRESS",
        "egress": "SAI_ACL_STAGE_EGRESS"
    }

    ADB_PACKET_ACTION_LOOKUP = {
        "FORWARD": "SAI_PACKET_ACTION_FORWARD",
        "DROP": "SAI_PACKET_ACTION_DROP"
    }

    ADB_MIRROR_ACTION_LOOKUP = {
        "ingress": "SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS",
        "egress": "SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_EGRESS"
    }

    ADB_PORTCHANNEL_ATTR_LOOKUP = {
        "ingress": "SAI_LAG_ATTR_INGRESS_ACL",
        "egress": "SAI_LAG_ATTR_EGRESS_ACL"
    }

    ADB_PORT_ATTR_LOOKUP = {
        "ingress": "SAI_PORT_ATTR_INGRESS_ACL",
        "egress": "SAI_PORT_ATTR_EGRESS_ACL"
    }

    def __init__(self, asic_db, config_db, state_db, counters_db):
        """Create a new DVS ACL Manager."""
        self.asic_db = asic_db
        self.config_db = config_db
        self.state_db = state_db
        self.counters_db = counters_db

    def create_acl_table_type(
            self,
            name: str,
            matches: List[str],
            bpoint_types: List[str]
    ) -> None:
        """Create a new ACL table type in Config DB.

        Args:
            name: The name for the new ACL table type.
            matches: A list of matches to use in ACL table.
            bpoint_types: A list of bind point types to use in ACL table.
        """
        table_type_attrs = {
            "matches@": ",".join(matches),
            "bind_points@": ",".join(bpoint_types)
        }

        self.config_db.create_entry(self.CDB_ACL_TABLE_TYPE_NAME, name, table_type_attrs)

    def create_acl_table(
            self,
            table_name: str,
            table_type: str,
            ports: List[str],
            stage: str = None
    ) -> None:
        """Create a new ACL table in Config DB.

        Args:
            table_name: The name for the new ACL table.
            table_type: The type of table to create.
            ports: A list of ports to bind to the ACL table.
            stage: The stage for the ACL table. {ingress, egress}
        """
        table_attrs = {
            "policy_desc": table_name,
            "type": table_type,
            "ports@": ",".join(ports)
        }

        if stage:
            table_attrs["stage"] = stage

        self.config_db.create_entry(self.CDB_ACL_TABLE_NAME, table_name, table_attrs)

    def create_control_plane_acl_table(
            self,
            table_name: str,
            services: List[str]
    ) -> None:
        """Create a new Control Plane ACL table in Config DB.

        Args:
            table_name: The name for the new ACL table.
            services: A list of services to bind to the ACL table.
        """
        table_attrs = {
            "policy_desc": table_name,
            "type": "CTRLPLANE",
            "services": ",".join(services)
        }

        self.config_db.create_entry(self.CDB_ACL_TABLE_NAME, table_name, table_attrs)

    def update_acl_table_port_list(self, table_name: str, ports: List[str]) -> None:
        """Update the port binding list for a given ACL table.

        Args:
            table_name: The name of the ACL table to update.
            ports: The new list of ports to bind to the ACL table.
        """
        table_attrs = {"ports@": ",".join(ports)}
        self.config_db.update_entry(self.CDB_ACL_TABLE_NAME, table_name, table_attrs)

    def remove_acl_table(self, table_name: str) -> None:
        """Remove an ACL table from Config DB.

        Args:
            table_name: The name of the ACL table to delete.
        """
        self.config_db.delete_entry(self.CDB_ACL_TABLE_NAME, table_name)

    def remove_acl_table_type(self, name: str) -> None:
        """Remove an ACL table type from Config DB.

        Args:
            name: The name of the ACL table type to delete.
        """
        self.config_db.delete_entry(self.CDB_ACL_TABLE_TYPE_NAME, name)

    def get_acl_counter_ids(self, expected: int) -> List[str]:
        """Get all of the ACL counter IDs in ASIC DB.

        This method will wait for the expected number of counters to exist, or fail.

        Args:
            expected: The number of counters that are expected to be present in ASIC DB.

        Returns:
            The list of ACL counter IDs in ASIC DB.
        """
        return self.asic_db.wait_for_n_keys(self.ADB_ACL_COUNTER_TABLE_NAME, expected)

    def get_acl_table_ids(self, expected: int) -> List[str]:
        """Get all of the ACL table IDs in ASIC DB.

        This method will wait for the expected number of tables to exist, or fail.

        Args:
            expected: The number of tables that are expected to be present in ASIC DB.

        Returns:
            The list of ACL table IDs in ASIC DB.
        """
        num_keys = len(self.asic_db.default_acl_tables) + expected
        keys = self.asic_db.wait_for_n_keys(self.ADB_ACL_TABLE_NAME, num_keys)
        for k in self.asic_db.default_acl_tables:
            assert k in keys

        acl_tables = [k for k in keys if k not in self.asic_db.default_acl_tables]

        return acl_tables

    def verify_acl_table_count(self, expected: int) -> None:
        """Verify that some number of tables exists in ASIC DB.

        This method will wait for the expected number of tables to exist, or fail.

        Args:
            expected: The number of tables that are expected to be present in ASIC DB.
        """
        self.get_acl_table_ids(expected)

    def get_acl_table_group_ids(self, expected: int) -> List[str]:
        """Get all of the ACL group IDs in ASIC DB.

        This method will wait for the expected number of groups to exist, or fail.

        Args:
            expected: The number of groups that are expected to be present in ASIC DB.

        Returns:
            The list of ACL group IDs in ASIC DB.
        """
        acl_table_groups = self.asic_db.wait_for_n_keys(self.ADB_ACL_GROUP_TABLE_NAME, expected)
        return acl_table_groups

    # FIXME: This method currently assumes only ingress xor egress tables exist.
    def verify_acl_table_groups(self, expected: int, stage: str = "ingress") -> None:
        """Verify that the expected ACL table groups exist in ASIC DB.

        This method will wait for the expected number of groups to exist, or fail.

        Args:
            expected: The number of groups that are expected to be present in ASIC DB.
            stage: The stage of the ACL table that was created.
        """
        acl_table_groups = self.get_acl_table_group_ids(expected)

        for group in acl_table_groups:
            fvs = self.asic_db.wait_for_entry(self.ADB_ACL_GROUP_TABLE_NAME, group)
            for k, v in fvs.items():
                if k == "SAI_ACL_TABLE_GROUP_ATTR_ACL_STAGE":
                    assert v == self.ADB_ACL_STAGE_LOOKUP[stage]
                elif k == "SAI_ACL_TABLE_GROUP_ATTR_ACL_BIND_POINT_TYPE_LIST":
                    assert v == "1:SAI_ACL_BIND_POINT_TYPE_PORT"
                elif k == "SAI_ACL_TABLE_GROUP_ATTR_TYPE":
                    assert v == "SAI_ACL_TABLE_GROUP_TYPE_PARALLEL"
                else:
                    assert False

    def verify_acl_table_group_members(self, acl_table_id: str, acl_table_group_ids: str, num_tables: int) -> None:
        """Verify that the expected ACL table group members exist in ASIC DB.

        Args:
            acl_table_id: The ACL table that the group members belong to.
            acl_table_group_ids: The ACL table groups to check.
            num_tables: The total number of ACL tables in ASIC DB.
        """
        members = self.asic_db.wait_for_n_keys(self.ADB_ACL_GROUP_MEMBER_TABLE_NAME,
                                               len(acl_table_group_ids) * num_tables)

        member_groups = []
        for member in members:
            fvs = self.asic_db.wait_for_entry(self.ADB_ACL_GROUP_MEMBER_TABLE_NAME, member)
            group_id = fvs.get("SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID")
            table_id = fvs.get("SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID")

            if group_id in acl_table_group_ids and table_id == acl_table_id:
                member_groups.append(group_id)

        assert set(member_groups) == set(acl_table_group_ids)

    def verify_acl_table_portchannel_binding(
            self,
            acl_table_id: str,
            bind_portchannels: List[str],
            num_tables: int,
            stage: str = "ingress"
    ) -> None:
        """Verify that the ACL table has been bound to the given list of portchannels.

        Args:
            acl_table_id: The ACL table that is being checked.
            bind_portchannels: The portchannels that should be bound to the given ACL table.
            num_tables: The total number of ACL tables in ASIC DB.
            stage: The stage of the ACL table that was created.
        """
        acl_table_group_ids = self.asic_db.wait_for_n_keys(self.ADB_ACL_GROUP_TABLE_NAME, len(bind_portchannels))

        portchannel_groups = []
        for portchannel in bind_portchannels:
            portchannel_oid = self.counters_db.get_entry("COUNTERS_LAG_NAME_MAP", "").get(portchannel)
            fvs = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_LAG", portchannel_oid)

            acl_table_group_id = fvs.pop(self.ADB_PORTCHANNEL_ATTR_LOOKUP[stage], None)
            assert acl_table_group_id in acl_table_group_ids
            portchannel_groups.append(acl_table_group_id)

        assert len(portchannel_groups) == len(bind_portchannels)
        assert set(portchannel_groups) == set(acl_table_group_ids)

        self.verify_acl_table_group_members(acl_table_id, acl_table_group_ids, num_tables)

    def verify_acl_table_port_binding(
            self,
            acl_table_id: str,
            bind_ports: List[str],
            num_tables: int,
            stage: str = "ingress"
    ) -> None:
        """Verify that the ACL table has been bound to the given list of ports.

        Args:
            acl_table_id: The ACL table that is being checked.
            bind_ports: The ports that should be bound to the given ACL table.
            num_tables: The total number of ACL tables in ASIC DB.
            stage: The stage of the ACL table that was created.
        """
        acl_table_group_ids = self.asic_db.wait_for_n_keys(self.ADB_ACL_GROUP_TABLE_NAME, len(bind_ports))

        port_groups = []
        for port in bind_ports:
            port_oid = self.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "").get(port)
            fvs = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)

            acl_table_group_id = fvs.pop(self.ADB_PORT_ATTR_LOOKUP[stage], None)
            assert acl_table_group_id in acl_table_group_ids
            port_groups.append(acl_table_group_id)

        assert len(port_groups) == len(bind_ports)
        assert set(port_groups) == set(acl_table_group_ids)

        self.verify_acl_table_group_members(acl_table_id, acl_table_group_ids, num_tables)

    def create_acl_rule(
            self,
            table_name: str,
            rule_name: str,
            qualifiers: Dict[str, str],
            action: str = "FORWARD",
            priority: str = "2020"
    ) -> None:
        """Create a new ACL rule in the given table.

        Args:
            table_name: The name of the ACL table to add the rule to.
            rule_name: The name of the ACL rule.
            qualifiers: The list of qualifiers to add to the rule.
            action: The packet action of the rule.
            priority: The priority of the rule.
        """
        fvs = {
            "priority": priority,
            "PACKET_ACTION": action
        }

        for k, v in qualifiers.items():
            fvs[k] = v

        self.config_db.create_entry("ACL_RULE", "{}|{}".format(table_name, rule_name), fvs)

    def update_acl_rule(
            self,
            table_name: str,
            rule_name: str,
            qualifiers: Dict[str, str],
            action: str = "FORWARD",
            priority: str = "2020"
    ) -> None:
        """Create a new ACL rule in the given table.

        Args:
            table_name: The name of the ACL table to add the rule to.
            rule_name: The name of the ACL rule.
            qualifiers: The list of qualifiers to add to the rule.
            action: The packet action of the rule.
            priority: The priority of the rule.
        """
        fvs = {
            "priority": priority,
            "PACKET_ACTION": action
        }

        for k, v in qualifiers.items():
            fvs[k] = v

        self.config_db.update_entry("ACL_RULE", "{}|{}".format(table_name, rule_name), fvs)

    def create_redirect_acl_rule(
            self,
            table_name: str,
            rule_name: str,
            qualifiers: Dict[str, str],
            intf: str,
            ip: str = None,
            priority: str = "2020"
    ) -> None:
        """Create a new ACL redirect rule in the given table.

        Args:
            table_name: The name of the ACL table to add the rule to.
            rule_name: The name of the ACL rule.
            qualifiers: The list of qualifiers to add to the rule.
            intf: The interface to redirect packets to.
            ip: The IP to redirect packets to, if the redirect is a Next Hop.
            priority: The priority of the rule.
        """
        if ip:
            redirect_action = f"{ip}@{intf}"
        else:
            redirect_action = intf

        fvs = {
            "priority": priority,
            "REDIRECT_ACTION": redirect_action
        }

        for k, v in qualifiers.items():
            fvs[k] = v

        self.config_db.create_entry("ACL_RULE", "{}|{}".format(table_name, rule_name), fvs)

    def create_mirror_acl_rule(
            self,
            table_name: str,
            rule_name: str,
            qualifiers: Dict[str, str],
            session_name: str,
            stage: str = None,
            priority: str = "2020"
    ) -> None:
        """Create a new ACL mirror rule in the given table.

        Args:
            table_name: The name of the ACL table to add the rule to.
            rule_name: The name of the ACL rule.
            qualifiers: The list of qualifiers to add to the rule.
            session_name: The name of the session to mirror to.
            stage: The type of mirroring to use. {ingress, egress}
            priority: The priority of the rule.
        """
        if not stage:
            mirror_action = "MIRROR_ACTION"
        else:
            mirror_action = self.CDB_MIRROR_ACTION_LOOKUP[stage]

        fvs = {
            "priority": priority,
            mirror_action: session_name
        }

        for k, v in qualifiers.items():
            fvs[k] = v

        self.config_db.create_entry("ACL_RULE", "{}|{}".format(table_name, rule_name), fvs)

    def remove_acl_rule(self, table_name: str, rule_name: str) -> None:
        """Remove the ACL rule from the table.

        Args:
            table_name: The name of the table to remove the rule from.
            rule_name: The name of the rule to remove.
        """
        self.config_db.delete_entry("ACL_RULE", "{}|{}".format(table_name, rule_name))

    def verify_acl_rule_count(self, expected: int) -> None:
        """Verify that there are N rules in the ASIC DB."""
        num_keys = len(self.asic_db.default_acl_entries)
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", num_keys + expected)

    def verify_no_acl_rules(self) -> None:
        """Verify that there are no ACL rules in the ASIC DB."""
        num_keys = len(self.asic_db.default_acl_entries)
        keys = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", num_keys)
        assert set(keys) == set(self.asic_db.default_acl_entries)

    def verify_acl_rule(
            self,
            sai_qualifiers: Dict[str, str],
            action: str = "FORWARD",
            priority: str = "2020",
            acl_rule_id: str = None
    ) -> None:
        """Verify that an ACL rule has the correct ASIC DB representation.

        Args:
            sai_qualifiers: The expected set of SAI qualifiers to be found in ASIC DB.
            action: The type of PACKET_ACTION the given rule has.
            priority: The priority of the rule.
            acl_rule_id: A specific OID to check in ASIC DB. If left empty, this method
                         assumes that only one rule exists in ASIC DB.
        """
        if not acl_rule_id:
            acl_rule_id = self._get_acl_rule_id()

        fvs = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", acl_rule_id)
        self._check_acl_entry_base(fvs, sai_qualifiers, action, priority)
        self._check_acl_entry_packet_action(fvs, action)
        self._check_acl_entry_counters_map(acl_rule_id)

    def verify_redirect_acl_rule(
            self,
            sai_qualifiers: Dict[str, str],
            expected_destination: str,
            priority: str = "2020",
            acl_rule_id=None
    ) -> None:
        """Verify that an ACL redirect rule has the correct ASIC DB representation.

        Args:
            sai_qualifiers: The expected set of SAI qualifiers to be found in ASIC DB.
            expected_destination: Where we expect the rule to redirect to. This can be an interface or
                                  a next hop.
            priority: The priority of the rule.
            acl_rule_id: A specific OID to check in ASIC DB. If left empty, this method
                         assumes that only one rule exists in ASIC DB.
        """
        if not acl_rule_id:
            acl_rule_id = self._get_acl_rule_id()

        fvs = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", acl_rule_id)
        self._check_acl_entry_base(fvs, sai_qualifiers, "REDIRECT", priority)
        self._check_acl_entry_redirect_action(fvs, expected_destination)
        self._check_acl_entry_counters_map(acl_rule_id)

    def verify_nat_acl_rule(
            self,
            sai_qualifiers: Dict[str, str],
            priority: str = "2020",
            acl_rule_id=None
    ) -> None:
        """Verify that an ACL nat rule has the correct ASIC DB representation.

        Args:
            sai_qualifiers: The expected set of SAI qualifiers to be found in ASIC DB.
            priority: The priority of the rule.
            acl_rule_id: A specific OID to check in ASIC DB. If left empty, this method
                         assumes that only one rule exists in ASIC DB.
        """
        if not acl_rule_id:
            acl_rule_id = self._get_acl_rule_id()

        fvs = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", acl_rule_id)
        self._check_acl_entry_base(fvs, sai_qualifiers, "DO_NOT_NAT", priority)
        self._check_acl_entry_counters_map(acl_rule_id)

    def verify_mirror_acl_rule(
            self,
            sai_qualifiers: Dict[str, str],
            session_oid: str,
            stage: str = "ingress",
            priority: str = "2020",
            acl_rule_id: str = None
    ) -> None:
        """Verify that an ACL mirror rule has the correct ASIC DB representation.

        Args:
            sai_qualifiers: The expected set of SAI qualifiers to be found in ASIC DB.
            session_oid: The OID of the mirror session this rule is using.
            stage: What stage/type of mirroring this rule is using.
            priority: The priority of the rule.
            acl_rule_id: A specific OID to check in ASIC DB. If left empty, this method
                         assumes that only one rule exists in ASIC DB.
        """
        if not acl_rule_id:
            acl_rule_id = self._get_acl_rule_id()

        fvs = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", acl_rule_id)
        self._check_acl_entry_base(fvs, sai_qualifiers, "MIRROR", priority)
        self._check_acl_entry_mirror_action(fvs, session_oid, stage)
        self._check_acl_entry_counters_map(acl_rule_id)

    def verify_acl_rule_generic(
            self,
            sai_qualifiers: Dict[str, str],
            acl_table_id: str = None,
            acl_rule_id: str = None
    ) -> None:
        """Verify that an ACL rule has the correct ASIC DB representation.

        Args:
            sai_qualifiers: The expected set of SAI qualifiers to be found in ASIC DB.
            acl_table_id: A specific OID to check in ASIC DB. If left empty, this method
                         assumes that only one table exists in ASIC DB.
            acl_rule_id: A specific OID to check in ASIC DB. If left empty, this method
                         assumes that only one rule exists in ASIC DB.
        """
        if not acl_table_id:
            acl_table_id = self.get_acl_table_ids(1)[0]

        if not acl_rule_id:
            acl_rule_id = self._get_acl_rule_id()

        entry = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", acl_rule_id)

        for k, v in entry.items():
            if k == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert v == acl_table_id
            elif k == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert v == "true"
            elif k in sai_qualifiers:
                assert sai_qualifiers[k](v)
            else:
                assert False, "Unknown SAI qualifier: key={}, value={}".format(k, v)

    def verify_acl_rule_set(
            self,
            priorities: List[str],
            in_actions: Dict[str, str],
            expected: Dict[str, Dict[str, str]]
    ) -> None:
        """Verify that a set of rules with PACKET_ACTIONs have the correct ASIC DB representation.

        Args:
            priorities: A list of the priorities of each rule to be checked.
            in_actions: The type of action each rule has, keyed by priority.
            expected: The expected SAI qualifiers for each rule, keyed by priority.
        """
        num_keys = len(self.asic_db.default_acl_entries) + len(priorities)
        keys = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", num_keys)

        acl_entries = [k for k in keys if k not in self.asic_db.default_acl_entries]
        for entry in acl_entries:
            rule = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", entry)
            priority = rule.get("SAI_ACL_ENTRY_ATTR_PRIORITY", None)
            assert priority in priorities
            self.verify_acl_rule(expected[priority], in_actions[priority], priority, entry)

    # FIXME: This `get_x_comparator` abstraction is a bit clunky, we should try to improve this later.
    def get_simple_qualifier_comparator(self, expected_qualifier: str) -> Callable[[str], bool]:
        """Generate a method that compares if a given SAI qualifer matches `expected_qualifier`.

        Args:
            expected_qualifier: The SAI qualifier that the generated method will check against.

        Returns:
            A method that will compare qualifiers to `expected_qualifier`.
        """
        def _match_qualifier(sai_qualifier):
            return expected_qualifier == sai_qualifier

        return _match_qualifier

    def get_port_list_comparator(self, expected_ports: List[str]) -> Callable[[str], bool]:
        """Generate a method that compares if a list of SAI ports matches the ports from `expected_ports`.

        Args:
            expected_ports: The port list that the generated method will check against.

        Returns:
            A method that will compare SAI port lists against `expected_ports`.
        """
        def _match_port_list(sai_port_list):
            if not sai_port_list.startswith("{}:".format(len(expected_ports))):
                return False
            for port in expected_ports:
                if self.asic_db.port_name_map[port] not in sai_port_list:
                    return False

            return True

        return _match_port_list

    def get_acl_range_comparator(self, expected_type: str, expected_ports: str) -> Callable[[str], bool]:
        """Generate a method that compares if a SAI range object matches the range from `expected ports`.

        Args:
            expected_type: The expected type of range.
            expected_ports: A range of numbers described as "lower_bound,upper_bound".

        Returns:
            A method that will compare SAI ranges against `expected_type` and `expected_ports`.
        """
        def _match_acl_range(sai_acl_range):
            range_id = sai_acl_range.split(":", 1)[1]
            fvs = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_RANGE", range_id)
            for k, v in fvs.items():
                if k == "SAI_ACL_RANGE_ATTR_TYPE" and v == expected_type:
                    continue
                elif k == "SAI_ACL_RANGE_ATTR_LIMIT" and v == expected_ports:
                    continue
                else:
                    return False

            return True

        return _match_acl_range

    def _get_acl_rule_id(self) -> str:
        num_keys = len(self.asic_db.default_acl_entries) + 1
        keys = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", num_keys)

        acl_entries = [k for k in keys if k not in self.asic_db.default_acl_entries]
        return acl_entries[0]

    def _check_acl_entry_base(
            self,
            entry: Dict[str, str],
            qualifiers: Dict[str, str],
            action: str, priority: str
    ) -> None:
        acl_table_id = self.get_acl_table_ids(1)[0]

        for k, v in entry.items():
            if k == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert v == acl_table_id
            elif k == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert v == "true"
            elif k == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert v == priority
            elif k == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif k == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert action in self.ADB_PACKET_ACTION_LOOKUP
            elif k == "SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT":
                assert action == "REDIRECT"
            elif "SAI_ACL_ENTRY_ATTR_ACTION_MIRROR" in k:
                assert action == "MIRROR"
            elif "SAI_ACL_ENTRY_ATTR_ACTION_NO_NAT" in k:
                assert action == "DO_NOT_NAT"
                assert v == "true"
            elif k in qualifiers:
                assert qualifiers[k](v)
            else:
                assert False

    def _check_acl_entry_packet_action(self, entry: Dict[str, str], action: str) -> None:
        assert "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION" in entry
        assert self.ADB_PACKET_ACTION_LOOKUP.get(action, None) == entry["SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION"]

    def _check_acl_entry_redirect_action(self, entry: Dict[str, str], expected_destination: str) -> None:
        assert entry.get("SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT", None) == expected_destination

    def _check_acl_entry_mirror_action(self, entry: Dict[str, str], session_oid: str, stage: str) -> None:
        assert stage in self.ADB_MIRROR_ACTION_LOOKUP
        assert entry.get(self.ADB_MIRROR_ACTION_LOOKUP[stage]) == session_oid

    def _check_acl_entry_counters_map(self, acl_entry_oid: str):
        entry = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", acl_entry_oid)
        counter_oid = entry.get("SAI_ACL_ENTRY_ATTR_ACTION_COUNTER")
        if counter_oid is None:
            return
        rule_to_counter_map = self.counters_db.get_entry("ACL_COUNTER_RULE_MAP", "")
        counter_to_rule_map = {v: k for k, v in rule_to_counter_map.items()}
        assert counter_oid in counter_to_rule_map
