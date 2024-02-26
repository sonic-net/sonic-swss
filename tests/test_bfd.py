import pytest
import time

from swsscommon import swsscommon

class TestBfd(object):
    def setup_db(self, dvs):
        dvs.setup_db()
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()
        self.sdb = dvs.get_state_db()
        self.cdb = dvs.get_config_db()

    def get_exist_bfd_session(self):
        return set(self.adb.get_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION"))

    def create_bfd_session(self, key, pairs):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "BFD_SESSION_TABLE")
        fvs = swsscommon.FieldValuePairs(list(pairs.items()))
        tbl.set(key, fvs)

    def remove_bfd_session(self, key):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "BFD_SESSION_TABLE")
        tbl._del(key)

    def check_asic_bfd_session_value(self, key, expected_values):
        fvs = self.adb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", key)
        for k, v in expected_values.items():
            assert fvs[k] == v

    def check_state_bfd_session_value(self, key, expected_values):
        fvs = self.sdb.get_entry("BFD_SESSION_TABLE", key)
        for k, v in expected_values.items():
            assert fvs[k] == v

    def update_bfd_session_state(self, dvs, session, state):
        bfd_sai_state = {"Admin_Down":  "SAI_BFD_SESSION_STATE_ADMIN_DOWN",
                         "Down":        "SAI_BFD_SESSION_STATE_DOWN",
                         "Init":        "SAI_BFD_SESSION_STATE_INIT",
                         "Up":          "SAI_BFD_SESSION_STATE_UP"}

        ntf = swsscommon.NotificationProducer(dvs.adb, "NOTIFICATIONS")
        fvp = swsscommon.FieldValuePairs()
        ntf_data = "[{\"bfd_session_id\":\""+session+"\",\"session_state\":\""+bfd_sai_state[state]+"\"}]"
        ntf.send("bfd_session_state_change", ntf_data, fvp)

    def update_bgp_global_dev_state(self, state):
        tbl = swsscommon.Table(self.cdb.db_connection, "BGP_DEVICE_GLOBAL")
        fvs = swsscommon.FieldValuePairs(list(state.items()))
        key = "STATE"
        tbl.set(key, fvs)
        time.sleep(1)

    def set_tsa(self):
        state = {"tsa_enabled": "true"}
        self.update_bgp_global_dev_state(state)

    def clear_tsa(self):
        state = {"tsa_enabled": "false"}
        self.update_bgp_global_dev_state(state)


    def test_addRemoveBfdSession(self, dvs):
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        # Create BFD session
        fieldValues = {"local_addr": "10.0.0.1","tos":"64"}
        self.create_bfd_session("default:default:10.0.0.2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        # Checked created BFD session in ASIC_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_adb_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE",
            "SAI_BFD_SESSION_ATTR_TOS": "64",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4"
        }
        self.check_asic_bfd_session_value(session, expected_adb_values)

        # Check STATE_DB entry related to the BFD session
        expected_sdb_values = {"state": "Down", "type": "async_active", "local_addr" : "10.0.0.1", "tx_interval" :"1000",
                                        "rx_interval" : "1000", "multiplier" : "10", "multihop": "false", "local_discriminator" : "1"}
        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        # Send BFD session state notification to update BFD session state
        self.update_bfd_session_state(dvs, session, "Up")
        time.sleep(2)

        # Confirm BFD session state in STATE_DB is updated as expected 
        expected_sdb_values["state"] = "Up"
        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        # Remove the BFD session
        self.remove_bfd_session("default:default:10.0.0.2")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)

    def test_addRemoveBfdSession_ipv6(self, dvs):
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        # Create BFD session
        fieldValues = {"local_addr": "2000::1"}
        self.create_bfd_session("default:default:2000::2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        # Checked created BFD session in ASIC_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_adb_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "2000::1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "2000::2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE",
            "SAI_BFD_SESSION_ATTR_TOS": "192",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "6"
        }
        self.check_asic_bfd_session_value(session, expected_adb_values)

        # Check STATE_DB entry related to the BFD session
        expected_sdb_values = {"state": "Down", "type": "async_active", "local_addr" : "2000::1", "tx_interval" :"1000",
                                        "rx_interval" : "1000", "multiplier" : "10", "multihop": "false", "local_discriminator" : "2"}
        self.check_state_bfd_session_value("default|default|2000::2", expected_sdb_values)

        # Send BFD session state notification to update BFD session state
        self.update_bfd_session_state(dvs, session, "Init")
        time.sleep(2)

        # Confirm BFD session state in STATE_DB is updated as expected 
        expected_sdb_values["state"] = "Init"
        self.check_state_bfd_session_value("default|default|2000::2", expected_sdb_values)

        # Remove the BFD session
        self.remove_bfd_session("default:default:2000::2")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)

    def test_addRemoveBfdSession_interface(self, dvs):
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        # Create BFD session
        fieldValues = {"local_addr": "10.0.0.1", "dst_mac": "00:02:03:04:05:06"}
        self.create_bfd_session("default:Ethernet0:10.0.0.2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        # Checked created BFD session in ASIC_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_adb_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE",
            "SAI_BFD_SESSION_ATTR_TOS": "192",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4",
            "SAI_BFD_SESSION_ATTR_HW_LOOKUP_VALID": "false",
            "SAI_BFD_SESSION_ATTR_DST_MAC_ADDRESS": "00:02:03:04:05:06"
        }
        self.check_asic_bfd_session_value(session, expected_adb_values)

        # Check STATE_DB entry related to the BFD session
        expected_sdb_values = {"state": "Down", "type": "async_active", "local_addr" : "10.0.0.1", "tx_interval" :"1000",
                                        "rx_interval" : "1000", "multiplier" : "10", "multihop": "false", "local_discriminator" : "3"}
        self.check_state_bfd_session_value("default|Ethernet0|10.0.0.2", expected_sdb_values)

        # Send BFD session state notification to update BFD session state
        self.update_bfd_session_state(dvs, session, "Down")
        time.sleep(2)

        # Confirm BFD session state in STATE_DB is updated as expected 
        expected_sdb_values["state"] = "Down"
        self.check_state_bfd_session_value("default|Ethernet0|10.0.0.2", expected_sdb_values)

        # Remove the BFD session
        self.remove_bfd_session("default:Ethernet0:10.0.0.2")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)

    def test_addRemoveBfdSession_txrx_interval(self, dvs):
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        # Create BFD session
        fieldValues = {"local_addr": "10.0.0.1", "tx_interval": "300", "rx_interval": "500"}
        self.create_bfd_session("default:default:10.0.0.2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        # Checked created BFD session in ASIC_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_adb_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE",
            "SAI_BFD_SESSION_ATTR_TOS": "192",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4",
            "SAI_BFD_SESSION_ATTR_MIN_TX": "300000",
            "SAI_BFD_SESSION_ATTR_MIN_RX": "500000",
        }
        self.check_asic_bfd_session_value(session, expected_adb_values)

        # Check STATE_DB entry related to the BFD session
        expected_sdb_values = {"state": "Down", "type": "async_active", "local_addr" : "10.0.0.1", "tx_interval" :"300",
                                        "rx_interval" : "500", "multiplier" : "10", "multihop": "false", "local_discriminator" : "4"}
        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        # Send BFD session state notification to update BFD session state
        self.update_bfd_session_state(dvs, session, "Admin_Down")
        time.sleep(2)

        # Confirm BFD session state in STATE_DB is updated as expected 
        expected_sdb_values["state"] = "Admin_Down"
        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        # Remove the BFD session
        self.remove_bfd_session("default:default:10.0.0.2")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)

    def test_addRemoveBfdSession_multiplier(self, dvs):
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        # Create BFD session
        fieldValues = {"local_addr": "10.0.0.1", "multiplier": "5"}
        self.create_bfd_session("default:default:10.0.0.2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        # Checked created BFD session in ASIC_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_adb_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE",
            "SAI_BFD_SESSION_ATTR_TOS": "192",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4",
            "SAI_BFD_SESSION_ATTR_MULTIPLIER": "5"
        }
        self.check_asic_bfd_session_value(session, expected_adb_values)

        # Check STATE_DB entry related to the BFD session
        expected_sdb_values = {"state": "Down", "type": "async_active", "local_addr" : "10.0.0.1", "tx_interval" :"1000",
                                        "rx_interval" : "1000", "multiplier" : "5", "multihop": "false", "local_discriminator" : "5"}
        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        # Send BFD session state notification to update BFD session state
        self.update_bfd_session_state(dvs, session, "Up")
        time.sleep(2)

        # Confirm BFD session state in STATE_DB is updated as expected 
        expected_sdb_values["state"] = "Up"
        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        # Remove the BFD session
        self.remove_bfd_session("default:default:10.0.0.2")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)

    def test_addRemoveBfdSession_multihop(self, dvs):
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        # Create BFD session
        fieldValues = {"local_addr": "10.0.0.1", "multihop": "true"}
        self.create_bfd_session("default:default:10.0.0.2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        # Checked created BFD session in ASIC_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_adb_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE",
            "SAI_BFD_SESSION_ATTR_TOS": "192",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4",
            "SAI_BFD_SESSION_ATTR_MULTIHOP": "true"
        }
        self.check_asic_bfd_session_value(session, expected_adb_values)

        # Check STATE_DB entry related to the BFD session
        expected_sdb_values = {"state": "Down", "type": "async_active", "local_addr" : "10.0.0.1", "tx_interval" :"1000",
                                        "rx_interval" : "1000", "multiplier" : "10", "multihop": "true", "local_discriminator" : "6"}
        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        # Send BFD session state notification to update BFD session state
        self.update_bfd_session_state(dvs, session, "Up")
        time.sleep(2)

        # Confirm BFD session state in STATE_DB is updated as expected 
        expected_sdb_values["state"] = "Up"
        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        # Remove the BFD session
        self.remove_bfd_session("default:default:10.0.0.2")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)

    def test_addRemoveBfdSession_type(self, dvs):
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        # Create BFD session
        fieldValues = {"local_addr": "10.0.0.1", "type": "demand_active"}
        self.create_bfd_session("default:default:10.0.0.2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        # Checked created BFD session in ASIC_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_adb_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_DEMAND_ACTIVE",
            "SAI_BFD_SESSION_ATTR_TOS": "192",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4"
        }
        self.check_asic_bfd_session_value(session, expected_adb_values)

        # Check STATE_DB entry related to the BFD session
        expected_sdb_values = {"state": "Down", "type": "demand_active", "local_addr" : "10.0.0.1", "tx_interval" :"1000",
                                        "rx_interval" : "1000", "multiplier" : "10", "multihop": "false", "local_discriminator" : "7"}
        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        # Send BFD session state notification to update BFD session state
        self.update_bfd_session_state(dvs, session, "Up")
        time.sleep(2)

        # Confirm BFD session state in STATE_DB is updated as expected 
        expected_sdb_values["state"] = "Up"
        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        # Remove the BFD session
        self.remove_bfd_session("default:default:10.0.0.2")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)

    def test_multipleBfdSessions(self, dvs):
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        # Create BFD session 1
        key1 = "default:default:10.0.0.2"
        fieldValues = {"local_addr": "10.0.0.1"}
        self.create_bfd_session(key1, fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        # Checked BFD session 1 in ASIC_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        bfdSessions = self.get_exist_bfd_session()
        assert len(createdSessions) == 1

        session1 = createdSessions.pop()
        expected_adb_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE",
            "SAI_BFD_SESSION_ATTR_TOS": "192",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4"
        }
        self.check_asic_bfd_session_value(session1, expected_adb_values)

        # Check STATE_DB entry related to the BFD session 1
        key_state_db1 = "default|default|10.0.0.2"
        expected_sdb_values1 = {"state": "Down", "type": "async_active", "local_addr" : "10.0.0.1", "tx_interval" :"1000",
                                        "rx_interval" : "1000", "multiplier" : "10", "multihop": "false", "local_discriminator" : "8"}
        self.check_state_bfd_session_value(key_state_db1, expected_sdb_values1)

        # Create BFD session 2
        key2 = "default:default:10.0.1.2"
        fieldValues = {"local_addr": "10.0.0.1", "tx_interval": "300", "rx_interval": "500"}
        self.create_bfd_session(key2, fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        # Checked BFD session 2 in ASIC_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        bfdSessions = self.get_exist_bfd_session()
        assert len(createdSessions) == 1

        session2 = createdSessions.pop()
        expected_adb_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.1.2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE",
            "SAI_BFD_SESSION_ATTR_TOS": "192",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4",
            "SAI_BFD_SESSION_ATTR_MIN_TX": "300000",
            "SAI_BFD_SESSION_ATTR_MIN_RX": "500000",
        }
        self.check_asic_bfd_session_value(session2, expected_adb_values)

        # Check STATE_DB entry related to the BFD session 2
        key_state_db2 = "default|default|10.0.1.2"
        expected_sdb_values2 = {"state": "Down", "type": "async_active", "local_addr" : "10.0.0.1", "tx_interval" :"300",
                                        "rx_interval" : "500", "multiplier" : "10", "multihop": "false", "local_discriminator" : "9"}
        self.check_state_bfd_session_value(key_state_db2, expected_sdb_values2)

        # Create BFD session 3
        key3 = "default:default:2000::2"
        fieldValues = {"local_addr": "2000::1", "type": "demand_active"}
        self.create_bfd_session(key3, fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        # Checked BFD session 3 in ASIC_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        bfdSessions = self.get_exist_bfd_session()
        assert len(createdSessions) == 1

        session3 = createdSessions.pop()
        expected_adb_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "2000::1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "2000::2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_DEMAND_ACTIVE",
            "SAI_BFD_SESSION_ATTR_TOS": "192",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "6"
        }
        self.check_asic_bfd_session_value(session3, expected_adb_values)

        # Check STATE_DB entry related to the BFD session 3
        key_state_db3 = "default|default|2000::2"
        expected_sdb_values3 = {"state": "Down", "type": "demand_active", "local_addr" : "2000::1", "tx_interval" :"1000",
                                        "rx_interval" : "1000", "multiplier" : "10", "multihop": "false", "local_discriminator" : "10"}
        self.check_state_bfd_session_value(key_state_db3, expected_sdb_values3)

        # Create BFD session 4
        key4 = "default:default:3000::2"
        fieldValues = {"local_addr": "3000::1"}
        self.create_bfd_session(key4, fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        # Checked BFD session 4 in ASIC_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        bfdSessions = self.get_exist_bfd_session()
        assert len(createdSessions) == 1

        session4 = createdSessions.pop()
        expected_adb_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "3000::1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "3000::2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE",
            "SAI_BFD_SESSION_ATTR_TOS": "192",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "6"
        }
        self.check_asic_bfd_session_value(session4, expected_adb_values)

        # Check STATE_DB entry related to the BFD session 4
        key_state_db4 = "default|default|3000::2"
        expected_sdb_values4 = {"state": "Down", "type": "async_active", "local_addr" : "3000::1", "tx_interval" :"1000",
                                        "rx_interval" : "1000", "multiplier" : "10", "multihop": "false", "local_discriminator" : "11"}
        self.check_state_bfd_session_value(key_state_db4, expected_sdb_values4)

        # Update BFD session states
        self.update_bfd_session_state(dvs, session1, "Up")
        expected_sdb_values1["state"] = "Up"
        self.update_bfd_session_state(dvs, session3, "Init")
        expected_sdb_values3["state"] = "Init"
        self.update_bfd_session_state(dvs, session4, "Admin_Down")
        expected_sdb_values4["state"] = "Admin_Down"
        time.sleep(2)

        # Confirm BFD session states in STATE_DB are updated as expected
        self.check_state_bfd_session_value(key_state_db1, expected_sdb_values1)
        self.check_state_bfd_session_value(key_state_db2, expected_sdb_values2)
        self.check_state_bfd_session_value(key_state_db3, expected_sdb_values3)
        self.check_state_bfd_session_value(key_state_db4, expected_sdb_values4)

        # Remove the BFD sessions
        self.remove_bfd_session(key1)
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session1)
        self.remove_bfd_session(key2)
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session2)
        self.remove_bfd_session(key3)
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session3)
        self.remove_bfd_session(key4)
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session4)

    def test_addRemoveBfdSession_with_tsa_case1(self, dvs):
        # This is a test for BFD caching mechanism.
        # This test sets up a BFD session with shutdown_bfd_during_tsa=true and checks state DB for session creation.
        # Then TSA is applied and removal of the session is verified in app db. This is followed by TSB and finally the
        # reinstated session is verified.
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        # Create BFD session
        fieldValues = {"local_addr": "10.0.0.1", "type": "demand_active", "shutdown_bfd_during_tsa": "true"}
        self.create_bfd_session("default:default:10.0.0.2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        # Checked created BFD session in ASIC_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_adb_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_DEMAND_ACTIVE",
            "SAI_BFD_SESSION_ATTR_TOS": "192",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4"
        }
        self.check_asic_bfd_session_value(session, expected_adb_values)

        # Check STATE_DB entry related to the BFD session
        expected_sdb_values = {"state": "Down", "type": "demand_active", "local_addr" : "10.0.0.1", "tx_interval" :"1000",
                                        "rx_interval" : "1000", "multiplier" : "10", "multihop": "false", "local_discriminator" : "12"}
        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        # Send BFD session state notification to update BFD session state
        self.update_bfd_session_state(dvs, session, "Up")
        time.sleep(2)
        bfdSessions = self.get_exist_bfd_session()
        # Confirm BFD session state in STATE_DB is updated as expected.
        expected_sdb_values["state"] = "Up"
        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        #set TSA
        self.set_tsa()
        time.sleep(2)

        #ensure the session is removed.
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)

        #set TSB
        self.clear_tsa()
        time.sleep(2)
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        session = createdSessions.pop()
        expected_sdb_values["local_discriminator"] = "13"
        self.update_bfd_session_state(dvs, session, "Up")
        time.sleep(2)
        # bfd session should come back
        expected_sdb_values["state"] = "Up"
        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        # Remove the BFD session

        self.remove_bfd_session("default:default:10.0.0.2")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)


    def test_addRemoveBfdSession_with_tsa_case2(self, dvs):
        # This is a test for BFD caching mechanism.
        # This test sets up a BFD session with shutdown_bfd_during_tsa=true and checks state DB for session creation.
        # Then TSA is applied and removal of the session is verified from app db. At this point the session is removed.
        # This isfollowed by TSB. Since the session configuration has been removed during TSB, the BFD session should not
        # start up.
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        # Create BFD session
        fieldValues = {"local_addr": "10.0.0.1", "type": "demand_active"}
        self.create_bfd_session("default:default:10.0.0.2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        # Checked created BFD session in ASIC_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_adb_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_DEMAND_ACTIVE",
            "SAI_BFD_SESSION_ATTR_TOS": "192",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4"
        }
        self.check_asic_bfd_session_value(session, expected_adb_values)

        # Check STATE_DB entry related to the BFD session
        expected_sdb_values = {"state": "Down", "type": "demand_active", "local_addr" : "10.0.0.1", "tx_interval" :"1000",
                                        "rx_interval" : "1000", "multiplier" : "10", "multihop": "false", "local_discriminator" : "14"}
        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        # Send BFD session state notification to update BFD session state
        self.update_bfd_session_state(dvs, session, "Up")
        time.sleep(2)
        # Confirm BFD session state in STATE_DB is updated as expected.
        expected_sdb_values["state"] = "Up"
        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        #set TSA
        self.set_tsa()
        time.sleep(2)

        #ensure the session is still present.
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)
        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        #set TSB
        self.clear_tsa()
        time.sleep(2)

        self.check_state_bfd_session_value("default|default|10.0.0.2", expected_sdb_values)

        # Remove the BFD session
        self.remove_bfd_session("default:default:10.0.0.2")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)

    def test_bfd_state_db_clear(self, dvs):
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        # Create BFD session
        fieldValues = {"local_addr": "10.0.0.1", "type": "demand_active"}
        self.create_bfd_session("default:default:10.0.0.2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        # Checked created BFD session in ASIC_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1
        dvs.stop_swss()
        dvs.start_swss()

        time.sleep(5)
        keys = self.sdb.get_keys("BFD_SESSION_TABLE")
        assert len(keys) == 0
