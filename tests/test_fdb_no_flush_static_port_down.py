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

def delete_entry_pst(db, table, key):
    tbl = swsscommon.ProducerStateTable(db, table)
    tbl._del(key)
    time.sleep(1)

def how_many_entries_exist(db, table):
    tbl =  swsscommon.Table(db, table)
    return len(tbl.getKeys())

def test_FDBNoStaticFlushPortDown(dvs, testlog):
    dvs.setup_db()

    dvs.runcmd("sonic-clear fdb all")
    time.sleep(2)

    # create a FDB entry in Application DB
    create_entry_pst(
        dvs.pdb,
        "FDB_TABLE", "Vlan2:52-54-00-25-06-E9",
        [
            ("port", "Ethernet0"),
            ("type", "static"),
        ]
    )

    # check that the FDB entry wasn't inserted into ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 0, "The fdb entry leaked to ASIC"

    vlan_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
    bp_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
    vm_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")

    # create vlan
    dvs.create_vlan("2")
    dvs.create_vlan_member("2", "Ethernet0")

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
            [("mac", "52:54:00:25:06:E9"), ("bvid", str(dvs.getVlanOid("2")))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC"),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet0"])]
    )

    assert ok, str(extra)

    dvs.runcmd("config interface shutdown Ethernet0")
    time.sleep(2)
    # check that the FDB entry was not flushed
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") != 0, "The fdb entry was flushed"
    delete_entry_pst(
        dvs.pdb,
        "FDB_TABLE", "Vlan2:52-54-00-25-06-E9",
    )
    dvs.remove_vlan_member("2", "Ethernet0")
    dvs.remove_vlan("2")

