"""DVS integration for the protection policy flow; requires matching CLI/YANG/SWSS.

These tests deliberately use the existing full DVS fixture, not a second
standalone consumer harness. They do not assert SAI switching or link FSM behavior.
"""
import json
import uuid

import pytest
from dvslib.dvs_common import wait_for_result
from dvslib.dvs_bridge_port_protection import parse_show


PORTCHANNEL = "PortChannel999"
CONFIG_TABLE = "BRIDGE_PORT_PROTECTION"
STATE_TABLE = "BRIDGE_PORT_PROTECTION_STATE"
DEFAULTS = {
    "protection_mode": "SOFTWARE",
    "admin_mode": "AUTO",
    "revertive": "false",
    "wait_to_restore_time_ms": "0",
}


def run(dvs, command):
    rc, output = dvs.runcmd(command)
    assert rc == 0, (command, output)
    return output


def check_policy(dvs, policy):
    def complete_policy_snapshot():
        state = dvs.get_state_db().get_entry(STATE_TABLE, PORTCHANNEL)
        return (state.get('admin_mode') == policy['admin_mode'] and
                state.get('protection_type') == 'sw_protection' and
                state.get('reconciliation_state') in ['STEADY', 'INCONSISTENT'], state)
    state = wait_for_result(complete_policy_snapshot)[1]
    assert state["protection_type"] == "sw_protection"
    assert state["reconciliation_state"] in ["STEADY","INCONSISTENT"]
    assert state.get("protection_state") in [None,"PRIMARY","PROTECTION"]
    output = run(dvs, ["show", "bridge-port-protection", PORTCHANNEL, "--verbose"])
    values=parse_show(output, PORTCHANNEL)
    headers=list(values)
    assert values["INTERFACE"] == PORTCHANNEL and values["ADMIN_MODE"] == policy["admin_mode"]
    assert values["CONVERGENCE"] == "n/a" and values["LAST_ARBITRATION"] == "n/a"
    for field in ["T_RECONCILE_ARMED_AT","LAST_ARBITRATION_SOURCE","LAST_CACHE_UPDATE"]: assert field in values
    for removed in ["PROGRAMMED_SAI_ADMIN_MODE","LAST_EVENT","REVERTIVE","WAIT_TO_RESTORE_TIME_MS"]: assert removed not in headers


@pytest.fixture
def protection_portchannel(dvs):
    assert PORTCHANNEL not in dvs.get_config_db().get_keys("PORTCHANNEL")
    assert PORTCHANNEL not in dvs.get_config_db().get_keys(CONFIG_TABLE)
    run(dvs, ["config", "portchannel", "add", PORTCHANNEL])
    try:
        yield dvs
    finally:
        run(dvs, ["config", "portchannel", "protection", PORTCHANNEL, "delete"])
        dvs.get_state_db().wait_for_deleted_entry(STATE_TABLE, PORTCHANNEL)
        run(dvs, ["config", "portchannel", "del", PORTCHANNEL])


def test_cli_policy_to_swss_state_and_show(protection_portchannel):
    dvs = protection_portchannel
    run(dvs, ["config", "portchannel999", "protection", "admin_mode", "primary"])
    policy = dict(DEFAULTS, admin_mode="PRIMARY")
    dvs.get_config_db().wait_for_field_match(CONFIG_TABLE, PORTCHANNEL, policy)
    check_policy(dvs, policy)

    run(dvs, ["config", "portchannel", "protection", PORTCHANNEL,
              "admin_mode", "protection"])
    policy["admin_mode"] = "PROTECTION"
    check_policy(dvs, policy)
    rc, _ = dvs.runcmd(["config", "portchannel999", "protection", "admin_mode", "invalid"])
    assert rc != 0
    assert dvs.get_config_db().get_entry(CONFIG_TABLE, PORTCHANNEL) == policy
    check_policy(dvs, policy)
    run(dvs, ["config", "portchannel999", "protection", "delete"])
    dvs.get_config_db().wait_for_deleted_entry(CONFIG_TABLE, PORTCHANNEL)
    dvs.get_state_db().wait_for_deleted_entry(STATE_TABLE, PORTCHANNEL)
    assert "No bridge-port protection state available" in run(
        dvs, ["show", "bridge-port-protection", PORTCHANNEL])


@pytest.mark.parametrize("defaults_only", [True, False])
def test_save_and_restart_restore_policy(protection_portchannel, defaults_only):
    dvs = protection_portchannel
    policy = dict(DEFAULTS)
    if defaults_only:
        dvs.get_config_db().create_entry(CONFIG_TABLE, PORTCHANNEL, {"NULL": "NULL"})
    else:
        for field, value in [("admin_mode", "protection"), ("revertive", "true"),
                             ("wait_to_restore_time_ms", "5000")]:
            run(dvs, ["config", "portchannel", "protection", PORTCHANNEL, field, value])
        policy.update(admin_mode="PROTECTION", revertive="true", wait_to_restore_time_ms="5000")
    check_policy(dvs, policy)

    token = uuid.uuid4().hex
    original = "/tmp/bp-protection-original-" + token + ".json"
    saved = "/tmp/bp-protection-saved-" + token + ".json"
    run(dvs, ["cp", "-p", "/etc/sonic/config_db.json", original])
    try:
        run(dvs, ["config", "save", "-y", saved])
        data = json.loads(run(dvs, ["cat", saved]))
        assert PORTCHANNEL in data["PORTCHANNEL"]
        expected = {} if defaults_only else policy
        assert data[CONFIG_TABLE][PORTCHANNEL] == expected
        run(dvs, ["cp", saved, "/etc/sonic/config_db.json"])
        # Follow the existing test_route.py save/restart path. This is a cold
        # restart/replay check, not warm recovery or a full config-reload check.
        dvs.restart()
        check_policy(dvs, policy)
        assert dvs.get_config_db().get_entry(CONFIG_TABLE, PORTCHANNEL) == (
            {"NULL": "NULL"} if defaults_only else policy)
    finally:
        run(dvs, ["cp", "-p", original, "/etc/sonic/config_db.json"])
        run(dvs, ["rm", "-f", original, saved])

# Additional policy-flow DVS assertions.
@pytest.mark.parametrize('field,value', [
    ('protection_mode','HARDWARE'), ('admin_mode','INVALID'), ('revertive','yes'),
    ('wait_to_restore_time_ms','-1'), ('wait_to_restore_time_ms','4294967296'),
    ('wait_to_restore_time_ms','1.5'), ('unexpected','value'),
])
def test_cli_invalid_fields_are_atomic(protection_portchannel, field, value):
    dvs = protection_portchannel
    run(dvs,['config','portchannel','protection',PORTCHANNEL,'admin_mode','primary'])
    accepted = dvs.get_config_db().get_entry(CONFIG_TABLE,PORTCHANNEL)
    rc, _ = dvs.runcmd(['config','portchannel','protection',PORTCHANNEL,field,value])
    assert rc != 0
    assert dvs.get_config_db().get_entry(CONFIG_TABLE,PORTCHANNEL) == accepted
    check_policy(dvs,accepted)


@pytest.mark.parametrize('field,value,valid', [
    ('admin_mode','PRIMARY',True), ('admin_mode','PROTECTION',True),
    ('wait_to_restore_time_ms','0',True), ('wait_to_restore_time_ms','4294967295',True),
    ('protection_mode','HARDWARE',False), ('admin_mode','INVALID',False),
    ('revertive','yes',False), ('wait_to_restore_time_ms','-1',False),
    ('wait_to_restore_time_ms','4294967296',False), ('unexpected','value',False),
])
def test_installed_yang_model_validates_each_field(protection_portchannel, field, value, valid):
    dvs = protection_portchannel
    data = {'PORTCHANNEL':{PORTCHANNEL:dvs.get_config_db().get_entry('PORTCHANNEL',PORTCHANNEL)},
            CONFIG_TABLE:{PORTCHANNEL:dict(DEFAULTS,**{field:value})}}
    script = '''import json,sys,sonic_yang
v=sonic_yang.SonicYang('/usr/local/yang-models',print_log_enabled=False)
v.loadYangModel()
try:
 v.loadData(configdbJson=json.loads(sys.argv[1])); v.validate_data_tree()
 assert 'BRIDGE_PORT_PROTECTION' not in v.tablesWithOutYang
 print('BPP_VALID')
except Exception:
 print('BPP_INVALID'); sys.exit(1)
'''
    rc, out = dvs.runcmd(['python3','-c',script,json.dumps(data)])
    assert (rc==0) == valid, out
    assert ('BPP_VALID' if valid else 'BPP_INVALID') in out


def test_cli_rejects_missing_portchannel_without_creating_policy(dvs):
    missing = 'PortChannel998'
    assert not dvs.get_config_db().get_entry('PORTCHANNEL',missing)
    rc, _ = dvs.runcmd(['config','portchannel','protection',missing,'admin_mode','auto'])
    assert rc != 0
    assert not dvs.get_config_db().get_entry(CONFIG_TABLE,missing)
    assert not dvs.get_state_db().get_entry(STATE_TABLE,missing)


def test_empty_row_sentinel_defaults_and_incremental_update(protection_portchannel):
    dvs = protection_portchannel
    db = dvs.get_config_db()
    db.create_entry(CONFIG_TABLE,PORTCHANNEL,{'NULL':'NULL'})
    check_policy(dvs,DEFAULTS)
    db.update_entry(CONFIG_TABLE,PORTCHANNEL,{'admin_mode':'PROTECTION'})
    check_policy(dvs,dict(DEFAULTS,admin_mode='PROTECTION'))
    assert db.get_entry(CONFIG_TABLE,PORTCHANNEL)['NULL'] == 'NULL'
    db.delete_entry(CONFIG_TABLE,PORTCHANNEL)
    dvs.get_state_db().wait_for_deleted_entry(STATE_TABLE,PORTCHANNEL)
    db.create_entry(CONFIG_TABLE,PORTCHANNEL,{'NULL':'NULL'})
    check_policy(dvs,DEFAULTS)


@pytest.mark.parametrize('bad', [
    {'admin_mode':'INVALID'}, {'protection_mode':'HARDWARE'}, {'revertive':'yes'},
    {'wait_to_restore_time_ms':'-1'}, {'NULL':'invalid'}, {'unexpected':'1'},
])
def test_raw_invalid_config_preserves_last_accepted_policy(protection_portchannel,bad):
    dvs = protection_portchannel
    run(dvs,['config','portchannel','protection',PORTCHANNEL,'admin_mode','primary'])
    expected = dict(DEFAULTS,admin_mode='PRIMARY')
    check_policy(dvs,expected)
    dvs.get_config_db().set_entry(CONFIG_TABLE,PORTCHANNEL,bad)
    # A raw invalid Redis row bypasses CLI validation, but must not replace
    # the orchestrator's last accepted policy. No STATE_DB seeding is used.
    from dvslib.dvs_bridge_port_protection import stable
    stable(lambda: check_policy(dvs,expected),duration=2)


def test_policy_lifecycle_unobserved_path_and_portchannel_delete_guard(protection_portchannel):
    dvs = protection_portchannel
    dvs.get_config_db().create_entry(CONFIG_TABLE,PORTCHANNEL,{'NULL':'NULL'})
    check_policy(dvs,DEFAULTS)
    state = dvs.get_state_db().get_entry(STATE_TABLE,PORTCHANNEL)
    assert 'protection_state' not in state
    assert 'programmed_sai_admin_mode' not in state
    assert state['reconciliation_state']=='INCONSISTENT'
    rc, _ = dvs.runcmd(['config','portchannel','del',PORTCHANNEL]); assert rc != 0
    run(dvs,['config','portchannel','protection',PORTCHANNEL,'delete'])
    dvs.get_state_db().wait_for_deleted_entry(STATE_TABLE,PORTCHANNEL)
    run(dvs,['config','portchannel','del',PORTCHANNEL])
    assert not dvs.get_config_db().get_entry('PORTCHANNEL',PORTCHANNEL)
    # Recreate for the shared fixture's normal cleanup.
    run(dvs,['config','portchannel','add',PORTCHANNEL])


def test_config_field_removal_reapplies_defaults(protection_portchannel):
    dvs = protection_portchannel
    run(dvs,['config','portchannel','protection',PORTCHANNEL,'admin_mode','protection'])
    dvs.get_config_db().update_entry(CONFIG_TABLE,PORTCHANNEL,{'revertive':'true','wait_to_restore_time_ms':'5000'})
    dvs.get_config_db().wait_for_field_match(CONFIG_TABLE,PORTCHANNEL,{'revertive':'true','wait_to_restore_time_ms':'5000'})
    dvs.get_config_db().set_entry(CONFIG_TABLE,PORTCHANNEL,{'admin_mode':'PRIMARY'})
    check_policy(dvs,dict(DEFAULTS,admin_mode='PRIMARY'))


def test_real_logger_registration_and_hld_verbose_fields(protection_portchannel):
    dvs = protection_portchannel
    db = dvs.get_config_db()
    component='bpProtOrch'
    previous = db.get_entry('LOGGER',component)
    process_previous = db.get_entry('LOGGER','orchagent')
    marker = __import__('uuid').uuid4().hex
    try:
        assert 'RUNNING' in run(dvs,['supervisorctl','status','orchagent'])
        db.create_entry('LOGGER',component,{'LOGLEVEL':'INFO','LOGOUTPUT':'SYSLOG'})
        run(dvs,['logger','BPP-DVS-BEGIN-'+marker])
        run(dvs,['config','portchannel','protection',PORTCHANNEL,'admin_mode','primary'])
        check_policy(dvs,dict(DEFAULTS,admin_mode='PRIMARY'))
        from dvslib.dvs_bridge_port_protection import poll, stable
        def lines():
            output = run(dvs,['cat','/var/log/syslog'])
            return output.split('BPP-DVS-BEGIN-'+marker)[-1]
        poll(lambda: True if 'bpProtOrch' in lines() and 'event=state_publish' in lines() else None)
        run(dvs,['config','portchannel','protection',PORTCHANNEL,'delete'])
        dvs.get_state_db().wait_for_deleted_entry(STATE_TABLE,PORTCHANNEL)
        poll(lambda: True if 'event=config_del' in lines() else None)
        run(dvs,['config','portchannel','protection',PORTCHANNEL,'admin_mode','primary'])
        check_policy(dvs,dict(DEFAULTS,admin_mode='PRIMARY'))
        info = run(dvs,['show','bridge-port-protection',PORTCHANNEL,'--verbose'])
        for field in ['RECONCILIATION','T_RECONCILE_ARMED_AT','LAST_ARBITRATION_SOURCE','LAST_CACHE_UPDATE']:
            assert field in info
        db.update_entry('LOGGER',component,{'LOGLEVEL':'ERROR'})
        # Let the independent logger configuration observer consume the change.
        import time
        time.sleep(1)
        marker = __import__('uuid').uuid4().hex
        run(dvs,['logger','BPP-DVS-BEGIN-'+marker])
        run(dvs,['config','portchannel','protection',PORTCHANNEL,'admin_mode','protection'])
        check_policy(dvs,dict(DEFAULTS,admin_mode='PROTECTION'))
        stable(lambda: assert_no_info(lines()),duration=1)
        assert db.get_entry('LOGGER','orchagent') == process_previous
    finally:
        if previous: db.set_entry('LOGGER',component,previous)
        else: db.delete_entry('LOGGER',component)


def assert_no_info(text):
    assert 'event=state_publish' not in text and 'event=config_set' not in text
