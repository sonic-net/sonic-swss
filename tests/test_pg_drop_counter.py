from swsscommon import swsscommon
import os
import re
import time
import json
import pytest
import redis


pg_drop_attr = "SAI_INGRESS_PRIORITY_GROUP_STAT_DROPPED_PACKETS"

class TestPGDropCounter(object):
    DEFAULT_POLL_INTERVAL = 10

    def enable_unittests(self, dvs, status):
        db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        ntf = swsscommon.NotificationProducer(db, "SAI_VS_UNITTEST_CHANNEL")
        fvp = swsscommon.FieldValuePairs()
        ntf.send("enable_unittests", status, fvp)

    def set_counter(self, dvs, obj_id, attr, val):

        db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        ntf = swsscommon.NotificationProducer(db, "SAI_VS_UNITTEST_CHANNEL")

        r = redis.Redis(unix_socket_path=dvs.redis_sock, db=swsscommon.ASIC_DB)
        rid = r.hget("VIDTORID", obj_id)

        assert rid is not None
        fvp = swsscommon.FieldValuePairs([(attr, val)])
        key = rid

        ntf.send("set_stats", key, fvp)

    def populate_asic(self, dvs, val):
        for obj_id in self.pgs:
            self.set_counter(dvs, obj_id, pg_drop_attr, val)

    def verify_value(self, dvs, obj_ids, entry_name, expected_value):
        counters_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
        table = swsscommon.Table(counters_db, "COUNTERS")

        for obj_id in obj_ids:
            ret = table.get(obj_id)
            status = ret[0]
            assert status
            keyvalues = ret[1]
            found = False
            for key, value in keyvalues:
              if key == entry_name:
                  assert value == expected_value, "Saved value not the same as expected"
                  found = True
            assert found, "no such entry found"

    def get_oids(self, dvs, obj_type):
        db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        tbl = swsscommon.Table(db, "ASIC_STATE:{0}".format(obj_type))
        keys = tbl.getKeys()
        return keys

    def set_up_flex_counter(self, dvs):
        for pg in self.pgs:
            dvs.runcmd("redis-cli -n 5 hset 'FLEX_COUNTER_TABLE:PG_DROP_STAT_COUNTER:{}' ".format(pg) + "PG_COUNTER_ID_LIST '{}'".format(pg_drop_attr))

        dvs.runcmd("redis-cli -n 4 hset 'FLEX_COUNTER_TABLE|PG_DROP' 'FLEX_COUNTER_STATUS' 'enable'")

        self.populate_asic(dvs, "0")


    def test_pg_drop(self, dvs):
        try:
            self.pgs = self.get_oids(dvs, "SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP")
            self.set_up_flex_counter(dvs)

            self.populate_asic(dvs, "100")
            time.sleep(self.DEFAULT_POLL_INTERVAL + 1)
            self.verify_value(dvs, self.pgs, pg_drop_attr , "100")

            self.populate_asic(dvs, "123")
            time.sleep(self.DEFAULT_POLL_INTERVAL + 1)
            self.verify_value(dvs, self.pgs, pg_drop_attr, "123")
        finally:
            dvs.runcmd("redis-cli -n 4 DEL 'FLEX_COUNTER_TABLE|PG_DROP'")

            for pg in self.pgs:
                dvs.runcmd("redis-cli -n 5 DEL 'FLEX_COUNTER_TABLE:PG_DROP_STAT_COUNTER:{}' ".format(pg))


