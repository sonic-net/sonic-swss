from swsscommon import swsscommon
import time
import re
import json

def test_VlanMemberCreation(dvs):

    db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
    adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)


    # create vlan in config db
    tbl = swsscommon.Table(db, "VLAN")
    fvs = swsscommon.FieldValuePairs([("vlanid", "2")])
    tbl.set("Vlan2", fvs)

    time.sleep(1)

    # check vlan in asic db
    atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN")

    keys = atbl.getKeys()
    assert len(keys) == 2

    vlan_oid = None

    for k in keys:
        if k == dvs.asicdb.default_vlan_id:
            continue

        (status, fvs) = atbl.get(k)
        assert status == True

        if fvs[0][0] == "SAI_VLAN_ATTR_VLAN_ID":
            assert fvs[0][1] == '2'
            vlan_oid = k

    assert vlan_oid != None

    # create vlan member in config db
    tbl = swsscommon.Table(db, "VLAN_MEMBER")
    fvs = swsscommon.FieldValuePairs([("tagging_mode", "untagged")])
    tbl.set("Vlan2|Ethernet0", fvs)

    time.sleep(1)

    # check vlan member in asic db
    bridge_port_map = {}
    atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT")
    keys = atbl.getKeys()
    for k in keys:
        (status, fvs) = atbl.get(k)
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_BRIDGE_PORT_ATTR_PORT_ID":
                bridge_port_map[k] = fv[1]
            
    atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER")
    keys = atbl.getKeys()
    assert len(keys) == 1

    (status, fvs) = atbl.get(keys[0])
    assert status == True
    for fv in fvs:
        if fv[0] == "SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE":
            assert fv[1] == "SAI_VLAN_TAGGING_MODE_UNTAGGED"
        elif fv[0] == "SAI_VLAN_MEMBER_ATTR_VLAN_ID":
            assert fv[1] == vlan_oid
        elif fv[0] == "SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID":
            assert dvs.asicdb.portoidmap[bridge_port_map[fv[1]]] == "Ethernet0"
        else:
            assert False

    # check pvid of the port
    atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
    (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
    assert status == True

    assert "SAI_PORT_ATTR_PORT_VLAN_ID" in [fv[0] for fv in fvs]
    for fv in fvs:
        if fv[0] == "SAI_PORT_ATTR_PORT_VLAN_ID":
            assert fv[1] == "2"

    # check vlan tag for the host interface
    atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF")
    (status, fvs) = atbl.get(dvs.asicdb.hostifnamemap["Ethernet0"])
    assert status == True

    assert "SAI_HOSTIF_ATTR_VLAN_TAG" in [fv[0] for fv in fvs]
    for fv in fvs:
        if fv[0] == "SAI_HOSTIF_ATTR_VLAN_TAG":
            assert fv[1] == "SAI_HOSTIF_VLAN_TAG_KEEP"
