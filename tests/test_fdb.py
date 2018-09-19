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

def test_FDBAddedAfterMemberCreated(dvs):
    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    # create a FDB entry in Application DB
    create_entry_pst(
        appl_db,
        "FDB_TABLE", "Vlan2:52-54-00-25-06-E9",
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
        "VLAN", "Vlan2",
        [
            ("vlanid", "2"),
        ]
    )

    # create vlan member entry in application db
    create_entry_tbl(
        conf_db,
        "VLAN_MEMBER", "Vlan2|Ethernet0",
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

    ok, extra = dvs.is_fdb_entry_exists(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                    [("mac", "52-54-00-25-06-E9"), ("vlan", "2")],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet0"]),
                     ('SAI_FDB_ENTRY_ATTR_PACKET_ACTION', 'SAI_PACKET_ACTION_FORWARD')]
    )
    assert ok, str(extra)


def test_fdb_notifications(dvs):
    dvs.setup_db(dvs)

    # create vlan; create vlan member
    dvs.create_vlan(dvs, "6")
    dvs.create_vlan_member("6", "Ethernet64")
    dvs.create_vlan_member("6", "Ethernet68")

    # bring up vlan and member
    dvs.set_interface_status("Vlan6", "up")
    dvs.add_ip_address("Vlan6", "6.6.6.1/24")
    dvs.set_interface_status("Ethernet64", "up")
    dvs.set_interface_status("Ethernet68", "up")
    dvs.servers[16].runcmd("ifconfig eth0 6.6.6.6/24 up")
    dvs.servers[16].runcmd("ip route add default via 6.6.6.1")
    dvs.servers[17].runcmd("ifconfig eth0 6.6.6.7/24 up")
    dvs.servers[17].runcmd("ip route add default via 6.6.6.1")

    # get neighbor and arp entry
    rc = dvs.servers[16].runcmd("ping -c 1 6.6.6.7")
    assert rc == 0

    # Get mapping between interface name and its bridge port_id
    iface_2_bridge_port_id = get_map_iface_bridge_port_id(dvs.adb, dvs)

    # check that the FDB entries were inserted into ASIC DB
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                    [],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet64"]),
                    ]
    )
    assert ok, str(extra)
    ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                    [],
                    [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                     ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet68"]),
                    ]
    )
    assert ok, str(extra)

    # check that the FDB entries were inserted into State DB
    ok, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
                    "Vlan6:.*",
                    [("port", "Ethernet64"),
                     ("type", "dynamic"),
                    ]
    )
    assert ok, str(extra)
    ok, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
                    "Vlan6:*",
                    [("port", "Ethernet68"),
                     ("type", "dynamic"),
                    ]
    )
    assert ok, str(extra)

    # enable warm restart
    # TODO: use cfg command to config it
    create_entry_tbl(
        dvs.cdb,
        swsscommon.CFG_WARM_RESTART_TABLE_NAME, "swss",
        [
            ("enable", "true"),
        ]
    )

    try:
        # restart orchagent
        dvs.runcmd(['sh', '-c', 'supervisorctl restart orchagent'])
        time.sleep(2)

        # Get mapping between interface name and its bridge port_id
        # Note: they are changed
        iface_2_bridge_port_id = get_map_iface_bridge_port_id(dvs.adb, dvs)

        # check that the FDB entries were inserted into ASIC DB
        ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                        [],
                        [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                         ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet64"]),
                        ]
        )
        assert ok, str(extra)
        ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                        [],
                        [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                         ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet68"]),
                        ]
        )
        assert ok, str(extra)

    finally:
        # disable warm restart
        # TODO: use cfg command to config it
        create_entry_tbl(
            dvs.cdb,
            swsscommon.CFG_WARM_RESTART_TABLE_NAME, "swss",
            [
                ("enable", "false"),
            ]
        )
