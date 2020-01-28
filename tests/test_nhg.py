import os
import re
import time
import json
import pytest

from swsscommon import swsscommon
from flaky import flaky


@pytest.mark.flaky
class TestNextHopGroup(object):
    def test_route_nhg(self, dvs, testlog):
        config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        intf_tbl = swsscommon.Table(config_db, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL","NULL")])
        intf_tbl.set("Ethernet0", fvs)
        intf_tbl.set("Ethernet4", fvs)
        intf_tbl.set("Ethernet8", fvs)
        intf_tbl.set("Ethernet0|10.0.0.0/31", fvs)
        intf_tbl.set("Ethernet4|10.0.0.2/31", fvs)
        intf_tbl.set("Ethernet8|10.0.0.4/31", fvs)
        dvs.runcmd("ifconfig Ethernet0 up")
        dvs.runcmd("ifconfig Ethernet4 up")
        dvs.runcmd("ifconfig Ethernet8 up")

        dvs.runcmd("arp -s 10.0.0.1 00:00:00:00:00:01")
        dvs.runcmd("arp -s 10.0.0.3 00:00:00:00:00:02")
        dvs.runcmd("arp -s 10.0.0.5 00:00:00:00:00:03")

        dvs.servers[0].runcmd("ip link set down dev eth0") == 0
        dvs.servers[1].runcmd("ip link set down dev eth0") == 0
        dvs.servers[2].runcmd("ip link set down dev eth0") == 0

        dvs.servers[0].runcmd("ip link set up dev eth0") == 0
        dvs.servers[1].runcmd("ip link set up dev eth0") == 0
        dvs.servers[2].runcmd("ip link set up dev eth0") == 0

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        ps = swsscommon.ProducerStateTable(db, "ROUTE_TABLE")
        fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1,10.0.0.3,10.0.0.5"), ("ifname", "Ethernet0,Ethernet4,Ethernet8")])

        ps.set("2.2.2.0/24", fvs)

        time.sleep(1)

        # check if route was propagated to ASIC DB

        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        rtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        nhgtbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")
        nhg_member_tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")

        keys = rtbl.getKeys()

        found_route = False
        for k in keys:
            rt_key = json.loads(k)

            if rt_key['dest'] == "2.2.2.0/24":
                found_route = True
                break

        assert found_route

        # assert the route points to next hop group
        (status, fvs) = rtbl.get(k)

        for v in fvs:
            if v[0] == "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID":
                nhgid = v[1]

        (status, fvs) = nhgtbl.get(nhgid)

        assert status

        keys = nhg_member_tbl.getKeys()

        assert len(keys) == 3

        for k in keys:
            (status, fvs) = nhg_member_tbl.get(k)

            for v in fvs:
                if v[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID":
                    assert v[1] == nhgid

        # bring links down one-by-one
        for i in [0, 1, 2]:
            dvs.servers[i].runcmd("ip link set down dev eth0") == 0

            time.sleep(1)

            tbl = swsscommon.Table(db, "PORT_TABLE")
            (status, fvs) = tbl.get("Ethernet%d" % (i * 4))

            assert status == True

            oper_status = "unknown"

            for v in fvs:
                if v[0] == "oper_status":
                    oper_status = v[1]
                    break

            assert oper_status == "down"

            keys = nhg_member_tbl.getKeys()

            assert len(keys) == 2 - i

        # bring links up one-by-one
        for i in [0, 1, 2]:
            dvs.servers[i].runcmd("ip link set up dev eth0") == 0

            time.sleep(1)

            tbl = swsscommon.Table(db, "PORT_TABLE")
            (status, fvs) = tbl.get("Ethernet%d" % (i * 4))

            assert status == True

            oper_status = "unknown"

            for v in fvs:
                if v[0] == "oper_status":
                    oper_status = v[1]
                    break

            assert oper_status == "up"

            keys = nhg_member_tbl.getKeys()

            assert len(keys) == i + 1

            for k in keys:
                (status, fvs) = nhg_member_tbl.get(k)

                for v in fvs:
                    if v[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID":
                        assert v[1] == nhgid
