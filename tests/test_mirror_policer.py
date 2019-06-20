# This test suite covers the functionality of mirror feature in SwSS

import platform
import pytest
import time
from distutils.version import StrictVersion

from swsscommon import swsscommon


class TestMirror(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        self.sdb = swsscommon.DBConnector(6, dvs.redis_sock, 0)

    def set_port_status(self, port, admin_status):
        tbl = swsscommon.Table(self.cdb, "PORT")
        fvs = swsscommon.FieldValuePairs([("admin_status", "up")])
        tbl.set(port, fvs)
        time.sleep(1)

    def add_ip_address(self, interface, ip):
        tbl = swsscommon.Table(self.cdb, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        tbl.set(interface + "|" + ip, fvs)
        time.sleep(1)

    def remove_ip_address(self, interface, ip):
        tbl = swsscommon.Table(self.cdb, "INTERFACE")
        tbl._del(interface + "|" + ip)
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

    def create_mirror_session(self, name, src, dst, gre, dscp, ttl, queue, policer):
        tbl = swsscommon.Table(self.cdb, "MIRROR_SESSION")
        fvs = swsscommon.FieldValuePairs([("src_ip", src),
                                          ("dst_ip", dst),
                                          ("gre_type", gre),
                                          ("dscp", dscp),
                                          ("ttl", ttl),
                                          ("queue", queue),
                                          ("policer", policer)])
        tbl.set(name, fvs)
        time.sleep(1)

    def remove_mirror_session(self, name):
        tbl = swsscommon.Table(self.cdb, "MIRROR_SESSION")
        tbl._del(name)
        time.sleep(1)

    def create_policer(self, name):
        tbl = swsscommon.Table(self.cdb, "POLICER")
        fvs = swsscommon.FieldValuePairs([("meter_type", "packets"),
                                          ("mode", "sr_tcm"),
                                          ("cir", "600"),
                                          ("cbs", "600"),
                                          ("red_packet_action", "drop")])
        tbl.set(name, fvs)
        time.sleep(1)

    def remove_policer(self, name):
        tbl = swsscommon.Table(self.cdb, "POLICER")
        tbl._del(name)
        time.sleep(1)

    def test_MirrorPolicer(self, dvs, testlog):
        self.setup_db(dvs)

        session = "MIRROR_SESSION"
        policer= "POLICER"

        # create policer
        self.create_policer(policer)

        # create mirror session
        self.create_mirror_session(session, "3.3.3.3", "2.2.2.2", "0x6558", "8", "100", "0", policer)
        self.set_port_status("Ethernet16", "up")
        self.add_ip_address("Ethernet16", "10.0.0.0/31")
        self.add_neighbor("Ethernet16", "10.0.0.1", "02:04:06:08:10:12")
        self.add_route(dvs, "2.2.2.2", "10.0.0.1")

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_POLICER")
        policer_entries = tbl.getKeys()
        assert len(policer_entries) == 1
        policer_oid = policer_entries[0]

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        mirror_entries = tbl.getKeys()
        assert len(mirror_entries) == 1

        (status, fvs) = tbl.get(mirror_entries[0])
        assert status == True
        assert len(fvs) == 12
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet16"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TYPE":
                assert fv[1] == "SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE":
                assert fv[1] == "SAI_ERSPAN_ENCAPSULATION_TYPE_MIRROR_L3_GRE_TUNNEL"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION":
                assert fv[1] == "4"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TOS":
                assert fv[1] == "32"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TTL":
                assert fv[1] == "100"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS":
                assert fv[1] == "3.3.3.3"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS":
                assert fv[1] == "2.2.2.2"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS":
                assert fv[1] == dvs.runcmd("bash -c \"ip link show eth0 | grep ether | awk '{print $2}'\"")[1].strip().upper()
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS":
                assert fv[1] == "02:04:06:08:10:12"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE":
                assert fv[1] == "25944" # 0x6558
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_POLICER":
                assert fv[1] == policer_oid
            else:
                assert False

        # remove mirror session
        self.remove_route(dvs, "2.2.2.2")
        self.remove_neighbor("Ethernet16", "10.0.0.1")
        self.remove_ip_address("Ethernet16", "10.0.0.0/31")
        self.set_port_status("Ethernet16", "down")
        self.remove_mirror_session(session)

        # remove policer
        self.remove_policer(policer)


    def create_acl_table(self, table, interfaces):
        tbl = swsscommon.Table(self.cdb, "ACL_TABLE")
        fvs = swsscommon.FieldValuePairs([("policy_desc", "mirror_test"),
                                          ("type", "mirror"),
                                          ("ports", ",".join(interfaces))])
        tbl.set(table, fvs)
        time.sleep(1)

    def remove_acl_table(self, table):
        tbl = swsscommon.Table(self.cdb, "ACL_TABLE")
        tbl._del(table)
        time.sleep(1)

    def create_mirror_acl_dscp_rule(self, table, rule, dscp, session):
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "1000"),
                                          ("mirror_action", session),
                                          ("DSCP", dscp)])
        tbl.set(table + "|" + rule, fvs)
        time.sleep(1)

    def remove_mirror_acl_dscp_rule(self, table, rule):
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        tbl._del(table + "|" + rule)
        time.sleep(1)

    def test_MirrorPolicerWithAcl(self, dvs, testlog):
        self.setup_db(dvs)

        session = "MIRROR_SESSION"
        policer= "POLICER"
        acl_table = "MIRROR_TABLE"
        acl_rule = "MIRROR_RULE"

        # create policer
        self.create_policer(policer)

        # create mirror session
        self.create_mirror_session(session, "3.3.3.3", "2.2.2.2", "0x6558", "8", "100", "0", policer)
        self.set_port_status("Ethernet16", "up")
        self.add_ip_address("Ethernet16", "10.0.0.0/31")
        self.add_neighbor("Ethernet16", "10.0.0.1", "02:04:06:08:10:12")
        self.add_route(dvs, "2.2.2.2", "10.0.0.1")

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        mirror_entries = tbl.getKeys()
        assert len(mirror_entries) == 1
        mirror_oid = mirror_entries[0]

        # create acl table
        self.create_acl_table(acl_table, ["Ethernet0", "Ethernet4"])

        # create acl rule with dscp value and mask
        self.create_mirror_acl_dscp_rule(acl_table, acl_rule, "8/56", session)

        # assert acl rule is created
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        rule_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_entries]
        assert len(rule_entries) == 1

        (status, fvs) = tbl.get(rule_entries[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_DSCP":
                assert fv[1] == "8&mask:0x38"
            if fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS":
                assert fv[1] == "1:" + mirror_oid

        # remove acl rule
        self.remove_mirror_acl_dscp_rule(acl_table, acl_rule)

        # remove acl table
        self.remove_acl_table(acl_table)

        # remove mirror session
        self.remove_route(dvs, "2.2.2.2")
        self.remove_neighbor("Ethernet16", "10.0.0.1")
        self.remove_ip_address("Ethernet16", "10.0.0.0/31")
        self.set_port_status("Ethernet16", "down")
        self.remove_mirror_session(session)

        # remove policer
        self.remove_policer(policer)
