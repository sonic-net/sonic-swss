from swsscommon import swsscommon
import os
import sys
import time
import json
from distutils.version import StrictVersion
import pytest

class TestStormControl(object):
    def setup_db(self,dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        self.sdb = swsscommon.DBConnector(6, dvs.redis_sock, 0)
        dvs.runcmd(['sh', '-c', "echo 0 > /var/log/syslog"])

    def create_port_channel(self, dvs, lag_name):
        dvs.runcmd("config portchannel add " + lag_name)
        time.sleep(1)

    def delete_port_channel(self, dvs, lag_name):
        dvs.runcmd("config portchannel del " + lag_name)
        time.sleep(1)

    def add_port_channel_member(self, dvs, lag_name, member):
        dvs.runcmd("config portchannel member add "+ lag_name + " "+ member)
        time.sleep(1)

    def remove_port_channel_member(self, dvs, lag_name, member):
        dvs.runcmd("config portchannel member del "+ lag_name + " "+ member)
        time.sleep(1)

    def create_vlan(self, dvs, vlan):
        dvs.runcmd("config vlan add " + vlan)
        time.sleep(1)

    def delete_vlan(self, dvs, vlan):
        dvs.runcmd("config vlan del " + vlan)
        time.sleep(1)

    def add_vlan_member(self, dvs, vlan, interface):
        dvs.runcmd("config vlan member add " + vlan + " " + interface)
        time.sleep(1)

    def remove_vlan_member(self, dvs, vlan, interface):
        dvs.runcmd("config vlan member del " + vlan + " " + interface)
        time.sleep(1)

    def add_storm_session(self, if_name, storm_type, kbps_value):
        tbl = swsscommon.Table(self.cdb, "PORT_STORM_CONTROL")
        fvs = swsscommon.FieldValuePairs([("kbps", str(kbps_value))])
        key = if_name + "|" + storm_type
        tbl.set(key,fvs)
        time.sleep(1)

    def delete_storm_session(self, if_name, storm_type):
        tbl = swsscommon.Table(self.cdb, "PORT_STORM_CONTROL")
        key = if_name + "|" + storm_type
        tbl._del(key)
        time.sleep(1)

    def test_bcast_storm(self,dvs,testlog):
        self.setup_db(dvs)

        if_name = "Ethernet0"
        storm_type = "broadcast"
        #User input is Kbps
        #Orchagent converts the value to CIR as below and programs the ASIC DB
        #kbps_value * 1000 / 8
        kbps_value = 1000000
        self.add_storm_control_on_interface(dvs,if_name,storm_type,kbps_value)
        self.del_storm_control(dvs,if_name,storm_type)

    def del_storm_control(self, dvs, if_name, storm_type):
        self.setup_db(dvs)
        port_oid = dvs.asicdb.portnamemap[if_name]
        atbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        status, fvs = atbl.get(dvs.asicdb.portnamemap[if_name])
        assert status == True

        storm_type_port_attr = self.get_port_attr_for_storm_type(storm_type)
        
        policer_oid = 0
        for fv in fvs:
            if fv[0] == storm_type_port_attr:
                policer_oid = fv[1]

        self.delete_storm_session(if_name, storm_type)
        tbl = swsscommon.Table(self.cdb, "PORT_STORM_CONTROL")
        (status,fvs) = tbl.get(if_name+"|"+storm_type)
        assert status == False

        atbl = swsscommon.Table(self.adb,"ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        status, fvs = atbl.get(dvs.asicdb.portnamemap[if_name])
        assert status == True

        for fv in fvs:
            if fv[0] == storm_type_port_attr:
                assert fv[1] == "oid:0x0"

        if policer_oid != 0:
            atbl = swsscommon.Table(self.adb,"ASIC_STATE:SAI_OBJECT_TYPE_POLICER")
            status, fvs = atbl.get(policer_oid)
            assert status == False

    def test_uucast_storm(self,dvs,testlog):
        self.setup_db(dvs)

        if_name = "Ethernet0"
        storm_type = "unknown-unicast"
        #User input is Kbps
        #Orchagent converts the value to CIR as below and programs the ASIC DB
        #kbps_value * 1000 / 8
        kbps_value = 1000000

        self.add_storm_control_on_interface(dvs,if_name,storm_type,kbps_value)
        self.del_storm_control(dvs,if_name,storm_type)

    def test_umcast_storm(self,dvs,testlog):
        self.setup_db(dvs)

        if_name = "Ethernet0"
        storm_type = "unknown-multicast"
        #User input is Kbps
        #Orchagent converts the value to CIR as below and programs the ASIC DB
        #kbps_value * 1000 / 8
        kbps_value = 1000000

        self.add_storm_control_on_interface(dvs,if_name,storm_type,kbps_value)
        self.del_storm_control(dvs,if_name,storm_type)

    def get_port_attr_for_storm_type(self,storm_type):
        port_attr = ""
        if storm_type == "broadcast":
            port_attr = "SAI_PORT_ATTR_BROADCAST_STORM_CONTROL_POLICER_ID"
        elif storm_type == "unknown-unicast":
            port_attr = "SAI_PORT_ATTR_FLOOD_STORM_CONTROL_POLICER_ID"
        elif storm_type == "unknown-multicast":
            port_attr = "SAI_PORT_ATTR_MULTICAST_STORM_CONTROL_POLICER_ID"

        return port_attr

    def check_storm_control_on_interface(self,dvs,if_name,storm_type,kbps_value):
        print ("interface {} storm_type {} kbps {}".format(if_name,storm_type, kbps_value))
        tbl = swsscommon.Table(self.cdb,"PORT_STORM_CONTROL")
        (status,fvs) = tbl.get(if_name+"|"+storm_type)

        assert status == True
        assert len(fvs) > 0

        port_oid = dvs.asicdb.portnamemap[if_name]

        atbl = swsscommon.Table(self.adb,"ASIC_STATE:SAI_OBJECT_TYPE_PORT")
        status, fvs = atbl.get(dvs.asicdb.portnamemap[if_name])
        assert status == True

        policer_oid = 0

        storm_type_port_attr = self.get_port_attr_for_storm_type(storm_type)

        for fv in fvs:
            if fv[0] == storm_type_port_attr:
                assert fv[1] != "oid:0x0"
                policer_oid = fv[1]

        if policer_oid != 0:
            atbl = swsscommon.Table(self.adb,"ASIC_STATE:SAI_OBJECT_TYPE_POLICER")
            status, fvs = atbl.get(policer_oid)
            assert status == True

        bps = 0

        for fv in fvs:
            if fv[0] == "SAI_POLICER_ATTR_CIR":
                bps = fv[1]

        #Retrieved value of bps from ASIC_DB is converted back to user input kbps
        kbps = int(int(bps) / int(1000) * 8)
        print ("Kbps value {}".format(kbps))

        assert str(kbps) == str(kbps_value)


    def add_storm_control_on_interface(self,dvs,if_name,storm_type,kbps_value):
        print ("interface {} storm_type {} kbps {}".format(if_name,storm_type,kbps_value))
        self.add_storm_session(if_name, storm_type, kbps_value)
        self.check_storm_control_on_interface(dvs,if_name,storm_type,kbps_value)

    def test_add_storm_all_interfaces(self,dvs,testlog):
        self.setup_db(dvs)

        tbl = swsscommon.Table(self.cdb,"PORT")
        for key in tbl.getKeys():
            self.add_storm_control_on_interface(dvs,key,"broadcast",1000000)
            self.add_storm_control_on_interface(dvs,key,"unknown-unicast",2000000)
            self.add_storm_control_on_interface(dvs,key,"unknown-multicast",3000000)
            self.del_storm_control(dvs,key,"broadcast")
            self.del_storm_control(dvs,key,"unknown-unicast")
            self.del_storm_control(dvs,key,"unknown-multicast")

    def test_warm_restart_all_interfaces(self,dvs,testlog):
        self.setup_db(dvs)

        tbl = swsscommon.Table(self.cdb,"PORT")
        for key in tbl.getKeys():
            self.add_storm_control_on_interface(dvs,key,"broadcast",1000000)
            self.add_storm_control_on_interface(dvs,key,"unknown-unicast",2000000)
            self.add_storm_control_on_interface(dvs,key,"unknown-multicast",3000000)
        #dvs.runcmd("config save -y")
        # enable warm restart
        dvs.warm_restart_swss("true")

        # freeze orchagent for warm restart
        (exitcode, result) = dvs.runcmd("/usr/bin/orchagent_restart_check", include_stderr=False)
        assert result == "RESTARTCHECK succeeded\n"
        time.sleep(2)

        dvs.stop_swss()
        time.sleep(10)
        dvs.start_swss()
        time.sleep(10)

        for key in tbl.getKeys():
            self.check_storm_control_on_interface(dvs,key,"broadcast",1000000)
            self.check_storm_control_on_interface(dvs,key,"unknown-unicast",2000000)
            self.check_storm_control_on_interface(dvs,key,"unknown-multicast",3000000)
            self.del_storm_control(dvs,key,"broadcast")
            self.del_storm_control(dvs,key,"unknown-unicast")
            self.del_storm_control(dvs,key,"unknown-multicast")
        # disable warm restart
        dvs.warm_restart_swss("false")

    def test_add_storm_lag_interface(self,dvs,testlog):
        self.setup_db(dvs)
        lag_name = "PortChannel10"
        member_interface = "Ethernet0"
        kbps_value = 1000000
        storm_list = ["broadcast","unknown-unicast","unknown-multicast"]
        kbps_value_list = [1000000,2000000,3000000]

        #Create LAG interface and add member
        self.create_port_channel(dvs,lag_name)
        self.add_port_channel_member(dvs,lag_name,member_interface)

        #click CLI verification
        #for storm_type in storm_list:
        #    dvs.runcmd("config interface storm-control add "+lag_name+" "+storm_type+" "+str(kbps_value))
        #    tbl = swsscommon.Table(self.cdb,"PORT_STORM_CONTROL")
        #    (status,fvs) = tbl.get(lag_name+"|"+storm_type)
        #    assert status == False
        #    assert len(fvs) == 0

        #Orchagent verification
        storm_list_db = ["broadcast","unknown-unicast","unknown-multicast"]
        for storm_type,kbps_value in zip(storm_list_db,kbps_value_list):
            #Cleanup syslog
            dvs.runcmd(['sh', '-c', "echo 0 > /var/log/syslog"])
            time.sleep(1)
            print ("storm type: {} kbps value: {}".format(storm_type,kbps_value))
            #Add storm entry to config DB directly
            self.add_storm_session(lag_name,storm_type,kbps_value)
            tbl = swsscommon.Table(self.cdb,"PORT_STORM_CONTROL")
            (status,fvs) = tbl.get(lag_name+"|"+storm_type)
            assert status == True
            assert len(fvs) > 0
            time.sleep(1)
            #grep for error message in syslog
            (exitcode,num) = dvs.runcmd(['sh', '-c', 'cat /var/log/syslog | grep -i "handlePortStormControlTable: {}: Unsupported / Invalid interface PortChannel10"'.format(storm_type)])
            time.sleep(1)
            assert exitcode == 0
            self.delete_storm_session(lag_name, storm_type)
        self.remove_port_channel_member(dvs,lag_name,member_interface)
        self.delete_port_channel(dvs,lag_name)

    def test_add_storm_vlan_interface(self,dvs,testlog):
        self.setup_db(dvs)
        vlan_id = 99
        member_interface = "Ethernet4"
        kbps_value = 1000000
        storm_list = ["broadcast","unknown-unicast","unknown-multicast"]
        kbps_value_list = [1000000,2000000,3000000]
        vlan_name = "Vlan"+str(vlan_id)

        #Create VLAN interface and add member
        self.create_vlan(dvs,str(vlan_id))
        self.add_vlan_member(dvs,str(vlan_id),member_interface)

        #click CLI verification 
        #for storm_type in storm_list:
        #    dvs.runcmd("config interface storm-control add Vlan"+str(vlan_id)+" "+storm_type+" "+str(kbps_value))
        #    tbl = swsscommon.Table(self.cdb,"PORT_STORM_CONTROL")
        #    (status,fvs) = tbl.get("Vlan"+str(vlan_id)+"|"+storm_type)
        #    assert status == False
        #    assert len(fvs) == 0

        #Orchagent verification
        storm_list_db = ["broadcast","unknown-unicast","unknown-multicast"]
        for storm_type,kbps_value in zip(storm_list_db,kbps_value_list):
            #Cleanup syslog
            dvs.runcmd(['sh', '-c', "echo 0 > /var/log/syslog"])
            time.sleep(1)
            print ("storm type: {} kbps value: {}".format(storm_type,kbps_value))
            #Add storm entry to config DB directly
            self.add_storm_session(vlan_name,storm_type,kbps_value)
            tbl = swsscommon.Table(self.cdb,"PORT_STORM_CONTROL")
            (status,fvs) = tbl.get(vlan_name+"|"+storm_type)
            assert status == True
            assert len(fvs) > 0
            time.sleep(1)
            #grep for error message in syslog
            (exitcode,num) = dvs.runcmd(['sh', '-c', 'cat /var/log/syslog | grep -i "handlePortStormControlTable: {}: Unsupported / Invalid interface {}"'.format(storm_type,vlan_name)])
            time.sleep(1)
            assert exitcode == 0
            self.delete_storm_session(vlan_name, storm_type)
        self.remove_vlan_member(dvs,str(vlan_id),member_interface)
        self.delete_vlan(dvs,str(vlan_id))
