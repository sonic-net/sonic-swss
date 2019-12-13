# This test suite covers the functionality of mirror feature in SwSS

import platform
import pytest
import time
from distutils.version import StrictVersion

from swsscommon import swsscommon

DVS_FAKE_PLATFORM = "mellanox"


class TestMirror(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        self.sdb = swsscommon.DBConnector(6, dvs.redis_sock, 0)

    def set_interface_status(self, interface, admin_status):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN"
        else:
            tbl_name = "PORT"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        fvs = swsscommon.FieldValuePairs([("admin_status", "up")])
        tbl.set(interface, fvs)
        time.sleep(1)

    def add_ip_address(self, interface, ip):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        else:
            tbl_name = "INTERFACE"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        tbl.set(interface, fvs)
        tbl.set(interface + "|" + ip, fvs)
        time.sleep(1)

    def remove_ip_address(self, interface, ip):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        else:
            tbl_name = "INTERFACE"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        tbl._del(interface + "|" + ip)
        tbl._del(interface)
        time.sleep(1)

    def add_neighbor(self, interface, ip, mac):
        tbl = swsscommon.ProducerStateTable(self.pdb, "NEIGH_TABLE")
        fvs = swsscommon.FieldValuePairs([("neigh", mac),
                                          ("family", "IPv4")])
        tbl.set(interface + ":" + ip, fvs)
        time.sleep(1)

    def remove_neighbor(self, interface, ip):
        tbl = swsscommon.ProducerStateTable(self.pdb, "NEIGH_TABLE")
        tbl._del(interface + ":" + ip)
        time.sleep(1)

    def add_route(self, dvs, prefix, nexthop):
        dvs.runcmd("ip route add " + prefix + " via " + nexthop)
        time.sleep(1)

    def remove_route(self, dvs, prefix):
        dvs.runcmd("ip route del " + prefix)
        time.sleep(1)

    def create_mirror_session(self, name, src, dst, gre, dscp, ttl, queue):
        tbl = swsscommon.Table(self.cdb, "MIRROR_SESSION")
        fvs = swsscommon.FieldValuePairs([("src_ip", src),
                                          ("dst_ip", dst),
                                          ("gre_type", gre),
                                          ("dscp", dscp),
                                          ("ttl", ttl),
                                          ("queue", queue)])
        tbl.set(name, fvs)
        time.sleep(1)

    def remove_mirror_session(self, name):
        tbl = swsscommon.Table(self.cdb, "MIRROR_SESSION")
        tbl._del(name)
        time.sleep(1)

    def get_mirror_session_status(self, name):
        return self.get_mirror_session_state(name)["status"]

    def get_mirror_session_state(self, name):
        tbl = swsscommon.Table(self.sdb, "MIRROR_SESSION_TABLE")
        (status, fvs) = tbl.get(name)
        assert status == True
        assert len(fvs) > 0
        return { fv[0]: fv[1] for fv in fvs }

    def create_acl_table(self, table, interfaces, type):
        tbl = swsscommon.Table(self.cdb, "ACL_TABLE")
        fvs = swsscommon.FieldValuePairs([("policy_desc", "mirror_test"),
                                          ("type", type),
                                          ("ports", ",".join(interfaces))])
        tbl.set(table, fvs)
        time.sleep(1)

    def remove_acl_table(self, table):
        tbl = swsscommon.Table(self.cdb, "ACL_TABLE")
        tbl._del(table)
        time.sleep(1)

    def create_mirror_acl_ipv4_rule(self, table, rule, session):
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "1000"),
                                          ("mirror_action", session),
                                          ("SRC_IP", "10.0.0.0/32"),
                                          ("DST_IP", "20.0.0.0/23")])
        tbl.set(table + "|" + rule, fvs)
        time.sleep(1)

    def create_mirror_acl_ipv6_rule(self, table, rule, session):
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "1000"),
                                          ("mirror_action", session),
                                          ("SRC_IPV6", "2777::0/64"),
                                          ("DST_IPV6", "3666::0/128")])
        tbl.set(table + "|" + rule, fvs)
        time.sleep(1)

    def remove_mirror_acl_rule(self, table, rule):
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        tbl._del(table + "|" + rule)
        time.sleep(1)

    def test_MirrorV6TableCreation(self, dvs, testlog):
        self.setup_db(dvs)

        acl_table_v6 = "MIRROR_TABLE_V6"
        ports = ["Ethernet0", "Ethernet4"]

        # Create the V6 table
        self.create_acl_table(acl_table_v6, ports, "MIRRORV6")

        # Check that the V6 table has been created
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        table_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_tables]
        assert len(table_entries) == 1

        # Get the data from the V6 table
        v6_table_id = table_entries[0]
        status, attributes = tbl.get(v6_table_id)
        assert status

        # TODO: Refactor mirror table tests so that these attributes can be shared between tests for v4, v6, and
        # dscp mirror tables.
        expected_sai_attributes = [
            "SAI_ACL_TABLE_ATTR_FIELD_ACL_IP_TYPE",
            "SAI_ACL_TABLE_ATTR_FIELD_IP_PROTOCOL",
            "SAI_ACL_TABLE_ATTR_FIELD_SRC_IPV6",
            "SAI_ACL_TABLE_ATTR_FIELD_DST_IPV6",
            "SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_TYPE",
            "SAI_ACL_TABLE_ATTR_FIELD_ICMPV6_CODE",
            "SAI_ACL_TABLE_ATTR_FIELD_L4_SRC_PORT",
            "SAI_ACL_TABLE_ATTR_FIELD_L4_DST_PORT",
            "SAI_ACL_TABLE_ATTR_FIELD_TCP_FLAGS",
            "SAI_ACL_TABLE_ATTR_FIELD_DSCP",
        ]

        expected_sai_list_attributes = [
            "SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE",
            "SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST"
        ]

        # Check that all of the V6 table attributes have been populated
        for attribute in attributes:
            key = attribute[0]
            value = attribute[1]

            if key in expected_sai_attributes:
                assert value == "true"
            elif key in expected_sai_list_attributes:
                count = int(value[0:1])
                list_attrs = value[2:].split(',')
                if key == "SAI_ACL_TABLE_ATTR_FIELD_ACL_RANGE_TYPE":
                    assert set(list_attrs) == set(["SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE", "SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE"])
                elif key == "SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST":
                    assert set(list_attrs) == set(["SAI_ACL_BIND_POINT_TYPE_PORT", "SAI_ACL_BIND_POINT_TYPE_LAG"])
                else:
                    print("Encountered unexpected range attribute on mirror table: {}".format(key))
                    assert False
            elif key == "SAI_ACL_TABLE_ATTR_ACL_STAGE":
                assert value == "SAI_ACL_STAGE_INGRESS"
            else:
                print("Encountered unexpected attribute on mirror table: {}".format(key))
                assert False

        # Delete the V6 table
        self.remove_acl_table(acl_table_v6)

    # Test case - create a MIRROR table and a MIRRORV6 table in separated mode
    # 0. predefine the VS platform: mellanox platform
    # 1. create a mirror session
    # 2. create two ACL tables that support IPv4 and IPv6 separatedly
    # 3. create two ACL rules with both IPv4 and IPv6 source and destination IP
    #    verify the ACL rules are created successfully
    # 4. remove all the configurations
    def test_AclBindMirrorSeparated(self, dvs, testlog):
        """
        This test verifies IPv6 rules cannot be inserted into MIRROR table
        """
        self.setup_db(dvs)

        session = "MIRROR_SESSION"
        acl_table = "MIRROR_TABLE"
        acl_table_v6 = "MIRROR_TABLE_V6"
        acl_rule_1 = "MIRROR_RULE_1"
        acl_rule_2 = "MIRROR_RULE_2"

        # bring up port; assign ip; create neighbor; create route
        self.set_interface_status("Ethernet32", "up")
        self.add_ip_address("Ethernet32", "20.0.0.0/31")
        self.add_neighbor("Ethernet32", "20.0.0.1", "02:04:06:08:10:12")
        self.add_route(dvs, "4.4.4.4", "20.0.0.1")

        # create mirror session
        self.create_mirror_session(session, "3.3.3.3", "4.4.4.4", "0x6558", "8", "100", "0")
        assert self.get_mirror_session_state(session)["status"] == "active"

        # assert mirror session in asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        mirror_session_oid = tbl.getKeys()[0]

        # create acl table ipv4
        self.create_acl_table(acl_table, ["Ethernet0", "Ethernet4"], "MIRROR")

        # assert acl table ipv4 is created
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        table_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_tables]
        assert len(table_entries) == 1

        table_id_v4 = table_entries[0]

        # create acl table ipv6
        self.create_acl_table(acl_table_v6, ["Ethernet0", "Ethernet4"], "MIRRORV6")

        # assert acl table ipv6 is created
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        table_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_tables]
        assert len(table_entries) == 2

        table_id_v6 = table_entries[1] if table_entries[0] == table_id_v4 else table_entries[0]

        # create acl rule with IPv4 addresses
        self.create_mirror_acl_ipv4_rule(acl_table, acl_rule_1, session)

        # assert acl rule ipv4 is created
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        rule_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_entries]
        assert len(rule_entries) == 1

        rule_id_v4 = rule_entries[0]

        # assert acl rule is assocaited with table ipv4
        (status, fvs) = tbl.get(rule_id_v4)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == table_id_v4

        # create acl rule with IPv6 addresses
        self.create_mirror_acl_ipv6_rule(acl_table_v6, acl_rule_2, session)

        # assert acl rule ipv6 is created
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        rule_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_entries]
        assert len(rule_entries) == 2

        rule_id_v6 = rule_entries[1] if rule_entries[0] == rule_id_v4 else rule_entries[0]

        # assert acl rule is associated with table ipv6
        (status, fvs) = tbl.get(rule_id_v6)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == table_id_v6

        # remove acl rule
        self.remove_mirror_acl_rule(acl_table, acl_rule_1)
        self.remove_mirror_acl_rule(acl_table_v6, acl_rule_2)

        # remove acl table
        self.remove_acl_table(acl_table)
        self.remove_acl_table(acl_table_v6)

        # remove mirror session
        self.remove_mirror_session(session)

        # assert no mirror session in asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 0

        # remove route; remove neighbor; remove ip; bring down port
        self.remove_route(dvs, "4.4.4.4")
        self.remove_neighbor("Ethernet32", "20.0.0.1")
        self.remove_ip_address("Ethernet32", "20.0.0.0/31")
        self.set_interface_status("Ethernet32", "down")

    # Test case - intervene rule creation in table creation
    # 0. predefine the VS platform: mellanox platform
    # 1. create a mirror session
    # 2. create the ipv4 ACL table
    # 3. create the ipv4 ACL rule
    # 4. create the ipv6 ACL table
    # 5. create the ipv6 ACL rule
    # 6. verify two rules are inserted successfully
    def test_AclBindMirrorV6Reorder1(self, dvs, testlog):
        """
        This test verifies IPv6 rules cannot be inserted into MIRROR table
        """
        self.setup_db(dvs)

        session = "MIRROR_SESSION"
        acl_table = "MIRROR_TABLE"
        acl_table_v6 = "MIRROR_TABLE_V6"
        acl_rule_1 = "MIRROR_RULE_1"
        acl_rule_2 = "MIRROR_RULE_2"

        # bring up port; assign ip; create neighbor; create route
        self.set_interface_status("Ethernet32", "up")
        self.add_ip_address("Ethernet32", "20.0.0.0/31")
        self.add_neighbor("Ethernet32", "20.0.0.1", "02:04:06:08:10:12")
        self.add_route(dvs, "4.4.4.4", "20.0.0.1")

        # create mirror session
        self.create_mirror_session(session, "3.3.3.3", "4.4.4.4", "0x6558", "8", "100", "0")
        assert self.get_mirror_session_state(session)["status"] == "active"

        # assert mirror session in asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        mirror_session_oid = tbl.getKeys()[0]

        # create acl table ipv4
        self.create_acl_table(acl_table, ["Ethernet0", "Ethernet4"], "MIRROR")

        # create acl rule with IPv4 addresses
        self.create_mirror_acl_ipv4_rule(acl_table, acl_rule_1, session)

        # create acl table ipv6
        self.create_acl_table(acl_table_v6, ["Ethernet0", "Ethernet4"], "MIRRORV6")

        # create acl rule with IPv6 addresses
        self.create_mirror_acl_ipv6_rule(acl_table_v6, acl_rule_2, session)

        # assert acl rules are created
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        rule_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_entries]
        assert len(rule_entries) == 2

        # remove acl rule
        self.remove_mirror_acl_rule(acl_table, acl_rule_1)
        self.remove_mirror_acl_rule(acl_table_v6, acl_rule_2)

        # remove acl table
        self.remove_acl_table(acl_table)
        self.remove_acl_table(acl_table_v6)

        # remove mirror session
        self.remove_mirror_session(session)

        # assert no mirror session in asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 0

        # remove route; remove neighbor; remove ip; bring down port
        self.remove_route(dvs, "4.4.4.4")
        self.remove_neighbor("Ethernet32", "20.0.0.1")
        self.remove_ip_address("Ethernet32", "20.0.0.0/31")
        self.set_interface_status("Ethernet32", "down")


    # Test case - intervene rule creation in table creation
    # 0. predefine the VS platform: mellanox platform
    # 1. create a mirror session
    # 2. create the ipv4 ACL table
    # 3. create the ipv6 ACL rule
    # 4. create the ipv6 ACL table
    # 5. create the ipv4 ACL rule
    # 6. verify two rules are inserted successfully
    def test_AclBindMirrorV6Reorder2(self, dvs, testlog):
        """
        This test verifies IPv6 rules cannot be inserted into MIRROR table
        """
        self.setup_db(dvs)

        session = "MIRROR_SESSION"
        acl_table = "MIRROR_TABLE"
        acl_table_v6 = "MIRROR_TABLE_V6"
        acl_rule_1 = "MIRROR_RULE_1"
        acl_rule_2 = "MIRROR_RULE_2"

        # bring up port; assign ip; create neighbor; create route
        self.set_interface_status("Ethernet32", "up")
        self.add_ip_address("Ethernet32", "20.0.0.0/31")
        self.add_neighbor("Ethernet32", "20.0.0.1", "02:04:06:08:10:12")
        self.add_route(dvs, "4.4.4.4", "20.0.0.1")

        # create mirror session
        self.create_mirror_session(session, "3.3.3.3", "4.4.4.4", "0x6558", "8", "100", "0")
        assert self.get_mirror_session_state(session)["status"] == "active"

        # assert mirror session in asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        mirror_session_oid = tbl.getKeys()[0]

        # create acl table ipv4
        self.create_acl_table(acl_table, ["Ethernet0", "Ethernet4"], "MIRROR")

        # create acl rule with IPv6 addresses
        self.create_mirror_acl_ipv6_rule(acl_table_v6, acl_rule_2, session)

        # assert acl rule is not created
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        rule_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_entries]
        assert len(rule_entries) == 0

        # create acl table ipv6
        self.create_acl_table(acl_table_v6, ["Ethernet0", "Ethernet4"], "MIRRORV6")

        # create acl rule with IPv4 addresses
        self.create_mirror_acl_ipv4_rule(acl_table, acl_rule_1, session)

        # assert acl rules are created
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        rule_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_entries]
        assert len(rule_entries) == 2

        # remove acl rule
        self.remove_mirror_acl_rule(acl_table, acl_rule_1)
        self.remove_mirror_acl_rule(acl_table_v6, acl_rule_2)

        # remove acl table
        self.remove_acl_table(acl_table)
        self.remove_acl_table(acl_table_v6)

        # remove mirror session
        self.remove_mirror_session(session)

        # assert no mirror session in asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 0

        # remove route; remove neighbor; remove ip; bring down port
        self.remove_route(dvs, "4.4.4.4")
        self.remove_neighbor("Ethernet32", "20.0.0.1")
        self.remove_ip_address("Ethernet32", "20.0.0.0/31")
        self.set_interface_status("Ethernet32", "down")


    # Test case - create ACL rules associated with wrong table
    # 0. predefine the VS platform: mellanox platform
    # 1. create a mirror session
    # 2. create the ipv4 ACL table
    # 3. create the ipv6 ACL rule associated with ipv4 table
    # 4. create the ipv6 ACL table
    # 5. create the ipv4 ACL rule associated with ipv6 table
    # 6. verify two rules are inserted successfully
    def test_AclBindMirrorV6WrongConfig(self, dvs, testlog):
        """
        This test verifies IPv6 rules cannot be inserted into MIRROR table
        """
        self.setup_db(dvs)

        session = "MIRROR_SESSION"
        acl_table = "MIRROR_TABLE"
        acl_table_v6 = "MIRROR_TABLE_V6"
        acl_rule_1 = "MIRROR_RULE_1"
        acl_rule_2 = "MIRROR_RULE_2"

        # bring up port; assign ip; create neighbor; create route
        self.set_interface_status("Ethernet32", "up")
        self.add_ip_address("Ethernet32", "20.0.0.0/31")
        self.add_neighbor("Ethernet32", "20.0.0.1", "02:04:06:08:10:12")
        self.add_route(dvs, "4.4.4.4", "20.0.0.1")

        # create mirror session
        self.create_mirror_session(session, "3.3.3.3", "4.4.4.4", "0x6558", "8", "100", "0")
        assert self.get_mirror_session_state(session)["status"] == "active"

        # assert mirror session in asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        mirror_session_oid = tbl.getKeys()[0]

        # create acl table ipv4
        self.create_acl_table(acl_table, ["Ethernet0", "Ethernet4"], "MIRROR")

        # create WRONG acl rule with IPv6 addresses
        self.create_mirror_acl_ipv6_rule(acl_table, acl_rule_2, session)

        # assert acl rule is not created
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        rule_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_entries]
        assert len(rule_entries) == 0

        # create acl table ipv6
        self.create_acl_table(acl_table_v6, ["Ethernet0", "Ethernet4"], "MIRRORV6")

        # create WRONG acl rule with IPv4 addresses
        self.create_mirror_acl_ipv4_rule(acl_table_v6, acl_rule_1, session)

        # assert acl rules are created
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        rule_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_entries]
        assert len(rule_entries) == 0

        # remove acl rule
        self.remove_mirror_acl_rule(acl_table, acl_rule_1)
        self.remove_mirror_acl_rule(acl_table_v6, acl_rule_2)

        # remove acl table
        self.remove_acl_table(acl_table)
        self.remove_acl_table(acl_table_v6)

        # remove mirror session
        self.remove_mirror_session(session)

        # assert no mirror session in asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 0

        # remove route; remove neighbor; remove ip; bring down port
        self.remove_route(dvs, "4.4.4.4")
        self.remove_neighbor("Ethernet32", "20.0.0.1")
        self.remove_ip_address("Ethernet32", "20.0.0.0/31")
        self.set_interface_status("Ethernet32", "down")


