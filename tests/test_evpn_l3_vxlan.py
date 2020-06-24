from swsscommon import swsscommon
import time
import json
import random
import pytest
from pprint import pprint


def create_entry(tbl, key, pairs):
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)
    time.sleep(1)


def create_entry_tbl(db, table, separator, key, pairs):
    tbl = swsscommon.Table(db, table)
    create_entry(tbl, key, pairs)

def delete_entry_tbl(db, table, key):
    tbl = swsscommon.Table(db, table)
    tbl._del(key)
    time.sleep(1)

def create_entry_pst(db, table, separator, key, pairs):
    tbl = swsscommon.ProducerStateTable(db, table)
    create_entry(tbl, key, pairs)

def delete_entry_pst(db, table, key):
    tbl = swsscommon.ProducerStateTable(db, table)
    tbl._del(key)
    time.sleep(1)

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

def get_deleted_entries(db, table, existed_entries, count):
    tbl =  swsscommon.Table(db, table)
    entries = set(tbl.getKeys())
    old_entries = list(existed_entries - entries)
    assert len(old_entries) == count, "Wrong number of deleted entries."
    old_entries.sort()
    return old_entries

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

def check_deleted_object(db, table, key):
    tbl =  swsscommon.Table(db, table)
    keys = tbl.getKeys()
    assert key not in keys, "The desired key is not removed"
    
def get_key_with_attr(db, table, expected_attributes ):
    tbl =  swsscommon.Table(db, table)
    keys = tbl.getKeys()
    retkey = list()
    #assert key in keys, "The desired key is not presented"

    for key in keys:
      status, fvs = tbl.get(key)
      assert status, "Got an error when get a key"

      assert len(fvs) >= len(expected_attributes), "Incorrect attributes"

      attr_keys = {entry[0] for entry in fvs}

      num_match = 0
      for name, value in fvs:
          if name in expected_attributes:
                 if expected_attributes[name] == value:
                    num_match += 1
      if num_match == len(expected_attributes):
         retkey.append(key)

    return retkey
       
def create_vrf_routes(dvs, prefix, vrf_name, endpoint, ifname, mac="", vni=0):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

    attrs = [
            ("nexthop", endpoint),
            ("ifname", ifname),
    ]

    if vni:
        attrs.append(('vni_label', vni))

    if mac:
        attrs.append(('router_mac', mac))

    create_entry_pst(
        app_db,
        "ROUTE_TABLE", ':', "%s:%s" % (vrf_name, prefix),
        attrs,
    )

    time.sleep(2)

def delete_vrf_routes(dvs, prefix, vrf_name):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

    delete_entry_pst(app_db, "ROUTE_TABLE", "%s:%s" % (vrf_name, prefix))

    time.sleep(2)

def create_vrf_routes_ecmp(dvs, prefix, vrf_name, ecmp_nexthop_attributes):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

    create_entry_pst(
        app_db,
        "ROUTE_TABLE", ':', "%s:%s" % (vrf_name, prefix),
        ecmp_nexthop_attributes,
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

def remove_vlan(dvs, vlan):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    tbl = swsscommon.Table(conf_db, "VLAN")
    tbl._del("Vlan" + vlan)
    time.sleep(1)

def create_vlan_member(dvs, vlan, interface, tagging_mode="untagged"):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    tbl = swsscommon.Table(conf_db, "VLAN_MEMBER")
    fvs = swsscommon.FieldValuePairs([("tagging_mode", tagging_mode)])
    tbl.set("Vlan" + vlan + "|" + interface, fvs)
    time.sleep(1)

def remove_vlan_member(dvs, vlan, interface):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    tbl = swsscommon.Table(conf_db, "VLAN_MEMBER")
    tbl._del("Vlan" + vlan + "|" + interface)
    time.sleep(1)

def create_vlan_interface(dvs, vlan_name, ifname, vrf_name, ipaddr):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

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
          ("vrf_name", vrf_name),
        ],
    )

    #FIXME - This is created by IntfMgr
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(
        app_db,
        "INTF_TABLE", ':', vlan_name,
        [
            ("vrf_name", vrf_name),
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


def delete_vlan_interface(dvs, ifname, ipaddr):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    delete_entry_tbl(conf_db, "VLAN_INTERFACE", "%s|%s" % (ifname, ipaddr))
    time.sleep(2)

    delete_entry_tbl(conf_db, "VLAN_INTERFACE", ifname)
    time.sleep(2)

def create_evpn_nvo(dvs, nvoname, tnlname):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    attrs = [
            ("source_vtep", tnlname),
    ]

    # create the VXLAN tunnel Term entry in Config DB
    create_entry_tbl(
        conf_db,
        "VXLAN_EVPN_NVO", '|', nvoname,
        attrs,
    )

def remove_evpn_nvo(dvs, nvoname):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    delete_entry_tbl(conf_db,"VXLAN_EVPN_NVO", nvoname,)

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

def create_vxlan_tunnel_map(dvs, tnlname, mapname, vni_id, vlan_id):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    attrs = [
            ("vni", vni_id),
            ("vlan", vlan_id),
    ]

    # create the VXLAN tunnel Term entry in Config DB
    create_entry_tbl(
        conf_db,
        "VXLAN_TUNNEL_MAP", '|', "%s|%s" % (tnlname, mapname),
        attrs,
    )


def create_vxlan_vrf_tunnel_map(dvs, vrfname, vni_id):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    attrs = [
            ("fallback", "false"),
            ("vni", vni_id),
    ]

    # create the VXLAN VRF tunnel Term entry in Config DB
    create_entry_tbl(
        conf_db,
        "VRF", '|', vrfname,
        attrs,
    )

def remove_vxlan_vrf_tunnel_map(dvs, vrfname):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    attrs = [
            ("fallback", "false"),
            ("vni", "0"),
    ]

    # remove the VXLAN VRF tunnel Term entry in Config DB
    create_entry_tbl(
        conf_db,
        "VRF", '|', vrfname,
        attrs,
    )

def create_evpn_remote_vni(dvs, vlan_id, remotevtep, vnid):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(
        app_db,
        "VXLAN_REMOTE_VNI_TABLE", ':', "%s:%s" % (vlan_id, remotevtep),
        [
            ("vni", vnid),
        ],
    )
    time.sleep(2)

def remove_vxlan_tunnel(dvs, tnlname):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    # create the VXLAN tunnel Term entry in Config DB
    delete_entry_tbl(
        conf_db,
        "VXLAN_TUNNEL", tnlname,
    )

def remove_vxlan_tunnel_map(dvs, tnlname, mapname,vni_id, vlan_id):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    attrs = [
            ("vni", vni_id),
            ("vlan", vlan_id),
    ]

    # create the VXLAN tunnel Term entry in Config DB
    delete_entry_tbl(
        conf_db,
        "VXLAN_TUNNEL_MAP", "%s|%s" % (tnlname, mapname),
    )

def remove_evpn_remote_vni(dvs, vlan_id, remotevtep ):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    delete_entry_pst(
        app_db,
        "VXLAN_REMOTE_VNI_TABLE", "%s:%s" % (vlan_id, remotevtep),
    )
    time.sleep(2)

def create_vlan1(dvs, vlan_name):
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


class VxlanTunnel(object):

    ASIC_TUNNEL_TABLE       = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL"
    ASIC_TUNNEL_MAP         = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP"
    ASIC_TUNNEL_MAP_ENTRY   = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY"
    ASIC_TUNNEL_TERM_ENTRY  = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY"
    ASIC_BRIDGE_PORT        = "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT"
    ASIC_VRF_TABLE          = "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"
    ASIC_RIF_TABLE          = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
    ASIC_ROUTE_ENTRY        = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"
    ASIC_NEXT_HOP           = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP"
    ASIC_NEXT_HOP_GRP       = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP"
    ASIC_NEXT_HOP_GRP_MEMBERS = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER"

    tunnel_map_ids           = set()
    tunnel_map_entry_ids     = set()
    tunnel_map_vrf_entry_ids = set()
    tunnel_ids               = set()
    tunnel_term_ids          = set()
    bridgeport_ids           = set()
    tunnel_map_map           = {}
    tunnel                   = {}
    tunnelterm               = {}
    mapentry_map             = {}
    diptunnel_map            = {}
    diptunterm_map           = {}
    diptunstate_map          = {}
    bridgeport_map           = {}
    vlan_id_map              = {}
    vlan_member_map          = {}
    vr_map                   = {}
    vnet_vr_ids              = set()
    nh_ids                   = {}
    nh_grp_id                = set()
    nh_grp_member_id         = set()
    route_id                 = {}

    def fetch_exist_entries(self, dvs):
        self.tunnel_ids = get_exist_entries(dvs, self.ASIC_TUNNEL_TABLE)
        self.tunnel_map_ids = get_exist_entries(dvs, self.ASIC_TUNNEL_MAP)
        self.tunnel_map_entry_ids = get_exist_entries(dvs, self.ASIC_TUNNEL_MAP_ENTRY)
        self.tunnel_term_ids = get_exist_entries(dvs, self.ASIC_TUNNEL_TERM_ENTRY)
        self.bridgeport_ids = get_exist_entries(dvs, self.ASIC_BRIDGE_PORT)
        self.vnet_vr_ids = get_exist_entries(dvs, self.ASIC_VRF_TABLE)
        self.rifs = get_exist_entries(dvs, self.ASIC_RIF_TABLE)
        self.routes = get_exist_entries(dvs, self.ASIC_ROUTE_ENTRY)
        self.nhops = get_exist_entries(dvs, self.ASIC_NEXT_HOP)
        self.nhop_grp = get_exist_entries(dvs, self.ASIC_NEXT_HOP_GRP)
        self.nhop_grp_members = get_exist_entries(dvs, self.ASIC_NEXT_HOP_GRP_MEMBERS)

        global switch_mac

        if switch_mac is None:
            switch_mac = get_switch_mac(dvs)

    def check_vxlan_tunnel_map_entry_delete(self, dvs, tunnel_name, vidlist, vnilist):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_MAP_ENTRY)

        for x in range(1):
            status, fvs = tbl.get(self.mapentry_map[tunnel_name + vidlist[x]])
            assert status == False, "SIP Tunnel Map entry not deleted"

    def check_vxlan_tunnel_vlan_map_entry(self, dvs, tunnel_name, vidlist, vnidlist):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_MAP_ENTRY)

        expected_attributes_1 = {
        'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID',
        'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': self.tunnel_map_map[tunnel_name][0],
        'SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE': vidlist[0],
        'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY': vnidlist[0],
        }

        for x in range(1):
            expected_attributes_1['SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE'] = vidlist[x]
            expected_attributes_1['SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY'] = vnidlist[x]
            ret = get_key_with_attr(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, expected_attributes_1)
            assert len(ret) > 0, "SIP TunnelMap entry not created"
            assert len(ret) == 1, "More than 1 SIP TunnMapEntry created"
            self.mapentry_map[tunnel_name + vidlist[x]] = ret[0]
   

    def check_vxlan_sip_tunnel_delete(self, dvs, tunnel_name):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_TERM_ENTRY)
        status, fvs = tbl.get(self.tunnelterm[tunnel_name])
        assert status == False, "SIP Tunnel Term entry not deleted"

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_TABLE)
        status, fvs = tbl.get(self.tunnel[tunnel_name])
        assert status == False, "SIP Tunnel entry not deleted"

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_MAP)
        status, fvs = tbl.get(self.tunnel_map_map[tunnel_name][0])
        assert status == False, "SIP Tunnel mapper0 not deleted"
        status, fvs = tbl.get(self.tunnel_map_map[tunnel_name][1])
        assert status == False, "SIP Tunnel mapper1 not deleted"
        status, fvs = tbl.get(self.tunnel_map_map[tunnel_name][2])
        assert status == False, "SIP Tunnel mapper2 not deleted"
        status, fvs = tbl.get(self.tunnel_map_map[tunnel_name][3])
        assert status == False, "SIP Tunnel mapper3 not deleted"

    def check_vxlan_sip_tunnel(self, dvs, tunnel_name, src_ip, vidlist, vnidlist):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tunnel_map_id  = get_created_entries(asic_db, self.ASIC_TUNNEL_MAP, self.tunnel_map_ids, 4)
        tunnel_id      = get_created_entry(asic_db, self.ASIC_TUNNEL_TABLE, self.tunnel_ids)
        tunnel_term_id = get_created_entry(asic_db, self.ASIC_TUNNEL_TERM_ENTRY, self.tunnel_term_ids)
        tunnel_map_entry_id = get_created_entries(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, self.tunnel_map_entry_ids, 3)

        # check that the vxlan tunnel termination are there
        assert how_many_entries_exist(asic_db, self.ASIC_TUNNEL_MAP) == (len(self.tunnel_map_ids) + 4), "The TUNNEL_MAP wasn't created"
        assert how_many_entries_exist(asic_db, self.ASIC_TUNNEL_MAP_ENTRY) == (len(self.tunnel_map_entry_ids) + 3), "The TUNNEL_MAP_ENTRY is created"
        assert how_many_entries_exist(asic_db, self.ASIC_TUNNEL_TABLE) == (len(self.tunnel_ids) + 1), "The TUNNEL wasn't created"
        assert how_many_entries_exist(asic_db, self.ASIC_TUNNEL_TERM_ENTRY) == (len(self.tunnel_term_ids) + 1), "The TUNNEL_TERM_TABLE_ENTRY wasm't created"

        check_object(asic_db, self.ASIC_TUNNEL_MAP, tunnel_map_id[2],
                        {
                            'SAI_TUNNEL_MAP_ATTR_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID',
                        }
                )

        check_object(asic_db, self.ASIC_TUNNEL_MAP, tunnel_map_id[3],
                        {
                            'SAI_TUNNEL_MAP_ATTR_TYPE': 'SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI',
                        }
                )

        decapstr = '2:' + tunnel_map_id[0] + ',' + tunnel_map_id[2]
        encapstr = '2:' + tunnel_map_id[1] + ',' + tunnel_map_id[3]

        check_object(asic_db, self.ASIC_TUNNEL_TABLE, tunnel_id,
                    {
                        'SAI_TUNNEL_ATTR_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
                        'SAI_TUNNEL_ATTR_DECAP_MAPPERS': decapstr,
                        'SAI_TUNNEL_ATTR_ENCAP_MAPPERS': encapstr,
                        'SAI_TUNNEL_ATTR_ENCAP_SRC_IP': src_ip,
                    }
                )

        expected_attributes = {
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE': 'SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP',
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP': src_ip,
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID': tunnel_id,
        }

        check_object(asic_db, self.ASIC_TUNNEL_TERM_ENTRY, tunnel_term_id, expected_attributes)

        expected_attributes_1 = {
        'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID',
        'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': tunnel_map_id[0],
        'SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE': vidlist[0],
        'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY': vnidlist[0],
        }

        for x in range(1):
            expected_attributes_1['SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE'] = vidlist[x]
            expected_attributes_1['SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY'] = vnidlist[x]
            check_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, tunnel_map_entry_id[x], expected_attributes_1)

        self.tunnel_map_ids.update(tunnel_map_id)
        self.tunnel_ids.add(tunnel_id)
        self.tunnel_term_ids.add(tunnel_term_id)
        self.tunnel_map_map[tunnel_name] = tunnel_map_id
        self.tunnel[tunnel_name] = tunnel_id
        self.tunnelterm[tunnel_name] = tunnel_term_id

    def check_vxlan_dip_tunnel_delete(self, dvs, dip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(state_db, 'VXLAN_TUNNEL_TABLE')
        status, fvs = tbl.get(self.diptunstate_map[dip])
        assert status == False, "State Table entry not deleted"

        #tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_TERM_ENTRY)
        #status, fvs = tbl.get(self.diptunterm_map[dip])
        #assert status == False, "Tunnel Term entry not deleted"

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_TABLE)
        status, fvs = tbl.get(self.diptunnel_map[dip])
        assert status == False, "Tunnel entry not deleted"

        tbl = swsscommon.Table(asic_db, self.ASIC_BRIDGE_PORT)
        status, fvs = tbl.get(self.bridgeport_map[dip])
        assert status == False, "Tunnel bridgeport entry not deleted"

    def check_vxlan_dip_tunnel(self, dvs, vtep_name, src_ip, dip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        expected_state_attributes = {
            'src_ip': src_ip,
            'dst_ip': dip,
            'tnl_src': 'EVPN',
        }

        ret = get_key_with_attr(state_db, 'VXLAN_TUNNEL_TABLE', expected_state_attributes)
        assert len(ret) > 0, "Tunnel Statetable entry not created"
        assert len(ret) == 1, "More than 1 Tunn statetable entry created"
        self.diptunstate_map[dip] = ret[0]

#        expected_term_attributes = {
#            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE': 'SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2P',
#            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP': src_ip,
#            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP': dip,
#            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
#        }
#        ret = get_key_with_attr(asic_db, self.ASIC_TUNNEL_TERM_ENTRY, expected_term_attributes)
#        assert len(ret) > 0, "Tunnel Term entry not created"
#        assert len(ret) == 1, "More than 1 tunnel term entry created"
#        self.diptunterm_map[dip] = ret[0]

#        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_TERM_ENTRY)
#        status, fvs = tbl.get(ret[0])
#        assert status, "Got an error when get a key"
#        for name, value in fvs:
#            if name == 'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID':
#               tunnel_id = value
#               break

#       self.diptunnel_map[dip] = tunnel_id

        tunnel_map_id = self.tunnel_map_map[vtep_name]

        decapstr = '2:' + tunnel_map_id[0] + ',' + tunnel_map_id[2]
        encapstr = '2:' + tunnel_map_id[1] + ',' + tunnel_map_id[3]

#        print decapstr
#        print encapstr

        expected_tun_attributes = {
                        'SAI_TUNNEL_ATTR_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
                        'SAI_TUNNEL_ATTR_PEER_MODE': 'SAI_TUNNEL_PEER_MODE_P2P',
                        'SAI_TUNNEL_ATTR_DECAP_MAPPERS': decapstr,
                        'SAI_TUNNEL_ATTR_ENCAP_MAPPERS': encapstr,
                        'SAI_TUNNEL_ATTR_ENCAP_SRC_IP': src_ip,
                        'SAI_TUNNEL_ATTR_ENCAP_DST_IP': dip,
                    }
           

#        check_object(asic_db, self.ASIC_TUNNEL_TABLE, tunnel_id, expected_tun_attributes)

        ret = get_key_with_attr(asic_db, self.ASIC_TUNNEL_TABLE, expected_tun_attributes)
        assert len(ret) > 0, "Tunnel entry not created"
        assert len(ret) == 1, "More than 1 tunnel entry created"

#       tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_ENTRY)
#       status, fvs = tbl.get(ret[0])
#       assert status, "Got an error when get a key"
#       for name, value in fvs:
#           if name == 'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID':
#              tunnel_id = value
#              break

        self.diptunnel_map[dip] = ret[0]
        tunnel_id = ret[0]

        expected_bridgeport_attributes = {
        'SAI_BRIDGE_PORT_ATTR_TYPE': 'SAI_BRIDGE_PORT_TYPE_TUNNEL',
        'SAI_BRIDGE_PORT_ATTR_TUNNEL_ID': tunnel_id,
        'SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE': 'SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DISABLE',
        'SAI_BRIDGE_PORT_ATTR_ADMIN_STATE': 'true',
        }

        ret = get_key_with_attr(asic_db, self.ASIC_BRIDGE_PORT, expected_bridgeport_attributes)
        assert len(ret) > 0, "Bridgeport entry not created"
        assert len(ret) == 1, "More than 1 bridgeport entry created"
        
        self.bridgeport_map[dip] = ret[0]

    def check_vlan_extension_delete(self, dvs, vlan_name, dip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER')
        status, fvs = tbl.get(self.vlan_member_map[dip+vlan_name])
        assert status == False, "VLAN Member entry not deleted"

    def check_vlan_extension(self, dvs, vlan_name, dip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        expected_vlan_attributes = {
            'SAI_VLAN_ATTR_VLAN_ID': vlan_name,
        }
        ret = get_key_with_attr(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_VLAN', expected_vlan_attributes)
        assert len(ret) > 0, "VLAN entry not created"
        assert len(ret) == 1, "More than 1 VLAN entry created"

        self.vlan_id_map[vlan_name] = ret[0]

        expected_vlan_member_attributes = {
            'SAI_VLAN_MEMBER_ATTR_VLAN_ID': self.vlan_id_map[vlan_name],
            'SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID': self.bridgeport_map[dip],
        }
        ret = get_key_with_attr(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER', expected_vlan_member_attributes)
        assert len(ret) > 0, "VLAN Member not created"
        assert len(ret) == 1, "More than 1 VLAN member created"
        self.vlan_member_map[dip+vlan_name] = ret[0]

    def check_vxlan_tunnel_vrf_map_entry(self, dvs, tunnel_name, vrf_name, vni_id):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

        if (self.tunnel_map_map.get(tunnel_name) is None):
            tunnel_map_id = get_created_entries(asic_db, self.ASIC_TUNNEL_MAP, self.tunnel_map_ids, 4)
        else:
            tunnel_map_id = self.tunnel_map_map[tunnel_name]

        tunnel_map_entry_id = get_created_entries(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, self.tunnel_map_entry_ids, 3)

        # check that the vxlan tunnel termination are there
        assert how_many_entries_exist(asic_db, self.ASIC_TUNNEL_MAP_ENTRY) == (len(self.tunnel_map_entry_ids) + 3), "The TUNNEL_MAP_ENTRY is created too early"

        check_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, tunnel_map_entry_id[1],
            {
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI',
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': tunnel_map_id[3],
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_KEY': self.vr_map[vrf_name].get('ing'),
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE': vni_id,
            }
        )

        self.tunnel_map_vrf_entry_ids.update(tunnel_map_entry_id[1])

        check_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, tunnel_map_entry_id[2],
            {
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID',
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': tunnel_map_id[2],
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY': vni_id,
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_VALUE': self.vr_map[vrf_name].get('egr'),
            }
        )

        self.tunnel_map_vrf_entry_ids.update(tunnel_map_entry_id[2])
        self.tunnel_map_entry_ids.update(tunnel_map_entry_id)

    def check_vxlan_tunnel_vrf_map_entry_remove(self, dvs, tunnel_name, vrf_name, vni_id):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tunnel_map_entry_id = get_deleted_entries(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, self.tunnel_map_entry_ids, 2)
        check_deleted_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, tunnel_map_entry_id[0])
        check_deleted_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, tunnel_map_entry_id[1])
        for vrf_map_id in self.tunnel_map_vrf_entry_ids:
            check_deleted_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, vrf_map_id)

    def check_router_interface(self, dvs, name, vlan_oid, route_count):
        # Check RIF in ingress VRF
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        global switch_mac

        expected_attr = {
            "SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID": self.vr_map[name].get('ing'),
            "SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS": switch_mac,
        }

        if vlan_oid:
            expected_attr.update({'SAI_ROUTER_INTERFACE_ATTR_TYPE': 'SAI_ROUTER_INTERFACE_TYPE_VLAN'})
            expected_attr.update({'SAI_ROUTER_INTERFACE_ATTR_VLAN_ID': vlan_oid})
        else:
            expected_attr.update({'SAI_ROUTER_INTERFACE_ATTR_TYPE': 'SAI_ROUTER_INTERFACE_TYPE_PORT'})

        new_rif = get_created_entry(asic_db, self.ASIC_RIF_TABLE, self.rifs)
        check_object(asic_db, self.ASIC_RIF_TABLE, new_rif, expected_attr)

        #IP2ME route will be created with every router interface
        new_route = get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, route_count)

        self.rifs.add(new_rif)
        self.routes.update(new_route)

    def check_del_router_interface(self, dvs, name):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        old_rif = get_deleted_entries(asic_db, self.ASIC_RIF_TABLE, self.rifs, 1)
        check_deleted_object(asic_db, self.ASIC_RIF_TABLE, old_rif[0])

        self.rifs.remove(old_rif[0])

    def vrf_route_ids(self, dvs, vrf_name):
        vr_set = set()

        vr_set.add(self.vr_map[vrf_name].get('ing'))
        return vr_set

    def check_vrf_routes(self, dvs, prefix, vrf_name, endpoint, tunnel, mac="", vni=0, no_update=0):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        vr_ids = self.vrf_route_ids(dvs, vrf_name)
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
        if no_update:
           count = 0
        new_route = get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, count)

        if count:
            self.route_id[vrf_name + ":" + prefix] = new_route

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
            found_route = False
            if rt_key['dest'] == prefix:
                found_route = True

            assert found_route

        if count:
            assert asic_vrs == vr_ids

            self.routes.update(new_route)
    
        return True

    def check_vrf_routes_ecmp(self, dvs, prefix, vrf_name, tunnel, nh_count, no_update=0):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        vr_ids = self.vrf_route_ids(dvs, vrf_name)
        count = len(vr_ids)

        new_nhg = get_created_entry(asic_db, self.ASIC_NEXT_HOP_GRP, self.nhop_grp)
        self.nh_grp_id.add(new_nhg)

        if no_update:
           count = 0

        new_route = get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, count)
        if count:
            self.route_id[vrf_name + ":" + prefix] = new_route

        #Check if the route is in expected VRF
        asic_vrs = set()
        for idx in range(count):
            check_object(asic_db, self.ASIC_ROUTE_ENTRY, new_route[idx],
                        {
                            "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID": new_nhg,
                        }
                    )
            rt_key = json.loads(new_route[idx])
            asic_vrs.add(rt_key['vr'])

            found_route = False
            if rt_key['dest'] == prefix:
                found_route = True

            assert found_route

        if count:
            assert asic_vrs == vr_ids

            self.routes.update(new_route)

        new_nhg_members = get_created_entries(asic_db, self.ASIC_NEXT_HOP_GRP_MEMBERS, self.nhop_grp_members, nh_count)

        for idx in range(nh_count):
            self.nh_grp_member_id.add(new_nhg_members[idx])
            check_object(asic_db, self.ASIC_NEXT_HOP_GRP_MEMBERS, new_nhg_members[idx],
                        {
                            "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID": new_nhg,
                        }
                    )

        nhid_list = list()

        nhg_member_tbl = swsscommon.Table(asic_db, self.ASIC_NEXT_HOP_GRP_MEMBERS)

        for k in new_nhg_members:
            (status, fvs) = nhg_member_tbl.get(k)
            assert status

            for v in fvs:
                if v[0] == "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID":
                    nhid = v[1]
                    nhid_list.append(nhid)

        return nhid_list

    def check_add_tunnel_nexthop(self, dvs, nh_id, endpoint, tunnel, mac, vni):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        # Check routes in ingress VRF
        expected_attr = {
                        "SAI_NEXT_HOP_ATTR_TYPE": "SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP",
                        "SAI_NEXT_HOP_ATTR_IP": endpoint,
                        "SAI_NEXT_HOP_ATTR_TUNNEL_ID": self.tunnel[tunnel],
                        "SAI_NEXT_HOP_ATTR_TUNNEL_VNI": vni,
                        "SAI_NEXT_HOP_ATTR_TUNNEL_MAC": mac,
                    }

        check_object(asic_db, self.ASIC_NEXT_HOP, nh_id, expected_attr)

    def check_del_tunnel_nexthop(self, dvs, vrf_name, endpoint, tunnel, mac="", vni=0):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        del_nh_ids = get_deleted_entries(asic_db, self.ASIC_NEXT_HOP, self.nhops, 1)
        check_deleted_object(asic_db, self.ASIC_NEXT_HOP, del_nh_ids[0])
        check_deleted_object(asic_db, self.ASIC_NEXT_HOP, self.nh_ids[endpoint])
        assert del_nh_ids[0] == self.nh_ids[endpoint]
        self.nh_ids.pop(endpoint)
        return True

    def check_vrf_routes_ecmp_nexthop_grp_del(self, dvs, nh_count):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        nh_grp_id_len = len(self.nh_grp_id)
        assert nh_grp_id_len == 1

        for nh_grp_id in self.nh_grp_id:
            check_deleted_object(asic_db, self.ASIC_NEXT_HOP_GRP, nh_grp_id)
        self.nh_grp_id.clear()

        nh_grp_member_id_len = len(self.nh_grp_member_id)
        assert nh_grp_member_id_len == nh_count

        for nh_grp_member_id in self.nh_grp_member_id:
            check_deleted_object(asic_db, self.ASIC_NEXT_HOP_GRP_MEMBERS, nh_grp_member_id)

        self.nh_grp_member_id.clear()

    def check_del_vrf_routes(self, dvs, prefix, vrf_name):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        del_route = get_deleted_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, 1)

        for idx in range(1):
            rt_key = json.loads(del_route[idx])
            found_route = False
            if rt_key['dest'] == prefix:
                found_route = True

            assert found_route

        check_deleted_object(asic_db, self.ASIC_ROUTE_ENTRY, self.route_id[vrf_name + ":" + prefix])
        self.route_id.clear()

        return True

    def create_vrf(self, dvs, vrf_name):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        initial_entries = set(tbl.getKeys())

        attrs = [
            ("fallback", "false"),
            ("vni", "0"),
        ]
        tbl = swsscommon.Table(conf_db, "VRF")
        fvs = swsscommon.FieldValuePairs(attrs)
        tbl.set(vrf_name, fvs)
        time.sleep(2)

        tbl = swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        current_entries = set(tbl.getKeys())
        assert len(current_entries - initial_entries) == 1

        new_vr_ids  = get_created_entries(asic_db, self.ASIC_VRF_TABLE, self.vnet_vr_ids, 1)
        self.vnet_vr_ids.update(new_vr_ids)
        self.vr_map[vrf_name] = { 'ing':new_vr_ids[0], 'egr':new_vr_ids[0]}

        return list(current_entries - initial_entries)[0]

    def remove_vrf(self, dvs, vrf_name):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        tbl = swsscommon.Table(conf_db, "VRF")
        tbl._del(vrf_name)
        time.sleep(2)


    def is_vrf_attributes_correct(self, db, table, key, expected_attributes):
        tbl =  swsscommon.Table(db, table)
        keys = set(tbl.getKeys())
        assert key in keys, "The created key wasn't found"

        status, fvs = tbl.get(key)
        assert status, "Got an error when get a key"

        # filter the fake 'NULL' attribute out
        fvs = filter(lambda x : x != ('NULL', 'NULL'), fvs)

        attr_keys = {entry[0] for entry in fvs}
        assert attr_keys == set(expected_attributes.keys())

        for name, value in fvs:
            assert expected_attributes[name] == value, "Wrong value %s for the attribute %s = %s" % \
                                  (value, name, expected_attributes[name])


class TestL3Vxlan(object):

    def get_vxlan_obj(self):
        return VxlanTunnel()

    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

#    Test 1 - Create and Delete SIP Tunnel and VRF VNI Map entries
#    @pytest.mark.skip(reason="Starting Route Orch, VRF Orch to be merged")
    @pytest.mark.dev_sanity
    def test_sip_tunnel_vrf_vni_map(self, dvs, testlog):
        vxlan_obj = self.get_vxlan_obj()

        self.setup_db(dvs)
        tunnel_name = 'tunnel_2'
        map_name = 'map_1000_100'
        vrf_map_name = 'evpn_map_1000_Vrf-RED'

        vxlan_obj.fetch_exist_entries(dvs)

        print "\n\nTesting Create and Delete SIP Tunnel and VRF VNI Map entries"
        print "\tCreate SIP Tunnel"
        create_vlan1(dvs,"Vlan100")
        create_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')
        create_evpn_nvo(dvs, 'nvo1', tunnel_name)
        print "\tPass"

        print "\tCreate Vlan-VNI map and VRF-VNI map"
        create_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')

        vxlan_obj.create_vrf(dvs, "Vrf-RED")
        create_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED', '1000')
        print "\tPass"

        #vlanlist = ['100', '101', '102']
        #vnilist = ['1000', '1001', '1002']

        print "\tTesting VRF-VNI map in APP DB"
        vlanlist = ['100']
        vnilist = ['1000']

        exp_attrs = [
                ("vni", "1000"),
        ]
        exp_attr = {}
        for an in xrange(len(exp_attrs)):
            exp_attr[exp_attrs[an][0]] = exp_attrs[an][1]

        check_object(self.pdb, "VRF_TABLE", 'Vrf-RED', exp_attr)

        exp_attrs1 = [
                ("vni", "1000"),
                ("vlan", "Vlan100"),
        ]
        exp_attr1 = {}
        for an in xrange(len(exp_attrs1)):
            exp_attr1[exp_attrs1[an][0]] = exp_attrs1[an][1]

        check_object(self.pdb, "VXLAN_VRF_TABLE", "%s:%s" % (tunnel_name, vrf_map_name), exp_attr1)
        print "\tPass"

        print "\tTesting SIP Tunnel Creation"
        vxlan_obj.check_vxlan_sip_tunnel(dvs, tunnel_name, '6.6.6.6', vlanlist, vnilist)
        print "\tPass"

        print "\tTesting Tunnel Vlan VNI Map Entry"
        vxlan_obj.check_vxlan_tunnel_vlan_map_entry(dvs, tunnel_name, vlanlist, vnilist)
        print "\tPass"

        print "\tTesting Tunnel VRF VNI Map Entry"
        vxlan_obj.check_vxlan_tunnel_vrf_map_entry(dvs, tunnel_name, 'Vrf-RED', '1000')
        print "\tPass"

        print "\tTesting Tunnel VRF VNI Map Entry removal"
        remove_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED')
        vxlan_obj.remove_vrf(dvs, "Vrf-RED")
        vxlan_obj.check_vxlan_tunnel_vrf_map_entry_remove(dvs, tunnel_name, 'Vrf-RED', '1000')
        print "\tPass"

        print "\tTesting Tunnel Vlan VNI Map entry removal"
        remove_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        vxlan_obj.check_vxlan_tunnel_map_entry_delete(dvs, tunnel_name, vlanlist, vnilist)
        print "\tPass"

        print "\tTesting SIP Tunnel Deletion"
        remove_vxlan_tunnel(dvs, tunnel_name)
        remove_evpn_nvo(dvs, 'nvo1')
        vxlan_obj.check_vxlan_sip_tunnel_delete(dvs, tunnel_name)
        remove_vlan(dvs, "100")
        print "\tPass\n"


#    Test 2 - Create and Delete DIP Tunnel on adding and removing prefix route
#    @pytest.mark.skip(reason="Starting Route Orch, VRF Orch to be merged")
    @pytest.mark.dev_sanity
    def test_prefix_route_create_dip_tunnel(self, dvs, testlog):
        vxlan_obj = self.get_vxlan_obj()

        self.setup_db(dvs)
        tunnel_name = 'tunnel_2'
        map_name = 'map_1000_100'
        vrf_map_name = 'evpn_map_1000_Vrf-RED'
        vxlan_obj.fetch_exist_entries(dvs)

        print "\n\nTesting Create and Delete DIP Tunnel on adding and removing prefix route"
        print "\tCreate SIP Tunnel"
        vlan_ids = get_exist_entries(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        vlan_oid = create_vlan(dvs,"Vlan100", vlan_ids)
        create_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')
        create_evpn_nvo(dvs, 'nvo1', tunnel_name)
        print "\tPass"

        print "\tCreate Vlan-VNI map and VRF-VNI map"
        create_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        print "\tPass"

        print "\tTesting VRF-VNI map in APP DB"
        vxlan_obj.create_vrf(dvs, "Vrf-RED")
        create_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED', '1000')

        vlanlist = ['100']
        vnilist = ['1000']

        exp_attrs = [
                ("vni", "1000"),
        ]
        exp_attr = {}
        for an in xrange(len(exp_attrs)):
            exp_attr[exp_attrs[an][0]] = exp_attrs[an][1]

        check_object(self.pdb, "VRF_TABLE", 'Vrf-RED', exp_attr)

        exp_attrs1 = [
                ("vni", "1000"),
                ("vlan", "Vlan100"),
        ]
        exp_attr1 = {}
        for an in xrange(len(exp_attrs1)):
            exp_attr1[exp_attrs1[an][0]] = exp_attrs1[an][1]

        check_object(self.pdb, "VXLAN_VRF_TABLE", "%s:%s" % (tunnel_name, vrf_map_name), exp_attr1)
        print "\tPass"

        print "\tTesting SIP Tunnel Creation"
        vxlan_obj.check_vxlan_sip_tunnel(dvs, tunnel_name, '6.6.6.6', vlanlist, vnilist)
        print "\tPass"

        print "\tTesting Tunnel Vlan Map Entry"
        vxlan_obj.check_vxlan_tunnel_vlan_map_entry(dvs, tunnel_name, vlanlist, vnilist)
        print "\tPass"

        print "\tTesting Tunnel Vrf Map Entry"
        vxlan_obj.check_vxlan_tunnel_vrf_map_entry(dvs, tunnel_name, 'Vrf-RED', '1000')
        print "\tPass"

        print "\tTesting VLAN 100 interface creation"
        create_vlan_interface(dvs, "Vlan100", "Ethernet24", "Vrf-RED", "100.100.3.1/24")
        vxlan_obj.check_router_interface(dvs, 'Vrf-RED', vlan_oid, 2)
        print "\tPass"

        print "\tTest VRF IPv4 Route with Tunnel Nexthop Add"
        vxlan_obj.fetch_exist_entries(dvs)
        create_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '7.7.7.7', "Vlan100", "00:11:11:11:11:11", '1000')
        vxlan_obj.check_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        print "\tPass"

        print "\tTesting DIP tunnel 7.7.7.7 creation"
        vxlan_obj.check_vxlan_dip_tunnel(dvs, tunnel_name, '6.6.6.6', '7.7.7.7')
        print "\tPass"

        print "\tTest VRF IPv4 Route with Tunnel Nexthop Delete"
        delete_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED')
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        vxlan_obj.check_del_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED')
        print "\tPass"

        print "\tTesting DIP tunnel 7.7.7.7 deletion"
        vxlan_obj.check_vxlan_dip_tunnel_delete(dvs, '7.7.7.7')
        print "\tPass"

        print "\tTesting Tunnel Vrf Map Entry removal"
        remove_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED')
        vxlan_obj.check_vxlan_tunnel_vrf_map_entry_remove(dvs, tunnel_name, 'Vrf-RED', '1000')
        print "\tPass"

        print "\tTesting Vlan 100 interface delete"
        delete_vlan_interface(dvs, "Vlan100", "100.100.3.1/24")
        vxlan_obj.check_del_router_interface(dvs, "Vlan100")
        print "\tPass"

        print "\tTesting Tunnel Map entry removal"
        remove_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        vxlan_obj.check_vxlan_tunnel_map_entry_delete(dvs, tunnel_name, vlanlist, vnilist)
        print "\tPass"

        print "\tTesting SIP Tunnel Deletion"
        remove_vxlan_tunnel(dvs, tunnel_name)
        remove_evpn_nvo(dvs, 'nvo1')
        vxlan_obj.check_vxlan_sip_tunnel_delete(dvs, tunnel_name)
        vxlan_obj.remove_vrf(dvs, "Vrf-RED")
        remove_vlan_member(dvs, "100", "Ethernet24")
        remove_vlan(dvs, "100")
        print "\tPass\n"


#    Test 3 - Create and Delete DIP Tunnel and Test IPv4 route and overlay nexthop add and delete
#    @pytest.mark.skip(reason="Starting Route Orch, VRF Orch to be merged")
    @pytest.mark.dev_sanity
    def test_dip_tunnel_ipv4_routes(self, dvs, testlog):
        vxlan_obj = self.get_vxlan_obj()

        self.setup_db(dvs)
        tunnel_name = 'tunnel_2'
        map_name = 'map_1000_100'
        vrf_map_name = 'evpn_map_1000_Vrf-RED'
        vxlan_obj.fetch_exist_entries(dvs)

        print "\n\nTesting IPv4 Route and Overlay Nexthop Add and Delete"
        print "\tCreate SIP Tunnel"
        create_vlan1(dvs,"Vlan100")
        create_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')
        create_evpn_nvo(dvs, 'nvo1', tunnel_name)
        print "\tPass"

        print "\tCreate Vlan-VNI map and VRF-VNI map"
        create_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')

        vxlan_obj.create_vrf(dvs, "Vrf-RED")
        create_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED', '1000')
        print "\tPass"

        print "\tTesting VRF-VNI map in APP DB"
        vlanlist = ['100']
        vnilist = ['1000']

        exp_attrs = [
                ("vni", "1000"),
        ]
        exp_attr = {}
        for an in xrange(len(exp_attrs)):
            exp_attr[exp_attrs[an][0]] = exp_attrs[an][1]

        check_object(self.pdb, "VRF_TABLE", 'Vrf-RED', exp_attr)

        exp_attrs1 = [
                ("vni", "1000"),
                ("vlan", "Vlan100"),
        ]
        exp_attr1 = {}
        for an in xrange(len(exp_attrs1)):
            exp_attr1[exp_attrs1[an][0]] = exp_attrs1[an][1]

        check_object(self.pdb, "VXLAN_VRF_TABLE", "%s:%s" % (tunnel_name, vrf_map_name), exp_attr1)
        print "\tPass"

        print "\tTesting SIP Tunnel Creation"
        vxlan_obj.check_vxlan_sip_tunnel(dvs, tunnel_name, '6.6.6.6', vlanlist, vnilist)
        print "\tPass"

        print "\tTesting Tunnel Vlan Map Entry"
        vxlan_obj.check_vxlan_tunnel_vlan_map_entry(dvs, tunnel_name, vlanlist, vnilist)
        print "\tPass"

        print "\tTesting Tunnel Vrf Map Entry"
        vxlan_obj.check_vxlan_tunnel_vrf_map_entry(dvs, tunnel_name, 'Vrf-RED', '1000')
        print "\tPass"

        print "\tTesting First DIP tunnel creation to 7.7.7.7"
        create_evpn_remote_vni(dvs, 'Vlan100', '7.7.7.7', '1000')
        vxlan_obj.check_vxlan_dip_tunnel(dvs, tunnel_name, '6.6.6.6', '7.7.7.7')
        print "\tPass"

        print "\tTesting VLAN 100 extension"
        vxlan_obj.check_vlan_extension(dvs, '100', '7.7.7.7')
        print "\tPass"

        print "\tTesting Second DIP tunnel creation to 8.8.8.8"
        create_evpn_remote_vni(dvs, 'Vlan100', '8.8.8.8', '1000')
        vxlan_obj.check_vxlan_dip_tunnel(dvs, tunnel_name, '6.6.6.6', '8.8.8.8')
        print "\tPass"

        print "\tTesting VLAN 100 extension to 8.8.8.8 and 7.7.7.7"
        vxlan_obj.check_vlan_extension(dvs, '100', '8.8.8.8')
        vxlan_obj.check_vlan_extension(dvs, '100', '7.7.7.7')
        print "\tPass"

        print "\tTesting VLAN 100 interface creation"
        create_vlan_interface(dvs, "Vlan100", "Ethernet24", "Vrf-RED", "100.100.3.1/24")
        vxlan_obj.check_router_interface(dvs, 'Vrf-RED', vxlan_obj.vlan_id_map['100'], 2)
        print "\tPass"

        print "\tTest VRF IPv4 Route with Tunnel Nexthop 7.7.7.7 Add"
        vxlan_obj.fetch_exist_entries(dvs)
        create_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '7.7.7.7', "Vlan100", "00:11:11:11:11:11", '1000')
        vxlan_obj.check_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        print "\tPass"

        print "\tTest VRF IPv4 Route with Tunnel Nexthop 7.7.7.7 Delete"
        delete_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED')
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        vxlan_obj.check_del_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED')
        print "\tPass"

        print "\n\nTesting IPv4 Route and Overlay Nexthop Update"
        print "\tTest VRF IPv4 Route with Tunnel Nexthop 7.7.7.7 Add"
        vxlan_obj.fetch_exist_entries(dvs)
        create_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '7.7.7.7', "Vlan100", "00:11:11:11:11:11", '1000')
        vxlan_obj.check_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        print "\tPass"

        print "\tTest Tunnel Nexthop change from 7.7.7.7 to 8.8.8.8"
        create_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '8.8.8.8', "Vlan100", "00:22:22:22:22:22", '1000')
        vxlan_obj.check_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '8.8.8.8', tunnel_name, "00:22:22:22:22:22", '1000', 1)
        print "\tPass"

        print "\tTest Previous Tunnel Nexthop 7.7.7.7 is removed"
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        print "\tPass"

        print "\tTest VRF IPv4 Route and Tunnel Nexthop 8.8.8.8 Delete"
        vxlan_obj.fetch_exist_entries(dvs)
        delete_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED')
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '8.8.8.8', tunnel_name, "00:22:22:22:22:22", '1000')
        vxlan_obj.check_del_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED')
        print "\tPass"

        print "\n\nTest VRF IPv4 Route with ECMP Tunnel Nexthop Add and Delete"
        vxlan_obj.fetch_exist_entries(dvs)

        ecmp_nexthop_attr = [
            ("nexthop", "7.7.7.7,8.8.8.8"),
            ("ifname", "Vlan100,Vlan100"),
            ("vni_label", "1000,1000"),
            ("router_mac", "00:11:11:11:11:11,00:22:22:22:22:22"),
        ]

        print "\tTest VRF IPv4 Route with ECMP Tunnel Nexthop [7.7.7.7 , 8.8.8.8] Add"
        create_vrf_routes_ecmp(dvs, "80.80.1.0/24", 'Vrf-RED', ecmp_nexthop_attr)

        nh_count = 2
        ecmp_nhid_list = vxlan_obj.check_vrf_routes_ecmp(dvs, "80.80.1.0/24", 'Vrf-RED', tunnel_name, nh_count)
        assert nh_count == len(ecmp_nhid_list)
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[0], '7.7.7.7', tunnel_name, '00:11:11:11:11:11', '1000')
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[1], '8.8.8.8', tunnel_name, '00:22:22:22:22:22', '1000')
        print "\tPass"

        print "\tTest VRF IPv4 Route with ECMP Tunnel Nexthop [7.7.7.7 , 8.8.8.8] Delete"
        vxlan_obj.fetch_exist_entries(dvs)
        delete_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED')
        check_deleted_object(self.adb, vxlan_obj.ASIC_NEXT_HOP, ecmp_nhid_list[0])
        check_deleted_object(self.adb, vxlan_obj.ASIC_NEXT_HOP, ecmp_nhid_list[1])
        
        vxlan_obj.check_vrf_routes_ecmp_nexthop_grp_del(dvs, 2)
        vxlan_obj.check_del_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED')
        print "\tPass"

        print "\n\nTest VRF IPv4 Route with Tunnel Nexthop update from non-ECMP to ECMP"
        print "\tTest VRF IPv4 Route with Tunnel Nexthop 7.7.7.7 Add"
        vxlan_obj.fetch_exist_entries(dvs)
        create_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '7.7.7.7', "Vlan100", "00:11:11:11:11:11", '1000')
        vxlan_obj.check_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        print "\tPass"

        ecmp_nexthop_attr = [
            ("nexthop", "7.7.7.7,8.8.8.8"),
            ("ifname", "Vlan100,Vlan100"),
            ("vni_label", "1000,1000"),
            ("router_mac", "00:11:11:11:11:11,00:22:22:22:22:22"),
        ]

        print "\tTest VRF IPv4 Route with ECMP Tunnel Nexthop [7.7.7.7 , 8.8.8.8] Udpate"
        create_vrf_routes_ecmp(dvs, "80.80.1.0/24", 'Vrf-RED', ecmp_nexthop_attr)

        nh_count = 2
        ecmp_nhid_list = vxlan_obj.check_vrf_routes_ecmp(dvs, "80.80.1.0/24", 'Vrf-RED', tunnel_name, nh_count, 1)
        assert nh_count == len(ecmp_nhid_list)
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[0], '7.7.7.7', tunnel_name, '00:11:11:11:11:11', '1000')
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[1], '8.8.8.8', tunnel_name, '00:22:22:22:22:22', '1000')
        print "\tPass"

        print "\n\nTest VRF IPv4 Route with Tunnel Nexthop update from ECMP to non-ECMP"
        print "\tTest VRF IPv4 Route with Tunnel Nexthop 8.8.8.8 Update"
        create_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '8.8.8.8', "Vlan100", "00:22:22:22:22:22", '1000')
        vxlan_obj.check_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED', '8.8.8.8', tunnel_name, "00:22:22:22:22:22", '1000', 1)
        print "\tPass"

        print "\tTest Tunnel Nexthop 7.7.7.7 is deleted"
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        print "\tPass"

        print "\tTest Tunnel Nexthop ECMP Group is deleted"
        vxlan_obj.check_vrf_routes_ecmp_nexthop_grp_del(dvs, 2)
        print "\tPass"

        print "\tTest VRF IPv4 Route with Tunnel Nexthop 8.8.8.8 Delete"
        vxlan_obj.fetch_exist_entries(dvs)
        delete_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED')

        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '8.8.8.8', tunnel_name, "00:22:22:22:22:22", '1000')
        vxlan_obj.check_del_vrf_routes(dvs, "80.80.1.0/24", 'Vrf-RED')
        print "\tPass"

        print "\n\nTest DIP and SIP Tunnel Deletion "
        print "\tTesting Tunnel Vrf VNI Map Entry removal"
        remove_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED')
        vxlan_obj.check_vxlan_tunnel_vrf_map_entry_remove(dvs, tunnel_name, 'Vrf-RED', '1000')
        print "\tPass"

        print "\tTesting LastVlan removal and DIP tunnel delete for 7.7.7.7"
        remove_evpn_remote_vni(dvs, 'Vlan100', '7.7.7.7')
        vxlan_obj.check_vlan_extension_delete(dvs, '100', '7.7.7.7')
        vxlan_obj.check_vxlan_dip_tunnel_delete(dvs, '7.7.7.7')
        print "\tPass"

        print "\tTesting LastVlan removal and DIP tunnel delete for 8.8.8.8"
        remove_evpn_remote_vni(dvs, 'Vlan100', '8.8.8.8')
        vxlan_obj.check_vlan_extension_delete(dvs, '100', '8.8.8.8')
        vxlan_obj.check_vxlan_dip_tunnel_delete(dvs, '8.8.8.8')
        print "\tPass"

        print "\tTesting Vlan 100 interface delete"
        delete_vlan_interface(dvs, "Vlan100", "100.100.3.1/24")
        vxlan_obj.check_del_router_interface(dvs, "Vlan100")
        print "\tPass"

        print "\tTesting Tunnel Map entry removal"
        remove_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        vxlan_obj.check_vxlan_tunnel_map_entry_delete(dvs, tunnel_name, vlanlist, vnilist)
        print "\tPass"

        print "\tTesting SIP Tunnel Deletion"
        remove_vxlan_tunnel(dvs, tunnel_name)
        remove_evpn_nvo(dvs, 'nvo1')
        vxlan_obj.check_vxlan_sip_tunnel_delete(dvs, tunnel_name)
        vxlan_obj.remove_vrf(dvs, "Vrf-RED")
        remove_vlan_member(dvs, "100", "Ethernet24")
        remove_vlan(dvs, "100")
        print "\tPass\n"


#    Test 4 - Create and Delete DIP Tunnel and Test IPv6 route and overlay nexthop add and delete
#    @pytest.mark.skip(reason="Starting Route Orch, VRF Orch to be merged")
    @pytest.mark.dev_sanity
    def test_dip_tunnel_ipv6_routes(self, dvs, testlog):
        vxlan_obj = self.get_vxlan_obj()

        self.setup_db(dvs)
        tunnel_name = 'tunnel_2'
        map_name = 'map_1000_100'
        vrf_map_name = 'evpn_map_1000_Vrf-RED'
        vxlan_obj.fetch_exist_entries(dvs)

        print "\n\nTesting IPv6 Route and Overlay Nexthop Add and Delete"
        print "\tCreate SIP Tunnel"
        create_vlan1(dvs,"Vlan100")
        create_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')
        create_evpn_nvo(dvs, 'nvo1', tunnel_name)
        print "\tPass"

        print "\tCreate Vlan-VNI map and VRF-VNI map"
        create_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        print "\tPass"

        print "\tTesting VRF-VNI map in APP DB"
        vxlan_obj.create_vrf(dvs, "Vrf-RED")
        create_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED', '1000')
        print "\tPass"

        vlanlist = ['100']
        vnilist = ['1000']

        exp_attrs = [
                ("vni", "1000"),
        ]
        exp_attr = {}
        for an in xrange(len(exp_attrs)):
            exp_attr[exp_attrs[an][0]] = exp_attrs[an][1]

        print "\tCheck VRF Table in APP DB"
        check_object(self.pdb, "VRF_TABLE", 'Vrf-RED', exp_attr)

        exp_attrs1 = [
                ("vni", "1000"),
                ("vlan", "Vlan100"),
        ]
        exp_attr1 = {}
        for an in xrange(len(exp_attrs1)):
            exp_attr1[exp_attrs1[an][0]] = exp_attrs1[an][1]

        check_object(self.pdb, "VXLAN_VRF_TABLE", "%s:%s" % (tunnel_name, vrf_map_name), exp_attr1)
        print "\tPass"

        print "\tTesting SIP Tunnel Creation"
        vxlan_obj.check_vxlan_sip_tunnel(dvs, tunnel_name, '6.6.6.6', vlanlist, vnilist)
        print "\tPass"

        print "\tTesting Tunnel Vlan Map Entry"
        vxlan_obj.check_vxlan_tunnel_vlan_map_entry(dvs, tunnel_name, vlanlist, vnilist)
        print "\tPass"

        print "\tTesting Tunnel Vrf Map Entry"
        vxlan_obj.check_vxlan_tunnel_vrf_map_entry(dvs, tunnel_name, 'Vrf-RED', '1000')
        print "\tPass"

        print "\tTesting First DIP tunnel creation to 7.7.7.7"
        create_evpn_remote_vni(dvs, 'Vlan100', '7.7.7.7', '1000')
        vxlan_obj.check_vxlan_dip_tunnel(dvs, tunnel_name, '6.6.6.6', '7.7.7.7')
        print "\tPass"

        print "\tTesting VLAN 100 extension"
        vxlan_obj.check_vlan_extension(dvs, '100', '7.7.7.7')
        print "\tPass"

        print "\tTesting Second DIP tunnel creation to 8.8.8.8"
        create_evpn_remote_vni(dvs, 'Vlan100', '8.8.8.8', '1000')
        vxlan_obj.check_vxlan_dip_tunnel(dvs, tunnel_name, '6.6.6.6', '8.8.8.8')
        print "\tPass"

        print "\tTesting VLAN 100 extension to 8.8.8.8 and 7.7.7.7"
        vxlan_obj.check_vlan_extension(dvs, '100', '8.8.8.8')
        vxlan_obj.check_vlan_extension(dvs, '100', '7.7.7.7')
        print "\tPass"

        vxlan_obj.fetch_exist_entries(dvs)
        print "\tTesting VLAN 100 interface creation"
        create_vlan_interface(dvs, "Vlan100", "Ethernet24", "Vrf-RED", "2001::8/64")
        vxlan_obj.check_router_interface(dvs, 'Vrf-RED', vxlan_obj.vlan_id_map['100'], 2)
        print "\tPass"

        print "\tTest VRF IPv6 Route with Tunnel Nexthop Add"
        vxlan_obj.fetch_exist_entries(dvs)
        create_vrf_routes(dvs, "2002::8/64", 'Vrf-RED', '7.7.7.7', "Vlan100", "00:11:11:11:11:11", '1000')
        vxlan_obj.check_vrf_routes(dvs, "2002::8/64", 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        print "\tPass"

        print "\tTest VRF IPv6 Route with Tunnel Nexthop Delete"
        delete_vrf_routes(dvs, "2002::8/64", 'Vrf-RED')
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        vxlan_obj.check_del_vrf_routes(dvs, "2002::8/64", 'Vrf-RED')
        print "\tPass"

        print "\n\nTesting IPv6 Route and Overlay Nexthop Update"
        print "\tTest VRF IPv6 Route with Tunnel Nexthop 7.7.7.7 Add"
        vxlan_obj.fetch_exist_entries(dvs)
        create_vrf_routes(dvs, "2002::8/64", 'Vrf-RED', '7.7.7.7', "Vlan100", "00:11:11:11:11:11", '1000')
        vxlan_obj.check_vrf_routes(dvs, "2002::8/64", 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        print "\tPass"

        print "\tTest Tunnel Nexthop change from 7.7.7.7 to 8.8.8.8"
        create_vrf_routes(dvs, "2002::8/64", 'Vrf-RED', '8.8.8.8', "Vlan100", "00:22:22:22:22:22", '1000')
        vxlan_obj.check_vrf_routes(dvs, "2002::8/64", 'Vrf-RED', '8.8.8.8', tunnel_name, "00:22:22:22:22:22", '1000', 1)
        print "\tPass"

        print "\tTest Previous Tunnel Nexthop 7.7.7.7 is removed"
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        print "\tPass"

        print "\tTest VRF IPv6 Route and Tunnel Nexthop 8.8.8.8 Delete"
        vxlan_obj.fetch_exist_entries(dvs)
        delete_vrf_routes(dvs, "2002::8/64", 'Vrf-RED')
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '8.8.8.8', tunnel_name, "00:22:22:22:22:22", '1000')
        vxlan_obj.check_del_vrf_routes(dvs, "2002::8/64", 'Vrf-RED')
        print "\tPass"

        print "\n\nTest VRF IPv6 Route with ECMP Tunnel Nexthop Add and delete"
        vxlan_obj.fetch_exist_entries(dvs)

        ecmp_nexthop_attr = [
            ("nexthop", "7.7.7.7,8.8.8.8"),
            ("ifname", "Vlan100,Vlan100"),
            ("vni_label", "1000,1000"),
            ("router_mac", "00:11:11:11:11:11,00:22:22:22:22:22"),
        ]

        print "\tTest VRF IPv6 Route with ECMP Tunnel Nexthop [7.7.7.7 , 8.8.8.8] Add"
        create_vrf_routes_ecmp(dvs, "2002::8/64", 'Vrf-RED', ecmp_nexthop_attr)

        nh_count = 2
        ecmp_nhid_list = vxlan_obj.check_vrf_routes_ecmp(dvs, "2002::8/64", 'Vrf-RED', tunnel_name, nh_count)
        assert nh_count == len(ecmp_nhid_list)
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[0], '7.7.7.7', tunnel_name, '00:11:11:11:11:11', '1000')
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[1], '8.8.8.8', tunnel_name, '00:22:22:22:22:22', '1000')
        print "\tPass"

        print "\tTest VRF IPv6 Route with ECMP Tunnel Nexthop [7.7.7.7 , 8.8.8.8] Delete"
        vxlan_obj.fetch_exist_entries(dvs)
        delete_vrf_routes(dvs, "2002::8/64", 'Vrf-RED')
        check_deleted_object(self.adb, vxlan_obj.ASIC_NEXT_HOP, ecmp_nhid_list[0])
        check_deleted_object(self.adb, vxlan_obj.ASIC_NEXT_HOP, ecmp_nhid_list[1])
        
        vxlan_obj.check_vrf_routes_ecmp_nexthop_grp_del(dvs, 2)
        vxlan_obj.check_del_vrf_routes(dvs, "2002::8/64", 'Vrf-RED')
        print "\tPass"

        print "\n\nTest VRF IPv6 Route with Tunnel Nexthop update from non-ECMP to ECMP"
        print "\tTest VRF IPv6 Route with Tunnel Nexthop 7.7.7.7 Add"
        vxlan_obj.fetch_exist_entries(dvs)
        create_vrf_routes(dvs, "2002::8/64", 'Vrf-RED', '7.7.7.7', "Vlan100", "00:11:11:11:11:11", '1000')
        vxlan_obj.check_vrf_routes(dvs, "2002::8/64", 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        print "\tPass"

        print "\tTest VRF IPv4 Route with ECMP Tunnel Nexthop [7.7.7.7 , 8.8.8.8] Udpate"
        ecmp_nexthop_attr = [
            ("nexthop", "7.7.7.7,8.8.8.8"),
            ("ifname", "Vlan100,Vlan100"),
            ("vni_label", "1000,1000"),
            ("router_mac", "00:11:11:11:11:11,00:22:22:22:22:22"),
        ]

        create_vrf_routes_ecmp(dvs, "2002::8/64", 'Vrf-RED', ecmp_nexthop_attr)

        nh_count = 2
        ecmp_nhid_list = vxlan_obj.check_vrf_routes_ecmp(dvs, "2002::8/64", 'Vrf-RED', tunnel_name, nh_count, 1)
        assert nh_count == len(ecmp_nhid_list)
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[0], '7.7.7.7', tunnel_name, '00:11:11:11:11:11', '1000')
        vxlan_obj.check_add_tunnel_nexthop(dvs, ecmp_nhid_list[1], '8.8.8.8', tunnel_name, '00:22:22:22:22:22', '1000')
        print "\tPass"

        print "\n\nTest VRF IPv6 Route with Tunnel Nexthop update from ECMP to non-ECMP"
        print "\tTest VRF IPv6 Route with Tunnel Nexthop 8.8.8.8 Update"
        create_vrf_routes(dvs, "2002::8/64", 'Vrf-RED', '8.8.8.8', "Vlan100", "00:22:22:22:22:22", '1000')
        vxlan_obj.check_vrf_routes(dvs, "2002::8/64", 'Vrf-RED', '8.8.8.8', tunnel_name, "00:22:22:22:22:22", '1000', 1)
        print "\tPass"

        print "\tTest Tunnel Nexthop 7.7.7.7 is deleted"
        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '7.7.7.7', tunnel_name, "00:11:11:11:11:11", '1000')
        print "\tPass"

        print "\tTest Tunnel Nexthop ECMP Group is deleted"
        vxlan_obj.check_vrf_routes_ecmp_nexthop_grp_del(dvs, 2)
        print "\tPass"

        print "\tTest VRF IPv6 Route with Tunnel Nexthop 8.8.8.8 Delete"
        vxlan_obj.fetch_exist_entries(dvs)
        delete_vrf_routes(dvs, "2002::8/64", 'Vrf-RED')

        vxlan_obj.check_del_tunnel_nexthop(dvs, 'Vrf-RED', '8.8.8.8', tunnel_name, "00:22:22:22:22:22", '1000')
        vxlan_obj.check_del_vrf_routes(dvs, "2002::8/64", 'Vrf-RED')
        print "\tPass"

        print "\n\nTest DIP and SIP Tunnel Deletion "
        print "\tTesting Tunnel Vrf Map Entry removal"
        remove_vxlan_vrf_tunnel_map(dvs, 'Vrf-RED')
        vxlan_obj.check_vxlan_tunnel_vrf_map_entry_remove(dvs, tunnel_name, 'Vrf-RED', '1000')
        print "\tPass"

        print "\tTesting LastVlan removal and DIP tunnel delete for 7.7.7.7"
        remove_evpn_remote_vni(dvs, 'Vlan100', '7.7.7.7')
        vxlan_obj.check_vlan_extension_delete(dvs, '100', '7.7.7.7')
        vxlan_obj.check_vxlan_dip_tunnel_delete(dvs, '7.7.7.7')
        print "\tPass"

        print "\tTesting LastVlan removal and DIP tunnel delete for 8.8.8.8"
        remove_evpn_remote_vni(dvs, 'Vlan100', '8.8.8.8')
        vxlan_obj.check_vlan_extension_delete(dvs, '100', '8.8.8.8')
        vxlan_obj.check_vxlan_dip_tunnel_delete(dvs, '8.8.8.8')
        print "\tPass"

        print "\tTesting Vlan 100 interface delete"
        delete_vlan_interface(dvs, "Vlan100", "2001::8/64")
        vxlan_obj.check_del_router_interface(dvs, "Vlan100")
        print "\tPass"

        print "\tTesting Tunnel Map entry removal"
        remove_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        vxlan_obj.check_vxlan_tunnel_map_entry_delete(dvs, tunnel_name, vlanlist, vnilist)
        print "\tPass"

        print "\tTesting SIP Tunnel Deletion"
        remove_vxlan_tunnel(dvs, tunnel_name)
        remove_evpn_nvo(dvs, 'nvo1')
        vxlan_obj.check_vxlan_sip_tunnel_delete(dvs, tunnel_name)
        vxlan_obj.remove_vrf(dvs, "Vrf-RED")
        remove_vlan_member(dvs, "100", "Ethernet24")
        remove_vlan(dvs, "100")
        print "\tPass\n"


