import pytest
import time

from dvslib.dvs_common import PollingConfig, wait_for_result


class TestBuffer(object):
    from conftest import DockerVirtualSwitch
    lossless_pgs = []
    INTF = "Ethernet0"

    def setup_db(self, dvs):
        from conftest import ApplDbValidator, AsicDbValidator
        from dvslib.dvs_database import DVSDatabase
        
        self.app_db: ApplDbValidator = dvs.get_app_db()
        self.asic_db: AsicDbValidator = dvs.get_asic_db()
        self.config_db: DVSDatabase = dvs.get_config_db()
        self.counter_db: DVSDatabase = dvs.get_counters_db()

        # enable PG watermark
        self.set_pg_wm_status('enable')

    def get_pfc_enable_queues(self):
        qos_map = self.config_db.get_entry("PORT_QOS_MAP", self.INTF)
        return qos_map['pfc_enable'].split(',')

    def get_pg_oid(self, pg):
        fvs = dict()
        fvs = self.counter_db.get_entry("COUNTERS_PG_NAME_MAP", "")
        return fvs[pg]

    def set_pg_wm_status(self, state):
        fvs = {'FLEX_COUNTER_STATUS': state}
        self.config_db.update_entry("FLEX_COUNTER_TABLE", "PG_WATERMARK", fvs)
        time.sleep(1)

    def teardown(self):
        # disable PG watermark
        self.set_pg_wm_status('disable')

    def get_asic_buf_profile(self):
        return set(self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE"))

    def check_new_profile_in_asic_db(self, profile):
        retry_count = 0
        self.new_profile = None
        while retry_count < 5:
            retry_count += 1
            diff = set(self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE")) - self.orig_profiles
            if len(diff) == 1:
                self.new_profile = diff.pop()
                break
            else:
                time.sleep(1)
        assert self.new_profile, "Can't get SAI OID for newly created profile {} after retry {} times".format(profile, retry_count)

    def get_asic_buf_pg_profiles(self):
        self.buf_pg_profile = dict()
        for pg in self.pg_name_map:
            buf_pg_entries = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP", self.pg_name_map[pg])
            self.buf_pg_profile[pg] = buf_pg_entries["SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE"]

    def change_cable_len(self, cable_len, extra_port=None):
        fvs = dict()
        fvs[self.INTF] = cable_len
        if extra_port:
            fvs[extra_port] = cable_len
        self.config_db.update_entry("CABLE_LENGTH", "AZURE", fvs)

    def set_port_qos_table(self, port, pfc_enable_flag):
        fvs=dict()
        fvs['pfc_enable'] = pfc_enable_flag
        self.config_db.update_entry("PORT_QOS_MAP", port, fvs)
        self.lossless_pgs = pfc_enable_flag.split(',')

    def get_pg_name_map(self):
        pg_name_map = dict()
        for pg in self.lossless_pgs:
            pg_name = "{}:{}".format(self.INTF, pg)
            pg_name_map[pg_name] = self.get_pg_oid(pg_name)
        return pg_name_map
    
    def unused_cable_len(self, speed, cable_lens):
        """A length the lookup has, that no port uses and with no generated profile yet."""
        in_use = set(cable_lens.values())
        profiles = set(self.config_db.get_keys("BUFFER_PROFILE"))
        candidates = [c for c in ("5m", "40m", "300m")
                      if c not in in_use and "pg_lossless_{}_{}_profile".format(speed, c) not in profiles]
        assert candidates, "no unused cable length to test with"
        return candidates[0]

    def check_syslog(self, dvs, marker, err_log, expected_cnt=1):
        (exitcode, num) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep \"%s\" | wc -l" % (marker, err_log)])
        assert num.strip() >= str(expected_cnt)

    def wait_for_syslog(self, dvs, marker, pattern):
        def logged():
            _, num = dvs.runcmd(['sh', '-c', 'awk -v m="$1" \'found; index($0, m) {found=1}\' /var/log/syslog | grep -c -e "$2"',
                                 'sh', marker, pattern])
            return (num.strip().isdigit() and int(num.strip()) > 0, num)

        wait_for_result(logged, PollingConfig(polling_interval=0.5, timeout=20, strict=True),
                        "'{}' was not logged after {}".format(pattern, marker))

    @pytest.fixture
    def setup_teardown_test(self, dvs):
        self.setup_db(dvs)
        self.set_port_qos_table(self.INTF, '3,4')
        self.lossless_pg_combinations = ['3-4']
        time.sleep(2)

        yield

        self.teardown()

    def test_zero_cable_len_profile_update(self, dvs, setup_teardown_test):
        orig_cable_len = None
        orig_speed = None
        try:
            # get orig cable length and speed
            fvs = self.config_db.get_entry("CABLE_LENGTH", "AZURE")
            orig_cable_len = fvs[self.INTF]
            if orig_cable_len == "0m":
                fvs[self.INTF] = "300m"
                cable_len_before_test = "300m"
                self.config_db.update_entry("CABLE_LENGTH", "AZURE", fvs)
            else:
                cable_len_before_test = orig_cable_len
            fvs = self.config_db.get_entry("PORT", self.INTF)
            orig_speed = fvs["speed"]

            if orig_speed == "100000":
                test_speed = "40000"
            elif orig_speed == "40000":
                test_speed = "100000"
            test_cable_len = "0m"

            dvs.port_admin_set(self.INTF, "up")
            # Make sure the buffer PG has been created
            orig_lossless_profile = "pg_lossless_{}_{}_profile".format(orig_speed, cable_len_before_test)
            self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", orig_lossless_profile)
            self.pg_name_map = self.get_pg_name_map()
            self.orig_profiles = self.get_asic_buf_profile()

            # check if the lossless profile for the test speed is already present
            fvs = dict()
            new_lossless_profile = "pg_lossless_{}_{}_profile".format(test_speed, cable_len_before_test)
            fvs = self.app_db.get_entry("BUFFER_PROFILE_TABLE", new_lossless_profile)
            if len(fvs):
                profile_exp_cnt_diff = 0
            else:
                profile_exp_cnt_diff = 1

            # get the orig buf profiles attached to the pgs
            self.get_asic_buf_pg_profiles()

            # change cable length to 'test_cable_len'
            self.change_cable_len(test_cable_len)

            # change intf speed to 'test_speed'
            dvs.port_field_set(self.INTF, "speed", test_speed)
            test_lossless_profile = "pg_lossless_{}_{}_profile".format(test_speed, test_cable_len)
            # buffer profile should not get created
            self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", test_lossless_profile)

            # buffer pgs should still point to the original buffer profile
            for pg in self.lossless_pg_combinations:
                self.app_db.wait_for_field_match("BUFFER_PG_TABLE", self.INTF + ":" + pg, {"profile": orig_lossless_profile})
            fvs = dict()
            for pg in self.pg_name_map:
                fvs["SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE"] = self.buf_pg_profile[pg]
                self.asic_db.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP", self.pg_name_map[pg], fvs)

            # change cable length to 'cable_len_before_test'
            self.change_cable_len(cable_len_before_test)

            # change intf speed to 'test_speed'
            dvs.port_field_set(self.INTF, "speed", test_speed)
            if profile_exp_cnt_diff != 0:
                # new profile will get created
                self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", new_lossless_profile)
                self.check_new_profile_in_asic_db(new_lossless_profile)
                # verify that buffer pgs point to the new profile
                fvs = dict()
                for pg in self.pg_name_map:
                    fvs["SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE"] = self.new_profile
                    self.asic_db.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP", self.pg_name_map[pg], fvs)

            else:
                # verify that buffer pgs do not point to the old profile since we cannot deduce the new profile oid
                fvs = dict()
                for pg in self.pg_name_map:
                    fvs["SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE"] = self.buf_pg_profile[pg]
                    self.asic_db.wait_for_field_negative_match("ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP", self.pg_name_map[pg], fvs)
        finally:
            if orig_cable_len:
                self.change_cable_len(orig_cable_len)
            if orig_speed:
                dvs.port_field_set(self.INTF, "speed", orig_speed)
            dvs.port_admin_set(self.INTF, "down")

    # To verify the BUFFER_PG is not hardcoded to 3,4
    # buffermgrd will read 'pfc_enable' entry and apply lossless profile to that queue
    def test_lossless_pg_survives_buffermgrd_restart(self, dvs: DockerVirtualSwitch, setup_teardown_test):
        """
        A BUFFER_PG entry already in CONFIG_DB when buffermgrd starts is configuration,
        and a port event rewrites the same keys, so neither may remove it, even when the
        profile it references carries the generated pg_lossless_ prefix. A pfc_enable
        change removes only the range the previous value generated, so it stays then too.

        Each check that an entry is still there waits first for buffermgrd to log that
        it has rendered the port again, so it cannot pass merely because buffermgrd
        has not got to the port yet.
        """
        orig_cable_len = None
        extra_pg_key = "{}|6".format(self.INTF)
        rendered = "Mapping {}|3-4 already present".format(self.INTF)

        try:
            speed = self.config_db.get_entry("PORT", self.INTF)["speed"]

            # a 0m cable means no profile is generated at all, so give the port a
            # real length for the duration of the test
            orig_cable_len = self.config_db.get_entry("CABLE_LENGTH", "AZURE")[self.INTF]
            cable_len = "300m" if orig_cable_len == "0m" else orig_cable_len
            if cable_len != orig_cable_len:
                self.change_cable_len(cable_len)

            dvs.port_admin_set(self.INTF, "up")
            lossless_profile = "pg_lossless_{}_{}_profile".format(speed, cable_len)
            self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", lossless_profile)

            # a pg outside the range pfc_enable asks for, on a generated profile
            self.config_db.update_entry("BUFFER_PG", extra_pg_key, {"profile": lossless_profile})
            self.app_db.wait_for_entry("BUFFER_PG_TABLE", "{}:6".format(self.INTF))

            marker = dvs.add_log_marker()
            exitcode, output = dvs.runcmd("supervisorctl restart buffermgrd")
            assert exitcode == 0, "buffermgrd restart failed: {}".format(output)
            self.wait_for_syslog(dvs, marker, rendered)

            # start up only renders what is configured, so the entry is still there
            fvs = self.config_db.get_entry("BUFFER_PG", extra_pg_key)
            assert fvs.get("profile") == lossless_profile, \
                "BUFFER_PG {} was reclaimed while buffermgrd was starting up".format(extra_pg_key)

            # nor does a port event, which cannot change the set of ranges
            marker = dvs.add_log_marker()
            dvs.port_admin_set(self.INTF, "down")
            self.wait_for_syslog(dvs, marker, rendered)
            marker = dvs.add_log_marker()
            dvs.port_admin_set(self.INTF, "up")
            self.wait_for_syslog(dvs, marker, rendered)
            fvs = self.config_db.get_entry("BUFFER_PG", extra_pg_key)
            assert fvs.get("profile") == lossless_profile, \
                "BUFFER_PG {} was reclaimed on a port event".format(extra_pg_key)

            # a pfc_enable change removes the range the previous value generated
            old_pg_key = "{}|3-4".format(self.INTF)
            marker = dvs.add_log_marker()
            self.set_port_qos_table(self.INTF, '3,4,5')
            self.wait_for_syslog(dvs, marker, "Removing lossless PG {} of port".format(old_pg_key))
            assert not self.config_db.get_entry("BUFFER_PG", old_pg_key), \
                "BUFFER_PG {} survived a pfc_enable change".format(old_pg_key)
            fvs = self.config_db.get_entry("BUFFER_PG", "{}|3-5".format(self.INTF))
            assert fvs.get("profile") == lossless_profile, \
                "BUFFER_PG for the new pfc_enable was not generated"

            # and nothing it did not generate
            fvs = self.config_db.get_entry("BUFFER_PG", extra_pg_key)
            assert fvs.get("profile") == lossless_profile, \
                "BUFFER_PG {} was removed on a pfc_enable change".format(extra_pg_key)

        finally:
            self.config_db.delete_entry("BUFFER_PG", extra_pg_key)
            self.set_port_qos_table(self.INTF, '3,4')
            if orig_cable_len:
                self.change_cable_len(orig_cable_len)
            dvs.port_admin_set(self.INTF, "down")

    def test_superseded_pg_removed_with_pfc_enable(self, dvs: DockerVirtualSwitch, setup_teardown_test):
        """
        A pfc_enable change made while the port is not rendered, here because the cable
        length is 0m, leaves the previous range in place. Removing pfc_enable afterwards
        must remove that range too, not only the one for the value never rendered.

        rsyslog holds back a line identical to the one before it, so the change is
        confirmed by buffermgrd's INFO line for it, which carries the new value.
        """
        orig_cable_len = None
        orig_qos_map = None
        old_pg_key = "{}|3-4".format(self.INTF)
        not_rendered = "Not creating/updating PG profile for port {}. Cable length is set to 0m".format(self.INTF)

        try:
            dvs.runcmd("swssloglevel -l INFO -c buffermgrd")
            speed = self.config_db.get_entry("PORT", self.INTF)["speed"]
            orig_cable_len = self.config_db.get_entry("CABLE_LENGTH", "AZURE")[self.INTF]
            orig_qos_map = self.config_db.get_entry("PORT_QOS_MAP", self.INTF)

            self.change_cable_len("300m")
            dvs.port_admin_set(self.INTF, "up")
            lossless_profile = "pg_lossless_{}_300m_profile".format(speed)
            self.config_db.wait_for_field_match("BUFFER_PG", old_pg_key, {"profile": lossless_profile})

            marker = dvs.add_log_marker()
            self.change_cable_len("0m")
            self.wait_for_syslog(dvs, marker, not_rendered)

            marker = dvs.add_log_marker()
            self.set_port_qos_table(self.INTF, '3,4,5')
            self.wait_for_syslog(dvs, marker, "Got pfc enable status for port {} status 3,4,5".format(self.INTF))
            fvs = self.config_db.get_entry("BUFFER_PG", old_pg_key)
            assert fvs.get("profile") == lossless_profile, \
                "BUFFER_PG {} was removed while the port was not rendered".format(old_pg_key)

            marker = dvs.add_log_marker()
            self.config_db.delete_field("PORT_QOS_MAP", self.INTF, "pfc_enable")
            self.wait_for_syslog(dvs, marker, "Removing lossless PG {} of port".format(old_pg_key))
            assert not self.config_db.get_entry("BUFFER_PG", old_pg_key), \
                "BUFFER_PG {} survived the removal of pfc_enable".format(old_pg_key)

        finally:
            if orig_qos_map:
                self.config_db.update_entry("PORT_QOS_MAP", self.INTF, orig_qos_map)
                self.lossless_pgs = orig_qos_map["pfc_enable"].split(',')
            if orig_cable_len:
                self.change_cable_len(orig_cable_len)
            dvs.port_admin_set(self.INTF, "down")
            dvs.runcmd("swssloglevel -l NOTICE -c buffermgrd")

    def test_replaced_profile_removed_when_unused(self, dvs: DockerVirtualSwitch, setup_teardown_test):
        """
        A cable length change moves the port's lossless PGs to a new generated profile.
        Changing it back must remove that profile from CONFIG_DB, APPL_DB and ASIC_DB
        once no BUFFER_PG references it, and keep it while another port still does.
        """
        extra_port = "Ethernet4"
        orig_cable_lens = None
        orig_extra_qos_map = None
        kept = "Keeping lossless profile"

        try:
            dvs.runcmd("swssloglevel -l INFO -c buffermgrd")
            speed = self.config_db.get_entry("PORT", self.INTF)["speed"]
            assert self.config_db.get_entry("PORT", extra_port)["speed"] == speed
            orig_cable_lens = self.config_db.get_entry("CABLE_LENGTH", "AZURE")
            orig_extra_qos_map = self.config_db.get_entry("PORT_QOS_MAP", extra_port)

            test_cable_len = self.unused_cable_len(speed, orig_cable_lens)
            test_profile = "pg_lossless_{}_{}_profile".format(speed, test_cable_len)

            # a real length for both ports, so their PGs reference a generated profile
            base_cable_len = orig_cable_lens[self.INTF]
            if base_cable_len == "0m":
                base_cable_len = next(c for c in ("5m", "40m", "300m") if c != test_cable_len)
            base_profile = "pg_lossless_{}_{}_profile".format(speed, base_cable_len)
            self.set_port_qos_table(extra_port, '3,4')
            self.change_cable_len(base_cable_len, extra_port)
            dvs.port_admin_set(self.INTF, "up")
            dvs.port_admin_set(extra_port, "up")
            for port in (self.INTF, extra_port):
                self.config_db.wait_for_field_match("BUFFER_PG", "{}|3-4".format(port), {"profile": base_profile})
            self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", base_profile)
            self.orig_profiles = self.get_asic_buf_profile()

            # both ports move to the test length
            self.change_cable_len(test_cable_len, extra_port)
            for port in (self.INTF, extra_port):
                self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "{}:3-4".format(port), {"profile": test_profile})
            self.check_new_profile_in_asic_db(test_profile)

            # Ethernet0 moves back; Ethernet4 still references the test profile
            marker = dvs.add_log_marker()
            self.change_cable_len(base_cable_len)
            self.wait_for_syslog(dvs, marker, "{} {}, BUFFER_PG {}|3-4 still".format(kept, test_profile, extra_port))
            assert self.config_db.get_entry("BUFFER_PROFILE", test_profile), \
                "{} was removed while {} still references it".format(test_profile, extra_port)
            assert self.app_db.get_entry("BUFFER_PROFILE_TABLE", test_profile), \
                "{} was removed from APPL_DB while {} still references it".format(test_profile, extra_port)

            # Ethernet4 moves back too, so nothing references the test profile
            self.change_cable_len(base_cable_len, extra_port)
            self.config_db.wait_for_deleted_entry("BUFFER_PROFILE", test_profile)
            self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", test_profile)
            self.asic_db.wait_for_deleted_keys("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE", [self.new_profile])
            # the base profile may have been removed and created again while no port
            # used it, so compare the count rather than the OIDs
            self.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE", len(self.orig_profiles))
            assert self.config_db.get_entry("BUFFER_PROFILE", base_profile)

        finally:
            if orig_cable_lens:
                self.config_db.update_entry("CABLE_LENGTH", "AZURE", orig_cable_lens)
            if orig_extra_qos_map is not None:
                if "pfc_enable" in orig_extra_qos_map:
                    self.config_db.update_entry("PORT_QOS_MAP", extra_port, orig_extra_qos_map)
                else:
                    self.config_db.delete_field("PORT_QOS_MAP", extra_port, "pfc_enable")
            dvs.port_admin_set(self.INTF, "down")
            dvs.port_admin_set(extra_port, "down")
            dvs.runcmd("swssloglevel -l NOTICE -c buffermgrd")

    def test_profile_removed_with_pfc_enable(self, dvs: DockerVirtualSwitch, setup_teardown_test):
        """
        Removing pfc_enable removes the port's generated lossless PGs. The profile they
        referenced must go too once no BUFFER_PG references it.
        """
        orig_cable_lens = None
        orig_qos_map = None
        pg_key = "{}|3-4".format(self.INTF)

        try:
            speed = self.config_db.get_entry("PORT", self.INTF)["speed"]
            orig_cable_lens = self.config_db.get_entry("CABLE_LENGTH", "AZURE")
            orig_qos_map = self.config_db.get_entry("PORT_QOS_MAP", self.INTF)
            test_cable_len = self.unused_cable_len(speed, orig_cable_lens)
            test_profile = "pg_lossless_{}_{}_profile".format(speed, test_cable_len)

            dvs.port_admin_set(self.INTF, "up")
            self.orig_profiles = self.get_asic_buf_profile()
            self.change_cable_len(test_cable_len)
            self.config_db.wait_for_field_match("BUFFER_PG", pg_key, {"profile": test_profile})
            self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", test_profile)
            self.check_new_profile_in_asic_db(test_profile)

            self.config_db.delete_field("PORT_QOS_MAP", self.INTF, "pfc_enable")
            self.config_db.wait_for_deleted_entry("BUFFER_PG", pg_key)
            self.config_db.wait_for_deleted_entry("BUFFER_PROFILE", test_profile)
            self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", test_profile)
            self.asic_db.wait_for_deleted_keys("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE", [self.new_profile])

        finally:
            # cable length first, so restoring pfc_enable renders the original profile only
            if orig_cable_lens:
                self.config_db.update_entry("CABLE_LENGTH", "AZURE", orig_cable_lens)
            if orig_qos_map:
                self.config_db.update_entry("PORT_QOS_MAP", self.INTF, orig_qos_map)
                self.lossless_pgs = orig_qos_map["pfc_enable"].split(',')
            dvs.port_admin_set(self.INTF, "down")

    def test_superseded_range_profile_removed(self, dvs: DockerVirtualSwitch, setup_teardown_test):
        """
        A pfc_enable change made while the port is not rendered (0m cable) leaves the
        previous range, on the previous length's profile, to be removed by the next
        render. When that render is for another length, the previous profile is left
        with no BUFFER_PG and must be removed too.
        """
        orig_cable_lens = None
        old_pg_key = "{}|3-4".format(self.INTF)
        new_pg_key = "{}|3-5".format(self.INTF)
        not_rendered = "Not creating/updating PG profile for port {}. Cable length is set to 0m".format(self.INTF)

        try:
            dvs.runcmd("swssloglevel -l INFO -c buffermgrd")
            speed = self.config_db.get_entry("PORT", self.INTF)["speed"]
            orig_cable_lens = self.config_db.get_entry("CABLE_LENGTH", "AZURE")
            test_cable_len = self.unused_cable_len(speed, orig_cable_lens)
            test_profile = "pg_lossless_{}_{}_profile".format(speed, test_cable_len)
            base_cable_len = orig_cable_lens[self.INTF]
            if base_cable_len == "0m":
                base_cable_len = next(c for c in ("5m", "40m", "300m") if c != test_cable_len)
            base_profile = "pg_lossless_{}_{}_profile".format(speed, base_cable_len)

            dvs.port_admin_set(self.INTF, "up")
            self.orig_profiles = self.get_asic_buf_profile()
            self.change_cable_len(test_cable_len)
            self.config_db.wait_for_field_match("BUFFER_PG", old_pg_key, {"profile": test_profile})
            self.check_new_profile_in_asic_db(test_profile)

            marker = dvs.add_log_marker()
            self.change_cable_len("0m")
            self.wait_for_syslog(dvs, marker, not_rendered)

            marker = dvs.add_log_marker()
            self.set_port_qos_table(self.INTF, '3,4,5')
            self.wait_for_syslog(dvs, marker, "Got pfc enable status for port {} status 3,4,5".format(self.INTF))

            # the next render binds 3-5 to the base profile and removes 3-4 with its profile
            self.change_cable_len(base_cable_len)
            self.config_db.wait_for_field_match("BUFFER_PG", new_pg_key, {"profile": base_profile})
            self.config_db.wait_for_deleted_entry("BUFFER_PG", old_pg_key)
            self.config_db.wait_for_deleted_entry("BUFFER_PROFILE", test_profile)
            self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", test_profile)
            self.asic_db.wait_for_deleted_keys("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE", [self.new_profile])

        finally:
            if orig_cable_lens:
                self.config_db.update_entry("CABLE_LENGTH", "AZURE", orig_cable_lens)
            self.set_port_qos_table(self.INTF, '3,4')
            dvs.port_admin_set(self.INTF, "down")
            dvs.runcmd("swssloglevel -l NOTICE -c buffermgrd")

    def test_replaced_operator_profile_kept(self, dvs: DockerVirtualSwitch, setup_teardown_test):
        """
        A cable length change rebinds a lossless PG even when it points at a profile
        without the generated pg_lossless_ prefix. That profile is the operator's, so
        it must stay although nothing references it any more.

        buffermgrd handles cable length changes in order, so once it logs the removal
        for the second change, it has finished the first.
        """
        operator_profile = "operator_lossless_profile"
        pg_key = "{}|3-4".format(self.INTF)
        orig_cable_lens = None

        try:
            speed = self.config_db.get_entry("PORT", self.INTF)["speed"]
            orig_cable_lens = self.config_db.get_entry("CABLE_LENGTH", "AZURE")
            test_cable_len = self.unused_cable_len(speed, orig_cable_lens)
            test_profile = "pg_lossless_{}_{}_profile".format(speed, test_cable_len)

            base_cable_len = orig_cable_lens[self.INTF]
            if base_cable_len == "0m":
                base_cable_len = next(c for c in ("5m", "40m", "300m") if c != test_cable_len)
                self.change_cable_len(base_cable_len)
            base_profile = "pg_lossless_{}_{}_profile".format(speed, base_cable_len)
            dvs.port_admin_set(self.INTF, "up")
            self.config_db.wait_for_field_match("BUFFER_PG", pg_key, {"profile": base_profile})

            # the operator points the PG at a profile of their own
            fvs = self.config_db.get_entry("BUFFER_PROFILE", base_profile)
            self.config_db.update_entry("BUFFER_PROFILE", operator_profile, fvs)
            self.config_db.update_entry("BUFFER_PG", pg_key, {"profile": operator_profile})
            self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "{}:3-4".format(self.INTF),
                                             {"profile": operator_profile})

            # rebinds the PG to the generated profile for the new length
            self.change_cable_len(test_cable_len)
            self.config_db.wait_for_field_match("BUFFER_PG", pg_key, {"profile": test_profile})

            # moving back removes the generated test profile, which marks the first change done
            marker = dvs.add_log_marker()
            self.change_cable_len(base_cable_len)
            self.wait_for_syslog(dvs, marker, "Removing lossless profile {}, no BUFFER_PG".format(test_profile))

            assert self.config_db.get_entry("BUFFER_PROFILE", operator_profile), \
                "{} was removed when the PG was moved off it".format(operator_profile)
            assert self.app_db.get_entry("BUFFER_PROFILE_TABLE", operator_profile), \
                "{} was removed from APPL_DB when the PG was moved off it".format(operator_profile)

        finally:
            # a profile still referenced is never removed by bufferorch, so unbind it first,
            # before the cable length restore lets buffermgrd rebind the PG as well
            if self.config_db.get_entry("BUFFER_PG", pg_key).get("profile") == operator_profile:
                self.config_db.update_entry("BUFFER_PG", pg_key, {"profile": base_profile})
                self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "{}:3-4".format(self.INTF),
                                                 {"profile": base_profile})
            self.config_db.delete_entry("BUFFER_PROFILE", operator_profile)
            if orig_cable_lens:
                self.config_db.update_entry("CABLE_LENGTH", "AZURE", orig_cable_lens)
            dvs.port_admin_set(self.INTF, "down")

    def test_buffer_pg_update(self, dvs, setup_teardown_test):
        orig_cable_len = None
        orig_speed = None
        test_speed = None
        extra_port = "Ethernet4"
        try:
            # Retrieve cable len
            fvs_cable_len = self.config_db.get_entry("CABLE_LENGTH", "AZURE")
            orig_cable_len = fvs_cable_len[self.INTF]
            if orig_cable_len == "0m":
                cable_len_for_test = "300m"
                fvs_cable_len[self.INTF] = cable_len_for_test
                fvs_cable_len[extra_port] = cable_len_for_test
                
                self.config_db.update_entry("CABLE_LENGTH", "AZURE", fvs_cable_len)
            else:
                cable_len_for_test = orig_cable_len
            # Ethernet4 is set to up, while no 'pfc_enable' available. `Ethernet0` is not supposed to be impacted
            dvs.port_admin_set(extra_port, "up")

            dvs.port_admin_set(self.INTF, "up")
            
             # Retrieve port speed
            fvs_port = self.config_db.get_entry("PORT", self.INTF)
            orig_speed = fvs_port["speed"]

            # Make sure the buffer PG has been created
            orig_lossless_profile = "pg_lossless_{}_{}_profile".format(orig_speed, cable_len_for_test)
            self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", orig_lossless_profile)
            self.pg_name_map = self.get_pg_name_map()
            self.orig_profiles = self.get_asic_buf_profile()

            # get the orig buf profiles attached to the pgs
            self.get_asic_buf_pg_profiles()

            # Update port speed
            if orig_speed == "100000":
                test_speed = "40000"
            elif orig_speed == "40000":
                test_speed = "100000"
            # change intf speed to 'test_speed'
            dvs.port_field_set(self.INTF, "speed", test_speed)
            dvs.port_field_set(extra_port, "speed", test_speed)
            # Verify new profile is generated
            new_lossless_profile = "pg_lossless_{}_{}_profile".format(test_speed, cable_len_for_test)
            self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", new_lossless_profile)

            # Verify BUFFER_PG is updated
            for pg in self.lossless_pg_combinations:
                self.app_db.wait_for_field_match("BUFFER_PG_TABLE", self.INTF + ":" + pg, {"profile": new_lossless_profile})

            fvs_negative = {}
            for pg in self.pg_name_map:
                # verify that buffer pgs do not point to the old profile since we cannot deduce the new profile oid
                fvs_negative["SAI_INGRESS_PRIORITY_GROUP_ATTR_BUFFER_PROFILE"] = self.buf_pg_profile[pg]
                self.asic_db.wait_for_field_negative_match("ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP", self.pg_name_map[pg], fvs_negative)

             # Add pfc_enable field for extra port
            self.set_port_qos_table(extra_port, '2,3,4,6')
            self.lossless_pg_combinations = ['2-4', '6']
            time.sleep(1)
            # Verify BUFFER_PG is updated when pfc_enable is available
            for pg in self.lossless_pg_combinations:
                self.app_db.wait_for_field_match("BUFFER_PG_TABLE", extra_port + ":" + pg, {"profile": new_lossless_profile})
        finally:
            if orig_cable_len:
                self.change_cable_len(orig_cable_len, extra_port)
            if orig_speed:
                dvs.port_field_set(self.INTF, "speed", orig_speed)
                dvs.port_field_set(extra_port, "speed", orig_speed)
            dvs.port_admin_set(self.INTF, "down")
            dvs.port_admin_set(extra_port, "down")
            
    def test_no_pg_profile_for_speed_and_length(self, dvs: DockerVirtualSwitch, setup_teardown_test):
        """
        Test to verify that buffermgrd correctly handles a scenario where no PG profile
        is configured for a given speed (10000) and cable length (80m) for Ethernet0 (self.INTF).
        """
        orig_cable_len = None
        orig_port_speed = None
        orig_port_status = None
        orig_port_qos_map = None
        
        test_cable_len = "80m" # cable length must not exist for test_speed in 
        test_speed = "10000"
        test_port_status ="down" # can be up or down, but it must exist in port configuration
        test_port_pfc_enable = "3,4" # does not matter, but must exist
        
        try:
            ##################################
            ## Save original configurations ##
            ##################################
            
            # Save original cable length
            fvs_cable_len = self.config_db.get_entry("CABLE_LENGTH", "AZURE")
            orig_cable_len = fvs_cable_len.get(self.INTF) if fvs_cable_len else None
            
            # Save original port speed and admin status
            fvs_port = self.config_db.get_entry("PORT", self.INTF)
            orig_port_speed = fvs_port.get("speed") if fvs_port else None
            orig_port_status = fvs_port.get("admin_status") if fvs_port else None

            # Save original port qos map
            fvs_qos_map = self.config_db.get_entry("PORT_QOS_MAP", self.INTF)
            orig_cable_len = fvs_qos_map.get("pfc_enable") if fvs_qos_map else None
            
            ######################################
            ## Send configurations to CONFIG_DB ##
            ######################################
            
            # Configure cable length 
            self.change_cable_len(test_cable_len)

            # Configure port speed
            dvs.port_field_set(self.INTF, "speed", test_speed)

            # Configure PFC enable
            self.set_port_qos_table(self.INTF, test_port_pfc_enable)
            
            # Add marker to log to make syslog verification easier
            # Set before setting admin status to not miss syslog
            marker = dvs.add_log_marker()

            # Configure admin status
            dvs.port_admin_set(self.INTF, test_port_status)
            
            # Wait for buffermgrd to process the changes
            time.sleep(2)
            
            ##################
            ## Verification ##
            ##################
            
            
            # Check syslog if this error is present. This is expected.
            self.check_syslog(dvs, marker, "Failed to process invalid entry, drop it")
        
        finally:
            ###############################
            ## Revert to original values ##
            ###############################
            
            # Revert values to original values
            # If there are none, then assume entry/field never existed and should be deleted
            
            # Revert cable length
            if orig_cable_len:
                self.change_cable_len(orig_cable_len)
            else:
                self.config_db.delete_entry("CABLE_LENGTH", "AZURE")
               
            # Revert port speed 
            if orig_port_speed:
                dvs.port_field_set(self.INTF, "speed", orig_port_speed)
            else:
                self.config_db.delete_field("PORT", self.INTF, "speed")
            
            # Revert admin status
            if orig_port_status:
                dvs.port_admin_set(self.INTF, orig_port_status)
            else:
                self.config_db.delete_field("PORT", self.INTF, "admin_status")
            
            # Revert port qos map
            if orig_port_qos_map:
                self.config_db.update_entry("PORT_QOS_MAP", self.INTF, orig_port_qos_map)
            else:
                self.config_db.delete_entry("PORT_QOS_MAP", self.INTF)

    def test_config_db_buffer_pg_update(self, dvs: DockerVirtualSwitch, setup_teardown_test):
        """
        Test to verify BUFFER_PG table entry creation and its consistency when admin_status is updated.
        """

        orig_port_qos_map = None
        orig_cable_len = None
        orig_fvs_port = None
        
        # Test parameters
        test_cable_len = "300m"
        test_speed = "100000"
        test_port_pfc_enable = "3,4"

        try:
            ##################################
            ## Save original configurations ##
            ##################################

            # Save original cable length
            fvs_cable_len = self.config_db.get_entry("CABLE_LENGTH", "AZURE")
            orig_cable_len = fvs_cable_len.get(self.INTF) if fvs_cable_len else None

            # Save original port speed and admin status
            orig_fvs_port = self.config_db.get_entry("PORT", self.INTF)

            # Save original port qos map
            fvs_qos_map = self.config_db.get_entry("PORT_QOS_MAP", self.INTF)
            orig_port_qos_map = fvs_qos_map if fvs_qos_map else None

            ######################################
            ## Send configurations to CONFIG_DB ##
            ######################################

            # Configure cable length
            self.change_cable_len(test_cable_len)

            if orig_fvs_port:
                self.config_db.delete_entry("PORT", self.INTF)

            fvs_port = orig_fvs_port.copy()

            # Delete admin_status field if it exists
            fvs_port.pop("admin_status", None)

            fvs_port["speed"] = test_speed

            self.config_db.update_entry("PORT", self.INTF, fvs_port)

            # Configure PFC enable
            self.set_port_qos_table(self.INTF, test_port_pfc_enable)

            # Wait for buffermgrd to process the changes
            time.sleep(2)

            ##################
            ## Verification ##
            ##################

            # Verify BUFFER_PG table entry in CONFIG_DB without admin_status field
            expected_profile = "pg_lossless_{}_{}_profile".format(test_speed, test_cable_len)
            pg_field_key = "{}|{}".format(self.INTF, test_port_pfc_enable.replace(',', '-'))
            self.config_db.wait_for_field_match("BUFFER_PG", "{}".format(pg_field_key),
                                                {"profile": expected_profile})

            # Set admin_status to up
            dvs.port_admin_set(self.INTF, "up")

            fvs_buffer_pg = self.config_db.get_entry("BUFFER_PG", pg_field_key)
            assert fvs_buffer_pg.get("profile") == expected_profile, \
                "BUFFER_PG entry for {} does not match expected profile {}".format(pg_field_key, expected_profile)

        finally:
            ###############################
            ## Revert to original values ##
            ###############################

            # Revert cable length
            if orig_cable_len:
                self.change_cable_len(orig_cable_len)
            else:
                self.config_db.delete_entry("CABLE_LENGTH", "AZURE")

            # Revert to original PORT configuration
            if orig_fvs_port:
                self.config_db.update_entry("PORT", self.INTF, orig_fvs_port)
            else:
                self.config_db.delete_entry("PORT", self.INTF)

            # Revert port qos map
            if orig_port_qos_map:
                self.config_db.update_entry("PORT_QOS_MAP", self.INTF, orig_port_qos_map)
            else:
                self.config_db.delete_entry("PORT_QOS_MAP", self.INTF)

    def test_update_buffer_pg_for_cable_len_change(self, dvs: DockerVirtualSwitch, setup_teardown_test):
        """
        Test to verify that buffermgrd correctly updated the buffer_pg entry of a given port when cable length is
        updated.
        """

        orig_port_qos_map = None
        orig_cable_len = None
        orig_fvs_port = None

        # Test parameters
        test_cable_len = "300m"
        test_speed = "100000"
        test_port_pfc_enable = "3,4"

        try:
            ##################################
            ## Save original configurations ##
            ##################################

            # Save original cable length
            fvs_cable_len = self.config_db.get_entry("CABLE_LENGTH", "AZURE")
            orig_cable_len = fvs_cable_len.get(self.INTF) if fvs_cable_len else None

            # Save original port speed and admin status
            orig_fvs_port = self.config_db.get_entry("PORT", self.INTF)

            # Save original port qos map
            fvs_qos_map = self.config_db.get_entry("PORT_QOS_MAP", self.INTF)
            orig_port_qos_map = fvs_qos_map if fvs_qos_map else None

            ######################################
            ## Send configurations to CONFIG_DB ##
            ######################################

            # Configure PFC enable
            self.set_port_qos_table(self.INTF, test_port_pfc_enable)

            # Configure cable length
            self.change_cable_len(test_cable_len)

            # Wait for buffermgrd to process the changes
            time.sleep(2)

            ##################
            ## Verification ##
            ##################

            # Verify BUFFER_PG table entry in CONFIG_DB without port qos map entry
            pg_field_key = "{}|{}".format(self.INTF, test_port_pfc_enable.replace(',', '-'))
            fvs_buffer_pg = self.config_db.get_entry("BUFFER_PG", pg_field_key)

            # Check if fvs_buffer_pg profile is not equal to expected dynamic profile
            expected_profile = "pg_lossless_{}_{}_profile".format(test_speed, test_cable_len)
            if fvs_buffer_pg.get("profile") != expected_profile:
                assert False, "BUFFER_PG profile {} is not {}".format(fvs_buffer_pg.get("profile"), expected_profile)

        finally:
            ###############################
            ## Revert to original values ##
            ###############################

            # Revert cable length
            if orig_cable_len:
                self.change_cable_len(orig_cable_len)
            else:
                self.config_db.delete_entry("CABLE_LENGTH", "AZURE")

            # Revert to original PORT configuration
            if orig_fvs_port:
                self.config_db.update_entry("PORT", self.INTF, orig_fvs_port)
            else:
                self.config_db.delete_entry("PORT", self.INTF)

            # Revert port qos map
            if orig_port_qos_map:
                self.config_db.update_entry("PORT_QOS_MAP", self.INTF, orig_port_qos_map)
            else:
                self.config_db.delete_entry("PORT_QOS_MAP", self.INTF)

    def test_no_buffer_pg_for_no_port_qos_map(self, dvs: DockerVirtualSwitch, setup_teardown_test):
        """
        Test to verify that buffermgrd correctly handles the scenario where no PG profile
        is assigned to a port when no port QoS map entry is configured.
        """

        orig_port_qos_map = None
        orig_cable_len = None
        orig_fvs_port = None

        # Test parameters
        test_cable_len = "300m"
        test_speed = "100000"
        test_port_pfc_enable = "3,4"

        try:
            ##################################
            ## Save original configurations ##
            ##################################

            # Save original cable length
            fvs_cable_len = self.config_db.get_entry("CABLE_LENGTH", "AZURE")
            orig_cable_len = fvs_cable_len.get(self.INTF) if fvs_cable_len else None

            # Save original port speed and admin status
            orig_fvs_port = self.config_db.get_entry("PORT", self.INTF)

            # Save original port qos map
            fvs_qos_map = self.config_db.get_entry("PORT_QOS_MAP", self.INTF)
            orig_port_qos_map = fvs_qos_map if fvs_qos_map else None

            ######################################
            ## Send configurations to CONFIG_DB ##
            ######################################

            # Configure cable length
            self.change_cable_len(test_cable_len)

            # Configure PFC enable
            self.set_port_qos_table(self.INTF, test_port_pfc_enable)

            # Set admin_status to up
            dvs.port_admin_set(self.INTF, "up")

            # Delete BUFFER_PG entry for lossless pg
            pg_field_key = "{}|{}".format(self.INTF, test_port_pfc_enable.replace(',', '-'))
            self.config_db.delete_entry("BUFFER_PG", pg_field_key)

            # Delete PORT_QOS_MAP
            self.config_db.delete_entry("PORT_QOS_MAP", self.INTF)

            if orig_fvs_port:
                self.config_db.delete_entry("PORT", self.INTF)

            assert orig_fvs_port is not None, "Expected PORT entry to exist for {}".format(self.INTF)
            fvs_port = orig_fvs_port.copy()

            fvs_port["speed"] = test_speed

            self.config_db.update_entry("PORT", self.INTF, fvs_port)

            # Set admin_status to up
            dvs.port_admin_set(self.INTF, "up")

            # Wait for buffermgrd to process the changes
            time.sleep(2)

            ##################
            ## Verification ##
            ##################

            # Verify BUFFER_PG table entry in CONFIG_DB without port qos map entry
            pg_field_key = "{}|{}".format(self.INTF, test_port_pfc_enable.replace(',', '-'))

            fvs_buffer_pg = self.config_db.get_entry("BUFFER_PG", pg_field_key)
            assert not fvs_buffer_pg, \
                "Unexpected BUFFER_PG entry found for {}".format(fvs_buffer_pg)

        finally:
            ###############################
            ## Revert to original values ##
            ###############################

            # Revert cable length
            if orig_cable_len:
                self.change_cable_len(orig_cable_len)
            else:
                self.config_db.delete_entry("CABLE_LENGTH", "AZURE")

            # Revert to original PORT configuration
            if orig_fvs_port:
                self.config_db.update_entry("PORT", self.INTF, orig_fvs_port)
            else:
                self.config_db.delete_entry("PORT", self.INTF)

            # Revert port qos map
            if orig_port_qos_map:
                self.config_db.update_entry("PORT_QOS_MAP", self.INTF, orig_port_qos_map)
            else:
                self.config_db.delete_entry("PORT_QOS_MAP", self.INTF)

