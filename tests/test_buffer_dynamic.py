import time
import json
import redis
import pytest
import re
import buffer_model

from pprint import pprint
from swsscommon import swsscommon
from dvslib.dvs_common import PollingConfig

@pytest.yield_fixture
def dynamic_buffer(dvs):
    buffer_model.enable_dynamic_buffer(dvs.get_config_db(), dvs.runcmd)
    yield
    buffer_model.disable_dynamic_buffer(dvs.get_config_db(), dvs.runcmd)


@pytest.mark.usefixtures("dynamic_buffer")
class TestBufferMgrDyn(object):
    DEFAULT_POLLING_CONFIG = PollingConfig(polling_interval=0.01, timeout=60, strict=True)
    def setup_db(self, dvs):
        self.initialized = False
        self.cableLenTest1 = "15m"
        self.cableLenTest2 = "25m"
        self.speedToTest1 = "50000"
        self.speedToTest2 = "10000"

        self.app_db = dvs.get_app_db()
        self.asic_db = dvs.get_asic_db()
        self.config_db = dvs.get_config_db()
        self.state_db = dvs.get_state_db()

        fvs = self.config_db.wait_for_entry("PORT", "Ethernet0")
        self.originalSpeed = fvs["speed"]
        if self.originalSpeed == self.speedToTest1:
            self.speedToTest1 = "100000"
        elif self.originalSpeed == self.speedToTest2:
            self.speedToTest2 = "100000"
        elif self.originalSpeed == "":
            self.originalSpeed = "100000"

        # Check whether cable length has been configured
        fvs = self.config_db.wait_for_entry("CABLE_LENGTH", "AZURE")
        self.originalCableLen = fvs["Ethernet0"]
        if self.originalCableLen == self.cableLenTest1:
            self.cableLenTest1 = "20m"
        elif self.originalCableLen == self.cableLenTest2:
            self.cableLenTest2 = "20m"

        fvs = {"mmu_size": "12766208"}
        self.state_db.create_entry("BUFFER_MAX_PARAM_TABLE", "global", fvs)

        # The default lossless priority group will be removed ahead of staring test
        # By doing so all the dynamically generated profiles will be removed and
        # it's easy to identify the SAI OID of the new profile once it's created by
        # comparing the new set of ASIC_STATE:BUFFER_PROFILE table against the initial one
        pgs = self.config_db.get_keys('BUFFER_PG')
        for key in pgs:
            pg = self.config_db.get_entry('BUFFER_PG', key)
            if pg['profile'] == 'NULL':
                self.config_db.delete_entry('BUFFER_PG', key)

        # wait 5 seconds to make sure all the lossless PGs and profiles have been removed
        seconds_delayed = 0
        lossless_profile_name_pattern = 'pg_lossless_([1-9][0-9]*000)_([1-9][0-9]*m)_profile'
        max_delay_seconds = 10
        while seconds_delayed <= max_delay_seconds:
            time.sleep(2)
            seconds_delayed += 2

            lossless_profile = None
            profiles = self.app_db.get_keys('BUFFER_PROFILE_TABLE')
            for key in profiles:
                if re.search(lossless_profile_name_pattern, key):
                    lossless_profile = key
                    break

            if not lossless_profile:
                break

        assert not lossless_profile, \
            "There is still lossless profile ({}) {} seconds after all lossless PGs have been removed".format(lossless_profile, seconds_delayed)

        self.setup_asic_db(dvs)

        self.initialized = True

    def setup_asic_db(self, dvs):
        buffer_pool_set = set(self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_POOL"))
        self.initProfileSet = set(self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE"))
        self.initPGSet = set(self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP"))

        ingress_lossless_pool = self.app_db.get_entry("BUFFER_POOL_TABLE", "ingress_lossless_pool")
        self.ingress_lossless_pool_asic = None

        for key in buffer_pool_set:
            bufferpool = self.asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_POOL", key)
            if bufferpool["SAI_BUFFER_POOL_ATTR_TYPE"] == "SAI_BUFFER_POOL_TYPE_INGRESS":
                if ingress_lossless_pool["size"] == bufferpool["SAI_BUFFER_POOL_ATTR_SIZE"]:
                    self.ingress_lossless_pool_asic = bufferpool
                    self.ingress_lossless_pool_oid = key

    def check_new_profile_in_asic_db(self, dvs, profile):
        diff = set(self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE")) - self.initProfileSet
        if len(diff) == 1:
            self.newProfileInAsicDb = diff.pop()
        assert self.newProfileInAsicDb, "Can't get SAI OID for newly created profile {}".format(profile)

        # in case diff is empty, we just treat the newProfileInAsicDb cached the latest one
        fvs = self.app_db.get_entry("BUFFER_PROFILE_TABLE", profile)
        if fvs.get('dynamic_th'):
            sai_threshold_value = fvs['dynamic_th']
            sai_threshold_mode = 'SAI_BUFFER_PROFILE_THRESHOLD_MODE_DYNAMIC'
        else:
            sai_threshold_value = fvs['static_th']
            sai_threshold_mode = 'SAI_BUFFER_PROFILE_THRESHOLD_MODE_STATIC'
        self.asic_db.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE", self.newProfileInAsicDb,
                                             {'SAI_BUFFER_PROFILE_ATTR_XON_TH': fvs['xon'],
                                              'SAI_BUFFER_PROFILE_ATTR_XOFF_TH': fvs['xoff'],
                                              'SAI_BUFFER_PROFILE_ATTR_RESERVED_BUFFER_SIZE': fvs['size'],
                                              'SAI_BUFFER_PROFILE_ATTR_POOL_ID': self.ingress_lossless_pool_oid,
                                              'SAI_BUFFER_PROFILE_ATTR_THRESHOLD_MODE': sai_threshold_mode,
                                              'SAI_BUFFER_PROFILE_ATTR_SHARED_DYNAMIC_TH': sai_threshold_value},
                                          self.DEFAULT_POLLING_CONFIG)

    def make_lossless_profile_name(self, speed, cable_length, mtu = None, dynamic_th = None):
        extra = ""
        if mtu:
            extra += "_mtu" + mtu
        if dynamic_th:
            extra += "_th" + dynamic_th

        return "pg_lossless_" + speed + "_" + cable_length + extra + "_profile"

    def change_cable_length(self, cable_length):
        cable_lengths = self.config_db.get_entry('CABLE_LENGTH', 'AZURE')
        cable_lengths['Ethernet0'] = cable_length
        self.config_db.update_entry('CABLE_LENGTH', 'AZURE', cable_lengths)

    def test_changeSpeed(self, dvs, testlog):
        self.setup_db(dvs)

        # configure lossless PG 3-4 on interface
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'NULL'})

        # Change speed to speed1 and verify whether the profile has been updated
        dvs.runcmd("config interface speed Ethernet0 " + self.speedToTest1)

        expectedProfile = self.make_lossless_profile_name(self.speedToTest1, self.originalCableLen)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.check_new_profile_in_asic_db(dvs, expectedProfile)

        # Check whether buffer pg align
        bufferPg = self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile + "]"})

        # Remove lossless PG
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')
        self.app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", "Ethernet0:3-4")

        # Change speed to speed2 and verify
        dvs.runcmd("config interface speed Ethernet0 " + self.speedToTest2)
        expectedProfile = self.make_lossless_profile_name(self.speedToTest2, self.originalCableLen)

        # Re-add another lossless PG
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|6', {'profile': 'NULL'})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:6", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile + "]"})

        # Remove the lossless PG 6
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|6')
        self.app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", "Ethernet0:6")

        # Remove the lossless PG 3-4 and revert speed
        dvs.runcmd("config interface speed Ethernet0 " + self.originalSpeed)
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'NULL'})

        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.check_new_profile_in_asic_db(dvs, expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile + "]"})

        # remove lossless PG 3-4 on interface
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')
        self.app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", "Ethernet0:3-4")

    def test_changeCableLen(self, dvs, testlog):
        self.setup_db(dvs)

        # configure lossless PG 3-4 on interface
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'NULL'})

        # Change to new cable length
        self.change_cable_length(self.cableLenTest1)
        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.cableLenTest1)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.check_new_profile_in_asic_db(dvs, expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile + "]"})

        # Remove the lossless PGs
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')
        self.app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", "Ethernet0:3-4")

        # Change to another cable length
        self.change_cable_length(self.cableLenTest2)
        # Check whether the old profile has been removed
        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", expectedProfile)

        # Re-add lossless PGs
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'NULL'})
        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.cableLenTest2)
        # Check the BUFFER_PROFILE_TABLE and BUFFER_PG_TABLE
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.check_new_profile_in_asic_db(dvs, expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile + "]"})

        # Revert the cable length
        self.change_cable_length(self.originalCableLen)
        # Check the BUFFER_PROFILE_TABLE and BUFFER_PG_TABLE
        # we are not able to check whether the SAI OID is removed from ASIC DB here
        # because sometimes the SAI OID in ASIC DB can be reused for the newly created profile
        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", expectedProfile)

        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile + "]"})

        # remove lossless PG 3-4 on interface
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')

    def test_MultipleLosslessPg(self, dvs, testlog):
        self.setup_db(dvs)

        # configure lossless PG 3-4 on interface
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'NULL'})

        # Add another lossless PG
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|6', {'profile': 'NULL'})
        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:6", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile + "]"})

        # change speed and check
        dvs.runcmd("config interface speed Ethernet0 " + self.speedToTest1)
        expectedProfile = self.make_lossless_profile_name(self.speedToTest1, self.originalCableLen)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile + "]"})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:6", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile + "]"})

        # change cable length and check
        self.change_cable_length(self.cableLenTest1)
        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        expectedProfile = self.make_lossless_profile_name(self.speedToTest1, self.cableLenTest1)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.check_new_profile_in_asic_db(dvs, expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile + "]"})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:6", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile + "]"})

        # revert the speed and cable length and check
        self.change_cable_length(self.originalCableLen)
        dvs.runcmd("config interface speed Ethernet0 " + self.originalSpeed)
        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.asic_db.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE", self.newProfileInAsicDb)
        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile + "]"})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:6", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile + "]"})

        # remove lossless PG 3-4 and 6 on interface
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|6')

    def test_headroomOverride(self, dvs, testlog):
        self.setup_db(dvs)

        # Configure static profile
        self.config_db.update_entry('BUFFER_PROFILE', 'test',
                                    {'xon': '18432',
                                     'xoff': '16384',
                                     'size': '34816',
                                     'dynamic_th': '0',
                                     'pool': '[BUFFER_POOL|ingress_lossless_pool]'})

        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", "test")
        self.app_db.wait_for_exact_match("BUFFER_PROFILE_TABLE", "test",
                        { "pool" : "[BUFFER_POOL_TABLE:ingress_lossless_pool]",
                          "xon" : "18432",
                          "xoff" : "16384",
                          "size" : "34816",
                          "dynamic_th" : "0"
                          })
        self.check_new_profile_in_asic_db(dvs, "test")

        # configure lossless PG 3-4 on interface
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'NULL'})

        self.change_cable_length(self.cableLenTest1)
        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.cableLenTest1)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile + "]"})

        # configure lossless PG 3-4 with headroom override
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': '[BUFFER_PROFILE|test]'})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "[BUFFER_PROFILE_TABLE:test]"})
        # configure lossless PG 6 with headroom override
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|6', {'profile': '[BUFFER_PROFILE|test]'})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:6", {"profile": "[BUFFER_PROFILE_TABLE:test]"})

        # update the profile
        self.config_db.update_entry('BUFFER_PROFILE', 'test',
                                    {'xon': '18432',
                                     'xoff': '18432',
                                     'size': '36864',
                                     'dynamic_th': '0',
                                     'pool': '[BUFFER_POOL|ingress_lossless_pool]'})
        self.app_db.wait_for_exact_match("BUFFER_PROFILE_TABLE", "test",
                        { "pool" : "[BUFFER_POOL_TABLE:ingress_lossless_pool]",
                          "xon" : "18432",
                          "xoff" : "18432",
                          "size" : "36864",
                          "dynamic_th" : "0"
                          })
        self.check_new_profile_in_asic_db(dvs, "test")

        # remove all lossless PGs
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|6')

        self.app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", "Ethernet0:3-4")
        self.app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", "Ethernet0:6")

        # readd lossless PG with dynamic profile
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profie': 'NULL'})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile + "]"})

        # remove the headroom override profile
        self.config_db.delete_entry('BUFFER_PROFILE', 'test')
        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", "test")
        self.asic_db.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE", self.newProfileInAsicDb)

        self.change_cable_length(self.originalCableLen)
        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile + "]"})

        # remove lossless PG 3-4 on interface
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')

    def test_mtuUpdate(self, dvs, testlog):
        self.setup_db(dvs)

        test_mtu = '1500'
        default_mtu = '9100'
        expectedProfileMtu = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen, mtu = test_mtu)
        expectedProfileNormal = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen)

        # update the mtu on the interface
        dvs.runcmd("config interface mtu Ethernet0 {}".format(test_mtu))

        # configure lossless PG 3-4 on interface
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'NULL'})

        self.app_db.wait_for_entry("BUFFER_PG_TABLE", "Ethernet0:3-4")
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfileMtu)
        self.check_new_profile_in_asic_db(dvs, expectedProfileMtu)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "[BUFFER_PROFILE_TABLE:{}]".format(expectedProfileMtu)})

        dvs.runcmd("config interface mtu Ethernet0 {}".format(default_mtu))

        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", expectedProfileMtu)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfileNormal)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "[BUFFER_PROFILE_TABLE:{}]".format(expectedProfileNormal)})

        # clear configuration
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')

    def test_nonDefaultAlpha(self, dvs, testlog):
        self.setup_db(dvs)

        test_dynamic_th_1 = '1'
        expectedProfile_th1 = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen, dynamic_th = test_dynamic_th_1)
        test_dynamic_th_2 = '2'
        expectedProfile_th2 = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen, dynamic_th = test_dynamic_th_2)

        # add a profile with non-default dynamic_th
        self.config_db.update_entry('BUFFER_PROFILE', 'non-default-dynamic',
                                    {'dynamic_th': test_dynamic_th_1,
                                     'headroom_type': 'dynamic',
                                     'pool': '[BUFFER_POOL|ingress_lossless_pool]'})

        # configure lossless PG 3-4 on interface
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': '[BUFFER_PROFILE|non-default-dynamic]'})

        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile_th1)
        self.check_new_profile_in_asic_db(dvs, expectedProfile_th1)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile_th1 + "]"})

        # modify the profile to another dynamic_th
        self.config_db.update_entry('BUFFER_PROFILE', 'non-default-dynamic',
                                    {'dynamic_th': test_dynamic_th_2,
                                     'headroom_type': 'dynamic',
                                     'pool': '[BUFFER_POOL|ingress_lossless_pool]'})

        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", expectedProfile_th1)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile_th2)
        self.check_new_profile_in_asic_db(dvs, expectedProfile_th2)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "[BUFFER_PROFILE_TABLE:" + expectedProfile_th2 + "]"})

        # clear configuration
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')
        self.config_db.delete_entry('BUFFER_PROFILE', 'non-default-dynamic')
