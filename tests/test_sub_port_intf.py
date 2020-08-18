import json

from dvslib.dvs_common import wait_for_result
from dvslib.dvs_database import DVSDatabase

CFG_VLAN_SUB_INTF_TABLE_NAME = "VLAN_SUB_INTERFACE"
CFG_PORT_TABLE_NAME = "PORT"
CFG_LAG_TABLE_NAME = "PORTCHANNEL"

STATE_PORT_TABLE_NAME = "PORT_TABLE"
STATE_LAG_TABLE_NAME = "LAG_TABLE"
STATE_INTERFACE_TABLE_NAME = "INTERFACE_TABLE"

APP_INTF_TABLE_NAME = "INTF_TABLE"

ASIC_RIF_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
ASIC_ROUTE_ENTRY_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"

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

    def set_parent_port_admin_status(self, dvs, port_name, status):
        fvs = {ADMIN_STATUS: status}

        if port_name.startswith(ETHERNET_PREFIX):
            tbl_name = CFG_PORT_TABLE_NAME
        else:
            assert port_name.startswith(LAG_PREFIX)
            tbl_name = CFG_LAG_TABLE_NAME
        self.config_db.create_entry(tbl_name, port_name, fvs)

        # follow the treatment in TestSubPortIntf::set_admin_status
        if tbl_name == CFG_LAG_TABLE_NAME:
            dvs.runcmd("bash -c 'echo " + ("1" if status == "up" else "0") + \
                    " > /sys/class/net/" + port_name + "/carrier'")

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

    def verify_sub_port_intf_removal(self, rif_oid):
        self.asic_db.wait_for_deleted_keys(ASIC_RIF_TABLE, [rif_oid])

    def remove_sub_port_intf_ip_addr(self, sub_port_intf_name, ip_addr):
        key = "{}|{}".format(sub_port_intf_name, ip_addr)
        self.config_db.delete_entry(CFG_VLAN_SUB_INTF_TABLE_NAME, key)

    def verify_sub_port_intf_ip_addr_removal(self, sub_port_intf_name, ip_addrs):
        interfaces = ["{}:{}".format(sub_port_intf_name, addr) for addr in ip_addrs]
        self.app_db.wait_for_deleted_keys(APP_INTF_TABLE_NAME, interfaces)

    def get_oids(self, table):
        return self.asic_db.get_keys(table)

    def get_newly_created_oid(self, table, old_oids):
        new_oids = self.asic_db.wait_for_n_keys(table, len(old_oids) + 1)
        oid = [ids for ids in new_oids if ids not in old_oids]
        return oid[0]

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
            "SAI_ROUTER_INTERFACE_ATTR_MTU": "9100",
        }
        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)
        self.check_sub_port_intf_fvs(self.asic_db, ASIC_RIF_TABLE, rif_oid, fv_dict)

        # Remove a sub port interface
        self.remove_sub_port_intf_profile(sub_port_intf_name)
        self.verify_sub_port_intf_removal(rif_oid)

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
        self.verify_sub_port_intf_ip_addr_removal(sub_port_intf_name,
                                                  [self.IPV4_ADDR_UNDER_TEST,
                                                   self.IPV6_ADDR_UNDER_TEST])

        # Remove a sub port interface
        self.remove_sub_port_intf_profile(sub_port_intf_name)
        self.verify_sub_port_intf_removal(rif_oid)

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
            "SAI_ROUTER_INTERFACE_ATTR_MTU": "9100",
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
            "SAI_ROUTER_INTERFACE_ATTR_MTU": "9100",
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
            "SAI_ROUTER_INTERFACE_ATTR_MTU": "9100",
        }
        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)
        self.check_sub_port_intf_fvs(self.asic_db, ASIC_RIF_TABLE, rif_oid, fv_dict)

        # Remove IP addresses
        self.remove_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV4_ADDR_UNDER_TEST)
        self.remove_sub_port_intf_ip_addr(sub_port_intf_name, self.IPV6_ADDR_UNDER_TEST)
        self.verify_sub_port_intf_ip_addr_removal(sub_port_intf_name,
                                                  [self.IPV4_ADDR_UNDER_TEST,
                                                   self.IPV6_ADDR_UNDER_TEST])

        # Remove a sub port interface
        self.remove_sub_port_intf_profile(sub_port_intf_name)
        self.verify_sub_port_intf_removal(rif_oid)

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
        self.verify_sub_port_intf_removal(rif_oid)

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
        self.verify_sub_port_intf_ip_addr_removal(sub_port_intf_name,
                                                  [self.IPV4_ADDR_UNDER_TEST,
                                                   self.IPV6_ADDR_UNDER_TEST])

        # Remove a sub port interface
        self.remove_sub_port_intf_profile(sub_port_intf_name)
        self.verify_sub_port_intf_removal(rif_oid)

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


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
