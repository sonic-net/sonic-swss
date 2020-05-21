import redis
import time
import os
import pytest
from pytest import *
import json
import re
from port_dpb import DPB

maxPorts = 32
maxBreakout = 4
maxRootPorts = maxPorts/maxBreakout
maxAclTables = 16

@pytest.mark.usefixtures('dpb_setup_fixture')
@pytest.mark.usefixtures('dvs_acl_manager')
class TestPortDPBAcl(object):

    '''
    @pytest.mark.skip()
    '''
    def test_acl_table_empty_port_list(self, dvs):

        # Create ACL table "test" and bind it to Ethernet0
        bind_ports = []
        self.dvs_acl.create_acl_table("test", "L3", bind_ports)
        self.dvs_acl.verify_acl_table_count(1)
        self.dvs_acl.verify_acl_group_num(0)

        bind_ports = ["Ethernet0"]
        self.dvs_acl.update_acl_table("test", bind_ports)

        # Verify table, group, and member have been created
        self.dvs_acl.verify_acl_table_count(1)
        self.dvs_acl.verify_acl_group_num(1)
        acl_table_ids = self.dvs_acl.get_acl_table_ids()
        self.dvs_acl.verify_acl_table_ports_binding(bind_ports, acl_table_ids[0])

        bind_ports = []
        self.dvs_acl.update_acl_table("test", bind_ports)
        self.dvs_acl.verify_acl_table_count(1)
        self.dvs_acl.verify_acl_group_num(0)

    '''
    @pytest.mark.skip()
    '''
    def test_one_port_two_acl_tables(self, dvs):

        # Create ACL table "test" and bind it to Ethernet0
        bind_ports = ["Ethernet0"]
        self.dvs_acl.create_acl_table("test", "L3", bind_ports)
        self.dvs_acl.verify_acl_table_count(1)
        self.dvs_acl.verify_acl_group_num(1)
        acl_table_ids = self.dvs_acl.get_acl_table_ids()
        self.dvs_acl.verify_acl_table_ports_binding(bind_ports, acl_table_ids[0])

        # Create ACL table "test1" and bind it to Ethernet0
        bind_ports = ["Ethernet0"]
        self.dvs_acl.create_acl_table("test1", "L3", bind_ports)
        self.dvs_acl.verify_acl_table_count(2)
        self.dvs_acl.verify_acl_group_num(1)
        acl_table_ids = self.dvs_acl.get_acl_table_ids(2)
        self.dvs_acl.verify_acl_table_ports_binding(bind_ports, acl_table_ids[0])
        self.dvs_acl.verify_acl_table_ports_binding(bind_ports, acl_table_ids[1])

        #Delete ACL tables
        self.dvs_acl.remove_acl_table("test")
        self.dvs_acl.verify_acl_table_count(1)
        self.dvs_acl.verify_acl_group_num(1)

        self.dvs_acl.remove_acl_table("test1")
        self.dvs_acl.verify_acl_table_count(0)
        self.dvs_acl.verify_acl_group_num(0)

    '''
    @pytest.mark.skip()
    '''
    def test_one_acl_table_many_ports(self, dvs):

        # Create ACL table and bind it to Ethernet0 and Ethernet4
        bind_ports = ["Ethernet0", "Ethernet4"]
        self.dvs_acl.create_acl_table("test", "L3", bind_ports)
        self.dvs_acl.verify_acl_table_count(1)
        self.dvs_acl.verify_acl_group_num(2)
        acl_table_ids = self.dvs_acl.get_acl_table_ids()
        self.dvs_acl.verify_acl_table_ports_binding(bind_ports, acl_table_ids[0])

        # Update bind list and verify
        bind_ports = ["Ethernet4"]
        self.dvs_acl.update_acl_table("test", bind_ports)
        self.dvs_acl.verify_acl_group_num(1)
        acl_table_ids = self.dvs_acl.get_acl_table_ids()
        self.dvs_acl.verify_acl_table_ports_binding(bind_ports, acl_table_ids[0])

        # Breakout Ethernet0
        dpb = DPB()
        dpb.breakout(dvs, "Ethernet0", maxBreakout)
        time.sleep(2)

        #Update bind list and verify
        bind_ports = ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3","Ethernet4"]
        self.dvs_acl.update_acl_table("test", bind_ports)
        self.dvs_acl.verify_acl_group_num(5)
        self.dvs_acl.verify_acl_table_ports_binding(bind_ports, acl_table_ids[0])

        # Update bind list and verify
        bind_ports = ["Ethernet4"]
        self.dvs_acl.update_acl_table("test", bind_ports)
        self.dvs_acl.verify_acl_group_num(1)
        self.dvs_acl.verify_acl_table_ports_binding(bind_ports, acl_table_ids[0])

        #Breakin Ethernet0, 1, 2, 3
        dpb.breakin(dvs, ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"])
        time.sleep(2)

        # Update bind list and verify
        bind_ports = ["Ethernet0", "Ethernet4"]
        self.dvs_acl.update_acl_table("test", bind_ports)
        self.dvs_acl.verify_acl_group_num(2)
        self.dvs_acl.verify_acl_table_ports_binding(bind_ports, acl_table_ids[0])

        #Delete ACL table
        self.dvs_acl.remove_acl_table("test")
        self.dvs_acl.verify_acl_group_num(0)

    '''
    @pytest.mark.skip()
    '''
    def test_one_port_many_acl_tables(self, dvs):

        # Create 4 ACL tables and bind them to Ethernet0
        bind_ports = ["Ethernet0"]
        acl_tables = ["test1", "test2", "test3", "test4"]
        for acl_tbl in acl_tables:
            self.dvs_acl.create_acl_table(acl_tbl, "L3", bind_ports)

        self.dvs_acl.verify_acl_table_count(len(acl_tables))
        self.dvs_acl.verify_acl_group_num(len(bind_ports))
        acl_table_ids = self.dvs_acl.get_acl_table_ids(len(acl_tables))
        for acl_tbl_id in acl_table_ids:
            self.dvs_acl.verify_acl_table_ports_binding(bind_ports, acl_tbl_id)

        # Update bind list and verify
        bind_ports = []
        for acl_tbl in acl_tables:
            self.dvs_acl.update_acl_table(acl_tbl, bind_ports)

        self.dvs_acl.verify_acl_group_num(0)

        # Breakout Ethernet0
        dpb = DPB()
        dpb.breakout(dvs, "Ethernet0", maxBreakout)

        #Breakin Ethernet0, 1, 2, 3
        dpb.breakin(dvs, ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"])

        for acl_tbl in acl_tables:
            self.dvs_acl.remove_acl_table(acl_tbl)

    '''
    @pytest.mark.skip()
    '''
    def test_many_ports_many_acl_tables(self, dvs):

        # Prepare ACL table names
        aclTableNames = []
        for i in range(maxAclTables):
            aclTableNames.append("aclTable" + str(i+1))

        # Prepare all port names
        portNames = []
        for i in range(maxPorts):
            portNames.append("Ethernet" + str(i))

        # Prepare root port names
        rootPortNames = []
        for i in range(0, maxPorts, maxBreakout):
            rootPortNames.append("Ethernet" + str(i))

        # Create ACL tables and bind root ports
        for aclTable in aclTableNames:
            self.dvs_acl.create_acl_table(aclTable, "L3", rootPortNames)
        self.dvs_acl.verify_acl_group_num(maxRootPorts)

        # Remove the dependency on all root ports by
        # unbinding them from all ACL tables.
        bind_ports = []
        for aclTable in aclTableNames:
            self.dvs_acl.update_acl_table(aclTable, bind_ports)
        self.dvs_acl.verify_acl_group_num(0)

        # Breakout all root ports
        dpb = DPB()
        for pName in rootPortNames:
            #print "Breaking out %s"%pName
            dpb.breakout(dvs, pName, maxBreakout)

        # Add all ports to aclTable1
        self.dvs_acl.update_acl_table(aclTableNames[0], portNames)
        self.dvs_acl.verify_acl_group_num(maxPorts)

        # Remove all ports from aclTable1
        bind_ports = []
        self.dvs_acl.update_acl_table(aclTableNames[0], bind_ports)
        self.dvs_acl.verify_acl_group_num(0)

        # Breakin all ports
        for i in range(0, maxPorts, maxBreakout):
            #print "Breaking in %s"%portNames[i:i+maxBreakout]
            dpb.breakin(dvs, portNames[i:i+maxBreakout])

        for aclTable in aclTableNames:
            self.dvs_acl.remove_acl_table(aclTable)
        self.dvs_acl.verify_acl_table_count(0)
