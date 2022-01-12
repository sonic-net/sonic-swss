import time
import json
import random
import time
import pytest

from swsscommon import swsscommon
from pprint import pprint
from dvslib.dvs_common import wait_for_result


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


def delete_entry_tbl(db, table, key):
    tbl = swsscommon.Table(db, table)
    tbl._del(key)
    time.sleep(1)


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


def get_all_created_entries(db, table, existed_entries):
    tbl =  swsscommon.Table(db, table)
    entries = set(tbl.getKeys())
    new_entries = list(entries - existed_entries)
    assert len(new_entries) >= 0, "Get all could be no new created entries."
    new_entries.sort()
    return new_entries


def get_created_entries(db, table, existed_entries, count):
    new_entries = get_all_created_entries(db, table, existed_entries)
    assert len(new_entries) == count, "Wrong number of created entries."
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


def create_vnet_local_routes(dvs, prefix, vnet_name, ifname):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    create_entry_tbl(
        conf_db,
        "VNET_ROUTE", '|', "%s|%s" % (vnet_name, prefix),
        [
            ("ifname", ifname),
        ]
    )

    time.sleep(2)


def delete_vnet_local_routes(dvs, prefix, vnet_name):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

    delete_entry_pst(app_db, "VNET_ROUTE_TABLE", "%s:%s" % (vnet_name, prefix))

    time.sleep(2)


def create_vnet_routes(dvs, prefix, vnet_name, endpoint, mac="", vni=0, ep_monitor=""):
    set_vnet_routes(dvs, prefix, vnet_name, endpoint, mac=mac, vni=vni, ep_monitor=ep_monitor)


def set_vnet_routes(dvs, prefix, vnet_name, endpoint, mac="", vni=0, ep_monitor=""):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    attrs = [
            ("endpoint", endpoint),
    ]

    if vni:
        attrs.append(('vni', vni))

    if mac:
        attrs.append(('mac_address', mac))

    if ep_monitor:
        attrs.append(('endpoint_monitor', ep_monitor))

    tbl = swsscommon.Table(conf_db, "VNET_ROUTE_TUNNEL")
    fvs = swsscommon.FieldValuePairs(attrs)
    tbl.set("%s|%s" % (vnet_name, prefix), fvs)

    time.sleep(2)


def delete_vnet_routes(dvs, prefix, vnet_name):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

    delete_entry_pst(app_db, "VNET_ROUTE_TUNNEL_TABLE", "%s:%s" % (vnet_name, prefix))

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
          ("proxy_arp", "enabled"),
        ],
    )

    #FIXME - This is created by IntfMgr
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(
        app_db,
        "INTF_TABLE", ':', vlan_name,
        [
            ("vnet_name", vnet_name),
            ("proxy_arp", "enabled"),
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


def delete_vlan_interface(dvs, ifname, ipaddr):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    delete_entry_tbl(conf_db, "VLAN_INTERFACE", "%s|%s" % (ifname, ipaddr))

    time.sleep(2)

    delete_entry_tbl(conf_db, "VLAN_INTERFACE", ifname)

    time.sleep(2)


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


def delete_phy_interface(dvs, ifname, ipaddr):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    delete_entry_tbl(conf_db, "INTERFACE", "%s|%s" % (ifname, ipaddr))

    time.sleep(2)

    delete_entry_tbl(conf_db, "INTERFACE", ifname)

    time.sleep(2)


def create_vnet_entry(dvs, name, tunnel, vni, peer_list, scope="", advertise_prefix=False):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

    attrs = [
            ("vxlan_tunnel", tunnel),
            ("vni", vni),
            ("peer_list", peer_list),
    ]

    if scope:
        attrs.append(('scope', scope))

    if advertise_prefix:
        attrs.append(('advertise_prefix', 'true'))

    # create the VXLAN tunnel Term entry in Config DB
    create_entry_tbl(
        conf_db,
        "VNET", '|', name,
        attrs,
    )

    time.sleep(2)


def delete_vnet_entry(dvs, name):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    delete_entry_tbl(conf_db, "VNET", "%s" % (name))

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


def create_vxlan_tunnel_map(dvs, tunnel_name, tunnel_map_entry_name, vlan, vni_id):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    # create the VXLAN tunnel map entry in Config DB
    create_entry_tbl(
        conf_db,
        "VXLAN_TUNNEL_MAP", '|', "%s|%s" % (tunnel_name, tunnel_map_entry_name),
        [
            ("vni",  vni_id),
            ("vlan", vlan),
        ],
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


def check_linux_intf_arp_proxy(dvs, ifname):
    (exitcode, out) = dvs.runcmd("cat /proc/sys/net/ipv4/conf/{0}/proxy_arp_pvlan".format(ifname))
    assert out != "1", "ARP proxy is not enabled for VNET interface in Linux kernel"


def update_bfd_session_state(dvs, addr, state):
    bfd_id = get_bfd_session_id(dvs, addr)
    assert bfd_id is not None

    bfd_sai_state = {"Admin_Down":  "SAI_BFD_SESSION_STATE_ADMIN_DOWN",
                     "Down":        "SAI_BFD_SESSION_STATE_DOWN",
                     "Init":        "SAI_BFD_SESSION_STATE_INIT",
                     "Up":          "SAI_BFD_SESSION_STATE_UP"}

    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    ntf = swsscommon.NotificationProducer(asic_db, "NOTIFICATIONS")
    fvp = swsscommon.FieldValuePairs()
    ntf_data = "[{\"bfd_session_id\":\""+bfd_id+"\",\"session_state\":\""+bfd_sai_state[state]+"\"}]"
    ntf.send("bfd_session_state_change", ntf_data, fvp)


def get_bfd_session_id(dvs, addr):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    tbl =  swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION")
    entries = set(tbl.getKeys())
    for entry in entries:
        status, fvs = tbl.get(entry)
        fvs = dict(fvs)
        assert status, "Got an error when get a key"
        if fvs["SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS"] == addr:
            return entry

    return None


def check_del_bfd_session(dvs, addrs):
    for addr in addrs:
        assert get_bfd_session_id(dvs, addr) is None


def check_bfd_session(dvs, addrs):
    for addr in addrs:
        assert get_bfd_session_id(dvs, addr) is not None


def check_state_db_routes(dvs, vnet, prefix, endpoints):
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
    tbl =  swsscommon.Table(state_db, "VNET_ROUTE_TUNNEL_TABLE")

    status, fvs = tbl.get(vnet + '|' + prefix)
    assert status, "Got an error when get a key"

    fvs = dict(fvs)
    assert fvs['active_endpoints'] == ','.join(endpoints)

    if endpoints:
        assert fvs['state'] == 'active'
    else:
        assert fvs['state'] == 'inactive'


def check_remove_state_db_routes(dvs, vnet, prefix):
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
    tbl =  swsscommon.Table(state_db, "VNET_ROUTE_TUNNEL_TABLE")
    keys = tbl.getKeys()

    assert vnet + '|' + prefix not in keys


def check_routes_advertisement(dvs, prefix):
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
    tbl =  swsscommon.Table(state_db, "ADVERTISE_NETWORK_TABLE")
    keys = tbl.getKeys()

    assert prefix in keys


def check_remove_routes_advertisement(dvs, prefix):
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
    tbl =  swsscommon.Table(state_db, "ADVERTISE_NETWORK_TABLE")
    keys = tbl.getKeys()

    assert prefix not in keys


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
    ASIC_VLAN_TABLE         = "ASIC_STATE:SAI_OBJECT_TYPE_VLAN"
    ASIC_NEXT_HOP_GROUP     = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP"
    ASIC_NEXT_HOP_GROUP_MEMBER  = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER"
    ASIC_BFD_SESSION        = "ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION"

    tunnel_map_ids       = set()
    tunnel_map_entry_ids = set()
    tunnel_ids           = set()
    tunnel_term_ids      = set()
    tunnel_map_map       = {}
    tunnel               = {}
    vnet_vr_ids          = set()
    vr_map               = {}
    nh_ids               = {}
    nhg_ids              = {}

    def fetch_exist_entries(self, dvs):
        self.vnet_vr_ids = get_exist_entries(dvs, self.ASIC_VRF_TABLE)
        self.tunnel_ids = get_exist_entries(dvs, self.ASIC_TUNNEL_TABLE)
        self.tunnel_map_ids = get_exist_entries(dvs, self.ASIC_TUNNEL_MAP)
        self.tunnel_map_entry_ids = get_exist_entries(dvs, self.ASIC_TUNNEL_MAP_ENTRY)
        self.tunnel_term_ids = get_exist_entries(dvs, self.ASIC_TUNNEL_TERM_ENTRY)
        self.rifs = get_exist_entries(dvs, self.ASIC_RIF_TABLE)
        self.routes = get_exist_entries(dvs, self.ASIC_ROUTE_ENTRY)
        self.nhops = get_exist_entries(dvs, self.ASIC_NEXT_HOP)
        self.nhgs = get_exist_entries(dvs, self.ASIC_NEXT_HOP_GROUP)
        self.bfd_sessions = get_exist_entries(dvs, self.ASIC_BFD_SESSION)

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

        tunnel_map_id  = get_created_entries(asic_db, self.ASIC_TUNNEL_MAP, self.tunnel_map_ids, 4)
        tunnel_id      = get_created_entry(asic_db, self.ASIC_TUNNEL_TABLE, self.tunnel_ids)
        tunnel_term_id = get_created_entry(asic_db, self.ASIC_TUNNEL_TERM_ENTRY, self.tunnel_term_ids)

        # check that the vxlan tunnel termination are there
        assert how_many_entries_exist(asic_db, self.ASIC_TUNNEL_MAP) == (len(self.tunnel_map_ids) + 4), "The TUNNEL_MAP wasn't created"
        assert how_many_entries_exist(asic_db, self.ASIC_TUNNEL_MAP_ENTRY) == len(self.tunnel_map_entry_ids), "The TUNNEL_MAP_ENTRY is created"
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

        check_object(asic_db, self.ASIC_TUNNEL_MAP, tunnel_map_id[0],
                        {
                            'SAI_TUNNEL_MAP_ATTR_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID',
                        }
                    )

        check_object(asic_db, self.ASIC_TUNNEL_MAP, tunnel_map_id[1],
                        {
                            'SAI_TUNNEL_MAP_ATTR_TYPE': 'SAI_TUNNEL_MAP_TYPE_VLAN_ID_TO_VNI',
                        }
                )

        check_object(asic_db, self.ASIC_TUNNEL_TABLE, tunnel_id,
                    {
                        'SAI_TUNNEL_ATTR_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
                        'SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE': loopback_id,
                        'SAI_TUNNEL_ATTR_DECAP_MAPPERS': '2:%s,%s' % (tunnel_map_id[0], tunnel_map_id[2]),
                        'SAI_TUNNEL_ATTR_ENCAP_MAPPERS': '2:%s,%s' % (tunnel_map_id[1], tunnel_map_id[3]),
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
            tunnel_map_id = get_created_entries(asic_db, self.ASIC_TUNNEL_MAP, self.tunnel_map_ids, 4)
        else:
            tunnel_map_id = self.tunnel_map_map[tunnel_name]

        tunnel_map_entry_id = get_created_entries(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, self.tunnel_map_entry_ids, 2)

        # check that the vxlan tunnel termination are there
        assert how_many_entries_exist(asic_db, self.ASIC_TUNNEL_MAP_ENTRY) == (len(self.tunnel_map_entry_ids) + 2), "The TUNNEL_MAP_ENTRY is created too early"

        check_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, tunnel_map_entry_id[0],
            {
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI',
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': tunnel_map_id[3],
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_KEY': self.vr_map[vnet_name].get('ing'),
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE': vni_id,
            }
        )

        check_object(asic_db, self.ASIC_TUNNEL_MAP_ENTRY, tunnel_map_entry_id[1],
            {
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID',
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': tunnel_map_id[2],
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY': vni_id,
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_VALUE': self.vr_map[vnet_name].get('egr'),
            }
        )

        self.tunnel_map_entry_ids.update(tunnel_map_entry_id)

    def check_vnet_entry(self, dvs, name, peer_list=[]):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

        #Assert if there are linklocal entries
        tbl = swsscommon.Table(app_db, "VNET_ROUTE_TUNNEL_TABLE")
        route_entries = tbl.getKeys()
        assert "ff00::/8" not in route_entries
        assert "fe80::/64" not in route_entries

        #Check virtual router objects
        assert how_many_entries_exist(asic_db, self.ASIC_VRF_TABLE) == (len(self.vnet_vr_ids) + 1),\
                                     "The VR objects are not created"

        new_vr_ids  = get_created_entries(asic_db, self.ASIC_VRF_TABLE, self.vnet_vr_ids, 1)

        self.vnet_vr_ids.update(new_vr_ids)
        self.vr_map[name] = { 'ing':new_vr_ids[0], 'egr':new_vr_ids[0], 'peer':peer_list }

    def check_default_vnet_entry(self, dvs, name):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        #Check virtual router objects
        assert how_many_entries_exist(asic_db, self.ASIC_VRF_TABLE) == (len(self.vnet_vr_ids)),\
                                     "Some VR objects are created"
        #Mappers for default VNET is created with default VR objects.
        self.vr_map[name] = { 'ing':list(self.vnet_vr_ids)[0], 'egr':list(self.vnet_vr_ids)[0], 'peer':[] }

    def check_del_vnet_entry(self, dvs, name):
        # TODO: Implement for VRF VNET
        return True

    def vnet_route_ids(self, dvs, name, local=False):
        vr_set = set()

        vr_set.add(self.vr_map[name].get('ing'))

        try:
            for peer in self.vr_map[name].get('peer'):
                vr_set.add(self.vr_map[peer].get('ing'))
        except IndexError:
            pass

        return vr_set

    def check_router_interface(self, dvs, intf_name, name, vlan_oid=0):
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

        #IP2ME route will be created with every router interface
        new_route = get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, 1)

        if vlan_oid:
            expected_attr = { 'SAI_VLAN_ATTR_BROADCAST_FLOOD_CONTROL_TYPE': 'SAI_VLAN_FLOOD_CONTROL_TYPE_NONE' }
            check_object(asic_db, self.ASIC_VLAN_TABLE, vlan_oid, expected_attr)

        check_linux_intf_arp_proxy(dvs, intf_name)

        self.rifs.add(new_rif)
        self.routes.update(new_route)

    def check_del_router_interface(self, dvs, name):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        old_rif = get_deleted_entries(asic_db, self.ASIC_RIF_TABLE, self.rifs, 1)
        check_deleted_object(asic_db, self.ASIC_RIF_TABLE, old_rif[0])

        self.rifs.remove(old_rif[0])

    def check_vnet_local_routes(self, dvs, name):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        vr_ids = self.vnet_route_ids(dvs, name, True)
        count = len(vr_ids)

        new_route = get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, count)

        #Routes are not replicated to egress VRF, return if count is 0, else check peering
        if not count:
            return

        asic_vrs = set()
        for idx in range(count):
            rt_key = json.loads(new_route[idx])
            asic_vrs.add(rt_key['vr'])

        assert asic_vrs == vr_ids

        self.routes.update(new_route)

    def check_del_vnet_local_routes(self, dvs, name):
        # TODO: Implement for VRF VNET
        return True

    def check_vnet_routes(self, dvs, name, endpoint, tunnel, mac="", vni=0, route_ids=""):
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
        if not route_ids:
            new_route = get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, count)
        else:
            new_route = route_ids

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

        return new_route

    def serialize_endpoint_group(self, endpoints):
        endpoints.sort()
        return ",".join(endpoints)

    def check_next_hop_group_member(self, dvs, nhg, expected_endpoint, expected_attrs):
        expected_endpoint_str = self.serialize_endpoint_group(expected_endpoint)
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        tbl_nhgm =  swsscommon.Table(asic_db, self.ASIC_NEXT_HOP_GROUP_MEMBER)
        tbl_nh =  swsscommon.Table(asic_db, self.ASIC_NEXT_HOP)
        entries = set(tbl_nhgm.getKeys())
        endpoints = []
        for entry in entries:
            status, fvs = tbl_nhgm.get(entry)
            fvs = dict(fvs)
            assert status, "Got an error when get a key"
            if fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhg:
                nh_key = fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"]
                status, nh_fvs = tbl_nh.get(nh_key)
                nh_fvs = dict(nh_fvs)
                assert status, "Got an error when get a key"
                endpoint = nh_fvs["SAI_NEXT_HOP_ATTR_IP"]
                endpoints.append(endpoint)
                assert endpoint in expected_attrs
                check_object(asic_db, self.ASIC_NEXT_HOP, nh_key, expected_attrs[endpoint])

        assert self.serialize_endpoint_group(endpoints) == expected_endpoint_str

    def check_vnet_ecmp_routes(self, dvs, name, endpoints, tunnel, mac=[], vni=[], route_ids=[], nhg=""):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        endpoint_str = name + "|" + self.serialize_endpoint_group(endpoints)

        vr_ids = self.vnet_route_ids(dvs, name)
        count = len(vr_ids)

        expected_attrs = {}
        for idx, endpoint in enumerate(endpoints):
            expected_attr = {
                        "SAI_NEXT_HOP_ATTR_TYPE": "SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP",
                        "SAI_NEXT_HOP_ATTR_IP": endpoint,
                        "SAI_NEXT_HOP_ATTR_TUNNEL_ID": self.tunnel[tunnel],
                    }
            if vni and vni[idx]:
                expected_attr.update({'SAI_NEXT_HOP_ATTR_TUNNEL_VNI': vni[idx]})
            if mac and mac[idx]:
                expected_attr.update({'SAI_NEXT_HOP_ATTR_TUNNEL_MAC': mac[idx]})
            expected_attrs[endpoint] = expected_attr

        if nhg:
            new_nhg = nhg
        elif endpoint_str in self.nhg_ids:
            new_nhg = self.nhg_ids[endpoint_str]
        else:
            new_nhg = get_created_entry(asic_db, self.ASIC_NEXT_HOP_GROUP, self.nhgs)
            self.nhg_ids[endpoint_str] = new_nhg
            self.nhgs.add(new_nhg)


        # Check routes in ingress VRF
        expected_nhg_attr = {
                        "SAI_NEXT_HOP_GROUP_ATTR_TYPE": "SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP",
                    }
        check_object(asic_db, self.ASIC_NEXT_HOP_GROUP, new_nhg, expected_nhg_attr)

        # Check nexthop group member
        self.check_next_hop_group_member(dvs, new_nhg, endpoints, expected_attrs)

        if route_ids:
            new_route = route_ids
        else:
            new_route = get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, count)

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

        assert asic_vrs == vr_ids

        self.routes.update(new_route)

        return new_route, new_nhg

    def check_del_vnet_routes(self, dvs, name, prefixes=[]):
        # TODO: Implement for VRF VNET

        def _access_function():
            route_entries = get_exist_entries(dvs, self.ASIC_ROUTE_ENTRY)
            route_prefixes = [json.loads(route_entry)["dest"] for route_entry in route_entries]
            return (all(prefix not in route_prefixes for prefix in prefixes), None)

        if prefixes:
            wait_for_result(_access_function)

        return True


class TestVnetOrch(object):

    def get_vnet_obj(self):
        return VnetVxlanVrfTunnel()

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
        vnet_obj.check_router_interface(dvs, "Vlan100", 'Vnet_2000', vid)

        vid = create_vlan_interface(dvs, "Vlan101", "Ethernet28", "Vnet_2000", "100.100.4.1/24")
        vnet_obj.check_router_interface(dvs, "Vlan101", 'Vnet_2000', vid)

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", 'Vnet_2000', '10.10.10.1')
        vnet_obj.check_vnet_routes(dvs, 'Vnet_2000', '10.10.10.1', tunnel_name)
        check_state_db_routes(dvs, 'Vnet_2000', "100.100.1.1/32", ['10.10.10.1'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        create_vnet_local_routes(dvs, "100.100.3.0/24", 'Vnet_2000', 'Vlan100')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_2000')

        create_vnet_local_routes(dvs, "100.100.4.0/24", 'Vnet_2000', 'Vlan101')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_2000')

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
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet_2000')

        delete_vnet_local_routes(dvs, "100.100.4.0/24", 'Vnet_2000')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet_2000')

        delete_vnet_local_routes(dvs, "100.102.1.0/24", 'Vnet_2001')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet_2001')

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
        vnet_obj.check_router_interface(dvs, "Vlan1001", 'Vnet_1', vid)

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
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_1')

        create_vnet_entry(dvs, 'Vnet_2', tunnel_name, '2222', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet_2')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet_2', '2222')

        vid = create_vlan_interface(dvs, "Vlan1002", "Ethernet4", "Vnet_2", "2.2.10.1/24")
        vnet_obj.check_router_interface(dvs, "Vlan1002", 'Vnet_2', vid)

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
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_2')

        # Clean-up and verify remove flows

        delete_vnet_local_routes(dvs, "2.2.10.0/24", 'Vnet_2')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet_2')

        delete_vnet_local_routes(dvs, "1.1.10.0/24", 'Vnet_1')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet_1')

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
        vnet_obj.check_router_interface(dvs, "Vlan2001", 'Vnet_10', vid)

        vid = create_vlan_interface(dvs, "Vlan2002", "Ethernet12", "Vnet_20", "8.8.10.1/24")
        vnet_obj.check_router_interface(dvs, "Vlan2002", 'Vnet_20', vid)

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
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_10')

        create_vnet_local_routes(dvs, "8.8.10.0/24", 'Vnet_20', 'Vlan2002')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet_20')

        # Clean-up and verify remove flows

        delete_vnet_local_routes(dvs, "5.5.10.0/24", 'Vnet_10')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet_10')

        delete_vnet_local_routes(dvs, "8.8.10.0/24", 'Vnet_20')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet_20')

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
        vnet_obj.check_router_interface(dvs, "Vlan300", 'Vnet3001', vid)

        vid = create_vlan_interface(dvs, "Vlan301", "Ethernet28", 'Vnet3001', "100.100.4.1/24")
        vnet_obj.check_router_interface(dvs, "Vlan301",  'Vnet3001', vid)

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
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet3001')

        create_vnet_local_routes(dvs, "100.100.4.0/24", 'Vnet3001', 'Vlan301')
        vnet_obj.check_vnet_local_routes(dvs, 'Vnet3001')

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
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet3002')

        delete_phy_interface(dvs, "Ethernet60", "100.102.1.1/24")
        vnet_obj.check_del_router_interface(dvs, "Ethernet60")

        delete_vnet_entry(dvs, 'Vnet3002')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet3002')

        delete_vnet_local_routes(dvs, "100.100.3.0/24", 'Vnet3001')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet3001')

        delete_vnet_local_routes(dvs, "100.100.4.0/24", 'Vnet3001')
        vnet_obj.check_del_vnet_local_routes(dvs, 'Vnet3001')

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

    '''
    Test 7 - Test for vnet tunnel routes with ECMP nexthop group
    '''
    def test_vnet_orch_7(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_7'

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '7.7.7.7')
        create_vnet_entry(dvs, 'Vnet7', tunnel_name, '10007', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet7')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet7', '10007')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '7.7.7.7')

        # Create an ECMP tunnel route
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", 'Vnet7', '7.0.0.1,7.0.0.2,7.0.0.3')
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet7', ['7.0.0.1', '7.0.0.2', '7.0.0.3'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet7', "100.100.1.1/32", ['7.0.0.1', '7.0.0.2', '7.0.0.3'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Set the tunnel route to another nexthop group
        set_vnet_routes(dvs, "100.100.1.1/32", 'Vnet7', '7.0.0.1,7.0.0.2,7.0.0.3,7.0.0.4')
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet7', ['7.0.0.1', '7.0.0.2', '7.0.0.3', '7.0.0.4'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, 'Vnet7', "100.100.1.1/32", ['7.0.0.1', '7.0.0.2', '7.0.0.3', '7.0.0.4'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_1 not in vnet_obj.nhgs

        # Create another tunnel route to the same set of endpoints
        create_vnet_routes(dvs, "100.100.2.1/32", 'Vnet7', '7.0.0.1,7.0.0.2,7.0.0.3,7.0.0.4')
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet7', ['7.0.0.1', '7.0.0.2', '7.0.0.3', '7.0.0.4'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet7', "100.100.2.1/32", ['7.0.0.1', '7.0.0.2', '7.0.0.3', '7.0.0.4'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        assert nhg2_1 == nhg1_2

        # Remove one of the tunnel routes
        delete_vnet_routes(dvs, "100.100.1.1/32", 'Vnet7')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet7', ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, 'Vnet7', "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Check the nexthop group still exists
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_2 in vnet_obj.nhgs

        # Remove the other tunnel route
        delete_vnet_routes(dvs, "100.100.2.1/32", 'Vnet7')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet7', ["100.100.2.1/32"])
        check_remove_state_db_routes(dvs, 'Vnet7', "100.100.2.1/32")
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        # Check the nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg2_1 not in vnet_obj.nhgs

        delete_vnet_entry(dvs, 'Vnet7')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet7')

    '''
    Test 8 - Test for ipv6 vnet tunnel routes with ECMP nexthop group
    '''
    def test_vnet_orch_8(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_8'

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, 'fd:8::32')
        create_vnet_entry(dvs, 'Vnet8', tunnel_name, '10008', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet8')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet8', '10008')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, 'fd:8::32')

        # Create an ECMP tunnel route
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "fd:8:10::32/128", 'Vnet8', 'fd:8:1::1,fd:8:1::2,fd:8:1::3')
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet8', ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet8', "fd:8:10::32/128", ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")

        # Set the tunnel route to another nexthop group
        set_vnet_routes(dvs, "fd:8:10::32/128", 'Vnet8', 'fd:8:1::1,fd:8:1::2,fd:8:1::3,fd:8:1::4')
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet8', ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3', 'fd:8:1::4'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, 'Vnet8', "fd:8:10::32/128", ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3', 'fd:8:1::4'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_1 not in vnet_obj.nhgs

        # Create another tunnel route to the same set of endpoints
        create_vnet_routes(dvs, "fd:8:20::32/128", 'Vnet8', 'fd:8:1::1,fd:8:1::2,fd:8:1::3,fd:8:1::4')
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet8', ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3', 'fd:8:1::4'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet8', "fd:8:20::32/128", ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3', 'fd:8:1::4'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:8:20::32/128")

        assert nhg2_1 == nhg1_2

        # Create another tunnel route with ipv4 prefix to the same set of endpoints
        create_vnet_routes(dvs, "8.0.0.0/24", 'Vnet8', 'fd:8:1::1,fd:8:1::2,fd:8:1::3,fd:8:1::4')
        route3, nhg3_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet8', ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3', 'fd:8:1::4'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet8', "8.0.0.0/24", ['fd:8:1::1', 'fd:8:1::2', 'fd:8:1::3', 'fd:8:1::4'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "8.0.0.0/24")

        assert nhg3_1 == nhg1_2

        # Remove one of the tunnel routes
        delete_vnet_routes(dvs, "fd:8:10::32/128", 'Vnet8')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet8', ["fd:8:10::32/128"])
        check_remove_state_db_routes(dvs, 'Vnet8', "fd:8:10::32/128")
        check_remove_routes_advertisement(dvs, "fd:8:10::32/128")

        # Check the nexthop group still exists
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_2 in vnet_obj.nhgs

        # Remove tunnel route 2
        delete_vnet_routes(dvs, "fd:8:20::32/128", 'Vnet8')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet8', ["fd:8:20::32/128"])
        check_remove_state_db_routes(dvs, 'Vnet8', "fd:8:20::32/128")
        check_remove_routes_advertisement(dvs, "fd:8:20::32/128")

        # Remove tunnel route 3
        delete_vnet_routes(dvs, "8.0.0.0/24", 'Vnet8')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet8', ["8.0.0.0/24"])
        check_remove_state_db_routes(dvs, 'Vnet8', "8.0.0.0/24")
        check_remove_routes_advertisement(dvs, "8.0.0.0/24")

        # Check the nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg2_1 not in vnet_obj.nhgs

        delete_vnet_entry(dvs, 'Vnet8')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet8')
    

    '''
    Test 9 - Test for vnet tunnel routes with ECMP nexthop group with endpoint health monitor
    '''
    def test_vnet_orch_9(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_9'

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')
        create_vnet_entry(dvs, 'Vnet9', tunnel_name, '10009', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet9')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet9', '10009')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '9.9.9.9')

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", 'Vnet9', '9.0.0.1,9.0.0.2,9.0.0.3', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3')

        # default bfd status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet9', ["100.100.1.1/32"])
        check_state_db_routes(dvs, 'Vnet9', "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Route should be properly configured when all bfd session states go up
        update_bfd_session_state(dvs, '9.1.0.1', 'Up')
        update_bfd_session_state(dvs, '9.1.0.2', 'Up')
        update_bfd_session_state(dvs, '9.1.0.3', 'Up')
        time.sleep(2)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet9', ['9.0.0.1', '9.0.0.2', '9.0.0.3'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet9', "100.100.1.1/32", ['9.0.0.1', '9.0.0.2', '9.0.0.3'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Remove endpoint from group if it goes down
        update_bfd_session_state(dvs, '9.1.0.2', 'Down')
        time.sleep(2)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet9', ['9.0.0.1', '9.0.0.3'], tunnel_name, route_ids=route1, nhg=nhg1_1)
        check_state_db_routes(dvs, 'Vnet9', "100.100.1.1/32", ['9.0.0.1', '9.0.0.3'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Create another tunnel route with endpoint group overlapped with route1
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.2.1/32", 'Vnet9', '9.0.0.1,9.0.0.2,9.0.0.5', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.5')
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet9', ['9.0.0.1'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet9', "100.100.2.1/32", ['9.0.0.1'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Update BFD session state and verify route change
        update_bfd_session_state(dvs, '9.1.0.5', 'Up')
        time.sleep(2)
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet9', ['9.0.0.1', '9.0.0.5'], tunnel_name, route_ids=route2, nhg=nhg2_1)
        check_state_db_routes(dvs, 'Vnet9', "100.100.2.1/32", ['9.0.0.1', '9.0.0.5'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        # Update BFD state and check route nexthop
        update_bfd_session_state(dvs, '9.1.0.3', 'Down')
        time.sleep(2)

        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet9', ['9.0.0.1'], tunnel_name, route_ids=route1, nhg=nhg1_1)
        check_state_db_routes(dvs, 'Vnet9', "100.100.1.1/32", ['9.0.0.1'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Set the route1 to a new group
        set_vnet_routes(dvs, "100.100.1.1/32", 'Vnet9', '9.0.0.1,9.0.0.2,9.0.0.3,9.0.0.4', ep_monitor='9.1.0.1,9.1.0.2,9.1.0.3,9.1.0.4')
        update_bfd_session_state(dvs, '9.1.0.4', 'Up')
        time.sleep(2)
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet9', ['9.0.0.1', '9.0.0.4'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, 'Vnet9', "100.100.1.1/32", ['9.0.0.1', '9.0.0.4'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_1 not in vnet_obj.nhgs

        # Set BFD session state for a down endpoint to up
        update_bfd_session_state(dvs, '9.1.0.2', 'Up')
        time.sleep(2)
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet9', ['9.0.0.1', '9.0.0.2', '9.0.0.4'], tunnel_name, route_ids=route1, nhg=nhg1_2)
        check_state_db_routes(dvs, 'Vnet9', "100.100.1.1/32", ['9.0.0.1', '9.0.0.2', '9.0.0.4'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Set all endpoint to down state
        update_bfd_session_state(dvs, '9.1.0.1', 'Down')
        update_bfd_session_state(dvs, '9.1.0.2', 'Down')
        update_bfd_session_state(dvs, '9.1.0.3', 'Down')
        update_bfd_session_state(dvs, '9.1.0.4', 'Down')
        time.sleep(2)

        # Confirm the tunnel route is updated in ASIC
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet9', ["100.100.1.1/32"])
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet9', ['9.0.0.5'], tunnel_name, route_ids=route2, nhg=nhg2_1)
        check_state_db_routes(dvs, 'Vnet9', "100.100.2.1/32", ['9.0.0.5'])
        check_state_db_routes(dvs, 'Vnet9', "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        # Remove tunnel route2
        delete_vnet_routes(dvs, "100.100.2.1/32", 'Vnet9')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet9', ["100.100.2.1/32"])
        check_remove_state_db_routes(dvs, 'Vnet9', "100.100.2.1/32")
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        # Check the corresponding nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg2_1 not in vnet_obj.nhgs

        # Check the BFD session specific to the endpoint group is removed while others exist
        check_del_bfd_session(dvs, ['9.1.0.5'])
        check_bfd_session(dvs, ['9.1.0.1', '9.1.0.2', '9.1.0.3', '9.1.0.4'])

        # Remove tunnel route 1
        delete_vnet_routes(dvs, "100.100.1.1/32", 'Vnet9')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet9', ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, 'Vnet9', "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_2 not in vnet_obj.nhgs

        # Confirm the BFD sessions are removed
        check_del_bfd_session(dvs, ['9.1.0.1', '9.1.0.2', '9.1.0.3', '9.1.0.4', '9.1.0.5'])

        delete_vnet_entry(dvs, 'Vnet9')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet9')


    '''
    Test 10 - Test for ipv6 vnet tunnel routes with ECMP nexthop group with endpoint health monitor
    '''
    def test_vnet_orch_10(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_10'

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, 'fd:10::32')
        create_vnet_entry(dvs, 'Vnet10', tunnel_name, '10010', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet10')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet10', '10010')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, 'fd:10::32')

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "fd:10:10::1/128", 'Vnet10', 'fd:10:1::1,fd:10:1::2,fd:10:1::3', ep_monitor='fd:10:2::1,fd:10:2::2,fd:10:2::3')

        # default bfd status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet10', ["fd:10:10::1/128"])
        check_state_db_routes(dvs, 'Vnet10', "fd:10:10::1/128", [])
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")

        # Route should be properly configured when all bfd session states go up
        update_bfd_session_state(dvs, 'fd:10:2::1', 'Up')
        update_bfd_session_state(dvs, 'fd:10:2::2', 'Up')
        update_bfd_session_state(dvs, 'fd:10:2::3', 'Up')
        time.sleep(2)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet10', ['fd:10:1::1', 'fd:10:1::2', 'fd:10:1::3'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet10', "fd:10:10::1/128", ['fd:10:1::1', 'fd:10:1::2', 'fd:10:1::3'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")

        # Remove endpoint from group if it goes down
        update_bfd_session_state(dvs, 'fd:10:2::2', 'Down')
        time.sleep(2)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet10', ['fd:10:1::1', 'fd:10:1::3'], tunnel_name, route_ids=route1, nhg=nhg1_1)
        check_state_db_routes(dvs, 'Vnet10', "fd:10:10::1/128", ['fd:10:1::1', 'fd:10:1::3'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")

        # Create another tunnel route with endpoint group overlapped with route1
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "fd:10:20::1/128", 'Vnet10', 'fd:10:1::1,fd:10:1::2,fd:10:1::5', ep_monitor='fd:10:2::1,fd:10:2::2,fd:10:2::5')
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet10', ['fd:10:1::1'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet10', "fd:10:20::1/128", ['fd:10:1::1'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:10:20::1/128")

        # Update BFD session state and verify route change
        update_bfd_session_state(dvs, 'fd:10:2::5', 'Up')
        time.sleep(2)
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet10', ['fd:10:1::1', 'fd:10:1::5'], tunnel_name, route_ids=route2, nhg=nhg2_1)
        check_state_db_routes(dvs, 'Vnet10', "fd:10:20::1/128", ['fd:10:1::1', 'fd:10:1::5'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:10:20::1/128")

        # Update BFD state and check route nexthop
        update_bfd_session_state(dvs, 'fd:10:2::3', 'Down')
        update_bfd_session_state(dvs, 'fd:10:2::2', 'Up')
        time.sleep(2)

        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet10', ['fd:10:1::1', 'fd:10:1::2'], tunnel_name, route_ids=route1, nhg=nhg1_1)
        check_state_db_routes(dvs, 'Vnet10', "fd:10:10::1/128", ['fd:10:1::1', 'fd:10:1::2'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")

        # Set the route to a new group
        set_vnet_routes(dvs, "fd:10:10::1/128", 'Vnet10', 'fd:10:1::1,fd:10:1::2,fd:10:1::3,fd:10:1::4', ep_monitor='fd:10:2::1,fd:10:2::2,fd:10:2::3,fd:10:2::4')
        update_bfd_session_state(dvs, 'fd:10:2::4', 'Up')
        time.sleep(2)
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet10', ['fd:10:1::1', 'fd:10:1::2', 'fd:10:1::4'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, 'Vnet10', "fd:10:10::1/128", ['fd:10:1::1', 'fd:10:1::2', 'fd:10:1::4'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_1 not in vnet_obj.nhgs

        # Set BFD session state for a down endpoint to up
        update_bfd_session_state(dvs, 'fd:10:2::3', 'Up')
        time.sleep(2)
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet10', ['fd:10:1::1', 'fd:10:1::2', 'fd:10:1::3', 'fd:10:1::4'], tunnel_name, route_ids=route1, nhg=nhg1_2)
        check_state_db_routes(dvs, 'Vnet10', "fd:10:10::1/128", ['fd:10:1::1', 'fd:10:1::2', 'fd:10:1::3', 'fd:10:1::4'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")

        # Set all endpoint to down state
        update_bfd_session_state(dvs, 'fd:10:2::1', 'Down')
        update_bfd_session_state(dvs, 'fd:10:2::2', 'Down')
        update_bfd_session_state(dvs, 'fd:10:2::3', 'Down')
        update_bfd_session_state(dvs, 'fd:10:2::4', 'Down')
        time.sleep(2)

        # Confirm the tunnel route is updated in ASIC
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet10', ["fd:10:10::1/128"])
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet10', ['fd:10:1::5'], tunnel_name, route_ids=route2, nhg=nhg2_1)
        check_state_db_routes(dvs, 'Vnet10', "fd:10:20::1/128", ['fd:10:1::5'])
        check_state_db_routes(dvs, 'Vnet10', "fd:10:10::1/128", [])
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "fd:10:20::1/128")

        # Remove tunnel route2
        delete_vnet_routes(dvs, "fd:10:20::1/128", 'Vnet10')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet10', ["fd:10:20::1/128"])
        check_remove_state_db_routes(dvs, 'Vnet10', "fd:10:20::1/128")
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
        delete_vnet_routes(dvs, "fd:10:10::1/128", 'Vnet10')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet10', ["fd:10:10::1/128"])
        check_remove_state_db_routes(dvs, 'Vnet10', "fd:10:10::1/128")
        check_remove_routes_advertisement(dvs, "fd:10:10::1/128")

        # Confirm the BFD sessions are removed
        check_del_bfd_session(dvs, ['fd:10:2::1', 'fd:10:2::2', 'fd:10:2::3', 'fd:10:2::4', 'fd:10:2::5'])

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_2 not in vnet_obj.nhgs

        delete_vnet_entry(dvs, 'Vnet10')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet10')


    '''
    Test 11 - Test for vnet tunnel routes with both single endpoint and ECMP group with endpoint health monitor
    '''
    def test_vnet_orch_11(self, dvs, testlog):
        vnet_obj = self.get_vnet_obj()

        tunnel_name = 'tunnel_11'

        vnet_obj.fetch_exist_entries(dvs)

        create_vxlan_tunnel(dvs, tunnel_name, '11.11.11.11')
        create_vnet_entry(dvs, 'Vnet11', tunnel_name, '100011', "")

        vnet_obj.check_vnet_entry(dvs, 'Vnet11')
        vnet_obj.check_vxlan_tunnel_entry(dvs, tunnel_name, 'Vnet11', '100011')

        vnet_obj.check_vxlan_tunnel(dvs, tunnel_name, '11.11.11.11')

        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.1.1/32", 'Vnet11', '11.0.0.1', ep_monitor='11.1.0.1')

        # default bfd status is down, route should not be programmed in this status
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet11', ["100.100.1.1/32"])
        check_state_db_routes(dvs, 'Vnet11', "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Route should be properly configured when bfd session state goes up
        update_bfd_session_state(dvs, '11.1.0.1', 'Up')
        time.sleep(2)
        vnet_obj.check_vnet_routes(dvs, 'Vnet11', '11.0.0.1', tunnel_name)
        check_state_db_routes(dvs, 'Vnet11', "100.100.1.1/32", ['11.0.0.1'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Create another tunnel route with endpoint group overlapped with route1
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.2.1/32", 'Vnet11', '11.0.0.1,11.0.0.2', ep_monitor='11.1.0.1,11.1.0.2')
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet11', ['11.0.0.1'], tunnel_name)
        check_state_db_routes(dvs, 'Vnet11', "100.100.2.1/32", ['11.0.0.1'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        # Create a third tunnel route with another endpoint
        vnet_obj.fetch_exist_entries(dvs)
        create_vnet_routes(dvs, "100.100.3.1/32", 'Vnet11', '11.0.0.2', ep_monitor='11.1.0.2')

        # Update BFD session state and verify route change
        update_bfd_session_state(dvs, '11.1.0.2', 'Up')
        time.sleep(2)
        vnet_obj.check_vnet_routes(dvs, 'Vnet11', '11.0.0.2', tunnel_name)
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet11', ['11.0.0.1', '11.0.0.2'], tunnel_name, route_ids=route2, nhg=nhg2_1)
        check_state_db_routes(dvs, 'Vnet11', "100.100.3.1/32", ['11.0.0.2'])
        check_state_db_routes(dvs, 'Vnet11', "100.100.2.1/32", ['11.0.0.1', '11.0.0.2'])
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.3.1/32")

        update_bfd_session_state(dvs, '11.1.0.1', 'Down')
        time.sleep(2)
        route2, nhg2_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet11', ['11.0.0.2'], tunnel_name, route_ids=route2, nhg=nhg2_1)
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet11', ["100.100.1.1/32"])
        check_state_db_routes(dvs, 'Vnet11', "100.100.2.1/32", ['11.0.0.2'])
        check_state_db_routes(dvs, 'Vnet11', "100.100.1.1/32", [])
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        # Set the route1 to a new endpoint
        vnet_obj.fetch_exist_entries(dvs)
        set_vnet_routes(dvs, "100.100.1.1/32", 'Vnet11', '11.0.0.2', ep_monitor='11.1.0.2')
        vnet_obj.check_vnet_routes(dvs, 'Vnet11', '11.0.0.2', tunnel_name)
        check_state_db_routes(dvs, 'Vnet11', "100.100.3.1/32", ['11.0.0.2'])
        # The default Vnet setting does not advertise prefix
        check_remove_routes_advertisement(dvs, "100.100.3.1/32")

        # Remove tunnel route2
        delete_vnet_routes(dvs, "100.100.2.1/32", 'Vnet11')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet11', ["100.100.2.1/32"])
        check_remove_state_db_routes(dvs, 'Vnet11', "100.100.2.1/32")
        check_remove_routes_advertisement(dvs, "100.100.2.1/32")

        # Check the corresponding nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg2_1 not in vnet_obj.nhgs

        # Check the BFD session specific to the endpoint group is removed while others exist
        check_del_bfd_session(dvs, ['11.1.0.1'])
        check_bfd_session(dvs, ['11.1.0.2'])

        # Remove tunnel route 1
        delete_vnet_routes(dvs, "100.100.1.1/32", 'Vnet11')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet11', ["100.100.1.1/32"])
        check_remove_state_db_routes(dvs, 'Vnet11', "100.100.1.1/32")
        check_remove_routes_advertisement(dvs, "100.100.1.1/32")

        # Remove tunnel route 3
        delete_vnet_routes(dvs, "100.100.3.1/32", 'Vnet11')
        vnet_obj.check_del_vnet_routes(dvs, 'Vnet11', ["100.100.3.1/32"])
        check_remove_state_db_routes(dvs, 'Vnet11', "100.100.3.1/32")
        check_remove_routes_advertisement(dvs, "100.100.3.1/32")

        # Confirm the BFD sessions are removed
        check_del_bfd_session(dvs, ['11.1.0.1', '11.1.0.2'])

        delete_vnet_entry(dvs, 'Vnet11')
        vnet_obj.check_del_vnet_entry(dvs, 'Vnet11')


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
        create_vnet_routes(dvs, "100.100.1.1/32", 'Vnet12', '12.0.0.1,12.0.0.2,12.0.0.3', ep_monitor='12.1.0.1,12.1.0.2,12.1.0.3')

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
        check_routes_advertisement(dvs, "100.100.1.1/32")

        # Remove endpoint from group if it goes down
        update_bfd_session_state(dvs, '12.1.0.2', 'Down')
        time.sleep(2)
        route1, nhg1_1 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet12', ['12.0.0.1', '12.0.0.3'], tunnel_name, route_ids=route1, nhg=nhg1_1)
        check_state_db_routes(dvs, 'Vnet12', "100.100.1.1/32", ['12.0.0.1', '12.0.0.3'])
        check_routes_advertisement(dvs, "100.100.1.1/32")

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
        check_routes_advertisement(dvs, "100.100.1.1/32")

        # Set the route1 to a new group
        set_vnet_routes(dvs, "100.100.1.1/32", 'Vnet12', '12.0.0.1,12.0.0.2,12.0.0.3,12.0.0.4', ep_monitor='12.1.0.1,12.1.0.2,12.1.0.3,12.1.0.4')
        update_bfd_session_state(dvs, '12.1.0.4', 'Up')
        time.sleep(2)
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet12', ['12.0.0.1', '12.0.0.4'], tunnel_name, route_ids=route1)
        check_state_db_routes(dvs, 'Vnet12', "100.100.1.1/32", ['12.0.0.1', '12.0.0.4'])
        check_routes_advertisement(dvs, "100.100.1.1/32")

        # Check the previous nexthop group is removed
        vnet_obj.fetch_exist_entries(dvs)
        assert nhg1_1 not in vnet_obj.nhgs

        # Set BFD session state for a down endpoint to up
        update_bfd_session_state(dvs, '12.1.0.2', 'Up')
        time.sleep(2)
        route1, nhg1_2 = vnet_obj.check_vnet_ecmp_routes(dvs, 'Vnet12', ['12.0.0.1', '12.0.0.2', '12.0.0.4'], tunnel_name, route_ids=route1, nhg=nhg1_2)
        check_state_db_routes(dvs, 'Vnet12', "100.100.1.1/32", ['12.0.0.1', '12.0.0.2', '12.0.0.4'])
        check_routes_advertisement(dvs, "100.100.1.1/32")

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


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
