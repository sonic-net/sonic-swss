import time
import json
import random
import time
import pytest

from swsscommon import swsscommon
from pprint import pprint

def get_exist_entries(dvs, table):
    db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    tbl =  swsscommon.Table(db, table)
    return set(tbl.getKeys())


def create_entry(tbl, key, pairs):
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)

    # FIXME: better to wait until DB create them
    time.sleep(1)


def create_entry_tbl(db, table, separator, key, pairs):
    tbl = swsscommon.Table(db, table)
    create_entry(tbl, key, pairs)


def get_created_entry(db, table, existed_entries):
    tbl =  swsscommon.Table(db, table)
    entries = set(tbl.getKeys())
    new_entries = list(entries - existed_entries)
    assert len(new_entries) == 1, "Wrong number of created entries."
    return new_entries[0]


def how_many_entries_exist(db, table):
    tbl =  swsscommon.Table(db, table)
    return len(tbl.getKeys())


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


def create_nvgre_tunnel(dvs, name, src_ip, tunnel_map_ids, tunnel_map_entry_ids, tunnel_ids, tunnel_term_ids):
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

    # create the VXLAN tunnel Term entry in Config DB
    create_entry_tbl(
        conf_db,
        "NVGRE_TUNNEL", '|', name,
        attrs,
    )


class TestNvgreTunnel(object):

    def test_nvgre(self, dvs, testlog):
        tunnel_map_ids       = set()
        tunnel_map_entry_ids = set()
        tunnel_ids           = set()
        tunnel_term_ids      = set()
        tunnel_map_map       = {}
        vlan_ids             = get_exist_entries(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")

        create_vlan(dvs, "Vlan50", vlan_ids)

        create_nvgre_tunnel(
            dvs, 'tunnel_1', '10.0.0.1',
            tunnel_map_ids, tunnel_map_entry_ids,
            tunnel_ids, tunnel_term_ids
        )

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
