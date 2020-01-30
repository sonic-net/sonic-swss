import pytest
import time
import json

from swsscommon import swsscommon

CFG_VLAN_SUB_INTF_TABLE_NAME = "VLAN_SUB_INTERFACE"
CFG_PORT_TABLE_NAME = "PORT"

STATE_PORT_TABLE_NAME = "PORT_TABLE"
STATE_INTERFACE_TABLE_NAME = "INTERFACE_TABLE"

APP_INTF_TABLE_NAME = "INTF_TABLE"

ASIC_RIF_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
ASIC_ROUTE_ENTRY_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"

ADMIN_STATUS = "admin_status"


class TestSubPortIntf(object):
    PHYSICAL_PORT_UNDER_TEST = "Ethernet64"
    SUB_PORT_INTERFACE_UNDER_TEST = "Ethernet64.10"

    IPV4_ADDR_UNDER_TEST = "10.0.0.33/31"
    IPV4_TOME_UNDER_TEST = "10.0.0.33/32"
    IPV4_SUBNET_UNDER_TEST = "10.0.0.32/31"

    IPV6_ADDR_UNDER_TEST = "fc00::41/126"
    IPV6_TOME_UNDER_TEST = "fc00::41/128"
    IPV6_SUBNET_UNDER_TEST = "fc00::40/126"

    def connect_dbs(self, dvs):
        self.config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        self.state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
        self.appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        self.asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

    def set_parent_port_admin_status(self, port_name, status):
        fvs = swsscommon.FieldValuePairs([(ADMIN_STATUS, status)])

        tbl = swsscommon.Table(self.config_db, CFG_PORT_TABLE_NAME)
        tbl.set(port_name, fvs)

        time.sleep(1)

    def create_sub_port_intf_profile(self, sub_port_intf_name):
        fvs = swsscommon.FieldValuePairs([(ADMIN_STATUS, "up")])

        tbl = swsscommon.Table(self.config_db, CFG_VLAN_SUB_INTF_TABLE_NAME)
        tbl.set(sub_port_intf_name, fvs)

        time.sleep(1)

    def add_sub_port_intf_ip_addr(self, sub_port_intf_name, ip_addr):
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])

        tbl = swsscommon.Table(self.config_db, CFG_VLAN_SUB_INTF_TABLE_NAME)
        tbl.set(sub_port_intf_name + "|" + ip_addr, fvs)

        time.sleep(2)

    def set_sub_port_intf_admin_status(self, sub_port_intf_name, status):
        fvs = swsscommon.FieldValuePairs([(ADMIN_STATUS, status)])

        tbl = swsscommon.Table(self.config_db, CFG_VLAN_SUB_INTF_TABLE_NAME)
        tbl.set(sub_port_intf_name, fvs)

        time.sleep(1)

    def remove_sub_port_intf_profile(self, sub_port_intf_name):
        tbl = swsscommon.Table(self.config_db, CFG_VLAN_SUB_INTF_TABLE_NAME)
        tbl._del(sub_port_intf_name)

        time.sleep(1)

    def remove_sub_port_intf_ip_addr(self, sub_port_intf_name, ip_addr):
        tbl = swsscommon.Table(self.config_db, CFG_VLAN_SUB_INTF_TABLE_NAME)
        tbl._del(sub_port_intf_name + "|" + ip_addr)

        time.sleep(1)

    def get_oids(self, table):
        tbl = swsscommon.Table(self.asic_db, table)
        return set(tbl.getKeys())

    def get_newly_created_oid(self, table, old_oids):
        new_oids = self.get_oids(table)
        oid = list(new_oids - old_oids)
        assert len(oid) == 1, "Wrong # of newly created oids: %d, expected #: 1." % (len(oid))
        return oid[0]

    def check_sub_port_intf_key_existence(self, db, table_name, key):
        tbl = swsscommon.Table(db, table_name)

        keys = tbl.getKeys()
        assert key in keys, "Key %s not exist" % (key)

    def check_sub_port_intf_fvs(self, db, table_name, key, fv_dict):
        tbl = swsscommon.Table(db, table_name)

        keys = tbl.getKeys()
        assert key in keys

        (status, fvs) = tbl.get(key)
        assert status == True
        assert len(fvs) >= len(fv_dict)

        for field, value in fvs:
            if field in fv_dict:
                assert fv_dict[field] == value, \
                    "Wrong value for field %s: %s, expected value: %s" % (field, value, fv_dict[field])

    def check_sub_port_intf_route_entries(self):
        ipv4_ip2me_found = False
        ipv4_subnet_found = False
        ipv6_ip2me_found = False
        ipv6_subnet_found = False

        tbl = swsscommon.Table(self.asic_db, ASIC_ROUTE_ENTRY_TABLE)
        raw_route_entries = tbl.getKeys()

        for raw_route_entry in raw_route_entries:
            route_entry = json.loads(raw_route_entry)
            if route_entry["dest"] == self.IPV4_TOME_UNDER_TEST:
                ipv4_ip2me_found = True
            elif route_entry["dest"] == self.IPV4_SUBNET_UNDER_TEST:
                ipv4_subnet_found = True
            elif route_entry["dest"] == self.IPV6_TOME_UNDER_TEST:
                ipv6_ip2me_found = True
            elif route_entry["dest"] == self.IPV6_SUBNET_UNDER_TEST:
                ipv6_subnet_found = True

        assert ipv4_ip2me_found and ipv4_subnet_found and ipv6_ip2me_found and ipv6_subnet_found

    def check_sub_port_intf_key_removal(self, db, table_name, key):
        tbl = swsscommon.Table(db, table_name)

        keys = tbl.getKeys()
        assert key not in keys, "Key %s not removed" % (key)

    def check_sub_port_intf_route_entries_removal(self, removed_route_entries):
        tbl = swsscommon.Table(self.asic_db, ASIC_ROUTE_ENTRY_TABLE)
        raw_route_entries = tbl.getKeys()
        for raw_route_entry in raw_route_entries:
            route_entry = json.loads(raw_route_entry)
            assert route_entry["dest"] not in removed_route_entries

    def test_sub_port_intf_creation(self, dvs):
        self.connect_dbs(dvs)

        old_rif_oids = self.get_oids(ASIC_RIF_TABLE)

        self.set_parent_port_admin_status(self.PHYSICAL_PORT_UNDER_TEST, "up")
        self.create_sub_port_intf_profile(self.SUB_PORT_INTERFACE_UNDER_TEST)

        # Verify that sub port interface state ok is pushed to STATE_DB by Intfmgrd
        fv_dict = {
            "state": "ok",
        }
        self.check_sub_port_intf_fvs(self.state_db, STATE_PORT_TABLE_NAME, self.SUB_PORT_INTERFACE_UNDER_TEST, fv_dict)

        # Verify that sub port interface configuration is synced to APPL_DB INTF_TABLE by Intfmgrd
        fv_dict = {
            ADMIN_STATUS: "up",
        }
        self.check_sub_port_intf_fvs(self.appl_db, APP_INTF_TABLE_NAME, self.SUB_PORT_INTERFACE_UNDER_TEST, fv_dict)

        # Verify that a sub port router interface entry is created in ASIC_DB
        fv_dict = {
            "SAI_ROUTER_INTERFACE_ATTR_TYPE": "SAI_ROUTER_INTERFACE_TYPE_SUB_PORT",
            "SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID": "10",
            "SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE": "true",
            "SAI_ROUTER_INTERFACE_ATTR_ADMIN_V6_STATE": "true",
            "SAI_ROUTER_INTERFACE_ATTR_MTU": "9100",
        }
        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)
        self.check_sub_port_intf_fvs(self.asic_db, ASIC_RIF_TABLE, rif_oid, fv_dict)

        # Remove a sub port interface
        self.remove_sub_port_intf_profile(self.SUB_PORT_INTERFACE_UNDER_TEST)

    def test_sub_port_intf_add_ip_addrs(self, dvs):
        self.connect_dbs(dvs)

        old_rif_oids = self.get_oids(ASIC_RIF_TABLE)

        self.set_parent_port_admin_status(self.PHYSICAL_PORT_UNDER_TEST, "up")
        self.create_sub_port_intf_profile(self.SUB_PORT_INTERFACE_UNDER_TEST)

        self.add_sub_port_intf_ip_addr(self.SUB_PORT_INTERFACE_UNDER_TEST, self.IPV4_ADDR_UNDER_TEST)
        self.add_sub_port_intf_ip_addr(self.SUB_PORT_INTERFACE_UNDER_TEST, self.IPV6_ADDR_UNDER_TEST)

        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)

        # Verify that ip address state ok is pushed to STATE_DB INTERFACE_TABLE by Intfmgrd
        fv_dict = {
            "state": "ok",
        }
        self.check_sub_port_intf_fvs(self.state_db, STATE_INTERFACE_TABLE_NAME, \
                self.SUB_PORT_INTERFACE_UNDER_TEST + "|" + self.IPV4_ADDR_UNDER_TEST, fv_dict)
        self.check_sub_port_intf_fvs(self.state_db, STATE_INTERFACE_TABLE_NAME, \
                self.SUB_PORT_INTERFACE_UNDER_TEST + "|" + self.IPV6_ADDR_UNDER_TEST, fv_dict)

        # Verify that ip address configuration is synced to APPL_DB INTF_TABLE by Intfmgrd
        fv_dict = {
            "scope": "global",
            "family": "IPv4",
        }
        self.check_sub_port_intf_fvs(self.appl_db, APP_INTF_TABLE_NAME, \
                self.SUB_PORT_INTERFACE_UNDER_TEST + ":" + self.IPV4_ADDR_UNDER_TEST, fv_dict)
        fv_dict["family"] = "IPv6"
        self.check_sub_port_intf_fvs(self.appl_db, APP_INTF_TABLE_NAME, \
                self.SUB_PORT_INTERFACE_UNDER_TEST + ":" + self.IPV6_ADDR_UNDER_TEST, fv_dict)

        # Verify that an IPv4 ip2me route entry is created in ASIC_DB
        # Verify that an IPv4 subnet route entry is created in ASIC_DB
        # Verify that an IPv6 ip2me route entry is created in ASIC_DB
        # Verify that an IPv6 subnet route entry is created in ASIC_DB
        self.check_sub_port_intf_route_entries()

        # Remove IP addresses
        self.remove_sub_port_intf_ip_addr(self.SUB_PORT_INTERFACE_UNDER_TEST, self.IPV4_ADDR_UNDER_TEST)
        self.remove_sub_port_intf_ip_addr(self.SUB_PORT_INTERFACE_UNDER_TEST, self.IPV6_ADDR_UNDER_TEST)
        # Remove a sub port interface
        self.remove_sub_port_intf_profile(self.SUB_PORT_INTERFACE_UNDER_TEST)

    def test_sub_port_intf_admin_status_change(self, dvs):
        self.connect_dbs(dvs)

        old_rif_oids = self.get_oids(ASIC_RIF_TABLE)

        self.set_parent_port_admin_status(self.PHYSICAL_PORT_UNDER_TEST, "up")
        self.create_sub_port_intf_profile(self.SUB_PORT_INTERFACE_UNDER_TEST)

        self.add_sub_port_intf_ip_addr(self.SUB_PORT_INTERFACE_UNDER_TEST, self.IPV4_ADDR_UNDER_TEST)
        self.add_sub_port_intf_ip_addr(self.SUB_PORT_INTERFACE_UNDER_TEST, self.IPV6_ADDR_UNDER_TEST)

        fv_dict = {
            ADMIN_STATUS: "up",
        }
        self.check_sub_port_intf_fvs(self.appl_db, APP_INTF_TABLE_NAME, self.SUB_PORT_INTERFACE_UNDER_TEST, fv_dict)

        fv_dict = {
            "SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE": "true",
            "SAI_ROUTER_INTERFACE_ATTR_ADMIN_V6_STATE": "true",
            "SAI_ROUTER_INTERFACE_ATTR_MTU": "9100",
        }
        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)
        self.check_sub_port_intf_fvs(self.asic_db, ASIC_RIF_TABLE, rif_oid, fv_dict)

        # Change sub port interface admin status to down
        self.set_sub_port_intf_admin_status(self.SUB_PORT_INTERFACE_UNDER_TEST, "down")

        # Verify that sub port interface admin status change is synced to APPL_DB INTF_TABLE by Intfmgrd
        fv_dict = {
            ADMIN_STATUS: "down",
        }
        self.check_sub_port_intf_fvs(self.appl_db, APP_INTF_TABLE_NAME, self.SUB_PORT_INTERFACE_UNDER_TEST, fv_dict)

        # Verify that sub port router interface entry in ASIC_DB has the updated admin status
        fv_dict = {
            "SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE": "false",
            "SAI_ROUTER_INTERFACE_ATTR_ADMIN_V6_STATE": "false",
            "SAI_ROUTER_INTERFACE_ATTR_MTU": "9100",
        }
        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)
        self.check_sub_port_intf_fvs(self.asic_db, ASIC_RIF_TABLE, rif_oid, fv_dict)

        # Change sub port interface admin status to up
        self.set_sub_port_intf_admin_status(self.SUB_PORT_INTERFACE_UNDER_TEST, "up")

        # Verify that sub port interface admin status change is synced to APPL_DB INTF_TABLE by Intfmgrd
        fv_dict = {
            ADMIN_STATUS: "up",
        }
        self.check_sub_port_intf_fvs(self.appl_db, APP_INTF_TABLE_NAME, self.SUB_PORT_INTERFACE_UNDER_TEST, fv_dict)

        # Verify that sub port router interface entry in ASIC_DB has the updated admin status
        fv_dict = {
            "SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE": "true",
            "SAI_ROUTER_INTERFACE_ATTR_ADMIN_V6_STATE": "true",
            "SAI_ROUTER_INTERFACE_ATTR_MTU": "9100",
        }
        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)
        self.check_sub_port_intf_fvs(self.asic_db, ASIC_RIF_TABLE, rif_oid, fv_dict)

        # Remove IP addresses
        self.remove_sub_port_intf_ip_addr(self.SUB_PORT_INTERFACE_UNDER_TEST, self.IPV4_ADDR_UNDER_TEST)
        self.remove_sub_port_intf_ip_addr(self.SUB_PORT_INTERFACE_UNDER_TEST, self.IPV6_ADDR_UNDER_TEST)
        # Remove a sub port interface
        self.remove_sub_port_intf_profile(self.SUB_PORT_INTERFACE_UNDER_TEST)

    def test_sub_port_intf_remove_ip_addrs(self, dvs):
        self.connect_dbs(dvs)

        old_rif_oids = self.get_oids(ASIC_RIF_TABLE)

        self.set_parent_port_admin_status(self.PHYSICAL_PORT_UNDER_TEST, "up")
        self.create_sub_port_intf_profile(self.SUB_PORT_INTERFACE_UNDER_TEST)

        self.add_sub_port_intf_ip_addr(self.SUB_PORT_INTERFACE_UNDER_TEST, self.IPV4_ADDR_UNDER_TEST)
        self.add_sub_port_intf_ip_addr(self.SUB_PORT_INTERFACE_UNDER_TEST, self.IPV6_ADDR_UNDER_TEST)

        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)

        # Remove IPv4 address
        self.remove_sub_port_intf_ip_addr(self.SUB_PORT_INTERFACE_UNDER_TEST, self.IPV4_ADDR_UNDER_TEST)

        # Verify that IPv4 address state ok is removed from STATE_DB INTERFACE_TABLE by Intfmgrd
        self.check_sub_port_intf_key_removal(self.state_db, STATE_INTERFACE_TABLE_NAME, \
                self.SUB_PORT_INTERFACE_UNDER_TEST + "|" + self.IPV4_ADDR_UNDER_TEST)

        # Verify that IPv4 address configuration is removed from APPL_DB INTF_TABLE by Intfmgrd
        self.check_sub_port_intf_key_removal(self.appl_db, APP_INTF_TABLE_NAME, \
                self.SUB_PORT_INTERFACE_UNDER_TEST + ":" + self.IPV4_ADDR_UNDER_TEST)

        # Verify that IPv4 subnet route entry is removed from ASIC_DB
        # Verify that IPv4 ip2me route entry is removed from ASIC_DB
        removed_route_entries = set([self.IPV4_TOME_UNDER_TEST, self.IPV4_SUBNET_UNDER_TEST])
        self.check_sub_port_intf_route_entries_removal(removed_route_entries)

        # Remove IPv6 address
        self.remove_sub_port_intf_ip_addr(self.SUB_PORT_INTERFACE_UNDER_TEST, self.IPV6_ADDR_UNDER_TEST)

        # Verify that IPv6 address state ok is removed from STATE_DB INTERFACE_TABLE by Intfmgrd
        self.check_sub_port_intf_key_removal(self.state_db, STATE_INTERFACE_TABLE_NAME, \
                self.SUB_PORT_INTERFACE_UNDER_TEST + "|" + self.IPV6_ADDR_UNDER_TEST)

        # Verify that IPv6 address configuration is removed from APPL_DB INTF_TABLE by Intfmgrd
        self.check_sub_port_intf_key_removal(self.appl_db, APP_INTF_TABLE_NAME, \
                self.SUB_PORT_INTERFACE_UNDER_TEST + ":" + self.IPV6_ADDR_UNDER_TEST)

        # Verify that IPv6 subnet route entry is removed from ASIC_DB
        # Verify that IPv6 ip2me route entry is removed from ASIC_DB
        removed_route_entries.update([self.IPV6_TOME_UNDER_TEST, self.IPV6_SUBNET_UNDER_TEST])
        self.check_sub_port_intf_route_entries_removal(removed_route_entries)

        # Verify that sub port router interface entry still exists in ASIC_DB
        self.check_sub_port_intf_key_existence(self.asic_db, ASIC_RIF_TABLE, rif_oid)

        # Remove a sub port interface
        self.remove_sub_port_intf_profile(self.SUB_PORT_INTERFACE_UNDER_TEST)

    def test_sub_port_intf_removal(self, dvs):
        self.connect_dbs(dvs)

        old_rif_oids = self.get_oids(ASIC_RIF_TABLE)

        self.set_parent_port_admin_status(self.PHYSICAL_PORT_UNDER_TEST, "up")
        self.create_sub_port_intf_profile(self.SUB_PORT_INTERFACE_UNDER_TEST)

        self.add_sub_port_intf_ip_addr(self.SUB_PORT_INTERFACE_UNDER_TEST, self.IPV4_ADDR_UNDER_TEST)
        self.add_sub_port_intf_ip_addr(self.SUB_PORT_INTERFACE_UNDER_TEST, self.IPV6_ADDR_UNDER_TEST)

        rif_oid = self.get_newly_created_oid(ASIC_RIF_TABLE, old_rif_oids)

        fv_dict = {
            "state": "ok",
        }
        self.check_sub_port_intf_fvs(self.state_db, STATE_PORT_TABLE_NAME, self.SUB_PORT_INTERFACE_UNDER_TEST, fv_dict)

        fv_dict = {
            ADMIN_STATUS: "up",
        }
        self.check_sub_port_intf_fvs(self.appl_db, APP_INTF_TABLE_NAME, self.SUB_PORT_INTERFACE_UNDER_TEST, fv_dict)

        # Remove IP addresses
        self.remove_sub_port_intf_ip_addr(self.SUB_PORT_INTERFACE_UNDER_TEST, self.IPV4_ADDR_UNDER_TEST)
        self.remove_sub_port_intf_ip_addr(self.SUB_PORT_INTERFACE_UNDER_TEST, self.IPV6_ADDR_UNDER_TEST)

        # Remove a sub port interface
        self.remove_sub_port_intf_profile(self.SUB_PORT_INTERFACE_UNDER_TEST)

        # Verify that sub port interface state ok is removed from STATE_DB by Intfmgrd
        self.check_sub_port_intf_key_removal(self.state_db, STATE_PORT_TABLE_NAME, self.SUB_PORT_INTERFACE_UNDER_TEST)

        # Verify that sub port interface configuration is removed from APPL_DB INTF_TABLE by Intfmgrd
        self.check_sub_port_intf_key_removal(self.appl_db, APP_INTF_TABLE_NAME, self.SUB_PORT_INTERFACE_UNDER_TEST)

        # Verify that sub port router interface entry is removed from ASIC_DB
        self.check_sub_port_intf_key_removal(self.asic_db, ASIC_RIF_TABLE, rif_oid)
