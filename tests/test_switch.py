from swsscommon import swsscommon
import time


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


def vxlan_switch_test(dvs, oid, port, mac):
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    create_entry_pst(
        app_db,
        "SWITCH_TABLE", ':', "switch",
        [
            ("vxlan_port", port),
            ("vxlan_router_mac", mac)
        ],
    )
    time.sleep(2)

    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    check_object(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH", oid,
        {
            'SAI_SWITCH_ATTR_VXLAN_DEFAULT_PORT': port,
            'SAI_SWITCH_ATTR_VXLAN_DEFAULT_ROUTER_MAC': mac,
        }
    )


class TestSwitch(object):
    
    '''
    Test- Check switch attributes
    '''
    def test_switch_attribute(self, dvs, testlog):
        switch_oid = get_exist_entry(dvs, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")
        
        vxlan_switch_test(dvs, switch_oid, "12345", "00:01:02:03:04:05")
        
        vxlan_switch_test(dvs, switch_oid, "56789", "00:0A:0B:0C:0D:0E")

