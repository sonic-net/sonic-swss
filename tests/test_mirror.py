# This test suite covers the functionality of mirror feature in SwSS

import time

from swsscommon import swsscommon


class TestMirror(object):
    def get_acl_table_id(self, dvs):
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        keys = tbl.getKeys()
        for k in dvs.asicdb.default_acl_tables:
            assert k in keys

        acl_tables = [k for k in keys if k not in dvs.asicdb.default_acl_tables]
        assert len(acl_tables) == 1

        return acl_tables[0]

    def get_mirror_session_id(self, dvs):
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        mirror_sessions = tbl.getKeys()
        assert len(mirror_sessions) == 1

        return mirror_sessions[0]

    def test_AclMirrorTableCreation(self, dvs):
        pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

        bind_ports = ["Ethernet0", "Ethernet4"]
        tbl = swsscommon.Table(cdb, "ACL_TABLE")

        fvs = swsscommon.FieldValuePairs([("POLICY_DESC", "MIRROR_TEST"),
                                          ("TYPE", "MIRROR"),
                                          ("PORTS", ",".join(bind_ports))])
        # create the ACL table
        tbl.set("EVERFLOW_TABLE", fvs)
        time.sleep(1)

        # assert the table is created
        tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        acl_table_ids = tbl.getKeys()
        assert len(acl_table_ids) == 2

        tbl = swsscommon.Table(cdb, "MIRROR_SESSION")
        fvs = swsscommon.FieldValuePairs([("src_ip", "10.1.0.32"),
                                          ("dst_ip", "10.20.30.40")])
        # create the mirror session
        tbl.set("EVERFLOW_SESSION", fvs)
        time.sleep(1)

        # assert the mirror session is created
        tbl = swsscommon.Table(pdb, "MIRROR_SESSION")
        mirror_sessions = tbl.getKeys()
        assert len(mirror_sessions) == 1

        # assert the mirror session is inactive
        (status, fvs) = tbl.get(mirror_sessions[0])
        assert status == True
        assert len(fvs) == 1
        for fv in fvs:
            if fv[0] == "status":
                assert fv[1] == "inactive"
            else:
                assert False

    def test_MirrorSessionActivation(self, dvs):
        # assign the IP address to Ethernet0
        dvs.runcmd("ifconfig Ethernet0 10.0.0.0/24 up")
        time.sleep(1)

        pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        tbl = swsscommon.ProducerStateTable(pdb, "NEIGH_TABLE")
        fvs = swsscommon.FieldValuePairs([("NEIGH", "02:04:06:08:10:12"),
                                          ("FAMILY", "IPv4")])
        # create the neighbor entry associated with Ethernet0
        tbl.set("Ethernet0:10.0.0.1", fvs)
        time.sleep(1)

        # add the route of mirror session destination via the neighbor
        dvs.runcmd("ip route add 10.20.30.40/32 via 10.0.0.1")
        time.sleep(1)

        # assert the mirror session is active
        tbl = swsscommon.Table(pdb, "MIRROR_SESSION")
        (status, fvs) = tbl.get("EVERFLOW_SESSION")
        assert status == True
        assert len(fvs) == 1
        for fv in fvs:
            if fv[0] == "status":
                fv[1] == "active"

        # assert the mirror session is created
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        mirror_session_ids = tbl.getKeys()
        assert len(mirror_session_ids) == 1

    def test_AclRuleDscpWithoutMask(self, dvs):

        """
        hmset ACL_RULE|EVERFLOW_TABLE|EVERFLOW_DSCP_TEST_RULE_1 priority 1000 PACKET_ACTION FORWARD DSCP 48
        """

        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

        tbl = swsscommon.Table(cdb, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("PRIORITY", "1000"),
                                          ("MIRROR_ACTION", "EVERFLOW_SESSION"),
                                          ("DSCP", "48")])
        # create the ACL rule contains DSCP match field with a mask
        tbl.set("EVERFLOW_TABLE|EVERFLOW_DSCP_TEST_RULE_1", fvs)
        time.sleep(1)

        test_acl_table_id = self.get_acl_table_id(dvs)
        test_mirror_session_id = self.get_mirror_session_id(dvs)

        # assert the ACL rule is created
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()
        acl_entries = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entries) == 1

        # assert the ACL rule content is correct
        (status, fvs) = atbl.get(acl_entries[0])
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
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_DSCP":
                assert fv[1] == "48&mask:0x3f"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS":
                assert fv[1] == "1:" + test_mirror_session_id
            else:
                assert False

        # remove the ACL rule
        tbl._del("EVERFLOW_TABLE|EVERFLOW_DSCP_TEST_RULE_1")
        time.sleep(1)

        # assert the ACL rule is removed
        (status, fvs) = atbl.get(acl_entries[0])
        assert status == False

    def test_AclRuleDscpWithMask(self, dvs):

        """
        hmset ACL_RULE|EVERFLOW_TABLE|EVERFLOW_DSCP_TEST_RULE_2 priority 1000 PACKET_ACTION FORWARD DSCP 16/16
        """

        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

        tbl = swsscommon.Table(cdb, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("PRIORITY", "1000"),
                                          ("MIRROR_ACTION", "EVERFLOW_SESSION"),
                                          ("DSCP", "16/16")])
        # create the ACL rule contains DSCP match field with a mask
        tbl.set("EVERFLOW_TABLE|EVERFLOW_DSCP_TEST_RULE_2", fvs)
        time.sleep(1)

        test_acl_table_id = self.get_acl_table_id(dvs)
        test_mirror_session_id = self.get_mirror_session_id(dvs)

        # assert the ACL rule is created
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        keys = atbl.getKeys()
        acl_entries = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
        assert len(acl_entries) == 1

        # assert the ACL rule content is correct
        (status, fvs) = atbl.get(acl_entries[0])
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
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_DSCP":
                assert fv[1] == "16&mask:0x10"
            elif fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS":
                assert fv[1] == "1:" + test_mirror_session_id
            else:
                assert False

        # remove the ACL rule
        tbl._del("EVERFLOW_TABLE|EVERFLOW_DSCP_TEST_RULE_2")
        time.sleep(1)

        # assert the ACL rule is removed
        (status, fvs) = atbl.get(acl_entries[0])
        assert status == False

    def test_AclMirrorTableDeletion(self, dvs):
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

        tbl = swsscommon.Table(cdb, "ACL_TABLE")
        # remove the ACL table
        tbl._del("EVERFLOW_TABLE")
        time.sleep(1)

        # assert the ACL table is removed
        tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        acl_table_ids = tbl.getKeys()
        assert len(acl_table_ids) == 3

        tbl = swsscommon.Table(cdb, "MIRROR_SESSION")
        # remove the mirror session
        tbl._del("EVERFLOW_SESSION")
        time.sleep(1)

        # assert the mirror session is created
        tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        mirror_session_ids = tbl.getKeys()
        assert len(mirror_session_ids) == 0
