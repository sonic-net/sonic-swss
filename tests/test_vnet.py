from swsscommon import swsscommon
import time
import json
import random
import time
from pprint import pprint


def create_entry(tbl, key, pairs):
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)
    time.sleep(1)


def create_entry_tbl(db, table, separator, key, pairs):
    tbl = swsscommon.Table(db, table)
    create_entry(tbl, key, pairs)


def create_entry_pst(db, table, separator, key, pairs):
    tbl = swsscommon.ProducerStateTable(db, table)
    create_entry(tbl, key, pairs)


def how_many_entries_exist(db, table):
    tbl =  swsscommon.Table(db, table)
    return len(tbl.getKeys())


def entries(db, table):
    tbl =  swsscommon.Table(db, table)
    return set(tbl.getKeys())


def get_exist_entries(dvs, table):
    db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    tbl =  swsscommon.Table(db, table)
    return set(tbl.getKeys())


def get_created_entry(db, table, existed_entries):
    tbl =  swsscommon.Table(db, table)
    entries = set(tbl.getKeys())
    new_entries = list(entries - existed_entries)
    assert len(new_entries) == 1, "Wrong number of created entries."
    return new_entries[0]


def get_created_entries(db, table, existed_entries, count):
    tbl =  swsscommon.Table(db, table)
    entries = set(tbl.getKeys())
    new_entries = list(entries - existed_entries)
    assert len(new_entries) == count, "Wrong number of created entries."
    new_entries.sort()
    return new_entries


def get_default_vr_id(dvs):
    db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    table = 'ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER'
    tbl =  swsscommon.Table(db, table)
    keys = tbl.getKeys()
    assert len(keys) == 1, "Wrong number of virtual routers found"

    return keys[0]


def check_object(db, table, key, expected_attributes):
    tbl =  swsscommon.Table(db, table)
    keys = tbl.getKeys()
    assert key in keys, "The desired key is not presented"

    status, fvs = tbl.get(key)
    assert status, "Got an error when get a key"

    assert len(fvs) >= len(expected_attributes), "Incorrect attributes"

    attr_keys = {entry[0] for entry in fvs}

    for name, value in fvs:
        if name in expected_attributes:
            assert expected_attributes[name] == value, "Wrong value %s for the attribute %s = %s" % \
                                               (value, name, expected_attributes[name])


def create_vnet_local_routes(dvs, prefix, vnet_name, ifname):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

    create_entry_pst(
        app_db,
        "VNET_ROUTE_TABLE", ':', "%s:%s" % (vnet_name, prefix),
        [
            ("ifname", ifname),
        ]
    )

    time.sleep(2)


def create_vnet_routes(dvs, prefix, vnet_name, endpoint, mac="", vni=0):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

    attrs = [
            ("endpoint", endpoint),
    ]

    if vni:
        attrs.append(('vni', vni))

    if mac:
        attrs.append(('mac_address', mac))

    create_entry_pst(
        app_db,
        "VNET_ROUTE_TUNNEL_TABLE", ':', "%s:%s" % (vnet_name, prefix),
        attrs,
    )

    time.sleep(2)


def create_vlan(dvs, vlan_name, vlan_ids):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    vlan_id = vlan_name[4:]

    # create vlan
    create_entry_tbl(
        conf_db,
        "VLAN", '|', vlan_name,
        [
          ("vlanid", vlan_id),
        ],
    )

    time.sleep(1)

    vlan_oid = get_created_entry(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN", vlan_ids)

    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN", vlan_oid,
                    {
                        "SAI_VLAN_ATTR_VLAN_ID": vlan_id,
                    }
                )

    return vlan_oid


def create_vlan_interface(dvs, vlan_name, ifname, vnet_name, ipaddr):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    vlan_ids = get_exist_entries(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")

    vlan_oid = create_vlan (dvs, vlan_name, vlan_ids)

   # create a vlan member in config db
    create_entry_tbl(
        conf_db,
        "VLAN_MEMBER", '|', "%s|%s" % (vlan_name, ifname),
        [
          ("tagging_mode", "untagged"),
        ],
    )

    time.sleep(1)

    # create vlan interface in config db
    create_entry_tbl(
        conf_db,
        "VLAN_INTERFACE", '|', vlan_name,
        [
          ("vnet_name", vnet_name),
        ],
    )

    #FIXME - This is created by IntfMgr
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(
        app_db,
        "INTF_TABLE", ':', vlan_name,
        [
            ("vnet_name", vnet_name),
        ],
    )
    time.sleep(2)

    create_entry_tbl(
        conf_db,
        "VLAN_INTERFACE", '|', "%s|%s" % (vlan_name, ipaddr),
        [
          ("family", "IPv4"),
        ],
    )

    time.sleep(2)

    return vlan_oid


def create_phy_interface(dvs, ifname, vnet_name, ipaddr):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    exist_rifs = get_exist_entries(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")

    # create vlan interface in config db
    create_entry_tbl(
        conf_db,
        "INTERFACE", '|', ifname,
        [
          ("vnet_name", vnet_name),
        ],
    )

    #FIXME - This is created by IntfMgr
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(
        app_db,
        "INTF_TABLE", ':', ifname,
        [
            ("vnet_name", vnet_name),
        ],
    )
    time.sleep(2)

    create_entry_tbl(
        conf_db,
        "INTERFACE", '|', "%s|%s" % (ifname, ipaddr),
        [
          ("family", "IPv4"),
        ],
    )


def create_vnet_entry(dvs, name, tunnel, vni, peer_list):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

    attrs = [
            ("vxlan_tunnel", tunnel),
            ("vni", vni),
            ("peer_list", peer_list),
    ]

    # create the VXLAN tunnel Term entry in Config DB
    create_entry_tbl(
        conf_db,
        "VNET", '|', name,
        attrs,
    )

    time.sleep(2)


def create_vxlan_tunnel(dvs, name, src_ip):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    attrs = [
            ("src_ip", src_ip),
    ]

    # create the VXLAN tunnel Term entry in Config DB
    create_entry_tbl(
        conf_db,
        "VXLAN_TUNNEL", '|', name,
        attrs,
    )


def get_lo(dvs):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    vr_id = get_default_vr_id(dvs)

    tbl = swsscommon.Table(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE')

    entries = tbl.getKeys()
    lo_id = None
    for entry in entries:
        status, fvs = tbl.get(entry)
        assert status, "Got an error when get a key"
        for key, value in fvs:
            if key == 'SAI_ROUTER_INTERFACE_ATTR_TYPE' and value == 'SAI_ROUTER_INTERFACE_TYPE_LOOPBACK':
                lo_id = entry
                break
        else:
            assert False, 'Don\'t found loopback id'

    return lo_id


def get_switch_mac(dvs):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

    tbl = swsscommon.Table(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_SWITCH')

    entries = tbl.getKeys()
    mac = None
    for entry in entries:
        status, fvs = tbl.get(entry)
        assert status, "Got an error when get a key"
        for key, value in fvs:
            if key == 'SAI_SWITCH_ATTR_SRC_MAC_ADDRESS':
                mac = value
                break
        else:
            assert False, 'Don\'t found switch mac'

    return mac


loopback_id = 0
def_vr_id = 0
switch_mac = None


class VnetVxlanVrfTunnel(object):

    ASIC_TUNNEL_TABLE       = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL"
    ASIC_TUNNEL_MAP         = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP"
    ASIC_TUNNEL_MAP_ENTRY   = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY"
    ASIC_TUNNEL_TERM_ENTRY  = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY"
    ASIC_RIF_TABLE          = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
    ASIC_VRF_TABLE          = "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"
    ASIC_ROUTE_ENTRY        = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"
    ASIC_NEXT_HOP           = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP"

    tunnel_map_ids       = set()
    tunnel_map_entry_ids = set()
    tunnel_ids           = set()
    tunnel_term_ids      = set()
    tunnel_map_map       = {}
    tunnel               = {}
    vnet_vr_ids          = set()
    vr_map               = {}
    nh_ids               = {}

    def fetch_exist_entries(self, dvs):
        self.vnet_vr_ids = get_exist_entries(dvs, self.ASIC_VRF_TABLE)
        self.tunnel_ids = get_exist_entries(dvs, self.ASIC_TUNNEL_TABLE)
        self.tunnel_map_ids = get_exist_entries(dvs, self.ASIC_TUNNEL_MAP)
        self.tunnel_map_entry_ids = get_exist_entries(dvs, self.ASIC_TUNNEL_MAP_ENTRY)
        self.tunnel_term_ids = get_exist_entries(dvs, self.ASIC_TUNNEL_TERM_ENTRY)
        self.rifs = get_exist_entries(dvs, self.ASIC_RIF_TABLE)
        self.routes = get_exist_entries(dvs, self.ASIC_ROUTE_ENTRY)
        self.nhops = get_exist_entries(dvs, self.ASIC_NEXT_HOP)

        global loopback_id, def_vr_id, switch_mac
        if not loopback_id:
            loopback_id = get_lo(dvs)

        if not def_vr_id:
            def_vr_id = get_default_vr_id(dvs)

        if switch_mac is None:
            switch_mac = get_switch_mac(dvs)

    def check_vxlan_tunnel(self, dvs, tunnel_name, src_ip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        global loopback_id, def_vr_id

        tunnel_map_id  = get_created_entries(asic_db, self.ASIC_TUNNEL_MAP, self.tunnel_map_ids, 2)
        tunnel_id      = get_created_entry(asic_db, self.ASIC_TUNNEL_TABLE, self.tunnel_ids)
        tunnel_term_id = get_created_entry(asic_db, self.ASIC_TUNNEL_TERM_ENTRY, self.tunnel_term_ids)

        # check that the vxlan tunnel termination are there
        assert how_many_entries_exist(asic_db, self.ASIC_TUNNEL_MAP) == (len(self.tunnel_map_ids) + 2), "The TUNNEL_MAP wasn't created"
        assert how_many_entries_exist(asic_db, self.ASIC_TUNNEL_MAP_ENTRY) == len(self.tunnel_map_entry_ids), "The TUNNEL_MAP_ENTRY is created"
        assert how_many_entries_exist(asic_db, self.ASIC_TUNNEL_TABLE) == (len(self.tunnel_ids) + 1), "The TUNNEL wasn't created"
        assert how_many_entries_exist(asic_db, self.ASIC_TUNNEL_TERM_ENTRY) == (len(self.tunnel_term_ids) + 1), "The TUNNEL_TERM_TABLE_ENTRY wasm't created"

        check_object(asic_db, self.ASIC_TUNNEL_MAP, tunnel_map_id[0],
                        {
                            'SAI_TUNNEL_MAP_ATTR_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID',
                        }
                )

        check_object(asic_db, self.ASIC_TUNNEL_MAP, tunnel_map_id[1],
                        {
                            'SAI_TUNNEL_MAP_ATTR_TYPE': 'SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI',
                        }
                )

        check_object(asic_db, self.ASIC_TUNNEL_TABLE, tunnel_id,
                    {
                        'SAI_TUNNEL_ATTR_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
                        'SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE': loopback_id,
                        'SAI_TUNNEL_ATTR_DECAP_MAPPERS': '1:%s' % tunnel_map_id[0],
                        'SAI_TUNNEL_ATTR_ENCAP_MAPPERS': '1:%s' % tunnel_map_id[1],
                        'SAI_TUNNEL_ATTR_ENCAP_SRC_IP': src_ip,
                    }
                )

        expected_attributes = {
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE': 'SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP',
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID': def_vr_id,
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP': src_ip,
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID': tunnel_id,
        }

        check_object(asic_db, self.ASIC_TUNNEL_TERM_ENTRY, tunnel_term_id, expected_attributes)

        self.tunnel_map_ids.update(tunnel_map_id)
        self.tunnel_ids.add(tunnel_id)
        self.tunnel_term_ids.add(tunnel_term_id)
        self.tunnel_map_map[tunnel_name] = tunnel_map_id
        self.tunnel[tunnel_name] = tunnel_id

    def check_vxlan_tunnel_entry(self, dvs, tunnel_name, vnet_name, vni_id):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

        time.sleep(2)

        if (self.tunnel_map_map.get(tunnel_name) is None):
            tunnel_map_id = get_created_entries(asic_db, self.ASIC_TUNNEL_MAP, self.tunnel_map_ids, 2)
        else:
            tunnel_map_id = self.tunnel_map_map[tunnel_name]

        tunnel_map_entry_id = get_created_entries(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, self.tunnel_map_entry_ids, 2)

        # check that the vxlan tunnel termination are there
        assert how_many_entries_exist(asic_db, self.ASIC_TUNNEL_MAP_ENTRY) == (len(self.tunnel_map_entry_ids) + 2), "The TUNNEL_MAP_ENTRY is created too early"

        check_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, tunnel_map_entry_id[0],
            {
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI',
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': tunnel_map_id[1],
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_KEY': self.vr_map[vnet_name].get('ing'),
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE': vni_id,
            }
        )

        check_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, tunnel_map_entry_id[1],
            {
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID',
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': tunnel_map_id[0],
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY': vni_id,
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_VALUE': self.vr_map[vnet_name].get('egr'),
            }
        )

        self.tunnel_map_entry_ids.update(tunnel_map_entry_id)

    def check_vnet_entry(self, dvs, name, peer_list=[]):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        #Check virtual router objects
        assert how_many_entries_exist(asic_db, self.ASIC_VRF_TABLE) == (len(self.vnet_vr_ids) + 2),\
                                     "The VR objects are not created"

        new_vr_ids  = get_created_entries(asic_db, self.ASIC_VRF_TABLE, self.vnet_vr_ids, 2)

        self.vnet_vr_ids.update(new_vr_ids)
        self.vr_map[name] = { 'ing':new_vr_ids[0], 'egr':new_vr_ids[1], 'peer':peer_list }

    def vnet_route_ids(self, dvs, name, local=False):
        vr_set = set()

        if local:
            vr_set.add(self.vr_map[name].get('egr'))
        else:
            vr_set.add(self.vr_map[name].get('ing'))

        try:
            for peer in self.vr_map[name].get('peer'):
                vr_set.add(self.vr_map[peer].get('ing'))
        except IndexError:
            pass

        return vr_set

    def check_router_interface(self, dvs, name, vlan_oid=0):
        # Check RIF in ingress VRF
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        global switch_mac

        expected_attr = {
                        "SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID": self.vr_map[name].get('ing'),
                        "SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS": switch_mac,
                        "SAI_ROUTER_INTERFACE_ATTR_MTU": "9100",
                    }

        if vlan_oid:
            expected_attr.update({'SAI_ROUTER_INTERFACE_ATTR_TYPE': 'SAI_ROUTER_INTERFACE_TYPE_VLAN'})
            expected_attr.update({'SAI_ROUTER_INTERFACE_ATTR_VLAN_ID': vlan_oid})
        else:
            expected_attr.update({'SAI_ROUTER_INTERFACE_ATTR_TYPE': 'SAI_ROUTER_INTERFACE_TYPE_PORT'})

        new_rif = get_created_entry(asic_db, self.ASIC_RIF_TABLE, self.rifs)
        check_object(asic_db, self.ASIC_RIF_TABLE, new_rif, expected_attr)

        #IP2ME and subnet routes will be created with every router interface
        new_route = get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, 2)

        self.rifs.add(new_rif)
        self.routes.update(new_route)

    def check_vnet_local_routes(self, dvs, name):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        vr_ids = self.vnet_route_ids(dvs, name, True)
        count = len(vr_ids)

        new_route = get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, count)

        #Check if the route is duplicated to egress VRF
        asic_vrs = set()
        for idx in range(count):
            rt_key = json.loads(new_route[idx])
            asic_vrs.add(rt_key['vr'])

        assert asic_vrs == vr_ids

        self.routes.update(new_route)

    def check_vnet_routes(self, dvs, name, endpoint, tunnel, mac="", vni=0):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        vr_ids = self.vnet_route_ids(dvs, name)
        count = len(vr_ids)

        # Check routes in ingress VRF
        expected_attr = {
                        "SAI_NEXT_HOP_ATTR_TYPE": "SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP",
                        "SAI_NEXT_HOP_ATTR_IP": endpoint,
                        "SAI_NEXT_HOP_ATTR_TUNNEL_ID": self.tunnel[tunnel],
                    }

        if vni:
            expected_attr.update({'SAI_NEXT_HOP_ATTR_TUNNEL_VNI': vni})

        if mac:
            expected_attr.update({'SAI_NEXT_HOP_ATTR_TUNNEL_MAC': mac})

        if endpoint in self.nh_ids:
            new_nh = self.nh_ids[endpoint]
        else:
            new_nh = get_created_entry(asic_db, self.ASIC_NEXT_HOP, self.nhops)
            self.nh_ids[endpoint] = new_nh
            self.nhops.add(new_nh)

        check_object(asic_db, self.ASIC_NEXT_HOP, new_nh, expected_attr)
        new_route = get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, count)

        #Check if the route is in expected VRF
        asic_vrs = set()
        for idx in range(count):
            check_object(asic_db, self.ASIC_ROUTE_ENTRY, new_route[idx],
                        {
                            "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID": new_nh,
                        }
                    )
            rt_key = json.loads(new_route[idx])
            asic_vrs.add(rt_key['vr'])

        assert asic_vrs == vr_ids

        self.routes.update(new_route)

class TestVnetOrch(object):

    '''
    Test 1 - Create Vlan Interface, Tunnel and Vnet
    '''
    def test_vnet_orch_1(self, dvs, testlog):
        vnet_obj = VnetVxlanVrfTunnel()

        tunnel_name = 'tunnel_1'

        vnet_obj.fetch_exist_entries(dvs)
        create_vxlan_tunnel(dvs, tunnel_name, '10.10.10.10')
        create_vnet_entry(dvs, 'Vnet_2000', tunnel_name, '2000', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet_2000')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet_2000', '2000')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '10.10.10.10')

        vid = create_vlan_interface(dvs, "Vlan100", "Ethernet24", "Vnet_2000", "100.100.3.1/24")
        vnet_obj.check_router_interface(dvs, 'Vnet_2000', vid)

        vid = create_vlan_interface(dvs, "Vlan101", "Ethernet28", "Vnet_2000", "100.100.4.1/24")
        vnet_obj.check_router_interface(dvs, 'Vnet_2000', vid)

        create_vnet_routes(dvs, "100.100.1.1/32", 'Vnet_2000', '10.10.10.1')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_2000', '10.10.10.1', tunnel_name)

        create_vnet_local_routes(dvs, "100.100.3.0/24", 'Vnet_2000', 'Vlan100')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_2000')

        create_vnet_local_routes(dvs, "100.100.4.0/24", 'Vnet_2000', 'Vlan101')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_2000')

        #Create Physical Interface in another Vnet

        create_vnet_entry(dvs, 'Vnet_2001', tunnel_name, '2001', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet_2001')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet_2001', '2001')

        # use Ethernet32 so it doesn't interfere with the following test cases.
        create_phy_interface(dvs, "Ethernet32", "Vnet_2001", "100.102.1.1/24")
        vnet_obj.check_router_interface(dvs, 'Vnet_2001')

        create_vnet_routes(dvs, "100.100.2.1/32", 'Vnet_2001', '10.10.10.2', "00:12:34:56:78:9A")
        vnet_obj.check_vnet_routes(dvs, 'Vnet_2001', '10.10.10.2', tunnel_name, "00:12:34:56:78:9A")

        create_vnet_local_routes(dvs, "100.102.1.0/24", 'Vnet_2001', 'Ethernet32')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_2001')

    '''
    Test 2 - Two VNets, One HSMs per VNet
    '''
    def test_vnet_orch_2(self, dvs, testlog):
        vnet_obj = VnetVxlanVrfTunnel()

        tunnel_name = 'tunnel_2'

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')
        create_vnet_entry(dvs, 'Vnet_1', tunnel_name, '1111', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet_1')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet_1', '1111')

        tun_id = vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')

        vid = create_vlan_interface(dvs, "Vlan1001", "Ethernet0", "Vnet_1", "1.1.10.1/24")
        vnet_obj.check_router_interface(dvs, 'Vnet_1', vid)

        create_vnet_routes(dvs, "1.1.1.10/32", 'Vnet_1', '100.1.1.10')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_1', '100.1.1.10', tunnel_name)

        create_vnet_routes(dvs, "1.1.1.11/32", 'Vnet_1', '100.1.1.10')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_1', '100.1.1.10', tunnel_name)

        create_vnet_routes(dvs, "1.1.1.12/32", 'Vnet_1', '200.200.1.200')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_1', '200.200.1.200', tunnel_name)

        create_vnet_routes(dvs, "1.1.1.14/32", 'Vnet_1', '200.200.1.201')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_1', '200.200.1.201', tunnel_name)

        create_vnet_local_routes(dvs, "1.1.10.0/24", 'Vnet_1', 'Vlan1001')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_1')

        create_vnet_entry(dvs, 'Vnet_2', tunnel_name, '2222', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet_2')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet_2', '2222')

        vid = create_vlan_interface(dvs, "Vlan1002", "Ethernet4", "Vnet_2", "2.2.10.1/24")
        vnet_obj.check_router_interface(dvs, 'Vnet_2', vid)

        create_vnet_routes(dvs, "2.2.2.10/32", 'Vnet_2', '100.1.1.20')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_2', '100.1.1.20', tunnel_name)

        create_vnet_routes(dvs, "2.2.2.11/32", 'Vnet_2', '100.1.1.20')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_2', '100.1.1.20', tunnel_name)

        create_vnet_local_routes(dvs, "2.2.10.0/24", 'Vnet_2', 'Vlan1002')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_2')

    '''
    Test 3 - Two VNets, One HSMs per VNet, Peering
    '''
    def test_vnet_orch_3(self, dvs, testlog):
        vnet_obj = VnetVxlanVrfTunnel()

        tunnel_name = 'tunnel_3'

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '7.7.7.7')

        create_vnet_entry(dvs, 'Vnet_10', tunnel_name, '1111', "Vnet_20")

        vnet_obj.check_vnet_entry(dvs, 'Vnet_10', ['Vnet_20'])
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet_10', '1111')

        create_vnet_entry(dvs, 'Vnet_20', tunnel_name, '2222', "Vnet_10")

        vnet_obj.check_vnet_entry(dvs, 'Vnet_20', ['Vnet_10'])
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet_20', '2222')

        tun_id = vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '7.7.7.7')

        vid = create_vlan_interface(dvs, "Vlan2001", "Ethernet8", "Vnet_10", "5.5.10.1/24")
        vnet_obj.check_router_interface(dvs, 'Vnet_10', vid)

        vid = create_vlan_interface(dvs, "Vlan2002", "Ethernet12", "Vnet_20", "8.8.10.1/24")
        vnet_obj.check_router_interface(dvs, 'Vnet_20', vid)

        create_vnet_routes(dvs, "5.5.5.10/32", 'Vnet_10', '50.1.1.10')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_10', '50.1.1.10', tunnel_name)

        create_vnet_routes(dvs, "8.8.8.10/32", 'Vnet_20', '80.1.1.20')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_10', '80.1.1.20', tunnel_name)

        create_vnet_local_routes(dvs, "5.5.10.0/24", 'Vnet_10', 'Vlan2001')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_10')

        create_vnet_local_routes(dvs, "8.8.10.0/24", 'Vnet_20', 'Vlan2002')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_20')
