from swsscommon import swsscommon
import time
import re
import json


class BaseTestAcl(object):
    """ base class with helpers for Test classes """
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        self.sdb = swsscommon.DBConnector(6, dvs.redis_sock, 0)

    def create_acl_table(self, table, type, ports, stage=None):
        tbl = swsscommon.Table(self.cdb, "ACL_TABLE")
        table_props = [("policy_desc", "test"),
                       ("type", type),
                       ("ports", ",".join(ports))]

        if stage is not None:
            table_props += [("stage", stage)]

        fvs = swsscommon.FieldValuePairs(table_props)
        tbl.set(table, fvs)
        time.sleep(1)

    def remove_acl_table(self, table):
        tbl = swsscommon.Table(self.cdb, "ACL_TABLE")
        tbl._del(table)
        time.sleep(1)

    def get_acl_table_id(self, dvs):
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        keys = tbl.getKeys()

        for k in  dvs.asicdb.default_acl_tables:
            assert k in keys

        acl_tables = [k for k in keys if k not in dvs.asicdb.default_acl_tables]
        assert len(acl_tables) == 1

        return acl_tables[0]

    def verify_if_any_acl_table_created(self, dvs, adb):
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        keys = atbl.getKeys()
        for k in  dvs.asicdb.default_acl_tables:
            assert k in keys
        acl_tables = [k for k in keys if k not in dvs.asicdb.default_acl_tables]

        if len(acl_tables) != 0:
            return True

        return False

    def clean_up_left_over(self, dvs):
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        keys = atbl.getKeys()
        for key in keys:
            atbl._del(key)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        keys = atbl.getKeys()
        assert len(keys) == 0

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER")
        keys = atbl.getKeys()
        for key in keys:
            atbl._del(key)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER")
        keys = atbl.getKeys()
        assert len(keys) == 0

    def verify_acl_group_num(self, adb, expt):
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        acl_table_groups = atbl.getKeys()
        assert len(acl_table_groups) == expt

        for k in acl_table_groups:
            (status, fvs) = atbl.get(k)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_ACL_TABLE_GROUP_ATTR_ACL_STAGE":
                    assert fv[1] == "SAI_ACL_STAGE_INGRESS"
                elif fv[0] == "SAI_ACL_TABLE_GROUP_ATTR_ACL_BIND_POINT_TYPE_LIST":
                    assert fv[1] == "1:SAI_ACL_BIND_POINT_TYPE_PORT"
                elif fv[0] == "SAI_ACL_TABLE_GROUP_ATTR_TYPE":
                    assert fv[1] == "SAI_ACL_TABLE_GROUP_TYPE_PARALLEL"
                else:
                    assert False

    def verify_acl_group_member(self, adb, acl_group_ids, acl_table_id):
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER")
        keys = atbl.getKeys()

        member_groups = []
        for k in keys:
            (status, fvs) = atbl.get(k)
            assert status == True
            assert len(fvs) == 3
            for fv in fvs:
                if fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID":
                    assert fv[1] in acl_group_ids
                    member_groups.append(fv[1])
                elif fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID":
                    assert fv[1] == acl_table_id
                elif fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_PRIORITY":
                    assert True
                else:
                    assert False

        assert set(member_groups) == set(acl_group_ids)

    def verify_acl_port_binding(self, dvs, adb, bind_ports):
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        acl_table_groups = atbl.getKeys()
        assert len(acl_table_groups) == len(bind_ports)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        port_groups = []
        for p in [dvs.asicdb.portnamemap[portname] for portname in bind_ports]:
            (status, fvs) = atbl.get(p)
            for fv in fvs:
                if fv[0] == "SAI_PORT_ATTR_INGRESS_ACL":
                    assert fv[1] in acl_table_groups
                    port_groups.append(fv[1])

        assert len(port_groups) == len(bind_ports)
        assert set(port_groups) == set(acl_table_groups)

    def check_rule_existence(self, entry, rules, verifs):
        """ helper function to verify if rule exists """

        for rule in rules:
           ruleD = dict(rule)
           # find the rule to match with based on priority
           if ruleD["PRIORITY"] == entry['SAI_ACL_ENTRY_ATTR_PRIORITY']:
              ruleIndex = rules.index(rule)
              # use verification dictionary to match entry to rule
              for key in verifs[ruleIndex]:
                 assert verifs[ruleIndex][key] == entry[key]
              return True
        return False

    def create_acl_rule(self, table, rule, field, value):
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "666"),
                                          ("PACKET_ACTION", "FORWARD"),
                                          (field, value)])
        tbl.set(table + "|" + rule, fvs)
        time.sleep(1)

    def remove_acl_rule(self, table, rule):
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        tbl._del(table + "|" + rule)
        time.sleep(1)

    def verify_acl_rule(self, dvs, field, value):
        acl_table_id = self.get_acl_table_id(dvs)

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        acl_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entries) == 1

        (status, fvs) = tbl.get(acl_entries[0])
        assert status == True
        assert len(fvs) == 6
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "666"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_FORWARD"
            elif fv[0] == field:
                assert fv[1] == value
            else:
                assert False


class TestAcl(BaseTestAcl):
    def test_AclTableCreation(self, dvs, testlog):
        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        # create ACL_TABLE in config db
        bind_ports = ["Ethernet0", "Ethernet4"]
        tbl = swsscommon.Table(db, "ACL_TABLE")
        fvs = swsscommon.FieldValuePairs([("policy_desc", "test"), ("type", "L3"), ("ports", ",".join(bind_ports))])
        tbl.set("test", fvs)
        time.sleep(1)

        # check acl table in asic db
        test_acl_table_id = self.get_acl_table_id(dvs)
        assert test_acl_table_id

        # check acl table group in asic db
        self.verify_acl_group_num(adb, 2)

        # get acl table group ids and verify the id numbers
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        acl_group_ids = atbl.getKeys()
        assert len(acl_group_ids) == 2

        # check acl table group member
        self.verify_acl_group_member(adb, acl_group_ids, test_acl_table_id)

        # check port binding
        self.verify_acl_port_binding(dvs, adb, bind_ports)

    def test_AclRuleL4SrcPort(self, dvs, testlog):
        """
        hmset ACL_RULE|test|acl_test_rule priority 55 PACKET_ACTION FORWARD L4_SRC_PORT 65000
        """

        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        # create acl rule
        tbl = swsscommon.Table(db, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "55"), ("PACKET_ACTION", "FORWARD"), ("L4_SRC_PORT", "65000")])
        tbl.set("test|acl_test_rule", fvs)

        time.sleep(1)

        test_acl_table_id = self.get_acl_table_id(dvs)

        # check acl table in asic db
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == True
        assert len(fvs) == 6
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == test_acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "55"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT":
                assert fv[1] == "65000&mask:0xffff"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_FORWARD"
            else:
                assert False

        # remove acl rule
        tbl._del("test|acl_test_rule")

        time.sleep(1)

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False

    def test_AclRuleInOutPorts(self, dvs, testlog):
        """
        hmset ACL_RULE|test|acl_test_rule priority 55 PACKET_ACTION FORWARD IN_PORTS Ethernet0,Ethernet4 OUT_PORTS Ethernet8,Ethernet12
        """

        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        # create acl rule
        tbl = swsscommon.Table(db, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "55"),
                                          ("PACKET_ACTION", "FORWARD"),
                                          ("IN_PORTS", "Ethernet0,Ethernet4"),
                                          ("OUT_PORTS", "Ethernet8,Ethernet12")])
        tbl.set("test|acl_test_rule", fvs)

        time.sleep(1)

        test_acl_table_id = self.get_acl_table_id(dvs)

        # check acl table in asic db
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == True
        assert len(fvs) == 7
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == test_acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "55"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS":
                assert fv[1].startswith("2:")
                assert dvs.asicdb.portnamemap["Ethernet0"] in fv[1]
                assert dvs.asicdb.portnamemap["Ethernet4"] in fv[1]
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_OUT_PORTS":
                assert fv[1].startswith("2:")
                assert dvs.asicdb.portnamemap["Ethernet8"] in fv[1]
                assert dvs.asicdb.portnamemap["Ethernet12"] in fv[1]
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_FORWARD"
            else:
                assert False

        # remove acl rule
        tbl._del("test|acl_test_rule")

        time.sleep(1)

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False

    def test_AclRuleInPortsNonExistingInterface(self, dvs, testlog):
        self.setup_db(dvs)

        # Create ACL rule with a completely wrong interface
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "55"),
                                          ("PACKET_ACTION", "FORWARD"),
                                          ("IN_PORTS", "FOO_BAR_BAZ")])
        tbl.set("test|foo_bar_baz", fvs)
        time.sleep(1)

        # Make sure no rules were created
        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()
        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 0

        # Create ACL rule with a correct interface and a completely wrong interface
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "55"),
                                          ("PACKET_ACTION", "FORWARD"),
                                          ("IN_PORTS", "Ethernet0,FOO_BAR_BAZ")])
        tbl.set("test|foo_bar_baz", fvs)
        time.sleep(1)

        # Make sure no rules were created
        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()
        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 0

        # Delete rule
        tbl._del("test|foo_bar_baz")
        time.sleep(1)

    def test_AclRuleOutPortsNonExistingInterface(self, dvs, testlog):
        self.setup_db(dvs)

        # Create ACL rule with a completely wrong interface
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "55"),
                                          ("PACKET_ACTION", "FORWARD"),
                                          ("OUT_PORTS", "FOO_BAR_BAZ")])
        tbl.set("test|foo_bar_baz", fvs)
        time.sleep(1)

        # Make sure no rules were created
        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()
        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 0

        # Create ACL rule with a correct interface and a completely wrong interface
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "55"),
                                          ("PACKET_ACTION", "FORWARD"),
                                          ("OUT_PORTS", "Ethernet0,FOO_BAR_BAZ")])
        tbl.set("test|foo_bar_baz", fvs)
        time.sleep(1)

        # Make sure no rules were created
        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()
        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 0

        # Delete rule
        tbl._del("test|foo_bar_baz")
        time.sleep(1)

    def test_AclTableDeletion(self, dvs, testlog):
        self.setup_db(dvs)

        tbl = swsscommon.Table(self.cdb, "ACL_TABLE")
        tbl._del("test")

        time.sleep(1)

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        keys = atbl.getKeys()
        # only the default table was left along with DTel tables
        assert len(keys) >= 1

    def test_V6AclTableCreation(self, dvs, testlog):

        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        bind_ports = ["Ethernet0", "Ethernet4", "Ethernet8"]
        # create ACL_TABLE in config db
        tbl = swsscommon.Table(db, "ACL_TABLE")
        fvs = swsscommon.FieldValuePairs([("policy_desc", "testv6"), ("type", "L3V6"), ("ports", ",".join(bind_ports))])
        tbl.set("test-aclv6", fvs)

        time.sleep(1)

        # check acl table in asic db
        test_acl_table_id = self.get_acl_table_id(dvs)

        # check acl table group in asic db
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        acl_table_groups = atbl.getKeys()
        assert len(acl_table_groups) == 3

        for k in acl_table_groups:
            (status, fvs) = atbl.get(k)
            assert status == True

            for fv in fvs:
                if fv[0] == "SAI_ACL_TABLE_GROUP_ATTR_ACL_STAGE":
                    assert fv[1] == "SAI_ACL_STAGE_INGRESS"
                elif fv[0] == "SAI_ACL_TABLE_GROUP_ATTR_ACL_BIND_POINT_TYPE_LIST":
                    assert fv[1] == "1:SAI_ACL_BIND_POINT_TYPE_PORT"
                elif fv[0] == "SAI_ACL_TABLE_GROUP_ATTR_TYPE":
                    assert fv[1] == "SAI_ACL_TABLE_GROUP_TYPE_PARALLEL"
                else:
                    assert False

        # check acl table group member
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER")
        keys = atbl.getKeys()
        # three ports
        assert len(keys) == 3

        member_groups = []
        for k in keys:
            (status, fvs) = atbl.get(k)
            assert status == True

            assert len(fvs) == 3
            for fv in fvs:
                if fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID":
                    assert fv[1] in acl_table_groups
                    member_groups.append(fv[1])
                elif fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID":
                    assert fv[1] == test_acl_table_id
                elif fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_PRIORITY":
                    assert True
                else:
                    assert False

        assert set(member_groups) == set(acl_table_groups)

        # check port binding
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")

        port_groups = []
        for p in [dvs.asicdb.portnamemap[portname] for portname in bind_ports]:
            (status, fvs) = atbl.get(p)
            for fv in fvs:
                if fv[0] == "SAI_PORT_ATTR_INGRESS_ACL":
                    assert fv[1] in acl_table_groups
                    port_groups.append(fv[1])

        assert set(port_groups) == set(acl_table_groups)

    def test_V6AclRuleIPv6Any(self, dvs, testlog):
        """
        hmset ACL_RULE|test-aclv6|test_rule1 priority 1000 PACKET_ACTION FORWARD IPv6Any
        """

        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        # create acl rule
        tbl = swsscommon.Table(db, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "1001"), ("PACKET_ACTION", "FORWARD"), ("IP_TYPE", "IPv6ANY")])
        tbl.set("test-aclv6|test_rule1", fvs)

        time.sleep(1)

        test_acl_table_id = self.get_acl_table_id(dvs)

        # check acl table in asic db
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == True
        assert len(fvs) == 6
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == test_acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "1001"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE":
                assert fv[1] == "SAI_ACL_IP_TYPE_IPV6ANY&mask:0xffffffffffffffff"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_FORWARD"
            else:
                assert False

        # remove acl rule
        tbl._del("test-aclv6|test_rule1")

        time.sleep(1)

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False

    def test_V6AclRuleIPv6AnyDrop(self, dvs, testlog):
        """
        hmset ACL_RULE|test-aclv6|test_rule2 priority 1002 PACKET_ACTION DROP IPv6Any
        """

        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        # create acl rule
        tbl = swsscommon.Table(db, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "1002"), ("PACKET_ACTION", "DROP"), ("IP_TYPE", "IPv6ANY")])
        tbl.set("test-aclv6|test_rule2", fvs)

        time.sleep(1)

        test_acl_table_id = self.get_acl_table_id(dvs)

        # check acl table in asic db
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == True
        assert len(fvs) == 6
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == test_acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "1002"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE":
                assert fv[1] == "SAI_ACL_IP_TYPE_IPV6ANY&mask:0xffffffffffffffff"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_DROP"
            else:
                assert False

        # remove acl rule
        tbl._del("test-aclv6|test_rule2")

        time.sleep(1)

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False

    def test_V6AclRuleIpProtocol(self, dvs, testlog):
        """
        hmset ACL_RULE|test-aclv6|test_rule3 priority 1003 PACKET_ACTION DROP IP_PROTOCOL 6
        """

        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        # create acl rule
        tbl = swsscommon.Table(db, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "1003"), ("PACKET_ACTION", "DROP"), ("IP_PROTOCOL", "6")])
        tbl.set("test-aclv6|test_rule3", fvs)

        time.sleep(1)

        test_acl_table_id = self.get_acl_table_id(dvs)

        # check acl table in asic db
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == True
        assert len(fvs) == 6
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == test_acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "1003"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_IP_PROTOCOL":
                assert fv[1] == "6&mask:0xff"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_DROP"
            else:
                assert False

        # remove acl rule
        tbl._del("test-aclv6|test_rule3")

        time.sleep(1)

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False

    def test_V6AclRuleSrcIPv6(self, dvs, testlog):
        """
        hmset ACL_RULE|test-aclv6|test_rule4 priority 1004 PACKET_ACTION DROP SRC_IPV6 2777::0/64
        """

        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        # create acl rule
        tbl = swsscommon.Table(db, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "1004"), ("PACKET_ACTION", "DROP"), ("SRC_IPV6", "2777::0/64")])
        tbl.set("test-aclv6|test_rule4", fvs)

        time.sleep(1)

        test_acl_table_id = self.get_acl_table_id(dvs)

        # check acl table in asic db
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == True
        assert len(fvs) == 6
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == test_acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "1004"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_SRC_IPV6":
                assert fv[1] == "2777::&mask:ffff:ffff:ffff:ffff::"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_DROP"
            else:
                assert False

        # remove acl rule
        tbl._del("test-aclv6|test_rule4")

        time.sleep(1)

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False

    def test_V6AclRuleDstIPv6(self, dvs, testlog):
        """
        hmset ACL_RULE|test-aclv6|test_rule5 priority 1005 PACKET_ACTION DROP DST_IPV6 2002::2/128
        """

        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        # create acl rule
        tbl = swsscommon.Table(db, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "1005"), ("PACKET_ACTION", "DROP"), ("DST_IPV6", "2002::2/128")])
        tbl.set("test-aclv6|test_rule5", fvs)

        time.sleep(1)

        test_acl_table_id = self.get_acl_table_id(dvs)

        # check acl table in asic db
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == True
        assert len(fvs) == 6
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == test_acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "1005"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_DST_IPV6":
                assert fv[1] == "2002::2&mask:ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_DROP"
            else:
                assert False

        # remove acl rule
        tbl._del("test-aclv6|test_rule5")

        time.sleep(1)

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False

    def test_V6AclRuleL4SrcPort(self, dvs, testlog):
        """
        hmset ACL_RULE|test-aclv6|test_rule6 priority 1006 PACKET_ACTION DROP L4_SRC_PORT 65000
        """

        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        # create acl rule
        tbl = swsscommon.Table(db, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "1006"), ("PACKET_ACTION", "DROP"), ("L4_SRC_PORT", "65000")])
        tbl.set("test-aclv6|test_rule6", fvs)

        time.sleep(1)

        test_acl_table_id = self.get_acl_table_id(dvs)

        # check acl table in asic db
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == True
        assert len(fvs) == 6
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == test_acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "1006"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT":
                assert fv[1] == "65000&mask:0xffff"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_DROP"
            else:
                assert False

        # remove acl rule
        tbl._del("test-aclv6|test_rule6")

        time.sleep(1)

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False

    def test_V6AclRuleL4DstPort(self, dvs, testlog):
        """
        hmset ACL_RULE|test-aclv6|test_rule7 priority 1007 PACKET_ACTION DROP L4_DST_PORT 65001
        """

        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        # create acl rule
        tbl = swsscommon.Table(db, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "1007"), ("PACKET_ACTION", "DROP"), ("L4_DST_PORT", "65001")])
        tbl.set("test-aclv6|test_rule7", fvs)

        time.sleep(1)

        test_acl_table_id = self.get_acl_table_id(dvs)

        # check acl table in asic db
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == True
        assert len(fvs) == 6
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == test_acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "1007"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_L4_DST_PORT":
                assert fv[1] == "65001&mask:0xffff"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_DROP"
            else:
                assert False

        # remove acl rule
        tbl._del("test-aclv6|test_rule7")

        time.sleep(1)

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False

    def test_V6AclRuleTCPFlags(self, dvs, testlog):
        """
        hmset ACL_RULE|test-aclv6|test_rule8 priority 1008 PACKET_ACTION DROP TCP_FLAGS 0x7/0x3f
        """

        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        # create acl rule
        tbl = swsscommon.Table(db, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "1008"), ("PACKET_ACTION", "DROP"), ("TCP_FLAGS", "0x07/0x3f")])
        tbl.set("test-aclv6|test_rule8", fvs)

        time.sleep(1)

        test_acl_table_id = self.get_acl_table_id(dvs)

        # check acl table in asic db
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == True
        assert len(fvs) == 6
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == test_acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "1008"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_TCP_FLAGS":
                assert fv[1] == "7&mask:0x3f"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_DROP"
            else:
                assert False

        # remove acl rule
        tbl._del("test-aclv6|test_rule8")

        time.sleep(1)

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False

    def test_V6AclRuleL4SrcPortRange(self, dvs, testlog):
        """
        hmset ACL_RULE|test-aclv6|test_rule9 priority 1009 PACKET_ACTION DROP L4_SRC_PORT_RANGE 1-100
        """

        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        # create acl rule
        tbl = swsscommon.Table(db, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "1009"), ("PACKET_ACTION", "DROP"), ("L4_SRC_PORT_RANGE", "1-100")])
        tbl.set("test-aclv6|test_rule9", fvs)

        time.sleep(1)

        test_acl_table_id = self.get_acl_table_id(dvs)

        # check acl table in asic db
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == True
        assert len(fvs) == 6
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == test_acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "1009"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE":
                aclrange = fv[1]
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_DROP"
            else:
                assert False

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_RANGE")
        aclrange_obj = aclrange.split(":", 1)[1]

        (status, fvs) = atbl.get(aclrange_obj)
        assert status == True
        assert len(fvs) == 2
        for fv in fvs:
            if fv[0] == "SAI_ACL_RANGE_ATTR_TYPE":
                assert fv[1] == "SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE"
            elif fv[0] == "SAI_ACL_RANGE_ATTR_LIMIT":
                assert fv[1] == "1,100"
            else:
                assert False

        # remove acl rule
        tbl._del("test-aclv6|test_rule9")

        time.sleep(1)

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False

    def test_V6AclRuleL4DstPortRange(self, dvs, testlog):
        """
        hmset ACL_RULE|test-aclv6|test_rule10 priority 1010 PACKET_ACTION DROP L4_DST_PORT_RANGE 101-200
        """

        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        # create acl rule
        tbl = swsscommon.Table(db, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "1010"), ("PACKET_ACTION", "DROP"), ("L4_DST_PORT_RANGE", "101-200")])
        tbl.set("test-aclv6|test_rule10", fvs)

        time.sleep(1)

        test_acl_table_id = self.get_acl_table_id(dvs)

        # check acl table in asic db
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == True
        assert len(fvs) == 6
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == test_acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "1010"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE":
                aclrange = fv[1]
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_DROP"
            else:
                assert False

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_RANGE")
        aclrange_obj = aclrange.split(":", 1)[1]

        (status, fvs) = atbl.get(aclrange_obj)
        assert status == True
        assert len(fvs) == 2
        for fv in fvs:
            if fv[0] == "SAI_ACL_RANGE_ATTR_TYPE":
                assert fv[1] == "SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE"
            elif fv[0] == "SAI_ACL_RANGE_ATTR_LIMIT":
                assert fv[1] == "101,200"
            else:
                assert False

        # remove acl rule
        tbl._del("test-aclv6|test_rule10")

        time.sleep(1)

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False

    def test_V6AclTableDeletion(self, dvs, testlog):

        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        # get ACL_TABLE in config db
        tbl = swsscommon.Table(db, "ACL_TABLE")
        tbl._del("test-aclv6")

        time.sleep(1)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        keys = atbl.getKeys()
        # only the default table was left
        assert len(keys) >= 1

    def test_InsertAclRuleBetweenPriorities(self, dvs, testlog):
        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        bind_ports = ["Ethernet0", "Ethernet4"]
        # create ACL_TABLE in config db
        tbl = swsscommon.Table(db, "ACL_TABLE")
        fvs = swsscommon.FieldValuePairs([("policy_desc", "test"), ("type", "L3"), ("ports", ",".join(bind_ports))])
        tbl.set("test_insert", fvs)

        time.sleep(2)

        num_rules = 0
        #create ACL rules
        tbl = swsscommon.Table(db, "ACL_RULE")
        rules = [ [("PRIORITY", "10"), ("PACKET_ACTION", "DROP"), ("SRC_IP", "10.0.0.0/32")],
                  [("PRIORITY", "20"), ("PACKET_ACTION", "DROP"), ("DST_IP", "104.44.94.0/23")],
                  [("PRIORITY", "30"), ("PACKET_ACTION", "DROP"), ("DST_IP", "192.168.0.16/32")],
                  [("PRIORITY", "40"), ("PACKET_ACTION", "FORWARD"), ("DST_IP", "100.64.0.0/10")] ]
        #used to verify how ACL rules are programmed in ASICDB
        verifs = [ {'SAI_ACL_ENTRY_ATTR_PRIORITY': '10',
                    'SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP': '10.0.0.0&mask:255.255.255.255',
                    'SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION': 'SAI_PACKET_ACTION_DROP'},
                   {'SAI_ACL_ENTRY_ATTR_PRIORITY': '20',
                    'SAI_ACL_ENTRY_ATTR_FIELD_DST_IP': '104.44.94.0&mask:255.255.254.0',
                    'SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION': 'SAI_PACKET_ACTION_DROP'},
                   {'SAI_ACL_ENTRY_ATTR_PRIORITY': '30',
                    'SAI_ACL_ENTRY_ATTR_FIELD_DST_IP': '192.168.0.16&mask:255.255.255.255',
                    'SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION': 'SAI_PACKET_ACTION_DROP'},
                   {'SAI_ACL_ENTRY_ATTR_PRIORITY': '40',
                    'SAI_ACL_ENTRY_ATTR_FIELD_DST_IP': '100.64.0.0&mask:255.192.0.0',
                    'SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION': 'SAI_PACKET_ACTION_FORWARD'} ]
        #insert rules
        for rule in rules:
           fvs = swsscommon.FieldValuePairs(rule)
           num_rules += 1
           tbl.set( "test_insert|acl_test_rule%s" % num_rules, fvs)

        time.sleep(1)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        #assert that first set of rules are programmed
        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == num_rules

        #insert new rule with odd priority
        tbl = swsscommon.Table(db, "ACL_RULE")
        insertrule = [("PRIORITY", "21"), ("PACKET_ACTION", "DROP"), ("ETHER_TYPE", "4660")]
        #create verification for that rule
        verifs.append({'SAI_ACL_ENTRY_ATTR_PRIORITY': '21',
                       'SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE': '4660&mask:0xffff',
                       'SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION': 'SAI_PACKET_ACTION_DROP'})
        rules.append(insertrule)
        fvs = swsscommon.FieldValuePairs(insertrule)
        num_rules += 1
        tbl.set("test_insert|acl_test_rule%s" % num_rules, fvs)

        time.sleep(1)

        #assert all rules are programmed
        keys = atbl.getKeys()
        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == num_rules

        #match each entry to its corresponding verification
        matched_rules = 0
        for entry in acl_entry:
           (status, fvs) = atbl.get(entry)
           assert status == True
           assert len(fvs) == 6
           #helper function
           if self.check_rule_existence(dict(fvs), rules, verifs):
              matched_rules += 1

        assert num_rules == matched_rules

        #cleanup
        while num_rules > 0:
           tbl._del("test_insert|acl_test_rule%s" % num_rules)
           num_rules -= 1

        time.sleep(1)

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False

        tbl = swsscommon.Table(db, "ACL_TABLE")
        tbl._del("test_insert")

        time.sleep(1)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        keys = atbl.getKeys()
        # only the default table was left
        assert len(keys) >= 1

    def test_RulesWithDiffMaskLengths(self, dvs, testlog):
        self.setup_db(dvs)
        db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        bind_ports = ["Ethernet0", "Ethernet4"]
        # create ACL_TABLE in config db
        tbl = swsscommon.Table(db, "ACL_TABLE")
        fvs = swsscommon.FieldValuePairs([("policy_desc", "test"), ("type", "L3"), ("ports", ",".join(bind_ports))])
        tbl.set("test_subnet", fvs)

        time.sleep(2)

        subnet_mask_rules = 0
        #create ACL rules
        tbl = swsscommon.Table(db, "ACL_RULE")
        rules = [ [("PRIORITY", "10"), ("PACKET_ACTION", "FORWARD"), ("SRC_IP", "23.103.0.0/18")],
                  [("PRIORITY", "20"), ("PACKET_ACTION", "FORWARD"), ("SRC_IP", "104.44.94.0/23")],
                  [("PRIORITY", "30"), ("PACKET_ACTION", "FORWARD"), ("DST_IP", "172.16.0.0/12")],
                  [("PRIORITY", "40"), ("PACKET_ACTION", "FORWARD"), ("DST_IP", "100.64.0.0/10")],
                  [("PRIORITY", "50"), ("PACKET_ACTION", "FORWARD"), ("DST_IP", "104.146.32.0/19")],
                  [("PRIORITY", "60"), ("PACKET_ACTION", "FORWARD"), ("SRC_IP", "21.0.0.0/8")] ]
        #used to verify how ACL rules are programmed in ASICDB
        #order must match the list of rules
        verifs = [ {'SAI_ACL_ENTRY_ATTR_PRIORITY': '10',
                    'SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP': '23.103.0.0&mask:255.255.192.0',
                    'SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION': 'SAI_PACKET_ACTION_FORWARD'},
                   {'SAI_ACL_ENTRY_ATTR_PRIORITY': '20',
                    'SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP': '104.44.94.0&mask:255.255.254.0',
                    'SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION': 'SAI_PACKET_ACTION_FORWARD'},
                   {'SAI_ACL_ENTRY_ATTR_PRIORITY': '30',
                    'SAI_ACL_ENTRY_ATTR_FIELD_DST_IP': '172.16.0.0&mask:255.240.0.0',
                    'SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION': 'SAI_PACKET_ACTION_FORWARD'},
                   {'SAI_ACL_ENTRY_ATTR_PRIORITY': '40',
                    'SAI_ACL_ENTRY_ATTR_FIELD_DST_IP': '100.64.0.0&mask:255.192.0.0',
                    'SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION': 'SAI_PACKET_ACTION_FORWARD'},
                   {'SAI_ACL_ENTRY_ATTR_PRIORITY': '50',
                    'SAI_ACL_ENTRY_ATTR_FIELD_DST_IP': '104.146.32.0&mask:255.255.224.0',
                    'SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION': 'SAI_PACKET_ACTION_FORWARD'},
                   {'SAI_ACL_ENTRY_ATTR_PRIORITY': '60',
                    'SAI_ACL_ENTRY_ATTR_FIELD_SRC_IP': '21.0.0.0&mask:255.0.0.0',
                    'SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION': 'SAI_PACKET_ACTION_FORWARD'} ]
        #insert rules
        for rule in rules:
           fvs  = swsscommon.FieldValuePairs(rule)
           subnet_mask_rules += 1
           tbl.set( "test_subnet|acl_test_rule%s" % subnet_mask_rules, fvs )

        time.sleep(1)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == subnet_mask_rules

        #match each entry to its corresponding verification
        matched_masks = 0
        for entry in acl_entry:
           (status, fvs) = atbl.get(entry)
           assert status == True
           assert len(fvs) == 6
           #helper function
           if self.check_rule_existence(dict(fvs), rules, verifs):
              matched_masks += 1

        assert matched_masks == subnet_mask_rules

        while subnet_mask_rules > 0:
           tbl._del("test_subnet|acl_test_rule%s" % subnet_mask_rules)
           subnet_mask_rules -= 1

        time.sleep(1)

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False

        tbl = swsscommon.Table(db, "ACL_TABLE")
        tbl._del("test_subnet")

        time.sleep(1)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        keys = atbl.getKeys()
        assert len(keys) >= 1


    def test_AclRuleIcmp(self, dvs, testlog):
        self.setup_db(dvs)

        acl_table = "TEST_TABLE"
        acl_rule = "TEST_RULE"

        self.create_acl_table(acl_table, "L3", ["Ethernet0", "Ethernet4"])

        self.create_acl_rule(acl_table, acl_rule, "ICMP_TYPE", "8")

        self.verify_acl_rule(dvs, "SAI_ACL_ENTRY_ATTR_FIELD_ICMP_TYPE", "8&mask:0xff")

        self.remove_acl_rule(acl_table, acl_rule)

        self.create_acl_rule(acl_table, acl_rule, "ICMP_CODE", "9")

        self.verify_acl_rule(dvs, "SAI_ACL_ENTRY_ATTR_FIELD_ICMP_CODE", "9&mask:0xff")

        self.remove_acl_rule(acl_table, acl_rule)

        self.remove_acl_table(acl_table)

    def test_AclRuleIcmpV6(self, dvs, testlog):
        self.setup_db(dvs)

        acl_table = "TEST_TABLE"
        acl_rule = "TEST_RULE"

        self.create_acl_table(acl_table, "L3V6", ["Ethernet0", "Ethernet4"])

        self.create_acl_rule(acl_table, acl_rule, "ICMPV6_TYPE", "8")

        self.verify_acl_rule(dvs, "SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_TYPE", "8&mask:0xff")

        self.remove_acl_rule(acl_table, acl_rule)

        self.create_acl_rule(acl_table, acl_rule, "ICMPV6_CODE", "9")

        self.verify_acl_rule(dvs, "SAI_ACL_ENTRY_ATTR_FIELD_ICMPV6_CODE", "9&mask:0xff")

        self.remove_acl_rule(acl_table, acl_rule)

        self.remove_acl_table(acl_table)

    def test_AclRuleRedirectToNexthop(self, dvs, testlog):
        dvs.setup_db()
        self.setup_db(dvs)

        # bring up interface
        dvs.set_interface_status("Ethernet4", "up")

        # assign IP to interface
        dvs.add_ip_address("Ethernet4", "10.0.0.1/24")

        # add neighbor
        dvs.add_neighbor("Ethernet4", "10.0.0.2", "00:01:02:03:04:05")

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        keys = atbl.getKeys()
        assert len(keys) == 1
        nhi_oid = keys[0]

        # create ACL_TABLE in config db
        tbl = swsscommon.Table(self.cdb, "ACL_TABLE")
        fvs = swsscommon.FieldValuePairs([("policy_desc", "test"), ("type", "L3"), ("ports", "Ethernet0")])
        tbl.set("test_redirect", fvs)

        time.sleep(2)

        # create acl rule
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([
                                        ("priority", "100"),
                                        ("L4_SRC_PORT", "65000"),
                                        ("PACKET_ACTION", "REDIRECT:10.0.0.2@Ethernet4")])
        tbl.set("test_redirect|test_rule1", fvs)

        time.sleep(1)

        test_acl_table_id = self.get_acl_table_id(dvs)

        # check acl table in asic db
        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == True
        assert len(fvs) == 6
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == test_acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "100"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_L4_SRC_PORT":
                assert fv[1] == "65000&mask:0xffff"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_REDIRECT":
                assert fv[1] == nhi_oid
            else:
                assert False

        # remove acl rule
        tbl._del("test_redirect|test_rule1")

        time.sleep(1)

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False

        tbl = swsscommon.Table(self.cdb, "ACL_TABLE")
        tbl._del("test_redirect")

        time.sleep(1)

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        keys = atbl.getKeys()
        assert len(keys) >= 1

        # remove neighbor
        dvs.remove_neighbor("Ethernet4", "10.0.0.2")

        # remove interface ip
        dvs.remove_ip_address("Ethernet4", "10.0.0.1/24")

        # bring down interface
        dvs.set_interface_status("Ethernet4", "down")

class TestAclRuleValidation(BaseTestAcl):
    """ Test class for cases that check if orchagent corectly validates
    ACL rules input
    """

    SWITCH_CAPABILITY_TABLE = "SWITCH_CAPABILITY"

    def get_acl_actions_supported(self, stage):
        capability_table = swsscommon.Table(self.sdb, self.SWITCH_CAPABILITY_TABLE)
        keys = capability_table.getKeys()
        # one switch available
        assert len(keys) == 1
        status, fvs = capability_table.get(keys[0])
        assert status == True

        field = "ACL_ACTIONS|{}".format(stage.upper())
        fvs = dict(fvs)

        values_list = fvs.get(field, None)

        if values_list is not None:
            values_list = values_list.split(",")

        return values_list

    def test_AclActionValidation(self, dvs, testlog):
        """ The test overrides R/O SAI_SWITCH_ATTR_ACL_STAGE_INGRESS/EGRESS switch attributes
        to check the case when orchagent refuses to process rules with action that is not supported
        by the ASIC
        """

        self.setup_db(dvs)

        stage_name_map = {
            "ingress": "SAI_SWITCH_ATTR_ACL_STAGE_INGRESS",
            "egress": "SAI_SWITCH_ATTR_ACL_STAGE_EGRESS",
        }

        for stage in stage_name_map:
            action_values = self.get_acl_actions_supported(stage)

            # virtual switch supports all actions
            assert action_values is not None
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
            # wait for daemons to start
            time.sleep(2)
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

            self.create_acl_table(acl_table, "L3", bind_ports, stage=stage)
            self.create_acl_rule(acl_table, acl_rule, "ICMP_TYPE", "8")

            atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
            keys = atbl.getKeys()

            # verify there are no non-default ACL rules created
            acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
            assert len(acl_entry) == 0

            self.remove_acl_table(acl_table)
            # remove rules from CFG DB
            self.remove_acl_rule(acl_table, acl_rule)

            dvs.runcmd("supervisorctl restart syncd")
            dvs.stop_swss()
            dvs.start_swss()
            time.sleep(5)
