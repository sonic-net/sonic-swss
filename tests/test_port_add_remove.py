import pytest
import time
import buffer_model
from dvslib.dvs_common import PollingConfig

# the port to be removed and add
PORT_A = "Ethernet64"
PORT_B = "Ethernet68"

"""
DELETE_CREATE_ITERATIONS defines the number of iteration of delete and create to  ports,
we add different timeouts between delete/create to catch potential race condition that can lead to system crush

Add \ Remove of Buffers can be done only when the model is dynamic.
"""
DELETE_CREATE_ITERATIONS = 10

@pytest.yield_fixture
def dynamic_buffer(dvs):
    buffer_model.enable_dynamic_buffer(dvs.get_config_db(), dvs.runcmd)
    yield
    buffer_model.disable_dynamic_buffer(dvs)


@pytest.mark.skip(reason="Temporarily skip failing tests")
@pytest.mark.usefixtures('dvs_port_manager')
@pytest.mark.usefixtures("dynamic_buffer")    
class TestPortAddRemove(object):

    def set_mmu(self,dvs):
        state_db = dvs.get_state_db()
        # set mmu size
        fvs = {"mmu_size": "12766208"}
        state_db.create_entry("BUFFER_MAX_PARAM_TABLE", "global", fvs)


    def test_remove_add_remove_port_with_buffer_cfg(self, dvs, testlog):
        config_db = dvs.get_config_db()
        asic_db = dvs.get_asic_db()
        state_db = dvs.get_state_db()
        app_db = dvs.get_app_db()

        # set mmu size
        self.set_mmu(dvs)

        # Startup interface
        dvs.port_admin_set(PORT_A, 'up')

        # get port info
        port_info = config_db.get_entry("PORT", PORT_A)

        # get the number of ports before removal
        num_of_ports = len(asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT"))

        # remove buffer pg cfg for the port (record the buffer pgs before removing them)
        pgs = config_db.get_keys('BUFFER_PG')
        buffer_pgs = {}
        for key in pgs:
            if PORT_A in key:
                buffer_pgs[key] = config_db.get_entry('BUFFER_PG', key)
                config_db.delete_entry('BUFFER_PG', key)
                app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", key)

        # modify buffer queue entry to egress_lossless_profile instead of egress_lossy_profile
        config_db.update_entry("BUFFER_QUEUE", "%s|0-2"%PORT_A, {"profile": "egress_lossless_profile"})

        # remove buffer queue cfg for the port
        queues = config_db.get_keys('BUFFER_QUEUE')
        buffer_queues = {}
        for key in queues:
            if PORT_A in key:
                buffer_queues[key] = config_db.get_entry('BUFFER_QUEUE', key)
                config_db.delete_entry('BUFFER_QUEUE', key)
                app_db.wait_for_deleted_entry('BUFFER_QUEUE_TABLE', key)

        # Shutdown interface
        dvs.port_admin_set(PORT_A, 'down')
                
        # try to remove this port
        config_db.delete_entry('PORT', PORT_A)
        num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                              num_of_ports-1,
                              polling_config = PollingConfig(polling_interval = 1, timeout = 5.00, strict = True))

        # verify that the port was removed properly since all buffer configuration was removed also
        assert len(num) == num_of_ports - 1

        # set back the port 
        config_db.update_entry("PORT", PORT_A, port_info)

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

        time.sleep(5)

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
            app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", key)

        num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                                      num_of_ports-1,
                                      polling_config = PollingConfig(polling_interval = 1, timeout = 5.00, strict = False))

        # verify that the port wasn't removed since we still have buffer cfg
        assert len(num) == num_of_ports

        # Remove buffer queue
        for key in buffer_queues.keys():
            config_db.delete_entry('BUFFER_QUEUE', key)
            app_db.wait_for_deleted_entry('BUFFER_QUEUE_TABLE', key)

        num = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT",
                                      num_of_ports-1,
                                      polling_config = PollingConfig(polling_interval = 1, timeout = 5.00, strict = True))

        # verify that the port wasn't removed since we still have buffer cfg
        assert len(num) == num_of_ports - 1

        # set back the port as it is required for next test 
        config_db.update_entry("PORT", PORT_A, port_info)
       


    @pytest.mark.parametrize("scenario", ["one_port", "all_ports"])
    def test_add_remove_all_the_ports(self, dvs, testlog, scenario):
        config_db = dvs.get_config_db()
        state_db = dvs.get_state_db()
        asic_db = dvs.get_asic_db()
        app_db = dvs.get_app_db()

        # set mmu size
        self.set_mmu(dvs)

        # get the number of ports before removal
        num_of_ports = len(asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT"))
        
        # remove buffer pg cfg for the port
        if scenario == "all_ports":
            ports = config_db.get_keys('PORT')
        elif scenario == "one_port":
            ports = [PORT_A]
        else:
            assert False

        # delete all PGs and QUEUEs from the relevant ports 
        pgs = config_db.get_keys('BUFFER_PG')
        queues = config_db.get_keys('BUFFER_QUEUE')

        for port in ports:
            for key in pgs:
                if port in key:
                    config_db.delete_entry('BUFFER_PG', key)
                    app_db.wait_for_deleted_entry('BUFFER_PG_TABLE', key)

            for key in queues:
                if port in key:
                    config_db.delete_entry('BUFFER_QUEUE', key)
                    app_db.wait_for_deleted_entry('BUFFER_QUEUE_TABLE', key)

        ports_info = {}

        for key in ports:
            # read port info and save it
            ports_info[key] = config_db.get_entry("PORT", key)


        for i in range(DELETE_CREATE_ITERATIONS):
            # remove ports
            for key in ports:
                config_db.delete_entry('PORT',key)
                app_db.wait_for_deleted_entry("PORT_TABLE", key)
    
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
                config_db.update_entry("PORT", key, ports_info[key])
                app_db.wait_for_entry('PORT_TABLE',key)

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
        dvs.remove_ip_address("Vlan6", "6.6.6.1/24")
        dvs.remove_vlan("6")


@pytest.mark.skip(reason="Temporarily skip failing tests")
@pytest.mark.usefixtures("dynamic_buffer")
@pytest.mark.usefixtures("dvs_port_manager")
class TestPortAddRemoveDup(object):
    def test_add_remove_with_dup_lanes(self, testlog, dvs):
        config_db = dvs.get_config_db()
        app_db = dvs.get_app_db()
        state_db = dvs.get_state_db()

        # set mmu size
        fvs = {"mmu_size": "12766208"}
        state_db.create_entry("BUFFER_MAX_PARAM_TABLE", "global", fvs)

        # get port count
        port_count = len(self.dvs_port.get_port_ids())

        # get port info
        port_info = config_db.get_entry("PORT", PORT_A)

        # remove buffer pg cfg for the port
        pgs = config_db.get_keys("BUFFER_PG")
        buffer_pgs = {}
        for key in pgs:
            if PORT_A in key:
                buffer_pgs[key] = config_db.get_entry("BUFFER_PG", key)
                config_db.delete_entry("BUFFER_PG", key)
                app_db.wait_for_deleted_entry("BUFFER_PG_TABLE", key.replace(config_db.separator, app_db.separator))

        # remove buffer queue cfg for the port
        queues = config_db.get_keys("BUFFER_QUEUE")
        buffer_queues = {}
        for key in queues:
            if PORT_A in key:
                buffer_queues[key] = config_db.get_entry("BUFFER_QUEUE", key)
                config_db.delete_entry("BUFFER_QUEUE", key)
                app_db.wait_for_deleted_entry("BUFFER_QUEUE_TABLE", key.replace(config_db.separator, app_db.separator))

        # shutdown port
        dvs.port_admin_set(PORT_A, "down")

        # remove port
        self.dvs_port.remove_port_generic(PORT_A)
        self.dvs_port.verify_port_count(port_count-1)

        # make port config with duplicate lanes
        dup_lanes = port_info["lanes"]
        dup_lanes += ",{}".format(port_info["lanes"].split(",")[-1])

        # add port
        self.dvs_port.create_port_generic(PORT_A, dup_lanes, port_info["speed"])
        self.dvs_port.verify_port_count(port_count)

        # shutdown port
        dvs.port_admin_set(PORT_A, "down")

        # remove port
        self.dvs_port.remove_port_generic(PORT_A)
        self.dvs_port.verify_port_count(port_count-1)

        # make port config
        port_lanes = port_info.pop("lanes")
        port_speed = port_info.pop("speed")

        # re-add port
        self.dvs_port.create_port_generic(PORT_A, port_lanes, port_speed, port_info)
        self.dvs_port.verify_port_count(port_count)

        # re-add buffer pg and queue cfg to the port
        for key, pg in buffer_pgs.items():
            config_db.update_entry("BUFFER_PG", key, pg)
            app_db.wait_for_entry("BUFFER_PG_TABLE", key.replace(config_db.separator, app_db.separator))

        for key, queue in buffer_queues.items():
            config_db.update_entry("BUFFER_QUEUE", key, queue)
            app_db.wait_for_entry("BUFFER_QUEUE_TABLE", key.replace(config_db.separator, app_db.separator))


@pytest.mark.skip(reason="Temporarily skip failing tests")
@pytest.mark.usefixtures("dvs_port_manager")
class TestPortAddRemoveInvalidMandatoryParam(object):
    @pytest.mark.parametrize(
        "port,lanes,speed", [
            pytest.param("Ethernet1000", "", "10000", id="empty-lanes-list"),
            pytest.param("Ethernet1004", "1004,x,1006,1007", "10000", id="invalid-lanes-list"),
            pytest.param("Ethernet1008", "1008,1009,1010,1011", "", id="empty-speed"),
            pytest.param("Ethernet1012", "1012,1013,1014,1015", "invalid", id="invalid-speed"),
            pytest.param("Ethernet1016", "1016,1017,1018,1019", "0", id="out-of-range-speed")
        ]
    )
    def test_add_remove_neg(self, testlog, port, lanes, speed):
        # get port count
        port_asicdb_count = len(self.dvs_port.get_port_ids(dbid=self.dvs_port.ASIC_DB))
        port_appdb_count = len(self.dvs_port.get_port_ids(dbid=self.dvs_port.APPL_DB))

        # add port
        self.dvs_port.create_port_generic(port, lanes, speed)
        self.dvs_port.verify_port_count(port_appdb_count+1, self.dvs_port.APPL_DB)
        self.dvs_port.verify_port_count(port_asicdb_count, self.dvs_port.ASIC_DB)

        # remove port
        self.dvs_port.remove_port_generic(port)
        self.dvs_port.verify_port_count(port_appdb_count, self.dvs_port.APPL_DB)
        self.dvs_port.verify_port_count(port_asicdb_count, self.dvs_port.ASIC_DB)


@pytest.mark.skip(reason="Temporarily skip failing tests")
@pytest.mark.usefixtures("dvs_port_manager")
class TestPortAddRemoveInvalidSerdesParam(object):
    @pytest.fixture(scope="class")
    def port_attr(self):
        meta_dict = {
            "port": "Ethernet1000",
            "lanes": "1000,1001,1002,1003",
            "speed": "100000",
            "port_asicdb_count": len(self.dvs_port.get_port_ids(dbid=self.dvs_port.ASIC_DB)),
            "port_appdb_count": len(self.dvs_port.get_port_ids(dbid=self.dvs_port.APPL_DB))
        }
        yield meta_dict

    def verify_add_remove(self, attr, qualifiers):
        # add port
        self.dvs_port.create_port_generic(attr["port"], attr["lanes"], attr["speed"], qualifiers)
        self.dvs_port.verify_port_count(attr["port_appdb_count"]+1, self.dvs_port.APPL_DB)
        self.dvs_port.verify_port_count(attr["port_asicdb_count"], self.dvs_port.ASIC_DB)

        # remove port
        self.dvs_port.remove_port_generic(attr["port"])
        self.dvs_port.verify_port_count(attr["port_appdb_count"], self.dvs_port.APPL_DB)
        self.dvs_port.verify_port_count(attr["port_asicdb_count"], self.dvs_port.ASIC_DB)

    @pytest.mark.parametrize(
        "serdes", [
            pytest.param("preemphasis", id="preemphasis"),
            pytest.param("idriver", id="idriver"),
            pytest.param("ipredriver", id="ipredriver"),
            pytest.param("pre1", id="pre1"),
            pytest.param("pre2", id="pre2"),
            pytest.param("pre3", id="pre3"),
            pytest.param("main", id="main"),
            pytest.param("post1", id="post1"),
            pytest.param("post2", id="post2"),
            pytest.param("post3", id="post3"),
            pytest.param("attn", id="attn")
        ]
    )
    def test_add_remove_neg(self, testlog, port_attr, serdes):
        qualifiers = { serdes: "" }
        self.verify_add_remove(port_attr, qualifiers)

        qualifiers = { serdes: "invalid" }
        self.verify_add_remove(port_attr, qualifiers)


@pytest.mark.skip(reason="Temporarily skip failing tests")
@pytest.mark.usefixtures("dvs_port_manager")
class TestPortAddRemoveInvalidParam(object):
    def verify_add_remove(self, qualifiers):
        port = "Ethernet1000"
        lanes = "1000,1001,1002,1003"
        speed = "100000"

        # get port count
        port_asicdb_count = len(self.dvs_port.get_port_ids(dbid=self.dvs_port.ASIC_DB))
        port_appdb_count = len(self.dvs_port.get_port_ids(dbid=self.dvs_port.APPL_DB))

        # add port
        self.dvs_port.create_port_generic(port, lanes, speed, qualifiers)
        self.dvs_port.verify_port_count(port_appdb_count+1, self.dvs_port.APPL_DB)
        self.dvs_port.verify_port_count(port_asicdb_count, self.dvs_port.ASIC_DB)

        # remove port
        self.dvs_port.remove_port_generic(port)
        self.dvs_port.verify_port_count(port_appdb_count, self.dvs_port.APPL_DB)
        self.dvs_port.verify_port_count(port_asicdb_count, self.dvs_port.ASIC_DB)

    def test_add_remove_neg_alias(self, testlog):
        qualifiers = { "alias": "" }
        self.verify_add_remove(qualifiers)

    def test_add_remove_neg_index(self, testlog):
        qualifiers = { "index": "" }
        self.verify_add_remove(qualifiers)

        qualifiers = { "index": "invalid" }
        self.verify_add_remove(qualifiers)

    def test_add_remove_neg_autoneg(self, testlog):
        qualifiers = { "autoneg": "" }
        self.verify_add_remove(qualifiers)

        qualifiers = { "autoneg": "invalid" }
        self.verify_add_remove(qualifiers)

    def test_add_remove_neg_adv_speeds(self, testlog):
        qualifiers = { "adv_speeds": "" }
        self.verify_add_remove(qualifiers)

        qualifiers = { "adv_speeds": "0" }
        self.verify_add_remove(qualifiers)

        qualifiers = { "adv_speeds": "invalid" }
        self.verify_add_remove(qualifiers)

    def test_add_remove_neg_interface_type(self, testlog):
        qualifiers = { "interface_type": "" }
        self.verify_add_remove(qualifiers)

        qualifiers = { "interface_type": "invalid" }
        self.verify_add_remove(qualifiers)

    def test_add_remove_neg_adv_interface_types(self, testlog):
        qualifiers = { "adv_interface_types": "" }
        self.verify_add_remove(qualifiers)

        qualifiers = { "adv_interface_types": "invalid" }
        self.verify_add_remove(qualifiers)

    def test_add_remove_neg_fec(self, testlog):
        qualifiers = { "fec": "" }
        self.verify_add_remove(qualifiers)

        qualifiers = { "fec": "invalid" }
        self.verify_add_remove(qualifiers)

    def test_add_remove_neg_mtu(self, testlog):
        qualifiers = { "mtu": "" }
        self.verify_add_remove(qualifiers)

        qualifiers = { "mtu": "0" }
        self.verify_add_remove(qualifiers)

        qualifiers = { "mtu": "invalid" }
        self.verify_add_remove(qualifiers)

    def test_add_remove_neg_tpid(self, testlog):
        qualifiers = { "tpid": "" }
        self.verify_add_remove(qualifiers)

        qualifiers = { "tpid": "invalid" }
        self.verify_add_remove(qualifiers)

    def test_add_remove_neg_pfc_asym(self, testlog):
        qualifiers = { "pfc_asym": "" }
        self.verify_add_remove(qualifiers)

        qualifiers = { "pfc_asym": "invalid" }
        self.verify_add_remove(qualifiers)

    def test_add_remove_neg_learn_mode(self, testlog):
        qualifiers = { "learn_mode": "" }
        self.verify_add_remove(qualifiers)

        qualifiers = { "learn_mode": "invalid" }
        self.verify_add_remove(qualifiers)

    def test_add_remove_neg_link_training(self, testlog):
        qualifiers = { "link_training": "" }
        self.verify_add_remove(qualifiers)

        qualifiers = { "link_training": "invalid" }
        self.verify_add_remove(qualifiers)

    def test_add_remove_neg_role(self, testlog):
        qualifiers = { "role": "" }
        self.verify_add_remove(qualifiers)

        qualifiers = { "role": "invalid" }
        self.verify_add_remove(qualifiers)

    def test_add_remove_neg_admin_status(self, testlog):
        qualifiers = { "admin_status": "" }
        self.verify_add_remove(qualifiers)

        qualifiers = { "admin_status": "invalid" }
        self.verify_add_remove(qualifiers)
