# This test suite covers the functionality of mirror feature in SwSS
import distro
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

    def add_ip_address(self, interface, ip):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        else:
            tbl_name = "INTERFACE"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        fvs = swsscommon.FieldValuePairs([("NULL", "NULL")])
        tbl.set(interface + "|" + ip, fvs)
        tbl.set(interface, fvs)
        time.sleep(1)

    def remove_ip_address(self, interface, ip):
        if interface.startswith("PortChannel"):
            tbl_name = "PORTCHANNEL_INTERFACE"
        elif interface.startswith("Vlan"):
            tbl_name = "VLAN_INTERFACE"
        else:
            tbl_name = "INTERFACE"
        tbl = swsscommon.Table(self.cdb, tbl_name)
        tbl._del(interface + "|" + ip)
        tbl._del(interface)
        time.sleep(1)

    def add_neighbor(self, interface, ip, mac):
        tbl = swsscommon.ProducerStateTable(self.pdb, "NEIGH_TABLE")
        fvs = swsscommon.FieldValuePairs([("neigh", mac),
                                          ("family", "IPv4" if "." in ip else "IPv6")])
        tbl.set(interface + ":" + ip, fvs)
        time.sleep(1)

    def remove_neighbor(self, interface, ip):
        tbl = swsscommon.ProducerStateTable(self.pdb, "NEIGH_TABLE")
        tbl._del(interface + ":" + ip)
        time.sleep(1)

    def add_route(self, dvs, prefix, nexthop):
        dvs.runcmd("ip route add " + prefix + " via " + nexthop)
        time.sleep(1)

    def remove_route(self, dvs, prefix):
        dvs.runcmd("ip route del " + prefix)
        time.sleep(1)

    def create_mirror_session(self, name, src, dst, gre, dscp, ttl, queue):
        tbl = swsscommon.Table(self.cdb, "MIRROR_SESSION")
        fvs = swsscommon.FieldValuePairs([("src_ip", src),
                                          ("dst_ip", dst),
                                          ("gre_type", gre),
                                          ("dscp", dscp),
                                          ("ttl", ttl),
                                          ("queue", queue)])
        tbl.set(name, fvs)
        time.sleep(1)

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

    def mirror_session_entry_exists(self, name):
        tbl = swsscommon.Table(self.sdb, "MIRROR_SESSION_TABLE")
        (status, _) = tbl.get(name)
        return status

    def check_syslog(self, dvs, marker, log, expected_cnt):
        (ec, out) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep \'%s\' | wc -l" % (marker, log)])
        assert out.strip() == str(expected_cnt)

    def test_MirrorInvalidEntry(self, dvs):
        """
        This test ensures that an invalid mirror session entry is not created.
        Here, "invalid" means an entry in which src IP is IPv4 while dst IP is IPv6
        (or vice versa).
        """
        self.setup_db(dvs)
        session = "TEST_SESSION"
        self.create_mirror_session(session, "1.1.1.1", "fc00::2:2:2:2", "0x6558", "8", "100", "0")
        assert self.mirror_session_entry_exists(session) == False

    def _test_MirrorAddRemove(self, dvs, testlog, v6_encap=False):
        """
        This test covers the basic mirror session creation and removal operations
        Operation flow:
        1. Create mirror session
           The session remains inactive because no nexthop/neighbor exists
        2. Bring up port; assign IP; create neighbor; create route
           The session remains inactive until the route is created
        3. Remove route; remove neighbor; remove IP; bring down port
           The session becomes inactive again till the end
        4. Remove miror session
        """
        session = "TEST_SESSION"
        src_ip = "1.1.1.1" if v6_encap == False else "fc00::1:1:1:1"
        dst_ip = "2.2.2.2" if v6_encap == False else "fc00::2:2:2:2"
        intf_addr = "10.0.0.0/31" if v6_encap == False else "fc00::/126"
        nhop_ip = "10.0.0.1" if v6_encap == False else "fc00::1"

        marker = dvs.add_log_marker()
        # create mirror session
        self.create_mirror_session(session, src_ip, dst_ip, "0x6558", "8", "100", "0")
        assert self.get_mirror_session_state(session)["status"] == "inactive"
        assert self.get_mirror_session_state(session)["next_hop_ip"] == ("0.0.0.0@" if v6_encap == False else "::@")
        self.check_syslog(dvs, marker, "Attached next hop observer .* for destination IP {}".format(dst_ip), 1)

        # bring up Ethernet16
        self.set_interface_status(dvs, "Ethernet16", "up")
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # add IP address to Ethernet16
        self.add_ip_address("Ethernet16", intf_addr)
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # add neighbor to Ethernet16
        self.add_neighbor("Ethernet16", nhop_ip, "02:04:06:08:10:12")
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # add route to mirror destination via next hop ip
        self.add_route(dvs, dst_ip, nhop_ip)
        assert self.get_mirror_session_state(session)["status"] == "active"
        assert self.get_mirror_session_state(session)["monitor_port"] == "Ethernet16"
        assert self.get_mirror_session_state(session)["dst_mac"] == "02:04:06:08:10:12"
        assert self.get_mirror_session_state(session)["route_prefix"] == "{}/{}".format(dst_ip, 32 if v6_encap == False else 128)

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        mirror_entries = tbl.getKeys()
        assert len(mirror_entries) == 1

        (status, fvs) = tbl.get(mirror_entries[0])
        assert status == True
        assert len(fvs) == 11
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet16"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TYPE":
                assert fv[1] == "SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE":
                assert fv[1] == "SAI_ERSPAN_ENCAPSULATION_TYPE_MIRROR_L3_GRE_TUNNEL"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION":
                assert fv[1] == "4" if v6_encap == False else "6"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TOS":
                assert fv[1] == "32"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TTL":
                assert fv[1] == "100"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS":
                assert fv[1] == src_ip
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS":
                assert fv[1] == dst_ip
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS":
                assert fv[1] == dvs.runcmd("bash -c \"ip link show eth0 | grep ether | awk '{print $2}'\"")[1].strip().upper()
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS":
                assert fv[1] == "02:04:06:08:10:12"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE":
                assert fv[1] == "25944" # 0x6558
            else:
                assert False

        # remove route
        self.remove_route(dvs, dst_ip)
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # remove neighbor
        self.remove_neighbor("Ethernet16", nhop_ip)
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # remove IP address
        self.remove_ip_address("Ethernet16", intf_addr)
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # bring down Ethernet16
        self.set_interface_status(dvs, "Ethernet16", "down")
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        marker = dvs.add_log_marker()
        # remove mirror session
        self.remove_mirror_session(session)
        self.check_syslog(dvs, marker, "Detached next hop observer for destination IP {}".format(dst_ip), 1)

    def test_MirrorAddRemove(self, dvs, testlog):
        self.setup_db(dvs)

        self._test_MirrorAddRemove(dvs, testlog)
        self._test_MirrorAddRemove(dvs, testlog, v6_encap=True)

    def create_vlan(self, dvs, vlan):
        #dvs.runcmd("ip link del Bridge")
        #dvs.runcmd("ip link add Bridge up type bridge")
        tbl = swsscommon.Table(self.cdb, "VLAN")
        fvs = swsscommon.FieldValuePairs([("vlanid", vlan)])
        tbl.set("Vlan" + vlan, fvs)
        time.sleep(1)

    def remove_vlan(self, vlan):
        tbl = swsscommon.Table(self.cdb, "VLAN")
        tbl._del("Vlan" + vlan)
        time.sleep(1)

    def create_vlan_member(self, vlan, interface):
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        fvs = swsscommon.FieldValuePairs([("tagging_mode", "untagged")])
        tbl.set("Vlan" + vlan + "|" + interface, fvs)
        time.sleep(1)

    def remove_vlan_member(self, vlan, interface):
        tbl = swsscommon.Table(self.cdb, "VLAN_MEMBER")
        tbl._del("Vlan" + vlan + "|" + interface)
        time.sleep(1)

    def create_fdb(self, vlan, mac, interface):
        tbl = swsscommon.ProducerStateTable(self.pdb, "FDB_TABLE")
        fvs = swsscommon.FieldValuePairs([("port", interface),
                                          ("type", "dynamic")])
        tbl.set("Vlan" + vlan + ":" + mac, fvs)
        time.sleep(1)

    def remove_fdb(self, vlan, mac):
        tbl = swsscommon.ProducerStateTable(self.pdb, "FDB_TABLE")
        tbl._del("Vlan" + vlan + ":" + mac)
        time.sleep(1)

    def _test_MirrorToVlanAddRemove(self, dvs, testlog, v6_encap=False):
        """
        This test covers basic mirror session creation and removal operation
        with destination port sits in a VLAN
        Opeartion flow:
        1. Create mirror session
        2. Create VLAN; assign IP; create neighbor; create FDB
           The session should be up only at this time.
        3. Remove FDB; remove neighbor; remove IP; remove VLAN
        4. Remove mirror session
        """
        session = "TEST_SESSION"
        src_ip = "5.5.5.5" if v6_encap == False else "fc00::5:5:5:5"
        # dst ip in directly connected vlan subnet
        dst_ip = "6.6.6.6" if v6_encap == False else "fc00::6:6:6:6"
        intf_addr = "6.6.6.0/24" if v6_encap == False else "fc00::6:6:6:0/112"

        marker = dvs.add_log_marker()
        # create mirror session
        self.create_mirror_session(session, src_ip, dst_ip, "0x6558", "8", "100", "0")
        assert self.get_mirror_session_state(session)["status"] == "inactive"
        assert self.get_mirror_session_state(session)["next_hop_ip"] == ("0.0.0.0@" if v6_encap == False else "::@")
        self.check_syslog(dvs, marker, "Attached next hop observer .* for destination IP {}".format(dst_ip), 1)

        # create vlan; create vlan member
        self.create_vlan(dvs, "6")
        self.create_vlan_member("6", "Ethernet4")

        # bring up vlan and member
        self.set_interface_status(dvs, "Vlan6", "up")
        self.set_interface_status(dvs, "Ethernet4", "up")

        # add ip address to vlan 6
        self.add_ip_address("Vlan6", intf_addr)
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # create neighbor to vlan 6
        self.add_neighbor("Vlan6", dst_ip, "66:66:66:66:66:66")
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # create fdb entry to ethernet4
        self.create_fdb("6", "66-66-66-66-66-66", "Ethernet4")
        assert self.get_mirror_session_state(session)["status"] == "active"

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        mirror_entries = tbl.getKeys()
        assert len(mirror_entries) == 1

        (status, fvs) = tbl.get(mirror_entries[0])
        assert status == True
        assert len(fvs) == 16
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet4"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TYPE":
                assert fv[1] == "SAI_MIRROR_SESSION_TYPE_ENHANCED_REMOTE"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_ERSPAN_ENCAPSULATION_TYPE":
                assert fv[1] == "SAI_ERSPAN_ENCAPSULATION_TYPE_MIRROR_L3_GRE_TUNNEL"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION":
                assert fv[1] == "4" if v6_encap == False else "6"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TOS":
                assert fv[1] == "32"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TTL":
                assert fv[1] == "100"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS":
                assert fv[1] == src_ip
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS":
                assert fv[1] == dst_ip
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS":
                assert fv[1] == dvs.runcmd("bash -c \"ip link show eth0 | grep ether | awk '{print $2}'\"")[1].strip().upper()
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS":
                assert fv[1] == "66:66:66:66:66:66"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE":
                assert fv[1] == "25944" # 0x6558
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_VLAN_HEADER_VALID":
                assert fv[1] == "true"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_VLAN_TPID":
                assert fv[1] == "33024"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_VLAN_ID":
                assert fv[1] == "6"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_VLAN_PRI":
                assert fv[1] == "0"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_VLAN_CFI":
                assert fv[1] == "0"
            else:
                assert False

        # remove fdb entry
        self.remove_fdb("6", "66-66-66-66-66-66")
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # remove neighbor
        self.remove_neighbor("Vlan6", dst_ip)
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # remove ip address
        self.remove_ip_address("Vlan6", intf_addr)
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # bring down vlan and member
        self.set_interface_status(dvs, "Ethernet4", "down")
        self.set_interface_status(dvs, "Vlan6", "down")

        # remove vlan member; remove vlan
        self.remove_vlan_member("6", "Ethernet4")
        self.remove_vlan("6")

        marker = dvs.add_log_marker()
        # remove mirror session
        self.remove_mirror_session(session)
        self.check_syslog(dvs, marker, "Detached next hop observer for destination IP {}".format(dst_ip), 1)

    # Ignore testcase in Debian Jessie
    # TODO: Remove this skip if Jessie support is no longer needed
    @pytest.mark.skipif(StrictVersion(distro.linux_distribution()[1]) <= StrictVersion('8.9'), reason="Debian 8.9 or before has no support")
    def test_MirrorToVlanAddRemove(self, dvs, testlog):
        self.setup_db(dvs)

        self._test_MirrorToVlanAddRemove(dvs, testlog)
        self._test_MirrorToVlanAddRemove(dvs, testlog, v6_encap=True)

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

    def _test_MirrorToLagAddRemove(self, dvs, testlog, v6_encap=False):
        """
        This test covers basic mirror session creation and removal operations
        with destination port sits in a LAG
        Operation flow:
        1. Create mirror sesion
        2. Create LAG; assign IP; create directly connected neighbor
           The session shoudl be up only at this time.
        3. Remove neighbor; remove IP; remove LAG
        4. Remove mirror session

        """
        session = "TEST_SESSION"
        src_ip = "10.10.10.10" if v6_encap == False else "fc00::10:10:10:10"
        dst_ip = "11.11.11.11" if v6_encap == False else "fc00::11:11:11:11"
        # dst ip in directly connected subnet
        intf_addr = "11.11.11.0/24" if v6_encap == False else "fc00::11:11:11:0/112"

        marker = dvs.add_log_marker()
        # create mirror session
        self.create_mirror_session(session, src_ip, dst_ip, "0x6558", "8", "100", "0")
        assert self.get_mirror_session_state(session)["status"] == "inactive"
        assert self.get_mirror_session_state(session)["next_hop_ip"] == ("0.0.0.0@" if v6_encap == False else "::@")
        self.check_syslog(dvs, marker, "Attached next hop observer .* for destination IP {}".format(dst_ip), 1)

        # create port channel; create port channel member
        self.create_port_channel(dvs, "008")
        self.create_port_channel_member("008", "Ethernet88")

        # bring up port channel and port channel member
        self.set_interface_status(dvs, "PortChannel008", "up")
        self.set_interface_status(dvs, "Ethernet88", "up")

        # add ip address to port channel 008
        self.add_ip_address("PortChannel008", intf_addr)
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # create neighbor to port channel 008
        self.add_neighbor("PortChannel008", dst_ip, "88:88:88:88:88:88")
        assert self.get_mirror_session_state(session)["status"] == "active"

        # check asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1

        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet88"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS":
                assert fv[1] == "88:88:88:88:88:88"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION":
                assert fv[1] == "4" if v6_encap == False else "6"

        # remove neighbor
        self.remove_neighbor("PortChannel008", dst_ip)
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # remove ip address
        self.remove_ip_address("PortChannel008", intf_addr)
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # bring down port channel and port channel member
        self.set_interface_status(dvs, "PortChannel008", "down")
        self.set_interface_status(dvs, "Ethernet88", "down")

        # remove port channel member; remove port channel
        self.remove_port_channel_member("008", "Ethernet88")
        self.remove_port_channel(dvs, "008")

        marker = dvs.add_log_marker()
        # remove mirror session
        self.remove_mirror_session(session)
        self.check_syslog(dvs, marker, "Detached next hop observer for destination IP {}".format(dst_ip), 1)

    def test_MirrorToLagAddRemove(self, dvs, testlog):
        self.setup_db(dvs)

        self._test_MirrorToLagAddRemove(dvs, testlog)
        self._test_MirrorToLagAddRemove(dvs, testlog, v6_encap=True)

    def _test_MirrorDestMoveVlan(self, dvs, testlog, v6_encap=False):
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
        session = "TEST_SESSION"
        src_ip = "7.7.7.7" if v6_encap == False else "fc00::7:7:7:7"
        dst_ip = "8.8.8.8" if v6_encap == False else "fc00::8:8:8:8"
        port_intf_addr = "80.0.0.0/31" if v6_encap == False else "fc00::80:0:0:0/126"
        port_nhop_ip = "80.0.0.1" if v6_encap == False else "fc00::80:0:0:1"
        port_ip_prefix = "8.8.0.0/16" if v6_encap == False else "fc00::8:8:0:0/96"
        # dst ip moves to directly connected vlan subnet
        vlan_intf_addr = "8.8.8.0/24" if v6_encap == False else "fc00::8:8:8:0/112"

        # create mirror session
        self.create_mirror_session(session, src_ip, dst_ip, "0x6558", "8", "100", "0")
        assert self.get_mirror_session_state(session)["status"] == "inactive"
        assert self.get_mirror_session_state(session)["next_hop_ip"] == ("0.0.0.0@" if v6_encap == False else "::@")

        # bring up port; add ip; add neighbor; add route
        self.set_interface_status(dvs, "Ethernet32", "up")
        self.add_ip_address("Ethernet32", port_intf_addr)
        self.add_neighbor("Ethernet32", port_nhop_ip, "02:04:06:08:10:12")
        self.add_route(dvs, port_ip_prefix, port_nhop_ip)
        assert self.get_mirror_session_state(session)["status"] == "active"

        # check monitor port
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet32"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_VLAN_HEADER_VALID":
                assert fv[1] == "false"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION":
                assert fv[1] == "4" if v6_encap == False else "6"

        # mirror session move round 1
        # create vlan; create vlan member; bring up vlan and member
        self.create_vlan(dvs, "9")
        self.create_vlan_member("9", "Ethernet48")
        self.set_interface_status(dvs, "Vlan9", "up")
        self.set_interface_status(dvs, "Ethernet48", "up")
        assert self.get_mirror_session_state(session)["status"] == "active"

        # add ip address to vlan 9
        self.add_ip_address("Vlan9", vlan_intf_addr)
        time.sleep(2)
        # inactive due to no neighbor mac or fdb entry
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # create neighbor to vlan 9
        self.add_neighbor("Vlan9", dst_ip, "88:88:88:88:88:88")
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # create fdb entry to ethernet48
        self.create_fdb("9", "88-88-88-88-88-88", "Ethernet48")
        assert self.get_mirror_session_state(session)["status"] == "active"

        # check monitor port
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet48"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_VLAN_HEADER_VALID":
                assert fv[1] == "true"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_VLAN_TPID":
                assert fv[1] == "33024"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_VLAN_ID":
                assert fv[1] == "9"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_VLAN_PRI":
                assert fv[1] == "0"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_VLAN_CFI":
                assert fv[1] == "0"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION":
                assert fv[1] == "4" if v6_encap == False else "6"

        # mirror session move round 2
        # remove fdb entry
        self.remove_fdb("9", "88-88-88-88-88-88")
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # remove neighbor
        self.remove_neighbor("Vlan9", dst_ip)
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # remove ip address
        self.remove_ip_address("Vlan9", vlan_intf_addr)
        assert self.get_mirror_session_state(session)["status"] == "active"

        # check monitor port
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet32"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_VLAN_HEADER_VALID":
                assert fv[1] == "false"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION":
                assert fv[1] == "4" if v6_encap == False else "6"

        # bring down vlan and member; remove vlan member; remove vlan
        self.set_interface_status(dvs, "Ethernet48", "down")
        self.set_interface_status(dvs, "Vlan9", "down")
        self.remove_vlan_member("9", "Ethernet48")
        self.remove_vlan("9")

        # remove route; remove neighbor; remove ip; bring down port
        self.remove_route(dvs, vlan_intf_addr)
        self.remove_neighbor("Ethernet32", port_nhop_ip)
        self.remove_ip_address("Ethernet32", port_intf_addr)
        self.set_interface_status(dvs, "Ethernet32", "down")

        # remove mirror session
        self.remove_mirror_session(session)

    # Ignore testcase in Debian Jessie
    # TODO: Remove this skip if Jessie support is no longer needed
    @pytest.mark.skipif(StrictVersion(distro.linux_distribution()[1]) <= StrictVersion('8.9'), reason="Debian 8.9 or before has no support")
    def test_MirrorDestMoveVlan(self, dvs, testlog):
        self.setup_db(dvs)

        self._test_MirrorDestMoveVlan(dvs, testlog)
        self._test_MirrorDestMoveVlan(dvs, testlog, v6_encap=True)

    def _test_MirrorDestMoveLag(self, dvs, testlog, v6_encap=False):
        """
        This test tests mirror session destination move from non-LAG to LAG
        and back to non-LAG port
        1. Create mirror session
        2. Enable non-LAG monitor port
        3. Create LAG; move to LAG with one member
        4. Remove LAG member
        5. Create LAG member
        6. Remove LAG; move to non-LAG
        7. Disable non-LAG monitor port
        8. Remove mirror session
        """
        session = "TEST_SESSION"
        src_ip = "12.12.12.12" if v6_encap == False else "fc00::12:12:12:12"
        dst_ip = "13.13.13.13" if v6_encap == False else "fc00::13:13:13:13"
        port_intf_addr = "100.0.0.0/31" if v6_encap == False else "fc00::100:0:0:0/126"
        port_nhop_ip = "100.0.0.1" if v6_encap == False else "fc00::100:0:0:1"
        port_ip_prefix = "13.13.0.0/16" if v6_encap == False else "fc00::13:13:0:0/96"
        lag_intf_addr = "200.0.0.0/31" if v6_encap == False else "fc00::200:0:0:0/126"
        lag_nhop_ip = "200.0.0.1" if v6_encap == False else "fc00::200:0:0:1"
        lag_ip_prefix = "13.13.13.0/24" if v6_encap == False else "fc00::13:13:13:0/112"

        # create mirror session
        self.create_mirror_session(session, src_ip, dst_ip, "0x6558", "8", "100", "0")
        assert self.get_mirror_session_state(session)["status"] == "inactive"
        assert self.get_mirror_session_state(session)["next_hop_ip"] == ("0.0.0.0@" if v6_encap == False else "::@")

        # bring up port; add ip; add neighbor; add route
        self.set_interface_status(dvs, "Ethernet64", "up")
        self.add_ip_address("Ethernet64", port_intf_addr)
        self.add_neighbor("Ethernet64", port_nhop_ip, "02:04:06:08:10:12")
        self.add_route(dvs, port_ip_prefix, port_nhop_ip)
        assert self.get_mirror_session_state(session)["status"] == "active"

        # check monitor port
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet64"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS":
                assert fv[1] == "02:04:06:08:10:12"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION":
                assert fv[1] == "4" if v6_encap == False else "6"

        # mirror session move round 1
        # create port channel; create port channel member; bring up
        self.create_port_channel(dvs, "080")
        self.create_port_channel_member("080", "Ethernet32")
        self.set_interface_status(dvs, "PortChannel080", "up")
        self.set_interface_status(dvs, "Ethernet32", "up")

        # add ip address to port channel 080; create neighbor to port channel 080
        self.add_ip_address("PortChannel080", lag_intf_addr)
        self.add_neighbor("PortChannel080", lag_nhop_ip, "12:10:08:06:04:02")
        assert self.get_mirror_session_state(session)["status"] == "active"

        # add route
        self.add_route(dvs, lag_ip_prefix, lag_nhop_ip)
        assert self.get_mirror_session_state(session)["status"] == "active"

        # check monitor port
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet32"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS":
                assert fv[1] == "12:10:08:06:04:02"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION":
                assert fv[1] == "4" if v6_encap == False else "6"

        # mirror session move round 2
        # remove port channel member
        self.remove_port_channel_member("080", "Ethernet32")
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # mirror session move round 3
        # create port channel member
        self.create_port_channel_member("080", "Ethernet32")
        assert self.get_mirror_session_state(session)["status"] == "active"

        # check monitor port
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet32"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS":
                assert fv[1] == "12:10:08:06:04:02"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION":
                assert fv[1] == "4" if v6_encap == False else "6"

        # mirror session move round 4
        # remove route
        self.remove_route(dvs, lag_ip_prefix)
        assert self.get_mirror_session_state(session)["status"] == "active"

        # check monitor port
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet64"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS":
                assert fv[1] == "02:04:06:08:10:12"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_IPHDR_VERSION":
                assert fv[1] == "4" if v6_encap == False else "6"

        # remove neighbor; remove ip address to port channel 080
        self.remove_neighbor("PortChannel080", lag_nhop_ip)
        self.remove_ip_address("PortChannel080", lag_intf_addr)

        # bring down; remove port channel member; remove port channel
        self.set_interface_status(dvs, "Ethernet32", "down")
        self.set_interface_status(dvs, "PortChannel080", "down")
        self.remove_port_channel_member("080", "Ethernet32")
        self.remove_port_channel(dvs, "080")
        assert self.get_mirror_session_state(session)["status"] == "active"

        # remove route; remove neighbor; remove ip; bring down port
        self.remove_route(dvs, port_ip_prefix)
        self.remove_neighbor("Ethernet64", port_nhop_ip)
        self.remove_ip_address("Ethernet64", port_intf_addr)
        self.set_interface_status(dvs, "Ethernet64", "down")
        assert self.get_mirror_session_state(session)["status"] == "inactive"

        # remove mirror session
        self.remove_mirror_session(session)

    def test_MirrorDestMoveLag(self, dvs, testlog):
        self.setup_db(dvs)

        self._test_MirrorDestMoveLag(dvs, testlog)
        self._test_MirrorDestMoveLag(dvs, testlog, v6_encap=True)

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

    def create_mirror_acl_dscp_rule(self, table, rule, dscp, session, stage=None):
        action_name = "mirror_action"
        action_name_map = {
            "ingress": "MIRROR_INGRESS_ACTION",
            "egress": "MIRROR_EGRESS_ACTION"
        }
        if stage is not None: # else it should be treated as ingress by default in orchagent
            assert stage in ('ingress', 'egress'), "invalid stage input {}".format(stage)
            action_name = action_name_map[stage]
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        fvs = swsscommon.FieldValuePairs([("priority", "1000"),
                                          (action_name, session),
                                          ("DSCP", dscp)])
        tbl.set(table + "|" + rule, fvs)
        time.sleep(1)

    def remove_mirror_acl_dscp_rule(self, table, rule):
        tbl = swsscommon.Table(self.cdb, "ACL_RULE")
        tbl._del(table + "|" + rule)
        time.sleep(1)

    def test_AclBindMirrorPerStage(self, dvs, testlog):
        """
        This test configures mirror rules with specifying explicitely
        the mirror action stage (ingress, egress) and verifies ASIC db
        entry set with correct mirror action
        """
        self.setup_db(dvs)

        session = "MIRROR_SESSION"
        acl_table = "MIRROR_TABLE"
        acl_rule = "MIRROR_RULE"

        # bring up port; assign ip; create neighbor; create route
        self.set_interface_status(dvs, "Ethernet32", "up")
        self.add_ip_address("Ethernet32", "20.0.0.0/31")
        self.add_neighbor("Ethernet32", "20.0.0.1", "02:04:06:08:10:12")
        self.add_route(dvs, "4.4.4.4", "20.0.0.1")

        # create mirror session
        self.create_mirror_session(session, "3.3.3.3", "4.4.4.4", "0x6558", "8", "100", "0")
        assert self.get_mirror_session_state(session)["status"] == "active"

        # assert mirror session in asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        mirror_session_oid = tbl.getKeys()[0]

        # create acl table
        self.create_acl_table(acl_table, ["Ethernet0", "Ethernet4"])

        for stage, asic_attr in (("ingress", "SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS"),
                                 ("egress", "SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_EGRESS")):
            # create acl rule with dscp value 48
            self.create_mirror_acl_dscp_rule(acl_table, acl_rule, "48", session, stage=stage)

            # assert acl rule is created
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
            rule_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_entries]
            assert len(rule_entries) == 1

            (status, fvs) = tbl.get(rule_entries[0])
            assert status == True

            asic_attr_found = False
            for fv in fvs:
                if fv[0] == asic_attr:
                    asic_attr_found = True

            assert asic_attr_found == True

            # remove acl rule
            self.remove_mirror_acl_dscp_rule(acl_table, acl_rule)

        # remove acl table
        self.remove_acl_table(acl_table)

        # remove mirror session
        self.remove_mirror_session(session)

        # assert no mirror session in asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 0

        # remove route; remove neighbor; remove ip; bring down port
        self.remove_route(dvs, "4.4.4.4")
        self.remove_neighbor("Ethernet32", "20.0.0.1")
        self.remove_ip_address("Ethernet32", "20.0.0.0/31")
        self.set_interface_status(dvs, "Ethernet32", "down")

    def _test_AclBindMirror(self, dvs, testlog, create_seq_test=False):
        """
        This test tests ACL associated with mirror session with DSCP value
        The DSCP value is tested on both with mask and without mask
        """

        session = "MIRROR_SESSION"
        acl_table = "MIRROR_TABLE"
        acl_rule = "MIRROR_RULE"

        # bring up port; assign ip; create neighbor; create route
        self.set_interface_status(dvs, "Ethernet32", "up")
        self.add_ip_address("Ethernet32", "20.0.0.0/31")
        self.add_neighbor("Ethernet32", "20.0.0.1", "02:04:06:08:10:12")
        if create_seq_test == False:
            self.add_route(dvs, "4.4.4.4", "20.0.0.1")

        # create mirror session
        self.create_mirror_session(session, "3.3.3.3", "4.4.4.4", "0x6558", "8", "100", "0")
        assert self.get_mirror_session_state(session)["status"] == ("active" if create_seq_test == False else "inactive")

        # check mirror session in asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == (1 if create_seq_test == False else 0)
        if create_seq_test == False:
            mirror_session_oid = tbl.getKeys()[0]

        # create acl table
        self.create_acl_table(acl_table, ["Ethernet0", "Ethernet4"])

        # create acl rule with dscp value 48
        self.create_mirror_acl_dscp_rule(acl_table, acl_rule, "48", session)

        # acl rule creation check
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        rule_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_entries]
        assert len(rule_entries) == (1 if create_seq_test == False else 0)

        if create_seq_test == True:
            self.add_route(dvs, "4.4.4.4", "20.0.0.1")

            assert self.get_mirror_session_state(session)["status"] == "active"

            # assert mirror session in asic database
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
            assert len(tbl.getKeys()) == 1
            mirror_session_oid = tbl.getKeys()[0]

            # assert acl rule is created
            tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
            rule_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_entries]
            assert len(rule_entries) == 1

        (status, fvs) = tbl.get(rule_entries[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_DSCP":
                assert fv[1] == "48&mask:0x3f"
            if fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS":
                assert fv[1] == "1:" + mirror_session_oid

        # remove acl rule
        self.remove_mirror_acl_dscp_rule(acl_table, acl_rule)

        # create acl rule with dscp value 16/16
        self.create_mirror_acl_dscp_rule(acl_table, acl_rule, "16/16", session)

        # assert acl rule is created
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_ACL_ENTRY")
        rule_entries = [k for k in tbl.getKeys() if k not in dvs.asicdb.default_acl_entries]
        assert len(rule_entries) == 1

        (status, fvs) = tbl.get(rule_entries[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_ACL_ENTRY_ATTR_FIELD_DSCP":
                assert fv[1] == "16&mask:0x10"
            if fv[0] == "SAI_ACL_ENTRY_ATTR_ACTION_MIRROR_INGRESS":
                assert fv[1] == "1:" + mirror_session_oid

        # remove acl rule
        self.remove_mirror_acl_dscp_rule(acl_table, acl_rule)

        # remove acl table
        self.remove_acl_table(acl_table)

        # remove mirror session
        self.remove_mirror_session(session)

        # assert no mirror session in asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 0

        # remove route; remove neighbor; remove ip; bring down port
        self.remove_route(dvs, "4.4.4.4")
        self.remove_neighbor("Ethernet32", "20.0.0.1")
        self.remove_ip_address("Ethernet32", "20.0.0.0/31")
        self.set_interface_status(dvs, "Ethernet32", "down")

    def test_AclBindMirror(self, dvs, testlog):
        self.setup_db(dvs)

        self._test_AclBindMirror(dvs, testlog)
        self._test_AclBindMirror(dvs, testlog, create_seq_test=True)


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
