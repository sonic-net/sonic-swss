# This test suite covers the functionality of sampled port mirroring with truncation on ERSPAN sessions
import pytest

@pytest.mark.usefixtures("testlog")
@pytest.mark.usefixtures('dvs_vlan_manager')
@pytest.mark.usefixtures('dvs_lag_manager')
@pytest.mark.usefixtures('dvs_mirror_manager')
class TestSampledMirror(object):

    def _setup_mirror_session(self, dvs, session, src_ports, sample_rate=None, truncate_size=None):
        """Helper to set up network and create a sampled ERSPAN mirror session"""
        dvs.setup_db()
        pmap = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        pmap = dict(pmap)

        # create sampled mirror session
        self.dvs_mirror.create_erspan_session_sampled(
            session, "1.1.1.1", "2.2.2.2", "0x8949", "8", "64", "0",
            src_ports=src_ports, direction="RX",
            sample_rate=sample_rate, truncate_size=truncate_size)

        # Session starts inactive until route exists
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # Bring up port and create route to dst_ip
        dvs.set_interface_status("Ethernet16", "up")
        dvs.add_ip_address("Ethernet16", "10.0.0.0/30")
        dvs.add_neighbor("Ethernet16", "10.0.0.1", "02:04:06:08:10:12")
        dvs.add_route("2.2.2.2", "10.0.0.1")
        self.dvs_mirror.verify_session_status(session, status="active")

        return pmap

    def _teardown_mirror_session(self, dvs, session):
        """Helper to tear down network and remove mirror session"""
        dvs.remove_route("2.2.2.2")
        dvs.remove_neighbor("Ethernet16", "10.0.0.1")
        dvs.remove_ip_address("Ethernet16", "10.0.0.0/30")
        dvs.set_interface_status("Ethernet16", "down")
        self.dvs_mirror.remove_mirror_session(session)
        self.dvs_mirror.verify_no_mirror()

    def test_SampledMirrorCreateRemove(self, dvs, testlog):
        """
        Test sampled ERSPAN mirror session create and remove.
        Verify SAMPLEPACKET object is created in ASIC_DB with correct attributes.
        """
        session = "SAMPLED_SESSION"
        pmap = self._setup_mirror_session(dvs, session, "Ethernet12", sample_rate="50000")

        # Verify SAMPLEPACKET object exists in ASIC_DB
        samplepacket_keys = dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1)
        samplepacket_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", samplepacket_keys[0])

        # Verify correct attributes
        assert samplepacket_entry["SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE"] == "50000"
        assert samplepacket_entry["SAI_SAMPLEPACKET_ATTR_TYPE"] == "SAI_SAMPLEPACKET_TYPE_MIRROR_SESSION"

        # Verify port attributes
        fvs = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        fvs = dict(fvs)
        port_oid = fvs.get("Ethernet12")
        port_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
        assert "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE" in port_entry
        assert "SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION" in port_entry

        # Remove and verify cleanup
        self._teardown_mirror_session(dvs, session)
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)

    def test_SampledMirrorWithTruncation(self, dvs, testlog):
        """
        Test sampled ERSPAN mirror session with truncation.
        Verify truncation attributes are set on SAMPLEPACKET object.
        """
        session = "SAMPLED_TRUNCATE_SESSION"
        pmap = self._setup_mirror_session(dvs, session, "Ethernet12",
                                          sample_rate="50000", truncate_size="128")

        # Verify SAMPLEPACKET with truncation
        samplepacket_keys = dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1)
        samplepacket_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", samplepacket_keys[0])

        assert samplepacket_entry["SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE"] == "50000"
        assert samplepacket_entry["SAI_SAMPLEPACKET_ATTR_TYPE"] == "SAI_SAMPLEPACKET_TYPE_MIRROR_SESSION"
        assert samplepacket_entry["SAI_SAMPLEPACKET_ATTR_TRUNCATE_ENABLE"] == "true"
        assert samplepacket_entry["SAI_SAMPLEPACKET_ATTR_TRUNCATE_SIZE"] == "128"

        self._teardown_mirror_session(dvs, session)

    def test_NoSamplePacketWhenSampleRateAbsent(self, dvs, testlog):
        """
        Test backward compatibility: no SAMPLEPACKET created when sample_rate is not configured.
        """
        session = "FULL_MIRROR_SESSION"

        dvs.setup_db()
        # Create regular ERSPAN session without sample_rate
        self.dvs_mirror.create_erspan_session(
            session, "1.1.1.1", "2.2.2.2", "0x8949", "8", "64", "0",
            src_ports="Ethernet12", direction="RX")

        dvs.set_interface_status("Ethernet16", "up")
        dvs.add_ip_address("Ethernet16", "10.0.0.0/30")
        dvs.add_neighbor("Ethernet16", "10.0.0.1", "02:04:06:08:10:12")
        dvs.add_route("2.2.2.2", "10.0.0.1")
        self.dvs_mirror.verify_session_status(session, status="active")

        # Verify NO SAMPLEPACKET object in ASIC_DB
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)

        # Verify full mirror path (INGRESS_MIRROR_SESSION, not SAMPLE_MIRROR_SESSION)
        fvs = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        fvs = dict(fvs)
        port_oid = fvs.get("Ethernet12")
        port_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
        assert "SAI_PORT_ATTR_INGRESS_MIRROR_SESSION" in port_entry

        self._teardown_mirror_session(dvs, session)

    def test_SampledMirrorStateDB(self, dvs, testlog):
        """
        Test that sample_rate and truncate_size are written to STATE_DB.
        """
        session = "SAMPLED_STATE_SESSION"
        pmap = self._setup_mirror_session(dvs, session, "Ethernet12",
                                          sample_rate="50000", truncate_size="128")

        # Verify STATE_DB has sample_rate and truncate_size
        state_entry = dvs.state_db.wait_for_entry("MIRROR_SESSION_TABLE", session)
        assert state_entry.get("sample_rate") == "50000"
        assert state_entry.get("truncate_size") == "128"

        self._teardown_mirror_session(dvs, session)

    def test_SampledMirrorCapabilityInStateDB(self, dvs, testlog):
        """
        Test that sampled mirror capability entries exist in STATE_DB.
        """
        dvs.setup_db()

        # Verify capability entries exist
        cap_entry = dvs.state_db.wait_for_entry("SWITCH_CAPABILITY", "switch")
        assert "PORT_INGRESS_SAMPLE_MIRROR_CAPABLE" in cap_entry
        assert "PORT_EGRESS_SAMPLE_MIRROR_CAPABLE" in cap_entry
        assert "SAMPLEPACKET_TRUNCATION_CAPABLE" in cap_entry
