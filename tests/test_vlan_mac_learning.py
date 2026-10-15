class TestVlanMacLearning(object):
    def test_LearnDisableReachesAsicDb(self, dvs, testlog):
        config_db = dvs.get_config_db()
        asic_db = dvs.get_asic_db()

        # Create Vlan100 with learning disabled
        config_db.create_entry("VLAN", "Vlan100",
                               {"vlanid": "100", "learn_disable": "true"})

        # Wait for VLAN to appear in ASIC_DB
        vlan_oids = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VLAN", 2)

        vlan100_oid = None
        for oid in vlan_oids:
            fvs = asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_VLAN", oid)
            if fvs.get("SAI_VLAN_ATTR_VLAN_ID") == "100":
                vlan100_oid = oid

        assert vlan100_oid is not None, "Vlan100 not found in ASIC_DB"

        # Verify learn_disable reached ASIC_DB
        asic_db.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_VLAN", vlan100_oid,
                                     {"SAI_VLAN_ATTR_LEARN_DISABLE": "true"})

        # Toggle to enabled
        config_db.update_entry("VLAN", "Vlan100",
                               {"vlanid": "100", "learn_disable": "false"})
        asic_db.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_VLAN", vlan100_oid,
                                     {"SAI_VLAN_ATTR_LEARN_DISABLE": "false"})

        # Cleanup
        config_db.delete_entry("VLAN", "Vlan100")
        asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VLAN", 1)
