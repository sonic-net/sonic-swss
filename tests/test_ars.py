import os
import re
import time
import json
import pytest
import ipaddress
import subprocess
import pdb

from swsscommon import swsscommon
from dvslib.dvs_common import wait_for_result

# CONFIG_DB table names
CFG_ARS_PROFILE_TBL = "ARS_PROFILE"
CFG_ARS_OBJECT_TBL = "ARS_OBJECT"
CFG_ARS_INTERFACE_TBL = "ARS_INTERFACE"
CFG_ARS_NEXTHOP_TBL = "ARS_NEXTHOP"

# STATE_DB table names
STATE_ARS_PROFILE_TBL = "ARS_PROFILE_TABLE"
STATE_ARS_OBJECT_TBL = "ARS_OBJECT_TABLE"
STATE_ARS_INTERFACE_TBL = "ARS_INTERFACE_TABLE"


class TestArsBase(object):
    NUM_PORTS = 32
    def setup_db(self, dvs):
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()
        self.cdb = dvs.get_config_db()
        self.sdb = dvs.get_state_db()
        self.config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        self.state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
        self.asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        self.dvs = dvs

    def set_admin_status(self, interface, status):
        self.cdb.update_entry("PORT", interface, {"admin_status": status})

    def create_vrf(self, vrf_name):
        initial_entries = set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))

        self.cdb.create_entry("VRF", vrf_name, {"empty": "empty"})
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER", len(initial_entries) + 1)

        current_entries = set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))
        assert len(current_entries - initial_entries) == 1
        return list(current_entries - initial_entries)[0]

    def remove_vrf(self, vrf_name):
        self.cdb.delete_entry("VRF", vrf_name)

    def create_l3_intf(self, interface, vrf_name):
        if len(vrf_name) == 0:
            self.cdb.create_entry("INTERFACE", interface, {"NULL": "NULL"})
        else:
            self.cdb.create_entry("INTERFACE", interface, {"vrf_name": vrf_name})

    def remove_l3_intf(self, interface):
        self.cdb.delete_entry("INTERFACE", interface)

    def add_ip_address(self, interface, ip):
        self.cdb.create_entry("INTERFACE", interface + "|" + ip, {"NULL": "NULL"})

    def remove_ip_address(self, interface, ip):
        self.cdb.delete_entry("INTERFACE", interface + "|" + ip)

    def create_route_entry(self, key, pairs):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "ROUTE_TABLE")
        fvs = swsscommon.FieldValuePairs(list(pairs.items()))
        tbl.set(key, fvs)

    def remove_route_entry(self, key):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "ROUTE_TABLE")
        tbl._del(key)

    def check_route_entries(self, destinations):
        def _access_function():
            route_entries = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            route_destinations = [json.loads(route_entry)["dest"]
                                  for route_entry in route_entries]
            return (all(destination in route_destinations for destination in destinations), None)

        wait_for_result(_access_function)

    def check_route_entries_with_vrf(self, destinations, vrf_oids):
        def _access_function():
            route_entries = self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
            route_destination_vrf = [(json.loads(route_entry)["dest"], json.loads(route_entry)["vr"])
                                           for route_entry in route_entries]
            return (all((destination, vrf_oid) in route_destination_vrf
                        for destination, vrf_oid in zip(destinations, vrf_oids)), None)

        wait_for_result(_access_function)

    def check_route_entries_nexthop_ars(self):
        keys = self.asic_db.keys('ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP*')
        nhg_keys = [k for k in keys if 'SAI_OBJECT_TYPE_NEXT_HOP_GROUP:' in k]
    
        # Validate that at least one NEXT_HOP_GROUP exists
        assert len(nhg_keys) > 0, "No NEXT_HOP_GROUP objects found"

        for nhg_key in nhg_keys:
            attrs = self.asic_db.hgetall(nhg_key)

            # Validate mandatory attributes
            assert "SAI_NEXT_HOP_GROUP_ATTR_TYPE" in attrs, f"{nhg_key} missing TYPE attribute"
            assert "SAI_NEXT_HOP_GROUP_ATTR_ARS_OBJECT_ID" in attrs, f"{nhg_key} missing ARS_OBJECT_ID attribute"

            # Optional: check type value
            nhg_type = attrs.get("SAI_NEXT_HOP_GROUP_ATTR_TYPE")
            # Optional: print attributes for debugging
            print(f"{nhg_key} attributes: {attrs}")

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

    def create_ars_profile(self, dvs, profile_name="ars_profile_default"):
        self.setup_db(dvs)

        # Create a handle to the ARS profile table
        tbl = swsscommon.Table(self.config_db, CFG_ARS_PROFILE_TBL)

         # Define the ARS profile field-value pairs
        fvs = swsscommon.FieldValuePairs([
            ("algorithm", "ewma"),
            ("ars_nhg_path_selector_mode", "interface"),
            ("ars_lag_path_selector_mode", "interface"),
            ("default_ars_object", "ars_obj_1"),
            ("max_flows", "1000"),
        ])

        # Set the profile in CONFIG_DB
        tbl.set(profile_name, fvs)

    def check_ars_profile(self):
        # Get all ARS_PROFILE entries
        for _ in range(10):
            ars_keys = self.adb.get_keys('ASIC_STATE:SAI_OBJECT_TYPE_ARS_PROFILE')
            if ars_keys:
                break
            time.sleep(1)
        else:
            assert False, "No ARS_PROFILE objects found in ASIC DB"

    def create_ars_object(self, dvs, object_name="ars_obj_1"):
        self.setup_db(dvs)

        # Create a handle to the ARS object table
        tbl = swsscommon.Table(self.config_db, CFG_ARS_OBJECT_TBL)

        # Define ARS object field-value pairs
        fvs = swsscommon.FieldValuePairs([
            ("assign_mode", "PER_FLOWLET_QUALITY"),
            ("flowlet_idle_time", "10000"),
            ("max_flows", "256"),
            ("primary_path_threshold", "80"),
            ("alternative_path_cost", "20"),
        ])

        # Create the ARS object in CONFIG_DB
        tbl.set(object_name, fvs)

    def remove_ars_object(self, object_name="ars_obj_1"):
        tbl = swsscommon.Table(self.config_db, CFG_ARS_OBJECT_TBL)
        tbl._del(object_name)

    def check_ars_object(self):
        # Wait until ARS object is programmed
        for _ in range(10):
            ars_keys = self.adb.get_keys('ASIC_STATE:SAI_OBJECT_TYPE_ARS')
            if ars_keys:
                break
            time.sleep(1)
        else:
            assert False, "No ARS objects found in ASIC DB"

    def check_ars_interface(self, expected=True):
        # Wait until all port records are available
        asic_port_records = self.adb.wait_for_n_keys(
            "ASIC_STATE:SAI_OBJECT_TYPE_PORT",
            self.NUM_PORTS + 1  # +1 for CPU port
        )

        # Filter only the ports we care about
        ports = [port for port in asic_port_records if port in self.adb.port_name_map.values()]

        # Check ARS_ENABLE for each port using wait_for_field_match
        for port in ports:
            self.adb.wait_for_field_match(
                "ASIC_STATE:SAI_OBJECT_TYPE_PORT",  # table name
                port,                               # key (port OID)
                {"SAI_PORT_ATTR_ARS_ENABLE": "true"}
            )
    def dump_state_table(self, table_name):
        """Debug helper to dump entire STATE_DB table."""
        tbl = swsscommon.Table(self.state_db, table_name)
        keys = tbl.getKeys()
        print(f"--- Dump of STATE_DB:{table_name} ---")
        for k in keys:
            status, fvs = tbl.get(k)
            if status:
                print(f"{k}: {dict(fvs)}")
        print("--- End dump ---")

    def read_syslog(self, dvs):
        try:
            logs = subprocess.check_output(
                ["docker", "exec", self.dvs.dvsname, "cat", "/var/log/syslog"],
                text=True,
                timeout=10
            )
            return logs
        except Exception as e:
            pytest.fail(f"Failed to read syslog: {e}")
        return ""

    def check_syslog_for_invalid_profile(self, profile_name, logs):
        """Check syslog for invalid ARS profile log."""
        pattern = rf"Failed to create/set ARS profile {re.escape(profile_name)}"
        return bool(re.search(pattern, logs, re.IGNORECASE))

    def check_syslog_for_nexthop(self, nexthop_key, logs):
        """Check syslog for ARS nexthop creation log."""
        # nexthop_key format: "vrf|ip"
        ip = nexthop_key.split('|')[1]
        pattern = rf"doTaskArsNexthop: ARS NHG Op SET, VRF default, Nexthop {re.escape(ip)}"
        return bool(re.search(pattern, logs, re.IGNORECASE))

class TestArsOrch(TestArsBase):
    """ Functionality tests for ars """

    def test_ars_profile(self, dvs):
        """Validate ARS profile creation."""
        self.setup_db(dvs)
        # create ARS Profile
        self.create_ars_profile(dvs)
        # check ARS Profile
        self.check_ars_profile()

    def test_ars_object(self, dvs):
        """Validate ARS object creation."""
        self.setup_db(dvs)
        # create ARS Profile
        self.create_ars_profile(dvs)
        # create ARS object
        self.create_ars_object(dvs)
        # check ARS object
        self.check_ars_object()
        # remove ARS object
        self.remove_ars_object()


    def test_ars_interface(self, dvs):
        """Create ARS interface entry and validate in ASIC DB"""
        self.setup_db(dvs)
        # create ARS Profile
        self.create_ars_profile(dvs)
        # create ARS object
        self.create_ars_object(dvs)

        tbl = swsscommon.Table(self.config_db, CFG_ARS_INTERFACE_TBL)
        # Generate port list: Ethernet0, Ethernet4, ..., Ethernet32
        ports = [f"Ethernet{i}" for i in range(0, 125, 4)]

        for port in ports:
            fvs = swsscommon.FieldValuePairs([
                ("scaling_factor", "1"),
                ("ars_obj_name", "ars_obj_default"),
            ])
            tbl.set(port, fvs)
            time.sleep(0.1)
        self.check_ars_interface()
        for port in ports:
            tbl._del(port)
            time.sleep(0.1)

    def test_ars_nexthops(self, dvs):
        """Create ARS nexthops and validate orchagent logs in syslog."""
        self.setup_db(dvs)
        # create ARS Profile
        self.create_ars_profile(dvs)
        # create ARS object
        self.create_ars_object(dvs)

        tbl = swsscommon.Table(self.config_db, CFG_ARS_NEXTHOP_TBL)

        nexthops = [
            "default|1.1.1.10",
            "default|2.2.2.20",
            "default|3.3.3.30"
        ]
        #create ARS nexthop
        for key in nexthops:
            fvs = swsscommon.FieldValuePairs([("ars_obj_name", "ars_obj_1")])
            tbl.set(key, fvs)

        time.sleep(5)

        logs = self.read_syslog(dvs)

        for key in nexthops:
            assert self.check_syslog_for_nexthop(key, logs), \
                f"Orchagent did not log ARS nexthop creation for {key} in syslog"
        #delete ARS nexthop
        for key in nexthops:
            tbl._del(key)

    def test_ars_profile_invalid_config(self, dvs):
        """Test invalid ARS profile configuration."""
        self.setup_db(dvs)
        tbl = swsscommon.Table(self.config_db, CFG_ARS_PROFILE_TBL)
        fvs = swsscommon.FieldValuePairs([
            ("algorithm", "invalid_algo"),
        ])
        tbl.set("ars_profile_invalid", fvs)

        time.sleep(2)

        logs = self.read_syslog(dvs)
        assert self.check_syslog_for_invalid_profile("ars_profile_invalid", logs), \
            "Orchagent did not log 'Failed to create/set ARS profile in syslog"

    def test_ars_profile_global_mode(self, dvs):
        """Validate ARS global mode profile creation."""
        self.setup_db(dvs)
        tbl = swsscommon.Table(self.config_db, CFG_ARS_PROFILE_TBL)

        # Create a profile in INTERFACE mode
        fvs = swsscommon.FieldValuePairs([
            ("algorithm", "ewma"),
            ("ars_nhg_path_selector_mode", "global"),
            ("default_ars_object", "ars_obj_1"),
        ])
        tbl.set("ars_profile_default", fvs)

        # Create a object in INTERFACE mode
        tbl = swsscommon.Table(self.config_db, CFG_ARS_OBJECT_TBL)
        fvs = swsscommon.FieldValuePairs([
            ("assign_mode", "PER_FLOWLET_QUALITY"),
            ("flowlet_idle_time", "10000"),
            ("max_flows", "256"),
            ("primary_path_threshold", "80"),
            ("alternative_path_cost", "20"),
        ])
        tbl.set("ars_obj_1", fvs)

        time.sleep(2)

        # create l3 interface
        self.create_l3_intf("Ethernet0", "")
        self.create_l3_intf("Ethernet4", "")

        # set ip address
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        self.add_ip_address("Ethernet4", "20.0.0.2/31")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        dvs.servers[1].runcmd("ip address add 20.0.0.3/31 dev eth0")
        dvs.servers[1].runcmd("ip route add default via 20.0.0.2")
        time.sleep(2)

        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 2.2.2.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 2.2.2.0/24 20.0.0.3\"")

        # add route entry -- multiple nexthop
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 3.3.3.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 3.3.3.0/24 20.0.0.3\"")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "2.2.2.0/24")
        self.pdb.wait_for_entry("ROUTE_TABLE", "3.3.3.0/24")

        # check neighbor got resolved and removed from NEIGH_RESOLVE_TABLE
        self.pdb.wait_for_deleted_entry("NEIGH_RESOLVE_TABLE", "Ethernet0:10.0.0.1")
        self.pdb.wait_for_deleted_entry("NEIGH_RESOLVE_TABLE", "Ethernet4:20.0.0.3")

        # check ASIC route database
        self.check_route_entries(["2.2.2.0/24", "3.3.3.0/24"])
     
        # check ASIC ARS nexthop
        self.check_route_entries_nexthop_ars()
        # remove route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 2.2.2.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 2.2.2.0/24 20.0.0.3\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 3.3.3.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 3.3.3.0/24 20.0.0.3\"")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "2.2.2.0/24")
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "3.3.3.0/24")

        # check ASIC route database
        self.check_deleted_route_entries(["2.2.2.0/24", "3.3.3.0/24"])

        # remove ip address
        self.remove_ip_address("Ethernet0", "10.0.0.0/31")
        self.remove_ip_address("Ethernet4", "20.0.0.2/31")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")
        self.remove_l3_intf("Ethernet4")

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip route del default dev eth0")
        dvs.servers[0].runcmd("ip address del 10.0.0.1/31 dev eth0")

        dvs.servers[1].runcmd("ip route del default dev eth0")
        dvs.servers[1].runcmd("ip address del 20.0.0.3/31 dev eth0")

    def test_ars_profile_interface_mode(self, dvs):
        """Validate ARS interface mode profile creation."""
        self.setup_db(dvs)

        tbl = swsscommon.Table(self.config_db, CFG_ARS_PROFILE_TBL)

        # Create a profile in INTERFACE mode
        fvs = swsscommon.FieldValuePairs([
            ("algorithm", "ewma"),
            ("ars_nhg_path_selector_mode", "interface"),
            ("default_ars_object", "ars_obj_1"),
        ])
        tbl.set("ars_profile_default", fvs)

        # Create a object in INTERFACE mode
        tbl = swsscommon.Table(self.config_db, CFG_ARS_OBJECT_TBL)
        fvs = swsscommon.FieldValuePairs([
            ("assign_mode", "PER_FLOWLET_QUALITY"),
            ("flowlet_idle_time", "10000"),
            ("max_flows", "256"),
            ("primary_path_threshold", "80"),
            ("alternative_path_cost", "20"),
        ])
        tbl.set("ars_obj_1", fvs)

        # Create a interface in INTERFACE mode
        tbl = swsscommon.Table(self.config_db, CFG_ARS_INTERFACE_TBL)
        fvs = swsscommon.FieldValuePairs([
            ("scaling_factor", "1"),
            ("ars_obj_name", "ars_obj_default"),
        ])
        for intf in ["Ethernet0", "Ethernet4", "Ethernet8"]:
            tbl.set(intf, fvs)

        time.sleep(2)
        self.clear_srv_config(dvs)

        # create l3 interface
        self.create_l3_intf("Ethernet0", "")
        self.create_l3_intf("Ethernet4", "")

        # set ip address
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        self.add_ip_address("Ethernet4", "20.0.0.2/31")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        dvs.servers[1].runcmd("ip address add 20.0.0.3/31 dev eth0")
        dvs.servers[1].runcmd("ip route add default via 20.0.0.2")
        time.sleep(2)

        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 2.2.2.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 2.2.2.0/24 20.0.0.3\"")

        # add route entry -- multiple nexthop
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 3.3.3.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 3.3.3.0/24 20.0.0.3\"")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "2.2.2.0/24")
        self.pdb.wait_for_entry("ROUTE_TABLE", "3.3.3.0/24")

        # check neighbor got resolved and removed from NEIGH_RESOLVE_TABLE
        self.pdb.wait_for_deleted_entry("NEIGH_RESOLVE_TABLE", "Ethernet0:10.0.0.1")
        self.pdb.wait_for_deleted_entry("NEIGH_RESOLVE_TABLE", "Ethernet4:20.0.0.3")

        # check ASIC route database
        self.check_route_entries(["2.2.2.0/24", "3.3.3.0/24"])
     
        time.sleep(2)
        # check ASIC ARS nexthop
        self.check_route_entries_nexthop_ars()
        # remove route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 2.2.2.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 2.2.2.0/24 20.0.0.3\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 3.3.3.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 3.3.3.0/24 20.0.0.3\"")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "2.2.2.0/24")
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "3.3.3.0/24")

        # check ASIC route database
        self.check_deleted_route_entries(["2.2.2.0/24", "3.3.3.0/24"])

        # remove ip address
        self.remove_ip_address("Ethernet0", "10.0.0.0/31")
        self.remove_ip_address("Ethernet4", "20.0.0.2/31")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")
        self.remove_l3_intf("Ethernet4")

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip route del default dev eth0")
        dvs.servers[0].runcmd("ip address del 10.0.0.1/31 dev eth0")

        dvs.servers[1].runcmd("ip route del default dev eth0")
        dvs.servers[1].runcmd("ip address del 20.0.0.3/31 dev eth0")

    def test_ars_profile_nexthop_mode(self, dvs):
        """Validate ARS nexthop mode profile creation."""
        self.setup_db(dvs)

        tbl = swsscommon.Table(self.config_db, CFG_ARS_PROFILE_TBL)

        # Create a profile in GLOBAL mode
        fvs = swsscommon.FieldValuePairs([
            ("algorithm", "ewma"),
            ("ars_nhg_path_selector_mode", "nexthop"),
            ("default_ars_object", "ars_obj_1"),
        ])
        tbl.set("ars_profile_default", fvs)

        tbl = swsscommon.Table(self.config_db, CFG_ARS_OBJECT_TBL)
        fvs = swsscommon.FieldValuePairs([
            ("assign_mode", "PER_FLOWLET_QUALITY"),
            ("flowlet_idle_time", "10000"),
            ("max_flows", "256"),
            ("primary_path_threshold", "80"),
            ("alternative_path_cost", "20"),
        ])
        tbl.set("ars_obj_1", fvs)

        tbl = swsscommon.Table(self.config_db, CFG_ARS_INTERFACE_TBL)
        fvs = swsscommon.FieldValuePairs([
            ("scaling_factor", "1"),
        ])
        for intf in ["Ethernet0", "Ethernet4"]:
            tbl.set(intf, fvs)

        tbl = swsscommon.Table(self.config_db, CFG_ARS_NEXTHOP_TBL)

        nexthops = [
            "|10.0.0.1",
            "|20.0.0.3"
        ]

        for key in nexthops:
            fvs = swsscommon.FieldValuePairs([("ars_obj_name", "ars_obj_1")])
            tbl.set(key, fvs)

        time.sleep(2)  # let interfaces settle

        self.clear_srv_config(dvs)

        # create l3 interface
        self.create_l3_intf("Ethernet0", "")
        self.create_l3_intf("Ethernet4", "")

        # set ip address
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        self.add_ip_address("Ethernet4", "20.0.0.2/31")

        # bring up interface
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")

        # set ip address and default route
        dvs.servers[0].runcmd("ip address add 10.0.0.1/31 dev eth0")
        dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

        dvs.servers[1].runcmd("ip address add 20.0.0.3/31 dev eth0")
        dvs.servers[1].runcmd("ip route add default via 20.0.0.2")
        time.sleep(2)

        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 2.2.2.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 2.2.2.0/24 20.0.0.3\"")

        # add route entry -- multiple nexthop
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 3.3.3.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"ip route 3.3.3.0/24 20.0.0.3\"")

        # check application database
        self.pdb.wait_for_entry("ROUTE_TABLE", "2.2.2.0/24")
        self.pdb.wait_for_entry("ROUTE_TABLE", "3.3.3.0/24")

        # check neighbor got resolved and removed from NEIGH_RESOLVE_TABLE
        self.pdb.wait_for_deleted_entry("NEIGH_RESOLVE_TABLE", "Ethernet0:10.0.0.1")
        self.pdb.wait_for_deleted_entry("NEIGH_RESOLVE_TABLE", "Ethernet4:20.0.0.3")

        # check ASIC route database
        self.check_route_entries(["2.2.2.0/24", "3.3.3.0/24"])
     
        # check ASIC ARS nexthop
        self.check_route_entries_nexthop_ars()
        # remove route entry
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 2.2.2.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 2.2.2.0/24 20.0.0.3\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 3.3.3.0/24 10.0.0.1\"")
        dvs.runcmd("vtysh -c \"configure terminal\" -c \"no ip route 3.3.3.0/24 20.0.0.3\"")

        # check application database
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "2.2.2.0/24")
        self.pdb.wait_for_deleted_entry("ROUTE_TABLE", "3.3.3.0/24")

        # check ASIC route database
        self.check_deleted_route_entries(["2.2.2.0/24", "3.3.3.0/24"])

        # remove ip address
        self.remove_ip_address("Ethernet0", "10.0.0.0/31")
        self.remove_ip_address("Ethernet4", "20.0.0.2/31")

        # remove l3 interface
        self.remove_l3_intf("Ethernet0")
        self.remove_l3_intf("Ethernet4")

        self.set_admin_status("Ethernet0", "down")
        self.set_admin_status("Ethernet4", "down")

        # remove ip address and default route
        dvs.servers[0].runcmd("ip route del default dev eth0")
        dvs.servers[0].runcmd("ip address del 10.0.0.1/31 dev eth0")

        dvs.servers[1].runcmd("ip route del default dev eth0")
        dvs.servers[1].runcmd("ip address del 20.0.0.3/31 dev eth0")


