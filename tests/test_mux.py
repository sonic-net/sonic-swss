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
    APP_ROUTE_TABLE             = "ROUTE_TABLE"
    APP_TUNNEL_DECAP_TABLE_NAME = "TUNNEL_DECAP_TABLE"
    APP_TUNNEL_ROUTE_TABLE_NAME = "TUNNEL_ROUTE_TABLE"
    ASIC_TUNNEL_TABLE           = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL"
    ASIC_TUNNEL_TERM_ENTRIES    = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY"
    ASIC_RIF_TABLE              = "ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE"
    ASIC_VRF_TABLE              = "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"
    ASIC_NEIGH_TABLE            = "ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY"
    ASIC_NEXTHOP_TABLE          = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP"
    ASIC_NHG_MEMBER_TABLE       = "ASIC_STATE:SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER"
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
    SERV1_SOC_IPV4              = "192.168.0.103"
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

    DEFAULT_TUNNEL_PARAMS = {
        "tunnel_type": "IPINIP",
        "dst_ip": SELF_IPV4,
        "dscp_mode": "pipe",
        "ecn_mode": "standard",
        "ttl_mode": "pipe",
        "encap_tc_to_queue_map": TUNNEL_QOS_MAP_NAME,
        "encap_tc_to_dscp_map": TUNNEL_QOS_MAP_NAME,
        "decap_dscp_to_tc_map": TUNNEL_QOS_MAP_NAME,
        "decap_tc_to_pg_map": TUNNEL_QOS_MAP_NAME
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
    
    TC_TO_DSCP_MAP = {str(i):str(i) for i in range(0, 8)}
    TC_TO_QUEUE_MAP = {str(i):str(i) for i in range(0, 8)}
    DSCP_TO_TC_MAP = {str(i):str(1) for i in range(0, 64)}
    TC_TO_PRIORITY_GROUP_MAP = {str(i):str(i) for i in range(0, 8)}

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

        dvs.port_admin_set("Ethernet0", "up")
        dvs.port_admin_set("Ethernet4", "up")
        dvs.port_admin_set("Ethernet8", "up")

    def create_mux_cable(self, confdb):
        fvs = {
            "server_ipv4":self.SERV1_IPV4 + self.IPV4_MASK,
            "server_ipv6":self.SERV1_IPV6 + self.IPV6_MASK,
            "soc_ipv4": self.SERV1_SOC_IPV4 + self.IPV4_MASK,
            "cable_type": "active-active" # "cable_type" is not used by orchagent, this is a dummy value
        }
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
    
    def get_nexthop_oid(self, asicdb, nexthop):
        # gets nexthop oid
        nexthop_keys = asicdb.get_keys(self.ASIC_NEXTHOP_TABLE)

        nexthop_oid = ''
        for nexthop_key in nexthop_keys:
            entry = asicdb.get_entry(self.ASIC_NEXTHOP_TABLE, nexthop_key)
            if entry["SAI_NEXT_HOP_ATTR_IP"] == nexthop:
                nexthop_oid = nexthop_key
                break

        return nexthop_oid
    
    def get_route_nexthop_oid(self, route_key, asicdb):
        # gets nexthop oid
        entry = asicdb.get_entry(self.ASIC_ROUTE_TABLE, route_key)
        assert 'SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID' in entry

        return entry['SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID']

    def check_tunnel_route_in_app_db(self, dvs, destinations, expected=True):
        appdb = dvs.get_app_db()

        if expected:
            appdb.wait_for_matching_keys(self.APP_TUNNEL_ROUTE_TABLE_NAME, destinations)
        else:
            appdb.wait_for_deleted_keys(self.APP_TUNNEL_ROUTE_TABLE_NAME, destinations)

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

    def check_route_nexthop(self, dvs_route, asicdb, route, nexthop, tunnel=False):
        route_key = dvs_route.check_asicdb_route_entries([route])
        route_nexthop_oid = self.get_route_nexthop_oid(route_key[0], asicdb)
        
        if tunnel:
            assert route_nexthop_oid == nexthop
            return

        nexthop_oid = self.get_nexthop_oid(asicdb, nexthop)

        assert route_nexthop_oid == nexthop_oid

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

    def add_route(self, dvs, route, nexthops, ifaces=[]):
        apdb = dvs.get_app_db()
        if len(nexthops) > 1:
            nexthop_str = ",".join(nexthops)
            if len(ifaces) == 0:
                ifaces = [self.VLAN_1000 for k in range(len(nexthops))]
            iface_str = ",".join(ifaces)
        else:
            nexthop_str = str(nexthops[0])
            if len(ifaces) == 0:
                iface_str = self.VLAN_1000
            else:
                iface_str = ifaces[0]

        ps = swsscommon.ProducerStateTable(apdb.db_connection, self.APP_ROUTE_TABLE)
        fvs = swsscommon.FieldValuePairs(
                [
                    ("nexthop", nexthop_str),
                    ("ifname", iface_str)
                ]
              )
        ps.set(route, fvs)

    def del_route(self, dvs, route):
        apdb = dvs.get_app_db()
        ps = swsscommon.ProducerStateTable(apdb.db_connection, self.APP_ROUTE_TABLE)
        ps._del(route)

    def create_and_test_neighbor(self, confdb, appdb, asicdb, dvs, dvs_route):

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

    def create_and_test_soc(self, appdb, asicdb, dvs, dvs_route):

        self.set_mux_state(appdb, "Ethernet0", "active")

        self.add_fdb(dvs, "Ethernet0", "00-00-00-00-00-01")
        self.add_neighbor(dvs, self.SERV1_SOC_IPV4, "00:00:00:00:00:01")

        time.sleep(1)

        srv1_soc_v4 = self.check_neigh_in_asic_db(asicdb, self.SERV1_SOC_IPV4)
        self.check_tunnel_route_in_app_db(dvs, [self.SERV1_SOC_IPV4+self.IPV4_MASK], expected=False)

        self.set_mux_state(appdb, "Ethernet0", "standby")

        asicdb.wait_for_deleted_entry(self.ASIC_NEIGH_TABLE, srv1_soc_v4)
        dvs_route.check_asicdb_route_entries(
            [self.SERV1_SOC_IPV4+self.IPV4_MASK]
        )
        self.check_tunnel_route_in_app_db(dvs, [self.SERV1_SOC_IPV4+self.IPV4_MASK], expected=False)

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

    def multi_nexthop_test(self, dvs, dvs_route, asicdb, appdb, route, neighbors, macs):
        mux_ports = ["Ethernet0", "Ethernet4"]
        starting_states = [(ACTIVE, ACTIVE), (ACTIVE, STANDBY), (STANDBY, ACTIVE), (STANDBY, STANDBY)]

        # Set state to active for initial state
        for port in mux_ports:
            self.set_mux_state(appdb, port, "active")

        # add neighbors for initial state
        for i,neighbor in enumerate(neighbors):
            self.add_neighbor(dvs, neighbor, macs[i])
        
        try:
            # toggle between states and add route in various combos of state
            print("Testing add/remove/update of route")
            for start in starting_states:
                print("Adding route with %s: %s and %s: %s" % (mux_ports[0], start[0], mux_ports[1], start[1]))
                self.set_mux_state(appdb, mux_ports[0], start[0])
                self.set_mux_state(appdb, mux_ports[1], start[1])
                self.add_route(dvs, route, neighbors)
                if start[0] == "active":
                    self.check_route_nexthop(dvs_route, asicdb, route, neighbors[0])
                elif start[1] == "active":
                    self.check_route_nexthop(dvs_route, asicdb, route, neighbors[1])
                else:
                    self.check_route_nexthop(dvs_route, asicdb, route, tunnel_nh_id, True)

                print("Testing fdb update in %s, %s for %s" % (start[0], start[1], neighbors[0]))
                # move neighbor 1
                self.add_neighbor(dvs, neighbors[0], "00:aa:bb:cc:dd:ee")
                if start[0] == "active":
                    self.check_route_nexthop(dvs_route, asicdb, route, neighbors[0])
                elif start[1] == "active":
                    self.check_route_nexthop(dvs_route, asicdb, route, neighbors[1])
                else:
                    self.check_route_nexthop(dvs_route, asicdb, route, tunnel_nh_id, True)

                # move neighbor 1 back
                self.add_neighbor(dvs, neighbors[0], macs[0])

                print("Testing fdb update in %s, %s for %s" % (start[0], start[1], neighbors[1]))
                # move neighbor 2
                self.add_neighbor(dvs, neighbors[0], "00:aa:bb:cc:dd:ee")
                if start[0] == "active":
                    self.check_route_nexthop(dvs_route, asicdb, route, neighbors[0])
                elif start[1] == "active":
                    self.check_route_nexthop(dvs_route, asicdb, route, neighbors[1])
                else:
                    self.check_route_nexthop(dvs_route, asicdb, route, tunnel_nh_id, True)

                # move neighbor 2 back
                self.add_neighbor(dvs, neighbors[1], macs[1])

                self.del_route(dvs, route)


            # toggle mux states to check setState actions
            print("Testing toggling state")
            for start in starting_states:
                self.set_mux_state(appdb, mux_ports[0], start[0])
                self.set_mux_state(appdb, mux_ports[1], start[1])
                self.add_route(dvs, route, neighbors)

                for toggle_index,port in enumerate(mux_ports):
                    keep_index = (toggle_index + 1) % 2

                    print("keeping %s as %s while toggling %s from %s" % \
                          (mux_ports[keep_index], start[keep_index], mux_ports[toggle_index], start[toggle_index]))

                    if start[toggle_index] == ACTIVE:
                        print("setting %s to %s" % (mux_ports[toggle_index], STANDBY))
                        self.set_mux_state(appdb, mux_ports[toggle_index], STANDBY)
                        if start[keep_index] == ACTIVE:
                            self.check_route_nexthop(dvs_route, asicdb, route, neighbors[keep_index])
                        else:
                            self.check_route_nexthop(dvs_route, asicdb, route, tunnel_nh_id, True)

                        print("setting %s to %s" % (mux_ports[toggle_index], ACTIVE))
                        self.set_mux_state(appdb, mux_ports[toggle_index], ACTIVE)
                        if start[keep_index] == ACTIVE:
                            self.check_route_nexthop(dvs_route, asicdb, route, neighbors[0])
                        else:
                            self.check_route_nexthop(dvs_route, asicdb, route, neighbors[toggle_index])
                    else:
                        print("setting %s to %s" % (mux_ports[toggle_index], ACTIVE))
                        self.set_mux_state(appdb, mux_ports[toggle_index], ACTIVE)
                        if start[keep_index] == ACTIVE:
                            self.check_route_nexthop(dvs_route, asicdb, route, neighbors[0])
                        else:
                            self.check_route_nexthop(dvs_route, asicdb, route, neighbors[toggle_index])

                        print("setting %s to %s" % (mux_ports[toggle_index], STANDBY))
                        self.set_mux_state(appdb, mux_ports[toggle_index], STANDBY)
                        if start[keep_index] == ACTIVE:
                            self.check_route_nexthop(dvs_route, asicdb, route, neighbors[keep_index])
                        else:
                            self.check_route_nexthop(dvs_route, asicdb, route, tunnel_nh_id, True)
                self.del_route(dvs, route)

            # Check route_updates
            self.add_neighbor(dvs, self.SERV3_IPV4, macs[2])
            print("Testing route updates")
            self.add_route(dvs, route, neighbors)
            print("setting states to active")
            for port in mux_ports:
                self.set_mux_state(appdb, port, "active")
            self.set_mux_state(appdb, "Ethernet4", "active")

            print("triggering another route update")
            # add new neighbor to route to force route update
            self.add_route(dvs, route, [neighbors[0], self.SERV3_IPV4])
            self.check_route_nexthop(dvs_route, asicdb, route, neighbors[0])
            self.del_route(dvs,route)

            time.sleep(2)

            self.add_route(dvs, route, neighbors)
            print("setting states to standby")
            for port in mux_ports:
                self.set_mux_state(appdb, port, "standby")
            self.set_mux_state(appdb, "Ethernet4", "standby")

            print("triggering another route update")
            # add new neighbor to route to force route update
            self.add_route(dvs, route, [neighbors[0], self.SERV3_IPV4])
            self.add_route(dvs, route, neighbors)
            self.check_route_nexthop(dvs_route, asicdb, route, tunnel_nh_id, True)
            self.del_route(dvs,route)

            self.del_neighbor(dvs, self.SERV3_IPV4)

            print("Testing route update to single nexthop")
            for start in starting_states:
                self.set_mux_state(appdb, mux_ports[0], start[0])
                self.set_mux_state(appdb, mux_ports[1], start[1])
                self.add_route(dvs, route, neighbors)

                print("route update to single nexthop")
                # add new neighbor to route to force route update
                self.add_route(dvs, route, [neighbors[0]])
                if start[0] == ACTIVE:
                    self.check_route_nexthop(dvs_route, asicdb, route, neighbors[0])
                else:
                    self.check_route_nexthop(dvs_route, asicdb, route, tunnel_nh_id, True)

                print("change state of nexthop")
                if start[0] == ACTIVE:
                    self.set_mux_state(appdb, mux_ports[0], STANDBY)
                    self.check_route_nexthop(dvs_route, asicdb, route, tunnel_nh_id, True)
                else:
                    self.set_mux_state(appdb, mux_ports[0], ACTIVE)
                    self.check_route_nexthop(dvs_route, asicdb, route, neighbors[0])

                self.del_route(dvs,route)

            for neighbor in neighbors:
                self.del_neighbor(dvs, neighbor)

            print("Testing add/remove of neighbors")
            for start in starting_states:
                print("Testing add/remove of neighbors in %s, %s" % start)
                self.set_mux_state(appdb, mux_ports[0], start[0])
                self.set_mux_state(appdb, mux_ports[1], start[1])
                self.add_route(dvs, route, neighbors)

                # add first neighbor
                self.add_neighbor(dvs, neighbors[0], macs[0])
                if start[0] == ACTIVE:
                    self.check_route_nexthop(dvs_route, asicdb, route, neighbors[0])

                # add second neighbor
                self.add_neighbor(dvs, neighbors[1], macs[1])
                time.sleep(1)
                if start[0] == ACTIVE:
                    self.check_route_nexthop(dvs_route, asicdb, route, neighbors[0])
                elif start[1] == ACTIVE:
                    self.check_route_nexthop(dvs_route, asicdb, route, neighbors[1])
                else:
                    self.check_route_nexthop(dvs_route, asicdb, route, tunnel_nh_id, True)

                # remove neighbors (shouldn't work)
                for neighbor in neighbors:
                    self.del_neighbor(dvs, neighbor)
                    if start[0] == ACTIVE:
                        self.check_route_nexthop(dvs_route, asicdb, route, neighbors[0])
                    elif start[1] == ACTIVE:
                        self.check_route_nexthop(dvs_route, asicdb, route, neighbors[1])
                    else:
                        self.check_route_nexthop(dvs_route, asicdb, route, tunnel_nh_id, True)

                # add neighbor again to trick ip
                for i,neighbor in enumerate(neighbors):
                    self.add_neighbor(dvs, neighbor, macs[i])

                self.del_route(dvs,route)
                for neighbor in neighbors:
                    self.del_neighbor(dvs, neighbor)

            # add the neighbors
            for i,neighbor in enumerate(neighbors):
                self.add_neighbor(dvs, neighbor, macs[i])

            print("Testing multiple routes pointing to one of the NH")
            r2 = "3.4.5.0/24"
            for start in starting_states:
                print("Adding routes with %s: %s and %s: %s" % (mux_ports[0], start[0], mux_ports[1], start[1]))

                self.set_mux_state(appdb, mux_ports[0], start[0])
                self.set_mux_state(appdb, mux_ports[1], start[1])
                self.add_route(dvs, route, neighbors)

                # add R2 -> NH1
                # R2 should behave like a normal route.
                self.add_route(dvs, r2, [neighbors[0]])
                if start[0] == "active":
                    self.check_route_nexthop(dvs_route, asicdb, r2, neighbors[0])
                else:
                    self.check_route_nexthop(dvs_route, asicdb, r2, tunnel_nh_id, True)

                self.del_route(dvs,route)
                self.del_route(dvs,r2)

            # remove the neighbors
            for neighbor in neighbors:
                self.del_neighbor(dvs, neighbor)

            # add one neighbor as MUX other as standalone
            self.add_neighbor(dvs, neighbors[0], macs[i])
            self.add_neighbor(dvs, neighbors[1], "00:00:00:00:00:00")

            print("Testing one mux, one standalone neighbor")
            for start in starting_states:
                print("Testing add/remove of neighbors in %s, %s" % start)
                self.set_mux_state(appdb, mux_ports[0], start[0])
                self.set_mux_state(appdb, mux_ports[1], start[1])
                self.add_route(dvs, route, neighbors)

                if start[0] == "active":
                    self.check_route_nexthop(dvs_route, asicdb, route, neighbors[0])
                else:
                    # N2 should always be standby
                    self.check_route_nexthop(dvs_route, asicdb, route, tunnel_nh_id, True)

                self.del_route(dvs,route)

            # remove the neighbors
            for neighbor in neighbors:
                self.del_neighbor(dvs, neighbor)

        finally:
            # Cleanup
            for port in mux_ports:
                self.set_mux_state(appdb, port, "active")
            self.del_route(dvs,route)
            for neighbor in neighbors:
                self.del_neighbor(dvs, neighbor)

    def create_and_test_multi_nexthop_routes(self, dvs, dvs_route, appdb, macs, asicdb):
        '''
        Tests case where there are multiple nexthops tied to a route
        If the nexthops are tied to a mux, then only the first active neighbor will be programmed
        If not, the route should point to a regular ECMP group
        '''

        route_ipv4 = "2.3.4.0/24"
        route_ipv6 = "2023::/64"
        ipv4_neighbors = [self.SERV1_IPV4, self.SERV2_IPV4]
        ipv6_neighbors = [self.SERV1_IPV6, self.SERV2_IPV6]

        self.multi_nexthop_test(dvs, dvs_route, asicdb, appdb, route_ipv4, ipv4_neighbors, macs)
        self.multi_nexthop_test(dvs, dvs_route, asicdb, appdb, route_ipv6, ipv6_neighbors, macs)

        try:
            # neighbor not tied to mux cable case
            non_mux_ipv4 = ["11.11.11.11", "12.12.12.12"]
            non_mux_ipv6 = ["2222::100", "2222::101"]
            non_mux_macs = ["00:aa:bb:cc:dd:ee", "00:aa:bb:cc:dd:ff"]
            print("Testing neighbors that are not tied to a mux cable")

            for i in range(2):
                self.add_neighbor(dvs, non_mux_ipv4[i], non_mux_macs[i])
                self.add_neighbor(dvs, non_mux_ipv6[i], non_mux_macs[i])

            self.add_route(dvs, route_ipv4, non_mux_ipv4)
            self.add_route(dvs, route_ipv6, non_mux_ipv6)

            # Check for route pointing to first neighbor
            self.check_route_nexthop(dvs_route, asicdb, route_ipv4, non_mux_ipv4[0])
            self.check_route_nexthop(dvs_route, asicdb, route_ipv6, non_mux_ipv6[0])

            # Cleanup
            self.del_route(dvs, route_ipv4)
            self.del_route(dvs, route_ipv6)
            for i in range(2):
                self.del_neighbor(dvs, non_mux_ipv4[i])
                self.del_neighbor(dvs, non_mux_ipv6[i])

            # neighbor not in mux cable case
            non_mux_ipv4 = ["11.11.11.11", "12.12.12.12"]
            non_mux_ipv6 = ["2222::100", "2222::101"]
            non_mux_macs = ["00:aa:bb:cc:dd:ee", "00:aa:bb:cc:dd:ff"]
            print("Testing neighbors that are not tied to a mux cable")
            for i in range(2):
                self.add_neighbor(dvs, non_mux_ipv4[i], non_mux_macs[i])
                self.add_neighbor(dvs, non_mux_ipv6[i], non_mux_macs[i])

            self.add_route(dvs, route_ipv4, non_mux_ipv4)
            self.add_route(dvs, route_ipv6, non_mux_ipv6)

            # Check for route pointing to first neighbor
            self.check_route_nexthop(dvs_route, asicdb, route_ipv4, non_mux_ipv4[0])
            self.check_route_nexthop(dvs_route, asicdb, route_ipv6, non_mux_ipv6[0])

            # Cleanup
            self.del_route(dvs, route_ipv4)
            self.del_route(dvs, route_ipv6)
            for i in range(2):
                self.del_neighbor(dvs, non_mux_ipv4[i])
                self.del_neighbor(dvs, non_mux_ipv6[i])


            # add one neighbor as MUX other as non-mux
            mux_ports = ["Ethernet0", "Ethernet4"]
            self.set_mux_state(appdb, mux_ports[0], ACTIVE)
            self.set_mux_state(appdb, mux_ports[1], ACTIVE)
            self.add_neighbor(dvs, ipv4_neighbors[0], macs[0])
            self.add_neighbor(dvs, non_mux_ipv4[0], "00:aa:bb:cc:dd:ee")

            print("Testing one mux, one standalone neighbor ipv4")
            starting_states = [(ACTIVE, ACTIVE), (ACTIVE, STANDBY), (STANDBY, ACTIVE), (STANDBY, STANDBY)]
            for start in starting_states:
                print("Testing add/remove of neighbors in %s, %s" % start)
                self.set_mux_state(appdb, mux_ports[0], start[0])
                self.set_mux_state(appdb, mux_ports[1], start[1])
                self.add_route(dvs, route_ipv4, [ipv4_neighbors[0], non_mux_ipv4[0]])

                #N2 should always be active
                self.check_route_nexthop(dvs_route, asicdb, route_ipv4, non_mux_ipv4[0])

                self.del_route(dvs,route_ipv4)

            self.del_neighbor(dvs, ipv4_neighbors[0])
            self.del_neighbor(dvs, non_mux_ipv4[0])

            # add one neighbor as MUX other as non-mux for ipv6
            self.set_mux_state(appdb, mux_ports[0], ACTIVE)
            self.set_mux_state(appdb, mux_ports[1], ACTIVE)
            self.add_neighbor(dvs, ipv6_neighbors[0], macs[0])
            self.add_neighbor(dvs, non_mux_ipv6[0], "00:aa:bb:cc:dd:ee")

            print("Testing one mux, one standalone neighbor ipv6")
            mux_ports = ["Ethernet0", "Ethernet4"]
            starting_states = [(ACTIVE, ACTIVE), (ACTIVE, STANDBY), (STANDBY, ACTIVE), (STANDBY, STANDBY)]
            for start in starting_states:
                print("Testing add/remove of neighbors in %s, %s" % start)
                self.set_mux_state(appdb, mux_ports[0], start[0])
                self.set_mux_state(appdb, mux_ports[1], start[1])
                self.add_route(dvs, route_ipv6, [ipv6_neighbors[0], non_mux_ipv6[0]])

                #N2 should always be active
                self.check_route_nexthop(dvs_route, asicdb, route_ipv6, non_mux_ipv6[0])

                self.del_route(dvs,route_ipv6)

            self.del_neighbor(dvs, ipv6_neighbors[0])
            self.del_neighbor(dvs, non_mux_ipv6[0])

        finally:
            # Cleanup
            self.del_route(dvs, route_ipv4)
            self.del_route(dvs, route_ipv6)
            for i in range(2):
                self.del_neighbor(dvs, non_mux_ipv4[i])
                self.del_neighbor(dvs, non_mux_ipv6[i])

    def create_and_test_NH_routes(self, appdb, asicdb, dvs, dvs_route, mac):
        '''
        Tests case where neighbor is removed in standby and added in active with route
        '''
        nh_route = "2.2.2.0/24"
        nh_route_ipv6 = "2023::/64"
        neigh_ip = self.SERV1_IPV4
        neigh_ipv6 = self.SERV1_IPV6
        apdb = dvs.get_app_db()

        # Setup
        self.set_mux_state(appdb, "Ethernet0", "active")
        self.add_neighbor(dvs, neigh_ip, mac)
        self.add_neighbor(dvs, neigh_ipv6, mac)
        dvs.runcmd(
            "vtysh -c \"configure terminal\" -c \"ip route " + nh_route +
            " " + neigh_ip + "\""
        )
        dvs.runcmd(
            "vtysh -c \"configure terminal\" -c \"ipv6 route " + nh_route_ipv6 +
            " " + neigh_ipv6 + "\""
        )
        apdb.wait_for_entry("ROUTE_TABLE", nh_route)
        apdb.wait_for_entry("ROUTE_TABLE", nh_route_ipv6)

        rtkeys = dvs_route.check_asicdb_route_entries([nh_route])
        rtkeys_ipv6 = dvs_route.check_asicdb_route_entries([nh_route_ipv6])
        self.check_nexthop_in_asic_db(asicdb, rtkeys[0])
        self.check_nexthop_in_asic_db(asicdb, rtkeys_ipv6[0])

        # Set state to standby and delete neighbor
        self.set_mux_state(appdb, "Ethernet0", "standby")
        self.check_nexthop_in_asic_db(asicdb, rtkeys[0], True)
        self.check_nexthop_in_asic_db(asicdb, rtkeys_ipv6[0], True)

        self.del_neighbor(dvs, neigh_ip)
        self.del_neighbor(dvs, neigh_ipv6)
        apdb.wait_for_deleted_entry(self.APP_NEIGH_TABLE, neigh_ip)
        apdb.wait_for_deleted_entry(self.APP_NEIGH_TABLE, neigh_ipv6)
        asicdb.wait_for_deleted_entry(self.ASIC_NEIGH_TABLE, neigh_ip)
        asicdb.wait_for_deleted_entry(self.ASIC_NEIGH_TABLE, neigh_ip)

        self.check_nexthop_in_asic_db(asicdb, rtkeys[0], True)
        self.check_nexthop_in_asic_db(asicdb, rtkeys_ipv6[0], True)

        # Set state to active, learn neighbor again
        self.set_mux_state(appdb, "Ethernet0", "active")

        self.add_neighbor(dvs, neigh_ip, mac)
        self.add_neighbor(dvs, neigh_ipv6, mac)
        self.check_neigh_in_asic_db(asicdb, neigh_ip)
        self.check_neigh_in_asic_db(asicdb, neigh_ipv6)

        self.check_nexthop_in_asic_db(asicdb, rtkeys[0])
        self.check_nexthop_in_asic_db(asicdb, rtkeys_ipv6[0])
        dvs.runcmd(
            "ip neigh flush " + neigh_ip
        )
        dvs.runcmd(
            "ip neigh flush " + neigh_ipv6
        )

        # Cleanup
        dvs.runcmd(
            "vtysh -c \"configure terminal\" -c \"no ip route " + nh_route +
            " " + neigh_ip + "\""
        )
        dvs.runcmd(
            "vtysh -c \"configure terminal\" -c \"no ipv6 route " + nh_route_ipv6 +
            " " + neigh_ipv6 + "\""
        )
        self.del_neighbor(dvs, neigh_ip)
        self.del_neighbor(dvs, neigh_ipv6)

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

        # Set mux port in active-active cable type, no ACL rules programmed
        self.set_mux_state(appdb, "Ethernet0", "standby")
        dvs_acl.verify_no_acl_rules()

        # Set one mux port to standby, verify ACL rule with inport bitmap (1 port)
        self.set_mux_state(appdb, "Ethernet4", "standby")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet4"], dvs_acl)
        dvs_acl.verify_acl_rule(sai_qualifier, action="DROP", priority=self.ACL_PRIORITY)

        # Set two mux ports to standby, verify ACL rule with inport bitmap (2 ports)
        self.set_mux_state(appdb, "Ethernet8", "standby")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet4", "Ethernet8"], dvs_acl)
        dvs_acl.verify_acl_rule(sai_qualifier, action="DROP", priority=self.ACL_PRIORITY)

        self.set_mux_state(appdb, "Ethernet0", "active")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet4", "Ethernet8"], dvs_acl)
        dvs_acl.verify_acl_rule(sai_qualifier, action="DROP", priority=self.ACL_PRIORITY)

        # Set one mux port to active, verify ACL rule with inport bitmap (1 port)
        self.set_mux_state(appdb, "Ethernet4", "active")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet8"], dvs_acl)
        dvs_acl.verify_acl_rule(sai_qualifier, action="DROP", priority=self.ACL_PRIORITY)

        # Set last mux port to active, verify ACL rule is deleted
        self.set_mux_state(appdb, "Ethernet8", "active")
        dvs_acl.verify_no_acl_rules()

        # Set unknown state and verify the behavior as standby
        self.set_mux_state(appdb, "Ethernet4", "unknown")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet4"], dvs_acl)
        dvs_acl.verify_acl_rule(sai_qualifier, action="DROP", priority=self.ACL_PRIORITY)

        # Verify change while setting unknown from active
        self.set_mux_state(appdb, "Ethernet8", "unknown")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet4", "Ethernet8"], dvs_acl)
        dvs_acl.verify_acl_rule(sai_qualifier, action="DROP", priority=self.ACL_PRIORITY)

        self.set_mux_state(appdb, "Ethernet4", "active")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet8"], dvs_acl)
        dvs_acl.verify_acl_rule(sai_qualifier, action="DROP", priority=self.ACL_PRIORITY)

        self.set_mux_state(appdb, "Ethernet4", "standby")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet4", "Ethernet8"], dvs_acl)
        dvs_acl.verify_acl_rule(sai_qualifier, action="DROP", priority=self.ACL_PRIORITY)

        # Verify no change while setting unknown from standby
        self.set_mux_state(appdb, "Ethernet4", "unknown")
        sai_qualifier = self.get_expected_sai_qualifiers(["Ethernet4", "Ethernet8"], dvs_acl)
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

    def create_and_test_peer(self, asicdb, tc_to_dscp_map_oid=None, tc_to_queue_map_oid=None):
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
            elif field == "SAI_TUNNEL_ATTR_DECAP_TTL_MODE":
                assert value == "SAI_TUNNEL_TTL_MODE_PIPE_MODEL"
            elif field == "SAI_TUNNEL_ATTR_LOOPBACK_PACKET_ACTION":
                assert value == "SAI_PACKET_ACTION_DROP"
            elif field == "SAI_TUNNEL_ATTR_ENCAP_QOS_TC_AND_COLOR_TO_DSCP_MAP":
                assert value == tc_to_dscp_map_oid
            elif field == "SAI_TUNNEL_ATTR_ENCAP_QOS_TC_TO_QUEUE_MAP":
                assert value == tc_to_queue_map_oid
            elif field == "SAI_TUNNEL_ATTR_ENCAP_DSCP_MODE":
                assert value == "SAI_TUNNEL_DSCP_MODE_PIPE_MODEL"
            elif field == "SAI_TUNNEL_ATTR_DECAP_DSCP_MODE":
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

    def create_and_test_tunnel(self, db, asicdb, tunnel_name, tunnel_params):
        """ Create tunnel and verify all needed enties in ASIC DB exists """

        is_symmetric_tunnel = "src_ip" in tunnel_params

        # 6 parameters to check in case of decap tunnel
        # + 1 (SAI_TUNNEL_ATTR_ENCAP_SRC_IP) in case of symmetric tunnel
        expected_len = 7 if is_symmetric_tunnel else 6

        if 'decap_tc_to_pg_map_id' in tunnel_params:
            expected_len += 1
            decap_tc_to_pg_map_id = tunnel_params.pop('decap_tc_to_pg_map_id')

        if 'decap_dscp_to_tc_map_id' in tunnel_params:
            expected_len += 1
            decap_dscp_to_tc_map_id = tunnel_params.pop('decap_dscp_to_tc_map_id')

        # check asic db table
        tunnels = asicdb.wait_for_n_keys(self.ASIC_TUNNEL_TABLE, 1)

        tunnel_sai_obj = tunnels[0]

        fvs = asicdb.wait_for_entry(self.ASIC_TUNNEL_TABLE, tunnel_sai_obj)

        assert len(fvs) == expected_len

        expected_ecn_mode = self.ecn_modes_map[tunnel_params["ecn_mode"]]
        expected_dscp_mode = self.dscp_modes_map[tunnel_params["dscp_mode"]]
        expected_ttl_mode = self.ttl_modes_map[tunnel_params["ttl_mode"]]


        for field, value in fvs.items():
            if field == "SAI_TUNNEL_ATTR_TYPE":
                assert value == "SAI_TUNNEL_TYPE_IPINIP"
            elif field == "SAI_TUNNEL_ATTR_ENCAP_SRC_IP":
                assert value == tunnel_params["src_ip"]
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
                assert value == decap_dscp_to_tc_map_id
            elif field == "SAI_TUNNEL_ATTR_DECAP_QOS_TC_TO_PRIORITY_GROUP_MAP":
                assert value == decap_tc_to_pg_map_id
            else:
                assert False, "Field %s is not tested" % field


        src_ip = tunnel_params['src_ip'] if 'src_ip' in tunnel_params else None
        self.check_tunnel_termination_entry_exists_in_asicdb(asicdb, tunnel_sai_obj, tunnel_params["dst_ip"].split(","), src_ip)

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
    @pytest.fixture(scope='class')
    def setup(self, dvs):
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


    def test_Tunnel(self, dvs, setup_tunnel, testlog, setup):
        """ test IPv4 Mux tunnel creation """
        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        #self.cleanup_left_over(db, asicdb)
        _, _, dscp_to_tc_map_oid, tc_to_pg_map_oid = setup
        tunnel_params = self.DEFAULT_TUNNEL_PARAMS
        tunnel_params["decap_dscp_to_tc_map_id"] = dscp_to_tc_map_oid
        tunnel_params["decap_tc_to_pg_map_id"] = tc_to_pg_map_oid

        # create tunnel IPv4 tunnel
        self.create_and_test_tunnel(db, asicdb, self.MUX_TUNNEL_0, tunnel_params)

    def test_Peer(self, dvs, setup_peer_switch, setup_tunnel, setup, testlog):

        """ test IPv4 Mux tunnel creation """

        asicdb = dvs.get_asic_db()
        
        encap_tc_to_dscp_map_id, encap_tc_to_queue_map_id, _, _ = setup

        self.create_and_test_peer(asicdb, encap_tc_to_dscp_map_id, encap_tc_to_queue_map_id)

    def test_Neighbor(self, dvs, dvs_route, setup_vlan, setup_mux_cable, testlog):
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

    def test_NH(self, dvs, dvs_route, intf_fdb_map, setup, setup_mux_cable,
                setup_peer_switch, setup_tunnel, testlog):
        """ test NH routes and mux state change """
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        mac = intf_fdb_map["Ethernet0"]

        # get tunnel nexthop
        self.check_tnl_nexthop_in_asic_db(asicdb, 5)

        self.create_and_test_NH_routes(appdb, asicdb, dvs, dvs_route, mac)

    def test_multi_nexthop(self, dvs, dvs_route, intf_fdb_map, neighbor_cleanup, testlog):
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        macs = [intf_fdb_map["Ethernet0"], intf_fdb_map["Ethernet4"], intf_fdb_map["Ethernet8"]]

        self.create_and_test_multi_nexthop_routes(dvs, dvs_route, appdb, macs, asicdb)

    def test_acl(self, dvs, dvs_acl, testlog):
        """ test acl and mux state change """

        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

        try:
            self.create_and_test_acl(appdb, dvs_acl)
        finally:
            self.set_mux_state(appdb, "Ethernet0", "active")
            self.set_mux_state(appdb, "Ethernet4", "active")
            self.set_mux_state(appdb, "Ethernet8", "active")
            dvs_acl.verify_no_acl_rules()

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

    def test_neighbor_miss_no_mux(
            self, dvs, dvs_route, setup_vlan, setup_tunnel, setup,
            setup_peer_switch, neighbor_cleanup, testlog
    ):
        config_db = dvs.get_config_db()
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

        test_ip = self.SERV1_SOC_IPV4
        self.ping_ip(dvs, test_ip)

        # no mux present, no standalone tunnel route installed
        self.check_neighbor_state(dvs, dvs_route, test_ip, expect_route=False)

        # setup the mux
        config_db = dvs.get_config_db()
        self.create_mux_cable(config_db)
        # tunnel route should be installed immediately after mux setup
        self.check_neighbor_state(dvs, dvs_route, test_ip, expect_route=True)

        # set port state as standby
        self.set_mux_state(appdb, "Ethernet0", "standby")
        self.check_neighbor_state(dvs, dvs_route, test_ip, expect_route=True)

        # set port state as active
        self.set_mux_state(appdb, "Ethernet0", "active")
        self.check_neighbor_state(dvs, dvs_route, test_ip, expect_route=True)

        # clear the FAILED neighbor
        self.clear_neighbors(dvs)
        self.check_neighbor_state(dvs, dvs_route, test_ip, expect_route=False)

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

    def test_soc_ip(self, dvs, dvs_route, setup_vlan, setup_mux_cable, testlog):
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        self.create_and_test_soc(appdb, asicdb, dvs, dvs_route)

    def test_warm_boot_mux_state(
            self, dvs, dvs_route, setup_vlan, setup_mux_cable, setup_tunnel,
            remove_peer_switch, neighbor_cleanup, testlog
    ):
        """
        test mux initialization during warm boot.
        """
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        apdb = dvs.get_app_db()

        self.set_mux_state(appdb, "Ethernet0", "active")
        self.set_mux_state(appdb, "Ethernet4", "active")
        self.set_mux_state(appdb, "Ethernet8", "standby")

        # Execute the warm reboot
        dvs.runcmd("config warm_restart enable swss")
        dvs.stop_swss()
        dvs.start_swss()

        time.sleep(5)

        fvs = apdb.get_entry(self.APP_MUX_CABLE, "Ethernet0")
        for key in fvs:
            if key == "state":
                assert fvs[key] == "active", "Ethernet0 Mux state is not active after warm boot, state: {}".format(fvs[key])

        fvs = apdb.get_entry(self.APP_MUX_CABLE, "Ethernet4")
        for key in fvs:
            if key == "state":
                assert fvs[key] == "active", "Ethernet4 Mux state is not active after warm boot, state: {}".format(fvs[key])

        fvs = apdb.get_entry(self.APP_MUX_CABLE, "Ethernet8")
        for key in fvs:
            if key == "state":
                assert fvs[key] == "standby", "Ethernet8 Mux state is not standby after warm boot, state: {}".format(fvs[key])


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
