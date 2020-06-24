# Common file to test all MCLAG related changes
from swsscommon import swsscommon
import time
import re
import json
import pytest
import platform
from distutils.version import StrictVersion

# Create port-channel
def create_portchannel(table, po_name):
    fvs = swsscommon.FieldValuePairs([("admin", "up"), ("mtu", "1500")])
    table.set(po_name, fvs)
    time.sleep(1)

# Delete port-channel
def delete_portchannel(table, po_name):
    table._del(po_name)
    time.sleep(1)

# Create port-channel member
def create_portchannel_member(table, po_name, po_member_name):
    fvs = swsscommon.FieldValuePairs([("status", "enabled")])
    table.set(po_name + ":" + po_member_name, fvs)
    time.sleep(1)

# Delete port-channel member
def delete_portchannel_member(table, po_name, po_member_name):
    table._del(po_name + ":" + po_member_name)
    time.sleep(1)


# Test newly introduce traffic_disable attribute in APP_DB LAG table
class TestPortChannelTrafficDisable(object):

    APP_LAG_TABLE          = "LAG_TABLE"
    APP_LAG_MEMBER_TABLE   = "LAG_MEMBER_TABLE"
    ASIC_LAG_TABLE         = "ASIC_STATE:SAI_OBJECT_TYPE_LAG"
    ASIC_LAG_MEMBER_TABLE  = "ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER"

    PORTCHANNEL_NAME       = "PortChannel100"
    PORTCHANNEL_MEMBER1    = "Ethernet0"
    PORTCHANNEL_MEMBER2    = "Ethernet4"

    # Set LAG traffic_disable attribute
    def set_portchannel_trafficdisable(self, table, po_name, bool_str):
        fvs = swsscommon.FieldValuePairs([("traffic_disable", bool_str)])
        table.set(po_name, fvs)
        time.sleep(1)

    def check_portchannel_member_in_asicdb(self, dvs, po_member_name):
        asic_lag_table = swsscommon.Table(self.asic_db, self.ASIC_LAG_TABLE)
        lag_entries = asic_lag_table.getKeys()
        assert len(lag_entries) == 1, "No LAG entry in ASIC_DB"

        asic_lag_mbr_table = swsscommon.Table(self.asic_db, self.ASIC_LAG_MEMBER_TABLE)
        lag_mem_entries = asic_lag_mbr_table.getKeys()
        if len(lag_mem_entries) == 0:
            return False
        for i in range(len(lag_mem_entries)):
            (status, fvs) = asic_lag_mbr_table.get(lag_mem_entries[i])
            for fv in fvs:
                if fv[0] == "SAI_LAG_MEMBER_ATTR_LAG_ID":
                    assert fv[1] == lag_entries[0]
                elif fv[0] == "SAI_LAG_MEMBER_ATTR_PORT_ID":
                    if dvs.asicdb.portoidmap[fv[1]] == po_member_name:
                        return True
        return False

    # Test traffic_disable attribute setting on LAG table entry in APP_DB
    # 1. Create LAG with 1 member. Verify LAG and LAG member are added to ASIC_DB
    # 2. Set traffic disable to true for LAG. Verify LAG member is removed from ASIC_DB
    # 3. Add a second LAG member. Verify it is not added to ASIC_DB
    # 4. Set traffic disable to false for LAG. Verify both LAG members are added to ASIC_DB
    # 5. Delete LAG and its two members
    def test_portchannel_traffic_disable(self, dvs, testlog):
        self.app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        self.asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        lag_table = swsscommon.ProducerStateTable(self.app_db, self.APP_LAG_TABLE)
        lag_member_table = swsscommon.ProducerStateTable(self.app_db, self.APP_LAG_MEMBER_TABLE)

        # Test1: verify default traffic_disable is not set
        create_portchannel(lag_table, self.PORTCHANNEL_NAME)
        create_portchannel_member(lag_member_table, self.PORTCHANNEL_NAME, self.PORTCHANNEL_MEMBER1)
        status = self.check_portchannel_member_in_asicdb(dvs, self.PORTCHANNEL_MEMBER1)
        assert status == True, "LAG member is not added to ASIC_DB"

        # Test2: verify LAG member is removed from ASIC_DB when traffic_disable
        #        attribute is set to true
        self.set_portchannel_trafficdisable(lag_table, self.PORTCHANNEL_NAME, "true")
        status = self.check_portchannel_member_in_asicdb(dvs, self.PORTCHANNEL_MEMBER1)
        assert status == False, "LAG member is not removed from ASIC_DB"

        # Test3: verify new LAG member is not added to ASIC_DB when traffic_disable
        #        attribute is set to true
        create_portchannel_member(lag_member_table, self.PORTCHANNEL_NAME, self.PORTCHANNEL_MEMBER2)
        status = self.check_portchannel_member_in_asicdb(dvs, self.PORTCHANNEL_MEMBER2)
        assert status == False, "LAG member is incorrectly added to ASIC_DB"

        # Test4: verify both LAG members are added to ASIC_DB whent traffic_disable
        #        attribute is set to false
        self.set_portchannel_trafficdisable(lag_table, self.PORTCHANNEL_NAME, "false")
        status = self.check_portchannel_member_in_asicdb(dvs, self.PORTCHANNEL_MEMBER1)
        assert status == True, "LAG member Ethernet0 is not added to ASIC_DB"
        status = self.check_portchannel_member_in_asicdb(dvs, self.PORTCHANNEL_MEMBER2)
        assert status == True,  "LAG member Ethernet4 is not added to ASIC_DB"

        # Cleanup
        delete_portchannel_member(lag_member_table, self.PORTCHANNEL_NAME, self.PORTCHANNEL_MEMBER1)
        delete_portchannel_member(lag_member_table, self.PORTCHANNEL_NAME, self.PORTCHANNEL_MEMBER2)
        delete_portchannel(lag_table, self.PORTCHANNEL_NAME)
