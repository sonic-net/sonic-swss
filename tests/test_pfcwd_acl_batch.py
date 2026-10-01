import time
import pytest
from dvslib.dvs_common import wait_for_result, PollingConfig

# PfcWdAclHandler is only instantiated for Broadcom platforms; the vs container
# maps ASIC_TYPE=broadcom-dnx to platform=broadcom, and PFC_DLR_INIT_ENABLE=0
# selects the software watchdog over the hardware one.
DVS_ENV = ["ASIC_TYPE=broadcom-dnx", "PFC_DLR_INIT_ENABLE=0"]


class TestPfcwdBatchedStorm(object):
    """PFC storm on several ports inside a single detection cycle.

    PfcWdAclHandler does not program the ingress ACL from its constructor: it
    queues an IN_PORTS change that PfcWdSwOrch applies once per batch of storm
    events, one set_acl_entry_attribute per queue index instead of one per
    (port, queue). These tests check that the batching is transparent - every
    storming port ends up in the rule, a port that recovers alone leaves the
    others in place, and the rule is removed rather than written empty when the
    last port recovers.
    """

    TEST_PORTS = ["Ethernet0", "Ethernet8", "Ethernet16", "Ethernet24"]

    ACL_ENTRY_TABLE = "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY"
    TC_FIELD = "SAI_ACL_ENTRY_ATTR_FIELD_TC"
    IN_PORTS_FIELD = "SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS"

    @pytest.fixture
    def setup_teardown_test(self, dvs):
        self.asic_db = dvs.get_asic_db()
        self.config_db = dvs.get_config_db()
        self.counters_db = dvs.get_counters_db()
        self.test_ports = list(self.TEST_PORTS)
        self.setup_test(dvs)
        self.port_oids = self.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        self.queue_oids = self.counters_db.get_entry("COUNTERS_QUEUE_NAME_MAP", "")
        yield
        self.teardown_test(dvs)

    def setup_test(self, dvs):
        fvs = self.config_db.get_entry("CABLE_LENGTH", "AZURE")
        self.orig_cable_len = dict()
        for port in self.test_ports:
            self.orig_cable_len[port] = fvs[port]
            # A down port reports cable length 0; the buffer model needs a real one.
            self.set_cable_len(port, "5m")
            dvs.port_admin_set(port, "up")
        self.set_flex_counter_status("PFCWD", "enable")
        # Queue OIDs are only generated once the queue counters are enabled.
        self.set_flex_counter_status("QUEUE", "enable")

    def teardown_test(self, dvs):
        self.set_flex_counter_status("PFCWD", "disable")
        self.set_flex_counter_status("QUEUE", "disable")
        for port in self.test_ports:
            if self.orig_cable_len:
                self.set_cable_len(port, self.orig_cable_len[port])
            dvs.port_admin_set(port, "down")

    def set_flex_counter_status(self, key, state):
        fvs = {'FLEX_COUNTER_STATUS': state}
        self.config_db.update_entry("FLEX_COUNTER_TABLE", key, fvs)
        time.sleep(1)

    def set_cable_len(self, port_name, cable_len):
        fvs = {port_name: cable_len}
        self.config_db.update_entry("CABLE_LEN", "AZURE", fvs)

    def _get_bitmask(self, queues):
        mask = 0
        for queue in queues:
            mask = mask | 1 << queue
        return str(mask)

    def set_ports_pfc(self, status='enable', pfc_queues=[3, 4]):
        keyname = 'pfcwd_sw_enable'
        for port in self.test_ports:
            if 'enable' in status:
                queues = ",".join([str(q) for q in pfc_queues])
                fvs = {keyname: queues, 'pfc_enable': queues}
                self.config_db.create_entry("PORT_QOS_MAP", port, fvs)
            else:
                self.config_db.delete_entry("PORT_QOS_MAP", port)

    def verify_ports_pfc(self, queues):
        fvs = {"SAI_PORT_ATTR_PRIORITY_FLOW_CONTROL": self._get_bitmask(queues)}
        for port in self.test_ports:
            self.asic_db.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", self.port_oids[port], fvs)

    def start_pfcwd_on_ports(self, poll_interval="200", detection_time="200", restoration_time="200", action="drop"):
        self.config_db.update_entry("PFC_WD", "GLOBAL", {"POLL_INTERVAL": poll_interval})
        pfcwd_info = {"action": action,
                      "detection_time": detection_time,
                      "restoration_time": restoration_time}
        for port in self.test_ports:
            self.config_db.update_entry("PFC_WD", port, pfcwd_info)

    def stop_pfcwd_on_ports(self):
        for port in self.test_ports:
            self.config_db.delete_entry("PFC_WD", port)

    def set_storm_state(self, queues, state="enabled", ports=None):
        fvs = {"DEBUG_STORM": state}
        for port in (ports if ports is not None else self.test_ports):
            for queue in queues:
                queue_name = port + ":" + str(queue)
                self.counters_db.update_entry("COUNTERS", self.queue_oids[queue_name], fvs)

    def verify_pfcwd_state(self, queues, state="stormed", ports=None):
        fvs = {"PFC_WD_STATUS": state}
        for port in (ports if ports is not None else self.test_ports):
            for queue in queues:
                queue_name = port + ":" + str(queue)
                self.counters_db.wait_for_field_match("COUNTERS", self.queue_oids[queue_name], fvs)

    def reset_pfcwd_counters(self, queues):
        fvs = {"PFC_WD_QUEUE_STATS_DEADLOCK_DETECTED": "0",
               "PFC_WD_QUEUE_STATS_DEADLOCK_RESTORED": "0"}
        for port in self.test_ports:
            for queue in queues:
                queue_name = port + ":" + str(queue)
                self.counters_db.update_entry("COUNTERS", self.queue_oids[queue_name], fvs)

    def _pfcwd_acl_entry_in_ports(self, tc):
        """IN_PORTS of the PFCWD ingress drop rule for `tc`, or None."""
        for key in self.asic_db.get_keys(self.ACL_ENTRY_TABLE):
            entry = self.asic_db.get_entry(self.ACL_ENTRY_TABLE, key)
            tc_value = entry.get(self.TC_FIELD, "")
            if not tc_value.startswith("{}&mask".format(tc)):
                continue
            if self.IN_PORTS_FIELD not in entry:
                continue
            return entry[self.IN_PORTS_FIELD]

        return None

    def _verify_in_ports(self, tc, expected_ports):
        """Wait until the TC rule's IN_PORTS is exactly `expected_ports`."""
        expected_oids = set(self.port_oids[p] for p in expected_ports)

        def _matches():
            in_ports = self._pfcwd_acl_entry_in_ports(tc)

            if not expected_oids:
                # The last storming port recovered: the rule must be gone
                # rather than left with an empty (i.e. match-everything) list.
                return (in_ports is None, in_ports)

            if in_ports is None:
                return (False, None)

            # Format is "<count>:oid:0x..,oid:0x.."
            count, _, oid_list = in_ports.partition(":")
            found = set(oid_list.split(",")) if oid_list else set()

            return (count == str(len(expected_oids)) and found == expected_oids, in_ports)

        # strict=False so the assertion below reports what was actually found.
        status, result = wait_for_result(
            _matches, PollingConfig(polling_interval=1, timeout=30, strict=False))
        assert status, \
            "IN_PORTS for TC {} is {}, expected exactly {}".format(tc, result, expected_oids)

    def test_pfcwd_storm_on_all_ports_batched(self, dvs, setup_teardown_test):
        """All ports storm on the same queue, then all recover."""
        test_queues = [3, 4]
        storm_queue = [3]

        try:
            self.set_ports_pfc(pfc_queues=test_queues)
            self.verify_ports_pfc(test_queues)

            self.start_pfcwd_on_ports()

            self.set_storm_state(storm_queue)
            self.verify_pfcwd_state(storm_queue)

            # Every storming port must be in the single TC 3 rule, even though
            # the events were coalesced into one ACL update.
            self._verify_in_ports(storm_queue[0], self.test_ports)

            self.set_storm_state(storm_queue, state="disabled")
            self.verify_pfcwd_state(storm_queue, state="operational")

            # ...and all of them must be removed again.
            self._verify_in_ports(storm_queue[0], [])

        finally:
            self.set_storm_state(storm_queue, state="disabled")
            self.reset_pfcwd_counters(storm_queue)
            self.stop_pfcwd_on_ports()
            self.set_ports_pfc(status='disable')

    def test_pfcwd_partial_restore_keeps_other_ports(self, dvs, setup_teardown_test):
        """One port recovering must not clear the rule for the others."""
        test_queues = [3, 4]
        storm_queue = [3]
        recovered = self.test_ports[0]
        still_storming = self.test_ports[1:]

        try:
            self.set_ports_pfc(pfc_queues=test_queues)
            self.verify_ports_pfc(test_queues)

            self.start_pfcwd_on_ports()

            self.set_storm_state(storm_queue)
            self.verify_pfcwd_state(storm_queue)
            self._verify_in_ports(storm_queue[0], self.test_ports)

            # Stop the storm on one port only.
            self.set_storm_state(storm_queue, state="disabled", ports=[recovered])
            self.verify_pfcwd_state(storm_queue, state="operational", ports=[recovered])

            self._verify_in_ports(storm_queue[0], still_storming)

        finally:
            self.set_storm_state(storm_queue, state="disabled")
            self.reset_pfcwd_counters(storm_queue)
            self.stop_pfcwd_on_ports()
            self.set_ports_pfc(status='disable')


#
# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
