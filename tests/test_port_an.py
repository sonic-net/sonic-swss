import time
import os
import pytest

from swsscommon import swsscommon
from flaky import flaky


@pytest.mark.flaky
class TestPortAutoNeg(object):
    def test_PortAutoNegCold(self, dvs, testlog):

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)

        tbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")

        # set autoneg = false and speed = 1000
        fvs = swsscommon.FieldValuePairs([("autoneg","1"), ("speed", "1000")])

        tbl.set("Ethernet0", fvs)

        time.sleep(1)

        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        assert "SAI_PORT_ATTR_AUTO_NEG_MODE" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_SPEED" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
                assert fv[1] == "1:1000"

        # set speed = 100
        fvs = swsscommon.FieldValuePairs([("speed", "100")])

        tbl.set("Ethernet0", fvs)

        time.sleep(1)

        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
                assert fv[1] == "1:100"

        # change autoneg to false
        fvs = swsscommon.FieldValuePairs([("autoneg","0")])

        tbl.set("Ethernet0", fvs)

        time.sleep(1)

        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        assert "SAI_PORT_ATTR_AUTO_NEG_MODE" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_SPEED" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_SPEED" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                assert fv[1] == "false"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
                assert fv[1] == "1:100"
            elif fv[0] == "SAI_PORT_ATTR_SPEED":
                assert fv[1] == "100"

        # set speed = 1000
        fvs = swsscommon.FieldValuePairs([("speed", "1000")])

        tbl.set("Ethernet0", fvs)

        time.sleep(1)

        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                assert fv[1] == "false"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
                assert fv[1] == "1:100"
            elif fv[0] == "SAI_PORT_ATTR_SPEED":
                assert fv[1] == "1000"

    def test_PortAutoNegWarm(self, dvs, testlog):

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        sdb = swsscommon.DBConnector(6, dvs.redis_sock, 0)

        tbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        ctbl = swsscommon.Table(cdb, "PORT")
        stbl = swsscommon.Table(sdb, "PORT_TABLE")

        # set autoneg = true and speed = 1000
        fvs = swsscommon.FieldValuePairs([("autoneg","1"), ("speed", "1000")])
        ctbl.set("Ethernet0", fvs)

        time.sleep(1)

        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        assert "SAI_PORT_ATTR_AUTO_NEG_MODE" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_SPEED" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
                assert fv[1] == "1:1000"

        # set speed = 100
        fvs = swsscommon.FieldValuePairs([("speed", "100")])

        ctbl.set("Ethernet0", fvs)

        time.sleep(1)

        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
                assert fv[1] == "1:100"

        # set admin up
        cfvs = swsscommon.FieldValuePairs([("admin_status", "up")])
        ctbl.set("Ethernet0", cfvs)

        # enable warm restart
        (exitcode, result) = dvs.runcmd("config warm_restart enable swss")
        assert exitcode == 0

        # freeze orchagent for warm restart
        (exitcode, result) = dvs.runcmd("/usr/bin/orchagent_restart_check")
        assert result == "RESTARTCHECK succeeded\n"
        time.sleep(2)

        try:
            # restart orchagent
            # clean port state
            dvs.stop_swss()
            ports = stbl.getKeys()
            for port in ports:
                stbl._del(port)
            dvs.start_swss()
            time.sleep(2)

            # check ASIC DB after warm restart
            (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
            assert status == True

            assert "SAI_PORT_ATTR_AUTO_NEG_MODE" in [fv[0] for fv in fvs]
            assert "SAI_PORT_ATTR_ADVERTISED_SPEED" in [fv[0] for fv in fvs]
            for fv in fvs:
                if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                    assert fv[1] == "true"
                elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
                    assert fv[1] == "1:100"

        finally:
            # disable warm restart
            dvs.runcmd("config warm_restart disable swss")
            # slow down crm polling
            dvs.runcmd("crm config polling interval 10000")
