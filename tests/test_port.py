import time
import os
import pytest

from swsscommon import swsscommon


class TestPort(object):
    def test_PortTpid(self, dvs, testlog):
        pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

        # set TPID to port
        cdb_port_tbl = swsscommon.Table(cdb, "PORT")
        fvs = swsscommon.FieldValuePairs([("tpid", "0x9200")])
        cdb_port_tbl.set("Ethernet8", fvs)
        time.sleep(1)

        # check application database
        pdb_port_tbl = swsscommon.Table(pdb, "PORT_TABLE")
        (status, fvs) = pdb_port_tbl.get("Ethernet8")
        assert status == True
        for fv in fvs:
            if fv[0] == "tpid":
                tpid = fv[1]
        assert tpid == "0x9200"

        # Check ASIC DB
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        # get TPID and validate it to be 0x9200 (37376)
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet8"])
        assert status == True
        asic_tpid = "0"

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_TPID":
                asic_tpid = fv[1]

        assert asic_tpid == "37376"

    def test_PortMtu(self, dvs, testlog):
        pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

        # set MTU to port
        tbl = swsscommon.Table(cdb, "PORT")
        fvs = swsscommon.FieldValuePairs([("MTU", "9100")])
        tbl.set("Ethernet8", fvs)
        time.sleep(1)

        # check application database
        tbl = swsscommon.Table(pdb, "PORT_TABLE")
        (status, fvs) = tbl.get("Ethernet8")
        assert status == True
        for fv in fvs:
            if fv[0] == "mtu":
                assert fv[1] == "9100"

    def test_PortNotification(self, dvs, testlog):
        dvs.port_admin_set("Ethernet0", "up")
        dvs.interface_ip_add("Ethernet0", "10.0.0.0/31")

        dvs.port_admin_set("Ethernet4", "up")
        dvs.interface_ip_add("Ethernet4", "10.0.0.2/31")

        dvs.servers[0].runcmd("ip link set down dev eth0") == 0

        time.sleep(1)

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)

        tbl = swsscommon.Table(db, "PORT_TABLE")

        (status, fvs) = tbl.get("Ethernet0")

        assert status == True

        oper_status = "unknown"

        for v in fvs:
            if v[0] == "oper_status":
                oper_status = v[1]
                break

        assert oper_status == "down"

        dvs.servers[0].runcmd("ip link set up dev eth0") == 0

        time.sleep(1)

        (status, fvs) = tbl.get("Ethernet0")

        assert status == True

        oper_status = "unknown"

        for v in fvs:
            if v[0] == "oper_status":
                oper_status = v[1]
                break

        assert oper_status == "up"

    def test_PortFecForce(self, dvs, testlog):
        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = dvs.get_asic_db()

        ptbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")

        # set fec
        fvs = swsscommon.FieldValuePairs([("fec","none")])
        ptbl.set("Ethernet0", fvs)
        fvs = swsscommon.FieldValuePairs([("fec","rs")])
        ptbl.set("Ethernet4", fvs)

        # validate if fec none is pushed to asic db when set first time
        port_oid = adb.port_name_map["Ethernet0"]
        expected_fields = {"SAI_PORT_ATTR_FEC_MODE":"SAI_PORT_FEC_MODE_NONE"}
        adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid, expected_fields)

        # validate if fec rs is pushed to asic db when set first time
        port_oid = adb.port_name_map["Ethernet4"]
        expected_fields = {"SAI_PORT_ATTR_FEC_MODE":"SAI_PORT_FEC_MODE_RS"}
        adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid, expected_fields)

    def test_PortFec(self, dvs, testlog):
        dvs.port_admin_set("Ethernet0", "up")
        dvs.interface_ip_add("Ethernet0", "10.0.0.0/31")

        dvs.port_admin_set("Ethernet4", "up")
        dvs.interface_ip_add("Ethernet4", "10.0.0.2/31")

        dvs.servers[0].runcmd("ip link set down dev eth0") == 0

        time.sleep(1)

        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        tbl = swsscommon.Table(db, "PORT_TABLE")
        ptbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")

        (status, fvs) = tbl.get("Ethernet0")

        assert status == True

        oper_status = "unknown"

        for v in fvs:
            if v[0] == "oper_status":
                oper_status = v[1]
                break

        assert oper_status == "down"

        dvs.servers[0].runcmd("ip link set up dev eth0") == 0

        time.sleep(1)

        (status, fvs) = tbl.get("Ethernet0")

        assert status == True

        oper_status = "unknown"

        for v in fvs:
            if v[0] == "oper_status":
                oper_status = v[1]
                break

        assert oper_status == "up"

        # set fec
        fvs = swsscommon.FieldValuePairs([("fec","rs"), ("speed", "1000")])
        ptbl.set("Ethernet0", fvs)

        time.sleep(1)

        # get fec
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_FEC_MODE":
                assert fv[1] == "SAI_PORT_FEC_MODE_RS"
            assert fv[0] != "SAI_PORT_ATTR_AUTO_NEG_FEC_MODE_OVERRIDE"

    def test_PortPreemp(self, dvs, testlog):

        pre_name = 'preemphasis'
        pre_val = [0x1234,0x2345,0x3456,0x4567]
        pre_val_str = str(hex(pre_val[0])) + "," + str(hex(pre_val[1]))+ "," + \
                      str(hex(pre_val[2]))+ "," + str(hex(pre_val[3]))

        pre_val_asic = '4:' + str(pre_val[0]) + "," + str(pre_val[1]) + "," + \
                       str(pre_val[2]) + "," + str(pre_val[3])
        fvs = swsscommon.FieldValuePairs([(pre_name, pre_val_str)])
        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        tbl = swsscommon.Table(db, "PORT_TABLE")
        ptbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")

        ptbl.set("Ethernet0", fvs)


        time.sleep(1)

        # get fec
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_SERDES_PREEMPHASIS":
                assert fv[1] == pre_val_asic

    def test_PortIdriver(self, dvs, testlog):

        idrv_name = 'idriver'
        idrv_val = [0x1,0x1,0x2,0x2]
        idrv_val_str = str(hex(idrv_val[0])) + "," + str(hex(idrv_val[1]))+ "," + \
                       str(hex(idrv_val[2]))+ "," + str(hex(idrv_val[3]))

        idrv_val_asic = '4:' + str(idrv_val[0]) + "," + str(idrv_val[1]) + "," + \
                       str(idrv_val[2]) + "," + str(idrv_val[3])
        fvs = swsscommon.FieldValuePairs([(idrv_name, idrv_val_str)])
        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        tbl = swsscommon.Table(db, "PORT_TABLE")
        ptbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")

        ptbl.set("Ethernet0", fvs)


        time.sleep(1)

        # get fec
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_SERDES_IDRIVER":
                assert fv[1] == idrv_val_asic

    def test_PortIpredriver(self, dvs, testlog):

        ipre_name = 'ipredriver'
        ipre_val = [0x2,0x3,0x4,0x5]
        ipre_val_str = str(hex(ipre_val[0])) + "," + str(hex(ipre_val[1]))+ "," + \
                       str(hex(ipre_val[2]))+ "," + str(hex(ipre_val[3]))

        ipre_val_asic = '4:' + str(ipre_val[0]) + "," + str(ipre_val[1]) + "," + \
                       str(ipre_val[2]) + "," + str(ipre_val[3])
        fvs = swsscommon.FieldValuePairs([(ipre_name, ipre_val_str)])
        db = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        tbl = swsscommon.Table(db, "PORT_TABLE")
        ptbl = swsscommon.ProducerStateTable(db, "PORT_TABLE")
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")

        ptbl.set("Ethernet0", fvs)


        time.sleep(1)

        # get fec
        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_SERDES_IPREDRIVER":
                assert fv[1] == ipre_val_asic

    def test_PortHostif(self, dvs):
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF")
        host_intfs = atbl.getKeys()
        for intf in host_intfs:
            status, fvs = atbl.get(intf)
            assert status, "Error getting value for key"
            attributes = dict(fvs)
            if attributes.get("SAI_HOSTIF_ATTR_TYPE") != "SAI_HOSTIF_TYPE_NETDEV":
                continue
            hostif_queue = attributes.get("SAI_HOSTIF_ATTR_QUEUE")
            assert hostif_queue == "7"

    def test_PortHostTxSignalSet(self, dvs, testlog):
        adb = dvs.get_asic_db()
        statedb = dvs.get_state_db()

        transceiver_info_tbl = swsscommon.Table(statedb.db_connection, "TRANSCEIVER_INFO")
        fvs = swsscommon.FieldValuePairs([("supported_max_tx_power","N/A")])
        transceiver_info_tbl.set("Ethernet0", fvs)

        port_oid = adb.port_name_map["Ethernet0"]
        expected_fields = {"SAI_PORT_ATTR_HOST_TX_SIGNAL_ENABLE":"true"}
        adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid, expected_fields)

        transceiver_info_tbl.hdel("Ethernet0", "supported_max_tx_power")
        expected_fields = {"SAI_PORT_ATTR_HOST_TX_SIGNAL_ENABLE":"false"}
        adb.wait_for_field_match("ASIC_STATE:SAI_OBJECT_TYPE_PORT", port_oid, expected_fields)

    def test_PortPathTracing(self, dvs, testlog):
        pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)

        ctbl = swsscommon.Table(cdb, "PORT")
        ptbl = swsscommon.Table(pdb, "PORT_TABLE")
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")

        # get the number of ports before removal
        num_of_ports = len(atbl.getKeys())

        initial_entries = set(atbl.getKeys())

        # read port info and save it
        (status, ports_info) = ctbl.get("Ethernet124")
        assert status

        # remove buffer pg cfg for the port (record the buffer pgs before removing them)
        pgs = dvs.get_config_db().get_keys('BUFFER_PG')
        buffer_pgs = {}
        for key in pgs:
            if "Ethernet124" in key:
                buffer_pgs[key] = dvs.get_config_db().get_entry('BUFFER_PG', key)
                dvs.get_config_db().delete_entry('BUFFER_PG', key)
                dvs.get_app_db().wait_for_deleted_entry("BUFFER_PG_TABLE", key)

        # remove buffer queue cfg for the port
        queues = dvs.get_config_db().get_keys('BUFFER_QUEUE')
        buffer_queues = {}
        for key in queues:
            if "Ethernet124" in key:
                buffer_queues[key] = dvs.get_config_db().get_entry('BUFFER_QUEUE', key)
                dvs.get_config_db().delete_entry('BUFFER_QUEUE', key)
                dvs.get_app_db().wait_for_deleted_entry('BUFFER_QUEUE_TABLE', key)

        # shutdown port
        dvs.port_admin_set("Ethernet124", 'down')

        # remove this port
        ctbl.delete("Ethernet124")

        # verify that the port has been removed
        num = dvs.get_asic_db().wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT", num_of_ports - 1)
        assert len(num) == num_of_ports - 1

        # re-add the port with Path Tracing enabled
        fvs = swsscommon.FieldValuePairs(ports_info + (("pt_interface_id", "129"), ("pt_timestamp_template", "template2")))
        ctbl.set("Ethernet124", fvs)

        # check application database
        dvs.get_app_db().wait_for_entry('PORT_TABLE', "Ethernet124")
        (status, fvs) = ptbl.get("Ethernet124")
        assert status
        for fv in fvs:
            if fv[0] == "pt_interface_id":
                assert fv[1] == "129"
            if fv[0] == "pt_timestamp_template":
                assert fv[1] == "template2"

        # verify that the port has been re-added
        num = dvs.get_asic_db().wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT", num_of_ports)
        assert len(num) == num_of_ports

        # check ASIC DB
        atbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        # get PT Interface ID and validate it to be 129
        entries = set(atbl.getKeys())
        new_entries = list(entries - initial_entries)
        assert len(new_entries) == 1, "Wrong number of created entries."

        (status, fvs) = atbl.get(new_entries[0])
        assert status

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_PATH_TRACING_INTF":
                assert fv[1] == "129"
            if fv[0] == "SAI_PORT_ATTR_PATH_TRACING_TIMESTAMP_TYPE":
                assert fv[1] == "SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_12_19"

        # change Path Tracing Interface ID and Timestamp Template on the port
        fvs = swsscommon.FieldValuePairs([("pt_interface_id", "130"), ("pt_timestamp_template", "template3")])
        ctbl.set("Ethernet124", fvs)
        time.sleep(5)

        # check application database
        (status, fvs) = ptbl.get("Ethernet124")
        assert status
        for fv in fvs:
            if fv[0] == "pt_interface_id":
                assert fv[1] == "130"
            if fv[0] == "pt_timestamp_template":
                assert fv[1] == "template3"

        time.sleep(5)

        # check ASIC DB
        # get PT Interface ID and validate it to be 130
        (status, fvs) = atbl.get(new_entries[0])
        assert status

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_PATH_TRACING_INTF":
                assert fv[1] == "130"
            if fv[0] == "SAI_PORT_ATTR_PATH_TRACING_TIMESTAMP_TYPE":
                assert fv[1] == "SAI_PORT_PATH_TRACING_TIMESTAMP_TYPE_16_23"

        # shutdown port
        dvs.port_admin_set("Ethernet124", 'down')

        # remove the port
        ctbl.delete("Ethernet124")

        # re-add the port with the original configuration
        ctbl.set("Ethernet124", ports_info)

        # verify that the port has been re-added
        num = dvs.get_asic_db().wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_PORT", num_of_ports)
        assert len(num) == num_of_ports

        # re-add buffer pg and queue cfg to the port
        for key, pg in buffer_pgs.items():
            dvs.get_config_db().update_entry("BUFFER_PG", key, pg)

        for key, queue in buffer_queues.items():
            dvs.get_config_db().update_entry("BUFFER_QUEUE", key, queue)

    def test_InvalidPortLoopbackMode(self, dvs, testlog):
        pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        table_name = "PORT_TABLE"
        port_name = "Ethernet0"
        attribute_name = "loopback_mode"

        for attribute_value in ["", "invalid"]:
            # Set invalid loopback mode.
            tbl = swsscommon.ProducerStateTable(pdb, table_name)
            fvs = swsscommon.FieldValuePairs([(attribute_name, attribute_value)])
            tbl.set(port_name, fvs)
            time.sleep(1)

            # Check application database.
            tbl = swsscommon.Table(pdb, table_name)
            (status, fvs) = tbl.get(port_name)
            assert status == True
            attribute_found = False
            for fv in fvs:
                if fv[0] == attribute_name:
                    attribute_found = True
                    assert fv[1] == attribute_value
            assert attribute_found == True

            # Check attribute not present in asic database.
            tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
            (status, fvs) = tbl.get(dvs.asicdb.portnamemap[port_name])
            assert status == True
            attribute_found = False
            for fv in fvs:
                if fv[0] == "SAI_PORT_ATTR_LOOPBACK_MODE":
                    attribute_found = True
            assert attribute_found == False

    def test_PortLoopbackMode(self, dvs, testlog):
        pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        table_name = "PORT_TABLE"
        port_name = "Ethernet0"
        attribute_name = "loopback_mode"
        attribute_value = "mac_local"

        # Enable loopback mode.
        tbl = swsscommon.ProducerStateTable(pdb, table_name)
        fvs = swsscommon.FieldValuePairs([(attribute_name, attribute_value)])
        tbl.set(port_name, fvs)
        time.sleep(1)

        # Check application database.
        tbl = swsscommon.Table(pdb, table_name)
        (status, fvs) = tbl.get(port_name)
        assert status == True
        attribute_found = False
        for fv in fvs:
            if fv[0] == attribute_name:
                attribute_found = True
                assert fv[1] == attribute_value
        assert attribute_found == True

        # Check asic database.
        tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = tbl.get(dvs.asicdb.portnamemap[port_name])
        assert status == True
        attribute_found = False
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_LOOPBACK_MODE":
                attribute_found = True
                assert fv[1] == "SAI_PORT_LOOPBACK_MODE_MAC"
        assert attribute_found == True

        # Disable loopback mode.
        attribute_value = "none"
        tbl = swsscommon.ProducerStateTable(pdb, table_name)
        fvs = swsscommon.FieldValuePairs([(attribute_name, attribute_value)])
        tbl.set(port_name, fvs)
        time.sleep(1)

        # Check application database.
        tbl = swsscommon.Table(pdb, table_name)
        (status, fvs) = tbl.get(port_name)
        assert status == True
        attribute_found = False
        for fv in fvs:
            if fv[0] == attribute_name:
                attribute_found = True
                assert fv[1] == attribute_value
        assert attribute_found == True

        # Check asic database.
        tbl = swsscommon.Table(adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        (status, fvs) = tbl.get(dvs.asicdb.portnamemap[port_name])
        assert status == True
        attribute_found = False
        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_LOOPBACK_MODE":
                attribute_found = True
                assert fv[1] == "SAI_PORT_LOOPBACK_MODE_NONE"
        assert attribute_found == True

    def test_PortLinkEventDamping(self, dvs, testlog):
        cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)

        cfg_tbl = swsscommon.Table(cdb, "PORT")
        app_tbl = swsscommon.Table(pdb, "PORT_TABLE")
        port_name = "Ethernet0"

        # Set link event damping.
        fvs = swsscommon.FieldValuePairs([("link_event_damping_algorithm", "aied"),
                                          ("max_suppress_time", "54000"),
                                          ("decay_half_life", "45000"),
                                          ("suppress_threshold", "1650"),
                                          ("reuse_threshold", "1500"),
                                          ("flap_penalty", "1000")
                                         ])
        cfg_tbl.set(port_name, fvs)
        time.sleep(1)

        # Check application database.
        (status, fvs) = app_tbl.get(port_name)
        assert status == True
        for fv in fvs:
            if fv[0] == "link_event_damping_algorithm":
                assert fv[1] == "aied"
            elif fv[0] == "max_suppress_time":
                assert fv[1] == "54000"
            elif fv[0] == "decay_half_life":
                assert fv[1] == "45000"
            elif fv[0] == "suppress_threshold":
                assert fv[1] == "1650"
            elif fv[0] == "reuse_threshold":
                assert fv[1] == "1500"
            elif fv[0] == "flap_penalty":
                assert fv[1] == "1000"

        # Disable link event damping.
        fvs = swsscommon.FieldValuePairs([("link_event_damping_algorithm", "disabled")])
        cfg_tbl.set(port_name, fvs)
        time.sleep(1)

        # Check application database.
        (status, fvs) = app_tbl.get(port_name)
        assert status == True
        for fv in fvs:
            if fv[0] == "link_event_damping_algorithm":
                assert fv[1] == "disabled"

    def test_PortAdminRestore(self, dvs, testlog):
        appdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        asicdb = swsscommon.DBConnector(1, dvs.redis_sock, 0)

        ptbl = swsscommon.ProducerStateTable(appdb, "PORT_TABLE")
        atbl = swsscommon.Table(asicdb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")

        # Initialize Ethernet0 (admin_status, fec) = (up, rs)
        fvs = swsscommon.FieldValuePairs([("admin_status", "up"),
                                          ("fec", "rs")])
        ptbl.set("Ethernet0", fvs)

        time.sleep(1)

        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_FEC_MODE":
                assert fv[1] == "SAI_PORT_FEC_MODE_RS"
            if fv[0] == "SAI_PORT_ATTR_ADMIN_STATE":
                assert fv[1] == "true"

        # Verify pCfg.admin_status.is_set false by (fec) = (none)
        fvs = swsscommon.FieldValuePairs([("fec", "none")])
        ptbl.set("Ethernet0", fvs)

        time.sleep(1)

        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_FEC_MODE":
                assert fv[1] == "SAI_PORT_FEC_MODE_NONE"
            if fv[0] == "SAI_PORT_ATTR_ADMIN_STATE":
                assert fv[1] == "true"

        # Verify pCfg.admin_status.is_set true by (admin_status, fec) = (down, rs)
        fvs = swsscommon.FieldValuePairs([("admin_status", "down"),
                                          ("fec", "rs")])
        ptbl.set("Ethernet0", fvs)

        time.sleep(1)

        (status, fvs) = atbl.get(dvs.asicdb.portnamemap["Ethernet0"])
        assert status == True

        for fv in fvs:
            if fv[0] == "SAI_PORT_ATTR_FEC_MODE":
                assert fv[1] == "SAI_PORT_FEC_MODE_RS"
            if fv[0] == "SAI_PORT_ATTR_ADMIN_STATE":
                assert fv[1] == "false"

# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
