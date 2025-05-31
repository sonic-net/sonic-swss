import pytest
import time

from swsscommon import swsscommon


class TestIcmpEcho(object):
    def setup_db(self, dvs):
        dvs.setup_db()
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()
        self.sdb = dvs.get_state_db()
        self.cdb = dvs.get_config_db()

    def get_exist_icmp_echo_session(self):
        return set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_ICMP_ECHO_SESSION"))

    def create_icmp_echo_session(self, key, pairs):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "ICMP_ECHO_SESSION_TABLE")
        fvs = swsscommon.FieldValuePairs(list(pairs.items()))
        tbl.set(key, fvs)

    def remove_icmp_echo_session(self, key):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "ICMP_ECHO_SESSION_TABLE")
        tbl._del(key)

    def check_asic_icmp_echo_session_value(self, key, expected_values):
        fvs = self.adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_ICMP_ECHO_SESSION", key)
        for k, v in expected_values.items():
            assert fvs[k] == v

    def check_state_icmp_echo_session_value(self, key, expected_values):
        fvs = self.sdb.get_entry("ICMP_ECHO_SESSION_TABLE", key)
        for k, v in expected_values.items():
            assert fvs[k] == v

    def update_icmp_echo_session_state(self, dvs, session, state):
        icmp_echo_sai_state = {"Down":  "SAI_ICMP_ECHO_SESSION_STATE_DOWN",
                               "Up":    "SAI_ICMP_ECHO_SESSION_STATE_UP"}

        ntf = swsscommon.NotificationProducer(dvs.adb, "NOTIFICATIONS")
        fvp = swsscommon.FieldValuePairs()
        ntf_data = "[{\"icmp_echo_session_id\":\""+session+"\",\"session_state\":\""+icmp_echo_sai_state[state]+"\"}]"
        ntf.send("icmp_echo_session_state_change", ntf_data, fvp)

    def set_admin_status(self, interface, status):
        self.cdb.update_entry("PORT", interface, {"admin_status": status})

    def create_vrf(self, vrf_name):
        initial_entries = set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))

        self.cdb.create_entry("VRF", vrf_name, {"empty": "empty"})
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER", len(initial_entries) + 1)

        current_entries = set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"))
        assert len(current_entries - initial_entries) == 1
        return list(current_entries - initial_entries)[0]

    def remove_vrf(self, vrf_name):
        self.cdb.delete_entry("VRF", vrf_name)

    def create_l3_intf(self, interface, vrf_name):
        if len(vrf_name) == 0:
            self.cdb.create_entry("INTERFACE", interface, {"NULL": "NULL"})
        else:
            self.cdb.create_entry("INTERFACE", interface, {"vrf_name": vrf_name})

    def remove_l3_intf(self, interface):
        self.cdb.delete_entry("INTERFACE", interface)

    def add_ip_address(self, interface, ip):
        self.cdb.create_entry("INTERFACE", interface + "|" + ip, {"NULL": "NULL"})

    def remove_ip_address(self, interface, ip):
        self.cdb.delete_entry("INTERFACE", interface + "|" + ip)

    def test_addUpdateRemoveIcmpEchoSession(self, dvs):
        self.setup_db(dvs)

        icmpEchoSessions = self.get_exist_icmp_echo_session()

        # Create ICMP ECHO session
        fieldValues = {"session_cookie": "12345",
                       "src_ip": "10.0.0.1", "dst_ip":"10.0.0.2", "tx_interval":
                       "10", "rx_interval": "10"}
        self.create_icmp_echo_session("default:default:5000:NORMAL", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ICMP_ECHO_SESSION", len(icmpEchoSessions) + 1)

        # Checked created ICMP ECHO session in ASIC_DB
        createdSessions = self.get_exist_icmp_echo_session() - icmpEchoSessions
        assert len(createdSessions) == 1

        # self session
        session = createdSessions.pop()
        expected_adb_values = {
            "SAI_ICMP_ECHO_SESSION_ATTR_GUID": "5000",
            "SAI_ICMP_ECHO_SESSION_ATTR_COOKIE": "12345",
            "SAI_ICMP_ECHO_SESSION_ATTR_TX_INTERVAL": "10000",
            "SAI_ICMP_ECHO_SESSION_ATTR_RX_INTERVAL": "10000",
            "SAI_ICMP_ECHO_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_ICMP_ECHO_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_ICMP_ECHO_SESSION_ATTR_IPHDR_VERSION": "4",
            "SAI_ICMP_ECHO_SESSION_ATTR_HW_LOOKUP_VALID": "true",
        }
        self.check_asic_icmp_echo_session_value(session, expected_adb_values)

        # Check STATE_DB entry related to the ICMP ECHO session
        expected_sdb_values = {"session_guid": "5000", "session_cookie": "12345",
                               "src_ip": "10.0.0.1", "dst_ip": "10.0.0.2", "tx_interval" :"10",
                               "rx_interval": "10", "hw_lookup": "true"}
        self.check_state_icmp_echo_session_value("default|default|5000|NORMAL", expected_sdb_values)

        # Send ICMP ECHO session state notification to update ICMP ECHO session state
        self.update_icmp_echo_session_state(dvs, session, "Up")
        time.sleep(2)

        # Confirm ICMP ECHO session state in STATE_DB is updated as expected
        expected_sdb_values["state"] = "Up"
        self.check_state_icmp_echo_session_value("default|default|5000|NORMAL", expected_sdb_values)

        # Update tx/rx_interval in ICMP ECHO session
        update_fieldValues = {"session_guid": "5000", "session_cookie": "12345",
                       "src_ip": "10.0.0.1", "dst_ip":"10.0.0.2", "tx_interval":
                       "100", "rx_interval": "50"}
        self.create_icmp_echo_session("default:default:5000:NORMAL", update_fieldValues)
        # wait after update
        time.sleep(2)

        # Confirm tx/rx_interval does get updated
        expected_sdb_values["tx_interval"] = "100"
        expected_sdb_values["rx_interval"] = "50"
        self.check_state_icmp_echo_session_value("default|default|5000|NORMAL", expected_sdb_values)

        # Verify the ASIC_DB gets the updated value
        expected_adb_values = {
            "SAI_ICMP_ECHO_SESSION_ATTR_GUID": "5000",
            "SAI_ICMP_ECHO_SESSION_ATTR_COOKIE": "12345",
            "SAI_ICMP_ECHO_SESSION_ATTR_TX_INTERVAL": "100000",
            "SAI_ICMP_ECHO_SESSION_ATTR_RX_INTERVAL": "50000",
            "SAI_ICMP_ECHO_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_ICMP_ECHO_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_ICMP_ECHO_SESSION_ATTR_IPHDR_VERSION": "4",
            "SAI_ICMP_ECHO_SESSION_ATTR_HW_LOOKUP_VALID": "true",
        }
        self.check_asic_icmp_echo_session_value(session, expected_adb_values)

        # remove the session
        self.remove_icmp_echo_session("default:default:5000:NORMAL")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_ICMP_ECHO_SESSION", session)

        # RX session
        peer_fieldValues = {"session_cookie": "12345",
                       "src_ip": "10.0.0.1", "dst_ip":"10.0.0.2", "tx_interval":
                       "10", "rx_interval": "10"}
        self.create_icmp_echo_session("default:default:5000:RX", peer_fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ICMP_ECHO_SESSION", len(icmpEchoSessions)+1)

        # Checked created ICMP ECHO session in ASIC_DB
        createdSessions = self.get_exist_icmp_echo_session() - icmpEchoSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_adb_values = {
            "SAI_ICMP_ECHO_SESSION_ATTR_GUID": "5000",
            "SAI_ICMP_ECHO_SESSION_ATTR_COOKIE": "12345",
            "SAI_ICMP_ECHO_SESSION_ATTR_TX_INTERVAL": "0",
            "SAI_ICMP_ECHO_SESSION_ATTR_RX_INTERVAL": "10000",
            "SAI_ICMP_ECHO_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_ICMP_ECHO_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_ICMP_ECHO_SESSION_ATTR_IPHDR_VERSION": "4",
        }
        self.check_asic_icmp_echo_session_value(session, expected_adb_values)

        # Check STATE_DB entry related to the ICMP ECHO session
        expected_sdb_values = {"session_guid": "5000", "session_cookie": "12345",
                               "src_ip": "10.0.0.1", "dst_ip": "10.0.0.2", "tx_interval" :"0",
                               "rx_interval": "10", "hw_lookup": "true"}
        self.check_state_icmp_echo_session_value("default|default|5000|RX", expected_sdb_values)

        # Confirm tx_interval does not get updated
        peer_update_fieldValues = {"session_cookie": "12345",
                       "src_ip": "10.0.0.1", "dst_ip":"10.0.0.2", "tx_interval":
                       "100", "rx_interval": "100"}
        self.create_icmp_echo_session("default:default:5000:RX", peer_update_fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ICMP_ECHO_SESSION", len(icmpEchoSessions)+1)

        # Checked ICMP ECHO session in ASIC_DB post update
        createdSessions = self.get_exist_icmp_echo_session() - icmpEchoSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_adb_values = {
            "SAI_ICMP_ECHO_SESSION_ATTR_GUID": "5000",
            "SAI_ICMP_ECHO_SESSION_ATTR_COOKIE": "12345",
            "SAI_ICMP_ECHO_SESSION_ATTR_TX_INTERVAL": "0",
            "SAI_ICMP_ECHO_SESSION_ATTR_RX_INTERVAL": "100000",
            "SAI_ICMP_ECHO_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_ICMP_ECHO_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_ICMP_ECHO_SESSION_ATTR_IPHDR_VERSION": "4",
        }
        self.check_asic_icmp_echo_session_value(session, expected_adb_values)

        # Check STATE_DB entry related to the ICMP ECHO session post update
        expected_sdb_values = {"session_guid": "5000", "session_cookie": "12345",
                               "src_ip": "10.0.0.1", "dst_ip": "10.0.0.2", "tx_interval" :"0",
                               "rx_interval": "100", "hw_lookup": "true"}
        self.check_state_icmp_echo_session_value("default|default|5000|RX", expected_sdb_values)

        # Send ICMP ECHO session state notification to update ICMP ECHO session state
        self.update_icmp_echo_session_state(dvs, session, "Up")
        time.sleep(2)

        # Confirm ICMP ECHO session state in STATE_DB is updated as expected
        expected_sdb_values["state"] = "Up"
        self.check_state_icmp_echo_session_value("default|default|5000|RX", expected_sdb_values)

        # Remove the ICMP sessions
        self.remove_icmp_echo_session("default:default:5000:RX")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_ICMP_ECHO_SESSION", session)

        keys = self.sdb.get_keys("ICMP_ECHO_SESSION_TABLE")
        assert len(keys) == 0

    def test_multipleIcmpEchoSessions(self, dvs):
        self.setup_db(dvs)

        # create interfaces and add IP address
        self.create_l3_intf("Ethernet0", "default")
        self.create_l3_intf("Ethernet4", "default")
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        self.add_ip_address("Ethernet4", "10.0.1.0/31")
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")

        icmpEchoSessions = self.get_exist_icmp_echo_session()

        # Create ICMP session 1
        fieldValues = {"session_cookie": "12345",
                       "src_ip": "10.0.0.1", "dst_ip":"10.0.0.2", "tx_interval":
                       "10", "rx_interval": "10", "dst_mac": "01:23:45:aa:bb:cc"}

        key1_self = "default:Ethernet0:5000:NORMAL"
        self.create_icmp_echo_session(key1_self, fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ICMP_ECHO_SESSION", len(icmpEchoSessions) + 1)

        # Checked created ICMP ECHO session in ASIC_DB
        createdSessions = self.get_exist_icmp_echo_session() - icmpEchoSessions
        icmpEchoSessions = self.get_exist_icmp_echo_session()
        assert len(createdSessions) == 1

        # self session
        session1 = createdSessions.pop()
        expected_adb_values = {
            "SAI_ICMP_ECHO_SESSION_ATTR_GUID": "5000",
            "SAI_ICMP_ECHO_SESSION_ATTR_COOKIE": "12345",
            "SAI_ICMP_ECHO_SESSION_ATTR_TX_INTERVAL": "10000",
            "SAI_ICMP_ECHO_SESSION_ATTR_RX_INTERVAL": "10000",
            "SAI_ICMP_ECHO_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_ICMP_ECHO_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_ICMP_ECHO_SESSION_ATTR_IPHDR_VERSION": "4",
            "SAI_ICMP_ECHO_SESSION_ATTR_HW_LOOKUP_VALID": "false",
            "SAI_ICMP_ECHO_SESSION_ATTR_DST_MAC_ADDRESS": "01:23:45:AA:BB:CC",
        }
        self.check_asic_icmp_echo_session_value(session1, expected_adb_values)

        # Check STATE_DB entry related to the ICMP ECHO session
        expected_sdb_values = {"session_guid": "5000", "session_cookie": "12345",
                               "src_ip": "10.0.0.1", "dst_ip": "10.0.0.2", "tx_interval" :"10",
                               "rx_interval": "10", "hw_lookup": "false"}
        self.check_state_icmp_echo_session_value("default|Ethernet0|5000|NORMAL", expected_sdb_values)

        # Send ICMP ECHO session state notification to update ICMP ECHO session state
        self.update_icmp_echo_session_state(dvs, session1, "Up")
        time.sleep(2)

        # Confirm ICMP ECHO session state in STATE_DB is updated as expected
        expected_sdb_values["state"] = "Up"
        self.check_state_icmp_echo_session_value("default|Ethernet0|5000|NORMAL", expected_sdb_values)

        # RX session
        peer_fieldValues = {"session_cookie": "12345",
                       "src_ip": "10.0.0.1", "dst_ip":"10.0.0.2", "tx_interval":
                       "10", "rx_interval": "10", "dst_mac": "01:23:45:aa:bb:cc"}

        key1_peer = "default:Ethernet0:6000:RX"
        self.create_icmp_echo_session(key1_peer, peer_fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ICMP_ECHO_SESSION", len(icmpEchoSessions) + 1)

        # Checked created ICMP ECHO session in ASIC_DB
        createdSessions = self.get_exist_icmp_echo_session() - icmpEchoSessions
        assert len(createdSessions) == 1

        session2 = createdSessions.pop()
        expected_adb_values = {
            "SAI_ICMP_ECHO_SESSION_ATTR_GUID": "6000",
            "SAI_ICMP_ECHO_SESSION_ATTR_COOKIE": "12345",
            "SAI_ICMP_ECHO_SESSION_ATTR_TX_INTERVAL": "0",
            "SAI_ICMP_ECHO_SESSION_ATTR_RX_INTERVAL": "10000",
            "SAI_ICMP_ECHO_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_ICMP_ECHO_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_ICMP_ECHO_SESSION_ATTR_IPHDR_VERSION": "4",
            "SAI_ICMP_ECHO_SESSION_ATTR_HW_LOOKUP_VALID": "false",
            "SAI_ICMP_ECHO_SESSION_ATTR_DST_MAC_ADDRESS": "01:23:45:AA:BB:CC",
        }
        self.check_asic_icmp_echo_session_value(session2, expected_adb_values)

        # Check STATE_DB entry related to the ICMP ECHO session
        expected_sdb_values = {"session_guid": "6000", "session_cookie": "12345",
                               "src_ip": "10.0.0.1", "dst_ip": "10.0.0.2", "tx_interval" :"0",
                               "rx_interval": "10", "hw_lookup": "false"}
        self.check_state_icmp_echo_session_value("default|Ethernet0|6000|RX", expected_sdb_values)

        # Send ICMP ECHO session state notification to update ICMP ECHO session state
        self.update_icmp_echo_session_state(dvs, session2, "Up")
        time.sleep(2)

        # Confirm ICMP ECHO session state in STATE_DB is updated as expected
        expected_sdb_values["state"] = "Up"
        self.check_state_icmp_echo_session_value("default|Ethernet0|6000|RX", expected_sdb_values)

        # Remove the ICMP sessions
        self.remove_icmp_echo_session(key1_self)
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_ICMP_ECHO_SESSION", session1)
        self.remove_icmp_echo_session(key1_peer)
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_ICMP_ECHO_SESSION", session2)

        keys = self.sdb.get_keys("ICMP_ECHO_SESSION_TABLE")
        assert len(keys) == 0

    def test_icmp_echo_state_db_clear(self, dvs):
        self.setup_db(dvs)

        icmpEchoSessions = self.get_exist_icmp_echo_session()

        # Create Icmp echo session
        fieldValues = {"session_cookie": "12345",
                       "src_ip": "10.0.0.1", "dst_ip":"10.0.0.2", "tx_interval":
                       "10", "rx_interval": "10"}

        key1_self = "default:default:5000:NORMAL"
        self.create_icmp_echo_session(key1_self, fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ICMP_ECHO_SESSION", len(icmpEchoSessions) + 1)

        # Checked created icmp session in ASIC_DB
        createdSessions = self.get_exist_icmp_echo_session() - icmpEchoSessions
        assert len(createdSessions) == 1

        dvs.stop_swss()
        dvs.start_swss()

        time.sleep(5)
        keys = self.sdb.get_keys("ICMP_ECHO_SESSION_TABLE")
        assert len(keys) == 0

    def test_FailIcmpEchoSessions(self, dvs):
        self.setup_db(dvs)

        # create interfaces and add IP address
        self.create_l3_intf("Ethernet0", "default")
        self.create_l3_intf("Ethernet4", "default")
        self.add_ip_address("Ethernet0", "10.0.0.0/31")
        self.add_ip_address("Ethernet4", "10.0.1.0/31")
        self.set_admin_status("Ethernet0", "up")
        self.set_admin_status("Ethernet4", "up")

        icmpEchoSessions = self.get_exist_icmp_echo_session()

        # Create ICMP session 1
        fieldValues = {"session_cookie": "12345",
                       "src_ip": "10.0.0.1", "dst_ip":"10.0.0.2", "tx_interval": "10",
                       "rx_interval": "10", "src_mac": "01:01:02:02:03:04", "ttl" : "3",
                       "hw_lookup": "true", "tos": "1"}

        # bad key
        key1_self = "default:Ethernet0:5000"
        self.create_icmp_echo_session(key1_self, fieldValues)
        time.sleep(2)

        # Create should for bad key fail
        createdSessions = self.get_exist_icmp_echo_session() - icmpEchoSessions
        icmpEchoSessions = self.get_exist_icmp_echo_session()
        assert len(createdSessions) == 0

        # missing dst_mac, creation should fail with proper key
        key1_self = "default:Ethernet0:5000:"
        self.create_icmp_echo_session(key1_self, fieldValues)
        time.sleep(2)

        # Create should fail for missing dst_mac when using non-default alias
        createdSessions = self.get_exist_icmp_echo_session() - icmpEchoSessions
        icmpEchoSessions = self.get_exist_icmp_echo_session()
        assert len(createdSessions) == 0

        # add the dst_mac
        fieldValues["hw_lookup"] = "false"
        fieldValues["dst_mac"] = "01:23:45:aa:bb:cc"

        # default alias with dst_mac should fail
        key1_self = "default:default:5000:"
        self.create_icmp_echo_session(key1_self, fieldValues)
        time.sleep(2)

        # Create should fail
        createdSessions = self.get_exist_icmp_echo_session() - icmpEchoSessions
        icmpEchoSessions = self.get_exist_icmp_echo_session()
        assert len(createdSessions) == 0

        # unkown port alias should fail
        key1_self = "default:Ethernet128:5000:"
        self.create_icmp_echo_session(key1_self, fieldValues)
        time.sleep(2)

        # Create should fail
        createdSessions = self.get_exist_icmp_echo_session() - icmpEchoSessions
        icmpEchoSessions = self.get_exist_icmp_echo_session()
        assert len(createdSessions) == 0

        # Remove the ICMP sessions
        self.remove_icmp_echo_session(key1_self)
        time.sleep(1)

        # creation should pass with unsupported attrib
        fieldValues["unknown_attrib"] = "XXXX"
        # creation should pass after hw_lookup is set ot false
        key1_self = "default:Ethernet0:5000:"
        self.create_icmp_echo_session(key1_self, fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ICMP_ECHO_SESSION", len(icmpEchoSessions) + 1)

        # Checked created ICMP ECHO session in ASIC_DB
        createdSessions = self.get_exist_icmp_echo_session() - icmpEchoSessions
        icmpEchoSessions = self.get_exist_icmp_echo_session()
        assert len(createdSessions) == 1

        # self session
        session1 = createdSessions.pop()
        expected_adb_values = {
            "SAI_ICMP_ECHO_SESSION_ATTR_GUID": "5000",
            "SAI_ICMP_ECHO_SESSION_ATTR_COOKIE": "12345",
            "SAI_ICMP_ECHO_SESSION_ATTR_TX_INTERVAL": "10000",
            "SAI_ICMP_ECHO_SESSION_ATTR_RX_INTERVAL": "10000",
            "SAI_ICMP_ECHO_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_ICMP_ECHO_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_ICMP_ECHO_SESSION_ATTR_IPHDR_VERSION": "4",
            "SAI_ICMP_ECHO_SESSION_ATTR_HW_LOOKUP_VALID": "false",
            "SAI_ICMP_ECHO_SESSION_ATTR_DST_MAC_ADDRESS": "01:23:45:AA:BB:CC",
            "SAI_ICMP_ECHO_SESSION_ATTR_TTL": "3",
        }
        self.check_asic_icmp_echo_session_value(session1, expected_adb_values)

        # Check STATE_DB entry related to the ICMP ECHO session
        expected_sdb_values = {"session_guid": "5000", "session_cookie": "12345",
                               "src_ip": "10.0.0.1", "dst_ip": "10.0.0.2", "tx_interval": "10",
                               "rx_interval": "10", "hw_lookup": "false"}
        self.check_state_icmp_echo_session_value("default|Ethernet0|5000|NORMAL", expected_sdb_values)

        # notification with wrong key
        self.update_icmp_echo_session_state(dvs, session1, "Up")
        time.sleep(2)

        # Confirm ICMP ECHO session state in STATE_DB is updated as expected
        expected_sdb_values["state"] = "Up"
        self.check_state_icmp_echo_session_value("default|Ethernet0|5000|NORMAL", expected_sdb_values)

        # Remove the ICMP sessions
        self.remove_icmp_echo_session(key1_self)
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_ICMP_ECHO_SESSION", session1)

        keys = self.sdb.get_keys("ICMP_ECHO_SESSION_TABLE")
        assert len(keys) == 0

        # RX session
        peer_fieldValues = {"session_cookie": "12345", "src_ip": "10.0.0.1",
                            "dst_ip":"10.0.0.2", "tx_interval": "10",
                            "rx_interval": "10", "dst_mac": "01:23:45:aa:bb:cc",
                            "ttl" : "10"}

        key1_peer = "default:Ethernet0:6000:RX"
        self.create_icmp_echo_session(key1_peer, peer_fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_ICMP_ECHO_SESSION", len(icmpEchoSessions))

        # Checked created ICMP ECHO session in ASIC_DB
        createdSessions = self.get_exist_icmp_echo_session() - icmpEchoSessions
        assert len(createdSessions) == 1

        session2 = createdSessions.pop()
        expected_adb_values = {
            "SAI_ICMP_ECHO_SESSION_ATTR_GUID": "6000",
            "SAI_ICMP_ECHO_SESSION_ATTR_COOKIE": "12345",
            "SAI_ICMP_ECHO_SESSION_ATTR_TX_INTERVAL": "0",
            "SAI_ICMP_ECHO_SESSION_ATTR_RX_INTERVAL": "10000",
            "SAI_ICMP_ECHO_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_ICMP_ECHO_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_ICMP_ECHO_SESSION_ATTR_IPHDR_VERSION": "4",
            "SAI_ICMP_ECHO_SESSION_ATTR_HW_LOOKUP_VALID": "false",
            "SAI_ICMP_ECHO_SESSION_ATTR_DST_MAC_ADDRESS": "01:23:45:AA:BB:CC",
            "SAI_ICMP_ECHO_SESSION_ATTR_TTL": "10",
        }
        self.check_asic_icmp_echo_session_value(session2, expected_adb_values)

        # Check STATE_DB entry related to the ICMP ECHO session
        expected_sdb_values = {"session_guid": "6000", "session_cookie": "12345",
                               "src_ip": "10.0.0.1", "dst_ip": "10.0.0.2", "tx_interval" :"0",
                               "rx_interval": "10", "hw_lookup": "false"}
        self.check_state_icmp_echo_session_value("default|Ethernet0|6000|RX", expected_sdb_values)

        # Send ICMP ECHO session state notification to update ICMP ECHO session state
        self.update_icmp_echo_session_state(dvs, session2, "Up")
        time.sleep(2)

        # Confirm ICMP ECHO session state in STATE_DB is updated as expected
        expected_sdb_values["state"] = "Up"
        self.check_state_icmp_echo_session_value("default|Ethernet0|6000|RX", expected_sdb_values)

        # Failure Remove the ICMP sessions
        self.remove_icmp_echo_session(key1_self)
        time.sleep(1)

        keys = self.sdb.get_keys("ICMP_ECHO_SESSION_TABLE")
        assert len(keys) == 1

        # Update with valid new field should fail
        peer_fieldValues["tos"] = "2"

        self.create_icmp_echo_session(key1_peer, peer_fieldValues)
        time.sleep(1)

        # Checked no new created session
        createdSessions = self.get_exist_icmp_echo_session() - icmpEchoSessions
        assert len(createdSessions) == 1

        del peer_fieldValues["tos"]

        # Update tx_interval should fail for RX session
        peer_fieldValues["tx_interval"] = "20"

        self.create_icmp_echo_session(key1_peer, peer_fieldValues)
        time.sleep(1)

        # Checked no new created session
        createdSessions = self.get_exist_icmp_echo_session() - icmpEchoSessions
        assert len(createdSessions) == 1

        # check expected values did not change
        self.check_state_icmp_echo_session_value("default|Ethernet0|6000|RX", expected_sdb_values)

        del peer_fieldValues["tx_interval"]

        # Update unsupported field should fail
        peer_fieldValues["ttl"] = "1"

        self.create_icmp_echo_session(key1_peer, peer_fieldValues)
        time.sleep(1)

        # Checked no new created session
        createdSessions = self.get_exist_icmp_echo_session() - icmpEchoSessions
        assert len(createdSessions) == 1

        # check expected values did not change
        self.check_state_icmp_echo_session_value("default|Ethernet0|6000|RX", expected_sdb_values)

        peer_fieldValues["ttl"] = "10"

        # Update unknown field should fail
        peer_fieldValues["unknown_attrib"] = "DDDD"

        self.create_icmp_echo_session(key1_peer, peer_fieldValues)
        time.sleep(1)

        # Checked no new created session
        createdSessions = self.get_exist_icmp_echo_session() - icmpEchoSessions
        assert len(createdSessions) == 1

        # check expected values did not change
        self.check_state_icmp_echo_session_value("default|Ethernet0|6000|RX", expected_sdb_values)

        del peer_fieldValues["unknown_attrib"]

        session2 = createdSessions.pop()

        #remove the second session
        self.remove_icmp_echo_session(key1_peer)
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_ICMP_ECHO_SESSION", session2)

        keys = self.sdb.get_keys("ICMP_ECHO_SESSION_TABLE")
        assert len(keys) == 0
