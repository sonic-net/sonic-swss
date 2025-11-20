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

def get_created_entries(db, table, existed_entries, number):
    tbl =  swsscommon.Table(db, table)
    entries = set(tbl.getKeys())
    new_entries = list(entries - existed_entries)
    assert len(new_entries) == number, "Wrong number of created entries."
    return new_entries

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
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDX4"
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
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDX6"
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

        fvs=swsscommon.FieldValuePairs([('seg_src',segsrc), ('segment',segname)])
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
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:1::/48 block-len 32 node-len 16 func-bits 16\"")

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
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:1::/48 block-len 32 node-len 16 func-bits 16\"")

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
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:1::/48 block-len 32 node-len 16 func-bits 16\"")

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
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:1::/48 block-len 32 node-len 16 func-bits 16\"")

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
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"srv6\" -c \"locators\" -c \"locator loc1\" -c \"prefix fc00:0:1::/48 block-len 32 node-len 16 func-bits 16\"")

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
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDT4"
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
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDT6"
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
                assert fv[1] == "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_UDT46"
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

    def verify_attribute_value(self, table, key, attribute, expected_value):
        (status, fvs) = table.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == attribute:
                assert fv[1] == expected_value

    def add_mysid_cfgdb(self, locator, addr, prefix="", dscp_mode="uniform", func_len=0):
        if not prefix:
            prefix = addr
        self.cdb.create_entry("SRV6_MY_LOCATORS", locator, {"prefix": prefix, "block_len": "32", "node_len": "16", "func_len": str(func_len), "arg_len": "0"})
        self.cdb.create_entry("SRV6_MY_SIDS", f'{locator}|{addr}/{48 + func_len}', {"decap_dscp_mode": dscp_mode})

    def remove_mysid_cfgdb(self, locator, addr, func_len=0):
        self.cdb.delete_entry("SRV6_MY_SIDS", f"{locator}|{addr}/{48 + func_len}")
        self.cdb.delete_entry("SRV6_MY_LOCATORS", locator)

    def add_mysid_vtysh(self, dvs, locator, addr, prefix="", func_len=0):
        if not prefix:
            prefix = addr
        loc_cmd = f'vtysh -c "configure terminal" -c "segment-routing" -c "srv6" -c "locators" -c "locator {locator}" -c "prefix {prefix}/48 block-len 32 node-len 16 func-bits {func_len}" -c "behavior usid"'
        sid_cmd = f'vtysh -c "configure terminal" -c "segment-routing" -c "srv6" -c "static-sids" -c "sid {addr}/{48 + func_len} locator {locator} behavior uN"'
        dvs.runcmd(loc_cmd)
        dvs.runcmd(sid_cmd)

    def remove_mysid_vtysh(self, dvs, locator, addr, func_len=0):
        sid_cmd = f'vtysh -c "configure terminal" -c "segment-routing" -c "srv6" -c "static-sids" -c "no sid {addr}/{48 + func_len} locator {locator} behavior uN"'
        loc_cmd = f'vtysh -c "configure terminal" -c "segment-routing" -c "srv6" -c "locators" -c "no locator {locator}"'
        dvs.runcmd(sid_cmd)
        dvs.runcmd(loc_cmd)

    def test_Srv6MySidUNTunnelDscpMode(self, dvs, testlog):

        _, output = dvs.runcmd(f"vtysh -c 'show zebra dplane providers'")
        if 'dplane_fpm_sonic' not in output:
            pytest.skip("'dplane_fpm_sonic' required for this test is not available, skipping", allow_module_level=True)

        self.setup_srv6(dvs)

        my_sid_table = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY")
        tunnel_table = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        tunnel_term_table = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY")
        tunnel_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        tunnel_term_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY")

        mysid1 = "fc00:0:1::"
        mysid2 = "fd00:0:1::"
        mysid3 = "fe00:0:1:aabb::"

        # Confiure the dcsp_mode in config db
        self.add_mysid_cfgdb("loc1", mysid1, dscp_mode="uniform")
        self.add_mysid_cfgdb("loc2", mysid2, dscp_mode="pipe")
        self.add_mysid_cfgdb("loc3", mysid3, "fe00:0:1::", dscp_mode="pipe", func_len=16)

        # Create MySID entry with dscp_mode uniformß
        self.add_mysid_vtysh(dvs, "loc1", mysid1)

        self.pdb.wait_for_entry("SRV6_MY_SID_TABLE", f"32:16:0:0:{mysid1}")
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries) + 1)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", len(tunnel_entries) + 1)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY", len(tunnel_term_entries) + 1)
        my_sid_uniform = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", self.initial_my_sid_entries)
        tunnel_uniform = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", tunnel_entries)
        mysid_uniform_term_entry = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY", tunnel_term_entries)

        # Create MySID entry with dscp_mode pipe
        self.add_mysid_vtysh(dvs, "loc2", mysid2)

        self.pdb.wait_for_entry("SRV6_MY_SID_TABLE", f"32:16:0:0:{mysid2}")
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries) + 2)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", len(tunnel_entries) + 2)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY", len(tunnel_term_entries) + 2)
        my_sid_pipe = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", self.initial_my_sid_entries | set([my_sid_uniform]))
        tunnel_pipe = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", tunnel_entries | set([tunnel_uniform]))
        mysid_pipe_term_entry = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY", tunnel_term_entries | set([mysid_uniform_term_entry]))

        # Validate tunnel DSCP mode configuration
        self.verify_attribute_value(my_sid_table, my_sid_uniform, "SAI_MY_SID_ENTRY_ATTR_TUNNEL_ID", tunnel_uniform)
        self.verify_attribute_value(my_sid_table, my_sid_pipe, "SAI_MY_SID_ENTRY_ATTR_TUNNEL_ID", tunnel_pipe)
        self.verify_attribute_value(tunnel_table, tunnel_uniform, "SAI_TUNNEL_ATTR_DECAP_DSCP_MODE", "SAI_TUNNEL_DSCP_MODE_UNIFORM_MODEL")
        self.verify_attribute_value(tunnel_table, tunnel_pipe, "SAI_TUNNEL_ATTR_DECAP_DSCP_MODE", "SAI_TUNNEL_DSCP_MODE_PIPE_MODEL")
        self.verify_attribute_value(tunnel_table, tunnel_uniform, "SAI_TUNNEL_ATTR_DECAP_TTL_MODE", "SAI_TUNNEL_TTL_MODE_PIPE_MODEL")
        self.verify_attribute_value(tunnel_table, tunnel_pipe, "SAI_TUNNEL_ATTR_DECAP_TTL_MODE", "SAI_TUNNEL_TTL_MODE_PIPE_MODEL")
        self.verify_attribute_value(my_sid_table, my_sid_uniform, "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR", "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_USD")
        self.verify_attribute_value(my_sid_table, my_sid_pipe, "SAI_MY_SID_ENTRY_ATTR_ENDPOINT_BEHAVIOR_FLAVOR", "SAI_MY_SID_ENTRY_ENDPOINT_BEHAVIOR_FLAVOR_USD")

        # Validate tunnel term configuration
        self.verify_attribute_value(tunnel_term_table, mysid_uniform_term_entry, "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID", tunnel_uniform)
        self.verify_attribute_value(tunnel_term_table, mysid_uniform_term_entry, "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP", mysid1)
        self.verify_attribute_value(tunnel_term_table, mysid_pipe_term_entry, "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID", tunnel_pipe)
        self.verify_attribute_value(tunnel_term_table, mysid_pipe_term_entry, "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP", mysid2)

        # Add another MySID entry with dscp_mode pipe
        self.add_mysid_vtysh(dvs, "loc3", mysid3, prefix="fe00:0:1::", func_len=16)

        self.pdb.wait_for_entry("SRV6_MY_SID_TABLE", f"32:16:16:0:{mysid3}")
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries) + 3)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY", len(tunnel_term_entries) + 3)

        # Verify that the tunnel is reused
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", len(tunnel_entries) + 2)
        my_sid_pipe2 = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", self.initial_my_sid_entries | set([my_sid_uniform, my_sid_pipe]))
        self.verify_attribute_value(my_sid_table, my_sid_pipe2, "SAI_MY_SID_ENTRY_ATTR_TUNNEL_ID", tunnel_pipe)

        # Remove MySID entries
        self.remove_mysid_vtysh(dvs, "loc1", mysid1)
        self.remove_mysid_vtysh(dvs, "loc2", mysid2)
        self.remove_mysid_vtysh(dvs, "loc3", mysid3, func_len=16)

        self.pdb.wait_for_deleted_entry("SRV6_MY_SID_TABLE", f"32:16:0:0:{mysid1}")
        self.pdb.wait_for_deleted_entry("SRV6_MY_SID_TABLE", f"32:16:0:0:{mysid2}")
        self.pdb.wait_for_deleted_entry("SRV6_MY_SID_TABLE", f"32:16:16:0:{mysid3}")

        self.remove_mysid_cfgdb("loc1", mysid1)
        self.remove_mysid_cfgdb("loc2", mysid2)
        self.remove_mysid_cfgdb("loc3", mysid3, func_len=16)

        # Verify that the MySID and tunnel configuration is removed
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries))
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", len(tunnel_entries))
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(tunnel_term_entries))

        # Unconfigure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"no srv6\"")

        self.teardown_srv6(dvs)

    def test_Srv6MySidUNTunnelDscpModeAmbiguity(self, dvs, testlog):
        _, output = dvs.runcmd(f"vtysh -c 'show zebra dplane providers'")
        if 'dplane_fpm_sonic' not in output:
            pytest.skip("'dplane_fpm_sonic' required for this test is not available, skipping", allow_module_level=True)

        self.setup_srv6(dvs)

        tunnel_table = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        tunnel_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")

        loc1_prefix = "aaaa:bbbb:1::"
        loc2_prefix = "aaaa:bbbb:1:2::"
        sid_addr = "aaaa:bbbb:1:2::/64"

        # Add locator 1
        dvs.runcmd(f'vtysh -c "configure terminal" -c "segment-routing" -c "srv6" -c "locators" -c "locator loc1" -c "prefix {loc1_prefix}/48 block-len 32 node-len 16 func-bits 16" -c "behavior usid"')
        self.cdb.create_entry("SRV6_MY_LOCATORS", "loc1", {"prefix": loc1_prefix, "block_len": "32", "node_len": "16", "func_len": "16", "arg_len": "0"})

        # Add locator 2
        dvs.runcmd(f'vtysh -c "configure terminal" -c "segment-routing" -c "srv6" -c "locators" -c "locator loc2" -c "prefix {loc2_prefix}/64 block-len 32 node-len 32 func-bits 0" -c "behavior usid"')
        self.cdb.create_entry("SRV6_MY_LOCATORS", "loc2", {"prefix": loc2_prefix, "block_len": "32", "node_len": "32", "func_len": "0", "arg_len": "0"})

        # Add SIDs CONIFG_DB
        self.cdb.create_entry("SRV6_MY_SIDS", f'loc1|{sid_addr}', {"decap_dscp_mode": "uniform"})
        self.cdb.create_entry("SRV6_MY_SIDS", f'loc2|{sid_addr}', {"decap_dscp_mode": "pipe"})

        # Add first SID
        dvs.runcmd(f'vtysh -c "configure terminal" -c "segment-routing" -c "srv6" -c "static-sids" -c "sid {sid_addr} locator loc1 behavior uN"')

        self.pdb.wait_for_entry("SRV6_MY_SID_TABLE", f"32:16:16:0:aaaa:bbbb:1:2::")
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries) + 1)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", len(tunnel_entries) + 1)
        tunnel_uniform = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", tunnel_entries)

        # Add second SID
        dvs.runcmd(f'vtysh -c "configure terminal" -c "segment-routing" -c "srv6" -c "static-sids" -c "sid {sid_addr} locator loc2 behavior uN"')

        self.pdb.wait_for_entry("SRV6_MY_SID_TABLE", f"32:32:0:0:aaaa:bbbb:1:2::")
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MY_SID_ENTRY", len(self.initial_my_sid_entries) + 2)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", len(tunnel_entries) + 2)
        tunnel_pipe = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", tunnel_entries | set([tunnel_uniform]))

        self.verify_attribute_value(tunnel_table, tunnel_uniform, "SAI_TUNNEL_ATTR_DECAP_DSCP_MODE", "SAI_TUNNEL_DSCP_MODE_UNIFORM_MODEL")
        self.verify_attribute_value(tunnel_table, tunnel_pipe, "SAI_TUNNEL_ATTR_DECAP_DSCP_MODE", "SAI_TUNNEL_DSCP_MODE_PIPE_MODEL")
        self.verify_attribute_value(tunnel_table, tunnel_uniform, "SAI_TUNNEL_ATTR_DECAP_TTL_MODE", "SAI_TUNNEL_TTL_MODE_PIPE_MODEL")
        self.verify_attribute_value(tunnel_table, tunnel_pipe, "SAI_TUNNEL_ATTR_DECAP_TTL_MODE", "SAI_TUNNEL_TTL_MODE_PIPE_MODEL")

        # Cleanup
        dvs.runcmd(f'vtysh -c "configure terminal" -c "segment-routing" -c "srv6" -c "static-sids" -c "no sid {sid_addr} locator loc1 behavior uN"')
        dvs.runcmd(f'vtysh -c "configure terminal" -c "segment-routing" -c "srv6" -c "static-sids" -c "no sid {sid_addr} locator loc2 behavior uN"')
        dvs.runcmd(f'vtysh -c "configure terminal" -c "segment-routing" -c "srv6" -c "locators" -c "no locator loc1"')
        dvs.runcmd(f'vtysh -c "configure terminal" -c "segment-routing" -c "srv6" -c "locators" -c "no locator loc2"')

        self.cdb.delete_entry("SRV6_MY_SIDS", f'loc1|{sid_addr}')
        self.cdb.delete_entry("SRV6_MY_SIDS", f'loc2|{sid_addr}')
        self.cdb.delete_entry("SRV6_MY_LOCATORS", "loc1")
        self.cdb.delete_entry("SRV6_MY_LOCATORS", "loc2")

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
        expected_fields = {"segment": "fc00:0:1:e000::", "seg_src": "fc00:0:2::1"}
        self.pdb.wait_for_field_match("ROUTE_TABLE", "Vrf13:192.0.2.0/24", expected_fields)

        self.pdb.wait_for_entry("SRV6_SID_LIST_TABLE", "fc00:0:1:e000::")
        expected_fields = {"path": "fc00:0:1:e000::"}
        self.pdb.wait_for_field_match("SRV6_SID_LIST_TABLE", "fc00:0:1:e000::", expected_fields)

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
        self.pdb.wait_for_deleted_entry("SRV6_SID_LIST_TABLE", "fc00:0:1:e000::")

        # verify that the route has been removed from the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", len(nexthop_entries))
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", len(tunnel_entries))
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", len(route_entries))

        # unconfigure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"no srv6\"")

        self.teardown_srv6(dvs)

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
        expected_fields = {"segment": "fc00:0:1:e000::", "seg_src": "fc00:0:2::1"}
        self.pdb.wait_for_field_match("ROUTE_TABLE", "Vrf13:2001:db8:1:1::/64", expected_fields)

        self.pdb.wait_for_entry("SRV6_SID_LIST_TABLE", "fc00:0:1:e000::")
        expected_fields = {"path": "fc00:0:1:e000::"}
        self.pdb.wait_for_field_match("SRV6_SID_LIST_TABLE", "fc00:0:1:e000::", expected_fields)

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
        self.pdb.wait_for_deleted_entry("SRV6_SID_LIST_TABLE", "fc00:0:1:e000::")

        # verify that the route has been removed from the ASIC
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", len(nexthop_entries))
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", len(tunnel_entries))
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", len(route_entries))

        # unconfigure srv6 locator
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"segment-routing\" -c \"no srv6\"")

        self.teardown_srv6(dvs)

class TestSrv6Vpn(object):
    def setup_db(self, dvs):
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()
        self.cdb = dvs.get_config_db()

    def create_srv6_vpn_route(self, routeip, nexthop, segsrc, vpn_sid, ifname):
        table = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"
        existed_entries = get_exist_entries(self.adb.db_connection, table)

        fvs=swsscommon.FieldValuePairs([('seg_src', segsrc), ('nexthop', nexthop), ('vpn_sid', vpn_sid), ('ifname', ifname)])
        routetbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "ROUTE_TABLE")
        routetbl.set(routeip,fvs)

        self.adb.wait_for_n_keys(table, len(existed_entries) + 1)
        return get_created_entry(self.adb.db_connection, table, existed_entries)

    def create_srv6_vpn_route_with_nhg(self, routeip, nhg_index, pic_ctx_index):
        table = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"
        existed_entries = get_exist_entries(self.adb.db_connection, table)

        fvs=swsscommon.FieldValuePairs([('nexthop_group', nhg_index), ('pic_context_id', pic_ctx_index)])
        routetbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "ROUTE_TABLE")
        routetbl.set(routeip,fvs)

        self.adb.wait_for_n_keys(table, len(existed_entries) + 1)
        return get_created_entry(self.adb.db_connection, table, existed_entries)
    
    def update_srv6_vpn_route_attribute_with_nhg(self, routeip, nhg_index, pic_ctx_index):
        fvs=swsscommon.FieldValuePairs([('nexthop_group', nhg_index), ('pic_context_id', pic_ctx_index)])
        routetbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "ROUTE_TABLE")
        routetbl.set(routeip,fvs)
        return True

    def update_srv6_vpn_route_attribute(self, routeip, nexthops, segsrc_list, vpn_list, ifname_list):
        fvs=swsscommon.FieldValuePairs([('seg_src', ",".join(segsrc_list)), ('nexthop', ",".join(nexthops)), ('vpn_sid', ",".join(vpn_list)), ('ifname', ",".join(ifname_list))])
        routetbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "ROUTE_TABLE")
        routetbl.set(routeip,fvs)
        return True

    def remove_srv6_route(self, routeip):
        routetbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "ROUTE_TABLE")
        routetbl._del(routeip)

    def create_nhg(self, nhg_index, nexthops, segsrc_list, ifname_list):
        table = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER"
        existed_entries = get_exist_entries(self.adb.db_connection, table)

        fvs=swsscommon.FieldValuePairs([('seg_src', ",".join(segsrc_list)), ('nexthop', ",".join(nexthops)), ('ifname', ",".join(ifname_list))])
        nhgtbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "NEXTHOP_GROUP_TABLE")
        nhgtbl.set(nhg_index,fvs)

        self.adb.wait_for_n_keys(table, len(existed_entries) + len(nexthops))
        return get_created_entries(self.adb.db_connection, table, existed_entries, len(nexthops))
    
    def remove_nhg(self, nhg_index):
        nhgtbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "NEXTHOP_GROUP_TABLE")
        nhgtbl._del(nhg_index)

    def create_pic_context(self, pic_ctx_id, nexthops, vpn_list):
        table = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY"
        existed_entries = get_exist_entries(self.adb.db_connection, table)

        fvs=swsscommon.FieldValuePairs([('nexthop', ",".join(nexthops)), ('vpn_sid', ",".join(vpn_list))])
        pictbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "PIC_CONTEXT_TABLE")
        pictbl.set(pic_ctx_id,fvs)

        self.adb.wait_for_n_keys(table, len(existed_entries) + len(vpn_list))
        return get_created_entries(self.adb.db_connection, table, existed_entries, len(vpn_list))
    
    def remove_pic_context(self, pic_ctx_id):
        pictbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "PIC_CONTEXT_TABLE")
        pictbl._del(pic_ctx_id)

    def check_deleted_route_entries(self, destinations):
        def _access_function():
            route_entries = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            route_destinations = [json.loads(route_entry)["dest"] for route_entry in route_entries]
            return (all(destination not in route_destinations for destination in destinations), None)

        wait_for_result(_access_function)

    def test_srv6_vpn_with_single_nh(self, dvs, testlog):
        self.setup_db(dvs)
        dvs.setup_db()

        # save exist asic db entries
        tunnel_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        nexthop_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        map_entry_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")
        map_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP")

        # create v4 route with vpn sid
        route_key = self.create_srv6_vpn_route('5000::/64', '2001::1', '1001:2000::1', '3000::1', 'unknown')
        nexthop_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nexthop_entries)
        tunnel_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", tunnel_entries)
        map_entry_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY", map_entry_entries)
        map_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP", map_entries)
        prefix_agg_id = "1"

        # check ASIC SAI_OBJECT_TYPE_ROUTE_ENTRY database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        (status, fvs) = tbl.get(route_key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_ROUTE_ENTRY_ATTR_PREFIX_AGG_ID":
                assert prefix_agg_id == fv[1]

        # check ASIC SAI_OBJECT_TYPE_TUNNEL_MAP database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP")
        (status, fvs) = tbl.get(map_id)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_TUNNEL_MAP_ATTR_TYPE":
                assert fv[1] == "SAI_TUNNEL_MAP_TYPE_PREFIX_AGG_ID_TO_SRV6_VPN_SID"

        # check ASIC SAI_OBJECT_TYPE_TUNNEL database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        (status, fvs) = tbl.get(tunnel_id)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_TUNNEL_ATTR_PEER_MODE":
                assert fv[1] == "SAI_TUNNEL_PEER_MODE_P2P"

        # check vpn sid value in SRv6 route is created
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")
        (status, fvs) = tbl.get(map_entry_id)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_TUNNEL_MAP_ENTRY_ATTR_SRV6_VPN_SID_VALUE":
                assert fv[1] == "3000::1"
            if fv[0] == "SAI_TUNNEL_MAP_ENTRY_ATTR_PREFIX_AGG_ID_KEY":
                assert fv[1] == prefix_agg_id

        # check sid list value in ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP is created
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        (status, fvs) = tbl.get(nexthop_id)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_NEXT_HOP_ATTR_TYPE":
                assert fv[1] == "SAI_NEXT_HOP_TYPE_SRV6_SIDLIST"

        self.remove_srv6_route('5000::/64')
        self.check_deleted_route_entries('5000::/64')
        time.sleep(5)
        # check ASIC SAI_OBJECT_TYPE_TUNNEL_MAP is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP")
        (status, fvs) = tbl.get(map_id)
        assert status == False

        # check ASIC SAI_OBJECT_TYPE_TUNNEL is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        (status, fvs) = tbl.get(tunnel_id)
        assert status == False

        # check vpn sid value in SRv6 route is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")
        (status, fvs) = tbl.get(map_entry_id)
        assert status == False

        # check nexthop id in SRv6 route is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        (status, fvs) = tbl.get(nexthop_id)
        assert status == False

        # check ASIC SAI_OBJECT_TYPE_ROUTE_ENTRY is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        (status, fvs) = tbl.get(route_key)
        assert status == False

    def test_pic(self, dvs, testlog):
        self.setup_db(dvs)
        dvs.setup_db()

        segsrc_list = []
        nexthop_list = []
        ifname_list = []
        vpn_list = []
        nhg_index = '100'
        pic_ctx_index = '200'

        # save exist asic db entries
        tunnel_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        nexthop_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        map_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP")
        nexthop_group_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")
        nexthop_group_member_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
        map_entry_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")

        segsrc_list.append('1001:2000::1')
        segsrc_list.append('1001:2000::1')

        nexthop_list.append('2000::1')
        nexthop_list.append('2000::2')

        ifname_list.append('unknown')
        ifname_list.append('unknown')

        vpn_list.append('3000::1')
        vpn_list.append('3000::2')

        self.create_nhg(nhg_index, nexthop_list, segsrc_list, ifname_list)
        tunnel_ids = get_created_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", tunnel_entries, 2)
        nh_ids = get_created_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nexthop_entries, 2)
        nhg_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP", nexthop_group_entries)
        nhg_mem = get_created_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER", nexthop_group_member_entries, 2)
        map_ids = get_created_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP", map_entries, 2)

        nh_ids = sorted(nh_ids)
        nhg_mem = sorted(nhg_mem)

        # check ASIC SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
        (status, fvs) = tbl.get(nhg_mem[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID":
                assert fv[1] == nhg_id
            elif fv[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID":
                assert fv[1] == nh_ids[0]

        # check ASIC SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
        (status, fvs) = tbl.get(nhg_mem[1])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID":
                assert fv[1] == nhg_id
            elif fv[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID":
                assert fv[1] == nh_ids[1]

        # check ASIC SAI_OBJECT_TYPE_TUNNEL_MAP database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP")
        for map_id in map_ids:
            (status, fvs) = tbl.get(map_id)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_TUNNEL_MAP_ATTR_TYPE":
                    assert fv[1] == "SAI_TUNNEL_MAP_TYPE_PREFIX_AGG_ID_TO_SRV6_VPN_SID"

        # check ASIC SAI_OBJECT_TYPE_TUNNEL database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        for tunnel_id in tunnel_ids:
            (status, fvs) = tbl.get(tunnel_id)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_TUNNEL_ATTR_PEER_MODE":
                    assert fv[1] == "SAI_TUNNEL_PEER_MODE_P2P"

        # check sid list value in ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP is created
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        for nh_id in nh_ids:
            (status, fvs) = tbl.get(nh_id)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_NEXT_HOP_ATTR_TYPE":
                    assert fv[1] == "SAI_NEXT_HOP_TYPE_SRV6_SIDLIST"

        self.create_pic_context(pic_ctx_index, nexthop_list, vpn_list)
        map_entry_ids = get_created_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY", map_entry_entries, 2)
        prefix_agg_id = "1"

        # check vpn sid value in SRv6 route is created
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")
        for map_entry_id in map_entry_ids:
            (status, fvs) = tbl.get(map_entry_id)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_TUNNEL_MAP_ENTRY_ATTR_PREFIX_AGG_ID_KEY":
                    assert fv[1] == prefix_agg_id

        # remove nhg and pic_context
        self.remove_nhg(nhg_index)
        self.remove_pic_context(pic_ctx_index)

        time.sleep(5)
        # check ASIC SAI_OBJECT_TYPE_NEXT_HOP_GROUP is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")
        (status, fvs) = tbl.get(nhg_id)
        assert status == False

        # check ASIC SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
        for nhg_mem_id in nhg_mem:
            (status, fvs) = tbl.get(nhg_mem_id)
            assert status == False

        # check ASIC SAI_OBJECT_TYPE_TUNNEL_MAP is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP")
        for map_id in map_ids:
            (status, fvs) = tbl.get(map_id)
            assert status == False

        # check ASIC SAI_OBJECT_TYPE_TUNNEL is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        for tunnel_id in tunnel_ids:
            (status, fvs) = tbl.get(tunnel_id)
            assert status == False

        # check next hop in ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        for nh_id in nh_ids:
            (status, fvs) = tbl.get(nh_id)
            assert status == False

        # check vpn sid value in SRv6 route is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")
        for map_entry_id in map_entry_ids:
            (status, fvs) = tbl.get(map_entry_id)
            assert status == False

    def test_srv6_vpn_with_nhg(self, dvs, testlog):
        self.setup_db(dvs)
        dvs.setup_db()

        segsrc_list = []
        nexthop_list = []
        vpn_list = []
        ifname_list = []
        nhg_index = '100'
        pic_ctx_index = '200'

        # save exist asic db entries
        nexthop_group_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")

        segsrc_list.append('1001:2000::1')
        segsrc_list.append('1001:2000::1')

        nexthop_list.append('2000::1')
        nexthop_list.append('2000::2')

        vpn_list.append('3000::1')
        vpn_list.append('3000::2')

        ifname_list.append('unknown')
        ifname_list.append('unknown')

        self.create_nhg(nhg_index, nexthop_list, segsrc_list, ifname_list)
        self.create_pic_context(pic_ctx_index, nexthop_list, vpn_list)
        route_key = self.create_srv6_vpn_route_with_nhg('5000::/64', nhg_index, pic_ctx_index)
        
        nhg_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP", nexthop_group_entries)
        prefix_agg_id = "1"

        # check ASIC SAI_OBJECT_TYPE_ROUTE_ENTRY database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        (status, fvs) = tbl.get(route_key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID":
                assert fv[1] == nhg_id
            if fv[0] == "SAI_ROUTE_ENTRY_ATTR_PREFIX_AGG_ID":
                assert fv[1] == prefix_agg_id

        route_key_new = self.create_srv6_vpn_route_with_nhg('5001::/64', nhg_index, pic_ctx_index)
        
        # check ASIC SAI_OBJECT_TYPE_ROUTE_ENTRY database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        (status, fvs) = tbl.get(route_key_new)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID":
                assert fv[1] == nhg_id
            if fv[0] == "SAI_ROUTE_ENTRY_ATTR_PREFIX_AGG_ID":
                assert fv[1] == prefix_agg_id

        # remove routes
        self.remove_srv6_route('5001::/64')
        self.check_deleted_route_entries('5001::/64')

        time.sleep(5)
        # check ASIC SAI_OBJECT_TYPE_ROUTE_ENTRY is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        (status, fvs) = tbl.get(route_key_new)
        assert status == False

        # remove routes
        self.remove_srv6_route('5000::/64')
        self.check_deleted_route_entries('5000::/64')

        time.sleep(5)
        # check ASIC SAI_OBJECT_TYPE_ROUTE_ENTRY is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        (status, fvs) = tbl.get(route_key)
        assert status == False

        # remove nhg and pic_context
        self.remove_nhg(nhg_index)
        self.remove_pic_context(pic_ctx_index)

    def test_srv6_vpn_nh_update(self, dvs, testlog):
        self.setup_db(dvs)
        dvs.setup_db()

        segsrc_list = []
        nexthop_list = []
        vpn_list = []
        ifname_list = []

        # save exist asic db entries
        nexthop_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        map_entry_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")

        nexthop_group_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")
        nexthop_group_member_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
        map_entry_prefix_agg_id = "1"
        route_entry_prefix_agg_id = "1"
        route_entry_next_hop_id = "1"

        # create v4 route with vpn sid
        route_key = self.create_srv6_vpn_route('5000::/64', '2000::1', '1001:2000::1', '3000::1', 'unknown')
        map_entry_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY", map_entry_entries)

        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")
        (status, fvs) = tbl.get(map_entry_id)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_TUNNEL_MAP_ENTRY_ATTR_PREFIX_AGG_ID_KEY":
                map_entry_prefix_agg_id = fv[1]

        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        (status, fvs) = tbl.get(route_key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID":
                route_entry_next_hop_id = fv[1]
            if fv[0] == "SAI_ROUTE_ENTRY_ATTR_PREFIX_AGG_ID":
                route_entry_prefix_agg_id = fv[1]

        segsrc_list.append('1001:2000::1')
        segsrc_list.append('1001:2000::1')

        nexthop_list.append('2000::1')
        nexthop_list.append('2000::2')

        vpn_list.append('3000::1')
        vpn_list.append('3000::2')

        ifname_list.append('unknown')
        ifname_list.append('unknown')

        nhg_index = '100'
        pic_ctx_index = '200'

        map_entry_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")

        self.create_nhg(nhg_index, nexthop_list, segsrc_list, ifname_list)
        self.create_pic_context(pic_ctx_index, nexthop_list, vpn_list)
        self.update_srv6_vpn_route_attribute_with_nhg('5000::/64', nhg_index, pic_ctx_index)

        time.sleep(5)
        nh_ids = get_created_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nexthop_entries, 2)
        nhg_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP", nexthop_group_entries)
        nhg_mem = get_created_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER", nexthop_group_member_entries, 2)

        map_entry_ids = get_created_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY", map_entry_entries, 2)
        map_entry_id_group = "1"

        for map_id in map_entry_ids:
            if map_id != map_entry_id:
                map_entry_id_group = map_id
                break

        nh_ids = sorted(nh_ids)
        nhg_mem = sorted(nhg_mem)

        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")
        (status, fvs) = tbl.get(map_entry_id_group)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_TUNNEL_MAP_ENTRY_ATTR_PREFIX_AGG_ID_KEY":
                assert fv[1] != map_entry_prefix_agg_id

        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")
        (status, fvs) = tbl.get(nhg_id)
        assert status == True

        # check ASIC SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
        (status, fvs) = tbl.get(nhg_mem[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID":
                assert fv[1] == nhg_id
            elif fv[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID":
                assert fv[1] == nh_ids[0]

        # check ASIC SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER database
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
        (status, fvs) = tbl.get(nhg_mem[1])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID":
                assert fv[1] == nhg_id
            elif fv[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID":
                assert fv[1] == nh_ids[1]

        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        (status, fvs) = tbl.get(route_key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID":
                assert fv[1] != route_entry_next_hop_id
            if fv[0] == "SAI_ROUTE_ENTRY_ATTR_PREFIX_AGG_ID":
                assert fv[1] != route_entry_prefix_agg_id

        # remove routes
        self.remove_srv6_route('5000::/64')
        self.check_deleted_route_entries('5000::/64')
        time.sleep(5)

        # remove nhg and pic_context
        self.remove_nhg(nhg_index)
        self.remove_pic_context(pic_ctx_index)

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
