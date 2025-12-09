import time
import ipaddress
import json
import random
import time
import pytest

from swsscommon import swsscommon
from pprint import pprint
from dvslib.dvs_common import wait_for_result
from vnet_lib import *


class TestVnet2Orch(object):
    CFG_SUBNET_DECAP_TABLE_NAME = "SUBNET_DECAP"

    @pytest.fixture
    def setup_subnet_decap(self, dvs):

        def _apply_subnet_decap_config(subnet_decap_config):
            """Apply subnet decap config to CONFIG_DB."""
            subnet_decap_tbl = swsscommon.Table(configdb, self.CFG_SUBNET_DECAP_TABLE_NAME)
            fvs = create_fvs(**subnet_decap_config)
            subnet_decap_tbl.set("AZURE", fvs)

        def _cleanup_subnet_decap_config():
            """Cleanup subnet decap config in CONFIG_DB."""
            subnet_decap_tbl = swsscommon.Table(configdb, self.CFG_SUBNET_DECAP_TABLE_NAME)
            for key in subnet_decap_tbl.getKeys():
                subnet_decap_tbl._del(key)

        configdb = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        _cleanup_subnet_decap_config()

        yield _apply_subnet_decap_config

        _cleanup_subnet_decap_config()

    def get_vnet_obj(self):
        return VnetVxlanVrfTunnel()

    def setup_db(self, dvs):
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()
        self.cdb = dvs.get_config_db()
        self.sdb = dvs.get_state_db()

    def clear_srv_config(self, dvs):
        dvs.servers[0].runcmd("ip address flush dev eth0")
        dvs.servers[1].runcmd("ip address flush dev eth0")
        dvs.servers[2].runcmd("ip address flush dev eth0")
        dvs.servers[3].runcmd("ip address flush dev eth0")

    def set_admin_status(self, interface, status):
        self.cdb.update_entry("PORT", interface, {"admin_status": status})

    def create_l3_intf(self, interface, vrf_name):
        if len(vrf_name) == 0:
            self.cdb.create_entry("INTERFACE", interface, {"NULL": "NULL"})
        else:
            self.cdb.create_entry("INTERFACE", interface, {"vrf_name": vrf_name})

    def add_ip_address(self, interface, ip):
        self.cdb.create_entry("INTERFACE", interface + "|" + ip, {"NULL": "NULL"})

    def remove_ip_address(self, interface, ip):
        self.cdb.delete_entry("INTERFACE", interface + "|" + ip)

    def check_route_entries(self, destinations, absent=False):
        def _access_function():
            route_entries = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            route_destinations = [json.loads(route_entry)["dest"]
                                  for route_entry in route_entries]
            return (all(destination in route_destinations for destination in destinations), None)
        if absent:
            return True if _access_function() == None else False

        wait_for_result(_access_function)
        return True


    '''
    Test 1 - Test for vnet tunnel routes interaction with regular route.
        Add the conflicting route and then add the vnet route with same nexthops.
        Bring up the bfd sessions and check the vnet route is programmed in hardware.
        Remove the vnet route and check the vnet route is removed.
        Remove the conflicting route and check the conflicting route is removed.
    '''
    def test_vnet_orch_1(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_1'
        vnet_name = 'Vnet1'
        self.setup_db(dvs)
        vnet_obj.fetch_exist_entries(dvs)
        # create l3 interface and bring it up
        self.create_l3_intf("Ethernet0", "")
        self.add_ip_address("Ethernet0", "20.20.20.1/24")
        self.set_admin_status("Ethernet0", "down")
        time.sleep(1)
        self.set_admin_status("Ethernet0", "up")

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 20.20.20.5/24 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 20.20.20.1")
        
        # create vxlan tunnel and verfiy it
        create_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '1001', "", scope="default")
        vnet_obj.check_default_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '1001')
        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')

        vnet_obj.fetch_exist_entries(dvs)

        # add conflicting route
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 103.100.1.1/32 20.20.20.5\"")

        # check ASIC route database
        self.check_route_entries(["103.100.1.1/32"])

        create_vnet_routes(dvs, "103.100.1.1/32", vnet_name, '9.0.0.1,9.0.0.2,9.0.0.3', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3')

        # default bfd status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["103.100.1.1/32"], absent=True)
        check_state_db_routes(dvs, vnet_name, "103.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "103.100.1.1/32")

        # Route should be properly configured when all bfd session states go up
        update_bfd_session_state(dvs, '9.1.0.2', 'Up')
        time.sleep(1)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.2'], tunnel_name)

        update_bfd_session_state(dvs, '9.1.0.3', 'Up')
        time.sleep(1)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.2', '9.0.0.3'], tunnel_name, route_ids=route1, nhg=nhg1_1)

        update_bfd_session_state(dvs, '9.1.0.1', 'Up')
        time.sleep(1)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.1', '9.0.0.2', '9.0.0.3'], tunnel_name, route_ids=route1, nhg=nhg1_1)
        check_state_db_routes(dvs, vnet_name, "103.100.1.1/32", ['9.0.0.1', '9.0.0.2', '9.0.0.3'])

        # Remove all endpoint from group route shouldnt come back up.
        update_bfd_session_state(dvs, '9.1.0.2', 'Down')
        update_bfd_session_state(dvs, '9.1.0.1', 'Down')
        update_bfd_session_state(dvs, '9.1.0.3', 'Down')

        time.sleep(1)
        # after removal of vnet route, conflicting route is not getting programmed as its not a bgp learnt route.
        self.check_route_entries(["103.100.1.1/32"], absent=True)
        # Remove tunnel route 1
        delete_vnet_routes(dvs, "103.100.1.1/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["103.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "103.100.1.1/32")
        check_remove_routes_advertisement(dvs, "103.100.1.1/32")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)        
        assert nhg1_1 not in vnet_obj.nhgs

        # Confirm the BFD sessions are removed
        check_del_bfd_session(dvs, ['9.1.0.1', '9.1.0.2', '9.1.0.3'])
        vnet_obj.nhg_ids = {}
        vnet_obj.fetch_exist_entries(dvs)
        # readd the same route.
        create_vnet_routes(dvs, "103.100.1.1/32", vnet_name, '9.0.0.1,9.0.0.2,9.0.0.3', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3')

        # default bfd status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["103.100.1.1/32"], absent=True)
        check_state_db_routes(dvs, vnet_name, "103.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "103.100.1.1/32")

        # Route should be properly configured when all bfd session states go up
        update_bfd_session_state(dvs, '9.1.0.2', 'Up')
        time.sleep(1)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.2'], tunnel_name)

        update_bfd_session_state(dvs, '9.1.0.3', 'Up')
        time.sleep(1)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.2', '9.0.0.3'], tunnel_name, route_ids=route1, nhg=nhg1_1)

        update_bfd_session_state(dvs, '9.1.0.1', 'Up')
        time.sleep(1)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.1', '9.0.0.2', '9.0.0.3'], tunnel_name, route_ids=route1, nhg=nhg1_1)
        check_state_db_routes(dvs, vnet_name, "103.100.1.1/32", ['9.0.0.1', '9.0.0.2', '9.0.0.3'])

        # Remove all endpoint from group route shouldnt come back up.
        update_bfd_session_state(dvs, '9.1.0.2', 'Down')
        update_bfd_session_state(dvs, '9.1.0.1', 'Down')
        update_bfd_session_state(dvs, '9.1.0.3', 'Down')

        time.sleep(1)
        # after removal of vnet route, conflicting route is not getting programmed as its not a bgp learnt route.
        self.check_route_entries(["103.100.1.1/32"], absent=True)
        # Remove tunnel route 1
        delete_vnet_routes(dvs, "103.100.1.1/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["103.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "103.100.1.1/32")
        check_remove_routes_advertisement(dvs, "103.100.1.1/32")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_1 not in vnet_obj.nhgs

        # Confirm the BFD sessions are removed
        check_del_bfd_session(dvs, ['9.1.0.1', '9.1.0.2', '9.1.0.3'])
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 103.100.1.1/32\"")
        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 2 - Test for vnet tunnel routes interaction with regular route with endpoints bieng up.
        Add the conflicting route and then add the vnet route with same nexthops.
        Bring up the bfd sessions and check the vnet route is programmed in hardware.
        Add the 2nd conflicting route and then add the 2nd vnet route with same nexthops as first vnet route.
        This way we check if the newly added route works when the nexthops are already UP.
        Verify the vnet routes are programmed in hardware.
        Remove all the vnet route and check the vnet route is removed.
        Remove all the conflicting route and check the conflicting route is removed.
    '''
    def test_vnet_orch_2(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_2'
        vnet_name = 'Vnet2'
        self.setup_db(dvs)
        vnet_obj.fetch_exist_entries(dvs)

        # create l3 interface and bring it up
        self.create_l3_intf("Ethernet0", "")
        self.add_ip_address("Ethernet0", "20.20.20.1/24")
        self.set_admin_status("Ethernet0", "down")
        time.sleep(1)
        self.set_admin_status("Ethernet0", "up")

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 20.20.20.6/24 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 20.20.20.1")
        
        # create vxlan tunnel and verfiy it
        create_vxlan_tunnel(dvs, tunnel_name, '9.8.8.9')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '1002', "", scope="default")
        vnet_obj.check_default_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '1002')
        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '9.8.8.9')
        vnet_obj.fetch_exist_entries(dvs)

        # add conflicting route
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 200.100.1.1/32 20.20.20.6\"")

        # check ASIC route database
        self.check_route_entries(["200.100.1.1/32"])

        create_vnet_routes(dvs, "200.100.1.1/32", vnet_name, '9.0.0.1,9.0.0.2,9.0.0.3', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3')

        # default bfd status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["200.100.1.1/32"], absent=True)
        check_state_db_routes(dvs, vnet_name, "200.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "200.100.1.1/32")

        # Route should be properly configured when all bfd session states go up
        update_bfd_session_state(dvs, '9.1.0.2', 'Up')
        time.sleep(1)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.2'], tunnel_name)

        update_bfd_session_state(dvs, '9.1.0.3', 'Up')
        time.sleep(1)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.2', '9.0.0.3'], tunnel_name, route_ids=route1, nhg=nhg1_1)

        update_bfd_session_state(dvs, '9.1.0.1', 'Up')
        time.sleep(1)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.1', '9.0.0.2', '9.0.0.3'], tunnel_name, route_ids=route1, nhg=nhg1_1)
        check_state_db_routes(dvs, vnet_name, "200.100.1.1/32", ['9.0.0.1', '9.0.0.2', '9.0.0.3'])

        # create a new regular and vnet route with same different prefix but same nexthops as before.
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 200.200.1.1/32 20.20.20.6\"")
        # check ASIC route database
        self.check_route_entries(["200.200.1.1/32"])

        create_vnet_routes(dvs, "200.200.1.1/32", vnet_name, '9.0.0.1,9.0.0.2,9.0.0.3', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3')
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 200.100.1.1/32 20.20.20.6\"")

        # Remove all endpoint from group route shouldnt come back up.
        update_bfd_session_state(dvs, '9.1.0.2', 'Down')
        update_bfd_session_state(dvs, '9.1.0.1', 'Down')
        update_bfd_session_state(dvs, '9.1.0.3', 'Down')

        time.sleep(1)
        # after removal of vnet route, conflicting route is not getting programmed.
        self.check_route_entries(["200.100.1.1/32"], absent=True)
        self.check_route_entries(["200.200.1.1/32"], absent=True)

        # Remove tunnel route 1
        delete_vnet_routes(dvs, "200.100.1.1/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["200.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "200.100.1.1/32")
        check_remove_routes_advertisement(dvs, "200.100.1.1/32")

        # Remove tunnel route 2
        delete_vnet_routes(dvs, "200.200.1.1/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["200.200.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "200.200.1.1/32")
        check_remove_routes_advertisement(dvs, "200.200.1.1/32")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_1 not in vnet_obj.nhgs

        # Confirm the BFD sessions are removed
        check_del_bfd_session(dvs, ['9.1.0.1', '9.1.0.2', '9.1.0.3'])

        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)


    '''
    Test 3 - Test for vnet tunnel routes (custom monitoring) interaction with regular route.
        Add the conflicting route and then add the vnet route with same nexthops.
        Bring up the bfd sessions and check the vnet route is programmed in hardware.
        Remove the vnet route and check the vnet route is removed.
        Remove the conflicting route and check the conflicting route is removed.
    '''
    def test_vnet_orch_3(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_3'
        vnet_name = 'Vnet3'
        self.setup_db(dvs)
        vnet_obj.fetch_exist_entries(dvs)
        # create l3 interface and bring it up
        self.create_l3_intf("Ethernet0", "")
        self.add_ip_address("Ethernet0", "20.20.20.1/24")
        self.set_admin_status("Ethernet0", "down")
        time.sleep(1)
        self.set_admin_status("Ethernet0", "up")

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 20.20.20.7/24 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 20.20.20.1")
        
        # create vxlan tunnel and verfiy it
        create_vxlan_tunnel(dvs, tunnel_name, '19.19.19.19')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '1003', "", 'default', advertise_prefix=True, overlay_dmac="22:33:33:44:44:66")
        vnet_obj.check_default_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '1003')
        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '19.19.19.19')

        vnet_obj.fetch_exist_entries(dvs)

        # add conflicting route
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 105.100.1.1/32 20.20.20.7\"")

        # check ASIC route database
        self.check_route_entries(["105.100.1.1/32"])

        create_vnet_routes(dvs, "105.100.1.1/32", vnet_name, '9.7.0.1,9.7.0.2,9.7.0.3,9.7.0.4', ep_monitor='9.1.2.1,9.1.2.2,9.1.2.3,9.1.2.4',profile = "test_prf", primary='9.7.0.1,9.7.0.2', monitoring='custom',adv_prefix='105.100.1.1/32')
        # Route should be properly configured when all monitor session states go up
        update_monitor_session_state(dvs, "105.100.1.1/32", '9.1.2.2', 'up')
        update_monitor_session_state(dvs, "105.100.1.1/32", '9.1.2.3', 'up')
        update_monitor_session_state(dvs, "105.100.1.1/32", '9.1.2.1', 'up')
        time.sleep(1)
        route1= vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.7.0.2,9.7.0.1'], tunnel_name)
        check_state_db_routes(dvs, vnet_name, "105.100.1.1/32", ['9.7.0.1', '9.7.0.2'])

        # Remove all endpoint from group route shouldnt come back up.
        update_monitor_session_state(dvs, "105.100.1.1/32", '9.1.2.2', 'down')
        update_monitor_session_state(dvs, "105.100.1.1/32", '9.1.2.1', 'down')
        update_monitor_session_state(dvs, "105.100.1.1/32", '9.1.2.3', 'down')
        time.sleep(1)
        # after removal of vnet route, conflicting route is not getting programmed as its not a bgp learnt route.
        self.check_route_entries(["105.100.1.1/32"], absent=True)
        # Remove tunnel route 1
        delete_vnet_routes(dvs, "105.100.1.1/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["105.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "105.100.1.1/32")
        check_remove_routes_advertisement(dvs, "105.100.1.1/32")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)        

        vnet_obj.nhg_ids = {}
        vnet_obj.fetch_exist_entries(dvs)
        # readd the same route.
        create_vnet_routes(dvs, "105.100.1.1/32", vnet_name, '9.7.0.1,9.7.0.2,9.7.0.3,9.7.0.4', ep_monitor='9.1.2.1,9.1.2.2,9.1.2.3,9.1.2.4',primary='9.7.0.1,9.7.0.2', monitoring='custom')

        # default bfd status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["105.100.1.1/32"], absent=True)
        check_state_db_routes(dvs, vnet_name, "105.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "105.100.1.1/32")

        # Route should be properly configured when all bfd session states go up
        update_monitor_session_state(dvs, "105.100.1.1/32", '9.1.2.2', 'up')
        time.sleep(1)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.7.0.2'], tunnel_name)

        update_monitor_session_state(dvs, "105.100.1.1/32", '9.1.2.1', 'up')
        time.sleep(1)
        vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.7.0.2,9.7.0.1'], tunnel_name)

        # Remove all endpoint from group route shouldnt come back up.
        update_monitor_session_state(dvs, "105.100.1.1/32", '9.1.2.2', 'down')
        update_monitor_session_state(dvs, "105.100.1.1/32", '9.1.2.1', 'down')

        time.sleep(1)
        # after removal of vnet route, conflicting route is not getting programmed as its not a bgp learnt route.
        self.check_route_entries(["105.100.1.1/32"], absent=True)
        # Remove tunnel route 1
        delete_vnet_routes(dvs, "105.100.1.1/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["105.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "105.100.1.1/32")
        check_remove_routes_advertisement(dvs, "105.100.1.1/32")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)

        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 105.100.1.1/32\"")
        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 4 - Test that conflicting IP space does not result in deletion on default VRF for vnets which are not in default scope
    '''
    def test_vnet_orch_conflicting_route_non_default_vnet(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_1'
        vnet_name = 'Vnet1'
        self.setup_db(dvs)
        vnet_obj.fetch_exist_entries(dvs)
        # create l3 interface and bring it up
        self.create_l3_intf("Ethernet0", "")
        self.add_ip_address("Ethernet0", "20.20.20.1/24")
        self.set_admin_status("Ethernet0", "down")
        time.sleep(1)
        self.set_admin_status("Ethernet0", "up")

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 20.20.20.5/24 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 20.20.20.1") 

        # create vxlan tunnel and verify it
        create_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '1001', "")
        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '1001')
        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')

        # add conflicting route
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 106.100.1.1/32 20.20.20.5\"")
        # check ASIC route database
        self.check_route_entries(["106.100.1.1/32"])
        vnet_obj.fetch_exist_entries(dvs)

        create_vnet_routes(dvs, "106.100.1.1/32", vnet_name, '9.8.0.1,9.8.0.2,9.8.0.3')
        # Default vrf route should be present
        vnet_obj.check_default_vrf_route(dvs, "106.100.1.1/32")
        # State DB route should show non existent endpoints as bfd state is down
        check_state_db_routes(dvs, vnet_name, "106.100.1.1/32", ['9.8.0.1', '9.8.0.2', '9.8.0.3'])

        vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.8.0.1','9.8.0.2', '9.8.0.3'], tunnel_name)

        # Default vrf route should still be present
        vnet_obj.check_default_vrf_route(dvs, "106.100.1.1/32")

        delete_vnet_routes(dvs, "106.100.1.1/32", vnet_name)
        vnet_obj.check_del_vnet_route_in_vnet(dvs, vnet_name, "106.100.1.1/32")
        check_remove_state_db_routes(dvs, vnet_name, "106.100.1.1/32")

        # Default vrf route should still be present
        vnet_obj.check_default_vrf_route(dvs, "106.100.1.1/32")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 106.100.1.1/32\"")
        self.check_route_entries(["106.100.1.1/32"], absent=True)
        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
