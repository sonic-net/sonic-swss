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
       
      
def create_evpn_nvo(dvs, nvoname, tnl_name):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    attrs = [
            ("source_vtep", tnl_name),
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

def create_vxlan_tunnel(dvs, name, src_ip, dst_ip = '0.0.0.0', skip_dst_ip=True):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    attrs = [
            ("src_ip", src_ip),
    ]

    if not skip_dst_ip:
        attrs.append(("dst_ip", dst_ip))

    # create the VXLAN tunnel Term entry in Config DB
    create_entry_tbl(
        conf_db,
        "VXLAN_TUNNEL", '|', name,
        attrs,
    )

def create_vxlan_tunnel_map(dvs, tnl_name, map_name, vni_id, vlan_id):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    attrs = [
            ("vni", vni_id),
            ("vlan", vlan_id),
    ]

    # create the VXLAN tunnel Term entry in Config DB
    create_entry_tbl(
        conf_db,
        "VXLAN_TUNNEL_MAP", '|', "%s|%s" % (tnl_name, map_name),
        attrs,
    )

def create_evpn_remote_vni(dvs, vlan_id, remote_vtep, vnid):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(
        app_db,
        "VXLAN_REMOTE_VNI_TABLE", ':', "%s:%s" % (vlan_id, remote_vtep),
        [
            ("vni", vnid),
        ],
    )
    time.sleep(2)

def remove_vxlan_tunnel(dvs, tnl_name):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    # create the VXLAN tunnel Term entry in Config DB
    delete_entry_tbl(
        conf_db,
        "VXLAN_TUNNEL", tnl_name,
    )

def remove_vxlan_tunnel_map(dvs, tnl_name, map_name,vni_id, vlan_id):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    attrs = [
            ("vni", vni_id),
            ("vlan", vlan_id),
    ]

    # create the VXLAN tunnel Term entry in Config DB
    delete_entry_tbl(
        conf_db,
        "VXLAN_TUNNEL_MAP", "%s|%s" % (tnl_name, map_name),
    )

def remove_evpn_remote_vni(dvs, vlan_id, remote_vtep ):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    delete_entry_pst(
        app_db,
        "VXLAN_REMOTE_VNI_TABLE", "%s:%s" % (vlan_id, remote_vtep),
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

loopback_id = 0
def_vr_id = 0
switch_mac = None

class VxlanTunnel(object):

    ASIC_TUNNEL_TABLE       = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL"
    ASIC_TUNNEL_MAP         = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP"
    ASIC_TUNNEL_MAP_ENTRY   = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY"
    ASIC_TUNNEL_TERM_ENTRY  = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY"
    ASIC_BRIDGE_PORT        = "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT"

    tunnel_map_ids       = set()
    tunnel_map_entry_ids = set()
    tunnel_ids           = set()
    tunnel_term_ids      = set()
    bridgeport_ids       = set()
    tunnel_map_map       = {}
    tunnel               = {}
    tunnel_appdb         = {}
    tunnel_term          = {}
    map_entry_map        = {}
    dip_tunnel_map       = {}
    dip_tun_state_map    = {}
    bridgeport_map       = {}
    vlan_id_map          = {}
    vlan_member_map      = {}

    def fetch_exist_entries(self, dvs):
        self.tunnel_ids = get_exist_entries(dvs, self.ASIC_TUNNEL_TABLE)
        self.tunnel_map_ids = get_exist_entries(dvs, self.ASIC_TUNNEL_MAP)
        self.tunnel_map_entry_ids = get_exist_entries(dvs, self.ASIC_TUNNEL_MAP_ENTRY)
        self.tunnel_term_ids = get_exist_entries(dvs, self.ASIC_TUNNEL_TERM_ENTRY)
        self.bridgeport_ids = get_exist_entries(dvs, self.ASIC_BRIDGE_PORT)

    def check_vxlan_tunnel_map_entry_delete(self, dvs, tunnel_name, vidlist, vnilist):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_MAP_ENTRY)

        for x in range(len(vidlist)):
            status, fvs = tbl.get(self.map_entry_map[tunnel_name + vidlist[x]])
            assert status == False, "SIP Tunnel Map entry not deleted"
            iplinkcmd = "ip link show type vxlan dev " + tunnel_name + "-" + vidlist[x]
            (exitcode, out) = dvs.runcmd(iplinkcmd)
            assert exitcode != 0, "Kernel device not deleted"

    def check_vxlan_tunnel_map_entry(self, dvs, tunnel_name, vidlist, vnidlist):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_MAP_ENTRY)

        expected_attributes_1 = {
        'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID',
        'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': self.tunnel_map_map[tunnel_name][0],
        'SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE': vidlist[0],
        'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY': vnidlist[0],
        }

        for x in range(len(vidlist)):
            expected_attributes_1['SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE'] = vidlist[x]
            expected_attributes_1['SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY'] = vnidlist[x]
            ret = get_key_with_attr(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, expected_attributes_1)
            assert len(ret) > 0, "SIP TunnelMap entry not created"
            assert len(ret) == 1, "More than 1 SIP TunnMapEntry created"
            self.map_entry_map[tunnel_name + vidlist[x]] = ret[0]
            iplinkcmd = "ip link show type vxlan dev " + tunnel_name + "-" + vidlist[x]
            (exitcode, out) = dvs.runcmd(iplinkcmd)
            assert exitcode == 0, "Kernel device not created"
            
   
    def check_vxlan_sip_tunnel_delete(self, dvs, tunnel_name):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(app_db, "VXLAN_TUNNEL_TABLE")
        status, fvs = tbl.get(self.tunnel_appdb[tunnel_name])
        assert status == False, "SIP Tunnel entry not deleted from APP_DB"

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_TERM_ENTRY)
        status, fvs = tbl.get(self.tunnel_term[tunnel_name])
        assert status == False, "SIP Tunnel Term entry not deleted from ASIC_DB"

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_TABLE)
        status, fvs = tbl.get(self.tunnel[tunnel_name])
        assert status == False, "SIP Tunnel entry not deleted from ASIC_DB"

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_MAP)
        status, fvs = tbl.get(self.tunnel_map_map[tunnel_name][0])
        assert status == False, "SIP Tunnel mapper0 not deleted from ASIC_DB"
        status, fvs = tbl.get(self.tunnel_map_map[tunnel_name][1])
        assert status == False, "SIP Tunnel mapper1 not deleted from ASIC_DB"
        status, fvs = tbl.get(self.tunnel_map_map[tunnel_name][2])
        assert status == False, "SIP Tunnel mapper2 not deleted from ASIC_DB"
        status, fvs = tbl.get(self.tunnel_map_map[tunnel_name][3])
        assert status == False, "SIP Tunnel mapper3 not deleted from ASIC_DB"

    def check_vxlan_sip_tunnel(self, dvs, tunnel_name, src_ip, vidlist, vnidlist, dst_ip = '0.0.0.0', skip_dst_ip = 'True'):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

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
                        #'SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE': loopback_id,

        check_object(asic_db, self.ASIC_TUNNEL_TABLE, tunnel_id,
                    {
                        'SAI_TUNNEL_ATTR_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
                        'SAI_TUNNEL_ATTR_DECAP_MAPPERS': decapstr,
                        'SAI_TUNNEL_ATTR_ENCAP_MAPPERS': encapstr,
                        'SAI_TUNNEL_ATTR_PEER_MODE': 'SAI_TUNNEL_PEER_MODE_P2MP',
                        'SAI_TUNNEL_ATTR_ENCAP_SRC_IP': src_ip,
                    }
                )

        expected_attributes = {
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE': 'SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP',
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP': src_ip,
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID': tunnel_id,
        }

        if not skip_dst_ip:
           expected_attributes['SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP'] = dst_ip
           expected_attributes['SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE'] = 'SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2P'

        check_object(asic_db, self.ASIC_TUNNEL_TERM_ENTRY, tunnel_term_id, expected_attributes)

        expected_attributes_1 = {
        'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID',
        'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': tunnel_map_id[0],
        'SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE': vidlist[0],
        'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY': vnidlist[0],
        }

        for x in range(len(vidlist)):
            expected_attributes_1['SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE'] = vidlist[x]
            expected_attributes_1['SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY'] = vnidlist[x]
            check_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, tunnel_map_entry_id[x], expected_attributes_1)

        expected_siptnl_attributes = {
            'src_ip': src_ip,
        }

        if not skip_dst_ip:
           expected_siptnl_attributes['dst_ip'] =  dst_ip

        ret = get_key_with_attr(app_db, "VXLAN_TUNNEL_TABLE", expected_siptnl_attributes)
        assert len(ret) > 0, "SIP Tunnel entry not created in APPDB"
        assert len(ret) == 1, "More than 1 Tunn statetable entry created"
        self.tunnel_appdb[tunnel_name] = ret[0]

        self.tunnel_map_ids.update(tunnel_map_id)
        self.tunnel_ids.add(tunnel_id)
        self.tunnel_term_ids.add(tunnel_term_id)
        self.tunnel_map_map[tunnel_name] = tunnel_map_id
        self.tunnel[tunnel_name] = tunnel_id
        self.tunnel_term[tunnel_name] = tunnel_term_id

    def check_vxlan_dip_tunnel_delete(self, dvs, dip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)

        tbl = swsscommon.Table(state_db, 'VXLAN_TUNNEL_TABLE')
        status, fvs = tbl.get(self.dip_tun_state_map[dip])
        assert status == False, "State Table entry not deleted"

        tbl = swsscommon.Table(asic_db, self.ASIC_TUNNEL_TABLE)
        status, fvs = tbl.get(self.dip_tunnel_map[dip])
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
        self.dip_tun_state_map[dip] = ret[0]


        tunnel_map_id = self.tunnel_map_map[vtep_name]

        decapstr = '2:' + tunnel_map_id[0] + ',' + tunnel_map_id[2]
        encapstr = '2:' + tunnel_map_id[1] + ',' + tunnel_map_id[3]

        print(decapstr)
        print(encapstr)

        expected_tun_attributes = {
                        'SAI_TUNNEL_ATTR_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
                        'SAI_TUNNEL_ATTR_PEER_MODE': 'SAI_TUNNEL_PEER_MODE_P2P',
                        'SAI_TUNNEL_ATTR_DECAP_MAPPERS': decapstr,
                        'SAI_TUNNEL_ATTR_ENCAP_MAPPERS': encapstr,
                        'SAI_TUNNEL_ATTR_ENCAP_SRC_IP': src_ip,
                        'SAI_TUNNEL_ATTR_ENCAP_DST_IP': dip,
                    }
           
        ret = get_key_with_attr(asic_db, self.ASIC_TUNNEL_TABLE, expected_tun_attributes)
        assert len(ret) > 0, "Tunnel entry not created"
        assert len(ret) == 1, "More than 1 tunnel entry created"

        self.dip_tunnel_map[dip] = ret[0]
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

class TestVxlanOrch(object):

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

        create_vlan1(dvs,"Vlan100")
        create_vlan1(dvs,"Vlan101")
        create_vlan1(dvs,"Vlan102")
        create_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')
        create_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        create_vxlan_tunnel_map(dvs, tunnel_name, map_name_1, '1001', 'Vlan101')
        create_vxlan_tunnel_map(dvs, tunnel_name, map_name_2, '1002', 'Vlan102')

        vlanlist = ['100', '101', '102']
        vnilist = ['1000', '1001', '1002']

        print("Testing SIP Tunnel Creation")
        vxlan_obj.check_vxlan_sip_tunnel(dvs, tunnel_name, '6.6.6.6', vlanlist, vnilist)

        print("Testing Tunnel Map Entry")
        vxlan_obj.check_vxlan_tunnel_map_entry(dvs, tunnel_name, vlanlist, vnilist)

        print("Testing Tunnel Map entry removal")
        remove_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        remove_vxlan_tunnel_map(dvs, tunnel_name, map_name_1, '1001', 'Vlan101')
        remove_vxlan_tunnel_map(dvs, tunnel_name, map_name_2, '1002', 'Vlan102')
        vxlan_obj.check_vxlan_tunnel_map_entry_delete(dvs, tunnel_name, vlanlist, vnilist)

        print("Testing SIP Tunnel Deletion")
        remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan_obj.check_vxlan_sip_tunnel_delete(dvs, tunnel_name)

#    Test 2 - DIP Tunnel Tests
    def test_p2p_tunnel(self, dvs, testlog):
        vxlan_obj = self.get_vxlan_obj()

        tunnel_name = 'tunnel_2'
        map_name = 'map_1000_100'
        map_name_1 = 'map_1001_101'
        map_name_2 = 'map_1002_102'
        vlanlist = ['100', '101', '102']
        vnilist = ['1000', '1001', '1002']

        vxlan_obj.fetch_exist_entries(dvs)
        create_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6')
        create_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        create_vxlan_tunnel_map(dvs, tunnel_name, map_name_1, '1001', 'Vlan101')
        create_vxlan_tunnel_map(dvs, tunnel_name, map_name_2, '1002', 'Vlan102')

        vxlan_obj.check_vxlan_sip_tunnel(dvs, tunnel_name, '6.6.6.6', vlanlist, vnilist)
        vxlan_obj.check_vxlan_tunnel_map_entry(dvs, tunnel_name, vlanlist, vnilist)

        create_evpn_nvo(dvs, 'nvo1', tunnel_name)
        create_evpn_remote_vni(dvs, 'Vlan100', '7.7.7.7', '1000')

        print("Testing DIP tunnel creation")
        vxlan_obj.check_vxlan_dip_tunnel(dvs, tunnel_name, '6.6.6.6', '7.7.7.7')
        print("Testing VLAN 100 extension")
        vxlan_obj.check_vlan_extension(dvs, '100', '7.7.7.7')

        create_evpn_remote_vni(dvs, 'Vlan101', '7.7.7.7', '1001')
        create_evpn_remote_vni(dvs, 'Vlan102', '7.7.7.7', '1002')

        print("Testing DIP tunnel not created again")
        vxlan_obj.check_vxlan_dip_tunnel(dvs, tunnel_name, '6.6.6.6', '7.7.7.7')

        print("Testing VLAN 101 extension")
        vxlan_obj.check_vlan_extension(dvs, '101', '7.7.7.7')

        print("Testing VLAN 102 extension")
        vxlan_obj.check_vlan_extension(dvs, '102', '7.7.7.7')

        print("Testing another DIP tunnel to 8.8.8.8")
        create_evpn_remote_vni(dvs, 'Vlan100', '8.8.8.8', '1000')
        print("Testing DIP tunnel creation to 8.8.8.8")
        vxlan_obj.check_vxlan_dip_tunnel(dvs, tunnel_name, '6.6.6.6', '8.8.8.8')
        print("Testing VLAN 100 extension to 8.8.8.8 and 7.7.7.7")
        vxlan_obj.check_vlan_extension(dvs, '100', '8.8.8.8')
        vxlan_obj.check_vlan_extension(dvs, '100', '7.7.7.7')

        print("Testing Vlan Extension removal")
        remove_evpn_remote_vni(dvs, 'Vlan100', '7.7.7.7')
        remove_evpn_remote_vni(dvs, 'Vlan101', '7.7.7.7')
        vxlan_obj.check_vlan_extension_delete(dvs, '100', '7.7.7.7')
        vxlan_obj.check_vlan_extension_delete(dvs, '101', '7.7.7.7')
        print("Testing DIP tunnel not deleted")
        vxlan_obj.check_vxlan_dip_tunnel(dvs, tunnel_name, '6.6.6.6', '7.7.7.7')

        print("Testing Last Vlan removal and DIP tunnel delete")
        remove_evpn_remote_vni(dvs, 'Vlan102', '7.7.7.7')
        vxlan_obj.check_vlan_extension_delete(dvs, '102', '7.7.7.7')
        vxlan_obj.check_vxlan_dip_tunnel_delete(dvs, '7.7.7.7')

        print("Testing Last Vlan removal and DIP tunnel delete for 8.8.8.8")
        remove_evpn_remote_vni(dvs, 'Vlan100', '8.8.8.8')
        vxlan_obj.check_vlan_extension_delete(dvs, '100', '8.8.8.8')
        vxlan_obj.check_vxlan_dip_tunnel_delete(dvs, '8.8.8.8')

        remove_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        remove_vxlan_tunnel_map(dvs, tunnel_name, map_name_1, '1001', 'Vlan101')
        remove_vxlan_tunnel_map(dvs, tunnel_name, map_name_2, '1002', 'Vlan102')
        vxlan_obj.check_vxlan_tunnel_map_entry_delete(dvs, tunnel_name, vlanlist, vnilist)

        print("Testing SIP Tunnel Deletion")
        remove_evpn_nvo(dvs, 'nvo1')
        remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan_obj.check_vxlan_sip_tunnel_delete(dvs, tunnel_name)


#    Test 3 - Create and Delete SIP Tunnel and Map entries
    def test_p2mp_tunnel_with_dip(self, dvs, testlog):
        vxlan_obj = self.get_vxlan_obj()

        tunnel_name = 'tunnel_2'
        map_name = 'map_1000_100'
        map_name_1 = 'map_1001_101'
        map_name_2 = 'map_1002_102'

        vxlan_obj.fetch_exist_entries(dvs)

        create_vlan1(dvs,"Vlan100")
        create_vlan1(dvs,"Vlan101")
        create_vlan1(dvs,"Vlan102")
        create_vxlan_tunnel(dvs, tunnel_name, '6.6.6.6', '2.2.2.2', False)
        create_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        create_vxlan_tunnel_map(dvs, tunnel_name, map_name_1, '1001', 'Vlan101')
        create_vxlan_tunnel_map(dvs, tunnel_name, map_name_2, '1002', 'Vlan102')

        vlanlist = ['100', '101', '102']
        vnilist = ['1000', '1001', '1002']

        print("Testing SIP Tunnel Creation")
        vxlan_obj.check_vxlan_sip_tunnel(dvs, tunnel_name, '6.6.6.6', vlanlist, vnilist, '2.2.2.2', False)

        print("Testing Tunnel Map Entry")
        vxlan_obj.check_vxlan_tunnel_map_entry(dvs, tunnel_name, vlanlist, vnilist)

        print("Testing Tunnel Map entry removal")
        remove_vxlan_tunnel_map(dvs, tunnel_name, map_name, '1000', 'Vlan100')
        remove_vxlan_tunnel_map(dvs, tunnel_name, map_name_1, '1001', 'Vlan101')
        remove_vxlan_tunnel_map(dvs, tunnel_name, map_name_2, '1002', 'Vlan102')
        vxlan_obj.check_vxlan_tunnel_map_entry_delete(dvs, tunnel_name, vlanlist, vnilist)

        print("Testing SIP Tunnel Deletion")
        remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan_obj.check_vxlan_sip_tunnel_delete(dvs, tunnel_name)
