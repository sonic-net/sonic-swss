"""Bridge-port protection real-daemon DVS cases.

Collection does not start a testbed. Matching protection-capable SAI and
companion FRR serializer artifacts are prerequisites for execution.
"""
import time
import pytest
from dvslib.dvs_bridge_port_protection import (
    bpp, bpp_instrumented, BP, NH, NHG, NHGM, TUNNEL, OBJECTS, BINDING,
    CONFIG, STATE, GROUP_ATTR, ADMIN_ATTR, run, poll, stable,
)


def peer(bpp, ip=None):
    ip = ip or bpp.ip
    def find():
        matches = [(key,bpp.asic.get_entry(NH,key)) for key in bpp.asic.get_keys(NH)
                   if bpp.asic.get_entry(NH,key).get('SAI_NEXT_HOP_ATTR_IP')==ip and
                   bpp.asic.get_entry(NH,key).get('SAI_NEXT_HOP_ATTR_TYPE')=='SAI_NEXT_HOP_TYPE_BRIDGE_PORT']
        return matches[0] if len(matches)==1 else None
    nh, attrs = poll(find)
    tunnel = attrs['SAI_NEXT_HOP_ATTR_TUNNEL_ID']
    return nh,tunnel


def group_oid(bpp):
    nh,_ = peer(bpp)
    def find():
        groups = {bpp.asic.get_entry(NHGM,key)['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID']
                  for key in bpp.asic.get_keys(NHGM)
                  if bpp.asic.get_entry(NHGM,key).get('SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID')==nh}
        return next(iter(groups)) if len(groups)==1 else None
    return poll(find)


def group_members(bpp, oid):
    return {key:bpp.asic.get_entry(NHGM,key) for key in bpp.asic.get_keys(NHGM)
            if bpp.asic.get_entry(NHGM,key).get('SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID')==oid}


def not_applicable_before_remove(bpp, operation, since):
    trace = bpp.trace()[since:]
    removed = [i for i,event in enumerate(trace) if event['operation']==operation and event['status']==0]
    assert removed, trace
    assert any(e['operation']=='get' and e['status']==0 and
               e['observed']==bpp.instrumented.constants['state_not_applicable'] for e in trace[:removed[0]]), trace


def test_rmac_update_and_clear_use_shared_tunnel_without_rebuild(bpp):
    # Keep a genuine IMET user so RMAC clear/update can be observed on a
    # surviving tunnel; the backup pin alone is checked separately below.
    bpp.app('VXLAN_REMOTE_VNI_TABLE',bpp.vlan+':'+bpp.ip,{'vni':'99999'})
    poll(lambda: True if any(bpp.asic.get_entry(TUNNEL,k).get('SAI_TUNNEL_ATTR_ENCAP_DST_IP')==bpp.ip
                            for k in bpp.asic.get_keys(TUNNEL)) else None)
    bpp.member(); nh,tunnel = peer(bpp)
    bpp.asic.wait_for_field_match(TUNNEL,tunnel,{'SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC':bpp.mac})
    assert 'SAI_NEXT_HOP_ATTR_TUNNEL_MAC' not in bpp.asic.get_entry(NH,nh)
    updated='02:00:00:00:00:22'
    bpp.member(mac=updated)
    poll(lambda: True if peer(bpp)[1]==tunnel and
         bpp.asic.get_entry(TUNNEL,tunnel).get('SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC')==updated else None)
    assert peer(bpp)==(nh,tunnel)
    bpp.delete_app(OBJECTS,str(bpp.member_id))
    def restored():
        values=[e['mac'] for e in bpp.trace() if e['operation']=='default_mac']
        attrs=bpp.asic.get_entry(TUNNEL,tunnel)
        return True if values and attrs.get('SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC')==values[-1] else None
    poll(restored)
    assert tunnel in bpp.asic.get_keys(TUNNEL)
    bpp.delete_app('VXLAN_REMOTE_VNI_TABLE',bpp.vlan+':'+bpp.ip)
    bpp.asic.wait_for_deleted_entry(TUNNEL,tunnel)


def test_backup_alone_creates_group_nh_rmac_and_releases_every_resource(bpp):
    assert not bpp.app_db.get_keys('VXLAN_REMOTE_VNI_TABLE')
    assert not bpp.app_db.get_keys('VXLAN_FDB_TABLE')
    before = set(bpp.asic.get_keys(TUNNEL))
    bpp.member(); bpp.group(); oid=group_oid(bpp); nh,tunnel=peer(bpp)
    assert tunnel not in before
    assert group_members(bpp,oid)
    bpp.asic.wait_for_field_match(TUNNEL,tunnel,{'SAI_TUNNEL_ATTR_PEER_MODE':'SAI_TUNNEL_PEER_MODE_P2P',
                                              'SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC':bpp.mac})
    bpp.delete_app(OBJECTS,str(bpp.group_id)); bpp.asic.wait_for_deleted_entry(NHG,oid)
    bpp.delete_app(OBJECTS,str(bpp.member_id))
    bpp.asic.wait_for_deleted_entry(NH,nh); bpp.asic.wait_for_deleted_entry(TUNNEL,tunnel)


def test_membership_churn_updates_group_in_place_without_reattachment(bpp):
    bpp.start(); oid=group_oid(bpp); original=group_members(bpp,oid)
    since=len(bpp.trace())
    bpp.member(bpp.member_id+1,'10.10.10.3','02:00:00:00:00:03')
    bpp.group([bpp.member_id,bpp.member_id+1])
    poll(lambda: True if len(group_members(bpp,oid))==2 else None)
    bpp.group([bpp.member_id+1]); poll(lambda: True if len(group_members(bpp,oid))==1 else None)
    assert oid in bpp.asic.get_keys(NHG)
    assert not (set(original)&set(group_members(bpp,oid)))
    bpp.path('PRIMARY'); assert not bpp.admin_writes(since)


def test_missing_rmac_parks_then_installs_and_l2_type_stays_inert(bpp):
    bpp.policy(); bpp.member(mac=''); bpp.group(); bpp.bind()
    bpp.wait_runtime(reconciliation_state='INCONSISTENT',last_event='nhg_pending')
    assert_no_ready_nhg(bpp)
    bpp.member(); bpp.path('PRIMARY')
    bpp.delete_app(BINDING,bpp.po); bpp.delete_app(OBJECTS,str(bpp.group_id));
    bpp.delete_app(OBJECTS,str(bpp.member_id))
    poll(lambda: True if not bpp.asic.get_keys(NHG) else None)
    bpp.member(kind='l2'); bpp.group(); bpp.bind()
    bpp.wait_runtime(reconciliation_state='INCONSISTENT',last_event='nhg_pending')
    stable(lambda: assert_no_ready_nhg(bpp))


def assert_no_ready_nhg(bpp):
    # An unresolved group may own an empty SAI NHG; it cannot be usable or
    # attached until at least one member is ready.
    assert not bpp.asic.get_keys(NHGM)
    assert bpp.asic.get_entry(BP,bpp.bridge_oid()).get(GROUP_ATTR,'oid:0x0')=='oid:0x0'


def test_same_peer_contexts_share_hop_tunnel_and_withdraw_independently(bpp):
    bpp.start(); nh,tunnel=peer(bpp); oid=group_oid(bpp)
    bpp.member(bpp.member_id+1); bpp.group([bpp.member_id,bpp.member_id+1])
    stable(lambda: assert_shared_peer(bpp,nh,tunnel,oid))
    bpp.delete_app(OBJECTS,str(bpp.member_id)); bpp.group([bpp.member_id+1])
    bpp.path('PRIMARY'); assert_shared_peer(bpp,nh,tunnel,oid)


def assert_shared_peer(bpp,nh,tunnel,oid):
    assert peer(bpp)==(nh,tunnel)
    assert len(group_members(bpp,oid))==1


def test_conflicting_rmac_preserves_existing_peer_and_pending_group(bpp):
    bpp.start(); nh,tunnel=peer(bpp)
    bpp.member(bpp.member_id+1,mac='02:00:00:00:00:99')
    bpp.group([bpp.member_id+1],identifier=bpp.group_id+1)
    stable(lambda: assert_shared_mac(bpp,nh,tunnel))
    bpp.bind(bpp.group_id+1)
    bpp.wait_runtime(reconciliation_state='INCONSISTENT',last_event='nhg_pending')
    bpp.bind(); bpp.path('PRIMARY')


def assert_shared_mac(bpp,nh,tunnel):
    assert peer(bpp)==(nh,tunnel)
    assert bpp.asic.get_entry(TUNNEL,tunnel)['SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC']==bpp.mac


def test_group_withdrawal_holds_pin_until_clear_readback_and_orders_delete(bpp):
    bpp.start(); oid=group_oid(bpp); bp=bpp.bridge_oid(); since=len(bpp.trace())
    bpp.fault(fail='clear'); bpp.delete_app(OBJECTS,str(bpp.group_id))
    bpp.wait_runtime(last_event='sai_retry')
    def pinned():
        assert oid in bpp.asic.get_keys(NHG)
        assert bpp.asic.get_entry(BP,bp)[GROUP_ATTR]==oid
        assert not any(e['operation']=='group_remove' for e in bpp.trace()[since:])
    stable(pinned,duration=2)
    bpp.fault(group_oid=oid); bpp.asic.wait_for_deleted_entry(NHG,oid)
    bpp.wait_runtime(reconciliation_state='INCONSISTENT',last_event='nhg_pending')
    not_applicable_before_remove(bpp,'group_remove',since)


@pytest.mark.parametrize('stage',['group','mode','admin','get'])
def test_initial_attach_failure_recovers_without_losing_configuration(bpp,stage):
    bpp.fault(fail=stage); bpp.policy(); bpp.member(); bpp.group(); bpp.bind()
    bpp.wait_runtime(reconciliation_state='INCONSISTENT',last_event='sai_retry')
    assert bpp.config.get_entry(CONFIG,bpp.po)['admin_mode']=='AUTO'
    assert group_oid(bpp) in bpp.asic.get_keys(NHG)
    bpp.fault(); bpp.path('PRIMARY')


def test_unsupported_attachment_is_visible_and_never_falls_back_to_hardware(bpp):
    bpp.fault(fail='admin',status='unsupported'); bpp.policy(); bpp.member(); bpp.group(); bpp.bind()
    bpp.wait_runtime(reconciliation_state='INCONSISTENT',last_event='sai_unsupported')
    stable(lambda: assert_software_only(bpp))
    bpp.fault(); bpp.policy(); bpp.path('PRIMARY')


def assert_software_only(bpp):
    mode=bpp.asic.get_entry(BP,bpp.bridge_oid()).get('SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_MODE')
    assert mode in (None,'SAI_BRIDGE_PORT_PROTECTION_MODE_SOFTWARE')


def test_group_replacement_preserves_both_groups_until_acknowledged(bpp):
    bpp.start(); old=group_oid(bpp)
    bpp.member(bpp.member_id+1,'10.10.10.3','02:00:00:00:00:03')
    bpp.group([bpp.member_id+1],identifier=bpp.group_id+1)
    bpp.fault(fail='mode'); bpp.bind(bpp.group_id+1)
    bpp.wait_runtime(last_event='sai_retry')
    # Both group objects remain alive while replacement is partially written.
    assert old in bpp.asic.get_keys(NHG)
    assert len(bpp.asic.get_keys(NHG))==2
    bpp.fault(); bpp.path('PRIMARY')
    new=bpp.asic.get_entry(BP,bpp.bridge_oid())[GROUP_ATTR]; assert new!=old
    bpp.delete_app(OBJECTS,str(bpp.group_id)); bpp.asic.wait_for_deleted_entry(NHG,old)
    assert new in bpp.asic.get_keys(NHG)


@pytest.mark.parametrize('last',['config','binding','group','member','bridge','oper'])
def test_dependency_and_oper_arrival_order_retains_intent_and_activates_once(bpp,last):
    if last=='bridge':
        old=bpp.bridge(False); bpp.asic.wait_for_deleted_entry(BP,old)
    if last=='oper': bpp.link(False)
    actions={'config':lambda:bpp.policy(), 'binding':lambda:bpp.bind(), 'group':lambda:bpp.group(),
             'member':lambda:bpp.member(), 'bridge':lambda:bpp.bridge(True), 'oper':lambda:bpp.link(True)}
    for name in ['config','binding','group','member']:
        if name!=last: actions[name]()
    if last=='oper':
        # DOWN is an input to AUTO, not a missing dependency.
        bpp.path('PROTECTION')
    elif last!='config':
        poll(lambda: True if bpp.state().get('reconciliation_state')=='INCONSISTENT' else None)
        assert 'protection_state' not in bpp.state()
    actions[last](); bpp.path('PROTECTION' if last=='oper' else 'PRIMARY')
    since=len(bpp.trace()); bpp.bind(); bpp.group(); bpp.link(True)
    stable(lambda: assert_no_admin_writes(bpp,since))


def assert_no_admin_writes(bpp,since):
    assert not bpp.admin_writes(since)


def test_bridge_pre_remove_failure_defers_effects_then_removes_and_recreates(bpp):
    bpp.start(); old=bpp.bridge_oid(); since=len(bpp.trace())
    bpp.fault(fail='clear'); bpp.bridge(False); bpp.wait_runtime(last_event='sai_retry')
    def held():
        assert old in bpp.asic.get_keys(BP)
        entries=bpp.trace()[since:]
        assert not any(e['operation']=='bridge_remove' for e in entries)
        assert not any(e['operation']=='set' and e['attr']==bpp.instrumented.constants['attr_bp_admin_state']
                       and e['value']==0 for e in entries)
    stable(held,duration=2)
    bpp.fault(); bpp.asic.wait_for_deleted_entry(BP,old)
    not_applicable_before_remove(bpp,'bridge_remove',since)
    bpp.bridge(True); bpp.link(True); bpp.path('PRIMARY')
    assert bpp.bridge_oid()!=old


def test_config_deletion_clear_failure_keeps_resources_until_retry(bpp):
    bpp.start(); oid=group_oid(bpp)
    bpp.fault(fail='clear_get'); bpp.config.delete_entry(CONFIG,bpp.po)
    bpp.wait_runtime(last_event='sai_retry'); assert oid in bpp.asic.get_keys(NHG)
    bpp.fault(); bpp.state_db.wait_for_deleted_entry(STATE,bpp.po)
    bpp.asic.wait_for_field_match(BP,bpp.bridge_oid(),{GROUP_ATTR:'oid:0x0'})


@pytest.mark.parametrize('admin',['AUTO','PRIMARY','PROTECTION'])
@pytest.mark.parametrize('revertive',[False,True])
def test_initial_down_attaches_policy_and_active_down_updates_remain_valid(bpp,admin,revertive):
    bpp.link(False); bpp.fault(); since=len(bpp.trace())
    bpp.policy(admin,revertive,wtr=2000); bpp.member(); bpp.group(); bpp.bind()
    expected='PRIMARY' if admin=='PRIMARY' else 'PROTECTION'
    bpp.path(expected); assert bpp.latest_log()['wtr_state']=='IDLE'
    writes=bpp.admin_writes(since)
    assert writes and writes[0]['value']==bpp.instrumented.constants['admin_'+expected.lower()]
    assert bpp.asic.get_entry(BP,bpp.bridge_oid())[GROUP_ATTR]==group_oid(bpp)
    bpp.link(True)
    if admin=='AUTO' and revertive:
        bpp.wait_runtime(wtr_state='WAIT_TO_RESTORE'); bpp.path('PRIMARY')
    else:
        stable(lambda: assert_path(bpp,expected),duration=3)
    bpp.link(False); bpp.policy('PROTECTION'); bpp.path('PROTECTION')
    bpp.policy('PRIMARY'); bpp.path('PRIMARY')


@pytest.mark.parametrize('revertive',[False,True])
def test_fsm_down_recovery_matches_sai_and_config_policy(bpp,revertive):
    bpp.start(revertive=revertive,wtr=2000); bpp.link(False); bpp.path('PROTECTION')
    bpp.link(True)
    if revertive: bpp.wait_runtime(wtr_state='WAIT_TO_RESTORE'); bpp.path('PRIMARY')
    else: bpp.path('PROTECTION'); stable(lambda: assert_path(bpp,'PROTECTION'),duration=3)


def assert_path(bpp, expected):
    assert bpp.state()['protection_state']==expected
    bpp.asic.wait_for_field_match(BP,bpp.bridge_oid(),{ADMIN_ATTR:'SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_'+expected})
    assert bpp.state()['reconciliation_state']=='STEADY'


def test_wtr_repeated_flaps_cancel_and_restart_full_window(bpp):
    bpp.start(revertive=True,wtr=5000); bpp.link(False); bpp.path('PROTECTION')
    for _ in range(3):
        bpp.link(True); bpp.wait_runtime(wtr_state='WAIT_TO_RESTORE')
        stable(lambda: assert_path(bpp,'PROTECTION'),duration=1)
        bpp.link(False); bpp.wait_runtime(wtr_state='IDLE'); bpp.path('PROTECTION')
    bpp.link(True); due=int(bpp.wait_log(wtr_state='WAIT_TO_RESTORE')['wtr_deadline_monotonic_ms'])
    since=len(bpp.trace()); bpp.path('PRIMARY')
    primary=bpp.instrumented.constants['admin_primary']
    writes=[e for e in bpp.admin_writes(since) if e['value']==primary and e['status']==0]
    assert writes and all(e['time_ms']>=due for e in writes)


def test_nonrevertive_manual_primary_then_auto_stays_primary(bpp):
    bpp.start(); bpp.link(False); bpp.path('PROTECTION'); bpp.link(True)
    stable(lambda: assert_path(bpp,'PROTECTION'),duration=2)
    bpp.policy('PRIMARY'); bpp.path('PRIMARY'); bpp.policy('AUTO'); bpp.path('PRIMARY')


@pytest.mark.parametrize('admin,revertive',[('PRIMARY',False),('PRIMARY',True),('PROTECTION',False),('PROTECTION',True)])
def test_pinned_modes_ignore_link_changes_but_track_current_state(bpp,admin,revertive):
    bpp.start(admin,revertive,2000); since=len(bpp.trace())
    bpp.link(False); bpp.path(admin); bpp.link(True); bpp.path(admin)
    stable(lambda: assert_no_admin_writes(bpp,since),duration=3)


@pytest.mark.parametrize('revertive',[False,True])
def test_protection_to_auto_on_up_honors_revertive_policy(bpp,revertive):
    bpp.start('PROTECTION',revertive,2000); bpp.policy('AUTO',revertive,2000)
    if revertive:
        bpp.wait_runtime(wtr_state='WAIT_TO_RESTORE'); bpp.path('PRIMARY')
    else:
        stable(lambda: assert_path(bpp,'PROTECTION'),duration=3)
        bpp.policy('PRIMARY'); bpp.path('PRIMARY'); bpp.policy(); bpp.path('PRIMARY')


def test_pinned_primary_down_then_auto_uses_tracked_down_state(bpp):
    bpp.start('PRIMARY'); bpp.link(False); bpp.path('PRIMARY')
    bpp.policy('AUTO'); bpp.path('PROTECTION')


@pytest.mark.parametrize('fault',['reject','delayed'])
def test_inconsistent_reports_observed_old_path_and_backoff_recovers(bpp,fault):
    bpp.start(); since=len(bpp.trace())
    bpp.fault(fail='admin' if fault=='reject' else '',state='PRIMARY' if fault=='delayed' else None)
    bpp.link(False); bpp.wait_runtime(reconciliation_state='INCONSISTENT',protection_state='PRIMARY')
    # Trace persists every retry even if the test runner misses fast STATE_DB
    # counter updates. No fragile requirement to sample a 100-ms state window.
    def attempts():
        if fault=='reject': return bpp.admin_writes(since)
        return [e for e in bpp.trace()[since:] if e['operation']=='get' and e['value']==4]
    poll(lambda: True if len(attempts())>=8 else None,timeout=30)
    retries=attempts()[:8]
    if fault=='delayed': assert len(bpp.admin_writes(since))==1
    for before,after,minimum in zip(retries,retries[1:],[100,200,400,800,1600,3200,5000]):
        assert after['time_ms']-before['time_ms']>=minimum-10
    assert bpp.state()['protection_state']=='PRIMARY'
    assert bpp.state()['reconciliation_state']=='INCONSISTENT'
    assert bpp.asic.get_entry(BP,bpp.bridge_oid())[ADMIN_ATTR].replace('SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_','')==('PRIMARY' if fault=='reject' else 'PROTECTION')
    bpp.fault(); bpp.path('PROTECTION')


def test_manual_primary_during_wtr_cancels_timer_and_late_writes(bpp):
    bpp.start(revertive=True,wtr=2000); bpp.link(False); bpp.path('PROTECTION')
    bpp.link(True); bpp.wait_runtime(wtr_state='WAIT_TO_RESTORE'); bpp.policy('PRIMARY',True,2000)
    bpp.path('PRIMARY'); bpp.wait_runtime(wtr_state='IDLE'); since=len(bpp.trace())
    stable(lambda: assert_no_admin_writes(bpp,since),duration=3)


@pytest.mark.parametrize('fault',['reject','delayed'])
def test_primary_restore_failure_retries_without_new_wtr(bpp,fault):
    bpp.start(revertive=True,wtr=2000); bpp.link(False); bpp.path('PROTECTION'); bpp.link(True)
    bpp.wait_runtime(wtr_state='WAIT_TO_RESTORE')
    bpp.fault(fail='admin' if fault=='reject' else '',state='PROTECTION' if fault=='delayed' else None)
    bpp.wait_runtime(reconciliation_state='INCONSISTENT',protection_state='PROTECTION',desired_sai_admin_mode='PRIMARY',wtr_state='IDLE')
    assert bpp.asic.get_entry(BP,bpp.bridge_oid())[ADMIN_ATTR].replace('SAI_BRIDGE_PORT_PROTECTION_ADMIN_MODE_','')==('PROTECTION' if fault=='reject' else 'PRIMARY')
    bpp.fault(); bpp.path('PRIMARY'); assert bpp.latest_log()['wtr_state']=='IDLE'


def test_down_during_primary_retry_preempts_and_next_up_gets_fresh_wtr(bpp):
    bpp.start(revertive=True,wtr=2000); bpp.link(False); bpp.path('PROTECTION'); bpp.link(True)
    bpp.wait_runtime(wtr_state='WAIT_TO_RESTORE'); bpp.fault(fail='admin')
    bpp.wait_runtime(reconciliation_state='INCONSISTENT',desired_sai_admin_mode='PRIMARY')
    bpp.link(False)
    # DOWN is consumed while the failed PRIMARY write remains rejected;
    # clearing the fault first would race a scheduled PRIMARY retry.
    bpp.wait_runtime(desired_sai_admin_mode='PROTECTION',wtr_state='IDLE')
    bpp.fault(); bpp.path('PROTECTION'); since=len(bpp.trace())
    stable(lambda: assert_no_admin_writes(bpp,since),duration=3)
    bpp.link(True); bpp.wait_runtime(wtr_state='WAIT_TO_RESTORE'); bpp.path('PRIMARY')


def test_duplicate_events_wtr_edits_and_revertive_cancellation(bpp):
    bpp.start(revertive=True,wtr=5000); bpp.link(False); bpp.path('PROTECTION'); bpp.link(True)
    due=bpp.wait_log(wtr_state='WAIT_TO_RESTORE')['wtr_deadline_monotonic_ms']
    bpp.link(True); bpp.bind(); bpp.policy('AUTO',True,5000)
    assert bpp.latest_log()['wtr_deadline_monotonic_ms']==due
    bpp.policy('AUTO',True,10000)
    poll(lambda: True if int(bpp.latest_log()['wtr_deadline_monotonic_ms'])>int(due) else None)
    bpp.policy('AUTO',False,10000); bpp.wait_runtime(wtr_state='IDLE')
    stable(lambda: assert_path(bpp,'PROTECTION'),duration=2)


@pytest.mark.parametrize('wtr',[0,4294967295])
def test_wtr_boundary_values_do_not_wrap_or_use_sai_auto(bpp,wtr):
    bpp.start('PROTECTION',True,wtr); since=len(bpp.trace()); bpp.policy('AUTO',True,wtr)
    if wtr==0: bpp.path('PRIMARY')
    else:
        due=int(bpp.wait_log(wtr_state='WAIT_TO_RESTORE')['wtr_deadline_monotonic_ms'])
        now=int(__import__('dvslib.dvs_bridge_port_protection',fromlist=['run']).run(
            bpp.dvs,['python3','-c','import time;print(time.monotonic_ns()//1000000)']).strip())
        assert 4294967295-10000 <= due-now <= 4294967295
        stable(lambda: assert_path(bpp,'PROTECTION'),duration=2)
    assert all(e['value'] in [bpp.instrumented.constants['admin_primary'],bpp.instrumented.constants['admin_protection']]
               for e in bpp.admin_writes(since))


@pytest.mark.parametrize('revertive',[False,True])
def test_binding_withdrawal_cancels_wtr_and_down_replay_restores_backup(bpp,revertive):
    bpp.start(revertive=revertive,wtr=2000); bpp.link(False); bpp.path('PROTECTION'); bpp.link(True)
    if revertive: bpp.wait_runtime(wtr_state='WAIT_TO_RESTORE')
    bpp.delete_app(BINDING,bpp.po)
    bpp.wait_runtime(reconciliation_state='INCONSISTENT',wtr_state='IDLE')
    bpp.link(False); bpp.bind(); bpp.path('PROTECTION')
    assert bpp.latest_log()['wtr_state']=='IDLE'
    bpp.link(True)
    if revertive: bpp.wait_runtime(wtr_state='WAIT_TO_RESTORE'); bpp.path('PRIMARY')
    else: stable(lambda: assert_path(bpp,'PROTECTION'),duration=3)


@pytest.mark.parametrize('failure',['group','mode','admin','get'])
def test_initial_down_attach_failure_recovers_without_port_up(bpp,failure):
    bpp.link(False); bpp.fault(fail=failure)
    bpp.policy(); bpp.member(); bpp.group(); bpp.bind()
    bpp.wait_runtime(reconciliation_state='INCONSISTENT',last_event='sai_retry')
    assert 'protection_state' not in bpp.state()
    assert bpp.latest_log()['desired_admin']=='PROTECTION'
    bpp.fault(); bpp.path('PROTECTION')
    assert bpp.latest_log()['wtr_state']=='IDLE'


@pytest.mark.parametrize('last',['config','binding','group','member','bridge'])
def test_initial_down_waits_only_for_missing_objects_then_attaches(bpp,last):
    bpp.link(False)
    if last=='bridge':
        old=bpp.bridge(False); bpp.asic.wait_for_deleted_entry(BP,old)
    actions={'config':lambda:bpp.policy(), 'binding':lambda:bpp.bind(),
             'group':lambda:bpp.group(), 'member':lambda:bpp.member(),
             'bridge':lambda:bpp.bridge(True)}
    for name in ['config','binding','group','member']:
        if name!=last: actions[name]()
    if last!='config':
        bpp.wait_runtime(reconciliation_state='INCONSISTENT',protection_state=None)
        assert 'pending_dependencies' not in bpp.state()
    actions[last](); bpp.path('PROTECTION')
    assert bpp.latest_log()['wtr_state']=='IDLE'


def test_existing_fdb_user_keeps_shared_tunnel_after_backup_withdrawal(bpp):
    # Remote FDB installation needs the normal IMET-created VLAN membership.
    imet=bpp.vlan+':'+bpp.ip
    bpp.app('VXLAN_REMOTE_VNI_TABLE',imet,{'vni':'99999'})
    poll(lambda: True if any(bpp.asic.get_entry(TUNNEL,k).get('SAI_TUNNEL_ATTR_ENCAP_DST_IP')==bpp.ip
                            for k in bpp.asic.get_keys(TUNNEL)) else None)
    key=bpp.vlan+':02:00:00:00:99:01'
    bpp.app('VXLAN_FDB_TABLE',key,{'remote_vtep':bpp.ip,'type':'dynamic','vni':'99999'})
    fdb_table='ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY'
    poll(lambda: True if any('02:00:00:00:99:01' in key.lower() for key in bpp.asic.get_keys(fdb_table)) else None)
    bpp.member(); bpp.group(); nh,tunnel=peer(bpp); oid=group_oid(bpp)
    bpp.delete_app('VXLAN_REMOTE_VNI_TABLE',imet)
    stable(lambda: assert_remote_fdb_present(bpp,key.split(':',1)[1]))
    bpp.delete_app(OBJECTS,str(bpp.group_id)); bpp.asic.wait_for_deleted_entry(NHG,oid)
    bpp.delete_app(OBJECTS,str(bpp.member_id)); bpp.asic.wait_for_deleted_entry(NH,nh)
    assert tunnel in bpp.asic.get_keys(TUNNEL)
    bpp.delete_app('VXLAN_FDB_TABLE',key); bpp.asic.wait_for_deleted_entry(TUNNEL,tunnel)


def assert_remote_fdb_present(bpp, mac):
    assert any(mac.lower() in key.lower() for key in bpp.asic.get_keys('ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY'))


def test_missing_source_vtep_and_mixed_family_park_then_retry(bpp):
    bpp.config.delete_entry('VXLAN_EVPN_NVO',bpp.nvo)
    bpp.config.delete_entry('VXLAN_TUNNEL_MAP',bpp.source_name+'|bpp_map')
    bpp.config.delete_entry('VXLAN_TUNNEL',bpp.source_name)
    poll(lambda: True if not bpp.asic.get_keys(TUNNEL) else None)
    bpp.policy(); bpp.member(ip='2001:db8::2'); bpp.group(); bpp.bind()
    bpp.wait_runtime(last_event='nhg_pending'); stable(lambda: assert_no_ready_nhg(bpp))
    bpp.source(); stable(lambda: assert_no_ready_nhg(bpp))  # IPv6 member / IPv4 SIP.
    bpp.member(); bpp.path('PRIMARY')


def test_ipv6_source_and_member_materialize_backup_group(bpp):
    bpp.config.delete_entry('VXLAN_EVPN_NVO',bpp.nvo)
    bpp.config.delete_entry('VXLAN_TUNNEL_MAP',bpp.source_name+'|bpp_map')
    bpp.config.delete_entry('VXLAN_TUNNEL',bpp.source_name)
    poll(lambda: True if not bpp.asic.get_keys(TUNNEL) else None)
    bpp.app_db.wait_for_deleted_entry('VXLAN_EVPN_NVO_TABLE',bpp.nvo)
    bpp.app_db.wait_for_deleted_entry('VXLAN_TUNNEL_TABLE',bpp.source_name)
    # Exercise IPv6 creation, without depending on a live source-IP mutation
    # of a same-name source that may still be retiring in orchagent.
    bpp.source_name='BppVtep6'; bpp.nvo='BppNvo6'
    bpp.ip='2001:db8::2'; bpp.source('2001:db8::1'); bpp.start()
    nh,tunnel=peer(bpp)
    bpp.asic.wait_for_field_match(TUNNEL,tunnel,{'SAI_TUNNEL_ATTR_ENCAP_DST_IP':bpp.ip})
    assert group_members(bpp,group_oid(bpp))
    assert bpp.asic.get_entry(NH,nh)['SAI_NEXT_HOP_ATTR_IP']==bpp.ip


def test_partial_group_is_ready_with_one_peer_and_empty_group_withdraws(bpp):
    bpp.policy(); bpp.member(); bpp.group([bpp.member_id,bpp.member_id+1]); bpp.bind()
    bpp.path('PRIMARY'); oid=group_oid(bpp)
    assert len(group_members(bpp,oid))==1
    bpp.member(bpp.member_id+1,'10.10.10.3','02:00:00:00:00:03')
    poll(lambda: True if len(group_members(bpp,oid))==2 else None)
    bpp.group([]); bpp.wait_runtime(reconciliation_state='INCONSISTENT',last_event='nhg_pending')
    bpp.asic.wait_for_field_match(BP,bpp.bridge_oid(),{GROUP_ATTR:'oid:0x0'})
    bpp.group(); bpp.path('PRIMARY')


@pytest.mark.parametrize('bad',['0','-1','4294967296','01','abc'])
def test_invalid_binding_keeps_last_accepted_attachment(bpp,bad):
    bpp.start(); old=bpp.asic.get_entry(BP,bpp.bridge_oid())[GROUP_ATTR]
    bpp.app(BINDING,bpp.po,{'nexthop_group':bad})
    stable(lambda: assert_path(bpp,'PRIMARY'))
    assert bpp.asic.get_entry(BP,bpp.bridge_oid())[GROUP_ATTR]==old


def test_second_port_pins_group_until_both_attachments_clear(bpp):
    bpp.start(); second='PortChannel998'; old=group_oid(bpp)
    run(bpp.dvs,['config','portchannel','add',second])
    try:
        run(bpp.dvs,['config','vlan','member','add','999',second])
        bpp.app('LAG_TABLE',second,{'admin_status':'up','oper_status':'up','mtu':'9100'})
        bpp.config.create_entry(CONFIG,second,{'admin_mode':'PROTECTION'})
        bpp.app(BINDING,second,{'nexthop_group':str(bpp.group_id)})
        bpp.state_db.wait_for_field_match(STATE,second,{'protection_state':'PROTECTION','reconciliation_state':'STEADY'})
        bpp.fault(fail='clear'); bpp.delete_app(OBJECTS,str(bpp.group_id))
        bpp.wait_runtime(last_event='sai_retry')
        bpp.state_db.wait_for_field_match(STATE,second,{'reconciliation_state':'INCONSISTENT'})
        assert old in bpp.asic.get_keys(NHG)
        bpp.fault(group_oid=old); bpp.asic.wait_for_deleted_entry(NHG,old)
    finally:
        bpp.fault(); bpp.config.delete_entry(CONFIG,second)
        bpp.state_db.wait_for_deleted_entry(STATE,second)
        bpp.delete_app(BINDING,second)
        run(bpp.dvs,['config','vlan','member','del','999',second])
        bpp.delete_app('LAG_TABLE',second)
        run(bpp.dvs,['config','portchannel','del',second])


def test_new_vlan_demand_cancels_deferred_bridge_removal(bpp):
    bpp.start(); old=bpp.bridge_oid()
    bpp.fault(fail='clear'); bpp.bridge(False); bpp.wait_runtime(last_event='sai_retry')
    bpp.bridge(True); bpp.fault(); bpp.path('PRIMARY')
    assert bpp.bridge_oid()==old


def test_path_get_failure_retains_confirmed_path_then_recovers(bpp):
    bpp.start(); bpp.fault(fail='get'); bpp.link(False)
    bpp.wait_runtime(reconciliation_state='INCONSISTENT',protection_state='PRIMARY')
    bpp.fault(); bpp.path('PROTECTION')


def test_member_events_do_not_switch_an_up_aggregate(bpp):
    bpp.start(); since=len(bpp.trace())
    table='ASIC_STATE:SAI_OBJECT_TYPE_LAG_MEMBER'; before=set(bpp.asic.get_keys(table))
    key=bpp.po+':Ethernet0'
    bpp.app('LAG_MEMBER_TABLE',key,{'status':'enabled'})
    created=poll(lambda: (set(bpp.asic.get_keys(table))-before) or None)
    bpp.delete_app('LAG_MEMBER_TABLE',key)
    for oid in created: bpp.asic.wait_for_deleted_entry(table,oid)
    stable(lambda: assert_path(bpp,'PRIMARY'))
    assert not bpp.admin_writes(since)


def test_unsupported_path_write_requires_explicit_config_reapply(bpp):
    bpp.start(); bpp.fault(fail='admin',status='unsupported'); bpp.link(False)
    bpp.wait_runtime(reconciliation_state='INCONSISTENT',protection_state='PRIMARY',last_event='sai_unsupported')
    bpp.fault(); since=len(bpp.trace())
    stable(lambda: assert_no_admin_writes(bpp,since),duration=2)
    bpp.policy(); bpp.path('PROTECTION')


def test_hld_state_row_and_verbose_show_observation_contract(bpp):
    hld={'protection_state','protection_type','convergence_mode','reconciliation_state','admin_mode',
         't_reconcile_armed_at','last_arbitration_outcome','last_arbitration_source','last_arbitration_ts',
         'last_cache_update_ts','last_resync_ts'}
    bpp.policy(); state=bpp.wait(reconciliation_state='INCONSISTENT')
    assert set(state)==hld-{'protection_state'} and state['last_resync_ts']==''
    assert '—' in bpp.show()
    bpp.member(); bpp.group(); bpp.bind(); bpp.path('PRIMARY')
    state=bpp.state(); assert set(state)==hld and state['last_resync_ts'].endswith('Z')
    observed_at=state['last_resync_ts']; bpp.fault(fail='get'); bpp.link(False)
    bpp.wait(reconciliation_state='INCONSISTENT',protection_state='PRIMARY',last_resync_ts=observed_at)
    output=bpp.show()
    for header in ['STATE','TYPE','CONVERGENCE','RECONCILIATION','ADMIN_MODE','LAST_ARBITRATION','LAST_RESYNC',
                   'T_RECONCILE_ARMED_AT','LAST_ARBITRATION_SOURCE','LAST_CACHE_UPDATE']: assert header in output
    for removed in ['PROGRAMMED_SAI_ADMIN_MODE','LAST_EVENT','REVERTIVE','WAIT_TO_RESTORE_TIME_MS']: assert removed not in output
    bpp.fault(); bpp.path('PROTECTION'); assert set(bpp.state())==hld


@pytest.mark.parametrize('removed',['wait_to_restore_time_ms','revertive'])
def test_hld_policy_field_removal_uses_defaults_in_runtime(bpp,removed):
    bpp.start(revertive=True,wtr=10000); bpp.link(False); bpp.path('PROTECTION'); bpp.link(True)
    bpp.wait_log(wtr_state='WAIT_TO_RESTORE'); bpp.mark_logs()
    run(bpp.dvs,['sonic-db-cli','CONFIG_DB','HDEL',CONFIG+'|'+bpp.po,removed])
    if removed=='wait_to_restore_time_ms': bpp.path('PRIMARY')
    else:
        bpp.wait_runtime(wtr_state='IDLE'); stable(lambda: assert_path(bpp,'PROTECTION'),duration=3)
    assert 'revertive' not in bpp.state() and 'wait_to_restore_time_ms' not in bpp.state()


@pytest.mark.parametrize('fault',['get','delayed'])
def test_verification_read_first_uses_backoff_without_repeating_write(bpp,fault):
    bpp.start(revertive=False); since=len(bpp.trace())
    bpp.fault(fail='get' if fault=='get' else '',state='PRIMARY' if fault=='delayed' else None)
    bpp.link(False); bpp.wait(reconciliation_state='INCONSISTENT')
    def reads(): return [e for e in bpp.trace()[since:] if e['operation']=='get' and e['value']==4]
    poll(lambda: True if len(reads())>=4 else None)
    assert len(bpp.admin_writes(since))==1
    for before,after,minimum in zip(reads(),reads()[1:],[100,200,400]):
        assert after['time_ms']-before['time_ms']>=minimum-10
    assert bpp.asic.get_entry(BP,bpp.bridge_oid())[ADMIN_ATTR].endswith('_PROTECTION')
    bpp.fault(); bpp.path('PROTECTION'); assert len(bpp.admin_writes(since))==1


def test_initial_get_retry_polls_without_rewriting_attachment(bpp):
    bpp.policy(); bpp.member(); bpp.group(); bpp.fault(fail='get'); since=len(bpp.trace()); bpp.bind()
    bpp.wait(reconciliation_state='INCONSISTENT',protection_state=None)
    poll(lambda: True if len([e for e in bpp.trace()[since:] if e['operation']=='get' and e['value']==4])>=3 else None)
    writes=[e for e in bpp.trace()[since:] if e['operation']=='set']
    assert len(writes)==3 and group_oid(bpp) in bpp.asic.get_keys(NHG)
    bpp.fault(); bpp.path('PRIMARY')
    assert len([e for e in bpp.trace()[since:] if e['operation']=='set'])==3


@pytest.mark.parametrize('attribute',['group','mode','admin'])
def test_read_first_corrects_only_reported_writable_attribute(bpp,attribute):
    bpp.start(revertive=False); bpp.fault(mismatch=attribute); since=len(bpp.trace())
    bpp.link(False); bpp.wait(reconciliation_state='INCONSISTENT')
    expected=bpp.instrumented.constants['attr_'+attribute]
    def corrective():
        events=bpp.trace()[since:]
        # First command selects PROTECTION. Corrections follow GET feedback.
        writes=[e for e in events if e['operation']=='set'][1:]
        return writes if len(writes)>=2 else None
    writes=poll(corrective)
    assert all(e['attr']==expected for e in writes)
    events=bpp.trace()[since:]
    for index,event in enumerate(events):
        if event['operation']=='set' and index>0:
            assert events[index-1]['operation']=='get'
            assert events[index+1]['operation']=='get'
    assert group_oid(bpp) in bpp.asic.get_keys(NHG)
    count=len([e for e in bpp.trace()[since:] if e['operation']=='set'])
    bpp.fault(); bpp.path('PROTECTION')
    assert len([e for e in bpp.trace()[since:] if e['operation']=='set'])==count


@pytest.mark.parametrize('status',['unsupported','unimplemented'])
@pytest.mark.parametrize('operation',['admin','get'])
def test_terminal_active_error_ignores_port_binding_and_timer_until_config_reapply(bpp,status,operation):
    bpp.start(revertive=False); cursor=len(bpp.terminal_errors()); bpp.fault(fail=operation,status=status)
    bpp.link(False); bpp.wait_runtime(reconciliation_state='INCONSISTENT',last_event='sai_unsupported')
    errors=poll(lambda: rows if len(rows:=bpp.terminal_errors())>cursor else None)
    assert errors[-1]['operation']==('set' if operation=='admin' else 'get')
    assert errors[-1]['status_class']==('NOT_SUPPORTED' if status=='unsupported' else 'NOT_IMPLEMENTED')
    assert errors[-1]['reapply_required']=='true' and errors[-1]['stage']=='programming'
    assert int(errors[-1]['status'])==bpp.instrumented.constants['status_not_supported' if status=='unsupported' else 'status_not_implemented']
    assert 'SAI_BRIDGE_PORT_ATTR_BRIDGE_PORT_PROTECTION_ADMIN_MODE' in errors[-1]['attributes']
    bpp.fault(); since=len(bpp.trace()); bpp.link(True); bpp.link(False); bpp.bind()
    stable(lambda: assert_no_protection_calls(bpp,since),duration=6)
    assert bpp.config.get_entry(CONFIG,bpp.po)['admin_mode']=='AUTO'
    assert bpp.state()['reconciliation_state']=='INCONSISTENT'
    bpp.policy('AUTO',False); bpp.path('PROTECTION')


def assert_no_protection_calls(bpp,since):
    ids={bpp.instrumented.constants['attr_group'],bpp.instrumented.constants['attr_mode'],bpp.instrumented.constants['attr_admin']}
    assert not [e for e in bpp.trace()[since:] if e['operation']=='get' or (e['operation']=='set' and e['attr'] in ids)]


@pytest.mark.parametrize('status',['unsupported','unimplemented'])
@pytest.mark.parametrize('stage',['group','mode','admin','get'])
def test_terminal_initial_error_drains_once_and_needs_explicit_reapply(bpp,status,stage):
    # Fault programming GET only, so the separate clear/GET drain can confirm absence.
    bpp.policy(); bpp.member(); bpp.group(); bpp.fault(fail='programming_get' if stage=='get' else stage,status=status); bpp.bind()
    bpp.wait_runtime(reconciliation_state='INCONSISTENT',last_event='sai_unsupported')
    bpp.asic.wait_for_field_match(BP,bpp.bridge_oid(),{GROUP_ATTR:'oid:0x0'})
    bpp.wait(protection_state=None); bpp.fault(); since=len(bpp.trace()); bpp.bind(); bpp.link(False)
    stable(lambda: assert_no_protection_calls(bpp,since),duration=6)
    bpp.policy('AUTO',False); bpp.path('PROTECTION')


@pytest.mark.parametrize('status',['unsupported','unimplemented'])
@pytest.mark.parametrize('stage',['clear','clear_get'])
def test_terminal_cleanup_keeps_group_and_veto_until_explicit_reapply(bpp,status,stage):
    bpp.start(revertive=False); oid=group_oid(bpp); bpp.fault(fail=stage,status=status)
    bpp.delete_app(BINDING,bpp.po); bpp.wait_runtime(reconciliation_state='INCONSISTENT',last_event='sai_unsupported')
    assert oid in bpp.asic.get_keys(NHG)
    bpp.fault(); since=len(bpp.trace()); bpp.bind(); bpp.bridge(False)
    stable(lambda: assert_no_protection_calls(bpp,since),duration=6)
    assert bpp.bridge_oid() and oid in bpp.asic.get_keys(NHG)
    bpp.bridge(True); bpp.link(True); bpp.policy('AUTO',False); bpp.path('PRIMARY')


def test_terminal_error_leaves_orchagent_and_independent_interface_running(bpp):
    bpp.start(revertive=False); bpp.fault(fail='admin',status='unsupported'); bpp.link(False)
    bpp.wait_runtime(reconciliation_state='INCONSISTENT',last_event='sai_unsupported')
    second='PortChannel998'; run(bpp.dvs,['config','portchannel','add',second])
    try:
        run(bpp.dvs,['config','vlan','member','add','999',second])
        bpp.app('LAG_TABLE',second,{'admin_status':'up','oper_status':'up','mtu':'9100'})
        bpp.config.create_entry(CONFIG,second,{'admin_mode':'PRIMARY'})
        bpp.app(BINDING,second,{'nexthop_group':str(bpp.group_id)})
        bpp.state_db.wait_for_field_match(STATE,second,{'protection_state':'PRIMARY','reconciliation_state':'STEADY'})
        assert 'RUNNING' in run(bpp.dvs,['supervisorctl','status','orchagent'])
        assert bpp.state()['reconciliation_state']=='INCONSISTENT'
    finally:
        bpp.fault(); bpp.config.delete_entry(CONFIG,second); bpp.state_db.wait_for_deleted_entry(STATE,second)
        bpp.delete_app(BINDING,second); run(bpp.dvs,['config','vlan','member','del','999',second])
        bpp.delete_app('LAG_TABLE',second); run(bpp.dvs,['config','portchannel','del',second])


@pytest.mark.parametrize('fault',['reject','get','delayed','group_mismatch','mode_mismatch','admin_mismatch'])
def test_wtr_up_deadline_and_expiry_are_independent_of_backup_reconciliation(bpp,fault):
    bpp.start(revertive=True,wtr=5000)
    bpp.fault(fail='admin' if fault=='reject' else 'get' if fault=='get' else '',
              state='PRIMARY' if fault=='delayed' else None,
              mismatch=fault.split('_')[0] if fault.endswith('_mismatch') else '')
    bpp.link(False); bpp.wait(reconciliation_state='INCONSISTENT')
    bpp.mark_logs(); bpp.link(True)
    due=int(bpp.wait_log(wtr_state='WAIT_TO_RESTORE')['wtr_deadline_monotonic_ms'])
    assert due>0; since=len(bpp.trace()); bpp.mark_logs()
    bpp.wait_log(desired_sai_admin_mode='PRIMARY',wtr_state='IDLE')
    primary=bpp.instrumented.constants['admin_primary']
    attempts=poll(lambda: rows if (rows:=[e for e in bpp.admin_writes(since) if e['value']==primary]) else None)
    assert attempts[0]['time_ms']>=due
    # Expiry supersedes old backup retries; subsequent commands remain PRIMARY.
    later=[e for e in bpp.admin_writes(since) if e['time_ms']>=attempts[0]['time_ms']]
    assert all(e['value']==primary for e in later)
    bpp.fault(); bpp.path('PRIMARY')
    assert bpp.latest_log()['wtr_state']=='IDLE'


@pytest.mark.parametrize('fault',['reject','get','delayed'])
def test_failed_primary_confirmation_does_not_rearm_completed_wtr(bpp,fault):
    bpp.start(revertive=True,wtr=5000); bpp.link(False); bpp.path('PROTECTION')
    bpp.mark_logs(); bpp.link(True); bpp.wait_log(wtr_state='WAIT_TO_RESTORE')
    bpp.fault(fail='admin' if fault=='reject' else 'get' if fault=='get' else '',
              state='PROTECTION' if fault=='delayed' else None)
    bpp.mark_logs(); bpp.wait_runtime(reconciliation_state='INCONSISTENT',desired_sai_admin_mode='PRIMARY',wtr_state='IDLE')
    def completed():
        row=bpp.latest_log()
        assert row['wtr_state']=='IDLE' and row['wtr_deadline_monotonic_ms']=='0'
        assert row['desired_admin']=='PRIMARY'
    stable(completed,duration=2)
    bpp.policy('AUTO',True,5000); stable(completed,duration=1)
    bpp.fault(); bpp.path('PRIMARY'); assert bpp.latest_log()['wtr_state']=='IDLE'


def test_confirmed_primary_runs_one_policy_window_without_redundant_sai_calls(bpp):
    bpp.start(revertive=True,wtr=5000); bpp.path('PRIMARY')
    due=int(bpp.wait_log(wtr_state='WAIT_TO_RESTORE')['wtr_deadline_monotonic_ms'])
    assert due>0; since=len(bpp.trace()); bpp.mark_logs()
    bpp.wait_log(wtr_state='IDLE',desired_sai_admin_mode='PRIMARY')
    bpp.link(True); bpp.bind(); bpp.policy('AUTO',True,5000)
    stable(lambda: assert_no_protection_calls(bpp,since),duration=6)
    assert bpp.latest_log()['wtr_state']=='IDLE'


def test_initial_policy_deadline_survives_late_binding_and_failed_get(bpp):
    bpp.policy('AUTO',True,5000)
    due=bpp.wait_log(wtr_state='WAIT_TO_RESTORE')['wtr_deadline_monotonic_ms']
    assert not bpp.state().get('protection_state')
    bpp.member(); bpp.group(); bpp.fault(fail='get'); bpp.bind()
    bpp.wait(reconciliation_state='INCONSISTENT',protection_state=None)
    assert bpp.latest_log()['wtr_deadline_monotonic_ms']==due
    bpp.mark_logs(); bpp.wait_log(wtr_state='IDLE',desired_sai_admin_mode='PRIMARY')
    bpp.fault(); bpp.path('PRIMARY'); assert bpp.latest_log()['wtr_state']=='IDLE'


@pytest.mark.parametrize('fault',['mode','get'])
def test_replacement_retries_cannot_extend_its_fresh_wtr_window(bpp,fault):
    bpp.start(revertive=True,wtr=5000); bpp.link(False); bpp.path('PROTECTION'); bpp.link(True)
    bpp.wait_log(wtr_state='WAIT_TO_RESTORE'); old=group_oid(bpp)
    bpp.group(identifier=bpp.group_id+1); bpp.fault(fail=fault); bpp.mark_logs(); bpp.bind(bpp.group_id+1)
    due=bpp.wait_log(wtr_state='WAIT_TO_RESTORE')['wtr_deadline_monotonic_ms']
    bpp.wait(reconciliation_state='INCONSISTENT'); assert old in bpp.asic.get_keys(NHG)
    poll(lambda: rows if (rows:=[e for e in bpp.events() if e['wtr_state']=='WAIT_TO_RESTORE' and
                                e['wtr_deadline_monotonic_ms']==due and int(e['retry_attempts'])>=2]) else None)
    bpp.mark_logs(); bpp.wait_log(wtr_state='IDLE',desired_sai_admin_mode='PRIMARY')
    # Every retry in this replacement episode retains the one original deadline.
    running=[e for e in bpp.events()[bpp.log_after:] if e['wtr_state']=='WAIT_TO_RESTORE']
    assert all(e['wtr_deadline_monotonic_ms']==due for e in running)
    bpp.fault(); bpp.path('PRIMARY'); bpp.delete_app(OBJECTS,str(bpp.group_id))
    bpp.asic.wait_for_deleted_entry(NHG,old)
    assert bpp.latest_log()['wtr_state']=='IDLE'


def installed_member_for_peer(bpp,group,ip):
    nh,tunnel=peer(bpp,ip)
    rows=group_members(bpp,group)
    matches=[key for key,attrs in rows.items() if attrs['SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID']==nh]
    assert len(matches)==1
    return matches[0],nh,tunnel


@pytest.mark.parametrize('failed_index',[0,1,2])
def test_partial_member_removal_continues_and_keeps_the_bridge_attached(bpp,failed_index):
    bpp.start(revertive=False); group=group_oid(bpp)
    ips=[bpp.ip,'10.10.10.3','10.10.10.4','10.10.10.5']
    for index in range(1,4): bpp.member(bpp.member_id+index,ips[index],f'02:00:00:00:00:{index+2:02x}')
    bpp.group([bpp.member_id+i for i in range(4)])
    poll(lambda: True if len(group_members(bpp,group))==4 else None)
    objects=[installed_member_for_peer(bpp,group,ip) for ip in ips]
    failed,nh,tunnel=objects[failed_index]
    bpp.fault(fail='member_remove',member_oid=failed,group_oid=group); since=len(bpp.trace())
    bpp.group([bpp.member_id+3])
    for index in range(3): bpp.delete_app(OBJECTS,str(bpp.member_id+index))
    poll(lambda: True if set(group_members(bpp,group))=={failed,objects[3][0]} else None)
    assert nh in bpp.asic.get_keys(NH) and tunnel in bpp.asic.get_keys(TUNNEL)
    assert bpp.asic.get_entry(BP,bpp.bridge_oid())[GROUP_ATTR]==group
    assert bpp.state()['reconciliation_state']=='STEADY'
    assert_no_protection_calls(bpp,since)
    errors=poll(lambda: rows if (rows:=member_removal_errors(bpp,group)) else None)
    assert errors[-1]['member_id']==str(bpp.member_id+failed_index)
    assert errors[-1]['action']=='retain_and_continue'
    assert int(errors[-1]['status'])==bpp.instrumented.constants['status_failure']
    for index in range(3):
        if index!=failed_index:
            bpp.asic.wait_for_deleted_entry(NH,objects[index][1])
            bpp.asic.wait_for_deleted_entry(TUNNEL,objects[index][2])
    bpp.fault(group_oid=group); bpp.asic.wait_for_deleted_entry(NHGM,failed)
    bpp.asic.wait_for_deleted_entry(NH,nh); bpp.asic.wait_for_deleted_entry(TUNNEL,tunnel)
    assert group in bpp.asic.get_keys(NHG) and len(group_members(bpp,group))==1
    bpp.path('PRIMARY')


def member_removal_errors(bpp,group):
    import re
    rows=[dict(re.findall(r'(\w+)=([^\s]*)',line)) for line in run(bpp.dvs,['cat','/var/log/syslog']).splitlines()
          if 'event=protection_member_remove_failed' in line]
    wanted=hex(int(group.split(':',1)[1],16))
    return [row for row in rows if row.get('nhg_oid')==wanted and row.get('nhg_id')==str(bpp.group_id)]


@pytest.mark.parametrize('failed_index',[0,1,2])
def test_group_delete_removes_other_members_and_retains_the_failed_member_owner(bpp,failed_index):
    bpp.start(revertive=False); group=group_oid(bpp); ips=[bpp.ip,'10.10.10.3','10.10.10.4']
    for index in range(1,3): bpp.member(bpp.member_id+index,ips[index],f'02:00:00:00:00:{index+2:02x}')
    bpp.group([bpp.member_id+i for i in range(3)])
    poll(lambda: True if len(group_members(bpp,group))==3 else None)
    objects=[installed_member_for_peer(bpp,group,ip) for ip in ips]
    failed,nh,tunnel=objects[failed_index]
    bpp.fault(fail='member_remove',member_oid=failed,group_oid=group); since=len(bpp.trace())
    bpp.delete_app(OBJECTS,str(bpp.group_id))
    for index in range(3): bpp.delete_app(OBJECTS,str(bpp.member_id+index))
    bpp.asic.wait_for_field_match(BP,bpp.bridge_oid(),{GROUP_ATTR:'oid:0x0'})
    poll(lambda: True if set(group_members(bpp,group))=={failed} else None)
    assert group in bpp.asic.get_keys(NHG) and nh in bpp.asic.get_keys(NH) and tunnel in bpp.asic.get_keys(TUNNEL)
    assert not [e for e in bpp.trace()[since:] if e['operation']=='group_remove']
    for index in range(3):
        if index!=failed_index:
            bpp.asic.wait_for_deleted_entry(NH,objects[index][1])
            bpp.asic.wait_for_deleted_entry(TUNNEL,objects[index][2])
    bpp.fault(group_oid=group)
    bpp.asic.wait_for_deleted_entry(NHGM,failed); bpp.asic.wait_for_deleted_entry(NHG,group)
    bpp.asic.wait_for_deleted_entry(NH,nh); bpp.asic.wait_for_deleted_entry(TUNNEL,tunnel)
    events=bpp.trace()[since:]
    successful=[e for e in events if e['operation']=='member_remove' and e['oid']==int(failed.split(':',1)[1],16) and e['status']==0]
    deleted=[e for e in events if e['operation']=='group_remove' and e['status']==0]
    assert successful and deleted and successful[-1]['time_ms']<=deleted[-1]['time_ms']


def test_failed_member_cleanup_does_not_block_a_new_ready_peer(bpp):
    bpp.start(revertive=False); group=group_oid(bpp)
    bpp.member(bpp.member_id+1,'10.10.10.3','02:00:00:00:00:03'); bpp.group([bpp.member_id,bpp.member_id+1])
    poll(lambda: True if len(group_members(bpp,group))==2 else None)
    failed,nh,tunnel=installed_member_for_peer(bpp,group,bpp.ip)
    bpp.fault(fail='member_remove',member_oid=failed,group_oid=group); since=len(bpp.trace())
    bpp.member(bpp.member_id+2,'10.10.10.4','02:00:00:00:00:04')
    bpp.group([bpp.member_id+1,bpp.member_id+2]); bpp.delete_app(OBJECTS,str(bpp.member_id))
    poll(lambda: True if len(group_members(bpp,group))==3 else None)
    installed_member_for_peer(bpp,group,'10.10.10.4')
    assert bpp.state()['reconciliation_state']=='STEADY' and nh in bpp.asic.get_keys(NH)
    assert_no_protection_calls(bpp,since)
    bpp.fault(group_oid=group); bpp.asic.wait_for_deleted_entry(NHGM,failed)
    assert len(group_members(bpp,group))==2 and group in bpp.asic.get_keys(NHG)


def test_one_group_member_failure_does_not_block_another_groups_shared_peer_cleanup(bpp):
    bpp.start(revertive=False); first=group_oid(bpp)
    bpp.member(bpp.member_id+1,'10.10.10.3','02:00:00:00:00:03'); bpp.group([bpp.member_id,bpp.member_id+1])
    poll(lambda: True if len(group_members(bpp,first))==2 else None)
    failed,shared,tunnel=installed_member_for_peer(bpp,first,bpp.ip)
    before=set(bpp.asic.get_keys(NHG)); bpp.group([bpp.member_id,bpp.member_id+1],identifier=bpp.group_id+1)
    second=poll(lambda: next(iter(values)) if len(values:=set(bpp.asic.get_keys(NHG))-before)==1 else None)
    poll(lambda: True if len(group_members(bpp,second))==2 else None)
    bpp.fault(fail='member_remove',member_oid=failed,group_oid=first)
    bpp.group([bpp.member_id+1]); bpp.group([bpp.member_id+1],identifier=bpp.group_id+1)
    bpp.delete_app(OBJECTS,str(bpp.member_id))
    poll(lambda: True if len(group_members(bpp,first))==2 and len(group_members(bpp,second))==1 else None)
    assert shared in bpp.asic.get_keys(NH) and tunnel in bpp.asic.get_keys(TUNNEL)
    bpp.fault(group_oid=first); bpp.asic.wait_for_deleted_entry(NHGM,failed)
    bpp.asic.wait_for_deleted_entry(NH,shared); bpp.asic.wait_for_deleted_entry(TUNNEL,tunnel)
    assert first in bpp.asic.get_keys(NHG) and second in bpp.asic.get_keys(NHG)


def rmac_snapshot(bpp):
    nh,tunnel=peer(bpp); oid=group_oid(bpp); bridge=bpp.bridge_oid()
    return nh,tunnel,oid,bridge,group_members(bpp,oid)


def assert_rmac_objects_stable(bpp,snapshot):
    nh,tunnel,oid,bridge,members=snapshot
    assert peer(bpp)==(nh,tunnel)
    assert group_members(bpp,oid)==members
    assert bpp.bridge_oid()==bridge
    assert bpp.asic.get_entry(BP,bridge)[GROUP_ATTR]==oid


def assert_no_bridge_churn(bpp,since):
    assert not [e for e in bpp.trace()[since:] if e['operation'] in
                ('set','get','bridge_remove','member_remove','group_remove','default_mac')]


@pytest.mark.parametrize('imet',[False,True])
def test_rmac_only_update_preserves_all_objects_and_bridge_attachment(bpp,imet):
    if imet:
        bpp.app('VXLAN_REMOTE_VNI_TABLE',bpp.vlan+':'+bpp.ip,{'vni':'99999'})
    bpp.start(); snapshot=rmac_snapshot(bpp); tunnel=snapshot[1]
    bpp.fault(tunnel_oid=tunnel); since=len(bpp.trace()); updated='02:00:00:00:00:22'
    bpp.member(mac=updated)
    bpp.asic.wait_for_field_match(TUNNEL,tunnel,{'SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC':updated})
    stable(lambda: assert_rmac_objects_stable(bpp,snapshot)); assert_no_bridge_churn(bpp,since)
    writes=[e for e in bpp.trace()[since:] if e['operation']=='rmac_set']
    assert len(writes)==1 and writes[0]['status']==0 and writes[0]['mac']==updated


@pytest.mark.parametrize('up',[True,False])
def test_failed_rmac_update_retains_path_and_retries_latest_value_without_bridge_churn(bpp,up):
    bpp.start(); bpp.link(up); bpp.path('PRIMARY' if up else 'PROTECTION')
    snapshot=rmac_snapshot(bpp); tunnel=snapshot[1]; bpp.fault(fail='rmac',tunnel_oid=tunnel)
    since=len(bpp.trace()); bpp.member(mac='02:00:00:00:00:22')
    poll(lambda: True if any(e['operation']=='rmac_set' and e['status']!=0 for e in bpp.trace()[since:]) else None)
    stable(lambda: assert_rmac_objects_stable(bpp,snapshot))
    assert bpp.asic.get_entry(TUNNEL,tunnel)['SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC']==bpp.mac
    assert_no_bridge_churn(bpp,since)
    rc,logs=bpp.dvs.runcmd(['sh','-c','cat /var/log/syslog'])
    assert rc==0
    matching=[line for line in logs.splitlines() if 'event=protection_rmac_update_failed' in line and
              'tunnel_oid='+tunnel.split(':',1)[1] in line]
    assert any('requested_rmac=02:00:00:00:00:22' in line and 'owners=1' in line and
               'status=-1' in line and 'action=retain_and_retry' in line for line in matching)
    bpp.member(mac='02:00:00:00:00:33')
    poll(lambda: True if any(e['operation']=='rmac_set' and e['status']!=0 and e['mac']=='02:00:00:00:00:33'
                            for e in bpp.trace()[since:]) else None)
    bpp.fault(tunnel_oid=tunnel)
    bpp.asic.wait_for_field_match(TUNNEL,tunnel,{'SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC':'02:00:00:00:00:33'})
    stable(lambda: assert_rmac_objects_stable(bpp,snapshot)); assert_no_bridge_churn(bpp,since)
    successful=[e for e in bpp.trace()[since:] if e['operation']=='rmac_set' and e['status']==0]
    assert successful and all(e['mac']=='02:00:00:00:00:33' for e in successful)


def test_shared_rmac_conflict_and_later_agreement_keep_the_existing_physical_peer(bpp):
    bpp.start(); bpp.member(bpp.member_id+1); bpp.group([bpp.member_id,bpp.member_id+1])
    snapshot=rmac_snapshot(bpp); stable(lambda: assert_rmac_objects_stable(bpp,snapshot))
    tunnel=snapshot[1]; bpp.fault(tunnel_oid=tunnel); since=len(bpp.trace()); updated='02:00:00:00:00:22'
    bpp.member(mac=updated)
    stable(lambda: assert_rmac_objects_stable(bpp,snapshot))
    assert bpp.asic.get_entry(TUNNEL,tunnel)['SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC']==bpp.mac
    assert not [e for e in bpp.trace()[since:] if e['operation']=='rmac_set']
    bpp.member(bpp.member_id+1,mac=updated)
    bpp.asic.wait_for_field_match(TUNNEL,tunnel,{'SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC':updated})
    stable(lambda: assert_rmac_objects_stable(bpp,snapshot)); assert_no_bridge_churn(bpp,since)
    successful=[e for e in bpp.trace()[since:] if e['operation']=='rmac_set' and e['status']==0]
    assert len(successful)==1


def test_pending_rmac_update_withdrawal_still_waits_for_confirmed_bridge_clear(bpp):
    bpp.start(); snapshot=rmac_snapshot(bpp); tunnel=snapshot[1]
    bpp.fault(fail='rmac',tunnel_oid=tunnel); bpp.member(mac='02:00:00:00:00:22')
    poll(lambda: True if any(e['operation']=='rmac_set' and e['status']!=0 for e in bpp.trace()) else None)
    # Keep the RMAC failure active while arming the independent clear failure;
    # otherwise an asynchronous retry can succeed before the DEL is consumed.
    bpp.fault(fail='rmac,clear',tunnel_oid=tunnel); since=len(bpp.trace())
    # Stop the pending update before requesting group retirement.
    bpp.delete_app(OBJECTS,str(bpp.member_id)); bpp.wait_runtime(last_event='sai_retry')
    # Before DEL consumption the previous RMAC intent is still valid; failed
    # attempts in that delivery window are allowed, successful updates are not.
    withdrawn=len(bpp.trace())
    assert not [e for e in bpp.trace()[since:] if e['operation']=='rmac_set' and
                e['mac']=='02:00:00:00:00:22' and e['status']==0]
    bpp.delete_app(OBJECTS,str(bpp.group_id))
    stable(lambda: assert_rmac_objects_stable(bpp,snapshot))
    assert not [e for e in bpp.trace()[withdrawn:] if e['operation']=='rmac_set' and e['mac']=='02:00:00:00:00:22']
    bpp.fault(group_oid=snapshot[2],tunnel_oid=tunnel)
    bpp.asic.wait_for_deleted_entry(NHG,snapshot[2]); bpp.asic.wait_for_deleted_entry(TUNNEL,tunnel)
    not_applicable_before_remove(bpp,'member_remove',since)
    assert not [e for e in bpp.trace()[withdrawn:] if e['operation']=='rmac_set' and e['mac']=='02:00:00:00:00:22']


def test_duplicate_rmac_update_and_cancel_to_installed_value_issue_no_extra_set(bpp):
    bpp.start(); snapshot=rmac_snapshot(bpp); tunnel=snapshot[1]
    bpp.fault(tunnel_oid=tunnel); since=len(bpp.trace()); bpp.member(); bpp.member()
    stable(lambda: assert_rmac_objects_stable(bpp,snapshot))
    assert not [e for e in bpp.trace()[since:] if e['operation']=='rmac_set']
    bpp.fault(fail='rmac',tunnel_oid=tunnel); bpp.member(mac='02:00:00:00:00:22')
    poll(lambda: True if any(e['operation']=='rmac_set' and e['status']!=0 for e in bpp.trace()[since:]) else None)
    bpp.member()
    stable(lambda: assert_rmac_objects_stable(bpp,snapshot),duration=1)
    bpp.fault(tunnel_oid=tunnel); since=len(bpp.trace())
    stable(lambda: assert_rmac_objects_stable(bpp,snapshot))
    assert bpp.asic.get_entry(TUNNEL,tunnel)['SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC']==bpp.mac
    assert not [e for e in bpp.trace()[since:] if e['operation']=='rmac_set' and e['status']==0]


def test_new_owner_waits_for_pending_rmac_then_shares_the_same_objects(bpp):
    bpp.start(); snapshot=rmac_snapshot(bpp); tunnel=snapshot[1]
    updated='02:00:00:00:00:22'; bpp.fault(fail='rmac',tunnel_oid=tunnel); bpp.member(mac=updated)
    poll(lambda: True if any(e['operation']=='rmac_set' and e['status']!=0 for e in bpp.trace()) else None)
    bpp.member(bpp.member_id+1,mac=updated); bpp.group([bpp.member_id,bpp.member_id+1])
    stable(lambda: assert_rmac_objects_stable(bpp,snapshot))
    assert bpp.asic.get_entry(TUNNEL,tunnel)['SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC']==bpp.mac
    bpp.fault(tunnel_oid=tunnel)
    bpp.asic.wait_for_field_match(TUNNEL,tunnel,{'SAI_TUNNEL_ATTR_VXLAN_TUNNEL_MAC':updated})
    stable(lambda: assert_rmac_objects_stable(bpp,snapshot))
    bpp.group([bpp.member_id+1]); bpp.delete_app(OBJECTS,str(bpp.member_id))
    stable(lambda: assert_rmac_objects_stable(bpp,snapshot))


# Observe real primary objects. All writes below are normal CONFIG_DB
# or APPL_DB inputs; no ASIC_DB/STATE_DB seeding or traffic claim.
def primary_forwarding_entries(bpp):
    import json
    bpp.interface_ip('192.0.2.1/24')
    mac='00:11:22:33:44:55'; ip='192.0.2.2'; prefix='198.18.0.0/24'
    bpp.app('FDB_TABLE',bpp.vlan+':'+mac,{'port':bpp.po,'type':'dynamic'})
    bpp.app('NEIGH_TABLE',bpp.vlan+':'+ip,{'neigh':mac,'family':'IPv4'})
    bpp.app('ROUTE_TABLE',prefix,{'nexthop':ip,'ifname':bpp.vlan})
    result={}
    for table,field,value in [('ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY','mac',mac),
                              ('ASIC_STATE:SAI_OBJECT_TYPE_NEIGHBOR_ENTRY','ip',ip),
                              ('ASIC_STATE:SAI_OBJECT_TYPE_ROUTE_ENTRY','dest',prefix)]:
        def find():
            matches=[key for key in bpp.asic.get_keys(table) if str(json.loads(key).get(field,'')).lower()==value.lower()]
            return matches[0] if len(matches)==1 else None
        key=poll(find); result[table]=(key,bpp.asic.get_entry(table,key))
    return result


def assert_primary_forwarding_preserved(bpp,entries):
    for table,(key,attrs) in entries.items():
        assert key in bpp.asic.get_keys(table)
        assert bpp.asic.get_entry(table,key)==attrs


def successful_port_fdb_flushes(bpp,since):
    return [e for e in bpp.trace()[since:] if e['operation']=='fdb_flush' and e['status']==0]


@pytest.mark.parametrize('admin',['AUTO','PRIMARY','PROTECTION'])
def test_policy_before_binding_preserves_primary_fdb_neighbor_route_on_down(bpp,admin):
    entries=primary_forwarding_entries(bpp); bpp.policy(admin,False,0); bpp.fault(); since=len(bpp.trace())
    bpp.link(False); stable(lambda: assert_primary_forwarding_preserved(bpp,entries))
    assert not successful_port_fdb_flushes(bpp,since)


def test_binding_before_policy_preserves_primary_entries_while_group_is_missing(bpp):
    entries=primary_forwarding_entries(bpp); bpp.bind(); bpp.fault(); since=len(bpp.trace())
    bpp.link(False); stable(lambda: assert_primary_forwarding_preserved(bpp,entries))
    assert not successful_port_fdb_flushes(bpp,since)


@pytest.mark.parametrize('admin',['AUTO','PRIMARY','PROTECTION'])
def test_attached_protection_preserves_primary_entries_through_down_and_recovery(bpp,admin):
    entries=primary_forwarding_entries(bpp); bpp.start(admin,False,0); since=len(bpp.trace())
    bpp.link(False); bpp.path('PRIMARY' if admin=='PRIMARY' else 'PROTECTION')
    stable(lambda: assert_primary_forwarding_preserved(bpp,entries)); bpp.link(True)
    stable(lambda: assert_primary_forwarding_preserved(bpp,entries))
    assert not successful_port_fdb_flushes(bpp,since)


@pytest.mark.parametrize('config_first',[True,False])
@pytest.mark.parametrize('down',[True,False])
def test_complete_intent_withdrawal_flushes_once_only_if_port_remains_down(bpp,config_first,down):
    entries=primary_forwarding_entries(bpp); bpp.start(); bpp.link(not down)
    bpp.path('PROTECTION' if down else 'PRIMARY'); since=len(bpp.trace())
    def policy_del(): bpp.config.delete_entry(CONFIG,bpp.po)
    def binding_del(): bpp.delete_app(BINDING,bpp.po)
    first,second=(policy_del,binding_del) if config_first else (binding_del,policy_del)
    first(); stable(lambda: assert_primary_forwarding_preserved(bpp,entries))
    assert not successful_port_fdb_flushes(bpp,since)
    second()
    fdb_table='ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY'; fdb_key=entries[fdb_table][0]
    if down:
        bpp.asic.wait_for_deleted_entry(fdb_table,fdb_key)
        poll(lambda: True if len(successful_port_fdb_flushes(bpp,since))==1 else None)
    else:
        stable(lambda: assert_primary_forwarding_preserved(bpp,entries))
        assert not successful_port_fdb_flushes(bpp,since)
    policy_del(); binding_del(); time.sleep(0.5)
    assert len(successful_port_fdb_flushes(bpp,since))==(1 if down else 0)


def test_fpm_row_flush_and_ordered_replay_with_policy_preserve_primary_entries(bpp):
    entries=primary_forwarding_entries(bpp); bpp.start(); bpp.link(False); bpp.path('PROTECTION'); old_group=group_oid(bpp); since=len(bpp.trace())
    # Real queued consumer retirement. Full TCP/coalesced replay remains the
    # separate recovery boundary; this case verifies the protection intent gate.
    _,old_tunnel=peer(bpp)
    bpp.delete_app(BINDING,bpp.po); bpp.wait_runtime(last_event='binding_pending')
    bpp.delete_app(OBJECTS,str(bpp.group_id)); bpp.delete_app(OBJECTS,str(bpp.member_id))
    bpp.asic.wait_for_deleted_entry(NHG,old_group)
    bpp.asic.wait_for_deleted_entry(TUNNEL,old_tunnel)
    stable(lambda: assert_primary_forwarding_preserved(bpp,entries))
    bpp.member(); bpp.group(); bpp.bind(); bpp.path('PROTECTION')
    stable(lambda: assert_primary_forwarding_preserved(bpp,entries)); assert not successful_port_fdb_flushes(bpp,since)


def test_failed_clear_preserves_fdb_until_both_intents_are_removed(bpp):
    entries=primary_forwarding_entries(bpp); bpp.start(); bpp.link(False); bpp.path('PROTECTION'); since=len(bpp.trace())
    bpp.fault(fail='clear'); bpp.config.delete_entry(CONFIG,bpp.po)
    stable(lambda: assert_primary_forwarding_preserved(bpp,entries)); assert not successful_port_fdb_flushes(bpp,since)
    bpp.delete_app(BINDING,bpp.po)
    bpp.asic.wait_for_deleted_entry('ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY',entries['ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY'][0])
    poll(lambda: True if len(successful_port_fdb_flushes(bpp,since))==1 else None)
    bpp.fault(); time.sleep(0.5); assert len(successful_port_fdb_flushes(bpp,since))==1


def test_unprotected_port_down_keeps_the_existing_normal_fdb_flush(bpp):
    entries=primary_forwarding_entries(bpp); bpp.fault(); since=len(bpp.trace()); bpp.link(False)
    bpp.asic.wait_for_deleted_entry('ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY',entries['ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY'][0])
    poll(lambda: True if len(successful_port_fdb_flushes(bpp,since))==1 else None)


def test_pending_group_and_terminal_attachment_error_still_preserve_primary_entries(bpp):
    entries=primary_forwarding_entries(bpp); bpp.policy('AUTO',False,0); bpp.bind(); bpp.fault(); since=len(bpp.trace())
    bpp.link(False); stable(lambda: assert_primary_forwarding_preserved(bpp,entries))
    bpp.fault(fail='group',status='unsupported'); bpp.member(); bpp.group()
    bpp.wait_runtime(last_event='sai_unsupported'); stable(lambda: assert_primary_forwarding_preserved(bpp,entries))
    assert not successful_port_fdb_flushes(bpp,since)


def test_explicit_vlan_removal_retains_normal_fdb_teardown_even_when_protected(bpp):
    entries=primary_forwarding_entries(bpp); bpp.start(); since=len(bpp.trace()); bpp.bridge(False)
    bpp.asic.wait_for_deleted_entry('ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY',entries['ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY'][0])
    assert successful_port_fdb_flushes(bpp,since)


@pytest.mark.parametrize('fault',['admin','get'])
def test_wtr_restore_failure_preserves_primary_fdb_neighbor_and_route(bpp,fault):
    entries=primary_forwarding_entries(bpp); bpp.start(revertive=True,wtr=5000)
    since=len(bpp.trace()); bpp.link(False); bpp.path('PROTECTION')
    bpp.mark_logs(); bpp.link(True); bpp.wait_log(wtr_state='WAIT_TO_RESTORE')
    bpp.fault(fail=fault); bpp.mark_logs()
    bpp.wait_runtime(reconciliation_state='INCONSISTENT',
                     desired_sai_admin_mode='PRIMARY',wtr_state='IDLE')
    stable(lambda: assert_primary_forwarding_preserved(bpp,entries))
    assert not successful_port_fdb_flushes(bpp,since)
    bpp.fault(); bpp.path('PRIMARY')
    stable(lambda: assert_primary_forwarding_preserved(bpp,entries))
    assert not successful_port_fdb_flushes(bpp,since)


@pytest.mark.parametrize('config_first',[True,False])
def test_intent_withdrawal_during_wtr_cancels_restore_and_restores_normal_flush(bpp,config_first):
    entries=primary_forwarding_entries(bpp); bpp.start(revertive=True,wtr=5000)
    bpp.link(False); bpp.path('PROTECTION')
    bpp.mark_logs(); bpp.link(True); bpp.wait_log(wtr_state='WAIT_TO_RESTORE')
    def policy_del(): bpp.config.delete_entry(CONFIG,bpp.po)
    def binding_del(): bpp.delete_app(BINDING,bpp.po)
    first,second=(policy_del,binding_del) if config_first else (binding_del,policy_del)
    first(); stable(lambda: assert_primary_forwarding_preserved(bpp,entries))
    second(); poll(lambda: True if not bpp.state() else None)
    since=len(bpp.trace())
    stable(lambda: assert_primary_forwarding_preserved(bpp,entries),duration=6)
    assert not bpp.admin_writes(since)
    assert not successful_port_fdb_flushes(bpp,since)
    bpp.link(False)
    bpp.asic.wait_for_deleted_entry('ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY',
                                  entries['ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY'][0])
    poll(lambda: True if len(successful_port_fdb_flushes(bpp,since))==1 else None)
