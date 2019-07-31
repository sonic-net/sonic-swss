from swsscommon import swsscommon
import time
import re
import json
import pytest
import platform
from distutils.version import StrictVersion


class TestVlan(object):
    def check_syslog(self, dvs, marker, process, err_log, vlan_str, expected_cnt):
        (exitcode, num) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep %s | grep \"%s\" | grep -i %s | wc -l" % (marker, process, err_log, vlan_str)])
        assert num.strip() == str(expected_cnt)

    def create_acl_table(self, dvs, table_name, ports):
        tbl = swsscommon.Table(dvs.cdb, "ACL_TABLE")
        fvs = swsscommon.FieldValuePairs([("POLICY_DESC", table_name),
                                          ("TYPE", "L3"),
                                          ("PORTS", ports)])
        tbl.set(table_name, fvs)
        time.sleep(1)

    def remove_acl_table(self, dvs, table_name):
        tbl = swsscommon.Table(dvs.cdb, "ACL_TABLE")
        tbl._del(table_name)
        time.sleep(1)

    def check_asic_table_existed(self, dvs, vlanid):
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan = tbl.getKeys()
        assert dvs.asicdb.default_vlan_id in vlan
        vlan = [k for k in vlan if k not in dvs.asicdb.default_vlan_id]
        assert len(vlan) == 1

        (status, fvs) = tbl.get(vlan[0])
        assert status == True
        assert len(fvs) == 2
        for fv in fvs:
            if fv[0] == "SAI_VLAN_ATTR_INGRESS_ACL":
                table_group_id = fv[1]
            elif fv[0] == "SAI_VLAN_ATTR_VLAN_ID":
                assert vlanid == fv[1]
            else:
                assert False

        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        (status, fvs) = tbl.get(table_group_id)
        assert status == True
        assert len(fvs) == 3
        for fv in fvs:
            if fv[0] == "SAI_ACL_TABLE_GROUP_ATTR_ACL_STAGE":
                assert fv[1] == "SAI_ACL_STAGE_INGRESS"
            elif fv[0] == "SAI_ACL_TABLE_GROUP_ATTR_ACL_BIND_POINT_TYPE_LIST":
                assert fv[1] == "1:SAI_ACL_BIND_POINT_TYPE_VLAN"
            elif fv[0] == "SAI_ACL_TABLE_GROUP_ATTR_TYPE":
                assert fv[1] == "SAI_ACL_TABLE_GROUP_TYPE_PARALLEL"
            else:
                assert False

        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER")
        member = tbl.getKeys()[0]
        (status, fvs) = tbl.get(member)
        assert status == True
        assert len(fvs) == 3
        for fv in fvs:
            if fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_GROUP_ID":
                assert table_group_id == fv[1]
            elif fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_ACL_TABLE_ID":
                table_id = fv[1]
            elif fv[0] == "SAI_ACL_TABLE_GROUP_MEMBER_ATTR_PRIORITY":
                assert fv[1] == "100"
            else:
                assert False

        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE")
        (status, fvs) = tbl.get(table_id)
        assert status == True

    def check_asic_tablegroup_absent(self, dvs):
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        acl_table_groups = tbl.getKeys()
        assert len(acl_table_groups) == 0

    def check_asic_tablegroupmember_absent(self, dvs):
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER")
        acl_table_group_members = tbl.getKeys()
        assert len(acl_table_group_members) == 0

    def test_VlanAddRemove(self, dvs, testlog):
        dvs.setup_db()

        # create vlan
        dvs.create_vlan("2")

        # check asic database
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 1
        vlan_oid = vlan_entries[0]

        (status, fvs) = tbl.get(vlan_oid)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_VLAN_ATTR_VLAN_ID":
                assert fv[1] == "2"

        # create vlan member
        dvs.create_vlan_member("2", "Ethernet0")

        # check asic database
        bridge_port_map = {}
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
        bridge_port_entries = tbl.getKeys()
        for key in bridge_port_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_BRIDGE_PORT_ATTR_PORT_ID":
                    bridge_port_map[key] = fv[1]

        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
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
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = tbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True
        assert "SAI_PORT_ATTR_PORT_VLAN_ID" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_PORT_VLAN_ID":
                assert fv[1] == "2"

        # check host interface vlan tag
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF")
        (status, fvs) = tbl.get(dvs.asicdb.hostifnamemap["Ethernet0"])
        assert status == True
        assert "SAI_HOSTIF_ATTR_VLAN_TAG" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_HOSTIF_ATTR_VLAN_TAG":
                assert fv[1] == "SAI_HOSTIF_VLAN_TAG_KEEP"

        # remove vlan member
        dvs.remove_vlan_member("2", "Ethernet0")

        # remove vlan
        dvs.remove_vlan("2")

    def test_MultipleVlan(self, dvs, testlog):
        return
        dvs.setup_db()

        # create vlan and vlan members
        dvs.create_vlan("18")
        dvs.create_vlan_member("18", "Ethernet0")
        dvs.create_vlan_member("18", "Ethernet4")
        dvs.create_vlan_member("18", "Ethernet8")

        # check asic database
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 1

        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
        vlan_member_entries = tbl.getKeys()
        assert len(vlan_member_entries) == 3

        # remove vlan members
        dvs.remove_vlan_member("18", "Ethernet0")
        dvs.remove_vlan_member("18", "Ethernet4")
        dvs.remove_vlan_member("18", "Ethernet8")

        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
        vlan_member_entries = tbl.getKeys()
        assert len(vlan_member_entries) == 0

        # create vlan and vlan members
        dvs.create_vlan("188")
        dvs.create_vlan_member("188", "Ethernet20")
        dvs.create_vlan_member("188", "Ethernet24")
        dvs.create_vlan_member("188", "Ethernet28")

        # check asic database
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 2

        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
        vlan_member_entries = tbl.getKeys()
        assert len(vlan_member_entries) == 3

        # create vlan members
        dvs.create_vlan_member("18", "Ethernet40")
        dvs.create_vlan_member("18", "Ethernet44")
        dvs.create_vlan_member("18", "Ethernet48")

        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
        vlan_member_entries = tbl.getKeys()
        assert len(vlan_member_entries) == 6

        # remove vlan members
        dvs.remove_vlan_member("18", "Ethernet40")
        dvs.remove_vlan_member("18", "Ethernet44")
        dvs.remove_vlan_member("18", "Ethernet48")

        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
        vlan_member_entries = tbl.getKeys()
        assert len(vlan_member_entries) == 3

        # remove vlan members
        dvs.remove_vlan_member("188", "Ethernet20")
        dvs.remove_vlan_member("188", "Ethernet24")
        dvs.remove_vlan_member("188", "Ethernet28")

        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
        vlan_member_entries = tbl.getKeys()
        assert len(vlan_member_entries) == 0

        # remove vlans
        dvs.remove_vlan("18")
        dvs.remove_vlan("188")

        # check asic database
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 0

    def test_VlanIncrementalConfig(self, dvs, testlog):
        dvs.setup_db()

        # create vlan
        dvs.create_vlan("2")

        # check asic database
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 1
        vlan_oid = vlan_entries[0]

        (status, fvs) = tbl.get(vlan_oid)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_VLAN_ATTR_VLAN_ID":
                assert fv[1] == "2"

        # create vlan member
        dvs.create_vlan_member("2", "Ethernet0")

        # check asic database
        bridge_port_map = {}
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
        bridge_port_entries = tbl.getKeys()
        for key in bridge_port_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_BRIDGE_PORT_ATTR_PORT_ID":
                    bridge_port_map[key] = fv[1]

        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
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

        # assign IP to interface
        dvs.add_ip_address("Vlan2", "20.0.0.8/29")

        # check ASIC router interface database for mtu changes.
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # one loopback router interface one vlan based router interface
        assert len(intf_entries) == 2

        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            # a Vlan based router interface has five field/value tuples
            if len(fvs) == 5:
                for fv in fvs:
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_TYPE":
                        assert fv[1] == "SAI_ROUTER_INTERFACE_TYPE_VLAN"
                    # assert the default value 9100 for the router interface
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_MTU":
                        assert fv[1] == "9100"

        # configure MTU to interface
        dvs.set_mtu("Vlan2", "8888")
        intf_entries = tbl.getKeys()
        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            # a Vlan based router interface has five field/value tuples
            if len(fvs) == 5:
                for fv in fvs:
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_TYPE":
                        assert fv[1] == "SAI_ROUTER_INTERFACE_TYPE_VLAN"
                    # assert the new value set to the router interface
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_MTU":
                        assert fv[1] == "8888"

        # check appDB for VLAN admin_status change.
        tbl = swsscommon.Table(dvs.pdb, "VLAN_TABLE")
        dvs.set_interface_status("Vlan2", "down")
        (status, fvs) = tbl.get("Vlan2")
        assert status == True
        for fv in fvs:
            if fv[0] == "admin_status":
                assert fv[1] == "down"

        dvs.set_interface_status("Vlan2", "up")
        (status, fvs) = tbl.get("Vlan2")
        assert status == True
        for fv in fvs:
            if fv[0] == "admin_status":
                assert fv[1] == "up"

        # remove IP from interface
        dvs.remove_ip_address("Vlan2", "20.0.0.8/29")

        # remove vlan member
        dvs.remove_vlan_member("2", "Ethernet0")

        # remove vlan
        dvs.remove_vlan("2")


    @pytest.mark.skipif(StrictVersion(platform.linux_distribution()[1]) <= StrictVersion('8.9'), reason="Debian 8.9 or before has no support")
    @pytest.mark.parametrize("test_input, expected", [
        (["Vla",  "2"], 0),
        (["VLAN", "3"], 0),
        (["vlan", "4"], 0),
        (["Vlan", "5"], 1),
    ])
    def test_AddVlanWithIncorrectKeyPrefix(self, dvs, testlog, test_input, expected):
        dvs.setup_db()
        marker = dvs.add_log_marker()
        vlan_prefix = test_input[0]
        vlan = test_input[1]

        # create vlan
        tbl = swsscommon.Table(dvs.cdb, "VLAN")
        fvs = swsscommon.FieldValuePairs([("vlanid", vlan)])
        tbl.set(vlan_prefix + vlan, fvs)
        time.sleep(1)

        # check asic database
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == expected

        if len(vlan_entries) == 0:
            # check error log
            self.check_syslog(dvs, marker, "vlanmgrd", "Invalid key format. No 'Vlan' prefix:", vlan_prefix+vlan, 1)
        else:
            #remove vlan
            dvs.remove_vlan(vlan)

    @pytest.mark.skipif(StrictVersion(platform.linux_distribution()[1]) <= StrictVersion('8.9'), reason="Debian 8.9 or before has no support")
    @pytest.mark.parametrize("test_input, expected", [
        (["Vlan", "abc"], 0),
        (["Vlan", "a3"],  0),
        (["Vlan", ""],    0),
        (["Vlan", "5"], 1),
    ])
    def test_AddVlanWithIncorrectValueType(self, dvs, testlog, test_input, expected):
        dvs.setup_db()
        marker = dvs.add_log_marker()
        vlan_prefix = test_input[0]
        vlan = test_input[1]

        # create vlan
        tbl = swsscommon.Table(dvs.cdb, "VLAN")
        fvs = swsscommon.FieldValuePairs([("vlanid", vlan)])
        tbl.set(vlan_prefix + vlan, fvs)
        time.sleep(1)

        # check asic database
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == expected

        if len(vlan_entries) == 0:
            # check error log
            self.check_syslog(dvs, marker, "vlanmgrd", "Invalid key format. Not a number after \'Vlan\' prefix:", vlan_prefix+vlan, 1)
        else:
            #remove vlan
            dvs.remove_vlan(vlan)

    def test_AddVlanMemberWithNonExistVlan(self, dvs, testlog):
        dvs.setup_db()
        marker = dvs.add_log_marker()
        vlan = "2"

        # create vlan member
        dvs.create_vlan_member(vlan, "Ethernet0")

        # check asic database
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 0

        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
        vlan_member_entries = tbl.getKeys()
        assert len(vlan_member_entries) == 0

        # remove vlan member from cfgdb
        dvs.remove_vlan_member(vlan, "Ethernet0")

    def test_RemoveNonexistentVlan(self, dvs, testlog):
        dvs.setup_db()
        marker = dvs.add_log_marker()
        vlan = "2"

        # check asic database
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 0

        # remove nonexistent vlan
        dvs.remove_vlan(vlan)

        # create vlan
        dvs.create_vlan(vlan)

        # check asic database
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 1

        # remove vlan
        dvs.remove_vlan(vlan)

    @pytest.mark.skip(reason="AddMaxVlan take too long to execute")
    def test_AddMaxVlan(self, dvs, testlog):
        dvs.setup_db()

        min_vid = 2
        max_vid = 4094

        # create max vlan
        vlan = min_vid
        while vlan <= max_vid:
            dvs.create_vlan(str(vlan))
            vlan += 1

        # check asic database
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == (4094-1)

        # remove all vlan
        vlan = min_vid
        while vlan <= max_vid:
            dvs.remove_vlan(str(vlan))
            vlan += 1

        # check asic database
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 0

    def test_RemoveVlanWithRouterInterface(self, dvs, testlog):
        dvs.setup_db()
        marker = dvs.add_log_marker()

        # create vlan
        dvs.create_vlan("100")

        # check asic database
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 1
        vlan_oid = vlan_entries[0]

        (status, fvs) = tbl.get(vlan_oid)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_VLAN_ATTR_VLAN_ID":
                assert fv[1] == "100"

        # assign IP to interface
        dvs.add_ip_address("Vlan100", "20.0.0.8/29")

        # check ASIC router interface database for mtu changes.
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # one loopback router interface one vlan based router interface
        assert len(intf_entries) == 2

        # remove vlan
        dvs.remove_vlan("100")

        # check asic database still contains the vlan
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 1
        vlan_oid = vlan_entries[0]

        (status, fvs) = tbl.get(vlan_oid)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_VLAN_ATTR_VLAN_ID":
                assert fv[1] == "100"

        # remove IP from interface
        dvs.remove_ip_address("Vlan100", "20.0.0.8/29")

        # remove vlan
        dvs.remove_vlan("100")

        # check asic database does not contain the vlan anymore
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 0

    # Frist create ACL table
    # Second create Vlan
    # Third remove Vlan
    def test_VlanBeforeAcl(self, dvs, testlog):
        dvs.setup_db()

        # create ACL table
        self.create_acl_table(dvs, "VLAN_ACL_TABLE", "Vlan1234")

        # create Vlan
        dvs.create_vlan("1234")

        # check ASIC table
        self.check_asic_table_existed(dvs,"1234")

        # remove port channel
        dvs.remove_vlan("1234")

        # check ASIC table group and member
        self.check_asic_tablegroup_absent(dvs)
        self.check_asic_tablegroupmember_absent(dvs)

        # remove ACL table
        self.remove_acl_table(dvs, "VLAN_ACL_TABLE")
