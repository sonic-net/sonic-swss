from dvslib.dvs_common import PollingConfig
from dvslib.dvs_database import DVSDatabase
from swsscommon import swsscommon
import random

class TestVirtualChassis(object):
    def test_voq_switch_fabric_rate(self, vst):
        """Test fabric counters rate mpbs commands.

        Choose a fabric link, get the tx_rate.
        Set the test field in the state_db, so the testing value can be read.
        Now wait for the tx_rate increase in the state_db. 
        """

        dvss = vst.dvss
        for name in dvss.keys():
            dvs = dvss[name]
            # Get the config info
            config_db = dvs.get_config_db()
            adb = dvs.get_app_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")

            cfg_switch_type = metatbl.get("switch_type")
            if cfg_switch_type == "fabric":

               max_poll = PollingConfig(polling_interval=60, timeout=600, strict=True)
               config_db.update_entry("FABRIC_MONITOR", "FABRIC_MONITOR_DATA",{'monState': 'disable'})
               adb.wait_for_field_match("FABRIC_MONITOR_TABLE","FABRIC_MONITOR_DATA", {'monState': 'disable'}, polling_config=max_poll)
               config_db.update_entry("FABRIC_MONITOR", "FABRIC_MONITOR_DATA",{'monState': 'enable'})
               adb.wait_for_field_match("FABRIC_MONITOR_TABLE","FABRIC_MONITOR_DATA", {'monState': 'enable'}, polling_config=max_poll)
               # get state_db infor
               sdb = dvs.get_state_db()

               try:
                   # There are 16 fabric ports in the test environment.
                   # Choose one link to test.
                   portNum = random.randint(1, 16)
                   sdb_port = "PORT"+str(portNum)

                   fvs = sdb.wait_for_fields("FABRIC_PORT_TABLE", sdb_port, ['OLD_TX_DATA'], polling_config=max_poll)
                   tx_rate = fvs['OLD_TX_DATA']

                   sdb.update_entry("FABRIC_PORT_TABLE", sdb_port, {"TEST": "TEST"})
                   sdb.wait_for_field_negative_match("FABRIC_PORT_TABLE", sdb_port, {'OLD_TX_DATA': tx_rate}, polling_config=max_poll)
               finally:
                   # cleanup
                   sdb.update_entry("FABRIC_PORT_TABLE", sdb_port, {"TEST": "product"})
            else:
               print( "We do not check switch type:", cfg_switch_type )


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
