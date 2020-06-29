# This test suite covers the functionality of mirror feature in SwSS
import platform
import pytest
import time

from swsscommon import swsscommon
from distutils.version import StrictVersion

@pytest.mark.usefixtures("testlog")
@pytest.mark.usefixtures('dvs_vlan_manager')
@pytest.mark.usefixtures('dvs_lag_manager')
@pytest.mark.usefixtures('dvs_mirror_manager')

class TestMirror(object):
    def test_PortMirrorERSpanAddRemove(self, dvs, testlog):
        """
        This test covers the basic ERSPANmirror session creation and removal operations
        Operation flow:
        1. Create mirror session with source ports.
           The session remains inactive because no nexthop/neighbor exists
        2. Bring up port; assign IP; create neighbor; create route
           The session remains inactive until the route is created
        3. Verify that port mirror config is proper.
        4. Remove route; remove neighbor; remove IP; bring down port
           The session becomes inactive again till the end
        """
        dvs.setup_db()
        pmap = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        pmap = dict(pmap)

        session = "TEST_SESSION"
        src_ports = "Ethernet12"
        src_asic_ports = ["Ethernet12"]

        # create mirror session
        self.dvs_mirror.create_erspan_session(session, "1.1.1.1", "2.2.2.2", "0x6558", "8", "100", "0", None, src_ports)
        self.dvs_mirror.verify_session_status(session, status="inactive") 

        # bring up Ethernet16
        dvs.set_interface_status("Ethernet16", "up")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # add IP address to Ethernet16
        dvs.add_ip_address("Ethernet16", "10.0.0.0/31")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # add neighbor to Ethernet16
        dvs.add_neighbor("Ethernet16", "10.0.0.1", "02:04:06:08:10:12")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # add route to mirror destination via 10.0.0.1
        dvs.add_route("2.2.2.2", "10.0.0.1")
        src_mac = dvs.runcmd("bash -c \"ip link show eth0 | grep ether | awk '{print $2}'\"")[1].strip().upper()
        expected_asic_db = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap.get("Ethernet16"),
                            "SAI_MIRROR_SESSION_ATTR_TYPE": "SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE",
                            "SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE": "SAI_ERSPAN_ENCAPSULATION_TYPE_MIRROR_L3_GRE_TUNNEL",
                            "SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION": "4",
                            "SAI_MIRROR_SESSION_ATTR_TOS": "32",
                            "SAI_MIRROR_SESSION_ATTR_TTL": "100",
                            "SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS": "1.1.1.1",
                            "SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS": "2.2.2.2",
                            "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS": "02:04:06:08:10:12",
                            "SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS": src_mac,
                            "SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE": "25944"}
        expected_state_db = {"status": "active",
                                "monitor_port": "Ethernet16",
                                "dst_mac": "02:04:06:08:10:12",
                                "route_prefix": "2.2.2.2/32"}
        self.dvs_mirror.verify_session_status(session)
        self.dvs_mirror.verify_session(dvs, session, asic_db=expected_asic_db, state_db=expected_state_db, src_ports=src_asic_ports, asic_size=11)

        # remove route
        dvs.remove_route("2.2.2.2")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # remove neighbor
        dvs.remove_neighbor("Ethernet16", "10.0.0.1")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # remove IP address
        dvs.remove_ip_address("Ethernet16", "10.0.0.0/31")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # bring down Ethernet16
        dvs.set_interface_status("Ethernet16", "down")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # remove mirror session
        self.dvs_mirror.remove_mirror_session(session)
        self.dvs_mirror.verify_no_mirror()

    def test_PortMirrorToVlanAddRemove(self, dvs, testlog):
        """
        This test covers basic mirror session creation and removal operation
        with destination port sits in a VLAN
        Opeartion flow:
        1. Create mirror session with source ports.
        2. Create VLAN; assign IP; create neighbor; create FDB
           The session should be up only at this time.
           verify source port mirror config.
        3. Remove FDB; remove neighbor; remove IP; remove VLAN
        4. Remove mirror session
        """
        dvs.setup_db()
        pmap = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        pmap = dict(pmap)

        session = "TEST_SESSION"
        src_ports = "Ethernet12,Ethernet16"
        src_asic_ports = ["Ethernet12", "Ethernet16"]
        vlan_id = "10"
        vlan = "Vlan10"

        # create mirror session
        self.dvs_mirror.create_erspan_session(session, "5.5.5.5", "6.6.6.6", "0x6558", "8", "100", "0", None, src_ports, direction="TX")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # create vlan; create vlan member
        self.dvs_vlan.create_vlan(vlan_id)
        self.dvs_vlan.create_vlan_member(vlan_id, "Ethernet4")

        # bring up vlan and member
        dvs.set_interface_status(vlan, "up")
        dvs.set_interface_status("Ethernet4", "up")

        # add ip address to vlan 6
        dvs.add_ip_address(vlan, "6.6.6.0/24")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # create neighbor to vlan 6
        dvs.add_neighbor(vlan, "6.6.6.6", "66:66:66:66:66:66")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # create fdb entry to ethernet4
        dvs.create_fdb(vlan_id, "66-66-66-66-66-66", "Ethernet4")
        self.dvs_mirror.verify_session_status(session)

        src_mac = dvs.runcmd("bash -c \"ip link show eth0 | grep ether | awk '{print $2}'\"")[1].strip().upper()
        expected_asic_db = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap.get("Ethernet4"),
                            "SAI_MIRROR_SESSION_ATTR_TYPE": "SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE",
                            "SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE": "SAI_ERSPAN_ENCAPSULATION_TYPE_MIRROR_L3_GRE_TUNNEL",
                            "SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION": "4",
                            "SAI_MIRROR_SESSION_ATTR_TOS": "32",
                            "SAI_MIRROR_SESSION_ATTR_TTL": "100",
                            "SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS": "5.5.5.5",
                            "SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS": "6.6.6.6",
                            "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS": "66:66:66:66:66:66",
                            "SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS": src_mac,
                            "SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE": "25944",
                            "SAI_MIRROR_SESSION_ATTR_VLAN_HEADER_VALID": "true",
                            "SAI_MIRROR_SESSION_ATTR_VLAN_TPID": "33024",
                            "SAI_MIRROR_SESSION_ATTR_VLAN_ID": vlan_id,
                            "SAI_MIRROR_SESSION_ATTR_VLAN_PRI": "0",
                            "SAI_MIRROR_SESSION_ATTR_VLAN_CFI": "0"}
        self.dvs_mirror.verify_session(dvs, session, asic_db=expected_asic_db, src_ports=src_asic_ports, asic_size=16, direction="TX")

        dvs.set_interface_status("Ethernet4", "down")

        # remove fdb entry
        dvs.remove_fdb(vlan_id, "66-66-66-66-66-66")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # remove neighbor
        dvs.remove_neighbor(vlan, "6.6.6.6")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # remove ip address
        dvs.remove_ip_address(vlan, "6.6.6.0/24")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # bring down vlan and member
        dvs.set_interface_status("Ethernet4", "down")
        dvs.set_interface_status(vlan, "down")

        # remove mirror session
        self.dvs_mirror.remove_mirror_session(session)
        self.dvs_mirror.verify_no_mirror()

        # remove vlan member; remove vlan
        self.dvs_vlan.remove_vlan_member(vlan_id, "Ethernet4")
        self.dvs_vlan.remove_vlan(vlan_id)


    def test_PortMirrorToLagAddRemove(self, dvs, testlog):
        """
        This test covers basic mirror session creation and removal operations
        with destination port sits in a LAG
        Operation flow:
        1. Create mirror sesion with source ports, direction
        2. Create LAG; assign IP; create directly connected neighbor
           The session shoudl be up only at this time.
        3. Remove neighbor; remove IP; remove LAG
        4. Remove mirror session

        """
        dvs.setup_db()
        pmap = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        pmap = dict(pmap)

        session = "TEST_SESSION"
        src_ports = "Ethernet0,Ethernet4"
        src_asic_ports = ["Ethernet0", "Ethernet4"]

        # create mirror session
        self.dvs_mirror.create_erspan_session(session, "10.10.10.10", "11.11.11.11", "0x6558",
                                              "8", "100", "0", None, src_ports, "TX")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # create port channel; create port channel member
        self.dvs_lag.create_port_channel("008")
        lag_entries = self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 1)
        self.dvs_lag.create_port_channel_member("008", "Ethernet88")
                                                
        # Verify the LAG has been initialized properly
        lag_member_entries = self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER", 1)
        fvs = self.dvs_vlan.asic_db.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER", lag_member_entries[0])
        assert len(fvs) == 4
        assert fvs.get("SAI_LAG_MEMBER_ATTR_LAG_ID") == lag_entries[0]
        assert self.dvs_vlan.asic_db.port_to_id_map[fvs.get("SAI_LAG_MEMBER_ATTR_PORT_ID")] == "Ethernet88"

        # bring up port channel and port channel member
        dvs.set_interface_status("PortChannel008", "up")
        dvs.set_interface_status("Ethernet88", "up")

        # add ip address to port channel 008
        dvs.add_ip_address("PortChannel008", "11.11.11.0/24")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # create neighbor to port channel 008
        dvs.add_neighbor("PortChannel008", "11.11.11.11", "88:88:88:88:88:88")
        self.dvs_mirror.verify_session_status(session)

        # Check src_port state.
        expected_asic_db = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap.get("Ethernet88"),
                            "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS": "88:88:88:88:88:88"}

        self.dvs_mirror.verify_session(dvs, session, asic_db=expected_asic_db, src_ports=src_asic_ports, direction="TX")

        # remove neighbor
        dvs.remove_neighbor("PortChannel008", "11.11.11.11")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # remove ip address
        dvs.remove_ip_address("PortChannel008", "11.11.11.0/24")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # bring down port channel and port channel member
        dvs.set_interface_status("PortChannel008", "down")
        dvs.set_interface_status("Ethernet88", "down")

        # remove port channel member; remove port channel

        self.dvs_lag.remove_port_channel_member("008", "Ethernet88")
        self.dvs_lag.remove_port_channel("008")

        # remove mirror session
        self.dvs_mirror.remove_mirror_session(session)
        self.dvs_mirror.verify_no_mirror()

    def test_PortMirrorDestMoveVlan(self, dvs, testlog):
        """
        This test tests mirror session destination move from non-VLAN to VLAN
        and back to non-VLAN port
        1. Create mirror session
        2. Enable non-VLAN monitor port
        3. Create VLAN; move to VLAN without FDB entry
        4. Create FDB entry
        5. Remove FDB entry
        6. Remove VLAN; move to non-VLAN
        7. Disable non-VLAN monitor port
        8. Remove mirror session
        """
        dvs.setup_db()
        pmap = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        pmap = dict(pmap)

        session = "TEST_SESSION"
        src_ports = "Ethernet0"
        src_asic_ports = ["Ethernet0"]

        # create mirror session
        self.dvs_mirror.create_erspan_session(session, "7.7.7.7", "8.8.8.8", "0x6558", "8", "100", "0", None, src_ports)
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # bring up port; add ip; add neighbor; add route
        dvs.set_interface_status("Ethernet32", "up")
        dvs.add_ip_address("Ethernet32", "80.0.0.0/31")
        dvs.add_neighbor("Ethernet32", "80.0.0.1", "02:04:06:08:10:12")
        dvs.add_route("8.8.0.0/16", "80.0.0.1")

        self.dvs_mirror.verify_session_status(session)

        # check monitor port
        expected_asic_db = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap.get("Ethernet32")}
        self.dvs_mirror.verify_session(dvs, session, asic_db=expected_asic_db)

        # mirror session move round 1
        # create vlan; create vlan member; bring up vlan and member
        self.dvs_vlan.create_vlan("9")
        self.dvs_vlan.create_vlan_member("9", "Ethernet48")
        dvs.set_interface_status("Vlan9", "up")
        dvs.set_interface_status("Ethernet48", "up")

        self.dvs_mirror.verify_session_status(session)

        # add ip address to vlan 9
        dvs.add_ip_address("Vlan9", "8.8.8.0/24")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # create neighbor to vlan 9
        dvs.add_neighbor("Vlan9", "8.8.8.8", "88:88:88:88:88:88")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # create fdb entry to ethernet48
        dvs.create_fdb("9", "88-88-88-88-88-88", "Ethernet48")
        self.dvs_mirror.verify_session_status(session)

        # check monitor port
        expected_asic_db = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap.get("Ethernet48"),
                            "SAI_MIRROR_SESSION_ATTR_VLAN_HEADER_VALID": "true",
                            "SAI_MIRROR_SESSION_ATTR_VLAN_TPID": "33024",
                            "SAI_MIRROR_SESSION_ATTR_VLAN_ID": "9",
                            "SAI_MIRROR_SESSION_ATTR_VLAN_PRI": "0",
                            "SAI_MIRROR_SESSION_ATTR_VLAN_CFI": "0"}

        self.dvs_mirror.verify_session(dvs, session, asic_db=expected_asic_db, src_ports=src_asic_ports)

        # mirror session move round 2
        # remove fdb entry
        dvs.remove_fdb("9", "88-88-88-88-88-88")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # remove neighbor
        dvs.remove_neighbor("Vlan9", "8.8.8.8")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # remove ip address
        dvs.remove_ip_address("Vlan9", "8.8.8.0/24")
        self.dvs_mirror.verify_session_status(session)

        expected_asic_db = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap.get("Ethernet32")}
        self.dvs_mirror.verify_session(dvs, session, asic_db=expected_asic_db)

        # bring down vlan and member; remove vlan member; remove vlan
        dvs.set_interface_status("Ethernet48", "down")
        dvs.set_interface_status("Vlan9", "down")
        dvs.remove_vlan_member("9", "Ethernet48")
        dvs.remove_vlan("9")

        # remove route; remove neighbor; remove ip; bring down port
        dvs.remove_route("8.8.8.0/24")
        dvs.remove_neighbor("Ethernet32", "80.0.0.1")
        dvs.remove_ip_address("Ethernet32", "80.0.0.0/31")
        dvs.set_interface_status("Ethernet32", "down")

        # remove mirror session
        self.dvs_mirror.remove_mirror_session(session)
        self.dvs_mirror.verify_no_mirror()


    def test_PortMirrorDestMoveLag(self, dvs, testlog):
        """
        This test tests mirror session destination move from non-LAG to LAG
        and back to non-LAG port
        1. Create mirror session with source ports.
        2. Enable non-LAG monitor port
        3. Create LAG; move to LAG with one member
        4. Remove LAG member
        5. Create LAG member
        6. Remove LAG; move to non-LAG
        7. Disable non-LAG monitor port
        8. Remove mirror session
        """
        dvs.setup_db()
        pmap = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        pmap = dict(pmap)

        session = "TEST_SESSION"
        src_ports = "Ethernet0,Ethernet4"
        src_asic_ports = ["Ethernet0", "Ethernet4"]
        # create mirror session
        self.dvs_mirror.create_erspan_session(session, "12.12.12.12", "13.13.13.13", "0x6558", "8", "100", "0", None, src_ports, direction="RX")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # bring up port; add ip; add neighbor; add route
        dvs.set_interface_status("Ethernet64", "up")
        dvs.add_ip_address("Ethernet64", "100.0.0.0/31")
        dvs.add_neighbor("Ethernet64", "100.0.0.1", "02:04:06:08:10:12")
        dvs.add_route("13.13.0.0/16", "100.0.0.1")
        self.dvs_mirror.verify_session_status(session)

        # check monitor port
        expected_asic_db = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap.get("Ethernet64"),
                            "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS": "02:04:06:08:10:12"}
        self.dvs_mirror.verify_session(dvs, session, asic_db=expected_asic_db, src_ports=src_asic_ports, direction="RX")

        # mirror session move round 1
        # create port channel; create port channel member; bring up
        self.dvs_lag.create_port_channel("080")
        lag_entries = self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 1)
        self.dvs_lag.create_port_channel_member("080", "Ethernet32")
        dvs.set_interface_status("PortChannel080", "up")
        dvs.set_interface_status("Ethernet32", "up")

        # add ip address to port channel 080; create neighbor to port channel 080
        dvs.add_ip_address("PortChannel080", "200.0.0.0/31")
        dvs.add_neighbor("PortChannel080", "200.0.0.1", "12:10:08:06:04:02")
        self.dvs_mirror.verify_session_status(session)

        # add route
        dvs.add_route("13.13.13.0/24", "200.0.0.1")
        self.dvs_mirror.verify_session_status(session)

        expected_asic_db = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap.get("Ethernet32"),
                            "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS": "12:10:08:06:04:02"}
        self.dvs_mirror.verify_session(dvs, session, asic_db=expected_asic_db, src_ports=src_asic_ports, direction="RX")

        # mirror session move round 2
        # remove port channel member
        self.dvs_lag.remove_port_channel_member("080", "Ethernet32")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # mirror session move round 3
        # create port channel member
        self.dvs_lag.create_port_channel_member("080", "Ethernet32")
        self.dvs_mirror.verify_session_status(session)

        # check monitor port
        expected_asic_db = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap.get("Ethernet32"),
                            "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS": "12:10:08:06:04:02"}
        self.dvs_mirror.verify_session(dvs, session, asic_db=expected_asic_db, src_ports=src_asic_ports, direction="RX")

        # mirror session move round 4
        # remove route
        dvs.remove_route("13.13.13.0/24")
        self.dvs_mirror.verify_session_status(session)

        # check monitor port
        expected_asic_db = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap.get("Ethernet64"),
                            "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS": "02:04:06:08:10:12"}
        self.dvs_mirror.verify_session(dvs, session, asic_db=expected_asic_db, src_ports=src_asic_ports, direction="RX")

        # remove neighbor; remove ip address to port channel 080
        dvs.remove_neighbor("PortChannel080", "200.0.0.1")
        dvs.remove_ip_address("PortChannel080", "200.0.0.0/31")

        # bring down; remove port channel member; remove port channel
        dvs.set_interface_status("Ethernet32", "down")
        dvs.set_interface_status("PortChannel080", "down")
        self.dvs_lag.remove_port_channel_member("080", "Ethernet32")
        self.dvs_lag.remove_port_channel("080")
        self.dvs_mirror.verify_session_status(session)


        # remove route; remove neighbor; remove ip; bring down port
        dvs.remove_route("13.13.0.0/16")
        dvs.remove_neighbor("Ethernet64", "100.0.0.1")
        dvs.remove_ip_address("Ethernet64", "100.0.0.0/31")
        dvs.set_interface_status("Ethernet64", "down")
        self.dvs_mirror.verify_session_status(session, status="inactive")


        # remove mirror session
        self.dvs_mirror.remove_mirror_session(session)
        self.dvs_mirror.verify_no_mirror()

    def test_LAGMirrorToERSPANLagAddRemove(self, dvs, testlog):
        """
        This test covers basic LAG mirror session creation and removal operations
        with destination port sits in a LAG
        Operation flow:
        1. Create source LAG 
        2. configure mirror sesion with LAG as source port.
        3. Create destination LAG; assign IP; create directly connected neighbor
           The session shoudl be up only at this time.
        4. Remove neighbor; remove IP; remove LAG
        5. Remove mirror session

        """
        dvs.setup_db()
        pmap = dvs.counters_db.get_entry("COUNTERS_PORT_NAME_MAP", "")
        pmap = dict(pmap)

        session = "TEST_SESSION"
        src_port1="Ethernet0"
        po_src_port="PortChannel001"
        src_port2="Ethernet4"
        src_ports = "PortChannel001,Ethernet8"
        src_asic_ports = ["Ethernet0", "Ethernet4", "Ethernet8"]

        # create port channel; create port channel member
        self.dvs_lag.create_port_channel("001")
        self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 1)
        self.dvs_lag.create_port_channel_member("001", src_port1)
        self.dvs_lag.create_port_channel_member("001", src_port2)

        # bring up port channel and port channel member
        dvs.set_interface_status(po_src_port, "up")
        dvs.set_interface_status(src_port1, "up")
        dvs.set_interface_status(src_port2, "up")

        # create mirror session
        self.dvs_mirror.create_erspan_session(session, "10.10.10.10", "11.11.11.11", "0x6558", "8", "100", "0", None, src_ports)
        self.dvs_mirror.verify_session_status(session, status="inactive")
        
        # create port channel; create port channel member
        self.dvs_lag.create_port_channel("008")
        lag_entries = self.dvs_vlan.asic_db.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_LAG", 2)
        self.dvs_lag.create_port_channel_member("008", "Ethernet88")

        # bring up port channel and port channel member
        dvs.set_interface_status("PortChannel008", "up")
        dvs.set_interface_status("Ethernet88", "up")

        # add ip address to port channel 008
        dvs.add_ip_address("PortChannel008", "11.11.11.0/24")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # create neighbor to port channel 008
        dvs.add_neighbor("PortChannel008", "11.11.11.11", "88:88:88:88:88:88")
        self.dvs_mirror.verify_session_status(session)

        # Check src_port state.
        expected_asic_db = {"SAI_MIRROR_SESSION_ATTR_MONITOR_PORT": pmap.get("Ethernet88"),
                            "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS": "88:88:88:88:88:88"}

        self.dvs_mirror.verify_session(dvs, session, asic_db=expected_asic_db, src_ports=src_asic_ports)

        # remove neighbor
        dvs.remove_neighbor("PortChannel008", "11.11.11.11")
        self.dvs_mirror.verify_session_status(session, status="inactive")

        # remove ip address
        dvs.remove_ip_address("PortChannel008", "11.11.11.0/24")
        self.dvs_mirror.verify_session_status(session, status="inactive")
        # bring down port channel and port channel member
        dvs.set_interface_status("PortChannel008", "down")
        dvs.set_interface_status("Ethernet88", "down")

        # remove port channel member; remove port channel
        self.dvs_lag.remove_port_channel_member("008", "Ethernet88")
        self.dvs_lag.remove_port_channel("008")
        self.dvs_lag.remove_port_channel_member("001", src_port1)
        self.dvs_lag.remove_port_channel_member("001", src_port2)
        self.dvs_lag.remove_port_channel("001")

        # remove mirror session
        self.dvs_mirror.remove_mirror_session(session)
        self.dvs_mirror.verify_no_mirror()

