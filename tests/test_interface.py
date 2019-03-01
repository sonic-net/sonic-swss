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
                    # the default MTU without any configuration is 9100
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_MTU":
                        assert fv[1] == "9100"

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
                    # the default MTU without any configuration is 9100
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_MTU":
                        assert fv[1] == "9100"

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
                    # the default MTU without any configuration is 9100
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_MTU":
                        assert fv[1] == "9100"

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

class TestLoopbackRouterInterface(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

    def createLoIntf(self, interface, ip):
        tbl = swsscommon.Table(self.cdb, "LOOPBACK_INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        tbl.set(interface + "|" + ip, fvs)
        time.sleep(1)

    def removeLoIntf(self, interface, ip):
        tbl = swsscommon.Table(self.cdb, "LOOPBACK_INTERFACE")
        tbl._del(interface + "|" + ip);
        time.sleep(1)

    def test_InterfacesCreateRemove(self, dvs, testlog):
        self.setup_db(dvs)

        # Create loopback interfaces
        self.createLoIntf("Loopback0", "10.1.0.1/32")
        self.createLoIntf("Loopback1", "10.1.0.2/32")

        # Check configuration database
        tbl = swsscommon.Table(self.cdb, "LOOPBACK_INTERFACE")
        intf_entries = tbl.getKeys()

        assert len(intf_entries) == 2
        assert "Loopback0|10.1.0.1/32" in intf_entries
        assert "Loopback1|10.1.0.2/32" in intf_entries

        # Check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:lo")
        intf_entries = tbl.getKeys()

        assert len(intf_entries) == 2
        assert "10.1.0.1/32" in intf_entries
        assert "10.1.0.2/32" in intf_entries

        # Check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.1.0.1/32":
                lo0_ip2me_found = True
            if route["dest"] == "10.1.0.2/32":
                lo1_ip2me_found = True

        assert lo0_ip2me_found and lo1_ip2me_found

        # Remove lopback interfaces
        self.removeLoIntf("Loopback0", "10.1.0.1/32")
        self.removeLoIntf("Loopback1", "10.1.0.2/32")

        # Check configuration database
        tbl = swsscommon.Table(self.cdb, "LOOPBACK_INTERFACE")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # Check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:lo")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # Check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.1.0.1/32":
                assert False
            if route["dest"] == "10.1.0.2/32":
                assert False


#
# Testing interface-overlapping scenarios.
#
class TestRouterInterfaceOverlap(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

    def set_admin_status(self, interface, status):
        tbl = swsscommon.Table(self.cdb, "PORT")
        fvs = swsscommon.FieldValuePairs([("admin_status", status)])
        tbl.set(interface, fvs)
        time.sleep(1)

    def add_ip_address(self, interface, ip, afi, scope):
        tbl = swsscommon.Table(self.cdb, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("scope", scope),
                                          ("family", afi)])

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

    #
    # Verifies that the passed interface is present in AppDB.
    #
    # intf: Interface being tested
    # addr: ip-address of the interface
    # afi: address-family
    # scope: "global" vs "local" scope
    #
    def verify_appdb_ip_intf_addr_presence(self, intf, addr, afi, scope):

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:%s" % intf)
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        assert intf_entries[0] == addr

        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        assert len(fvs) == 2
        for fv in fvs:
            if fv[0] == "scope":
                assert fv[1] == scope
            elif fv[0] == "family":
                assert fv[1] == afi
            else:
                assert False

    #
    # Verifies that the passed interface is NOT present in AppDB.
    #
    # intf: Interface being tested
    #
    def verify_appdb_ip_intf_addr_absence(self, intf):

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:%s" % intf)
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

    #
    # Verifies that the passed interface-routes are present in asicDB.
    #
    # addr1: IPsubnet route
    # addr2: IP2me route
    # intfsNum: Number of explicitly-configured router-interfaces expected in the
    #           system. Keep in mind that loopback interface is always present.
    #
    def verify_asicdb_ip_intf_addr_presence(self, addr1, addr2, intfsNum):

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # One loopback router-interface plus the number of newly-configured intfs
        assert len(intf_entries) == 1 + intfsNum

        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            # a port based router interface has five field/value tuples
            if len(fvs) == 5:
                for fv in fvs:
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_TYPE":
                        assert fv[1] == "SAI_ROUTER_INTERFACE_TYPE_PORT"
                    # the default MTU without any configuration is 9100
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_MTU":
                        assert fv[1] == "9100"

        # check ASIC route database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == addr1:
                subnet_found = True
            if route["dest"] == addr2:
                ip2me_found = True

        assert subnet_found and ip2me_found

    #
    # Verifies that the passed interface-routes are NOT present in asicDB.
    #
    # addr1: IPsubnet route
    # addr2: IP2me route
    # subnet_expected: "True" if the subnet-route's presence is expected.
    #
    def verify_asicdb_ip_intf_addr_absence(self, addr1, addr2, subnet_expected):

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if subnet_expected == False and route["dest"] == addr1:
                assert False
            if route["dest"] == addr2:
                assert False


    #
    # IPv4 partial-overlap scenario:
    #
    # intf #1: 10.1.1.1/24
    # intf #2: 10.1.1.2/24
    #
    # Success criteria:
    #
    # - Only one IPsubnet route ("10.1.1.0/24") must be injected in asicDB
    #   corresponding to the first interface being created.
    # - Both IP2me routes ("10.1.1.1/32" & "10.1.1.2/32") must be injected
    #   into asicDB corresponding to the two interfaces being created.
    # - Verify internal router-intfs are created/eliminated.
    #
    def test_IPv4InterfacePartialOverlap(self, dvs, testlog):

        self.setup_db(dvs)

        # Add an ip-address to interface #1
        self.add_ip_address("Ethernet8", "10.1.1.1/24", "IPv4", "global")

        self.verify_appdb_ip_intf_addr_presence("Ethernet8", "10.1.1.1/24", "IPv4", "global")

        self.verify_asicdb_ip_intf_addr_presence("10.1.1.0/24", "10.1.1.1/32", 1)

        #
        # Adding an overlapping ip-address to interface #2
        #
        self.add_ip_address("Ethernet12", "10.1.1.2/24", "IPv4", "global")

        self.verify_appdb_ip_intf_addr_presence("Ethernet12", "10.1.1.2/24", "IPv4", "global")

        self.verify_asicdb_ip_intf_addr_presence("10.1.1.0/24", "10.1.1.2/32", 2)

        #
        # Remove overlapped ip-address from interface #2
        #
        self.remove_ip_address("Ethernet12", "10.1.1.2/24")

        self.verify_appdb_ip_intf_addr_absence("Ethernet12")

        self.verify_asicdb_ip_intf_addr_absence("10.1.1.0/24", "10.1.1.2/32", True)

        #
        # Remove ip-address from interface #1
        #
        self.remove_ip_address("Ethernet8", "10.1.1.1/24")

        self.verify_appdb_ip_intf_addr_absence("Ethernet8")

        self.verify_asicdb_ip_intf_addr_absence("10.1.1.0/24", "10.1.1.1/32", False)


    #
    # IPv4 partial-overlap scenario (reversed):
    #
    # intf #1: 10.1.1.1/24
    # intf #2: 10.1.1.2/24
    #
    # Success criteria:
    #
    # - Same as above. The difference this time is that, as we are eliminating the
    #   interface with the primary IPsubnet route ("10.1.1.0/24"), our implementation
    #   must 'resurrect' the equivalent subnet-route associated to the secondary
    #   intf.
    #
    def test_IPv4InterfacePartialOverlapReversed(self, dvs, testlog):

        self.setup_db(dvs)

        # Add an ip-address to interface #1
        self.add_ip_address("Ethernet8", "10.1.1.1/24", "IPv4", "global")

        self.verify_appdb_ip_intf_addr_presence("Ethernet8", "10.1.1.1/24", "IPv4", "global")

        self.verify_asicdb_ip_intf_addr_presence("10.1.1.0/24", "10.1.1.1/32", 1)

        #
        # Adding overlapping ip-address to interface #2
        #
        self.add_ip_address("Ethernet12", "10.1.1.2/24", "IPv4", "global")

        self.verify_appdb_ip_intf_addr_presence("Ethernet12", "10.1.1.2/24", "IPv4", "global")

        self.verify_asicdb_ip_intf_addr_presence("10.1.1.0/24", "10.1.1.2/32", 2)

        #
        # Remove overlapped ip-address from interface #1
        #
        self.remove_ip_address("Ethernet8", "10.1.1.1/24")

        self.verify_appdb_ip_intf_addr_absence("Ethernet8")

        self.verify_asicdb_ip_intf_addr_absence("10.1.1.0/24", "10.1.1.1/32", True)

        #
        # Remove ip-address from interface #2
        #
        self.remove_ip_address("Ethernet12", "10.1.1.2/24")

        self.verify_appdb_ip_intf_addr_absence("Ethernet12")

        self.verify_asicdb_ip_intf_addr_absence("10.1.1.0/24", "10.1.1.2/32", False)


    #
    # IPv4 full-overlap scenario:
    #
    # intf #1: 10.1.1.1/24
    # intf #2: 10.1.1.1/24
    #
    # Success criteria:
    #
    # - Only one IPsubnet route ("10.1.1.0/24") must be injected in asicDB
    #   corresponding to the first interface being created.
    # - Only one IP2me route ("10.1.1.1/32") must be injected into asicDB
    #   corresponding to the first interface being created.
    # - Verify internal router-intfs are created/eliminated.
    #
    def test_IPv4InterfaceFullOverlap(self, dvs, testlog):

        self.setup_db(dvs)

        # Add an ip-address to interface #1
        self.add_ip_address("Ethernet8", "10.1.1.1/24", "IPv4", "global")

        self.verify_appdb_ip_intf_addr_presence("Ethernet8", "10.1.1.1/24", "IPv4", "global")

        self.verify_asicdb_ip_intf_addr_presence("10.1.1.0/24", "10.1.1.1/32", 1)

        #
        # Adding an overlapping ip-address to interface #2
        #
        self.add_ip_address("Ethernet12", "10.1.1.1/24", "IPv4", "global")

        self.verify_appdb_ip_intf_addr_presence("Ethernet12", "10.1.1.1/24", "IPv4", "global")

        self.verify_asicdb_ip_intf_addr_presence("10.1.1.0/24", "10.1.1.1/32", 2)

        #
        # Remove overlapped ip-address from interface #2
        #
        self.remove_ip_address("Ethernet12", "10.1.1.1/24")

        self.verify_appdb_ip_intf_addr_absence("Ethernet12")

        self.verify_asicdb_ip_intf_addr_presence("10.1.1.0/24", "10.1.1.1/32", 1)

        #
        # Remove ip-address from interface #1
        #
        self.remove_ip_address("Ethernet8", "10.1.1.1/24")

        self.verify_appdb_ip_intf_addr_absence("Ethernet8")

        self.verify_asicdb_ip_intf_addr_absence("10.1.1.0/24", "10.1.1.1/32", False)


    #
    # IPv4 full-overlap scenario:
    #
    # intf #1: 10.1.1.1/24
    # intf #2: 10.1.1.1/24
    #
    # Success criteria:
    #
    # - Same as above. The difference this time is that, as we are eliminating the
    #   interface with the primary IPsubnet route ("10.1.1.0/24") AND the primary
    #   IP2me route ("10.1.1.1/24"), our implementation must 'resurrect' both
    #   routes associated to the secondary interface.
    #
    def test_IPv4InterfaceFullOverlapReversed(self, dvs, testlog):

        self.setup_db(dvs)

        # Add an ip-address to interface #1
        self.add_ip_address("Ethernet8", "10.1.1.1/24", "IPv4", "global")

        self.verify_appdb_ip_intf_addr_presence("Ethernet8", "10.1.1.1/24", "IPv4", "global")

        self.verify_asicdb_ip_intf_addr_presence("10.1.1.0/24", "10.1.1.1/32", 1)

        #
        # Adding overlapping ip-address to interface #2
        #
        self.add_ip_address("Ethernet12", "10.1.1.1/24", "IPv4", "global")

        self.verify_appdb_ip_intf_addr_presence("Ethernet12", "10.1.1.1/24", "IPv4", "global")

        self.verify_asicdb_ip_intf_addr_presence("10.1.1.0/24", "10.1.1.1/32", 2)

        #
        # Remove overlapped ip-address from interface #1
        #
        self.remove_ip_address("Ethernet8", "10.1.1.1/24")

        self.verify_appdb_ip_intf_addr_absence("Ethernet8")

        self.verify_asicdb_ip_intf_addr_presence("10.1.1.0/24", "10.1.1.1/32", 1)


        #
        # Remove ip-address from interface #2
        #
        self.remove_ip_address("Ethernet12", "10.1.1.1/24")

        self.verify_appdb_ip_intf_addr_absence("Ethernet12")

        self.verify_asicdb_ip_intf_addr_absence("10.1.1.0/24", "10.1.1.1/32", False)


    #
    # IPv6 partial-overlap scenario:
    #
    # intf #1: fc00:1:1::1/64
    # intf #2: fc00:1:1::2/64
    #
    # Success criteria:
    #
    # - Only one IPsubnet route ("fc00:1:1::/64") must be injected in asicDB
    #   corresponding to the first interface being created.
    # - Both IP2me routes ("fc00:1:1::1/128" & "fc00:1:1::2/128") must be injected
    #   into asicDB corresponding to the two interfaces being created.
    # - Verify internal router-intfs are created/eliminated.
    #
    def test_IPv6InterfacePartialOverlap(self, dvs, testlog):

        self.setup_db(dvs)

        # Add a new ip-address to interface #1
        self.add_ip_address("Ethernet8", "fc00:1:1::1/64", "IPv6", "global")

        self.verify_appdb_ip_intf_addr_presence("Ethernet8", "fc00:1:1::1/64", "IPv6", "global")

        self.verify_asicdb_ip_intf_addr_presence("fc00:1:1::/64", "fc00:1:1::1/128", 1)

        #
        # Adding an overlapping ip-address to interface #2
        #
        self.add_ip_address("Ethernet12", "fc00:1:1::2/64", "IPv6", "global")

        self.verify_appdb_ip_intf_addr_presence("Ethernet12", "fc00:1:1::2/64", "IPv6", "global")

        self.verify_asicdb_ip_intf_addr_presence("fc00:1:1::/64", "fc00:1:1::2/128", 2)

        #
        # Remove overlapped ip-address from interface #2
        #
        self.remove_ip_address("Ethernet12", "fc00:1:1::2/64")

        self.verify_appdb_ip_intf_addr_absence("Ethernet12")

        self.verify_asicdb_ip_intf_addr_absence("fc00:1:1::/64", "fc00:1:1::2/128", True)

        #
        # Remove ip-address from interface #1
        #
        self.remove_ip_address("Ethernet8", "fc00:1:1::1/64")

        self.verify_appdb_ip_intf_addr_absence("Ethernet8")

        self.verify_asicdb_ip_intf_addr_absence("fc00:1:1::/64", "fc00:1:1::1/128", False)


    #
    # IPv6 partial-overlap scenario (reversed):
    #
    # intf #1: fc00:1:1::1/64
    # intf #2: fc00:1:1::2/64
    #
    # Success criteria:
    #
    # - Same as above. The difference this time is that, as we are eliminating the
    #   interface with the primary IPsubnet route ("fc00:1:1::/64"), our implementation
    #   must 'resurrect' the equivalent subnet-route associated to the secondary
    #   intf.
    #
    def test_IPv6InterfacePartialOverlapReversed(self, dvs, testlog):

        self.setup_db(dvs)

        # Add a new ip-address to interface #!
        self.add_ip_address("Ethernet8", "fc00:1:1::1/64", "IPv6", "global")

        self.verify_appdb_ip_intf_addr_presence("Ethernet8", "fc00:1:1::1/64", "IPv6", "global")

        self.verify_asicdb_ip_intf_addr_presence("fc00:1:1::/64", "fc00:1:1::1/128", 1)

        #
        # Adding overlap ip-address to a interface #2
        #
        self.add_ip_address("Ethernet12", "fc00:1:1::2/64", "IPv6", "global")

        self.verify_appdb_ip_intf_addr_presence("Ethernet12", "fc00:1:1::2/64", "IPv6", "global")

        self.verify_asicdb_ip_intf_addr_presence("fc00:1:1::/64", "fc00:1:1::2/128", 2)

        #
        # Remove overlapped ip-address from interface #1
        #
        self.remove_ip_address("Ethernet8", "fc00:1:1::1/64")

        self.verify_appdb_ip_intf_addr_absence("Ethernet8")

        self.verify_asicdb_ip_intf_addr_absence("fc00:1:1::/64", "fc00:1:1::1/128", True)

        #
        # Remove ip-address from interface #2
        #
        self.remove_ip_address("Ethernet12", "fc00:1:1::2/64")

        self.verify_appdb_ip_intf_addr_absence("Ethernet12")

        self.verify_asicdb_ip_intf_addr_absence("fc00:1:1::/64", "fc00:1:1::2/128", False)


    #
    # IPv6 full-overlap scenario:
    #
    # intf #1: fc00:1:1::1/64
    # intf #2: fc00:1:1::1/64
    #
    # Success criteria:
    #
    # - Only one IPsubnet route ("fc00:1:1::/64") must be injected in asicDB
    #   corresponding to the first interface being created.
    # - Only one IP2me route ("fc00:1:1::1/128") must be injected into asicDB
    #   corresponding to the first interface being created.
    # - Verify internal router-intfs are created/eliminated.
    #
    def test_IPv6InterfaceFullOverlap(self, dvs, testlog):

        self.setup_db(dvs)

        # Add a new ip-address to interface #1
        self.add_ip_address("Ethernet8", "fc00:1:1::1/64", "IPv6", "global")

        self.verify_appdb_ip_intf_addr_presence("Ethernet8", "fc00:1:1::1/64", "IPv6", "global")

        self.verify_asicdb_ip_intf_addr_presence("fc00:1:1::/64", "fc00:1:1::1/128", 1)

        #
        # Adding an overlapping ip-address to interface #2
        #
        self.add_ip_address("Ethernet12", "fc00:1:1::1/64", "IPv6", "global")

        self.verify_appdb_ip_intf_addr_presence("Ethernet12", "fc00:1:1::1/64", "IPv6", "global")

        self.verify_asicdb_ip_intf_addr_presence("fc00:1:1::/64", "fc00:1:1::1/128", 2)

        #
        # Remove overlapped ip-address from interface #2
        #
        self.remove_ip_address("Ethernet12", "fc00:1:1::1/64")

        self.verify_appdb_ip_intf_addr_absence("Ethernet12")

        self.verify_asicdb_ip_intf_addr_presence("fc00:1:1::/64", "fc00:1:1::1/128", 1)

        #
        # Remove ip-address from interface #1
        #
        self.remove_ip_address("Ethernet8", "fc00:1:1::1/64")

        self.verify_appdb_ip_intf_addr_absence("Ethernet8")

        self.verify_asicdb_ip_intf_addr_absence("fc00:1:1::/64", "fc00:1:1::1/128", False)


    #
    # IPv6 full-overlap scenario (reversed):
    #
    # intf #1: fc00:1:1::1/64
    # intf #2: fc00:1:1::1/64
    #
    # Success criteria:
    #
    # - Same as above. The difference this time is that, as we are eliminating the
    #   interface with the primary IPsubnet route ("fc00:1:1::/64") AND the primary
    #   IP2me route ("fc00:1:1::1/128"), our implementation must 'resurrect' both
    #   routes associated to the secondary interface.
    #
    def test_IPv6InterfaceFullOverlapReversed(self, dvs, testlog):

        self.setup_db(dvs)

        # Add a new ip-address to interface #!
        self.add_ip_address("Ethernet8", "fc00:1:1::1/64", "IPv6", "global")

        self.verify_appdb_ip_intf_addr_presence("Ethernet8", "fc00:1:1::1/64", "IPv6", "global")

        self.verify_asicdb_ip_intf_addr_presence("fc00:1:1::/64", "fc00:1:1::1/128", 1)

        #
        # Adding overlap ip-address to a interface #2
        #
        self.add_ip_address("Ethernet12", "fc00:1:1::1/64", "IPv6", "global")

        self.verify_appdb_ip_intf_addr_presence("Ethernet12", "fc00:1:1::1/64", "IPv6", "global")

        self.verify_asicdb_ip_intf_addr_presence("fc00:1:1::/64", "fc00:1:1::1/128", 2)

        #
        # Remove overlapped ip-address from interface #1
        #
        self.remove_ip_address("Ethernet8", "fc00:1:1::1/64")

        self.verify_appdb_ip_intf_addr_absence("Ethernet8")

        self.verify_asicdb_ip_intf_addr_presence("fc00:1:1::/64", "fc00:1:1::1/128", 1)

        #
        # Remove ip-address from interface #2
        #
        self.remove_ip_address("Ethernet12", "fc00:1:1::1/64")

        self.verify_appdb_ip_intf_addr_absence("Ethernet12")

        self.verify_asicdb_ip_intf_addr_absence("fc00:1:1::/64", "fc00:1:1::1/128", False)


    #
    # IPv6 link-local partial-overlap scenario:
    #
    # intf #1: fe80:1:1::1/64
    # intf #2: fe80:1:1::2/64
    #
    # Success criteria:
    #
    # - Only one IPsubnet route ("fe80:1:1::/64") must be injected in asicDB
    #   corresponding to the first interface being created.
    # - Both IP2me routes ("fe80:1:1::1/128" & "fe80:1:1::2/128") must be injected
    #   into asicDB corresponding to the two interfaces being created.
    # - Verify internal router-intfs are created/eliminated.
    #
    def test_IPv6LinkLocalInterfacePartialOverlap(self, dvs, testlog):

        self.setup_db(dvs)

        # Add a regular ipv4 address to interface #1
        self.add_ip_address("Ethernet8", "fe80:1:1::1/64", "IPv6", "local")

        self.verify_appdb_ip_intf_addr_presence("Ethernet8", "fe80:1:1::1/64", "IPv6", "local")

        self.verify_asicdb_ip_intf_addr_presence("fe80:1:1::/64", "fe80:1:1::1/128", 1)

        #
        # Adding overlap ip-address to a interface #2
        #
        self.add_ip_address("Ethernet12", "fe80:1:1::2/64", "IPv6", "local")

        self.verify_appdb_ip_intf_addr_presence("Ethernet12", "fe80:1:1::2/64", "IPv6", "local")

        self.verify_asicdb_ip_intf_addr_presence("fe80:1:1::/64", "fe80:1:1::2/128", 2)

        #
        # Remove overlapped ip-address from interface #2
        #
        self.remove_ip_address("Ethernet12", "fe80:1:1::2/64")

        self.verify_appdb_ip_intf_addr_absence("Ethernet12")

        self.verify_asicdb_ip_intf_addr_absence("fe80:1:1::/64", "fe80:1:1::2/128", True)

        #
        # Remove ip-address from original interface #1
        #
        self.remove_ip_address("Ethernet8", "fe80:1:1::1/64")

        self.verify_appdb_ip_intf_addr_absence("Ethernet8")

        self.verify_asicdb_ip_intf_addr_absence("fe80:1:1::/64", "fe80:1:1::1/128", False)


    #
    # IPv6 link-local partial-overlap scenario (reversed):
    #
    # intf #1: fe80:1:1::1/64
    # intf #2: fe80:1:1::2/64
    #
    # Success criteria:
    #
    # - Same as above. The difference this time is that, as we are eliminating the
    #   interface with the primary IPsubnet route ("fe80:1:1::/64"), our implementation
    #   must 'resurrect' the equivalent subnet-route associated to the secondary
    #   intf.
    #
    def test_IPv6LinkLocalInterfacePartialOverlapReversed(self, dvs, testlog):

        self.setup_db(dvs)

        # Add a regular ipv4 address to interface #1
        self.add_ip_address("Ethernet8", "fe80:1:1::1/64", "IPv6", "local")

        self.verify_appdb_ip_intf_addr_presence("Ethernet8", "fe80:1:1::1/64", "IPv6", "local")

        self.verify_asicdb_ip_intf_addr_presence("fe80:1:1::/64", "fe80:1:1::1/128", 1)

        #
        # Adding overlap ip-address to a interface #2
        #
        self.add_ip_address("Ethernet12", "fe80:1:1::2/64", "IPv6", "local")

        self.verify_appdb_ip_intf_addr_presence("Ethernet12", "fe80:1:1::2/64", "IPv6", "local")

        self.verify_asicdb_ip_intf_addr_presence("fe80:1:1::/64", "fe80:1:1::2/128", 2)

        #
        # Remove overlapped ip-address from interface #1
        #
        self.remove_ip_address("Ethernet8", "fe80:1:1::1/64")

        self.verify_appdb_ip_intf_addr_absence("Ethernet8")

        self.verify_asicdb_ip_intf_addr_absence("fe80:1:1::/64", "fe80:1:1::1/128", True)

        #
        # Remove ip-address from original interface #2
        #
        self.remove_ip_address("Ethernet12", "fe80:1:1::2/64")

        self.verify_appdb_ip_intf_addr_absence("Ethernet12")

        self.verify_asicdb_ip_intf_addr_absence("fe80:1:1::/64", "fe80:1:1::2/128", False)


    #
    # IPv6 link-local full-overlap scenario:
    #
    # intf #1: fe80:1:1::1/64
    # intf #2: fe80:1:1::1/64
    #
    # Success criteria:
    #
    # - Only one IPsubnet route ("fe80:1:1::/64") must be injected in asicDB
    #   corresponding to the first interface being created.
    # - Only one IP2me route ("fe80:1:1::1/128") must be injected into asicDB
    #   corresponding to the first interface being created.
    # - Verify internal router-intfs are created/eliminated.
    #
    def test_IPv6LinkLocalInterfaceFullOverlap(self, dvs, testlog):

        self.setup_db(dvs)

        # Add a regular ipv4 address to interface #1
        self.add_ip_address("Ethernet8", "fe80:1:1::1/64", "IPv6", "local")

        self.verify_appdb_ip_intf_addr_presence("Ethernet8", "fe80:1:1::1/64", "IPv6", "local")

        self.verify_asicdb_ip_intf_addr_presence("fe80:1:1::/64", "fe80:1:1::1/128", 1)

        #
        # Adding overlap ip-address to a interface #2
        #
        self.add_ip_address("Ethernet12", "fe80:1:1::1/64", "IPv6", "local")

        self.verify_appdb_ip_intf_addr_presence("Ethernet12", "fe80:1:1::1/64", "IPv6", "local")

        self.verify_asicdb_ip_intf_addr_presence("fe80:1:1::/64", "fe80:1:1::1/128", 2)

        #
        # Remove overlapped ip-address from interface #2
        #
        self.remove_ip_address("Ethernet12", "fe80:1:1::1/64")

        self.verify_appdb_ip_intf_addr_absence("Ethernet12")

        self.verify_asicdb_ip_intf_addr_presence("fe80:1:1::/64", "fe80:1:1::1/128", 1)

        #
        # Remove ip-address from original interface #1
        #
        self.remove_ip_address("Ethernet8", "fe80:1:1::1/64")

        self.verify_appdb_ip_intf_addr_absence("Ethernet8")

        self.verify_asicdb_ip_intf_addr_absence("fe80:1:1::/64", "fe80:1:1::1/128", False)


    #
    # IPv6 link-local full-overlap scenario (reversed):
    #
    # intf #1: fe80:1:1::1/64
    # intf #2: fe80:1:1::1/64
    #
    # Success criteria:
    #
    # - Same as above. The difference this time is that, as we are eliminating the
    #   interface with the primary IPsubnet route ("fe80:1:1::/64") AND the primary
    #   IP2me route ("fe80:1:1::1/128"), our implementation must 'resurrect' both
    #   routes associated to the secondary interface.
    #
    def test_IPv6LinkLocalInterfaceFullOverlapReversed(self, dvs, testlog):

        self.setup_db(dvs)

        # Add a regular ipv4 address to interface #1
        self.add_ip_address("Ethernet8", "fe80:1:1::1/64", "IPv6", "local")

        self.verify_appdb_ip_intf_addr_presence("Ethernet8", "fe80:1:1::1/64", "IPv6", "local")

        self.verify_asicdb_ip_intf_addr_presence("fe80:1:1::/64", "fe80:1:1::1/128", 1)

        #
        # Adding overlap ip-address to a interface #2
        #
        self.add_ip_address("Ethernet12", "fe80:1:1::1/64", "IPv6", "local")

        self.verify_appdb_ip_intf_addr_presence("Ethernet12", "fe80:1:1::1/64", "IPv6", "local")

        self.verify_asicdb_ip_intf_addr_presence("fe80:1:1::/64", "fe80:1:1::1/128", 2)

        #
        # Remove overlapped ip-address from interface #1
        #
        self.remove_ip_address("Ethernet8", "fe80:1:1::1/64")

        self.verify_appdb_ip_intf_addr_absence("Ethernet8")

        self.verify_asicdb_ip_intf_addr_presence("fe80:1:1::/64", "fe80:1:1::1/128", 1)

        #
        # Remove ip-address from original interface #1
        #
        self.remove_ip_address("Ethernet12", "fe80:1:1::1/64")

        self.verify_appdb_ip_intf_addr_absence("Ethernet12")

        self.verify_asicdb_ip_intf_addr_absence("fe80:1:1::/64", "fe80:1:1::1/128", False)
