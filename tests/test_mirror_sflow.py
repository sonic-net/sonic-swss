import pytest


@pytest.mark.usefixtures("testlog")
@pytest.mark.usefixtures('dvs_mirror_manager')
class TestSflowMirror(object):

    @pytest.fixture(autouse=True)
    def _isolate(self, dvs):
        self._force_clean(dvs)
        yield
        self._force_clean(dvs)

    def _force_clean(self, dvs):
        dvs.setup_db()
        mirror_keys = dvs.config_db.get_keys("MIRROR_SESSION")
        for key in mirror_keys:
            self.dvs_mirror.remove_mirror_session(key)
        self._tear_down_route(dvs)
        for key in mirror_keys:
            dvs.state_db.wait_for_deleted_entry("MIRROR_SESSION_TABLE", key)
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION", 0)
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)

    def _bring_up_route(self, dvs):
        dvs.set_interface_status("Ethernet16", "up")
        dvs.add_ip_address("Ethernet16", "10.0.0.0/30")
        dvs.add_neighbor("Ethernet16", "10.0.0.1", "02:04:06:08:10:12")
        dvs.add_route("2.2.2.2", "10.0.0.1")

    def _tear_down_route(self, dvs):
        dvs.remove_route("2.2.2.2")
        dvs.remove_neighbor("Ethernet16", "10.0.0.1")
        dvs.remove_ip_address("Ethernet16", "10.0.0.0/30")
        dvs.set_interface_status("Ethernet16", "down")

    def _create_active_session(self, dvs, session, **kwargs):
        self.dvs_mirror.create_sflow_session(session, "1.1.1.1", "2.2.2.2", "8", "64", "0",
                                             "Ethernet12", **kwargs)
        self._bring_up_route(dvs)
        dvs.state_db.wait_for_field_match("MIRROR_SESSION_TABLE", session, {"status": "active"})

    def _get_single_entry(self, dvs, table):
        keys = dvs.asic_db.wait_for_n_keys(table, 1)
        return keys[0], dvs.asic_db.wait_for_entry(table, keys[0])

    def test_SflowCapabilityInStateDB(self, dvs, testlog):
        cap = dvs.state_db.wait_for_entry("SWITCH_CAPABILITY", "switch")
        assert cap.get("MIRROR_SESSION_SFLOW_CAPABLE") == "true"

    def test_SflowSessionCreateRemove(self, dvs, testlog):
        session = "SFLOW_SESSION"
        self._create_active_session(dvs, session, sample_rate="50000")

        pmap = dict(dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", ""))
        src_mac = dvs.runcmd("bash -c \"ip link show eth0 | grep ether | awk '{print $2}'\"")[1].strip().upper()
        session_oid, session_entry = self._get_single_entry(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert session_entry == {
            "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap["Ethernet16"],
            "SAI_MIRROR_SESSION_ATTR_TYPE": "SAI_MIRROR_SESSION_TYPE_SFLOW",
            "SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION": "4",
            "SAI_MIRROR_SESSION_ATTR_TOS": "32",
            "SAI_MIRROR_SESSION_ATTR_TTL": "64",
            "SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS": "1.1.1.1",
            "SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS": "2.2.2.2",
            "SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS": src_mac,
            "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS": "02:04:06:08:10:12",
            "SAI_MIRROR_SESSION_ATTR_UDP_SRC_PORT": "6343",
            "SAI_MIRROR_SESSION_ATTR_UDP_DST_PORT": "6343",
            "SAI_MIRROR_SESSION_ATTR_SAMPLE_RATE": "50000",
        }

        samplepacket_oid, samplepacket_entry = self._get_single_entry(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET")
        assert samplepacket_entry["SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE"] == "50000"
        assert samplepacket_entry["SAI_SAMPLEPACKET_ATTR_TYPE"] == "SAI_SAMPLEPACKET_TYPE_MIRROR_SESSION"

        port_entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", pmap["Ethernet12"])
        assert port_entry["SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE"] == samplepacket_oid
        assert port_entry["SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION"] == "1:" + session_oid

        self.dvs_mirror.remove_mirror_session(session)
        dvs.state_db.wait_for_deleted_entry("MIRROR_SESSION_TABLE", session)
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION", 0)
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)

    def test_SflowTruncateOnSessionOnly(self, dvs, testlog):
        session = "SFLOW_TRUNCATE_SESSION"
        self._create_active_session(dvs, session, sample_rate="50000", truncate_size="128")

        _, session_entry = self._get_single_entry(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert session_entry["SAI_MIRROR_SESSION_ATTR_TRUNCATE_SIZE"] == "128"

        _, samplepacket_entry = self._get_single_entry(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET")
        assert "SAI_SAMPLEPACKET_ATTR_TRUNCATE_ENABLE" not in samplepacket_entry
        assert "SAI_SAMPLEPACKET_ATTR_TRUNCATE_SIZE" not in samplepacket_entry

    def test_SflowUdpDstPort(self, dvs, testlog):
        session = "SFLOW_UDP_SESSION"
        self._create_active_session(dvs, session, sample_rate="50000", udp_dst_port="6400")

        _, session_entry = self._get_single_entry(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert session_entry["SAI_MIRROR_SESSION_ATTR_UDP_SRC_PORT"] == "6343"
        assert session_entry["SAI_MIRROR_SESSION_ATTR_UDP_DST_PORT"] == "6400"

    @pytest.mark.parametrize("bad_fields", [{}, {"gre_type": "0x8949"}],
                             ids=["no_sample_rate", "gre_type"])
    def test_SflowInvalidSessionRejected(self, dvs, testlog, bad_fields):
        sample_rate = None if not bad_fields else "50000"
        self.dvs_mirror.create_sflow_session("SFLOW_BAD", "1.1.1.1", "2.2.2.2", "8", "64", "0",
                                             "Ethernet12", sample_rate=sample_rate, **bad_fields)
        # MirrorOrch handles keys in order, so once the later valid session shows
        # up in STATE_DB the invalid one has already been processed.
        self._create_active_session(dvs, "SFLOW_OK", sample_rate="50000")

        assert "SFLOW_BAD" not in dvs.state_db.get_keys("MIRROR_SESSION_TABLE")
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION", 1)
