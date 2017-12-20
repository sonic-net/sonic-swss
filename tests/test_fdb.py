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


def get_map_iface_bridge_port_id(asic_db, dvs):
    port_id_2_iface = dvs.asicdb.portoidmap
    tbl = swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
    iface_2_bridge_port_id = {}
    for key in tbl.getKeys():
        status, data = tbl.get(key)
        assert status
        values = dict(data)
        iface_id = values["SAI_BRIDGE_PORT_ATTR_PORT_ID"]
        iface_name = port_id_2_iface[iface_id]
        iface_2_bridge_port_id[iface_name] = key

    return iface_2_bridge_port_id

def how_many_entries_exist(db, table):
    tbl =  swsscommon.Table(db, table)
    return len(tbl.getKeys())


def is_fdb_entry_exists(db, table, key_values, attributes):
    tbl =  swsscommon.Table(db, table)
    keys = tbl.getKeys()

    exists = True
    extra_info = []
    key_found = False
    for key in keys:
        d_key = json.loads(key)
        for k, v in key_values:
            if k not in d_key or v != d_key[k]:
                continue

        key_found = True

        status, fvs = tbl.get(key)
        assert status, "Error reading from table %s" % table

        d_attributes = dict(attributes)
        for k, v in fvs:
            if k in d_attributes and d_attributes[k] == v:
                del d_attributes[k]

        if len(d_attributes) != 0:
            exists = False
            extra_info.append("Desired attributes %s was not found for key %s" % (str(d_attributes), key))
        
        break

    if not key_found:
        exists = False
        extra_info.append("Desired key with parameters %s was not found" % str(key_values))

    return exists, extra_info


def test_FDBAddedAfterMemberCreated(dvs):
    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    # create a FDB entry in Application DB
    create_entry_pst(
        appl_db,
        "FDB_TABLE", ':', "Vlan2:52-54-00-25-06-E9",
        [
            ("port", "Ethernet0"),
            ("type", "dynamic"),
        ]
    )

    # check that the FDB entry wasn't inserted into ASIC DB 
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 0, "The fdb entry leaked to ASIC"

    # create vlan
    create_entry_tbl(
        conf_db,
        "VLAN", '|', "Vlan2",
        [
            ("vlanid", "2"),
        ]
    )

    # create vlan member entry in application db
    create_entry_tbl(
        conf_db,
        "VLAN_MEMBER", '|', "Vlan2|Ethernet0",
         [
            ("tagging_mode", "untagged"),
         ]
    )

    # check that the vlan information was propagated
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN") == 2, "The 2 vlan wasn't created"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT") == 1, "The bridge port wasn't created"
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER") == 1, "The vlan member wasn't added"

    # Get mapping between interface name and its bridge port_id
    iface_2_bridge_port_id = get_map_iface_bridge_port_id(asic_db, dvs)

    # check that the FDB entry was inserted into ASIC DB
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 1, "The fdb entry wasn't inserted to ASIC"

    ok, extra = is_fdb_entry_exists(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                    [("mac", "52-54-00-25-06-E9"), ("vlan", "2")],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet0"]),
                     ('SAI_FDB_ENTRY_ATTR_PACKET_ACTION', 'SAI_PACKET_ACTION_FORWARD')]
    )
    assert ok, str(extra)
