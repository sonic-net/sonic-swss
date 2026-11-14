"""Source-table regression for the real neighsyncd/Redis/kernel replay path."""

import shlex

import pytest

from dvslib.dvs_common import wait_for_result


@pytest.fixture(scope="function")
def dvs(request, manage_dvs):
    if request.config.getoption("--dvsname"):
        pytest.skip("The adversarial configuration cases require a disposable DVS")
    phase = request.node.callspec.params["phase"]
    # Duplicate legacy rows also affect intfmgrd. Use a fresh switch for each
    # case and let manage_dvs destroy it, rather than feeding duplicate deletes
    # back to the interface manager during per-row cleanup.
    return manage_dvs("link_local_resync_" + phase,
                      ["LINK_LOCAL_REPLAY_TEST=" + phase])


@pytest.mark.parametrize("phase", ["startup", "runtime"])
def test_SubPortReplayIgnoresWrongConfigTable(dvs, testlog, phase):
    cdb = dvs.get_config_db()
    pdb = dvs.get_app_db()
    parent, interface = "Ethernet0", "Ethernet0.100"
    neighbor, probe = "fe80::2", "fe80::3"
    mac = "02:00:00:00:00:02"
    key = interface + ":" + neighbor
    assert cdb.get_entry("INTERFACE", parent) == {}
    assert cdb.get_entry("INTERFACE", interface) == {}
    assert cdb.get_entry("VLAN_SUB_INTERFACE", interface) == {}

    def command(cmd):
        rc, output = dvs.runcmd(cmd)
        assert rc == 0, output
        return output

    def wait_for_log(marker, message):
        script = (
            "awk -v marker={} -v expected={} "
            "'index($0, marker) {{seen=1}} "
            "seen && index($0, expected) {{found=1; exit}} "
            "END {{exit !found}}' /var/log/syslog"
        ).format(shlex.quote(marker), shlex.quote(message))

        def observed():
            rc, output = dvs.runcmd(["sh", "-c", script])
            return rc == 0, output

        wait_for_result(observed, failure_message="No consumed-config barrier: " + message)

    command("supervisorctl stop neighsyncd")
    cdb.update_entry("LOGGER", "neighsyncd", {"LOGLEVEL": "DEBUG"})
    cdb.update_entry("PORT", parent, {"admin_status": "up"})
    cdb.create_entry("VLAN_SUB_INTERFACE", interface, {
        "admin_status": "up", "vlan": "100", "ipv6_use_link_local_only": "disable",
    })
    dvs.get_state_db().wait_for_field_match("PORT_TABLE", interface, {"state": "ok"})

    # Preserve the real kernel interface while adversarial CONFIG_DB rows are
    # changed. Pause only intfmgrd in this disposable switch; normal replay and
    # sibling tests run separately with every daemon active.
    manager_pid = int(command("supervisorctl pid intfmgrd").strip())
    assert manager_pid > 1
    command("kill -STOP {}".format(manager_pid))
    cdb.create_entry("INTERFACE", parent, {"ipv6_use_link_local_only": "enable"})
    if phase == "startup":
        # With an authoritative disabled startup row, the old implementation
        # clears the wrong state. Reproduce the actual late-config case instead.
        cdb.delete_entry("VLAN_SUB_INTERFACE", interface)
    command("ip -6 neigh replace {} lladdr {} nud permanent dev {}".format(neighbor, mac, interface))
    marker = dvs.add_log_marker()
    if phase == "startup":
        cdb.create_entry("INTERFACE", interface, {"ipv6_use_link_local_only": "enable"})
    command("supervisorctl start neighsyncd")
    wait_for_log(marker, "Initialized link-local mode for {} from INTERFACE".format(parent))
    wait_for_log(marker, "IPv6 Link local is not enabled on " + interface)
    if phase == "runtime":
        marker = dvs.add_log_marker()
        cdb.create_entry("INTERFACE", interface, {"ipv6_use_link_local_only": "enable"})
    wait_for_log(marker, "link-local update from INTERFACE for " + interface)
    assert pdb.get_entry("NEIGH_TABLE", key) == {}
    before = command("ip -6 neigh show to {} dev {}".format(neighbor, interface))
    assert neighbor in before and "PERMANENT" in before.upper(), before
    print("Kernel neighbor before enable:", before.strip())

    trace = "/var/log/link-local-replay-" + phase + ".log"
    monitor_pid = int(command(["sh", "-c",
        "stdbuf -oL ip -6 monitor neigh > {} 2>&1 & echo $!".format(shlex.quote(trace))]).strip())
    assert monitor_pid > 1
    try:
        command("ip -6 neigh replace {} lladdr {} nud permanent dev {}".format(probe, mac, interface))

        def monitor_ready():
            rc, output = dvs.runcmd(["cat", trace])
            return rc == 0 and probe in output, output

        wait_for_result(monitor_ready, failure_message="Neighbor-event monitor did not subscribe")
        command("ip -6 neigh del {} dev {}".format(probe, interface))
        cdb.create_entry("VLAN_SUB_INTERFACE", interface, {
            "admin_status": "up", "vlan": "100", "ipv6_use_link_local_only": "enable",
        })
        pdb.wait_for_field_match("NEIGH_TABLE", key, {"family": "IPv6", "neigh": mac})
    finally:
        alive = dvs.runcmd("kill -0 {}".format(monitor_pid))[0] == 0
        dvs.runcmd("kill {}".format(monitor_pid))
        rc, output = dvs.runcmd(["cat", trace])
        print("Kernel neighbor event trace:", output)
        assert alive and rc == 0, "Neighbor-event monitor failed"
        assert neighbor not in output, "A new target-neighbor event could hide missing replay"
    after = command("ip -6 neigh show to {} dev {}".format(neighbor, interface))
    assert neighbor in after and "PERMANENT" in after.upper(), after
