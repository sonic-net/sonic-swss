import time
import os
import pytest

from swsscommon import swsscommon


class TestPortMacLearn(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        self.cntdb = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)

    def get_learn_mode_map(self):
        learn_mode_map = { "drop": "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DROP",
                           "disable": "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DISABLE",
                           "hardware": "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW",
                           "cpu_trap": "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_CPU_TRAP",
                           "cpu_log": "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_CPU_LOG",
                           "notification": "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_FDB_NOTIFICATION"}
        return learn_mode_map

    def get_port_oid(self, port_name):
        port_map_tbl = swsscommon.Table(self.cntdb, 'COUNTERS_PORT_NAME_MAP')
        for k in port_map_tbl.get('')[1]:
            if k[0] == port_name:
                return k[1]
        return None

    def get_bridge_port_oid(self, port_oid):
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
        for key in tbl.getKeys():
            status, data = tbl.get(key)
            assert status
            values = dict(data)
            if port_oid == values["SAI_BRIDGE_PORT_ATTR_PORT_ID"]:
                return key
        return None

    def check_learn_mode_in_appdb(self, table, interface, learn_mode):
        (status, fvs) = table.get(interface)
        assert status == True
        for fv in fvs:
            if fv[0] == "learn_mode":
                if fv[1] == learn_mode:
                    return True
        return False

    def check_learn_mode_in_asicdb(self, interface_oid, learn_mode):
        # Get bridge port oid
        bridge_port_oid = self.get_bridge_port_oid(interface_oid)
        assert bridge_port_oid is not None

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
        (status, fvs) = tbl.get(bridge_port_oid)
        assert status == True
        values = dict(fvs)
        if values["SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE"] == learn_mode:
            return True
        else:
            return False

    def test_PortMacLearnMode(self, dvs, testlog):
        self.setup_db(dvs)

        # create vlan
        tbl = swsscommon.Table(self.cdb, "VLAN")
        fvs = swsscommon.FieldValuePairs([("vlanid", "2")])
        tbl.set("Vlan2", fvs)
        time.sleep(1)

        # create vlan member entry in application db
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        fvs = swsscommon.FieldValuePairs([("tagging_mode", "untagged")])
        tbl.set("Vlan2|Ethernet8", fvs)
        time.sleep(1)

        # get port oid
        port_oid = self.get_port_oid("Ethernet8")
        assert port_oid is not None

        # check asicdb before setting mac learn mode; The default learn_mode value is SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW.
        status = self.check_learn_mode_in_asicdb(port_oid, "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW")
        assert status == True

        learn_mode_map = self.get_learn_mode_map()
        for key, value in learn_mode_map.items():
            # set MAC learn mode to port
            tbl = swsscommon.Table(self.cdb, "PORT")
            fvs = swsscommon.FieldValuePairs([("learn_mode", key)])
            tbl.set("Ethernet8", fvs)
            time.sleep(1)

            # check application database
            tbl = swsscommon.Table(self.pdb, "PORT_TABLE")
            status = self.check_learn_mode_in_appdb(tbl, "Ethernet8", key)
            assert status == True

            # check ASIC bridge port database
            status = self.check_learn_mode_in_asicdb(port_oid, value)
            assert status == True

        # set default learn mode for Ethernet8
        tbl = swsscommon.Table(self.cdb, "PORT")
        fvs = swsscommon.FieldValuePairs([("learn_mode", "hardware")])
        tbl.set("Ethernet8", fvs)
        time.sleep(1)

        # remove vlan member
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        tbl._del("Vlan2|Ethernet8")
        time.sleep(1)

        # remove vlan
        tbl = swsscommon.Table(self.cdb, "VLAN")
        tbl._del("Vlan2")
        time.sleep(1)

    def test_PortchannelMacLearnMode(self, dvs, testlog):
        self.setup_db(dvs)

        #create portchannel
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL")
        fvs = swsscommon.FieldValuePairs([("admin_status", "up"),
                                          ("mtu", "9100")])
        tbl.set("PortChannel001", fvs)
        time.sleep(1)

        # create vlan
        tbl = swsscommon.Table(self.cdb, "VLAN")
        fvs = swsscommon.FieldValuePairs([("vlanid", "3")])
        tbl.set("Vlan3", fvs)
        time.sleep(1)

        # create vlan member entry in application db
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        fvs = swsscommon.FieldValuePairs([("tagging_mode", "untagged")])
        tbl.set("Vlan3|PortChannel001", fvs)
        time.sleep(1)

        # get PortChannel oid; When sonic-swss pr885 is complete, you can get oid directly from COUNTERS_LAG_NAME_MAP, which would be better.
        lag_tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_LAG")
        lag_entries = lag_tbl.getKeys()
        # At this point there should be only one lag in the system, which is PortChannel001.
        assert len(lag_entries) == 1
        lag_oid = lag_entries[0]

        # check asicdb before setting mac learn mode; The default learn_mode value is SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW.
        status = self.check_learn_mode_in_asicdb(lag_oid, "SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW")
        assert status == True

        learn_mode_map = self.get_learn_mode_map()
        for key, value in learn_mode_map.items():
            # set mac learn mode to PortChannel
            tbl = swsscommon.Table(self.cdb, "PORTCHANNEL")
            fvs = swsscommon.FieldValuePairs([("learn_mode", key)])
            tbl.set("PortChannel001", fvs)
            time.sleep(1)

            # check application database
            tbl = swsscommon.Table(self.pdb, "LAG_TABLE")
            status = self.check_learn_mode_in_appdb(tbl, "PortChannel001", key)
            assert status == True

            # check ASIC bridge port database
            status = self.check_learn_mode_in_asicdb(lag_oid, value)
            assert status == True

        # remove vlan member
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        tbl._del("Vlan3|PortChannel001")
        time.sleep(1)

        # create vlan
        tbl = swsscommon.Table(self.cdb, "VLAN")
        tbl._del("Vlan3")
        time.sleep(1)

        # remove PortChannel
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL")
        tbl._del("PortChannel001")
        time.sleep(1)
