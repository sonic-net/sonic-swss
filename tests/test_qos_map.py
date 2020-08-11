import pytest
import json
import sys
import time

from swsscommon import swsscommon

CFG_DOT1P_TO_TC_MAP_TABLE_NAME =  "DOT1P_TO_TC_MAP"
CFG_DOT1P_TO_TC_MAP_KEY = "AZURE"
DOT1P_TO_TC_MAP = {
    "0": "0",
    "1": "6",
    "2": "5",
    "3": "3",
    "4": "4",
    "5": "2",
    "6": "1",
    "7": "7",
}

CFG_PORT_QOS_MAP_TABLE_NAME =  "PORT_QOS_MAP"
CFG_PORT_QOS_MAP_FIELD = "dot1p_to_tc_map"
CFG_PORT_TABLE_NAME = "PORT"


class TestDot1p(object):
    def connect_dbs(self, dvs):
        self.asic_db = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.config_db = swsscommon.DBConnector(4, dvs.redis_sock, 0)


    def create_dot1p_profile(self):
        tbl = swsscommon.Table(self.config_db, CFG_DOT1P_TO_TC_MAP_TABLE_NAME)
        fvs = swsscommon.FieldValuePairs(list(DOT1P_TO_TC_MAP.items()))
        tbl.set(CFG_DOT1P_TO_TC_MAP_KEY, fvs)
        time.sleep(1)


    def find_dot1p_profile(self):
        found = False
        dot1p_tc_map_raw = None
        dot1p_tc_map_key = None
        tbl = swsscommon.Table(self.asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP")
        keys = tbl.getKeys()
        for key in keys:
            (status, fvs) = tbl.get(key)
            assert status == True

            for fv in fvs:
                if fv[0] == "SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST":
                    dot1p_tc_map_raw = fv[1]
                elif fv[0] == "SAI_QOS_MAP_ATTR_TYPE" and fv[1] == "SAI_QOS_MAP_TYPE_DOT1P_TO_TC":
                    dot1p_tc_map_key = key
                    found = True

            if found:
                break

        assert found == True

        return (key, dot1p_tc_map_raw)


    def apply_dot1p_profile_on_all_ports(self):
        tbl = swsscommon.Table(self.config_db, CFG_PORT_QOS_MAP_TABLE_NAME)
        fvs = swsscommon.FieldValuePairs([(CFG_PORT_QOS_MAP_FIELD, "[" + CFG_DOT1P_TO_TC_MAP_TABLE_NAME + "|" + CFG_DOT1P_TO_TC_MAP_KEY + "]")])
        ports = swsscommon.Table(self.config_db, CFG_PORT_TABLE_NAME).getKeys()
        for port in ports:
            tbl.set(port, fvs)

        time.sleep(1)


    def test_dot1p_cfg(self, dvs):
        self.connect_dbs(dvs)
        self.create_dot1p_profile()
        oid, dot1p_tc_map_raw = self.find_dot1p_profile()

        dot1p_tc_map = json.loads(dot1p_tc_map_raw);
        for dot1p2tc in dot1p_tc_map['list']:
            dot1p = str(dot1p2tc['key']['dot1p'])
            tc = str(dot1p2tc['value']['tc'])
            assert tc == DOT1P_TO_TC_MAP[dot1p]


    def test_port_dot1p(self, dvs):
        self.connect_dbs(dvs)
        self.create_dot1p_profile()
        oid, dot1p_tc_map_raw = self.find_dot1p_profile()

        self.apply_dot1p_profile_on_all_ports()

        cnt = 0
        tbl = swsscommon.Table(self.asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        keys = tbl.getKeys()
        for key in keys:
            (status, fvs) = tbl.get(key)
            assert status == True

            for fv in fvs:
                if fv[0] == "SAI_PORT_ATTR_QOS_DOT1P_TO_TC_MAP":
                    cnt += 1
                    assert fv[1] == oid

        port_cnt = len(swsscommon.Table(self.config_db, CFG_PORT_TABLE_NAME).getKeys())
        assert port_cnt == cnt
# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
