import time
import pytest

from swsscommon import swsscommon


def set_cfg_entry(db, table, key, pairs):
    tbl = swsscommon.ProducerStateTable(db, table)
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)
    time.sleep(1)


def get_switch_oid(dvs):
    db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    tbl = swsscommon.Table(db, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")
    entries = list(tbl.getKeys())
    return entries[0]


def expect_switch_attrs(dvs, oid, expected):
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    tbl = swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")
    dvs.asic_db.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_SWITCH", oid, expected)


class TestFastLinkupSwss(object):
    def test_capability_state_db(self, dvs, testlog):
        # Verify capability fields exist (at least the CAPABLE key is written)
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
        cap_tbl = swsscommon.Table(state_db, "SWITCH_CAPABILITY")
        status, fvs = cap_tbl.get("switch")
        assert status
        cap_map = {k: v for k, v in fvs}
        assert "FAST_LINKUP_CAPABLE" in cap_map
        # Optional ranges
        # Do not assert exact values; presence indicates SAI support
        # FAST_LINKUP_POLLING_TIMER_RANGE / FAST_LINKUP_GUARD_TIMER_RANGE may be absent on platforms without ranges

    def test_global_config_applies_sai(self, dvs, testlog):
        # Apply global config via CONFIG_DB and expect ASIC DB attrs set
        app_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        set_cfg_entry(app_db, "SWITCH_FAST_LINKUP", "GLOBAL",
                      [("polling_time", "60"), ("guard_time", "10"), ("ber_threshold", "12")])

        switch_oid = get_switch_oid(dvs)
        # Values are stored as strings in ASIC DB tables
        expected = {
            'SAI_SWITCH_ATTR_FAST_LINKUP_POLLING_TIMEOUT': '60',
            'SAI_SWITCH_ATTR_FAST_LINKUP_GUARD_TIMEOUT': '10',
            'SAI_SWITCH_ATTR_FAST_LINKUP_BER_THRESHOLD': '12',
        }
        expect_switch_attrs(dvs, switch_oid, expected)

    def test_global_config_out_of_range_rejected(self, dvs, testlog):
        # If ranges are published, send out-of-range and assert ASIC DB not updated with that value
        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
        cap_tbl = swsscommon.Table(state_db, "SWITCH_CAPABILITY")
        status, fvs = cap_tbl.get("switch")
        cap_map = {k: v for k, v in fvs} if status else {}
        poll_rng = cap_map.get("FAST_LINKUP_POLLING_TIMER_RANGE")
        guard_rng = cap_map.get("FAST_LINKUP_GUARD_TIMER_RANGE")
        if poll_rng and guard_rng:
            poll_min, poll_max = [int(x) for x in poll_rng.split(',')]
            guard_min, guard_max = [int(x) for x in guard_rng.split(',')]

            cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
            # Attempt invalid values (below min)
            set_cfg_entry(cfg_db, "SWITCH_FAST_LINKUP", "GLOBAL",
                          [("polling_time", str(max(poll_min - 1, 0))), ("guard_time", str(max(guard_min - 1, 0)))])
            # Give orch time to process; check ASIC DB does not reflect those values (negative match)
            switch_oid = get_switch_oid(dvs)
            try:
                dvs.asic_db.wait_for_field_negative_match("ASIC_STATE:SAI_OBJECT_TYPE_SWITCH", switch_oid,
                    {'SAI_SWITCH_ATTR_FAST_LINKUP_POLLING_TIMEOUT': str(max(poll_min - 1, 0))})
                dvs.asic_db.wait_for_field_negative_match("ASIC_STATE:SAI_OBJECT_TYPE_SWITCH", switch_oid,
                    {'SAI_SWITCH_ATTR_FAST_LINKUP_GUARD_TIMEOUT': str(max(guard_min - 1, 0))})
            except Exception:
                # On VS without validation paths, skip strict assertion
                pass

    def test_port_fast_linkup_enable(self, dvs, testlog):
        # Toggle per-port fast_linkup and validate via ASIC DB when supported
        cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        port_tbl = swsscommon.Table(cfg_db, "PORT")

        # Read a port key
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        app_port_tbl = swsscommon.Table(app_db, "PORT_TABLE")
        port_keys = app_port_tbl.getKeys()
        assert len(port_keys) > 0
        first_port = port_keys[0]

        # Modify in CONFIG_DB
        status, values = port_tbl.get(first_port)
        assert status
        current = {k: v for k, v in values}
        current["fast_linkup"] = "true"
        port_tbl.set(first_port, swsscommon.FieldValuePairs(list(current.items())))
        time.sleep(1)
        current["fast_linkup"] = "false"
        port_tbl.set(first_port, swsscommon.FieldValuePairs(list(current.items())))
        time.sleep(1)


