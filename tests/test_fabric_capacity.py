import random
from dvslib.dvs_database import DVSDatabase
from dvslib.dvs_common import PollingConfig


class TestVirtualChassis(object):
    def test_voq_switch_fabric_capacity(self, vst):
        """Test basic fabric capacity infrastructure in VOQ switchs.

        This test validates that when fabric links get isolated, the fabric capacity
        get updated in the state_db.
        When the link get unisolated, the fabric capacity get set back as well.
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
               # There are 16 fabric ports in the test environment.
               # Choose one link to test.
               portNum = random.randint(1, 16)
               cdb_port = "Fabric"+str(portNum)
               sdb_port = "PORT"+str(portNum)

               # setup test environment
               sdb.update_entry("FABRIC_PORT_TABLE", sdb_port, {"TEST": "TEST"})

               # get current fabric capacity
               capacity = sdb.get_entry("FABRIC_CAPACITY_TABLE", "FABRIC_CAPACITY_DATA")['operating_links']
               if sdb.get_entry("FABRIC_PORT_TABLE", sdb_port)['STATUS'] == 'up':
                   try:
                       # clean up the testing port.
                       # set TEST_CRC_ERRORS to 0
                       # set TEST_CODE_ERRORS to 0
                       sdb.update_entry("FABRIC_PORT_TABLE", sdb_port, {"TEST_CRC_ERRORS":"0"})
                       sdb.update_entry("FABRIC_PORT_TABLE", sdb_port, {"TEST_CODE_ERRORS": "0"})

                       # isolate the link from config_db
                       config_db.update_entry("FABRIC_PORT", cdb_port, {"isolateStatus": "True"})
                       sdb.wait_for_field_match("FABRIC_PORT_TABLE", sdb_port, {"ISOLATED": "1"}, polling_config=max_poll)
                       # check if capacity reduced
                       sdb.wait_for_field_negative_match("FABRIC_CAPACITY_TABLE", "FABRIC_CAPACITY_DATA", {'operating_links': capacity}, polling_config=max_poll)
                       # unisolate the link from config_db
                       config_db.update_entry("FABRIC_PORT", cdb_port, {"isolateStatus": "False"})
                       sdb.wait_for_field_match("FABRIC_PORT_TABLE", sdb_port, {"ISOLATED": "0"}, polling_config=max_poll)
                       sdb.wait_for_field_match("FABRIC_CAPACITY_TABLE", "FABRIC_CAPACITY_DATA", {'operating_links': capacity}, polling_config=max_poll)
                   finally:
                       # cleanup
                       sdb.update_entry("FABRIC_PORT_TABLE", sdb_port, {"TEST_CRC_ERRORS": "0"})
                       sdb.update_entry("FABRIC_PORT_TABLE", sdb_port, {"TEST_CODE_ERRORS": "0"})
                       sdb.update_entry("FABRIC_PORT_TABLE", sdb_port, {"TEST": "product"})
               else:
                   print("The link ", port, " is down")
            else:
               print("We do not check switch type:", cfg_switch_type)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass

