from swsscommon import swsscommon
import time
import os

def test_PortNotification(dvs):

    dvs.restart()

    dvs.ready()

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
