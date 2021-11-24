import time
import json
import pytest

from swsscommon import swsscommon

class TestIPv6LinkLocal(object):
    def setup_db(self, dvs):
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()
        self.cdb = dvs.get_config_db()

    def set_admin_status(self, interface, status):
        self.cdb.update_entry("PORT", interface, {"admin_status": status})

    def add_ip_address(self, interface, ip):
        self.cdb.create_entry("INTERFACE", interface + "|" + ip, {"NULL": "NULL"})
        time.sleep(2)

    def remove_ip_address(self, interface, ip):
        self.cdb.delete_entry("INTERFACE", interface + "|" + ip)
        time.sleep(2)

    def create_ipv6_link_local_intf(self, interface):
        self.cdb.create_entry("INTERFACE", interface, {"ipv6_use_link_local_only": "enable"})
        time.sleep(2)

    def remove_ipv6_link_local_intf(self, interface):
        self.cdb.delete_entry("INTERFACE", interface)
        time.sleep(2)

    def test_NeighborAddRemoveIpv6LinkLocal(self, dvs, testlog):
        self.setup_db(dvs)

        # create ipv6 link local intf
        self.create_ipv6_link_local_intf("Ethernet0")
        self.create_ipv6_link_local_intf("Ethernet4")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")

        # set ip address
        self.add_ip_address("Ethernet0", "2000::1/64")
        self.add_ip_address("Ethernet4", "2001::1/64")
        dvs.runcmd("sysctl -w net.ipv6.conf.all.forwarding=1")
        dvs.runcmd("sysctl -w net.ipv6.conf.default.forwarding=1")

        # set ip address and default route
        dvs.servers[0].runcmd("ip -6 address add 2000::2/64 dev eth0")
        dvs.servers[0].runcmd("ip -6 route add default via 2000::1")

        dvs.servers[1].runcmd("ip -6 address add 2001::2/64 dev eth0")
        dvs.servers[1].runcmd("ip -6 route add default via 2001::1")
        time.sleep(2)

        # get neighbor entry
        dvs.servers[0].runcmd("ping -6 -c 1 2001::2")
        time.sleep(2)

        # Neigh entries should contain Ipv6-link-local neighbors, should be 4
        neigh_entries = self.pdb.get_keys("NEIGH_TABLE")
        assert (len(neigh_entries) == 4)

        found_entry = False
        for key in neigh_entries:
            if (key.find("Ethernet4:2001::2") or key.find("Ethernet0:2000::2")):
                found_entry = True

        assert found_entry

        # remove ip address
        self.remove_ip_address("Ethernet0", "2000::1/64")
        self.remove_ip_address("Ethernet4", "2001::1/64")

        # remove ipv6 link local intf
        self.remove_ipv6_link_local_intf("Ethernet0")
        self.remove_ipv6_link_local_intf("Ethernet4")

        # Neigh entries should not contain Ipv6-link-local neighbors, should be 2
        neigh_after_entries = self.pdb.get_keys("NEIGH_TABLE")
        print(neigh_after_entries)
        assert (len(neigh_after_entries) == 2)

        found_existing_entry = False
        for key in neigh_entries:
            if (key.find("Ethernet4:2001::2") or key.find("Ethernet0:2000::2")):
                found_existing_entry = True

        assert found_existing_entry

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip -6 route del default dev eth0")
        dvs.servers[0].runcmd("ip -6 address del 2000::2/64 dev eth0")

        dvs.servers[1].runcmd("ip -6 route del default dev eth0")
        dvs.servers[1].runcmd("ip -6 address del 2001::2/64 dev eth0")

        dvs.runcmd("sysctl -w net.ipv6.conf.all.forwarding=0")
        dvs.runcmd("sysctl -w net.ipv6.conf.default.forwarding=0")

        neigh_entries = self.pdb.get_keys("NEIGH_TABLE")
        assert (len(neigh_entries) == 0)

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass

