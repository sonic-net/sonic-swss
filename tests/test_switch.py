import time
import pytest

from swsscommon import swsscommon


def create_entry(tbl, key, pairs):
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)
    time.sleep(1)


def get_exist_entry(dvs, table):
    db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    tbl =  swsscommon.Table(db, table)
    entries = list(tbl.getKeys())
    return entries[0]


def create_entry_pst(db, table, separator, key, pairs):
    tbl = swsscommon.ProducerStateTable(db, table)
    create_entry(tbl, key, pairs)


def check_object(db, table, key, expected_attributes):
    tbl =  swsscommon.Table(db, table)
    keys = tbl.getKeys()
    assert key in keys, "The desired key is not presented"

    status, fvs = tbl.get(key)
    assert status, "Got an error when get a key"

    assert len(fvs) >= len(expected_attributes), "Incorrect attributes"

    attr_keys = {entry[0] for entry in fvs}

    for name, value in fvs:
        if name in expected_attributes:
            assert expected_attributes[name] == value, "Wrong value %s for the attribute %s = %s" % \
                                               (value, name, expected_attributes[name])


def vxlan_switch_test(dvs, oid, port, mac, mask, sport, security="false"):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(
        app_db,
        "SWITCH_TABLE", ':', "switch",
        [
            ("vxlan_port", port),
            ("vxlan_router_mac", mac),
            ("vxlan_mask", mask),
            ("vxlan_sport", sport),
            ("vxlan_security", security),
        ],
    )
    time.sleep(2)

    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    expected_attrs = {
        'SAI_SWITCH_ATTR_VXLAN_DEFAULT_PORT': port,
        'SAI_SWITCH_ATTR_VXLAN_DEFAULT_ROUTER_MAC': mac,
        'SAI_SWITCH_TUNNEL_ATTR_VXLAN_UDP_SPORT_MASK': mask,
        'SAI_SWITCH_TUNNEL_ATTR_VXLAN_UDP_SPORT': sport,
        'SAI_SWITCH_TUNNEL_ATTR_VXLAN_UDP_SPORT_SECURITY': security,
    }
    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH", oid, expected_attrs)


def ecmp_lag_hash_offset_test(dvs, oid, lag_offset, ecmp_offset):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(
        app_db,
        "SWITCH_TABLE", ':', "switch",
        [
            ("ecmp_hash_offset", ecmp_offset),
            ("lag_hash_offset", lag_offset)
        ],
    )
    time.sleep(2)

    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH", oid,
        {
            'SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_OFFSET': ecmp_offset,
            'SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_OFFSET': lag_offset,
        }
    )


def credit_watchdog_non_voq_test(dvs, oid):
    """Verify credit watchdog fields are skipped on non-VOQ switches.

    credit_watchdog and credit_watchdog_timer are valid only for VOQ switches
    (validonly SAI_SWITCH_ATTR_TYPE == SAI_SWITCH_TYPE_VOQ). On a non-VOQ
    switch, orchagent must skip them without blocking the rest of the batch.
    """
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(
        app_db,
        "SWITCH_TABLE", ':', "switch",
        [
            ("credit_watchdog", "0"),
            ("credit_watchdog_timer", "600"),
            ("fdb_aging_time", "777"),
        ],
    )
    time.sleep(2)

    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

    # The supported attribute from the same batch must still be applied
    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH", oid,
        {
            'SAI_SWITCH_ATTR_FDB_AGING_TIME': "777",
        }
    )

    # The VOQ-only attributes must not have been programmed
    tbl = swsscommon.Table(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")
    status, fvs = tbl.get(oid)
    assert status, "Got an error when get a key"
    attr_names = {fv[0] for fv in fvs}
    assert "SAI_SWITCH_ATTR_CREDIT_WD" not in attr_names, \
        "credit_watchdog was programmed on a non-VOQ switch"
    assert "SAI_SWITCH_ATTR_CREDIT_WD_TIMER" not in attr_names, \
        "credit_watchdog_timer was programmed on a non-VOQ switch"


class TestSwitch(object):
    '''
    Test- Check switch attributes
    '''
    def test_switch_attribute(self, dvs, testlog):
        switch_oid = get_exist_entry(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")
        vxlan_switch_test(dvs, switch_oid, "12345", "00:01:02:03:04:05", "20", "54321")

        vxlan_switch_test(dvs, switch_oid, "56789", "00:0A:0B:0C:0D:0E", "15", "56789")

        vxlan_switch_test(dvs, switch_oid, "56789", "00:0A:0B:0C:0D:0E", "15", "56789", "true")

        vxlan_switch_test(dvs, switch_oid, "56789", "00:0A:0B:0C:0D:0E", "15", "56789", "invalid")

        ecmp_lag_hash_offset_test(dvs, switch_oid, "10", "10")

    def test_credit_watchdog_non_voq(self, dvs, testlog):
        '''
        Test credit watchdog fields are skipped on a non-VOQ switch
        '''
        switch_oid = get_exist_entry(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")
        credit_watchdog_non_voq_test(dvs, switch_oid)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
