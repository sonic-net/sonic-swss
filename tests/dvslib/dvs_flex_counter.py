import time

NUMBER_OF_RETRIES = 10

class TestFlexCountersBase(object):

    def setup_dbs(self, dvs):
        self.config_db = dvs.get_config_db()
        self.flex_db = dvs.get_flex_db()
        self.counters_db = dvs.get_counters_db()
        self.app_db = dvs.get_app_db()

    def wait_for_table(self, table):
        for retry in range(NUMBER_OF_RETRIES):
            counters_keys = self.counters_db.db_connection.hgetall(table)
            if len(counters_keys) > 0:
                return
            else:
                time.sleep(1)

        assert False, str(table) + " not created in Counters DB"

    def wait_for_table_empty(self, table):
        for retry in range(NUMBER_OF_RETRIES):
            counters_keys = self.counters_db.db_connection.hgetall(table)
            if len(counters_keys) == 0:
                return
            else:
                time.sleep(1)

        assert False, str(table) + " is still in Counters DB"

    def wait_for_id_list(self, stat, name, oid):
        for retry in range(NUMBER_OF_RETRIES):
            id_list = self.flex_db.db_connection.hgetall("FLEX_COUNTER_TABLE:" + stat + ":" + oid).items()
            if len(id_list) > 0:
                return
            else:
                time.sleep(1)

        assert False, "No ID list for counter " + str(name)

    def wait_for_id_list_remove(self, stat, name, oid):
        for retry in range(NUMBER_OF_RETRIES):
            id_list = self.flex_db.db_connection.hgetall("FLEX_COUNTER_TABLE:" + stat + ":" + oid).items()
            if len(id_list) == 0:
                return
            else:
                time.sleep(1)

        assert False, "ID list for counter " + str(name) + " is still there"

    def wait_for_interval_set(self, group, interval):
        interval_value = None
        for retry in range(NUMBER_OF_RETRIES):
            interval_value = self.flex_db.db_connection.hget("FLEX_COUNTER_GROUP_TABLE:" + group, 'POLL_INTERVAL')
            if interval_value == interval:
                return
            else:
                time.sleep(1)

        assert False, "Polling interval is not applied to FLEX_COUNTER_GROUP_TABLE for group {}, expect={}, actual={}".format(group, interval, interval_value)

    def set_flex_counter_group_status(self, group, map, status='enable', check_name_map=True):
        group_stats_entry = {"FLEX_COUNTER_STATUS": status}
        self.config_db.create_entry("FLEX_COUNTER_TABLE", group, group_stats_entry)
        if check_name_map:
            if status == 'enable':
                self.wait_for_table(map)
            else:
                self.wait_for_table_empty(map)

    def verify_flex_counters_populated(self, map, stat):
        counters_keys = self.counters_db.db_connection.hgetall(map)
        for counter_entry in counters_keys.items():
            name = counter_entry[0]
            oid = counter_entry[1]
            self.wait_for_id_list(stat, name, oid)

    def set_flex_counter_group_interval(self, key, group, interval):
        group_stats_entry = {"POLL_INTERVAL": interval}
        self.config_db.create_entry("FLEX_COUNTER_TABLE", key, group_stats_entry)
        self.wait_for_interval_set(group, interval)

    def verify_no_flex_counters_tables(self, counter_stat):
        counters_stat_keys = self.flex_db.get_keys("FLEX_COUNTER_TABLE:" + counter_stat)
        assert len(counters_stat_keys) == 0, "FLEX_COUNTER_TABLE:" + str(counter_stat) + " tables exist before enabling the flex counter group"

    def verify_flex_counter_flow(self, dvs, meta_data):
        """
        The test will check there are no flex counters tables on FlexCounter DB when the counters are disabled.
        After enabling each counter group, the test will check the flow of creating flex counters tables on FlexCounter DB.
        For some counter types the MAPS on COUNTERS DB will be created as well after enabling the counter group, this will be also verified on this test.
        """
        self.setup_dbs(dvs)
        counter_key = meta_data['key']
        counter_stat = meta_data['group_name']
        counter_map = meta_data['name_map']
        pre_test = meta_data.get('pre_test')
        post_test = meta_data.get('post_test')
        meta_data['dvs'] = dvs

        self.verify_no_flex_counters_tables(counter_stat)

        if pre_test:
            if not hasattr(self, pre_test):
                assert False, "Test object does not have the method {}".format(pre_test)
            cb = getattr(self, pre_test)
            cb(meta_data)

        self.set_flex_counter_group_status(counter_key, counter_map)
        self.verify_flex_counters_populated(counter_map, counter_stat)
        self.set_flex_counter_group_interval(counter_key, counter_stat, '2500')

        if post_test:
            if not hasattr(self, post_test):
                assert False, "Test object does not have the method {}".format(post_test)
            cb = getattr(self, post_test)
            cb(meta_data)

