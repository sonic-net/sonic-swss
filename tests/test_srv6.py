import os
import re
import time
import json
import pytest
import distro
import platform

from swsscommon import swsscommon
from distutils.version import LooseVersion
from dvslib.dvs_common import wait_for_result

def get_exist_entries(db, table):
    tbl =  swsscommon.Table(db, table)
    return set(tbl.getKeys())

def get_created_entry(db, table, existed_entries):
    tbl =  swsscommon.Table(db, table)
    entries = set(tbl.getKeys())
    new_entries = list(entries - existed_entries)
    assert len(new_entries) == 1, "Wrong number of created entries."
    return new_entries[0]

class TestSrv6Mysid(object):
    def setup_db(self, dvs):
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()
        self.cdb = dvs.get_config_db()

    def create_vrf(self, vrf_name):
        table = "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"
        existed_entries = get_exist_entries(self.adb.db_connection, table)

        self.cdb.create_entry("VRF", vrf_name, {"empty": "empty"})

        self.adb.wait_for_n_keys(table, len(existed_entries) + 1)
        return get_created_entry(self.adb.db_connection, table, existed_entries)

    def remove_vrf(self, vrf_name):
        self.cdb.delete_entry("VRF", vrf_name)

    def add_ip_address(self, interface, ip):
        self.cdb.create_entry("INTERFACE", interface + "|" + ip, {"NULL": "NULL"})

    def remove_ip_address(self, interface, ip):
        self.cdb.delete_entry("INTERFACE", interface + "|" + ip)

    def add_neighbor(self, interface, ip, mac, family):
        table = "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY"
        existed_entries = get_exist_entries(self.adb.db_connection, table)

        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "NEIGH_TABLE")
        fvs = swsscommon.FieldValuePairs([("neigh", mac),
                                          ("family", family)])
        tbl.set(interface + ":" + ip, fvs)

        self.adb.wait_for_n_keys(table, len(existed_entries) + 1)
        return get_created_entry(self.adb.db_connection, table, existed_entries)

    def remove_neighbor(self, interface, ip):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "NEIGH_TABLE")
        tbl._del(interface + ":" + ip)
        time.sleep(1)

    def create_mysid(self, mysid, fvs):
        table = "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY"
        existed_entries = get_exist_entries(self.adb.db_connection, table)

        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "SRV6_MY_SID_TABLE")
        tbl.set(mysid, fvs)

        self.adb.wait_for_n_keys(table, len(existed_entries) + 1)
        return get_created_entry(self.adb.db_connection, table, existed_entries)

    def remove_mysid(self, mysid):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "SRV6_MY_SID_TABLE")
        tbl._del(mysid)

    def create_l3_intf(self, interface, vrf_name):
        table = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
        existed_entries = get_exist_entries(self.adb.db_connection, table)

        if len(vrf_name) == 0:
            self.cdb.create_entry("INTERFACE", interface, {"NULL": "NULL"})
        else:
            self.cdb.create_entry("INTERFACE", interface, {"vrf_name": vrf_name})

        self.adb.wait_for_n_keys(table, len(existed_entries) + 1)
        return get_created_entry(self.adb.db_connection, table, existed_entries)

    def remove_l3_intf(self, interface):
        self.cdb.delete_entry("INTERFACE", interface)

    def get_nexthop_id(self, ip_address):
        next_hop_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        for next_hop_entry in next_hop_entries:
            (status, fvs) = tbl.get(next_hop_entry)

            assert status == True
            assert len(fvs) == 3

            for fv in fvs:
                if fv[0] == "SAI_NEXT_HOP_ATTR_IP" and fv[1] == ip_address:
                    return next_hop_entry

        return None

    def set_interface_status(self, dvs, interface, admin_status):
        tbl_name = "PORT"
        tbl = swsscommon.Table(self.cdb.db_connection, tbl_name)
        fvs = swsscommon.FieldValuePairs([("admin_status", "up")])
        tbl.set(interface, fvs)
        time.sleep(1)

    def test_mysid(self, dvs, testlog):
        self.setup_db(dvs)

        # create MySID entries
        mysid1='16:8:8:8:baba:2001:10::'
        mysid2='16:8:8:8:baba:2001:20::'
        mysid3='16:8:8:8:fcbb:bb01:800::'
        mysid4='16:8:8:8:baba:2001:40::'
        mysid5='32:16:16:0:fc00:0:1:e000::'
        mysid6='32:16:16:0:fc00:0:1:e001::'
        mysid7='32:16:16:0:fc00:0:1:e002::'
        mysid8='32:16:16:0:fc00:0:1:e003::'
        mysid9='32:16:16:0:fc00:0:1:e004::'
        mysid10='32:16:16:0:fc00:0:1:e005::'

        # create MySID END
        fvs = swsscommon.FieldValuePairs([('action', 'end')])
        key = self.create_mysid(mysid1, fvs)

        # check ASIC MySID database
        mysid = json.loads(key)
        assert mysid["sid"] == "baba:2001:10::"
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_E"

        # create vrf
        vrf_id = self.create_vrf("VrfDt46")

        # create MySID END.DT46
        fvs = swsscommon.FieldValuePairs([('action', 'end.dt46'), ('vrf', 'VrfDt46')])
        key = self.create_mysid(mysid2, fvs)

        # check ASIC MySID database
        mysid = json.loads(key)
        assert mysid["sid"] == "baba:2001:20::"
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_VRF":
                assert fv[1] == vrf_id
            elif fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT46"

        # create MySID uN
        fvs = swsscommon.FieldValuePairs([('action', 'un')])
        key = self.create_mysid(mysid3, fvs)

        # check ASIC MySID database
        mysid = json.loads(key)
        assert mysid["sid"] == "fcbb:bb01:800::"
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UN"
            elif fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD"

        # create MySID END.DT4 with default vrf
        fvs = swsscommon.FieldValuePairs([('action', 'end.dt4'), ('vrf', 'default')])
        key = self.create_mysid(mysid4, fvs)

        # check ASIC MySID database
        mysid = json.loads(key)
        assert mysid["sid"] == "baba:2001:40::"
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_VRF":
                assert True
            elif fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT4"

        # create interface
        self.create_l3_intf("Ethernet104", "")

        # Assign IP to interface
        self.add_ip_address("Ethernet104", "2001::2/126")
        self.add_ip_address("Ethernet104", "192.0.2.2/30")

        # create neighbor
        self.add_neighbor("Ethernet104", "2001::1", "00:00:00:01:02:04", "IPv6")
        self.add_neighbor("Ethernet104", "192.0.2.1", "00:00:00:01:02:05", "IPv4")

        # get nexthops
        next_hop_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        assert len(next_hop_entries) == 2

        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        for next_hop_entry in next_hop_entries:
            (status, fvs) = tbl.get(next_hop_entry)

            assert status == True
            assert len(fvs) == 3

            for fv in fvs:
                if fv[0] == "SAI_NEXT_HOP_ATTR_IP":
                    if fv[1] == "2001::1":
                        next_hop_ipv6_id = next_hop_entry
                    elif fv[1] == "192.0.2.1":
                        next_hop_ipv4_id = next_hop_entry
                    else:
                        assert False, "Nexthop IP %s not expected" % fv[1]

        assert next_hop_ipv6_id is not None
        assert next_hop_ipv4_id is not None

        # create MySID END.X
        fvs = swsscommon.FieldValuePairs([('action', 'end.x'), ('adj', '2001::1')])
        key = self.create_mysid(mysid5, fvs)

        # check ASIC MySID database
        mysid = json.loads(key)
        assert mysid["sid"] == "fc00:0:1:e000::"
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_NEXT_HOP_ID":
                assert fv[1] == next_hop_ipv6_id
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_X"
            elif fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD"

        # create MySID END.DX4
        fvs = swsscommon.FieldValuePairs([('action', 'end.dx4'), ('adj', '192.0.2.1')])
        key = self.create_mysid(mysid6, fvs)

        # check ASIC MySID database
        mysid = json.loads(key)
        assert mysid["sid"] == "fc00:0:1:e001::"
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_NEXT_HOP_ID":
                assert fv[1] == next_hop_ipv4_id
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX4"
            elif fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD"

        # create MySID END.DX6
        fvs = swsscommon.FieldValuePairs([('action', 'end.dx6'), ('adj', '2001::1')])
        key = self.create_mysid(mysid7, fvs)

        # check ASIC MySID database
        mysid = json.loads(key)
        assert mysid["sid"] == "fc00:0:1:e002::"
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_NEXT_HOP_ID":
                assert fv[1] == next_hop_ipv6_id
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX6"
            elif fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD"

        # create MySID uA
        fvs = swsscommon.FieldValuePairs([('action', 'ua'), ('adj', '2001::1')])
        key = self.create_mysid(mysid8, fvs)

        # check ASIC MySID database
        mysid = json.loads(key)
        assert mysid["sid"] == "fc00:0:1:e003::"
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_NEXT_HOP_ID":
                assert fv[1] == next_hop_ipv6_id
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UA"
            elif fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD"

        # create MySID uDX4
        fvs = swsscommon.FieldValuePairs([('action', 'udx4'), ('adj', '192.0.2.1')])
        key = self.create_mysid(mysid9, fvs)

        # check ASIC MySID database
        mysid = json.loads(key)
        assert mysid["sid"] == "fc00:0:1:e004::"
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_NEXT_HOP_ID":
                assert fv[1] == next_hop_ipv4_id
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX4"
            elif fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD"

        # create MySID END.DX6
        fvs = swsscommon.FieldValuePairs([('action', 'udx6'), ('adj', '2001::1')])
        key = self.create_mysid(mysid10, fvs)

        # check ASIC MySID database
        mysid = json.loads(key)
        assert mysid["sid"] == "fc00:0:1:e005::"
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_NEXT_HOP_ID":
                assert fv[1] == next_hop_ipv6_id
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DX6"
            elif fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD"

        # delete MySID
        self.remove_mysid(mysid1)
        self.remove_mysid(mysid2)
        self.remove_mysid(mysid3)
        self.remove_mysid(mysid4)
        self.remove_mysid(mysid5)
        self.remove_mysid(mysid6)
        self.remove_mysid(mysid7)
        self.remove_mysid(mysid8)
        self.remove_mysid(mysid9)
        self.remove_mysid(mysid10)

        # remove vrf
        self.remove_vrf("VrfDt46")

        # remove nexthop
        self.remove_neighbor("Ethernet104", "2001::1")
        self.remove_neighbor("Ethernet104", "192.0.2.1")

        # Reemove IP from interface
        self.remove_ip_address("Ethernet104", "2001::2/126")
        self.remove_ip_address("Ethernet104", "192.0.2.2/30")

        self.remove_l3_intf("Ethernet104")

    def test_mysid_l3adj(self, dvs, testlog):
        self.setup_db(dvs)

        # create MySID entries
        mysid1='32:16:16:0:fc00:0:1:e000::'

        # create interface
        self.create_l3_intf("Ethernet104", "")

        # assign IP to interface
        self.add_ip_address("Ethernet104", "2001::2/64")

        time.sleep(3)

        # bring up Ethernet104
        self.set_interface_status(dvs, "Ethernet104", "up")

        time.sleep(3)

        # save the initial number of entries in MySID table
        initial_my_sid_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")

        # save the initial number of entries in Nexthop table
        initial_next_hop_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")

        # create MySID END.X, neighbor does not exist yet
        fvs = swsscommon.FieldValuePairs([('action', 'end.x'), ('adj', '2001::1')])
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "SRV6_MY_SID_TABLE")
        tbl.set(mysid1, fvs)

        time.sleep(2)

        # check the current number of entries in MySID table
        # since the neighbor does not exist yet, we expect the SID has not been installed (i.e., we
        # expect the same number of MySID entries as before)
        exist_my_sid_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        assert len(exist_my_sid_entries) == len(initial_my_sid_entries)

        # now, let's create the neighbor
        self.add_neighbor("Ethernet104", "2001::1", "00:00:00:01:02:04", "IPv6")

        # verify that the nexthop is created in the ASIC (i.e., we have the previous number of next hop entries + 1)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", len(initial_next_hop_entries) + 1)

        # get the new nexthop and nexthop ID, which will be used later to verify the MySID entry
        next_hop_entry = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", initial_next_hop_entries)
        assert next_hop_entry is not None
        next_hop_id = self.get_nexthop_id("2001::1")
        assert next_hop_id is not None

        # now the neighbor has been created in the ASIC, we expect the MySID entry to be created in the ASIC as well
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(initial_my_sid_entries) + 1)
        my_sid_entry = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", initial_my_sid_entries)
        assert my_sid_entry is not None

        # check ASIC MySID database and verify the SID
        mysid = json.loads(my_sid_entry)
        assert mysid is not None
        assert mysid["sid"] == "fc00:0:1:e000::"
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(my_sid_entry)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_NEXT_HOP_ID":
                assert fv[1] == next_hop_id
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_X"
            elif fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD"

        # remove neighbor
        self.remove_neighbor("Ethernet104", "2001::1")

        # delete MySID
        self.remove_mysid(mysid1)

        # # verify that the nexthop has been removed from the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", len(initial_next_hop_entries))

        # check the current number of entries in MySID table
        # since the MySID has been removed, we expect the SID has been removed from the ASIC as well
        exist_my_sid_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        assert len(exist_my_sid_entries) == len(initial_my_sid_entries)

        # remove IP from interface
        self.remove_ip_address("Ethernet104", "2001::2/64")

        # remove interface
        self.remove_l3_intf("Ethernet104")

class TestSrv6(object):
    def setup_db(self, dvs):
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()
        self.cdb = dvs.get_config_db()

    def create_sidlist(self, segname, ips, type=None):
        table = "ASIC_STATE:SAI_OBJECT_TYPE_SRV6_SIDLIST"
        existed_entries = get_exist_entries(self.adb.db_connection, table)

        if type is None:
            fvs=swsscommon.FieldValuePairs([('path', ips)])
        else:
            fvs=swsscommon.FieldValuePairs([('path', ips), ('type', type)])
        segtbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "SRV6_SID_LIST_TABLE")
        segtbl.set(segname, fvs)

        self.adb.wait_for_n_keys(table, len(existed_entries) + 1)
        return get_created_entry(self.adb.db_connection, table, existed_entries)

    def remove_sidlist(self, segname):
        segtbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "SRV6_SID_LIST_TABLE")
        segtbl._del(segname)

    def create_srv6_route(self, routeip,segname,segsrc):
        table = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"
        existed_entries = get_exist_entries(self.adb.db_connection, table)

        fvs=swsscommon.FieldValuePairs([('seg_src',segsrc),('segment',segname)])
        routetbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "ROUTE_TABLE")
        routetbl.set(routeip,fvs)

        self.adb.wait_for_n_keys(table, len(existed_entries) + 1)
        return get_created_entry(self.adb.db_connection, table, existed_entries)

    def remove_srv6_route(self, routeip):
        routetbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "ROUTE_TABLE")
        routetbl._del(routeip)

    def check_deleted_route_entries(self, destinations):
        def _access_function():
            route_entries = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            route_destinations = [json.loads(route_entry)["dest"] for route_entry in route_entries]
            return (all(destination not in route_destinations for destination in destinations), None)

        wait_for_result(_access_function)

    def add_neighbor(self, interface, ip, mac):
        fvs=swsscommon.FieldValuePairs([("neigh", mac)])
        neightbl = swsscommon.Table(self.cdb.db_connection, "NEIGH")
        neightbl.set(interface + "|" +ip, fvs)
        time.sleep(1)

    def remove_neighbor(self, interface,ip):
        neightbl = swsscommon.Table(self.cdb.db_connection, "NEIGH")
        neightbl._del(interface + "|" + ip)
        time.sleep(1)

    def test_srv6(self, dvs, testlog):
        self.setup_db(dvs)
        dvs.setup_db()

        # save exist asic db entries
        tunnel_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        nexthop_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        route_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")


        # bring up interfacee
        dvs.set_interface_status("Ethernet104", "up")
        dvs.set_interface_status("Ethernet112", "up")
        dvs.set_interface_status("Ethernet120", "up")

        # add neighbors
        self.add_neighbor("Ethernet104", "baba:2001:10::", "00:00:00:01:02:01")
        self.add_neighbor("Ethernet112", "baba:2002:10::", "00:00:00:01:02:02")
        self.add_neighbor("Ethernet120", "baba:2003:10::", "00:00:00:01:02:03")

        # create seg lists
        sidlist_id = self.create_sidlist('seg1', 'baba:2001:10::,baba:2001:20::')

        # check ASIC SAI_OBJECT_TYPE_SRV6_SIDLIST database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_SRV6_SIDLIST")
        (status, fvs) = tbl.get(sidlist_id)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_SRV6_SIDLIST_ATTR_SEGMENT_LIST":
                assert fv[1] == "2:baba:2001:10::,baba:2001:20::"
            elif fv[0] == "SAI_SRV6_SIDLIST_ATTR_TYPE":
                assert fv[1] == "SAI_SRV6_SIDLIST_TYPE_ENCAPS_RED"


        # create v4 route with single sidlists
        route_key = self.create_srv6_route('20.20.20.20/32','seg1','1001:2000::1')
        nexthop_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nexthop_entries)
        tunnel_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", tunnel_entries)

        # check ASIC SAI_OBJECT_TYPE_ROUTE_ENTRY database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        (status, fvs) = tbl.get(route_key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID":
                assert fv[1] == nexthop_id

        # check ASIC SAI_OBJECT_TYPE_NEXT_HOP database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        (status, fvs) = tbl.get(nexthop_id)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_NEXT_HOP_ATTR_SRV6_SIDLIST_ID":
                assert fv[1] == sidlist_id
            elif fv[0] == "SAI_NEXT_HOP_ATTR_TUNNEL_ID":
                assert fv[1] == tunnel_id

        # check ASIC SAI_OBJECT_TYPE_TUNNEL database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        (status, fvs) = tbl.get(tunnel_id)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_TUNNEL_ATTR_TYPE":
                assert fv[1] == "SAI_TUNNEL_TYPE_SRV6"
            elif fv[0] == "SAI_TUNNEL_ATTR_ENCAP_SRC_IP":
                assert fv[1] == "1001:2000::1"


        # create 2nd seg lists
        sidlist_id = self.create_sidlist('seg2', 'baba:2002:10::,baba:2002:20::', 'insert.red')

        # check ASIC SAI_OBJECT_TYPE_SRV6_SIDLIST database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_SRV6_SIDLIST")
        (status, fvs) = tbl.get(sidlist_id)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_SRV6_SIDLIST_ATTR_SEGMENT_LIST":
                assert fv[1] == "2:baba:2002:10::,baba:2002:20::"
            elif fv[0] == "SAI_SRV6_SIDLIST_ATTR_TYPE":
                assert fv[1] == "SAI_SRV6_SIDLIST_TYPE_INSERT_RED"

        # create 3rd seg lists with unsupported or wrong naming of sid list type, for this case, it will use default type: ENCAPS_RED
        sidlist_id = self.create_sidlist('seg3', 'baba:2003:10::,baba:2003:20::', 'reduced')

        # check ASIC SAI_OBJECT_TYPE_SRV6_SIDLIST database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_SRV6_SIDLIST")
        (status, fvs) = tbl.get(sidlist_id)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_SRV6_SIDLIST_ATTR_SEGMENT_LIST":
                assert fv[1] == "2:baba:2003:10::,baba:2003:20::"
            elif fv[0] == "SAI_SRV6_SIDLIST_ATTR_TYPE":
                assert fv[1] == "SAI_SRV6_SIDLIST_TYPE_ENCAPS_RED"

        # create 2nd v4 route with single sidlists
        self.create_srv6_route('20.20.20.21/32','seg2','1001:2000::1')
        # create 3rd v4 route with single sidlists
        self.create_srv6_route('20.20.20.22/32','seg3','1001:2000::1')

        # remove routes
        self.remove_srv6_route('20.20.20.20/32')
        self.check_deleted_route_entries('20.20.20.20/32')
        self.remove_srv6_route('20.20.20.21/32')
        self.check_deleted_route_entries('20.20.20.21/32')
        self.remove_srv6_route('20.20.20.22/32')
        self.check_deleted_route_entries('20.20.20.22/32')

        # remove sid lists
        self.remove_sidlist('seg1')
        self.remove_sidlist('seg2')
        self.remove_sidlist('seg3')

        # remove neighbors
        self.remove_neighbor("Ethernet104", "baba:2001:10::")
        self.remove_neighbor("Ethernet112", "baba:2002:10::")
        self.remove_neighbor("Ethernet120", "baba:2003:10::")

        # check if asic db entries are all restored
        assert tunnel_entries == get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        assert nexthop_entries == get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        assert route_entries == get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")


class TestSrv6MySidFpmsyncd(object):
    """ Functionality tests for Srv6 MySid handling in fpmsyncd """

    def setup_db(self, dvs):
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()
        self.cdb = dvs.get_config_db()

    def create_vrf(self, vrf_name):
        table = "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"
        existed_entries = get_exist_entries(self.adb.db_connection, table)

        self.cdb.create_entry("VRF", vrf_name, {"empty": "empty"})

        self.adb.wait_for_n_keys(table, len(existed_entries) + 1)
        return get_created_entry(self.adb.db_connection, table, existed_entries)

    def remove_vrf(self, vrf_name):
        self.cdb.delete_entry("VRF", vrf_name)

    def add_ip_address(self, interface, ip):
        self.cdb.create_entry("INTERFACE", interface + "|" + ip, {"NULL": "NULL"})

    def remove_ip_address(self, interface, ip):
        self.cdb.delete_entry("INTERFACE", interface + "|" + ip)

    def add_neighbor(self, interface, ip, mac, family):
        table = "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY"
        existed_entries = get_exist_entries(self.adb.db_connection, table)

        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "NEIGH_TABLE")
        fvs = swsscommon.FieldValuePairs([("neigh", mac),
                                          ("family", family)])
        tbl.set(interface + ":" + ip, fvs)

        self.adb.wait_for_n_keys(table, len(existed_entries) + 1)
        return get_created_entry(self.adb.db_connection, table, existed_entries)

    def remove_neighbor(self, interface, ip):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "NEIGH_TABLE")
        tbl._del(interface + ":" + ip)
        time.sleep(1)

    def create_l3_intf(self, interface, vrf_name):
        table = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
        existed_entries = get_exist_entries(self.adb.db_connection, table)

        if len(vrf_name) == 0:
            self.cdb.create_entry("INTERFACE", interface, {"NULL": "NULL"})
        else:
            self.cdb.create_entry("INTERFACE", interface, {"vrf_name": vrf_name})

        self.adb.wait_for_n_keys(table, len(existed_entries) + 1)
        return get_created_entry(self.adb.db_connection, table, existed_entries)

    def remove_l3_intf(self, interface):
        self.cdb.delete_entry("INTERFACE", interface)

    def get_nexthop_id(self, ip_address):
        next_hop_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        for next_hop_entry in next_hop_entries:
            (status, fvs) = tbl.get(next_hop_entry)

            assert status == True
            assert len(fvs) == 3

            for fv in fvs:
                if fv[0] == "SAI_NEXT_HOP_ATTR_IP" and fv[1] == ip_address:
                    return next_hop_entry

        return None

    def set_interface_status(self, dvs, interface, admin_status):
        tbl_name = "PORT"
        tbl = swsscommon.Table(self.cdb.db_connection, tbl_name)
        fvs = swsscommon.FieldValuePairs([("admin_status", "up")])
        tbl.set(interface, fvs)
        time.sleep(1)

    def setup_srv6(self, dvs):
        self.setup_db(dvs)

        dvs.runcmd("sysctl -w net.vrf.strict_mode=1")

        # create interface
        self.create_l3_intf("Ethernet104", "")

        # assign IP to interface
        self.add_ip_address("Ethernet104", "2001::2/126")
        self.add_ip_address("Ethernet104", "192.0.2.2/30")

        time.sleep(3)

        # bring up Ethernet104
        self.set_interface_status(dvs, "Ethernet104", "up")

        time.sleep(3)

        # save the initial number of entries in MySID table
        self.initial_my_sid_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")

        # save the initial number of entries in Nexthop table
        self.initial_next_hop_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")

        # now, let's create the IPv6 neighbor
        self.add_neighbor("Ethernet104", "2001::1", "00:00:00:01:02:04", "IPv6")

        # verify that the nexthop is created in the ASIC (i.e., we have the previous number of next hop entries + 1)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", len(self.initial_next_hop_entries) + 1)

        # get the new nexthop and nexthop ID, which will be used later to verify the MySID entry
        next_hop_entry = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", self.initial_next_hop_entries)
        assert next_hop_entry is not None
        self.next_hop_ipv6_id = self.get_nexthop_id("2001::1")
        assert self.next_hop_ipv6_id is not None

        # save the number of entries in Nexthop table, after adding the ipv6 neighbor
        updated_next_hop_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")

        # now, let's create the IPv4 neighbor
        self.add_neighbor("Ethernet104", "192.0.2.1", "00:00:00:01:02:05", "IPv4")

        # verify that the nexthop is created in the ASIC (i.e., we have the previous number of next hop entries + 1)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", len(updated_next_hop_entries) + 1)

        # get the new nexthop and nexthop ID, which will be used later to verify the MySID entry
        next_hop_entry = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", updated_next_hop_entries)
        assert next_hop_entry is not None
        self.next_hop_ipv4_id = self.get_nexthop_id("192.0.2.1")
        assert self.next_hop_ipv4_id is not None

        # create vrf
        initial_vrf_entries = set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))
        self.create_vrf("Vrf10")
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER", len(initial_vrf_entries) + 1)
        current_vrf_entries = set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))
        self.vrf_id = list(current_vrf_entries - initial_vrf_entries)[0]
        _, vrf_info = dvs.runcmd("ip --json -d link show Vrf10")
        vrf_info_json = json.loads(vrf_info)
        self.vrf_table_id = str(vrf_info_json[0]["linkinfo"]["info_data"]["table"])

        # create dummy interface sr0
        dvs.runcmd("ip link add sr0 type dummy")
        dvs.runcmd("ip link set sr0 up")

    def teardown_srv6(self, dvs):
        # remove dummy interface sr0
        dvs.runcmd("ip link del sr0 type dummy")

        # remove vrf
        initial_vrf_entries = set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))
        self.remove_vrf("Vrf10")
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER", len(initial_vrf_entries) - 1)

        # remove the IPv4 neighbor
        initial_neighbor_entries = set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP"))
        self.remove_neighbor("Ethernet104", "192.0.2.1")
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", len(initial_neighbor_entries) - 1)

        # remove the IPv6 neighbor
        initial_neighbor_entries = set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP"))
        self.remove_neighbor("Ethernet104", "2001::1")
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", len(initial_neighbor_entries) - 1)

        time.sleep(3)

        # put Ethernet104 down
        self.set_interface_status(dvs, "Ethernet104", "down")

        time.sleep(3)

        # remove IP from interface
        self.remove_ip_address("Ethernet104", "2001::2/126")
        self.remove_ip_address("Ethernet104", "192.0.2.2/30")

        # remove interface
        initial_interface_entries = set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"))
        self.remove_l3_intf("Ethernet104")
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", len(initial_interface_entries) - 1)

    def test_AddRemoveSrv6MySidEnd(self, dvs, testlog):

        _, output = dvs.runcmd(f"vtysh -c 'show zebra dplane providers'")
        if 'dplane_fpm_sonic' not in output:
            pytest.skip("'dplane_fpm_sonic' required for this test is not available, skipping", allow_module_level=True)

        self.setup_srv6(dvs)

        # configure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:1::/64 block-len 32 node-len 16 func-bits 16\"")

        # create srv6 mysid end behavior
        dvs.runcmd("ip -6 route add fc00:0:1:64::/128 encap seg6local action End dev sr0")

        # check application database
        self.pdb.wait_for_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:1:64::")
        expected_fields = {"action": "end"}
        self.pdb.wait_for_field_match("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:1:64::", expected_fields)

        # verify that the mysid has been programmed into the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries) + 1)

        # check ASIC SAI_OBJECT_TYPE_MY_SID_ENTRY database
        my_sid = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", self.initial_my_sid_entries)
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(my_sid)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_E"

        # remove srv6 mysid end behavior
        dvs.runcmd("ip -6 route del fc00:0:1:64::/128 encap seg6local action End dev sr0")

        # check application database
        self.pdb.wait_for_deleted_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:1:64::")

        # verify that the mysid has been removed from the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries))

        # unconfigure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"no srv6\"")

        self.teardown_srv6(dvs)

    def test_AddRemoveSrv6MySidEndX(self, dvs, testlog):

        _, output = dvs.runcmd(f"vtysh -c 'show zebra dplane providers'")
        if 'dplane_fpm_sonic' not in output:
            pytest.skip("'dplane_fpm_sonic' required for this test is not available, skipping", allow_module_level=True)

        self.setup_srv6(dvs)

        # configure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:1::/64 block-len 32 node-len 16 func-bits 16\"")

        # create srv6 mysid end.x behavior
        dvs.runcmd("ip -6 route add fc00:0:1:65::/128 encap seg6local action End.X nh6 2001::1 dev sr0")

        # check application database
        self.pdb.wait_for_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:1:65::")
        expected_fields = {"action": "end.x", "adj": "2001::1"}
        self.pdb.wait_for_field_match("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:1:65::", expected_fields)

        # verify that the mysid has been programmed into the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries) + 1)

        # check ASIC SAI_OBJECT_TYPE_MY_SID_ENTRY database
        my_sid = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", self.initial_my_sid_entries)
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(my_sid)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_X"
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_NEXT_HOP_ID":
                assert fv[1] == self.next_hop_ipv6_id
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_PSP_AND_USD"

        # remove srv6 mysid end.x behavior
        dvs.runcmd("ip -6 route del fc00:0:1:65::/128 encap seg6local action End.X nh6 2001::1 dev sr0")

        # check application database
        self.pdb.wait_for_deleted_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:1:65::")

        # verify that the mysid has been removed from the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries))

        # unconfigure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"no srv6\"")

        self.teardown_srv6(dvs)

    @pytest.mark.skipif(LooseVersion(platform.release()) < LooseVersion('5.11'),
                        reason="This test requires Linux kernel 5.11 or higher")
    def test_AddRemoveSrv6MySidEndDT4(self, dvs, testlog):

        _, output = dvs.runcmd(f"vtysh -c 'show zebra dplane providers'")
        if 'dplane_fpm_sonic' not in output:
            pytest.skip("'dplane_fpm_sonic' required for this test is not available, skipping", allow_module_level=True)

        self.setup_srv6(dvs)

        # enable VRF strict mode
        dvs.runcmd("sysctl -w net.vrf.strict_mode=1")

        # configure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:1::/64 block-len 32 node-len 16 func-bits 16\"")

        # create srv6 mysid end.dt4 behavior
        dvs.runcmd("ip -6 route add fc00:0:1:6b::/128 encap seg6local action End.DT4 vrftable {} dev sr0".format(self.vrf_table_id))

        # check application database
        self.pdb.wait_for_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:1:6b::")

        # verify that the mysid has been programmed into the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries) + 1)

        # check ASIC SAI_OBJECT_TYPE_MY_SID_ENTRY database
        my_sid = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", self.initial_my_sid_entries)
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(my_sid)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT4"
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_VRF":
                assert fv[1] == self.vrf_id

        # remove srv6 mysid end.dt4 behavior
        dvs.runcmd("ip -6 route del fc00:0:1:6b::/128 encap seg6local action End.DT4 vrftable {} dev sr0".format(self.vrf_table_id))

        # check application database
        self.pdb.wait_for_deleted_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:1:6b::")

        # verify that the mysid has been removed from the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries))

        # unconfigure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"no srv6\"")

        self.teardown_srv6(dvs)

    def test_AddRemoveSrv6MySidEndDT6(self, dvs, testlog):

        _, output = dvs.runcmd(f"vtysh -c 'show zebra dplane providers'")
        if 'dplane_fpm_sonic' not in output:
            pytest.skip("'dplane_fpm_sonic' required for this test is not available, skipping", allow_module_level=True)

        self.setup_srv6(dvs)

        # configure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:1::/64 block-len 32 node-len 16 func-bits 16\"")

        # create srv6 mysid end.dt6 behavior
        dvs.runcmd("ip -6 route add fc00:0:1:6b::/128 encap seg6local action End.DT6 vrftable {} dev sr0".format(self.vrf_table_id))

        # check application database
        self.pdb.wait_for_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:1:6b::")
        expected_fields = {"action": "end.dt6", "vrf": "Vrf10"}
        self.pdb.wait_for_field_match("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:1:6b::", expected_fields)

        # verify that the mysid has been programmed into the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries) + 1)

        # check ASIC SAI_OBJECT_TYPE_MY_SID_ENTRY database
        my_sid = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", self.initial_my_sid_entries)
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(my_sid)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT6"
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_VRF":
                assert fv[1] == self.vrf_id

        # remove srv6 mysid end.dt6 behavior
        dvs.runcmd("ip -6 route del fc00:0:1:6b::/128 encap seg6local action End.DT6 vrftable {} dev sr0".format(self.vrf_table_id))

        # check application database
        self.pdb.wait_for_deleted_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:1:6b::")

        # verify that the mysid has been removed from the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries))

        # unconfigure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"no srv6\"")

        self.teardown_srv6(dvs)

    @pytest.mark.skipif(LooseVersion(platform.release()) < LooseVersion('5.14'),
                        reason="This test requires Linux kernel 5.14 or higher")
    def test_AddRemoveSrv6MySidEndDT46(self, dvs, testlog):

        _, output = dvs.runcmd(f"vtysh -c 'show zebra dplane providers'")
        if 'dplane_fpm_sonic' not in output:
            pytest.skip("'dplane_fpm_sonic' required for this test is not available, skipping", allow_module_level=True)

        self.setup_srv6(dvs)

        # enable VRF strict mode
        dvs.runcmd("sysctl -w net.vrf.strict_mode=1")

        # configure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:1::/64 block-len 32 node-len 16 func-bits 16\"")

        # create srv6 mysid end.dt46 behavior
        dvs.runcmd("ip -6 route add fc00:0:1:6b::/128 encap seg6local action End.DT46 vrftable {} dev sr0".format(self.vrf_table_id))

        # check application database
        self.pdb.wait_for_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:1:6b::")

        # verify that the mysid has been programmed into the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries) + 1)

        # check ASIC SAI_OBJECT_TYPE_MY_SID_ENTRY database
        my_sid = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", self.initial_my_sid_entries)
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(my_sid)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT46"
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_VRF":
                assert fv[1] == self.vrf_id

        # remove srv6 mysid end.dt46 behavior
        dvs.runcmd("ip -6 route del fc00:0:1:6b::/128 encap seg6local action End.DT46 vrftable {} dev sr0".format(self.vrf_table_id))

        # check application database
        self.pdb.wait_for_deleted_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:1:6b::")

        # verify that the mysid has been removed from the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries))

        # unconfigure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"no srv6\"")

        self.teardown_srv6(dvs)

    @pytest.mark.skipif(LooseVersion(platform.release()) < LooseVersion('6.1'),
                        reason="This test requires Linux kernel 6.1 or higher")
    def test_AddRemoveSrv6MySidUN(self, dvs, testlog):

        _, output = dvs.runcmd(f"vtysh -c 'show zebra dplane providers'")
        if 'dplane_fpm_sonic' not in output:
            pytest.skip("'dplane_fpm_sonic' required for this test is not available, skipping", allow_module_level=True)

        self.setup_srv6(dvs)

        # configure srv6 usid locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:2::/48 block-len 32 node-len 16 func-bits 16\" -c \"behavior usid\"")

        # create srv6 mysid un behavior
        dvs.runcmd("ip -6 route add fc00:0:2::/48 encap seg6local action End dev sr0")
        # dvs.runcmd("ip -6 route add fc00:0:2::/48 encap seg6local action End flavors next-csid lblen 32 nflen 16 dev sr0")

        # check application database
        self.pdb.wait_for_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:2::")

        # verify that the mysid has been programmed into the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries) + 1)

        # check ASIC SAI_OBJECT_TYPE_MY_SID_ENTRY database
        my_sid = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", self.initial_my_sid_entries)
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(my_sid)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UN"

        # remove srv6 mysid un behavior
        dvs.runcmd("ip -6 route del fc00:0:2::/48 encap seg6local action End dev sr0".format(self.vrf_table_id))
        # dvs.runcmd("ip -6 route del fc00:0:2::/48 encap seg6local action End flavors next-csid lblen 32 nflen 16 dev sr0".format(self.vrf_table_id))

        # check application database
        self.pdb.wait_for_deleted_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:2::")

        # verify that the mysid has been removed from the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries))

        # unconfigure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"no srv6\"")

        self.teardown_srv6(dvs)

    @pytest.mark.skipif(LooseVersion(platform.release()) < LooseVersion('6.6'),
                        reason="This test requires Linux kernel 6.6 or higher")
    def test_AddRemoveSrv6MySidUA(self, dvs, testlog):

        _, output = dvs.runcmd(f"vtysh -c 'show zebra dplane providers'")
        if 'dplane_fpm_sonic' not in output:
            pytest.skip("'dplane_fpm_sonic' required for this test is not available, skipping", allow_module_level=True)

        self.setup_srv6(dvs)

        # configure srv6 usid locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:2::/48 block-len 32 node-len 16 func-bits 16\" -c \"behavior usid\"")

        # create srv6 mysid ua behavior
        dvs.runcmd("ip -6 route add fc00:0:2:ff00::/64 encap seg6local action End.X nh6 2001::1 dev sr0")
        # dvs.runcmd("ip -6 route add fc00:0:2:ff00::/64 encap seg6local action End.X nh6 2001::1 flavors next-csid lblen 32 nflen 16 dev sr0")

        # check application database
        self.pdb.wait_for_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:2:ff00::")

        # verify that the mysid has been programmed into the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries) + 1)

        # check ASIC SAI_OBJECT_TYPE_MY_SID_ENTRY database
        my_sid = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", self.initial_my_sid_entries)
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(my_sid)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UA"
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_NEXT_HOP_ID":
                assert fv[1] == self.next_hop_ipv6_id

        # remove srv6 mysid ua behavior
        dvs.runcmd("ip -6 route del fc00:0:2:ff00::/64 encap seg6local action End.DT6 nh6 2001::1 dev sr0")
        # dvs.runcmd("ip -6 route del fc00:0:2:ff00::/64 encap seg6local action End.DT6 nh6 2001::1 flavors next-csid lblen 32 nflen 16 dev sr0")

        # check application database
        self.pdb.wait_for_deleted_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:2:ff00::")

        # verify that the mysid has been removed from the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries))

        # unconfigure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"no srv6\"")

        self.teardown_srv6(dvs)

    @pytest.mark.skipif(LooseVersion(platform.release()) < LooseVersion('5.11'),
                        reason="This test requires Linux kernel 5.11 or higher")
    def test_AddRemoveSrv6MySidUDT4(self, dvs, testlog):

        _, output = dvs.runcmd(f"vtysh -c 'show zebra dplane providers'")
        if 'dplane_fpm_sonic' not in output:
            pytest.skip("'dplane_fpm_sonic' required for this test is not available, skipping", allow_module_level=True)

        self.setup_srv6(dvs)

        # enable VRF strict mode
        dvs.runcmd("sysctl -w net.vrf.strict_mode=1")

        # configure srv6 usid locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:2::/48 block-len 32 node-len 16 func-bits 16\" -c \"behavior usid\"")

        # create srv6 mysid udt4 behavior
        dvs.runcmd("ip -6 route add fc00:0:2:ff05::/128 encap seg6local action End.DT4 vrftable {} dev sr0".format(self.vrf_table_id))

        # check application database
        self.pdb.wait_for_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:2:ff05::")

        # verify that the mysid has been programmed into the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries) + 1)

        # check ASIC SAI_OBJECT_TYPE_MY_SID_ENTRY database
        my_sid = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", self.initial_my_sid_entries)
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(my_sid)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT4"
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_VRF":
                assert fv[1] == self.vrf_id

        # remove srv6 mysid udt4 behavior
        dvs.runcmd("ip -6 route del fc00:0:2:ff05::/128 encap seg6local action End.DT4 vrftable {} dev sr0".format(self.vrf_table_id))

        # check application database
        self.pdb.wait_for_deleted_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:2:ff05::")

        # verify that the mysid has been removed from the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries))

        # unconfigure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"no srv6\"")

        self.teardown_srv6(dvs)

    def test_AddRemoveSrv6MySidUDT6(self, dvs, testlog):

        _, output = dvs.runcmd(f"vtysh -c 'show zebra dplane providers'")
        if 'dplane_fpm_sonic' not in output:
            pytest.skip("'dplane_fpm_sonic' required for this test is not available, skipping", allow_module_level=True)

        self.setup_srv6(dvs)

        # configure srv6 usid locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:2::/48 block-len 32 node-len 16 func-bits 16\" -c \"behavior usid\"")

        # create srv6 mysid udt6 behavior
        dvs.runcmd("ip -6 route add fc00:0:2:ff05::/128 encap seg6local action End.DT6 vrftable {} dev sr0".format(self.vrf_table_id))

        # check application database
        self.pdb.wait_for_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:2:ff05::")
        expected_fields = {"action": "udt6", "vrf": "Vrf10"}
        self.pdb.wait_for_field_match("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:2:ff05::", expected_fields)

        # verify that the mysid has been programmed into the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries) + 1)

        # check ASIC SAI_OBJECT_TYPE_MY_SID_ENTRY database
        my_sid = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", self.initial_my_sid_entries)
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(my_sid)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT6"
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_VRF":
                assert fv[1] == self.vrf_id

        # remove srv6 mysid udt6 behavior
        dvs.runcmd("ip -6 route del fc00:0:2:ff05::/128 encap seg6local action End.DT6 vrftable {} dev sr0".format(self.vrf_table_id))

        # check application database
        self.pdb.wait_for_deleted_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:2:ff05::")

        # verify that the mysid has been removed from the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries))

        # unconfigure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"no srv6\"")

        self.teardown_srv6(dvs)

    @pytest.mark.skipif(LooseVersion(platform.release()) < LooseVersion('5.14'),
                        reason="This test requires Linux kernel 5.14 or higher")
    def test_AddRemoveSrv6MySidUDT46(self, dvs, testlog):

        _, output = dvs.runcmd(f"vtysh -c 'show zebra dplane providers'")
        if 'dplane_fpm_sonic' not in output:
            pytest.skip("'dplane_fpm_sonic' required for this test is not available, skipping", allow_module_level=True)

        self.setup_srv6(dvs)

        # enable VRF strict mode
        dvs.runcmd("sysctl -w net.vrf.strict_mode=1")

        # configure srv6 usid locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:2::/48 block-len 32 node-len 16 func-bits 16\" -c \"behavior usid\"")

        # create srv6 mysid udt46 behavior
        dvs.runcmd("ip -6 route add fc00:0:2:ff05::/128 encap seg6local action End.DT46 vrftable {} dev sr0".format(self.vrf_table_id))

        # check application database
        self.pdb.wait_for_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:2:ff05::")

        # verify that the mysid has been programmed into the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries) + 1)

        # check ASIC SAI_OBJECT_TYPE_MY_SID_ENTRY database
        my_sid = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", self.initial_my_sid_entries)
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        (status, fvs) = tbl.get(my_sid)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR":
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_DT46"
            if fv[0] == "SAI_MY_SID_ENTRY_ATTR_VRF":
                assert fv[1] == self.vrf_id

        # remove srv6 mysid udt46 behavior
        dvs.runcmd("ip -6 route del fc00:0:2:ff05::/128 encap seg6local action End.DT46 vrftable {} dev sr0".format(self.vrf_table_id))

        # check application database
        self.pdb.wait_for_deleted_entry("SRV6_MY_SID_TABLE", "32:16:16:0:fc00:0:2:ff05::")

        # verify that the mysid has been removed from the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries))

        # unconfigure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"no srv6\"")

        self.teardown_srv6(dvs)


class TestSrv6VpnFpmsyncd(object):
    """ Functionality tests for SRv6 VPN handling in fpmsyncd """
   
    def setup_db(self, dvs):
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()
        self.cdb = dvs.get_config_db()

    def create_vrf(self, vrf_name):
        table = "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"
        existed_entries = get_exist_entries(self.adb.db_connection, table)

        self.cdb.create_entry("VRF", vrf_name, {"empty": "empty"})

        self.adb.wait_for_n_keys(table, len(existed_entries) + 1)
        return get_created_entry(self.adb.db_connection, table, existed_entries)

    def remove_vrf(self, vrf_name):
        self.cdb.delete_entry("VRF", vrf_name)

    def setup_srv6(self, dvs):
        self.setup_db(dvs)

        # create vrf
        initial_vrf_entries = set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))
        self.create_vrf("Vrf13")
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER", len(initial_vrf_entries) + 1)

        # create dummy interface sr0
        dvs.runcmd("ip link add sr0 type dummy")
        dvs.runcmd("ip link set sr0 up")

    def teardown_srv6(self, dvs):
        # remove dummy interface sr0
        dvs.runcmd("ip link del sr0 type dummy")

        # remove vrf
        initial_vrf_entries = set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))
        self.remove_vrf("Vrf13")
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER", len(initial_vrf_entries) - 1)

    @pytest.mark.xfail(reason="Failing after Bookworm/libnl 3.7.0 upgrade")
    def test_AddRemoveSrv6SteeringRouteIpv4(self, dvs, testlog):

        _, output = dvs.runcmd(f"vtysh -c 'show zebra dplane providers'")
        if 'dplane_fpm_sonic' not in output:
            pytest.skip("'dplane_fpm_sonic' required for this test is not available, skipping", allow_module_level=True)

        self.setup_srv6(dvs)

        dvs.runcmd("vtysh -c \"configure terminal\" -c \"interface lo\" -c \"ip address fc00:0:2::1/128\"")

        # configure srv6 usid locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:2::/48 block-len 32 node-len 16 func-bits 16\" -c \"behavior usid\"")

        # save exist asic db entries
        tunnel_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        nexthop_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        route_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        sidlist_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_SRV6_SIDLIST")

        # create v4 route with vpn sid
        dvs.runcmd("ip route add 192.0.2.0/24 encap seg6 mode encap segs fc00:0:1:e000:: dev sr0 vrf Vrf13")

        time.sleep(3)

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "Vrf13:192.0.2.0/24")
        expected_fields = {"segment": "Vrf13:192.0.2.0/24", "seg_src": "fc00:0:2::1"}
        self.pdb.wait_for_field_match("ROUTE_TABLE", "Vrf13:192.0.2.0/24", expected_fields)

        self.pdb.wait_for_entry("SRV6_SID_LIST_TABLE", "Vrf13:192.0.2.0/24")
        expected_fields = {"path": "fc00:0:1:e000::"}
        self.pdb.wait_for_field_match("SRV6_SID_LIST_TABLE", "Vrf13:192.0.2.0/24", expected_fields)

        # verify that the route has been programmed into the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", len(tunnel_entries) + 1)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", len(nexthop_entries) + 1)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SRV6_SIDLIST", len(sidlist_entries) + 1)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", len(route_entries) + 1)

        # get created entries
        route_key = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", route_entries)
        nexthop_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nexthop_entries)
        tunnel_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", tunnel_entries)
        sidlist_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_SRV6_SIDLIST", sidlist_entries)

        # check ASIC SAI_OBJECT_TYPE_SRV6_SIDLIST database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_SRV6_SIDLIST")
        (status, fvs) = tbl.get(sidlist_id)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_SRV6_SIDLIST_ATTR_SEGMENT_LIST":
                assert fv[1] == "1:fc00:0:1:e000::"
            elif fv[0] == "SAI_SRV6_SIDLIST_ATTR_TYPE":
                assert fv[1] == "SAI_SRV6_SIDLIST_TYPE_ENCAPS_RED"

        # check ASIC SAI_OBJECT_TYPE_ROUTE_ENTRY database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        (status, fvs) = tbl.get(route_key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID":
                assert fv[1] == nexthop_id

        # check ASIC SAI_OBJECT_TYPE_NEXT_HOP database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        (status, fvs) = tbl.get(nexthop_id)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_NEXT_HOP_ATTR_TYPE":
                assert fv[1] == "SAI_NEXT_HOP_TYPE_SRV6_SIDLIST"
            if fv[0] == "SAI_NEXT_HOP_ATTR_SRV6_SIDLIST_ID":
                assert fv[1] == sidlist_id
            elif fv[0] == "SAI_NEXT_HOP_ATTR_TUNNEL_ID":
                assert fv[1] == tunnel_id

        # check ASIC SAI_OBJECT_TYPE_TUNNEL database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        (status, fvs) = tbl.get(tunnel_id)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_TUNNEL_ATTR_TYPE":
                assert fv[1] == "SAI_TUNNEL_TYPE_SRV6"
            elif fv[0] == "SAI_TUNNEL_ATTR_ENCAP_SRC_IP":
                assert fv[1] == "fc00:0:2::1"

        # remove v4 route with vpn sid
        dvs.runcmd("ip route del 192.0.2.0/24 encap seg6 mode encap segs fc00:0:1:e000:: dev sr0 vrf Vrf13")

        time.sleep(3)

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "Vrf13:192.0.2.0/24")
        self.pdb.wait_for_deleted_entry("SRV6_SID_LIST_TABLE", "Vrf13:192.0.2.0/24")

        # verify that the route has been removed from the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", len(nexthop_entries))
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", len(tunnel_entries))
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", len(route_entries))

        # unconfigure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"no srv6\"")

        self.teardown_srv6(dvs)

    @pytest.mark.xfail(reason="Failing after Bookworm/libnl 3.7.0 upgrade")
    def test_AddRemoveSrv6SteeringRouteIpv6(self, dvs, testlog):

        _, output = dvs.runcmd(f"vtysh -c 'show zebra dplane providers'")
        if 'dplane_fpm_sonic' not in output:
            pytest.skip("'dplane_fpm_sonic' required for this test is not available, skipping", allow_module_level=True)

        self.setup_srv6(dvs)

        dvs.runcmd("vtysh -c \"configure terminal\" -c \"interface lo\" -c \"ip address fc00:0:2::1/128\"")

        # configure srv6 usid locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:2::/48 block-len 32 node-len 16 func-bits 16\" -c \"behavior usid\"")

        # save exist asic db entries
        tunnel_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        nexthop_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        route_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        sidlist_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_SRV6_SIDLIST")

        # create v6 route with vpn sid
        dvs.runcmd("ip -6 route add 2001:db8:1:1::/64 encap seg6 mode encap segs fc00:0:1:e000:: dev sr0 vrf Vrf13")

        time.sleep(3)

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "Vrf13:2001:db8:1:1::/64")
        expected_fields = {"segment": "Vrf13:2001:db8:1:1::/64", "seg_src": "fc00:0:2::1"}
        self.pdb.wait_for_field_match("ROUTE_TABLE", "Vrf13:2001:db8:1:1::/64", expected_fields)

        self.pdb.wait_for_entry("SRV6_SID_LIST_TABLE", "Vrf13:2001:db8:1:1::/64")
        expected_fields = {"path": "fc00:0:1:e000::"}
        self.pdb.wait_for_field_match("SRV6_SID_LIST_TABLE", "Vrf13:2001:db8:1:1::/64", expected_fields)

        # verify that the route has been programmed into the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", len(tunnel_entries) + 1)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", len(nexthop_entries) + 1)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SRV6_SIDLIST", len(sidlist_entries) + 1)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", len(route_entries) + 1)

        # get created entries
        route_key = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", route_entries)
        nexthop_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nexthop_entries)
        tunnel_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", tunnel_entries)
        sidlist_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_SRV6_SIDLIST", sidlist_entries)

        # check ASIC SAI_OBJECT_TYPE_SRV6_SIDLIST database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_SRV6_SIDLIST")
        (status, fvs) = tbl.get(sidlist_id)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_SRV6_SIDLIST_ATTR_SEGMENT_LIST":
                assert fv[1] == "1:fc00:0:1:e000::"
            elif fv[0] == "SAI_SRV6_SIDLIST_ATTR_TYPE":
                assert fv[1] == "SAI_SRV6_SIDLIST_TYPE_ENCAPS_RED"

        # check ASIC SAI_OBJECT_TYPE_ROUTE_ENTRY database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        (status, fvs) = tbl.get(route_key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID":
                assert fv[1] == nexthop_id

        # check ASIC SAI_OBJECT_TYPE_NEXT_HOP database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        (status, fvs) = tbl.get(nexthop_id)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_NEXT_HOP_ATTR_TYPE":
                assert fv[1] == "SAI_NEXT_HOP_TYPE_SRV6_SIDLIST"
            if fv[0] == "SAI_NEXT_HOP_ATTR_SRV6_SIDLIST_ID":
                assert fv[1] == sidlist_id
            elif fv[0] == "SAI_NEXT_HOP_ATTR_TUNNEL_ID":
                assert fv[1] == tunnel_id

        # check ASIC SAI_OBJECT_TYPE_TUNNEL database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        (status, fvs) = tbl.get(tunnel_id)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_TUNNEL_ATTR_TYPE":
                assert fv[1] == "SAI_TUNNEL_TYPE_SRV6"
            elif fv[0] == "SAI_TUNNEL_ATTR_ENCAP_SRC_IP":
                assert fv[1] == "fc00:0:2::1"

        # remove v4 route with vpn sid
        dvs.runcmd("ip route del 2001:db8:1:1::/64 encap seg6 mode encap segs fc00:0:1:e000:: dev sr0 vrf Vrf13")

        time.sleep(3)

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "Vrf13:2001:db8:1:1::/64")
        self.pdb.wait_for_deleted_entry("SRV6_SID_LIST_TABLE", "Vrf13:2001:db8:1:1::/64")

        # verify that the route has been removed from the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", len(nexthop_entries))
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", len(tunnel_entries))
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", len(route_entries))

        # unconfigure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"no srv6\"")

        self.teardown_srv6(dvs)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
