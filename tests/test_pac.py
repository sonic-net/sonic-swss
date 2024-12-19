import time

from swsscommon import swsscommon

def create_entry(tbl, key, pairs):
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)

    # FIXME: better to wait until DB create them
    time.sleep(1)

def remove_entry(tbl, key):
    tbl._del(key)
    time.sleep(1)

def create_entry_tbl(db, table, key, pairs):
    tbl = swsscommon.Table(db, table)
    create_entry(tbl, key, pairs)

def remove_entry_tbl(db, table, key):
    tbl = swsscommon.Table(db, table)
    remove_entry(tbl, key)

def create_entry_pst(db, table, key, pairs):
    tbl = swsscommon.ProducerStateTable(db, table)
    create_entry(tbl, key, pairs)

def how_many_entries_exist(db, table):
    tbl =  swsscommon.Table(db, table)
    return len(tbl.getKeys())

def get_port_oid(db, port_name):
    port_map_tbl = swsscommon.Table(db, 'COUNTERS_PORT_NAME_MAP')
    for k in port_map_tbl.get('')[1]:
        if k[0] == port_name:
            return k[1]
    return None

def get_bridge_port_oid(db, port_oid):
    tbl = swsscommon.Table(db, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
    for key in tbl.getKeys():
        status, data = tbl.get(key)
        assert status
        values = dict(data)
        if port_oid == values["SAI_BRIDGE_PORT_ATTR_PORT_ID"]:
            return key
    return None

def check_learn_mode_in_asicdb(db, interface_oid, learn_mode):
    # Get bridge port oid
    bridge_port_oid = get_bridge_port_oid(db, interface_oid)
    assert bridge_port_oid is not None

    tbl = swsscommon.Table(db, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
    (status, fvs) = tbl.get(bridge_port_oid)
    assert status == True
    values = dict(fvs)
    if values["SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE"] == learn_mode:
        return True
    else:
        return False

class TestPac(object):
    def test_PacvlanMemberAndFDBAddRemove(self, dvs, testlog):
        dvs.setup_db()
        time.sleep(2)

        vlan_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        bp_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
        vm_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")

        # create vlan
        dvs.create_vlan("2")
        time.sleep(1)

        # Get bvid from vlanid
        ok, bvid = dvs.get_vlan_oid(dvs.adb, "2")
        assert ok, bvid

        dvs.create_vlan("3")
        time.sleep(1)

        # create vlan member
        dvs.create_vlan_member("3", "Ethernet0")
        time.sleep(1)

        # create a Vlan member entry in Oper State DB
        create_entry_tbl(
            dvs.sdb,
            "OPER_VLAN_MEMBER", "Vlan2|Ethernet0",
            [
                ("tagging_mode", "untagged"),
            ]
        )
        
        # check that the vlan information was propagated
        vlan_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        bp_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
        vm_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")

        assert vlan_after - vlan_before == 2, "The Vlan2 wasn't created"
        assert bp_after - bp_before == 1, "The bridge port wasn't created"
        assert vm_after - vm_before == 1, "The vlan member wasn't added"

        # Add FDB entry in Oper State DB
        create_entry_tbl(
            dvs.sdb,
            "OPER_FDB", "Vlan2|00:00:00:00:00:01",
            [
                ("port", "Ethernet0"),
                ("type", "dynamic"),
                ("discard", "false"),
            ]
        )
        # Get mapping between interface name and its bridge port_id
        iface_2_bridge_port_id = dvs.get_map_iface_bridge_port_id(dvs.adb)

        # check that the FDB entry was inserted into ASIC DB
        ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                        [("mac", "00:00:00:00:00:01"), ("bvid", bvid)],
                        [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                         ("SAI_FDB_ENTRY_ATTR_PACKET_ACTION", "SAI_PACKET_ACTION_FORWARD"),
                         ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet0"])])

        assert ok, str(extra)

        # Remove FDB entry in Oper State DB
        remove_entry_tbl(
            dvs.sdb,
            "OPER_FDB", "Vlan2|00:00:00:00:00:01"
        )

        # check that the FDB entry was removed from ASIC DB
        ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                        [("mac", "00:00:00:00:00:01"), ("bvid", bvid)], [])
        assert ok == False, "The fdb entry still exists in ASIC"

        # remove Vlan member entry in Oper State DB
        remove_entry_tbl(
            dvs.sdb,
            "OPER_VLAN_MEMBER", "Vlan2|Ethernet0"
        )
        # check that the vlan information was propagated
        vlan_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        bp_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
        vm_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")

        assert vlan_after - vlan_before == 2, "The Vlan2 wasn't created"
        assert bp_after - bp_before == 1, "The bridge port wasn't created"
        assert vm_after - vm_before == 1, "The vlan member wasn't added"
        
        dvs.remove_vlan("2")
        dvs.remove_vlan_member("3", "Ethernet0")
        dvs.remove_vlan("3")

        vlan_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        bp_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
        vm_after = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")

        assert vlan_after - vlan_before == 0, "The Vlan2 wasn't removed"
        assert bp_after - bp_before == 0, "The bridge port wasn't removed"
        assert vm_after - vm_before == 0, "The vlan member wasn't removed"

    def test_PacPortLearnMode(self, dvs, testlog):
        dvs.setup_db()
        time.sleep(2)

        # create vlan
        dvs.create_vlan("2")
        time.sleep(1)

        # create vlan member
        dvs.create_vlan_member("2", "Ethernet0")
        time.sleep(1)

        cntdb = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
        # get port oid
        port_oid = get_port_oid(cntdb, "Ethernet0")
        assert port_oid is not None

        # check asicdb before setting mac learn mode; The default learn_mode value is SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW.
        status = check_learn_mode_in_asicdb(dvs.adb, port_oid, "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW")
        assert status == True

        # Set port learn mode to CPU
        create_entry_tbl(
            dvs.sdb,
            "OPER_PORT", "Ethernet0",
            [
                ("learn_mode", "cpu_trap"),
            ]
        )
        status = check_learn_mode_in_asicdb(dvs.adb, port_oid, "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_CPU_TRAP")
        assert status == True

        # Set port learn mode back to default
        remove_entry_tbl(
            dvs.sdb,
            "OPER_PORT", "Ethernet0"
        )
        status = check_learn_mode_in_asicdb(dvs.adb, port_oid, "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW")
        assert status == True
        dvs.remove_vlan_member("2", "Ethernet0")
        dvs.remove_vlan("2")

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
