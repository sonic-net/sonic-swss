from swsscommon import swsscommon
import os
import sys
import time
import json
from distutils.version import StrictVersion

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

def how_many_entries_exist(db, table):
    tbl =  swsscommon.Table(db, table)
    return len(tbl.getKeys())

def test_FDBAddedAfterMemberCreated(dvs):
    dvs.setup_db()

    dvs.runcmd("sonic-clear fdb all")
    time.sleep(2)

    # create a FDB entry in Application DB
    create_entry_pst(
        dvs.pdb,
        "FDB_TABLE", "Vlan2:52-54-00-25-06-E9",
        [
            ("port", "Ethernet0"),
            ("type", "dynamic"),
        ]
    )

    # check that the FDB entry wasn't inserted into ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 0, "The fdb entry leaked to ASIC"

    vlan_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
    bp_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
    vm_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")

    # create vlan
    create_entry_tbl(
        dvs.cdb,
        "VLAN", "Vlan2",
        [
            ("vlanid", "2"),
        ]
    )

    # create vlan member entry in application db
    create_entry_tbl(
        dvs.cdb,
        "VLAN_MEMBER", "Vlan2|Ethernet0",
        [
            ("tagging_mode", "untagged"),
        ]
    )

    # check that the vlan information was propagated
    vlan_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
    bp_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
    vm_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")

    assert vlan_after - vlan_before == 1, "The Vlan2 wasn't created"
    assert bp_after - bp_before == 1, "The bridge port wasn't created"
    assert vm_after - vm_before == 1, "The vlan member wasn't added"

    # Get mapping between interface name and its bridge port_id
    iface_2_bridge_port_id = dvs.get_map_iface_bridge_port_id(dvs.adb)

    # check that the FDB entry was inserted into ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 1, "The fdb entry wasn't inserted to ASIC"

    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                    [("mac", "52-54-00-25-06-E9"), ("vlan", "2")],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet0"]),
                     ('SAI_FDB_ENTRY_ATTR_PACKET_ACTION', 'SAI_PACKET_ACTION_FORWARD')]
    )
    assert ok, str(extra)
