import time
import pytest

from swsscommon import swsscommon


# The test check that the ports will be up, when the admin state is UP by conf db.
class TestPortBuffer(object):
    def test_PortsAreUpAfterBuffers(self, dvs, testlog):
        num_ports = 32
        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        conf_port_table = swsscommon.Table(conf_db, "PORT")
        asic_port_table = swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")

        # enable all ports
        fvs = swsscommon.FieldValuePairs([("admin_status", "up")])
        for i in range(0, num_ports):
            conf_port_table.set("Ethernet%d" % (i*4), fvs)

        time.sleep(5)

        # check that the ports are enabled in ASIC
        asic_port_records = asic_port_table.getKeys()
        assert len(asic_port_records) == (num_ports + 1), "Number of port records doesn't match number of ports" # +CPU port
        num_set = 0
        for k in asic_port_records:
            status, fvs = asic_port_table.get(k)
            assert status, "Got an error when get a key"
            for fv in fvs:
                if fv[0] == "SAI_PORT_ATTR_ADMIN_STATE":
                    assert fv[1] == "true", "The port isn't UP as expected"
                    num_set += 1
        # make sure that state is set for all "num_ports" ports
        assert num_set == num_ports, "Not all ports are up"
# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
