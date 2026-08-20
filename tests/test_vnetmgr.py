# Tests for vnetmgrd: CONFIG_DB -> APP_DB propagation (VNET_ROUTE and
# VNET_ROUTE_TUNNEL) and the install_on_kernel kernel-programming path
# (VXLAN device + VRF route + host-route static neigh).

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
    Cleanup CONFIG_DB / APP_DB state around each test, and tear down
    any VNET / VXLAN_TUNNEL / VNET_ROUTE_TUNNEL rows left behind.
    """
    cfg_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)
    app_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)

    # vxlanmgrd needs vxlan_router_mac; vnetmgrd only needs the SWITCH_TABLE key to exist.
    delete_entry_tbl(app_db, "SWITCH_TABLE", "switch")
    create_entry_tbl(app_db, "SWITCH_TABLE", ":", "switch",
                     [("vxlan_router_mac", "00:aa:bb:cc:dd:ee")])
    # Restart both daemons so their in-memory VxlanSwitchTableConfig cache is
    # fresh for each test; without this, a router_mac / vxlan_port override in
    # one test would be masked by a stale value from a previous test.
    dvs.runcmd(["supervisorctl", "restart", "vxlanmgrd", "vnetmgrd"])
    time.sleep(3)

    yield cfg_db, app_db

    for key in swsscommon.Table(cfg_db, "VNET_ROUTE_TUNNEL").getKeys():
        delete_entry_tbl(cfg_db, "VNET_ROUTE_TUNNEL", key)
    for key in swsscommon.Table(cfg_db, "VNET_ROUTE").getKeys():
        delete_entry_tbl(cfg_db, "VNET_ROUTE", key)
    for key in swsscommon.Table(cfg_db, "VNET").getKeys():
        delete_entry_tbl(cfg_db, "VNET", key)
    for key in swsscommon.Table(cfg_db, "VXLAN_TUNNEL").getKeys():
        delete_entry_tbl(cfg_db, "VXLAN_TUNNEL", key)
    delete_entry_tbl(app_db, "SWITCH_TABLE", "switch")
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
        # Subnet route: Vxlan netdev + VRF route created (no neigh)
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
        assert "install_on_kernel" not in fv_map, \
            "install_on_kernel must be stripped before write to APP_DB"

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
            failure_message=f"APP_DB VNET_ROUTE_TABLE row {app_key} not created by vnetmgrd",
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
            failure_message=f"APP_DB VNET_ROUTE_TABLE row {app_key} not deleted by vnetmgrd",
        )

    def test_install_on_kernel_uses_vxlan_port_override(self, dvs, vnetmgr_env):
        # APP_SWITCH_TABLE:switch|vxlan_port override must appear as dstport on the Vxlan dev.
        cfg_db, app_db = vnetmgr_env
        override_port = "13579"
        create_entry_tbl(app_db, "SWITCH_TABLE", ':', "switch",
                         [("vxlan_port", override_port)])

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
            failure_message=f"{dev} did not pick up vxlan_port override {override_port}",
        )

        delete_entry_tbl(cfg_db, "VNET_ROUTE_TUNNEL", f"{vnet}|{prefix}")

        def _dev_gone():
            return (not _link_present(dvs, dev), _link_details(dvs, dev))

        wait_for_result(
            _dev_gone,
            polling_config=PollingConfig(polling_interval=1, timeout=10, strict=True),
            failure_message=f"{dev} was not deleted on VNET_ROUTE_TUNNEL delete",
        )

    def test_vnetmgrd_cold_restart_clears_stale_vxlan_netdevs(self, dvs, vnetmgr_env):
        # Cold restart test to check that stale Vxlan netdevs + associated vrf neigh/routes
        # are deleted
        cfg_db, _ = vnetmgr_env

        # Stale: should be deleted after cold restart.
        stale_vrf   = "VnetStale99"
        stale_dev   = "Vxlan99999"
        stale_pfx   = "203.0.113.0/24"
        stale_neigh = "203.0.113.5"
        stale_mac   = "aa:bb:cc:dd:ee:99"

        # Live: should be recreated after cold restart.
        tunnel    = TUNNEL
        vnet      = VNET
        vnet_vni  = "6000"
        route_vni = "6001"
        live_pfx  = "100.200.1.0/24"
        endpoint  = "10.20.20.1"
        src_ip    = "10.0.0.6"
        live_mac  = "aa:bb:cc:dd:ee:66"
        live_dev  = f"Vxlan{route_vni}"

        def _cleanup():
            dvs.runcmd(["ip", "link", "del", stale_dev])
            dvs.runcmd(["ip", "link", "del", stale_vrf])
            delete_entry_tbl(cfg_db, "VNET_ROUTE_TUNNEL", f"{vnet}|{live_pfx}")

        _cleanup()

        rc, out = dvs.runcmd(["ip", "link", "add", stale_vrf,
                              "type", "vrf", "table", "99999"])
        assert rc == 0, f"failed to create stale VRF {stale_vrf}: {out}"
        dvs.runcmd(["ip", "link", "set", "dev", stale_vrf, "up"])

        rc, out = dvs.runcmd(["ip", "link", "add", stale_dev,
                              "type", "vxlan", "id", "99999", "dstport", "4789"])
        assert rc == 0, f"failed to create stale netdev {stale_dev}: {out}"
        dvs.runcmd(["ip", "link", "set", "dev", stale_dev, "master", stale_vrf])
        dvs.runcmd(["ip", "link", "set", "dev", stale_dev, "up"])

        rc, out = dvs.runcmd(["ip", "route", "add", stale_pfx,
                              "dev", stale_dev, "vrf", stale_vrf])
        assert rc == 0, f"failed to add stale route {stale_pfx}: {out}"
        rc, out = dvs.runcmd(["ip", "neigh", "add", stale_neigh,
                              "lladdr", stale_mac, "dev", stale_dev])
        assert rc == 0, f"failed to add stale neigh {stale_neigh}: {out}"

        _push_vnet_topology(cfg_db, tunnel, vnet, vnet_vni, src_ip)
        create_entry_tbl(
            cfg_db, "VNET_ROUTE_TUNNEL", '|', f"{vnet}|{live_pfx}",
            [("endpoint", endpoint),
             ("mac_address", live_mac),
             ("vni", route_vni),
             ("install_on_kernel", "true")],
        )

        def _live_dev_and_route_present():
            details = _link_details(dvs, live_dev)
            route   = _route_in_vrf(dvs, live_pfx, vnet)
            return ("vxlan" in details and live_dev in route,
                    f"link={details!r} route={route!r}")

        wait_for_result(
            _live_dev_and_route_present,
            polling_config=PollingConfig(polling_interval=1, timeout=30, strict=True),
            failure_message=f"live {live_dev} + route {live_pfx} not created pre-restart",
        )

        assert _link_present(dvs, stale_dev)
        assert stale_pfx in _route_in_vrf(dvs, stale_pfx, stale_vrf)
        assert _neigh_on(dvs, stale_dev, stale_neigh) != ""
        assert _link_present(dvs, live_dev)

        dvs.runcmd(["config", "warm_restart", "disable", "swss"])
        dvs.runcmd(["supervisorctl", "restart", "vnetmgrd"])

        def _stale_gone():
            netdev_gone = not _link_present(dvs, stale_dev)
            route_gone  = _route_in_vrf(dvs, stale_pfx, stale_vrf) == ""
            neigh_gone  = _neigh_on(dvs, stale_dev, stale_neigh) == ""
            return (netdev_gone and route_gone and neigh_gone,
                    f"netdev={_link_details(dvs, stale_dev)!r} "
                    f"route={_route_in_vrf(dvs, stale_pfx, stale_vrf)!r} "
                    f"neigh={_neigh_on(dvs, stale_dev, stale_neigh)!r}")

        try:
            wait_for_result(
                _stale_gone,
                polling_config=PollingConfig(polling_interval=1, timeout=15, strict=True),
                failure_message=(
                    f"vnetmgrd cold restart did not cascade-clean {stale_dev} / "
                    f"{stale_pfx} in {stale_vrf} / neigh {stale_neigh}"
                ),
            )

            def _live_recreated():
                details = _link_details(dvs, live_dev)
                route   = _route_in_vrf(dvs, live_pfx, vnet)
                ok = ("vxlan" in details and
                      f"id {route_vni}" in details and
                      f"local {src_ip}" in details and
                      f"remote {endpoint}" in details and
                      live_dev in route)
                return (ok, f"link={details!r} route={route!r}")

            wait_for_result(
                _live_recreated,
                polling_config=PollingConfig(polling_interval=1, timeout=30, strict=True),
                failure_message=(
                    f"vnetmgrd cold restart did not recreate live {live_dev} / "
                    f"{live_pfx} in vrf {vnet} from CONFIG_DB"
                ),
            )
        finally:
            _cleanup()


    def test_install_on_kernel_defers_when_switch_table_missing(self, dvs, vnetmgr_env):
        # Missing SWITCH_TABLE|switch must make vnetmgrd defer and log the retry.
        cfg_db, app_db = vnetmgr_env

        tunnel   = TUNNEL
        vnet     = VNET
        vni      = "6600"
        prefix   = "100.160.1.0/24"
        endpoint = "10.30.30.2"
        src_ip   = "10.0.0.8"
        dst_mac  = "aa:bb:cc:dd:ee:66"
        dev      = f"Vxlan{vni}"

        # Erase fixture's seeded router_mac and reset daemon caches.
        delete_entry_tbl(app_db, "SWITCH_TABLE", "switch")
        dvs.runcmd(["ip", "link", "del", dev])
        dvs.runcmd(["supervisorctl", "restart", "vxlanmgrd", "vnetmgrd"])
        time.sleep(3)

        marker = dvs.add_log_marker("/var/log/syslog")

        _push_vnet_topology(cfg_db, tunnel, vnet, vni, src_ip)
        create_entry_tbl(
            cfg_db, "VNET_ROUTE_TUNNEL", "|", f"{vnet}|{prefix}",
            [("endpoint", endpoint),
             ("mac_address", dst_mac),
             ("vni", vni),
             ("install_on_kernel", "true")],
        )

        try:
            def _switch_table_not_ready_log_seen():
                _, raw = dvs.runcmd(
                    ["sh", "-c",
                     "awk '/%s/,ENDFILE {print;}' /var/log/syslog | "
                     "grep -F 'SWITCH_TABLE|switch not present for vxlan device %s' | tail -1"
                     % (marker, dev)],
                )
                return (raw.strip() != "", raw)

            wait_for_result(
                _switch_table_not_ready_log_seen,
                polling_config=PollingConfig(polling_interval=1, timeout=15, strict=True),
                failure_message=(
                    f"vnetmgrd did not log the 'SWITCH_TABLE|switch not present' defer "
                    f"message for {dev}"
                ),
            )

            # Log seen => vnetmgrd processed the route; deferral must be side-effect-free.
            assert not _link_present(dvs, dev)
            assert _route_in_vrf(dvs, prefix, vnet) == ""

            # vxlanmgrd still needs vxlan_router_mac to build Vxlan<vni>.
            create_entry_tbl(app_db, "SWITCH_TABLE", ":", "switch",
                             [("vxlan_router_mac", "00:11:22:33:44:55")])

            def _route_installed():
                r = _route_in_vrf(dvs, prefix, vnet)
                return (dev in r, r)
            wait_for_result(
                _route_installed,
                polling_config=PollingConfig(polling_interval=1, timeout=30, strict=True),
                failure_message=(
                    f"vnetmgrd never installed {prefix} in {vnet} vrf after router_mac was seeded"
                ),
            )
        finally:
            delete_entry_tbl(cfg_db, "VNET_ROUTE_TUNNEL", f"{vnet}|{prefix}")
            delete_entry_tbl(app_db, "SWITCH_TABLE", "switch")
            dvs.runcmd(["ip", "link", "del", dev])

    def test_install_on_kernel_vni_matches_vnet_defers_until_netdev_exists(self, dvs, vnetmgr_env):
        # router_mac is present (fixture seed); stopping vxlanmgrd holds off Vxlan<vni>.
        # vnetmgrd must log the "not yet created by vxlanmgrd" defer message, then
        # program the route once vxlanmgrd is restarted and creates the netdev.
        cfg_db, app_db = vnetmgr_env

        tunnel   = TUNNEL
        vnet     = VNET
        vni      = "6500"
        prefix   = "100.150.1.0/24"
        endpoint = "10.30.30.1"
        src_ip   = "10.0.0.7"
        dst_mac  = "aa:bb:cc:dd:ee:65"
        dev      = f"Vxlan{vni}"

        # Ensure Vxlan<vni> netdev is absent and vxlanmgrd can't recreate it.
        dvs.runcmd(["ip", "link", "del", dev])
        dvs.runcmd(["supervisorctl", "stop", "vxlanmgrd"])
        dvs.runcmd(["supervisorctl", "restart", "vnetmgrd"])
        time.sleep(3)

        marker = dvs.add_log_marker("/var/log/syslog")

        _push_vnet_topology(cfg_db, tunnel, vnet, vni, src_ip)
        create_entry_tbl(
            cfg_db, "VNET_ROUTE_TUNNEL", "|", f"{vnet}|{prefix}",
            [("endpoint", endpoint),
             ("mac_address", dst_mac),
             ("vni", vni),
             ("install_on_kernel", "true")],
        )

        try:
            time.sleep(5)
            assert not _link_present(dvs, dev), (
                f"{dev} unexpectedly present before vxlanmgrd was restarted; "
                f"details={_link_details(dvs, dev)!r}"
            )
            assert _route_in_vrf(dvs, prefix, vnet) == "", (
                f"route {prefix} present in {vnet} vrf before netdev was created"
            )

            def _retry_log_seen():
                _, raw = dvs.runcmd(
                    ["sh", "-c",
                     "awk '/%s/,ENDFILE {print;}' /var/log/syslog | "
                     "grep -F 'Vxlan device %s not yet created by vxlanmgrd' | tail -1"
                     % (marker, dev)],
                )
                return (raw.strip() != "", raw)

            wait_for_result(
                _retry_log_seen,
                polling_config=PollingConfig(polling_interval=1, timeout=15, strict=True),
                failure_message=(
                    f"vnetmgrd did not log the 'not yet created by vxlanmgrd' retry "
                    f"for {dev}"
                ),
            )

            dvs.runcmd(["supervisorctl", "start", "vxlanmgrd"])

            def _netdev_created():
                return (_link_present(dvs, dev), _link_details(dvs, dev))
            wait_for_result(
                _netdev_created,
                polling_config=PollingConfig(polling_interval=1, timeout=30, strict=True),
                failure_message=(
                    f"vxlanmgrd never created {dev} after being restarted"
                ),
            )

            def _route_installed():
                r = _route_in_vrf(dvs, prefix, vnet)
                return (dev in r, r)
            wait_for_result(
                _route_installed,
                polling_config=PollingConfig(polling_interval=1, timeout=30, strict=True),
                failure_message=(
                    f"vnetmgrd never installed {prefix} in {vnet} vrf after "
                    f"{dev} was created by vxlanmgrd"
                ),
            )
        finally:
            dvs.runcmd(["supervisorctl", "start", "vxlanmgrd"])
            delete_entry_tbl(cfg_db, "VNET_ROUTE_TUNNEL", f"{vnet}|{prefix}")
            dvs.runcmd(["ip", "link", "del", dev])

    def test_install_on_kernel_vxlan_sport_and_mask_are_accepted(self, dvs, vnetmgr_env):
        # sport=20000/mask=8 => srcport 19968 20223; vxlan_port omitted => dstport 4789.
        cfg_db, app_db = vnetmgr_env
        create_entry_tbl(app_db, "SWITCH_TABLE", ":", "switch",
                         [("vxlan_sport", "20000"),
                          ("vxlan_mask", "8")])
        tunnel     = TUNNEL
        vnet       = VNET
        vnet_vni   = "8000"
        route_vni  = "8001"
        prefix     = "100.180.1.0/24"
        endpoint   = "10.30.50.1"
        src_ip     = "10.0.0.9"
        dst_mac    = "aa:bb:cc:dd:ee:88"
        dev        = f"Vxlan{route_vni}"

        _push_vnet_topology(cfg_db, tunnel, vnet, vnet_vni, src_ip)
        create_entry_tbl(
            cfg_db, "VNET_ROUTE_TUNNEL", "|", f"{vnet}|{prefix}",
            [("endpoint", endpoint),
             ("mac_address", dst_mac),
             ("vni", route_vni),
             ("install_on_kernel", "true")],
        )

        # Wait for kernel to have every expected attribute in one shot.
        expected = [
            f"vxlan id {route_vni}",
            f"local {src_ip}",
            f"remote {endpoint}",
            "dstport 4789",
            "srcport 19968 20223",
        ]

        def _dev_matches_all():
            details = _link_details(dvs, dev)
            missing = [tok for tok in expected if tok not in details]
            return (not missing, f"missing={missing!r} details={details!r}")

        try:
            try:
                wait_for_result(
                    _dev_matches_all,
                    polling_config=PollingConfig(polling_interval=1, timeout=30, strict=True),
                    failure_message=(
                        f"{dev} did not materialize with all expected SWITCH_TABLE attrs "
                        f"(id/local/remote/dstport=4789/srcport 19968 20223)"
                    ),
                )
            except AssertionError:
                _, _dbg = dvs.runcmd(["ip", "-d", "link", "show", dev])
                raise AssertionError(f"{dev} attrs incomplete. `ip -d link show`:\n{_dbg}")
        finally:
            delete_entry_tbl(cfg_db, "VNET_ROUTE_TUNNEL", f"{vnet}|{prefix}")
            delete_entry_tbl(app_db, "SWITCH_TABLE", "switch")
            dvs.runcmd(["ip", "link", "del", dev])

    def test_vxlanmgrd_creates_bridge_and_vxlan_with_switch_table(self, dvs, vnetmgr_env):
        cfg_db, app_db = vnetmgr_env

        router_mac = "00:11:22:33:44:88"
        vxlan_port = "14444"
        sport      = "40000"
        mask       = "6"
        srcport_min = "40000"
        srcport_max = "40063"

        tunnel   = TUNNEL
        vnet     = VNET
        vni      = "9100"
        src_ip   = "10.0.0.11"
        vxlan_dev = f"Vxlan{vni}"
        bridge   = f"Brvxlan{vni}"

        # Reset stale cache/state in both daemons before test.
        delete_entry_tbl(app_db, "SWITCH_TABLE", "switch")
        dvs.runcmd(["ip", "link", "del", vxlan_dev])
        dvs.runcmd(["ip", "link", "del", bridge])
        dvs.runcmd(["supervisorctl", "restart", "vxlanmgrd", "vnetmgrd"])
        time.sleep(3)

        create_entry_tbl(app_db, "SWITCH_TABLE", ":", "switch",
                         [("vxlan_router_mac", router_mac),
                          ("vxlan_port",       vxlan_port),
                          ("vxlan_sport",      sport),
                          ("vxlan_mask",       mask)])

        state_db = swsscommon.DBConnector(swsscommon.STATE_DB, dvs.redis_sock, 0)
        create_entry_tbl(state_db, "VRF_TABLE", "|", vnet, [("state", "ok")])

        create_entry_tbl(cfg_db, "VXLAN_TUNNEL", "|", tunnel, [("src_ip", src_ip)])
        create_entry_tbl(cfg_db, "VNET", "|", vnet,
                         [("vxlan_tunnel", tunnel),
                          ("vni", vni)])

        try:
            expected_vxlan = [
                f"vxlan id {vni}",
                f"local {src_ip}",
                f"dstport {vxlan_port}",
                f"srcport {srcport_min} {srcport_max}",
                f"master {bridge}",
            ]

            def _vxlan_matches():
                details = _link_details(dvs, vxlan_dev)
                missing = [tok for tok in expected_vxlan if tok not in details]
                return (not missing, f"missing={missing!r} details={details!r}")

            wait_for_result(
                _vxlan_matches,
                polling_config=PollingConfig(polling_interval=1, timeout=60, strict=True),
                failure_message=(
                    f"{vxlan_dev} missing expected attrs from "
                    f"SWITCH_TABLE/VXLAN_TUNNEL/VNET"
                ),
            )

            expected_bridge = [
                f"link/ether {router_mac}",
                "bridge",
            ]

            def _bridge_matches():
                details = _link_details(dvs, bridge)
                missing = [tok for tok in expected_bridge if tok not in details]
                return (not missing, f"missing={missing!r} details={details!r}")

            wait_for_result(
                _bridge_matches,
                polling_config=PollingConfig(polling_interval=1, timeout=30, strict=True),
                failure_message=(
                    f"{bridge} is absent for router_mac={router_mac}"
                ),
            )
        finally:
            delete_entry_tbl(cfg_db, "VNET", vnet)
            delete_entry_tbl(cfg_db, "VXLAN_TUNNEL", tunnel)
            delete_entry_tbl(app_db, "SWITCH_TABLE", "switch")
            delete_entry_tbl(state_db, "VRF_TABLE", vnet)
            dvs.runcmd(["ip", "link", "del", vxlan_dev])
            dvs.runcmd(["ip", "link", "del", bridge])



def test_nonflaky_dummy():
    pass
