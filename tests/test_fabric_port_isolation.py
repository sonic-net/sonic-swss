import random
from dvslib.dvs_database import DVSDatabase
from dvslib.dvs_common import PollingConfig


class TestVirtualChassis(object):
    def test_voq_switch_fabric_link(self, vst):
        """Test basic fabric link monitoring infrastructure in VOQ switchs.

        This test validates that fabric links get isolated if they experienced some errors.
        And the link get unisolated if it clears the error for several consecutive polls.
        """

        dvss = vst.dvss
        for name in dvss.keys():
            dvs = dvss[name]
            # Get the config information and choose a linecard or fabric card to test.
            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")

            cfg_switch_type = metatbl.get("switch_type")
            if cfg_switch_type == "fabric":

               max_poll = PollingConfig(polling_interval=60, timeout=600, strict=True)
               config_db.update_entry("FABRIC_MONITOR", "FABRIC_MONITOR_DATA",{'monState': 'enable'})
               adb = dvs.get_app_db()
               adb.wait_for_field_match("FABRIC_MONITOR_TABLE","FABRIC_MONITOR_DATA", {'monState': 'enable'}, polling_config=max_poll)

               # get state_db infor
               sdb = dvs.get_state_db()
               # key
               port = "PORT1"
               # There are 16 fabric ports in the test environment.
               portNum = random.randint(1, 16)
               port = "PORT"+str(portNum)
               # wait for link monitoring algorithm skips init pollings
               sdb.update_entry("FABRIC_PORT_TABLE", port, {"TEST": "TEST"})
               if sdb.get_entry("FABRIC_PORT_TABLE", port)['STATUS'] == 'up':
                   try:
                       # clean up the system for the testing port.
                       # set TEST_CRC_ERRORS to 0
                       # set TEST_CODE_ERRORS to 0
                       # set TEST to "TEST"
                       sdb.update_entry("FABRIC_PORT_TABLE", port, {"TEST_CRC_ERRORS":"0"})
                       sdb.update_entry("FABRIC_PORT_TABLE", port, {"TEST_CODE_ERRORS": "0"})
                       # inject testing errors and wait for link get isolated.
                       sdb.update_entry("FABRIC_PORT_TABLE", port, {"TEST_CRC_ERRORS": "2"})
                       sdb.wait_for_field_match("FABRIC_PORT_TABLE", port, {"AUTO_ISOLATED": "1"}, polling_config=max_poll)

                       # clear the testing errors and wait for link get unisolated.
                       sdb.update_entry("FABRIC_PORT_TABLE", port, {"TEST_CRC_ERRORS": "0"})
                       sdb.wait_for_field_match("FABRIC_PORT_TABLE", port, {"AUTO_ISOLATED": "0"}, polling_config=max_poll)

                       # inject testing errors and wait for link get isolated again.
                       sdb.update_entry("FABRIC_PORT_TABLE", port, {"TEST_CRC_ERRORS": "2"})
                       sdb.wait_for_field_match("FABRIC_PORT_TABLE", port, {"AUTO_ISOLATED": "1"}, polling_config=max_poll)
                       # now test force unisolate this link
                       configKey = "Fabric"+str(portNum)
                       curForceStatus = int( config_db.get_entry( "FABRIC_PORT", configKey)['forceUnisolateStatus'] )
                       curForceStatus += 1
                       config_db.update_entry("FABRIC_PORT", configKey, {'forceUnisolateStatus': str(curForceStatus)})
                       config_db.wait_for_field_match("FABRIC_PORT", configKey, {'forceUnisolateStatus': str(curForceStatus)},
                                                      polling_config=max_poll)
                       sdb.wait_for_field_match("FABRIC_PORT_TABLE", port, {"AUTO_ISOLATED": "0"}, polling_config=max_poll)
                   finally:
                       # cleanup
                       sdb.update_entry("FABRIC_PORT_TABLE", port, {"TEST_CRC_ERRORS": "0"})
                       sdb.update_entry("FABRIC_PORT_TABLE", port, {"TEST_CODE_ERRORS": "0"})
                       sdb.update_entry("FABRIC_PORT_TABLE", port, {"TEST": "product"})
               else:
                   print("The link ", port, " is down")
            else:
               print("We do not check switch type:", cfg_switch_type)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass

