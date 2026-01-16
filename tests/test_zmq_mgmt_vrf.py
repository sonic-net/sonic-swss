import time

from swsscommon import swsscommon
from test_route import TestRouteBase


class TestZmqRouteBase(TestRouteBase):

    def toggle_zmq(self, dvs, enable=True):
        cdb_value = "true" if enable else "false"
        self.cdb.set_field("DEVICE_METADATA", "localhost",
                           "orch_northbond_route_zmq_enabled", cdb_value)
        if enable:
            dvs.runcmd("cp /usr/bin/orchagent.sh /usr/bin/orchagent.sh_vrf_ut_backup")
            dvs.runcmd(
                "sed -i.bak 's/\\/usr\\/bin\\/orchagent /\\/usr\\/bin\\/orchagent -q tcp:\\/\\/127.0.0.1 -v mgmt /g' /usr/bin/orchagent.sh")
        else:
            dvs.runcmd("cp /usr/bin/orchagent.sh_vrf_ut_backup /usr/bin/orchagent.sh")
        dvs.stop_swss()
        dvs.start_swss()
        dvs.stop_fpmsyncd()
        dvs.start_fpmsyncd()

    def add_mgmt_vrf(self, dvs):
        appl_db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        asic_db = dvs.get_asic_db()
        cfg_db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        self.cdb.set_field("DEVICE_METADATA", "localhost",
                           "orch_northbond_route_zmq_enabled", "false")
        initial_entries = set(asic_db.get_keys(
            "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))
        dvs.runcmd("ip link add mgmt type vrf table 5000")
        dvs.runcmd("ifconfig mgmt up")
        time.sleep(2)

        # check application database
        tbl = swsscommon.Table(appl_db, 'VRF_TABLE')
        vrf_keys = tbl.getKeys()
        assert len(vrf_keys) == 0

        tbl = swsscommon.Table(cfg_db, 'MGMT_VRF_CONFIG')
        fvs = swsscommon.FieldValuePairs([('mgmtVrfEnabled', 'true'),
                                          ('in_band_mgmt_enabled', 'true')])
        tbl.set('vrf_global', fvs)
        time.sleep(1)

        # check application database
        tbl = swsscommon.Table(appl_db, 'VRF_TABLE')
        vrf_keys = tbl.getKeys()
        assert len(vrf_keys) == 1
        assert vrf_keys[0] == 'mgmt'

        # check SAI database info present in ASIC_DB
        asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER",
                                len(initial_entries) + 1)
        current_entries = set(asic_db.get_keys(
            "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))
        assert len(current_entries - initial_entries) == 1
        return list(current_entries - initial_entries)[0]


class TestRoute(TestZmqRouteBase):

    """ Functionality tests for route with mgmt VRF and ZMQ enabled

    This test validates that fpmsyncd is able to connecto the orchagent, while mgmt
    VRF is configured along with ZMQ. This excercises the code used to fix the 
    following issue:
    https://github.com/sonic-net/sonic-swss/issues/3985
    """

    def test_RouteAddRemoveIpv4Route(self, dvs, testlog):
        self.setup_db(dvs)
        self.add_mgmt_vrf(dvs)
        # Set localhost ip address so that zmq bind is able succeed in the mgmt
        # vrf
        dvs.runcmd(['sh', '-c', 'ip address add 127.0.0.1 dev mgmt'])
        self.toggle_zmq(dvs, enable=True)
        self.clear_srv_config(dvs)

        # create l3 interface
        self.create_l3_intf("Ethernet0", "")
        self.create_l3_intf("Ethernet4", "")

        # set ip address
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        self.add_ip_address("Ethernet4", "10.0.0.2/31")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        dvs.servers[1].runcmd("ip address add 10.0.0.3/31 dev eth0")
        dvs.servers[1].runcmd("ip route add default via 10.0.0.2")

        # get neighbor and arp entry
        dvs.servers[0].runcmd("ping -c 1 10.0.0.3")

        # add route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 2.2.2.0/24 10.0.0.1\"")

        # add default route entry
        fieldValues = {"nexthop": "10.0.0.1", "ifname": "Ethernet0"}
        self.create_route_entry("0.0.0.0/0", fieldValues)

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "2.2.2.0/24")

        # remove route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 2.2.2.0/24 10.0.0.1\"")

        # remove default route entry
        self.remove_route_entry("0.0.0.0/0")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "2.2.2.0/24")

        # remove ip address
        self.remove_ip_address("Ethernet0", "10.0.0.0/31")
        self.remove_ip_address("Ethernet4", "10.0.0.2/31")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")
        self.remove_l3_intf("Ethernet4")

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")

        # check STATE route database, state set to "na" after deleting
        # the default route
        self.check_route_state("0.0.0.0/0", "na")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip route del default dev eth0")
        dvs.servers[0].runcmd("ip address del 10.0.0.1/31 dev eth0")

        dvs.servers[1].runcmd("ip route del default dev eth0")
        dvs.servers[1].runcmd("ip address del 10.0.0.3/31 dev eth0")
        self.toggle_zmq(dvs, enable=False)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before
# retrying
def test_nonflaky_dummy():
    pass
