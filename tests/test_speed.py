"""
    test_speed.py implements list of tests to check speed set on
    interfaces and correct buffer manager behavior on speed change

    These tests need to be run in prepared environment and with the
    SONiC version compiled for PLATFORM=vs

    See README.md for details
"""
from swsscommon import swsscommon
import time
import re
import json
import os

class TestSpeedSet(object):
    num_ports = 32
    def test_SpeedAndBufferSet(self, dvs):
        speed_list = ['50000', '25000', '40000', '10000', '100000']

        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        cfg_port_table = swsscommon.Table(cdb, "PORT")
        cfg_buffer_profile_table = swsscommon.Table(cdb, "BUFFER_PROFILE")
        cfg_buffer_pg_table = swsscommon.Table(cdb, "BUFFER_PG")
        asic_port_table = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        asic_profile_table = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE")

        buffer_profiles = cfg_buffer_profile_table.getKeys()
        expected_buffer_profiles_num = len(buffer_profiles)
        # buffers.json used for the test defines 7 static profiles:
        #    "ingress_lossless_profile"
        #    "ingress_lossy_profile"
        #    "egress_lossless_profile"
        #    "egress_lossy_profile"
        #    "pg_lossy_profile"
        #    "q_lossless_profile"
        #    "q_lossy_profile"
        # check if they get the DB
        assert expected_buffer_profiles_num == 7
        # and if they were successfully created on ASIC
        assert len(asic_profile_table.getKeys()) == expected_buffer_profiles_num

        for speed in speed_list:
            fvs = swsscommon.FieldValuePairs([("speed", speed)])
            # set same speed on all ports
            for i in range(0, self.num_ports):
                cfg_port_table.set("Ethernet%d" % (i*4), fvs)

            time.sleep(1) # let configuration settle down

            # check the speed was set
            asic_port_records = asic_port_table.getKeys()
            assert len(asic_port_records) == (self.num_ports + 1) # +CPU port
            num_set = 0
            for k in asic_port_records:
                (status, fvs) = asic_port_table.get(k)
                assert status == True
                for fv in fvs:
                    if fv[0] == "SAI_PORT_ATTR_SPEED":
                        assert fv[1] == speed
                        num_set += 1
            # make sure speed is set for all "num_ports" ports
            assert num_set == self.num_ports

            # check number of created profiles
            expected_buffer_profiles_num += 1  # new speed should add new PG profile
            current_buffer_profiles = cfg_buffer_profile_table.getKeys()
            assert len(current_buffer_profiles) == expected_buffer_profiles_num
            # make sure the same number of profiles are created on ASIC
            assert len(asic_profile_table.getKeys()) == expected_buffer_profiles_num

            # check new profile name
            expected_new_profile_name = "pg_lossless_%s_300m_profile" % speed
            assert current_buffer_profiles.index(expected_new_profile_name) > -1

            # check correct profile is set for all ports
            pg_tables = cfg_buffer_pg_table.getKeys()
            for i in range(0, self.num_ports):
                expected_pg_table = "Ethernet%d|3-4" % (i*4)
                assert pg_tables.index(expected_pg_table) > -1
                (status, fvs) = cfg_buffer_pg_table.get(expected_pg_table)
                for fv in fvs:
                    if fv[0] == "profile":
                        assert fv[1] == "[BUFFER_PROFILE|%s]" % expected_new_profile_name
