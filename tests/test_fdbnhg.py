import pytest
from swsscommon import swsscommon
from typing import Dict, List

tunnel_nh_id1 = '5'
tunnel_nh_id2 = '6'
tunnel_nh_ip1 = '10.5.5.5'
tunnel_nh_ip2 = '10.6.6.6'
tunnel_nhg_id = '536870913'

app_fdb_name = 'VXLAN_FDB_TABLE:'
tunnel_device = 'vtep-100'
tunnel_vlan = 'Vlan100'
tunnel_remote_fdb = '12:34:55:12:34:56'
tunnel_remote_fdb_type = 'extern_learn'
tunnel_vni = '1000'
tunnel_src_ip = '1.1.1.1'
dstport = '4789'
tunnel_remote_fdb_type_static = 'static'


def create_fdb_nexthop(dvs, id: str, ip: str) -> Dict[str, str]:
    dvs.runcmd(f"ip nexthop add id {id} via {ip} fdb")
    fvs = {"remote_vtep": ip}
    dvs.get_app_db().wait_for_exact_match(swsscommon.APP_L2_NEXTHOP_GROUP_TABLE_NAME, id, fvs)


def create_fdb_nexthop_group(dvs, id: str, nh_ids: List[str]) -> Dict[str, str]:
    dvs.runcmd(f"ip nexthop add id {id} group {'/'.join(nh_ids)} fdb")
    fvs = {"nexthop_group": ','.join(nh_ids)}
    dvs.get_app_db().wait_for_exact_match(swsscommon.APP_L2_NEXTHOP_GROUP_TABLE_NAME, id, fvs)


def remove_l2_nexthop(dvs, id: str):
    dvs.runcmd(f"ip nexthop del id {id}")
    return dvs.get_app_db().wait_for_deleted_entry(swsscommon.APP_L2_NEXTHOP_GROUP_TABLE_NAME, id)


class TestFdbNhg(object):
    @pytest.fixture
    def setup_teardown_test(self, dvs):
        dvs.setup_db()
        yield
        # ip nexthop objects are not disposed of otherwise upon test failure
        dvs.runcmd("ip nexthop flush")

    def test_add_nexthop(self, dvs, testlog, setup_teardown_test):
        create_fdb_nexthop(dvs, tunnel_nh_id1, tunnel_nh_ip1)
        remove_l2_nexthop(dvs, tunnel_nh_id1)

    def test_add_nexthop_group_1_nh(self, dvs, testlog, setup_teardown_test):
        create_fdb_nexthop(dvs, tunnel_nh_id1, tunnel_nh_ip1)
        create_fdb_nexthop_group(dvs, tunnel_nhg_id, [tunnel_nh_id1])

        remove_l2_nexthop(dvs, tunnel_nhg_id)
        remove_l2_nexthop(dvs, tunnel_nh_id1)

    def test_add_nexthop_group_2_nh(self, dvs, testlog, setup_teardown_test):
        create_fdb_nexthop(dvs, tunnel_nh_id1, tunnel_nh_ip1)
        create_fdb_nexthop(dvs, tunnel_nh_id2, tunnel_nh_ip2)
        create_fdb_nexthop_group(dvs, tunnel_nhg_id, [tunnel_nh_id1, tunnel_nh_id2])

        remove_l2_nexthop(dvs, tunnel_nhg_id)
        remove_l2_nexthop(dvs, tunnel_nh_id1)
        remove_l2_nexthop(dvs, tunnel_nh_id2)

    def test_add_fdb_nexthop_group(self, dvs, testlog, setup_teardown_test):
        create_fdb_nexthop(dvs, tunnel_nh_id1, tunnel_nh_ip1)
        create_fdb_nexthop(dvs, tunnel_nh_id2, tunnel_nh_ip2)
        create_fdb_nexthop_group(dvs, tunnel_nhg_id, [tunnel_nh_id1, tunnel_nh_id2])

        dvs.runcmd(f"ip link add {tunnel_device} type vxlan id {tunnel_vni} local {tunnel_src_ip} dstport {dstport}")
        dvs.runcmd(f"ip link set up {tunnel_device}")
        dvs.runcmd(f"bridge fdb add {tunnel_remote_fdb} dev {tunnel_device} nhid {tunnel_nhg_id} self {tunnel_remote_fdb_type}")

        # Check in the APP DB for the FDB entry to be present APP_VXLAN_FDB_TABLE_NAME "APP_VXLAN_FDB_TABLE_NAME"
        # check application database
        fvs = dvs.get_app_db().wait_for_entry(app_fdb_name+tunnel_vlan, tunnel_remote_fdb)
        assert len(fvs) == 3
        assert fvs.get("nexthop_group") == tunnel_nhg_id
        assert fvs.get("type") == tunnel_remote_fdb_type_static
        assert fvs.get("vni") == tunnel_vni

        # Remove the fdb entry, and check the APP_DB
        dvs.runcmd(f"bridge fdb del {tunnel_remote_fdb} dev {tunnel_device} nhid {tunnel_nhg_id} self {tunnel_remote_fdb_type}")
        intf_entries = dvs.get_app_db().wait_for_deleted_entry(app_fdb_name+tunnel_vlan, tunnel_remote_fdb)

        remove_l2_nexthop(dvs, tunnel_nhg_id)
        remove_l2_nexthop(dvs, tunnel_nh_id1)
        remove_l2_nexthop(dvs, tunnel_nh_id2)

    def test_del_fdb_nhg_via_nhg_del(self, dvs, testlog, setup_teardown_test):
        dvs.runcmd("swssloglevel -l INFO -c fpmsyncd")
        dvs.runcmd("swssloglevel -l INFO -c fdbsyncd")
        create_fdb_nexthop(dvs, tunnel_nh_id1, tunnel_nh_ip1)
        create_fdb_nexthop(dvs, tunnel_nh_id2, tunnel_nh_ip2)
        create_fdb_nexthop_group(dvs, tunnel_nhg_id, [tunnel_nh_id1, tunnel_nh_id2])

        dvs.runcmd(f"ip link add {tunnel_device} type vxlan id {tunnel_vni} local {tunnel_src_ip} dstport {dstport}")
        dvs.runcmd(f"ip link set up {tunnel_device}")
        dvs.runcmd(f"bridge fdb add {tunnel_remote_fdb} dev {tunnel_device} nhid {tunnel_nhg_id} self {tunnel_remote_fdb_type}")

        # Check in the APP DB for the FDB entry to be present APP_VXLAN_FDB_TABLE_NAME "APP_VXLAN_FDB_TABLE_NAME"
        # check application database
        fvs = dvs.get_app_db().wait_for_entry(app_fdb_name+tunnel_vlan, tunnel_remote_fdb)
        assert len(fvs) == 3
        assert fvs.get("nexthop_group") == tunnel_nhg_id
        assert fvs.get("type") == tunnel_remote_fdb_type_static
        assert fvs.get("vni") == tunnel_vni

        remove_l2_nexthop(dvs, tunnel_nh_id1)
        remove_l2_nexthop(dvs, tunnel_nh_id2)
        dvs.get_app_db().wait_for_deleted_entry(swsscommon.APP_L2_NEXTHOP_GROUP_TABLE_NAME, tunnel_nhg_id)
        dvs.get_app_db().wait_for_deleted_entry(app_fdb_name+tunnel_vlan, tunnel_remote_fdb)
