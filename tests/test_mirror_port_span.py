# This test suite covers the functionality of mirror feature in SwSS
import pytest

@pytest.mark.usefixtures("testlog")
@pytest.mark.usefixtures('dvs_vlan_manager')
@pytest.mark.usefixtures('dvs_lag_manager')
@pytest.mark.usefixtures('dvs_mirror_manager')
@pytest.mark.usefixtures('dvs_policer_manager')
class TestMirror(object):
    def test_PortMirrorAddRemove(self, dvs, testlog):
        """
        This test covers the basic SPAN mirror session creation and removal operations
        Operation flow:
        1. Create mirror session with only dst_port , verify session becomes active.
        2. Create mirror session with invalid dst_port, verify session doesnt get created.
        3. Create mirror session with invalid source port, verify session doesnt get created.
        4. Create mirror session with source port, verify session becomes active
        5. Create mirror session with Vlan as dst_port, verify session doesnt get created.
        6. Create mirror session with Vlan as source port, verify session doesnt get created.
        """

        pmap = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        pmap = dict(pmap)

        session = "TEST_SESSION"
        dst_port = "Ethernet16"
                
        # Sub Test 1
        self.dvs_mirror.create_span_session(session, dst_port)
        self.dvs_mirror.verify_session_status(session)
        a = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap.get("Ethernet16"),
                "SAI_MIRROR_SESSION_ATTR_TYPE": "SAI_MIRROR_SESSION_TYPE_LOCAL"}
        self.dvs_mirror.verify_session(dvs, session, asic_db=a)
        self.dvs_mirror.remove_mirror_session(session)

        # Sub Test 2
        self.dvs_mirror.create_span_session(session, "Invalid")
        self.dvs_mirror.verify_session_status(session, expected=0)

        # Sub Test 3
        self.dvs_mirror.create_span_session(session, dst_port, "Invalid", "RX")
        self.dvs_mirror.verify_session_status(session, expected=0)
        
        # Sub Test 4
        # create mirror session with dst_port, src_port, direction
        src_ports = "Ethernet12"
        src_asic_ports = ["Ethernet12"]
        self.dvs_mirror.create_span_session(session, dst_port, src_ports, "RX")
        self.dvs_mirror.verify_session_status(session)
        self.dvs_mirror.verify_session(dvs, session, asic_db=a, src_ports=src_asic_ports, direction = "RX")
        self.dvs_mirror.remove_mirror_session(session)
        self.dvs_mirror.verify_no_mirror()
        
        ## Sub Test 5
        self.dvs_vlan.create_vlan("10")
        self.dvs_mirror.create_span_session(session, dst_port="Vlan10")
        self.dvs_mirror.verify_session_status(session, expected=0)
        
        ## Sub Test 6
        self.dvs_mirror.create_span_session(session, dst_port, src_ports="Vlan10")
        self.dvs_mirror.verify_session_status(session, expected=0)
        self.dvs_mirror.remove_mirror_session(session)
        self.dvs_mirror.verify_no_mirror()
        self.dvs_vlan.remove_vlan("10")
        self.dvs_vlan.get_and_verify_vlan_ids(0)


    def test_PortMirrorMultiSpanAddRemove(self, dvs, testlog):
        """
        This test covers the Multiple SPAN mirror session creation and removal operations
        Operation flow:
        1. Create mirror session with multiple source ports, verify that session is active
        2. Create mirror session with multiple source with valid,invalid ports, session doesnt get created.
        3. Create mirror session with multiple source with invalid destination, session doesnt get created.
        4. Create two mirror sessions with multiple source ports.
        5. Verify session config in both sessions.
        """

        pmap = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        pmap = dict(pmap)

        session1 = "TEST_SESSION1"
        session2 = "TEST_SESSION2"
        dst_port1 = "Ethernet16"
        dst_oid1 = pmap.get(dst_port1)
        dst_port2 = "Ethernet20"
        dst_oid2 = pmap.get(dst_port2)

        # Sub test 1
        src_ports = "Ethernet0,Ethernet4,Ethernet8"
        src_asic_ports = ["Ethernet0","Ethernet4","Ethernet8"]
        self.dvs_mirror.create_span_session(session1, dst_port1, src_ports)
        a = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": dst_oid1,
                "SAI_MIRROR_SESSION_ATTR_TYPE": "SAI_MIRROR_SESSION_TYPE_LOCAL"}
        self.dvs_mirror.verify_session_status(session1)
        self.dvs_mirror.verify_session(dvs, session1, asic_db=a, src_ports=src_asic_ports)
        self.dvs_mirror.remove_mirror_session(session1)
        self.dvs_mirror.verify_no_mirror()

        #Subtest 2

        # create mirror session with valid and invalid ports.
        src_ports = "Ethernet0,Invalid,Ethernet8"
        self.dvs_mirror.create_span_session(session1, dst_port1, src_ports)
        self.dvs_mirror.verify_session_status(session1, expected=0)

        # Subtest 3
        src_ports = "Ethernet0,Ethernet4,Ethernet8"
        self.dvs_mirror.create_span_session(session1, "Invalid", src_ports)
        self.dvs_mirror.verify_session_status(session1, expected=0)

        # create mirror session
        src_ports1 = "Ethernet0,Ethernet4"
        src_asic_ports1 = ["Ethernet0","Ethernet4"]
        self.dvs_mirror.create_span_session(session1, dst_port1, src_ports1)
        src_ports2 = "Ethernet8,Ethernet12"
        src_asic_ports2 = ["Ethernet8","Ethernet12"]
        self.dvs_mirror.create_span_session(session2, dst_port2, src_ports2)

        a1 = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": dst_oid1,
                "SAI_MIRROR_SESSION_ATTR_TYPE": "SAI_MIRROR_SESSION_TYPE_LOCAL"}
        a2 = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": dst_oid2,
                "SAI_MIRROR_SESSION_ATTR_TYPE": "SAI_MIRROR_SESSION_TYPE_LOCAL"}
        self.dvs_mirror.verify_session_status(session1, expected = 2)
        self.dvs_mirror.verify_session_status(session2, expected = 2)
        self.dvs_mirror.verify_session(dvs, session1, dst_oid=dst_oid1, asic_db=a1, src_ports=src_asic_ports1, expected = 2)
        self.dvs_mirror.verify_session(dvs, session2, dst_oid=dst_oid2, asic_db=a2, src_ports=src_asic_ports2, expected = 2)
        self.dvs_mirror.remove_mirror_session(session1)
        self.dvs_mirror.remove_mirror_session(session2)
        self.dvs_mirror.verify_no_mirror()

    def test_PortMirrorPolicerAddRemove(self, dvs, testlog):
        """
        This test covers the basic SPAN mirror session creation and removal operations
        Operation flow:
        1. Create mirror session with only dst_port and policer , verify session becomes active
        2. Create session with invalid policer, verify session doesnt get created.
        2. Create mirror with policer and multiple source ports, verify session config on all ports.
        """
        pmap = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        pmap = dict(pmap)
        session = "TEST_SESSION"
        dst_port = "Ethernet16"
        policer="POLICER"

        #Sub Test 1
        self.dvs_policer.create_policer(policer)
        self.dvs_policer.verify_policer(policer)
        self.dvs_mirror.create_span_session(session, dst_port, policer="POLICER")
        self.dvs_mirror.verify_session_status(session)
        a = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap.get("Ethernet16"),
                "SAI_MIRROR_SESSION_ATTR_TYPE": "SAI_MIRROR_SESSION_TYPE_LOCAL"}
        self.dvs_mirror.verify_session(dvs, session, asic_db=a, policer="POLICER")
        self.dvs_mirror.remove_mirror_session(session)
        self.dvs_policer.remove_policer("POLICER")
        self.dvs_policer.verify_no_policer()
        self.dvs_mirror.verify_no_mirror()

        # Sub test 2
        src_ports = "Ethernet0,Ethernet4,Ethernet8"
        src_asic_ports = ["Ethernet0","Ethernet4","Ethernet8"]
        self.dvs_mirror.create_span_session(session, dst_port, src_ports, policer="POLICER")
        self.dvs_mirror.verify_session_status(session, expected=0)

        # Sub test 3
        self.dvs_policer.create_policer(policer)
        self.dvs_policer.verify_policer(policer)
        self.dvs_mirror.create_span_session(session, dst_port, src_ports, policer="POLICER")
        self.dvs_mirror.verify_session_status(session)
        self.dvs_mirror.verify_session(dvs, session, asic_db=a, src_ports=src_asic_ports, policer="POLICER")
        self.dvs_mirror.remove_mirror_session(session)
        self.dvs_policer.remove_policer("POLICER")
        self.dvs_policer.verify_no_policer()
        self.dvs_mirror.verify_no_mirror()


    def test_PortMultiMirrorPolicerAddRemove(self, dvs, testlog):
        """
        This test covers the basic SPAN mirror session creation and removal operations
        Operation flow:
        1. Create mirror session with multiple source with multiple policer.
        2. Verify port/policer/session config on all.
        """
        pmap = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        pmap = dict(pmap)

        session1 = "TEST_SESSION1"
        session2 = "TEST_SESSION2"
        dst_port1 = "Ethernet16"
        dst_oid1 = pmap.get(dst_port1)
        dst_port2 = "Ethernet20"
        dst_oid2 = pmap.get(dst_port2)
        policer1 = "POLICER1"
        policer2 = "POLICER2"

        #Sub Test 1
        self.dvs_policer.create_policer(policer1, cir="600")
        self.dvs_policer.verify_policer(policer1)
        self.dvs_policer.create_policer(policer2, cir="800")
        self.dvs_policer.verify_policer(policer2, expected = 2)

        src_ports1 = "Ethernet0,Ethernet4"
        src_asic_ports1 = ["Ethernet0","Ethernet4"]
        self.dvs_mirror.create_span_session(session1, dst_port1, src_ports1, policer=policer1)
        src_ports2 = "Ethernet8,Ethernet12"
        src_asic_ports2 = ["Ethernet8","Ethernet12"]
        self.dvs_mirror.create_span_session(session2, dst_port2, src_ports2, policer=policer2)

        a1 = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": dst_oid1,
                "SAI_MIRROR_SESSION_ATTR_TYPE": "SAI_MIRROR_SESSION_TYPE_LOCAL"}
        a2 = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": dst_oid2,
                "SAI_MIRROR_SESSION_ATTR_TYPE": "SAI_MIRROR_SESSION_TYPE_LOCAL"}
        self.dvs_mirror.verify_session_status(session1, expected = 2)
        self.dvs_mirror.verify_session_status(session2, expected = 2)
        self.dvs_mirror.verify_session(dvs, session1, dst_oid=dst_oid1, asic_db=a1, src_ports=src_asic_ports1, expected = 2, policer=policer1)
        self.dvs_mirror.verify_session(dvs, session2, dst_oid=dst_oid2, asic_db=a2, src_ports=src_asic_ports2, expected = 2, policer=policer2)
        self.dvs_mirror.remove_mirror_session(session1)
        self.dvs_mirror.remove_mirror_session(session2)
        self.dvs_policer.remove_policer(policer1)
        self.dvs_policer.remove_policer(policer2)
        self.dvs_policer.verify_no_policer()
        self.dvs_mirror.verify_no_mirror()

    def test_LAGMirrorSpanAddRemove(self, dvs, testlog):
        """
        This test covers the LAG mirror session creation and removal operations
        Operation flow:
        1. Create port channel with 2 members.
        2. Create mirror session with LAG as source port.
        3. Verify that source ports have proper mirror config.
        4. Remove port from port-channel and verify mirror config is removed from the port.
        5. Remove second port and verify mirror config is removed.
        """
        dvs.setup_db()
        pmap = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        pmap = dict(pmap)

        session = "TEST_SESSION"
        dst_port = "Ethernet16"
        src_port1="Ethernet8"
        src_port2="Ethernet4"
        po_src_port="PortChannel008"
        src_ports = "PortChannel008,Ethernet12"
        src_asic_ports = ["Ethernet8", "Ethernet4", "Ethernet12"]

        # create port channel; create port channel member
        self.dvs_lag.create_port_channel("008")
        self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 1)
        self.dvs_lag.create_port_channel_member("008", src_port1)
        self.dvs_lag.create_port_channel_member("008", src_port2)

        # bring up port channel and port channel member
        dvs.set_interface_status(po_src_port, "up")
        dvs.set_interface_status(src_port1, "up")
        dvs.set_interface_status(src_port2, "up")

        # Sub Test 1
        self.dvs_mirror.create_span_session(session, dst_port, src_ports)
        self.dvs_mirror.verify_session_status(session)

        # verify asicdb
        # Check src_port state.
        expected_asic_db = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap.get(dst_port),
                            "SAI_MIRROR_SESSION_ATTR_TYPE": "SAI_MIRROR_SESSION_TYPE_LOCAL"}
        self.dvs_mirror.verify_session(dvs, session, asic_db=expected_asic_db, src_ports=src_asic_ports, asic_size=2)

        # Sub Test 2
        # remove port channel member; remove port channel
        self.dvs_lag.remove_port_channel_member("008", src_port1)
        src_asic_ports = ["Ethernet4", "Ethernet12"]
        self.dvs_mirror.verify_session(dvs, session, asic_db=expected_asic_db, src_ports=src_asic_ports, asic_size=2)
        self.dvs_lag.remove_port_channel_member("008", src_port2)

        self.dvs_lag.remove_port_channel("008")
        self.dvs_mirror.remove_mirror_session(session)
        self.dvs_mirror.verify_no_mirror()

    def test_PortMirrorPolicerWithAcl(self, dvs, dvs_acl, testlog):
        """
        This test covers the port mirroring with policer and ACL configurations.
        Operation flow:
        1. Create port mirror session with policer.
        2. Create ACL and configure mirror 
        2. Verify mirror and ACL config is proper.
        """
        dvs.setup_db()
        session = "MIRROR_SESSION"
        policer= "POLICER"
        dst_port = "Ethernet16"

        # create policer
        self.dvs_policer.create_policer(policer)
        self.dvs_policer.verify_policer(policer)

        # create mirror session
        self.dvs_mirror.create_span_session(session, dst_port, policer=policer)
        self.dvs_mirror.verify_session_status(session)

        member_ids = dvs.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION", 1)

        # create acl table
        bind_ports = ["Ethernet0", "Ethernet4"]
        dvs_acl.create_acl_table("test", "mirror", bind_ports)
        dvs_acl.verify_acl_table_count(1)
        dvs_acl.verify_acl_table_groups(len(bind_ports))

        config_qualifiers = {"DSCP": "8/56"}

        mirror_oid = "1:" + member_ids[0]
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_FIELD_DSCP": dvs_acl.get_simple_qualifier_comparator("8&mask:0x38")
        }

        dvs_acl.create_mirror_acl_rule("test", "mirror_rule", config_qualifiers, session)
        dvs_acl.verify_mirror_acl_rule(expected_sai_qualifiers, mirror_oid)

        dvs_acl.remove_acl_rule("test", "mirror_rule")
        dvs_acl.verify_no_acl_rules()

        dvs_acl.remove_acl_table("test")
        dvs_acl.verify_acl_table_count(0)

        self.dvs_mirror.remove_mirror_session(session)

        self.dvs_policer.remove_policer(policer)
        self.dvs_policer.verify_no_policer()
        self.dvs_mirror.verify_no_mirror()

    def test_PortMirrorLAGPortSpanAddRemove(self, dvs, testlog):
        """
        This test covers the LAG mirror session creation and removal operations
        Operation flow:
        1. Create port channel with 2 members.
        2. Create mirror session with LAG and LAG port. session creation has to fail.
        3. Create mirror session with LAG and other LAG port. session creation has to fail
        4. Create mirror session with LAG and new port, session will become active.
        5. Verify all LAG and new port has session config.
        6. Remove one LAG member and verify that failing session works fine.
        """
        dvs.setup_db()
        pmap = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        pmap = dict(pmap)

        session = "TEST_SESSION"
        dst_port = "Ethernet16"
        src_port1="Ethernet8"
        src_port2="Ethernet4"
        po_src_port="PortChannel008"

        # create port channel; create port channel member
        self.dvs_lag.create_port_channel("008")
        self.dvs_lag.get_and_verify_port_channel(1)
        self.dvs_lag.create_port_channel_member("008", src_port1)
        self.dvs_lag.create_port_channel_member("008", src_port2)
        self.dvs_lag.get_and_verify_port_channel_members(2)

        # bring up port channel and port channel member
        dvs.set_interface_status(po_src_port, "up")
        dvs.set_interface_status(src_port1, "up")
        dvs.set_interface_status(src_port2, "up")

        # Sub Test 1
        src_ports = "PortChannel008,Ethernet8"
        self.dvs_mirror.create_span_session(session, dst_port, src_ports)
        self.dvs_mirror.verify_session_status(session, expected=0)
        self.dvs_mirror.verify_session_status(session, status="inactive", expected = 0)

        # Sub Test 2
        src_ports = "PortChannel008,Ethernet4"
        self.dvs_mirror.create_span_session(session, dst_port, src_ports)
        self.dvs_mirror.verify_session_status(session, expected=0)
        self.dvs_mirror.verify_session_status(session, status="inactive", expected=0)

        # Sub Test 3
        src_ports = "PortChannel008,Ethernet40"
        src_asic_ports = ["Ethernet8", "Ethernet40", "Ethernet4"]
        self.dvs_mirror.create_span_session(session, dst_port, src_ports)
        self.dvs_mirror.verify_session_status(session, status="active")

        expected_asic_db = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap.get(dst_port),
                            "SAI_MIRROR_SESSION_ATTR_TYPE": "SAI_MIRROR_SESSION_TYPE_LOCAL"}
        self.dvs_mirror.verify_session(dvs, session, asic_db=expected_asic_db, src_ports=src_asic_ports, asic_size=2)
        self.dvs_mirror.remove_mirror_session(session)
        self.dvs_mirror.verify_no_mirror()

        # Sub Test 4
        self.dvs_lag.remove_port_channel_member("008", "Ethernet4")
        self.dvs_lag.get_and_verify_port_channel_members(1)
        src_ports = "PortChannel008,Ethernet40"
        src_asic_ports = ["Ethernet8", "Ethernet40"]
        self.dvs_mirror.create_span_session(session, dst_port, src_ports)
        self.dvs_mirror.verify_session_status(session)
        self.dvs_mirror.verify_session(dvs, session, src_ports=src_asic_ports)

        self.dvs_mirror.remove_mirror_session(session)
        self.dvs_mirror.verify_no_mirror()
        self.dvs_lag.remove_port_channel_member("008", "Ethernet8")
        self.dvs_lag.get_and_verify_port_channel_members(0)
        self.dvs_lag.remove_port_channel("008")
        self.dvs_lag.get_and_verify_port_channel(0)
        self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 0)


    def test_PortLAGMirrorUpdateLAG(self, dvs, testlog):
        """
        This test covers the LAG mirror session creation and removal operations
        Operation flow:
        1. Create port channel with 2 members.
        2. Create mirror session with LAG and other port P1
        3. Verify mirror session is active and ports have proper config
        4. Add port P1 to LAG and verify mirror config on all ports.
        5. Remove port P1 from LAG and verify mirror config on P1 is intact.
        6. Remove port from LAG and verify mirror config on other ports in intact.
        """
        dvs.setup_db()
        pmap = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        pmap = dict(pmap)

        session = "TEST_SESSION"
        dst_port = "Ethernet16"
        src_port1="Ethernet8"
        src_port2="Ethernet4"
        po_src_port="PortChannel008"

        # create port channel; create port channel member
        self.dvs_lag.create_port_channel("008")
        self.dvs_lag.get_and_verify_port_channel(1)
        self.dvs_lag.create_port_channel_member("008", src_port1)
        self.dvs_lag.create_port_channel_member("008", src_port2)
        self.dvs_lag.get_and_verify_port_channel_members(2)

        # bring up port channel and port channel member
        dvs.set_interface_status(po_src_port, "up")
        dvs.set_interface_status(src_port1, "up")
        dvs.set_interface_status(src_port2, "up")

        # Sub Test 1
        src_ports = "PortChannel008,Ethernet40"
        src_asic_ports = ["Ethernet8", "Ethernet40", "Ethernet4"]
        self.dvs_mirror.create_span_session(session, dst_port, src_ports)
        self.dvs_mirror.verify_session_status(session, status="active")

        expected_asic_db = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap.get(dst_port),
                            "SAI_MIRROR_SESSION_ATTR_TYPE": "SAI_MIRROR_SESSION_TYPE_LOCAL"}
        self.dvs_mirror.verify_session(dvs, session, asic_db=expected_asic_db, src_ports=src_asic_ports, asic_size=2)


        # Add source port Ethernet40 to LAG
        self.dvs_lag.create_port_channel_member("008", "Ethernet40")
        self.dvs_lag.get_and_verify_port_channel_members(3)
        self.dvs_mirror.verify_session(dvs, session, src_ports=src_asic_ports)

        # Remove source port Ethernet40 from LAG
        self.dvs_lag.remove_port_channel_member("008", "Ethernet40")
        self.dvs_lag.get_and_verify_port_channel_members(2)
        self.dvs_mirror.verify_session(dvs, session, src_ports=src_asic_ports)
        
        # Remove one port from LAG
        self.dvs_lag.remove_port_channel_member("008", "Ethernet4")
        self.dvs_lag.get_and_verify_port_channel_members(1)
        src_asic_ports = ["Ethernet8"]
        self.dvs_mirror.verify_session(dvs, session, src_ports=src_asic_ports)
        
        #cleanup
        self.dvs_mirror.remove_mirror_session(session)
        self.dvs_mirror.verify_no_mirror()
        self.dvs_lag.remove_port_channel_member("008", "Ethernet8")
        self.dvs_lag.get_and_verify_port_channel_members(0)
        self.dvs_lag.remove_port_channel("008")
        self.dvs_lag.get_and_verify_port_channel(0)
        self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 0)

