from swsscommon import swsscommon
import os
import re
import time
import json

def test_RouteAdd(dvs, testlog):

    dvs.runcmd("ifconfig Ethernet0 10.0.0.0/31 up")
    dvs.runcmd("ifconfig Ethernet4 10.0.0.2/31 up")

    dvs.servers[0].runcmd("ifconfig eth0 10.0.0.1/31")
    dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

    dvs.servers[1].runcmd("ifconfig eth0 10.0.0.3/31")
    dvs.servers[1].runcmd("ip route add default via 10.0.0.2")

    # get neighbor and arp entry
    dvs.servers[0].runcmd("ping -c 1 10.0.0.3")

    db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
    ps = swsscommon.ProducerStateTable(db, "ROUTE_TABLE")
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1"), ("ifname", "Ethernet0")])

    pubsub = dvs.SubscribeAsicDbObject("SAI_OBJECT_TYPE_ROUTE_ENTRY")

    ps.set("2.2.2.0/24", fvs)

    # check if route was propagated to ASIC DB

    (addobjs, delobjs) = dvs.GetSubscribedAsicDbObjects(pubsub)

    assert len(addobjs) == 1

    rt_key = json.loads(addobjs[0]['key'])

    assert rt_key['dest'] == "2.2.2.0/24"
