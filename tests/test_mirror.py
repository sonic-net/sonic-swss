# This test suite covers the functionality of mirror feature in SwSS

import time

from swsscommon import swsscommon


class TestMirror(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        self.sdb = swsscommon.DBConnector(6, dvs.redis_sock, 0)

    def set_interface_status(self, interface, admin_status):
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
        time.sleep(1)

    def add_neighbor(self, interface, ip, mac):
        tbl = swsscommon.ProducerStateTable(self.pdb, "NEIGH_TABLE")
        fvs = swsscommon.FieldValuePairs([("neigh", mac),
                                          ("family", "IPv4")])
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
        status = ""
        tbl = swsscommon.Table(self.sdb, "MIRROR_SESSION")
        (status, fvs) = tbl.get(name)
        assert status == True
        assert len(fvs) > 0
        for fv in fvs:
            if fv[0] == "status":
                status = fv[1]
                return status

        # If there is no status field, then it is a logic error
        assert False

    def test_MirrorAddRemove(self, dvs):
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
        self.setup_db(dvs)

        session = "TEST_SESSION"

        # create mirror session
        self.create_mirror_session(session, "1.1.1.1", "2.2.2.2", "0x6558", "8", "100", "0")
        assert self.get_mirror_session_status(session) == "inactive"

        # bring up Ethernet16
        self.set_interface_status("Ethernet16", "up")
        assert self.get_mirror_session_status(session) == "inactive"

        # add IP address to Ethernet16
        self.add_ip_address("Ethernet16", "10.0.0.0/31")
        assert self.get_mirror_session_status(session) == "inactive"

        # add neighbor to Ethernet16
        self.add_neighbor("Ethernet16", "10.0.0.1", "02:04:06:08:10:12")
        assert self.get_mirror_session_status(session) == "inactive"

        # add route to mirror destination via 10.0.0.1
        self.add_route(dvs, "2.2.2.2", "10.0.0.1")
        assert self.get_mirror_session_status(session) == "active"

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
                assert fv[1] == "4"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TOS":
                assert fv[1] == "32"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TTL":
                assert fv[1] == "100"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS":
                assert fv[1] == "1.1.1.1"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS":
                assert fv[1] == "2.2.2.2"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_SRC_MAC_ADDRESS":
                assert fv[1] == dvs.runcmd("bash -c \"ip link show eth0 | grep ether | awk '{print $2}'\"")[1].strip().upper()
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS":
                assert fv[1] == "02:04:06:08:10:12"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_GRE_PROTOCOL_TYPE":
                assert fv[1] == "25944" # 0x6558
            else:
                assert False

        # remove route
        self.remove_route(dvs, "2.2.2.2")
        assert self.get_mirror_session_status(session) == "inactive"

        # remove neighbor
        self.remove_neighbor("Ethernet16", "10.0.0.1")
        assert self.get_mirror_session_status(session) == "inactive"

        # remove IP address
        self.remove_ip_address("Ethernet16", "10.0.0.0/31")
        assert self.get_mirror_session_status(session) == "inactive"

        # bring down Ethernet16
        self.set_interface_status("Ethernet16", "down")
        assert self.get_mirror_session_status(session) == "inactive"

        # remove mirror session
        self.remove_mirror_session(session)

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


    def test_MirrorToVlanAddRemove(self, dvs):
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
        self.setup_db(dvs)

        session = "TEST_SESSION"

        # create mirror session
        self.create_mirror_session(session, "5.5.5.5", "6.6.6.6", "0x6558", "8", "100", "0")
        assert self.get_mirror_session_status(session) == "inactive"

        # create vlan; create vlan member
        self.create_vlan(dvs, "6")
        self.create_vlan_member("6", "Ethernet4")

        # bring up vlan and member
        self.set_interface_status("Vlan6", "up")
        self.set_interface_status("Ethernet4", "up")

        # add ip address to vlan 6
        self.add_ip_address("Vlan6", "6.6.6.0/24")
        assert self.get_mirror_session_status(session) == "inactive"

        # create neighbor to vlan 6
        self.add_neighbor("Vlan6", "6.6.6.6", "66:66:66:66:66:66")
        assert self.get_mirror_session_status(session) == "inactive"

        # create fdb entry to ethernet4
        self.create_fdb("6", "66-66-66-66-66-66", "Ethernet4")
        assert self.get_mirror_session_status(session) == "active"

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
                assert fv[1] == "4"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TOS":
                assert fv[1] == "32"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_TTL":
                assert fv[1] == "100"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_SRC_IP_ADDRESS":
                assert fv[1] == "5.5.5.5"
            elif fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_IP_ADDRESS":
                assert fv[1] == "6.6.6.6"
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
        assert self.get_mirror_session_status(session) == "inactive"

        # remove neighbor
        self.remove_neighbor("Vlan6", "6.6.6.6")
        assert self.get_mirror_session_status(session) == "inactive"

        # remove ip address
        self.remove_ip_address("Vlan6", "6.6.6.0/24")
        assert self.get_mirror_session_status(session) == "inactive"

        # bring down vlan and member
        self.set_interface_status("Ethernet4", "down")
        self.set_interface_status("Vlan6", "down")

        # remove vlan member; remove vlan
        self.remove_vlan_member("6", "Ethernet4")
        self.remove_vlan("6")

        # remove mirror session
        self.remove_mirror_session(session)

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


    def test_MirrorToLagAddRemove(self, dvs):
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
        self.setup_db(dvs)

        session = "TEST_SESSION"

        # create mirror session
        self.create_mirror_session(session, "10.10.10.10", "11.11.11.11", "0x6558", "8", "100", "0")
        assert self.get_mirror_session_status(session) == "inactive"

        # create port channel; create port channel member
        self.create_port_channel(dvs, "008")
        self.create_port_channel_member("008", "Ethernet88")

        # bring up port channel and port channel member
        self.set_interface_status("PortChannel008", "up")
        self.set_interface_status("Ethernet88", "up")

        # add ip address to port channel 008
        self.add_ip_address("PortChannel008", "11.11.11.0/24")
        assert self.get_mirror_session_status(session) == "inactive"

        # create neighbor to port channel 008
        self.add_neighbor("PortChannel008", "11.11.11.11", "88:88:88:88:88:88")
        assert self.get_mirror_session_status(session) == "active"

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

        # remove neighbor
        self.remove_neighbor("PortChannel008", "11.11.11.11")
        assert self.get_mirror_session_status(session) == "inactive"

        # remove ip address
        self.remove_ip_address("PortChannel008", "11.11.11.0/24")
        assert self.get_mirror_session_status(session) == "inactive"

        # bring down port channel and port channel member
        self.set_interface_status("PortChannel008", "down")
        self.set_interface_status("Ethernet88", "down")

        # remove port channel member; remove port channel
        self.remove_port_channel_member("008", "Ethernet88")
        self.remove_port_channel(dvs, "008")

        # remove mirror session
        self.remove_mirror_session(session)


    def test_MirrorDestMoveVlan(self, dvs):
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
        self.setup_db(dvs)

        session = "TEST_SESSION"

        # create mirror session
        self.create_mirror_session(session, "7.7.7.7", "8.8.8.8", "0x6558", "8", "100", "0")
        assert self.get_mirror_session_status(session) == "inactive"

        # bring up port; add ip; add neighbor; add route
        self.set_interface_status("Ethernet32", "up")
        self.add_ip_address("Ethernet32", "80.0.0.0/31")
        self.add_neighbor("Ethernet32", "80.0.0.1", "02:04:06:08:10:12")
        self.add_route(dvs, "8.8.0.0/16", "80.0.0.1")
        assert self.get_mirror_session_status(session) == "active"

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

        # mirror session move round 1
        # create vlan; create vlan member; bring up vlan and member
        self.create_vlan(dvs, "9")
        self.create_vlan_member("9", "Ethernet48")
        self.set_interface_status("Vlan9", "up")
        self.set_interface_status("Ethernet48", "up")
        assert self.get_mirror_session_status(session) == "active"

        # add ip address to vlan 9
        self.add_ip_address("Vlan9", "8.8.8.0/24")
        assert self.get_mirror_session_status(session) == "inactive"

        # create neighbor to vlan 9
        self.add_neighbor("Vlan9", "8.8.8.8", "88:88:88:88:88:88")
        assert self.get_mirror_session_status(session) == "inactive"

        # create fdb entry to ethernet48
        self.create_fdb("9", "88-88-88-88-88-88", "Ethernet48")
        assert self.get_mirror_session_status(session) == "active"

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

        # mirror session move round 2
        # remove fdb entry
        self.remove_fdb("9", "88-88-88-88-88-88")
        assert self.get_mirror_session_status(session) == "inactive"

        # remove neighbor
        self.remove_neighbor("Vlan9", "8.8.8.8")
        assert self.get_mirror_session_status(session) == "inactive"

        # remove ip address
        self.remove_ip_address("Vlan9", "8.8.8.0/24")
        assert self.get_mirror_session_status(session) == "active"

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

        # bring down vlan and member; remove vlan member; remove vlan
        self.set_interface_status("Ethernet48", "down")
        self.set_interface_status("Vlan9", "down")
        self.remove_vlan_member("9", "Ethernet48")
        self.remove_vlan("9")

        # remove route; remove neighbor; remove ip; bring down port
        self.remove_route(dvs, "8.8.8.0/24")
        self.remove_neighbor("Ethernet32", "80.0.0.1")
        self.remove_ip_address("Ethernet32", "80.0.0.0/31")
        self.set_interface_status("Ethernet32", "down")

        # remove mirror session
        self.remove_mirror_session(session)


    def test_MirrorDestMoveLag(self, dvs):
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
        self.setup_db(dvs)

        session = "TEST_SESSION"

        # create mirror session
        self.create_mirror_session(session, "12.12.12.12", "13.13.13.13", "0x6558", "8", "100", "0")
        assert self.get_mirror_session_status(session) == "inactive"

        # bring up port; add ip; add neighbor; add route
        self.set_interface_status("Ethernet64", "up")
        self.add_ip_address("Ethernet64", "100.0.0.0/31")
        self.add_neighbor("Ethernet64", "100.0.0.1", "02:04:06:08:10:12")
        self.add_route(dvs, "13.13.0.0/16", "100.0.0.1")
        assert self.get_mirror_session_status(session) == "active"

        # check monitor port
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet64"
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS":
                assert fv[1] == "02:04:06:08:10:12"

        # mirror session move round 1
        # create port channel; create port channel member; bring up
        self.create_port_channel(dvs, "080")
        self.create_port_channel_member("080", "Ethernet32")
        self.set_interface_status("PortChannel080", "up")
        self.set_interface_status("Ethernet32", "up")

        # add ip address to port channel 080; create neighbor to port channel 080
        self.add_ip_address("PortChannel080", "200.0.0.0/31")
        self.add_neighbor("PortChannel080", "200.0.0.1", "12:10:08:06:04:02")
        assert self.get_mirror_session_status(session) == "active"

        # add route
        self.add_route(dvs, "13.13.13.0/24", "200.0.0.1")
        assert self.get_mirror_session_status(session) == "active"

        # check monitor port
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet32"
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS":
                assert fv[1] == "12:10:08:06:04:02"

        # mirror session move round 2
        # remove port channel member
        self.remove_port_channel_member("080", "Ethernet32")
        assert self.get_mirror_session_status(session) == "inactive"

        # mirror session move round 3
        # create port channel member
        self.create_port_channel_member("080", "Ethernet32")
        assert self.get_mirror_session_status(session) == "active"

        # check monitor port
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet32"
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS":
                assert fv[1] == "12:10:08:06:04:02"

        # mirror session move round 4
        # remove route
        self.remove_route(dvs, "13.13.13.0/24")
        assert self.get_mirror_session_status(session) == "active"

        port_oid = ""
        # check monitor port
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        (status, fvs) = tbl.get(tbl.getKeys()[0])
        assert status == True
        for fv in fvs:
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_MONITOR_PORT":
                assert dvs.asicdb.portoidmap[fv[1]] == "Ethernet64"
            if fv[0] == "SAI_MIRROR_SESSION_ATTR_DST_MAC_ADDRESS":
                assert fv[1] == "02:04:06:08:10:12"

        # remove neighbor; remove ip address to port channel 080
        self.remove_neighbor("PortChannel080", "200.0.0.1")
        self.remove_ip_address("PortChannel080", "200.0.0.0/31")

        # bring down; remove port channel member; remove port channel
        self.set_interface_status("Ethernet32", "down")
        self.set_interface_status("PortChannel080", "down")
        self.remove_port_channel_member("080", "Ethernet32")
        self.remove_port_channel(dvs, "080")
        assert self.get_mirror_session_status(session) == "active"

        # remove route; remove neighbor; remove ip; bring down port
        self.remove_route(dvs, "13.13.0.0/16")
        self.remove_neighbor("Ethernet64", "100.0.0.1")
        self.remove_ip_address("Ethernet64", "100.0.0.0/31")
        self.set_interface_status("Ethernet64", "down")
        assert self.get_mirror_session_status(session) == "inactive"

        # remove mirror session
        self.remove_mirror_session(session)


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

    def test_AclBindMirror(self, dvs):
        """
        This test tests ACL associated with mirror session with DSCP value
        The DSCP value is tested on both with mask and without mask
        """
        self.setup_db(dvs)

        session = "MIRROR_SESSION"
        acl_table = "MIRROR_TABLE"
        acl_rule = "MIRROR_RULE"

        # bring up port; assign ip; create neighbor; create route
        self.set_interface_status("Ethernet32", "up")
        self.add_ip_address("Ethernet32", "20.0.0.0/31")
        self.add_neighbor("Ethernet32", "20.0.0.1", "02:04:06:08:10:12")
        self.add_route(dvs, "4.4.4.4", "20.0.0.1")

        # create mirror session
        self.create_mirror_session(session, "3.3.3.3", "4.4.4.4", "0x6558", "8", "100", "0")
        assert self.get_mirror_session_status(session) == "active"

        # assert mirror session in asic database
        tbl = swsscommon.Table(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_MIRROR_SESSION")
        assert len(tbl.getKeys()) == 1
        mirror_session_oid = tbl.getKeys()[0]

        # create acl table
        self.create_acl_table(acl_table, ["Ethernet0", "Ethernet4"])

        # create acl rule with dscp value 48
        self.create_mirror_acl_dscp_rule(acl_table, acl_rule, "48", session)

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
        self.set_interface_status("Ethernet32", "down")
