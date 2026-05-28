"""VS tests for dualtor mux subnet slicing.

Scope:
  1. server_ipv6_subnet is parsed and published to STATE_DB.
  2. The slice gate keeps in-slice IPv6 neighbors out of the SAI neighbor
     table regardless of the landing port (sliced vs non-sliced) and
     regardless of the mux state (ACTIVE vs STANDBY).
  3. Out-of-slice IPv6 neighbors are NOT touched by the slice gate;
     they follow the normal mux state machine (present in ACTIVE,
     removed in STANDBY for active-standby cables).

Topology:
  Ethernet0  - non-sliced control (cable_type=active-active)
  Ethernet4  - sliced (active-standby), server_ipv6_subnet=fc02:1000::100/120
"""

import time
import pytest

from swsscommon import swsscommon

from test_mux import TestMuxTunnelBase


ACTIVE  = "active"
STANDBY = "standby"


class TestMuxSubnetSliceBase(TestMuxTunnelBase):
    STATE_MUX_CABLE_TABLE = "MUX_CABLE_TABLE"

    SLICED_PORT       = "Ethernet4"
    NON_SLICED_PORT   = "Ethernet0"

    SLICE_PREFIX      = "fc02:1000::100/120"   # covers ::100 .. ::1ff
    SERVER_IPV6       = TestMuxTunnelBase.SERV2_IPV6   # fc02:1000::101

    IN_SLICE_IPV6     = "fc02:1000::150"
    OUT_OF_SLICE_IPV6 = "fc02:1000::ff"

    MAC               = "00:aa:00:00:00:01"
    MAC_DASH          = "00-aa-00-00-00-01"

    def create_mux_cable(self, confdb):
        # Ethernet0 - non-sliced (active-active)
        fvs = {
            "server_ipv4":   self.SERV1_IPV4 + self.IPV4_MASK,
            "server_ipv6":   self.SERV1_IPV6 + self.IPV6_MASK,
            "soc_ipv4":      self.SERV1_SOC_IPV4 + self.IPV4_MASK,
            "cable_type":    "active-active",
        }
        confdb.create_entry(self.CONFIG_MUX_CABLE, self.NON_SLICED_PORT, fvs)

        # Ethernet4 - sliced (default active-standby)
        fvs = {
            "server_ipv4":         self.SERV2_IPV4 + self.IPV4_MASK,
            "server_ipv6":         self.SERVER_IPV6 + self.IPV6_MASK,
            "server_ipv6_subnet":  self.SLICE_PREFIX,
        }
        confdb.create_entry(self.CONFIG_MUX_CABLE, self.SLICED_PORT, fvs)

    def get_state_db_mux_entry(self, dvs, port):
        state_db = dvs.get_state_db()
        return state_db.get_entry(self.STATE_MUX_CABLE_TABLE, port) or {}

    def add_neigh_on_port(self, dvs, ip, mac, mac_dash, port):
        self.add_fdb(dvs, port, mac_dash)
        self.add_neighbor(dvs, ip, mac)
        time.sleep(1)

    def del_neigh_on_port(self, dvs, ip, mac_dash):
        self.del_neighbor(dvs, ip)
        self.del_fdb(dvs, mac_dash)
        time.sleep(1)


class TestMuxSubnetSlice(TestMuxSubnetSliceBase):

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

    def test_slice_prefix_published_to_state_db(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        """STATE_DB MUX_CABLE_TABLE shows server_ipv6_subnet on sliced port
        only; non-sliced port has no such field."""
        sliced = {}
        for _ in range(20):
            sliced = self.get_state_db_mux_entry(dvs, self.SLICED_PORT)
            if sliced.get("server_ipv6_subnet"):
                break
            time.sleep(0.5)
        assert sliced.get("server_ipv6_subnet") == self.SLICE_PREFIX, (
            f"sliced port state: {sliced}"
        )

        non_sliced = self.get_state_db_mux_entry(dvs, self.NON_SLICED_PORT)
        assert "server_ipv6_subnet" not in non_sliced, (
            f"non-sliced port should not have server_ipv6_subnet: {non_sliced}"
        )

    def test_in_slice_ipv6_port_affinity(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        """Port-affinity truth table for an in-slice IPv6:
          (1) FDB on the SLICED port (slice cable owns the IP)  -> suppressed
              regardless of mux state (ACTIVE or STANDBY).
          (2) FDB on the NON-SLICED port                        -> programmed
              (follows the non-sliced cable's normal semantics).
        """
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        # (1) On sliced port: never in ASIC, across both mux states.
        for state in (ACTIVE, STANDBY):
            self.set_mux_state(appdb, self.SLICED_PORT, state)
            self.add_neigh_on_port(dvs, self.IN_SLICE_IPV6, self.MAC,
                                   self.MAC_DASH, self.SLICED_PORT)
            try:
                self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_IPV6,
                                            expected=False)
            finally:
                self.del_neigh_on_port(dvs, self.IN_SLICE_IPV6,
                                       self.MAC_DASH)

        # (2) On non-sliced port (active-active): SAI neighbor is programmed.
        self.set_mux_state(appdb, self.NON_SLICED_PORT, ACTIVE)
        self.add_neigh_on_port(dvs, self.IN_SLICE_IPV6, self.MAC,
                               self.MAC_DASH, self.NON_SLICED_PORT)
        try:
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_IPV6,
                                        expected=True)
        finally:
            self.del_neigh_on_port(dvs, self.IN_SLICE_IPV6, self.MAC_DASH)

    def test_out_of_slice_ipv6_follows_normal_mux_switchover(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        """Out-of-slice IPv6 on the sliced cable is NOT touched by the slice
        gate and continues to follow the active-standby mux state machine:
          ACTIVE  -> SAI neighbor present
          STANDBY -> SAI neighbor removed (tunnel route takes over)
          ACTIVE  -> SAI neighbor re-installed
        """
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        self.set_mux_state(appdb, self.SLICED_PORT, ACTIVE)
        self.add_neigh_on_port(dvs, self.OUT_OF_SLICE_IPV6, self.MAC,
                               self.MAC_DASH, self.SLICED_PORT)
        try:
            neigh_key = self.check_neigh_in_asic_db(
                asicdb, self.OUT_OF_SLICE_IPV6, expected=True
            )

            self.set_mux_state(appdb, self.SLICED_PORT, STANDBY)
            asicdb.wait_for_deleted_entry(self.ASIC_NEIGH_TABLE, neigh_key)

            self.set_mux_state(appdb, self.SLICED_PORT, ACTIVE)
            self.check_neigh_in_asic_db(asicdb, self.OUT_OF_SLICE_IPV6,
                                        expected=True)
        finally:
            self.del_neigh_on_port(dvs, self.OUT_OF_SLICE_IPV6, self.MAC_DASH)

    def test_neighbor_port_move_unsuppresses_in_slice(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        """FDB-only port move (neighbor entry NOT deleted) follows the new
        truth table:
          (a) in-slice IPv6:
                start on sliced port  -> suppressed (no SAI neighbor)
                FDB moves to non-sliced port -> un-suppressed (SAI neighbor
                                                appears, same kernel entry).
          (b) out-of-slice IPv6: SAI neighbor stays programmed across the
              move, identified by the same ASIC key (slice gate untouched).
        Both cables ACTIVE so we isolate the slice gate from active-standby
        teardown.
        """
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        self.set_mux_state(appdb, self.SLICED_PORT, ACTIVE)
        self.set_mux_state(appdb, self.NON_SLICED_PORT, ACTIVE)

        # (a) in-slice IP: suppressed on sliced port; programmed after move.
        self.add_neigh_on_port(dvs, self.IN_SLICE_IPV6, self.MAC,
                               self.MAC_DASH, self.SLICED_PORT)
        try:
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_IPV6,
                                        expected=False)

            # FDB move only (neighbor kernel entry untouched).
            self.del_fdb(dvs, self.MAC_DASH)
            self.add_fdb(dvs, self.NON_SLICED_PORT, self.MAC_DASH)
            time.sleep(2)

            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_IPV6,
                                        expected=True)
        finally:
            self.del_neigh_on_port(dvs, self.IN_SLICE_IPV6, self.MAC_DASH)

        # (b) out-of-slice IP: SAI neighbor stays programmed across the move.
        self.add_neigh_on_port(dvs, self.OUT_OF_SLICE_IPV6, self.MAC,
                               self.MAC_DASH, self.SLICED_PORT)
        try:
            neigh_key_before = self.check_neigh_in_asic_db(
                asicdb, self.OUT_OF_SLICE_IPV6, expected=True
            )

            self.del_fdb(dvs, self.MAC_DASH)
            self.add_fdb(dvs, self.NON_SLICED_PORT, self.MAC_DASH)
            time.sleep(2)

            neigh_key_after = self.check_neigh_in_asic_db(
                asicdb, self.OUT_OF_SLICE_IPV6, expected=True
            )
            assert neigh_key_before == neigh_key_after, (
                f"OUT_OF_SLICE neigh key changed across FDB move: "
                f"before={neigh_key_before} after={neigh_key_after}"
            )
        finally:
            self.del_neigh_on_port(dvs, self.OUT_OF_SLICE_IPV6, self.MAC_DASH)

    def test_in_slice_fdb_move_to_slice_port_suppresses(
        self, dvs, dvs_route, setup, setup_vlan, setup_peer_switch,
        setup_tunnel, setup_mux_cable, testlog
    ):
        """Reverse direction of port-affinity: an in-slice IPv6 neighbor
        that was programmed in SAI because its FDB initially resolved to
        the non-sliced port must be retroactively suppressed when the MAC
        moves onto the slice cable's own port. Then moving the MAC back
        off the slice port un-suppresses it again.

        Both cables are ACTIVE so the slice gate is isolated from
        active-standby teardown.
        """
        appdb = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
        asicdb = dvs.get_asic_db()

        self.set_mux_state(appdb, self.SLICED_PORT, ACTIVE)
        self.set_mux_state(appdb, self.NON_SLICED_PORT, ACTIVE)

        # Seed: FDB on non-sliced port -> in-slice IPv6 is programmed.
        self.add_neigh_on_port(dvs, self.IN_SLICE_IPV6, self.MAC,
                               self.MAC_DASH, self.NON_SLICED_PORT)
        try:
            neigh_key = self.check_neigh_in_asic_db(
                asicdb, self.IN_SLICE_IPV6, expected=True
            )

            # FDB move ONTO the slice cable's port -> must be suppressed.
            self.del_fdb(dvs, self.MAC_DASH)
            self.add_fdb(dvs, self.SLICED_PORT, self.MAC_DASH)
            time.sleep(2)

            asicdb.wait_for_deleted_entry(self.ASIC_NEIGH_TABLE, neigh_key)
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_IPV6,
                                        expected=False)

            # FDB back to non-slice port -> re-programmed (exercises the
            # forward unsuppress path; guards against regressions in (a)).
            self.del_fdb(dvs, self.MAC_DASH)
            self.add_fdb(dvs, self.NON_SLICED_PORT, self.MAC_DASH)
            time.sleep(2)
            self.check_neigh_in_asic_db(asicdb, self.IN_SLICE_IPV6,
                                        expected=True)
        finally:
            self.del_neigh_on_port(dvs, self.IN_SLICE_IPV6, self.MAC_DASH)
