from swsscommon import swsscommon

import time
import json

class TestRouterInterfaceMac(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

    def add_ip_address(self, interface, ip):
        tbl = swsscommon.Table(self.cdb, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        tbl.set(interface + "|" + ip, fvs)
        time.sleep(2) # IPv6 netlink message needs longer time

    def remove_ip_address(self, interface, ip):
        tbl = swsscommon.Table(self.cdb, "INTERFACE")
        tbl._del(interface + "|" + ip);
        time.sleep(1)

    def set_mac(self, interface, mac, ip):
        tbl = swsscommon.Table(self.cdb, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("mac_addr", mac)])
        tbl.set(interface + "|" + ip, fvs)
        time.sleep(2)
        
    def test_InterfaceSetMac(self, dvs, testlog):
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
        #assert len(intf_entries) == 2

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

        # set MAC to interface
        self.set_mac("Ethernet8", "6C:EC:5A:11:22:33", "10.0.0.4/31")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Ethernet8")
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        assert len(fvs) == 3
        for fv in fvs:
            if fv[0] == "scope":
                assert fv[1] == "global"
            elif fv[0] == "family":
                assert fv[1] == "IPv4"
            elif fv[0] == "mac_addr":
                assert fv[1] == "6c:ec:5a:11:22:33"
            else:
                assert False

        # check ASIC router interface database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # one loopback router interface one port based router interface
        #assert len(intf_entries) == 2

        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            # a port based router interface has five field/value tuples
            if len(fvs) == 5:
                for fv in fvs:
                    # check the MAC
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS":
                        assert fv[1] == "6C:EC:5A:11:22:33"
                        
        # remove IP from interface
        self.remove_ip_address("Ethernet8", "10.0.0.4/31")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

class TestLagRouterInterfaceMac(object):
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

    def set_mac(self, interface, mac, ip):
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL_INTERFACE")
        fvs = swsscommon.FieldValuePairs([("mac_addr", mac)])
        tbl.set(interface + "|" + ip, fvs)
        time.sleep(2)
        
    def test_InterfaceSetMac(self, dvs, testlog):
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
        #assert len(intf_entries) == 2

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

        # set MAC to interface
        self.set_mac("PortChannel001", "6C:EC:5A:11:22:33", "30.0.0.4/31")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:PortChannel001")
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        assert len(fvs) == 3
        for fv in fvs:
            if fv[0] == "scope":
                assert fv[1] == "global"
            elif fv[0] == "family":
                assert fv[1] == "IPv4"
            elif fv[0] == "mac_addr":
                assert fv[1] == "6c:ec:5a:11:22:33"
            else:
                assert False

        # check ASIC router interface database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # one loopback router interface one port based router interface
        #assert len(intf_entries) == 2

        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            # a port based router interface has five field/value tuples
            if len(fvs) == 5:
                for fv in fvs:
                    # check the MAC
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS":
                        assert fv[1] == "6C:EC:5A:11:22:33"
                        
        # remove IP from interface
        self.remove_ip_address("PortChannel001", "30.0.0.4/31")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:PortChannel001")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # remove port channel
        self.remove_port_channel(dvs, "PortChannel001")

