from swsscommon import swsscommon
import random
import pytest
from pprint import pprint
from evpn_tunnel import VxlanTunnel,VxlanEvpnHelper
import time

class TestL3Vxlan(object):

    def get_vxlan_obj(self):
        return VxlanTunnel()

    def get_vxlan_helper(self):
        return VxlanEvpnHelper()

    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        self.sdb = swsscommon.DBConnector(6, dvs.redis_sock, 0)

#    Test 1 - Create and Delete SIP Tunnel and VRF VNI Map entries
#    @pytest.mark.skip(reason="Starting Route Orch, VRF Orch to be merged")
#    @pytest.mark.dev_sanity
    def test_sip_tunnel_vrf_vni_map(self, dvs, testlog):
        vxlan_obj = self.get_vxlan_obj()
        helper = self.get_vxlan_helper()

        self.setup_db(dvs)
        tunnel_name = 'tunnel_2'
        map_name = 'map_1000_100'
        vrf_map_name = 'evpn_map_1000_Vrf-RED'

        vxlan_obj.fetch_exist_entries(dvs)

        print ("\n\nTesting Create and Delete SIP Tunnel and VRF VNI Map entries")
        print ("\tCreate SIP Tunnel")
        vxlan_obj.create_vlan1(dvs,"Vlan100")
        vxlan_obj.check_vlan_obj(dvs, "100")
        vxlan_obj.create_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')
        vxlan_obj.create_evpn_nvo(dvs, 'nvo1', tunnel_name)

        print ("\tCreate Vlan-VNI map and VRF-VNI map")
        vxlan_obj.create_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')

        vxlan_obj.create_vrf(dvs, "Vrf-RED")
        vxlan_obj.create_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED', '1000')

        print ("\tTesting VRF-VNI map in APP DB")
        vlanlist = ['100']
        vnilist = ['1000']

        exp_attrs = [
                ("vni", "1000"),
        ]
        exp_attr = {}
        for an in range(len(exp_attrs)):
            exp_attr[exp_attrs[an][0]] = exp_attrs[an][1]

        helper.check_object(self.pdb, "VRF_TABLE", 'Vrf-RED', exp_attr)

        exp_attrs1 = [
                ("vni", "1000"),
                ("vlan", "Vlan100"),
        ]
        exp_attr1 = {}
        for an in range(len(exp_attrs1)):
            exp_attr1[exp_attrs1[an][0]] = exp_attrs1[an][1]

        helper.check_object(self.pdb, "VXLAN_VRF_TABLE", "%s:%s" % (tunnel_name, vrf_map_name), exp_attr1)

        print ("\tTesting SIP Tunnel Creation")
        vxlan_obj.check_vxlan_sip_tunnel(dvs, tunnel_name, '6.6.6.6', vlanlist, vnilist, tunnel_map_entry_count = 2)

        print ("\tTesting Tunnel Vlan VNI Map Entry")
        vxlan_obj.check_vxlan_tunnel_map_entry_removed(dvs, tunnel_name, vlanlist, vnilist)

        print ("\tTesting Tunnel VRF VNI Map Entry")
        vxlan_obj.check_vxlan_tunnel_vrf_map_entry(dvs, tunnel_name, 'Vrf-RED', '1000')

        print ("\tTesting Tunnel VRF VNI Map Entry removal")
        vxlan_obj.remove_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED')
        vxlan_obj.remove_vrf(dvs, "Vrf-RED")
        vxlan_obj.check_vxlan_tunnel_vrf_map_entry_remove(dvs, tunnel_name, 'Vrf-RED', '1000')

        print ("\tTesting Tunnel Vlan VNI Map entry removal")
        vxlan_obj.check_vxlan_tunnel_map_entry(dvs, tunnel_name, vlanlist, vnilist)
        vxlan_obj.remove_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        vxlan_obj.check_vxlan_tunnel_map_entry_delete(dvs, tunnel_name, vlanlist, vnilist)

        print ("\tTesting SIP Tunnel Deletion")
        vxlan_obj.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan_obj.remove_evpn_nvo(dvs, 'nvo1')
        time.sleep(2)
        vxlan_obj.check_vxlan_sip_tunnel_delete(dvs, tunnel_name, '6.6.6.6')
        vxlan_obj.remove_vlan(dvs, "100")


#    Test 2 - Create and Delete DIP Tunnel on adding and removing prefix route
#    @pytest.mark.skip(reason="Starting Route Orch, VRF Orch to be merged")
#    @pytest.mark.dev_sanity
    def test_prefix_route_create_tunnel(self, dvs, testlog):
        vxlan_obj = self.get_vxlan_obj()
        helper = self.get_vxlan_helper()

        self.setup_db(dvs)
        tunnel_name = 'tunnel_2'
        map_name = 'map_1000_100'
        vrf_map_name = 'evpn_map_1000_Vrf-RED'
        vxlan_obj.fetch_exist_entries(dvs)

        print ("\n\nTesting Create and Delete DIP Tunnel on adding and removing prefix route")
        print ("\tCreate SIP Tunnel")
        vlan_ids = helper.get_exist_entries(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_oid = vxlan_obj.create_vlan(dvs,"Vlan100", vlan_ids)
        vxlan_obj.create_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')
        vxlan_obj.create_evpn_nvo(dvs, 'nvo1', tunnel_name)

        print ("\tCreate Vlan-VNI map and VRF-VNI map")
        vxlan_obj.create_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')

        print ("\tTesting VRF-VNI map in APP DB")
        vxlan_obj.create_vrf(dvs, "Vrf-RED")
        vxlan_obj.create_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED', '1000')

        vlanlist = ['100']
        vnilist = ['1000']

        exp_attrs = [
                ("vni", "1000"),
        ]
        exp_attr = {}
        for an in range(len(exp_attrs)):
            exp_attr[exp_attrs[an][0]] = exp_attrs[an][1]

        helper.check_object(self.pdb, "VRF_TABLE", 'Vrf-RED', exp_attr)

        exp_attrs1 = [
                ("vni", "1000"),
                ("vlan", "Vlan100"),
        ]
        exp_attr1 = {}
        for an in range(len(exp_attrs1)):
            exp_attr1[exp_attrs1[an][0]] = exp_attrs1[an][1]

        helper.check_object(self.pdb, "VXLAN_VRF_TABLE", "%s:%s" % (tunnel_name, vrf_map_name), exp_attr1)

        print ("\tTesting SIP Tunnel Creation")
        vxlan_obj.check_vxlan_sip_tunnel(dvs, tunnel_name, '6.6.6.6', vlanlist, vnilist, tunnel_map_entry_count = 2)

        print ("\tTesting Tunnel Vlan Map Entry")
        vxlan_obj.check_vxlan_tunnel_map_entry_removed(dvs, tunnel_name, vlanlist, vnilist)

        print ("\tTesting Tunnel Vrf Map Entry")
        vxlan_obj.check_vxlan_tunnel_vrf_map_entry(dvs, tunnel_name, 'Vrf-RED', '1000')

        print ("\tTesting VLAN 100 interface creation")
        vxlan_obj.create_vlan_interface(dvs, "Vlan100", "Ethernet24", "Vrf-RED", "100.100.3.1/24")
        vxlan_obj.check_router_interface(dvs, 'Vrf-RED', vlan_oid, 2)

        print ("\tTest VRF IPv4 Route with Tunnel Nexthop Add")
        vxlan_obj.fetch_exist_entries(dvs)
        vxlan_obj.create_vrf_route(dvs, "80.80.1.0/24", 'Vrf-RED', '7.7.7.7', "Vlan100", "00:11:11:11:11:11", '1000')
        vxlan_obj.check_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')

        print ("\tTest VRF IPv4 Route with Tunnel Nexthop Delete")
        vxlan_obj.delete_vrf_route(dvs, "80.80.1.0/24", 'Vrf-RED')
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        vxlan_obj.check_del_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED')

        print ("\tTesting Tunnel Vrf Map Entry removal")
        vxlan_obj.remove_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED')
        vxlan_obj.check_vxlan_tunnel_vrf_map_entry_remove(dvs, tunnel_name, 'Vrf-RED', '1000')

        print ("\tTesting Vlan 100 interface delete")
        vxlan_obj.delete_vlan_interface(dvs, "Vlan100", "100.100.3.1/24")
        vxlan_obj.check_del_router_interface(dvs, "Vlan100")

        print ("\tTesting Tunnel Map entry removal")
        vxlan_obj.check_vxlan_tunnel_map_entry(dvs, tunnel_name, vlanlist, vnilist)
        vxlan_obj.remove_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        vxlan_obj.check_vxlan_tunnel_map_entry_delete(dvs, tunnel_name, vlanlist, vnilist)

        print ("\tTesting SIP Tunnel Deletion")
        vxlan_obj.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan_obj.remove_evpn_nvo(dvs, 'nvo1')
        time.sleep(2)
        vxlan_obj.check_vxlan_sip_tunnel_delete(dvs, tunnel_name, '6.6.6.6')
        vxlan_obj.remove_vrf(dvs, "Vrf-RED")
        vxlan_obj.remove_vlan_member(dvs, "100", "Ethernet24")
        vxlan_obj.remove_vlan(dvs, "100")


#    Test 3 - Create and Delete DIP Tunnel and Test IPv4 route and overlay nexthop add and delete
#    @pytest.mark.skip(reason="Starting Route Orch, VRF Orch to be merged")
#    @pytest.mark.dev_sanity
    def test_tunnel_ipv4_routes(self, dvs, testlog):
        vxlan_obj = self.get_vxlan_obj()
        helper = self.get_vxlan_helper()

        self.setup_db(dvs)
        tunnel_name = 'tunnel_2'
        map_name = 'map_1000_100'
        vrf_map_name = 'evpn_map_1000_Vrf-RED'
        vxlan_obj.fetch_exist_entries(dvs)

        print ("\n\nTesting IPv4 Route and Overlay Nexthop Add and Delete")
        print ("\tCreate SIP Tunnel")
        vxlan_obj.create_vlan1(dvs,"Vlan100")
        vxlan_obj.check_vlan_obj(dvs, "100")
        vxlan_obj.create_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')
        vxlan_obj.create_evpn_nvo(dvs, 'nvo1', tunnel_name)

        print ("\tCreate Vlan-VNI map and VRF-VNI map")
        vxlan_obj.create_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')

        vxlan_obj.create_vrf(dvs, "Vrf-RED")
        vxlan_obj.create_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED', '1000')

        print ("\tTesting VRF-VNI map in APP DB")
        vlanlist = ['100']
        vnilist = ['1000']

        exp_attrs = [
                ("vni", "1000"),
        ]
        exp_attr = {}
        for an in range(len(exp_attrs)):
            exp_attr[exp_attrs[an][0]] = exp_attrs[an][1]

        helper.check_object(self.pdb, "VRF_TABLE", 'Vrf-RED', exp_attr)

        exp_attrs1 = [
                ("vni", "1000"),
                ("vlan", "Vlan100"),
        ]
        exp_attr1 = {}
        for an in range(len(exp_attrs1)):
            exp_attr1[exp_attrs1[an][0]] = exp_attrs1[an][1]

        helper.check_object(self.pdb, "VXLAN_VRF_TABLE", "%s:%s" % (tunnel_name, vrf_map_name), exp_attr1)

        print ("\tTesting SIP Tunnel Creation")
        vxlan_obj.check_vxlan_sip_tunnel(dvs, tunnel_name, '6.6.6.6', vlanlist, vnilist, tunnel_map_entry_count = 2)

        print ("\tTesting Tunnel Vlan Map Entry")
        vxlan_obj.check_vxlan_tunnel_map_entry_removed(dvs, tunnel_name, vlanlist, vnilist)

        print ("\tTesting Tunnel Vrf Map Entry")
        vxlan_obj.check_vxlan_tunnel_vrf_map_entry(dvs, tunnel_name, 'Vrf-RED', '1000')

        print ("\tTesting VLAN 100 interface creation")
        vxlan_obj.create_vlan_interface(dvs, "Vlan100", "Ethernet24", "Vrf-RED", "100.100.3.1/24")
        vxlan_obj.check_router_interface(dvs, 'Vrf-RED', vxlan_obj.vlan_id_map['100'], 2)

        print ("\tTest VRF IPv4 Route with Tunnel Nexthop 7.7.7.7 Add")
        vxlan_obj.fetch_exist_entries(dvs)
        vxlan_obj.create_vrf_route(dvs, "80.80.1.0/24", 'Vrf-RED', '7.7.7.7', "Vlan100", "00:11:11:11:11:11", '1000')
        vxlan_obj.check_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')

        print ("\tTest VRF IPv4 Route with Tunnel Nexthop 7.7.7.7 Delete")
        vxlan_obj.delete_vrf_route(dvs, "80.80.1.0/24", 'Vrf-RED')
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        vxlan_obj.check_del_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED')

        print ("\n\nTesting IPv4 Route and Overlay Nexthop Update")
        print ("\tTest VRF IPv4 Route with Tunnel Nexthop 7.7.7.7 Add")
        vxlan_obj.fetch_exist_entries(dvs)
        vxlan_obj.create_vrf_route(dvs, "80.80.1.0/24", 'Vrf-RED', '7.7.7.7', "Vlan100", "00:11:11:11:11:11", '1000')
        vxlan_obj.check_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')

        print ("\tTest Tunnel Nexthop change from 7.7.7.7 to 8.8.8.8")
        vxlan_obj.create_vrf_route(dvs, "80.80.1.0/24", 'Vrf-RED', '8.8.8.8', "Vlan100", "00:22:22:22:22:22", '1000')
        vxlan_obj.check_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '8.8.8.8', tunnel_name, "00:22:22:22:22:22", '1000', 1)

        print ("\tTest Previous Tunnel Nexthop 7.7.7.7 is removed")
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')

        print ("\tTest VRF IPv4 Route and Tunnel Nexthop 8.8.8.8 Delete")
        vxlan_obj.fetch_exist_entries(dvs)
        vxlan_obj.delete_vrf_route(dvs, "80.80.1.0/24", 'Vrf-RED')
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '8.8.8.8', tunnel_name, "00:22:22:22:22:22", '1000')
        vxlan_obj.check_del_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED')

        print ("\n\nTest VRF IPv4 Route with ECMP Tunnel Nexthop Add and Delete")
        vxlan_obj.fetch_exist_entries(dvs)

        ecmp_nexthop_attr = [
            ("nexthop", "7.7.7.7,8.8.8.8"),
            ("ifname", "Vlan100,Vlan100"),
            ("vni_label", "1000,1000"),
            ("router_mac", "00:11:11:11:11:11,00:22:22:22:22:22"),
        ]

        print ("\tTest VRF IPv4 Route with ECMP Tunnel Nexthop [7.7.7.7 , 8.8.8.8] Add")
        vxlan_obj.create_vrf_route_ecmp(dvs, "80.80.1.0/24", 'Vrf-RED', ecmp_nexthop_attr)

        nh_count = 2
        ecmp_nhid_list = vxlan_obj.check_vrf_routes_ecmp(dvs, "80.80.1.0/24", 'Vrf-RED', tunnel_name, nh_count)
        assert nh_count == len(ecmp_nhid_list)
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[0], '7.7.7.7', tunnel_name, '00:11:11:11:11:11', '1000')
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[1], '8.8.8.8', tunnel_name, '00:22:22:22:22:22', '1000')

        print ("\tTest VRF IPv4 Route with ECMP Tunnel Nexthop [7.7.7.7 , 8.8.8.8] Delete")
        vxlan_obj.fetch_exist_entries(dvs)
        vxlan_obj.delete_vrf_route(dvs, "80.80.1.0/24", 'Vrf-RED')
        helper.check_deleted_object(self.adb, vxlan_obj.ASIC_NEXT_HOP, ecmp_nhid_list[0])
        helper.check_deleted_object(self.adb, vxlan_obj.ASIC_NEXT_HOP, ecmp_nhid_list[1])
        
        vxlan_obj.check_vrf_routes_ecmp_nexthop_grp_del(dvs, 2)
        vxlan_obj.check_del_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED')

        print ("\n\nTest VRF IPv4 Multiple Route with ECMP Tunnel Nexthop Add and Delete")
        vxlan_obj.fetch_exist_entries(dvs)

        ecmp_nexthop_attr = [
            ("nexthop", "7.7.7.7,8.8.8.8"),
            ("ifname", "Vlan100,Vlan100"),
            ("vni_label", "1000,1000"),
            ("router_mac", "00:11:11:11:11:11,00:22:22:22:22:22"),
        ]

        print ("\tTest VRF IPv4 Multiple Route with ECMP Tunnel Nexthop [7.7.7.7 , 8.8.8.8] Add")
        vxlan_obj.create_vrf_route_ecmp(dvs, "80.80.1.0/24", 'Vrf-RED', ecmp_nexthop_attr)

        nh_count = 2
        ecmp_nhid_list = vxlan_obj.check_vrf_routes_ecmp(dvs, "80.80.1.0/24", 'Vrf-RED', tunnel_name, nh_count)
        assert nh_count == len(ecmp_nhid_list)
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[0], '7.7.7.7', tunnel_name, '00:11:11:11:11:11', '1000')
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[1], '8.8.8.8', tunnel_name, '00:22:22:22:22:22', '1000')

        nh_count = 2
        vxlan_obj.create_vrf_route_ecmp(dvs, "90.90.1.0/24", 'Vrf-RED', ecmp_nexthop_attr)
        ecmp_nhid_list = vxlan_obj.check_vrf_routes_ecmp(dvs, "90.90.1.0/24", 'Vrf-RED', tunnel_name, nh_count)
        assert nh_count == len(ecmp_nhid_list)
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[0], '7.7.7.7', tunnel_name, '00:11:11:11:11:11', '1000')
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[1], '8.8.8.8', tunnel_name, '00:22:22:22:22:22', '1000')

        print ("\tTest VRF IPv4 Multiple Route with ECMP Tunnel Nexthop [7.7.7.7 , 8.8.8.8] Delete")
        vxlan_obj.fetch_exist_entries(dvs)
        vxlan_obj.delete_vrf_route(dvs, "80.80.1.0/24", 'Vrf-RED')
        vxlan_obj.check_del_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED')
        vxlan_obj.fetch_exist_entries(dvs)
        vxlan_obj.delete_vrf_route(dvs, "90.90.1.0/24", 'Vrf-RED')
        vxlan_obj.check_del_vrf_routes(dvs, "90.90.1.0/24", 'Vrf-RED')
        helper.check_deleted_object(self.adb, vxlan_obj.ASIC_NEXT_HOP, ecmp_nhid_list[0])
        helper.check_deleted_object(self.adb, vxlan_obj.ASIC_NEXT_HOP, ecmp_nhid_list[1])

        vxlan_obj.check_vrf_routes_ecmp_nexthop_grp_del(dvs, 2)

        print ("\n\nTest VRF IPv4 Route with Tunnel Nexthop update from non-ECMP to ECMP")
        print ("\tTest VRF IPv4 Route with Tunnel Nexthop 7.7.7.7 Add")
        vxlan_obj.fetch_exist_entries(dvs)
        vxlan_obj.create_vrf_route(dvs, "80.80.1.0/24", 'Vrf-RED', '7.7.7.7', "Vlan100", "00:11:11:11:11:11", '1000')
        vxlan_obj.check_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')

        ecmp_nexthop_attr = [
            ("nexthop", "7.7.7.7,8.8.8.8"),
            ("ifname", "Vlan100,Vlan100"),
            ("vni_label", "1000,1000"),
            ("router_mac", "00:11:11:11:11:11,00:22:22:22:22:22"),
        ]

        print ("\tTest VRF IPv4 Route with ECMP Tunnel Nexthop [7.7.7.7 , 8.8.8.8] Udpate")
        vxlan_obj.create_vrf_route_ecmp(dvs, "80.80.1.0/24", 'Vrf-RED', ecmp_nexthop_attr)

        nh_count = 2
        ecmp_nhid_list = vxlan_obj.check_vrf_routes_ecmp(dvs, "80.80.1.0/24", 'Vrf-RED', tunnel_name, nh_count, 1)
        assert nh_count == len(ecmp_nhid_list)
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[0], '7.7.7.7', tunnel_name, '00:11:11:11:11:11', '1000')
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[1], '8.8.8.8', tunnel_name, '00:22:22:22:22:22', '1000')

        print ("\n\nTest VRF IPv4 Route with Tunnel Nexthop update from ECMP to non-ECMP")
        print ("\tTest VRF IPv4 Route with Tunnel Nexthop 8.8.8.8 Update")
        vxlan_obj.create_vrf_route(dvs, "80.80.1.0/24", 'Vrf-RED', '8.8.8.8', "Vlan100", "00:22:22:22:22:22", '1000')
        vxlan_obj.check_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '8.8.8.8', tunnel_name, "00:22:22:22:22:22", '1000', 1)

        print ("\tTest Tunnel Nexthop 7.7.7.7 is deleted")
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')

        print ("\tTest Tunnel Nexthop ECMP Group is deleted")
        vxlan_obj.check_vrf_routes_ecmp_nexthop_grp_del(dvs, 2)

        print ("\tTest VRF IPv4 Route with Tunnel Nexthop 8.8.8.8 Delete")
        vxlan_obj.fetch_exist_entries(dvs)
        vxlan_obj.delete_vrf_route(dvs, "80.80.1.0/24", 'Vrf-RED')

        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '8.8.8.8', tunnel_name, "00:22:22:22:22:22", '1000')
        vxlan_obj.check_del_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED')

        print ("\n\nTest DIP and SIP Tunnel Deletion ")
        print ("\tTesting Tunnel Vrf VNI Map Entry removal")
        vxlan_obj.remove_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED')
        vxlan_obj.check_vxlan_tunnel_vrf_map_entry_remove(dvs, tunnel_name, 'Vrf-RED', '1000')

        print ("\tTesting Vlan 100 interface delete")
        vxlan_obj.delete_vlan_interface(dvs, "Vlan100", "100.100.3.1/24")
        vxlan_obj.check_del_router_interface(dvs, "Vlan100")

        print ("\tTesting Tunnel Map entry removal")
        vxlan_obj.check_vxlan_tunnel_map_entry(dvs, tunnel_name, vlanlist, vnilist)
        vxlan_obj.remove_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        vxlan_obj.check_vxlan_tunnel_map_entry_delete(dvs, tunnel_name, vlanlist, vnilist)

        print ("\tTesting SIP Tunnel Deletion")
        vxlan_obj.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan_obj.remove_evpn_nvo(dvs, 'nvo1')
        time.sleep(2)
        vxlan_obj.check_vxlan_sip_tunnel_delete(dvs, tunnel_name, '6.6.6.6')
        vxlan_obj.remove_vrf(dvs, "Vrf-RED")
        vxlan_obj.remove_vlan_member(dvs, "100", "Ethernet24")
        vxlan_obj.remove_vlan(dvs, "100")


#    Test 4 - Create and Delete DIP Tunnel and Test IPv6 route and overlay nexthop add and delete
#    @pytest.mark.skip(reason="Starting Route Orch, VRF Orch to be merged")
#    @pytest.mark.dev_sanity
    def test_tunnel_ipv6_routes(self, dvs, testlog):
        vxlan_obj = self.get_vxlan_obj()
        helper = self.get_vxlan_helper()

        self.setup_db(dvs)
        tunnel_name = 'tunnel_2'
        map_name = 'map_1000_100'
        vrf_map_name = 'evpn_map_1000_Vrf-RED'
        vxlan_obj.fetch_exist_entries(dvs)

        print ("\n\nTesting IPv6 Route and Overlay Nexthop Add and Delete")
        print ("\tCreate SIP Tunnel")
        vxlan_obj.create_vlan1(dvs,"Vlan100")
        vxlan_obj.check_vlan_obj(dvs, "100")
        vxlan_obj.create_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')
        vxlan_obj.create_evpn_nvo(dvs, 'nvo1', tunnel_name)

        print ("\tCreate Vlan-VNI map and VRF-VNI map")
        vxlan_obj.create_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')

        print ("\tTesting VRF-VNI map in APP DB")
        vxlan_obj.create_vrf(dvs, "Vrf-RED")
        vxlan_obj.create_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED', '1000')

        vlanlist = ['100']
        vnilist = ['1000']

        exp_attrs = [
                ("vni", "1000"),
        ]
        exp_attr = {}
        for an in range(len(exp_attrs)):
            exp_attr[exp_attrs[an][0]] = exp_attrs[an][1]

        print ("\tCheck VRF Table in APP DB")
        helper.check_object(self.pdb, "VRF_TABLE", 'Vrf-RED', exp_attr)

        exp_attrs1 = [
                ("vni", "1000"),
                ("vlan", "Vlan100"),
        ]
        exp_attr1 = {}
        for an in range(len(exp_attrs1)):
            exp_attr1[exp_attrs1[an][0]] = exp_attrs1[an][1]

        helper.check_object(self.pdb, "VXLAN_VRF_TABLE", "%s:%s" % (tunnel_name, vrf_map_name), exp_attr1)

        print ("\tTesting SIP Tunnel Creation")
        vxlan_obj.check_vxlan_sip_tunnel(dvs, tunnel_name, '6.6.6.6', vlanlist, vnilist, tunnel_map_entry_count = 2)

        print ("\tTesting Tunnel Vlan Map Entry")
        vxlan_obj.check_vxlan_tunnel_map_entry_removed(dvs, tunnel_name, vlanlist, vnilist)

        print ("\tTesting Tunnel Vrf Map Entry")
        vxlan_obj.check_vxlan_tunnel_vrf_map_entry(dvs, tunnel_name, 'Vrf-RED', '1000')


        vxlan_obj.fetch_exist_entries(dvs)
        print ("\tTesting VLAN 100 interface creation")
        vxlan_obj.create_vlan_interface(dvs, "Vlan100", "Ethernet24", "Vrf-RED", "2001::8/64")
        vxlan_obj.check_router_interface(dvs, 'Vrf-RED', vxlan_obj.vlan_id_map['100'], 2)

        print ("\tTest VRF IPv6 Route with Tunnel Nexthop Add")
        vxlan_obj.fetch_exist_entries(dvs)
        vxlan_obj.create_vrf_route(dvs, "2002::8/64", 'Vrf-RED', '7.7.7.7', "Vlan100", "00:11:11:11:11:11", '1000')
        vxlan_obj.check_vrf_routes(dvs, "2002::8/64", 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')

        print ("\tTest VRF IPv6 Route with Tunnel Nexthop Delete")
        vxlan_obj.delete_vrf_route(dvs, "2002::8/64", 'Vrf-RED')
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        vxlan_obj.check_del_vrf_routes(dvs, "2002::8/64", 'Vrf-RED')

        print ("\n\nTesting IPv6 Route and Overlay Nexthop Update")
        print ("\tTest VRF IPv6 Route with Tunnel Nexthop 7.7.7.7 Add")
        vxlan_obj.fetch_exist_entries(dvs)
        vxlan_obj.create_vrf_route(dvs, "2002::8/64", 'Vrf-RED', '7.7.7.7', "Vlan100", "00:11:11:11:11:11", '1000')
        vxlan_obj.check_vrf_routes(dvs, "2002::8/64", 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')

        print ("\tTest Tunnel Nexthop change from 7.7.7.7 to 8.8.8.8")
        vxlan_obj.create_vrf_route(dvs, "2002::8/64", 'Vrf-RED', '8.8.8.8', "Vlan100", "00:22:22:22:22:22", '1000')
        vxlan_obj.check_vrf_routes(dvs, "2002::8/64", 'Vrf-RED', '8.8.8.8', tunnel_name, "00:22:22:22:22:22", '1000', 1)

        print ("\tTest Previous Tunnel Nexthop 7.7.7.7 is removed")
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')

        print ("\tTest VRF IPv6 Route and Tunnel Nexthop 8.8.8.8 Delete")
        vxlan_obj.fetch_exist_entries(dvs)
        vxlan_obj.delete_vrf_route(dvs, "2002::8/64", 'Vrf-RED')
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '8.8.8.8', tunnel_name, "00:22:22:22:22:22", '1000')
        vxlan_obj.check_del_vrf_routes(dvs, "2002::8/64", 'Vrf-RED')

        print ("\n\nTest VRF IPv6 Route with ECMP Tunnel Nexthop Add and delete")
        vxlan_obj.fetch_exist_entries(dvs)

        ecmp_nexthop_attr = [
            ("nexthop", "7.7.7.7,8.8.8.8"),
            ("ifname", "Vlan100,Vlan100"),
            ("vni_label", "1000,1000"),
            ("router_mac", "00:11:11:11:11:11,00:22:22:22:22:22"),
        ]

        print ("\tTest VRF IPv6 Route with ECMP Tunnel Nexthop [7.7.7.7 , 8.8.8.8] Add")
        vxlan_obj.create_vrf_route_ecmp(dvs, "2002::8/64", 'Vrf-RED', ecmp_nexthop_attr)

        nh_count = 2
        ecmp_nhid_list = vxlan_obj.check_vrf_routes_ecmp(dvs, "2002::8/64", 'Vrf-RED', tunnel_name, nh_count)
        assert nh_count == len(ecmp_nhid_list)
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[0], '7.7.7.7', tunnel_name, '00:11:11:11:11:11', '1000')
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[1], '8.8.8.8', tunnel_name, '00:22:22:22:22:22', '1000')

        print ("\tTest VRF IPv6 Route with ECMP Tunnel Nexthop [7.7.7.7 , 8.8.8.8] Delete")
        vxlan_obj.fetch_exist_entries(dvs)
        vxlan_obj.delete_vrf_route(dvs, "2002::8/64", 'Vrf-RED')
        helper.check_deleted_object(self.adb, vxlan_obj.ASIC_NEXT_HOP, ecmp_nhid_list[0])
        helper.check_deleted_object(self.adb, vxlan_obj.ASIC_NEXT_HOP, ecmp_nhid_list[1])
        
        vxlan_obj.check_vrf_routes_ecmp_nexthop_grp_del(dvs, 2)
        vxlan_obj.check_del_vrf_routes(dvs, "2002::8/64", 'Vrf-RED')

        print ("\n\nTest VRF IPv6 Route with Tunnel Nexthop update from non-ECMP to ECMP")
        print ("\tTest VRF IPv6 Route with Tunnel Nexthop 7.7.7.7 Add")
        vxlan_obj.fetch_exist_entries(dvs)
        vxlan_obj.create_vrf_route(dvs, "2002::8/64", 'Vrf-RED', '7.7.7.7', "Vlan100", "00:11:11:11:11:11", '1000')
        vxlan_obj.check_vrf_routes(dvs, "2002::8/64", 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')

        print ("\tTest VRF IPv4 Route with ECMP Tunnel Nexthop [7.7.7.7 , 8.8.8.8] Udpate")
        ecmp_nexthop_attr = [
            ("nexthop", "7.7.7.7,8.8.8.8"),
            ("ifname", "Vlan100,Vlan100"),
            ("vni_label", "1000,1000"),
            ("router_mac", "00:11:11:11:11:11,00:22:22:22:22:22"),
        ]

        vxlan_obj.create_vrf_route_ecmp(dvs, "2002::8/64", 'Vrf-RED', ecmp_nexthop_attr)

        nh_count = 2
        ecmp_nhid_list = vxlan_obj.check_vrf_routes_ecmp(dvs, "2002::8/64", 'Vrf-RED', tunnel_name, nh_count, 1)
        assert nh_count == len(ecmp_nhid_list)
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[0], '7.7.7.7', tunnel_name, '00:11:11:11:11:11', '1000')
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[1], '8.8.8.8', tunnel_name, '00:22:22:22:22:22', '1000')

        print ("\n\nTest VRF IPv6 Route with Tunnel Nexthop update from ECMP to non-ECMP")
        print ("\tTest VRF IPv6 Route with Tunnel Nexthop 8.8.8.8 Update")
        vxlan_obj.create_vrf_route(dvs, "2002::8/64", 'Vrf-RED', '8.8.8.8', "Vlan100", "00:22:22:22:22:22", '1000')
        vxlan_obj.check_vrf_routes(dvs, "2002::8/64", 'Vrf-RED', '8.8.8.8', tunnel_name, "00:22:22:22:22:22", '1000', 1)

        print ("\tTest Tunnel Nexthop 7.7.7.7 is deleted")
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')

        print ("\tTest Tunnel Nexthop ECMP Group is deleted")
        vxlan_obj.check_vrf_routes_ecmp_nexthop_grp_del(dvs, 2)

        print ("\tTest VRF IPv6 Route with Tunnel Nexthop 8.8.8.8 Delete")
        vxlan_obj.fetch_exist_entries(dvs)
        vxlan_obj.delete_vrf_route(dvs, "2002::8/64", 'Vrf-RED')

        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '8.8.8.8', tunnel_name, "00:22:22:22:22:22", '1000')
        vxlan_obj.check_del_vrf_routes(dvs, "2002::8/64", 'Vrf-RED')

        print ("\n\nTest DIP and SIP Tunnel Deletion ")
        print ("\tTesting Tunnel Vrf Map Entry removal")
        vxlan_obj.remove_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED')
        vxlan_obj.check_vxlan_tunnel_vrf_map_entry_remove(dvs, tunnel_name, 'Vrf-RED', '1000')

        print ("\tTesting Vlan 100 interface delete")
        vxlan_obj.delete_vlan_interface(dvs, "Vlan100", "2001::8/64")
        vxlan_obj.check_del_router_interface(dvs, "Vlan100")

        print ("\tTesting Tunnel Map entry removal")
        vxlan_obj.check_vxlan_tunnel_map_entry(dvs, tunnel_name, vlanlist, vnilist)
        vxlan_obj.remove_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        vxlan_obj.check_vxlan_tunnel_map_entry_delete(dvs, tunnel_name, vlanlist, vnilist)

        print ("\tTesting SIP Tunnel Deletion")
        vxlan_obj.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan_obj.remove_evpn_nvo(dvs, 'nvo1')
        time.sleep(2)
        vxlan_obj.check_vxlan_sip_tunnel_delete(dvs, tunnel_name, '6.6.6.6')
        vxlan_obj.remove_vrf(dvs, "Vrf-RED")
        vxlan_obj.remove_vlan_member(dvs, "100", "Ethernet24")
        vxlan_obj.remove_vlan(dvs, "100")


    def test_vrf_state_db_update(self, dvs, testlog):
        vxlan_obj = self.get_vxlan_obj()
        helper = self.get_vxlan_helper()

        self.setup_db(dvs)
        tunnel_name = 'tunnel_2'
        map_name = 'map_1000_100'
        vrf_map_name = 'evpn_map_1000_Vrf-RED'

        vxlan_obj.fetch_exist_entries(dvs)


        vxlan_obj.create_vrf(dvs, "Vrf-RED")
        vxlan_obj.create_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED', '1000')

        time.sleep(2)
        #adding nvo after
        vxlan_obj.create_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')
        vxlan_obj.create_evpn_nvo(dvs, 'nvo1', tunnel_name)
        exp_attr = [
                ("state", "ok"),
        ]
        helper.check_object(self.sdb, "VRF_OBJECT_TABLE", 'Vrf-RED', exp_attr)
        vxlan_obj.remove_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED')
        vxlan_obj.remove_vrf(dvs, "Vrf-RED")
        vxlan_obj.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan_obj.remove_evpn_nvo(dvs, 'nvo1')

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
