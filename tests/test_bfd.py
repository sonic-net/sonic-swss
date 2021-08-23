import pytest
import time

from swsscommon import swsscommon

class TestBfd(object):
    def setup_db(self, dvs):
        self.pdb = dvs.get_app_db()
        self.adb = dvs.get_asic_db()

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

    def test_addRemoveBfdSession(self, dvs):
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        fieldValues = {"local_addr": "10.0.0.1"}
        self.create_bfd_session("default:10.0.0.2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4"
        }
        self.check_asic_bfd_session_value(session, expected_values)

        self.remove_bfd_session("default:10.0.0.2")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)

    def test_addRemoveBfdSession_ipv6(self, dvs):
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        fieldValues = {"local_addr": "2000::1"}
        self.create_bfd_session("default:2000::2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "2000::1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "2000::2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "6"
        }
        self.check_asic_bfd_session_value(session, expected_values)

        self.remove_bfd_session("default:2000::2")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)

    def test_addRemoveBfdSession_interface(self, dvs):
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        fieldValues = {"local_addr": "10.0.0.1", "dst_mac": "00:02:03:04:05:06"}
        self.create_bfd_session("Ethernet0:10.0.0.2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4",
            "SAI_BFD_SESSION_ATTR_HW_LOOKUP_VALID": "false",
            "SAI_BFD_SESSION_ATTR_DST_MAC_ADDRESS": "00:02:03:04:05:06"
        }
        self.check_asic_bfd_session_value(session, expected_values)

        self.remove_bfd_session("Ethernet0:10.0.0.2")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)

    def test_addRemoveBfdSession_txrx_interval(self, dvs):
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        fieldValues = {"local_addr": "10.0.0.1", "tx_interval": "300", "rx_interval": "500"}
        self.create_bfd_session("default:10.0.0.2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4",
            "SAI_BFD_SESSION_ATTR_MIN_TX": "300000",
            "SAI_BFD_SESSION_ATTR_MIN_RX": "500000",
        }
        self.check_asic_bfd_session_value(session, expected_values)

        self.remove_bfd_session("default:10.0.0.2")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)

    def test_addRemoveBfdSession_multiplier(self, dvs):
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        fieldValues = {"local_addr": "10.0.0.1", "multiplier": "5"}
        self.create_bfd_session("default:10.0.0.2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4",
            "SAI_BFD_SESSION_ATTR_MULTIPLIER": "5"
        }
        self.check_asic_bfd_session_value(session, expected_values)

        self.remove_bfd_session("default:10.0.0.2")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)

    def test_addRemoveBfdSession_multihop(self, dvs):
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        fieldValues = {"local_addr": "10.0.0.1", "multihop": "true"}
        self.create_bfd_session("default:10.0.0.2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_ASYNC_ACTIVE",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4",
            "SAI_BFD_SESSION_ATTR_MULTIHOP": "true"
        }
        self.check_asic_bfd_session_value(session, expected_values)

        self.remove_bfd_session("default:10.0.0.2")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)

    def test_addRemoveBfdSession_type(self, dvs):
        self.setup_db(dvs)

        bfdSessions = self.get_exist_bfd_session()

        fieldValues = {"local_addr": "10.0.0.1", "type": "demand_active"}
        self.create_bfd_session("default:10.0.0.2", fieldValues)
        self.adb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", len(bfdSessions) + 1)

        createdSessions = self.get_exist_bfd_session() - bfdSessions
        assert len(createdSessions) == 1

        session = createdSessions.pop()
        expected_values = {
            "SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS": "10.0.0.1",
            "SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS": "10.0.0.2",
            "SAI_BFD_SESSION_ATTR_TYPE": "SAI_BFD_SESSION_TYPE_DEMAND_ACTIVE",
            "SAI_BFD_SESSION_ATTR_IPHDR_VERSION": "4"
        }
        self.check_asic_bfd_session_value(session, expected_values)

        self.remove_bfd_session("default:10.0.0.2")
        self.adb.wait_for_deleted_entry("ASIC_STATE:SAI_OBJECT_TYPE_BFD_SESSION", session)
