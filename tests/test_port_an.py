import time
import os
import pytest

from swsscommon import swsscommon


class TestPortAutoNeg(object):
    def test_PortAutoNegForce(self, dvs, testlog):

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = dvs.get_asic_db()

        tbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        fvs = swsscommon.FieldValuePairs([("autoneg","off")])
        tbl.set("Ethernet0", fvs)

        tbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        fvs = swsscommon.FieldValuePairs([("autoneg","on"), ("speed", "1000")])
        tbl.set("Ethernet4", fvs)

        # validate if autoneg false is pushed to asic db when set first time
        port_oid = adb.port_name_map["Ethernet0"]
        expected_fields = {"SAI_PORT_ATTR_AUTO_NEG_MODE":"false"}
        adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid, expected_fields)

        # validate if autoneg true is pushed to asic db when set first time
        port_oid = adb.port_name_map["Ethernet4"]
        expected_fields = {"SAI_PORT_ATTR_AUTO_NEG_MODE":"true"}
        adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid, expected_fields)

    def test_PortAutoNegCold(self, dvs, testlog):
        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)

        tbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")

        # set autoneg = true and adv_speeds = 1000
        fvs = swsscommon.FieldValuePairs([("autoneg","on"), ("adv_speeds", "1000")])

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

        # set adv_speeds = 100,1000
        fvs = swsscommon.FieldValuePairs([("adv_speeds", "100,1000")])

        tbl.set("Ethernet0", fvs)

        time.sleep(1)
        
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        assert "SAI_PORT_ATTR_AUTO_NEG_MODE" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_SPEED" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
                assert fv[1] == "2:100,1000"

        # set adv_interface_types = CR2
        fvs = swsscommon.FieldValuePairs([("adv_interface_types", "CR2")])

        tbl.set("Ethernet0", fvs)

        time.sleep(1)

        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        assert "SAI_PORT_ATTR_AUTO_NEG_MODE" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_SPEED" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
                assert fv[1] == "2:100,1000"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE":
                assert fv[1] == "1:SAI_PORT_INTERFACE_TYPE_CR2"

        # set adv_interface_types = CR2,CR4
        fvs = swsscommon.FieldValuePairs([("adv_interface_types", "CR2,CR4")])

        tbl.set("Ethernet0", fvs)

        time.sleep(1)

        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        assert "SAI_PORT_ATTR_AUTO_NEG_MODE" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_SPEED" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
                assert fv[1] == "2:100,1000"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE":
                assert fv[1] == "2:SAI_PORT_INTERFACE_TYPE_CR2,SAI_PORT_INTERFACE_TYPE_CR4"

        # change autoneg to false
        fvs = swsscommon.FieldValuePairs([("autoneg","off"), ("speed", "100")])

        tbl.set("Ethernet0", fvs)

        time.sleep(1)

        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        assert "SAI_PORT_ATTR_AUTO_NEG_MODE" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_SPEED" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_SPEED" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                assert fv[1] == "false"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
                assert fv[1] == "2:100,1000"
            elif fv[0] == "SAI_PORT_ATTR_SPEED":
                assert fv[1] == "100"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE":
                assert fv[1] == "2:SAI_PORT_INTERFACE_TYPE_CR2,SAI_PORT_INTERFACE_TYPE_CR4"

        # set speed = 1000
        fvs = swsscommon.FieldValuePairs([("speed", "1000")])

        tbl.set("Ethernet0", fvs)

        time.sleep(1)

        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        assert "SAI_PORT_ATTR_AUTO_NEG_MODE" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_SPEED" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_SPEED" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                assert fv[1] == "false"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
                assert fv[1] == "2:100,1000"
            elif fv[0] == "SAI_PORT_ATTR_SPEED":
                assert fv[1] == "1000"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE":
                assert fv[1] == "2:SAI_PORT_INTERFACE_TYPE_CR2,SAI_PORT_INTERFACE_TYPE_CR4"

        # set interface_type = CR4
        fvs = swsscommon.FieldValuePairs([("interface_type", "CR4")])
        tbl.set("Ethernet0", fvs)

        time.sleep(1)

        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        assert "SAI_PORT_ATTR_AUTO_NEG_MODE" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_SPEED" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_SPEED" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_INTERFACE_TYPE" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                assert fv[1] == "false"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
                assert fv[1] == "2:100,1000"
            elif fv[0] == "SAI_PORT_ATTR_SPEED":
                assert fv[1] == "1000"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE":
                assert fv[1] == "2:SAI_PORT_INTERFACE_TYPE_CR2,SAI_PORT_INTERFACE_TYPE_CR4"
            elif fv[0] == "SAI_PORT_ATTR_INTERFACE_TYPE":
                assert fv[1] == "SAI_PORT_INTERFACE_TYPE_CR4"

        # set interface_type = CR2
        fvs = swsscommon.FieldValuePairs([("interface_type", "CR2")])
        tbl.set("Ethernet0", fvs)

        time.sleep(1)

        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        assert "SAI_PORT_ATTR_AUTO_NEG_MODE" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_SPEED" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_SPEED" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_INTERFACE_TYPE" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                assert fv[1] == "false"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
                assert fv[1] == "2:100,1000"
            elif fv[0] == "SAI_PORT_ATTR_SPEED":
                assert fv[1] == "1000"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE":
                assert fv[1] == "2:SAI_PORT_INTERFACE_TYPE_CR2,SAI_PORT_INTERFACE_TYPE_CR4"
            elif fv[0] == "SAI_PORT_ATTR_INTERFACE_TYPE":
                assert fv[1] == "SAI_PORT_INTERFACE_TYPE_CR2"

    def test_PortAutoNegWarm(self, dvs, testlog):

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        sdb = swsscommon.DBConnector(6, dvs.redis_sock, 0)

        tbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        ctbl = swsscommon.Table(cdb, "PORT")
        stbl = swsscommon.Table(sdb, "PORT_TABLE")

        # set autoneg = true and speed = 1000
        fvs = swsscommon.FieldValuePairs([("autoneg","on"), 
                                          ("adv_speeds", "100,1000"),
                                          ("adv_interface_types", "CR2,CR4")])
        ctbl.set("Ethernet0", fvs)

        time.sleep(1)

        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        assert "SAI_PORT_ATTR_AUTO_NEG_MODE" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_SPEED" in [fv[0] for fv in fvs]
        assert "SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                assert fv[1] == "true"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
                assert fv[1] == "2:100,1000"
            elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE":
                assert fv[1] == "2:SAI_PORT_INTERFACE_TYPE_CR2,SAI_PORT_INTERFACE_TYPE_CR4"

        # set admin up
        cfvs = swsscommon.FieldValuePairs([("admin_status", "up")])
        ctbl.set("Ethernet0", cfvs)


        dvs.warm_restart_swss("true")

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
            assert "SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE" in [fv[0] for fv in fvs]
            for fv in fvs:
                if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                    assert fv[1] == "true"
                elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
                    assert fv[1] == "2:100,1000"
                elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_INTERFACE_TYPE":
                    assert fv[1] == "2:SAI_PORT_INTERFACE_TYPE_CR2,SAI_PORT_INTERFACE_TYPE_CR4"

        finally:
            # disable warm restart
            dvs.warm_restart_swss("disable")
            # slow down crm polling
            dvs.crm_poll_set("10000")


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
