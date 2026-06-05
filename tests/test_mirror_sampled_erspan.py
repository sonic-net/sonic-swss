# This test suite covers the functionality of sampled port mirroring with truncation on ERSPAN sessions
import pytest
import time

@pytest.mark.usefixtures("testlog")
@pytest.mark.usefixtures('dvs_vlan_manager')
@pytest.mark.usefixtures('dvs_lag_manager')
@pytest.mark.usefixtures('dvs_mirror_manager')
class TestSampledMirror(object):

    def _bring_up_route(self, dvs):
        """Bring up the monitor port and install the route to the dst_ip"""
        dvs.set_interface_status("Ethernet16", "up")
        dvs.add_ip_address("Ethernet16", "10.0.0.0/30")
        dvs.add_neighbor("Ethernet16", "10.0.0.1", "02:04:06:08:10:12")
        dvs.add_route("2.2.2.2", "10.0.0.1")

    def _tear_down_route(self, dvs):
        """Remove the route and bring down the monitor port"""
        dvs.remove_route("2.2.2.2")
        dvs.remove_neighbor("Ethernet16", "10.0.0.1")
        dvs.remove_ip_address("Ethernet16", "10.0.0.0/30")
        dvs.set_interface_status("Ethernet16", "down")

    def _setup_mirror_session(self, dvs, session, src_ports, sample_rate=None,
                              truncate_size=None, direction="RX"):
        """Helper to set up network and create a sampled ERSPAN mirror session"""
        dvs.setup_db()

        # create sampled mirror session
        self.dvs_mirror.create_erspan_session_sampled(
            session, "1.1.1.1", "2.2.2.2", "0x8949", "8", "64", "0",
            src_ports=src_ports, direction=direction,
            sample_rate=sample_rate, truncate_size=truncate_size)

        # Session starts inactive until route exists
        dvs.state_db.wait_for_field_match("MIRROR_SESSION_TABLE", session, {"status": "inactive"})

        # Bring up port and create route to dst_ip
        self._bring_up_route(dvs)
        dvs.state_db.wait_for_field_match("MIRROR_SESSION_TABLE", session, {"status": "active"})

    def _teardown_mirror_session(self, dvs, session):
        """Helper to tear down network and remove mirror session"""
        self._tear_down_route(dvs)
        self.dvs_mirror.remove_mirror_session(session)
        dvs.state_db.wait_for_deleted_entry("MIRROR_SESSION_TABLE", session)

    def _assert_sample_mirror_capable(self, dvs, ingress=True, egress=True, truncation=True):
        """
        Assert the sampled-mirror capability values published to STATE_DB.
        """
        cap = dvs.state_db.wait_for_entry("SWITCH_CAPABILITY", "switch")
        assert cap.get("PORT_INGRESS_SAMPLE_MIRROR_CAPABLE") == ("true" if ingress else "false")
        assert cap.get("PORT_EGRESS_SAMPLE_MIRROR_CAPABLE") == ("true" if egress else "false")
        assert cap.get("SAMPLEPACKET_TRUNCATION_CAPABLE") == ("true" if truncation else "false")

    def test_SampledMirrorCreateRemove(self, dvs, testlog):
        """
        Test sampled ERSPAN mirror session create and remove.
        Verify SAMPLEPACKET object is created in ASIC_DB with correct attributes.
        """
        session = "SAMPLED_SESSION"
        self._setup_mirror_session(dvs, session, "Ethernet12", sample_rate="50000")

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
        self._setup_mirror_session(dvs, session, "Ethernet12",
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
        dvs.state_db.wait_for_field_match("MIRROR_SESSION_TABLE", session, {"status": "active"})

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
        self._setup_mirror_session(dvs, session, "Ethernet12",
                                          sample_rate="50000", truncate_size="128")

        # Verify STATE_DB has sample_rate and truncate_size
        state_entry = dvs.state_db.wait_for_entry("MIRROR_SESSION_TABLE", session)
        assert state_entry.get("sample_rate") == "50000"
        assert state_entry.get("truncate_size") == "128"

        self._teardown_mirror_session(dvs, session)

    def test_SampledMirrorCapabilityInStateDB(self, dvs, testlog):
        """
        Test that sampled mirror capability values are published to STATE_DB.
        In the virtual switch all three are expected to be "true"; this also
        guards the precondition for the TX/BOTH sampled mirror VS tests.
        """
        dvs.setup_db()
        self._assert_sample_mirror_capable(dvs, ingress=True, egress=True, truncation=True)

    def test_SampledMirrorTxDirection(self, dvs, testlog):
        """
        Test sampled ERSPAN mirror session with TX (egress) direction.
        Verify the SAMPLEPACKET is created and the source port is programmed
        with egress sampled mirror attributes only (no ingress attrs).
        """
        session = "SAMPLED_TX_SESSION"
        dvs.setup_db()
        # Egress sampled mirroring must be reported capable for this VS test
        self._assert_sample_mirror_capable(dvs, egress=True)
        self._setup_mirror_session(dvs, session, "Ethernet12",
                                          sample_rate="50000", direction="TX")

        # SAMPLEPACKET created
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1)

        # Port programmed with egress sampled mirror attrs
        fvs = dict(dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", ""))
        port_oid = fvs.get("Ethernet12")
        port_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
        assert "SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE" in port_entry
        assert "SAI_PORT_ATTR_EGRESS_SAMPLE_MIRROR_SESSION" in port_entry
        assert "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE" not in port_entry
        assert "SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION" not in port_entry

        self._teardown_mirror_session(dvs, session)
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)

    def test_SampledMirrorTruncateWithoutRateDefaultsToRate1(self, dvs, testlog):
        """
        Test that truncate_size without an explicit sample_rate defaults the
        sample rate to 1 (sample every packet) so a SAMPLEPACKET object is
        still created to carry the truncation.
        """
        session = "SAMPLED_TRUNC_NO_RATE_SESSION"
        self._setup_mirror_session(dvs, session, "Ethernet12",
                                          truncate_size="128")

        # SAMPLEPACKET is created with sample_rate defaulted to 1 and truncation applied
        samplepacket_keys = dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1)
        samplepacket_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", samplepacket_keys[0])
        assert samplepacket_entry["SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE"] == "1"
        assert samplepacket_entry["SAI_SAMPLEPACKET_ATTR_TRUNCATE_ENABLE"] == "true"
        assert samplepacket_entry["SAI_SAMPLEPACKET_ATTR_TRUNCATE_SIZE"] == "128"

        self._teardown_mirror_session(dvs, session)
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)

    def test_SampledMirrorPortCleanup(self, dvs, testlog):
        """
        Test that port attributes are properly cleaned up after removing a sampled mirror session.
        Verify SAMPLEPACKET_ENABLE and SAMPLE_MIRROR_SESSION are cleared from port.
        """
        session = "SAMPLED_CLEANUP_SESSION"
        self._setup_mirror_session(dvs, session, "Ethernet12", sample_rate="50000")

        # Verify port has sampled mirror attributes set
        fvs = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        fvs = dict(fvs)
        port_oid = fvs.get("Ethernet12")
        port_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
        assert "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE" in port_entry
        assert "SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION" in port_entry

        # Remove mirror session
        self._teardown_mirror_session(dvs, session)

        # Verify SAMPLEPACKET object removed
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)

        # Verify port attributes cleaned up
        port_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
        # After cleanup, SAMPLEPACKET_ENABLE should be null or absent
        if "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE" in port_entry:
            assert port_entry["SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE"] == "oid:0x0"
        if "SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION" in port_entry:
            assert port_entry["SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION"] == "0:null"

    def test_SampledMirrorMultipleSrcPorts(self, dvs, testlog):
        """
        Test sampled ERSPAN mirror session with multiple source ports.
        Verify both ports have SAMPLEPACKET_ENABLE and SAMPLE_MIRROR_SESSION set.
        """
        session = "SAMPLED_MULTI_PORT_SESSION"
        self._setup_mirror_session(dvs, session, "Ethernet0,Ethernet4", sample_rate="50000")

        # Verify SAMPLEPACKET object exists
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1)

        # Verify both ports have sampled mirror attributes
        fvs = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        fvs = dict(fvs)
        for port_name in ["Ethernet0", "Ethernet4"]:
            port_oid = fvs.get(port_name)
            port_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
            assert "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE" in port_entry, \
                f"SAMPLEPACKET_ENABLE not set on {port_name}"
            assert "SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION" in port_entry, \
                f"SAMPLE_MIRROR_SESSION not set on {port_name}"

        self._teardown_mirror_session(dvs, session)

    def test_SampledMirrorBothDirection(self, dvs, testlog):
        """
        Test sampled ERSPAN mirror session with BOTH direction.
        Verify a single SAMPLEPACKET is created and the source port is
        programmed with both ingress and egress sampled mirror attributes.
        """
        session = "SAMPLED_BOTH_SESSION"
        dvs.setup_db()
        # Both ingress and egress sampled mirroring must be reported capable
        self._assert_sample_mirror_capable(dvs, ingress=True, egress=True)
        self._setup_mirror_session(dvs, session, "Ethernet12",
                                          sample_rate="50000", direction="BOTH")

        # A single SAMPLEPACKET serves both directions
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1)

        # Port programmed with both ingress and egress sampled mirror attrs
        fvs = dict(dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", ""))
        port_oid = fvs.get("Ethernet12")
        port_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
        assert "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE" in port_entry
        assert "SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION" in port_entry
        assert "SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE" in port_entry
        assert "SAI_PORT_ATTR_EGRESS_SAMPLE_MIRROR_SESSION" in port_entry

        self._teardown_mirror_session(dvs, session)
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)

    def test_SampledMirrorDeleteRecreate(self, dvs, testlog):
        """
        Verify recreating a session under the same key applies the new sample_rate and truncation.
        """
        session = "SAMPLED_RECREATE_SESSION"
        dvs.setup_db()

        # Create initial sampled session (rate only)
        self.dvs_mirror.create_erspan_session_sampled(
            session, "1.1.1.1", "2.2.2.2", "0x8949", "8", "64", "0",
            src_ports="Ethernet12", direction="RX", sample_rate="50000")
        self._bring_up_route(dvs)
        dvs.state_db.wait_for_field_match("MIRROR_SESSION_TABLE", session, {"status": "active"})

        samplepacket_keys = dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1)
        entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", samplepacket_keys[0])
        assert entry["SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE"] == "50000"

        # Delete the session; the SAMPLEPACKET must be cleaned up
        self.dvs_mirror.remove_mirror_session(session)
        dvs.state_db.wait_for_deleted_entry("MIRROR_SESSION_TABLE", session)
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)

        # Recreate under the same key with a new rate + truncation
        self.dvs_mirror.create_erspan_session_sampled(
            session, "1.1.1.1", "2.2.2.2", "0x8949", "8", "64", "0",
            src_ports="Ethernet12", direction="RX",
            sample_rate="100000", truncate_size="128")
        dvs.state_db.wait_for_field_match("MIRROR_SESSION_TABLE", session, {"status": "active"})

        new_keys = dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1)
        new_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", new_keys[0])
        assert new_entry["SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE"] == "100000"
        assert new_entry["SAI_SAMPLEPACKET_ATTR_TRUNCATE_ENABLE"] == "true"
        assert new_entry["SAI_SAMPLEPACKET_ATTR_TRUNCATE_SIZE"] == "128"

        # STATE_DB reflects the recreated values
        state_entry = dvs.state_db.wait_for_entry("MIRROR_SESSION_TABLE", session)
        assert state_entry.get("sample_rate") == "100000"
        assert state_entry.get("truncate_size") == "128"

        self._teardown_mirror_session(dvs, session)
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)

    def test_SampledMirrorDuplicateSetIgnored(self, dvs, testlog):
        """
        Verify the existing SAMPLEPACKET and STATE_DB are not mutated.
        """
        session = "SAMPLED_DUP_SET_SESSION"
        self._setup_mirror_session(dvs, session, "Ethernet12", sample_rate="50000")

        samplepacket_keys = dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1)
        entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", samplepacket_keys[0])
        assert entry["SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE"] == "50000"

        # Partial SET on the existing key - should be ignored as a duplicate
        self.dvs_mirror.config_db.set_field(
            "MIRROR_SESSION", session, "sample_rate", "100000")
        time.sleep(1)

        # Still exactly one SAMPLEPACKET, rate unchanged
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1)
        entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", samplepacket_keys[0])
        assert entry["SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE"] == "50000"

        # STATE_DB unchanged
        state_entry = dvs.state_db.wait_for_entry("MIRROR_SESSION_TABLE", session)
        assert state_entry.get("sample_rate") == "50000"

        self._teardown_mirror_session(dvs, session)

    def test_SampledMirrorRejectsWhenSflowBound(self, dvs, testlog):
        """
        Test that sampled mirror activation is rejected when sFlow
        already has a samplepacket bound on the same source port.
        """
        dvs.setup_db()

        # Step 1: Enable sFlow and configure on Ethernet12
        dvs.config_db.create_entry("SFLOW", "global", {"admin_state": "up"})
        dvs.config_db.create_entry("SFLOW_SESSION", "Ethernet12",
                                    {"sample_rate": "1000", "admin_state": "up"})

        # Wait for sFlow samplepacket to appear in ASIC_DB
        dvs.asic_db.wait_for_n_keys(
            "ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1, wait_at_least_n_keys=True)

        # Step 2: Create sampled mirror on same port
        self.dvs_mirror.create_erspan_session_sampled(
            "CONFLICT_SESSION", "1.1.1.1", "2.2.2.2", "0x8949", "8", "64", "0",
            src_ports="Ethernet12", direction="RX", sample_rate="50000")

        # Setup route for ERSPAN destination
        dvs.set_interface_status("Ethernet16", "up")
        dvs.add_ip_address("Ethernet16", "10.0.0.0/30")
        dvs.add_neighbor("Ethernet16", "10.0.0.1", "02:04:06:08:10:12")
        dvs.add_route("2.2.2.2", "10.0.0.1")

        # Wait for activation attempt
        import time
        time.sleep(3)

        # Step 3: Verify mirror session stays inactive (conflict rejected it)
        dvs.state_db.wait_for_field_match("MIRROR_SESSION_TABLE",
                                           "CONFLICT_SESSION",
                                           {"status": "inactive"})

        # Step 4: Verify sFlow samplepacket still exists (not overwritten)
        sp_keys = dvs.asic_db.wait_for_n_keys(
            "ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1, wait_at_least_n_keys=True)
        sp_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", sp_keys[0])
        assert sp_entry["SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE"] == "1000"

        # Cleanup
        dvs.remove_route("2.2.2.2")
        dvs.remove_neighbor("Ethernet16", "10.0.0.1")
        dvs.remove_ip_address("Ethernet16", "10.0.0.0/30")
        dvs.set_interface_status("Ethernet16", "down")
        self.dvs_mirror.remove_mirror_session("CONFLICT_SESSION")
        dvs.config_db.delete_entry("SFLOW_SESSION", "Ethernet12")
        dvs.config_db.delete_entry("SFLOW", "global")
        time.sleep(2)
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)

    def test_SampledMirrorOnLag(self, dvs, testlog):
        """
        Sampled ERSPAN mirror session with a LAG as src_port.
        Verifies that LAG members each get SAMPLEPACKET/SAMPLE_MIRROR_SESSION
        bindings (LAG OID itself is not a valid SAI target for these attrs).
        Covers MirrorOrch::setUnsetPortMirror LAG dispatch branch.
        """
        dvs.setup_db()

        session = "SAMPLED_LAG_SESSION"
        po = "001"
        po_name = "PortChannel" + po
        member1 = "Ethernet0"
        member2 = "Ethernet4"

        # create LAG and add two members
        self.dvs_lag.create_port_channel(po)
        self.dvs_lag.create_port_channel_member(po, member1)
        self.dvs_lag.create_port_channel_member(po, member2)
        dvs.set_interface_status(po_name, "up")
        dvs.set_interface_status(member1, "up")
        dvs.set_interface_status(member2, "up")

        # create sampled mirror session with LAG as src_port
        self.dvs_mirror.create_erspan_session_sampled(
            session, "1.1.1.1", "2.2.2.2", "0x8949", "8", "64", "0",
            src_ports=po_name, direction="RX",
            sample_rate="50000", truncate_size=None)

        # set up neighbor on a separate PHY port to activate session
        dvs.set_interface_status("Ethernet16", "up")
        dvs.add_ip_address("Ethernet16", "10.0.0.0/30")
        dvs.add_neighbor("Ethernet16", "10.0.0.1", "02:04:06:08:10:12")
        dvs.add_route("2.2.2.2", "10.0.0.1")
        dvs.state_db.wait_for_field_match("MIRROR_SESSION_TABLE", session, {"status": "active"})

        # SAMPLEPACKET object should be created
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1)

        # Both LAG members should have INGRESS_SAMPLEPACKET_ENABLE +
        # INGRESS_SAMPLE_MIRROR_SESSION attributes set on their port objects.
        fvs = dict(dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", ""))
        for member in (member1, member2):
            port_oid = fvs.get(member)
            assert port_oid is not None
            entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
            assert "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE" in entry
            assert "SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION" in entry

        # Cleanup
        dvs.remove_route("2.2.2.2")
        dvs.remove_neighbor("Ethernet16", "10.0.0.1")
        dvs.remove_ip_address("Ethernet16", "10.0.0.0/30")
        dvs.set_interface_status("Ethernet16", "down")
        self.dvs_mirror.remove_mirror_session(session)
        dvs.state_db.wait_for_deleted_entry("MIRROR_SESSION_TABLE", session)
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)
        self.dvs_lag.remove_port_channel_member(po, member1)
        self.dvs_lag.remove_port_channel_member(po, member2)
        self.dvs_lag.remove_port_channel(po)

    def test_SampledMirrorInactiveNotProgrammed(self, dvs, testlog):
        """
        A sampled session must not program the ASIC until its ERSPAN dst is
        routable. Verify that while the session is inactive (no route) there
        is no SAMPLEPACKET and the source port carries no sampled mirror
        bindings, and that installing the route activates and programs it.
        """
        session = "SAMPLED_INACTIVE_SESSION"
        dvs.setup_db()

        # Create the sampled session but do NOT install the route yet
        self.dvs_mirror.create_erspan_session_sampled(
            session, "1.1.1.1", "2.2.2.2", "0x8949", "8", "64", "0",
            src_ports="Ethernet12", direction="RX",
            sample_rate="50000", truncate_size="128")

        # Without a route to the ERSPAN dst the session stays inactive
        dvs.state_db.wait_for_field_match("MIRROR_SESSION_TABLE", session, {"status": "inactive"})

        # Nothing is programmed into the ASIC while inactive
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)
        fvs = dict(dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", ""))
        port_oid = fvs.get("Ethernet12")
        port_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
        if "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE" in port_entry:
            assert port_entry["SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE"] == "oid:0x0"
        if "SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION" in port_entry:
            assert port_entry["SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION"] == "0:null"

        # Installing the route activates the session and programs the ASIC
        self._bring_up_route(dvs)
        dvs.state_db.wait_for_field_match("MIRROR_SESSION_TABLE", session, {"status": "active"})
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1)
        port_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
        assert "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE" in port_entry
        assert "SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION" in port_entry

        self._teardown_mirror_session(dvs, session)
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)

    def test_SampledMirrorRouteWithdrawReactivate(self, dvs, testlog):
        """
        Verify the activation lifecycle when the route to the ERSPAN dst is
        withdrawn and later restored while the session still exists. The
        SAMPLEPACKET and port bindings must be torn down on route loss and
        recreated when the route returns.
        """
        session = "SAMPLED_ROUTE_FLAP_SESSION"
        self._setup_mirror_session(dvs, session, "Ethernet12", sample_rate="50000")

        # Active: SAMPLEPACKET present and the source port is bound
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1)
        fvs = dict(dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", ""))
        port_oid = fvs.get("Ethernet12")
        port_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
        assert "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE" in port_entry
        assert "SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION" in port_entry

        # Withdraw only the route; the session must deactivate and unprogram
        self._tear_down_route(dvs)
        dvs.state_db.wait_for_field_match("MIRROR_SESSION_TABLE", session, {"status": "inactive"})
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)
        port_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
        if "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE" in port_entry:
            assert port_entry["SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE"] == "oid:0x0"
        if "SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION" in port_entry:
            assert port_entry["SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION"] == "0:null"

        # Restore the route; the session reactivates and reprograms the ASIC
        self._bring_up_route(dvs)
        dvs.state_db.wait_for_field_match("MIRROR_SESSION_TABLE", session, {"status": "active"})
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1)
        port_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
        assert "SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE" in port_entry
        assert "SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION" in port_entry

        self._teardown_mirror_session(dvs, session)
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)
