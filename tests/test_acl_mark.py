import pytest
from requests import request

OVERLAY_TABLE_TYPE = "UNDERLAY_SET_DSCP"
OVERLAY_TABLE_NAME = "OVERLAY_MARK_META_TEST"
OVERLAY_BIND_PORTS = ["Ethernet0", "Ethernet4", "Ethernet8", "Ethernet12"]
OVERLAY_RULE_NAME = "OVERLAY_TEST_RULE"

OVERLAY_TABLE_TYPE6 = "UNDERLAY_SET_DSCPV6"
OVERLAY_TABLE_NAME6 = "OVERLAY_MARK_META_TEST6"
OVERLAY_BIND_PORTS6 = ["Ethernet20", "Ethernet24", "Ethernet28", "Ethernet32"]
OVERLAY_RULE_NAME6 = "OVERLAY_TEST_RULE6"

# tests for UNDERLAY_SET_DSCP table


class TestAclMarkMeta:
    @pytest.fixture
    def overlay_acl_table(self, dvs_acl):
        try:
            dvs_acl.create_acl_table(OVERLAY_TABLE_NAME,
                                     OVERLAY_TABLE_TYPE,
                                     OVERLAY_BIND_PORTS)
            yield dvs_acl.get_acl_table_ids(2)
        finally:
            dvs_acl.remove_acl_table(OVERLAY_TABLE_NAME)
            dvs_acl.verify_acl_table_count(0)

    @pytest.fixture
    def overlay6_acl_table(self, dvs_acl):
        try:
            dvs_acl.create_acl_table(OVERLAY_TABLE_NAME6,
                                     OVERLAY_TABLE_TYPE6,
                                     OVERLAY_BIND_PORTS6)
            yield dvs_acl.get_acl_table_ids(2)
        finally:
            dvs_acl.remove_acl_table(OVERLAY_TABLE_NAME6)
            dvs_acl.verify_acl_table_count(0)

    def verify_acl_table_group_members_multitable(self, dvs_acl, acl_table_id, acl_table_group_ids, member_count):
        members = dvs_acl.asic_db.wait_for_n_keys(dvs_acl.ADB_ACL_GROUP_MEMBER_TABLE_NAME,
                                               member_count)

        member_groups = []
        table_member_map = {}
        for member in members:
            fvs = dvs_acl.asic_db.wait_for_entry(dvs_acl.ADB_ACL_GROUP_MEMBER_TABLE_NAME, member)
            group_id = fvs.get("SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID")
            table_id = fvs.get("SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID")

            if group_id in acl_table_group_ids and table_id in acl_table_id:
                member_groups.append(group_id)
                if table_id not in table_member_map:
                    table_member_map[table_id] = []
                table_member_map[table_id].append(group_id)

        assert set(member_groups) == set(acl_table_group_ids)
        return table_member_map

    def get_table_stage(self, dvs_acl, acl_table_id, v4_ports, v6_ports):
        stages = []
        names = []
        ports = []
        for table in acl_table_id:
            fvs = dvs_acl.asic_db.wait_for_entry(dvs_acl.ADB_ACL_TABLE_NAME, table)
            stage = fvs.get("SAI_ACL_TABLE_ATTR_ACL_STAGE")
            if stage == "SAI_ACL_STAGE_INGRESS":
                stages.append("ingress")
            elif stage == "SAI_ACL_STAGE_EGRESS":
                stages.append("egress")
            qual = fvs.get("SAI_ACL_TABLE_ATTR_FIELD_ACL_USER_META")
            if qual == "true":
                names.append("EGR_SET_DSCP")
                ports.append(v4_ports+v6_ports)
            qual = fvs.get("SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6")
            if qual == "true":
                names.append("MARK_META6")
                ports.append(v6_ports)
            qual = fvs.get("SAI_ACL_TABLE_ATTR_FIELD_DST_IP")
            if qual == "true":
                names.append("MARK_META")
                ports.append(v4_ports)
        return stages, names, ports

    def verify_acl_table_port_binding_multi(self, dvs_acl, table_member_map, bind_ports, stages, acl_table_id):
        for i in range(0, len(stages)):
            stage = stages[i]
            table = acl_table_id[i]
            port_groups = []
            for port in bind_ports[i]:
                port_oid = dvs_acl.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "").get(port)
                fvs = dvs_acl.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
                acl_table_group_id = fvs.pop(dvs_acl.ADB_PORT_ATTR_LOOKUP[stage], None)
                assert acl_table_group_id in table_member_map[table]
                port_groups.append(acl_table_group_id)

            assert len(port_groups) == len(bind_ports[i])
            assert set(port_groups) == set(table_member_map[table])


    def get_acl_rules_with_action(self, dvs_acl, total_rules):
        """Verify that there are N rules in the ASIC DB."""
        members = dvs_acl.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY",
                                               total_rules)

        member_groups = []
        table_member_map = {}
        for member in members:
            fvs = dvs_acl.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", member)
            table_id = fvs.get("SAI_ACL_ENTRY_ATTR_TABLE_ID")
            entry = {}
            entry['id'] = member
            action = fvs.get("SAI_ACL_ENTRY_ATTR_ACTION_SET_DSCP")
            if action:
                entry['action_type'] = "dscp"
                entry['action_value'] = action
                meta = fvs.get("SAI_ACL_ENTRY_ATTR_FIELD_ACL_USER_META")
                entry['match_meta'] = meta.split('&')[0]
            action = fvs.get("SAI_ACL_ENTRY_ATTR_ACTION_SET_ACL_META_DATA")
            if action:
                entry['action_type'] = "meta"
                entry['action_value'] = action

            if table_id not in table_member_map:
                table_member_map[table_id] = []
            table_member_map[table_id].append(entry)
        return table_member_map

    def verify_acl_rules_with_action(self, table_names, acl_table_id, table_rules, meta, dscp):
        for i in range(0, len(table_names)):
            if acl_table_id[i] in table_rules:
                for j in range(0, len(table_rules[acl_table_id[i]])):
                    if table_names[i] == "MARK_META" or table_names[i] == "MARK_META6":
                        assert table_rules[acl_table_id[i]][j]['action_type'] == "meta"
                        assert table_rules[acl_table_id[i]][j]['action_value'] in meta
                    else:
                        assert table_rules[acl_table_id[i]][j]['action_type'] == "dscp"
                        assert table_rules[acl_table_id[i]][j]['action_value'] in dscp
                        assert table_rules[acl_table_id[i]][j]['match_meta'] in meta

    def test_OverlayTableCreationDeletion(self, dvs_acl):
        try:
            dvs_acl.create_acl_table(OVERLAY_TABLE_NAME, OVERLAY_TABLE_TYPE, OVERLAY_BIND_PORTS)
            # this should create 2 tables. MARK_META and EGR_SET_DSCP Verify the table count.
            acl_table_id = dvs_acl.get_acl_table_ids(2)
            stages, names, ports = self.get_table_stage(dvs_acl, acl_table_id, OVERLAY_BIND_PORTS, [])

            acl_table_group_ids = dvs_acl.get_acl_table_group_ids(len(OVERLAY_BIND_PORTS)*2)
            table_member_map = self.verify_acl_table_group_members_multitable(dvs_acl, acl_table_id, acl_table_group_ids, 8)

            self.verify_acl_table_port_binding_multi(dvs_acl, table_member_map, ports, stages, acl_table_id)

            # Verify status is written into STATE_DB
            dvs_acl.verify_acl_table_status(OVERLAY_TABLE_NAME, "Active")
        finally:
            dvs_acl.remove_acl_table(OVERLAY_TABLE_NAME)
            dvs_acl.verify_acl_table_count(0)
            # Verify the STATE_DB entry is removed
            dvs_acl.verify_acl_table_status(OVERLAY_TABLE_NAME, None)

    def test_Overlay6TableCreationDeletion(self, dvs_acl):
        try:
            dvs_acl.create_acl_table(OVERLAY_TABLE_NAME6, OVERLAY_TABLE_TYPE6, OVERLAY_BIND_PORTS6)
           # this should create 2 tables. MARK_META and EGR_SET_DSCP Verify the table count.
            acl_table_id = dvs_acl.get_acl_table_ids(2)
            stages, names, ports = self.get_table_stage(dvs_acl, acl_table_id, [], OVERLAY_BIND_PORTS6)
            acl_table_group_ids = dvs_acl.get_acl_table_group_ids(len(OVERLAY_BIND_PORTS6)*2)
            table_member_map = self.verify_acl_table_group_members_multitable(dvs_acl, acl_table_id, acl_table_group_ids, 8)

            self.verify_acl_table_port_binding_multi(dvs_acl, table_member_map, ports, stages, acl_table_id)

            # Verify status is written into STATE_DB
            dvs_acl.verify_acl_table_status(OVERLAY_TABLE_NAME6, "Active")
        finally:
            dvs_acl.remove_acl_table(OVERLAY_TABLE_NAME6)
            dvs_acl.verify_acl_table_count(0)
            # Verify the STATE_DB entry is removed
            dvs_acl.verify_acl_table_status(OVERLAY_TABLE_NAME6, None)

    def test_OverlayBothv4v6TableCreationDeletion(self, dvs_acl):
        try:
            dvs_acl.create_acl_table(OVERLAY_TABLE_NAME, OVERLAY_TABLE_TYPE, OVERLAY_BIND_PORTS)
            dvs_acl.create_acl_table(OVERLAY_TABLE_NAME6, OVERLAY_TABLE_TYPE6, OVERLAY_BIND_PORTS6)
           # this should create 2 tables. MARK_META and EGR_SET_DSCP Verify the table count.
            acl_table_id = dvs_acl.get_acl_table_ids(3)
            stages, names, ports = self.get_table_stage(dvs_acl, acl_table_id,OVERLAY_BIND_PORTS, OVERLAY_BIND_PORTS6)
            acl_table_group_ids = dvs_acl.get_acl_table_group_ids(len(OVERLAY_BIND_PORTS6)*4)
            table_member_map = self.verify_acl_table_group_members_multitable(dvs_acl, acl_table_id, acl_table_group_ids, 16)

            self.verify_acl_table_port_binding_multi(dvs_acl, table_member_map, ports, stages, acl_table_id)

            # Verify status is written into STATE_DB
            dvs_acl.verify_acl_table_status(OVERLAY_TABLE_NAME, "Active")
            dvs_acl.verify_acl_table_status(OVERLAY_TABLE_NAME6, "Active")
        finally:
            dvs_acl.remove_acl_table(OVERLAY_TABLE_NAME)
            dvs_acl.verify_acl_table_count(2)
            # Verify the STATE_DB entry is removed
            dvs_acl.verify_acl_table_status(OVERLAY_TABLE_NAME, None)
            acl_table_id = dvs_acl.get_acl_table_ids(2)

            stages, names, ports = self.get_table_stage(dvs_acl, acl_table_id, [], OVERLAY_BIND_PORTS6)
            acl_table_group_ids = dvs_acl.get_acl_table_group_ids(len(OVERLAY_BIND_PORTS6)*2)
            table_member_map = self.verify_acl_table_group_members_multitable(dvs_acl, acl_table_id, acl_table_group_ids, 8)

            dvs_acl.remove_acl_table(OVERLAY_TABLE_NAME6)
            dvs_acl.verify_acl_table_count(0)
            # Verify the STATE_DB entry is removed
            dvs_acl.verify_acl_table_status(OVERLAY_TABLE_NAME6, None)

    def test_OverlayBothv4v6TableSameintfCreationDeletion(self, dvs_acl):
        try:
            dvs_acl.create_acl_table(OVERLAY_TABLE_NAME, OVERLAY_TABLE_TYPE, OVERLAY_BIND_PORTS)
            dvs_acl.create_acl_table(OVERLAY_TABLE_NAME6, OVERLAY_TABLE_TYPE6, OVERLAY_BIND_PORTS)
           # this should create 2 tables. MARK_META and EGR_SET_DSCP Verify the table count.
            acl_table_id = dvs_acl.get_acl_table_ids(3)
            stages, names, ports = self.get_table_stage(dvs_acl, acl_table_id,OVERLAY_BIND_PORTS, OVERLAY_BIND_PORTS)
            acl_table_group_ids = dvs_acl.get_acl_table_group_ids(len(OVERLAY_BIND_PORTS)*2)
            table_member_map = self.verify_acl_table_group_members_multitable(dvs_acl, acl_table_id, acl_table_group_ids, 12)

            self.verify_acl_table_port_binding_multi(dvs_acl, table_member_map, ports, stages, acl_table_id)

            # Verify status is written into STATE_DB
            dvs_acl.verify_acl_table_status(OVERLAY_TABLE_NAME, "Active")
            dvs_acl.verify_acl_table_status(OVERLAY_TABLE_NAME6, "Active")
        finally:
            dvs_acl.remove_acl_table(OVERLAY_TABLE_NAME)
            dvs_acl.verify_acl_table_count(2)
            # Verify the STATE_DB entry is removed
            dvs_acl.verify_acl_table_status(OVERLAY_TABLE_NAME, None)
            acl_table_id = dvs_acl.get_acl_table_ids(2)

            stages, names, ports = self.get_table_stage(dvs_acl, acl_table_id, [], OVERLAY_BIND_PORTS)
            acl_table_group_ids = dvs_acl.get_acl_table_group_ids(len(OVERLAY_BIND_PORTS)*2)
            table_member_map = self.verify_acl_table_group_members_multitable(dvs_acl, acl_table_id, acl_table_group_ids, 8)

            dvs_acl.remove_acl_table(OVERLAY_TABLE_NAME6)
            dvs_acl.verify_acl_table_count(0)
            # Verify the STATE_DB entry is removed
            dvs_acl.verify_acl_table_status(OVERLAY_TABLE_NAME6, None)

    def test_OverlayEntryCreationDeletion(self, dvs_acl, overlay_acl_table):
        config_qualifiers = {"DST_IP": "20.0.0.1/32",
                             "SRC_IP": "10.0.0.0/32"}
        acl_table_id = dvs_acl.get_acl_table_ids(2)
        _, names, _ = self.get_table_stage(dvs_acl, acl_table_id, [], OVERLAY_BIND_PORTS)
        dvs_acl.create_dscp_acl_rule(OVERLAY_TABLE_NAME, "VALID_RULE", config_qualifiers,action="12")
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "VALID_RULE", "Active")
        table_rules = self.get_acl_rules_with_action(dvs_acl, 2)
        self.verify_acl_rules_with_action(names, acl_table_id, table_rules, ["1"], ["12"])
        dvs_acl.remove_acl_rule(OVERLAY_TABLE_NAME, "VALID_RULE")
        # Verify the STATE_DB entry is removed
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "VALID_RULE", None)
        dvs_acl.verify_no_acl_rules()

    def test_OverlayEntryMultiRuleRef(self, dvs_acl, overlay_acl_table):
        config_qualifiers = {"DST_IP": "20.0.0.1/32",
                             "SRC_IP": "10.0.0.0/32",
                             "DSCP": "1"
                             }
        acl_table_id = dvs_acl.get_acl_table_ids(2)
        _, names, _ = self.get_table_stage(dvs_acl, acl_table_id, [], OVERLAY_BIND_PORTS)
        #create 1st Rule
        dvs_acl.create_dscp_acl_rule(OVERLAY_TABLE_NAME, "1", config_qualifiers, action="12")
        #create 2nd Rule
        config_qualifiers["DSCP"] = "2"
        dvs_acl.create_dscp_acl_rule(OVERLAY_TABLE_NAME, "2", config_qualifiers, action="12")
        #create 3rd Rule
        config_qualifiers["DSCP"] = "3"
        dvs_acl.create_dscp_acl_rule(OVERLAY_TABLE_NAME, "3", config_qualifiers, action="12")

        #This should create 4 rules 3 for MARK_META and 1 for EGR_SET_DSCP
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "1", "Active")
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "2", "Active")
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "3", "Active")
        table_rules = self.get_acl_rules_with_action(dvs_acl, 4)
        self.verify_acl_rules_with_action(names, acl_table_id, table_rules, ["1"], ["12"])

        # remove first rule. We should still have 3 rules, 2 for MARK_META and 1 for EGR_SET_DSCP
        dvs_acl.remove_acl_rule(OVERLAY_TABLE_NAME, "1")
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "1", None)
        table_rules = self.get_acl_rules_with_action(dvs_acl, 3)
        self.verify_acl_rules_with_action(names, acl_table_id, table_rules, ["1"], ["12"])
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "1", None)

        # remove 2nd rule. We should still have 2 rules, 1 for MARK_META and 1 for EGR_SET_DSCP
        dvs_acl.remove_acl_rule(OVERLAY_TABLE_NAME, "2")
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "2", None)
        table_rules = self.get_acl_rules_with_action(dvs_acl, 2)
        self.verify_acl_rules_with_action(names, acl_table_id, table_rules, ["1"], ["12"])
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "2", None)

        # Verify the STATE_DB entry is removed
        dvs_acl.remove_acl_rule(OVERLAY_TABLE_NAME, "3")
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "3", None)

        dvs_acl.verify_no_acl_rules()

    def test_OverlayEntryMultiTableRules(self, dvs_acl):
        config_qualifiers = {"DST_IP": "20.0.0.1/32",
                             "SRC_IP": "10.0.0.0/32",
                             "DSCP": "1"}
        dvs_acl.create_acl_table(OVERLAY_TABLE_NAME,
                                    OVERLAY_TABLE_TYPE,
                                    OVERLAY_BIND_PORTS)
        dvs_acl.create_acl_table(OVERLAY_TABLE_NAME6,
                                     OVERLAY_TABLE_TYPE6,
                                     OVERLAY_BIND_PORTS6)
        acl_table_id = dvs_acl.get_acl_table_ids(3)
        _, names, _ = self.get_table_stage(dvs_acl, acl_table_id, [], OVERLAY_BIND_PORTS)
        #create 1st Rule
        dvs_acl.create_dscp_acl_rule(OVERLAY_TABLE_NAME, "1", config_qualifiers, action="12")
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "1", "Active")

        #create 2nd Rule ipv6
        config_qualifiers6 = {"SRC_IPV6": "2777::0/64",
                             "DST_IPV6": "2788::0/64",
                             "DSCP" : "1"};
        dvs_acl.create_dscp_acl_rule(OVERLAY_TABLE_NAME6, "1", config_qualifiers6, action="12")

        # Verify status of both rules.
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "1", "Active")
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME6, "1", "Active")
        table_rules = self.get_acl_rules_with_action(dvs_acl, 3)
        self.verify_acl_rules_with_action(names, acl_table_id, table_rules, ["1"], ["12"])

        # remove first rule. We should still have 1 rule, 1 for MARK_META
        dvs_acl.remove_acl_rule(OVERLAY_TABLE_NAME6, "1")
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME6, "1", None)
        table_rules = self.get_acl_rules_with_action(dvs_acl, 2)
        self.verify_acl_rules_with_action(names, acl_table_id, table_rules, ["1"], ["12"])
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "1", "Active")

        # remove 2nd rule. We should still have 2 rules, 1 for MARK_META and 1 for EGR_SET_DSCP
        dvs_acl.remove_acl_rule(OVERLAY_TABLE_NAME, "1")
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "1", None)
        dvs_acl.verify_no_acl_rules()
        dvs_acl.remove_acl_table(OVERLAY_TABLE_NAME)
        dvs_acl.remove_acl_table(OVERLAY_TABLE_NAME6)
        dvs_acl.verify_acl_table_count(0)

    def test_OverlayEntryMultiMetaRule(self, dvs_acl, overlay_acl_table):
        config_qualifiers = {"DST_IP": "20.0.0.1/32",
                             "SRC_IP": "10.0.0.0/32",
                             "DSCP": "1"
                             }

        acl_table_id = dvs_acl.get_acl_table_ids(2)
        _, names, _ = self.get_table_stage(dvs_acl, acl_table_id, [], OVERLAY_BIND_PORTS)
        #create 1st Rule
        dvs_acl.create_dscp_acl_rule(OVERLAY_TABLE_NAME, "1", config_qualifiers, action="12")
        #create 2nd Rule
        config_qualifiers["DSCP"] = "2"
        dvs_acl.create_dscp_acl_rule(OVERLAY_TABLE_NAME, "2", config_qualifiers, action="13")
        #create 3rd Rule
        config_qualifiers["DSCP"] = "3"
        dvs_acl.create_dscp_acl_rule(OVERLAY_TABLE_NAME, "3", config_qualifiers, action="14")

        #This should create 4 rules 3 for MARK_META and 1 for EGR_SET_DSCP
        # Verify status is written into STATE_DB
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "1", "Active")
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "2", "Active")
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "3", "Active")
        table_rules = self.get_acl_rules_with_action(dvs_acl, 6)
        self.verify_acl_rules_with_action(names, acl_table_id, table_rules, ["1", "2", "3"], ["12", "13", "14"])

        # remove first rule. We should still have 3 rules, 2 for MARK_META and 1 for EGR_SET_DSCP
        dvs_acl.remove_acl_rule(OVERLAY_TABLE_NAME, "1")
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "1", None)
        table_rules = self.get_acl_rules_with_action(dvs_acl, 4)
        self.verify_acl_rules_with_action(names, acl_table_id, table_rules, ["1", "2", "3"], ["12", "13", "14"])
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "1", None)

        # remove 2nd rule. We should still have 2 rules, 1 for MARK_META and 1 for EGR_SET_DSCP
        dvs_acl.remove_acl_rule(OVERLAY_TABLE_NAME, "2")
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "2", None)
        table_rules = self.get_acl_rules_with_action(dvs_acl, 2)
        self.verify_acl_rules_with_action(names, acl_table_id, table_rules, ["1", "2", "3"], ["12", "13", "14"])
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "2", None)

        # Verify the STATE_DB entry is removed
        dvs_acl.remove_acl_rule(OVERLAY_TABLE_NAME, "3")
        dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, "3", None)

        dvs_acl.verify_no_acl_rules()

    def test_OverlayEntryExhaustMeta(self, dvs_acl, overlay_acl_table):
        config_qualifiers = {"DST_IP": "20.0.0.1/32",
                             "SRC_IP": "10.0.0.0/32",
                             "DSCP": "1"
                             }
        acl_table_id = dvs_acl.get_acl_table_ids(2)
        _, names, _ = self.get_table_stage(dvs_acl, acl_table_id, [], OVERLAY_BIND_PORTS)
        #create 8 rules. 8th one should fail.
        for i in range(1, 9):
            config_qualifiers["DSCP"] = str(i)
            dvs_acl.create_dscp_acl_rule(OVERLAY_TABLE_NAME, str(i), config_qualifiers, action=str(i+10))
            if i < 8:
                dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, str(i), "Active")
            else:
                dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, str(i), None)

        table_rules = self.get_acl_rules_with_action(dvs_acl, 14)
        meta = [str(i) for i in range(1, 8)]
        dscps = [str(i) for i in range(11, 18)]
        self.verify_acl_rules_with_action(names, acl_table_id, table_rules, meta, dscps)

        for i in range(1, 9):
            dvs_acl.remove_acl_rule(OVERLAY_TABLE_NAME, str(i))
            dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, str(i), None)
        dvs_acl.verify_no_acl_rules()

    def test_OverlayEntryTestMetaDataMgr(self, dvs_acl, overlay_acl_table):
        # allocate all 7 metadata values and free them multiple times.
        # At the end there should be no rules allocated.
        for i in range(1, 4):
            config_qualifiers = {"DST_IP": "20.0.0.1/32",
                                "SRC_IP": "10.0.0.0/32",
                                "DSCP": "1"
                                }
            acl_table_id = dvs_acl.get_acl_table_ids(2)
            _, names, _ = self.get_table_stage(dvs_acl, acl_table_id, [], OVERLAY_BIND_PORTS)
            #create 8 rules. 8th one should fail.
            for i in range(1, 9):
                config_qualifiers["DSCP"] = str(i)
                dvs_acl.create_dscp_acl_rule(OVERLAY_TABLE_NAME, str(i), config_qualifiers, action=str(i+10))
                if i < 8:
                    dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, str(i), "Active")
                else:
                    dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, str(i), None)

            table_rules = self.get_acl_rules_with_action(dvs_acl, 14)
            meta = [str(i) for i in range(1, 8)]
            dscps = [str(i) for i in range(11, 18)]
            self.verify_acl_rules_with_action(names, acl_table_id, table_rules, meta, dscps)

            for i in range(1, 9):
                dvs_acl.remove_acl_rule(OVERLAY_TABLE_NAME, str(i))
                dvs_acl.verify_acl_rule_status(OVERLAY_TABLE_NAME, str(i), None)
        dvs_acl.verify_no_acl_rules()

 # Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
