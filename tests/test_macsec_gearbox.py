import pytest

from test_gearbox import Gearbox
from gearbox import TestGearboxHelper
from macsec import TestMacsecHelper

DVS_ENV = ["HWSKU=brcm_gearbox_vs"]

@pytest.fixture(scope="module")
def gearbox(dvs):
    return Gearbox(dvs)

@pytest.fixture(scope="module")
def gearbox_config(dvs):
    """
    Backup and restore gearbox_config.json once for all tests in the module.

    Backup happens before the first test, restore happens after the last test.
    """
    # Resolve symlink to get actual config path
    config_path = "/usr/share/sonic/hwsku/gearbox_config.json"
    rc, actual_path = dvs.runcmd(f"readlink -f {config_path}")
    if rc == 0 and actual_path.strip():
        config_path = actual_path.strip()

    # Backup original config (once at start)
    dvs.runcmd(f"cp {config_path} {config_path}.bak")

    yield config_path

    # Restore original config (once at end)
    dvs.runcmd(f"mv {config_path}.bak {config_path}")

class TestMacsecGearbox(object):

    def test_macsec_phy_switch_default(self, dvs, gearbox, gearbox_config):
        """
        When macsec_supported field is ABSENT (not specified), the system should:
        1. Default to using PHY switch for MACsec
        2. Create MACsec objects in GB_ASIC_DB (not ASIC_DB)
        3. This preserves backward compatibility with existing platforms

        Args:
            dvs: Docker Virtual Switch instance (pytest fixture)
            gearbox: Gearbox fixture
            gearbox_config: Gearbox config fixture (auto backup/restore)
        """
        # Derive port and phy_id from Gearbox object
        port_name, phy_id = TestGearboxHelper.get_first_gearbox_port(gearbox)

        try:
            TestGearboxHelper.configure_gearbox_macsec_support(dvs, gearbox, phy_id=phy_id, macsec_supported=None, restart=True)
            TestMacsecHelper.enable_macsec_on_port(dvs, port_name=port_name, with_secure_channels=True)

            assert TestMacsecHelper.verify_macsec_for_port_in_gb_asic_db(dvs, port_name, should_exist=True), (
                f"FAILED: MACsec objects for {port_name} should exist in GB_ASIC_DB "
                "when macsec_supported is absent"
            )

            assert TestMacsecHelper.verify_macsec_for_port_in_asic_db(dvs, port_name, should_exist=False), (
                f"FAILED: MACsec objects for {port_name} should NOT exist in ASIC_DB "
                "when using PHY backend"
            )

        finally:
            TestMacsecHelper.cleanup_macsec(dvs, port_name)

    def test_macsec_phy_switch_explicit(self, dvs, gearbox, gearbox_config):
        """
        When macsec_supported field is explicitly set to TRUE, the system should:
        1. Use PHY switch for MACsec (same as default)
        2. Create MACsec objects in GB_ASIC_DB (not ASIC_DB)
        3. This is the explicit way to declare PHY MACsec support

        Args:
            dvs: Docker Virtual Switch instance (pytest fixture)
            gearbox: Gearbox fixture
            gearbox_config: Gearbox config fixture (auto backup/restore)
        """
        # Derive port and phy_id from Gearbox object
        port_name, phy_id = TestGearboxHelper.get_first_gearbox_port(gearbox)

        try:
            TestGearboxHelper.configure_gearbox_macsec_support(dvs, gearbox, phy_id=phy_id, macsec_supported=True, restart=True)
            TestMacsecHelper.enable_macsec_on_port(dvs, port_name=port_name, with_secure_channels=True)

            assert TestMacsecHelper.verify_macsec_for_port_in_gb_asic_db(dvs, port_name, should_exist=True), (
                f"FAILED: MACsec objects for {port_name} should exist in GB_ASIC_DB "
                "when macsec_supported=true"
            )

            assert TestMacsecHelper.verify_macsec_for_port_in_asic_db(dvs, port_name, should_exist=False), (
                f"FAILED: MACsec objects for {port_name} should NOT exist in ASIC_DB "
                "when using PHY backend"
            )

        finally:
            TestMacsecHelper.cleanup_macsec(dvs, port_name)

    def test_macsec_npu_switch(self, dvs, gearbox, gearbox_config):
        """
        Test MACsec NPU backend selection when macsec_supported=false.

        1. When a gearbox PHY has macsec_supported=false in _GEARBOX_TABLE,
        2. MACsec objects should be created in ASIC_DB (NPU backend), not in
           GB_ASIC_DB (PHY backend).

        Args:
            dvs: Docker Virtual Switch instance (pytest fixture)
            gearbox: Gearbox fixture
            gearbox_config: Gearbox config fixture (auto backup/restore)
        """
        # Derive port and phy_id from Gearbox object
        port_name, phy_id = TestGearboxHelper.get_first_gearbox_port(gearbox)

        try:
            # Setup gearbox with macsec_supported=false
            TestGearboxHelper.configure_gearbox_macsec_support(dvs, gearbox, phy_id=phy_id, macsec_supported=False, restart=True)
            TestMacsecHelper.enable_macsec_on_port(dvs, port_name=port_name, with_secure_channels=True)

            assert TestMacsecHelper.verify_macsec_for_port_in_asic_db(dvs, port_name, should_exist=True), (
                f"FAILED: MACsec objects for {port_name} should exist in ASIC_DB "
                "when macsec_supported=false"
            )

            assert TestMacsecHelper.verify_macsec_for_port_in_gb_asic_db(dvs, port_name, should_exist=False), (
                f"FAILED: MACsec objects for {port_name} should NOT exist in GB_ASIC_DB "
                "when macsec_supported=false"
            )

        finally:
            TestMacsecHelper.cleanup_macsec(dvs, port_name)

    def test_macsec_mixed_phy_support(self, dvs, gearbox, gearbox_config):
        """
        Test mixed MACsec support across multiple PHYs.

        This test validates the scenario where a platform owner enables macsec_supported
        only for some gearbox PHYs, not all of them.

        Args:
            dvs: Docker Virtual Switch instance (pytest fixture)
            gearbox: Gearbox fixture
            gearbox_config: Gearbox config fixture (auto backup/restore)
        """
        # Reassign last interface from PHY 1 to PHY 2
        phy1_interfaces = [
            intf.get("name") for intf in gearbox.interfaces.values()
            if str(intf.get("phy_id")) == "1"
        ]
        interface_to_reassign = phy1_interfaces[-1]

        # Reassign interface to PHY 2 with macsec_supported=False
        TestGearboxHelper.reassign_interface_to_phy(
            dvs, interface_name=interface_to_reassign, new_phy_id=2, macsec_supported=False
        )

        # Set macsec_supported=True for PHY 1 and restart DVS
        TestGearboxHelper.configure_gearbox_macsec_support(
            dvs, gearbox, phy_id=1, macsec_supported=True, restart=True
        )

        gearbox_reloaded = Gearbox(dvs)

        port1_name, _ = TestGearboxHelper.get_gearbox_port_by_phy(gearbox_reloaded, phy_id=1)
        port2_name = interface_to_reassign

        try:
            TestMacsecHelper.enable_macsec_on_port(dvs, port_name=port1_name, with_secure_channels=True)
            TestMacsecHelper.enable_macsec_on_port(dvs, port_name=port2_name, with_secure_channels=True)

            # Verify MACsec keys are created in correct databases
            # PHY 1 (macsec_supported=true) -> keys should be in GB_ASIC_DB ONLY
            assert TestMacsecHelper.verify_macsec_for_port_in_gb_asic_db(dvs, port1_name, should_exist=True), (
                f"FAILED: MACsec keys for {port1_name} (PHY 1) should exist in GB_ASIC_DB "
                "when macsec_supported=true"
            )
            assert TestMacsecHelper.verify_macsec_for_port_in_asic_db(dvs, port1_name, should_exist=False), (
                f"FAILED: MACsec keys for {port1_name} (PHY 1) should NOT exist in ASIC_DB "
                "when macsec_supported=true (should be in GB_ASIC_DB only)"
            )

            # PHY 2 (macsec_supported=false) -> keys should be in ASIC_DB ONLY
            assert TestMacsecHelper.verify_macsec_for_port_in_asic_db(dvs, port2_name, should_exist=True), (
                f"FAILED: MACsec keys for {port2_name} (PHY 2) should exist in ASIC_DB "
                "when macsec_supported=false"
            )
            assert TestMacsecHelper.verify_macsec_for_port_in_gb_asic_db(dvs, port2_name, should_exist=False), (
                f"FAILED: MACsec keys for {port2_name} (PHY 2) should NOT exist in GB_ASIC_DB "
                "when macsec_supported=false (should be in ASIC_DB only)"
            )

        finally:
            TestMacsecHelper.cleanup_macsec(dvs, port1_name)
            TestMacsecHelper.cleanup_macsec(dvs, port2_name)

