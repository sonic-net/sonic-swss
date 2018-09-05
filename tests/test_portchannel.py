from swsscommon import swsscommon
import time
import re
import json

def test_PortChannel(dvs):

    # create port channel
    db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
    ps = swsscommon.ProducerStateTable(db, "LAG_TABLE")
    fvs = swsscommon.FieldValuePairs([("admin", "up"), ("mtu", "1500")])

    ps.set("PortChannel0001", fvs)

    # create port channel member
    ps = swsscommon.ProducerStateTable(db, "LAG_MEMBER_TABLE")
    fvs = swsscommon.FieldValuePairs([("status", "enabled")])

    ps.set("PortChannel0001:Ethernet0", fvs)

    time.sleep(1)

    # check asic db table
    asicdb = swsscommon.DBConnector(1, dvs.redis_sec_sock, 0)

    lagtbl = swsscommon.Table(asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_LAG")

    lags = lagtbl.getKeys()
    assert len(lags) == 1

    lagmtbl = swsscommon.Table(asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER")

    lagms = lagmtbl.getKeys()
    assert len(lagms) == 1

    (status, fvs) = lagmtbl.get(lagms[0])
    for fv in fvs:
        if fv[0] == "SAI_LAG_MEMBER_ATTR_LAG_ID":
            assert fv[1] == lags[0]
        elif fv[0] == "SAI_LAG_MEMBER_ATTR_PORT_ID":
            assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet0"
        else:
            assert False
