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

def test_fdb_notifications(dvs):
    dvs.setup_db()

    #dvs.runcmd("sonic-clear fdb all")

    dvs.runcmd("crm config polling interval 1")
    dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_FDB_ENTRY', '1000')

    time.sleep(2)
    counter_before = dvs.getCrmCounterValue('STATS', 'crm_stats_fdb_entry_used')

    # create vlan; create vlan member
    dvs.create_vlan("6")
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
    time.sleep(2)
    rc = dvs.servers[16].runcmd("ping -c 1 6.6.6.7")
    assert rc == 0
    rc = dvs.servers[17].runcmd("ping -c 1 6.6.6.6")
    assert rc == 0

    # Get mapping between interface name and its bridge port_id
    time.sleep(2)
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

    time.sleep(2)
    counter_inserted = dvs.getCrmCounterValue('STATS', 'crm_stats_fdb_entry_used')
    assert counter_inserted - counter_before == 2

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
        dvs.stop_swss()
        dvs.start_swss()
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

        time.sleep(2)
        counter_restarted = dvs.getCrmCounterValue('STATS', 'crm_stats_fdb_entry_used')
        assert counter_inserted == counter_restarted

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
