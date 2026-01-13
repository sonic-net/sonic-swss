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
            TestGearboxHelper.configure_gearbox_macsec_support(dvs, gearbox, phy_id=phy_id, macsec_supported=None)
            TestMacsecHelper.enable_macsec_on_port(dvs, port_name=port_name, with_secure_channels=True)

            assert TestMacsecHelper.verify_macsec_in_gb_asic_db(dvs, should_exist=True), (
                "FAILED: MACsec objects should exist in GB_ASIC_DB "
                "when macsec_supported is absent"
            )

            assert TestMacsecHelper.verify_macsec_in_asic_db(dvs, should_exist=False), (
                "FAILED: MACsec objects should NOT exist in ASIC_DB "
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
            TestGearboxHelper.configure_gearbox_macsec_support(dvs, gearbox, phy_id=phy_id, macsec_supported=True)
            TestMacsecHelper.enable_macsec_on_port(dvs, port_name=port_name, with_secure_channels=True)

            assert TestMacsecHelper.verify_macsec_in_gb_asic_db(dvs, should_exist=True), (
                "FAILED: MACsec objects should exist in GB_ASIC_DB "
                "when macsec_supported=true"
            )

            assert TestMacsecHelper.verify_macsec_in_asic_db(dvs, should_exist=False), (
                "FAILED: MACsec objects should NOT exist in ASIC_DB "
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
            TestGearboxHelper.configure_gearbox_macsec_support(dvs, gearbox, phy_id=phy_id, macsec_supported=False)
            TestMacsecHelper.enable_macsec_on_port(dvs, port_name=port_name, with_secure_channels=True)

            assert TestMacsecHelper.verify_macsec_in_asic_db(dvs, should_exist=True), (
                "FAILED: MACsec objects should exist in ASIC_DB "
                "when macsec_supported=false"
            )

            assert TestMacsecHelper.verify_macsec_in_gb_asic_db(dvs, should_exist=False), (
                "FAILED: MACsec objects should NOT exist in GB_ASIC_DB "
                "when macsec_supported=false"
            )

        finally:
            TestMacsecHelper.cleanup_macsec(dvs, port_name)

