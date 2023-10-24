import time
import os
import pytest

from swsscommon import swsscommon

DVS_ENV = ["HWSKU=Mellanox-SN2700"]

class TestPort(object):
    def test_PortFecOverride(self, dvs, testlog):
        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = dvs.get_asic_db()

        ptbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")

        # set fec
        fvs = swsscommon.FieldValuePairs([("fec","auto")])
        ptbl.set("Ethernet0", fvs)
        fvs = swsscommon.FieldValuePairs([("fec","rs")])
        ptbl.set("Ethernet4", fvs)

        # validate if fec none is pushed to asic db when set first time
        port_oid = adb.port_name_map["Ethernet0"]
        expected_fields = {"SAI_PORT_ATTR_FEC_MODE":"SAI_PORT_FEC_MODE_NONE", "SAI_PORT_ATTR_AUTO_NEG_FEC_MODE_OVERRIDE":"false"}
        adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid, expected_fields)

        # validate if fec rs is pushed to asic db when set first time
        port_oid = adb.port_name_map["Ethernet4"]
        expected_fields = {"SAI_PORT_ATTR_FEC_MODE":"SAI_PORT_FEC_MODE_RS", "SAI_PORT_ATTR_AUTO_NEG_FEC_MODE_OVERRIDE":"true"}
        adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid, expected_fields)

        fvs = swsscommon.FieldValuePairs([("fec","none")])
        ptbl.set("Ethernet0", fvs)
        ptbl.set("Ethernet4", fvs)
        port_oid = adb.port_name_map["Ethernet0"]
        expected_fields = {"SAI_PORT_ATTR_FEC_MODE":"SAI_PORT_FEC_MODE_NONE", "SAI_PORT_ATTR_AUTO_NEG_FEC_MODE_OVERRIDE":"true"}
        adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid, expected_fields)
        port_oid = adb.port_name_map["Ethernet4"]
        expected_fields = {"SAI_PORT_ATTR_FEC_MODE":"SAI_PORT_FEC_MODE_NONE", "SAI_PORT_ATTR_AUTO_NEG_FEC_MODE_OVERRIDE":"true"}
        adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid, expected_fields)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
