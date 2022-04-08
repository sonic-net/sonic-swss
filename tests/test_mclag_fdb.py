from swsscommon import swsscommon
import os
import sys
import time
import json
import pytest
from distutils.version import StrictVersion

def create_entry(tbl, key, pairs):
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)

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

def get_port_oid(dvs, port_name):
    counters_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
    port_map_tbl = swsscommon.Table(counters_db, 'COUNTERS_PORT_NAME_MAP')
    for k in port_map_tbl.get('')[1]:
        if k[0] == port_name:
            return k[1]
    return None

def get_bridge_port_oid(dvs, port_oid):
    tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
    for key in tbl.getKeys():
        status, data = tbl.get(key)
        assert status
        values = dict(data)
        if port_oid == values["SAI_BRIDGE_PORT_ATTR_PORT_ID"]:
            return key
    return None

def create_port_channel(dvs, alias):
    tbl = swsscommon.Table(dvs.cdb, "PORTCHANNEL")
    fvs = swsscommon.FieldValuePairs([("admin_status", "up"),
                                  ("mtu", "9100")])
    tbl.set(alias, fvs)
    time.sleep(1)

def remove_port_channel(dvs, alias):
    tbl = swsscommon.Table(dvs.cdb, "PORTCHANNEL")
    tbl._del(alias)
    time.sleep(1)

def add_port_channel_members(dvs, lag, members):
    tbl = swsscommon.Table(dvs.cdb, "PORTCHANNEL_MEMBER")
    fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
    for member in members:
        tbl.set(lag + "|" + member, fvs)
        time.sleep(1)

def remove_port_channel_members(dvs, lag, members):
    tbl = swsscommon.Table(dvs.cdb, "PORTCHANNEL_MEMBER")
    for member in members:
        tbl._del(lag + "|" + member)
        time.sleep(1)

def how_many_entries_exist(db, table):
    tbl =  swsscommon.Table(db, table)
    return len(tbl.getKeys())

# Test-1 Verify basic config add

@pytest.mark.dev_sanity
def test_mclagFdb_basic_config_add(dvs, testlog):
    dvs.setup_db()
    dvs.clear_fdb()
    time.sleep(2)

    vlan_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
    bp_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
    vm_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")

    # create PortChannel0005
    create_port_channel(dvs, "PortChannel0005")

    # add members to PortChannel0005
    add_port_channel_members(dvs, "PortChannel0005", ["Ethernet4"])

    # create PortChannel0006
    create_port_channel(dvs, "PortChannel0006")

    # add members to PortChannel0006
    add_port_channel_members(dvs, "PortChannel0006", ["Ethernet8"])

    # create PortChannel0008
    create_port_channel(dvs, "PortChannel0008")

    # add members to PortChannel0008
    add_port_channel_members(dvs, "PortChannel0008", ["Ethernet12"])

    # create vlan
    dvs.create_vlan("200")

    # Add vlan members
    dvs.create_vlan_member("200", "PortChannel0005")
    dvs.create_vlan_member("200", "PortChannel0006")
    dvs.create_vlan_member("200", "PortChannel0008")

    # check that the vlan information was propagated
    vlan_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
    bp_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
    vm_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")

    assert vlan_after - vlan_before == 1, "The Vlan200 wasn't created"
    assert bp_after - bp_before == 3, "The bridge port wasn't created"
    assert vm_after - vm_before == 3, "The vlan member wasn't added"

# Test-2 Remote Dynamic MAC Add

@pytest.mark.dev_sanity
def test_mclagFdb_remote_dynamic_mac_add(dvs, testlog):
    dvs.setup_db()
    # create FDB entry in APP_DB MCLAG_FDB_TABLE 
    create_entry_pst(
        dvs.pdb,
        "MCLAG_FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
        [
            ("port", "PortChannel0005"),
            ("type", "dynamic"),
        ]
    )

    # check that the FDB entry was inserted into ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 1, "The MCLAG fdb entry not inserted to ASIC"

    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "3C:85:99:5E:00:01"), ("bvid", str(dvs.getVlanOid("200")))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC"),
                     ("SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE", "true")]
    )

    assert ok, str(extra)

# Test-3 Remote Dynamic MAC Delete

@pytest.mark.dev_sanity
def test_mclagFdb_remote_dynamic_mac_delete(dvs, testlog):
    dvs.setup_db()

    delete_entry_pst(
        dvs.pdb,
        "MCLAG_FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
    )

    time.sleep(2)
    # check that the FDB entry was deleted from ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 0, "The MCLAG fdb entry not deleted"


# Test-4 Remote Static MAC Add

@pytest.mark.dev_sanity
def test_mclagFdb_remote_static_mac_add(dvs, testlog):
    dvs.setup_db()
    
    create_entry_pst(
        dvs.pdb,
        "MCLAG_FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
        [
            ("port", "PortChannel0005"),
            ("type", "static"),
        ]
    )

    # check that the FDB entry was inserted into ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 1, "The MCLAG static fdb entry not inserted to ASIC"

    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "3C:85:99:5E:00:01"), ("bvid", str(dvs.getVlanOid("200")))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC")]
    )

    assert ok, str(extra)

# Test-5 Remote Static MAC Del

@pytest.mark.dev_sanity
def test_mclagFdb_remote_static_mac_del(dvs, testlog):
    dvs.setup_db()
    
    delete_entry_pst(
        dvs.pdb,
        "MCLAG_FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
    )

    time.sleep(2)
    # check that the FDB entry was deleted from ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 0, "The MCLAG static fdb entry not deleted"


# Test-6 Verify Remote to Local Move.

@pytest.mark.dev_sanity
def test_mclagFdb_remote_to_local_mac_move(dvs, testlog):
    dvs.setup_db()

    #Add remote MAC to MCLAG_FDB_TABLE on PortChannel0005
    create_entry_pst(
        dvs.pdb,
        "MCLAG_FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
        [
            ("port", "PortChannel0005"),
            ("type", "dynamic"),
        ]
    )

    # check that the FDB entry inserted into ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 1, "The MCLAG fdb entry not inserted to ASIC"

    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "3C:85:99:5E:00:01"), ("bvid", str(dvs.getVlanOid("200")))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC"),
                     ("SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE", "true")]
    )

    assert ok, str(extra)

    #Move MAC to PortChannel0008 on Local FDB_TABLE
    create_entry_pst(
        dvs.pdb,
        "FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
        [
            ("port", "PortChannel0008"),
            ("type", "dynamic"),
        ]
    )

    # check that the FDB entry was inserted into ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 1, "The fdb entry not inserted to ASIC"

    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "3C:85:99:5E:00:01"), ("bvid", str(dvs.getVlanOid("200")))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC")]
    )

    assert ok, str(extra)

# Test-7 Verify Remote MAC del should not del local MAC after move 

@pytest.mark.dev_sanity
def test_mclagFdb_local_mac_move_del(dvs, testlog):
    dvs.setup_db()
    
    #Cleanup the FDB from MCLAG_FDB_TABLE
    delete_entry_pst(
        dvs.pdb,
        "MCLAG_FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
    )

    # Verify that the local FDB entry still inserted into ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 1, "The fdb entry not inserted to ASIC"

    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "3C:85:99:5E:00:01"), ("bvid", str(dvs.getVlanOid("200")))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC")]
    )

    assert ok, str(extra)


    #delete the local FDB and Verify
    delete_entry_pst(
        dvs.pdb,
        "FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
    )

    time.sleep(2)
    # check that the FDB entry was deleted from ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 0, "The fdb entry was not deleted"


# Test-8 Verify Local to Remote Move.

@pytest.mark.dev_sanity    
def test_mclagFdb_local_to_remote_move(dvs, testlog):
    dvs.setup_db()

    #Add local MAC to FDB_TABLE on PortChannel0008
    create_entry_pst(
        dvs.pdb,
        "FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
        [
            ("port", "PortChannel0008"),
            ("type", "dynamic"),
        ]
    )

    # check that the FDB entry was inserted into ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 1, "The MCLAG fdb entry not inserted to ASIC"

    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "3C:85:99:5E:00:01"), ("bvid", str(dvs.getVlanOid("200")))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC")]
    )

    assert ok, str(extra)

    #Move MAC to PortChannel0005 on Remote FDB_TABLE
    create_entry_pst(
        dvs.pdb,
        "MCLAG_FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
        [
            ("port", "PortChannel0005"),
            ("type", "dynamic"),
        ]
    )

    # check that the FDB entry was inserted into ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 1, "The MCLAG fdb entry not inserted to ASIC"

    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "3C:85:99:5E:00:01"), ("bvid", str(dvs.getVlanOid("200")))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC"),
                     ("SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE", "true")]
    )

    assert ok, str(extra)

# Test-9 Verify local MAC del should not del remote MAC after move 

@pytest.mark.dev_sanity
def test_mclagFdb_remote_move_del(dvs, testlog):
    dvs.setup_db()

    #Cleanup the local FDB
    delete_entry_pst(
        dvs.pdb,
        "FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
    )

    # check that the remote FDB entry still inserted into ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 1, "The MCLAG fdb entry not inserted to ASIC"

    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "3C:85:99:5E:00:01"), ("bvid", str(dvs.getVlanOid("200")))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC"),
                     ("SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE", "true")]
    )

    assert ok, str(extra)

    #delete the remote FDB and Verify
    delete_entry_pst(
        dvs.pdb,
        "MCLAG_FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
    )

    time.sleep(2)
    # check that the FDB entry was deleted from ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 0, "The fdb entry not deleted"



# Test-10 Verify remote MAC move in remote node is updated locally.

@pytest.mark.dev_sanity
def test_mclagFdb_remote_move_peer_node(dvs, testlog):
    dvs.setup_db()

    #Add remote MAC to MCLAG_FDB_TABLE on PortChannel0005
    create_entry_pst(
        dvs.pdb,
        "MCLAG_FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
        [
            ("port", "PortChannel0005"),
            ("type", "dynamic"),
        ]
    )

    # check that the FDB entry inserted into ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 1, "The MCLAG fdb entry not inserted to ASIC"

    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "3C:85:99:5E:00:01"), ("bvid", str(dvs.getVlanOid("200")))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC"),
                     ("SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE", "true")]
    )

    assert ok, str(extra)

    # Move remote MAC in MCLAG_FDB_TABLE to PortChannel0006
    create_entry_pst(
        dvs.pdb,
        "MCLAG_FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
        [
            ("port", "PortChannel0006"),
            ("type", "dynamic"),
        ]
    )

    # check that the FDB entry inserted into ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 1, "The MCLAG fdb entry not inserted to ASIC"

    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "3C:85:99:5E:00:01"), ("bvid", str(dvs.getVlanOid("200")))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC"),
                     ("SAI_FDB_ENTRY_ATTR_ALLOW_MAC_MOVE", "true")]
    )

    assert ok, str(extra) 

    #delete the remote FDB and Verify
    delete_entry_pst(
        dvs.pdb,
        "MCLAG_FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
    )

    time.sleep(2)
    # check that the FDB entry was deleted from ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 0, "The fdb entry not deleted"


# Test-11 Verify remote dynamic MAC move rejection in presecense of local static MAC.

@pytest.mark.dev_sanity
def test_mclagFdb_static_mac_dynamic_move_reject(dvs, testlog):
    dvs.setup_db()
    
    #Add local MAC to FDB_TABLE on PortChannel0008
    create_entry_pst(
        dvs.pdb,
        "FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
        [
            ("port", "PortChannel0008"),
            ("type", "static"),
        ]
    )

    # check that the static FDB entry was inserted into ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 1, "The MCLAG fdb entry not inserted to ASIC"

    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "3C:85:99:5E:00:01"), ("bvid", str(dvs.getVlanOid("200")))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC")]
    )

    assert ok, str(extra)

    #Add remote MAC to MCLAG_FDB_TABLE on PortChannel0005
    create_entry_pst(
        dvs.pdb,
        "MCLAG_FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
        [
            ("port", "PortChannel0005"),
            ("type", "dynamic"),
        ]
    )

    # check that still static FDB entry is inserted into ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 1, "The MCLAG fdb entry not inserted to ASIC"

    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "3C:85:99:5E:00:01"), ("bvid", str(dvs.getVlanOid("200")))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC")]
    )

    assert ok, str(extra)

    delete_entry_pst(
        dvs.pdb,
        "FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
    )

    time.sleep(2)
    # check that the FDB entry was deleted from ASIC DB
    assert how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 0, "The MCLAG static fdb entry not deleted"

    delete_entry_pst(
        dvs.pdb,
        "MCLAG_FDB_TABLE", "Vlan200:3C:85:99:5E:00:01",
    )

# Test-12 Verify cleanup of the basic config.

@pytest.mark.dev_sanity
def test_mclagFdb_basic_config_del(dvs, testlog):
    dvs.setup_db()

    dvs.remove_vlan_member("200", "PortChannel0005")
    dvs.remove_vlan_member("200", "PortChannel0006")
    dvs.remove_vlan_member("200", "PortChannel0008")
    dvs.remove_vlan("200")
    remove_port_channel_members(dvs, "PortChannel0005",
            ["Ethernet4"])
    remove_port_channel_members(dvs, "PortChannel0006",
            ["Ethernet8"])
    remove_port_channel_members(dvs, "PortChannel0008",
            ["Ethernet12"])
    remove_port_channel(dvs, "PortChannel0005")
    remove_port_channel(dvs, "PortChannel0006")
    remove_port_channel(dvs, "PortChannel0008")

