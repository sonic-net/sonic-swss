"""
Generic helper functions for MACsec testing.

This module provides reusable utility functions for MACsec-related tests,
including port configuration, secure channel/association management, and verification.
"""

from swsscommon import swsscommon
from dvslib.dvs_database import DVSDatabase
from test_macsec import WPASupplicantMock


class TestMacsecHelper:
    """Helper class for MACsec-related test operations."""

    @staticmethod
    def enable_macsec_on_port(dvs, port_name, with_secure_channels=True):
        """
        Enable MACsec on a port with optional secure channels and associations.

        Args:
            dvs: Docker Virtual Switch instance
            port_name: Port name to enable MACsec on
            with_secure_channels: If True, create Secure Channels and Associations with static keys

        Returns:
            WPASupplicantMock: The WPA supplicant mock instance
        """
        wpa = WPASupplicantMock(dvs)
        wpa.init_macsec_port(port_name)

        # Configure MACsec port with protection and encryption enabled
        wpa.config_macsec_port(port_name, {
            "enable": True,
            "enable_protect": True,
            "enable_encrypt": True,
            "send_sci": True,
        })

        # If requested, create Secure Channels and Associations with static keys
        if with_secure_channels:
            local_mac_address = "00-15-5D-78-FF-C1"
            peer_mac_address = "00-15-5D-78-FF-C2"
            macsec_port_identifier = 1
            an = 0  # Association Number
            sak = "0" * 32  # SAK: 128-bit key (32 hex chars)
            auth_key = "0" * 32  # Auth key: 128-bit key (32 hex chars)
            packet_number = 1
            ssci = 1  # Short SCI
            salt = "0" * 24  # Salt for XPN cipher suites

            # Create Transmit Secure Channel (local) - MUST come first!
            wpa.create_transmit_sc(
                port_name,
                local_mac_address,
                macsec_port_identifier)

            # Create Receive Secure Channel (from peer)
            wpa.create_receive_sc(
                port_name,
                peer_mac_address,
                macsec_port_identifier)

            # Create Receive Secure Association with static keys
            wpa.create_receive_sa(
                port_name,
                peer_mac_address,
                macsec_port_identifier,
                an,
                sak,
                auth_key,
                packet_number,
                ssci,
                salt)

            # Create Transmit Secure Association with static keys
            wpa.create_transmit_sa(
                port_name,
                local_mac_address,
                macsec_port_identifier,
                an,
                sak,
                auth_key,
                packet_number,
                ssci,
                salt)

            # Enable Receive SA
            wpa.set_enable_receive_sa(
                port_name,
                peer_mac_address,
                macsec_port_identifier,
                an,
                True)

            # Enable MACsec control
            wpa.set_macsec_control(port_name, True)

            # Enable Transmit SA
            wpa.set_enable_transmit_sa(
                port_name,
                local_mac_address,
                macsec_port_identifier,
                an,
                True)

        return wpa

    @staticmethod
    def cleanup_macsec(dvs, port_name):
        """
        Cleanup MACsec configuration on a port to prevent test pollution.

        Args:
            dvs: Docker Virtual Switch instance
            port_name: Port name to cleanup
        """
        try:
            wpa = WPASupplicantMock(dvs)
            app_db = dvs.get_app_db()

            # Disable MACsec control first
            wpa.set_macsec_control(port_name, False)

            # Delete all SAs for this port (must delete before SCs)
            for table in ["MACSEC_EGRESS_SA_TABLE", "MACSEC_INGRESS_SA_TABLE"]:
                for key in app_db.get_keys(table):
                    if key.startswith(f"{port_name}:"):
                        app_db.delete_entry(table, key)

            # Delete all SCs for this port
            for table in ["MACSEC_EGRESS_SC_TABLE", "MACSEC_INGRESS_SC_TABLE"]:
                for key in app_db.get_keys(table):
                    if key.startswith(f"{port_name}:"):
                        app_db.delete_entry(table, key)

            # Finally delete the MACsec port entry
            wpa.deinit_macsec_port(port_name)

        except Exception as e:
            print(f"Cleanup encountered error: {e}")

    @staticmethod
    def verify_macsec_for_port_in_gb_asic_db(dvs, port_name, should_exist=True):
        """
        Verify MACsec objects for a specific port exist (or don't exist) in GB_ASIC_DB.

        This method checks if the specified port's MACsec configuration is present
        in GB_ASIC_DB by mapping port name to line-side OID via GB_COUNTERS_DB and
        checking if any MACSEC_PORT entry references that port OID.

        Args:
            dvs: Docker Virtual Switch instance
            port_name: Name of the port to check (e.g., "Ethernet4")
            should_exist: True if objects should exist, False otherwise

        Returns:
            bool: True if verification passes
        """
        gb_asic_db = DVSDatabase(swsscommon.GB_ASIC_DB, dvs.redis_sock)
        gb_counters_db = DVSDatabase(swsscommon.GB_COUNTERS_DB, dvs.redis_sock)

        # Get port's line-side OID from GB_COUNTERS_DB
        # Gearbox ports are stored with "_line" suffix for line-side port
        port_map = gb_counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        line_port_key = f"{port_name}_line"
        expected_port_oid = port_map.get(line_port_key)

        if not expected_port_oid:
            # Port not found in GB_COUNTERS_DB (not a gearbox port)
            return not should_exist

        # Check if any MACSEC_PORT entry references this port OID
        macsec_port_keys = gb_asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_MACSEC_PORT")
        port_found = any(
            gb_asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_MACSEC_PORT", key).get(
                "SAI_MACSEC_PORT_ATTR_PORT_ID"
            ) == expected_port_oid
            for key in macsec_port_keys
        )

        if should_exist:
            return port_found
        else:
            return not port_found

    @staticmethod
    def verify_macsec_for_port_in_asic_db(dvs, port_name, should_exist=True):
        """
        Verify MACsec objects for a specific port exist (or don't exist) in ASIC_DB (NPU).

        This method checks if the specified port's MACsec configuration is present
        in ASIC_DB by mapping port name to OID via COUNTERS_DB and checking if any
        MACSEC_PORT entry references that port OID.

        Args:
            dvs: Docker Virtual Switch instance
            port_name: Name of the port to check (e.g., "Ethernet0")
            should_exist: True if objects should exist, False otherwise

        Returns:
            bool: True if verification passes
        """
        asic_db = dvs.get_asic_db()
        counters_db = dvs.get_counters_db()

        # Get port OID from COUNTERS_DB port name map
        port_map = counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        expected_port_oid = port_map.get(port_name)

        if not expected_port_oid:
            # Port not found in COUNTERS_DB
            return not should_exist

        # Check if any MACSEC_PORT entry references this port OID
        macsec_port_keys = asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_MACSEC_PORT")
        port_found = any(
            asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_MACSEC_PORT", key).get(
                "SAI_MACSEC_PORT_ATTR_PORT_ID"
            ) == expected_port_oid
            for key in macsec_port_keys
        )

        if should_exist:
            return port_found
        else:
            return not port_found

