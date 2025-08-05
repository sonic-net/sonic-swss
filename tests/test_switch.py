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

def get_fvs_for_a_key(db, table, key):
    tbl =  swsscommon.Table(db, table)
    status, fvs = tbl.get(key)
    assert status, "Got an error when fetching fvs for a key"
    return fvs

def vxlan_switch_test(dvs, table, key, oid, port, mac, mask, sport):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(
        app_db,
        table, ':', key,
        [
            ("vxlan_port", port),
            ("vxlan_router_mac", mac),
            ("vxlan_mask", mask),
            ("vxlan_sport", sport),
        ],
    )
    time.sleep(2)

    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH", oid,
        {
            'SAI_SWITCH_ATTR_VXLAN_DEFAULT_PORT': port,
            'SAI_SWITCH_ATTR_VXLAN_DEFAULT_ROUTER_MAC': mac,
            'SAI_SWITCH_TUNNEL_ATTR_VXLAN_UDP_SPORT_MASK': mask,
            'SAI_SWITCH_TUNNEL_ATTR_VXLAN_UDP_SPORT': sport,
        }
    )

def create_a_hash_object(dvs, hash_object_name, hash_field_list_value):
    # Get existing hash objects' OIDs.
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    asic_db_hash_table = swsscommon.Table(asic_db,
                                          "ASIC_STATE:SAI_OBJECT_TYPE_HASH")
    existing_keys = asic_db_hash_table.getKeys()

    # Add a new hash object.
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(app_db, "HASH_TABLE", ":", hash_object_name,
                     [("hash_field_list", hash_field_list_value)])
    time.sleep(2)

    # Get new hash object's OID.
    keys = asic_db_hash_table.getKeys()

    assert len(keys) == len(existing_keys) + 1, \
        "Hash object with name %s is not created or already exists." % (hash_object_name)

    for key in keys:
        if key not in existing_keys:
            return key

def switch_hash_object_ref_test(dvs, switch_table, switch_name, switch_oid):
    # Create a hash object.
    hash_object_name_1 = "hash_config_1"
    hash_field_list_value_1 = "[\"src_ip\", \"dst_ip\"]"
    hash_object_oid_1 = create_a_hash_object(
        dvs, hash_object_name_1, hash_field_list_value_1)

    # Set switch ECMP's IPv4 hash attribute to the created hash object.
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(
        app_db,
        switch_table, ':', switch_name,
        [
            ("ecmp_hash_ipv4", hash_object_name_1),
        ],
    )
    time.sleep(2)

    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH", switch_oid,
        {
            'SAI_SWITCH_ATTR_ECMP_HASH_IPV4': hash_object_oid_1,
        }
    )

    app_state_db = \
        swsscommon.DBConnector(swsscommon.APPL_STATE_DB, dvs.redis_sock, 0)
    check_object(app_state_db, switch_table, switch_name,
        {
            "ecmp_hash_ipv4": hash_object_name_1,
        }
    )

    # Create a new hash object.
    hash_object_name_2 = "hash_config_2"
    hash_field_list_value_2 = "[\"src_ip\"]"
    hash_object_oid_2 = create_a_hash_object(
        dvs, hash_object_name_2, hash_field_list_value_2)

    # Update switch ECMP's IPv4 hash attribute to the new hash object.
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(
        app_db,
        switch_table, ':', switch_name,
        [
            ("ecmp_hash_ipv4", hash_object_name_2),
        ],
    )
    time.sleep(2)

    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH", switch_oid,
        {
            'SAI_SWITCH_ATTR_ECMP_HASH_IPV4': hash_object_oid_2,
        }
    )

    app_state_db = \
        swsscommon.DBConnector(swsscommon.APPL_STATE_DB, dvs.redis_sock, 0)
    check_object(app_state_db, switch_table, switch_name,
        {
            "ecmp_hash_ipv4": hash_object_name_2,
        }
    )

def switch_ecmp_hash_test(
    dvs, table, key, oid, hash_algorithm, asic_hash_algorithm, hash_seed,
    hash_offset):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(
        app_db,
        table, ':', key,
        [
            ("ecmp_hash_algorithm", hash_algorithm),
            ("ecmp_hash_seed", hash_seed),
            ("ecmp_hash_offset", hash_offset)
        ],
    )
    time.sleep(2)

    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH", oid,
        {
            'SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM': asic_hash_algorithm,
            'SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_SEED': hash_seed,
            'SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_OFFSET': hash_offset
        }
    )

    app_state_db = \
        swsscommon.DBConnector(swsscommon.APPL_STATE_DB, dvs.redis_sock, 0)
    check_object(app_state_db, table, key,
        {
            "ecmp_hash_algorithm": hash_algorithm,
            "ecmp_hash_seed": hash_seed,
            "ecmp_hash_offset": hash_offset
        }
    )

def switch_lag_hash_test(
    dvs, table, key, oid, hash_algorithm, asic_hash_algorithm, hash_seed,
    hash_offset):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(
        app_db,
        table, ':', key,
        [
            ("lag_hash_algorithm", hash_algorithm),
            ("lag_hash_seed", hash_seed),
            ("lag_hash_offset", hash_offset)
        ],
    )
    time.sleep(2)

    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH", oid,
        {
            'SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM': asic_hash_algorithm,
            'SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_SEED': hash_seed,
            'SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_OFFSET': hash_offset
        }
    )

    app_state_db = \
        swsscommon.DBConnector(swsscommon.APPL_STATE_DB, dvs.redis_sock, 0)
    check_object(app_state_db, table, key,
        {
            "lag_hash_algorithm": hash_algorithm,
            "lag_hash_seed": hash_seed,
            "lag_hash_offset": hash_offset
        }
    )

def switch_hash_test_invalid_attribute(dvs, table, key, oid, field, value):
    # Get current switch attributes in ASIC_DB and APPL_STATE_DB.
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    original_asic_fvs = get_fvs_for_a_key(asic_db,
                                          "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH",
                                          oid)
    app_state_db = swsscommon.DBConnector(swsscommon.APPL_STATE_DB,
                                          dvs.redis_sock, 0)
    original_app_state_fvs = \
        get_fvs_for_a_key(app_state_db, table, key)

    # Intend to configure unsupported field/value.
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(app_db, table, ':', key, [(field, value)])
    time.sleep(2)

    # Verify that switch attributes in ASIC_DB don't change, as the operation
    # should fail.
    asic_fvs = get_fvs_for_a_key(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH",
                                 oid)
    assert original_asic_fvs == asic_fvs, \
        "Error: Switch attributes in ASIC_DB changed."

    # Verify that APPL_STATE_DB entry doesn't change.
    app_state_fvs = get_fvs_for_a_key(app_state_db, table, key)
    assert original_app_state_fvs == app_state_fvs, \
        "Error: Switch attributes in APPL_STATE_DB changed."

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


class TestSwitch(object):
    # APPL_DB table name and key.
    SWITCH_TABLE = "SWITCH_TABLE"
    SWITCH = "switch"
    '''
    Test- Check switch attributes
    '''
    def test_switch_attribute(self, dvs, testlog):
        switch_oid = get_exist_entry(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")

        vxlan_switch_test(
            dvs, self.SWITCH_TABLE, self.SWITCH, switch_oid, "12345", "00:01:02:03:04:05", "20", "54321")

        vxlan_switch_test(
            dvs, self.SWITCH_TABLE, self.SWITCH, switch_oid, "56789", "00:0A:0B:0C:0D:0E", "15", "56789")

        switch_ecmp_hash_test(
            dvs, self.SWITCH_TABLE, self.SWITCH, switch_oid, "xor",
            "SAI_HASH_ALGORITHM_XOR", "10", "4")

        switch_ecmp_hash_test(
            dvs, self.SWITCH_TABLE, self.SWITCH, switch_oid, "crc_32hi",
            "SAI_HASH_ALGORITHM_CRC_32HI", "10", "8")

        switch_lag_hash_test(
            dvs, self.SWITCH_TABLE, self.SWITCH, switch_oid, "xor",
            "SAI_HASH_ALGORITHM_XOR", "10", "4")

        switch_lag_hash_test(
            dvs, self.SWITCH_TABLE, self.SWITCH, switch_oid, "crc_32hi",
            "SAI_HASH_ALGORITHM_CRC_32HI", "10", "8")

        switch_hash_test_invalid_attribute(
            dvs, self.SWITCH_TABLE, self.SWITCH, switch_oid,
            "unsupported_field", "invalid_value")

        switch_hash_test_invalid_attribute(
            dvs, self.SWITCH_TABLE, self.SWITCH, switch_oid,
            "ecmp_hash_algorithm", "invalid_hash_algorithm")

        switch_hash_object_ref_test(
            dvs, self.SWITCH_TABLE, self.SWITCH, switch_oid)

        ecmp_lag_hash_offset_test(dvs, switch_oid, "10", "10")


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
