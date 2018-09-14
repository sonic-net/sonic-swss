from swsscommon import swsscommon
import time
import json
import random
import time
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


def create_vxlan_tunnel(dvs, name, src_ip, dst_ip, tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids, skip_dst_ip=False):
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

    tunnel_map_id  = get_created_entry(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP", tunnel_map_ids)
    tunnel_id      = get_created_entry(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", tunnel_ids)
    tunnel_term_id = get_created_entry(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY", tunnel_term_ids)

    # check that the vxlan tunnel termination are there
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP") == (len(tunnel_map_ids) + 1), "The TUNNEL_MAP wasn't created"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY") == len(tunnel_map_entry_ids), "The TUNNEL_MAP_ENTRY is created too early"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL") == (len(tunnel_ids) + 1), "The TUNNEL wasn't created"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY") == (len(tunnel_term_ids) + 1), "The TUNNEL_TERM_TABLE_ENTRY wasm't created"

    default_vr_id = get_default_vr_id(asic_db)

    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP", tunnel_map_id,
                        {
                            'SAI_TUNNEL_MAP_ATTR_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID',
                        }
                )

# FIXME: !!!
#    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL", tunnel_id,
#                    {
#                        'SAI_TUNNEL_ATTR_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
#                        'SAI_TUNNEL_ATTR_DECAP_MAPPERS': '1:%s' % tunnel_map_id,
#                    }
#                )

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

    tunnel_map_ids.add(tunnel_map_id)
    tunnel_ids.add(tunnel_id)
    tunnel_term_ids.add(tunnel_term_id)

    return tunnel_map_id

def create_vxlan_tunnel_entry(dvs, tunnel_name, tunnel_map_entry_name, tunnel_map_id, vlan, vni_id,
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

    tunnel_map_entry_id = get_created_entry(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY", tunnel_map_entry_ids)

    # check that the vxlan tunnel termination are there
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP") == len(tunnel_map_ids), "The TUNNEL_MAP wasn't created"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY") == (len(tunnel_map_entry_ids) + 1), "The TUNNEL_MAP_ENTRY is created too early"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL") == len(tunnel_ids), "The TUNNEL wasn't created"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY") == len(tunnel_term_ids), "The TUNNEL_TERM_TABLE_ENTRY wasm't created"

    vlan_id = vlan[4:]
    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY", tunnel_map_entry_id,
        {
            'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID',
            'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': tunnel_map_id,
            'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY': vni_id,
            'SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE': vlan_id,
        }
    )

    tunnel_map_entry_ids.add(tunnel_map_entry_id)

    return


def test_vxlan_term_orch(dvs):
    return
    tunnel_map_ids       = set()
    tunnel_map_entry_ids = set()
    tunnel_ids           = set()
    tunnel_term_ids      = set()
    tunnel_map_map       = {}
    vlan_ids             = get_exist_entries(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")

    create_vlan(dvs, "Vlan50", vlan_ids)
    create_vlan(dvs, "Vlan51", vlan_ids)
    create_vlan(dvs, "Vlan52", vlan_ids)
    create_vlan(dvs, "Vlan53", vlan_ids)
    create_vlan(dvs, "Vlan54", vlan_ids)
    create_vlan(dvs, "Vlan55", vlan_ids)
    create_vlan(dvs, "Vlan56", vlan_ids)
    create_vlan(dvs, "Vlan57", vlan_ids)

    tunnel_map_map['tunnel_1'] = create_vxlan_tunnel(dvs, 'tunnel_1', '10.0.0.1', '100.100.100.1',
                                                     tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

    create_vxlan_tunnel_entry(dvs, 'tunnel_1', 'entry_1', tunnel_map_map['tunnel_1'], 'Vlan50', '850',
                              tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

    tunnel_map_map['tunnel_2'] = create_vxlan_tunnel(dvs, 'tunnel_2', '11.0.0.2', '101.101.101.2',
                                                     tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

    create_vxlan_tunnel_entry(dvs, 'tunnel_2', 'entry_1', tunnel_map_map['tunnel_2'], 'Vlan51', '851',
                              tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

    tunnel_map_map['tunnel_3'] = create_vxlan_tunnel(dvs, 'tunnel_3', '12.0.0.3', '0.0.0.0',
                                                     tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

    create_vxlan_tunnel_entry(dvs, 'tunnel_3', 'entry_1', tunnel_map_map['tunnel_3'], 'Vlan52', '852',
                              tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

    tunnel_map_map['tunnel_4'] = create_vxlan_tunnel(dvs, 'tunnel_4', '15.0.0.5', '0.0.0.0',
                                                     tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids, True)

    create_vxlan_tunnel_entry(dvs, 'tunnel_4', 'entry_1', tunnel_map_map['tunnel_4'], 'Vlan53', '853',
                              tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

    create_vxlan_tunnel_entry(dvs, 'tunnel_1', 'entry_2', tunnel_map_map['tunnel_1'], 'Vlan54', '854',
                              tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

    create_vxlan_tunnel_entry(dvs, 'tunnel_2', 'entry_2', tunnel_map_map['tunnel_2'], 'Vlan55', '855',
                              tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

    create_vxlan_tunnel_entry(dvs, 'tunnel_3', 'entry_2', tunnel_map_map['tunnel_3'], 'Vlan56', '856',
                              tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)

    create_vxlan_tunnel_entry(dvs, 'tunnel_4', 'entry_2', tunnel_map_map['tunnel_4'], 'Vlan57', '857',
                              tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids)
