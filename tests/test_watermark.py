from swsscommon import swsscommon
import os
import re
import time
import json
import redis


class SaiWmStats:
    queue_shared = "SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES"
    pg_shared = "SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_BYTES"
    pg_headroom = "SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_BYTES"


class WmTables:
    persistent = "PERSISTENT_WATERMARKS"
    periodic = "PERIODIC_WATERMARKS"
    user = "USER_WATERMARKS"


class TestWatermark(object):

    DEFAULT_TELEMETRY_INTERVAL = 120
    NEW_INTERVAL = 5

    def populate_table(self, dvs, table_name, value):

        counters_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
        table = swsscommon.Table(counters_db, table_name)
        
        for q in self.qs:
            table.set(q, [(SaiWmStats.queue_shared, value)])

        for pg in self.pgs:
            table.set(pg, [(SaiWmStats.pg_shared, value),
                           (SaiWmStats.pg_headroom, value)])

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

    def getOids(self, dvs, obj_type):

        db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        tbl = swsscommon.Table(db, "ASIC_STATE:{0}".format(obj_type))
        keys = tbl.getKeys()
        return keys

    def run_plugins(self, dvs):
        
        dvs.runcmd("redis-cli --eval /usr/share/swss/watermark_queue.lua '{}' , 2".format("' '".join(self.qs)))
        dvs.runcmd("redis-cli --eval /usr/share/swss/watermark_pg.lua '{}' , 2".format("' '".join(self.pgs)))

    def set_up_flex_counter(self, dvs):
        for q in self.qs:
            dvs.runcmd("redis-cli -n 5 hset 'FLEX_COUNTER_TABLE:QUEUE_WATERMARK_STAT_COUNTER:{}' ".format(q) + \
                      "QUEUE_COUNTER_ID_LIST SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES")

        for pg in self.pgs:
            dvs.runcmd("redis-cli -n 5 hset 'FLEX_COUNTER_TABLE:PG_WATERMARK_STAT_COUNTER:{}' ".format(pg) + \
                      "PG_COUNTER_ID_LIST 'SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_BYTES,SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_BYTES'")

        dvs.runcmd("redis-cli -n 4 hset 'FLEX_COUNTER_TABLE|PG_WATERMARK' 'FLEX_COUNTER_STATUS' 'enable'")
        dvs.runcmd("redis-cli -n 4 hset 'FLEX_COUNTER_TABLE|QUEUE_WATERMARK' 'FLEX_COUNTER_STATUS' 'enable'")

        time.sleep(self.DEFAULT_TELEMETRY_INTERVAL*2)

    def set_up(self, dvs):
        
        self.qs = self.getOids(dvs, "SAI_OBJECT_TYPE_QUEUE")
        self.pgs = self.getOids(dvs, "SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP")  

        db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
        tbl = swsscommon.Table(db, "COUNTERS_QUEUE_TYPE_MAP")                

        self.uc_q = []
        self.mc_q = []

        for q in self.qs:
             if self.qs.index(q) % 16 < 8:
                 tbl.set('', [(q, "SAI_QUEUE_TYPE_UNICAST")])
                 self.uc_q.append(q)
             else:
                 tbl.set('', [(q, "SAI_QUEUE_TYPE_MULTICAST")])
                 self.mc_q.append(q)

    def test_telemetry_period(self, dvs):
        
        self.set_up(dvs)
        self.set_up_flex_counter(dvs)

        self.populate_table(dvs, WmTables.periodic, "321")

        time.sleep(self.DEFAULT_TELEMETRY_INTERVAL + 1)

        self.verify_value(dvs, self.pgs, WmTables.periodic, SaiWmStats.pg_shared, "0")
        self.verify_value(dvs, self.pgs, WmTables.periodic, SaiWmStats.pg_headroom, "0")
        self.verify_value(dvs, self.qs, WmTables.periodic, SaiWmStats.queue_shared, "0")

        self.populate_table(dvs, WmTables.periodic, "123")

        dvs.runcmd("config watermark telemetry interval {}".format(5))

        time.sleep(self.DEFAULT_TELEMETRY_INTERVAL + 1)
        time.sleep(self.NEW_INTERVAL + 1)

        self.verify_value(dvs, self.pgs, WmTables.periodic, SaiWmStats.pg_shared, "0")
        self.verify_value(dvs, self.pgs, WmTables.periodic, SaiWmStats.pg_headroom, "0")
        self.verify_value(dvs, self.qs, WmTables.periodic, SaiWmStats.queue_shared, "0")

    def test_lua_plugins(self, dvs):
        
        self.set_up(dvs)

        self.populate_table(dvs, "COUNTERS", "192")
        self.run_plugins(dvs)
        time.sleep(1)

        for table_name in [WmTables.user, WmTables.persistent]:
            self.verify_value(dvs, self.qs, table_name, SaiWmStats.queue_shared, "192")
            self.verify_value(dvs, self.pgs, table_name, SaiWmStats.pg_headroom, "192")
            self.verify_value(dvs, self.pgs, table_name, SaiWmStats.pg_shared, "192")
        
        self.populate_table(dvs, "COUNTERS", "96")
        self.run_plugins(dvs)
        time.sleep(1)

        for table_name in [WmTables.user, WmTables.persistent]:
            self.verify_value(dvs, self.qs, table_name, SaiWmStats.queue_shared, "192")
            self.verify_value(dvs, self.pgs, table_name, SaiWmStats.pg_headroom, "192")
            self.verify_value(dvs, self.pgs, table_name, SaiWmStats.pg_shared, "192")

        self.populate_table(dvs, "COUNTERS", "288")
        self.run_plugins(dvs)
        time.sleep(1)
        
        for table_name in [WmTables.user, WmTables.persistent]:
            self.verify_value(dvs, self.qs, table_name, SaiWmStats.queue_shared, "288")
            self.verify_value(dvs, self.pgs, table_name, SaiWmStats.pg_headroom, "288")
            self.verify_value(dvs, self.pgs, table_name, SaiWmStats.pg_shared, "288")

    def test_clear(self, dvs):

        self.set_up(dvs) 

        self.populate_table(dvs, "COUNTERS", "288")
        self.run_plugins(dvs)

        # clear pg shared watermark, and verify that headroom watermark and persistent watermarks are not affected

        dvs.runcmd("sonic-clear priority-group watermark shared")

        # make sure it cleared
        self.verify_value(dvs, self.pgs, WmTables.user, SaiWmStats.pg_shared, "0")

        # make sure the rest is untouched

        self.verify_value(dvs, self.pgs, WmTables.user, SaiWmStats.pg_headroom, "288") 
        self.verify_value(dvs, self.pgs, WmTables.persistent, SaiWmStats.pg_shared, "288") 
        self.verify_value(dvs, self.pgs, WmTables.persistent, SaiWmStats.pg_headroom, "288") 

        # clear queue unicast persistent watermark, and verify that multicast watermark and user watermarks are not affected

        dvs.runcmd("sonic-clear queue persistent_watermark unicast")

        # make sure it cleared
        self.verify_value(dvs, self.uc_q, WmTables.persistent, SaiWmStats.queue_shared, "0")

        # make sure the rest is untouched

        self.verify_value(dvs, self.mc_q, WmTables.persistent, SaiWmStats.queue_shared, "288") 
        self.verify_value(dvs, self.uc_q, WmTables.user, SaiWmStats.queue_shared, "288") 
        self.verify_value(dvs, self.mc_q, WmTables.user, SaiWmStats.queue_shared, "288") 

