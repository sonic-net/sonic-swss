import time
import json
import random
import pytest

from swsscommon import swsscommon
from pprint import pprint
from evpn_tunnel import VxlanTunnel
from dvslib.dvs_common import PollingConfig, wait_for_result


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
                map_type = attrs["SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE"]
                if "VIRTUAL_ROUTER" in map_type:
                    map_types.append(map_type)
        return sorted(map_types)

    def default_virtual_router_id(self):
        """Return the switch-owned default virtual-router OID."""
        tbl = swsscommon.Table(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")
        switch_keys = tbl.getKeys()
        assert len(switch_keys) == 1, \
            "expected one switch object, found %s" % switch_keys
        status, fvs = tbl.get(switch_keys[0])
        assert status
        default_vr = dict(fvs).get(
            "SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID")
        if default_vr:
            return default_vr

        # Some VS SAI versions do not serialize this read-only switch
        # attribute into ASIC_DB. Follow the existing DVS fallback used by
        # other routing tests: before any named VRF is created, the sole SAI
        # virtual-router object is the switch-owned default VR.
        vr_tbl = swsscommon.Table(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        vr_keys = vr_tbl.getKeys()
        assert len(vr_keys) == 1, \
            "default VR switch attribute is absent and VR is ambiguous: %s" % vr_keys
        return vr_keys[0]

    def tunnel_mapper_ids(self, source_ip, required=True):
        """Return the encap/decap mapper OIDs owned by one P2MP VXLAN tunnel."""
        tbl = swsscommon.Table(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL")
        matches = []
        for key in tbl.getKeys():
            status, fvs = tbl.get(key)
            assert status
            attrs = dict(fvs)
            if (attrs.get("SAI_TUNNEL_ATTR_TYPE") ==
                    "SAI_TUNNEL_TYPE_VXLAN" and
                    attrs.get("SAI_TUNNEL_ATTR_PEER_MODE") ==
                    "SAI_TUNNEL_PEER_MODE_P2MP" and
                    attrs.get("SAI_TUNNEL_ATTR_ENCAP_SRC_IP") == source_ip):
                matches.append((key, attrs))

        if not matches:
            assert not required, \
                "expected one P2MP VXLAN tunnel for %s, found none" % source_ip
            return None
        assert len(matches) == 1, \
            "expected one P2MP VXLAN tunnel for %s, found %s" % (
                source_ip, [entry[0] for entry in matches])

        def object_list(value):
            count, objects = value.split(":", 1)
            object_ids = set(objects.split(",")) if objects else set()
            assert len(object_ids) == int(count)
            return object_ids

        tunnel_id, attrs = matches[0]
        return {
            "tunnel_id": tunnel_id,
            "encap": object_list(attrs["SAI_TUNNEL_ATTR_ENCAP_MAPPERS"]),
            "decap": object_list(attrs["SAI_TUNNEL_ATTR_DECAP_MAPPERS"]),
        }

    def assert_vrf_tunnel_map_association(
            self, vni, vr_id, tunnel_source_ip, excluded_source_ip=None):
        """Verify both VRF mapper directions target one VR and tunnel."""
        tbl = swsscommon.Table(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY")
        directions = {}
        for key in tbl.getKeys():
            status, fvs = tbl.get(key)
            assert status
            attrs = dict(fvs)
            map_type = attrs.get(
                "SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE")
            if (map_type ==
                    "SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI" and
                    attrs.get(
                        "SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE") == vni):
                assert map_type not in directions, \
                    "duplicate VR-to-VNI mapper entries for VNI %s" % vni
                directions[map_type] = attrs
            elif (map_type ==
                    "SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID" and
                    attrs.get(
                        "SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY") == vni):
                assert map_type not in directions, \
                    "duplicate VNI-to-VR mapper entries for VNI %s" % vni
                directions[map_type] = attrs

        encap_type = "SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI"
        decap_type = "SAI_TUNNEL_MAP_TYPE_VNI_TO_VIRTUAL_ROUTER_ID"
        assert set(directions) == {encap_type, decap_type}, \
            "missing VRF mapper direction for VNI %s: %s" % (
                vni, sorted(directions))

        encap_entry = directions[encap_type]
        decap_entry = directions[decap_type]
        assert encap_entry[
            "SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_KEY"] == vr_id
        assert decap_entry[
            "SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_VALUE"] == vr_id

        expected_tunnel = self.tunnel_mapper_ids(tunnel_source_ip)
        encap_mapper = encap_entry[
            "SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP"]
        decap_mapper = decap_entry[
            "SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP"]
        assert encap_mapper in expected_tunnel["encap"]
        assert decap_mapper in expected_tunnel["decap"]

        if excluded_source_ip is not None:
            excluded_tunnel = self.tunnel_mapper_ids(
                excluded_source_ip, required=False)
            if excluded_tunnel is not None:
                assert encap_mapper not in excluded_tunnel["encap"]
                assert decap_mapper not in excluded_tunnel["decap"]

    def wait_for_vrf_tunnel_map_association(
            self, vni, vr_id, tunnel_source_ip, excluded_source_ip=None):
        """Wait until both mapper directions belong to the expected objects."""
        def association_converged():
            try:
                self.assert_vrf_tunnel_map_association(
                    vni, vr_id, tunnel_source_ip, excluded_source_ip)
            except AssertionError as error:
                return False, str(error)
            return True, "VRF tunnel-map association converged"

        status, result = wait_for_result(
            association_converged,
            PollingConfig(strict=False),
        )
        assert status, \
            "VRF tunnel-map association for VNI %s did not converge: %s" % (
                vni, result)

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

        # Kernel netdevice cleanup follows APP_DB and ASIC_DB cleanup
        # asynchronously. Poll for the link to disappear instead of racing
        # vrfmgrd immediately after the database assertions complete.
        def _vrf_link_removed():
            status, output = dvs.runcmd("ip link show " + vrf_name)
            return (status != 0, output)

        wait_for_result(
            _vrf_link_removed,
            PollingConfig(polling_interval=0.1, timeout=20, strict=True),
            failure_message="VRF netdevice %s was not removed" % vrf_name,
        )

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

        # The VRF and NVO may arrive before the VNI-to-VLAN map. vrfmgrd
        # publishes the L3VNI intent immediately; the later L2 map is handled
        # independently by vxlanmgrd/VxlanTunnelMapOrch.
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", old_map_key,
            {"vni": "5000", "vrf": "default"})
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, "map-5000", "5000", "Vlan100")
        app_db.wait_for_field_match(
            "VXLAN_TUNNEL_MAP_TABLE", "%s:map-5000" % tunnel_name,
            {"vni": "5000", "vlan": "Vlan100"})
        vxlan.create_vxlan_tunnel_map(
            dvs, inactive_tunnel_name, "map-5000", "5000", "Vlan100")
        inactive_map_key = "%s:evpn_map_5000_default" % inactive_tunnel_name
        assert inactive_map_key not in vxlan_vrf_table.getKeys()

        # VRFOrch must associate the name with the pre-existing switch default
        # virtual router rather than asking SAI to allocate another one.
        assert self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == initial_vrs

        # Replaying the same value is idempotent.
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", "5000")])
        app_db.wait_for_entry("VXLAN_VRF_TABLE", old_map_key)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, "map-5001", "5001", "Vlan101")

        # A direct second APP_DB mapping for the default VRF remains pending in
        # VxlanVrfMapOrch and cannot create another SAI encap/decap pair. APP_DB
        # writers are responsible for valid intent; deleting the entry cancels
        # this deliberately conflicting request.
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
        # A low-level CONFIG_DB SET can represent a coalesced explicit delete
        # followed by add. vrfmgrd must complete the same break-before-make
        # transition instead of leaving CONFIG_DB and APP_DB divergent.
        app_db.wait_for_field_match("VRF_TABLE", "default", {"vni": "5001"})
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", old_map_key)
        app_db.wait_for_entry("VXLAN_VRF_TABLE", new_map_key)
        assert self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == initial_vrs

        # The default-VRF row exists only to carry the logical L3VNI mapping.
        # Deleting it must remove the APP_DB logical state and mapping without
        # deleting the switch-owned default virtual router or a Linux device.
        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VRF_TABLE", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", new_map_key)
        state_db.wait_for_deleted_entry("VRF_TABLE", "default")
        state_db.wait_for_deleted_entry("VRF_OBJECT_TABLE", "default")
        assert self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == initial_vrs
        (status, _) = dvs.runcmd("ip link show dev default")
        assert status != 0

        # A different VNI is valid after the prior association and its
        # dependent mapping have completed deletion.
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", "5000")])
        app_db.wait_for_field_match("VRF_TABLE", "default", {"vni": "5000"})
        app_db.wait_for_entry("VXLAN_VRF_TABLE", old_map_key)
        assert self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == initial_vrs
        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VRF_TABLE", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", old_map_key)

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

    def test_VRFMgr_DefaultVrfL3VniUpdateWithoutVniPreservesMapping(self, dvs, testlog):
        """A SET that does not carry vni must not remove an active mapping."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-default-no-vni"
        nvo_name = "nvo-default-no-vni"
        map_name = "map-default-no-vni"
        vni = "5600"
        vlan = "Vlan160"
        map_key = "%s:evpn_map_%s_default" % (tunnel_name, vni)
        app_db = dvs.get_app_db()

        vxlan.create_vlan1(dvs, vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "13.13.13.13")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, vni, vlan)
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", vni)])

        app_db.wait_for_field_match(
            "VRF_TABLE", "default", {"vni": vni})
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", map_key,
            {"vni": vni, "vrf": "default"})

        # Simulate a partial CONFIG_DB SET that updates another property. The
        # notification does not carry a vni field, so vrfmgrd must preserve the
        # existing mapping rather than treating the event as vni=0 removal.
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("v4", "false")])
        app_db.wait_for_field_match(
            "VRF_TABLE", "default", {"v4": "false", "vni": vni})
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", map_key,
            {"vni": vni, "vrf": "default"})

        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VRF_TABLE", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", map_key)
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, vni, vlan)
        vxlan.remove_evpn_nvo(dvs, nvo_name)
        vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan.remove_vlan(dvs, "160")

    @pytest.mark.parametrize("invalid_vni", [
        "not-a-number",
        "123junk",
        "16777216",
    ])
    def test_VRFMgr_DefaultVrfRejectsInvalidVni(self, dvs, testlog, invalid_vni):
        """Reject malformed and out-of-range VNIs written directly to CONFIG_DB."""
        self.setup_db(dvs)

        initial_vrs = self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        app_db = dvs.get_app_db()
        initial_app_vrf = app_db.get_entry("VRF_TABLE", "default")
        initial_default_maps = {
            key for key in app_db.get_keys("VXLAN_VRF_TABLE")
            if key.endswith("_default")
        }

        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", invalid_vni)])

        # vrfmgrd consumes and rejects the event without changing the logical
        # default-VRF placeholder or publishing any new VXLAN mapping.
        assert app_db.get_entry("VRF_TABLE", "default") == initial_app_vrf
        assert {
            key for key in app_db.get_keys("VXLAN_VRF_TABLE")
            if key.endswith("_default")
        } == initial_default_maps
        assert self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") == initial_vrs
        self.delete_entry_tbl(self.cdb, "VRF", "default")

    def test_VRFMgr_DefaultVrfCanonicalizesNumericVni(self, dvs, testlog):
        """Equivalent numeric spellings must have one internal VNI identity."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-default-canonical-vni"
        nvo_name = "nvo-default-canonical-vni"
        map_name = "map-default-canonical-vni"
        canonical_vni = "5700"
        configured_vni = "05700"
        vlan = "Vlan170"
        map_key = "%s:evpn_map_%s_default" % (
            tunnel_name, canonical_vni)
        app_db = dvs.get_app_db()

        vxlan.create_vlan1(dvs, vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "17.17.17.17")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, configured_vni, vlan)
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", configured_vni)])

        app_db.wait_for_field_match(
            "VRF_TABLE", "default", {"vni": canonical_vni})
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", map_key,
            {"vni": canonical_vni, "vrf": "default"})

        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", map_key)
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, configured_vni, vlan)
        vxlan.remove_evpn_nvo(dvs, nvo_name)
        vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan.remove_vlan(dvs, "170")

    def test_VRFMgr_DefaultVrfIgnoresMalformedTunnelMapKey(self, dvs, testlog):
        """A malformed VXLAN_TUNNEL_MAP event must not disturb a valid map."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-default-bad-key"
        nvo_name = "nvo-default-bad-key"
        map_name = "map-default-bad-key"
        vni = "5700"
        vlan = "Vlan170"
        map_key = "%s:evpn_map_%s_default" % (tunnel_name, vni)
        app_db = dvs.get_app_db()

        vxlan.create_vlan1(dvs, vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "14.14.14.14")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, vni, vlan)
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", vni)])
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", map_key,
            {"vni": vni, "vrf": "default"})

        # The key deliberately has no CONFIG_DB '|' delimiter. vrfmgrd must
        # reject only this event and preserve the already converged mapping.
        self.create_entry_tbl(
            self.cdb, "VXLAN_TUNNEL_MAP", "malformed-key",
            [("vni", vni), ("vlan", vlan)])
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", map_key,
            {"vni": vni, "vrf": "default"})

        self.delete_entry_tbl(self.cdb, "VXLAN_TUNNEL_MAP", "malformed-key")
        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VRF_TABLE", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", map_key)
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, vni, vlan)
        vxlan.remove_evpn_nvo(dvs, nvo_name)
        vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan.remove_vlan(dvs, "170")

    def test_VRFMgr_DefaultVrfPendingDeleteBeforeDependencies(self, dvs, testlog):
        """A removed pending mapping must not return when dependencies arrive."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-default-pending-delete"
        nvo_name = "nvo-default-pending-delete"
        map_name = "map-default-pending-delete"
        vni = "5800"
        vlan = "Vlan180"
        map_key = "%s:evpn_map_%s_default" % (tunnel_name, vni)
        app_db = dvs.get_app_db()

        # Start with a valid logical association whose VXLAN dependencies do
        # not exist yet. It is retained in VRF_TABLE but cannot publish a
        # VXLAN_VRF_TABLE row.
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", vni)])
        app_db.wait_for_field_match(
            "VRF_TABLE", "default", {"vni": vni})
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", map_key)

        # Cancel the association before the missing dependencies arrive.
        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VRF_TABLE", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", map_key)

        # A late tunnel/NVO/map event must not replay the stale VNI.
        vxlan.create_vlan1(dvs, vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "15.15.15.15")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, vni, vlan)
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", map_key)
        assert self.vrf_tunnel_map_types(vni) == []

        # A fresh explicit add after dependency convergence must still work.
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", vni)])
        app_db.wait_for_field_match(
            "VRF_TABLE", "default", {"vni": vni})
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", map_key,
            {"vni": vni, "vrf": "default"})

        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VRF_TABLE", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", map_key)
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, vni, vlan)
        vxlan.remove_evpn_nvo(dvs, nvo_name)
        vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan.remove_vlan(dvs, "180")

    def test_VRFMgr_DefaultVrfL3VniOwnershipConflicts(self, dvs, testlog):
        """Keep conflicting ownership pending without publishing partial state."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-vrf-owner"
        nvo_name = "nvo-vrf-owner"
        tenant_first = "VrfTenantFirst"
        tenant_second = "VrfTenantSecond"
        app_db = dvs.get_app_db()
        initial_default_vrf = app_db.get_entry("VRF_TABLE", "default")

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

        # Tenant first: a direct CONFIG_DB writer can bypass CLI uniqueness
        # validation. vrfmgrd keeps the conflicting default SET pending before
        # publishing any APP_DB state, so a valid SET-before-DEL move can retry.
        self.create_entry_tbl(
            self.cdb, "VRF", tenant_first, [("vni", "5300")])
        app_db.wait_for_field_match(
            "VRF_TABLE", tenant_first, {"vni": "5300"})
        app_db.wait_for_entry("VXLAN_VRF_TABLE", tenant_map_key)
        marker = dvs.add_log_marker()
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", "5300")])
        # A baseline image may retain a logical default-VRF placeholder with
        # no VNI. Deferring this conflict must preserve that pre-existing row,
        # not require the whole APP_DB key to be absent.
        def default_conflict_deferred():
            _, output = dvs.runcmd([
                "sh", "-c",
                "awk '/{}/,ENDFILE {{print;}}' /var/log/syslog | "
                "grep -c 'Deferring VNI 5300 for VRF default; it is currently mapped to VRF {}'".format(
                    marker, tenant_first),
            ])
            return int(output.strip()) >= 1, output

        wait_for_result(
            default_conflict_deferred,
            failure_message="vrfmgrd did not defer the duplicate VNI owner",
        )
        assert app_db.get_entry("VRF_TABLE", "default") == initial_default_vrf
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", default_5300_key)
        (status, _) = dvs.runcmd("ip link show dev default")
        assert status != 0

        self.delete_entry_tbl(self.cdb, "VRF", tenant_first)
        app_db.wait_for_deleted_entry("VRF_TABLE", tenant_first)
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", tenant_map_key)
        app_db.wait_for_field_match(
            "VRF_TABLE", "default", {"vni": "5300"})
        app_db.wait_for_entry("VXLAN_VRF_TABLE", default_5300_key)
        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VRF_TABLE", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", default_5300_key)

        # Default first: defer the tenant before creating its Linux VRF,
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

        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VRF_TABLE", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", default_5400_key)
        app_db.wait_for_field_match(
            "VRF_TABLE", tenant_second, {"vni": "5400"})
        app_db.wait_for_entry("VXLAN_VRF_TABLE", tenant_5400_key)
        self.delete_entry_tbl(self.cdb, "VRF", tenant_second)
        app_db.wait_for_deleted_entry("VRF_TABLE", tenant_second)
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", tenant_5400_key)
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, "map-owner-5300", "5300", "Vlan130")
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, "map-owner-5400", "5400", "Vlan131")
        vxlan.remove_evpn_nvo(dvs, nvo_name)
        vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan.remove_vlan(dvs, "130")
        vxlan.remove_vlan(dvs, "131")

    def test_VRFMgr_DefaultVrfL3VniDependencyOrdering(self, dvs, testlog):
        """Publish intent before dependencies and let orchagent converge."""
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

        # NVO arrival is enough for vrfmgrd to publish the intent and for
        # VxlanVrfMapOrch to create the L3 tunnel-map directions. The L2
        # VNI-to-VLAN map is accepted independently when it appears.
        vxlan.create_vlan1(dvs, vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "8.8.8.8")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", map_key, {"vni": vni, "vrf": "default"})
        default_vr = self.default_virtual_router_id()
        self.wait_for_vrf_tunnel_map_association(
            vni, default_vr, "8.8.8.8")

        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, vni, vlan)
        app_db.wait_for_field_match(
            "VXLAN_TUNNEL_MAP_TABLE", "%s:%s" % (tunnel_name, map_name),
            {"vni": vni, "vlan": vlan})

        self.wait_for_vrf_tunnel_map_association(
            vni, default_vr, "8.8.8.8")

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

    def test_VRFMgr_UnrelatedTunnelMapDoesNotChangeL3Vni(self, dvs, testlog):
        """An unrelated L2VNI event must not alter an existing L3VNI map."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-default-stale-delete"
        nvo_name = "nvo-default-stale-delete"
        vni = "5900"
        unrelated_vni = "5910"
        vlan = "Vlan190"
        unrelated_vlan = "Vlan191"
        map_key = "%s:evpn_map_%s_default" % (tunnel_name, vni)
        app_db = dvs.get_app_db()

        vxlan.create_vlan1(dvs, vlan)
        vxlan.create_vlan1(dvs, unrelated_vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "16.16.16.16")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)

        try:
            # vrfmgrd publishes L3VNI intent without consuming tunnel-map
            # state. The L3 map is programmed independently of L2 VNI maps.
            self.create_entry_tbl(
                self.cdb, "VRF", "default", [("vni", vni)])
            app_db.wait_for_field_match(
                "VRF_TABLE", "default", {"vni": vni})
            app_db.wait_for_field_match(
                "VXLAN_VRF_TABLE", map_key,
                {"vni": vni, "vrf": "default"})
            default_vr = self.default_virtual_router_id()
            self.wait_for_vrf_tunnel_map_association(
                vni, default_vr, "16.16.16.16")

            # An unrelated L2 map does not alter the L3VNI request.
            vxlan.create_vxlan_tunnel_map(
                dvs, tunnel_name, "map-unrelated-5910",
                unrelated_vni, unrelated_vlan)
            app_db.wait_for_field_match(
                "VXLAN_TUNNEL_MAP_TABLE",
                "%s:map-unrelated-5910" % tunnel_name,
                {"vni": unrelated_vni, "vlan": unrelated_vlan})
            app_db.wait_for_field_match(
                "VXLAN_VRF_TABLE", map_key,
                {"vni": vni, "vrf": "default"})
            self.wait_for_vrf_tunnel_map_association(
                vni, default_vr, "16.16.16.16")

            # Once the matching dependency arrives, the newest desired state
            # is present and both SAI mapper directions must remain present.
            vxlan.create_vxlan_tunnel_map(
                dvs, tunnel_name, "map-default-5900", vni, vlan)
            app_db.wait_for_field_match(
                "VXLAN_VRF_TABLE", map_key,
                {"vni": vni, "vrf": "default"})
            self.wait_for_vrf_tunnel_map_association(
                vni, default_vr, "16.16.16.16")
        finally:
            self.delete_entry_tbl(self.cdb, "VRF", "default")
            vxlan.remove_vxlan_tunnel_map(
                dvs, tunnel_name, "map-default-5900", vni, vlan)
            vxlan.remove_vxlan_tunnel_map(
                dvs, tunnel_name, "map-unrelated-5910",
                unrelated_vni, unrelated_vlan)
            vxlan.remove_evpn_nvo(dvs, nvo_name)
            vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
            vxlan.remove_vlan(dvs, "190")
            vxlan.remove_vlan(dvs, "191")

    def test_VRFMgr_DefaultVrfVniFieldRemovalClearsStaleState(self, dvs, testlog):
        """Removing only the CONFIG_DB vni field must not leave its old value."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-default-field-remove"
        nvo_name = "nvo-default-field-remove"
        map_name = "map-default-field-remove"
        vni = "5920"
        vlan = "Vlan192"
        map_key = "%s:evpn_map_%s_default" % (tunnel_name, vni)
        app_db = dvs.get_app_db()
        config_db = dvs.get_config_db()

        vxlan.create_vlan1(dvs, vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "17.17.17.17")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, vni, vlan)

        try:
            # Keep another field in the row so hdel(vni) produces a SET-style
            # field-removal update rather than deleting the whole VRF key.
            self.create_entry_tbl(
                self.cdb, "VRF", "default",
                [("vni", vni), ("v4", "true")])
            app_db.wait_for_field_match(
                "VRF_TABLE", "default", {"vni": vni})
            app_db.wait_for_entry("VXLAN_VRF_TABLE", map_key)

            config_db.delete_field("VRF", "default", "vni")
            app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", map_key)
            app_db.wait_for_field_match(
                "VRF_TABLE", "default", {"vni": "0"})

            app_vrf = app_db.get_entry("VRF_TABLE", "default")
            assert app_vrf.get("vni") == "0", \
                "removed CONFIG_DB vni did not publish the unmap lifecycle value: %s" % app_vrf
        finally:
            self.delete_entry_tbl(self.cdb, "VRF", "default")
            vxlan.remove_vxlan_tunnel_map(
                dvs, tunnel_name, map_name, vni, vlan)
            vxlan.remove_evpn_nvo(dvs, nvo_name)
            vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
            vxlan.remove_vlan(dvs, "192")

    def test_VRFMgr_ExistingTunnelMapFieldRemovalDoesNotWithdrawL3Vni(self, dvs, testlog):
        """An ignored raw field removal must not alter accepted L3VNI state."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-map-field-remove"
        nvo_name = "nvo-map-field-remove"
        map_name = "map-field-remove"
        barrier_map_name = "zz-field-remove-barrier"
        vni = "5930"
        barrier_vni = "5931"
        vlan = "Vlan193"
        barrier_vlan = "Vlan194"
        config_key = "%s|%s" % (tunnel_name, map_name)
        tunnel_map_app_key = "%s:%s" % (tunnel_name, map_name)
        barrier_map_app_key = "%s:%s" % (tunnel_name, barrier_map_name)
        app_key = "%s:evpn_map_%s_default" % (tunnel_name, vni)
        app_db = dvs.get_app_db()
        config_db = dvs.get_config_db()

        vxlan.create_vlan1(dvs, vlan)
        vxlan.create_vlan1(dvs, barrier_vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "18.18.18.18")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, vni, vlan)

        try:
            self.create_entry_tbl(
                self.cdb, "VRF", "default", [("vni", vni)])
            app_db.wait_for_entry("VXLAN_VRF_TABLE", app_key)

            config_db.delete_field(
                "VXLAN_TUNNEL_MAP", config_key, "vni")
            vxlan.create_vxlan_tunnel_map(
                dvs, tunnel_name, barrier_map_name,
                barrier_vni, barrier_vlan)
            app_db.wait_for_field_match(
                "VXLAN_TUNNEL_MAP_TABLE", barrier_map_app_key,
                {"vni": barrier_vni, "vlan": barrier_vlan})

            app_db.wait_for_field_match(
                "VXLAN_TUNNEL_MAP_TABLE", tunnel_map_app_key,
                {"vni": vni, "vlan": vlan})
            app_db.wait_for_field_match(
                "VXLAN_VRF_TABLE", app_key,
                {"vni": vni, "vrf": "default"})
            app_db.wait_for_field_match(
                "VRF_TABLE", "default", {"vni": vni})
            default_vr = self.default_virtual_router_id()
            self.wait_for_vrf_tunnel_map_association(
                vni, default_vr, "18.18.18.18")
        finally:
            self.delete_entry_tbl(self.cdb, "VRF", "default")
            vxlan.remove_vxlan_tunnel_map(
                dvs, tunnel_name, barrier_map_name,
                barrier_vni, barrier_vlan)
            vxlan.remove_vxlan_tunnel_map(
                dvs, tunnel_name, map_name, vni, vlan)
            vxlan.remove_evpn_nvo(dvs, nvo_name)
            vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
            vxlan.remove_vlan(dvs, "193")
            vxlan.remove_vlan(dvs, "194")

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
        default_vr = self.default_virtual_router_id()
        self.wait_for_vrf_tunnel_map_association(
            vni, default_vr, "12.12.12.12")

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
            # Validate both directions against the post-restart default VR and
            # the intended tunnel; mapper OIDs may be recreated during replay.
            default_vr = self.default_virtual_router_id()
            self.wait_for_vrf_tunnel_map_association(
                vni, default_vr, "12.12.12.12")
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

    def test_VRFMgr_DefaultVrfL3VniExistingNvoUpdateIsIgnored(self, dvs, testlog):
        """Keep the first accepted NVO when its CONFIG_DB row is updated."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        old_tunnel = "tunnel-default-old"
        new_tunnel = "tunnel-default-new"
        nvo_name = "nvo-default-retarget"
        vni = "5200"
        vlan = "Vlan120"
        old_key = "%s:evpn_map_%s_default" % (old_tunnel, vni)
        new_key = "%s:evpn_map_%s_default" % (new_tunnel, vni)
        old_tunnel_map_key = "%s:map-default-old" % old_tunnel
        new_tunnel_map_key = "%s:map-default-new" % new_tunnel
        app_db = dvs.get_app_db()

        vxlan.create_vlan1(dvs, vlan)
        vxlan.create_vxlan_tunnel(dvs, old_tunnel, "9.9.9.9")
        vxlan.create_vxlan_tunnel(dvs, new_tunnel, "10.10.10.10")
        vxlan.create_vxlan_tunnel_map(
            dvs, old_tunnel, "map-default-old", vni, vlan)
        vxlan.create_evpn_nvo(dvs, nvo_name, old_tunnel)
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", vni)])

        app_db.wait_for_field_match(
            "VXLAN_EVPN_NVO_TABLE", nvo_name,
            {"source_vtep": old_tunnel})
        app_db.wait_for_field_match(
            "VXLAN_TUNNEL_MAP_TABLE", old_tunnel_map_key,
            {"vni": vni, "vlan": vlan})
        app_db.wait_for_deleted_entry(
            "VXLAN_TUNNEL_MAP_TABLE", new_tunnel_map_key)
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", old_key,
            {"vni": vni, "vrf": "default"})
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", new_key)
        default_vr = self.default_virtual_router_id()
        self.assert_vrf_tunnel_map_association(
            vni, default_vr, "9.9.9.9", excluded_source_ip="10.10.10.10")

        marker = dvs.add_log_marker()
        vxlan.create_evpn_nvo(dvs, nvo_name, new_tunnel)

        def vrfmgr_ignored_existing_nvo_update():
            _, output = dvs.runcmd([
                "sh", "-c",
                "awk '/{}/,ENDFILE {{print;}}' /var/log/syslog | "
                "grep -c 'Ignoring update for existing EVPN NVO {}'".format(
                    marker, nvo_name),
            ])
            return int(output.strip()) >= 1, output

        wait_for_result(vrfmgr_ignored_existing_nvo_update)
        app_db.wait_for_field_match(
            "VXLAN_EVPN_NVO_TABLE", nvo_name,
            {"source_vtep": old_tunnel})
        app_db.wait_for_field_match(
            "VXLAN_TUNNEL_MAP_TABLE", old_tunnel_map_key,
            {"vni": vni, "vlan": vlan})
        app_db.wait_for_deleted_entry(
            "VXLAN_TUNNEL_MAP_TABLE", new_tunnel_map_key)
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", old_key,
            {"vni": vni, "vrf": "default"})
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", new_key)
        self.assert_vrf_tunnel_map_association(
            vni, default_vr, "9.9.9.9", excluded_source_ip="10.10.10.10")

        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VRF_TABLE", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", old_key)
        vxlan.remove_vxlan_tunnel_map(
            dvs, old_tunnel, "map-default-old", vni, vlan)
        vxlan.remove_evpn_nvo(dvs, nvo_name)
        vxlan.remove_vxlan_tunnel(dvs, old_tunnel)
        vxlan.remove_vxlan_tunnel(dvs, new_tunnel)
        vxlan.remove_vlan(dvs, "120")

    def test_VRFMgr_NvoBeforeTunnelConverges(self, dvs, testlog):
        """Keep the baseline intent flow when the NVO precedes its tunnel."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-nvo-acceptance-pending"
        nvo_name = "nvo-acceptance-pending"
        vni = "5950"
        map_key = "%s:evpn_map_%s_default" % (tunnel_name, vni)
        app_db = dvs.get_app_db()

        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", vni)])
        app_db.wait_for_field_match("VRF_TABLE", "default", {"vni": vni})

        marker = dvs.add_log_marker()
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)

        def pending_nvo_was_processed():
            _, output = dvs.runcmd([
                "sh", "-c",
                "awk '/{}/,ENDFILE {{print;}}' /var/log/syslog | "
                "grep -c 'NVO {} creation failed. VTEP not present'".format(
                    marker, nvo_name),
            ])
            return int(output.strip()) >= 1, output

        wait_for_result(pending_nvo_was_processed)
        app_db.wait_for_deleted_entry("VXLAN_EVPN_NVO_TABLE", nvo_name)
        # vrfmgrd publishes CONFIG_DB intent without waiting on vxlanmgrd.
        # VxlanVrfMapOrch keeps the entry pending until the tunnel exists.
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", map_key,
            {"vni": vni, "vrf": "default"})

        # Once the tunnel appears, the independently pending paths converge.
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "22.22.22.22")
        app_db.wait_for_field_match(
            "VXLAN_EVPN_NVO_TABLE", nvo_name,
            {"source_vtep": tunnel_name})
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", map_key,
            {"vni": vni, "vrf": "default"})

        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VRF_TABLE", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", map_key)
        vxlan.remove_evpn_nvo(dvs, nvo_name)
        app_db.wait_for_deleted_entry("VXLAN_EVPN_NVO_TABLE", nvo_name)
        vxlan.remove_vxlan_tunnel(dvs, tunnel_name)

    def test_VRFMgr_NvoDeleteUsesBaselineIndependentManagerFlow(self, dvs, testlog):
        """Withdraw VRF intent without coupling vrfmgrd to vxlanmgrd state."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-nvo-delete-pending"
        nvo_name = "nvo-delete-pending"
        default_vni = "5940"
        named_vni = "5941"
        replacement_vni = "5942"
        default_vlan = "Vlan195"
        named_vlan = "Vlan196"
        named_vrf = "VrfNvoDel"
        replacement_tunnel = "tunnel-nvo-delete-readd"
        default_map_name = "map-nvo-delete-default"
        named_map_name = "map-nvo-delete-named"
        default_vrf_map_key = "%s:evpn_map_%s_default" % (
            tunnel_name, default_vni)
        named_vrf_map_key = "%s:evpn_map_%s_%s" % (
            tunnel_name, named_vni, named_vrf)
        replacement_map_key = "%s:evpn_map_%s_default" % (
            replacement_tunnel, replacement_vni)
        app_db = dvs.get_app_db()

        vxlan.create_vlan1(dvs, default_vlan)
        vxlan.create_vlan1(dvs, named_vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "19.19.19.19")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, default_map_name, default_vni, default_vlan)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, named_map_name, named_vni, named_vlan)
        initial_vrs = self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        default_vr = self.default_virtual_router_id()
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", default_vni)])
        self.create_entry_tbl(
            self.cdb, "VRF", named_vrf, [("vni", named_vni)])

        app_db.wait_for_field_match(
            "VXLAN_EVPN_NVO_TABLE", nvo_name,
            {"source_vtep": tunnel_name})
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", default_vrf_map_key,
            {"vni": default_vni, "vrf": "default"})
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", named_vrf_map_key,
            {"vni": named_vni, "vrf": named_vrf})
        named_vrs = self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") - initial_vrs
        assert len(named_vrs) == 1, \
            "expected one named VR object, found %s" % named_vrs
        named_vr = next(iter(named_vrs))
        self.wait_for_vrf_tunnel_map_association(
            default_vni, default_vr, "19.19.19.19")
        self.wait_for_vrf_tunnel_map_association(
            named_vni, named_vr, "19.19.19.19")

        vxlan.remove_evpn_nvo(dvs, nvo_name)

        # vxlanmgrd cannot remove its NVO row while the L2 maps retain the
        # tunnel. vrfmgrd retains the baseline independent flow and withdraws
        # its named/default VRF-map intent immediately.
        app_db.wait_for_field_match(
            "VXLAN_EVPN_NVO_TABLE", nvo_name,
            {"source_vtep": tunnel_name})
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", default_vrf_map_key)
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", named_vrf_map_key)

        # Remove the VRF associations and the references that blocked the NVO
        # deletion. vxlanmgrd can now complete its independently pending NVO
        # delete; vrfmgrd has no APP_DB dependency on that completion.
        self.delete_entry_tbl(self.cdb, "VRF", "default")
        self.delete_entry_tbl(self.cdb, "VRF", named_vrf)
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", default_vrf_map_key)
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", named_vrf_map_key)
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, default_map_name, default_vni, default_vlan)
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, named_map_name, named_vni, named_vlan)
        app_db.wait_for_deleted_entry("VXLAN_EVPN_NVO_TABLE", nvo_name)

        # Recreate the NVO on a different tunnel. Publishing the next mapping
        # under that tunnel proves vrfmgrd cleared its earlier config-driven
        # NVO state without waiting on vxlanmgrd.
        vxlan.create_vxlan_tunnel(
            dvs, replacement_tunnel, "19.19.19.20")
        vxlan.create_evpn_nvo(dvs, nvo_name, replacement_tunnel)
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", replacement_vni)])
        app_db.wait_for_field_match(
            "VXLAN_EVPN_NVO_TABLE", nvo_name,
            {"source_vtep": replacement_tunnel})
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", replacement_map_key,
            {"vni": replacement_vni, "vrf": "default"})
        self.wait_for_vrf_tunnel_map_association(
            replacement_vni, default_vr, "19.19.19.20")

        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", replacement_map_key)
        vxlan.remove_evpn_nvo(dvs, nvo_name)
        app_db.wait_for_deleted_entry("VXLAN_EVPN_NVO_TABLE", nvo_name)

        vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan.remove_vxlan_tunnel(dvs, replacement_tunnel)
        vxlan.remove_vlan(dvs, "195")
        vxlan.remove_vlan(dvs, "196")

    def test_VRFMgr_ExistingTunnelMapUpdateDoesNotWithdrawL3Vni(self, dvs, testlog):
        """Use the tunnel-map state accepted by vxlanmgrd, not its raw update."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-default-map-update"
        nvo_name = "nvo-default-map-update"
        map_name = "map-default-update"
        barrier_map_name = "zz-map-update-barrier"
        old_vni = "5250"
        new_vni = "5251"
        barrier_vni = "5252"
        old_vlan = "Vlan125"
        new_vlan = "Vlan126"
        barrier_vlan = "Vlan127"
        tunnel_map_key = "%s:%s" % (tunnel_name, map_name)
        barrier_map_key = "%s:%s" % (tunnel_name, barrier_map_name)
        vrf_map_key = "%s:evpn_map_%s_default" % (tunnel_name, old_vni)
        app_db = dvs.get_app_db()

        vxlan.create_vlan1(dvs, old_vlan)
        vxlan.create_vlan1(dvs, new_vlan)
        vxlan.create_vlan1(dvs, barrier_vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "10.25.25.25")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, old_vni, old_vlan)
        self.create_entry_tbl(
            self.cdb, "VRF", "default", [("vni", old_vni)])

        app_db.wait_for_field_match(
            "VXLAN_TUNNEL_MAP_TABLE", tunnel_map_key,
            {"vni": old_vni, "vlan": old_vlan})
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", vrf_map_key,
            {"vni": old_vni, "vrf": "default"})

        # vxlanmgrd ignores SET for an existing tunnel-map key. The barrier is
        # a later key in the same CONFIG_DB table; observing it in APP_DB proves
        # the owning table consumer processed the preceding SET.
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, new_vni, new_vlan)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, barrier_map_name, barrier_vni, barrier_vlan)
        app_db.wait_for_field_match(
            "VXLAN_TUNNEL_MAP_TABLE", barrier_map_key,
            {"vni": barrier_vni, "vlan": barrier_vlan})

        app_db.wait_for_field_match(
            "VXLAN_TUNNEL_MAP_TABLE", tunnel_map_key,
            {"vni": old_vni, "vlan": old_vlan})
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", vrf_map_key,
            {"vni": old_vni, "vrf": "default"})
        default_vr = self.default_virtual_router_id()
        self.wait_for_vrf_tunnel_map_association(
            old_vni, default_vr, "10.25.25.25")
        assert self.vrf_tunnel_map_types(new_vni) == []

        self.delete_entry_tbl(self.cdb, "VRF", "default")
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", vrf_map_key)
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, barrier_map_name, barrier_vni, barrier_vlan)
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, map_name, new_vni, new_vlan)
        vxlan.remove_evpn_nvo(dvs, nvo_name)
        vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan.remove_vlan(dvs, "125")
        vxlan.remove_vlan(dvs, "126")
        vxlan.remove_vlan(dvs, "127")

    def test_VRFMgr_RejectedDuplicateTunnelMapDoesNotAffectL3Vni(self, dvs, testlog):
        """A rejected L2 map must never become a vrfmgrd dependency."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-default-duplicate-map"
        nvo_name = "nvo-default-duplicate-map"
        accepted_map_name = "map-accepted-5260"
        rejected_map_name = "map-rejected-5260"
        vni = "5260"
        accepted_vlan = "Vlan128"
        rejected_vlan = "Vlan129"
        accepted_app_key = "%s:%s" % (tunnel_name, accepted_map_name)
        rejected_app_key = "%s:%s" % (tunnel_name, rejected_map_name)
        vrf_map_key = "%s:evpn_map_%s_default" % (tunnel_name, vni)
        app_db = dvs.get_app_db()

        vxlan.create_vlan1(dvs, accepted_vlan)
        vxlan.create_vlan1(dvs, rejected_vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "10.26.26.26")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)

        try:
            vxlan.create_vxlan_tunnel_map(
                dvs, tunnel_name, accepted_map_name, vni, accepted_vlan)
            app_db.wait_for_field_match(
                "VXLAN_TUNNEL_MAP_TABLE", accepted_app_key,
                {"vni": vni, "vlan": accepted_vlan})

            self.create_entry_tbl(
                self.cdb, "VRF", "default", [("vni", vni)])
            app_db.wait_for_field_match(
                "VXLAN_VRF_TABLE", vrf_map_key,
                {"vni": vni, "vrf": "default"})
            default_vr = self.default_virtual_router_id()
            self.wait_for_vrf_tunnel_map_association(
                vni, default_vr, "10.26.26.26")

            marker = dvs.add_log_marker()
            vxlan.create_vxlan_tunnel_map(
                dvs, tunnel_name, rejected_map_name, vni, rejected_vlan)

            def duplicate_map_rejected():
                _, output = dvs.runcmd([
                    "sh", "-c",
                    "awk '/{}/,ENDFILE {{print;}}' /var/log/syslog | "
                    "grep -c 'VNI {} already mapped. Map Create failed for : {}:{}'".format(
                        marker, vni, tunnel_name, rejected_map_name),
                ])
                return int(output.strip()) >= 1, output

            wait_for_result(duplicate_map_rejected)
            app_db.wait_for_deleted_entry(
                "VXLAN_TUNNEL_MAP_TABLE", rejected_app_key)
            app_db.wait_for_field_match(
                "VXLAN_TUNNEL_MAP_TABLE", accepted_app_key,
                {"vni": vni, "vlan": accepted_vlan})

            # Remove the only map vxlanmgrd accepted. The rejected CONFIG_DB
            # row must not act as a hidden reference in vrfmgrd. L3VNI intent
            # remains until its VRF mapping is explicitly removed.
            vxlan.remove_vxlan_tunnel_map(
                dvs, tunnel_name, accepted_map_name, vni, accepted_vlan)
            app_db.wait_for_deleted_entry(
                "VXLAN_TUNNEL_MAP_TABLE", accepted_app_key)
            app_db.wait_for_deleted_entry(
                "VXLAN_TUNNEL_MAP_TABLE", rejected_app_key)
            app_db.wait_for_field_match(
                "VXLAN_VRF_TABLE", vrf_map_key,
                {"vni": vni, "vrf": "default"})
            self.wait_for_vrf_tunnel_map_association(
                vni, default_vr, "10.26.26.26")
        finally:
            self.delete_entry_tbl(self.cdb, "VRF", "default")
            vxlan.remove_vxlan_tunnel_map(
                dvs, tunnel_name, accepted_map_name, vni, accepted_vlan)
            vxlan.remove_vxlan_tunnel_map(
                dvs, tunnel_name, rejected_map_name, vni, rejected_vlan)
            vxlan.remove_evpn_nvo(dvs, nvo_name)
            vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
            vxlan.remove_vlan(dvs, "128")
            vxlan.remove_vlan(dvs, "129")

    def test_VRFMgr_RejectedDuplicateVlanMapDoesNotAffectL3Vni(self, dvs, testlog):
        """A map rejected for duplicate VLAN must not alter L3VNI state."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-default-duplicate-vlan"
        nvo_name = "nvo-default-duplicate-vlan"
        accepted_map_name = "map-accepted-vlan132"
        rejected_map_name = "map-rejected-vlan132"
        accepted_vni = "5270"
        rejected_vni = "5271"
        vlan = "Vlan132"
        accepted_app_key = "%s:%s" % (tunnel_name, accepted_map_name)
        rejected_app_key = "%s:%s" % (tunnel_name, rejected_map_name)
        vrf_map_key = "%s:evpn_map_%s_default" % (
            tunnel_name, accepted_vni)
        app_db = dvs.get_app_db()

        vxlan.create_vlan1(dvs, vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "10.27.27.27")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)

        try:
            vxlan.create_vxlan_tunnel_map(
                dvs, tunnel_name, accepted_map_name, accepted_vni, vlan)
            app_db.wait_for_field_match(
                "VXLAN_TUNNEL_MAP_TABLE", accepted_app_key,
                {"vni": accepted_vni, "vlan": vlan})

            self.create_entry_tbl(
                self.cdb, "VRF", "default", [("vni", accepted_vni)])
            app_db.wait_for_field_match(
                "VXLAN_VRF_TABLE", vrf_map_key,
                {"vni": accepted_vni, "vrf": "default"})
            default_vr = self.default_virtual_router_id()
            self.wait_for_vrf_tunnel_map_association(
                accepted_vni, default_vr, "10.27.27.27")

            marker = dvs.add_log_marker()
            vxlan.create_vxlan_tunnel_map(
                dvs, tunnel_name, rejected_map_name, rejected_vni, vlan)

            def duplicate_vlan_map_rejected():
                _, output = dvs.runcmd([
                    "sh", "-c",
                    "awk '/{}/,ENDFILE {{print;}}' /var/log/syslog | "
                    "grep -c 'Vlan {} already mapped. Map Create failed for : {}:{}'".format(
                        marker, vlan, tunnel_name, rejected_map_name),
                ])
                return int(output.strip()) >= 1, output

            wait_for_result(duplicate_vlan_map_rejected)
            app_db.wait_for_deleted_entry(
                "VXLAN_TUNNEL_MAP_TABLE", rejected_app_key)
            app_db.wait_for_field_match(
                "VXLAN_TUNNEL_MAP_TABLE", accepted_app_key,
                {"vni": accepted_vni, "vlan": vlan})
            app_db.wait_for_field_match(
                "VXLAN_VRF_TABLE", vrf_map_key,
                {"vni": accepted_vni, "vrf": "default"})
            self.wait_for_vrf_tunnel_map_association(
                accepted_vni, default_vr, "10.27.27.27")
            assert self.vrf_tunnel_map_types(rejected_vni) == []
        finally:
            self.delete_entry_tbl(self.cdb, "VRF", "default")
            vxlan.remove_vxlan_tunnel_map(
                dvs, tunnel_name, accepted_map_name, accepted_vni, vlan)
            vxlan.remove_vxlan_tunnel_map(
                dvs, tunnel_name, rejected_map_name, rejected_vni, vlan)
            vxlan.remove_evpn_nvo(dvs, nvo_name)
            vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
            vxlan.remove_vlan(dvs, "132")

    def test_VRFMgr_RejectedTunnelMapCanBeResubmitted(self, dvs, testlog):
        """A rejected map can be accepted after its VNI conflict is removed."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-default-map-resubmit"
        nvo_name = "nvo-default-map-resubmit"
        first_map_name = "map-first-5280"
        replacement_map_name = "map-replacement-5280"
        vni = "5280"
        first_vlan = "Vlan133"
        replacement_vlan = "Vlan134"
        first_app_key = "%s:%s" % (tunnel_name, first_map_name)
        replacement_app_key = "%s:%s" % (
            tunnel_name, replacement_map_name)
        vrf_map_key = "%s:evpn_map_%s_default" % (tunnel_name, vni)
        app_db = dvs.get_app_db()

        vxlan.create_vlan1(dvs, first_vlan)
        vxlan.create_vlan1(dvs, replacement_vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "10.28.28.28")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)

        try:
            vxlan.create_vxlan_tunnel_map(
                dvs, tunnel_name, first_map_name, vni, first_vlan)
            app_db.wait_for_field_match(
                "VXLAN_TUNNEL_MAP_TABLE", first_app_key,
                {"vni": vni, "vlan": first_vlan})

            self.create_entry_tbl(
                self.cdb, "VRF", "default", [("vni", vni)])
            app_db.wait_for_field_match(
                "VXLAN_VRF_TABLE", vrf_map_key,
                {"vni": vni, "vrf": "default"})
            default_vr = self.default_virtual_router_id()
            self.wait_for_vrf_tunnel_map_association(
                vni, default_vr, "10.28.28.28")

            marker = dvs.add_log_marker()
            vxlan.create_vxlan_tunnel_map(
                dvs, tunnel_name, replacement_map_name,
                vni, replacement_vlan)

            def replacement_map_rejected():
                _, output = dvs.runcmd([
                    "sh", "-c",
                    "awk '/{}/,ENDFILE {{print;}}' /var/log/syslog | "
                    "grep -c 'VNI {} already mapped. Map Create failed for : {}:{}'".format(
                        marker, vni, tunnel_name, replacement_map_name),
                ])
                return int(output.strip()) >= 1, output

            wait_for_result(replacement_map_rejected)
            app_db.wait_for_deleted_entry(
                "VXLAN_TUNNEL_MAP_TABLE", replacement_app_key)

            vxlan.remove_vxlan_tunnel_map(
                dvs, tunnel_name, first_map_name, vni, first_vlan)
            app_db.wait_for_deleted_entry(
                "VXLAN_TUNNEL_MAP_TABLE", first_app_key)

            # Re-submit the previously rejected key after removing the owner.
            vxlan.create_vxlan_tunnel_map(
                dvs, tunnel_name, replacement_map_name,
                vni, replacement_vlan)
            app_db.wait_for_field_match(
                "VXLAN_TUNNEL_MAP_TABLE", replacement_app_key,
                {"vni": vni, "vlan": replacement_vlan})
            app_db.wait_for_field_match(
                "VXLAN_VRF_TABLE", vrf_map_key,
                {"vni": vni, "vrf": "default"})
            self.wait_for_vrf_tunnel_map_association(
                vni, default_vr, "10.28.28.28")
        finally:
            self.delete_entry_tbl(self.cdb, "VRF", "default")
            vxlan.remove_vxlan_tunnel_map(
                dvs, tunnel_name, first_map_name, vni, first_vlan)
            vxlan.remove_vxlan_tunnel_map(
                dvs, tunnel_name, replacement_map_name,
                vni, replacement_vlan)
            vxlan.remove_evpn_nvo(dvs, nvo_name)
            vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
            vxlan.remove_vlan(dvs, "133")
            vxlan.remove_vlan(dvs, "134")

    def test_VRFMgr_NamedVrfL3VniSurvivesL2MapRecreate(self, dvs, testlog):
        """Keep a named-VRF L3 map attached while its L2 map is recreated."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-named-map-recreate"
        nvo_name = "nvo-named-map-recreate"
        vrf_name = "VrfMapRecreate"
        first_map_name = "map-named-first-5290"
        replacement_map_name = "map-named-replacement-5290"
        vni = "5290"
        first_vlan = "Vlan135"
        replacement_vlan = "Vlan136"
        first_app_key = "%s:%s" % (tunnel_name, first_map_name)
        replacement_app_key = "%s:%s" % (
            tunnel_name, replacement_map_name)
        vrf_map_key = "%s:evpn_map_%s_%s" % (
            tunnel_name, vni, vrf_name)
        app_db = dvs.get_app_db()

        vxlan.create_vlan1(dvs, first_vlan)
        vxlan.create_vlan1(dvs, replacement_vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, "10.29.29.29")
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)
        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, first_map_name, vni, first_vlan)
        app_db.wait_for_field_match(
            "VXLAN_TUNNEL_MAP_TABLE", first_app_key,
            {"vni": vni, "vlan": first_vlan})

        initial_vrs = self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER")
        self.create_entry_tbl(
            self.cdb, "VRF", vrf_name, [("vni", vni)])
        app_db.wait_for_field_match(
            "VXLAN_VRF_TABLE", vrf_map_key,
            {"vni": vni, "vrf": vrf_name})
        named_vrs = self.entries(
            self.adb, "ASIC_STATE:SAI_OBJECT_TYPE_VIRTUAL_ROUTER") - initial_vrs
        assert len(named_vrs) == 1, \
            "expected one named VR object, found %s" % named_vrs
        named_vr = next(iter(named_vrs))
        self.wait_for_vrf_tunnel_map_association(
            vni, named_vr, "10.29.29.29")

        # This was a baseline lifecycle gap: the L2 delete drove the shared
        # tunnel reference count to zero even though the named L3 map remained.
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, first_map_name, vni, first_vlan)
        app_db.wait_for_deleted_entry(
            "VXLAN_TUNNEL_MAP_TABLE", first_app_key)
        self.wait_for_vrf_tunnel_map_association(
            vni, named_vr, "10.29.29.29")

        vxlan.create_vxlan_tunnel_map(
            dvs, tunnel_name, replacement_map_name,
            vni, replacement_vlan)
        app_db.wait_for_field_match(
            "VXLAN_TUNNEL_MAP_TABLE", replacement_app_key,
            {"vni": vni, "vlan": replacement_vlan})
        self.wait_for_vrf_tunnel_map_association(
            vni, named_vr, "10.29.29.29")

        self.delete_entry_tbl(self.cdb, "VRF", vrf_name)
        app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", vrf_map_key)
        vxlan.remove_vxlan_tunnel_map(
            dvs, tunnel_name, replacement_map_name,
            vni, replacement_vlan)
        vxlan.remove_evpn_nvo(dvs, nvo_name)
        vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan.remove_vlan(dvs, "135")
        vxlan.remove_vlan(dvs, "136")

    def test_VRFMgr_LastVrfMapRemovesSharedTunnelHardware(self, dvs, testlog):
        """Remove shared tunnel hardware when the VRF map is its last user."""
        self.setup_db(dvs)

        vxlan = VxlanTunnel()
        tunnel_name = "tunnel-last-vrf-map"
        nvo_name = "nvo-last-vrf-map"
        map_name = "map-last-vrf-5300"
        vni = "5300"
        vlan = "Vlan137"
        source_ip = "10.30.30.30"
        l2_map_key = "%s:%s" % (tunnel_name, map_name)
        vrf_map_key = "%s:evpn_map_%s_default" % (tunnel_name, vni)
        app_db = dvs.get_app_db()

        vxlan.create_vlan1(dvs, vlan)
        vxlan.create_vxlan_tunnel(dvs, tunnel_name, source_ip)
        vxlan.create_evpn_nvo(dvs, nvo_name, tunnel_name)

        try:
            vxlan.create_vxlan_tunnel_map(
                dvs, tunnel_name, map_name, vni, vlan)
            app_db.wait_for_field_match(
                "VXLAN_TUNNEL_MAP_TABLE", l2_map_key,
                {"vni": vni, "vlan": vlan})

            self.create_entry_tbl(
                self.cdb, "VRF", "default", [("vni", vni)])
            app_db.wait_for_field_match(
                "VXLAN_VRF_TABLE", vrf_map_key,
                {"vni": vni, "vrf": "default"})
            self.wait_for_vrf_tunnel_map_association(
                vni, self.default_virtual_router_id(), source_ip)

            # Delete the L2 map first. The L3 map remains as the only live
            # user, so the shared SIP tunnel must still be programmed.
            vxlan.remove_vxlan_tunnel_map(
                dvs, tunnel_name, map_name, vni, vlan)
            app_db.wait_for_deleted_entry(
                "VXLAN_TUNNEL_MAP_TABLE", l2_map_key)
            assert self.tunnel_mapper_ids(source_ip, required=False) is not None

            # Removing the final L3 map must now remove the shared tunnel and
            # termination hardware rather than leave an inactive orphan.
            self.delete_entry_tbl(self.cdb, "VRF", "default")
            app_db.wait_for_deleted_entry("VXLAN_VRF_TABLE", vrf_map_key)

            def tunnel_hardware_removed():
                result = self.tunnel_mapper_ids(source_ip, required=False)
                return result is None, result

            wait_for_result(tunnel_hardware_removed)
        finally:
            self.delete_entry_tbl(self.cdb, "VRF", "default")
            vxlan.remove_vxlan_tunnel_map(
                dvs, tunnel_name, map_name, vni, vlan)
            vxlan.remove_evpn_nvo(dvs, nvo_name)
            vxlan.remove_vxlan_tunnel(dvs, tunnel_name)
            vxlan.remove_vlan(dvs, "137")

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
