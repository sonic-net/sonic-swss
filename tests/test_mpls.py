import os
import re
import time
import json
import pytest

from swsscommon import swsscommon
from dvslib.dvs_common import wait_for_result

class TestMplsBase(object):
    def mpls_appdb_mode(self):
        # run in APP_DB mode unless using "fpmsyncd -l net"
        return True

    def setup_db(self, dvs):
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()
        self.cdb = dvs.get_config_db()

    def set_admin_status(self, interface, status):
        self.cdb.update_entry("PORT", interface, {"admin_status": status})

    def create_l3_intf(self, interface):
        self.cdb.create_entry("INTERFACE", interface, {"NULL": "NULL"})

    def remove_l3_intf(self, interface):
        self.cdb.delete_entry("INTERFACE", interface)

    def create_mpls_intf(self, interface):
        self.cdb.create_entry("INTERFACE", interface, {"mpls": "enable"})

    def remove_mpls_intf(self, interface):
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

    def check_route_entries(self, present, destinations):
        def _access_function():
            route_entries = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            route_destinations = [json.loads(route_entry)["dest"]
                                  for route_entry in route_entries]
            if present:
                return (all(destination in route_destinations for destination in destinations), None)
            else:
                return (all(destination not in route_destinations for destination in destinations), None)

        wait_for_result(_access_function)

    def get_route_nexthop(self, prefix):
        route_entries = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for route_entry in route_entries:
            if json.loads(route_entry)["dest"] == prefix:
                return self.adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", route_entry).get("SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID")
        return None

    def check_route_nexthop(self, prefix, nhtype, ip, ostype, labels):
        def _access_function():
            nh = self.get_route_nexthop(prefix)
            if not bool(nh): return (False, None)
            fvs = self.adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nh)
            # NH should be in NH Table
            if not bool(fvs): return (False, None)
            if fvs.get("SAI_NEXT_HOP_ATTR_TYPE") != nhtype: return (False, None)
            if fvs.get("SAI_NEXT_HOP_ATTR_IP") != ip: return (False, None)
            if nhtype != "SAI_NEXT_HOP_TYPE_MPLS": return (True, None)
            if fvs.get("SAI_NEXT_HOP_ATTR_OUTSEG_TYPE") != ostype: return (False, None)
            if fvs.get("SAI_NEXT_HOP_ATTR_LABELSTACK") != labels: return (False, None)
            return (True, None)

        wait_for_result(_access_function)

    def check_route_nexthop_group(self, prefix, count):
        def _access_function():
            nhg = self.get_route_nexthop(prefix)
            if not bool(nhg): return (False, None)
            fvs = self.adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP", nhg)
            # NH should be in NHG Table
            if not bool(fvs): return (False, None)
            keys = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
            matched_nhgms = []
            for key in keys:
                fvs = self.adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER", key)
                if fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhg:
                    matched_nhgms.append(key)

            if len(matched_nhgms) != count: return (False, None)
            return (True, None)

        wait_for_result(_access_function)

    def create_inseg_entry(self, key, pairs):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "LABEL_ROUTE_TABLE")
        fvs = swsscommon.FieldValuePairs(list(pairs.items()))
        tbl.set(key, fvs)

    def remove_inseg_entry(self, key):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "LABEL_ROUTE_TABLE")
        tbl._del(key)

    def check_inseg_entries(self, present, labels):
        def _access_function():
            inseg_entries = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_INSEG_ENTRY")
            inseg_labels = [json.loads(inseg_entry)["label"]
                                  for inseg_entry in inseg_entries]
            if present:
                return (all(label in inseg_labels for label in labels), None)
            else:
                return (all(label not in inseg_labels for label in labels), None)

        wait_for_result(_access_function)

    def get_inseg_nexthop(self, label):
        inseg_entries = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_INSEG_ENTRY")
        for inseg_entry in inseg_entries:
            if json.loads(inseg_entry)["label"] == label:
                return self.adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_INSEG_ENTRY", inseg_entry).get("SAI_INSEG_ENTRY_ATTR_NEXT_HOP_ID")
        return None

    def check_inseg_nexthop(self, label, nhtype, ip, ostype, labels):
        def _access_function():
            nh = self.get_inseg_nexthop(label)
            if not bool(nh): return (False, None)
            fvs = self.adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nh)
            # NH should be in NH Table
            if not bool(fvs): return (False, None)
            if fvs.get("SAI_NEXT_HOP_ATTR_TYPE") != nhtype: return (False, None)
            if fvs.get("SAI_NEXT_HOP_ATTR_IP") != ip: return (False, None)
            if nhtype != "SAI_NEXT_HOP_TYPE_MPLS": return (True, None)
            if fvs.get("SAI_NEXT_HOP_ATTR_OUTSEG_TYPE") != ostype: return (False, None)
            if fvs.get("SAI_NEXT_HOP_ATTR_LABELSTACK") != labels: return (False, None)
            return (True, None)

        wait_for_result(_access_function)

    def check_inseg_nexthop_group(self, label, count):
        def _access_function():
            nhg = self.get_inseg_nexthop(label)
            if not bool(nhg): return (False, None)
            fvs = self.adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP", nhg)
            # NH should be in NHG Table
            if not bool(fvs): return (False, None)
            keys = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
            matched_nhgms = []
            for key in keys:
                fvs = self.adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER", key)
                if fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhg:
                    matched_nhgms.append(key)

            if len(matched_nhgms) != count: return (False, None)
            return (True, None)

        wait_for_result(_access_function)

    def check_nexthop(self, present, nhtype, ip, ostype, labels):
        def _access_function():
            nhs = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
            for nh in nhs:
                fvs = self.adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nh)
                if fvs.get("SAI_NEXT_HOP_ATTR_TYPE") != nhtype: continue
                if fvs.get("SAI_NEXT_HOP_ATTR_IP") != ip: continue
                if nhtype == "SAI_NEXT_HOP_TYPE_IP": return (present, None)
                if nhtype != "SAI_NEXT_HOP_TYPE_MPLS": continue
                if fvs.get("SAI_NEXT_HOP_ATTR_OUTSEG_TYPE") != ostype: continue
                if fvs.get("SAI_NEXT_HOP_ATTR_LABELSTACK") == labels: return (present, None)
            return (not present, None)

        wait_for_result(_access_function)

    def check_nexthop_group(self, present, nhg):
        def _access_function():
            fvs = self.adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP", nhg)
            if bool(fvs): return (present, None)
            return (not present, None)

        wait_for_result(_access_function)

    def clear_srv_config(self, dvs):
        dvs.servers[0].runcmd("ip address flush dev eth0")
        dvs.servers[1].runcmd("ip address flush dev eth0")
        dvs.servers[2].runcmd("ip address flush dev eth0")
        dvs.servers[3].runcmd("ip address flush dev eth0")

    def setup_mpls(self, dvs, resolve):
        if not self.mpls_appdb_mode():
            dvs.runcmd("modprobe mpls_router")
            dvs.runcmd("modprobe mpls_iptunnel")
            dvs.runcmd("sysctl -w net.mpls.platform_labels=1000")

        self.setup_db(dvs)

        self.clear_srv_config(dvs)

        # create mpls interface
        self.create_mpls_intf("Ethernet0")
        self.create_mpls_intf("Ethernet4")
        self.create_mpls_intf("Ethernet8")

        # set ip address
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        self.add_ip_address("Ethernet4", "10.0.0.2/31")
        self.add_ip_address("Ethernet8", "10.0.0.4/31")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")
        self.set_admin_status("Ethernet8", "up")

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        dvs.servers[1].runcmd("ip address add 10.0.0.3/31 dev eth0")
        dvs.servers[1].runcmd("ip route add default via 10.0.0.2")

        dvs.servers[2].runcmd("ip address add 10.0.0.5/31 dev eth0")
        dvs.servers[2].runcmd("ip route add default via 10.0.0.4")

        # get neighbor and arp entry
        if resolve:
            dvs.servers[0].runcmd("ping -c 1 10.0.0.3")
            dvs.servers[2].runcmd("ping -c 1 10.0.0.3")

    def teardown_mpls(self, dvs):
        # remove ip address
        self.remove_ip_address("Ethernet0", "10.0.0.0/31")
        self.remove_ip_address("Ethernet4", "10.0.0.2/31")
        self.remove_ip_address("Ethernet8", "10.0.0.4/31")

        # remove mpls interface
        self.remove_mpls_intf("Ethernet0")
        self.remove_mpls_intf("Ethernet4")
        self.remove_mpls_intf("Ethernet8")

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")
        self.set_admin_status("Ethernet8", "down")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip route del default dev eth0")
        dvs.servers[0].runcmd("ip address del 10.0.0.1/31 dev eth0")

        dvs.servers[1].runcmd("ip route del default dev eth0")
        dvs.servers[1].runcmd("ip address del 10.0.0.3/31 dev eth0")

        dvs.servers[2].runcmd("ip route del default dev eth0")
        dvs.servers[2].runcmd("ip address del 10.0.0.5/31 dev eth0")

        if not self.mpls_appdb_mode():
            dvs.runcmd("rmmod mpls_iptunnel")
            dvs.runcmd("rmmod mpls_router")

class TestMplsRoute(TestMplsBase):
    """ Functionality tests for mpls """
    def test_RouteAddRemoveIpRoutePush(self, dvs, testlog):
        self.setup_mpls(dvs, True)

        # add route entry
        prefix = "2.2.2.0/24"
        if self.mpls_appdb_mode():
            fieldValues = {"nexthop": "10.0.0.1", "ifname": "Ethernet0", "mpls_nh": "push+201"}
            self.create_route_entry(prefix, fieldValues)
        else:
            # dvs.runcmd("ip route add 2.2.2.0/24 encap mpls 201 via inet 10.0.0.1 dev Ethernet0")
            dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 2.2.2.0/24 10.0.0.1 Ethernet0 label 201\"")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", prefix)

        # check ASIC route database
        self.check_route_entries(True, [prefix])
        self.check_route_nexthop(prefix, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.1", "SAI_OUTSEG_TYPE_PUSH", "1:201")

        # remove route entry
        if self.mpls_appdb_mode():
            self.remove_route_entry(prefix)
        else:
            # dvs.runcmd("ip route del 2.2.2.0/24 encap mpls 201 via inet 10.0.0.1 dev Ethernet0")
            dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 2.2.2.0/24 10.0.0.1 Ethernet0 label 201\"")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", prefix)

        # check ASIC route database
        self.check_route_entries(False, [prefix])
        self.check_nexthop(False, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.1", "SAI_OUTSEG_TYPE_PUSH", "1:201")

        self.teardown_mpls(dvs)

    def test_RouteAddRemoveMplsRouteSwap(self, dvs, testlog):
        self.setup_mpls(dvs, True)

        # add route entry
        label = "200"
        if self.mpls_appdb_mode():
            fieldValues = {"nexthop": "10.0.0.1", "ifname": "Ethernet0", "mpls_nh": "swap+201", "mpls_pop": "1"}
            self.create_inseg_entry(label, fieldValues)
        else:
            # dvs.runcmd("ip -f mpls route add 200 as 201 via inet 10.0.0.1 dev Ethernet0")
            dvs.runcmd("vtysh -c \"configure terminal\" -c \"mpls lsp 200 10.0.0.1 201\"")

        # check application database
        self.pdb.wait_for_entry("LABEL_ROUTE_TABLE", label)

        # check ASIC inseg database
        self.check_inseg_entries(True, [label])
        self.check_inseg_nexthop(label, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.1", "SAI_OUTSEG_TYPE_SWAP", "1:201")

        # remove route entry
        if self.mpls_appdb_mode():
            self.remove_inseg_entry(label)
        else:
            # dvs.runcmd("ip -f mpls route del 200 as 201 via inet 10.0.0.1 dev Ethernet0")
            dvs.runcmd("vtysh -c \"configure terminal\" -c \"no mpls lsp 200 10.0.0.1 201\"")

        # check application database
        self.pdb.wait_for_deleted_entry("LABEL_ROUTE_TABLE", label)

        # check ASIC inseg database
        self.check_inseg_entries(False, [label])
        self.check_nexthop(False, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.1", "SAI_OUTSEG_TYPE_SWAP", "1:201")

        self.teardown_mpls(dvs)

    def test_RouteAddRemoveMplsRouteImplicitNull(self, dvs, testlog):
        self.setup_mpls(dvs, True)

        # add route entry
        label = "200"
        if self.mpls_appdb_mode():
            fieldValues = {"nexthop": "10.0.0.1", "ifname": "Ethernet0", "mpls_pop": "1"}
            self.create_inseg_entry(label, fieldValues)
        else:
            # dvs.runcmd("ip -f mpls route add 200 via inet 10.0.0.1 dev Ethernet0")
            dvs.runcmd("vtysh -c \"configure terminal\" -c \"mpls lsp 200 10.0.0.1 implicit-null\"")

        # check application database
        self.pdb.wait_for_entry("LABEL_ROUTE_TABLE", label)

        # check ASIC inseg database
        self.check_inseg_entries(True, [label])
        self.check_inseg_nexthop(label, "SAI_NEXT_HOP_TYPE_IP", "10.0.0.1", "", "")

        # remove route entry
        if self.mpls_appdb_mode():
            self.remove_inseg_entry(label)
        else:
            # dvs.runcmd("ip -f mpls route del 200 via inet 10.0.0.1 dev Ethernet0")
            dvs.runcmd("vtysh -c \"configure terminal\" -c \"no mpls lsp 200 10.0.0.1 implicit-null\"")

        # check application database
        self.pdb.wait_for_deleted_entry("LABEL_ROUTE_TABLE", label)

        # check ASIC inseg database
        self.check_inseg_entries(False, [label])
        # IP NHs are created by neighbor resolution and should still be present
        self.check_nexthop(True, "SAI_NEXT_HOP_TYPE_IP", "10.0.0.1", "", "")

        self.teardown_mpls(dvs)

    def test_RouteAddRemoveMplsRouteExplicitNull(self, dvs, testlog):
        self.setup_mpls(dvs, True)

        # add route entry
        label = "200"
        if self.mpls_appdb_mode():
            fieldValues = {"nexthop": "10.0.0.1", "ifname": "Ethernet0", "mpls_nh": "swap+0", "mpls_pop": "1"}
            self.create_inseg_entry(label, fieldValues)
        else:
            # dvs.runcmd("ip -f mpls route add 200 as 0 via inet 10.0.0.1 dev Ethernet0")
            dvs.runcmd("vtysh -c \"configure terminal\" -c \"mpls lsp 200 10.0.0.1 explicit-null\"")

        # check application database
        self.pdb.wait_for_entry("LABEL_ROUTE_TABLE", label)

        # check ASIC inseg database
        self.check_inseg_entries(True, [label])
        self.check_inseg_nexthop(label, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.1", "SAI_OUTSEG_TYPE_SWAP", "1:0")

        # remove route entry
        if self.mpls_appdb_mode():
            self.remove_inseg_entry(label)
        else:
            # dvs.runcmd("ip -f mpls route del 200 as 0 via inet 10.0.0.1 dev Ethernet0")
            dvs.runcmd("vtysh -c \"configure terminal\" -c \"no mpls lsp 200 10.0.0.1 explicit-null\"")

        # check application database
        self.pdb.wait_for_deleted_entry("LABEL_ROUTE_TABLE", label)

        # check ASIC inseg database
        self.check_inseg_entries(False, [label])
        self.check_nexthop(False, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.1", "SAI_OUTSEG_TYPE_SWAP", "1:0")

        self.teardown_mpls(dvs)

    def test_RouteAddRemoveIpRoutePushNHG(self, dvs, testlog):
        self.setup_mpls(dvs, True)

        # add route entry
        prefix = "2.2.2.0/24"
        if self.mpls_appdb_mode():
            fieldValues = {"nexthop": "10.0.0.1,10.0.0.5", "ifname": "Ethernet0,Ethernet8", "mpls_nh": "push+200,push+201"}
            self.create_route_entry(prefix, fieldValues)
        else:
            dvs.runcmd("ip route add 2.2.2.0/24 nexthop encap mpls 200 via inet 10.0.0.1 dev Ethernet0 nexthop encap mpls 201 via inet 10.0.0.5 dev Ethernet8")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", prefix)

        # check ASIC route database
        self.check_route_entries(True, [prefix])
        self.check_route_nexthop_group(prefix, 2)
        self.check_nexthop(True, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.1", "SAI_OUTSEG_TYPE_PUSH", "1:200")
        self.check_nexthop(True, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.5", "SAI_OUTSEG_TYPE_PUSH", "1:201")
        nhg = self.get_route_nexthop(prefix)

        # remove route entry
        if self.mpls_appdb_mode():
            self.remove_route_entry(prefix)
        else:
            dvs.runcmd("ip route del 2.2.2.0/24 nexthop encap mpls 200 via inet 10.0.0.1 dev Ethernet0 nexthop encap mpls 201 via inet 10.0.0.5 dev Ethernet8")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", prefix)

        # check ASIC route database
        self.check_route_entries(False, [prefix])
        self.check_nexthop_group(False, nhg)
        self.check_nexthop(False, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.1", "SAI_OUTSEG_TYPE_PUSH", "1:200")
        self.check_nexthop(False, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.5", "SAI_OUTSEG_TYPE_PUSH", "1:201")

        self.teardown_mpls(dvs)

    def test_RouteAddRemoveMplsRouteSwapNHG(self, dvs, testlog):
        self.setup_mpls(dvs, True)

        # add route entry
        label = "200"
        if self.mpls_appdb_mode():
            fieldValues = {"nexthop": "10.0.0.1,10.0.0.5", "ifname": "Ethernet0,Ethernet8", "mpls_nh": "swap+201,swap+202", "mpls_pop": "1"}
            self.create_inseg_entry(label, fieldValues)
        else:
            dvs.runcmd("ip -f mpls route add 200 nexthop as 201 via inet 10.0.0.1 dev Ethernet0 nexthop as 202 via inet 10.0.0.5 dev Ethernet8")

        # check application database
        self.pdb.wait_for_entry("LABEL_ROUTE_TABLE", label)

        # check ASIC inseg database
        self.check_inseg_entries(True, [label])
        self.check_inseg_nexthop_group(label, 2)
        self.check_nexthop(True, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.1", "SAI_OUTSEG_TYPE_SWAP", "1:201")
        self.check_nexthop(True, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.5", "SAI_OUTSEG_TYPE_SWAP", "1:202")
        nhg = self.get_inseg_nexthop(label)

        # remove route entry
        if self.mpls_appdb_mode():
            self.remove_inseg_entry(label)
        else:
            dvs.runcmd("ip -f mpls route del 200 nexthop as 201 via inet 10.0.0.1 dev Ethernet0 nexthop as 202 via inet 10.0.0.5 dev Ethernet8")

        # check application database
        self.pdb.wait_for_deleted_entry("LABEL_ROUTE_TABLE", label)

        # check ASIC inseg database
        self.check_inseg_entries(False, [label])
        self.check_nexthop_group(False, nhg)
        self.check_nexthop(False, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.1", "SAI_OUTSEG_TYPE_SWAP", "1:201")
        self.check_nexthop(False, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.5", "SAI_OUTSEG_TYPE_SWAP", "1:202")

        self.teardown_mpls(dvs)

    def test_RouteAddRemoveMplsRoutePopNHG(self, dvs, testlog):
        self.setup_mpls(dvs, True)

        # add route entry
        label = "200"
        if self.mpls_appdb_mode():
            fieldValues = {"nexthop": "10.0.0.1,10.0.0.5", "ifname": "Ethernet0,Ethernet8", "mpls_pop": "1"}
            self.create_inseg_entry(label, fieldValues)
        else:
            dvs.runcmd("ip -f mpls route add 200 nexthop via inet 10.0.0.1 dev Ethernet0 nexthop via inet 10.0.0.5 dev Ethernet8")

        # check application database
        self.pdb.wait_for_entry("LABEL_ROUTE_TABLE", label)

        # check ASIC inseg database
        self.check_inseg_entries(True, [label])
        self.check_inseg_nexthop_group(label, 2)
        self.check_nexthop(True, "SAI_NEXT_HOP_TYPE_IP", "10.0.0.1", "", "")
        self.check_nexthop(True, "SAI_NEXT_HOP_TYPE_IP", "10.0.0.5", "", "")
        nhg = self.get_inseg_nexthop(label)

        # remove route entry
        if self.mpls_appdb_mode():
            self.remove_inseg_entry(label)
        else:
            dvs.runcmd("ip -f mpls route del 200 nexthop via inet 10.0.0.1 dev Ethernet0 nexthop via inet 10.0.0.5 dev Ethernet8")

        # check application database
        self.pdb.wait_for_deleted_entry("LABEL_ROUTE_TABLE", label)

        # check ASIC inseg database
        self.check_inseg_entries(False, [label])
        self.check_nexthop_group(False, nhg)
        # IP NHs are created by neighbor resolution and should still be present
        self.check_nexthop(True, "SAI_NEXT_HOP_TYPE_IP", "10.0.0.1", "", "")
        self.check_nexthop(True, "SAI_NEXT_HOP_TYPE_IP", "10.0.0.5", "", "")

        self.teardown_mpls(dvs)

    def test_RouteAddRemoveIpRouteMixedNHG(self, dvs, testlog):
        self.setup_mpls(dvs, True)

        # add route entry
        prefix = "2.2.2.0/24"
        if self.mpls_appdb_mode():
            fieldValues = {"nexthop": "10.0.0.1,10.0.0.5", "ifname": "Ethernet0,Ethernet8", "mpls_nh": "push+200,na"}
            self.create_route_entry(prefix, fieldValues)
        else:
            dvs.runcmd("ip route add 2.2.2.0/24 nexthop encap mpls 200 via inet 10.0.0.1 dev Ethernet0 nexthop via inet 10.0.0.5 dev Ethernet8")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", prefix)

        # check ASIC route database
        self.check_route_entries(True, [prefix])
        self.check_route_nexthop_group(prefix, 2)
        self.check_nexthop(True, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.1", "SAI_OUTSEG_TYPE_PUSH", "1:200")
        self.check_nexthop(True, "SAI_NEXT_HOP_TYPE_IP", "10.0.0.5", "", "")
        nhg = self.get_route_nexthop(prefix)

        # remove route entry
        if self.mpls_appdb_mode():
            self.remove_route_entry(prefix)
        else:
            dvs.runcmd("ip route del 2.2.2.0/24 nexthop encap mpls 200 via inet 10.0.0.1 dev Ethernet0 nexthop via inet 10.0.0.5 dev Ethernet8")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", prefix)

        # check ASIC route database
        self.check_route_entries(False, [prefix])
        self.check_nexthop_group(False, nhg)
        self.check_nexthop(False, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.1", "SAI_OUTSEG_TYPE_SWAP", "1:200")
        # IP NHs are created by neighbor resolution and should still be present
        self.check_nexthop(True, "SAI_NEXT_HOP_TYPE_IP", "10.0.0.5", "", "")

        self.teardown_mpls(dvs)

    def test_RouteAddRemoveMplsRouteMixedNHG(self, dvs, testlog):
        self.setup_mpls(dvs, True)

        # add route entry
        label = "200"
        if self.mpls_appdb_mode():
            fieldValues = {"nexthop": "10.0.0.1,10.0.0.5", "ifname": "Ethernet0,Ethernet8", "mpls_nh": "na,swap+201", "mpls_pop": "1"}
            self.create_inseg_entry(label, fieldValues)
        else:
            dvs.runcmd("ip -f mpls route add 200 nexthop via inet 10.0.0.1 dev Ethernet0 nexthop as 201 via inet 10.0.0.5 dev Ethernet8")

        # check application database
        self.pdb.wait_for_entry("LABEL_ROUTE_TABLE", label)

        # check ASIC route database
        self.check_inseg_entries(True, [label])
        self.check_inseg_nexthop_group(label, 2)
        self.check_nexthop(True, "SAI_NEXT_HOP_TYPE_IP", "10.0.0.1", "", "")
        self.check_nexthop(True, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.5", "SAI_OUTSEG_TYPE_SWAP", "1:201")
        nhg = self.get_inseg_nexthop(label)

        # remove inseg entry
        if self.mpls_appdb_mode():
            self.remove_inseg_entry(label)
        else:
            dvs.runcmd("ip -f mpls route del 200 nexthop via inet 10.0.0.1 dev Ethernet0 nexthop as 201 via inet 10.0.0.5 dev Ethernet8")

        # check application database
        self.pdb.wait_for_deleted_entry("LABEL_ROUTE_TABLE", label)

        # check ASIC route database
        self.check_inseg_entries(False, [label])
        self.check_nexthop_group(False, nhg)
        self.check_nexthop(False, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.5", "SAI_OUTSEG_TYPE_SWAP", "1:201")
        # IP NHs are created by neighbor resolution and should still be present
        self.check_nexthop(True, "SAI_NEXT_HOP_TYPE_IP", "10.0.0.1", "", "")

        self.teardown_mpls(dvs)

    def test_RouteUnresolvedMplsRouteSwap(self, dvs, testlog):
        self.setup_mpls(dvs, False)

        # add route entry
        label = "200"
        if self.mpls_appdb_mode():
            fieldValues = {"nexthop": "10.0.0.1", "ifname": "Ethernet0", "mpls_nh": "swap+201", "mpls_pop": "1"}
            self.create_inseg_entry(label, fieldValues)
        else:
            # dvs.runcmd("ip -f mpls route add 200 as 201 via inet 10.0.0.1 dev Ethernet0")
            dvs.runcmd("vtysh -c \"configure terminal\" -c \"mpls lsp 200 10.0.0.1 201\"")

        # check application database
        self.pdb.wait_for_entry("LABEL_ROUTE_TABLE", label)

        # check ASIC inseg database. inseg with unresolved NH should not be present.
        self.check_inseg_entries(False, [label])
        self.check_nexthop(False, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.1", "SAI_OUTSEG_TYPE_SWAP", "1:201")

        # now resolve the NH
        dvs.servers[0].runcmd("ping -c 1 10.0.0.3")
        dvs.servers[2].runcmd("ping -c 1 10.0.0.3")

        # check ASIC inseg database
        self.check_inseg_entries(True, [label])
        self.check_inseg_nexthop(label, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.1", "SAI_OUTSEG_TYPE_SWAP", "1:201")

        # remove route entry
        if self.mpls_appdb_mode():
            self.remove_inseg_entry(label)
        else:
            # dvs.runcmd("ip -f mpls route del 200 as 201 via inet 10.0.0.1 dev Ethernet0")
            dvs.runcmd("vtysh -c \"configure terminal\" -c \"no mpls lsp 200 10.0.0.1 201\"")

        # check application database
        self.pdb.wait_for_deleted_entry("LABEL_ROUTE_TABLE", label)

        # check ASIC inseg database
        self.check_inseg_entries(False, [label])
        self.check_nexthop(False, "SAI_NEXT_HOP_TYPE_MPLS", "10.0.0.1", "SAI_OUTSEG_TYPE_SWAP", "1:201")

        self.teardown_mpls(dvs)

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
