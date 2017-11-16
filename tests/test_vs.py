from swsscommon import swsscommon
import os
import re

# run docker with command:
# docker run -v /var/run/redis-vs:/var/run/redis --privileged --network container:sw -d docker-sonic-vs
# run test: sudo python -c "import test_vs; test_vs.test_RouteAdd()"

DOCKER_NAME = "vs"
HOST_REDIS_SOCKET = "/var/run/redis-vs/redis.sock"

def test_RouteAdd():

    # bring up interfaces that will be in use

    assert os.system("docker exec -i " + DOCKER_NAME + " ifconfig Ethernet0 10.0.0.0/31 up") == 0
    assert os.system("docker exec -i " + DOCKER_NAME + " ifconfig Ethernet4 10.0.0.2/31 up") == 0

    assert os.system("ip netns exec sw-srv0 ifconfig eth0 10.0.0.1/31") == 0
    os.system("ip netns exec sw-srv0 ip route add default via 10.0.0.0") == 0

    assert os.system("ip netns exec sw-srv1 ifconfig eth0 10.0.0.3/31") == 0
    os.system("ip netns exec sw-srv1 ip route add default via 10.0.0.2") == 0

    # get neighbor and arp entry
    assert os.system("ip netns exec sw-srv0 ping -c 1 10.0.0.3") == 0

    db = swsscommon.DBConnector(0, HOST_REDIS_SOCKET, 0)
    ps = swsscommon.ProducerStateTable(db, "ROUTE_TABLE")
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1"), ("ifname", "Ethernet0")])

    ps.set("2.2.2.0/24", fvs)

    os.system("sleep 1")

    # check if route was propagated to ASIC DB

    db = swsscommon.DBConnector(1, HOST_REDIS_SOCKET, 0)

    tbl = swsscommon.Table(db, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")

    keys = tbl.getKeys()

    for k in keys:

        if bool(re.search('{"dest":"2.2.2.0/24"', k)):

            print "PASSED"
            return

    print "FAILED - route not found in ASIC db"
    assert 1 == 0
