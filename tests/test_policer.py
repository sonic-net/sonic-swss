import platform
import pytest
import time

from swsscommon import swsscommon


class TestPolicer(object):
    def test_PolicerBasic(self, dvs, testlog):
        dvs.setup_db()
        policer = "POLICER"

        # create policer
        tbl = swsscommon.Table(dvs.cdb, "POLICER")
        fvs = swsscommon.FieldValuePairs([("meter_type", "packets"),
                                          ("mode", "sr_tcm"),
                                          ("cir", "600"),
                                          ("cbs", "600"),
                                          ("red_packet_action", "drop")])
        tbl.set(policer, fvs)
        time.sleep(1)

        # check asic database
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_POLICER")
        policer_entries = tbl.getKeys()
        assert len(policer_entries) == 1

        (status, fvs) = tbl.get(policer_entries[0])
        assert status == True
        assert len(fvs) == 5
        for fv in fvs:
            if fv[0] == "SAI_POLICER_ATTR_CBS":
                assert fv[1] == "600"
            if fv[0] == "SAI_POLICER_ATTR_CIR":
                assert fv[1] == "600"
            if fv[0] == "SAI_POLICER_ATTR_RED_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_DROP"
            if fv[0] == "SAI_POLICER_ATTR_MODE":
                assert fv[1] == "SAI_POLICER_MODE_SR_TCM"
            if fv[0] == "SAI_POLICER_ATTR_METER_TYPE":
                assert fv[1] == "SAI_METER_TYPE_PACKETS"

        # update cir
        tbl = swsscommon.Table(dvs.cdb, "POLICER")
        fvs = swsscommon.FieldValuePairs([("cir", "800")])
        tbl.set(policer, fvs)
        time.sleep(1)

        # check asic database
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_POLICER")
        (status, fvs) = tbl.get(policer_entries[0])
        assert status == True
        assert len(fvs) == 5
        for fv in fvs:
            if fv[0] == "SAI_POLICER_ATTR_CBS":
                assert fv[1] == "600"
            if fv[0] == "SAI_POLICER_ATTR_CIR":
                assert fv[1] == "800" # updated
            if fv[0] == "SAI_POLICER_ATTR_RED_PACKET_ACTION":
                assert fv[1] == "SAI_PACKET_ACTION_DROP"
            if fv[0] == "SAI_POLICER_ATTR_MODE":
                assert fv[1] == "SAI_POLICER_MODE_SR_TCM"
            if fv[0] == "SAI_POLICER_ATTR_METER_TYPE":
                assert fv[1] == "SAI_METER_TYPE_PACKETS"

        # remove policer
        tbl = swsscommon.Table(dvs.cdb, "POLICER")
        tbl._del(policer)
        time.sleep(1)

        # check asic database
        tbl = swsscommon.Table(dvs.adb, "ASIC_STATE:SAI_OBJECT_TYPE_POLICER")
        policer_entries = tbl.getKeys()
        assert len(policer_entries) == 0
