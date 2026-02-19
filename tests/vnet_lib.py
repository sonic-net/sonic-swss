import time
import ipaddress
import json
import time

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
    new_entries = list(entries - set(existed_entries))
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


def create_vnet_local_routes(dvs, prefix, vnet_name, ifname, nexthop=""):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    create_entry_tbl(
        conf_db,
        "VNET_ROUTE", '|', "%s|%s" % (vnet_name, prefix),
        [
            ("ifname", ifname),
            ("nexthop", nexthop),
        ]
    )

    time.sleep(2)


def delete_vnet_local_routes(dvs, prefix, vnet_name):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

    delete_entry_pst(app_db, "VNET_ROUTE_TABLE", "%s:%s" % (vnet_name, prefix))

    time.sleep(2)


def create_vnet_routes(dvs, prefix, vnet_name, endpoint, mac="", vni=0, ep_monitor="", profile="", primary="", monitoring="", rx_monitor_timer=-1, tx_monitor_timer=-1, adv_prefix="", check_directly_connected=False, metric=-1):
    set_vnet_routes(dvs, prefix, vnet_name, endpoint, mac=mac, vni=vni, ep_monitor=ep_monitor, profile=profile, primary=primary, monitoring=monitoring, rx_monitor_timer=rx_monitor_timer, tx_monitor_timer=tx_monitor_timer, adv_prefix=adv_prefix, check_directly_connected=check_directly_connected, pinned_state=pinned_state, metric=metric)


def set_vnet_routes(dvs, prefix, vnet_name, endpoint, mac="", vni=0, ep_monitor="", profile="", primary="", monitoring="", rx_monitor_timer=-1, tx_monitor_timer=-1, adv_prefix="", check_directly_connected=False, pinned_state="", metric=-1):
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

    if profile:
        attrs.append(('profile', profile))

    if primary:
        attrs.append(('primary', primary))

    if monitoring:
        attrs.append(('monitoring', monitoring))

    if adv_prefix:
        attrs.append(('adv_prefix', adv_prefix))

    if check_directly_connected:
        attrs.append(('check_directly_connected', 'true'))

    if rx_monitor_timer != -1:
        attrs.append(('rx_monitor_timer', str(rx_monitor_timer)))

    if tx_monitor_timer != -1:
        attrs.append(('tx_monitor_timer', str(tx_monitor_timer)))

    if pinned_state:
        attrs.append(('pinned_state', pinned_state))

    if metric >= 0:
        attrs.append(('metric', str(metric)))

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


def create_vnet_entry(dvs, name, tunnel, vni, peer_list, scope="", advertise_prefix=False, overlay_dmac=""):
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

    if overlay_dmac:
        attrs.append(('overlay_dmac', overlay_dmac))

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

def delete_vxlan_tunnel(dvs, name):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    delete_entry_tbl(conf_db, "VXLAN_TUNNEL", name)

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

def update_monitor_session_state(dvs, addr, monitor, state):
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
    create_entry_tbl(
        state_db,
        "VNET_MONITOR_TABLE", '|', "%s|%s" % (monitor,addr),
        [
            ("state", state),
        ]
    )

def get_bfd_session_id(dvs, addr):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    tbl =  swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION")
    entries = set(tbl.getKeys())
    for entry in entries:
        status, fvs = tbl.get(entry)
        fvs = dict(fvs)
        assert status, "Got an error when get a key"
        if fvs["SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS"] == addr and fvs["SAI_BFD_SESSION_ATTR_MULTIHOP"] == "true":
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


def check_routes_advertisement(dvs, prefix, profile=""):
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
    tbl =  swsscommon.Table(state_db, "ADVERTISE_NETWORK_TABLE")
    keys = tbl.getKeys()

    assert prefix in keys

    if profile:
        status, fvs = tbl.get(prefix)
        assert status, "Got an error when get a key"
        fvs = dict(fvs)
        assert fvs['profile'] == profile


def check_remove_routes_advertisement(dvs, prefix):
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
    tbl =  swsscommon.Table(state_db, "ADVERTISE_NETWORK_TABLE")
    keys = tbl.getKeys()

    assert prefix not in keys


def check_syslog(dvs, marker, err_log):
    (exitcode, num) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep \"%s\" | wc -l" % (marker, err_log)])
    assert num.strip() == "0"


def create_fvs(**kwargs):
    return swsscommon.FieldValuePairs(list(kwargs.items()))


def create_subnet_decap_tunnel(dvs, tunnel_name, **kwargs):
    """Create tunnel and verify all needed entries in state DB exists."""
    appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
    fvs = create_fvs(**kwargs)
    # create tunnel entry in DB
    ps = swsscommon.ProducerStateTable(appdb, "TUNNEL_DECAP_TABLE")
    ps.set(tunnel_name, fvs)

    # wait till config will be applied
    time.sleep(1)

    # validate the tunnel entry in state db
    tunnel_state_table = swsscommon.Table(statedb, "TUNNEL_DECAP_TABLE")

    tunnels = tunnel_state_table.getKeys()
    for tunnel in tunnels:
        status, fvs = tunnel_state_table.get(tunnel)
        assert status == True

        for field, value in fvs:
            if field == "tunnel_type":
                assert value == "IPINIP"
            elif field == "dscp_mode":
                assert value == kwargs["dscp_mode"]
            elif field == "ecn_mode":
                assert value == kwargs["ecn_mode"]
            elif field == "ttl_mode":
                assert value == kwargs["ttl_mode"]
            elif field == "encap_ecn_mode":
                assert value == kwargs["encap_ecn_mode"]
            else:
                assert False, "Field %s is not tested" % field


def delete_subnet_decap_tunnel(dvs, tunnel_name):
    """Delete tunnel and checks that state DB is cleared."""
    appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    statedb = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
    tunnel_app_table = swsscommon.Table(appdb, "TUNNEL_DECAP_TABLE")
    tunnel_state_table = swsscommon.Table(statedb, "TUNNEL_DECAP_TABLE")

    ps = swsscommon.ProducerStateTable(appdb, "TUNNEL_DECAP_TABLE")
    ps._del(tunnel_name)

    # wait till config will be applied
    time.sleep(1)

    assert len(tunnel_app_table.getKeys()) == 0
    assert len(tunnel_state_table.getKeys()) == 0


loopback_id = 0
def_vr_id = 0
switch_mac = None
# Creation of a physical interface should only add one route to 'ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY' (i.e., a route to the interface's IP).
# But after creating a VLAN interface, two entries should be added to this table: One for the VLAN interface's IP and one for the VLAN's subnet.
# Creating a VLAN in a VNet that is peered with another Vnet will create an additional entry for the VLAN's subnet in the peer Vnet.
intf_route_count = {"physical": 1, "vlan": 2, "vlan-one-peer": 3}

def update_bgp_global_dev_state(dvs, state):
    config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    create_entry_tbl(
        config_db,
        "BGP_DEVICE_GLOBAL",'|',"STATE",
        [
            ("tsa_enabled", state),
        ]
    )

def set_tsa(dvs):
    update_bgp_global_dev_state(dvs, "true")

def clear_tsa(dvs):
    update_bgp_global_dev_state(dvs, "false")

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
    APP_VNET_MONITOR        =  "VNET_MONITOR_TABLE"

    ecn_modes_map = {
        "standard"       : "SAI_TUNNEL_DECAP_ECN_MODE_STANDARD",
        "copy_from_outer": "SAI_TUNNEL_DECAP_ECN_MODE_COPY_FROM_OUTER"
    }

    dscp_modes_map = {
        "pipe"    : "SAI_TUNNEL_DSCP_MODE_PIPE_MODEL",
        "uniform" : "SAI_TUNNEL_DSCP_MODE_UNIFORM_MODEL"
    }

    ttl_modes_map = {
        "pipe"    : "SAI_TUNNEL_TTL_MODE_PIPE_MODEL",
        "uniform" : "SAI_TUNNEL_TTL_MODE_UNIFORM_MODEL"
    }

    def __init__(self):
        self.tunnel_map_ids       = set()
        self.tunnel_map_entry_ids = set()
        self.tunnel_ids           = set()
        self.tunnel_term_ids      = set()
        self.ipinip_tunnel_term_ids = {}
        self.tunnel_map_map       = {}
        self.tunnel               = {}
        self.vnet_vr_ids          = set()
        self.vr_map               = {}
        self.nh_ids               = {}
        self.nhg_ids              = {}

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
        self.nhgms = get_exist_entries(dvs, self.ASIC_NEXT_HOP_GROUP_MEMBER)
        self.bfd_sessions = get_exist_entries(dvs, self.ASIC_BFD_SESSION)

        global loopback_id, def_vr_id, switch_mac
        if not loopback_id:
            loopback_id = get_lo(dvs)

        if not def_vr_id:
            def_vr_id = get_default_vr_id(dvs)

        if switch_mac is None:
            switch_mac = get_switch_mac(dvs)

    def check_ipinip_tunnel(self, dvs, tunnel_name, dscp_mode, ecn_mode, ttl_mode):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tunnel_id = get_created_entry(asic_db, self.ASIC_TUNNEL_TABLE, self.tunnel_ids)
        tunnel_attrs = {
            'SAI_TUNNEL_ATTR_TYPE': 'SAI_TUNNEL_TYPE_IPINIP',
            'SAI_TUNNEL_ATTR_ENCAP_DSCP_MODE': self.dscp_modes_map[dscp_mode],
            'SAI_TUNNEL_ATTR_ENCAP_ECN_MODE': self.ecn_modes_map[ecn_mode],
            'SAI_TUNNEL_ATTR_ENCAP_TTL_MODE': self.ttl_modes_map[ttl_mode]
        }
        check_object(asic_db, self.ASIC_TUNNEL_TABLE, tunnel_id, tunnel_attrs)

        self.tunnel_ids.add(tunnel_id)
        self.tunnel[tunnel_name] = tunnel_id

    def check_del_ipinip_tunnel(self, dvs, tunnel_name):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        tunnel_id = get_deleted_entries(asic_db, self.ASIC_TUNNEL_TABLE, self.tunnel_ids, 1)[0]
        check_deleted_object(asic_db, self.ASIC_TUNNEL_TABLE, tunnel_id)
        self.tunnel_ids.remove(tunnel_id)
        assert tunnel_id == self.tunnel[tunnel_name]
        self.tunnel.pop(tunnel_name)

    def check_ipinip_tunnel_decap_term(self, dvs, tunnel_name, dst_ip, src_ip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        dst_ip = ipaddress.ip_network(dst_ip)
        src_ip = ipaddress.ip_network(src_ip)
        tunnel_term_id = get_created_entry(asic_db, self.ASIC_TUNNEL_TERM_ENTRY, self.tunnel_term_ids)
        tunnel_term_attrs = {
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE': 'SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_MP2MP',
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE': 'SAI_TUNNEL_TYPE_IPINIP',
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP': str(dst_ip.network_address),
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP_MASK': str(dst_ip.netmask),
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP': str(src_ip.network_address),
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP_MASK': str(src_ip.netmask),
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID': self.tunnel[tunnel_name]
        }
        check_object(asic_db, self.ASIC_TUNNEL_TERM_ENTRY, tunnel_term_id, tunnel_term_attrs)

        self.tunnel_term_ids.add(tunnel_term_id)
        self.ipinip_tunnel_term_ids[(tunnel_name, src_ip, dst_ip)] = tunnel_term_id

    def check_del_ipinip_tunnel_decap_term(self, dvs, tunnel_name, dst_ip, src_ip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        dst_ip = ipaddress.ip_network(dst_ip)
        src_ip = ipaddress.ip_network(src_ip)
        tunnel_term_id = get_deleted_entries(asic_db, self.ASIC_TUNNEL_TERM_ENTRY, self.tunnel_term_ids, 1)[0]
        check_deleted_object(asic_db, self.ASIC_TUNNEL_TERM_ENTRY, tunnel_term_id)
        self.tunnel_term_ids.remove(tunnel_term_id)
        assert self.ipinip_tunnel_term_ids[(tunnel_name, src_ip, dst_ip)] == tunnel_term_id
        self.ipinip_tunnel_term_ids.pop((tunnel_name, src_ip, dst_ip))

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

    def check_del_vxlan_tunnel(self, dvs):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        old_tunnel = get_deleted_entries(asic_db, self.ASIC_TUNNEL_TABLE, self.tunnel_ids, 1)
        check_deleted_object(asic_db, self.ASIC_TUNNEL_TABLE, old_tunnel[0])
        self.tunnel_ids.remove(old_tunnel[0])

        old_tunnel_maps = get_deleted_entries(asic_db, self.ASIC_TUNNEL_MAP, self.tunnel_map_ids, 4)
        for old_tunnel_map in old_tunnel_maps:
            check_deleted_object(asic_db, self.ASIC_TUNNEL_MAP, old_tunnel_map)
            self.tunnel_map_ids.remove(old_tunnel_map)

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

    def check_router_interface(self, dvs, intf_name, name, vlan_oid=0, intf_type="physical"):
        '''
            :param str intf_type Indicates whether the interface named 'intf_name' is a physical interface,
            a VLAN interface in a peerless VNet, or a VLAN interface in a VNet with one peer. This is important since
            it determines how many new routes we should expect to be added to the 'ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY'
            table after the creation of the interface. Entries for more than one peers are currently not added
            to the 'intf_route_count' dictionary since no test uses them.
        '''
        # Check RIF in ingress VRF
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        global switch_mac
        global intf_route_count

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
        new_route = get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, intf_route_count[intf_type])

        if vlan_oid:
            expected_attr = { 'SAI_VLAN_ATTR_BROADCAST_FLOOD_CONTROL_TYPE': 'SAI_VLAN_FLOOD_CONTROL_TYPE_NONE' }
            check_object(asic_db, self.ASIC_VLAN_TABLE, vlan_oid, expected_attr)

            expected_attr = { 'SAI_VLAN_ATTR_UNKNOWN_MULTICAST_FLOOD_CONTROL_TYPE': 'SAI_VLAN_FLOOD_CONTROL_TYPE_NONE' }
            check_object(asic_db, self.ASIC_VLAN_TABLE, vlan_oid, expected_attr)

        check_linux_intf_arp_proxy(dvs, intf_name)

        self.rifs.add(new_rif)
        self.routes.update(new_route)

    def check_del_router_interface(self, dvs, name):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        old_rif = get_deleted_entries(asic_db, self.ASIC_RIF_TABLE, self.rifs, 1)
        check_deleted_object(asic_db, self.ASIC_RIF_TABLE, old_rif[0])

        self.rifs.remove(old_rif[0])

    def check_default_vrf_route(self, dvs, ip_pref):
        global def_vr_id

        def _access_function():
            route_entries = get_exist_entries(dvs, self.ASIC_ROUTE_ENTRY)
            for route_entry in route_entries:
                json_rt_entry = json.loads(route_entry)
                if (json_rt_entry["dest"] == ip_pref and json_rt_entry['vr'] == def_vr_id):
                    return (True, None)
                else:
                    continue
            return (False, None)

        wait_for_result(_access_function)

    def check_vnet_local_routes(self, dvs, name, vlan_subnet_route=False):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        vr_ids = self.vnet_route_ids(dvs, name, True)
        count = len(vr_ids)
        # The route to the VLAN subnet must have been added when the VLAN was created.
        # We are not expecting any new routes in that case.
        expected_route_count = 0 if vlan_subnet_route else count
        new_route = get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, expected_route_count)

        #Routes are not replicated to egress VRF, return if count is 0, else check peering
        if not expected_route_count:
            return

        asic_vrs = set()
        for idx in range(expected_route_count):
            rt_key = json.loads(new_route[idx])
            asic_vrs.add(rt_key['vr'])

        assert asic_vrs == vr_ids

        self.routes.update(new_route)
        return new_route

    def check_vnet_local_route_nexthops(self, dvs, route_key, nexthops=[]):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        route_tbl = swsscommon.Table(asic_db, self.ASIC_ROUTE_ENTRY)
        nhgm_tbl = swsscommon.Table(asic_db, self.ASIC_NEXT_HOP_GROUP_MEMBER)
        nh_tbl = swsscommon.Table(asic_db, self.ASIC_NEXT_HOP)
                
        # Verify next hop or next hop group
        status, route_fvs = route_tbl.get(route_key)
        route_fvs = dict(route_fvs)
        assert status, "Error occurred when trying to get a route entry key"

        if (len(nexthops) == 1):
            expected_attr = {
                "SAI_NEXT_HOP_ATTR_IP": nexthops[0]
            }

            # Check next hop entry
            nh_key = route_fvs.get("SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID")
            assert nh_key, "Route entry does not have a next hop"
            check_object(asic_db, self.ASIC_NEXT_HOP, nh_key, expected_attr)

        elif (len(nexthops) > 1):
            expected_attr = {
                "SAI_NEXT_HOP_GROUP_ATTR_TYPE": "SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP",
            }

            # Check next hop group entry
            nhg_key = route_fvs.get("SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID")
            assert nhg_key, "Route entry does not have a next hop"
            check_object(asic_db, self.ASIC_NEXT_HOP_GROUP, nhg_key, expected_attr)

            # Check next hop group members
            remaining_nexthops = set(nexthops)
            nhgms = get_exist_entries(dvs, self.ASIC_NEXT_HOP_GROUP_MEMBER)
            for nhgm in nhgms:
                status, fvs = nhgm_tbl.get(nhgm)
                fvs = dict(fvs)
                assert status, "Error occurred when trying to get a next hop group member key"

                if fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhg_key:
                    nh_key = fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"]
                    status, nh_fvs = nh_tbl.get(nh_key)
                    nh_fvs = dict(nh_fvs)
                    assert status, "Error occurred when trying to get a next hop key"
                    assert nh_fvs["SAI_NEXT_HOP_ATTR_IP"] in remaining_nexthops, "Next hop in next hop group member not expected"
                    remaining_nexthops.remove(nh_fvs["SAI_NEXT_HOP_ATTR_IP"])

            assert len(remaining_nexthops) == 0, "Not all nexthops are associated with the next hop group"

    def check_del_vnet_local_routes(self, dvs, name, prefix):
        # TODO: Implement for VRF VNET
        vr_ids = self.vnet_route_ids(dvs, name, True)

        routes = get_exist_entries(dvs, self.ASIC_ROUTE_ENTRY)

        for route in routes:
            rt_key = json.loads(route)
            assert rt_key['vr'] not in vr_ids or rt_key['dest'] != prefix, "The route %s in VRF %s is not deleted" % (prefix, name)

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

    def check_next_hop_group_member(self, dvs, nhg, ordered_ecmp, expected_endpoint, expected_attrs):
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
                if ordered_ecmp == "true":
                    assert fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID"] == expected_attrs[endpoint]['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID']
                    del expected_attrs[endpoint]['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID']
                else:
                    assert fvs.get("SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID") is None

                check_object(asic_db, self.ASIC_NEXT_HOP, nh_key, expected_attrs[endpoint])

        assert self.serialize_endpoint_group(endpoints) == expected_endpoint_str

    def get_nexthop_groups(self, dvs, nhg):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        tbl_nhgm =  swsscommon.Table(asic_db, self.ASIC_NEXT_HOP_GROUP_MEMBER)
        tbl_nh =  swsscommon.Table(asic_db, self.ASIC_NEXT_HOP)
        nhg_data = {}
        nhg_data['id'] = nhg
        entries = set(tbl_nhgm.getKeys())
        nhg_data['endpoints'] = []
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
                nhg_data['endpoints'].append(endpoint)
        return nhg_data
    def check_vnet_ecmp_routes(self, dvs, name, endpoints, tunnel, mac=[], vni=[], route_ids=[], nhg="", ordered_ecmp="false", nh_seq_id=None):
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
            if ordered_ecmp == "true" and nh_seq_id:
                expected_attr.update({'SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID': nh_seq_id[idx]})
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
                        "SAI_NEXT_HOP_GROUP_ATTR_TYPE": "SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP" if ordered_ecmp == "false" else "SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_ORDERED_ECMP",
                    }
        check_object(asic_db, self.ASIC_NEXT_HOP_GROUP, new_nhg, expected_nhg_attr)

        # Check nexthop group member
        self.check_next_hop_group_member(dvs, new_nhg, ordered_ecmp, endpoints, expected_attrs)

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

    def check_priority_vnet_ecmp_routes(self, dvs, name, endpoints_primary, tunnel, mac=[], vni=[], route_ids=[], count =1, prefix =""):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        endpoint_str_primary = name + "|" + self.serialize_endpoint_group(endpoints_primary)
        new_nhgs = []
        expected_attrs_primary = {}
        for idx, endpoint in enumerate(endpoints_primary):
            expected_attr = {
                        "SAI_NEXT_HOP_ATTR_TYPE": "SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP",
                        "SAI_NEXT_HOP_ATTR_IP": endpoint,
                        "SAI_NEXT_HOP_ATTR_TUNNEL_ID": self.tunnel[tunnel],
                    }
            if vni and vni[idx]:
                expected_attr.update({'SAI_NEXT_HOP_ATTR_TUNNEL_VNI': vni[idx]})
            if mac and mac[idx]:
                expected_attr.update({'SAI_NEXT_HOP_ATTR_TUNNEL_MAC': mac[idx]})
            expected_attrs_primary[endpoint] = expected_attr

        if len(endpoints_primary) == 1:
            if route_ids:
                new_route = route_ids
            else:
                new_route = get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, count)
            return new_route
        else :
            new_nhgs = get_all_created_entries(asic_db, self.ASIC_NEXT_HOP_GROUP, self.nhgs)
            found_match = False

            for nhg in new_nhgs:
                nhg_data = self.get_nexthop_groups(dvs, nhg)
                eplist = self.serialize_endpoint_group(nhg_data['endpoints'])
                if eplist == self.serialize_endpoint_group(endpoints_primary):
                    self.nhg_ids[endpoint_str_primary] = nhg
                    found_match = True

            assert found_match, "the expected Nexthop group was not found."

            # Check routes in ingress VRF
            expected_nhg_attr = {
                            "SAI_NEXT_HOP_GROUP_ATTR_TYPE": "SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP",
                        }
            for nhg in new_nhgs:
                check_object(asic_db, self.ASIC_NEXT_HOP_GROUP, nhg, expected_nhg_attr)

            # Check nexthop group member
            self.check_next_hop_group_member(dvs, self.nhg_ids[endpoint_str_primary], "false", endpoints_primary, expected_attrs_primary)

            if route_ids:
                new_route = route_ids
            else:
                new_route = get_created_entries(asic_db, self.ASIC_ROUTE_ENTRY, self.routes, count)

            #Check if the route is in expected VRF
            active_nhg = self.nhg_ids[endpoint_str_primary]
            for idx in range(count):
                if prefix != "" and prefix not in new_route[idx] :
                    continue
                check_object(asic_db, self.ASIC_ROUTE_ENTRY, new_route[idx],
                            {
                                "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID": active_nhg,
                            }
                        )
                rt_key = json.loads(new_route[idx])


            self.routes.update(new_route)
            del self.nhg_ids[endpoint_str_primary]
            return new_route

    def check_del_vnet_route_in_vnet(self, dvs, vnet_name, prefix):
        vr_id = self.vr_map[vnet_name].get('ing')

        def _access_function():
            route_entries = get_exist_entries(dvs, self.ASIC_ROUTE_ENTRY)
            for route_entry in route_entries:
                rt_json = json.loads(route_entry)
                if rt_json['vr'] == vr_id:
                    if rt_json['dest'] == prefix:
                        return (False, None)
            return (True, None)
        wait_for_result(_access_function)

    def check_del_vnet_routes(self, dvs, name, prefixes=[], absent=False):
        # TODO: Implement for VRF VNET

        def _access_function():
            route_entries = get_exist_entries(dvs, self.ASIC_ROUTE_ENTRY)
            route_prefixes = [json.loads(route_entry)["dest"] for route_entry in route_entries]
            return (all(prefix not in route_prefixes for prefix in prefixes), None)

        if absent:
            return True if _access_function()== None else False
        elif prefixes:
            wait_for_result(_access_function)

        return True

    def check_custom_monitor_app_db(self, dvs, prefix, endpoint, packet_type, overlay_dmac):
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        key = endpoint + ':' + prefix
        check_object(app_db, self.APP_VNET_MONITOR, key,
            {
                "packet_type": packet_type,
                "overlay_dmac" : overlay_dmac
            }
        )
        return True

    def check_custom_monitor_deleted(self, dvs, prefix, endpoint):
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        key = endpoint + ':' + prefix
        check_deleted_object(app_db, self.APP_VNET_MONITOR, key)
