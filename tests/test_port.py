import time
import os
import pytest

from swsscommon import swsscommon


class TestPort(object):
    def test_PortTpid(self, dvs, testlog):
        pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

        # set TPID to port
        cdb_port_tbl = swsscommon.Table(cdb, "PORT")
        fvs = swsscommon.FieldValuePairs([("tpid", "0x9200")])
        cdb_port_tbl.set("Ethernet8", fvs)
        time.sleep(1)

        # check application database
        pdb_port_tbl = swsscommon.Table(pdb, "PORT_TABLE")
        (status, fvs) = pdb_port_tbl.get("Ethernet8")
        assert status == True
        for fv in fvs:
            if fv[0] == "tpid":
                tpid = fv[1]
        assert tpid == "0x9200"

        # Check ASIC DB
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        # get TPID and validate it to be 0x9200 (37376)
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet8"])
        assert status == True
        asic_tpid = "0"

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_TPID":
                asic_tpid = fv[1]

        assert asic_tpid == "37376"

    def test_PortMtu(self, dvs, testlog):
        pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

        # set MTU to port
        tbl = swsscommon.Table(cdb, "PORT")
        fvs = swsscommon.FieldValuePairs([("MTU", "9100")])
        tbl.set("Ethernet8", fvs)
        time.sleep(1)

        # check application database
        tbl = swsscommon.Table(pdb, "PORT_TABLE")
        (status, fvs) = tbl.get("Ethernet8")
        assert status == True
        for fv in fvs:
            if fv[0] == "mtu":
                assert fv[1] == "9100"

    def test_PortNotification(self, dvs, testlog):
        dvs.port_admin_set("Ethernet0", "up")
        dvs.interface_ip_add("Ethernet0", "10.0.0.0/31")

        dvs.port_admin_set("Ethernet4", "up")
        dvs.interface_ip_add("Ethernet4", "10.0.0.2/31")

        dvs.servers[0].runcmd("ip link set down dev eth0") == 0

        time.sleep(1)

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)

        tbl = swsscommon.Table(db, "PORT_TABLE")

        (status, fvs) = tbl.get("Ethernet0")

        assert status == True

        oper_status = "unknown"

        for v in fvs:
            if v[0] == "oper_status":
                oper_status = v[1]
                break

        assert oper_status == "down"

        dvs.servers[0].runcmd("ip link set up dev eth0") == 0

        time.sleep(1)

        (status, fvs) = tbl.get("Ethernet0")

        assert status == True

        oper_status = "unknown"

        for v in fvs:
            if v[0] == "oper_status":
                oper_status = v[1]
                break

        assert oper_status == "up"

    def test_PortFecForce(self, dvs, testlog):
        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = dvs.get_asic_db()

        ptbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")

        # set fec
        fvs = swsscommon.FieldValuePairs([("fec","none")])
        ptbl.set("Ethernet0", fvs)
        fvs = swsscommon.FieldValuePairs([("fec","rs")])
        ptbl.set("Ethernet4", fvs)

        # validate if fec none is pushed to asic db when set first time
        port_oid = adb.port_name_map["Ethernet0"]
        expected_fields = {"SAI_PORT_ATTR_FEC_MODE":"SAI_PORT_FEC_MODE_NONE"}
        adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid, expected_fields)

        # validate if fec rs is pushed to asic db when set first time
        port_oid = adb.port_name_map["Ethernet4"]
        expected_fields = {"SAI_PORT_ATTR_FEC_MODE":"SAI_PORT_FEC_MODE_RS"}
        adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid, expected_fields)

    def test_PortFec(self, dvs, testlog):
        dvs.port_admin_set("Ethernet0", "up")
        dvs.interface_ip_add("Ethernet0", "10.0.0.0/31")

        dvs.port_admin_set("Ethernet4", "up")
        dvs.interface_ip_add("Ethernet4", "10.0.0.2/31")

        dvs.servers[0].runcmd("ip link set down dev eth0") == 0

        time.sleep(1)

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        tbl = swsscommon.Table(db, "PORT_TABLE")
        ptbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")

        (status, fvs) = tbl.get("Ethernet0")

        assert status == True

        oper_status = "unknown"

        for v in fvs:
            if v[0] == "oper_status":
                oper_status = v[1]
                break

        assert oper_status == "down"

        dvs.servers[0].runcmd("ip link set up dev eth0") == 0

        time.sleep(1)

        (status, fvs) = tbl.get("Ethernet0")

        assert status == True

        oper_status = "unknown"

        for v in fvs:
            if v[0] == "oper_status":
                oper_status = v[1]
                break

        assert oper_status == "up"

        # set fec
        fvs = swsscommon.FieldValuePairs([("fec","rs"), ("speed", "1000")])
        ptbl.set("Ethernet0", fvs)

        time.sleep(1)

        # get fec
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_FEC_MODE":
                assert fv[1] == "SAI_PORT_FEC_MODE_RS"

    def test_PortPreemp(self, dvs, testlog):

        pre_name = 'preemphasis'
        pre_val = [0x1234,0x2345,0x3456,0x4567]
        pre_val_str = str(hex(pre_val[0])) + "," + str(hex(pre_val[1]))+ "," + \
                      str(hex(pre_val[2]))+ "," + str(hex(pre_val[3]))

        pre_val_asic = '4:' + str(pre_val[0]) + "," + str(pre_val[1]) + "," + \
                       str(pre_val[2]) + "," + str(pre_val[3])
        fvs = swsscommon.FieldValuePairs([(pre_name, pre_val_str)])
        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        tbl = swsscommon.Table(db, "PORT_TABLE")
        ptbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")

        ptbl.set("Ethernet0", fvs)


        time.sleep(1)

        # get fec
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_SERDES_PREEMPHASIS":
                assert fv[1] == pre_val_asic

    def test_PortIdriver(self, dvs, testlog):

        idrv_name = 'idriver'
        idrv_val = [0x1,0x1,0x2,0x2]
        idrv_val_str = str(hex(idrv_val[0])) + "," + str(hex(idrv_val[1]))+ "," + \
                       str(hex(idrv_val[2]))+ "," + str(hex(idrv_val[3]))

        idrv_val_asic = '4:' + str(idrv_val[0]) + "," + str(idrv_val[1]) + "," + \
                       str(idrv_val[2]) + "," + str(idrv_val[3])
        fvs = swsscommon.FieldValuePairs([(idrv_name, idrv_val_str)])
        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        tbl = swsscommon.Table(db, "PORT_TABLE")
        ptbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")

        ptbl.set("Ethernet0", fvs)


        time.sleep(1)

        # get fec
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_SERDES_IDRIVER":
                assert fv[1] == idrv_val_asic

    def test_PortIpredriver(self, dvs, testlog):

        ipre_name = 'ipredriver'
        ipre_val = [0x2,0x3,0x4,0x5]
        ipre_val_str = str(hex(ipre_val[0])) + "," + str(hex(ipre_val[1]))+ "," + \
                       str(hex(ipre_val[2]))+ "," + str(hex(ipre_val[3]))

        ipre_val_asic = '4:' + str(ipre_val[0]) + "," + str(ipre_val[1]) + "," + \
                       str(ipre_val[2]) + "," + str(ipre_val[3])
        fvs = swsscommon.FieldValuePairs([(ipre_name, ipre_val_str)])
        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        tbl = swsscommon.Table(db, "PORT_TABLE")
        ptbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")

        ptbl.set("Ethernet0", fvs)


        time.sleep(1)

        # get fec
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_SERDES_IPREDRIVER":
                assert fv[1] == ipre_val_asic


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
