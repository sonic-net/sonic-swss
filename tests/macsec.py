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
    def verify_macsec_in_gb_asic_db(dvs, should_exist=True):
        """
        Verify MACsec objects exist (or don't exist) in GB_ASIC_DB

        Args:
            dvs: Docker Virtual Switch instance
            should_exist: True if objects should exist, False otherwise

        Returns:
            bool: True if verification passes
        """

        gb_asic_db = DVSDatabase(swsscommon.GB_ASIC_DB, dvs.redis_sock)

        macsec_keys = gb_asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_MACSEC")

        if should_exist:
            return len(macsec_keys) > 0  # Should have at least one object
        else:
            return len(macsec_keys) == 0  # Should have no objects

    @staticmethod
    def verify_macsec_in_asic_db(dvs, should_exist=True):
        """
        Verify MACsec objects exist (or don't exist) in ASIC_DB (NPU)

        Args:
            dvs: Docker Virtual Switch instance
            should_exist: True if objects should exist, False otherwise

        Returns:
            bool: True if verification passes
        """
        asic_db = dvs.get_asic_db()

        macsec_keys = asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_MACSEC")

        if should_exist:
            return len(macsec_keys) > 0
        else:
            return len(macsec_keys) == 0

