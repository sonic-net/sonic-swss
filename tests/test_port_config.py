import redis
import time
import os
import pytest

from swsscommon import swsscommon
from dvslib.dvs_common import wait_for_result, PollingConfig


@pytest.fixture
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
        cnt_r = redis.Redis(unix_socket_path=dvs.redis_sock, db=swsscommon.COUNTERS_DB,
                            encoding="utf-8", decode_responses=True)
        return cnt_r.hget("COUNTERS_PORT_NAME_MAP", port_name);


    def getVIDfromRID(self, dvs, port_rid):
        asic_r = redis.Redis(unix_socket_path=dvs.redis_sock, db=swsscommon.ASIC_DB,
                             encoding="utf-8", decode_responses=True)
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

    @pytest.mark.skip(reason="Failing. Under investigation")
    def test_recirc_port(self, dvs):

        # Get port config from configDB
        cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        cfg_port_tbl = swsscommon.Table(cfg_db, swsscommon.CFG_PORT_TABLE_NAME)

        indexes = []
        lanes = []
        keys = cfg_port_tbl.getKeys()
        for port in keys:
            (status, fvs) = cfg_port_tbl.get(port)
            assert(status == True)

            for fv in fvs:
                if fv[0] == "index":
                    indexes.append(int(fv[1]))
                if fv[0] == "lanes":
                    lanes.extend([int(lane) for lane in fv[1].split(",")])

        # Stop swss before modifing the configDB
        dvs.stop_swss()
        time.sleep(1)

        recirc_port_lane_base = max(lanes) + 1
        recirc_port_index_base = max(indexes) + 1

        # Add recirc ports to port config in configDB
        recirc_port_lane_name_map = {}
        for i in range(2):
            name = alias = "Ethernet-Rec%s" % i
            fvs = swsscommon.FieldValuePairs([("role", "Rec" if i % 2 == 0 else "Inb"),
                                              ("alias", alias),
                                              ("lanes", str(recirc_port_lane_base + i)),
                                              ("speed", "10000"),
                                              ("index", str(recirc_port_index_base + i))])
            cfg_port_tbl.set(name, fvs)

        # Start swss
        dvs.start_swss()
        time.sleep(5)

        polling_config = PollingConfig(polling_interval=0.1, timeout=15, strict=True)

        # Verify recirc ports in port table in applDB
        for i in range(2):
            name = alias = "Ethernet-Rec%s" % i
            dvs.get_app_db().wait_for_field_match(swsscommon.APP_PORT_TABLE_NAME, name,
                                                  {"role" : "Rec" if i % 2 == 0 else "Inb",
                                                   "alias" : name,
                                                   "lanes" : str(recirc_port_lane_base + i),
                                                   "speed" : "10000",
                                                   "index" : str(recirc_port_index_base + i) },
                                                  polling_config=polling_config)

        # Verify recirc port lanes in asicDB
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        asic_db_lanes_tbl = swsscommon.Table(asic_db, "LANES")

        def _access_function():
            lanes = asic_db_lanes_tbl.get('')[1]
            if len(lanes) == 0:
                return (False, None)

            recirc_port_lanes = [recirc_port_lane_base, recirc_port_lane_base + 1]
            for lane in lanes:
                lane_num = int(lane[0])
                if int(lane_num) in recirc_port_lanes:
                    recirc_port_lanes.remove( lane_num )
            return (not recirc_port_lanes, None)
        wait_for_result(_access_function, polling_config=polling_config)



# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
