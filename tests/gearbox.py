"""
Generic helper functions for gearbox testing.

This module provides reusable utility functions for gearbox-related tests,
including port management and configuration setup.
"""

import json
import base64


class TestGearboxHelper:
    """Helper class for gearbox-related test operations."""

    @staticmethod
    def load_gearbox_config(dvs):
        """
        Load gearbox configuration from gearbox_config.json.

        Args:
            dvs: Docker Virtual Switch instance

        Returns:
            tuple: (config_path, config_dict) - Path to the config file and parsed JSON config
        """
        # Resolve symlink to get actual config path
        config_path = "/usr/share/sonic/hwsku/gearbox_config.json"
        rc, actual_path = dvs.runcmd(f"readlink -f {config_path}")
        if rc == 0 and actual_path.strip():
            config_path = actual_path.strip()

        # Read current config
        rc, config_json = dvs.runcmd(f"cat {config_path}")
        assert rc == 0, f"Failed to read gearbox_config.json from {config_path}"
        config = json.loads(config_json)

        return config_path, config

    @staticmethod
    def write_gearbox_config(dvs, config_path, config_dict):
        """
        Write gearbox configuration to gearbox_config.json.

        Args:
            dvs: Docker Virtual Switch instance
            config_path: Path to the config file
            config_dict: Configuration dictionary to write

        Returns:
            None
        """
        config_str = json.dumps(config_dict, indent=2)
        encoded = base64.b64encode(config_str.encode('utf-8')).decode('utf-8')
        cmd = f"python3 -c \"import base64; data = base64.b64decode('{encoded}').decode('utf-8'); open('{config_path}', 'w').write(data + '\\n')\""
        rc, _ = dvs.runcmd(cmd)
        assert rc == 0, f"Failed to write modified config to {config_path}"

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
    def get_gearbox_port_by_phy(gearbox, phy_id):
        """
        Get a port connected to a specific PHY from Gearbox object.

        Args:
            gearbox: Gearbox fixture
            phy_id: PHY ID to search for

        Returns:
            tuple: (port_name, phy_id) - Port connected to the specified PHY
        """
        for idx, intf in gearbox.interfaces.items():
            if int(intf.get("phy_id")) == phy_id:
                port_name = intf.get("name")
                assert port_name, f"Interface on PHY {phy_id} has no 'name' field"
                return port_name, phy_id

        raise AssertionError(f"No interface found connected to PHY {phy_id}")

    @staticmethod
    def reassign_interface_to_phy(dvs, interface_name, new_phy_id, macsec_supported=None, restart=False):
        """
        Reassign an existing interface to a different PHY for testing multi-PHY scenarios.

        This creates a new PHY (if it doesn't exist) and reassigns the specified interface
        to that PHY. The interface keeps its original lane configuration which is valid in VS.

        Args:
            dvs: Docker Virtual Switch instance
            interface_name: The interface to reassign (e.g., "Ethernet8")
            new_phy_id: The PHY ID to reassign the interface to (e.g., 2)
            macsec_supported: MACsec support for the new PHY (None=omit field, True, or False)
            restart: If True, restart DVS after modifying the config (default: False)

        Returns:
            None
        """
        config_path, config = TestGearboxHelper.load_gearbox_config(dvs)

        # Check if PHY already exists, if not create it
        phy_ids = [int(phy.get("phy_id")) for phy in config.get("phys", [])]
        new_phy_id = int(new_phy_id)
        if new_phy_id not in phy_ids:
            # Get the first PHY as a template
            assert len(config.get("phys", [])) > 0, "No PHYs found in config"
            first_phy = config["phys"][0]

            # Create new PHY based on first PHY, reusing file paths that exist in VS
            new_phy = {
                "phy_id": new_phy_id,
                "name": f"sesto-{new_phy_id}",
                "address": f"0x{new_phy_id}000",
                "lib_name": first_phy.get("lib_name"),  # Reuse PHY 1's lib
                "firmware_path": first_phy.get("firmware_path"),  # Reuse PHY 1's firmware
                "config_file": first_phy.get("config_file"),  # Reuse PHY 1's config
                "sai_init_config_file": first_phy.get("sai_init_config_file"),  # Reuse PHY 1's init config
                "phy_access": first_phy.get("phy_access", "mdio"),
                "bus_id": first_phy.get("bus_id", 0),
                "context_id": first_phy.get("context_id", 1),
                "hwinfo": first_phy.get("hwinfo", "mdio0_0_0/0")  # Reuse PHY 1's hwinfo
            }

            # Set macsec_supported if specified
            if macsec_supported is not None:
                new_phy["macsec_supported"] = macsec_supported

            config["phys"].append(new_phy)

        # Find and reassign the interface (keep original lanes)
        interface_found = False
        for intf in config.get("interfaces", []):
            if intf.get("name") == interface_name:
                intf["phy_id"] = new_phy_id
                interface_found = True
                break

        assert interface_found, f"Interface {interface_name} not found in gearbox_config.json"

        TestGearboxHelper.write_gearbox_config(dvs, config_path, config)

        if restart:
            dvs.restart()

    @staticmethod
    def configure_gearbox_macsec_support(dvs, gearbox, phy_id=None, macsec_supported=None, restart=False):
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
            restart: If True, restart DVS after modifying the config (default: False)
        """
        # If phy_id not provided, use the first PHY from Gearbox object
        if phy_id is None:
            assert len(gearbox.phys) > 0, "No PHYs found in gearbox"
            phy_id = next(iter(gearbox.phys))
            print(f"No phy_id provided, using first PHY: {phy_id}")

        config_path, config = TestGearboxHelper.load_gearbox_config(dvs)

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

        TestGearboxHelper.write_gearbox_config(dvs, config_path, config)

        if restart:
            dvs.restart()
