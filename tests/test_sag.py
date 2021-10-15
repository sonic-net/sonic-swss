import pytest
import time
import re
import json
import ipaddress
from swsscommon import swsscommon

@pytest.mark.usefixtures('testlog')
class TestSag(object):
    def setup_db(self, dvs):
        dvs.setup_db()
        self.app_db = dvs.get_app_db()
        self.cfg_db = dvs.get_config_db()
        self.asic_db = dvs.get_asic_db()

    def setup_interface(self, dvs, interface, vlan):
        # bring up interface
        dvs.set_interface_status(interface, "up")

        # create VLAN
        vlan_intf = "Vlan{}".format(vlan)
        dvs.create_vlan(vlan)
        dvs.create_vlan_member(vlan, interface)
        dvs.set_interface_status(vlan_intf, "up")

    def reset_interface(self, dvs, interface, vlan):
        # remove VLAN
        dvs.remove_vlan_member(vlan, interface)
        dvs.remove_vlan(vlan)

    def create_vrf(self, vrf_name):
        initial_entries = set(self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))
        self.cfg_db.create_entry(swsscommon.CFG_VRF_TABLE_NAME, vrf_name, {"empty": "empty"})
        time.sleep(1)

        current_entries = set(self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))
        assert len(current_entries - initial_entries) == 1
        return list(current_entries - initial_entries)[0]

    def remove_vrf(self, vrf_name):
        self.cfg_db.delete_entry(swsscommon.CFG_VRF_TABLE_NAME, vrf_name)
        time.sleep(1)

    def add_sag_mac(self, mac):
        self.cfg_db.create_entry(swsscommon.CFG_SAG_TABLE_NAME, "GLOBAL", {"gwmac": mac})
        time.sleep(1)

    def remove_sag_mac(self):
        self.cfg_db.delete_entry(swsscommon.CFG_SAG_TABLE_NAME, "GLOBAL")
        time.sleep(1)

    def enable_sag(self, vlan):
        vlan_intf = "Vlan{}".format(vlan)
        self.cfg_db.update_entry(swsscommon.CFG_VLAN_INTF_TABLE_NAME, vlan_intf, {"static_anycast_gateway": "true"})
        time.sleep(1)

    def disable_sag(self, vlan):
        vlan_intf = "Vlan{}".format(vlan)
        self.cfg_db.update_entry(swsscommon.CFG_VLAN_INTF_TABLE_NAME, vlan_intf, {"static_anycast_gateway": "false"})
        time.sleep(1)

    def get_system_mac(self, dvs):
        (exit_code, result) = dvs.runcmd(["sh", "-c", "ip link show eth0 | grep ether | awk '{print $2}'"])
        assert exit_code == 0
        return result.rstrip().lower()

    def get_asic_db_default_vrf_oid(self):
        vrf_entries = self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        assert len(vrf_entries) == 1
        return list(set(vrf_entries))[0]
        return set(vrf_entries)

    def generate_ipv6_link_local_addr(self, mac, prefix_len):
        eui64 = re.sub(r'[.:-]', '', mac).lower()
        eui64 = eui64[0:6] + 'fffe' + eui64[6:]
        eui64 = hex(int(eui64[0:2], 16) ^ 2)[2:].zfill(2) + eui64[2:]
        eui64_str = ':'.join(re.findall(r'.{4}', eui64))
        return ipaddress.IPv6Interface("fe80::{}/{}".format(eui64_str, str(prefix_len)))

    def check_kernel_intf_mac(self, dvs, interface, mac):
        (exit_code, result) = dvs.runcmd(["sh", "-c", "ip link show {}".format(interface)])
        assert exit_code == 0
        assert mac in result

    def check_kernel_intf_ipv6_addr(self, dvs, interface, addr):
        (exit_code, result) = dvs.runcmd(["sh", "-c", "ip -6 address show {}".format(interface)])
        assert exit_code == 0
        assert addr in result

    def check_app_db_sag_mac(self, fvs, mac):
        assert fvs.get("gwmac") == mac

    def check_app_db_intf(self, fvs, mac, sag):
        assert fvs.get("mac_addr") == mac and fvs.get("static_anycast_gateway") == sag

    def check_object_exist(self, db, table, expected_attributes):
        keys = db.get_keys(table)
        found = False
        for key in keys:
            fvs = db.get_entry(table, key)
            found |= all(fvs.get(k).casefold() == v.casefold() for k, v in expected_attributes.items())
            if found:
                break
        assert found, f"Expected field/value pairs not found: expcted={expected_attributes}"

    def check_asic_db_router_interface(self, vlan_oid, mac, vrf_oid):
        self.check_object_exist(self.asic_db,
            "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE",
            {
                "SAI_ROUTER_INTERFACE_ATTR_TYPE": "SAI_ROUTER_INTERFACE_TYPE_VLAN",
                "SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS": mac,
                "SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID": vrf_oid,
                "SAI_ROUTER_INTERFACE_ATTR_VLAN_ID": vlan_oid
            }
        )

    def check_asic_db_route_entry(self, destination, vrf_oid, exist):
        route_entries = self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY")
        dest_vrf = [(json.loads(route_entry)["dest"], json.loads(route_entry)["vr"])
                        for route_entry in route_entries]

        if exist:
            assert (destination, vrf_oid) in dest_vrf
        else:
            assert (destination, vrf_oid) not in dest_vrf


    def test_SagAddRemove(self, dvs):
        self.setup_db(dvs)

        default_mac = "00:00:00:00:00:00"
        default_vrf_oid = self.get_asic_db_default_vrf_oid()
        system_mac = self.get_system_mac(dvs)

        interface = "Ethernet0"
        vlan = "100"
        vlan_intf = "Vlan{}".format(vlan)
        self.setup_interface(dvs, interface, vlan)

        ip = "1.1.1.1/24"
        dvs.add_ip_address(vlan_intf, ip)

        # add SAG global MAC address
        mac = "00:11:22:33:44:55"
        self.add_sag_mac(mac)
        fvs = dvs.get_app_db().wait_for_entry(swsscommon.APP_SAG_TABLE_NAME, "GLOBAL")
        self.check_app_db_sag_mac(fvs, mac)

        ipv6_ll_route = self.generate_ipv6_link_local_addr(mac, 128)
        self.check_asic_db_route_entry(str(ipv6_ll_route), default_vrf_oid, exist=True)

        # enable SAG on the VLAN interface
        self.enable_sag(vlan)
        fvs = self.app_db.wait_for_field_match(
            swsscommon.APP_INTF_TABLE_NAME,
            vlan_intf,
            {"mac_addr": mac})

        self.check_app_db_intf(fvs, mac, "true")
        self.check_kernel_intf_mac(dvs, vlan_intf, mac)

        ipv6_ll = self.generate_ipv6_link_local_addr(mac, 64)
        self.check_kernel_intf_ipv6_addr(dvs, vlan_intf, str(ipv6_ll))
        self.check_asic_db_router_interface(dvs.getVlanOid(vlan), mac, default_vrf_oid)

        # disable SAG on the VLAN interface
        self.disable_sag(vlan)
        fvs = self.app_db.wait_for_field_match(
            swsscommon.APP_INTF_TABLE_NAME,
            vlan_intf,
            {"NULL": "NULL"}
        )

        self.check_app_db_intf(fvs, default_mac, "false")
        self.check_kernel_intf_mac(dvs, vlan_intf, system_mac)

        ipv6_ll = self.generate_ipv6_link_local_addr(system_mac, 64)
        self.check_kernel_intf_ipv6_addr(dvs, vlan_intf, str(ipv6_ll))
        self.check_asic_db_router_interface(dvs.getVlanOid(vlan), system_mac, default_vrf_oid)

        # delete SAG global MAC address
        self.remove_sag_mac()
        self.app_db.wait_for_deleted_entry(swsscommon.APP_SAG_TABLE_NAME, "GLOBAL")

        ipv6_ll_route = self.generate_ipv6_link_local_addr(mac, 128)
        self.check_asic_db_route_entry(str(ipv6_ll_route), default_vrf_oid, exist=False)

        # remove ip
        dvs.remove_ip_address(vlan_intf, ip)

        # reset interface
        self.reset_interface(dvs, interface, vlan)

    def test_SagAddRemoveInVrf(self, dvs):
        self.setup_db(dvs)

        default_mac = "00:00:00:00:00:00"
        default_vrf_oid = self.get_asic_db_default_vrf_oid()
        system_mac = self.get_system_mac(dvs)

        interface = "Ethernet0"
        vlan = "100"
        vlan_intf = "Vlan{}".format(vlan)
        self.setup_interface(dvs, interface, vlan)

        vrf_name = "Vrf1"
        vrf_oid = self.create_vrf(vrf_name)

        ip = "1.1.1.1/24"
        dvs.add_ip_address(vlan_intf, ip, vrf_name)

        # add SAG global MAC address
        mac = "00:11:22:33:44:55"
        self.add_sag_mac(mac)
        fvs = dvs.get_app_db().wait_for_entry(swsscommon.APP_SAG_TABLE_NAME, "GLOBAL")
        self.check_app_db_sag_mac(fvs, mac)

        ipv6_ll_route = self.generate_ipv6_link_local_addr(mac, 128)
        self.check_asic_db_route_entry(str(ipv6_ll_route), default_vrf_oid, exist=True)
        self.check_asic_db_route_entry(str(ipv6_ll_route), vrf_oid, exist=True)

        # enable SAG on the VLAN interface
        self.enable_sag(vlan)
        fvs = self.app_db.wait_for_field_match(
            swsscommon.APP_INTF_TABLE_NAME,
            vlan_intf,
            {"mac_addr": mac,
             "vrf_name": vrf_name})

        self.check_app_db_intf(fvs, mac, "true")
        self.check_kernel_intf_mac(dvs, vlan_intf, mac)

        ipv6_ll = self.generate_ipv6_link_local_addr(mac, 64)
        self.check_kernel_intf_ipv6_addr(dvs, vlan_intf, str(ipv6_ll))
        self.check_asic_db_router_interface(dvs.getVlanOid(vlan), mac, vrf_oid)

        # disable SAG on the VLAN interface
        self.disable_sag(vlan)
        fvs = self.app_db.wait_for_field_match(
            swsscommon.APP_INTF_TABLE_NAME,
            vlan_intf,
            {"vrf_name": vrf_name}
        )

        self.check_app_db_intf(fvs, default_mac, "false")
        self.check_kernel_intf_mac(dvs, vlan_intf, system_mac)

        ipv6_ll = self.generate_ipv6_link_local_addr(system_mac, 64)
        self.check_kernel_intf_ipv6_addr(dvs, vlan_intf, str(ipv6_ll))
        self.check_asic_db_router_interface(dvs.getVlanOid(vlan), system_mac, vrf_oid)

        # delete SAG global MAC address
        self.remove_sag_mac()
        self.app_db.wait_for_deleted_entry(swsscommon.APP_SAG_TABLE_NAME, "GLOBAL")

        ipv6_ll_route = self.generate_ipv6_link_local_addr(mac, 128)
        self.check_asic_db_route_entry(str(ipv6_ll_route), default_vrf_oid, exist=False)
        self.check_asic_db_route_entry(str(ipv6_ll_route), vrf_oid, exist=False)

        # remove ip
        dvs.remove_ip_address(vlan_intf, ip)

        # reset interface
        self.reset_interface(dvs, interface, vlan)
