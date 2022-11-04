import time
import pytest
import json

from ipaddress import ip_network, ip_address, IPv4Address
from swsscommon import swsscommon

from mux_neigh_miss_tests import *

def create_fvs(**kwargs):
    return swsscommon.FieldValuePairs(list(kwargs.items()))

tunnel_nh_id = 0

class TestMuxTunnelBase():
    APP_MUX_CABLE               = "MUX_CABLE_TABLE"
    APP_NEIGH_TABLE             = "NEIGH_TABLE"
    APP_TUNNEL_DECAP_TABLE_NAME = "TUNNEL_DECAP_TABLE"
    ASIC_TUNNEL_TABLE           = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL"
    ASIC_TUNNEL_TERM_ENTRIES    = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY"
    ASIC_RIF_TABLE              = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
    ASIC_VRF_TABLE              = "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"
    ASIC_NEIGH_TABLE            = "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY"
    ASIC_NEXTHOP_TABLE          = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP"
    ASIC_ROUTE_TABLE            = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY"
    ASIC_FDB_TABLE              = "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY"
    ASIC_SWITCH_TABLE           = "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH"
    CONFIG_MUX_CABLE            = "MUX_CABLE"
    CONFIG_PEER_SWITCH          = "PEER_SWITCH"
    STATE_FDB_TABLE             = "FDB_TABLE"
    MUX_TUNNEL_0                = "MuxTunnel0"
    PEER_SWITCH_HOST            = "peer_switch_hostname"
    CONFIG_TUNNEL_TABLE_NAME    = "TUNNEL"
    ASIC_QOS_MAP_TABLE_KEY      = "ASIC_STATE:SAI_OBJECT_TYPE_QOS_MAP"
    TUNNEL_QOS_MAP_NAME         = "AZURE_TUNNEL"

    SELF_IPV4                   = "10.1.0.32"
    PEER_IPV4                   = "10.1.0.33"
    SERV1_IPV4                  = "192.168.0.100"
    SERV1_IPV6                  = "fc02:1000::100"
    SERV2_IPV4                  = "192.168.0.101"
    SERV2_IPV6                  = "fc02:1000::101"
    SERV3_IPV4                  = "192.168.0.102"
    SERV3_IPV6                  = "fc02:1000::102"
    NEIGH1_IPV4                 = "192.168.0.200"
    NEIGH1_IPV6                 = "fc02:1000::200"
    NEIGH2_IPV4                 = "192.168.0.201"
    NEIGH2_IPV6                 = "fc02:1000::201"
    NEIGH3_IPV4                 = "192.168.0.202"
    NEIGH3_IPV6                 = "fc02:1000::202"
    IPV4_MASK                   = "/32"
    IPV6_MASK                   = "/128"
    TUNNEL_NH_ID                = 0
    ACL_PRIORITY                = "999"
    VLAN_1000                   = "Vlan1000"

    PING_CMD                    = "timeout 0.5 ping -c1 -W1 -i0 -n -q {ip}"

    SAI_ROUTER_INTERFACE_ATTR_TYPE = "SAI_ROUTER_INTERFACE_ATTR_TYPE"
    SAI_ROUTER_INTERFACE_TYPE_VLAN = "SAI_ROUTER_INTERFACE_TYPE_VLAN"

    TC_TO_DSCP_MAP = {str(i):str(i) for i in range(0, 8)}
    TC_TO_QUEUE_MAP = {str(i):str(i) for i in range(0, 8)}
    DSCP_TO_TC_MAP = {str(i):str(1) for i in range(0, 64)}
    TC_TO_PRIORITY_GROUP_MAP = {str(i):str(i) for i in range(0, 8)}

    DEFAULT_TUNNEL_PARAMS = {
        "tunnel_type": "IPINIP",
        "dst_ip": SELF_IPV4,
        "dscp_mode": "pipe",
        "ecn_mode": "standard",
        "ttl_mode": "pipe",
        "decap_tc_to_pg_map": "[" + swsscommon.CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME + "|" + TUNNEL_QOS_MAP_NAME + "]",
        "decap_dscp_to_tc_map": "[" + swsscommon.CFG_DSCP_TO_TC_MAP_TABLE_NAME + "|" + TUNNEL_QOS_MAP_NAME + "]",
        "encap_tc_to_queue_map": "[" + swsscommon.CFG_TC_TO_QUEUE_MAP_TABLE_NAME + "|" + TUNNEL_QOS_MAP_NAME + "]",
        "encap_tc_to_dscp_map": "[" + swsscommon.CFG_TC_TO_DSCP_MAP_TABLE_NAME + "|" + TUNNEL_QOS_MAP_NAME + "]"
    }

    DEFAULT_PEER_SWITCH_PARAMS = {
        "address_ipv4": PEER_IPV4
    }

    ecn_modes_map = {
        "standard"       : "SAI_TUNNEL_DECAP_ECN_MODE_STANDARD",
        "copy_from_outer": "SAI_TUNNEL_DECAP_ECN_MODE_COPY_FROM_OUTER"
    }

    dscp_modes_map = {
        "pipe"    : "SAI_TUNNEL_DSCP_MODE_PIPE_MODEL",
        "uniform" : "SAI_TUNNEL_DSCP_MODE_UNIFORM_MODEL"
    }

    ttl_modes_map = {
        "pipe"    : "SAI_TUNNEL_TTL_MODE_PIPE_MODEL",
        "uniform" : "SAI_TUNNEL_TTL_MODE_UNIFORM_MODEL"
    }

    def check_syslog(self, dvs, marker, err_log, expected_cnt):
        (exitcode, num) = dvs.runcmd(['sh', '-c', "awk \'/%s/,ENDFILE {print;}\' /var/log/syslog | grep \"%s\" | wc -l" % (marker, err_log)])
        assert num.strip() >= str(expected_cnt)

    def create_vlan_interface(self, dvs):
        confdb = dvs.get_config_db()

        fvs = {"vlanid": "1000"}
        confdb.create_entry("VLAN", self.VLAN_1000, fvs)

        fvs = {"tagging_mode": "untagged"}
        confdb.create_entry("VLAN_MEMBER", "Vlan1000|Ethernet0", fvs)
        confdb.create_entry("VLAN_MEMBER", "Vlan1000|Ethernet4", fvs)
        confdb.create_entry("VLAN_MEMBER", "Vlan1000|Ethernet8", fvs)

        fvs = {"NULL": "NULL"}
        confdb.create_entry("VLAN_INTERFACE", self.VLAN_1000, fvs)
        confdb.create_entry("VLAN_INTERFACE", "Vlan1000|192.168.0.1/24", fvs)
        confdb.create_entry("VLAN_INTERFACE", "Vlan1000|fc02:1000::1/64", fvs)

        dvs.runcmd("config interface startup Ethernet0")
        dvs.runcmd("config interface startup Ethernet4")
        dvs.runcmd("config interface startup Ethernet8")

    def create_mux_cable(self, confdb):
        fvs = {"server_ipv4": self.SERV1_IPV4+self.IPV4_MASK,
               "server_ipv6": self.SERV1_IPV6+self.IPV6_MASK}
        confdb.create_entry(self.CONFIG_MUX_CABLE, "Ethernet0", fvs)

        fvs = {"server_ipv4": self.SERV2_IPV4+self.IPV4_MASK,
               "server_ipv6": self.SERV2_IPV6+self.IPV6_MASK}
        confdb.create_entry(self.CONFIG_MUX_CABLE, "Ethernet4", fvs)

        fvs = {"server_ipv4": self.SERV3_IPV4+self.IPV4_MASK,
               "server_ipv6": self.SERV3_IPV6+self.IPV6_MASK}
        confdb.create_entry(self.CONFIG_MUX_CABLE, "Ethernet8", fvs)

    def set_mux_state(self, appdb, ifname, state_change):

        ps = swsscommon.ProducerStateTable(appdb, self.APP_MUX_CABLE)

        fvs = create_fvs(state=state_change)

        ps.set(ifname, fvs)

        time.sleep(1)

    def get_switch_oid(self, asicdb):
        # Assumes only one switch is ever present
        keys = asicdb.wait_for_n_keys(self.ASIC_SWITCH_TABLE, 1)
        return keys[0]

    def get_vlan_rif_oid(self, asicdb):
        # create_vlan_interface should be called before this method
        # Assumes only one VLAN RIF is present
        rifs = asicdb.get_keys(self.ASIC_RIF_TABLE)

        vlan_oid = ''
        for rif_key in rifs:
            entry = asicdb.get_entry(self.ASIC_RIF_TABLE, rif_key)
            if entry[self.SAI_ROUTER_INTERFACE_ATTR_TYPE] == self.SAI_ROUTER_INTERFACE_TYPE_VLAN:
                vlan_oid = rif_key
                break

        return vlan_oid

    def check_neigh_in_asic_db(self, asicdb, ip, expected=True):
        rif_oid = self.get_vlan_rif_oid(asicdb)
        switch_oid = self.get_switch_oid(asicdb)
        neigh_key_map = {
            "ip": ip,
            "rif": rif_oid,
            "switch_id": switch_oid
        }
        expected_key = json.dumps(neigh_key_map, sort_keys=True, separators=(',', ':'))

        if expected:
            nbr_keys = asicdb.wait_for_matching_keys(self.ASIC_NEIGH_TABLE, [expected_key])

            for key in nbr_keys:
                if ip in key:
                    return key

        else:
            asicdb.wait_for_deleted_keys(self.ASIC_NEIGH_TABLE, [expected_key])

        return ''

    def check_tnl_nexthop_in_asic_db(self, asicdb, expected=1):

        global tunnel_nh_id

        nh = asicdb.wait_for_n_keys(self.ASIC_NEXTHOP_TABLE, expected)

        for key in nh:
            fvs = asicdb.get_entry(self.ASIC_NEXTHOP_TABLE, key)
            if fvs.get("SAI_NEXT_HOP_ATTR_TYPE") == "SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP":
                tunnel_nh_id = key

        assert tunnel_nh_id

    def check_nexthop_in_asic_db(self, asicdb, key, standby=False):

        fvs = asicdb.get_entry(self.ASIC_ROUTE_TABLE, key)
        if not fvs:
            assert False

        nhid = fvs.get("SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID")
        if standby:
            assert (nhid == tunnel_nh_id)
        else:
            assert (nhid != tunnel_nh_id)

    def check_nexthop_group_in_asic_db(self, asicdb, key, num_tnl_nh=0):

        fvs = asicdb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY", key)

        nhg_id = fvs["SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID"]

        asicdb.wait_for_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP", nhg_id)

        # Two NH group members are expected to be added
        keys = asicdb.wait_for_n_keys("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER", 2)

        count = 0

        for k in keys:
            fvs = asicdb.get_entry("ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER", k)
            assert fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID"] == nhg_id

            # Count the number of Nexthop member pointing to tunnel
            if fvs["SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID"] == tunnel_nh_id:
                count += 1

        assert num_tnl_nh == count

    def add_neighbor(self, dvs, ip, mac):
        if ip_address(ip).version == 6:
            dvs.runcmd("ip -6 neigh replace " + ip + " lladdr " + mac + " dev Vlan1000")
        else:
            dvs.runcmd("ip -4 neigh replace " + ip + " lladdr " + mac + " dev Vlan1000")

    def del_neighbor(self, dvs, ip):
        cmd = 'ip neigh del {} dev {}'.format(ip, self.VLAN_1000)
        dvs.runcmd(cmd)

    def add_fdb(self, dvs, port, mac):

        appdb = dvs.get_app_db()
        ps = swsscommon.ProducerStateTable(appdb.db_connection, "FDB_TABLE")
        fvs = swsscommon.FieldValuePairs([("port", port), ("type", "dynamic")])

        ps.set("Vlan1000:"+mac, fvs)

        time.sleep(1)

    def del_fdb(self, dvs, mac):

        appdb = dvs.get_app_db()
        ps = swsscommon.ProducerStateTable(appdb.db_connection, "FDB_TABLE")
        ps._del("Vlan1000:"+mac)

        time.sleep(1)

    def create_and_test_neighbor(self, confdb, appdb, asicdb, dvs, dvs_route):

        self.create_vlan_interface(dvs)

        self.create_mux_cable(confdb)

        self.set_mux_state(appdb, "Ethernet0", "active")
        self.set_mux_state(appdb, "Ethernet4", "standby")

        self.add_neighbor(dvs, self.SERV1_IPV4, "00:00:00:00:00:01")
        srv1_v4 = self.check_neigh_in_asic_db(asicdb, self.SERV1_IPV4)

        self.add_neighbor(dvs, self.SERV1_IPV6, "00:00:00:00:00:01")
        srv1_v6 = self.check_neigh_in_asic_db(asicdb, self.SERV1_IPV6)

        existing_keys = asicdb.get_keys(self.ASIC_NEIGH_TABLE)

        self.add_neighbor(dvs, self.SERV2_IPV4, "00:00:00:00:00:02")
        self.add_neighbor(dvs, self.SERV2_IPV6, "00:00:00:00:00:02")
        time.sleep(1)

        # In standby mode, the entry must not be added to Neigh table but Route
        asicdb.wait_for_matching_keys(self.ASIC_NEIGH_TABLE, existing_keys)
        dvs_route.check_asicdb_route_entries(
            [self.SERV2_IPV4+self.IPV4_MASK, self.SERV2_IPV6+self.IPV6_MASK]
        )

        # The first standby route also creates as tunnel Nexthop
        self.check_tnl_nexthop_in_asic_db(asicdb, 3)

        # Change state to Standby. This will delete Neigh and add Route
        self.set_mux_state(appdb, "Ethernet0", "standby")

        asicdb.wait_for_deleted_entry(self.ASIC_NEIGH_TABLE, srv1_v4)
        asicdb.wait_for_deleted_entry(self.ASIC_NEIGH_TABLE, srv1_v6)
        dvs_route.check_asicdb_route_entries(
            [self.SERV1_IPV4+self.IPV4_MASK, self.SERV1_IPV6+self.IPV6_MASK]
        )

        # Change state to Active. This will add Neigh and delete Route
        self.set_mux_state(appdb, "Ethernet4", "active")

        dvs_route.check_asicdb_deleted_route_entries(
            [self.SERV2_IPV4+self.IPV4_MASK, self.SERV2_IPV6+self.IPV6_MASK]
        )
        self.check_neigh_in_asic_db(asicdb, self.SERV2_IPV4)
        self.check_neigh_in_asic_db(asicdb, self.SERV2_IPV6)

        marker = dvs.add_log_marker()

        self.set_mux_state(appdb, "Ethernet0", "active")
        self.set_mux_state(appdb, "Ethernet0", "active")
        self.check_syslog(dvs, marker, "Maintaining current MUX state", 1)

        self.set_mux_state(appdb, "Ethernet0", "init")
        self.check_syslog(dvs, marker, "State transition from active to init is not-handled", 1)

    def create_and_test_fdb(self, appdb, asicdb, dvs, dvs_route):

        self.set_mux_state(appdb, "Ethernet0", "active")
        self.set_mux_state(appdb, "Ethernet4", "standby")

        self.add_fdb(dvs, "Ethernet0", "00-00-00-00-00-11")
        self.add_fdb(dvs, "Ethernet4", "00-00-00-00-00-12")

        ip_1 = "fc02:1000::10"
        ip_2 = "fc02:1000::11"

        self.add_neighbor(dvs, ip_1, "00:00:00:00:00:11")
        self.add_neighbor(dvs, ip_2, "00:00:00:00:00:12")

        # ip_1 is on Active Mux, hence added to Host table
        self.check_neigh_in_asic_db(asicdb, ip_1)

        # ip_2 is on Standby Mux, hence added to Route table
        dvs_route.check_asicdb_route_entries([ip_2+self.IPV6_MASK])

        # Check ip_1 move to standby mux, should be pointing to tunnel
        self.add_neighbor(dvs, ip_1, "00:00:00:00:00:12")

        # ip_1 moved to standby Mux, hence added to Route table
        dvs_route.check_asicdb_route_entries([ip_1+self.IPV6_MASK])

        # Check ip_2 move to active mux, should be host entry
        self.add_neighbor(dvs, ip_2, "00:00:00:00:00:11")

        # ip_2 moved to active Mux, hence remove from Route table
        dvs_route.check_asicdb_deleted_route_entries([ip_2+self.IPV6_MASK])
        self.check_neigh_in_asic_db(asicdb, ip_2)

        # Simulate FDB aging out test case
        ip_3 = "192.168.0.200"

        self.add_neighbor(dvs, ip_3, "00:00:00:00:00:12")

        # ip_3 is added to standby mux
        dvs_route.check_asicdb_route_entries([ip_3+self.IPV4_MASK])

        # Simulate FDB age out
        self.del_fdb(dvs, "00-00-00-00-00-12")

        # FDB ageout is not expected to change existing state of neighbor
        dvs_route.check_asicdb_route_entries([ip_3+self.IPV4_MASK])

        # Change to active
        self.set_mux_state(appdb, "Ethernet4", "active")
        dvs_route.check_asicdb_deleted_route_entries([ip_3+self.IPV4_MASK])

        self.del_fdb(dvs, "00-00-00-00-00-11")

    def create_and_test_route(self, appdb, asicdb, dvs, dvs_route):

        self.set_mux_state(appdb, "Ethernet0", "active")

        rtprefix = "2.3.4.0/24"

        dvs.runcmd(
            "vtysh -c \"configure terminal\" -c \"ip route " + rtprefix +
            " " + self.SERV1_IPV4 + "\""
        )

        pdb = dvs.get_app_db()
        pdb.wait_for_entry("ROUTE_TABLE", rtprefix)

        rtkeys = dvs_route.check_asicdb_route_entries([rtprefix])

        self.check_nexthop_in_asic_db(asicdb, rtkeys[0])

        # Change Mux state to Standby and verify route pointing to Tunnel
        self.set_mux_state(appdb, "Ethernet0", "standby")

        self.check_nexthop_in_asic_db(asicdb, rtkeys[0], True)

        # Change Mux state back to Active and verify route is not pointing to Tunnel
        self.set_mux_state(appdb, "Ethernet0", "active")

        self.check_nexthop_in_asic_db(asicdb, rtkeys[0])

        # Check route set flow and changing nexthop
        self.set_mux_state(appdb, "Ethernet4", "active")

        ps = swsscommon.ProducerStateTable(pdb.db_connection, "ROUTE_TABLE")
        fvs = swsscommon.FieldValuePairs([("nexthop", self.SERV2_IPV4), ("ifname", "Vlan1000")])
        ps.set(rtprefix, fvs)

        # Check if route was propagated to ASIC DB
        rtkeys = dvs_route.check_asicdb_route_entries([rtprefix])

        # Change Mux status for Ethernet0 and expect no change to replaced route
        self.set_mux_state(appdb, "Ethernet0", "standby")
        self.check_nexthop_in_asic_db(asicdb, rtkeys[0])

        self.set_mux_state(appdb, "Ethernet4", "standby")
        self.check_nexthop_in_asic_db(asicdb, rtkeys[0], True)

        # Delete the route
        ps._del(rtprefix)

        self.set_mux_state(appdb, "Ethernet4", "active")
        dvs_route.check_asicdb_deleted_route_entries([rtprefix])

        # Test ECMP routes

        self.set_mux_state(appdb, "Ethernet0", "active")
        self.set_mux_state(appdb, "Ethernet4", "active")

        rtprefix = "5.6.7.0/24"

        dvs_route.check_asicdb_deleted_route_entries([rtprefix])

        ps = swsscommon.ProducerStateTable(pdb.db_connection, "ROUTE_TABLE")

        fvs = swsscommon.FieldValuePairs(
                [
                    ("nexthop", self.SERV1_IPV4 + "," + self.SERV2_IPV4),
                    ("ifname", "Vlan1000,Vlan1000")
                ]
              )

        ps.set(rtprefix, fvs)

        # Check if route was propagated to ASIC DB
        rtkeys = dvs_route.check_asicdb_route_entries([rtprefix])

        # Check for nexthop group and validate nexthop group member in asic db
        self.check_nexthop_group_in_asic_db(asicdb, rtkeys[0])

        # Step: 1 - Change one NH to standby and verify ecmp route
        self.set_mux_state(appdb, "Ethernet0", "standby")
        self.check_nexthop_group_in_asic_db(asicdb, rtkeys[0], 1)

        # Step: 2 - Change the other NH to standby and verify ecmp route
        self.set_mux_state(appdb, "Ethernet4", "standby")
        self.check_nexthop_group_in_asic_db(asicdb, rtkeys[0], 2)

        # Step: 3 - Change one NH to back to Active and verify ecmp route
        self.set_mux_state(appdb, "Ethernet0", "active")
        self.check_nexthop_group_in_asic_db(asicdb, rtkeys[0], 1)

        # Step: 4 - Change the other NH to Active and verify ecmp route
        self.set_mux_state(appdb, "Ethernet4", "active")
        self.check_nexthop_group_in_asic_db(asicdb, rtkeys[0])

        ps._del(rtprefix)

        # Test IPv6 ECMP routes and start with standby config
        self.set_mux_state(appdb, "Ethernet0", "standby")
        self.set_mux_state(appdb, "Ethernet4", "standby")

        rtprefix = "2020::/64"

        dvs_route.check_asicdb_deleted_route_entries([rtprefix])

        ps = swsscommon.ProducerStateTable(pdb.db_connection, "ROUTE_TABLE")

        fvs = swsscommon.FieldValuePairs(
                [
                    ("nexthop", self.SERV1_IPV6 + "," + self.SERV2_IPV6),
                    ("ifname", "tun0,tun0")
                ]
              )

        ps.set(rtprefix, fvs)

        # Check if route was propagated to ASIC DB
        rtkeys = dvs_route.check_asicdb_route_entries([rtprefix])

        # Check for nexthop group and validate nexthop group member in asic db
        self.check_nexthop_group_in_asic_db(asicdb, rtkeys[0], 2)

        # Step: 1 - Change one NH to active and verify ecmp route
        self.set_mux_state(appdb, "Ethernet0", "active")
        self.check_nexthop_group_in_asic_db(asicdb, rtkeys[0], 1)

        # Step: 2 - Change the other NH to active and verify ecmp route
        self.set_mux_state(appdb, "Ethernet4", "active")
        self.check_nexthop_group_in_asic_db(asicdb, rtkeys[0])

        # Step: 3 - Change one NH to back to standby and verify ecmp route
        self.set_mux_state(appdb, "Ethernet0", "standby")
        self.check_nexthop_group_in_asic_db(asicdb, rtkeys[0], 1)

        # Step: 4 - Change the other NH to standby and verify ecmp route
        self.set_mux_state(appdb, "Ethernet4", "standby")
        self.check_nexthop_group_in_asic_db(asicdb, rtkeys[0], 2)

        ps._del(rtprefix)

    def get_expected_sai_qualifiers(self, portlist, dvs_acl):
        expected_sai_qualifiers = {
            "SAI_ACL_ENTRY_ATTR_PRIORITY": self.ACL_PRIORITY,
            "SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS": dvs_acl.get_port_list_comparator(portlist)
        }

        return expected_sai_qualifiers

    def create_and_test_acl(self, appdb, dvs_acl):

        # Start with active, verify NO ACL rules exists
        self.set_mux_state(appdb, "Ethernet0", "active")
        self.set_mux_state(appdb, "Ethernet4", "active")
        self.set_mux_state(appdb, "Ethernet8", "active")

        dvs_acl.verify_no_acl_rules()

        # Set one mux port to standby, verify ACL rule with inport bitmap (1 port)
        self.set_mux_state(appdb, "Ethernet0", "standby")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet0"], dvs_acl)
        dvs_acl.verify_acl_rule(sai_qualifier, action="DROP", priority=self.ACL_PRIORITY)

        # Set two mux ports to standby, verify ACL rule with inport bitmap (2 ports)
        self.set_mux_state(appdb, "Ethernet4", "standby")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet0", "Ethernet4"], dvs_acl)
        dvs_acl.verify_acl_rule(sai_qualifier, action="DROP", priority=self.ACL_PRIORITY)

        # Set one mux port to active, verify ACL rule with inport bitmap (1 port)
        self.set_mux_state(appdb, "Ethernet0", "active")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet4"], dvs_acl)
        dvs_acl.verify_acl_rule(sai_qualifier, action="DROP", priority=self.ACL_PRIORITY)

        # Set last mux port to active, verify ACL rule is deleted
        self.set_mux_state(appdb, "Ethernet4", "active")
        dvs_acl.verify_no_acl_rules()

        # Set unknown state and verify the behavior as standby
        self.set_mux_state(appdb, "Ethernet0", "unknown")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet0"], dvs_acl)
        dvs_acl.verify_acl_rule(sai_qualifier, action="DROP", priority=self.ACL_PRIORITY)

        # Verify change while setting unknown from active
        self.set_mux_state(appdb, "Ethernet4", "unknown")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet0", "Ethernet4"], dvs_acl)
        dvs_acl.verify_acl_rule(sai_qualifier, action="DROP", priority=self.ACL_PRIORITY)

        self.set_mux_state(appdb, "Ethernet0", "active")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet4"], dvs_acl)
        dvs_acl.verify_acl_rule(sai_qualifier, action="DROP", priority=self.ACL_PRIORITY)

        self.set_mux_state(appdb, "Ethernet0", "standby")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet0", "Ethernet4"], dvs_acl)
        dvs_acl.verify_acl_rule(sai_qualifier, action="DROP", priority=self.ACL_PRIORITY)

        # Verify no change while setting unknown from standby
        self.set_mux_state(appdb, "Ethernet0", "unknown")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet0", "Ethernet4"], dvs_acl)
        dvs_acl.verify_acl_rule(sai_qualifier, action="DROP", priority=self.ACL_PRIORITY)

    def create_and_test_metrics(self, appdb, statedb):

        # Set to active and test attributes for start and end time
        self.set_mux_state(appdb, "Ethernet0", "active")
        keys = statedb.get_keys("MUX_METRICS_TABLE")
        assert len(keys) != 0

        for key in keys:
            if key != "Ethernet0":
                continue
            fvs = statedb.get_entry("MUX_METRICS_TABLE", key)
            assert fvs != {}

            start = end = False
            for f, _ in fvs.items():
                if f == "orch_switch_active_start":
                    start = True
                elif f == "orch_switch_active_end":
                    end = True

            assert start
            assert end

        # Set to standby and test attributes for start and end time
        self.set_mux_state(appdb, "Ethernet0", "standby")

        keys = statedb.get_keys("MUX_METRICS_TABLE")
        assert len(keys) != 0

        for key in keys:
            if key != "Ethernet0":
                continue
            fvs = statedb.get_entry("MUX_METRICS_TABLE", key)
            assert fvs != {}

            start = end = False
            for f, v in fvs.items():
                if f == "orch_switch_standby_start":
                    start = True
                elif f == "orch_switch_standby_end":
                    end = True

            assert start
            assert end

    def check_interface_exists_in_asicdb(self, asicdb, sai_oid):
        asicdb.wait_for_entry(self.ASIC_RIF_TABLE, sai_oid)
        return True

    def check_vr_exists_in_asicdb(self, asicdb, sai_oid):
        asicdb.wait_for_entry(self.ASIC_VRF_TABLE, sai_oid)
        return True

    def create_and_test_peer(self, db, asicdb, peer_name, peer_ip, src_ip, tc_to_dscp_map_oid=None, tc_to_queue_map_oid=None):
        """ Create PEER entry verify all needed enties in ASIC DB exists """

        # check asic db table
        # There will be two tunnels, one P2MP and another P2P
        tunnels = asicdb.wait_for_n_keys(self.ASIC_TUNNEL_TABLE, 2)

        p2p_obj = None

        for tunnel_sai_obj in tunnels:
            fvs = asicdb.wait_for_entry(self.ASIC_TUNNEL_TABLE, tunnel_sai_obj)

            for field, value in fvs.items():
                if field == "SAI_TUNNEL_ATTR_TYPE":
                    assert value == "SAI_TUNNEL_TYPE_IPINIP"
                if field == "SAI_TUNNEL_ATTR_PEER_MODE":
                    if value == "SAI_TUNNEL_PEER_MODE_P2P":
                        p2p_obj = tunnel_sai_obj

        assert p2p_obj != None

        fvs = asicdb.wait_for_entry(self.ASIC_TUNNEL_TABLE, p2p_obj)

        if tc_to_dscp_map_oid:
            assert "SAI_TUNNEL_ATTR_ENCAP_QOS_TC_AND_COLOR_TO_DSCP_MAP" in fvs
        if tc_to_queue_map_oid:
            assert "SAI_TUNNEL_ATTR_ENCAP_QOS_TC_TO_QUEUE_MAP" in fvs
        
        for field, value in fvs.items():
            if field == "SAI_TUNNEL_ATTR_TYPE":
                assert value == "SAI_TUNNEL_TYPE_IPINIP"
            elif field == "SAI_TUNNEL_ATTR_ENCAP_SRC_IP":
                assert value == self.SELF_IPV4
            elif field == "SAI_TUNNEL_ATTR_ENCAP_DST_IP":
                assert value == self.PEER_IPV4
            elif field == "SAI_TUNNEL_ATTR_PEER_MODE":
                assert value == "SAI_TUNNEL_PEER_MODE_P2P"
            elif field == "SAI_TUNNEL_ATTR_OVERLAY_INTERFACE":
                assert self.check_interface_exists_in_asicdb(asicdb, value)
            elif field == "SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE":
                assert self.check_interface_exists_in_asicdb(asicdb, value)
            elif field == "SAI_TUNNEL_ATTR_ENCAP_TTL_MODE":
                assert value == "SAI_TUNNEL_TTL_MODE_PIPE_MODEL"
            elif field == "SAI_TUNNEL_ATTR_LOOPBACK_PACKET_ACTION":
                assert value == "SAI_PACKET_ACTION_DROP"
            elif field == "SAI_TUNNEL_ATTR_ENCAP_QOS_TC_AND_COLOR_TO_DSCP_MAP":
                assert value == tc_to_dscp_map_oid
            elif field == "SAI_TUNNEL_ATTR_ENCAP_QOS_TC_TO_QUEUE_MAP":
                assert value == tc_to_queue_map_oid
            elif field == "SAI_TUNNEL_ATTR_ENCAP_DSCP_MODE":
                assert value == "SAI_TUNNEL_DSCP_MODE_PIPE_MODEL"
            else:
                assert False, "Field %s is not tested" % field

    def check_tunnel_termination_entry_exists_in_asicdb(self, asicdb, tunnel_sai_oid, dst_ips, src_ip=None):
        tunnel_term_entries = asicdb.wait_for_n_keys(self.ASIC_TUNNEL_TERM_ENTRIES, len(dst_ips))
        expected_term_type = "SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2P" if src_ip else "SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP"
        expected_len = 6 if src_ip else 5
        for term_entry in tunnel_term_entries:
            fvs = asicdb.get_entry(self.ASIC_TUNNEL_TERM_ENTRIES, term_entry)

            assert len(fvs) == expected_len

            for field, value in fvs.items():
                if field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID":
                    assert self.check_vr_exists_in_asicdb(asicdb, value)
                elif field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE":
                    assert value == expected_term_type
                elif field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE":
                    assert value == "SAI_TUNNEL_TYPE_IPINIP"
                elif field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID":
                    assert value == tunnel_sai_oid
                elif field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP":
                    assert value in dst_ips
                elif field == "SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_SRC_IP" and src_ip:
                    assert value == src_ip
                else:
                    assert False, "Field %s is not tested" % field

    def create_and_test_tunnel(self, db, asicdb, tunnel_name, tunnel_param):
        """ Create tunnel and verify all needed enties in ASIC DB exists """

        is_symmetric_tunnel = "src_ip" in tunnel_param

        # 6 parameters to check in case of decap tunnel
        # + 1 (SAI_TUNNEL_ATTR_ENCAP_SRC_IP) in case of symmetric tunnel
        expected_len = 7 if is_symmetric_tunnel else 6

        if 'decap_tc_to_pg_map_id' in tunnel_param:
            expected_len += 1

        if 'decap_dscp_to_tc_map_id' in tunnel_param:
            expected_len += 1

        # check asic db table
        tunnels = asicdb.wait_for_n_keys(self.ASIC_TUNNEL_TABLE, 1)

        tunnel_sai_obj = tunnels[0]

        fvs = asicdb.wait_for_entry(self.ASIC_TUNNEL_TABLE, tunnel_sai_obj)

        assert len(fvs) == expected_len

        expected_ecn_mode = self.ecn_modes_map[tunnel_param["ecn_mode"]]
        expected_dscp_mode = self.dscp_modes_map[tunnel_param["dscp_mode"]]
        expected_ttl_mode = self.ttl_modes_map[tunnel_param["ttl_mode"]]

        for field, value in fvs.items():
            if field == "SAI_TUNNEL_ATTR_TYPE":
                assert value == "SAI_TUNNEL_TYPE_IPINIP"
            elif field == "SAI_TUNNEL_ATTR_ENCAP_SRC_IP":
                assert value == tunnel_param["src_ip"]
            elif field == "SAI_TUNNEL_ATTR_DECAP_ECN_MODE":
                assert value == expected_ecn_mode
            elif field == "SAI_TUNNEL_ATTR_DECAP_TTL_MODE":
                assert value == expected_ttl_mode
            elif field == "SAI_TUNNEL_ATTR_DECAP_DSCP_MODE":
                assert value == expected_dscp_mode
            elif field == "SAI_TUNNEL_ATTR_OVERLAY_INTERFACE":
                assert self.check_interface_exists_in_asicdb(asicdb, value)
            elif field == "SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE":
                assert self.check_interface_exists_in_asicdb(asicdb, value)
            elif field == "SAI_TUNNEL_ATTR_DECAP_QOS_DSCP_TO_TC_MAP":
                assert value == tunnel_param["decap_dscp_to_tc_map_id"]
            elif field == "SAI_TUNNEL_ATTR_DECAP_QOS_TC_TO_PRIORITY_GROUP_MAP":
                assert value == tunnel_param["decap_tc_to_pg_map_id"]
            else:
                assert False, "Field %s is not tested" % field
        src_ip = tunnel_param['src_ip'] if 'src_ip' in tunnel_param else None
        self.check_tunnel_termination_entry_exists_in_asicdb(
            asicdb, tunnel_sai_obj, tunnel_param["dst_ip"].split(","), src_ip)

    def remove_and_test_tunnel(self, db, asicdb, tunnel_name):
        """ Removes tunnel and checks that ASIC db is clear"""

        tunnel_table = swsscommon.Table(asicdb, self.ASIC_TUNNEL_TABLE)
        tunnel_term_table = swsscommon.Table(asicdb, self.ASIC_TUNNEL_TERM_ENTRIES)
        tunnel_app_table = swsscommon.Table(asicdb, self.APP_TUNNEL_DECAP_TABLE_NAME)

        tunnels = tunnel_table.getKeys()
        tunnel_sai_obj = tunnels[0]

        status, fvs = tunnel_table.get(tunnel_sai_obj)

        # get overlay loopback interface oid to check if it is deleted with the tunnel
        overlay_infs_id = {f:v for f, v in fvs}["SAI_TUNNEL_ATTR_OVERLAY_INTERFACE"]

        ps = swsscommon.ProducerStateTable(db, self.APP_TUNNEL_DECAP_TABLE_NAME)
        ps.set(tunnel_name, create_fvs(), 'DEL')

        # wait till config will be applied
        time.sleep(1)

        assert len(tunnel_table.getKeys()) == 0
        assert len(tunnel_term_table.getKeys()) == 0
        assert len(tunnel_app_table.getKeys()) == 0
        assert not self.check_interface_exists_in_asicdb(asicdb, overlay_infs_id)

    def add_qos_map(self, configdb, asicdb, qos_map_type_name, qos_map_name, qos_map):
        current_oids = asicdb.get_keys(self.ASIC_QOS_MAP_TABLE_KEY)
        # Apply QoS map to config db
        table = swsscommon.Table(configdb.db_connection, qos_map_type_name)
        fvs = swsscommon.FieldValuePairs(list(qos_map.items()))
        table.set(qos_map_name, fvs)
        time.sleep(1)

        diff = set(asicdb.get_keys(self.ASIC_QOS_MAP_TABLE_KEY)) - set(current_oids)
        assert len(diff) == 1
        oid = diff.pop()
        return oid

    def remove_qos_map(self, configdb, qos_map_type_name, qos_map_oid):
        """ Remove the testing qos map"""
        table = swsscommon.Table(configdb.db_connection, qos_map_type_name)
        table._del(qos_map_oid)
    def check_app_db_neigh_table(
            self, appdb, intf, neigh_ip,
            mac="00:00:00:00:00:00", expect_entry=True
        ):
        key = "{}:{}".format(intf, neigh_ip)
        if isinstance(ip_address(neigh_ip), IPv4Address):
            family = 'IPv4'
        else:
            family = 'IPv6'

        if expect_entry:
            appdb.wait_for_matching_keys(self.APP_NEIGH_TABLE, [key])
            appdb.wait_for_field_match(self.APP_NEIGH_TABLE, key, {'family': family})
            appdb.wait_for_field_match(self.APP_NEIGH_TABLE, key, {'neigh': mac})
        else:
            appdb.wait_for_deleted_keys(self.APP_NEIGH_TABLE, key)

    def cleanup_left_over(self, db, asicdb):
        """ Cleanup APP and ASIC tables """

        tunnel_table = asicdb.get_keys(self.ASIC_TUNNEL_TABLE)
        for key in tunnel_table:
            asicdb.delete_entry(self.ASIC_TUNNEL_TABLE, key)

        tunnel_term_table = asicdb.get_keys(self.ASIC_TUNNEL_TERM_ENTRIES)
        for key in tunnel_term_table:
            asicdb.delete_entry(self.ASIC_TUNNEL_TERM_ENTRIES, key)

        tunnel_app_table = swsscommon.Table(db, self.APP_TUNNEL_DECAP_TABLE_NAME)
        for key in tunnel_app_table.getKeys():
            tunnel_table._del(key)

    def ping_ip(self, dvs, ip):
        dvs.runcmd(self.PING_CMD.format(ip=ip))

    def check_neighbor_state(
            self, dvs, dvs_route, neigh_ip, expect_route=True,
            expect_neigh=False, expected_mac='00:00:00:00:00:00'
        ):
        """
        Checks the status of neighbor entries in APPL and ASIC DB
        """
        if expect_route and expect_neigh:
            pytest.fail('expect_routes and expect_neigh cannot both be True')
        app_db = dvs.get_app_db()
        asic_db = dvs.get_asic_db()
        prefix = str(ip_network(neigh_ip))
        self.check_app_db_neigh_table(
            app_db, self.VLAN_1000, neigh_ip,
            mac=expected_mac, expect_entry=expect_route
        )
        if expect_route:
            self.check_tnl_nexthop_in_asic_db(asic_db)
            routes = dvs_route.check_asicdb_route_entries([prefix])
            for route in routes:
                self.check_nexthop_in_asic_db(asic_db, route, standby=expect_route)
        else:
            dvs_route.check_asicdb_deleted_route_entries([prefix])
            self.check_neigh_in_asic_db(asic_db, neigh_ip, expected=expect_neigh)

    def execute_action(self, action, dvs, test_info):
        if action in (PING_SERV, PING_NEIGH):
            self.ping_ip(dvs, test_info[IP])
        elif action in (ACTIVE, STANDBY):
            app_db_connector = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
            self.set_mux_state(app_db_connector, test_info[INTF], action)
        elif action == RESOLVE_ENTRY:
            self.add_neighbor(dvs, test_info[IP], test_info[MAC])
        elif action == DELETE_ENTRY:
            self.del_neighbor(dvs, test_info[IP])
        else:
            pytest.fail('Invalid test action {}'.format(action))

    @pytest.fixture(scope='module')
    def setup_vlan(self, dvs):
        self.create_vlan_interface(dvs)

    @pytest.fixture(scope='module')
    def setup_mux_cable(self, dvs):
        config_db = dvs.get_config_db()
        self.create_mux_cable(config_db)

    @pytest.fixture(scope='module')
    def setup_qos_map(self, dvs):
        db = dvs.get_config_db()
        asicdb = dvs.get_asic_db()

        tc_to_dscp_map_oid = self.add_qos_map(db, asicdb, swsscommon.CFG_TC_TO_DSCP_MAP_TABLE_NAME, self.TUNNEL_QOS_MAP_NAME, self.TC_TO_DSCP_MAP)
        tc_to_queue_map_oid = self.add_qos_map(db, asicdb, swsscommon.CFG_TC_TO_QUEUE_MAP_TABLE_NAME, self.TUNNEL_QOS_MAP_NAME, self.TC_TO_QUEUE_MAP)

        dscp_to_tc_map_oid = self.add_qos_map(db, asicdb, swsscommon.CFG_DSCP_TO_TC_MAP_TABLE_NAME, self.TUNNEL_QOS_MAP_NAME, self.DSCP_TO_TC_MAP)
        tc_to_pg_map_oid = self.add_qos_map(db, asicdb, swsscommon.CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME, self.TUNNEL_QOS_MAP_NAME, self.TC_TO_PRIORITY_GROUP_MAP)

        yield tc_to_dscp_map_oid, tc_to_queue_map_oid, dscp_to_tc_map_oid, tc_to_pg_map_oid

        self.remove_qos_map(db, swsscommon.CFG_TC_TO_DSCP_MAP_TABLE_NAME, tc_to_dscp_map_oid)
        self.remove_qos_map(db, swsscommon.CFG_TC_TO_QUEUE_MAP_TABLE_NAME, tc_to_queue_map_oid)
        self.remove_qos_map(db, swsscommon.CFG_DSCP_TO_TC_MAP_TABLE_NAME, dscp_to_tc_map_oid)
        self.remove_qos_map(db, swsscommon.CFG_TC_TO_PRIORITY_GROUP_MAP_TABLE_NAME, tc_to_pg_map_oid)

    @pytest.fixture(scope='module')
    def setup_tunnel(self, dvs):
        app_db_connector = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        ps = swsscommon.ProducerStateTable(app_db_connector, self.APP_TUNNEL_DECAP_TABLE_NAME)
        fvs = create_fvs(**self.DEFAULT_TUNNEL_PARAMS)
        ps.set(self.MUX_TUNNEL_0, fvs)

    @pytest.fixture
    def setup_peer_switch(self, dvs):
        config_db = dvs.get_config_db()
        config_db.create_entry(
            self.CONFIG_PEER_SWITCH,
            self.PEER_SWITCH_HOST,
            self.DEFAULT_PEER_SWITCH_PARAMS
        )

    @pytest.fixture
    def remove_peer_switch(self, dvs):
        config_db = dvs.get_config_db()
        config_db.delete_entry(self.CONFIG_PEER_SWITCH, self.PEER_SWITCH_HOST)

    @pytest.fixture(params=['IPv4', 'IPv6'])
    def ip_version(self, request):
        return request.param

    def clear_neighbors(self, dvs):
        _, neighs_str = dvs.runcmd('ip neigh show all')
        neighs = [entry.split()[0] for entry in neighs_str.split('\n')[:-1]]

        for neigh in neighs:
            self.del_neighbor(dvs, neigh)

    @pytest.fixture
    def neighbor_cleanup(self, dvs):
        """
        Ensures that all kernel neighbors are removed before and after tests
        """
        self.clear_neighbors(dvs)
        yield
        self.clear_neighbors(dvs)

    @pytest.fixture
    def server_test_ips(self, ip_version):
        if ip_version == 'IPv4':
            return [self.SERV1_IPV4, self.SERV2_IPV4, self.SERV3_IPV4]
        else:
            return [self.SERV1_IPV6, self.SERV2_IPV6, self.SERV3_IPV6]

    @pytest.fixture
    def neigh_test_ips(self, ip_version):
        if ip_version == 'IPv4':
            return [self.NEIGH1_IPV4, self.NEIGH2_IPV4, self.NEIGH3_IPV4]
        else:
            return [self.NEIGH1_IPV6, self.NEIGH2_IPV6, self.NEIGH3_IPV6]

    @pytest.fixture
    def ips_for_test(self, server_test_ips, neigh_test_ips, neigh_miss_test_sequence):
        # Assumes that each test sequence has at exactly one of
        # PING_NEIGH OR PING_SERV as a step
        for step in neigh_miss_test_sequence:
            if step[TEST_ACTION] == PING_SERV:
                return server_test_ips
            if step[TEST_ACTION] == PING_NEIGH:
                return neigh_test_ips

        # If we got here, the test sequence did not contain a ping command
        pytest.fail('No ping command found in test sequence {}'.format(neigh_miss_test_sequence))

    @pytest.fixture
    def ip_to_intf_map(self, server_test_ips, neigh_test_ips):
        map = {
            server_test_ips[0]: 'Ethernet0',
            server_test_ips[1]: 'Ethernet4',
            server_test_ips[2]: 'Ethernet8',
            neigh_test_ips[0]: 'Ethernet0',
            neigh_test_ips[1]: 'Ethernet4',
            neigh_test_ips[2]: 'Ethernet8'
        }
        return map

    @pytest.fixture(
        params=NEIGH_MISS_TESTS,
        ids=['->'.join([step[TEST_ACTION] for step in scenario])
             for scenario in NEIGH_MISS_TESTS]
    )
    def neigh_miss_test_sequence(self, request):
        return request.param

    @pytest.fixture
    def intf_fdb_map(self, dvs, setup_vlan):
        """
        Note: this fixture invokes the setup_vlan fixture so that
        the interfaces are brought up before attempting to access FDB information
        """
        state_db = dvs.get_state_db()
        keys = state_db.get_keys(self.STATE_FDB_TABLE)

        fdb_map = {}
        for key in keys:
            entry = state_db.get_entry(self.STATE_FDB_TABLE, key)
            mac = key.replace('{}:'.format(self.VLAN_1000), '')
            port = entry['port']
            fdb_map[port] = mac

        return fdb_map


class TestMuxTunnel(TestMuxTunnelBase):
    """ Tests for Mux tunnel creation and removal """
    def test_Tunnel(self, dvs, setup_tunnel, setup_qos_map, testlog):
        """ test IPv4 Mux tunnel creation """

        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        #self.cleanup_left_over(db, asicdb)
        _, _, dscp_to_tc_map_oid, tc_to_pg_map_oid = setup_qos_map
        test_param = self.DEFAULT_TUNNEL_PARAMS.copy()
        test_param.update(
            {
                "decap_dscp_to_tc_map_id": dscp_to_tc_map_oid,
                "decap_tc_to_pg_map_id": tc_to_pg_map_oid
            }
        )
        # create tunnel IPv4 tunnel
        self.create_and_test_tunnel(db, asicdb, self.MUX_TUNNEL_0, test_param)

    def test_Peer(self, dvs, setup_peer_switch, setup_tunnel, setup_qos_map, testlog):
        """ test IPv4 Mux tunnel creation """
        
        db = dvs.get_config_db()
        asicdb = dvs.get_asic_db()

        encap_tc_to_dscp_map_id, encap_tc_to_queue_map_id, _, _ = setup_qos_map

        self.create_and_test_peer(db, asicdb, "peer", "1.1.1.1", "10.1.0.32", encap_tc_to_dscp_map_id, encap_tc_to_queue_map_id)

    def test_Neighbor(self, dvs, dvs_route, testlog):
        """ test Neighbor entries and mux state change """

        confdb = dvs.get_config_db()
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        self.create_and_test_neighbor(confdb, appdb, asicdb, dvs, dvs_route)

    def test_Fdb(self, dvs, dvs_route, testlog):
        """ test Fdb entries and mux state change """

        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        self.create_and_test_fdb(appdb, asicdb, dvs, dvs_route)

    def test_Route(self, dvs, dvs_route, testlog):
        """ test Route entries and mux state change """

        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        self.create_and_test_route(appdb, asicdb, dvs, dvs_route)

    def test_acl(self, dvs, dvs_acl, testlog):
        """ test acl and mux state change """

        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

        self.create_and_test_acl(appdb, dvs_acl)

    def test_mux_metrics(self, dvs, testlog):
        """ test metrics for mux state change """

        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        statedb = dvs.get_state_db()

        self.create_and_test_metrics(appdb, statedb)

    def test_neighbor_miss(
            self, dvs, dvs_route, ips_for_test, neigh_miss_test_sequence,
            ip_to_intf_map, intf_fdb_map, neighbor_cleanup, setup_vlan,
            setup_mux_cable, setup_tunnel, setup_peer_switch, testlog
    ):
        ip = ips_for_test[0]
        intf = ip_to_intf_map[ip]
        mac = intf_fdb_map[intf]
        test_info = {
            IP: ip,
            INTF: intf,
            MAC: mac
        }

        for step in neigh_miss_test_sequence:
            self.execute_action(step[TEST_ACTION], dvs, test_info)
            exp_result = step[EXPECTED_RESULT]
            self.check_neighbor_state(
                dvs, dvs_route, ip,
                expect_route=exp_result[EXPECT_ROUTE],
                expect_neigh=exp_result[EXPECT_NEIGH],
                expected_mac=mac if exp_result[REAL_MAC] else '00:00:00:00:00:00'
            )

    def test_neighbor_miss_no_peer(
            self, dvs, dvs_route, setup_vlan, setup_mux_cable, setup_tunnel,
            remove_peer_switch, neighbor_cleanup, testlog
    ):
        """
        test neighbor miss with no peer switch configured
        No new entries are expected in APPL_DB or ASIC_DB
        """
        test_ips = [self.NEIGH3_IPV4, self.SERV3_IPV4, self.NEIGH1_IPV6, self.SERV1_IPV6]

        for ip in test_ips:
            self.ping_ip(dvs, ip)

        for ip in test_ips:
            self.check_neighbor_state(dvs, dvs_route, ip, expect_route=False)



# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
