class DVSMirror(object):
    def __init__(self, adb, cdb, sdb, cntrdb, appdb):
        self.asic_db = adb
        self.config_db = cdb
        self.state_db = sdb
        self.counters_db = cntrdb
        self.app_db = appdb

    def create_span_session(self, name, dst_port, src_ports=None, direction="BOTH", queue=None, policer=None):
        mirror_entry = {"type": "SPAN"}
        if dst_port:
            mirror_entry["dst_port"] = dst_port

        if src_ports:
            mirror_entry["src_port"] = src_ports
        # set direction without source port to uncover any swss issues.
        mirror_entry["direction"] = direction

        if queue:
            mirror_entry["queue"] = queue
        if policer:
            mirror_entry["policer"] = policer
        self.config_db.create_entry("MIRROR_SESSION", name, mirror_entry)

    def create_erspan_session(self, name, src, dst, gre, dscp, ttl, queue, policer=None, src_ports=None, direction="BOTH"):
        mirror_entry = {
            "src_ip": src,
            "dst_ip": dst,
            "gre_type": gre,
            "dscp": dscp,
            "ttl": ttl,
            "queue": queue,
            "direction": direction
        }

        if policer:
            mirror_entry["policer"] = policer

        if src_ports:
            mirror_entry["src_port"] = src_ports

        self.config_db.create_entry("MIRROR_SESSION", name, mirror_entry)

    def create_sampled_erspan_session(self, name, src, dst, gre, dscp, ttl, queue,
                                      sample_rate, direction="RX", src_ports=None,
                                      truncate_size=None, erspan_id=None,
                                      congestion_mode=None):
        mirror_entry = {
            "src_ip": src,
            "dst_ip": dst,
            "gre_type": gre,
            "dscp": dscp,
            "ttl": ttl,
            "queue": queue,
            "direction": direction,
            "sample_rate": str(sample_rate)
        }

        if src_ports:
            mirror_entry["src_port"] = src_ports
        if truncate_size:
            mirror_entry["truncate_size"] = str(truncate_size)
        if erspan_id is not None:
            mirror_entry["erspan_id"] = str(erspan_id)
        if congestion_mode:
            mirror_entry["congestion_mode"] = congestion_mode

        self.config_db.create_entry("MIRROR_SESSION", name, mirror_entry)

    def remove_mirror_session(self, name):
        self.config_db.delete_entry("MIRROR_SESSION", name)

    def verify_no_mirror(self):
        self.config_db.wait_for_n_keys("MIRROR_SESSION", 0)
        self.state_db.wait_for_n_keys("MIRROR_SESSION_TABLE", 0)

    def verify_session_status(self, name, status="active", expected=1):
        self.state_db.wait_for_n_keys("MIRROR_SESSION_TABLE", expected)
        if expected:
            self.state_db.wait_for_field_match("MIRROR_SESSION_TABLE", name, {"status": status})

    def verify_port_mirror_config(self, dvs, ports, direction, session_oid="null"):
        fvs = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        fvs = dict(fvs)
        for p in ports:
            port_oid = fvs.get(p)
            member = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
            if direction in {"RX", "BOTH"}:
                assert member["SAI_PORT_ATTR_INGRESS_MIRROR_SESSION"] == "1:"+session_oid
            else:
                assert "SAI_PORT_ATTR_INGRESS_MIRROR_SESSION" not in member.keys() or member["SAI_PORT_ATTR_INGRESS_MIRROR_SESSION"] == "0:null"
            if direction in {"TX", "BOTH"}:
                assert member["SAI_PORT_ATTR_EGRESS_MIRROR_SESSION"] == "1:"+session_oid
            else:
                assert "SAI_PORT_ATTR_EGRESS_MIRROR_SESSION" not in member.keys() or member["SAI_PORT_ATTR_EGRESS_MIRROR_SESSION"] == "0:null"

    def verify_session_db(self, dvs, name, asic_table=None, asic=None, state=None, asic_size=None):
        if asic:
            dvs.asic_db.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION", asic_table, asic)
        if state:
            dvs.state_db.wait_for_field_match("MIRROR_SESSION_TABLE", name, state)

    def verify_session_policer(self, dvs, policer_oid, cir):
        if cir:
            entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_POLICER", policer_oid)
            assert entry["SAI_POLICER_ATTR_CIR"] == cir
            
    def verify_session(self, dvs, name, asic_db=None, state_db=None, dst_oid=None, src_ports=None, direction="BOTH", policer=None, expected = 1, asic_size=None):
        member_ids = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION", expected)
        session_oid=member_ids[0]
        # with multiple sessions, match on dst_oid to get session_oid
        if dst_oid:
            for member in member_ids:
                entry=dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION", member)
                if entry["SAI_MIRROR_SESSION_ATTR_MONITOR_PORT"] == dst_oid:
                    session_oid = member

        self.verify_session_db(dvs, name, session_oid, asic=asic_db, state=state_db, asic_size=asic_size)
        if policer:
            cir = dvs.config_db.wait_for_entry("POLICER", policer)["cir"]
            entry=dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION", session_oid)
            self.verify_session_policer(dvs, entry["SAI_MIRROR_SESSION_ATTR_POLICER"], cir)
        if src_ports:
            self.verify_port_mirror_config(dvs, src_ports, direction, session_oid=session_oid)

    def verify_sample_mirror_port_config(self, dvs, ports, session_oid="null", samplepacket_oid="null"):
        """Verify SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION is set on ports."""
        fvs = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        fvs = dict(fvs)
        for p in ports:
            port_oid = fvs.get(p)
            member = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
            assert member.get("SAI_PORT_ATTR_INGRESS_SAMPLE_MIRROR_SESSION") == "1:" + session_oid
            assert member.get("SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE") == samplepacket_oid

    def verify_samplepacket(self, dvs, rate, truncate_size=None):
        """Verify SAMPLEPACKET object exists in ASIC_DB with expected attributes."""
        keys = dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 1)
        entry = dvs.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", keys[0])
        assert entry["SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE"] == str(rate)
        if truncate_size:
            assert entry.get("SAI_SAMPLEPACKET_ATTR_TRUNCATE_SIZE") == str(truncate_size)
            assert entry.get("SAI_SAMPLEPACKET_ATTR_TRUNCATE_ENABLE") == "true"
        return keys[0]

    def verify_no_samplepacket(self, dvs):
        """Verify no SAMPLEPACKET objects exist in ASIC_DB."""
        dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_SAMPLEPACKET", 0)

