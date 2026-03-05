"""
Generic helper functions for gearbox testing.

This module provides reusable utility functions for gearbox-related tests,
including port management and configuration setup.
"""

import json


class TestGearboxHelper:
    """Helper class for gearbox-related test operations."""

    @staticmethod
    def get_first_gearbox_port(gearbox):
        """
        Get the first port from Gearbox object (reads from _GEARBOX_TABLE in APPL_DB).

        Args:
            gearbox: Gearbox fixture

        Returns:
            tuple: (port_name, phy_id) - First available gearbox port and its PHY ID
        """
        assert len(gearbox.interfaces) > 0, "No interfaces found in gearbox"

        # Get first interface
        first_idx = next(iter(gearbox.interfaces))
        first_intf = gearbox.interfaces[first_idx]

        port_name = first_intf.get("name")
        phy_id = first_intf.get("phy_id")

        assert port_name, "First interface has no 'name' field"
        assert phy_id is not None, "First interface has no 'phy_id' field"

        return port_name, phy_id

    @staticmethod
    def configure_gearbox_macsec_support(dvs, gearbox, phy_id=None, macsec_supported=None):
        """
        Configure MACsec support on a gearbox PHY by modifying gearbox_config.json and restarting DVS.

        This is necessary because:
        1. gearsyncd reads gearbox_config.json only at startup
        2. PortsOrch caches _GEARBOX_TABLE only at startup (initGearbox)
        3. MACsecOrch reads from PortsOrch's cache, not from _GEARBOX_TABLE
        4. Full DVS restart is the only reliable way to reload the configuration
           because partial service restarts cause inconsistent port state

        Args:
            dvs: Docker Virtual Switch instance
            gearbox: Gearbox fixture
            phy_id: PHY ID (string, e.g., "1"). If None, uses the first PHY from Gearbox object.
            macsec_supported: None (remove field), True, or False
        """
        # If phy_id not provided, use the first PHY from Gearbox object
        if phy_id is None:
            assert len(gearbox.phys) > 0, "No PHYs found in gearbox"
            phy_id = next(iter(gearbox.phys))
            print(f"No phy_id provided, using first PHY: {phy_id}")

        # Resolve symlink to get actual config path
        config_path = "/usr/share/sonic/hwsku/gearbox_config.json"
        rc, actual_path = dvs.runcmd(f"readlink -f {config_path}")
        if rc == 0 and actual_path.strip():
            config_path = actual_path.strip()

        # Read current config
        rc, config_json = dvs.runcmd(f"cat {config_path}")
        assert rc == 0, f"Failed to read gearbox_config.json from {config_path}"
        config = json.loads(config_json)

        phy_id = int(phy_id)

        # Find and modify the PHY configuration
        phy_found = False
        for phy in config.get("phys", []):
            if phy.get("phy_id") == phy_id:
                phy_found = True
                if macsec_supported is None:
                    # Remove the field if it exists
                    if "macsec_supported" in phy:
                        del phy["macsec_supported"]
                else:
                    # Set the field
                    phy["macsec_supported"] = macsec_supported
                break

        assert phy_found, f"PHY {phy_id} not found in gearbox_config.json"

        # Write modified config back using heredoc
        config_str = json.dumps(config, indent=2)
        heredoc = "__GEARBOX_JSON__"
        rc, _ = dvs.runcmd(
            "bash -lc 'cat > {path} <<\"{tag}\"\n{payload}\n{tag}\n'".format(
                path=config_path,
                tag=heredoc,
                payload=config_str,
            )
        )
        assert rc == 0, f"Failed to write modified config to {config_path}"

        # Restart DVS to reload configuration
        dvs.restart()
