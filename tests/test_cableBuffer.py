"""
    test_Cablebuffer.py implements list of tests to check  whether the approprotae buffer profile
    is selected for an interface depending upon cable and Speed values .

    These tests need to be run in prepared environment and with the
    SONiC version compiled for PLATFORM=vs

    See README.md for details
    For Running this script , we are using   pg_profile_lookup.ini under ../tests directory.
    In future  if there is any change in buffer profiles, pg_profile_lookup.ini has to be updated in ../tests directory.
"""

import os
import re
import time
import docker
import pytest
import commands
from swsscommon import swsscommon
import pytest


class TestCableBufferProfile(object):
    cable_list = ['5m', '40m', '300m']
    speed_list = ['50000' , '25000', '40000', '10000', '100000']
    num_ports = 32

    def getProfileDetails(self):
        profile = open("pg_profile_lookup.ini", 'r')
        profile_details = {}
        for cable in self.cable_list:
            profile_details[cable] = {}
            for speed in self.speed_list:
                profile_details[cable][speed] = {}
        # get the profile details for cable and speed combination
        for line in profile:
            match = re.search("(\d+)\s+(\w+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)", line)
            if match:
                length = match.group(2)
                profile_details[length][match.group(1)]['size'] = match.group(3)
                profile_details[length][match.group(1)]['xon'] = match.group(4)
                profile_details[length][match.group(1)]['xoff'] = match.group(5)
                profile_details[length][match.group(1)]['threshold'] = match.group(6)
        return profile_details

    def test_CableBufferProfile(self, dvs):
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        cfg_port_table = swsscommon.Table(cdb, "PORT")
        cfg_cable_length_table = swsscommon.Table(cdb, "CABLE_LENGTH")
        cfg_buffer_profile_table = swsscommon.Table(cdb, "BUFFER_PROFILE")
        cfg_buffer_pg_table = swsscommon.Table(cdb, "BUFFER_PG")
        asic_port_table = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        asic_profile_table = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE")

        profile_details = self.getProfileDetails()
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
        assert len(asic_profile_table.getKeys()) == expected_buffer_profiles_num

        for cable in self.cable_list:
            for i in range(0, self.num_ports):
                fvc = swsscommon.FieldValuePairs([("Ethernet%d" % (i*4), cable)])
                cfg_cable_length_table.set("AZURE", fvc)
            interface_cable_records = cfg_cable_length_table.getKeys()
            (status, fvs) = cfg_cable_length_table.get('AZURE')
            assert status == True
            # check cable length is set for the interfaces
            for interface in fvs:
                assert interface[1] == cable

            for speed in self.speed_list:
                fvs = swsscommon.FieldValuePairs([("speed", speed)])
                # set same speed on all ports
                for i in range(0, self.num_ports):
                    cfg_port_table.set("Ethernet%d" % (i*4), fvs)
                time.sleep(1)

                # check the speed was set
                asic_port_records = asic_port_table.getKeys()
                assert len(asic_port_records) == (self.num_ports + 1)  # +CPU port
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
                expected_new_profile_name = "pg_lossless_%s_%s_profile" % (speed, cable)
                assert current_buffer_profiles.index(expected_new_profile_name) > -1

                # check the profile is mapped with correct values from lookuptable
                (status, fvs) = cfg_buffer_profile_table.get(expected_new_profile_name)
                for fv in fvs:
                    if fv[0] == "xon":
                        assert fv[1] == profile_details[cable][speed]['xon']
                    elif fv[0] == "xoff":
                        assert fv[1] == profile_details[cable][speed]['xoff']
                    elif fv[0] == "size":
                        assert fv[1] == profile_details[cable][speed]['size']
                    elif fv[0] == "dynamic_th":
                        assert fv[1] == profile_details[cable][speed]['threshold']

                # check correct profile is set for all ports
                pg_tables = cfg_buffer_pg_table.getKeys()
                for i in range(0, self.num_ports):
                    expected_pg_table = "Ethernet%d|3-4" % (i*4)
                    assert pg_tables.index(expected_pg_table) > -1
                    (status, fvs) = cfg_buffer_pg_table.get(expected_pg_table)
                    for fv in fvs:
                        if fv[0] == "profile":
                            assert fv[1] == "[BUFFER_PROFILE|%s]" % expected_new_profile_name

