# This test suite covers the functionality of gearbox feature
import time
import os
import pytest
from swsscommon import swsscommon
from dvslib.dvs_database import DVSDatabase
from dvslib.dvs_common import PollingConfig, wait_for_result

# module specific dvs env variables
DVS_ENV = ["HWSKU=brcm_gearbox_vs"]

class Gearbox(object):
    def __init__(self, dvs):
        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        t = swsscommon.Table(db, "_GEARBOX_TABLE")
        assert len(t.getKeys()) > 0
        sr = t.getTableNameSeparator()

        # "_GEARBOX_TABLE:phy:1"
        # "_GEARBOX_TABLE:phy:1:ports:0"
        # "_GEARBOX_TABLE:phy:1:lanes:200"
        self.phys = {}
        phy_table  = swsscommon.Table(db, sr.join([t.getKeyName(""), "phy"]))
        for i in [x for x in phy_table.getKeys() if sr not in x]:
            (status, fvs) = phy_table.get(i)
            assert status == True
            self.phys[i] = {"attrs" : dict(fvs)}

            port_table = swsscommon.Table(db, sr.join([phy_table.getKeyName(i), "ports"]))
            port_list = [x for x in port_table.getKeys() if sr not in x]
            self.phys[i]["port_table"] = port_table
            self.phys[i]["ports"] = {}
            for j in port_list:
                (status, fvs) = port_table.get(j)
                assert status == True
                self.phys[i]["ports"][j] = dict(fvs)

            lane_table = swsscommon.Table(db, sr.join([phy_table.getKeyName(i), "lanes"]))
            lane_list = [x for x in lane_table.getKeys() if sr not in x]
            self.phys[i]["lanes"] = {}
            for j in lane_list:
                (status, fvs) = lane_table.get(j)
                assert status == True
                self.phys[i]["lanes"][j] = dict(fvs)

        # "_GEARBOX_TABLE:interface:0"
        self.interfaces = {}
        intf_table = swsscommon.Table(db, sr.join([t.getKeyName(""), "interface"]))
        for i in [x for x in intf_table.getKeys() if sr not in x]:
            (status, fvs) = intf_table.get(i)
            assert status == True
            self.interfaces[i] = dict(fvs)

    def SanityCheck(self, testlog):
        """
        Verify data integrity of Gearbox objects in APPL_DB
        """
        for i in self.interfaces:
            phy_id = self.interfaces[i]["phy_id"]
            assert phy_id in self.phys
            assert self.interfaces[i]["index"] in self.phys[phy_id]["ports"]

            for lane in self.interfaces[i]["system_lanes"].split(','):
                assert lane in self.phys[phy_id]["lanes"]
            for lane in self.interfaces[i]["line_lanes"].split(','):
                assert lane in self.phys[phy_id]["lanes"]

class GBAsic(DVSDatabase):
    def __init__(self, db_id: int, connector: str, gearbox: Gearbox):
        DVSDatabase.__init__(self, db_id, connector)
        self.gearbox = gearbox
        self.ports = {}
        self._wait_for_gb_asic_db_to_initialize()

        for connector in self.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT_CONNECTOR"):
            fvs = self.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT_CONNECTOR", connector)
            system_port_oid = fvs.get("SAI_PORT_CONNECTOR_ATTR_SYSTEM_SIDE_PORT_ID")
            line_port_oid = fvs.get("SAI_PORT_CONNECTOR_ATTR_LINE_SIDE_PORT_ID")

            fvs = self.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", system_port_oid)
            system_lanes = fvs.get("SAI_PORT_ATTR_HW_LANE_LIST").split(":")[-1]

            fvs = self.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", line_port_oid)
            line_lanes = fvs.get("SAI_PORT_ATTR_HW_LANE_LIST").split(":")[-1]

            for i in self.gearbox.interfaces:
                intf = self.gearbox.interfaces[i]
                if intf["system_lanes"] == system_lanes:
                    assert intf["line_lanes"] == line_lanes
                    self.ports[intf["index"]] = (system_port_oid, line_port_oid)

        assert len(self.ports) == len(self.gearbox.interfaces)

    def _wait_for_gb_asic_db_to_initialize(self) -> None:
        """Wait up to 30 seconds for the default fields to appear in ASIC DB."""
        def _verify_db_contents():
            if len(self.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")) != \
               len(self.gearbox.phys):
                return (False, None)

            if len(self.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT")) != \
               2 * len(self.gearbox.interfaces):
                return (False, None)

            if len(self.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT_CONNECTOR")) != \
               len(self.gearbox.interfaces):
                return (False, None)

            return (True, None)

        # Verify that GB ASIC DB has been fully initialized
        init_polling_config = PollingConfig(2, 30, strict=True)
        wait_for_result(_verify_db_contents, init_polling_config)

@pytest.fixture(scope="module")
def gearbox(dvs):
    return Gearbox(dvs)

@pytest.fixture(scope="module")
def gbasic(dvs, gearbox):
    return GBAsic(swsscommon.GB_ASIC_DB, dvs.redis_sock, gearbox)

@pytest.fixture(scope="module")
def enable_port_counter(dvs):
    flex_counter_table = swsscommon.Table(dvs.get_config_db().db_connection,
        "FLEX_COUNTER_TABLE")

    # Enable port counter
    flex_counter_table.hset("PORT", "FLEX_COUNTER_STATUS", "enable")
    yield
    # Disable port counter
    flex_counter_table.hdel("PORT", "FLEX_COUNTER_STATUS")

class TestGearbox(object):
    def test_GearboxSanity(self, gearbox, testlog):
        gearbox.SanityCheck(testlog)

    def test_GearboxCounter(self, dvs, gbasic, enable_port_counter, testlog):
        counters_db = DVSDatabase(swsscommon.COUNTERS_DB, dvs.redis_sock)
        gb_counters_db = DVSDatabase(swsscommon.GB_COUNTERS_DB, dvs.redis_sock)

        intf = gbasic.gearbox.interfaces["0"]
        port_oid = counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")[intf["name"]]
        system_port_oid, line_port_oid = gbasic.ports["0"]

        fvs = gb_counters_db.wait_for_entry("COUNTERS", system_port_oid)
        assert fvs.get("SAI_PORT_STAT_IF_OUT_ERRORS")

        fvs = gb_counters_db.wait_for_entry("COUNTERS", line_port_oid)
        assert fvs.get("SAI_PORT_STAT_IF_IN_ERRORS")

        fvs = counters_db.wait_for_entry("COUNTERS", port_oid)
        assert fvs.get("SAI_PORT_STAT_IF_IN_ERRORS")

        fvs = counters_db.wait_for_entry("COUNTERS", port_oid)
        assert fvs.get("SAI_PORT_STAT_IF_IN_ERRORS")

    def test_GbAsicFEC(self, gbasic, testlog):

        # set fec rs on port 0 of phy 1
        fvs = swsscommon.FieldValuePairs([("system_fec","rs")])
        gbasic.gearbox.phys["1"]["port_table"].set("0", fvs)
        fvs = swsscommon.FieldValuePairs([("line_fec","rs")])
        gbasic.gearbox.phys["1"]["port_table"].set("0", fvs)

        """FIXME: uncomment it after GearboxOrch is implemented
        # validate if fec rs is pushed to system/line port in gb asic db
        system_port_oid, line_port_oid = gbasic.ports["0"]
        expected_fields = {"SAI_PORT_ATTR_FEC_MODE":"SAI_PORT_FEC_MODE_RS"}
        gbasic.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", \
                system_port_oid, expected_fields)
        gbasic.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", \
                line_port_oid, expected_fields)
        """


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
