import os
import sys
import time
import json
import pytest

from swsscommon import swsscommon
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


class TestFdb(object):
    def test_FdbWarmRestartNotifications(self, dvs, testlog):
        dvs.setup_db()

        dvs.clear_fdb()

        dvs.crm_poll_set("1")
        
        dvs.setReadOnlyAttr('SAI_OBJECT_TYPE_SWITCH', 'SAI_SWITCH_ATTR_AVAILABLE_FDB_ENTRY', '1000')

        time.sleep(2)
        counter_before = dvs.getCrmCounterValue('STATS', 'crm_stats_fdb_entry_used')

        # create vlan; create vlan member
        dvs.create_vlan("6")
        dvs.create_vlan_member("6", "Ethernet64")
        dvs.create_vlan_member("6", "Ethernet68")

        # Put Ethernet72 and Ethernet76 into vlan 7 in untagged mode, they will have pvid of 7
        # and into vlan8 in tagged mode
        dvs.create_vlan("7")
        dvs.create_vlan_member("7", "Ethernet72")
        dvs.create_vlan_member("7", "Ethernet76")

        dvs.create_vlan("8")
        dvs.create_vlan_member_tagged("8", "Ethernet72")
        dvs.create_vlan_member_tagged("8", "Ethernet76")


        # Get mapping between interface name and its bridge port_id
        time.sleep(2)
        iface_2_bridge_port_id = dvs.get_map_iface_bridge_port_id(dvs.adb)

        # check FDB learning mode
        ok, extra = dvs.is_table_entry_exists(dvs.adb, 'ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT',
            iface_2_bridge_port_id["Ethernet64"],
            [("SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE", "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW")])
        assert ok, str(extra)
        ok, extra = dvs.is_table_entry_exists(dvs.adb, 'ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT',
            iface_2_bridge_port_id["Ethernet68"],
            [("SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE", "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW")])
        assert ok, str(extra)

        ok, extra = dvs.is_table_entry_exists(dvs.adb, 'ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT',
            iface_2_bridge_port_id["Ethernet72"],
            [("SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE", "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW")])
        assert ok, str(extra)

        ok, extra = dvs.is_table_entry_exists(dvs.adb, 'ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT',
            iface_2_bridge_port_id["Ethernet76"],
            [("SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE", "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW")])
        assert ok, str(extra)

        # check fdb aging attr
        ok, extra = dvs.all_table_entry_has_no(dvs.adb, 'ASIC_STATE:SAI_OBJECT_TYPE_SWITCH',
            ".*",
            ["SAI_SWITCH_ATTR_FDB_AGING_TIME"])

        # bring up vlan and member
        dvs.set_interface_status("Vlan6", "up")
        dvs.set_interface_status("Vlan7", "up")
        dvs.set_interface_status("Vlan8", "up")

        dvs.add_ip_address("Vlan6", "6.6.6.1/24")
        dvs.add_ip_address("Vlan7", "7.7.7.1/24")
        dvs.add_ip_address("Vlan8", "8.8.8.1/24")

        dvs.set_interface_status("Ethernet64", "up")
        dvs.set_interface_status("Ethernet68", "up")
        dvs.set_interface_status("Ethernet72", "up")
        dvs.set_interface_status("Ethernet76", "up")
        dvs.servers[16].runcmd("ifconfig eth0 6.6.6.6/24 up")
        dvs.servers[16].runcmd("ip route add default via 6.6.6.1")
        dvs.servers[17].runcmd("ifconfig eth0 6.6.6.7/24 up")
        dvs.servers[17].runcmd("ip route add default via 6.6.6.1")

        dvs.servers[18].runcmd("vconfig add eth0 8")
        dvs.servers[18].runcmd("ifconfig eth0.8 8.8.8.6/24 up")
        dvs.servers[18].runcmd("ip route add default via 8.8.8.1")

        dvs.servers[19].runcmd("vconfig add eth0 8")
        dvs.servers[19].runcmd("ifconfig eth0.8 8.8.8.7/24 up")
        dvs.servers[19].runcmd("ip route add default via 8.8.8.1")

        dvs.servers[18].runcmd("ifconfig eth0 7.7.7.6/24 up")
        dvs.servers[18].runcmd("ip route add default via 7.7.7.1")
        dvs.servers[19].runcmd("ifconfig eth0 7.7.7.7/24 up")
        dvs.servers[19].runcmd("ip route add default via 7.7.7.1")

        # get neighbor and arp entry
        time.sleep(2)
        rc = dvs.servers[16].runcmd("ping -c 1 6.6.6.7")
        assert rc == 0
        rc = dvs.servers[17].runcmd("ping -c 1 6.6.6.6")
        assert rc == 0

        # get neighbor and arp entry
        time.sleep(2)
        rc = dvs.servers[18].runcmd("ping -c 1 8.8.8.7")
        assert rc == 0
        rc = dvs.servers[19].runcmd("ping -c 1 8.8.8.6")
        assert rc == 0

        time.sleep(2)
        rc = dvs.servers[18].runcmd("ping -c 1 -I 7.7.7.6 7.7.7.7")
        assert rc == 0
        rc = dvs.servers[19].runcmd("ping -c 1 -I 7.7.7.7 7.7.7.6")
        assert rc == 0

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

        ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                        [],
                        [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                         ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet72"]),
                        ]
        )
        assert ok, str(extra)
        ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                        [],
                        [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                         ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet76"]),
                        ]
        )
        assert ok, str(extra)

        time.sleep(2)
        counter_inserted = dvs.getCrmCounterValue('STATS', 'crm_stats_fdb_entry_used')
        # vlan 6: Ethernet64, Ethernet68;
        # vlan 7: Ethernet72, Ethernet76;
        # vlan 8 (tagged): Ethernet72, Ethernet76;
        # 6 FDB entries wil be created in total
        assert counter_inserted - counter_before == 6

        # check that the FDB entries were inserted into State DB for Ethernet64, Ethernet68 with Vlan6
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

        # check that the FDB entries were inserted into State DB,
        # Vlan7(untagged) in the key for Ethernet72, Ethernet76
        ok, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
                        "Vlan7:.*",
                        [("port", "Ethernet72"),
                         ("type", "dynamic"),
                        ]
        )
        assert ok, str(extra)
        ok, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
                        "Vlan7:*",
                        [("port", "Ethernet76"),
                         ("type", "dynamic"),
                        ]
        )
        assert ok, str(extra)

        # check that the FDB entries were inserted into State DB,
        # Vlan8 (tagged) in the key for Ethernet72, Ethernet76
        ok, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
                        "Vlan8:.*",
                        [("port", "Ethernet72"),
                         ("type", "dynamic"),
                        ]
        )
        assert ok, str(extra)
        ok, extra = dvs.is_table_entry_exists(dvs.sdb, "FDB_TABLE",
                        "Vlan8:*",
                        [("port", "Ethernet76"),
                         ("type", "dynamic"),
                        ]
        )
        assert ok, str(extra)

        # enable warm restart
        dvs.warm_restart_swss("true")

        # freeze orchagent for warm restart
        (exitcode, result) = dvs.runcmd("/usr/bin/orchagent_restart_check")
        assert result == "RESTARTCHECK succeeded\n"
        time.sleep(2)

        try:
            # restart orchagent
            dvs.stop_swss()

            # check FDB learning mode
            ok, extra = dvs.all_table_entry_has(dvs.adb, 'ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT',
                ".*",
                [("SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE", "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DISABLE")])
            assert ok, str(extra)
            # check FDB aging time
            ok, extra = dvs.all_table_entry_has(dvs.adb, 'ASIC_STATE:SAI_OBJECT_TYPE_SWITCH',
                ".*",
                [("SAI_SWITCH_ATTR_FDB_AGING_TIME", "0")])
            assert ok, str(extra)

            dvs.start_swss()
            time.sleep(2)

            # Get mapping between interface name and its bridge port_id
            # Note: they are changed
            iface_2_bridge_port_id = dvs.get_map_iface_bridge_port_id(dvs.adb)

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

            # check that the FDB entries were inserted into ASIC DB
            ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                            [],
                            [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                             ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet72"]),
                            ]
            )
            assert ok, str(extra)
            ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                            [],
                            [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                             ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet76"]),
                            ]
            )
            assert ok, str(extra)

            # check FDB learning mode
            ok, extra = dvs.is_table_entry_exists(dvs.adb, 'ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT',
                iface_2_bridge_port_id["Ethernet64"],
                [("SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE", "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW")])
            assert ok, str(extra)
            ok, extra = dvs.is_table_entry_exists(dvs.adb, 'ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT',
                iface_2_bridge_port_id["Ethernet68"],
                [("SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE", "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW")])
            assert ok, str(extra)

            ok, extra = dvs.is_table_entry_exists(dvs.adb, 'ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT',
                iface_2_bridge_port_id["Ethernet72"],
                [("SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE", "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW")])
            assert ok, str(extra)
            ok, extra = dvs.is_table_entry_exists(dvs.adb, 'ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT',
                iface_2_bridge_port_id["Ethernet76"],
                [("SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE", "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW")])
            assert ok, str(extra)

            time.sleep(2)
            counter_restarted = dvs.getCrmCounterValue('STATS', 'crm_stats_fdb_entry_used')
            assert counter_inserted == counter_restarted

            # check fdb aging attr
            ok, extra = dvs.all_table_entry_has_no(dvs.adb, 'ASIC_STATE:SAI_OBJECT_TYPE_SWITCH',
                ".*",
                ["SAI_SWITCH_ATTR_FDB_AGING_TIME"])

        finally:
            # disable warm restart
            dvs.warm_restart_swss("false")
            # slow down crm polling
            dvs.crm_poll_set("10000")

    def test_FdbAddedAfterMemberCreated(self, dvs, testlog):
        dvs.setup_db()

        dvs.clear_fdb()
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

        vlan_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        bp_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
        vm_before = how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")

        # create vlan
        dvs.create_vlan("2")
        time.sleep(1)

        # Get bvid from vlanid
        ok, bvid = dvs.get_vlan_oid(dvs.adb, "2")
        assert ok, bvid

        # check that the FDB entry wasn't inserted into ASIC DB
        ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                        [("mac", "52:54:00:25:06:E9"), ("bvid", bvid)], [])
        assert ok == False, "The fdb entry leaked to ASIC"

        # create vlan member
        dvs.create_vlan_member("2", "Ethernet0")
        time.sleep(1)

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
        ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                        [("mac", "52:54:00:25:06:E9"), ("bvid", bvid)],
                        [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                         ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet0"])])
        assert ok, str(extra)

        dvs.clear_fdb()
        dvs.remove_vlan_member("2", "Ethernet0")
        dvs.remove_vlan("2")


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
