from swsscommon import swsscommon

SOFT_BFD_STATE_TABLE = "BFD_SOFTWARE_SESSION_TABLE"

# SAI reports HW BFD offload as supported, but SYSTEM_DEFAULTS|software_bfd
# in CONFIG_DB overrides it to force software BFD.
DVS_ENV = ["BFDOFFLOAD=true"]


class TestSoftBfdOverride(object):
    """Verify that SYSTEM_DEFAULTS|software_bfd=enabled forces software BFD
    even when the SAI/ASIC reports HW BFD offload support (BFDOFFLOAD=true).
    """

    def setup_db(self, dvs):
        dvs.setup_db()
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()
        self.sdb = dvs.get_state_db()
        self.cdb = dvs.get_config_db()

        # Set SYSTEM_DEFAULTS|software_bfd to enabled BEFORE restarting swss
        # so that BgpGlobalStateOrch picks it up during initialization.
        self.cdb.create_entry("SYSTEM_DEFAULTS", "software_bfd",
                              {"status": "enabled"})

        # Restart swss so BgpGlobalStateOrch re-reads the config
        dvs.stop_swss()
        dvs.start_swss()

    def get_exist_bfd_session(self):
        return set(self.sdb.get_keys(SOFT_BFD_STATE_TABLE))

    def get_exist_hw_bfd_session(self):
        return set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION"))

    def create_bfd_session(self, key, pairs):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "BFD_SESSION_TABLE")
        fvs = swsscommon.FieldValuePairs(list(pairs.items()))
        tbl.set(key, fvs)

    def remove_bfd_session(self, key):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "BFD_SESSION_TABLE")
        tbl._del(key)

    def test_softwareBfdOverridesHwOffload(self, dvs):
        """When SYSTEM_DEFAULTS|software_bfd=enabled and SAI reports HW offload,
        BFD sessions should go to STATE_DB BFD_SOFTWARE_SESSION_TABLE
        (software path) and NOT to ASIC_DB (hardware path)."""
        self.setup_db(dvs)

        hw_sessions_before = self.get_exist_hw_bfd_session()
        sw_sessions_before = self.get_exist_bfd_session()

        # Create a BFD session
        key = "default:default:10.0.0.2"
        fieldValues = {"local_addr": "10.0.0.1", "tx_interval": "300",
                       "rx_interval": "500", "multiplier": "3"}
        self.create_bfd_session(key, fieldValues)

        # Session should appear in SOFTWARE BFD table (software path)
        self.sdb.wait_for_n_keys(SOFT_BFD_STATE_TABLE,
                                 len(sw_sessions_before) + 1)

        # Session should NOT appear in ASIC_DB (hardware path)
        hw_sessions_after = self.get_exist_hw_bfd_session()
        assert hw_sessions_after == hw_sessions_before, \
            "BFD session was created in ASIC_DB despite software_bfd=enabled"

        # Verify the STATE_DB entry has the correct values
        state_key = key.replace(":", "|", 2)
        fvs = self.sdb.get_entry(SOFT_BFD_STATE_TABLE, state_key)
        assert fvs["local_addr"] == "10.0.0.1"
        assert fvs["tx_interval"] == "300"
        assert fvs["rx_interval"] == "500"
        assert fvs["multiplier"] == "3"

        # Clean up
        self.remove_bfd_session(key)
        state_key_cleanup = key.replace(":", "|", 2)
        self.sdb.wait_for_deleted_entry(SOFT_BFD_STATE_TABLE, state_key_cleanup)

    def test_softwareBfdOverrideIpv6(self, dvs):
        """Same override test with an IPv6 BFD session."""
        self.setup_db(dvs)

        hw_sessions_before = self.get_exist_hw_bfd_session()
        sw_sessions_before = self.get_exist_bfd_session()

        key = "default:default:2000::2"
        fieldValues = {"local_addr": "2000::1", "multihop": "true",
                       "multiplier": "3", "tx_interval": "400",
                       "rx_interval": "200"}
        self.create_bfd_session(key, fieldValues)

        # Software path
        self.sdb.wait_for_n_keys(SOFT_BFD_STATE_TABLE,
                                 len(sw_sessions_before) + 1)

        # NOT hardware path
        hw_sessions_after = self.get_exist_hw_bfd_session()
        assert hw_sessions_after == hw_sessions_before

        # Verify STATE_DB entry contents
        state_key = key.replace(":", "|", 2)
        fvs = self.sdb.get_entry(SOFT_BFD_STATE_TABLE, state_key)
        assert fvs["local_addr"] == "2000::1"
        assert fvs["multihop"] == "true"
        assert fvs["multiplier"] == "3"
        assert fvs["tx_interval"] == "400"
        assert fvs["rx_interval"] == "200"

        # Clean up
        self.remove_bfd_session(key)
        self.sdb.wait_for_deleted_entry(SOFT_BFD_STATE_TABLE, state_key)

    def test_hwOffloadUsedWhenSoftwareBfdNotSet(self, dvs):
        """When SYSTEM_DEFAULTS|software_bfd is NOT set and SAI reports HW
        offload (BFDOFFLOAD=true), sessions should go to ASIC_DB (HW path)
        and NOT to BFD_SOFTWARE_SESSION_TABLE."""
        dvs.setup_db()
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()
        self.sdb = dvs.get_state_db()
        self.cdb = dvs.get_config_db()

        # Ensure software_bfd is NOT set
        self.cdb.delete_entry("SYSTEM_DEFAULTS", "software_bfd")

        dvs.stop_swss()
        dvs.start_swss()

        hw_sessions_before = self.get_exist_hw_bfd_session()
        sw_sessions_before = self.get_exist_bfd_session()

        key = "default:default:10.0.0.3"
        fieldValues = {"local_addr": "10.0.0.1", "tx_interval": "300",
                       "rx_interval": "500", "multiplier": "3"}
        self.create_bfd_session(key, fieldValues)

        # Session should go to ASIC_DB (hardware path)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION",
                                 len(hw_sessions_before) + 1)

        # Session should NOT appear in software BFD table
        sw_sessions_after = self.get_exist_bfd_session()
        assert sw_sessions_after == sw_sessions_before, \
            "BFD session appeared in SOFTWARE table when software_bfd is not set"

        # Clean up
        self.remove_bfd_session(key)

    def test_hwOffloadUsedWhenSoftwareBfdDisabled(self, dvs):
        """When SYSTEM_DEFAULTS|software_bfd is explicitly set to 'disabled',
        sessions should still go to ASIC_DB (HW path). Only 'enabled' triggers
        the software override."""
        dvs.setup_db()
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()
        self.sdb = dvs.get_state_db()
        self.cdb = dvs.get_config_db()

        # Explicitly set to disabled
        self.cdb.create_entry("SYSTEM_DEFAULTS", "software_bfd",
                              {"status": "disabled"})

        dvs.stop_swss()
        dvs.start_swss()

        hw_sessions_before = self.get_exist_hw_bfd_session()
        sw_sessions_before = self.get_exist_bfd_session()

        key = "default:default:10.0.0.4"
        fieldValues = {"local_addr": "10.0.0.1", "tx_interval": "300",
                       "rx_interval": "500", "multiplier": "3"}
        self.create_bfd_session(key, fieldValues)

        # Session should go to ASIC_DB (hardware path)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION",
                                 len(hw_sessions_before) + 1)

        # Session should NOT appear in software BFD table
        sw_sessions_after = self.get_exist_bfd_session()
        assert sw_sessions_after == sw_sessions_before, \
            "BFD session appeared in SOFTWARE table when software_bfd is disabled"

        # Clean up
        self.remove_bfd_session(key)
        self.cdb.delete_entry("SYSTEM_DEFAULTS", "software_bfd")
