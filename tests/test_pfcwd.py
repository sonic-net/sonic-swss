import redis
import time
import os
import pytest
import re
import json
from swsscommon import swsscommon

PFCWD_TABLE_NAME = "DROP_TEST_TABLE"
PFCWD_TABLE_TYPE = "DROP"
PFCWD_TC = ["3", "4"]
PFCWD_RULE_NAME_1 =  "DROP_TEST_RULE_1"
PFCWD_RULE_NAME_2 =  "DROP_TEST_RULE_2"

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


class TestPfcwdFunc(object):
    @pytest.fixture
    def setup_teardown_test(self, dvs):
        self.get_db_handle(dvs)

        self.test_ports = ["Ethernet0"]

        self.setup_test(dvs)
        self.get_port_oids()
        self.get_queue_oids()

        yield

        self.teardown_test(dvs)

    def setup_test(self, dvs):
        # get original cable len for test ports
        fvs = self.config_db.get_entry("CABLE_LENGTH", "AZURE")
        self.orig_cable_len = dict()
        for port in self.test_ports:
            self.orig_cable_len[port] = fvs[port]
            # set cable len to non zero value. if port is down, default cable len is 0
            self.set_cable_len(port, "5m")
            # startup port
            dvs.port_admin_set(port, "up")

        # enable pfcwd
        self.set_flex_counter_status("PFCWD", "enable")
        # enable queue so that queue oids are generated
        self.set_flex_counter_status("QUEUE", "enable")

    def teardown_test(self, dvs):
        # disable pfcwd
        self.set_flex_counter_status("PFCWD", "disable")
        # disable queue
        self.set_flex_counter_status("QUEUE", "disable")

        for port in self.test_ports:
            if self.orig_cable_len:
                self.set_cable_len(port, self.orig_cable_len[port])
            # shutdown port
            dvs.port_admin_set(port, "down")

    def get_db_handle(self, dvs):
        self.app_db = dvs.get_app_db()
        self.asic_db = dvs.get_asic_db()
        self.config_db = dvs.get_config_db()
        self.counters_db = dvs.get_counters_db()

    def set_flex_counter_status(self, key, state):
        fvs = {'FLEX_COUNTER_STATUS': state}
        self.config_db.update_entry("FLEX_COUNTER_TABLE", key, fvs)
        time.sleep(1)

    def get_queue_oids(self):
        self.queue_oids = self.counters_db.get_entry("COUNTERS_QUEUE_NAME_MAP", "")

    def get_port_oids(self):
        self.port_oids = self.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")

    def _get_bitmask(self, queues):
        mask = 0
        if queues is not None:
            for queue in queues:
                mask = mask | 1 << queue

        return str(mask)

    def set_ports_pfc(self, status='enable', pfc_queues=[3,4]):
        keyname = 'pfcwd_sw_enable'
        for port in self.test_ports:
            if 'enable' in status:
                queues = ",".join([str(q) for q in pfc_queues])
                fvs = {keyname: queues, 'pfc_enable': queues}
                self.config_db.create_entry("PORT_QOS_MAP", port, fvs)
            else:
                self.config_db.delete_entry("PORT_QOS_MAP", port)

    def set_cable_len(self, port_name, cable_len):
        fvs = {port_name: cable_len}
        self.config_db.update_entry("CABLE_LEN", "AZURE", fvs)

    def start_pfcwd_on_ports(self, poll_interval="200", detection_time="200", restoration_time="200", action="drop"):
        pfcwd_info = {"POLL_INTERVAL": poll_interval}
        self.config_db.update_entry("PFC_WD", "GLOBAL", pfcwd_info)

        pfcwd_info = {"action": action,
                      "detection_time" : detection_time,
                      "restoration_time": restoration_time
                     }
        for port in self.test_ports:
            self.config_db.update_entry("PFC_WD", port, pfcwd_info)

    def stop_pfcwd_on_ports(self):
        for port in self.test_ports:
            self.config_db.delete_entry("PFC_WD", port)

    def verify_ports_pfc(self, queues=None):
        mask = self._get_bitmask(queues)
        fvs = {"SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL" : mask}
        for port in self.test_ports:
            self.asic_db.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", self.port_oids[port], fvs)

    def verify_pfcwd_state(self, queues, state="stormed"):
        fvs = {"PFC_WD_STATUS": state}
        for port in self.test_ports:
            for queue in queues:
                queue_name = port + ":" + str(queue)
                self.counters_db.wait_for_field_match("COUNTERS", self.queue_oids[queue_name], fvs)

    def verify_pfcwd_counters(self, queues, restore="0"):
        fvs = {"PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED" : "1",
               "PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED" : restore
              }
        for port in self.test_ports:
            for queue in queues:
                queue_name = port + ":" + str(queue)
                self.counters_db.wait_for_field_match("COUNTERS", self.queue_oids[queue_name], fvs)

    def reset_pfcwd_counters(self, queues):
        fvs = {"PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED" : "0",
               "PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED" : "0"
              }
        for port in self.test_ports:
            for queue in queues:
                queue_name = port + ":" + str(queue)
                self.counters_db.update_entry("COUNTERS", self.queue_oids[queue_name], fvs)

    def set_storm_state(self, queues, state="enabled"):
        fvs = {"DEBUG_STORM": state}
        for port in self.test_ports:
            for queue in queues:
                queue_name = port + ":" + str(queue)
                self.counters_db.update_entry("COUNTERS", self.queue_oids[queue_name], fvs)

    def test_pfcwd_software_single_queue(self, dvs, setup_teardown_test):
        try:
            # enable PFC on queues
            test_queues = [3, 4]
            self.set_ports_pfc(pfc_queues=test_queues)

            # verify in asic db
            self.verify_ports_pfc(test_queues)

            # start pfcwd
            self.start_pfcwd_on_ports()

            # start pfc storm
            storm_queue = [3]
            self.set_storm_state(storm_queue)

            # verify pfcwd is triggered
            self.verify_pfcwd_state(storm_queue)

            # verify pfcwd counters
            self.verify_pfcwd_counters(storm_queue)

            # verify if queue is disabled
            self.verify_ports_pfc(queues=[4])

            # stop storm
            self.set_storm_state(storm_queue, state="disabled")

            # verify pfcwd state is restored
            self.verify_pfcwd_state(storm_queue, state="operational")

            # verify pfcwd counters
            self.verify_pfcwd_counters(storm_queue, restore="1")

            # verify if queue is enabled
            self.verify_ports_pfc(test_queues)

        finally:
            self.reset_pfcwd_counters(storm_queue)
            self.stop_pfcwd_on_ports()

    def test_pfcwd_software_multi_queue(self, dvs, setup_teardown_test):
        try:
            # enable PFC on queues
            test_queues = [3, 4]
            self.set_ports_pfc(pfc_queues=test_queues)

            # verify in asic db
            self.verify_ports_pfc(test_queues)

            # start pfcwd
            self.start_pfcwd_on_ports()

            # start pfc storm
            self.set_storm_state(test_queues)

            # verify pfcwd is triggered
            self.verify_pfcwd_state(test_queues)

            # verify pfcwd counters
            self.verify_pfcwd_counters(test_queues)

            # verify if queue is disabled. Expected mask is 0
            self.verify_ports_pfc()

            # stop storm
            self.set_storm_state(test_queues, state="disabled")

            # verify pfcwd state is restored
            self.verify_pfcwd_state(test_queues, state="operational")

            # verify pfcwd counters
            self.verify_pfcwd_counters(test_queues, restore="1")

            # verify if queue is enabled
            self.verify_ports_pfc(test_queues)

        finally:
            self.reset_pfcwd_counters(test_queues)
            self.stop_pfcwd_on_ports()

    def test_pfcwd_global_key_del_no_crash(self, dvs, setup_teardown_test):
        """
        Verify orchagent does not crash when PFC_WD|GLOBAL is deleted from CONFIG_DB.

        Deleting PFC_WD|GLOBAL fires a "del" keyspace notification which routes to
        deleteEntry("GLOBAL"). Before the fix, getPort("GLOBAL") failed silently,
        leaving a default-constructed Port with empty m_queue_ids. unregisterFromWdDb()
        then accessed m_queue_ids[0] on the empty vector (null data pointer), causing
        SIGSEGV at address 0. After the fix, deleteEntry() checks the getPort() return
        value and returns task_invalid_entry cleanly for non-port keys.
        """
        storm_queue = [3]
        try:
            test_queues = [3, 4]
            self.set_ports_pfc(pfc_queues=test_queues)
            self.verify_ports_pfc(test_queues)

            # Start pfcwd — writes POLL_INTERVAL to PFC_WD|GLOBAL
            self.start_pfcwd_on_ports()
            time.sleep(1)

            # Delete PFC_WD|GLOBAL entirely.
            # Fires "del" keyspace notification → deleteEntry("GLOBAL") in orchagent.
            # Before fix: SIGSEGV in unregisterFromWdDb() accessing null m_queue_ids[0].
            self.config_db.delete_entry("PFC_WD", "GLOBAL")
            time.sleep(1)

            # Verify orchagent survived: restore GLOBAL and confirm pfcwd still works
            self.config_db.update_entry("PFC_WD", "GLOBAL", {"POLL_INTERVAL": "200"})
            time.sleep(1)

            self.set_storm_state(storm_queue)
            self.verify_pfcwd_state(storm_queue)
            self.verify_pfcwd_counters(storm_queue)

        finally:
            self.set_storm_state(storm_queue, state="disabled")
            self.reset_pfcwd_counters(storm_queue)
            self.stop_pfcwd_on_ports()
            self.config_db.delete_entry("PFC_WD", "GLOBAL")

    def test_pfcwd_global_hdel_last_field_no_crash(self, dvs, setup_teardown_test):
        """
        Verify orchagent does not crash when hdel removes the last field of PFC_WD|GLOBAL.

        With Redis notify-keyspace-events=AKE (which includes 'g' for generic commands),
        hdel of the last field causes Redis to auto-delete the key and fire both "hdel"
        and "del" keyspace notifications. The "del" notification triggers
        deleteEntry("GLOBAL"), hitting the same crash path as test_pfcwd_global_key_del_no_crash.
        """
        storm_queue = [3]
        try:
            test_queues = [3, 4]
            self.set_ports_pfc(pfc_queues=test_queues)
            self.verify_ports_pfc(test_queues)

            self.start_pfcwd_on_ports()
            time.sleep(1)

            # Replace GLOBAL entry with only BIG_RED_SWITCH so hdel of it is the last field
            self.config_db.delete_entry("PFC_WD", "GLOBAL")
            time.sleep(0.5)
            self.config_db.update_entry("PFC_WD", "GLOBAL", {"BIG_RED_SWITCH": "enable"})
            time.sleep(1)

            # Hdel the only remaining field.
            # With AKE keyspace events, Redis fires "hdel" + "del" → deleteEntry("GLOBAL").
            self.config_db.delete_field("PFC_WD", "GLOBAL", "BIG_RED_SWITCH")
            time.sleep(1)

            # Verify orchagent survived: restore GLOBAL and confirm pfcwd still works.
            # BIG_RED_SWITCH:enable above put pfcwdorch into big-red-switch mode, which
            # force-drops monitored queues and bypasses normal storm detection. Deleting
            # the field does not transition BRS off, so explicitly disable it before
            # verifying storm detection, otherwise the queue stays operational.
            self.config_db.update_entry("PFC_WD", "GLOBAL", {"BIG_RED_SWITCH": "disable"})
            time.sleep(1)
            self.config_db.delete_entry("PFC_WD", "GLOBAL")
            time.sleep(1)
            self.config_db.update_entry("PFC_WD", "GLOBAL", {"POLL_INTERVAL": "200"})
            time.sleep(1)

            # The BIG_RED_SWITCH enable/disable cycle above force-drops and restores the
            # monitored queues, bumping the cumulative DEADLOCK_DETECTED/RESTORED counters.
            # Reset them so the single storm below yields the expected detected=1,
            # restored=0 that verify_pfcwd_counters checks.
            self.reset_pfcwd_counters(storm_queue)

            self.set_storm_state(storm_queue)
            self.verify_pfcwd_state(storm_queue)
            self.verify_pfcwd_counters(storm_queue)

        finally:
            self.set_storm_state(storm_queue, state="disabled")
            self.reset_pfcwd_counters(storm_queue)
            self.stop_pfcwd_on_ports()
            self.config_db.delete_entry("PFC_WD", "GLOBAL")


#
# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
