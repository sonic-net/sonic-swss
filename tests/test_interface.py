from swsscommon import swsscommon

import time
import json

class TestInterfaceIpv4Addresses(object):
    def test_InterfaceAddIpv4Address(self, dvs):
        pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

        # assign IP to interface
        tbl = swsscommon.Table(cdb, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        tbl.set("Ethernet8|10.0.0.4/31", fvs)
        time.sleep(1)

        # check application database
        tbl = swsscommon.Table(pdb, "INTF_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        assert intf_entries[0] == "10.0.0.4/31"

        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        assert len(fvs) == 2
        for fv in fvs:
            if fv[0] == "scope":
                assert fv[1] == "global"
            elif fv[0] == "family":
                assert fv[1] == "IPv4"
            else:
                assert False

        # check ASIC router interface database
        tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # one loopback router interface one port based router interface
        assert len(intf_entries) == 2

        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            # a port based router interface has five field/value tuples
            if len(fvs) == 5:
                for fv in fvs:
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_TYPE":
                        assert fv[1] == "SAI_ROUTER_INTERFACE_TYPE_PORT"
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_MTU":
                        assert fv[1] == "1500"

        # check ASIC route database
        tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.0.0.4/31":
                subnet_found = True
            if route["dest"] == "10.0.0.4/32":
                ip2me_found = True

        assert subnet_found and ip2me_found

    def test_InterfaceChangeMtu(self, dvs):
        pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

        tbl = swsscommon.ProducerStateTable(pdb, "PORT_TABLE")
        fvs = swsscommon.FieldValuePairs([("mtu", "8888")])
        tbl.set("Ethernet8", fvs)

        time.sleep(1)

        # check ASIC router interface database
        tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # one loopback router interface one port based router interface
        assert len(intf_entries) == 2

        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            # a port based router interface has five field/value tuples
            if len(fvs) == 5:
                for fv in fvs:
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_TYPE":
                        assert fv[1] == "SAI_ROUTER_INTERFACE_TYPE_PORT"
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_MTU":
                        assert fv[1] == "8888"

    def test_InterfaceRemoveIpv4Address(self, dvs):
        pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

        # remove IP from interface
        tbl = swsscommon.Table(cdb, "INTERFACE")
        tbl._del("Ethernet8|10.0.0.4/31")
        time.sleep(1)

        # check application database
        tbl = swsscommon.Table(pdb, "INTF_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # check ASIC database
        tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.0.0.4/31":
                assert False
            if route["dest"] == "10.0.0.4/32":
                assert False


def test_InterfaceIpChange(dvs):

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
