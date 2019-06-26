from swsscommon import swsscommon
import time
import re
import json

def test_DirectedBroadcast(dvs, testlog):

    db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
    adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

    # create vlan in config db
    tbl = swsscommon.Table(db, "VLAN")
    fvs = swsscommon.FieldValuePairs([("vlanid", "100")])
    tbl.set("Vlan100", fvs)

    # create a vlan member in config db
    tbl = swsscommon.Table(db, "VLAN_MEMBER")
    fvs = swsscommon.FieldValuePairs([("tagging_mode", "tagged")])
    tbl.set("Vlan100|Ethernet24", fvs)

    time.sleep(1)

    # create vlan interface in config db
    tbl = swsscommon.Table(db, "VLAN_INTERFACE")
    fvs = swsscommon.FieldValuePairs([("family", "IPv4")])
    tbl.set("Vlan100", fvs)
    tbl.set("Vlan100|192.169.0.1/27", fvs)

    time.sleep(1)

    # check vlan in asic db
    atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")
    keys = atbl.getKeys()
    vlan_oid = None

    for key in keys:
        if key == dvs.asicdb.default_vlan_id:
            continue

        (status, fvs) = atbl.get(key)
        assert status == True

        if fvs[0][0] == "SAI_VLAN_ATTR_VLAN_ID":
            assert fvs[0][1] == '100'
            vlan_oid = key

    assert vlan_oid != None

    # check router interface in asic db
    atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
    keys = atbl.getKeys()
    rif_oid = None

    for key in keys:
        (status, fvs) = atbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_ROUTER_INTERFACE_ATTR_VLAN_ID":
                assert vlan_oid == fv[1]
                rif_oid = key

    assert rif_oid != None

    # check neighbor entry in asic db
    atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
    keys = atbl.getKeys()
    dir_bcast = False

    for key in keys:
        neigh = json.loads(key)

        if neigh['ip'] == "192.169.0.31":
            dir_bcast = True
            assert neigh['rif'] == rif_oid
            (status, fvs) = atbl.get(key)
            assert status == True
            if fvs[0][0] == "SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS":
                assert fvs[0][1] == "FF:FF:FF:FF:FF:FF"

    assert dir_bcast

    # Explicitly add a neighbor entry with BCAST MAC and check if its in ASIC_DB
    dvs.runcmd("ip neigh replace 192.169.0.30 lladdr FF:FF:FF:FF:FF:FF dev Vlan100")

    keys = atbl.getKeys()
    for key in keys:
        neigh = json.loads(key)

        if neigh['ip'] == "192.169.0.30":
            assert False
