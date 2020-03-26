import platform
import pytest

from distutils.version import StrictVersion
from dvslib.dvs_common import PollingConfig
from dvslib.dvs_database import DVSDatabase

@pytest.mark.usefixtures("testlog")
class TestVlan(object):
    def setup_db(self, dvs):
        self.app_db = dvs.get_app_db()
        self.asic_db = dvs.get_asic_db()
        self.config_db = dvs.get_config_db()
        self.state_db = dvs.get_state_db()

    def create_vlan(self, vlan):
        vlan = "Vlan{}".format(vlan)
        vlan_entry = {"vlanid": vlan}
        self.config_db.create_entry("VLAN", vlan, vlan_entry)

    def remove_vlan(self, vlan):
        vlan = "Vlan{}".format(vlan)
        self.config_db.delete_entry("VLAN", vlan)

    def create_vlan_member(self, vlan, interface, tagging_mode="untagged"):
        member = "Vlan{}|{}".format(vlan, interface)
        if tagging_mode:
            member_entry = {"tagging_mode": tagging_mode}
        else:
            member_entry = {"no_tag_mode": ""}

        self.config_db.create_entry("VLAN_MEMBER", member, member_entry)

    def remove_vlan_member(self, vlan, interface):
        member = "Vlan{}|{}".format(vlan, interface)
        self.config_db.delete_entry("VLAN_MEMBER", member)

    def create_port_channel(self, lag_id, admin_status="up", mtu="1500"):
        lag = "PortChannel{}".format(lag_id)
        lag_entry = {"admin_status": admin_status, "mtu": mtu}
        self.config_db.create_entry("PORTCHANNEL", lag, lag_entry)

    def remove_port_channel(self, lag_id):
        lag = "PortChannel{}".format(lag_id)
        self.config_db.delete_entry("PORTCHANNEL", lag)

    def create_port_channel_member(self, lag_id, interface):
        member = "PortChannel{}|{}".format(lag_id, interface)
        member_entry = {"NULL": "NULL"}
        self.config_db.create_entry("PORTCHANNEL_MEMBER", member, member_entry)

    def remove_port_channel_member(self, lag_id, interface):
        member = "PortChannel{}|{}".format(lag_id, interface)
        self.config_db.delete_entry("PORTCHANNEL_MEMBER", member)

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

    def check_app_db_vlan_fields(self, fvs, admin_status="up", mtu="9100"):
        assert fvs.get("admin_status") == admin_status
        assert fvs.get("mtu") == mtu

    def check_app_db_vlan_member_fields(self, fvs, tagging_mode="untagged"):
        assert fvs.get("tagging_mode") == tagging_mode

    def check_state_db_vlan_fields(self, fvs, state="ok"):
        assert fvs.get("state") == state

    def check_state_db_vlan_member_fields(self, fvs, state="ok"):
        assert fvs.get("state") == state

    def verify_vlan(self, vlan_oid, vlan_id):
        vlan = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_VLAN", vlan_oid)
        assert vlan.get("SAI_VLAN_ATTR_VLAN_ID") == vlan_id

    def get_and_verify_vlan_ids(self,
                                expected_num,
                                polling_config=DVSDatabase.DEFAULT_POLLING_CONFIG):
        vlan_entries = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VLAN",
                                                    expected_num + 1,
                                                    polling_config)
        return [v for v in vlan_entries if v != self.asic_db.default_vlan_id]

    def verify_vlan_member(self, vlan_oid, iface, tagging_mode="SAI_VLAN_TAGGING_MODE_UNTAGGED"):
        member_ids = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER", 1)
        member = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER", member_ids[0])
        assert member == {"SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE": tagging_mode,
                          "SAI_VLAN_MEMBER_ATTR_VLAN_ID": vlan_oid,
                          "SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID": self.get_bridge_port_id(iface)}

    def get_and_verify_vlan_member_ids(self, expected_num):
        return self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER", expected_num)

    def get_bridge_port_id(self, expected_iface):
        bridge_port_id = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT", 1)[0]
        bridge_port = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT", bridge_port_id)
        assert self.asic_db.port_to_id_map[bridge_port["SAI_BRIDGE_PORT_ATTR_PORT_ID"]] == expected_iface
        return bridge_port_id

    def test_VlanAddRemove(self, dvs):
        self.setup_db(dvs)

        vlan = "2"
        interface = "Ethernet0"

        self.create_vlan(vlan)
        vlan_oid = self.get_and_verify_vlan_ids(1)[0]
        self.verify_vlan(vlan_oid, vlan)

        self.create_vlan_member(vlan, interface)
        self.verify_vlan_member(vlan_oid, interface)

        # Verify the physical port configuration
        member_port = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                                             self.asic_db.port_name_map[interface])
        assert member_port.get("SAI_PORT_ATTR_PORT_VLAN_ID") == vlan

        # Verify the host interface configuration
        member_iface = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF",
                                              self.asic_db.hostif_name_map[interface])
        assert member_iface.get("SAI_HOSTIF_ATTR_VLAN_TAG") == "SAI_HOSTIF_VLAN_TAG_KEEP"

        self.remove_vlan_member(vlan, interface)
        self.get_and_verify_vlan_member_ids(0)

        self.remove_vlan(vlan)
        self.get_and_verify_vlan_ids(0)

    def test_MultipleVlan(self, dvs):
        self.setup_db(dvs)

        def _create_vlan_members(vlan, member_list):
            for member in member_list:
                self.create_vlan_member(vlan, member)

        def _remove_vlan_members(vlan, member_list):
            for member in member_list:
                self.remove_vlan_member(vlan, member)

        vlan1 = "18"
        vlan1_members = ["Ethernet0", "Ethernet4", "Ethernet8"]
        vlan2 = "188"
        vlan2_members = ["Ethernet20", "Ethernet24", "Ethernet28"]

        self.create_vlan(vlan1)
        _create_vlan_members(vlan1, vlan1_members)

        self.get_and_verify_vlan_ids(1)
        self.get_and_verify_vlan_member_ids(3)

        _remove_vlan_members(vlan1, vlan1_members)

        self.get_and_verify_vlan_member_ids(0)

        self.create_vlan(vlan2)
        _create_vlan_members(vlan2, vlan2_members)

        self.get_and_verify_vlan_ids(2)
        self.get_and_verify_vlan_member_ids(3)

        _create_vlan_members(vlan1, vlan1_members)

        self.get_and_verify_vlan_member_ids(6)

        _remove_vlan_members(vlan1, vlan1_members)

        self.get_and_verify_vlan_member_ids(3)

        _remove_vlan_members(vlan2, vlan2_members)

        self.get_and_verify_vlan_member_ids(0)

        # Member ports should have been detached from master bridge port
        for member in vlan2_members:
            exitcode, _ = dvs.runcmd(['sh', '-c', "ip link show {} | grep -w master".format(member)])
            assert not exitcode

        self.remove_vlan(vlan1)
        self.get_and_verify_vlan_ids(1)

        self.remove_vlan(vlan2)
        self.get_and_verify_vlan_ids(0)

    def test_VlanIncrementalConfig(self, dvs):
        # TODO: add_ip_address has a dependency on cdb within dvs,
        # so we still need to setup the db. This should be refactored.
        dvs.setup_db()
        self.setup_db(dvs)

        vlan = "2"
        vlan_interface = "Vlan{}".format(vlan)
        interface = "Ethernet0"
        ip = "20.0.0.8/29"
        initial_mtu = "9100"
        new_mtu = "8888"

        self.create_vlan(vlan)

        vlan_oid = self.get_and_verify_vlan_ids(1)[0]
        self.verify_vlan(vlan_oid, vlan)

        self.create_vlan_member(vlan, interface)
        self.verify_vlan_member(vlan_oid, interface)

        dvs.add_ip_address(vlan_interface, ip)

        # Separate the VLAN interface from the Loopback interface
        vlan_rif = None
        intf_entries = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", 2)
        for key in intf_entries:
            fvs = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", key)

            if fvs.get("SAI_ROUTER_INTERFACE_ATTR_TYPE") == "SAI_ROUTER_INTERFACE_TYPE_VLAN":
                assert fvs.get("SAI_ROUTER_INTERFACE_ATTR_MTU") == initial_mtu
                vlan_rif = key

        assert vlan_rif

        dvs.set_mtu(vlan_interface, new_mtu)
        self.asic_db.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE",
                                          vlan_rif,
                                          {"SAI_ROUTER_INTERFACE_ATTR_MTU": new_mtu})

        dvs.set_interface_status(vlan_interface, "down")
        self.app_db.wait_for_field_match("VLAN_TABLE", vlan_interface, {"admin_status": "down"})

        dvs.set_interface_status(vlan_interface, "up")
        self.app_db.wait_for_field_match("VLAN_TABLE", vlan_interface, {"admin_status": "up"})

        dvs.remove_ip_address(vlan_interface, ip)
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", 1)

        self.remove_vlan_member(vlan, interface)
        self.get_and_verify_vlan_member_ids(0)

        self.remove_vlan(vlan)
        self.get_and_verify_vlan_ids(0)

    @pytest.mark.skipif(StrictVersion(platform.linux_distribution()[1]) <= StrictVersion('8.9'),
                        reason="Debian 8.9 or before has no support")
    @pytest.mark.parametrize("test_input, expected", [
        (["Vla", "2"], 0),
        (["VLAN", "3"], 0),
        (["vlan", "4"], 0),
        (["Vlan", "5"], 1),
    ])
    def test_AddVlanWithIncorrectKeyPrefix(self, dvs, test_input, expected):
        self.setup_db(dvs)
        marker = dvs.add_log_marker()

        vlan_id = test_input[1]
        vlan = "{}{}".format(test_input[0], vlan_id)

        self.config_db.create_entry("VLAN", vlan, {"vlanid": vlan_id})
        vlan_entries = self.get_and_verify_vlan_ids(expected)

        if not vlan_entries:
            # If no VLAN is created, we should see the error in the logs
            # TODO: refactor to use loganalyzer
            self.check_syslog(dvs, marker, "vlanmgrd", "Invalid key format. No 'Vlan' prefix:", vlan, 1)
        else:
            self.remove_vlan(vlan_id)
            self.get_and_verify_vlan_ids(0)

    @pytest.mark.skipif(StrictVersion(platform.linux_distribution()[1]) <= StrictVersion('8.9'),
                        reason="Debian 8.9 or before has no support")
    @pytest.mark.parametrize("test_input, expected", [
        (["Vlan", "abc"], 0),
        (["Vlan", "a3"], 0),
        (["Vlan", ""], 0),
        (["Vlan", "5"], 1),
    ])
    def test_AddVlanWithIncorrectValueType(self, dvs, test_input, expected):
        self.setup_db(dvs)
        marker = dvs.add_log_marker()

        vlan_id = test_input[1]
        vlan = "{}{}".format(test_input[0], vlan_id)

        self.config_db.create_entry("VLAN", vlan, {"vlanid": vlan_id})
        vlan_entries = self.get_and_verify_vlan_ids(expected)

        if not vlan_entries:
            # If no VLAN is created, we should see the error in the logs
            # TODO: refactor to use loganalyzer
            self.check_syslog(dvs, marker, "vlanmgrd",
                              "Invalid key format. Not a number after \'Vlan\' prefix:", vlan, 1)
        else:
            self.remove_vlan(vlan_id)
            self.get_and_verify_vlan_ids(0)

    def test_AddPortChannelToVlan(self, dvs):
        self.setup_db(dvs)

        vlan = "2"
        lag_member = "Ethernet0"
        lag_id = "0001"
        lag_interface = "PortChannel{}".format(lag_id)

        self.create_port_channel(lag_id)
        lag_entries = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 1)

        self.create_port_channel_member(lag_id, lag_member)

        # Verify the LAG has been initialized properly
        lag_member_entries = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER", 1)
        fvs = self.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER", lag_member_entries[0])
        assert len(fvs) == 4
        assert fvs.get("SAI_LAG_MEMBER_ATTR_LAG_ID") == lag_entries[0]
        assert self.asic_db.port_to_id_map[fvs.get("SAI_LAG_MEMBER_ATTR_PORT_ID")] == lag_member

        self.create_vlan(vlan)
        self.get_and_verify_vlan_ids(1)

        self.create_vlan_member(vlan, lag_interface, "tagged")
        self.get_and_verify_vlan_member_ids(1)

        self.remove_vlan_member(vlan, lag_interface)
        self.get_and_verify_vlan_member_ids(0)

        self.remove_vlan(vlan)
        self.get_and_verify_vlan_ids(0)

        self.remove_port_channel_member(lag_id, lag_member)
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER", 0)

        self.remove_port_channel(lag_id)
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 0)

    def test_AddVlanMemberWithNonExistVlan(self, dvs):
        self.setup_db(dvs)

        vlan = "2"
        interface = "Ethernet0"

        self.create_vlan_member(vlan, interface)

        # Nothing should be created because there's no VLAN
        self.get_and_verify_vlan_member_ids(0)
        self.get_and_verify_vlan_ids(0)

        self.remove_vlan_member(vlan, interface)

    def test_RemoveNonexistentVlan(self, dvs):
        self.setup_db(dvs)

        vlan = "2"

        self.get_and_verify_vlan_ids(0)

        self.remove_vlan(vlan)

        # Verify that we're still able to create the VLAN after "deleting" it
        self.create_vlan(vlan)
        self.get_and_verify_vlan_ids(1)

        self.remove_vlan(vlan)
        self.get_and_verify_vlan_ids(0)

    @pytest.mark.skipif(StrictVersion(platform.linux_distribution()[1]) <= StrictVersion('8.9'),
                        reason="Debian 8.9 or before has no support")
    @pytest.mark.parametrize("test_input, expected", [
        (["tagging_mode", "untagged"], [1, "SAI_VLAN_TAGGING_MODE_UNTAGGED"]),
        (["tagging_mode", "tagged"], [1, "SAI_VLAN_TAGGING_MODE_TAGGED"]),
        (["tagging_mode", "priority_tagged"], [1, "SAI_VLAN_TAGGING_MODE_PRIORITY_TAGGED"]),
        (["tagging_mode", "unexpected_mode"], [0, ""]),
        (["no_tag_mode", ""], [1, "SAI_VLAN_TAGGING_MODE_UNTAGGED"]),
    ])
    def test_VlanMemberTaggingMode(self, dvs, test_input, expected):
        self.setup_db(dvs)
        marker = dvs.add_log_marker()

        if test_input[0] == "no_tag_mode":
            tagging_mode = None
        else:
            tagging_mode = test_input[1]

        vlan = "2"
        interface = "Ethernet0"

        self.create_vlan(vlan)
        vlan_oid = self.get_and_verify_vlan_ids(1)[0]

        self.create_vlan_member(vlan, interface, tagging_mode)
        vlan_member_entries = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER", expected[0])

        if len(vlan_member_entries) == 1:
            self.verify_vlan_member(vlan_oid, interface, expected[1])
        else:
            # If no VLAN is created, we should see the error in the logs
            # TODO: refactor to use loganalyzer
            self.check_syslog(dvs, marker, "vlanmgrd", "Wrong tagging_mode", test_input, 1)

        self.remove_vlan_member(vlan, interface)
        self.get_and_verify_vlan_member_ids(0)

        self.remove_vlan(vlan)
        self.get_and_verify_vlan_ids(0)

    @pytest.mark.skip(reason="AddMaxVlan takes too long to execute")
    def test_AddMaxVlan(self, dvs):
        self.setup_db(dvs)
        max_poll = PollingConfig(polling_interval=3, timeout=300, strict=True)

        min_vid = 2
        max_vid = 4094

        for vlan in range(min_vid, max_vid + 1):
            self.create_vlan(str(vlan))

        self.get_and_verify_vlan_ids(max_vid - 1, polling_config=max_poll)

        for vlan in range(min_vid, max_vid + 1):
            self.remove_vlan(str(vlan))

        self.get_and_verify_vlan_ids(0, polling_config=max_poll)

    def test_RemoveVlanWithRouterInterface(self, dvs):
        # TODO: add_ip_address has a dependency on cdb within dvs,
        # so we still need to setup the db. This should be refactored.
        dvs.setup_db()
        self.setup_db(dvs)

        vlan = "100"
        vlan_interface = "Vlan{}".format(vlan)
        ip = "20.0.0.8/29"

        self.create_vlan(vlan)
        vlan_oid = self.get_and_verify_vlan_ids(1)[0]
        self.verify_vlan(vlan_oid, vlan)

        dvs.add_ip_address(vlan_interface, ip)

        # Should see 1 VLAN interface and 1 Loopback interface
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", 2)

        self.remove_vlan(vlan)

        # VLAN should still be preserved since the RIF depends on it
        vlan_oid = self.get_and_verify_vlan_ids(1)[0]
        self.verify_vlan(vlan_oid, vlan)

        dvs.remove_ip_address(vlan_interface, ip)
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", 1)

        self.remove_vlan(vlan)

        self.get_and_verify_vlan_ids(0)

    def test_VlanDbData(self, dvs):
        self.setup_db(dvs)
        vlan = "2"

        self.create_vlan(vlan)

        vlan_oid = self.app_db.wait_for_n_keys("VLAN_TABLE", 1)[0]
        fvs = self.app_db.wait_for_entry("VLAN_TABLE", vlan_oid)
        self.check_app_db_vlan_fields(fvs)

        vlan_oid = self.state_db.wait_for_n_keys("VLAN_TABLE", 1)[0]
        fvs = self.state_db.wait_for_entry("VLAN_TABLE", vlan_oid)
        self.check_state_db_vlan_fields(fvs)

        vlan_oid = self.get_and_verify_vlan_ids(1)[0]
        self.verify_vlan(vlan_oid, vlan)

        self.remove_vlan(vlan)

    @pytest.mark.skipif(StrictVersion(platform.linux_distribution()[1]) <= StrictVersion('8.9'),
                        reason="Debian 8.9 or before has no support")
    @pytest.mark.parametrize("test_input, expected", [
        (["untagged"], ["SAI_VLAN_TAGGING_MODE_UNTAGGED"]),
        (["tagged"], ["SAI_VLAN_TAGGING_MODE_TAGGED"]),
        (["priority_tagged"], ["SAI_VLAN_TAGGING_MODE_PRIORITY_TAGGED"]),
    ])
    def test_VlanMemberDbData(self, dvs, test_input, expected):
        self.setup_db(dvs)

        vlan = "2"
        interface = "Ethernet0"
        tagging_mode = test_input[0]

        self.create_vlan(vlan)

        self.create_vlan_member(vlan, interface, tagging_mode)

        vlan_oid = self.app_db.wait_for_n_keys("VLAN_MEMBER_TABLE", 1)[0]
        fvs = self.app_db.wait_for_entry("VLAN_MEMBER_TABLE", vlan_oid)
        self.check_app_db_vlan_member_fields(fvs, tagging_mode)

        vlan_oid = self.state_db.wait_for_n_keys("VLAN_MEMBER_TABLE", 1)[0]
        fvs = self.state_db.wait_for_entry("VLAN_MEMBER_TABLE", vlan_oid)
        self.check_state_db_vlan_member_fields(fvs)

        vlan_oid = self.get_and_verify_vlan_ids(1)[0]
        self.verify_vlan_member(vlan_oid, interface, expected[0])

        self.remove_vlan_member(vlan, interface)
        self.get_and_verify_vlan_member_ids(0)

        self.remove_vlan(vlan)
        self.get_and_verify_vlan_ids(0)
