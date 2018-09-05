from swsscommon import swsscommon
import time
import os

def test_PortAutoNeg(dvs):

    db = swsscommon.DBConnector(0, dvs.redis_sock, 0)

    tbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")

    # set autoneg = false and speed = 1000
    fvs = swsscommon.FieldValuePairs([("autoneg","1"), ("speed", "1000")])

    tbl.set("Ethernet0", fvs)

    time.sleep(1)

    adb = swsscommon.DBConnector(1, dvs.redis_sec_sock, 0)

    atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
    (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
    assert status == True

    assert "SAI_PORT_ATTR_AUTO_NEG_MODE" in [fv[0] for fv in fvs]
    assert "SAI_PORT_ATTR_ADVERTISED_SPEED" in [fv[0] for fv in fvs]
    for fv in fvs:
        if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
            assert fv[1] == "true"
        elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
            assert fv[1] == "1:1000"

    # set speed = 100
    fvs = swsscommon.FieldValuePairs([("speed", "100")])
 
    tbl.set("Ethernet0", fvs)

    time.sleep(1)

    (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
    assert status == True

    for fv in fvs:
        if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
            assert fv[1] == "true"
        elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
            assert fv[1] == "1:100"

    # change autoneg to false 
    fvs = swsscommon.FieldValuePairs([("autoneg","0")])

    tbl.set("Ethernet0", fvs)

    time.sleep(1)

    (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
    assert status == True

    assert "SAI_PORT_ATTR_AUTO_NEG_MODE" in [fv[0] for fv in fvs]
    assert "SAI_PORT_ATTR_ADVERTISED_SPEED" in [fv[0] for fv in fvs]
    assert "SAI_PORT_ATTR_SPEED" in [fv[0] for fv in fvs]
    for fv in fvs:
        if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
            assert fv[1] == "false"
        elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
            assert fv[1] == "1:100"
        elif fv[0] == "SAI_PORT_ATTR_SPEED":
            assert fv[1] == "100"

    # set speed = 1000
    fvs = swsscommon.FieldValuePairs([("speed", "1000")])
 
    tbl.set("Ethernet0", fvs)

    time.sleep(1)

    (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
    assert status == True

    for fv in fvs:
        if fv[0] == "SAI_PORT_ATTR_AUTO_NEG_MODE":
            assert fv[1] == "false"
        elif fv[0] == "SAI_PORT_ATTR_ADVERTISED_SPEED":
            assert fv[1] == "1:100"
        elif fv[0] == "SAI_PORT_ATTR_SPEED":
            assert fv[1] == "1000"
