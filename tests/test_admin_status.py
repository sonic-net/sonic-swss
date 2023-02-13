import time
import pytest

from swsscommon import swsscommon


class TestAdminStatus(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        self.sdb = swsscommon.DBConnector(6, dvs.redis_sock, 0)

    def set_admin_status(self, port, admin_status):
        assert admin_status == "up" or admin_status == "down"
        tbl = swsscommon.Table(self.cdb, "PORT")
        fvs = swsscommon.FieldValuePairs([("admin_status", admin_status)])
        tbl.set(port, fvs)
        time.sleep(1)

    def create_port_channel(self, dvs, alias):
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL")
        fvs = swsscommon.FieldValuePairs([("admin_status", "up"),
                                          ("mtu", "9100")])
        tbl.set(alias, fvs)
        time.sleep(1)

    def remove_port_channel(self, dvs, alias):
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL")
        tbl._del(alias)
        time.sleep(1)

    def add_port_channel_members(self, dvs, lag, members):
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL_MEMBER")
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        for member in members:
            tbl.set(lag + "|" + member, fvs)
            time.sleep(1)

    def remove_port_channel_members(self, dvs, lag, members):
        tbl = swsscommon.Table(self.cdb, "PORTCHANNEL_MEMBER")
        for member in members:
            tbl._del(lag + "|" + member)
            time.sleep(1)

    def check_admin_status(self, dvs, port, admin_status):
        assert admin_status == "up" or admin_status == "down"
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = tbl.get(dvs.asicdb.portnamemap[port])
        assert status == True
        assert "SAI_PORT_ATTR_ADMIN_STATE" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_ADMIN_STATE":
                assert fv[1] == "true" if admin_status == "up" else "false"

    def check_host_tx_ready_status(self, dvs, port, admin_status):
        assert admin_status == "up" or admin_status == "down"
        ptbl = swsscommon.Table(self.sdb, "PORT_TABLE")
        (status, fvs) = ptbl.get(port)
        assert status == True
        assert "host_tx_ready" in [fv[0] for fv in fvs]
        for fv in fvs:
            if fv[0] == "host_tx_ready":
                assert fv[1] == "true" if admin_status == "up" else "false"

    def test_PortChannelMemberAdminStatus(self, dvs, testlog):
        self.setup_db(dvs)

        # create port channel
        self.create_port_channel(dvs, "PortChannel6")

        # add port channel members
        self.add_port_channel_members(dvs, "PortChannel6",
                ["Ethernet0", "Ethernet4", "Ethernet8"])

        # configure admin status to interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "down")
        self.set_admin_status("Ethernet8", "up")

        # check ASIC port database
        self.check_admin_status(dvs, "Ethernet0", "up")
        self.check_admin_status(dvs, "Ethernet4", "down")
        self.check_admin_status(dvs, "Ethernet8", "up")

        # remove port channel members
        self.remove_port_channel_members(dvs, "PortChannel6",
                ["Ethernet0", "Ethernet4", "Ethernet8"])

        # remove port channel
        self.remove_port_channel(dvs, "PortChannel6")

    def test_PortHostTxReadiness(self, dvs, testlog):
        self.setup_db(dvs)

        # configure admin status to interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "down")
        self.set_admin_status("Ethernet8", "up")

        # check ASIC port database
        self.check_admin_status(dvs, "Ethernet0", "up")
        self.check_admin_status(dvs, "Ethernet4", "down")
        self.check_admin_status(dvs, "Ethernet8", "up")

        # check host readiness status in PORT TABLE of STATE-DB
        self.check_host_tx_ready_status(dvs, "Ethernet0", "up")
        self.check_host_tx_ready_status(dvs, "Ethernet4", "down")
        self.check_host_tx_ready_status(dvs, "Ethernet8", "up")


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
