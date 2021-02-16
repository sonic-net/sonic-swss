import redis
import time
import os
import pytest
import re
import json
from swsscommon import swsscommon

DEFAULT_DETECTION_TIME = '10'
DEFAULT_RESTORATION_TIME = '10'
DEFAULT_POLL_INTERVAL = 10
DEFAULT_ACTION = 'drop'

CONFIG_DB_PFC_WD_TABLE_NAME = 'PFC_WD' 

class TestPfcWD(object):

    def setup_dbs(self, dvs):

        self.asic_db = dvs.get_asic_db()
        self.counters_db = dvs.get_counters_db()
        self.config_db = dvs.get_config_db()
        self.flex_db = dvs.get_flex_db()

    def set_admin_status(self, interface, status):
        self.config_db.update_entry("PORT", interface, {"admin_status":status})

    #@pytest.fixture(scope="module", autouse=True)
    def enable_device_pfcwd(self, dvs):

        device_meta = self.config_db.get_entry('DEVICE_METADATA', 'localhost')
        device_meta['default_pfcwd_status'] = 'enable'
        self.config_db.update_entry('DEVICE_METADATA', 'localhost', device_meta)

    def enable_queue_counters(self, dvs):

        dvs.runcmd("counterpoll queue enable")

    def start_pfcwd_on_all_ports(self, dvs):

        dvs.runcmd("sudo pfcwd start_default")
        time.sleep(10)

    def enable_unittests(self, dvs, status):

        db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        ntf = swsscommon.NotificationProducer(db, "SAI_VS_UNITTEST_CHANNEL")
        fvp = swsscommon.FieldValuePairs()
        ntf.send("enable_unittests", status, fvp)

    def set_counter(self, dvs, obj_id, attr, val):

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

    def populate_asic(self, dvs, oid, attr, val):

        db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        self.set_counter(dvs, oid, attr, val)

    def populate_asic_all(self, dvs, obj_type, oid, val):

        if obj_type == "SAI_OBJECT_TYPE_PORT":
            self.populate_asic(dvs, oid, "SAI_PORT_STAT_PFC_3_RX_PKTS", val)
            self.populate_asic(dvs, oid, "SAI_PORT_STAT_PFC_4_RX_PKTS", val)
            self.populate_asic(dvs, oid, "SAI_PORT_STAT_PFC_3_ON2OFF_RX_PKTS", val)
            self.populate_asic(dvs, oid, "SAI_PORT_STAT_PFC_4_ON2OFF_RX_PKTS", val)
        else:
            self.populate_asic(dvs, oid, "SAI_QUEUE_STAT_PACKETS", val)
            self.populate_asic(dvs, oid, "SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES", val)
            self.populate_asic(dvs, oid, "SAI_QUEUE_ATTR_PAUSE_STATUS", "1")

        time.sleep(10)   

    def set_up_flex_counter(self, dvs, port_oid, queue_oid):
        fc_status_enable = {"FLEX_COUNTER_STATUS": "enable"}
        self.config_db.create_entry("FLEX_COUNTER_TABLE", "PFCWD", fc_status_enable)

        self.flex_db.create_entry("FLEX_COUNTER_TABLE",
                                  "PFC_WD:{}".format(port_oid),
                                  {"PORT_COUNTER_ID_LIST":"{},{},{},{}".format("SAI_PORT_STAT_PFC_3_RX_PKTS",
                                                                               "SAI_PORT_STAT_PFC_4_RX_PKTS",
                                                                               "SAI_PORT_STAT_PFC_3_ON2OFF_RX_PKTS",
                                                                               "SAI_PORT_STAT_PFC_4_ON2OFF_RX_PKTS")})
        self.flex_db.create_entry("FLEX_COUNTER_TABLE",
                                  "PFC_WD:{}".format(queue_oid),
                                  {"QUEUE_COUNTER_ID_LIST":"{},{}".format("SAI_QUEUE_STAT_PACKETS", 
                                                                          "SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES")})

        self.flex_db.create_entry("FLEX_COUNTER_TABLE",
                                   "PFC_WD:{}".format(queue_oid),
                                   {"QUEUE_ATTR_ID_LIST":"{}".format("SAI_QUEUE_ATTR_PAUSE_STATUS")})

        #self.populate_asic_all(dvs, "SAI_OBJECT_TYPE_PORT", port_oid, "0")
        #self.populate_asic_all(dvs, "SAI_OBJECT_TYPE_QUEUE", queue_oid, "0")

    
    def set_port_pfc(self, dvs, port_name, pfc_queues):

       config_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
       port_qos_tbl = swsscommon.Table(config_db, 'PORT_QOS_MAP')
       fvs = swsscommon.FieldValuePairs([('pfc_enable', ",".join(str(q) for q in pfc_queues))])
       port_qos_tbl.set(port_name, fvs)

       time.sleep(1)

    def get_bitmask_str(self, bits):

        mask = 0
        for b in bits:
            mask = mask | 1 << b
        return str(mask)

    def get_port_oid(self, dvs, port_name):

        cnt_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
        port_map_tbl = swsscommon.Table(cnt_db, 'COUNTERS_PORT_NAME_MAP')

        for k in port_map_tbl.get('')[1]:
            if k[0] == port_name:
                return k[1]

        return ''

    def get_port_attr(self, dvs, port_oid, port_attr):

        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        port_tbl = swsscommon.Table(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_PORT:{0}'.format(port_oid))

        for k in port_tbl.get('')[1]:
            if k[0] == port_attr:
                return k[1]

        return ''

    def get_queue_oid(self, dvs, queue):

       cnt_r = redis.Redis(unix_socket_path=dvs.redis_sock, db=swsscommon.COUNTERS_DB,
                           encoding="utf-8", decode_responses=True)
       return  cnt_r.hget("COUNTERS_QUEUE_NAME_MAP", queue)

    def set_pfc_storm_queue_oid(self, dvs, queue_oid):

       cnt_r = redis.Redis(unix_socket_path=dvs.redis_sock, db=swsscommon.COUNTERS_DB,
                           encoding="utf-8", decode_responses=True)
       cnt_r.hset("COUNTERS:"+queue_oid, "DEBUG_STORM","enabled")

    def verify_value(self, dvs, obj_id, table_name, counter_name, expected_value):

        counters_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
        table = swsscommon.Table(counters_db, table_name)

        ret = table.get(obj_id)
        status = ret[0]
        assert status
        keyvalues = ret[1]
        found = False
        for key, value in keyvalues:
            if key == counter_name:
                assert value == expected_value
                found = True
        assert found, "no such counter found"
  
    def verify_acl_table_creation(self, dvs):

        # check acl table in asic db
        acl_table_id = dvs_acl.get_acl_table_id(self, dvs)
        assert acl_table_id is not None

        # check acl table group in asic db
        dvs_acl.verify_acl_group_num(2)

        # get acl table group ids and verify the id numbers
        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE_GROUP")
        acl_group_ids = atbl.getKeys()
        assert len(acl_group_ids) == 2

        # check acl table group member
        dvs_acl.verify_acl_group_member(acl_group_ids, acl_table_id)

        # check port binding
        dvs_acl.verify_acl_port_binding(dvs, "Ethernet64")

    def verify_acl_rule_in_ports(self, dvs):

       # check acl rule table in asic db
       acl_table_id = self.get_acl_table_id(dvs)

       atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
       keys = atbl.getKeys()

       acl_entry = [k for k in keys if k not in dvs.asicdb.default_acl_entries]
       assert len(acl_entry) == 1

       (status, fvs) = atbl.get(acl_entry[0])
       assert status == True

       value = dict(fvs)
       assert value["SAI_ACL_ENTRY_ATTR_TABLE_ID"] == acl_table_id
       assert value["SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION"] == "SAI_PACKET_ACTION_DROP"
       in_ports = value["SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS"]
       assert dvs.asicdb.portnamemap["Ethernet64"] in in_ports

#    def clear_flex_counter(self, dvs):
#        for q in self.qs:
#            self.flex_db.delete_entry("FLEX_COUNTER_TABLE",
#                                     "QUEUE_WATERMARK_STAT_COUNTER:{}".format(q))
#
#        for pg in self.pgs:
#            self.flex_db.delete_entry("FLEX_COUNTER_TABLE",
#                                     "PG_WATERMARK_STAT_COUNTER:{}".format(pg))
#
#        for buffer in self.buffers:
#            self.flex_db.delete_entry("FLEX_COUNTER_TABLE",
#                                      "BUFFER_POOL_WATERMARK_STAT_COUNTER:{}".format(buffer))
#
#        self.config_db.delete_entry("FLEX_COUNTER_TABLE", "PG_WATERMARK")
#        self.config_db.delete_entry("FLEX_COUNTER_TABLE", "QUEUE_WATERMARK")
#        self.config_db.delete_entry("FLEX_COUNTER_TABLE", "BUFFER_POOL_WATERMARK")
#    

    def test_ingress_pfcwd_handler(self, dvs, testlog):

       try:
            self.setup_dbs(dvs)
            self.enable_device_pfcwd(dvs)
            #self.enable_queue_counters(dvs)
            self.set_admin_status("Ethernet64", "up")

            pfcwd_info = {
                    'detection_time': DEFAULT_DETECTION_TIME,
                    'restoration_time': DEFAULT_RESTORATION_TIME,
                    'action': DEFAULT_ACTION
            }

            self.config_db.update_entry(CONFIG_DB_PFC_WD_TABLE_NAME, "Ethernet64", pfcwd_info)
            self.config_db.update_entry(CONFIG_DB_PFC_WD_TABLE_NAME, "GLOBAL", {'POLL_INTERVAL': '10'})
            self.config_db.update_entry('FLEX_COUNTER_TABLE', "PFCWD", {'FLEX_COUNTER_STATUS': 'enable'})
            
            self.set_port_pfc(dvs, "Ethernet64", [3,4])

            # Get SAI object ID for the interface
            port_oid = self.get_port_oid(dvs, "Ethernet64")

            # Verify default PFC is set to configured value
            pfc = self.get_port_attr(dvs, port_oid, 'SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL')
            assert pfc == self.get_bitmask_str([3,4])

            # Find OID for the queue 3
            queue_oid = self.get_queue_oid(dvs, "Ethernet64:3")

            self.set_up_flex_counter(dvs, port_oid, queue_oid)
            self.enable_unittests(dvs, "true")
            time.sleep(5)   

#          self.verify_value(dvs, port_oid, "PFC_WD", "SAI_PORT_STAT_PFC_3_RX_PKTS", "0")
#          self.verify_value(dvs, port_oid, "PFC_WD", "SAI_PORT_STAT_PFC_4_RX_PKTS", "0")
#          self.verify_value(dvs, port_oid, "PFC_WD", "SAI_PORT_STAT_PFC_3_ON2OFF_RX_PKTS", "0")
#          self.verify_value(dvs, port_oid, "PFC_WD", "SAI_PORT_STAT_PFC_4_ON2OFF_RX_PKTS", "0")
#         
#          self.verify_value(dvs, queue_oid, "PFC_WD", "SAI_QUEUE_STAT_PACKETS", "0")
#          self.verify_value(dvs, queue_oid, "PFC_WD", "SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES", "0")
#          self.verify_value(dvs, queue_oid, "PFC_WD", "SAI_QUEUE_ATTR_PAUSE_STATUS", "0")
#          self.populate_asic_all(dvs, "SAI_OBJECT_TYPE_PORT", port_oid, "100")
#          self.populate_asic_all(dvs, "SAI_OBJECT_TYPE_QUEUE", queue_oid, "100")
            
            self.start_pfcwd_on_all_ports(dvs)
            # Enable PFC Storm on queue 3
            self.set_pfc_storm_queue_oid(dvs, queue_oid)
            
            # Verify ASIC_DB for table creation
            #verify_acl_table_creation(self, dvs)

            #Verify ASIC_DB for rule creation
            #verify_acl_rule_in_ports(dvs)

       finally:
            #self.clear_flex_counter(dvs)
            #self.enable_unittests(dvs, "false")
            print("Test case finished executing ........Done")

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
