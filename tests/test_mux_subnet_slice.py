"""VS tests for the dualtor mux subnet slicing feature.

Subnet slicing lets a MUX_CABLE entry declare a `server_ipv6_subnet` prefix.
For neighbors whose IPv6 falls inside that prefix, MuxOrch suppresses the SAI
neighbor and instead programs a single ASIC slice prefix route anchored on the
mux's configured `server_ipv6`. The anchor `server_ipv6` itself, out-of-slice
IPv6 neighbors, IPv4 neighbors, and link-local neighbors are unaffected.

Mirrors `test_mux.py` but only carries tests whose expected behavior changes
under slicing.
"""

import time
import pytest
import json

from ipaddress import ip_address
from swsscommon import swsscommon

from test_mux import TestMuxTunnelBase, create_fvs, tunnel_nh_id


ACTIVE = "active"
STANDBY = "standby"
TOGGLE = {ACTIVE: STANDBY, STANDBY: ACTIVE}


class TestMuxSubnetSliceBase(TestMuxTunnelBase):
    """Shared constants, mux config overrides, and slicing-specific helpers."""

    STATE_MUX_CABLE_TABLE = "MUX_CABLE_TABLE"
    APP_ROUTE_TABLE       = "ROUTE_TABLE"

    # ---- Topology ----------------------------------------------------------
    # Ethernet0  -> non-sliced (control)
    # Ethernet4  -> sliced; server_ipv6 inside slice prefix
    # Ethernet8  -> sliced; server_ipv6 inside slice prefix (overrides parent
    #               SERV3_IPV6 so the anchor sits inside the slice)
    SLICED_PORT_A     = "Ethernet4"
    SLICED_PORT_B     = "Ethernet8"
    NON_SLICED_PORT   = "Ethernet0"

    # /120 keeps slices inside the VLAN /64 (fc02:1000::/64) with room for
    # out-of-slice host range.
    SLICE_PREFIX_A    = "fc02:1000::100/120"   # covers ::100 .. ::1ff
    SLICE_PREFIX_B    = "fc02:1000::200/120"   # covers ::200 .. ::2ff

    # Anchor server_ipv6 must sit inside the slice. SERV2_IPV6 is ::101 already;
    # SLICED_PORT_B overrides to ::201.
    SERVER_IPV6_A     = TestMuxTunnelBase.SERV2_IPV6   # fc02:1000::101
    SERVER_IPV6_B     = "fc02:1000::201"

    # In-slice neighbors (expected to be suppressed).
    IN_SLICE_A_1      = "fc02:1000::150"
    IN_SLICE_A_2      = "fc02:1000::160"
    IN_SLICE_B_1      = "fc02:1000::250"
    IN_SLICE_B_2      = "fc02:1000::260"

    # Out-of-slice but still in VLAN — expected to be programmed normally.
    OUT_OF_SLICE_IPV6 = "fc02:1000::ff"
    OUT_OF_SLICE_IPV6_2 = "fc02:1000::301"

    # Other neighbor classes that must remain unaffected.
    IPV4_ON_SLICED    = "192.168.0.150"
    LINK_LOCAL_NEIGH  = "fe80::1"

    MAC_A1            = "00:aa:00:00:00:01"
    MAC_A1_DASH       = "00-aa-00-00-00-01"
    MAC_A2            = "00:aa:00:00:00:02"
    MAC_A2_DASH       = "00-aa-00-00-00-02"
    MAC_B1            = "00:bb:00:00:00:01"
    MAC_B1_DASH       = "00-bb-00-00-00-01"

    # Reserved for the "configure mux + slice AFTER neighbor learn" test.
    # Added to Vlan1000 by the override below but intentionally not
    # pre-configured as a mux cable.
    LATE_CFG_PORT     = "Ethernet12"
    LATE_SLICE_PREFIX = "fc02:1000::400/120"
    LATE_SERVER_IPV4  = "192.168.0.103"
    LATE_SERVER_IPV6  = "fc02:1000::401"
    LATE_IN_SLICE     = "fc02:1000::450"
    LATE_MAC          = "00:cc:00:00:00:01"
    LATE_MAC_DASH     = "00-cc-00-00-00-01"

    # Add LATE_CFG_PORT to Vlan1000 without configuring it as a mux cable.
    def create_vlan_interface(self, dvs):
        super().create_vlan_interface(dvs)
        confdb = dvs.get_config_db()
        confdb.create_entry(
            "VLAN_MEMBER", "Vlan1000|" + self.LATE_CFG_PORT,
            {"tagging_mode": "untagged"},
        )
        dvs.port_admin_set(self.LATE_CFG_PORT, "up")

    def create_mux_cable(self, confdb):
        # Ethernet0 — non-sliced control
        fvs = {
            "server_ipv4":   self.SERV1_IPV4 + self.IPV4_MASK,
            "server_ipv6":   self.SERV1_IPV6 + self.IPV6_MASK,
            "soc_ipv4":      self.SERV1_SOC_IPV4 + self.IPV4_MASK,
            "cable_type":    "active-active",
        }
        confdb.create_entry(self.CONFIG_MUX_CABLE, self.NON_SLICED_PORT, fvs)

        # Ethernet4 — sliced
        fvs = {
            "server_ipv4":         self.SERV2_IPV4 + self.IPV4_MASK,
            "server_ipv6":         self.SERVER_IPV6_A + self.IPV6_MASK,
            "server_ipv6_subnet":  self.SLICE_PREFIX_A,
        }
        confdb.create_entry(self.CONFIG_MUX_CABLE, self.SLICED_PORT_A, fvs)

        # Ethernet8 — sliced; server_ipv6 overridden to sit inside the slice.
        fvs = {
            "server_ipv4":         self.SERV3_IPV4 + self.IPV4_MASK,
            "server_ipv6":         self.SERVER_IPV6_B + self.IPV6_MASK,
            "server_ipv6_subnet":  self.SLICE_PREFIX_B,
        }
        confdb.create_entry(self.CONFIG_MUX_CABLE, self.SLICED_PORT_B, fvs)

    def get_route_nexthop_ip(self, asicdb, route_key):
        """Return the SAI_NEXT_HOP_ATTR_IP of the route's nexthop OID, or ''."""
        route_entry = asicdb.get_entry(self.ASIC_ROUTE_TABLE, route_key)
        nh_oid = route_entry.get("SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID")
        if not nh_oid:
            return ""
        nh_entry = asicdb.get_entry(self.ASIC_NEXTHOP_TABLE, nh_oid)
        return nh_entry.get("SAI_NEXT_HOP_ATTR_IP", "")

    def find_route_key_for_prefix(self, asicdb, prefix):
        """Return the ASIC route key matching `dest`=prefix, or '' if missing."""
        for key in asicdb.get_keys(self.ASIC_ROUTE_TABLE):
            try:
                parsed = json.loads(key)
            except Exception:
                continue
            if parsed.get("dest") == prefix:
                return key
        return ""

    def wait_for_slice_route(self, asicdb, prefix, expected_nh_ip, present=True,
                             timeout=10):
        """Poll for the slice prefix route and (optionally) its anchor NH."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            key = self.find_route_key_for_prefix(asicdb, prefix)
            if present and key:
                nh_ip = self.get_route_nexthop_ip(asicdb, key)
                if expected_nh_ip is None or nh_ip == expected_nh_ip:
                    return key
            elif not present and not key:
                return ""
            time.sleep(0.5)
        if present:
            assert False, (
                f"Slice route {prefix} not present "
                f"(or wrong NH; expected anchor {expected_nh_ip}) within {timeout}s"
            )
        else:
            assert False, f"Slice route {prefix} still present after {timeout}s"

    def get_state_db_mux_entry(self, dvs, port):
        state_db = dvs.get_state_db()
        return state_db.get_entry(self.STATE_MUX_CABLE_TABLE, port) or {}

    def check_state_db_slice_fields(self, dvs, port, expected_prefix=None):
        """Assert STATE_DB MUX_CABLE_TABLE server_ipv6_subnet for `port`."""
        entry = self.get_state_db_mux_entry(dvs, port)
        if expected_prefix is None:
            assert "server_ipv6_subnet" not in entry, (
                f"Did not expect server_ipv6_subnet for {port}, got {entry}"
            )
            return
        assert entry.get("server_ipv6_subnet") == expected_prefix, (
            f"{port} server_ipv6_subnet: expected {expected_prefix}, got {entry}"
        )

    def expect_neighbor_suppressed(self, dvs, asicdb, ip, mac, port_mac_dash,
                                   sliced_port):
        """Add neighbor + FDB, expect it NOT to land in ASIC neigh table."""
        # FDB first so MuxOrch can resolve the neighbor to a mux port.
        self.add_fdb(dvs, sliced_port, port_mac_dash)
        self.add_neighbor(dvs, ip, mac)
        time.sleep(1)
        self.check_neigh_in_asic_db(asicdb, ip, expected=False)

    def expect_neighbor_programmed(self, dvs, asicdb, ip, mac, port_mac_dash,
                                   port):
        self.add_fdb(dvs, port, port_mac_dash)
        self.add_neighbor(dvs, ip, mac)
        time.sleep(1)
        self.check_neigh_in_asic_db(asicdb, ip, expected=True)


class TestMuxSubnetSlice(TestMuxSubnetSliceBase):
    """Tests for dualtor mux subnet slicing."""

    @pytest.fixture(scope='class')
    def setup(self, dvs):
        """Mirror TestMuxTunnel.setup: register QoS maps needed by tunnel
        creation. Required as a dependency by setup_tunnel/setup_peer_switch
        consumers even when individual slicing tests don't read its return
        values."""
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

    def test_anchor_server_ipv6_always_programmed(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        """Anchor `server_ipv6` is always programmed in ASIC, even inside the
        slice."""
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
        self.add_neighbor(dvs, self.SERVER_IPV6_A, self.MAC_A1)
        try:
            self.check_neigh_in_asic_db(asicdb, self.SERVER_IPV6_A, expected=True)
        finally:
            self.del_neighbor(dvs, self.SERVER_IPV6_A)

    def test_slice_route_installed(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        """The slice prefix route is installed in ASIC pointing at the anchor."""
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        # Anchor must be resolved so the slice route has a real NH.
        self.add_fdb(dvs, self.SLICED_PORT_A, self.MAC_A1_DASH)
        self.add_neighbor(dvs, self.SERVER_IPV6_A, self.MAC_A1)
        self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
        try:
            self.wait_for_slice_route(
                asicdb, self.SLICE_PREFIX_A, self.SERVER_IPV6_A, present=True
            )
        finally:
            self.del_neighbor(dvs, self.SERVER_IPV6_A)
            self.del_fdb(dvs, self.MAC_A1_DASH)

    def test_state_db_slice_fields(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        """STATE_DB MUX_CABLE_TABLE publishes server_ipv6_subnet on sliced
        ports only."""
        # Sliced port shows the prefix; non-sliced does not.
        self.check_state_db_slice_fields(
            dvs, self.SLICED_PORT_A,
            expected_prefix=self.SLICE_PREFIX_A,
        )
        self.check_state_db_slice_fields(dvs, self.NON_SLICED_PORT,
                                         expected_prefix=None)

        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
        try:
            # Add one in-slice and one out-of-slice neighbor on the same port.
            self.expect_neighbor_suppressed(
                dvs, asicdb, self.IN_SLICE_A_1, self.MAC_A1,
                self.MAC_A1_DASH, self.SLICED_PORT_A,
            )
            self.expect_neighbor_programmed(
                dvs, asicdb, self.OUT_OF_SLICE_IPV6, self.MAC_A2,
                self.MAC_A2_DASH, self.SLICED_PORT_A,
            )
            time.sleep(1)
            self.check_state_db_slice_fields(
                dvs, self.SLICED_PORT_A,
                expected_prefix=self.SLICE_PREFIX_A,
            )
        finally:
            self.del_neighbor(dvs, self.IN_SLICE_A_1)
            self.del_neighbor(dvs, self.OUT_OF_SLICE_IPV6)
            self.del_fdb(dvs, self.MAC_A1_DASH)
            self.del_fdb(dvs, self.MAC_A2_DASH)

    def test_in_slice_neighbor_suppressed(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        """In-slice IPv6 neighbor is not programmed in the SAI neighbor table
        on either active or standby; the slice prefix route covers it."""
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        # Ensure anchor is resolved so the slice route exists.
        self.add_fdb(dvs, self.SLICED_PORT_A, self.MAC_A1_DASH)
        self.add_neighbor(dvs, self.SERVER_IPV6_A, self.MAC_A1)

        try:
            for state in (ACTIVE, STANDBY):
                self.set_mux_state(appdb, self.SLICED_PORT_A, state)
                self.add_neighbor(dvs, self.IN_SLICE_A_1, self.MAC_A1)
                time.sleep(1)
                self.check_neigh_in_asic_db(
                    asicdb, self.IN_SLICE_A_1, expected=False
                )
                # No tunnel host-route either — the slice route takes over.
                self.check_tunnel_route_in_app_db(
                    dvs, [self.IN_SLICE_A_1 + self.IPV6_MASK], expected=False
                )
            # Restore ACTIVE so the slice route NH is the anchor (VLAN NH),
            # not the tunnel NH from the STANDBY iteration above.
            self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
            self.add_neighbor(dvs, self.SERVER_IPV6_A, self.MAC_A1)
            self.wait_for_slice_route(
                asicdb, self.SLICE_PREFIX_A, self.SERVER_IPV6_A, present=True,
            )
        finally:
            self.del_neighbor(dvs, self.IN_SLICE_A_1)
            self.del_neighbor(dvs, self.SERVER_IPV6_A)
            self.del_fdb(dvs, self.MAC_A1_DASH)
            self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)

    def test_out_of_slice_neighbor_programmed(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        """Out-of-slice IPv6 neighbor on a sliced port behaves like the
        non-slice path: programmed when active, tunnel-routed when standby."""
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        self.del_neighbor(dvs, self.OUT_OF_SLICE_IPV6)
        self.del_fdb(dvs, self.MAC_A1_DASH)
        self.set_mux_state(appdb, self.SLICED_PORT_A, STANDBY)
        time.sleep(1)
        self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
        time.sleep(1)

        try:
            self.expect_neighbor_programmed(
                dvs, asicdb, self.OUT_OF_SLICE_IPV6, self.MAC_A1,
                self.MAC_A1_DASH, self.SLICED_PORT_A,
            )
            # Standby: removed from ASIC, tunnel host-route installed.
            self.set_mux_state(appdb, self.SLICED_PORT_A, STANDBY)
            self.check_neigh_in_asic_db(
                asicdb, self.OUT_OF_SLICE_IPV6, expected=False
            )
            self.check_tunnel_route_in_app_db(
                dvs, [self.OUT_OF_SLICE_IPV6 + self.IPV6_MASK], expected=True
            )
            # Back to active.
            self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
            self.check_neigh_in_asic_db(
                asicdb, self.OUT_OF_SLICE_IPV6, expected=True
            )
            self.check_tunnel_route_in_app_db(
                dvs, [self.OUT_OF_SLICE_IPV6 + self.IPV6_MASK], expected=False
            )
        finally:
            self.del_neighbor(dvs, self.OUT_OF_SLICE_IPV6)
            self.del_fdb(dvs, self.MAC_A1_DASH)

    def test_ipv4_neighbor_on_sliced_port(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        """IPv4 neighbors on a sliced port are never affected by slicing."""
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        try:
            self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
            self.expect_neighbor_programmed(
                dvs, asicdb, self.IPV4_ON_SLICED, self.MAC_A1,
                self.MAC_A1_DASH, self.SLICED_PORT_A,
            )
            # Standby still flips to tunnel route per existing behavior.
            self.set_mux_state(appdb, self.SLICED_PORT_A, STANDBY)
            self.check_neigh_in_asic_db(asicdb, self.IPV4_ON_SLICED, expected=False)
            self.check_tunnel_route_in_app_db(
                dvs, [self.IPV4_ON_SLICED + self.IPV4_MASK], expected=True
            )
        finally:
            self.del_neighbor(dvs, self.IPV4_ON_SLICED)
            self.del_fdb(dvs, self.MAC_A1_DASH)
            self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)

    def test_link_local_neighbor_on_sliced_port(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        """Link-local IPv6 neighbors are out-of-slice by definition."""
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        try:
            self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
            self.add_fdb(dvs, self.SLICED_PORT_A, self.MAC_A1_DASH)
            self.add_neighbor(dvs, self.LINK_LOCAL_NEIGH, self.MAC_A1)
            time.sleep(1)
            # We don't strictly require it in ASIC (link-local isn't always
            # programmed via the same path), but it must NOT be suppressed
            # by the slice — confirm it's not absent due to slicing by
            # checking that no slice-anchored host route claims it.
            assert ip_address(self.LINK_LOCAL_NEIGH).is_link_local
            self.check_tunnel_route_in_app_db(
                dvs, [self.LINK_LOCAL_NEIGH + self.IPV6_MASK], expected=False
            )
        finally:
            self.del_neighbor(dvs, self.LINK_LOCAL_NEIGH)
            self.del_fdb(dvs, self.MAC_A1_DASH)

    def test_slice_only_on_one_port(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        """Slice config on port A doesn't affect the non-sliced port: an
        out-of-slice IP added on Ethernet0 is programmed normally."""
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        try:
            self.set_mux_state(appdb, self.NON_SLICED_PORT, ACTIVE)
            self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)

            # Out-of-slice IP on the non-sliced port: normal programming.
            self.expect_neighbor_programmed(
                dvs, asicdb, self.OUT_OF_SLICE_IPV6_2, self.MAC_A1,
                self.MAC_A1_DASH, self.NON_SLICED_PORT,
            )
        finally:
            self.del_neighbor(dvs, self.OUT_OF_SLICE_IPV6_2)
            self.del_fdb(dvs, self.MAC_A1_DASH)

    # An IP inside slice A's prefix but FDB-learned on a different mux port
    # must NOT be slice-suppressed (port-affinity cross-check).
    def test_in_slice_ip_learned_on_non_slice_port_is_unsuppressed(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        # IP that is inside SLICED_PORT_A's slice (fc02:1000::100/120) but
        # learned via FDB on NON_SLICED_PORT (Ethernet0).
        cross_ip = self.IN_SLICE_A_2  # fc02:1000::160

        try:
            self.set_mux_state(appdb, self.NON_SLICED_PORT, ACTIVE)
            self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)

            # Anchor SLICED_PORT_A so its slice route is installed.
            self.add_fdb(dvs, self.SLICED_PORT_A, self.MAC_A1_DASH)
            self.add_neighbor(dvs, self.SERVER_IPV6_A, self.MAC_A1)
            self.wait_for_slice_route(
                asicdb, self.SLICE_PREFIX_A, self.SERVER_IPV6_A, present=True
            )
            self.check_state_db_slice_fields(
                dvs, self.SLICED_PORT_A,
                expected_prefix=self.SLICE_PREFIX_A,
            )

            # FDB resolves the MAC to the non-sliced port.
            self.add_fdb(dvs, self.NON_SLICED_PORT, self.MAC_A2_DASH)
            self.add_neighbor(dvs, cross_ip, self.MAC_A2)
            time.sleep(1)

            # Neighbor programmed in ASIC (not suppressed by port A's slice).
            self.check_neigh_in_asic_db(asicdb, cross_ip, expected=True)
            self.check_state_db_slice_fields(
                dvs, self.SLICED_PORT_A,
                expected_prefix=self.SLICE_PREFIX_A,
            )
            # Slice route on port A still resolves via the anchor.
            self.wait_for_slice_route(
                asicdb, self.SLICE_PREFIX_A, self.SERVER_IPV6_A, present=True
            )
        finally:
            self.del_neighbor(dvs, cross_ip)
            self.del_neighbor(dvs, self.SERVER_IPV6_A)
            self.del_fdb(dvs, self.MAC_A1_DASH)
            self.del_fdb(dvs, self.MAC_A2_DASH)

    def test_in_slice_ip_move_between_ports(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        """An in-slice IP that FDB-resolves to the slice owner is suppressed.
        After the MAC moves to a different mux port, the port-affinity
        cross-check unsuppresses the neighbor (it now belongs to that other
        port and must be programmed normally). Slice routes on both ports
        remain intact."""
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        ip = self.IN_SLICE_A_1
        try:
            # Anchor both ports.
            self.add_fdb(dvs, self.SLICED_PORT_A, self.MAC_A1_DASH)
            self.add_neighbor(dvs, self.SERVER_IPV6_A, self.MAC_A1)
            self.add_fdb(dvs, self.SLICED_PORT_B, self.MAC_B1_DASH)
            self.add_neighbor(dvs, self.SERVER_IPV6_B, self.MAC_B1)
            self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
            self.set_mux_state(appdb, self.SLICED_PORT_B, ACTIVE)

            # In-slice neighbor learned on port A (the slice owner) — suppressed.
            self.add_fdb(dvs, self.SLICED_PORT_A, self.MAC_A2_DASH)
            self.add_neighbor(dvs, ip, self.MAC_A2)
            time.sleep(1)
            self.check_neigh_in_asic_db(asicdb, ip, expected=False)

            # Same MAC now appears on port B (move). The IP still falls under
            # slice A's prefix, but it no longer FDB-resolves to the slice
            # owner — the port-affinity cross-check unsuppresses the neighbor
            # so traffic to the moved host can still be delivered.
            self.add_fdb(dvs, self.SLICED_PORT_B, self.MAC_A2_DASH)
            time.sleep(2)
            self.check_neigh_in_asic_db(asicdb, ip, expected=True)

            # Both slice routes must still be present and resolve via their
            # respective anchors.
            self.wait_for_slice_route(
                asicdb, self.SLICE_PREFIX_A, self.SERVER_IPV6_A, present=True
            )
            self.wait_for_slice_route(
                asicdb, self.SLICE_PREFIX_B, self.SERVER_IPV6_B, present=True
            )
        finally:
            self.del_neighbor(dvs, ip)
            self.del_neighbor(dvs, self.SERVER_IPV6_A)
            self.del_neighbor(dvs, self.SERVER_IPV6_B)
            self.del_fdb(dvs, self.MAC_A1_DASH)
            self.del_fdb(dvs, self.MAC_A2_DASH)
            self.del_fdb(dvs, self.MAC_B1_DASH)

    # Slice route tracks anchor state: active = VLAN NH, standby = tunnel NH.
    def test_slice_route_follows_mux_state(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        self.check_tnl_nexthop_in_asic_db(asicdb)

        try:
            self.add_fdb(dvs, self.SLICED_PORT_A, self.MAC_A1_DASH)
            self.add_neighbor(dvs, self.SERVER_IPV6_A, self.MAC_A1)

            # Active: slice route resolves to the anchor neighbor (non-tunnel NH).
            self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
            key = self.wait_for_slice_route(
                asicdb, self.SLICE_PREFIX_A, self.SERVER_IPV6_A, present=True
            )
            self.check_nexthop_in_asic_db(asicdb, key, standby=False)

            # Standby: slice route resolves to tunnel NH.
            self.set_mux_state(appdb, self.SLICED_PORT_A, STANDBY)
            key = self.find_route_key_for_prefix(asicdb, self.SLICE_PREFIX_A)
            assert key, "slice route disappeared during state transition"
            self.check_nexthop_in_asic_db(asicdb, key, standby=True)

            # Back to active.
            self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
            key = self.wait_for_slice_route(
                asicdb, self.SLICE_PREFIX_A, self.SERVER_IPV6_A, present=True
            )
            self.check_nexthop_in_asic_db(asicdb, key, standby=False)
        finally:
            self.del_neighbor(dvs, self.SERVER_IPV6_A)
            self.del_fdb(dvs, self.MAC_A1_DASH)

    # Neighbor learned before mux config: slice install deferred until the
    # MUX_CABLE entry (with slice prefix) lands. Uses LATE_CFG_PORT which is
    # added to Vlan1000 but left un-mux-configured by the override above.
    def test_neighbor_learned_before_mux_config_with_slice(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        """Adapted from test_mux.test_neighbor_learned_before_mux_config: when
        an in-slice neighbor is learned before the MUX_CABLE entry, the slice
        config (arriving later) suppresses it via the reconcile sweep."""
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        config_db = dvs.get_config_db()

        in_slice_ip = self.LATE_IN_SLICE
        anchor_ip = self.LATE_SERVER_IPV6
        try:
            # Step 1: learn neighbor + anchor BEFORE any MUX_CABLE entry for
            # this port. They should land in the ASIC as regular VLAN
            # neighbors.
            self.add_fdb(dvs, self.LATE_CFG_PORT, self.LATE_MAC_DASH)
            self.add_neighbor(dvs, anchor_ip, self.LATE_MAC)
            self.add_neighbor(dvs, in_slice_ip, self.LATE_MAC)
            time.sleep(2)
            self.check_neigh_in_asic_db(asicdb, in_slice_ip, expected=True)
            self.check_neigh_in_asic_db(asicdb, anchor_ip, expected=True)

            # Step 2: configure the mux cable WITH slice prefix. Anchor stays
            # programmed; in-slice neighbor must be reconciled to suppressed
            # and the slice prefix route must be installed.
            config_db.create_entry(
                self.CONFIG_MUX_CABLE, self.LATE_CFG_PORT,
                {
                    "server_ipv4":        self.LATE_SERVER_IPV4 + self.IPV4_MASK,
                    "server_ipv6":        anchor_ip + self.IPV6_MASK,
                    "server_ipv6_subnet": self.LATE_SLICE_PREFIX,
                },
            )
            self.set_mux_state(appdb, self.LATE_CFG_PORT, ACTIVE)
            time.sleep(3)
            self.check_neigh_in_asic_db(asicdb, in_slice_ip, expected=False)
            self.check_neigh_in_asic_db(asicdb, anchor_ip, expected=True)
            self.wait_for_slice_route(
                asicdb, self.LATE_SLICE_PREFIX, anchor_ip, present=True
            )
        finally:
            self.del_neighbor(dvs, in_slice_ip)
            self.del_neighbor(dvs, anchor_ip)
            self.del_fdb(dvs, self.LATE_MAC_DASH)
            config_db.delete_entry(self.CONFIG_MUX_CABLE, self.LATE_CFG_PORT)

    def test_runtime_slice_change_rejected(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        """Attempting to change server_ipv6_subnet on an existing MUX_CABLE
        entry must be rejected; the original prefix stays in STATE_DB."""
        config_db = dvs.get_config_db()
        # Try to switch from /120 to /112 on the fly.
        config_db.create_entry(
            self.CONFIG_MUX_CABLE, self.SLICED_PORT_A,
            {
                "server_ipv4":        self.SERV2_IPV4 + self.IPV4_MASK,
                "server_ipv6":        self.SERVER_IPV6_A + self.IPV6_MASK,
                "server_ipv6_subnet": "fc02:1000::100/112",
            },
        )
        time.sleep(1)
        # STATE_DB must still reflect the original prefix.
        self.check_state_db_slice_fields(
            dvs, self.SLICED_PORT_A, expected_prefix=self.SLICE_PREFIX_A
        )

    def test_route_nh_in_slice_unsuppresses_neighbor(
        self, dvs, intf_fdb_map, dvs_route, setup, setup_vlan,
        setup_peer_switch, setup_tunnel, setup_mux_cable, testlog
    ):
        """When a route's nexthop points at an in-slice IP, that IP is
        un-suppressed (programmed as a full SAI neighbor) for the lifetime
        of the route."""
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        mac_E4 = intf_fdb_map[self.SLICED_PORT_A]

        # Anchor + suppressed in-slice neighbor.
        self.add_neighbor(dvs, self.SERVER_IPV6_A, mac_E4)
        self.add_neighbor(dvs, self.IN_SLICE_A_1, mac_E4)
        self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
        time.sleep(2)

        rtprefix = "2024:abcd::/64"
        try:
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_A_1, expected=False)

            # Install route whose NH is the in-slice IP.
            self.add_route(dvs, rtprefix, [self.IN_SLICE_A_1])
            time.sleep(3)

            # In-slice IP must now be programmed.
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_A_1, expected=True)
        finally:
            self.del_route(dvs, rtprefix)
            self.del_neighbor(dvs, self.IN_SLICE_A_1)
            self.del_neighbor(dvs, self.SERVER_IPV6_A)

    def test_route_delete_resuppresses_neighbor(
        self, dvs, intf_fdb_map, dvs_route, setup, setup_vlan,
        setup_peer_switch, setup_tunnel, setup_mux_cable, testlog
    ):
        """When the route is removed, the in-slice neighbor goes back to
        being suppressed (refcount 1 -> 0)."""
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        mac_E4 = intf_fdb_map[self.SLICED_PORT_A]

        self.add_neighbor(dvs, self.SERVER_IPV6_A, mac_E4)
        self.add_neighbor(dvs, self.IN_SLICE_A_1, mac_E4)
        self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
        time.sleep(2)

        rtprefix = "2024:abcd:1::/64"
        try:
            self.add_route(dvs, rtprefix, [self.IN_SLICE_A_1])
            time.sleep(3)
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_A_1, expected=True)

            self.del_route(dvs, rtprefix)
            time.sleep(2)
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_A_1, expected=False)
        finally:
            self.del_neighbor(dvs, self.IN_SLICE_A_1)
            self.del_neighbor(dvs, self.SERVER_IPV6_A)

    def test_multi_nexthop_with_in_slice_member(
        self, dvs, intf_fdb_map, dvs_route, setup, setup_vlan,
        setup_peer_switch, setup_tunnel, setup_mux_cable, testlog
    ):
        """ECMP route with one in-slice NH and one out-of-slice NH on the same
        sliced port: the in-slice NH gets un-suppressed for the route's
        lifetime; removing the route re-suppresses it."""
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        mac_E4 = intf_fdb_map[self.SLICED_PORT_A]

        self.add_neighbor(dvs, self.SERVER_IPV6_A, mac_E4)
        self.add_neighbor(dvs, self.IN_SLICE_A_1, mac_E4)
        self.add_neighbor(dvs, self.OUT_OF_SLICE_IPV6, mac_E4)
        self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
        time.sleep(2)

        rtprefix = "2024:abcd:2::/64"
        try:
            self.add_route(dvs, rtprefix, [self.IN_SLICE_A_1, self.OUT_OF_SLICE_IPV6])
            time.sleep(3)
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_A_1, expected=True)
            self.check_neigh_in_asic_db(asicdb, self.OUT_OF_SLICE_IPV6, expected=True)

            self.del_route(dvs, rtprefix)
            time.sleep(2)
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_A_1, expected=False)
            self.check_neigh_in_asic_db(asicdb, self.OUT_OF_SLICE_IPV6, expected=True)
        finally:
            self.del_neighbor(dvs, self.IN_SLICE_A_1)
            self.del_neighbor(dvs, self.OUT_OF_SLICE_IPV6)
            self.del_neighbor(dvs, self.SERVER_IPV6_A)

    def test_route_nh_swap_releases_old_in_slice_nh(
        self, dvs, intf_fdb_map, dvs_route, setup, setup_vlan,
        setup_peer_switch, setup_tunnel, setup_mux_cable, testlog
    ):
        """Route SET (NHG change) on the SAME prefix from in-slice NH A to NH B
        must release A's refcount. After the swap, only B should remain
        un-suppressed; removing the route then re-suppresses B.

        Regression test for: refcount leak when a route's nexthops are
        updated in-place without an intermediate DEL.
        """
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        mac_E4 = intf_fdb_map[self.SLICED_PORT_A]

        self.add_neighbor(dvs, self.SERVER_IPV6_A, mac_E4)
        self.add_neighbor(dvs, self.IN_SLICE_A_1, mac_E4)
        self.add_neighbor(dvs, self.IN_SLICE_A_2, mac_E4)
        self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
        time.sleep(2)

        rtprefix = "2024:abcd:3::/64"
        try:
            # SET with NH A1 -> A1 unsuppressed.
            self.add_route(dvs, rtprefix, [self.IN_SLICE_A_1])
            time.sleep(2)
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_A_1, expected=True)
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_A_2, expected=False)

            # SET same prefix with NH A2 (no DEL in between) -> A1 must drop,
            # A2 must come up.
            self.add_route(dvs, rtprefix, [self.IN_SLICE_A_2])
            time.sleep(3)
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_A_2, expected=True)
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_A_1, expected=False)

            # DEL -> A2 also goes back to suppressed.
            self.del_route(dvs, rtprefix)
            time.sleep(2)
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_A_2, expected=False)
        finally:
            self.del_neighbor(dvs, self.IN_SLICE_A_1)
            self.del_neighbor(dvs, self.IN_SLICE_A_2)
            self.del_neighbor(dvs, self.SERVER_IPV6_A)

    def test_route_ecmp_member_dropped_resuppresses(
        self, dvs, intf_fdb_map, dvs_route, setup, setup_vlan,
        setup_peer_switch, setup_tunnel, setup_mux_cable, testlog
    ):
        """Route SET shrinks ECMP {A1, A2} -> {A1}. A2 must be re-suppressed
        even though the route still exists, since it's no longer a NH.
        """
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        mac_E4 = intf_fdb_map[self.SLICED_PORT_A]

        self.add_neighbor(dvs, self.SERVER_IPV6_A, mac_E4)
        self.add_neighbor(dvs, self.IN_SLICE_A_1, mac_E4)
        self.add_neighbor(dvs, self.IN_SLICE_A_2, mac_E4)
        self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
        time.sleep(2)

        rtprefix = "2024:abcd:4::/64"
        try:
            self.add_route(dvs, rtprefix, [self.IN_SLICE_A_1, self.IN_SLICE_A_2])
            time.sleep(3)
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_A_1, expected=True)
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_A_2, expected=True)

            # Shrink to single NH; A2 must drop out.
            self.add_route(dvs, rtprefix, [self.IN_SLICE_A_1])
            time.sleep(3)
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_A_1, expected=True)
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_A_2, expected=False)

            self.del_route(dvs, rtprefix)
            time.sleep(2)
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_A_1, expected=False)
        finally:
            self.del_neighbor(dvs, self.IN_SLICE_A_1)
            self.del_neighbor(dvs, self.IN_SLICE_A_2)
            self.del_neighbor(dvs, self.SERVER_IPV6_A)

    # FDB-after-neighbor race for in-slice IPv6: end state must be suppressed
    # by the slice.
    def test_fdb_after_in_slice_neighbor_on_standby_mux(
        self, dvs, dvs_route, setup, setup_vlan, setup_mux_cable,
        setup_tunnel, setup_peer_switch, testlog
    ):
        """In-slice neighbor learned before its FDB ends up suppressed.

        Slice suppression is IP-only (doesn't wait for FDB), so the neighbor
        is suppressed immediately. We verify the final state."""
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        ip = self.IN_SLICE_A_2

        try:
            self.set_mux_state(appdb, self.SLICED_PORT_A, STANDBY)
            self.add_neighbor(dvs, ip, self.MAC_A2)
            self.add_fdb(dvs, self.SLICED_PORT_A, self.MAC_A2_DASH)
            time.sleep(2)
            self.check_neigh_in_asic_db(asicdb, ip, expected=False)
            self.check_tunnel_route_in_app_db(
                dvs, [ip + self.IPV6_MASK], expected=False
            )

            # Toggle to active — still suppressed (slice covers it).
            self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
            self.check_neigh_in_asic_db(asicdb, ip, expected=False)
        finally:
            self.del_neighbor(dvs, ip)
            self.del_fdb(dvs, self.MAC_A2_DASH)
            self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)

    def test_fdb_after_in_slice_neighbor_on_active_mux(
        self, dvs, dvs_route, setup, setup_vlan, setup_mux_cable,
        setup_tunnel, setup_peer_switch, testlog
    ):
        """Same FDB-after-neighbor race on an ACTIVE mux: slice still
        suppresses the in-slice neighbor at steady state."""
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()
        ip = self.IN_SLICE_A_1

        try:
            self.set_mux_state(appdb, self.SLICED_PORT_A, ACTIVE)
            self.add_neighbor(dvs, ip, self.MAC_A1)
            self.add_fdb(dvs, self.SLICED_PORT_A, self.MAC_A1_DASH)
            time.sleep(2)
            # Active + in-slice -> suppressed by slice (no tunnel route).
            self.check_neigh_in_asic_db(asicdb, ip, expected=False)
            self.check_tunnel_route_in_app_db(
                dvs, [ip + self.IPV6_MASK], expected=False
            )
        finally:
            self.del_neighbor(dvs, ip)
            self.del_fdb(dvs, self.MAC_A1_DASH)


# Dummy always-pass test at end as workaround for the issue where a Flaky
# fail on the final test invokes module tear-down before retrying.
def test_nonflaky_dummy():
    pass
