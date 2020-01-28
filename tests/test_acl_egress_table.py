import time
import pytest

from swsscommon import swsscommon
from flaky import flaky


@pytest.mark.flaky
class TestEgressAclTable(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

    def create_egress_acl_table(self, table_name, ports):
        tbl = swsscommon.Table(self.cdb, "ACL_TABLE")

        fvs = swsscommon.FieldValuePairs([("POLICY_DESC", "EGRESS_ACL_TEST"),
                                          ("TYPE", "L3"),
                                          ("PORTS", ports),
                                          ("stage", "EGRESS")])
        tbl.set(table_name, fvs)
        time.sleep(1)

    def create_acl_rule(self, fv_pairs, rule_name):
        rule_tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs(fv_pairs)
        rule_tbl.set("egress_acl_table|" + rule_name, fvs)
        time.sleep(1)

    def remove_acl_table(self, table_name):
        tbl = swsscommon.Table(self.cdb, "ACL_TABLE")
        tbl._del(table_name)
        time.sleep(1)

    def get_acl_table_id(self, dvs):
        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        keys = atbl.getKeys()
        for k in dvs.asicdb.default_acl_tables:
            assert k in keys
        acl_tables = [k for k in keys if k not in dvs.asicdb.default_acl_tables]

        assert len(acl_tables) == 1

        return acl_tables[0]

    def remove_acl_rule(self, table_name, rule_name):
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        tbl._del(table_name + "|" + rule_name)
        time.sleep(1)

    def verify_acl_asic_table(self, dvs, bind_ports):
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        acl_table_groups = tbl.getKeys()
        assert len(acl_table_groups) == len(bind_ports)

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        port_groups = []
        for p in [dvs.asicdb.portnamemap[portname] for portname in bind_ports]:
            (status, fvs) = tbl.get(p)
            for fv in fvs:
                if fv[0] == "SAI_PORT_ATTR_EGRESS_ACL":
                    assert fv[1] in acl_table_groups
                    port_groups.append(fv[1])

        assert len(port_groups) == len(bind_ports)
        assert set(port_groups) == set(acl_table_groups)

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        for port_group in port_groups:
            (status, fvs) = tbl.get(port_group)
            assert status == True
            assert len(fvs) == 3
            for fv in fvs:
                if fv[0] == "SAI_ACL_TABLE_GROUP_ATTR_ACL_STAGE":
                    assert fv[1] == "SAI_ACL_STAGE_EGRESS"
                elif fv[0] == "SAI_ACL_TABLE_GROUP_ATTR_ACL_BIND_POINT_TYPE_LIST":
                    assert fv[1] == "1:SAI_ACL_BIND_POINT_TYPE_PORT"
                elif fv[0] == "SAI_ACL_TABLE_GROUP_ATTR_TYPE":
                    assert fv[1] == "SAI_ACL_TABLE_GROUP_TYPE_PARALLEL"
                else:
                    assert False

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER")
        member = tbl.getKeys()[0]
        (status, fvs) = tbl.get(member)
        assert status == True
        assert len(fvs) == 3
        for fv in fvs:
            if fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID":
                assert fv[1] in port_groups 
            elif fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID":
                table_id = fv[1]
            elif fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_PRIORITY":
                assert fv[1] == "100"
            else:
                assert False

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        (status, fvs) = tbl.get(table_id)
        assert status == True

    def verify_acl_rule_asic_fvs(self, dvs, fv_tuple):
        # Verify Acl entry in ASIC DB
        test_acl_table_id = self.get_acl_table_id(dvs)
        acl_tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = acl_tbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1

        (status, fvs) = acl_tbl.get(acl_entry[0])
        assert status == True
        assert len(fvs) == 6
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == test_acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "1000"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == fv_tuple[0]:
                assert fv[1] == fv_tuple[1]
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_DROP"
            else:
                assert False

    def verify_acl_rule_with_L4PortRange_asic_fvs(self, dvs, fv_tuple):
        # Verify Acl entry in ASIC DB
        test_acl_table_id = self.get_acl_table_id(dvs)
        acl_tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = acl_tbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1

        (status, fvs) = acl_tbl.get(acl_entry[0])
        assert status == True
        assert len(fvs) == 6
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_TABLE_ID":
                assert fv[1] == test_acl_table_id
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ADMIN_STATE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_PRIORITY":
                assert fv[1] == "999"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_COUNTER":
                assert True
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_ACL_RANGE_TYPE":
                aclrange = fv[1]
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_FORWARD"
            else:
                assert False

        # Verify Acl range in ASIC DB
        acl_tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_RANGE")
        aclrange_obj = aclrange.split(":", 1)[1]

        (status, fvs) = acl_tbl.get(aclrange_obj)
        assert status == True
        assert len(fvs) == 2
        for fv in fvs:
            if fv[0] == "SAI_ACL_RANGE_ATTR_TYPE":
                assert fv[1] == fv_tuple[0]
            elif fv[0] == "SAI_ACL_RANGE_ATTR_LIMIT":
                assert fv[1] == fv_tuple[1]
            else:
                assert False

    def check_asic_table_absent(self, dvs):
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        acl_tables = tbl.getKeys()
        for key in dvs.asicdb.default_acl_tables:
            assert key in acl_tables
        acl_tables = [k for k in acl_tables if k not in dvs.asicdb.default_acl_tables]

        assert len(acl_tables) == 0

    def check_asic_rule_absent(self, dvs):
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        acl_entries = tbl.getKeys()
        for key in dvs.asicdb.default_acl_entries:
            assert key in acl_entries
        acl_entries = [k for k in acl_entries if k not in dvs.asicdb.default_acl_entries]

        assert len(acl_entries) == 0

    def test_EgressAclTableCreation(self, dvs):
        self.setup_db(dvs)

        # Create ACL_TABLE in config db
        bind_ports = ["Ethernet0", "Ethernet4"]
        self.create_egress_acl_table("egress_acl_table", ",".join(bind_ports))

        time.sleep(1)

        # Check acl table in asic db
        self.verify_acl_asic_table(dvs, bind_ports)

    def test_EgressAclRuleL4SrcPortRange(self, dvs):
        self.setup_db(dvs)

        # Create L4 SrcPortRange Acl rule
        fvPairs = [("priority", "999"), ("PACKET_ACTION", "FORWARD"), ("L4_SRC_PORT_RANGE", "0-1001")]
        self.create_acl_rule(fvPairs, "L4SrcPortRange_rule")

        # Verify Acl rule in ASIC DB
        fv_tuple = ("SAI_ACL_RANGE_TYPE_L4_SRC_PORT_RANGE", "0,1001")
        self.verify_acl_rule_with_L4PortRange_asic_fvs(dvs, fv_tuple)

        # Remove Acl rule
        self.remove_acl_rule("egress_acl_table", "L4SrcPortRange_rule")
        self.check_asic_rule_absent(dvs)

    def test_EgressAclRuleL4DstPortRange(self, dvs):
        self.setup_db(dvs)

        # Create L4 DstPortRange Acl rule
        fvPairs = [("priority", "999"), ("PACKET_ACTION", "FORWARD"), ("L4_DST_PORT_RANGE", "1003-6666")]
        self.create_acl_rule(fvPairs, "L4DstPortRange_rule")

        # Verify Acl rule in ASIC DB
        fv_tuple = ("SAI_ACL_RANGE_TYPE_L4_DST_PORT_RANGE", "1003,6666")
        self.verify_acl_rule_with_L4PortRange_asic_fvs(dvs, fv_tuple)

        # Remove Acl rule
        self.remove_acl_rule("egress_acl_table", "L4DstPortRange_rule")
        self.check_asic_rule_absent(dvs)

    def test_EgressAclRuleL2EthType(self, dvs):
        self.setup_db(dvs)

        # Create L4 L2EthType Acl rule
        fvPairs = [("priority", "1000"), ("PACKET_ACTION", "DROP"), ("ETHER_TYPE", "8000")]
        self.create_acl_rule(fvPairs, "L2EthType_rule")

        # Verify Acl rule in ASIC DB
        fv_tuple = ("SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE", "8000&mask:0xffff")
        self.verify_acl_rule_asic_fvs( dvs, fv_tuple)

        # Remove Acl rule
        self.remove_acl_rule("egress_acl_table", "L2EthType_rule")
        self.check_asic_rule_absent(dvs)

    def test_EgressAclRuleTunnelVNI(self, dvs):
        self.setup_db(dvs)

        # Create Tunnel VNI Acl rule
        fvPairs = [("priority", "1000"), ("PACKET_ACTION", "DROP"), ("TUNNEL_VNI", "5000")]
        self.create_acl_rule(fvPairs, "TunnelVNI_rule")

        # Verify Acl rule in ASIC DB
        fv_tuple = ("SAI_ACL_ENTRY_ATTR_FIELD_TUNNEL_VNI", "5000&mask:0xffffffff")
        self.verify_acl_rule_asic_fvs(dvs, fv_tuple)

        # Remove Acl rule
        self.remove_acl_rule("egress_acl_table", "TunnelVNI_rule")
        self.check_asic_rule_absent(dvs)

    def test_EgressAclRuleTC(self, dvs):
        self.setup_db(dvs)

        # Create TC Acl rule
        fvPairs = [("priority", "1000"), ("PACKET_ACTION", "DROP"), ("TC", "1")]
        self.create_acl_rule(fvPairs, "TC_rule")

        # Verify Acl rule in ASIC DB
        fv_tuple = ("SAI_ACL_ENTRY_ATTR_FIELD_TC", "1&mask:0xff")
        self.verify_acl_rule_asic_fvs(dvs, fv_tuple)

        # Remove Acl rule
        self.remove_acl_rule("egress_acl_table", "TC_rule")
        self.check_asic_rule_absent(dvs)

    def test_EgressAclInnerIPProtocol(self, dvs):
        self.setup_db(dvs)

        # Create InnerIPProtocol Acl rule
        fvPairs = [("priority", "1000"), ("PACKET_ACTION", "DROP"), ("INNER_IP_PROTOCOL", "8")]
        self.create_acl_rule(fvPairs, "InnerIPProtocol_rule")

        # Verify Acl rule in ASIC DB
        fv_tuple = ("SAI_ACL_ENTRY_ATTR_FIELD_INNER_IP_PROTOCOL", "8&mask:0xff")
        self.verify_acl_rule_asic_fvs(dvs, fv_tuple)

        # Remove Acl rule
        self.remove_acl_rule("egress_acl_table", "InnerIPProtocol_rule")
        self.check_asic_rule_absent(dvs)

    def test_EgressAclInnerEthType(self, dvs):
        self.setup_db(dvs)

        # Create InnerEthernetType Acl rule
        fvPairs = [("priority", "1000"), ("PACKET_ACTION", "DROP"), ("INNER_ETHER_TYPE", "8000")]
        self.create_acl_rule(fvPairs, "InnerEthType_rule")

        # Verify Acl rule in ASIC DB
        fv_tuple = ("SAI_ACL_ENTRY_ATTR_FIELD_INNER_ETHER_TYPE", "8000&mask:0xffff")
        self.verify_acl_rule_asic_fvs(dvs, fv_tuple)

        # Remove Acl rule
        self.remove_acl_rule("egress_acl_table", "InnerEthType_rule")
        self.check_asic_rule_absent(dvs)

    def test_EgressAclInnerL4SrcPort(self, dvs):
        self.setup_db(dvs)

        # Create InnerL4SrcPort Acl rule
        fvPairs = [("priority", "1000"), ("PACKET_ACTION", "DROP"), ("INNER_L4_SRC_PORT", "999")]
        self.create_acl_rule(fvPairs, "InnerL4SrcPort_rule")

        # Verify Acl rule in ASIC DB
        fv_tuple = ("SAI_ACL_ENTRY_ATTR_FIELD_INNER_L4_SRC_PORT", "999&mask:0xffff")
        self.verify_acl_rule_asic_fvs(dvs, fv_tuple)

        # Remove Acl rule
        self.remove_acl_rule("egress_acl_table", "InnerL4SrcPort_rule")
        self.check_asic_rule_absent(dvs)

    def test_EgressAclInnerL4DstPort(self, dvs):
        self.setup_db(dvs)

        # Create InnerL4DstPort Acl rule
        fvPairs = [("priority", "1000"), ("PACKET_ACTION", "DROP"), ("INNER_L4_DST_PORT", "999")]
        self.create_acl_rule(fvPairs, "InnerL4DstPort_rule")

        # Verify Acl rule in ASIC DB
        fv_tuple = ("SAI_ACL_ENTRY_ATTR_FIELD_INNER_L4_DST_PORT", "999&mask:0xffff")
        self.verify_acl_rule_asic_fvs(dvs, fv_tuple)

        # Remove Acl rule
        self.remove_acl_rule("egress_acl_table", "InnerL4DstPort_rule")
        self.check_asic_rule_absent(dvs)

    def test_EgressAclTableDeletion(self, dvs):
        self.setup_db(dvs)

        # Remove Acl table
        self.remove_acl_table("egress_acl_table")
        self.check_asic_table_absent(dvs)
