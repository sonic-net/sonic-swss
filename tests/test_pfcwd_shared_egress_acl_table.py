import time
import pytest
from dvslib.dvs_common import wait_for_result, PollingConfig

# set ASIC_TYPE=broadcom-dnx to test broadcom-dnx specific implementation.
# set PFC_DLR_INIT_ENABLE=0 to test PfcWdAclHandler, instead of PfcWdDlrHandler.
DVS_ENV = ["ASIC_TYPE=broadcom-dnx", "PFC_DLR_INIT_ENABLE=0"]
   
class TestPfcwdFunc(object):
    @pytest.fixture
    def select_lc(self, vct):
        # find a LC to test PFCWD
        self.dvs = None
        for name in vct.dvss.keys():
            dvs = vct.dvss[name]
            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")
            cfg_switch_type = metatbl.get("switch_type")
            if cfg_switch_type == "voq":
                self.dvs = dvs
        assert self.dvs
       
    @pytest.fixture
    def setup_teardown_test(self, select_lc):
        self.asic_db = self.dvs.get_asic_db()
        self.config_db = self.dvs.get_config_db()
        self.counters_db = self.dvs.get_counters_db()
       
        self.test_ports = ["Ethernet0"]
        self.setup_test(self.dvs)

        self.port_oids = self.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        self.queue_oids = self.counters_db.get_entry("COUNTERS_QUEUE_NAME_MAP", "")
        
        yield

        self.teardown_test(self.dvs)

    def setup_test(self, dvs):
        # save original cable length and set new cabel length
        fvs = self.config_db.get_entry("CABLE_LENGTH", "AZURE")
        self.orig_cable_len = dict()
        for port in self.test_ports:
            self.orig_cable_len[port] = fvs[port]
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

    def set_flex_counter_status(self, key, state):
        fvs = {'FLEX_COUNTER_STATUS': state}
        self.config_db.update_entry("FLEX_COUNTER_TABLE", key, fvs)
        time.sleep(1)
            
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

    def set_storm_state(self, queues, state="enabled"):
        fvs = {"DEBUG_STORM": state}
        for port in self.test_ports:
            for queue in queues:
                queue_name = port + ":" + str(queue)
                self.counters_db.update_entry("COUNTERS", self.queue_oids[queue_name], fvs)

    def verify_egress_acls(self, expected_acls=None):
        def do_verify_egress_acls():
            egress_acl_table_oids = []
            acl_table_name = "ASIC_STATE:SAI_OBJECT_TYPE_ACL_TABLE"
            for oid_key in self.asic_db.get_keys(acl_table_name):
                entry = self.asic_db.get_entry(acl_table_name, oid_key)
                # find egress ACL table by checking ACL table attributes
                if (entry.get("SAI_ACL_TABLE_ATTR_ACL_STAGE") == "SAI_ACL_STAGE_EGRESS" and
                    entry.get("SAI_ACL_TABLE_ATTR_FIELD_TC") == "true" and
                    entry.get("SAI_ACL_TABLE_ATTR_FIELD_OUT_PORT") == "true" and
                    entry.get("SAI_ACL_TABLE_ATTR_ACL_BIND_POINT_TYPE_LIST") == "1:SAI_ACL_BIND_POINT_TYPE_SWITCH"):
                    egress_acl_table_oids.append(oid_key)
            if len(egress_acl_table_oids) != 1:
                return (False, None)

            # find installed ACL entries in egress ACL tables. 
            installed_acls = []
            acl_entry_table_name = "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY"
            for oid_key in self.asic_db.get_keys(acl_entry_table_name):
                entry = self.asic_db.get_entry(acl_entry_table_name, oid_key)
                if entry.get("SAI_ACL_ENTRY_ATTR_TABLE_ID") in egress_acl_table_oids:
                    port_oid = entry.get("SAI_ACL_ENTRY_ATTR_FIELD_OUT_PORT")
                    tc = entry.get("SAI_ACL_ENTRY_ATTR_FIELD_TC")
                    tc = int(tc.replace("&mask:0xff", ""))
                    installed_acls.append((port_oid, tc))

            # verify installed ACLs against expected ones
            return (sorted(installed_acls) == sorted(expected_acls), None)
            
        max_poll = PollingConfig(polling_interval=5, timeout=600, strict=True)
        wait_for_result(do_verify_egress_acls, polling_config=max_poll)
            
    def test_pfcwd_shared_egress_acl_table(self, setup_teardown_test):
        try:
            # enable PFC on queues
            test_queues = [3, 4]
            self.set_ports_pfc(pfc_queues=test_queues)

            # start PFCWD on ports and PFC storm
            self.start_pfcwd_on_ports()
            storm_queue = test_queues
            self.set_storm_state(storm_queue)

            # verify egress ACLs in asic db
            expected_acls = []
            for port in self.test_ports:
                for queue in storm_queue:
                    expected_acls.append((self.port_oids[port], queue))
            self.verify_egress_acls(expected_acls)
            
            # stop storm and PFCWD on port.
            self.set_storm_state(storm_queue, state="disabled")
            self.stop_pfcwd_on_ports()

            # verify egress ACLs and table are deleted.
            expected_acls = []
            self.verify_egress_acls(expected_acls)

        finally:
            self.stop_pfcwd_on_ports()


#
# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
