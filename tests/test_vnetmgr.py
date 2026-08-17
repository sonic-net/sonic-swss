# Tests for vnetmgrd: CONFIG_DB -> APP_DB propagation (VNET_ROUTE and
# VNET_ROUTE_TUNNEL) and the install_on_kernel kernel-programming path
# (VXLAN device + VRF route + host-route static neigh).

import time
import pytest

from swsscommon import swsscommon
from dvslib.dvs_common import wait_for_result, PollingConfig


TUNNEL = "Tunnel1"
VNET = "Vnet1"


def create_entry(tbl, key, pairs):
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)
    time.sleep(1)


def create_entry_tbl(db, table, separator, key, pairs):
    tbl = swsscommon.Table(db, table)
    create_entry(tbl, key, pairs)


def create_entry_pst(db, table, separator, key, pairs):
    tbl = swsscommon.ProducerStateTable(db, table)
    create_entry(tbl, key, pairs)


def delete_entry_tbl(db, table, key):
    tbl = swsscommon.Table(db, table)
    tbl._del(key)
    time.sleep(1)


def delete_entry_pst(db, table, key):
    tbl = swsscommon.ProducerStateTable(db, table)
    tbl._del(key)
    time.sleep(1)


def _link_details(dvs, ifname):
    """Return `ip -d link show <ifname>` output ("" if the device is absent)."""
    rc, out = dvs.runcmd(["ip", "-d", "link", "show", ifname])
    if rc != 0:
        return ""
    return out or ""


def _link_present(dvs, ifname):
    return "does not exist" not in _link_details(dvs, ifname) and \
           _link_details(dvs, ifname).strip() != ""


def _route_in_vrf(dvs, prefix, vrf):
    rc, out = dvs.runcmd(["ip", "route", "show", prefix, "vrf", vrf])
    if rc != 0:
        return ""
    return out or ""


def _neigh_on(dvs, ifname, ip=None):
    rc, out = dvs.runcmd(["ip", "neigh", "show", "dev", ifname])
    if rc != 0:
        return ""
    if ip is None:
        return out or ""
    for line in (out or "").splitlines():
        if line.split()[:1] == [ip]:
            return line
    return ""


@pytest.fixture
def vnetmgr_env(dvs):
    """
    Provide clean CONFIG_DB / APP_DB state around each test, and tear down
    any VNET / VXLAN_TUNNEL / VNET_ROUTE_TUNNEL rows left behind.
    """
    cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

    delete_entry_pst(app_db, "SWITCH_TABLE", "switch")

    yield cfg_db, app_db

    for key in swsscommon.Table(cfg_db, "VNET_ROUTE_TUNNEL").getKeys():
        delete_entry_tbl(cfg_db, "VNET_ROUTE_TUNNEL", key)
    for key in swsscommon.Table(cfg_db, "VNET_ROUTE").getKeys():
        delete_entry_tbl(cfg_db, "VNET_ROUTE", key)
    for key in swsscommon.Table(cfg_db, "VNET").getKeys():
        delete_entry_tbl(cfg_db, "VNET", key)
    for key in swsscommon.Table(cfg_db, "VXLAN_TUNNEL").getKeys():
        delete_entry_tbl(cfg_db, "VXLAN_TUNNEL", key)
    delete_entry_pst(app_db, "SWITCH_TABLE", "switch")
    time.sleep(2)


def _push_vnet_topology(cfg_db, tunnel, vnet, vnet_vni, src_ip):
    """Common CONFIG_DB setup shared by every install_on_kernel test."""
    create_entry_tbl(cfg_db, "VXLAN_TUNNEL", '|', tunnel,
                     [("src_ip", src_ip)])
    create_entry_tbl(cfg_db, "VNET", '|', vnet,
                     [("vxlan_tunnel", tunnel),
                      ("vni", vnet_vni)])
    # Let vnetmgrd populate its m_vnetCache before the route arrives.
    time.sleep(3)


class TestVnetMgr(object):

    def test_vnetmgrd_process_running(self, dvs, vnetmgr_env):
        # vnetmgrd process must be running in the DVS.
        _, out = dvs.runcmd(["pgrep", "-a", "vnetmgrd"])
        assert out.strip(), "vnetmgrd is not running inside the DVS"

    def test_install_on_kernel_prefix_route_full_lifecycle(self, dvs, vnetmgr_env):
        # Subnet route: Vxlan netdev + VRF route created (no neigh); all cleaned on delete.
        cfg_db, _ = vnetmgr_env

        tunnel     = TUNNEL
        vnet       = VNET
        vnet_vni   = "5000"
        route_vni  = "5001"
        prefix     = "100.100.1.0/24"
        endpoint   = "10.10.10.1"
        src_ip     = "10.0.0.1"
        dst_mac    = "aa:bb:cc:dd:ee:ff"
        dev        = f"Vxlan{route_vni}"

        _push_vnet_topology(cfg_db, tunnel, vnet, vnet_vni, src_ip)

        create_entry_tbl(
            cfg_db, "VNET_ROUTE_TUNNEL", '|', f"{vnet}|{prefix}",
            [("endpoint", endpoint),
             ("mac_address", dst_mac),
             ("vni", route_vni),
             ("install_on_kernel", "true")],
        )

        def _dev_created():
            details = _link_details(dvs, dev)
            ok = ("vxlan" in details and
                  f"id {route_vni}" in details and
                  f"local {src_ip}" in details and
                  f"remote {endpoint}" in details and
                  "dstport 4789" in details)
            return (ok, details.replace("\n", " | "))

        wait_for_result(
            _dev_created,
            polling_config=PollingConfig(polling_interval=1, timeout=30, strict=True),
            failure_message=f"kernel {dev} not created with expected attributes",
        )

        def _route_installed():
            r = _route_in_vrf(dvs, prefix, vnet)
            return (dev in r, r)

        wait_for_result(
            _route_installed,
            polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
            failure_message=f"kernel route {prefix} vrf {vnet} not installed via {dev}",
        )

        assert _neigh_on(dvs, dev).strip() == "", \
            f"unexpected neigh entry for non-host route: {_neigh_on(dvs, dev)}"

        app_rt_tunnel = swsscommon.Table(vnetmgr_env[1], "VNET_ROUTE_TUNNEL_TABLE")
        app_key = f"{vnet}:{prefix}"

        def _app_tunnel_row_present():
            keys = app_rt_tunnel.getKeys()
            return (app_key in keys, keys)

        wait_for_result(
            _app_tunnel_row_present,
            polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
            failure_message=f"APP_DB VNET_ROUTE_TUNNEL_TABLE row {app_key} not created",
        )
        ok, fvs = app_rt_tunnel.get(app_key)
        assert ok, f"APP_DB VNET_ROUTE_TUNNEL_TABLE {app_key} vanished before read"
        fv_map = dict(fvs)
        assert fv_map.get("endpoint") == endpoint
        assert fv_map.get("vni") == route_vni
        assert fv_map.get("mac_address") == dst_mac

        delete_entry_tbl(cfg_db, "VNET_ROUTE_TUNNEL", f"{vnet}|{prefix}")

        def _dev_gone():
            return (not _link_present(dvs, dev), _link_details(dvs, dev))

        wait_for_result(
            _dev_gone,
            polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
            failure_message=f"{dev} was not deleted on VNET_ROUTE_TUNNEL delete",
        )

        assert dev not in _route_in_vrf(dvs, prefix, vnet), \
            "kernel route was not cleaned up after Vxlan device delete"
        assert _neigh_on(dvs, dev).strip() == "", \
            "kernel neigh entry survived Vxlan device delete"

        def _app_tunnel_row_gone():
            keys = app_rt_tunnel.getKeys()
            return (app_key not in keys, keys)

        wait_for_result(
            _app_tunnel_row_gone,
            polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
            failure_message=f"APP_DB VNET_ROUTE_TUNNEL_TABLE row {app_key} not deleted",
        )

    def test_install_on_kernel_host_route_adds_static_neigh(self, dvs, vnetmgr_env):
        # /32 host route additionally installs a static neigh with lladdr=mac_address.
        cfg_db, _ = vnetmgr_env

        tunnel     = TUNNEL
        vnet       = VNET
        vnet_vni   = "6000"
        route_vni  = "6001"
        host_ip    = "200.200.1.1"
        prefix     = f"{host_ip}/32"
        endpoint   = "10.20.30.1"
        src_ip     = "10.0.0.2"
        dst_mac    = "aa:bb:cc:dd:ee:22"
        dev        = f"Vxlan{route_vni}"

        _push_vnet_topology(cfg_db, tunnel, vnet, vnet_vni, src_ip)
        create_entry_tbl(
            cfg_db, "VNET_ROUTE_TUNNEL", '|', f"{vnet}|{prefix}",
            [("endpoint", endpoint),
             ("mac_address", dst_mac),
             ("vni", route_vni),
             ("install_on_kernel", "true")],
        )

        def _neigh_installed():
            line = _neigh_on(dvs, dev, ip=host_ip)
            return (dst_mac in line.lower(), line)

        wait_for_result(
            _neigh_installed,
            polling_config=PollingConfig(polling_interval=1, timeout=30, strict=True),
            failure_message=f"static neigh {host_ip} lladdr {dst_mac} not on {dev}",
        )

        delete_entry_tbl(cfg_db, "VNET_ROUTE_TUNNEL", f"{vnet}|{prefix}")

        def _dev_and_neigh_gone():
            return (not _link_present(dvs, dev) and
                    _neigh_on(dvs, dev, ip=host_ip) == "",
                    _link_details(dvs, dev))

        wait_for_result(
            _dev_and_neigh_gone,
            polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
            failure_message=f"host-route teardown left {dev} or its neigh behind",
        )

    def test_vnet_route_config_to_app_db(self, dvs, vnetmgr_env):
        # CFG_DB VNET_ROUTE -> vnetmgrd -> APP_DB VNET_ROUTE_TABLE lifecycle.
        cfg_db, app_db = vnetmgr_env

        tunnel   = TUNNEL
        vnet     = VNET
        vnet_vni = "8000"
        prefix   = "30.0.0.0/24"
        nexthop  = "1.2.3.4"
        ifname   = "Ethernet0"
        src_ip   = "10.0.0.4"

        _push_vnet_topology(cfg_db, tunnel, vnet, vnet_vni, src_ip)

        cfg_key = f"{vnet}|{prefix}"
        app_key = f"{vnet}:{prefix}"

        create_entry_tbl(
            cfg_db, "VNET_ROUTE", '|', cfg_key,
            [("nexthop", nexthop), ("ifname", ifname)],
        )

        app_rt = swsscommon.Table(app_db, "VNET_ROUTE_TABLE")

        def _app_route_present():
            keys = app_rt.getKeys()
            return (app_key in keys, keys)

        wait_for_result(
            _app_route_present,
            polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
            failure_message=f"APP_DB VNET_ROUTE_TABLE row {app_key} not created",
        )
        ok, fvs = app_rt.get(app_key)
        assert ok, f"APP_DB VNET_ROUTE_TABLE {app_key} vanished before read"
        fv_map = dict(fvs)
        assert fv_map.get("nexthop") == nexthop
        assert fv_map.get("ifname") == ifname

        delete_entry_tbl(cfg_db, "VNET_ROUTE", cfg_key)

        def _app_route_gone():
            keys = app_rt.getKeys()
            return (app_key not in keys, keys)

        wait_for_result(
            _app_route_gone,
            polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
            failure_message=f"APP_DB VNET_ROUTE_TABLE row {app_key} not deleted",
        )

    def test_install_on_kernel_uses_vxlan_sport_override(self, dvs, vnetmgr_env):
        # APP_SWITCH_TABLE:switch|vxlan_sport override must appear as dstport on the Vxlan dev.
        cfg_db, app_db = vnetmgr_env
        override_port = "13579"
        create_entry_pst(app_db, "SWITCH_TABLE", ':', "switch",
                         [("vxlan_sport", override_port)])

        tunnel     = TUNNEL
        vnet       = VNET
        vnet_vni   = "7000"
        route_vni  = "7001"
        prefix     = "100.101.1.0/24"
        endpoint   = "10.30.40.1"
        src_ip     = "10.0.0.3"
        dst_mac    = "aa:bb:cc:dd:ee:33"
        dev        = f"Vxlan{route_vni}"

        _push_vnet_topology(cfg_db, tunnel, vnet, vnet_vni, src_ip)
        create_entry_tbl(
            cfg_db, "VNET_ROUTE_TUNNEL", '|', f"{vnet}|{prefix}",
            [("endpoint", endpoint),
             ("mac_address", dst_mac),
             ("vni", route_vni),
             ("install_on_kernel", "true")],
        )

        def _dev_uses_port():
            details = _link_details(dvs, dev)
            return (f"dstport {override_port}" in details, details)

        wait_for_result(
            _dev_uses_port,
            polling_config=PollingConfig(polling_interval=1, timeout=30, strict=True),
            failure_message=f"{dev} did not pick up vxlan_sport override {override_port}",
        )

        delete_entry_tbl(cfg_db, "VNET_ROUTE_TUNNEL", f"{vnet}|{prefix}")

        def _dev_gone():
            return (not _link_present(dvs, dev), _link_details(dvs, dev))

        wait_for_result(
            _dev_gone,
            polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
            failure_message=f"{dev} was not deleted on VNET_ROUTE_TUNNEL delete",
        )


def test_nonflaky_dummy():
    pass
