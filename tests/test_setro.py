from swsscommon import swsscommon
import time
import json
from pprint import pprint

def test_SetReadOnlyAttribute(dvs):

    db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

    tbl = swsscommon.Table(db, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")

    keys = tbl.getKeys()

    assert len(keys) == 1

    swVid = keys[0]
 
    rc = swsscommon.RedisClient(db)
 
    swRid = rc.hget("VIDTORID", swVid)

    ntf = swsscommon.NotificationProducer(db, "SAI_VS_UNITTEST_CHANNEL")

    fvp = swsscommon.FieldValuePairs()

    ntf.send("enable_unittests", "true", fvp)

    fvp = swsscommon.FieldValuePairs([('SAI_SWITCH_ATTR_PORT_MAX_MTU', '42')])

    key = "SAI_OBJECT_TYPE_SWITCH:" + swRid

    ntf.send("set_ro", key, fvp)

    # make action on appdb so orchagent will get RO value
    # read asic db to see if orchagent behaved correctly
