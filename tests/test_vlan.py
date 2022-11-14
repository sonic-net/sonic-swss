import distro
import pytest

from distutils.version import StrictVersion
from dvslib.dvs_common import PollingConfig, wait_for_result

@pytest.mark.usefixtures("testlog")
@pytest.mark.usefixtures('dvs_vlan_manager')
@pytest.mark.usefixtures('dvs_lag_manager')
class TestVlan(object):

    def check_syslog(self, dvs, marker, process, err_log, vlan_str, expected_cnt):
        (_, num) = dvs.runcmd(
            ["sh",
             "-c",
             "awk '/{}/,ENDFILE {{print;}}' /var/log/syslog \
              | grep {} \
              | grep \"{}\" \
              | grep -i \"{}\" \
              | wc -l".format(marker, process, err_log, vlan_str)])

        assert num.strip() == str(expected_cnt)

    def test_VlanAddRemove(self, dvs):

        vlan = "2"
        interface = "Ethernet0"

        self.dvs_vlan.create_vlan(vlan)
        vlan_oid = self.dvs_vlan.get_and_verify_vlan_ids(1)[0]
        self.dvs_vlan.verify_vlan(vlan_oid, vlan)

        self.dvs_vlan.create_vlan_member(vlan, interface)
        self.dvs_vlan.verify_vlan_member(vlan_oid, interface)

        # Verify the physical port configuration
        member_port = self.dvs_vlan.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                                                      dvs.asic_db.port_name_map[interface])
        assert member_port.get("SAI_PORT_ATTR_PORT_VLAN_ID") == vlan

        # Verify the host interface configuration
        member_iface = self.dvs_vlan.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF",
                                              dvs.asic_db.hostif_name_map[interface])
        assert member_iface.get("SAI_HOSTIF_ATTR_VLAN_TAG") == "SAI_HOSTIF_VLAN_TAG_KEEP"

        self.dvs_vlan.remove_vlan_member(vlan, interface)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        self.dvs_vlan.remove_vlan(vlan)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

    def test_MultipleVlan(self, dvs):

        def _create_vlan_members(vlan, member_list):
            for member in member_list:
                self.dvs_vlan.create_vlan_member(vlan, member)

        def _remove_vlan_members(vlan, member_list):
            for member in member_list:
                self.dvs_vlan.remove_vlan_member(vlan, member)

        vlan1 = "18"
        vlan1_members = ["Ethernet0", "Ethernet4", "Ethernet8"]
        vlan2 = "188"
        vlan2_members = ["Ethernet20", "Ethernet24", "Ethernet28"]

        self.dvs_vlan.create_vlan(vlan1)
        _create_vlan_members(vlan1, vlan1_members)

        self.dvs_vlan.get_and_verify_vlan_ids(1)
        self.dvs_vlan.get_and_verify_vlan_member_ids(3)

        _remove_vlan_members(vlan1, vlan1_members)

        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        self.dvs_vlan.create_vlan(vlan2)
        _create_vlan_members(vlan2, vlan2_members)

        self.dvs_vlan.get_and_verify_vlan_ids(2)
        self.dvs_vlan.get_and_verify_vlan_member_ids(3)

        _create_vlan_members(vlan1, vlan1_members)

        self.dvs_vlan.get_and_verify_vlan_member_ids(6)

        _remove_vlan_members(vlan1, vlan1_members)

        self.dvs_vlan.get_and_verify_vlan_member_ids(3)

        _remove_vlan_members(vlan2, vlan2_members)

        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        # Member ports should have been detached from master bridge port
        for member in vlan2_members:
            _, output = dvs.runcmd(['sh', '-c', "ip link show {}".format(member)])
            assert "master" not in output

        self.dvs_vlan.remove_vlan(vlan1)
        self.dvs_vlan.get_and_verify_vlan_ids(1)

        self.dvs_vlan.remove_vlan(vlan2)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

    def test_VlanIncrementalConfig(self, dvs):

        # TODO: add_ip_address has a dependency on cdb within dvs,
        # so we still need to setup the db. This should be refactored.
        dvs.setup_db()

        vlan = "2"
        vlan_interface = "Vlan{}".format(vlan)
        interface = "Ethernet0"
        ip = "20.0.0.8/29"
        initial_mtu = "9100"
        new_mtu = "8888"

        self.dvs_vlan.create_vlan(vlan)

        vlan_oid = self.dvs_vlan.get_and_verify_vlan_ids(1)[0]
        self.dvs_vlan.verify_vlan(vlan_oid, vlan)

        self.dvs_vlan.create_vlan_member(vlan, interface)
        self.dvs_vlan.verify_vlan_member(vlan_oid, interface)

        dvs.add_ip_address(vlan_interface, ip)

        # Separate the VLAN interface from the Loopback interface
        vlan_rif = None
        intf_entries = self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", 2)
        for key in intf_entries:
            fvs = self.dvs_vlan.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", key)

            if fvs.get("SAI_ROUTER_INTERFACE_ATTR_TYPE") == "SAI_ROUTER_INTERFACE_TYPE_VLAN":
                assert fvs.get("SAI_ROUTER_INTERFACE_ATTR_MTU") == initial_mtu
                vlan_rif = key

        assert vlan_rif

        dvs.set_mtu(vlan_interface, new_mtu)
        self.dvs_vlan.asic_db.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE",
                                          vlan_rif,
                                          {"SAI_ROUTER_INTERFACE_ATTR_MTU": new_mtu})

        dvs.set_interface_status(vlan_interface, "down")
        self.dvs_vlan.app_db.wait_for_field_match("VLAN_TABLE", vlan_interface, {"admin_status": "down"})

        dvs.set_interface_status(vlan_interface, "up")
        self.dvs_vlan.app_db.wait_for_field_match("VLAN_TABLE", vlan_interface, {"admin_status": "up"})

        dvs.remove_ip_address(vlan_interface, ip)
        self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", 1)

        self.dvs_vlan.remove_vlan_member(vlan, interface)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        self.dvs_vlan.remove_vlan(vlan)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

    @pytest.mark.skipif(StrictVersion(distro.linux_distribution()[1]) <= StrictVersion('8.9'),
                        reason="Debian 8.9 or before has no support")
    @pytest.mark.parametrize("test_input, expected", [
        (["Vla", "2"], 0),
        (["VLAN", "3"], 0),
        (["vlan", "4"], 0),
        (["Vlan", "5"], 1),
    ])

    def test_AddVlanWithIncorrectKeyPrefix(self, dvs, test_input, expected):

        marker = dvs.add_log_marker()

        vlan_id = test_input[1]
        vlan = "{}{}".format(test_input[0], vlan_id)

        self.dvs_vlan.config_db.create_entry("VLAN", vlan, {"vlanid": vlan_id})
        vlan_entries = self.dvs_vlan.get_and_verify_vlan_ids(expected)

        if not vlan_entries:
            # If no VLAN is created, we should see the error in the logs
            # TODO: refactor to use loganalyzer
            self.check_syslog(dvs, marker, "vlanmgrd", "Invalid key format. No 'Vlan' prefix:", vlan, 1)
        else:
            self.dvs_vlan.remove_vlan(vlan_id)
            self.dvs_vlan.get_and_verify_vlan_ids(0)

    @pytest.mark.skipif(StrictVersion(distro.linux_distribution()[1]) <= StrictVersion('8.9'),
                        reason="Debian 8.9 or before has no support")
    @pytest.mark.parametrize("test_input, expected", [
        (["Vlan", "abc"], 0),
        (["Vlan", "a3"], 0),
        (["Vlan", ""], 0),
        (["Vlan", "5"], 1),
    ])
    def test_AddVlanWithIncorrectValueType(self, dvs, test_input, expected):

        marker = dvs.add_log_marker()

        vlan_id = test_input[1]
        vlan = "{}{}".format(test_input[0], vlan_id)

        self.dvs_vlan.config_db.create_entry("VLAN", vlan, {"vlanid": vlan_id})
        vlan_entries = self.dvs_vlan.get_and_verify_vlan_ids(expected)

        if not vlan_entries:
            # If no VLAN is created, we should see the error in the logs
            # TODO: refactor to use loganalyzer
            self.check_syslog(dvs, marker, "vlanmgrd",
                              "Invalid key format. Not a number after \'Vlan\' prefix:", vlan, 1)
        else:
            self.dvs_vlan.remove_vlan(vlan_id)
            self.dvs_vlan.get_and_verify_vlan_ids(0)

    def test_AddPortChannelToVlan(self, dvs):

        vlan = "2"
        lag_member = "Ethernet0"
        lag_id = "0001"
        lag_interface = "PortChannel{}".format(lag_id)

        self.dvs_lag.create_port_channel(lag_id)
        lag_entries = self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 1)

        self.dvs_lag.create_port_channel_member(lag_id, lag_member)

        # Verify the LAG has been initialized properly
        lag_member_entries = self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER", 1)
        fvs = self.dvs_vlan.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER", lag_member_entries[0])
        assert len(fvs) == 4
        assert fvs.get("SAI_LAG_MEMBER_ATTR_LAG_ID") == lag_entries[0]
        assert self.dvs_vlan.asic_db.port_to_id_map[fvs.get("SAI_LAG_MEMBER_ATTR_PORT_ID")] == lag_member

        self.dvs_vlan.create_vlan(vlan)
        self.dvs_vlan.get_and_verify_vlan_ids(1)

        self.dvs_vlan.create_vlan_member(vlan, lag_interface, "tagged")
        self.dvs_vlan.get_and_verify_vlan_member_ids(1)

        self.dvs_vlan.remove_vlan_member(vlan, lag_interface)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        self.dvs_vlan.remove_vlan(vlan)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

        self.dvs_lag.remove_port_channel_member(lag_id, lag_member)
        self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER", 0)

        self.dvs_lag.remove_port_channel(lag_id)
        self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 0)

    def test_AddVlanMemberWithNonExistVlan(self, dvs):

        vlan = "2"
        interface = "Ethernet0"

        self.dvs_vlan.create_vlan_member(vlan, interface)

        # Nothing should be created because there's no VLAN
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

        self.dvs_vlan.remove_vlan_member(vlan, interface)

    def test_RemoveNonexistentVlan(self, dvs):

        vlan = "2"

        self.dvs_vlan.get_and_verify_vlan_ids(0)

        self.dvs_vlan.remove_vlan(vlan)

        # Verify that we're still able to create the VLAN after "deleting" it
        self.dvs_vlan.create_vlan(vlan)
        self.dvs_vlan.get_and_verify_vlan_ids(1)

        self.dvs_vlan.remove_vlan(vlan)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

    @pytest.mark.skipif(StrictVersion(distro.linux_distribution()[1]) <= StrictVersion('8.9'),
                        reason="Debian 8.9 or before has no support")
    @pytest.mark.parametrize("test_input, expected", [
        (["tagging_mode", "untagged"], [1, "SAI_VLAN_TAGGING_MODE_UNTAGGED"]),
        (["tagging_mode", "tagged"], [1, "SAI_VLAN_TAGGING_MODE_TAGGED"]),
        (["tagging_mode", "priority_tagged"], [1, "SAI_VLAN_TAGGING_MODE_PRIORITY_TAGGED"]),
        (["tagging_mode", "unexpected_mode"], [0, ""]),
        (["no_tag_mode", ""], [1, "SAI_VLAN_TAGGING_MODE_UNTAGGED"]),
    ])

    def test_VlanMemberTaggingMode(self, dvs, test_input, expected):

        marker = dvs.add_log_marker()

        if test_input[0] == "no_tag_mode":
            tagging_mode = None
        else:
            tagging_mode = test_input[1]

        vlan = "2"
        interface = "Ethernet0"

        self.dvs_vlan.create_vlan(vlan)
        vlan_oid = self.dvs_vlan.get_and_verify_vlan_ids(1)[0]

        self.dvs_vlan.create_vlan_member(vlan, interface, tagging_mode)
        vlan_member_entries = self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER", expected[0])

        if len(vlan_member_entries) == 1:
            self.dvs_vlan.verify_vlan_member(vlan_oid, interface, expected[1])
        else:
            # If no VLAN is created, we should see the error in the logs
            # TODO: refactor to use loganalyzer
            self.check_syslog(dvs, marker, "vlanmgrd", "Wrong tagging_mode", test_input, 1)

        self.dvs_vlan.remove_vlan_member(vlan, interface)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        self.dvs_vlan.remove_vlan(vlan)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

    @pytest.mark.skip(reason="AddMaxVlan takes too long to execute")
    def test_AddMaxVlan(self, dvs):

        max_poll = PollingConfig(polling_interval=3, timeout=300, strict=True)

        min_vid = 2
        max_vid = 4094

        for vlan in range(min_vid, max_vid + 1):
            self.dvs_vlan.create_vlan(str(vlan))

        self.dvs_vlan.get_and_verify_vlan_ids(max_vid - 1, polling_config=max_poll)

        for vlan in range(min_vid, max_vid + 1):
            self.dvs_vlan.remove_vlan(str(vlan))

        self.dvs_vlan.get_and_verify_vlan_ids(0, polling_config=max_poll)

    def test_RemoveVlanWithRouterInterface(self, dvs):
        # TODO: add_ip_address has a dependency on cdb within dvs,
        # so we still need to setup the db. This should be refactored.
        dvs.setup_db()

        vlan = "100"
        vlan_interface = "Vlan{}".format(vlan)
        ip = "20.0.0.8/29"

        self.dvs_vlan.create_vlan(vlan)
        vlan_oid = self.dvs_vlan.get_and_verify_vlan_ids(1)[0]
        self.dvs_vlan.verify_vlan(vlan_oid, vlan)

        dvs.add_ip_address(vlan_interface, ip)

        # Should see 1 VLAN interface and 1 Loopback interface
        self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", 2)

        self.dvs_vlan.remove_vlan(vlan)

        # VLAN should still be preserved since the RIF depends on it
        vlan_oid = self.dvs_vlan.get_and_verify_vlan_ids(1)[0]
        self.dvs_vlan.verify_vlan(vlan_oid, vlan)

        dvs.remove_ip_address(vlan_interface, ip)
        self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", 1)

        self.dvs_vlan.remove_vlan(vlan)

        self.dvs_vlan.get_and_verify_vlan_ids(0)

    def test_VlanDbData(self, dvs):
        vlan = "2"

        self.dvs_vlan.create_vlan(vlan)

        vlan_oid = self.dvs_vlan.app_db.wait_for_n_keys("VLAN_TABLE", 1)[0]
        fvs = self.dvs_vlan.app_db.wait_for_entry("VLAN_TABLE", vlan_oid)
        self.dvs_vlan.check_app_db_vlan_fields(fvs)

        vlan_oid = self.dvs_vlan.state_db.wait_for_n_keys("VLAN_TABLE", 1)[0]
        fvs = self.dvs_vlan.state_db.wait_for_entry("VLAN_TABLE", vlan_oid)
        self.dvs_vlan.check_state_db_vlan_fields(fvs)

        vlan_oid = self.dvs_vlan.get_and_verify_vlan_ids(1)[0]
        self.dvs_vlan.verify_vlan(vlan_oid, vlan)

        self.dvs_vlan.remove_vlan(vlan)

    @pytest.mark.skipif(StrictVersion(distro.linux_distribution()[1]) <= StrictVersion('8.9'),
                        reason="Debian 8.9 or before has no support")
    @pytest.mark.parametrize("test_input, expected", [
        (["untagged"], ["SAI_VLAN_TAGGING_MODE_UNTAGGED"]),
        (["tagged"], ["SAI_VLAN_TAGGING_MODE_TAGGED"]),
        (["priority_tagged"], ["SAI_VLAN_TAGGING_MODE_PRIORITY_TAGGED"]),
    ])
    def test_VlanMemberDbData(self, dvs, test_input, expected):

        vlan = "2"
        interface = "Ethernet0"
        tagging_mode = test_input[0]

        self.dvs_vlan.create_vlan(vlan)

        self.dvs_vlan.create_vlan_member(vlan, interface, tagging_mode)

        vlan_oid = self.dvs_vlan.app_db.wait_for_n_keys("VLAN_MEMBER_TABLE", 1)[0]
        fvs = self.dvs_vlan.app_db.wait_for_entry("VLAN_MEMBER_TABLE", vlan_oid)
        self.dvs_vlan.check_app_db_vlan_member_fields(fvs, tagging_mode)

        vlan_oid = self.dvs_vlan.state_db.wait_for_n_keys("VLAN_MEMBER_TABLE", 1)[0]
        fvs = self.dvs_vlan.state_db.wait_for_entry("VLAN_MEMBER_TABLE", vlan_oid)
        self.dvs_vlan.check_state_db_vlan_member_fields(fvs)

        vlan_oid = self.dvs_vlan.get_and_verify_vlan_ids(1)[0]
        self.dvs_vlan.verify_vlan_member(vlan_oid, interface, expected[0])

        self.dvs_vlan.remove_vlan_member(vlan, interface)
        self.dvs_vlan.get_and_verify_vlan_member_ids(0)

        self.dvs_vlan.remove_vlan(vlan)
        self.dvs_vlan.get_and_verify_vlan_ids(0)

    def test_VlanHostIf(self, dvs):

        vlan = "2"
        hostif_name = "MonVlan2"

        self.dvs_vlan.create_vlan(vlan)
        vlan_oid = self.dvs_vlan.get_and_verify_vlan_ids(1)[0]
        self.dvs_vlan.verify_vlan(vlan_oid, vlan)

        self.dvs_vlan.create_vlan_hostif(vlan, hostif_name)
        hostif_oid = self.dvs_vlan.get_and_verify_vlan_hostif_ids(len(dvs.asic_db.hostif_name_map))
        self.dvs_vlan.verify_vlan_hostif(hostif_name, hostif_oid, vlan_oid)

        self.dvs_vlan.remove_vlan(vlan)
        self.dvs_vlan.get_and_verify_vlan_ids(0)
        self.dvs_vlan.get_and_verify_vlan_hostif_ids(len(dvs.asic_db.hostif_name_map) - 1)

    def test_VlanGratArp(self, dvs):
        def arp_accept_enabled():
            rc, res = dvs.runcmd("cat /proc/sys/net/ipv4/conf/Vlan{}/arp_accept".format(vlan))
            return (res.strip("\n") == "1", res)

        def arp_accept_disabled():
            rc, res = dvs.runcmd("cat /proc/sys/net/ipv4/conf/Vlan{}/arp_accept".format(vlan))
            return (res.strip("\n") == "0", res)

        vlan = "2"
        self.dvs_vlan.create_vlan(vlan)
        self.dvs_vlan.create_vlan_interface(vlan)
        self.dvs_vlan.set_vlan_intf_property(vlan, "grat_arp", "enabled")

        wait_for_result(arp_accept_enabled, PollingConfig(), "IPv4 arp_accept not enabled")

        # Not currently possible to test `accept_untracked_na` as it doesn't exist in the kernel for
        # our test VMs (only present in kernels 5.19 and above)

        self.dvs_vlan.set_vlan_intf_property(vlan, "grat_arp", "disabled")

        wait_for_result(arp_accept_disabled, PollingConfig(), "IPv4 arp_accept not disabled")

        self.dvs_vlan.remove_vlan(vlan)

    def test_VlanProxyArp(self, dvs): 

        def proxy_arp_enabled():
            rc, proxy_arp_res = dvs.runcmd("cat /proc/sys/net/ipv4/conf/Vlan{}/proxy_arp".format(vlan))
            rc, pvlan_res = dvs.runcmd("cat /proc/sys/net/ipv4/conf/Vlan{}/proxy_arp_pvlan".format(vlan))

            return (proxy_arp_res.strip("\n") == "1" and pvlan_res.strip("\n") == "1", (proxy_arp_res, pvlan_res))

        def proxy_arp_disabled():
            rc, proxy_arp_res = dvs.runcmd("cat /proc/sys/net/ipv4/conf/Vlan{}/proxy_arp".format(vlan))
            rc, pvlan_res = dvs.runcmd("cat /proc/sys/net/ipv4/conf/Vlan{}/proxy_arp_pvlan".format(vlan))

            return (proxy_arp_res.strip("\n") == "0" and pvlan_res.strip("\n") == "0", (proxy_arp_res, pvlan_res))
            
        vlan = "2"
        self.dvs_vlan.create_vlan(vlan)
        self.dvs_vlan.create_vlan_interface(vlan)
        self.dvs_vlan.set_vlan_intf_property(vlan, "proxy_arp", "enabled")

        wait_for_result(proxy_arp_enabled, PollingConfig(), 'IPv4 proxy_arp or proxy_arp_pvlan not enabled')

        self.dvs_vlan.set_vlan_intf_property(vlan, "proxy_arp", "disabled")

        wait_for_result(proxy_arp_disabled, PollingConfig(), 'IPv4 proxy_arp or proxy_arp_pvlan not disabled')

        self.dvs_vlan.remove_vlan(vlan)

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
