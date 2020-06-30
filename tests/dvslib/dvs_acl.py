class DVSAcl(object):
    def __init__(self, adb, cdb, sdb, cntrdb):
        self.asic_db = adb
        self.config_db = cdb
        self.state_db = sdb
        self.counters_db = cntrdb

    def create_acl_table(self, table_name, table_type, ports, stage=None):
        table_attrs = {
            "policy_desc": "DVS acl table test",
            "type": table_type,
            "ports": ",".join(ports)
        }

        if stage:
            table_attrs["stage"] = stage

        self.config_db.create_entry("ACL_TABLE", table_name, table_attrs)

    def update_acl_table(self, acl_table_name, ports):
        table_attrs = {
            "ports": ",".join(ports)
        }
        self.config_db.update_entry("ACL_TABLE", acl_table_name, table_attrs)

    def remove_acl_table(self, table_name):
        self.config_db.delete_entry("ACL_TABLE", table_name)

    def get_acl_table_group_ids(self, expt):
        acl_table_groups = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP", expt)
        return acl_table_groups

    def get_acl_table_ids(self, expt=1):
        num_keys = len(self.asic_db.default_acl_tables) + expt
        keys = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE", num_keys)
        for k in self.asic_db.default_acl_tables:
            assert k in keys

        acl_tables = [k for k in keys if k not in self.asic_db.default_acl_tables]

        return acl_tables

    def get_acl_table_id(self):
        acl_tables = self.get_acl_table_ids()
        return acl_tables[0]

    def verify_acl_table_count(self, expt):
        num_keys = len(self.asic_db.default_acl_tables) + expt
        keys = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE", num_keys)
        for k in self.asic_db.default_acl_tables:
            assert k in keys

        acl_tables = [k for k in keys if k not in self.asic_db.default_acl_tables]

        assert len(acl_tables) == expt

    def verify_acl_group_num(self, expt):
        acl_table_groups = self.get_acl_table_group_ids(expt)

        for group in acl_table_groups:
            fvs = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP", group)
            for k, v in fvs.items():
                if k == "SAI_ACL_TABLE_GROUP_ATTR_ACL_STAGE":
                    assert v == "SAI_ACL_STAGE_INGRESS"
                elif k == "SAI_ACL_TABLE_GROUP_ATTR_ACL_BIND_POINT_TYPE_LIST":
                    assert v == "1:SAI_ACL_BIND_POINT_TYPE_PORT"
                elif k == "SAI_ACL_TABLE_GROUP_ATTR_TYPE":
                    assert v == "SAI_ACL_TABLE_GROUP_TYPE_PARALLEL"
                else:
                    assert False

    def verify_acl_table_group_member(self, acl_table_group_id, acl_table_id):
        self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP", acl_table_group_id)
        self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE", acl_table_id)
        members = self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER")
        for m in members:
            fvs = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER", m)
            fvs = dict(fvs)
            if (fvs.pop("SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID") == acl_table_group_id and
                    fvs.pop("SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID") == acl_table_id) :
                return True
        assert False

    def verify_acl_group_member(self, acl_group_ids, acl_table_id):
        members = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER", len(acl_group_ids))

        member_groups = []
        for member in members:
            fvs = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER", member)
            for k, v in fvs.items():
                if k == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID":
                    assert v in acl_group_ids
                    member_groups.append(v)
                elif k == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID":
                    assert v == acl_table_id
                elif k == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_PRIORITY":
                    assert True
                else:
                    assert False

        assert set(member_groups) == set(acl_group_ids)

    def verify_acl_table_ports_binding(self, ports, acl_table_id):
        for p in ports:
            # TBD: Introduce new API in dvs_databse.py to read by field
            fvs = self.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
            fvs = dict(fvs)
            port_oid = fvs.pop(p)
            #port_oid = self.counters_db.hget_entry("COUNTERS_PORT_NAME_MAP", "", p)
            fvs = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
            fvs = dict(fvs)
            acl_table_group_id = fvs.pop("SAI_PORT_ATTR_INGRESS_ACL")
            self.verify_acl_table_group_member(acl_table_group_id, acl_table_id)

    def verify_acl_port_binding(self, bind_ports):
        acl_table_groups = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP", len(bind_ports))

        port_groups = []
        for port in [self.asic_db.port_name_map[p] for p in bind_ports]:
            fvs = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port)
            acl_table_group = fvs.pop("SAI_PORT_ATTR_INGRESS_ACL", None)
            assert acl_table_group in acl_table_groups
            port_groups.append(acl_table_group)

        assert len(port_groups) == len(bind_ports)
        assert set(port_groups) == set(acl_table_groups)

    def create_acl_rule(self, table_name, rule_name, qualifiers, action="FORWARD", priority="2020"):
        fvs = {
            "priority": priority,
            "PACKET_ACTION": action
        }

        for k, v in qualifiers.items():
            fvs[k] = v

        self.config_db.create_entry("ACL_RULE", "{}|{}".format(table_name, rule_name), fvs)

    def create_mirror_acl_rule(self, table_name, rule_name, qualifiers, priority="2020"):
        fvs = {
            "priority": priority
        }

        for k, v in qualifiers.items():
            fvs[k] = v

        self.config_db.create_entry("ACL_RULE", "{}|{}".format(table_name, rule_name), fvs)

    def remove_acl_rule(self, table_name, rule_name):
        self.config_db.delete_entry("ACL_RULE", "{}|{}".format(table_name, rule_name))

    def get_acl_rule_id(self):
        num_keys = len(self.asic_db.default_acl_entries) + 1
        keys = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", num_keys)

        acl_entries = [k for k in keys if k not in self.asic_db.default_acl_entries]
        return acl_entries[0]

    def verify_no_acl_rules(self):
        num_keys = len(self.asic_db.default_acl_entries)
        keys = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", num_keys)
        assert set(keys) == set(self.asic_db.default_acl_entries)

    def verify_acl_rule(self, qualifiers, action="FORWARD", priority="2020"):
        acl_rule_id = self.get_acl_rule_id()

        fvs = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", acl_rule_id)
        self._check_acl_entry(fvs, qualifiers, action, priority)

    def verify_acl_rule_set(self, priorities, in_actions, expected):
        num_keys = len(self.asic_db.default_acl_entries) + len(priorities)
        keys = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", num_keys)

        acl_entries = [k for k in keys if k not in self.asic_db.default_acl_entries]
        for entry in acl_entries:
            rule = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY", entry)
            priority = rule.get("SAI_ACL_ENTRY_ATTR_PRIORITY", None)
            assert priority in priorities
            self._check_acl_entry(rule, expected[priority],
                                  action=in_actions[priority], priority=priority)

    def _check_acl_entry(self, entry, qualifiers, action, priority):
        acl_table_id = self.get_acl_table_id()

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
                if action == "FORWARD":
                    assert v == "SAI_PACKET_ACTION_FORWARD"
                elif action == "DROP":
                    assert v == "SAI_PACKET_ACTION_DROP"
                else:
                    assert False
            elif k == "SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT":
                if "REDIRECT" not in action:
                    assert False
            elif k in qualifiers:
                assert qualifiers[k](v)
            else:
                assert False

    def get_simple_qualifier_comparator(self, expected_qualifier):
        def _match_qualifier(sai_qualifier):
            return expected_qualifier == sai_qualifier

        return _match_qualifier

    def get_port_list_comparator(self, expected_ports):
        def _match_port_list(sai_port_list):
            if not sai_port_list.startswith("{}:".format(len(expected_ports))):
                return False
            for port in expected_ports:
                if self.asic_db.port_name_map[port] not in sai_port_list:
                    return False

            return True

        return _match_port_list

    def get_acl_range_comparator(self, expected_type, expected_ports):
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

