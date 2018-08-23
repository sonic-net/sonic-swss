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


def get_default_vr_id(db):
    table = 'ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER'
    tbl =  swsscommon.Table(db, table)
    keys = tbl.getKeys()
    assert len(keys) == 1, "Wrong number of virtual routers found"

    return keys[0]


def check_object(db, table, expected_attributes):
    tbl =  swsscommon.Table(db, table)
    keys = tbl.getKeys()
    assert len(keys) == 1, "Wrong number of keys found"

    key = keys[0]
    status, fvs = tbl.get(key)
    assert status, "Got an error when get a key"

    assert len(fvs) == len(expected_attributes), "Unexpected number of attributes"

    attr_keys = {entry[0] for entry in fvs}

    for name, value in fvs:
        assert expected_attributes[name] == value, "Wrong value %s for the attribute %s = %s" % \
                                                   (value, name, expected_attributes[name])

    return key


def create_vxlan_tunnel_term_orch(dvs, ip_address, vni, vlan):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    
    # check that the objects don't exist
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP") == 0, "The initial state is incorrect"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY") == 0, "The initial state is incorrect"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL") == 0, "The initial state is incorrect"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY") == 0, "The initial state is incorrect"

    # create the VXLAN tunnel Term entry in Config DB
    create_entry_tbl(
        conf_db,
        "VXLAN_TUNNEL_DECAP", '|', "%s|%s" % (ip_address, vni),
        [
            ("vlan", vlan),
        ],
    )

    # check that the vxlan tunnel termination are there
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP") == 1, "The TUNNEL_MAP wasn't created"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY") == 1, "The TUNNEL_MAP_ENTRY wasn't created"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL") == 1, "The TUNNEL wasn't created"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY") == 1, "The TUNNEL_TERM_TABLE_ENTRY wasm't created"

    default_vr_id = get_default_vr_id(asic_db)
    vlan_id = vlan[4:]

    tunnel_map_id = check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP",
                        {
                            'SAI_TUNNEL_MAP_ATTR_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID',
                        }
                    )

    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY",
        {
            'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID',
            'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': tunnel_map_id,
            'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY': vni,
            'SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE': vlan_id,
        }
    )

    tunnel_id = check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL",
                    {
                        'SAI_TUNNEL_ATTR_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
                        'SAI_TUNNEL_ATTR_DECAP_MAPPERS': '1:%s' % tunnel_map_id,
                    }
                )

    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY",
        {
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE': 'SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP',
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID': default_vr_id,
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP': ip_address,
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
            'SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID': tunnel_id,
        }
    )


def test_vxlan_term_orch(dvs):
#    create_vxlan_tunnel_term_orch(dvs, '10.1.2.3', '2001', 'Vlan50')
    pass

