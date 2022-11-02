import time
import json
import pytest

from swsscommon import swsscommon

VLAN_SUB_INTERFACE_SEPARATOR = '.'

class TestRouterInterface(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

    def set_admin_status(self, dvs, interface, status):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN"
        else:
            tbl_name = "PORT"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        fvs = swsscommon.FieldValuePairs([("admin_status", status)])
        tbl.set(interface, fvs)
        time.sleep(1)

        # when using FRR, route cannot be inserted if the neighbor is not
        # connected. thus it is mandatory to force the interface up manually
        if interface.startswith("PortChannel"):
            dvs.runcmd("bash -c 'echo " + ("1" if status == "up" else "0") +\
                    " > /sys/class/net/" + interface + "/carrier'")
        time.sleep(1)

    def create_vrf(self, vrf_name):
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        initial_entries = set(tbl.getKeys())

        tbl = swsscommon.Table(self.cdb, "VRF")
        fvs = swsscommon.FieldValuePairs([('empty', 'empty')])
        tbl.set(vrf_name, fvs)
        time.sleep(1)

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        current_entries = set(tbl.getKeys())
        assert len(current_entries - initial_entries) == 1
        return list(current_entries - initial_entries)[0]

    def remove_vrf(self, vrf_name):
        tbl = swsscommon.Table(self.cdb, "VRF")
        tbl._del(vrf_name)
        time.sleep(1)

    def create_l3_intf(self, interface, vrf_name):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        elif interface.startswith("Loopback"):
            tbl_name = "LOOPBACK_INTERFACE"
        else:
            tbl_name = "INTERFACE"
        if len(vrf_name) == 0:
            fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        else:
            fvs = swsscommon.FieldValuePairs([("vrf_name", vrf_name)])
        tbl = swsscommon.Table(self.cdb, tbl_name)
        tbl.set(interface, fvs)
        time.sleep(1)

    def remove_l3_intf(self, interface):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        elif interface.startswith("Loopback"):
            tbl_name = "LOOPBACK_INTERFACE"
        else:
            tbl_name = "INTERFACE"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        tbl._del(interface)
        time.sleep(1)

    def add_ip_address(self, interface, ip):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        elif interface.startswith("Loopback"):
            tbl_name = "LOOPBACK_INTERFACE"
        else:
            tbl_name = "INTERFACE"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        tbl.set(interface + "|" + ip, fvs)
        time.sleep(1)

    def remove_ip_address(self, interface, ip):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        elif interface.startswith("Loopback"):
            tbl_name = "LOOPBACK_INTERFACE"
        else:
            tbl_name = "INTERFACE"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        tbl._del(interface + "|" + ip)
        time.sleep(1)

    def set_mtu(self, interface, mtu):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN"
        else:
            tbl_name = "PORT"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        fvs = swsscommon.FieldValuePairs([("mtu", mtu)])
        tbl.set(interface, fvs)
        time.sleep(1)

    def test_PortInterfaceAddRemoveIpv6Address(self, dvs, testlog):
        self.setup_db(dvs)

        # create interface
        self.create_l3_intf("Ethernet8", "")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get("Ethernet8")
        assert status == True
        for fv in fvs:
            assert fv[0] != "vrf_name"

        # bring up interface
        # NOTE: For IPv6, only when the interface is up will the netlink message
        # get generated.
        self.set_admin_status(dvs, "Ethernet8", "up")

        # assign IP to interface
        self.add_ip_address("Ethernet8", "fc00::1/126")
        time.sleep(2)   # IPv6 netlink message needs longer time

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

        # remove interface
        self.remove_l3_intf("Ethernet8")

        # bring down interface
        self.set_admin_status(dvs, "Ethernet8", "down")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "Ethernet8"

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "fc00::/126":
                assert False
            if route["dest"] == "fc00::1/128":
                assert False

    def test_PortInterfaceAddRemoveIpv4Address(self, dvs, testlog):
        self.setup_db(dvs)

        # bring up interface
        self.set_admin_status(dvs, "Ethernet8", "up")

        # create interface
        self.create_l3_intf("Ethernet8", "")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get("Ethernet8")
        assert status == True
        for fv in fvs:
            assert fv[0] != "vrf_name"

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

        # remove interface
        self.remove_l3_intf("Ethernet8")

        # bring down interface
        self.set_admin_status(dvs, "Ethernet8", "down")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "Ethernet8"

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.0.0.4/31":
                assert False
            if route["dest"] == "10.0.0.4/32":
                assert False

    def test_PortInterfaceSetMtu(self, dvs, testlog):
        self.setup_db(dvs)

        # create interface
        self.create_l3_intf("Ethernet16", "")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get("Ethernet16")
        assert status == True
        for fv in fvs:
            assert fv[0] != "vrf_name"

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

        # remove interface
        self.remove_l3_intf("Ethernet16")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Ethernet16")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "Ethernet16"

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "20.0.0.8/29":
                assert False
            if route["dest"] == "20.0.0.8/32":
                assert False

    def test_PortInterfaceAddRemoveIpv6AddressWithVrf(self, dvs, testlog):
        self.setup_db(dvs)

        # bring up interface
        self.set_admin_status(dvs, "Ethernet8", "up")

        # create vrf
        vrf_oid = self.create_vrf("Vrf_0")

        # create interface with vrf
        self.create_l3_intf("Ethernet8", "Vrf_0")

        # check interface's vrf
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get("Ethernet8")
        assert status == True
        for fv in fvs:
            if fv[0] == "vrf_name":
                assert fv[1] == "Vrf_0"
                vrf_found = True
                break
        assert vrf_found == True

        # check linux kernel
        (exitcode, result) = dvs.runcmd(['sh', '-c', "ip link show Ethernet8 | grep Vrf"])
        assert "Vrf_0" in result

        # assign IP to interface
        self.add_ip_address("Ethernet8", "fc00::1/126")
        time.sleep(2)  # IPv6 netlink message needs longer time

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
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID":
                        assert fv[1] == vrf_oid

        # check ASIC route database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "fc00::/126":
                subnet_found = True
                assert route["vr"] == vrf_oid
            if route["dest"] == "fc00::1/128":
                ip2me_found = True
                assert route["vr"] == vrf_oid

        assert subnet_found and ip2me_found

        # remove IP from interface
        self.remove_ip_address("Ethernet8", "fc00::1/126")

        # remove vrf from interface
        self.remove_l3_intf("Ethernet8")

        # remove vrf
        self.remove_vrf("Vrf_0")

        # bring down interface
        self.set_admin_status(dvs, "Ethernet8", "down")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # check linux kernel
        (exitcode, result) = dvs.runcmd(['sh', '-c', "ip link show Ethernet8 | grep Vrf"])
        assert "Vrf_0" not in result

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "Ethernet8"

        tbl = swsscommon.Table(self.pdb, "VRF")
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

    def test_PortInterfaceAddRemoveIpv4AddressWithVrf(self, dvs, testlog):
        self.setup_db(dvs)

        # bring up interface
        self.set_admin_status(dvs, "Ethernet8", "up")

        # create vrf
        vrf_oid = self.create_vrf("Vrf_0")

        # create interface with vrf
        self.create_l3_intf("Ethernet8", "Vrf_0")

        # check interface's vrf
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get("Ethernet8")
        assert status == True
        for fv in fvs:
            if fv[0] == "vrf_name":
                assert fv[1] == "Vrf_0"
                vrf_found = True
                break
        assert vrf_found == True

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
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID":
                        assert fv[1] == vrf_oid

        # check ASIC route database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.0.0.4/31":
                subnet_found = True
                assert route["vr"] == vrf_oid
            if route["dest"] == "10.0.0.4/32":
                ip2me_found = True
                assert route["vr"] == vrf_oid

        assert subnet_found and ip2me_found

        # remove IP from interface
        self.remove_ip_address("Ethernet8", "10.0.0.4/31")

        # remove interface
        self.remove_l3_intf("Ethernet8")

        # remove vrf
        self.remove_vrf("Vrf_0")

        # bring down interface
        self.set_admin_status(dvs, "Ethernet8", "down")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "Ethernet8"

        tbl = swsscommon.Table(self.pdb, "VRF")
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

    def test_PortInterfaceAddSameIpv4AddressWithDiffVrf(self, dvs, testlog):
        self.setup_db(dvs)

        for i in [0, 4]:
            # record ASIC router interface database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
            old_intf_entries = set(tbl.getKeys())

            # record ASIC router entry database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            old_route_entries = set(tbl.getKeys())

            intf_name = "Ethernet" + str(i)
            vrf_name = "Vrf_" + str(i)

            # bring up interface
            self.set_admin_status(dvs, intf_name, "up")

            # create vrf
            vrf_oid = self.create_vrf(vrf_name)

            # create interface with vrf
            self.create_l3_intf(intf_name, vrf_name)

            # check interface's vrf
            tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
            (status, fvs) = tbl.get(intf_name)
            assert status == True
            for fv in fvs:
                if fv[0] == "vrf_name":
                    assert fv[1] == vrf_name
                    vrf_found = True
                    break
            assert vrf_found == True

            # assign IP to interface
            self.add_ip_address(intf_name, "10.0.0.4/31")

            # check application database
            tbl = swsscommon.Table(self.pdb, "INTF_TABLE:" + intf_name)
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
            current_intf_entries = set(tbl.getKeys())
            intf_entries = list(current_intf_entries - old_intf_entries)
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
                        if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID":
                            assert fv[1] == vrf_oid

            # check ASIC route database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            current_route_entries = set(tbl.getKeys())
            route_entries = list(current_route_entries - old_route_entries)

            for key in route_entries:
                route = json.loads(key)
                if route["dest"] == "10.0.0.4/31":
                    subnet_found = True
                    assert route["vr"] == vrf_oid
                if route["dest"] == "10.0.0.4/32":
                    ip2me_found = True
                    assert route["vr"] == vrf_oid

            assert subnet_found and ip2me_found


        for i in [0, 4]:
            # check ASIC router interface database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
            old_intf_entries = set(tbl.getKeys())

            intf_name = "Ethernet" + str(i)
            vrf_name = "Vrf_" + str(i)

            # remove IP from interface
            self.remove_ip_address(intf_name, "10.0.0.4/31")

            # remove interface
            self.remove_l3_intf(intf_name)

            # remove vrf
            self.remove_vrf(vrf_name)

            # bring down interface
            self.set_admin_status(dvs, intf_name, "down")

            # check application database
            tbl = swsscommon.Table(self.pdb, "INTF_TABLE:" + intf_name)
            intf_entries = tbl.getKeys()
            assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "Ethernet0" and entry[0] != "Ethernet4"

        tbl = swsscommon.Table(self.pdb, "VRF")
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

    def create_port_channel(self, alias):
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL")
        fvs = swsscommon.FieldValuePairs([("admin_status", "up"),
                                          ("mtu", "9100")])
        tbl.set(alias, fvs)
        time.sleep(1)

    def remove_port_channel(self, alias):
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL")
        tbl._del(alias)
        time.sleep(1)

    def add_port_channel_members(self, lag, members):
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL_MEMBER")
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        for member in members:
            tbl.set(lag + "|" + member, fvs)
            time.sleep(1)

    def remove_port_channel_members(self, lag, members):
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL_MEMBER")
        for member in members:
            tbl._del(lag + "|" + member)
            time.sleep(1)

    def test_LagInterfaceAddRemoveIpv6Address(self, dvs, testlog):
        self.setup_db(dvs)

        # create port channel
        self.create_port_channel("PortChannel001")

        # bring up interface
        self.set_admin_status(dvs, "PortChannel001", "up")

        # create l3 interface
        self.create_l3_intf("PortChannel001", "")

        # assign IP to interface
        self.add_ip_address("PortChannel001", "fc00::1/126")
        time.sleep(2)  # IPv6 netlink message needs longer time

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:PortChannel001")
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
        self.remove_ip_address("PortChannel001", "fc00::1/126")

        # remove interface
        self.remove_l3_intf("PortChannel001")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:PortChannel001")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "PortChannel001"

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "fc00::/126":
                assert False
            if route["dest"] == "fc00::1/128":
                assert False

        # remove port channel
        self.remove_port_channel("PortChannel001")

    def test_LagInterfaceAddRemoveIpv4Address(self, dvs, testlog):
        self.setup_db(dvs)

        # create port channel
        self.create_port_channel("PortChannel001")

        # bring up interface
        self.set_admin_status(dvs, "PortChannel001", "up")

        # create l3 interface
        self.create_l3_intf("PortChannel001", "")

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

        # remove l3 interface
        self.remove_l3_intf("PortChannel001")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:PortChannel001")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "PortChannel001"

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "30.0.0.4/31":
                assert False
            if route["dest"] == "30.0.0.4/32":
                assert False

        # remove port channel
        self.remove_port_channel("PortChannel001")

    @pytest.mark.skip(reason="vs image issue: Azure/sonic-sairedis#574")
    def test_LagInterfaceSetMtu(self, dvs, testlog):
        self.setup_db(dvs)

        # create port channel
        self.create_port_channel("PortChannel002")

        # add port channel members
        self.add_port_channel_members("PortChannel002", ["Ethernet0", "Ethernet4"])

        # create l3 interface
        self.create_l3_intf("PortChannel002", "")

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

        # remove l3 interface
        self.remove_l3_intf("PortChannel002")

        # remove port channel members
        self.remove_port_channel_members("PortChannel002", ["Ethernet0", "Ethernet4"])

        # remove port channel
        self.remove_port_channel("PortChannel002")

    def test_LagInterfaceAddRemoveIpv6AddressWithVrf(self, dvs, testlog):
        self.setup_db(dvs)

        # create vrf
        vrf_oid = self.create_vrf("Vrf_0")

        # create port channel
        self.create_port_channel("PortChannel001")

        # bring up interface
        self.set_admin_status(dvs, "PortChannel001", "up")

        # create l3 interface
        self.create_l3_intf("PortChannel001", "Vrf_0")

        # check interface's vrf
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get("PortChannel001")
        assert status == True
        for fv in fvs:
            if fv[0] == "vrf_name":
                assert fv[1] == "Vrf_0"
                vrf_found = True
                break
        assert vrf_found == True

        # assign IP to interface
        self.add_ip_address("PortChannel001", "fc00::1/126")
        time.sleep(2)  # IPv6 netlink message needs longer time

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:PortChannel001")
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
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID":
                        assert fv[1] == vrf_oid
        # check ASIC route database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "fc00::/126":
                subnet_found = True
                assert route["vr"] == vrf_oid
            if route["dest"] == "fc00::1/128":
                ip2me_found = True
                assert route["vr"] == vrf_oid

        assert subnet_found and ip2me_found

        # remove IP from interface
        self.remove_ip_address("PortChannel001", "fc00::1/126")

        # remove interface
        self.remove_l3_intf("PortChannel001")

        # remove vrf
        self.remove_vrf("Vrf_0")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:PortChannel001")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "PortChannel001"

        tbl = swsscommon.Table(self.pdb, "VRF")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "fc00::1/126":
                assert False
            if route["dest"] == "fc00::1/128":
                assert False

        # remove port channel
        self.remove_port_channel("PortChannel001")

    def test_LagInterfaceAddRemoveIpv4AddressWithVrf(self, dvs, testlog):
        self.setup_db(dvs)

        # create port channel
        self.create_port_channel("PortChannel001")

        # create vrf
        vrf_oid = self.create_vrf("Vrf_0")

        # create interface with vrf
        self.create_l3_intf("PortChannel001", "Vrf_0")

        # bring up interface
        self.set_admin_status(dvs, "PortChannel001", "up")

        # check interface's vrf
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get("PortChannel001")
        assert status == True
        for fv in fvs:
            if fv[0] == "vrf_name":
                assert fv[1] == "Vrf_0"
                vrf_found = True
                break
        assert vrf_found == True

        # check linux kernel
        (exitcode, result) = dvs.runcmd(['sh', '-c', "ip link show PortChannel001 | grep Vrf"])
        assert "Vrf_0" in result

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
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID":
                        assert fv[1] == vrf_oid

        # check ASIC route database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "30.0.0.4/31":
                subnet_found = True
                assert route["vr"] == vrf_oid
            if route["dest"] == "30.0.0.4/32":
                ip2me_found = True
                assert route["vr"] == vrf_oid

        assert subnet_found and ip2me_found

        # remove IP from interface
        self.remove_ip_address("PortChannel001", "30.0.0.4/31")

        # remove l3 interface
        self.remove_l3_intf("PortChannel001")

        # check linux kernel
        (exitcode, result) = dvs.runcmd(['sh', '-c', "ip link show PortChannel001 | grep Vrf"])
        assert "Vrf_0" not in result

        # remove vrf
        self.remove_vrf("Vrf_0")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:PortChannel001")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "PortChannel001"

        tbl = swsscommon.Table(self.pdb, "VRF")
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
        self.remove_port_channel("PortChannel001")

    def create_vlan(self, vlan_id):
        tbl = swsscommon.Table(self.cdb, "VLAN")
        fvs = swsscommon.FieldValuePairs([("vlanid", vlan_id)])
        tbl.set("Vlan" + vlan_id, fvs)
        time.sleep(1)

    def remove_vlan(self, vlan_id):
        tbl = swsscommon.Table(self.cdb, "VLAN")
        tbl._del("Vlan" + vlan_id)
        time.sleep(1)

    def create_vlan_member(self, vlan_id, interface):
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        fvs = swsscommon.FieldValuePairs([("tagging_mode", "untagged")])
        tbl.set("Vlan" + vlan_id + "|" + interface, fvs)
        time.sleep(1)

    def remove_vlan_member(self, vlan_id, interface):
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        tbl._del("Vlan" + vlan_id + "|" + interface)
        time.sleep(1)

    def test_VLanInterfaceAddRemoveIpv6Address(self, dvs, testlog):
        self.setup_db(dvs)

        # create vlan
        self.create_vlan("10")

        # add vlan member
        self.create_vlan_member("10", "Ethernet0")

        # bring up interface
        self.set_admin_status(dvs, "Ethernet0", "up")
        self.set_admin_status(dvs, "Vlan10", "up")

        # create vlan interface
        self.create_l3_intf("Vlan10", "")

        # assign IP to interface
        self.add_ip_address("Vlan10", "fc00::1/126")
        time.sleep(2)  # IPv6 netlink message needs longer time

        # check asic database and get vlan_oid
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 1
        vlan_oid = vlan_entries[0]

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Vlan10")
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
        # one loopback router interface one vlan router interface
        assert len(intf_entries) == 2

        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            if len(fvs) == 5:
                for fv in fvs:
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_TYPE":
                        assert fv[1] == "SAI_ROUTER_INTERFACE_TYPE_VLAN"
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_VLAN_ID":
                        assert fv[1] == vlan_oid

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
        self.remove_ip_address("Vlan10", "fc00::1/126")

        # remove interface
        self.remove_l3_intf("Vlan10")

        # remove vlan member
        self.remove_vlan_member("10", "Ethernet0")

        # remove vlan
        self.remove_vlan("10")

        # bring down interface
        self.set_admin_status(dvs, "Ethernet0", "down")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Vlan10")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "Vlan10"

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "fc00::/126":
                assert False
            if route["dest"] == "fc00::1/128":
                assert False

    def test_VLanInterfaceAddRemoveIpv4Address(self, dvs, testlog):
        self.setup_db(dvs)

        # create vlan
        self.create_vlan("10")

        # add vlan member
        self.create_vlan_member("10", "Ethernet0")

        # bring up interface
        self.set_admin_status(dvs, "Ethernet0", "up")
        self.set_admin_status(dvs, "Vlan10", "up")

        #create vlan interface
        self.create_l3_intf("Vlan10", "")

        # assign IP to interface
        self.add_ip_address("Vlan10", "10.0.0.4/31")

        # check asic database and get vlan_oid
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 1
        vlan_oid = vlan_entries[0]

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Vlan10")
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
        # one loopback router interface one vlan router interface
        assert len(intf_entries) == 2

        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            if len(fvs) == 5:
                for fv in fvs:
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_TYPE":
                        assert fv[1] == "SAI_ROUTER_INTERFACE_TYPE_VLAN"
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_VLAN_ID":
                        assert fv[1] == vlan_oid

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
        self.remove_ip_address("Vlan10", "10.0.0.4/31")

        # remove vlan interface
        self.remove_l3_intf("Vlan10")

        # remove vlan member
        self.remove_vlan_member("10", "Ethernet0")

        # remove vlan
        self.remove_vlan("10")

        # bring down interface
        self.set_admin_status(dvs, "Ethernet0", "down")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Vlan10")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "VLAN_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "Vlan10"

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.0.0.4/31":
                assert False
            if route["dest"] == "10.0.0.4/32":
                assert False

    def test_VLanInterfaceAddRemoveIpv6AddressWithVrf(self, dvs, testlog):
        self.setup_db(dvs)

        # create vlan
        self.create_vlan("10")

        # add vlan member
        self.create_vlan_member("10", "Ethernet0")

        # bring up interface
        self.set_admin_status(dvs, "Ethernet0", "up")
        self.set_admin_status(dvs, "Vlan10", "up")

        # create vrf
        vrf_oid = self.create_vrf("Vrf_0")

        # create vlan interface
        self.create_l3_intf("Vlan10", "Vrf_0")

        # check linux kernel
        (exitcode, result) = dvs.runcmd(['sh', '-c', "ip link show Vlan10 | grep Vrf"])
        assert "Vrf_0" in result

        # assign IP to interface
        self.add_ip_address("Vlan10", "fc00::1/126")
        time.sleep(2)  # IPv6 netlink message needs longer time

        # check interface's vrf
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get("Vlan10")
        assert status == True
        for fv in fvs:
            if fv[0] == "vrf_name":
                assert fv[1] == "Vrf_0"
                vrf_found = True
                break
        assert vrf_found == True

        # check asic database and get vlan_oid
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 1
        vlan_oid = vlan_entries[0]

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Vlan10")
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
        # one loopback router interface one vlan router interface
        assert len(intf_entries) == 2

        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            if len(fvs) == 5:
                for fv in fvs:
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_TYPE":
                        assert fv[1] == "SAI_ROUTER_INTERFACE_TYPE_VLAN"
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_VLAN_ID":
                        assert fv[1] == vlan_oid
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID":
                        assert fv[1] == vrf_oid

        # check ASIC route database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "fc00::/126":
                subnet_found = True
                assert route["vr"] == vrf_oid
            if route["dest"] == "fc00::1/128":
                ip2me_found = True
                assert route["vr"] == vrf_oid

        assert subnet_found and ip2me_found

        # remove IP from interface
        self.remove_ip_address("Vlan10", "fc00::1/126")

        # remove vlan interface
        self.remove_l3_intf("Vlan10")

        # check linux kernel
        (exitcode, result) = dvs.runcmd(['sh', '-c', "ip link show Vlan10 | grep Vrf"])
        assert "Vrf_0" not in result

        # remove vlan member
        self.remove_vlan_member("10", "Ethernet0")

        # remove vlan
        self.remove_vlan("10")

        # bring down interface
        self.set_admin_status(dvs, "Ethernet0", "down")

        # remove vrf
        self.remove_vrf("Vrf_0")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Vlan10")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "Vlan10"

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "fc00::/126":
                assert False
            if route["dest"] == "fc00::1/128":
                assert False

    def test_VLanInterfaceAddRemoveIpv4AddressWithVrf(self, dvs, testlog):
        self.setup_db(dvs)

        # create vlan
        self.create_vlan("10")

        # add vlan member
        self.create_vlan_member("10", "Ethernet0")

        # bring up interface
        self.set_admin_status(dvs, "Ethernet0", "up")
        self.set_admin_status(dvs, "Vlan10", "up")

        # create vrf
        vrf_oid = self.create_vrf("Vrf_0")

        # create vlan interface
        self.create_l3_intf("Vlan10", "Vrf_0")

        # assign IP to interface
        self.add_ip_address("Vlan10", "10.0.0.4/31")

        # check interface's vrf
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get("Vlan10")
        assert status == True
        for fv in fvs:
            if fv[0] == "vrf_name":
                assert fv[1] == "Vrf_0"
                vrf_found = True
                break
        assert vrf_found == True

        # check asic database and get vlan_oid
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 1
        vlan_oid = vlan_entries[0]

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Vlan10")
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
        # one loopback router interface one vlan router interface
        assert len(intf_entries) == 2

        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            if len(fvs) == 5:
                for fv in fvs:
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_TYPE":
                        assert fv[1] == "SAI_ROUTER_INTERFACE_TYPE_VLAN"
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_VLAN_ID":
                        assert fv[1] == vlan_oid
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID":
                        assert fv[1] == vrf_oid

        # check ASIC route database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.0.0.4/31":
                subnet_found = True
                assert route["vr"] == vrf_oid
            if route["dest"] == "10.0.0.4/32":
                ip2me_found = True
                assert route["vr"] == vrf_oid

        assert subnet_found and ip2me_found

        # remove IP from interface
        self.remove_ip_address("Vlan10", "10.0.0.4/31")

        # remove vlan interface
        self.remove_l3_intf("Vlan10")

        # remove vlan member
        self.remove_vlan_member("10", "Ethernet0")

        # remove vlan
        self.remove_vlan("10")

        # bring down interface
        self.set_admin_status(dvs, "Ethernet0", "down")

        # remove vrf
        self.remove_vrf("Vrf_0")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Vlan10")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "VLAN_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "Vlan10"

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.0.0.4/31":
                assert False
            if route["dest"] == "10.0.0.4/32":
                assert False

    def test_LoopbackInterfacesAddRemoveIpv4Address(self, dvs, testlog):
        self.setup_db(dvs)

        # Create loopback interfaces
        self.create_l3_intf("Loopback0", "")
        self.create_l3_intf("Loopback1", "")

        # add ip address
        self.add_ip_address("Loopback0", "10.1.0.1/32")
        self.add_ip_address("Loopback1", "10.1.0.2/32")

        # Check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Loopback0")
        intf_entries = tbl.getKeys()
        assert intf_entries[0] == "10.1.0.1/32"
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Loopback1")
        intf_entries = tbl.getKeys()
        assert intf_entries[0] == "10.1.0.2/32"

        # Check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.1.0.1/32":
                lo0_ip2me_found = True
            if route["dest"] == "10.1.0.2/32":
                lo1_ip2me_found = True

        assert lo0_ip2me_found and lo1_ip2me_found

        # Remove ip address
        self.remove_ip_address("Loopback0", "10.1.0.1/32")
        self.remove_ip_address("Loopback1", "10.1.0.2/32")

        # Remove interface
        self.remove_l3_intf("Loopback0")
        self.remove_l3_intf("Loopback1")

        # Check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Loopback0")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Loopback1")
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

    def test_LoopbackInterfacesAddRemoveIpv6Address(self, dvs, testlog):
        self.setup_db(dvs)

        # Create loopback interfaces
        self.create_l3_intf("Loopback0", "")
        self.create_l3_intf("Loopback1", "")

        # add ip address
        self.add_ip_address("Loopback0", "fc00::1/128")
        self.add_ip_address("Loopback1", "fd00::1/128")
        time.sleep(2)  # IPv6 netlink message needs longer time

        # Check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Loopback0")
        intf_entries = tbl.getKeys()
        assert intf_entries[0] == "fc00::1/128"
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Loopback1")
        intf_entries = tbl.getKeys()
        assert intf_entries[0] == "fd00::1/128"

        # Check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "fc00::1/128":
                lo0_ip2me_found = True
            if route["dest"] == "fd00::1/128":
                lo1_ip2me_found = True

        assert lo0_ip2me_found and lo1_ip2me_found

        # Remove ip address
        self.remove_ip_address("Loopback0", "fc00::1/128")
        self.remove_ip_address("Loopback1", "fd00::1/128")

        # Remove interface
        self.remove_l3_intf("Loopback0")
        self.remove_l3_intf("Loopback1")

        # Check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Loopback0")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Loopback1")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # Check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "fc00::1/128":
                assert False
            if route["dest"] == "fd00::1/128":
                assert False

    def test_LoopbackInterfaceIpv4AddressWithVrf(self, dvs, testlog):
        self.setup_db(dvs)

        for i in [0, 1]:
            # record ASIC router interface database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
            old_intf_entries = set(tbl.getKeys())

            # record ASIC router entry database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            old_route_entries = set(tbl.getKeys())

            intf_name = "Loopback" + str(i)
            vrf_name = "Vrf_" + str(i)

            # create vrf
            vrf_oid = self.create_vrf(vrf_name)

            # create interface with vrf
            self.create_l3_intf(intf_name, vrf_name)

            # check linux kernel
            (exitcode, result) = dvs.runcmd(['sh', '-c', "ip link show %s | grep Vrf" % intf_name])
            assert "%s" % vrf_name in result

            # check interface's vrf
            tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
            (status, fvs) = tbl.get(intf_name)
            assert status == True
            for fv in fvs:
                if fv[0] == "vrf_name":
                    assert fv[1] == vrf_name
                    vrf_found = True
                    break
            assert vrf_found == True

            # assign IP to interface
            self.add_ip_address(intf_name, "10.0.0.4/32")

            # check application database
            tbl = swsscommon.Table(self.pdb, "INTF_TABLE:" + intf_name)
            intf_entries = tbl.getKeys()
            assert len(intf_entries) == 1
            assert intf_entries[0] == "10.0.0.4/32"

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
            current_intf_entries = set(tbl.getKeys())
            intf_entries = list(current_intf_entries - old_intf_entries)
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
                        if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID":
                            assert fv[1] == vrf_oid

            # check ASIC route database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            current_route_entries = set(tbl.getKeys())
            route_entries = list(current_route_entries - old_route_entries)

            for key in route_entries:
                route = json.loads(key)
                if route["dest"] == "10.0.0.4/32":
                    ip2me_found = True
                    assert route["vr"] == vrf_oid

            assert ip2me_found


        for i in [0, 1]:
            # check ASIC router interface database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
            old_intf_entries = set(tbl.getKeys())

            intf_name = "Loopback" + str(i)
            vrf_name = "Vrf_" + str(i)

            # remove IP from interface
            self.remove_ip_address(intf_name, "10.0.0.4/32")

            # remove interface
            self.remove_l3_intf(intf_name)

            # remove vrf
            self.remove_vrf(vrf_name)

            # check linux kernel
            (exitcode, result) = dvs.runcmd(['sh', '-c', "ip link show %s | grep Vrf" % intf_name])
            assert "%s" % vrf_name not in result

            # check application database
            tbl = swsscommon.Table(self.pdb, "INTF_TABLE:" + intf_name)
            intf_entries = tbl.getKeys()
            assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "Loopback0" and entry[0] != "Loopback1"

        tbl = swsscommon.Table(self.pdb, "VRF")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # check ASIC database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for key in tbl.getKeys():
            route = json.loads(key)
            if route["dest"] == "10.0.0.4/32":
                assert False


    def create_ipv6_link_local(self, interface):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        else:
            tbl_name = "INTERFACE"

        fvs = swsscommon.FieldValuePairs([("ipv6_use_link_local_only", "enable")])
        tbl = swsscommon.Table(self.cdb, tbl_name)
        tbl.set(interface, fvs)
        time.sleep(1)

    def remove_ipv6_link_local(self, interface):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        else:
            tbl_name = "INTERFACE"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        tbl._del(interface)
        time.sleep(1)

    def test_InterfaceIpv6LinkLocalOnly(self, dvs, testlog):
        # Check enable/disable ipv6-link-local mode for physical interface
        self.setup_db(dvs)

        # create ipv6 link local interface
        self.create_ipv6_link_local("Ethernet8")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get("Ethernet8")
        assert status == True
        for fv in fvs:
            if fv[0] == "ipv6_use_link_local_only":
                ipv6_link_local_found = True
                assert fv[1] == "enable"

        assert ipv6_link_local_found

        # bring up interface
        self.set_admin_status(dvs, "Ethernet8", "up")

        # check ASIC router interface database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # one loopback router interface one port based router interface
        assert len(intf_entries) == 2

        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            # a port based router interface has five field/value tuples
            if len(fvs) >= 5:
                for fv in fvs:
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_TYPE":
                        assert fv[1] == "SAI_ROUTER_INTERFACE_TYPE_PORT"
                    # the default MTU without any configuration is 9100
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_MTU":
                        assert fv[1] == "9100"

        # remove ipv6 link local interface
        self.remove_ipv6_link_local("Ethernet8")

        # bring down interface
        self.set_admin_status(dvs, "Ethernet8", "down")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "Ethernet8"

        # check ASIC router interface database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # one loopback router interface
        assert len(intf_entries) == 1

    def test_LagInterfaceIpv6LinkLocalOnly(self, dvs, testlog):
        # Check enable/disable ipv6-link-local mode for lag interface
        self.setup_db(dvs)

        # create port channel
        self.create_port_channel("PortChannel001")

        # bring up interface
        self.set_admin_status(dvs, "PortChannel001", "up")

        # create ipv6 link local interface
        self.create_ipv6_link_local("PortChannel001")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get("PortChannel001")
        assert status == True
        for fv in fvs:
            if fv[0] == "ipv6_use_link_local_only":
                ipv6_link_local_found = True
                assert fv[1] == "enable"

        assert ipv6_link_local_found

        # check ASIC router interface database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # one loopback router interface one port based router interface
        assert len(intf_entries) == 2

        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            # a port based router interface has five field/value tuples
            if len(fvs) >= 5:
                for fv in fvs:
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_TYPE":
                        assert fv[1] == "SAI_ROUTER_INTERFACE_TYPE_PORT"
                    # the default MTU without any configuration is 9100
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_MTU":
                        assert fv[1] == "9100"

        # remove ipv6 link local interface
        self.remove_ipv6_link_local("PortChannel001")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:PortChannel001")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "PortChannel001"

        # check ASIC router interface database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # one loopback router interface
        assert len(intf_entries) == 1

        # remove port channel
        self.remove_port_channel("PortChannel001")


    def test_VLanInterfaceIpv6LinkLocalOnly(self, dvs, testlog):
        # Check enable/disable ipv6-link-local mode for vlan interface
        self.setup_db(dvs)

        # create vlan
        self.create_vlan("10")

        # add vlan member
        self.create_vlan_member("10", "Ethernet0")

        # bring up interface
        self.set_admin_status(dvs, "Ethernet0", "up")
        self.set_admin_status(dvs, "Vlan10", "up")

        # create ipv6 link local interface
        self.create_ipv6_link_local("Vlan10")

        # check asic database and get vlan_oid
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_entries = [k for k in tbl.getKeys() if k != dvs.asicdb.default_vlan_id]
        assert len(vlan_entries) == 1
        vlan_oid = vlan_entries[0]

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get("Vlan10")
        assert status == True
        for fv in fvs:
            if fv[0] == "ipv6_use_link_local_only":
                ipv6_link_local_found = True
                assert fv[1] == "enable"

        assert ipv6_link_local_found

        # check ASIC router interface database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # one loopback router interface one vlan router interface
        assert len(intf_entries) == 2

        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True
            if len(fvs) >= 5:
                for fv in fvs:
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_TYPE":
                        assert fv[1] == "SAI_ROUTER_INTERFACE_TYPE_VLAN"
                    if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_VLAN_ID":
                        assert fv[1] == vlan_oid


        # remove ipv6 link local interface
        self.remove_ipv6_link_local("Vlan10")

        # remove vlan member
        self.remove_vlan_member("10", "Ethernet0")

        # remove vlan
        self.remove_vlan("10")

        # bring down interface
        self.set_admin_status(dvs, "Ethernet0", "down")

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE:Vlan10")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        intf_entries = tbl.getKeys()
        for entry in intf_entries:
            assert entry[0] != "Vlan10"

        # check ASIC router interface database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()
        # one loopback router interface
        assert len(intf_entries) == 1

    def set_loopback_action(self, interface, action):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        else:
            sub_intf_sep_idx = interface.find(VLAN_SUB_INTERFACE_SEPARATOR)
            if sub_intf_sep_idx != -1:
                tbl_name = "VLAN_SUB_INTERFACE"
            else:
                tbl_name = "INTERFACE"

        fvs = swsscommon.FieldValuePairs([("loopback_action", action)])
        tbl = swsscommon.Table(self.cdb, tbl_name)
        tbl.set(interface, fvs)
        time.sleep(1)

    def loopback_action_test(self, iface, action):
        # create interface
        self.create_l3_intf(iface, "")

        # set interface loopback action in config db
        self.set_loopback_action(iface, action)

        # check application database
        tbl = swsscommon.Table(self.pdb, "INTF_TABLE")
        (status, fvs) = tbl.get(iface)
        assert status == True

        action_found = False
        for fv in fvs:
            if fv[0] == "loopback_action":
                action_found = True
                assert fv[1] == action
        assert action_found == True

        # check asic db
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        intf_entries = tbl.getKeys()

        action_map = {"drop": "SAI_PACKET_ACTION_DROP", "forward": "SAI_PACKET_ACTION_FORWARD"}
        action_found = False
        for key in intf_entries:
            (status, fvs) = tbl.get(key)
            assert status == True

            for fv in fvs:
                if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_LOOPBACK_PACKET_ACTION":
                    action_found = True
                    assert fv[1] == action_map[action]
        assert action_found == True

        # remove interface
        self.remove_l3_intf(iface)

    def test_interfaceLoopbackActionDrop(self, dvs, testlog):
        self.setup_db(dvs)
        self.loopback_action_test("Ethernet8", "drop")
        
    def test_interfaceLoopbackActionForward(self, dvs, testlog):
        self.setup_db(dvs)
        self.loopback_action_test("Ethernet8", "forward")

    def test_subInterfaceLoopbackActionDrop(self, dvs, testlog):
        self.setup_db(dvs)
        self.loopback_action_test("Ethernet8.1", "drop")
        
    def test_subInterfaceLoopbackActionForward(self, dvs, testlog):
        self.setup_db(dvs)
        self.loopback_action_test("Ethernet8.1", "forward")

    def test_vlanInterfaceLoopbackActionDrop(self, dvs, testlog):
        self.setup_db(dvs)
        self.create_vlan("10")
        self.loopback_action_test("Vlan10", "drop")
        self.remove_vlan("10")
        
    def test_vlanInterfaceLoopbackActionForward(self, dvs, testlog):
        self.setup_db(dvs)
        self.create_vlan("20")
        self.loopback_action_test("Vlan20", "forward")
        self.remove_vlan("20")

    def test_portChannelInterfaceLoopbackActionDrop(self, dvs, testlog):
        self.setup_db(dvs)
        self.create_port_channel("PortChannel009")
        self.loopback_action_test("PortChannel009", "drop")
        self.remove_port_channel("PortChannel009")
        
    def test_portChannelInterfaceLoopbackActionForward(self, dvs, testlog):
        self.setup_db(dvs)
        self.create_port_channel("PortChannel010")
        self.loopback_action_test("PortChannel010", "forward")
        self.remove_port_channel("PortChannel010")

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
