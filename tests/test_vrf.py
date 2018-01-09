from swsscommon import swsscommon
import time
import json
from pprint import pprint


def create_entry(tbl, key, pairs):
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)

    # FIXME: better to wait until DB create them
    time.sleep(1)

def create_entry_tbl(db, table, separator, key, pairs):
    tbl = swsscommon.Table(db, table, separator)
    create_entry(tbl, key, pairs)

def create_entry_pst(db, table, separator, key, pairs):
    tbl = swsscommon.ProducerStateTable(db, table)
    create_entry(tbl, key, pairs)

def delete_entry_tbl(db, table, separator, key):
    tbl = swsscommon.Table(db, table, separator)
    tbl._del(key)
    time.sleep(1)

def how_many_entries_exist(db, table):
    tbl =  swsscommon.Table(db, table)
    return len(tbl.getKeys())

def entries(db, table):
    tbl =  swsscommon.Table(db, table)
    return set(tbl.getKeys())


def is_vrf_attributes_correct(db, table, key, expected_attributes):
    tbl =  swsscommon.Table(db, table)
    keys = set(tbl.getKeys())
    assert key in keys, "The created key wasn't found"

    status, fvs = tbl.get(key)
    assert status, "Got an error when get a key"

    attr_keys = {entry[0] for entry in fvs}
    assert attr_keys == set(expected_attributes.keys())

    for name, value in fvs:
        assert expected_attributes[name] == value, "Wrong value %s for the attribute %s = %s" % \
                                                   (value, name, expected_attributes[name])


def case(asic_db, conf_db, vrf_name, attributes, expected_attributes):
    # check that the vrf wasn't exist before
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == 1, "The initial state is incorrect"

    initial_entries = entries(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")

    # create a VRF entry in Config DB
    create_entry_tbl(
        conf_db,
        "VRF", '|', "vrf1",
        attributes,
    )

    # check that the vrf entry was created
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == 2, "The vrf wasn't created"

    added_entry_id = (list(entries(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") - initial_entries))[0]

    is_vrf_attributes_correct(
        asic_db,
        "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER",
        added_entry_id,
        expected_attributes,
    )

    # delete the created vrf entry
    delete_entry_tbl(
        conf_db,
        "VRF", '|', "vrf1",
    )

    # check that the vrf entry was removed
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == 1, "The vrf wasn't removed"

    # check that the correct vrv entry was removed
    assert initial_entries == entries(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"), "The incorrect entry was removed"


def test_VRFOrch(dvs):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

#    case(asic_db, conf_db, "vrf0",
#        [
#            ('empty', 'empty'),
#        ],
#        {
#        }
#    )


    case(asic_db, conf_db, "vrf1",
        [
            ('v4', 'true'),
            ('src_mac', '02:04:06:07:08:09'),
        ],
        {
            'SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS': '02:04:06:07:08:09',
            'SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V4_STATE':  'true',
        }
    )
