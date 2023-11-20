"""Utilities for interacting with TWAMP Light objects when writing VS tests."""

class DVSTwamp(object):
    def __init__(self, adb, cdb, sdb, cntrdb, appdb):
        self.asic_db = adb
        self.config_db = cdb
        self.state_db = sdb
        self.counters_db = cntrdb
        self.app_db = appdb

    def create_twamp_light_session_sender_packet_count(self, name, sip, sport, dip, dport, packet_count=100, tx_interval=100, timeout=5, stats_interval=None):
        twamp_light_entry = {"mode": "LIGHT",
                             "role": "SENDER",
                             "src_ip": sip,
                             "src_udp_port": sport,
                             "dst_ip": dip,
                             "dst_udp_port": dport,
                             "packet_count": packet_count,
                             "tx_interval": tx_interval,
                             "timeout": timeout
                            }
        if stats_interval:
            twamp_light_entry["statistics_interval"] = str(stats_interval)
        else:
            twamp_light_entry["statistics_interval"] = str(int(packet_count) * int(tx_interval) + int(timeout)*1000)
        self.config_db.create_entry("TWAMP_SESSION", name, twamp_light_entry)

    def create_twamp_light_session_sender_continuous(self, name, sip, sport, dip, dport, monitor_time=0, tx_interval=100, timeout=5, stats_interval=None):
        twamp_light_entry = {"mode": "LIGHT",
                             "role": "SENDER",
                             "src_ip": sip,
                             "src_udp_port": sport,
                             "dst_ip": dip,
                             "dst_udp_port": dport,
                             "monitor_time": monitor_time,
                             "tx_interval": tx_interval,
                             "timeout": timeout
                            }
        if stats_interval:
            twamp_light_entry["statistics_interval"] = str(stats_interval)
        else:
            twamp_light_entry["statistics_interval"] = str(int(monitor_time)*1000)
        self.config_db.create_entry("TWAMP_SESSION", name, twamp_light_entry)

    def create_twamp_light_session_reflector(self, name, sip, sport, dip, dport):
        twamp_light_entry = {"mode": "LIGHT",
                             "role": "REFLECTOR",
                             "src_ip": sip,
                             "src_udp_port": sport,
                             "dst_ip": dip,
                             "dst_udp_port": dport
                            }
        self.config_db.create_entry("TWAMP_SESSION", name, twamp_light_entry)

    def start_twamp_light_sender(self, name):
        twamp_light_entry = {"admin_state": "enabled"}
        self.config_db.create_entry("TWAMP_SESSION", name, twamp_light_entry)

    def stop_twamp_light_sender(self, name):
        twamp_light_entry = {"admin_state": "disabled"}
        self.config_db.create_entry("TWAMP_SESSION", name, twamp_light_entry)

    def remove_twamp_light_session(self, name):
        self.config_db.delete_entry("TWAMP_SESSION", name)

    def get_twamp_light_session_status(self, name):
        return self.get_twamp_light_session_state(name)["status"]

    def get_twamp_light_session_state(self, name):
        tbl = swsscommon.Table(self.sdb, "TWAMP_SESSION_TABLE")
        (status, fvs) = tbl.get(name)
        assert status == True
        assert len(fvs) > 0
        return { fv[0]: fv[1] for fv in fvs }

    def verify_session_status(self, name, status="active", expected=1):
        self.state_db.wait_for_n_keys("TWAMP_SESSION_TABLE", expected)
        if expected:
            self.state_db.wait_for_field_match("TWAMP_SESSION_TABLE", name, {"status": status})

    def verify_no_session(self):
        self.config_db.wait_for_n_keys("TWAMP_SESSION", 0)
        self.state_db.wait_for_n_keys("TWAMP_SESSION_TABLE", 0)

    def verify_session_asic_db(self, dvs, name, asic_table=None, expected=1):
        session_oids = self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_TWAMP_SESSION", expected)
        session_oid = session_oids[0]
        dvs.asic_db.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_TWAMP_SESSION", session_oid, asic_table)

    def verify_session_counter_db(self, dvs, name, counter_table=None, expected=1, expected_item=1):
        fvs = dvs.counters_db.get_entry("COUNTERS_TWAMP_SESSION_NAME_MAP", "")
        fvs = dict(fvs)
        total_key = self.counters_db.db_connection.keys("COUNTERS:{}".format(fvs[name]))
        assert len(total_key) == expected, "TWAMP Light counter entries are not available in counter db"
        dvs.counters_db.wait_for_field_match("COUNTERS", fvs[name], counter_table)
        item_keys = self.counters_db.db_connection.keys("COUNTERS:{}:INDEX:*".format(fvs[name]))
        assert len(item_keys) == expected_item, "TWAMP Light counter entries are not available in counter db"

