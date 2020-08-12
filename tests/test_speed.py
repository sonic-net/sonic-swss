"""test_speed.py verifies that speed is set on interfaces and buffer manager behavies correctly on speed change."""


class TestSpeedSet:
    # FIXME: This value should probably not be hardcoded since the persistent container can
    # specify a dynamic number of ports now.
    NUM_PORTS = 32

    def test_SpeedAndBufferSet(self, dvs, testlog):
        configured_speed_list = []
        speed_list = ["10000", "25000", "40000", "50000", "100000"]

        # buffers_config.j2 is used for the test and defines 3 static profiles and 1 dynamic profile:
        #    "ingress_lossy_profile"
        #    "egress_lossless_profile"
        #    "egress_lossy_profile"
        #    "pg_lossless_40000_300m_profile"
        num_buffer_profiles = 4

        cdb = dvs.get_config_db()
        adb = dvs.get_asic_db()

        # Get speed from the first port we hit in ASIC DB port walk, and
        # assume that its the initial configured speed for all ports, and
        # as new port configuration file i.e. 'platform.json' guarantees
        # 100G as initial port speed for all ports and the dynamic buffer
        # profile has already been created for it.
        asic_port_records = adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        for k in asic_port_records:
            fvs = adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_PORT", k)
            if "SAI_PORT_ATTR_SPEED" in fvs.keys():
                configured_speed_list.append(fvs["SAI_PORT_ATTR_SPEED"])
                break

            if configured_speed_list:
                break

        # Check if the buffer profiles make it to Config DB
        cdb.wait_for_n_keys("BUFFER_PROFILE", num_buffer_profiles)

        # Check if they make it to the ASIC
        adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE", num_buffer_profiles)

        for speed in speed_list:
            # Set same speed on all ports
            for i in range(0, self.NUM_PORTS):
                cdb.update_entry("PORT", "Ethernet{}".format(i * 4), {"speed": speed})

            # Check that the speed was set for all "NUM_PORTS" ports
            asic_port_records = adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT", self.NUM_PORTS + 1)  # +1 CPU Port
            for port in [port for port in asic_port_records if port in adb.port_name_map.values()]:
                adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port, {"SAI_PORT_ATTR_SPEED": speed})

            # Check number of created profiles
            if speed not in configured_speed_list:
                num_buffer_profiles += 1  # New speed should add new PG profile
                configured_speed_list.append(speed)

            current_buffer_profiles = cdb.wait_for_n_keys("BUFFER_PROFILE", num_buffer_profiles)
            # Make sure the same number of profiles are created on the ASIC
            adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE", num_buffer_profiles)

            # Check new profile name
            expected_new_profile_name = "pg_lossless_{}_300m_profile".format(speed)
            assert expected_new_profile_name in current_buffer_profiles

            # Check correct profile is set for all ports
            pg_tables = cdb.get_keys("BUFFER_PG")
            for i in range(0, self.NUM_PORTS):
                expected_pg_table = "Ethernet{}|3-4".format(i * 4)
                assert expected_pg_table in pg_tables

                expected_fields = {"profile": "[BUFFER_PROFILE|{}]".format(expected_new_profile_name)}
                cdb.wait_for_field_match("BUFFER_PG", expected_pg_table, expected_fields)
