import pytest
import time
from dvslib.dvs_common import PollingConfig

# the port to be removed and add
PORT_A = "Ethernet64"
PORT_B = "Ethernet68"

"""
DELETE_CREATE_ITERATIONS defines the number of iteration of delete and create to  ports,
we add different timeouts between delete/create to catch potential race condition that can lead to system crush.
"""
DELETE_CREATE_ITERATIONS = 10

class TestPortAddRemove(object):

    def test_remove_add_remove_port_with_buffer_cfg(self, dvs, testlog):
        config_db = dvs.get_config_db()
        asic_db = dvs.get_asic_db()

        # get port info
        port_info = config_db.get_entry("PORT", PORT_A)

        # get the number of ports before removal
        num_of_ports = len(asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT"))

        # remove buffer pg cfg for the port
        # record the buffer pgs before removing them
        pgs = config_db.get_keys('BUFFER_PG')
        buffer_pgs = {}
        for key in pgs:
            if PORT_A in key:
                buffer_pgs[key] = config_db.get_entry('BUFFER_PG', key)
                config_db.delete_entry('BUFFER_PG', key)

        # modify buffer queue entry to egress_lossless_profile instead of egress_lossy_profile
        config_db.update_entry("BUFFER_QUEUE", "%s|0-2"%PORT_A, {"profile": "egress_lossless_profile"})

        # remove buffer queue cfg for the port
        queues = config_db.get_keys('BUFFER_QUEUE')
        buffer_queues = {}
        for key in queues:
            if PORT_A in key:
                buffer_queues[key] = config_db.get_entry('BUFFER_QUEUE', key)
                config_db.delete_entry('BUFFER_QUEUE', key)

        # try to remove this port
        config_db.delete_entry('PORT', PORT_A)
        num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                              num_of_ports-1,
                              polling_config = PollingConfig(polling_interval = 1, timeout = 5.00, strict = True))

        # verify that the port was removed properly since all buffer configuration was removed also
        assert len(num) == num_of_ports - 1

        config_db.create_entry("PORT", PORT_A, port_info)
        # verify that the port has been readded
        num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                              num_of_ports,
                              polling_config = PollingConfig(polling_interval = 1, timeout = 5.00, strict = True))

        assert len(num) == num_of_ports

        # re-add buffer pg and queue cfg to the port
        for key, pg in buffer_pgs.items():
            config_db.update_entry("BUFFER_PG", key, pg)

        for key, queue in buffer_queues.items():
            config_db.update_entry("BUFFER_QUEUE", key, queue)

        # Remove the port with buffer configuration
        config_db.delete_entry('PORT', PORT_A)
        num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                                      num_of_ports-1,
                                      polling_config = PollingConfig(polling_interval = 1, timeout = 5.00, strict = False))

        # verify that the port wasn't removed since we still have buffer cfg
        assert len(num) == num_of_ports

        # Remove buffer pgs
        for key in buffer_pgs.keys():
            config_db.delete_entry('BUFFER_PG', key)

        num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                                      num_of_ports-1,
                                      polling_config = PollingConfig(polling_interval = 1, timeout = 5.00, strict = False))

        # verify that the port wasn't removed since we still have buffer cfg
        assert len(num) == num_of_ports

        # Remove buffer queue
        for key in buffer_queues.keys():
            config_db.delete_entry('BUFFER_QUEUE', key)

        num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                                      num_of_ports-1,
                                      polling_config = PollingConfig(polling_interval = 1, timeout = 5.00, strict = True))

        # verify that the port wasn't removed since we still have buffer cfg
        assert len(num) == num_of_ports - 1


    @pytest.mark.parametrize("scenario", ["one_port", "all_ports"])
    def test_add_remove_all_the_ports(self, dvs, testlog, scenario):
        config_db = dvs.get_config_db()
        state_db = dvs.get_state_db()
        asic_db = dvs.get_asic_db()
   
        # get the number of ports before removal
        num_of_ports = len(asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT"))
        
        # remove buffer pg cfg for the port
        if scenario == "all_ports":
            ports = config_db.get_keys('PORT')
        elif scenario == "one_port":
            ports = [PORT_A]
        else:
            assert False

        ports_info = {}
        
        for i in range(DELETE_CREATE_ITERATIONS):
            # remove ports
            for key in ports:
                # read port info and save it
                ports_info[key] = config_db.get_entry("PORT", key)
                
                # remove a port
                self.dvs_port.remove_port(key)
    
            # verify remove port
            num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                                          num_of_ports-len(ports))
            assert len(num) == num_of_ports-len(ports)
            
            # add port
            """
            DELETE_CREATE_ITERATIONS defines the number of iteration of delete and create to  ports,
            we add different timeouts between delete/create to catch potential race condition that can lead to system crush.
            """
            time.sleep(i%3)
            for key in ports:
                config_db.create_entry("PORT", key, ports_info[key])

            # verify add port            
            num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                                  num_of_ports)
            assert len(num) == num_of_ports
                
            time.sleep((i%2)+1)           
            
        # run ping
        dvs.setup_db()
        dvs.create_vlan("6")
        dvs.create_vlan_member("6", PORT_A)
        dvs.create_vlan_member("6", PORT_B)
        
        port_entry_a = state_db.get_entry("PORT_TABLE",PORT_A)
        port_entry_b = state_db.get_entry("PORT_TABLE",PORT_B)
        port_admin_a = port_entry_a['admin_status']
        port_admin_b = port_entry_b['admin_status']
        
        dvs.set_interface_status("Vlan6", "up")
        dvs.add_ip_address("Vlan6", "6.6.6.1/24")
        dvs.set_interface_status(PORT_A, "up")
        dvs.set_interface_status(PORT_B, "up")
        
        dvs.servers[16].runcmd("ifconfig eth0 6.6.6.6/24 up")
        dvs.servers[16].runcmd("ip route add default via 6.6.6.1")
        dvs.servers[17].runcmd("ifconfig eth0 6.6.6.7/24 up")
        dvs.servers[17].runcmd("ip route add default via 6.6.6.1")
        
        time.sleep(2)
        
        rc = dvs.servers[16].runcmd("ping -c 1 6.6.6.7")
        assert rc == 0

        rc = dvs.servers[17].runcmd("ping -c 1 6.6.6.6")
        assert rc == 0

        dvs.set_interface_status(PORT_A, port_admin_a)
        dvs.set_interface_status(PORT_B, port_admin_b)
        dvs.remove_vlan_member("6", PORT_A)
        dvs.remove_vlan_member("6", PORT_B)
        dvs.remove_vlan("6")
