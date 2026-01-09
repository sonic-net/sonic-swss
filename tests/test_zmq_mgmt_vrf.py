import time
import json

from swsscommon import swsscommon
from dvslib.dvs_common import wait_for_result


class TestRouteBase(object):
    def setup_db(self, dvs):
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()
        self.cdb = dvs.get_config_db()
        self.sdb = dvs.get_state_db()

    def set_admin_status(self, interface, status):
        self.cdb.update_entry("PORT", interface, {"admin_status": status})

    def create_vrf(self, vrf_name):
        initial_entries = set(self.adb.get_keys(
            "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))

        self.cdb.create_entry("VRF", vrf_name, {"empty": "empty"})
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER",
                                 len(initial_entries) + 1)

        current_entries = set(self.adb.get_keys(
            "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))
        assert len(current_entries - initial_entries) == 1
        return list(current_entries - initial_entries)[0]

    def remove_vrf(self, vrf_name):
        self.cdb.delete_entry("VRF", vrf_name)

    def create_l3_intf(self, interface, vrf_name):
        if len(vrf_name) == 0:
            self.cdb.create_entry("INTERFACE", interface, {"NULL": "NULL"})
        else:
            self.cdb.create_entry("INTERFACE", interface,
                                  {"vrf_name": vrf_name})

    def remove_l3_intf(self, interface):
        self.cdb.delete_entry("INTERFACE", interface)

    def add_ip_address(self, interface, ip):
        self.cdb.create_entry("INTERFACE", interface + "|" + ip,
                              {"NULL": "NULL"})

    def remove_ip_address(self, interface, ip):
        self.cdb.delete_entry("INTERFACE", interface + "|" + ip)

    def create_route_entry(self, key, pairs):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection,
                                            "ROUTE_TABLE")
        fvs = swsscommon.FieldValuePairs(list(pairs.items()))
        tbl.set(key, fvs)

    def remove_route_entry(self, key):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection,
                                            "ROUTE_TABLE")
        tbl._del(key)

    def check_route_entries(self, destinations):
        def _access_function():
            route_entries = self.adb.get_keys(
                "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            route_destinations = [json.loads(route_entry)["dest"]
                                  for route_entry in route_entries]
            return (all(destination in route_destinations for destination in destinations), None)

        wait_for_result(_access_function)

    def check_route_state(self, prefix, value):
        found = False
        fvs = {}

        for _ in range(5):  # Try for up to ~5 seconds
            route_entries = self.sdb.get_keys("ROUTE_TABLE")
            for key in route_entries:
                if key != prefix:
                    continue
                found = True
                fvs = self.sdb.get_entry("ROUTE_TABLE", key)
                if fvs.get("state") == value:
                    return
            time.sleep(1)

        assert found
        assert fvs.get("state") == \
            value, f"Expected state '{value}', but got '{fvs.get('state')}'"

    def get_asic_db_key(self, destination):
        route_entries = self.adb.get_keys(
            "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        for route_entry in route_entries:
            if json.loads(route_entry)["dest"] == destination:
                return route_entry
        return None

    def check_route_entries_with_vrf(self, destinations, vrf_oids):
        def _access_function():
            route_entries = self.adb.get_keys(
                "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            route_destination_vrf = [(json.loads(route_entry)["dest"],
                                      json.loads(route_entry)["vr"])
                                     for route_entry in route_entries]
            return (all((destination, vrf_oid) in route_destination_vrf
                        for destination, vrf_oid in zip(destinations, vrf_oids)), None)

        wait_for_result(_access_function)

    def check_route_entries_nexthop(self, destinations, vrf_oids, nexthops):
        def _access_function_nexthop():
            nexthop_entries = self.adb.get_keys(
                "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP")
            nexthop_oids = \
                dict([(self.adb.get_entry(
                    "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP",
                    key)["SAI_NEXT_HOP_ATTR_IP"], key)
                                 for key in nexthop_entries])
            return (all(nexthop in nexthop_oids for nexthop in nexthops),
                    nexthop_oids)

        status, nexthop_oids = wait_for_result(_access_function_nexthop)

        def _access_function_route_nexthop():
            route_entries = self.adb.get_keys(
                "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            route_destination_nexthop = \
                dict([((json.loads(route_entry)["dest"],
                        json.loads(route_entry)["vr"]),
                       self.adb.get_entry(
                        "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY",
                        route_entry).get("SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"))
                      for route_entry in route_entries])
            return (all(route_destination_nexthop.get(
                          (destination, vrf_oid)) == nexthop_oids.get(nexthop)
                        for destination, vrf_oid, nexthop in zip(destinations, vrf_oids, nexthops)), None)

        wait_for_result(_access_function_route_nexthop)

    def check_deleted_route_entries(self, destinations):
        def _access_function():
            route_entries = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            route_destinations = [json.loads(route_entry)["dest"] for route_entry in route_entries]
            return (all(destination not in route_destinations for destination in destinations), None)

        wait_for_result(_access_function)

    def clear_srv_config(self, dvs):
        dvs.servers[0].runcmd("ip address flush dev eth0")
        dvs.servers[1].runcmd("ip address flush dev eth0")
        dvs.servers[2].runcmd("ip address flush dev eth0")
        dvs.servers[3].runcmd("ip address flush dev eth0")

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


class TestRoute(TestRouteBase):

    """ Functionality tests for route """

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
