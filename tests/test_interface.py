from swsscommon import swsscommon
import time
import re
import json

def test_InterfaceIpChange(dvs):

    dvs.restart()

    dvs.ready()

    dvs.runcmd("ifconfig Ethernet0 10.0.0.0/31 up")

    time.sleep(1)

    # check if route was propagated to ASIC DB

    db = swsscommon.DBConnector(1, dvs.redis_sock, 0)

    tbl = swsscommon.Table(db, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")

    keys = tbl.getKeys()

    for k in keys:
        rt_key = json.loads(k)

        if rt_key['dest'] == "10.0.0.0/31":
            subnet_found = True

        if rt_key['dest'] == "10.0.0.0/32":
            ip2me_found = True

    assert subnet_found == True and ip2me_found == True

    subnet_found = False
    ip2me_found = False

    dvs.runcmd("ifconfig Ethernet0 10.0.0.0/24 up")

    time.sleep(1)

    # check if route was propagated to ASIC DB

    keys = tbl.getKeys()

    for k in keys:
        rt_key = json.loads(k)

        if rt_key['dest'] == "10.0.0.0/24":
            subnet_found = True

        if rt_key['dest'] == "10.0.0.0/32":
            ip2me_found = True

    assert subnet_found == True and ip2me_found == True
