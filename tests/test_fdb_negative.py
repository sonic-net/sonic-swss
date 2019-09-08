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

    # FIXME: better to wait until DB create them
    time.sleep(1)

def remove_entry_tbl(db, table, key):
    tbl = swsscommon.Table(db, table)
    tbl._del(key)
    # FIXME: better to wait until DB create them
    time.sleep(1)

def create_entry_tbl(db, table, key, pairs):
    tbl = swsscommon.Table(db, table)
    create_entry(tbl, key, pairs)

def how_many_entries_exist(db, table):
    tbl =  swsscommon.Table(db, table)
    return len(tbl.getKeys())

def test_negativeFDB(dvs, testlog):
    dvs.setup_db()

    #dvs.runcmd("sonic-clear fdb all")
    time.sleep(2)

    #Find switch_id
    switch_id = dvs.getSwitchOid()
    print("Switch_id="+str(switch_id))

    vlan_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
    bp_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
    vm_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")

    # create vlan
    dvs.create_vlan("2")
    dvs.create_vlan_member("2", "Ethernet0")
    dvs.create_vlan_member("2", "Ethernet4")
    # Find the vlan_oid_2 to be used in DB communications
    vlan_oid_2 = dvs.getVlanOid("2")
    assert vlan_oid_2 is not None, "Could not find Vlan_oid"
    print("VLan-2 vlan_oid="+str(vlan_oid_2))


    # create vlan
    dvs.create_vlan("4")
    dvs.create_vlan_member("4", "Ethernet8")
    # Find the vlan_oid_4 to be used in DB communications
    vlan_oid_4 = dvs.getVlanOid("4")
    assert vlan_oid_4 is not None, "Could not find Vlan_oid"
    print("VLan-4 vlan_oid="+str(vlan_oid_4))


    dvs.create_vlan("10")
    dvs.create_vlan_member("10", "Ethernet12")
    # Find the vlan_oid_10 to be used in DB communications
    vlan_oid_10 = dvs.getVlanOid("10")
    assert vlan_oid_10 is not None, "Could not find Vlan_oid"
    print("VLan-10 vlan_oid="+str(vlan_oid_10))
    
    # check that the vlan information was propagated
    vlan_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
    bp_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
    vm_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")

    assert vlan_after - vlan_before == 3, "The Vlan2/Vlan4 wasn't created"
    assert bp_after - bp_before == 4, "The bridge port wasn't created"
    assert vm_after - vm_before == 4, "The vlan member wasn't added"

    # Get mapping between interface name and its bridge port_id
    iface_2_bridge_port_id = dvs.get_map_iface_bridge_port_id(dvs.adb)

    #dvs.runcmd("swssloglevel -l DEBUG -c orchagent")
    #dvs.runcmd("swssloglevel -l DEBUG -c vlanmgrd")

    print("NEG1 - Add MAC address to an out of range vlan and later delete it")
    mac = "52:54:00:25:06:E9"
    #dvs.runcmd("config mac add " + mac.lower() + " 3 Ethernet0")
    print("ACTION: Creating static FDB Vlan33333|"+mac.lower()+"|Ethernet0 in CONFIG-DB")
    create_entry_tbl(
        dvs.cdb,
        "FDB", "Vlan33333|"+mac.lower(),
        [
            ("port", "Ethernet0"),
        ]
    )
    time.sleep(2)

    # check that the FDB entry was added in Config DB
    print("CHECK: Static FDB Vlan33333:"+mac.lower()+":Ethernet0 is created in Config-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.cdb, "FDB",
                    "Vlan33333\|"+mac.lower(),
                    [("port", "Ethernet0")]
    )
    assert mac1_found, str(extra)
    print("CONFIRM: Static FDB Vlan33333:"+mac.lower()+":Ethernet0 is created in Config-DB")

    # check that the FDB entry was not added in APP DB
    print("CHECK: Static FDB Vlan33333:"+mac.lower()+":Ethernet0 is not created in APP-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.pdb, "FDB_TABLE",
                    "Vlan33333:"+mac.lower(),
                    [("port", "Ethernet0"),
                     ("type", "static"),
                    ]
    )
    assert mac1_found == False, str(extra)
    print("CONFIRM: Static FDB Vlan33333:"+mac.lower()+":Ethernet0 is not created in APP-DB")
    
    print("ACTION: Deleting Static FDB Vlan33333:"+mac.lower()+":Ethernet0")
    remove_entry_tbl(dvs.cdb, "FDB", "Vlan33333|"+mac.lower())
    time.sleep(2)

    #Check the mac is removed from config-db
    print("CHECK: Static FDB Vlan33333:"+mac.lower()+":Ethernet0 is deleted from Config-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.cdb, "FDB",
                    "Vlan33333\|"+mac.lower(),
                    [("port", "Ethernet0")]
    )
    assert mac1_found == False, str(extra)
    print("CONFIRM: Static FDB Vlan33333:"+mac.lower()+":Ethernet0 is deleted from Config-DB")
    
    print("NEG2 - Add MAC address to a vlan which does not exist and later delete it")
    mac = "52:54:00:25:06:E9"
    #dvs.runcmd("config mac add " + mac.lower() + " 3 Ethernet0")
    print("ACTION: Creating static FDB Vlan3:"+mac.lower()+":Ethernet0 in CONFIG-DB")
    create_entry_tbl(
        dvs.cdb,
        "FDB", "Vlan3|"+mac.lower(),
        [
            ("port", "Ethernet0"),
        ]
    )
    time.sleep(2)

    # check that the FDB entry was added in Config DB
    print("CHECK: Static FDB Vlan3:"+mac.lower()+":Ethernet0 is created in Config-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.cdb, "FDB",
                    "Vlan3\|"+mac.lower(),
                    [("port", "Ethernet0")]
    )
    assert mac1_found, str(extra)
    print("CONFIRM: Static FDB Vlan3:"+mac.lower()+":Ethernet0 is created in Config-DB")

    # check that the FDB entry was added in APP DB
    print("CHECK: Static FDB Vlan3:"+mac.lower()+":Ethernet0 is created in APP-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.pdb, "FDB_TABLE",
                    "Vlan3:"+mac.lower(),
                    [("port", "Ethernet0"),
                     ("type", "static"),
                    ]
    )
    assert mac1_found, str(extra)
    print("CONFIRM: Static FDB Vlan3:"+mac.lower()+":Ethernet0 is created in APP-DB")
    
    # check that the FDB entry is not inserted into ASIC DB
    print("CHECK: Static FDB Vlan3:"+mac.lower()+":Ethernet0 is not created in ASIC-DB")
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", mac.lower())],
            [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC")]
    )
    assert ok == False, str(extra)
    print("CONFIRM: Static FDB Vlan3:"+mac.lower()+":Ethernet0 is not created in ASIC-DB")

    print("ACTION: Deleting Static FDB Vlan3:"+mac.lower()+"Ethernet0")
    remove_entry_tbl(dvs.cdb, "FDB", "Vlan3|"+mac.lower())
    time.sleep(2)

    #Check the mac is removed from config-db
    print("CHECK: Static FDB Vlan3:"+mac.lower()+":Ethernet0 is deleted from Config-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.cdb, "FDB",
                    "Vlan3\|"+mac.lower(),
                    [("port", "Ethernet0")]
    )
    assert mac1_found == False, str(extra)
    print("CONFIRM: Static FDB Vlan3:"+mac.lower()+":Ethernet0 is deleted from Config-DB")
    
    # check that the FDB entry is removed from APP DB
    print("CHECK: Static FDB Vlan3:"+mac.lower()+":Ethernet0 is deleted from APP-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.pdb, "FDB_TABLE",
                    "Vlan3:"+mac.lower(),
                    [("port", "Ethernet0"),
                     ("type", "static"),
                    ]
    )
    assert mac1_found == False, str(extra)
    print("CONFIRM: Static FDB Vlan3:"+mac.lower()+":Ethernet0 is deleted from APP-DB")
    

    print("NEG3 - Add MAC address to an invalid port which does not exist and later delete it")
    mac = "52:54:00:25:06:E9"
    #dvs.runcmd("config mac add " + mac.lower() + " 3 Ethernet0")
    print("ACTION: Creating static FDB Vlan2:"+mac.lower()+":Port0 in CONFIG-DB")
    create_entry_tbl(
        dvs.cdb,
        "FDB", "Vlan2|"+mac.lower(),
        [
            ("port", "Port0"),
        ]
    )
    time.sleep(2)


    # check that the FDB entry was added in Config DB
    print("CHECK: Static FDB Vlan2:"+mac.lower()+":Port0 is created in Config-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.cdb, "FDB",
                    "Vlan2\|"+mac.lower(),
                    [("port", "Port0")]
    )
    assert mac1_found, str(extra)
    print("CONFIRM: Static FDB Vlan2:"+mac.lower()+":Port0 is created in Config-DB")

    # check that the FDB entry was added in APP DB
    print("CHECK: Static FDB Vlan2:"+mac.lower()+":Port0 is created in APP-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.pdb, "FDB_TABLE",
                    "Vlan2:"+mac.lower(),
                    [("port", "Port0"),
                     ("type", "static"),
                    ]
    )
    assert mac1_found, str(extra)
    print("CONFIRM: Static FDB Vlan2:"+mac.lower()+"Port0 is created in APP-DB")
    
    # check that the FDB entry is not inserted into ASIC DB
    print("CHECK: Static FDB Vlan2:"+mac.lower()+":Port0 is not created in ASIC-DB")
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", mac.lower())],
            [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC")]
    )
    assert ok == False, str(extra)
    print("CONFIRM: Static FDB Vlan2:"+mac.lower()+":Port0 is not created in ASIC-DB")

    print("ACTION: Removing static FDB Vlan2:"+mac.lower()+":Port0 from CONFIG-DB")
    remove_entry_tbl(dvs.cdb, "FDB", "Vlan2|"+mac.lower())
    time.sleep(2)


    #Check the mac is removed from config-db
    print("CHECK: Static FDB Vlan2:"+mac.lower()+":Port0 is deleted from Config-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.cdb, "FDB",
                    "Vlan2\|"+mac.lower(),
                    [("port", "Port0")]
    )
    assert mac1_found == False, str(extra)
    print("CONFIRM: Static FDB Vlan2:"+mac.lower()+":Port0 is deleted from Config-DB")
    
    # check that the FDB entry is removed from APP DB
    print("CHECK: Static FDB Vlan2:"+mac.lower()+":Port0 is deleted from APP-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.pdb, "FDB_TABLE",
                    "Vlan2:"+mac.lower(),
                    [("port", "Port0"),
                     ("type", "static"),
                    ]
    )
    assert mac1_found == False, str(extra)
    print("CONFIRM: Static FDB Vlan2:"+mac.lower()+":Port0 is deleted from APP-DB")
    
    print("NEG4 - simulate mac learn event for a port which is not part of vlan")
    bp_eth8 = iface_2_bridge_port_id["Ethernet8"]
    dvs.remove_vlan_member("4", "Ethernet8")

    print("ACTION Creating FDB Vlan4:52-54-00-25-06-E9:Ethernet8 in ASIC-DB")
    create_entry_tbl(
        dvs.adb,
        "ASIC_STATE", "SAI_OBJECT_TYPE_FDB_ENTRY:{\"bvid\":\""+vlan_oid_4+"\",\"mac\":\"52:54:00:25:06:E9\",\"switch_id\":\""+switch_id+"\"}",
        [
            ("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
            ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", bp_eth8),
        ]
    )

    ntf = swsscommon.NotificationProducer(dvs.adb, "NOTIFICATIONS")
    fvp = swsscommon.FieldValuePairs()
    ntf_data = "[{\"fdb_entry\":\"{\\\"bvid\\\":\\\""+vlan_oid_4+"\\\",\\\"mac\\\":\\\"52:54:00:25:06:E9\\\",\\\"switch_id\\\":\\\""+switch_id+"\\\"}\",\"fdb_event\":\"SAI_FDB_EVENT_LEARNED\",\"list\":[{\"id\":\"SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID\",\"value\":\""+bp_eth8+"\"}]}]"
    ntf.send("fdb_event", ntf_data, fvp)

    time.sleep(2)

    # check that the FDB entry was added in ASIC DB
    print("CHECK: FDB Vlan4:52-54-00-25-06-E9:Ethernet8 is created in ASIC-DB")
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "52:54:00:25:06:E9"), ("bvid", vlan_oid_4)],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", bp_eth8)]
    )
    assert ok, str(extra)
    print("CONFIRM: FDB Vlan4:52-54-00-25-06-E9:Ethernet8 is created in ASIC-DB")

    # check that the FDB entry was not added in STATE DB
    print("CHECK: FDB Vlan4:52-54-00-25-06-E9:Ethernet8 is not created in STATE-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
                    "Vlan4:52:54:00:25:06:e9",
                    [("port", "Ethernet8"),
                     ("type", "dynamic"),
                    ]
    )
    assert mac1_found == False, str(extra)
    print("CONFIRM: FDB Vlan4:52-54-00-25-06-E9:Ethernet8 is not created in STATE-DB")

    print("NEG5 - simulate mac learn event for a vlan which does not exist")
    bp_eth12 = iface_2_bridge_port_id["Ethernet12"]
    dvs.remove_vlan_member("10", "Ethernet12")
    dvs.remove_vlan("10")

    print("ACTION: Creating FDB Vlan10:52-54-00-25-06-E9:Ethernet12 in ASIC-DB")
    create_entry_tbl(
        dvs.adb,
        "ASIC_STATE", "SAI_OBJECT_TYPE_FDB_ENTRY:{\"bvid\":\""+vlan_oid_10+"\",\"mac\":\"52:54:00:25:06:E9\",\"switch_id\":\""+switch_id+"\"}",
        [
            ("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
            ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", bp_eth12),
        ]
    )

    ntf = swsscommon.NotificationProducer(dvs.adb, "NOTIFICATIONS")
    fvp = swsscommon.FieldValuePairs()
    ntf_data = "[{\"fdb_entry\":\"{\\\"bvid\\\":\\\""+vlan_oid_10+"\\\",\\\"mac\\\":\\\"52:54:00:25:06:E9\\\",\\\"switch_id\\\":\\\""+switch_id+"\\\"}\",\"fdb_event\":\"SAI_FDB_EVENT_LEARNED\",\"list\":[{\"id\":\"SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID\",\"value\":\""+bp_eth12+"\"}]}]"
    ntf.send("fdb_event", ntf_data, fvp)

    time.sleep(2)

    # check that the FDB entry was added in ASIC DB
    print("CHECK: FDB Vlan10:52-54-00-25-06-E9:Ethernet12 is created in ASIC-DB")
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "52:54:00:25:06:E9"), ("bvid", vlan_oid_10)],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", bp_eth12)]
    )
    assert ok, str(extra)
    print("CONFIRM: FDB Vlan10:52-54-00-25-06-E9:Ethernet12 is created in ASIC-DB")

    # check that the FDB entry was not added in STATE DB
    print("CHECK: FDB Vlan10:52-54-00-25-06-E9:Ethernet12 is not created in STATE-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
                    "Vlan10:52:54:00:25:06:e9",
                    [("port", "Ethernet12"),
                     ("type", "dynamic"),
                    ]
    )
    assert mac1_found == False, str(extra)
    print("CONFIRM: FDB Vlan10:52-54-00-25-06-E9:Ethernet12 is not created in STATE-DB")


    print("NEG6 - simulate mac age event for a vlan which does not exist")

    print("ACTION: Deleting FDB Vlan10:52-54-00-25-06-E9:Ethernet12 from ASIC-DB")
    remove_entry_tbl(dvs.adb, "ASIC_STATE", "SAI_OBJECT_TYPE_FDB_ENTRY:{\"bvid\":\""+vlan_oid_10+"\",\"mac\":\"52:54:00:25:06:E9\",\"switch_id\":\""+switch_id+"\"}")

    ntf = swsscommon.NotificationProducer(dvs.adb, "NOTIFICATIONS")
    fvp = swsscommon.FieldValuePairs()
    ntf_data = "[{\"fdb_entry\":\"{\\\"bvid\\\":\\\""+vlan_oid_10+"\\\",\\\"mac\\\":\\\"52:54:00:25:06:E9\\\",\\\"switch_id\\\":\\\""+switch_id+"\\\"}\",\"fdb_event\":\"SAI_FDB_EVENT_AGED\",\"list\":[{\"id\":\"SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID\",\"value\":\""+bp_eth12+"\"}]}]"
    ntf.send("fdb_event", ntf_data, fvp)

    time.sleep(2)

    # check that the FDB entry is not present ASIC DB
    print("CHECK: FDB Vlan10:52-54-00-25-06-E9:Ethernet12 is not found in ASIC-DB")
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "52:54:00:25:06:E9"), ("bvid", vlan_oid_10)],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", bp_eth12)]
    )
    assert ok == False, str(extra)
    print("CONFIRM: FDB Vlan10:52-54-00-25-06-E9:Ethernet12 is not found in ASIC-DB")

    # check that the FDB entry was not found in STATE DB
    print("CHECK: FDB Vlan10:52-54-00-25-06-E9:Ethernet12 is not found in STATE-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
                    "Vlan10:52:54:00:25:06:e9",
                    [("port", "Ethernet12"),
                     ("type", "dynamic"),
                    ]
    )
    assert mac1_found == False, str(extra)
    print("CONFIRM: FDB Vlan10:52-54-00-25-06-E9:Ethernet12 is not found in STATE-DB")

    print("NEG7 - simulate mac age event for a port which is not part of vlan")

    print("ACTION: Deleting FDB Vlan4:52-54-00-25-06-E9:Ethernet8 from ASIC-DB")
    remove_entry_tbl(dvs.adb, "ASIC_STATE", "SAI_OBJECT_TYPE_FDB_ENTRY:{\"bvid\":\""+vlan_oid_4+"\",\"mac\":\"52:54:00:25:06:E9\",\"switch_id\":\""+switch_id+"\"}")

    ntf = swsscommon.NotificationProducer(dvs.adb, "NOTIFICATIONS")
    fvp = swsscommon.FieldValuePairs()
    ntf_data = "[{\"fdb_entry\":\"{\\\"bvid\\\":\\\""+vlan_oid_4+"\\\",\\\"mac\\\":\\\"52:54:00:25:06:E9\\\",\\\"switch_id\\\":\\\""+switch_id+"\\\"}\",\"fdb_event\":\"SAI_FDB_EVENT_AGED\",\"list\":[{\"id\":\"SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID\",\"value\":\""+bp_eth8+"\"}]}]"
    ntf.send("fdb_event", ntf_data, fvp)

    time.sleep(2)

    # check that the FDB entry is not present ASIC DB
    print("CHECK: FDB Vlan4:52-54-00-25-06-E9:Ethernet8 is not found in ASIC-DB")
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "52:54:00:25:06:E9"), ("bvid", vlan_oid_4)],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", bp_eth8)]
    )
    assert ok == False, str(extra)
    print("CONFIRM: FDB Vlan4:52-54-00-25-06-E9:Ethernet8 is not found in ASIC-DB")

    # check that the FDB entry was not found in STATE DB
    print("CHECK: FDB Vlan4:52-54-00-25-06-E9:Ethernet8 is not found in STATE-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
                    "Vlan4:52:54:00:25:06:e9",
                    [("port", "Ethernet8"),
                     ("type", "dynamic"),
                    ]
    )
    assert mac1_found == False, str(extra)
    print("CONFIRM: FDB Vlan4:52-54-00-25-06-E9:Ethernet8 is not found in STATE-DB")


    print("NEG8 - simulate mac age event for a mac which does not exist")

    bp_eth0 = iface_2_bridge_port_id["Ethernet0"]
    print("ACTION: Deleting FDB Vlan2:52-54-00-25-06-E9:Ethernet0 from ASIC-DB")
    remove_entry_tbl(dvs.adb, "ASIC_STATE", "SAI_OBJECT_TYPE_FDB_ENTRY:{\"bvid\":\""+vlan_oid_2+"\",\"mac\":\"52:54:00:25:06:E9\",\"switch_id\":\""+switch_id+"\"}")

    ntf = swsscommon.NotificationProducer(dvs.adb, "NOTIFICATIONS")
    fvp = swsscommon.FieldValuePairs()
    ntf_data = "[{\"fdb_entry\":\"{\\\"bvid\\\":\\\""+vlan_oid_2+"\\\",\\\"mac\\\":\\\"52:54:00:25:06:E9\\\",\\\"switch_id\\\":\\\""+switch_id+"\\\"}\",\"fdb_event\":\"SAI_FDB_EVENT_AGED\",\"list\":[{\"id\":\"SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID\",\"value\":\""+bp_eth0+"\"}]}]"
    ntf.send("fdb_event", ntf_data, fvp)

    time.sleep(2)

    # check that the FDB entry is not present ASIC DB
    print("CHECK: FDB Vlan2:52-54-00-25-06-E9:Ethernet0 is not found in ASIC-DB")
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", "52:54:00:25:06:E9"), ("bvid", vlan_oid_2)],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", bp_eth0)]
    )
    assert ok == False, str(extra)
    print("CONFIRM: FDB Vlan2:52-54-00-25-06-E9:Ethernet0 is not found in ASIC-DB")

    # check that the FDB entry was not found in STATE DB
    print("CHECK: FDB Vlan2:52-54-00-25-06-E9:Ethernet0 is not found in STATE-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
                    "Vlan2:52:54:00:25:06:e9",
                    [("port", "Ethernet0"),
                     ("type", "dynamic"),
                    ]
    )
    assert mac1_found == False, str(extra)
    print("CONFIRM: FDB Vlan2:52-54-00-25-06-E9:Ethernet0 is not found in STATE-DB")


    print("NEG9 - Add Static MAC M1 to Vlan V1 and Port P1; create V1; assign V1 to P1; remove V1 from P1; remove V1")
    mac = "52:54:00:25:06:EF"
    #dvs.runcmd("config mac add " + mac.lower() + " 10 Ethernet12")
    print("ACTION: Creating static FDB Vlan10|"+mac.lower()+"|Ethernet12 in CONFIG-DB")
    create_entry_tbl(
        dvs.cdb,
        "FDB", "Vlan10|"+mac.lower(),
        [
            ("port", "Ethernet12"),
        ]
    )   
    time.sleep(5)

    # check that the FDB entry was added in Config DB
    print("CHECK: Static FDB Vlan10:"+mac.lower()+":Ethernet12 is created in Config-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.cdb, "FDB",
                    "Vlan10\|"+mac.lower(),
                    [("port", "Ethernet12")]
    )   
    assert mac1_found, str(extra)
    print("CONFIRM: Static FDB Vlan10:"+mac.lower()+":Ethernet12 is created in Config-DB")

    # check that the FDB entry was added in APP DB
    print("CHECK: Static FDB Vlan10:"+mac.lower()+":Ethernet12 is created in APP-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.pdb, "FDB_TABLE",
                    "Vlan10:"+mac.lower(),
                    [("port", "Ethernet12"),
                     ("type", "static")
                    ]
    )   
    assert mac1_found, str(extra)
    print("CONFIRM: Static FDB Vlan10:"+mac.lower()+":Ethernet12 is created in APP-DB")

    # check that the FDB entry is not inserted into ASIC DB
    print("CHECK: Static FDB Vlan10:"+mac.lower()+":Ethernet12 is not created in ASIC-DB")
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", mac.lower())],
            [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC")]
    )   
    assert ok == False, str(extra)
    print("CONFIRM: Static FDB Vlan10:"+mac.lower()+":Ethernet12 is not created in ASIC-DB")

    dvs.create_vlan("10")
    time.sleep(1)
    dvs.create_vlan_member("10", "Ethernet12")
    time.sleep(1)
    # Find the vlan_oid_10 to be used in DB communications
    vlan_oid_10 = dvs.getVlanOid("10")
    assert vlan_oid_10 is not None, "Could not find Vlan_oid"
    print("VLan-10 vlan_oid="+str(vlan_oid_10))
    iface_2_bridge_port_id = dvs.get_map_iface_bridge_port_id(dvs.adb)
    bp_eth12 = iface_2_bridge_port_id["Ethernet12"]
    print("bp_eth12="+str(bp_eth12))

    print("CHECK: Static FDB Vlan10:"+mac+":Ethernet12 is created in ASIC-DB")
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", mac), ("bvid", str(vlan_oid_10))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC"),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", bp_eth12)]
    )
    assert ok, str(extra)
    print("CONFIRM: Static FDB Vlan10:"+mac+":Ethernet12 is created in ASIC-DB")

    # check that the FDB entry was added in STATE DB
    print("CHECK: Static FDB Vlan10:"+mac.lower()+":Ethernet12 is created in STATE-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
                    "Vlan10:"+mac.lower(),
                    [("port", "Ethernet12"),
                     ("type", "static"),
                    ]
    )
    assert mac1_found, str(extra)
    print("CONFIRM: Static FDB Vlan10:"+mac.lower()+":Ethernet12 is created in STATE-DB")

    print("ACTION: Remove vlan member Ethernet12")
    dvs.remove_vlan_member("10", "Ethernet12")
    time.sleep(2)

    print("CHECK: Static FDB Vlan10:"+mac+":Ethernet12 is deleted from ASIC-DB")
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
            [("mac", mac), ("bvid", str(vlan_oid_10))],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_STATIC"),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", bp_eth12)]
    )
    assert ok == False, str(extra)
    print("CONFIRM: Static FDB Vlan10:"+mac+":Ethernet12 is deleted from ASIC-DB")

    # check that the FDB entry was deleted from STATE DB
    print("CHECK: Static FDB Vlan10:"+mac.lower()+":Ethernet12 is deleted from STATE-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
                    "Vlan10:"+mac.lower(),
                    [("port", "Ethernet12"),
                     ("type", "static"),
                    ]
    )
    assert mac1_found == False, str(extra)
    print("CONFIRM: Static FDB Vlan10:"+mac.lower()+":Ethernet12 is deleted from STATE-DB")


    print("ACTION: Remove vlan Vlan10")
    dvs.remove_vlan("10")
    time.sleep(2)

    print("ACTION: Remove FDB Vlan10|"+mac.lower()+" from Config DB")
    remove_entry_tbl(dvs.cdb, "FDB", "Vlan10|"+mac.lower())
    time.sleep(2)

    #Check the mac is removed from config-db
    print("CHECK: Static FDB Vlan10:"+mac.lower()+":Ethernet12 is deleted from Config-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.cdb, "FDB",
                    "Vlan10\|"+mac.lower(),
                    [("port", "Ethernet12")]
    )
    assert mac1_found == False, str(extra)
    print("CONFIRM: Static FDB Vlan10:"+mac.lower()+":Ethernet12 is deleted from Config-DB")

    # check that the FDB entry is removed from APP DB
    print("CHECK: Static FDB Vlan10:"+mac.lower()+":Ethernet12 is deleted from APP-DB")
    mac1_found, extra = dvs.is_table_entry_exists(dvs.pdb, "FDB_TABLE",
                    "Vlan10:"+mac.lower(),
                    [("port", "Ethernet12"),
                     ("type", "static"),
                    ]
    )
    assert mac1_found == False, str(extra)
    print("CONFIRM: Static FDB Vlan10:"+mac.lower()+":Ethernet12 is deleted from APP-DB")

    print("NEG10 - Received move event with invalid bridge-port")
    # Move a FDB entry in ASIC DB
    print("Action: Creating FDB Vlan2:52-54-00-25-06-EB:Ethernet0 in ASIC-DB")
    create_entry_tbl(
        dvs.adb,
        "ASIC_STATE", "SAI_OBJECT_TYPE_FDB_ENTRY:{\"bvid\":\""+vlan_oid_2+"\",\"mac\":\"52:54:00:25:06:EB\",\"switch_id\":\""+switch_id+"\"}",
        [   
            ("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
            ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet0"])]
    )   

    ntf = swsscommon.NotificationProducer(dvs.adb, "NOTIFICATIONS")
    fvp = swsscommon.FieldValuePairs()
    ntf_data = "[{\"fdb_entry\":\"{\\\"bvid\\\":\\\""+vlan_oid_2+"\\\",\\\"mac\\\":\\\"52:54:00:25:06:EB\\\",\\\"switch_id\\\":\\\""+switch_id+"\\\"}\",\"fdb_event\":\"SAI_FDB_EVENT_LEARNED\",\"list\":[{\"id\":\"SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID\",\"value\":\""+iface_2_bridge_port_id["Ethernet0"]+"\"}]}]"
    ntf.send("fdb_event", ntf_data, fvp)

    time.sleep(2)

    print("Action: Moving FDB Vlan2:52-54-00-25-06-EB:Ethernet0 to non-existing bridge-port Ethernet12")
    ntf = swsscommon.NotificationProducer(dvs.adb, "NOTIFICATIONS")
    fvp = swsscommon.FieldValuePairs()
    ntf_data = "[{\"fdb_entry\":\"{\\\"bvid\\\":\\\""+vlan_oid_2+"\\\",\\\"mac\\\":\\\"52:54:00:25:06:EB\\\",\\\"switch_id\\\":\\\""+switch_id+"\\\"}\",\"fdb_event\":\"SAI_FDB_EVENT_MOVE\",\"list\":[{\"id\":\"SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID\",\"value\":\""+bp_eth12+"\"}]}]"
    ntf.send("fdb_event", ntf_data, fvp)

    time.sleep(2)

    print("CHECK: FDB Vlan2:52-54-00-25-06-EB is not Moved in STATE-DB")
    # check that the FDB entry was not moved in STATE DB
    mac2_found, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
                    "Vlan2:52:54:00:25:06:eb",
                    [("port", "Ethernet0"),
                     ("type", "dynamic"),
                    ]   
    )   
    assert mac2_found, str(extra)
    print("CONFIRM: FDB Vlan2:52-54-00-25-06-EB is not Moved in STATE-DB")

    #raw_input("Check at the end")

    dvs.runcmd("sonic-clear fdb all")
    time.sleep(10)

    dvs.remove_vlan_member("2", "Ethernet0")
    dvs.remove_vlan_member("2", "Ethernet4")
    dvs.remove_vlan("2")
    dvs.remove_vlan("4")

