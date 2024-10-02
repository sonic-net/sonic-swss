import time
import pytest

from swsscommon import swsscommon


class TestAdminStatus(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.countdb = swsscommon.DBConnector(2, dvs.redis_sock, 0)
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

    def update_host_tx_ready_status(self, dvs, port_id, switch_id, admin_state):
        host_tx_ready = "SAI_PORT_HOST_TX_READY_STATUS_READY" if admin_state == "up" else "SAI_PORT_HOST_TX_READY_STATUS_NOT_READY"
        ntf = swsscommon.NotificationProducer(dvs.adb, "NOTIFICATIONS")
        fvp = swsscommon.FieldValuePairs()
        ntf_data =  "[{\"host_tx_ready_status\":\""+host_tx_ready+"\",\"port_id\":\""+port_id+"\",\"switch_id\":\""+switch_id+"\"}]"
        ntf.send("port_host_tx_ready", ntf_data, fvp)

    def get_port_id(self, dvs, port_name):
        port_name_map = swsscommon.Table(self.countdb, "COUNTERS_PORT_NAME_MAP")
        status, returned_value = port_name_map.hget("", port_name)
        assert status == True
        return returned_value

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
        dvs.setup_db()
        self.setup_db(dvs)

        #Find switch_id
        switch_id = dvs.getSwitchOid()

        # configure admin status to interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "down")
        self.set_admin_status("Ethernet8", "up")

        # check ASIC port database
        self.check_admin_status(dvs, "Ethernet0", "up")
        self.check_admin_status(dvs, "Ethernet4", "down")
        self.check_admin_status(dvs, "Ethernet8", "up")

        self.update_host_tx_ready_status(dvs, self.get_port_id(dvs, "Ethernet0") , switch_id, "up")
        self.update_host_tx_ready_status(dvs, self.get_port_id(dvs, "Ethernet4") , switch_id, "down")
        self.update_host_tx_ready_status(dvs, self.get_port_id(dvs, "Ethernet8") , switch_id, "up")
        time.sleep(3)

        # check host readiness status in PORT TABLE of STATE-DB
        self.check_host_tx_ready_status(dvs, "Ethernet0", "up")
        self.check_host_tx_ready_status(dvs, "Ethernet4", "down")
        self.check_host_tx_ready_status(dvs, "Ethernet8", "up")


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
