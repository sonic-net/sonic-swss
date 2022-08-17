import time
import os
import pytest

from swsscommon import swsscommon


class TestPortLinkTraining(object):
    def test_PortLinkTrainingForce(self, dvs, testlog):

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = dvs.get_asic_db()

        tbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        fvs = swsscommon.FieldValuePairs([("link_training","off")])
        tbl.set("Ethernet0", fvs)

        tbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        fvs = swsscommon.FieldValuePairs([("link_training","on")])
        tbl.set("Ethernet4", fvs)

        # validate if link_training false is pushed to asic db when set first time
        port_oid = adb.port_name_map["Ethernet0"]
        expected_fields = {"SAI_PORT_ATTR_LINK_TRAINING_ENABLE":"false"}
        adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid, expected_fields)

        # validate if link_training true is pushed to asic db when set first time
        port_oid = adb.port_name_map["Ethernet4"]
        expected_fields = {"SAI_PORT_ATTR_LINK_TRAINING_ENABLE":"true"}
        adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid, expected_fields)

    def test_PortLinkTrainingCold(self, dvs, testlog):
        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)

        tbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")

        # set link_training = true
        fvs = swsscommon.FieldValuePairs([("link_training","on")])

        tbl.set("Ethernet0", fvs)

        time.sleep(1)

        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        assert "SAI_PORT_ATTR_LINK_TRAINING_ENABLE" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_LINK_TRAINING_ENABLE":
                assert fv[1] == "true"

        # change link_training to false
        fvs = swsscommon.FieldValuePairs([("link_training","off")])

        tbl.set("Ethernet0", fvs)

        time.sleep(1)

        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        assert "SAI_PORT_ATTR_LINK_TRAINING_ENABLE" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_LINK_TRAINING_ENABLE":
                assert fv[1] == "false"

    def test_PortLinkTrainingWarm(self, dvs, testlog):

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        sdb = swsscommon.DBConnector(6, dvs.redis_sock, 0)

        tbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        ctbl = swsscommon.Table(cdb, "PORT")
        stbl = swsscommon.Table(sdb, "PORT_TABLE")

        # set link_training = true
        fvs = swsscommon.FieldValuePairs([("link_training","on")])
        ctbl.set("Ethernet0", fvs)

        time.sleep(1)

        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        assert "SAI_PORT_ATTR_LINK_TRAINING_ENABLE" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
                assert fv[1] == "true"

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

            assert "SAI_PORT_ATTR_LINK_TRAINING_ENABLE" in [fv[0] for fv in fvs]
            for fv in fvs:
                if fv[0] == "SAI_PORT_ATTR_LINK_TRAINING_ENABLE":
                    assert fv[1] == "true"

        finally:
            # disable warm restart
            dvs.runcmd("config warm_restart disable swss")
            # slow down crm polling
            dvs.runcmd("crm config polling interval 10000")


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
