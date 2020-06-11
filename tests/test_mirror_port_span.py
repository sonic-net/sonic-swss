# This test suite covers the functionality of mirror feature in SwSS
import platform
import pytest
import time

from swsscommon import swsscommon
from distutils.version import StrictVersion


class TestMirror(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        self.sdb = swsscommon.DBConnector(6, dvs.redis_sock, 0)

    def set_interface_status(self, dvs, interface, admin_status):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN"
        else:
            tbl_name = "PORT"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        fvs = swsscommon.FieldValuePairs([("admin_status", "up")])
        tbl.set(interface, fvs)
        time.sleep(1)

        # when using FRR, route cannot be inserted if the neighbor is not
        # connected. thus it is mandatory to force the interface up manually
        if interface.startswith("PortChannel"):
            dvs.runcmd("bash -c 'echo " + ("1" if admin_status == "up" else "0") +\
                    " > /sys/class/net/" + interface + "/carrier'")

    def getPortOid(self, dvs, port_name):

        cnt_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
        port_map_tbl = swsscommon.Table(cnt_db, 'COUNTERS_PORT_NAME_MAP')

        for k in port_map_tbl.get('')[1]:
            if k[0] == port_name:
                return k[1]


    def getPortAttr(self, dvs, port_oid, port_attr):

        asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
        port_tbl = swsscommon.Table(asic_db, 'ASIC_STATE:SAI_OBJECT_TYPE_PORT:{0}'.format(port_oid))

        for k in port_tbl.get('')[1]:
            if k[0] == port_attr:
                return k[1]

    def create_mirror_span_session(self, name, dst_port, src_port=None, direction=None, queue=None, policer=None):
        tbl = swsscommon.Table(self.cdb, "MIRROR_SESSION")
        if src_port is None and direction is None:
            if queue is None and policer is None:
                fvs = swsscommon.FieldValuePairs([("type", "SPAN"),
                                                    ("dst_port", dst_port)])
            elif queue is None:
                fvs = swsscommon.FieldValuePairs([("type", "SPAN"),
                                                ("dst_port", dst_port),
                                                ("policer", policer)])
            elif policer is None:
                fvs = swsscommon.FieldValuePairs([("type", "SPAN"),
                                                ("dst_port", dst_port),
                                                ("queue", queue)])
            else:
                fvs = swsscommon.FieldValuePairs([("type", "SPAN"),
                                                ("dst_port", dst_port),
                                                ("queue", queue),
                                                ("policer", policer)])
        else:
            if queue is None and policer is None:
                fvs = swsscommon.FieldValuePairs([("type", "SPAN"),
                                                  ("dst_port", dst_port),
                                                  ("src_port", src_port),
                                                  ("direction", direction)])
            elif queue is None:
                fvs = swsscommon.FieldValuePairs([("type", "SPAN"),
                                                  ("dst_port", dst_port),
                                                  ("src_port", src_port),
                                                  ("direction", direction),
                                                  ("policer", policer)])
            elif policer is None:
                fvs = swsscommon.FieldValuePairs([("type", "SPAN"),
                                                  ("dst_port", dst_port),
                                                  ("src_port", src_port),
                                                  ("direction", direction),
                                                  ("queue", queue)])
        tbl.set(name, fvs)
        time.sleep(2)

    def remove_mirror_session(self, name):
        tbl = swsscommon.Table(self.cdb, "MIRROR_SESSION")
        tbl._del(name)
        time.sleep(1)

    def get_mirror_session_status(self, name):
        return self.get_mirror_session_state(name)["status"]

    def get_mirror_session_state(self, name):
        tbl = swsscommon.Table(self.sdb, "MIRROR_SESSION_TABLE")
        (status, fvs) = tbl.get(name)
        assert status == True
        assert len(fvs) > 0
        return { fv[0]: fv[1] for fv in fvs }
    
    def check_mirror_session_state(self, name, status):
        tbl = swsscommon.Table(self.sdb, "MIRROR_SESSION_TABLE")
        (session_status, fvs) = tbl.get(name)
        assert session_status == status

    @pytest.mark.skipif(StrictVersion(platform.linux_distribution()[1]) <= StrictVersion('8.9'), reason="Debian 8.9 or before has no support")
    def test_PortMirrorAddRemove(self, dvs, testlog):
        """
        This test covers the basic SPAN mirror session creation and removal operations
        Operation flow:
        1. Create mirror session with only dst_port , verify session becomes active.
        2. Create mirror session with invalid dst_port, verify session doesnt get created.
        3. Create mirror session with invalid source port, verify session doesnt get created.
        4. Create mirror session with source port, verify session becomes active
        """
        self.setup_db(dvs)

        session = "TEST_SESSION"
        src_port = "Ethernet12"
        dst_port = "Ethernet16"
        invld_port = "Invalid"
        src_oid = self.getPortOid(dvs, src_port)

        # Sub Test 1
        # create mirror session
        self.create_mirror_span_session(session, "Ethernet16")
        assert self.get_mirror_session_state(session)["status"] == "active"

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        mirror_entries = tbl.getKeys()
        assert len(mirror_entries) == 1

        # verify asicdb
        (status, fvs) = tbl.get(mirror_entries[0])
        assert status == True
        assert len(fvs) == 2
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet16"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TYPE":
                assert fv[1] == "SAI_MIRROR_SESSION_TYPE_LOCAL"
    
        # remove mirror session
        self.remove_mirror_session(session)

        # Sub Test 2
        # create mirror session with invalid dst_port
        self.create_mirror_span_session(session, invld_port)
        self.check_mirror_session_state(session, False)

        # Sub Test 3
        # create mirror session with invalid src_port
        self.create_mirror_span_session(session, dst_port, invld_port, "RX")
        self.check_mirror_session_state(session, False)
        
        # Sub Test 4
        # create mirror session with dst_port, src_port, direction
        self.create_mirror_span_session(session, dst_port, src_port, "RX")
        assert self.get_mirror_session_state(session)["status"] == "active"
  
        #verify asicdb
        mirror_entries = tbl.getKeys()
        (status, fvs) = tbl.get(mirror_entries[0])
        assert status == True
        assert len(fvs) == 2
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet16"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TYPE":
                assert fv[1] == "SAI_MIRROR_SESSION_TYPE_LOCAL"

        session_status= "1:"+format(mirror_entries[0])
        # Verify port has mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == session_status
        # Verify port has egress mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == None or status == "0:null"

        # remove mirror session
        self.remove_mirror_session(session)

    def test_PortMirrorMultiSpanAddRemove(self, dvs, testlog):
        """
        This test covers the Multiple SPAN mirror session creation and removal operations
        Operation flow:
        1. Create mirror session with multiple source ports, verify that session is active
        2. Create mirror session with multiple source with valid,invalid ports, session becomes inactive.
        3. Create multiple mirror sessions with multiple source ports.
        4. Create mirror session with invalid source port, the session becomes inactive,
        5. Create mirror session with multiple source ports, the session becomes active
        6. Create mirror session with valid and invalid source ports, the session becomes inactive
        5. Remove mirror session
        """
        self.setup_db(dvs)

        session1 = "TEST_SESSION1"
        session2 = "TEST_SESSION2"
        src_port1 = "Ethernet0"
        src_port2 = "Ethernet4"
        src_port3 = "Ethernet8"
        src_port4 = "Ethernet12"
        dst_port1 = "Ethernet16"
        dst_port2 = "Ethernet20"
        invld_port = "Ethernet"
        src_oid1 = self.getPortOid(dvs, src_port1)
        src_oid2 = self.getPortOid(dvs, src_port2)
        src_oid3 = self.getPortOid(dvs, src_port3)
        src_oid4 = self.getPortOid(dvs, src_port4)
        queue = 1

        # Sub test 1
        # Create mirror session with multiple source ports, verify that session is active
        self.create_mirror_span_session(session1, dst_port1, src_port1+","+src_port2+","+src_port3, "BOTH")
        assert self.get_mirror_session_state(session1)["status"] == "active"

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        mirror_entries = tbl.getKeys()
        assert len(mirror_entries) == 1

        # verify asicdb
        (status, fvs) = tbl.get(mirror_entries[0])
        assert status == True
        assert len(fvs) == 2
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == dst_port1
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TYPE":
                assert fv[1] == "SAI_MIRROR_SESSION_TYPE_LOCAL"

        session_status= "1:"+format(mirror_entries[0])
        # Verify port has mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid1, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == session_status
        # Verify port has egress mirror session enabled with session oid.
        session_status = self.getPortAttr(dvs, src_oid1, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == session_status

        # Verify port has mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid2, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == session_status
        # Verify port has egress mirror session enabled with session oid.
        session_status = self.getPortAttr(dvs, src_oid2, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == session_status
        
        # Verify port has mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid3, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == session_status
        # Verify port has egress mirror session enabled with session oid.
        session_status = self.getPortAttr(dvs, src_oid3, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == session_status

        # remove mirror session
        self.remove_mirror_session(session1)
        
        #Subtest 2
        # create mirror session with valid and invalid ports.
        self.create_mirror_span_session(session1, dst_port1, src_port1+','+invld_port+','+src_port2, "BOTH")
        self.check_mirror_session_state(session1, False)

        # Subtest 3
        # create mirror session with multiple source ports.
        self.create_mirror_span_session(session1, invld_port, src_port1+','+src_port2+','+src_port3, "BOTH")
        self.check_mirror_session_state(session1, False)

        # create mirror session
        self.create_mirror_span_session(session1, dst_port1, src_port1+","+src_port2, "BOTH")
        assert self.get_mirror_session_state(session1)["status"] == "active"
        self.create_mirror_span_session(session2, dst_port2, src_port3+","+src_port4, "BOTH")
        assert self.get_mirror_session_state(session2)["status"] == "active"

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        mirror_entries = tbl.getKeys()
        assert len(mirror_entries) == 2

        # verify asicdb
        (status1, fvs1) = tbl.get(mirror_entries[0])
        (status2, fvs2) = tbl.get(mirror_entries[1])
        assert status1 == True and status2 == True
        assert len(fvs1) == 2 and len(fvs2) == 2
        for fv in fvs1:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                if dvs.asicdb.portoidmap[fv[1]] == dst_port1:
                    session_oid1 = mirror_entries[0]
                if dvs.asicdb.portoidmap[fv[1]] == dst_port2:
                    session_oid2 = mirror_entries[0]

        for fv in fvs2:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                if dvs.asicdb.portoidmap[fv[1]] == dst_port1:
                    session_oid1 = mirror_entries[1]
                if dvs.asicdb.portoidmap[fv[1]] == dst_port2:
                    session_oid2 = mirror_entries[1]
        assert session_oid1 is not None or session_oid2 is not None

        (status1, fvs1) = tbl.get(session_oid1)
        for fv in fvs1:       
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == dst_port1
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TYPE":
                assert fv[1] == "SAI_MIRROR_SESSION_TYPE_LOCAL"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_POLICER":
                assert fv[1] == policer_oid1

        (status1, fvs2) = tbl.get(session_oid2)
        for fv in fvs2:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == dst_port2
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TYPE":
                assert fv[1] == "SAI_MIRROR_SESSION_TYPE_LOCAL"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_POLICER":
                assert fv[1] == policer_oid2

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        mirror_entries = tbl.getKeys()
        assert len(mirror_entries) == 2

        session_status1= "1:"+format(session_oid1)
        session_status2="1:"+format(session_oid2)
        # Verify port has mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid1, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == session_status1
        # Verify port has egress mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid1, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == session_status1

        # Verify port has mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid2, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == session_status1
        # Verify port has egress mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid2, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == session_status1
        
        # Verify port has mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid3, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == session_status2
        # Verify port has egress mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid3, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == session_status2

        # Verify port has mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid4, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == session_status2
        # Verify port has egress mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid4, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == session_status2

        # remove mirror session
        self.remove_mirror_session(session1)
        self.remove_mirror_session(session2)

    def create_policer(self, name):
        tbl = swsscommon.Table(self.cdb, "POLICER")
        fvs = swsscommon.FieldValuePairs([("meter_type", "packets"),
                                          ("mode", "sr_tcm"),
                                          ("cir", "600"),
                                          ("cbs", "600"),
                                          ("red_packet_action", "drop")])
        tbl.set(name, fvs)
        time.sleep(1)

    def remove_policer(self, name):
        tbl = swsscommon.Table(self.cdb, "POLICER")
        tbl._del(name)
        time.sleep(1)

    def test_PortMirrorPolicerAddRemove(self, dvs, testlog):
        """
        This test covers the basic SPAN mirror session creation and removal operations
        Operation flow:
        1. Create mirror session with only dst_port and policer , verify session becomes active
        2. Verify policer config is proper.
        """
        self.setup_db(dvs)

        session = "TEST_SESSION"
        src_port = "Ethernet12"
        dst_port = "Ethernet16"
        invld_port = "Invalid"
        policer= "POLICER"
        src_oid = self.getPortOid(dvs, src_port)

        # create policer
        self.create_policer(policer)

        # Sub Test 1
        # create mirror session
        self.create_mirror_span_session(session, "Ethernet16", None, None, None, policer)
        assert self.get_mirror_session_state(session)["status"] == "active"

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_POLICER")
        policer_entries = tbl.getKeys()
        assert len(policer_entries) == 1
        policer_oid = policer_entries[0]

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        mirror_entries = tbl.getKeys()
        assert len(mirror_entries) == 1

        # verify asicdb
        (status, fvs) = tbl.get(mirror_entries[0])
        assert status == True
        assert len(fvs) == 3
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet16"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TYPE":
                assert fv[1] == "SAI_MIRROR_SESSION_TYPE_LOCAL"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_POLICER":
                assert fv[1] == policer_oid
    
        # remove mirror session
        self.remove_mirror_session(session)
        self.remove_policer(policer)
        
    def test_PortMirrorPolicerMultiAddRemove(self, dvs, testlog):
        """
        This test covers the SPAN mirror session with multiple source ports and multiple policers.
        Operation flow:
        1. Create mirror session with multiple source ports and policers.
        2. Verify mirror and policer config is proper.
        """
        self.setup_db(dvs)

        session1 = "TEST_SESSION1"
        session2 = "TEST_SESSION2"
        src_port1 = "Ethernet0"
        src_port2 = "Ethernet4"
        src_port3 = "Ethernet8"
        src_port4 = "Ethernet12"
        dst_port1 = "Ethernet16"
        dst_port2 = "Ethernet20"
        policer1= "POLICER1"
        policer2= "POLICER2"
        src_oid1 = self.getPortOid(dvs, src_port1)
        src_oid2 = self.getPortOid(dvs, src_port2)
        src_oid3 = self.getPortOid(dvs, src_port3)
        src_oid4 = self.getPortOid(dvs, src_port4)

        # create policer
        self.create_policer(policer1)
        # create policer
        self.create_policer(policer2)
        
        # create mirror session
        source=src_port1+","+src_port2
        self.create_mirror_span_session(session1, dst_port1, source, "BOTH", None, policer1)
        assert self.get_mirror_session_state(session1)["status"] == "active"
        # create mirror session
        source=src_port3+","+src_port4
        self.create_mirror_span_session(session2, dst_port2, source, "BOTH", None, policer2)
        assert self.get_mirror_session_state(session2)["status"] == "active"


        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_POLICER")
        policer_entries = tbl.getKeys()
        assert len(policer_entries) == 2
        policer_oid1 = policer_entries[1]
        policer_oid2 = policer_entries[0]
        
        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        mirror_entries = tbl.getKeys()
        assert len(mirror_entries) == 2

        # verify asicdb
        (status1, fvs1) = tbl.get(mirror_entries[0])
        (status2, fvs2) = tbl.get(mirror_entries[1])
        assert status1 == True and status2 == True
        assert len(fvs1) == 3 and len(fvs2) == 3
        for fv in fvs1:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                if dvs.asicdb.portoidmap[fv[1]] == dst_port1:
                    session_oid1 = mirror_entries[0]
                if dvs.asicdb.portoidmap[fv[1]] == dst_port2:
                    session_oid2 = mirror_entries[0]

        for fv in fvs2:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                if dvs.asicdb.portoidmap[fv[1]] == dst_port1:
                    session_oid1 = mirror_entries[1]
                if dvs.asicdb.portoidmap[fv[1]] == dst_port2:
                    session_oid2 = mirror_entries[1]
        assert session_oid1 is not None or session_oid2 is not None

        (status1, fvs1) = tbl.get(session_oid1)
        for fv in fvs1:       
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == dst_port1
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TYPE":
                assert fv[1] == "SAI_MIRROR_SESSION_TYPE_LOCAL"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_POLICER":
                assert fv[1] == policer_oid1 or fv[1] == policer_oid2

        (status1, fvs2) = tbl.get(session_oid2)
        for fv in fvs2:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == dst_port2
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TYPE":
                assert fv[1] == "SAI_MIRROR_SESSION_TYPE_LOCAL"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_POLICER":
                assert fv[1] == policer_oid2 or fv[1] == policer_oid1 

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        mirror_entries = tbl.getKeys()
        assert len(mirror_entries) == 2

        session_status1= "1:"+format(session_oid1)
        session_status2="1:"+format(session_oid2)
        # Verify port has mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid1, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == session_status1
        # Verify port has egress mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid1, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == session_status1

        # Verify port has mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid2, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == session_status1 
        # Verify port has egress mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid2, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == session_status1
        
        # Verify port has mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid3, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == session_status2
        # Verify port has egress mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid3, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == session_status2

        # Verify port has mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid4, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == session_status2
        # Verify port has egress mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid4, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == session_status2

        # remove mirror session
        self.remove_mirror_session(session1)
        self.remove_mirror_session(session2)

    def create_acl_table(self, table, interfaces):
        tbl = swsscommon.Table(self.cdb, "ACL_TABLE")
        fvs = swsscommon.FieldValuePairs([("policy_desc", "mirror_test"),
                                          ("type", "mirror"),
                                          ("ports", ",".join(interfaces))])
        tbl.set(table, fvs)
        time.sleep(1)

    def remove_acl_table(self, table):
        tbl = swsscommon.Table(self.cdb, "ACL_TABLE")
        tbl._del(table)
        time.sleep(1)

    def create_mirror_acl_dscp_rule(self, table, rule, dscp, session):
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "1000"),
                                          ("mirror_action", session),
                                          ("DSCP", dscp)])
        tbl.set(table + "|" + rule, fvs)
        time.sleep(1)

    def remove_mirror_acl_dscp_rule(self, table, rule):
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        tbl._del(table + "|" + rule)
        time.sleep(1)

    def test_PortMirrorPolicerWithAcl(self, dvs, testlog):
        """
        This test covers the port mirroring with policer and ACL configurations.
        Operation flow:
        1. Create port mirror session with policer.
        2. Create ACL and configure mirror 
        2. Verify mirror and ACL config is proper.
        """
        self.setup_db(dvs)

        session = "MIRROR_SESSION"
        policer= "POLICER"
        acl_table = "MIRROR_TABLE"
        acl_rule = "MIRROR_RULE"
        dst_port = "Ethernet16"

        # create policer
        self.create_policer(policer)

        # create mirror session
        self.create_mirror_span_session(session, dst_port, None, None, None, policer=policer)

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        mirror_entries = tbl.getKeys()
        assert len(mirror_entries) == 1
        mirror_oid = mirror_entries[0]

        # create acl table
        self.create_acl_table(acl_table, ["Ethernet0", "Ethernet4"])

        # create acl rule with dscp value and mask
        self.create_mirror_acl_dscp_rule(acl_table, acl_rule, "8/56", session)

        # assert acl rule is created
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        rule_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_entries]
        assert len(rule_entries) == 1

        (status, fvs) = tbl.get(rule_entries[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_DSCP":
                assert fv[1] == "8&mask:0x38"
            if fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS":
                assert fv[1] == "1:" + mirror_oid

        # remove acl rule
        self.remove_mirror_acl_dscp_rule(acl_table, acl_rule)
        # remove acl table
        self.remove_acl_table(acl_table)
        self.remove_mirror_session(session)

        # remove policer
        self.remove_policer(policer)
        

    def create_port_channel(self, dvs, channel):
        tbl = swsscommon.ProducerStateTable(self.pdb, "LAG_TABLE")
        fvs = swsscommon.FieldValuePairs([("admin", "up"), ("mtu", "9100")])
        tbl.set("PortChannel" + channel, fvs)
        dvs.runcmd("ip link add PortChannel" + channel + " type bond")
        tbl = swsscommon.Table(self.sdb, "LAG_TABLE")
        fvs = swsscommon.FieldValuePairs([("state", "ok")])
        tbl.set("PortChannel" + channel, fvs)
        time.sleep(1)

    def remove_port_channel(self, dvs, channel):
        tbl = swsscommon.ProducerStateTable(self.pdb, "LAG_TABLE")
        tbl._del("PortChannel" + channel)
        dvs.runcmd("ip link del PortChannel" + channel)
        tbl = swsscommon.Table(self.sdb, "LAG_TABLE")
        tbl._del("PortChannel" + channel)
        time.sleep(1)

    def create_port_channel_member(self, channel, interface):
        tbl = swsscommon.ProducerStateTable(self.pdb, "LAG_MEMBER_TABLE")
        fvs = swsscommon.FieldValuePairs([("status", "enabled")])
        tbl.set("PortChannel" + channel + ":" + interface, fvs)
        time.sleep(1)

    def remove_port_channel_member(self, channel, interface):
        tbl = swsscommon.ProducerStateTable(self.pdb, "LAG_MEMBER_TABLE")
        tbl._del("PortChannel" + channel + ":" + interface)
        time.sleep(1)
    
    def test_LAGMirorrSpanAddRemove(self, dvs, testlog):
        """
        This test covers the LAG mirror session creation and removal operations
        Operation flow:
        1. Create port channel with 2 members.
        2. Create mirror session with LAG as source port.
        3. Verify that source ports have proper mirror config.
        4. Remove port from port-channel and verify mirror config is removed from the port.
        5. Remove second port and verify mirror config is removed.
        """
        self.setup_db(dvs)

        session = "TEST_SESSION"
        dst_port = "Ethernet16"
        po_src_port = "PortChannel008"
        src_port1 = "Ethernet0"
        src_port2 = "Ethernet4"
        src_oid1 = self.getPortOid(dvs, src_port1)
        src_oid2 = self.getPortOid(dvs, src_port2)

        # create port channel; create port channel member
        self.create_port_channel(dvs, "008")
        self.create_port_channel_member("008", src_port1)
        self.create_port_channel_member("008", src_port2)

        # bring up port channel and port channel member
        self.set_interface_status(dvs, po_src_port, "up")
        self.set_interface_status(dvs, src_port1, "up")
        self.set_interface_status(dvs, src_port2, "up")

        # Sub Test 1
        self.create_mirror_span_session(session, dst_port, po_src_port, "BOTH")
        assert self.get_mirror_session_state(session)["status"] == "active"

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        mirror_entries = tbl.getKeys()
        assert len(mirror_entries) == 1

        # verify asicdb
        (status, fvs) = tbl.get(mirror_entries[0])
        assert status == True
        assert len(fvs) == 2
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == dst_port
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TYPE":
                assert fv[1] == "SAI_MIRROR_SESSION_TYPE_LOCAL"

        session_status= "1:"+format(mirror_entries[0])
        # Verify port has mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid1, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == session_status
        # Verify port has egress mirror session enabled with session oid.
        session_status = self.getPortAttr(dvs, src_oid1, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == session_status

        # Verify port has mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid2, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == session_status
        # Verify port has egress mirror session enabled with session oid.
        session_status = self.getPortAttr(dvs, src_oid2, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == session_status

        # Sub Test 2
        # remove port channel member; remove port channel
        self.remove_port_channel_member("008", src_port1)

        session_status= "1:"+format(mirror_entries[0])

        # Verify first port doesnt have any mirror config
        status = self.getPortAttr(dvs, src_oid1, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == None or status == "0:null"
        status = self.getPortAttr(dvs, src_oid1, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == None or status == "0:null"

        # Verify second port has mirror config
        status = self.getPortAttr(dvs, src_oid2, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == session_status
        # Verify port has egress mirror session enabled with session oid.
        status = self.getPortAttr(dvs, src_oid2, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == session_status

        self.remove_port_channel_member("008", src_port2)

         # Verify second port doesnt have any mirror config
        status = self.getPortAttr(dvs, src_oid2, 'SAI_PORT_ATTR_INGRESS_MIRROR_SESSION')
        assert status == None or status == "0:null"
        status = self.getPortAttr(dvs, src_oid2, 'SAI_PORT_ATTR_EGRESS_MIRROR_SESSION')
        assert status == None or status == "0:null"

        self.remove_port_channel(dvs, "008")
        self.remove_mirror_session(session)
