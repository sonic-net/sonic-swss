import time
import pytest
import re
import buffer_model

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
        self.cableLenBeforeTest = self.originalCableLen
        if self.originalCableLen == self.cableLenTest1:
            self.cableLenTest1 = "20m"
        elif self.originalCableLen == self.cableLenTest2:
            self.cableLenTest2 = "20m"
        elif self.originalCableLen == "0m":
            fvs["Ethernet0"] = "5m"
            self.originalCableLen = "5m"
            self.config_db.update_entry("CABLE_LENGTH", "AZURE", fvs)

        fvs = {"mmu_size": "12766208"}
        self.state_db.create_entry("BUFFER_MAX_PARAM_TABLE", "global", fvs)
        self.bufferMaxParameter = self.state_db.wait_for_entry("BUFFER_MAX_PARAM_TABLE", "Ethernet0")

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

        time.sleep(10)

        self.setup_asic_db(dvs)

        self.initialized = True

    def cleanup_db(self, dvs):
        # Clean up: restore the origin cable length
        fvs = self.config_db.wait_for_entry("CABLE_LENGTH", "AZURE")
        fvs["Ethernet0"] = self.cableLenBeforeTest
        self.config_db.update_entry("CABLE_LENGTH", "AZURE", fvs)

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
        retry_count = 0
        self.newProfileInAsicDb = None
        while retry_count < 5:
            retry_count += 1
            diff = set(self.asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE")) - self.initProfileSet
            if len(diff) == 1:
                self.newProfileInAsicDb = diff.pop()
                break
            else:
                time.sleep(1)
        assert self.newProfileInAsicDb, "Can't get SAI OID for newly created profile {} after retry {} times".format(profile, retry_count)

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

    def check_queues_after_port_startup(self, dvs):
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "{}:0-2".format("Ethernet0"), {"profile": "egress_lossy_profile"})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "{}:3-4".format("Ethernet0"), {"profile": "egress_lossless_profile"})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "{}:5-6".format("Ethernet0"), {"profile": "egress_lossy_profile"})

    def test_changeSpeed(self, dvs, testlog):
        self.setup_db(dvs)

        # Startup interface
        dvs.port_admin_set('Ethernet0', 'up')
        self.check_queues_after_port_startup(dvs)

        # Configure lossless PG 3-4 on interface
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'NULL'})

        # Change speed to speed1 and verify whether the profile has been updated
        dvs.port_field_set("Ethernet0", "speed", self.speedToTest1)

        expectedProfile = self.make_lossless_profile_name(self.speedToTest1, self.originalCableLen)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.check_new_profile_in_asic_db(dvs, expectedProfile)

        # Check whether buffer pg align
        bufferPg = self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})

        # Remove lossless PG
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')
        self.app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", "Ethernet0:3-4")

        # Change speed to speed2 and verify
        dvs.port_field_set("Ethernet0", "speed", self.speedToTest2)
        expectedProfile = self.make_lossless_profile_name(self.speedToTest2, self.originalCableLen)

        # Re-add another lossless PG
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|6', {'profile': 'NULL'})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:6", {"profile": expectedProfile})

        # Remove the lossless PG 6
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|6')
        self.app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", "Ethernet0:6")

        # Remove the lossless PG 3-4 and revert speed
        dvs.port_field_set("Ethernet0", "speed", self.originalSpeed)
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'NULL'})

        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.check_new_profile_in_asic_db(dvs, expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})

        # Remove lossless PG 3-4 on interface
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')
        self.app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", "Ethernet0:3-4")

        # Shutdown interface
        dvs.port_admin_set('Ethernet0', 'down')

        self.cleanup_db(dvs)

    @pytest.mark.skip(reason="Failing. Under investigation")
    def test_changeCableLen(self, dvs, testlog):
        self.setup_db(dvs)

        # Startup interface
        dvs.port_admin_set('Ethernet0', 'up')

        # Configure lossless PG 3-4 on interface
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'NULL'})

        # Change to new cable length
        self.change_cable_length(self.cableLenTest1)
        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.cableLenTest1)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.check_new_profile_in_asic_db(dvs, expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})

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
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})

        # Revert the cable length
        self.change_cable_length(self.originalCableLen)
        # Check the BUFFER_PROFILE_TABLE and BUFFER_PG_TABLE
        # we are not able to check whether the SAI OID is removed from ASIC DB here
        # because sometimes the SAI OID in ASIC DB can be reused for the newly created profile
        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", expectedProfile)

        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})

        # Remove lossless PG 3-4 on interface
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')

        # Shutdown interface
        dvs.port_admin_set('Ethernet0', 'down')

        self.cleanup_db(dvs)

    def test_MultipleLosslessPg(self, dvs, testlog):
        self.setup_db(dvs)

        # Startup interface
        dvs.port_admin_set('Ethernet0', 'up')

        # Configure lossless PG 3-4 on interface
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'NULL'})

        # Add another lossless PG
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|6', {'profile': 'NULL'})
        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:6", {"profile": expectedProfile})

        # Change speed and check
        dvs.port_field_set("Ethernet0", "speed", self.speedToTest1)
        expectedProfile = self.make_lossless_profile_name(self.speedToTest1, self.originalCableLen)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:6", {"profile": expectedProfile})

        # Change cable length and check
        self.change_cable_length(self.cableLenTest1)
        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        expectedProfile = self.make_lossless_profile_name(self.speedToTest1, self.cableLenTest1)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.check_new_profile_in_asic_db(dvs, expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:6", {"profile": expectedProfile})

        # Revert the speed and cable length and check
        self.change_cable_length(self.originalCableLen)
        dvs.port_field_set("Ethernet0", "speed", self.originalSpeed)
        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.asic_db.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE", self.newProfileInAsicDb)
        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:6", {"profile": expectedProfile})

        # Remove lossless PG 3-4 and 6 on interface
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|6')

        # Shutdown interface
        dvs.port_admin_set('Ethernet0', 'down')

        self.cleanup_db(dvs)

    def test_headroomOverride(self, dvs, testlog):
        self.setup_db(dvs)

        # Startup interface
        dvs.port_admin_set('Ethernet0', 'up')

        # Configure static profile
        self.config_db.update_entry('BUFFER_PROFILE', 'test',
                                    {'xon': '18432',
                                     'xoff': '16384',
                                     'size': '34816',
                                     'dynamic_th': '0',
                                     'pool': 'ingress_lossless_pool'})

        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", "test")
        self.app_db.wait_for_exact_match("BUFFER_PROFILE_TABLE", "test",
                        { "pool" : "ingress_lossless_pool",
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
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})

        # configure lossless PG 3-4 with headroom override
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'test'})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": "test"})
        # configure lossless PG 6 with headroom override
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|6', {'profile': 'test'})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:6", {"profile": "test"})

        # update the profile
        self.config_db.update_entry('BUFFER_PROFILE', 'test',
                                    {'xon': '18432',
                                     'xoff': '18432',
                                     'size': '36864',
                                     'dynamic_th': '0',
                                     'pool': 'ingress_lossless_pool'})
        self.app_db.wait_for_exact_match("BUFFER_PROFILE_TABLE", "test",
                        { "pool" : "ingress_lossless_pool",
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
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'NULL'})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})

        # remove the headroom override profile
        self.config_db.delete_entry('BUFFER_PROFILE', 'test')
        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", "test")
        self.asic_db.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE", self.newProfileInAsicDb)

        self.change_cable_length(self.originalCableLen)
        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})

        # remove lossless PG 3-4 on interface
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')

        # Shutdown interface
        dvs.port_admin_set('Ethernet0', 'down')

        self.cleanup_db(dvs)

    def test_mtuUpdate(self, dvs, testlog):
        self.setup_db(dvs)

        # Startup interface
        dvs.port_admin_set('Ethernet0', 'up')

        test_mtu = '1500'
        default_mtu = '9100'
        expectedProfileMtu = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen, mtu = test_mtu)
        expectedProfileNormal = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen)

        # update the mtu on the interface
        dvs.port_field_set("Ethernet0", "mtu", test_mtu)

        # configure lossless PG 3-4 on interface
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'NULL'})

        self.app_db.wait_for_entry("BUFFER_PG_TABLE", "Ethernet0:3-4")
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfileMtu)
        self.check_new_profile_in_asic_db(dvs, expectedProfileMtu)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfileMtu})

        dvs.port_field_set("Ethernet0", "mtu", default_mtu)

        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", expectedProfileMtu)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfileNormal)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfileNormal})

        # clear configuration
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')

        # Shutdown interface
        dvs.port_admin_set('Ethernet0', 'down')

        self.cleanup_db(dvs)

    def test_nonDefaultAlpha(self, dvs, testlog):
        self.setup_db(dvs)

        # Startup interface
        dvs.port_admin_set('Ethernet0', 'up')

        test_dynamic_th_1 = '1'
        expectedProfile_th1 = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen, dynamic_th = test_dynamic_th_1)
        test_dynamic_th_2 = '2'
        expectedProfile_th2 = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen, dynamic_th = test_dynamic_th_2)

        # add a profile with non-default dynamic_th
        self.config_db.update_entry('BUFFER_PROFILE', 'non-default-dynamic',
                                    {'dynamic_th': test_dynamic_th_1,
                                     'headroom_type': 'dynamic',
                                     'pool': 'ingress_lossless_pool'})

        # configure lossless PG 3-4 on interface
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'non-default-dynamic'})

        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile_th1)
        self.check_new_profile_in_asic_db(dvs, expectedProfile_th1)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile_th1})

        # modify the profile to another dynamic_th
        self.config_db.update_entry('BUFFER_PROFILE', 'non-default-dynamic',
                                    {'dynamic_th': test_dynamic_th_2,
                                     'headroom_type': 'dynamic',
                                     'pool': 'ingress_lossless_pool'})

        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE_TABLE", expectedProfile_th1)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile_th2)
        self.check_new_profile_in_asic_db(dvs, expectedProfile_th2)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile_th2})

        # clear configuration
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')
        self.config_db.delete_entry('BUFFER_PROFILE', 'non-default-dynamic')

        # Shutdown interface
        dvs.port_admin_set('Ethernet0', 'down')

        self.cleanup_db(dvs)

    def test_sharedHeadroomPool(self, dvs, testlog):
        self.setup_db(dvs)

        # Startup interface
        dvs.port_admin_set('Ethernet0', 'up')

        # configure lossless PG 3-4 on interface and start up the interface
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'NULL'})

        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})
        self.check_new_profile_in_asic_db(dvs, expectedProfile)
        profileInApplDb = self.app_db.get_entry('BUFFER_PROFILE_TABLE', expectedProfile)

        # enable shared headroom pool by configuring over subscribe ratio
        default_lossless_buffer_parameter = self.config_db.get_entry('DEFAULT_LOSSLESS_BUFFER_PARAMETER', 'AZURE')
        over_subscribe_ratio = default_lossless_buffer_parameter.get('over_subscribe_ratio')
        assert not over_subscribe_ratio or over_subscribe_ratio == '0', "Over subscribe ratio isn't 0"

        # config over subscribe ratio to 2
        default_lossless_buffer_parameter['over_subscribe_ratio'] = '2'
        self.config_db.update_entry('DEFAULT_LOSSLESS_BUFFER_PARAMETER', 'AZURE', default_lossless_buffer_parameter)

        # check buffer profile: xoff should be removed from size
        profileInApplDb['size'] = profileInApplDb['xon']
        self.app_db.wait_for_field_match('BUFFER_PROFILE_TABLE', expectedProfile, profileInApplDb)
        self.check_new_profile_in_asic_db(dvs, expectedProfile)

        # Check ingress_lossless_pool between appldb and asicdb
        # There are only two lossless PGs configured on one port.
        # Hence the shared headroom pool size should be (pg xoff * 2 - private headroom) / over subscribe ratio (2) = xoff - private_headroom / 2.
        ingress_lossless_pool_in_appldb = self.app_db.get_entry('BUFFER_POOL_TABLE', 'ingress_lossless_pool')
        private_headroom = 10 * 1024
        shp_size = str(int(profileInApplDb['xoff']) - int(private_headroom / 2))
        ingress_lossless_pool_in_appldb['xoff'] = shp_size
        # toggle shared headroom pool, it requires some time to update pools
        time.sleep(20)
        self.app_db.wait_for_field_match('BUFFER_POOL_TABLE', 'ingress_lossless_pool', ingress_lossless_pool_in_appldb)
        ingress_lossless_pool_in_asicdb = self.asic_db.get_entry('ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_PROFILE', self.ingress_lossless_pool_oid)
        ingress_lossless_pool_in_asicdb['SAI_BUFFER_POOL_ATTR_XOFF_SIZE'] = shp_size
        self.asic_db.wait_for_field_match('ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_POOL', self.ingress_lossless_pool_oid, ingress_lossless_pool_in_asicdb)

        # config shared headroom pool size
        shp_size = '204800'
        ingress_lossless_pool_in_configdb = self.config_db.get_entry('BUFFER_POOL', 'ingress_lossless_pool')
        ingress_lossless_pool_in_configdb['xoff'] = shp_size
        self.config_db.update_entry('BUFFER_POOL', 'ingress_lossless_pool', ingress_lossless_pool_in_configdb)
        # make sure the size is still equal to xon in the profile
        self.app_db.wait_for_field_match('BUFFER_PROFILE_TABLE', expectedProfile, profileInApplDb)
        self.check_new_profile_in_asic_db(dvs, expectedProfile)

        # config over subscribe ratio to 4
        default_lossless_buffer_parameter['over_subscribe_ratio'] = '4'
        self.config_db.update_entry('DEFAULT_LOSSLESS_BUFFER_PARAMETER', 'AZURE', default_lossless_buffer_parameter)
        # shp size wins in case both size and over subscribe ratio is configured
        ingress_lossless_pool_in_appldb['xoff'] = shp_size
        self.app_db.wait_for_field_match('BUFFER_POOL_TABLE', 'ingress_lossless_pool', ingress_lossless_pool_in_appldb)
        ingress_lossless_pool_in_asicdb['SAI_BUFFER_POOL_ATTR_XOFF_SIZE'] = shp_size
        self.asic_db.wait_for_field_match('ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_POOL', self.ingress_lossless_pool_oid, ingress_lossless_pool_in_asicdb)
        # make sure the size is still equal to xon in the profile
        self.app_db.wait_for_field_match('BUFFER_PROFILE_TABLE', expectedProfile, profileInApplDb)
        self.check_new_profile_in_asic_db(dvs, expectedProfile)

        # remove size configuration, new over subscribe ratio takes effect
        ingress_lossless_pool_in_configdb['xoff'] = '0'
        self.config_db.update_entry('BUFFER_POOL', 'ingress_lossless_pool', ingress_lossless_pool_in_configdb)
        # shp size: (pg xoff * 2 - private headroom) / over subscribe ratio (4)
        shp_size = str(int((2 * int(profileInApplDb['xoff']) - private_headroom) / 4))
        time.sleep(30)
        ingress_lossless_pool_in_appldb['xoff'] = shp_size
        self.app_db.wait_for_field_match('BUFFER_POOL_TABLE', 'ingress_lossless_pool', ingress_lossless_pool_in_appldb)
        ingress_lossless_pool_in_asicdb['SAI_BUFFER_POOL_ATTR_XOFF_SIZE'] = shp_size
        self.asic_db.wait_for_field_match('ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_POOL', self.ingress_lossless_pool_oid, ingress_lossless_pool_in_asicdb)
        # make sure the size is still equal to xon in the profile
        self.app_db.wait_for_field_match('BUFFER_PROFILE_TABLE', expectedProfile, profileInApplDb)
        self.check_new_profile_in_asic_db(dvs, expectedProfile)

        # remove over subscribe ratio configuration
        default_lossless_buffer_parameter['over_subscribe_ratio'] = '0'
        self.config_db.update_entry('DEFAULT_LOSSLESS_BUFFER_PARAMETER', 'AZURE', default_lossless_buffer_parameter)
        # check whether shp size has been removed from both asic db and appl db
        ingress_lossless_pool_in_appldb['xoff'] = '0'
        self.app_db.wait_for_field_match('BUFFER_POOL_TABLE', 'ingress_lossless_pool', ingress_lossless_pool_in_appldb)
        ingress_lossless_pool_in_asicdb['SAI_BUFFER_POOL_ATTR_XOFF_SIZE'] = '0'
        self.asic_db.wait_for_field_match('ASIC_STATE:SAI_OBJECT_TYPE_BUFFER_POOL', self.ingress_lossless_pool_oid, ingress_lossless_pool_in_asicdb)
        # make sure the size is equal to xon + xoff in the profile
        profileInApplDb['size'] = str(int(profileInApplDb['xon']) + int(profileInApplDb['xoff']))
        self.app_db.wait_for_field_match('BUFFER_PROFILE_TABLE', expectedProfile, profileInApplDb)
        self.check_new_profile_in_asic_db(dvs, expectedProfile)

        # remove lossless PG 3-4 on interface
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')
        dvs.port_admin_set('Ethernet0', 'down')

        # Shutdown interface
        dvs.port_admin_set('Ethernet0', 'down')

        self.cleanup_db(dvs)

    def test_shutdownPort(self, dvs, testlog):
        self.setup_db(dvs)

        lossy_pg_reference_config_db = 'ingress_lossy_profile'
        lossy_pg_reference_appl_db = 'ingress_lossy_profile'
        lossy_queue_reference_config_db = 'egress_lossy_profile'
        lossy_queue_reference_appl_db = 'egress_lossy_profile'
        lossless_queue_reference_appl_db = 'egress_lossless_profile'

        lossy_pg_zero_reference = 'ingress_lossy_pg_zero_profile'
        lossy_queue_zero_reference = 'egress_lossy_zero_profile'
        lossless_queue_zero_reference = 'egress_lossless_zero_profile'

        # Startup interface
        dvs.port_admin_set('Ethernet0', 'up')

        # Configure lossless PG 3-4 on interface
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'NULL'})
        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})

        # Shutdown port and check whether zero profiles have been applied on queues and the PG 0
        maximumQueues = int(self.bufferMaxParameter['max_queues']) - 1
        dvs.port_admin_set('Ethernet0', 'down')
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:0", {"profile": lossy_pg_zero_reference})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:0-2", {"profile": lossy_queue_zero_reference})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:3-4", {"profile": lossless_queue_zero_reference})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:5-6", {"profile": lossy_queue_zero_reference})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:7-{}".format(maximumQueues), {"profile": lossy_queue_zero_reference})
        self.app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", "Ethernet0:3-4")
        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE", expectedProfile)

        # Add extra lossy and lossless PGs when a port is administratively down
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|1', {'profile': lossy_pg_reference_config_db})
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|6', {'profile': 'NULL'})

        # Add extra queue when a port is administratively down
        self.config_db.update_entry('BUFFER_QUEUE', 'Ethernet0|8', {"profile": lossy_queue_reference_config_db})

        # For queues, the slice in supported but not configured list should be '7-19'.
        # After queue '8' is added, '7-19' should be removed and '7', '8', and '9-19' will be added
        self.app_db.wait_for_deleted_entry("BUFFER_QUEUE_TABLE", "Ethernet0:7-{}".format(maximumQueues))
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:7", {"profile": lossy_queue_zero_reference})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:8", {"profile": lossy_queue_zero_reference})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:9-{}".format(maximumQueues), {"profile": lossy_queue_zero_reference})
        # Make sure they have not been added to APPL_DB
        time.sleep(30)
        self.app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", "Ethernet0:1")
        self.app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", "Ethernet0:6")

        # Startup port and check whether all the PGs have been added
        dvs.port_admin_set('Ethernet0', 'up')
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:0", {"profile": lossy_pg_reference_appl_db})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:1", {"profile": lossy_pg_reference_appl_db})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:6", {"profile": expectedProfile})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:0-2", {"profile": lossy_queue_reference_appl_db})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:5-6", {"profile": lossy_queue_reference_appl_db})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:8", {"profile": lossy_queue_reference_appl_db})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:3-4", {"profile": lossless_queue_reference_appl_db})
        self.app_db.wait_for_deleted_entry("BUFFER_QUEUE_TABLE", "Ethernet0:7")
        self.app_db.wait_for_deleted_entry("BUFFER_QUEUE_TABLE", "Ethernet0:9-{}".format(maximumQueues))

        # Shutdown the port again to verify flow to remove buffer objects from an admin down port
        dvs.port_admin_set('Ethernet0', 'down')
        # First, check whether the objects have been correctly handled
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:0", {"profile": lossy_pg_zero_reference})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:0-2", {"profile": lossy_queue_zero_reference})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:3-4", {"profile": lossless_queue_zero_reference})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:5-6", {"profile": lossy_queue_zero_reference})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:7", {"profile": lossy_queue_zero_reference})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:8", {"profile": lossy_queue_zero_reference})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:9-{}".format(maximumQueues), {"profile": lossy_queue_zero_reference})
        self.app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", "Ethernet0:3-4")
        self.app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", "Ethernet0:1")
        self.app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", "Ethernet0:6")
        self.app_db.wait_for_deleted_entry("BUFFER_PROFILE", expectedProfile)

        # Remove buffer objects from an admon down port
        self.config_db.delete_entry('BUFFER_QUEUE', 'Ethernet0|8')
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|1')
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|6')

        # Checking
        self.app_db.wait_for_deleted_entry("BUFFER_QUEUE_TABLE", "Ethernet0:7")
        self.app_db.wait_for_deleted_entry("BUFFER_QUEUE_TABLE", "Ethernet0:8")
        self.app_db.wait_for_deleted_entry("BUFFER_QUEUE_TABLE", "Ethernet0:9-{}".format(maximumQueues))
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:7-{}".format(maximumQueues), {"profile": lossy_queue_zero_reference})

        # Startup again
        dvs.port_admin_set('Ethernet0', 'up')
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:0-2", {"profile": lossy_queue_reference_appl_db})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:3-4", {"profile": lossless_queue_reference_appl_db})
        self.app_db.wait_for_field_match("BUFFER_QUEUE_TABLE", "Ethernet0:5-6", {"profile": lossy_queue_reference_appl_db})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:0", {"profile": lossy_pg_reference_appl_db})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})
        self.app_db.wait_for_deleted_entry("BUFFER_QUEUE_TABLE", "Ethernet0:7-{}".format(maximumQueues))

        # Remove lossless PG 3-4 on interface
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')

        # Shutdown interface
        dvs.port_admin_set('Ethernet0', 'down')

        self.cleanup_db(dvs)

    def test_autoNegPort(self, dvs, testlog):
        self.setup_db(dvs)

        advertised_speeds = '10000,25000,50000'
        maximum_advertised_speed = '50000'
        if maximum_advertised_speed == self.originalSpeed:
            # Let's make sure the configured speed isn't equal to maximum advertised speed
            advertised_speeds = '10000,25000'
            maximum_advertised_speed = '25000'

        # Startup interfaces
        dvs.port_admin_set('Ethernet0', 'up')

        # Configure lossless PG 3-4 on the interface
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|3-4', {'profile': 'NULL'})

        # Enable port auto negotiation
        dvs.port_field_set('Ethernet0','autoneg', 'on')
        dvs.port_field_set('Ethernet0','adv_speeds', advertised_speeds)

        # Check the buffer profile. The maximum_advertised_speed should be used
        expectedProfile = self.make_lossless_profile_name(maximum_advertised_speed, self.originalCableLen)
        self.app_db.wait_for_entry("BUFFER_PG_TABLE", "Ethernet0:3-4")
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.check_new_profile_in_asic_db(dvs, expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})

        # Configure another lossless PG on the interface
        self.config_db.update_entry('BUFFER_PG', 'Ethernet0|6', {'profile': 'NULL'})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:6", {"profile": expectedProfile})

        # Disable port auto negotiation
        dvs.port_field_set('Ethernet0','autoneg', 'off')

        # Check the buffer profile. The configured speed should be used
        expectedProfile = self.make_lossless_profile_name(self.originalSpeed, self.originalCableLen)
        self.app_db.wait_for_entry("BUFFER_PROFILE_TABLE", expectedProfile)
        self.check_new_profile_in_asic_db(dvs, expectedProfile)
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:3-4", {"profile": expectedProfile})
        self.app_db.wait_for_field_match("BUFFER_PG_TABLE", "Ethernet0:6", {"profile": expectedProfile})

        # Remove lossless PGs on the interface
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|3-4')
        self.config_db.delete_entry('BUFFER_PG', 'Ethernet0|6')

        # Shutdown interface
        dvs.port_admin_set('Ethernet0', 'down')

        self.cleanup_db(dvs)

    def test_removeBufferPool(self, dvs, testlog):
        self.setup_db(dvs)
        # Initialize additional databases that are used by this test only
        self.counter_db = dvs.get_counters_db()
        self.flex_db = dvs.get_flex_db()

        try:
            # Create a new pool
            self.config_db.update_entry('BUFFER_POOL', 'ingress_test_pool', {'size': '0', 'mode': 'static', 'type': 'ingress'})

            # Whether counterpoll is enabled? Enable it if not.
            flex_counter = self.config_db.get_entry("FLEX_COUNTER_TABLE", "BUFFER_POOL_WATERMARK")
            counter_poll_disabled = (not flex_counter or flex_counter["FLEX_COUNTER_STATUS"] != 'enable')
            if counter_poll_disabled:
                self.config_db.update_entry("FLEX_COUNTER_TABLE", "BUFFER_POOL_WATERMARK", {"FLEX_COUNTER_STATUS": "enable"})

            # Check whether counter poll has been enabled
            time.sleep(1)
            poolmap = self.counter_db.wait_for_entry("COUNTERS_BUFFER_POOL_NAME_MAP", "")
            assert poolmap["ingress_test_pool"]
            self.flex_db.wait_for_entry("FLEX_COUNTER_TABLE", "BUFFER_POOL_WATERMARK_STAT_COUNTER:{}".format(poolmap["ingress_test_pool"]))

            self.config_db.delete_entry('BUFFER_POOL', 'ingress_test_pool')
            oid_to_remove = poolmap.pop('ingress_test_pool')
            self.counter_db.wait_for_field_match("COUNTERS_BUFFER_POOL_NAME_MAP", "", poolmap)
            self.flex_db.wait_for_deleted_entry("FLEX_COUNTER_TABLE", "BUFFER_POOL_WATERMARK_STAT_COUNTER:{}".format(oid_to_remove))
        finally:
            # Clean up: disable counterpoll if it was disabled
            if counter_poll_disabled:
                self.config_db.delete_entry("FLEX_COUNTER_TABLE", "BUFFER_POOL_WATERMARK")

        self.cleanup_db(dvs)

    def test_bufferPortMaxParameter(self, dvs, testlog):
        self.setup_db(dvs)

        # Check whether port's maximum parameter has been exposed to STATE_DB
        fvs = self.state_db.wait_for_entry("BUFFER_MAX_PARAM_TABLE", "Ethernet0")
        assert int(fvs["max_queues"]) and int(fvs["max_priority_groups"])

        self.cleanup_db(dvs)
