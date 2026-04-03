import time
import pytest

from swsscommon import swsscommon


def set_cfg_entry(db, table, key, pairs):
    tbl = swsscommon.Table(db, table)
    tbl.set(key, swsscommon.FieldValuePairs(pairs))
    time.sleep(1)


def get_switch_oid(dvs):
    db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    tbl = swsscommon.Table(db, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")
    return list(tbl.getKeys())[0]


def get_switch_attrs(dvs, oid):
    db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    tbl = swsscommon.Table(db, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")
    status, fvs = tbl.get(oid)
    if not status:
        return {}
    return {k: v for k, v in fvs}


def expect_switch_attrs(dvs, oid, expected):
    dvs.asic_db.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_SWITCH", oid, expected)


def get_fast_linkup_capability(dvs):
    state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
    cap_tbl = swsscommon.Table(state_db, "SWITCH_CAPABILITY")
    status, fvs = cap_tbl.get("switch")
    if not status:
        return {}
    return {k: v for k, v in fvs}


def require_fast_linkup_capable(dvs):
    cap_map = get_fast_linkup_capability(dvs)
    if cap_map.get("FAST_LINKUP_CAPABLE") != "true":
        pytest.skip("Fast linkup not capable on this platform")
    return cap_map


def require_fast_linkup_not_capable(dvs):
    cap_map = get_fast_linkup_capability(dvs)
    if cap_map.get("FAST_LINKUP_CAPABLE") != "false":
        pytest.skip("Platform reports fast linkup capable")
    return cap_map


def parse_range_or_skip(cap_map, key):
    rng = cap_map.get(key)
    if not rng:
        pytest.skip(f"{key} not published")
    lo, hi = [int(x) for x in rng.split(",")]
    return lo, hi


def get_port_oid(dvs, port_name):
    counters_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
    port_map_tbl = swsscommon.Table(counters_db, "COUNTERS_PORT_NAME_MAP")
    status, fvs = port_map_tbl.get("")
    if not status:
        return None
    for k, v in fvs:
        if k == port_name:
            return v
    return None


def get_first_port(dvs):
    cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    port_tbl = swsscommon.Table(cfg_db, "PORT")
    keys = port_tbl.getKeys()
    assert len(keys) > 0
    return keys[0]


def set_port_admin_and_wait_down(dvs, port_name):
    cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    port_tbl = swsscommon.Table(cfg_db, "PORT")
    status, values = port_tbl.get(port_name)
    assert status
    current = {k: v for k, v in values}
    current["admin_status"] = "down"
    port_tbl.set(port_name, swsscommon.FieldValuePairs(list(current.items())))
    time.sleep(1)
    port_oid = get_port_oid(dvs, port_name)
    assert port_oid is not None
    dvs.asic_db.wait_for_field_match(
        "ASIC_STATE:SAI_OBJECT_TYPE_PORT",
        port_oid,
        {"SAI_PORT_ATTR_ADMIN_STATE": "false"},
    )
    return current, port_oid


class TestFastLinkupSwss(object):
    def test_capability_state_db(self, dvs, testlog):
        cap_map = get_fast_linkup_capability(dvs)
        assert "FAST_LINKUP_CAPABLE" in cap_map

    def test_global_config_applies_sai(self, dvs, testlog):
        require_fast_linkup_capable(dvs)
        cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        set_cfg_entry(
            cfg_db,
            "SWITCH_FAST_LINKUP",
            "GLOBAL",
            [("polling_time", "60"), ("guard_time", "10"), ("ber_threshold", "12")],
        )
        expect_switch_attrs(dvs, get_switch_oid(dvs), {
            "SAI_SWITCH_ATTR_FAST_LINKUP_POLLING_TIMEOUT": "60",
            "SAI_SWITCH_ATTR_FAST_LINKUP_GUARD_TIMEOUT": "10",
            "SAI_SWITCH_ATTR_FAST_LINKUP_BER_THRESHOLD": "12",
        })

    def test_global_config_rejected_when_not_capable(self, dvs, testlog):
        require_fast_linkup_not_capable(dvs)
        cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        switch_oid = get_switch_oid(dvs)
        before = get_switch_attrs(dvs, switch_oid)
        set_cfg_entry(
            cfg_db,
            "SWITCH_FAST_LINKUP",
            "GLOBAL",
            [("polling_time", "60"), ("guard_time", "10"), ("ber_threshold", "12")],
        )
        after = get_switch_attrs(dvs, switch_oid)
        assert "SAI_SWITCH_ATTR_FAST_LINKUP_POLLING_TIMEOUT" not in after
        assert "SAI_SWITCH_ATTR_FAST_LINKUP_GUARD_TIMEOUT" not in after
        assert "SAI_SWITCH_ATTR_FAST_LINKUP_BER_THRESHOLD" not in after
        assert before == after

    def test_global_config_invalid_values_do_not_override(self, dvs, testlog):
        require_fast_linkup_capable(dvs)
        cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        switch_oid = get_switch_oid(dvs)
        baseline = {
            "SAI_SWITCH_ATTR_FAST_LINKUP_POLLING_TIMEOUT": "60",
            "SAI_SWITCH_ATTR_FAST_LINKUP_GUARD_TIMEOUT": "10",
            "SAI_SWITCH_ATTR_FAST_LINKUP_BER_THRESHOLD": "12",
        }
        set_cfg_entry(
            cfg_db,
            "SWITCH_FAST_LINKUP",
            "GLOBAL",
            [("polling_time", "60"), ("guard_time", "10"), ("ber_threshold", "12")],
        )
        expect_switch_attrs(dvs, switch_oid, baseline)
        set_cfg_entry(
            cfg_db,
            "SWITCH_FAST_LINKUP",
            "GLOBAL",
            [("polling_time", "abc"), ("guard_time", "999"), ("ber_threshold", "1000")],
        )
        expect_switch_attrs(dvs, switch_oid, baseline)

    def test_global_config_unknown_field_is_ignored(self, dvs, testlog):
        require_fast_linkup_capable(dvs)
        cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        switch_oid = get_switch_oid(dvs)
        set_cfg_entry(
            cfg_db,
            "SWITCH_FAST_LINKUP",
            "GLOBAL",
            [("polling_time", "61"), ("unknown_field", "1"), ("ber_threshold", "13")],
        )
        expect_switch_attrs(dvs, switch_oid, {
            "SAI_SWITCH_ATTR_FAST_LINKUP_POLLING_TIMEOUT": "61",
            "SAI_SWITCH_ATTR_FAST_LINKUP_BER_THRESHOLD": "13",
        })

    def test_global_config_delete_does_not_clear_attrs(self, dvs, testlog):
        require_fast_linkup_capable(dvs)
        cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        cfg_tbl = swsscommon.Table(cfg_db, "SWITCH_FAST_LINKUP")
        switch_oid = get_switch_oid(dvs)
        expected = {
            "SAI_SWITCH_ATTR_FAST_LINKUP_POLLING_TIMEOUT": "62",
            "SAI_SWITCH_ATTR_FAST_LINKUP_GUARD_TIMEOUT": "11",
            "SAI_SWITCH_ATTR_FAST_LINKUP_BER_THRESHOLD": "14",
        }
        set_cfg_entry(
            cfg_db,
            "SWITCH_FAST_LINKUP",
            "GLOBAL",
            [("polling_time", "62"), ("guard_time", "11"), ("ber_threshold", "14")],
        )
        expect_switch_attrs(dvs, switch_oid, expected)
        cfg_tbl._del("GLOBAL")
        time.sleep(1)
        expect_switch_attrs(dvs, switch_oid, expected)

    def test_global_config_polling_above_max_rejected(self, dvs, testlog):
        cap_map = require_fast_linkup_capable(dvs)
        poll_min, poll_max = parse_range_or_skip(cap_map, "FAST_LINKUP_POLLING_TIMER_RANGE")
        cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        switch_oid = get_switch_oid(dvs)
        set_cfg_entry(cfg_db, "SWITCH_FAST_LINKUP", "GLOBAL", [("polling_time", str(poll_min))])
        expect_switch_attrs(dvs, switch_oid, {"SAI_SWITCH_ATTR_FAST_LINKUP_POLLING_TIMEOUT": str(poll_min)})
        invalid = poll_max + 1
        set_cfg_entry(cfg_db, "SWITCH_FAST_LINKUP", "GLOBAL", [("polling_time", str(invalid))])
        dvs.asic_db.wait_for_field_negative_match(
            "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH",
            switch_oid,
            {"SAI_SWITCH_ATTR_FAST_LINKUP_POLLING_TIMEOUT": str(invalid)},
        )
        expect_switch_attrs(dvs, switch_oid, {"SAI_SWITCH_ATTR_FAST_LINKUP_POLLING_TIMEOUT": str(poll_min)})

    def test_global_config_guard_above_max_rejected(self, dvs, testlog):
        cap_map = require_fast_linkup_capable(dvs)
        guard_min, guard_max = parse_range_or_skip(cap_map, "FAST_LINKUP_GUARD_TIMER_RANGE")
        cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        switch_oid = get_switch_oid(dvs)
        set_cfg_entry(cfg_db, "SWITCH_FAST_LINKUP", "GLOBAL", [("guard_time", str(guard_min))])
        expect_switch_attrs(dvs, switch_oid, {"SAI_SWITCH_ATTR_FAST_LINKUP_GUARD_TIMEOUT": str(guard_min)})
        invalid = guard_max + 1
        set_cfg_entry(cfg_db, "SWITCH_FAST_LINKUP", "GLOBAL", [("guard_time", str(invalid))])
        dvs.asic_db.wait_for_field_negative_match(
            "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH",
            switch_oid,
            {"SAI_SWITCH_ATTR_FAST_LINKUP_GUARD_TIMEOUT": str(invalid)},
        )
        expect_switch_attrs(dvs, switch_oid, {"SAI_SWITCH_ATTR_FAST_LINKUP_GUARD_TIMEOUT": str(guard_min)})

    def test_global_config_guard_below_min_rejected(self, dvs, testlog):
        cap_map = require_fast_linkup_capable(dvs)
        guard_min, _ = parse_range_or_skip(cap_map, "FAST_LINKUP_GUARD_TIMER_RANGE")
        if guard_min == 0:
            pytest.skip("Guard range minimum is 0, no below-min value exists")
        cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        switch_oid = get_switch_oid(dvs)
        set_cfg_entry(cfg_db, "SWITCH_FAST_LINKUP", "GLOBAL", [("guard_time", str(guard_min))])
        expect_switch_attrs(dvs, switch_oid, {"SAI_SWITCH_ATTR_FAST_LINKUP_GUARD_TIMEOUT": str(guard_min)})
        invalid = guard_min - 1
        set_cfg_entry(cfg_db, "SWITCH_FAST_LINKUP", "GLOBAL", [("guard_time", str(invalid))])
        dvs.asic_db.wait_for_field_negative_match(
            "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH",
            switch_oid,
            {"SAI_SWITCH_ATTR_FAST_LINKUP_GUARD_TIMEOUT": str(invalid)},
        )
        expect_switch_attrs(dvs, switch_oid, {"SAI_SWITCH_ATTR_FAST_LINKUP_GUARD_TIMEOUT": str(guard_min)})

    def test_port_fast_linkup_enable(self, dvs, testlog):
        first_port = get_first_port(dvs)
        cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        port_tbl = swsscommon.Table(cfg_db, "PORT")
        status, values = port_tbl.get(first_port)
        assert status
        current = {k: v for k, v in values}
        current["fast_linkup"] = "true"
        port_tbl.set(first_port, swsscommon.FieldValuePairs(list(current.items())))
        time.sleep(1)
        current["fast_linkup"] = "false"
        port_tbl.set(first_port, swsscommon.FieldValuePairs(list(current.items())))
        time.sleep(1)

    def test_port_fast_linkup_unsupported_no_blocking(self, dvs, testlog):
        require_fast_linkup_not_capable(dvs)
        first_port = get_first_port(dvs)
        current, port_oid = set_port_admin_and_wait_down(dvs, first_port)
        update = dict(current)
        update["admin_status"] = "up"
        update["fast_linkup"] = "true"
        cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        port_tbl = swsscommon.Table(cfg_db, "PORT")
        port_tbl.set(first_port, swsscommon.FieldValuePairs(list(update.items())))
        time.sleep(1)
        dvs.asic_db.wait_for_field_match(
            "ASIC_STATE:SAI_OBJECT_TYPE_PORT",
            port_oid,
            {"SAI_PORT_ATTR_ADMIN_STATE": "true"},
        )

    def test_port_fast_linkup_empty_value_rejected(self, dvs, testlog):
        first_port = get_first_port(dvs)
        current, port_oid = set_port_admin_and_wait_down(dvs, first_port)
        invalid = dict(current)
        invalid["admin_status"] = "up"
        invalid["fast_linkup"] = ""
        cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        port_tbl = swsscommon.Table(cfg_db, "PORT")
        port_tbl.set(first_port, swsscommon.FieldValuePairs(list(invalid.items())))
        time.sleep(1)
        dvs.asic_db.wait_for_field_match(
            "ASIC_STATE:SAI_OBJECT_TYPE_PORT",
            port_oid,
            {"SAI_PORT_ATTR_ADMIN_STATE": "true"},
        )

    def test_port_fast_linkup_invalid_value_rejected(self, dvs, testlog):
        first_port = get_first_port(dvs)
        current, port_oid = set_port_admin_and_wait_down(dvs, first_port)
        invalid = dict(current)
        invalid["admin_status"] = "up"
        invalid["fast_linkup"] = "not_bool"
        cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        port_tbl = swsscommon.Table(cfg_db, "PORT")
        port_tbl.set(first_port, swsscommon.FieldValuePairs(list(invalid.items())))
        time.sleep(1)
        dvs.asic_db.wait_for_field_match(
            "ASIC_STATE:SAI_OBJECT_TYPE_PORT",
            port_oid,
            {"SAI_PORT_ATTR_ADMIN_STATE": "true"},
        )

    def test_appdb_port_fast_linkup_true_false_paths(self, dvs, testlog):
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        first_port = get_first_port(dvs)
        set_cfg_entry(app_db, "PORT_TABLE", first_port, [("fast_linkup", "true")])
        set_cfg_entry(app_db, "PORT_TABLE", first_port, [("fast_linkup", "false")])

    def test_appdb_port_fast_linkup_invalid_rejects_whole_update(self, dvs, testlog):
        first_port = get_first_port(dvs)
        _, port_oid = set_port_admin_and_wait_down(dvs, first_port)
        app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        set_cfg_entry(
            app_db,
            "PORT_TABLE",
            first_port,
            [("admin_status", "up"), ("fast_linkup", "not_bool")],
        )
        dvs.asic_db.wait_for_field_negative_match(
            "ASIC_STATE:SAI_OBJECT_TYPE_PORT",
            port_oid,
            {"SAI_PORT_ATTR_ADMIN_STATE": "true"},
        )
        dvs.asic_db.wait_for_field_match(
            "ASIC_STATE:SAI_OBJECT_TYPE_PORT",
            port_oid,
            {"SAI_PORT_ATTR_ADMIN_STATE": "false"},
        )
