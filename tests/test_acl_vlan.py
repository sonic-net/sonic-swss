from swsscommon import swsscommon
import time
import re
import json
import pytest
import platform


class TestVlanAcl(object):
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

    # Frist create Vlan
    # Second create ACL table
    def test_VlanAfterAcl(self, dvs):
        dvs.setup_db()
        dvs.runcmd("crm config polling interval 1")
        time.sleep(2)

        used_counter = dvs.getCrmCounterValue('ACL_STATS:INGRESS:VLAN', 'crm_stats_acl_group_used')
        # create Vlan
        dvs.create_vlan("1234")

        # create ACL table
        self.create_acl_table(dvs, "VLAN_ACL_TABLE", "Vlan1234")

        time.sleep(2)

        new_used_counter = dvs.getCrmCounterValue('ACL_STATS:INGRESS:VLAN', 'crm_stats_acl_group_used')
        if used_counter is None:
            used_counter = 0
        assert new_used_counter - used_counter == 1
        # check ASIC table
        self.check_asic_table_existed(dvs,"1234")

        # remove ACL table
        self.remove_acl_table(dvs, "VLAN_ACL_TABLE")

        time.sleep(2)

        # check ASIC table group and member
        self.check_asic_tablegroupmember_absent(dvs)

        # remove Vlan
        dvs.remove_vlan("1234")

        # slow down crm polling
        dvs.runcmd("crm config polling interval 10000")

    # Frist create ACL table
    # Second create Vlan
    def test_VlanBeforeAcl(self, dvs, testlog):
        dvs.setup_db()

        # create ACL table
        self.create_acl_table(dvs, "VLAN_ACL_TABLE", "Vlan1234")

        # create Vlan
        dvs.create_vlan("1234")

        # check ASIC table
        self.check_asic_table_existed(dvs,"1234")

        # remove Vlan
        dvs.remove_vlan("1234")

        # check ASIC table group and member
        self.check_asic_tablegroup_absent(dvs)
        self.check_asic_tablegroupmember_absent(dvs)

        # remove ACL table
        self.remove_acl_table(dvs, "VLAN_ACL_TABLE")

