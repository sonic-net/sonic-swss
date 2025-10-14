from swsscommon import swsscommon
from dvslib.dvs_database import DVSDatabase


class TestVirtualChassis(object):
    @pytest.mark.skip(reason="Covered by mock test:  FabricPort_Isolation_And_Monitor_Propagates")
    def test_voq_switch_fabric_link(self, vst):
        """Test fabric link manual isolation commands in VOQ switch.

        By issuing config fabric port isolation command, the value
        of isolateStatus field in config_db get changed. This test validates appl_db
        updates of a fabric link isolateStatus as the value in config_db changed.
        """

        dvss = vst.dvss
        for name in dvss.keys():
            dvs = dvss[name]
            # Get the config info
            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")

            cfg_switch_type = metatbl.get("switch_type")
            if cfg_switch_type == "fabric":

               # get app_db/config_db information
               cdb = dvs.get_config_db()
               adb = dvs.get_app_db()

               # check if the fabric montior toggle working
               cdb.update_entry("FABRIC_MONITOR", "FABRIC_MONITOR_DATA",{'monState': 'disable'})
               adb.wait_for_field_match("FABRIC_MONITOR_TABLE","FABRIC_MONITOR_DATA", {'monState': 'disable'})

               cdb.update_entry("FABRIC_MONITOR", "FABRIC_MONITOR_DATA",{'monState': 'enable'})
               adb.wait_for_field_match("FABRIC_MONITOR_TABLE","FABRIC_MONITOR_DATA", {'monState': 'enable'})

               # set config_db to isolateStatus: True
               cdb.update_entry("FABRIC_PORT", "Fabric1", {"isolateStatus": "True"})
               cdb.wait_for_field_match("FABRIC_PORT", "Fabric1", {"isolateStatus": "True"})

               # check if appl_db value changes to isolateStatus: True
               adb.wait_for_field_match("FABRIC_PORT_TABLE", "Fabric1", {"isolateStatus": "True"})

               # cleanup
               cdb.update_entry("FABRIC_PORT", "Fabric1", {"isolateStatus": "False"})
               cdb.wait_for_field_match("FABRIC_PORT", "Fabric1", {"isolateStatus": "False"})
               adb.wait_for_field_match("FABRIC_PORT_TABLE", "Fabric1", {"isolateStatus": "False"})
            else:
               print( "We do not check switch type:", cfg_switch_type )


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass


