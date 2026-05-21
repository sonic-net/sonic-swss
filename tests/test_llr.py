
import os
import time
import pytest
from swsscommon import swsscommon

# Deterministic test fixture values – SPEED and CABLE profile values must
# exist in llr_profile_lookup.ini in the same directory.
SPEED    = "40000"
CABLE    = "40m"


# Path to the INI file injected into the container at runtime
LLR_INI_FILENAME = "llr_profile_lookup.ini"
LLR_INI_SRC      = os.path.join(os.path.dirname(__file__), LLR_INI_FILENAME)
LLR_INI_DEST     = "/usr/share/sonic/hwsku/"

INTF0 = "Ethernet0"
INTF4 = "Ethernet4"


class TestLlr:
    def setup_db(self, dvs):
        self.app_db    = dvs.get_app_db()
        self.config_db = dvs.get_config_db()
        self.state_db  = dvs.get_state_db()
        self.asic_db   = dvs.get_asic_db()

    def set_cable_length(self, port, cable_len):
        self.config_db.update_entry("CABLE_LENGTH", "AZURE", {port: cable_len})

    def del_cable_length(self, port):
        """Delete a single port's field from CABLE_LENGTH|AZURE."""
        tbl = swsscommon.Table(
            self.config_db.db_connection, "CABLE_LENGTH"
        )
        tbl.hdel("AZURE", port)

    def set_llr_port(self, port, llr_local="disabled", llr_remote="disabled",
                     llr_mode="static", llr_profile=None):
        fvs = {
            "llr_mode":   llr_mode,
            "llr_local":  llr_local,
            "llr_remote": llr_remote,
        }
        if llr_profile is not None:
            fvs["llr_profile"] = llr_profile
        self.config_db.update_entry("LLR_PORT", port, fvs)

    def del_llr_port(self, port):
        self.config_db.delete_entry("LLR_PORT", port)

    def set_user_profile(self, name, fields):
        self.config_db.update_entry("LLR_PROFILE", name, fields)

    def del_user_profile(self, name):
        self.config_db.delete_entry("LLR_PROFILE", name)

    def get_speed(self, port):
        fvs = self.state_db.wait_for_fields("PORT_TABLE", port, ["speed"])
        return fvs["speed"]

    @pytest.fixture(autouse=True)
    def setup_teardown(self, dvs):
        self.setup_db(dvs)

        # Inject the INI file and restart llrmgrd so it picks up the lookup table.
        dvs.copy_file(LLR_INI_DEST, LLR_INI_SRC)
        dvs.runcmd(['supervisorctl', 'restart', 'llrmgrd'])
        time.sleep(2)

        dvs.port_admin_set(INTF0, "up")
        dvs.port_admin_set(INTF4, "up")

        # Save originals so we can restore after the test.
        orig_speeds = {}
        orig_cables = {}
        cable_tbl = swsscommon.Table(
            self.config_db.db_connection, "CABLE_LENGTH"
        )
        for port in (INTF0, INTF4):
            orig_speeds[port] = self.get_speed(port)
            exists, val = cable_tbl.hget("AZURE", port)
            if exists:
                orig_cables[port] = val

        # Pin port speed to SPEED so profile names are deterministic.
        for port in (INTF0, INTF4):
            self.state_db.update_entry("PORT_TABLE", port, {"speed": SPEED})
        time.sleep(1)

        yield

        # Teardown: wipe LLR config
        for port in (INTF0, INTF4):
            try:
                self.config_db.delete_entry("LLR_PORT", port)
            except Exception:
                # Entry may not exist if the test did not create it.
                pass
        for entry in self.config_db.get_keys("LLR_PROFILE"):
            try:
                self.config_db.delete_entry("LLR_PROFILE", entry)
            except Exception:
                # Entry may already have been removed by the test.
                pass
        # Restore original cable lengths (or delete if none existed)
        for port in (INTF0, INTF4):
            try:
                if port in orig_cables:
                    self.set_cable_length(port, orig_cables[port])
                else:
                    self.del_cable_length(port)
            except Exception:
                # Best-effort restore; cable may not exist.
                pass
        # Restore original port speeds
        for port, speed in orig_speeds.items():
            try:
                self.state_db.update_entry("PORT_TABLE", port, {"speed": speed})
            except Exception:
                # Best-effort restore; port entry may not exist.
                pass

    def test_capability_written_to_state_db(self, dvs, testlog):
        """
        1. Verify LLR_CAPABLE is present in STATE_DB SWITCH_CAPABILITY|switch.
        2. Verify LLR_SUPPORTED_PROFILE_ATTRIBUTES is present in STATE_DB SWITCH_CAPABILITY|switch.
        """
        cap_fvs = self.state_db.wait_for_entry("SWITCH_CAPABILITY", "switch")
        assert "LLR_CAPABLE" in cap_fvs, \
            "LLR_CAPABLE field missing from STATE_DB SWITCH_CAPABILITY|switch"
        assert "LLR_SUPPORTED_PROFILE_ATTRIBUTES" in cap_fvs, \
            "LLR_SUPPORTED_PROFILE_ATTRIBUTES field missing from STATE_DB SWITCH_CAPABILITY|switch"

    def test_ini_profile_generation(self, dvs, testlog):
        """
        1. Delete pre-existing cable_length for INTF0.
        2. Enable LLR on INTF0 BEFORE cable_length — APPL_DB must NOT be populated yet.
        3. Set cable_length — deferred profile generation completes.
        4. LLR_PROFILE_TABLE|<profile> appears in APPL_DB with at least one field.
        5. LLR_PORT_TABLE|<port> appears with llr_local/remote=enabled and correct profile name.
        6. SAI_PORT_ATTR_LLR_MODE_LOCAL and SAI_PORT_ATTR_LLR_MODE_REMOTE set to "true" in ASIC_DB.
        7. SAI_OBJECT_TYPE_PORT_LLR_PROFILE object created in ASIC_DB.
        """

        speed = self.get_speed(INTF0)
        profile = f"llr_{speed}_{CABLE}_profile"

        # Ensure no stale cable_length from a previous test.
        self.del_cable_length(INTF0)

        # LLR_PORT before cable → profile generation deferred.
        self.set_llr_port(INTF0, llr_local="enabled", llr_remote="enabled")
        time.sleep(1)
        assert not self.app_db.get_entry("LLR_PORT_TABLE", INTF0), \
            "APPL_DB LLR_PORT must not appear before cable_length is set"

        # Cable arrival triggers deferred profile generation.
        self.set_cable_length(INTF0, CABLE)

        profile_fvs = self.app_db.wait_for_entry("LLR_PROFILE_TABLE", profile)
        assert profile_fvs, "APPL_DB LLR_PROFILE_TABLE entry is empty"

        port_fvs = self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
        assert port_fvs["llr_local"]   == "enabled"
        assert port_fvs["llr_remote"]  == "enabled"
        assert port_fvs["llr_profile"] == profile

        port_oid = self.asic_db.port_name_map[INTF0]
        self.asic_db.wait_for_field_match(
            "ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid,
            {"SAI_PORT_ATTR_LLR_MODE_LOCAL": "true",
             "SAI_PORT_ATTR_LLR_MODE_REMOTE": "true"})
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT_LLR_PROFILE", 1)

    def test_port_not_published_until_both_enabled(self, dvs, testlog):
        """
        1. Enable only llr_local on INTF0 (llr_remote disabled).
        2. LLR_PORT_TABLE must NOT appear in APPL_DB — publish requires both directions enabled.
        3. Enable llr_remote, LLR_PORT_TABLE and LLR_PROFILE_TABLE appear in APPL_DB.
        """

        self.set_cable_length(INTF0, CABLE)
        speed = self.get_speed(INTF0)
        profile = f"llr_{speed}_{CABLE}_profile"

        self.set_llr_port(INTF0, llr_local="enabled", llr_remote="disabled")
        time.sleep(1)
        assert not self.app_db.get_entry("LLR_PORT_TABLE", INTF0), \
            "APPL_DB LLR_PORT must not appear when only one direction is enabled"

        # Enable both directions → port published.
        self.set_llr_port(INTF0, llr_local="enabled", llr_remote="enabled")
        port_fvs = self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
        assert port_fvs["llr_local"]   == "enabled"
        assert port_fvs["llr_remote"]  == "enabled"
        assert port_fvs["llr_profile"] == profile
        self.app_db.wait_for_entry("LLR_PROFILE_TABLE", profile)

    def test_user_profile_precedence(self, dvs, testlog):
        """
        1. Define a user LLR_PROFILE in CONFIG_DB — must NOT appear in APPL_DB until bound.
        2. Enable LLR on INTF0 referencing the user profile name.
        3. LLR_PORT_TABLE.llr_profile equals the user profile name.
        4. LLR_PROFILE_TABLE has the user profile with correct field values.
        5. INI-derived profile name does NOT appear in LLR_PROFILE_TABLE.
        6. Speed/cable change does NOT switch away from the user profile.
        """

        user_profile = "my_custom_profile"
        user_fvs = {
            "max_outstanding_frames": "256",
            "max_outstanding_bytes":  "131072",
            "pcs_lost_timeout":       "500",
        }
        self.set_user_profile(user_profile, user_fvs)

        # Unbound user profile must NOT appear in APPL_DB.
        time.sleep(1)
        assert not self.app_db.get_entry("LLR_PROFILE_TABLE", user_profile), \
            "Unbound user profile must not be published to APPL_DB"

        self.set_cable_length(INTF0, CABLE)
        speed = self.get_speed(INTF0)
        ini_profile = f"llr_{speed}_{CABLE}_profile"

        self.set_llr_port(INTF0, llr_local="enabled", llr_remote="enabled",
                          llr_profile=user_profile)

        port_fvs = self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
        assert port_fvs["llr_profile"] == user_profile

        ap_profile = self.app_db.wait_for_entry("LLR_PROFILE_TABLE", user_profile)
        assert ap_profile.get("max_outstanding_frames") == "256"

        ini_profile_fvs = self.app_db.get_entry("LLR_PROFILE_TABLE", ini_profile)
        assert not ini_profile_fvs, "INI profile must not appear when user profile is set"

        # Speed/cable change must NOT affect user profile binding.
        self.set_cable_length(INTF0, "5m")
        time.sleep(1)
        port_fvs = self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
        assert port_fvs["llr_profile"] == user_profile, \
            "Cable change must not switch away from user profile"

        try:
            self.state_db.update_entry("PORT_TABLE", INTF0, {"speed": "200000"})
            time.sleep(1)
            port_fvs = self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
            assert port_fvs["llr_profile"] == user_profile, \
                "Speed change must not switch away from user profile"
        finally:
            self.state_db.update_entry("PORT_TABLE", INTF0, {"speed": SPEED})

    def test_shared_profile_refcount(self, dvs, testlog):
        """
        1. Enable LLR on INTF0 and INTF4 with same cable → same profile name.
        2. Both LLR_PORT_TABLE entries and the shared LLR_PROFILE_TABLE entry appear.
        3. Remove LLR from INTF0 — profile still in APPL_DB (INTF4 holds a ref).
        4. Remove LLR from INTF4 — profile deleted from APPL_DB.
        """

        self.set_cable_length(INTF0, CABLE)
        self.set_cable_length(INTF4, CABLE)
        speed = self.get_speed(INTF0)
        profile = f"llr_{speed}_{CABLE}_profile"

        self.set_llr_port(INTF0, llr_local="enabled", llr_remote="enabled")
        self.set_llr_port(INTF4, llr_local="enabled", llr_remote="enabled")

        self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
        self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF4)
        self.app_db.wait_for_entry("LLR_PROFILE_TABLE", profile)

        self.del_llr_port(INTF0)
        self.app_db.wait_for_deleted_entry("LLR_PORT_TABLE", INTF0)
        still_there = self.app_db.get_entry("LLR_PROFILE_TABLE", profile)
        assert still_there, "Profile deleted prematurely while INTF4 still holds a reference"

        self.del_llr_port(INTF4)
        self.app_db.wait_for_deleted_entry("LLR_PORT_TABLE", INTF4)
        self.app_db.wait_for_deleted_entry("LLR_PROFILE_TABLE", profile)

    def test_profile_change_while_active(self, dvs, testlog):
        """
        1. Enable LLR on INTF0 with CABLE="40m" and wait for APPL_DB profile.
        2. Change cable_length to "10m" while LLR is active.
        3. New profile appears in LLR_PROFILE_TABLE; old profile removed.
        4. LLR_PORT_TABLE reflects the new profile name.
        5. Restore cable, then change speed while LLR is active.
        6. New speed-based profile appears; old profile removed.
        7. LLR_PORT_TABLE reflects the speed-derived profile name.
        """

        self.set_cable_length(INTF0, CABLE)
        speed = self.get_speed(INTF0)
        profile = f"llr_{speed}_{CABLE}_profile"
        self.set_llr_port(INTF0, llr_local="enabled", llr_remote="enabled")
        self.app_db.wait_for_entry("LLR_PROFILE_TABLE", profile)

        # --- Cable change ---
        cable_new   = "5m"
        profile_new = f"llr_{speed}_{cable_new}_profile"

        self.set_cable_length(INTF0, cable_new)

        new_profile_fvs = self.app_db.wait_for_entry("LLR_PROFILE_TABLE", profile_new)
        assert new_profile_fvs, "New profile not published after cable_length change"
        self.app_db.wait_for_deleted_entry("LLR_PROFILE_TABLE", profile)

        port_fvs = self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
        assert port_fvs["llr_profile"] == profile_new

        # Restore cable for speed-change phase.
        self.set_cable_length(INTF0, CABLE)
        self.app_db.wait_for_entry("LLR_PROFILE_TABLE", profile)
        self.app_db.wait_for_deleted_entry("LLR_PROFILE_TABLE", profile_new)

        # --- Speed change ---
        speed_new       = "200000"
        speed_profile   = f"llr_{speed_new}_{CABLE}_profile"

        try:
            self.state_db.update_entry("PORT_TABLE", INTF0, {"speed": speed_new})

            new_profile_fvs = self.app_db.wait_for_entry("LLR_PROFILE_TABLE", speed_profile)
            assert new_profile_fvs, "New profile not published after speed change"
            self.app_db.wait_for_deleted_entry("LLR_PROFILE_TABLE", profile)

            port_fvs = self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
            assert port_fvs["llr_profile"] == speed_profile
        finally:
            self.state_db.update_entry("PORT_TABLE", INTF0, {"speed": SPEED})

    def test_cable_speed_delete_on_llr_active_port(self, dvs, testlog):
        """
        1. Enable LLR on INTF0; both LLR_PORT_TABLE and LLR_PROFILE_TABLE appear.
        2. Delete cable_length — LLR_PORT_TABLE and LLR_PROFILE_TABLE removed from APPL_DB.
        3. Re-add cable_length — port auto-recovers; LLR_PORT_TABLE and profile reappear.
        4. Simulate speed removal — LLR_PORT_TABLE and LLR_PROFILE_TABLE removed from APPL_DB.
        5. Restore speed — port auto-recovers; LLR_PORT_TABLE and profile reappear.
        """

        self.set_cable_length(INTF0, CABLE)
        speed = self.get_speed(INTF0)
        profile = f"llr_{speed}_{CABLE}_profile"
        self.set_llr_port(INTF0, llr_local="enabled", llr_remote="enabled")
        self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
        self.app_db.wait_for_entry("LLR_PROFILE_TABLE", profile)

        # --- Cable delete and recovery ---
        self.del_cable_length(INTF0)

        self.app_db.wait_for_deleted_entry("LLR_PORT_TABLE", INTF0)
        self.app_db.wait_for_deleted_entry("LLR_PROFILE_TABLE", profile)

        self.set_cable_length(INTF0, CABLE)
        self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
        fvs = self.app_db.wait_for_entry("LLR_PROFILE_TABLE", profile)
        assert fvs, "Profile not re-created after cable_length restored"

        # --- Speed delete and recovery ---
        try:
            self.state_db.delete_entry("PORT_TABLE", INTF0)

            self.app_db.wait_for_deleted_entry("LLR_PORT_TABLE", INTF0)
            self.app_db.wait_for_deleted_entry("LLR_PROFILE_TABLE", profile)

            self.state_db.update_entry("PORT_TABLE", INTF0, {"speed": SPEED})
            self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
            fvs = self.app_db.wait_for_entry("LLR_PROFILE_TABLE", profile)
            assert fvs, "Profile not re-created after speed restored"
        finally:
            # Ensure speed is always restored for subsequent tests.
            self.state_db.update_entry("PORT_TABLE", INTF0, {"speed": SPEED})

    def test_llr_port_del_removes_appl_db(self, dvs, testlog):
        """
        1. Enable LLR on INTF0; LLR_PORT_TABLE and LLR_PROFILE_TABLE appear in APPL_DB.
        2. SAI_PORT_ATTR_LLR_MODE_LOCAL/REMOTE set to "true" in ASIC_DB.
        3. Delete LLR_PORT in CONFIG_DB.
        4. LLR_PORT_TABLE entry removed from APPL_DB.
        5. LLR_PROFILE_TABLE entry removed from APPL_DB (last reference gone).
        6. SAI_PORT_ATTR_LLR_MODE_LOCAL/REMOTE reset to "false" in ASIC_DB.
        7. SAI_OBJECT_TYPE_PORT_LLR_PROFILE object removed from ASIC_DB.
        """

        self.set_cable_length(INTF0, CABLE)
        speed = self.get_speed(INTF0)
        profile = f"llr_{speed}_{CABLE}_profile"
        self.set_llr_port(INTF0, llr_local="enabled", llr_remote="enabled")
        self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
        self.app_db.wait_for_entry("LLR_PROFILE_TABLE", profile)

        port_oid = self.asic_db.port_name_map[INTF0]
        self.asic_db.wait_for_field_match(
            "ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid,
            {"SAI_PORT_ATTR_LLR_MODE_LOCAL": "true",
             "SAI_PORT_ATTR_LLR_MODE_REMOTE": "true"})
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT_LLR_PROFILE", 1)

        self.del_llr_port(INTF0)

        self.app_db.wait_for_deleted_entry("LLR_PORT_TABLE", INTF0)
        self.app_db.wait_for_deleted_entry("LLR_PROFILE_TABLE", profile)
        self.asic_db.wait_for_field_match(
            "ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid,
            {"SAI_PORT_ATTR_LLR_MODE_LOCAL": "false",
             "SAI_PORT_ATTR_LLR_MODE_REMOTE": "false"})
        self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT_LLR_PROFILE", 0)

    def test_invalid_config_db_profile_falls_back_to_ini(self, dvs, testlog):
        """
        1. Create a CONFIG_DB profile with missing mandatory fields.
        2. Bind that profile to INTF0 via llr_profile and enable LLR.
        3. Verify APPL_DB falls back to INI-derived profile (invalid profile rejected).
        4. Verify the invalid profile does not appear in APPL_DB LLR_PROFILE_TABLE.
        5. Update the CONFIG_DB profile to add mandatory fields.
        6. Verify APPL_DB switches to the now-valid user profile.
        """

        speed = self.get_speed(INTF0)
        ini_profile = f"llr_{speed}_{CABLE}_profile"
        bad_profile = "bad_profile"

        self.set_user_profile(bad_profile, {"pcs_lost_timeout": "500"})
        self.set_cable_length(INTF0, CABLE)
        self.set_llr_port(INTF0, llr_local="enabled", llr_remote="enabled",
                          llr_profile=bad_profile)

        port_fvs = self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
        assert port_fvs["llr_profile"] == ini_profile, \
            "Invalid CONFIG_DB profile should fall back to INI-derived profile"
        self.app_db.wait_for_entry("LLR_PROFILE_TABLE", ini_profile)

        bad_fvs = self.app_db.get_entry("LLR_PROFILE_TABLE", bad_profile)
        assert not bad_fvs, "Invalid profile must not appear in APPL_DB"

        # Fix the profile by adding mandatory fields.
        self.set_user_profile(bad_profile, {
            "max_outstanding_frames": "128",
            "max_outstanding_bytes":  "65536",
            "pcs_lost_timeout":       "500",
        })

        time.sleep(2)
        port_fvs = self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
        assert port_fvs["llr_profile"] == bad_profile, \
            "Port should switch to user profile after mandatory fields are added"
        self.app_db.wait_for_entry("LLR_PROFILE_TABLE", bad_profile)

    def test_late_profile_arrival_rebinds_port(self, dvs, testlog):
        """
        1. Enable LLR on INTF0 referencing a user profile that does not yet exist in CONFIG_DB.
        2. Verify port falls back to INI-derived profile in APPL_DB.
        3. Create the user profile in CONFIG_DB with valid mandatory fields.
        4. Verify port switches to the user profile in APPL_DB.
        """

        speed = self.get_speed(INTF0)
        ini_profile = f"llr_{speed}_{CABLE}_profile"
        late_profile = "late_arrival_profile"

        self.set_cable_length(INTF0, CABLE)
        self.set_llr_port(INTF0, llr_local="enabled", llr_remote="enabled",
                          llr_profile=late_profile)

        port_fvs = self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
        assert port_fvs["llr_profile"] == ini_profile, \
            "Non-existing user profile should fall back to INI"

        self.set_user_profile(late_profile, {
            "max_outstanding_frames": "512",
            "max_outstanding_bytes":  "262144",
        })

        time.sleep(2)
        port_fvs = self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
        assert port_fvs["llr_profile"] == late_profile, \
            "Port should rebind to user profile after late arrival"
        self.app_db.wait_for_entry("LLR_PROFILE_TABLE", late_profile)

    def test_cable_before_llr_port_backfill(self, dvs, testlog):
        """
        1. Set cable length and speed before configuring LLR_PORT.
        2. Verify no LLR_PORT_TABLE entry in APPL_DB before LLR is configured.
        3. Configure LLR_PORT with both directions enabled.
        4. Verify LLR activates immediately with correct profile (cable/speed back-filled).
        """

        self.set_cable_length(INTF0, CABLE)
        speed = self.get_speed(INTF0)
        profile = f"llr_{speed}_{CABLE}_profile"

        time.sleep(1)
        assert not self.app_db.get_entry("LLR_PORT_TABLE", INTF0), \
            "No LLR_PORT in APPL_DB before LLR_PORT configured"

        self.set_llr_port(INTF0, llr_local="enabled", llr_remote="enabled")
        port_fvs = self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
        assert port_fvs["llr_profile"] == profile
        self.app_db.wait_for_entry("LLR_PROFILE_TABLE", profile)

    def test_llr_config_recovery_after_invalid_cable_speed(self, dvs, testlog):
        """
        1. Enable LLR on INTF0 with valid cable and speed; verify profile is active.
        2. Change cable to a value with no INI entry.
        3. Verify LLR is disabled: LLR_PORT_TABLE and LLR_PROFILE_TABLE removed from APPL_DB.
        4. Restore original cable; verify port auto-recovers.
        5. Change speed to a value with no INI entry.
        6. Verify LLR is disabled: LLR_PORT_TABLE and LLR_PROFILE_TABLE removed from APPL_DB.
        7. Restore original speed; verify port auto-recovers.
        """

        self.set_cable_length(INTF0, CABLE)
        speed = self.get_speed(INTF0)
        profile = f"llr_{speed}_{CABLE}_profile"

        self.set_llr_port(INTF0, llr_local="enabled", llr_remote="enabled")
        self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
        self.app_db.wait_for_entry("LLR_PROFILE_TABLE", profile)

        # --- Invalid cable ---
        self.set_cable_length(INTF0, "999m")

        self.app_db.wait_for_deleted_entry("LLR_PORT_TABLE", INTF0)
        self.app_db.wait_for_deleted_entry("LLR_PROFILE_TABLE", profile)

        self.set_cable_length(INTF0, CABLE)
        self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
        fvs = self.app_db.wait_for_entry("LLR_PROFILE_TABLE", profile)
        assert fvs, "Profile not re-created after valid cable restored"

        # --- Invalid speed ---
        invalid_speed = "999999"
        try:
            self.state_db.update_entry("PORT_TABLE", INTF0, {"speed": invalid_speed})

            self.app_db.wait_for_deleted_entry("LLR_PORT_TABLE", INTF0)
            self.app_db.wait_for_deleted_entry("LLR_PROFILE_TABLE", profile)

            self.state_db.update_entry("PORT_TABLE", INTF0, {"speed": speed})
            self.app_db.wait_for_entry("LLR_PORT_TABLE", INTF0)
            fvs = self.app_db.wait_for_entry("LLR_PROFILE_TABLE", profile)
            assert fvs, "Profile not re-created after valid speed restored"
        finally:
            self.state_db.update_entry("PORT_TABLE", INTF0, {"speed": SPEED})
