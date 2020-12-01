import json
import time

from dvslib.dvs_common import wait_for_result
from dvslib.dvs_database import DVSDatabase
from swsscommon import swsscommon

DEFAULT_MTU = "9100"

CFG_VLAN_SUB_INTF_TABLE_NAME = "VLAN_SUB_INTERFACE"
CFG_PORT_TABLE_NAME = "PORT"
CFG_LAG_TABLE_NAME = "PORTCHANNEL"

STATE_PORT_TABLE_NAME = "PORT_TABLE"
STATE_LAG_TABLE_NAME = "LAG_TABLE"
STATE_INTERFACE_TABLE_NAME = "INTERFACE_TABLE"

APP_INTF_TABLE_NAME = "INTF_TABLE"
APP_ROUTE_TABLE_NAME = "ROUTE_TABLE"
APP_PORT_TABLE_NAME = "PORT_TABLE"
APP_LAG_TABLE_NAME = "LAG_TABLE"

ASIC_RIF_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
ASIC_ROUTE_ENTRY_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"
ASIC_NEXT_HOP_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP"
ASIC_NEXT_HOP_GROUP_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP"
ASIC_NEXT_HOP_GROUP_MEMBER_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER"

ADMIN_STATUS = "admin_status"

ETHERNET_PREFIX = "Ethernet"
LAG_PREFIX = "PortChannel"

VLAN_SUB_INTERFACE_SEPARATOR = "."


class TestSubPortIntf(object):
    SUB_PORT_INTERFACE_UNDER_TEST = "Ethernet64.10"
    LAG_SUB_PORT_INTERFACE_UNDER_TEST = "PortChannel1.20"

    IPV4_ADDR_UNDER_TEST = "10.0.0.33/31"
    IPV4_TOME_UNDER_TEST = "10.0.0.33/32"
    IPV4_SUBNET_UNDER_TEST = "10.0.0.32/31"

    IPV6_ADDR_UNDER_TEST = "fc00::41/126"
    IPV6_TOME_UNDER_TEST = "fc00::41/128"
    IPV6_SUBNET_UNDER_TEST = "fc00::40/126"

    def connect_dbs(self, dvs):
        self.app_db = dvs.get_app_db()
        self.asic_db = dvs.get_asic_db()
        self.config_db = dvs.get_config_db()
        self.state_db = dvs.get_state_db()
        dvs.setup_db()

    def get_parent_port_index(self, port_name):
        if port_name.startswith(ETHERNET_PREFIX):
            idx = int(port_name[len(ETHERNET_PREFIX):])
        else:
            assert port_name.startswith(LAG_PREFIX)
            idx = int(port_name[len(LAG_PREFIX):])
        return idx

    def set_parent_port_oper_status(self, dvs, port_name, status):
        if port_name.startswith(ETHERNET_PREFIX):
            srv_idx = self.get_parent_port_index(port_name) // 4
            dvs.servers[srv_idx].runcmd("ip link set dev eth0 " + status)
        else:
            assert port_name.startswith(LAG_PREFIX)
            dvs.runcmd("bash -c 'echo " + ("1" if status == "up" else "0") + \
                    " > /sys/class/net/" + port_name + "/carrier'")
        time.sleep(1)

    def set_parent_port_admin_status(self, dvs, port_name, status):
        fvs = {ADMIN_STATUS: status}

        if port_name.startswith(ETHERNET_PREFIX):
            tbl_name = CFG_PORT_TABLE_NAME
        else:
            assert port_name.startswith(LAG_PREFIX)
            tbl_name = CFG_LAG_TABLE_NAME
        self.config_db.create_entry(tbl_name, port_name, fvs)

        if port_name.startswith(ETHERNET_PREFIX):
            self.set_parent_port_oper_status(dvs, port_name, "down")
            self.set_parent_port_oper_status(dvs, port_name, "up")
        else:
            self.set_parent_port_oper_status(dvs, port_name, "up")

    def create_sub_port_intf_profile(self, sub_port_intf_name):
        fvs = {ADMIN_STATUS: "up"}

        self.config_db.create_entry(CFG_VLAN_SUB_INTF_TABLE_NAME, sub_port_intf_name, fvs)

    def add_sub_port_intf_ip_addr(self, sub_port_intf_name, ip_addr):
        fvs = {"NULL": "NULL"}

        key = "{}|{}".format(sub_port_intf_name, ip_addr)
        self.config_db.create_entry(CFG_VLAN_SUB_INTF_TABLE_NAME, key, fvs)

    def set_sub_port_intf_admin_status(self, sub_port_intf_name, status):
        fvs = {ADMIN_STATUS: status}

        self.config_db.create_entry(CFG_VLAN_SUB_INTF_TABLE_NAME, sub_port_intf_name, fvs)

    def remove_sub_port_intf_profile(self, sub_port_intf_name):
        self.config_db.delete_entry(CFG_VLAN_SUB_INTF_TABLE_NAME, sub_port_intf_name)

    def check_sub_port_intf_profile_removal(self, rif_oid):
        self.asic_db.wait_for_deleted_keys(ASIC_RIF_TABLE, [rif_oid])

    def remove_sub_port_intf_ip_addr(self, sub_port_intf_name, ip_addr):
        key = "{}|{}".format(sub_port_intf_name, ip_addr)
        self.config_db.delete_entry(CFG_VLAN_SUB_INTF_TABLE_NAME, key)

    def check_sub_port_intf_ip_addr_removal(self, sub_port_intf_name, ip_addrs):
        interfaces = ["{}:{}".format(sub_port_intf_name, addr) for addr in ip_addrs]
        self.app_db.wait_for_deleted_keys(APP_INTF_TABLE_NAME, interfaces)

    def get_oids(self, table):
        return self.asic_db.get_keys(table)

    def get_newly_created_oid(self, table, old_oids):
        new_oids = self.asic_db.wait_for_n_keys(table, len(old_oids) + 1)
        oid = [ids for ids in new_oids if ids not in old_oids]
        return oid[0]

    def get_ip_prefix_nhg_oid(self, ip_prefix):
        def _access_function():
            route_entry_found = False

            raw_route_entry_keys = self.asic_db.get_keys(ASIC_ROUTE_ENTRY_TABLE)
            for raw_route_entry_key in raw_route_entry_keys:
                route_entry_key = json.loads(raw_route_entry_key)
                if route_entry_key["dest"] == ip_prefix:
                    route_entry_found = True
                    break

            return (route_entry_found, raw_route_entry_key)

        (route_entry_found, raw_route_entry_key) = wait_for_result(_access_function, DVSDatabase.DEFAULT_POLLING_CONFIG)

        fvs = self.asic_db.get_entry(ASIC_ROUTE_ENTRY_TABLE, raw_route_entry_key)

        nhg_oid = fvs.get( "SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID", "")
        assert nhg_oid != ""
        assert nhg_oid != "oid:0x0"

        return nhg_oid

    def check_sub_port_intf_key_existence(self, db, table_name, key):
        db.wait_for_matching_keys(table_name, [key])

    def check_sub_port_intf_fvs(self, db, table_name, key, fv_dict):
        db.wait_for_field_match(table_name, key, fv_dict)

    def check_sub_port_intf_route_entries(self):
        expected_destinations = [self.IPV4_TOME_UNDER_TEST,
                                 self.IPV4_SUBNET_UNDER_TEST,
                                 self.IPV6_TOME_UNDER_TEST,
                                 self.IPV6_SUBNET_UNDER_TEST]

        def _access_function():
            raw_route_entries = self.asic_db.get_keys(ASIC_ROUTE_ENTRY_TABLE)
            route_destinations = [str(json.loads(raw_route_entry)["dest"])
                                  for raw_route_entry in raw_route_entries]
            return (all(dest in route_destinations for dest in expected_destinations), None)

        wait_for_result(_access_function, DVSDatabase.DEFAULT_POLLING_CONFIG)

    def check_sub_port_intf_key_removal(self, db, table_name, key):
        db.wait_for_deleted_keys(table_name, [key])

    def check_sub_port_intf_route_entries_removal(self, removed_route_entries):
        def _access_function():
            raw_route_entries = self.asic_db.get_keys(ASIC_ROUTE_ENTRY_TABLE)
            status = all(str(json.loads(raw_route_entry)["dest"])
                         not in removed_route_entries
                         for raw_route_entry in raw_route_entries)
            return (status, None)

        wait_for_result(_access_function, DVSDatabase.DEFAULT_POLLING_CONFIG)

    def _test_sub_port_intf_creation(self, dvs, sub_port_intf_name):
        substrs = sub_port_intf_name.split(VLAN_SUB_INTERFACE_SEPARATOR)
        parent_port = substrs[0]
        vlan_id = substrs[1]
        if parent_port.startswith(ETHERNET_PREFIX):
            state_tbl_name = STATE_PORT_TABLE_NAME
        else:
            assert parent_port.startswith(LAG_PREFIX)
            state_tbl_name = STATE_LAG_TABLE_NAME

        old_rif_oids = self.get_oids(ASIC_RIF_TABLE)

        self.set_parent_port_admin_status(dvs, parent_port, "up")
        self.create_sub_port_intf_profile(sub_port_intf_name)

        # Verify that sub port interface state ok is pushed to STATE_DB by Intfmgrd
        fv_dict = {
            "state": "ok",
        }
        self.check_sub_port_intf_fvs(self.state_db, state_tbl_name, sub_port_intf_name, fv_dict)

        # Verify that sub port interface configuration is synced to APPL_DB INTF_TABLE by Intfmgrd
        fv_dict = {
            ADMIN_STATUS: "up",
        }
        self.check_sub_port_intf_fvs(self.app_db, APP_INTF_TABLE_NAME, sub_port_intf_name, fv_dict)

        # Verify that a sub port router interface entry is created in ASIC_DB
        fv_dict = {
            "SAI_ROUTER_INTERFACE_ATTR_TYPE": "SAI_ROUTER_INTERFACE_TYPE_SUB_PORT",
            "SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID": "{}".format(vlan_id),
            "SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE": "true",
            "SAI_ROUTER_INTERFACE_ATTR_ADMIN_V6_STATE": "true",
            "SAI_ROUTER_INTERFACE_ATTR_MTU": DEFAULT_MTU,
        }
        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)
        self.check_sub_port_intf_fvs(self.asic_db, ASIC_RIF_TABLE, rif_oid, fv_dict)

        # Remove a sub port interface
        self.remove_sub_port_intf_profile(sub_port_intf_name)
        self.check_sub_port_intf_profile_removal(rif_oid)

    def test_sub_port_intf_creation(self, dvs):
        self.connect_dbs(dvs)

        self._test_sub_port_intf_creation(dvs, self.SUB_PORT_INTERFACE_UNDER_TEST)
        self._test_sub_port_intf_creation(dvs, self.LAG_SUB_PORT_INTERFACE_UNDER_TEST)

    def _test_sub_port_intf_add_ip_addrs(self, dvs, sub_port_intf_name):
        substrs = sub_port_intf_name.split(VLAN_SUB_INTERFACE_SEPARATOR)
        parent_port = substrs[0]

        old_rif_oids = self.get_oids(ASIC_RIF_TABLE)

        self.set_parent_port_admin_status(dvs, parent_port, "up")
        self.create_sub_port_intf_profile(sub_port_intf_name)

        self.add_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV4_ADDR_UNDER_TEST)
        self.add_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV6_ADDR_UNDER_TEST)

        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)

        # Verify that ip address state ok is pushed to STATE_DB INTERFACE_TABLE by Intfmgrd
        fv_dict = {
            "state": "ok",
        }
        self.check_sub_port_intf_fvs(self.state_db, STATE_INTERFACE_TABLE_NAME, \
                sub_port_intf_name + "|" + self.IPV4_ADDR_UNDER_TEST, fv_dict)
        self.check_sub_port_intf_fvs(self.state_db, STATE_INTERFACE_TABLE_NAME, \
                sub_port_intf_name + "|" + self.IPV6_ADDR_UNDER_TEST, fv_dict)

        # Verify that ip address configuration is synced to APPL_DB INTF_TABLE by Intfmgrd
        fv_dict = {
            "scope": "global",
            "family": "IPv4",
        }
        self.check_sub_port_intf_fvs(self.app_db, APP_INTF_TABLE_NAME, \
                sub_port_intf_name + ":" + self.IPV4_ADDR_UNDER_TEST, fv_dict)
        fv_dict["family"] = "IPv6"
        self.check_sub_port_intf_fvs(self.app_db, APP_INTF_TABLE_NAME, \
                sub_port_intf_name + ":" + self.IPV6_ADDR_UNDER_TEST, fv_dict)

        # Verify that an IPv4 ip2me route entry is created in ASIC_DB
        # Verify that an IPv4 subnet route entry is created in ASIC_DB
        # Verify that an IPv6 ip2me route entry is created in ASIC_DB
        # Verify that an IPv6 subnet route entry is created in ASIC_DB
        self.check_sub_port_intf_route_entries()

        # Remove IP addresses
        self.remove_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV4_ADDR_UNDER_TEST)
        self.remove_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV6_ADDR_UNDER_TEST)
        self.check_sub_port_intf_ip_addr_removal(sub_port_intf_name,
                                                 [self.IPV4_ADDR_UNDER_TEST,
                                                  self.IPV6_ADDR_UNDER_TEST])

        # Remove a sub port interface
        self.remove_sub_port_intf_profile(sub_port_intf_name)
        self.check_sub_port_intf_profile_removal(rif_oid)

    def test_sub_port_intf_add_ip_addrs(self, dvs):
        self.connect_dbs(dvs)

        self._test_sub_port_intf_add_ip_addrs(dvs, self.SUB_PORT_INTERFACE_UNDER_TEST)
        self._test_sub_port_intf_add_ip_addrs(dvs, self.LAG_SUB_PORT_INTERFACE_UNDER_TEST)

    def _test_sub_port_intf_admin_status_change(self, dvs, sub_port_intf_name):
        substrs = sub_port_intf_name.split(VLAN_SUB_INTERFACE_SEPARATOR)
        parent_port = substrs[0]

        old_rif_oids = self.get_oids(ASIC_RIF_TABLE)

        self.set_parent_port_admin_status(dvs, parent_port, "up")
        self.create_sub_port_intf_profile(sub_port_intf_name)

        self.add_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV4_ADDR_UNDER_TEST)
        self.add_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV6_ADDR_UNDER_TEST)

        fv_dict = {
            ADMIN_STATUS: "up",
        }
        self.check_sub_port_intf_fvs(self.app_db, APP_INTF_TABLE_NAME, sub_port_intf_name, fv_dict)

        fv_dict = {
            "SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE": "true",
            "SAI_ROUTER_INTERFACE_ATTR_ADMIN_V6_STATE": "true",
            "SAI_ROUTER_INTERFACE_ATTR_MTU": DEFAULT_MTU,
        }
        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)
        self.check_sub_port_intf_fvs(self.asic_db, ASIC_RIF_TABLE, rif_oid, fv_dict)

        # Change sub port interface admin status to down
        self.set_sub_port_intf_admin_status(sub_port_intf_name, "down")

        # Verify that sub port interface admin status change is synced to APP_DB by Intfmgrd
        fv_dict = {
            ADMIN_STATUS: "down",
        }
        self.check_sub_port_intf_fvs(self.app_db, APP_INTF_TABLE_NAME, sub_port_intf_name, fv_dict)

        # Verify that sub port router interface entry in ASIC_DB has the updated admin status
        fv_dict = {
            "SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE": "false",
            "SAI_ROUTER_INTERFACE_ATTR_ADMIN_V6_STATE": "false",
            "SAI_ROUTER_INTERFACE_ATTR_MTU": DEFAULT_MTU,
        }
        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)
        self.check_sub_port_intf_fvs(self.asic_db, ASIC_RIF_TABLE, rif_oid, fv_dict)

        # Change sub port interface admin status to up
        self.set_sub_port_intf_admin_status(sub_port_intf_name, "up")

        # Verify that sub port interface admin status change is synced to APP_DB by Intfmgrd
        fv_dict = {
            ADMIN_STATUS: "up",
        }
        self.check_sub_port_intf_fvs(self.app_db, APP_INTF_TABLE_NAME, sub_port_intf_name, fv_dict)

        # Verify that sub port router interface entry in ASIC_DB has the updated admin status
        fv_dict = {
            "SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE": "true",
            "SAI_ROUTER_INTERFACE_ATTR_ADMIN_V6_STATE": "true",
            "SAI_ROUTER_INTERFACE_ATTR_MTU": DEFAULT_MTU,
        }
        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)
        self.check_sub_port_intf_fvs(self.asic_db, ASIC_RIF_TABLE, rif_oid, fv_dict)

        # Remove IP addresses
        self.remove_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV4_ADDR_UNDER_TEST)
        self.remove_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV6_ADDR_UNDER_TEST)
        self.check_sub_port_intf_ip_addr_removal(sub_port_intf_name,
                                                 [self.IPV4_ADDR_UNDER_TEST,
                                                  self.IPV6_ADDR_UNDER_TEST])

        # Remove a sub port interface
        self.remove_sub_port_intf_profile(sub_port_intf_name)
        self.check_sub_port_intf_profile_removal(rif_oid)

    def test_sub_port_intf_admin_status_change(self, dvs):
        self.connect_dbs(dvs)

        self._test_sub_port_intf_admin_status_change(dvs, self.SUB_PORT_INTERFACE_UNDER_TEST)
        self._test_sub_port_intf_admin_status_change(dvs, self.LAG_SUB_PORT_INTERFACE_UNDER_TEST)

    def _test_sub_port_intf_remove_ip_addrs(self, dvs, sub_port_intf_name):
        substrs = sub_port_intf_name.split(VLAN_SUB_INTERFACE_SEPARATOR)
        parent_port = substrs[0]

        old_rif_oids = self.get_oids(ASIC_RIF_TABLE)

        self.set_parent_port_admin_status(dvs, parent_port, "up")
        self.create_sub_port_intf_profile(sub_port_intf_name)

        self.add_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV4_ADDR_UNDER_TEST)
        self.add_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV6_ADDR_UNDER_TEST)

        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)

        # Remove IPv4 address
        self.remove_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV4_ADDR_UNDER_TEST)

        # Verify that IPv4 address state ok is removed from STATE_DB INTERFACE_TABLE by Intfmgrd
        self.check_sub_port_intf_key_removal(self.state_db, STATE_INTERFACE_TABLE_NAME, \
                sub_port_intf_name + "|" + self.IPV4_ADDR_UNDER_TEST)

        # Verify that IPv4 address configuration is removed from APPL_DB INTF_TABLE by Intfmgrd
        self.check_sub_port_intf_key_removal(self.app_db, APP_INTF_TABLE_NAME, \
                sub_port_intf_name + ":" + self.IPV4_ADDR_UNDER_TEST)

        # Verify that IPv4 subnet route entry is removed from ASIC_DB
        # Verify that IPv4 ip2me route entry is removed from ASIC_DB
        removed_route_entries = set([self.IPV4_TOME_UNDER_TEST, self.IPV4_SUBNET_UNDER_TEST])
        self.check_sub_port_intf_route_entries_removal(removed_route_entries)

        # Remove IPv6 address
        self.remove_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV6_ADDR_UNDER_TEST)

        # Verify that IPv6 address state ok is removed from STATE_DB INTERFACE_TABLE by Intfmgrd
        self.check_sub_port_intf_key_removal(self.state_db, STATE_INTERFACE_TABLE_NAME, \
                sub_port_intf_name + "|" + self.IPV6_ADDR_UNDER_TEST)

        # Verify that IPv6 address configuration is removed from APPL_DB INTF_TABLE by Intfmgrd
        self.check_sub_port_intf_key_removal(self.app_db, APP_INTF_TABLE_NAME, \
                sub_port_intf_name + ":" + self.IPV6_ADDR_UNDER_TEST)

        # Verify that IPv6 subnet route entry is removed from ASIC_DB
        # Verify that IPv6 ip2me route entry is removed from ASIC_DB
        removed_route_entries.update([self.IPV6_TOME_UNDER_TEST, self.IPV6_SUBNET_UNDER_TEST])
        self.check_sub_port_intf_route_entries_removal(removed_route_entries)

        # Verify that sub port router interface entry still exists in ASIC_DB
        self.check_sub_port_intf_key_existence(self.asic_db, ASIC_RIF_TABLE, rif_oid)

        # Remove a sub port interface
        self.remove_sub_port_intf_profile(sub_port_intf_name)
        self.check_sub_port_intf_profile_removal(rif_oid)

    def test_sub_port_intf_remove_ip_addrs(self, dvs):
        self.connect_dbs(dvs)

        self._test_sub_port_intf_remove_ip_addrs(dvs, self.SUB_PORT_INTERFACE_UNDER_TEST)
        self._test_sub_port_intf_remove_ip_addrs(dvs, self.LAG_SUB_PORT_INTERFACE_UNDER_TEST)

    def _test_sub_port_intf_removal(self, dvs, sub_port_intf_name):
        substrs = sub_port_intf_name.split(VLAN_SUB_INTERFACE_SEPARATOR)
        parent_port = substrs[0]
        if parent_port.startswith(ETHERNET_PREFIX):
            state_tbl_name = STATE_PORT_TABLE_NAME
        else:
            assert parent_port.startswith(LAG_PREFIX)
            state_tbl_name = STATE_LAG_TABLE_NAME

        old_rif_oids = self.get_oids(ASIC_RIF_TABLE)

        self.set_parent_port_admin_status(dvs, parent_port, "up")
        self.create_sub_port_intf_profile(sub_port_intf_name)

        self.add_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV4_ADDR_UNDER_TEST)
        self.add_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV6_ADDR_UNDER_TEST)

        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)

        fv_dict = {
            "state": "ok",
        }
        self.check_sub_port_intf_fvs(self.state_db, state_tbl_name, sub_port_intf_name, fv_dict)

        fv_dict = {
            ADMIN_STATUS: "up",
        }
        self.check_sub_port_intf_fvs(self.app_db, APP_INTF_TABLE_NAME, sub_port_intf_name, fv_dict)

        # Remove IP addresses
        self.remove_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV4_ADDR_UNDER_TEST)
        self.remove_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV6_ADDR_UNDER_TEST)
        self.check_sub_port_intf_ip_addr_removal(sub_port_intf_name,
                                                 [self.IPV4_ADDR_UNDER_TEST,
                                                  self.IPV6_ADDR_UNDER_TEST])

        # Remove a sub port interface
        self.remove_sub_port_intf_profile(sub_port_intf_name)
        self.check_sub_port_intf_profile_removal(rif_oid)

        # Verify that sub port interface state ok is removed from STATE_DB by Intfmgrd
        self.check_sub_port_intf_key_removal(self.state_db, state_tbl_name, sub_port_intf_name)

        # Verify that sub port interface configuration is removed from APP_DB by Intfmgrd
        self.check_sub_port_intf_key_removal(self.app_db, APP_INTF_TABLE_NAME, sub_port_intf_name)

        # Verify that sub port router interface entry is removed from ASIC_DB
        self.check_sub_port_intf_key_removal(self.asic_db, ASIC_RIF_TABLE, rif_oid)

    def test_sub_port_intf_removal(self, dvs):
        self.connect_dbs(dvs)

        self._test_sub_port_intf_removal(dvs, self.SUB_PORT_INTERFACE_UNDER_TEST)
        self._test_sub_port_intf_removal(dvs, self.LAG_SUB_PORT_INTERFACE_UNDER_TEST)

    def _test_sub_port_intf_mtu(self, dvs, sub_port_intf_name):
        substrs = sub_port_intf_name.split(VLAN_SUB_INTERFACE_SEPARATOR)
        parent_port = substrs[0]

        old_rif_oids = self.get_oids(ASIC_RIF_TABLE)

        self.set_parent_port_admin_status(dvs, parent_port, "up")
        self.create_sub_port_intf_profile(sub_port_intf_name)

        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)

        # Change parent port mtu
        mtu = "8888"
        dvs.set_mtu(parent_port, mtu)

        # Verify that sub port router interface entry in ASIC_DB has the updated mtu
        fv_dict = {
            "SAI_ROUTER_INTERFACE_ATTR_MTU": mtu,
        }
        self.check_sub_port_intf_fvs(self.asic_db, ASIC_RIF_TABLE, rif_oid, fv_dict)

        # Restore parent port mtu
        dvs.set_mtu(parent_port, DEFAULT_MTU)

        # Verify that sub port router interface entry in ASIC_DB has the default mtu
        fv_dict = {
            "SAI_ROUTER_INTERFACE_ATTR_MTU": DEFAULT_MTU,
        }
        self.check_sub_port_intf_fvs(self.asic_db, ASIC_RIF_TABLE, rif_oid, fv_dict)

        # Remove a sub port interface
        self.remove_sub_port_intf_profile(sub_port_intf_name)
        self.check_sub_port_intf_profile_removal(rif_oid)

    def test_sub_port_intf_mtu(self, dvs):
        self.connect_dbs(dvs)

        self._test_sub_port_intf_mtu(dvs, self.SUB_PORT_INTERFACE_UNDER_TEST)
        self._test_sub_port_intf_mtu(dvs, self.LAG_SUB_PORT_INTERFACE_UNDER_TEST)

    def create_nhg_router_intfs(self, dvs, parent_port_prefix, parent_port_idx_base, vlan_id, nhop_num):
        ifnames = []
        parent_port_idx = parent_port_idx_base
        for i in range(0, nhop_num):
            if vlan_id != 0:
                port_name = "{}{}.{}".format(parent_port_prefix, parent_port_idx, vlan_id)
            else:
                port_name = "{}{}".format(parent_port_prefix, parent_port_idx)
            ip_addr = "10.{}.{}.0/31".format(parent_port_idx, vlan_id)
            if vlan_id != 0:
                self.create_sub_port_intf_profile(port_name)
                self.add_sub_port_intf_ip_addr(port_name, ip_addr)
            else:
                dvs.add_ip_address(port_name, ip_addr)

            ifnames.append(port_name)

            parent_port_idx += (4 if parent_port_prefix == ETHERNET_PREFIX else 1)
        return ifnames

    def create_nhg_next_hop_objs(self, dvs, parent_port_prefix, parent_port_idx_base, vlan_id, nhop_num):
        nhop_ips = []
        parent_port_idx = parent_port_idx_base
        for i in range(0, nhop_num):
            nhop_ip = "10.{}.{}.1".format(parent_port_idx, vlan_id)
            nhop_mac = "00:00:00:{:02d}:{}:01".format(parent_port_idx, vlan_id)
            dvs.runcmd("arp -s " + nhop_ip + " " + nhop_mac)

            nhop_ips.append(nhop_ip)

            parent_port_idx += (4 if parent_port_prefix == ETHERNET_PREFIX else 1)
        return nhop_ips

    def remove_nhg_router_intfs(self, dvs, parent_port_prefix, parent_port_idx_base, vlan_id, nhop_num):
        parent_port_idx = parent_port_idx_base
        for i in range(0, nhop_num):
            if vlan_id != 0:
                port_name = "{}{}.{}".format(parent_port_prefix, parent_port_idx, vlan_id)
            else:
                port_name = "{}{}".format(parent_port_prefix, parent_port_idx)
            ip_addr = "10.{}.{}.0/31".format(parent_port_idx, vlan_id)
            if vlan_id != 0:
                # Remove IP address on sub port interface
                self.remove_sub_port_intf_ip_addr(port_name, ip_addr)
                # Remove sub port interface
                self.remove_sub_port_intf_profile(port_name)
            else:
                dvs.remove_ip_address(port_name, ip_addr)

            parent_port_idx += (4 if parent_port_prefix == ETHERNET_PREFIX else 1)

    def remove_nhg_next_hop_objs(self, dvs, parent_port_prefix, parent_port_idx_base, vlan_id, nhop_num):
        parent_port_idx = parent_port_idx_base
        for i in range(0, nhop_num):
            nhop_ip = "10.{}.{}.1".format(parent_port_idx, vlan_id)
            dvs.runcmd("arp -d " + nhop_ip)

            parent_port_idx += (4 if parent_port_prefix == ETHERNET_PREFIX else 1)

    def check_nhg_members_on_parent_port_oper_status_change(self, dvs, parent_port_prefix, parent_port_idx_base, status, \
                                                            nhg_oid, nhop_num, create_intf_on_parent_port):
        parent_port_idx = parent_port_idx_base
        for i in range(0, nhop_num):
            port_name = "{}{}".format(parent_port_prefix, parent_port_idx)
            self.set_parent_port_oper_status(dvs, port_name, status)

            # Verify parent port oper status
            fv_dict = {
                "oper_status" : status,
            }
            if parent_port_prefix == ETHERNET_PREFIX:
                self.check_sub_port_intf_fvs(self.app_db, APP_PORT_TABLE_NAME, port_name, fv_dict)
            else:
                self.check_sub_port_intf_fvs(self.app_db, APP_LAG_TABLE_NAME, port_name, fv_dict)

            # Verify next hop group member # in ASIC_DB
            if status == "up":
                nhg_member_oids = self.asic_db.wait_for_n_keys(ASIC_NEXT_HOP_GROUP_MEMBER_TABLE, \
                                                               1 + i if create_intf_on_parent_port == False else (1 + i) * 2)
            else:
                assert status == "down"
                nhg_member_oids = self.asic_db.wait_for_n_keys(ASIC_NEXT_HOP_GROUP_MEMBER_TABLE, \
                                                               (nhop_num - 1) - i if create_intf_on_parent_port == False else \
                                                               ((nhop_num - 1) - i) * 2)

            # Verify that next hop group members in ASIC_DB all
            # belong to the next hop group of the specified oid
            fv_dict = {
                "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID": nhg_oid,
            }
            for nhg_member_oid in nhg_member_oids:
                self.check_sub_port_intf_fvs(self.asic_db, ASIC_NEXT_HOP_GROUP_MEMBER_TABLE, nhg_member_oid, fv_dict)

            parent_port_idx += (4 if parent_port_prefix == ETHERNET_PREFIX else 1)

    def _test_sub_port_intf_nhg_accel(self, dvs, sub_port_intf_name, nhop_num=3, create_intf_on_parent_port=False):
        substrs = sub_port_intf_name.split(VLAN_SUB_INTERFACE_SEPARATOR)
        parent_port = substrs[0]
        vlan_id = substrs[1]
        assert len(vlan_id) == 2

        if parent_port.startswith(ETHERNET_PREFIX):
            parent_port_prefix = ETHERNET_PREFIX
        else:
            assert parent_port.startswith(LAG_PREFIX)
            parent_port_prefix = LAG_PREFIX
        parent_port_idx_base = self.get_parent_port_index(parent_port)

        # Set parent ports admin status up
        parent_port_idx = parent_port_idx_base
        for i in range(0, nhop_num):
            port_name = "{}{}".format(parent_port_prefix, parent_port_idx)
            self.set_parent_port_admin_status(dvs, port_name, "up")

            parent_port_idx += (4 if parent_port_prefix == ETHERNET_PREFIX else 1)

        ifnames = []
        # Create sub port interfaces
        ifnames.extend(self.create_nhg_router_intfs(dvs, parent_port_prefix, parent_port_idx_base, int(vlan_id), nhop_num))

        # Create router interfaces on parent ports
        if create_intf_on_parent_port == True:
            ifnames.extend(self.create_nhg_router_intfs(dvs, parent_port_prefix, parent_port_idx_base, 0, nhop_num))

        nhop_ips = []
        nhop_cnt = len(self.asic_db.get_keys(ASIC_NEXT_HOP_TABLE))
        # Create next hop objects on sub port interfaces
        nhop_ips.extend(self.create_nhg_next_hop_objs(dvs, parent_port_prefix, parent_port_idx_base, int(vlan_id), nhop_num))

        # Create next hop objects on router interfaces
        if create_intf_on_parent_port == True:
            nhop_ips.extend(self.create_nhg_next_hop_objs(dvs, parent_port_prefix, parent_port_idx_base, 0, nhop_num))

        self.asic_db.wait_for_n_keys(ASIC_NEXT_HOP_TABLE, nhop_cnt + nhop_num if create_intf_on_parent_port == False else nhop_cnt + nhop_num * 2)

        # Create multi-next-hop route entry
        rt_tbl = swsscommon.ProducerStateTable(self.app_db.db_connection, APP_ROUTE_TABLE_NAME)
        fvs = swsscommon.FieldValuePairs([("nexthop", ",".join(nhop_ips)), ("ifname", ",".join(ifnames))])
        ip_prefix = "2.2.2.0/24"
        rt_tbl.set(ip_prefix, fvs)

        # Verify route entry created in ASIC_DB and get next hop group oid
        nhg_oid = self.get_ip_prefix_nhg_oid(ip_prefix)

        # Verify next hop group of the specified oid created in ASIC_DB
        self.check_sub_port_intf_key_existence(self.asic_db, ASIC_NEXT_HOP_GROUP_TABLE, nhg_oid)

        # Verify next hop group members created in ASIC_DB
        nhg_member_oids = self.asic_db.wait_for_n_keys(ASIC_NEXT_HOP_GROUP_MEMBER_TABLE, \
                                                       nhop_num if create_intf_on_parent_port == False else nhop_num * 2)

        # Verify that next hop group members all belong to the next hop group of the specified oid
        fv_dict = {
            "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID": nhg_oid,
        }
        for nhg_member_oid in nhg_member_oids:
            self.check_sub_port_intf_fvs(self.asic_db, ASIC_NEXT_HOP_GROUP_MEMBER_TABLE, nhg_member_oid, fv_dict)

        # Bring parent ports oper status down one at a time, and verify next hop group members
        self.check_nhg_members_on_parent_port_oper_status_change(dvs, parent_port_prefix, parent_port_idx_base, "down", \
                                                                 nhg_oid, nhop_num, create_intf_on_parent_port)

        # Bring parent ports oper status up one at a time, and verify next hop group members
        self.check_nhg_members_on_parent_port_oper_status_change(dvs, parent_port_prefix, parent_port_idx_base, "up", \
                                                                 nhg_oid, nhop_num, create_intf_on_parent_port)

        # Clean up
        rif_cnt = len(self.asic_db.get_keys(ASIC_RIF_TABLE))

        # Remove sub port interfaces
        self.remove_nhg_router_intfs(dvs, parent_port_prefix, parent_port_idx_base, int(vlan_id), nhop_num)

        # Remove router interfaces on parent ports
        if create_intf_on_parent_port == True:
            self.remove_nhg_router_intfs(dvs, parent_port_prefix, parent_port_idx_base, 0, nhop_num)

        # Remove ecmp route entry
        rt_tbl._del(ip_prefix)

        # Removal of router interfaces indicates the proper removal of nhg, nhg members, next hop objects, and neighbor entries
        self.asic_db.wait_for_n_keys(ASIC_RIF_TABLE, rif_cnt - nhop_num if create_intf_on_parent_port == False else rif_cnt - nhop_num * 2)

        # Make sure parent port is oper status up
        parent_port_idx = parent_port_idx_base
        for i in range(0, nhop_num):
            port_name = "{}{}".format(parent_port_prefix, parent_port_idx)
            self.set_parent_port_oper_status(dvs, port_name, "up")

            parent_port_idx += (4 if parent_port_prefix == ETHERNET_PREFIX else 1)

    def test_sub_port_intf_nhg_accel(self, dvs):
        self.connect_dbs(dvs)

        self._test_sub_port_intf_nhg_accel(dvs, self.SUB_PORT_INTERFACE_UNDER_TEST)
        self._test_sub_port_intf_nhg_accel(dvs, self.SUB_PORT_INTERFACE_UNDER_TEST, create_intf_on_parent_port=True)
        self._test_sub_port_intf_nhg_accel(dvs, self.LAG_SUB_PORT_INTERFACE_UNDER_TEST)
        self._test_sub_port_intf_nhg_accel(dvs, self.LAG_SUB_PORT_INTERFACE_UNDER_TEST, create_intf_on_parent_port=True)

    def _test_sub_port_intf_oper_down_with_pending_neigh_route_tasks(self, dvs, sub_port_intf_name, nhop_num=3, create_intf_on_parent_port=False):
        substrs = sub_port_intf_name.split(VLAN_SUB_INTERFACE_SEPARATOR)
        parent_port = substrs[0]
        vlan_id = substrs[1]
        assert len(vlan_id) == 2

        if parent_port.startswith(ETHERNET_PREFIX):
            parent_port_prefix = ETHERNET_PREFIX
        else:
            assert parent_port.startswith(LAG_PREFIX)
            parent_port_prefix = LAG_PREFIX
        parent_port_idx_base = self.get_parent_port_index(parent_port)

        # Set parent ports admin status up
        parent_port_idx = parent_port_idx_base
        for i in range(0, nhop_num):
            port_name = "{}{}".format(parent_port_prefix, parent_port_idx)
            self.set_parent_port_admin_status(dvs, port_name, "up")

            parent_port_idx += (4 if parent_port_prefix == ETHERNET_PREFIX else 1)

        ifnames = []
        # Create sub port interfaces
        ifnames.extend(self.create_nhg_router_intfs(dvs, parent_port_prefix, parent_port_idx_base, int(vlan_id), nhop_num))
        # Create router interfaces on parent ports
        if create_intf_on_parent_port == True:
            ifnames.extend(self.create_nhg_router_intfs(dvs, parent_port_prefix, parent_port_idx_base, 0, nhop_num))

        # Bring parent port oper status down one at a time
        # Verify next hop group members created after processing pending tasks
        parent_port_idx = parent_port_idx_base
        for i in range(0, nhop_num):
            port_name = "{}{}".format(parent_port_prefix, parent_port_idx)
            self.set_parent_port_oper_status(dvs, port_name, "down")

            # Verify parent port oper status down
            fv_dict = {
                "oper_status" : "down",
            }
            if parent_port_prefix == ETHERNET_PREFIX:
                self.check_sub_port_intf_fvs(self.app_db, APP_PORT_TABLE_NAME, port_name, fv_dict)
            else:
                self.check_sub_port_intf_fvs(self.app_db, APP_LAG_TABLE_NAME, port_name, fv_dict)

            # Mimic pending neighbor task
            nhop_ips = []
            nhop_cnt = len(self.asic_db.get_keys(ASIC_NEXT_HOP_TABLE))
            # Create next hop objects on sub port interfaces
            nhop_ips.extend(self.create_nhg_next_hop_objs(dvs, parent_port_prefix, parent_port_idx_base, int(vlan_id), nhop_num))
            # Create next hop objects on router interfaces
            if create_intf_on_parent_port == True:
                nhop_ips.extend(self.create_nhg_next_hop_objs(dvs, parent_port_prefix, parent_port_idx_base, 0, nhop_num))
            self.asic_db.wait_for_n_keys(ASIC_NEXT_HOP_TABLE, nhop_cnt + nhop_num if create_intf_on_parent_port == False else nhop_cnt + nhop_num * 2)

            # Mimic pending multi-next-hop route entry task
            rt_tbl = swsscommon.ProducerStateTable(self.app_db.db_connection, APP_ROUTE_TABLE_NAME)
            fvs = swsscommon.FieldValuePairs([("nexthop", ",".join(nhop_ips)), ("ifname", ",".join(ifnames))])
            ip_prefix = "2.2.2.0/24"
            rt_tbl.set(ip_prefix, fvs)

            # Verify route entry created in ASIC_DB and get next hop group oid
            nhg_oid = self.get_ip_prefix_nhg_oid(ip_prefix)

            # Verify next hop group of the specified oid created in ASIC_DB
            self.check_sub_port_intf_key_existence(self.asic_db, ASIC_NEXT_HOP_GROUP_TABLE, nhg_oid)

            # Verify next hop group member # created in ASIC_DB
            nhg_member_oids = self.asic_db.wait_for_n_keys(ASIC_NEXT_HOP_GROUP_MEMBER_TABLE, \
                                                           (nhop_num - 1) - i if create_intf_on_parent_port == False else ((nhop_num - 1) - i) * 2)

            # Verify that next hop group members all belong to the next hop group of the specified oid
            fv_dict = {
                "SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID": nhg_oid,
            }
            for nhg_member_oid in nhg_member_oids:
                self.check_sub_port_intf_fvs(self.asic_db, ASIC_NEXT_HOP_GROUP_MEMBER_TABLE, nhg_member_oid, fv_dict)

            nhop_cnt = len(self.asic_db.get_keys(ASIC_NEXT_HOP_TABLE))
            # Remove next hop objects on sub port interfaces
            self.remove_nhg_next_hop_objs(dvs, parent_port_prefix, parent_port_idx_base, int(vlan_id), nhop_num)
            # Remove next hop objects on router interfaces
            if create_intf_on_parent_port == True:
                self.remove_nhg_next_hop_objs(dvs, parent_port_prefix, parent_port_idx_base, 0, nhop_num)
            # Remove ecmp route entry
            rt_tbl._del(ip_prefix)
            # Removal of next hop objects indicates the proper removal of route entry, nhg, and nhg members
            self.asic_db.wait_for_n_keys(ASIC_NEXT_HOP_TABLE, nhop_cnt - nhop_num if create_intf_on_parent_port == False else nhop_cnt - nhop_num * 2)

            parent_port_idx += (4 if parent_port_prefix == ETHERNET_PREFIX else 1)

        # Clean up
        rif_cnt = len(self.asic_db.get_keys(ASIC_RIF_TABLE))
        # Remove sub port interfaces
        self.remove_nhg_router_intfs(dvs, parent_port_prefix, parent_port_idx_base, int(vlan_id), nhop_num)
        # Remove router interfaces on parent ports
        if create_intf_on_parent_port == True:
            self.remove_nhg_router_intfs(dvs, parent_port_prefix, parent_port_idx_base, 0, nhop_num)
        self.asic_db.wait_for_n_keys(ASIC_RIF_TABLE, rif_cnt - nhop_num if create_intf_on_parent_port == False else rif_cnt - nhop_num * 2)

        # Make sure parent port oper status is up
        parent_port_idx = parent_port_idx_base
        for i in range(0, nhop_num):
            port_name = "{}{}".format(parent_port_prefix, parent_port_idx)
            self.set_parent_port_oper_status(dvs, port_name, "up")

            parent_port_idx += (4 if parent_port_prefix == ETHERNET_PREFIX else 1)

    def test_sub_port_intf_oper_down_with_pending_neigh_route_tasks(self, dvs):
        self.connect_dbs(dvs)

        self._test_sub_port_intf_oper_down_with_pending_neigh_route_tasks(dvs, self.SUB_PORT_INTERFACE_UNDER_TEST)
        self._test_sub_port_intf_oper_down_with_pending_neigh_route_tasks(dvs, self.SUB_PORT_INTERFACE_UNDER_TEST, create_intf_on_parent_port=True)
        self._test_sub_port_intf_oper_down_with_pending_neigh_route_tasks(dvs, self.LAG_SUB_PORT_INTERFACE_UNDER_TEST)
        self._test_sub_port_intf_oper_down_with_pending_neigh_route_tasks(dvs, self.LAG_SUB_PORT_INTERFACE_UNDER_TEST, create_intf_on_parent_port=True)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
