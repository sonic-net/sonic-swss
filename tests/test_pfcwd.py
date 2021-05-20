import redis
import time
import os
import pytest
import re
import json
from swsscommon import swsscommon
from dvslib.dvs_common import wait_for_result

DVS_FAKE_PLATFORM = "broadcom"

PFCWD_TABLE_NAME = "DROP_TEST_TABLE"
PFCWD_TABLE_TYPE = "DROP"
QUEUE_3 = "3"
QUEUE_4 = "4"
PFCWD_TC = [QUEUE_3, QUEUE_4]
PFCWD_RULE_NAME_1 =  "DROP_TEST_RULE_1"
PFCWD_RULE_NAME_2 =  "DROP_TEST_RULE_2"

CFG_PORT_QOS_MAP_TABLE_NAME = "PORT_QOS_MAP"
CFG_PFC_WD_TABLE_NAME = "PFC_WD"
CFG_PFC_WD_TABLE_GLOBAL_KEY = "GLOBAL"
CFG_FLEX_COUNTER_TABLE_NAME = "FLEX_COUNTER_TABLE"
CFG_FLEX_COUNTER_TABLE_PFCWD_KEY = "PFCWD"

ASIC_PORT_TABLE_NAME = "ASIC_STATE:SAI_OBJECT_TYPE_PORT"

FC_FLEX_COUNTER_GROUP_TABLE_NAME = "FLEX_COUNTER_GROUP_TABLE"
FC_FLEX_COUNTER_GROUP_TABLE_PFC_WD_KEY = "PFC_WD"
FC_FLEX_COUNTER_TABLE_NAME = "FLEX_COUNTER_TABLE"
FC_FLEX_COUNTER_TABLE_PFC_WD_KEY_PREFIX = "PFC_WD"

CNTR_COUNTERS_QUEUE_NAME_MAP = "COUNTERS_QUEUE_NAME_MAP"
CNTR_COUNTERS_TABLE_NAME = "COUNTERS"

APPL_PFC_WD_INSTORM_TABLE_NAME = "PFC_WD_TABLE_INSTORM"

PFC_ENABLE = "pfc_enable"

ACTION = "action"
DROP = "drop"
DETECTION_TIME = "detection_time"
RESTORATION_TIME = "restoration_time"
BIG_RED_SWITCH = "BIG_RED_SWITCH"
DISABLE = "disable"

FLEX_COUNTER_STATUS = "FLEX_COUNTER_STATUS"
ENABLE = "enable"

DEBUG_STORM = "DEBUG_STORM"
ENABLED = "enabled"
PFC_WD_STATUS = "PFC_WD_STATUS"
STORMED = "stormed"
OPERATIONAL = "operational"
BIG_RED_SWITCH_MODE = "BIG_RED_SWITCH_MODE"
PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED = "PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED"
PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED = "PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED"

STORM = "storm"

PORT_UNDER_TEST = "Ethernet64"


class TestPfcWd:
    def test_PfcWdAclCreationDeletion(self, dvs, dvs_acl, testlog):
        try:
            dvs_acl.create_acl_table(PFCWD_TABLE_NAME, PFCWD_TABLE_TYPE, ["Ethernet0","Ethernet8", "Ethernet16", "Ethernet24"], stage="ingress")

            config_qualifiers = {
                "TC" : PFCWD_TC[0],
                "IN_PORTS": "Ethernet0"
            }

            expected_sai_qualifiers = {
                "SAI_ACL_ENTRY_ATTR_FIELD_TC" : dvs_acl.get_simple_qualifier_comparator("3&mask:0xff"),
                "SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS": dvs_acl.get_port_list_comparator(["Ethernet0"])
            }

            dvs_acl.create_acl_rule(PFCWD_TABLE_NAME, PFCWD_RULE_NAME_1, config_qualifiers, action="DROP")
            time.sleep(5)
            dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP")

            config_qualifiers = {
                "TC" : PFCWD_TC[0],
                "IN_PORTS": "Ethernet0,Ethernet16"
            }

            expected_sai_qualifiers = {
                "SAI_ACL_ENTRY_ATTR_FIELD_TC" : dvs_acl.get_simple_qualifier_comparator("3&mask:0xff"),
                "SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS": dvs_acl.get_port_list_comparator(["Ethernet0","Ethernet16"])
            }

            dvs_acl.update_acl_rule(PFCWD_TABLE_NAME, PFCWD_RULE_NAME_1, config_qualifiers, action="DROP")
            time.sleep(5)
            dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP")
            dvs_acl.remove_acl_rule(PFCWD_TABLE_NAME, PFCWD_RULE_NAME_1)

            config_qualifiers = {
                "TC" : PFCWD_TC[1],
                "IN_PORTS": "Ethernet8"
            }

            expected_sai_qualifiers = {
                "SAI_ACL_ENTRY_ATTR_FIELD_TC" : dvs_acl.get_simple_qualifier_comparator("4&mask:0xff"),
                "SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS": dvs_acl.get_port_list_comparator(["Ethernet8"]),
            }

            dvs_acl.create_acl_rule(PFCWD_TABLE_NAME, PFCWD_RULE_NAME_2, config_qualifiers, action="DROP")
            time.sleep(5)
            dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP")

            config_qualifiers = {
                "TC" : PFCWD_TC[1],
                "IN_PORTS": "Ethernet8,Ethernet24"
            }

            expected_sai_qualifiers = {
                "SAI_ACL_ENTRY_ATTR_FIELD_TC" : dvs_acl.get_simple_qualifier_comparator("4&mask:0xff"),
                "SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS": dvs_acl.get_port_list_comparator(["Ethernet8","Ethernet24"]),
            }

            dvs_acl.update_acl_rule(PFCWD_TABLE_NAME, PFCWD_RULE_NAME_2, config_qualifiers, action="DROP")
            time.sleep(5)
            dvs_acl.verify_acl_rule(expected_sai_qualifiers, action="DROP")
            dvs_acl.remove_acl_rule(PFCWD_TABLE_NAME, PFCWD_RULE_NAME_2)

        finally:
            dvs_acl.remove_acl_table(PFCWD_TABLE_NAME)

    def connect_dbs(self, dvs):
        self.appl_db = dvs.get_app_db()
        self.asic_db = dvs.get_asic_db()
        self.config_db = dvs.get_config_db()
        self.flex_cntr_db = dvs.get_flex_db()
        self.cntrs_db = dvs.get_counters_db()

        self.cnt_r = redis.Redis(unix_socket_path=dvs.redis_sock, db=swsscommon.COUNTERS_DB,
                                 encoding="utf-8", decode_responses=True)

    def set_port_pfc(self, port_name, pfc_tcs):
        fvs = {
            PFC_ENABLE: ",".join(pfc_tcs),
        }
        self.config_db.create_entry(CFG_PORT_QOS_MAP_TABLE_NAME, port_name, fvs)

    def enable_flex_counter(self, key):
        fvs = {
            FLEX_COUNTER_STATUS: ENABLE,
        }
        self.config_db.create_entry(CFG_FLEX_COUNTER_TABLE_NAME, key, fvs)

    def start_port_pfcwd(self, port_name):
        fvs = {
            ACTION: DROP,
            DETECTION_TIME: "400",
            RESTORATION_TIME: "400",
        }
        self.config_db.create_entry(CFG_PFC_WD_TABLE_NAME, port_name, fvs)

    def start_queue_pfc_storm(self, queue_oid):
        fvs = {
            DEBUG_STORM: ENABLED,
        }
        self.cntrs_db.create_entry(CNTR_COUNTERS_TABLE_NAME, queue_oid, fvs)

    def enable_big_red_switch(self):
        fvs = {
            BIG_RED_SWITCH: ENABLE,
        }
        self.config_db.create_entry(CFG_PFC_WD_TABLE_NAME, CFG_PFC_WD_TABLE_GLOBAL_KEY, fvs)

    def get_queue_oid(self, dvs, port_name, qidx):
        def _access_function():
            queue_oid = self.cnt_r.hget(CNTR_COUNTERS_QUEUE_NAME_MAP, "{}:{}".format(port_name, qidx))
            return (True if queue_oid else False, queue_oid)

        (queue_oid_found, queue_oid) = wait_for_result(_access_function)

        return queue_oid

    def check_db_key_existence(self, db, table_name, key):
        db.wait_for_matching_keys(table_name, [key])

    def check_db_fvs(self, db, table_name, key, fv_dict):
        db.wait_for_field_match(table_name, key, fv_dict)

    def check_db_key_removal(self, db, table_name, key):
        db.wait_for_deleted_keys(table_name, [key])

    def check_db_fields_removal(self, db, table_name, key, fields):
        def _access_function():
            fvs = db.get_entry(table_name, key)
            status = all(f not in fvs for f in fields)
            return (status, None)

        wait_for_result(_access_function)

    def stop_port_pfcwd(self, port_name):
        self.config_db.delete_entry(CFG_PFC_WD_TABLE_NAME, port_name)

    def stop_queue_pfc_storm(self, queue_oid):
        self.cnt_r.hdel("{}:{}".format(CNTR_COUNTERS_TABLE_NAME, queue_oid), DEBUG_STORM)

    def disable_big_red_switch(self):
        fvs = {
            BIG_RED_SWITCH: DISABLE,
        }
        self.config_db.create_entry(CFG_PFC_WD_TABLE_NAME, CFG_PFC_WD_TABLE_GLOBAL_KEY, fvs)

    def clear_queue_cntrs(self, queue_oid):
        fvs = {
            PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED: "0",
            PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED: "0",
        }
        self.cntrs_db.create_entry(CNTR_COUNTERS_TABLE_NAME, queue_oid, fvs)

    def test_pfc_en_bits_user_wd_cfg_sep(self, dvs, testlog):
        self.connect_dbs(dvs)

        # Enable pfc wd flex counter polling
        self.enable_flex_counter(CFG_FLEX_COUNTER_TABLE_PFCWD_KEY)
        # Verify pfc wd flex counter status published to FLEX_COUNTER_DB FLEX_COUNTER_GROUP_TABLE by flex counter orch
        fv_dict = {
            FLEX_COUNTER_STATUS: ENABLE,
        }
        self.check_db_fvs(self.flex_cntr_db, FC_FLEX_COUNTER_GROUP_TABLE_NAME, FC_FLEX_COUNTER_GROUP_TABLE_PFC_WD_KEY, fv_dict)

        # Enable pfc on tc 3
        pfc_tcs = [QUEUE_3]
        self.set_port_pfc(PORT_UNDER_TEST, pfc_tcs)

        # Verify pfc enable bits in ASIC_DB
        port_oid = dvs.asicdb.portnamemap[PORT_UNDER_TEST]
        fv_dict = {
            "SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL": "8",
        }
        self.check_db_fvs(self.asic_db, ASIC_PORT_TABLE_NAME, port_oid, fv_dict)

        # Start pfc wd (config) on port
        self.start_port_pfcwd(PORT_UNDER_TEST)
        # Verify port level counter to poll published to FLEX_COUNTER_DB FLEX_COUNTER_TABLE by pfc wd orch
        self.check_db_key_existence(self.flex_cntr_db, FC_FLEX_COUNTER_TABLE_NAME,
                                    "{}:{}".format(FC_FLEX_COUNTER_TABLE_PFC_WD_KEY_PREFIX, port_oid))
        # Verify queue level counter to poll published to FLEX_COUNTER_DB FLEX_COUNTER_TABLE by pfc wd orch
        q3_oid = self.get_queue_oid(dvs, PORT_UNDER_TEST, QUEUE_3)
        self.check_db_key_existence(self.flex_cntr_db, FC_FLEX_COUNTER_TABLE_NAME,
                                    "{}:{}".format(FC_FLEX_COUNTER_TABLE_PFC_WD_KEY_PREFIX, q3_oid))

        # Verify pfc enable bits stay unchanged in ASIC_DB
        time.sleep(2)
        fv_dict = {
            "SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL": "8",
        }
        self.check_db_fvs(self.asic_db, ASIC_PORT_TABLE_NAME, port_oid, fv_dict)

        # Start pfc storm on queue 3
        self.start_queue_pfc_storm(q3_oid)
        # Verify queue in storm from COUNTERS_DB
        fv_dict = {
            PFC_WD_STATUS: STORMED,
        }
        self.check_db_fvs(self.cntrs_db, CNTR_COUNTERS_TABLE_NAME, q3_oid, fv_dict)
        # Verify queue in storm from APPL_DB
        fv_dict = {
            QUEUE_3: STORM,
        }
        self.check_db_fvs(self.appl_db, APPL_PFC_WD_INSTORM_TABLE_NAME, PORT_UNDER_TEST, fv_dict)

        # Verify pfc enable bits change in ASIC_DB
        fv_dict = {
            "SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL": "0",
        }
        self.check_db_fvs(self.asic_db, ASIC_PORT_TABLE_NAME, port_oid, fv_dict)

        # Re-set pfc enable on tc 3
        pfc_tcs = [QUEUE_3]
        self.set_port_pfc(PORT_UNDER_TEST, pfc_tcs)

        # Verify pfc enable bits stay unchanged in ASIC_DB
        time.sleep(2)
        fv_dict = {
            "SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL": "0",
        }
        self.check_db_fvs(self.asic_db, ASIC_PORT_TABLE_NAME, port_oid, fv_dict)

        # Change pfc enable bits: disable pfc on tc 3, and enable pfc on tc 4
        pfc_tcs = [QUEUE_4]
        self.set_port_pfc(PORT_UNDER_TEST, pfc_tcs)

        # Verify pfc enable bits change in ASIC_DB
        fv_dict = {
            "SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL": "16",
        }
        self.check_db_fvs(self.asic_db, ASIC_PORT_TABLE_NAME, port_oid, fv_dict)

        # Stop pfc wd on port (i.e., remove pfc wd config from port)
        self.stop_port_pfcwd(PORT_UNDER_TEST)
        # Verify port level counter removed from FLEX_COUNTER_DB
        self.check_db_key_removal(self.flex_cntr_db, FC_FLEX_COUNTER_TABLE_NAME,
                                  "{}:{}".format(FC_FLEX_COUNTER_TABLE_PFC_WD_KEY_PREFIX, port_oid))
        # Verify queue level counter removed from FLEX_COUNTER_DB
        self.check_db_key_removal(self.flex_cntr_db, FC_FLEX_COUNTER_TABLE_NAME,
                                  "{}:{}".format(FC_FLEX_COUNTER_TABLE_PFC_WD_KEY_PREFIX, q3_oid))
        q4_oid = self.get_queue_oid(dvs, PORT_UNDER_TEST, QUEUE_4)
        self.check_db_key_removal(self.flex_cntr_db, FC_FLEX_COUNTER_TABLE_NAME,
                                  "{}:{}".format(FC_FLEX_COUNTER_TABLE_PFC_WD_KEY_PREFIX, q4_oid))
        # Verify pfc wd fields removed from COUNTERS_DB
        fields = [PFC_WD_STATUS]
        self.check_db_fields_removal(self.cntrs_db, CNTR_COUNTERS_TABLE_NAME, q3_oid, fields)
        self.check_db_fields_removal(self.cntrs_db, CNTR_COUNTERS_TABLE_NAME, q4_oid, fields)

        # Verify queue in-storm status removed from APPL_DB
        self.check_db_key_removal(self.appl_db, APPL_PFC_WD_INSTORM_TABLE_NAME, PORT_UNDER_TEST)

        # Verify pfc enable bits in ASIC_DB (stay unchanged)
        fv_dict = {
            "SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL": "16",
        }
        self.check_db_fvs(self.asic_db, ASIC_PORT_TABLE_NAME, port_oid, fv_dict)

        # clean up
        # Stop pfc storm on queue 3
        self.stop_queue_pfc_storm(q3_oid)
        # Verify DEBUG_STORM field removed from COUNTERS_DB
        fields = [DEBUG_STORM]
        self.check_db_fields_removal(self.cntrs_db, CNTR_COUNTERS_TABLE_NAME, q3_oid, fields)

        # Clear queue 3 counters
        self.clear_queue_cntrs(q3_oid)

    # brs: big red switch
    def test_pfc_en_bits_user_wd_cfg_sep_brs(self, dvs, testlog):
        self.connect_dbs(dvs)

        # Enable pfc wd flex counter polling
        self.enable_flex_counter(CFG_FLEX_COUNTER_TABLE_PFCWD_KEY)
        # Verify pfc wd flex counter status published to FLEX_COUNTER_DB FLEX_COUNTER_GROUP_TABLE by flex counter orch
        fv_dict = {
            FLEX_COUNTER_STATUS: ENABLE,
        }
        self.check_db_fvs(self.flex_cntr_db, FC_FLEX_COUNTER_GROUP_TABLE_NAME, FC_FLEX_COUNTER_GROUP_TABLE_PFC_WD_KEY, fv_dict)

        # Enable pfc on tc 4
        pfc_tcs = [QUEUE_4]
        self.set_port_pfc(PORT_UNDER_TEST, pfc_tcs)

        # Verify pfc enable bits in ASIC_DB
        port_oid = dvs.asicdb.portnamemap[PORT_UNDER_TEST]
        fv_dict = {
            "SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL": "16",
        }
        self.check_db_fvs(self.asic_db, ASIC_PORT_TABLE_NAME, port_oid, fv_dict)

        # Start pfc wd (config) on port
        self.start_port_pfcwd(PORT_UNDER_TEST)
        # Verify port level counter to poll published to FLEX_COUNTER_DB FLEX_COUNTER_TABLE by pfc wd orch
        self.check_db_key_existence(self.flex_cntr_db, FC_FLEX_COUNTER_TABLE_NAME,
                                    "{}:{}".format(FC_FLEX_COUNTER_TABLE_PFC_WD_KEY_PREFIX, port_oid))
        # Verify queue level counter to poll published to FLEX_COUNTER_DB FLEX_COUNTER_TABLE by pfc wd orch
        q4_oid = self.get_queue_oid(dvs, PORT_UNDER_TEST, QUEUE_4)
        self.check_db_key_existence(self.flex_cntr_db, FC_FLEX_COUNTER_TABLE_NAME,
                                    "{}:{}".format(FC_FLEX_COUNTER_TABLE_PFC_WD_KEY_PREFIX, q4_oid))

        # Verify pfc enable bits stay unchanged in ASIC_DB
        time.sleep(2)
        fv_dict = {
            "SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL": "16",
        }
        self.check_db_fvs(self.asic_db, ASIC_PORT_TABLE_NAME, port_oid, fv_dict)

        # Enable big red switch
        self.enable_big_red_switch()
        # Verify queue 4 in brs from COUNTERS_DB
        fv_dict = {
            BIG_RED_SWITCH_MODE: ENABLE,
            PFC_WD_STATUS: STORMED,
            PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED: "1",
            PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED: "0",
        }
        self.check_db_fvs(self.cntrs_db, CNTR_COUNTERS_TABLE_NAME, q4_oid, fv_dict)

        # Verify pfc enable bits change in ASIC_DB
        fv_dict = {
            "SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL": "0",
        }
        self.check_db_fvs(self.asic_db, ASIC_PORT_TABLE_NAME, port_oid, fv_dict)

        # Re-set pfc enable on tc 4
        pfc_tcs = [QUEUE_4]
        self.set_port_pfc(PORT_UNDER_TEST, pfc_tcs)

        # Verify pfc enable bits stay unchanged in ASIC_DB
        time.sleep(2)
        fv_dict = {
            "SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL": "0",
        }
        self.check_db_fvs(self.asic_db, ASIC_PORT_TABLE_NAME, port_oid, fv_dict)

        # Change pfc enable bits: disable pfc on tc 4, and enable pfc on tc 3
        pfc_tcs = [QUEUE_3]
        self.set_port_pfc(PORT_UNDER_TEST, pfc_tcs)

        # Verify pfc enable bits change in ASIC_DB
        fv_dict = {
            "SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL": "8",
        }
        self.check_db_fvs(self.asic_db, ASIC_PORT_TABLE_NAME, port_oid, fv_dict)

        # Disable big red switch
        self.disable_big_red_switch()
        # Verify brs field removed from COUNTERS_DB
        fields = [BIG_RED_SWITCH_MODE]
        self.check_db_fields_removal(self.cntrs_db, CNTR_COUNTERS_TABLE_NAME, q4_oid, fields)
        # Verify queue 4 operational from COUNTERS_DB
        fv_dict = {
            PFC_WD_STATUS: OPERATIONAL,
            PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED: "1",
            PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED: "1",
        }
        self.check_db_fvs(self.cntrs_db, CNTR_COUNTERS_TABLE_NAME, q4_oid, fv_dict)

        # Verify pfc enable bits in ASIC_DB (stay unchanged)
        fv_dict = {
            "SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL": "8",
        }
        self.check_db_fvs(self.asic_db, ASIC_PORT_TABLE_NAME, port_oid, fv_dict)

        # clean up
        # Stop pfc wd on port (i.e., remove pfc wd config from port)
        self.stop_port_pfcwd(PORT_UNDER_TEST)
        # Verify port level counter removed from FLEX_COUNTER_DB
        self.check_db_key_removal(self.flex_cntr_db, FC_FLEX_COUNTER_TABLE_NAME,
                                  "{}:{}".format(FC_FLEX_COUNTER_TABLE_PFC_WD_KEY_PREFIX, port_oid))
        # Verify queue level counter removed from FLEX_COUNTER_DB
        self.check_db_key_removal(self.flex_cntr_db, FC_FLEX_COUNTER_TABLE_NAME,
                                  "{}:{}".format(FC_FLEX_COUNTER_TABLE_PFC_WD_KEY_PREFIX, q4_oid))
        q3_oid = self.get_queue_oid(dvs, PORT_UNDER_TEST, QUEUE_3)
        self.check_db_key_removal(self.flex_cntr_db, FC_FLEX_COUNTER_TABLE_NAME,
                                  "{}:{}".format(FC_FLEX_COUNTER_TABLE_PFC_WD_KEY_PREFIX, q3_oid))
        # Verify pfc wd fields removed from COUNTERS_DB
        fields = [PFC_WD_STATUS]
        self.check_db_fields_removal(self.cntrs_db, CNTR_COUNTERS_TABLE_NAME, q4_oid, fields)
        self.check_db_fields_removal(self.cntrs_db, CNTR_COUNTERS_TABLE_NAME, q3_oid, fields)

        # Clear queue counters
        self.clear_queue_cntrs(q3_oid)
        self.clear_queue_cntrs(q4_oid)

    # brs: big red switch
    def test_appl_db_storm_status_removal_brs(self, dvs, testlog):
        self.connect_dbs(dvs)

        # Enable pfc wd flex counter polling
        self.enable_flex_counter(CFG_FLEX_COUNTER_TABLE_PFCWD_KEY)
        # Verify pfc wd flex counter status published to FLEX_COUNTER_DB FLEX_COUNTER_GROUP_TABLE by flex counter orch
        fv_dict = {
            FLEX_COUNTER_STATUS: ENABLE,
        }
        self.check_db_fvs(self.flex_cntr_db, FC_FLEX_COUNTER_GROUP_TABLE_NAME, FC_FLEX_COUNTER_GROUP_TABLE_PFC_WD_KEY, fv_dict)

        # Enable pfc on tc 3
        pfc_tcs = [QUEUE_3]
        self.set_port_pfc(PORT_UNDER_TEST, pfc_tcs)
        # Verify pfc enable bits in ASIC_DB
        port_oid = dvs.asicdb.portnamemap[PORT_UNDER_TEST]
        fv_dict = {
            "SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL": "8",
        }
        self.check_db_fvs(self.asic_db, ASIC_PORT_TABLE_NAME, port_oid, fv_dict)

        # Start pfc wd (config) on port
        self.start_port_pfcwd(PORT_UNDER_TEST)
        # Verify port level counter to poll published to FLEX_COUNTER_DB FLEX_COUNTER_TABLE by pfc wd orch
        self.check_db_key_existence(self.flex_cntr_db, FC_FLEX_COUNTER_TABLE_NAME,
                                    "{}:{}".format(FC_FLEX_COUNTER_TABLE_PFC_WD_KEY_PREFIX, port_oid))
        # Verify queue level counter to poll published to FLEX_COUNTER_DB FLEX_COUNTER_TABLE by pfc wd orch
        q3_oid = self.get_queue_oid(dvs, PORT_UNDER_TEST, QUEUE_3)
        self.check_db_key_existence(self.flex_cntr_db, FC_FLEX_COUNTER_TABLE_NAME,
                                    "{}:{}".format(FC_FLEX_COUNTER_TABLE_PFC_WD_KEY_PREFIX, q3_oid))

        # Start pfc storm on queue 3
        self.start_queue_pfc_storm(q3_oid)
        # Verify queue in storm from COUNTERS_DB
        fv_dict = {
            PFC_WD_STATUS: STORMED,
            PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED: "1",
            PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED: "0",
        }
        self.check_db_fvs(self.cntrs_db, CNTR_COUNTERS_TABLE_NAME, q3_oid, fv_dict)
        # Verify queue in storm from APPL_DB
        fv_dict = {
            QUEUE_3: STORM,
        }
        self.check_db_fvs(self.appl_db, APPL_PFC_WD_INSTORM_TABLE_NAME, PORT_UNDER_TEST, fv_dict)

        # Enable big red switch
        self.enable_big_red_switch()
        # Verify queue 3 in brs from COUNTERS_DB
        fv_dict = {
            BIG_RED_SWITCH_MODE: ENABLE,
            PFC_WD_STATUS: STORMED,
            PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED: "2",
            PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED: "1",
        }
        self.check_db_fvs(self.cntrs_db, CNTR_COUNTERS_TABLE_NAME, q3_oid, fv_dict)

        # Verify queue in-storm status removed from APPL_DB
        self.check_db_key_removal(self.appl_db, APPL_PFC_WD_INSTORM_TABLE_NAME, PORT_UNDER_TEST)

        # Stop pfc storm on queue 3
        self.stop_queue_pfc_storm(q3_oid)
        # Verify DEBUG_STORM field removed from COUNTERS_DB
        fields = [DEBUG_STORM]
        self.check_db_fields_removal(self.cntrs_db, CNTR_COUNTERS_TABLE_NAME, q3_oid, fields)

        # Disable big red switch
        self.disable_big_red_switch()
        # Verify brs field removed from COUNTERS_DB
        fields = [BIG_RED_SWITCH_MODE]
        self.check_db_fields_removal(self.cntrs_db, CNTR_COUNTERS_TABLE_NAME, q3_oid, fields)
        # Verify queue operational from COUNTERS_DB
        fv_dict = {
            PFC_WD_STATUS: OPERATIONAL,
            PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED: "2",
            PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED: "2",
        }
        self.check_db_fvs(self.cntrs_db, CNTR_COUNTERS_TABLE_NAME, q3_oid, fv_dict)

        # Verify queue in-storm status removed from APPL_DB
        self.check_db_key_removal(self.appl_db, APPL_PFC_WD_INSTORM_TABLE_NAME, PORT_UNDER_TEST)

        # clean up
        # Stop pfc wd on port (i.e., remove pfc wd config from port)
        self.stop_port_pfcwd(PORT_UNDER_TEST)
        # Verify port level counter removed from FLEX_COUNTER_DB
        self.check_db_key_removal(self.flex_cntr_db, FC_FLEX_COUNTER_TABLE_NAME,
                                  "{}:{}".format(FC_FLEX_COUNTER_TABLE_PFC_WD_KEY_PREFIX, port_oid))
        # Verify queue level counter removed from FLEX_COUNTER_DB
        self.check_db_key_removal(self.flex_cntr_db, FC_FLEX_COUNTER_TABLE_NAME,
                                  "{}:{}".format(FC_FLEX_COUNTER_TABLE_PFC_WD_KEY_PREFIX, q3_oid))
        # Verify pfc wd fields removed from COUNTERS_DB
        fields = [PFC_WD_STATUS]
        self.check_db_fields_removal(self.cntrs_db, CNTR_COUNTERS_TABLE_NAME, q3_oid, fields)

        # Clear queue 3 counters
        self.clear_queue_cntrs(q3_oid)

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
