from swsscommon import swsscommon
import time
import re
import json

class TestMclagAcl(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

    def create_entry(self, tbl, key, pairs):
        fvs = swsscommon.FieldValuePairs(pairs)
        tbl.set(key, fvs)
        time.sleep(1)

    def remove_entry(self, tbl, key):
        tbl._del(key)
        time.sleep(1)

    def create_entry_tbl(self, db, table, key, pairs):
        tbl = swsscommon.Table(db, table)
        self.create_entry(tbl, key, pairs)

    def remove_entry_tbl(self, db, table, key):
        tbl = swsscommon.Table(db, table)
        self.remove_entry(tbl, key)

    def create_entry_pst(self, db, table, key, pairs):
        tbl = swsscommon.ProducerStateTable(db, table)
        self.create_entry(tbl, key, pairs)

    def remove_entry_pst(self, db, table, key):
        tbl = swsscommon.ProducerStateTable(db, table)
        self.remove_entry(tbl, key)

    def get_acl_table_id(self, dvs):
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        keys = tbl.getKeys()

        for k in  dvs.asicdb.default_acl_tables:
            assert k in keys

        acl_tables = [k for k in keys if k not in dvs.asicdb.default_acl_tables]
        if len(acl_tables) == 1:
            return acl_tables[0]
        else:
            return None

    def verify_acl_group_num(self, expt):
        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
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

    def verify_acl_group_member(self, acl_group_ids, acl_table_id):
        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER")
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

    def verify_acl_port_binding(self, dvs, bind_ports):
        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        acl_table_groups = atbl.getKeys()
        assert len(acl_table_groups) == len(bind_ports)

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        port_groups = []
        for p in [dvs.asicdb.portnamemap[portname] for portname in bind_ports]:
            (status, fvs) = atbl.get(p)
            for fv in fvs:
                if fv[0] == "SAI_PORT_ATTR_INGRESS_ACL":
                    assert fv[1] in acl_table_groups
                    port_groups.append(fv[1])

        assert len(port_groups) == len(bind_ports)
        assert set(port_groups) == set(acl_table_groups)

    def test_AclTableCreation(self, dvs, testlog):
        """
        hmset ACL_TABLE_TABLE:mclag policy_desc "Mclag egress port isolate acl" type L3 ports Ethernet0,Ethernet4
        """
        self.setup_db(dvs)

        # create ACL_TABLE_TABLE in app db
        bind_ports = ["Ethernet0", "Ethernet4"]
        self.create_entry_pst(
            self.pdb,
            "ACL_TABLE_TABLE", "mclag",
            [
                ("policy_desc", "Mclag egress port isolate acl"),
                ("type", "L3"),
                ("ports", ",".join(bind_ports)),
            ]
        )

        # check acl table in asic db
        acl_table_id = self.get_acl_table_id(dvs)
        assert acl_table_id is not None

        # check acl table group in asic db
        self.verify_acl_group_num(2)

        # get acl table group ids and verify the id numbers
        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        acl_group_ids = atbl.getKeys()
        assert len(acl_group_ids) == 2

        # check acl table group member
        self.verify_acl_group_member(acl_group_ids, acl_table_id)

        # check port binding
        self.verify_acl_port_binding(dvs, bind_ports)

    def test_AclRuleOutPorts(self, dvs, testlog):
        """
        hmset ACL_RULE_TABLE:mclag:mclag IP_TYPE ANY PACKET_ACTION DROP OUT_PORTS Ethernet8,Ethernet12
        """
        self.setup_db(dvs)

        # create acl rule
        bind_ports = ["Ethernet8", "Ethernet12"]
        self.create_entry_pst(
            self.pdb,
            "ACL_RULE_TABLE", "mclag:mclag",
            [
                ("IP_TYPE", "ANY"),
                ("PACKET_ACTION", "DROP"),
                ("OUT_PORTS", ",".join(bind_ports)),
            ]
        )

        # check acl rule table in asic db
        acl_table_id = self.get_acl_table_id(dvs)

        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()

        acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entry) == 1

        (status, fvs) = atbl.get(acl_entry[0])
        assert status == True

        value = dict(fvs)
        assert value["SAI_ACL_ENTRY_ATTR_TABLE_ID"] == acl_table_id
        assert value["SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION"] == "SAI_PACKET_ACTION_DROP"
        assert value["SAI_ACL_ENTRY_ATTR_FIELD_ACL_IP_TYPE"] == "SAI_ACL_IP_TYPE_ANY&mask:0xffffffffffffffff"
        out_ports = value["SAI_ACL_ENTRY_ATTR_FIELD_OUT_PORTS"]
        assert out_ports.startswith("2:")
        assert dvs.asicdb.portnamemap["Ethernet8"] in out_ports
        assert dvs.asicdb.portnamemap["Ethernet12"] in out_ports

        # remove acl rule
        self.remove_entry_pst(
            self.pdb,
            "ACL_RULE_TABLE", "mclag:mclag"
        )

        # check acl rule in asic db
        (status, fvs) = atbl.get(acl_entry[0])
        assert status == False

        # remove acl
        self.remove_entry_pst(
            self.pdb,
            "ACL_TABLE_TABLE", "mclag:mclag"
        )

        # check acl in asic db
        acl_table_id = self.get_acl_table_id(dvs)
        assert acl_table_id is None