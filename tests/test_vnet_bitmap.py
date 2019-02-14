from swsscommon import swsscommon
import time
import test_vnet as vnet


# Define fake platform for "DVS" fixture, so it will set "platform" environment variable for "orchagent".
# It is needed in order to enable platform specific "orchagent" code for testing "bitmap" VNET implementation.
DVS_FAKE_PLATFORM = "mellanox"


'''
Implements "check" APIs for the "bitmap" VNET feature.
These APIs provides functionality to verify whether specified config is correcly applied to ASIC_DB.
Such object should be passed to the test class, so it can use valid APIs to check whether config is applied.
'''
class VnetBitmapVxlanTunnel(object):

    ASIC_TUNNEL_TABLE        = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL"
    ASIC_TUNNEL_MAP          = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP"
    ASIC_TUNNEL_MAP_ENTRY    = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY"
    ASIC_TUNNEL_TERM_ENTRY   = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY"
    ASIC_RIF_TABLE           = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
    ASIC_NEXT_HOP            = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP"
    ASIC_BITMAP_CLASS_ENTRY  = "ASIC_STATE:SAI_OBJECT_TYPE_TABLE_BITMAP_CLASSIFICATION_ENTRY"
    ASIC_BITMAP_ROUTER_ENTRY = "ASIC_STATE:SAI_OBJECT_TYPE_TABLE_BITMAP_ROUTER_ENTRY"
    ASIC_FDB_ENTRY           = "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY"
    ASIC_NEIGH_ENTRY         = "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY"

    tunnel_map_ids        = set()
    tunnel_map_entry_ids  = set()
    tunnel_ids            = set()
    tunnel_term_ids       = set()
    vnet_bitmap_class_ids = set()
    vnet_bitmap_route_ids = set()
    tunnel_map_map        = {}
    vnet_map              = {}
    vnet_mac_vni_list     = []

    _loopback_id = 0
    _def_vr_id = 0
    _switch_mac = None

    @property
    def loopback_id(self):
        return type(self)._loopback_id

    @loopback_id.setter
    def loopback_id(self, val):
        type(self)._loopback_id = val

    @property
    def def_vr_id(self):
        return type(self)._def_vr_id

    @def_vr_id.setter
    def def_vr_id(self, val):
        type(self)._def_vr_id = val

    @property
    def switch_mac(self):
        return type(self)._switch_mac

    @switch_mac.setter
    def switch_mac(self, val):
        type(self)._switch_mac = val

    def fetch_exist_entries(self, dvs):
        self.tunnel_ids = vnet.get_exist_entries(dvs, self.ASIC_TUNNEL_TABLE)
        self.tunnel_map_ids = vnet.get_exist_entries(dvs, self.ASIC_TUNNEL_MAP)
        self.tunnel_map_entry_ids = vnet.get_exist_entries(dvs, self.ASIC_TUNNEL_MAP_ENTRY)
        self.tunnel_term_ids = vnet.get_exist_entries(dvs, self.ASIC_TUNNEL_TERM_ENTRY)
        self.vnet_bitmap_class_ids = vnet.get_exist_entries(dvs, self.ASIC_BITMAP_CLASS_ENTRY)
        self.vnet_bitmap_route_ids = vnet.get_exist_entries(dvs, self.ASIC_BITMAP_ROUTER_ENTRY)
        self.rifs = vnet.get_exist_entries(dvs, self.ASIC_RIF_TABLE)
        self.nhops = vnet.get_exist_entries(dvs, self.ASIC_NEXT_HOP)
        self.fdbs = vnet.get_exist_entries(dvs, self.ASIC_FDB_ENTRY)
        self.neighs = vnet.get_exist_entries(dvs, self.ASIC_NEIGH_ENTRY)

        if not self.loopback_id:
            self.loopback_id = vnet.get_lo(dvs)

        if not self.def_vr_id:
            self.def_vr_id = vnet.get_default_vr_id(dvs)

        if self.switch_mac is None:
            self.switch_mac = vnet.get_switch_mac(dvs)

    def check_vxlan_tunnel(self, dvs, tunnel_name, src_ip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tunnel_map_id  = vnet.get_created_entries(asic_db, self.ASIC_TUNNEL_MAP, self.tunnel_map_ids, 2)
        tunnel_id      = vnet.get_created_entry(asic_db, self.ASIC_TUNNEL_TABLE, self.tunnel_ids)
        tunnel_term_id = vnet.get_created_entry(asic_db, self.ASIC_TUNNEL_TERM_ENTRY, self.tunnel_term_ids)

        assert vnet.how_many_entries_exist(asic_db, self.ASIC_TUNNEL_MAP) == (len(self.tunnel_map_ids) + 2),\
                                           "The TUNNEL_MAP wasn't created"
        assert vnet.how_many_entries_exist(asic_db, self.ASIC_TUNNEL_MAP_ENTRY) == len(self.tunnel_map_entry_ids),\
                                           "The TUNNEL_MAP_ENTRY is created"
        assert vnet.how_many_entries_exist(asic_db, self.ASIC_TUNNEL_TABLE) == (len(self.tunnel_ids) + 1),\
                                           "The TUNNEL wasn't created"
        assert vnet.how_many_entries_exist(asic_db, self.ASIC_TUNNEL_TERM_ENTRY) == (len(self.tunnel_term_ids) + 1),\
                                           "The TUNNEL_TERM_TABLE_ENTRY wasn't created"

        expected_attrs = { 'SAI_TUNNEL_MAP_ATTR_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_BRIDGE_IF' }
        vnet.check_object(asic_db, self.ASIC_TUNNEL_MAP, tunnel_map_id[0], expected_attrs)

        expected_attrs = { 'SAI_TUNNEL_MAP_ATTR_TYPE': 'SAI_TUNNEL_MAP_TYPE_BRIDGE_IF_TO_VNI' }
        vnet.check_object(asic_db, self.ASIC_TUNNEL_MAP, tunnel_map_id[1], expected_attrs)

        expected_attrs = {
            'SAI_TUNNEL_ATTR_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
            'SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE': self.loopback_id,
            'SAI_TUNNEL_ATTR_DECAP_MAPPERS': '1:%s' % tunnel_map_id[0],
            'SAI_TUNNEL_ATTR_ENCAP_MAPPERS': '1:%s' % tunnel_map_id[1],
            'SAI_TUNNEL_ATTR_ENCAP_SRC_IP': src_ip,
        }
        vnet.check_object(asic_db, self.ASIC_TUNNEL_TABLE, tunnel_id, expected_attrs)

        expected_attrs = {
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE': 'SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP',
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID': self.def_vr_id,
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP': src_ip,
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID': tunnel_id,
        }
        vnet.check_object(asic_db, self.ASIC_TUNNEL_TERM_ENTRY, tunnel_term_id, expected_attrs)

        self.tunnel_map_ids.update(tunnel_map_id)
        self.tunnel_ids.add(tunnel_id)
        self.tunnel_term_ids.add(tunnel_term_id)
        self.tunnel_map_map[tunnel_name] = tunnel_map_id

    def check_vxlan_tunnel_entry(self, dvs, tunnel_name, vnet_name, vni_id):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

        time.sleep(2)

        if (self.tunnel_map_map.get(tunnel_name) is None):
            tunnel_map_id = vnet.get_created_entries(asic_db, self.ASIC_TUNNEL_MAP, self.tunnel_map_ids, 2)
        else:
            tunnel_map_id = self.tunnel_map_map[tunnel_name]

        tunnel_map_entry_id = vnet.get_created_entries(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, self.tunnel_map_entry_ids, 1)

        assert vnet.how_many_entries_exist(asic_db, self.ASIC_TUNNEL_MAP_ENTRY) == (len(self.tunnel_map_entry_ids) + 1),\
                                           "The TUNNEL_MAP_ENTRY is created too early"

        expected_attrs = {
            'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_BRIDGE_IF_TO_VNI',
            'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': tunnel_map_id[1],
            'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE': vni_id,
        }
        vnet.check_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, tunnel_map_entry_id[0], expected_attrs)

        self.tunnel_map_entry_ids.update(tunnel_map_entry_id)
        self.vnet_map[vnet_name].update({'vni':vni_id})

    def check_vnet_entry(self, dvs, name, peer_list=[]):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        assert vnet.how_many_entries_exist(asic_db, self.ASIC_BITMAP_CLASS_ENTRY) == (len(self.vnet_bitmap_class_ids) + 1),\
                                           "The bitmap class object is not created"

        new_bitmap_class_id = vnet.get_created_entries(asic_db, self.ASIC_BITMAP_CLASS_ENTRY, self.vnet_bitmap_class_ids, 1)

        self.vnet_bitmap_class_ids.update(new_bitmap_class_id)
        self.rifs = vnet.get_exist_entries(dvs, self.ASIC_RIF_TABLE)
        self.vnet_map.update({name:{}})

    def check_router_interface(self, dvs, name, vlan_oid=0):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        expected_attrs = {
            "SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID": self.def_vr_id,
            "SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS": self.switch_mac,
            "SAI_ROUTER_INTERFACE_ATTR_MTU": "9100",
        }

        if vlan_oid:
            expected_attrs.update({'SAI_ROUTER_INTERFACE_ATTR_TYPE': 'SAI_ROUTER_INTERFACE_TYPE_VLAN'})
            expected_attrs.update({'SAI_ROUTER_INTERFACE_ATTR_VLAN_ID': vlan_oid})
        else:
            expected_attrs.update({'SAI_ROUTER_INTERFACE_ATTR_TYPE': 'SAI_ROUTER_INTERFACE_TYPE_PORT'})

        new_rif = vnet.get_created_entry(asic_db, self.ASIC_RIF_TABLE, self.rifs)
        vnet.check_object(asic_db, self.ASIC_RIF_TABLE, new_rif, expected_attrs)

        new_bitmap_route = vnet.get_created_entries(asic_db, self.ASIC_BITMAP_ROUTER_ENTRY, self.vnet_bitmap_route_ids, 1)

        new_bitmap_class_id  = vnet.get_created_entries(asic_db, self.ASIC_BITMAP_CLASS_ENTRY, self.vnet_bitmap_class_ids, 1)

        self.rifs.add(new_rif)
        self.vnet_bitmap_route_ids.update(new_bitmap_route)
        self.vnet_bitmap_class_ids.update(new_bitmap_class_id)

    def check_vnet_local_routes(self, dvs, name):
        # TODO: Implement once support of local vnet_bitmap_route_ids is added for "bitmap" VNET implementation
        pass

    def check_vnet_routes(self, dvs, name, endpoint, tunnel, mac="", vni=0):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        _vni = str(vni) if vni != 0 else self.vnet_map[name]['vni']

        if (mac,_vni) not in self.vnet_mac_vni_list:
            new_fdb = vnet.get_created_entry(asic_db, self.ASIC_FDB_ENTRY, self.fdbs)

            expected_attrs = {
                "SAI_FDB_ENTRY_ATTR_TYPE": "SAI_FDB_ENTRY_TYPE_STATIC",
                "SAI_FDB_ENTRY_ATTR_ENDPOINT_IP": endpoint
            }
            vnet.check_object(asic_db, self.ASIC_FDB_ENTRY, new_fdb, expected_attrs)

            self.fdbs.add(new_fdb)
            self.vnet_mac_vni_list.append((mac,_vni))

            new_neigh = vnet.get_created_entry(asic_db, self.ASIC_NEIGH_ENTRY, self.neighs)

            expected_attrs = { "SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS": mac if mac != "" else "00:00:00:00:00:00" }
            vnet.check_object(asic_db, self.ASIC_NEIGH_ENTRY, new_neigh, expected_attrs)

            self.neighs.add(new_neigh)

        new_nh = vnet.get_created_entry(asic_db, self.ASIC_NEXT_HOP, self.nhops)

        expected_attrs = { "SAI_TABLE_BITMAP_ROUTER_ENTRY_ATTR_ACTION": "SAI_TABLE_BITMAP_ROUTER_ENTRY_ACTION_TO_NEXTHOP" }

        new_bitmap_route = vnet.get_created_entries(asic_db, self.ASIC_BITMAP_ROUTER_ENTRY, self.vnet_bitmap_route_ids, 1)
        vnet.check_object(asic_db, self.ASIC_BITMAP_ROUTER_ENTRY, new_bitmap_route[0], expected_attrs)

        self.nhops.add(new_nh)
        self.vnet_bitmap_route_ids.update(new_bitmap_route)


'''
Provides test cases for the "bitmap" VNET implementation.
Test cases are inherited from "test_vnet.py::TestVnetOrch" since they are the same for both "legacy" and "bitmap" implementation.
Difference between these two implementations is in set SAI attributes, so different values should be checked in ASIC_DB.
This class should override "get_vnet_obj()" method in order to return object with appropriate implementation of "check" APIs.
'''
class TestVnetBitmapOrch(vnet.TestVnetOrch):

    '''
    Returns specific VNET object with the appropriate implementation of "check" APIs for the "bitmap" VNET.
    Test cases use these "check" APIs in order to verify whether correct config is applied to ASIC_DB.
    '''
    def get_vnet_obj(self):
        return VnetBitmapVxlanTunnel()

