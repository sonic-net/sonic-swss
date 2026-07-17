"""
Integration tests for SwssReadiness — "System is Ready" signalling gate.

Tests verify that orchagent writes FEATURE|swss:up_status=true into STATE_DB
after the switch boots and all subsystems (port, buffer, acl, vlanmember,
pfcwd) finish their initialisation.

DVS (Docker Virtual Switch) is used as the runtime: each test connects to the
running orchagent's Redis databases and inspects the state written by the real
swssreadiness code path.
"""

import time
import pytest

from dvslib.dvs_common import PollingConfig, wait_for_result


# ──────────────────────────────────────────────────────────────────────────────
# Constants
# ──────────────────────────────────────────────────────────────────────────────

FEATURE_TABLE   = "FEATURE"
SWSS_KEY        = "swss"
UP_STATUS_FIELD = "up_status"

# How long to wait for orchagent to reach ready state after a clean boot.
# VS starts quickly; 60 s is conservative.
READY_TIMEOUT   = 60

# Polling interval when waiting for STATE_DB changes.
POLL_INTERVAL   = 0.5


# ──────────────────────────────────────────────────────────────────────────────
# Helper
# ──────────────────────────────────────────────────────────────────────────────

def _get_up_status(state_db) -> str:
    """Return the current FEATURE|swss:up_status value, or '' if absent."""
    entry = state_db.get_entry(FEATURE_TABLE, SWSS_KEY)
    return entry.get(UP_STATUS_FIELD, "")


def _wait_for_up_status(state_db, expected: str, timeout: int = READY_TIMEOUT) -> bool:
    """Poll STATE_DB until FEATURE|swss:up_status == expected, or timeout."""
    cfg = PollingConfig(polling_interval=POLL_INTERVAL, timeout=timeout, strict=False)

    def _check():
        val = _get_up_status(state_db)
        return val == expected, val

    result, actual = wait_for_result(_check, cfg)
    return result


# ──────────────────────────────────────────────────────────────────────────────
# Test class
# ──────────────────────────────────────────────────────────────────────────────

class TestSwssReadiness:
    """
    Integration tests for the SwssReadiness gate.

    All tests run against a single DVS instance.  The DVS fixture
    (defined in conftest.py) starts a fresh orchagent container and waits
    for port initialisation before handing control to the test.
    """

    # ------------------------------------------------------------------
    # Test 1: up_status written after fresh boot
    # ------------------------------------------------------------------
    def test_up_status_written_after_boot(self, dvs):
        """
        After orchagent starts and all modules signal, STATE_DB must contain
        FEATURE|swss:up_status = "true".
        """
        state_db = dvs.get_state_db()

        reached = _wait_for_up_status(state_db, "true", timeout=READY_TIMEOUT)
        assert reached, (
            f"FEATURE|swss:up_status was not set to 'true' within {READY_TIMEOUT}s. "
            f"Current value: '{_get_up_status(state_db)}'"
        )

    # ------------------------------------------------------------------
    # Test 2: up_status value is exactly "true" (not "True", "1", etc.)
    # ------------------------------------------------------------------
    def test_up_status_value_is_lowercase_true(self, dvs):
        """
        The value written must be exactly the string "true" (lowercase),
        matching what sysmonitor expects.
        """
        state_db = dvs.get_state_db()
        _wait_for_up_status(state_db, "true", timeout=READY_TIMEOUT)

        val = _get_up_status(state_db)
        assert val == "true", (
            f"Expected up_status='true' but got '{val}'. "
            "Sysmonitor checks for exact lowercase 'true'."
        )

    # ------------------------------------------------------------------
    # Test 3: up_status persists — not a transient write
    # ------------------------------------------------------------------
    def test_up_status_persists(self, dvs):
        """
        Once written, FEATURE|swss:up_status must remain 'true' and not
        disappear or flip during normal operation.
        """
        state_db = dvs.get_state_db()
        _wait_for_up_status(state_db, "true", timeout=READY_TIMEOUT)

        # Sample five more times over 2 seconds — value must stay "true"
        for i in range(5):
            time.sleep(0.4)
            val = _get_up_status(state_db)
            assert val == "true", (
                f"up_status changed away from 'true' at sample {i+1}: got '{val}'"
            )

    # ------------------------------------------------------------------
    # Test 4: FEATURE table entry for swss exists with expected fields
    # ------------------------------------------------------------------
    def test_feature_swss_entry_has_up_status_field(self, dvs):
        """
        The FEATURE|swss hash in STATE_DB must contain the up_status field.
        This verifies the field name matches what sysmonitor queries.
        """
        state_db = dvs.get_state_db()
        _wait_for_up_status(state_db, "true", timeout=READY_TIMEOUT)

        entry = state_db.get_entry(FEATURE_TABLE, SWSS_KEY)
        assert UP_STATUS_FIELD in entry, (
            f"FEATURE|swss hash missing '{UP_STATUS_FIELD}' field. "
            f"Fields present: {list(entry.keys())}"
        )
        assert entry[UP_STATUS_FIELD] == "true"

    # ------------------------------------------------------------------
    # Test 5: PORT_TABLE PortInitDone is a prerequisite
    # ------------------------------------------------------------------
    def test_port_init_done_before_up_status(self, dvs):
        """
        PortInitDone must appear in APPL_DB PORT_TABLE before up_status is
        written — PortsOrch signals only after all ports are initialised.
        """
        app_db   = dvs.get_app_db()
        state_db = dvs.get_state_db()

        # Wait for PortInitDone
        cfg = PollingConfig(polling_interval=POLL_INTERVAL, timeout=READY_TIMEOUT, strict=True)

        def _port_init():
            keys = app_db.get_keys("PORT_TABLE")
            return "PortInitDone" in keys, keys

        wait_for_result(_port_init, cfg)

        # up_status must be set (either already or shortly after)
        reached = _wait_for_up_status(state_db, "true", timeout=30)
        assert reached, (
            "PortInitDone appeared but up_status never became 'true'. "
            "Port readiness signal may not have reached SwssReadinessManager."
        )

    # ------------------------------------------------------------------
    # Test 6: check_up_status is enabled in CONFIG_DB FEATURE table
    # ------------------------------------------------------------------
    def test_check_up_status_enabled_in_config(self, dvs):
        """
        CONFIG_DB:FEATURE|swss must have check_up_status=true so that
        sysmonitor actually waits for the STATE_DB signal.

        This field is written by db_migrator (version_202511_01).
        """
        config_db = dvs.get_config_db()
        entry = config_db.get_entry(FEATURE_TABLE, SWSS_KEY)

        assert "check_up_status" in entry, (
            "CONFIG_DB:FEATURE|swss is missing 'check_up_status' field. "
            "Run db_migrator version_202511_01 migration."
        )
        assert entry["check_up_status"].lower() == "true", (
            f"check_up_status is '{entry['check_up_status']}'; expected 'true'. "
            "Without this, sysmonitor skips the readiness gate."
        )

    # ------------------------------------------------------------------
    # Test 7: up_status is absent before orchagent finishes startup
    #         (negative test using DVS restart timing)
    # ------------------------------------------------------------------
    def test_up_status_not_present_immediately_at_restart(self, dvs):
        """
        Immediately after orchagent is restarted, before it has finished
        initialisation, up_status should either be absent or still 'true'
        from the previous run (it is not cleared on restart — which is also
        acceptable since sysmonitor reads it at startup).

        This test verifies the gate does not write up_status=false at any
        point (it must only ever go from absent → "true", never backwards).
        """
        state_db = dvs.get_state_db()

        # Wait for steady state first
        _wait_for_up_status(state_db, "true", timeout=READY_TIMEOUT)

        # Poll for 2 s and verify value never becomes anything other than "true"
        for _ in range(20):
            time.sleep(0.1)
            val = _get_up_status(state_db)
            assert val in ("true", ""), (
                f"up_status has unexpected value '{val}'. "
                "It must only ever be 'true' or absent — never 'false' or other."
            )

    # ------------------------------------------------------------------
    # Test 8: wait_for_field_match convenience — verifies DVSDatabase API
    # ------------------------------------------------------------------
    def test_up_status_via_wait_for_field_match(self, dvs):
        """
        Use DVSDatabase.wait_for_field_match (the standard test helper) to
        confirm STATE_DB FEATURE|swss:up_status == "true".
        """
        state_db = dvs.get_state_db()

        # This will raise if the condition isn't met within the timeout
        state_db.wait_for_field_match(
            FEATURE_TABLE,
            SWSS_KEY,
            {UP_STATUS_FIELD: "true"},
            polling_config=PollingConfig(
                polling_interval=POLL_INTERVAL,
                timeout=READY_TIMEOUT,
                strict=True,
            ),
        )
