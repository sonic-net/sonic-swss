import pytest
import time
from swsscommon import swsscommon
from evpn_tunnel import VxlanTunnel, VxlanEvpnHelper
from typing import List, Dict, Tuple

tunnel_name = 'tunnel_2'
vni_id = '1000'
vlan_id = '100'
vlan = 'Vlan100'
vlanlist = [vlan_id]
vnilist = [vni_id]
map_name = 'map_1000_100'
src_ip = '6.6.6.6'
dst_ip_1 = '7.7.7.7'
dst_ip_2 = '8.8.8.8'
dst_ip_3 = '9.9.9.9'
nvo_name = 'nvo1'

nh_id_1 = '11'
nh_id_2 = '12'
nh_id_3 = '13'

nhg_id_1 = '123'
nhg_id_2 = '321'

l2_ecmp_grp_tb = "ASIC_STATE:SAI_OBJECT_TYPE_L2_ECMP_GROUP"
l2_ecmp_grp_member_tb = "ASIC_STATE:SAI_OBJECT_TYPE_L2_ECMP_GROUP_MEMBER"
tunnel_tb = "ASIC_STATE:SAI_OBJECT_TYPE_TUNNEL"
bridge_port_tb = "ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT"


def create_entry(tbl, key, pairs):
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)


def remove_entry(tbl, key):
    tbl._del(key)


def create_entry_pst(db, table, key, pairs):
    tbl = swsscommon.ProducerStateTable(db, table)
    create_entry(tbl, key, pairs)


def remove_entry_pst(db, table, key):
    tbl = swsscommon.ProducerStateTable(db, table)
    remove_entry(tbl, key)


class TestL2NhgOrch(object):

    def get_vxlan_obj(self):
        return VxlanTunnel()

    @pytest.fixture
    def setup_vxlan(self, dvs):
        dvs.setup_db()
        vxlan_obj = self.get_vxlan_obj()
        self.helper = VxlanEvpnHelper()

        dvs.runcmd("swssloglevel -l INFO -c orchagent")
        vxlan_obj.fetch_exist_entries(dvs)

        vxlan_obj.create_vlan1(dvs, vlan)
        vxlan_obj.create_vxlan_tunnel(dvs, tunnel_name, src_ip)
        vxlan_obj.create_evpn_nvo(dvs, nvo_name, tunnel_name)
        vxlan_obj.create_vxlan_tunnel_map(dvs, tunnel_name, map_name, vni_id, vlan)
        vxlan_obj.create_evpn_remote_vni(dvs, vlan, dst_ip_1, vni_id)
        vxlan_obj.create_evpn_remote_vni(dvs, vlan, dst_ip_2, vni_id)
        vxlan_obj.create_evpn_remote_vni(dvs, vlan, dst_ip_3, vni_id)
        yield
        vxlan_obj.remove_evpn_remote_vni(dvs, vlan, dst_ip_3)
        vxlan_obj.remove_evpn_remote_vni(dvs, vlan, dst_ip_2)
        vxlan_obj.remove_evpn_remote_vni(dvs, vlan, dst_ip_1)
        vxlan_obj.remove_vxlan_tunnel_map(dvs, tunnel_name, map_name, vni_id, vlan)
        vxlan_obj.remove_evpn_nvo(dvs, nvo_name)
        vxlan_obj.remove_vxlan_tunnel(dvs, tunnel_name)
        vxlan_obj.remove_vlan(dvs, vlan_id)
        time.sleep(5)

    def create_l2_nexthop(self, dvs, id: str, ip: str):
        create_entry_pst(dvs.pdb, swsscommon.APP_L2_NEXTHOP_GROUP_TABLE_NAME,
                         id, [("remote_vtep", ip)])

    def create_l2_nexthop_group(self, dvs, id: str, nh_ids: List[str], nh_ips: List[str]):
        old_grps = dvs.get_asic_db().get_keys(l2_ecmp_grp_tb)
        old_members = dvs.get_asic_db().get_keys(l2_ecmp_grp_member_tb)
        old_bps = dvs.get_asic_db().get_keys(bridge_port_tb)

        create_entry_pst(dvs.pdb, swsscommon.APP_L2_NEXTHOP_GROUP_TABLE_NAME,
                         id, [("nexthop_group", ",".join(nh_ids))])

        l2_ecmp_grps = dvs.get_asic_db().wait_for_n_keys(l2_ecmp_grp_tb, len(old_grps) + 1)
        assert len(l2_ecmp_grps) == len(old_grps) + 1
        l2_ecmp_grp_m = dvs.get_asic_db().wait_for_n_keys(l2_ecmp_grp_member_tb, len(old_members) + len(nh_ids))
        assert len(l2_ecmp_grp_m) == len(old_members) + len(nh_ids)
        bridge_ports = dvs.get_asic_db().wait_for_n_keys(bridge_port_tb, len(old_bps) + 1)
        assert len(bridge_ports) == len(old_bps) + 1

        new_nhg = list(set(l2_ecmp_grps) - set(old_grps))[0]
        new_nh = list(set(l2_ecmp_grp_m) - set(old_members))
        new_bp_key = list(set(bridge_ports) - set(old_bps))[0]

        for nh in [dvs.get_asic_db().get_entry(l2_ecmp_grp_member_tb, k) for k in new_nh]:
            assert nh['SAI_L2_ECMP_GROUP_MEMBER_ATTR_L2_ECMP_GROUP_ID'] == new_nhg
            tun = dvs.get_asic_db().get_entry(tunnel_tb, nh['SAI_L2_ECMP_GROUP_MEMBER_ATTR_TUNNEL_ID'])
            assert tun['SAI_TUNNEL_ATTR_TYPE'] == "SAI_TUNNEL_TYPE_VXLAN"
            assert tun['SAI_TUNNEL_ATTR_PEER_MODE'] == "SAI_TUNNEL_PEER_MODE_P2P"
            assert tun['SAI_TUNNEL_ATTR_ENCAP_SRC_IP'] == src_ip
            assert tun['SAI_TUNNEL_ATTR_ENCAP_DST_IP'] in nh_ips

        new_bp = dvs.get_asic_db().get_entry(bridge_port_tb, new_bp_key)
        assert new_bp['SAI_BRIDGE_PORT_ATTR_TYPE'] == 'SAI_BRIDGE_PORT_TYPE_L2_ECMP_GROUP'
        assert 'SAI_BRIDGE_PORT_ATTR_L2_ECMP_GROUP_ID' in new_bp
        assert new_bp['SAI_BRIDGE_PORT_ATTR_L2_ECMP_GROUP_ID'] == new_nhg

        return new_nhg, new_nh, new_bp_key

    def add_l2_nexthop_to_group(self, dvs, id: str, new_nh_ids: List[str], new_nh_ips: List[str],
                                nhg_oid: str, old_nh_oids: List[str]) -> Tuple[str]:
        old_members = dvs.get_asic_db().get_keys(l2_ecmp_grp_member_tb)

        len_new_members = len(old_members) + (len(new_nh_ids) - len(old_nh_oids))

        create_entry_pst(dvs.pdb, swsscommon.APP_L2_NEXTHOP_GROUP_TABLE_NAME,
                         id, [("nexthop_group", ",".join(new_nh_ids))])

        l2_ecmp_grp_m = dvs.get_asic_db().wait_for_n_keys(l2_ecmp_grp_member_tb, len_new_members)
        assert len(l2_ecmp_grp_m) == len_new_members

        new_nh = list(set(l2_ecmp_grp_m) - set(old_members))

        for nh, oid in [(dvs.get_asic_db().get_entry(l2_ecmp_grp_member_tb, k), k) for k in new_nh]:
            assert nh['SAI_L2_ECMP_GROUP_MEMBER_ATTR_L2_ECMP_GROUP_ID'] == nhg_oid
            tun = dvs.get_asic_db().get_entry(tunnel_tb, nh['SAI_L2_ECMP_GROUP_MEMBER_ATTR_TUNNEL_ID'])
            assert tun['SAI_TUNNEL_ATTR_TYPE'] == "SAI_TUNNEL_TYPE_VXLAN"
            assert tun['SAI_TUNNEL_ATTR_PEER_MODE'] == "SAI_TUNNEL_PEER_MODE_P2P"
            assert tun['SAI_TUNNEL_ATTR_ENCAP_SRC_IP'] == src_ip
            assert tun['SAI_TUNNEL_ATTR_ENCAP_DST_IP'] in new_nh_ips

        return new_nh

    def remove_l2_nexthop_from_group(self, dvs, id: str, new_nh_ids: List[str], new_nh_ips: List[str],
                                nhg_oid: str, old_nh_oids: List[str]) -> Tuple[str]:
        old_members = dvs.get_asic_db().get_keys(l2_ecmp_grp_member_tb)
        len_new_members = len(old_members) - (len(old_nh_oids) - len(new_nh_ids))

        create_entry_pst(dvs.pdb, swsscommon.APP_L2_NEXTHOP_GROUP_TABLE_NAME,
                         id, [("nexthop_group", ",".join(new_nh_ids))])

        l2_ecmp_grp_m = dvs.get_asic_db().wait_for_n_keys(l2_ecmp_grp_member_tb, len_new_members)
        assert len(l2_ecmp_grp_m) == len_new_members

        new_nh = list()
        for k in dvs.get_asic_db().get_keys(l2_ecmp_grp_member_tb):
            if dvs.get_asic_db().get_entry(l2_ecmp_grp_member_tb, k)['SAI_L2_ECMP_GROUP_MEMBER_ATTR_L2_ECMP_GROUP_ID'] == nhg_oid:
                new_nh.append(k)

        for nh, oid in [(dvs.get_asic_db().get_entry(l2_ecmp_grp_member_tb, k), k) for k in new_nh]:
            assert nh['SAI_L2_ECMP_GROUP_MEMBER_ATTR_L2_ECMP_GROUP_ID'] == nhg_oid
            tun = dvs.get_asic_db().get_entry(tunnel_tb, nh['SAI_L2_ECMP_GROUP_MEMBER_ATTR_TUNNEL_ID'])
            assert tun['SAI_TUNNEL_ATTR_TYPE'] == "SAI_TUNNEL_TYPE_VXLAN"
            assert tun['SAI_TUNNEL_ATTR_PEER_MODE'] == "SAI_TUNNEL_PEER_MODE_P2P"
            assert tun['SAI_TUNNEL_ATTR_ENCAP_SRC_IP'] == src_ip
            assert tun['SAI_TUNNEL_ATTR_ENCAP_DST_IP'] in new_nh_ips

        return new_nh

    def remove_l2_nexthop_group(self, dvs, nhg_id: str, nhg_oid: str,
                                nh_oids: List[str], bp_oid: str):
        remove_entry_pst(dvs.pdb, swsscommon.APP_L2_NEXTHOP_GROUP_TABLE_NAME, nhg_id)
        dvs.get_asic_db().wait_for_deleted_entry(l2_ecmp_grp_tb, nhg_id)
        for oid in nh_oids:
            dvs.get_asic_db().wait_for_deleted_entry(l2_ecmp_grp_member_tb, oid)
        dvs.get_asic_db().wait_for_deleted_entry(bridge_port_tb, bp_oid)

    def remove_l2_nexthop(self, dvs, nh_id: str, nh_oid: str = ""):
        remove_entry_pst(dvs.pdb, swsscommon.APP_L2_NEXTHOP_GROUP_TABLE_NAME, nh_id)
        if nh_oid:
            dvs.get_asic_db().wait_for_deleted_entry(l2_ecmp_grp_member_tb, nh_oid)

    # ----------------------------------------------------------------------------------------

    def test_create_l2_nexthop_group_1_nh(self, dvs, testlog, setup_vxlan):
        self.create_l2_nexthop(dvs, nh_id_1, dst_ip_1)
        nhg_oid, nh_oids, bp_oid = self.create_l2_nexthop_group(dvs, nhg_id_1, [nh_id_1], [dst_ip_1])
        self.remove_l2_nexthop_group(dvs, nhg_id_1, nhg_oid, nh_oids, bp_oid)
        self.remove_l2_nexthop(dvs, nh_id_1)

    def test_create_l2_nexthop_group_2_nh(self, dvs, testlog, setup_vxlan):
        self.create_l2_nexthop(dvs, nh_id_1, dst_ip_1)
        self.create_l2_nexthop(dvs, nh_id_2, dst_ip_2)
        nhg_oid, nh_oids, bp_oid = self.create_l2_nexthop_group(
            dvs, nhg_id_1, [nh_id_1, nh_id_2], [dst_ip_1, dst_ip_2])
        self.remove_l2_nexthop_group(dvs, nhg_id_1, nhg_oid, nh_oids, bp_oid)
        self.remove_l2_nexthop(dvs, nh_id_2)
        self.remove_l2_nexthop(dvs, nh_id_1)

    def test_add_nexthop_to_l2_nhg(self, dvs, testlog, setup_vxlan):
        self.create_l2_nexthop(dvs, nh_id_1, dst_ip_1)
        nhg_oid, nh_oids, bp_oid = self.create_l2_nexthop_group(dvs, nhg_id_1, [nh_id_1], [dst_ip_1])

        self.create_l2_nexthop(dvs, nh_id_2, dst_ip_2)

        nh_oids = list(nh_oids)
        nh_oids.extend(self.add_l2_nexthop_to_group(dvs, nhg_id_1, [nh_id_1, nh_id_2], [dst_ip_1, dst_ip_2],
                                                    nhg_oid, nh_oids))

        self.remove_l2_nexthop_group(dvs, nhg_id_1, nhg_oid, nh_oids, bp_oid)
        self.remove_l2_nexthop(dvs, nh_id_2)
        self.remove_l2_nexthop(dvs, nh_id_1)

    def test_remove_nexthop_from_l2_nhg(self, dvs, testlog, setup_vxlan):
        self.create_l2_nexthop(dvs, nh_id_1, dst_ip_1)
        self.create_l2_nexthop(dvs, nh_id_2, dst_ip_2)
        nhg_oid, nh_oids, bp_oid = self.create_l2_nexthop_group(dvs, nhg_id_1, [nh_id_1, nh_id_2], [dst_ip_1, dst_ip_2])

        nh_oids = self.remove_l2_nexthop_from_group(dvs, nhg_id_1, [nh_id_2], [dst_ip_2], nhg_oid, nh_oids)

        self.remove_l2_nexthop_group(dvs, nhg_id_1, nhg_oid, nh_oids, bp_oid)
        self.remove_l2_nexthop(dvs, nh_id_2)
        self.remove_l2_nexthop(dvs, nh_id_1)

    def test_create_multiple_l2_nhgs(self, dvs, testlog, setup_vxlan):
        self.create_l2_nexthop(dvs, nh_id_1, dst_ip_1)
        self.create_l2_nexthop(dvs, nh_id_2, dst_ip_2)
        self.create_l2_nexthop(dvs, nh_id_3, dst_ip_3)

        nhg_oid_1, nh_oids_1, bp_oid_1 = self.create_l2_nexthop_group(
            dvs, nhg_id_1, [nh_id_1, nh_id_2], [dst_ip_1, dst_ip_2])

        nhg_oid_2, nh_oids_2, bp_oid_2 = self.create_l2_nexthop_group(
            dvs, nhg_id_2, [nh_id_3], [dst_ip_3])

        self.remove_l2_nexthop_group(dvs, nhg_id_2, nhg_oid_2, nh_oids_2, bp_oid_2)
        self.remove_l2_nexthop_group(dvs, nhg_id_1, nhg_oid_1, nh_oids_1, bp_oid_1)

        self.remove_l2_nexthop(dvs, nh_id_3)
        self.remove_l2_nexthop(dvs, nh_id_2)
        self.remove_l2_nexthop(dvs, nh_id_1)

    def test_create_multiple_l2_nhgs_shared_nh(self, dvs, testlog, setup_vxlan):
        self.create_l2_nexthop(dvs, nh_id_1, dst_ip_1)
        self.create_l2_nexthop(dvs, nh_id_2, dst_ip_2)
        self.create_l2_nexthop(dvs, nh_id_3, dst_ip_3)

        nhg_oid_1, nh_oids_1, bp_oid_1 = self.create_l2_nexthop_group(
            dvs, nhg_id_1, [nh_id_1, nh_id_2], [dst_ip_1, dst_ip_2])

        nhg_oid_2, nh_oids_2, bp_oid_2 = self.create_l2_nexthop_group(
            dvs, nhg_id_2, [nh_id_2, nh_id_3], [dst_ip_2, dst_ip_3])

        self.remove_l2_nexthop_group(dvs, nhg_id_2, nhg_oid_2, nh_oids_2, bp_oid_2)
        self.remove_l2_nexthop_group(dvs, nhg_id_1, nhg_oid_1, nh_oids_1, bp_oid_1)

        self.remove_l2_nexthop(dvs, nh_id_3)
        self.remove_l2_nexthop(dvs, nh_id_2)
        self.remove_l2_nexthop(dvs, nh_id_1)

    def test_add_shared_nh_to_l2_nhgs(self, dvs, testlog, setup_vxlan):
        self.create_l2_nexthop(dvs, nh_id_1, dst_ip_1)
        self.create_l2_nexthop(dvs, nh_id_2, dst_ip_2)
        self.create_l2_nexthop(dvs, nh_id_3, dst_ip_3)

        nhg_oid_1, nh_oids_1, bp_oid_1 = self.create_l2_nexthop_group(
              dvs, nhg_id_1, [nh_id_1], [dst_ip_1])

        nhg_oid_2, nh_oids_2, bp_oid_2 = self.create_l2_nexthop_group(
              dvs, nhg_id_2, [nh_id_2], [dst_ip_2])

        nh_oids_1 = list(nh_oids_1)
        nh_oids_1.extend(self.add_l2_nexthop_to_group(dvs, nhg_id_1, [nh_id_1, nh_id_3], [dst_ip_1, dst_ip_3],
                                                      nhg_oid_1, nh_oids_1))

        nh_oids_2 = list(nh_oids_2)
        nh_oids_2.extend(self.add_l2_nexthop_to_group(dvs, nhg_id_2, [nh_id_2, nh_id_3], [dst_ip_2, dst_ip_3],
                                                      nhg_oid_2, nh_oids_2))

        self.remove_l2_nexthop_group(dvs, nhg_id_2, nhg_oid_2, nh_oids_2, bp_oid_2)
        self.remove_l2_nexthop_group(dvs, nhg_id_1, nhg_oid_1, nh_oids_1, bp_oid_1)

        self.remove_l2_nexthop(dvs, nh_id_3)
        self.remove_l2_nexthop(dvs, nh_id_2)
        self.remove_l2_nexthop(dvs, nh_id_1)

    def test_remove_shared_nh_from_l2_nhgs(self, dvs, testlog, setup_vxlan):
        self.create_l2_nexthop(dvs, nh_id_1, dst_ip_1)
        self.create_l2_nexthop(dvs, nh_id_2, dst_ip_2)
        self.create_l2_nexthop(dvs, nh_id_3, dst_ip_3)

        nhg_oid_1, nh_oids_1, bp_oid_1 = self.create_l2_nexthop_group(
            dvs, nhg_id_1, [nh_id_1, nh_id_3], [dst_ip_1, dst_ip_3])

        nhg_oid_2, nh_oids_2, bp_oid_2 = self.create_l2_nexthop_group(
            dvs, nhg_id_2, [nh_id_2, nh_id_3], [dst_ip_2, dst_ip_3])

        nh_oids_1 = self.remove_l2_nexthop_from_group(dvs, nhg_id_1, [nh_id_1], [dst_ip_1],
                                            nhg_oid_1, nh_oids_1)

        nh_oids_2 = self.remove_l2_nexthop_from_group(dvs, nhg_id_2, [nh_id_2], [dst_ip_2],
                                            nhg_oid_2, nh_oids_2)

        self.remove_l2_nexthop_group(dvs, nhg_id_2, nhg_oid_2, nh_oids_2, bp_oid_2)
        self.remove_l2_nexthop_group(dvs, nhg_id_1, nhg_oid_1, nh_oids_1, bp_oid_1)

        self.remove_l2_nexthop(dvs, nh_id_3)
        self.remove_l2_nexthop(dvs, nh_id_2)
        self.remove_l2_nexthop(dvs, nh_id_1)
