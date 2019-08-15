from swsscommon import swsscommon

import time
import json

class TestRouterInterfaceMac(object):
    def add_ip_address(self, dvs, interface, ip):
        tbl = swsscommon.Table(dvs.cdb, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        tbl.set(interface, fvs)
        tbl.set(interface + "|" + ip, fvs)
        time.sleep(1)

    def remove_ip_address(self, dvs, interface, ip):
        tbl = swsscommon.Table(dvs.cdb, "INTERFACE")
        tbl._del(interface + "|" + ip);
        tbl.hdel(interface, "NULL")
        time.sleep(1)

    def set_mac(self, dvs, interface, mac):
        tbl = swsscommon.Table(dvs.cdb, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("mac_addr", mac)])
        tbl.set(interface, fvs)
        time.sleep(1)

    def remove_mac(self, dvs, interface):
        tbl = swsscommon.Table(dvs.cdb, "INTERFACE")
        tbl.hdel(interface, "mac_addr")
        time.sleep(1)

    def find_mac(self, dvs, interface, mac):
        port_oid = dvs.asicdb.portnamemap[interface]

        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            values = dict(fvs)
            if "SAI_ROUTER_INTERFACE_TYPE_PORT" != values["SAI_ROUTER_INTERFACE_ATTR_TYPE"]:
                continue
            if port_oid == values["SAI_ROUTER_INTERFACE_ATTR_PORT_ID"] and mac == values["SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS"]:
                return True
        return False

    def test_InterfaceSetMac(self, dvs, testlog):
        dvs.setup_db()

        # assign IP to interface
        self.add_ip_address(dvs, "Ethernet8", "10.0.0.4/31")

        # set MAC to interface
        self.set_mac(dvs, "Ethernet8", "6C:EC:5A:11:22:33")

        # check application database
        tbl = swsscommon.Table(dvs.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get("Ethernet8")
        assert status == True
        values = dict(fvs)
        assert values["mac_addr"] == "6C:EC:5A:11:22:33"

        # check ASIC router interface database
        src_mac_addr_found = self.find_mac(dvs, "Ethernet8", "6C:EC:5A:11:22:33")
        assert src_mac_addr_found == True

        # remove IP from interface
        self.remove_ip_address(dvs, "Ethernet8", "10.0.0.4/31")

        # remove MAC from interface
        self.remove_mac(dvs, "Ethernet8")

    def test_InterfaceChangeMac(self, dvs, testlog):
        dvs.setup_db()

        # assign IP to interface
        self.add_ip_address(dvs, "Ethernet12", "12.0.0.4/31")

        # set MAC to interface
        self.set_mac(dvs, "Ethernet12", "6C:EC:5A:22:33:44")

        # change interface MAC
        self.set_mac(dvs, "Ethernet12", "6C:EC:5A:33:44:55")

        # check application database
        tbl = swsscommon.Table(dvs.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get("Ethernet12")
        assert status == True
        values = dict(fvs)
        assert values["mac_addr"] == "6C:EC:5A:33:44:55"

        # check ASIC router interface database
        src_mac_addr_found = self.find_mac(dvs, "Ethernet12", "6C:EC:5A:33:44:55")
        assert src_mac_addr_found == True

        # remove IP from interface
        self.remove_ip_address(dvs, "Ethernet12", "12.0.0.4/31")

        # remove MAC from interface
        self.remove_mac(dvs, "Ethernet12")

class TestLagRouterInterfaceMac(object):
    def create_port_channel(self, dvs, alias):
        tbl = swsscommon.Table(dvs.cdb, "PORTCHANNEL")
        fvs = swsscommon.FieldValuePairs([("admin_status", "up"),
                                          ("mtu", "9100")])
        tbl.set(alias, fvs)
        time.sleep(1)

    def remove_port_channel(self, dvs, alias):
        tbl = swsscommon.Table(dvs.cdb, "PORTCHANNEL")
        tbl._del(alias)
        time.sleep(1)

    def add_port_channel_members(self, dvs, lag, members):
        tbl = swsscommon.Table(dvs.cdb, "PORTCHANNEL_MEMBER")
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        for member in members:
            tbl.set(lag + "|" + member, fvs)
            time.sleep(1)

    def remove_port_channel_members(self, dvs, lag, members):
        tbl = swsscommon.Table(dvs.cdb, "PORTCHANNEL_MEMBER")
        for member in members:
            tbl._del(lag + "|" + member)
            time.sleep(1)

    def add_ip_address(self, dvs, interface, ip):
        tbl = swsscommon.Table(dvs.cdb, "PORTCHANNEL_INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        tbl.set(interface, fvs)
        tbl.set(interface + "|" + ip, fvs)
        time.sleep(1)

    def remove_ip_address(self, dvs, interface, ip):
        tbl = swsscommon.Table(dvs.cdb, "PORTCHANNEL_INTERFACE")
        tbl._del(interface + "|" + ip);
        tbl.hdel(interface, "NULL")
        time.sleep(1)

    def set_mac(self, dvs, interface, mac):
        tbl = swsscommon.Table(dvs.cdb, "PORTCHANNEL_INTERFACE")
        fvs = swsscommon.FieldValuePairs([("mac_addr", mac)])
        tbl.set(interface, fvs)
        time.sleep(1)

    def remove_mac(self, dvs, interface):
        tbl = swsscommon.Table(dvs.cdb, "PORTCHANNEL_INTERFACE")
        tbl.hdel(interface, "mac_addr")
        time.sleep(1)

    def find_mac(self, dvs, lag_oid, mac):
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()

        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            values = dict(fvs)
            if "SAI_ROUTER_INTERFACE_TYPE_PORT" != values["SAI_ROUTER_INTERFACE_ATTR_TYPE"]:
                continue
            if lag_oid == values["SAI_ROUTER_INTERFACE_ATTR_PORT_ID"] and mac == values["SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS"]:
                return True
        return False

    def test_InterfaceSetMac(self, dvs, testlog):
        dvs.setup_db()

        # create port channel
        self.create_port_channel(dvs, "PortChannel001")

        # assign IP to interface
        self.add_ip_address(dvs, "PortChannel001", "30.0.0.4/31")

        # set MAC to interface
        self.set_mac(dvs, "PortChannel001", "6C:EC:5A:11:22:33")

        # check application database
        tbl = swsscommon.Table(dvs.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get("PortChannel001")
        assert status == True
        values = dict(fvs)
        assert values["mac_addr"] == "6C:EC:5A:11:22:33"

        # get PortChannel oid; When sonic-swss pr885 is complete, you can get oid directly from COUNTERS_LAG_NAME_MAP, which would be better.
        lag_tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_LAG")
        lag_entries = lag_tbl.getKeys()
        # At this point there should be only one lag in the system, which is PortChannel001.
        assert len(lag_entries) == 1
        lag_oid = lag_entries[0]

        # check ASIC router interface database
        src_mac_addr_found = self.find_mac(dvs, lag_oid, "6C:EC:5A:11:22:33")
        assert src_mac_addr_found == True

        # remove IP from interface
        self.remove_ip_address(dvs, "PortChannel001", "30.0.0.4/31")

        # remove MAC from interface
        self.remove_mac(dvs, "PortChannel001")

        # remove port channel
        self.remove_port_channel(dvs, "PortChannel001")

    def test_InterfaceChangeMac(self, dvs, testlog):
        dvs.setup_db()

        # create port channel
        self.create_port_channel(dvs, "PortChannel002")

        # assign IP to interface
        self.add_ip_address(dvs, "PortChannel002", "32.0.0.4/31")

        # set MAC to interface
        self.set_mac(dvs, "PortChannel002", "6C:EC:5A:22:33:44")

        # change interface MAC
        self.set_mac(dvs, "PortChannel002", "6C:EC:5A:33:44:55")

        # check application database
        tbl = swsscommon.Table(dvs.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get("PortChannel002")
        assert status == True
        values = dict(fvs)
        assert values["mac_addr"] == "6C:EC:5A:33:44:55"

        # get PortChannel oid; When sonic-swss pr885 is complete, you can get oid directly from COUNTERS_LAG_NAME_MAP, which would be better.
        lag_tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_LAG")
        lag_entries = lag_tbl.getKeys()
        # At this point there should be only one lag in the system, which is PortChannel002.
        assert len(lag_entries) == 1
        lag_oid = lag_entries[0]

        # check ASIC router interface database
        src_mac_addr_found = self.find_mac(dvs, lag_oid, "6C:EC:5A:33:44:55")
        assert src_mac_addr_found == True

        # remove IP from interface
        self.remove_ip_address(dvs, "PortChannel002", "32.0.0.4/31")

        # remove MAC from interface
        self.remove_mac(dvs, "PortChannel002")

        # remove port channel
        self.remove_port_channel(dvs, "PortChannel002")
