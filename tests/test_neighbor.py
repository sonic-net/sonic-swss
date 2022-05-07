import time
import json
import pytest

from swsscommon import swsscommon


class TestNeighbor(object):
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


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
