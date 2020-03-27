import redis
import time
import os
import pytest

from swsscommon import swsscommon


@pytest.yield_fixture
def port_config(request, dvs):
    file_name = "/usr/share/sonic/hwsku/port_config.ini"
    dvs.runcmd("cp %s %s.bak" % (file_name, file_name))
    yield file_name
    dvs.runcmd("mv %s.bak %s" % (file_name, file_name))


class TestPortConfig(object):

    def getPortName(self, dvs, port_vid):
        cnt_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
        port_map = swsscommon.Table(cnt_db, 'COUNTERS_PORT_NAME_MAP')

        for k in port_map.get('')[1]:
            if k[1] == port_vid:
                return k[0]

        return ''


    def getPortOid(self, dvs, port_name):
        cnt_r = redis.Redis(unix_socket_path=dvs.redis_sock, db=swsscommon.COUNTERS_DB)
        return cnt_r.hget("COUNTERS_PORT_NAME_MAP", port_name);


    def getVIDfromRID(self, dvs, port_rid):
        asic_r = redis.Redis(unix_socket_path=dvs.redis_sock, db=swsscommon.ASIC_DB)
        return asic_r.hget("RIDTOVID", port_rid);

    def test_port_hw_lane(self, dvs):

        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        app_db_ptbl = swsscommon.Table(app_db, "PORT_TABLE")
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        asic_db_lanes_tbl = swsscommon.Table(asic_db, "LANES")

        lanes = asic_db_lanes_tbl.get('')[1]
        num_lanes = len(lanes)
        for lane in lanes:
            lane_num = lane[0];
            port_rid = lane[1];
            port_vid = self.getVIDfromRID(dvs, port_rid)
            port_name = self.getPortName(dvs, port_vid)
            (status, fvs) = app_db_ptbl.get(port_name)
            assert status == True
            for fv in fvs:
                if fv[0] == "lanes":
                    assert str(lane_num) in list(fv[1].split(","))

    def test_port_breakout(self, dvs, port_config):

        # Breakout the port from 1 to 4
        '''
        "Ethernet0": {
            "alias": "fortyGigE0/0",
            "index": "0",
            "lanes": "25,26,27,28",
            "speed": "40000"
        },

        to:
        "Ethernet0": {
            "alias": "tenGigE0",
            "index": "0",
            "lanes": "25",
            "speed": "10000"
        },

        "Ethernet1": {
            "alias": "tenGigE1",
            "index": "0",
            "lanes": "26",
            "speed": "10000"
        },

        "Ethernet2": {
            "alias": "tenGigE2",
            "index": "0",
            "lanes": "27",
            "speed": "10000"
        },

        "Ethernet3": {
            "alias": "tenGigE3",
            "index": "0",
            "lanes": "28",
            "speed": "10000"
        },
        '''
        # Get port config from configDB
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        portTbl = swsscommon.Table(conf_db, swsscommon.CFG_PORT_TABLE_NAME)

        chg_port = "Ethernet0"

        keys = portTbl.getKeys()
        assert chg_port in keys

        (status, fvs) = portTbl.get(chg_port)
        assert(status == True)

        for fv in fvs:
            if fv[0] == "index":
                new_index = fv[1]
            if fv[0] == "lanes":
                new_lanes = fv[1].split(",")

        # Stop swss before modifing the configDB
        dvs.stop_swss()
        time.sleep(1)

        # breakout the port in configDB
        portTbl._del(chg_port)

        new_ports = ["Ethernet0","Ethernet1","Ethernet2","Ethernet3"]
        new_speed = "10000"
        new_alias = ["tenGigE0", "tenGigE1", "tenGigE2", "tenGigE3"]

        for i in range (0 ,4):
            fvs = swsscommon.FieldValuePairs([("alias", new_alias[i]),
                                ("lanes", new_lanes[i]),
                                ("speed", new_speed),
                                ("index", new_index)])

            portTbl.set(new_ports[i], fvs)

        # start to apply new port_config.ini
        dvs.start_swss()
        time.sleep(5)

        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        for i in range(0,4):
            port_name = 'Ethernet{0}'.format(i)
            port_oid = self.getPortOid(dvs, port_name)
            port_tbl = swsscommon.Table(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_PORT:{0}'.format(port_oid))
            hw_lane_value = None

            for k in port_tbl.get('')[1]:
                if k[0] == "SAI_PORT_ATTR_HW_LANE_LIST":
                    hw_lane_value = k[1]

            assert hw_lane_value, "Can't get hw_lane list"
            assert hw_lane_value == "1:%s" % (new_lanes[i])


