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


def create_entry_tbl(db, table, key, pairs):
    tbl = swsscommon.Table(db, table)
    create_entry(tbl, key, pairs)


def create_entry_pst(db, table, key, pairs):
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


def is_vrf_attributes_correct(db, table, key, expected_attributes):
    tbl =  swsscommon.Table(db, table)
    keys = set(tbl.getKeys())
    assert key in keys, "The created key wasn't found"

    status, fvs = tbl.get(key)
    assert status, "Got an error when get a key"

    # filter the fake 'NULL' attribute out
    fvs = filter(lambda x : x != ('NULL', 'NULL'), fvs)

    attr_keys = {entry[0] for entry in fvs}
    assert attr_keys == set(expected_attributes.keys())

    for name, value in fvs:
        assert expected_attributes[name] == value, "Wrong value %s for the attribute %s = %s" % \
                                                   (value, name, expected_attributes[name])


def vrf_create(asic_db, appl_db, vrf_name, attributes, expected_attributes):
    # check that the vrf wasn't exist before
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == 1, "The initial state is incorrect"

    # read existing entries in the DB
    initial_entries = entries(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")

    # create a fake attribute if we don't have attributes in the request
    if len(attributes) == 0:
        attributes = [('empty', 'empty')]

    # create the VRF entry in Config DB
    create_entry_pst(appl_db, "VRF_TABLE", vrf_name, attributes)

    # check that the vrf entry was created
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == 2, "The vrf wasn't created"

    # find the id of the entry which was added
    added_entry_id = list(entries(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") - initial_entries)[0]

    # check correctness of the created attributes
    is_vrf_attributes_correct(
        asic_db,
        "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER",
        added_entry_id,
        expected_attributes,
    )

    state = {
        'initial_entries': initial_entries,
        'entry_id': added_entry_id,
    }

    return state


def vrf_remove(asic_db, appl_db, vrf_name, state):
    # delete the created vrf entry
    delete_entry_pst(appl_db, "VRF_TABLE", vrf_name)

    # check that the vrf entry was removed
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == 1, "The vrf wasn't removed"

    # check that the correct vrf entry was removed
    assert state['initial_entries'] == entries(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"), "The incorrect entry was removed"


def vrf_update(asic_db, appl_db, vrf_name, attributes, expected_attributes, state):
    # update the VRF entry in Config DB
    create_entry_pst(appl_db, "VRF_TABLE", vrf_name, attributes)

    # check correctness of the created attributes
    is_vrf_attributes_correct(
        asic_db,
        "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER",
        state['entry_id'],
        expected_attributes,
    )


def boolean_gen():
    result = random.choice(['false', 'true'])
    return result, result


def mac_addr_gen():
    ns = [random.randint(0, 255) for _ in xrange(6)]
    ns[0] &= 0xfe
    mac = ':'.join("%02x" % n for n in ns)
    return mac, mac.upper()


def packet_action_gen():
    values = [
        ("drop",        "SAI_PACKET_ACTION_DROP"),
        ("forward",     "SAI_PACKET_ACTION_FORWARD"),
        ("copy",        "SAI_PACKET_ACTION_COPY"),
        ("copy_cancel", "SAI_PACKET_ACTION_COPY_CANCEL"),
        ("trap",        "SAI_PACKET_ACTION_TRAP"),
        ("log",         "SAI_PACKET_ACTION_LOG"),
        ("deny",        "SAI_PACKET_ACTION_DENY"),
        ("transit",     "SAI_PACKET_ACTION_TRANSIT"),
    ]

    r = random.choice(values)
    return r[0], r[1]


def test_VRFOrch_Comprehensive(dvs, testlog):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

    attributes = [
        ('v4',            'SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V4_STATE',                     boolean_gen),
        ('v6',            'SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V6_STATE',                     boolean_gen),
        ('src_mac',       'SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS',                    mac_addr_gen),
        ('ttl_action',    'SAI_VIRTUAL_ROUTER_ATTR_VIOLATION_TTL1_PACKET_ACTION',       packet_action_gen),
        ('ip_opt_action', 'SAI_VIRTUAL_ROUTER_ATTR_VIOLATION_IP_OPTIONS_PACKET_ACTION', packet_action_gen),
        ('l3_mc_action',  'SAI_VIRTUAL_ROUTER_ATTR_UNKNOWN_L3_MULTICAST_PACKET_ACTION', packet_action_gen),
    ]

    random.seed(int(time.clock()))

    for n in xrange(2**len(attributes)):
        # generate testcases for all combinations of attributes
        req_attr = []
        exp_attr = {}
        vrf_name = "vrf_%d" % n
        bmask = 0x1
        for an in xrange(len(attributes)):
            if (bmask & n) > 0:
                req_res, exp_res = attributes[an][2]()
                req_attr.append((attributes[an][0], req_res))
                exp_attr[attributes[an][1]] = exp_res
            bmask <<= 1
        state = vrf_create(asic_db, appl_db, vrf_name, req_attr, exp_attr)
        vrf_remove(asic_db, appl_db, vrf_name, state)


def test_VRFOrch(dvs, testlog):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    state = vrf_create(asic_db, appl_db, "vrf0",
        [
        ],
        {
        }
    )
    vrf_remove(asic_db, appl_db, "vrf0", state)

    state = vrf_create(asic_db, appl_db, "vrf1",
        [
            ('v4', 'true'),
            ('src_mac', '02:04:06:07:08:09'),
        ],
        {
            'SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V4_STATE':  'true',
            'SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS': '02:04:06:07:08:09',
        }
    )
    vrf_remove(asic_db, appl_db, "vrf1", state)

def test_VRFOrch_Update(dvs, testlog):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

    attributes = [
        ('v4',            'SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V4_STATE',                     boolean_gen),
        ('v6',            'SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V6_STATE',                     boolean_gen),
        ('src_mac',       'SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS',                    mac_addr_gen),
        ('ttl_action',    'SAI_VIRTUAL_ROUTER_ATTR_VIOLATION_TTL1_PACKET_ACTION',       packet_action_gen),
        ('ip_opt_action', 'SAI_VIRTUAL_ROUTER_ATTR_VIOLATION_IP_OPTIONS_PACKET_ACTION', packet_action_gen),
        ('l3_mc_action',  'SAI_VIRTUAL_ROUTER_ATTR_UNKNOWN_L3_MULTICAST_PACKET_ACTION', packet_action_gen),
    ]

    random.seed(int(time.clock()))

    state = vrf_create(asic_db, appl_db, "vrf_a",
        [
        ],
        {
        }
    )

    # try to update each attribute
    req_attr = []
    exp_attr = {}
    for attr in attributes:
        req_res, exp_res = attr[2]()
        req_attr.append((attr[0], req_res))
        exp_attr[attr[1]] = exp_res
        vrf_update(asic_db, appl_db, "vrf_a", req_attr, exp_attr, state)

    vrf_remove(asic_db, appl_db, "vrf_a", state)
