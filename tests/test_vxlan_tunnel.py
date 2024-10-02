import time
import json
import random
import time
import pytest

from swsscommon import swsscommon
from pprint import pprint


def create_entry(tbl, key, pairs):
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)

    # FIXME: better to wait until DB create them
    time.sleep(1)


def create_entry_tbl(db, table, separator, key, pairs):
    tbl = swsscommon.Table(db, table)
    create_entry(tbl, key, pairs)


def create_entry_pst(db, table, separator, key, pairs):
    tbl = swsscommon.ProducerStateTable(db, table)
    create_entry(tbl, key, pairs)


def delete_entry_pst(db, table, key):
    tbl = swsscommon.ProducerStateTable(db, table)
    tbl._del(key)
    time.sleep(1)


def delete_entry_tbl(db, table, key):
    tbl = swsscommon.Table(db, table)
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

def get_created_entry_mapid(db, table, existed_entries):
    tbl =  swsscommon.Table(db, table)
    entries = set(tbl.getKeys())
    new_entries = list(entries - existed_entries)
    new_entries.sort()
    return new_entries

def get_created_entry(db, table, existed_entries):
    tbl =  swsscommon.Table(db, table)
    entries = set(tbl.getKeys())
    new_entries = list(entries - existed_entries)
    assert len(new_entries) == 1, "Wrong number of created entries."
    return new_entries[0]


def get_default_vr_id(db):
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

    assert len(fvs) == len(expected_attributes), "Unexpected number of attributes"

    attr_keys = {entry[0] for entry in fvs}

    for name, value in fvs:
        assert expected_attributes[name] == value, "Wrong value %s for the attribute %s = %s" % \
                                                   (value, name, expected_attributes[name])

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

    vlan_ids.add(vlan_oid)

    return


def check_vxlan_tunnel(dvs, src_ip, dst_ip, tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids, lo_id):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    tunnel_map_id  = get_created_entry_mapid(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP", tunnel_map_ids)
    tunnel_id      = get_created_entry(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", tunnel_ids)
    tunnel_term_id = get_created_entry(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY", tunnel_term_ids)

    # check that the vxlan tunnel termination are there
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP") == (len(tunnel_map_ids) + 4), "The TUNNEL_MAP wasn't created"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY") == len(tunnel_map_entry_ids), "The TUNNEL_MAP_ENTRY is created too early"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL") == (len(tunnel_ids) + 1), "The TUNNEL wasn't created"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY") == (len(tunnel_term_ids) + 1), "The TUNNEL_TERM_TABLE_ENTRY wasm't created"

    default_vr_id = get_default_vr_id(asic_db)

    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP", tunnel_map_id[0],
                        {
                            'SAI_TUNNEL_MAP_ATTR_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID',
                        }
                )

    decapstr = '2:' + tunnel_map_id[0] + ',' + tunnel_map_id[2]
    encapstr = '2:' + tunnel_map_id[1] + ',' + tunnel_map_id[3]

    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", tunnel_id,
                    {
                        'SAI_TUNNEL_ATTR_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
                        'SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE': lo_id,
                        'SAI_TUNNEL_ATTR_DECAP_MAPPERS': decapstr,
                        'SAI_TUNNEL_ATTR_ENCAP_MAPPERS': encapstr,
                        'SAI_TUNNEL_ATTR_PEER_MODE': 'SAI_TUNNEL_PEER_MODE_P2MP',
                        'SAI_TUNNEL_ATTR_ENCAP_SRC_IP': src_ip
                    }
                )

    tunnel_type = 'SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2P' if dst_ip != '0.0.0.0' else 'SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP'
    expected_attributes = {
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE': tunnel_type,
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID': default_vr_id,
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP': src_ip,
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID': tunnel_id,
    }

    if dst_ip != '0.0.0.0':
        expected_attributes['SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP'] = dst_ip

    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY", tunnel_term_id, expected_attributes)

    tunnel_map_ids.update(tunnel_map_id)
    tunnel_ids.add(tunnel_id)
    tunnel_term_ids.add(tunnel_term_id)

    return tunnel_map_id[0]


def create_vxlan_tunnel(dvs, name, src_ip, dst_ip, tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids, lo_id, skip_dst_ip=False):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    # check the source information
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP") == len(tunnel_map_ids), "The initial state is incorrect"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY") == len(tunnel_map_entry_ids), "The initial state is incorrect"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL") == len(tunnel_ids), "The initial state is incorrect"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY") == len(tunnel_term_ids), "The initial state is incorrect"

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


def create_vxlan_tunnel_entry(dvs, tunnel_name, tunnel_map_entry_name, tunnel_map_map, vlan, vni_id,
                              tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    # Check source information
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP") == len(tunnel_map_ids), "The initial state is incorrect"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY") == len(tunnel_map_entry_ids), "The initial state is incorrect"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL") == len(tunnel_ids), "The initial state is incorrect"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY") == len(tunnel_term_ids), "The initial state is incorrect"

    # create the VXLAN tunnel Term entry in Config DB
    create_entry_tbl(
        conf_db,
        "VXLAN_TUNNEL_MAP", '|', "%s|%s" % (tunnel_name, tunnel_map_entry_name),
        [
            ("vni",  vni_id),
            ("vlan", vlan),
        ],
    )

    if (tunnel_map_map.get(tunnel_name) is None):
        tunnel_map_id = get_created_entry_mapid(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP", tunnel_map_ids)
        vni_vlan_map_id = tunnel_map_id[0]
    else:
        vni_vlan_map_id = tunnel_map_map[tunnel_name]

    tunnel_map_entry_id = get_created_entry(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY", tunnel_map_entry_ids)

    # check that the vxlan tunnel termination are there
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY") == (len(tunnel_map_entry_ids) + 1), "The TUNNEL_MAP_ENTRY is created too early"

    vlan_id = vlan[4:]
    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY", tunnel_map_entry_id,
        {
            'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID',
            'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': vni_vlan_map_id,
            'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY': vni_id,
            'SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE': vlan_id,
        }
    )

    tunnel_map_entry_ids.add(tunnel_map_entry_id)

    return

def get_lo(dvs):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    vr_id = get_default_vr_id(asic_db)

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


class TestVxlan(object):
    def test_vxlan_term_orch(self, dvs, testlog):
        tunnel_map_ids       = set()
        tunnel_map_entry_ids = set()
        tunnel_ids           = set()
        tunnel_term_ids      = set()
        tunnel_map_map       = {}
        vlan_ids             = get_exist_entries(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        loopback_id          = get_lo(dvs)

        create_vlan(dvs, "Vlan50", vlan_ids)
        create_vlan(dvs, "Vlan51", vlan_ids)
        create_vlan(dvs, "Vlan52", vlan_ids)
        create_vlan(dvs, "Vlan53", vlan_ids)
        create_vlan(dvs, "Vlan54", vlan_ids)
        create_vlan(dvs, "Vlan55", vlan_ids)
        create_vlan(dvs, "Vlan56", vlan_ids)
        create_vlan(dvs, "Vlan57", vlan_ids)

        create_vxlan_tunnel(dvs, 'tunnel_1', '10.0.0.1', '100.100.100.1',
                                  tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids, loopback_id)

        create_vxlan_tunnel_entry(dvs, 'tunnel_1', 'entry_1', tunnel_map_map, 'Vlan50', '850',
                                  tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

        tunnel_map_map['tunnel_1'] = check_vxlan_tunnel(dvs,'10.0.0.1', '100.100.100.1',
                                  tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids, loopback_id)

        create_vxlan_tunnel(dvs, 'tunnel_2', '11.0.0.2', '101.101.101.2',
                                  tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids, loopback_id)

        create_vxlan_tunnel_entry(dvs, 'tunnel_2', 'entry_1', tunnel_map_map, 'Vlan51', '851',
                                  tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

        tunnel_map_map['tunnel_2'] = check_vxlan_tunnel(dvs,'11.0.0.2', '101.101.101.2',
                                  tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids, loopback_id)

        create_vxlan_tunnel(dvs, 'tunnel_3', '12.0.0.3', '0.0.0.0',
                                  tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids, loopback_id)

        create_vxlan_tunnel_entry(dvs, 'tunnel_3', 'entry_1', tunnel_map_map, 'Vlan52', '852',
                                  tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

        tunnel_map_map['tunnel_3'] = check_vxlan_tunnel(dvs, '12.0.0.3', '0.0.0.0',
                                  tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids, loopback_id)

        create_vxlan_tunnel(dvs, 'tunnel_4', '15.0.0.5', '0.0.0.0',
                                  tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids, loopback_id, True)

        create_vxlan_tunnel_entry(dvs, 'tunnel_4', 'entry_1', tunnel_map_map, 'Vlan53', '853',
                                  tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

        tunnel_map_map['tunnel_4'] = check_vxlan_tunnel(dvs, '15.0.0.5', '0.0.0.0',
                                  tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids, loopback_id)

        create_vxlan_tunnel_entry(dvs, 'tunnel_1', 'entry_2', tunnel_map_map, 'Vlan54', '854',
                                  tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

        create_vxlan_tunnel_entry(dvs, 'tunnel_2', 'entry_2', tunnel_map_map, 'Vlan55', '855',
                                  tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

        create_vxlan_tunnel_entry(dvs, 'tunnel_3', 'entry_2', tunnel_map_map, 'Vlan56', '856',
                                  tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

        create_vxlan_tunnel_entry(dvs, 'tunnel_4', 'entry_2', tunnel_map_map, 'Vlan57', '857',
                                  tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

def apply_test_vnet_cfg(cfg_db):

    # create VXLAN Tunnel
    create_entry_tbl(
        cfg_db,
        "VXLAN_TUNNEL", '|', "tunnel1",
        [
            ("src_ip", "1.1.1.1")
        ],
    )

    # create VNET
    create_entry_tbl(
        cfg_db,
        "VNET", '|', "tunnel1",
        [
            ("vxlan_tunnel", "tunnel1"),
            ("vni", "1")
        ],
    )

    return 


@pytest.fixture
def env_setup(dvs):
    cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

    create_entry_pst(
        app_db,
        "SWITCH_TABLE", ':', "switch",
        [
            ("vxlan_router_mac", "00:01:02:03:04:05")
        ],
    )

    apply_test_vnet_cfg(cfg_db)

    yield 

    delete_entry_pst(app_db, "SWITCH_TABLE", "switch")
    delete_entry_tbl(cfg_db, "VXLAN_TUNNEL", "tunnel1")
    delete_entry_tbl(cfg_db, "VNET", "Vnet1")

def test_vnet_cleanup_config_reload(dvs, env_setup):

    # Restart vxlanmgrd Process
    dvs.runcmd(["systemctl", "restart", "vxlanmgrd"])

    # Reapply cfg to simulate cfg reload
    cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    apply_test_vnet_cfg(cfg_db)

    time.sleep(0.5)

    # Check if the netdevices is created as expected
    ret, stdout = dvs.runcmd(["ip", "link", "show"])
    assert "Vxlan1" in stdout
    assert "Brvxlan1" in stdout

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
