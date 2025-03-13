from swsscommon import swsscommon

#Replace with swsscommon.SOFTWARE_BFD_SESSION_STATE_TABLE once available in azure pipeline
#SOFT_BFD_STATE_TABLE = swsscommon.STATE_BFD_SOFTWARE_SESSION_TABLE_NAME
SOFT_BFD_STATE_TABLE = "BFD_SOFTWARE_SESSION_TABLE"

DVS_ENV = ["BFDOFFLOAD=false"]

class TestSoftBfd(object):
    def setup_db(self, dvs):
        dvs.setup_db()
        self.pdb = dvs.get_app_db()
        self.sdb = dvs.get_state_db()
        self.cdb = dvs.get_config_db()

        #Restart swss to pick up new switch type
        dvs.stop_swss()
        dvs.start_swss()

    def get_exist_bfd_session(self):
        return set(self.sdb.get_keys(SOFT_BFD_STATE_TABLE))

    def create_bfd_session(self, key, pairs):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "BFD_SESSION_TABLE")
        fvs = swsscommon.FieldValuePairs(list(pairs.items()))
        tbl.set(key, fvs)

    def remove_bfd_session(self, key):
        tbl = swsscommon.ProducerStateTable(self.pdb.db_connection, "BFD_SESSION_TABLE")
        tbl._del(key)

    def check_state_bfd_session_value(self, key, expected_values):
        #Key format is different in STATE_DB compared to APP_DB
        key = key.replace(":", "|", 2)
        fvs = self.sdb.get_entry(SOFT_BFD_STATE_TABLE, key)
        for k, v in expected_values.items():
            assert fvs[k] == v

    def test_addRemoveBfdSession(self, dvs):
        self.setup_db(dvs)
        bfd_session_key = "default:default:10.0.0.2"
        bfdSessions = self.get_exist_bfd_session()

        # Create BFD session
        fieldValues = {"local_addr": "10.0.0.1", "tos": "64", "multiplier": "5", "tx_interval": "300",
                       "rx_interval": "500"}
        self.create_bfd_session(bfd_session_key, fieldValues)
        self.sdb.wait_for_n_keys(SOFT_BFD_STATE_TABLE, len(bfdSessions) + 1)

        # Check created BFD session in STATE_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()

        # Check STATE_DB entry related to the BFD session
        self.check_state_bfd_session_value(bfd_session_key, fieldValues)

        # Remove the BFD session
        self.remove_bfd_session(bfd_session_key)
        self.sdb.wait_for_deleted_entry(SOFT_BFD_STATE_TABLE, session)

    def test_addRemoveBfdSession_ipv6(self, dvs):
        self.setup_db(dvs)
        bfd_session_key = "default:default:2000::2"
        bfdSessions = self.get_exist_bfd_session()

        # Create BFD session
        fieldValues = {"local_addr": "2000::1", "multihop": "true", "multiplier": "3", "tx_interval": "400",
                       "rx_interval": "200"}
        self.create_bfd_session(bfd_session_key, fieldValues)
        self.sdb.wait_for_n_keys(SOFT_BFD_STATE_TABLE, len(bfdSessions) + 1)

        # Check created BFD session in STATE_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()

        # Check STATE_DB entry related to the BFD session
        self.check_state_bfd_session_value(bfd_session_key, fieldValues)

        # Remove the BFD session
        self.remove_bfd_session(bfd_session_key)
        self.sdb.wait_for_deleted_entry(SOFT_BFD_STATE_TABLE, session)

    def test_addRemoveBfdSession_interface(self, dvs):
        self.setup_db(dvs)
        bfd_session_key = "default:Ethernet0:10.0.0.2"
        bfdSessions = self.get_exist_bfd_session()

        # Create BFD session
        fieldValues = {"local_addr": "10.0.0.1", "dst_mac": "00:02:03:04:05:06", "type": "async_passive"}
        self.create_bfd_session("default:Ethernet0:10.0.0.2", fieldValues)
        self.sdb.wait_for_n_keys(SOFT_BFD_STATE_TABLE, len(bfdSessions) + 1)

        # Check created BFD session in STATE_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()

        # Check STATE_DB entry related to the BFD session
        self.check_state_bfd_session_value(bfd_session_key, fieldValues)

        # Remove the BFD session
        self.remove_bfd_session(bfd_session_key)
        self.sdb.wait_for_deleted_entry(SOFT_BFD_STATE_TABLE, session)

    def test_multipleBfdSessions(self, dvs):
        self.setup_db(dvs)
        bfdSessions = self.get_exist_bfd_session()

        # Create BFD session 1
        key1 = "default:default:10.0.0.2"
        fieldValues = {"local_addr": "10.0.0.1"}
        self.create_bfd_session(key1, fieldValues)
        self.sdb.wait_for_n_keys(SOFT_BFD_STATE_TABLE, len(bfdSessions) + 1)

        # Checked BFD session 1 in STATE_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        bfdSessions = self.get_exist_bfd_session()
        assert len(createdSessions) == 1

        session1 = createdSessions.pop()

        # Check STATE_DB entry related to the BFD session
        self.check_state_bfd_session_value(key1, fieldValues)

        # Create BFD session 2
        key2 = "default:default:10.0.1.2"
        fieldValues = {"local_addr": "10.0.0.1", "tx_interval": "300", "rx_interval": "500"}
        self.create_bfd_session(key2, fieldValues)
        self.sdb.wait_for_n_keys(SOFT_BFD_STATE_TABLE, len(bfdSessions) + 1)

        # Check BFD session 2 in STATE_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        bfdSessions = self.get_exist_bfd_session()
        assert len(createdSessions) == 1

        session2 = createdSessions.pop()

        # Check STATE_DB entry related to the BFD session
        self.check_state_bfd_session_value(key2, fieldValues)

        # Create BFD session 3
        key3 = "default:default:2000::2"
        fieldValues = {"local_addr": "2000::1", "type": "demand_active"}
        self.create_bfd_session(key3, fieldValues)
        self.sdb.wait_for_n_keys(SOFT_BFD_STATE_TABLE, len(bfdSessions) + 1)

        # Check BFD session 3 in STATE_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        bfdSessions = self.get_exist_bfd_session()
        assert len(createdSessions) == 1

        session3 = createdSessions.pop()

        # Check STATE_DB entry related to the BFD session
        self.check_state_bfd_session_value(key3, fieldValues)

        # Create BFD session 4
        key4 = "default:default:3000::2"
        fieldValues = {"local_addr": "3000::1"}
        self.create_bfd_session(key4, fieldValues)
        self.sdb.wait_for_n_keys(SOFT_BFD_STATE_TABLE, len(bfdSessions) + 1)

        # Check BFD session 3 in STATE_DB
        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session4 = createdSessions.pop()

        # Check STATE_DB entry related to the BFD session
        self.check_state_bfd_session_value(key4, fieldValues)

        # Remove the BFD sessions
        self.remove_bfd_session(key1)
        self.sdb.wait_for_deleted_entry(SOFT_BFD_STATE_TABLE, session1)
        self.remove_bfd_session(key2)
        self.sdb.wait_for_deleted_entry(SOFT_BFD_STATE_TABLE, session2)
        self.remove_bfd_session(key3)
        self.sdb.wait_for_deleted_entry(SOFT_BFD_STATE_TABLE, session3)
        self.remove_bfd_session(key4)
        self.sdb.wait_for_deleted_entry(SOFT_BFD_STATE_TABLE, session4)
