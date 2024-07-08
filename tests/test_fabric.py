from swsscommon import swsscommon
from dvslib.dvs_database import DVSDatabase
import ast
import json

# Fabric counters
NUMBER_OF_RETRIES = 10

counter_group_meta = {
    'fabric_port_counter': {
        'key': 'FABRIC_PORT',
        'group_name': 'FABRIC_PORT_STAT_COUNTER',
        'name_map': 'COUNTERS_FABRIC_PORT_NAME_MAP',
        'post_test':  'post_port_counter_test',
    },
    'fabric_queue_counter': {
        'key': 'FABRIC_QUEUE',
        'group_name': 'FABRIC_QUEUE_STAT_COUNTER',
        'name_map': 'COUNTERS_FABRIC_QUEUE_NAME_MAP',
    },
}

class TestVirtualChassis(object):

    def wait_for_id_list(self, flex_db, stat, name, oid):
        for retry in range(NUMBER_OF_RETRIES):
            id_list = flex_db.db_connection.hgetall("FLEX_COUNTER_TABLE:" + stat + ":" + oid).items()
            if len(id_list) > 0:
                return
            else:
                time.sleep(1)

        assert False, "No ID list for counter " + str(name)

    def verify_flex_counters_populated(self, flex_db, counters_db, map, stat):
        counters_keys = counters_db.db_connection.hgetall(map)
        for counter_entry in counters_keys.items():
            name = counter_entry[0]
            oid = counter_entry[1]
            self.wait_for_id_list(flex_db, stat, name, oid)

    def test_voq_switch(self, vst):
        """Test VOQ switch objects configuration.

        This test validates configuration of switch creation objects required for
        VOQ switches. The switch_type, max_cores and switch_id attributes configuration
        are verified. For the System port config list, it is verified that all the
        configured system ports are avaiable in the asic db by checking the count.
        """

        if vst is None:
            return

        dvss = vst.dvss
        for name in dvss.keys():
            dvs = dvss[name]
            # Get the config info
            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")

            cfg_switch_type = metatbl.get("switch_type")
            if cfg_switch_type == "fabric":
               flex_db = dvs.get_flex_db()
               counters_db = dvs.get_counters_db()
               for ct in counter_group_meta.keys():
                  meta_data = counter_group_meta[ct]
                  counter_key = meta_data['key']
                  counter_stat = meta_data['group_name']
                  counter_map = meta_data['name_map']
                  self.verify_flex_counters_populated(flex_db, counters_db, counter_map, counter_stat)

                  port_counters_keys = counters_db.db_connection.hgetall(meta_data['name_map'])
                  port_counters_stat_keys = flex_db.get_keys("FLEX_COUNTER_TABLE:" + meta_data['group_name'])
                  for port_stat in port_counters_stat_keys:
                     assert port_stat in dict(port_counters_keys.items()).values(), "Non port created on PORT_STAT_COUNTER group: {}".format(port_stat)

               # update some config_db entries
               cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB,  dvs.redis_sock, 0)
               tb = swsscommon.Table(cfg_db, "FABRIC_PORT")
               fvs = swsscommon.FieldValuePairs([("isolateStatus","True")])
               tb.set("FABRIC_PORT|Fabric0", fvs )
               fvs = swsscommon.FieldValuePairs([("forceUnisolateStatus", "1")])
               tb.set("FABRIC_PORT|Fabric0", fvs )
            else:
               print( "We do not check switch type:", cfg_switch_type )

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass

