from swsscommon import swsscommon


class TestPortAutoNeg(object):
    def test_PortUnreliableLosForce(self, dvs, testlog):

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = dvs.get_asic_db()

        tbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        fvs = swsscommon.FieldValuePairs([("unreliable_los","off")])
        tbl.set("Ethernet0", fvs)

        tbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        fvs = swsscommon.FieldValuePairs([("unreliable_los","on")])
        tbl.set("Ethernet4", fvs)

        # validate if unreliable false is pushed to asic db when set first time
        port_oid = adb.port_name_map["Ethernet0"]
        expected_fields = {'NULL': 'NULL', 'SAI_PORT_ATTR_ADMIN_STATE': 'false', 'SAI_PORT_ATTR_AUTO_NEG_MODE': 'true', 'SAI_PORT_ATTR_MTU': '9122', 'SAI_PORT_ATTR_SPEED': '100000', 'SAI_PORT_ATTR_UNRELIABLE_LOS': 'false'}
        adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid, expected_fields)

        # validate if unreliable true is pushed to asic db when set first time
        port_oid = adb.port_name_map["Ethernet4"]
        expected_fields = {'NULL': 'NULL', 'SAI_PORT_ATTR_ADMIN_STATE': 'false', 'SAI_PORT_ATTR_AUTO_NEG_MODE': 'true', 'SAI_PORT_ATTR_MTU': '9122', 'SAI_PORT_ATTR_SPEED': '100000', 'SAI_PORT_ATTR_UNRELIABLE_LOS': 'true'}
        adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid, expected_fields)

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
