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

class TestVnetOrch(object):

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

    def add_neighbor(self, interface, ip, mac):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "NEIGH_TABLE")
        fvs = swsscommon.FieldValuePairs([("neigh", mac),
                                          ("family", "IPv4")])
        tbl.set(interface + ":" + ip, fvs)
        time.sleep(1)

    def remove_neighbor(self, interface, ip):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "NEIGH_TABLE")
        tbl._del(interface + ":" + ip)
        time.sleep(1)

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


    @pytest.fixture(params=["true", "false"])
    def ordered_ecmp(self, dvs, request):

        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        if request.param == "true":
            create_entry_pst(
                app_db,
                "SWITCH_TABLE", ':', "switch",
                [
                    ('ordered_ecmp', 'true')
                ],
            )
            dvs.get_state_db().wait_for_field_match("SWITCH_CAPABILITY", "switch", {"ORDERED_ECMP_CAPABLE": "true"})

        yield request.param

        if request.param == "true":
            create_entry_pst(
                app_db,
                "SWITCH_TABLE", ':', "switch",
                [
                    ('ordered_ecmp', 'false')
                ],
            )
            dvs.get_state_db().wait_for_field_match("SWITCH_CAPABILITY", "switch", {"ORDERED_ECMP_CAPABLE": "false"})

    '''
    Test 1 - Create Vlan Interface, Tunnel and Vnet
    '''
    def test_vnet_orch_1(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_1'

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '10.10.10.10')
        create_vnet_entry(dvs, 'Vnet_2000', tunnel_name, '2000', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet_2000')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet_2000', '2000')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '10.10.10.10')

        vid = create_vlan_interface(dvs, "Vlan100", "Ethernet24", "Vnet_2000", "100.100.3.1/24")
        vnet_obj.check_router_interface(dvs, "Vlan100", 'Vnet_2000', vid, intf_type="vlan")

        vid = create_vlan_interface(dvs, "Vlan101", "Ethernet28", "Vnet_2000", "100.100.4.1/24")
        vnet_obj.check_router_interface(dvs, "Vlan101", 'Vnet_2000', vid, intf_type="vlan")

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", 'Vnet_2000', '10.10.10.1')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_2000', '10.10.10.1', tunnel_name)
        check_state_db_routes(dvs, 'Vnet_2000', "100.100.1.1/32", ['10.10.10.1'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        create_vnet_local_routes(dvs, "100.100.3.0/24", 'Vnet_2000', 'Vlan100')
        # The route to Vlan100's subnet must have already been added to self.ASIC_ROUTE_ENTRY
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_2000', vlan_subnet_route=True)

        create_vnet_local_routes(dvs, "100.100.4.0/24", 'Vnet_2000', 'Vlan101')
        # The route to Vlan101's subnet must have already been added to self.ASIC_ROUTE_ENTRY
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_2000', vlan_subnet_route=True)

        #Create Physical Interface in another Vnet

        create_vnet_entry(dvs, 'Vnet_2001', tunnel_name, '2001', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet_2001')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet_2001', '2001')

        create_phy_interface(dvs, "Ethernet4", "Vnet_2001", "100.102.1.1/24")
        vnet_obj.check_router_interface(dvs, "Ethernet4", 'Vnet_2001')

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.2.1/32", 'Vnet_2001', '10.10.10.2', "00:12:34:56:78:9A")
        vnet_obj.check_vnet_routes(dvs, 'Vnet_2001', '10.10.10.2', tunnel_name, "00:12:34:56:78:9A")
        check_state_db_routes(dvs, 'Vnet_2001', "100.100.2.1/32", ['10.10.10.2'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        create_vnet_local_routes(dvs, "100.102.1.0/24", 'Vnet_2001', 'Ethernet4')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_2001')

        # Clean-up and verify remove flows

        delete_vnet_local_routes(dvs, "100.100.3.0/24", 'Vnet_2000')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet_2000', "100.100.3.0/24")

        delete_vnet_local_routes(dvs, "100.100.4.0/24", 'Vnet_2000')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet_2000', "100.100.4.0/24")

        delete_vnet_local_routes(dvs, "100.102.1.0/24", 'Vnet_2001')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet_2001', "100.102.1.0/24")

        delete_vnet_routes(dvs, "100.100.2.1/32", 'Vnet_2001')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet_2001')
        check_remove_state_db_routes(dvs, 'Vnet_2001', "100.100.2.1/32")
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        delete_vnet_routes(dvs, "100.100.1.1/32", 'Vnet_2000')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet_2000')
        check_remove_state_db_routes(dvs, 'Vnet_2000', "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        delete_phy_interface(dvs, "Ethernet4", "100.102.1.1/24")
        vnet_obj.check_del_router_interface(dvs, "Ethernet4")

        delete_vlan_interface(dvs, "Vlan100", "100.100.3.1/24")
        vnet_obj.check_del_router_interface(dvs, "Vlan100")

        delete_vlan_interface(dvs, "Vlan101", "100.100.4.1/24")
        vnet_obj.check_del_router_interface(dvs, "Vlan101")

        delete_vnet_entry(dvs, 'Vnet_2001')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet_2001')

        delete_vnet_entry(dvs, 'Vnet_2000')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet_2000')

        delete_vxlan_tunnel(dvs, tunnel_name)
        vnet_obj.check_del_vxlan_tunnel(dvs)

    '''
    Test 2 - Two VNets, One HSMs per VNet
    '''
    def test_vnet_orch_2(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_2'

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')
        create_vnet_entry(dvs, 'Vnet_1', tunnel_name, '1111', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet_1')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet_1', '1111')

        tun_id = vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')

        vid = create_vlan_interface(dvs, "Vlan1001", "Ethernet0", "Vnet_1", "1.1.10.1/24")
        vnet_obj.check_router_interface(dvs, "Vlan1001", 'Vnet_1', vid, intf_type="vlan")

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "1.1.1.10/32", 'Vnet_1', '100.1.1.10')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_1', '100.1.1.10', tunnel_name)
        check_state_db_routes(dvs, 'Vnet_1', "1.1.1.10/32", ['100.1.1.10'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "1.1.1.10/32")

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "1.1.1.11/32", 'Vnet_1', '100.1.1.10')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_1', '100.1.1.10', tunnel_name)
        check_state_db_routes(dvs, 'Vnet_1', "1.1.1.11/32", ['100.1.1.10'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "1.1.1.11/32")

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "1.1.1.12/32", 'Vnet_1', '200.200.1.200')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_1', '200.200.1.200', tunnel_name)
        check_state_db_routes(dvs, 'Vnet_1', "1.1.1.12/32", ['200.200.1.200'])
        check_remove_routes_advertisement(dvs, "1.1.1.12/32")

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "1.1.1.14/32", 'Vnet_1', '200.200.1.201')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_1', '200.200.1.201', tunnel_name)
        check_state_db_routes(dvs, 'Vnet_1', "1.1.1.14/32", ['200.200.1.201'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "1.1.1.14/32")

        create_vnet_local_routes(dvs, "1.1.10.0/24", 'Vnet_1', 'Vlan1001')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_1', vlan_subnet_route=True)

        create_vnet_entry(dvs, 'Vnet_2', tunnel_name, '2222', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet_2')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet_2', '2222')

        vid = create_vlan_interface(dvs, "Vlan1002", "Ethernet4", "Vnet_2", "2.2.10.1/24")
        vnet_obj.check_router_interface(dvs, "Vlan1002", 'Vnet_2', vid, intf_type="vlan")

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "2.2.2.10/32", 'Vnet_2', '100.1.1.20')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_2', '100.1.1.20', tunnel_name)
        check_state_db_routes(dvs, 'Vnet_2', "2.2.2.10/32", ['100.1.1.20'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "2.2.2.10/32")

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "2.2.2.11/32", 'Vnet_2', '100.1.1.20')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_2', '100.1.1.20', tunnel_name)
        check_state_db_routes(dvs, 'Vnet_2', "2.2.2.11/32", ['100.1.1.20'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "2.2.2.11/32")

        create_vnet_local_routes(dvs, "2.2.10.0/24", 'Vnet_2', 'Vlan1002')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_2', vlan_subnet_route=True)

        # Clean-up and verify remove flows

        delete_vnet_local_routes(dvs, "2.2.10.0/24", 'Vnet_2')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet_2', "2.2.10.0/24")

        delete_vnet_local_routes(dvs, "1.1.10.0/24", 'Vnet_1')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet_1', "1.1.10.0/24")

        delete_vnet_routes(dvs, "2.2.2.11/32", 'Vnet_2')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet_2')
        check_remove_state_db_routes(dvs, 'Vnet_2', "2.2.2.11/32")
        check_remove_routes_advertisement(dvs, "2.2.2.11/32")

        delete_vnet_routes(dvs, "2.2.2.10/32", 'Vnet_2')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet_2')
        check_remove_state_db_routes(dvs, 'Vnet_2', "2.2.2.10/32")
        check_remove_routes_advertisement(dvs, "2.2.2.10/32")

        delete_vnet_routes(dvs, "1.1.1.14/32", 'Vnet_1')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet_1')
        check_remove_state_db_routes(dvs, 'Vnet_1', "1.1.1.14/32")
        check_remove_routes_advertisement(dvs, "1.1.1.14/32")

        delete_vnet_routes(dvs, "1.1.1.12/32", 'Vnet_1')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet_1')
        check_remove_state_db_routes(dvs, 'Vnet_1', "1.1.1.12/32")
        check_remove_routes_advertisement(dvs, "1.1.1.12/32")

        delete_vnet_routes(dvs, "1.1.1.11/32", 'Vnet_1')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet_1')
        check_remove_state_db_routes(dvs, 'Vnet_1', "1.1.1.11/32")
        check_remove_routes_advertisement(dvs, "1.1.1.11/32")

        delete_vnet_routes(dvs, "1.1.1.10/32", 'Vnet_1')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet_1')
        check_remove_state_db_routes(dvs, 'Vnet_1', "1.1.1.10/32")
        check_remove_routes_advertisement(dvs, "1.1.1.10/32")

        delete_vlan_interface(dvs, "Vlan1002", "2.2.10.1/24")
        vnet_obj.check_del_router_interface(dvs, "Vlan1002")

        delete_vlan_interface(dvs, "Vlan1001", "1.1.10.1/24")
        vnet_obj.check_del_router_interface(dvs, "Vlan1001")

        delete_vnet_entry(dvs, 'Vnet_1')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet_1')

        delete_vnet_entry(dvs, 'Vnet_2')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet_2')

        delete_vxlan_tunnel(dvs, tunnel_name)
        vnet_obj.check_del_vxlan_tunnel(dvs)

    '''
    Test 3 - Two VNets, One HSMs per VNet, Peering
    '''
    def test_vnet_orch_3(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_3'

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '7.7.7.7')

        create_vnet_entry(dvs, 'Vnet_10', tunnel_name, '3333', "Vnet_20")

        vnet_obj.check_vnet_entry(dvs, 'Vnet_10', ['Vnet_20'])
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet_10', '3333')

        create_vnet_entry(dvs, 'Vnet_20', tunnel_name, '4444', "Vnet_10")

        vnet_obj.check_vnet_entry(dvs, 'Vnet_20', ['Vnet_10'])
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet_20', '4444')

        tun_id = vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '7.7.7.7')

        vid = create_vlan_interface(dvs, "Vlan2001", "Ethernet8", "Vnet_10", "5.5.10.1/24")
        vnet_obj.check_router_interface(dvs, "Vlan2001", 'Vnet_10', vid, intf_type="vlan-one-peer")

        vid = create_vlan_interface(dvs, "Vlan2002", "Ethernet12", "Vnet_20", "8.8.10.1/24")
        vnet_obj.check_router_interface(dvs, "Vlan2002", 'Vnet_20', vid, intf_type="vlan-one-peer")

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "5.5.5.10/32", 'Vnet_10', '50.1.1.10')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_10', '50.1.1.10', tunnel_name)
        check_state_db_routes(dvs, 'Vnet_10', "5.5.5.10/32", ['50.1.1.10'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "5.5.5.10/32")

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "8.8.8.10/32", 'Vnet_20', '80.1.1.20')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_10', '80.1.1.20', tunnel_name)
        check_state_db_routes(dvs, 'Vnet_20', "8.8.8.10/32", ['80.1.1.20'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "8.8.8.10/32")

        create_vnet_local_routes(dvs, "5.5.10.0/24", 'Vnet_10', 'Vlan2001')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_10', vlan_subnet_route=True)

        create_vnet_local_routes(dvs, "8.8.10.0/24", 'Vnet_20', 'Vlan2002')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_20', vlan_subnet_route=True)

        # Clean-up and verify remove flows

        delete_vnet_local_routes(dvs, "5.5.10.0/24", 'Vnet_10')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet_10', "5.5.10.0/24")

        delete_vnet_local_routes(dvs, "8.8.10.0/24", 'Vnet_20')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet_20', "8.8.10.0/24")

        delete_vnet_routes(dvs, "5.5.5.10/32", 'Vnet_10')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet_10')
        check_remove_state_db_routes(dvs, 'Vnet_10', "5.5.5.10/32")
        check_remove_routes_advertisement(dvs, "5.5.5.10/32")

        delete_vnet_routes(dvs, "8.8.8.10/32", 'Vnet_20')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet_20')
        check_remove_state_db_routes(dvs, 'Vnet_20', "8.8.8.10/32")
        check_remove_routes_advertisement(dvs, "8.8.8.10/32")

        delete_vlan_interface(dvs, "Vlan2001", "5.5.10.1/24")
        vnet_obj.check_del_router_interface(dvs, "Vlan2001")

        delete_vlan_interface(dvs, "Vlan2002", "8.8.10.1/24")
        vnet_obj.check_del_router_interface(dvs, "Vlan2002")

        delete_vnet_entry(dvs, 'Vnet_10')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet_10')

        delete_vnet_entry(dvs, 'Vnet_20')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet_20')

        delete_vxlan_tunnel(dvs, tunnel_name)
        vnet_obj.check_del_vxlan_tunnel(dvs)

    '''
    Test 4 - IPv6 Vxlan tunnel test
    '''
    @pytest.mark.skip(reason="Failing. Under investigation")
    def test_vnet_orch_4(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_v6'

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, 'fd:2::32')
        create_vnet_entry(dvs, 'Vnet3001', tunnel_name, '3001', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet3001')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet3001', '3001')
        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, 'fd:2::32')

        vid = create_vlan_interface(dvs, "Vlan300", "Ethernet24", 'Vnet3001', "100.100.3.1/24")
        vnet_obj.check_router_interface(dvs, "Vlan300", 'Vnet3001', vid, intf_type="vlan")

        vid = create_vlan_interface(dvs, "Vlan301", "Ethernet28", 'Vnet3001', "100.100.4.1/24")
        vnet_obj.check_router_interface(dvs, "Vlan301",  'Vnet3001', vid, intf_type="vlan")

        create_vnet_routes(dvs, "100.100.1.1/32", 'Vnet3001', '2000:1000:2000:3000:4000:5000:6000:7000')
        vnet_obj.check_vnet_routes(dvs, 'Vnet3001', '2000:1000:2000:3000:4000:5000:6000:7000', tunnel_name)
        check_state_db_routes(dvs, 'Vnet3001', "100.100.1.1/32", ['2000:1000:2000:3000:4000:5000:6000:7000'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        create_vnet_routes(dvs, "100.100.1.2/32", 'Vnet3001', '2000:1000:2000:3000:4000:5000:6000:7000')
        vnet_obj.check_vnet_routes(dvs, 'Vnet3001', '2000:1000:2000:3000:4000:5000:6000:7000', tunnel_name)
        check_state_db_routes(dvs, 'Vnet3001', "100.100.1.2/32", ['2000:1000:2000:3000:4000:5000:6000:7000'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.1.2/32")

        create_vnet_local_routes(dvs, "100.100.3.0/24", 'Vnet3001', 'Vlan300')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet3001', vlan_subnet_route=True)

        create_vnet_local_routes(dvs, "100.100.4.0/24", 'Vnet3001', 'Vlan301')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet3001', vlan_subnet_route=True)

        #Create Physical Interface in another Vnet

        create_vnet_entry(dvs, 'Vnet3002', tunnel_name, '3002', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet3002')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet3002', '3002')

        create_phy_interface(dvs, "Ethernet60", 'Vnet3002', "100.102.1.1/24")
        vnet_obj.check_router_interface(dvs, "Ethernet60", 'Vnet3002')

        create_vnet_routes(dvs, "100.100.2.1/32", 'Vnet3002', 'fd:2::34', "00:12:34:56:78:9A")
        vnet_obj.check_vnet_routes(dvs, 'Vnet3002', 'fd:2::34', tunnel_name, "00:12:34:56:78:9A")
        check_state_db_routes(dvs, 'Vnet3002', "100.100.2.1/32", ['fd:2::34'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        create_vnet_local_routes(dvs, "100.102.1.0/24", 'Vnet3002', 'Ethernet60')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet3002')

        # Test peering
        create_vnet_entry(dvs, 'Vnet3003', tunnel_name, '3003', 'Vnet3004')

        vnet_obj.check_vnet_entry(dvs, 'Vnet3003', ['Vnet3004'])
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet3003', '3003')

        create_vnet_entry(dvs, 'Vnet3004', tunnel_name, '3004', 'Vnet3003')

        vnet_obj.check_vnet_entry(dvs, 'Vnet3004', ['Vnet3003'])
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet3004', '3004')

        create_vnet_routes(dvs, "5.5.5.10/32", 'Vnet3003', 'fd:2::35')
        vnet_obj.check_vnet_routes(dvs, 'Vnet3004', 'fd:2::35', tunnel_name)
        check_state_db_routes(dvs, 'Vnet3003', "5.5.5.10/32", ['fd:2::35'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "5.5.5.10/32")

        create_vnet_routes(dvs, "8.8.8.10/32", 'Vnet3004', 'fd:2::36')
        vnet_obj.check_vnet_routes(dvs, 'Vnet3003', 'fd:2::36', tunnel_name)
        check_state_db_routes(dvs, 'Vnet3004', "8.8.8.10/32", ['fd:2::36'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "8.8.8.10/32")

        # Clean-up and verify remove flows

        delete_vnet_routes(dvs, "5.5.5.10/32", 'Vnet3003')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet3003')
        check_remove_state_db_routes(dvs, 'Vnet3003', "5.5.5.10/32")
        check_remove_routes_advertisement(dvs, "5.5.5.10/32")

        delete_vnet_routes(dvs, "8.8.8.10/32", 'Vnet3004')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet3004')
        check_remove_state_db_routes(dvs, 'Vnet3004', "8.8.8.10/32")
        check_remove_routes_advertisement(dvs, "8.8.8.10/32")

        delete_vnet_entry(dvs, 'Vnet3003')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet3003')

        delete_vnet_entry(dvs, 'Vnet3004')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet3004')

        delete_vnet_routes(dvs, "100.100.2.1/24", 'Vnet3002')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet3002')
        check_remove_state_db_routes(dvs, 'Vnet3002', "100.100.2.1/24")
        check_remove_routes_advertisement(dvs, "100.100.2.1/24")

        delete_vnet_local_routes(dvs, "100.102.1.0/24", 'Vnet3002')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet3002', "100.102.1.0/24")

        delete_phy_interface(dvs, "Ethernet60", "100.102.1.1/24")
        vnet_obj.check_del_router_interface(dvs, "Ethernet60")

        delete_vnet_entry(dvs, 'Vnet3002')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet3002')

        delete_vnet_local_routes(dvs, "100.100.3.0/24", 'Vnet3001')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet3001', "100.100.3.0/24")

        delete_vnet_local_routes(dvs, "100.100.4.0/24", 'Vnet3001')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet3001', "100.100.4.0/24")

        delete_vnet_routes(dvs, "100.100.1.1/32", 'Vnet3001')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet3001')
        check_remove_state_db_routes(dvs, 'Vnet3001', "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        delete_vnet_routes(dvs, "100.100.1.2/32", 'Vnet3001')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet3001')
        check_remove_state_db_routes(dvs, 'Vnet3001', "100.100.1.2/32")
        check_remove_routes_advertisement(dvs, "100.100.1.2/32")

        delete_vlan_interface(dvs, "Vlan300", "100.100.3.1/24")
        vnet_obj.check_del_router_interface(dvs, "Vlan300")

        delete_vlan_interface(dvs, "Vlan301", "100.100.4.1/24")
        vnet_obj.check_del_router_interface(dvs, "Vlan301")

        delete_vnet_entry(dvs, 'Vnet3001')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet3001')

        delete_vxlan_tunnel(dvs, tunnel_name)
        vnet_obj.check_del_vxlan_tunnel(dvs)

    '''
    Test 5 - Default VNet test
    '''
    def test_vnet_orch_5(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_5'

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '8.8.8.8')
        create_vnet_entry(dvs, 'Vnet_5', tunnel_name, '4789', "", 'default')

        vnet_obj.check_default_vnet_entry(dvs, 'Vnet_5')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet_5', '4789')

        delete_vnet_entry(dvs, 'Vnet_5')
        vnet_obj.check_default_vnet_entry(dvs, 'Vnet_5')

    '''
    Test 6 - Test VxLAN tunnel with multiple maps
    '''
    def test_vnet_vxlan_multi_map(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_v4'

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '10.1.0.32')
        create_vnet_entry(dvs, 'Vnet1', tunnel_name, '10001', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet1')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet1', '10001')
        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '10.1.0.32')

        create_vxlan_tunnel_map(dvs, tunnel_name, 'map_1', 'Vlan1000', '1000')

        delete_vnet_entry(dvs, 'Vnet1')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet1')
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 7 - Test for vnet tunnel routes with ECMP nexthop group
    '''
    def test_vnet_orch_7(self, dvs, ordered_ecmp, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_7' + ordered_ecmp
        vnet_name = 'Vnet7' + ordered_ecmp

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '7.7.7.7')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10007', "")

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '10007')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '7.7.7.7')

        # Create an ECMP tunnel route
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", vnet_name, '7.0.0.3,7.0.0.2,7.0.0.1')
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['7.0.0.1', '7.0.0.2', '7.0.0.3'], tunnel_name, ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '2', '3'])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['7.0.0.1', '7.0.0.2', '7.0.0.3'])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Set the tunnel route to another nexthop group
        set_vnet_routes(dvs, "100.100.1.1/32", vnet_name, '7.0.0.1,7.0.0.2,7.0.0.4,7.0.0.3')
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['7.0.0.1', '7.0.0.2', '7.0.0.3', '7.0.0.4'], tunnel_name, route_ids=route1, 
                                                         ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '2', '3', '4'])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['7.0.0.1', '7.0.0.2', '7.0.0.3', '7.0.0.4'])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_1 not in vnet_obj.nhgs

        # Create another tunnel route to the same set of endpoints
        create_vnet_routes(dvs, "100.100.2.1/32", vnet_name, '7.0.0.1,7.0.0.2,7.0.0.3,7.0.0.4')
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['7.0.0.1', '7.0.0.2', '7.0.0.3', '7.0.0.4'], tunnel_name,
                                                         ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '2', '3', '4'])
        check_state_db_routes(dvs, vnet_name, "100.100.2.1/32", ['7.0.0.1', '7.0.0.2', '7.0.0.3', '7.0.0.4'])
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        assert nhg2_1 == nhg1_2

        # Remove one of the tunnel routes
        delete_vnet_routes(dvs, "100.100.1.1/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Check the nexthop group still exists
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_2 in vnet_obj.nhgs

        # Remove the other tunnel route
        delete_vnet_routes(dvs, "100.100.2.1/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.2.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.2.1/32")
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        # Check the nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg2_1 not in vnet_obj.nhgs

        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 8 - Test for ipv6 vnet tunnel routes with ECMP nexthop group
    '''
    def test_vnet_orch_8(self, dvs, ordered_ecmp, testlog):

        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_8' + ordered_ecmp
        vnet_name = 'Vnet8' + ordered_ecmp


        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, 'fd:8::32')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10008', "")

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '10008')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, 'fd:8::32')

        # Create an ECMP tunnel route
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "fd:8:10::32/128", vnet_name, 'fd:8:1::1,fd:8:1::3,fd:8:1::2')
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3'], tunnel_name, ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '2', '3'])
        check_state_db_routes(dvs, vnet_name, "fd:8:10::32/128", ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3'])
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")

        # Set the tunnel route to another nexthop group
        set_vnet_routes(dvs, "fd:8:10::32/128", vnet_name, 'fd:8:1::2,fd:8:1::3,fd:8:1::1,fd:8:1::4')
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3', 'fd:8:1::4'], tunnel_name, route_ids=route1,
                                                         ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '2', '3', '4'])
        check_state_db_routes(dvs, vnet_name, "fd:8:10::32/128", ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3', 'fd:8:1::4'])
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_1 not in vnet_obj.nhgs

        # Create another tunnel route to the same set of endpoints
        create_vnet_routes(dvs, "fd:8:20::32/128", vnet_name, 'fd:8:1::1,fd:8:1::2,fd:8:1::3,fd:8:1::4')
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3', 'fd:8:1::4'], tunnel_name,
                                                        ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '2', '3', '4'])
        check_state_db_routes(dvs, vnet_name, "fd:8:20::32/128", ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3', 'fd:8:1::4'])
        check_remove_routes_advertisement(dvs, "fd:8:20::32/128")

        assert nhg2_1 == nhg1_2

        # Create another tunnel route with ipv4 prefix to the same set of endpoints
        create_vnet_routes(dvs, "8.0.0.0/24", vnet_name, 'fd:8:1::1,fd:8:1::2,fd:8:1::3,fd:8:1::4')
        route3, nhg3_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3', 'fd:8:1::4'], tunnel_name,
                                                         ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '2', '3', '4'])
        check_state_db_routes(dvs, vnet_name, "8.0.0.0/24", ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3', 'fd:8:1::4'])
        check_remove_routes_advertisement(dvs, "8.0.0.0/24")

        assert nhg3_1 == nhg1_2

        # Remove one of the tunnel routes
        delete_vnet_routes(dvs, "fd:8:10::32/128", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["fd:8:10::32/128"])
        check_remove_state_db_routes(dvs, vnet_name, "fd:8:10::32/128")
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")

        # Check the nexthop group still exists
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_2 in vnet_obj.nhgs

        # Remove tunnel route 2
        delete_vnet_routes(dvs, "fd:8:20::32/128", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["fd:8:20::32/128"])
        check_remove_state_db_routes(dvs, vnet_name, "fd:8:20::32/128")
        check_remove_routes_advertisement(dvs, "fd:8:20::32/128")

        # Remove tunnel route 3
        delete_vnet_routes(dvs, "8.0.0.0/24", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["8.0.0.0/24"])
        check_remove_state_db_routes(dvs, vnet_name, "8.0.0.0/24")
        check_remove_routes_advertisement(dvs, "8.0.0.0/24")

        # Check the nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg2_1 not in vnet_obj.nhgs

        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)
    

        '''
    Test 9 - Test for vnet tunnel routes with ECMP nexthop group with endpoint health monitor
    '''
    def test_vnet_orch_9(self, dvs, ordered_ecmp, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_9' + ordered_ecmp
        vnet_name = 'Vnet9' + ordered_ecmp

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10009', "")

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '10009')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", vnet_name, '9.0.0.1,9.0.0.2,9.0.0.3', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3')

        # default bfd status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Route should be properly configured when all bfd session states go up
        update_bfd_session_state(dvs, '9.1.0.2', 'Up')
        update_bfd_session_state(dvs, '9.1.0.3', 'Up')
        update_bfd_session_state(dvs, '9.1.0.1', 'Up')
        time.sleep(2)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.1', '9.0.0.2', '9.0.0.3'], tunnel_name, ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '2', '3'])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.0.0.1', '9.0.0.2', '9.0.0.3'])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Remove endpoint from group if it goes down
        update_bfd_session_state(dvs, '9.1.0.2', 'Down')
        time.sleep(2)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.1', '9.0.0.3'], tunnel_name, route_ids=route1, nhg=nhg1_1, ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '3'])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.0.0.1', '9.0.0.3'])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Create another tunnel route with endpoint group overlapped with route1
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.2.1/32", vnet_name, '9.0.0.1,9.0.0.2,9.0.0.5', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.5')
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.1'], tunnel_name, ordered_ecmp=ordered_ecmp, nh_seq_id=['1'])
        check_state_db_routes(dvs, vnet_name, "100.100.2.1/32", ['9.0.0.1'])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Update BFD session state and verify route change
        update_bfd_session_state(dvs, '9.1.0.5', 'Up')
        time.sleep(2)
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.1', '9.0.0.5'], tunnel_name, route_ids=route2, nhg=nhg2_1, ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '3'])
        check_state_db_routes(dvs, vnet_name, "100.100.2.1/32", ['9.0.0.1', '9.0.0.5'])
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        # Update BFD state and check route nexthop
        update_bfd_session_state(dvs, '9.1.0.3', 'Down')
        time.sleep(2)

        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.1'], tunnel_name, route_ids=route1, nhg=nhg1_1, ordered_ecmp=ordered_ecmp, nh_seq_id=['1'])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.0.0.1'])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Set the route1 to a new group
        set_vnet_routes(dvs, "100.100.1.1/32", vnet_name, '9.0.0.1,9.0.0.2,9.0.0.3,9.0.0.4', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4')
        update_bfd_session_state(dvs, '9.1.0.4', 'Up')
        time.sleep(2)
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.1', '9.0.0.4'], tunnel_name, route_ids=route1, ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '4'])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.0.0.1', '9.0.0.4'])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_1 not in vnet_obj.nhgs

        # Set BFD session state for a down endpoint to up
        update_bfd_session_state(dvs, '9.1.0.2', 'Up')
        time.sleep(2)
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.1', '9.0.0.2', '9.0.0.4'], tunnel_name, route_ids=route1, nhg=nhg1_2, ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '2', '4'])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.0.0.1', '9.0.0.2', '9.0.0.4'])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Set all endpoint to down state
        update_bfd_session_state(dvs, '9.1.0.1', 'Down')
        update_bfd_session_state(dvs, '9.1.0.2', 'Down')
        update_bfd_session_state(dvs, '9.1.0.3', 'Down')
        update_bfd_session_state(dvs, '9.1.0.4', 'Down')
        time.sleep(2)

        # Confirm the tunnel route is updated in ASIC
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['9.0.0.5'], tunnel_name, route_ids=route2, nhg=nhg2_1, ordered_ecmp=ordered_ecmp, nh_seq_id=['3'])
        check_state_db_routes(dvs, vnet_name, "100.100.2.1/32", ['9.0.0.5'])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        # Remove tunnel route2
        delete_vnet_routes(dvs, "100.100.2.1/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.2.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.2.1/32")
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        # Check the corresponding nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg2_1 not in vnet_obj.nhgs

        # Check the BFD session specific to the endpoint group is removed while others exist
        check_del_bfd_session(dvs, ['9.1.0.5'])
        check_bfd_session(dvs, ['9.1.0.1', '9.1.0.2', '9.1.0.3', '9.1.0.4'])

        # Remove tunnel route 1
        delete_vnet_routes(dvs, "100.100.1.1/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_2 not in vnet_obj.nhgs

        # Confirm the BFD sessions are removed
        check_del_bfd_session(dvs, ['9.1.0.1', '9.1.0.2', '9.1.0.3', '9.1.0.4', '9.1.0.5'])

        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)


    '''
    Test 10 - Test for ipv6 vnet tunnel routes with ECMP nexthop group with endpoint health monitor
    '''
    def test_vnet_orch_10(self, dvs, ordered_ecmp, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_10' + ordered_ecmp
        vnet_name = 'Vnet10' + ordered_ecmp

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, 'fd:10::32')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10010', "")

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '10010')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, 'fd:10::32')

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "fd:10:10::1/128", vnet_name, 'fd:10:1::1,fd:10:1::2,fd:10:1::3', ep_monitor='fd:10:2::1,fd:10:2::2,fd:10:2::3')

        # default bfd status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["fd:10:10::1/128"])
        check_state_db_routes(dvs, vnet_name, "fd:10:10::1/128", [])
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")

        # Route should be properly configured when all bfd session states go up
        update_bfd_session_state(dvs, 'fd:10:2::2', 'Up')
        update_bfd_session_state(dvs, 'fd:10:2::3', 'Up')
        update_bfd_session_state(dvs, 'fd:10:2::1', 'Up')
        time.sleep(2)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['fd:10:1::1', 'fd:10:1::2', 'fd:10:1::3'], tunnel_name, ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '2', '3'])
        check_state_db_routes(dvs, vnet_name, "fd:10:10::1/128", ['fd:10:1::1', 'fd:10:1::2', 'fd:10:1::3'])
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")

        # Remove endpoint from group if it goes down
        update_bfd_session_state(dvs, 'fd:10:2::2', 'Down')
        time.sleep(2)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['fd:10:1::1', 'fd:10:1::3'], tunnel_name, route_ids=route1, nhg=nhg1_1, ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '3'])
        check_state_db_routes(dvs, vnet_name, "fd:10:10::1/128", ['fd:10:1::1', 'fd:10:1::3'])
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")

        # Create another tunnel route with endpoint group overlapped with route1
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "fd:10:20::1/128", vnet_name, 'fd:10:1::1,fd:10:1::2,fd:10:1::5', ep_monitor='fd:10:2::1,fd:10:2::2,fd:10:2::5')
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['fd:10:1::1'], tunnel_name, ordered_ecmp=ordered_ecmp, nh_seq_id=['1'])
        check_state_db_routes(dvs, vnet_name, "fd:10:20::1/128", ['fd:10:1::1'])
        check_remove_routes_advertisement(dvs, "fd:10:20::1/128")

        # Update BFD session state and verify route change
        update_bfd_session_state(dvs, 'fd:10:2::5', 'Up')
        time.sleep(2)
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['fd:10:1::1', 'fd:10:1::5'], tunnel_name, route_ids=route2, nhg=nhg2_1, ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '3'])
        check_state_db_routes(dvs, vnet_name, "fd:10:20::1/128", ['fd:10:1::1', 'fd:10:1::5'])
        check_remove_routes_advertisement(dvs, "fd:10:20::1/128")

        # Update BFD state and check route nexthop
        update_bfd_session_state(dvs, 'fd:10:2::3', 'Down')
        update_bfd_session_state(dvs, 'fd:10:2::2', 'Up')
        time.sleep(2)

        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['fd:10:1::1', 'fd:10:1::2'], tunnel_name, route_ids=route1, nhg=nhg1_1, ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '2'])
        check_state_db_routes(dvs, vnet_name, "fd:10:10::1/128", ['fd:10:1::1', 'fd:10:1::2'])
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")

        # Set the route to a new group
        set_vnet_routes(dvs, "fd:10:10::1/128", vnet_name, 'fd:10:1::1,fd:10:1::2,fd:10:1::3,fd:10:1::4', ep_monitor='fd:10:2::1,fd:10:2::2,fd:10:2::3,fd:10:2::4')
        update_bfd_session_state(dvs, 'fd:10:2::4', 'Up')
        time.sleep(2)
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['fd:10:1::1', 'fd:10:1::2', 'fd:10:1::4'], tunnel_name, route_ids=route1, ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '2', '4'])
        check_state_db_routes(dvs, vnet_name, "fd:10:10::1/128", ['fd:10:1::1', 'fd:10:1::2', 'fd:10:1::4'])
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")
        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_1 not in vnet_obj.nhgs

        # Set BFD session state for a down endpoint to up
        update_bfd_session_state(dvs, 'fd:10:2::3', 'Up')
        time.sleep(2)
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['fd:10:1::1', 'fd:10:1::2', 'fd:10:1::3', 'fd:10:1::4'], tunnel_name, route_ids=route1, nhg=nhg1_2,
                                                         ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '2', '3', '4'])
        check_state_db_routes(dvs, vnet_name, "fd:10:10::1/128", ['fd:10:1::1', 'fd:10:1::2', 'fd:10:1::3', 'fd:10:1::4'])
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")

        # Set all endpoint to down state
        update_bfd_session_state(dvs, 'fd:10:2::1', 'Down')
        update_bfd_session_state(dvs, 'fd:10:2::2', 'Down')
        update_bfd_session_state(dvs, 'fd:10:2::3', 'Down')
        update_bfd_session_state(dvs, 'fd:10:2::4', 'Down')
        time.sleep(2)

        # Confirm the tunnel route is updated in ASIC
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["fd:10:10::1/128"])
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['fd:10:1::5'], tunnel_name, route_ids=route2, nhg=nhg2_1, ordered_ecmp=ordered_ecmp, nh_seq_id=['3'])
        check_state_db_routes(dvs, vnet_name, "fd:10:20::1/128", ['fd:10:1::5'])
        check_state_db_routes(dvs, vnet_name, "fd:10:10::1/128", [])
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:10:20::1/128")

        # Remove tunnel route2
        delete_vnet_routes(dvs, "fd:10:20::1/128", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["fd:10:20::1/128"])
        check_remove_state_db_routes(dvs, vnet_name, "fd:10:20::1/128")
        check_remove_routes_advertisement(dvs, "fd:10:20::1/128")

        # Check the corresponding nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg2_1 not in vnet_obj.nhgs

        # Check the BFD session specific to the endpoint group is removed while others exist
        check_del_bfd_session(dvs, ['fd:10:2::5'])
        check_bfd_session(dvs, ['fd:10:2::1', 'fd:10:2::2', 'fd:10:2::3', 'fd:10:2::4'])

        # Check the BFD session specific to the endpoint group is removed while others exist
        check_del_bfd_session(dvs, ['fd:10:2::5'])
        check_bfd_session(dvs, ['fd:10:2::1', 'fd:10:2::2', 'fd:10:2::3', 'fd:10:2::4'])

        # Remove tunnel route 1
        delete_vnet_routes(dvs, "fd:10:10::1/128", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["fd:10:10::1/128"])
        check_remove_state_db_routes(dvs, vnet_name, "fd:10:10::1/128")
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")

        # Confirm the BFD sessions are removed
        check_del_bfd_session(dvs, ['fd:10:2::1', 'fd:10:2::2', 'fd:10:2::3', 'fd:10:2::4', 'fd:10:2::5'])

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_2 not in vnet_obj.nhgs

        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 11 - Test for vnet tunnel routes with both single endpoint and ECMP group with endpoint health monitor
    '''
    def test_vnet_orch_11(self, dvs, ordered_ecmp, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_11' + ordered_ecmp
        vnet_name = 'Vnet11' + ordered_ecmp

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '11.11.11.11')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '100011', "")

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '100011')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '11.11.11.11')

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", vnet_name, '11.0.0.1', ep_monitor='11.1.0.1')

        # default bfd status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Route should be properly configured when bfd session state goes up
        update_bfd_session_state(dvs, '11.1.0.1', 'Up')
        time.sleep(2)
        vnet_obj.check_vnet_routes(dvs, vnet_name, '11.0.0.1', tunnel_name)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['11.0.0.1'])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Create another tunnel route with endpoint group overlapped with route1
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.2.1/32", vnet_name, '11.0.0.2,11.0.0.1', ep_monitor='11.1.0.2,11.1.0.1')
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['11.0.0.1'], tunnel_name, ordered_ecmp=ordered_ecmp, nh_seq_id=['1'])
        check_state_db_routes(dvs, vnet_name, "100.100.2.1/32", ['11.0.0.1'])
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        # Create a third tunnel route with another endpoint
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.3.1/32", vnet_name, '11.0.0.2', ep_monitor='11.1.0.2')

        # Update BFD session state and verify route change
        update_bfd_session_state(dvs, '11.1.0.2', 'Up')
        time.sleep(2)
        vnet_obj.check_vnet_routes(dvs, vnet_name, '11.0.0.2', tunnel_name)
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['11.0.0.1', '11.0.0.2'], tunnel_name, route_ids=route2, nhg=nhg2_1, ordered_ecmp=ordered_ecmp, nh_seq_id=['1', '2'])
        check_state_db_routes(dvs, vnet_name, "100.100.3.1/32", ['11.0.0.2'])
        check_state_db_routes(dvs, vnet_name, "100.100.2.1/32", ['11.0.0.1', '11.0.0.2'])
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.3.1/32")


        update_bfd_session_state(dvs, '11.1.0.1', 'Down')
        time.sleep(2)
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['11.0.0.2'], tunnel_name, route_ids=route2, nhg=nhg2_1, ordered_ecmp=ordered_ecmp, nh_seq_id=['2'])
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_state_db_routes(dvs, vnet_name, "100.100.2.1/32", ['11.0.0.2'])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")


        # Set the route1 to a new endpoint
        vnet_obj.fetch_exist_entries(dvs)
        set_vnet_routes(dvs, "100.100.1.1/32", vnet_name, '11.0.0.2', ep_monitor='11.1.0.2')
        vnet_obj.check_vnet_routes(dvs, vnet_name, '11.0.0.2', tunnel_name)
        check_state_db_routes(dvs, vnet_name, "100.100.3.1/32", ['11.0.0.2'])
        check_remove_routes_advertisement(dvs, "100.100.3.1/32")

        # Remove tunnel route2
        delete_vnet_routes(dvs, "100.100.2.1/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.2.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.2.1/32")
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        # Check the corresponding nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg2_1 not in vnet_obj.nhgs

        # Check the BFD session specific to the endpoint group is removed while others exist
        check_del_bfd_session(dvs, ['11.1.0.1'])
        check_bfd_session(dvs, ['11.1.0.2'])

        # Remove tunnel route 1
        delete_vnet_routes(dvs, "100.100.1.1/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Remove tunnel route 3
        delete_vnet_routes(dvs, "100.100.3.1/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.3.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.3.1/32")
        check_remove_routes_advertisement(dvs, "100.100.3.1/32")

        # Confirm the BFD sessions are removed
        check_del_bfd_session(dvs, ['11.1.0.1', '11.1.0.2'])

        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)


    '''
    Test 12 - Test for vnet tunnel routes with ECMP nexthop group with endpoint health monitor and route advertisement
    '''
    def test_vnet_orch_12(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_12'

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '12.12.12.12')
        create_vnet_entry(dvs, 'Vnet12', tunnel_name, '10012', "", advertise_prefix=True)

        vnet_obj.check_vnet_entry(dvs, 'Vnet12')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet12', '10012')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '12.12.12.12')

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", 'Vnet12', '12.0.0.1,12.0.0.2,12.0.0.3', ep_monitor='12.1.0.1,12.1.0.2,12.1.0.3', profile="test_profile")

        # default bfd status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet12', ["100.100.1.1/32"])
        check_state_db_routes(dvs, 'Vnet12', "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Route should be properly configured when all bfd session states go up
        update_bfd_session_state(dvs, '12.1.0.1', 'Up')
        update_bfd_session_state(dvs, '12.1.0.2', 'Up')
        update_bfd_session_state(dvs, '12.1.0.3', 'Up')
        time.sleep(2)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet12', ['12.0.0.1', '12.0.0.2', '12.0.0.3'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet12', "100.100.1.1/32", ['12.0.0.1', '12.0.0.2', '12.0.0.3'])
        check_routes_advertisement(dvs, "100.100.1.1/32", "test_profile")

        # Remove endpoint from group if it goes down
        update_bfd_session_state(dvs, '12.1.0.2', 'Down')
        time.sleep(2)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet12', ['12.0.0.1', '12.0.0.3'], tunnel_name, route_ids=route1, nhg=nhg1_1)
        check_state_db_routes(dvs, 'Vnet12', "100.100.1.1/32", ['12.0.0.1', '12.0.0.3'])
        check_routes_advertisement(dvs, "100.100.1.1/32", "test_profile")

        # Create another tunnel route with endpoint group overlapped with route1
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.2.1/32", 'Vnet12', '12.0.0.1,12.0.0.2,12.0.0.5', ep_monitor='12.1.0.1,12.1.0.2,12.1.0.5')
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet12', ['12.0.0.1'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet12', "100.100.2.1/32", ['12.0.0.1'])
        check_routes_advertisement(dvs, "100.100.1.1/32")

        # Update BFD session state and verify route change
        update_bfd_session_state(dvs, '12.1.0.5', 'Up')
        time.sleep(2)
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet12', ['12.0.0.1', '12.0.0.5'], tunnel_name, route_ids=route2, nhg=nhg2_1)
        check_state_db_routes(dvs, 'Vnet12', "100.100.2.1/32", ['12.0.0.1', '12.0.0.5'])
        check_routes_advertisement(dvs, "100.100.2.1/32")

        # Update BFD state and check route nexthop
        update_bfd_session_state(dvs, '12.1.0.3', 'Down')
        time.sleep(2)

        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet12', ['12.0.0.1'], tunnel_name, route_ids=route1, nhg=nhg1_1)
        check_state_db_routes(dvs, 'Vnet12', "100.100.1.1/32", ['12.0.0.1'])
        check_routes_advertisement(dvs, "100.100.1.1/32", "test_profile")

        # Set the route1 to a new group
        set_vnet_routes(dvs, "100.100.1.1/32", 'Vnet12', '12.0.0.1,12.0.0.2,12.0.0.3,12.0.0.4', ep_monitor='12.1.0.1,12.1.0.2,12.1.0.3,12.1.0.4', profile="test_profile2")
        update_bfd_session_state(dvs, '12.1.0.4', 'Up')
        time.sleep(2)
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet12', ['12.0.0.1', '12.0.0.4'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, 'Vnet12', "100.100.1.1/32", ['12.0.0.1', '12.0.0.4'])
        check_routes_advertisement(dvs, "100.100.1.1/32", "test_profile2")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_1 not in vnet_obj.nhgs

        # Set BFD session state for a down endpoint to up
        update_bfd_session_state(dvs, '12.1.0.2', 'Up')
        time.sleep(2)
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet12', ['12.0.0.1', '12.0.0.2', '12.0.0.4'], tunnel_name, route_ids=route1, nhg=nhg1_2)
        check_state_db_routes(dvs, 'Vnet12', "100.100.1.1/32", ['12.0.0.1', '12.0.0.2', '12.0.0.4'])
        check_routes_advertisement(dvs, "100.100.1.1/32", "test_profile2")

        # Set all endpoint to down state
        update_bfd_session_state(dvs, '12.1.0.1', 'Down')
        update_bfd_session_state(dvs, '12.1.0.2', 'Down')
        update_bfd_session_state(dvs, '12.1.0.3', 'Down')
        update_bfd_session_state(dvs, '12.1.0.4', 'Down')
        time.sleep(2)

        # Confirm the tunnel route is updated in ASIC
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet12', ["100.100.1.1/32"])
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet12', ['12.0.0.5'], tunnel_name, route_ids=route2, nhg=nhg2_1)
        check_state_db_routes(dvs, 'Vnet12', "100.100.2.1/32", ['12.0.0.5'])
        check_state_db_routes(dvs, 'Vnet12', "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")
        check_routes_advertisement(dvs, "100.100.2.1/32")

        # Remove tunnel route2
        delete_vnet_routes(dvs, "100.100.2.1/32", 'Vnet12')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet12', ["100.100.2.1/32"])
        check_remove_state_db_routes(dvs, 'Vnet12', "100.100.2.1/32")
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        # Check the corresponding nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg2_1 not in vnet_obj.nhgs

        # Check the BFD session specific to the endpoint group is removed while others exist
        check_del_bfd_session(dvs, ['12.1.0.5'])
        check_bfd_session(dvs, ['12.1.0.1', '12.1.0.2', '12.1.0.3', '12.1.0.4'])

        # Remove tunnel route 1
        delete_vnet_routes(dvs, "100.100.1.1/32", 'Vnet12')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet12', ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, 'Vnet12', "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_2 not in vnet_obj.nhgs

        # Confirm the BFD sessions are removed
        check_del_bfd_session(dvs, ['12.1.0.1', '12.1.0.2', '12.1.0.3', '12.1.0.4', '12.1.0.5'])

        delete_vnet_entry(dvs, 'Vnet12')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet12')
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 13 - Test for configuration idempotent behaviour
    '''
    def test_vnet_orch_13(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_13'
        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, 'fd:8::32')
        create_vnet_entry(dvs, 'Vnet13', tunnel_name, '10008', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet13')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet13', '10008')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, 'fd:8::32')

        # Create an ECMP tunnel route
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "fd:8:10::32/128", 'Vnet13', 'fd:8:1::1,fd:8:1::2,fd:8:1::3')
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet13', ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet13', "fd:8:10::32/128", ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")

        # readd same tunnel again
        set_vnet_routes(dvs, "fd:8:10::32/128", 'Vnet13', 'fd:8:1::1,fd:8:1::2,fd:8:1::3')
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet13', ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, 'Vnet13', "fd:8:10::32/128", ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")
        # Check only one group is present
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_1 in vnet_obj.nhgs
        assert len(vnet_obj.nhgs) == 1
        assert nhg1_1 == nhg1_2

        # Remove one of the tunnel routes
        delete_vnet_routes(dvs, "fd:8:10::32/128", 'Vnet13')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet13', ["fd:8:10::32/128"])
        check_remove_state_db_routes(dvs, 'Vnet13', "fd:8:10::32/128")
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")

        # Check the nexthop group still exists
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_2 not in  vnet_obj.nhgs
        assert len(vnet_obj.nhgs) == 0
        delete_vnet_entry(dvs, 'Vnet13')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet13')

    '''
    Test 14 - Test for configuration idempotent behaviour 2
    '''
    def test_vnet_orch_14(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_14'
        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, 'fd:8::32')
        create_vnet_entry(dvs, 'Vnet14', tunnel_name, '10008', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet14')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet14', '10008')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, 'fd:8::32')

        # Create an ECMP tunnel route
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "fd:8:10::32/128", 'Vnet14', 'fd:8:1::1,fd:8:1::2,fd:8:1::3')
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet14', ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet14', "fd:8:10::32/128", ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")

        # readd same tunnel again
        set_vnet_routes(dvs, "fd:8:10::32/128", 'Vnet14', 'fd:8:1::1,fd:8:1::2,fd:8:1::3')
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet14', ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, 'Vnet14', "fd:8:10::32/128", ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")

        #update nexthops for the same tunnel.
        set_vnet_routes(dvs, "fd:8:10::32/128", 'Vnet14', 'fd:8:1::1,fd:8:1::2,fd:8:1::3,fd:8:1::4')
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet14', ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3', 'fd:8:1::4'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, 'Vnet14', "fd:8:10::32/128", ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3', 'fd:8:1::4'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_1 not in vnet_obj.nhgs
        assert nhg1_2 in vnet_obj.nhgs

        # Remove the tunnel route
        delete_vnet_routes(dvs, "fd:8:10::32/128", 'Vnet14')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet14', ["fd:8:10::32/128"])
        check_remove_state_db_routes(dvs, 'Vnet14', "fd:8:10::32/128")
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")
        # Remove the tunnel route
        delete_vnet_routes(dvs, "fd:8:10::32/128", 'Vnet14')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet14', ["fd:8:10::32/128"])
        check_remove_state_db_routes(dvs, 'Vnet14', "fd:8:10::32/128")
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")

        # Check the nexthop group still exists
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_2 not in  vnet_obj.nhgs
        assert nhg1_1 not in  vnet_obj.nhgs

        delete_vnet_entry(dvs, 'Vnet14')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet14')
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 15 - Test for configuration idempotent behaviour single endpoint
    '''
    def test_vnet_orch_15(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_15'
        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, 'fd:8::32')
        create_vnet_entry(dvs, 'Vnet15', tunnel_name, '10008', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet15')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet15', '10008')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, 'fd:8::32')

        # Create an tunnel route
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "fd:8:10::32/128", 'Vnet15', 'fd:8:1::1')
        route1 = vnet_obj.check_vnet_routes(dvs, 'Vnet15', 'fd:8:1::1', tunnel_name)
        check_state_db_routes(dvs, 'Vnet15', "fd:8:10::32/128", ['fd:8:1::1'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")

        # readd same tunnel again
        set_vnet_routes(dvs, "fd:8:10::32/128", 'Vnet15', 'fd:8:1::1')
        route1 = vnet_obj.check_vnet_routes(dvs, 'Vnet15', 'fd:8:1::1', tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, 'Vnet15', "fd:8:10::32/128", ['fd:8:1::1'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")
        # Check only one group is present
        vnet_obj.fetch_exist_entries(dvs)
        assert len(vnet_obj.nhops) == 1

        # Remove one of the tunnel routes
        delete_vnet_routes(dvs, "fd:8:10::32/128", 'Vnet15')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet15', ["fd:8:10::32/128"])
        check_remove_state_db_routes(dvs, 'Vnet15', "fd:8:10::32/128")
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")

        # Check the nexthop group still exists
        vnet_obj.fetch_exist_entries(dvs)
        assert len(vnet_obj.nhops) == 0
        delete_vnet_entry(dvs, 'Vnet15')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet15')
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 16 - Test for configuration idempotent behaviour single endpoint with BFD
    '''
    def test_vnet_orch_16(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_16'
        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, 'fd:8::33')
        create_vnet_entry(dvs, 'Vnet16', tunnel_name, '10008', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet16')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet16', '10008')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, 'fd:8::33')

        # Create a tunnel route
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "fd:8:11::32/128", 'Vnet16', 'fd:8:2::1', ep_monitor='fd:8:2::1')
        update_bfd_session_state(dvs, 'fd:8:2::1', 'Up')
        time.sleep(2)

        route1 = vnet_obj.check_vnet_routes(dvs, 'Vnet16', 'fd:8:2::1', tunnel_name)
        check_state_db_routes(dvs, 'Vnet16', "fd:8:11::32/128", ['fd:8:2::1'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:8:11::32/128")

        # readd same tunnel again
        set_vnet_routes(dvs, "fd:8:11::32/128", 'Vnet16', 'fd:8:2::1', ep_monitor='fd:8:2::1')
        route1 = vnet_obj.check_vnet_routes(dvs, 'Vnet16', 'fd:8:2::1', tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, 'Vnet16', "fd:8:11::32/128", ['fd:8:2::1'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:8:11::32/128")
        # Check only one group is present
        vnet_obj.fetch_exist_entries(dvs)
        assert len(vnet_obj.nhops) == 1

        update_bfd_session_state(dvs, 'fd:8:2::1', 'Down')
        time.sleep(2)
        # readd same tunnel again
        set_vnet_routes(dvs, "fd:8:11::32/128", 'Vnet16', 'fd:8:2::1', ep_monitor='fd:8:2::1')

        update_bfd_session_state(dvs, 'fd:8:2::1', 'Up')
        time.sleep(2)

        route1 = vnet_obj.check_vnet_routes(dvs, 'Vnet16', 'fd:8:2::1', tunnel_name,route_ids=route1)
        check_state_db_routes(dvs, 'Vnet16', "fd:8:11::32/128", ['fd:8:2::1'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:8:11::32/128")


        # Remove one of the tunnel routes
        delete_vnet_routes(dvs, "fd:8:11::32/128", 'Vnet16')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet16', ["fd:8:11::32/128"])
        check_remove_state_db_routes(dvs, 'Vnet16', "fd:8:11::32/128")
        check_remove_routes_advertisement(dvs, "fd:8:11::32/128")

        # Check the nexthop group still exists
        vnet_obj.fetch_exist_entries(dvs)
        assert len(vnet_obj.nhops) == 0
        delete_vnet_entry(dvs, 'Vnet16')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet16')
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 17 - Test for configuration idempotent behaviour multiple endpoint with BFD
    '''
    def test_vnet_orch_17(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_17'

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')
        create_vnet_entry(dvs, 'Vnet17', tunnel_name, '10017', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet17')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet17', '10017')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", 'Vnet17', '9.0.0.1,9.0.0.2,9.0.0.3', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3')

        # default bfd status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet17', ["100.100.1.1/32"])
        check_state_db_routes(dvs, 'Vnet17', "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        #readd the route
        set_vnet_routes(dvs, "100.100.1.1/32", 'Vnet17', '9.0.0.1,9.0.0.2,9.0.0.3',ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet17', ["100.100.1.1/32"])
        check_state_db_routes(dvs, 'Vnet17', "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Route should be properly configured when all bfd session states go up
        update_bfd_session_state(dvs, '9.1.0.1', 'Up')
        update_bfd_session_state(dvs, '9.1.0.2', 'Up')
        update_bfd_session_state(dvs, '9.1.0.3', 'Up')
        time.sleep(2)

        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet17', ['9.0.0.1', '9.0.0.2', '9.0.0.3'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet17', "100.100.1.1/32", ['9.0.0.1', '9.0.0.2', '9.0.0.3'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        #readd the active route
        set_vnet_routes(dvs, "100.100.1.1/32", 'Vnet17', '9.0.0.1,9.0.0.2,9.0.0.3',ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3')
        route2, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet17', ['9.0.0.1', '9.0.0.2', '9.0.0.3'], tunnel_name, route_ids=route1, nhg=nhg1_1)
        check_state_db_routes(dvs, 'Vnet17', "100.100.1.1/32", ['9.0.0.1', '9.0.0.2', '9.0.0.3'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")
        assert nhg1_1 == nhg1_2
        assert len(vnet_obj.nhgs) == 1

        # Remove tunnel route
        delete_vnet_routes(dvs, "100.100.1.1/32", 'Vnet17')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet17', ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, 'Vnet17', "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Check the corresponding nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_1 not in vnet_obj.nhgs
        # Check the BFD session specific to the endpoint group is removed while others exist
        check_del_bfd_session(dvs, ['9.1.0.1', '9.1.0.2', '9.1.0.3'])

        delete_vnet_entry(dvs, 'Vnet17')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet17')
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 18 - Test for priority vnet tunnel routes with ECMP nexthop group. test primary secondary switchover.
    '''
    def test_vnet_orch_18(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()
        tunnel_name = 'tunnel_18'
        vnet_name = 'vnet18'
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10018', "", advertise_prefix=True, overlay_dmac="22:33:33:44:44:66")

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '10018')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", vnet_name, '9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4', primary ='9.1.0.1,9.1.0.2', monitoring='custom', adv_prefix='100.100.1.0/24')

        # default monitor status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")

        # Route should be properly configured when all monitor session states go up. Only primary Endpoints should be in use.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.1', 'up')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.2', 'up')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.3', 'up')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.4', 'up')

        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1','9.1.0.2'], tunnel_name)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1','9.1.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        # Remove first primary endpoint from group.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.2', 'down')
        time.sleep(2)
        route1= vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        # Switch to secondary if both primary down
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.1', 'down')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.3','9.1.0.4'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.3','9.1.0.4'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        # removing first endpoint of secondary. route should remain on secondary NHG
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.3', 'down')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.4'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.4'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        # removing last endpoint of secondary. route should be removed
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.4', 'down')
        time.sleep(2)

        new_nhgs = get_all_created_entries(asic_db, vnet_obj.ASIC_NEXT_HOP_GROUP, [])
        assert len(new_nhgs) == 0
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.1.1/32")

        #Route should come up with secondary endpoints.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.3', 'up')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.4', 'up')

        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.3','9.1.0.4'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.3','9.1.0.4'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        #Route should be switched to the primary endpoint.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.1', 'up')
        time.sleep(2)
        route1= vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        #Route should be updated with the second primary endpoint.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.2', 'up')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1','9.1.0.2'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1','9.1.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        #Route should not be impacted by seconday endpoints going down.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.3', 'down')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.4', 'down')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1','9.1.0.2'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1','9.1.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        #Route should not be impacted by seconday endpoints coming back up.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.3', 'up')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.4', 'up')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1','9.1.0.2'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1','9.1.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        # Remove tunnel route 1
        delete_vnet_routes(dvs, "100.100.1.1/32", vnet_name)
        time.sleep(2)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")

        # Confirm the monitor sessions are removed
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.1/32", "9.1.0.1")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.1/32", "9.1.0.2")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.1/32", "9.1.0.3")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.1/32", "9.1.0.4")

        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 19 - Test for 2 priority vnet tunnel routes with overlapping primary secondary ECMP nexthop group.
    '''
    def test_vnet_orch_19(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()
        tunnel_name = 'tunnel_19'
        vnet_name = 'Vnet19'
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '9.9.9.19')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10019', "", advertise_prefix=True, overlay_dmac="22:33:33:44:44:66")

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '10019')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '9.9.9.19')

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", vnet_name, '9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4', profile="Test_profile", primary ='9.1.0.1,9.1.0.2', monitoring='custom', adv_prefix='100.100.1.0/24')
        create_vnet_routes(dvs, "200.100.1.1/32", vnet_name, '9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4', primary ='9.1.0.3,9.1.0.4', monitoring='custom', adv_prefix='200.100.1.0/24')

        # default monitor session status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")

        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["200.100.1.1/32"])
        check_state_db_routes(dvs, vnet_name, "200.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "200.100.1.0/24")

        # Route should be properly configured when all monitor session states go up. Only primary Endpoints should be in use.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.1', 'up')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1'], tunnel_name, prefix="100.100.1.1/32")
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1'])
        check_routes_advertisement(dvs, "100.100.1.0/24", "Test_profile")

        update_monitor_session_state(dvs, '200.100.1.1/32', '9.1.0.1', 'up')
        time.sleep(2)
        route2 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1'], tunnel_name, route_ids=route1, prefix="200.100.1.1/32")
        check_state_db_routes(dvs, vnet_name, "200.100.1.1/32", ['9.1.0.1'])
        check_routes_advertisement(dvs, "200.100.1.0/24", "")

        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.2', 'up')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1','9.1.0.2'], tunnel_name, route_ids=route1, prefix="100.100.1.1/32")
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1','9.1.0.2'])
        check_routes_advertisement(dvs, "100.100.1.0/24", "Test_profile")

        update_monitor_session_state(dvs, '200.100.1.1/32', '9.1.0.2', 'up')
        time.sleep(2)
        route2 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1','9.1.0.2'], tunnel_name, route_ids=route1, prefix="200.100.1.1/32")
        check_state_db_routes(dvs, vnet_name, "200.100.1.1/32", ['9.1.0.1','9.1.0.2'])
        check_routes_advertisement(dvs, "200.100.1.0/24", "")

        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.3', 'up')
        update_monitor_session_state(dvs, '200.100.1.1/32', '9.1.0.3', 'up')
        time.sleep(2)

        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1','9.1.0.2'], tunnel_name, route_ids=route1, prefix="100.100.1.1/32")
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1','9.1.0.2'])
        check_routes_advertisement(dvs, "100.100.1.0/24", "Test_profile")

        route2 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.3'], tunnel_name, route_ids=route1, prefix="200.100.1.1/32")
        check_state_db_routes(dvs, vnet_name, "200.100.1.1/32", ['9.1.0.3'])
        check_routes_advertisement(dvs, "200.100.1.0/24", "")

        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.4', 'up')
        update_monitor_session_state(dvs, '200.100.1.1/32', '9.1.0.4', 'up')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1','9.1.0.2'], tunnel_name, route_ids=route1, prefix="100.100.1.1/32")
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1','9.1.0.2'])
        check_routes_advertisement(dvs, "100.100.1.0/24", "Test_profile")

        route2 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.3','9.1.0.4'], tunnel_name, route_ids=route1, prefix="200.100.1.1/32")
        check_state_db_routes(dvs, vnet_name, "200.100.1.1/32", ['9.1.0.3','9.1.0.4'])
        check_routes_advertisement(dvs, "200.100.1.0/24", "")

        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.1', 'down')
        update_monitor_session_state(dvs, '200.100.1.1/32', '9.1.0.1', 'down')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.2'], tunnel_name, route_ids=route1, prefix="100.100.1.1/32")
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.2'])
        check_routes_advertisement(dvs, "100.100.1.0/24", "Test_profile")

        route2 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.3','9.1.0.4'], tunnel_name, route_ids=route1, prefix="200.100.1.1/32")
        check_state_db_routes(dvs, vnet_name, "200.100.1.1/32", ['9.1.0.3','9.1.0.4'])
        check_routes_advertisement(dvs, "200.100.1.0/24", "")

        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.2', 'down')
        update_monitor_session_state(dvs, '200.100.1.1/32', '9.1.0.2', 'down')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.3','9.1.0.4'], tunnel_name, route_ids=route1, prefix="100.100.1.1/32")
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.3','9.1.0.4'])
        check_routes_advertisement(dvs, "100.100.1.0/24", "Test_profile")

        route2 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.3','9.1.0.4'], tunnel_name, route_ids=route1, prefix="200.100.1.1/32")
        check_state_db_routes(dvs, vnet_name, "200.100.1.1/32", ['9.1.0.3','9.1.0.4'])
        check_routes_advertisement(dvs, "200.100.1.0/24", "")

        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.3', 'down')
        update_monitor_session_state(dvs, '200.100.1.1/32', '9.1.0.3', 'down')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.4'], tunnel_name, route_ids=route1, prefix="100.100.1.1/32")
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.4'])
        check_routes_advertisement(dvs, "100.100.1.0/24", "Test_profile")

        route2 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.4'], tunnel_name, route_ids=route1, prefix="200.100.1.1/32")
        check_state_db_routes(dvs, vnet_name, "200.100.1.1/32", ['9.1.0.4'])
        check_routes_advertisement(dvs, "200.100.1.0/24", "")

        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.4', 'down')
        update_monitor_session_state(dvs, '200.100.1.1/32', '9.1.0.4', 'down')
        time.sleep(2)

        #we should still have two NHGs but no active route
        new_nhgs = get_all_created_entries(asic_db, vnet_obj.ASIC_NEXT_HOP_GROUP, vnet_obj.nhgs)
        assert len(new_nhgs) == 0
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "200.100.1.1/32")
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["200.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.1.1/32")
        check_remove_state_db_routes(dvs, vnet_name, "200.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")
        check_remove_routes_advertisement(dvs, "200.100.1.0/24")

        # Remove tunnel route 1
        delete_vnet_routes(dvs, "100.100.1.1/32", vnet_name)
        delete_vnet_routes(dvs, "200.100.1.1/32", vnet_name)

        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["200.100.1.1/32"])

        check_remove_state_db_routes(dvs, vnet_name, "100.100.1.1/32")
        check_remove_state_db_routes(dvs, vnet_name, "200.100.1.1/32")

        check_remove_routes_advertisement(dvs, "100.100.1.0/24")
        check_remove_routes_advertisement(dvs, "200.100.1.0/24")


        # Confirm the monitor sessions are removed
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.1/32", "9.1.0.1")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.1/32", "9.1.0.2")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.1/32", "9.1.0.3")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.1/32", "9.1.0.4")

        vnet_obj.check_custom_monitor_deleted(dvs, "200.100.1.1/32", "9.1.0.1")
        vnet_obj.check_custom_monitor_deleted(dvs, "200.100.1.1/32", "9.1.0.2")
        vnet_obj.check_custom_monitor_deleted(dvs, "200.100.1.1/32", "9.1.0.3")
        vnet_obj.check_custom_monitor_deleted(dvs, "200.100.1.1/32", "9.1.0.4")

        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 20 - Test for Single enpoint priority vnet tunnel routes. Test primary secondary switchover.
    '''
    def test_vnet_orch_20(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()
        tunnel_name = 'tunnel_20'
        vnet_name = 'Vnet20'
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10020', "", advertise_prefix=True, overlay_dmac="22:33:33:44:44:66")

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '10020')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", vnet_name, '9.1.0.1,9.1.0.2', ep_monitor='9.1.0.1,9.1.0.2', primary ='9.1.0.1', profile="Test_profile", monitoring='custom', adv_prefix='100.100.1.0/24')

        # default monitor session status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")

        # Route should be properly configured when all monitor session states go up. Only primary Endpoints should be in use.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.1', 'up')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.2', 'up')
        time.sleep(2)
        nhids = get_all_created_entries(asic_db, vnet_obj.ASIC_NEXT_HOP,set())
        tbl_nh =  swsscommon.Table(asic_db, vnet_obj.ASIC_NEXT_HOP)
        nexthops = dict()
        for nhid in nhids:
            status, nh_fvs = tbl_nh.get(nhid)
            nh_fvs = dict(nh_fvs)
            for key in nh_fvs.keys():
                if key == 'SAI_NEXT_HOP_ATTR_IP':
                    nexthops[nh_fvs[key]] = nhid
        assert len(nexthops.keys()) == 1

        route = get_created_entries(asic_db, vnet_obj.ASIC_ROUTE_ENTRY, vnet_obj.routes, 1)
        check_object(asic_db, vnet_obj.ASIC_ROUTE_ENTRY, route[0],
                        {
                            "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID": nexthops['9.1.0.1'],
                        }
                    )
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1'])
        check_routes_advertisement(dvs, "100.100.1.0/24", "Test_profile")

        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.2', 'down')
        time.sleep(2)

        route = get_created_entries(asic_db, vnet_obj.ASIC_ROUTE_ENTRY, vnet_obj.routes, 1)
        check_object(asic_db, vnet_obj.ASIC_ROUTE_ENTRY, route[0],
                        {
                            "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID": nexthops['9.1.0.1'],
                        }
                    )
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1'])
        check_routes_advertisement(dvs, "100.100.1.0/24", "Test_profile")

        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.1', 'down')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.2', 'up')

        time.sleep(2)

        nhids = get_all_created_entries(asic_db, vnet_obj.ASIC_NEXT_HOP,set())
        tbl_nh =  swsscommon.Table(asic_db, vnet_obj.ASIC_NEXT_HOP)
        nexthops = dict()
        for nhid in nhids:
            status, nh_fvs = tbl_nh.get(nhid)
            nh_fvs = dict(nh_fvs)
            for key in nh_fvs.keys():
                if key == 'SAI_NEXT_HOP_ATTR_IP':
                    nexthops[nh_fvs[key]] = nhid
        assert len(nexthops.keys()) == 1

        route = get_created_entries(asic_db, vnet_obj.ASIC_ROUTE_ENTRY, vnet_obj.routes, 1)
        check_object(asic_db, vnet_obj.ASIC_ROUTE_ENTRY, route[0],
                        {
                            "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID": nexthops['9.1.0.2'],
                        }
                    )
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.2'])
        check_routes_advertisement(dvs, "100.100.1.0/24", "Test_profile")

        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.1', 'up')
        time.sleep(2)

        nhids = get_all_created_entries(asic_db, vnet_obj.ASIC_NEXT_HOP,set())
        tbl_nh =  swsscommon.Table(asic_db, vnet_obj.ASIC_NEXT_HOP)
        nexthops = dict()
        for nhid in nhids:
            status, nh_fvs = tbl_nh.get(nhid)
            nh_fvs = dict(nh_fvs)
            for key in nh_fvs.keys():
                if key == 'SAI_NEXT_HOP_ATTR_IP':
                    nexthops[nh_fvs[key]] = nhid
        assert len(nexthops.keys()) == 1

        route = get_created_entries(asic_db, vnet_obj.ASIC_ROUTE_ENTRY, vnet_obj.routes, 1)
        check_object(asic_db, vnet_obj.ASIC_ROUTE_ENTRY, route[0],
                        {
                            "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID": nexthops['9.1.0.1'],
                        }
                    )
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1'])
        check_routes_advertisement(dvs, "100.100.1.0/24", "Test_profile")

        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.1', 'down')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.2', 'down')

        time.sleep(2)

        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "200.100.1.0/24")


        # Remove tunnel route 1
        delete_vnet_routes(dvs, "100.100.1.1/32", vnet_name)

        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")

        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.1/32", "9.1.0.1")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.1/32", "9.1.0.2")

        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 21 - Test for priority vxlan tunnel with adv_prefix, adv profile. test route re-addition, route update, primary seocndary swap.
    '''
    def test_vnet_orch_21(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_21'
        vnet_name = "Vnet21"
        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, 'fd:10::32')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10021', "", advertise_prefix=True, overlay_dmac="22:33:33:44:44:66")

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '10021')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, 'fd:10::32')
        vnet_obj.fetch_exist_entries(dvs)

        #Add first Route
        create_vnet_routes(dvs, "fd:10:10::1/128", vnet_name, 'fd:10:1::1,fd:10:1::2,fd:10:1::3,fd:10:1::4', ep_monitor='fd:10:2::1,fd:10:2::2,fd:10:2::3,fd:10:2::4', profile = "test_prf", primary ='fd:10:1::3,fd:10:1::4',monitoring='custom', adv_prefix="fd:10:10::/64")
        update_monitor_session_state(dvs, 'fd:10:10::1/128', 'fd:10:2::1', 'up')
        update_monitor_session_state(dvs, 'fd:10:10::1/128', 'fd:10:2::2', 'up')

        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['fd:10:1::1','fd:10:1::2'], tunnel_name, prefix="fd:10:10::1/128")
        check_state_db_routes(dvs, vnet_name, "fd:10:10::1/128", ['fd:10:1::1,fd:10:1::2'])
        check_routes_advertisement(dvs, "fd:10:10::/64", "test_prf")

        #add 2nd route
        create_vnet_routes(dvs, "fd:10:10::21/128", vnet_name, 'fd:11:1::1,fd:11:1::2,fd:11:1::3,fd:11:1::4', ep_monitor='fd:11:2::1,fd:11:2::2,fd:11:2::3,fd:11:2::4', profile = "test_prf", primary ='fd:11:1::1,fd:11:1::2',monitoring='custom', adv_prefix='fd:10:10::/64')
        update_monitor_session_state(dvs, 'fd:10:10::21/128', 'fd:11:2::1', 'up')
        update_monitor_session_state(dvs, 'fd:10:10::21/128', 'fd:11:2::2', 'up')
        update_monitor_session_state(dvs, 'fd:10:10::21/128', 'fd:11:2::3', 'up')
        update_monitor_session_state(dvs, 'fd:10:10::21/128', 'fd:11:2::4', 'up')
        
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['fd:11:1::1','fd:11:1::2'], tunnel_name, route_ids=route1, prefix="fd:10:10::21/128")
        check_state_db_routes(dvs, vnet_name, "fd:10:10::21/128", ['fd:11:1::1,fd:11:1::2'])
        check_routes_advertisement(dvs, "fd:10:10::/64", "test_prf")

        #remove first route
        delete_vnet_routes(dvs, "fd:10:10::1/128", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet12', ["fd:10:10::1/128"])
        check_remove_state_db_routes(dvs, 'Vnet12', "fd:10:10::1/128")

        #adv should still be up.
        check_routes_advertisement(dvs, "fd:10:10::/64")

        #add 3rd route
        create_vnet_routes(dvs, "fd:10:10::31/128", vnet_name, 'fd:11:1::1,fd:11:1::2,fd:11:1::3,fd:11:1::4', ep_monitor='fd:11:2::1,fd:11:2::2,fd:11:2::3,fd:11:2::4', profile = "test_prf", primary ='fd:11:1::1,fd:11:1::2',monitoring='custom', adv_prefix='fd:10:10::/64')
        update_monitor_session_state(dvs, 'fd:10:10::31/128', 'fd:11:2::1', 'up')
        update_monitor_session_state(dvs, 'fd:10:10::31/128', 'fd:11:2::2', 'up')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['fd:11:1::1','fd:11:1::2'], tunnel_name, route_ids=route1, prefix="fd:10:10::31/128")
        check_state_db_routes(dvs, vnet_name, "fd:10:10::31/128", ['fd:11:1::1,fd:11:1::2'])
        check_routes_advertisement(dvs, "fd:10:10::/64", "test_prf")

        #delete 2nd route
        delete_vnet_routes(dvs, "fd:10:10::21/128", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet12', ["fd:10:10::21/128"])
        check_remove_state_db_routes(dvs, 'Vnet12', "fd:10:10::21/128")

        #adv should still be up.
        check_routes_advertisement(dvs, "fd:10:10::/64")

        #remove 3rd route
        delete_vnet_routes(dvs, "fd:10:10::31/128", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet12', ["fd:10:10::31/128"])
        check_remove_state_db_routes(dvs, 'Vnet12', "fd:10:10::31/128")

        #adv should be gone.
        check_remove_routes_advertisement(dvs, "fd:10:10::/64")
        delete_vnet_entry(dvs,vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 22 - Test for vxlan custom monitoring with adv_prefix. Add route twice and change nexthops case
    '''
    def test_vnet_orch_22(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_22'
        vnet_name = "Vnet22"
        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '9.9.9.3')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10022', "", advertise_prefix=True, overlay_dmac="22:33:33:44:44:66")

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '10022')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '9.9.9.3')

        vnet_obj.fetch_exist_entries(dvs)
        #Add first Route
        create_vnet_routes(dvs, "100.100.1.11/32", vnet_name, '19.0.0.1,19.0.0.2,19.0.0.3', ep_monitor='19.1.0.1,19.1.0.2,19.1.0.3', profile = "test_prf", primary ='19.0.0.1',monitoring='custom', adv_prefix='100.100.1.0/24')
        update_monitor_session_state(dvs, '100.100.1.11/32', '19.1.0.1', 'up')
        time.sleep(2)
        vnet_obj.check_vnet_routes(dvs, vnet_name, '19.0.0.1', tunnel_name)
        check_state_db_routes(dvs, vnet_name, "100.100.1.11/32", ['19.0.0.1'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24", "test_prf")

        #Add first Route again
        create_vnet_routes(dvs, "100.100.1.11/32", vnet_name, '19.0.0.1,19.0.0.2,19.0.0.3', ep_monitor='19.1.0.1,19.1.0.2,19.1.0.3', profile = "test_prf", primary ='19.0.0.1',monitoring='custom', adv_prefix='100.100.1.0/24')
        check_state_db_routes(dvs, vnet_name, "100.100.1.11/32", ['19.0.0.1'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24", "test_prf")

        #remove first route
        delete_vnet_routes(dvs, "100.100.1.11/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet12', ["100.100.1.11/32"])
        check_remove_state_db_routes(dvs, 'Vnet12', "100.100.1.11/32")

        #adv should be gone.
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")

        #add 2nd route
        create_vnet_routes(dvs, "100.100.1.57/32", vnet_name, '5.0.0.1,5.0.0.2,5.0.0.3,5.0.0.4', ep_monitor='5.1.0.1,5.1.0.2,5.1.0.3,5.1.0.4', profile = "test_prf", primary ='5.0.0.1,5.0.0.2',monitoring='custom', adv_prefix='100.100.1.0/24')
        update_monitor_session_state(dvs, '100.100.1.57/32', '5.1.0.1', 'up')
        update_monitor_session_state(dvs, '100.100.1.57/32', '5.1.0.2', 'up')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['5.0.0.1','5.0.0.2'], tunnel_name, prefix="100.100.1.57/32")
        check_state_db_routes(dvs, vnet_name, "100.100.1.57/32", ['5.0.0.1,5.0.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24", "test_prf")

        #modify  2nd route switch primary with secondary
        create_vnet_routes(dvs, "100.100.1.57/32", vnet_name, '5.0.0.1,5.0.0.2,5.0.0.3,5.0.0.4', ep_monitor='5.1.0.1,5.1.0.2,5.1.0.3,5.1.0.4', profile = "test_prf", primary ='5.0.0.3,5.0.0.4',monitoring='custom', adv_prefix='100.100.1.0/24')
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['5.0.0.1','5.0.0.2'], tunnel_name, route_ids=route1, prefix="100.100.1.57/32")
        check_state_db_routes(dvs, vnet_name, "100.100.1.57/32", ['5.0.0.1','5.0.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24", "test_prf")

        #delete 2nd route
        delete_vnet_routes(dvs, "100.100.1.57/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet12', ["100.100.1.57/32"])
        check_remove_state_db_routes(dvs, 'Vnet12', "100.100.1.57/32")
        #adv should be gone.
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")

        #add 3rd route
        create_vnet_routes(dvs, "100.100.1.67/32", vnet_name, '5.0.0.1,5.0.0.2,5.0.0.3,5.0.0.4', ep_monitor='5.1.0.1,5.1.0.2,5.1.0.3,5.1.0.4', profile = "test_prf", primary ='5.0.0.1,5.0.0.2',monitoring='custom', adv_prefix='100.100.1.0/24')
        update_monitor_session_state(dvs, '100.100.1.67/32', '5.1.0.1', 'up')
        update_monitor_session_state(dvs, '100.100.1.67/32', '5.1.0.2', 'up')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['5.0.0.1','5.0.0.2'], tunnel_name, prefix="100.100.1.67/32")
        check_state_db_routes(dvs, vnet_name, "100.100.1.67/32", ['5.0.0.1,5.0.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24", "test_prf")

        #modify  3rd route next hops to secondary
        create_vnet_routes(dvs, "100.100.1.67/32", vnet_name, '5.0.0.1,5.0.0.2,5.0.0.3,5.0.0.4', ep_monitor='5.1.0.1,5.1.0.2,5.1.0.3,5.1.0.4', profile = "test_prf", primary ='5.0.0.3,5.0.0.4',monitoring='custom', adv_prefix='100.100.1.0/24')
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['5.0.0.1','5.0.0.2'], tunnel_name, route_ids=route1, prefix="100.100.1.67/32")
        check_state_db_routes(dvs, vnet_name, "100.100.1.67/32", ['5.0.0.1','5.0.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24", "test_prf")

         #modify  3rd route next hops to a new set.
        create_vnet_routes(dvs, "100.100.1.67/32", vnet_name, '5.0.0.5,5.0.0.6,5.0.0.7,5.0.0.8', ep_monitor='5.1.0.5,5.1.0.6,5.1.0.7,5.1.0.8', profile = "test_prf", primary ='5.0.0.5,5.0.0.6',monitoring='custom', adv_prefix='100.100.1.0/24')
        update_monitor_session_state(dvs, '100.100.1.67/32', '5.1.0.5', 'up')
        update_monitor_session_state(dvs, '100.100.1.67/32', '5.1.0.6', 'up')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['5.0.0.5','5.0.0.6'], tunnel_name, route_ids=route1, prefix="100.100.1.67/32")
        check_state_db_routes(dvs, vnet_name, "100.100.1.67/32", ['5.0.0.5,5.0.0.6'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24", "test_prf")

        update_monitor_session_state(dvs, '100.100.1.67/32', '5.1.0.7', 'up')
        update_monitor_session_state(dvs, '100.100.1.67/32', '5.1.0.8', 'up')

        create_vnet_routes(dvs, "100.100.1.67/32", vnet_name, '5.0.0.5,5.0.0.6,5.0.0.7,5.0.0.8', ep_monitor='5.1.0.5,5.1.0.6,5.1.0.7,5.1.0.8', profile = "test_prf", primary ='5.0.0.7,5.0.0.8',monitoring='custom', adv_prefix='100.100.1.0/24')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['5.0.0.7','5.0.0.8'], tunnel_name, route_ids=route1, prefix="100.100.1.67/32")
        check_state_db_routes(dvs, vnet_name, "100.100.1.67/32", ['5.0.0.7,5.0.0.8'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24", "test_prf")

        #delete 3rd route
        delete_vnet_routes(dvs, "100.100.1.67/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet12', ["100.100.1.67/32"])
        check_remove_state_db_routes(dvs, 'Vnet12', "100.100.1.67/32")
        #adv should be gone.
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")

        #Add priority route with no secondary enpoints
        create_vnet_routes(dvs, "100.100.1.71/32", vnet_name, '19.0.0.1,19.0.0.2', ep_monitor='19.0.0.1,19.0.0.2', profile = "test_prf", primary ='19.0.0.1,19.0.0.2',monitoring='custom', adv_prefix='100.100.1.0/24')
        update_monitor_session_state(dvs, '100.100.1.71/32', '19.0.0.1', 'up')
        update_monitor_session_state(dvs, '100.100.1.71/32', '19.0.0.2', 'up')

        #verify that no BFD sessions are created.
        check_del_bfd_session(dvs, ['19.0.0.1'])
        check_del_bfd_session(dvs, ['19.0.0.2'])
        time.sleep(2)
        check_state_db_routes(dvs, vnet_name, "100.100.1.71/32", ['19.0.0.1,19.0.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24", "test_prf")

        update_monitor_session_state(dvs, '100.100.1.71/32', '19.0.0.1', 'down')
        check_state_db_routes(dvs, vnet_name, "100.100.1.71/32", ['19.0.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24", "test_prf")

        update_monitor_session_state(dvs, '100.100.1.71/32', '19.0.0.2', 'down')
        check_remove_state_db_routes(dvs, 'Vnet12', "100.100.1.71/32")

        #remove first route
        delete_vnet_routes(dvs, "100.100.1.71/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet12', ["100.100.1.71/32"])
        check_remove_state_db_routes(dvs, 'Vnet12', "100.100.1.71/32")

        delete_vnet_entry(dvs,vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 23 - Test for vxlan custom monitoring. CHanging the overlay_dmac of the Vnet on the fly.
    '''
    def test_vnet_orch_23(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_22'
        vnet_name = "Vnet22"
        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '9.9.9.3')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10022', "", advertise_prefix=True, overlay_dmac="22:33:33:44:44:66")
        delete_vnet_entry(dvs,vnet_name)

        create_vnet_entry(dvs, vnet_name, tunnel_name, '10022', "", advertise_prefix=True, overlay_dmac="22:33:33:44:44:66")
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10022', "", advertise_prefix=True, overlay_dmac="22:33:33:44:44:77")
        delete_vnet_entry(dvs,vnet_name)

        #update the Dmac of the vnet before adding any routes.
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10022', "", advertise_prefix=True, overlay_dmac="22:33:33:44:44:66")
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10022', "", advertise_prefix=True, overlay_dmac="22:33:33:44:44:77")

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '10022')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '9.9.9.3')

        vnet_obj.fetch_exist_entries(dvs)
        #Add first Route
        create_vnet_routes(dvs, "100.100.1.11/32", vnet_name, '19.0.0.1,19.0.0.2,19.0.0.3', ep_monitor='19.1.0.1,19.1.0.2,19.1.0.3', profile = "test_prf", primary ='19.0.0.1',monitoring='custom', adv_prefix='100.100.1.0/24')
        #verify the appdb entries.
        vnet_obj.check_custom_monitor_app_db(dvs, "100.100.1.11/32", "19.1.0.1", "vxlan", "22:33:33:44:44:77")
        vnet_obj.check_custom_monitor_app_db(dvs, "100.100.1.11/32", "19.1.0.2", "vxlan", "22:33:33:44:44:77")
        vnet_obj.check_custom_monitor_app_db(dvs, "100.100.1.11/32", "19.1.0.3", "vxlan", "22:33:33:44:44:77")

        #update the Dmac after a route is added.
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10022', "", advertise_prefix=True, overlay_dmac="22:33:33:44:44:88")

        #verify the appdb entries.
        vnet_obj.check_custom_monitor_app_db(dvs, "100.100.1.11/32", "19.1.0.1", "vxlan", "22:33:33:44:44:88")
        vnet_obj.check_custom_monitor_app_db(dvs, "100.100.1.11/32", "19.1.0.2", "vxlan", "22:33:33:44:44:88")
        vnet_obj.check_custom_monitor_app_db(dvs, "100.100.1.11/32", "19.1.0.3", "vxlan", "22:33:33:44:44:88")

        #bring up an enpoint.
        update_monitor_session_state(dvs, '100.100.1.11/32', '19.1.0.1', 'up')

        #verify the appdb entries.
        vnet_obj.check_custom_monitor_app_db(dvs, "100.100.1.11/32", "19.1.0.1", "vxlan", "22:33:33:44:44:88")
        vnet_obj.check_custom_monitor_app_db(dvs, "100.100.1.11/32", "19.1.0.2", "vxlan", "22:33:33:44:44:88")
        vnet_obj.check_custom_monitor_app_db(dvs, "100.100.1.11/32", "19.1.0.3", "vxlan", "22:33:33:44:44:88")

        #update the Dmac to empty. This should have no impact.
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10022', "", advertise_prefix=True, overlay_dmac="")

        #verify the appdb entries.
        vnet_obj.check_custom_monitor_app_db(dvs, "100.100.1.11/32", "19.1.0.1", "vxlan", "22:33:33:44:44:88")
        vnet_obj.check_custom_monitor_app_db(dvs, "100.100.1.11/32", "19.1.0.2", "vxlan", "22:33:33:44:44:88")
        vnet_obj.check_custom_monitor_app_db(dvs, "100.100.1.11/32", "19.1.0.3", "vxlan", "22:33:33:44:44:88")

        #remove first route
        delete_vnet_routes(dvs, "100.100.1.11/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet12', ["100.100.1.11/32"])
        check_remove_state_db_routes(dvs, 'Vnet12', "100.100.1.11/32")

        #make sure that the app db entries are removed.
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.11/32", "19.1.0.1")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.11/32", "19.1.0.2")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.11/32", "19.1.0.3")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.11/32", "19.1.0.4")
        time.sleep(2)

        #bring down an enpoint.
        update_monitor_session_state(dvs, '100.100.1.11/32', '19.1.0.1', 'down')

        create_vnet_entry(dvs, vnet_name, tunnel_name, '10022', "", advertise_prefix=True, overlay_dmac="22:33:33:44:44:66")

        #Add first Route again
        create_vnet_routes(dvs, "100.100.1.11/32", vnet_name, '19.0.0.1,19.0.0.2,19.0.0.3', ep_monitor='19.1.0.1,19.1.0.2,19.1.0.3', profile = "test_prf", primary ='19.0.0.1',monitoring='custom', adv_prefix='100.100.1.0/24')

        #bring up the endpoint.
        update_monitor_session_state(dvs, '100.100.1.11/32', '19.1.0.1', 'up')

        # The default Vnet setting advertises the prefix.
        check_routes_advertisement(dvs, "100.100.1.0/24", "test_prf")

        #verify the appdb entries.
        vnet_obj.check_custom_monitor_app_db(dvs, "100.100.1.11/32", "19.1.0.1", "vxlan", "22:33:33:44:44:66")
        vnet_obj.check_custom_monitor_app_db(dvs, "100.100.1.11/32", "19.1.0.2", "vxlan", "22:33:33:44:44:66")
        vnet_obj.check_custom_monitor_app_db(dvs, "100.100.1.11/32", "19.1.0.3", "vxlan", "22:33:33:44:44:66")

        #remove first route
        delete_vnet_routes(dvs, "100.100.1.11/32", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet12', ["100.100.1.11/32"])
        check_remove_state_db_routes(dvs, 'Vnet12', "100.100.1.11/32")

        #make sure that the app db entries are removed.
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.11/32", "19.1.0.1")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.11/32", "19.1.0.2")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.11/32", "19.1.0.3")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.11/32", "19.1.0.4")
        time.sleep(2)
        delete_vnet_entry(dvs,vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 24 - Test duplicate route addition and removal.
    '''
    def test_vnet_orch_24(self, dvs, testlog):
        self.setup_db(dvs)
        self.clear_srv_config(dvs)

        vnet_obj = self.get_vnet_obj()
        vnet_obj.fetch_exist_entries(dvs)

        # create vxlan tunnel and vnet in default vrf
        tunnel_name = 'tunnel_24'
        create_vxlan_tunnel(dvs, tunnel_name, '10.10.10.10')
        create_vnet_entry(dvs, 'Vnet_2000', tunnel_name, '2000', "", 'default')

        vnet_obj.check_default_vnet_entry(dvs, 'Vnet_2000')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet_2000', '2000')
        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '10.10.10.10')
        vnet_obj.fetch_exist_entries(dvs)

        # create vnet route
        create_vnet_routes(dvs, "100.100.1.0/24", 'Vnet_2000', '10.10.10.3')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_2000', '10.10.10.3', tunnel_name)
        check_state_db_routes(dvs, 'Vnet_2000', "100.100.1.0/24", ['10.10.10.3'])
        time.sleep(2)

        # create l3 interface
        self.create_l3_intf("Ethernet0", "")

        # set ip address
        self.add_ip_address("Ethernet0", "10.10.10.1/24")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 10.10.10.3/24 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.10.10.1")

        marker = dvs.add_log_marker("/var/log/syslog")
        time.sleep(2)

        # add another route for same prefix as vnet route
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 100.100.1.0/24 10.10.10.3\"")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "100.100.1.0/24")

        # check ASIC route database
        self.check_route_entries(["100.100.1.0/24"])

        log_string = "Encountered failure in create operation, exiting orchagent, SAI API: SAI_API_ROUTE, status: SAI_STATUS_NOT_EXECUTED"
        # check for absence of log_string in syslog
        check_syslog(dvs, marker, log_string)

        # remove route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 100.100.1.0/24 10.10.10.3\"")

        # delete vnet route
        delete_vnet_routes(dvs, "100.100.1.0/24", 'Vnet_2000')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet_2000')
        check_remove_state_db_routes(dvs, 'Vnet_2000', "100.100.1.0/24")

        # delete vnet
        delete_vnet_entry(dvs, 'Vnet_2000')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet_2000')

        # delete vxlan tunnel
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 25 - Test for BFD TSA and TSB behaviour within overlay tunnel routes.
    '''
    def test_vnet_orch_25(self, dvs, testlog):
        # This test creates a vnet route with BFD monitoring.This followd by application of TSA and absence of BFD sessions
        # is verified. Following the removal of TSA the Vnet route is verified to be up.
        vnet_obj = self.get_vnet_obj()
        tunnel_name = 'tunnel_25'

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')
        create_vnet_entry(dvs, 'Vnet25', tunnel_name, '10025', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet25')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet25', '10025')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "125.100.1.1/32", 'Vnet25', '9.0.0.1,9.0.0.2,9.0.0.3', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3')

        # default bfd status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet25', ["125.100.1.1/32"])
        check_state_db_routes(dvs, 'Vnet25', "125.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "125.100.1.1/32")

        # Route should be properly configured when all bfd session states go up
        update_bfd_session_state(dvs, '9.1.0.1', 'Up')
        update_bfd_session_state(dvs, '9.1.0.2', 'Up')
        update_bfd_session_state(dvs, '9.1.0.3', 'Up')
        time.sleep(2)

        # make sure the route is up.
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet25', ['9.0.0.1', '9.0.0.2', '9.0.0.3'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet25', "125.100.1.1/32", ['9.0.0.1', '9.0.0.2', '9.0.0.3'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "125.100.1.1/32")

        # tsa would remove all bfd sessions down.
        set_tsa(dvs)
        time.sleep(2)

        # Route should be removed.
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet25', ["125.100.1.1/32"])
        check_state_db_routes(dvs, 'Vnet25', "125.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "125.100.1.1/32")

        #clearing TSA should bring the route back.
        clear_tsa(dvs)
        time.sleep(2)

        update_bfd_session_state(dvs, '9.1.0.1', 'Up')
        update_bfd_session_state(dvs, '9.1.0.2', 'Up')
        update_bfd_session_state(dvs, '9.1.0.3', 'Up')
        time.sleep(2)

        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet25', ['9.0.0.1', '9.0.0.2', '9.0.0.3'], tunnel_name, route_ids=route1, nhg=nhg1_1)
        check_state_db_routes(dvs, 'Vnet25', "125.100.1.1/32", ['9.0.0.1', '9.0.0.2', '9.0.0.3'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "125.100.1.1/32")

        # Remove tunnel route
        delete_vnet_routes(dvs, "125.100.1.1/32", 'Vnet25')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet25', ["125.100.1.1/32"])
        check_remove_state_db_routes(dvs, 'Vnet25', "125.100.1.1/32")
        check_remove_routes_advertisement(dvs, "125.100.1.1/32")

        # Check the corresponding nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_1 not in vnet_obj.nhgs
        # Check the BFD session specific to the endpoint group is removed while others exist
        check_del_bfd_session(dvs, ['9.1.0.1', '9.1.0.2', '9.1.0.3'])

        delete_vnet_entry(dvs, 'Vnet25')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet25')
        delete_vxlan_tunnel(dvs, tunnel_name)

    '''
    Test 26 - Test for vnet tunnel routes with ECMP nexthop group with subnet decap enable
    '''
    def test_vnet_orch_26(self, dvs, setup_subnet_decap):
        # apply subnet decap config
        subnet_decap_config = {
            "status": "enable",
            "src_ip": "10.10.10.0/24",
            "src_ip_v6": "20c1:ba8::/64"
        }
        setup_subnet_decap(subnet_decap_config)

        vnet_obj = self.get_vnet_obj()
        vnet_obj.fetch_exist_entries(dvs)

        # Add the subnet decap tunnel
        create_subnet_decap_tunnel(dvs, "IPINIP_SUBNET", tunnel_type="IPINIP",
                                   dscp_mode="uniform", ecn_mode="standard", ttl_mode="pipe")
        vnet_obj.check_ipinip_tunnel(dvs, "IPINIP_SUBNET", "uniform", "standard", "pipe")

        vnet_obj.fetch_exist_entries(dvs)
        tunnel_name = 'tunnel_26'
        create_vxlan_tunnel(dvs, tunnel_name, '26.26.26.26')
        create_vnet_entry(dvs, 'Vnet26', tunnel_name, '10026', "", advertise_prefix=True)

        vnet_obj.check_vnet_entry(dvs, 'Vnet26')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet26', '10026')
        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '26.26.26.26')

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", 'Vnet26', '26.0.0.1,26.0.0.2,26.0.0.3', ep_monitor='26.1.0.1,26.1.0.2,26.1.0.3', profile="test_profile")

        with pytest.raises(AssertionError):
            vnet_obj.check_ipinip_tunnel_decap_term(dvs, "IPINIP_SUBNET", "100.100.1.1/32", "10.10.10.0/24")

        # default bfd status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet26', ["100.100.1.1/32"])
        check_state_db_routes(dvs, 'Vnet26', "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Route should be properly configured when all bfd session states go up
        update_bfd_session_state(dvs, '26.1.0.1', 'Up')

        time.sleep(2)
        # subnet decap term should be created as one bfd session state go up
        vnet_obj.check_ipinip_tunnel_decap_term(dvs, "IPINIP_SUBNET", "100.100.1.1/32", "10.10.10.0/24")

        update_bfd_session_state(dvs, '26.1.0.2', 'Up')
        update_bfd_session_state(dvs, '26.1.0.3', 'Up')
        time.sleep(2)
        vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet26', ['26.0.0.1', '26.0.0.2', '26.0.0.3'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet26', "100.100.1.1/32", ['26.0.0.1', '26.0.0.2', '26.0.0.3'])
        check_routes_advertisement(dvs, "100.100.1.1/32", "test_profile")

        # Set all endpoint to down state
        update_bfd_session_state(dvs, '26.1.0.1', 'Down')
        update_bfd_session_state(dvs, '26.1.0.2', 'Down')
        update_bfd_session_state(dvs, '26.1.0.3', 'Down')
        time.sleep(2)

        # subnet decap term should be removed as all bfd session states go down
        vnet_obj.check_del_ipinip_tunnel_decap_term(dvs, "IPINIP_SUBNET", "100.100.1.1/32", "10.10.10.0/24")

        # Confirm the tunnel route is updated in ASIC
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet26', ["100.100.1.1/32"])
        check_state_db_routes(dvs, 'Vnet26', "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Remove tunnel route
        delete_vnet_routes(dvs, "100.100.1.1/32", 'Vnet26')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet26', ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, 'Vnet26', "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Confirm the BFD sessions are removed
        check_del_bfd_session(dvs, ['12.1.0.1', '12.1.0.2', '12.1.0.3'])

        delete_vnet_entry(dvs, 'Vnet26')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet26')
        delete_vxlan_tunnel(dvs, tunnel_name)

        # Remove the subnet decap tunnel
        vnet_obj.fetch_exist_entries(dvs)
        delete_subnet_decap_tunnel(dvs, "IPINIP_SUBNET")
        vnet_obj.check_del_ipinip_tunnel(dvs, "IPINIP_SUBNET")

    '''
    Test 27 - Test for IPv6 vnet tunnel routes with ECMP nexthop group with subnet decap enable
    '''
    def test_vnet_orch_27(self, dvs, setup_subnet_decap):
        subnet_decap_config = {
            "status": "enable",
            "src_ip": "10.10.10.0/24",
            "src_ip_v6": "20c1:ba8::/64"
        }
        setup_subnet_decap(subnet_decap_config)

        vnet_obj = self.get_vnet_obj()
        vnet_obj.fetch_exist_entries(dvs)

        # Add the subnet decap tunnel
        create_subnet_decap_tunnel(dvs, "IPINIP_SUBNET_V6", tunnel_type="IPINIP",
                                   dscp_mode="uniform", ecn_mode="standard", ttl_mode="pipe")
        vnet_obj.check_ipinip_tunnel(dvs, "IPINIP_SUBNET_V6", "uniform", "standard", "pipe")

        vnet_obj.fetch_exist_entries(dvs)
        tunnel_name = 'tunnel_27'
        vnet_name = 'Vnet26'
        create_vxlan_tunnel(dvs, tunnel_name, 'fd:10::32')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10010', "", advertise_prefix=True)

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '10010')
        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, 'fd:10::32')

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "fd:10:10::1/128", vnet_name, 'fd:10:1::1,fd:10:1::2,fd:10:1::3', ep_monitor='fd:10:2::1,fd:10:2::2,fd:10:2::3', profile="test_profile")

        with pytest.raises(AssertionError):
            vnet_obj.check_ipinip_tunnel_decap_term(dvs, "IPINIP_SUBNET_V6", "100.100.1.1/32", "10.10.10.0/24")

        # default bfd status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["fd:10:10::1/128"])
        check_state_db_routes(dvs, vnet_name, "fd:10:10::1/128", [])
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")

        # Route should be properly configured when all bfd session states go up
        update_bfd_session_state(dvs, 'fd:10:2::2', 'Up')

        time.sleep(2)
        # subnet decap term should be created as one bfd session state go up
        vnet_obj.check_ipinip_tunnel_decap_term(dvs, "IPINIP_SUBNET_V6", "fd:10:10::1/128", "20c1:ba8::/64")

        update_bfd_session_state(dvs, 'fd:10:2::3', 'Up')
        update_bfd_session_state(dvs, 'fd:10:2::1', 'Up')
        time.sleep(2)
        vnet_obj.check_vnet_ecmp_routes(dvs, vnet_name, ['fd:10:1::1', 'fd:10:1::2', 'fd:10:1::3'], tunnel_name)
        check_state_db_routes(dvs, vnet_name, "fd:10:10::1/128", ['fd:10:1::1', 'fd:10:1::2', 'fd:10:1::3'])
        check_routes_advertisement(dvs, "fd:10:10::1/128", "test_profile")

        # Set all endpoint to down state
        update_bfd_session_state(dvs, 'fd:10:2::1', 'Down')
        update_bfd_session_state(dvs, 'fd:10:2::2', 'Down')
        update_bfd_session_state(dvs, 'fd:10:2::3', 'Down')
        time.sleep(2)

        # subnet decap term should be removed as all bfd session states go down
        vnet_obj.check_del_ipinip_tunnel_decap_term(dvs, "IPINIP_SUBNET_V6", "fd:10:10::1/128", "20c1:ba8::/64")

        # Confirm the tunnel route is updated in ASIC
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["fd:10:10::1/128"])
        check_state_db_routes(dvs, vnet_name, "fd:10:10::1/128", [])
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")

        # Remove tunnel route
        delete_vnet_routes(dvs, "fd:10:10::1/128", vnet_name)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["fd:10:10::1/128"])
        check_remove_state_db_routes(dvs, vnet_name, "fd:10:10::1/128")
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")

        # Confirm the BFD sessions are removed
        check_del_bfd_session(dvs, ['fd:10:2::1', 'fd:10:2::2', 'fd:10:2::3'])

        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)

        # Remove the subnet decap tunnel
        vnet_obj.fetch_exist_entries(dvs)
        delete_subnet_decap_tunnel(dvs, "IPINIP_SUBNET_V6")
        vnet_obj.check_del_ipinip_tunnel(dvs, "IPINIP_SUBNET_V6")

    '''
    Test 28 - Test for Single endpoint priority vnet tunnel routes. Test with local endpoint.
    '''
    def test_vnet_orch_28(self, dvs, dvs_acl, testlog):
        self.setup_db(dvs)
        self.clear_srv_config(dvs)

        vnet_obj = self.get_vnet_obj()
        tunnel_name = 'tunnel_28'
        vnet_name = 'Vnet28'
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10028', "", advertise_prefix=True, overlay_dmac="22:33:33:44:44:66")

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '10028')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')

        # no acl rule before creating vnet route
        dvs_acl.verify_no_acl_rules()

        # create l3 interface
        self.create_l3_intf("Ethernet8", "")

        # set ip address
        self.add_ip_address("Ethernet8", "9.1.0.1/32")

        # bring up interface
        self.set_admin_status("Ethernet8", "up")

        # add neighbor for direcetly connected endpoint
        self.add_neighbor("Ethernet8", "9.1.0.1", "00:01:02:03:04:05")

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", vnet_name, '9.1.0.1,9.1.0.2', ep_monitor='9.1.0.3,9.1.0.4', primary ='9.1.0.1', profile="Test_profile", monitoring='custom', adv_prefix='100.100.1.0/24', check_directly_connected=True)

        # verify acl table action list
        expected_action_list = [
            "SAI_ACL_ACTION_TYPE_COUNTER",
            "SAI_ACL_ACTION_TYPE_REDIRECT"
        ]
        acl_table_id = dvs_acl.get_acl_table_ids(1)[0]
        dvs_acl.verify_acl_table_action_list(acl_table_id, expected_action_list)

        # verify tunnel term acl 
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_DST_IP": dvs_acl.get_simple_qualifier_comparator("100.100.1.1&mask:255.255.255.255")
        }
        nh_id = dvs.get_asic_db().wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP", 1)[0]
        dvs_acl.verify_redirect_acl_rule(expected_sai_qualifiers, nh_id, priority="9998")

        # default monitor session status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")

        # Route should be properly configured when all monitor session states go up. Only primary Endpoints should be in use.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.3', 'up')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.4', 'up')
        time.sleep(2)
        nhids = get_all_created_entries(asic_db, vnet_obj.ASIC_NEXT_HOP,set())
        tbl_nh =  swsscommon.Table(asic_db, vnet_obj.ASIC_NEXT_HOP)
        nexthops = dict()
        for nhid in nhids:
            status, nh_fvs = tbl_nh.get(nhid)
            nh_fvs = dict(nh_fvs)
            for key in nh_fvs.keys():
                if key == 'SAI_NEXT_HOP_ATTR_IP':
                    nexthops[nh_fvs[key]] = nhid
        assert len(nexthops.keys()) == 1

        route = get_created_entries(asic_db, vnet_obj.ASIC_ROUTE_ENTRY, vnet_obj.routes, 1)
        check_object(asic_db, vnet_obj.ASIC_ROUTE_ENTRY, route[0],
                        {
                            "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID": nexthops['9.1.0.1'],
                        }
                    )
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1'])
        check_routes_advertisement(dvs, "100.100.1.0/24", "Test_profile")

        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.4', 'down')
        time.sleep(2)

        route = get_created_entries(asic_db, vnet_obj.ASIC_ROUTE_ENTRY, vnet_obj.routes, 1)
        check_object(asic_db, vnet_obj.ASIC_ROUTE_ENTRY, route[0],
                        {
                            "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID": nexthops['9.1.0.1'],
                        }
                    )
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1'])
        check_routes_advertisement(dvs, "100.100.1.0/24", "Test_profile")

        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.3', 'down')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.4', 'up')

        time.sleep(2)

        nhids = get_all_created_entries(asic_db, vnet_obj.ASIC_NEXT_HOP,set())
        tbl_nh =  swsscommon.Table(asic_db, vnet_obj.ASIC_NEXT_HOP)
        nexthops = dict()
        for nhid in nhids:
            status, nh_fvs = tbl_nh.get(nhid)
            nh_fvs = dict(nh_fvs)
            for key in nh_fvs.keys():
                if key == 'SAI_NEXT_HOP_ATTR_IP':
                    nexthops[nh_fvs[key]] = nhid
        # nexthop from diectly connected endpoint should NOT be removed
        assert len(nexthops.keys()) == 2

        route = get_created_entries(asic_db, vnet_obj.ASIC_ROUTE_ENTRY, vnet_obj.routes, 1)
        check_object(asic_db, vnet_obj.ASIC_ROUTE_ENTRY, route[0],
                        {
                            "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID": nexthops['9.1.0.2'],
                        }
                    )
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.2'])
        check_routes_advertisement(dvs, "100.100.1.0/24", "Test_profile")

        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.3', 'up')
        time.sleep(2)

        nhids = get_all_created_entries(asic_db, vnet_obj.ASIC_NEXT_HOP,set())
        tbl_nh =  swsscommon.Table(asic_db, vnet_obj.ASIC_NEXT_HOP)
        nexthops = dict()
        for nhid in nhids:
            status, nh_fvs = tbl_nh.get(nhid)
            nh_fvs = dict(nh_fvs)
            for key in nh_fvs.keys():
                if key == 'SAI_NEXT_HOP_ATTR_IP':
                    nexthops[nh_fvs[key]] = nhid
        assert len(nexthops.keys()) == 1

        route = get_created_entries(asic_db, vnet_obj.ASIC_ROUTE_ENTRY, vnet_obj.routes, 1)
        check_object(asic_db, vnet_obj.ASIC_ROUTE_ENTRY, route[0],
                        {
                            "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID": nexthops['9.1.0.1'],
                        }
                    )
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1'])
        check_routes_advertisement(dvs, "100.100.1.0/24", "Test_profile")

        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.3', 'down')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.4', 'down')

        time.sleep(2)

        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "200.100.1.0/24")

        # Remove tunnel route 1
        delete_vnet_routes(dvs, "100.100.1.1/32", vnet_name)

        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")

        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.1/32", "9.1.0.3")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.1/32", "9.1.0.4")

        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)

        self.remove_neighbor("Ethernet8", "9.1.0.1")
        self.remove_ip_address("Ethernet8", "9.1.0.1/32")
        self.set_admin_status("Ethernet8", "down")

    '''
    Test 29 - Test for priority vnet tunnel routes with ECMP nexthop group and local nhg. test primary secondary switchover.
    '''
    def test_vnet_orch_29(self, dvs, testlog):
        self.setup_db(dvs)
        self.clear_srv_config(dvs)

        vnet_obj = self.get_vnet_obj()
        tunnel_name = 'tunnel_29'
        vnet_name = 'vnet29'
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10029', "", advertise_prefix=True, overlay_dmac="22:33:33:44:44:66")

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '10029')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')

        # create l3 interface
        self.create_l3_intf("Ethernet8", "")
        self.create_l3_intf("Ethernet12", "")

        # set ip address
        self.add_ip_address("Ethernet8", "9.1.0.3/32")
        self.add_ip_address("Ethernet12", "9.1.0.4/32")

        # bring up interface
        self.set_admin_status("Ethernet8", "up")
        self.set_admin_status("Ethernet12", "up")

        # add neighbor for direcetly connected endpoint
        self.add_neighbor("Ethernet8", "9.1.0.3", "00:01:02:03:04:05")
        self.add_neighbor("Ethernet12", "9.1.0.4", "00:01:02:03:04:06")

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", vnet_name, '9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4', primary ='9.1.0.1,9.1.0.2', monitoring='custom', adv_prefix='100.100.1.0/24', check_directly_connected=True)

        # default monitor status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")

        # Route should be properly configured when all monitor session states go up. Only primary Endpoints should be in use.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.1', 'up')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.2', 'up')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.3', 'up')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.4', 'up')

        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1','9.1.0.2'], tunnel_name)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1','9.1.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        # Remove first primary endpoint from group.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.2', 'down')
        time.sleep(2)
        route1= vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        # Switch to secondary if both primary down
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.1', 'down')
        time.sleep(2)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.3','9.1.0.4'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        # removing first endpoint of secondary. route should remain on secondary NHG
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.3', 'down')
        time.sleep(2)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.4'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        # removing last endpoint of secondary. route should be removed
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.4', 'down')
        time.sleep(2)

        new_nhgs = get_all_created_entries(asic_db, vnet_obj.ASIC_NEXT_HOP_GROUP, [])
        assert len(new_nhgs) == 0
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.1.1/32")

        #Route should come up with secondary endpoints.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.3', 'up')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.4', 'up')

        time.sleep(2)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.3','9.1.0.4'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        #Route should be switched to the primary endpoint.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.1', 'up')
        time.sleep(2)
        route1= vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        #Route should be updated with the second primary endpoint.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.2', 'up')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1','9.1.0.2'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1','9.1.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        #Route should not be impacted by seconday endpoints going down.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.3', 'down')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.4', 'down')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1','9.1.0.2'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1','9.1.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        #Route should not be impacted by seconday endpoints coming back up.
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.3', 'up')
        update_monitor_session_state(dvs, '100.100.1.1/32', '9.1.0.4', 'up')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1','9.1.0.2'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1','9.1.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        # Remove tunnel route 1
        delete_vnet_routes(dvs, "100.100.1.1/32", vnet_name)
        time.sleep(2)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")

        # Confirm the monitor sessions are removed
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.1/32", "9.1.0.1")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.1/32", "9.1.0.2")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.1/32", "9.1.0.3")
        vnet_obj.check_custom_monitor_deleted(dvs, "100.100.1.1/32", "9.1.0.4")

        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)

        self.remove_neighbor("Ethernet8", "9.1.0.3")
        self.remove_ip_address("Ethernet8", "9.1.0.3/32")
        self.set_admin_status("Ethernet8", "down")

        self.remove_neighbor("Ethernet12", "9.1.0.4")
        self.remove_ip_address("Ethernet12", "9.1.0.4/32")
        self.set_admin_status("Ethernet12", "down")

    '''
    Test 30 - Test vnet local route with single nexthop
    '''
    def test_vnet_local_route_single(self, dvs, testlog):
        self.setup_db(dvs)

        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_30'

        vnet_obj.fetch_exist_entries(dvs)
        vnet_name = "Vnet5000"

        # setup vnet and vlan
        create_vxlan_tunnel(dvs, tunnel_name, '30.30.30.30')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '5000', "")

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '5000')
        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '30.30.30.30')

        # setup interface under vnet
        create_phy_interface(dvs, "Ethernet20", vnet_name, "10.10.0.8/31")
        vnet_obj.check_router_interface(dvs, "Ethernet20", vnet_name)

        # bring up interface
        self.set_admin_status("Ethernet20", "up")

        # setup vnet neighbor
        self.add_neighbor("Ethernet20", "10.10.0.9", "00:01:02:03:04:05")

        vnet_obj.fetch_exist_entries(dvs)

        num_nh = len(vnet_obj.nhops)
        num_nhg = len(vnet_obj.nhgs)
        num_nhgm = len(vnet_obj.nhgms)

        # create vnet local route
        create_vnet_local_routes(dvs, "10.10.0.0/24", vnet_name, 'Ethernet20', "10.10.0.9")
        route_keys = vnet_obj.check_vnet_local_routes(dvs, vnet_name)
        vnet_obj.check_vnet_local_route_nexthops(dvs, route_keys[0], ["10.10.0.9"])

        # Clean-up and verify remove flows
        delete_vnet_local_routes(dvs, "10.10.0.0/24", vnet_name)
        vnet_obj.check_del_vnet_local_routes(dvs, vnet_name, "10.10.0.0/24")

        vnet_obj.fetch_exist_entries(dvs)
        assert len(vnet_obj.nhops) == num_nh
        assert len(vnet_obj.nhgs) == num_nhg
        assert len(vnet_obj.nhgms) == num_nhgm

        self.remove_neighbor("Ethernet20", "10.10.0.9")
        self.set_admin_status("Ethernet20", "down")

        delete_phy_interface(dvs, "Ethernet20", "10.10.0.8/31")
        vnet_obj.check_del_router_interface(dvs, "Ethernet20")

        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)

        delete_vxlan_tunnel(dvs, tunnel_name)
        vnet_obj.check_del_vxlan_tunnel(dvs)

    '''
    Test 31 - Test ecmp vnet local route with multiple nexthops
    '''
    def test_vnet_local_route_ecmp(self, dvs, testlog):
        self.setup_db(dvs)

        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_31'
        vnet_name = "Vnet5001"

        vnet_obj.fetch_exist_entries(dvs)

        # setup vnet and vlan
        create_vxlan_tunnel(dvs, tunnel_name, '31.31.31.31')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '5001', "")

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '5001')
        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '31.31.31.31')

        # setup interfaces under vnet
        create_phy_interface(dvs, "Ethernet20", vnet_name, "10.10.0.8/31")
        vnet_obj.check_router_interface(dvs, "Ethernet20", vnet_name)

        create_phy_interface(dvs, "Ethernet16", vnet_name, "10.10.0.10/31")
        vnet_obj.check_router_interface(dvs, "Ethernet16", vnet_name)

        # bring up interface
        self.set_admin_status("Ethernet20", "up")
        self.set_admin_status("Ethernet16", "up")

        # setup vnet neighbor
        self.add_neighbor("Ethernet20", "10.10.0.9", "00:01:02:03:04:05")
        self.add_neighbor("Ethernet16", "10.10.0.11", "00:01:02:03:04:06")

        vnet_obj.fetch_exist_entries(dvs)

        num_nh = len(vnet_obj.nhops)
        num_nhg = len(vnet_obj.nhgs)
        num_nhgm = len(vnet_obj.nhgms)

        # create vnet local route
        create_vnet_local_routes(dvs, "10.10.0.0/24", vnet_name, 'Ethernet20,Ethernet16', "10.10.0.9,10.10.0.11")
        route_keys = vnet_obj.check_vnet_local_routes(dvs, vnet_name)
        vnet_obj.check_vnet_local_route_nexthops(dvs, route_keys[0], ["10.10.0.9", "10.10.0.11"])

        # update vnet local route to single nexthop
        create_vnet_local_routes(dvs, "10.10.0.0/24", vnet_name, 'Ethernet20', "10.10.0.9")
        vnet_obj.check_vnet_local_route_nexthops(dvs, route_keys[0], ["10.10.0.9"])

        # update vnet local route back to ecmp
        create_vnet_local_routes(dvs, "10.10.0.0/24", vnet_name, 'Ethernet20,Ethernet16', "10.10.0.9,10.10.0.11")
        vnet_obj.check_vnet_local_route_nexthops(dvs, route_keys[0], ["10.10.0.9", "10.10.0.11"])

        # Clean-up and verify remove flows
        delete_vnet_local_routes(dvs, "10.10.0.0/24", vnet_name)
        vnet_obj.check_del_vnet_local_routes(dvs, vnet_name, "10.10.0.0/24")

        vnet_obj.fetch_exist_entries(dvs)
        assert len(vnet_obj.nhops) == num_nh
        assert len(vnet_obj.nhgs) == num_nhg
        assert len(vnet_obj.nhgms) == num_nhgm

        self.remove_neighbor("Ethernet20", "10.10.0.9")
        self.remove_neighbor("Ethernet16", "10.10.0.11")
        self.set_admin_status("Ethernet20", "down")
        self.set_admin_status("Ethernet16", "down")

        delete_phy_interface(dvs, "Ethernet20", "10.10.0.8/31")
        vnet_obj.check_del_router_interface(dvs, "Ethernet20")
        delete_phy_interface(dvs, "Ethernet16", "10.10.0.10/31")
        vnet_obj.check_del_router_interface(dvs, "Ethernet16")

        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)

        delete_vxlan_tunnel(dvs, tunnel_name)
        vnet_obj.check_del_vxlan_tunnel(dvs)


    '''
    Test 32 - Test for priority vnet tunnel routes with ECMP nexthop group and local nhg. test primary secondary switchover + bfd monitoring + rx and tx timer.
    '''
    def test_vnet_orch_32(self, dvs, testlog):
        self.setup_db(dvs)

        vnet_obj = self.get_vnet_obj()
        tunnel_name = 'tunnel_32'
        vnet_name = 'vnet32'
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')
        create_vnet_entry(dvs, vnet_name, tunnel_name, '10029', "", advertise_prefix=True, overlay_dmac="22:33:33:44:44:66")

        vnet_obj.check_vnet_entry(dvs, vnet_name)
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, vnet_name, '10029')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')

        # create l3 interface
        self.create_l3_intf("Ethernet8", "")
        self.create_l3_intf("Ethernet12", "")

        # set ip address
        self.add_ip_address("Ethernet8", "9.1.0.3/32")
        self.add_ip_address("Ethernet12", "9.1.0.4/32")

        # bring up interface
        self.set_admin_status("Ethernet8", "up")
        self.set_admin_status("Ethernet12", "up")

        # add neighbor for directly connected endpoint
        self.add_neighbor("Ethernet8", "9.1.0.3", "00:01:02:03:04:05")
        self.add_neighbor("Ethernet12", "9.1.0.4", "00:01:02:03:04:06")

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", vnet_name, '9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4', primary ='9.1.0.1,9.1.0.2', monitoring='custom_bfd', adv_prefix='100.100.1.0/24', check_directly_connected=True, rx_monitor_timer=100, tx_monitor_timer=100)

        # default monitor status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")

        # Route should be properly configured when all monitor session states go up. Only primary Endpoints should be in use.
        update_bfd_session_state(dvs, '9.1.0.1', 'Up')
        update_bfd_session_state(dvs, '9.1.0.2', 'Up')
        update_bfd_session_state(dvs, '9.1.0.3', 'Up')
        update_bfd_session_state(dvs, '9.1.0.4', 'Up')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1','9.1.0.2'], tunnel_name)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1','9.1.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        # Remove second primary endpoint from group.
        update_bfd_session_state(dvs, '9.1.0.2', 'Down')

        time.sleep(2)
        route1= vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        # Switch to secondary if both primary down
        update_bfd_session_state(dvs, '9.1.0.1', 'Down')
        time.sleep(2)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.3','9.1.0.4'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        # removing first endpoint of secondary. route should remain on secondary NHG
        update_bfd_session_state(dvs, '9.1.0.3', 'Down')
        time.sleep(2)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.4'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        # removing last endpoint of secondary. route should be removed
        update_bfd_session_state(dvs, '9.1.0.4', 'Down')
        time.sleep(2)

        new_nhgs = get_all_created_entries(asic_db, vnet_obj.ASIC_NEXT_HOP_GROUP, [])
        assert len(new_nhgs) == 0
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.1.1/32")

        # Route should come up with secondary endpoints.
        update_bfd_session_state(dvs, '9.1.0.3', 'Up')
        update_bfd_session_state(dvs, '9.1.0.4', 'Up')

        time.sleep(2)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.3','9.1.0.4'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        #Route should be switched to the primary endpoint.
        update_bfd_session_state(dvs, '9.1.0.1', 'Up')
        time.sleep(2)
        route1= vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        #Route should be updated with the second primary endpoint.
        update_bfd_session_state(dvs, '9.1.0.2', 'Up')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1','9.1.0.2'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1','9.1.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        #Route should not be impacted by seconday endpoints going down.
        update_bfd_session_state(dvs, '9.1.0.3', 'Down')
        update_bfd_session_state(dvs, '9.1.0.4', 'Down')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1','9.1.0.2'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1','9.1.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        #Route should not be impacted by seconday endpoints coming back up.
        update_bfd_session_state(dvs, '9.1.0.3', 'Up')
        update_bfd_session_state(dvs, '9.1.0.4', 'Up')
        time.sleep(2)
        route1 = vnet_obj.check_priority_vnet_ecmp_routes(dvs, vnet_name, ['9.1.0.1','9.1.0.2'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, vnet_name, "100.100.1.1/32", ['9.1.0.1','9.1.0.2'])
        # The default Vnet setting does not advertise prefix
        check_routes_advertisement(dvs, "100.100.1.0/24")

        # Remove tunnel route 1
        delete_vnet_routes(dvs, "100.100.1.1/32", vnet_name)
        time.sleep(2)
        vnet_obj.check_del_vnet_routes(dvs, vnet_name, ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, vnet_name, "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.0/24")

        # Confirm the monitor sessions are removed
        check_del_bfd_session(dvs, ['9.1.0.1', '9.1.0.2', '9.1.0.3', '9.1.0.4'])

        delete_vnet_entry(dvs, vnet_name)
        vnet_obj.check_del_vnet_entry(dvs, vnet_name)
        delete_vxlan_tunnel(dvs, tunnel_name)

        self.remove_neighbor("Ethernet8", "9.1.0.3")
        self.remove_ip_address("Ethernet8", "9.1.0.3/32")
        self.set_admin_status("Ethernet8", "down")

        self.remove_neighbor("Ethernet12", "9.1.0.4")
        self.remove_ip_address("Ethernet12", "9.1.0.4/32")
        self.set_admin_status("Ethernet12", "down")

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
