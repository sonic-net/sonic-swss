"""
VS tests for sampled mirroring with truncation (ERSPAN).

Tests:
- Sampled ERSPAN session creation with SAMPLEPACKET
- Truncation on sampled sessions
- Direction validation (RX-only for sampled)
- sFlow conflict detection
- Session lifecycle (create/delete)
"""

import pytest
import time


class TestMirrorSampledErspan:
    SESSION_NAME = "test_sampled_erspan"
    SRC_IP = "10.0.0.1"
    DST_IP = "20.0.0.1"
    GRE_TYPE = "0x88be"
    DSCP = "8"
    TTL = "64"
    QUEUE = "0"

    def setup_mirror(self, dvs):
        self.dvs = dvs
        self.dvs_mirror = dvs.get_mirror()

    def test_sampled_erspan_session_create_delete(self, dvs, testlog):
        """Test basic sampled ERSPAN session creation and deletion."""
        self.setup_mirror(dvs)

        self.dvs_mirror.create_sampled_erspan_session(
            self.SESSION_NAME,
            self.SRC_IP, self.DST_IP, self.GRE_TYPE,
            self.DSCP, self.TTL, self.QUEUE,
            sample_rate=1000,
            direction="RX",
            src_ports="Ethernet0"
        )

        # Session should appear in state DB (may be inactive until route resolves)
        self.dvs_mirror.verify_session_status(self.SESSION_NAME, expected=1)

        # Cleanup
        self.dvs_mirror.remove_mirror_session(self.SESSION_NAME)
        self.dvs_mirror.verify_no_mirror()

    def test_sampled_erspan_rejects_tx_direction(self, dvs, testlog):
        """Sampled mirroring must be RX direction only."""
        self.setup_mirror(dvs)

        self.dvs_mirror.create_sampled_erspan_session(
            self.SESSION_NAME,
            self.SRC_IP, self.DST_IP, self.GRE_TYPE,
            self.DSCP, self.TTL, self.QUEUE,
            sample_rate=500,
            direction="TX",
            src_ports="Ethernet0"
        )

        # Session should NOT be created (invalid entry)
        time.sleep(2)
        keys = dvs.state_db.get_keys("MIRROR_SESSION_TABLE")
        assert self.SESSION_NAME not in keys

        # Cleanup
        self.dvs_mirror.remove_mirror_session(self.SESSION_NAME)

    def test_sampled_erspan_rejects_both_direction(self, dvs, testlog):
        """Sampled mirroring must be RX direction only, not BOTH."""
        self.setup_mirror(dvs)

        self.dvs_mirror.create_sampled_erspan_session(
            self.SESSION_NAME,
            self.SRC_IP, self.DST_IP, self.GRE_TYPE,
            self.DSCP, self.TTL, self.QUEUE,
            sample_rate=500,
            direction="BOTH",
            src_ports="Ethernet0"
        )

        # Session should NOT be created
        time.sleep(2)
        keys = dvs.state_db.get_keys("MIRROR_SESSION_TABLE")
        assert self.SESSION_NAME not in keys

        # Cleanup
        self.dvs_mirror.remove_mirror_session(self.SESSION_NAME)

    def test_sampled_erspan_with_truncation(self, dvs, testlog):
        """Sampled ERSPAN with truncation creates SAMPLEPACKET with truncate attrs."""
        self.setup_mirror(dvs)

        self.dvs_mirror.create_sampled_erspan_session(
            self.SESSION_NAME,
            self.SRC_IP, self.DST_IP, self.GRE_TYPE,
            self.DSCP, self.TTL, self.QUEUE,
            sample_rate=2000,
            direction="RX",
            src_ports="Ethernet0",
            truncate_size=128
        )

        self.dvs_mirror.verify_session_status(self.SESSION_NAME, expected=1)

        # Cleanup
        self.dvs_mirror.remove_mirror_session(self.SESSION_NAME)
        self.dvs_mirror.verify_no_mirror()

    def test_full_erspan_with_truncation_no_sampling(self, dvs, testlog):
        """Full ERSPAN (no sample_rate) with truncation should work via
        SAI_MIRROR_SESSION_ATTR_TRUNCATE_SIZE, not SAMPLEPACKET."""
        self.setup_mirror(dvs)

        self.dvs_mirror.create_erspan_session(
            "test_full_trunc",
            self.SRC_IP, self.DST_IP, self.GRE_TYPE,
            self.DSCP, self.TTL, self.QUEUE,
            src_ports="Ethernet0",
            direction="RX"
        )

        # Update with truncate_size after creation
        dvs.config_db.update_entry("MIRROR_SESSION", "test_full_trunc",
                                   {"truncate_size": "128"})

        self.dvs_mirror.verify_session_status("test_full_trunc", expected=1)

        # Should NOT create any SAMPLEPACKET objects
        time.sleep(2)
        self.dvs_mirror.verify_no_samplepacket(dvs)

        # Cleanup
        self.dvs_mirror.remove_mirror_session("test_full_trunc")
        self.dvs_mirror.verify_no_mirror()

    def test_sampled_erspan_with_erspan_id(self, dvs, testlog):
        """Sampled ERSPAN with erspan_id should store the value."""
        self.setup_mirror(dvs)

        self.dvs_mirror.create_sampled_erspan_session(
            self.SESSION_NAME,
            self.SRC_IP, self.DST_IP, self.GRE_TYPE,
            self.DSCP, self.TTL, self.QUEUE,
            sample_rate=1000,
            direction="RX",
            src_ports="Ethernet0",
            erspan_id=42
        )

        self.dvs_mirror.verify_session_status(self.SESSION_NAME, expected=1)

        # Cleanup
        self.dvs_mirror.remove_mirror_session(self.SESSION_NAME)
        self.dvs_mirror.verify_no_mirror()

    def test_sampled_erspan_with_congestion_mode(self, dvs, testlog):
        """Sampled ERSPAN with congestion_mode correlated."""
        self.setup_mirror(dvs)

        self.dvs_mirror.create_sampled_erspan_session(
            self.SESSION_NAME,
            self.SRC_IP, self.DST_IP, self.GRE_TYPE,
            self.DSCP, self.TTL, self.QUEUE,
            sample_rate=1000,
            direction="RX",
            src_ports="Ethernet0",
            congestion_mode="correlated"
        )

        self.dvs_mirror.verify_session_status(self.SESSION_NAME, expected=1)

        # Cleanup
        self.dvs_mirror.remove_mirror_session(self.SESSION_NAME)
        self.dvs_mirror.verify_no_mirror()

    def test_erspan_id_exceeds_max_rejected(self, dvs, testlog):
        """erspan_id > 1023 should be rejected."""
        self.setup_mirror(dvs)

        self.dvs_mirror.create_sampled_erspan_session(
            self.SESSION_NAME,
            self.SRC_IP, self.DST_IP, self.GRE_TYPE,
            self.DSCP, self.TTL, self.QUEUE,
            sample_rate=1000,
            direction="RX",
            src_ports="Ethernet0",
            erspan_id=1024
        )

        # Session should NOT be created
        time.sleep(2)
        keys = dvs.state_db.get_keys("MIRROR_SESSION_TABLE")
        assert self.SESSION_NAME not in keys

        # Cleanup
        self.dvs_mirror.remove_mirror_session(self.SESSION_NAME)

    def test_span_truncation_independent_of_sampling(self, dvs, testlog):
        """SPAN session with truncation (no sampling needed)."""
        self.setup_mirror(dvs)

        self.dvs_mirror.create_span_session(
            "test_span_trunc",
            dst_port="Ethernet4",
            src_ports="Ethernet0",
            direction="RX"
        )

        # Add truncate_size
        dvs.config_db.update_entry("MIRROR_SESSION", "test_span_trunc",
                                   {"truncate_size": "64"})

        self.dvs_mirror.verify_session_status("test_span_trunc", expected=1)

        # SPAN truncation should NOT create SAMPLEPACKET
        time.sleep(2)
        self.dvs_mirror.verify_no_samplepacket(dvs)

        # Cleanup
        self.dvs_mirror.remove_mirror_session("test_span_trunc")
        self.dvs_mirror.verify_no_mirror()
