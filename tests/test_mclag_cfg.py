# Common file to test all MCLAG related changes
from swsscommon import swsscommon
import time
import re
import json
import pytest
import platform
from distutils.version import StrictVersion


def delete_table_keys(db, table):
    tbl =  swsscommon.Table(db, table)
    keys = tbl.getKeys()
    for key in keys:
        tbl.delete(key)

#check table entry exits with this key
def check_table_exists(db, table, key):
    error_info = [ ]
    tbl =  swsscommon.Table(db, table)
    keys = tbl.getKeys()
    if key not in keys:
        error_info.append("The table with desired key %s not found" % key)
        return False, error_info
    return True, error_info

#check table entry doesn't exits with this key
def check_table_doesnt_exists(db, table, key):
    error_info = [ ]
    tbl =  swsscommon.Table(db, table)
    keys = tbl.getKeys()
    if key in keys:
        error_info.append("unexcpected: The table with desired key %s is found" % key)
        return False, error_info
    return True, error_info


def create_mclag_domain(dvs, domain_id, source_ip, peer_ip, peer_link):
    tbl = swsscommon.Table(dvs.cdb, "MCLAG_DOMAIN")
    fvs = swsscommon.FieldValuePairs([("source_ip", source_ip),
                                  ("peer_ip", peer_ip),
				("peer_link", peer_link)])
    tbl.set(domain_id, fvs)
    time.sleep(1)

def remove_mclag_domain(dvs, domain_id):
    tbl = swsscommon.Table(dvs.cdb, "MCLAG_DOMAIN")
    tbl._del(domain_id)
    time.sleep(1)

def add_mclag_domain_field(dvs, domain_id, field, value):
    tbl = swsscommon.Table(dvs.cdb, "MCLAG_DOMAIN")
    fvs = swsscommon.FieldValuePairs([(field, value)])
    tbl.set(domain_id, fvs)
    time.sleep(1)

def create_mclag_interface(dvs, domain_id, mclag_interface):
    tbl = swsscommon.Table(dvs.cdb, "MCLAG_INTERFACE")
    fvs = swsscommon.FieldValuePairs([("if_type", "PortChannel")])
    key_string = domain_id + "|" + mclag_interface
    tbl.set(key_string, fvs)
    time.sleep(1)

def remove_mclag_interface(dvs, domain_id, mclag_interface):
    tbl = swsscommon.Table(dvs.cdb, "MCLAG_INTERFACE")
    key_string = domain_id + "|" + mclag_interface
    tbl._del(key_string)
    time.sleep(1)

# Test MCLAG Configs
class TestMclagConfig(object):
    CFG_MCLAG_DOMAIN_TABLE    = "MCLAG_DOMAIN"
    CFG_MCLAG_INTERFACE_TABLE = "MCLAG_INTERFACE"

    PORTCHANNEL1            = "PortChannel11"
    PORTCHANNEL2            = "PortChannel50"
    PORTCHANNEL3            = "PortChannel51"

    MCLAG_DOMAIN_ID        = "4095"
    MCLAG_SRC_IP           = "10.5.1.1"
    MCLAG_PEER_IP          = "10.5.1.2"
    MCLAG_PEER_LINK        = PORTCHANNEL1

    MCLAG_DOMAIN_2         = "111"

    MCLAG_SESS_TMOUT_VALID_LIST    = ["3","3600"]
    MCLAG_KA_VALID_LIST    = ["1","60"]

    MCLAG_KA_INVALID_LIST            = ["0","61"]
    MCLAG_SESS_TMOUT_INVALID_LIST    = ["0","3601"]

    MCLAG_INTERFACE1       = PORTCHANNEL2
    MCLAG_INTERFACE2       = PORTCHANNEL3


    # Testcase 1 Verify Configuration of MCLAG Domain with src, peer ip and peer link config gets updated in CONFIG_DB
    @pytest.mark.dev_sanity
    def test_mclag_cfg_domain_add(self, dvs, testlog):
        self.cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        #cleanup existing entries
        delete_table_keys(self.cfg_db, self.CFG_MCLAG_DOMAIN_TABLE) 
        delete_table_keys(self.cfg_db, self.CFG_MCLAG_INTERFACE_TABLE) 

        create_mclag_domain(dvs, self.MCLAG_DOMAIN_ID, self.MCLAG_SRC_IP, self.MCLAG_PEER_IP, self.MCLAG_PEER_LINK)
        time.sleep(2)
        
        #check whether domain cfg table contents are same as configured values
        ok,error_info = dvs.all_table_entry_has(self.cfg_db, self.CFG_MCLAG_DOMAIN_TABLE, self.MCLAG_DOMAIN_ID,
                    [ 
                        ("source_ip",self.MCLAG_SRC_IP),
                        ("peer_ip",self.MCLAG_PEER_IP),
                        ("peer_link",self.MCLAG_PEER_LINK)
                    ]                    
                )
        assert ok,error_info


    # Testcase 3 Verify Configuration of MCLAG Interface to existing domain
    @pytest.mark.dev_sanity
    def test_mclag_cfg_intf_add(self, dvs, testlog):
        self.cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        create_mclag_interface(dvs, self.MCLAG_DOMAIN_ID, self.MCLAG_INTERFACE1)
        time.sleep(2)
        
        #check whether mclag interface config is reflected
        key_string = self.MCLAG_DOMAIN_ID + "|" + self.MCLAG_INTERFACE1
        ok,error_info = check_table_exists(self.cfg_db, self.CFG_MCLAG_INTERFACE_TABLE, key_string)
        assert ok,error_info

    # Testcase 4 Verify remove and add mclag interface
    @pytest.mark.dev_sanity
    def test_mclag_cfg_intf_remove_and_add(self, dvs, testlog):
        self.cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
        remove_mclag_interface(dvs, self.MCLAG_DOMAIN_ID, self.MCLAG_INTERFACE1)
        time.sleep(2)
        
        #check whether mclag interface is removed
        key_string = self.MCLAG_DOMAIN_ID + "|" + self.MCLAG_INTERFACE1
        ok,error_info = check_table_doesnt_exists(self.cfg_db, self.CFG_MCLAG_INTERFACE_TABLE, key_string)
        assert ok,error_info

        #add different mclag interface
        create_mclag_interface(dvs, self.MCLAG_DOMAIN_ID, self.MCLAG_INTERFACE2)
        time.sleep(2)

        #check whether new mclag interface is added
        key_string = self.MCLAG_DOMAIN_ID + "|" + self.MCLAG_INTERFACE2
        ok,error_info = check_table_exists(self.cfg_db, self.CFG_MCLAG_INTERFACE_TABLE, key_string)
        assert ok,error_info

    # Testcase 5 Verify Configuration of valid values for session timeout
    @pytest.mark.dev_sanity
    def test_mclag_cfg_session_timeout_valid_values(self, dvs, testlog):
        self.cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        for value in self.MCLAG_SESS_TMOUT_VALID_LIST:
            add_mclag_domain_field(dvs, self.MCLAG_DOMAIN_ID, "session_timeout", value)
            
            time.sleep(2)
            
            #check whether domain cfg table contents are same as configured values
            ok,error_info = dvs.all_table_entry_has(self.cfg_db, self.CFG_MCLAG_DOMAIN_TABLE, self.MCLAG_DOMAIN_ID,
                        [ 
                            ("source_ip",self.MCLAG_SRC_IP),
                            ("peer_ip",self.MCLAG_PEER_IP),
                            ("peer_link",self.MCLAG_PEER_LINK),
                            ("session_timeout",value)
                        ]                    
                    )
            assert ok,error_info

    # Testcase 6 Verify Configuration of valid values for KA timer
    @pytest.mark.dev_sanity
    def test_mclag_cfg_ka_valid_values(self, dvs, testlog):
        self.cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        for value in self.MCLAG_KA_VALID_LIST:
            add_mclag_domain_field(dvs, self.MCLAG_DOMAIN_ID, "keepalive_interval", value)
            time.sleep(2)
            #check whether domain cfg table contents are same as configured values
            ok,error_info = dvs.all_table_entry_has(self.cfg_db, self.CFG_MCLAG_DOMAIN_TABLE, self.MCLAG_DOMAIN_ID,
                        [ 
                            ("source_ip",self.MCLAG_SRC_IP),
                            ("peer_ip",self.MCLAG_PEER_IP),
                            ("peer_link",self.MCLAG_PEER_LINK),
                            ("keepalive_interval",value)
                        ]                    
                    )
            assert ok,error_info


    # Testcase 7 Verify Deletion of MCLAG Domain
    @pytest.mark.dev_sanity
    def test_mclag_cfg_domain_del(self, dvs, testlog):
        self.cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

        remove_mclag_domain(dvs, self.MCLAG_DOMAIN_ID)
        time.sleep(2)
        
        #check whether domain cfg table contents are same as configured values
        ok, error_info = check_table_doesnt_exists(self.cfg_db, self.CFG_MCLAG_DOMAIN_TABLE, self.MCLAG_DOMAIN_ID)
        assert ok,error_info

        #make sure mclag interface tables entries are also deleted when mclag domain is deleted
        key_string = self.MCLAG_DOMAIN_ID 
        ok,error_info = check_table_doesnt_exists(self.cfg_db, self.CFG_MCLAG_INTERFACE_TABLE, key_string)
        assert ok,error_info

