from swsscommon import swsscommon

import time
import json

class TestRouterInterface(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

    def set_admin_status(self, interface, status):
        tbl = swsscommon.Table(self.cdb, "PORT")
        fvs = swsscommon.FieldValuePairs([("admin_status", status)])
        tbl.set(interface, fvs)
        time.sleep(1)

    def add_ip_address(self, interface, ip):
        tbl = swsscommon.Table(self.cdb, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        tbl.set(interface + "|" + ip, fvs)
        time.sleep(2) # IPv6 netlink message needs longer time

    def remove_ip_address(self, interface, ip):
        tbl = swsscommon.Table(self.cdb, "INTERFACE")
        tbl._del(interface + "|" + ip);
        time.sleep(1)

    def set_mtu(self, interface, mtu):
        tbl = swsscommon.Table(self.cdb, "PORT")
        fvs = swsscommon.FieldValuePairs([("mtu", mtu)])
        tbl.set(interface, fvs)
        time.sleep(1)

    def test_InterfaceAddRemoveIpv6Address(self, dvs, testlog):
        self.setup_db(dvs)

        # bring up interface
        # NOTE: For IPv6, only when the interface is up will the netlink message
        # get generated.
        self.set_admin_status("Ethernet8", "up")

        # assign IP to interface
        self.add_ip_address("Ethernet8", "fc00::1/126")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        assert intf_entries[0] == "fc00::1/126"

        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        assert len(fvs) == 2
        for fv in fvs:
            if fv[0] == "scope":
                assert fv[1] == "global"
            elif fv[0] == "family":
                assert fv[1] == "IPv6"
            else:
                assert False

        # check ASIC router interface database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
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
                    # the default MTU without any configuration is 1492
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_MTU":
                        assert fv[1] == "1492"

        # check ASIC route database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "fc00::/126":
                subnet_found = True
            if route["dest"] == "fc00::1/128":
                ip2me_found = True

        assert subnet_found and ip2me_found

        # remove IP from interface
        self.remove_ip_address("Ethernet8", "fc00::1/126")

        # bring down interface
        self.set_admin_status("Ethernet8", "down")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "fc00::/126":
                assert False
            if route["dest"] == "fc00::1/128":
                assert False

    def test_InterfaceAddRemoveIpv4Address(self, dvs, testlog):
        self.setup_db(dvs)

        # assign IP to interface
        self.add_ip_address("Ethernet8", "10.0.0.4/31")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Ethernet8")
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
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
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
                    # the default MTU without any configuration is 1492
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_MTU":
                        assert fv[1] == "1492"

        # check ASIC route database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.0.0.4/31":
                subnet_found = True
            if route["dest"] == "10.0.0.4/32":
                ip2me_found = True

        assert subnet_found and ip2me_found

        # remove IP from interface
        self.remove_ip_address("Ethernet8", "10.0.0.4/31")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.0.0.4/31":
                assert False
            if route["dest"] == "10.0.0.4/32":
                assert False

    def test_InterfaceSetMtu(self, dvs, testlog):
        self.setup_db(dvs)

        # assign IP to interface
        self.add_ip_address("Ethernet16", "20.0.0.8/29")

        # configure MTU to interface
        self.set_mtu("Ethernet16", "8888")

        # check ASIC router interface database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
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
                    # assert the new value set to the router interface
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_MTU":
                        assert fv[1] == "8888"

        # remove IP from interface
        self.remove_ip_address("Ethernet16", "20.0.0.8/29")

class TestLagRouterInterfaceIpv4(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

    def create_port_channel(self, dvs, alias):
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL")
        fvs = swsscommon.FieldValuePairs([("admin_status", "up"),
                                          ("mtu", "9100")])
        tbl.set(alias, fvs)
        time.sleep(1)

    def remove_port_channel(self, dvs, alias):
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL")
        tbl._del(alias)
        time.sleep(1)

    def add_port_channel_members(self, dvs, lag, members):
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL_MEMBER")
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        for member in members:
            tbl.set(lag + "|" + member, fvs)
            time.sleep(1)

    def remove_port_channel_members(self, dvs, lag, members):
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL_MEMBER")
        for member in members:
            tbl._del(lag + "|" + member)
            time.sleep(1)

    def add_ip_address(self, interface, ip):
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL_INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        tbl.set(interface + "|" + ip, fvs)
        time.sleep(1)

    def remove_ip_address(self, interface, ip):
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL_INTERFACE")
        tbl._del(interface + "|" + ip);
        time.sleep(1)

    def set_mtu(self, interface, mtu):
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL")
        fvs = swsscommon.FieldValuePairs([("mtu", mtu)])
        tbl.set(interface, fvs)
        time.sleep(1)

    def test_InterfaceAddRemoveIpv4Address(self, dvs, testlog):
        self.setup_db(dvs)

        # create port channel
        self.create_port_channel(dvs, "PortChannel001")

        # assign IP to interface
        self.add_ip_address("PortChannel001", "30.0.0.4/31")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:PortChannel001")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        assert intf_entries[0] == "30.0.0.4/31"

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
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
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
                    # the default MTU without any configuration is 1492
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_MTU":
                        assert fv[1] == "1492"

        # check ASIC route database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "30.0.0.4/31":
                subnet_found = True
            if route["dest"] == "30.0.0.4/32":
                ip2me_found = True

        assert subnet_found and ip2me_found

        # remove IP from interface
        self.remove_ip_address("PortChannel001", "30.0.0.4/31")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:PortChannel001")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "30.0.0.4/31":
                assert False
            if route["dest"] == "30.0.0.4/32":
                assert False

        # remove port channel
        self.remove_port_channel(dvs, "PortChannel001")


    def test_InterfaceSetMtu(self, dvs, testlog):
        self.setup_db(dvs)

        # create port channel
        self.create_port_channel(dvs, "PortChannel002")

        # add port channel members
        self.add_port_channel_members(dvs, "PortChannel002", ["Ethernet0", "Ethernet4"])

        # assign IP to interface
        self.add_ip_address("PortChannel002", "40.0.0.8/29")

        # configure MTU to interface
        self.set_mtu("PortChannel002", "8888")

        # check ASIC router interface database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
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
                    # assert the new value set to the router interface
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_MTU":
                        assert fv[1] == "8888"

        # check ASIC port database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        port_entries = tbl.getKeys()

        for key in port_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            # a member port configured with MTU will have six field/value tuples
            if len(fvs) == 6:
                for fv in fvs:
                    # asser the new value 8888 + 22 = 8910 set to the port
                    if fv[0] == "SAI_PORT_ATTR_MTU":
                        assert fv[1] == "8910"

        # remove IP from interface
        self.remove_ip_address("PortChannel002", "40.0.0.8/29")

        # remove port channel members
        self.remove_port_channel_members(dvs, "PortChannel002", ["Ethernet0", "Ethernet4"])

        # remove port channel
        self.remove_port_channel(dvs, "PortChannel002")
