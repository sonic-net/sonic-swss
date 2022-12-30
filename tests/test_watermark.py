import os
import re
import time
import json
import pytest
import redis

from swsscommon import swsscommon

class SaiWmStats:
    queue_shared = "SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES"
    pg_shared = "SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_BYTES"
    pg_headroom = "SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_BYTES"
    buffer_pool = "SAI_BUFFER_POOL_STAT_WATERMARK_BYTES"
    hdrm_pool = "SAI_BUFFER_POOL_STAT_XOFF_ROOM_WATERMARK_BYTES"


class WmTables:
    persistent = "PERSISTENT_WATERMARKS"
    periodic = "PERIODIC_WATERMARKS"
    user = "USER_WATERMARKS"


class WmFCEntry:
    queue_stats_entry = {"QUEUE_COUNTER_ID_LIST": SaiWmStats.queue_shared}
    pg_stats_entry = {"PG_COUNTER_ID_LIST": "{},{}".format(SaiWmStats.pg_shared, SaiWmStats.pg_headroom)}
    buffer_stats_entry = {"BUFFER_POOL_COUNTER_ID_LIST": "{},{}".format(SaiWmStats.buffer_pool, SaiWmStats.hdrm_pool)}


class TestWatermark(object):

    DEFAULT_TELEMETRY_INTERVAL = 120
    NEW_INTERVAL = 5
    DEFAULT_POLL_INTERVAL = 10

    def setup_dbs(self, dvs):
        self.asic_db = dvs.get_asic_db()
        self.counters_db = dvs.get_counters_db()
        self.config_db = dvs.get_config_db()
        self.flex_db = dvs.get_flex_db()

    def enable_unittests(self, dvs, status):
        db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        ntf = swsscommon.NotificationProducer(db, "SAI_VS_UNITTEST_CHANNEL")
        fvp = swsscommon.FieldValuePairs()
        ntf.send("enable_unittests", status, fvp)

    def set_counter(self, dvs, obj_type, obj_id, attr, val):

        db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        ntf = swsscommon.NotificationProducer(db, "SAI_VS_UNITTEST_CHANNEL")

        r = redis.Redis(unix_socket_path=dvs.redis_sock, db=swsscommon.ASIC_DB,
                        encoding="utf-8", decode_responses=True)
        rid = r.hget("VIDTORID", obj_id)

        assert rid is not None

        fvp = swsscommon.FieldValuePairs([(attr, val)])
        key = rid

        # explicit convert unicode string to str for python2
        ntf.send("set_stats", str(key), fvp)

    def populate_asic(self, dvs, obj_type, attr, val):

        db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)

        if obj_type == "SAI_OBJECT_TYPE_QUEUE":
            oids = self.qs
        elif obj_type == "SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP":
            oids = self.pgs
        else:
            oids = self.buffers

        for obj_id in oids:
            self.set_counter(dvs, obj_type, obj_id, attr, val)

    def populate_asic_all(self, dvs, val):
        self.populate_asic(dvs, "SAI_OBJECT_TYPE_QUEUE", SaiWmStats.queue_shared, val)
        self.populate_asic(dvs, "SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP", SaiWmStats.pg_shared, val)
        self.populate_asic(dvs, "SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP", SaiWmStats.pg_headroom, val)
        self.populate_asic(dvs, "SAI_OBJECT_TYPE_BUFFER_POOL", SaiWmStats.buffer_pool, val)
        self.populate_asic(dvs, "SAI_OBJECT_TYPE_BUFFER_POOL", SaiWmStats.hdrm_pool, val)
        time.sleep(self.DEFAULT_POLL_INTERVAL)

    def verify_value(self, dvs, obj_ids, table_name, watermark_name, expected_value):

        counters_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
        table = swsscommon.Table(counters_db, table_name)

        for obj_id in obj_ids:

            ret = table.get(obj_id)

            status = ret[0]
            assert status
            keyvalues = ret[1]
            found = False
            for key, value in keyvalues:
              if key == watermark_name:
                  assert value == expected_value
                  found = True
            assert found, "no such watermark found"

    def set_up_flex_counter(self, dvs):
        fc_status_enable = {"FLEX_COUNTER_STATUS": "enable"}

        self.config_db.create_entry("FLEX_COUNTER_TABLE",
                                    "PG_WATERMARK",
                                    fc_status_enable)
        self.config_db.create_entry("FLEX_COUNTER_TABLE",
                                    "QUEUE_WATERMARK",
                                    fc_status_enable)
        self.config_db.create_entry("FLEX_COUNTER_TABLE",
                                    "BUFFER_POOL_WATERMARK",
                                    fc_status_enable)

        # Wait for DB's to populate by orchagent
        time.sleep(2)

    def clear_flex_counter(self, dvs):
        for q in self.qs:
            self.flex_db.delete_entry("FLEX_COUNTER_TABLE",
                                     "QUEUE_WATERMARK_STAT_COUNTER:{}".format(q))

        for pg in self.pgs:
            self.flex_db.delete_entry("FLEX_COUNTER_TABLE",
                                     "PG_WATERMARK_STAT_COUNTER:{}".format(pg))

        for buffer in self.buffers:
            self.flex_db.delete_entry("FLEX_COUNTER_TABLE",
                                      "BUFFER_POOL_WATERMARK_STAT_COUNTER:{}".format(buffer))

        self.config_db.delete_entry("FLEX_COUNTER_TABLE", "PG_WATERMARK")
        self.config_db.delete_entry("FLEX_COUNTER_TABLE", "QUEUE_WATERMARK")
        self.config_db.delete_entry("FLEX_COUNTER_TABLE", "BUFFER_POOL_WATERMARK")

    def set_up(self, dvs):
        self.pgs = self.counters_db.db_connection.hgetall("COUNTERS_PG_NAME_MAP").values()
        assert self.pgs is not None and len(self.pgs) > 0
        self.qs = self.counters_db.db_connection.hgetall("COUNTERS_QUEUE_NAME_MAP").values()
        assert self.qs is not None and len(self.pgs) > 0
        self.buffers = self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_POOL")

        self.populate_asic_all(dvs, "0")

        db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
        tbl = swsscommon.Table(db, "COUNTERS_QUEUE_TYPE_MAP")

        self.uc_q = []
        self.mc_q = []
        self.all_q = []

        for q in self.qs:
             if self.qs.index(q) % 16 < 5:
                 tbl.set('', [(q, "SAI_QUEUE_TYPE_UNICAST")])
                 self.uc_q.append(q)
             elif self.qs.index(q) % 16 < 10:
                 tbl.set('', [(q, "SAI_QUEUE_TYPE_MULTICAST")])
                 self.mc_q.append(q)
             else:
                 tbl.set('', [(q, "SAI_QUEUE_TYPE_ALL")])
                 self.all_q.append(q)

    def clear_watermark(self, dvs, data):
        adb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        msg = json.dumps(data, separators=(',',':'))
        adb.publish('WATERMARK_CLEAR_REQUEST', msg)
        time.sleep(1)

    def test_telemetry_period(self, dvs):
        self.setup_dbs(dvs)
        self.set_up_flex_counter(dvs)
        self.set_up(dvs)
        try:
            self.enable_unittests(dvs, "true")

            self.populate_asic_all(dvs, "100")

            time.sleep(self.DEFAULT_TELEMETRY_INTERVAL + 1)

            self.verify_value(dvs, self.pgs, WmTables.periodic, SaiWmStats.pg_shared, "0")
            self.verify_value(dvs, self.pgs, WmTables.periodic, SaiWmStats.pg_headroom, "0")
            self.verify_value(dvs, self.qs, WmTables.periodic, SaiWmStats.queue_shared, "0")
            self.verify_value(dvs, self.buffers, WmTables.periodic, SaiWmStats.buffer_pool, "0")
            self.verify_value(dvs, self.buffers, WmTables.periodic, SaiWmStats.hdrm_pool, "0")

            self.populate_asic_all(dvs, "123")

            interval = {"interval": "5"}
            self.config_db.create_entry("WATERMARK_TABLE",
                                        "TELEMETRY_INTERVAL",
                                        interval)

            time.sleep(self.DEFAULT_TELEMETRY_INTERVAL + 1)
            time.sleep(self.NEW_INTERVAL + 1)

            self.verify_value(dvs, self.pgs, WmTables.periodic, SaiWmStats.pg_shared, "0")
            self.verify_value(dvs, self.pgs, WmTables.periodic, SaiWmStats.pg_headroom, "0")
            self.verify_value(dvs, self.qs, WmTables.periodic, SaiWmStats.queue_shared, "0")
            self.verify_value(dvs, self.buffers, WmTables.periodic, SaiWmStats.buffer_pool, "0")
            self.verify_value(dvs, self.buffers, WmTables.periodic, SaiWmStats.hdrm_pool, "0")

        finally:
            self.clear_flex_counter(dvs)
            self.enable_unittests(dvs, "false")

    @pytest.mark.skip(reason="This test is not stable enough")
    def test_lua_plugins(self, dvs):

        self.setup_dbs(dvs)
        self.set_up(dvs)
        try:
            self.set_up_flex_counter(dvs)
            self.enable_unittests(dvs, "true")

            self.populate_asic_all(dvs, "192")

            for table_name in [WmTables.user, WmTables.persistent]:
                self.verify_value(dvs, self.selected_qs, table_name, SaiWmStats.queue_shared, "192")
                self.verify_value(dvs, self.selected_pgs, table_name, SaiWmStats.pg_headroom, "192")
                self.verify_value(dvs, self.selected_pgs, table_name, SaiWmStats.pg_shared, "192")
                self.verify_value(dvs, self.buffers, table_name, SaiWmStats.buffer_pool, "192")
                self.verify_value(dvs, self.buffers, table_name, SaiWmStats.hdrm_pool, "192")

            self.populate_asic_all(dvs, "96")

            for table_name in [WmTables.user, WmTables.persistent]:
                self.verify_value(dvs, self.selected_qs, table_name, SaiWmStats.queue_shared, "192")
                self.verify_value(dvs, self.selected_pgs, table_name, SaiWmStats.pg_headroom, "192")
                self.verify_value(dvs, self.selected_pgs, table_name, SaiWmStats.pg_shared, "192")
                self.verify_value(dvs, self.buffers, table_name, SaiWmStats.buffer_pool, "192")
                self.verify_value(dvs, self.buffers, table_name, SaiWmStats.hdrm_pool, "192")

            self.populate_asic_all(dvs, "288")

            for table_name in [WmTables.user, WmTables.persistent]:
                self.verify_value(dvs, self.selected_qs, table_name, SaiWmStats.queue_shared, "288")
                self.verify_value(dvs, self.selected_pgs, table_name, SaiWmStats.pg_headroom, "288")
                self.verify_value(dvs, self.selected_pgs, table_name, SaiWmStats.pg_shared, "288")
                self.verify_value(dvs, self.buffers, table_name, SaiWmStats.buffer_pool, "288")
                self.verify_value(dvs, self.buffers, table_name, SaiWmStats.hdrm_pool, "288")

        finally:
            self.clear_flex_counter(dvs)
            self.enable_unittests(dvs, "false")

    @pytest.mark.skip(reason="This test is not stable enough")
    def test_clear(self, dvs):

        self.setup_dbs(dvs)
        self.set_up(dvs)
        self.enable_unittests(dvs, "true")

        self.populate_asic_all(dvs, "288")

        # clear pg shared watermark, and verify that headroom watermark and persistent watermarks are not affected

        self.clear_watermark(dvs, ["USER", "PG_SHARED"])
        self.verify_value(dvs, self.pgs, WmTables.user, SaiWmStats.pg_shared, "0")

        # make sure the rest is untouched

        self.verify_value(dvs, self.pgs, WmTables.user, SaiWmStats.pg_headroom, "288")
        self.verify_value(dvs, self.pgs, WmTables.persistent, SaiWmStats.pg_shared, "288")
        self.verify_value(dvs, self.pgs, WmTables.persistent, SaiWmStats.pg_headroom, "288")

        # clear queue unicast persistent watermark, and verify that multicast watermark and user watermarks are not affected

        self.clear_watermark(dvs, ["PERSISTENT", "Q_SHARED_UNI"])

        # make sure it cleared
        self.verify_value(dvs, self.uc_q, WmTables.persistent, SaiWmStats.queue_shared, "0")

        # make sure the rest is untouched

        self.verify_value(dvs, self.mc_q, WmTables.persistent, SaiWmStats.queue_shared, "288")
        self.verify_value(dvs, self.uc_q, WmTables.user, SaiWmStats.queue_shared, "288")
        self.verify_value(dvs, self.mc_q, WmTables.user, SaiWmStats.queue_shared, "288")
        self.verify_value(dvs, self.all_q, WmTables.user, SaiWmStats.queue_shared, "288")
        self.verify_value(dvs, self.all_q, WmTables.persistent, SaiWmStats.queue_shared, "288")

        # clear queue all watermark, and verify that multicast and unicast watermarks are not affected

        # clear persistent all watermark
        self.clear_watermark(dvs, ["PERSISTENT", "Q_SHARED_ALL"])

        # make sure it cleared
        self.verify_value(dvs, self.all_q, WmTables.persistent, SaiWmStats.queue_shared, "0")

        # clear user all watermark
        self.clear_watermark(dvs, ["USER", "Q_SHARED_ALL"])

        # make sure it cleared
        self.verify_value(dvs, self.all_q, WmTables.user, SaiWmStats.queue_shared, "0")

        # make sure the rest is untouched
        self.verify_value(dvs, self.mc_q, WmTables.user, SaiWmStats.queue_shared, "288")
        self.verify_value(dvs, self.mc_q, WmTables.persistent, SaiWmStats.queue_shared, "288")
        self.verify_value(dvs, self.uc_q, WmTables.user, SaiWmStats.queue_shared, "288")
        self.verify_value(dvs, self.uc_q, WmTables.persistent, SaiWmStats.queue_shared, "0")

        self.enable_unittests(dvs, "false")


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
