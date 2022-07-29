from evpn_tunnel import VxlanTunnel

DVS_ENV = ["HWSKU=Mellanox-SN2700"]

class TestVxlanOrchP2MP(object):

    def get_vxlan_obj(self):
        return VxlanTunnel()

#    Test 1 - Create and Delete SIP Tunnel and Map entries
    def test_p2mp_tunnel(self, dvs, testlog):
        vxlan_obj = self.get_vxlan_obj()

        tunnel_name = 'tunnel_1'
        map_name = 'map_1000_100'
        map_name_1 = 'map_1001_101'
        map_name_2 = 'map_1002_102'

        vxlan_obj.fetch_exist_entries(dvs)

        vxlan_obj.create_vlan1(dvs,"Vlan100")
        vxlan_obj.create_vlan1(dvs,"Vlan101")
        vxlan_obj.create_vlan1(dvs,"Vlan102")
        vxlan_obj.create_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')
        vxlan_obj.create_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        vxlan_obj.create_vxlan_tunnel_map(dvs, tunnel_name, map_name_1, '1001', 'Vlan101')
        vxlan_obj.create_vxlan_tunnel_map(dvs, tunnel_name, map_name_2, '1002', 'Vlan102')

        vlanlist = ['100', '101', '102']
        vnilist = ['1000', '1001', '1002']

        print("Testing SIP Tunnel Creation")
        vxlan_obj.check_vxlan_sip_tunnel(dvs, tunnel_name, '6.6.6.6', vlanlist, vnilist)

        print("Testing Tunnel Map Entry")
        vxlan_obj.check_vxlan_tunnel_map_entry(dvs, tunnel_name, vlanlist, vnilist)

        print("Testing Tunnel Map entry removal")
        vxlan_obj.remove_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        vxlan_obj.remove_vxlan_tunnel_map(dvs, tunnel_name, map_name_1, '1001', 'Vlan101')
        vxlan_obj.remove_vxlan_tunnel_map(dvs, tunnel_name, map_name_2, '1002', 'Vlan102')
        vxlan_obj.check_vxlan_tunnel_map_entry_delete(dvs, tunnel_name, vlanlist, vnilist)

        print("Testing SIP Tunnel Deletion")
        vxlan_obj.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan_obj.check_vxlan_sip_tunnel_delete(dvs, tunnel_name, '6.6.6.6')

#    Test 2 - Vlan extension Tests
    def test_vlan_extension(self, dvs, testlog):
        vxlan_obj = self.get_vxlan_obj()

        tunnel_name = 'tunnel_2'
        map_name = 'map_1000_100'
        map_name_1 = 'map_1001_101'
        map_name_2 = 'map_1002_102'
        vlanlist = ['100', '101', '102']
        vnilist = ['1000', '1001', '1002']

        vxlan_obj.fetch_exist_entries(dvs)
        vxlan_obj.create_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')
        vxlan_obj.create_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        vxlan_obj.create_vxlan_tunnel_map(dvs, tunnel_name, map_name_1, '1001', 'Vlan101')
        vxlan_obj.create_vxlan_tunnel_map(dvs, tunnel_name, map_name_2, '1002', 'Vlan102')

        vxlan_obj.check_vxlan_sip_tunnel(dvs, tunnel_name, '6.6.6.6', vlanlist, vnilist)
        vxlan_obj.check_vxlan_tunnel_map_entry(dvs, tunnel_name, vlanlist, vnilist)

        vxlan_obj.create_evpn_nvo(dvs, 'nvo1', tunnel_name)
        vxlan_obj.create_evpn_remote_vni(dvs, 'Vlan100', '7.7.7.7', '1000')

        print("Testing VLAN 100 extension")
        vxlan_obj.check_vlan_extension_p2mp(dvs, '100', '6.6.6.6', '7.7.7.7')

        vxlan_obj.create_evpn_remote_vni(dvs, 'Vlan101', '7.7.7.7', '1001')
        vxlan_obj.create_evpn_remote_vni(dvs, 'Vlan102', '7.7.7.7', '1002')

        print("Testing VLAN 101 extension")
        vxlan_obj.check_vlan_extension_p2mp(dvs, '101', '6.6.6.6', '7.7.7.7')

        print("Testing VLAN 102 extension")
        vxlan_obj.check_vlan_extension_p2mp(dvs, '102', '6.6.6.6', '7.7.7.7')

        print("Testing another remote endpoint to 8.8.8.8")
        vxlan_obj.create_evpn_remote_vni(dvs, 'Vlan100', '8.8.8.8', '1000')
        print("Testing remote endpoint creation to 8.8.8.8")

        print("Testing VLAN 100 extension to 8.8.8.8 and 7.7.7.7")
        vxlan_obj.check_vlan_extension_p2mp(dvs, '100', '6.6.6.6', '8.8.8.8')
        vxlan_obj.check_vlan_extension_p2mp(dvs, '100', '6.6.6.6', '7.7.7.7')

        print("Testing Vlan Extension removal")
        vxlan_obj.remove_evpn_remote_vni(dvs, 'Vlan100', '7.7.7.7')
        vxlan_obj.remove_evpn_remote_vni(dvs, 'Vlan101', '7.7.7.7')
        vxlan_obj.check_vlan_extension_delete_p2mp(dvs, '100', '6.6.6.6', '7.7.7.7')
        vxlan_obj.check_vlan_extension_delete_p2mp(dvs, '101', '6.6.6.6', '7.7.7.7')

        print("Testing Last Vlan removal and remote endpoint delete")
        vxlan_obj.remove_evpn_remote_vni(dvs, 'Vlan102', '7.7.7.7')
        vxlan_obj.check_vlan_extension_delete_p2mp(dvs, '102', '6.6.6.6', '7.7.7.7')

        print("Testing Last Vlan removal and remote endpoint delete for 8.8.8.8")
        vxlan_obj.remove_evpn_remote_vni(dvs, 'Vlan100', '8.8.8.8')
        vxlan_obj.check_vlan_extension_delete_p2mp(dvs, '100', '6.6.6.6', '8.8.8.8')

        print("Testing remote endpoint again to 8.8.8.8")
        vxlan_obj.create_evpn_remote_vni(dvs, 'Vlan100', '8.8.8.8', '1000')
        print("Testing remote endpoint creation to 8.8.8.8")

        print("Testing VLAN 100 extension to 8.8.8.8")
        vxlan_obj.check_vlan_extension_p2mp(dvs, '100', '6.6.6.6', '8.8.8.8')

        print("Testing Last Vlan removal and remote endpoint delete for 8.8.8.8")
        vxlan_obj.remove_evpn_remote_vni(dvs, 'Vlan100', '8.8.8.8')
        vxlan_obj.check_vlan_extension_delete_p2mp(dvs, '100', '6.6.6.6', '8.8.8.8')

        vxlan_obj.remove_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        vxlan_obj.remove_vxlan_tunnel_map(dvs, tunnel_name, map_name_1, '1001', 'Vlan101')
        vxlan_obj.remove_vxlan_tunnel_map(dvs, tunnel_name, map_name_2, '1002', 'Vlan102')
        vxlan_obj.check_vxlan_tunnel_map_entry_delete(dvs, tunnel_name, vlanlist, vnilist)

        print("Testing SIP Tunnel Deletion")
        vxlan_obj.remove_evpn_nvo(dvs, 'nvo1')
        vxlan_obj.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan_obj.check_vxlan_sip_tunnel_delete(dvs, tunnel_name, '6.6.6.6')
