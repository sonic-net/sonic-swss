import time
import pytest
import json
import itertools

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

    # Test IPs for FDB-after-neighbor conversion tests (non-pre-configured)
    TEST_NEIGH1_IPV4           = "192.168.0.110"
    TEST_NEIGH2_IPV4           = "192.168.0.111"
    TEST_NEIGH3_IPV4           = "192.168.0.112"
    TEST_NEIGH4_IPV4           = "192.168.0.113"
    TEST_NEIGH5_IPV4           = "192.168.0.114"
    TEST_NEIGH6_IPV4           = "192.168.0.115"

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
        "src_ip": PEER_IPV4,
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

    BULK_NEIGHBOR_COUNT = 254

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

    def check_neigh_in_asic_db(self, asicdb, ip, expected=True, no_host_route=True):
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
                    if no_host_route:
                        fvs = asicdb.get_entry(self.ASIC_NEIGH_TABLE, key)
                        assert fvs.get("SAI_NEIGHBOR_ENTRY_ATTR_NO_HOST_ROUTE") == "true"
                        return key
                    else:
                        return key

        else:
            asicdb.wait_for_deleted_keys(self.ASIC_NEIGH_TABLE, [expected_key])

        return ''

    def check_tnl_nexthop_in_asic_db(self, asicdb, expected=None):

        global tunnel_nh_id

        if expected:
            nh = asicdb.wait_for_n_keys(self.ASIC_NEXTHOP_TABLE, expected)
        else:
            nh = asicdb.get_keys(self.ASIC_NEXTHOP_TABLE)

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
        """
        Checks if nexthop is a member of a given route
        """
        route_key = dvs_route.check_asicdb_route_entries([route])
        route_nexthop_oid = self.get_route_nexthop_oid(route_key[0], asicdb)

        if tunnel:
            return route_nexthop_oid == nexthop

        nexthop_oid = self.get_nexthop_oid(asicdb, nexthop)

        return route_nexthop_oid == nexthop_oid

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

    def simulate_fdb_learn_notification(self, dvs, port, mac, vlan_name="Vlan1000"):
        """
        Simulate a SAI FDB learn notification event.

        This function sends a direct SAI FDB_EVENT_LEARNED notification
        to simulate hardware FDB learning, which should trigger MUX neighbor
        conversion if the MAC matches an existing neighbor.
        """
        dvs.setup_db()
        # Get required SAI OIDs
        dvs.get_asic_db()

        # Get switch ID
        switch_id = dvs.getSwitchOid()

        # Get VLAN OID
        vlan_oid = dvs.getVlanOid("1000")

        # Get bridge port OID for the specified port
        # Get mapping between interface name and its bridge port_id
        iface_2_bridge_port_id = dvs.get_map_iface_bridge_port_id(dvs.adb)
        # Get bridge port OID for the specified port
        bridge_port_oid = iface_2_bridge_port_id[port]

        # Create notification producer
        ntf = swsscommon.NotificationProducer(dvs.adb, "NOTIFICATIONS")
        fvp = swsscommon.FieldValuePairs()

        # Format MAC address for SAI (uppercase, colon-separated)
        sai_mac = mac.upper()

        # Create FDB learn notification data
        ntf_data = f'[{{"fdb_entry":"{{\\"bvid\\":\\"{vlan_oid}\\",\\"mac\\":\\"{sai_mac}\\",\\"switch_id\\":\\"{switch_id}\\"}}","fdb_event":"SAI_FDB_EVENT_LEARNED","list":[{{"id":"SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID","value":"{bridge_port_oid}"}}]}}]'

        # Send the FDB learn notification
        ntf.send("fdb_event", ntf_data, fvp)

        # Allow time for processing and potential neighbor conversion
        time.sleep(2)

    def simulate_fdb_aged_notification(self, dvs, port, mac, vlan_name="Vlan1000"):
        """
        Simulate a SAI FDB aged notification event.

        This function sends a direct SAI FDB_EVENT_AGED notification
        to simulate hardware FDB aging/removal.
        """
        dvs.setup_db()
        # Get required SAI OIDs
        dvs.get_asic_db()

        # Get switch ID
        switch_id = dvs.getSwitchOid()

        # Get VLAN OID
        vlan_oid = dvs.getVlanOid("1000")

        # Get mapping between interface name and its bridge port_id
        iface_2_bridge_port_id = dvs.get_map_iface_bridge_port_id(dvs.adb)
        # Get bridge port OID for the specified port
        bridge_port_oid = iface_2_bridge_port_id[port]

        # Create notification producer
        ntf = swsscommon.NotificationProducer(dvs.adb, "NOTIFICATIONS")
        fvp = swsscommon.FieldValuePairs()

        # Format MAC address for SAI (uppercase, colon-separated)
        sai_mac = mac.upper()

        # Create FDB aged notification data
        ntf_data = f'[{{"fdb_entry":"{{\\"bvid\\":\\"{vlan_oid}\\",\\"mac\\":\\"{sai_mac}\\",\\"switch_id\\":\\"{switch_id}\\"}}","fdb_event":"SAI_FDB_EVENT_AGED","list":[{{"id":"SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID","value":"{bridge_port_oid}"}}]}}]'

        # Send the FDB aged notification
        ntf.send("fdb_event", ntf_data, fvp)

        # Allow time for processing
        time.sleep(2)

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

    def wait_for_mux_state(self, dvs, interface, expected_state):
        """
        Waits until state change completes - expected state is in state_db
        """

        apdb = dvs.get_app_db()
        expected_field = {"state": expected_state}
        apdb.wait_for_field_match(self.APP_MUX_CABLE, interface, expected_field)

    def bulk_neighbor_test(self, confdb, appdb, asicdb, dvs, dvs_route):
        dvs.runcmd("ip neigh flush all")
        self.add_fdb(dvs, "Ethernet0", "00-00-00-00-11-11")
        self.set_mux_state(appdb, "Ethernet0", "active")

        class neighbor_info:
            ipv4_key = ""
            ipv6_key = ""
            ipv4 = ""
            ipv6 = ""

            def __init__(self, i):
                self.ipv4 = "192.168.1." + str(i)
                self.ipv6 = "fc02:1001::" + str(i)

        neighbor_list = [neighbor_info(i) for i in range(100, self.BULK_NEIGHBOR_COUNT)]
        for neigh_info in neighbor_list:
            self.add_neighbor(dvs, neigh_info.ipv4, "00:00:00:00:11:11")
            self.add_neighbor(dvs, neigh_info.ipv6, "00:00:00:00:11:11")
            neigh_info.ipv4_key = self.check_neigh_in_asic_db(asicdb, neigh_info.ipv4)
            neigh_info.ipv6_key = self.check_neigh_in_asic_db(asicdb, neigh_info.ipv6)
            dvs_route.check_asicdb_route_entries(
                [neigh_info.ipv4+self.IPV4_MASK, neigh_info.ipv6+self.IPV6_MASK]
            )
            rtv4_keys = dvs_route.check_asicdb_route_entries([neigh_info.ipv4+self.IPV4_MASK])
            rtv6_keys = dvs_route.check_asicdb_route_entries([neigh_info.ipv6+self.IPV6_MASK])
            self.check_nexthop_in_asic_db(asicdb, rtv4_keys[0])
            self.check_nexthop_in_asic_db(asicdb, rtv6_keys[0])

        try:
            self.set_mux_state(appdb, "Ethernet0", "standby")
            self.wait_for_mux_state(dvs, "Ethernet0", "standby")

            # Number of NH = Neighbor NHs + 1 tunnel NH,
            # also populates the tunnel_nh_id
            expected_nhs = (len(neighbor_list) * 2) + 1
            self.check_tnl_nexthop_in_asic_db(asicdb, expected_nhs)

            for neigh_info in neighbor_list:
                # neighbor should not be removed
                ipv4_key = self.check_neigh_in_asic_db(asicdb, neigh_info.ipv4)
                assert ipv4_key == neigh_info.ipv4_key
                ipv6_key = self.check_neigh_in_asic_db(asicdb, neigh_info.ipv6)
                assert ipv6_key == neigh_info.ipv6_key
                dvs_route.check_asicdb_route_entries(
                    [neigh_info.ipv4+self.IPV4_MASK, neigh_info.ipv6+self.IPV6_MASK]
                )
                self.check_nexthop_in_asic_db(asicdb, rtv4_keys[0], True)
                self.check_nexthop_in_asic_db(asicdb, rtv6_keys[0], True)

            self.set_mux_state(appdb, "Ethernet0", "active")
            self.wait_for_mux_state(dvs, "Ethernet0", "active")

            for neigh_info in neighbor_list:
                neigh_info.ipv4_key = self.check_neigh_in_asic_db(asicdb, neigh_info.ipv4)
                self.check_nexthop_in_asic_db(asicdb, rtv4_keys[0])
                neigh_info.ipv6_key = self.check_neigh_in_asic_db(asicdb, neigh_info.ipv6)
                self.check_nexthop_in_asic_db(asicdb, rtv6_keys[0])

        finally:
            for neigh_info in neighbor_list:
                self.del_neighbor(dvs, neigh_info.ipv4)
                self.del_neighbor(dvs, neigh_info.ipv6)

    def create_and_test_neighbor(self, confdb, appdb, asicdb, dvs, dvs_route):

        self.bulk_neighbor_test(confdb, appdb, asicdb, dvs, dvs_route)
        self.set_mux_state(appdb, "Ethernet0", "active")
        self.set_mux_state(appdb, "Ethernet4", "standby")

        self.add_neighbor(dvs, self.SERV1_IPV4, "00:00:00:00:00:01")
        srv1_v4 = self.check_neigh_in_asic_db(asicdb, self.SERV1_IPV4)

        self.add_neighbor(dvs, self.SERV1_IPV6, "00:00:00:00:00:01")
        srv1_v6 = self.check_neigh_in_asic_db(asicdb, self.SERV1_IPV6)

        existing_keys = asicdb.get_keys(self.ASIC_NEIGH_TABLE)

        # neighbors must get added even for standby port
        self.add_neighbor(dvs, self.SERV2_IPV4, "00:00:00:00:00:02")
        self.check_neigh_in_asic_db(asicdb, self.SERV2_IPV4)

        self.add_neighbor(dvs, self.SERV2_IPV6, "00:00:00:00:00:02")
        self.check_neigh_in_asic_db(asicdb, self.SERV2_IPV6)

        time.sleep(1)

        # neighbors use no_host_route and explicit routes are added
        rt_keys = dvs_route.check_asicdb_route_entries(
            [self.SERV1_IPV4+self.IPV4_MASK, self.SERV1_IPV6+self.IPV6_MASK,
             self.SERV2_IPV4+self.IPV4_MASK, self.SERV2_IPV6+self.IPV6_MASK]
        )

        # The first standby route also creates as tunnel Nexthop
        self.check_tnl_nexthop_in_asic_db(asicdb, (len(rt_keys) + 1))

        # check for local and standby nexthop
        rt_keys_srv1 = dvs_route.check_asicdb_route_entries(
            [self.SERV1_IPV4+self.IPV4_MASK, self.SERV1_IPV6+self.IPV6_MASK]
        )
        rt_keys_srv2 = dvs_route.check_asicdb_route_entries(
             [self.SERV2_IPV4+self.IPV4_MASK, self.SERV2_IPV6+self.IPV6_MASK]
        )
        self.check_nexthop_in_asic_db(asicdb, rt_keys_srv1[0])
        self.check_nexthop_in_asic_db(asicdb, rt_keys_srv1[1])
        self.check_nexthop_in_asic_db(asicdb, rt_keys_srv2[0], True)
        self.check_nexthop_in_asic_db(asicdb, rt_keys_srv2[1], True)

        # Change state to Standby. check NHs should point to tunnel
        self.set_mux_state(appdb, "Ethernet0", "standby")
        self.check_nexthop_in_asic_db(asicdb, rt_keys_srv1[0], True)
        self.check_nexthop_in_asic_db(asicdb, rt_keys_srv1[1], True)

        # Change state to Active. Check NHs should not point to tunnel
        self.set_mux_state(appdb, "Ethernet4", "active")
        self.check_nexthop_in_asic_db(asicdb, rt_keys_srv2[0])
        self.check_nexthop_in_asic_db(asicdb, rt_keys_srv2[1])


    def create_and_test_soc(self, appdb, asicdb, dvs, dvs_route):

        self.set_mux_state(appdb, "Ethernet0", "active")

        self.add_fdb(dvs, "Ethernet0", "00-00-00-00-00-01")
        self.add_neighbor(dvs, self.SERV1_SOC_IPV4, "00:00:00:00:00:01")

        time.sleep(1)

        srv1_soc_v4 = self.check_neigh_in_asic_db(asicdb, self.SERV1_SOC_IPV4)
        self.check_tunnel_route_in_app_db(dvs, [self.SERV1_SOC_IPV4+self.IPV4_MASK], expected=False)

        self.set_mux_state(appdb, "Ethernet0", "standby")

        # neighbor entry should not be removed
        soc_v4 = self.check_neigh_in_asic_db(asicdb, self.SERV1_SOC_IPV4)
        assert soc_v4 == srv1_soc_v4
        dvs_route.check_asicdb_route_entries(
            [self.SERV1_SOC_IPV4+self.IPV4_MASK]
        )
        self.check_tunnel_route_in_app_db(dvs, [self.SERV1_SOC_IPV4+self.IPV4_MASK], expected=False)
        self.check_tnl_nexthop_in_asic_db(asicdb, 2)
        self.check_route_nexthop(dvs_route, asicdb, self.SERV1_SOC_IPV4+self.IPV4_MASK, tunnel_nh_id, True)

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

        # Both ip_1 and ip_2 should be added as neighbors
        # with no_host_route.
        # ip_1 should have route pointing to neigh nh
        # ip_2 should have route pointing to tunnel nh
        self.check_neigh_in_asic_db(asicdb, ip_1)
        self.check_neigh_in_asic_db(asicdb, ip_2)

        ip1_rt_key = dvs_route.check_asicdb_route_entries([ip_1+self.IPV6_MASK])
        ip2_rt_key = dvs_route.check_asicdb_route_entries([ip_2+self.IPV6_MASK])

        self.check_nexthop_in_asic_db(asicdb, ip1_rt_key[0])
        self.check_nexthop_in_asic_db(asicdb, ip2_rt_key[0], True)
        # Check ip_1 move to standby mux, should be pointing to tunnel
        self.add_neighbor(dvs, ip_1, "00:00:00:00:00:12")
        time.sleep(1)
        self.check_nexthop_in_asic_db(asicdb, ip1_rt_key[0], True)

        # Check ip_2 move to active mux, should be host entry
        self.add_neighbor(dvs, ip_2, "00:00:00:00:00:11")
        time.sleep(1)
        self.check_nexthop_in_asic_db(asicdb, ip2_rt_key[0])

        # Simulate FDB aging out test case
        ip_3 = "192.168.0.200"

        self.add_neighbor(dvs, ip_3, "00:00:00:00:00:12")
        time.sleep(1)

        # ip_3 is added to standby mux
        ip3_rt_key = dvs_route.check_asicdb_route_entries([ip_3+self.IPV4_MASK])
        self.check_nexthop_in_asic_db(asicdb, ip3_rt_key[0], True)

        # Simulate FDB age out
        self.del_fdb(dvs, "00-00-00-00-00-12")

        # FDB ageout is not expected to change existing state of neighbor
        self.check_nexthop_in_asic_db(asicdb, ip3_rt_key[0], True)

        # Change to active
        self.set_mux_state(appdb, "Ethernet4", "active")
        self.check_nexthop_in_asic_db(asicdb, ip3_rt_key[0])

        self.del_fdb(dvs, "00-00-00-00-00-11")

    def create_and_test_route(self, appdb, asicdb, dvs, dvs_route, mac_Ethernet0, mac_Ethernet4):
        self.set_mux_state(appdb, "Ethernet0", "active")

        rtprefix = "2.3.4.0/24"

        # Make sure neighbor is present
        self.add_neighbor(dvs, self.SERV1_IPV4, mac_Ethernet0)
        self.add_neighbor(dvs, self.SERV2_IPV4, mac_Ethernet0)

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

        self.check_tnl_nexthop_in_asic_db(asicdb)
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

        dvs.runcmd(
            "vtysh -c \"configure terminal\" -c \"no ip route " + rtprefix +
            " " + self.SERV1_IPV4 + "\""
        )

    def create_and_test_route_learned_before_neighbor(self, appdb, asicdb, dvs, dvs_route, mac):
        rtprefix = "2.3.4.0/24"
        neigh = "192.168.0.110"
        mux_port = "Ethernet0"

        nexthop_map = {"active": neigh, "standby": tunnel_nh_id}
        toggle_map = {"active": "standby", "standby": "active"}

        for state in ["standby", "active"]:
            try:
                current_state = state
                self.set_mux_state(appdb, mux_port, current_state)

                self.add_route(dvs, rtprefix, [neigh])
                time.sleep(1)
                self.add_neighbor(dvs, neigh, mac)

                # Confirm route is pointing to tunnel nh
                self.check_route_nexthop(dvs_route, asicdb, rtprefix, nexthop_map[current_state], (current_state == "standby"))

                # Toggle the mux a few times
                current_state = toggle_map[current_state]
                self.set_mux_state(appdb, mux_port, current_state)
                self.check_route_nexthop(dvs_route, asicdb, rtprefix, nexthop_map[current_state], (current_state == "standby"))

                current_state = toggle_map[current_state]
                self.set_mux_state(appdb, mux_port, current_state)
                self.check_route_nexthop(dvs_route, asicdb, rtprefix, nexthop_map[current_state], (current_state == "standby"))

            finally:
                self.del_route(dvs, rtprefix)
                self.del_neighbor(dvs, neigh)

    def multi_nexthop_check(self, asicdb, dvs_route, route, nexthops, mux_states, non_mux_nexthop=None, expect_active=None):
        """
        Checks if multi-mux route points to an active nexthop or tunnel.
        - If a non_mux_nexthop is present, expect to point to that
        - Checks all active mux ports first
        - If no active nexthops are found, assert tunnel nexthop is programmed
        - If an active nexthop is expected but none is found, alert
        """
        if isinstance(route, list):
            route_copy = route.copy()
        else:
            route_copy = [route]

        # If we don't specify expected active state, default to true if Active mux cable is present
        assert_active = (ACTIVE in mux_states)
        if expect_active is not None:
            assert_active = expect_active

        for rt in route_copy:
            if non_mux_nexthop != None:
                assert self.check_route_nexthop(dvs_route, asicdb, rt, non_mux_nexthop), \
                    f"Nexthop {non_mux_nexthop} expected but not found for route {rt}"
                continue
            for i,state in enumerate(mux_states):
                # Find first active mux port, and check that route points to that neighbor
                if state == ACTIVE and self.check_route_nexthop(dvs_route, asicdb, rt, nexthops[i]):
                    break
            else:
                # If no active mux port, check that route points to tunnel
                if assert_active:
                    assert False, f"Active nexthop expected for route {rt}, but not found"
                else:
                    assert self.check_route_nexthop(dvs_route, asicdb, rt, tunnel_nh_id, True), \
                        f"Tunnel nexthop expected for route {rt}, but not found"

    def multi_nexthop_test_create(self, appdb, asicdb, dvs, dvs_route, route, mux_ports, nexthops, non_mux_nexthop = None):
        '''
        Tests the creation of a route with multiple nexthops in various combinations of initial mux state
        '''
        init_mux_states = list(itertools.product([ACTIVE, STANDBY], repeat=len(mux_ports)))

        print("Test create route in various combos of mux nexthop states for route with multiple nexthops")
        for states in init_mux_states:
            print("Create route with mux ports: %s in states: %s" % (str(mux_ports), str(states)))
            # Set mux states
            for i,port in enumerate(mux_ports):
                self.set_mux_state(appdb, port, states[i])

            # Add route
            if non_mux_nexthop != None:
                self.add_route(dvs, route, nexthops + [non_mux_nexthop])
            else:
                self.add_route(dvs, route, nexthops)
            self.multi_nexthop_check(asicdb, dvs_route, route, nexthops, states, non_mux_nexthop)

            self.del_route(dvs, route)

    def multi_nexthop_test_toggle(self, appdb, asicdb, dvs_route, route, mux_ports, nexthops, non_mux_nexthop=None):
        '''
        Tests toggling mux state for a route with multiple nexthops
        '''
        init_mux_states = list(list(tup) for tup in itertools.product([ACTIVE, STANDBY], repeat=len(mux_ports)))

        print("Test toggling mux state for route with multiple mux nexthops")
        for states in init_mux_states:
            print("Testing state change in states: %s, for nexthops: %s" % (str(states), str(nexthops)))
            for i,port in enumerate(mux_ports):
                if nexthops[i] == non_mux_nexthop:
                    continue
                self.set_mux_state(appdb, port, states[i])

            for toggle_index,toggle_port in enumerate(mux_ports):
                if nexthops[toggle_index] == non_mux_nexthop:
                    continue
                new_states = states.copy()

                print("Toggling %s from %s" % (toggle_port, states[toggle_index]))

                if states[toggle_index] == ACTIVE:
                    new_states[toggle_index] = STANDBY
                    self.set_mux_state(appdb, toggle_port, STANDBY)
                    self.multi_nexthop_check(asicdb, dvs_route, route, nexthops, new_states, non_mux_nexthop)

                    new_states[toggle_index] = ACTIVE
                    self.set_mux_state(appdb, toggle_port, ACTIVE)
                    self.multi_nexthop_check(asicdb, dvs_route, route, nexthops, new_states, non_mux_nexthop)
                else:
                    new_states[toggle_index] = ACTIVE
                    self.set_mux_state(appdb, toggle_port, ACTIVE)
                    self.multi_nexthop_check(asicdb, dvs_route, route, nexthops, new_states, non_mux_nexthop)

                    new_states[toggle_index] = STANDBY
                    self.set_mux_state(appdb, toggle_port, STANDBY)
                    self.multi_nexthop_check(asicdb, dvs_route, route, nexthops, new_states, non_mux_nexthop)

        # Set everything back to active
        for i,port in enumerate(mux_ports):
            if nexthops[i] == non_mux_nexthop:
                continue
            self.set_mux_state(appdb, port, ACTIVE)

    def multi_nexthop_test_route_update_keep_size(self, appdb, asicdb, dvs, dvs_route, route, mux_ports, nexthops, new_nexthop, new_mux_port, nh_is_mux=True):
        '''
        Tests route update for a route with multiple nexthops with same number of nexthops
         - nh_is_mux: is True if new nexthop is a mux nexthop, False if not
        '''
        # Add route
        self.add_route(dvs, route, nexthops)

        print("Test route update for route with multiple mux nexthops")
        for i,nexthop in enumerate(nexthops):
            new_nexthops = nexthops.copy()
            new_muxports = mux_ports.copy()

            print("Triggering route update %s to replace: %s with: %s" % (str(new_nexthops), str(nexthop), str(new_nexthop)))
            new_nexthops[i] = new_nexthop
            new_muxports[i] = new_mux_port

            if nh_is_mux:
                # We need to sort the nexthops to match the way they will pe processed
                new_nexthops.sort()
                new_muxports.sort()

            self.add_route(dvs, route, new_nexthops)

            if nh_is_mux:
                self.multi_nexthop_test_toggle(appdb, asicdb, dvs_route, route, new_muxports, new_nexthops)
            else:
                self.multi_nexthop_test_toggle(appdb, asicdb, dvs_route, route, new_muxports, new_nexthops, non_mux_nexthop=new_nexthop)

            # Reset route
            self.add_route(dvs, route, nexthops)

        self.del_route(dvs, route)

    def multi_nexthop_test_route_update_increase_size(self, appdb, asicdb, dvs, dvs_route, route, mux_ports, nexthops, non_mux_nexthop=None):
        '''
        Tests route update for a route with multiple nexthops increasing number of nexthops over time
        '''
        print("Test route update for route with multiple mux nexthops")
        for i,nexthop in enumerate(nexthops):
            print("Triggering route update to add: %s. new route %s -> %s" % (str(nexthop), route, nexthops[:i+1]))
            self.add_route(dvs, route, nexthops[:i+1])
            self.multi_nexthop_test_toggle(appdb, asicdb, dvs_route, route, mux_ports[:i+1], nexthops[:i+1])

        # Add non_mux_nexthop to route list
        if non_mux_nexthop != None:
            print("Triggering route update to add non_mux: %s. new route %s -> %s" % (str(non_mux_nexthop), route, nexthops + [non_mux_nexthop]))
            self.add_route(dvs, route, nexthops + [non_mux_nexthop])
            self.multi_nexthop_test_toggle(appdb, asicdb, dvs_route, route, mux_ports + [None], nexthops + [non_mux_nexthop], non_mux_nexthop=non_mux_nexthop)

        self.del_route(dvs, route)

    def multi_nexthop_test_route_update_decrease_size(self, appdb, asicdb, dvs, dvs_route, route, mux_ports, nexthops, non_mux_nexthop=None):
        '''
        Tests route update for a route with multiple nexthops increasing number of nexthops over time
        '''
        print("Test route update for route with multiple mux nexthops")

        if non_mux_nexthop != None:
            print("Triggering route update to add non_mux: %s. new route %s -> %s" % (str(non_mux_nexthop), route, [non_mux_nexthop] + nexthops))
            self.add_route(dvs, route, [non_mux_nexthop] + nexthops)
            self.multi_nexthop_test_toggle(appdb, asicdb, dvs_route, route, [None] + mux_ports, [non_mux_nexthop] + nexthops, non_mux_nexthop=non_mux_nexthop)

        for i,nexthop in enumerate(nexthops):
            print("Triggering route update to remove: %s. new route %s -> %s" % (str(nexthop), route, nexthops[i:]))
            self.add_route(dvs, route, nexthops[i:])
            self.multi_nexthop_test_toggle(appdb, asicdb, dvs_route, route, mux_ports[i:], nexthops[i:])

        self.del_route(dvs, route)

    def multi_nexthop_test_neighbor_delete_and_create(self, appdb, asicdb, dvs, dvs_route, route, mux_ports, nexthops, macs):
        '''
        Tests deleting neighbors in one state, then adding them in another
        '''
        # Get array of mux states to test:
        mux_states = list(list(tup) for tup in itertools.product([ACTIVE, STANDBY], repeat=len(mux_ports)*2))

        print("Test deleting then creating neighbors:")
        for states in mux_states:
            initial_states = states[:len(mux_ports)]
            final_states = states[len(mux_ports):]

            print(f"Test delete neighbors in {str(initial_states)} then creating in {str(final_states)}")

            # Set intial states:
            for i,port in enumerate(mux_ports):
                self.set_mux_state(appdb, port, initial_states[i])

            # Delete all neighbors, ensure either active existing neighbor or tunnel nexthop as we go
            for i, nexthop in enumerate(nexthops):
                self.del_neighbor(dvs, nexthop)

                expect_active = i < len(initial_states) and \
                    ACTIVE in initial_states[i:]
                self.multi_nexthop_check(asicdb, dvs_route, route, nexthops, initial_states, expect_active=expect_active)

            # Set final states:
            for i,port in enumerate(mux_ports):
                self.set_mux_state(appdb, port, initial_states[i])

            # Create all neighbors, ensure either active existing neighbor or tunnel nexthop as we go
            for i, nexthop in enumerate(nexthops):
                self.add_neighbor(dvs, nexthop, macs[i])

                expect_active = i < len(initial_states) and \
                    ACTIVE in initial_states[:i]
                self.multi_nexthop_check(asicdb, dvs_route, route, nexthops, initial_states, expect_active=expect_active)

    def multi_nexthop_test_vlan_neighbor_update(self, appdb, asicdb, dvs, dvs_route, route, mux_ports, nexthops, macs, new_neighbor):
        """
        Tests crash case where neighbor update on vlan updated multi-mux route when it should not have
            Assumes 2-nexthop route
        """
        for state in [ACTIVE, STANDBY]:
            mux_states = [STANDBY, state]

            # Set intial states:
            for i,port in enumerate(mux_ports):
                self.set_mux_state(appdb, port, mux_states[i])

            # Delete neighbor
            print(f"Delete Neighbor on {mux_ports[0]}: {nexthops[0]}")
            self.del_neighbor(dvs, nexthops[0])

            # Add neighbor on other port:
            print(f"Add Neighbor on {mux_ports[1]} in {state} state: {new_neighbor}")
            self.add_neighbor(dvs, new_neighbor, macs[1])

            # Check route is pointing to nexthop[1] (if Active) or tunnel nexthop
            self.multi_nexthop_check(asicdb, dvs_route, route, nexthops, mux_states, expect_active=(state == ACTIVE))

            # Delete neighbor on other port:
            print(f"Delete Neighbor on {mux_ports[1]} in {state} state: {new_neighbor}")
            self.del_neighbor(dvs, new_neighbor)
            self.add_neighbor(dvs, nexthops[0], macs[0])

    def multi_nexthop_test_fdb(self, appdb, asicdb, dvs, dvs_route, route, mux_ports, nexthops, macs):
        '''
        Tests fbd updates for mux neighbors
        '''
        init_mux_states = list(itertools.product([ACTIVE, STANDBY], repeat=len(mux_ports)))

        print("Test fdb update on route with multiple mux nexthops for various mux states")
        for states in init_mux_states:
            print("Testing fdb update in states: %s, for nexthops: %s" % (str(states), str(nexthops)))

            # Set mux states
            for i,port in enumerate(mux_ports):
                self.set_mux_state(appdb, port, states[i])

            for i,nexthop in enumerate(nexthops):
                print("Triggering fdb update for %s" % (nexthop))
                # only supports testing up to 9 nexhops at the moment
                self.add_neighbor(dvs, nexthop, "00:aa:bb:cc:dd:0%d" % (i))
                self.multi_nexthop_check(asicdb, dvs_route, route, nexthops, states)

                # Reset fdb
                self.add_neighbor(dvs, nexthop, macs[i])

    def multi_nexthop_test_neighbor_unresolve(self, appdb, asicdb, dvs, dvs_route, route, mux_ports, nexthops):
        '''
        Tests deleting neighbors for a route with multiple nexthops
        '''
        print("Test setting 0 mac neighbors for route with multiple mux nexthops")
        for nexthop in nexthops:
            print("Triggering neighbor unresolved for %s" % (nexthop))
            self.add_neighbor(dvs, nexthop, "00:00:00:00:00:00")
            self.multi_nexthop_test_toggle(appdb, asicdb, dvs_route, route, mux_ports, nexthops)

    def multi_nexthop_test_neighbor_resolve(self, appdb, asicdb, dvs, dvs_route, route, mux_ports, nexthops, macs):
        '''
        Tests adding neighbors for a route with multiple nexthops
        '''
        print("Test adding neighbors for route with multiple mux nexthops")
        for i,nexthop in enumerate(nexthops):
            print("Triggering neighbor resolved for %s" % (nexthop))
            self.add_neighbor(dvs, nexthop, macs[i])
            self.multi_nexthop_test_toggle(appdb, asicdb, dvs_route, route, mux_ports, nexthops)

    def create_and_test_multi_nexthop_routes(self, dvs, dvs_route, appdb, macs, new_mac, asicdb):
        '''
        Tests case where there are multiple nexthops tied to a route
        If the nexthops are tied to a mux, then only the first active neighbor will be programmed
        If not, the route should point to a regular ECMP group
        '''
        # Routes
        route_ipv4 = "2.3.4.0/24"
        route_ipv6 = "2023::/64"
        route_B_ipv4 = "2.3.5.0/24"
        route_B_ipv6 = "2024::/64"

        # Nexthop groups
        ipv4_nexthops = [self.SERV1_IPV4, self.SERV2_IPV4]
        ipv6_nexthops = [self.SERV1_IPV6, self.SERV2_IPV6]
        new_ipv4_nexthop = self.SERV3_IPV4
        new_ipv6_nexthop = self.SERV3_IPV6

        # Neighbor IPs
        non_mux_ipv4 = "11.11.11.11"
        non_mux_ipv6 = "2222::100"
        mux_neighbor_ipv4 = "192.170.0.100"
        mux_neighbor_ipv6 = "fc02:1000:100::100"

        # Neighbor mac
        non_mux_mac = "00:aa:aa:aa:aa:aa"

        # Mux ports
        mux_ports = ["Ethernet0", "Ethernet4"]
        new_mux_port = "Ethernet8"

        for i,mac in enumerate(macs):
            self.add_neighbor(dvs, ipv4_nexthops[i], mac)
            self.add_neighbor(dvs, ipv6_nexthops[i], mac)

        self.add_neighbor(dvs, new_ipv4_nexthop, new_mac)
        self.add_neighbor(dvs, new_ipv6_nexthop, new_mac)
        self.add_neighbor(dvs, non_mux_ipv4, non_mux_mac)
        self.add_neighbor(dvs, non_mux_ipv6, non_mux_mac)

        for port in mux_ports:
            self.set_mux_state(appdb, port, ACTIVE)
        self.set_mux_state(appdb, new_mux_port, ACTIVE)

        try:
            ### These tests create route: ###

            ## Testing route changes
            dvs.add_log_marker()
            self.multi_nexthop_test_create(appdb, asicdb, dvs, dvs_route, route_ipv4, mux_ports, ipv4_nexthops)
            dvs.add_log_marker()
            self.multi_nexthop_test_create(appdb, asicdb, dvs, dvs_route, route_ipv6, mux_ports, ipv6_nexthops)
            dvs.add_log_marker()
            self.multi_nexthop_test_create(appdb, asicdb, dvs, dvs_route, route_ipv4, mux_ports, ipv4_nexthops, non_mux_ipv4)
            dvs.add_log_marker()
            self.multi_nexthop_test_create(appdb, asicdb, dvs, dvs_route, route_ipv6, mux_ports, ipv6_nexthops, non_mux_ipv6)
            dvs.add_log_marker()
            self.multi_nexthop_test_route_update_keep_size(appdb, asicdb, dvs, dvs_route, route_ipv4, mux_ports, ipv4_nexthops, new_ipv4_nexthop, new_mux_port)
            dvs.add_log_marker()
            self.multi_nexthop_test_route_update_keep_size(appdb, asicdb, dvs, dvs_route, route_ipv6, mux_ports, ipv6_nexthops, new_ipv6_nexthop, new_mux_port)
            dvs.add_log_marker()
            self.multi_nexthop_test_route_update_keep_size(appdb, asicdb, dvs, dvs_route, route_ipv4, mux_ports, ipv4_nexthops, non_mux_ipv4, None, nh_is_mux=False)
            dvs.add_log_marker()
            self.multi_nexthop_test_route_update_keep_size(appdb, asicdb, dvs, dvs_route, route_ipv6, mux_ports, ipv6_nexthops, non_mux_ipv6, None, nh_is_mux=False)
            dvs.add_log_marker()
            self.multi_nexthop_test_route_update_increase_size(appdb, asicdb, dvs, dvs_route, route_ipv4, mux_ports, ipv4_nexthops, non_mux_nexthop=non_mux_ipv4)
            dvs.add_log_marker()
            self.multi_nexthop_test_route_update_increase_size(appdb, asicdb, dvs, dvs_route, route_ipv6, mux_ports, ipv6_nexthops, non_mux_nexthop=non_mux_ipv6)
            dvs.add_log_marker()
            self.multi_nexthop_test_route_update_decrease_size(appdb, asicdb, dvs, dvs_route, route_ipv4, mux_ports, ipv4_nexthops, non_mux_nexthop=non_mux_ipv4)
            dvs.add_log_marker()
            self.multi_nexthop_test_route_update_decrease_size(appdb, asicdb, dvs, dvs_route, route_ipv6, mux_ports, ipv6_nexthops, non_mux_nexthop=non_mux_ipv6)
            dvs.add_log_marker()


            ### The following tests do not create their own routes, create before and delete after ###

            ## Testing mux neighbors that do not match mux configured ip

            # add new mux vlan neighbor and route to test
            self.add_neighbor(dvs, mux_neighbor_ipv4, macs[1])
            dvs.add_log_marker()
            self.add_neighbor(dvs, mux_neighbor_ipv6, macs[1])
            dvs.add_log_marker()
            self.add_route(dvs, route_ipv4, [self.SERV1_IPV4, mux_neighbor_ipv4])
            dvs.add_log_marker()
            self.add_route(dvs, route_ipv6, [self.SERV1_IPV6, mux_neighbor_ipv6])
            dvs.add_log_marker()

            # test
            self.multi_nexthop_test_toggle(appdb, asicdb, dvs_route, route_ipv4, mux_ports, [self.SERV1_IPV4, mux_neighbor_ipv4])
            dvs.add_log_marker()
            self.multi_nexthop_test_toggle(appdb, asicdb, dvs_route, route_ipv6, mux_ports, [self.SERV1_IPV6, mux_neighbor_ipv6])
            dvs.add_log_marker()

            # cleanup new mux vlan neighbor and route to test
            self.del_route(dvs,route_ipv4)
            dvs.add_log_marker()
            self.del_route(dvs,route_ipv6)
            dvs.add_log_marker()
            self.del_neighbor(dvs, mux_neighbor_ipv4)
            dvs.add_log_marker()
            self.del_neighbor(dvs, mux_neighbor_ipv6)
            dvs.add_log_marker()

            ## Test neighbor operations:
            # create the route
            self.add_route(dvs, route_ipv4, ipv4_nexthops)
            dvs.add_log_marker()
            self.add_route(dvs, route_ipv6, ipv6_nexthops)
            dvs.add_log_marker()
            self.add_route(dvs, route_B_ipv4, ipv4_nexthops)
            dvs.add_log_marker()
            self.add_route(dvs, route_B_ipv6, ipv6_nexthops)
            dvs.add_log_marker()

            self.multi_nexthop_test_vlan_neighbor_update(appdb, asicdb, dvs, dvs_route, route_ipv4, mux_ports, ipv4_nexthops, macs, mux_neighbor_ipv4)
            dvs.add_log_marker()
            self.multi_nexthop_test_vlan_neighbor_update(appdb, asicdb, dvs, dvs_route, route_ipv6, mux_ports, ipv6_nexthops, macs, mux_neighbor_ipv6)
            dvs.add_log_marker()

            self.multi_nexthop_test_neighbor_delete_and_create(appdb, asicdb, dvs, dvs_route, route_ipv4, mux_ports, ipv4_nexthops, macs)
            dvs.add_log_marker()
            self.multi_nexthop_test_neighbor_delete_and_create(appdb, asicdb, dvs, dvs_route, route_ipv6, mux_ports, ipv6_nexthops, macs)
            dvs.add_log_marker()

            self.multi_nexthop_test_fdb(appdb, asicdb, dvs, dvs_route, [route_ipv4, route_B_ipv4], mux_ports, ipv4_nexthops, macs)
            dvs.add_log_marker()
            self.multi_nexthop_test_fdb(appdb, asicdb, dvs, dvs_route, [route_ipv6, route_B_ipv6], mux_ports, ipv6_nexthops, macs)
            dvs.add_log_marker()
            self.multi_nexthop_test_neighbor_unresolve(appdb, asicdb, dvs, dvs_route, [route_ipv4, route_B_ipv4], mux_ports, ipv4_nexthops)
            dvs.add_log_marker()
            self.multi_nexthop_test_neighbor_unresolve(appdb, asicdb, dvs, dvs_route, [route_ipv6, route_B_ipv6], mux_ports, ipv6_nexthops)
            dvs.add_log_marker()
            self.multi_nexthop_test_neighbor_resolve(appdb, asicdb, dvs, dvs_route, [route_ipv4, route_B_ipv4], mux_ports, ipv4_nexthops, macs)
            dvs.add_log_marker()
            self.multi_nexthop_test_neighbor_resolve(appdb, asicdb, dvs, dvs_route, [route_ipv6, route_B_ipv6], mux_ports, ipv6_nexthops, macs)
            dvs.add_log_marker()

        finally:
            # Cleanup
            self.del_route(dvs,route_ipv4)
            dvs.add_log_marker()
            self.del_route(dvs,route_B_ipv4)
            dvs.add_log_marker()
            self.del_route(dvs,route_ipv6)
            dvs.add_log_marker()
            self.del_route(dvs,route_B_ipv6)
            dvs.add_log_marker()
            for neighbor in ipv4_nexthops:
                self.del_neighbor(dvs, neighbor)
            dvs.add_log_marker()
            for neighbor in ipv6_nexthops:
                self.del_neighbor(dvs, neighbor)
            dvs.add_log_marker()
            self.del_neighbor(dvs, new_ipv4_nexthop)
            dvs.add_log_marker()
            self.del_neighbor(dvs, new_ipv6_nexthop)
            dvs.add_log_marker()
            self.del_neighbor(dvs, mux_neighbor_ipv4)
            dvs.add_log_marker()
            self.del_neighbor(dvs, mux_neighbor_ipv6)
            dvs.add_log_marker()

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

        # continue to point to standby
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

    def remove_and_test_tunnel(self, configdb, asicdb, tunnel_name):
        """ Removes tunnel and checks that ASIC db is clear"""
        tunnel_table = swsscommon.Table(asicdb.db_connection, self.ASIC_TUNNEL_TABLE)
        tunnel_term_table = swsscommon.Table(asicdb.db_connection, self.ASIC_TUNNEL_TERM_ENTRIES)

        tunnels = tunnel_table.getKeys()
        tunnel_sai_obj = tunnels[0]

        _, fvs = tunnel_table.get(tunnel_sai_obj)

        # get overlay loopback interface oid to check if it is deleted with the tunnel
        overlay_infs_id = {f:v for f, v in fvs}["SAI_TUNNEL_ATTR_OVERLAY_INTERFACE"]

        configdb.delete_entry(self.CONFIG_TUNNEL_TABLE_NAME, tunnel_name)

        # wait till config will be applied
        time.sleep(5)

        assert len(tunnel_table.getKeys()) == 0
        assert len(tunnel_term_table.getKeys()) == 0
        with pytest.raises(AssertionError):
            self.check_interface_exists_in_asicdb(asicdb, overlay_infs_id)

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
            expect_neigh=False, expected_mac='00:00:00:00:00:00',
            no_host_route=True
        ):
        """
        Checks the status of neighbor entries in APPL and ASIC DB
        """
        standby_state=False
        if expect_route and expect_neigh==False:
            standby_state=True
        app_db = dvs.get_app_db()
        asic_db = dvs.get_asic_db()
        prefix = str(ip_network(neigh_ip))
        self.check_app_db_neigh_table(
            app_db, self.VLAN_1000, neigh_ip,
            mac=expected_mac, expect_entry=expect_route
        )
        if expect_route:
            if expected_mac == '00:00:00:00:00:00':
                self.check_tnl_nexthop_in_asic_db(asic_db, 1)
            else:
                # expected_nhs = neighbor nh + tunnel_nh
                self.check_tnl_nexthop_in_asic_db(asic_db, 2)
            routes = dvs_route.check_asicdb_route_entries([prefix])
            for route in routes:
                self.check_nexthop_in_asic_db(asic_db, route, standby=standby_state)
        else:
            dvs_route.check_asicdb_deleted_route_entries([prefix])
            self.check_neigh_in_asic_db(asic_db, neigh_ip, expected=expect_neigh, no_host_route=no_host_route)

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
        config_db = dvs.get_config_db()
        config_db.create_entry(
            self.CONFIG_TUNNEL_TABLE_NAME,
            self.MUX_TUNNEL_0,
            self.DEFAULT_TUNNEL_PARAMS
        )

    @pytest.fixture
    def restore_tunnel(self, dvs):
        yield
        config_db = dvs.get_config_db()
        config_db.create_entry(
            self.CONFIG_TUNNEL_TABLE_NAME,
            self.MUX_TUNNEL_0,
            self.DEFAULT_TUNNEL_PARAMS
        )

    @pytest.fixture
    def setup_peer_switch(self, dvs):
        config_db = dvs.get_config_db()
        config_db.create_entry(
            self.CONFIG_PEER_SWITCH,
            self.PEER_SWITCH_HOST,
            self.DEFAULT_PEER_SWITCH_PARAMS
        )

        yield

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

    def test_Tunnel(self, dvs, setup_tunnel, restore_tunnel, testlog, setup):
        """ test IPv4 Mux tunnel creation """
        db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        configdb = dvs.get_config_db()

        #self.cleanup_left_over(db, asicdb)
        _, _, dscp_to_tc_map_oid, tc_to_pg_map_oid = setup
        tunnel_params = self.DEFAULT_TUNNEL_PARAMS
        tunnel_params["decap_dscp_to_tc_map_id"] = dscp_to_tc_map_oid
        tunnel_params["decap_tc_to_pg_map_id"] = tc_to_pg_map_oid

        # create tunnel IPv4 tunnel
        self.create_and_test_tunnel(db, asicdb, self.MUX_TUNNEL_0, tunnel_params)
        # remove tunnel IPv4 tunnel
        self.remove_and_test_tunnel(configdb, asicdb, self.MUX_TUNNEL_0)

    def test_Peer(self, dvs, setup_peer_switch, setup_tunnel, setup, testlog):

        """ test IPv4 Mux tunnel creation """

        asicdb = dvs.get_asic_db()

        encap_tc_to_dscp_map_id, encap_tc_to_queue_map_id, _, _ = setup

        self.create_and_test_peer(asicdb, encap_tc_to_dscp_map_id, encap_tc_to_queue_map_id)

    def test_neighbor_learned_before_mux_config(self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch, setup_tunnel, testlog):
        """ test neighbors learned before mux config """
        #dvs.runcmd("swssloglevel -l INFO -c orchagent")
        test_ip_v4 = "192.168.0.110"
        test_ip_v6 = "fc02:1000::110"

        toggle_map = {"active": "standby", "standby": "active"}

        asicdb = dvs.get_asic_db()
        config_db = dvs.get_config_db()
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

        dvs.runcmd("ip neigh flush all")
        for create_route in [False, True]:
            for state in ["active", "standby"]:
                current_state = state

                # Step 1.a: add neighbor on port
                self.add_fdb(dvs, "Ethernet4", "00-00-00-11-11-11")

                self.add_neighbor(dvs, test_ip_v4, "00:00:00:11:11:11")
                # Before mux config: neighbor without prefix route (no_host_route=False)
                self.check_neigh_in_asic_db(asicdb, test_ip_v4, expected=True, no_host_route=False)

                if create_route:
                    # Step 1.b: Create a route pointing to the neighbor
                    self.add_route(dvs, "11.11.11.11/32", ["192.168.0.110"])

                # Step 2: configure mux port and verify neighbor state.
                fvs = {"server_ipv4": self.SERV2_IPV4+self.IPV4_MASK,
                    "server_ipv6": self.SERV2_IPV6+self.IPV6_MASK}
                config_db.create_entry(self.CONFIG_MUX_CABLE, "Ethernet4", fvs)
                time.sleep(1)
                self.set_mux_state(appdb, "Ethernet4", current_state)

                expect_neigh = (current_state == "active")
                print("current state: %s" % current_state)
                self.check_neighbor_state(dvs, dvs_route, test_ip_v4, 
                                        expect_route=True, expect_neigh=expect_neigh,
                                        expected_mac="00:00:00:11:11:11", no_host_route=True)
                expected_tunnel_route = (current_state == "standby")
                self.check_tunnel_route_in_app_db(dvs, [test_ip_v4+self.IPV4_MASK], expected=expected_tunnel_route)

                # Step 3: toggle mux state and verify neighbor state.
                current_state = toggle_map[current_state]
                self.set_mux_state(appdb, "Ethernet4", current_state)

                # Verify prefix-based mux neighbor state after toggle
                # expect_neigh indicates whether route should point to neighbor NH or tunnel NH
                expect_neigh = (current_state == "active")
                self.check_neighbor_state(dvs, dvs_route, test_ip_v4,
                                        expect_route=True, expect_neigh=expect_neigh,
                                        expected_mac="00:00:00:11:11:11", no_host_route=True)
                expected_tunnel_route = (current_state == "standby")
                self.check_tunnel_route_in_app_db(dvs, [test_ip_v4+self.IPV4_MASK], expected=expected_tunnel_route)

                # Step 4: toggle mux state back to initial state and verify neighbor state.
                current_state = toggle_map[current_state]
                self.set_mux_state(appdb, "Ethernet4", current_state)

                # Verify prefix-based mux neighbor state after second toggle
                # expect_neigh indicates whether route should point to neighbor NH or tunnel NH
                expect_neigh = (current_state == "active")
                expected_tunnel_route = (current_state == "standby")
                self.check_neighbor_state(dvs, dvs_route, test_ip_v4,
                                        expect_route=True, expect_neigh=expect_neigh,
                                        expected_mac="00:00:00:11:11:11", no_host_route=True)
                self.check_tunnel_route_in_app_db(dvs, [test_ip_v4+self.IPV4_MASK], expected=expected_tunnel_route)

                if create_route:
                    self.del_route(dvs, "11.11.11.11/32")
                self.del_neighbor(dvs, test_ip_v4)
                config_db.delete_entry(self.CONFIG_MUX_CABLE, "Ethernet4")
                dvs.runcmd("ip neigh flush all")
                time.sleep(1)

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

    def test_Route(self, dvs, intf_fdb_map, dvs_route, setup, setup_vlan, setup_peer_switch, setup_tunnel, setup_mux_cable, testlog):
        """ test Route entries and mux state change """

        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        mac_Ethernet0 = intf_fdb_map["Ethernet0"]
        mac_Ethernet4 = intf_fdb_map["Ethernet4"]

        self.create_and_test_route(appdb, asicdb, dvs, dvs_route, mac_Ethernet0, mac_Ethernet4)
        self.create_and_test_route_learned_before_neighbor(appdb, asicdb, dvs, dvs_route, mac_Ethernet0)

    def test_NH(self, dvs, dvs_route, intf_fdb_map, setup, setup_mux_cable,
                setup_peer_switch, setup_tunnel, testlog):
        """ test NH routes and mux state change """
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        mac = intf_fdb_map["Ethernet0"]

        # get tunnel nexthop
        self.check_tnl_nexthop_in_asic_db(asicdb, 8)

        self.create_and_test_NH_routes(appdb, asicdb, dvs, dvs_route, mac)

    def test_multi_nexthop(self, dvs, dvs_route, intf_fdb_map, neighbor_cleanup, testlog, setup):
        dvs.runcmd("swssloglevel -l INFO -c orchagent")
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        macs = [intf_fdb_map["Ethernet0"], intf_fdb_map["Ethernet4"]]
        new_mac = intf_fdb_map["Ethernet8"]

        self.create_and_test_multi_nexthop_routes(dvs, dvs_route, appdb, macs, new_mac, asicdb)

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
            ip_to_intf_map, intf_fdb_map, neighbor_cleanup, setup, setup_vlan,
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
            print(step)
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
            neighbor_cleanup, testlog
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

    def test_standalone_tunnel_route_non_mux_neighbor_zero_mac(
            self, dvs, dvs_route, setup_vlan, setup_mux_cable, setup_tunnel,
            setup_peer_switch, neighbor_cleanup, testlog
    ):
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        config_db = dvs.get_config_db()
        asicdb = dvs.get_asic_db()

        # Add Ethernet12 to VLAN1000 but without MUX cable configuration
        fvs = {"tagging_mode": "untagged"}
        config_db.create_entry("VLAN_MEMBER", "Vlan1000|Ethernet12", fvs)
        dvs.port_admin_set("Ethernet12", "up")

        # Step 1: First test that zero MAC neighbors DO create standalone tunnel routes
        non_mux_neighbor_ip = "192.168.0.150"
        self.add_neighbor(dvs, non_mux_neighbor_ip, "00:00:00:00:00:00")

        # Verify standalone tunnel route is created for zero MAC neighbor on non-MUX port
        time.sleep(2)
        non_mux_prefix = non_mux_neighbor_ip + self.IPV4_MASK
        self.check_neighbor_state(dvs, dvs_route, non_mux_neighbor_ip,
                                expect_route=True, expect_neigh=False,
                                expected_mac='00:00:00:00:00:00')

        # Step 2: Set up a MUX port (Ethernet0) in standby and create MUX neighbor
        self.set_mux_state(appdb, "Ethernet0", "standby")
        self.wait_for_mux_state(dvs, "Ethernet0", "standby")

        # Add FDB entry for Ethernet0 (MUX port)
        self.add_fdb(dvs, "Ethernet0", "00-00-00-00-00-01")

        # Add a neighbor on the MUX port to trigger tunnel route creation
        mux_neighbor_ip = self.SERV1_IPV4
        self.add_neighbor(dvs, mux_neighbor_ip, "00:00:00:00:00:01")

        # Verify that standalone tunnel route is created for MUX neighbor
        time.sleep(2)
        mux_prefix = mux_neighbor_ip + self.IPV4_MASK
        dvs_route.check_asicdb_route_entries([mux_prefix])
        rtkeys_mux = dvs_route.check_asicdb_route_entries([mux_prefix])
        self.check_nexthop_in_asic_db(asicdb, rtkeys_mux[0], True)  # Should point to tunnel

        # At this point we should have tunnel routes for both neighbors
        dvs_route.check_asicdb_route_entries([non_mux_prefix, mux_prefix])

        # Step 3: Test the scenario - update the zero MAC neighbor on non-MUX port
        # with a non-zero MAC. This triggers removeStandaloneTunnelRoute, which should
        # only remove the non-MUX neighbor's standalone tunnel route and NOT affect the MUX neighbor route
        self.add_neighbor(dvs, non_mux_neighbor_ip, "00:aa:bb:cc:dd:ee")
        time.sleep(1)

        # Step 4: Verify that:
        # a) The non-MUX neighbor now has a regular neighbor entry (no longer standalone tunnel route)
        self.check_neighbor_state(dvs, dvs_route, non_mux_neighbor_ip,
                                expect_route=False, expect_neigh=True,
                                expected_mac='00:aa:bb:cc:dd:ee',
                                no_host_route=False)

        # b) The MUX neighbor's tunnel route is still intact
        # This is the key test - the fix ensures removeStandaloneTunnelRoute
        # checks if neighbor belongs to MUX port before removing routes
        dvs_route.check_asicdb_route_entries([mux_prefix])
        rtkeys_final = dvs_route.check_asicdb_route_entries([mux_prefix])
        self.check_nexthop_in_asic_db(asicdb, rtkeys_final[0], True)  # Should still point to tunnel

        # Cleanup
        self.del_neighbor(dvs, non_mux_neighbor_ip)
        self.del_neighbor(dvs, mux_neighbor_ip)
        self.del_fdb(dvs, "00-00-00-00-00-01")
        config_db.delete_entry("VLAN_MEMBER", "Vlan1000|Ethernet12")

    def test_fdb_after_soc_neighbor_conversion_active_mux(
        self, dvs, dvs_route, setup, setup_vlan, setup_mux_cable, setup_tunnel,
        setup_peer_switch, neighbor_cleanup, testlog
    ):
        """
        Test SoC neighbor conversion to MUX neighbor when FDB event comes after neighbor is added (active MUX).
        This test verifies the convertToMuxNeighbor() API for SoC neighbor
        """
        # Setup test environment
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        dvs.get_state_db()
        dvs.get_config_db()

        dvs.runcmd("swssloglevel -l INFO -c orchagent")

        # Add MUX cable and set it to active state
        cable_name = "Ethernet0"
        self.set_mux_state(appdb, cable_name, "active")
        time.sleep(1)

        neighbor_ip = self.SERV1_SOC_IPV4  # Use SoC IP for FDB conversion test
        neighbor_mac = "00:11:22:33:44:55"

        # Step 1: Add neighbor first (before FDB learn event)
        # This neighbor will be created as a regular neighbor initially
        self.add_neighbor(dvs, neighbor_ip, neighbor_mac)
        time.sleep(1)

        # Step 2: Verify initial state: SoC neighbor should always be added as
        # prefix route
        self.check_neighbor_state(dvs, dvs_route, neighbor_ip,
                                expect_route=True, expect_neigh=True,
                                expected_mac=neighbor_mac,
                                no_host_route=True)

        # Step 3: Simulate FDB learn event (SoC neighbor MAC learned on MUX port)
        # This should trigger conversion of the existing SoC neighbor to MUX neighbor
        self.simulate_fdb_learn_notification(dvs, cable_name, neighbor_mac, vlan_name="Vlan1000")
        time.sleep(2)  # Allow time for FDB processing and neighbor conversion

        # Step 4: Verify SoC neighbor still has no_host_route flag set
        self.check_neighbor_state(dvs, dvs_route, neighbor_ip,
                                expect_route=True, expect_neigh=True,
                                expected_mac=neighbor_mac,
                                no_host_route=True)

        # Verify MUX prefix route exists and points to neighbor nexthop (active state)
        mux_prefix = neighbor_ip + "/32"
        rtkeys = dvs_route.check_asicdb_route_entries([mux_prefix])
        self.check_nexthop_in_asic_db(asicdb, rtkeys[0], False)  # Should point to neighbor NH, not tunnel

        # Step 5: Test MUX state change after fdb learn
        # Switch to standby - route should now point to tunnel
        self.set_mux_state(appdb, cable_name, "standby")
        time.sleep(1)

        # Verify route now points to tunnel nexthop
        rtkeys_standby = dvs_route.check_asicdb_route_entries([mux_prefix])
        self.check_nexthop_in_asic_db(asicdb, rtkeys_standby[0], True)  # Should point to tunnel

        # Cleanup
        self.del_neighbor(dvs, neighbor_ip)
        #self.del_fdb(dvs, neighbor_mac.replace(":", "-"))
        self.simulate_fdb_aged_notification(dvs, cable_name, neighbor_mac, vlan_name="Vlan1000")
        time.sleep(2)  # Allow time for FDB processing

    def test_fdb_soc_neighbor_active_mux(
        self, dvs, dvs_route, setup, setup_vlan, setup_mux_cable, setup_tunnel,
        setup_peer_switch, neighbor_cleanup, testlog
    ):
        """
        Test SoC neighbor when FDB event comes before neighbor is added (active MUX)
        SoC FDB learn event followed by neighbor addition.
        """
        # Setup test environment
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        dvs.get_state_db()
        dvs.get_config_db()

        # Add MUX cable and set it to active state
        cable_name = "Ethernet0"
        self.set_mux_state(appdb, cable_name, "active")
        time.sleep(1)

        neighbor_ip = self.SERV1_SOC_IPV4 # Use SoC IP
        neighbor_mac = "00:11:22:33:44:55"

        # Step 1: Simulate FDB learn event (SoC neighbor MAC learned on MUX port)
        self.simulate_fdb_learn_notification(dvs, cable_name, neighbor_mac, vlan_name="Vlan1000")
        time.sleep(2)  # Allow time for FDB processing and neighbor conversion

        # Step 2: Add SoC neighbor (after FDB learn event)
        # This neighbor will be created as a prefix neighbor
        self.add_neighbor(dvs, neighbor_ip, neighbor_mac)
        time.sleep(1)

        # Verify initial state: SoC neighbor should always be mux neighbor
        # (should have prefix route)
        self.check_neighbor_state(dvs, dvs_route, neighbor_ip,
                                expect_route=True, expect_neigh=True,
                                expected_mac=neighbor_mac,
                                no_host_route=True)

        # Verify MUX prefix route exists and points to neighbor nexthop (active state)
        mux_prefix = neighbor_ip + "/32"
        rtkeys = dvs_route.check_asicdb_route_entries([mux_prefix])
        self.check_nexthop_in_asic_db(asicdb, rtkeys[0], False)  # Should point to neighbor NH, not tunnel

        # Step 4: Test MUX state change after conversion
        # Switch to standby - route should now point to tunnel
        self.set_mux_state(appdb, cable_name, "standby")
        time.sleep(1)

        # Verify route now points to tunnel nexthop
        rtkeys_standby = dvs_route.check_asicdb_route_entries([mux_prefix])
        self.check_nexthop_in_asic_db(asicdb, rtkeys_standby[0], True)  # Should point to tunnel

        # Cleanup
        self.del_neighbor(dvs, neighbor_ip)
        self.simulate_fdb_aged_notification(dvs, cable_name, neighbor_mac, vlan_name="Vlan1000")
        time.sleep(2)  # Allow time for FDB processing

    def test_fdb_after_neighbor_conversion_active_mux(
        self, dvs, dvs_route, setup, setup_vlan, setup_mux_cable, setup_tunnel,
        setup_peer_switch, neighbor_cleanup, testlog
    ):
        """Test neighbor conversion to MUX neighbor when FDB event comes after neighbor is added (active MUX).

        This test verifies the convertToMuxNeighbor() API by:
        1. Adding a regular neighbor first (not pre-configured as MUX neighbor)
        2. Later triggering FDB learn event on MUX port
        3. Verifying neighbor gets converted to MUX neighbor with prefix route
        """
        # Setup test environment
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        dvs.runcmd("swssloglevel -l INFO -c orchagent")

        # Add MUX cable and set it to active state
        cable_name = "Ethernet0"
        self.set_mux_state(appdb, cable_name, "active")
        time.sleep(1)

        neighbor_ip = self.TEST_NEIGH1_IPV4  # Use non-pre-configured IP for FDB conversion test
        neighbor_mac = "00:11:22:33:44:55"

        # Step 1: Add neighbor first (before FDB learn event)
        # This neighbor will be created as a regular neighbor initially
        self.add_neighbor(dvs, neighbor_ip, neighbor_mac)
        time.sleep(1)

        # Verify initial state: neighbor exists but is NOT a MUX neighbor yet
        # (should have normal host route, not prefix route)
        self.check_neighbor_state(dvs, dvs_route, neighbor_ip,
                                expect_route=False, expect_neigh=True,
                                expected_mac=neighbor_mac,
                                no_host_route=False)

        # Step 2: Simulate FDB learn event (neighbor MAC learned on MUX port)
        # This should trigger conversion of the existing neighbor to MUX neighbor
        self.simulate_fdb_learn_notification(dvs, cable_name, neighbor_mac, vlan_name="Vlan1000")
        time.sleep(2)  # Allow time for FDB processing and neighbor conversion

        # Step 3: Verify neighbor has been converted to MUX neighbor
        # Should now have NO_HOST_ROUTE flag and prefix route created
        self.check_neighbor_state(dvs, dvs_route, neighbor_ip,
                                expect_route=True, expect_neigh=True,
                                expected_mac=neighbor_mac,
                                no_host_route=True)

        # Verify MUX prefix route exists and points to neighbor nexthop (active state)
        mux_prefix = neighbor_ip + "/32"
        rtkeys = dvs_route.check_asicdb_route_entries([mux_prefix])
        self.check_nexthop_in_asic_db(asicdb, rtkeys[0], False)  # Should point to neighbor NH, not tunnel

        # Step 4: Test MUX state change after conversion
        # Switch to standby - route should now point to tunnel
        self.set_mux_state(appdb, cable_name, "standby")
        time.sleep(1)

        # Verify route now points to tunnel nexthop
        rtkeys_standby = dvs_route.check_asicdb_route_entries([mux_prefix])
        self.check_nexthop_in_asic_db(asicdb, rtkeys_standby[0], True)  # Should point to tunnel

        # Cleanup
        self.del_neighbor(dvs, neighbor_ip)
        #self.del_fdb(dvs, neighbor_mac.replace(":", "-"))
        self.simulate_fdb_aged_notification(dvs, cable_name, neighbor_mac, vlan_name="Vlan1000")
        time.sleep(2)  # Allow time for FDB processing

    def test_fdb_after_neighbor_conversion_standby_mux(
        self, dvs, dvs_route, setup, setup_vlan, setup_mux_cable, setup_tunnel,
        setup_peer_switch, neighbor_cleanup, testlog
    ):
        """Test neighbor conversion to MUX neighbor when FDB event comes after neighbor is added (standby MUX).

        This test verifies the convertToMuxNeighbor() API by:
        1. Adding a regular neighbor first (not pre-configured as MUX neighbor)
        2. Setting MUX port to standby state
        3. Later triggering FDB learn event on MUX port
        4. Verifying neighbor gets converted to MUX neighbor without prefix route (standby)
        """
        # Setup test environment
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        # Add MUX cable and set it to standby state
        cable_name = "Ethernet0"
        self.set_mux_state(appdb, cable_name, "standby")
        time.sleep(1)

        neighbor_ip = self.TEST_NEIGH2_IPV4  # Use non-pre-configured IP for FDB conversion test
        neighbor_mac = "00:55:44:33:22:11"

        # Step 1: Add neighbor first (before FDB learn event)
        self.add_neighbor(dvs, neighbor_ip, neighbor_mac)
        time.sleep(1)

        # Verify initial state: regular neighbor (not MUX neighbor)
        self.check_neighbor_state(dvs, dvs_route, neighbor_ip,
                                expect_route=False, expect_neigh=True,
                                expected_mac=neighbor_mac,
                                no_host_route=False)

        # Step 2: Simulate FDB learn event - should trigger conversion
        self.simulate_fdb_learn_notification(dvs, cable_name, neighbor_mac, vlan_name="Vlan1000")
        time.sleep(2)

        # Step 3: Verify conversion to MUX neighbor in standby state
        # Should have NO_HOST_ROUTE flag and prefix route pointing to tunnel
        self.check_neighbor_state(dvs, dvs_route, neighbor_ip,
                                expect_route=True, expect_neigh=False,
                                expected_mac=neighbor_mac,
                                no_host_route=True)

        mux_prefix = neighbor_ip + "/32"
        rtkeys = dvs_route.check_asicdb_route_entries([mux_prefix])
        self.check_nexthop_in_asic_db(asicdb, rtkeys[0], True)  # Should point to tunnel (standby)

        # Step 4: Test state change to active
        self.set_mux_state(appdb, cable_name, "active")
        time.sleep(1)

        # Verify route now points to neighbor nexthop
        rtkeys_active = dvs_route.check_asicdb_route_entries([mux_prefix])
        self.check_nexthop_in_asic_db(asicdb, rtkeys_active[0], False)  # Should point to neighbor NH

        # Cleanup
        self.del_neighbor(dvs, neighbor_ip)
        self.simulate_fdb_aged_notification(dvs, cable_name, neighbor_mac, vlan_name="Vlan1000")
        time.sleep(2)  # Allow time for FDB processing

    def test_fdb_after_neighbor_multiple_conversions(
        self, dvs, dvs_route, setup, setup_vlan, setup_mux_cable, setup_tunnel,
        setup_peer_switch, neighbor_cleanup, testlog
    ):
        """Test multiple neighbors getting converted when FDB events come after neighbor additions."""
        # Setup test environment
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        # Add MUX cable and set to active
        cable_name = "Ethernet0"
        self.set_mux_state(appdb, cable_name, "active")
        time.sleep(1)

        # Multiple neighbors - use non-pre-configured IPs for FDB conversion test
        neighbors = [
            (self.TEST_NEIGH3_IPV4, "00:11:11:11:11:11"),
            (self.TEST_NEIGH4_IPV4, "00:22:22:22:22:22"),
            (self.TEST_NEIGH5_IPV4, "00:33:33:33:33:33"),
        ]

        # Step 1: Add all neighbors first (before any FDB events)
        for ip, mac in neighbors:
            self.add_neighbor(dvs, ip, mac)
        time.sleep(1)

        # Verify all are regular neighbors initially
        for ip, mac in neighbors:
            self.check_neighbor_state(dvs, dvs_route, ip,
                                    expect_route=False, expect_neigh=True,
                                    expected_mac=mac,
                                    no_host_route=False)

        # Step 2: Add FDB entries one by one and verify each conversion
        converted_neighbors = []
        for ip, mac in neighbors:
            # Add FDB entry to trigger conversion
            self.add_fdb(dvs, cable_name, mac)
            time.sleep(1)
            converted_neighbors.append((ip, mac))

            # Verify MUX prefix routes exist for all converted neighbors
            mux_prefixes = [neighbor_ip + "/32" for neighbor_ip, _ in converted_neighbors]
            dvs_route.check_asicdb_route_entries(mux_prefixes)

        # Verify number of NHs = Neighbor NHs + 1 tunnel NH
        expected_nhs = (len(converted_neighbors) + 1)
        self.check_tnl_nexthop_in_asic_db(asicdb, expected_nhs)

        # Step 3: Test bulk state change affects all converted neighbors
        self.set_mux_state(appdb, cable_name, "standby")
        time.sleep(1)

        # All should now point to tunnel
        for ip, mac in converted_neighbors:
            mux_prefix = ip + "/32"
            rtkeys = dvs_route.check_asicdb_route_entries([mux_prefix])
            self.check_nexthop_in_asic_db(asicdb, rtkeys[0], True)  # Should point to tunnel

        # Cleanup
        for ip, mac in neighbors:
            self.del_neighbor(dvs, ip)
            self.del_fdb(dvs, mac.replace(":", "-"))

    def test_fdb_after_neighbor_no_conversion_non_mux_port(
        self, dvs, dvs_route, setup, setup_vlan, setup_mux_cable, setup_tunnel,
        setup_peer_switch, neighbor_cleanup, testlog
    ):
        """Test that neighbors remain regular when FDB events come from non-MUX ports."""
        # Setup test environment
        config_db = dvs.get_config_db()

        # Setup a non-MUX VLAN member port
        config_db.create_entry("VLAN_MEMBER", "Vlan1000|Ethernet12", {"tagging_mode": "untagged"})
        time.sleep(1)

        neighbor_ip = "192.168.0.150"
        neighbor_mac = "00:aa:bb:cc:dd:ff"

        # Step 1: Add neighbor on VLAN
        self.add_neighbor(dvs, neighbor_ip, neighbor_mac)
        time.sleep(1)

        # Verify initial state: regular neighbor
        self.check_neighbor_state(dvs, dvs_route, neighbor_ip,
                                expect_route=False, expect_neigh=True,
                                expected_mac=neighbor_mac,
                                no_host_route=False)

        # Step 2: Add FDB entry for non-MUX port (Ethernet12)
        # This should NOT trigger conversion to MUX neighbor
        self.add_fdb(dvs, "Ethernet12", neighbor_mac)
        time.sleep(1)

        # Step 3: Verify neighbor remains regular (no conversion)
        self.check_neighbor_state(dvs, dvs_route, neighbor_ip,
                                expect_route=False, expect_neigh=True,
                                expected_mac=neighbor_mac,
                                no_host_route=False)

        # No MUX prefix route should be created
        self.check_neighbor_state(dvs, dvs_route, neighbor_ip,
                                expect_route=False, expect_neigh=True,
                                expected_mac=neighbor_mac,
                                no_host_route=False)

        # Cleanup
        self.del_neighbor(dvs, neighbor_ip)
        self.del_fdb(dvs, neighbor_mac.replace(":", "-"))
        config_db.delete_entry("VLAN_MEMBER", "Vlan1000|Ethernet12")

    def test_fdb_after_neighbor_early_mux_state_changes(
        self, dvs, dvs_route, setup, setup_vlan, setup_mux_cable, setup_tunnel,
        setup_peer_switch, neighbor_cleanup, testlog
    ):
        """Test MUX state changes that happen before FDB-triggered neighbor conversion."""
        # Setup test environment
        asicdb = dvs.get_asic_db()
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

        # Add MUX cable and set to active
        cable_name = "Ethernet0"
        self.set_mux_state(appdb, cable_name, "active")
        time.sleep(1)

        neighbor_ip = self.TEST_NEIGH6_IPV4  # Use non-pre-configured IP for FDB conversion test
        neighbor_mac = "00:99:88:77:66:55"

        # Step 1: Add neighbor (regular neighbor initially)
        self.add_neighbor(dvs, neighbor_ip, neighbor_mac)
        time.sleep(1)

        # Step 2: Change MUX state before FDB event
        # This should not affect the regular neighbor yet
        self.set_mux_state(appdb, cable_name, "standby")
        time.sleep(1)

        # Verify still a regular neighbor (no conversion yet)
        self.check_neighbor_state(dvs, dvs_route, neighbor_ip,
                                expect_route=False, expect_neigh=True,
                                expected_mac=neighbor_mac,
                                no_host_route=False)

        # Step 3: Now trigger FDB event - should convert and respect current MUX state (standby)
        self.add_fdb(dvs, cable_name, neighbor_mac)
        time.sleep(2)

        # Step 4: Verify conversion happened and route points to tunnel (standby state)
        self.check_neighbor_state(dvs, dvs_route, neighbor_ip,
                                expect_route=True, expect_neigh=False,
                                expected_mac=neighbor_mac,
                                no_host_route=True)

        mux_prefix = neighbor_ip + "/32"
        rtkeys = dvs_route.check_asicdb_route_entries([mux_prefix])
        self.check_nexthop_in_asic_db(asicdb, rtkeys[0], True)  # Should point to tunnel (standby)

        # Step 5: Verify subsequent MUX state changes work correctly
        self.set_mux_state(appdb, cable_name, "active")
        time.sleep(1)

        rtkeys_active = dvs_route.check_asicdb_route_entries([mux_prefix])
        self.check_nexthop_in_asic_db(asicdb, rtkeys_active[0], False)  # Should point to neighbor NH

        # Cleanup
        self.del_neighbor(dvs, neighbor_ip)
        self.del_fdb(dvs, neighbor_mac.replace(":", "-"))

    def test_warm_boot_neighbor_restore(
        self, dvs, dvs_route, setup, setup_vlan, setup_mux_cable, setup_tunnel,
        setup_peer_switch, neighbor_cleanup, testlog
    ):
        """Test neighbors could be restored to correct state based on mux state after warm boot."""
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        apdb = dvs.get_app_db()
        asicdb = dvs.get_asic_db()

        self.set_mux_state(appdb, "Ethernet0", "active")
        self.set_mux_state(appdb, "Ethernet4", "active")
        self.set_mux_state(appdb, "Ethernet8", "standby")

        self.add_fdb(dvs, "Ethernet0", "00-00-00-00-00-01")
        self.add_fdb(dvs, "Ethernet4", "00-00-00-00-00-02")
        self.add_fdb(dvs, "Ethernet8", "00-00-00-00-00-03")

        self.add_neighbor(dvs, self.SERV1_IPV4, "00:00:00:00:00:01")
        self.add_neighbor(dvs, self.SERV1_IPV6, "00:00:00:00:00:01")
        self.add_neighbor(dvs, self.NEIGH1_IPV4, "00:00:00:00:00:01")
        self.add_neighbor(dvs, self.NEIGH1_IPV6, "00:00:00:00:00:01")
        self.add_neighbor(dvs, self.SERV2_IPV4, "00:00:00:00:00:02")
        self.add_neighbor(dvs, self.SERV2_IPV6, "00:00:00:00:00:02")
        self.add_neighbor(dvs, self.NEIGH2_IPV4, "00:00:00:00:00:02")
        self.add_neighbor(dvs, self.NEIGH2_IPV6, "00:00:00:00:00:02")
        self.add_neighbor(dvs, self.SERV3_IPV4, "00:00:00:00:00:03")
        self.add_neighbor(dvs, self.SERV3_IPV6, "00:00:00:00:00:03")
        self.add_neighbor(dvs, self.NEIGH3_IPV4, "00:00:00:00:00:03")
        self.add_neighbor(dvs, self.NEIGH3_IPV6, "00:00:00:00:00:03")

        time.sleep(5)

        self.check_neigh_in_asic_db(asicdb, self.SERV1_IPV4)
        self.check_neigh_in_asic_db(asicdb, self.SERV1_IPV6)
        self.check_neigh_in_asic_db(asicdb, self.NEIGH1_IPV4)
        self.check_neigh_in_asic_db(asicdb, self.NEIGH1_IPV6)
        self.check_neigh_in_asic_db(asicdb, self.SERV2_IPV4)
        self.check_neigh_in_asic_db(asicdb, self.SERV2_IPV6)
        self.check_neigh_in_asic_db(asicdb, self.NEIGH2_IPV4)
        self.check_neigh_in_asic_db(asicdb, self.NEIGH2_IPV6)
        dvs_route.check_asicdb_route_entries(
            [
                self.SERV3_IPV4 + self.IPV4_MASK,
                self.SERV3_IPV6 + self.IPV6_MASK,
                self.NEIGH3_IPV4 + self.IPV4_MASK,
                self.NEIGH3_IPV6 + self.IPV6_MASK
            ]
        )
        # Execute the warm reboot
        dvs.runcmd("config warm_restart enable system")
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

        self.check_neigh_in_asic_db(asicdb, self.SERV1_IPV4)
        self.check_neigh_in_asic_db(asicdb, self.SERV1_IPV6)
        self.check_neigh_in_asic_db(asicdb, self.NEIGH1_IPV4)
        self.check_neigh_in_asic_db(asicdb, self.NEIGH1_IPV6)
        self.check_neigh_in_asic_db(asicdb, self.SERV2_IPV4)
        self.check_neigh_in_asic_db(asicdb, self.SERV2_IPV6)
        self.check_neigh_in_asic_db(asicdb, self.NEIGH2_IPV4)
        self.check_neigh_in_asic_db(asicdb, self.NEIGH2_IPV6)
        dvs_route.check_asicdb_route_entries(
            [
                self.SERV3_IPV4 + self.IPV4_MASK,
                self.SERV3_IPV6 + self.IPV6_MASK,
                self.NEIGH3_IPV4 + self.IPV4_MASK,
                self.NEIGH3_IPV6 + self.IPV6_MASK
            ]
        )

    def test_warm_boot_mux_state(
            self, dvs, dvs_route, setup_vlan, setup_mux_cable, setup_tunnel,
            setup_peer_switch, neighbor_cleanup, testlog
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
        dvs.runcmd(['sh', '-c', 'supervisorctl start restore_neighbors'])

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
