import pytest
import time
from swsscommon import swsscommon

class TestBufferOrch(object):
    def make_dict(self, input_list):
        return dict(input_list[1])

    def setup_db(self, dvs):
        self.asic_db = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.config_db = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        self.counter_db = swsscommon.DBConnector(2, dvs.redis_sock, 0)

        self.flex_counter_table = swsscommon.Table(self.config_db, "FLEX_COUNTER_TABLE")
        self.asic_pg_table = swsscommon.Table(self.asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP")
        self.asic_queue_table = swsscommon.Table(self.asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_QUEUE")
        self.buffer_pg_table = swsscommon.Table(self.config_db, "BUFFER_PG")
        self.buffer_queue_table = swsscommon.Table(self.config_db, "BUFFER_QUEUE")

        # Enable PG watermark polling in flex counter table
        # After that the COUNTERS_PG_NAME_MAP and COUNTERS_QUEUE_NAME_MAP will be inserted to COUNTER_DB
        fvs = swsscommon.FieldValuePairs([('FLEX_COUNTER_STATUS', 'enable')])
        self.flex_counter_table.set('PG_WATERMARK', fvs)
        time.sleep(1)

        self.pg_name_map_table = swsscommon.Table(self.counter_db, "COUNTERS_PG_NAME_MAP")
        self.pg_name_map_dict = self.make_dict(self.pg_name_map_table.get(''))
        self.queue_name_map_table = swsscommon.Table(self.counter_db, "COUNTERS_QUEUE_NAME_MAP")
        self.queue_name_map_dict = self.make_dict(self.queue_name_map_table.get(''))

    def test_buffer_pg(self, dvs):
        self.setup_db(dvs)

        pg_key_config = 'Ethernet0|0'
        pg_key_map = 'Ethernet0:0'
        pg_oid = self.pg_name_map_dict[pg_key_map]

        original_profile_reference = self.make_dict(self.buffer_pg_table.get(pg_key_config))['profile']

        # Make sure the PG is in ASIC table
        pg_asic_original = self.make_dict(self.asic_pg_table.get(pg_oid))
        assert pg_asic_original and pg_asic_original['SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE'] != 'oid:0x0'
        original_profile_oid = pg_asic_original['SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE']

        # Remove the PG from BUFFER_PG_TABLE
        self.buffer_pg_table._del(pg_key_config)
        time.sleep(1)

        # Make sure the PG has been removed
        pg_asic = self.make_dict(self.asic_pg_table.get(pg_oid))
        assert pg_asic and pg_asic['SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE'] == 'oid:0x0'

        # Readd the PG to BUFFER_PG_TABLE
        fvs = swsscommon.FieldValuePairs([('profile', original_profile_reference)])
        self.buffer_pg_table.set(pg_key_config, fvs)
        time.sleep(1)
        pg_asic = self.make_dict(self.asic_pg_table.get(pg_oid))
        assert pg_asic and pg_asic['SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE'] == original_profile_oid

    def test_buffer_queue(self, dvs):
        self.setup_db(dvs)

        queue_key_config = 'Ethernet0|0-2'
        queue_keys_map = ['Ethernet0:0', 'Ethernet0:1', 'Ethernet0:2']
        queue_oids = [self.queue_name_map_dict[key] for key in queue_keys_map]

        original_profile_reference = self.make_dict(self.buffer_queue_table.get(queue_key_config))['profile']

        # Make sure the queue is in ASIC table
        original_profile_oid = None
        for oid in queue_oids:
            # assert queue_asic is correct
            queue_asic = self.make_dict(self.asic_queue_table.get(oid))
            assert queue_asic and queue_asic['SAI_QUEUE_ATTR_BUFFER_PROFILE_ID'] != 'oid:0x0'
            if original_profile_oid:
                assert queue_asic['SAI_QUEUE_ATTR_BUFFER_PROFILE_ID'] == original_profile_oid
            else:
                original_profile_oid = queue_asic['SAI_QUEUE_ATTR_BUFFER_PROFILE_ID']

        # Remove the queue from BUFFER_QUEUE_TABLE
        self.buffer_queue_table._del(queue_key_config)
        time.sleep(1)

        # Make sure the queue has been removed
        for oid in queue_oids:
            queue_asic = self.make_dict(self.asic_queue_table.get(oid))
            assert queue_asic and queue_asic['SAI_QUEUE_ATTR_BUFFER_PROFILE_ID'] == 'oid:0x0'

        # Readd the queue to BUFFER_QUEUE_TABLE
        fvs = swsscommon.FieldValuePairs([('profile', original_profile_reference)])
        self.buffer_queue_table.set(queue_key_config, fvs)
        time.sleep(1)
        for oid in queue_oids:
            queue_asic = self.make_dict(self.asic_queue_table.get(oid))
            assert queue_asic and queue_asic['SAI_QUEUE_ATTR_BUFFER_PROFILE_ID'] == original_profile_oid
