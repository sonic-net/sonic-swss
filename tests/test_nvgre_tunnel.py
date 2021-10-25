import time
import json
import random
import time
import pytest

from swsscommon import swsscommon
from pprint import pprint


NVGRE_TUNNEL = 'NVGRE_TUNNEL'
NVGRE_TUNNEL_MAP = 'NVGRE_TUNNEL_MAP'
NVGRE_TUNNEL_NAME = 'tunnel_1'
NVGRE_TUNNEL_MAP_ENTRY_NAME = 'entry_1'
NVGRE_VSID = '850'
VLAN_ID = '500'
VALID_IP_ADDR = '10.0.0.1'


SAI_OBJECT_TYPE_TUNNEL = 'ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL'
SAI_OBJECT_TYPE_TUNNEL_MAP = 'ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP'
SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY = 'ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY'


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


def get_created_entry_mapid(db, table, existed_entries):
    tbl =  swsscommon.Table(db, table)
    entries = set(tbl.getKeys())
    new_entries = list(entries - existed_entries)
    new_entries.sort()
    return new_entries


def how_many_entries_exist(db, table):
    tbl =  swsscommon.Table(db, table)
    return len(tbl.getKeys())


def get_lo(dvs):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

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


def create_nvgre_tunnel(dvs, tunnel_name, src_ip, tunnel_ids, tunnel_map_ids):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    assert how_many_entries_exist(asic_db, SAI_OBJECT_TYPE_TUNNEL) == len(tunnel_ids), 'The initial state is incorrect'
    assert how_many_entries_exist(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP) == len(tunnel_map_ids), 'The initial state is incorrect'

    attrs = [
        ('src_ip', src_ip),
    ]

    create_entry_tbl(conf_db, NVGRE_TUNNEL, '|', tunnel_name, attrs)


def remove_nvgre_tunnel(dvs, tunnel_name, tunnel_ids, tunnel_map_ids):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    delete_entry_tbl(conf_db, NVGRE_TUNNEL, tunnel_name)
    time.sleep(1)
    tunnel_ids = get_exist_entries(dvs, SAI_OBJECT_TYPE_TUNNEL)
    tunnel_map_ids = get_exist_entries(dvs, SAI_OBJECT_TYPE_TUNNEL_MAP)


def check_nvgre_tunnel(dvs, src_ip, tunnel_ids, tunnel_map_ids, loopback_id):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

    tunnel_id = get_created_entry(asic_db, SAI_OBJECT_TYPE_TUNNEL, tunnel_ids)
    tunnel_map_id = get_created_entry_mapid(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP, tunnel_map_ids)

    assert how_many_entries_exist(asic_db, SAI_OBJECT_TYPE_TUNNEL) == len(tunnel_ids) + 1, 'NVGRE Tunnel was not created'
    assert how_many_entries_exist(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP) == len(tunnel_map_ids) + 4, 'NVGRE Tunnel mappers was not created'

    check_object(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP, tunnel_map_id[0], { 'SAI_TUNNEL_MAP_ATTR_TYPE': 'SAI_TUNNEL_MAP_TYPE_VLAN_ID_TO_VNI' })

    encapstr = '2:' + tunnel_map_id[0] + ',' + tunnel_map_id[1]
    decapstr = '2:' + tunnel_map_id[2] + ',' + tunnel_map_id[3]

    # TODO: change types to NVGRE
    check_object(asic_db, SAI_OBJECT_TYPE_TUNNEL, tunnel_id,
        {
            'SAI_TUNNEL_ATTR_TYPE': 'SAI_TUNNEL_TYPE_VXLAN',
            'SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE': loopback_id,
            'SAI_TUNNEL_ATTR_DECAP_MAPPERS': decapstr,
            'SAI_TUNNEL_ATTR_ENCAP_MAPPERS': encapstr,
            'SAI_TUNNEL_ATTR_ENCAP_SRC_IP': src_ip
        }
    )

    tunnel_map_ids.update(tunnel_map_id)
    tunnel_ids.add(tunnel_id)


def create_nvgre_tunnel_entry(dvs, tunnel_name, tunnel_map_entry_name, vlan_id, vsid, tunnel_map_ids, tunnel_map_entry_ids):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    assert how_many_entries_exist(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY) == len(tunnel_map_entry_ids), 'The initial state is incorrect'

    create_entry_tbl(
        conf_db,
        NVGRE_TUNNEL_MAP, '|', '%s|%s' % (tunnel_name, tunnel_map_entry_name),
        [
            ('vsid', vsid),
            ('vlan', vlan_id),
        ],
    )

    tunnel_map_id = get_created_entry_mapid(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP, tunnel_map_ids)
    tunnel_map_entry_id = get_created_entry(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, tunnel_map_entry_ids)

    check_object(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, tunnel_map_entry_id,
        {
            'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID',
            'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': tunnel_map_id[2],
            'SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY': vsid,
            'SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE': vlan_id,
        }
    )

    tunnel_map_entry_ids.add(tunnel_map_entry_id)


def remove_nvgre_tunnel_map_entry(dvs, tunnel_name, tunnel_map_entry_name, tunnel_map_entry_ids):
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    delete_entry_tbl(conf_db, NVGRE_TUNNEL_MAP, "%s|%s" % (tunnel_name, tunnel_map_entry_name))
    time.sleep(1)
    tunnel_map_entry_ids = get_exist_entries(dvs, SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY)


@pytest.mark.usefixtures('dvs_vlan_manager')
class TestNvgreTunnel(object):

    def test_nvgre_create_tunnel(self, dvs, testlog):
        try:
            tunnel_ids = set()
            tunnel_map_ids = set()
            loopback_id = get_lo(dvs)

            create_nvgre_tunnel(dvs, NVGRE_TUNNEL_NAME, VALID_IP_ADDR, tunnel_ids, tunnel_map_ids)
            check_nvgre_tunnel(dvs, VALID_IP_ADDR, tunnel_ids, tunnel_map_ids, loopback_id)
        finally:
            remove_nvgre_tunnel(dvs, NVGRE_TUNNEL_NAME, tunnel_ids, tunnel_map_ids)


    def test_nvgre_create_tunnel_map_entry(self, dvs, testlog):
        try:
            tunnel_ids = set()
            tunnel_map_ids = set()
            tunnel_map_entry_ids = set()
            loopback_id = get_lo(dvs)

            self.dvs_vlan.create_vlan(VLAN_ID)
            create_nvgre_tunnel(dvs, NVGRE_TUNNEL_NAME, VALID_IP_ADDR, tunnel_ids, tunnel_map_ids)
            create_nvgre_tunnel_entry(dvs, NVGRE_TUNNEL_NAME, NVGRE_TUNNEL_MAP_ENTRY_NAME, VLAN_ID, NVGRE_VSID, tunnel_map_ids, tunnel_map_entry_ids)
        finally:
            remove_nvgre_tunnel_map_entry(dvs, NVGRE_TUNNEL_NAME, NVGRE_TUNNEL_MAP_ENTRY_NAME, tunnel_map_entry_ids)
            remove_nvgre_tunnel(dvs, NVGRE_TUNNEL_NAME, tunnel_ids, tunnel_map_ids)
            self.dvs_vlan.remove_vlan(VLAN_ID)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
