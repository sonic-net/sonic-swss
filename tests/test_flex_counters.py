import time

# Counter keys on ConfigDB
PORT_KEY                  =   "PORT"
QUEUE_KEY                 =   "QUEUE"
RIF_KEY                   =   "RIF"
BUFFER_POOL_WATERMARK_KEY =   "BUFFER_POOL_WATERMARK"
PORT_BUFFER_DROP_KEY      =   "PORT_BUFFER_DROP"
PG_WATERMARK_KEY          =   "PG_WATERMARK"

# Counter stats on FlexCountersDB
PORT_STAT                  =   "PORT_STAT_COUNTER"
QUEUE_STAT                 =   "QUEUE_STAT_COUNTER"
RIF_STAT                   =   "RIF_STAT_COUNTER"
BUFFER_POOL_WATERMARK_STAT =   "BUFFER_POOL_WATERMARK_STAT_COUNTER"
PORT_BUFFER_DROP_STAT      =   "PORT_BUFFER_DROP_STAT"
PG_WATERMARK_STAT          =   "PG_WATERMARK_STAT_COUNTER"

# Counter maps on CountersDB
PORT_MAP                  =   "COUNTERS_PORT_NAME_MAP"
QUEUE_MAP                 =   "COUNTERS_QUEUE_NAME_MAP"
RIF_MAP                   =   "COUNTERS_RIF_NAME_MAP"
BUFFER_POOL_WATERMARK_MAP =   "COUNTERS_BUFFER_POOL_NAME_MAP"
PORT_BUFFER_DROP_MAP      =   "COUNTERS_PORT_NAME_MAP"
PG_WATERMARK_MAP          =   "COUNTERS_PG_NAME_MAP"

class TestFlexCounters(object):

    def setup_dbs(self, dvs):
        self.config_db = dvs.get_config_db()
        self.flex_db = dvs.get_flex_db()
        self.counters_db = dvs.get_counters_db()

    def verify_flex_counters_populated(self, map, stat):
        counters_keys = self.counters_db.db_connection.hgetall(map)
        assert len(counters_keys) > 0, str(map) + " not created in Counters DB"

        for counter_entry in counters_keys.items():
            id_list = self.flex_db.db_connection.hgetall("FLEX_COUNTER_TABLE:" + stat + ":" + counter_entry[1]).items()
            assert len(id_list) > 0, "No ID list for counter " + str(counter_entry[0])

    def enable_flex_counter_group(self, group):
        group_stats_entry = {"FLEX_COUNTER_STATUS": "enable"}
        self.config_db.create_entry("FLEX_COUNTER_TABLE", group, group_stats_entry)
        time.sleep(2)

    def test_port_counters(self, dvs):
        self.setup_dbs(dvs)
        try:
            self.enable_flex_counter_group(PORT_KEY)
            self.verify_flex_counters_populated(PORT_MAP, PORT_STAT)
        except Exception as e:
            assert False, "Failed to write/read from DB, exception: {}".format(str(e))

    def test_queue_counters(self, dvs):
        self.setup_dbs(dvs)
        try:
            self.enable_flex_counter_group(QUEUE_KEY)
            self.verify_flex_counters_populated(QUEUE_MAP, QUEUE_STAT)
        except Exception as e:
            assert False, "Failed to write/read from DB, exception: {}".format(str(e))

    def test_rif_counters(self, dvs):
        self.setup_dbs(dvs)
        try:
            self.config_db.db_connection.hset('INTERFACE|Ethernet0', "NULL", "NULL")
            self.config_db.db_connection.hset('INTERFACE|Ethernet0|192.168.0.1/24', "NULL", "NULL")

            self.enable_flex_counter_group(RIF_KEY)
            self.verify_flex_counters_populated(RIF_MAP, RIF_STAT)

            self.config_db.db_connection.hdel('INTERFACE|Ethernet0|192.168.0.1/24', "NULL")
        except Exception as e:
            assert False, "Failed to write/read from DB, exception: {}".format(str(e))

    def test_buffer_pool_watermark_counters(self, dvs):
        self.setup_dbs(dvs)
        try:
            self.enable_flex_counter_group(BUFFER_POOL_WATERMARK_KEY)
            self.verify_flex_counters_populated(BUFFER_POOL_WATERMARK_MAP, BUFFER_POOL_WATERMARK_STAT)
        except Exception as e:
            assert False, "Failed to write/read from DB, exception: {}".format(str(e))

    def test_port_buffer_drop_counters(self, dvs):
        self.setup_dbs(dvs)
        try:
            self.enable_flex_counter_group(PORT_BUFFER_DROP_KEY)
            self.verify_flex_counters_populated(PORT_BUFFER_DROP_MAP, PORT_BUFFER_DROP_STAT)
        except Exception as e:
            assert False, "Failed to write/read from DB, exception: {}".format(str(e))

    def test_pg_watermark_counters(self, dvs):
        self.setup_dbs(dvs)
        try:
            self.enable_flex_counter_group(PG_WATERMARK_KEY)
            self.verify_flex_counters_populated(PG_WATERMARK_MAP, PG_WATERMARK_STAT)
        except Exception as e:
            assert False, "Failed to write/read from DB, exception: {}".format(str(e))
