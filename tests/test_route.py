import os
import re
import time
import json
import pytest
import ipaddress

from swsscommon import swsscommon
from dvslib.dvs_common import wait_for_result

class TestRouteBase(object):
    def setup_db(self, dvs):
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()
        self.cdb = dvs.get_config_db()
        self.sdb = dvs.get_state_db()

    def set_admin_status(self, interface, status):
        self.cdb.update_entry("PORT", interface, {"admin_status": status})

    def create_vrf(self, vrf_name):
        initial_entries = set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))

        self.cdb.create_entry("VRF", vrf_name, {"empty": "empty"})
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER", len(initial_entries) + 1)

        current_entries = set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))
        assert len(current_entries - initial_entries) == 1
        return list(current_entries - initial_entries)[0]

    def remove_vrf(self, vrf_name):
        self.cdb.delete_entry("VRF", vrf_name)

    def create_l3_intf(self, interface, vrf_name):
        if len(vrf_name) == 0:
            self.cdb.create_entry("INTERFACE", interface, {"NULL": "NULL"})
        else:
            self.cdb.create_entry("INTERFACE", interface, {"vrf_name": vrf_name})

    def remove_l3_intf(self, interface):
        self.cdb.delete_entry("INTERFACE", interface)

    def add_ip_address(self, interface, ip):
        self.cdb.create_entry("INTERFACE", interface + "|" + ip, {"NULL": "NULL"})

    def remove_ip_address(self, interface, ip):
        self.cdb.delete_entry("INTERFACE", interface + "|" + ip)

    def create_route_entry(self, key, pairs):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "ROUTE_TABLE")
        fvs = swsscommon.FieldValuePairs(list(pairs.items()))
        tbl.set(key, fvs)

    def remove_route_entry(self, key):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "ROUTE_TABLE")
        tbl._del(key)

    def check_route_entries(self, destinations):
        def _access_function():
            route_entries = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            route_destinations = [json.loads(route_entry)["dest"]
                                  for route_entry in route_entries]
            return (all(destination in route_destinations for destination in destinations), None)

        wait_for_result(_access_function)

    def check_route_state(self, prefix, value):
        found = False
        fvs = {}
        
        for _ in range(5):  # Try for up to ~5 seconds
            route_entries = self.sdb.get_keys("ROUTE_TABLE")
            for key in route_entries:
                if key != prefix:
                    continue
                found = True
                fvs = self.sdb.get_entry("ROUTE_TABLE", key)
                if fvs.get("state") == value:
                    return
            time.sleep(1)

        assert found
        assert fvs.get("state") == value, f"Expected state '{value}', but got '{fvs.get('state')}'"

    def get_asic_db_key(self, destination):
        route_entries = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for route_entry in route_entries:
            if json.loads(route_entry)["dest"] == destination:
                return route_entry
        return None

    def check_route_entries_with_vrf(self, destinations, vrf_oids):
        def _access_function():
            route_entries = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            route_destination_vrf = [(json.loads(route_entry)["dest"], json.loads(route_entry)["vr"])
                                           for route_entry in route_entries]
            return (all((destination, vrf_oid) in route_destination_vrf
                        for destination, vrf_oid in zip(destinations, vrf_oids)), None)

        wait_for_result(_access_function)

    def check_route_entries_nexthop(self, destinations, vrf_oids, nexthops):
        def _access_function_nexthop():
            nexthop_entries = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
            nexthop_oids = dict([(self.adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", key)["SAI_NEXT_HOP_ATTR_IP"], key)
                                 for key in nexthop_entries])
            return (all(nexthop in nexthop_oids for nexthop in nexthops), nexthop_oids)

        status, nexthop_oids = wait_for_result(_access_function_nexthop)

        def _access_function_route_nexthop():
            route_entries = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            route_destination_nexthop = dict([((json.loads(route_entry)["dest"], json.loads(route_entry)["vr"]), 
                                               self.adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", route_entry).get("SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"))
                                               for route_entry in route_entries])
            return (all(route_destination_nexthop.get((destination, vrf_oid)) == nexthop_oids.get(nexthop)
                        for destination, vrf_oid, nexthop in zip(destinations, vrf_oids, nexthops)), None)

        wait_for_result(_access_function_route_nexthop)

    def check_deleted_route_entries(self, destinations):
        def _access_function():
            route_entries = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            route_destinations = [json.loads(route_entry)["dest"] for route_entry in route_entries]
            return (all(destination not in route_destinations for destination in destinations), None)

        wait_for_result(_access_function)

    def clear_srv_config(self, dvs):
        dvs.servers[0].runcmd("ip address flush dev eth0")
        dvs.servers[1].runcmd("ip address flush dev eth0")
        dvs.servers[2].runcmd("ip address flush dev eth0")
        dvs.servers[3].runcmd("ip address flush dev eth0")
        self.remove_route_entry("0.0.0.0/0")

class TestRoute(TestRouteBase):
    """ Functionality tests for route """
    @pytest.mark.skip(reason="Covered by mock test: RouteOrch_AddRemoveIPv4_And_DefaultRoute_State (GTest)")
    def test_RouteAddRemoveIpv4Route(self, dvs, testlog):
        self.setup_db(dvs)

        self.clear_srv_config(dvs)

        # create l3 interface
        self.create_l3_intf("Ethernet0", "")
        self.create_l3_intf("Ethernet4", "")

        # check STATE route database, initial state shall be "na"
        self.check_route_state("0.0.0.0/0", "na")

        # set ip address
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        self.add_ip_address("Ethernet4", "10.0.0.2/31")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        dvs.servers[1].runcmd("ip address add 10.0.0.3/31 dev eth0")
        dvs.servers[1].runcmd("ip route add default via 10.0.0.2")

        # get neighbor and arp entry
        dvs.servers[0].runcmd("ping -c 1 10.0.0.3")

        # add route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 2.2.2.0/24 10.0.0.1\"")

        # add default route entry
        fieldValues = {"nexthop": "10.0.0.1", "ifname": "Ethernet0"}
        self.create_route_entry("0.0.0.0/0", fieldValues)

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "2.2.2.0/24")

        # check ASIC route database
        self.check_route_entries(["2.2.2.0/24"])

        # check STATE route database
        self.check_route_state("0.0.0.0/0", "ok")

        # remove route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 2.2.2.0/24 10.0.0.1\"")

        # remove default route entry
        self.remove_route_entry("0.0.0.0/0")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "2.2.2.0/24")

        # check ASIC route database
        self.check_deleted_route_entries(["2.2.2.0/24"])

        # remove ip address
        self.remove_ip_address("Ethernet0", "10.0.0.0/31")
        self.remove_ip_address("Ethernet4", "10.0.0.2/31")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")
        self.remove_l3_intf("Ethernet4")

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")

        # check STATE route database, state set to "na" after deleting the default route
        self.check_route_state("0.0.0.0/0", "na")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip route del default dev eth0")
        dvs.servers[0].runcmd("ip address del 10.0.0.1/31 dev eth0")

        dvs.servers[1].runcmd("ip route del default dev eth0")
        dvs.servers[1].runcmd("ip address del 10.0.0.3/31 dev eth0")

    @pytest.mark.skip(reason="Covered by mock test: RouteOrch_AddRemoveIPv6_And_DefaultRoute_State (GTest)")
    def test_RouteAddRemoveIpv6Route(self, dvs, testlog):
        self.setup_db(dvs)

        # create l3 interface
        self.create_l3_intf("Ethernet0", "")
        self.create_l3_intf("Ethernet4", "")

        # check STATE route database, initial state shall be "na"
        self.check_route_state("::/0", "na")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")

        # set ip address
        self.add_ip_address("Ethernet0", "2000::1/64")
        self.add_ip_address("Ethernet4", "2001::1/64")
        dvs.runcmd("sysctl -w net.ipv6.conf.all.forwarding=1")

        # set ip address and default route
        dvs.servers[0].runcmd("ip -6 address add 2000::2/64 dev eth0")
        dvs.servers[0].runcmd("ip -6 route add default via 2000::1")

        dvs.servers[1].runcmd("ip -6 address add 2001::2/64 dev eth0")
        dvs.servers[1].runcmd("ip -6 route add default via 2001::1")
        time.sleep(2)

        # get neighbor entry
        dvs.servers[0].runcmd("ping -6 -c 1 2001::2")

        # add route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ipv6 route 3000::0/64 2000::2\"")

        # add default route entry
        fieldValues = {"nexthop": "2000::2", "ifname": "Ethernet0"}
        self.create_route_entry("::/0", fieldValues)

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "3000::/64")

        # check ASIC route database
        self.check_route_entries(["3000::/64"])

        # check STATE route database
        self.check_route_state("::/0", "ok")

        # remove route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ipv6 route 3000::0/64 2000::2\"")

        # remove default route entry
        self.remove_route_entry("::/0")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "3000::/64")

        # check ASIC route database
        self.check_deleted_route_entries(["3000::/64"])

        # remove ip address
        self.remove_ip_address("Ethernet0", "2000::1/64")
        self.remove_ip_address("Ethernet4", "2001::1/64")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")
        self.remove_l3_intf("Ethernet4")

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")

        # check STATE route database, state set to "na" after deleting the default route
        self.check_route_state("::/0", "na")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip -6 route del default dev eth0")
        dvs.servers[0].runcmd("ip -6 address del 2000::2/64 dev eth0")

        dvs.servers[1].runcmd("ip -6 route del default dev eth0")
        dvs.servers[1].runcmd("ip -6 address del 2001::2/64 dev eth0")

    def test_RouteAddRemoveIpv4RouteResolveNeigh(self, dvs, testlog):
        self.setup_db(dvs)

        self.clear_srv_config(dvs)

        # create l3 interface
        self.create_l3_intf("Ethernet0", "")
        self.create_l3_intf("Ethernet4", "")

        # set ip address
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        self.add_ip_address("Ethernet4", "10.0.0.2/31")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        dvs.servers[1].runcmd("ip address add 10.0.0.3/31 dev eth0")
        dvs.servers[1].runcmd("ip route add default via 10.0.0.2")
        time.sleep(2)

        # add route entry -- single nexthop
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 2.2.2.0/24 10.0.0.1\"")

        # add route entry -- multiple nexthop
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 3.3.3.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 3.3.3.0/24 10.0.0.3\"")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "2.2.2.0/24")
        self.pdb.wait_for_entry("ROUTE_TABLE", "3.3.3.0/24")

        # check neighbor got resolved and removed from NEIGH_RESOLVE_TABLE
        self.pdb.wait_for_deleted_entry("NEIGH_RESOLVE_TABLE", "Ethernet0:10.0.0.1")
        self.pdb.wait_for_deleted_entry("NEIGH_RESOLVE_TABLE", "Ethernet4:10.0.0.3")

        # check ASIC route database
        self.check_route_entries(["2.2.2.0/24", "3.3.3.0/24"])

        # remove route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 2.2.2.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 3.3.3.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 3.3.3.0/24 10.0.0.3\"")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "2.2.2.0/24")
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "3.3.3.0/24")

        # check ASIC route database
        self.check_deleted_route_entries(["2.2.2.0/24", "3.3.3.0/24"])

        # remove ip address
        self.remove_ip_address("Ethernet0", "10.0.0.0/31")
        self.remove_ip_address("Ethernet4", "10.0.0.2/31")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")
        self.remove_l3_intf("Ethernet4")

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip route del default dev eth0")
        dvs.servers[0].runcmd("ip address del 10.0.0.1/31 dev eth0")

        dvs.servers[1].runcmd("ip route del default dev eth0")
        dvs.servers[1].runcmd("ip address del 10.0.0.3/31 dev eth0")

    def test_RouteAddRemoveIpv6RouteResolveNeigh(self, dvs, testlog):
        self.setup_db(dvs)

        # create l3 interface
        self.create_l3_intf("Ethernet0", "")
        self.create_l3_intf("Ethernet4", "")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")

        # set ip address
        self.add_ip_address("Ethernet0", "2000::1/64")
        self.add_ip_address("Ethernet4", "2001::1/64")
        dvs.runcmd("sysctl -w net.ipv6.conf.all.forwarding=1")

        # set ip address and default route
        dvs.servers[0].runcmd("ip -6 address add 2000::2/64 dev eth0")
        dvs.servers[0].runcmd("ip -6 route add default via 2000::1")

        dvs.servers[1].runcmd("ip -6 address add 2001::2/64 dev eth0")
        dvs.servers[1].runcmd("ip -6 route add default via 2001::1")
        time.sleep(2)

        # add route entry -- single nexthop
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ipv6 route 3000::0/64 2000::2\"")

        # add route entry -- multiple nexthop
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ipv6 route 4000::0/64 2000::2\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ipv6 route 4000::0/64 2001::2\"")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "3000::/64")
        self.pdb.wait_for_entry("ROUTE_TABLE", "4000::/64")

        # check neighbor got resolved and removed from NEIGH_RESOLVE_TABLE
        self.pdb.wait_for_deleted_entry("NEIGH_RESOLVE_TABLE", "Ethernet0:2000::2")
        self.pdb.wait_for_deleted_entry("NEIGH_RESOLVE_TABLE", "Ethernet4:2001::2")

        # check ASIC route database
        self.check_route_entries(["3000::/64", "4000::/64"])

        # remove route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ipv6 route 3000::0/64 2000::2\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ipv6 route 4000::0/64 2000::2\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ipv6 route 4000::0/64 2001::2\"")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "3000::/64")
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "4000::/64")

        # check ASIC route database
        self.check_deleted_route_entries(["3000::/64", "4000::/64"])

        # remove ip address
        self.remove_ip_address("Ethernet0", "2000::1/64")
        self.remove_ip_address("Ethernet4", "2001::1/64")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")
        self.remove_l3_intf("Ethernet4")

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip -6 route del default dev eth0")
        dvs.servers[0].runcmd("ip -6 address del 2000::2/64 dev eth0")

        dvs.servers[1].runcmd("ip -6 route del default dev eth0")
        dvs.servers[1].runcmd("ip -6 address del 2001::2/64 dev eth0")

    def test_RouteAddRemoveIpv4RouteUnresolvedNeigh(self, dvs, testlog):
        self.setup_db(dvs)

        self.clear_srv_config(dvs)

        # create l3 interface
        self.create_l3_intf("Ethernet0", "")
        self.create_l3_intf("Ethernet4", "")

        # set ip address
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        self.add_ip_address("Ethernet4", "10.0.0.2/31")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")

        # add route entry -- single nexthop
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 2.2.2.0/24 10.0.0.1\"")

        # add route entry -- multiple nexthop
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 3.3.3.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 3.3.3.0/24 10.0.0.3\"")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "2.2.2.0/24")
        self.pdb.wait_for_entry("ROUTE_TABLE", "3.3.3.0/24")

        # check for unresolved neighbor entries
        self.pdb.wait_for_entry("NEIGH_RESOLVE_TABLE", "Ethernet0:10.0.0.1")
        self.pdb.wait_for_entry("NEIGH_RESOLVE_TABLE", "Ethernet4:10.0.0.3")

        # check routes does not show up in ASIC_DB
        self.check_deleted_route_entries(["2.2.2.0/24", "3.3.3.0/24"])

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        dvs.servers[1].runcmd("ip address add 10.0.0.3/31 dev eth0")
        dvs.servers[1].runcmd("ip route add default via 10.0.0.2")
        time.sleep(2)

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "2.2.2.0/24")
        self.pdb.wait_for_entry("ROUTE_TABLE", "3.3.3.0/24")

        # check neighbor got resolved and removed from NEIGH_RESOLVE_TABLE
        self.pdb.wait_for_deleted_entry("NEIGH_RESOLVE_TABLE", "Ethernet0:10.0.0.1")
        self.pdb.wait_for_deleted_entry("NEIGH_RESOLVE_TABLE", "Ethernet4:10.0.0.3")

        # check ASIC route database
        self.check_route_entries(["2.2.2.0/24", "3.3.3.0/24"])

        # remove route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 2.2.2.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 3.3.3.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 3.3.3.0/24 10.0.0.3\"")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "2.2.2.0/24")
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "3.3.3.0/24")

        # check ASIC route database
        self.check_deleted_route_entries(["2.2.2.0/24", "3.3.3.0/24"])

        # remove ip address
        self.remove_ip_address("Ethernet0", "10.0.0.0/31")
        self.remove_ip_address("Ethernet4", "10.0.0.2/31")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")
        self.remove_l3_intf("Ethernet4")

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip route del default dev eth0")
        dvs.servers[0].runcmd("ip address del 10.0.0.1/31 dev eth0")

        dvs.servers[1].runcmd("ip route del default dev eth0")
        dvs.servers[1].runcmd("ip address del 10.0.0.3/31 dev eth0")

    def test_RouteAddRemoveIpv6RouteUnresolvedNeigh(self, dvs, testlog):
        self.setup_db(dvs)

        # create l3 interface
        self.create_l3_intf("Ethernet0", "")
        self.create_l3_intf("Ethernet4", "")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")

        # set ip address
        self.add_ip_address("Ethernet0", "2000::1/64")
        self.add_ip_address("Ethernet4", "2001::1/64")
        dvs.runcmd("sysctl -w net.ipv6.conf.all.forwarding=1")

        # add route entry -- single nexthop
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ipv6 route 3000::0/64 2000::2\"")

        # add route entry -- multiple nexthop
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ipv6 route 4000::0/64 2000::2\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ipv6 route 4000::0/64 2001::2\"")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "3000::/64")
        self.pdb.wait_for_entry("ROUTE_TABLE", "4000::/64")

        # check for unresolved neighbor entries
        self.pdb.wait_for_entry("NEIGH_RESOLVE_TABLE", "Ethernet0:2000::2")
        self.pdb.wait_for_entry("NEIGH_RESOLVE_TABLE", "Ethernet4:2001::2")

        # check routes does not show up in ASIC_DB
        self.check_deleted_route_entries(["3000::/64", "4000::/64"])

        # set ip address and default route
        dvs.servers[0].runcmd("ip -6 address add 2000::2/64 dev eth0")
        dvs.servers[0].runcmd("ip -6 route add default via 2000::1")

        dvs.servers[1].runcmd("ip -6 address add 2001::2/64 dev eth0")
        dvs.servers[1].runcmd("ip -6 route add default via 2001::1")
        time.sleep(5)

        dvs.servers[0].runcmd("ping -6 -c 1 2001::2")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "3000::/64")
        self.pdb.wait_for_entry("ROUTE_TABLE", "4000::/64")

        # check neighbor got resolved and removed from NEIGH_RESOLVE_TABLE
        self.pdb.wait_for_deleted_entry("NEIGH_RESOLVE_TABLE", "Ethernet0:2000::2")
        self.pdb.wait_for_deleted_entry("NEIGH_RESOLVE_TABLE", "Ethernet4:2001::2")

        # check ASIC route database
        self.check_route_entries(["3000::/64", "4000::/64"])

        # remove route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ipv6 route 3000::0/64 2000::2\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ipv6 route 4000::0/64 2000::2\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ipv6 route 4000::0/64 2001::2\"")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "3000::/64")
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "4000::/64")

        # check ASIC route database
        self.check_deleted_route_entries(["3000::/64", "4000::/64"])

        # remove ip address
        self.remove_ip_address("Ethernet0", "2000::1/64")
        self.remove_ip_address("Ethernet4", "2001::1/64")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")
        self.remove_l3_intf("Ethernet4")

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip -6 route del default dev eth0")
        dvs.servers[0].runcmd("ip -6 address del 2000::2/64 dev eth0")

        dvs.servers[1].runcmd("ip -6 route del default dev eth0")
        dvs.servers[1].runcmd("ip -6 address del 2001::2/64 dev eth0")

    @pytest.mark.skip(reason="Failing. Under investigation")
    def test_RouteAddRemoveIpv4RouteWithVrf(self, dvs, testlog):
        self.setup_db(dvs)

        # create vrf
        vrf_1_oid = self.create_vrf("Vrf_1")
        vrf_2_oid = self.create_vrf("Vrf_2")

        # create l3 interface
        self.create_l3_intf("Ethernet0", "Vrf_1")
        self.create_l3_intf("Ethernet4", "Vrf_1")
        self.create_l3_intf("Ethernet8", "Vrf_2")
        self.create_l3_intf("Ethernet12", "Vrf_2")

        # set ip address
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        self.add_ip_address("Ethernet4", "10.0.0.2/31")
        self.add_ip_address("Ethernet8", "10.0.0.0/31")
        self.add_ip_address("Ethernet12", "10.0.0.2/31")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")
        self.set_admin_status("Ethernet8", "up")
        self.set_admin_status("Ethernet12", "up")

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        dvs.servers[1].runcmd("ip address add 10.0.0.3/31 dev eth0")
        dvs.servers[1].runcmd("ip route add default via 10.0.0.2")

        dvs.servers[2].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[2].runcmd("ip route add default via 10.0.0.0")

        dvs.servers[3].runcmd("ip address add 10.0.0.3/31 dev eth0")
        dvs.servers[3].runcmd("ip route add default via 10.0.0.2")

        # get neighbor entry
        dvs.servers[0].runcmd("ping -c 1 10.0.0.3")
        dvs.servers[2].runcmd("ping -c 1 10.0.0.3")

        # add route
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 2.2.2.0/24 10.0.0.1 vrf Vrf_1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 3.3.3.0/24 10.0.0.1 vrf Vrf_2\"")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE:Vrf_1", "2.2.2.0/24")
        self.pdb.wait_for_entry("ROUTE_TABLE:Vrf_2", "3.3.3.0/24")

        # check ASIC route database
        self.check_route_entries_with_vrf(["2.2.2.0/24", "3.3.3.0/24"], [vrf_1_oid, vrf_2_oid])

        # remove route
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 2.2.2.0/24 10.0.0.1 vrf Vrf_1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 3.3.3.0/24 10.0.0.1 vrf Vrf_2\"")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE:Vrf_1", "2.2.2.0/24")
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE:Vrf_2", "3.3.3.0/24")

        # check ASIC route database
        self.check_deleted_route_entries(["2.2.2.0/24", "3.3.3.0/24"])

        # remove ip address
        self.remove_ip_address("Ethernet0", "10.0.0.0/31")
        self.remove_ip_address("Ethernet4", "10.0.0.2/31")
        self.remove_ip_address("Ethernet8", "10.0.0.0/31")
        self.remove_ip_address("Ethernet12", "10.0.0.2/31")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")
        self.remove_l3_intf("Ethernet4")
        self.remove_l3_intf("Ethernet8")
        self.remove_l3_intf("Ethernet12")

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")
        self.set_admin_status("Ethernet8", "down")
        self.set_admin_status("Ethernet12", "down")

        # remove vrf
        self.remove_vrf("Vrf_1")
        self.remove_vrf("Vrf_2")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip route del default dev eth0")
        dvs.servers[0].runcmd("ip address del 10.0.0.1/31 dev eth0")
        dvs.servers[1].runcmd("ip route del default dev eth0")
        dvs.servers[1].runcmd("ip address del 10.0.0.3/31 dev eth0")
        dvs.servers[2].runcmd("ip route del default dev eth0")
        dvs.servers[2].runcmd("ip address del 10.0.0.1/31 dev eth0")
        dvs.servers[3].runcmd("ip route del default dev eth0")
        dvs.servers[3].runcmd("ip address del 10.0.0.3/31 dev eth0")

    @pytest.mark.skip(reason="FRR 7.5 issue https://github.com/Azure/sonic-buildimage/issues/6359")
    def test_RouteAddRemoveIpv6RouteWithVrf(self, dvs, testlog):
        self.setup_db(dvs)

        # create vrf
        vrf_1_oid = self.create_vrf("Vrf_1")
        vrf_2_oid = self.create_vrf("Vrf_2")

        # create l3 interface
        self.create_l3_intf("Ethernet0", "Vrf_1")
        self.create_l3_intf("Ethernet4", "Vrf_1")
        self.create_l3_intf("Ethernet8", "Vrf_2")
        self.create_l3_intf("Ethernet12", "Vrf_2")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")
        self.set_admin_status("Ethernet8", "up")
        self.set_admin_status("Ethernet12", "up")

        # set ip address
        self.add_ip_address("Ethernet0", "2000::1/64")
        self.add_ip_address("Ethernet4", "2001::1/64")
        self.add_ip_address("Ethernet8", "2000::1/64")
        self.add_ip_address("Ethernet12", "2001::1/64")

        dvs.runcmd("sysctl -w net.ipv6.conf.all.forwarding=1")

        # set ip address and default route
        dvs.servers[0].runcmd("ip -6 address add 2000::2/64 dev eth0")
        dvs.servers[0].runcmd("ip -6 route add default via 2000::1")
        dvs.servers[1].runcmd("ip -6 address add 2001::2/64 dev eth0")
        dvs.servers[1].runcmd("ip -6 route add default via 2001::1")
        dvs.servers[2].runcmd("ip -6 address add 2000::2/64 dev eth0")
        dvs.servers[2].runcmd("ip -6 route add default via 2000::1")
        dvs.servers[3].runcmd("ip -6 address add 2001::2/64 dev eth0")
        dvs.servers[3].runcmd("ip -6 route add default via 2001::1")
        time.sleep(2)

        # get neighbor entry
        dvs.servers[0].runcmd("ping -6 -c 1 2001::2")
        dvs.servers[2].runcmd("ping -6 -c 1 2001::2")

        # add route
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ipv6 route 3000::0/64 2000::2 vrf Vrf_1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ipv6 route 4000::0/64 2000::2 vrf Vrf_2\"")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE:Vrf_1", "3000::/64")
        self.pdb.wait_for_entry("ROUTE_TABLE:Vrf_2", "4000::/64")

        # check ASIC route database
        self.check_route_entries_with_vrf(["3000::/64", "4000::/64"], [vrf_1_oid, vrf_2_oid])

        # remove route
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ipv6 route 3000::0/64 2000::2 vrf Vrf_1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ipv6 route 4000::0/64 2000::2 vrf Vrf_2\"")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE:Vrf_1", "3000::/64")
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE:Vrf_2", "4000::/64")

        # check ASIC route database
        self.check_deleted_route_entries(["3000::/64", "4000::/64"])

        # remove ip address
        self.remove_ip_address("Ethernet0", "2000::1/64")
        self.remove_ip_address("Ethernet4", "2001::1/64")
        self.remove_ip_address("Ethernet8", "2000::1/64")
        self.remove_ip_address("Ethernet12", "2001::1/64")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")
        self.remove_l3_intf("Ethernet4")
        self.remove_l3_intf("Ethernet8")
        self.remove_l3_intf("Ethernet12")

        # bring down interface
        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")
        self.set_admin_status("Ethernet8", "down")
        self.set_admin_status("Ethernet12", "down")

        # remove vrf
        self.remove_vrf("Vrf_1")
        self.remove_vrf("Vrf_2")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip -6 route del default dev eth0")
        dvs.servers[0].runcmd("ip -6 address del 2000::2/64 dev eth0")
        dvs.servers[1].runcmd("ip -6 route del default dev eth0")
        dvs.servers[1].runcmd("ip -6 address del 2001::2/64 dev eth0")
        dvs.servers[2].runcmd("ip -6 route del default dev eth0")
        dvs.servers[2].runcmd("ip -6 address del 2000::2/64 dev eth0")
        dvs.servers[3].runcmd("ip -6 route del default dev eth0")
        dvs.servers[3].runcmd("ip -6 address del 2001::2/64 dev eth0")

    @pytest.mark.skip(reason="FRR 7.5 issue https://github.com/Azure/sonic-buildimage/issues/6359")
    def test_RouteAndNexthopInDifferentVrf(self, dvs, testlog):
        self.setup_db(dvs)

        # create vrf
        vrf_1_oid = self.create_vrf("Vrf_1")
        vrf_2_oid = self.create_vrf("Vrf_2")

        # create l3 interface
        self.create_l3_intf("Ethernet0", "Vrf_1")
        self.create_l3_intf("Ethernet4", "Vrf_1")
        self.create_l3_intf("Ethernet8", "Vrf_2")
        self.create_l3_intf("Ethernet12", "Vrf_2")

        # set ip address
        self.add_ip_address("Ethernet0", "10.0.0.1/24")
        self.add_ip_address("Ethernet4", "10.0.1.1/24")
        self.add_ip_address("Ethernet8", "20.0.0.1/24")
        self.add_ip_address("Ethernet12", "20.0.1.1/24")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")
        self.set_admin_status("Ethernet8", "up")
        self.set_admin_status("Ethernet12", "up")

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 10.0.0.2/24 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.1")

        dvs.servers[1].runcmd("ip address add 10.0.1.2/24 dev eth0")
        dvs.servers[1].runcmd("ip route add default via 10.0.1.1")

        dvs.servers[2].runcmd("ip address add 20.0.0.2/24 dev eth0")
        dvs.servers[2].runcmd("ip route add default via 20.0.0.1")

        dvs.servers[3].runcmd("ip address add 20.0.1.2/24 dev eth0")
        dvs.servers[3].runcmd("ip route add default via 20.0.1.1")

        # get neighbor entry
        dvs.servers[0].runcmd("ping -c 1 10.0.1.2")
        dvs.servers[2].runcmd("ping -c 1 20.0.1.2")

        # add route
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 20.0.1.2/32 20.0.1.2 vrf Vrf_1 nexthop-vrf Vrf_2\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 10.0.0.2/32 10.0.0.2 vrf Vrf_2 nexthop-vrf Vrf_1\"")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE:Vrf_1", "20.0.1.2")
        self.pdb.wait_for_entry("ROUTE_TABLE:Vrf_2", "10.0.0.2")

        # check ASIC neighbor interface database
        self.check_route_entries_nexthop(["10.0.0.2/32", "20.0.1.2/32"], [vrf_2_oid, vrf_1_oid], ["10.0.0.2", "20.0.1.2"])

        # Ping should work
        ping_stats = dvs.servers[0].runcmd("ping -c 1 20.0.1.2")
        assert ping_stats == 0

        # remove route
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 20.0.1.2/32 20.0.1.2 vrf Vrf_1 nexthop-vrf Vrf_2\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 10.0.0.2/32 10.0.0.2 vrf Vrf_2 nexthop-vrf Vrf_1\"")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE:Vrf_1", "20.0.1.2")
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE:Vrf_2", "10.0.0.2")

        # check ASIC route database
        self.check_deleted_route_entries(["10.0.0.2/32", "20.0.1.2/32"])

        # remove ip address
        self.remove_ip_address("Ethernet0", "10.0.0.1/24")
        self.remove_ip_address("Ethernet4", "10.0.1.1/24")
        self.remove_ip_address("Ethernet8", "20.0.0.1/24")
        self.remove_ip_address("Ethernet12", "20.0.1.1/24")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")
        self.remove_l3_intf("Ethernet4")
        self.remove_l3_intf("Ethernet8")
        self.remove_l3_intf("Ethernet12")

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")
        self.set_admin_status("Ethernet8", "down")
        self.set_admin_status("Ethernet12", "down")

        # remove vrf
        self.remove_vrf("Vrf_1")
        self.remove_vrf("Vrf_2")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip route del default dev eth0")
        dvs.servers[0].runcmd("ip address del 10.0.0.2/24 dev eth0")
        dvs.servers[1].runcmd("ip route del default dev eth0")
        dvs.servers[1].runcmd("ip address del 10.0.1.2/24 dev eth0")
        dvs.servers[2].runcmd("ip route del default dev eth0")
        dvs.servers[2].runcmd("ip address del 20.0.0.2/24 dev eth0")
        dvs.servers[3].runcmd("ip route del default dev eth0")
        dvs.servers[3].runcmd("ip address del 20.0.1.2/24 dev eth0")

    def test_RouteAddRemoveIpv4BlackholeRoute(self, dvs, testlog):
        self.setup_db(dvs)

        self.clear_srv_config(dvs)

        # add route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 2.2.2.0/24 blackhole\"")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "2.2.2.0/24")

        # check ASIC route database
        self.check_route_entries(["2.2.2.0/24"])
        key = self.get_asic_db_key("2.2.2.0/24")
        assert key
        fvs = self.adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", key)
        assert fvs.get("SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION") == "SAI_PACKET_ACTION_DROP"

        # remove route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 2.2.2.0/24 blackhole\"")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "2.2.2.0/24")

        # check ASIC route database
        self.check_deleted_route_entries(["2.2.2.0/24"])

    def test_RouteAddRemoveIpv6BlackholeRoute(self, dvs, testlog):
        self.setup_db(dvs)

        self.clear_srv_config(dvs)

        # add route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ipv6 route 3000::0/64 blackhole\"")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "3000::/64")

        # check ASIC route database
        self.check_route_entries(["3000::/64"])
        key = self.get_asic_db_key("3000::/64")
        assert key
        fvs = self.adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", key)
        assert fvs.get("SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION") == "SAI_PACKET_ACTION_DROP"

        # remove route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ipv6 route 3000::0/64 blackhole\"")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "3000::/64")

        # check ASIC route database
        self.check_deleted_route_entries(["3000::/64"])

class TestRoutePerf(TestRouteBase):
    """ Performance tests for route """
    def test_PerfAddRemoveRoute(self, dvs, testlog):
        self.setup_db(dvs)
        self.clear_srv_config(dvs)
        numRoutes = 5000   # number of routes to add/remove

        # generate ip prefixes of routes
        prefixes = []
        for i in range(numRoutes):
            prefixes.append("%d.%d.%d.%d/%d" % (100 + int(i / 256 ** 2), int(i / 256), i % 256, 0, 24))

        # create l3 interface
        self.create_l3_intf("Ethernet0", "")
        self.create_l3_intf("Ethernet4", "")

        # set ip address
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        self.add_ip_address("Ethernet4", "10.0.0.2/31")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        dvs.servers[1].runcmd("ip address add 10.0.0.3/31 dev eth0")
        dvs.servers[1].runcmd("ip route add default via 10.0.0.2")
        time.sleep(2)

        fieldValues = [{"nexthop": "10.0.0.1", "ifname": "Ethernet0"}, {"nexthop": "10.0.0.3", "ifname": "Ethernet4"}]

        # get neighbor and arp entry
        dvs.servers[0].runcmd("ping -c 1 10.0.0.3")
        dvs.servers[1].runcmd("ping -c 1 10.0.0.1")
        time.sleep(2)

        # get number of routes before adding new routes
        startNumRoutes = len(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"))

        # add route entries
        timeStart = time.time()
        for i in range(numRoutes):
            self.create_route_entry(prefixes[i], fieldValues[i % 2])

        # wait until all routes are added into ASIC database
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", startNumRoutes + numRoutes) # default timeout is 5 seconds
        print("Time to add %d routes is %.2f seconds. " % (numRoutes, time.time() - timeStart))

        # confirm all routes are added
        self.check_route_entries(prefixes)

        # remove route entries
        timeStart = time.time()
        for i in range(numRoutes):
            self.remove_route_entry(prefixes[i])

        # wait until all routes are removed from ASIC database
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", startNumRoutes) # default timeout is 5 seconds
        print("Time to remove %d routes is %.2f seconds. " % (numRoutes, time.time() - timeStart))

        # confirm all routes are removed
        self.check_deleted_route_entries(prefixes)

        # remove ip address
        self.remove_ip_address("Ethernet0", "10.0.0.0/31")
        self.remove_ip_address("Ethernet4", "10.0.0.2/31")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")
        self.remove_l3_intf("Ethernet4")

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip route del default dev eth0")
        dvs.servers[0].runcmd("ip address del 10.0.0.1/31 dev eth0")

        dvs.servers[1].runcmd("ip route del default dev eth0")
        dvs.servers[1].runcmd("ip address del 10.0.0.3/31 dev eth0")

class TestFpmSyncResponse(TestRouteBase):
    @pytest.fixture
    def setup(self, dvs):
        self.setup_db(dvs)

        # create l3 interface
        self.create_l3_intf("Ethernet0", "")
        # set ip address
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        # bring up interface
        self.set_admin_status("Ethernet0", "up")

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        dvs.runcmd("ping -c 1 10.0.0.1")

        yield

        # remove ip address and default route
        dvs.servers[0].runcmd("ip route del default dev eth0")
        dvs.servers[0].runcmd("ip address del 10.0.0.1/31 dev eth0")

        # bring interface down
        self.set_admin_status("Ethernet0", "down")
        # remove ip address
        self.remove_ip_address("Ethernet0", "10.0.0.0/31")
        # remove l3 interface
        self.remove_l3_intf("Ethernet0")

    def is_offloaded(self, dvs, route):
        rc, output = dvs.runcmd(f"vtysh -c 'show ip route {route} json'")
        assert rc == 0

        route_entry = json.loads(output)
        return bool(route_entry[route][0].get('offloaded'))

    @pytest.mark.xfail(reason="BGP suppress FIB disabled on master/202405 - https://github.com/sonic-net/sonic-buildimage/issues/19092")
    @pytest.mark.parametrize("suppress_state", ["enabled", "disabled"])
    def test_offload(self, suppress_state, setup, dvs):
        route = "1.1.1.0/24"

        # enable route suppression
        rc, _ = dvs.runcmd(f"config suppress-fib-pending {suppress_state}")
        assert rc == 0, "Failed to configure suppress-fib-pending"

        time.sleep(5)

        try:
            rc, _ = dvs.runcmd("bash -c 'kill -SIGSTOP $(pidof orchagent)'")
            assert rc == 0, "Failed to suspend orchagent"

            rc, _ = dvs.runcmd(f"ip route add {route} via 10.0.0.1 proto bgp")
            assert rc == 0, "Failed to configure route"

            time.sleep(5)

            if suppress_state == 'disabled':
                assert self.is_offloaded(dvs,route), f"{route} is expected to be offloaded (suppression is {suppress_state})"
                return

            assert not self.is_offloaded(dvs, route), f"{route} is expected to be not offloaded (suppression is {suppress_state})"

            rc, _ = dvs.runcmd("bash -c 'kill -SIGCONT $(pidof orchagent)'")
            assert rc == 0, "Failed to resume orchagent"

            def check_offloaded():
                return (self.is_offloaded(dvs, route), None)

            wait_for_result(check_offloaded, failure_message=f"{route} is expected to be offloaded after orchagent resume")
        finally:
            dvs.runcmd("bash -c 'kill -SIGCONT $(pidof orchagent)'")
            dvs.runcmd(f"ip route del {route}")

            # make sure route suppression is disabled
            dvs.runcmd("config suppress-fib-pending disabled")


class TestSubnetDecapVipRoute(TestRouteBase):
    VLAN_ID = "1000"
    VLAN_INTF = "Vlan1000"
    SERV_IPV4 = "192.168.0.100"
    SERV_IPV6 = "fc02:1000::100"
    CFG_SUBNET_DECAP_TABLE_NAME = "SUBNET_DECAP"
    APP_TUNNEL_DECAP_TERM_TABLE_NAME = "TUNNEL_DECAP_TERM_TABLE"

    def add_neighbor(self, dvs, ip, mac):
        if ipaddress.ip_address(ip).version == 6:
            dvs.runcmd("ip -6 neigh replace " + ip + " lladdr " + mac + " dev " + self.VLAN_INTF)
        else:
            dvs.runcmd("ip -4 neigh replace " + ip + " lladdr " + mac + " dev " + self.VLAN_INTF)

    def add_route(self, dvs, ip_prefix):
        if ipaddress.ip_network(ip_prefix).version == 4:
            dvs.runcmd(
                'vtysh -c "configure terminal" -c "ip route %s %s"' % (ip_prefix, self.SERV_IPV4)
            )
        else:
            dvs.runcmd(
                'vtysh -c "configure terminal" -c "ipv6 route %s %s"' % (ip_prefix, self.SERV_IPV6)
            )

    def remove_route(self, dvs, ip_prefix):
        if ipaddress.ip_network(ip_prefix).version == 4:
            dvs.runcmd(
                'vtysh -c "configure terminal" -c "no ip route %s %s"' % (ip_prefix, self.SERV_IPV4)
            )
        else:
            dvs.runcmd(
                'vtysh -c "configure terminal" -c "no ipv6 route %s %s"' % (ip_prefix, self.SERV_IPV6)
            )

    def validate_subnet_decap_term(self, dvs, ip_prefix_list):
        tunnel_decap_term_app_table = swsscommon.Table(dvs.pdb, self.APP_TUNNEL_DECAP_TERM_TABLE_NAME)
        decap_term_list = tunnel_decap_term_app_table.getKeys()
        decap_term_prefix_list = [decap_term.split(":", 1)[1] for decap_term in decap_term_list]

        assert len(ip_prefix_list) == len(decap_term_prefix_list)
        for ip_prefix in ip_prefix_list:
            assert ip_prefix in decap_term_prefix_list

        for decap_term in decap_term_list:
            _, fvs = tunnel_decap_term_app_table.get(decap_term)
            decap_term_attrs = dict(fvs)
            assert decap_term_attrs["term_type"] == "MP2MP"
            assert decap_term_attrs["subnet_type"] == "vip"

    @pytest.fixture(scope="class", autouse=True)
    def setup_vlan(self, dvs, dvs_vlan_manager, setup_subnet_decap):
        dvs.setup_db()

        subnet_decap_config = {
            "status": "enable",
            "src_ip": "10.10.10.0/24",
            "src_ip_v6": "20c1:ba8::/64"
        }
        setup_subnet_decap(subnet_decap_config)

        vlan = self.VLAN_ID
        vlan_intf = self.VLAN_INTF
        self.dvs_vlan.create_vlan(vlan)
        vlan_oid = self.dvs_vlan.get_and_verify_vlan_ids(1)[0]
        self.dvs_vlan.verify_vlan(vlan_oid, vlan)

        dvs.port_admin_set("Ethernet0", "up")
        self.dvs_vlan.create_vlan_member(vlan, "Ethernet0")
        self.dvs_vlan.verify_vlan_member(vlan_oid, "Ethernet0")

        dvs.add_ip_address(vlan_intf, "192.168.0.1/24")
        dvs.add_ip_address(vlan_intf, "fc02:1000::1/64")

        yield

        dvs.remove_ip_address(vlan_intf, "192.168.0.1/24")
        dvs.remove_ip_address(vlan_intf, "fc02:1000::1/64")

        self.dvs_vlan.remove_vlan_member(vlan, "Ethernet0")
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        time.sleep(2)
        self.dvs_vlan.remove_vlan(vlan)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

    @pytest.fixture(scope="class", autouse=True)
    def setup_server_neighbor(self, dvs, setup_vlan):
        self.add_neighbor(dvs, self.SERV_IPV4, "00:00:00:00:00:01")
        self.add_neighbor(dvs, self.SERV_IPV6, "00:00:00:00:00:01")

    @pytest.fixture(scope="class")
    def setup_subnet_decap(self, dvs):

        def _apply_subnet_decap_config(subnet_decap_config):
            """Apply subnet decap config to CONFIG_DB."""
            fvs = swsscommon.FieldValuePairs(list(subnet_decap_config.items()))
            subnet_decap_tbl.set("AZURE", fvs)

        def _cleanup_subnet_decap_config():
            """Cleanup subnet decap config in CONFIG_DB."""
            for key in subnet_decap_tbl.getKeys():
                subnet_decap_tbl._del(key)

        configdb = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        subnet_decap_tbl = swsscommon.Table(configdb, self.CFG_SUBNET_DECAP_TABLE_NAME)
        _cleanup_subnet_decap_config()

        yield _apply_subnet_decap_config

        _cleanup_subnet_decap_config()

    def test_vip_route(self, dvs, setup_subnet_decap):
        self.add_route(dvs, "10.10.20.0/24")
        self.add_route(dvs, "2001:506:28:9d::/64")

        time.sleep(2)
        self.validate_subnet_decap_term(dvs, ["10.10.20.0/24", "2001:506:28:9d::/64"])

        self.remove_route(dvs, "10.10.20.0/24")
        self.remove_route(dvs, "2001:506:28:9d::/64")

        time.sleep(2)
        self.validate_subnet_decap_term(dvs, [])

        subnet_decap_config = {
            "status": "disable",
            "src_ip": "10.10.10.0/24",
            "src_ip_v6": "20c1:ba8::/64"
        }
        setup_subnet_decap(subnet_decap_config)

        self.add_route(dvs, "10.10.20.0/24")
        self.add_route(dvs, "2001:506:28:9d::/64")

        time.sleep(2)
        self.validate_subnet_decap_term(dvs, [])

        self.remove_route(dvs, "10.10.20.0/24")
        self.remove_route(dvs, "2001:506:28:9d::/64")

        time.sleep(2)
        self.validate_subnet_decap_term(dvs, [])

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
