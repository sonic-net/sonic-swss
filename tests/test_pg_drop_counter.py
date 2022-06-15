import os
import re
import time
import json
import pytest
import redis

from swsscommon import swsscommon

pg_drop_attr = "SAI_INGRESS_PRIORITY_GROUP_STAT_DROPPED_PACKETS"

PORT = "Ethernet0"

@pytest.mark.usefixtures('dvs_port_manager')
class TestPGDropCounter(object):
    DEFAULT_POLL_INTERVAL = 10
    pgs = {}

    def setup_dbs(self, dvs):
        self.asic_db = dvs.get_asic_db()
        self.counters_db = dvs.get_counters_db()
        self.config_db = dvs.get_config_db()
        self.flex_db = dvs.get_flex_db()

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

    def populate_asic(self, dvs, val):
        for obj_id in self.pgs:
            self.set_counter(dvs, "SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP", obj_id, pg_drop_attr, val)

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
            assert found, "entry name %s not found" % (entry_name)

    def set_up_flex_counter(self):
        pg_stats_entry = {"PG_COUNTER_ID_LIST": "{}".format(pg_drop_attr)}
        for pg in self.pgs:
            self.flex_db.create_entry("FLEX_COUNTER_TABLE", "PG_DROP_STAT_COUNTER:{}".format(pg), pg_stats_entry)

        fc_status_enable = {"FLEX_COUNTER_STATUS": "enable"}

        self.config_db.create_entry("FLEX_COUNTER_TABLE", "PG_DROP", fc_status_enable)
        self.config_db.create_entry("FLEX_COUNTER_TABLE", "PG_WATERMARK", fc_status_enable)

    def clear_flex_counter(self):
        for pg in self.pgs:
            self.flex_db.delete_entry("FLEX_COUNTER_TABLE", "PG_DROP_STAT_COUNTER:{}".format(pg))

        self.config_db.delete_entry("FLEX_COUNTER_TABLE", "PG_DROP")
        self.config_db.delete_entry("FLEX_COUNTER_TABLE", "PG_WATERMARK")
        
    def test_pg_drop_counters(self, dvs):
        self.setup_dbs(dvs)
        self.pgs = self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP")
        try:
            self.set_up_flex_counter()

            self.populate_asic(dvs, "0")
            time.sleep(self.DEFAULT_POLL_INTERVAL)
            self.verify_value(dvs, self.pgs, pg_drop_attr, "0")

            self.populate_asic(dvs, "100")
            time.sleep(self.DEFAULT_POLL_INTERVAL)
            self.verify_value(dvs, self.pgs, pg_drop_attr, "100")

            self.populate_asic(dvs, "123")
            time.sleep(self.DEFAULT_POLL_INTERVAL)
            self.verify_value(dvs, self.pgs, pg_drop_attr, "123")
        finally:
            self.clear_flex_counter()

    def test_pg_drop_counter_port_add_remove(self, dvs):
        self.setup_dbs(dvs)

        try:
            # configure pg drop flex counter
            self.set_up_flex_counter()

            # receive port info
            fvs = self.config_db.get_entry("PORT", PORT)
            assert len(fvs) > 0
        
            # save all the oids of the pg drop counters            
            oid_list = []
            for priority in range(0,7):
                oid_list.append(dvs.get_counters_db().get_entry("COUNTERS_PG_NAME_MAP", "")["%s:%d"%(PORT, priority)])
                # verify that counters exists on flex counter
                fields = self.flex_db.get_entry("FLEX_COUNTER_TABLE", "PG_WATERMARK_STAT_COUNTER:%s"%oid_list[-1])
                assert len(fields) == 1

            # remove port
            port_oid = self.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")[PORT]
            self.dvs_port.remove_port(PORT)
            dvs.get_asic_db().wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid)
        
            # verify counters were removed from flex counter table
            for oid in oid_list:
                fields = self.flex_db.get_entry("FLEX_COUNTER_TABLE", "PG_WATERMARK_STAT_COUNTER:%s"%oid)
                assert len(fields) == 0
              
            # add port and wait until the port is added on asic db
            num_of_keys_without_port = len(dvs.get_asic_db().get_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT"))
            self.config_db.create_entry("PORT", PORT, fvs)
            dvs.get_asic_db().wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT", num_of_keys_without_port + 1)
            dvs.get_counters_db().wait_for_fields("COUNTERS_PG_NAME_MAP", "", ["%s:0"%(PORT)])
            
            # verify counter was added
            for priority in range(0,7):
                oid = dvs.get_counters_db().get_entry("COUNTERS_PG_NAME_MAP", "")["%s:%d"%(PORT, priority)]
      
                # verify that counters exists on flex counter
                fields = self.flex_db.get_entry("FLEX_COUNTER_TABLE", "PG_WATERMARK_STAT_COUNTER:%s"%oid)
                assert len(fields) == 1
            
        finally:
            self.clear_flex_counter()
