from swsscommon import swsscommon

import time
import os

class TestPort(object):
    def test_PortMtu(self, dvs):
        pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sec_sock, 0)
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

def test_PortNotification(dvs):

    dvs.runcmd("ifconfig Ethernet0 10.0.0.0/31 up") == 0
    dvs.runcmd("ifconfig Ethernet4 10.0.0.2/31 up") == 0

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

def test_PortFec(dvs):

    dvs.runcmd("ifconfig Ethernet0 10.0.0.0/31 up") == 0
    dvs.runcmd("ifconfig Ethernet4 10.0.0.2/31 up") == 0

    dvs.servers[0].runcmd("ip link set down dev eth0") == 0

    time.sleep(1)

    db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
    adb = swsscommon.DBConnector(1, dvs.redis_sec_sock, 0)

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
