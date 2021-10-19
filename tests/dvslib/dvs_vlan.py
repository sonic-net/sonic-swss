from .dvs_database import DVSDatabase

class DVSVlan(object):
    def __init__(self, adb, cdb, sdb, cntrdb, appdb):
        self.asic_db = adb
        self.config_db = cdb
        self.state_db = sdb
        self.counters_db = cntrdb
        self.app_db = appdb

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
        #TBD: port_to_id_map may NOT be updated one in case port is deleted and re-created.
        #     Hence the map needs refreshed. Need to think trough and decide when and where
        #     to do it.
        assert self.asic_db.port_to_id_map[bridge_port["SAI_BRIDGE_PORT_ATTR_PORT_ID"]] == expected_iface
        return bridge_port_id

