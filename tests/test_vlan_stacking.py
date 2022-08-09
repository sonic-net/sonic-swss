import pytest
import time

from swsscommon import swsscommon

class TestVlanStacking(object):

    CFG_VLAN_STACKING_TBL_NAME = "VLAN_STACKING"
    CFG_VLAN_TRANSLATION_TBL_NAME = "VLAN_TRANSLATION"
    ASIC_TBL_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_STACK"

    def setup_db(self, dvs):
        self.adb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        # clear configDB before test
        tbl = swsscommon.Table(self.cdb, self.CFG_VLAN_STACKING_TBL_NAME)
        keys = tbl.getKeys()

        for key in keys:
            self.remove_vlan_stack(key)

        tbl = swsscommon.Table(self.cdb, self.CFG_VLAN_TRANSLATION_TBL_NAME)
        keys = tbl.getKeys()

        for key in keys:
            self.remove_vlan_xlate(key)

    def create_vlan_stack(self, key, attrs):
        tbl = swsscommon.Table(self.cdb, self.CFG_VLAN_STACKING_TBL_NAME)
        fvs = swsscommon.FieldValuePairs(attrs)
        tbl.set(key, fvs)

    def remove_vlan_stack(self, key):
        tbl = swsscommon.Table(self.cdb, self.CFG_VLAN_STACKING_TBL_NAME)
        tbl._del(key)

    def create_vlan_xlate(self, key, attrs):
        tbl = swsscommon.Table(self.cdb, self.CFG_VLAN_TRANSLATION_TBL_NAME)
        fvs = swsscommon.FieldValuePairs(attrs)
        tbl.set(key, fvs)

    def remove_vlan_xlate(self, key):
        tbl = swsscommon.Table(self.cdb, self.CFG_VLAN_TRANSLATION_TBL_NAME)
        tbl._del(key)

    def test_vlan_stacking(self, dvs):
        self.setup_db(dvs)

        port = "Ethernet8"
        s_vlan_id = "20"
        s_vlan_priority = "0"
        c_vlanids = "22"

        table_key = port + "|" +s_vlan_id

        attrs = [
            ("c_vlanids@", c_vlanids),
            ("s_vlan_priority", s_vlan_priority)
        ]

        self.create_vlan_stack(table_key, attrs)
        time.sleep(1)

        tbl = swsscommon.Table(self.adb, self.ASIC_TBL_NAME)
        asic_keys = tbl.getKeys()
        assert len(asic_keys) == 2

        for asic_key in asic_keys:
            (status, fvs) = tbl.get(asic_key)
            assert status == True

            fvs = dict(fvs)

            assert fvs.get("SAI_VLAN_STACK_ATTR_PORT", "") != ""

            if fvs.get("SAI_VLAN_STACK_ATTR_STAGE", "") == "SAI_VLAN_STACK_STAGE_EGRESS":
                assert len(fvs) == 5

                assert fvs["SAI_VLAN_STACK_ATTR_ORIGINAL_VLAN_ID"] == s_vlan_id
                assert fvs["SAI_VLAN_STACK_ATTR_MATCH_TYPE"] == "SAI_VLAN_STACK_MATCH_TYPE_OUTER"
                assert fvs["SAI_VLAN_STACK_ATTR_ACTION"] == "SAI_VLAN_STACK_ACTION_POP"
            else:
                assert len(fvs) == 7

                assert fvs["SAI_VLAN_STACK_ATTR_ORIGINAL_VLAN_ID"] == c_vlanids
                assert fvs["SAI_VLAN_STACK_ATTR_MATCH_TYPE"] == "SAI_VLAN_STACK_MATCH_TYPE_INNER"
                assert fvs["SAI_VLAN_STACK_ATTR_ACTION"] == "SAI_VLAN_STACK_ACTION_PUSH"
                assert fvs["SAI_VLAN_STACK_ATTR_VLAN_APPLIED_PRI"] == s_vlan_priority
                assert fvs["SAI_VLAN_STACK_ATTR_APPLIED_VLAN_ID"] == s_vlan_id

        self.remove_vlan_stack(table_key)
        time.sleep(1)

        asic_keys = tbl.getKeys()
        assert len(asic_keys) == 0

    def test_vlan_stacking_multi_c_vlanids(self, dvs):
        self.setup_db(dvs)

        port = "Ethernet8"
        s_vlan_id = "21"
        s_vlan_priority = "7"
        c_vlanids = "1..3,10"
        expected_c_vlan_list = ["1", "2", "3", "10"]

        table_key = port + "|" +s_vlan_id

        attrs = [
            ("c_vlanids@", c_vlanids),
            ("s_vlan_priority", s_vlan_priority)
        ]

        self.create_vlan_stack(table_key, attrs)
        time.sleep(1)

        tbl = swsscommon.Table(self.adb, self.ASIC_TBL_NAME)
        asic_keys = tbl.getKeys()
        assert len(asic_keys) == 5

        for asic_key in asic_keys:
            (status, fvs) = tbl.get(asic_key)
            assert status == True

            fvs = dict(fvs)

            assert fvs["SAI_VLAN_STACK_ATTR_PORT"] != ""

            if fvs.get("SAI_VLAN_STACK_ATTR_STAGE", "") == "SAI_VLAN_STACK_STAGE_EGRESS":
                assert len(fvs) == 5

                assert fvs["SAI_VLAN_STACK_ATTR_ORIGINAL_VLAN_ID"] == s_vlan_id
                assert fvs["SAI_VLAN_STACK_ATTR_MATCH_TYPE"] == "SAI_VLAN_STACK_MATCH_TYPE_OUTER"
                assert fvs["SAI_VLAN_STACK_ATTR_ACTION"] == "SAI_VLAN_STACK_ACTION_POP"
            else:
                assert len(fvs) == 7

                c_vlan = fvs["SAI_VLAN_STACK_ATTR_ORIGINAL_VLAN_ID"]
                assert c_vlan in expected_c_vlan_list
                expected_c_vlan_list.remove(c_vlan)

                assert fvs["SAI_VLAN_STACK_ATTR_MATCH_TYPE"] == "SAI_VLAN_STACK_MATCH_TYPE_INNER"
                assert fvs["SAI_VLAN_STACK_ATTR_ACTION"] == "SAI_VLAN_STACK_ACTION_PUSH"
                assert fvs["SAI_VLAN_STACK_ATTR_VLAN_APPLIED_PRI"] == s_vlan_priority
                assert fvs["SAI_VLAN_STACK_ATTR_APPLIED_VLAN_ID"] == s_vlan_id

        assert len(expected_c_vlan_list) == 0

        self.remove_vlan_stack(table_key)
        time.sleep(1)

        asic_keys = tbl.getKeys()
        assert len(asic_keys) == 0

    def test_vlan_translation(self, dvs):
        self.setup_db(dvs)

        port = "Ethernet8"
        s_vlan_id = "21"
        c_vlanid = "22"

        table_key = port + "|" +s_vlan_id

        attrs = [
            ("c_vlanid", c_vlanid)
        ]

        self.create_vlan_xlate(table_key, attrs)
        time.sleep(1)

        tbl = swsscommon.Table(self.adb, self.ASIC_TBL_NAME)
        asic_keys = tbl.getKeys()
        assert len(asic_keys) == 2

        for asic_key in asic_keys:
            (status, fvs) = tbl.get(asic_key)
            assert status == True
            assert len(fvs) == 6

            fvs = dict(fvs)

            assert fvs.get("SAI_VLAN_STACK_ATTR_PORT", "") != ""

            if fvs["SAI_VLAN_STACK_ATTR_STAGE"] == "SAI_VLAN_STACK_STAGE_EGRESS":
                assert fvs["SAI_VLAN_STACK_ATTR_ORIGINAL_VLAN_ID"] == s_vlan_id
                assert fvs["SAI_VLAN_STACK_ATTR_MATCH_TYPE"] == "SAI_VLAN_STACK_MATCH_TYPE_OUTER"
                assert fvs["SAI_VLAN_STACK_ATTR_ACTION"] == "SAI_VLAN_STACK_ACTION_SWAP"
                assert fvs["SAI_VLAN_STACK_ATTR_APPLIED_VLAN_ID"] == c_vlanid
            else:
                assert fvs["SAI_VLAN_STACK_ATTR_ORIGINAL_VLAN_ID"] == c_vlanid
                assert fvs["SAI_VLAN_STACK_ATTR_MATCH_TYPE"] == "SAI_VLAN_STACK_MATCH_TYPE_OUTER"
                assert fvs["SAI_VLAN_STACK_ATTR_ACTION"] == "SAI_VLAN_STACK_ACTION_SWAP"
                assert fvs["SAI_VLAN_STACK_ATTR_APPLIED_VLAN_ID"] == s_vlan_id

        self.remove_vlan_xlate(table_key)
        time.sleep(1)

        asic_keys = tbl.getKeys()
        assert len(asic_keys) == 0