import pytest
import time
from dvslib.dvs_common import PollingConfig

# the port to be removed and add
PORT = "Ethernet0"

class TestPortAddRemove(object):

    def test_remove_port_with_buffer_cfg(self, dvs, testlog):
        config_db = dvs.get_config_db()
        asic_db = dvs.get_asic_db()

        # get port info
        port_info = config_db.get_entry("PORT", PORT)

        # get the number of ports before removal
        num_of_ports = len(asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT"))

        # try to remove this port
        config_db.delete_entry('PORT', PORT)
        num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                                      num_of_ports-1,
                                      polling_config = PollingConfig(polling_interval = 1, timeout = 5.00, strict = False))

        # verify that the port wasn't removed since we still have buffer cfg
        assert len(num) == num_of_ports

        # remove buffer pg cfg for the port
        pgs = config_db.get_keys('BUFFER_PG')
        for key in pgs:
            if PORT in key:
                config_db.delete_entry('BUFFER_PG', key)

        num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                              num_of_ports-1,
                              polling_config = PollingConfig(polling_interval = 1, timeout = 5.00, strict = False))

        # verify that the port wasn't removed since we still have buffer cfg
        assert len(num) == num_of_ports

        # modify buffer queue entry to egress_lossless_profile instead of egress_lossy_profile
        config_db.update_entry("BUFFER_QUEUE", "%s|0-2"%PORT, {"profile": "egress_lossless_profile"})

        # remove buffer queue cfg for the port
        pgs = config_db.get_keys('BUFFER_QUEUE')
        for key in pgs:
            if PORT in key:
                config_db.delete_entry('BUFFER_QUEUE', key)

        num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                              num_of_ports-1,
                              polling_config = PollingConfig(polling_interval = 1, timeout = 5.00, strict = True))

        # verify that the port was removed properly since all buffer configuration was removed also
        assert len(num) == num_of_ports - 1
