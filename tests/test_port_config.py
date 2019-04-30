from swsscommon import swsscommon
import redis
import time
import os
import pytest

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
        port_tbl = swsscommon.Table(app_db, "PORT_TABLE")

        asic_r = redis.Redis(unix_socket_path=dvs.redis_sock, db=swsscommon.ASIC_DB)
        num_lanes = asic_r.hlen("LANES")
        for i in range(1, num_lanes+1):
            port_rid = asic_r.hget("LANES", i)
            port_vid = self.getVIDfromRID(dvs, port_rid)
            port_name = self.getPortName(dvs, port_vid)

            (status, fvs) = port_tbl.get(port_name)
            assert status == True
            for fv in fvs:
                if fv[0] == "lanes":
                    assert str(i) in list(fv[1].split(","))


    def test_port_breakout(self, dvs, port_config):

        # check port_config.ini
        (exitcode, output) = dvs.runcmd(['sh', '-c', "cat %s | tail -n 1" % (port_config)])
        try:
            name_str, lanes_str, alias_str, index_str, speed_str = list(output.split())
        except:
            print "parse port_config.ini fail"

        LANES_L = list(lanes_str.split(","))
        assert len(LANES_L) == 4
        assert int(speed_str) == 40000

        # modify port_config.ini
        eth = int(name_str.replace("Ethernet", ""))
        index = int(index_str)
        speed_str = "tenGigE0"
        speed = 10000
        dvs.runcmd("sed -i '$d' %s" % (port_config)) == 0
        for i in range(0,4):
            dvs.runcmd("sed -i '$a Ethernet%-7d %-17d %s/%-8d %-11d %d' %s" %
                       (eth+i, int(LANES_L[i]), speed_str, eth+i, index+i, speed, port_config)) == 0

        # delete port config
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        portTbl = swsscommon.Table(conf_db, swsscommon.CFG_PORT_TABLE_NAME)
        keys = portTbl.getKeys()
        assert  len(keys) > 0
        for key in keys:
            portTbl._del(key)

        # restart to apply new port_config.ini
        dvs.stop_swss()
        dvs.start_swss()
        time.sleep(5)

        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        for i in range(0,4):
            port_name = 'Ethernet{0}'.format(eth+i)
            port_oid = self.getPortOid(dvs, port_name)
            port_tbl = swsscommon.Table(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_PORT:{0}'.format(port_oid))

            for k in port_tbl.get('')[1]:
                if k[0] == "SAI_PORT_ATTR_HW_LANE_LIST":
                    hw_lane_value = k[1]

            assert hw_lane_value == "1:%s" % (LANES_L[i])


