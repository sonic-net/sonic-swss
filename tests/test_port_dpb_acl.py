from swsscommon import swsscommon
import redis
import time
import os
import pytest
from pytest import *
import json
import re
from port_dpb import DPB

@pytest.mark.usefixtures('dpb_setup_fixture')
class TestPortDPBAcl(object):

    '''
    @pytest.mark.skip()
    '''
    def test_acl_table_empty_port_list(self, dvs):
        dvs.setup_db()

        # Create ACL table "test" and bind it to Ethernet0
        bind_ports = []
        dvs.create_acl_table("test", "L3", bind_ports)
        time.sleep(2)
        acl_table_ids = dvs.get_acl_table_ids()
        assert len(acl_table_ids) == 1
        dvs.verify_acl_group_num(0)
        acl_group_ids = dvs.get_acl_group_ids()
        assert len(acl_group_ids) == 0

        bind_ports = ["Ethernet0"]
        fvs = swsscommon.FieldValuePairs([("ports", ",".join(bind_ports))])
        dvs.update_acl_table("test", fvs)
        time.sleep(2)
        acl_table_ids = dvs.get_acl_table_ids()
        assert len(acl_table_ids) == 1
        dvs.verify_acl_group_num(1)
        acl_group_ids = dvs.get_acl_group_ids()
        assert len(acl_group_ids) == 1
        dvs.verify_acl_group_member(acl_group_ids[0], acl_table_ids[0])
        dvs.verify_acl_port_binding(bind_ports)

        bind_ports = []
        fvs = swsscommon.FieldValuePairs([("ports", ",".join(bind_ports))])
        dvs.update_acl_table("test", fvs)
        time.sleep(2)
        acl_table_ids = dvs.get_acl_table_ids()
        assert len(acl_table_ids) == 1
        dvs.verify_acl_group_num(0)
        acl_group_ids = dvs.get_acl_group_ids()
        assert len(acl_group_ids) == 0

    '''
    @pytest.mark.skip()
    '''
    def test_one_port_two_acl_tables(self, dvs):
        dvs.setup_db()

        # Create ACL table "test" and bind it to Ethernet0
        bind_ports = ["Ethernet0"]
        dvs.create_acl_table("test", "L3", bind_ports)
        time.sleep(2)
        acl_table_ids = dvs.get_acl_table_ids()
        assert len(acl_table_ids) == 1
        dvs.verify_acl_group_num(1)
        acl_group_ids = dvs.get_acl_group_ids()
        assert len(acl_group_ids) == 1
        dvs.verify_acl_group_member(acl_group_ids[0], acl_table_ids[0])
        dvs.verify_acl_port_binding(bind_ports)

        # Create ACL table "test1" and bind it to Ethernet0
        bind_ports = ["Ethernet0"]
        dvs.create_acl_table("test1", "L3", bind_ports)
        time.sleep(2)
        acl_table_ids = dvs.get_acl_table_ids()
        assert len(acl_table_ids) == 2
        dvs.verify_acl_group_num(1)
        dvs.verify_acl_group_member(acl_group_ids[0], acl_table_ids[0])
        dvs.verify_acl_group_member(acl_group_ids[0], acl_table_ids[1])
        dvs.verify_acl_port_binding(bind_ports)

        #Delete ACL tables
        dvs.remove_acl_table("test")
        time.sleep(2)
        dvs.verify_acl_group_num(1)
        dvs.remove_acl_table("test1")
        time.sleep(2)
        dvs.verify_acl_group_num(0)

    '''
    @pytest.mark.skip()
    '''
    def test_one_acl_table_many_ports(self, dvs):
        dvs.setup_db()

        # Create ACL table and bind it to Ethernet0 and Ethernet4
        bind_ports = ["Ethernet0", "Ethernet4"]
        dvs.create_acl_table("test", "L3", bind_ports)
        time.sleep(2)
        acl_table_ids = dvs.get_acl_table_ids()
        assert len(acl_table_ids) == 1
        dvs.verify_acl_group_num(2)
        acl_group_ids = dvs.get_acl_group_ids()
        dvs.verify_acl_group_member(acl_group_ids[0], acl_table_ids[0])
        dvs.verify_acl_group_member(acl_group_ids[1], acl_table_ids[0])
        dvs.verify_acl_port_binding(bind_ports)

        # Update bind list and verify
        bind_ports = ["Ethernet4"]
        fvs = swsscommon.FieldValuePairs([("ports", ",".join(bind_ports))])
        dvs.update_acl_table("test", fvs)
        time.sleep(2)
        dvs.verify_acl_group_num(1)
        acl_group_ids = dvs.get_acl_group_ids()
        dvs.verify_acl_group_member(acl_group_ids[0], acl_table_ids[0])
        dvs.verify_acl_port_binding(bind_ports)

        # Breakout Ethernet0
        dpb = DPB()
        dpb.breakout(dvs, "Ethernet0", 4)
        time.sleep(2)

        #Update bind list and verify
        bind_ports = ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3","Ethernet4"]
        fvs = swsscommon.FieldValuePairs([("ports", ",".join(bind_ports))])
        dvs.update_acl_table("test", fvs)
        time.sleep(2)
        dvs.verify_acl_group_num(5)
        acl_group_ids = dvs.get_acl_group_ids()
        dvs.verify_acl_group_member(acl_group_ids[0], acl_table_ids[0])
        dvs.verify_acl_group_member(acl_group_ids[1], acl_table_ids[0])
        dvs.verify_acl_group_member(acl_group_ids[2], acl_table_ids[0])
        dvs.verify_acl_group_member(acl_group_ids[3], acl_table_ids[0])
        dvs.verify_acl_group_member(acl_group_ids[4], acl_table_ids[0])
        dvs.verify_acl_port_binding(bind_ports)
        time.sleep(2)

        # Update bind list and verify
        bind_ports = ["Ethernet4"]
        fvs = swsscommon.FieldValuePairs([("ports", ",".join(bind_ports))])
        dvs.update_acl_table("test", fvs)
        dvs.verify_acl_group_num(1)
        acl_group_ids = dvs.get_acl_group_ids()
        dvs.verify_acl_group_member(acl_group_ids[0], acl_table_ids[0])
        dvs.verify_acl_port_binding(bind_ports)

        #Breakin Ethernet0, 1, 2, 3
        dpb.breakin(dvs, ["Ethernet0", "Ethernet1", "Ethernet2", "Ethernet3"])
        time.sleep(2)

        # Update bind list and verify
        bind_ports = ["Ethernet0", "Ethernet4"]
        fvs = swsscommon.FieldValuePairs([("ports", ",".join(bind_ports))])
        dvs.update_acl_table("test", fvs)
        time.sleep(2)
        dvs.verify_acl_group_num(2)
        acl_group_ids = dvs.get_acl_group_ids()
        dvs.verify_acl_group_member(acl_group_ids[0], acl_table_ids[0])
        dvs.verify_acl_group_member(acl_group_ids[1], acl_table_ids[0])
        dvs.verify_acl_port_binding(bind_ports)

        #Delete ACL table
        dvs.remove_acl_table("test")
        time.sleep(2)
        dvs.verify_acl_group_num(0)

    @pytest.mark.skip()
    def test_one_port_many_acl_tables(self, dvs):
        #TBD
        return
