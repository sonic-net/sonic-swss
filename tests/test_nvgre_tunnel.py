import time
import json
import random
import time
import pytest


from swsscommon import swsscommon
from pprint import pprint


NVGRE_TUNNEL = 'NVGRE_TUNNEL'
NVGRE_TUNNEL_MAP = 'NVGRE_TUNNEL_MAP'


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


def get_all_created_entries(db, table, existed_entries):
    tbl =  swsscommon.Table(db, table)
    entries = set(tbl.getKeys())
    new_entries = list(entries - existed_entries)
    assert len(new_entries) >= 0, "DB entries was't created"
    new_entries.sort()
    return new_entries


def get_created_entries(db, table, existed_entries, count):
    new_entries = get_all_created_entries(db, table, existed_entries)
    assert len(new_entries) == count, "Wrong number of created entries."
    return new_entries


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


loopback_id = 0


class NvgreTunnel(object):
    tunnel_ids           = set()
    tunnel_map_ids       = set()
    tunnel_map_entry_ids = set()
    tunnel_map_map       = {}
    tunnel               = {}


    def fetch_exist_entries(self, dvs):
        self.tunnel_ids = get_exist_entries(dvs, SAI_OBJECT_TYPE_TUNNEL)
        self.tunnel_map_ids = get_exist_entries(dvs, SAI_OBJECT_TYPE_TUNNEL_MAP)
        self.tunnel_map_entry_ids = get_exist_entries(dvs, SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY)

        global loopback_id
        if not loopback_id:
            loopback_id = get_lo(dvs)


    def create_nvgre_tunnel(self, dvs, tunnel_name, src_ip):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        create_entry_tbl(conf_db, NVGRE_TUNNEL, '|', tunnel_name, [ ('src_ip', src_ip) ])
        time.sleep(1)


    def check_nvgre_tunnel(self, dvs, tunnel_name, src_ip):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        global loopback_id

        tunnel_id = get_created_entry(asic_db, SAI_OBJECT_TYPE_TUNNEL, self.tunnel_ids)
        tunnel_map_ids = get_created_entries(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP, self.tunnel_map_ids, 4)

        assert how_many_entries_exist(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP) == (len(self.tunnel_map_ids) + 4), "The TUNNEL_MAP wasn't created"
        assert how_many_entries_exist(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY) == len(self.tunnel_map_entry_ids), "The TUNNEL_MAP_ENTRY is created too early"
        assert how_many_entries_exist(asic_db, SAI_OBJECT_TYPE_TUNNEL) == (len(self.tunnel_ids) + 1), "The TUNNEL wasn't created"

        check_object(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP, tunnel_map_ids[0], { 'SAI_TUNNEL_MAP_ATTR_TYPE': 'SAI_TUNNEL_MAP_TYPE_VLAN_ID_TO_VSID' })
        check_object(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP, tunnel_map_ids[1], { 'SAI_TUNNEL_MAP_ATTR_TYPE': 'SAI_TUNNEL_MAP_TYPE_BRIDGE_IF_TO_VSID' })
        check_object(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP, tunnel_map_ids[2], { 'SAI_TUNNEL_MAP_ATTR_TYPE': 'SAI_TUNNEL_MAP_TYPE_VSID_TO_VLAN_ID' })
        check_object(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP, tunnel_map_ids[3], { 'SAI_TUNNEL_MAP_ATTR_TYPE': 'SAI_TUNNEL_MAP_TYPE_VSID_TO_BRIDGE_IF' })

        check_object(asic_db, SAI_OBJECT_TYPE_TUNNEL, tunnel_id,
            {
                'SAI_TUNNEL_ATTR_TYPE': 'SAI_TUNNEL_TYPE_NVGRE',
                'SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE': loopback_id,
                'SAI_TUNNEL_ATTR_DECAP_MAPPERS': f'2:{tunnel_map_ids[2]},{tunnel_map_ids[3]}',
                'SAI_TUNNEL_ATTR_ENCAP_MAPPERS': f'2:{tunnel_map_ids[0]},{tunnel_map_ids[1]}',
                'SAI_TUNNEL_ATTR_ENCAP_SRC_IP': src_ip
            }
        )

        self.tunnel_map_ids.update(tunnel_map_ids)
        self.tunnel_ids.add(tunnel_id)
        self.tunnel_map_map[tunnel_name] = tunnel_map_ids
        self.tunnel[tunnel_name] = tunnel_id


    def check_invalid_nvgre_tunnel(self, dvs):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        assert how_many_entries_exist(asic_db, SAI_OBJECT_TYPE_TUNNEL) == len(self.tunnel_ids), "Invalid TUNNEL was created"
        assert how_many_entries_exist(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP) == len(self.tunnel_map_ids), "Invalid TUNNEL_MAP was created"


    def remove_nvgre_tunnel(self, dvs, tunnel_name):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        delete_entry_tbl(conf_db, NVGRE_TUNNEL, tunnel_name)
        time.sleep(1)


    def check_remove_nvgre_tunnel(self, dvs, tunnel_name):
        self.fetch_exist_entries(dvs)
        self.tunnel.pop(tunnel_name, None)
        self.tunnel_map_map.pop(tunnel_name, None)


    def create_nvgre_tunnel_map_entry(self, dvs, tunnel_name, tunnel_map_entry_name, vlan_id, vsid):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        create_entry_tbl(
            conf_db,
            NVGRE_TUNNEL_MAP, '|', f'{tunnel_name}|{tunnel_map_entry_name}',
            [
                ('vsid', vsid),
                ('vlan_id', f'Vlan{vlan_id}'),
            ],
        )
        time.sleep(1)


    def check_nvgre_tunnel_map_entry(self, dvs, tunnel_name, vlan_id, vsid):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        if (self.tunnel_map_map.get(tunnel_name) is None):
            tunnel_map_ids = get_created_entries(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP, self.tunnel_map_ids, 4)
        else:
            tunnel_map_ids = self.tunnel_map_map[tunnel_name]

        tunnel_map_entry_id = get_created_entries(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, self.tunnel_map_entry_ids, 1)

        assert how_many_entries_exist(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY) == (len(self.tunnel_map_entry_ids) + 1), "The TUNNEL_MAP_ENTRY is created too early"

        check_object(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, tunnel_map_entry_id[0],
            {
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE': 'SAI_TUNNEL_MAP_TYPE_VSID_TO_VLAN_ID',
                'SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP': tunnel_map_ids[2],
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VSID_ID_KEY': vsid,
                'SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE': vlan_id,
            }
        )

        self.tunnel_map_entry_ids.update(tunnel_map_entry_id)


    def check_invalid_nvgre_tunnel_map_entry(self, dvs):
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        assert how_many_entries_exist(asic_db, SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY) == len(self.tunnel_map_entry_ids), "Invalid TUNNEL_MAP_ENTRY was created"


    def remove_nvgre_tunnel_map_entry(self, dvs, tunnel_name, tunnel_map_entry_name):
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        delete_entry_tbl(conf_db, NVGRE_TUNNEL_MAP, f'{tunnel_name}|{tunnel_map_entry_name}')
        time.sleep(1)


    def check_remove_nvgre_tunnel_map_entry(self, dvs):
        self.fetch_exist_entries(dvs)


@pytest.mark.usefixtures('dvs_vlan_manager')
class TestNvgreTunnel(object):

    def get_nvgre_tunnel_obj(self):
        return NvgreTunnel()


    def test_nvgre_create_tunnel_map_entry(self, dvs, testlog):
        try:
            tunnel_name = 'tunnel_1'
            tunnel_map_entry_name = 'entry_1'
            src_ip = '10.0.0.1'
            vlan_id = '500'
            vsid = '850'

            nvgre_obj = self.get_nvgre_tunnel_obj()
            nvgre_obj.fetch_exist_entries(dvs)

            self.dvs_vlan.create_vlan(vlan_id)

            nvgre_obj.create_nvgre_tunnel(dvs, tunnel_name, src_ip)
            nvgre_obj.check_nvgre_tunnel(dvs, tunnel_name, src_ip)

            nvgre_obj.create_nvgre_tunnel_map_entry(dvs, tunnel_name, tunnel_map_entry_name, vlan_id, vsid)
            nvgre_obj.check_nvgre_tunnel_map_entry(dvs, tunnel_name, vlan_id, vsid)
        finally:
            nvgre_obj.remove_nvgre_tunnel_map_entry(dvs, tunnel_name, tunnel_map_entry_name)
            nvgre_obj.check_remove_nvgre_tunnel_map_entry(dvs)

            nvgre_obj.remove_nvgre_tunnel(dvs, tunnel_name)
            nvgre_obj.check_remove_nvgre_tunnel(dvs, tunnel_name)

            self.dvs_vlan.remove_vlan(vlan_id)


    def test_multiple_nvgre_tunnels_entries(self, dvs, testlog):
        try:
            tunnel_name_1 = 'tunnel_1'
            tunnel_name_2 = 'tunnel_2'
            tunnel_name_3 = 'tunnel_3'
            entry_1 = 'entry_1'
            entry_2 = 'entry_2'
            entry_3 = 'entry_3'
            entry_4 = 'entry_4'

            nvgre_obj = self.get_nvgre_tunnel_obj()
            nvgre_obj.fetch_exist_entries(dvs)

            self.dvs_vlan.create_vlan('501')
            self.dvs_vlan.create_vlan('502')
            self.dvs_vlan.create_vlan('503')
            self.dvs_vlan.create_vlan('504')

            nvgre_obj.create_nvgre_tunnel(dvs, tunnel_name_1, '10.0.0.1')
            nvgre_obj.check_nvgre_tunnel(dvs, tunnel_name_1, '10.0.0.1')

            nvgre_obj.create_nvgre_tunnel_map_entry(dvs, tunnel_name_1, entry_1, '501', '801')
            nvgre_obj.check_nvgre_tunnel_map_entry(dvs, tunnel_name_1, '501', '801')

            nvgre_obj.create_nvgre_tunnel(dvs, tunnel_name_2, '10.0.0.2')
            nvgre_obj.check_nvgre_tunnel(dvs, tunnel_name_2, '10.0.0.2')

            nvgre_obj.create_nvgre_tunnel_map_entry(dvs, tunnel_name_2, entry_2, '502', '802')
            nvgre_obj.check_nvgre_tunnel_map_entry(dvs, tunnel_name_2, '502', '802')

            nvgre_obj.create_nvgre_tunnel(dvs, tunnel_name_3, '10.0.0.3')
            nvgre_obj.check_nvgre_tunnel(dvs, tunnel_name_3, '10.0.0.3')

            nvgre_obj.create_nvgre_tunnel_map_entry(dvs, tunnel_name_3, entry_3, '503', '803')
            nvgre_obj.check_nvgre_tunnel_map_entry(dvs, tunnel_name_3, '503', '803')

            nvgre_obj.create_nvgre_tunnel_map_entry(dvs, tunnel_name_3, entry_4, '504', '804')
            nvgre_obj.check_nvgre_tunnel_map_entry(dvs, tunnel_name_3, '504', '804')
        finally:
            nvgre_obj.remove_nvgre_tunnel_map_entry(dvs, tunnel_name_1, entry_1)
            nvgre_obj.check_remove_nvgre_tunnel_map_entry(dvs)

            nvgre_obj.remove_nvgre_tunnel(dvs, tunnel_name_1)
            nvgre_obj.check_remove_nvgre_tunnel(dvs, tunnel_name_1)

            nvgre_obj.remove_nvgre_tunnel_map_entry(dvs, tunnel_name_2, entry_2)
            nvgre_obj.check_remove_nvgre_tunnel_map_entry(dvs)

            nvgre_obj.remove_nvgre_tunnel(dvs, tunnel_name_2)
            nvgre_obj.check_remove_nvgre_tunnel(dvs, tunnel_name_2)

            nvgre_obj.remove_nvgre_tunnel_map_entry(dvs, tunnel_name_3, entry_3)
            nvgre_obj.check_remove_nvgre_tunnel_map_entry(dvs)

            nvgre_obj.remove_nvgre_tunnel_map_entry(dvs, tunnel_name_3, entry_4)
            nvgre_obj.check_remove_nvgre_tunnel_map_entry(dvs)

            nvgre_obj.remove_nvgre_tunnel(dvs, tunnel_name_3)
            nvgre_obj.check_remove_nvgre_tunnel(dvs, tunnel_name_3)

            self.dvs_vlan.remove_vlan('501')
            self.dvs_vlan.remove_vlan('502')
            self.dvs_vlan.remove_vlan('503')
            self.dvs_vlan.remove_vlan('504')


    def test_invalid_nvgre_tunnel(self, dvs, testlog):
        nvgre_obj = self.get_nvgre_tunnel_obj()
        nvgre_obj.fetch_exist_entries(dvs)

        nvgre_obj.create_nvgre_tunnel(dvs, 'tunnel_1', '1111.1111.1111.1111')
        nvgre_obj.check_invalid_nvgre_tunnel(dvs)


    def test_invalid_nvgre_tunnel_map_entry(self, dvs, testlog):
        try:
            tunnel_name = 'tunnel_1'
            tunnel_map_entry_name = 'entry_1'
            src_ip = '10.0.0.1'
            vlan_id = '500'
            vsid = 'INVALID'

            nvgre_obj = self.get_nvgre_tunnel_obj()
            nvgre_obj.fetch_exist_entries(dvs)

            self.dvs_vlan.create_vlan(vlan_id)

            nvgre_obj.create_nvgre_tunnel(dvs, tunnel_name, src_ip)
            nvgre_obj.check_nvgre_tunnel(dvs, tunnel_name, src_ip)

            nvgre_obj.create_nvgre_tunnel_map_entry(dvs, tunnel_name, tunnel_map_entry_name, vlan_id, vsid)
            nvgre_obj.check_invalid_nvgre_tunnel_map_entry(dvs)
        finally:
            nvgre_obj.remove_nvgre_tunnel(dvs, tunnel_name)
            nvgre_obj.check_remove_nvgre_tunnel(dvs, tunnel_name)

            self.dvs_vlan.remove_vlan(vlan_id)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
