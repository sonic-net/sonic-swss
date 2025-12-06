import pytest
import time


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
    
    def check_syslog(self, dvs, marker, err_log, expected_cnt=1):
        (exitcode, num) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep \"%s\" | wc -l" % (marker, err_log)])
        assert num.strip() >= str(expected_cnt)

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
            orig_port_speed = orig_fvs_port.get("speed") if orig_fvs_port else None
            orig_port_status = orig_fvs_port.get("admin_status") if orig_fvs_port else None

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

