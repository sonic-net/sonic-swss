import os
import re
import time
import json
import pytest

from swsscommon import swsscommon
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

    def test_mysid(self, dvs, testlog):
        self.setup_db(dvs)

        # create MySID entries
        mysid1='16:8:8:8:baba:2001:10::'
        mysid2='16:8:8:8:baba:2001:20::'
        mysid3='16:8:8:8:fcbb:bb01:800::'
        mysid4='16:8:8:8:baba:2001:40::'

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

        # delete MySID
        self.remove_mysid(mysid1)
        self.remove_mysid(mysid2)
        self.remove_mysid(mysid3)
        self.remove_mysid(mysid4)

        # remove vrf
        self.remove_vrf("VrfDt46")

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

    def create_srv6_vpn_route_with_nhg(self, routeip, nexthops, segsrc_list, vpn_list, ifname_list):
        table = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"
        existed_entries = get_exist_entries(self.adb.db_connection, table)

        fvs=swsscommon.FieldValuePairs([('seg_src', ",".join(segsrc_list)), ('nexthop', ",".join(nexthops)), ('vpn_sid', ",".join(vpn_list)), ('ifname', ",".join(ifname_list))])
        routetbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "ROUTE_TABLE")
        routetbl.set(routeip,fvs)

        self.adb.wait_for_n_keys(table, len(existed_entries) + 1)
        return get_created_entry(self.adb.db_connection, table, existed_entries)

    def update_srv6_vpn_route_attribute(self, routeip, nexthops, segsrc_list, vpn_list, ifname_list):
        fvs=swsscommon.FieldValuePairs([('seg_src', ",".join(segsrc_list)), ('nexthop', ",".join(nexthops)), ('vpn_sid', ",".join(vpn_list)), ('ifname', ",".join(ifname_list))])
        routetbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "ROUTE_TABLE")
        routetbl.set(routeip,fvs)
        return True

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

    def test_srv6_vpn(self, dvs, testlog):
        self.setup_db(dvs)
        dvs.setup_db()

        # save exist asic db entries
        tunnel_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        nexthop_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        route_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        vpn_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_TUNNEL_MAP_TYPE_PREFIX_AGG_ID_TO_SRV6_VPN_SID")
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

    def test_srv6_vpn_with_nhg(self, dvs, testlog):
        self.setup_db(dvs)
        dvs.setup_db()

        segsrc_list = []
        nexthop_list = []
        vpn_list = []
        ifname_list = []

        # save exist asic db entries
        tunnel_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        nexthop_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        route_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        vpn_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_TUNNEL_MAP_TYPE_PREFIX_AGG_ID_TO_SRV6_VPN_SID")
        map_entry_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")
        map_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP")

        nexthop_group_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP")
        nexthop_group_member_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")

        segsrc_list.append('1001:2000::1')
        segsrc_list.append('1001:2000::1')

        nexthop_list.append('2000::1')
        nexthop_list.append('2000::2')

        vpn_list.append('3000::1')
        vpn_list.append('3000::2')

        ifname_list.append('unknown')
        ifname_list.append('unknown')

        route_key = self.create_srv6_vpn_route_with_nhg('5000::/64', nexthop_list, segsrc_list, vpn_list, ifname_list)
        tunnel_ids = get_created_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", tunnel_entries, 2)
        nh_ids = get_created_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", nexthop_entries, 2)
        nhg_id = get_created_entry(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP", nexthop_group_entries)
        nhg_mem = get_created_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER", nexthop_group_member_entries, 2)
        map_entry_ids = get_created_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY", map_entry_entries, 2)
        map_ids = get_created_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP", map_entries, 2)

        nh_ids = sorted(nh_ids)
        nhg_mem = sorted(nhg_mem)
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

        # check vpn sid value in SRv6 route is created
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")
        for map_entry_id in map_entry_ids:
            (status, fvs) = tbl.get(map_entry_id)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_TUNNEL_MAP_ENTRY_ATTR_PREFIX_AGG_ID_KEY":
                    assert fv[1] == prefix_agg_id

        # check sid list value in ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP is created
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        for nh_id in nh_ids:
            (status, fvs) = tbl.get(nh_id)
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_NEXT_HOP_ATTR_TYPE":
                    assert fv[1] == "SAI_NEXT_HOP_TYPE_SRV6_SIDLIST"


        route_key_new = self.create_srv6_vpn_route_with_nhg('5001::/64', nexthop_list, segsrc_list, vpn_list, ifname_list)
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

        # check ASIC SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
        (status, fvs) = tbl.get(nhg_mem[0])
        assert status == False

        # check ASIC SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER")
        (status, fvs) = tbl.get(nhg_mem[1])
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

        # check vpn sid value in SRv6 route is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")
        for map_entry_id in map_entry_ids:
            (status, fvs) = tbl.get(map_entry_id)
            assert status == False

        # check next hop in ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP is removed
        tbl = swsscommon.Table(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        for nh_id in nh_ids:
            (status, fvs) = tbl.get(nh_id)
            assert status == False

    def test_srv6_vpn_nh_update(self, dvs, testlog):
        self.setup_db(dvs)
        dvs.setup_db()

        segsrc_list = []
        nexthop_list = []
        vpn_list = []
        ifname_list = []

        # save exist asic db entries
        tunnel_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        nexthop_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
        route_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        vpn_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_TUNNEL_MAP_TYPE_PREFIX_AGG_ID_TO_SRV6_VPN_SID")
        map_entry_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")
        map_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP")

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

        map_entry_entries = get_exist_entries(self.adb.db_connection, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")
        self.update_srv6_vpn_route_attribute('5000::/64', nexthop_list, segsrc_list, vpn_list, ifname_list)

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

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
