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
        mirror_entry = {"type": "ERSPAN"}
        mirror_entry["src_ip"] = src
        mirror_entry["dst_ip"] = dst
        mirror_entry["gre_type"] = gre
        mirror_entry["dscp"] = dscp
        mirror_entry["ttl"] = ttl
        mirror_entry["queue"] = queue
        if policer:
            mirror_entry["policer"] = policer
        if src_ports:
            mirror_entry["src_port"] = src_ports
        mirror_entry["direction"] = direction

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
            member = dvs.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
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
            fv_pairs = dvs.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION", asic_table)
            assert all(fv_pairs.get(k) == v for k, v in asic.items())
            if asic_size:
                assert asic_size == len(fv_pairs)
        if state:
            fv_pairs = dvs.state_db.get_entry("MIRROR_SESSION_TABLE", name)
            assert all(fv_pairs.get(k) == v for k, v in state.items())

    def verify_session_policer(self, dvs, policer_oid, cir):
        if cir:
            entry = dvs.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_POLICER", policer_oid)
            assert entry["SAI_POLICER_ATTR_CIR"] == cir
            
    def verify_session(self, dvs, name, asic_db=None, state_db=None, dst_oid=None, src_ports=None, direction="BOTH", policer=None, expected = 1, asic_size=None):
        member_ids = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION", expected)
        session_oid=member_ids[0]
        # with multiple sessions, match on dst_oid to get session_oid
        if dst_oid:
            for member in member_ids:
                entry=dvs.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION", member)
                if entry["SAI_MIRROR_SESSION_ATTR_MONITOR_PORT"] == dst_oid:
                    session_oid = member

        self.verify_session_db(dvs, name, session_oid, asic=asic_db, state=state_db, asic_size=asic_size)
        if policer:
            cir = dvs.config_db.get_entry("POLICER", policer)["cir"]
            entry=dvs.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION", session_oid)
            self.verify_session_policer(dvs, entry["SAI_MIRROR_SESSION_ATTR_POLICER"], cir)
        if src_ports:
            self.verify_port_mirror_config(dvs, src_ports, direction, session_oid=session_oid)

