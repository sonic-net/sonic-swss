import time
import json
import random
import pytest

from swsscommon import swsscommon
from pprint import pprint
from evpn_tunnel import VxlanTunnel


class TestVrf(object):
    def setup_db(self, dvs):
        self.pdb = swsscommon.DBConnector(0, dvs.redis_sock, 0)
        self.adb = swsscommon.DBConnector(1, dvs.redis_sock, 0)
        self.cdb = swsscommon.DBConnector(4, dvs.redis_sock, 0)
        self.sdb = swsscommon.DBConnector(6, dvs.redis_sock, 0)

    def create_entry(self, tbl, key, pairs):
        fvs = swsscommon.FieldValuePairs(pairs)
        tbl.set(key, fvs)
        time.sleep(1)

    def create_entry_tbl(self, db, table, key, pairs):
        tbl = swsscommon.Table(db, table)
        self.create_entry(tbl, key, pairs)

    def delete_entry_tbl(self, db, table, key):
        tbl = swsscommon.Table(db, table)
        tbl._del(key)
        time.sleep(1)

    def how_many_entries_exist(self, db, table):
        tbl =  swsscommon.Table(db, table)
        return len(tbl.getKeys())

    def entries(self, db, table):
        tbl =  swsscommon.Table(db, table)
        return set(tbl.getKeys())

    def vrf_tunnel_map_types(self, vni):
        """Return the programmed VRF tunnel-map directions for one VNI."""
        tbl = swsscommon.Table(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")
        map_types = []
        for key in tbl.getKeys():
            status, fvs = tbl.get(key)
            assert status
            attrs = dict(fvs)
            if (attrs.get("SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY") == vni or
                    attrs.get("SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE") == vni):
                map_types.append(attrs["SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE"])
        return sorted(map_types)

    def is_vrf_attributes_correct(self, db, table, key, expected_attributes):
        tbl =  swsscommon.Table(db, table)
        keys = set(tbl.getKeys())
        assert key in keys, "The created key wasn't found"

        status, fvs = tbl.get(key)
        assert status, "Got an error when get a key"

        # filter the fake 'NULL' attribute out
        fvs = [x for x in fvs if x != ('NULL', 'NULL')]

        attr_keys = {entry[0] for entry in fvs}
        assert attr_keys == set(expected_attributes.keys())

        for name, value in fvs:
            assert expected_attributes[name] == value, "Wrong value %s for the attribute %s = %s" % \
                                                   (value, name, expected_attributes[name])


    def vrf_create(self, dvs, vrf_name, attributes, expected_attributes):
        # Read the baseline state. A logical VRF_TABLE|default entry may be
        # present without creating another ASIC virtual-router object.
        initial_entries = self.entries(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        initial_app_entries = self.entries(self.pdb, "VRF_TABLE")
        assert vrf_name not in initial_app_entries

        # create a fake attribute if we don't have attributes in the request
        if len(attributes) == 0:
            attributes = [('empty', 'empty')]

        # create the VRF entry in Config DB
        self.create_entry_tbl(self.cdb, "VRF", vrf_name, attributes)

        # check vrf created in kernel
        (status, rslt) = dvs.runcmd("ip link show " + vrf_name)
        assert status == 0

        # check application database
        tbl = swsscommon.Table(self.pdb, "VRF_TABLE")
        intf_entries = set(tbl.getKeys())
        assert intf_entries == initial_app_entries | {vrf_name}
        exp_attr = {}
        for an in range(len(attributes)):
            exp_attr[attributes[an][0]] = attributes[an][1]
        self.is_vrf_attributes_correct(self.pdb, "VRF_TABLE", vrf_name, exp_attr)

        # check that the vrf entry was created
        assert self.how_many_entries_exist(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == \
            len(initial_entries) + 1, "The vrf wasn't created"

        # find the id of the entry which was added
        added_entry_id = list(self.entries(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") - initial_entries)[0]

        # check correctness of the created attributes
        self.is_vrf_attributes_correct(
            self.adb,
            "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER",
            added_entry_id,
            expected_attributes,
        )

        state = {
            'initial_entries': initial_entries,
            'initial_app_entries': initial_app_entries,
            'entry_id': added_entry_id,
        }

        return state


    def vrf_remove(self, dvs, vrf_name, state):
        # delete the created vrf entry
        self.delete_entry_tbl(self.cdb, "VRF", vrf_name)

        # check application database
        tbl = swsscommon.Table(self.pdb, "VRF_TABLE")
        intf_entries = set(tbl.getKeys())
        assert intf_entries == state['initial_app_entries']

        # check that the vrf entry was removed
        assert self.how_many_entries_exist(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == \
            len(state['initial_entries']), "The vrf wasn't removed"

        # check that the correct vrf entry was removed
        assert state['initial_entries'] == self.entries(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER"), "The incorrect entry was removed"

        # check vrf was removed from kernel
        (status, rslt) = dvs.runcmd("ip link show " + vrf_name)
        assert status != 0

    def vrf_update(self, vrf_name, attributes, expected_attributes, state):
        # update the VRF entry in Config DB
        self.create_entry_tbl(self.cdb, "VRF", vrf_name, attributes)

        # check correctness of the created attributes
        self.is_vrf_attributes_correct(
            self.adb,
            "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER",
            state['entry_id'],
            expected_attributes,
        )


    def boolean_gen(self):
        result = random.choice(['false', 'true'])
        return result, result


    def mac_addr_gen(self):
        ns = [random.randint(0, 255) for _ in range(6)]
        ns[0] &= 0xfe
        mac = ':'.join("%02x" % n for n in ns)
        return mac, mac.upper()


    def packet_action_gen(self):
        values = [
            ("drop",        "SAI_PACKET_ACTION_DROP"),
            ("forward",     "SAI_PACKET_ACTION_FORWARD"),
            ("copy",        "SAI_PACKET_ACTION_COPY"),
            ("copy_cancel", "SAI_PACKET_ACTION_COPY_CANCEL"),
            ("trap",        "SAI_PACKET_ACTION_TRAP"),
            ("log",         "SAI_PACKET_ACTION_LOG"),
            ("deny",        "SAI_PACKET_ACTION_DENY"),
            ("transit",     "SAI_PACKET_ACTION_TRANSIT"),
        ]

        r = random.choice(values)
        return r[0], r[1]

    def test_VRFMgr_Comprehensive(self, dvs, testlog):
        self.setup_db(dvs)

        attributes = [
            ('v4',            'SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V4_STATE',                     self.boolean_gen),
            ('v6',            'SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V6_STATE',                     self.boolean_gen),
            ('src_mac',       'SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS',                    self.mac_addr_gen),
            ('ttl_action',    'SAI_VIRTUAL_ROUTER_ATTR_VIOLATION_TTL1_PACKET_ACTION',       self.packet_action_gen),
            ('ip_opt_action', 'SAI_VIRTUAL_ROUTER_ATTR_VIOLATION_IP_OPTIONS_PACKET_ACTION', self.packet_action_gen),
            ('l3_mc_action',  'SAI_VIRTUAL_ROUTER_ATTR_UNKNOWN_L3_MULTICAST_PACKET_ACTION', self.packet_action_gen),
        ]

        random.seed(int(time.time()))

        for n in range(2**len(attributes)):
            # generate testcases for all combinations of attributes
            req_attr = []
            exp_attr = {}
            vrf_name = "Vrf_%d" % n
            bmask = 0x1
            for an in range(len(attributes)):
                if (bmask & n) > 0:
                    req_res, exp_res = attributes[an][2]()
                    req_attr.append((attributes[an][0], req_res))
                    exp_attr[attributes[an][1]] = exp_res
                bmask <<= 1
            state = self.vrf_create(dvs, vrf_name, req_attr, exp_attr)
            self.vrf_remove(dvs, vrf_name, state)


    def test_VRFMgr(self, dvs, testlog):
        self.setup_db(dvs)

        state = self.vrf_create(dvs, "Vrf0",
            [
            ],
            {
            }
        )
        self.vrf_remove(dvs, "Vrf0", state)

        state = self.vrf_create(dvs, "Vrf1",
            [
                ('v4', 'true'),
                ('src_mac', '02:04:06:07:08:09'),
            ],
            {
                'SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V4_STATE':  'true',
                'SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS': '02:04:06:07:08:09',
            }
        )
        self.vrf_remove(dvs, "Vrf1", state)

    def test_VRFMgr_DefaultVrfL3Vni(self, dvs, testlog):
        """The default VRF is logical state and must reuse the switch VR."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-default-vrf"
        inactive_tunnel_name = "tunnel-without-nvo"
        vxlan.create_vlan1(dvs, "Vlan100")
        vxlan.create_vlan1(dvs, "Vlan101")
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "6.6.6.6")
        vxlan.create_vxlan_tunnel(dvs, inactive_tunnel_name, "7.7.7.7")
        vxlan.create_evpn_nvo(dvs, "nvo-default-vrf", tunnel_name)

        initial_vrs = self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        assert len(initial_vrs) == 1
        app_db = dvs.get_app_db()
        state_db = dvs.get_state_db()

        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", "5000")])

        # vrfmgrd must publish the L3VNI without creating a Linux VRF device.
        (status, _) = dvs.runcmd("ip link show dev default")
        assert status != 0
        app_db.wait_for_field_match("VRF_TABLE", "default", {"vni": "5000"})
        state_db.wait_for_field_match(
            "VRF_TABLE", "default", {"state": "ok"})
        state_db.wait_for_field_match(
            "VRF_OBJECT_TABLE", "default", {"state": "ok"})
        vxlan_vrf_table = swsscommon.Table(self.pdb, "VXLAN_VRF_TABLE")
        old_map_key = "%s:evpn_map_5000_default" % tunnel_name
        new_map_key = "%s:evpn_map_5001_default" % tunnel_name

        # The VRF and NVO may arrive before the tunnel map. The later map event
        # must reconcile the missing APP_DB entry without replaying the VRF.
        assert old_map_key not in vxlan_vrf_table.getKeys()
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, "map-5000", "5000", "Vlan100")
        app_db.wait_for_entry("VXLAN_VRF_TABLE", old_map_key)
        vxlan.create_vxlan_tunnel_map(
            dvs, inactive_tunnel_name, "map-5000", "5000", "Vlan100")
        inactive_map_key = "%s:evpn_map_5000_default" % inactive_tunnel_name
        assert inactive_map_key not in vxlan_vrf_table.getKeys()

        # VRFOrch must associate the name with the pre-existing switch default
        # virtual router rather than asking SAI to allocate another one.
        assert self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == initial_vrs

        # Replaying the same value is idempotent. A live replacement is
        # rejected and leaves the old mapping undisturbed.
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", "5000")])
        app_db.wait_for_entry("VXLAN_VRF_TABLE", old_map_key)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, "map-5001", "5001", "Vlan101")

        # A direct second APP_DB mapping for the default VRF is rejected by
        # VxlanVrfMapOrch before it can create another SAI encap/decap pair.
        tunnel_map_entries = self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")
        duplicate_map_key = "%s:duplicate-default-map" % tunnel_name
        self.create_entry_tbl(
            self.pdb, "VXLAN_VRF_TABLE", duplicate_map_key,
            [("vni", "5001"), ("vrf", "default")])
        assert self.entries(
            self.adb,
            "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY") == tunnel_map_entries
        self.delete_entry_tbl(
            self.pdb, "VXLAN_VRF_TABLE", duplicate_map_key)

        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", "5001")])
        app_db.wait_for_field_match("VRF_TABLE", "default", {"vni": "5000"})
        app_db.wait_for_entry("VXLAN_VRF_TABLE", old_map_key)
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", new_map_key)
        assert self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == initial_vrs

        # The CLI removes only the VNI association by writing vni=0. The
        # logical default VRF remains live while its VXLAN mapping is removed.
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", "0")])
        app_db.wait_for_field_match("VRF_TABLE", "default", {"vni": "0"})
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", old_map_key)
        state_db.wait_for_field_match(
            "VRF_TABLE", "default", {"state": "ok"})
        state_db.wait_for_field_match(
            "VRF_OBJECT_TABLE", "default", {"state": "ok"})
        assert self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == initial_vrs
        (status, _) = dvs.runcmd("ip link show dev default")
        assert status != 0

        # A different VNI is valid only after the old association and its
        # dependent mapping have completed deletion.
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", "5001")])
        app_db.wait_for_field_match("VRF_TABLE", "default", {"vni": "5001"})
        app_db.wait_for_entry("VXLAN_VRF_TABLE", new_map_key)
        assert self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == initial_vrs
        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VRF_TABLE", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", new_map_key)

        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, "map-5000", "5000", "Vlan100")
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, "map-5001", "5001", "Vlan101")
        vxlan.remove_vxlan_tunnel_map(
            dvs, inactive_tunnel_name, "map-5000", "5000", "Vlan100")
        vxlan.remove_evpn_nvo(dvs, "nvo-default-vrf")
        vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan.remove_vxlan_tunnel(dvs, inactive_tunnel_name)
        vxlan.remove_vlan(dvs, "100")
        vxlan.remove_vlan(dvs, "101")

    def test_VRFMgr_DefaultVrfL3VniOwnershipConflicts(self, dvs, testlog):
        """Reject one L3VNI being owned by both default and tenant VRFs."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-vrf-owner"
        nvo_name = "nvo-vrf-owner"
        tenant_first = "VrfTenantFirst"
        tenant_second = "VrfTenantSecond"
        app_db = dvs.get_app_db()

        vxlan.create_vlan1(dvs, "Vlan130")
        vxlan.create_vlan1(dvs, "Vlan131")
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "11.11.11.11")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, "map-owner-5300", "5300", "Vlan130")
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, "map-owner-5400", "5400", "Vlan131")

        tenant_map_key = "%s:evpn_map_5300_%s" % (tunnel_name, tenant_first)
        default_5300_key = "%s:evpn_map_5300_default" % tunnel_name
        default_5400_key = "%s:evpn_map_5400_default" % tunnel_name
        tenant_5400_key = "%s:evpn_map_5400_%s" % (tunnel_name, tenant_second)

        # Tenant first: CONFIG_DB accepts the row, but vrfmgrd rejects the
        # conflicting default mapping before publishing any APP_DB state.
        self.create_entry_tbl(
            self.cdb, "VRF", tenant_first, [("vni", "5300")])
        app_db.wait_for_field_match(
            "VRF_TABLE", tenant_first, {"vni": "5300"})
        app_db.wait_for_entry("VXLAN_VRF_TABLE", tenant_map_key)
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", "5300")])
        app_db.wait_for_deleted_entry("VRF_TABLE", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", default_5300_key)
        (status, _) = dvs.runcmd("ip link show dev default")
        assert status != 0

        self.delete_entry_tbl(self.cdb, "VRF", "default")
        self.delete_entry_tbl(self.cdb, "VRF", tenant_first)
        app_db.wait_for_deleted_entry("VRF_TABLE", tenant_first)
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", tenant_map_key)

        # Default first: reject the tenant before creating its Linux VRF,
        # APP_DB entry, SAI VR, or tunnel-map entries.
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", "5400")])
        app_db.wait_for_field_match("VRF_TABLE", "default", {"vni": "5400"})
        app_db.wait_for_entry("VXLAN_VRF_TABLE", default_5400_key)
        initial_vrs = self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        self.create_entry_tbl(
            self.cdb, "VRF", tenant_second, [("vni", "5400")])
        app_db.wait_for_deleted_entry("VRF_TABLE", tenant_second)
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", tenant_5400_key)
        (status, _) = dvs.runcmd("ip link show dev " + tenant_second)
        assert status != 0
        assert self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == initial_vrs

        self.delete_entry_tbl(self.cdb, "VRF", tenant_second)
        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VRF_TABLE", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", default_5400_key)
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, "map-owner-5300", "5300", "Vlan130")
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, "map-owner-5400", "5400", "Vlan131")
        vxlan.remove_evpn_nvo(dvs, nvo_name)
        vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan.remove_vlan(dvs, "130")
        vxlan.remove_vlan(dvs, "131")

    def test_VRFMgr_DefaultVrfL3VniDependencyLifecycle(self, dvs, testlog):
        """Reconcile default-VRF state as NVO and tunnel-map inputs change."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-default-order"
        nvo_name = "nvo-default-order"
        map_name = "map-default-order"
        vni = "5100"
        vlan = "Vlan110"
        map_key = "%s:evpn_map_%s_default" % (tunnel_name, vni)
        app_db = dvs.get_app_db()

        initial_vrs = self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")

        # Configure the logical VRF before any VXLAN dependency exists.
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", vni)])
        app_db.wait_for_field_match("VRF_TABLE", "default", {"vni": vni})
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", map_key)

        # NVO alone is insufficient; the VNI-to-VLAN map completes the mapping.
        vxlan.create_vlan1(dvs, vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "8.8.8.8")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", map_key)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, vni, vlan)
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", map_key, {"vni": vni, "vrf": "default"})

        # Removing and restoring the tunnel map must withdraw and recreate the
        # APP_DB mapping without replaying VRF configuration.
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, vni, vlan)
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", map_key)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, vni, vlan)
        app_db.wait_for_entry("VXLAN_VRF_TABLE", map_key)

        # The same reconciliation is required when the active NVO disappears
        # and later returns.
        vxlan.remove_evpn_nvo(dvs, nvo_name)
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", map_key)
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)
        app_db.wait_for_entry("VXLAN_VRF_TABLE", map_key)

        assert self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == initial_vrs

        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VRF_TABLE", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", map_key)
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, vni, vlan)
        vxlan.remove_evpn_nvo(dvs, nvo_name)
        vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan.remove_vlan(dvs, "110")

    def test_VRFMgr_DefaultVrfL3VniWarmRestart(self, dvs, testlog):
        """Warm-restart SWSS without duplicating or losing the default mapping."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-default-warm"
        nvo_name = "nvo-default-warm"
        map_name = "map-default-warm"
        vni = "5500"
        vlan = "Vlan150"
        map_key = "%s:evpn_map_%s_default" % (tunnel_name, vni)
        app_db = dvs.get_app_db()

        vxlan.create_vlan1(dvs, vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "12.12.12.12")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, vni, vlan)
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", vni)])
        app_db.wait_for_field_match("VRF_TABLE", "default", {"vni": vni})
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", map_key, {"vni": vni, "vrf": "default"})

        initial_vrs = self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        expected_map_types = sorted([
            "SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI",
            "SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID",
        ])
        assert self.vrf_tunnel_map_types(vni) == expected_map_types

        (exitcode, _) = dvs.runcmd("config warm_restart enable swss")
        assert exitcode == 0
        try:
            # The shared DVS can retain unrelated pending work from earlier
            # modules. The mapping itself is already converged above, so wait
            # for the restart handshake while ignoring unrelated queues.
            (exitcode, result) = dvs.runcmd(
                "/usr/bin/orchagent_restart_check -s -r 20 -w 1000",
                include_stderr=False)
            assert exitcode == 0
            assert result == "RESTARTCHECK succeeded\n"
            dvs.stop_swss()
            dvs.start_swss()

            app_db.wait_for_field_match(
                "VRF_TABLE", "default", {"vni": vni})
            app_db.wait_for_field_match(
                "VXLAN_VRF_TABLE", map_key,
                {"vni": vni, "vrf": "default"})
            assert self.entries(
                self.adb,
                "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == initial_vrs
            # VS may allocate new OIDs while reconciling a warm restart. The
            # invariant is exactly one encap and one decap direction, not OID
            # identity across the restart.
            assert self.vrf_tunnel_map_types(vni) == expected_map_types
        finally:
            dvs.runcmd("config warm_restart disable swss")

        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VRF_TABLE", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", map_key)
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, vni, vlan)
        vxlan.remove_evpn_nvo(dvs, nvo_name)
        vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan.remove_vlan(dvs, "150")

    def test_VRFMgr_DefaultVrfL3VniNvoRetarget(self, dvs, testlog):
        """Move an NVO between tunnels without leaving a stale VRF map."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        old_tunnel = "tunnel-default-old"
        new_tunnel = "tunnel-default-new"
        nvo_name = "nvo-default-retarget"
        vni = "5200"
        vlan = "Vlan120"
        old_key = "%s:evpn_map_%s_default" % (old_tunnel, vni)
        new_key = "%s:evpn_map_%s_default" % (new_tunnel, vni)
        app_db = dvs.get_app_db()

        vxlan.create_vlan1(dvs, vlan)
        vxlan.create_vxlan_tunnel(dvs, old_tunnel, "9.9.9.9")
        vxlan.create_vxlan_tunnel(dvs, new_tunnel, "10.10.10.10")
        vxlan.create_vxlan_tunnel_map(
            dvs, old_tunnel, "map-default-old", vni, vlan)
        vxlan.create_vxlan_tunnel_map(
            dvs, new_tunnel, "map-default-new", vni, vlan)
        vxlan.create_evpn_nvo(dvs, nvo_name, old_tunnel)
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", vni)])

        app_db.wait_for_entry("VXLAN_VRF_TABLE", old_key)
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", new_key)

        # Updating the same NVO key changes the active source tunnel.
        vxlan.create_evpn_nvo(dvs, nvo_name, new_tunnel)
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", old_key)
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", new_key, {"vni": vni, "vrf": "default"})

        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VRF_TABLE", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", new_key)
        vxlan.remove_evpn_nvo(dvs, nvo_name)
        vxlan.remove_vxlan_tunnel_map(
            dvs, old_tunnel, "map-default-old", vni, vlan)
        vxlan.remove_vxlan_tunnel_map(
            dvs, new_tunnel, "map-default-new", vni, vlan)
        vxlan.remove_vxlan_tunnel(dvs, old_tunnel)
        vxlan.remove_vxlan_tunnel(dvs, new_tunnel)
        vxlan.remove_vlan(dvs, "120")

    def test_VRFMgr_Update(self, dvs, testlog):
        self.setup_db(dvs)

        attributes = [
            ('v4',            'SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V4_STATE',                     self.boolean_gen),
            ('v6',            'SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V6_STATE',                     self.boolean_gen),
            ('src_mac',       'SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS',                    self.mac_addr_gen),
            ('ttl_action',    'SAI_VIRTUAL_ROUTER_ATTR_VIOLATION_TTL1_PACKET_ACTION',       self.packet_action_gen),
            ('ip_opt_action', 'SAI_VIRTUAL_ROUTER_ATTR_VIOLATION_IP_OPTIONS_PACKET_ACTION', self.packet_action_gen),
            ('l3_mc_action',  'SAI_VIRTUAL_ROUTER_ATTR_UNKNOWN_L3_MULTICAST_PACKET_ACTION', self.packet_action_gen),
        ]

        random.seed(int(time.time()))

        state = self.vrf_create(dvs, "Vrf_a",
            [
            ],
            {
            }
        )

        # try to update each attribute
        req_attr = []
        exp_attr = {}
        for attr in attributes:
            req_res, exp_res = attr[2]()
            req_attr.append((attr[0], req_res))
            exp_attr[attr[1]] = exp_res
            self.vrf_update("Vrf_a", req_attr, exp_attr, state)

        self.vrf_remove(dvs, "Vrf_a", state)

    @pytest.mark.xfail(reason="Test unstable, blocking PR builds")
    def test_VRFMgr_Capacity(self, dvs, testlog):
        self.setup_db(dvs)

        initial_entries_cnt = self.how_many_entries_exist(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")

        maximum_vrf_cnt = 4096

        def create_entry(self, tbl, key, pairs):
            fvs = swsscommon.FieldValuePairs(pairs)
            tbl.set(key, fvs)
            time.sleep(1)

        def create_entry_tbl(self, db, table, key, pairs):
            tbl = swsscommon.Table(db, table)
            self.create_entry(tbl, key, pairs)

        # create the VRF entry in Config DB
        tbl = swsscommon.Table(self.cdb, "VRF")
        fvs = swsscommon.FieldValuePairs([('empty', 'empty')])
        for i in range(maximum_vrf_cnt):
            tbl.set("Vrf_%d" % i, fvs)

        # wait for all VRFs pushed to database and linux
        time.sleep(30)

        # check app_db
        intf_entries_cnt = self.how_many_entries_exist(self.pdb, "VRF_TABLE")
        assert intf_entries_cnt == maximum_vrf_cnt

        # check asic_db
        current_entries_cnt = self.how_many_entries_exist(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        assert (current_entries_cnt - initial_entries_cnt) == maximum_vrf_cnt

        # check linux kernel
        (exitcode, num) = dvs.runcmd(['sh', '-c', "ip link show | grep Vrf | wc -l"])
        assert num.strip() == str(maximum_vrf_cnt)

        # remove VRF from Config DB
        for i in range(maximum_vrf_cnt):
            tbl._del("Vrf_%d" % i)

        # wait for all VRFs deleted
        time.sleep(120)

        # check app_db
        intf_entries_cnt = self.how_many_entries_exist(self.pdb, "VRF_TABLE")
        assert intf_entries_cnt == 0

        # check asic_db
        current_entries_cnt = self.how_many_entries_exist(self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        assert (current_entries_cnt - initial_entries_cnt) == 0

        # check linux kernel
        (exitcode, num) = dvs.runcmd(['sh', '-c', "ip link show | grep Vrf | wc -l"])
        assert num.strip() == '0'


# Add Dummy always-pass test at end as workaroud
# for issue when Flaky fail on final test it invokes module tear-down before retrying
def test_nonflaky_dummy():
    pass
