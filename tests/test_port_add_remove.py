import pytest
import time
from dvslib.dvs_common import PollingConfig

# the port to be removed and add
PORT = "Ethernet0"

@pytest.mark.usefixtures('dvs_port_manager')
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
        
        config_db.create_entry("PORT",PORT, port_info)
        
    def test_add_remove_a_port(self, dvs, testlog):
        config_db = dvs.get_config_db()
        asic_db = dvs.get_asic_db()

        # get port info
        port_info = config_db.get_entry("PORT", PORT)

#         # get the number of ports before removal
#         num_of_ports = len(asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT"))
# 
#         # try to remove this port
#         config_db.delete_entry('PORT', PORT)
#         num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
#                                       num_of_ports-1,
#                                       polling_config = PollingConfig(polling_interval = 1, timeout = 5.00, strict = False))
# 
#         # verify that the port wasn't removed since we still have buffer cfg
#         assert len(num) == num_of_ports
        
        
        # remove buffer pg cfg for the port
        pgs = config_db.get_keys('BUFFER_PG')
        for key in pgs:
            if PORT in key:
                config_db.delete_entry('BUFFER_PG', key)

        # remove buffer queue cfg for the port
        pgs = config_db.get_keys('BUFFER_QUEUE')
        for key in pgs:
            if PORT in key:
                config_db.delete_entry('BUFFER_QUEUE', key)
                
        
        # get the number of ports before removal
        num_of_ports = len(asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT"))
 
        for i in range(1,10):
            # try to remove this port
            config_db.delete_entry('PORT', PORT)
            num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                                          num_of_ports-1,
                                          polling_config = PollingConfig(polling_interval = 1, timeout = 5.00, strict = True))
     
            # verify that the port wasn't removed since we still have buffer cfg
            assert len(num) == num_of_ports - 1
            
            time.sleep((i+1)%3)
            
            config_db.create_entry("PORT",PORT, port_info)
            
            num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                                          num_of_ports)
            
            # verify that the port wasn't removed since we still have buffer cfg
            assert len(num) == num_of_ports
            
            time.sleep(i%3)
     
    def test_add_remove_all_the_ports(self, dvs, testlog):
        config_db = dvs.get_config_db()
        asic_db = dvs.get_asic_db()
     
        # get the number of ports before removal
        num_of_ports = len(asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT"))
        
        # remove buffer pg cfg for the port
        ports = config_db.get_keys('PORT')
        ports_info = {}
        
        for i in range(10):
            
            for key in ports:
                
                # read port info and save it
                ports_info[key] = config_db.get_entry("PORT", key)
                
                # remove a port
                self.dvs_port.remove_port(key)
    
            num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                                          num_of_ports-len(ports))
            assert len(num) == num_of_ports-len(ports)
            
            time.sleep((i%3)+1)
            for key in ports:
                # add port
                config_db.create_entry("PORT",key, ports_info[key])
            
            num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                                  num_of_ports)   
            assert len(num) == num_of_ports
                
            time.sleep((i%2)+2)           
            
        dvs.setup_db()        
        dvs.create_vlan("6")
        dvs.create_vlan_member("6", "Ethernet64")
        dvs.create_vlan_member("6", "Ethernet68")
        dvs.set_interface_status("Vlan6", "up")
        dvs.add_ip_address("Vlan6", "6.6.6.1/24")
        dvs.set_interface_status("Ethernet68", "up")
        dvs.set_interface_status("Ethernet64", "up")
        
        dvs.servers[16].runcmd("ifconfig eth0 6.6.6.6/24 up")
        dvs.servers[16].runcmd("ip route add default via 6.6.6.1")
        dvs.servers[17].runcmd("ifconfig eth0 6.6.6.7/24 up")
        dvs.servers[17].runcmd("ip route add default via 6.6.6.1")
        
        time.sleep(2)
        
        rc = dvs.servers[16].runcmd("ping -c 1 6.6.6.7")
        assert rc == 0
        
        rc = dvs.servers[17].runcmd("ping -c 1 6.6.6.6")
        assert rc == 0

                
                
                
                
                
