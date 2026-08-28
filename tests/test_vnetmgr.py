# Tests for vnetmgrd (Single-Vxlan-Device model): CONFIG_DB -> APP_DB
# propagation for VNET_ROUTE_TUNNEL and the install_on_kernel programming path
# (VRF route + host-route static neigh + bridge FDB entry on the shared Vxlan).

import time
import pytest

from swsscommon import swsscommon
from dvslib.dvs_common import wait_for_result, PollingConfig


TUNNEL = "Tunnel1"
VNET   = "Vnet1"


def create_entry(tbl, key, pairs):
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)
    time.sleep(1)


def create_entry_tbl(db, table, separator, key, pairs):
    tbl = swsscommon.Table(db, table)
    create_entry(tbl, key, pairs)


def delete_entry_tbl(db, table, key):
    tbl = swsscommon.Table(db, table)
    tbl._del(key)
    time.sleep(1)


def _run(dvs, argv):
    rc, out = dvs.runcmd(argv)
    return rc, (out or "")


def _link_present(dvs, ifname):
    rc, _ = _run(dvs, ["ip", "-o", "link", "show", "dev", ifname])
    return rc == 0


def _route_in_vrf(dvs, prefix, vrf):
    _, out = _run(dvs, ["ip", "route", "show", prefix, "vrf", vrf])
    return out


def _neigh_line(dvs, ifname, ip):
    _, out = _run(dvs, ["ip", "neigh", "show", "dev", ifname])
    for line in out.splitlines():
        if line.split()[:1] == [ip]:
            return line
    return ""


def _fdb_lines(dvs, ifname):
    _, out = _run(dvs, ["bridge", "fdb", "show", "dev", ifname])
    return out


def _make_vxlan_pair(dvs, vni, vrf, src_ip):
    """Simulate the Vxlan/Brvxlan/VRF trio that vxlanmgrd would create."""
    vx = f"Vxlan{vni}"
    br = f"Brvxlan{vni}"
    _run(dvs, ["ip", "link", "add", vrf, "type", "vrf", "table", str(1000 + int(vni) % 1000)])
    _run(dvs, ["ip", "link", "set", vrf, "up"])
    _run(dvs, ["ip", "link", "add", br, "type", "bridge"])
    _run(dvs, ["ip", "link", "set", br, "master", vrf])
    _run(dvs, ["ip", "link", "set", br, "up"])
    _run(dvs, ["ip", "link", "add", vx, "type", "vxlan",
               "id", str(vni), "local", src_ip, "dstport", "4789", "nolearning"])
    _run(dvs, ["ip", "link", "set", vx, "master", br])
    _run(dvs, ["ip", "link", "set", vx, "up"])


def _teardown_vxlan_pair(dvs, vni, vrf):
    for dev in [f"Vxlan{vni}", f"Brvxlan{vni}", vrf]:
        _run(dvs, ["ip", "link", "del", dev])


@pytest.fixture
def vnetmgr_env(dvs):
    """Clean CONFIG_DB / APP_DB before and after each test."""
    cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

    dvs.runcmd(["supervisorctl", "restart", "vnetmgrd"])
    time.sleep(3)

    yield cfg_db, app_db

    for tbl in ("VNET_ROUTE_TUNNEL", "VNET_ROUTE", "VNET", "VXLAN_TUNNEL"):
        for key in swsscommon.Table(cfg_db, tbl).getKeys():
            delete_entry_tbl(cfg_db, tbl, key)
    time.sleep(2)


def _push_vnet(cfg_db, tunnel, vnet, vnet_vni, src_ip):
    create_entry_tbl(cfg_db, "VXLAN_TUNNEL", '|', tunnel, [("src_ip", src_ip)])
    create_entry_tbl(cfg_db, "VNET", '|', vnet,
                     [("vxlan_tunnel", tunnel), ("vni", vnet_vni)])
    time.sleep(2)


class TestVnetMgr(object):

    def test_vnetmgrd_process_running(self, dvs, vnetmgr_env):
        _, out = dvs.runcmd(["pgrep", "-a", "vnetmgrd"])
        assert out.strip(), "vnetmgrd not running"

    def test_route_install_n_delete_same_vni_as_vnet(self, dvs, vnetmgr_env):
        cfg_db, app_db = vnetmgr_env
        vnet_vni = "5000"
        host_ip  = "100.100.1.1"
        prefix   = f"{host_ip}/32"
        endpoint = "10.10.10.1"
        src_ip   = "10.0.0.1"
        dst_mac  = "aa:bb:cc:dd:ee:ff"
        vx       = f"Vxlan{vnet_vni}"
        br       = f"Brvxlan{vnet_vni}"

        try:
            _make_vxlan_pair(dvs, vnet_vni, VNET, src_ip)
            _push_vnet(cfg_db, TUNNEL, VNET, vnet_vni, src_ip)
            create_entry_tbl(
                cfg_db, "VNET_ROUTE_TUNNEL", '|', f"{VNET}|{prefix}",
                [("endpoint", endpoint),
                 ("mac_address", dst_mac),
                 ("vni", vnet_vni),
                 ("install_on_kernel", "true")])

            def _route_ok():
                r = _route_in_vrf(dvs, prefix, VNET)
                return (br in r, r)
            wait_for_result(_route_ok,
                            polling_config=PollingConfig(polling_interval=1, timeout=15, strict=True),
                            failure_message=f"kernel route {prefix} not installed on {br}")

            def _neigh_ok():
                line = _neigh_line(dvs, br, host_ip)
                return (dst_mac in line.lower(), line)
            wait_for_result(_neigh_ok,
                            polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
                            failure_message=f"neigh {host_ip} lladdr {dst_mac} missing on {br}")

            def _fdb_ok():
                for l in _fdb_lines(dvs, vx).splitlines():
                    if dst_mac in l and f"dst {endpoint}" in l:
                        return ("vni " not in l, l)
                return (False, _fdb_lines(dvs, vx))
            wait_for_result(_fdb_ok,
                            polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
                            failure_message="same-VNI FDB entry must NOT carry a vni override")

            app_tbl = swsscommon.Table(app_db, "VNET_ROUTE_TUNNEL_TABLE")
            def _app_row_present():
                return (f"{VNET}:{prefix}" in app_tbl.getKeys(), app_tbl.getKeys())
            wait_for_result(_app_row_present,
                            polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
                            failure_message="APP_DB VNET_ROUTE_TUNNEL_TABLE row missing")
            _, fvs = app_tbl.get(f"{VNET}:{prefix}")
            fm = dict(fvs)
            assert fm.get("endpoint") == endpoint
            assert fm.get("vni") == vnet_vni
            assert fm.get("mac_address") == dst_mac
            assert "install_on_kernel" not in fm

            delete_entry_tbl(cfg_db, "VNET_ROUTE_TUNNEL", f"{VNET}|{prefix}")

            def _route_gone():
                r = _route_in_vrf(dvs, prefix, VNET)
                return (br not in r, r)
            wait_for_result(_route_gone,
                            polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
                            failure_message="kernel route not removed on delete")

            def _neigh_gone():
                return (_neigh_line(dvs, br, host_ip) == "", "")
            wait_for_result(_neigh_gone,
                            polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
                            failure_message="neigh survived delete")

            def _fdb_gone():
                for l in _fdb_lines(dvs, vx).splitlines():
                    if dst_mac in l and f"dst {endpoint}" in l:
                        return (False, l)
                return (True, "")
            wait_for_result(_fdb_gone,
                            polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
                            failure_message="bridge FDB entry survived delete")

            def _app_row_gone():
                return (f"{VNET}:{prefix}" not in app_tbl.getKeys(), app_tbl.getKeys())
            wait_for_result(_app_row_gone,
                            polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
                            failure_message="APP_DB VNET_ROUTE_TUNNEL_TABLE row survived delete")
        finally:
            _teardown_vxlan_pair(dvs, vnet_vni, VNET)

    def test_cross_vni_fdb_has_override(self, dvs, vnetmgr_env):
        cfg_db, _ = vnetmgr_env
        vnet_vni  = "7000"
        route_vni = "9999"
        prefix    = "50.50.60.1/32"
        endpoint  = "10.30.40.1"
        src_ip    = "10.0.0.4"
        dst_mac   = "aa:bb:cc:dd:ee:44"

        try:
            _make_vxlan_pair(dvs, vnet_vni, VNET, src_ip)
            _push_vnet(cfg_db, TUNNEL, VNET, vnet_vni, src_ip)
            create_entry_tbl(
                cfg_db, "VNET_ROUTE_TUNNEL", '|', f"{VNET}|{prefix}",
                [("endpoint", endpoint), ("mac_address", dst_mac),
                 ("vni", route_vni), ("install_on_kernel", "true")])

            def _fdb_ok():
                lines = _fdb_lines(dvs, f"Vxlan{vnet_vni}")
                for l in lines.splitlines():
                    if dst_mac in l and f"dst {endpoint}" in l:
                        return (f"vni {route_vni}" in l, l)
                return (False, lines)
            wait_for_result(_fdb_ok,
                            polling_config=PollingConfig(polling_interval=1, timeout=15, strict=True),
                            failure_message=f"cross-VNI FDB entry missing 'vni {route_vni}' override")
        finally:
            _teardown_vxlan_pair(dvs, vnet_vni, VNET)

    def test_route_defers_when_netdev_missing(self, dvs, vnetmgr_env):
        cfg_db, _ = vnetmgr_env
        vnet_vni = "8500"
        prefix   = "60.60.60.0/24"
        endpoint = "10.44.55.1"
        src_ip   = "10.0.0.5"
        dst_mac  = "aa:bb:cc:dd:ee:55"

        _push_vnet(cfg_db, TUNNEL, VNET, vnet_vni, src_ip)
        create_entry_tbl(
            cfg_db, "VNET_ROUTE_TUNNEL", '|', f"{VNET}|{prefix}",
            [("endpoint", endpoint), ("mac_address", dst_mac),
             ("vni", vnet_vni), ("install_on_kernel", "true")])

        time.sleep(3)
        assert f"Brvxlan{vnet_vni}" not in _route_in_vrf(dvs, prefix, VNET)

        try:
            _make_vxlan_pair(dvs, vnet_vni, VNET, src_ip)
            def _route_ok():
                r = _route_in_vrf(dvs, prefix, VNET)
                return (f"Brvxlan{vnet_vni}" in r, r)
            wait_for_result(_route_ok,
                            polling_config=PollingConfig(polling_interval=1, timeout=15, strict=True),
                            failure_message="deferred route not installed after netdev appeared")
        finally:
            _teardown_vxlan_pair(dvs, vnet_vni, VNET)

    def test_local_vnet_route_reaches_app_db(self, dvs, vnetmgr_env):
        cfg_db, app_db = vnetmgr_env
        vnet_vni = "9500"
        prefix   = "70.70.70.0/24"
        nexthop  = "70.70.70.254"

        _push_vnet(cfg_db, TUNNEL, VNET, vnet_vni, "10.0.0.9")
        create_entry_tbl(cfg_db, "VNET_ROUTE", '|', f"{VNET}|{prefix}",
                         [("nexthop", nexthop)])
        app_tbl = swsscommon.Table(app_db, "VNET_ROUTE_TABLE")
        def _app_row():
            return (f"{VNET}:{prefix}" in app_tbl.getKeys(), app_tbl.getKeys())
        wait_for_result(_app_row,
                        polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
                        failure_message="APP_DB VNET_ROUTE_TABLE row missing")


def test_nonflaky_dummy():
    pass
