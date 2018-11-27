from swsscommon import swsscommon

import time

class TestPortChannelAcl(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

    def create_acl_table(self, dvs):
        tbl = swsscommon.Table(self.cdb, "ACL_TABLE")
        fvs = swsscommon.FieldValuePairs([("POLICY_DESC", "CTRL_ACL_TEST"),
                                          ("TYPE", "CTRLPLANE"),
                                          ("SERVICES@", "SNMP")])
        tbl.set("CTRL_ACL_TABLE", fvs)
        time.sleep(1)

    def remove_acl_table(self, dvs):
        tbl = swsscommon.Table(self.cdb, "ACL_TABLE")
        tbl._del("CTRL_ACL_TABLE")
        time.sleep(1)

    def create_acl_rule(self, dvs):
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("PRIORITY", "88"),
                                          ("PACKET_ACTION", "FORWARD"),
                                          ("L4_SRC_PORT", "8888")])
        tbl.set("CTRL_ACL_TABLE|CTRL_ACL_RULE", fvs)
        time.sleep(1)

    def remove_acl_rule(self, dvs):
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        tbl._del("CTRL_ACL_TABLE|CTRL_ACL_RULE")
        time.sleep(1)

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

    def test_AclCtrl(self, dvs):
        self.setup_db(dvs)

        # create ACL table and ACL rule
        self.create_acl_table(dvs)
        self.create_acl_rule(dvs)

        # check ASIC table
        self.check_asic_table_absent(dvs)
        self.check_asic_rule_absent(dvs)

        # remove ACL table
        self.remove_acl_table(dvs)
        self.remove_acl_rule(dvs)

