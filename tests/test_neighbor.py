import time
import json
import pytest

from swsscommon import swsscommon
from dvslib.dvs_common import wait_for_result


class TestNeighbor(object):
    CONFIG_PEER_SWITCH          = "PEER_SWITCH"
    PEER_SWITCH_HOST            = "peer_switch_hostname"
    PEER_IPV4                   = "10.1.0.33"

    DEFAULT_PEER_SWITCH_PARAMS = {
        "address_ipv4": PEER_IPV4
    }

    DUALTOR_VLAN                = "Vlan1000"
    DUALTOR_VLAN_MEMBER         = "Ethernet0"
    DUALTOR_VLAN_IPV6           = "fc02:1000::1/64"
    APP_NEIGH_FAILED_TABLE      = "NEIGH_FAILED_TABLE"

    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

    def set_admin_status(self, interface, status):
        tbl = swsscommon.Table(self.cdb, "PORT")
        fvs = swsscommon.FieldValuePairs([("admin_status", status)])
        tbl.set(interface, fvs)
        time.sleep(1)

    def create_vrf(self, vrf_name):
        tbl = swsscommon.Table(self.cdb, "VRF")
        fvs = swsscommon.FieldValuePairs([('empty', 'empty')])
        tbl.set(vrf_name, fvs)
        time.sleep(1)

    def remove_vrf(self, vrf_name):
        tbl = swsscommon.Table(self.cdb, "VRF")
        tbl._del(vrf_name)
        time.sleep(1)

    def create_l3_intf(self, interface, vrf_name):
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        initial_entries = set(tbl.getKeys())

        tbl = swsscommon.Table(self.cdb, "INTERFACE")
        if len(vrf_name) == 0:
            fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        else:
            fvs = swsscommon.FieldValuePairs([("vrf_name", vrf_name)])
        tbl.set(interface, fvs)
        time.sleep(1)

        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
        current_entries = set(tbl.getKeys())
        assert len(current_entries - initial_entries) == 1
        return list(current_entries - initial_entries)[0]

    def remove_l3_intf(self, interface):
        tbl = swsscommon.Table(self.cdb, "INTERFACE")
        tbl._del(interface)
        time.sleep(1)

    def add_ip_address(self, interface, ip):
        tbl = swsscommon.Table(self.cdb, "INTERFACE")
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        tbl.set(interface + "|" + ip, fvs)
        time.sleep(2) # IPv6 netlink message needs longer time

    def remove_ip_address(self, interface, ip):
        tbl = swsscommon.Table(self.cdb, "INTERFACE")
        tbl._del(interface + "|" + ip)
        time.sleep(1)

    def add_neighbor(self, interface, ip, mac):
        tbl = swsscommon.Table(self.cdb, "NEIGH")
        fvs = swsscommon.FieldValuePairs([("neigh", mac)])
        tbl.set(interface + "|" + ip, fvs)
        time.sleep(1)

    def remove_neighbor(self, interface, ip):
        tbl = swsscommon.Table(self.cdb, "NEIGH")
        tbl._del(interface + "|" + ip)
        time.sleep(1)

    def test_NeighborAddRemoveIpv6(self, dvs, testlog):
        self.setup_db(dvs)

        # bring up interface
        # NOTE: For IPv6, only when the interface is up will the netlink message
        # get generated.
        self.set_admin_status("Ethernet8", "up")

        # create interface and get rif_oid
        rif_oid = self.create_l3_intf("Ethernet8", "")

        # assign IP to interface
        self.add_ip_address("Ethernet8", "2000::1/64")

        # add neighbor
        self.add_neighbor("Ethernet8", "2000::2", "00:01:02:03:04:05")

        # check application database
        tbl = swsscommon.Table(self.pdb, "NEIGH_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        assert intf_entries[0] == "2000::2"
        (status, fvs) = tbl.get(intf_entries[0])
        assert status == True
        assert len(fvs) == 2
        for fv in fvs:
            if fv[0] == "neigh":
                assert fv[1] == "00:01:02:03:04:05"
            elif fv[0] == "family":
                assert fv[1] == "IPv6"
            else:
                assert False

        # check ASIC neighbor database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        route = json.loads(intf_entries[0])
        assert route["ip"] == "2000::2"
        assert route["rif"] == rif_oid

        (status, fvs) = tbl.get(intf_entries[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS":
                assert fv[1] == "00:01:02:03:04:05"

        # remove neighbor
        self.remove_neighbor("Ethernet8", "2000::2")

        # remove IP from interface
        self.remove_ip_address("Ethernet8", "2000::1/64")

        # remove interface
        self.remove_l3_intf("Ethernet8")

        # bring down interface
        self.set_admin_status("Ethernet8", "down")

        # check application database
        tbl = swsscommon.Table(self.pdb, "NEIGH_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # check ASIC neighbor database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0


    def test_NeighborAddRemoveIpv4(self, dvs, testlog):
        self.setup_db(dvs)

        # bring up interface
        self.set_admin_status("Ethernet8", "up")

        # create interface and get rif_oid
        rif_oid = self.create_l3_intf("Ethernet8", "")

        # assign IP to interface
        self.add_ip_address("Ethernet8", "10.0.0.1/24")

        # add neighbor
        self.add_neighbor("Ethernet8", "10.0.0.2", "00:01:02:03:04:05")

        # check application database
        tbl = swsscommon.Table(self.pdb, "NEIGH_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        assert intf_entries[0] == "10.0.0.2"
        (status, fvs) = tbl.get(intf_entries[0])
        assert status == True
        assert len(fvs) == 2
        for fv in fvs:
            if fv[0] == "neigh":
                assert fv[1] == "00:01:02:03:04:05"
            elif fv[0] == "family":
                assert fv[1] == "IPv4"
            else:
                assert False

        # check ASIC neighbor database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1
        route = json.loads(intf_entries[0])
        assert route["ip"] == "10.0.0.2"
        assert route["rif"] == rif_oid

        (status, fvs) = tbl.get(intf_entries[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS":
                assert fv[1] == "00:01:02:03:04:05"

        # remove neighbor
        self.remove_neighbor("Ethernet8", "10.0.0.2")

        # remove IP from interface
        self.remove_ip_address("Ethernet8", "10.0.0.1/24")

        # remove interface
        self.remove_l3_intf("Ethernet8")

        # bring down interface
        self.set_admin_status("Ethernet8", "down")

        # check application database
        tbl = swsscommon.Table(self.pdb, "NEIGH_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # check ASIC neighbor database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

    def test_NeighborAddRemoveIpv6WithVrf(self, dvs, testlog):
        self.setup_db(dvs)

        for i in [0, 4]:
            # record ASIC neighbor database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
            old_neigh_entries = set(tbl.getKeys())

            intf_name = "Ethernet" + str(i)
            vrf_name = "Vrf_" + str(i)

            # bring up interface
            self.set_admin_status(intf_name, "up")

            # create vrf
            self.create_vrf(vrf_name)

            # create interface and get rif_oid
            rif_oid = self.create_l3_intf(intf_name, vrf_name)

            # assign IP to interface
            self.add_ip_address(intf_name, "2000::1/64")

            # add neighbor
            self.add_neighbor(intf_name, "2000::2", "00:01:02:03:04:05")

            # check application database
            tbl = swsscommon.Table(self.pdb, "NEIGH_TABLE:" + intf_name)
            neigh_entries = tbl.getKeys()
            assert len(neigh_entries) == 1
            assert neigh_entries[0] == "2000::2"
            (status, fvs) = tbl.get(neigh_entries[0])
            assert status == True
            assert len(fvs) == 2
            for fv in fvs:
                if fv[0] == "neigh":
                    assert fv[1] == "00:01:02:03:04:05"
                elif fv[0] == "family":
                    assert fv[1] == "IPv6"
                else:
                    assert False

            # check ASIC neighbor interface database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
            current_neigh_entries = set(tbl.getKeys())
            neigh_entries = list(current_neigh_entries - old_neigh_entries)
            assert len(neigh_entries) == 1
            route = json.loads(neigh_entries[0])
            assert route["ip"] == "2000::2"
            assert route["rif"] == rif_oid

            (status, fvs) = tbl.get(neigh_entries[0])
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS":
                    assert fv[1] == "00:01:02:03:04:05"

        for i in [0, 4]:
            # record ASIC neighbor database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
            old_neigh_entries_cnt = len(tbl.getKeys())

            intf_name = "Ethernet" + str(i)
            vrf_name = "Vrf_" + str(i)

            # remove neighbor
            self.remove_neighbor(intf_name, "2000::2")

            # remove IP from interface
            self.remove_ip_address(intf_name, "2000::1/64")

            # remove interface
            self.remove_l3_intf(intf_name)

            # remove vrf
            self.remove_vrf(vrf_name)

            # bring down interface
            self.set_admin_status(intf_name, "down")

            # check application database
            tbl = swsscommon.Table(self.pdb, "NEIGH_TABLE:" + intf_name)
            intf_entries = tbl.getKeys()
            assert len(intf_entries) == 0

            # check ASIC neighbor interface database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
            current_neigh_entries_cnt = len(tbl.getKeys())
            dec_neigh_entries_cnt = (old_neigh_entries_cnt - current_neigh_entries_cnt)
            assert dec_neigh_entries_cnt == 1

    def test_NeighborAddRemoveIpv4WithVrf(self, dvs, testlog):
        self.setup_db(dvs)

        for i in [0, 4]:
            # record ASIC neighbor database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
            old_neigh_entries = set(tbl.getKeys())

            intf_name = "Ethernet" + str(i)
            vrf_name = "Vrf_" + str(i)

            # bring up interface
            self.set_admin_status(intf_name, "up")

            # create vrf
            self.create_vrf(vrf_name)

            # create interface and get rif_oid
            rif_oid = self.create_l3_intf(intf_name, vrf_name)

            # assign IP to interface
            self.add_ip_address(intf_name, "10.0.0.1/24")

            # add neighbor
            self.add_neighbor(intf_name, "10.0.0.2", "00:01:02:03:04:05")

            # check application database
            tbl = swsscommon.Table(self.pdb, "NEIGH_TABLE:" + intf_name)
            neigh_entries = tbl.getKeys()
            assert len(neigh_entries) == 1
            assert neigh_entries[0] == "10.0.0.2"
            (status, fvs) = tbl.get(neigh_entries[0])
            assert status == True
            assert len(fvs) == 2
            for fv in fvs:
                if fv[0] == "neigh":
                    assert fv[1] == "00:01:02:03:04:05"
                elif fv[0] == "family":
                    assert fv[1] == "IPv4"
                else:
                    assert False

            # check ASIC neighbor interface database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
            current_neigh_entries = set(tbl.getKeys())
            neigh_entries = list(current_neigh_entries - old_neigh_entries)
            assert len(neigh_entries) == 1
            route = json.loads(neigh_entries[0])
            assert route["ip"] == "10.0.0.2"
            assert route["rif"] == rif_oid

            (status, fvs) = tbl.get(neigh_entries[0])
            assert status == True
            for fv in fvs:
                if fv[0] == "SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS":
                    assert fv[1] == "00:01:02:03:04:05"

        for i in [0, 4]:
            # record ASIC neighbor database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
            old_neigh_entries_cnt = len(tbl.getKeys())

            intf_name = "Ethernet" + str(i)
            vrf_name = "Vrf_" + str(i)

            # remove neighbor
            self.remove_neighbor(intf_name, "10.0.0.2")

            # remove IP from interface
            self.remove_ip_address(intf_name, "10.0.0.1/24")

            # remove interface
            self.remove_l3_intf(intf_name)

            # remove vrf
            self.remove_vrf(vrf_name)

            # bring down interface
            self.set_admin_status(intf_name, "down")

            # check application database
            tbl = swsscommon.Table(self.pdb, "NEIGH_TABLE:" + intf_name)
            intf_entries = tbl.getKeys()
            assert len(intf_entries) == 0

            # check ASIC neighbor interface database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
            current_neigh_entries_cnt = len(tbl.getKeys())
            dec_neigh_entries_cnt = (old_neigh_entries_cnt - current_neigh_entries_cnt)
            assert dec_neigh_entries_cnt == 1

    def test_FlushResolveNeighborIpv6(self, dvs, testlog):
        appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        prod_state_tbl = swsscommon.ProducerStateTable(appl_db, swsscommon.APP_NEIGH_RESOLVE_TABLE_NAME)
        fvs = swsscommon.FieldValuePairs([("mac", "52:54:00:25:06:E9")])

        prod_state_tbl.set("Vlan2:2000:1::1", fvs)
        time.sleep(2)

        (exitcode, output) = dvs.runcmd(['sh', '-c', "supervisorctl status nbrmgrd | awk '{print $2}'"])
        assert output == "RUNNING\n"

    def test_FlushResolveNeighborIpv4(self, dvs, testlog):
        appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        prod_state_tbl = swsscommon.ProducerStateTable(appl_db, swsscommon.APP_NEIGH_RESOLVE_TABLE_NAME)
        fvs = swsscommon.FieldValuePairs([("mac", "52:54:00:25:06:E9")])

        prod_state_tbl.set("Vlan2:192.168.10.1", fvs)
        time.sleep(2)

        (exitcode, output) = dvs.runcmd(['sh', '-c', "supervisorctl status nbrmgrd | awk '{print $2}'"])
        assert output == "RUNNING\n"

    def test_Ipv4LinkLocalNeighbor(self, dvs, testlog):
        self.setup_db(dvs)

        # bring up interface
        self.set_admin_status("Ethernet8", "up")

        # create interface
        self.create_l3_intf("Ethernet8", "")

        # assign IP to interface
        self.add_ip_address("Ethernet8", "10.0.0.1/24")

        # add neighbor
        self.add_neighbor("Ethernet8", "169.254.0.0", "00:01:02:03:04:05")

        # check application database
        tbl = swsscommon.Table(self.pdb, "NEIGH_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1

        # check ASIC neighbor database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 1

        # remove neighbor
        self.remove_neighbor("Ethernet8", "169.254.0.0")

        # remove IP from interface
        self.remove_ip_address("Ethernet8", "10.0.0.1/24")

        # remove interface
        self.remove_l3_intf("Ethernet8")

        # bring down interface
        self.set_admin_status("Ethernet8", "down")

        # check application database
        tbl = swsscommon.Table(self.pdb, "NEIGH_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # check ASIC neighbor database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

    def test_Ipv4LinkLocalNeighborWithDualToR(self, dvs, testlog, setup_peer_switch):
        self.setup_db(dvs)

        # bring up interface
        self.set_admin_status("Ethernet8", "up")

        # create interface
        self.create_l3_intf("Ethernet8", "")

        # assign IP to interface
        self.add_ip_address("Ethernet8", "10.0.0.1/24")

        # add neighbor
        self.add_neighbor("Ethernet8", "169.254.0.0", "00:01:02:03:04:05")

        # check application database
        tbl = swsscommon.Table(self.pdb, "NEIGH_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # check ASIC neighbor database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # remove neighbor
        self.remove_neighbor("Ethernet8", "169.254.0.0")

        # remove IP from interface
        self.remove_ip_address("Ethernet8", "10.0.0.1/24")

        # remove interface
        self.remove_l3_intf("Ethernet8")

        # bring down interface
        self.set_admin_status("Ethernet8", "down")

        # check application database
        tbl = swsscommon.Table(self.pdb, "NEIGH_TABLE:Ethernet8")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

        # check ASIC neighbor database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
        intf_entries = tbl.getKeys()
        assert len(intf_entries) == 0

    def restart_nbrmgrd(self, dvs):
        dvs.runcmd("supervisorctl restart nbrmgrd")
        (exitcode, output) = dvs.runcmd(['sh', '-c', "supervisorctl status nbrmgrd | awk '{print $2}'"])
        assert output == "RUNNING\n"

    def set_nbrmgrd_paused(self, dvs, paused):
        signal = "SIGSTOP" if paused else "SIGCONT"
        dvs.runcmd("bash -c 'kill -{} $(pidof nbrmgrd)'".format(signal))

    def failed_neighbor_updates_consumed(self, dvs):
        _, output = dvs.runcmd("redis-cli -n 0 SCARD {}_KEY_SET".format(self.APP_NEIGH_FAILED_TABLE))
        return (output.strip() == "0", output)

    @pytest.fixture
    def setup_dualtor_vlan(self, dvs):
        config_db = dvs.get_config_db()
        config_db.create_entry(
            self.CONFIG_PEER_SWITCH,
            self.PEER_SWITCH_HOST,
            self.DEFAULT_PEER_SWITCH_PARAMS
        )
        # nbrmgrd only consumes NEIGH_FAILED_TABLE when PEER_SWITCH exists at startup
        self.restart_nbrmgrd(dvs)

        vlan_member_key = self.DUALTOR_VLAN + "|" + self.DUALTOR_VLAN_MEMBER
        vlan_ip_key = self.DUALTOR_VLAN + "|" + self.DUALTOR_VLAN_IPV6
        config_db.create_entry("VLAN", self.DUALTOR_VLAN, {"vlanid": "1000"})
        config_db.create_entry("VLAN_MEMBER", vlan_member_key, {"tagging_mode": "untagged"})
        config_db.create_entry("VLAN_INTERFACE", self.DUALTOR_VLAN, {"NULL": "NULL"})
        config_db.create_entry("VLAN_INTERFACE", vlan_ip_key, {"NULL": "NULL"})
        dvs.port_admin_set(self.DUALTOR_VLAN_MEMBER, "up")

        vlan_ip = self.DUALTOR_VLAN_IPV6.split("/")[0]

        def vlan_ipv6_configured():
            _, output = dvs.runcmd("ip -6 addr show dev {}".format(self.DUALTOR_VLAN))
            return (vlan_ip in output, output)

        wait_for_result(vlan_ipv6_configured,
                        failure_message="{} is not configured on {}".format(vlan_ip, self.DUALTOR_VLAN))

        yield

        dvs.runcmd("ip -6 neigh flush dev {}".format(self.DUALTOR_VLAN))
        config_db.delete_entry("VLAN_INTERFACE", vlan_ip_key)
        config_db.delete_entry("VLAN_INTERFACE", self.DUALTOR_VLAN)
        config_db.delete_entry("VLAN_MEMBER", vlan_member_key)
        config_db.delete_entry("VLAN", self.DUALTOR_VLAN)
        dvs.port_admin_set(self.DUALTOR_VLAN_MEMBER, "down")
        config_db.delete_entry(self.CONFIG_PEER_SWITCH, self.PEER_SWITCH_HOST)
        self.restart_nbrmgrd(dvs)

    def test_DualToRFailedIpv6NeighborEntryRemovedOnDelete(self, dvs, testlog, setup_dualtor_vlan):
        app_db = dvs.get_app_db()
        neigh_ip = "fc02:1000::20"
        failed_key = self.DUALTOR_VLAN + ":" + neigh_ip

        # No host owns the address, so the kernel marks the neighbor FAILED once
        # its solicitations go unanswered, and neighsyncd publishes it.
        dvs.runcmd("ping -6 -c 1 -W 1 {}".format(neigh_ip))
        app_db.wait_for_entry(self.APP_NEIGH_FAILED_TABLE, failed_key)

        dvs.runcmd("ip -6 neigh del {} dev {}".format(neigh_ip, self.DUALTOR_VLAN))
        app_db.wait_for_deleted_entry(self.APP_NEIGH_FAILED_TABLE, failed_key)

    def test_DualToRResolvedIpv6NeighborDeleteLeavesNoFailedEntry(self, dvs, testlog, setup_dualtor_vlan):
        app_db = dvs.get_app_db()
        neigh_ip = "fc02:1000::10"
        neigh_key = self.DUALTOR_VLAN + ":" + neigh_ip

        dvs.runcmd("ip -6 neigh replace {} lladdr 00:aa:bb:cc:dd:01 dev {} nud stale".format(
            neigh_ip, self.DUALTOR_VLAN))
        app_db.wait_for_entry("NEIGH_TABLE", neigh_key)

        # Deleting a resolved neighbor makes the kernel report NUD_FAILED immediately
        # followed by RTM_DELNEIGH. Pause nbrmgrd so neighsyncd handles both reports
        # before the failed neighbor entry can be consumed.
        self.set_nbrmgrd_paused(dvs, True)
        try:
            dvs.runcmd("ip -6 neigh del {} dev {}".format(neigh_ip, self.DUALTOR_VLAN))
            app_db.wait_for_deleted_entry("NEIGH_TABLE", neigh_key)
        finally:
            self.set_nbrmgrd_paused(dvs, False)

        wait_for_result(lambda: self.failed_neighbor_updates_consumed(dvs),
                        failure_message="nbrmgrd did not consume pending failed neighbor updates")
        assert neigh_key not in app_db.get_keys(self.APP_NEIGH_FAILED_TABLE)

    @pytest.fixture
    def setup_peer_switch(self, dvs):
        config_db = dvs.get_config_db()
        config_db.create_entry(
            self.CONFIG_PEER_SWITCH,
            self.PEER_SWITCH_HOST,
            self.DEFAULT_PEER_SWITCH_PARAMS
        )

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
