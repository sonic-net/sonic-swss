from dvslib.dvs_common import wait_for_result, PollingConfig
import pytest

class TestFabricSwitchId(object):
    def check_syslog(self, dvs, marker, log):
        def do_check_syslog():
            (ec, out) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep \'%s\' | wc -l" %(marker, log)])
            return (int(out.strip()) >= 1, None)
        max_poll = PollingConfig(polling_interval=5, timeout=600, strict=True)
        wait_for_result(do_check_syslog, polling_config=max_poll)

    def test_invalid_fabric_switch_id(self, vst):
        # Find supervisor dvs.
        dvs = None
        config_db = None
        for name in vst.dvss.keys():
            dvs = vst.dvss[name]
            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")
            cfg_switch_type = metatbl.get("switch_type")
            if cfg_switch_type == "fabric":
                break
        assert dvs and config_db

        # Verify orchagent's handling of invalid fabric switch_id in following cases:
        # - Invalid fabric switch_id, e.g, -1, is set.
        # - fabric switch_id is missing in ConfigDb.
        for invalid_switch_id in (-1, None):
            print(f"Test invalid switch id {invalid_switch_id}")
            if invalid_switch_id is None:
                config_db.delete_field("DEVICE_METADATA", "localhost", "switch_id")
                expected_log = "Fabric switch id is not configured"
            else:
                config_db.set_field("DEVICE_METADATA", "localhost", "switch_id", str(invalid_switch_id))
                expected_log = f"Invalid fabric switch id {invalid_switch_id} configured"

            # Restart orchagent and verify orchagent behavior by checking syslog.
            dvs.stop_swss()
            marker = dvs.add_log_marker()
            dvs.start_swss()
            self.check_syslog(dvs, marker, expected_log)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass

