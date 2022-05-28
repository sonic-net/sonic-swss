from swsscommon import swsscommon
from dvslib.dvs_database import DVSDatabase
import ast

class TestVirtualChassis(object):

    def set_lag_id_boundaries(self, vct):
        """This functions sets lag id boundaries in the chassis app db.
        
        In VOQ systems the lag id boundaries need to be set before configuring any PortChannels.
        The lag id boundaries are used by lag id allocator while adding a PortChannel to the asic db.
        Note:
            In real systems, the lag id boundries are taken from a platform specific file. For testing
            we assume the chassis capability with maximum 2 lags so that we can test the lag id allocator
            table full error with less number of PortChannel configuration
        """

        dvss = vct.dvss
        for name in dvss.keys():
            if name.startswith("supervisor"):
                dvs = dvss[name]
                chassis_app_db = DVSDatabase(swsscommon.CHASSIS_APP_DB, dvs.redis_chassis_sock)
                chassis_app_db.db_connection.set("SYSTEM_LAG_ID_START", "1")
                chassis_app_db.db_connection.set("SYSTEM_LAG_ID_END", "2")
                break
            
    def config_inbandif_port(self, vct, ibport):
        """This function configures port type inband interface in each linecard"""

        dvss = vct.dvss
        for name in dvss.keys():
            dvs = dvss[name]
            # Get the config info
            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")

            cfg_switch_type = metatbl.get("switch_type")

            # Configure only for line cards
            if cfg_switch_type == "voq":
                dvs.runcmd(f"config interface startup {ibport}")
                config_db.create_entry("VOQ_INBAND_INTERFACE", f"{ibport}", {"inband_type": "port"})
                
    def del_inbandif_port(self, vct, ibport):
        """This function deletes existing port type inband interface"""

        dvss = vct.dvss
        for name in dvss.keys():
            dvs = dvss[name]
            # Get the config info
            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")

            cfg_switch_type = metatbl.get("switch_type")

            # Applicable only for line cards
            if cfg_switch_type == "voq":
                config_db.delete_entry("VOQ_INBAND_INTERFACE", f"{ibport}")
                
    def test_connectivity(self, vct):
        if vct is None:
            return
        dvss = vct.dvss
        nbrs = vct.get_topo_neigh()
        for name in dvss.keys():
            dv = dvss[name]
            # ping all vs's inband address
            for ctn in vct.inbands.keys():
                ip = vct.inbands[ctn]["inband_address"]
                ip = ip.split("/")[0]
                print("%s: ping inband address %s" % (name, ip))
                _, out = dv.runcmd(['sh', "-c", "ping -c 5 -W 0 -q %s" % ip])
                print(out)
                assert '5 received' in out
            if name not in nbrs.keys():
                continue
            for item in nbrs[name]:
                ip = str(item[1])
                print("%s: ping neighbor address %s" % (name, ip))
                _, out = dv.runcmd(['sh', "-c", "ping -c 5 -W 0 -q %s" % ip])
                print(out)
                assert '5 received' in out

    def test_voq_switch(self, vct):
        """Test VOQ switch objects configuration.

        This test validates configuration of switch creation objects required for
        VOQ switches. The switch_type, max_cores and switch_id attributes configuration
        are verified. For the System port config list, it is verified that all the
        configured system ports are avaiable in the asic db by checking the count.
        """

        if vct is None:
            return

        dvss = vct.dvss
        for name in dvss.keys():
            dvs = dvss[name]
            # Get the config info
            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")

            cfg_switch_type = metatbl.get("switch_type")

            # Test only for line cards
            if cfg_switch_type == "voq":
                print("VOQ Switch test for {}".format(name))
                cfg_switch_id = metatbl.get("switch_id")
                assert cfg_switch_id != "", "Got error in getting switch_id from CONFIG_DB DEVICE_METADATA"

                cfg_max_cores = metatbl.get("max_cores")
                assert cfg_max_cores != "", "Got error in getting max_cores from CONFIG_DB DEVICE_METADATA"

                cfgspkeys = config_db.get_keys("SYSTEM_PORT")
                sp_count = len(cfgspkeys)

                asic_db = dvs.get_asic_db()
                keys = asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")
                switch_oid_key = keys[0]

                switch_entry = asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_SWITCH", switch_oid_key)

                value = switch_entry.get("SAI_SWITCH_ATTR_TYPE")
                assert value == "SAI_SWITCH_TYPE_VOQ", "Switch type is not VOQ"

                value = switch_entry.get("SAI_SWITCH_ATTR_SWITCH_ID")
                assert value == cfg_switch_id, "VOQ switch id is invalid"

                value = switch_entry.get("SAI_SWITCH_ATTR_MAX_SYSTEM_CORES")
                assert value == cfg_max_cores, "Max system cores is invalid"

                value = switch_entry.get("SAI_SWITCH_ATTR_SYSTEM_PORT_CONFIG_LIST")
                assert value != "", "Empty system port config list"
                # Convert the spcfg string to dictionary
                spcfg = ast.literal_eval(value)
                assert spcfg['count'] == sp_count, "Number of systems ports configured is invalid"

    def test_chassis_app_db_sync(self, vct):
        """Test chassis app db syncing.

        This test is for verifying the database sync mechanism. With the virtual chassis
        setup, it is verified that at least one database entry is synced from line card to
        supervisor card. An interface entry is used as sample database entry for verification
        of syncing mechanism.
        """

        if vct is None:
            return

        dvss = vct.dvss
        for name in dvss.keys():
            if name.startswith("supervisor"):
                dvs = dvss[name]
                chassis_app_db = DVSDatabase(swsscommon.CHASSIS_APP_DB, dvs.redis_chassis_sock)
                keys = chassis_app_db.get_keys("SYSTEM_INTERFACE")
                assert len(keys), "No chassis app db syncing is done"

    def test_chassis_system_interface(self, vct):
        """Test RIF record creation in ASIC_DB for remote interfaces.

        This test verifies RIF programming in ASIC_DB for remote interface. The orchagent
        creates RIF record for system port interfaces from other line cards. It is verified
        by retrieving a RIF record from local ASIC_DB that corresponds to a remote system port
        and checking that the switch id of that remote system port does not match the local asic
        switch id.
        """

        if vct is None:
            return 

        dvss = vct.dvss
        for name in dvss.keys():
            dvs = dvss[name]

            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")

            cfg_switch_type = metatbl.get("switch_type")

            # Test only for line cards
            if cfg_switch_type == "voq":
                lc_switch_id = metatbl.get("switch_id")
                assert lc_switch_id != "", "Got error in getting switch_id from CONFIG_DB DEVICE_METADATA"
                if lc_switch_id == "0":
                    # Testing in Linecard1, In Linecard1 there will be RIF for Ethernet12 from Linecard3
                    # Note: Tesing can be done in any linecard for RIF of any system port interface.
                    #       Here testing is done on linecard with switch id 0
                    asic_db = dvs.get_asic_db()
                    keys = asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE")
                    assert len(keys), "No router interfaces in ASIC_DB"

                    rif_port_oid = ""
                    for key in keys:
                        rif_entry = asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE", key)
                        value = rif_entry.get("SAI_ROUTER_INTERFACE_ATTR_TYPE")
                        assert value != "", "Got error in getting RIF type"
                        if value == "SAI_ROUTER_INTERFACE_TYPE_PORT":
                            value = rif_entry.get("SAI_ROUTER_INTERFACE_ATTR_PORT_ID")
                            assert value != "", "Got error in getting RIF port"
                            if value.startswith("oid:0x5d"):
                                # System port RIF, this is used as key for system port config info retrieval
                                rif_port_oid = value
                                break

                    assert rif_port_oid != "", "No RIF records for remote interfaces in ASIC_DB"
                    # Validate if the system port is from valid switch
                    sp_entry = asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_SYSTEM_PORT", rif_port_oid)
                    value = sp_entry.get("SAI_SYSTEM_PORT_ATTR_CONFIG_INFO")
                    assert value != "", "Got error in getting system port config info for rif system port"
                    spcfginfo = ast.literal_eval(value)
                    # Remote system ports's switch id should not match local switch id
                    assert spcfginfo["attached_switch_id"] != lc_switch_id, "RIF system port with wrong switch_id"

    def test_chassis_system_neigh(self, vct):
        """Test neigh record create/delete and syncing to chassis app db.

        This test validates that:
           (i)   Local neighbor entry is created with encap index
           (ii)  Local neighbor is synced to chassis ap db with assigned encap index
           (iii) Remote neighbor entry is created in ASIC_DB with received encap index
           (iv)  Local neighbor entry is deleted when neighbor is deleted
           (v)   Local neighbor delete is synced to chassis ap db
           (vi)  Remote neighbor entry is cleared in ASIC_DB
        """
        
        if vct is None:
            return

        # We use Ethernet0 as inband port in each line card. In real hardware, this will be a
        # special port used for inband. For testing purpose, we need port record and rif record
        # for the inband interface and valid kernel interface. Since Ethernet0 is already 
        # setup, the port record, rif record and kernel interface already exist. So we use it
        # for testing
        inband_port = "Ethernet0"

        # Configure port type inband interface
        self.config_inbandif_port(vct, inband_port)

        # Test neighbor on Ethernet4 since Ethernet0 is used as Inband port
        test_neigh_dev = "Ethernet4"
        test_neigh_ip = "10.8.104.3"
        test_neigh_mac = "00:01:02:03:04:05"

        dvss = vct.dvss
        print("name {}".format(dvss.keys()))
        for name in dvss.keys():
            dvs = dvss[name]

            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")

            cfg_switch_type = metatbl.get("switch_type")

            # Neighbor record verifiation done in line card
            if cfg_switch_type == "voq":
                lc_switch_id = metatbl.get("switch_id")
                assert lc_switch_id != "", "Got error in getting switch_id from CONFIG_DB DEVICE_METADATA"
                if lc_switch_id == "0":

                    # Add a static neighbor
                    _, res = dvs.runcmd(['sh', "-c", "ip neigh show"])
                    _, res = dvs.runcmd(['sh', "-c", f"ip neigh add {test_neigh_ip} lladdr {test_neigh_mac} dev {test_neigh_dev}"])
                    assert res == "", "Error configuring static neigh"

                    asic_db = dvs.get_asic_db()
                    asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", 1)
                    neighkeys = asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
                    assert len(neighkeys), "No neigh entries in ASIC_DB"

                    # Check for presence of the neighbor in ASIC_DB
                    test_neigh = ""
                    for nkey in neighkeys:
                        ne = ast.literal_eval(nkey)
                        if ne['ip'] == test_neigh_ip:
                            test_neigh = nkey
                            break

                    assert test_neigh != "", "Neigh not found in ASIC_DB"

                    # Preserve test neigh asic db key for delete verification later
                    test_neigh_asic_db_key = test_neigh

                    # Check for presence of encap index, retrieve and store it for sync verification
                    test_neigh_entry = asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", test_neigh)
                    test_neigh_entry_attrs = asic_db.wait_for_fields("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", test_neigh, ["SAI_NEIGHBOR_ENTRY_ATTR_ENCAP_INDEX"])
                    print(test_neigh)
                    print(test_neigh_entry)
                    print(test_neigh_entry_attrs)
                    encap_index = test_neigh_entry_attrs["SAI_NEIGHBOR_ENTRY_ATTR_ENCAP_INDEX"]
                    assert encap_index != "" and encap_index != None, "VOQ encap index is not programmed in ASIC_DB"

                    break

        # Verify neighbor record syncing with encap index
        for name in dvss.keys():
            if name.startswith("supervisor"):
                dvs = dvss[name]
                chassis_app_db = DVSDatabase(swsscommon.CHASSIS_APP_DB, dvs.redis_chassis_sock)
                chassis_app_db.wait_for_n_keys("SYSTEM_NEIGH", 1)
                sysneighkeys = chassis_app_db.get_keys("SYSTEM_NEIGH")

                print(sysneighkeys)
                test_sysneigh = ""
                for sysnk in sysneighkeys:
                    sysnk_tok = sysnk.split("|")
                    assert len(sysnk_tok) == 3, "Invalid system neigh key in chassis app db"
                    if sysnk_tok[2] == test_neigh_ip:
                        test_sysneigh = sysnk
                        break

                assert test_sysneigh != "", "Neigh is not sync-ed to chassis app db"

                # Preserve test sys neigh chassis app db key for delete verification later
                test_sysneigh_chassis_app_db_key = test_sysneigh

                test_sysneigh_entry = chassis_app_db.get_entry("SYSTEM_NEIGH", test_sysneigh)
                sys_neigh_encap_index = test_sysneigh_entry.get("encap_index")
                assert sys_neigh_encap_index != "", "System neigh in chassis app db does not have encap index"

                assert encap_index == sys_neigh_encap_index, "Encap index not sync-ed correctly"

                break

        # Verify programming of remote neighbor in asic db and programming of static route and static
        # neigh in the kernel for the remote neighbor. The neighbor created in linecard 1  will be a 
        # remote neighbor in other linecards. Verity existence of the test neighbor in  linecards other 
        # than linecard 1
        for name in dvss.keys():
            dvs = dvss[name]

            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")

            cfg_switch_type = metatbl.get("switch_type")

            # Neighbor record verifiation done in line card
            if cfg_switch_type == "voq":    
                lc_switch_id = metatbl.get("switch_id")
                assert lc_switch_id != "", "Got error in getting switch_id from CONFIG_DB DEVICE_METADATA"
                if lc_switch_id != "0":
                    # Linecard other than linecard 1
                    asic_db = dvs.get_asic_db()
                    asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", 1)
                    neighkeys = asic_db.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY")
                    assert len(neighkeys), "No neigh entries in ASIC_DB"
                    
                    # Check for presence of the remote neighbor in ASIC_DB
                    remote_neigh = ""
                    for nkey in neighkeys:
                        ne = ast.literal_eval(nkey)
                        if ne['ip'] == test_neigh_ip:
                            remote_neigh = nkey
                            break
                        
                    assert remote_neigh != "", "Remote neigh not found in ASIC_DB"

                    # Preserve remote neigh asic db neigh key for delete verification later
                    test_remote_neigh_asic_db_key = remote_neigh
                    
                    # Check for kernel entries

                    _, output = dvs.runcmd("ip neigh show")
                    assert f"{test_neigh_ip} dev {inband_port}" in output, "Kernel neigh not found for remote neighbor"

                    _, output = dvs.runcmd("ip route show")
                    assert f"{test_neigh_ip} dev {inband_port} scope link" in output, "Kernel route not found for remote neighbor"
                   
                    # Check for ASIC_DB entries. 

                    # Check for presence of encap index, retrieve and store it for sync verification
                    remote_neigh_entry = asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", remote_neigh)
                    
                    # Validate encap index
                    remote_encap_index = remote_neigh_entry.get("SAI_NEIGHBOR_ENTRY_ATTR_ENCAP_INDEX")
                    assert remote_encap_index != "", "VOQ encap index is not programmed for remote neigh in ASIC_DB"
                    assert remote_encap_index == encap_index, "Encap index of remote neigh mismatch with allocated encap index"
                    
                    # Validate MAC
                    mac = remote_neigh_entry.get("SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS")
                    assert mac != "", "MAC address is not programmed for remote neigh in ASIC_DB"
                    assert mac == test_neigh_mac, "Encap index of remote neigh mismatch with allocated encap index"
                    
                    # Check for other mandatory attributes
                    # For remote neighbors, is_local must be "false" 
                    is_local = remote_neigh_entry.get("SAI_NEIGHBOR_ENTRY_ATTR_IS_LOCAL")
                    assert is_local != "", "is_local attribute is not programmed for remote neigh in ASIC_DB"
                    assert is_local == "false", "is_local attribute is true for remote neigh"
                    
                    break

        # Verify system neighbor delete and clearing
        for name in dvss.keys():
            dvs = dvss[name]

            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")

            cfg_switch_type = metatbl.get("switch_type")

            # Neighbor record verifiation done in line card
            if cfg_switch_type == "voq":    
                lc_switch_id = metatbl.get("switch_id")
                assert lc_switch_id != "", "Got error in getting switch_id from CONFIG_DB DEVICE_METADATA"
                if lc_switch_id == "0":

                    # Delete the static neighbor neighbor
                    _, res = dvs.runcmd(['sh', "-c", f"ip neigh del {test_neigh_ip} dev {test_neigh_dev}"])
                    assert res == "", "Error deleting static neigh"

                    # Check for presence of the neighbor in ASIC_DB. The deleted neighbor should
                    # not be present in the asic db
                    asic_db = dvs.get_asic_db()
                    neighkeys = asic_db.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", test_neigh_asic_db_key)
                    assert len(neighkeys) == 0, "Stale neigh entry found in ASIC_DB"

                    break
                    
        # Verify syncing of neighbor record delete in chassis app db
        dvss = vct.dvss
        for name in dvss.keys():
            if name.startswith("supervisor"):
                dvs = dvss[name]
                chassis_app_db = DVSDatabase(swsscommon.CHASSIS_APP_DB, dvs.redis_chassis_sock)
                sysneighkeys = chassis_app_db.wait_for_deleted_entry("SYSTEM_NEIGH", test_sysneigh_chassis_app_db_key)
                assert len(sysneighkeys) == 0, "Stale neigh entry in chassis app db"

                break
            
        # Verify clearing of remote neighbor in non-owner linecard
        dvss = vct.dvss
        for name in dvss.keys():
            dvs = dvss[name]

            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")

            cfg_switch_type = metatbl.get("switch_type")

            # Neighbor record verifiation done in line card
            if cfg_switch_type == "voq":    
                lc_switch_id = metatbl.get("switch_id")
                assert lc_switch_id != "", "Got error in getting switch_id from CONFIG_DB DEVICE_METADATA"
                if lc_switch_id != "0":
                    # Linecard other than linecard 1

                    # Check for presence of the remote neighbor in ASIC_DB. The remote neighbor corresponding
                    # to the deleted static neigbor should not be present
                    asic_db = dvs.get_asic_db()
                    neighkeys = asic_db.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY", test_remote_neigh_asic_db_key)
                    assert len(neighkeys) == 0, "Stale remote neigh in ASIC_DB"
                    
                    # Check for kernel entries. Kernel entries (neigh and route) should have been removed

                    _, output = dvs.runcmd("ip neigh show")
                    assert f"{test_neigh_ip} dev {inband_port}" not in output, "Kernel neigh of remote neighbor not removed"

                    _, output = dvs.runcmd("ip route show")
                    assert f"{test_neigh_ip} dev {inband_port} scope link" not in output, "Kernel route of remote neighbor not removed"
                    
                    break

        # Cleanup inband if configuration
        self.del_inbandif_port(vct, inband_port)
        
    def test_chassis_system_lag(self, vct):
        """Test PortChannel in VOQ based chassis systems.
        
        This test validates that
           (i)   PortChannel is created in local asic with system port aggregator id (system lag id)
                     - Unique lag id is allocated from chassis app db in supervisor card
                     - The unique lag id is sent in system port aggregator id attribute
           (ii)  PortChannel members are successfully added in the PortChannel created 
           (iii) Local PortChannel is synced in chassis app db
           (iv)  PortChannel members addition is synced in the chassis app db
           (v)   System LAG is created for the remote PortChannel with system lag id.
           (vi)  System LAG of remote Portchannel has members with system port id
        """
        
        if vct is None:
            return
       
        test_lag1_name = "PortChannel0001" 
        test_lag1_member = "Ethernet4"

        # Set the lag id boundaries in the chassis ap db
        self.set_lag_id_boundaries(vct)
        
        # Create a PortChannel in a line card 1 (owner line card)
        dvss = vct.dvss
        for name in dvss.keys():
            dvs = dvss[name]

            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")
            
            # Get the host name and asic name for the system lag alias verification
            cfg_hostname = metatbl.get("hostname")
            assert cfg_hostname != "", "Got error in getting hostname from CONFIG_DB DEVICE_METADATA"

            cfg_asic_name = metatbl.get("asic_name")
            assert cfg_asic_name != "", "Got error in getting asic_name from CONFIG_DB DEVICE_METADATA"

            cfg_switch_type = metatbl.get("switch_type")

            # Portchannel record verifiation done in line card
            if cfg_switch_type == "voq":    
                lc_switch_id = metatbl.get("switch_id")
                assert lc_switch_id != "", "Got error in getting switch_id from CONFIG_DB DEVICE_METADATA"
                if lc_switch_id == "0":

                    # Connect to app db: lag table and lag member table 
                    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
                    psTbl_lag = swsscommon.ProducerStateTable(app_db, "LAG_TABLE")
                    psTbl_lagMember = swsscommon.ProducerStateTable(app_db, "LAG_MEMBER_TABLE")

                    # Create PortChannel
                    fvs = swsscommon.FieldValuePairs([("admin", "up"), ("mtu", "9100")])
                    psTbl_lag.set(f"{test_lag1_name}", fvs)
                    
                    # Verify creation of the PorChannel with voq system port aggregator id in asic db
                    asic_db = dvs.get_asic_db()
                    lagkeys = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 1)
                    assert len(lagkeys) == 1, "The LAG entry for configured PortChannel is not available in asic db"
                    
                    # Check for the presence of voq system port aggregate id attribute
                    lag_entry = asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_LAG", lagkeys[0])
                    spa_id = lag_entry.get("SAI_LAG_ATTR_SYSTEM_PORT_AGGREGATE_ID")
                    assert spa_id != "", "VOQ System port aggregate id not present for the LAG"
                    
                    # Add port channel member
                    fvs = swsscommon.FieldValuePairs([("status", "enabled")])
                    psTbl_lagMember.set(f"{test_lag1_name}:{test_lag1_member}", fvs)
                    
                    # Check for presence of lag member added
                    lagmemberkeys = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER", 1)
                    assert len(lagmemberkeys) == 1, "The LAG member for configured PortChannel is not available in asic db"
                    
                    break
                
        # Check syncing of the PortChannel and PortChannel member in chasiss app db
        for name in dvss.keys():
            if name.startswith("supervisor"):
                dvs = dvss[name]
                chassis_app_db = DVSDatabase(swsscommon.CHASSIS_APP_DB, dvs.redis_chassis_sock)
                syslagkeys = chassis_app_db.wait_for_n_keys("SYSTEM_LAG_TABLE", 1)
                assert len(syslagkeys) == 1, "System lag entry is not available in chassis app db"
               
                # system lag alias (key) should be unique across chassis. To ensure such uniqueness,
                # the system lag name is derived from hostname, asic_name and PortChannel name
                # Verify for correct name
                assert f"{cfg_hostname}|{cfg_asic_name}|{test_lag1_name}" in syslagkeys[0], "Invalid unique system lag name"
                
                # Verify lag id of the system lag in chassis app db
                syslag_entry = chassis_app_db.get_entry("SYSTEM_LAG_TABLE", syslagkeys[0])
                remote_lag_id = syslag_entry.get("lag_id")
                assert remote_lag_id != "", "Lag id is not present in the sytem lag table in chassis app db"
                # This id must be same as the id allocated in owner linecard.
                assert remote_lag_id == spa_id, "System lag id in chassis app db is not same as allocated lag id"
                    
                syslagmemberkeys = chassis_app_db.wait_for_n_keys("SYSTEM_LAG_MEMBER_TABLE", 1)
                assert len(syslagmemberkeys) == 1, "No system lag member entries in chassis app db"
                
                break
                
        # Verify programming of remote system lag with received system lag id in non-owner line card
        # Verify programming of lag menbers with system port id in non-owner line card
        for name in dvss.keys():
            dvs = dvss[name]

            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")

            cfg_switch_type = metatbl.get("switch_type")

            # System LAG info verifiation done in non-owner line card
            if cfg_switch_type == "voq":    
                lc_switch_id = metatbl.get("switch_id")
                assert lc_switch_id != "", "Got error in getting switch_id from CONFIG_DB DEVICE_METADATA"
                if lc_switch_id != "0":
                    # Linecard other than linecard 1 (owner line card)
                    asic_db = dvs.get_asic_db()
                    remotesyslagkeys = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 1)
                    assert len(remotesyslagkeys) == 1, "No remote system lag entries in ASIC_DB"
                    
                    remotesyslag_entry = asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_LAG", remotesyslagkeys[0])
                    remote_lag_id = remotesyslag_entry.get("SAI_LAG_ATTR_SYSTEM_PORT_AGGREGATE_ID")
                    assert remote_lag_id != "", "Lag id not present in the remote syslag entry in asic db"
                    assert remote_lag_id == spa_id, "Remote system lag programmed with wrong lag id"
                    
                    # Verify remote system lag has system port as member
                    lagmemberkeys = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER", 1)
                    assert len(lagmemberkeys) == 1, "The LAG member for remote system lag is not available in asic db"
                    
                    remotelagmember_entry = asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER", lagmemberkeys[0])
                    member_port_id = remotelagmember_entry.get("SAI_LAG_MEMBER_ATTR_PORT_ID")
                    #Verify that the member is a system port
                    assert "oid:0x5d" in member_port_id, "System LAG member is not system port"
                    
                    break

    def test_chassis_system_lag_id_allocator_table_full(self, vct):
        """Test lag id allocator table full.
        
        Pre-requisite: 
            (i) Test case: test_chassis_system_lag
        This test validates that
            (i)  If PortChannel configuration goes beyond the platfrom capacitty boundary, lag id
                 allocator returns table full error
        """
        
        if vct is None:
            return
       
        test_lag2_name = "PortChannel0002" 
        test_lag3_name = "PortChannel0003" 

        # Create a PortChannel in a line card 1 (owner line card)
        dvss = vct.dvss
        for name in dvss.keys():
            dvs = dvss[name]

            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")
            
            # Get the host name and asic name for the system lag alias verification
            cfg_hostname = metatbl.get("hostname")
            assert cfg_hostname != "", "Got error in getting hostname from CONFIG_DB DEVICE_METADATA"

            cfg_asic_name = metatbl.get("asic_name")
            assert cfg_asic_name != "", "Got error in getting asic_name from CONFIG_DB DEVICE_METADATA"

            cfg_switch_type = metatbl.get("switch_type")

            # Portchannel record verifiation done in line card
            if cfg_switch_type == "voq":    
                lc_switch_id = metatbl.get("switch_id")
                assert lc_switch_id != "", "Got error in getting switch_id from CONFIG_DB DEVICE_METADATA"
                if lc_switch_id == "0":
                    
                    # Connect to app db: lag table
                    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
                    psTbl_lag = swsscommon.ProducerStateTable(app_db, "LAG_TABLE")

                    # Create PortChannel 2. This should be successfully configured
                    fvs = swsscommon.FieldValuePairs([("admin", "up"), ("mtu", "9100")])
                    psTbl_lag.set(f"{test_lag2_name}", fvs)
                    
                    # Verify creation of the PorChannels with voq system port aggregator id in asic db
                    asic_db = dvs.get_asic_db()
                    lagkeys = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 2)
                    assert len(lagkeys) == 2, "Two configured LAG entries are not available in asic db"
                    
                    # Check for the presence of voq system port aggregate id attribute for 2 LAGs
                    lag_entry = asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_LAG", lagkeys[0])
                    spa_id = lag_entry.get("SAI_LAG_ATTR_SYSTEM_PORT_AGGREGATE_ID")
                    assert spa_id != "", "VOQ System port aggregate id not present for the LAG 1"
                    
                    lag_entry = asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_LAG", lagkeys[1])
                    spa_id = lag_entry.get("SAI_LAG_ATTR_SYSTEM_PORT_AGGREGATE_ID")
                    assert spa_id != "", "VOQ System port aggregate id not present for the LAG 2"

                    # Create PortChannel 3. This should not be configured since lag id limit reached
                    fvs = swsscommon.FieldValuePairs([("admin", "up"), ("mtu", "9100")])
                    psTbl_lag.set(f"{test_lag3_name}", fvs)

                    # Check syslog for the table full error
                    marker = "ERR #orchagent"
                    srch_str = f"addLag: Failed to allocate unique LAG id for local lag {test_lag3_name} rv:-1"
                    _, num =  dvs.runcmd(["sh", "-c", "awk '/%s/,ENDFILE {print;}' /var/log/syslog \
                                        | grep \"%s\" | wc -l" % (marker, srch_str)])
                    assert num.strip() == '1', "LAG ID allocator table full error is not returned"

                    # Clean up the app db for the PortChannel creation failure
                    psTbl_lag.delete(f"{test_lag3_name}")
                    
                    break

    def test_chassis_system_lag_id_allocator_del_id(self, vct):
        """Test lag id allocator's release id and re-use id processing.
        
        Pre-requisite: 
            (i)  Test case: test_chassis_system_lag
            (ii) Test case: test_chassis_system_lag_id_allocator_table_full
        This test validates that
            (i)   Portchannel is deleted and id allocator does not return error
            (ii)  Should be able to add PortChannel to re-use released id
            (iii) Deleted portchaneels are removed from chassis app db
            (iv)  Remote asics remove the system lag corresponding to the deleted PortChannels
        """
        
        if vct is None:
            return
       
        test_lag1_name = "PortChannel0001" 
        test_lag1_member = "Ethernet4"
        test_lag2_name = "PortChannel0002" 
        test_lag3_name = "PortChannel0003" 

        # Create a PortChannel in a line card 1 (owner line card)
        dvss = vct.dvss
        for name in dvss.keys():
            dvs = dvss[name]

            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")
            
            # Get the host name and asic name for the system lag alias verification
            cfg_hostname = metatbl.get("hostname")
            assert cfg_hostname != "", "Got error in getting hostname from CONFIG_DB DEVICE_METADATA"

            cfg_asic_name = metatbl.get("asic_name")
            assert cfg_asic_name != "", "Got error in getting asic_name from CONFIG_DB DEVICE_METADATA"

            cfg_switch_type = metatbl.get("switch_type")

            # Portchannel record verifiation done in line card
            if cfg_switch_type == "voq":    
                lc_switch_id = metatbl.get("switch_id")
                assert lc_switch_id != "", "Got error in getting switch_id from CONFIG_DB DEVICE_METADATA"
                if lc_switch_id == "0":
                    
                    # At this point we have 2 port channels test_lag1_name and test_lag2_name.
                    # These were created by the above two test cases. Now delete the PortChannel
                    # test_lag1_name and verify that the lag is removed and add test_lag3_name to 
                    # test for lag id allocator allocating newly available lag id

                    # Connect to app db: lag table and lag member table
                    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
                    psTbl_lag = swsscommon.ProducerStateTable(app_db, "LAG_TABLE")
                    psTbl_lagMember = swsscommon.ProducerStateTable(app_db, "LAG_MEMBER_TABLE")

                    # Make sure presence of 2 port channels before deleting
                    asic_db = dvs.get_asic_db()
                    lagkeys = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 2)
                    assert len(lagkeys) == 2, "Expected 2 PortChannels are not available"

                    # Make sure presence of total of 1 lag member added in test_lag1_name
                    # No lag member added in test_lag2_name
                    lagmemberkeys = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER", 1)
                    assert len(lagmemberkeys) == 1, "Expected 1 LAG members are not available"

                    # Delete port channel member of PortChannel test_lag1_name
                    psTbl_lagMember.delete(f"{test_lag1_name}:{test_lag1_member}")

                    # Verify the lag member is removed from asic db
                    lagmemberkeys = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER", 0)
                    assert len(lagmemberkeys) == 0, "Deleted LAG member is not removed from asic db"

                    # Delete PortChannel test_lag1_name
                    psTbl_lag.delete(f"{test_lag1_name}")
                    
                    # Verify deletion of the PorChannel
                    asic_db = dvs.get_asic_db()
                    lagkeys = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 1)
                    assert len(lagkeys) == 1, "Two LAG entries in asic db even after deleting a PortChannel"

                    # Create PortChannel test_lag3_name. This should be addedd successfully since deleting 
                    # PortChannel test_lag1_name made a lag id available for allocation
                    fvs = swsscommon.FieldValuePairs([("admin", "up"), ("mtu", "9100")])
                    psTbl_lag.set(f"{test_lag3_name}", fvs)

                    # Verify creation of the additional PortChannel after making space for more 
                    # PortChannels by deleting some PortChannels
                    asic_db = dvs.get_asic_db()
                    lagkeys = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 2)
                    assert len(lagkeys) == 2, "Two configured LAG entries are not available in asic db"
                    
                    # Check for the presence of voq system port aggregate id attribute for 2 LAGs
                    lag_entry = asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_LAG", lagkeys[0])
                    spa_id = lag_entry.get("SAI_LAG_ATTR_SYSTEM_PORT_AGGREGATE_ID")
                    assert spa_id != "", "VOQ System port aggregate id not present for the LAG 1"
                    
                    lag_entry = asic_db.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_LAG", lagkeys[1])
                    spa_id = lag_entry.get("SAI_LAG_ATTR_SYSTEM_PORT_AGGREGATE_ID")
                    assert spa_id != "", "VOQ System port aggregate id not present for the LAG 2"

                    # Now delete all the PortChannels so that we can veify the chassis app db
                    # clearing and remote asics clearing
                    psTbl_lag.delete(f"{test_lag2_name}")
                    
                    psTbl_lag.delete(f"{test_lag3_name}")

                    # Verify deletion of all PortChannels
                    asic_db = dvs.get_asic_db()
                    lagkeys = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 0)
                    assert len(lagkeys) == 0, "LAG entries in asic db even after deleting all PortChannels"
                    
                    break

        # Check syncing deletion of the PortChannels and PortChannel member in chasiss app db
        for name in dvss.keys():
            if name.startswith("supervisor"):
                dvs = dvss[name]
                chassis_app_db = DVSDatabase(swsscommon.CHASSIS_APP_DB, dvs.redis_chassis_sock)
                syslagkeys = chassis_app_db.wait_for_n_keys("SYSTEM_LAG_TABLE", 0)
                assert len(syslagkeys) == 0, "Stale system lag entries in chassis app db"
                    
                syslagmemberkeys = chassis_app_db.wait_for_n_keys("SYSTEM_LAG_MEMBER_TABLE", 0)
                assert len(syslagmemberkeys) == 0, "Stale system lag member entries in chassis app db"
                
                break

        # Verify removal of remote system lag in non-owner line card
        # Verify removal of system lag menbers in non-owner line card
        for name in dvss.keys():
            dvs = dvss[name]

            config_db = dvs.get_config_db()
            metatbl = config_db.get_entry("DEVICE_METADATA", "localhost")

            cfg_switch_type = metatbl.get("switch_type")

            # System LAG info verifiation done in non-owner line card
            if cfg_switch_type == "voq":    
                lc_switch_id = metatbl.get("switch_id")
                assert lc_switch_id != "", "Got error in getting switch_id from CONFIG_DB DEVICE_METADATA"
                if lc_switch_id != "0":
                    # Linecard other than linecard 1 (owner line card)
                    asic_db = dvs.get_asic_db()
                    remotesyslagkeys = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 0)
                    assert len(remotesyslagkeys) == 0, "Stale remote system lag entries in asic db"
                    
                    # Verify cleaning of system lag members
                    lagmemberkeys = asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER", 0)
                    assert len(lagmemberkeys) == 0, "Stale system lag member entries in asic db"
                    
                    break
                    
# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
