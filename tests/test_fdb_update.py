import os
import sys
import time
import json
import pytest

from swsscommon import swsscommon
from distutils.version import StrictVersion

class TestFdbUpdate(object):
    def create_entry(self, tbl, key, pairs):
        fvs = swsscommon.FieldValuePairs(pairs)
        tbl.set(key, fvs)
        time.sleep(1)

    def remove_entry(self, tbl, key):
        tbl._del(key)
        time.sleep(1)

    def create_entry_tbl(self, db, table, key, pairs):
        tbl = swsscommon.Table(db, table)
        self.create_entry(tbl, key, pairs)

    def remove_entry_tbl(self, db, table, key):
        tbl = swsscommon.Table(db, table)
        self.remove_entry(tbl, key)

    def create_entry_pst(self, db, table, key, pairs):
        tbl = swsscommon.ProducerStateTable(db, table)
        self.create_entry(tbl, key, pairs)

    def remove_entry_pst(self, db, table, key):
        tbl = swsscommon.ProducerStateTable(db, table)
        self.remove_entry(tbl, key)

    def how_many_entries_exist(self, db, table):
        tbl = swsscommon.Table(db, table)
        return len(tbl.getKeys())

    def get_mac_by_bridge_id(self, dvs, bridge_id):
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY")
        keys = tbl.getKeys()

        mac = []
        for key in keys:
            (status, fvs) = tbl.get(key)
            assert status
            value = dict(fvs)
            if value["SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID"] == bridge_id:
                try:
                    d_key = json.loads(key)
                except ValueError:
                    d_key = json.loads('{' + key + '}')
                mac.append(d_key["mac"])
        return mac

    def test_FDBAddedAndUpdated(self, dvs, testlog):
        dvs.setup_db()
        dvs.clear_fdb()
        time.sleep(2)

        # create a FDB entry in Application DB
        self.create_entry_pst(
            dvs.pdb,
            "FDB_TABLE", "Vlan2:52-54-00-25-06-E9",
            [
                ("port", "Ethernet0"),
                ("type", "dynamic"),
            ]
        )

        # check that the FDB entry wasn't inserted into ASIC DB
        assert self.how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 0, "The fdb entry leaked to ASIC"

        vlan_before = self.how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        bp_before = self.how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
        vm_before = self.how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")

        # create vlan
        self.create_entry_tbl(
            dvs.cdb,
            "VLAN", "Vlan2",
            [
                ("vlanid", "2"),
            ]
        )

        # create vlan member entry in Config db
        self.create_entry_tbl(
            dvs.cdb,
            "VLAN_MEMBER", "Vlan2|Ethernet0",
            [
                ("tagging_mode", "untagged"),
            ]
        )

        # check that the vlan information was propagated
        vlan_after = self.how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
        bp_after = self.how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
        vm_after = self.how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")

        assert vlan_after - vlan_before == 1, "The Vlan2 wasn't created"
        assert bp_after - bp_before == 1, "The bridge port wasn't created"
        assert vm_after - vm_before == 1, "The vlan member wasn't added"

        # Get bvid from vlanid
        ok, bvid = dvs.get_vlan_oid(dvs.adb, "2")
        assert ok, bvid

        # Get mapping between interface name and its bridge port_id
        iface_2_bridge_port_id = dvs.get_map_iface_bridge_port_id(dvs.adb)

        # check that the FDB entry was inserted into ASIC DB
        assert self.how_many_entries_exist(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 1, "The fdb entry wasn't inserted to ASIC"

        ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                                            [("mac", "52:54:00:25:06:E9"), ("bvid", bvid)],
                                            [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                                             ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet0"])])
        assert ok, str(extra)

        # create vlan member entry in Config DB
        self.create_entry_tbl(
            dvs.cdb,
            "VLAN_MEMBER", "Vlan2|Ethernet4",
            [
                ("tagging_mode", "untagged"),
            ]
        )

        # update FDB entry port in Application DB
        self.create_entry_pst(
            dvs.pdb,
            "FDB_TABLE", "Vlan2:52-54-00-25-06-E9",
            [
                ("port", "Ethernet4"),
                ("type", "dynamic"),
            ]
        )

        # Get mapping between interface name and its bridge port_id
        iface_2_bridge_port_id = dvs.get_map_iface_bridge_port_id(dvs.adb)

        ok, extra = dvs.is_fdb_entry_exists(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY",
                                            [("mac", "52:54:00:25:06:E9"), ("bvid", bvid)],
                                            [("SAI_FDB_ENTRY_ATTR_TYPE", "SAI_FDB_ENTRY_TYPE_DYNAMIC"),
                                             ("SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID", iface_2_bridge_port_id["Ethernet4"])])
        assert ok, str(extra)

        # remove FDB entry from Application DB
        self.remove_entry_pst(
            dvs.pdb,
            "FDB_TABLE", "Vlan2:52-54-00-25-06-E9"
        )

        # remove vlan member entry from Config DB
        self.remove_entry_tbl(
            dvs.cdb,
            "VLAN_MEMBER", "Vlan2|Ethernet4"
        )
        self.remove_entry_tbl(
            dvs.cdb,
            "VLAN_MEMBER", "Vlan2|Ethernet0"
        )

        # remove vlan entry from Config DB
        self.remove_entry_tbl(
            dvs.cdb,
            "VLAN", "Vlan2"
        )

    def test_FDBLearnedAndUpdated(self, dvs, testlog):
        dvs.setup_db()

        dvs.clear_fdb()

        # create vlan; create vlan member
        dvs.create_vlan("6")
        dvs.create_vlan_member("6", "Ethernet64")
        dvs.create_vlan_member("6", "Ethernet68")
        dvs.create_vlan_member("6", "Ethernet72")

        # Get mapping between interface name and its bridge port_id
        time.sleep(2)
        iface_2_bridge_port_id = dvs.get_map_iface_bridge_port_id(dvs.adb)

        # bring up vlan and member
        dvs.set_interface_status("Vlan6", "up")

        dvs.add_ip_address("Vlan6", "6.6.6.1/24")

        dvs.set_interface_status("Ethernet64", "up")
        dvs.set_interface_status("Ethernet68", "up")
        dvs.set_interface_status("Ethernet72", "up")

        dvs.servers[16].runcmd("ifconfig eth0 hw ether 00:00:00:00:00:11")
        dvs.servers[16].runcmd("ifconfig eth0 6.6.6.6/24 up")
        dvs.servers[16].runcmd("ip route add default via 6.6.6.1")
        dvs.servers[17].runcmd("ifconfig eth0 hw ether 00:00:00:00:17:11")
        dvs.servers[17].runcmd("ifconfig eth0 6.6.6.7/24 up")
        dvs.servers[17].runcmd("ip route add default via 6.6.6.1")
        time.sleep(2)

        # get neighbor and arp entry
        rc = dvs.servers[16].runcmd("ping -c 1 6.6.6.7")
        assert rc == 0
        rc = dvs.servers[17].runcmd("ping -c 1 6.6.6.6")
        assert rc == 0
        time.sleep(2)

        # check that the FDB entries were inserted into ASIC DB
        Ethernet64_mac = self.get_mac_by_bridge_id(dvs, iface_2_bridge_port_id["Ethernet64"])
        assert "00:00:00:00:00:11" in Ethernet64_mac

        # update FDB entry port in Application DB
        self.create_entry_pst(
            dvs.pdb,
            "FDB_TABLE", "Vlan6:00-00-00-00-00-11",
            [
                ("port", "Ethernet72"),
                ("type", "dynamic"),
            ]
        )

        # check that the FDB entry was updated in ASIC DB
        Ethernet72_mac = self.get_mac_by_bridge_id(dvs, iface_2_bridge_port_id["Ethernet72"])
        assert "00:00:00:00:00:11" in Ethernet72_mac, "Updating fdb entry to Ethernet72 failed"

        Ethernet64_mac = self.get_mac_by_bridge_id(dvs, iface_2_bridge_port_id["Ethernet64"])
        assert "00:00:00:00:00:11" not in Ethernet64_mac, "Updating fdb entry from Ethernet64 failed"

        # remove FDB entry from Application DB
        self.remove_entry_pst(
            dvs.pdb,
            "FDB_TABLE", "Vlan6:00-00-00-00-00-11"
        )

        # restore the default value of the servers
        dvs.servers[16].runcmd("ip route del default via 6.6.6.1")
        dvs.servers[16].runcmd("ifconfig eth0 0 down")
        dvs.servers[17].runcmd("ip route del default via 6.6.6.1")
        dvs.servers[17].runcmd("ifconfig eth0 0 down")
        dvs.servers[18].runcmd("ifconfig eth0 0 down")

        # bring down port
        dvs.set_interface_status("Ethernet64", "down")
        dvs.set_interface_status("Ethernet68", "down")
        dvs.set_interface_status("Ethernet72", "down")

        # remove vlan ip
        dvs.remove_ip_address("Vlan6", "6.6.6.1/24")

        # bring down vlan
        dvs.set_interface_status("Vlan6", "down")

        # remove vlan member; remove vlan
        dvs.remove_vlan_member("6", "Ethernet64")
        dvs.remove_vlan_member("6", "Ethernet68")
        dvs.remove_vlan_member("6", "Ethernet72")
        dvs.remove_vlan("6")

        # clear fdb
        dvs.clear_fdb()

    def test_FDBLearnedAndFlushed(self, dvs, testlog):
        dvs.setup_db()

        dvs.clear_fdb()

        VLAN = "9"
        VLAN_NAME = "Vlan9"
        PORT = "Ethernet80"
        server = 20
        server_mac = "00:00:00:00:20:11"

        # create vlan; create vlan member
        dvs.create_vlan(VLAN)
        dvs.create_vlan_member(VLAN, PORT)
        time.sleep(2)

        # Get mapping between interface name and its bridge port_id
        iface_2_bridge_port_id = dvs.get_map_iface_bridge_port_id(dvs.adb)

        # bring up vlan and member
        dvs.set_interface_status(VLAN_NAME, "up")
        dvs.add_ip_address(VLAN_NAME, "7.7.7.1/24")
        dvs.set_interface_status(PORT, "up")

        dvs.servers[server].runcmd("ifconfig eth0 hw ether {}".format(server_mac))
        dvs.servers[server].runcmd("ifconfig eth0 7.7.7.7/24 up")

        # get neighbor and arp entry
        rc = dvs.servers[server].runcmd("ping -c 1 7.7.7.1")
        assert rc == 0
        time.sleep(2)

        # check that the FDB entries were inserted into ASIC DB
        mac = self.get_mac_by_bridge_id(dvs, iface_2_bridge_port_id[PORT])
        assert server_mac in mac

        # bring down port
        dvs.servers[server].runcmd("ip link set down dev eth0") == 0
        #dvs.set_interface_status(PORT, "down")
        time.sleep(2)

        # fdb should be flushed
        mac = self.get_mac_by_bridge_id(dvs, iface_2_bridge_port_id[PORT])
        assert not mac

        # remove vlan ip
        dvs.remove_ip_address(VLAN_NAME, "7.7.7.1/24")

        # bring down vlan
        dvs.set_interface_status(VLAN_NAME, "down")

        # remove vlan member; remove vlan
        dvs.remove_vlan_member(VLAN, PORT)
        dvs.remove_vlan(VLAN)

        # restore the default value of the servers
        dvs.servers[server].runcmd("ifconfig eth0 0 down")


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
