from swsscommon import swsscommon

import time
import json

class TestRouterInterfaceMac(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        self.cntdb = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)

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

    def get_port_oid(self, interface):
        tbl = swsscommon.Table(self.cntdb, "COUNTERS_PORT_NAME_MAP")
        fvs = tbl.get('')[1]
        for fv in fvs:
            if fv[0] == interface:
                return fv[1]
        return None

    def find_mac(self, interface, mac):
        port_oid = self.get_port_oid(interface)
        assert port_oid is not None

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
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
        self.setup_db(dvs)

        # assign IP to interface
        self.add_ip_address("Ethernet8", "10.0.0.4/31")

        # set MAC to interface
        self.set_mac("Ethernet8", "6C:EC:5A:11:22:33", "10.0.0.4/31")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Ethernet8")
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        values = dict(fvs)
        assert values["mac_addr"] == "6c:ec:5a:11:22:33"

        # check ASIC router interface database
        src_mac_addr_found = self.find_mac("Ethernet8", "6C:EC:5A:11:22:33")
        assert src_mac_addr_found == True

        # remove IP from interface
        self.remove_ip_address("Ethernet8", "10.0.0.4/31")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

    def test_InterfaceChangeMac(self, dvs, testlog):
        self.setup_db(dvs)

        # assign IP to interface
        self.add_ip_address("Ethernet12", "12.0.0.4/31")

        # set MAC to interface
        self.set_mac("Ethernet12", "6C:EC:5A:22:33:44", "12.0.0.4/31")

        # change interface MAC
        self.set_mac("Ethernet12", "6C:EC:5A:33:44:55", "12.0.0.4/31")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Ethernet12")
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        values = dict(fvs)
        assert values["mac_addr"] == "6c:ec:5a:33:44:55"

        # check ASIC router interface database
        src_mac_addr_found = self.find_mac("Ethernet12", "6C:EC:5A:33:44:55")
        assert src_mac_addr_found == True

        # remove IP from interface
        self.remove_ip_address("Ethernet12", "12.0.0.4/31")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Ethernet12")
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

    def find_mac(self, lag_oid, mac):
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
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
        self.setup_db(dvs)

        # create port channel
        self.create_port_channel(dvs, "PortChannel001")

        # assign IP to interface
        self.add_ip_address("PortChannel001", "30.0.0.4/31")

        # set MAC to interface
        self.set_mac("PortChannel001", "6C:EC:5A:11:22:33", "30.0.0.4/31")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:PortChannel001")
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        values = dict(fvs)
        assert values["mac_addr"] == "6c:ec:5a:11:22:33"

        # get PortChannel oid; When sonic-swss pr885 is complete, you can get oid directly from COUNTERS_LAG_NAME_MAP, which would be better.
        lag_tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_LAG")
        lag_entries = lag_tbl.getKeys()
        # At this point there should be only one lag in the system, which is PortChannel001.
        assert len(lag_entries) == 1
        lag_oid = lag_entries[0]

        # check ASIC router interface database
        src_mac_addr_found = self.find_mac(lag_oid, "6C:EC:5A:11:22:33")
        assert src_mac_addr_found == True

        # remove IP from interface
        self.remove_ip_address("PortChannel001", "30.0.0.4/31")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:PortChannel001")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # remove port channel
        self.remove_port_channel(dvs, "PortChannel001")

    def test_InterfaceChangeMac(self, dvs, testlog):
        self.setup_db(dvs)

        # create port channel
        self.create_port_channel(dvs, "PortChannel002")

        # assign IP to interface
        self.add_ip_address("PortChannel002", "32.0.0.4/31")

        # set MAC to interface
        self.set_mac("PortChannel002", "6C:EC:5A:22:33:44", "32.0.0.4/31")

        # change interface MAC
        self.set_mac("PortChannel002", "6C:EC:5A:33:44:55", "32.0.0.4/31")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:PortChannel002")
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        values = dict(fvs)
        assert values["mac_addr"] == "6c:ec:5a:33:44:55"

        # get PortChannel oid; When sonic-swss pr885 is complete, you can get oid directly from COUNTERS_LAG_NAME_MAP, which would be better.
        lag_tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_LAG")
        lag_entries = lag_tbl.getKeys()
        # At this point there should be only one lag in the system, which is PortChannel002.
        assert len(lag_entries) == 1
        lag_oid = lag_entries[0]

        # check ASIC router interface database
        src_mac_addr_found = self.find_mac(lag_oid, "6C:EC:5A:33:44:55")
        assert src_mac_addr_found == True

        # remove IP from interface
        self.remove_ip_address("PortChannel002", "32.0.0.4/31")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:PortChannel002")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # remove port channel
        self.remove_port_channel(dvs, "PortChannel002")