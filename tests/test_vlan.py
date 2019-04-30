from swsscommon import swsscommon
import time
import re
import json
import pytest
import platform
from distutils.version import StrictVersion


class TestVlan(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

    def create_vlan(self, vlan):
        tbl = swsscommon.Table(self.cdb, "VLAN")
        fvs = swsscommon.FieldValuePairs([("vlanid", vlan)])
        tbl.set("Vlan" + vlan, fvs)
        time.sleep(1)

    def remove_vlan(self, vlan):
        tbl = swsscommon.Table(self.cdb, "VLAN")
        tbl._del("Vlan" + vlan)
        time.sleep(1)

    def create_vlan_member(self, vlan, interface):
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        fvs = swsscommon.FieldValuePairs([("tagging_mode", "untagged")])
        tbl.set("Vlan" + vlan + "|" + interface, fvs)
        time.sleep(1)

    def remove_vlan_member(self, vlan, interface):
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        tbl._del("Vlan" + vlan + "|" + interface)
        time.sleep(1)

    def check_syslog(self, dvs, marker, err_log, vlan_str, expected_cnt):
        (exitcode, num) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep vlanmgrd | grep \"%s\" | grep -i %s | wc -l" % (marker, err_log, vlan_str)])
        assert num.strip() == str(expected_cnt)

    def test_VlanAddRemove(self, dvs, testlog):
        self.setup_db(dvs)

        # create vlan
        self.create_vlan("2")

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 1
        vlan_oid = vlan_entries[0]

        (status, fvs) = tbl.get(vlan_oid)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_VLAN_ATTR_VLAN_ID":
                assert fv[1] == "2"

        # create vlan member
        self.create_vlan_member("2", "Ethernet0")

        # check asic database
        bridge_port_map = {}
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
        bridge_port_entries = tbl.getKeys()
        for key in bridge_port_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_BRIDGE_PORT_ATTR_PORT_ID":
                    bridge_port_map[key] = fv[1]

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
        vlan_member_entries = tbl.getKeys()
        assert len(vlan_member_entries) == 1

        (status, fvs) = tbl.get(vlan_member_entries[0])
        assert status == True
        assert len(fvs) == 3
        for fv in fvs:
            if fv[0] == "SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE":
                assert fv[1] == "SAI_VLAN_TAGGING_MODE_UNTAGGED"
            elif fv[0] == "SAI_VLAN_MEMBER_ATTR_VLAN_ID":
                assert fv[1] == vlan_oid
            elif fv[0] == "SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID":
                assert dvs.asicdb.portoidmap[bridge_port_map[fv[1]]] == "Ethernet0"
            else:
                assert False

        # check port pvid
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = tbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True
        assert "SAI_PORT_ATTR_PORT_VLAN_ID" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_PORT_VLAN_ID":
                assert fv[1] == "2"

        # check host interface vlan tag
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF")
        (status, fvs) = tbl.get(dvs.asicdb.hostifnamemap["Ethernet0"])
        assert status == True
        assert "SAI_HOSTIF_ATTR_VLAN_TAG" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_HOSTIF_ATTR_VLAN_TAG":
                assert fv[1] == "SAI_HOSTIF_VLAN_TAG_KEEP"

        # remove vlan member
        self.remove_vlan_member("2", "Ethernet0")

        # remvoe vlan
        self.remove_vlan("2")

    def test_MultipleVlan(self, dvs, testlog):
        return
        self.setup_db(dvs)

        # create vlan and vlan members
        self.create_vlan("18")
        self.create_vlan_member("18", "Ethernet0")
        self.create_vlan_member("18", "Ethernet4")
        self.create_vlan_member("18", "Ethernet8")

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 1

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
        vlan_member_entries = tbl.getKeys()
        assert len(vlan_member_entries) == 3

        # remove vlan members
        self.remove_vlan_member("18", "Ethernet0")
        self.remove_vlan_member("18", "Ethernet4")
        self.remove_vlan_member("18", "Ethernet8")

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
        vlan_member_entries = tbl.getKeys()
        assert len(vlan_member_entries) == 0

        # create vlan and vlan members
        self.create_vlan("188")
        self.create_vlan_member("188", "Ethernet20")
        self.create_vlan_member("188", "Ethernet24")
        self.create_vlan_member("188", "Ethernet28")

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 2

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
        vlan_member_entries = tbl.getKeys()
        assert len(vlan_member_entries) == 3

        # create vlan members
        self.create_vlan_member("18", "Ethernet40")
        self.create_vlan_member("18", "Ethernet44")
        self.create_vlan_member("18", "Ethernet48")

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
        vlan_member_entries = tbl.getKeys()
        assert len(vlan_member_entries) == 6

        # remove vlan members
        self.remove_vlan_member("18", "Ethernet40")
        self.remove_vlan_member("18", "Ethernet44")
        self.remove_vlan_member("18", "Ethernet48")

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
        vlan_member_entries = tbl.getKeys()
        assert len(vlan_member_entries) == 3

        # remove vlan members
        self.remove_vlan_member("188", "Ethernet20")
        self.remove_vlan_member("188", "Ethernet24")
        self.remove_vlan_member("188", "Ethernet28")

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
        vlan_member_entries = tbl.getKeys()
        assert len(vlan_member_entries) == 0

        # remove vlans
        self.remove_vlan("18")
        self.remove_vlan("188")

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 0

    @pytest.mark.parametrize("test_input, expected", [
        (["Vla",  "2"], 0),
        (["VLAN", "3"], 0),
        (["vlan", "4"], 0),
        (["Vlan", "5"], 1),
    ])
    def test_AddVlanWithIncorrectKeyPrefix(self, dvs, testlog, test_input, expected):
        self.setup_db(dvs)
        marker = dvs.add_log_marker()
        vlan_prefix = test_input[0]
        vlan = test_input[1]

        # create vlan
        tbl = swsscommon.Table(self.cdb, "VLAN")
        fvs = swsscommon.FieldValuePairs([("vlanid", vlan)])
        tbl.set(vlan_prefix + vlan, fvs)
        time.sleep(1)

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == expected

        if len(vlan_entries) == 0:
            # check error log
            self.check_syslog(dvs, marker, "Invalid key format. No 'Vlan' prefix:", vlan_prefix+vlan, 1)
        else:
            #remove vlan
            self.remove_vlan(vlan)

    @pytest.mark.parametrize("test_input, expected", [
        (["Vlan", "abc"], 0),
        (["Vlan", "a3"],  0),
        (["Vlan", ""],    0),
        (["Vlan", "5"], 1),
    ])
    def test_AddVlanWithIncorrectValueType(self, dvs, testlog, test_input, expected):
        self.setup_db(dvs)
        marker = dvs.add_log_marker()
        vlan_prefix = test_input[0]
        vlan = test_input[1]

        # create vlan
        tbl = swsscommon.Table(self.cdb, "VLAN")
        fvs = swsscommon.FieldValuePairs([("vlanid", vlan)])
        tbl.set(vlan_prefix + vlan, fvs)
        time.sleep(1)

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == expected

        if len(vlan_entries) == 0:
            # check error log
            self.check_syslog(dvs, marker, "Invalid key format. Not a number after \'Vlan\' prefix:", vlan_prefix+vlan, 1)
        else:
            #remove vlan
            self.remove_vlan(vlan)

