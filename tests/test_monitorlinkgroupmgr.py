import time
import pytest
from swsscommon import swsscommon
from dvslib.dvs_common import PollingConfig

CFG_MLG_TABLE = "MONITOR_LINK_GROUP"
STATE_MLG_STATE_TABLE = "MONITOR_LINK_GROUP_STATE"
STATE_MLG_MEMBER_TABLE = "MONITOR_LINK_GROUP_MEMBER"
STATE_PORT_TABLE = "PORT_TABLE"


class TestMonitorLinkGroup:
    def setup_dbs(self, dvs):
        self.cfg_db = dvs.get_config_db()
        self.sdb = dvs.get_state_db()
        # dvs.asicdb is AsicDbValidator(DVSDatabase) with portnamemap and wait_for_field_match
        self.asic_db = dvs.asicdb

    def set_port_oper(self, port, status):
        """Write oper_status to STATE_DB:PORT_TABLE (simulates portsyncd)."""
        self.sdb.create_entry(STATE_PORT_TABLE, port, {"oper_status": status})
        time.sleep(0.5)

    def set_port_cfg_admin(self, port, status):
        self.cfg_db.update_entry("PORT", port, {"admin_status": status})
        time.sleep(0.5)

    def create_group(self, name, uplinks, downlinks, min_uplinks=1, linkup_delay=0, desc=""):
        entry = {
            "description": desc,
            "uplinks": ",".join(uplinks),
            "downlinks": ",".join(downlinks),
            "min-uplinks": str(min_uplinks),
            "link-up-delay": str(linkup_delay),
        }
        self.cfg_db.create_entry(CFG_MLG_TABLE, name, entry)

    def delete_group(self, name):
        self.cfg_db.delete_entry(CFG_MLG_TABLE, name)

    def wait_group_state(self, name, state, timeout=15):
        self.sdb.wait_for_field_match(
            STATE_MLG_STATE_TABLE, name, {"state": state},
            polling_config=PollingConfig(timeout=timeout),
        )

    def wait_member_state(self, port, state, timeout=15):
        self.sdb.wait_for_field_match(
            STATE_MLG_MEMBER_TABLE, port, {"state": state},
            polling_config=PollingConfig(timeout=timeout),
        )

    def wait_asic_admin(self, port, up, timeout=15):
        oid = self.asic_db.portnamemap[port]
        self.asic_db.wait_for_field_match(
            "ASIC_STATE:SAI_OBJECT_TYPE_PORT", oid,
            {"SAI_PORT_ATTR_ADMIN_STATE": "true" if up else "false"},
            polling_config=PollingConfig(timeout=timeout),
        )

    def test_group_lifecycle(self, dvs, testlog):
        """Group add → STATE_DB appears with correct state; delete → STATE_DB cleaned up."""
        self.setup_dbs(dvs)

        self.set_port_oper("Ethernet0", "up")
        self.set_port_oper("Ethernet4", "up")

        self.create_group("ml_lifecycle", ["Ethernet0"], ["Ethernet4"])

        # Uplink is up: group must be UP immediately (no link-up-delay)
        self.wait_group_state("ml_lifecycle", "up")
        self.wait_member_state("Ethernet4", "allow_up")

        self.delete_group("ml_lifecycle")
        self.sdb.wait_for_deleted_entry(STATE_MLG_STATE_TABLE, "ml_lifecycle")
        self.sdb.wait_for_deleted_entry(STATE_MLG_MEMBER_TABLE, "Ethernet4")

    def test_uplink_down_forces_downlink(self, dvs, testlog):
        """Uplink oper-down → group DOWN + downlink force_down + ASIC false; recovery reverses all."""
        self.setup_dbs(dvs)

        self.set_port_cfg_admin("Ethernet0", "up")
        self.set_port_cfg_admin("Ethernet4", "up")
        self.set_port_oper("Ethernet0", "up")
        self.set_port_oper("Ethernet4", "up")
        self.wait_asic_admin("Ethernet4", True)

        self.create_group("ml_force", ["Ethernet0"], ["Ethernet4"])
        self.wait_group_state("ml_force", "up")
        self.wait_member_state("Ethernet4", "allow_up")

        # Uplink goes down
        self.set_port_oper("Ethernet0", "down")
        self.wait_group_state("ml_force", "down")
        self.wait_member_state("Ethernet4", "force_down")
        self.wait_asic_admin("Ethernet4", False)

        # Uplink recovers
        self.set_port_oper("Ethernet0", "up")
        self.wait_group_state("ml_force", "up")
        self.wait_member_state("Ethernet4", "allow_up")
        self.wait_asic_admin("Ethernet4", True)

        self.delete_group("ml_force")
        self.sdb.wait_for_deleted_entry(STATE_MLG_STATE_TABLE, "ml_force")
        self.sdb.wait_for_deleted_entry(STATE_MLG_MEMBER_TABLE, "Ethernet4")

    def test_linkup_delay(self, dvs, testlog):
        """link-up-delay=3: state is pending immediately after recovery, up after delay elapses."""
        self.setup_dbs(dvs)

        self.set_port_oper("Ethernet0", "down")
        self.set_port_oper("Ethernet4", "up")

        self.create_group("ml_delay", ["Ethernet0"], ["Ethernet4"], linkup_delay=3)
        self.wait_group_state("ml_delay", "down")
        self.wait_member_state("Ethernet4", "force_down")

        # Uplink comes back; daemon starts 3s timer and writes state=pending
        self.set_port_oper("Ethernet0", "up")

        # T+1s: timer not yet elapsed → state must be pending
        time.sleep(1)
        fvs = self.sdb.get_entry(STATE_MLG_STATE_TABLE, "ml_delay")
        assert fvs.get("state") == "pending", \
            f"Expected pending at T+1s but got state={fvs.get('state')}"

        # After delay elapses (timer fires within ~4-5s of oper-up): state becomes up
        self.wait_group_state("ml_delay", "up", timeout=10)
        self.wait_member_state("Ethernet4", "allow_up", timeout=10)

        self.delete_group("ml_delay")
        self.sdb.wait_for_deleted_entry(STATE_MLG_STATE_TABLE, "ml_delay")
        self.sdb.wait_for_deleted_entry(STATE_MLG_MEMBER_TABLE, "Ethernet4")

    def test_config_admin_down_respected(self, dvs, testlog):
        """When config admin_status=down, monitor clearing allow_up must not raise ASIC to true."""
        self.setup_dbs(dvs)

        self.set_port_cfg_admin("Ethernet4", "down")
        self.set_port_oper("Ethernet0", "down")
        self.set_port_oper("Ethernet4", "up")

        self.create_group("ml_cfgdown", ["Ethernet0"], ["Ethernet4"])
        self.wait_group_state("ml_cfgdown", "down")
        self.wait_member_state("Ethernet4", "force_down")
        self.wait_asic_admin("Ethernet4", False)

        # Uplink recovers: monitor releases Ethernet4 (allow_up), but config says down
        self.set_port_oper("Ethernet0", "up")
        self.wait_group_state("ml_cfgdown", "up")
        self.wait_member_state("Ethernet4", "allow_up")
        # Config admin_status=down must hold: ASIC stays false
        self.wait_asic_admin("Ethernet4", False)

        self.delete_group("ml_cfgdown")
        self.sdb.wait_for_deleted_entry(STATE_MLG_STATE_TABLE, "ml_cfgdown")
        self.sdb.wait_for_deleted_entry(STATE_MLG_MEMBER_TABLE, "Ethernet4")
        self.set_port_cfg_admin("Ethernet4", "up")
        self.wait_asic_admin("Ethernet4", True)

    def test_cross_role_cascade(self, dvs, testlog):
        """Chain topology: Eth0→uplink_A, Eth4→downlink_A+uplink_B, Eth8→downlink_B.
        Eth0 down cascades through to Eth8; recovery cascades back up."""
        self.setup_dbs(dvs)

        for port in ("Ethernet0", "Ethernet4", "Ethernet8"):
            self.set_port_cfg_admin(port, "up")
            self.set_port_oper(port, "up")
        self.wait_asic_admin("Ethernet8", True)

        self.create_group("ml_chain_A", ["Ethernet0"], ["Ethernet4"])
        self.create_group("ml_chain_B", ["Ethernet4"], ["Ethernet8"])
        self.wait_group_state("ml_chain_A", "up")
        self.wait_group_state("ml_chain_B", "up")
        self.wait_member_state("Ethernet4", "allow_up")
        self.wait_member_state("Ethernet8", "allow_up")

        # Cascade down: Eth0 → group_A down → Eth4 force_down
        self.set_port_oper("Ethernet0", "down")
        self.wait_group_state("ml_chain_A", "down")
        self.wait_member_state("Ethernet4", "force_down")
        self.wait_asic_admin("Ethernet4", False)

        # Simulate portsyncd propagating Eth4 admin-down → oper-down → group_B down → Eth8 force_down
        self.set_port_oper("Ethernet4", "down")
        self.wait_group_state("ml_chain_B", "down")
        self.wait_member_state("Ethernet8", "force_down")
        self.wait_asic_admin("Ethernet8", False)

        # Cascade recovery: Eth0 up → group_A up → Eth4 allow_up
        self.set_port_oper("Ethernet0", "up")
        self.wait_group_state("ml_chain_A", "up")
        self.wait_member_state("Ethernet4", "allow_up")
        self.wait_asic_admin("Ethernet4", True)

        # Simulate portsyncd propagating Eth4 admin-up → oper-up → group_B up → Eth8 allow_up
        self.set_port_oper("Ethernet4", "up")
        self.wait_group_state("ml_chain_B", "up")
        self.wait_member_state("Ethernet8", "allow_up")
        self.wait_asic_admin("Ethernet8", True)

        # Delete B first so Eth4 (downlink_A, uplink_B) loses uplink_B; then delete A releases Eth4
        self.delete_group("ml_chain_B")
        self.delete_group("ml_chain_A")
        self.sdb.wait_for_deleted_entry(STATE_MLG_STATE_TABLE, "ml_chain_A")
        self.sdb.wait_for_deleted_entry(STATE_MLG_STATE_TABLE, "ml_chain_B")
        self.sdb.wait_for_deleted_entry(STATE_MLG_MEMBER_TABLE, "Ethernet4")
        self.sdb.wait_for_deleted_entry(STATE_MLG_MEMBER_TABLE, "Ethernet8")


# Dummy always-pass test guards against module teardown on final-test flaky retry
def test_nonflaky_dummy():
    pass
